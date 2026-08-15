#!/usr/bin/env python3
"""
Per-opcode DWT cycle profile: which x86 opcodes are the cycle hogs.

Answers the question static counts cannot -- `asm_metrics.py` says how many
instructions a handler contains, this says what a dispatch of it actually COSTS
in core cycles including stalls, weighted by how often the guest runs it. That
is the number that should direct micro-optimisation (PLAN.md Phase 2).

Needs a target built with X86_PROFILE=1 (Makefile / Makefile.core, OFF by
default -- the instrumentation sits in the fetch/dispatch tail). Flags are not
dependencies in this project, so build clean:

    make clean && make sdcard X86_PROFILE=1        # tier 2/3 core
    make clean && make X86_PROFILE=1               # tier 1 harness

Then, TOPBENCH as the workload:

    # tier 2 -- gwemu. Proves the plumbing and the COUNTS. Cycles are noise.
    python3 scripts/profile_opcodes.py --retrogo --settle 12 --window 12

    # tier 3 -- silicon. The only run whose cycle figures mean anything.
    python3 scripts/profile_opcodes.py --retrogo --target hardware \\
        --settle 20 --window 20 --json build/artifacts/profile_topbench.json

    # sort to find individually expensive handlers, or hot-but-cheap ones
    python3 scripts/profile_opcodes.py --retrogo --sort mean
    python3 scripts/profile_opcodes.py --retrogo --sort count

`--settle` runs the guest and then RESETS the table, so the window measures a
steady state rather than DOS startup and the unpacker. `--json`/`--csv` make a
profile diffable across builds, the way tests/asm_baseline.json already is for
static counts.

Read the caveat the tool prints. Attribution is approximate by construction and
a gwemu profile is not a profile.
"""

import argparse
import os
import sys

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common import opcode_profile as prof                  # noqa: E402
from scripts.common.probe import load_symbol_maps, wait_for_value  # noqa: E402
from scripts.common.target import (Image, add_target_args,         # noqa: E402
                                   open_target, timeline_quit_time)

