#!/usr/bin/env python3
"""Tier-1 (and below) test for the PER-TITLE SETTINGS format and the .cfg parser.

This one does not need a target at all. ``x86_settings.c`` and ``x86_cfg.c``
deliberately do no I/O and depend on nothing but ``string.h`` and one ``printf``
-- which is the entire reason the prior port's two-layer split was worth keeping
-- so the whole of the format is compiled and run NATIVELY here, in under a
second, with no emulator in the way. src/main.c owns the other half (work out
the path, open, read, close) and is graded at tier 2.

Everything asserted lives in ``tests/host_settings.c``; this is the runner.
Build scratch goes under ``build/run/`` like every other test, never /tmp.

    python3 tests/test_settings.py              # the format, ~400 checks
    python3 tests/test_settings.py --negative   # EVERY control, SEEN TO FAIL

NEGATIVE CONTROLS. ``--negative`` rebuilds once per control with one of the
guards compiled out and REQUIRES the run to fail. A test that only ever sees a
guard working cannot tell a guard from a coincidence: if a control passes, the
check it disables was never checking anything and the arm is reported as a
FAILURE of this script, not as a pass.
"""

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRCS = [os.path.join(ROOT, "tests", "host_settings.c"),
        os.path.join(ROOT, "x86_settings.c"),
        os.path.join(ROOT, "x86_cfg.c")]
OUTDIR = os.path.join(ROOT, "build", "run", "host_settings")

# One entry per guard this format has, and what disabling it must break. The
# text is what gets printed, so it has to name the check rather than the macro:
# "-DX86_SETTINGS_CTL_NOCRC=1 passed" tells a reader nothing.
CONTROLS = [
    ("X86_SETTINGS_CTL_NOCRC",
     "the .dosset CRC32 is not checked, so a corrupt file is trusted"),
    ("X86_SETTINGS_CTL_NORANGE",
     "a pair value out of range is stored instead of keeping its default"),
    ("X86_SETTINGS_CTL_NOVERSION",
     "a file from an unknown format version is parsed anyway"),
    ("X86_SETTINGS_CTL_TRUNCSTR",
     "an over-long entry/args is truncated instead of refused"),
    ("X86_CFG_CTL_NOBINARY",
     "a .cfg that is actually a binary is scanned line by line"),
    ("X86_CFG_CTL_NOTOOBIG",
     "a .cfg past the size cap is half-applied instead of refused"),
]


def build_and_run(cc, exe, defines, quiet=False):
    """Compile the harness with `defines` and return its exit code."""
    cmd = [cc, "-O2", "-Wall", "-Wextra", "-Werror", "-std=c99",
           "-I", ROOT, "-o", exe] + [f"-D{d}=1" for d in defines] + SRCS
    if not quiet:
        print("$ " + " ".join(cmd))
    subprocess.run(cmd, check=True)
    # The harness prints its own FAIL lines; on a control arm they are the
    # expected output, so they are captured rather than shown.
    proc = subprocess.run([exe], capture_output=quiet, text=True)
    return proc.returncode


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"),
                    help="host compiler (default: $CC or cc)")
    ap.add_argument("--negative", action="store_true",
                    help="run every negative control; each MUST fail")
    ap.add_argument("--keep-temp", action="store_true",
                    help="keep the build directory for analysis")
    # Accepted and ignored, so this can be run alongside the target-driven tests
    # with the same flags rather than being a special case in scripts.
    ap.add_argument("--target", default="host")
    args, _unknown = ap.parse_known_args()

    os.makedirs(OUTDIR, exist_ok=True)
    exe = os.path.join(OUTDIR, "host_settings")
    rc = 0
    try:
        if not args.negative:
            rc = build_and_run(args.cc, exe, [])
            print("PASS" if rc == 0 else "FAIL")
            return rc

        print(f"NEGATIVE CONTROLS -- {len(CONTROLS)} guards, each must FAIL\n")
        bad = 0
        for macro, what in CONTROLS:
            got = build_and_run(args.cc, exe, [macro], quiet=True)
            if got == 0:
                bad += 1
                print(f"  NOT CAUGHT  {macro}")
                print(f"              with it off, {what} -- and the harness"
                      f" still passed, so nothing was testing it")
            else:
                print(f"  caught      {macro}")
                print(f"              ({what})")
        print(f"\n{len(CONTROLS) - bad}/{len(CONTROLS)} controls seen to fail")
        rc = 1 if bad else 0
    except subprocess.CalledProcessError as e:
        print(f"build failed: {e}")
        return 1
    finally:
        if not args.keep_temp:
            shutil.rmtree(OUTDIR, ignore_errors=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
