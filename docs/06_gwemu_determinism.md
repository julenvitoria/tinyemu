# gwemu Determinism — Findings and Handoff

Measured 2026-08-12 on `test_core.elf`, benchmark = 1,000,001 x86 instructions
(register-only `DEC EAX` / `JNZ` loop, no memory operands, no devices).
Reproduce with `python3 -m scripts.common.asm_metrics --cycles --target <t>`.

## Headline

**gwemu was non-deterministic because the runner never passed `-icount`.**
Adding it makes runs bit-identical. This is already implemented:
`scripts/common/qemu_runner.py` accepts `icount=`, exposed as `--icount` on every
harness via `add_runner_args`.

## Data

### Before — no `-icount` (virtual time follows host wall-clock)

| run | DWT cycles | cyc/insn |
|---|---|---|
| 1 | 7,137,363 | 7.14 |
| 2 | 6,532,397 | 6.53 |
| 3 | 6,467,742 | 6.47 |
| 4 | 7,395,714 | 7.40 |

Spread **927,972 cycles = 14.3%**. Instruction count was exact every run
(1,000,001), which is why the project rule is "counters, not cycles".

### After — `-icount shift=N,align=off,sleep=off`

| shift | DWT cycles | cyc/insn | runs | spread |
|---|---|---|---|---|
| 0 | 11,340,012 | 11.34 | 3 | **0** |
| 1 | 22,680,024 | 22.68 | 1 | — |
| 2 | 45,360,048 | 45.36 | 1 | — |
| 3 | 90,720,096 | 90.72 | 1 | — |
| 4 | 181,440,192 | 181.44 | 1 | — |

Exactly 2× per shift, as expected (`shift=N` → 2^N ns of virtual time per
instruction). Three consecutive `shift=0` runs returned **11,340,012 every time**.

### Hardware reference (STM32H7B0VB, 280 MHz)

| run | DWT cycles | cyc/insn |
|---|---|---|
| 1 | 57,500,191 | 57.500 |
| 2 | 57,500,184 | 57.500 |
| 3 | 57,500,183 | 57.500 |
| 4 | 57,500,186 | 57.500 |

Spread **8 cycles in 57.5M (~1.4e-7)**. Silicon resolves sub-0.001% changes.

### Calibration

Hardware 57.50 cyc/insn sits between `shift=2` (45.36) and `shift=3` (90.72).
No integer shift matches: shift=2 under-reports 21%, shift=3 over-reports 58%.
Interpolated ideal shift ≈ 2.34.

## What this does and does not buy

**Does:** deterministic, repeatable runs. Under `-icount` every instruction costs
the same fixed virtual time, so the DWT count becomes a **dynamic executed-work
counter** scaled by a constant. Ratios between two builds are therefore
meaningful and exactly reproducible — which is strictly better than the static
counts `asm_metrics` collects today, because it weights by execution frequency.

**Does not:** model the pipeline. Uniform cost per instruction means dual-issue
pairing, load-use stalls and branch mispredicts are all invisible by
construction. `-icount` cannot make gwemu a cycle-accurate M7. **Anything
scheduling-shaped still requires silicon.** Note that the previously documented
"~400x off" figure is wrong for CPU work — measured ratio is ~5x at shift=0 and
~8.5x without icount; the 400x presumably came from a full-system run where the
blit/SD ratio inversion compounds.

## Suggested next steps for whoever picks this up

1. **Decide the default.** Consider making `--icount 2` the default for gwemu
   runs so results are reproducible by default, with `--icount none` to opt out.
   Weigh against wall-clock cost: icount serialises the emulator, so runs get
   slower.
2. **Measure the slowdown** icount imposes on the full opcode suite (105 cases);
   if it is small, defaulting it on is free.
3. **Add a dynamic-count mode to `asm_metrics`** — an `--icount` run before and
   after a change gives an exact, execution-weighted work delta. Pair it with the
   existing static per-handler baseline.
4. **Do not chase absolute calibration.** A fractional shift is not available and
   matching hardware's 57.50 is not the point; determinism is. Use silicon for
   absolute numbers.
5. **Check whether icount changes SD/OCTOSPI behaviour** — `docs/04` notes gwemu
   does not model SD busy state at all; icount does not change that.

## Reproducing

```
# non-deterministic (current default)
python3 -m scripts.common.asm_metrics --cycles --target qemu

# deterministic
python3 -m scripts.common.asm_metrics --cycles --target qemu --icount 0

# hardware reference (flashes internal flash bank 1, overwriting retro-go)
python3 -m scripts.common.asm_metrics --cycles --target hardware
```
