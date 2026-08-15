# Per-title state, and the verdicts that should not be re-derived

What each title does today, what stops it, and — where the answer is "never" —
the arithmetic, so nobody spends a day rediscovering it.

Measure with `scripts/probe_report.py --title <name>`, which leads with what we
did **not** answer. Judge pictures with `scripts/screenshot.py`; judge a text
screen's *shading* with `--attr`, never `--text`.

## THE "THREE TITLES CONVERGE ON ONE ADDRESS" FINDING IS RETRACTED — MEASURED 2026-08-15

Dark Forces, Mortal Kombat and Epic Pinball were all recorded as dying on
`X86_EXIT_UNIMPLEMENTED`, `exit_info=0x0F`, `CS:EIP = 0081:032F`. **Not one part
of that was a fact about the guests.** It was two reporting defects in our own
interpreter, and the reasoning built on it ("three unrelated programs do not
converge by coincidence", "segment 0x81 is the child load segment, so it is our
EXEC") was reasoning about an artefact. EXEC is exonerated: `dos_load_mz()`,
`mz_place()` and the relocation-factor choice were never in the path.

**Defect 1 — the exit named the ESCAPE, not the opcode.** `.op_0f_0F` loads the
second opcode byte into **R1** and dispatches; `.exit_loop` stores **R0**, which
still held `0x0F`. So *every* unimplemented two-byte opcode in the machine
reported the identical `exit_info=0x0F`. The convergence was that one byte, and
it was a constant.

**Defect 2 — `0F 01` is a GROUP, not an opcode.** Even the right byte would not
have said which member. The group-7 exit now carries its ModRM in the high half.

**Defect 3 — the reported `CS` was never read from the CPU.** `probe_report.py`
now derefs `cpu_state_ptr` directly (`_read_death_site()`), and the same three
titles read `02BF:032F`, `…`, `099E:20D7` — three different addresses. `0081`
does not appear anywhere. `032F` was real; the segment beside it was not.

**What the three were ACTUALLY executing — three different opcodes:**

| title | real `exit_info` | instruction |
|---|---|---|
| Dark Forces | `0xF00F01` | `0F 01 F0` = **LMSW AX** |
| Mortal Kombat | `0xF00F01` | `0F 01 F0` = **LMSW AX** |
| Epic Pinball | `0x0FAF` | `0F AF` = **IMUL Gv,Ev** |

Both are implemented now (`cpu_exec_handlers_0f.S`), with `tests/test_opcodes.py`
arms 276-283 and every one of the eight **seen to fail** when the fix is backed
out. Cost: `__CORE_ITCM_CODE_END__` **0xD2D8 -> 0xD580** (+680 B, all cold) and
`__CORE_BSS_END__` **unchanged** — zero RAM_EMU, zero pager frames.

**The lesson worth keeping: an instrument that cannot distinguish two failures
will report them as one.** Three titles were read as sharing a blocker for
several passes because the one number anybody looked at was incapable of
telling them apart.

### LMSW put the first title ever into PROTECTED MODE

**`cpu_cr0` reads 1 for Dark Forces and Mortal Kombat.** The descriptor walk of
[`docs/14`](docs/14_protected_mode.md) — built, unit-tested at 80 checks, and
recorded on every prior pass as "NEVER exercised by any title" — **is now
exercised by two.** It held: both titles come out of the transition with
`CS=0x0070` (a real GDT selector, base `0xA900`/`0xA930`, not `sel<<4`) and
`SS=0x0020`, so `LGDT`, the GDT fetch, the type/DPL/present rules and the
same-privilege far JMP all ran against a guest's own table for the first time.
`x86_pmode_sync_mode()` re-derived the dispatch tables on the PE edge.

`LMSW` and not `MOV CR0` is the reason it took this long: a DOS extender must
also run on a 286, so it uses the 286 entry sequence, and `MOV CR0` (0F 22) —
which we have had all along — is never issued.

### Where all three stop NOW

| title | insns | `CR0` | `CS:EIP` | stops on | needs |
|---|---:|---:|---|---|---|
| Dark Forces | 102,517 | **1** | `0070:5403` | `exit_info=0xCD`, `INT imm8` with PE set | protected-mode **IDT gate dispatch** |
| Mortal Kombat | 42,188 | **1** | `0070:559D` | `exit_info=0xCD`, same | the same one thing |
| Epic Pinball | 443,391 | 0 | `078F:093C` | `exit_info=0x0FA0`, `PUSH FS` | `0F A0/A1/A8/A9` |

Dark Forces and Mortal Kombat **do** now converge, and this time it is a real
convergence with a named cause: `L(HANDLE_INTERRUPT)` branches to `.exit_loop`
when `CR0.PE` is set, because the gate path needs descriptor validation and a
possible stack switch. That is PLAN.md Queue B item 5's "`IRET` across
privilege, call gates, stack switches", and it is now **measured demand from two
titles** rather than the unsized item it has been.

Epic Pinball is unrelated to both, still real mode, and moved from 477,239 to
443,391 instructions into a different phase — it programmed the PIT
(`guest_tick_divisor` 65,536 -> **19,890**), which it had never reached.

Health on all three: `core_ready=1`, `x86_pg_frames_total=155`,
`guest_ram_pages=155`, `guest_ram_pages_short=5` (the `X86_PROBE=1` build's
own cost, identical with and without this change), `x86_pg_exhausted=0`,
`x86_dos_load_shorts=0`.

## The non-regression baseline

Three titles, three renderers. These pictures and pixel counts are the
non-regression test for anything touching the launch path, the DOS layer or
video:

| title | renderer | non-zero px | what it shows |
|---|---|---:|---|
| WOLF3D | Mode Y | 57,874 / 76,800 | title screen: logo, two guards, id credit |
| ALLEYCAT | CGA mode 4 | 64,000 | joystick prompt |
| KEEN4 | EGA 0Dh | 60,463 | MAIN MENU console |

**An instruction counter is not a non-regression test.** A DOS-layer change once
executed 64.7 M instructions "and still running" while printing rubbish, and was
merged on that basis.

## Wolfenstein 3-D — runs, with two live rendering defects

Reaches gameplay. Mode Y arms, the page stride is **learned as 16,640** from the
guest's own display-start writes (the id Galaxy engine's `SCREENSIZE`), and the
signon, title, Options and difficulty screens render correctly.

Live defects, both measured on hardware:

1. **Cached pictures are noise.** The episode-select screen has perfect text and
   six thumbnails of garbage. Wolf3D caches pictures in off-screen VRAM and
   blits them with latch copies; text goes straight to the visible page, which
   is why they differ.

   | counter | value |
   |---|---:|
   | `x86_vga_modey_cache_offs` | 8,192 (claim succeeded, `_lost_nocache=0`) |
   | `x86_vga_modey_lost_cache` | **16,896** stores discarded above `0xE300` |
   | `x86_vga_latch_miss` | **4,928** latch loads reading an absent cell |

   The cache covers `0xC300`-`0xE2FF`; Wolf3D's writes reach `0xEBFF`, needing
   ~10,496 offsets. ~12 KB of arena is free.

2. **Mid-draw frames.** `x86_modey_mid_draw` climbs into the thousands during
   gameplay — the panel showing a half-drawn page. Four presentation bugs have
   been fixed already; see [`08_vga_mode13h.md`](08_vga_mode13h.md) §4.

**Performance:** ~1.4 M ips in Mode Y against ~3.0 M in text, PC samples in
`x86_vga_planar_store`. The planar store path, not the interpreter, is the limit.

## Commander Keen 4 — banner and menus render; levels need more card

