#!/usr/bin/env python3
"""
Compose the SD card the firmware boots from — image, mounted card, or device.

Same content, three destinations:

    # gwemu: build a fresh partitioned image
    python3 scripts/sdcard.py --image sdcard_retrogo.img

    # a card in a reader (bulk, e.g. repackaging DOS ROMs)
    python3 scripts/sdcard.py --mount /media/doug/RETROGO

    # the device's own card over the debug probe (incremental)
    python3 scripts/sdcard.py --probe

Builds `/CONFIG` automatically so the ROM is preselected and idle standby is
off — that is what removes launcher navigation from automated testing.

This replaces the mtools-in-Makefile recipe: make compiles, Python composes and
deploys. The Makefile's SD target now calls this.
"""

import argparse
import os
import sys

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.retrogo_config import build as build_config, rom_path  # noqa: E402
from scripts.common.sdcard import (                                        # noqa: E402
    HardwarePhysicalSDCardManager, HardwareProbeSDCardManager,
    QemuSDCardManager, compose, create_image,
)

DEFAULT_CONTENT = "references/game-and-watch-retro-go-sd/sd_content"
DEFAULT_ROM = "references/game-and-watch-retro-go-sd/roms/dos/ALLEYCAT.dsk"


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    dest = p.add_mutually_exclusive_group(required=True)
    dest.add_argument("--image", metavar="PATH",
                      help="create/populate an SD image for gwemu")
    dest.add_argument("--mount", metavar="DIR",
                      help="write to a mounted card")
    dest.add_argument("--probe", action="store_true",
                      help="write to the device over the debug probe")

    p.add_argument("--size-mb", type=int, default=128,
                   help="image size when creating one (default: 128)")
    p.add_argument("--keep", action="store_true",
                   help="populate an existing image instead of recreating it")
    p.add_argument("--core", default="dos.bin", help="packed core .bin")
    p.add_argument("--core-name", default="dos", help="-> /cores/<name>.bin")
    p.add_argument("--rom", action="append", default=None,
                   help="guest .dsk -> /roms/<dir>/. Repeatable: pass it once "
                        "per title to build a card with the whole library "
                        "(what an INTERACTIVE session wants). Defaults to one "
                        "title so an automated capture stays deterministic")
    p.add_argument("--all-roms", metavar="DIR",
                   help="add every .dsk in DIR (with .cfg/.dosmeta/.jpg "
                        "sidecars). Implies no single preselected title, so "
                        "use with --no-config")
    p.add_argument("--rom-dir", default="dos", help="-> /roms/<dir>/")
    p.add_argument("--content", default=DEFAULT_CONTENT,
                   help="firmware sd_content tree (fonts/lang/bios)")
    p.add_argument("--config-out", default="build/CONFIG")
    p.add_argument("--oc-level", type=int, default=0, choices=[0, 1, 2, 3],
                   help="retro-go CPU overclock level baked into /CONFIG: "
                        "0=280MHz stock, 1=312, 2=340 (launcher max), "
                        "3=353 (not offered in the menu; unstable under "
                        "sustained load on some cores). NOTE /CONFIG is "
                        "rewritten on every push, so omitting this RESETS a "
                        "level set through the launcher UI")
    p.add_argument("--menu-timeout", type=int, default=0,
                   help="idle standby seconds; 0 disables (default)")
    p.add_argument("--no-config", action="store_true",
                   help="do not write /CONFIG (leaves the launcher showing)")
    args = p.parse_args()

    if args.all_roms:
        import glob
        found = sorted(glob.glob(os.path.join(args.all_roms, "*.dsk")))
        if not found:
            sys.exit(f"no .dsk files in {args.all_roms}")
        args.rom = (args.rom or []) + found
    if not args.rom:
        args.rom = [DEFAULT_ROM]

    for path in [args.core] + list(args.rom):
        if path and not os.path.exists(path):
            sys.exit(f"missing {path} -- build it first "
                     f"(make -f Makefile.core pack)")

    config = None
    if not args.no_config:
        if len(args.rom) != 1:
            sys.exit("--no-config is required when more than one ROM is on "
                     "the card: /CONFIG preselects exactly one title")
        startup = rom_path(args.rom_dir, os.path.basename(args.rom[0]))
        os.makedirs(os.path.dirname(args.config_out) or ".", exist_ok=True)
        with open(args.config_out, "wb") as fh:
            fh.write(build_config(startup_file=startup,
                                  menu_timeout_s=args.menu_timeout,
                                  cpu_oc_level=args.oc_level))
        oc = {0: "280MHz stock", 1: "312MHz", 2: "340MHz", 3: "353MHz"}
        print(f"/CONFIG preselects {startup}"
              f"{'  (standby off)' if args.menu_timeout == 0 else ''}"
              f"  [cpu {oc[args.oc_level]}]")
        config = args.config_out

    if args.image:
        if not args.keep or not os.path.exists(args.image):
            create_image(args.image, size_mb=args.size_mb)
        manager = QemuSDCardManager(args.image)
    elif args.mount:
        manager = HardwarePhysicalSDCardManager(args.mount)
    else:
        from gnwmanager.gnw import GnW
        from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend
        backend = OpenOCDBackend()
        backend.open()
        gnw = GnW(backend)
        gnw.start_gnwmanager()
        manager = HardwareProbeSDCardManager(gnw)

    compose(manager, core_bin=args.core, core_name=args.core_name,
            rom=args.rom, rom_dir=args.rom_dir, config=config,
            content_dir=args.content)

    if args.image:
        print(manager.listing("/"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