FW_DIR = "references/game-and-watch-retro-go-sd/build"
CORE_MAGIC = 0x54494E59


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_target_args(p)
    p.add_argument("--retrogo", action="store_true",
                   help="boot retro-go + our packed core (the tier the real "
                        "workload runs in); default is the bare-metal harness")
    p.add_argument("--firmware-dir", default=FW_DIR)
    p.add_argument("--sd", default="sdcard_retrogo.img")
    p.add_argument("--elf", default=None, help="ELF for symbols")
    p.add_argument("--settle", type=float, default=12.0,
                   help="seconds to run BEFORE resetting the table, so the "
                        "window is a steady state and not DOS startup")
    p.add_argument("--window", type=float, default=12.0,
                   help="seconds to profile after the reset")
    p.add_argument("--sort", choices=["total", "mean", "count"],
                   default="total")
    p.add_argument("--top", type=int, default=40, help="rows to print")
    p.add_argument("--opsize", type=int, choices=[16, 32], default=16,
                   help="which pass's handler symbols to name (real-mode DOS "
                        "guests are 16)")
    p.add_argument("--include-host", action="store_true",
                   help="also rank the HOST slot (time outside the interpreter)")
    p.add_argument("--json", metavar="PATH", help="machine-readable output")
    p.add_argument("--csv", metavar="PATH", help="machine-readable output")
    p.add_argument("--bench", action="store_true",
                   help="bare-metal harness only: run the synthetic benchmark "
                        "inside the window. It executes EXACTLY 1,000,001 "
                        "instructions (500k DEC EAX + 500k JNZ + HLT), so the "
                        "counts are checkable against a known answer -- which "
                        "is how the plumbing is proven under gwemu")
    p.add_argument("--no-reset", action="store_true",
                   help="do not reset; read whatever has accumulated since boot")
    p.add_argument("--gate", action="append", metavar="SYM[=climb|const]",
                   help="assert a counter MOVED (or did not) across the window, "
                        "so the report proves WHICH workload it profiled. "
                        "Repeatable and comma-separated. Mode Y gameplay is "
                        "--gate x86_modey_presents; a menu control is "
                        "--gate x86_modey_presents=const. A failed gate makes "
                        "the run exit non-zero")
    p.add_argument("--compare", nargs=2, metavar=("A.json", "B.json"),
                   help="no target: decompose the cycles/instruction difference "
                        "between two saved profiles into a MIX change and a "
                        "per-opcode COST change")
    args = p.parse_args()

    if args.compare:
        prof.compare(args.compare[0], args.compare[1], top=args.top)
        return 0

    if args.retrogo:
        fw = args.firmware_dir
        elf = args.elf or "build/core/dos_core.elf"
        image = Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                      bank2=os.path.join(fw, "qemu_bank2.bin"),
                      extflash=os.path.join(fw, "extflash.bin"),
                      sdcard=args.sd, elf=elf, intflash_bank=1,
                      intflash=os.path.join(fw, "gw_retro_go_intflash.bin"))
        if args.timeline is None:
            args.timeline = "timelines/run-to-completion.tl"
        fw_elf = os.path.join(fw, "gw_retro_go.elf")
    else:
        elf = args.elf or "test_core.elf"
        image = Image(bank1="qemu_bank1.bin", bank2="qemu_bank2.bin",
                      extflash="extflash.bin", sdcard="sdcard.img",
                      elf=elf, intflash_bank=1)
        fw_elf = None

    syms, maps = load_symbol_maps(fw_elf, elf)

    # A run that outlives its timeline loses gwemu mid-read, and the dropped
    # GDB link reads as a target fault. Derive the budget, never hardcode it.
    if args.target != "hardware" and args.timeline:
        quit_at = timeline_quit_time(args.timeline)
        if quit_at and args.settle + args.window > quit_at - 3:
            prof.die(f"--settle + --window ({args.settle + args.window:.0f}s) "
                     f"outlives the timeline's quit at {quit_at:.0f}s "
                     f"({args.timeline}). Shorten the window.")

    target = open_target(args, image)
    target.start()
    try:
        if args.retrogo and "core_magic" in syms:
            print("waiting for core_magic...")
            if not wait_for_value(target.backend, syms["core_magic"],
                                  CORE_MAGIC, 30.0):
                prof.die("the core never started; nothing to profile")
            print("core running")

        prof.wait(args.settle, "settling (this time is discarded)")

        target.backend.halt()
        try:
            addr = prof.block_address(target.backend, syms)
            if not args.no_reset:
                prof.reset(target.backend, addr)
            gates = prof.parse_gates(args.gate)
            gate_before = prof.read_gates(target.backend, syms, gates)
        except prof.ProfileError as exc:
            target.backend.resume()
            prof.die(str(exc))
        target.backend.resume()

        if args.bench:
            if "run_benchmark_flag" not in syms:
                prof.die("--bench needs the bare-metal harness "
                         "(run_benchmark_flag is not in this ELF)")
            target.backend.write_uint32(syms["run_benchmark_flag"], 1)
            print("  benchmark started; waiting for it to finish")
            if not wait_for_value(target.backend, syms["run_benchmark_flag"],
                                  0, args.window):
                prof.die("the benchmark did not finish inside --window")
            insns = target.backend.read_uint32(syms["benchmark_insns"])
            print(f"  benchmark reported {insns} instructions")
        else:
            prof.wait(args.window, "profiling")

        target.backend.halt()
        gate_after = prof.read_gates(target.backend, syms, gates)
        profile = prof.read_profile(target.backend, addr)
        handlers = prof.resolve_handlers(target.backend, syms, maps,
                                         opsize=args.opsize)
        target.backend.resume()

        print("")
        gates_ok = prof.check_gates(gates, gate_before, gate_after)
        print("fault state after the run:")
        clean = prof.check_clean(target, maps)
        if fw_elf:
            # The firmware log is the only channel the device has to report a
            # fault, and a profile read off a faulted target is a lie.
            from scripts.common.rglog import dump_log
            target.backend.halt()
            dump_log(target.backend, fw_elf, tail=12)
            target.backend.resume()

        rows, total = prof.rank(profile, handlers, sort=args.sort,
                                include_host=args.include_host)
        print("")
        prof.print_table(rows, total, profile, args.target, top=args.top,
                         sort=args.sort)

        meta = {"settle_s": args.settle, "window_s": args.window,
                "elf": elf, "opsize": args.opsize,
                "fault_state_clean": clean,
                "gates": [{"symbol": g, "want": w,
                           "before": gate_before.get(g),
                           "after": gate_after.get(g)} for g, w in gates],
                "gates_ok": gates_ok}
        if args.json:
            os.makedirs(os.path.dirname(args.json) or ".", exist_ok=True)
            print(f"\nwrote {prof.write_json(args.json, rows, total, profile, args.target, meta)}")
        if args.csv:
            os.makedirs(os.path.dirname(args.csv) or ".", exist_ok=True)
            print(f"wrote {prof.write_csv(args.csv, rows)}")

        if not rows:
            prof.die("no opcode executed during the window -- is the guest "
                     "running? Check with scripts/probe.py.")
        return 0 if (clean and gates_ok) else 1
    finally:
        target.stop()


if __name__ == "__main__":
    sys.exit(main())