Mode 0Dh is **adopted**, not refused, and the arena claim succeeds. The startup
banner detects correctly — `--attr` at 2 s shows the bright runs are **values**:
`√CGA`, `√EGA/VGA`, `√Keyboard`, `√PC Speaker`, and the memory box reads
`√Standard 390 Kbytes` where the EXE ships `xxxxx` placeholders.

Keen brightens what it detected; the characters are identical either way, so
**`--text` is structurally blind to the result** and reading it cost a session
once.

Past the banner it drew black, and the geometry says why: Keen programs CRTC
Offset `0x20` (64 B/row, a 512-pixel virtual row), so a 200-row window spans
**12,800 plane offsets against 8,192 fitted**. 60.2% of its stores were
discarded and the plane dump was all zeros — an honest black frame, not a blit
defect. Growing the card from the arena moved this; the remaining gap is below.

### The WEDGE: measured on silicon, and it is NOT the pagefile

Reproducible on HARDWARE every run. Both `guest_insns_total` **and**
`frames_rendered` freeze together, WWDG fires (`boot_magic=0xbad00005`), the
device reboots into KEEN4 and does it again. The freeze point VARIES between
runs -- 14.9 M insns/347 frames, 19.9 M/352, 31.4 M/640 -- so the reported
"34.7 M over 944 frames" is one sample of a spread, not a landmark. **Under
gwemu the same core reaches the MAIN MENU and runs indefinitely** (153 M insns,
2,081 frames, screenshots in `build/artifacts/keen4*/`), so nothing below tier 3
can see it.

**The pagefile-thrash hypothesis is DISPROVEN, on silicon.** The tidy story was:
0Dh donates 60 of 160 frames, Keen's 134-frame working set overflows the
remaining 100, and a storm of 4 KB pagefile writes at up to 433 ms each stalls
the frame loop past the watchdog. It is wrong. With `X86_PG_DONATE_RESERVE`
raised to 134 the donation is refused outright and the pagefile is never
touched -- `x86_pg_donated=0`, `x86_pg_frames_total=160`, `x86_pg_evictions=0`,
`x86_pg_wb_file=0`, `x86_pg_wb_us_worst=0`, measured on the device -- **and it
still wedges.**

> `x86_pg_exhausted == 0` is consistent with thrash AND with a completely
> unrelated hang. Only the eviction and writeback counters separate them, and
> they say: no I/O anywhere near the freeze.

That both counters freeze together says the FRAME LOOP stopped, not the guest --
so this is host-side, and `MAX_SLICE_CEIL` (which bounds a slice in
instructions) cannot be the guard. **This paragraph was right and the section
below now says why.**

**THE `ega_shadow_rebuild()` LEAD IS DEAD AS A CAUSE.** It was measured with a
counter and a DWT bracket (`X86_EGA_SHPROF=1`, `x86_video_planar.c`), on
silicon, over five KEEN4 runs, and the numbers refuse it:

| | measured, hardware |
|---|---:|
| one rebuild | **846,000 cycles = 3.0 ms** at 280 MHz |
| rebuilds in a whole pre-wedge run | **10** (~0.15/s), not "continuous" |
| burst coincident with the wedge | 76-86, ~230 ms of cycles |
| the wedging stall | **seconds** -- the WWDG window is ~472 ms |

So the rebuild is extravagant and it is a MARKER of the wedging code path (the
burst appears in 3 of 3 wedging runs and in neither non-wedging run), but it is
a passenger. Proven by fixing it: the precise read-state guard below cuts
steady-state rebuilds 10 -> 2 and the burst is UNCHANGED at 76-79, because
those are genuine read-state changes -- **and KEEN4 wedges exactly as before,
twice in a 60 s run, at 31.5 M/638 and 22.5 M/468.**

### ANSWERED, AND FIXED: one FRAME outlives a watchdog window that covers the whole frame

**There is no runaway loop, in the guest or in the host.** The
"one SLICE never returns / the guest is still executing" reading (from the
rebuild counter appearing to advance across the freeze) is RETRACTED. It was
built on a counter sampled either side of the reset the wedge itself causes.
The direct measurement refuses it: `cpu_state.pit_counter` is stored **once per
guest instruction** and is readable over the probe while the target runs, and
across the freeze it is **FROZEN** -- 2066, unchanged over four samples spanning
1.5 s, with `pit_reload` 5304. The guest is not executing at all.

`probe.py --deref cpu_state_ptr+2184=PITCNT` is the whole instrument. It costs
no build, no RAM_EMU and no flag; reach for it before adding a counter.

**What is actually happening, measured on silicon:**

1. A frame's GUEST stage takes longer than the ~472 ms WWDG window. The
   watchdog was refreshed **exactly once per iteration, at the bottom of the
   body**, so the window covered a WHOLE frame and any single overlong stage
   reset the device.
2. The reset is `BSOD_WATCHDOG` = **5**, and `0xbad00005` is
   `BOOT_MAGIC_BSOD | 5` -- not a code to decode further.
3. After it, retro-go's `BSOD()` sits in `while (buttons_get() == 0 || ...)`
   waiting for a keypress, then `HAL_NVIC_SystemReset()`. **Every PC sample
   taken during "the wedge" is that spin loop** (`buttons_get`,
   `HAL_GPIO_ReadPin`, `HAL_Delay`), which is why PC sampling kept naming
   firmware input code. The freeze is not a hang; the device is already dead
   and waiting to be told to reboot.
4. Why the stage is slow is NOT a mystery and NOT a new bug: the instruction
   budget is not a time budget. ~50,000 instructions is 18 ms of ordinary code
   and most of a second of the aperture-plus-`ega_shadow_rebuild()` mix at
   846,000 cycles a rebuild. So `ega_shadow_rebuild` is, after all, the
   PASSENGER the measurement said it was -- it makes the frame expensive, and
   the missing refresh is what turns an expensive frame into a reset.

**The fix is host-side and is a general guard, not a Keen workaround**
(`src/main.c`): `FRAME_PHASE(id)` publishes which stage the frame loop is in
and **refreshes the watchdog at every stage boundary**, so the window bounds a
STAGE rather than a whole iteration. A slow stage is now a slow frame.

Graded on silicon, both directions, on the same timeline:

| build | result |
|---|---|
| per-stage refresh ON, wall-clock escape OFF | **51.6 s, 56.0 M insns, 1,095 frames, no reset** |
| per-stage refresh OFF (`X86_PHASE_WDOG=0`), escape ON | **WEDGES TWICE** -- 29.5 M/601 and 20.2 M/419, `x86_frame_phase` = 4 (`X86_PHASE_GUEST`) at both |
| shipping (both) | 51.6 s, 55.7 M insns, 1,128 frames, ~3.0 M insn/s, `frame_ms_worst` 67 |

`X86_PHASE_WDOG=0` is the negative control and it is SEEN TO FAIL. The stage
identifier in `x86_frame_phase` at the freeze is what named the guest stage;
nothing derived it.

**Two further bounds went in with it, and neither is the fix** -- say so if
either is ever quoted as one. `GUEST_SLICE_WALL_MS` (120 ms) escapes the
sub-slice loop, `guest_slice_overruns` counts it, and it fired 9 times in one
run; `GUEST_ASM_MS_MAX` (60 ms) halves the next budget when ONE
uninterruptible `x86_cpu_interp_asm()` call overruns, published as
`guest_asm_ms_worst` / `guest_asm_overruns`. The escape ALONE does not fix the
wedge (row 2 above has it on), because it can only act between calls and the
overlong stage is a single call. `guest_asm_ms_worst` reads **14 ms** on a
healthy run, so the ceiling has headroom over normal play.

