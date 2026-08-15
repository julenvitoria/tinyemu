#!/usr/bin/env python3
"""
Build the Retro-Go SD firmware for testing our core against.

gwemu and the device boot the SAME image, so this is one operation regardless of
where it ends up running. Defaults follow retro-go's documented release build
with one deliberate deviation -- INTFLASH_BANK=1 rather than 2, because the
device boots straight into bank 1 and a development loop should not have to deal
with dual-boot bootloader patching. Use --bank 2 for a productive install
alongside stock firmware.

Examples:
    python3 scripts/build_retrogo.py                    # bank 1, 64MB extflash
    python3 scripts/build_retrogo.py --clean            # force a full rebuild
    python3 scripts/build_retrogo.py --bank 2           # productive layout
    python3 scripts/build_retrogo.py --extflash-part-mb 64 --extflash-offset-mb 4
    python3 scripts/build_retrogo.py --stage build/retrogo   # + copy media out

Build flags are NOT dependencies in retro-go's build system: changing one
recompiles nothing. If you change any parameter, pass --clean or you will boot
stale objects and test a configuration you did not build.
"""

import argparse
import os
import sys

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.retrogo_build import (            # noqa: E402
    DEFAULT_FIRMWARE_PATH, DEV_EXTFLASH_OFFSET_MB, DEV_EXTFLASH_PART_MB,
    DEV_INTFLASH_BANK, VALID_EXTFLASH_PART_MB, build, stage,
)


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--path", default=DEFAULT_FIRMWARE_PATH,
                   help=f"firmware checkout (default: {DEFAULT_FIRMWARE_PATH})")
    p.add_argument("--clean", action="store_true",
                   help="make clean first; REQUIRED after changing any build "
                        "parameter, since flags are not dependencies")
    p.add_argument("--bank", type=int, choices=(1, 2), default=DEV_INTFLASH_BANK,
                   help=f"INTFLASH_BANK (default: {DEV_INTFLASH_BANK}, boots "
                        f"directly; 2 is the productive dual-boot layout)")
    # Give the PART and the OFFSET; EXTFLASH_SIZE_MB is derived as part-offset.
    # retro-go's EXTFLASH_SIZE_MB is the usable region AFTER the offset, so a
    # 4MB offset on a 64MB part is SIZE_MB=60, not 64. Never hand-subtract.
    p.add_argument("--extflash-part-mb", type=int, default=DEV_EXTFLASH_PART_MB,
                   choices=VALID_EXTFLASH_PART_MB,
                   help=f"physical external flash size in MB "
                        f"(default: {DEV_EXTFLASH_PART_MB})")
    p.add_argument("--extflash-offset-mb", type=int,
                   default=DEV_EXTFLASH_OFFSET_MB,
                   help=f"MB reserved at the bottom of extflash, e.g. 4 to keep "
                        f"a stock Zelda ROM (default: {DEV_EXTFLASH_OFFSET_MB})")
    p.add_argument("--target", default="gwemu_release",
                   help="make target; gwemu_release also produces the emulator "
                        "media using the SAME parameters (default)")
    p.add_argument("--jobs", type=int, default=None, help="parallel make jobs")
    p.add_argument("--docker", action="store_true",
                   help="build inside retro-go's docker image")
    p.add_argument("--define", action="append", metavar="VAR=VALUE", default=[],
                   help="extra make variable; repeatable")
    p.add_argument("--dry-run", action="store_true",
                   help="print the make command instead of running it")
    p.add_argument("--stage", metavar="DIR", default=None,
                   help="copy the built media into DIR (banks padded to 256KB)")
    p.add_argument("--sd-image", default=None,
                   help="with --stage, use this SD image instead of the "
                        "firmware's own (e.g. ours carrying /cores/dos.bin)")
    args = p.parse_args()

    extra = {}
    for d in args.define:
        if "=" not in d:
            sys.exit(f"--define needs VAR=VALUE, got {d!r}")
        k, v = d.split("=", 1)
        extra[k] = v

    try:
        fb = build(path=args.path, clean=args.clean, target=args.target,
                   intflash_bank=args.bank,
                   extflash_part_mb=args.extflash_part_mb,
                   extflash_offset_mb=args.extflash_offset_mb,
                   jobs=args.jobs, docker=args.docker, extra=extra,
                   dry_run=args.dry_run)
    except (FileNotFoundError, ValueError, RuntimeError) as exc:
        sys.exit(f"build_retrogo: {exc}")

    if args.dry_run:
        return 0

    if args.stage:
        stage(fb, args.stage, sd_image=args.sd_image)

    print("\nparameters used:")
    for k, v in sorted(fb.params.items()):
        print(f"    {k}={v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
