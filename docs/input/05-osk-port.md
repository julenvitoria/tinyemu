# The on-screen keyboard as it exists HERE

Our record of the port. The design reasoning it is a port OF lives in
[`03-onscreen-keyboard.md`](03-onscreen-keyboard.md) and
[`02-button-mapping.md`](02-button-mapping.md), vendored from the author's
previous DOS port; this file is what is true of THIS tree.

## On-screen keyboard — shipped; mouse UI — shell only

`x86_osk.c`. The UI is a port of the author's previous DOS port
(`docs/input/03-onscreen-keyboard.md`, vendored): three layers of two 8x8 rows
in the letterbox bars, alphabetical rather than QWERTY, sticky Shift/Ctrl/Alt, a
38-character echo of what was sent, and one button for everything —
**GAME cycles OFF -> keyboard -> mouse -> OFF**, on every unit, which is why
`pad_scancode[6]` is deliberately 0. Everything under the layout is ours: a cell
press becomes XT make/break codes through `x86_kbd_scancode()` plus IRQ 1, the
path `test_keyboard.py` already measured.

What was NOT ported, because our renderer makes it unnecessary: the two-frame
repaint counter and the BDA video-mode watch. `x86_video_render()` rewrites all
240 rows every frame, so the OSK is redrawn every frame and both framebuffers
converge by construction.

Two things had to change underneath:

- **INT 09h now decodes Shift** (`x86_bios.c`) and maintains `0040:0017`, so
  INT 16h AH=02 reports the real state. Without it a cell labelled `!` puts `1`
  in the BDA ring and every counter still looks correct.
- **IRQ 1 is re-raised inside the slice loop** (`src/main.c`). The BIOS handler
  takes ONE code per interrupt and a shifted keypress is up to six; at one IRQ
  per frame a keystroke took 100 ms to arrive. Delivery happens in-guest, so
  this does not shorten the slice.

Measured, all under gwemu:

| What | Result |
|---|---|
| `tests/test_osk.py` (tier 1) | 12/12: `a`, `b`, Backspace, the shifted `!` (0x0221 in the BDA ring), no latched Shift afterwards, the 3-state cycle, guest VRAM untouched, widest row 38 cells of 40 |
| `tests/test_core_retrogo.py` (tier 2) | GAME opens the OSK and pushes NO pad scancode; A then puts 0x1E on port 0x60 in the guest; cycle 1 -> 2 -> 0 |
| PAUSE overlay coexistence | `timelines/osk-with-pause.tl` under `probe.py --faults`: menu opens over the keyboard, frames resume with PC in `draw_row`, CFSR/HFSR clean |
| Screenshots | `build/artifacts/osk_topbench.png` (over a text-mode screen), `osk_alleycat.png` (CGA mode 4), `osk_mouse.png` (pointer + status line) |
| Cost | `x86_osk.o` = 3,331 B text + 132 B bss. RAM_EMU headroom after it: **4,960 B** (`__CORE_BSS_END__` 0x24056CA0) |

**MOUSE MODE IS A UI SHELL WITH NOTHING LISTENING, and that is deliberate.**
The pointer moves over a 640x200 virtual screen with the ported acceleration
ramp, the status line tracks it, B/A count as clicks and the arrow is drawn over
the guest picture — but **there is no INT 33h driver**, so no guest can ask
where it is. Half-building one would be worse than not: INT 33h has no error
channel, so an unimplemented function returns with every register unchanged and
a guest cannot tell it from success. What a real one needs, none of which
exists yet:

- [ ] INT 33h vector taken by the HLE table (`0F 0B 33`), plus its own service
      block: 0000h reset/present, 0001h/0002h show/hide with the nesting
      counter, 0003h position+buttons, 0004h set position, 0005h/0006h
      press/release counts since the last call, 0007h/0008h ranges, 000Bh
      relative motion, 000Ch/0014h event callbacks, 000Fh mickey ratio.
- [ ] A callback path. 000Ch registers a FAR routine the driver must CALL on
      motion or a click — the emulator has to synthesise a guest far call at an
      instruction boundary and unwind it, which nothing here does today.
- [ ] Virtual extent tied to the video mode rather than a constant, and the
      cursor's guest-side show/hide ANDed with the user's render toggle.

