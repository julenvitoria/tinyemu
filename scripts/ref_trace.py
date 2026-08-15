#!/usr/bin/env python3
"""
Run a DOS guest under a REFERENCE x86 emulator and record what it does.

Our interpreter is hand-written assembly; when a guest walks off into memory it
does not own, the question is always "which instruction first behaved
differently from a real 8086". Answering it by reading disassembly is how days
disappear. This runs the identical memory image under Unicorn and prints the
same kind of trace, so the two can be diffed.

The image is built by scripts/common/dosimage.py, which mirrors x86_dos_exec --
same PSP, same load segment, same relocations, same entry registers. Starting
from a different state would make any comparison meaningless.

    # every 4096th instruction, which is what to compare against a sampled
    # trace from the target
    python3 scripts/ref_trace.py --dsk references/.../TOPBENCH.dsk \\
        --exe TOPBENCH.EXE --args -l --every 4096 --count 256

    # a window of consecutive instructions once the divergence is localised
    python3 scripts/ref_trace.py --dsk ... --skip 651264 --count 64

    # where does the guest get to, and what does it print?
    python3 scripts/ref_trace.py --dsk ... --summary

INT is not emulated: the reference stops at the first software interrupt and
reports it, which for a self-unpacking program is exactly the finish line --
reaching INT 21h at all means the unpacker completed.
"""

import argparse
import os
import sys

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.dosimage import Fat12, load     # noqa: E402

RAM = 0xA0000        # 640 KB, same as the core


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dsk", required=True, help="FAT12 floppy image")
    p.add_argument("--exe", default=None,
                   help="file inside the .dsk (default: the only .EXE)")
    p.add_argument("--args", default="", help="command tail")
    p.add_argument("--psp", type=lambda s: int(s, 0), default=0x71)
    p.add_argument("--max", type=int, default=5_000_000,
                   help="instruction budget")
    p.add_argument("--skip", type=int, default=0,
                   help="do not print until this many instructions have run")
    p.add_argument("--every", type=int, default=1,
                   help="print one line per N instructions")
    p.add_argument("--count", type=int, default=64,
                   help="how many lines to print")
    p.add_argument("--summary", action="store_true",
                   help="print only where it stopped and why")
    args = p.parse_args()

    from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE, UC_HOOK_INTR
    from unicorn.x86_const import (
        UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS,
        UC_X86_REG_IP, UC_X86_REG_SP, UC_X86_REG_AX, UC_X86_REG_BX,
        UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_SI, UC_X86_REG_DI,
        UC_X86_REG_BP,
    )

    dsk = open(args.dsk, "rb").read()
    fs = Fat12(dsk)
    name = args.exe
    if not name:
        exes = [n for n, _, _ in fs.listdir() if n.upper().endswith(".EXE")]
        if len(exes) != 1:
            sys.exit(f"pick one with --exe: {exes}")
        name = exes[0]
    exe = fs.read(name)

    ram = bytearray(RAM)
    st = load(exe, ram, psp=args.psp, cmdline=args.args)
    print(f"{name}: {len(exe)} bytes, load_seg={st['load_seg']:#06x} "
          f"CS={st['cs']:#06x} IP={st['ip']:#06x} "
          f"SS={st['ss']:#06x} SP={st['sp']:#06x}")

    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, RAM)
    uc.mem_write(0, bytes(ram))
    for reg, key in ((UC_X86_REG_CS, "cs"), (UC_X86_REG_DS, "ds"),
                     (UC_X86_REG_ES, "es"), (UC_X86_REG_SS, "ss"),
                     (UC_X86_REG_IP, "ip"), (UC_X86_REG_SP, "sp"),
                     (UC_X86_REG_AX, "ax")):
        uc.reg_write(reg, st[key])

    state = {"n": 0, "printed": 0, "stop": None}

    def on_code(uc, addr, size, _):
        n = state["n"]
        state["n"] = n + 1
        if not args.summary and n >= args.skip and n % args.every == 0 \
                and state["printed"] < args.count:
            cs = uc.reg_read(UC_X86_REG_CS)
            print(f"{n:9d} {addr:#07x}  CS={cs:#06x}:{addr - cs * 16:#06x} "
                  f"AX={uc.reg_read(UC_X86_REG_AX):#06x} "
                  f"BX={uc.reg_read(UC_X86_REG_BX):#06x} "
                  f"CX={uc.reg_read(UC_X86_REG_CX):#06x} "
                  f"DX={uc.reg_read(UC_X86_REG_DX):#06x} "
                  f"SI={uc.reg_read(UC_X86_REG_SI):#06x} "
                  f"DI={uc.reg_read(UC_X86_REG_DI):#06x} "
                  f"BP={uc.reg_read(UC_X86_REG_BP):#06x} "
                  f"ES={uc.reg_read(UC_X86_REG_ES):#06x} "
                  f"DS={uc.reg_read(UC_X86_REG_DS):#06x}")
            state["printed"] += 1
        if state["n"] > args.max:
            state["stop"] = "instruction budget exhausted"
            uc.emu_stop()

    def on_intr(uc, intno, _):
        state["stop"] = f"INT {intno:#04x} after {state['n']} instructions"
        uc.emu_stop()

    uc.hook_add(UC_HOOK_CODE, on_code)
    uc.hook_add(UC_HOOK_INTR, on_intr)
    try:
        uc.emu_start(st["cs"] * 16 + st["ip"], 0)
    except Exception as e:            # a fault is a result, not a crash
        state["stop"] = state["stop"] or f"{type(e).__name__}: {e}"

    cs = uc.reg_read(UC_X86_REG_CS)
    ip = uc.reg_read(UC_X86_REG_IP)
    print(f"stopped: {state['stop']} at CS:IP = {cs:#06x}:{ip:#06x} "
          f"after {state['n']} instructions")
    print(f"  ES:DI = {uc.reg_read(UC_X86_REG_ES):#06x}:"
          f"{uc.reg_read(UC_X86_REG_DI):#06x}  "
          f"DS:SI = {uc.reg_read(UC_X86_REG_DS):#06x}:"
          f"{uc.reg_read(UC_X86_REG_SI):#06x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
