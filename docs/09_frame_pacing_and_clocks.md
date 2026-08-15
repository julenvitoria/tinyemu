# Frame pacing, and the three clocks

How fast the guest runs is not a property of the interpreter. It is a property
of this file's subject. Read it before optimising anything, because the first
question — "are we CPU-bound at all?" — is answered here and the answer has
been "no" more often than "yes".

Code: `src/main.c`, the slice controller and the frame loop.

## 1. Throughput is `max_slice x fps`

One loop iteration is **one frame**. The loop calls `GW_LCD_SWAP`
unconditionally every iteration and the swap paces to the panel, so the
iteration period *is* the display period whenever we are keeping up.

Therefore guest throughput is `max_slice` instructions x ~60 iterations/s, and
anything that caps `max_slice` caps the machine regardless of how fast the core
executes.

| knob | role |
|---|---|
| `GUEST_EXEC_PCT` (85) | **the binding constraint.** Share of a display period the guest may execute in; the rest is blit + audio refill + input poll |
| `MAX_SLICE_CEIL` (200,000) | a backstop for a pathological guest, ~36 ms at 280 MHz. **Not** the throughput limit — the regulator settles far below it (measured 29-78 k) |
| `MAX_SLICE_FLOOR` (1,000) | |
| `panel_ms_min` | the display period the budget is a percentage OF |

The regulator works on **measured time**, not instruction count: it grows
`max_slice` by 1/8 when `guest_exec_ms` is under target and shrinks it when
over. That is what makes a large ceiling safe — a guest whose instructions turn
expensive shrinks its own slice.

`last_slice_halted` suppresses growth after a `HLT`, because idle is not
headroom.

## 2. Two measured traps in the reference period

**`panel_ms_min` is a MINIMUM WITH AN UPWARD LEAK, and must be floored.** It
latches the shortest iteration ever completed — which during boot, in menus, and
on any frame the guest spent halted is genuinely 6-9 ms — then leaks back up
`panel_ms_min++` every 512 frames (~8.5 s).

Measured consequences before it was floored at `PANEL_MS_MIN_SANE` (14 ms):

- The budget was 90% of **11 ms** instead of ~16.7 ms. The controller held
  `guest_exec_ms` exactly on its target and the machine sat **~56% busy with 0
  dropped frames** — working perfectly against a target 40% too small.
- The score crept upward for minutes as the leak raised the budget. 6→11 ms over
  42 s is five increments of 512 frames = 2,560 frames ≈ 43 s. That drift is
  what "it gets faster the longer it runs" was.
- `frame_overruns` counts iterations over **1.5x** this value, so at a latched
  7 ms an ordinary 16.7 ms frame was reported as an overrun: **1,733 "overruns"
  in 20 s while `guest_fps_achieved` sat at a steady 59**. `frame_hist`'s
  buckets are in the same units and were skewed identically.

**Deriving the period from `guest_fps_achieved` instead is a RUNAWAY.** Tried
and measured: budget too big → frames dropped → fps falls → the measured
"period" reads longer → budget grows again. fps went 59 → 43 with the derived
period pinned at its 20 ms clamp and `guest_exec_ms` at 20 ms against a 16.9 ms
frame. The clamp bounded it; it did not stop it. **The reference must not be
something the budget can influence.**

## 3. Overrunning the deadline costs a WHOLE frame

The swap paces to the panel, so a slice that finishes in under ~16.7 ms catches
the next vsync and a slice that takes 20 ms misses it and waits for the one
after — losing the frame *and* idling for the remainder. Work either fits in one
panel period or costs two.

This is why the optimum is just **inside** the deadline and why overshooting by
3 ms costs 16.7. It is also why `GUEST_EXEC_PCT` is a cliff-edge tuning knob
rather than a smooth one: 90% produced 4.2 dropped frames, 85% did not.

## 4. The three clocks, none of them a hardcoded 60

1. **Display** — the panel's own swap signal; the period is *learned*, so an
   overclock needs no constant changed.
