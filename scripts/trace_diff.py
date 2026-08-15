#!/usr/bin/env python3
"""
Diff our interpreter's instruction trace against a reference x86, and bisect.

This is the tool for "the guest runs a million instructions and then executes
its own data". It rebuilds the core with the sampler in cpu_exec.S switched on
at a chosen stride and offset, runs it under gwemu, runs the identical image
under Unicorn, and reports the first sample where the two disagree.

    # coarse: 64 samples every 16384 instructions, spanning ~1M
    python3 scripts/trace_diff.py --dsk references/.../TOPBENCH.dsk --args=-l

    # narrow into the window the coarse pass reported
    python3 scripts/trace_diff.py --dsk ... --args=-l --shift 8 --skip 32768
    python3 scripts/trace_diff.py --dsk ... --args=-l --shift 0 --skip 43520

    # walk it automatically: 14 -> 8 -> 0, three runs, no arithmetic
    python3 scripts/trace_diff.py --dsk ... --args=-l --bisect

Why this exists as a script rather than as a sequence of commands: every part of
it is a trap.

  * A failed build followed by `&&`-chained steps still leaves a STALE image on
    the SD card, and the probe then reports a previous build's trace as if it
    were the new one. Every step here is checked.
  * The sampler must SATURATE, not wrap. A 64-entry ring spans 64 samples
    wherever you point it and quietly shows the end of the run instead of the
    window you asked for.
  * A long `REP` makes thousands of consecutive samples identical, so an
    index-by-index EIP diff reports "match" where there is only coincidence.
    Confirm any divergence by re-running with `--reg CX` and comparing a
    register over the same window.
  * Anything our interpreter executes that the reference cannot know about --
    HLE stub instructions after an interrupt -- shifts our index against
    theirs. `x86_cs_log_idx` tells you whether any interrupt was taken at all;
    if it is 1 (the program's own initial far transfer) the counters are
    directly comparable.

x86_trace.inc is restored to tracing-off on exit unless --keep-trace.
"""

import argparse
import os
import re
import subprocess
import sys

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common import refx86                        # noqa: E402
from scripts.common.probe import load_symbol_maps, sample, wait_for_value  # noqa: E402
from scripts.common.target import Image, add_target_args, open_target  # noqa: E402

FW_DIR = "references/game-and-watch-retro-go-sd/build"
CORE_MAGIC = 0x54494E59
TRACE_INC = "x86_trace.inc"

TRACE_OFF = """/* Instruction-trace parameters for cpu_exec.S.
 *
 * Rewritten by scripts/trace_diff.py; checked in with tracing OFF so a normal
 * build carries none of it. Defining X86_TRACE_EIP samples the linear EIP into
 * x86_eip_log[64], one slot every 1<<X86_TRACE_SHIFT instructions, starting
 * after X86_TRACE_SKIP of them and stopping once the 64 slots are full. A
 * saturating window rather than a ring: a ring spans only its own length and
 * silently shows the end of the run wherever you point it.
 */
/* #define X86_TRACE_EIP 1 */
#define X86_TRACE_SHIFT 14
#define X86_TRACE_SKIP  0
"""

# Byte offsets into X86CPUState, for --reg. A long REP makes thousands of
# consecutive EIP samples identical, so a register is the only way to tell a
# real divergence from a coincidental match.
REG_OFFSETS = {"AX": 8, "CX": 12, "DX": 16, "BX": 20,
               "SP": 24, "BP": 28, "SI": 32, "DI": 36, "FL": 4,
               # segment slots hold the cached BASE, not the selector
               "ES": 52, "CS": 64, "SS": 76, "DS": 88}

# CF PF ZF SF DF OF. Reserved bits, IF and IOPL differ by construction. AF is
# left out deliberately: x86 leaves it UNDEFINED after every shift, rotate,
# logical op and MUL/DIV, so the two models disagree there legitimately and
# comparing it buries the real divergences in noise. Add (1 << 4) back when
# chasing something that reads AF.
FLAG_MASK = (1 << 0) | (1 << 2) | (1 << 6) | (1 << 7) \
    | (1 << 10) | (1 << 11)


def write_trace_inc(on, shift, skip, reg=None):
    body = TRACE_OFF
    if on:
        body = body.replace("/* #define X86_TRACE_EIP 1 */",
                            "#define X86_TRACE_EIP 1")
    if reg == "FL":
        body += "#define X86_TRACE_FLAGS 1\n"
    elif reg:
        body += f"#define X86_TRACE_REG {REG_OFFSETS[reg]}   /* {reg} */\n"
    body = re.sub(r"#define X86_TRACE_SHIFT .*",
                  f"#define X86_TRACE_SHIFT {shift}", body)
    body = re.sub(r"#define X86_TRACE_SKIP .*",
                  f"#define X86_TRACE_SKIP  {skip}", body)
    open(TRACE_INC, "w").write(body)


def build(rom):
    subprocess.run(["make", "-f", "Makefile.core", "pack"], check=True,
                   stdout=subprocess.DEVNULL)
    subprocess.run(["python3", "scripts/sdcard.py", "--image",
                    "sdcard_retrogo.img", "--rom", rom], check=True,
                   stdout=subprocess.DEVNULL)


