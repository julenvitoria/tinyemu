# Status

Last verified 2026-08-15. How to run: [`TOOLING.md`](TOOLING.md). Roadmap and
backlog: [`PLAN.md`](PLAN.md). Design: [`docs/`](docs/).

**This file is state, not history.** Under 100 lines — a status doc nobody reads is
where numbers go stale.

## Verification state

| Tier | State |
|---|---|
| 1. gwemu bare-metal | `test_opcodes` **295/295**; `test_video` **25/25 arms** at the shipping `EXTRA_PAGES=1`, Mode Y and EGA 0Dh **64000/64000**; `test_seam`, `test_pit`, `test_dos`, `test_paging` **205** (32 the HMA, 16 the memory policy, 24 the pagefile ring; `run_hma_controls.py` 2/2 caught), `test_nor`, `test_sysinfo`, `test_xms` **47/47**, `test_ems` **40/40**, `test_text`, `test_keyboard`, `test_osk`, `test_disk_cache`, **`test_pmode` 80/80** (protected mode; 10/10 negative controls caught by `scripts/run_pmode_controls.py`) -- and the walk is no longer theory: **two titles now run it**, see below |
| 2. gwemu + retro-go | `core_dtcm_ok=1`, bonus pool 153,600 B, 1,000,001 insns — matches tier 1. **XMS is live and a shipped title uses it**: WOLF3D detects it, allocates and moves 430,636 B in 278 moves, `xms_opens=1`, 0 errors, at a 4% frame cost ([`docs/12`](docs/12_memory_tiers.md) §6) |
| 3. Hardware | ~**2.8 M insn/s** stock, ~**3.8 M** overclocked, 59 fps. ABI VERIFIED on silicon. **Disk reads are NOT a live cost — they freeze at t+36 s and never move again; SylverB's CMD18/DMA work is already in the flashed firmware.** The live cost was the pagefile WRITE, now 5x cheaper (`docs/12` §1b) |

`scripts/run_planar_controls.py`: all 6 negative controls caught + the LEARN positive
control. **Golden gate: WOLF3D 0.000%**, ALLEYCAT 0.874% = the moving cat, KEEN4
known-bad.

**Known-failing, pre-existing, not news:**

- `test_nor.py` — the arm never runs. PRE-EXISTING, and the cause is now NAMED,
  not just reproduced: the arm's opening `memcpy` to `0x90000000` takes a
  **precise bus fault** (`CFSR` = BFARVALID|PRECISERR, `BFAR` = 0x90000000) and
  spins in `Fault_Handler_C`. Nothing in the bare-metal harness configures
  OCTOSPI memory-mapped mode, so the arm's stated premise — "the extflash window
  is ordinary backing memory under gwemu" — is **false without that setup**. The
  handler does not clear `run_nor_flag`, which is why it reads as "never ran"
  rather than as a fault. Fix = set OCTOSPI up in `tests/main.c`; until then the
  arm proves nothing. `test_nor_wdog.py` (new) covers the readback's watchdog
  heartbeat WITHOUT depending on that window.
- `test_screen.py` — text-mode CLUT assertion, entries 17-19.
- `test_core_retrogo.py` — blitter/CLUT assertions whenever a real `.dsk` is on the
  card: they expect `demo_guest`'s pattern, which only runs with no disk.

## WHERE THE CYCLES GO — read this before optimising anything

Wolf3D, Mode Y, **hardware**, gated gameplay window, **% of WALL** throughout.
Method and menu control: [`docs/09`](docs/09_frame_pacing_and_clocks.md) §8.

| block | % wall |
|---|---|
| idle waiting for the panel (vsync-quantised, not freely convertible) | 27.0 |
| **planar store hook** | **25.0** |
| **interpreter excluding the hook** — of which: | **47.1** |
| — ordinary opcodes, the guest's own work, **~57 cyc/insn** ← LARGEST BLOCK | 29.4 |
| — `88` aperture byte store 10.3, `AB` REP STOSW 3.3, `89` word store 2.3 | 15.9 |
| — VGA `OUT`/`IN`, **1,242 cyc each** (`ega_shadow_rebuild()`) | 1.5 |
| Mode Y blit + paint | 0.84 |
| residual (root-zone self-time, measured not derived) | 0.05 |

