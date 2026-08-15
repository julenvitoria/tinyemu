#!/usr/bin/env python3
"""
The golden-screenshot gate: does the emulator still draw the right PICTURE?

Two "the guest runs happily and the screen is garbage" regressions shipped in
one session and every tier-1 test passed through both, because nothing in the
suite can see a picture. This closes that: it boots a title under gwemu (or on
silicon), reconstructs the frame the LTDC is scanning out, and compares it
PIXEL BY PIXEL against a stored golden -- reporting where it differs, not just
whether it does.

    # what the gate covers
    python3 scripts/golden_screens.py list

    # run every title and compare (this is the gate)
    python3 scripts/golden_screens.py check --firmware-dir build/fw

    # one title, keeping the capture for inspection
    python3 scripts/golden_screens.py check --title WOLF3D

    # capture a candidate WITHOUT blessing it; then LOOK AT IT
    python3 scripts/golden_screens.py capture --title KEEN4

    # ...and only then record it as the golden
    python3 scripts/golden_screens.py capture --title KEEN4 --bless \\
        --verified-by doug --shows "Keen 4 startup screen, EGA 0Dh banner"

## The trap this is designed against

**A golden captured from a broken build silently blesses the bug**, and every
run afterwards agrees with it. So an image alone is never a golden here: it is
a golden only with an attestation sidecar naming who verified it by eye, when,
and what they saw. `check` REFUSES to pass a golden whose sidecar is missing,
incomplete, or marked `unverified`/`known_bad` -- a known-bad golden is a
recorded defect with a picture, and it fails.

Re-blessing is deliberate and visible, never a silent overwrite: `--bless` over
an existing golden requires `--because`, prints the pixel difference against the
image it is replacing, and appends the previous attestation to a `history` list
in the sidecar. The sha256 in the sidecar is checked on load, so an image
swapped underneath its attestation is an error rather than a new baseline.

## Determinism

The capture point is an INSTRUCTION COUNT (`guest_insns_total`), not a
wall-clock sleep: the emulator's speed varies with host load, so a `sleep(20)`
capture is a different moment in the guest on every run and the resulting
"tolerance" is really just a slop budget for the harness. Where a title needs
keypresses to reach the screen, the timeline still delivers those on wall clock
-- so the shipped set of screens is deliberately chosen to be STATIC (title
screens and input-waiting menus), where a few million instructions either side
of the capture point change nothing.
"""

import argparse
import datetime
import json
import os
import sys
import time

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.elf import get_symbols                            # noqa: E402
from scripts.common.golden import (                                   # noqa: E402
    Golden, GoldenError, compare, format_report, read_png, sha256)
from scripts.common.screen import (                                   # noqa: E402
    SCREEN_H, SCREEN_W, read_active_clut, read_ltdc, write_png)
from scripts.common.target import Image, add_target_args, open_target  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GOLDEN_DIR = os.path.join(ROOT, "tests", "goldens")
MANIFEST = os.path.join(GOLDEN_DIR, "manifest.json")
CORE_MAGIC = 0x54494E59
FW_DIR = "references/game-and-watch-retro-go-sd/build"

