#!/usr/bin/env python3
"""
Tier-1 check of the golden-screenshot gate's COMPARATOR and its attestation
rules. No target, no build, no gwemu -- runs in under a second.

This is the negative-control half of `scripts/golden_screens.py`. The gate's
whole value is that it FAILS on a bad picture, and a comparator that quietly
returns "identical" for everything would pass every run for ever and look
exactly like a healthy suite. So every check here is paired: a positive case
that must pass, and a perturbation of the SAME image that must be rejected --
and each perturbation is a real failure this project has shipped:

  * a plane written at the wrong offset          -> the whole image shifts
  * an off-screen VRAM cache too small           -> one rectangle turns to noise
  * a palette read from the wrong table          -> every pixel moves a little
  * a mode rendering at half height              -> the bottom goes black

It also checks the attestation rules, because "the golden was never looked at"
is the failure mode that makes every other check worthless.
"""

import json
import os
import random
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from scripts.common.golden import (  # noqa: E402
    Golden, GoldenError, compare, format_report, read_png, sha256)
from scripts.common.screen import write_png  # noqa: E402

W, H = 320, 240
FAILURES = []


def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}: {name}" + (f"  {detail}" if detail else ""))
    if not cond:
        FAILURES.append(name)


def synth():
    """A picture with structure in every region, so a shift cannot alias into
    agreement the way a flat field or a repeating pattern would."""
    rgb = bytearray(W * H * 3)
    rnd = random.Random(1234)
    for y in range(H):
        for x in range(W):
            o = (y * W + x) * 3
            rgb[o] = (x * 7 + y * 3) & 0xFF
            rgb[o + 1] = (x ^ y) & 0xFF
            rgb[o + 2] = rnd.randrange(256)
    return bytes(rgb)


def shifted(rgb, dx, dy):
    out = bytearray(len(rgb))
    for y in range(H):
        sy = y - dy
        for x in range(W):
            sx = x - dx
            if 0 <= sx < W and 0 <= sy < H:
                o, s = (y * W + x) * 3, (sy * W + sx) * 3
                out[o:o + 3] = rgb[s:s + 3]
    return bytes(out)


def noised_rect(rgb, x0, y0, x1, y1):
    out = bytearray(rgb)
    rnd = random.Random(99)
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            o = (y * W + x) * 3
            out[o] = rnd.randrange(256)
            out[o + 1] = rnd.randrange(256)
            out[o + 2] = rnd.randrange(256)
    return bytes(out)


def levelled(rgb, delta):
    return bytes(min(255, v + delta) for v in rgb)