**Still open, and separate: the artefacting.** The owner reaches the main menu
and the level-selection screen, where the picture corrupts before the crash.
That is the EGA card shortfall below (`x86_vga_planar_lost`), not this.

### The read-shadow guard, which is a real fix and is NOT this fix

`x86_vga_planar_gc_write()` rebuilt the 16 KB shadow whenever one of GC 2/4/5/7
changed VALUE. "The register changed" is not "what a read returns changed":
under read mode 0 Colour Compare and Don't Care are never consulted, under read
mode 1 Read Map Select is not, and only bit 3 of GC 5 is read state at all --
bits 0-1 are the WRITE mode, which is what a latch-copy loop toggles. So
`ega_read_sig()` reduces the four registers to what a read actually depends on
and the rebuild fires only when THAT moves. Graded by six read-path probes in
`tests/test_video.py`'s 0Dh arm (Read Map Select, read mode 1, Colour Compare,
Colour Don't Care, and the return to read mode 0) and by two negative controls
in `scripts/run_planar_controls.py`, both seen to fail.

### The EGA card, and what the intro costs

| | plane offsets | four planes |
|---|---:|---:|
| card fitted NOW (arena alone, reserve 134) | **35,840** | 143,360 B |
| card when the donation fired (reserve 100) | 61,440 | 245,760 B |
| 200 rows at `crtc_start=15360`, pitch 64 | 28,160 | 112,640 B |

`x86_vga_planar_lost` still reads **102,195** at 35,840 offsets, so the panning
intro drives `crtc_start` well past the 15,360 the static figure was taken at:
covering a pan that reaches start S costs `S + 12,800` offsets.

**The donation cannot close that gap for this title, structurally.** An arena
claim may not span regions, so the card is the best SINGLE run -- the arena's
143,360 B or the donated run, never their sum. Beating the arena therefore takes
~35 frames, and 160 minus Keen's 134-frame peak leaves 26. Above a reserve that
respects the working set the mechanism is inert here; below one, it buys card at
the price of SD writes in the frame loop. Growing the card for Keen means
growing the ARENA, not borrowing frames.

### The "levels are not fixable" verdict is RETRACTED

It reasoned from `x86_arena_free = 0` to *"no allocation, policy or eviction
change closes the gap"*, and that step does not follow: **the arena is not the
only AXI on this device.**

