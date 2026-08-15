#!/usr/bin/env python3
"""Tier-1 (and below) test for the PC-speaker wave generator.

This one does not need a target at all: ``x86_audio_wave.h`` depends on nothing
but ``<stdint.h>``, which is the entire reason it is a separate header. So the
divisor -> frequency arithmetic -- the part of the audio path most likely to be
silently wrong, and the part no screenshot and no gwemu run can judge -- is
compiled and run NATIVELY here, in under a second, with no emulator in the way.

Everything the C file asserts is in ``tests/host_audio.c``; this is the runner.
Build scratch goes under ``build/run/`` like every other test, never /tmp.
"""

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "tests", "host_audio.c")
OUTDIR = os.path.join(ROOT, "build", "run", "host_audio")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"),
                    help="host compiler (default: $CC or cc)")
    ap.add_argument("--keep-temp", action="store_true",
                    help="keep the build directory for analysis")
    # Accepted and ignored, so this can be run alongside the target-driven
    # tests with the same flags rather than being a special case in scripts.
    ap.add_argument("--target", default="host")
    args, _unknown = ap.parse_known_args()

    os.makedirs(OUTDIR, exist_ok=True)
    exe = os.path.join(OUTDIR, "host_audio")
    try:
        cmd = [args.cc, "-O2", "-Wall", "-Wextra", "-Werror",
               "-I", ROOT, "-o", exe, SRC, "-lm"]
        print("$ " + " ".join(cmd))
        subprocess.run(cmd, check=True)
        rc = subprocess.run([exe]).returncode
    except subprocess.CalledProcessError as e:
        print(f"build failed: {e}")
        return 1
    finally:
        if not args.keep_temp:
            shutil.rmtree(OUTDIR, ignore_errors=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
