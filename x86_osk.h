#ifndef X86_OSK_H
#define X86_OSK_H

#include <stdint.h>

/*
 * On-screen keyboard, and the mouse UI shell behind it.
 *
 * Design record: docs/input/03-onscreen-keyboard.md and
 * docs/input/02-button-mapping.md. The layout, the navigation, the sticky
 * modifiers, the echo line and the GAME cycle are a port of the previous DOS
 * port's dos_osk.c / dos_mouse_ui.c (the user's own code, whose retro-go
 * integration and UI were the parts worth keeping); the injection underneath is
 * OURS -- XT scancodes into x86_kbd_scancode(), which is the path
 * tests/test_keyboard.py and x86_kbd_read_log already measure.
 *
 * WHAT DELIBERATELY DID NOT COME ACROSS. Their key table carried SDL keysyms,
 * because their emulator's BIOS decoded a word format over an injection queue.
 * We have no such format: our keyboard is modelled at the 8042, so a key IS a
 * make/break pair and a shifted key IS a Left-Shift make wrapped round it. The
 * layout is the same table; every cell's payload changed.
 *
 * BUTTONS ARE A BITMASK, NOT odroid_gamepad_state_t. This file is linked into
 * the bare-metal harness too (Makefile), which is deliberately free of every
 * retro-go header -- that is what keeps tier 1 a clean measurement tier. src/
 * main.c already builds exactly this mask from the symbolic ODROID_INPUT_*
 * values, so the mapping stays where the compiler resolves it.
 */
#define X86_OSK_UP     (1u << 0)
#define X86_OSK_DOWN   (1u << 1)
#define X86_OSK_LEFT   (1u << 2)
#define X86_OSK_RIGHT  (1u << 3)
#define X86_OSK_A      (1u << 4)
#define X86_OSK_B      (1u << 5)
#define X86_OSK_GAME   (1u << 6)   /* ODROID_INPUT_START -- the OSK's door   */
#define X86_OSK_TIME   (1u << 7)   /* ODROID_INPUT_SELECT                    */
#define X86_OSK_START  (1u << 8)   /* ODROID_INPUT_X, zelda units only       */
#define X86_OSK_SELECT (1u << 9)   /* ODROID_INPUT_Y, zelda units only       */

/* Clear all state. Call once before the frame loop, and on a guest reset. */
void x86_osk_reset(void);

/* True while the OSK owns the buttons -- in EITHER non-off mode. The caller
   consults this and must not care which of the two it is. */
int x86_osk_visible(void);

/* GAME: advance one step round  off -> keyboard -> mouse -> off. */
void x86_osk_cycle(void);

/* Consume one frame of button state. Edge detection is internal, so this must
   be called exactly once per frame and only while visible. */
/* Host clock in milliseconds, for typematic repeat when a direction is held.
   Call before x86_osk_input(). Never calling it leaves repeat disabled. */
void x86_osk_set_now_ms(uint32_t ms);

void x86_osk_input(uint32_t buttons);

/* Paint over the rendered frame. Call once per painted frame, AFTER
   x86_video_render() and before the swap. Unlike the previous port this repaints
   unconditionally: our renderer rewrites all 240 rows every frame, so there is
   nothing for a dirty-count to save and a stale bar is not possible. */
void x86_osk_draw(uint8_t *fb, uint32_t stride);

/* Announce something for about a second and a half over the top eight rows, in
   any mode including OFF. */
void x86_osk_toast(const char *s);

/* Host-readable, for the same reason every other stage of the input chain is:
   "the OSK did nothing" has several causes and only counters separate them. */
extern volatile uint32_t x86_osk_mode;      /* 0 off, 1 keyboard, 2 mouse */
extern volatile uint32_t x86_osk_presses;   /* grid keys pressed          */
extern volatile uint32_t x86_osk_codes;     /* scancodes injected         */
extern volatile uint32_t x86_osk_last_key;  /* (scancode << 8) | ascii    */
/* The widest grid row, in 8 px cells, measured at reset -- and 1 if any row is
   past the 40 that fit. The renderer clips silently, so a row that grew loses
   its rightmost keys (the OFF cell, in every row that has one) with no fault to
   notice; tests/test_osk.py asserts on these rather than on a comment. */
extern volatile uint32_t x86_osk_row_cells_max;
extern volatile uint32_t x86_osk_row_overflow;
extern volatile uint32_t x86_mouse_clicks;  /* button edges the mouse UI made */

#endif /* X86_OSK_H */
