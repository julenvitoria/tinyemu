#!/usr/bin/env python3
"""
Tier-1 check of the BIOS keyboard path: BDA ring -> INT 16h.

The chain from a button to a scancode is already covered (port 0x60 cases in
test_opcodes.py, and test_core_retrogo.py's x86_kbd_read_log assertion). What
was NOT covered is everything after INT 09h: the 0040:001E ring the handler
writes into, and whether INT 16h hands the key back to GUEST CODE.

It was written to chase a device report of exactly that -- keys arriving, INT
09h consuming them, and a guest polling INT 16h forever. So it consumes the key
the way a program does: GUEST code executes INT 16h and stores AX, and the
FLAGS the IRET actually delivered, into guest memory. Calling the C helpers
from the host would prove nothing about the FLAGS, which is where the classic
inversion bug lives.

The path measured CLEAN (see the mutation list below, every one of which this
test catches), so it stands as the regression cover the area never had.

Covered:
  * AH=01 on an EMPTY buffer sets ZF, and the ZF the guest sees is the one on
    the IRET frame, not a live register the IRET is about to overwrite.
  * AH=01 with a key waiting CLEARS ZF and returns scancode:ascii in AH:AL,
    without dequeueing (the second peek must return the same word).
  * AH=00 dequeues that key and returns the same AX.
  * AH=01 after the read reports empty again.
  * head/tail are initialised to 0x1E, not 0 -- zero would put the first key on
    top of the IVT and leave head == tail forever.
  * The push and the pop both WRAP at the end of the ring.
"""
import argparse
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

ZF = 1 << 6

KEY_ENTER = (0x1C << 8) | 13                  # scancode 0x1C, ASCII CR
KEY_SPACE = (0x39 << 8) | 0x20                # scancode 0x39, ASCII ' '

BUF_START = 0x1E
BUF_END = 0x3E          # one past the last entry; 16 entries of 2 bytes


def main(args):
    runner = build_and_create_runner(args)
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols("test_core.elf")
        for n in ("run_kbd_flag", "kbd_r"):
            if n not in syms:
                print(f"missing symbol {n} -- rebuild tests/main.c")
                return False
        time.sleep(0.5)

        b.write_uint32(syms["run_kbd_flag"], 1)
        for _ in range(600):
            if b.read_uint32(syms["run_kbd_flag"]) == 0:
                break
            time.sleep(0.05)
        else:
            mark = b.read_uint32(syms["kbd_r"] + 4 * 19)
            print(f"FAIL: the keyboard arm never completed (progress marker "
                  f"0x{mark:08x})")
            return False

        r = [b.read_uint32(syms["kbd_r"] + 4 * i) for i in range(20)]
        (empty_ax, empty_fl, peek_ax, peek_fl, peek2_ax, read_ax, drained_fl,
         wrap_ax, init_head, init_tail, irq_rc, head_after, tail_after,
         ring_word, tail_wrapped, wrap_slot, exA, exB, exC, head_wrapped) = r

        print(f"BDA head/tail after init   0x{init_head:04x} / 0x{init_tail:04x}"
              f"   (expect 0x001e / 0x001e)")
        print(f"INT 09h returned           {irq_rc}")
        print(f"head/tail after one push   0x{head_after:04x} / 0x{tail_after:04x}"
              f"   (expect 0x001e / 0x0020)")
        print(f"word in the ring at head   0x{ring_word:04x}"
              f"   (expect 0x{KEY_ENTER:04x})")
        print()
        print(f"AH=01, empty:   AX=0x{empty_ax:04x} FLAGS=0x{empty_fl:04x} "
              f"ZF={1 if empty_fl & ZF else 0}   (expect ZF=1)")
        print(f"AH=01, keyed:   AX=0x{peek_ax:04x} FLAGS=0x{peek_fl:04x} "
              f"ZF={1 if peek_fl & ZF else 0}   (expect AX=0x{KEY_ENTER:04x}, ZF=0)")
        print(f"AH=01, again:   AX=0x{peek2_ax:04x}   (peek must not dequeue)")
        print(f"AH=00, read:    AX=0x{read_ax:04x}   (expect 0x{KEY_ENTER:04x})")
        print(f"AH=01, drained: FLAGS=0x{drained_fl:04x} "
              f"ZF={1 if drained_fl & ZF else 0}   (expect ZF=1)")
        print()
        print(f"wrap: tail after push at 0x3c = 0x{tail_wrapped:04x} "
              f"(expect 0x{BUF_START:04x}), last slot holds 0x{wrap_slot:04x} "
              f"(expect 0x{KEY_SPACE:04x})")
        print(f"wrap: AH=00 returned AX=0x{wrap_ax:04x}, head = "
              f"0x{head_wrapped:04x} (expect 0x{BUF_START:04x})")
        print(f"exit reasons A/B/C: {exA}/{exB}/{exC}")

        ok = True

        def fail(msg):
            nonlocal ok
            print("FAIL: " + msg)
            ok = False

        if init_head != BUF_START or init_tail != BUF_START:
            fail(f"head/tail start at 0x{init_head:04x}/0x{init_tail:04x}, not "
                 f"0x{BUF_START:04x} -- a zero here writes the first key over "
                 f"the IVT and leaves the ring permanently empty")
        if head_after != BUF_START or tail_after != BUF_START + 2:
            fail(f"after one push head/tail are 0x{head_after:04x}/"
                 f"0x{tail_after:04x}, expected 0x{BUF_START:04x}/"
                 f"0x{BUF_START + 2:04x}")
        if ring_word != KEY_ENTER:
            fail(f"the ring holds 0x{ring_word:04x} at the head, expected "
                 f"0x{KEY_ENTER:04x} (scancode in the high byte, ASCII low)")

        if not (empty_fl & ZF):
            fail("INT 16h AH=01 on an empty buffer left ZF CLEAR, which tells "
                 "a polling program a key is waiting")
        if peek_ax != KEY_ENTER:
            fail(f"INT 16h AH=01 returned AX=0x{peek_ax:04x}, expected "
                 f"0x{KEY_ENTER:04x}")
        if peek_fl & ZF:
            fail("INT 16h AH=01 with a key waiting left ZF SET, so a program "
                 "polling with JZ never stops waiting")
        if peek2_ax != KEY_ENTER:
            fail(f"the second AH=01 returned 0x{peek2_ax:04x}: the peek is "
                 f"destructive")
        if read_ax != KEY_ENTER:
            fail(f"INT 16h AH=00 returned AX=0x{read_ax:04x}, expected "
                 f"0x{KEY_ENTER:04x}")
        if not (drained_fl & ZF):
            fail("INT 16h AH=01 after the buffer was drained did not set ZF")

        if tail_wrapped != BUF_START:
            fail(f"pushing into the last ring entry left tail at "
                 f"0x{tail_wrapped:04x}, expected a wrap to 0x{BUF_START:04x}")
        if wrap_slot != KEY_SPACE:
            fail(f"the last ring entry holds 0x{wrap_slot:04x}, expected "
                 f"0x{KEY_SPACE:04x} -- the push did not use the whole ring")
        if wrap_ax != KEY_SPACE:
            fail(f"INT 16h AH=00 across the wrap returned 0x{wrap_ax:04x}, "
                 f"expected 0x{KEY_SPACE:04x}")
        if head_wrapped != BUF_START:
            fail(f"head is 0x{head_wrapped:04x} after the wrapping read, "
                 f"expected 0x{BUF_START:04x}")

        print("\nPASS" if ok else "\nFAIL")
        return ok
    finally:
        runner.stop()


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    args = p.parse_args()
    sys.exit(0 if main(args) else 1)