def run_target(args, count=64):
    fw = args.firmware_dir
    elf = "build/core/dos_core.elf"
    image = Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                  bank2=os.path.join(fw, "qemu_bank2.bin"),
                  extflash=os.path.join(fw, "extflash.bin"),
                  sdcard=args.sd, elf=elf, intflash_bank=1)
    if args.timeline is None:
        args.timeline = "timelines/start-preselected-game.tl"
    syms, _ = load_symbol_maps(os.path.join(fw, "gw_retro_go.elf"), elf)
    target = open_target(args, image)
    target.start()
    try:
        backend = target.backend
        wait_for_value(backend, syms["core_magic"], CORE_MAGIC, 30.0)
        sample(target, syms, watches=["x86_eip_log_idx", "x86_cs_log_idx"],
               count=2, interval=args.settle, out=lambda *_: None)
        backend.halt()
        total = backend.read_uint32(syms["x86_eip_log_idx"])
        cs_loads = backend.read_uint32(syms["x86_cs_log_idx"])
        raw = bytes(backend.read_memory(syms["x86_eip_log"], count * 4))
        backend.resume()
    finally:
        target.stop()
    ours = [int.from_bytes(raw[i * 4:i * 4 + 4], "little") for i in range(count)]
    return ours, total, cs_loads


def compare(ours, shift, skip, dsk, exe, cmdline, count=64, reg=None):
    exe_name, exe_bytes = refx86.load_exe(dsk, exe)
    every = 1 << shift
    ref, info = refx86.run(exe_bytes, cmdline=cmdline, skip=skip, every=every,
                           count=count)
    print(f"reference: {exe_name}, stop={info['stop']} after {info['n']} "
          f"instructions")
    first = None
    for k, row in enumerate(ref):
        if k >= len(ours):
            break
        mark = "  "
        if reg == "FL":
            mine, theirs = ours[k] & FLAG_MASK, row["FL"] & FLAG_MASK
        elif reg in ("ES", "CS", "SS", "DS"):
            mine, theirs = ours[k], row[reg] * 16
        elif reg:
            mine, theirs = ours[k] & 0xFFFF, row[reg]
        else:
            mine, theirs = ours[k], row["addr"]
        if mine != theirs:
            mark = ">>"
            if first is None:
                first = row["n"]
        print(f"{mark} {row['n']:9d} ours={mine:#08x} ref={theirs:#08x}"
              f"  {refx86.format_row(row).split('  ', 1)[1]}")
        if first is not None:
            break
    return first, ref, info


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_target_args(p)
    p.add_argument("--dsk", required=True)
    p.add_argument("--exe", default=None)
    p.add_argument("--args", dest="cmdline", default="")
    p.add_argument("--firmware-dir", default=FW_DIR)
    p.add_argument("--sd", default="sdcard_retrogo.img")
    p.add_argument("--shift", type=int, default=14,
                   help="sample every 1<<SHIFT instructions (default 14)")
    p.add_argument("--skip", type=int, default=0,
                   help="start sampling after this many instructions")
    p.add_argument("--bisect", action="store_true",
                   help="run shift 14, then 8, then 0, narrowing each time")
    p.add_argument("--settle", type=float, default=6.0,
                   help="seconds to let the guest run before reading the log")
    p.add_argument("--reg", choices=sorted(REG_OFFSETS),
                   help="sample this guest register instead of the EIP. Use it "
                        "to confirm a divergence inside a long REP, where every "
                        "EIP sample is the same address")
    p.add_argument("--keep-trace", action="store_true",
                   help="leave x86_trace.inc switched on")
    args = p.parse_args()

    def one(shift, skip):
        print(f"\n=== stride 1<<{shift} = {1 << shift}, skip {skip} ===")
        write_trace_inc(True, shift, skip, args.reg)
        build(args.dsk)
        ours, total, cs_loads = run_target(args)
        print(f"target: {total} guest instructions, "
              f"{cs_loads} CS load(s) logged")
        if cs_loads > 1:
            print("  NOTE: a far transfer or interrupt was taken. If any of "
                  "them entered an HLE stub, our instruction count runs ahead "
                  "of the reference's and the diff below is offset.")
        first, _, _ = compare(ours, shift, skip, args.dsk, args.exe,
                              args.cmdline, reg=args.reg)
        return first

    try:
        if not args.bisect:
            one(args.shift, args.skip)
            return 0
        skip = args.skip
        for shift in (14, 8, 0):
            # 64 slots span 64<<shift instructions, but the level above only
            # localised the divergence to ONE of its strides -- which at the
            # finest level is wider than the window. So walk the window forward
            # until it is found rather than reporting "no divergence" for a
            # window that simply started too early.
            span = 64 << shift
            limit = skip + max(span, 1 << (shift + 8))
            first = None
            while skip < limit:
                first = one(shift, skip)
                if first is not None:
                    break
                skip += span
                print(f"clean through {skip}; advancing the window")
            if first is None:
                print("no divergence found in the searched range")
                break
            print(f"first divergence at or before instruction {first}")
            skip = max(0, first - (1 << shift))
    finally:
        if not args.keep_trace:
            write_trace_inc(False, 14, 0)
            print(f"\n{TRACE_INC} restored to tracing off "
                  f"(rebuild before measuring anything)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
