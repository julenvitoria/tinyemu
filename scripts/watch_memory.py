#!/usr/bin/env python3
"""
Stop the target on a write to a given address, and name what wrote it.

`probe.py` samples; this catches. When guest memory is being corrupted, polling
never sees the write that did it -- it is one instruction among millions -- so
this sets a real data watchpoint and reports the ARM PC at the stop, resolved
against the ELF. Inside our interpreter that PC is the handler, which names the
guest instruction class immediately.

Addresses may be a raw 0x..., a symbol, a POINTER dereference with an offset,
or `guest:0xLIN` for a guest LINEAR address. Guest memory needs the last form:
`guest_ram` is a frame POOL since demand paging landed, so `*guest_ram+OFF`
reads an unrelated resident page. That spelling is REFUSED rather than served.

    # who is scribbling on the relocated stub at guest linear 0x2E920?
    python3 scripts/watch_memory.py --retrogo --wait-magic \\
        --addr guest:0x2E920 --length 4

    # a plain global, and keep going for the first three writes
    python3 scripts/watch_memory.py --retrogo --wait-magic \\
        --addr core_ready --hits 3

Each hit also reports the guest's own EIP/CS, so a corrupting write can be tied
back to the guest instruction that issued it, not just to our handler.
"""

import argparse
import os
import sys

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.probe import (                       # noqa: E402
    describe_pc, load_symbol_maps, reject_guest_ram, wait_for_value,
)
from scripts.common.target import Image, add_target_args, open_target  # noqa: E402
from scripts.common.watchpoint import (                  # noqa: E402
    clear_write_watchpoint, set_write_watchpoint, wait_for_stop,
)

FW_DIR = "references/game-and-watch-retro-go-sd/build"
CORE_MAGIC = 0x54494E59

# Byte offsets into X86CPUState, mirroring x86_offsets.inc. The index registers
# and segment bases are written back by the interpreter as it goes, so they are
# live at a watchpoint stop; cpu_state.eip is NOT -- it is only stored on exit.
# The live linear EIP is ARM r10, which is why that is read from the register
# file rather than from the struct.
CPU_FIELDS = [("ECX", 12), ("EDX", 16), ("ESI", 32), ("EDI", 36),
              ("ES.base", 52), ("CS.base", 64), ("SS.base", 76),
              ("DS.base", 88)]
GUEST_EIP_REG = "r10"


def resolve(backend, syms, spec):
    """'0xADDR' | 'SYM[+OFF]' | '*SYM[+OFF]' | 'guest:0xLIN'.

    `guest:` walks the fold table, which is the only correct way to reach guest
    memory since demand paging landed: `guest_ram` is a frame POOL, so
    `*guest_ram+OFF` no longer names a guest linear address at all.
    """
    if spec.startswith("guest:"):
        from scripts.common.probe import GUEST_PAGE_SIZE, FOLD_INVALID
        lin = int(spec.split(":", 1)[1], 0)
        base = backend.read_uint32(syms["guest_page_table_ptr"])
        k = backend.read_uint32(base + 4 * (lin // GUEST_PAGE_SIZE))
        if k & FOLD_INVALID:
            sys.exit(f"--addr: guest linear {lin:#x} is not mapped right now")
        return (lin + k) & 0xFFFFFFFF
    reject_guest_ram(spec)
    deref = spec.startswith("*")
    body = spec[1:] if deref else spec
    name, _, off = body.partition("+")
    off = int(off, 0) if off else 0
    name = name.strip()
    if name.startswith("0x"):
        base = int(name, 0)
    elif name in syms:
        base = syms[name]
    else:
        sys.exit(f"--addr: symbol {name!r} not found")
    if deref:
        base = backend.read_uint32(base)
    return base + off


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_target_args(p)
    p.add_argument("--retrogo", action="store_true",
                   help="boot retro-go + our packed core")
    p.add_argument("--firmware-dir", default=FW_DIR)
    p.add_argument("--sd", default="sdcard_retrogo.img")
    p.add_argument("--elf", default=None)
    p.add_argument("--addr", required=True,
                   metavar="0xADDR|SYM[+OFF]|*SYM[+OFF]",
                   help="address to watch; '*' dereferences the symbol first, "
                        "'guest:0xLIN' walks the fold table to guest memory")
    p.add_argument("--length", type=int, default=4,
                   help="bytes covered by the watchpoint (default 4)")
    p.add_argument("--hits", type=int, default=1,
                   help="stop this many times before giving up the watch")
    p.add_argument("--timeout", type=float, default=30.0,
                   help="seconds to wait for each hit")
    p.add_argument("--peek", action="append", default=[],
                   metavar="ADDRSPEC:N",
                   help="hexdump N bytes at ADDRSPEC on every hit; the same "
                        "address forms as --addr. Use a watchpoint on a "
                        "completion flag to get a clean snapshot of what some "
                        "load or copy actually produced. Repeatable.")
    p.add_argument("--wait-magic", action="store_true")
    args = p.parse_args()

    if args.retrogo:
        fw = args.firmware_dir
        elf = args.elf or "build/core/dos_core.elf"
        image = Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                      bank2=os.path.join(fw, "qemu_bank2.bin"),
                      extflash=os.path.join(fw, "extflash.bin"),
                      sdcard=args.sd, elf=elf, intflash_bank=1)
        if args.timeline is None:
            args.timeline = "timelines/start-preselected-game.tl"
        fw_elf = os.path.join(fw, "gw_retro_go.elf")
    else:
        elf = args.elf or "test_core.elf"
        image = Image(bank1="qemu_bank1.bin", bank2="qemu_bank2.bin",
                      extflash="extflash.bin", sdcard="sdcard.img",
                      elf=elf, intflash_bank=1)
        fw_elf = None

    syms, maps = load_symbol_maps(fw_elf, elf)
    target = open_target(args, image)
    target.start()
    try:
        backend = target.backend
        if args.wait_magic and "core_magic" in syms:
            print("waiting for core_magic...")
            if not wait_for_value(backend, syms["core_magic"], CORE_MAGIC, 30.0):
                print("core never started; watching anyway")
            else:
                print("core running")

        backend.halt()
        addr = resolve(backend, syms, args.addr)
        print(f"watching writes to 0x{addr:08x} ({args.length} bytes)")
        set_write_watchpoint(backend, addr, args.length)

        for hit in range(1, args.hits + 1):
            reply = wait_for_stop(backend, args.timeout)
            if reply is None:
                print(f"hit {hit}: no write within {args.timeout}s")
                break
            pc = backend.read_register("pc")
            val = int.from_bytes(bytes(backend.read_memory(addr, 4)), "little")
            print(f"hit {hit}: PC=0x{pc:08x} {describe_pc(pc, maps or [syms])}"
                  f"  [addr]=0x{val:08x}")
            lin = backend.read_register(GUEST_EIP_REG)
            print(f"        guest linear EIP=0x{lin:05x}")
            if "cpu_state_ptr" in syms:
                cpu = backend.read_uint32(syms["cpu_state_ptr"])
                fields = " ".join(
                    f"{n}=0x{backend.read_uint32(cpu + o):x}"
                    for n, o in CPU_FIELDS)
                print(f"        {fields}")
            for spec in args.peek:
                pspec, _, n = spec.rpartition(":")
                base = resolve(backend, syms, pspec)
                data = bytes(backend.read_memory(base, int(n, 0)))
                print(f"        peek 0x{base:08x} +{len(data)}:")
                for i in range(0, len(data), 16):
                    print(f"          {i:04x}  {data[i:i+16].hex(' ')}")
        clear_write_watchpoint(backend, addr, args.length)
    finally:
        target.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
