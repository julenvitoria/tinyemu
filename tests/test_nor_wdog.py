#!/usr/bin/env python3
"""
Tier-1 check that x86_nor_attach()'s readback KICKS THE WATCHDOG once per page.

Why this is a test and not a comment
------------------------------------
The guest-RAM -> NOR snapshot runs as a retro-go MENU callback, so the frame
loop -- the only thing in the shipping core that calls wdog_refresh() -- is not
running for its whole duration (measured at 1,595-1,614 ms under gwemu, where
OCTOSPI erase/program latency and SD busy state are BOTH unmodelled, so silicon
is strictly worse). Everything else on that path is kicked by somebody:
gw_flash.c's OSPI_Program() refreshes per 256-byte page and its comment names
the ~472 ms WWDG window outright, OSPI_Erase() is refreshed per command, and the
SPI SD driver refreshes inside its busy-waits.

x86_nor_attach()'s readback is the exception. It is 160 memcmps of 4 KB each
against the XIP window, it touches neither driver, and on silicon every byte is
an OCTOSPI read. Nothing would kick the watchdog across it.

What is asserted, and why the COUNT
-----------------------------------
The failure that matters is not a wrong number, it is a hook that is silently
never called -- so the count of heartbeats is the property, and it is asserted
on the REFUSED path as well as the accepted one. The refusal path is the slow
one: it walks the entire readback before it can discover the bytes disagree, so
a heartbeat that only ran on success would leave the longest stretch uncovered.

Not the extflash window, deliberately
-------------------------------------
tests/main.c's other NOR arm uses 0x90000000 so that bit 31 is set and the fold
check traps. That arm CANNOT RUN in this firmware -- nothing configures OCTOSPI
memory-mapped mode, so its opening store to 0x90000000 takes a precise bus fault
(CFSR BFARVALID|PRECISERR, BFAR 0x90000000) into Fault_Handler_C's spin, which
is why test_nor.py reports "target never ran the NOR test" instead of a failure.
That is a pre-existing defect in that arm's premise. This arm needs the readback
LOOP, not the trap behaviour, so its "NOR" is an ordinary aligned .bss buffer
and it is immune to that breakage.

Negative control
----------------
    make clean && make X86_NOR_WDOG_NEGCTL=1 test_core.elf
compiles the heartbeat out of the readback loop; every count below reads 0 and
this test must FAIL. Run it that way before believing a pass.
"""
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

PAGES = 4


def main(args):
    runner = build_and_create_runner(args)
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols(getattr(args, "elf", None) or "test_core.elf")
        for n in ("run_norwd_flag", "norwd_r"):
            if n not in syms:
                print(f"missing symbol {n}")
                return False

        time.sleep(0.5)
        b.write_uint32(syms["run_norwd_flag"], 1)
        for _ in range(200):
            if b.read_uint32(syms["run_norwd_flag"]) == 0:
                break
            time.sleep(0.05)
        else:
            print("FAIL: target never ran the NOR watchdog test")
            return False

        r = [b.read_uint32(syms["norwd_r"] + 4 * i) for i in range(12)]

        checks = [
            # 1. The REFUSED attach: the mismatch is planted in the LAST page,
            # so the readback runs end to end before it can refuse.
            ("mismatched snapshot refused",       r[0], 0xFFFFFFFF),
            ("  ... and says why (err 7)",        r[1], 7),
            ("  ... kicked once per page",        r[2], PAGES),
            ("  ... through the installed hook",  r[3], PAGES),
            ("  ... and nothing was moved",       r[4], 0),

            # 2. The ACCEPTED attach: same coverage on the success path.
            ("attach accepted",                   r[5], 0),
            ("  ... kicked once per page",        r[6], PAGES),
            ("  ... through the installed hook",  r[7], PAGES),
            ("  ... guest now on 'NOR'",          r[8], 1),

            # 3. A NULL hook is the tier-1 default: a no-op, NOT a branch to
            # address 0, and the counter still advances.
            ("attach accepted with no hook",      r[9], 0),
            ("  ... still counted",               r[10], PAGES),
            ("  ... and no hook was called",      r[11], 0),
        ]

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
