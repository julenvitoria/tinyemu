<!--
VENDORED DESIGN RECORD, from the author's previous DOS port (8086tiny on
game-and-watch-retro-go-sd). Kept verbatim below the line so the reasoning it
records cannot drift; it is a RECORD, not a specification of this tree.

Read it with these substitutions -- this port's front end is a port of that
port's UI, but nothing under it is shared:

  * dos_osk.c / dos_mouse_ui.c  ->  x86_osk.c (both, in one file)
  * dos_input.c's routing       ->  poll_input() in src/main.c
  * SDL key words + INT 7h      ->  XT scancodes into x86_kbd_scancode() and
                                    IRQ 1, decoded by x86_bios_kbd_irq()
  * dos_video.c's letterbox     ->  x86_video.c, which repaints EVERY row every
                                    frame -- so this tree has no repaint counter
                                    and no BDA video-mode watch
  * mode 12h / VBE 320x240      ->  not implemented here; CGA modes 3 and 4 only
  * INT 33h mouse driver        ->  DOES NOT EXIST here. Mouse mode is a UI
                                    shell with nothing listening; see STATUS.md

It lived in references/, which is gitignored, so anything the tree depends on
had to come in properly. Original provenance and licence: the user's own code.
-->

# On-screen keyboard

> **SHIPPED.** `Core/Src/porting/dos/dos_osk.c` + `dos_osk.h`, wired from
> `dos_input.c` (button routing) and `main_dos.c` (one `dos_osk_draw()` after the
> blit). This document is now part design record, part correction: the sections
> below are annotated where what shipped differs from what was planned, and the
> "What shipped" section at the end is authoritative.

Eight usable buttons ([`02-button-mapping.md`](02-button-mapping.md)) against a
machine whose primary interface is a command prompt. The OSK is how that gap
closes.

## The space we have

[`../video/06-framebuffer-clut.md`](../video/06-framebuffer-clut.md) reserves the
letterbox bars — every supported mode is 200 lines against a 240-line panel:

```
  rows   0.. 19    black bar        <- 320 x 20, ours
  rows  20..219    guest image      <- blit writes here ONLY
  rows 220..239    black bar        <- 320 x 20, ours
```

**Is 40 rows enough?** Quantified, with the fonts from
[`../video/04-text-rendering.md`](../video/04-text-rendering.md):

| Font | Cells per bar | Keys per bar (1 char + 1 space) |
|---|---|---|
| 8x8 | 40 cols x 2 rows = 80 | ~40 |
| 4x8 | 80 cols x 2 rows = 160 | ~80 |

> **CORRECTION — the "1 char + 1 space" column halves the real capacity.** The
> separator space is only needed if the selection is drawn as a box around a key.
> It is drawn as a **foreground/background inversion of the cell itself**, which
> the very last section of this document already proposes — and an inverted cell
> is unambiguous with no gap at all. So a bottom bar at 8x8 holds **80 keys, not
> 40**, which is why what shipped fits `a`-`z`, twenty punctuation marks, F1-F10,
> Home/End/PgUp/PgDn, three modifiers and five command keys across three layers
> without ever feeling cramped.

A 20-row bar fits exactly two 8-pixel text rows with 4 rows of padding. One bar
at 8x8 holds ~40 keys — enough for `a`–`z` plus a few. Both bars hold ~80, which
covers a full keyboard.

**Honest answer, revised: it is comfortably enough with a layered layout.** The
original text below read "enough, but only just", on the halved capacity estimate
corrected above. In practice each row has spare cells.
A comfortable full QWERTY with legible 8x8 glyphs, visible modifier state, and
room for Enter/Backspace/Tab does not fit in 40 rows. Anyone expecting a
phone-style keyboard will be disappointed. What fits is a compact 2-row grid with
a layer toggle — usable for typing `cd games` and `cat.exe`, not for writing a
letter in WordPerfect.

I do not think that is a problem — the target is launching DOS programs, not
word processing — but the plan should not pretend otherwise.

## Recommended layout

**Keyboard in the bottom bar only; status in the top bar.**

Splitting the key grid across the top and bottom of the screen, with the game in
between, is disorienting and puts half the keyboard at the far end of the panel
from the thumbs. Keeping it contiguous and low is worth the tighter fit.

```
  top bar     KBD L1  SHF CTL ALT           <- mode, layer, latched modifiers
              dir                            <- echo of what has been sent
  bottom bar  abcdefghijklmSPCENTBSPTAB      <- layer 1 row 1
              nopqrstuvwxyzSHFCTLALTESCLYROFF   layer 1 row 2
```

> **The alphabet is alphabetical, not QWERTY.** QWERTY exists to place frequent
> letters under strong fingers; with one moving cursor there are no fingers, only
> distance, and the user is *looking up* each letter rather than touch-typing. On a
> lookup interface alphabetical order is strictly faster to search. QWERTY here
> would be cargo cult.

- **Bottom bar** — 2 rows x ~20 keys, 8x8 font, the active grid.
- **Top bar** — current mode, latched modifier state, and (optionally) an echo of
  recent keystrokes, which is genuinely useful when the guest is mid-redraw.

