#!/usr/bin/env python3
"""
Tier-1 check of the machine DESCRIPTION services: INT 11h, INT 12h, INT 15h,
and the BIOS Data Area fields that must agree with them.

This is the surface a DOS program uses to decide what hardware it is talking
to, and it had no cover at all -- INT 11h, INT 12h and INT 15h were not even
in the served vector set, so each one reached the bare IRET stub.

**A bare IRET is not "absent"; it is worse than absent.** It does not clear a
register, so the guest reads back whatever it left in AX and believes it. A
program that asks INT 12h how much conventional memory it has and is told
0xFFFF has been handed 65 MB of RAM that is not there, by a BIOS that reported
success. Nothing on the host side moves: the vector is taken, the IRET runs,
every counter advances. That is why every call here POISONS AX first -- the
poison coming back is the only observable difference between "answered" and
"not answered".

CF is read off the IRET FRAME (a PUSHF after the INT), not from a live
register, because the stub's IRET is about to overwrite the live one -- the
same contract the INT 16h ZF arm turns on.

Covered:
  * INT 11h returns the equipment word, and it describes THIS machine: one
    floppy, no coprocessor, no game adapter, and initial video mode 80x25
    COLOUR (bits 4-5 = 10b). That video field is how a program is told this is
    a CGA-class colour adapter and not a mono one.
  * INT 12h returns 640 KB, and it is the SAME 640 the BDA word at 0040:0013
    holds -- a program is free to peek instead of call.
  * INT 15h AH=88h returns AX=64 with CF CLEAR: the HMA, and nothing else, is
    backed above 1 MB (x86_mem_wrap_1mb() gives the sixteen pages at 0x100000
    real frames whenever A20 is open), so 64 KB is the honest figure. It read 0
    while that memory did not exist; the number moved because the memory did.
  * INT 15h AH=C0h returns CF SET with AH=86h -- the honest "this BIOS does not
    have that call", which is a real answer and not silence.
  * 0040:0010 equipment, 0040:0013 memory size, 0040:0063 CRTC base port
    (0x3D4, the colour adapter -- the mono alias is deliberately NOT decoded),
    and the keyboard ring bounds at 0040:0080 / 0040:0082.
"""
import argparse
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

CF = 1 << 0

# One floppy (bit 0), no coprocessor (bit 1), video 80x25 colour (bits 4-5 =
# 10b), no game adapter, no serial, no parallel.
EXPECT_EQUIP = 0x0021
EXPECT_CONV_KB = 640
EXPECT_EXT_KB = 64      # the HMA: 16 pages x 4 KB, and nothing above it
EXPECT_CRTC = 0x3D4


