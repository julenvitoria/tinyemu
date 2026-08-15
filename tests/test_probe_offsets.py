#!/usr/bin/env python3
"""probe_report.py's DEATH-SITE offsets, checked against the generated ones.

WHY THIS EXISTS. `scripts/probe_report.py` reads the CPU's exit state by
dereferencing `cpu_state_ptr` and adding hard-coded byte offsets, because
`cpu_state_ptr` is a POINTER to a DTCM struct and a symbol read cannot reach
through it. Those offsets therefore duplicate `x86_offsets.inc`, which is
generated from `offsetof()` in `offset.c` -- and a duplicated offset that drifts
does not fail, it silently reports the NEIGHBOURING FIELD.

That failure mode is not hypothetical here. The whole reason the death-site rows
were added is that three titles were reported as dying at one shared `CS:EIP`
that had never been read off the CPU at all; the last thing this instrument
should do is reintroduce the same class of error one struct change later.

Needs no target and no build beyond `x86_offsets.inc`. Run it in seconds:

    python3 tests/test_probe_offsets.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

INC = os.path.join(ROOT, "x86_offsets.inc")

# probe_report's name -> the .equ symbol in x86_offsets.inc that defines it.
# CPU_EIP is 0 and CPU_REGS+4 is ESP; both are spelled out rather than assumed.
EXPECT = {
    "cpu_eip":         "CPU_EIP",
    "cpu_esp":         "CPU_ESP",
    "cpu_cs_sel":      "SEG_CS_SEL",
    "cpu_ss_sel":      "SEG_SS_SEL",
    "cpu_cr0":         "CPU_CR0",
    "cpu_exit_reason": "CPU_EXIT_REASON",
    "cpu_exit_info":   "CPU_EXIT_INFO",
}
# cpu_cs_base has no death-site row of its own beyond deriving cpu_lin_eip.
EXPECT_EXTRA = {"cpu_cs_base": "SEG_CS_BASE"}


def load_inc(path):
    if not os.path.exists(path):
        sys.exit("test_probe_offsets: %s is missing -- run "
                 "`make -f Makefile.core` first (it is generated)." % path)
    out = {}
    pat = re.compile(r"^\s*\.equ\s+(\w+)\s*,\s*(-?\d+)\s*$")
    for line in open(path):
        m = pat.match(line)
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


def main():
    inc = load_inc(INC)
    from probe_report import CPU_FIELDS

    want = dict(EXPECT)
    want.update(EXPECT_EXTRA)

    fails = []

    # 1. Every field probe_report reads is one this test knows how to check.
    #    A new field added without a mapping here is a HOLE, not a pass.
    for name in CPU_FIELDS:
        if name not in want:
            fails.append("probe_report reads %r but this test has no "
                         "x86_offsets.inc symbol for it -- add one to EXPECT"
                         % name)

    # 2. ...and every mapping is actually used.
    for name in want:
        if name not in CPU_FIELDS:
            fails.append("this test expects %r but probe_report no longer "
                         "reads it" % name)

    # 3. The offsets agree.
    for name, sym in want.items():
        if name not in CPU_FIELDS:
            continue
        if sym not in inc:
            fails.append("%s: %s is not in x86_offsets.inc" % (name, sym))
            continue
        got, exp = CPU_FIELDS[name], inc[sym]
        if got != exp:
            fails.append("%s: probe_report says %d, %s is %d -- probe_report "
                         "is reading the WRONG FIELD"
                         % (name, got, sym, exp))
        else:
            print("  PASS: %-16s = %-5d (%s)" % (name, got, sym))

    if fails:
        for f in fails:
            print("  FAIL: %s" % f)
        sys.exit("FAIL: %d of %d checks" % (len(fails), len(want) + len(fails)))
    print("PASS: %d checks" % len(want))


if __name__ == "__main__":
    main()
