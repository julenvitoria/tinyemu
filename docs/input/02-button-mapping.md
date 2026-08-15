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

# Buttons available, and what they map to

## What the hardware actually gives us

`buttons_get()` (`Core/Src/gw_buttons.c:8-47`) reads twelve GPIOs, but **two of
them do not exist on the baseline device**. `Core/Inc/main.h:228` is explicit:

```c
// Zelda only buttons; they are not connected on mario.
#define BTN_START_Pin GPIO_PIN_11
#define BTN_SELECT_Pin GPIO_PIN_12
```

`GNW_TARGET` defaults to `mario` (`Makefile.common:62`), so design for the
smaller set and treat the Zelda extras as a bonus.

| G&W button | odroid key | Available on mario | Notes |
|---|---|---|---|
| D-pad ×4 | `UP`/`DOWN`/`LEFT`/`RIGHT` | yes | |
| A | `ODROID_INPUT_A` | yes | |
| B | `ODROID_INPUT_B` | yes | |
| GAME | `ODROID_INPUT_START` | yes | |
| TIME | `ODROID_INPUT_SELECT` | yes | |
| PAUSE/SET | `ODROID_INPUT_VOLUME` | yes | **reserved — see below** |
| POWER | `ODROID_INPUT_POWER` | yes | **reserved — firmware** |
| START | `ODROID_INPUT_X` | **no** (zelda only) | |
| SELECT | `ODROID_INPUT_Y` | **no** (zelda only) | |

Mapping is at `Core/Src/porting/odroid_input.c:48-59`. Note the non-obvious
assignments: `B_GAME` → `START`, `B_TIME` → `SELECT`, `B_PAUSE` → `VOLUME`.

## PAUSE is not ours

`common_emu_input_loop()` (`Core/Src/porting/common.c:236`) consumes
`ODROID_INPUT_VOLUME` as a macro prefix: PAUSE+POWER opens the pause menu,
PAUSE+GAME takes a screenshot, and so on (`common.c:247-260`). `main_dos.c:70`
already calls this every frame.

So the usable budget is **eight inputs**: D-pad, A, B, GAME, TIME.

That is the single hardest constraint in this category, and it is why the
on-screen keyboard exists rather than a direct chord scheme.

## Two input modes

| Mode | Purpose | Buttons |
|---|---|---|
| **Game** | direct play; buttons act as keys | D-pad → arrows, A/B → configurable |
| **Keyboard** | typing at a prompt | D-pad navigates the OSK, A presses |

> **DECIDED — the mode toggle is `GAME` (`ODROID_INPUT_START`).** This document
> originally proposed TIME (`ODROID_INPUT_SELECT`); TIME ships as `y`, so that home
> was taken, and the roadmap suggested the zelda-only `START` (`ODROID_INPUT_X`) as
> "the obvious spare".
>
> **`START` is the wrong answer and was rejected.** It does not exist on a mario unit
> (`Core/Inc/main.h:228`), and a mario unit is precisely the one with *no* spare button
> at all — which the "Zelda extras" section below calls the strongest single argument
> for the OSK. Putting the only door to the keyboard on a button half the fleet lacks
> makes the feature unreachable exactly where it is most needed. Deprioritising mario
> is a reason not to *spend effort* on it, not a reason to ship a feature that cannot
> run there.
>
> `GAME` is the cheapest button to spend. It carried `n`, which was always a duplicate
> of `SELECT`'s `n` (that duplication existed only because `SELECT` is absent on mario)
> and which the OSK now makes reachable on every unit.
>
> **The cost on mario:** `n` is no longer a single button press — it takes opening the
> keyboard. The gain is ~90 keys that were not reachable at all. On zelda nothing is
> lost: `SELECT` still sends `n` directly.
>
> `START` (`ODROID_INPUT_X`) stays unassigned and is still the obvious home for a
> zelda-only convenience binding.

## Game mode defaults — SUPERSEDED