# Counters recorded alongside every capture. They do not gate anything -- the
# PICTURE does -- but when a comparison fails they are the first question
# asked, and re-running to collect them means re-running a failure that may not
# reproduce.
WITNESS = ("x86_video_mode_byte", "frames_rendered", "guest_insns_total",
           "x86_vga_modey_lost_cache", "x86_vga_modey_lost_nocache",
           "x86_vga_latch_miss", "x86_vga_planar_lost",
           "x86_vga_modey_cache_offs", "x86_vga_modey_lost_hi",
           "x86_vga_modey_lost_lo",
           "x86_vga_ega_card_offsets", "x86_arena_largest",
           # The DONATION POLICY. A card size on its own does not say whether
           # the frames behind it were idle or taken off a working set, and
           # that is the whole question the policy decides -- so every golden
           # run now reports the reserve it ran under and what pressure it saw.
           "x86_pg_donated", "x86_pg_frames_total", "x86_pg_resident_peak",
           "x86_pg_reserve_eff", "x86_pg_evictions", "x86_pg_wb_file",
           "x86_pg_shrink_signals", "x86_pg_grow_signals",
           # The RAM_EMU alarm (STATUS.md's Budgets row). A pool one frame
           # short changes every pager number under it, so a golden run that
           # did not report it could not be compared with one that did.
           "guest_ram_pages", "guest_ram_pages_short",
           # The DMA2D offload's witnesses. Absent from a control build, which
           # _read_u32 answers with None rather than a missing key -- that is
           # the difference between "the engine did nothing" and "this build
           # has no engine in it", and scripts/dma2d_ab.py grades both.
           "x86_dma2d_blits", "x86_dma2d_fallback", "x86_dma2d_waits",
           "x86_dma2d_selftest")


def load_manifest():
    man = json.load(open(MANIFEST))
    for name, e in man["titles"].items():
        if e.get("mask") and not e.get("mask_reason"):
            raise GoldenError(
                f"{name}: a mask with no mask_reason. A masked region is a "
                f"hole in the gate, so the manifest must say in words what "
                f"makes that region ungradeable.")
    return man


def golden_png(name):
    return os.path.join(GOLDEN_DIR, name + ".png")


def _read_u32(b, syms, name):
    return b.read_uint32(syms[name]) if name in syms else None


def make_sd(name, entry, args):
    """The SD image for one title, with the ROM preselected.

    Rebuilt every run by default. `scripts/sdcard.py` packages the core that is
    on disk, and the documented way to lose an afternoon here is to grade a
    change against an image assembled around the PREVIOUS core -- the picture
    then looks unchanged for a reason that has nothing to do with the picture.
    """
    img = os.path.join("build", f"sd-golden-{name.lower()}.img")
    if args.reuse_sd and os.path.exists(img):
        print(f"  reusing {img} (--reuse-sd: the core in it may be stale)")
        return img
    os.makedirs("build", exist_ok=True)
    cmd = [sys.executable, "scripts/sdcard.py", "--image", img,
           "--rom", entry["rom"], "--core", args.core_bin]
    import subprocess
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise GoldenError(f"building {img} failed:\n{r.stdout}\n{r.stderr}")
    return img


