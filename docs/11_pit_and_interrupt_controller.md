# The PIT and the 8259, and what reads them

Code: `x86_io.c` (ports `0x20`/`0x21`/`0x40`-`0x43`/`0x61`), the tick derivation
in `src/main.c`.

Two separate lessons live here, and both were "a register nobody reads is still
a register somebody reads":

- a **write-only** PIT made every guest that times itself compute nonsense;
- an **unimplemented** 8259 read port made a timing library declare overflow on
  every measurement it ever took.

## 1. Channel 0 drives the guest's tick, and the guest may retime it

Wolfenstein 3-D issues `OUT 0x43,0x36` (channel 0, lo/hi, mode 3) then two
writes to `0x40`, divisor `0x214B` = 8523 = **140.0 Hz**. Both writes used to be
dropped — `pit_write_control()` returned early for every channel but 2, and port
`0x40` was **not in the write switch at all** — so the guest asked for 140 Hz and
got the 18.2065 Hz boot rate.

Its tic counter paces turning, animation **and** menu key-repeat, so it ran at
**18.2/140 = 13% of real time**. That one cause presented as two symptoms
("rotation is slow" and "menus are sluggish") and neither was an input defect:
input was measured clean, with no per-frame delivery ceiling.

Two things must move with a programmable tick:

- **BDA `0x46C` keeps its own 18.2065 Hz accumulator.** It is DOS's time of day
  and must not accelerate because a game retimed the chip for itself.
- **One IRQ 0 per frame is a ceiling.** Invisible at 18.2 Hz against a 60 Hz
  panel, fatal at 140 Hz. The frame budget is cut into up to `TICK_IRQ_MAX` (8)
  sub-slices with one interrupt armed before each, because `hard_int_pending` is
  one deep. A `HLT` landing with a tick owed gets it immediately.

Cost: none measurable. A guest that never programs the timer is bit-for-bit on
the old path (Alley Cat: 18.14 Hz, `x86_pit0_writes` 0).

## 2. Counters must be READABLE and COUNTING

`IN 0x42` used to return `pit2_reload & 0xFF` — a static value with no high byte
— and `IN 0x40` was absent entirely, floating open bus. Alley Cat read `0x40`
**6,135,898 times in 28 s** getting one value; it is now 49,018 reads over 32
distinct values, because it was spinning on a dead clock.

What is modelled now, channels 0 and 2:

- a free-running 1.193182 MHz tick clock, where **guest time is instructions
  converted at `guest_kips`** — the emulated CPU's declared speed, published as
  `x86_pit_kips`. Wall-clock time was rejected deliberately: a precision timer
  sentinels past 54.9 ms, so a wall-clock counter would report the sentinel again
  and honestly measure only how slow the emulator is.
- counter-latch command (`OUT 0x43, xx00b`), lo/hi read sequencing per access
  mode, counting mode from the control word, mode 0 terminal count.
- channel 2 gated off `0x61` bit 0 (rising edge restarts, falling edge freezes),
  with `0x61` bit 5 derived from the real count.
- **the count range is `R-1..0`, not `R..1`.** At `R = 65536` the just-loaded
  value is `0x0000` and a guest sees the counter jump *up* on its next read.
- reset defaults model a **booted** machine (ch0 mode 3, free-running). Zeroed,
  they are mode 0, which reaches terminal count once and reads 0 forever.

Every host store to `pit_counter` goes through `x86_pit_counter_forced()` so
arming an IRQ is not charged as elapsed guest time.

## 3. Port `0x20` is a READ port, and that was the TOPBENCH bug

TOPBENCH reported **`65535 µsec` for MemTest, MemEA, Opcodes, VidMem, 3DGames
and Total** — every suite, always. The cause was not the PIT.

It times with Abrash's precision Zen Timer (`references/TOPBENCH/ZTIMER11/`,
used from `_MBLOCK.INC`, `_EA.INC`, `_OPCODES.INC`, `_VIDMEM.INC`,
`_3DGAMES.INC`). `PZTimerOff` tests for overflow by asking the **8259**, not the
counter:

```
OUT 20h, 0Ah     ; OCW3 = read IRR
IN  AL, 20h
AND AL, 1        ; -> OverflowFlag
```

On overflow `PZTimerCount()` returns `0xFFFFFFFF`, which TOPBENCH stores into a
`word` field → **65535**, and Total is their sum.

**Port `0x20` was not in our read switch.** It floated `0xFF`, bit 0 set,
overflow declared on every measurement ever taken. One absent port, six numbers.

Now: `0x20` answers IRR/ISR per OCW3, and `0x21` stores and returns the IMR (not
enforced). Measured mid-benchmark: `0x20 IN` ×1,640, **all 0** — no overflow
claimed on any measurement, where previously all 1,640 read `0xFF`.

**The honest claim is "no longer the sentinel", not "correct µsec"** — `CLAUDE.md`
says gwemu must not be trusted for timing, so real microseconds need hardware.
All six values come from that one path, so they change together; any that still
read 65535 are a genuine >54.9 ms overflow, not the port bug.

## 4. Testing

`tests/test_pit.py` runs the real guest sequences as **guest code**, including
the Zen Timer's own latch-and-read. Two negative controls, both proven to fail:

| control | reproduces |
|---|---|
| `--negative` | the `0x40` WRITE arm deleted — divisor 65536 / 18.207 Hz where the guest asked 8523 / 140 Hz |
| `--negative-clock` | the `0x40`/`0x42`/`0x20` READ arms deleted — ch2 reads `0/0/0`, elapsed `1`, **IRR `0xFF`**. The 65535 mechanism itself |
