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

# Per-title entry sequences, and proving one worked

> Status: **BUILT**. `entries/*.tl`, `test286/runtlparse.sh`,
> `test286/entryseq.py`, `DOS_SHOTS` in `test286/host_main.c`,
> `test286/shotdiff.py`, `test286/shotpng.py`, `test286/runentry.sh`,
> `test286/entrymeasure.sh`. Timeline player: `test286/host_tl.c` (authored on
> `feat/machine-size`, copied here byte-identical so this branch runs alone).
> Implements `improvement-brainstorming.md` §A2 and answers §L's last open item.

## 1. The problem this exists for

Every dirty-page and write-set number in this tree was taken **with no input**.
`poolmeasure.sh` says so in its own header, `obs21.sh` says so in its own header,
and §L says it is the reason §B was measured at 0 pages. Titles boot, sit at a
menu or a title screen, and are measured there. KEEN4's 83 pages is a title
screen. Sizing memory provisioning from those numbers is sizing from fiction.

The fix is not injection *per se* — `DOS_KEYS` has existed since `runkeyhold.sh`.
The fix is that **a key sequence which silently fails to advance produces exactly
the confident-looking zero it was meant to replace**, so the sequence and the
proof that it worked have to ship together. `WOLF3D 0/144` was once reported as a
triumph and meant the image never booted.

## 2. Where each of §A2's five fields lives

| §A2 field | where | note |
|---|---|---|
| executable | `--entry` | recorded in the `.tl` header as `#: exe` |
| working directory | `--subdir` | `dir` |
| parameters | `--entry-args` | `args` |
| start command | `make_autoexec()` composes it | still the only composer |
| **canned key sequence** | `entries/<IMAGE>.<entry>.tl` | new |

`entryseq.py mkdosdisk <file>` prints the first three back as a `mkdosdisk.py` argv.
It does **not** run it: regenerating a `.dsk` invalidates the `.dosmeta` sidecar
keyed on the image's CRC and size, so a mechanism that needed a disk rebuild
would be unusable for the measurement work it exists to serve.

Multiple entries per title are a list, exactly as §A2 wanted; `default: true`
marks the one a launcher would time out to. No title has needed a second entry
yet — none of the nine drivable images ships a separate setup binary.

## 3. One format: the `.tl` is the entry sequence

`entries/<IMAGE>.<entry>.tl` is a **gwemu timeline**, not a lookalike and not
something a converter emits. The same bytes are played by the host harness
(`DOS_TIMELINE=<file>`, player `test286/host_tl.c`) and by
`run_gwemu.sh --timeline`. Everything a timeline cannot express lives in `#:`
comment lines, which gwemu skips like any other comment; `entryseq.py` reads
only those.

**`test286/runtlparse.sh` is what makes that a fact rather than an intention.**
It runs the *real* gwemu binary headless with `GNW_TIMELINE` set and requires
`gnw-timeline: <file> armed (N time + M frame events)` back.
`gnw_timeline_init()` runs at machine init and needs no firmware, disk or
display, so it costs about a second per file. Two arms, both mutation-tested:

* an unknown button fails (`parse error: "10.0 press nosuch"`);
* **a file that armed fewer events than it declares fails.** `#` truncates a
  line anywhere (`gnw_timeline.c:215`), so a mistyped directive does not error —
  it silently becomes a comment and the timeline quietly gets shorter. `#:
  min-events N` is what turns that from invisible into loud.

### Time is emulated wall clock, and the bridge is `DOS_VIRT_IPMS`

`.tl` instants are guest wall-clock seconds, so the same file replays under
gwemu and under the harness at whatever rate each runs. That does **not** cost
reproducibility here, because the harness clock is `dos_host_millis()`, which
with `DOS_VIRT_IPMS=3000` is a deterministic function of retired instructions.
The rate is stated rather than hidden — **3,000,000 guest instructions per
second** — and is settable (`--ips`, `DOS_TL_IPS`) at the one place it is
applied, which is the conversion of `#: title-at`/`#: play-at`/`#: quit-at` into
the instruction-scheduled observation knobs (`DOS_SHOTS`, `DOS_BUDGET`).

`DOS_KEYS`, which schedules on retired instructions, is not expressible in a
`.tl` and does not agree with gwemu. It stays for the scripts already using it
(`runkeyhold.sh`, `runkeen4.sh`); nothing new uses it.

### What the format cannot express, and why that is left visible

Only the eight guest keys a Game & Watch button produces reach the guest
(`dos_key_map[]`). **Alley Cat's skill prompt wants `k` and there is no such
button**, and Enter does not substitute — measured: a run that answers the
joystick prompt and then presses A four times sits on the skill menu with a
0.00% screen delta.