def half_height(rgb):
    out = bytearray(rgb)
    for y in range(H // 2, H):
        out[y * W * 3:(y + 1) * W * 3] = b"\x00" * (W * 3)
    return bytes(out)


def test_compare():
    print("comparator:")
    a = synth()

    r = compare(a, a, W, H)
    check("identical images compare equal", r["n_diff"] == 0 and r["bbox"] is None)

    # NEGATIVE CONTROL 1 -- a one-pixel shift. The failure a wrong plane
    # offset or a wrong CRTC start address produces.
    r = compare(a, shifted(a, 1, 0), W, H)
    check("negative control: a 1-pixel horizontal shift is rejected",
          r["n_diff"] > W * H // 2, f"{r['n_diff']} pixels")

    # NEGATIVE CONTROL 2 -- one rectangle of noise, everything else perfect.
    # This is Wolf3D's episode-select screen: correct text, six thumbnails of
    # garbage. A gate that only looks at a global count could miss it; the
    # bounding box has to NAME it.
    bad = noised_rect(a, 40, 60, 120, 100)
    r = compare(a, bad, W, H)
    check("negative control: a noise rectangle is rejected", r["n_diff"] > 0,
          f"{r['n_diff']} pixels")
    check("...and the bounding box locates it", r["bbox"] == (40, 60, 120, 100),
          str(r["bbox"]))
    rows = [i for i, row in enumerate(r["grid"]) if any(row)]
    check("...and the block map marks only those rows",
          rows == [3, 4, 5], str(rows))

    # NEGATIVE CONTROL 3 -- a small uniform level shift, i.e. a palette read
    # from the wrong table. Every pixel moves a little; at tol=0 that must be
    # rejected, and the tolerance must be what lets it through, not luck.
    r = compare(a, levelled(a, 3), W, H, tol=0)
    check("negative control: a +3 level shift is rejected at tol=0",
          r["n_diff"] > 0, f"{r['n_diff']} pixels")
    r = compare(a, levelled(a, 3), W, H, tol=3)
    check("...and accepted at tol=3", r["n_diff"] == 0)
    r = compare(a, levelled(a, 4), W, H, tol=3)
    check("...while +4 at tol=3 is still rejected", r["n_diff"] > 0)

    # NEGATIVE CONTROL 4 -- half the screen black, which is Keen 4's intro.
    r = compare(a, half_height(a), W, H)
    check("negative control: a half-height render is rejected", r["n_diff"] > 0)
    check("...and the bounding box starts at the halfway line",
          r["bbox"][1] == H // 2, str(r["bbox"]))

    # The mask must exclude ONLY what it covers -- and must not swallow a
    # defect one pixel outside it. A mask that leaked would silently widen the
    # hole in the gate every time a band was declared.
    mask = [[40, 60, 120, 100]]
    r = compare(a, bad, W, H, mask=mask)
    check("a mask excludes its own region from the gate", r["n_diff"] == 0)
    check("...but still COUNTS and reports what is inside it",
          r["n_masked_diff"] > 0, f"{r['n_masked_diff']} pixels")
    check("...and the report says so",
          any("INSIDE the declared mask" in ln for ln in format_report(r)))
    r = compare(a, noised_rect(a, 121, 60, 130, 100), W, H, mask=mask)
    check("negative control: a defect just OUTSIDE the mask is still caught",
          r["n_diff"] > 0, f"{r['n_diff']} pixels")

    # `worst` must name the largest deviation, not the first one found.
    two = bytearray(a)
    two[(10 * W + 10) * 3] = (a[(10 * W + 10) * 3] + 5) & 0xFF
    o = (200 * W + 200) * 3
    two[o] = 255 if a[o] < 128 else 0
    r = compare(a, bytes(two), W, H)
    check("worst pixel is the LARGEST deviation, not the first",
          (r["worst"]["x"], r["worst"]["y"]) == (200, 200),
          f"({r['worst']['x']},{r['worst']['y']}) delta={r['worst']['delta']}")


def test_png_roundtrip(tmp):
    print("png:")
    a = synth()
    p = os.path.join(tmp, "a.png")
    write_png(p, W, H, a)
    w, h, back = read_png(p)
    check("write_png/read_png round-trips exactly",
          (w, h) == (W, H) and back == a)
    # A decoder that silently accepted some other PNG shape would let a golden
    # written by another tool into the gate, quantised differently.
    p2 = os.path.join(tmp, "grey.png")
    import struct
    import zlib
    raw = b"".join(b"\x00" + bytes(range(8)) for _ in range(8))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    open(p2, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 0, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    try:
        read_png(p2)
        check("negative control: a greyscale PNG is refused", False)
    except ValueError:
        check("negative control: a greyscale PNG is refused", True)


def test_attestation(tmp):
    print("attestation:")
    png = os.path.join(tmp, "T.png")
    write_png(png, W, H, synth())
    meta_path = os.path.join(tmp, "T.json")

    def put(meta):
        json.dump(meta, open(meta_path, "w"))

    good = {"state": "verified", "verified_by": "doug",
            "verified_at": "2026-08-14T12:00:00+02:00",
            "shows": "a test pattern", "sha256": sha256(png)}
    put(good)
    Golden.load(png).verified()
    check("a complete attestation loads and verifies", True)

    # THE TRAP THIS EXISTS FOR: an image with no record of anyone looking at it.
    os.remove(meta_path)
    try:
        Golden.load(png)
        check("negative control: an image with no sidecar is refused", False)
    except GoldenError:
        check("negative control: an image with no sidecar is refused", True)

    for missing in ("verified_by", "verified_at", "shows"):
        m = dict(good)
        del m[missing]
        put(m)
        try:
            Golden.load(png).verified()
            check(f"negative control: missing {missing} is refused", False)
        except GoldenError:
            check(f"negative control: missing {missing} is refused", True)

    for state in ("unverified", "known_bad"):
        m = dict(good)
        m["state"] = state
        put(m)
        try:
            Golden.load(png).verified()
            check(f"negative control: state={state} is refused", False)
        except GoldenError as exc:
            check(f"negative control: state={state} is refused", True)
            if state == "known_bad":
                check("...and says KNOWN BAD, so the summary can tell a "
                      "recorded defect from a regression",
                      "KNOWN BAD" in str(exc))

    # An image swapped underneath its attestation is not a new baseline.
    put(good)
    write_png(png, W, H, shifted(synth(), 3, 0))
    try:
        Golden.load(png)
        check("negative control: an image swapped under its sidecar is "
              "refused", False)
    except GoldenError:
        check("negative control: an image swapped under its sidecar is "
              "refused", True)


def test_shipped_goldens():
    """The goldens actually in the tree must load, and their sha256 must match.

    This is the cheap half of the gate and it needs no target at all: a golden
    edited or replaced without re-blessing fails here, in a second, rather than
    at the end of a 30 s emulator run.
    """
    print("shipped goldens:")
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    man = json.load(open(os.path.join(root, "tests/goldens/manifest.json")))
    for name, entry in sorted(man["titles"].items()):
        png = os.path.join(root, "tests/goldens", name + ".png")
        try:
            g = Golden.load(png)
        except GoldenError as exc:
            check(f"{name} loads", False, str(exc))
            continue
        state = g.meta.get("state")
        check(f"{name} sha256 matches its attestation", True, f"state={state}")
        check(f"{name} records what it shows", bool(g.meta.get("shows")))
        if entry.get("mask"):
            check(f"{name} mask has a written reason",
                  bool(entry.get("mask_reason")))


def main():
    tmp = tempfile.mkdtemp(prefix="golden-", dir="build/run"
                           if os.path.isdir("build/run") else None)
    try:
        test_compare()
        test_png_roundtrip(tmp)
        test_attestation(tmp)
        test_shipped_goldens()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if FAILURES:
        print(f"\nFAIL: {len(FAILURES)}: " + ", ".join(FAILURES))
        return 1
    print("\nPASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
