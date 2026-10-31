# Tooling & Testing Hierarchy

Three tiers, each isolating a different class of failure. **Never skip a tier** —
new tooling pointed straight at silicon turns one unknown into two.

| Tier | What it proves | Boots |
|---|---|---|
| 1. gwemu, bare-metal | CPU core in isolation | `test_core.elf` |
| 2. gwemu + retro-go | Core inside the real firmware | retro-go + `dos.bin` |
| 3. Real hardware | Timing, pipeline, SD behaviour | same image as tier 2 |

gwemu is the basis throughout; plain QEMU adds nothing it does not already do.

Guest titles are **`.dsk` floppy images** (FAT12), not bare executables.

## gwemu version

Tier 2 needs a gwemu that models **TIME on PC5** (and the charger PGOOD on
PA2): the 2026-10-10 tree or later. gwemu `a35a9b9acc` (2026-08-19) held PC5
low, so retro-go saw TIME pressed from reset, skipped auto-launch and ate every
key -- no game launched, and nothing pointed at gwemu. `GwemuTarget.start()`
reads `GPIOC_IDR` while gwemu is still halted and raises if PC5 reads low
(`GWEMU_ALLOW_PC5_LOW=1` turns that into a warning). The scripts run whatever
`gwemu` is first on `PATH`.

## Scripts (`scripts/`)

Run these directly. All take `--help`.

### `build_retrogo.py` — build the retro-go firmware

```bash
python3 scripts/build_retrogo.py                      # defaults
python3 scripts/build_retrogo.py --dry-run            # print the make command
python3 scripts/build_retrogo.py --clean              # required if flags change
python3 scripts/build_retrogo.py --extflash-offset-mb 4
python3 scripts/build_retrogo.py --bank 2             # productive dual-boot
```

Defaults to retro-go's release parameters with `INTFLASH_BANK=1` (boots
directly) and a 64 MB part. Give `--extflash-part-mb` and `--extflash-offset-mb`;
the size is derived. Builds `gwemu_release`, so emulator and device media match.

### `budget_gate.py` — the one command that fails when a budget regressed

```bash
python3 scripts/budget_gate.py                 # rebuild, then gate
python3 scripts/budget_gate.py --clean         # after a build-FLAG change
python3 scripts/budget_gate.py --no-build      # gate an existing ELF
python3 scripts/budget_gate.py --asm           # + the advisory baseline report
python3 scripts/budget_gate.py --update        # record today's numbers
```

Exits non-zero when any of these regressed. **Run it before you merge a core
change**, and read what it prints even when it passes.

| gated | against |
|---|---|
| `__CORE_ITCM_CODE_END__` | `itcm_code_end_max` in `tests/budgets.json` |
| `__CORE_BSS_END__` | `core_bss_end_max` — RAM_EMU overshoot is SILENT (`guest_ram` NULL, `core_ready` 0, a black frame loop). Three instrumented builds have died of it |
| pager frames implied by `__CORE_BSS_END__` | `frames_implied_min` |
| `docs/07`'s generated block | `gen_opcode_coverage.py --check`, so the table cannot rot again |
| `docs/07`/`STATUS`/`PLAN`/`CLAUDE` PROSE | same `--check`: a hand-written "`8D` is unimplemented" next to a generated "reached" now FAILS |

It also prints ITCM bytes free of 65,536, RAM_EMU headroom, and how many
**4,104**-byte frames that headroom implies (stride is 4,104, not 4,096 — the
granule-seam slack). **That frame count is an UPPER BOUND**: other RAM_EMU
claims land before the guest pool, so the live `guest_ram_pages` is lower. It
is a necessary condition, not the pool size; the pool size is a tier-2
measurement of `guest_ram_pages` / `guest_ram_pages_short`.

**It forces a real re-link.** `make: Nothing to be done` gave stale symbols
once and they were briefly reported as real, so the gate deletes the ELF before
building — which no make shortcut can satisfy. With `--no-build` it instead
compares the ELF's mtime against every tracked source and **refuses** (exit 2)
rather than reporting numbers it cannot stand behind; `--allow-stale` overrides
and says so on every line of output.

Thresholds live in `tests/budgets.json` so a deliberate increase is a
**reviewable diff**, not silent drift. Raise them with `--update` and say in the
commit message what grew and why. Build flags are not dependencies here, so the
recorded numbers are for a STOCK build; `--make VAR=VAL` marks the comparison
advisory.

### `asm_metrics.py` — per-handler assembly metrics

`tests/asm_baseline.json` is **ADVISORY**, not a gate. It is a review aid: a
handler gaining instructions should be a visible event. But it drifts with
every legitimate merge, and a lane refreshing it inside its own change would
fold pre-existing drift under its own diff and hide its additions. So it is
refreshed as a **standalone commit** that names what drifted, and
`budget_gate.py --asm` only reports it — the report never affects exit status.
Re-baseline (`--update`) alone, never as part of a feature commit.

```bash
python3 scripts/asm_metrics.py                        # compare to baseline
python3 scripts/asm_metrics.py --update               # re-baseline (commit it)
python3 scripts/asm_metrics.py --top 20
python3 scripts/asm_metrics.py --cycles --target hardware
```

### `disasm_diff.py` — "did this change cost the hot path anything?"

```bash
# on the base commit
python3 scripts/disasm_diff.py --dump build/run/before.json
# after the change
python3 scripts/disasm_diff.py --dump build/run/after.json
python3 scripts/disasm_diff.py --compare build/run/before.json build/run/after.json --hot
```

Per-symbol normalised disassembly, and the diff between two of them. `--hot`
restricts the report to the symbols the "real mode pays nothing" claim covers:
`.fetch_decode_loop`, every `.op_*` handler, the ModRM decoders and the dispatch
tables. `--elf` selects the ELF (default `build/core/dos_core.elf`), `--quiet`
names changed symbols without showing the first difference.

**This exists because `tests/test_benchmark.py` cannot decide the question and
must not be quoted for it** — under gwemu it spans 10.30–11.88 M cycles on the
BASELINE alone, a 15% spread that swallows anything a cold-path change could
cost. What CAN be decided exactly is whether the hot path's instructions changed
at all, which is the claim [`docs/14`](docs/14_protected_mode.md) §2 makes.

Three normalisations, each of which was a false positive before it was added:

* **branch targets → symbol names.** Adding cold code moves every later symbol,
  so a raw `objdump` diff is nothing but address noise.
* **jump-table entries → the symbol they point at.** The dispatch tables
  disassemble as `.word` literals and every one of them moves when any handler
  moves; resolved, a table that still dispatches every opcode to the same
  handler compares EQUAL, and one that re-points an entry is still caught.
* **trailing NOPs dropped.** `objdump` gives a symbol everything up to the next
  one, so alignment padding counts as code and a handler nobody touched reports
  "8 -> 7 instructions".

Symbols that differ ONLY in a moved address literal are reported as
`RELOCATED ONLY` rather than `CHANGED`, so "the handler gained an instruction"
stays a distinct, reviewable event from "a literal it loads moved". Exit status
is 1 when anything in scope changed.

### `probe.py` — see what a running target is doing

`--watch` reads globals. `--deref SYM+OFF` follows a pointer into a struct.
`--dump SYM:N[xG]` reads a symbol's OWN storage as N words grouped G per line,
which is what an array needs — `--deref` would treat the first word as a
pointer. A literal `0xADDR` works in place of a symbol, for memory with no name
(guest RAM, a stack frame, a peripheral register). Dumped words that land
inside a known function are annotated with it, so a saved LR reads as
`HANDLE_INTERRUPT_16+0xd9` rather than a bare address. Dumps happen AFTER
sampling: read at t=0 and you see the state before the target has done
anything, which is how a full log reads as empty.

`--bytes SYM[:N]` is the third shape and the one for BYTE buffers:
`--dump` reads words of a symbol's own storage and `--deref` reads exactly one
uint32, so **neither can hexdump the memory behind a `uint8_t *`** — pointed at
`vga_dac` (a `static uint8_t *`) `--dump` prints the four bytes of the pointer
itself. `--bytes` takes the same three address forms as `--dump` (`name`,
`0xADDR`, `*name+OFF`) and reads N bytes. `--bytes-group G` inserts a gap every
G bytes: at `G=3` the VGA DAC's r,g,b triples line up in columns, so a PHASE
error in a strided stream is visible instead of being counted by hand — which
is exactly how the palette stream was cleared of a channel rotation.
`--bytes-save SYM[:N]:FILE` writes the range out for a byte compare against a
reference. **For GUEST memory use `--guest`, never `--bytes`.**

`--guest-watch LINADDR[=LABEL]` reads a uint32 of guest LINEAR memory at
every sample, next to the `--watch` globals: a game's own counters become a
rate. Ironman keeps a frame counter at 05F7:06B7 (`--guest-watch
0x6627=FRAMES`), and dividing its growth by `pit_now`'s (1.193182 MHz) gave
its vertical-retrace syncs per guest second -- ~650 with the old 0x3DA beam,
70 with the current one.

`--set-at SECONDS` delays the `--set` writes by that many seconds instead of
applying them at once. Its use is re-arming a SATURATING log just before the
event you care about: `--set x86_dos_file_log_idx=0 --set-at 40` records the
file calls an F5 menu makes rather than the boot's. The probe clock runs ahead
of the timeline clock by the firmware's boot time, so check with
`x86_dos_file_calls` that the write landed before the event.

`--artifacts [DIR]` (default `build/artifacts`) copies the `.png`/`.ppm` that a
timeline's `screenshot` events produced out of the run directory **before**
`stop()` deletes it. gwemu resolves those paths against its own cwd, which is
the per-run temp directory, so without this the pictures a timeline exists to
take are written and then thrown away -- which reads exactly like gwemu
ignoring the events.

`--stdio-gdb` selects gwemu's GDB remote protocol over subprocess pipes. Use
this when the host sandbox denies local sockets; it preserves probe and
firmware log-buffer reads. QMP screenshots are unavailable on this transport,
so use timeline screenshots instead.

```bash
# the 768 DAC bytes behind the pointer, as r,g,b triples
python3 scripts/probe.py --retrogo --wait-magic \
    --bytes '*vga_dac:768=DAC' --bytes-group 3
python3 scripts/probe.py --retrogo --wait-magic \
    --bytes-save '*vga_dac:768:build/artifacts/ours_dac.bin'
python3 scripts/probe.py --retrogo --wait-magic \
    --watch core_ready,core_dtcm_ok,benchmark_insns --deref cpu_state_ptr+8=EAX
python3 scripts/probe.py --dump x86_cs_log:16x4=CSLOG  # needs X86_CS_LOG=1
python3 scripts/probe.py --dump 0x2408bf48:16x4=STACK  # or a raw address
python3 scripts/probe.py --target hardware --watch run_flag,benchmark_cycles
python3 scripts/probe.py --retrogo --set run_benchmark_flag=1 --watch benchmark_insns
```

Samples PC (labelled ITCM / firmware symbol / region), watched globals, and
pointer dereferences. Reach for this when a harness reports a value that makes
no sense — it is faster than lengthening a timeline, which is capped at 60s.

#### Reading GUEST memory — `--guest` and `--guest-save`

```bash
python3 scripts/probe.py --retrogo --wait-magic --guest 0x10e80:96=CSIP
python3 scripts/probe.py --retrogo --wait-magic --guest 0x400:64=BDA
python3 scripts/probe.py --retrogo --wait-magic \
    --guest-save 0x0:0xA0000:build/artifacts/ours.bin
```

`--guest LIN[:BYTES][=LABEL]` hexdumps guest LINEAR memory; `--guest-save
LIN:BYTES:FILE` writes it to a file so it can be byte-compared against a
reference emulator's image.

**This is the only correct way to read guest RAM, and `*guest_ram+OFF` is not.**
Since demand paging landed, `guest_ram` is a frame POOL, not a flat image of the
low megabyte: `*guest_ram+0x400` reads the pool's first page, so the BDA comes
back as zeros and a code dump comes back as an unrelated page — a wrong answer
that looks exactly like a correct one. Both options walk `guest_page_table_ptr`
(host = lin + k, one entry per 4 KB page) and render a page whose entry is
`FOLD_INVALID` as `0xFF`, which is what the guest would fetch there, so a dump
never silently invents zeros for memory that is not mapped.

#### Driving the reference PC (`scripts/ref_dos.py`)

```bash
python3 scripts/ref_dos.py --dsk .../WOLF3D.dsk --no-hooks --launch WOLF3D.EXE \
    --settle 30 --samples 5 --interval 8 --text \
    --save build/artifacts/ref.bin --screen build/artifacts/ref.ppm
```

Four traps are handled in the script because each one produced a WRONG ANSWER
rather than an error:

1. **CHS geometry** comes from the volume's own BPB (1 head / 63 sectors here).
   QEMU's default for a 3 MB disk is not that, and SeaBIOS then sits at 0x7C04
   forever showing "Booting from Hard Disk...".
2. **`--no-hooks`** rewrites CONFIG.SYS in a COPY: `XMSHOOK.SYS`/`EMSHOOK.SYS`
   are written for our emulator and hang a reference machine before
   AUTOEXEC.BAT runs, which reads exactly like the title crashing.
3. **The monitor is read to its PROMPT, never slept on.** It echoes every
   character, so a fixed sleep leaves part of one command's echo in the buffer
   and the next command arrives with that garbage prepended.
4. **Paths passed to `pmemsave`/`screendump` are QUOTED.** The monitor parses
   the size argument as an arithmetic EXPRESSION and `/` is DIVISION, so
   `pmemsave 0 0x100000 /home/...` becomes a division by `home` and dies with
   `invalid char 'h' in expression` — having written nothing. Relative paths
   happen to work, which is exactly what hid it.

`--display` defaults to VNC and **must not be `none`**: QEMU derives the VGA
status bits at port 0x3DA from its display refresh, so under `-display none` a
title polling for vertical retrace spins forever. Measured — WOLF3D sat in
`IN AL,0x3DA / TEST AL,1 / JNZ` indefinitely.

**A reference that is RUNNING is not a reference that has reached the same
place.** Check `--samples`, the mode byte and the screendump before trusting any
comparison, or the two images are snapshots of different moments and every
difference between them is noise.

##### `--engine dosbox` — because QEMU **cannot run Wolfenstein 3D at all**

`--cmd 'CMD'` runs any extra monitor command at every sample and `--qemu-arg=X`
passes X straight to QEMU, so a hypothesis about the machine model can be tested
without editing the script. Both were added to find the following, and the
finding is the reason the second engine exists.

**QEMU's port 0x3DA flips display-enable and vertical-retrace TOGETHER**, so the
state Wolf3D's `VL_SetScreen` waits for never occurs:

```
   mov cx,[TimeCount] / add cx,2         ; the only escape from the loop
L1: in al,3DA / test al,1 / jnz L1       ; wait for DISPLAY ENABLE
L2: cmp [TimeCount],cx / jae done
    in al,3DA / test al,8 / jnz L1       ; a VERTICAL RETRACE restarts at L1
               test al,1 / jz  L2
```

`vga_dumb_retrace()` returns `st01 ^ (ST01_V_RETRACE|ST01_DISP_ENABLE)`, so
0x3DA alternates **0x00 ↔ 0x09** and is never 0x01 or 0x08. Measured by
single-stepping the guest over QEMU's gdbstub (`--qemu-arg=-s`, then
`gdb -ex 'set architecture i8086' -ex 'target remote :1234'` and `stepi`):
`eip=56 al=0 … eip=65 al=9 → back to 56`, forever. The TimeCount escape cannot
fire either — TimeCount is advanced only by the game's own INT 08h handler,
which `SD_Startup()` installs *later*. `vga_precise_retrace` is dead code in
QEMU 10.1: the `retrace` property is exposed on **no** VGA model (`VGA`,
`isa-vga`, `cirrus-vga`, …), so `-global VGA.retrace=on` does nothing. **This
stall is QEMU's. It is not the guest's and it is not ours** — and every reading
of "the reference is stuck in the same place we are" that was taken from it is
a coincidence of two different causes.

DOSBox-X models the two bits independently and runs the title:

```bash
python3 scripts/ref_dos.py --engine dosbox --no-hooks --launch WOLF3D.EXE \
    --dsk references/.../WOLF3D.dsk --settle 38 --samples 10 --interval 3 \
    --keys space,space,space,space --key-pace 5 \
    --screen build/artifacts/w.png --save build/artifacts/w.bin
```

That exact line reaches the **episode select** in every slot; drop one press
(`--keys space,space,space`) and every slot is the **main menu** instead. Which
screen a given slot holds is a race between `--key-pace` and the title's own
timing, so read the PNGs — do not cite a slot number. It boots the image's OWN MS-DOS
(`imgmount 2 … -fs none` + `boot -l c`), not DOSBox-X's built-in DOS, so the
software stack matches the QEMU engine and matches ours.

State comes out of DOSBox-X's `autosave=` **save states**, which are ZIPs
holding the whole `Memory` and `Vga` components: `--save` writes the low
megabyte byte-comparably against `probe.py --guest-save`, `--screen` writes one
PNG per sample of the mode 13h / **Mode Y** framebuffer, and each line prints
the BIOS video mode, the INT 08h/09h vectors and the BIOS tick. **INT 08h ≠ the
DOS default is the single number that says `SD_Startup()` completed** — on
DOSBox-X it reads `2aee:0095` and INT 09h `26b7:0009`, i.e. `SDL_SetTimer0()`
and `IN_Startup()` both ran. Under QEMU both stay at DOS's `0de7:…` forever.

Four DOSBox-X traps, each of which produced silence rather than an error:

1. **`[sdl] priority`'s second field is the INACTIVE priority and can be
   `pause`.** Under `SDL_VIDEODRIVER=dummy` the window is never focused, so the
   emulator boots, prints its banner and then STOPS — an `[autoexec]` that
   simply never runs. `priority=normal,normal`.
2. **DOSBox-X's working directory is not yours.** Under the flatpak it is
   `~/.var/app/com.dosbox_x.DOSBox-X/config/dosbox-x`, so every relative path in
   `[autoexec]` resolves somewhere else and `imgmount` fails with nothing
   host-visible. Every path the script writes is ABSOLUTE.
3. **`mount d <hostdir>` is not writable from the guest here**, so the DOS shell
   cannot be used as an output channel. Everything observable comes from the
   save state and the `[log]` file.
4. **The signon screen waits for a key**, and a reference sitting on it looks
   exactly like one that hung. `--keys` is delivered with `autotype -w`, which
   is asynchronous and survives the `boot`.

Three decoding rules in `state_screen()` that a naive reader gets wrong, all
noted in the code. Wolf3D is **unchained**, so a pixel is
`plane = x & 3, offset = y*80 + (x >> 2)` over DOSBox-X's four-byte-interleaved
VRAM — read linearly it comes out quarter-width and repeated, which looks like a
rendering defect rather than a decoding one. The 6-bit DAC is widened to 8 by
**replication**, `(c<<2)|(c>>4)`, as DOSBox-X does — `c*255//63` is off by one
on most values. And **neither the DAC nor the VRAM base is searched for by
shape** (see below).

##### The oracle was miscolouring every frame, and could not tell

`state_screen()` used to take the first 768-byte window that was all-6-bit and
varied as the palette, and take the last 2 MB of the component as VRAM. Both
were plausible and both were wrong:

* The scan matched `vga.dac + 6` — the true table shifted by ten bytes of
  `combine[]`, i.e. three entries and one channel — so every index rendered as
  its neighbour's colour with the channels rotated and Wolf3D's menus came out
  **saturated blue** instead of dark red. Rotation alone leaves greys grey; it
  was the index shift that made it visible, and nothing in the scan could tell
  "found the palette" from "found something palette-shaped".
* VRAM is *not* the tail: `vgapages`, `memsize`, `vga.lfb` and the
  Paradise/Tseng/XGA blocks follow it, so every frame was read 1296 bytes late
  — rolled up by four scanlines.

Both are now read where DOSBox-X actually writes them, and **proven** there.
`SerializeVga::getBytes()` (`references/dosbox-x/src/hardware/vga.cpp:2050`)
writes one POD of `VGA_Dac` (`include/vga.h:573`), which carries the host-pixel
cache `xlat32[256]` right after `rgb[256]` and rebuilds it from `rgb[]` on load
(`vga_dac.cpp:501`) — so `xlat32[i] == pack(rgb[i] widened)` holds for all 256
entries of the live DAC and for no shift of it, no fade buffer and no second
palette the game keeps around. VRAM is located by arithmetic from the
`vgapages` + `vga.mem.memsize` pair written after it (`vga_memory.cpp:3018`).
If either check fails the script **raises**; there is no greyscale fallback and
no "closest match", because a reference that renders the wrong colours without
complaining silently invalidates every colour judgement made against it.

**The image itself is healthy and that was checked host-side, not assumed.** All
eight `.WL1` files plus `WOLF3D.EXE` are present on the MBR-partitioned FAT16
volume with an allocated cluster chain at least as long as the directory size —
nothing truncated, `VSWAP.WL1` exactly 742,912 B.

`scripts/watch_memory.py --addr guest:0xLIN` resolves the same way, for catching
the write that corrupted a guest address rather than reading it after the fact.

#### Catching a derailment: the invariant traps in `x86_trace.inc`

`X86_TRACE_EIP` SAMPLES. These CATCH: they arm a predicate, fire on the first
instruction that violates it, record the state, and **freeze a 64-deep ring of
the linear EIPs that led there**. Read the whole block with
`probe.py --dump x86_oddsp_log:73x1`; the ring is `[8..71]`, oldest at `[7]&63`.

| define | fires on |
|---|---|
| `X86_TRACE_ODDSP` | turns the trap on at all (required by the others) |
| `X86_TRACE_ODDREG R` | first boundary where guest register `R` is ODD; default `REG_ESP`. A 16-bit DOS guest moves SP by two for its whole life, so an odd SP is a defect at the instant it happens |
| `X86_TRACE_LOWEIP N` | first instruction with linear EIP below `N`. "The guest is running the IVT as code" — fires EARLIER in a derailment than any register predicate, so reach for it first |
| `X86_TRACE_LOWSP N` | first boundary with SP below `N`, for a push/pop imbalance whose single leak is invisible and whose drift is not |
| `X86_TRACE_MEMEQ A` + `X86_TRACE_MEMVAL V` | first boundary at which the guest 16-bit word at linear `A` equals `V` — a store watch. A hardware watchpoint cannot do this job here: the host address of a guest page is only known once its fold entry is live, and a guest that halts a second after it starts leaves no window in which a host-side tool can resolve and arm one (`watch_memory.py` armed on a stale entry and pointed at 0x02255706, which is not a host address at all). This fires on the instruction AFTER the store, and the ring names the writer |
| `X86_TRACE_ATEIP A` (+ `X86_TRACE_ATREG R`) | changes what the RING holds: register `R` (default `REG_ESP`) sampled at exactly one guest EIP, one entry per pass. The ring INDEX at `[7]` is then a hit count, which answers "was this instruction ever executed at all" — a 0 there is how the only writer of a variable was shown never to run. A slow stack leak cannot be found with a threshold — the drift is smaller than normal frame churn, so any bound low enough to miss a legitimate `sub sp,N` prologue is already past the start. Sampling the same instruction every iteration makes the per-iteration delta the reading |
| `X86_TRACE_EIPREG A` + `X86_TRACE_REGMASK 0` + `X86_TRACE_MEMVAL 0` | **"who called this function?"** — with a zero mask the register test is a tautology, so the predicate collapses to "the guest reached EIP `A`", and because `ATEIP` is NOT defined the ring keeps plain EIPs. It therefore freezes the last 64 instructions *before the first entry*, which is the CALLER and its whole lead-in. `ATEIP` cannot do this (it records one register per pass at that EIP and nothing else), and a hit COUNT cannot either. This is how Wolf3D's `CA_CacheGrChunk` was shown to be entered from `Quit()` rather than from `InitGame`. `[1]` is the instruction number, `[73]` the CS base, `[74..81]` eight stack words — enough to read a `char *` argument off the frame and name the error string |
| `X86_TRACE_SKIP N` | arms the trap only after `N` instructions. Not optional for a register predicate: BP is a general register during startup (the unpacker loads it with a segment value at instruction 28), so an ungated "first odd BP" fires on a non-defect and hides the real one. The ring keeps filling while disarmed |