Layers, cycled by a button:

| Layer | Contents |
|---|---|
| 1 | lowercase letters |
| 2 | digits and common punctuation |
| 3 | function keys, Esc, Tab, arrows, Enter, Backspace |

> **OPEN:** Layer count and contents. Three is a guess. Fewer layers means more
> cramped rows; more layers means more cycling. Should be settled by trying it on
> hardware rather than on paper.

## Navigation

| Input | Action | Shipped? |
|---|---|---|
| D-pad | move selection within the grid (left/right wrap, up/down flips row) | yes |
| A | press the selected key | yes |
| B | Backspace (by far the most-used key; deserves a dedicated button) | yes |
| GAME | **cycle: off → keyboard → mouse → off** | changed |
| TIME | **cycle layer** (in the key grid) / step back to the key grid (in mouse mode) | changed |

**GAME and TIME swapped jobs.** The mode toggle had to land on a button that exists
on every unit, and GAME is the only spare one that does — see the DECIDED note in
[`02-button-mapping.md`](02-button-mapping.md). Layer cycling moved to TIME, which is
free while the keyboard is up. Both also have on-grid keys (`LYR`, `OFF`), so neither
is a single point of failure.

> **DECIDED (2026-08-06) — GAME is a THREE-state cycle, and mouse mode is a
> top-level mode.** `off → keyboard → mouse → off`.
>
> This does **not** disturb the reasoning above; it depends on it. Mouse mode
> needed a door, and the same argument that put the keyboard's door on GAME
> applies to it unchanged: a mario unit has no START, so any second button would
> have made mouse mode a zelda-only feature. Cycling one button costs at most two
> presses to reach any mode and can never strand the user, because the cycle
> always comes back round to off.
>
> **`MSE` is gone from the grid and its three cells went back to `` ` ``.** It was
> the previous route into mouse mode. GAME now reaches mouse mode in one press
> from anywhere with no grid navigation, so an `MSE` cell would be a strictly
> slower duplicate of a button that already exists — whereas backtick, which was
> dropped to make room for `MSE`, is reachable nowhere else on the device. The
> `OSK_ACT_MOUSE` action is kept in `dos_osk.c` (three lines) so re-adding the
> cell is trivial if the button route proves undiscoverable.
>
> Discoverability moved from the grid to the **status bar**, which is strictly
> better: the top line reads `GAME>MSE` on *every* layer, where `MSE` was one cell
> on one layer. The status bar now names all three states — blank bars (off),
> `KBD L<n>` (keyboard), `MOUSE <x>,<y>` (mouse) — so there is no state the user
> can be in without being told.
>
> **TIME's step-back from mouse mode to the key grid is retained deliberately.**
> GAME only goes forwards, so without it the way from mouse mode to the keyboard
> is two presses *through off*, which hands the buttons back to the guest in
> between. It is one branch in `dos_osk_input()` and it is the arm a future
> per-mode TIME behaviour would replace.
>
> **Structure.** `osk_set_mode()` is the only place `osk_mode` changes, and it
> carries every obligation of a transition (mouse UI follows the mode, held click
> released, stale button image discarded, both framebuffers repainted). Adding a
> mode, or per-mode entry/exit behaviour, is a change there and nowhere else.
>
> **Tested** by [`test286/runoskmode.sh`](../../test286/runoskmode.sh), which
> compiles the real `dos_osk.c` for the host and drives it frame by frame. Its
> negative control is the *previous* two-state toggle (`% OSK_MODE_COUNT` → `% 2`),
> which the checker is required to reject; a third arm counts every grid row's
> cells from the source and fails over 40, because `draw_glyph()` clips silently
> and that trap is exactly why backtick was removed in the first place. On-panel
> evidence: `timelines/dos-osk-mode-cycle.tl`, one screenshot per state.

Modifiers are **sticky**: selecting Shift latches it for the next keypress and
then clears. This is standard for on-screen keyboards and avoids needing chords
on a device that cannot express them. Latched state must be visible in the top
bar or it is baffling.

> **OPEN, unchanged:** Should modifiers also support a locked state (double-tap to
> hold)? Ctrl+C is common enough in DOS that sticky-once may be fine, but Ctrl-held
> is occasionally needed. Not implemented. Selecting a latched modifier again
> *clears* it, so the gesture space for a lock is still free.

> **TRAP, and it is not optional: the key-UP must not carry the modifier bits.**
> `INT 7h` zeroes `keyflags1`/`keyflags2` on every SDL event and then re-adds
> whatever modifier bits the word carries (`bios.asm:436-462`) — on key-up as much as
> on key-down. A key-up sent with Shift set therefore leaves Shift latched in the BDA
> with nothing left to clear it. Modifiers go on the down word only.

> **The other half of the same trap: switching modes must release held keys.** Once a
> key has arrived in the SDL format the BIOS stops synthesising releases
> (`last_key_sdl` latches at `bios.asm:393`, auto-release skipped at `:937`). Opening
> the keyboard takes the buttons away from the game-mode table mid-press, so any key
> still down is released first (`dos_input_release_all()`), and the frames on which
> the mode changes emit nothing from the table at all.

## Compositing — the part that must not break

The bars belong to input; the blit must never write outside rows 20..219. Two
consequences from the video contract:

**Draw to both framebuffers.** `../video/06-framebuffer-clut.md` is explicit:
"anything drawn there must be drawn to both or it flickers at the swap rate."
So OSK state changes mark *both* buffers dirty and repaint over two frames, or
repaint both immediately.

**Do not repaint per frame.** The OSK changes only when the selection moves, a
layer cycles, or a modifier latches. Everything else is static. Redraw on state
change only — the same discipline as the text renderer's dirty-cell cache.

**Palette.** The OSK draws with the same CLUT the guest is using — 16 CGA colours
in slots `[0..16)`, with `[16..32)` free. Nothing extra to program, and it means
the OSK does not disturb the guest's palette. Highlighting the selected key is a
foreground/background swap, not a new colour.

> **RESOLVED — input owns it, and there are two cases, not one.**
>
> *Hiding.* `dos_osk_toggle()` arms the same repaint counter with the keyboard
> already marked hidden, so the next two painted frames `memset` both bars to index
> 0 in both buffers. No extra mechanism.
>
> *Guest video mode change.* This one was **not anticipated by this document and is
> the real hazard.** `dos_video.c`'s `clear_framebuffers()` wipes *entire* buffers —
> bars included — on a mode change, by design. The OSK cannot see that call and must
> not reach into video's state, so `dos_osk_draw()` watches the same two bytes video
> keys on (BDA `0x449` mode, `0x44A` columns) and re-arms its own repaint when they
> move. Two loads per painted frame. Without it the keyboard silently disappears the
> moment a program changes video mode, which looks like the OSK crashing.

## Minimum viable

For first light, the OSK does not need layers, modifiers, or a status bar. It
needs: a single row of `a`–`z` `0`–`9` `.` `\` `:` and Enter, D-pad to move, A to
press. That is enough to type `cd games` and `cat.exe`, which is the entire
milestone. Everything above is the shape to grow into, not the first commit.

> **Superseded.** The full shape shipped in one go, because on the corrected
> capacity numbers it cost ~1 KB of rodata more than the minimum and removed the
> need for a second round. See below.

## What shipped

`Core/Src/porting/dos/dos_osk.c`. Three layers, two rows of 8x8 cells each. Cell
width is one glyph per label character, so a row's labels must total <= 40 cells;
every row has spare.

| Layer | Row 1 | Row 2 |
|---|---|---|
| 1 letters | `a`-`m`, `SPC` `ENT` `BSP` `TAB` | `n`-`z`, `SHF` `CTL` `ALT` `ESC` `LYR` `OFF` |
| 2 digits/punct | `0`-`9` `. \ : / - _ = + * ?`, `SPC` `ENT` `BSP` | `, ; ' " ( ) [ ] < > ! @ # $ % & ^ ~ \| ` + modifiers/commands |
| 3 function | `F1`-`F10`, `UP` `DN` `LT` `RT` `HOM` `END` | `ESC` `TAB` `ENT` `BSP` `SPC` `PGU` `PGD` + modifiers/commands |