| | |
|---|---:|
| correct picture (the reference's own table) | 48 KB/plane = **196,608 B** |
| arena | 143,360 B |
| deficit | **53,248 B = 13 pages** |
| KEEN4 peak pager use | 134 of 160 frames |
| spare frames | **26 = 106,496 B** |

The route is `x86_pg_donated` + `x86_arena_add_region()` — see
[`12_memory_tiers.md`](12_memory_tiers.md) §5.

Two errors produced the wrong verdict, and both are the same shape:

- It quoted the **prior port's** budget (~24.9 KB of headroom, COW spare 0) as
  if it were a property of this hardware.
- It quoted **64 KB/plane**, which is where the reference's *discards reach
  zero*, not where the picture becomes correct — their own table says 48 KB
  renders correctly. That made the gap look 2x worse than it is.

> **An inherited number from another project is not a property of this
> hardware.** Re-measure before concluding anything is impossible. This cost a
> wrong "impossible" once and a wrong "38% idle" the same day.

## THE HMA IS BACKED WHEN A20 IS OPEN — MEASURED 2026-08-15, and it moved BOTH blocked titles

`x86_mem_wrap_1mb()` used to write `FOLD_INVALID` across the sixteen pages at
1 MB whenever the gate was OPEN. That was honest about the implementation and
wrong about the machine: a 286+ with A20 open HAS memory there, and it was the
single blocker under both entries below. The pages are now ordinary demand-paged
guest pages, through the pager's SECOND WINDOW — the same `x86_pager_own_aux()`
mechanism EMS uses at linear 4 MB, generalised from one window to two rather
than a third mechanism being invented.

**The ownership rule, because getting it wrong is silent corruption, not a fault:**

| gate | who owns the sixteen fold entries | the frames |
|---|---|---|
| OPEN | the pager, via the aux window. Filled from the ZERO tier on first touch | the HMA's own |
| MASKED | the WRAP. The alias onto low memory is the guest-visible truth, and the pager may not write those entries **even to unmap a page it is evicting** (`fold_is_wrapped()`, `x86_pager.c`) | kept, untouched, behind the alias |
| REOPENED | the pager again; `x86_pager_refold()` points every still-resident page back at its own frame | unchanged, data intact |

**They are EVICTABLE and NOT PINNED.** They are ordinary guest pages: untouched
they cost nothing, and a touched one is a normal victim with a normal writeback.
`x86_pinned_pages` is unchanged — nothing here pins. The only special-casing is
the fold entry above, which is about who may WRITE the table, not about frames.

**The 1 MB seam is real, refused and COUNTED, exactly as `docs/10` requires.**
Sixteen pages handed out in FAULT order are sixteen unrelated frames, so a
16-bit access straddling a boundary INSIDE the window is docs/10's defect — it
is served by the ordinary slack mechanism and `tests/test_paging.py` grades both
halves of the value landing in the right frames. The 1 MB LINE ITSELF is
different: page 255 is a fixed mapping with no slack of its own, so a straddle
across `0x100000` cannot be served, is refused, and shows up in
`x86_seam_overflow`. Both titles below read **`x86_seam_overflow=1`** — one
refused straddle each, the honest degradation and not a silent wrong byte.

**`INT 15h AH=88h` now answers 64 KB** (`EXT_MEM_KB`, `x86_bios.c`) because that
is exactly what is backed — sixteen pages and nothing above `0x110000`. `AH=87h`
(move block to extended memory) and `AH=89h` (switch to protected mode) are
still refused with CF=1/AH=86h, which is not an inconsistency: the memory exists
and those two services do not. `AH=C0h`/`AH=BFh` stay unanswered and counted —
answering them means synthesising a system configuration table, which is a
different piece of work and would be a lie about the machine, not about memory.
**XMS still reports no HMA** (`BL=90h`): XMS `AH=01h` grants EXCLUSIVE ownership
to one client and requires the driver to drive A20 on its behalf, none of which
is built. The memory being backed and a driver managing it are two claims, and
only the first is true. `test_xms` stays 47/47.

### The gates

`tests/test_paging.py` is **165 checks** (was 133): 32 new ones covering the
write/read-back, the mask-and-see-the-ALIAS round trip, the reopen, the straddle
inside the window, the refused straddle across the line, and the window coming
and going with the gate. `scripts/run_hma_controls.py` is the negative-control
runner and **both arms were SEEN TO FAIL**: `X86_HMA_NEGCTL=1` leaves the window
registered across a mask (the guest then reads its HMA data where the alias
belongs) and `=2` refuses the HMA's slack (the straddle is no longer served).
Both caught, baseline green.

### What it cost, measured and not estimated

`__CORE_ITCM_CODE_END__` = **0xD2D8**, byte-for-byte unchanged — none of this is
on the fast path. `__CORE_BSS_END__` **0x2405FC30 -> 0x2405FD38** (+264 B), and
that 264 B **crosses a 4 KB alignment boundary in the RAM_EMU claim**:
`guest_ram_free_at_claim` **655,360 -> 651,264**, so `guest_ram_pages` is
**158** with `guest_ram_pages_short=2`, one frame below stock master's 159/1.
One 4 KB guest frame is the price of sixteen, on demand. It is recoverable by
trimming any 264 bytes of RAM_EMU footprint, and it is quoted here so nobody
re-derives it as a starvation bug — `core_ready=1`, `x86_pg_frames_total=158`,
`x86_pg_exhausted=0`, `x86_pg_evictions=0`, and the WOLF3D golden is **0.000%**
on this exact core.

## Mortal Kombat — MEASURED 2026-08-15: THE A20 WRAP TEST PASSES, and it now runs 42,188 instructions

Same command as the entry below it (`start-preselected-game.tl`, gwemu tier 2),
plus `probe_report.py --title mortalkombat`, which is now a PRESET rather than a
hand-rolled run.

**The wrap test is answered, and answered the way a 286+ answers it.** The guest
opens A20 at port 0x92, compares `FFFF:0030` against `0000:0020` with
`REPE CMPSW`, and the two locations now DIFFER because the HMA is its own
memory. `x86_a20_changes=3` — open, mask, open — which is the shape of a program
that tests the gate in both directions and then leaves it where it wants it.
`x86_a20_state=1`, `x86_pg_unmapped=0` (it was 1, and that 1 WAS the wrap test).

**It went from 0 instructions to 42,188** and from wall 4 of its C runtime into
a whole extender phase it had never reached. It now asks for exactly what Dark
Forces asks for, in the same order: `INT 2Fh AX=1687h` (DPMI), `INT 67h AH=DEh`
(VCPI) ×2, `INT 67h AH=43h`/`45h`, `INT 15h AH=BFh` ×2, `AH=C0h`, and
`INT 21h AH=38h`. It also opens files now (`x86_dos_file_calls=7`, three
`AH=3Dh`, two `AH=3Fh`) and writes the CMOS index/data ports (`OUT 0x70,0x0F`,
`OUT 0x71,0x09` — unmodelled, dropped).

**Where it stops now:** `guest_exit_reason=4` (`X86_EXIT_UNIMPLEMENTED`) with
`exit_info=0x0F` at `CS:EIP = 0081:032F`, i.e. inside the image loaded at
segment 0x81. **That is the same exit, the same info byte and the same CS:EIP as
Dark Forces**, which is a fact worth carrying forward: the two titles are now
blocked on ONE thing, in shared extender code, not on two.

**SUPERSEDED — see the retraction at the top of this file.** The caveat below
was RIGHT to distrust the address, and the answer is that both the segment and
the opcode byte were our own reporting, not the guest's. Everything from here to
the end of this section describes the pre-LMSW machine.

**Read this caveat before chasing it.** A `--guest` hexdump at that linear
address (`0x810 + 0x32F = 0xB3F`) shows an 8-byte-stride DATA table, not an
instruction stream with an `0F` in it. So either `cpu_state.eip` is not the
faulting address on this exit path, or the guest has transferred into data.
Settle THAT before naming an opcode — an opcode named from a stale EIP is how a
day gets spent implementing something nothing asked for.

Everything else stayed healthy: `x86_pg_exhausted=0`, `_evictions=0`,
`x86_map_failures=0`, `core_ready=1`, `x86_xms_detects=0` (still never asked).

## Star Wars: Dark Forces — MEASURED 2026-08-15 (FOURTH PASS): past the memory hunt, into file loading

`probe_report.py --title darkforces --skip-cost`, gwemu, 30 s, same preset as
the third pass below.

**`guest_exit_reason` moved from 5 to 4.** The `X86_EXIT_MEM_FAULT` at the HMA
is gone: the extender opens A20 (`x86_a20_changes=3`, `x86_a20_state=1`), reads
over the 1 MB line through `ES=0xFFFF`, and **gets memory**. `x86_pg_unmapped=0`
where the third pass measured the fault.

**And it went to work.** 102,517 instructions and **28 distinct probes against
30 file calls**, where the third pass died in the hunt: ten `AH=3Fh` reads, eight
`AH=42h` seeks, three opens, three closes, sixteen `AH=4Ah` resizes and three
`AH=48h` allocations. It is loading its data files.

**`CR0=0`. STILL REAL MODE, AND THE CLAIM WAS TESTED AGAIN, NOT ASSUMED.** The
descriptor walk of `docs/14` remains unexercised by this title — backing the HMA
did not push it into protected mode, and nothing in this pass exercises the
walk. `x86_xms_detects=0`: XMS is still never asked.

**`INT 15h AH=88h` did not decide this.** Dark Forces does not call it in this
run at all — it asks `AH=BFh` and `AH=C0h`, both still unanswered. The honest 64
is there for the titles that do ask; it is not what moved this one.

**Where it stops now:** identical to Mortal Kombat above —
`X86_EXIT_UNIMPLEMENTED`, `exit_info=0x0F`, `CS:EIP = 0081:032F`, with the same
caveat about that EIP. `x86_seam_overflow=1`, `x86_pg_exhausted=0`,
`_evictions=0`, `x86_map_failures=0`.

**ANSWERED, AND IT WAS THE SECOND OF THE TWO — "a shape the exit path reports
for something else entirely". See the retraction at the top of this file.** The
question below was posed exactly right; the answer is that `exit_info=0x0F` was
the 0F ESCAPE byte, reported for every unimplemented two-byte opcode there is.

**THREE titles now end on `exit_info=0x0F`** — these two and Epic Pinball. That
is either one missing two-byte opcode shared by the runtimes they are built
with, or a shape the exit path reports for something else entirely. It is now
the single highest-value question on this page, and the first step is deciding
which of those two it is rather than implementing an opcode from an EIP nobody
has corroborated.

### The third pass, which named the HMA (superseded by the above)

## Star Wars: Dark Forces — MEASURED 2026-08-15 (THIRD PASS): AH=52h/29h are IN, and the extender now runs its MEMORY HUNT

`INT 21h AH=52h` (list of lists) and `AH=29h` (parse filename into FCB) are now
implemented in `x86_dos.c`. The second pass below named them as the suspects and
said the way to grade them was whether the far jump moved. **It did not move —
it disappeared.**

Same command, same flags, same 30 s window
(`probe_report.py --title darkforces --core-flag X86_DOS_EXEC=1 --skip-cost`),
plus a direct `probe.py` sample of `cpu_state_ptr` for the exit state.

**The control was run, and it matters.** The instrumented build reads
`guest_ram_pages=158`, `guest_ram_pages_short=2` — and so does the SAME build
with `x86_dos.c`, `x86_cpu.h` and `x86_hle.c` reverted to master
(`git checkout master -- …`, rebuilt clean, re-measured). `guest_ram_free_at_claim`
is **651,264 on both**. So the two lost frames are the instrument's, already
present on master, and **this change costs zero pager frames**. The baseline run
reproduced the second pass exactly: `guest_exit_reason=4`, 269,100 insns. Every
difference below is therefore attributable to the two new calls and to nothing
else.

**Both calls are answered.** `x86_dos_sysvars_calls=2`, `x86_dos_parse_calls=2`,
both `CF=0`; `AH=52h` returns `ES:BX = 0050:0022`.

**THE FAR JUMP INTO UNWRITTEN MEMORY IS GONE.** It used to die at linear
0x3E0C3, ~0x2E800 bytes past the image, executing `0xFF` filler. It now dies at
`CS:EIP = 023E:0C24` = linear **0x02FC4**, which is **inside the DOS4GW image**
(seg 0x81, 61,604 B, spanning 0x810..0xF6F4). It is executing the extender's own
code instead of blank memory.

**And it has reached a phase it had never reached.** The second pass recorded
that it asked "NOTHING about DPMI, VCPI or XMS". It now asks for all of the
first two, in order:

| probe | × | we answered |
|---|---:|---|
| `INT 2Fh AX=1687h` | 1 | **DPMI** — AX left non-zero, i.e. "not present" |
| `INT 67h AH=DEh` | 2 | **VCPI** — AH=0x84, "function not supported" |
| `INT 67h AH=43h` / `AH=45h` | 1 each | EMS allocate / free |
| `INT 15h AH=BFh AL=DEh` | 2 | unanswered (INT 15h is not even installed) |
| `INT 15h AH=C0h` | 1 | unanswered — get system configuration |
| `INT 21h AH=38h` | 1 | unanswered — get country info |

**THE NEXT BLOCKER, and it is mechanically explained, not guessed.** Told no by
DPMI, VCPI and INT 15h, the extender goes looking for extended memory itself:

* it opens A20 through the fast gate — the port log shows `OUT 0x92, 0x02` and
  `IN 0x92`, and `x86_a20_state=1` with `x86_a20_changes=1`, exactly one real
  transition;
* it loads **`ES=0xFFFF`** (register file at the exit: `ES=0xFFFF`, `DX=0xFFFF`,
  `AX=0x0002` — the value it just wrote to port 0x92);
* and it reads over the 1 MB line. `ES=0xFFFF` reaches linear 0x100000 and up.

There is nothing there. `x86_mem_wrap_1mb()` aliases 0x100000-0x110000 onto the
low 64 KB **only while A20 is MASKED** (`x86_cpu.h`, `X86_HMA_FIRST_PAGE`), and
the guest has just unmasked it; the pager owns `x86_pager_own(0, GUEST_RAM_SIZE
/ 4096)` = **640 KB** (`src/main.c:100,1885`). So the fold entry is invalid and
the access takes `X86_EXIT_MEM_FAULT`. **`guest_exit_reason=5`.**

**IT IS NOT THE WOLF3D MEMORY WALL, which is the other thing reason 5 means.**
`x86_pg_exhausted=0`, `x86_pg_evictions=0`, `x86_dos_load_shorts=0`,
`x86_map_failures=0`. Nothing was refused for want of a frame; the address was
never ownable in the first place.

**`CR0=0`. STILL REAL MODE.** The descriptor walk built in `docs/14` remains
completely unexercised by this title — the claim was tested here rather than
assumed, and it still holds. `x86_xms_detects=0`: XMS is never asked either, so
answering the HMA is upstream of every memory-manager question, not downstream
of one.

### The second pass, which named AH=52h/29h (superseded by the above)

## Star Wars: Dark Forces — MEASURED 2026-08-15 (SECOND PASS): AL=03h is IN, DOS4GW loads whole, and it dies past it

`INT 21h AH=4Bh AL=03h` (LOAD OVERLAY) is now implemented in `x86_dos.c`, and
the measurement below — which named it as the blocker — has been re-run against
it (`probe_report.py --title darkforces --core-flag X86_DOS_EXEC=1`, gwemu,
30 s).

**DOS4GW.EXE now loads, completely.** The overlay is taken (`4Bh/03 -> ok
AX=0x0000`, path log `DF/dos4gw.exe`), `x86_dos_overlays=1`, at load segment
**0x81** with relocation factor **0x81** — Dark Forces passes the two words
equal, which is the ordinary idiom. **The image is whole**:
`x86_dos_load_bytes=61604`, `x86_dos_load_short=0`, `x86_dos_load_shorts=0`.
The guest now executes **269,100** instructions where it previously executed
**0**.

**IT IS NOT THE MEMORY WALL.** `x86_pg_exhausted=0`, `x86_pg_evictions=0`,
`x86_pg_frames_total=158` (2 short only because `X86_PROBE=1` *and*
`X86_DOS_EXEC=1` are both in this build), `x86_seam_overflow=0`. Wolf3D's
`guest_exit_reason=5` (MEM_FAULT) does **not** appear, and an overlay load is
not the same shape after all: it writes into the caller's OWN block, which the
caller has already allocated and touched, so there are no ~30 fresh pages to
find. **This is a Queue B problem, not a Queue A one.**

**Where it now dies.** `guest_exit_reason=4` (UNIMPLEMENTED), `exit_info=0xFF`,
`CS:EIP = 0x3674:0x7983` = linear **0x3E0C3**, `CR0=0` (still REAL MODE). The
32 bytes there are **all `0xFF`** — memory that was never written. The DOS4GW
image spans linear 0x810..0xF8B4, so the guest far-jumped roughly **0x2E800
bytes past the end of the program it just loaded**. It is executing blank
memory; the "unimplemented opcode" is the blank itself, not a missing handler.

**The next blocker, and the prime suspect.** Two DOS calls are still refused,
and both happen in the handful of calls immediately BEFORE that jump:

| probe | × | what it is |
|---|---:|---|
| `INT 21h AH=52h` | 1 | **get the DOS list-of-lists.** A DOS extender uses it to find the DOS data segment and the head of the MCB chain. Refused, it returns CF=1 AX=0001 with ES:BX untouched — and a segment computed from that garbage is exactly the shape of a far jump to 0x3674 |
| `INT 21h AH=29h` | 2 | parse filename into an FCB. Refused immediately before the overlay load |

Neither was implemented in this pass. `AH=52h` is the one to build next, and
the way to grade it is this same re-measurement: if the far jump moves or
disappears, it was the cause.

**It still asks NOTHING about DPMI, VCPI or XMS** — no `INT 2Fh AX=1687h`, no
`AX=DE00h` — and touches exactly one port (`OUT 0x0000`, unmodelled). So the
extender has not yet reached its own memory-manager detection, and the
prerequisite chain further down this file is still not what gates the title.

### The first pass, which named AL=03h (superseded by the above)

The first measurement of this title with a working instrument
(`probe_report.py`, `X86_PROBE=1 X86_DOS_EXEC=1`, gwemu, 30 s;
`build/artifacts/probe_darkforces_exec.md`). It ran, made **67 recorded events
across 17 probes**, and stopped: `guest_exit_reason=4` (UNIMPLEMENTED),
`guest_halted=1`.

**DOS4GW.EXE never loads, and the memory wall is never reached.** What it asked
for and did not get:

| probe | × | what it is |
|---|---:|---|
| `INT 21h AH=4Bh AL=03h` | 1 | **LOAD OVERLAY.** At the time, `X86_DOS_EXEC=1` implemented AL=00h only. **This was the blocker, and it is now implemented — see above** |
| `INT 21h AH=52h` | 1 | get the DOS list-of-lists — a DOS-internals probe |
| `INT 21h AH=29h` | 2 | parse filename into an FCB |

Everything else it touched was answered: 30 × `AH=40h` (write — an error
message), 15 × `AH=4Ah`, 3 × `AH=48h`, the open/read/close set, and 2 ×
`AH=4Ch` (terminate).

**It asks NOTHING about DPMI, VCPI or XMS.** No `INT 2Fh AX=1687h`, no
`AX=DE00h`, no port access at all. So the prerequisite chain below is not
what gates this title today, and **`X86_DOS_EXEC=1` does not change its
outcome**: the next thing to build is EXEC AL=03h (overlay load), and only then
is there a memory question to measure. It did NOT hit Wolf3D's
`guest_exit_reason=5` wall — it never got far enough to be given the chance.

## Star Wars: Dark Forces — BLOCKED on the pagefile, not out of scope (SUPERSEDED by the measurement above)

> **RETRACTED, 2026-08-14: the "out of scope" verdict below is wrong.** It reads
> the zero-read-only-pages measurement as condemning *all* backing stores. It
> condemns **NOR** — XIP from flash needs a read-mostly set — and says nothing
> about a **pagefile**, whose entire purpose is pages that were written (the
> FILE tier). `x86_pager.h` sizes the pager at `X86_PG_PAGES = 2048` = **8 MB**,
> "sized for the space rather than for the 640 KB a real-mode DOS guest can
> address". The 640 KB figure below is not a ceiling, it is the pool that exists
> *because no pagefile is wired yet*. Prerequisite chain:
> **pagefile → XMS/EMS → DPMI → protected mode → Dark Forces.**
> Point 1 below (12.5x) is arithmetic against the wrong denominator; points 2
> and 3 stand as written.

## Star Wars: Dark Forces — the original, WRONG verdict (superseded above)

Halts in its **first frame slice** at `CS:EIP 0393:005E`, executing DOS/4GW's own
message strings (`"…$Not enough memory"`), `exit_reason=4`, opcode `ARPL`.

**The MCB arena is exercised and correct** — 3 × `AH=48h`, 15 × `AH=4Ah`, 0 ×
`AH=49h`, and the one refusal is real DOS semantics: block `0x0071` cannot grow
past an allocated block sitting above it. Real DOS refuses this too.

Why DPMI/VCPI/XMS stays unbuilt:

1. DF wants **~8 MB** flat against the pager's 160 frames = **640 KB**. 12.5x,
   and not a policy problem.
2. The read-mostly premise NOR-XIP rests on is false for every guest we can run
   (see [`12_memory_tiers.md`](12_memory_tiers.md) §3). A protected-mode linear
   heap will not be better than a real-mode game's, and it is 12.5x larger.
3. The dependency chain is XMS + VCPI + DPMI servers, `CR0.PE`, the descriptor
   walk, far transfers and gates, `IRET` across privileges, paging with a real
   TLB — **and then the memory still is not there.**

**Do not half-build DPMI.**

## Epic Pinball — PAST the EXEC wall; next stop is an unimplemented opcode

**UPDATE 2026-08-15, after `X86_DOS_EXEC` became the shipping default.** The
`AH=4Bh` blocker described below is CLEARED. Measured at tier 2 on the shipping
core, `EPIC_PINBALL.dsk`, `timelines/start-preselected-game.tl`:

| | flag OFF (the old default) | flag ON (shipping now) |
|---|---|---|
| `AH=4Bh AL=00h` | refused, `x86_hle_unhandled_func=0x4B` | **answered, CF=0**, `x86_dos_execs=1` |
| `x86_dos_load_bytes` | 36,437 (`PINBALL.EXE` itself) | **83,912** (the CHILD) |
| `x86_dos_load_short` / `_shorts` | — | **0 / 0** — the child landed WHOLE |
| video mode reached | 3 (text: "Please run SETUP!") | **0x13**, `x86_vga_dac_writes=1,536` |
| `guest_exit_reason` / `exit_info` | 4 / **0xFF** (executing unwritten memory after the refusal) | 4 / **0x0F** |
| `guest_insns_total` at the stop | 7,794,871 | 620,904 |
| `guest_ram_pages` / `x86_pg_frames_total` | 159 / 159 | **159 / 159 — the flag costs NO frames** |
| `x86_pg_exhausted` / `_evictions` | 0 / 0 | **0 / 0** |

So the load this title needed is not a memory problem at any point: nothing was
evicted, nothing was exhausted, and nothing came up short. **The two open
questions are now (1) `IN 0x22A` ×64,000 — a Sound Blaster detect spinning on
an unmodelled port, and (2) the `X86_EXIT_UNIMPLEMENTED` with `exit_info=0x0F`
that ends the run.** The screen at the stop is mode 13h and black: it dies
before painting. Neither question is the pager's and neither is the loader's.

Everything from here down is the ORIGINAL measurement, kept because it is the
before-picture and because its DOS-call trace is still the right description of
what the launcher does.

Measured 2026-08-15, gwemu tier 2, `EPIC_PINBALL.dsk` (11.7 MB), stock master,
`X86_PROBE=0` (rings only — the accumulator's RAM_EMU cost is not needed to
answer this and its pager trap is not worth taking).

**It boots, reaches DOS, loads and runs `PINBALL.EXE`, and stops at the launcher.**
The literal screen, mode 3, attribute `0x07` in all 2,000 cells (checked with
`--attr`; nothing is highlighted, nothing is a false "detected"):

```
Please run SETUP!
Press any Key.
```

Then `guest_halted=1`, `guest_exit_reason=4` (UNIMPLEMENTED), frozen at
`guest_insns_total=8,421,787` across three samples 8 s apart.

### The cause, and it is ours

`x86_hle_unhandled_vector=33` (0x21), `x86_hle_unhandled_func=75` (**0x4B**).
`probe.py --dos-log` gives the whole conversation — four file calls, and they
are the textbook prelude to an EXEC:

| # | call | result |
|---|---|---|
| 0 | `47h` getcwd | ok |
| 1 | `3Dh` open `PINBALL.EXE` (itself) | ok, handle 5 |
| 2 | `42h/02` lseek to end | ok, `AX=0x9255` = 37,461 = the exact file size |
| 3 | `3Eh` close | ok |
| 4 | `4Ah` resize own block to `0x926` paras (37,472 B) | ok — **shrink-to-self** |

Then `AH=4Bh`, which falls off the end of the `INT 21h` switch in `x86_dos.c`
and returns `CF=1, AX=0x01` ("invalid function"). The launcher reads that as
"cannot start the child", prints the SETUP message and waits for a key
(`x86_io_log`: one port access in the whole run, `IN 0x60` = 0x39, space, from
the timeline). After the key it runs off into an unimplemented opcode and halts.

**Negative control, seen to succeed where we fail:** the SAME `.dsk` under
DOSBox-X (`ref_dos.py --engine dosbox --launch PINBALL.EXE`) reaches mode 13h
and paints the "EPIC PIN…" dot-matrix intro
(`build/artifacts/ref_epic_db.png`). The image is configured and correct; the
message is not the disk's fault. (QEMU on the same image also reaches mode 13h
but paints nothing — a reference limit, not evidence either way.)