**Catch, do not bisect, when the event is rare.** A bisection needs the run to be
reproducible at the granularity you are bisecting; a trap needs nothing. That
mattered here: `X86_TRACE_EIP`'s index used to count DISPATCHES, and a batched
`REP` element re-enters the string handler without passing the sampler, so where
a batch got cut moved with the wall-clock-driven slice and the index differed by
~1,500 between identical builds. `.str_exit` bumps the counter now, and the index
is bit-reproducible (measured: 2,647,648 on three consecutive WOLF3D runs, from
2,453,372/2,454,873 before). **The guest itself was never the problem** —
`guest_insns_total` was 2,723,483 on every run throughout.

**A zero in `x86_eip_log` means NO SAMPLE, not a value of zero.** The index is
bumped for a batched `REP` element (that is what makes it deterministic) but the
sampler only stores from `.resume_fetch`, so a sample whose index falls inside a
batch leaves its slot at 0. Reading those zeros as data is how a healthy stack
sitting at 0xFFB8 was misread as collapsing to 0, and it cost a bisection step.
Filter empty slots out before interpreting a sweep.

**The old `*guest_ram+OFF` spelling now FAILS with a message** in `--dump`,
`--deref` and `watch_memory.py --addr`, rather than serving a wrong page. It is
still in older harnesses and in this file's history, and a reader that returns
plausible bytes for the wrong address is the exact failure class this project
keeps losing hours to.

#### The three clocks, and how to watch them

The emulator loop has three independent clocks (see the block comment at the
top of `src/main.c`). All of them are host-readable, and all of them are
host-**writable** where that is a knob, so the trade is measured on a running
target instead of rebuilt for:

```bash
# what the guest is actually getting
python3 scripts/probe.py --retrogo --wait-magic \
    --watch guest_kips,guest_slice_now,guest_ips_achieved,guest_fps_achieved,\
guest_insn_deficit,guest_ticks,guest_uptime_ms,guest_ticks_dropped,\
frame_ms_last,guest_exec_ms,panel_ms_min

python3 scripts/probe.py --retrogo --wait-magic --set guest_kips=0       # max
python3 scripts/probe.py --retrogo --wait-magic --set guest_kips=330     # 8088
```

#### Where the frame loop IS, and why the device reset

```bash
python3 scripts/probe.py --retrogo --target hardware --no-flash --hard-reset \
    --timeline timelines/keen4-keypress.tl --interval 0.4 --samples 130 \
    --watch x86_frame_phase,x86_frame_phase_seq,x86_frame_phase_worst,\
x86_frame_phase_worst_id,guest_slice_overruns,guest_asm_ms_worst,\
guest_asm_overruns,frame_ms_worst,guest_insns_total,frames_rendered

# is the GUEST executing right now? No build, no flag, no RAM_EMU.
python3 scripts/probe.py --retrogo --target hardware --no-flash \
    --deref cpu_state_ptr+2184=PITCNT --deref cpu_state_ptr+2188=PITREL
```

`guest_insns_total`, `frames_rendered` and `frame_ms_last` are all banked at
the BOTTOM of the frame body, so a stage that never finishes freezes every one
of them and leaves nothing saying which stage it was. `x86_frame_phase` is the
answer, readable WHILE the loop is stalled: `1` FWLOOP, `2` INPUT, `4` GUEST,
`5` VIDEO, `6` AUDIO, `7` SWAP, `8` TAIL (`X86_PHASE_*` in `src/main.c`).
`_seq` climbing while `frames_rendered` does not means an INNER loop is still
turning; `_worst`/`_worst_id` are the longest stage ever seen and which.

`FRAME_PHASE()` also **refreshes the watchdog at every stage boundary**, which
is the fix for the KEEN4 reset ([`docs/13`](docs/13_title_status.md)) --
without it the ~472 ms window covers a whole iteration. Its negative control is
`make -f Makefile.core clean && make -f Makefile.core pack
EXTRA_CFLAGS=-DX86_PHASE_WDOG=0`, which wedges KEEN4 twice in a 52 s run.
`EXTRA_CFLAGS` passes one-off `-D`s for a control build without editing the
source being graded; a changed define does NOT rebuild, so `clean` first.

**`boot_magic=0xbad00005` is `BOOT_MAGIC_BSOD | BSOD_WATCHDOG`, and the device
is already dead when you see it.** retro-go's `BSOD()` then spins on
`buttons_get()` until a button is pressed, so PC samples after it are that
loop and say nothing. Read `x86_frame_phase` instead.

