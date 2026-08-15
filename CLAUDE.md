# TinyEMU x86 (STM32H7B0VB / Retro-Go Port)

This project rewrites the TinyEMU x86 (32-bit) core into highly optimized ARM Thumb v2 assembly for the STM32H7B0VB MCU running the Retro-Go framework.

## Project Workflow & Navigation
This project relies on strict documentation organization to manage extreme architectural complexity. 
**ALWAYS consult these files to orient yourself:**

1. **`STATUS.md`**: Your immediate dashboard. Check this for a quick overview of current progress and our immediate sprint focus.
2. **`PLAN.md`**: The master roadmap. This is your cheatsheet to understand how the current task fits into the big picture, and points to the technical blueprints in `docs/`.
3. **`TOOLING.md`**: Details the scripts, the support modules, and the strict 3-tier testing hierarchy (gwemu bare-metal -> gwemu + Retro-Go -> Hardware).
4. **`docs/`**: The meat of the project. Contains the comprehensive, micro-managed technical designs for each subsystem.

*Note: `CLAUDE.md`, `STATUS.md`, and `PLAN.md` must strictly remain under 100 lines so they are never skipped over.*

## Where the cycles actually go — READ BEFORE OPTIMISING ANYTHING

Measured on HARDWARE, Wolf3D in Mode Y, as % of wall. Full table and method:
[`STATUS.md`](STATUS.md) and [`docs/09`](docs/09_frame_pacing_and_clocks.md) §8.

- **29.4%** ordinary opcodes — the guest's own work, **~57 cyc/insn**. The
  largest single block, and the one nobody has attacked.
- **25.0%** the planar store hook. An aperture byte store is **171 cyc**
  against **55** for the same store to ordinary memory.
- **27.0%** idle waiting for the panel — vsync-quantised, not freely convertible.
- **0.84%** the blit. **0.05%** residual.

**Already measured and REJECTED — do not re-propose:** DMA2D for the blit
(0.84% ceiling); `planar_store` in assembly (AAPCS ≤2%); planes in DTCM vs AXI
(121.8 vs 120.8 cyc, identical); `x86_code_reloc` (0.79%); `SEAM_CHECK`
(0.04%); `IDIV`/`IMUL` (below average cost); disk reads (freeze at t+36 s).
Each was plausible and each measured to nothing.

**A number without a tier is not a measurement.** gwemu does not model timing;
cycle claims are tier 3 only.

## Core Conceptual Constraints (Read Carefully)
Working on this project requires a fundamental mindset shift:
- **Silicon**: We must play to the STM32H7B0VB's strengths and utilize its capabilities as efficiently as possible.
- **Memory**: We must emulate 8MB of guest RAM using only ~840KB of AXI SRAM via a NOR Flash XIP + COW paging strategy. 
- **Pipeline**: Cortex-M7 is superscalar dual-issue. Assembly must be written to avoid data dependencies and pipeline stalls.
- **Zero-Wait-State**: Hot execution loops MUST be pinned to ITCM (64KB). Critical state and page tables MUST be pinned to DTCM (128KB).
- **FatFs HLE**: Do not emulate raw block devices. We bypass the guest OS block layer and translate DOS file I/O directly to FatFs calls.
- **Scripting & Target Interaction**: All scripts MUST be written in Python. **NO `.gdb` scripts.** Any script communicating with a target must use `gnwmanager`'s interchangeable backends (`GDBBackend` for `gwemu`, `OpenOCDBackend` for real hardware). This enables powerful, reusable Python scripts that can multiplex tasks (e.g., polling the retro-go log buffer while simultaneously reading/writing memory or inspecting PC values) seamlessly across both emulation and silicon.

## Reserved Register Contract
*The R12 incident — 136 handler lines using the dispatch table base as scratch —
happened because this contract lived only in a comment. It lives here now.*

| Reg | Holds |
|---|---|
| `R2` | **Active MODE RECORD** from dispatch until the ModRM call. Not reserved after that. It carries the dispatch table, the 0F table at `[R2,#-8]` and the address-size ModRM decoder at `[R2,#-4]`; `66`/`67` are one `EOR` on it. A handler must not clobber R2 before `BLX` on the decoder — use R3 for the mod test. |
| `R6` | **Active segment descriptor POINTER** (not a base value). `R8` is the "no override" sentinel; reset at `.fetch_decode_loop`, set by an override prefix, which re-enters at `.fetch_next_byte_no_reset` so it survives its own instruction and no further. |
| `R8` | CPU context (DTCM) |
| `R9` | Fold table (DTCM) |
| `R10` | Linear EIP |
| `R11` | Flag state |

Scratch is `R0`–`R5` and `R12`. `R0` is the opcode and `R1` the ModRM byte
through a handler; `R7` is the effective address out of `DECODE_MODRM`.
Handlers are emitted twice, once per operand size, and differ only through the
width macros (`LD_V`/`ST_V`/`LDREG_V`/`STREG_V`/`LDS_V`/`EIP_ADV_V`/`SP_*`/
`NSH`/`OPSZ`). Write against those, never a bare `LDR`/`STR`, or the handler is
silently 32-bit in both passes -- which is exactly what the dead
`MEM_READ_SZ`/`STACK_INC` scheme left behind. A handler that must be per-pass
is declared with `OP`, not `OP_AGNOSTIC`, and needs a `.set` alias in both
blocks of `cpu_exec_handlers_0f.S` or the shared tables will not resolve.
**`DECODE_MODRM` clobbers `R2`–`R5` and returns the linear address in `R7`** —
anything live across that call must be recomputed after it or spilled. Reading a
source register *before* the call to fill the walk's shadow is a real bug that
`MOV Ev,Gv` shipped with; `R1` survives, so recompute from it instead.