An aperture byte store costs **171 cyc**; the same store to ordinary memory
costs **55**. ~1.67 M insn/s, ~16 presents/s.

**Open and sized:** `REP STOSW`'s per-element hook (~3%; the page cannot change
mid-`REP`, so the decision is resolvable once), aperture reads (~4.5%), the
1,242-cyc `OUT`.

### Measured and REJECTED — do not re-propose

The full list with its numbers is in [`CLAUDE.md`](CLAUDE.md), which every
agent reads first: DMA2D for the blit, `planar_store` in assembly, planes in
DTCM vs AXI, `x86_code_reloc`, `SEAM_CHECK`, `IDIV`/`IMUL`, disk reads. Each
was plausible and each measured to nothing.

## Budgets — measure these before and after any core change

| | value | note |
|---|---|---|
| ITCM | ~**11.3 KB** free of 65,536 | `__CORE_ITCM_CODE_END__` = **0xD580** (54,656 B used; 0xD2D8 before LMSW/IMUL added +680 B, all cold) — MEASURED on this tree; the 0xCD08 quoted here for several passes was stale. The protected-mode walk cost **+912 B**, all of it cold: the out-of-line `SET_SREG_PMODE`, group 6/7 (`LLDT`/`SLDT`/`LGDT`/`LIDT`) and the `MOV CRn` transition |
| DTCM | **5,856 B** free — `core_dtcm_headroom_max`, MEASURED on the shipping core, not the pre-aperture 63,268 that is still quoted around the tree | the pool ends at `__dtc_padding_end__` = `0x2001A000`, NOT `0x20020000` — the 24 KB above it is the main stack ([`docs/12`](docs/12_memory_tiers.md) §8). **DTCM is not an escape hatch for instrument storage**: 5,856 B is the whole of it, and `.dtcm_bss` is a `TEST.ld` section only — the shipping core reaches DTCM through runtime `dtc_malloc` + a pointer, never a static |
| RAM_EMU | `arm-none-eabi-nm build/core/dos_core.elf \| grep __CORE_BSS_END__` — it MOVES every merge, so measure it, do not quote it. **THE CLIFF IS ALREADY CROSSED ON STOCK MASTER with no instrument enabled**, and it is now TWO frames deep, not one: measured at tier 2 on the STOCK master core (`b5b4ae3`), `guest_ram_pages`=**158**, `guest_ram_pages_short`=**2** — the "1" quoted here for several passes is stale, RAM_EMU has grown since. A 160-frame pool wants **656,640** B (stride **4,104**, not 4,096 — the granule-seam slack). Any earlier "slack" figure in this row was stale | **Exhaustion is no longer silent, but it IS being ignored.** The pool is sized to what is free and publishes `guest_ram_pages` / `_pages_short` / `_free_at_claim`. **Read `_pages_short` after adding ANY RAM_EMU storage** — one frame per 4,104 B comes out of the guest. It is already 1 before you add anything, and that alone **disables the NOR snapshot**, whose `guest_ram_pages != 160` guard then refuses every attempt (`nor_snap_result`=2). `X86_PROBE=1` costs one further frame |
| arena | 143,360 B | check `x86_arena_largest`, not just `_free` — claims fragment |

## What is broken right now

