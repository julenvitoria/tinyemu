#!/usr/bin/env python3
"""
The NEGATIVE CONTROLS for the protected-mode descriptor walk.

    python3 scripts/run_pmode_controls.py [--target gwemu] [--only 6]

Each arm rebuilds the bare-metal harness with ONE check in x86_pmode.c compiled
out (X86_PMODE_NEGCTL=n) and requires `tests/test_pmode.py` to FAIL. An arm
that still passes is reported loudly: the check it names is not testing what it
claims, and the suite is green over nothing.

WHY THIS IS NOT OPTIONAL FOR THIS FEATURE IN PARTICULAR. The descriptor walk is
almost entirely checks -- there is very little "behaviour" to get wrong that is
not a rule to enforce. And the obvious reference to copy, `references/tiny386`,
DOES NOT IMPLEMENT THEM: its set_seg() carries `// TODO: various permission
checks` (i386.c:1097) and has no DPL/RPL/CPL validation on a data segment load
at all. An implementation modelled on it would pass every privilege test anyone
wrote against it, because the test and the code would share the same hole. The
only thing that separates "the check works" from "the check is absent" is
watching the test go red when the check is removed.

Two of these arms found real defects on their first run rather than merely
confirming the design, which is the argument for the whole exercise:

  * NEGCTL 8 (granularity) initially SURVIVED, because the arm that graded G
    used a descriptor whose limit was non-zero -- so an ignored G still gave a
    different number from the null-selector case and the check looked live. The
    test now uses a limit FIELD of zero with G set, where an ignored G yields 0
    and a working one yields 0xFFF.
  * NEGCTL 4 (the SS privilege rule) is graded only because the suite loads SS
    with a selector whose RPL and DPL are correct AND one where they are not.
    A single arm cannot distinguish "rejected for privilege" from "rejected for
    type".

Flags are not dependencies, so each arm builds CLEAN. That is nearly all of the
runtime and there is no way around it.
"""
import argparse
import os
import subprocess
import sys

# (n, name, what the check is, and what breaks in the guest if it is missing)
ARMS = [
    (1,  "TABLE BOUND",
     "read_desc's `off + 7 > limit`. Without it any selector indexes past the "
     "GDT into whatever follows the table and is decoded as a descriptor."),
    (2,  "NULL SS",
     "a null selector loaded into SS. Without it the stack segment silently "
     "gets base 0 limit 0 and the next push writes to linear 0."),
    (3,  "SYSTEM DESCRIPTOR",
     "the S bit. Without it an LDT, TSS or gate descriptor loads into a data "
     "segment register and its base/limit fields are read out of a structure "
     "that does not have them."),
    (4,  "SS PRIVILEGE",
     "RPL == DPL == CPL for SS. tiny386 does not have this."),
    (5,  "SS TYPE",
     "SS must be WRITABLE DATA. Without it a code or read-only selector "
     "becomes the stack."),
    (6,  "DATA DPL",
     "DPL >= max(CPL, RPL) for DS/ES/FS/GS. THE RULE TINY386 OMITS ENTIRELY -- "
     "without it a ring-3 selector reaches ring-0 data."),
    (7,  "PRESENT",
     "the P bit, and the #NP/#SS vector split. Without it a swapped-out "
     "segment loads as though it were resident."),
    (8,  "GRANULARITY",
     "the G bit's 4 KB shift. Without it every G=1 descriptor's limit is 4096 "
     "times too small."),
    (9,  "CS TYPE",
     "CS must be a code segment. Without it a far transfer to a DATA selector "
     "executes whatever the data segment holds."),
    (10, "LLDT TYPE",
     "LLDT's descriptor must be type 2. Without it the LDT base can be "
     "pointed at any segment, including a code segment."),
]


def run_arm(n, target, quiet):
    env = dict(os.environ, X86_PMODE_NEGCTL=str(n))
    cmd = [sys.executable, "tests/test_pmode.py", "--clean", "--target", target]
    kw = {"stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL} if quiet else {}
    return subprocess.run(cmd, env=env, **kw).returncode == 0


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--target", default="gwemu")
    p.add_argument("--only", type=int, default=None,
                   help="run a single control by number")
    p.add_argument("--verbose", action="store_true",
                   help="show each arm's full test output")
    args = p.parse_args()

    arms = [a for a in ARMS if args.only is None or a[0] == args.only]
    if not arms:
        print(f"no such control: {args.only}")
        return 2

    # The POSITIVE control, and it is not a formality: if the suite does not
    # pass with everything ON, every arm below "fails" for a reason that has
    # nothing to do with the check it names.
    print("---- BASELINE: every check ON, the suite must PASS")
    if not run_arm(0, args.target, not args.verbose):
        print("BASELINE FAILS. Nothing below means anything until it passes.")
        return 1
    print("  baseline: pass")

    fail = 0
    for n, name, what in arms:
        print(f"---- NEGCTL {n} {name}: {what}")
        if run_arm(n, args.target, not args.verbose):
            print(f"NEGCTL {n} ({name}) SURVIVED -- tests/test_pmode.py passes "
                  f"with this check compiled OUT, so nothing in the suite is "
                  f"actually testing it.")
            fail = 1
        else:
            print(f"  negctl {n}: caught")

    if fail:
        print("PMODE CONTROLS FAIL")
        return 1
    print(f"PMODE CONTROLS PASS ({len(arms)}/{len(arms)} caught, baseline green)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