def capture_frame(args, entry, name):
    """Boot the title and return (rgb, w, h, witness dict).

    Runs to a fixed `guest_insns_total`, then halts and reads the LTDC layer,
    the framebuffer and the palette the firmware actually programmed.
    """
    fw = args.firmware_dir
    sd = make_sd(name, entry, args)
    image = Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                  bank2=os.path.join(fw, "qemu_bank2.bin"),
                  extflash=os.path.join(fw, "extflash.bin"),
                  sdcard=sd,
                  elf=os.path.join(fw, "gw_retro_go.elf"),
                  intflash_bank=1)
    args.timeline = entry["timeline"]
    core = get_symbols(args.core_elf)
    target = open_target(args, image)
    target.start()
    try:
        b = target.backend
        deadline = time.time() + args.boot_timeout
        while time.time() < deadline:
            if b.read_uint32(core["core_magic"]) == CORE_MAGIC and \
                    b.read_uint32(core["frames_rendered"]) > 2:
                break
            time.sleep(0.25)
        else:
            raise GoldenError("core never started drawing")

        # Adaptive polling. A flat 0.2 s poll overshoots the capture point by
        # a MILLION guest instructions -- several guest frames -- so two runs
        # land on different frames and any animated element on the screen
        # reads as a difference. Closing in with short sleeps pulls the
        # overshoot down to a fraction of a frame, which is what makes the
        # capture point reproducible at all.
        want = int(entry["insns"])
        deadline = time.time() + args.run_timeout
        last = 0
        while time.time() < deadline:
            last = b.read_uint32(core["guest_insns_total"])
            if last >= want:
                break
            behind = want - last
            time.sleep(0.2 if behind > 20_000_000 else
                       0.02 if behind > 2_000_000 else 0.0)
        else:
            raise GoldenError(
                f"only {last:,} of the {want:,} instructions the capture point "
                f"needs were executed in {args.run_timeout:g}s. This is a "
                f"harness/timing question, not a picture question -- do not "
                f"lower the capture point to make it fit.")

        b.halt()
        info = read_ltdc(b)
        if info is None:
            raise GoldenError("this target does not model the LTDC")
        pitch, lines = info["pitch"], info["lines"]
        data = bytes(b.read_memory(info["fb"], pitch * lines))
        fw_syms = get_symbols(image.elf)
        clut, clut_src = read_active_clut(b, fw_syms, core)
        witness = {k: _read_u32(b, core, k) for k in WITNESS}
        # The CRTC geometry, read straight out of the register file. "How many
        # rows does the fitted card cover?" is pitch and start, and neither is
        # a global -- x86_vga_row_pitch() and x86_vga_crtc_start() derive them
        # from vga_regs. A clipped picture asks this question first and it is
        # not worth a re-run to answer.
        if "vga_regs" in core:
            crtc = bytes(b.read_memory(core["vga_regs"], 32))
            off = crtc[0x13]
            witness["crtc_offset"] = off
            witness["row_pitch"] = (off * 2) if off else 80
            witness["crtc_start"] = (crtc[0x0C] << 8) | crtc[0x0D]
        b.resume()
    finally:
        target.stop()

    w, h = min(pitch, SCREEN_W), min(lines, SCREEN_H)
    rgb = bytearray(w * h * 3)
    for y in range(h):
        for x in range(w):
            argb = clut[data[y * pitch + x]] if clut else 0
            o = (y * w + x) * 3
            rgb[o] = (argb >> 16) & 0xFF
            rgb[o + 1] = (argb >> 8) & 0xFF
            rgb[o + 2] = argb & 0xFF
    witness["clut_src"] = clut_src
    witness["ltdc"] = {"pitch": pitch, "lines": lines, "fmt": info["pf"] & 7}
    return bytes(rgb), w, h, witness


def do_list(args):
    man = load_manifest()
    print(f"{'title':<12} {'renderer':<22} {'state':<11} tol  max%  shows")
    for name, e in sorted(man["titles"].items()):
        try:
            g = Golden.load(golden_png(name))
            state, shows = g.meta.get("state", "?"), g.meta.get("shows", "")
        except GoldenError as exc:
            state, shows = "MISSING", str(exc).split(".")[0]
        print(f"{name:<12} {e.get('renderer', ''):<22} {state:<11} "
              f"{e.get('tolerance', 0):<4} {e.get('max_diff_pct', 0):<5} {shows}")
    return 0


def _candidate_path(name):
    return os.path.join("build", "artifacts", f"golden-{name}.candidate.png")


def do_capture(args):
    man = load_manifest()
    names = [args.title] if args.title else sorted(man["titles"])
    rc = 0
    for name in names:
        entry = man["titles"][name]
        print(f"=== {name} ({entry.get('renderer', '?')})")
        rgb, w, h, witness = capture_frame(args, entry, name)
        os.makedirs("build/artifacts", exist_ok=True)
        cand = _candidate_path(name)
        write_png(cand, w, h, rgb)
        print(f"  captured {cand} ({w}x{h})")
        print(f"  witness: " + " ".join(
            f"{k}={v}" for k, v in witness.items() if k != "ltdc"))
        if not args.bless:
            print("  NOT blessed. Look at the image; if it is right, re-run "
                  "with --bless --verified-by NAME --shows '...'")
            continue
        rc |= _bless(name, cand, entry, witness, args)
    return rc


