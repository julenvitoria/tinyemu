#!/usr/bin/env python3
"""
Per-handler assembly metrics for the Thumb-2 x86 core.

Why this exists: gwemu does not model the Cortex-M7 pipeline, so it cannot show
dual-issue pairing, load-use stalls or mispredicts. Counters do translate to
silicon; scheduling does not. So assembly quality is proven in three tiers, and
this tool provides tiers 1 and 3:

  1. Static instruction count per handler, tracked as a committed baseline.
     Cheap, exact, and a handler growing becomes a reviewable event.
  2. Disassembly review for pairing and dependency chains (by hand).
  3. DWT cycle counts on real hardware, the only tier that sees pairing.

Without the static baseline every "optimization" is unfalsifiable, which is the most
expensive recurring failure mode this project has on record.

Caveat: objdump disassembles everything in the section, so the dispatch and ALU
jump tables (opcode_table_*, ALU_TABLE_*) are counted as "instructions". Their
size is stable, so deltas stay meaningful, but absolute totals are inflated by
roughly the table bytes -- do not quote the total as an instruction count.

Usage:
    python3 -m scripts.common.asm_metrics --update          # write baseline
    python3 -m scripts.common.asm_metrics                   # compare to it
    python3 -m scripts.common.asm_metrics --top 20          # biggest handlers
    python3 -m scripts.common.asm_metrics --cycles --target hardware
"""

import argparse
import json
import os
import re
import subprocess
import sys

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

DEFAULT_ELF = "test_core.elf"
DEFAULT_SECTION = "._itcram_hot"
DEFAULT_BASELINE = "tests/asm_baseline.json"

OBJDUMP = os.environ.get("OBJDUMP", "arm-none-eabi-objdump")

_SYMBOL_RE = re.compile(r"^([0-9a-f]+) <(.+)>:$")
# An instruction line: "  addr:\t<encoding>\t<mnemonic> ...". Encoding-only
# continuation lines (long literals) carry no tab-separated mnemonic.
_INSN_RE = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f ]+)\t\s*(\S+)")


def collect(elf, section):
    """Return {symbol: {"insns": n, "bytes": n, "addr": a}} for one section."""
    try:
        out = subprocess.run(
            [OBJDUMP, "-d", "--section=" + section, elf],
            capture_output=True, text=True, check=True,
        ).stdout
    except FileNotFoundError:
        sys.exit(f"{OBJDUMP} not found; set OBJDUMP or add the toolchain to PATH")
    except subprocess.CalledProcessError as exc:
        sys.exit(f"objdump failed on {elf}: {exc.stderr.strip()}")

    syms, cur = {}, None
    for line in out.splitlines():
        m = _SYMBOL_RE.match(line)
        if m:
            cur = m.group(2)
            syms[cur] = {"insns": 0, "bytes": 0, "addr": int(m.group(1), 16)}
            continue
        if cur is None:
            continue
        m = _INSN_RE.match(line)
        if m:
            syms[cur]["insns"] += 1
            # Encoding column is hex byte pairs; 2 hex chars per byte.
            syms[cur]["bytes"] += len(m.group(2).replace(" ", "")) // 2
    if not syms:
        sys.exit(f"no symbols found in {section} of {elf} -- wrong section name?")
    return syms


def totals(syms):
    return (
        sum(s["insns"] for s in syms.values()),
        sum(s["bytes"] for s in syms.values()),
        len(syms),
    )


def cmd_compare(syms, baseline_path):
    if not os.path.exists(baseline_path):
        sys.exit(f"no baseline at {baseline_path}; run with --update to create it")
    with open(baseline_path) as fh:
        base = json.load(fh)["symbols"]

    grown, shrunk, added, removed = [], [], [], []
    for name, cur in sorted(syms.items()):
        if name not in base:
            added.append((name, cur["insns"]))
        elif cur["insns"] > base[name]["insns"]:
            grown.append((name, base[name]["insns"], cur["insns"]))
        elif cur["insns"] < base[name]["insns"]:
            shrunk.append((name, base[name]["insns"], cur["insns"]))
    for name in sorted(base):
        if name not in syms:
            removed.append((name, base[name]["insns"]))

    ci, cb, cn = totals(syms)
    bi = sum(s["insns"] for s in base.values())
    bb = sum(s["bytes"] for s in base.values())

    print(f"symbols   {len(base):>6} -> {cn:>6}")
    print(f"insns     {bi:>6} -> {ci:>6}  ({ci - bi:+d})")
    print(f"bytes     {bb:>6} -> {cb:>6}  ({cb - bb:+d})")

    for label, rows in (("GREW", grown), ("shrunk", shrunk)):
        if rows:
            print(f"\n{label}:")
            for name, before, after in rows:
                print(f"  {name:<44} {before:>4} -> {after:<4} ({after - before:+d})")
    for label, rows in (("added", added), ("removed", removed)):
        if rows:
            print(f"\n{label}:")
            for name, n in rows:
                print(f"  {name:<44} {n:>4}")

    # Growth is not automatically wrong -- it needs a stated reason. Exit
    # non-zero so it cannot pass unnoticed in a script.
    return 1 if grown else 0


