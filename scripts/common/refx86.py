"""Reference 16-bit x86 execution, for diffing against our interpreter.

Unicorn runs the same memory image our core loads (see dosimage.py) and reports
the linear address of every instruction. That is the only way to answer "which
instruction first behaved differently from a real 8086" without reading
disassembly for a day.

Sampling matches the target-side sampler in cpu_exec.S: take one sample every
`every` instructions starting at `skip`, up to `count` of them. Note that a
`REP` element counts as one instruction on BOTH sides, which is what makes the
two comparable at all.
"""

import struct

RAM = 0xA0000        # 640 KB, same as the core
VIDEO = 0xB8000      # the text buffer, mapped separately so a program can
VIDEO_LEN = 0x8000   # write to the screen without faulting


def load_exe(dsk_path, exe_name=None):
    from scripts.common.dosimage import Fat12
    fs = Fat12(open(dsk_path, "rb").read())
    if not exe_name:
        exes = [n for n, _, _ in fs.listdir() if n.upper().endswith(".EXE")]
        if len(exes) != 1:
            raise SystemExit(f"pick one with --exe: {exes}")
        exe_name = exes[0]
    return exe_name, fs.read(exe_name)


def run(exe, cmdline="", psp=0x71, max_insns=5_000_000,
        skip=0, every=1, count=64, dos=False):
    """Returns (samples, info). Each sample is a dict with n, addr and registers."""
    from scripts.common.dosimage import load
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE, UC_HOOK_INTR
    from unicorn.x86_const import (
        UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS,
        UC_X86_REG_IP, UC_X86_REG_SP, UC_X86_REG_AX, UC_X86_REG_BX,
        UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_SI, UC_X86_REG_DI,
        UC_X86_REG_BP, UC_X86_REG_EFLAGS,
    )
    REGS = [("AX", UC_X86_REG_AX), ("BX", UC_X86_REG_BX),
            ("CX", UC_X86_REG_CX), ("DX", UC_X86_REG_DX),
            ("SI", UC_X86_REG_SI), ("DI", UC_X86_REG_DI),
            ("BP", UC_X86_REG_BP), ("SP", UC_X86_REG_SP),
            ("CS", UC_X86_REG_CS), ("DS", UC_X86_REG_DS),
            ("ES", UC_X86_REG_ES), ("SS", UC_X86_REG_SS),
            ("FL", UC_X86_REG_EFLAGS)]

    ram = bytearray(RAM)
    st = load(exe, ram, psp=psp, cmdline=cmdline)

    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, RAM)
    uc.mem_write(0, bytes(ram))
    uc.mem_map(VIDEO, VIDEO_LEN)
    uc.mem_write(VIDEO, b"\x20\x07" * (VIDEO_LEN // 2))
    for reg, key in ((UC_X86_REG_CS, "cs"), (UC_X86_REG_DS, "ds"),
                     (UC_X86_REG_ES, "es"), (UC_X86_REG_SS, "ss"),
                     (UC_X86_REG_IP, "ip"), (UC_X86_REG_SP, "sp"),
                     (UC_X86_REG_AX, "ax")):
        uc.reg_write(reg, st[key])

    samples = []
    info = {"n": 0, "stop": None, "entry": st, "prev": None, "rep_skipped": 0}

    def on_code(uc, addr, size, _):
        # Unicorn fires this hook once per REP ELEMENT and then ONE MORE TIME,
        # at the same address, with CX already zero -- the terminating check.
        # Our interpreter has no such pass: it leaves the string op as soon as
        # the count reaches zero. Counting that phantom firing shifts every
        # later index by one PER REP EXECUTED, which reads downstream as "the
        # target left the copy loop early" and is the single most misleading
        # artifact in this comparison. Drop it.
        if addr == info["prev"] and uc.mem_read(addr, 1)[0] in (0xF2, 0xF3) \
                and (uc.reg_read(UC_X86_REG_CX) & 0xFFFF) == 0:
            return
        info["prev"] = addr
        n = info["n"]
        info["n"] = n + 1
        if n >= skip and (n - skip) % every == 0 and len(samples) < count:
            row = {"n": n, "addr": addr}
            for name, r in REGS:
                row[name] = uc.reg_read(r)
            samples.append(row)
        if shim is not None and (n % 20000) == 0:
            # The BIOS tick at 0040:006C, which a DOS timing loop reads
            # directly rather than through INT 1Ah. Derived from the
            # instruction count so it advances monotonically without needing a
            # model of clock speed.
            shim.ticks = n // 20000
            uc.mem_write(0x46C, struct.pack("<I", shim.ticks))
        if info["n"] > max_insns:
            info["stop"] = "instruction budget exhausted"
            uc.emu_stop()

    shim = None
    if dos:
        from scripts.common.refdos import RefDos
        shim = RefDos(uc, psp=psp)
        info["dos"] = shim

    def on_intr(uc, intno, _):
        # Without a shim, the first software interrupt is the finish line: it
        # means the guest got through whatever ran before it. With one, service
        # the call and let it keep going -- IRET is what the guest expects, so
        # unwind the frame the interrupt pushed by hand.
        if shim is None:
            info["stop"] = f"INT {intno:#04x}"
            uc.emu_stop()
            return
        if not shim.service(intno):
            info["stop"] = f"program exited, code {shim.exit_code}"
            uc.emu_stop()
            return
        # Unicorn does NOT push an interrupt frame and leaves IP already past
        # the INT, so there is nothing to unwind -- resume where it points.
        cs = uc.reg_read(UC_X86_REG_CS)
        ip = uc.reg_read(UC_X86_REG_IP)
        info["resume"] = (cs << 4) + ip
        uc.emu_stop()

    uc.hook_add(UC_HOOK_CODE, on_code)
    uc.hook_add(UC_HOOK_INTR, on_intr)
    # An INT has to stop the engine so the shim can rewrite CS:IP, then the
    # run continues from where IRET would have landed.
    pc = st["cs"] * 16 + st["ip"]
    while True:
        info["resume"] = None
        try:
            uc.emu_start(pc, 0)
        except Exception as e:           # a guest fault is a result, not a crash
            info["stop"] = info["stop"] or f"{type(e).__name__}: {e}"
        if info.get("resume") is None or info["stop"]:
            break
        pc = info["resume"]
    info["final"] = {name: uc.reg_read(r) for name, r in REGS}
    info["final"]["IP"] = uc.reg_read(UC_X86_REG_IP)
    return samples, info


def format_row(row):
    return (f"{row['n']:9d} {row['addr']:#07x}  "
            + " ".join(f"{k}={row[k]:#06x}" for k in
                       ("CS", "AX", "BX", "CX", "DX", "SI", "DI", "BP", "FL",
                        "ES", "DS", "SS", "SP")))