| | where |
|---|---|
| **KEEN4 has no music, and it is NOT the synthesiser** — `x86_opl_writes`=264 (the AdLib detect plus id's 245-register zero pass), `keyons`=**0**: its driver never writes a note. Config right (`CONFIG.CK4` `SoundMode=2`/`MusicMode=1`), detection works (`opl_timer_expiry=1`), and WOLF3D drives the identical path and SOUNDS. **Lead: `INT 08h` reaches our stub ~16/s = the 18.2 Hz chained BIOS rate, not the 140 Hz music tick Keen should program.** Grade the ISR rate | [`docs/13`](docs/13_title_status.md) |
| **Sound costs 6 pager frames** — `guest_ram_pages` 156 -> **150**, `_pages_short` 4 -> **10**. Judged payable only because `x86_pg_evictions` stayed **0** at 150 across a full WOLF3D run on BOTH tiers. `X86_OPL_FM=0` returns every byte with detection intact. Silicon: ~5.9% of wall for Wolf3D's real music, 10.4-13.8% worst case | `Makefile.core` |
| **The game port is unmodelled** — WOLF3D issues **2.35 M `IN 0x201`** reads, larger than everything sound-related combined | `x86_io.c` |
| Wolf3D's cached pictures render as noise (off-screen VRAM cache too small) | [`docs/13`](docs/13_title_status.md) |
| Wolf3D paints mid-draw frames in gameplay (`x86_modey_mid_draw`) | [`docs/08`](docs/08_vga_mode13h.md) §4 |
| **Wolf3D's WEAPON SPRITE is corrupt** — a stable band of wrong pixels across the pistol at the spawn. DETERMINISTIC, so gradeable; may share a cause with the "rare" stage-select corruption | Queue B1 |
| **ONE 4 KB PAGEFILE WRITEBACK COSTS UP TO 433 ms ON SILICON** (`x86_pg_wb_us_worst`) vs 23 ms for a sector read, and GROWS over a session. The per-page `fopen`/`fclose` is fixed; `pgf_us_seek` vs `_us_write` says what the rest is, `FF_USE_FASTSEEK 0` the suspect. Gates Keen and anything EMS/XMS-backed | `src/main.c` |
| **The guest-RAM -> NOR snapshot's watchdog BSOD: TWO un-kicked stretches, both OURS, and NEITHER is the flash.** The firmware's transport is watchdog-safe throughout — `OSPI_Program()` kicks per 256-byte page and its comment names the **~472 ms WWDG window**, `OSPI_Erase()` kicks per command, the SPI SD driver kicks in its busy-waits. What is NOT kicked: (1) the snapshot is a **MENU callback**, so the frame loop's once-per-frame `wdog_refresh()` is not running for the whole 1,595–1,614 ms (gwemu, where OSPI erase/program AND SD busy are both unmodelled, so silicon is strictly worse) — and inside it `x86_nor_attach()`'s **640 KB XIP readback** touches neither driver, so nothing kicked across 160 memcmps out of OCTOSPI; (2) **the first slice AFTER the attach** — every guest page is in NOR the instant it returns, so every access AND every instruction fetch traps and is served from OCTOSPI, while `max_slice` is still the value the regulator tuned for AXI. The regulator corrects by 1/8 per frame and cannot help with a frame it does not survive. **This is the shape that fits the owner's report** ("seems to work, THEN bluescreens"). FIXED: heartbeat once per page across the readback (hook, so `x86_nor.c` still knows nothing about the ABI), kicks on the placeholder writer and the relocate callback, and the regulator handed back to `MAX_SLICE_FLOOR` with the debt cleared at attach. **`nor_snap_wdog_gap_ms` is the instrument that settles it in ONE tier-3 run** — worst gap between kicks, read against 472 ms. **NOT YET MEASURED ON SILICON**: the device was held by another agent's openocd for this whole session. **AND THE PATH IS CURRENTLY DEAD FOR AN UNRELATED REASON** — measured at tier 2 on STOCK master: the snapshot fills correctly (`nor_snap_cb_bytes`=655,360, addr 0x90000000) and is then REFUSED with `nor_snap_result`=**2** (NO_RAM), because `guest_ram_pages` is **159** not 160 (RAM_EMU row). So the BSOD is not reachable on today's tree at all; it was reachable on the owner's build, before RAM_EMU grew. `nor_snap_wdog_gap_ms` reads **77 ms under gwemu**, which is a FLOOR — gwemu models neither OSPI erase/program nor SD busy | `x86_nor.c`, `src/main.c` |
| **Sound: FM SYNTHESIS is still absent.** Digitised SB16 playback is built and graded end to end -- 8237 pair, IRQ 5 through the 8259 with both acknowledgements, the CT1745 mixer, DSP 4.05 and the 0xBx/0xCx block, `BLASTER` set, rate conversion into the frame-quantised buffer. What is NOT built is OPL tone generation, so a title whose MUSIC is AdLib is silent while its digitised effects play. ADPCM likewise runs, completes and interrupts, but emits silence (`x86_sb_adpcm_silenced`) | [`docs/03`](docs/03_devices_and_hle.md) §4f |
| **EPIC_PINBALL's 64,000-read spin on `IN 0x22A` is FIXED** (6 reads). It now resets the DSP twice, issues 5 commands with **none unknown**, programs the mixer and a 22,050 Hz rate -- and dies EARLIER, at 483,354 instructions, on `guest_exit_reason=4` `exit_info=0x0F`: **an unimplemented opcode, not a sound defect.** That is the next thing in its way | [`docs/07`](docs/07_opcode_coverage.md) |
| **WOLF3D and PRINCE OF PERSIA have sound** (digitised + FM music). Retro-go's Pause overlay is unreadable over 256-colour modes | [`docs/08`](docs/08_vga_mode13h.md) §1 |
| **`X86_VPROF=1` DOES NOT FIT and fails SILENTLY** — adds ~1,080 B to RAM_EMU, `guest_ram` returns NULL, `core_ready` stays 0, the frame loop paints nothing. Reproduced on STOCK master (KEEN4: `core_ready`=0 at 1, =1 with 157 M insns at 0). **THIRD instance of this shape** — see the RAM_EMU row in Budgets | `Makefile.core` |
| **THE DESCRIPTOR WALK IS EXERCISED BY A TITLE AT LAST — `CR0=1` on Dark Forces AND Mortal Kombat.** LMSW, not `MOV CR0`, is what a DOS extender issues (it must also run on a 286), which is why the walk sat unexercised for so long. It HELD: both come out with `CS=0x0070`, a real GDT selector with base `0xA900` rather than `sel<<4`, and `SS=0x0020`. Both now stop on the SAME real thing — `exit_info=0xCD`, `INT imm8` with PE set — which is protected-mode IDT gate dispatch, i.e. PLAN Queue B5, now **measured demand from two titles**. Epic Pinball is unrelated and still real mode, stopped on `PUSH FS` (`0x0FA0`). Cost: ITCM **0xD2D8 -> 0xD580**, `__CORE_BSS_END__` **unchanged** — zero RAM_EMU, zero pager frames | [`docs/13`](docs/13_title_status.md) |
| **Interpreter gaps**: seg override before `REP` survives one element; `moffs` uses operand not ADDRESS size; `MOV SS,x` does not inhibit interrupts; 5 handlers ignore `0x67`; 13 slots exit (x87, `BOUND`/`ARPL`, `XLAT`, `WAIT`, `INT1`) | [`docs/07`](docs/07_opcode_coverage.md) |

## Protected mode — the descriptor walk is IN, real mode pays NOTHING

`CR0.PE` no longer traps out: `LGDT`/`LIDT`/`LLDT`, GDT/LDT fetch, type/DPL/
present with the right vector and error code, cached per-segment descriptor,
`CR0.PE` both ways, same-privilege far JMP. `test_pmode.py` **80 checks**;
`run_pmode_controls.py` **10/10 seen to fail** when compiled out. Zero
real-mode cost proven by DISASSEMBLY DIFF, not argued — `.fetch_decode_loop`
and every `.op_*` handler byte-identical. **`test_benchmark` cannot decide
this**: under gwemu it spans 10.30–11.88 M cycles on the BASELINE alone. NOT
built: gates, `IRET` across privilege, stack switches, paging, per-access limit
enforcement. [`docs/14`](docs/14_protected_mode.md).

## Retractions worth keeping

One line each; their value is stopping a wrong answer being re-derived.

- **"WOLF3D DOES NOT DETECT THE SOUND BLASTER" IS FIXED, AND IT WAS NEVER AN SB DEFECT.** The card, `BLASTER=A220 I5 D1 H5 T6`, and the PSP link were all correct -- PSP:002C read **0x0071** out of guest memory ON HARDWARE, pointing at the block that holds the string. What was wrong is the CLOCK: WOLF3D's AdLib probe waits by reading the FM status port ~135 times, an ISA bus cycle is ~1 us **whatever the CPU is**, and we charged those accesses at the instruction rate -- `x86_opl_t1_gap` **56 ticks against the 96** an 80 us timer needs. Its SB probe is DOWNSTREAM of that conclusion, which is why it touched no 0x22x port at all. The FM card now carries its own clock (`X86_ISA_IO_TICKS`); on SILICON, `t1_gap` 56 -> **200**, `opl_timer_expiry` 0 -> **1**, `opl_writes` 6 -> **253**, and **`x86_sb_resets` 0 -> 1, `x86_sb_cmds` 0 -> 2, `cmds_unknown`=0**. `X86_ISA_IO_TICKS=0` puts every one of those back, seen to fail on hardware. The earlier "silicon is 4x slower so it should get there" arithmetic is DISPROVED, not outstanding: measured on the device, expiry was 0 there too. [`docs/03`](docs/03_devices_and_hle.md) 4e
- **The WOLF3D golden was RE-BLESSED, to the title screen.** Not a picture change: with sound detected the title takes its sound-present startup path and its intro is one screen further along at the fixed 250 M-instruction capture point. `repeat --runs 3` gave three identical frames first. ALLEYCAT unchanged at its pre-existing 0.874%.
- **A general ISA-bus-time model was built, measured and DEFERRED.** Charging every port access into `pit_now` is physically right and free off the I/O path, but it moves the machine's clock **+11%** (1.16 M bus ticks vs 10.4 M instruction ticks over 48 M insns, WOLF3D) because a 0x3DA retrace spin really does cost bus time -- a machine-pacing change to grade on its own, not a rider on a sound fix.
- **"EXEC AL=00h KILLS WOLF3D" WAS WRONG, AND THE NUMBER THAT DISPROVES IT IS `x86_dos_execs`.** WOLF3D never calls AH=4Bh in the graded window at all: `execs=0`, `overlays=0` over 340 M instructions, and the `x86_dos_load_bytes=109,927` attributed to a child EXEC is WOLF3D.EXE itself, loaded by the LAUNCH path (`dos_load_mz()`), which is not the EXEC switch. The golden is **0.000% with the flag on**, `x86_pg_exhausted=0`, `evictions=0`, `load_shorts=0`, and `x86_pg_frames_total`/`guest_ram_pages` read **159 with the flag both ways** — so it costs no frames either (`__CORE_BSS_END__` 0x2405EBF8 -> **0x2405F120**, +1,320 B, absorbed by existing slack). The reported "`insns_total`=0 with every other counter healthy" is the RAM_EMU starvation signature in the Budgets row above, which took out three instrumented builds in one session. **A flag was held off for several passes on a diagnosis nobody re-ran the cheapest counter against.**
- **`references/tiny386` HAS NO PROTECTED-MODE PERMISSION CHECKS.** `set_seg()` (i386.c:1076) carries `// TODO: various permission checks` and does no DPL/RPL/CPL validation. Right SHAPE, wrong RULES; DOSBox-X's `CPU_SetSegGeneral` (src/cpu/cpu.cpp:2976) is the authority on the rules.
- **Keen 4's levels are NOT impossible.** The verdict quoted the prior port's budget as if it were this hardware's. [`docs/13`](docs/13_title_status.md).
- **Do not derive the frame period from `guest_fps_achieved`.** It is a runaway: overrun lowers fps, raising the period, raising the budget. [`docs/09`](docs/09_frame_pacing_and_clocks.md) §2.
- **Wolf3D's split/mirrored gameplay was NOT a video bug.** `INC`/`DEC r16` never normalised their operand, corrupting `finetangent[]`. `test_alu_fuzz.py` grades it.
- **`boot_magic=0xbad00005` IS NOT A CODE TO DECODE — it is `BOOT_MAGIC_BSOD | BSOD_WATCHDOG`, and it means the device is ALREADY DEAD.** retro-go's `BSOD()` then spins in `while (buttons_get() == 0 …)` until a button is pressed, so every PC sampled after it is that loop (`buttons_get`, `HAL_GPIO_ReadPin`, `HAL_Delay`) and says nothing about the cause. `probe.py`'s stacked-frame decode is invalid there too (CLAUDE.md).
- **`cpu_state.pit_counter` (offset 2184) is stored ONCE PER GUEST INSTRUCTION and is readable live.** `probe.py --deref cpu_state_ptr+2184=PITCNT` answers "is the guest executing right now?" with no build, no flag and no RAM_EMU. It is what refuted "one slice never returns".
- **A pager counter read while no core is loaded is NOISE.** Check `core_ready`. **And KEEN4's wedge is NOT pagefile thrash.** Tested on SILICON with the donation refused and the pagefile provably untouched (`donated=0 frames_total=160 evictions=0 wb_file=0 wb_us_worst=0`) -- it still wedges. `x86_pg_exhausted=0` fits thrash AND an unrelated hang; only the writeback counters separate them. **And it is NOT `ega_shadow_rebuild()` either** -- measured on silicon with a counter and a DWT bracket, not sampled: one rebuild is 846,000 cycles (3.0 ms), a whole pre-wedge run does **10** of them, the burst coincident with the wedge is ~230 ms against a ~472 ms WWDG window, and fixing the guard (steady state 10 -> 2) leaves the burst and the wedge untouched. **The instrument replaced the conclusion it was built to test**: across the wedge the rebuild counter KEEPS ADVANCING while both frozen counters do not, so the guest is still executing and it is ONE SLICE THAT NEVER RETURNS, not a stopped frame loop. [`docs/13`](docs/13_title_status.md).
- **Mode Y's cost is NOT plane-storage placement.** The plane RMW cost the SAME in DTCM as in AXI (121.8 vs 120.8 cyc, hardware) — because the cost was the READ, now gone.
- **The Mode Y BLIT and PRESENTATION are 0.84% of wall** -- DMA2D's ceiling on this path is 0.84%. The blit was never the cost.
- **FIXED: the planar store read 4 bytes to write 1.** Nothing was computed from the cell's old contents. Store 286.9 -> **171.1 cyc**, WOLF3D **13.5 -> 16.3 fps (+21%)**.
- **Wolf3D's interpreter is NOT slower in gameplay, it runs a different MIX** (90% of 66 -> 125 cyc/insn): aperture STORES, not `IDIV`; `x86_code_reloc` **0.79%** of wall, `SEAM_CHECK` **0.04%**. Both §8-9 of [`docs/09`](docs/09_frame_pacing_and_clocks.md). > **An inherited number from another project is not a property of this hardware** —
> and neither is a bound that was never checked against the map.

## Where the design lives

[`docs/01`](docs/01_memory_architecture.md) fold table, address translation; [`docs/02`](docs/02_cpu_pipeline_optimization.md) Thumb-2 interpreter, dual-issue; [`docs/03`](docs/03_devices_and_hle.md) device model, DOS HLE; [`docs/04`](docs/04_retro_go_and_gwemu.md) firmware ABI, gwemu's limits; [`docs/06`](docs/06_gwemu_determinism.md) determinism; [`docs/07`](docs/07_opcode_coverage.md) opcode coverage; [`docs/08`](docs/08_vga_mode13h.md) mode 13h, Mode Y, EGA 0Dh, presentation; [`docs/09`](docs/09_frame_pacing_and_clocks.md) frame pacing and the three clocks; [`docs/10`](docs/10_granule_seam.md) the granule seam; [`docs/11`](docs/11_pit_and_interrupt_controller.md) PIT, 8259, and what reads them; [`docs/12`](docs/12_memory_tiers.md) pager tiers, arena, frame donation; [`docs/13`](docs/13_title_status.md) per-title state and settled verdicts; [`docs/14`](docs/14_protected_mode.md) protected mode, the descriptor walk