**This is the same blocker as Dark Forces' "Not enough memory"** — a second,
independent title dying on the one missing DOS call. `x86_dos_exec()` (the MZ
loader) already exists and is what loads the initial program; what is missing is
the `AH=4Bh` entry: the ASCIZ path at `DS:DX`, the EXEC parameter block at
`ES:BX` (child cmdline and two FCBs), a child PSP whose parent field points at
the caller, and save/restore of the parent's `CS:IP`/`SS:SP` so the child's
`AH=4Ch` returns to it rather than exiting the guest. `AL=03` (load overlay) is
a separate, smaller case.

### Everything else it asked for, we answered

| counter | value | reading |
|---|---|---|
| `x86_video_mode_byte` | 3 | text only |
| `x86_video_mode_log` | tag 6 request 3 (old 4), tag 1 adopted 3 / 80 col, tag 3 renderer followed, is_text=1 | **no tag 5** — no mode was refused, and no graphics mode was ever asked for |
| `x86_xms_detects` / `_calls` | 0 / 0 | never got that far |
| `x86_ems_calls` | 0 | ditto |
| DPMI `2Fh/1687h`, VCPI `DE00h` | never issued | — |
| Sound Blaster ports 0x220 / 0x388 | **never touched** | the only port access in the run is `IN 0x60`. `SOUNDCRD.INF` on the disk reads `REAL16 MAS ISBSERIES MDRV004R.MUS` — a Miles AIL/SB-series driver is configured, so SB probing is what to expect *after* EXEC lands, but this title has produced no evidence about it yet |
| `x86_pg_frames_total` | 160 | healthy; the guest ran |
| `x86_pg_evictions` / `_fill_file` / `_exhausted` | 0 / 0 / 0 | no thrash, nothing near the working set |
| `x86_arena_largest` | 110,592 | untouched, text mode only |

