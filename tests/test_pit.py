#!/usr/bin/env python3
"""
Tier-1 check of PIT channel 0 -- the SYSTEM TIMER's divisor -- driven by GUEST
code executing its own OUT instructions.

WHY THIS EXISTS. Channel 0 is the guest's entire sense of elapsed time. Until it
was modelled, `pit_write_control()` returned early for every channel but 2 and
port 0x40 was not in the write switch at all (x86_io.c), so a guest that
programmed its own timer rate got the 18.2065 Hz boot rate anyway. Wolfenstein 3D
does exactly that -- measured on our own target, `OUT 43h,36h` then `4Bh`/`21h`
to port 0x40, i.e. divisor 0x214B = 140 Hz -- and its tic counter, which is what
paces turning and menu key-repeat, therefore ran at 18.2/140 = 13% of real time.
That is a clock defect, not a keyboard one, and it produces exactly the reported
"rotating is slow and menu movement is sluggish".

It runs guest INSTRUCTIONS rather than calling the port handler from the host,
because the subject is what a program's instruction stream does to the machine.

Covered:
  * A cold machine reads 65536 -- the boot divisor -- before anything is written.
  * The real Wolf3D sequence lands 0x214B, and both halves of the lo/hi pair are
    consumed in the right order.
  * A LO-BYTE-ONLY access mode (control 0x14) leaves the HIGH byte alone. Storing
    the access field blindly, or treating every write as a lo/hi pair, silently
    halves or doubles the guest's clock.
  * Programming channel 2 -- the SPEAKER -- does not disturb channel 0, and vice
    versa. One control-word decoder serves both and a channel mix-up here retunes
    the timer every time a game beeps.
  * A reload of 0 means 65536, not 0. The frame loop divides by this value.
  * THE COUNTERS COUNT, AND CAN BE READ BACK (phase E). The guest runs Michael
    Abrash's Precision Zen Timer sequence verbatim -- the one TOPBENCH times
    all five of its suites with: program channel 2 mode 2 / count 65536, ungate
    it at port 0x61 bit 0, LATCH with OUT 43h,80h and read the lo/hi pair off
    0x42; then PZTimerOn/PZTimerOff on channel 0 (OUT 43h,34h, a 0/0 count,
    work, OUT 43h,00h, two reads of 0x40, NEG AX); then the OVERFLOW test,
    OCW3 0x0A to port 0x20 followed by IN AL,20h for the 8259's IRR.
    Asserted: the channel 2 count MOVES and moves DOWN across three reads, the
    channel 0 elapsed count is non-zero and plausible, and the IRR does NOT
    report IRQ 0 pending. That last one is the whole 65535 bug: port 0x20 was
    not in the read switch, floated to 0xFF, and PZTimerCount() therefore took
    its overflow path and returned 0xFFFFFFFF, which TOPBENCH truncates to a
    word and prints as 65535 (references/TOPBENCH/ZTIMER11/PZTIMER.ASM:488-493,
    TOPB_DAT.PAS:31-35).

NEGATIVE CONTROL (`--negative`): rebuilds the core with the `case 0x40:` arm
deleted from `x86_port_write()` -- the exact state of the tree before this fix --
re-runs, and requires the result to be FAIL. The source is restored in a
`finally`, the way scripts/trace_diff.py restores x86_trace.inc. It is here
because a check that cannot fail proves nothing, and this area's bug was a
silently dropped write: the exact shape a green-by-construction test hides.
Measured: the mutated build reports divisor 65536 / 18.207 Hz where the guest
asked for 8523 / 140 Hz, and five assertions fire.

`--negative-clock` is phase E's own control: it deletes the port 0x40 / 0x42 /
0x20 READ arms, restoring the tree exactly as it was -- a channel 2 counter that
answered with a static reload byte, a channel 0 counter that was not answered at
all, and a PIC that floated 0xFF and so declared overflow on every measurement.
"""
import argparse
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

WOLF3D_DIVISOR = 0x214B         # 8523 -> 140.0 Hz off the 1.193182 MHz input
LO_ONLY_DIVISOR = 0x21FF        # high byte kept, low byte replaced
SPEAKER_DIVISOR = 0x1234
BOOT_DIVISOR = 65536
PIT_HZ = 1193182


def hz(div):
    return PIT_HZ / div


