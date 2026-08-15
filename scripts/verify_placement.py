#!/usr/bin/env python3
"""
Verify what is loaded on the target matches what we built.

Checks that symbols landed in the memory they were designed for, and that a
section's bytes on the target equal the bytes in the ELF. Both have caught real
bugs: the hot loop linked into flash instead of ITCM, and a core segment whose
partial load surfaced only as an UNDEFINSTR HardFault.

    # bare-metal harness: is the hot loop in ITCM and the state in DTCM?
    python3 scripts/verify_placement.py

    # retro-go core: did the ITCM segment actually load?
    python3 scripts/verify_placement.py --retrogo --wait-magic \\
        --section .core_itcm
"""

import argparse
import os
import sys

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.elf import get_symbols                        # noqa: E402
from scripts.common.probe import wait_for_value                   # noqa: E402
from scripts.common.target import Image, add_target_args, open_target  # noqa: E402
from scripts.common.verify import check_regions, compare_section  # noqa: E402

FW_DIR = "references/game-and-watch-retro-go-sd/build"
CORE_MAGIC = 0x54494E59

# What each build's key symbols must land in.
BARE_METAL = {"x86_cpu_interp_asm": "ITCM",
              "cpu_state_dtcm": "DTCM",
              "guest_page_table": "DTCM"}
CORE = {"x86_cpu_interp_asm": "ITCM", "app_main": "AXI"}


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_target_args(p)
    p.add_argument("--retrogo", action="store_true",
                   help="verify the packed core inside retro-go")
    p.add_argument("--firmware-dir", default=FW_DIR)
    p.add_argument("--sd", default="sdcard_retrogo.img")
    p.add_argument("--elf", default=None)
    p.add_argument("--section", action="append", default=[],
                   help="also byte-compare this ELF section; repeatable")
    p.add_argument("--wait-magic", action="store_true",
                   help="wait for the core to start before checking")
    p.add_argument("--static-only", action="store_true",
                   help="check the ELF only; do not boot a target")
    args = p.parse_args()

    if args.retrogo:
        fw = args.firmware_dir
        elf = args.elf or "build/core/dos_core.elf"
        expect = CORE
        image = Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                      bank2=os.path.join(fw, "qemu_bank2.bin"),
                      extflash=os.path.join(fw, "extflash.bin"),
                      sdcard=args.sd, elf=elf, intflash_bank=1)
        if args.timeline is None:
            args.timeline = "timelines/start-preselected-game.tl"
    else:
        elf = args.elf or "test_core.elf"
        expect = BARE_METAL
        image = Image(bank1="qemu_bank1.bin", bank2="qemu_bank2.bin",
                      extflash="extflash.bin", sdcard="sdcard.img",
                      elf=elf, intflash_bank=1)

    if not os.path.exists(elf):
        sys.exit(f"missing {elf}")

    syms = get_symbols(elf)
    print(f"placement in {elf}:")
    ok = check_regions(syms, expect)

    if args.static_only or not args.section:
        return 0 if ok else 1

    target = open_target(args, image)
    target.start()
    try:
        if args.wait_magic and "core_magic" in syms:
            print("waiting for the core to start...")
            if not wait_for_value(target.backend, syms["core_magic"],
                                  CORE_MAGIC, 30.0):
                print("core never started")
                return 1
        target.backend.halt()
        print("section contents on target:")
        for sec in args.section:
            ok = compare_section(target.backend, elf, sec) and ok
        target.backend.resume()
    finally:
        target.stop()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
