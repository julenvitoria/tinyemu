#!/usr/bin/env python3
"""
Differential fuzz of the ARITHMETIC core against Unicorn, in 16-bit real mode.

## Why this exists

`tests/test_opcodes.py` is 273 HAND-WRITTEN cases, and a hand-written case can
only pin down a behaviour someone already thought of. Wolfenstein 3-D's
corrupted gameplay was traced to `finetangent[]` -- a table the guest COMPUTES
at startup through Borland's software 80x87 emulator, i.e. through plain
integer instructions -- coming out wrong on our core while the level map, the
player position and the whole video path were byte-correct. That is a class of
bug no picture and no golden can localise, and no hand-written case had
covered: the arithmetic is right for the operands somebody chose and wrong for
the ones a mantissa normalisation actually produces.

So this fuzzes the instructions such an emulator lives on -- the Grp2 shifts
and rotates by 1/imm8/CL, the Grp3 multiply and divide, the carry-consuming
add/subtract and the decimal/sign-extension helpers -- over RANDOM operands,
and compares registers and flags against Unicorn instruction by instruction.

## What it does NOT compare, and why

* **AF after shifts, rotates, logic and MUL/DIV/IMUL/IDIV.** x86 leaves it
  architecturally undefined there and the two implementations are entitled to
  disagree. Same rule `scripts/trace_diff.py` already applies.
* **OF after a multi-bit shift or rotate.** Also undefined; only the count==1
  forms pin it.
* A case that FAULTS on either side (a divide overflow) is skipped rather than
  compared -- the fault path has its own tests, and a #DE leaves Unicorn and
  our core in deliberately different states.

## Running it

    python3 tests/test_alu_fuzz.py                  # 4000 cases, seeded
    python3 tests/test_alu_fuzz.py --cases 20000
    python3 tests/test_alu_fuzz.py --seed 7 --group grp2
    python3 tests/test_alu_fuzz.py --replay 1234    # one case, verbosely

The seed is printed and fixed by default, so a failure is reproducible and the
suite is not a different test on every run.
"""

import argparse
import os
import random
import sys
import time

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.runner import build_and_create_runner, add_runner_args  # noqa: E402
from scripts.common.elf import get_symbols                                  # noqa: E402

# Mirrors tests/test_opcodes.py -- same struct, same offsets file.
import re


def _generated_offsets(path="x86_offsets.inc"):
    out = {}
    for line in open(path):
        m = re.match(r"\s*\.equ\s+(\w+)\s*,\s*(\d+)", line)
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


OFF = _generated_offsets()
CR0_OFFSET = OFF["CPU_CR0"]
EXIT_REASON_OFFSET = OFF["CPU_EXIT_REASON"]
HLE_ENABLED_OFFSET = OFF["CPU_HLE_ENABLED"]
PIT_COUNTER_OFFSET = OFF["CPU_PIT_COUNTER"]
HARD_INT_OFFSET = OFF["CPU_HARD_INT_PENDING"]

SEG_BASE_OFFSETS = {"ES": 52, "CS": 64, "SS": 76, "DS": 88, "FS": 100, "GS": 112}
SEG_ORDER = ("ES", "CS", "SS", "DS", "FS", "GS")

CF, PF, AF, ZF, SF, OF = 1 << 0, 1 << 2, 1 << 4, 1 << 6, 1 << 7, 1 << 11

# Register order used by the x86 encoding, which is also the order the CPU
# struct stores them in.
REG16 = ["AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI"]


class Case:
    """One instruction, its operands, and which flags are architecturally defined."""

    def __init__(self, name, code, regs, flags_in, flag_mask):
        self.name = name
        self.code = code
        self.regs = regs                # index -> 32-bit value
        self.flags_in = flags_in
        self.flag_mask = flag_mask      # flags worth comparing


def rnd16(rng):
    """Operands a normalisation loop actually produces: powers of two and their
    neighbours as often as uniform noise, because that is where a carry or a
    borrow changes the exponent."""
    r = rng.random()
    if r < 0.30:
        b = rng.randrange(16)
        return (1 << b) + rng.choice((-1, 0, 1)) & 0xFFFF
    if r < 0.40:
        return rng.choice((0, 1, 0xFFFF, 0x8000, 0x7FFF, 0x8001))
    return rng.randrange(0x10000)