def main(args):
    runner = build_and_create_runner(args)
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols("test_core.elf")
        for n in ("run_pit_flag", "pit_r", "ztimer_r"):
            if n not in syms:
                print(f"missing symbol {n} -- rebuild tests/main.c")
                return False
        time.sleep(0.5)

        b.write_uint32(syms["run_pit_flag"], 1)
        for _ in range(600):
            if b.read_uint32(syms["run_pit_flag"]) == 0:
                break
            time.sleep(0.05)
        else:
            print("FAIL: the PIT arm never completed")
            return False

        r = [b.read_uint32(syms["pit_r"] + 4 * i) for i in range(10)]
        (cold, wolf, writes_a, lo_only, after_spk, spk, reload0, writes_all,
         exit_a, exit_b) = r

        print(f"cold machine        divisor {cold:6d}  "
              f"({hz(cold or 1):8.3f} Hz)   expect {BOOT_DIVISOR} / 18.207 Hz")
        print(f"OUT 43,36 / 4B / 21 divisor {wolf:6d}  "
              f"({hz(wolf or 1):8.3f} Hz)   expect {WOLF3D_DIVISOR} / 140.0 Hz")
        print(f"  port 0x40 writes  {writes_a}   (expect 2)")
        print(f"OUT 43,14 / FF      divisor {lo_only:6d}  "
              f"({hz(lo_only or 1):8.3f} Hz)   expect {LO_ONLY_DIVISOR}")
        print(f"after a CH2 program divisor {after_spk:6d}   "
              f"expect {LO_ONLY_DIVISOR} (unchanged)")
        print(f"  speaker divisor   {spk}   (expect {SPEAKER_DIVISOR})")
        print(f"OUT 43,36 / 00 / 00 divisor {reload0:6d}   "
              f"expect {BOOT_DIVISOR} -- a reload of 0 is the full period")
        print(f"  port 0x40 writes  {writes_all}   (expect 5)")
        print(f"exit reasons A/B: {exit_a}/{exit_b}   (6 = HLT)")

        ok = True

        def fail(msg):
            nonlocal ok
            print("FAIL: " + msg)
            ok = False

        if cold != BOOT_DIVISOR:
            fail(f"a machine that has never been programmed reports {cold}, "
                 f"not {BOOT_DIVISOR} -- every guest's clock starts wrong")
        if wolf != WOLF3D_DIVISOR:
            fail(f"the Wolf3D program left {wolf} ({hz(wolf or 1):.1f} Hz), "
                 f"expected {WOLF3D_DIVISOR} (140 Hz). At {BOOT_DIVISOR} the "
                 f"write was DROPPED, which is the original defect")
        if writes_a != 2:
            fail(f"port 0x40 saw {writes_a} writes, expected 2 -- the guest "
                 f"wrote a lo/hi pair")
        if lo_only != LO_ONLY_DIVISOR:
            fail(f"lo-byte-only access left {lo_only}, expected "
                 f"{LO_ONLY_DIVISOR}: the high byte must survive")
        if after_spk != LO_ONLY_DIVISOR:
            fail(f"programming the SPEAKER moved the timer divisor to "
                 f"{after_spk} -- the control word's channel field is being "
                 f"ignored, so every beep retunes the guest's clock")
        if spk != SPEAKER_DIVISOR:
            fail(f"the speaker divisor is {spk}, expected {SPEAKER_DIVISOR} -- "
                 f"channel 0 support broke channel 2")
        if reload0 != BOOT_DIVISOR:
            fail(f"a reload of 0 reports {reload0}; it means the full 16-bit "
                 f"period ({BOOT_DIVISOR}). A 0 here is a divide by zero in "
                 f"the frame loop")
        if writes_all != 5:
            fail(f"port 0x40 saw {writes_all} writes across the run, expected 5")
        for name, val in (("A", exit_a), ("B", exit_b)):
            if val != 6:
                fail(f"phase {name} exited with reason {val}, not 6 (HLT) -- "
                     f"the guest did not reach the end of its code")

        # ---- phase E: the Zen Timer's own sequence ------------------------
        z = [b.read_uint32(syms["ztimer_r"] + 4 * i) for i in range(8)]
        ch2_a, ch2_b, ch0_elapsed, irr, ch2_c, exit_e, ticks = z[:7]

        print(f"\nphase E -- the Precision Zen Timer's sequence")
        print(f"  ch2 latched reads   {ch2_a:5d} -> {ch2_b:5d} -> {ch2_c:5d}"
              f"   (a 65536 count, so it counts DOWN)")
        print(f"  ch0 elapsed counts  {ch0_elapsed}"
              f"  = {ch0_elapsed / PIT_HZ * 1e6:.1f} usec of guest time")
        print(f"  8259 IRR            0x{irr:02X}   "
              f"(bit 0 = IRQ 0 pending = 'the timer overflowed')")
        print(f"  free-running ticks  {ticks}")
        print(f"  exit reason         {exit_e}   (6 = HLT)")

        if exit_e != 6:
            fail(f"phase E exited with reason {exit_e}, not 6 (HLT)")
        if ch2_a == ch2_b or ch2_b == ch2_c:
            fail(f"the channel 2 counter did not MOVE between latched reads "
                 f"({ch2_a} / {ch2_b} / {ch2_c}). A counter that stands still "
                 f"is what made every Zen Timer measurement nonsense")
        # A 65536-count mode 2 channel counts DOWN, and 4000 LOOP iterations
        # are nowhere near a wrap, so both steps must be strictly decreasing.
        if not (ch2_a > ch2_b > ch2_c):
            fail(f"the channel 2 counter is not counting DOWN monotonically: "
                 f"{ch2_a} -> {ch2_b} -> {ch2_c}. A timer that reads a counter "
                 f"going backwards computes garbage either way")
        if ch0_elapsed == 0:
            fail("the channel 0 elapsed count is 0 -- PZTimerOff's NEG AX saw "
                 "a counter that had not moved since PZTimerOn loaded it")
        if ch0_elapsed >= 65536 // 2:
            fail(f"the channel 0 elapsed count is {ch0_elapsed}, over half the "
                 f"period -- 8000 LOOP iterations cannot be that long, so the "
                 f"lo/hi read pair is out of order or unlatched")
        # 8000 LOOP iterations is ~8000 instructions; PIT ticks are instructions
        # converted at the emulated CPU speed (x86_io.c). The bare-metal harness
        # never publishes guest_kips, so the 5500 kips nominal applies:
        # ~8000 * 1193182 / 5.5e6 ~= 1700 ticks. An order of magnitude either
        # way is fine -- what is NOT fine is 0, a sentinel, or a constant.
        if not (100 <= ch0_elapsed <= 20000):
            fail(f"the channel 0 elapsed count is {ch0_elapsed}; ~1700 ticks is "
                 f"what 8000 guest instructions are worth at the nominal rate")
        if irr & 1:
            fail(f"the 8259 IRR reads 0x{irr:02X}: bit 0 says IRQ 0 is PENDING, "
                 f"so PZTimerCount() reports overflow and returns 0xFFFFFFFF -- "
                 f"which is the 65535 usec TOPBENCH prints for every suite")
        if irr == 0xFF:
            fail("the IRR read 0xFF, which is an unmodelled port floating high, "
                 "not an answer from a PIC")
        if ticks == 0:
            fail("the free-running PIT tick count is 0 after a whole phase of "
                 "guest execution -- guest time is not advancing at all")

        print("\nPASS" if ok else "\nFAIL")
        return ok
    finally:
        runner.stop()