The table below was the pre-implementation proposal and is **not what shipped**. The
`Ctrl`/`Space` guesses were never needed: Alley Cat wanted arrows plus `y`/`n` for its
prompts, and Escape for "paws mode".

**What actually ships** — `dos_key_map[]` in `Core/Src/porting/dos/dos_input.c`, which
is the authority. Change the mapping by editing that table and nothing else:

| Button | odroid enum | Sends | key word |
|---|---|---|---|
| D-pad U/D/L/R | `UP`/`DOWN`/`LEFT`/`RIGHT` | arrow keys (SDL `0x111`–`0x114`, in **UP, DOWN, RIGHT, LEFT** order) | `0x0511`–`0x0514` |
| A | `ODROID_INPUT_A` | `Space` (32) | `0x0420` |
| B | `ODROID_INPUT_B` | **Left Ctrl** (`0x132`) | `0x0532` |
| TIME | `ODROID_INPUT_SELECT` | `y` (121) | `0x0479` |
| SELECT *(zelda only)* | `ODROID_INPUT_Y` | **Left Shift** (`0x130`) | `0x0530` |
| GAME | `ODROID_INPUT_START` | **on-screen keyboard toggle** — not a keystroke | — |
| START *(zelda only)* | `ODROID_INPUT_X` | `Return` (13) | `0x040D` |

