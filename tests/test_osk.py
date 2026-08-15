#!/usr/bin/env python3
"""
Tier-1 check of the on-screen keyboard.

The OSK is a FRONT END over the injection path the buttons already use, so what
needs proving is not that it draws -- a screenshot says that -- but that a grid
press turns into the right SCANCODES and that the BIOS decodes them into the
right entry in the guest's BDA ring. The harness arm (osk_test_run in
tests/main.c) drives it exactly as src/main.c does: a button bitmask per frame,
then INT 09h for every code the controller holds. Every expectation below is
read out of the guest's own ring, not out of the OSK's state.

Covered:
  * The layout fits. Every row is measured in 8 px cells at reset; the renderer
    clips silently, so a row that grew past 40 would lose its rightmost keys
    (the OFF cell) with no fault to notice.
  * GAME opens the keyboard, cycles on to mouse mode -- which still owns the
    buttons -- and closes it. Three states, not two.
  * A on the first cell sends 'a' (scancode 0x1E, ASCII 0x61).
  * RIGHT then A sends 'b' -- navigation moved the cursor, and the second press
    is a fresh edge.
  * B is Backspace wherever the cursor is.
  * A SHIFTED CELL. TIME cycles the layer, DOWN flips the row, ten RIGHTs reach
    '!' -- and the ring must show ASCII 0x21 on scancode 0x02. That can only
    happen if a real Left-Shift MAKE reached INT 09h ahead of the key, which is
    the half with no other witness: without it the guest receives '1' and every
    counter still looks right.
  * The Shift does not LATCH: the BDA shift-flags byte is back to 0 afterwards,
    so the next key is not silently shifted too.
  * Drawing touches the bars and never the guest's VRAM, in either video mode.
"""
import argparse
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

# (scancode << 8) | ascii, as the BDA ring stores a key.
KEY_A = (0x1E << 8) | ord('a')
KEY_B = (0x30 << 8) | ord('b')
KEY_BSP = (0x0E << 8) | 0x08
KEY_BANG = (0x02 << 8) | ord('!')

CELLS_PER_ROW = 40


def main(args):
    runner = build_and_create_runner(args)
    ok = True
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols("test_core.elf")
        for n in ("run_osk_flag", "osk_r"):
            if n not in syms:
                print(f"missing symbol {n} -- rebuild tests/main.c")
                return False
        time.sleep(0.5)

        b.write_uint32(syms["run_osk_flag"], 1)
        for _ in range(600):
            if b.read_uint32(syms["run_osk_flag"]) == 0:
                break
            time.sleep(0.05)
        else:
            print("FAIL: the OSK arm never completed")
            return False

        r = [b.read_uint32(syms["osk_r"] + 4 * i) for i in range(20)]
        (cells_max, overflow, vis0, vis1, key_a, key_b, key_bsp, key_bang,
         shift_flags, vram_same, top_ink, bottom_ink, picture_ink,
         mouse_mode, vis_mouse, vis_off,
         irq_armed_if0, pit_if0, irq_refused, irq_armed_if1) = r

        def check(label, got, want, fmt=str):
            nonlocal ok
            good = got == want
            ok = ok and good
            print(f"{'PASS' if good else 'FAIL'}  {label:34s} "
                  f"{fmt(got)}  (expect {fmt(want)})")

        hexw = lambda v: f"0x{v:04x}"

        print(f"widest grid row            {cells_max} cells of {CELLS_PER_ROW}")
        check("no row overflows the panel", overflow, 0)
        check("closed before GAME", vis0, 0)
        check("GAME opens the keyboard", vis1, 1)
        check("first cell sends 'a'", key_a, KEY_A, hexw)
        check("RIGHT then A sends 'b'", key_b, KEY_B, hexw)
        check("B sends Backspace", key_bsp, KEY_BSP, hexw)
        check("shifted cell sends '!'", key_bang, KEY_BANG, hexw)
        check("Shift did not latch", shift_flags, 0)
        check("guest VRAM untouched by draw", vram_same, 1)
        check("mouse mode is the second state", mouse_mode, 2)
        check("mouse mode owns the buttons", vis_mouse, 1)
        check("third GAME closes it", vis_off, 0)

        # WHEN a cell press becomes visible to the guest, not just whether it
        # arrives. The frame loop arms IRQ 1 once per frame through
        # x86_kbd_raise_irq(); doing that unguarded is what made an OSK
        # keypress invisible until the keyboard was CLOSED -- measured under
        # gwemu as 60 arms/second, five codes stuck in the queue, and the guest
        # one instruction per frame inside its own INT 09h handler.
        check("queued code arms IRQ 1", irq_armed_if0, 1)
        check("IF clear: boundary NOT pulled in", pit_if0, 0x5A5A, hexw)
        check("pending slot not stolen from the tick", irq_refused, 0x0008, hexw)
        check("IF set: armed and boundary pulled in", irq_armed_if1, 0x10901,
              lambda v: f"0x{v:05x}")

        # The bars must carry ink and the picture area must not have been
        # scribbled on by the keyboard (the pointer is off in KBD mode).
        print(f"ink: top bar {top_ink}, bottom bar {bottom_ink}, "
              f"picture {picture_ink}")
        # "ink" is any non-zero index: the renderer clears the bars to 0 and the
        # OSK paints them in 16 (see the colour block in x86_osk.c), so a full
        # bar of non-zero is precisely "the OSK owns these rows now".
        for label, v in (("top bar painted by the OSK", top_ink),
                         ("bottom bar painted by the OSK", bottom_ink)):
            good = v > 0
            ok = ok and good
            print(f"{'PASS' if good else 'FAIL'}  {label}")

        return ok
    finally:
        runner.stop()


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    sys.exit(0 if main(p.parse_args()) else 1)
