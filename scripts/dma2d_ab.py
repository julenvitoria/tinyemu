#!/usr/bin/env python3
"""
A/B the Chrom-ART (DMA2D) offload of the chained mode 13h blit.

    python3 scripts/dma2d_ab.py                  # tier 2, both arms, compare
    python3 scripts/dma2d_ab.py --keep-dsk       # leave build/CHAIN13.dsk
    python3 scripts/dma2d_ab.py --target hardware   # tier 3, see below

WHY THIS SCRIPT EXISTS AT ALL.

`X86_VIDEO_DMA2D=1` (src/main.c, `dma2d_blit_hook`) is merged, OFF and, until
this, exercised by nothing: it covers CHAINED 13h only -- DMA2D has no L8
output format, so text, CGA, EGA 0Dh and Mode Y can never use it -- and no
title in this tree holds a chained 13h screen. WOLF3D unchains within a second
of starting, KEEN4 is 0Dh, ALLEYCAT is CGA. An A/B against those proves the
FALLBACK still works and never starts the engine once.

So the workload is SYNTHETIC and built here: a 27-byte DOS program that sets mode
13h, fills all 64,000 bytes of the aperture with `i & 0xFF` and spins. The
screen is then static, which is what makes an instruction-count capture point
land on the same picture every time, and the expected image is arithmetic this
script computes itself -- so a match is not two copies of the same bug
agreeing.

STATUS: THE HARNESS RUNS, THE GUEST DOES NOT YET REACH MODE 13h.

Do not read a PASS from this yet, and do not "fix" it by lowering a bound.
Measured, tier 2, both arms:

    dsk_mounted=1  dsk_startup_ok=1  dsk_exec_rc=0   -- the disk mounts, the
        AUTOEXEC is parsed and the MZ loader accepts CHAIN13.EXE
    x86_dma2d_selftest=1                            -- the ABI, START and POLL
        all work under gwemu; the plumbing is not the problem
    guest_insns_total freezes at 477,545, guest_halted=1,
        guest_exit_reason=4 (X86_EXIT_UNIMPLEMENTED -- HLT or an opcode with no
        handler), with CS=0x2322 and a linear EIP of 0x8CDE2, i.e. control has
        left the 27 bytes of the program entirely
    x86_video_mode_byte stays 0x03, so the INT 10h AX=0013h never took

Control leaving the program at its first INT is where to start: either the
vector this core installs for 10h does not land where the program's INT sends
it, or the entry state the MZ loader hands a 2-paragraph-header .EXE is not
what this program assumes. The same INT 10h AX=0013h sequence works at tier 1
(tests/main.c:539, modeswitch_test_init), which is the wedge -- that path is
driven with x86_hle_install() vectors the harness chooses, and this one is not.

Until that is closed, the A/B compares two text screens and says so: the
`x86_dma2d_blits is 0` check below is what refuses to call that a pass.

WHAT IT PROVES, AND WHAT IT CANNOT.

  PROVES (tier 2, gwemu): the two builds put the SAME PIXELS on the panel, the
  pixels are the ones the pattern says they should be, and the engine really
  ran -- `x86_dma2d_blits` is non-zero and `x86_dma2d_fallback` is zero in the
  DMA2D arm and both are absent in the control.

  CANNOT PROVE: the TIMING, or that `dma2d_sync()` is placed correctly against
  the buffer swap. gwemu runs the transfer SYNCHRONOUSLY
  (references/gwemu/hw/display/gnw_h7b0_dma2d.c:307), so a missing or misplaced
  wait completes before the next instruction there and would tear only on
  silicon. Nothing below tier 3 can see that. See the recipe this prints.
"""

import argparse
import os
import subprocess
import sys

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.fat12 import build_image                          # noqa: E402
from scripts.common.golden import compare, format_report              # noqa: E402
from scripts.common.screen import write_png                           # noqa: E402
from scripts.common.target import add_target_args                     # noqa: E402
import scripts.golden_screens as gs                                   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DSK = os.path.join("build", "CHAIN13.dsk")

# The guest. Hand-assembled because the toolchain here has no 16-bit assembler,
# and short enough to read:
#
#   B8 13 00     mov ax,0013h        ; mode 13h, 320x200x256 CHAINED
#   CD 10        int 10h
#   B8 00 A0     mov ax,0A000h
#   8E C0        mov es,ax
#   31 FF        xor di,di
#   B9 00 FA     mov cx,64000
#   30 C0        xor al,al
#   26 88 05     mov es:[di],al      ; fill: pixel i = i & 0xFF
#   FE C0        inc al
#   47           inc di
#   E2 F8        loop fill
#   EB EF        jmp again           ; paint the SAME pattern forever
#
# It repaints rather than spinning on `jmp $`. Measured: with a 2-byte self-
# jump the core stops advancing the guest after the first fill
# (guest_insns_total freezes at 477,545 and guest_ips_achieved reads 0), so the
# capture point is never reached. Repainting keeps the guest doing the work the
# blit is being measured on, and the picture is identical every pass -- which
# is what an instruction-count capture point needs.
CODE = bytes([0xB8, 0x13, 0x00, 0xCD, 0x10,
              0xB8, 0x00, 0xA0, 0x8E, 0xC0,
              0x31, 0xFF,
              0xB9, 0x00, 0xFA,
              0x30, 0xC0,
              0x26, 0x88, 0x05, 0xFE, 0xC0, 0x47, 0xE2, 0xF8,
              0xEB, 0xEF])


