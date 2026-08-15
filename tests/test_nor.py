#!/usr/bin/env python3
"""
Tier-1 check of NOR cold-page backing and its copy-on-write pull-back.

Everything here is driven by GUEST instructions, not by host C calls: a read
served out of NOR and a write that pulls a page back into AXI are properties of
the trap stubs in cpu_exec.S, and only executing x86 code can exercise those.

The "NOR" is the extflash window at 0x90000000 itself, written with plain
stores -- which works under gwemu, where that region is real backing memory. No
flash programming, no OCTOSPI, no SD card and no firmware are involved, but the
ADDRESS is genuine, so bit 31 is set and the fold check traps for exactly the
reason it will on silicon. A buffer in .bss would have bit 31 clear and would
never trap, so it would prove nothing.

What each slot of nor_r[] is is documented in tests/main.c next to the code
that fills it.
"""
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

PAGES = 8
PAGE = 4096


def pat(off):
    """nor_pattern_byte() in tests/main.c, recomputed here rather than read back
    off the target, so a wrong pattern cannot agree with itself."""
    return (off * 7 + (off >> 8) * 31 + 1) & 0xFF


def main(args):
    runner = build_and_create_runner(args)
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols(getattr(args, "elf", None) or "test_core.elf")
        for n in ("run_nor_flag", "nor_r"):
            if n not in syms:
                print(f"missing symbol {n}")
                return False

        time.sleep(0.5)
        b.write_uint32(syms["run_nor_flag"], 1)
        for _ in range(400):
            if b.read_uint32(syms["run_nor_flag"]) == 0:
                break
            time.sleep(0.05)
        else:
            print("FAIL: target never ran the NOR test")
            return False

        r = [b.read_uint32(syms["nor_r"] + 4 * i) for i in range(40)]

        checks = [
            # A snapshot whose readback disagrees must be refused OUTRIGHT --
            # this is the invariant that makes a half-applied remap impossible.
            ("mismatched snapshot refused",        r[0], 0xFFFFFFFF),
            ("  ... and says why (err 7)",         r[1], 7),
            ("  ... guest still not attached",     r[2], 0),
            ("  ... fold entry untouched",         r[3], 1),

            ("attach accepted",                    r[4], 0),
            ("attached",                           r[5], 1),
            ("pages snapshotted",                  r[6], PAGES),
            ("all pages start in NOR",             r[7], PAGES),
            ("AXI reclaimed at attach",            r[8], PAGES * PAGE),

            # exit_reason 6 = HLT: the program ran to the end rather than
            # dying on a memory fault, which is what it did before the stubs.
            ("guest ran to HLT",                   r[9], 6),
            ("slow-path reads",                    r[10], 1),
            ("slow-path writes",                   r[11], 1),
            ("COW pull-backs",                     r[12], 1),
            ("COW failures",                       r[13], 0),
            ("pages left in NOR",                  r[14], PAGES - 1),
            ("pages pulled into AXI",              r[15], 1),
            ("AXI still reclaimed",                r[16], (PAGES - 1) * PAGE),
            ("no unmapped traps",                  r[17], 0),

            # The byte the guest read really came out of the NOR image, and
            # the expected value is computed HERE, not read back off the target.
            ("target pattern agrees",              r[18], pat(0x3000)),
            ("read served from NOR",               r[19], pat(0x3000)),
            # And the write it did afterwards did NOT reach NOR.
            ("NOR copy unmodified at +0x5000",     r[20], pat(0x5000)),
            ("NOR copy unmodified at +0x5001",     r[21], pat(0x5001)),
            # Which is only meaningful if the page really moved to AXI.
            ("page 5 now on the fast path",        r[23], 0),

            # ---- NOR-TIER EVICTION: the compare against the snapshot -------
            # The pager's half of a NOR page's life. Nothing else in the tree
            # exercises it, and it is the branch that decides whether a cold
            # code page costs an SD write every time it is evicted.
            ("two pages filled from NOR",          r[24], 2),
            ("  ... byte-exact with the snapshot", r[25], 1),
            ("  ... and so is the second",         r[26], 1),
            ("unmodified victim: one eviction",    r[27], 1),
            ("  ... DROPPED, it still matches",    r[28], 1),
            ("  ... no pagefile write at all",     r[29], 0),

            # The negative control. One byte differs from the snapshot, so
            # neither page can be dropped -- and no pagefile is wired, so there
            # is nowhere to put it. A MEM_FAULT the guest can see beats a page
            # it cannot: MEASURED on KEEN4 before this rule, 38 evictions and
            # 38 io_errors, i.e. 38 pages of guest memory gone.
            ("modified victim: fault refused",     r[30], 1),
            ("  ... nothing was evicted",          r[31], 0),
            ("  ... nothing was dropped",          r[32], 0),
            ("  ... nothing was LOST",             r[33], 0),
            ("  ... exhaustion counted instead",   r[34], 1),
            ("  ... and the bytes are still there", r[35], 1),

            ("frames handed back",                 r[36], 0),
        ]

        # r[11] counts writes; the SECOND store went to the same page and must
        # not have trapped at all, so one write and one pull-back is the whole
        # point -- a 2 here would mean the fold entry never moved.
        bad = 0
        for name, got, want in checks:
            ok = got == want
            bad += not ok
            print(f"  {'PASS' if ok else 'FAIL'}: {name}: {got}"
                  + ("" if ok else f" (expected {want})"))
        if bad:
            print(f"FAIL: {bad} of {len(checks)} checks")
            return False
        print(f"PASS: {len(checks)} checks")
        return True
    finally:
        runner.stop()


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    args = p.parse_args()
    sys.exit(0 if main(args) else 1)