> **DECIDED 2026-08-03 — the game-first layout.** The user asked for
> `START=Return`, `SELECT=LeftShift`, `A=Space`, `B=LeftCtrl`. That reverses the
> earlier rule that the prompt-usable keys stay on buttons a mario unit has, and
> it is paid for by the mario deprioritisation plus the OSK: nothing became
> unreachable, only less immediate.
>
> **Escape and `n` lost their buttons and were NOT reassigned.** There is no spare
> input left; picking a victim quietly is how a mapping stops describing what the
> device does. Both are one OSK press away. `y` survives on TIME and is the only
> prompt answer left on a mario unit's buttons.
>
> **`k` lost its home before it ever had one.** `START` was the recorded candidate
> for `k` (Alley Cat's "(K)itten" skill prompt); `Return` took it. See
> `entries/ALLEYCAT.game.tl`.

`GAME` used to send `n` as well, as a duplicate of `SELECT`'s `n` for mario units.
It is now the OSK toggle (see the DECIDED note above); `n` is reachable from the
keyboard on every unit.

### `0x130`/`0x132` are keys, not the modifier flag bits

`Shift` and `Ctrl` reach the guest **twice over**, through two different pieces of
BIOS, and a port that does only one of them is broken in a way that compiles:

1. **As keys.** `INT 7h` special-cases keysyms `0x12F`–`0x134` *before* any other
   decode — `and bh,7` then `cmp bx,0x52f`..`0x534`, i.e. the `0x400` SDL bit plus
   the keysym (`bios.asm:496-537`) — and emits the bare make/break scancodes
   `0x36`/`0xB6` (Shift) and `0x1D`/`0x9D` (Ctrl) to port `0x60`. `INT 9h` then
   deliberately does **not** buffer them (`bios.asm:912-918`), exactly as a real
   BIOS does not: a modifier is a state, not a character. This is what a DOS
   action game reading scancodes wants, and it is the **only** way to express a
   *held* Ctrl or Shift.
2. **As flag bits on the other key's word.** `INT 7h` **zeroes** `keyflags1` and
   `keyflags2` on *every* SDL event (`bios.asm:484`) and re-adds only the
   `0x0800`/`0x1000`/`0x2000` that **that word** carried (`:539-557`). So the
   bare-modifier arm leaves `keyflags1` at **0** — a held Ctrl is invisible to
   `INT 16h AH=02` unless the port folds `0x2000` into the *next* key's word.

`dos_input.c` (`dos_mods_held`), `test286/host_tl.c` (`tl_mods_held`) and
`test286/host_sdl.c` (`mods_now()`) all do both. **Down words only** — a modifier
bit on a key-*up* latches in the BDA with nothing left to clear it
(`../traps.md`). `test286/runbuttons.sh` asserts scancode, ASCII, the absence of
the buffer entry, and the fold, each with a negative control.

**Keyboard mode rebinds the same buttons** while the OSK is up — d-pad moves the
selection, A presses, B is Backspace, TIME cycles the layer, GAME closes. Nothing
reaches the guest from the table while the keyboard owns the buttons, and every key
still held when the keyboard opens is released first (`dos_input_release_all()`) —
a key-down whose button is taken away by a mode switch would otherwise stick down
forever, since the BIOS stops synthesising releases once an SDL-format key arrives
(`bios.asm:393`, `:937`).

Note the odroid enum names do **not** match the physical labels: `B_GAME` → `START`,
`B_TIME` → `SELECT`, `B_PAUSE` → `VOLUME`. Read the comments, not the enum.

<details>
<summary>Original proposal, kept for the record</summary>

| Input | Sends |
|---|---|
| D-pad | arrow keys |
| A | `Ctrl` (the near-universal DOS "fire") |
| B | `Space` / `Alt` |
| GAME | `Enter` |
| TIME | mode toggle |

</details>

> **OPEN:** Per-game key mapping. A DOS core really wants a remap UI and
> per-title persistence, since every game binds differently. That is well beyond
> "basic", but the mapping table should be *data* from day one so adding the UI
> later is not a rewrite.

## Held keys

Game mode must express held keys — walking in Alley Cat means holding a
direction. That rules out the ASCII injection format and forces the SDL word
format; see [`01-injection-path.md`](01-injection-path.md).

Edge-detect against the previous frame's `odroid_gamepad_state_t.bitmask`: a
newly-set bit sends key-down, a newly-cleared bit sends key-up, and unchanged
bits send nothing.

**Held keys ARE modelled, at every layer, and the claim that they are not is
stale.** The device edge-detects, so a held button is one key-down and no key-up
until release. The timeline language has `hold <btn> <secs>`, `down` and
`release` verbs as well as `press` (`test286/host_tl.c` `dos_tl_load`), and even
`press` schedules its release **100 ms** later — at the harness default
`DOS_VIRT_IPMS=3000` that is 300,000 instructions, three times
`DOS_KEY_MIN_HOLD`. The note in `entries/KEEN4.play.tl` ("held-key input is not
modelled, so Keen does not walk") predates those verbs; whatever stops Keen
walking there, it is not the absence of a hold, and it needs re-measuring with
`hold right 5.0` before anything is concluded.

**A held button sends exactly one key-down, and nothing repeats it** — there is no
BIOS typematic repeat (`INT 9h` appends only on key-down, `bios_source/bios.asm:816`,
`:834`). That is fine for games, which track down/up themselves; Alley Cat walks
continuously while a direction is held. It does mean a held key does not auto-repeat
at a DOS prompt. See `../traps.md`.

The key-up half is **not optional**: once a key has arrived via the SDL format the
BIOS stops synthesising releases (`last_key_sdl` latches at `bios.asm:393`, auto-release
skipped at `:937`), so a missing key-up sticks the key down permanently.

## Zelda extras

When `GNW_TARGET=zelda`, `ODROID_INPUT_X` and `ODROID_INPUT_Y` become available.
`Y` ships as `n`; `X` is unassigned. It is **not** the home for the mode toggle — see
the DECIDED note above for why that was rejected.

The mario unit is **deprioritised** as of 2026-07-30, so this is no longer a hard
"nothing may depend on them" constraint. It remains worth knowing that a mario build
has *no* spare button at all, which is the strongest single argument for the OSK.
Switching target is not a flag flip — `GNW_TARGET` changes the binary layout, so it
needs a full `make clean` (see `../../../docs/gwemu.md`).