def mz(code):
    """Wrap `code` in the smallest honest MZ .EXE.

    It has to be an .EXE: x86_dos_exec() is an MZ loader and rejects anything
    without the signature (x86_dos.c:692), so a .COM -- which is what this
    program naturally is -- would never load. Header is 2 paragraphs, no
    relocations, entry at CS:0000, and maxalloc 0xFFFF because that is the
    idiom for "all of it" and costs nothing here.
    """
    import struct
    hdr_paras = 2
    total = hdr_paras * 16 + len(code)
    pages = (total + 511) // 512
    last = total % 512
    return struct.pack("<14H",
                       0x5A4D, last, pages, 0, hdr_paras,
                       0x10, 0xFFFF,          # minalloc, maxalloc
                       0, 0xFFFE,             # ss, sp
                       0, 0, 0,               # checksum, ip, cs
                       0x1C, 0) + code

W, H = 320, 200


def expected_indices():
    """What the guest wrote, computed here and not read from the target."""
    return bytes((i & 0xFF) for i in range(W * H))


def build_dsk(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img = build_image([("CHAIN13.EXE", mz(CODE)),
                       ("AUTOEXEC.BAT", b"CHAIN13.EXE\r\n")], sectors=128)
    with open(path, "wb") as f:
        f.write(img)
    return path


def build_core(dma2d):
    """One core build. Flags are not dependencies -- so this is always clean."""
    env = dict(os.environ, X86_VIDEO_DMA2D="1" if dma2d else "0")
    for target in ("clean", "pack"):
        r = subprocess.run(["make", "-f", "Makefile.core", target],
                           env=env, cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f"make {target} (DMA2D={int(dma2d)}) failed:\n"
                             f"{r.stdout}\n{r.stderr}")


def capture(args, name):
    """One arm. A capture that does not reach its instruction count is a
    RESULT, not a crash: the guest halting is exactly the failure this script
    currently reports, and letting a GDBError out of here buries it under a
    traceback about a closed socket."""
    entry = {"rom": DSK, "timeline": args.timeline_file, "insns": args.insns}
    try:
        return gs.capture_frame(args, entry, name)
    except Exception as exc:                       # noqa: BLE001 -- see above
        raise SystemExit(
            f"{name}: the capture point ({args.insns:,} guest instructions) "
            f"was never reached ({type(exc).__name__}: {exc}).\n"
            f"Ask the target why, do not lower the bound:\n"
            f"  python3 scripts/probe.py --retrogo "
            f"--sd build/sd-golden-{name}.img \\\n"
            f"      --elf {args.core_elf} --timeline {args.timeline_file} \\\n"
            f"      --watch guest_halted,guest_exit_reason,guest_insns_total,"
            f"x86_video_mode_byte,dsk_exec_rc \\\n"
            f"      --deref cpu_state_ptr+0=EIP --samples 3 --interval 4")


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_target_args(p)
    p.add_argument("--firmware-dir", default=gs.FW_DIR)
    p.add_argument("--core-bin", default="dos.bin")
    p.add_argument("--core-elf", default="build/core/dos_core.elf")
    p.add_argument("--reuse-sd", action="store_true")
    p.add_argument("--boot-timeout", type=float, default=40.0)
    p.add_argument("--run-timeout", type=float, default=55.0)
    p.add_argument("--insns", type=int, default=8_000_000,
                   help="capture point. The fill is ~256k guest instructions "
                        "after DOS is up; the screen never changes again")
    p.add_argument("--timeline-file",
                   default="timelines/start-preselected-game.tl")
    p.add_argument("--keep-dsk", action="store_true")
    args = p.parse_args()

    build_dsk(DSK)
    print(f"synthetic guest: {DSK} (CHAIN13.EXE, {len(CODE)} bytes of code)")

    shots = {}
    try:
        for dma2d in (0, 1):
            build_core(dma2d)
            arm = "dma2d" if dma2d else "control"
            rgb, w, h, wit = capture(args, f"chain13-{arm}")
            shots[arm] = (rgb, w, h, wit)
            out = f"build/artifacts/dma2d-ab-{arm}.png"
            write_png(out, w, h, rgb)
            print(f"  {arm}: {out}  mode=0x{(wit.get('x86_video_mode_byte') or 0) & 0xFF:02x} "
                  f"blits={wit.get('x86_dma2d_blits')} "
                  f"fallback={wit.get('x86_dma2d_fallback')} "
                  f"waits={wit.get('x86_dma2d_waits')} "
                  f"selftest={wit.get('x86_dma2d_selftest')}")
    finally:
        if not args.keep_dsk and os.path.exists(DSK):
            os.remove(DSK)

    fails = []

    # 1. The two arms must be pixel-identical.
    (a, w, h, wa), (b, _, _, wb) = shots["control"], shots["dma2d"]
    res = compare(a, b, w, h)
    print("\ncontrol vs dma2d:")
    print("\n".join(format_report(res)))
    if res["n_diff"]:
        fails.append("the two builds do not put the same pixels on the panel")

    # 2. ...and the pixels must be the ones the PATTERN says, not merely equal
    #    to each other. Two identically broken blits agree, so equality alone
    #    is not a check.
    #
    #    The test is palette-independent by construction. The guest wrote
    #    index(x,y) = (320*y + x) & 255, and 320 == 256 + 64, so
    #
    #        index(x - 64, y + 1) == (320y + 320 + x - 64) & 255 == index(x, y)
    #
    #    Every pixel repeats one row down and 64 columns left, for ANY palette
    #    -- which pins the row pitch, the row order and the letterbox at once
    #    without this script having to know what colour a DAC entry is. A blit
    #    that dropped, doubled or mis-strode a row breaks it immediately.
    if (wa.get("x86_video_mode_byte") or 0) & 0xFF != 0x13:
        fails.append(f"the guest is not in mode 13h "
                     f"(mode_byte=0x{(wa.get('x86_video_mode_byte') or 0):04x})")
    top = (h - H) // 2                       # the letterbox bar
    for arm, (px, _, _, _) in shots.items():
        bad = 0
        for y in range(top, top + H - 1):
            for x in range(64, W):
                o = (y * w + x) * 3
                o2 = ((y + 1) * w + (x - 64)) * 3
                if px[o:o + 3] != px[o2:o2 + 3]:
                    bad += 1
        if bad:
            fails.append(f"{arm}: {bad} pixels break the pattern's own "
                         f"identity (x,y) == (x-64,y+1); the picture is not "
                         f"what the guest wrote")

    # 3. The engine has to have RUN, or the A/B graded the fallback twice.
    if not wb.get("x86_dma2d_blits"):
        fails.append("x86_dma2d_blits is 0 in the DMA2D arm: the engine never "
                     "ran, so this compared the fallback with itself")
    if wb.get("x86_dma2d_fallback"):
        fails.append(f"x86_dma2d_fallback={wb['x86_dma2d_fallback']}: START "
                     f"was refused and the row loop did the work")
    if wb.get("x86_dma2d_selftest") not in (1, None):
        fails.append(f"x86_dma2d_selftest={wb['x86_dma2d_selftest']} (want 1)")

    print()
    if fails:
        for f in fails:
            print(f"FAIL: {f}")
    else:
        print("PASS: identical pixels with the offload on and off, and the "
              "engine did the work")

    print(__doc__.split("WHAT IT PROVES")[0].strip()[:0] or "", end="")
    print("""
TIER 3 -- the part this cannot settle. Run on the device, with the owner's
hands on it, and look at the panel:

    X86_VIDEO_DMA2D=1 make -f Makefile.core clean pack
    python3 scripts/dma2d_ab.py --target hardware --keep-dsk
    python3 scripts/probe.py --target hardware --retrogo \\
        --watch x86_dma2d_blits,x86_dma2d_fallback,x86_dma2d_waits,\\
x86_dma2d_selftest,frames_rendered --samples 20 --interval 1

What to look for, in order:
  1. x86_dma2d_selftest == 1. If it is 2 the ABI refused START, if it is 3 the
     bytes did not move; either way stop, nothing below this matters.
  2. x86_dma2d_blits climbing with frames_rendered, x86_dma2d_fallback flat 0.
  3. THE PANEL. A torn frame -- a horizontal seam with the old picture above
     and the new below, moving between frames -- means the swap overtook the
     engine and dma2d_sync() is in the wrong place. gwemu cannot show this.
  4. frames_rendered per second against the same run with X86_VIDEO_DMA2D=0.
     The claim being tested is a ~10x cheaper blit, because the framebuffer is
     strongly-ordered for the CPU (MPU TEX=0/C=0/B=0) and not for the engine.
     A smaller win than that is still a win; NO win means the framebuffer is
     not what src/main.c's comment says it is, and that comment is the thing
     to correct.""")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