| Symbol | Meaning |
|---|---|
| `guest_slice_overruns` | sub-slice loops abandoned by the `GUEST_SLICE_WALL_MS` (120 ms) wall-clock escape. Nonzero = a guest budget ran long in TIME; the escape makes that a dropped frame instead of a reset |
| `guest_asm_ms_worst` / `guest_asm_overruns` | the longest SINGLE `x86_cpu_interp_asm()` call, and how many passed `GUEST_ASM_MS_MAX` (60 ms). One call is uninterruptible, so this is the real exposure; an overrun halves the next budget. Healthy KEEN4 reads **14 ms** |
| `guest_kips` | emulated CPU speed in thousands of insns/s. **0 = unthrottled, and the default.** `KIPS_*` in `src/main.c` has the profile rungs; the PAUSE/SET menu's "CPU Speed" entry sets it too |
| `cpu_profile_persisted` / `cpu_profile_events` | what the settings store kept when the menu entry was changed, and how many dialog callbacks reached us. The menu path has no other witness — a malformed options array never calls us at all. NOTE the app-scoped store this reads is per-APP, not per-ROM; the per-TITLE value now lives in `<TITLE>.dosset` and this is the fallback under it |
| `settings_cfg_status` / `settings_cfg_bytes` / `settings_cfg_applied` | PER-TITLE `.cfg`: the `x86_cfg_status_t` (0 = OK, 1 = ABSENT, 2 = TOOBIG, 3 = BINARY), bytes read, and keys applied. **`bytes` non-zero with `applied` 0 is the normal reading for every title shipped so far** — their `.cfg` files came from the prior port and are entirely comments. `bytes` 0 means no file was found, which is where a wrong path shows up |
| `settings_dset_status` / `settings_dset_bytes` | PER-TITLE `.dosset` (the user's menu overrides): `x86_settings_status_t`, 1 = ABSENT and is normal until somebody changes a row. Anything ≥ 2 is a corrupt or truncated file and the title ran on defaults |
| `settings_saves` / `settings_save_errors` | `.dosset` files written, and writes that failed. **A non-zero error count is the `MAX_OPEN_FILES = 8` budget** as often as it is a full card |
| `frame_10us_avg` | mean iteration period, 8x oversampled, in 10 us units. What the firmware's `common_emu_frame_loop()` observer is fed |
| `upper_filler_ok` | is 0xA0000-0xFFFFF backed? It comes from the bonus pool now, but a 0 here explains an option-ROM scan faulting |
| `guest_insn_deficit` | instructions the emulated CPU owed and never ran. The answer to "I asked for a 386 — am I getting one?". Meaningful only in paced mode |
| `guest_ips_achieved` / `guest_fps_achieved` | measured over a 1 s window. The number that matters in unthrottled mode |
| `guest_ticks` / `guest_ticks_dropped` | IRQ 0s handed to the guest, and ones refused by the catch-up cap or by the one-deep pending slot. **The rate is the GUEST's, not 18.2065 Hz**: it is the PIT input divided by whatever channel 0 holds |
| `guest_tick_divisor` / `x86_pit0_writes` | the PIT channel 0 divisor in force, and how many times a guest has programmed port 0x40. 65536 is the boot value (18.2065 Hz); Wolfenstein 3D writes 0x214B (8523) for 140 Hz and paces its turning, animation and menu key-repeat off the resulting ISR. **A `guest_tick_divisor` of 65536 on a title whose `x86_pit0_writes` is non-zero means the program was dropped** -- which is what made Wolf3D's whole sense of time run at 13% of real. `guest_ticks / guest_uptime_ms` should read back as `1193182 / guest_tick_divisor` |
| `guest_tick_replans` | times the guest reprogrammed channel 0 MID-FRAME and the rest of the frame's ticks were re-planned at the new rate (the port 0x40 write ends the sub-slice early). Normally 1 per rate change. Before it existed the first gap after a speed-up was up to a frame of instructions, and Monkey Island (EGA), which counts loop passes over one tick and divides by 12 into a byte, died of a #DE on about half its boots. Up to **24** IRQ 0s per frame now (was 8, which dropped a quarter of a 473 Hz clock at anything under 60 fps) |
| `panel_ms_min` | the display period, **learned** from the shortest iteration ever completed. Nothing assumes 60 fps |
| `x86_kbd_read_log` / `x86_spk_log` | the last 32 codes the controller handed out on port 0x60 (once per code, however often the guest re-reads it), and the last 8 writes that can move the speaker tone. `--bytes x86_kbd_read_log:32` (bytes since 2026-10-24) |
| `x86_kb_pushes` / `x86_kb_pops` / `x86_kb_peek_hits` | the BDA ring's producer and its two consumers. "The guest polls INT 16h forever" has two opposite causes -- nothing was queued, or something dequeued it first -- and only these three separate them. `pushes == pops == peek_hits` means every key was queued, seen and taken; `pushes > 0` with `pops == 0` is the reported failure |
| `x86_dos_load_bytes` / `x86_dos_load_short` / `x86_dos_load_shorts` | **Whether the MZ loader actually got the whole program in.** `x86_guest_copy_in()` stops at the first page the pager cannot resolve and returns how far it got, so an image can land PARTIALLY -- and the loader used to just stop, returning success, leaving the tail of the program as whatever those pages already held. The guest then dies much later and somewhere else, executing `0xFF`, which is the hardest failure in `x86_dos.c` to diagnose from the outside. `_bytes` is how much of the LAST image reached guest memory, `_short` how much did not, `_shorts` how many loads came up short. **Read these before blaming an opcode for an `exit_reason=4` at an address full of `0xFF`** -- that is exactly the check that proved Dark Forces' DOS4GW load was COMPLETE (61,604 B, short 0) and moved the diagnosis off memory entirely |
| `x86_dos_overlays` / `x86_dos_overlay_seg` / `x86_dos_overlay_reloc` | `AH=4Bh AL=03h` (LOAD OVERLAY). An overlay is not a process -- nothing is allocated and nothing will terminate -- so the `x86_dos_exec*` counters cannot describe it. `_seg` and `_reloc` are the TWO WORDS of the AL=03h parameter block as the core read them: the load segment and the relocation factor, which are separate and need not be equal. Dark Forces passes them equal (both 0x81) |
| `x86_dos_sysvars_calls` / `x86_dos_parse_calls` | `AH=52h` (list of lists) and `AH=29h` (parse filename into FCB). Call counts ONLY, and deliberately so: the SysVars block is ordinary guest memory reached through the fold table, so **which OFFSETS a title reads cannot be seen without faulting the granule**, and the pager has no read-side mechanism for that -- the seam machinery is write-side and code-side. Knowing which fields matter is worth more than implementing all of them, so if a title ever needs that answer the way to get it is to put the block on its own granule and fault it, which is a pager change. What the counts DO settle is whether the call happened at all: Dark Forces reads 2 and 2 |
| `x86_dos_file_log` / `x86_dos_path_log` | The **first 16** file-family INT 21h calls, four words each -- `(func << 8) \| AL`, `(CF << 16) \| AX`, `BX`, `CX:DX` -- plus the last four paths. Read it with **`probe.py --dos-log`**, which decodes it to one line per call. It **SATURATES rather than wrapping**, and that is the point: a guest stuck in a loop overwrote a 32-slot ring with the loop body in milliseconds, so the call that ISSUED the handle the loop was failing on was always gone -- WOLF3D's 32 identical AH=42h slots said nothing, and with 96 saturating slots the answer was in entry 2. The old two-word form also could not say WHICH handle, which is the only question a failing seek raises. `x86_dos_file_calls` counts every call whether recorded or not; write 0 to `x86_dos_file_log_idx` to re-arm for a later phase. `x86_hle_log` cannot answer a file question at all: a guest polling the keyboard fills all 16 of its slots between one open and the next. **The MEMORY calls `48h`/`49h`/`4Ah` also take a slot** -- an arena that refuses an allocation is unreadable as a bare count, which is exactly how Dark Forces' 15 `4Ah` calls sat unexplained -- but they are NOT counted in `x86_dos_file_calls`, and for them slot+3 is `(ES << 16) \| DX`, since ES is the block for `49h`/`4Ah` and DX means nothing there. `--dos-log` prints those as `BX=... paras ES=...` |
| `x86_opl_t1_gap` | **the AdLib probe's delay, in PIT ticks** -- the longest wait ever observed for OPL timer 1, read against the 96 that preset FFh needs. It is what separates "the card refused" from "the guest's wait was too short for our clock", and it is what named the Sound Blaster non-detection (`docs/03` §4e). `x86_io_isa_ticks` beside it is the ISA bus time charged to FM-port accesses; 0 means the negative control is built |
| `x86_io_log` | Every port access, **coalesced by (direction, port)** into 16 runs (32 until 2026-10-24), decoded by **`probe.py --io-log`**. A diagnostic build can lengthen it (`make -f Makefile.core pack EXTRA_CFLAGS=-DX86_IO_LOG_SLOTS=512`); the decoder reads the length from `x86_io_log_slots`. Coalescing is what makes it useful: a mode-13h palette upload is 768 consecutive writes to 0x3C9 and would flush every other port out of a raw ring. What it shows is the SHAPE of a guest's hardware conversation -- which register files it programs, in what order -- and that named WOLF3D's wall in one run: `OUT 3D4,0F / OUT 3D5,66 / IN 3D5` reading back 0xFF, i.e. a CRTC write-then-read-back probe against a register file that did not exist. It also caught the FIX overshooting -- answering the 0x3B4/0x3B5 mono alias too made the guest conclude MONO and poll 0x3BA 32,000 times |
| `x86_vga_planar_stores` / `_cls_plane` / `_cls_copy` / `_cls_general` | Guest stores that took the planar hook, split by WRITE CLASS -- the class is decided at `OUT` time, not per store. `_plane` is the CPU byte straight into the enabled planes (write mode 0, no Set/Reset, Bit Mask 0xFF, no rotate, no ALU); `_copy` is the write-mode-1 latched page copy; `_general` is everything else and is the only class still paying the full pipeline. **The MIX is the number that says whether a fast path is earning its risk**, and `_general` is the one to watch: WOLF3D in Mode Y gameplay reads 0 there, so the fast paths cover its whole frame. The class counters count PER BYTE while `_stores` counts per store, so a guest doing word stores makes them exceed it -- that is not a discrepancy. Read the set with `probe.py --watch planar` |
| `x86_disk_reads` / `x86_disk_backend_reads` | guest-side `disk_read()` calls and how many of them reached the SD card. The RATIO is the sector cache's whole point, it is exact under gwemu, and it is the number to quote — TOPBENCH boots on 115 / 7 |
| `vga_aperture_ok` / `x86_vga13_ok` | is the mode 13h aperture (64 KB, bonus pool) and its palette block backed? A 0 in `x86_vga13_ok` means a guest asking for 13h is REFUSED and logged (event tag 5) rather than drawing into the open-bus filler -- which is the right behaviour, but it is a whole mode silently missing |
| `x86_vga_dac_writes` | DAC bytes the guest has streamed through 0x3C9. **0 while a screen is black says the guest never got as far as the palette**, which separates "13h is broken" from "the guest never reached video" -- the distinction WOLF3D turned on |
| `disk_cache_mem` | the AHB pointer the sector cache got. The AHB heap is shared with the firmware's own newlib allocations, so unlike a bump pool its free space is NOT knowable from the link map; this is the only witness |
| `x86_disk_cache_ok` / `x86_disk_cache_bytes` | is the cache BACKED? A 0 here is a silent-allocation failure and the only symptom is the stutter the cache exists to remove |
| `x86_disk_cache_hits` / `_misses` / `_fills` / `_evictions` / `_bypass` | sectors served from a resident window, sectors that forced a fetch, window fetches, and requests that went straight through. Rising `_evictions` with a poor hit rate means the working set no longer fits four windows |
| `x86_pg_wb_us_total` / `_us_worst` / `pgf_us_open` / `pgf_us_seek` / `pgf_us_write` / `pgf_us_read` / `pgf_seek_us_worst` / `pgf_write_us_worst` | **What a pagefile writeback costs, and WHICH PART of it.** Measured on hardware over 3 minutes of WOLF3D play, `_us_worst` reached **433,509 us for ONE 4 KB write** against 23,531 us for an ordinary sector read -- 20-80x -- and it GREW over the session. A single number cannot say whether that is `fopen`, `f_lseek` or the card, and the three have completely different fixes, so the phases are split. **Read `pgf_us_seek` first**: the firmware builds `FF_USE_FASTSEEK 0`, so `f_lseek` walks the cluster chain from byte zero on every call, and a seek past EOF in write mode allocates the intervening clusters -- the only candidate on the list that grows with offset and with session length. `_us_total` exists because a WORST case cannot be divided into a frame budget and attribution needs a total |
| `x86_disk_us_total` / `x86_disk_us_worst` / `x86_disk_errors` | time in the backend, and the worst single call. **Meaningless under gwemu** (`docs/04`: SD busy state is not modelled) and the reason the timing claim needs silicon |
| `frame_hist` / `frame_ms_worst` / `frame_overruns` | the frame-time TAIL, which is what "stutter" means. Buckets are multiples of the learned panel period, so `frame_hist[0]` is on-time and anything else is a missed deadline. A mean or a minimum cannot show a hitch; only this can. Write `frame_stats_reset=1` to open a window around a specific event. `--dump frame_hist:8x4` |
| `x86_video_mode_log` / `_idx` | the last 16 events that can change what the renderer draws, as `(tag << 24) \| (aux << 8) \| value`: **1** mode adopted (value = mode, aux = columns), **2** a port 0x3D8 write, **3** the renderer following a change (aux = is_text), **5** a mode REFUSED because nothing renders it, **6** an INT 10h AH=00 set-mode REQUEST, logged before the decision (a set to the mode already up adopts nothing and so has no tag 1, yet it still CLEARS the display -- which is how Commander Keen 4's banner disappears, and without tag 6 that wipe has no cause in the ring at all). A sampled mode byte cannot tell "never switched" from "switched and the renderer did not follow" — both read as a constant — and this can. `--dump x86_video_mode_log:16x4` |
| `x86_int_vec_log` | **needs `X86_CS_LOG=1`** (see below). The last 16 interrupts dispatched through the real-mode IVT, as `(vector << 16) \| the CS installed`. `x86_cs_log` says only "LR was `HANDLE_INTERRUPT_16`" -- it cannot name the vector, and a guest that derails after its own code has touched low memory is exactly the case where that is the whole question. `0x00080000` (INT 08h into CS=0x0000) is what named Commander Keen 4's failure in one dump. `--dump x86_int_vec_log:16x4` |
| `x86_div_error_eip` | **needs `X86_CS_LOG=1`**. `[0]` the linear EIP of the last divide error (#DE, already past the opcode and ModRM), `[1]` how many there were. A title that dies of "run-time error R6003 integer divide by 0" says only THAT it divided; this says WHERE -- it is what named Monkey Island's speed calibration (`DIV BL` of a loop count over one timer tick). `--dump x86_div_error_eip:2` |
| `x86_hle_log` | the last 16 services as `(vector << 8) \| AH`, bit 16 set if nobody answered. This is what says whether a guest is polling INT 16h AH=00 or AH=01 -- a distinction the two paths do not share a symptom for. `--dump x86_hle_log:16x8` |

Guest time must not move with render rate. The measurement that proves it is
`guest_ticks / guest_uptime_ms` at two different frame rates — force a low one
with a large `--set guest_kips=…` or by loading a heavy guest.

#### Cold guest RAM in NOR, and what to read off it

```bash
# the whole story in one word; the timeline opens the menu and presses A on
# "Cold RAM->NOR" after letting the guest settle for 20 s
python3 scripts/probe.py --retrogo --wait-magic \
    --timeline timelines/nor-snapshot.tl --samples 34 --interval 1.5 \
    --watch nor,guest_ips_achieved,frames_rendered
```

`--watch` takes GROUP names as well as symbols; `nor` is the set below. Groups
live in `WATCH_GROUPS` at the top of `scripts/probe.py` — add to that rather
than pasting seventeen symbols that go stale the moment a counter is added.

| Symbol | Meaning |
|---|---|
| `nor_snap_result` | 0 = never run, 1 = ok, else the reason. **8 is the one to fear**: the firmware's ROM cache served a HIT, programmed nothing and never called our relocate callback, so the address it returned points at an OLD snapshot. The core refuses to remap on it. See `NOR_SNAP_*` in `src/main.c` |
| `nor_snap_cb_bytes` | bytes the relocate callback actually filled. Anything other than 655,360 means the snapshot in flash is not the guest's memory, whatever the returned address looks like |
| `nor_snap_addr` / `nor_snap_ms` | where the firmware placed it, and what the whole operation cost (placeholder file to SD, erase, program, readback verify). **1,595–1,614 ms under gwemu, which is NOT a timing claim** — `docs/04`, and OCTOSPI erase/program latency is not modelled |
| `x86_nor_attached` / `x86_nor_attach_err` | is the guest running out of NOR at all, and if not, which check refused it. `7` means the flash readback did not match guest RAM — the fold table was never touched and the guest is still on AXI, which is the invariant the design turns on |
| `x86_nor_pages_nor` / `_axi` / `_total` | **the measurement.** Pages still served from NOR versus pulled back by copy-on-write. `pages_nor / pages_total` is the answer to "what fraction of this guest's RAM is genuinely read-only once it has settled", and it is measured rather than guessed because the policy is demand-driven with no heuristic in it |
| `x86_nor_axi_free` | AXI bytes reclaimed — what is left unclaimed in the COW pool. The pool IS the old guest RAM block; `ram_malloc()` has no `free()`, so this is the honest number rather than a claim about the allocator |
| `x86_nor_reads` / `x86_nor_writes` | slow-path reads and writes. Reads dominate by orders of magnitude and include every INSTRUCTION FETCH from a NOR-backed code page — that is the cost of the trade, and it is why `guest_ips_achieved` is the number to read next to these |
| `x86_nor_cow_pulls` / `x86_nor_host_cow` | pages copied NOR → AXI, and how many of those were forced by a HOST write rather than a guest one. The host side (BDA tick, INT 21h filling a DTA, a disk read landing in a guest buffer) goes through `x86_guest_ptr()` and never touches the trap stubs; a `pages_axi` that is implausibly small is what exposes a host path storing straight at the XIP window, which gwemu makes look like it works |
| `x86_nor_cow_failures` / `x86_nor_unmapped` | the loud failures: the COW pool empty (structurally impossible — it is exactly as large as the snapshot) and traps that were a genuine unmapped access. Both should be 0 |
| `x86_nor_slow_cycles` | DWT cycles inside the slow path. Ranking only under gwemu |
| `nor_snap_wdog_gap_ms` | **the WATCHDOG number.** The worst interval between two watchdog kicks over one snapshot, i.e. the longest stretch that would have been un-kicked without them. Read it against the **~472 ms WWDG window** — that constant is not ours, it is named in `references/game-and-watch-retro-go-sd/Core/Src/gw_flash.c`'s `OSPI_Program()`, which kicks per 256-byte page for exactly this reason. The snapshot is a MENU callback, so the frame loop's once-per-frame `wdog_refresh()` is not covering any of it. A value near the window says this path can only run chunked and says which chunk is the offender |
| `x86_nor_heartbeats` | kicks made by `x86_nor_attach()`'s readback, one per page (160 for a full 640 KB snapshot). Counted whether or not a hook is installed, so tier 1 can assert coverage with no watchdog to observe. **0 after a snapshot means the hook was never wired** — the failure this counter exists to catch |

#### The VRAM arena, A20, and the pin state

```bash
python3 scripts/probe.py --retrogo --wait-magic \
    --timeline timelines/start-preselected-game.tl --samples 2 \
    --watch x86_arena_bytes,x86_arena_used,x86_arena_free,x86_arena_largest,\
x86_arena_refusals,x86_arena_deficit,x86_pinned_pages,x86_a20_state
```

| Symbol | Meaning |
|---|---|
| `x86_vga_modey_alias_hits` / `x86_vga_modey_pages` | **The gate on `X86_MODEY_EXTRA_PAGES`, and the WOLF3D golden is not.** Each extra Mode Y display page costs 65,536 B / 16 pager frames donated to the arena, so the knob trades a RENDERING defect against a PAGING one. A/B'd at tier 2 on `timelines/wolf3d-still.tl`: **2 extra pages** -> `pages=3`, `alias_hits=0`, `frames_total=126`, 40 evictions; **1 extra page** -> `pages=2`, `frames_total=142` (+16 back to the guest), 18 evictions, and `alias_hits` **13,119,029**. The in-code claim that one extra page "already removes the hazard" is FALSE and this is what disproves it. **The golden passed on BOTH arms** -- it captures the credits screen, which never exercises a third display start -- so grading this knob on the golden alone would have shipped the doubled-walls defect back. Grade it here |
| `x86_vga_modey_cache_offs` / `_lost_cache` / `_lost_lo` / `_lost_hi` | The Mode Y off-screen cache: the plane offsets FITTED (it is sized from the arena, not fixed -- 15,616 on the shipping layout, covering 0xC300..0xFFFF), stores discarded past it, and **the lowest and highest offset a discard happened at**. The last two are the point: a discard COUNT cannot say how much bigger the cache needs to be -- 16,896 discards reads the same whether they are one offset past the end or spread over 8 KB -- and these named the shortfall as the window's TAIL in one run |
| `x86_arena_bytes` / `_used` / `_free` / `_largest` | the display's AXI, as one pool instead of three permanent blocks. **`_largest` is the number that decides whether a mode fits**, not `_free`: a claim's block must be contiguous, so a fragmented arena can have room for an EGA plane and no run to put it in |
| `x86_vga_ega_card_offsets` | the EGA 0Dh card actually fitted, in PLANE OFFSETS (one offset is four interleaved bytes, so the card is 4x this). Sized from the arena at the mode set, not fixed: 0Dh lends the CGA claim out and pulls the 13h aperture down to the bottom, which is 19,456 offsets on the shipping layout against the 8,192 that cut Keen 4's intro off at row 128. Read it with `x86_vga_planar_lost` and `x86_vga_latch_miss` -- a small card shows up there first |
| `x86_arena_deficit` | bytes the LAST refused claim could not place. This is what a refusal message quotes -- "EGA needs 118,784 bytes more than exist" is actionable and "EGA failed" is not |
| `x86_arena_refusals` / `_claims_ok` | claims refused outright and claims satisfied. A claim is **all or nothing**: the previous port measured a partial EGA card as producing worse output than no EGA (8 KB/plane truncates cleanly; 16 KB and 32 KB alias the floating pages onto each other and fill the screen with noise that reads as a rendering defect). A rising `_refusals` with a black screen is the system working |
| `x86_pinned_pages` | guest pages the pager may never evict. 24 with CGA VRAM and the 13h aperture mapped (8 + 16); a 0 here while a mode is up means the display's storage is evictable, which is a defect the picture would show a frame later |
| `x86_livemap_hits` | evictions vetoed because a host pointer into the page might still be live. `cow_lost == 0` does NOT cover this -- the previous project measured 5 such evictions producing 26,329 wrong bytes with that invariant green throughout |
| `x86_x87_escapes` | x87 ESCAPE opcodes (D8-DF) the interpreter did NOT execute. The x87 core (`x86_x87.c`) covers what the shipped library demands and no more, so this counts the REST: transcendentals, `FPREM`, BCD, `FSAVE`/`FRSTOR`/`FLDENV`/`FNSTENV`, m80 loads and stores, and `FCMOVcc`. **Non-zero after a Dark Forces run means one of those was reached and the arithmetic downstream of it is wrong** -- the full in/out list is [`docs/07`](docs/07_opcode_coverage.md) "x87 -- what is built". It was previously the count of every ignored ESC, back when nothing was implemented |
| `x86_x87_ops` | x87 instructions the core DID execute. The companion to the above: a runtime's `FNINIT` probe costs a handful, a title actually computing in floating point runs millions |
| `x86_a20_state` / `x86_a20_changes` | the A20 gate, and how many times a guest has actually MOVED it (a probe that writes an already-open gate is not a transition). While it is masked the HMA aliases the bottom of memory; while it is open there is memory above 1 MB at all |
| `x86_pg_frames_total` / `_frames_free` / `_resident` | the pager's AXI, in 4 KB frames, and how many guest pages are resident. **`_frames_total` is the number to check first after any change to the core**: it read 159 instead of 160 purely because `ram_malloc()` returned a 4-byte-aligned block, losing a frame to a partial page at each end |
| `x86_pg_faults_r` / `_faults_w` / `_faults_host` | fold misses serviced, split by cause. `_faults_host` are from `x86_guest_ptr()` and carry **every DOS program load** -- loading is a copy into memory the guest has never touched, so under demand paging all of it faults from the host side. A title whose `_faults_r` is 0 is not broken: any page the host wrote is already resident |
| `x86_pg_fill_zero` / `_fill_nor` / `_fill_file` | where each faulted page's contents came from. The ratio is the answer to "how much of this guest's memory has ever been anywhere but ZERO" |
| `x86_pg_donated` / `_donate_bytes` / `_donate_refused` | frames lent to the VRAM arena for a bigger EGA/Mode Y card, and refusals. **The floor is `X86_PG_DONATE_RESERVE`, and it is a WORKING SET, not a card size.** It shipped at 100 against KEEN4's measured 134-frame peak, which cost that title 84 evictions and 82 pagefile writes in 15 s -- free under gwemu, up to 433 ms EACH on silicon. Now 134, and KEEN4 measures 0/0 on hardware. A claim may not span arena regions, so the card is the best SINGLE run and never the sum: beating the arena needs ~35 frames, so above a working-set-safe reserve the donation is inert for Keen |
| `x86_pg_exhausted` | **the loud one.** No frame was free and no victim could be taken, so the guest got a clean `X86_EXIT_MEM_FAULT`. A pool that silently ate the store would be indistinguishable from one that works |
| `x86_seam_syncs` / `_writes` / `_overflow` / `_flushes` | THE GRANULE SEAM (`x86_seam.c`). `_syncs` is how many straddling accesses were SERVED -- the neighbour's leading bytes pulled into the low frame's eight slack bytes; `_writes` is how many bytes a straddling STORE pushed back at the instruction boundary; `_flushes` how many instruction boundaries actually ran a push. **`_overflow` is the loud one**: a straddle that could NOT be served -- a boundary whose low granule has no frame (a fixed mapping, or a NOR-backed page: neither has slack and neither's neighbour is ours to write), or five live boundaries in one instruction against four slots. A non-zero `_overflow` means some access fell back to the old, wrong behaviour, and it is a number to read rather than a silence to infer from. WOLF3D measured 52,216 / 1 / 2 / 52,188 over 25 s: the seam is RARE, and one wrong byte per 52,000 is exactly the shape of a defect that survives for months |
| `x86_pg_unmapped` | faults for addresses OUTSIDE the pager's window. Not a page fault -- a genuinely unmapped address, i.e. the guest's own wild pointer. Kept separate on purpose: conflating the two turns a guest bug into a silent allocation |
| `core_dtcm_next` / `core_dtcm_headroom_max` | where the firmware's DTCM bump pool got to after the core's allocations, and an UPPER BOUND on what is left. `GW_MEM_OP_FREE_SIZE` is "RAM only today", so the ABI cannot be asked; this is the only witness. **It used to over-report by 24,576 B** — it bounded against the end of the RAM (`0x20020000`) when the pool ends at `__dtc_padding_end__` = `0x2001A000`, the 24 KB above it being the main stack. The old "96,068 B free" reading, and the 87,844 that a task was planned against, are both that error. Now reads **63,268 B free above a bump at `0x2000A8DC`**. See [`docs/12`](docs/12_memory_tiers.md) §8 |
| `x86_vga_aperture_dtcm` / `_dtcm_fail` | where the 64 KB 13h aperture landed: 1 = DTCM, 0 = the AXI arena. `_dtcm_fail` = 1 means DTCM was asked and refused. **It reads 1 today** — the aperture is 2,300 B too big for what the pool has left — and the arena fallback is the old path unchanged. `docs/12` §8 has the arithmetic and the one route left |

### `probe_report.py` — what did this title ask for, and how did we answer

```bash
python3 scripts/probe_report.py --title keen4
python3 scripts/probe_report.py --title wolf3d --settle 30
python3 scripts/probe_report.py --rom .../GAME.dsk --name mygame
python3 scripts/probe_report.py --title keen4 --skip-build --skip-cost   # fast
python3 scripts/probe_report.py --compare before.json after.json
python3 scripts/probe_report.py --title epicpinball \
    --core-flag X86_DOS_EXEC=0 --skip-cost      # measure a NON-DEFAULT core
```

`--core-flag NAME=VALUE` (repeatable) is passed straight to `Makefile.core`. It
exists because the useful question is often "what does the title ask for once we
ANSWER something the shipping core refuses" — Dark Forces could not be diagnosed
at all until `X86_DOS_EXEC=1`, which was 0 by default at the time. It works the
other way round too, and that is how the flip to `X86_DOS_EXEC=1` was graded:
`--core-flag X86_DOS_EXEC=0` rebuilds the OLD behaviour so a title's before and
after come from the same runner. The flags are recorded in
the report's `meta.core_flags`, so **a report on a non-default core says so on
its face**; without that a number gets quoted later as if it described the
shipping build.

Boots a named title, lets it run **from its first instruction** (no settle-then-
reset — the startup probes *are* the subject), and writes one report to
`build/artifacts/probe_<title>_<target>.md` plus the same content as JSON.
Presets: `keen4`, `wolf3d`, `topbench`, `alleycat`, `darkforces`,
`epicpinball`, `mortalkombat`.

##### THE DEATH SITE — `cpu_*` rows, and why they exist

The health table carries eight `cpu_*` rows read by dereferencing
`cpu_state_ptr` (`_read_death_site()`, offsets from `x86_offsets.inc`):
`cpu_exit_reason`, `cpu_exit_info`, `cpu_cs_sel`, `cpu_eip`, `cpu_lin_eip`,
`cpu_ss_sel`, `cpu_esp`, `cpu_cr0`.

**`guest_exit_reason` says the guest stopped; these say WHERE and ON WHAT.**
They were added because three unrelated titles were reported for several passes
as dying at one identical `CS:EIP` on one identical opcode, and both halves of
that were wrong — the opcode was the `0F` ESCAPE rather than the instruction,
and the `CS` had not been read from the CPU at all. `docs/13` has the
retraction; the rule it leaves behind is that **a death site quoted without
these rows is a death site nobody read off the machine.**

The offsets duplicate `x86_offsets.inc`, and a duplicated offset that drifts
reports the NEIGHBOURING FIELD rather than failing, so
`python3 tests/test_probe_offsets.py` checks all eight against the generated
values. It needs no target and runs in a second.

* **`cpu_exit_info` on `guest_exit_reason=4`** is the opcode. `0x00`–`0xFF` is
  one byte; **`0x0Fnn` is the two-byte opcode `0F nn`**; `0xMM0F01` is group 7
  with its ModRM. `docs/07` has the table.
* **`cpu_cr0` bit 0 answers "did this title enter protected mode"** with no
  build flag and no guessing. It is how "the descriptor walk has never been
  exercised by any title" was checked every pass — and how it was finally
  falsified.

The health table also carries the **SOUND** set (`x86_sb_*`, `x86_dos_env_seg`),
because the port table cannot tell "programmed a transfer" from "played one".
`x86_sb_starts` against `x86_sb_samples` is exactly that difference, and
`x86_sb_irqs` against `_irqs_delivered` separates "the card asked" from "the
guest was given it" — which is the hang the SB path exists to remove. See
[`docs/03`](docs/03_devices_and_hle.md) §4c for what each one rules out.

##### `X86_PROBE=1` USED TO TAKE THE PAGER TO ZERO FRAMES — FIXED, BOTH ENDS

The accumulator is ~2.8 KB of RAM_EMU and **`guest_ram` is the last claim out of
that pool**, so the instrumented build pushed it over the edge: `ram_malloc()`
returned NULL, `x86_pg_frames_total` read **0**, every guest access was
`X86_EXIT_MEM_FAULT` and the guest halted before its first instruction, while
`core_dtcm_ok`, `pgf_result` and `x86_xms_total_kb` all still read healthy. That
is the trap. Measured on `darkforces` and reproduced on `wolf3d` as a control,
both dead with `core_ready=0`.

MEASURED, and the numbers are why the fix is what it is: RAM_EMU has **655,360 B**
free at that claim with the accumulator in, and a 160-frame pool wants **656,640**
(stride 4,104, not 4,096) — **1,280 B short, exactly one frame.**

* **`src/main.c` now sizes the pool to the free space** instead of taking it
  all-or-nothing, and publishes `guest_ram_pages`, `guest_ram_pages_short` and
  `guest_ram_free_at_claim`. The instrumented build runs on **159** frames with
  `x86_pg_evictions=0` (WOLF3D) rather than on none. The shipping build is
  unchanged: 160 pages, `guest_ram_pages_short=0`, 659,456 B free.
  **`guest_ram_pages_short` is the counter to read after adding ANY RAM_EMU
  storage** — one frame per 4,104 B comes straight out of the guest.
* **`X86_PROBE_ENTS`** (`Makefile.core`, default 128) is the other lever: 64
  saves exactly the 1,280 B and buys the frame back, at 64 keys against the 60 a
  real title used.
* **`probe_report.py` REFUSES to print a table** — no rows, exit status **2** —
  when `core_ready` is 0, `x86_pg_frames_total` is 0, or `guest_insns_total` is
  not advancing *and the accumulator is empty*. It samples that counter twice
  across 1.5 s, because a guest that faulted at instruction 1 has a non-zero
  count that never moves.
* A guest that ran, **asked things and then died** is a different state and is
  NOT refused: the table is printed under a **THE GUEST IS STOPPED — THIS IS A
  POST-MORTEM** banner (still exit 2), because what it asked for before it died
  is the whole diagnosis. `guest_insns_total` only accrues while the guest is
  not halted, so a title that dies inside its first frame reads 0 however much
  it did first — `darkforces` reads 0 with 67 recorded events.

When you see a refusal, re-measure with the rings against a build without the
accumulator: `probe.py --dos-log --io-log --watch …` answers the same question
and costs no RAM_EMU.

**The report leads with what we did NOT answer**, then with the hardware we
answered *absent*, and only then lists everything that worked. The column that
matters is `status`:

| status | means |
|---|---|
| `unanswered` | nobody claimed it. The guest kept whatever was in AX |
| `default-arm` | a catch-all produced a shaped, **invented** answer |
| `unmodelled-port` | the access fell off the end of the switch in `x86_io.c` |
| `unclassified` | answered, but nobody has vetted the answer — **ranked high** |
| `stub` | returns success and does nothing |
| `partial` | right for some inputs, falls through for the rest |
| `absent-by-design` | we say "this hardware is not here", and that is TRUE of the machine we claim to be. Not a defect — but it *is* what a title may be dying on, so it gets its own section |
| `implemented` | modelled |

**Runtime evidence outranks the table.** `unanswered` and `unmodelled-port` are
facts the target observed (`rc==0`, `x86_hle_unhandled_*` moving, the switch
fall-through) and cannot drift. The curated table in
`scripts/common/probe_expect.py` only classifies calls the target says *were*
answered — which is exactly the case the target cannot see the difference in.
Anything the table does not cover is `unclassified`, never quietly
`implemented`: a new probe appearing in a title is the thing this tool exists to
surface, and a table that swallowed it would defeat the tool.

That distinction is the whole point. `INT 21h AH=44h` with `AL>=2` returns
`CF=1 AX=0001` from a catch-all; a real handler returning the same thing is
indistinguishable in every other trace in this tree. Here it is a column.

It found, on its first three runs: Keen 4 and Wolf3D both stopping at the
`INT 10h AH=12h BL=10h` EGA gate, and **Alley Cat reading PIT channel 0 (port
`0x40`) 2,013,208 times in 28 s against a port nothing models** — a clock that,
from the guest's side, never moves.

`--compare A.json B.json` diffs two runs and reports answers that were **FIXED**
or **REGRESSED**, probes that appeared or vanished, and counts that moved — so a
fix can be shown to have changed an answer. Poll counts jitter run to run under
a frame-paced emulator; the *classifications* are stable, which is why the
compare leads with those.

#### Cost, and why it is off by default

`X86_PROBE=1`, off by default like every other instrument. **Every report prints
the delta it measured by building the core both ways in that same run** — not an
estimate, because RAM_EMU headroom in this tree has gone negative silently more
than once. On this tree:

| | |
|---|---|
| RAM_EMU | **+3,568 B** (the 2,848 B accumulator plus code) |
| `dos.bin` | **+720 B** — code only. The block is deliberately uninitialised `.bss` so it does not *also* occupy the packed image |
| hot path | **nothing.** One hash probe per INT service and per port access; never per guest instruction |

`--skip-cost` skips the second clean build when the number is not wanted.

#### Why an accumulator and not a tenth ring

There are nine trace rings in this tree and all of them **wrap**, so the probe
issued once at startup is gone by the time anyone looks — that is what cost an
earlier investigation hours when entry 2 was overwritten by 81 identical later
ones. `x86_probe` is keyed by `(kind, id, AH, sub)` and **never evicts**: a
title making 400 calls to twelve services occupies twelve slots, so a longer run
only ever adds. When the 128 entries are all claimed it **saturates and counts
what it refused**, and the report says so rather than presenting a subset as the
whole conversation.

The existing rings are untouched and still the right tool for **order**, which
an accumulator throws away. Reach for `probe.py --dos-log` / `--io-log` when the
question is "in what sequence"; reach for this when it is "what, and did we
answer it".

Two caveats the report states itself: **INT 15h is not dispatched at all**, so a
guest calling it never reaches the accumulator and its absence from a report is
not evidence nothing called it (check `x86_int_vec_log`). And the per-port
"distinct values" figure is a fold of the low 5 bits — enough to separate a
polled status register from a programmed register file, which is the question a
port row raises, and not more than that.

### `watch_memory.py` — catch whatever is writing to an address

```bash
# who is scribbling on the relocated stub at guest linear 0x2E920?
python3 scripts/watch_memory.py --retrogo --wait-magic \
    --addr guest:0x2E920 --length 4
python3 scripts/watch_memory.py --retrogo --wait-magic --addr core_ready --hits 3 \
    --peek 0x24058cd0:224
```

`probe.py` samples; this catches. A corrupting write is one instruction among
millions and polling never sees it, so this sets a real data watchpoint and
reports the ARM PC at the stop resolved against the ELF — inside the
interpreter that PC is the handler, which names the guest instruction class.
`--addr` takes a raw `0x…`, a symbol, `*SYM+OFF` to dereference a pointer, or
`guest:0xLIN` for a guest LINEAR address — which is what guest RAM needs.
`*guest_ram+OFF` is **refused**, not served: see below. `--peek` hexdumps on every hit, which turns a
watchpoint on a completion flag into a clean snapshot of what a load produced.

Caveat: a stop can repeat on the same access without advancing. Treat repeated
identical hits as one event, not as several.

### `trace_diff.py` — bisect our trace against a reference, automatically

```bash
python3 scripts/trace_diff.py --dsk references/.../TOPBENCH.dsk --args=-l --bisect
python3 scripts/trace_diff.py --dsk ... --args=-l --shift 0 --skip 43648
python3 scripts/trace_diff.py --dsk ... --args=-l --shift 0 --skip 1032768 --reg CX
```

Rebuilds the core with the sampler in `cpu_exec.S` switched on (parameters in
`x86_trace.inc`, rewritten by this script and restored on exit), runs it under
gwemu, runs the identical image under Unicorn, and reports the first sample
where the two disagree. `--bisect` walks 16384 → 256 → 1 by itself.

It exists as a script because every part of it is a trap, and each one cost
real time here:

- A failed build followed by `&&`-chained steps leaves a **stale image** on the
  SD card and the probe reports the previous build's trace as the new one.
- The sampler must **saturate, not wrap**. A ring spans only its own length and
  silently shows the end of the run wherever you point it.
- A long `REP` makes thousands of consecutive EIP samples identical, so an
  index-by-index diff reports a match where there is only coincidence. Confirm
  with `--reg CX`.
- `--reg FL` samples R11, not `cpu_state.eflags`: EFLAGS is cached in a register
  and only written back on exit. AF is excluded from the comparison because x86
  leaves it undefined after shifts, rotates, logic and MUL/DIV.
- Unicorn fires its code hook once per `REP` element **and once more** with CX
  already zero. `refx86` drops that phantom firing; without it every index after
  a REP is off by one and it reads as "the target left the loop early".
- Anything we execute that the reference cannot know about — HLE stub
  instructions after an interrupt — shifts our index against theirs.
  `x86_cs_log_idx` says whether any interrupt was taken at all -- but only in
  a build with `X86_CS_LOG=1`; see below.

### `ref_trace.py` — run the same guest under a reference x86

```bash
python3 scripts/ref_trace.py --dsk references/.../TOPBENCH.dsk --args=-l --summary
python3 scripts/ref_trace.py --dsk ... --args=-l --every 16384 --count 64
python3 scripts/ref_trace.py --dsk ... --args=-l --skip 43520 --every 1 --count 64
```

When a guest walks off into memory it does not own, the question is always
"which instruction first behaved differently from a real 8086". This runs the
identical image under Unicorn (`pip install unicorn`) and prints a trace in the
same shape, so the two can be diffed. The image is built by
`scripts/common/dosimage.py`, which mirrors `x86_dos_exec` — same PSP, same
load segment, same relocations, same entry registers; starting anywhere else
would make the comparison meaningless. It stops at the first software
interrupt, which for a self-unpacking program is the finish line.

**Aligning the two traces takes care.** Our interpreter re-enters its fetch loop
once per `REP` element, and so does Unicorn's hook, but instructions executed in
the HLE stub page are ours alone and desynchronise the counters. A long `REP`
also makes thousands of consecutive samples identical, so a naive index-by-index
diff reports a match where there is only coincidence — confirm a suspected
divergence by comparing a register (CX, DX) over the same window, not just EIP.

### `screenshot.py` — capture what the panel is being sent

```bash
python3 scripts/screenshot.py --wait 20 --out build/artifacts/topbench.png
python3 scripts/screenshot.py --timeline timelines/run-to-completion.tl \
    --wait 48 --pan 16 --text --out build/artifacts/topbench.png
python3 scripts/screenshot.py --target hardware --wait 20
```

**It follows `ext_clut` when a 256-colour mode is up, and that is not
cosmetic.** `lcd_set_clut_ext()` parks a CALLER-OWNED palette in `ext_clut` and
`clut_push()` programs the LTDC from it, bypassing the darkened-twin
`active_clut` cache entirely — so reading `active_clut` (which is what this tool
did, and only 32 entries of it) recovers the PREVIOUS 32-entry CGA table and
repaints every mode-13h frame in unrelated colours over perfectly correct
geometry. That is indistinguishable from a palette defect in the core, and
`build/artifacts/wolf3d_after_1ah.png` is a capture of it: the DAC, the
256-entry CLUT and the LTDC were all correct in that run.
**The firmware's own readback has the same gap and is NOT fixed here:**
`lcd_get_clut_rgb565()` (`gw_lcd.c:433`) and `lcd_convert_lut8_to_rgb565()`'s
`clut == NULL` arm both read `active_clut`, so retro-go's on-device screenshots
and save-state thumbnails of a 256-colour guest are miscoloured on silicon even
though the panel is right.

`--text` dumps the 80x25 character buffer alongside the image, decoded as
**CP437**; judging small glyphs by eye is how a `Runtime error 203` went unread
through two captures, and printing only 0x20-0x7E hid every box-drawing and
shading code behind a `.`, which made a correctly drawn full-screen UI read as
four rows of stray letters on an empty screen.
`--text-aa 0|1` picks the renderer before capturing, so the SAME screen can be
grabbed both ways from one run rather than from two builds that also differ in
where the guest got to. `--pairs` counts the DISTINCT (fg,bg) attribute pairs a
text screen uses — the number that decides whether anti-aliasing fits, since
each pair costs a contiguous block of four CLUT entries.

`--attr` dumps the ATTRIBUTE byte of every cell (two hex nibbles, bg then fg)
with a census of the distinct attributes, and it is not optional on any screen
where the SHADING carries the meaning. Commander Keen's startup screen lists
every video mode, input device and sound card it knows about and marks the ones
it DETECTED purely by brightening the attribute — so a `--text` dump of a
machine that detected nothing is byte-identical to one that detected
everything, and a whole session was spent believing the first was the second.

`--pan N` pins the text window's leftmost column. It is now a no-op: the text
cell is 4 px wide, so all 80 columns land on the 320 px panel and there is
nothing off-screen to pan to. It is kept for a mode wider than the panel.

gwemu's own headless screendump comes back black, so it proves nothing. The
LTDC registers ARE modelled: this reads Layer 1's framebuffer address, pitch
and line count, reads that memory and the firmware's CLUT, and rebuilds the
frame. It also prints a coarse text map of the result, so a black or garbage
capture is obvious without opening the file. `tests/test_screen.py` does this
for one expected image; this is the general form.

### `dma2d_ab.py` — A/B the Chrom-ART offload of the chained 13h blit

```bash
python3 scripts/dma2d_ab.py                     # both arms, tier 2
python3 scripts/dma2d_ab.py --keep-dsk          # keep build/CHAIN13.dsk
python3 scripts/dma2d_ab.py --insns 3000000     # earlier capture point
```

Builds the core twice (`X86_VIDEO_DMA2D=0` and `=1`), boots each under gwemu
against a **synthetic guest it generates itself** — a 27-byte MZ .EXE on a
FAT12 floppy that sets mode 13h, fills the aperture with `i & 0xFF` and
repaints forever — captures a frame from each and requires: the two arms
identical, the picture obeying the pattern's own `(x,y) == (x-64,y+1)`
identity (palette-independent, so it pins the row pitch without knowing the
DAC), and `x86_dma2d_blits > 0` with `x86_dma2d_fallback == 0`, so an A/B
cannot silently compare the fallback with itself.

It exists because nothing else can reach this code: the offload covers
**chained 13h only** and no title in the tree holds such a screen. **It does
not pass yet** — the guest halts at `guest_exit_reason=4` before the mode set
takes; the script's docstring records the measured state and the next
question. Timing is not settleable below tier 3 either (gwemu runs the
transfer synchronously), and the script prints the hardware recipe.

### `golden_screens.py` — the golden-screenshot GATE

```bash
python3 scripts/golden_screens.py list                       # what is covered
python3 scripts/golden_screens.py check                      # THE GATE
python3 scripts/golden_screens.py check --title WOLF3D
python3 scripts/golden_screens.py repeat --title ALLEYCAT --runs 3
python3 scripts/golden_screens.py capture --title KEEN4      # candidate only
python3 scripts/golden_screens.py capture --title KEEN4 --bless \
    --verified-by doug --shows "Keen 4 intro, EGA 0Dh, full height"
```

`screenshot.py` captures a picture; this **compares** it. Two "the guest runs
happily and the screen is garbage" regressions shipped in one session and every
tier-1 test passed through both, because nothing in the suite could see a
picture. Each title is booted from its own freshly-built SD image, run to a
fixed `guest_insns_total`, captured from the LTDC, and diffed against
`tests/goldens/<TITLE>.png` pixel by pixel. Failures print **where**: a
bounding box, a 16x12 block map and the worst pixel with both colours. A count
alone cannot tell a palette off by one level from six thumbnails of noise.

Four rules it is built around, each closing a way this gate could be worthless:

1. **A golden captured from a BROKEN build blesses the bug.** So an image is
   never a golden on its own: it needs a `<TITLE>.json` sidecar naming who
   verified it by eye, when, and what they saw. `check` refuses a golden whose
   sidecar is missing, incomplete, `unverified` or `known_bad`. The sha256 in
   the sidecar is checked on load, so an image swapped underneath its
   attestation is an error rather than a new baseline.
2. **Re-blessing is visible.** `--bless` over an existing golden requires
   `--because`, prints the pixel difference against the image it replaces, and
   pushes the old attestation onto a `history` list. There is no silent
   overwrite.
3. **`known_bad` is a defect WITH A PICTURE.** It fails the gate — an
   outstanding defect is not a pass — but it is summarised separately from a
   regression, so a real regression cannot hide behind a bug someone already
   knew about. `KEEN4.png` is one: the half-height EGA 0Dh intro.
4. **The capture point is an instruction count, not a sleep.** `guest_insns_total`
   is polled with adaptive sleeps so the overshoot is a fraction of a guest
   frame; a flat 0.2 s poll overshoots by a MILLION instructions and lands on a
   different frame every run. Prove a new capture point with `repeat` before
   blessing it — "it looked the same" is not the proof, two identical frames are.

**A mask is the last resort and never a wider tolerance.** Some elements are
driven by guest TIME, not by the instruction stream — Alley Cat's cat walks off
the PIT tick and its copyright line alternates between two equally correct
strings — and no instruction-count capture point can freeze those. The manifest
may declare `mask` rectangles, but only with a written `mask_reason` (enforced),
and masked pixels are still counted and printed (`m` in the block map). A
tolerance wide enough to absorb a moving sprite is wide enough to absorb the
corruption the gate exists to catch.

`tests/test_goldens.py` is the tier-1 half: it drives the comparator and the
attestation rules against synthetic images with **negative controls for every
failure this project has actually shipped** — a one-pixel shift, a rectangle of
noise in an otherwise perfect screen, a uniform level shift, a half-height
render, a defect one pixel outside a mask, and a golden with no attestation. It
needs no target and runs in under a second, and it also re-checks the shipped
goldens' hashes.

### `profile_opcodes.py` — which x86 opcodes burn the cycles

```bash
make clean && make sdcard X86_PROFILE=1                   # instrumented core
python3 scripts/profile_opcodes.py --retrogo --settle 20 --window 25
python3 scripts/profile_opcodes.py --retrogo --target hardware \
    --settle 20 --window 25 --json build/artifacts/profile_topbench.json
python3 scripts/profile_opcodes.py --retrogo --sort mean   # expensive handlers
python3 scripts/profile_opcodes.py --retrogo --sort count  # hot but cheap ones
python3 scripts/profile_opcodes.py --bench --window 60     # tier 1 self-check
```

Reads DWT->CYCCNT at every dispatch and charges the delta to the PREVIOUS
opcode, into a 513-slot DTCM table (256 one-byte + 256 `0F` + one HOST slot for
time outside the interpreter). Prints count, total cycles, mean, overhead-
corrected mean and % of all cycles; `--json`/`--csv` make a profile diffable
across builds the way `tests/asm_baseline.json` already is for static counts.
Handler names come from the LIVE dispatch tables, resolved with `probe.py`'s
symbol machinery, so they cannot drift from what ran.

`--settle N` discards the first N seconds and then resets the table, so the
window is a steady state and not DOS startup. `--bench` (bare-metal only) runs
the synthetic benchmark, whose answer is known exactly — that is how the
plumbing is checked, not by trusting the cycles.

**The instrumentation is behind `X86_PROFILE=1` and OFF by default.** It sits in
the fetch/dispatch tail, the hottest and most register-starved code in the
project, and it costs ~65 cycles per dispatch on top of a ~50-cycle instruction.
Flags are not dependencies: build clean or you are profiling the previous core.

Three things the tool says in its own output, because none of them is optional:

- **A gwemu profile is not a profile.** gwemu does not model the pipeline
  (`docs/06`), so only the COUNTS mean anything there. Run it under gwemu first
  anyway — the point of that pass is to prove an instrumented dispatch tail does
  not fault, before it goes anywhere near silicon.
- **Attribution is approximate.** A delta measured between two dispatches on a
  superscalar M7 carries part of the previous instruction and of the cache/AXI
  state this one inherited. Rank hogs with it; do not quote a per-opcode cycle
  count as truth.
- **Measurement overhead is calibrated on the target** by `x86_profile_calibrate`
  (an empty loop against the same macro), stored in the table, and printed as a
  separate corrected column. Nothing on the target subtracts it, and there is no
  fudge factor. It is a lower bound — the calibration loop predicts perfectly
  and stays in cache.

A slot is closed by the NEXT dispatch, so the last instruction before each exit
from the interpreter is uncounted (one per interp call). Profiling also slows
the emulator, so anything timing-dependent in the guest is perturbed; that is
noted, not corrected.

##### `--gate` — the check that says WHICH WORKLOAD was profiled

```bash
python3 scripts/profile_opcodes.py --retrogo --target hardware --no-flash \
    --hard-reset --timeline timelines/wolf3d-hw.tl --settle 33 --window 20 \
    --gate x86_modey_presents --gate guest_insns_total \
    --json build/artifacts/profile_wolf3d_play.json

# the CONTROL, same core and clock, no raycaster
python3 scripts/profile_opcodes.py --retrogo --target hardware --no-flash \
    --hard-reset --timeline timelines/wolf3d-menu-hw.tl --settle 12 --window 20 \
    --gate x86_modey_presents=const --gate guest_insns_total \
    --json build/artifacts/profile_wolf3d_menu.json
```

`--gate SYM[=climb|const]` reads named counters at the two ends of the
measurement window, prints before/after/delta, and **exits non-zero if any one
moved the wrong way**. Repeatable and comma-separated; `climb` is the default.

**A profile looks equally confident whether the guest was raycasting or sitting
on a menu**, and two full 57-second hardware runs of Wolf3D data turned out to
be menus before anything in the tooling ever looked. `x86_modey_presents` is
the Mode Y gameplay gate — it must CLIMB — and the same symbol with `=const` is
what makes the menu control a control rather than an assumption. The gate
result is written into the JSON (`meta.gates`, `meta.gates_ok`), so a saved
profile carries its own provenance and cannot be quoted later as the wrong
workload.

##### `--compare A.json B.json` — a changed MIX or a real slowdown?

```bash
python3 scripts/profile_opcodes.py --compare menu.json play.json --top 18
```

Needs no target. Decomposes the change in **cycles per instruction** between two
saved profiles into the part explained by a changed opcode MIX (same per-opcode
cost, different shares) and the part that is a genuine per-opcode COST increase,
and prints both columns per opcode.

**The two look identical in an average and are completely different problems.**
"Gameplay runs at 95 cycles per instruction against 64 in menus" is consistent
both with an interpreter that regressed on this code and with a guest that
simply asked for more expensive instructions; only the split says which. For
WOLF3D the answer was **90% mix, 10% cost** — see [`docs/09`](docs/09_frame_pacing_and_clocks.md) §8.

Requires both profiles to come from the same core and the same clock. Comparing
across builds compares the builds.

### `gen_opcode_coverage.py` — regenerate `docs/07` FROM SOURCE

`docs/07_opcode_coverage.md` was hand-written and rotted twice: it listed the
`0F 8x` near-`Jcc` and `E0`-`E3` address-size defects as open 249 commits after
`7f4c12a` fixed them, and called `60`/`61`/`68`/`84`/`85`, the BCD group and
INS/OUTS broken or missing when they were implemented. A sprint was planned
against those rows. The per-opcode tables are now generated and must not be
hand-edited.

```bash
python3 scripts/gen_opcode_coverage.py              # print the tables
python3 scripts/gen_opcode_coverage.py --write      # rewrite the block in docs/07
python3 scripts/gen_opcode_coverage.py --check      # exit 1 if docs/07 is stale
python3 scripts/gen_opcode_coverage.py --check-prose  # prose vs the table only
python3 scripts/gen_opcode_coverage.py --json build/artifacts/coverage.json
```

It parses `opcode_table.S`, `opcode_0f_table.S` and every `OP`/`OP_AGNOSTIC`
handler body in `cpu_exec_handlers.S` / `cpu_exec_handlers_0f.S`, following one
handler's branches into shared cores so an ALU handler is not falsely reported
as writing no flags. It emits, per slot: reached vs `.exit_loop`, the handler
symbol, dual vs AGN, the reachable flag macros, and mechanical `REVIEW:`
markers (no width primitive, open-coded stack, ModRM decoder called by symbol).
It also extracts the `CMP`/`BEQ` group dispatch chains, which is what decides
whether a `/reg` is reachable at all.

**It never claims a handler is correct.** Semantic questions — is this flag
computation right, is the segment base applied in the right place — are not
derivable by grep and are reported as UNKNOWN. `--check` belongs in CI: it is
the thing that stops the document rotting a third time.

Only the block between the `BEGIN GENERATED` / `END GENERATED` markers is
rewritten; the hand-written analysis after it is preserved.

#### `--check-prose` — the hand-written half, because the rot came back there

Generating the table fixed only half the problem. Within the same session the
PROSE around it went stale again: paragraphs twenty lines below a correct,
green generated table still called the BCD adjusts, `6C`-`6F`, `62`, `9B`,
`F1`, several `0F` slots, the bit set and the double shifts unimplemented.
`--check` did not notice, because it only diffed the generated block.

`--check-prose` (run automatically by `--check`, and therefore by
`budget_gate.py`) compares hand-written REACHABILITY claims against the
generated status column, in both directions:

| prose says | table says | result |
|---|---|---|
| "unimplemented" / "missing" / "falls to `.exit_loop`" | reached | **FAIL** |
| "implemented" / "reached" | `.exit_loop` | **FAIL** |

It checks `docs/07_opcode_coverage.md`, `STATUS.md`, `PLAN.md` and `CLAUDE.md`
(`--prose-file PATH`, repeatable, overrides that list; `--skip-prose` turns the
arm off inside `--check`; `-v` lists what it skipped and why).

**It judges reachability and NOTHING else.** "DEFECT", "broken", "silently
32-bit", "no flags" are deliberately not claim words: a reached handler can be
all of those, the status column cannot refute it, and firing on them would
flag the whole (correct) defect taxonomy table and get the checker switched
off. Its unit is one table row or one sentence, so a claim word must sit next
to its opcode; only backticked opcode-shaped tokens count; and two-letter flag
names (`CF`, `OF`, `AF`, …) are never read as opcodes, which costs recall on
those few slots and removes a large class of false positives.

Prose that is deliberately HISTORICAL — "formerly listed as unimplemented",
the superseded list docs/07 keeps by name — is what the fence is for:

```markdown
<!-- coverage-check: historical -->
... text whose status claims are about the past ...
<!-- coverage-check: end -->
```

`<!-- coverage-check: ignore -->` exempts a single line. Sentences that already
read as history ("formerly", "no longer", "now implemented", "superseded",
"was") are skipped automatically; the fence is for when that is not enough.

### `opcode_demand.py` — what does a title's binary actually CONTAIN

Replaces serial discovery. Instead of running a title, hitting
`guest_exit_reason=4`, reading `exit_info`, implementing that one opcode and
running again — one round trip per opcode, and only ever the first blocker —
this disassembles the shipped binaries out of the `.dsk` and ranks every opcode
and group `/reg` against what the core has.

```bash
# Epic Pinball: the launcher plus the 13 table binaries, 16-bit real mode
python3 scripts/opcode_demand.py --dsk EPIC_PINBALL --bits 16 --problems

# a DOS extender's load image is 32-bit; mixing the widths is the main way
# this scan goes wrong
python3 scripts/opcode_demand.py --dsk STAR_WARS_DARKFORCES \
    --file DF/DOS4GW.EXE --bits 32 --raw --pmode
```

Status comes from `gen_opcode_coverage.py`, never from docs/07's prose.

Three things it does that a naive `objdump` sweep does not, each of which
changed the answer materially:

* **It starts at the entry point.** Epic Pinball's `EP*.EXE` are ~250 KB whose
  first ~200 KB is a data blob beginning with the literal string `"STACK   "`;
  only the last ~40 KB at `CS:IP` is code. Sweeping from byte 0 reports 19,000
  `DAS` because `0x2F` is a common data byte. `--whole-image` opts out.
* **It subtracts a null model.** Every count is also taken over a SHUFFLED copy
  of the same bytes — the count the sweep would report if the region were pure
  data with that byte frequency. `enrich = count/noise` separates real code
  (`enrich >= 3`) from data noise (`enrich < 2`, marked `weak`).
  `--no-null-model` skips it.
* **`--pmode` adds a phase-independent byte-level privilege census** over the
  whole file — every `0F 00`/`0F 01` group member, `LAR`/`LSL`/`CLTS`,
  `MOV CRn`/`DRn` — each hit re-disassembled at its offset in both widths. For
  "does this extender ever change ring", one genuine `LTR` is decisive and a
  phase-dependent sweep can miss it.

Counts are occurrences in the IMAGE, never execution counts: it ranks what a
title COULD need. `profile_opcodes.py` ranks what it DOES need. Neither
substitutes for the other.

#### `--library` — the whole roms tree in one pass

```bash
python3 scripts/opcode_demand.py --library \
    --report build/artifacts/opcode_demand_library.md \
    --json   build/artifacts/opcode_demand_library.json

python3 scripts/opcode_demand.py --library --title WOLF3D --title KEEN4
```

Scans every `.dsk`, writes a per-title gap table plus a cross-title ranking of
missing opcodes and a list of titles with no strongly-evidenced gap. Takes
about five minutes for the full tree. Four things it does that the per-title
mode does not, each of which changed an answer:

* **It classifies the executable format and takes the decode width from it.**
  The tree is not all real-mode MZ: `SCRIPTIN.EXE` and `JAZZ2.EXE` are Win32
  PE, Tyrian's installer and all of WIN311 are Win16 NE, `WIN386.EXE` is a W3
  bundle, and Dark Forces and Mortal Kombat are DOS/4GW-bound LINEAR
  EXECUTABLES. Non-DOS files are excluded with a printed reason; an LE's code
  comes from the objects the header FLAGS as executable, at the width its BIG
  bit declares. `--from-entry` restores the old CS:IP rule.
* **It validates an LE header rather than searching for the signature.**
  `MK1.EXE`'s stub has no usable `e_lfanew`, so the payload is found by
  search — but `PRINCE.EXE` contains the bytes `LE\0\0` too, at an offset
  whose page size reads `0x414f4259`. Only a candidate with a 4096-byte page
  size, a sane object count and an in-file object table is accepted.
* **It keeps only the 4 KB windows that DECODE LIKE CODE** (`code_windows()`),
  scored by the fraction of instructions using common opcodes. Data blobs
  score 0.07, real code 0.32-0.94. This removed every phantom in one step:
  Epic Pinball's `0F 0F` PI2FD went from 217 occurrences to 4. `WOLF3D.EXE`
  is rejected entirely and correctly — it is LZEXE-packed, so there is no x86
  in the file to count. `--no-code-filter` opts out.
* **It reports `rel`, not raw `enrich`.** Shuffling preserves single bytes but
  destroys ordered pairs, so raw enrichment is not comparable across opcode
  lengths: `8B` MOV scores 1.27 and `0F 0F` scores 217 in the same file. `rel`
  divides by the median enrichment of the same opcode LENGTH. **Read `rel`.**

STRONG requires all three of count >= 20, `ctx` >= 14 and `rel` >= 4. `ctx` is
the median count of distinct byte values in the 24 bytes around each hit; it
exists because the shuffle control is blind to byte CLUSTERING, which is how
`0F 0D` PREFETCHW once scored 5.3x at the edge of a run of `0D` bytes.

`x87_idioms()` answers the FPU question without decoding at all, by searching
for fixed byte sequences (`DF E0 9E` = `FNSTSW AX; SAHF`, `DB E3` = `FNINIT`)
that are x87 and nothing else. Needed because both statistical controls have
opposite blind spots on x87 and both are wrong about it.

### `profile_all.py` — ONE command, one workload, one full report

```bash
python3 scripts/profile_all.py --workload alleycat
python3 scripts/profile_all.py --workload topbench-menu --window 15
python3 scripts/profile_all.py --workload alleycat --passes zones   # quick
python3 scripts/profile_all.py --rom .../GAME.dsk --name mygame
python3 scripts/profile_all.py --compare before.json after.json
```

Builds the cores, boots a named workload, settles it, opens a window, collects
**everything**, and writes ONE report to `build/artifacts/profile_<workload>_
<target>.md` plus the same content as JSON so runs diff across builds. The
report LEADS WITH THE ANSWER — a "where the time went" paragraph — and puts the
tables under it.

It does not reimplement any instrument. It drives the ones that exist
(`x86_zones.h`, `x86_profile.h`, `probe.py`'s counter reads, the frame
histogram, the link's headroom numbers) and adds the thing that was missing:
**how one frame's wall time divides**.

**Several passes, not one, and that is deliberate.** `X86_PROFILE` costs ~65
cycles at every dispatch — it more than doubles guest execution, so a zone
breakdown taken from that build would say "guest execution is everything",
which is true of the instrumented core and false of the real one. So each
instrument gets its own clean build and the report says which pass every number
came from. Take the wall-clock SHARES from the zone pass and the opcode RANKING
from the profile pass. `--passes zones` skips the second build when only the
breakdown is wanted.

Presets pair a `.dsk` with the timeline and settle time that agree with it:
`alleycat` (graphics + speaker steady state), `alleycat-boot` (DOS startup and
the unpacker — disk-heavy, not a steady state), `topbench-menu` (CGA text, a
database read, an INT 16h poll loop), `topbench-score` (`-l`, `--settle 0`
because it halts on `INSB` a few seconds in).

#### `X86_VIDEO_DMA2D=1` — Chrom-ART for the chained 13h blit (OFF, UNVERIFIED)

```
make -f Makefile.core pack X86_VIDEO_DMA2D=1
```

Not an instrument: an optimisation that **cannot be validated below tier 3**,
kept behind a flag for exactly that reason. It hands the chained mode 13h
identity copy (64,000 bytes) to DMA2D through the ABI's
`GW_DMA2D_M2M_RGB565_START`, declaring two L8 pixels as one RGB565 pixel — the
engine's M2M mode moves raw bytes and converts nothing. The case for it is that
the LCD framebuffer is mapped **strongly-ordered** by the firmware's MPU
(`references/game-and-watch-retro-go-sd/Core/Src/main.c:1358-1366`: TEX0,
non-cacheable, **non-bufferable**), so the CPU's copy is 16,000 serialised bus
round trips while the engine is a second AXI master that pays none of that. The
reasoning, the price and the tearing hazard are on `dma2d_blit_hook()` in
`src/main.c`.

Host-visible counters: `x86_dma2d_selftest` (0 not run, 1 pass, 2 START
refused, 3 bytes wrong), `x86_dma2d_blits`, `x86_dma2d_waits`,
`x86_dma2d_fallback`.

**What tier 2 can and cannot say.** gwemu models DMA2D
(`references/gwemu/hw/display/gnw_h7b0_dma2d.c`) but runs the transfer
*synchronously* (`:307`), so a missing or misplaced wait is invisible there and
would tear on silicon. Worse, **no workload in this tree holds a chained 13h
screen**: measured with the counters, WOLF3D is in Mode Y within one second and
PRINCE_OF_PERSIA never leaves text, so `x86_dma2d_blits` stays 0 and an A/B of
the picture exercises only the fallback. That is why `dma2d_selftest()` exists —
one 64-byte transfer at init that proves the ABI call, the encoding, the poll
and the cache maintenance on any target. It reads 1 under gwemu.

```
python3 scripts/probe.py --retrogo --timeline timelines/start-preselected-game.tl \
    --watch x86_dma2d_selftest,x86_dma2d_blits,x86_dma2d_fallback
```

#### `X86_PGF_BENCH=1` — what a pagefile write is made of, asked of the card

```bash
make -f Makefile.core clean && make -f Makefile.core pack X86_PGF_BENCH=1
python3 scripts/deploy_device.py --no-firmware --preselect KEEN4.dsk
python3 scripts/probe.py --retrogo --target hardware --no-flash --hard-reset \
    --timeline timelines/keen4-keypress.tl --interval 5 --samples 5 \
    --watch pgfb_grow4k,pgfb_inplace4k,pgfb_inplace1k,pgfb_inplace512,\
pgfb_inplace32k,pgfb_random4k,pgfb_read4k,pgfb_inplace_late,pgfb_grow4k_late
```

`pgf_bench()` in `src/main.c` writes the same 4 KB in six different shapes
(appended past EOF, rewritten in place, one 32 KB call, 512 B × 64, scattered,
and read back) and repeats two of them at the END so warm-up cannot be mistaken
for a result. ~1 s at title load and it truncates the pagefile afterwards, so
it is a build flag and never a default. It is what proved the cost is
**rewriting recently-written sectors** (60–67 ms/4 KB) rather than appending
(12–14 ms), the call size, or the cluster-chain walk — see `docs/12` §1b.

#### `X86_ISA_IO_TICKS=0` — the AdLib probe's negative control

```bash
make -f Makefile.core clean && make -f Makefile.core pack X86_ISA_IO_TICKS=0
python3 scripts/deploy_device.py --no-firmware --preselect WOLF3D.dsk
python3 scripts/probe.py --retrogo --target hardware --hard-reset \
    --timeline timelines/wolf3d-hw.tl --interval 6 --samples 3 \
    --watch x86_opl_t1_gap,x86_opl_timer_expiry,x86_opl_writes,\
x86_io_isa_ticks,x86_sb_resets,x86_sb_cmds
```

The shipping value is 1: one ~0.84 us PIT tick charged to every FM-port access,
which is what an 8-bit ISA I/O cycle costs on a real machine **whatever the CPU
is**. `0` charges nothing, so the OPL timer measures a bus-calibrated delay loop
in instructions again. SEEN TO FAIL on hardware: `x86_opl_t1_gap` **200 -> 56**
against the 96 an 80 us timer needs, `expiry` 1 -> 0, `opl_writes` 253 -> 6,
and `x86_sb_resets`/`_cmds` **1/2 -> 0/0** — i.e. WOLF3D stops probing the
Sound Blaster entirely, which is the owner-reported symptom, reproduced on
demand. `docs/03` §4e.

#### `X86_AUDIO_TONE=1` — is ANYTHING we write ever heard?

```bash
make -f Makefile.core clean && make -f Makefile.core pack X86_AUDIO_TONE=1
python3 scripts/deploy_device.py --no-firmware --preselect WOLF3D.dsk
python3 scripts/probe.py --retrogo --target hardware --no-flash --hard-reset \
    --timeline timelines/wolf3d-hw.tl --interval 12 --samples 4 \
    --watch x86_audio_submits,x86_audio_peak_last,x86_audio_peak_max,\
x86_audio_volume,dma_counter
```

Replaces every emulated source with a 440 Hz square written straight into the
firmware's DMA half-buffer, so what reaches the speaker depends on nothing in
the SB or PC-speaker model. **It partitions the whole problem in one run**:
audible means the host chain (`odroid_audio_init` -> `audio_start_playing` ->
SAI DMA -> amplifier -> volume) is sound and any silence is the guest's; silent
means no amount of DMA-engine work can ever be heard. Never ship 1.

The four counters it exercises are in the SHIPPING core and are the cheap
version of the same question: `x86_audio_submits` (buffers handed to the DMA,
one per frame), `x86_audio_peak_last`/`_peak_max` (the loudest sample in the
newest buffer / the session), `x86_audio_volume` (the launcher's own 0..255,
which is `volume_tbl[level]` and ALREADY a fraction of full scale). Read them
with the firmware's own `dma_counter`: submits climbing with peak 0 means the
chain runs and the sources are silent; submits stuck at 0 means the frame loop
never reaches the audio phase.

`x86_audio_blocks_missed` counts DMA halves (one `dma_counter` tick per
800-sample half) played without a refill -- each one a click. The refill is
`audio_pump()` in `src/main.c`: it runs at every guest sub-slice boundary
(the frame is cut into `AUDIO_CHUNK_DIV` runs for it) and at the audio phase,
and renders only when `dma_counter` moved, so it is exactly one block per
half however long frames are. Timing, so **tier 3 only** -- under gwemu the
number means nothing. On silicon it should stay near 0 at every CPU rung;
climbing means something blocks for >16.7 ms with no boundary (a long HLE
disk read, an interpreter call over `GUEST_ASM_MS_MAX`).

#### Sound Blaster: which commands, and did the guest unmask its IRQ?

`x86_sb_cmd_hist_lo` / `_hi` are the last EIGHT DSP command bytes, newest in
the low byte of `_lo`. "`x86_sb_cmds`=2" says the guest stopped after the
probe; only this says WHICH two. WOLF3D reads `0xD140` (speaker on, time
constant) and PRINCE OF PERSIA `0xD1D14014` once it is playing.

`x86_pic_imr_now` / `_ever` / `_writes` are the 8259 mask: `_ever` is every
value written to port 0x21 ANDed together, so a ZERO bit there means that line
was unmasked at least once. This is what separates "IRQ 5 was never delivered
because the guest never unmasked it" from "...because we were too slow", and
those two have completely different fixes.

`x86_sb_irq_requests` counts DSP `F2h`/`F3h` -- an interrupt with no transfer
behind it, which is how a driver finds its vector. See `docs/03` 4e-bis.

#### `X86_DSK_OPEN_PER_CALL=1` — the `.dsk` handle A/B

Puts the read path back on `rg_storage_copy_file_range_to_ram`, which f_opens
and f_closes the image around every sector, so the held handle can be graded
against it with the same workload. MEASURED on KEEN4, hardware, at the same 616
reads / 20 backend calls: `x86_disk_us_total` **352,173 µs** open-per-call
against **291,066–291,390 µs** held — ~3.0 ms per backend call. Never ship 1.

#### `X86_PG_DONATE_RESERVE=<n>` — force the pager to evict

The shipping floor (134) is KEEN4's measured working set and the title pays
ZERO writebacks at it, which is correct and useless for measuring the pagefile.
`X86_PG_DONATE_RESERVE=90` reproduces the eviction workload: 81 writebacks in a
21 s KEEN4 run. The default lives in `x86_pager.h` and is unchanged; this only
makes it `-D`-overridable.

#### `X86_EGA_SHPROF=1` — the EGA read-shadow rebuild

```bash
make -f Makefile.core clean && make -f Makefile.core pack X86_EGA_SHPROF=1
python3 scripts/deploy_device.py --no-firmware --preselect KEEN4.dsk
python3 scripts/probe.py --retrogo --target hardware --no-flash --hard-reset \
    --timeline timelines/keen4-keypress.tl --interval 1 --samples 60 \
    --watch guest_insns_total,frames_rendered,x86_ega_shadow_rebuilds,\
x86_ega_shadow_gc_writes,x86_ega_shadow_cyc,x86_ega_shadow_worst
```

Five words of bss and one DWT bracket around `ega_shadow_rebuild()`, the 16 KB
read-shadow rebuild. `x86_ega_shadow_rebuilds` is the count,
`_gc_writes` the writes to a read-affecting GC register that changed it (so
`gc_writes - rebuilds` is what the guard saves), `_cyc`/`_hi` the 64-bit total
and `_worst` the worst single rebuild.

**IT IS NOT PART OF `X86_VPROF`, AND THAT IS THE POINT.** `X86_VPROF=1` no
longer fits: it adds ~1,080 B to RAM_EMU, `guest_ram` comes back NULL,
`core_ready` stays 0 and the frame loop runs painting nothing, with every other
counter reading a plausible zero — the same silent shape as the `X86_PROBE`
trap. Reproduced on STOCK master at tier 2 on KEEN4. So one function could not
be measured by switching the whole instrument on, and this is the five-word
version. Measured cost on silicon: one rebuild is **846,000 cycles = 3.0 ms**.

#### `X86_VPROF=1` — where the cycles go INSIDE the video path

```bash
make -f Makefile.core clean && make -f Makefile.core pack X86_VPROF=1
python3 scripts/probe.py --retrogo --target hardware --no-flash \
    --timeline timelines/wolf3d-owner.tl --watch vprof,planar \
    --dump x86_vprof_frame:4x4  --dump x86_vprof_store:4x4 \
    --dump x86_vprof_paint:4x4  --dump x86_vprof_render:4x4 \
    --dump x86_vprof_osk:4x4
```

`--watch vprof` is the group of SCALARS. The five accumulators are
`{lo, hi, n, worst}` structs, and `--watch` reads one word — so they are read
with `--dump SYM:4x4`, which is why the command above carries both.

**Neither of the two instruments above can see the Mode Y cost, and that is why
this exists.** `X86_ZONES` is per-frame, but a Mode Y title paints itself from
`x86_video_page_flip()`, which is called from **inside the interpreter** — so
two full 64,000-byte paints per present are charged to the `guest` zone and
never appear as video at all. `X86_PROFILE` charges cycles to the x86 OPCODE,
and a `stosw` into the Mode Y aperture is the same opcode as a `stosw` into
ordinary RAM; the table averages the two into one meaningless mean.

Five DWT brackets, `x86_vprof.h`, storage in `x86_vprof.c` (plain BSS, not
DTCM — the pool is the scarcest thing in the project):

| accumulator | brackets |
|---|---|
| `x86_vprof_frame` | one frame-loop iteration. **The denominator** — every share is against MEASURED cycles, never an assumed core clock, because the device is overclocked to a level the core is not told |
| `x86_vprof_store` | `x86_vga_planar_store()`, the planar write hook |
| `x86_vprof_paint` | `modey_paint_both()` — BOTH buffers, render + OSK |
| `x86_vprof_render` | `x86_video_render()` alone: in Mode Y this IS the de-interleave blit |
| `x86_vprof_osk` | `x86_osk_draw()` alone |
| `x86_vprof_cell` | inside the PL_PLANE loop: the 4-byte read-modify-write of one plane cell and nothing else. **DIFFERENTIAL USE ONLY — see the warning below** |
| `x86_vprof_reloc` | `x86_code_reloc()` — staging a granule-straddling instruction into DTCM. Not video; it is here because this is the instrument that already has 64-bit accumulators and a window control, and because the RATE was counted for a long time while the COST never was |
| `x86_vprof_seam` | `x86_seam_sync()` — the multi-byte-access straddle prologue, the other long-standing "explicitly unmeasured" suspect |

**`reloc` and `seam` exist only when `X86_VPROF=1`**, unlike the five above,
which are declared on every build so a host tool reads a truthful zero rather
than failing to resolve a symbol. These two could not follow that rule: two more
BSS accumulators shift the link, shifting address literals, and the shipping
`dos.bin` stopped being byte-identical to master by 303 scattered bytes. A
profiling flag must not change the shipping binary at all, so on a clean build
`probe.py --watch` simply fails to resolve them — which is the honest signal
that the instrument is not compiled in. The same constraint is why
`x86_seam_sync`'s bracket is an `#if X86_VPROF` WRAPPER around the untouched
original function and not a split into wrapper + body: GCC re-inlines the split
body, but it reallocates registers doing it, and that alone broke byte-identity.

Each is `{lo, hi, n, worst}` — 64-bit because CYCCNT wraps every ~15 s at
280 MHz and a 32-bit accumulator would wrap inside a measurement window.
`worst` is there for the same reason `x86_zones.h` carries one: stutter is a
tail property and a mean cannot contain it.

**`paint` is a PARENT of `render` and `osk` and they are deliberately NOT
exclusive**, unlike zones — `paint - 2*(render + osk)` is itself a wanted
number. `store` is disjoint from `paint`.

Two paint counters, because **`x86_modey_presents` was never the paint count**:
`x86_vprof_paint_flip` is the guest's own page flips (what `presents` counts)
and `x86_vprof_paint_loop` is the frame loop's fallback paint, which fires
whenever `x86_video_modey_paint_due()` runs out of idle frames and which no
counter showed before. Both call `modey_paint_both()` and both cost two blits.

**Cost, and it is not zero.** `x86_vprof_calibrate()` measures an empty bracket
pair on the target after `DWT_CTRL` is enabled, and stores it in
`x86_vprof_ovh`. **Nothing on the target subtracts it** and it is a LOWER bound
(the calibration loop stays in cache). Four of the five brackets fire tens of
times a second and are noise; `store` fires ~180,000 times a second in Mode Y
gameplay and is the one bracket that perturbs its own measurement. Grade the
perturbation rather than assuming it: quote `guest_ips_achieved` on this build
against the same workload on a clean one.

**`X86_VPROF=1` AND `X86_ZONES=1` TOGETHER DO NOT FIT, AND THE FAILURE IS
SILENT.** Both alone are fine. Together they push `__CORE_BSS_END__` past
RAM_EMU, `guest_ram` comes back NULL, `core_ready` stays 0 and the emulator
simply never starts — `guest_insns_total` reads 0 and every zone and bracket
reads a plausible zero. Take the two as SEPARATE PASSES, exactly as
`profile_all.py` already does for zones and opcodes.

**`x86_vga_planar_cls_plane1`** counts the `PL_PLANE` stores that took the
no-load path — a single aligned `STR` when the Map Mask enables all four planes,
otherwise one `STRB` per enabled plane. Against `x86_vga_planar_cls_plane` it
says what fraction of the frame avoids a read; it is 100% of `PL_PLANE` on
WOLF3D because the read was removed for every mask ([`docs/09`](docs/09_frame_pacing_and_clocks.md) §9). The
intermediate arm that took the fast path only for a single-plane mask read 35%,
which is why "single plane" was the wrong condition to key it on.

**THE `cell` BRACKET IS NOT A VALID ABSOLUTE NUMBER.** DWT->CYCCNT lives on the
private peripheral bus, which is strongly ordered: a read of it inside a
store-heavy loop drains the store buffer, so the bracket does not merely
observe the region, it serialises it. The calibrated `x86_vprof_ovh` (~12
cycles) is measured in a tight loop where the PPB accesses overlap and it
UNDERSTATES this badly. The honest figure comes from comparing the two builds:
`x86_vprof_store` mean is **234 cycles without the cell bracket and 284 with
it**, so one bracket pair costs ~42 cycles in situ, not 12. Use `cell` to
compare two arms of an A/B where the artefact is common to both — which is
exactly how plane-storage placement was settled — and never quote it as the
cost of a plane access.

`x86_vprof_reset` is the window control — the host writes 1, the target clears
every accumulator at the next **frame boundary**, so a window never opens
across a half-open bracket. Link cost measured: ITCM `__CORE_ITCM_CODE_END__`
0xC938 → 0xC978 (+64 B), `__CORE_BSS_END__` +1,432 B with `X86_ZONES=1` as
well. **A changed `-D` does not trigger a rebuild — `make -f Makefile.core
clean` first, always.**

#### The zones, and what each one contains

`X86_ZONES=1`, off by default like every other instrument. The design, the
nesting scheme and the arithmetic are in `x86_zones.h`; the short version:

| zone | contains |
|---|---|
| `guest` | `x86_cpu_interp_asm()` — emulated instructions |
| `hle` | INT 10h/16h/1Ah/21h service, **minus** the disk I/O nested under it |
| `disk` | the SD backend round trip only. FatFs's own cluster-chain walk is CPU work and stays in `hle` |
| `video` | CGA render + OSK draw + CLUT push |
| `audio` | PC-speaker render into the DMA half-buffer |
| `input` | gamepad poll, retro-go's PAUSE menu, the on-screen keyboard |
| `fwloop` | the firmware's `common_emu_frame_loop()` bookkeeping |
| `swap` | **idle**, waiting for the panel. A big number here is HEADROOM, not cost |
| `residual` | **UNACCOUNTED** — see below |

**Nesting is an explicit push/pop stack and every zone accumulates EXCLUSIVE
SELF-TIME.** While a child is open its parent accumulates nothing, so nothing
is ever counted twice and a parent's *total* cost is its own row plus its
children's. The frame is the ROOT zone, which is what makes the residual honest:
`residual` is the root's own self-time — measured frame-loop time that fell
through every named zone — not a subtraction of one total from another. It is
printed even when small, and flagged above 2%. **A large residual is the report
saying a phase exists that nothing brackets**, which is the finding this
instrument exists to produce.

Per zone the report gives total cycles, occurrences, occurrences per frame,
mean, and **the worst single occurrence** — worst-case is what stutter is made
of and a mean cannot contain one. The frame-time histogram and `frame_overruns`
come from the counters that already existed.

Measured under gwemu: the instrument costs
**~91 cycles per enter/exit pair**, calibrated on the target by
`x86_zones_calibrate` and printed in the report as both a share of frame time
and a share of WORK. It is a lower bound and says so. It is per-FRAME and
per-SERVICE-CALL, never per guest instruction — that is what keeps it two
orders of magnitude cheaper than `X86_PROFILE`.

#### What is trustworthy, per tier — and the report says so itself

| tier | counts | cycles |
|---|---|---|
| gwemu | **exact and reproducible** | **not meaningful.** No M7 pipeline model (`docs/06`) and no SD busy state (`docs/04`), so `disk` reads as nearly free and *cannot* read as anything else |
| hardware | exact | real, including stalls |

That table is printed **in the report**, not only here, so a gwemu report can
never be mistaken for a silicon one.

### `make_font.py` — regenerate `font8x8.h`, `font4x8.h`, `font4x8aa.h`

```bash
python3 scripts/make_font.py                  # rewrite all three headers
python3 scripts/make_font.py --check          # fail if any is out of date
python3 scripts/make_font.py --show 0xC9      # one glyph, all three tables
```

Assembles a full 256-glyph **CP437** table from Daniel Hepper's public-domain
`font8x8` blocks, which are indexed by Unicode, plus 42 glyphs (card suits,
arrows, the maths operators at 0xEC-0xFE) drawn in the generator. Run it only
when the font changes; the results are committed.

It then emits the two **4-pixel-wide tables the renderer draws from**. 80
columns of 8 px is 640 and the panel is 320, so an 8x8 cell could only ever
show 40 of the 80 columns — and a DOS UI is centred on the 80, so the window
sliced it in half wherever it sat. At 4 px the whole 80x25 screen is exactly
320x200.

`font4x8.h` (1 bit per pixel) is the shipping face. Its letters are
**Bm437_EverexME_5x8** from VileR's Int10h Oldschool PC Font Pack, squeezed 5→4.
That replaced an OR-condensation of the 8x8 face, which looked poor for a
reason no tuning fixes: an 8 px face encodes stem placement and counters chosen
for eight columns, and halving the width destroys all of them at once. Everex
Micro Enhancer is a real character generator that showed 80 columns in a
five-pixel cell, so it was designed narrow. Measured over the 256 glyphs, only
77 put ink in the fifth column and 48 of those are line art that is hand-drawn
anyway, so the squeeze is exact almost everywhere; where it is not, columns 3
and 4 are OR-ed, touching only a right edge.

**0xB0–0xDF is hand-drawn in the generator, not converted** (`LINEART`,
`SHADES`, `SOLIDS`). This is what decides whether a narrow font is usable: a
horizontal rule must reach both cell edges or every frame comes out dashed, and
the vertical rule must sit in the same column in every glyph or corners miss. So
the grid is fixed — vertical at column 1, single horizontal at row 4, double
horizontals at rows 3 and 5. **Double VERTICAL rules deliberately alias to
single**: two strokes with a one-pixel gap in a four-pixel cell is mush and makes
every corner ambiguous. Double horizontals survive, so a `╔═╗` frame still reads
differently from a `┌─┐` one. `tests/test_text.py` asserts both properties over
the whole range, not just samples.

`font4x8aa.h` (2 bits per pixel) is the anti-aliased table: the authentic 8x8
glyph box-filtered 2:1 keeping the intermediate level, so a stem falling between
two output pixels becomes two half-lit pixels instead of vanishing. Line art is
promoted from the hand-drawn bilevel art at full level rather than filtered —
filtering a one-pixel rule would draw every frame at half brightness. See
"Anti-aliased text" below for why it is not the default.

DOS text is CP437, so the renderer indexes the font with the whole character
byte. The tree used to carry Hepper's 128 ASCII glyphs only, which forced a
`& 0x7F` in `text_render` and turned every box-drawing rule and shading block of
a full-screen UI into an unrelated letter.

### `make_dsk.py` — a game folder to a `.dsk`

```bash
python3 scripts/make_dsk.py --src build/run/games/rw/RISKY --out RISKY.dsk \
    --autoexec RISKY --size-mb 3          # hard disk: the guest sees C:
python3 scripts/make_dsk.py --src build/run/games/aw/ANOTHER --out ANOTHER.dsk \
    --autoexec ANOTHER                    # fits a floppy: 1.44 MB, the guest sees A:
```

Up to 2.88 MB it writes a plain floppy; larger (or with `--size-mb`) a hard
disk shaped like MS-DOS 5 left one: an MBR with one partition at LBA 63,
FAT12/FAT16 as mformat picks, the partition type read back from the boot
sector. `--autoexec` writes a one-line `AUTOEXEC.BAT`. The guest is told `C:`
for a hard-disk image and `A:` for a floppy (INT 21h AH=19h), so a title that
refuses to run from floppy (Risky Woods) needs `--size-mb`. Needs mtools.

### `lucasarts_unpack.py` / `so3_to_sou.py` — LucasArts install sets

```bash
python3 -I scripts/lucasarts_unpack.py --out build/run/g4/ext \
    build/run/g4/raw/PCVIQA_{A,B,C,D,E}.ND4      # parts in disk order
python3 -I scripts/so3_to_sou.py --src .../monster.so3 --out MONSTER.SOU
```

`lucasarts_unpack.py` reads a floppy set's `LFG!` parts (the files INSTALL.EXE
would unpack across disk swaps) straight into a folder for `make_dsk.py`; pure
Python (a port of the public-domain blast.c), each file's size checked.
`so3_to_sou.py` turns a ScummVM-compressed `monster.so3` back into the
MONSTER.SOU a talkie DOS interpreter reads, every line at its original offset;
needs ffmpeg, and is useless with a floppy EXE, which never opens the file.

### `sdcard.py` — compose the SD card

`create_image()` formats the partition with plain `mformat -F` and, only if that
fails (mtools 4.0.17: "Unknown geometry", then "Too few clusters" for FAT32 at
127 MB), retries with the partition's sector count, a 64x32 geometry and one
sector per cluster.

```bash
python3 scripts/sdcard.py --image sdcard_retrogo.img   # gwemu image
python3 scripts/sdcard.py --mount /media/you/RETROGO   # card in a reader
python3 scripts/sdcard.py --probe                      # device, over the probe
```

Same content, three destinations. Lays down `sd_content` (fonts/lang/bios),
`/cores/<name>.bin`, `/roms/<dir>/<rom>.dsk` + sidecars (a title's `.cfg` goes
to `/cores/dos386/`, where the core reads it), and a `/CONFIG` that
preselects the ROM and disables idle standby.

### `verify_placement.py` — check the target matches the build

```bash
python3 scripts/verify_placement.py                    # symbol regions only
python3 scripts/verify_placement.py --retrogo --wait-magic --section .core_itcm
```

Confirms symbols landed in the memory they were designed for, and byte-compares
a section against the ELF. Catches a hot loop linked into flash, or a core
segment that loaded stale or partially — which otherwise surfaces only as an
UNDEFINSTR HardFault far from the cause.

### `make_retrogo_config.py` — generate `/CONFIG`

```bash
python3 scripts/make_retrogo_config.py --out build/CONFIG --rom dos ALLEYCAT.dsk
```

Preselects a ROM (so no launcher navigation is needed) and disables idle
standby. Called automatically by `make retrogo_media`.

### `deploy_device.py` — update the real device

```bash
python3 scripts/deploy_device.py                  # firmware + core
python3 scripts/deploy_device.py --no-firmware    # core only (the inner loop)
```

Flashes `gw_retro_go_intflash.bin` to internal flash and pushes
`/cores/dos.bin` over the probe. **It never touches `/CONFIG`, `/roms` or
`sd_content`** — which is why this exists instead of `sdcard.py --probe`:
compose rewrites `/CONFIG`, silently resetting the overclock level and
preselected title the owner set through the launcher, and it would push the
whole ROM library over SWD.

**Do not call `gnw.erase()` before `gnw.flash()`.** `flash()` ->
`program(erase=True)` already erases each sector as it writes. A separate
whole-bank erase is redundant and the on-target helper answers `BAD_SEGFAULT`.

Verify without disturbing what is running:

```bash
python3 scripts/probe.py --retrogo --target hardware --no-flash \
    --watch core_ready,guest_insns_total,x86_vga13_ok
```

Add `--hard-reset` to reboot into freshly flashed firmware.

### `sdcard.py` — one ROM or the whole library

```bash
# a card for INTERACTIVE use: every DOS title, no preselection
python3 scripts/sdcard.py --image build/sd-menu.img --no-config \
    --core dos.bin --all-roms references/game-and-watch-retro-go-sd/roms/dos

# populate an EXISTING card (e.g. one from a gwemu profile) instead of recreating
python3 scripts/sdcard.py --image existing.img --keep --no-config \
    --core dos.bin --all-roms references/game-and-watch-retro-go-sd/roms/dos
```

`--rom` is repeatable and `--all-roms DIR` takes every `.dsk` in a directory,
with `.cfg`/`.dosmeta`/`.jpg` sidecars. More than one ROM REFUSES to write a
`/CONFIG` rather than picking one arbitrarily — `/CONFIG` preselects exactly
one title. `--preselect NAME.dsk` names it explicitly: a card that
auto-launches that title and still carries the others, which is what the OSK's
disk menu ("A:" cap) needs to have something to list:

```bash
python3 scripts/sdcard.py --image build/run/sd-swap.img --core dos.bin \
    --rom .../ALLEYCAT.dsk --rom .../PRINCE.dsk --preselect ALLEYCAT.dsk
```

**`--no-config` DELETES an existing `/CONFIG`, it does not merely skip writing
one.** Composing onto an existing card is not composing onto a fresh one: a
`/CONFIG` left by a previous run keeps preselecting its title, so "no config"
that only skipped the write left the card still booting straight into a game.
A gwemu profile's bundled card is exactly this case.

**A gwemu profile's SD is a qcow2**, which mtools cannot touch. Convert,
compose, convert back:

```bash
P=~/.local/share/gwemu/gwemu/profiles/<profile>
cp "$P/sdcard.qcow2" "$P/sdcard.qcow2.bak"
qemu-img convert -f qcow2 -O raw "$P/sdcard.qcow2" build/run/sd.img
python3 scripts/sdcard.py --image build/run/sd.img --keep --no-config \
    --core dos.bin --all-roms references/game-and-watch-retro-go-sd/roms/dos
qemu-img convert -f raw -O qcow2 build/run/sd.img "$P/sdcard.qcow2.new"
mv "$P/sdcard.qcow2.new" "$P/sdcard.qcow2"
```

### `gwemu_run.py` — an INTERACTIVE session the harness cannot close

```bash
# boot to the retro-go menu and drive it yourself (no ROM preselected)
python3 scripts/sdcard.py --image build/sd-menu.img \
    --rom references/game-and-watch-retro-go-sd/roms/dos/KEEN4.dsk \
    --core dos.bin --no-config
python3 scripts/gwemu_run.py --sd build/sd-menu.img

# same, sampling counters every 3 s while you play
python3 scripts/gwemu_run.py --sd build/sd-menu.img \
    --watch guest_insns_total,core_ready,x86_pg_exhausted --interval 3
```

**Use this, not `probe.py --gui`, whenever a HUMAN is going to touch the
emulator.** `GwemuTarget` starts gwemu with `-S` (halted) and only the GDB
backend's `resume()` runs it, then `atexit` tears the process down when the
harness exits. That is right for a measurement — boot, sample to a
deterministic point, stop — and exactly wrong for someone sitting in front of
the window: every sampling harness kills the emulator the moment it finishes
booting, which on a slow boot is the moment it becomes useful. This script owns
the process for as long as the WINDOW is open and exits when you close it.

`--watch` is optional and off by default, because sampling halts the CPU;
leave it off if you are judging responsiveness.

**Confirm CONFIG is really absent rather than trusting `--no-config`:**

```bash
mdir -i build/sd-menu.img@@1M -/ :: | grep -i config
```

### Vendored

`make_sdcard_image.py` (partitioned SD image), `sync_from_firmware.sh`
(re-vendor `sdk/` from a firmware checkout, then update `SDK_VERSION`).

**Re-vendoring is not done until it is COMMITTED.** Agent worktrees share
`.git` but have their own working trees, so an uncommitted `sdk/include/`
leaves every agent compiling against the OLD ABI while the main checkout uses
the new one — invisible from both sides. That skew cost two agents a wrong
diagnosis of tier 2, one of them concluding the harness was broken. It also
does not announce itself: the core still BUILDS and then refuses to start,
with retro-go spinning in `show_corrupted_installation_screen()`.

## Source layout (emulator side)

| File | Role |
|---|---|
| `cpu_exec*.S`, `opcode*.S` | the Thumb-2 interpreter and its dispatch tables |
| `x86_profile.c/.h` | per-opcode DWT cycle profiler storage + calibration (`X86_PROFILE=1` only) |
| `x86_disk.c` | the block device under our FatFs, plus the .dsk sector cache: 4 x 16 KB LRU windows from the **AHB heap** (D2 SRAM -- DMA-reachable, and the only ABI pool with a real `free()`), which are the read-ahead AND the cache. Geometry is a build-time tunable (`X86_DISK_WINDOW_SECTORS` / `_COUNT`) |
| `x86_zones.c/.h` | frame-loop PHASE accounting (`X86_ZONES=1` only): a push/pop zone stack over the emulator loop, exclusive self-time per zone, and the root zone as an honest unaccounted residual. The instrumentation points are in `src/main.c` |
| `x86_disk.c` | the block device under our FatFs, plus the .dsk sector cache: 4 x 16 KB LRU windows from the bonus pool, which are the read-ahead AND the cache |
| `x86_probe.c/.h` | the probe ACCUMULATOR (`X86_PROBE=1` only): every service and port the guest touched, keyed and never evicted, with the flags that say how each was answered. Hooked at exactly two funnels — `x86_hle_service()` and `x86_port_read`/`x86_port_write` — so no handler needs an annotation macro. Read it with `scripts/probe_report.py` |
| `x86_mem.c` | guest page pool, fold mapping, guest↔host copies, the A20 gate and the HMA-scoped real-mode wrap |
| `x86_arena.c/.h` | the VRAM arena -- AXI claimed at top priority and never evicted -- plus the pin state the pager's victim picker consults. Claims are `count` blocks of `block_bytes`, each block contiguous, **all or nothing**; an EGA claim is four independent 64 KB planes, which is why a claim is blocks and not bytes. Also the live-pointer ring: `x86_guest_ptr()` hands out raw host pointers and no caller releases them, so a refcount cannot work, and the ring is cleared at guest re-entry -- the one moment nothing can still hold one |
| `x86_io.c` | port dispatch, PIT ch2 + speaker, CGA registers |
| `x86_audio_wave.h` | PC-speaker square-wave generator: PIT divisor -> phase increment -> samples. `<stdint.h>` only, deliberately — that is what lets `tests/host_audio.c` build it natively. Vendored from the author's earlier DOS core |
| `x86_video.c`, `cga_blit.S` | the mode table and everything it selects: text 0/1 (8x8 cells) and 2/3/7 (4x8, all 80 columns), CGA 4/5 through `cga_blit.S`, CGA 6 decimated 2:1, **VGA 13h (320x200x256 linear) with its 256-entry DAC**, the BDA-driven per-frame mode detection, the graphics-mode glyph rasteriser, and the 32-entry CLUT layout the CGA modes share. The 13h aperture, its DAC and its 256-entry CLUT are all **caller-owned** (LCD bonus pool) -- 66 KB of new `.bss` would break the guest allocation silently. See [`docs/08_vga_mode13h.md`](docs/08_vga_mode13h.md) |
| `x86_osk.c/.h` | on-screen keyboard + the mouse UI shell: layout, navigation, sticky modifiers, echo, the GAME cycle, and the overlay painted into the letterbox bars. Ported UI (`docs/input/03`); the injection under it is our own scancode path |
| `x86_hle.c` | `0F 0B` stubs, IVT planting, service dispatch |
| `x86_bios.c` | INT 10h / 16h / 1Ah and the BIOS Data Area, including INT 09h's shift decode (the OSK's shifted cells depend on it) |
| `x86_dos.c` | INT 21h, PSP, MCB arena, MZ loader |
| `font8x8.h` | 8x8 CP437 font, public domain (Daniel Hepper); the BIOS font handed to the guest |
| `font4x8.h` | 4x8 1-bit CP437, generated — Everex letters + hand-drawn line art; what `text_render` draws |
| `font4x8aa.h` | 4x8 2-bit CP437, generated — the anti-aliased table, linked only when `X86_TEXT_AA=1` |

## Harnesses (`tests/`)

```bash
python3 tests/test_opcodes.py            # 287 cases: EFLAGS, segmentation,
                                         # 16-bit, 66/67, interrupts, speaker
python3 tests/test_alu_fuzz.py           # DIFFERENTIAL fuzz vs Unicorn, 16-bit
python3 tests/test_alu_fuzz.py --cases 20000 --seed 7
python3 tests/test_alu_fuzz.py --group grp2
python3 tests/test_alu_fuzz.py --group x87   # x87 condition codes

### `tests/test_alu_fuzz.py` — the arithmetic, graded against Unicorn

`test_opcodes.py` is hand-written and therefore only covers behaviours somebody
thought of. This generates RANDOM operands for the instructions a 16-bit program
actually lives on -- Grp2 shifts/rotates in all three count forms, Grp3
MUL/IMUL/DIV/IDIV/NEG/NOT/TEST, the ADC/SBB carry chain, INC/DEC, CBW/CWD,
AAM/AAD, SHLD/SHRD -- runs each one instruction under our core and under
Unicorn, and diffs the eight registers and the DEFINED flags.

Flags x86 leaves undefined are not compared (AF after shifts/rotates/logic and
MUL/DIV, OF after a multi-bit shift), and an opcode with no handler is reported
as a GAP (`exit_reason=UNIMPLEMENTED`, currently SHLD/SHRD) rather than graded
as a wrong answer -- otherwise one real bug drowns in forty known holes. The
seed is fixed and printed, so a failure is reproducible.

The **`x87` group** grades what `FNSTSW AX` puts in AX after a compare, which is
the whole of the x87 condition code (C0/C2/C3) and therefore the whole of the
branch a title takes on it. A case is several instructions with operands in
memory, so `Case` carries a `mem` dict planted on both sides and a `count` of
instructions to run; every other group keeps `count == 1` and no memory.
**Its operands are exact integers and powers of two on purpose**: Unicorn is
QEMU and QEMU's x87 is 80-bit extended, ours is 64-bit double
([`docs/07`](docs/07_opcode_coverage.md), "THE PRECISION DECISION"). The two
widths agree bit for bit on any exactly-representable value, so the restriction
is what makes this a real differential instead of a flaky one -- a last-ULP
disagreement is EXPECTED and grading it would only teach the suite to ignore
failures.

**It found the Wolfenstein 3-D defect four hand-written INC/DEC cases had
walked past**: `INC r16`/`DEC r16` (0x40-0x4F) took SF, ZF and OF off bit 31 of
an un-normalized 32-bit add. It also caught `TEST Ev,Iv` at 16 bits, AAM/AAD
setting no flags at all, and ROR/RCR computing OF as `MSB ^ MSB` (identically
zero). Needs `pip install unicorn`; it SKIPs cleanly without it.

python3 tests/test_benchmark.py
python3 tests/test_game.py
python3 tests/test_dos.py                # MCB arena, walked from the host, plus
python3 tests/test_dos.py --negative     # 13 controls: rollback, largest,
                                         # coalesce, owner, the three EXEC
                                         # ones -- pspparent (the child's
                                         # PSP:0016 link zeroed), reloc (the MZ
                                         # relocation table not applied) and
                                         # resume (the parent's registers not
                                         # restored) -- and the two OVERLAY
                                         # ones: ovlfactor (AL=03h relocates by
                                         # the LOAD SEGMENT instead of the
                                         # caller's relocation factor) and
                                         # ovlseg (the image is placed one
                                         # paragraph off the segment asked for,
                                         # so the caller's far CALL misses it).
                                         # Plus the four for AH=52h/AH=29h:
                                         # firstmcb (the SysVars -2 word is not
                                         # the first MCB -- caught only because
                                         # the GUEST walks the chain from it),
                                         # sysvarsbx (AH=52h returns the block
                                         # start, not the +0x22 SYSINITVAR
                                         # entry point), parsesi (AH=29h leaves
                                         # SI where the caller put it, which no
                                         # FCB-only check can see) and
                                         # parsestar ('*' is not expanded to
                                         # '?' across the field).
                                         # Each rebuilds
                                         # x86_dos.c with ONE fix mutated back
                                         # to its defective form and requires a
                                         # FAIL; source restored in a finally.
                                         # --negative rollback runs just one.
                                         # The arena half now drives STAGES
                                         # (run_dos_flag 2..7) so the host can
                                         # photograph the whole chain either
                                         # side of a single failed AH=4Ah and
                                         # compare it BYTE for byte, and stage
                                         # 7 runs the DOS/4GW probe loop and
                                         # reports MCB header reads per call
                                         # (x86_mcb_reads).
                                         # FILE I/O from guest code against a
                                         # FAT12 volume the test builds itself:
                                         # 3Dh/3Fh/42h/3Eh/43h/4Eh/4Fh, the
                                         # not-found path, a drive-letter
                                         # prefix and a lower-case name.
                                         # ALSO the MZ LOAD arm: a real .EXE
                                         # the test builds is run through
                                         # x86_dos_exec() and the IMAGE IS
                                         # COMPARED BYTE FOR BYTE at load_seg,
                                         # with the PSP (mem_top, env segment,
                                         # command tail) and the entry state
                                         # read out of guest memory. A one-byte
                                         # poke is the negative control. This
                                         # is the check whose absence let a
                                         # change that loaded WOLF3D.EXE as
                                         # garbage pass every tier-1 test --
                                         # "still running" is not "working".
                                         # NOTE tier 1 maps guest memory FLAT
                                         # (tests/main.c identity fold), so it
                                         # cannot see corruption that depends
                                         # on 4 KB granule alignment; that
                                         # class still needs a picture.
                                         # ALSO the EXEC arm (INT 21h AH=4Bh):
                                         # a real PARENT.EXE the test builds
                                         # EXECs a real CHILD.EXE, and every
                                         # claim is a byte one of them wrote.
                                         # The child reports its own PSP, the
                                         # PARENT LINK at PSP:0016, its
                                         # environment segment and the word its
                                         # RELOCATION entry fixed up, then exits
                                         # with a code the RESUMED parent reads
                                         # back with AH=4Dh. The parent also
                                         # reports the FLAGS, DS, SP and BP it
                                         # came back with -- the child's stack
                                         # and BP are deliberately unlike the
                                         # parent's, or a core that restores
                                         # nothing still lands on a plausible
                                         # frame. Needs X86_DOS_EXEC=1, which
                                         # is now the default EVERYWHERE, the
                                         # shipping core included.
                                         # ALSO the OVERLAY arm (AH=4Bh AL=03h):
                                         # a real HOST.EXE overlay-loads a real
                                         # OVL.EXE into a segment IT chose and
                                         # then FAR CALLs into it; the overlay
                                         # returns a sentinel computed from its
                                         # OWN relocated word, which is one byte
                                         # carrying three facts -- placed right,
                                         # fixed up right, reachable -- because
                                         # it had to execute to produce it.
                                         # THE LOAD SEGMENT AND THE RELOCATION
                                         # FACTOR ARE DELIBERATELY DIFFERENT
                                         # (the overlay's reloc entry carries a
                                         # compensating segment half), or a core
                                         # that ignored the factor would pass
                                         # and both overlay controls would be
                                         # inert. It also checks CF=0 and
                                         # AX=DX=0 on return, and that
                                         # x86_dos_execs stays 0 and the arena
                                         # is untouched -- an overlay is not a
                                         # process and allocates nothing.
python3 tests/test_sound.py              # FOUR ARMS, all driven by GUEST code.
                                         # The third is FM SYNTHESIS: guest code
                                         # programs one voice through OUT
                                         # 0x388/0x389 and the arm grades the
                                         # SAMPLES -- zero-crossing pitch AND a
                                         # host-side Goertzel against the chip's
                                         # own f = f_num * 49716 / 2^(20-block),
                                         # never against a second copy of our
                                         # synthesis -- plus the envelope at
                                         # both ends (a held note's level, and
                                         # silence 20 frames after key-off).
                                         # It also prints the DWT cost per
                                         # operator update; on gwemu read that
                                         # as an instruction count only.
                                         # Build knobs: X86_OPL_FM=0 compiles
                                         # the synthesis out (the off switch and
                                         # the cost control in one);
                                         # X86_OPL_BENCH=1 holds all nine
                                         # channels sounding and publishes
                                         # x86_opl_frame_cycles, which is how
                                         # the SILICON per-frame cost in
                                         # docs/03 4g was measured.
                                         # DETECTION: the SB DSP reset handshake
                                         # (write 1 then 0 to 0x226, poll 0x22E,
                                         # read 0xAA off 0x22A), the 0x22C
                                         # write-ready bit, E1h returning 4.05,
                                         # and the AdLib timer test (status
                                         # &0xE0  0 -> 0xC0 -> 0).
                                         # PLAYBACK: the guest writes a 256-byte
                                         # ramp, programs 8237 channel 1 at it,
                                         # sets 6,000 Hz, starts an 8-bit single-
                                         # cycle transfer and WAITS FOR IRQ 5 --
                                         # the wait that hangs a title today. It
                                         # grades terminal count, the interrupt
                                         # end to end (delivery, the DSP ack at
                                         # 0x22E, the EOI, the mixer's 0x82) and
                                         # then THE SAMPLES: 6,000 into 48,000 is
                                         # exactly 8 output samples per source
                                         # byte, so sample k must be the guest's
                                         # byte k/8. One assertion covering the
                                         # DMA walk, the decode and the rate
                                         # conversion, which no counter can fake.
                                         # The host loop is the FRAME LOOP in
                                         # miniature, because the engine has no
                                         # other clock (docs/03 §4b-2).
                                         # ADPCM: the same transport, with the
                                         # DSP command byte chosen by the HOST
                                         # so one guest program grades 0x75
                                         # (4-bit +ref), 0x74 (4-bit, NO ref),
                                         # 0x77 (2.6-bit +ref) and 0x17 (2-bit
                                         # +ref). Creative ADPCM is NOT IMA
                                         # ADPCM; the arm carries its own
                                         # reference decoder written from the
                                         # RULE (DOSBox-X's
                                         # decode_ADPCM_{2,3,4}_sample), not a
                                         # copy of x86_io.c. The source block
                                         # is a closed formula both sides
                                         # derive, so nothing crosses the debug
                                         # link but four command bytes. It
                                         # grades three LAYERS that fail
                                         # differently: not silence, not a
                                         # CONSTANT (a DC offset and real
                                         # silence are different failures --
                                         # an all-zero expectation would accept
                                         # both), and then sample for sample
                                         # against the reference decode, held
                                         # 2 apiece at 24,000 into 48,000 Hz.
                                         # x86_sb_adpcm_silenced must read 0.
python3 tests/test_sound.py --negative    # the DSP's 0xAA push
python3 tests/test_sound.py --negative-opl # the OPL timer-1 flag
python3 tests/test_sound.py --negative-tc  # the 8237's TERMINAL COUNT
python3 tests/test_sound.py --negative-irq # IRQ 5 DELIVERY
python3 tests/test_sound.py --negative-rate # the resampler step forced to 1:1
python3 tests/test_sound.py --negative-fm-env  # the FM envelope's RELEASE
python3 tests/test_sound.py --negative-fm-freq # the FM frequency multiple
python3 tests/test_sound.py --negative-f2  # the DSP's F2h IRQ REQUEST
python3 tests/test_sound.py --negative-adpcm      # 4-bit ADPCM back on the
                                         # SILENT STUB it shipped as
python3 tests/test_sound.py --negative-adpcm-ref  # the REFERENCE BYTE never
                                         # consumed as a reference. Every
                                         # counter stays green and the output
                                         # is neither silent nor constant, so
                                         # ONLY the sample-exact layer sees it
python3 tests/test_sound.py --negative-adpcm-pack # the 2.6-bit format's third
                                         # code read without its shift
python3 tests/test_sound.py --negative-adpcm-blk  # the block ticked per OUTPUT
                                         # sample instead of per DMA byte --
                                         # caught by dma_bytes, a third layer
python3 tests/test_sound.py --negative-f2-now # F2h's IMMEDIATE delivery -- the
                                         # line is still asserted, so every
                                         # counter stays green and only the
                                         # guest's ten-iteration window sees it
                                         # Each rebuilds with that one link
                                         # broken and requires a FAIL; source
                                         # restored in a finally, and the
                                         # helper asserts the target occurs
                                         # EXACTLY ONCE -- a control that
                                         # patches the wrong copy, or nothing,
                                         # passes for free. ALL ELEVEN
                                         # SEEN TO FAIL. --negative-rate and
                                         # --negative-f2-now are the two
                                         # that matter most: each leaves every
                                         # counter green, and only the SAMPLES
                                         # (or the F2h window) say so.
python3 tests/test_pit.py                # PIT channel 0 -- the SYSTEM TIMER's
                                         # divisor -- programmed by GUEST OUTs:
                                         # the real Wolf3D sequence (0x214B =
                                         # 140 Hz), lo-byte-only access keeping
                                         # the high byte, a channel 2 program
                                         # not disturbing it, and reload 0
                                         # meaning 65536. --negative rebuilds
                                         # with the port 0x40 case DELETED and
                                         # requires a FAIL, restoring the source
                                         # in a finally
python3 tests/test_keyboard.py           # BDA ring -> INT 16h, consumed by
                                         # GUEST code: AH=01 peek + its ZF on
                                         # the IRET frame, AH=00 dequeue, the
                                         # empty case and a wrap at the end of
                                         # the ring
python3 tests/test_audio.py              # PC-speaker wave generator, NATIVE --
                                         # no target at all. Compiles
                                         # tests/host_audio.c against
                                         # x86_audio_wave.h with the host cc and
                                         # MEASURES divisor -> output frequency,
                                         # the Nyquist guard and the anti-click
                                         # reset, in under a second. It also
                                         # carries the implementation it
                                         # replaced and prints both errors side
                                         # by side, because a wrong pitch is
                                         # invisible in a screenshot and
                                         # unjudgeable under gwemu
python3 tests/test_settings.py           # PER-TITLE SETTINGS, NATIVE -- no
                                         # target at all. Compiles
                                         # tests/host_settings.c against
                                         # x86_settings.c + x86_cfg.c with the
                                         # host cc: defaults, ranges, the round
                                         # trip, a corrupt file (every
                                         # CRC-protected byte flipped in turn),
                                         # a truncated one, an unknown version,
                                         # an unknown pair id CARRIED across a
                                         # downgrade, the DOSBox .cfg syntax
                                         # ([cpu]/[dos]/[autoexec]) and the
                                         # three layers. 266 checks, ~1 s.
                                         # Those two modules do NO I/O by
                                         # design, which is the whole reason
                                         # this is gradeable without a machine;
                                         # the path/open/read/close half lives
                                         # in src/main.c and is tier 2
python3 tests/test_settings.py --negative
                                         # SIX negative controls, each a build
                                         # with one guard compiled out
                                         # (X86_SETTINGS_CTL_NOCRC / _NORANGE /
                                         # _NOVERSION / _TRUNCSTR,
                                         # X86_CFG_CTL_NOBINARY / _NOTOOBIG).
                                         # Every one MUST make the harness fail
                                         # and the script reports a control that
                                         # PASSES as its own failure -- a guard
                                         # that cannot be made to fail was not
                                         # being tested. 6/6 seen to fail
python3 tests/test_pad.py                # PER-TITLE [gamepad] MAPPING and
                                         # typematic repeat, NATIVE -- no
                                         # target. Compiles tests/host_pad.c
                                         # against x86_pad.c + x86_cfg.c +
                                         # x86_settings.c: bindings, combos
                                         # (pressed in order, released in
                                         # reverse), aliases, `none`, refusals
                                         # (unknown key/button, four keys,
                                         # GAME), the repeat delay/rate and
                                         # that x86_cfg.c does not count the
                                         # section's keys as unknown, and
                                         # `mouse = on` (A/B become clicks,
                                         # directions emit no key, clicks
                                         # never repeat). 30 checks
python3 tests/test_pad.py --negative     # ONE control: X86_PAD_CTL_ALLOWGAME
                                         # lets a .cfg rebind GAME (the OSK's
                                         # only door); it MUST make the harness
                                         # fail. 1/1 seen to fail
python3 tests/test_osk.py                # the on-screen keyboard, driven by
                                         # button MASK exactly as src/main.c
                                         # drives it: the grid press, the
                                         # navigation (nearest-cap UP), B as
                                         # Backspace, a TIME-shifted '@'
                                         # (which only works if a
                                         # real Left-Shift make reaches INT 09h
                                         # first), the three-state GAME cycle,
                                         # the row-width check, and that
                                         # drawing never touches guest VRAM
python3 tests/test_video.py              # CGA mode 4 blitter, pixel-exact, plus
                                         # MODE SWITCHING driven by GUEST code
                                         # through INT 10h: 3 -> 4 -> 3 rendered
                                         # and compared at every step; AH=0Eh
                                         # rasterising an 8x8 glyph into BOTH
                                         # interleave banks in mode 4 (the Alley
                                         # Cat defect); mode 6 at 640x200x1;
                                         # mode 0 at 40 columns and the 8x8
                                         # cell; and a mode nothing renders
                                         # being REFUSED, not adopted.
                                         # It also runs the two PLANAR arms LAST
                                         # (they remap guest 0xA0000/0xB8000
                                         # onto the real aperture, which the
                                         # other arms do not expect): unchained
                                         # 13h "Mode Y" and EGA 0Dh, each 64,000
                                         # pixels exact against a host-computed
                                         # pattern, driven by GUEST code so the
                                         # store hook and the word `out dx,ax`
                                         # register writes are on the path.
                                         # --modey-only runs just the Mode Y arm.
                                         # An EGA-FIT arm runs after those, on
                                         # the layout the REAL core claims (35
                                         # pages, CGA then the 13h aperture):
                                         # mode 0Dh must lend the CGA claim out,
                                         # pull the aperture down and come back
                                         # with 19,456 plane offsets instead of
                                         # 8,192; returning to text must give the
                                         # CGA claim back BLANK; and against an
                                         # arena with no spare page 0Dh must be
                                         # refused and counted, never quietly
                                         # given a card of zero.
                                         # A THIRD Mode Y arm asks a question no
                                         # pixel test can: WHICH FRAMES ARE
                                         # PAINTED. Mode Y aliases every page
                                         # into one page of storage, so a blit
                                         # on a timer lands mid-redraw -- a
                                         # split raycast viewport, a menu
                                         # highlight showing the old item and
                                         # the new one. It drives
                                         # x86_video_modey_paint_due() directly
                                         # against four modelled guests, with
                                         # no LCD and no CPU -- including a
                                         # SLOW gameplay guest that draws in
                                         # every frame and flips every 300,
                                         # which is the case the device hits
                                         # and the 20-frame model missed.
                                         # --present-only runs just that arm
                                         # --vcard-only runs just the VIRTUAL
                                         # EGA CARD arm: an unmapped store is
                                         # discarded until a page is 30 frames
                                         # stale, then the stalest page is
                                         # recycled; written and DISPLAYED
                                         # pages are kept; the read shadow
                                         # answers above 16 KB
python3 scripts/run_planar_controls.py   # grades those arms: TWELVE negative
                                         # controls that must FAIL (chain-4
                                         # inert, a 40-offset blit stride, four
                                         # FLAT planes, the fixed 16 KB page
                                         # mask, the fixed 8,192-offset cache,
                                         # the presentation escape hatch back
                                         # on a plain TIMER, and the two EGA
                                         # read-shadow guard mutations --
                                         # -DX86_EGA_SIG_NEGCTL=1 a CONSTANT
                                         # signature, =2 the read-MODE bit
                                         # dropped -- graded by the 0Dh arm's
                                         # six read-path probes; the two
                                         # virtual-card mutations,
                                         # -DX86_EGA_VCARD_NEGCTL=1 the blit
                                         # not marking the displayed window,
                                         # =2 no staleness threshold, =3 no
                                         # lossless reuse of an all-zero page,
                                         # =4 no evacuation of a returned
                                         # second piece,
                                         # graded by --vcard-only) and one
                                         # POSITIVE control --
                                         # -DX86_MODEY_PAGE0=16384 must still
                                         # produce the same 64,000 pixels,
                                         # because the shipped default equals
                                         # the right answer and without it
                                         # nothing tests the stride learning.
                                         # One CLEAN build per arm (flags are
                                         # not dependencies), so it is slow
python3 tests/test_pmode.py              # PROTECTED MODE: the descriptor walk,
                                         # IDT gate delivery and IRET, and the
                                         # descriptor-INSPECTION instructions
                                         # (LAR, LSL, VERR, VERW, STR, LTR,
                                         # CLTS).
                                         # 164 checks, all driven by GUEST code
                                         # -- the walk is reachable from exactly
                                         # one place (L(SET_SREG)), so a
                                         # host-side call would leave the path
                                         # that reaches it, where a register-
                                         # contract violation would live,
                                         # untested. Covers LGDT/LIDT/LLDT,
                                         # the GDT/LDT fetch and its bound, the
                                         # type/DPL/present rules with the
                                         # right VECTOR and ERROR CODE for
                                         # each, base/limit/G taking effect,
                                         # null selectors (three different
                                         # behaviours for DS, SS and CS),
                                         # CR0.PE both ways, and a same-
                                         # privilege far JMP. It also asserts
                                         # x86_pmode_loads == 0 after a
                                         # REAL-MODE program: the check that
                                         # says real mode never enters the walk
python3 scripts/run_pmode_controls.py    # grades those arms. TWENTY-TWO
                                         # negative controls
                                         # (X86_PMODE_NEGCTL=1..22),
                                         # each compiling ONE rule out and each
                                         # required to make test_pmode FAIL,
                                         # plus a BASELINE arm that must pass
                                         # first -- without it every arm below
                                         # "fails" for an unrelated reason.
                                         # Not optional for this feature: the
                                         # obvious reference to copy,
                                         # references/tiny386, has NO
                                         # permission checks (its own
                                         # `// TODO` at i386.c:1097), so an
                                         # implementation and a test written
                                         # together from it would share the
                                         # same hole and stay green. THREE arms
                                         # survived on the first run and each
                                         # named a real gap in the suite --
                                         # see docs/14 section 7.
                                         # --only N runs one; --verbose shows
                                         # the arm's output. One CLEAN build
                                         # per arm (flags are not
                                         # dependencies), so it is slow
python3 tests/test_nor.py                # NOR cold-page backing and COW, driven
                                         # by GUEST code. The fake NOR is the
                                         # extflash window ITSELF, written with
                                         # plain stores -- ordinary memory under
                                         # gwemu, but a genuine 0x9xxxxxxx
                                         # address, so bit 31 is set and the
                                         # fold check traps for the same reason
                                         # it will on silicon. A .bss buffer
                                         # would never trap and would prove
                                         # nothing. 23 checks: a mismatched
                                         # snapshot REFUSED with the fold table
                                         # verifiably untouched, a read served
                                         # from NOR, a write that COWs exactly
                                         # one page, a second write to that page
                                         # that does not trap at all, and the
                                         # NOR image byte-unchanged after both
                                         # KNOWN-FAILING, PRE-EXISTING, and the
                                         # cause is now named: the arm's opening
                                         # store to 0x90000000 takes a PRECISE
                                         # BUS FAULT (CFSR BFARVALID|PRECISERR,
                                         # BFAR 0x90000000) because nothing in
                                         # this bare-metal firmware configures
                                         # OCTOSPI memory-mapped mode. The
                                         # docstring's premise -- "ordinary
                                         # memory under gwemu" -- is false
                                         # WITHOUT that setup. It dies in
                                         # Fault_Handler_C's spin, and the
                                         # handler does not clear run_nor_flag,
                                         # so the harness reports "target never
                                         # ran" instead of a fault. Fixing it
                                         # means setting OCTOSPI up in
                                         # tests/main.c; until then this arm
                                         # proves nothing
python3 tests/test_nor_wdog.py           # the WATCHDOG half of the snapshot:
                                         # x86_nor_attach()'s readback kicks the
                                         # dog once per page. 12 checks, on the
                                         # REFUSED path as well as the accepted
                                         # one (refusal walks the whole readback
                                         # before it can refuse, so it is the
                                         # LONGER stretch). Deliberately does
                                         # NOT use 0x90000000 -- it needs the
                                         # readback LOOP, not the trap, so it is
                                         # immune to test_nor.py's breakage
                                         # above. Negative control:
                                         #   make clean && \
                                         #   make X86_NOR_WDOG_NEGCTL=1 \
                                         #        test_core.elf
                                         # then re-run: 5 of 12 FAIL (SEEN)
python3 tests/test_video.py              # CGA mode 4 blitter, pixel-exact
python3 tests/test_text.py               # CGA text mode 3, pixel-exact:
                                         # CP437 at 4x8 and 8x8, attributes,
                                         # all 80 columns, scroll, line-art
                                         # continuity, the 32-entry CLUT, and
                                         # BOTH renderers with their per-frame
                                         # palette allocation
python3 tests/test_screen.py             # what the LTDC actually scans out,
                                         # reconstructed to build/artifacts/screen.png
                                         # (checks whichever mode the core is
                                         # in -- text if a guest is running)
python3 tests/test_goldens.py            # the golden gate's COMPARATOR and its
                                         # attestation rules, against synthetic
                                         # images with a negative control for
                                         # each shipped failure mode. No target,
                                         # under a second. The gate itself is
                                         # scripts/golden_screens.py check
python3 tests/test_disk_cache.py         # .dsk sector cache + read-ahead:
python3 tests/test_disk_cache.py --sweep # backend calls + hit rate across
                                         # several cache geometries. The stream
                                         # win is bought by window SIZE (4x16K
                                         # and 2x16K both do 248 reads in 9
                                         # calls; 8K needs 16, 4K needs 32);
                                         # the COUNT buys residency, which the
                                         # metadata-vs-stream arm shows by
                                         # FAILING at --window-count 2
                                         # window alignment, LRU residency
                                         # under a stream, a short window at
                                         # the end of the image, invalidation
                                         # on write, a failed fetch, and 3,000
                                         # random reads byte-compared against
                                         # the UNCACHED path
python3 tests/test_sysinfo.py            # the machine DESCRIPTION services,
                                         # from guest code: INT 11h equipment,
                                         # INT 12h conventional KB, INT 15h
                                         # AH=88h extended KB and AH=C0h, the
                                         # BDA fields that must agree with
                                         # them (0040:0010/0013/0063/0080/
                                         # 0082), and the PC-speaker feedback
                                         # bits on port 0x61. Every call
                                         # POISONS AX first and sets CF the
                                         # wrong way round, because these three
                                         # vectors used to reach the bare IRET
                                         # stub -- which does not clear a
                                         # register, so the guest read back its
                                         # own value and believed it. Measured
                                         # pre-fix: INT 12h answered 65,535 KB
                                         # of conventional memory. There is no
                                         # host-side symptom for that at all
python3 tests/test_timeline.py --offline # the .tl parser and event schedule,
                                         # no target at all
python3 tests/test_paging.py             # the guest memory ARCHITECTURE, as
                                         # opposed to one backing tier:
                                         # A20 through all three doors (the
                                         # 8042 output port, 0xDD/0xDF, and
                                         # port 0x92) with the wrap arms driven
                                         # by GUEST code through a segment
                                         # override -- the only way an address
                                         # above 0xFFFF is formed in real mode;
                                         # and the VRAM arena, whose load-
                                         # bearing arm is that a claim which
                                         # cannot be fully placed changes
                                         # NOTHING and reports its exact
                                         # deficit. Two arms (0x110000 and
                                         # 0x200000 not aliased) fail on the
                                         # pre-fix whole-space wrap.
                                         # 181 checks: 32 the HMA, 16 the
                                         # a guest that opens A20, writes the
                                         # HMA, reads it back, MASKS A20 and
                                         # must then see the ALIAS (getting
                                         # that backwards is silent
                                         # corruption, not a fault), reopens
                                         # and finds its data intact; plus a
                                         # 16-bit straddle INSIDE the window,
                                         # which is served, and one ACROSS the
                                         # 1 MB line, which is refused and
                                         # counted in x86_seam_overflow
python3 scripts/run_hma_controls.py      # grades those arms. TWO negative
                                         # controls (X86_HMA_NEGCTL=1,2), both
                                         # required to make test_paging FAIL,
                                         # plus a baseline that must pass
                                         # first. 1 leaves the HMA window
                                         # registered across a mask, so the
                                         # guest reads its HMA data where the
                                         # alias belongs; 2 refuses the HMA's
                                         # seam slack, so a straddle inside the
                                         # window is no longer served. Both
                                         # caught on the first run
python3 tests/test_xms.py                # XMS, driven by GUEST code doing the
                                         # real sequence: INT 2Fh AX=4300h ->
                                         # AX=4310h -> CALL FAR the returned
                                         # entry -> allocate -> move a pattern
                                         # up -> move it back down -> free.
                                         # The buffers STRADDLE a granule
                                         # boundary in both directions, in
                                         # pager pages primed OUT OF ORDER so
                                         # the shuffle is real, and the arm
                                         # asserts the shuffle rather than
                                         # assuming it. Three negative controls,
                                         # each seen to fail: a corrupted store
                                         # byte, the naive one-pointer memcpy,
                                         # and the guest's own refusals (odd
                                         # length, unissued handle, lock,
                                         # over-allocation). The store is a
                                         # plain RAM buffer standing in for the
                                         # pagefile -- no card, no FatFs
python3 tests/test_ems.py                # EMS, the MAPPED counterpart, driven
                                         # by GUEST code: open EMMXXXX0 with
                                         # INT 21h AH=3Dh -> INT 67h AH=41h for
                                         # the page frame -> allocate -> AH=44h
                                         # map a logical page -> WRITE 16 KB
                                         # THROUGH THE WINDOW WITH ORDINARY
                                         # STOSW -> remap -> map it back ->
                                         # read it with ordinary loads. A
                                         # logical page is FOUR granules, so
                                         # every pass crosses three seams each
                                         # way, and the arm asserts the frames
                                         # really are shuffled. THE PINNING
                                         # PROPERTY IS ASSERTED DIRECTLY: 12
                                         # frames for 24 backing pages, so the
                                         # squeeze must evict -- the MAPPED
                                         # page must survive and the allocated
                                         # but UNMAPPED one must not, which is
                                         # the control. Two negative controls,
                                         # both seen to fail: removing the pin
                                         # makes the mapped page evict and the
                                         # readback go wrong, and one flipped
                                         # byte through the window is detected.
                                         # RAM store, no card, no FatFs
python3 tests/test_seam.py --stack-seam   # GRADE the straddling PUSH/POP arm
                                         # instead of merely reporting it. It
                                         # FAILS on today's tree: PUSH_V/POP_V
                                         # are multi-byte fold sites with NO
                                         # SEAM_CHECK (zero occurrences), and
                                         # arm 1b demonstrates it -- the pushed
                                         # high byte lands in the LOW frame's
                                         # slack instead of the neighbour. The
                                         # guest's own POP reads it back, so the
                                         # round trip passes either way; the
                                         # neighbour's byte is what is lost.
                                         # docs/10 Sec.7
python3 tests/test_seam.py               # THE GRANULE SEAM. host = lin + k with
                                         # one k per 4 KB page, so two adjacent
                                         # GUEST granules are adjacent in HOST
                                         # memory only by luck -- the pager hands
                                         # frames out in FAULT order. A 16- or
                                         # 32-bit access spanning the boundary
                                         # resolves one pointer from the LOW
                                         # granule and runs off the top of that
                                         # frame, silently.
                                         # This is the ONE tier-1 harness whose
                                         # fold is deliberately NOT contiguous:
                                         # every other arm runs identity over
                                         # test_ram or a pager whose frames come
                                         # out of one extent in order, and the
                                         # defect cannot occur in either, which
                                         # is why it went unseen. The arm gives
                                         # the two granules one frame each from
                                         # extents 48 KB apart and paints the low
                                         # frame's slack 0xEE, so a broken access
                                         # returns something recognisable rather
                                         # than a plausible zero.
python3 tests/test_seam.py --negative    # the negative control, and it MUST
                                         # fail without --negative: builds
                                         # X86_SEAM_FIX=0 (no prologue, no
                                         # flush) and asserts the WRONG answers
                                         # -- reads returning 0xEE slack and a
                                         # straddling store landing in it. The
                                         # contiguous arm is asserted positively
                                         # in BOTH builds, because a fixed
                                         # mapping's fold entries compare equal
                                         # and x86_seam_sync() early-outs.
                                         # It also runs the CODE-STREAM arm
                                         # first: the dispatch threads the
                                         # opcode granule's fold constant to
                                         # every later fetch of the SAME
                                         # instruction (cpu->code_k), so an
                                         # instruction crossing the boundary
                                         # would take its ModRM, displacement or
                                         # immediate off the wrong page. Three
                                         # cases place the straddle at each of
                                         # those positions in turn, the third
                                         # with the opcode as the granule's LAST
                                         # byte, and each asserts that
                                         # x86_code_relocs moved -- a case that
                                         # passes without staging passed by
                                         # accident.
python3 tests/test_seam.py --code-negative
                                         # the code-stream negative control, and
                                         # it is a DIFFERENT build to --negative:
                                         # X86_CODE_THREAD_UNSAFE=1 keeps the
                                         # threading and removes the proof that
                                         # the instruction fits in the granule.
                                         # All three cases then read the painted
                                         # 0xEE slack as instruction bytes. Only
                                         # the executed RESULT is inverted --
                                         # whether the guest still reaches HLT is
                                         # not a property of the defect (a
                                         # corrupted immediate runs perfectly and
                                         # computes the wrong number), and
                                         # inverting the exit reason would have
                                         # made the control fire for the wrong
                                         # reason.
python3 tests/test_core_retrogo.py       # tier 2
python3 tests/test_timeline.py           # tier 2: replays a timeline through
                                         # the HARDWARE player (shadow word at
                                         # 0x30001FF4, which gwemu maps) and
                                         # times each button's arrival in
                                         # input_pad_seen. The one way to prove
                                         # the hardware input path without
                                         # silicon
```

`test_disk_cache.py` is the one harness that does NOT drive a target. It
compiles the shipping `x86_disk.c` for the HOST (`-DX86_DISK_HOST_TEST`, which
only swaps the DWT read for a variable) against a small shim that supplies a
backend reader, and drives `disk_read`/`disk_write` through ctypes. The cache
is arithmetic over a callback — alignment, replacement, boundaries — and those
bugs are found in milliseconds off-target and in ten minutes on one. It caught
two the design had not: a straddling multi-sector read cost MORE backend calls
than no cache at all, and an undersized buffer must be refused outright rather
than half-used.

All accept `--target gwemu|hardware`, `--gui`, `--audio`, `--icount`,
`--timeline`, `--physical-sdcard`, `--keep-temp`.

Each gwemu run copies the SD image and extflash (~66 MB) into `build/run/`,
which is deleted on exit. `--keep-temp` retains it for analysis. Nothing is
written to `/tmp` — it is RAM-backed on this machine.

## Tier 2 build order

```bash
python3 scripts/build_retrogo.py --clean   # firmware (separate tree)
make sdcard                                # core -> pack -> SD image
python3 tests/test_core_retrogo.py
```

`make sdcard` chains the core build, so the image can never be assembled around
a stale core.

**`gwemu_release` does NOT rebuild the emulator bank images.** Its prep block is
guarded by `if [ ! -f build/sdcard.img ]` and prints "QEMU images already exist
(skipping prep)" — so once any gwemu media exists, `qemu_bank1.bin` keeps
whatever the FIRST build put there while every later build updates
`gw_retro_go_intflash.bin` beside it. Tier 2 then boots hours-old firmware with
a successful build log above it; that is how a `REMOTE_INPUT=1` firmware came up
with no remote input in it, and it cost most of an evening. `retrogo_build.py`
now re-cuts the bank the firmware was linked for after every build (bank 1 on a
bank-2 build is the stock-firmware backup and is left alone). Same failure shape
as the stale SD image in docs/04, one layer down: **when tier 2 behaves like
code you did not write, check the mtime of `qemu_bank1.bin` against
`gw_retro_go_intflash.bin` before anything else.**

**Where the line sits:** make compiles (`Makefile` for the bare-metal harness,
`Makefile.core` for the packed core) — it owns source→binary dependency graphs.
Python composes, deploys, orchestrates and talks to targets, because those need
validation, multiple destinations and target awareness.

Launch is driven by `/CONFIG` preselection plus
`timelines/start-preselected-game.tl` (presses A twice). Do not navigate menus.

**Button names are not what they look like.** gwemu's `press start` drives
`B_START` — a button the stock Game & Watch does not have, which retro-go maps
to `ODROID_INPUT_X`, a bit the core does not read. The physical **GAME** button
is `ODROID_INPUT_START`, so `press game` is what tests the core's Return
mapping. A timeline that says `press start` exercises nothing, which is how
"START sends Return" stayed believed-working while the device disagreed.
`timelines/press-game.tl` presses TIME (N, to answer Alley Cat's joystick
prompt) and then GAME, and `tests/test_core_retrogo.py` asserts the byte
0x1C reaches the guest through `x86_kbd_read_log`.
`timelines/set-cpu-option.tl` walks the PAUSE/SET menu to the core's own
"CPU Speed" entry, changes it, then Save & Quits and relaunches — so one run
covers "the menu opens", "the change takes effect" and "it survives a restart".
It navigates by pressing UP once in the settings menu: the dialog's selection
wraps and our entry is appended last, so this keeps working if the firmware
grows an option.
`timelines/press-menu-keys.tl` and `timelines/press-game-menu.tl` drive a
guest's own MENU -- DOWN (scancode 0x50, an extended key with no ASCII) and
then GAME (Return) -- with a screenshot between each press. A keyboard chain is
only proven when the guest's PICTURE changes: counters can all look right while
the key goes to a program that ignores it. Capture with
`screenshot.py --timeline ... --wait N --text` at two different `N`.
`timelines/profile-alleycat.tl` gets Alley Cat actually RUNNING and then stops
touching it, which is what a profile window needs and what no other timeline
did. Alley Cat has **two** gates: the joystick prompt (answer N, which is
gwemu's `press select` -- `press start` sends Y and lands on "Game Control
Adapter is not present") and then a skill-level menu wanting K/H/T/A, a LETTER
**no button can send**. The on-screen keyboard is the only door: GAME opens it
on layer 0 cell 0, which is `a`, and two more GAME presses close it again.
Verified with `screenshot.py --text` at each stage. This matters because
profiling the unanswered prompt returns 56% `CMP Gv,Ev` + 44% `JZ Jb` -- an
exact measurement of a game that never started, and one that looks like a
result.
`timelines/keen4-keypress.tl` presses keys AT the guest, on several different
buttons, for a title whose startup ends in a "press any key" wait. Commander
Keen 4 sits in `IN_Ack` at guest linear `0x15C70` forever under
`start-preselected-game.tl`, which presses nothing after launching -- so every
Keen measurement before this one stopped there by construction and could say
nothing about what follows. It is proven by `x86_disk_reads` moving (618 -> 734)
and the planar store counters resuming, not by the screen, which stays black.
`timelines/run-to-completion.tl` is the same launch with a 58 s window, for a
guest doing real work whose result only appears at the end -- the one case where
the wait IS the result rather than a symptom to chase with `probe.py`.

`timelines/topbench-benchmark.tl` walks TOPBENCH's main menu to **Benchmarking**
(one DOWN, then START -- see the note below about GAME) and lets the suites run.
It is the timeline for the PIT question: the five Zen-Timer sub-timings are what
`IN 0x40` and the 8259's IRR at `0x20` decide. **It does not finish inside the
58 s cap** -- the guest is still in the CGA mode-4 VidMem suite when the window
closes, so the results screen is not reachable under gwemu and the evidence has
to come from the port table instead:

```bash
python3 scripts/probe_report.py --title topbench \
    --timeline timelines/topbench-benchmark.tl --settle 50 --skip-cost
```

Read three rows. `0x0020 OUT` last=0x0a is PZTimerOff selecting the IRR;
`0x0020 IN` must answer **0**, because a set bit 0 is "the timer overflowed" and
sends every sub-timing to 65535; `0x0040 IN` must show several distinct values,
because one value is a counter that is not counting. Measured: 1,640 IRR reads,
all 0, and 55,760 counter reads over 11 distinct low-5-bit values.

**GAME NO LONGER SENDS RETURN**, and a timeline written against that is testing
something that was deliberately removed. It is the on-screen keyboard's only
door (`docs/input/02-button-mapping.md`), so `pad_scancode[6]` is 0. Return
went to the physical START button (gwemu's `press start`) and, since
2026-10-22, to B (`press b`): the owner's default is A Space, B Return, START
Y, SELECT N, TIME Esc. Three timelines drive the keyboard:

- `timelines/osk-open.tl` -- open with GAME, move the cursor, press a key,
  cycle the layer, step into mouse mode, move the pointer, close. One
  screenshot per state, so the keyboard's own picture is the evidence.
- `timelines/osk-over-game.tl` -- the same over a GRAPHICS-mode guest. It
  answers Alley Cat's joystick prompt with `press select` first, because the
  game stays in text mode until it is answered and the capture would otherwise
  be of the other case.
- `timelines/osk-with-pause.tl` -- open the keyboard, then retro-go's PAUSE
  overlay on top of it. This is the one that exercises `core_repaint()` with
  the OSK live; run it under `probe.py --faults` rather than only capturing,
  because "the callback faulted" and "the menu is drawn over it" look identical
  in a screenshot.

Two more drive the keyboard's DELIVERY rather than its picture, and both were
written for the "keys only arrive once the OSK is closed" report:

- `timelines/osk-live-keys.tl` -- answer Alley Cat's prompt, open the OSK and
  press the same cell three times several seconds apart, leaving the keyboard
  UP throughout. Watch `x86_osk_codes` (codes injected) against
  `x86_kbd_port60_reads` (codes the guest took) and `input_kbd_ht` (the 8042
  head/tail): reads that lag the injections and then catch up all at once when
  the OSK closes ARE the defect, and nothing else in the tree shows it.
  `input_irqs` is the second witness -- 60/s means IRQ 1 is being re-armed
  every frame at a guest that cannot take it.
- `timelines/osk-disk-swap.tl` -- the "A:" cap's disk menu: open it, pick the
  next image, insert it. Needs a card with two images and one preselected
  (`sdcard.py --preselect`); watch `x86_osk_mode` (3 = menu), `dsk_swaps` and
  `dsk_mounted`.
- `timelines/osk-held-across.tl` -- LEFT held for 12 s while GAME cycles the
  OSK all the way round underneath it, then ordinary taps after the release.
  This is the "a button held across the transition" case; the release code goes
  out when the OSK takes the frame and the taps afterwards must still emit.

Which key each captures is worth knowing: the OSK opens on layer 0 cell 0, so
`press a` sends the letter `a` (scancode 0x1E) and `x86_kbd_read_log` is where
it is confirmed to have reached the guest.

### Recording an input timeline

`--record FILE.tl` on any target script captures an interactive gwemu session.
Needs the window (`--gui`); **quit gwemu normally** -- the file is written on
exit, so a killed process leaves nothing.

```bash
python3 scripts/probe.py --retrogo --gui --audio --timeline "" \
    --record timelines/alleycat-ingame.tl --samples 150 --interval 5
```

**The `.tl` grammar, read out of gwemu's own parser**
(`references/gwemu/hw/misc/gnw_timeline.c`):

```
# comment
[MM:]SS[.fff][s] | @FRAME    press   <btn>[+<btn>...]      # down, +100ms, up
                             hold    <btn>[+...] <secs>    # down, +secs, up
                             down    <btn>[+...]           # and stays down
                             release <btn>[+...]
                             screenshot <file.png>
                             quit
```

Buttons are case-insensitive: `up down left right a b game time start select
pause pwr` (gwemu spells it `pwr`; `power` is accepted too). `@N` addresses a
vblank FRAME rather than a time.

**A recording uses only half of this language.** `--record` emits `down` /
`release` pairs and nothing else, and every hand-written timeline in
`timelines/` is written in `press` / `screenshot` — so a parser built from a
recorded file rejects almost the whole directory. `scripts/common/timeline.py`
implements the grammar above, from the source, and `tests/test_timeline.py
--offline` parses every checked-in `.tl` as a smoke test.

`timelines/alleycat-ingame.tl` is a real session driving Alley Cat from launch
to gameplay: TIME answers the joystick prompt with N, then the skill menu needs
a LETTER, which is only reachable through the on-screen keyboard.

### Replaying a timeline on REAL HARDWARE

`--timeline FILE.tl` means the same thing on every tier: the tests do not know
which one they are on.

```bash
# gwemu (unchanged): the emulator replays the file itself, against emulated
# time -- exact.
python3 scripts/probe.py --retrogo --timeline timelines/press-game.tl

# hardware: the same file, injected over the debug probe.
python3 scripts/probe.py --retrogo --target hardware \
    --timeline timelines/press-game.tl --watch input_pad_seen,input_scancodes
```

How it works: a `REMOTE_INPUT` firmware build ORs a 32-bit shadow word at
`0x30001FF4` into the live button state on every input poll, so a probe write is
indistinguishable from a physical press. `HardwareTarget` folds the timeline
into a schedule of button masks and writes them from a background thread while
the harness keeps polling — the mechanism is vendored from the firmware's proven
`scripts/remote_input.py` (`references/` is gitignored, so nothing may depend on
it at runtime).

**The firmware must be built with `REMOTE_INPUT=1`.** Ours always is —
`retrogo_build.py`'s `RELEASE_DEFAULTS` — deliberately not a switch, because a
firmware without it turns every hardware timeline into a test that presses
nothing and passes. **Do not check for it by grepping the binary for
`30001FF4`: the compiler puts `.word 0x30001000` in the literal pool and reaches
the cell with `ldr.w rX,[rY,#4084]`, so the address never appears as four
consecutive bytes.** `retrogo_build.verify_remote_input(elf)` disassembles
`buttons_get` and checks the arithmetic; the build fails if the define did not
take.

**AN INTERRUPTED RUN USED TO STRAND THE DEVICE, AND NOW DOES NOT.** The shadow
word OVERRIDES the physical buttons on every input poll and it lives in D2
SRAM, which SURVIVES RESET -- so a run killed mid-flight left a fake button
state pinned through every later session, and the owner's own presses stopped
registering. It presents exactly like a guest input bug and it cost a session.
The player's thread is a daemon, so a killed process never reached its own
cleanup; `ShadowWordPlayer` now registers an `atexit` handler AND chains
`SIGINT`/`SIGTERM` to zero the word first. If a run ever does die in a way that
escapes those, clear it by hand before anyone touches the device:

```bash
python3 scripts/probe.py --target hardware --no-flash --zero 0x30001FF4:4
```

**`quit` on hardware stops injecting** — buttons released, shadow word zeroed,
the player finishes. There is no process to kill, and the device keeps running;
harnesses still derive their wait from `timeline_quit_time()`, so a run ends
where it would under gwemu.

**Accuracy.** Every state change is one 32-bit write; nothing compensates for
its latency, and the file is replayed as written. Measured under gwemu over
`GDBBackend` on `timelines/alleycat-ingame.tl` (78 state changes): the write
itself costs **0.11 ms mean / 0.28 ms max**, and events left **0.1–0.2 ms** late
against the file. End to end — press in the file to the bit appearing in the
firmware's `input_pad_seen` — was **+13 to +60 ms**, nearly all of it the
harness's own 50 ms observation poll. **OpenOCD over SWD has not been measured;
expect it to be slower than GDB-to-gwemu.** A press held for less than one input
poll (~16 ms) may be delivered and never observed; the player prints a warning
naming each such press rather than letting it silently do nothing. The shortest
press in any checked-in timeline is 40 ms.

## Modules (`scripts/common/`)

`dosimage.py` also carries **`FatVolume`**, a FAT12/FAT16 reader with partition
and subdirectory support. `Fat12` in the same module is floppy-only: an
unpartitioned volume, root directory only, which is all a 1.44 MB `.dsk` needs.
The larger titles ship as partitioned hard-disk images instead — an MBR at LBA
0, one FAT16 primary at LBA 63 — and keep their executables in subdirectories
(`DF/DOS4GW.EXE`), so `Fat12` cannot see them at all and returns an empty
listing rather than an error. `FatVolume(data).walk()` enumerates the whole
volume; `.read(path)` takes a `/`- or `\`-separated path.

Importable support only — no CLIs.

| Module | Purpose |
|---|---|
| `target.py` | `Image` (what boots) + `GwemuTarget`/`HardwareTarget` (where). One launch path for every tier. Also `timeline_quit_time(path)`: a test that polls longer than the timeline's `quit` outlives gwemu, and the dropped GDB link looks like a target fault. Derive waits from it; never hardcode. |
| `elf.py` | `get_symbols`, `nearest_symbol` |
| `sdcard.py` | Three SD destinations (image / probe / mounted) plus `compose()`, which lays out what the firmware needs |
| `timeline.py` | The `.tl` grammar (gwemu's, in full) plus the two players: gwemu replays a file itself, hardware gets `ShadowWordPlayer` writing `0x30001FF4` over the probe. `--timeline` therefore means the same thing on both tiers. Reports what was ACTUALLY delivered — lateness per state change and the hold each press got — instead of assuming the file's schedule |
| `retrogo_build.py` | Firmware build + artifact staging. Builds with `REMOTE_INPUT=1` and CHECKS the built image for it; also re-cuts `qemu_bank{1,2}.bin`, which `gwemu_release` silently does not (see below) |
| `retrogo_config.py` | `/CONFIG` blob generation |
| `core_abi.py` | Preflight: will this firmware accept this core? |
| `rglog.py` | Read retro-go's in-memory log ring |
| `faults.py` | Decode latched CPU faults |
| `asm_metrics.py` | Instruction counts, DWT capture |
| `budgets.py` | Host side of the budget gate: force a real re-link (or refuse a stale ELF), read `__CORE_ITCM_CODE_END__` / `__CORE_BSS_END__` / `__RAM_EMU_END__` via `nm`, derive ITCM free and the pager-frame upper bound, and compare against `tests/budgets.json`. The docstring records why the frame count is a bound and not a count |
| `zones.py` | Host side of the frame-loop zone accounting: locate the block, ask the TARGET to reset it at its own frame boundary (a host-side memset would race the stack marks), read it back, and derive the shares, the residual and the instrument's own overhead. Layout mirrors `x86_zones.h` |
| `opcode_profile.py` | Host side of the per-opcode DWT profile: locate/reset/read the DTCM table, resolve slots to handler symbols off the live dispatch tables, render table/JSON/CSV. Layout and the on-target half live in `x86_profile.h` / `cpu_exec.S` |
| `probe.py` | Sample PC, globals, pointer derefs and array dumps on a running target; `read_faults` decodes CFSR/HFSR/BFAR |
| `probe_acc.py` | Host side of the probe accumulator: locate `x86_probe`, ask the target to reset it, read it back, and JOIN runtime evidence with the expectations table into one status per row. The precedence rule is the module's whole correctness argument — **runtime evidence wins**, and the table only classifies calls the target says WERE answered. Layout mirrors `x86_probe.h` |
| `probe_expect.py` | The curated "what SHOULD we have returned" table — RBIL-shaped (vector, AH, subfunction, what a real BIOS/DOS does) plus what OUR handler does about it. This is the oracle, and the module docstring records why it is a table rather than a port-capable `refdos.py` or a DOSBox capture |
| `watchpoint.py` | Z2/Z3 data watchpoints over the GDB remote protocol, and the unsolicited stop reply that follows a resume |
| `fat12.py` | Builds a 64 KB FAT12 volume in memory with named files and known contents. A test that asserts on a shipped `.dsk` is asserting on someone else's data; this lets `test_dos.py` write the bytes it then expects to read back |
| `dosimage.py` | FAT12 reader + MZ loader that mirrors `x86_dos_exec`, so a reference emulator starts from OUR entry state |
| `refx86.py` | Unicorn run of that image, sampled like the target's own sampler |
| `refdos.py` | A minimal DOS/BIOS for the reference, so it can run PAST the first INT 21h and say what the program should do. Deliberately not modelled on our x86_dos.c -- mirroring it would make the two agree on the same mistake. It has no port I/O, so it cannot follow a program that times against the CGA retrace |
| `textmode.py` | Host-side model of the CGA text renderer: CP437 font load (4x8 by default, the table the renderer uses), the 80x25 -> palette-index render at 4x8 cells, AH=06 scrolling, and a readable CP437 dump. Shared by `test_text.py` (seeded pattern) and `test_screen.py` (the live guest) |
| `golden.py` | The golden gate's comparator and its attestation object: an 8-bit-truecolour PNG reader (deliberately strict -- a decoder that coped with other shapes would let a differently-quantised golden in), a masked pixel comparison that reports WHERE it differs, and `Golden`, which refuses to be a golden at all without a record of who verified it and what they saw. Kept separate from `golden_screens.py` because it is the part that has to be right and the only part testable without a target |
| `verify.py` | Symbol-region checks and ELF-vs-target section comparison |
| `builder.py`, `runner.py` | Tier-1 build convenience |

## Non-negotiables

- Build flags are **not** dependencies — change one, pass `--clean`. **This
  includes `lib/FatFs/ffconf.h`**: editing it and rebuilding incrementally links
  objects compiled against two different configs, and `FF_FS_EXFAT` alone
  changes the width of `LBA_t`. That produced a hang and a mounted-but-empty
  directory, both of which read as "the change broke FatFs" and neither of
  which was real. `rm -f lib/FatFs/*.o build/core/*.o` first.
- **Our FatFs is not the firmware's.** Ours mounts the FAT12 inside a guest
  `.dsk` and is built with LFN and exFAT OFF (a floppy has neither, and it buys
  8 KB of RAM_EMU). The firmware's separate instance reads the `.dsk` off the
  card and keeps LFN. Code that walks a `FILINFO` must use `DOS_SHORT_NAME()`,
  because `altname` does not exist without LFN.
- **The CS and interrupt-vector trace rings are OFF by default: `X86_CS_LOG=1`.**
  `x86_cs_log`, `x86_cs_log_idx` and `x86_int_vec_log` are written from inside
  `L(SET_SREG)` in `cpu_exec_handlers.S`, which is the single path every far
  `CALL`, `RETF`, `IRET` and interrupt takes -- fourteen instructions and five
  stores per far transfer, plus a four-instruction IVT re-read in the interrupt
  path that exists only because the logging needed R0. TOPBENCH takes 1.22 M far
  transfers per 25 s window, so that was ~17 M instructions of logging against
  56.6 M dispatches. `--gc-sections` drops the storage in a default build, so
  `probe.py --dump x86_cs_log:16x4` and `trace_diff.py` report the symbol as
  MISSING rather than reading zeros. Rebuild (clean -- flags are not
  dependencies) with `X86_CS_LOG=1` when you need either ring.

- **Measure `__CORE_BSS_END__` after any change to the core.** The core's TEXT
  lives in RAM_EMU too, so code growth counts. Overshooting is SILENT: the
  guest RAM allocation returns NULL, `core_ready` stays 0 and the emulator
  never starts. **`python3 scripts/budget_gate.py` is that measurement**, plus
  ITCM and `docs/07` — run it before merging a core change, and never quote a
  size from a build you did not force.
- Green under gwemu ≠ hardware ready; gwemu does not model peripheral setup.
- Never use raw GDB subprocesses or `gnwmanager monitor`; use the backends.
- Never run concurrent builds or tests from one directory.
- All scripts are Python with `argparse`. No `.gdb` scripts.

Detail lives with the code: each module's docstring, plus
[`docs/04`](docs/04_retro_go_and_gwemu.md) (gwemu/retro-go traps) and
[`docs/06`](docs/06_gwemu_determinism.md) (determinism data).

## Anti-aliased text, and the CLUT budget it lives in

`lcd_set_clut(clut, count)` clamps `count` to 32 and then writes a 40%-darkened
twin of entry `i` at `count + i`. Retro-go's overlay reaches those twins by
OR-ing `LCD_DARKEN_BIT = 0x20` — a FIXED +32 — into every framebuffer byte, so
the two only agree when `count` is exactly 32. Indices 64-67 are the overlay's
own theme (`LCD_OVERLAY_CLUT_BASE`), and the firmware cache is 68 entries.

**So the budget is 32 entries, not 64.** 32-63 are not free; they are the twins.
(One exception, and it is a different firmware path rather than a bigger count:
`GW_LCD_SET_CLUT_EXT` takes a caller-owned 256-entry palette and bypasses the
twin cache entirely, which is what VGA mode 13h uses. The price is that
`LCD_DARKEN_BIT` stops working while that mode is up --
[`docs/08_vga_mode13h.md`](docs/08_vga_mode13h.md).)
The core published 16, which put the twins at 16-31 while the overlay read
32-47 — zeros — so opening retro-go's menu dimmed the game to solid black. It
publishes 32 now, in every mode.

Anti-aliasing spends that budget as eight four-entry blocks, allocated **per
frame** to the (fg,bg) pairs the 2,000 cells actually use, ranked by cell count;
pairs that miss out degrade to bilevel in the nearest published colour.
Measured with `screenshot.py --pairs`, TOPBENCH's menu uses exactly 8 — a fit
with nothing to spare — and its boot console uses 1.

```bash
make sdcard X86_TEXT_AA=1      # capture-only core: AA on, bilevel table dropped
```

It is **off by default**, and the reason is size, not quality:

> RAM_EMU spans `0x2404B000`-`0x24100000` (741,376 B). The core ends at
> `__CORE_BSS_END__` (measure it: `arm-none-eabi-nm build/core/dos_core.elf`),
> and the guest asks for 32 KB VRAM + 640 KB RAM = 688,128 B from that region.
> **Measured 2026-08-13, after mode 13h and moving CGA VRAM to the bonus pool:
> BSS end `0x240587f8`, headroom 30,728 bytes** -- the guest now asks RAM_EMU
> for 640 KB rather than 672 KB, because its 32 KB of VRAM comes from the bonus
> pool. Mid-change the same measurement read **-2,024**, which is the state in
> which `guest_ram` returns NULL and nothing says so. It was
> 424 bytes before the 4 KB open-bus filler was moved to the bonus pool; the
> "2,528 bytes" this document used to quote was two commits of growth out of
> date, which is why the number is now a measurement instruction and not a
> constant.

Each narrow table is 2 KB, so an AA core must drop the bilevel one to pay for
it (`X86_TEXT_BILEVEL_FONT=0`, set automatically) and degrades by thresholding
the 2-bit glyph instead. The bare-metal harness has no such limit and links
both, which is how `test_text.py` checks each renderer against its own font.

**The trap this hid behind:** an over-allocation is quiet. `ram_malloc()` does
bound-check and returns NULL (`Core/Src/gw_malloc.c`) — so nothing is
overwritten — but a NULL that a caller tolerates is a feature silently missing,
not an error. That is exactly what happened to the 4 KB open-bus filler: it was
the LAST allocation, `if (upper_filler)` skipped the mapping, and the symptom
was an option-ROM scan faulting in the guest. It comes from the bonus pool now,
and `upper_filler_ok` reports it. If a change to the core makes a previously
working guest fault early, check `__CORE_BSS_END__` before anything else.