# The line that carries the whole fix. Deleting it restores the defect exactly:
# the control word still latches an access mode, and the divisor bytes go
# nowhere.
MUTATE_OUT = "    case 0x40:\n        pit_write_ch0((uint8_t)value);\n        break;\n"


# Phase E's own mutation: the three READ arms. Deleting them is exactly the
# tree before this fix -- 0x42 answered with a static reload byte, 0x40 and 0x20
# were not answered at all and floated to 0xFF.
MUTATE_READ = """    /* The counters. Not the reload value: see the PIT clock block above. */
    case 0x40:
        return pit_read_data(0);
    case 0x42:
        return pit_read_data(2);
    case 0x20:
        return (pic_ocw3_read == 3) ? 0u : pic_read_irr();
    case 0x21:
        return pic_imr;
"""
MUTATE_READ_WITH = """    case 0x42:
        return x86_io.pit2_reload & 0xFF;
"""


def _mutate_and_require_failure(args, target, replacement, what):
    import pathlib
    src = pathlib.Path("x86_io.c")
    original = src.read_text()
    if target not in original:
        print(f"FAIL: the mutation target ({what}) is not in x86_io.c -- this "
              f"control would pass without testing anything")
        return False
    try:
        src.write_text(original.replace(target, replacement, 1))
        print(f"-- negative control: rebuilt with {what} --")
        args.negative = False
        args.negative_clock = False
        failed = not main(args)
    finally:
        src.write_text(original)
        print("-- x86_io.c restored --")
    if failed:
        print("\nPASS (negative control): the test rejects the defective core")
        return True
    print("\nFAIL (negative control): the test PASSED without the fix, so it "
          "is not measuring anything")
    return False


def negative_control_clock(args):
    """Rebuild with the PIT/PIC READ arms deleted and require a FAIL."""
    return _mutate_and_require_failure(
        args, MUTATE_READ, MUTATE_READ_WITH,
        "the port 0x40 / 0x42 / 0x20 READ arms DELETED")


def negative_control(args):
    """Rebuild without the fix and require this test to FAIL."""
    import pathlib
    src = pathlib.Path("x86_io.c")
    original = src.read_text()
    if MUTATE_OUT not in original:
        print("FAIL: the mutation target is not in x86_io.c -- this control "
              "would pass without testing anything")
        return False
    try:
        src.write_text(original.replace(MUTATE_OUT, "", 1))
        print("-- negative control: rebuilt with the port 0x40 case DELETED --")
        args.negative = False
        failed = not main(args)
    finally:
        src.write_text(original)
        print("-- x86_io.c restored --")
    if failed:
        print("\nPASS (negative control): the test rejects the defective core")
        return True
    print("\nFAIL (negative control): the test PASSED without the fix, so it "
          "is not measuring anything")
    return False


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    p.add_argument("--negative", action="store_true",
                   help="rebuild with the 0x40 WRITE arm deleted, require FAIL")
    p.add_argument("--negative-clock", action="store_true",
                   help="rebuild with the PIT/PIC READ arms deleted, require FAIL")
    args = p.parse_args()
    if args.negative:
        sys.exit(0 if negative_control(args) else 1)
    if args.negative_clock:
        sys.exit(0 if negative_control_clock(args) else 1)
    sys.exit(0 if main(args) else 1)