2. **Guest wall clock** — the firmware's ms counter. 18.2065 Hz exactly, carry
   in fixed point, catch-up capped at `TICK_CATCHUP_MAX` (4) with the excess
   counted in `guest_ticks_dropped`. **This is DOS's time of day and must not
   accelerate because a game retimed the chip** — BDA `0x46C` keeps its own
   accumulator (`bda_acc`) even when the guest programs PIT channel 0 to 140 Hz.
3. **Guest CPU rate** — `guest_kips`, instructions per millisecond, paced
   against wall time. `0` is unthrottled (`KIPS_MAX`, the default); anything
   else runs at that speed *and no faster*, with the shortfall counted in
   `guest_insn_deficit`. A deficit climbing at ~37.5 M/s is a 40k rung asking
   for 40 M ips against ~2.8 M delivered — loud, not silent.

`guest_kips` is DOSBox's `cycles` unit exactly, which is why the CPU-profile
labels carry the number.

## 5. CPU profiles

A Retro-Go game-options entry (`cpu_profile_*` in `src/main.c`), persisted under
the key **`"Palette"`** — the firmware's `odroid_settings_app_int32_get/set`
match against a fixed whitelist and ignore anything else *silently*, and the
slot is per-app so the name is ours alone.

| rung | cycles | rung | cycles |
|---|---:|---|---:|
| Max | unthrottled | 386SX 16 | 2,500 |
| 8088 4.77 | 330 | 386DX 33 | 6,000 |
| 286 8 MHz | 800 | 486SX 25 | 9,000 |
| 286 12 MHz | 1,200 | 486DX2 66 | 24,000 |
| | | 486DX4 100 | 40,000 |

The 486 rungs are **stress targets, not period-correct emulation** — they are
above what the silicon delivers, so they never throttle and are
indistinguishable from Max in score. They earn their place by changing how the
frame is *shaped*, so a workload that keeps up at one rung and stutters at the
next names the subsystem that could not follow.

**A rung below the achievable rate is the only one that throttles.** Before the
pacing fixes the effective rate was ~3,000 cycles, which sits between the 2,500
and 6,000 rungs — that is why 386SX 16, 386DX 33 and Max all scored alike.

## 6. Reading it on a target

`scripts/probe.py --watch` these, and they answer different questions:

| counter | says |
|---|---|
| `guest_ips_achieved` | what we actually got. **The number to read in Max** |
| `guest_max_slice` | where the regulator settled |
| `guest_panel_ms_used` | the period the budget is 90%/85% of |
| `guest_exec_ms` | host ms spent RUNNING the guest (1 ms granularity — coarse) |
| `guest_slice_halted` / `guest_grow_blocked` | the guest is idling, and growth is being suppressed for it |
| `guest_insn_deficit` | a paced rung we cannot satisfy. 0 in Max by definition |
| `frame_overruns` / `frame_hist` / `frame_ms_worst` | the TAIL. A mean cannot show a stutter |
| `x86_hlt_log` | WHERE the guest halted — `(CS<<16)|IP`, 16 deep |

**`guest_slice_halted` alone cannot tell a benchmark's timing loop from a DOS
keyboard wait.** Reading it as the former when it was the latter produced a
"38% of frames idle on the tick" conclusion that does not hold for a running
benchmark, where it is 0. That is what `x86_hlt_log` exists for.

## 7. Measured state

Hardware, TOPBENCH (text mode, so it measures the interpreter and not the blit):

| | |
|---|---|
| stock 280 MHz | ~2.8 M insn/s |
| overclocked | **~3.8 M insn/s**, 59 fps, ~1.3 frame overruns/s |
| interpreter gain | **+12.9%** work across the decode round and opcode threading |

Wolf3D in Mode Y manages **~1.4 M ips** against the same core's ~3.0 M in text,
with PC samples landing in `x86_vga_planar_store`. The planar store path, not
the interpreter, is the limit for graphics titles.

## 8. What the interpreter's 47% of Mode Y wall time actually is