def main(args):
    runner = build_and_create_runner(args)
    ok = True
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols("test_core.elf")
        for n in ("run_sysinfo_flag", "sys_r"):
            if n not in syms:
                print(f"missing symbol {n} -- rebuild tests/main.c")
                return False
        time.sleep(0.5)

        b.write_uint32(syms["run_sysinfo_flag"], 1)
        for _ in range(600):
            if b.read_uint32(syms["run_sysinfo_flag"]) == 0:
                break
            time.sleep(0.05)
        else:
            print("FAIL: the sysinfo arm never completed")
            return False

        r = [b.read_uint32(syms["sys_r"] + 4 * i) for i in range(15)]
        (equip, conv, ext_ax, ext_fl, cfg_ax, cfg_fl, spk, dcc_ax, exit_reason,
         bda_equip, bda_conv, bda_crtc, bda_kb_lo, bda_kb_hi, dcc_bx) = r

        def check(label, got, want, fmt="0x{:04x}"):
            nonlocal ok
            good = got == want
            ok = ok and good
            print(f"  {'ok  ' if good else 'FAIL'} {label:<38} "
                  f"got {fmt.format(got)}  want {fmt.format(want)}")

        print(f"guest exit reason {exit_reason} (6 = X86_EXIT_HALT, the HLT)")
        if exit_reason != 6:
            print("FAIL: the guest did not reach its HLT")
            ok = False

        print("INT 11h / INT 12h / INT 15h, as the guest saw them:")
        check("INT 11h equipment word", equip, EXPECT_EQUIP)
        # Spelled out separately: a wrong TOTAL says nothing about which field
        # is wrong, and the video field is the one that carries "CGA colour".
        check("  ...video field (bits 4-5)", (equip >> 4) & 3, 2, "{}")
        check("  ...floppy present (bit 0)", equip & 1, 1, "{}")
        check("  ...coprocessor (bit 1)", (equip >> 1) & 1, 0, "{}")
        check("  ...game adapter (bit 12)", (equip >> 12) & 1, 0, "{}")
        check("INT 12h conventional KB", conv, EXPECT_CONV_KB, "{}")
        check("INT 15h AH=88h extended KB", ext_ax, EXPECT_EXT_KB, "{}")
        check("INT 15h AH=88h CF (IRET frame)", ext_fl & CF, 0, "{}")
        check("INT 15h AH=C0h CF (IRET frame)", cfg_fl & CF, CF, "{}")
        check("INT 15h AH=C0h AH", (cfg_ax >> 8) & 0xFF, 0x86)

        # INT 10h AH=1Ah, the display combination code. This is not a nicety:
        # it is the FIRST of the four probes in Wolf3D's VW_VideoID, and a
        # BIOS that answers AL=0 here sends the whole cascade down to the CGA
        # arm. Measured under gwemu on the pre-fix core: videocard came out 2
        # (CGAcard), VW_Startup called Quit("Improper video card!") before
        # CA_Startup ever ran, and every symptom after that -- no page file,
        # seeks on handle 0, SoundMode stuck at 0 -- was that one byte.
        print("INT 10h AH=1Ah display combination code:")
        check("  AL (1Ah = this BIOS has the call)", dcc_ax & 0xFF, 0x1A)
        check("  BL (active display, 08 = VGA colour)", dcc_bx & 0xFF, 0x08)
        check("  BH (alternate display, 00 = none)", (dcc_bx >> 8) & 0xFF, 0x00)

        # BL = OR of 1024 samples of port 0x61, BH = AND of the same. A bit
        # that ever went high is set in BL; a bit that ever went low is CLEAR
        # in BH. Both conditions, or the bit is stuck -- and a stuck bit is
        # what turns "wait until it flips" into a program that never returns.
        ever_high, ever_low = spk & 0xFF, (~(spk >> 8)) & 0xFF
        print("PC speaker feedback on port 0x61, 8192 guest samples:")
        check("bit 5 (PIT ch2 OUT) ever high", (ever_high >> 5) & 1, 1, "{}")
        check("bit 5 (PIT ch2 OUT) ever low", (ever_low >> 5) & 1, 1, "{}")
        check("bit 4 (refresh toggle) ever high", (ever_high >> 4) & 1, 1, "{}")
        check("bit 4 (refresh toggle) ever low", (ever_low >> 4) & 1, 1, "{}")
        # The bits we do NOT drive must still read back what was written, or a
        # read-modify-write of the gate bits silently turns the speaker off.
        check("bits 0-1 (gate + data) read back", ever_high & 3, 3, "{}")

        print("the same facts as MEMORY, which a program may peek instead:")
        check("0040:0010 equipment", bda_equip, EXPECT_EQUIP)
        check("0040:0013 memory size KB", bda_conv, EXPECT_CONV_KB, "{}")
        check("0040:0063 CRTC base port", bda_crtc, EXPECT_CRTC)
        check("0040:0080 keyboard ring start", bda_kb_lo, 0x001E)
        check("0040:0082 keyboard ring end", bda_kb_hi, 0x003E)

        # The two routes to the same number must not be able to disagree; that
        # is the whole reason the service reads the BDA rather than a constant.
        if conv != bda_conv:
            print("FAIL: INT 12h and 0040:0013 disagree about conventional "
                  "memory -- a program that peeks and one that calls would "
                  "size their heaps differently")
            ok = False

        print("PASS" if ok else "FAIL")
        return ok
    finally:
        runner.stop()


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    add_runner_args(p)
    sys.exit(0 if main(p.parse_args()) else 1)
