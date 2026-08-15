"""
Tier-2 convenience: build the bare-metal CPU harness and open a target on it.

Thin layer over scripts/common/target.py, which is the single implementation for
every tier and both targets. This module exists only because the CPU harness has
its own build step; it adds no launch, symbol or SD handling of its own.

The old qemu_runner.py / hardware_runner.py / retrogo_runner.py trio is gone --
see target.py for why one module now covers all of it.
"""

import argparse

from .builder import Builder
from .elf import get_symbols            # noqa: F401  (re-exported for harnesses)
from .target import Image, add_target_args, open_target


def add_runner_args(parser: argparse.ArgumentParser):
    """Tier-2 CLI surface: the shared target switches plus the build step."""
    add_target_args(parser)
    parser.add_argument("--clean", action="store_true",
                        help="clean the build directory before compiling")
    parser.add_argument("--elf", default="test_core.elf",
                        help="ELF file to test")


def build_and_create_runner(args):
    """Build test_core.elf + media, then return a Target ready to start()."""
    Builder(clean=getattr(args, "clean", False)).build(
        target=getattr(args, "target", "gwemu"))

    image = Image(bank1="qemu_bank1.bin",
                  bank2="qemu_bank2.bin",
                  extflash="extflash.bin",
                  sdcard="sdcard.img",
                  elf=getattr(args, "elf", "test_core.elf"),
                  intflash_bank=1)
    # Hardware flashes the raw intflash image, not the padded bank file.
    if getattr(args, "target", "gwemu") == "hardware":
        image.bank1 = "test_core_intflash.bin"
    return open_target(args, image)