Memory is **not** this title's problem and there is no evidence it ever will be:
it never allocated past its own image. `PINBALL.EXE` is a 37 KB launcher; the
13 tables are separate `EP1..EP13.EXE` of ~220–300 KB each, each with a `.DAT`,
which is precisely why it needs EXEC — and why, once EXEC lands, the next
measurement to take is a table's working set, not the launcher's.

**Next blocker, one line: `INT 21h AH=4Bh`.** Nothing beyond it has been
measured, so no further claim about this title is supported.
*(Superseded — see the UPDATE at the top of this section. The prediction in the
row above was right: SB probing is exactly what showed up once EXEC landed.)*

## Protected mode — SIZED, not started

`CR0` has never been non-zero on this tree. Sized against
`references/tiny386/i386.c` (C line counts; ours is higher, being Thumb-2):

| piece | tiny386 | ~lines |
|---|---|---:|
| `read_desc` + `set_seg` + `clear_segs` | `1048-1135` | 88 |
| paging + software TLB | `527-724` | 198 |
| far `CALL`/`JMP` + gates, `task_switch` | `4214-4509` | 296 |
| interrupts with privilege change | `4510-4848` | 339 |
| `IRET`/far `RET` across privileges | `4849-5041` | 193 |
| `LAR`/`LSL`/`VERR`/`VERW`, `check_ioperm` | `3335-3360`, `3536-3620` | 111 |