def _bless(name, cand, entry, witness, args):
    if not args.verified_by or not args.shows:
        print("  refusing to bless: --verified-by and --shows are both "
              "required. A golden with no record of who looked at it and what "
              "they saw is how a broken build gets blessed.")
        return 1
    dest = golden_png(name)
    history = []
    if os.path.exists(dest):
        if not args.because:
            print(f"  refusing to overwrite the existing golden {dest} "
                  f"without --because. Re-blessing is a claim that the OLD "
                  f"golden was wrong; say why.")
            return 1
        try:
            old = Golden.load(dest)
            history = old.meta.get("history", []) + [
                {k: v for k, v in old.meta.items() if k != "history"}]
        except GoldenError as exc:
            print(f"  (previous golden unreadable: {exc})")
        ow, oh, orgb = read_png(dest)
        nw, nh, nrgb = read_png(cand)
        if (ow, oh) == (nw, nh):
            res = compare(orgb, nrgb, ow, oh, tol=entry.get("tolerance", 0),
                          mask=entry.get("mask", ()))
            print("  what re-blessing CHANGES against the old golden:")
            for line in format_report(res, indent="    "):
                print(line)
        else:
            print(f"  geometry changed {ow}x{oh} -> {nw}x{nh}")

    os.makedirs(GOLDEN_DIR, exist_ok=True)
    with open(dest, "wb") as f:
        f.write(open(cand, "rb").read())
    meta = {
        "title": name,
        "renderer": entry.get("renderer"),
        "state": args.state,
        "shows": args.shows,
        "verified_by": args.verified_by,
        "verified_at": datetime.datetime.now().astimezone().isoformat(
            timespec="seconds"),
        "because": args.because or "first capture",
        "sha256": sha256(dest),
        "witness": witness,
        "timeline": entry["timeline"],
        "insns": entry["insns"],
    }
    if history:
        meta["history"] = history
    Golden(dest, meta).save(meta)
    print(f"  blessed {dest} as {args.state} by {args.verified_by}")
    return 0


def do_repeat(args):
    """Capture the same point N times and diff the captures against each other.

    A capture point has to be proven DETERMINISTIC before it can be a golden,
    and the proof is not "it looked the same" -- it is two captures that agree
    pixel for pixel. Anything that fails here is animating at the chosen
    instruction count, and the answer is a different capture point, never a
    wider tolerance: a tolerance large enough to absorb a moving sprite is
    large enough to absorb the corruption this gate exists to catch.
    """
    man = load_manifest()
    names = [args.title] if args.title else sorted(man["titles"])
    bad = []
    for name in names:
        entry = man["titles"][name]
        print(f"=== {name} ({entry.get('renderer', '?')}) "
              f"at {entry['insns']:,} insns, {args.runs} runs")
        shots = []
        for i in range(args.runs):
            rgb, w, h, witness = capture_frame(args, entry, name)
            print(f"  run {i}: guest_insns_total="
                  f"{witness['guest_insns_total']:,} "
                  f"frames_rendered={witness['frames_rendered']}")
            shots.append((rgb, w, h))
        for i in range(1, len(shots)):
            res = compare(shots[0][0], shots[i][0], shots[0][1], shots[0][2],
                          tol=entry.get("tolerance", 0),
                          mask=entry.get("mask", ()))
            for line in format_report(res, indent="    "):
                print(line)
            if res["n_diff"]:
                bad.append(name)
    if bad:
        print(f"\nNOT REPRODUCIBLE: {', '.join(sorted(set(bad)))}")
        return 1
    print("\nreproducible: every run produced an identical frame")
    return 0