The frame budget in §7 left one unexamined block: with the planar store hook at
25.0% of wall, idle at 27.0% and the Mode Y blit at 0.84%, the remaining ~47%
was "the interpreter", running at ~95 cycles per guest instruction in gameplay
against ~64 in menus. Nobody knew why.

**Measured, hardware, oc level 0 (280 MHz), `timelines/wolf3d-hw.tl`, a 19-20 s
gameplay window gated on `x86_modey_presents` climbing (6 -> 269, i.e. 13.5
presents/s).** The control is `timelines/wolf3d-menu-hw.tl`, the same core and
clock with `x86_modey_presents` gated CONSTANT.

### 8a. It is a changed MIX, not a slower interpreter

`profile_opcodes.py --compare menu.json play.json`:

| | cyc/insn |
|---|---|
| menus | 66.2 |
| gameplay | 125.2 |
| **change** | **+58.95** |
| ...from a CHANGED OPCODE MIX | **+53.04 (90%)** |
| ...from a PER-OPCODE COST increase | +5.91 (10%) |

(Both figures are on the `X86_PROFILE=1` build, which adds ~22 cyc/dispatch;
corrected, gameplay is 103.2 cyc/insn.)

**The mix change is not `IDIV`.** The raycaster's divides are opcode `F7`
GRP3 Ev, 4.24% of gameplay cycles at a corrected mean of 55.2 — cheaper than
average. The entire mix change is two opcodes:

| op | | %A menus | %B play | mean B | mix contribution |
|---|---|---|---|---|---|
| `88` | MOV Eb,Gb | 0.00 | 13.30 | 360.2 | **+47.91 cyc/insn** |
| `AB` | STOSW (rep) | 0.00 | 0.11 | 12,880.7 | **+14.42 cyc/insn** |

Both are stores into the Mode Y aperture. Gameplay is slower because it does
**5.3 million aperture stores per 19 s**, not because the interpreter regressed.

### 8b. Where the 47% goes

Shares of WALL. The interior is the opcode profile's corrected cycles scaled so
the block sums to the 47.1% the frame budget leaves for it.

| | % of wall |
|---|---|
| **interpreter, excluding the store hook** | **47.1** |
| `88` MOV Eb,Gb (aperture byte store), non-hook | 10.30 |
| `AB` REP STOSW loop, non-hook | 3.26 |
| `89` MOV Ev,Gv (ordinary word stores) | 2.28 |
| `EE`/`EC` VGA register OUT/IN (map mask, etc.) | 1.49 |
| `26` ES: segment prefix (4.29 M of them) | 0.38 |
| every other opcode — the guest's own arithmetic and control | 29.36 |

Cross-cutting costs, measured separately and already contained in the rows
above rather than added to them:

| | % of wall | rate | mean |
|---|---|---|---|
| aperture WRITE-trap detour (inside the `88` row) | ~4.63 | 212 k/s | ~45 cyc |
| aperture READ-trap detour (inside "every other opcode") | ~4.5 | — | ~46 cyc |
| `x86_code_reloc` (`x86_vprof_reloc`) | **0.79** | 14.8 k/s | 149.7 cyc |
| `x86_seam_sync` (`x86_vprof_seam`) | **0.04** | 0.9 k/s | 109.7 cyc |

### 8c. The four hypotheses, and what killed each

- **Opcode mix.** TRUE, and it is the answer — but the expensive instructions
  are aperture STORES, not the raycaster's `IDIV`/`IMUL`. 90% of the
  menu-to-gameplay slowdown.
- **`x86_code_reloc`.** DEAD: **0.79% of wall.** The rate was also wrong — 14.8
  k/s in gameplay, not the ~88 k/s quoted, which was a startup figure.
- **Aperture READS through the fold trap.** REAL but small, ~4.5% of wall. It
  shows up as the ONLY per-opcode cost increase in the whole comparison:
  `8A` MOV Gb,Eb goes 49.0 -> 95.0 cyc and `8B` MOV Gv,Ev 45.5 -> 75.5. The
  +46 on the byte load matches the +45 write-trap premium on `88` — two
  independent measurements of the same out-of-line detour.