**~1,225 lines**, plus the CPL and limit checks tiny386 macro-expands into
`cpu_exec1` itself — which for us means touching the R6 descriptor contract in
every handler, and that is not a line count.

`x86_cpu.h` already reserves 2 KB of `tlb_*` arrays that **nothing references**;
that is the intended backing.

**V86 is NOT needed.** It exists to run real-mode DOS under a supervisor, and
our INT 21h never reaches guest code. tiny386 carries 18 `VM`-flag sites we
would not have.

**Do not start without a title that needs it AND fits in 640 KB.** DF does not
fit, and nothing else in `roms/dos` sets `CR0` at all.

## TOPBENCH — the interpreter benchmark

Runs in **text mode**, which makes it a clean interpreter benchmark that
reflects none of the graphics work. Its six sub-timings read `65535` until port
`0x20` answered — see
[`11_pit_and_interrupt_controller.md`](11_pit_and_interrupt_controller.md) §3.

## Mortal Kombat — BLOCKED on the HMA, after four walls in the first 200 instructions (SUPERSEDED: the HMA is backed, see the entry above)

First measured 2026-08-15, gwemu tier 2, `timelines/start-preselected-game.tl`.
The `.dsk` mounts, the program LOADS (`dsk_startup_ok=1`, `dsk_exec_rc=0`), the
BIOS sets text mode 3 — and then the title dies inside its own C-runtime
startup, before it has drawn one pixel or asked for anything a device could
answer. **Every counter that names a subsystem read healthy the whole time**:
`x86_pg_frames_total=160`, `_exhausted=0`, `_evictions=0`, `x86_arena_largest=110,592`,
`x86_xms_detects=0`. The video mode log holds three events for the entire run —
the BIOS's own boot set-mode and the renderer following it — and no tag 5, so
there is nothing refused and nothing to render. This title has never asked for
video, memory, XMS or sound. It never got that far.

`guest_insns_total=0` is the shape to recognise: the guest halted inside its
FIRST frame slice, and the slice budget is only banked for a live guest, so a
title that dies this early reports zero rather than a rate.

### The four walls, in the order the guest hit them

Each was found the same way — `cpu_state_ptr->exit_reason`, `+2200` for the
offending opcode (`CPU_EXIT_INFO`), and `probe.py --guest` to disassemble
around CS:EIP. Three were ours and are FIXED here; the fourth is not an opcode.

| # | at | opcode | what the guest was doing |
|---|---|---|---|
| 1 | `023E:68CE` | `0F 01 /4` **SMSW** | reading CR0.EM before probing for an x87 |
| 2 | `023E:68D1` | `DB E3` **FNINIT** | the x87 detection itself |
| 3 | `023E:6624` | `0F BA /7` **BTC** | toggling EFLAGS.ID and .AC to identify the CPU |
| 3b| `023E:664D` | `0F A2` **CPUID** | ...having found ID writable |
| 4 | `023E:0C24` | *not an opcode* | `REPE CMPSW` against `FFFF:0030` — the A20 wrap test |

Walls 1-3b are one continuous routine: `PUSH -1 / SMSW AX / TEST AL,4 /
JNZ / FNINIT / FNSTSW [BP-2] / CMP [BP-2],CL`, then `PUSHF / BTC EDX,21 /
BTC EDX,18 / POPF / PUSHF / CPUID`. It is the standard Microsoft/Borland
startup preamble, **not** anything specific to this title, which is why the
fixes are worth more than one game: any DOS title built with those runtimes
runs it.

- **SMSW** was `.exit_loop` because `0F 01`'s ModRM walk was unconditional and
  SMSW is the only member of the group with a register form.
- **D8-DF** now behave as a machine with NO COPROCESSOR FITTED: the escape is
  ignored and only its operand is stepped over, which is what makes the
  detection above conclude "no x87" instead of killing the guest. `x86_x87_escapes`
  counts them, and **it is the number to read before trusting a picture from a
  new title** — a detection costs a handful (this title: **2**), a title
  actually computing in floating point runs millions and is silently wrong.
- **`0F BA`** (BT/BTS/BTR/BTC, imm8 form only) and **`0F A2`** (CPUID leaves 0
  and 1, a 486SX with no feature bits) are implemented. The register-source bit
  ops `0F A3/AB/B3/BB` address a bit STRING and are deliberately still loud.

Fourteen cases in `tests/test_opcodes.py` (274-287) cover all of it; all
fourteen were SEEN TO FAIL with the table entries reverted.

### The single next blocker: the HMA has no backing when A20 is open