Driving the on-screen keyboard *is* expressible as a `.tl` and is correct on the
device — but `host_tl.c` maps GAME and the unassigned buttons to keysym 0,
because the harness has no OSK. Such a file would load and silently achieve
nothing, which is the exact failure this mechanism exists to prevent. So it is
not written: `entries/ALLEYCAT.game.tl` is marked `reached: blocked` and carries
the one-line fix instead — bind the **unassigned** physical START button
(`ODROID_INPUT_X`) to `k` in `dos_input.c`, which that table's own comment
already records as the thing that answered this prompt during testing.

### The instruction-count ceiling, kept for the record

`inst_counter` is a 32-bit unsigned, so nothing past ~4.29e9 instructions can be
scheduled on the observation knobs, and a snapshot scheduled past it never
fires. Found on a 6e9-instruction PRINCE run whose second snapshot silently
never appeared. `runentry.sh` treats a missing snapshot as its own failure
rather than as a delta of 0.

## 4. The proof: `DOS_SHOTS` + `shotdiff.py`

Two screen snapshots per run — one immediately **before** the first key, one at
the end — and the entry declares how much of the screen must have changed
between them, optionally plus the video mode it must have reached.

A **screen delta** rather than a golden hash, because a percentage means the same
thing in text, CGA, mode 13h and EGA 0Dh, and a golden hash of a real game's
frame is a rot machine. The snapshot carries four regions and the first one is
the one that is easy to omit and fatal to omit:

| region | why |
|---|---|
| `0xC0000` 4 KB | **this BIOS prints here, not to `0xB8000`.** A snapshot of `0xB8000` alone reads all-zero for a title at a text menu |
| `0xB8000` 16 KB | text and CGA graphics |
| `0xA0000` 64000 | mode 13h linear |
| EGA planes | mode 0Dh |

`shotdiff.py` excludes any region that is all-zero in **both** snapshots, or a
text-mode title would score 0.3% because the 64,000-byte mode 13h aperture it
never touched dominates the divisor.

## 5. `runentry.sh` — three arms, and two of them must fail

1. the entry's sequence must **meet** the bar;
2. **no keys at all** must **miss** it — otherwise the check is measuring a title
   animating by itself and would pass an empty sequence;
3. the entry's declared `wrong_keys` must **miss** it — arm 2 cannot catch a
   sequence whose keys are merely misaimed but which still wakes the title up.

Arm 3 has already earned its place. The first TANKWARS entry used `'z'` at the
three instants its Enters go, and **`runentry.sh` failed it**: Tank Wars advances
on any key, so `'z'` reached the same menu and the same 75.36% delta. The control
had to become a wrong *time* instead — the keystrokes moved into the middle of
the DOS boot, where COMMAND.COM eats them. Recorded in `entries/TANKWARS.game.wrong.tl`
rather than quietly corrected, because the first version would have passed a
check that could not fail. KEEN4 needed the same treatment for the same reason.

Not the timeline's own `screenshot` verb: in the harness that dumps the whole
guest megabyte (`host_tl.c`), which changes on every BIOS tick and so cannot
carry a threshold. `DOS_SHOTS` is an *observation* knob, not an input format, and
the one-format rule is about input.

Two demonstrated failures, both real:

* **the wrong-time control**, KEEN4 and TANKWARS: the same button pressed 0.333 s
  in, while DOS is still booting, is eaten by COMMAND.COM and misses the bar
  (0.12% / 0.00% against 32.49% / 75.36%).
* **the wrong-key control on Alley Cat**, which is where this pays for itself:
  answering the joystick prompt `y` instead of `n` selects a joystick that does
  not exist, the keyboard stops driving the game, and the run sits on the prompt
  — **0.00% against 76.81%**, with an unmoved write set and every injected event
  reported as fired. See `docs/traps.md`.

For Alley Cat there is also a stronger, non-automated proof that this is play and
not an attract loop: with RIGHT held to the end of the run the cat finishes at
x≈240 of 320, and the identical run with no direction held finishes at x≈100.

## 6. `entrymeasure.sh` — three columns, not two

`legacy` (no keys, 40M instructions — what every number in the tree is),
`before` (no keys, the entry's budget) and `after` (the entry's keys, the **same**
budget). `after − before` is the cost of input; `before − legacy` is the cost of
time. Reporting `after` against `legacy` alone would add the two together and
attribute the sum to gameplay.

## 7. Cross-checks that were actually run

* **The `.tl` path and the legacy `DOS_KEYS` path agree to the digit.** KEEN4
  32.49% / 83→146 pages / `LOST=39582`, TANKWARS 75.36% / 85→90, measured both
  ways. The wall-clock schedule lands where the instruction schedule did.
* **Every shipped `.tl` loads in the real gwemu parser** (`runtlparse.sh`,
  13/13), and the check fails on both an unknown button and a silently-shortened
  timeline.
* **No generated `.tl` has been *replayed* under gwemu.** Loading is proved;
  replaying needs a local SDL GUI and a human, and is not claimed.
