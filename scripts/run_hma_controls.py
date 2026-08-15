#!/usr/bin/env python3
"""
The NEGATIVE CONTROLS for the HMA window and the A20 gating that owns it.

    python3 scripts/run_hma_controls.py [--target gwemu] [--only 1]

Each arm rebuilds the bare-metal harness with ONE property broken
(X86_HMA_NEGCTL=n) and requires `tests/test_paging.py` to FAIL. An arm that
still passes says the suite is green over nothing.

WHY THESE TWO, AND NOT AN ARM PER FUNCTION. Backing the HMA is a small change
with exactly two ways to be silently wrong, and neither of them faults:

  1  THE OWNERSHIP RULE. The sixteen pages at 1 MB are the pager's while A20 is
     OPEN and the WRAP's while it is masked. Leave the window registered across
     a mask and a guest that wrote the HMA, masked the gate and read low memory
     gets its HMA data instead of the alias -- wrong bytes, no counter, no
     fault. This control leaves the window registered and refolds the frames,
     which is precisely the bug.

  2  THE SEAM, INSIDE THE HMA. Sixteen pages handed out in FAULT order are
     sixteen unrelated frames, so a 16-bit access straddling a boundary inside
     the window is docs/10's defect at one address in 2,048. This control makes
     x86_pager_slack() refuse those pages, so the straddle cannot be served --
     and the arm must notice, both in the value the guest reads back and in
     x86_seam_overflow moving.

Flags are not dependencies, so each arm builds CLEAN. That is nearly all of the
runtime.
"""
import argparse
import os
import subprocess
import sys

ARMS = [
    (1, "A20 GATING",
     "the HMA window is left registered while the gate is MASKED. The guest "
     "then reads its own HMA data where the low-memory ALIAS belongs -- the "
     "silent corruption the whole ownership rule exists to prevent."),
    (2, "HMA SEAM",
     "x86_pager_slack() refuses the HMA's pages, so a 16-bit access straddling "
     "a page boundary inside the window cannot be served and falls back to the "
     "old, wrong behaviour."),
]


def run_arm(n, target, quiet):
    env = dict(os.environ, X86_HMA_NEGCTL=str(n))
    cmd = [sys.executable, "tests/test_paging.py", "--clean", "--target", target]
    kw = {"stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL} if quiet else {}
    return subprocess.run(cmd, env=env, **kw).returncode == 0


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--target", default="gwemu")
    p.add_argument("--only", type=int, default=None)
    p.add_argument("--verbose", action="store_true")
    args = p.parse_args()

    arms = [a for a in ARMS if args.only is None or a[0] == args.only]
    if not arms:
        print(f"no such control: {args.only}")
        return 2

    print("---- BASELINE: nothing broken, the suite must PASS")
    if not run_arm(0, args.target, not args.verbose):
        print("BASELINE FAILS. Nothing below means anything until it passes.")
        return 1
    print("  baseline: pass")

    fail = 0
    for n, name, what in arms:
        print(f"---- NEGCTL {n} {name}: {what}")
        if run_arm(n, args.target, not args.verbose):
            print(f"NEGCTL {n} ({name}) SURVIVED -- tests/test_paging.py passes "
                  f"with this property broken, so nothing in the suite tests it.")
            fail = 1
        else:
            print(f"  negctl {n}: caught")

    if fail:
        print("HMA CONTROLS FAIL")
        return 1
    print(f"HMA CONTROLS PASS ({len(arms)}/{len(arms)} caught, baseline green)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