The guest opens A20 through the **fast gate at port 0x92** (`x86_a20_changes=1`,
`x86_a20_state=1`; the io log's only two entries are that IN and that OUT) and
immediately runs the classic wrap test — `ES=0xFFFF`, `REPE CMPSW` at
`FFFF:0030` against `0000:0020`, i.e. linear **0x100020**. That page is not
mapped: `x86_mem_wrap_1mb()` writes `FOLD_INVALID` across the sixteen HMA pages
whenever the gate is open, on purpose ("nothing here owns it yet"), and the
pager's `owns()` does not cover them either. Result: `x86_pg_unmapped=1`,
`X86_EXIT_MEM_FAULT`, dead. `x86_seam_overflow=1` is the same event seen from
the seam — a straddle whose low granule has no frame cannot be served.

**Size of the fix: 16 pages = 64 KB, against 144 free frames.** It is not a
memory problem and not an XMS problem — `x86_xms_detects` is still 0, the guest
has not talked to a driver at all. The pager already has an `aux_first/aux_count`
region beside its own; giving the HMA sixteen ordinary demand-paged frames is
the shape of it. Whether the wrap test then PASSES is the second question and
the more interesting one: the guest is checking that the gate really opened.



CGA mode 4, pixel-exact. Its skill menu wants K/H/T/A, which no button can send;
that needs the on-screen keyboard, not a mapping change.


## Fixed this session — moved out of STATUS.md, which is state not history

Kept because each carries a MEASURED cause worth not re-deriving.

| | |
|---|---|
| ~~KEEN4 hangs, then the WATCHDOG resets~~ **FIXED.** The watchdog was refreshed ONCE per frame, so its ~472 ms window covered a WHOLE iteration and one overlong GUEST stage reset the device; `0xbad00005` is `BOOT_MAGIC_BSOD \| BSOD_WATCHDOG`, and every PC sampled "in the wedge" was retro-go's BSOD wait-for-a-keypress loop. `FRAME_PHASE()` now refreshes per STAGE, so a slow stage is a slow frame. Silicon: **51.6 s, 55.7 M insns, 1,128 frames, no reset**; negative control `X86_PHASE_WDOG=0` wedges twice. **There is no runaway loop** — `cpu_state.pit_counter` is FROZEN across the freeze, so the guest is not executing; the "one SLICE never returns" reading is retracted. Keen's remaining defect is the PICTURE (card shortfall), not the crash | [`docs/13`](docs/13_title_status.md) |
| ~~KEEN4: EGA card OR guest frames, pick one~~ **THE DICHOTOMY IS GONE — the reserve is now MEASURED, once per frame, not compiled in.** `reserve = max(FLOOR 64, x86_pg_resident_peak + 8, ratchet)`; the shrink fires on FREE FRAMES below the margin (an eviction is the cost, not the warning) and a shrink borrows nothing. MEASURED ON HARDWARE, KEEN4: the intro gets the **65,536 offsets/plane** the Galaxy engine wants (66 frames borrowed while idle), the watermark hands every frame back as the set grows, and gameplay runs on the whole pool — `evictions=0`, `wb_file=0`, `wb_us_worst=0`, `donated=0`, `frames_total=158`. WOLF3D untouched on hardware (`donated=0`, `shrink_signals=0`, golden 0.000%). `X86_PG_POLICY=0` restores the old fixed reserve; controls in `scripts/run_paging_controls.py` | [`docs/12`](docs/12_memory_tiers.md) §5b |
| ~~ONE 4 KB PAGEFILE WRITEBACK COSTS UP TO 433 ms~~ **FIXED**: `x86_pg_wb_us_worst` **433,509 -> 26,027-48,748 us**, mean per writeback **~59 -> 11.4 ms**, KEEN4 on hardware at reserve 90 (81 writebacks). NOT the chain walk -- 85 of 86 writes landed in allocated clusters and all seeks together were 340 ms against 4.0 s in `f_write`. `X86_PGF_BENCH=1` asked the card: appending 4 KB is 12-14 ms, REWRITING recently-written sectors is 60-67 ms, and 32 KB in one call costs what 64 writes of 512 B cost -- so batching/write-behind buys NOTHING and the fix is WHERE a page lands. Slots now come from a ring (`x86_pgfile.c`), map moves only on a write that succeeded | [`docs/12`](docs/12_memory_tiers.md) §1b |
| ~~KEEN4 gives 60 of 160 frames to the arena~~ **FIXED**: `X86_PG_DONATE_RESERVE` 100 -> 134 (its measured peak). Pool stays 160, evictions 84 -> **0**, pagefile writes 82 -> **0**, on hardware too. Cost: the card is the arena's 35,840 offsets, not the donated 61,440 — and donation is now **inert for this title by construction** (a claim cannot span regions, so beating the arena needs ~35 frames; Keen spares 26) | [`docs/12`](docs/12_memory_tiers.md) |
| ~~Sound: nothing is audible~~ **PRINCE OF PERSIA NOW PLAYS DIGITISED SOUND ON SILICON.** `x86_sb_starts` **0 -> 18**, `x86_sb_samples` **92,981**, `underruns` 0, 8,264/11,111 Hz, `x86_audio_peak_max` **991**. The bug was **DSP command `F2h`** -- "assert the IRQ, no transfer" -- answered as unknown, so PoP's IRQ autodetect failed and it fell back to the PC speaker. Implementing it was half: the assert was queued for the next sub-slice and PoP had already re-masked (`irqs_delivered`=0, `irqs_masked`>68,000, while `x86_pic_imr_ever`=0x10 PROVES it did unmask), so `F2h` is now delivered at the port write -- and `x86_sb_raise_irq()`'s `cpu->eflags` guard is STALE inside a slice (IF is in R11), so the boundary is armed unconditionally. **WOLF3D: OWNER-CONFIRMED WORKING** once digitised sound is enabled in the TITLE'S OWN setup. The earlier `x86_dma_writes`=0 reading was correct and was not a defect — its shipped config simply had digitised sound off, so `SDL_SBPlaySeg()` was never called. Nothing to fix here. **Music is still silent: that is AdLib FM, which is not built** | [`docs/03`](docs/03_devices_and_hle.md) 4e-bis |
| ~~Sound: the host chain has never been checked~~ **IT WORKS, AND IT WAS 12 dB TOO QUIET.** `x86_audio_submits`/`_peak_last`/`_peak_max`/`_volume` say a buffer goes out every frame and `dma_counter` consumes it -- with `peak_max`=0, i.e. the chain was being handed silence. `X86_AUDIO_TONE=1` is the standalone partition. The 12 dB: `common_emu_sound_get_volume()` is ALREADY a fraction of full scale and we scaled it again by `>>11` (1.5% FS at the default level 4 of 9; tama uses `>>10` and documented why), then HALVED the card again to duck it under music that is not built. Both gone | `src/main.c` |
| ~~INT 21h AH=4Bh (EXEC) is built but OFF~~ **`X86_DOS_EXEC` NOW DEFAULTS TO 1.** AL=00h and AL=03h are live in the shipping core; AL=01h is still refused. **The "honouring AL=00h kills WOLF3D" finding DID NOT SURVIVE RE-TESTING** — see Retractions | `Makefile.core` |
| ~~Dark Forces and Mortal Kombat stop at the HMA~~ **FIXED: THE HMA IS BACKED WHEN A20 IS OPEN.** The sixteen pages at 1 MB are ordinary demand-paged guest pages through the pager's SECOND WINDOW (the one EMS uses); masking A20 drops the window and the ALIAS is what the guest sees, and the pager may not write those fold entries while it is masked. Evictable, NOT pinned. **MK's A20 wrap test now PASSES** (0 -> **42,188** insns, `x86_a20_changes=3`, `x86_pg_unmapped` 1 -> 0); **Dark Forces is past the memory hunt** (`guest_exit_reason` 5 -> 4, 102,517 insns, 30 file calls, loading its data). **`CR0=0` on both: STILL REAL MODE, so the descriptor walk is STILL unexercised by any title -- tested, not assumed.** `INT 15h AH=88h` now answers **64 KB**, which is exactly what is backed. COST: `__CORE_ITCM_CODE_END__` unchanged at **0xD2D8**, `__CORE_BSS_END__` +264 B, and those 264 B cross a 4 KB claim boundary -- `guest_ram_pages` **158**, `_pages_short` **2** (stock master: 159/1). One frame for sixteen on demand. Both titles now end identically on `X86_EXIT_UNIMPLEMENTED` `exit_info=0x0F` at `0081:032F`, as does Epic Pinball | [`docs/13`](docs/13_title_status.md) |
| ~~Three titles die at the identical address~~ **RETRACTED — IT WAS OUR INSTRUMENT, AND LMSW WAS THE BUG BEHIND IT.** `.op_0f_0F` loads the second opcode byte into R1 and `.exit_loop` reports R0, which still held `0x0F` — so EVERY unimplemented two-byte opcode reported `exit_info=0x0F`, and the reported `CS` was never read from the CPU at all. Three titles, three DIFFERENT opcodes: Dark Forces and Mortal Kombat on **`0F 01 F0` = LMSW AX**, Epic Pinball on **`0F AF` = IMUL Gv,Ev**. Both implemented; `test_opcodes` **295/295** with 8 new arms, all 8 **seen to fail** backed out. **EXEC is exonerated** — the relocation factor and `dos_load_mz()` were never in the path | [`docs/13`](docs/13_title_status.md) |

