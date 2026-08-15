# Plan

**Goal**: rewrite TinyEMU's 32-bit x86 core as ARM Thumb-2 assembly for the
STM32H7B0VB running Retro-Go.

State: [`STATUS.md`](STATUS.md). Running: [`TOOLING.md`](TOOLING.md). Design:
[`docs/`](docs/). **Roadmap and backlog — a list, not a narrative.** Rationale
and measured history belong in `docs/`, not here.

## Phases

1. **Memory** ([`docs/01`](docs/01_memory_architecture.md),
   [`docs/10`](docs/10_granule_seam.md),
   [`docs/12`](docs/12_memory_tiers.md)) — DONE: checked translation
   (`host = lin + k`, `FOLD_INVALID` costs one `ADDS`+`BMI`), the granule seam,
   eviction, the SD pagefile, frame donation. **Dead, measured**: NOR backing.
   Eviction *policy* is not the lever — LRU caps at 2.1x over FIFO while the
   pool-size cliff is 350x.
2. **Interpreter** ([`docs/02`](docs/02_cpu_pipeline_optimization.md)) — hot
   loop in ITCM, `R8`-`R11` reserved (`CLAUDE.md`, binding). `LEA` is the
   decode floor to watch.
3. **Devices and HLE** ([`docs/03`](docs/03_devices_and_hle.md)) — DOS file I/O
   straight to FatFs. 13h, Mode Y and EGA 0Dh are pixel-exact
   ([`docs/08`](docs/08_vga_mode13h.md)); timing in
   [`docs/11`](docs/11_pit_and_interrupt_controller.md).

## Validation hierarchy

Strictly in this order — `TOOLING.md`, and `CLAUDE.md` on never skipping a tier.
**1.** gwemu bare-metal (Thumb-2 logic, ITCM/DTCM/AXI placement) · **2.** gwemu
+ Retro-Go (ABI, DTCM, SD, video, input, audio) · **3.** hardware — the only
tier that sees the pipeline, peripheral init or real memory ordering.

## Backlog

### Queue A — make the shipped titles good

Everything that is not Dark Forces. **Done this session:**

1. ~~**Per-game `.cfg`**~~ — dosbox.conf syntax, prior port's two-layer split kept
   (`x86_settings.{c,h}` owns format+policy with NO I/O, so it is gradeable at
   tier 1; `x86_cfg.{c,h}` parses; `src/main.c` binds). All Options plus
   `[autoexec]` launch parameters, per title. **Trap honoured**: `MAX_OPEN_FILES`
   is 8 firmware-wide and exhaustion is SILENT — open, read, close, never hold.
2. ~~**Guest RAM -> NOR watchdog**~~ — the flash was innocent (the firmware kicks
   the dog in `OSPI_Program`/`Erase` and the SD busy-waits). Both un-kicked
   stretches were ours: the snapshot is a menu callback outside the frame loop,
   and the slice regulator moves by 1/8 per frame while an attach changes the
   price of an instruction in ONE step. `nor_snap_wdog_gap_ms` is the witness.
   **Currently unreachable anyway**: RAM_EMU is short of a 160-frame pool, so
   `guest_ram_pages != 160` refuses the snapshot (`nor_snap_result=2`).
3. ~~**Sound**~~ — SB16 (8237 DMA, IRQ 5 via the 8259, DSP 4.05, CT1745 mixer,
   `BLASTER`) **and** OPL2 FM via **emu8950**, vendored from the owner's gnw-doom
   with its tuned `-D` set and its 16.16 linear resample (zero-order hold buzzes
   at ~1.7 kHz — measured there, not re-derived). **Prince of Persia plays
   digitised sound; Wolf3D plays BOTH** (digitised once enabled in the title's
   own setup; music `keyons=48`, `audio_peak_max` 337). Cost: **6 pager frames**
   and ~5.9% of wall for Wolf3D's actual music, 10.4-13.8% worst case.
   `X86_OPL_FM=0` returns every byte.
4. **Epic Pinball** — past its EXEC wall, reaches mode 13h, programs the mixer
   and 22,050 Hz. Now stops on `0F A0` PUSH FS, still real mode. **The closing
   task of Queue A.**

**Open, measured, sized:**