def do_check(args):
    man = load_manifest()
    names = [args.title] if args.title else sorted(man["titles"])
    failures, known_bad = [], []
    for name in names:
        entry = man["titles"][name]
        print(f"=== {name} ({entry.get('renderer', '?')})")
        try:
            g = Golden.load(golden_png(name))
            g.verified()
        except GoldenError as exc:
            print(f"  FAIL: {exc}")
            # A known-bad golden is a DEFECT with a picture, not a regression.
            # Both fail the gate -- an outstanding defect is not a pass -- but
            # conflating them in the summary is how a real regression hides
            # behind a bug someone already knew about.
            (known_bad if "KNOWN BAD" in str(exc) else failures).append(name)
            continue
        print(f"  golden: {g.meta['shows']}  "
              f"(verified by {g.meta['verified_by']} "
              f"{g.meta['verified_at']})")
        try:
            rgb, w, h, witness = capture_frame(args, entry, name)
        except GoldenError as exc:
            print(f"  FAIL: {exc}")
            failures.append(name)
            continue
        os.makedirs("build/artifacts", exist_ok=True)
        cand = _candidate_path(name)
        write_png(cand, w, h, rgb)
        gw, gh, grgb = read_png(g.path)
        if (gw, gh) != (w, h):
            print(f"  FAIL: geometry {w}x{h} against golden {gw}x{gh}")
            failures.append(name)
            continue
        res = compare(grgb, rgb, w, h, tol=entry.get("tolerance", 0),
                      mask=entry.get("mask", ()))
        for line in format_report(res):
            print(line)
        limit = float(entry.get("max_diff_pct", 0.0))
        ok = res["pct"] <= limit
        print(f"  {'PASS' if ok else 'FAIL'}: {res['pct']:.3f}% differ "
              f"(limit {limit}%); capture kept at {cand}")
        if not ok:
            print(f"  witness: " + " ".join(
                f"{k}={v}" for k, v in witness.items() if k != "ltdc"))
            failures.append(name)
    if known_bad:
        print(f"\nKNOWN BAD ({len(known_bad)}): " + ", ".join(known_bad)
              + " -- recorded defects, each with a picture and a sidecar "
                "saying what is wrong with it. Not regressions.")
    if failures:
        print(f"\nREGRESSIONS: {len(failures)} of {len(names)}: "
              + ", ".join(failures))
    if failures or known_bad:
        return 1
    print(f"\nPASS: {len(names)} golden screens match")
    return 0


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    for cmd in ("list", "capture", "check", "repeat"):
        sp = sub.add_parser(cmd)
        sp.set_defaults(func={"list": do_list, "capture": do_capture,
                              "check": do_check, "repeat": do_repeat}[cmd])
        if cmd == "repeat":
            sp.add_argument("--runs", type=int, default=2)
        if cmd == "list":
            continue
        add_target_args(sp)
        sp.add_argument("--firmware-dir", default=FW_DIR)
        sp.add_argument("--core-bin", default="dos.bin",
                        help="packed core to put on the SD image")
        sp.add_argument("--core-elf", default="build/core/dos_core.elf")
        sp.add_argument("--reuse-sd", action="store_true",
                        help="do not rebuild the per-title SD images. Fast, "
                             "and the documented way to grade a change "
                             "against the PREVIOUS core")
        sp.add_argument("--title", help="one title instead of all of them")
        sp.add_argument("--boot-timeout", type=float, default=40.0)
        sp.add_argument("--run-timeout", type=float, default=55.0)
        if cmd == "capture":
            sp.add_argument("--bless", action="store_true",
                            help="install the capture as the golden. Requires "
                                 "--verified-by and --shows, and --because "
                                 "when one already exists")
            sp.add_argument("--verified-by",
                            help="who looked at the image")
            sp.add_argument("--shows", help="what the image shows, in words")
            sp.add_argument("--because",
                            help="why the previous golden was wrong")
            sp.add_argument("--state", default="verified",
                            choices=Golden.STATES,
                            help="'known_bad' records a defect WITH a picture; "
                                 "the gate then fails on it, which is the "
                                 "point")
    args = p.parse_args()
    try:
        return args.func(args)
    except GoldenError as exc:
        print(f"error: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