- **`SEAM_CHECK`.** DEAD: **0.04% of wall.** The predicate is two instructions
  and a not-taken branch and the sync it guards fires 909 times a second.

### 8d. The honest verdict

~37 of the 47 points is the interpreter doing ordinary work on the instruction
stream the guest handed it, at 55-100 cycles per instruction. The `88` slot's
corrected mean of 338 cyc decomposes as ~237 of planar store hook plus ~101 of
interpreter, and the hook figure agrees with `x86_vprof_store`'s independent
286.9 cyc once the ~50 cyc `cell`-bracket inflation is removed. **Two
instruments built on different principles agree on the same number**, which is
the reason to believe either.

**`AB` REP STOSW calls the store hook once per element.** Its 12,858 cyc mean is
~38 elements x (237 hook + ~100 interpreter) = ~12,800 — the arithmetic closes.
The destination advances linearly and the page cannot change mid-`REP`, so the
cell decision is resolvable once per `REP` rather than 38 times; that
amortisation is still on the table and is worth ~3% of wall on these numbers.

## 9. The planar store was reading four bytes to write one, and that was 12% of wall

§8 left `x86_vga_planar_store` at ~237 cyc for a single guest byte and asked what
a `PL_PLANE` store — the class documented as skipping the latch reload and the
`vga_cell()` lookup — could possibly be spending it on. The answer was in the
merge itself:

```c
memcpy(&q, p, 4);                                   /* <-- THE READ */
q = (q & ~m) | ((v & 0xFF) * 0x01010101u & m);
memcpy(p, &q, 4);
```

**The read is pure waste and always was.** Every plane the Map Mask enables
receives the same byte `v`; nothing is computed from the cell's old contents.
The merge existed only to PRESERVE the bytes the mask does not enable — and
writing just the enabled bytes preserves them too, without ever loading the
cell.

The read is not merely an extra instruction. The layout is pixel-major
(`vga_cell()` returns `base + 4*off`), a scaler writes a vertical column, so
successive stores are 80 plane offsets = **320 bytes apart and each one is its
own cache line in AXI SRAM**. The load must complete before the merged value
exists, so the core stalls on a line fill it never needed; the byte stores that
replace it post to the store buffer and the core walks away.

```c
if (m == 0xFFFFFFFFu) {                 /* all four: one aligned STR   */
    q = (v & 0xFF) * 0x01010101u; memcpy(p, &q, 4);
} else {                                /* k planes: k STRBs, no load  */
    if (mm & 1u) p[0] = v8;  if (mm & 2u) p[1] = v8;
    if (mm & 4u) p[2] = v8;  if (mm & 8u) p[3] = v8;
}
```

**Measured on hardware, same gated 19 s gameplay window, oc level 0:**

| | before | after | |
|---|---|---|---|
| `x86_vprof_store` mean | 286.9 cyc | **171.1 cyc** | −40% |
| `x86_vprof_cell` mean (differential use only) | 121.8 cyc | **33.4 cyc** | −73% |
| guest throughput | 1.383 M ips | **1.667 M ips** | **+20.5%** |
| `x86_modey_presents` | 13.47 /s | **16.3 /s** | **+21%** |
| share of wall removed | | **~12.0%** | |

An intermediate arm that took the no-load path only when the Map Mask selected
exactly ONE plane got the store to 233.5 cyc and +5% throughput — **35% of
`PL_PLANE` stores are single-plane**, so restricting the fix to that case
captured barely a third of it. Dropping the read for EVERY mask is what the
argument actually licenses, and it is what the numbers above measure.

**The oracle is `tests/test_video.py`'s Mode Y and EGA 0Dh arms**, which drive
all four write modes, set/reset, the ALU and partial bit masks through the same
64,000 pixels: both stay **64000/64000 exact**, and
`scripts/run_planar_controls.py` still catches all 6 negative controls. A fast
path claiming a combination it should not have shows up as wrong pixels.