def cmd_update(syms, baseline_path, elf, section):
    i, b, n = totals(syms)
    os.makedirs(os.path.dirname(baseline_path) or ".", exist_ok=True)
    with open(baseline_path, "w") as fh:
        json.dump(
            {
                "_comment": "Generated by scripts/common/asm_metrics.py --update. "
                            "Commit this. A handler growing should be reviewed.",
                "elf": elf,
                "section": section,
                "totals": {"symbols": n, "insns": i, "bytes": b},
                "symbols": {k: {"insns": v["insns"], "bytes": v["bytes"]}
                            for k, v in sorted(syms.items())},
            },
            fh, indent=1, sort_keys=True,
        )
        fh.write("\n")
    print(f"wrote {baseline_path}: {n} symbols, {i} insns, {b} bytes")
    return 0


def cmd_top(syms, n):
    print(f"{'symbol':<48}{'insns':>7}{'bytes':>7}")
    for name, s in sorted(syms.items(), key=lambda kv: -kv[1]["insns"])[:n]:
        print(f"{name:<48}{s['insns']:>7}{s['bytes']:>7}")
    i, b, c = totals(syms)
    print(f"{'-- total':<48}{i:>7}{b:>7}  ({c} symbols)")
    return 0


def cmd_cycles(args):
    """Tier 3 (hardware): real cycle counts. Only hardware is authoritative -- gwemu's
    wall-clock is ~400x off and it does not model the pipeline at all."""
    from scripts.common.runner import build_and_create_runner
    from scripts.common.elf import get_symbols

    runner = build_and_create_runner(args)
    runner.start()
    try:
        backend = runner.backend
        syms = get_symbols(args.elf)
        for name in ("run_benchmark_flag", "benchmark_cycles"):
            if name not in syms:
                sys.exit(f"symbol {name} not found in {args.elf}")
        backend.write_uint32(syms["run_benchmark_flag"], 1)

        import time
        for _ in range(int(args.timeout * 10)):
            if backend.read_uint32(syms["run_benchmark_flag"]) == 0:
                break
            time.sleep(0.1)
        else:
            sys.exit("benchmark did not complete")

        cycles = backend.read_uint32(syms["benchmark_cycles"])
        # Instruction count is reported BY the firmware, not assumed here. The
        # old harness hardcoded 1,000,000 while the core actually executed ~20,
        # and printed the quotient as MIPS.
        insns = backend.read_uint32(syms["benchmark_insns"])
        print(f"target       {args.target}")
        print(f"cycles       {cycles}")
        print(f"insns        {insns}")
        if insns:
            print(f"cyc/insn     {cycles / insns:.2f}")
        else:
            print("cyc/insn     n/a -- guest executed nothing")
        if args.target == "hardware":
            print(f"MIPS@280MHz  {280e6 / (cycles / insns) / 1e6:.2f}"
                  if insns and cycles else "MIPS@280MHz  n/a")
        else:
            print("NOTE: gwemu cycle counts do not transfer and are not even "
                  "reproducible (~14% run-to-run spread measured; hardware is "
                  "stable to ~1e-7). Trust the instruction count, not cyc/insn.")
        return 0
    finally:
        runner.stop()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--elf", default=DEFAULT_ELF, help="ELF to analyze")
    p.add_argument("--section", default=DEFAULT_SECTION, help="section to measure")
    p.add_argument("--baseline", default=DEFAULT_BASELINE, help="baseline JSON path")
    p.add_argument("--update", action="store_true", help="rewrite the baseline")
    p.add_argument("--top", type=int, metavar="N", help="list the N largest handlers")
    p.add_argument("--cycles", action="store_true",
                   help="run the on-target benchmark and report DWT cycles")
    p.add_argument("--timeout", type=float, default=30.0,
                   help="seconds to wait for --cycles")
    # Only meaningful with --cycles, but harmless otherwise.
    p.add_argument("--target", choices=["qemu", "hardware"], default="qemu")
    p.add_argument("--clean", action="store_true", help="clean before building")
    p.add_argument("--gui", action="store_true", help="GUI display (qemu only)")
    p.add_argument("--physical-sdcard", default=None)
    p.add_argument("--icount", default=None, help="QEMU -icount shift (gwemu only)")
    args = p.parse_args()

    if args.cycles:
        return cmd_cycles(args)

    syms = collect(args.elf, args.section)
    if args.update:
        return cmd_update(syms, args.baseline, args.elf, args.section)
    if args.top:
        return cmd_top(syms, args.top)
    return cmd_compare(syms, args.baseline)


if __name__ == "__main__":
    sys.exit(main())
