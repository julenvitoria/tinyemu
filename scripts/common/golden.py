"""Programmatic image comparison for the golden-screenshot gate.

Why this module exists separately from the capture script: the comparison is
the part that has to be RIGHT, and it is the only part that can be tested
without a target. `tests/test_goldens.py` drives everything here against
synthetic images, including negative controls, so a broken comparator is
caught at tier 1 rather than by a gate that silently passes everything.

Two rules the design turns on:

1. **A count is not a diagnosis.** "3,412 pixels differ" cannot distinguish a
   palette that shifted by one level everywhere (harmless-looking, huge count)
   from six thumbnails of noise in an otherwise perfect screen (small count,
   fatal). So `compare()` returns WHERE: a bounding box, a block map, and the
   worst pixel. A gate that only prints pass/fail is the gate we already had.
2. **A golden is a claim about what the screen SHOWS**, and that claim can only
   be made by a human looking at it once. `Golden` therefore carries the
   attestation and `verified()` refuses anything that lacks it -- see
   `scripts/golden_screens.py`.
"""

import hashlib
import json
import os
import struct
import zlib

# Only the shape our own writer emits (8-bit truecolour, no interlace) is
# accepted. A decoder that silently coped with 16-bit or palette PNGs would let
# a golden written by some other tool into the gate, and the first sign of it
# would be a comparison against differently-quantised pixels.
def read_png(path):
    """Return (w, h, rgb_bytes) for an 8-bit truecolour non-interlaced PNG."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, hdr = 8, [], None
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if tag == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat.append(body)
        elif tag == b"IEND":
            break
    if hdr is None:
        raise ValueError(f"{path}: no IHDR")
    w, h, depth, ctype, comp, filt, interlace = hdr
    if (depth, ctype, interlace) != (8, 2, 0):
        raise ValueError(f"{path}: depth={depth} colour={ctype} "
                         f"interlace={interlace}; only 8-bit truecolour "
                         f"non-interlaced is accepted")
    raw = zlib.decompress(b"".join(idat))
    stride = w * 3
    out = bytearray(w * h * 3)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ft == 1:
            for i in range(3, stride):
                line[i] = (line[i] + line[i - 3]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - 3] if i >= 3 else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - 3] if i >= 3 else 0
                b = prev[i]
                c = prev[i - 3] if i >= 3 else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif ft != 0:
            raise ValueError(f"{path}: filter type {ft}")
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bytes(out)


def sha256(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def in_mask(mask, x, y):
    for (x0, y0, x1, y1) in mask:
        if x0 <= x <= x1 and y0 <= y <= y1:
            return True
    return False


def compare(a, b, w, h, tol=0, blocks=(16, 12), mask=()):
    """Compare two RGB buffers of the same geometry.

    `tol` is a per-CHANNEL absolute tolerance, not a distance: a pixel counts as
    different when ANY channel moves by more than `tol`. That is deliberate --
    the failures this gate exists to catch (a plane written to the wrong offset,
    an unpainted cache region, a palette read from the wrong table) move one
    channel a long way, and a Euclidean distance dilutes exactly those.

    Returns a dict. `n_diff` is the count, `bbox` the enclosing rectangle of
    every differing pixel, `grid` a per-block count over a `blocks` cols x rows
    division, and `worst` the single largest deviation with its coordinates.

    `mask` is a list of [x0,y0,x1,y1] rectangles whose pixels do NOT gate.
    They are still compared and still reported -- as `n_masked_diff` and as
    'm' in the map -- because a mask that hides its contents is a place where
    corruption can live forever. A mask exists only for elements driven by
    guest TIME rather than by the instruction stream (a sprite animated off
    the PIT), which no instruction-count capture point can freeze; the
    manifest requires a written reason for every one.
    """
    cols, rows = blocks
    grid = [[0] * cols for _ in range(rows)]
    masked_grid = [[0] * cols for _ in range(rows)]
    n = n_masked = 0
    x0, y0, x1, y1 = w, h, -1, -1
    worst = (0, -1, -1, None, None)
    for y in range(h):
        base = y * w * 3
        gr = grid[min(rows - 1, y * rows // h)]
        for x in range(w):
            o = base + x * 3
            dr = abs(a[o] - b[o])
            dg = abs(a[o + 1] - b[o + 1])
            db = abs(a[o + 2] - b[o + 2])
            d = dr if dr > dg else dg
            if db > d:
                d = db
            if d > tol:
                if mask and in_mask(mask, x, y):
                    n_masked += 1
                    masked_grid[min(rows - 1, y * rows // h)][
                        min(cols - 1, x * cols // w)] += 1
                    continue
                n += 1
                gr[min(cols - 1, x * cols // w)] += 1
                if x < x0:
                    x0 = x
                if x > x1:
                    x1 = x
                if y < y0:
                    y0 = y
                if y > y1:
                    y1 = y
                if d > worst[0]:
                    worst = (d, x, y,
                             (a[o], a[o + 1], a[o + 2]),
                             (b[o], b[o + 1], b[o + 2]))
    total = w * h
    return {
        "w": w, "h": h, "tol": tol,
        "n_diff": n,
        "n_masked_diff": n_masked,
        "masked_grid": masked_grid,
        "total": total,
        "pct": 100.0 * n / total if total else 0.0,
        "bbox": None if x1 < 0 else (x0, y0, x1, y1),
        "grid": grid,
        "blocks": blocks,
        "worst": None if worst[1] < 0 else {
            "delta": worst[0], "x": worst[1], "y": worst[2],
            "golden": worst[3], "captured": worst[4],
        },
    }


def format_report(res, indent="  "):
    """Human-readable WHERE, not just how many."""
    out = [f"{indent}{res['n_diff']} of {res['total']} pixels differ by more "
           f"than {res['tol']} ({res['pct']:.3f}%)"]
    masked = res.get("n_masked_diff", 0)
    if masked:
        # Printed even though it does not gate. A mask whose contents are never
        # shown is a hole in the gate; a mask whose contents are printed every
        # run is a place a human can still notice going wrong.
        out.append(f"{indent}(plus {masked} differing pixels INSIDE the "
                   f"declared mask, which do not gate)")
    if res["n_diff"] == 0:
        return out
    x0, y0, x1, y1 = res["bbox"]
    out.append(f"{indent}bounding box x={x0}..{x1} y={y0}..{y1} "
               f"({x1 - x0 + 1}x{y1 - y0 + 1})")
    wp = res["worst"]
    out.append(f"{indent}worst pixel ({wp['x']},{wp['y']}) delta={wp['delta']} "
               f"golden=rgb{wp['golden']} captured=rgb{wp['captured']}")
    cols, rows = res["blocks"]
    per = (res["w"] * res["h"]) // (cols * rows) or 1
    out.append(f"{indent}difference map ({cols}x{rows} blocks, "
               f"'.'=clean, digit = tenths of the block differing):")
    mg = res.get("masked_grid")
    for r in range(rows):
        line = "".join(
            ("m" if mg and mg[r][c] else ".") if v == 0
            else str(min(9, (v * 10) // per))
            for c, v in enumerate(res["grid"][r]))
        out.append(f"{indent}  |{line}|")
    return out


class GoldenError(Exception):
    pass


class Golden:
    """A stored golden and, inseparably, the attestation that it was LOOKED AT.

    The trap this class exists to close: a golden captured from a broken build
    blesses the bug, and every later run then agrees with it. So the image file
    alone is never enough -- `verified()` raises unless the sidecar carries who
    verified it, when, and what they saw, and `state` is `verified`. A golden
    recorded as `known_bad` is a deliberate, visible record of a defect and
    FAILS the gate rather than passing it.
    """

    STATES = ("verified", "unverified", "known_bad")

    def __init__(self, path, meta):
        self.path = path                 # the .png
        self.meta_path = path[:-4] + ".json"
        self.meta = meta

    @classmethod
    def load(cls, png_path):
        meta_path = png_path[:-4] + ".json"
        if not os.path.exists(png_path):
            raise GoldenError(f"no golden image at {png_path}")
        if not os.path.exists(meta_path):
            raise GoldenError(
                f"{png_path} has no attestation sidecar ({meta_path}). An "
                f"image with no record of who verified it, and of what it "
                f"shows, cannot be a golden -- that is exactly how a broken "
                f"build gets blessed.")
        meta = json.load(open(meta_path))
        g = cls(png_path, meta)
        digest = sha256(png_path)
        if meta.get("sha256") and meta["sha256"] != digest:
            raise GoldenError(
                f"{png_path} does not match the sha256 in its attestation "
                f"({digest[:16]} vs {meta['sha256'][:16]}). The image was "
                f"replaced without re-blessing it.")
        return g

    def verified(self):
        """Raise unless this golden carries a complete human attestation."""
        m = self.meta
        state = m.get("state")
        if state not in self.STATES:
            raise GoldenError(f"{self.meta_path}: state={state!r} is not one "
                              f"of {self.STATES}")
        if state == "known_bad":
            raise GoldenError(
                f"{self.path} is recorded as KNOWN BAD: "
                f"{m.get('shows', '(no description)')}. This is a defect with "
                f"a picture, not a passing test.")
        if state == "unverified":
            raise GoldenError(
                f"{self.path} was captured but never verified by eye. Look at "
                f"it and re-run with --bless --verified-by NAME --shows '...'.")
        for field in ("verified_by", "verified_at", "shows"):
            if not m.get(field):
                raise GoldenError(f"{self.meta_path}: missing {field}")
        return True

    def save(self, meta):
        self.meta = meta
        with open(self.meta_path, "w") as f:
            json.dump(meta, f, indent=2, sort_keys=True)
            f.write("\n")