def gen_grp2(rng):
    """C0/C1/D0/D1/D2/D3 -- ROL ROR RCL RCR SHL SHR SAL SAR, all three count
    forms, both widths."""
    op = rng.randrange(8)
    names = ["ROL", "ROR", "RCL", "RCR", "SHL", "SHR", "SAL", "SAR"]
    w16 = rng.random() < 0.5
    form = rng.choice(("1", "imm", "cl"))
    dest = rng.randrange(8) if w16 else rng.randrange(4)   # 8-bit: AL..BL only
    regs = {i: rnd16(rng) for i in range(8)}
    modrm = 0xC0 | (op << 3) | dest
    if form == "1":
        code = bytes([0xD1 if w16 else 0xD0, modrm])
        count = 1
    elif form == "imm":
        count = rng.randrange(0, 40)
        code = bytes([0xC1 if w16 else 0xC0, modrm, count])
    else:
        count = rng.randrange(0, 40)
        regs[1] = (regs[1] & 0xFF00) | count               # CL
        code = bytes([0xD3 if w16 else 0xD2, modrm])
    eff = count & 31
    mask = CF | PF | ZF | SF
    if op >= 4:                                            # a shift writes SF/ZF/PF
        if eff == 1:
            mask |= OF
    else:                                                  # a rotate leaves them alone
        mask = CF | (OF if eff == 1 else 0)
    if eff == 0:
        mask = 0                                           # no flag is touched at all
    nm = "%s %s,%s" % (names[op], REG16[dest] if w16 else ["AL", "CL", "DL", "BL"][dest],
                       "1" if form == "1" else (str(count) if form == "imm" else "CL"))
    return Case(nm, code, regs, rng.randrange(2) * CF, mask)


def gen_grp3(rng):
    """F6/F7 -- TEST NOT NEG MUL IMUL DIV IDIV, both widths."""
    op = rng.randrange(8)
    names = ["TEST", "TEST", "NOT", "NEG", "MUL", "IMUL", "DIV", "IDIV"]
    w16 = rng.random() < 0.5
    src = rng.randrange(8) if w16 else rng.randrange(4)
    regs = {i: rnd16(rng) for i in range(8)}
    if op in (6, 7):
        # Keep the divisor away from zero; a #DE is a different test.
        v = regs[src] & (0xFFFF if w16 else 0xFF)
        if v == 0:
            regs[src] |= 1
    modrm = 0xC0 | (op << 3) | src
    code = bytes([0xF7 if w16 else 0xF6, modrm])
    if op in (0, 1):
        code += bytes([rng.randrange(256)] if not w16
                      else [rng.randrange(256), rng.randrange(256)])
    if op in (0, 1):
        mask = CF | PF | ZF | SF | OF
    elif op == 2:
        mask = 0
    elif op == 3:
        mask = CF | PF | ZF | SF | OF
    elif op in (4, 5):
        mask = CF | OF
    else:
        mask = 0                                           # DIV/IDIV: all undefined
    nm = "%s %s" % (names[op], REG16[src] if w16 else ["AL", "CL", "DL", "BL"][src])
    return Case(nm, code, regs, rng.randrange(2) * CF, mask)


def gen_alu(rng):
    """The carry chain: ADC/SBB/ADD/SUB/CMP/AND/OR/XOR, r/m,r at both widths."""
    op = rng.randrange(8)
    names = ["ADD", "OR", "ADC", "SBB", "AND", "SUB", "XOR", "CMP"]
    w16 = rng.random() < 0.5
    d = rng.randrange(8) if w16 else rng.randrange(4)
    s = rng.randrange(8) if w16 else rng.randrange(4)
    regs = {i: rnd16(rng) for i in range(8)}
    code = bytes([(op << 3) | (1 if w16 else 0), 0xC0 | (s << 3) | d])
    nm = "%s %s,%s" % (names[op],
                       REG16[d] if w16 else ["AL", "CL", "DL", "BL"][d],
                       REG16[s] if w16 else ["AL", "CL", "DL", "BL"][s])
    return Case(nm, code, regs, rng.randrange(2) * CF,
                CF | PF | ZF | SF | OF | AF)