## Project Concepts
*If a conceptual misunderstanding happens repeatedly during a session, or if a third-party tool has extremely strict caveats, it MUST be briefly referenced here and thoroughly documented in `docs/` to prevent future errors.*
- **Never write scratch to `/tmp`, and always clean up.** `/tmp` here is RAM-backed; a gwemu run copies the SD image and extflash (~66 MB each) and leaking a few dozen fills it. Run scratch goes under `build/run/` in the project, is deleted in a `finally` (plus an `atexit` guard), and is kept only with an explicit `--keep-temp`. Any new code that creates temporary files follows the same pattern.
- **Never use the scratchpad for work the project will repeat.** Target inspection, measurement and build steps always recur — they belong in `scripts/` (user-facing CLIs) and `scripts/common/` (importable modules). Extend what exists rather than adding a near-duplicate; redundant scripts are how four overlapping `*_runner` modules happened. **Any change to that codebase requires updating [`TOOLING.md`](TOOLING.md)** — keep it to what each script does and how to run it, with rationale in the module docstring.
- **Timelines are capped at 60s TOTAL, and 30s is usually plenty.** A gwemu input timeline exists to open a preselected ROM, not to wait for something to happen. If a run needs a longer window to show a result, that is a bug to find with `scripts/probe.py`, not a wait to sit through — sampling PC and watched globals locates it in minutes, a long timeline just burns them.
- **Never skip test tiers.** New tooling's first execution must NOT be against silicon — a failure then has two possible causes (your tool, or the target) and you cannot separate them. Run under gwemu first, always. `HardwareTarget` also flashes internal flash, overwriting what is there. See [`TOOLING.md`](TOOLING.md).
- **Green under gwemu ≠ hardware ready.** `gwemu` does not model peripheral *initialisation*: extflash at `0x90000000` is plain readable memory there, but on silicon it needs OCTOSPI memory-mapped setup. This is broader than `docs/04`'s timing and SD-busy caveats — anything depending on a peripheral being set up is unproven under gwemu. Bare-metal firmware must have real fault handlers, or every hardware failure looks like a hang.
- **Firmware callbacks are not all NULL-guarded, and the CLUT count is a contract.** Retro-go's `repaint_overlay()` calls our repaint callback unconditionally, so `NULL` there is a branch to address 0 the moment PAUSE/SET opens the menu; and `lcd_set_clut(clut, count)` puts each entry's darkened twin at `count+i` while the overlay dims pixels by ORing `LCD_DARKEN_BIT` (32) — the two only agree at `count == 32`. After a crash retro-go BSODs and prints PC/LR on the panel; `probe.py`'s stacked-frame decode is invalid there. See [`docs/04`](docs/04_retro_go_and_gwemu.md) §4b.
- **The granule seam: an access that spans a 4 KB boundary is NOT one access.** `host = lin + k` with one `k` per granule, and the pager hands frames out in FAULT order, so guest page N+1 is almost never the host memory after page N. A 16/32-bit access spanning the boundary resolves one pointer from the LOW granule and runs off the top of that frame, silently. Every pager frame therefore carries 8 bytes of slack (stride 4,104) that `x86_seam.c` pulls the neighbour into before the access and pushes back at the instruction boundary; the trigger is `SEAM_CHECK` on every MULTI-BYTE fold site in `cpu_exec*.S`. **A new fold site that loads or stores more than one byte needs `SEAM_CHECK`, or it is silently wrong at one address in 2,048.** `x86_seam_overflow` is the one number that says a straddle was refused. See `x86_seam.c` and `tests/test_seam.py`.
- **The INSTRUCTION STREAM has the same seam, and it is threaded, not checked.** The dispatch folds the opcode byte and leaves that granule's `k` in `cpu->code_k`; every later fetch of the same instruction — ModRM, SIB, displacement, immediate — is then `CODE_PTR`, two instructions, with no `SEAM_CHECK` and no fold trap. That is only sound inside one granule, so the dispatch PROVES the opcode is at granule offset ≤ 0xFF0 (16 bytes, the longest x86 instruction) before threading anything, and calls `x86_code_reloc()` to stage the instruction into DTCM when it is not. **An in-stream fetch must use `CODE_PTR`, never a hand-rolled `[R9, …]` walk** — a hand-rolled one is merely slower, but a `CODE_PTR` at a site that is NOT within the dispatched instruction is wrong. `x86_code_relocs` counts the staged instructions; `tests/test_seam.py`'s code arm and `--code-negative` are the proof.
- **Retro-Go & gwemu Gotchas**: Do not trust `gwemu` for timing, beware of stale SD card images during build, and never run concurrent builds in a shared folder. See [`docs/04_retro_go_and_gwemu.md`](docs/04_retro_go_and_gwemu.md) for the exact rules to avoid wasting hours.
