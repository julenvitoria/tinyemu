#!/usr/bin/env python3
"""
The negative controls for the DONATION POLICY arms of tests/test_paging.py.
One clean build each, with one half of the loop compiled out, and the arms that
grade that half MUST FAIL.

    python3 scripts/run_paging_controls.py [--target gwemu]

WHY THIS EXISTS AS ITS OWN SCRIPT. The policy's two claims -- "frames are
borrowed while they are idle" and "they go back before the guest evicts" -- are
both claims about something NOT happening (no eviction, no idle pool). A green
run on its own cannot distinguish them from a policy that never fires at all,
and this tree has shipped that shape of check before (see
scripts/run_planar_controls.py's preamble). So each half is compiled out and
the check is required to notice:

  NOSHRINK  -DX86_PG_POLICY_NO_SHRINK=1  x86_pager_policy_tick() never returns
                                         SHRINK, so the frames stay lent while
                                         the working set grows into them. The
                                         evictions the shipping arm proves
                                         ABSENT must appear -- measured here as
                                         17 of them, with the pagefile written
                                         every time.

  NOGROW    -DX86_PG_POLICY_NO_GROW=1    the tick never gives permission to
                                         lend, so nothing is ever borrowed and
                                         the card stays whatever the arena
                                         alone can place. Six arms fail,
                                         including the pool shrinking at all.

Flags are not dependencies, so every arm builds from CLEAN. That is most of the
runtime and there is no way around it.
"""
import argparse
import os
import subprocess
import sys

# (name, flag, the arms it must break)
ARMS = [
    ("NOSHRINK", "-DX86_PG_POLICY_NO_SHRINK=1",
     "the shrink half: evictions must appear"),
    ("NOGROW", "-DX86_PG_POLICY_NO_GROW=1",
     "the grow half: nothing may be borrowed"),
]


def run_arm(flag, target):
    env = dict(os.environ, X86_VIDEO_NEGCTL=flag)
    cmd = [sys.executable, "tests/test_paging.py", "--clean", "--target", target]
    return subprocess.run(cmd, env=env).returncode == 0


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--target", default="gwemu")
    args = p.parse_args()

    fail = 0
    for name, flag, what in ARMS:
        print(f"---- {name}: {flag}  ({what})")
        if run_arm(flag, args.target):
            print(f"CONTROL FAIL -- test_paging.py PASSED with {flag}. The "
                  f"arms it grades cannot fail, so they prove nothing.")
            fail = 1
        else:
            print(f"ok: {name} caught")
    print("FAIL" if fail else "PASS: every control was caught")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