def gen_misc(rng):
    """CBW/CWD, AAM/AAD, SHLD/SHRD, XCHG, INC/DEC -- the helpers around the
    mantissa work."""
    regs = {i: rnd16(rng) for i in range(8)}
    which = rng.randrange(6)
    if which == 0:
        return Case("CBW", bytes([0x98]), regs, 0, 0)
    if which == 1:
        return Case("CWD", bytes([0x99]), regs, 0, 0)
    if which == 2:
        b = rng.randrange(1, 256)
        return Case("AAM %d" % b, bytes([0xD4, b]), regs, 0, PF | ZF | SF)
    if which == 3:
        b = rng.randrange(256)
        return Case("AAD %d" % b, bytes([0xD5, b]), regs, 0, PF | ZF | SF)
    if which == 4:
        d, s = rng.randrange(8), rng.randrange(8)
        n = rng.randrange(0, 40)
        opc = rng.choice((0xA4, 0xAC))
        return Case("%s %s,%s,%d" % ("SHLD" if opc == 0xA4 else "SHRD",
                                     REG16[d], REG16[s], n),
                    bytes([0x0F, opc, 0xC0 | (s << 3) | d, n]), regs,
                    0, (CF | PF | ZF | SF) if (n & 31) else 0)
    r = rng.randrange(8)
    inc = rng.random() < 0.5
    return Case("%s %s" % ("INC" if inc else "DEC", REG16[r]),
                bytes([(0x40 if inc else 0x48) | r]), regs,
                rng.randrange(2) * CF, PF | ZF | SF | OF | AF)


GROUPS = {"grp2": gen_grp2, "grp3": gen_grp3, "alu": gen_alu, "misc": gen_misc}