Top bar: `KBD L<n>` plus `SHF CTL ALT` (white when latched, dark grey when not) on
line 1, and a rolling 38-character echo of what has been sent on line 2. The echo
earns its space — the guest may be mid-redraw or not echoing at all, and then it is
the only feedback there is.

**Shifted keys set `0x1000` themselves.** `osk_needs_shift()` mirrors `a2shift_tbl`
(`bios.asm:4016`) for the printable range: `A`-`Z` and ``!"#$%&()*+:<>?@^_{|}~``.
The SDL decode path never consults that table, so without this, keysym `!` produces
`1`. This is the single most likely thing to get silently wrong here.

**Colours.** Slots 0 (black), 8 (dark grey) and 15 (white) of the CGA palette
`dos_video.c` already programs. Nothing extra is pushed and the guest's palette is
untouched, exactly as the Palette section above requires.

**Cost.** ~2.9 KB text, ~1.0 KB rodata, 53 bytes bss; measured 2,352 bytes off the
`.overlay_dos_bss` headroom (9,336 -> 6,984 against `__RAM_EMU_END__`). No glyph
atlas and no shadow buffer — it renders straight from `dos_font_data.c`'s 8x8 font
into the live framebuffer.

**Verification.** Timeline-driven under gwemu (`--timeline`, screenshot-asserted):
FreeDOS booted to a prompt, `dir` selected key-by-key through the OSK, and the guest
echoed `dir` and printed a directory listing — end-to-end proof that the characters
reached `kbbuf` and not merely the echo line. Bars confirmed populated on both sides
of a buffer swap, blanked on hide, and game-mode `A`/`B`/`TIME` confirmed unaffected.
Not yet confirmed on hardware.