- **Keen 4 has no music, and it is NOT the synthesiser.** `opl_writes`=264 (the
  detect plus id's 245-register zero pass), `keyons`=**0** — its driver never
  writes a note. Config is right (`CONFIG.CK4`: `SoundMode=2`, `MusicMode=1`),
  detection works (`opl_timer_expiry=1`), and Wolf3D drives the identical path
  and sounds. **Lead: `INT 08h` reaches our stub ~16/s — the 18.2 Hz chained
  BIOS rate, not the 140 Hz music tick Keen should program.** Grade the ISR
  rate, not FM.
- **Keen's intro is clipped** — the card is the arena's best CONTIGUOUS run, and
  a claim cannot span regions, so growing it means growing the arena, not
  borrowing frames. 200 rows at `crtc_start=15360`/pitch 64 needs 28,160
  offsets; the pan drives the start higher still (`x86_vga_planar_lost`).
- **Wolf3D's weapon sprite** is corrupt — deterministic, byte-identical across
  captures, so gradeable.
- **The game port**: Wolf3D issues **2.35 M `IN 0x201`** reads, unmodelled and
  larger than everything sound-related combined.
- `REP STOSW`'s per-element planar hook (~3% of wall); aperture reads (~4.5%).
- Retro-go's Pause overlay is unreadable over 256-colour modes.

### Queue B — Dark Forces. NOT DONE UNTIL DARK FORCES RUNS.

**Nothing here starts before the step above it is measured.** Every blocker so
far has been a missing DOS service or a memory gap — never the thing that was
predicted.

1. ~~**XMS**~~ — a COPY interface, never folded, costs the interpreter nothing.
2. ~~**EMS**~~ — the opposite shape: MAPPED, so pinned on map and released on
   unmap. Both advertise **0 with no pagefile**, by design.
3. ~~**`INT 21h AH=4Bh` EXEC**~~ — `AL=00h` and `AL=03h` (overlay). Default ON;
   the "honouring it kills Wolf3D" finding did not survive re-testing (Wolf3D
   never calls it — that was the RAM_EMU starvation trap wearing a disguise).
4. ~~**`AH=52h` list-of-lists / `AH=29h` parse filename**~~ — unpopulated chains
   are `0xFFFF:0xFFFF`, DOS's own end-of-chain marker, so a guest that WALKS one
   stops and one that INDEXES one faults visibly, rather than computing from
   plausible garbage.
5. ~~**The HMA**~~ — backed with real frames when A20 is open, alias when masked,
   and the pager may not write those entries even to unmap. This unblocked
   **two** titles.
6. ~~**Protected mode: the descriptor walk**~~ — and it is now EXERCISED. Dark
   Forces and Mortal Kombat both reach `CR0=1` with `CS=0x0070`, a real GDT
   selector (base `0xA900`, not `sel<<4`). **They get there by `LMSW`, not
   `MOV CR0`** — an extender must also run on a 286, so `0F 22` is never issued,
   which is why this took so long to exercise. Real mode pays NOTHING, proven by
   disassembly diff.
7. **NEXT: protected-mode IDT gate dispatch.** Both titles now stop on
   `INT imm8` with `CR0.PE` set — `L(HANDLE_INTERRUPT)` exits. **Measured demand
   from two titles**, which is the first time this item has had any.
8. Then: far transfers and call gates, `IRET` across privilege, stack switches,
   paging, and per-access segment limit enforcement (the predicate is written
   and tested; making it free needs a protected-mode mode-record axis — the same
   trick `66`/`67` already uses).

### Queued — SylverB's FatFs DMA + SPI perf work

`references/FatFs-DMA` (branch `DMA`, `6d6f6fb`). **BLOCKED on upstream** — the
driver file is in his FatFs repo but the retro-go-sd plumbing (DMA stream setup,
a transfer buffer in D2/AHB SRAM, cache maintenance) is not. Do not start half
of it. The read-side commit (`fd1ff62`, CMD18 multi-block) is **already in the
firmware we flash** — baseline, not a pending win — and it would not have
helped: both commits are READS and the pagefile's cost is a WRITE.

### Not scheduled, but live

- **VBE 320x240** — the only mode that fills the panel and reduces the blit to
  one `memcpy`. Custom mode number, so little period software asks for it.
- An **adapter-advertisement Options entry** (CGA / EGA+VGA / VGA-only).
- **`x86_vga_planar_store` in assembly** — gated on re-profiling first; the case
  rested on pre-optimisation data. **If the path is a single-digit share of
  frame time, the answer is no.**