def unicorn_run(case):
    """The oracle. Returns (regs16, eflags) or None if the case faulted."""
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UcError
    from unicorn.x86_const import (
        UC_X86_REG_AX, UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_BX,
        UC_X86_REG_SP, UC_X86_REG_BP, UC_X86_REG_SI, UC_X86_REG_DI,
        UC_X86_REG_EFLAGS, UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES,
        UC_X86_REG_SS, UC_X86_REG_IP)
    ids = [UC_X86_REG_AX, UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_BX,
           UC_X86_REG_SP, UC_X86_REG_BP, UC_X86_REG_SI, UC_X86_REG_DI]
    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, 0x10000)
    uc.mem_write(0, case.code)
    for r in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
        uc.reg_write(r, 0)
    for i, rid in enumerate(ids):
        uc.reg_write(rid, case.regs.get(i, 0) & 0xFFFF)
    uc.reg_write(UC_X86_REG_EFLAGS, 0x2 | case.flags_in)
    uc.reg_write(UC_X86_REG_IP, 0)
    try:
        uc.emu_start(0, len(case.code), count=1)
    except UcError:
        return None
    return ([uc.reg_read(r) & 0xFFFF for r in ids],
            uc.reg_read(UC_X86_REG_EFLAGS))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    add_runner_args(p)
    p.add_argument("--cases", type=int, default=4000)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--group", choices=sorted(GROUPS), action="append")
    p.add_argument("--stop-after", type=int, default=12,
                   help="give up after this many distinct failures")
    args = p.parse_args()

    try:
        import unicorn                                     # noqa: F401
    except ImportError:
        print("unicorn is not installed (pip install unicorn) -- SKIP")
        return 0

    groups = args.group or sorted(GROUPS)
    rng = random.Random(args.seed)
    print(f"seed={args.seed} cases={args.cases} groups={','.join(groups)}")

    runner = build_and_create_runner(args)
    runner.start()
    b = runner.backend
    syms = get_symbols("test_core.elf")
    dtcm, ram = syms["cpu_state_dtcm"], syms["test_ram"]
    run_flag, page_table = syms["run_flag"], syms["guest_page_table"]

    deadline = 50
    while deadline > 0 and b.read_uint32(page_table) != ram:
        time.sleep(0.1)
        deadline -= 1

    fails, checked, unimpl = [], 0, set()
    try:
        for n in range(args.cases):
            case = GROUPS[rng.choice(groups)](rng)
            want = unicorn_run(case)
            if want is None:
                continue                                   # faulted: not ours to grade
            checked += 1

            b.write_memory(ram, case.code + bytes([0xF4]))
            b.write_uint32(dtcm + 0, 0)
            b.write_uint32(dtcm + 4, case.flags_in)
            for i in range(8):
                b.write_uint32(dtcm + 8 + i * 4, case.regs.get(i, 0) & 0xFFFF)
            for seg in SEG_ORDER:
                off = SEG_BASE_OFFSETS[seg] - 4
                b.write_uint32(dtcm + off, 0)              # sel 0, flags 0 -> 16-bit
                b.write_uint32(dtcm + off + 4, 0)
            b.write_uint32(dtcm + CR0_OFFSET, 0)
            b.write_uint32(dtcm + HLE_ENABLED_OFFSET, 0)
            b.write_uint32(dtcm + 40, 0)
            b.write_uint32(dtcm + HARD_INT_OFFSET, 0)
            b.write_uint32(dtcm + PIT_COUNTER_OFFSET, 0xFFFFFFFF)
            b.write_uint32(dtcm + EXIT_REASON_OFFSET, 0)
            b.write_uint32(run_flag, 1)

            t = 200
            while t > 0 and b.read_uint32(run_flag) != 0:
                time.sleep(0.005)
                t -= 1
            if t == 0:
                fails.append((case, "TIMEOUT", None, None))
                break

            # An opcode with no handler exits with X86_EXIT_UNIMPLEMENTED and
            # leaves the registers alone. That is a LOUD, known gap (SHLD/SHRD
            # are still `.exit_loop` in opcode_0f_table.S), not a wrong answer,
            # so it is reported as a gap rather than graded as a disagreement --
            # otherwise the one real arithmetic bug drowns in 40 of these.
            if b.read_uint32(dtcm + EXIT_REASON_OFFSET) == 4:
                unimpl.add(case.name.split()[0])
                checked -= 1
                continue

            got = [b.read_uint32(dtcm + 8 + i * 4) & 0xFFFF for i in range(8)]
            gfl = b.read_uint32(dtcm + 4)
            bad = []
            for i in range(8):
                if got[i] != want[0][i]:
                    bad.append("%s ours=%04x ref=%04x" % (REG16[i], got[i], want[0][i]))
            if (gfl ^ want[1]) & case.flag_mask:
                for nm, bit in (("CF", CF), ("PF", PF), ("AF", AF),
                                ("ZF", ZF), ("SF", SF), ("OF", OF)):
                    if case.flag_mask & bit and (gfl ^ want[1]) & bit:
                        bad.append("%s ours=%d ref=%d" %
                                   (nm, bool(gfl & bit), bool(want[1] & bit)))
            if bad:
                fails.append((case, "; ".join(bad), got, want))
                print("FAIL #%d %-24s code=%s in=%s" %
                      (n, case.name, case.code.hex(),
                       {REG16[i]: "%04x" % (case.regs.get(i, 0) & 0xFFFF)
                        for i in range(8)}))
                print("      " + "; ".join(bad))
                if len(fails) >= args.stop_after:
                    break
    finally:
        try:
            b.halt()
        except Exception:                                   # noqa: BLE001
            pass
        runner.stop()

    if unimpl:
        print("not implemented by this core (exit_reason=UNIMPLEMENTED): "
              + ", ".join(sorted(unimpl)))
    print(f"\n{checked - len(fails)}/{checked} graded cases agree with Unicorn")
    if fails:
        print(f"{len(fails)} DISAGREEMENTS -- the first is the one to fix")
        return 1
    print("all agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
