# The memory tiers: pager, arena, and what is NOT built

Companion to [`01_memory_architecture.md`](01_memory_architecture.md), which
covers the fold table itself. This file is about what sits on top of it, and
about the gap between what `x86_pager.h` documents and what the tree does.

## 1. The pager

`x86_pager.h` / `x86_pager.c`. 4 KB pages, `X86_PG_PAGES` = 2,048 (8 MB of guest
space), per-page state in 8 KB of DTCM supplied by the caller.

A non-resident page is in exactly one of three places:

| tier | meaning | cost |
|---|---|---|
| ZERO | never written | nothing anywhere; most of an 8 MB space |
| NOR | a clean page of the boot image, read in place over XIP | zero AXI, zero SD |
| FILE | modified and then evicted, in the pagefile on SD | an SD read to fault back |

The fast path is free because a non-resident page holds `FOLD_INVALID`
(`0x80000000`) and the interpreter's existing `ADDS`/`BMI` catches it — no extra
test, no permission bits, no second table. `0x80000000`-`0x8FFFFFFF` (a fold
miss) and `0x90000000`+ (NOR-backed) are distinguishable by inspection, and the
faulting address falls out of the trap value as `bad - 0x80000000`.

Backing store is abstracted behind `x86_pg_read_fn`/`x86_pg_write_fn`, set by
`src/main.c`. That separation is deliberate: it is what lets the whole mechanism
be driven at tier 1 against a plain RAM buffer with no card, no FatFs and no
firmware.

### 1a. The pagefile — `src/main.c`, and it is wired

`pgf_read()` / `pgf_write()` in `src/main.c`, handed to `x86_pager_set_backing()`
from `guest_reset()`. The file is `x86page.swp` next to the `.dsk` — the one
directory this core has demonstrated it can write — and it is truncated at every
guest reset. **ONE handle, held open for the life of the title, shared with
XMS**; `xms_opens` is 1 for a run and is the witness. `pgf_ready` /
`pgf_result` / `pgf_writes` / `pgf_reads` / `pgf_errors` are the rest.

**The slot is NOT the guest page number** — it comes from a ring
(`x86_pgfile.c`), and that is the whole of §1b.

### 1b. What a 4 KB writeback costs, and why the slot moved

`x86_pg_wb_us_worst` reached **433,509 µs** on silicon and grew across a
session. Under gwemu the same write reads ~25 ms, which is why this was carried
to tier 3 before anything was designed against it.

**The split says where it goes.** `pgf_us_seek` / `pgf_us_write` and their
worsts, KEEN4 on hardware, 86 writebacks in one run:

| | total | worst |
|---|---|---|
| `f_lseek` | 340 ms over 86 seeks | 14 ms |
| `f_write` | **4.04 s over 86 writes** | **405 ms** |

**So it is not the cluster-chain walk, and `FF_USE_FASTSEEK` is not the fix.**
85 of the 86 writes landed in *already-allocated* clusters (`pgf_inp_writes`
against `pgf_ext_writes`), so no chain was being stretched. `f_expand()` and a
CLMT would have bought the 340 ms. They are also not reachable: the pagefile
goes through the ABI's **stdio**, so the `ffconf.h` that governs it is
retro-go's, which is upstream and not ours; our `lib/FatFs` interprets the
*guest* filesystem inside the `.dsk` and has nothing to do with the SD card.

**`X86_PGF_BENCH=1`** (`pgf_bench()`, one second at title load) then asked the
card directly, every arm 4 KB at a time and every shape run twice in opposite
order so warm-up cannot explain it:

| arm | per 4 KB |
|---|---|
| appended past EOF | **12.2 – 13.8 ms** |
| rewritten in place | **60.1 – 67.3 ms** |
| 32 KB rewritten in ONE call | 60.4 ms |
| 512 B × 64 over the same 32 KB | 62.0 ms |
| read | 3.8 ms |

Three findings, and they close three doors:

- **Call size and call count are irrelevant.** 32 KB in one write costs what 64
  writes of 512 B cost. **A write-behind queue would buy nothing**, so it is not
  here and its ordering hazard is not taken.
- **Rewriting recently-written sectors is 5×.** That is the card's own garbage
  collection; the only thing a core above the ABI chooses is *where* a page
  lands.
- Reads are 16× cheaper than writes per byte. Refills are not the problem.

**The fix is the slot ring** (`x86_pgfile.c`, and its header is the argument).
A page's slot comes from a cursor sweeping `X86_PGF_RING_SLOTS` (512 = 2 MB),
so a writeback lands on space untouched for a whole revolution, and `pgf_map`
remembers where each page's copy went. Two rules keep it honest: the map moves
**only on a write that succeeded** (a failed writeback now leaves the previous
copy readable instead of half-overwriting the only one), and a **live slot is
never handed out** (the ring is bigger than the 416 pages that can ever be
live; `x86_pgf_slot_full` says so if that is ever wrong). Ordering is unchanged
— every writeback is still synchronous and still completes before its frame is
reused.

The ring is **bounded for the seek's sake, not the card's**: the card would
rather the log grew forever, but with `FF_USE_FASTSEEK 0` a refill's backward
seek walks the chain from byte zero, so the file has to stay small.

`tests/test_paging.py` proves the ring at tier 1 — a page written, evicted and
read back byte-identical; a rewrite landing on a *different* slot with the old
one freed; a full wrap in which no live slot is ever handed out — with both
negative controls **seen to fail**: corrupting the writeback, and reading the
copy the page had before its last write (what a read that overtook a pending
write would get).

### 1c. The `.dsk` path had the same defect, and a worse one

`x86_disk.c` was reading through `rg_storage_copy_file_range_to_ram`, which
**opens and closes the image on every call** — `f_open` is a directory walk with
LFN on, and only then does the chain walk and the 512 bytes happen. The `.dsk`
handle is now held open too (`dsk_opens` is the witness, and it is 1 per title),
and one handle serves reads *and* writes, which also removes the duplicate open
`FF_FS_LOCK 0` was not refusing. `dsk_us_open` / `dsk_us_seek` / `dsk_us_read`
split it the way `pgf_us_*` does. `f_expand()` is **not** an option here: `.dsk`
files are copied onto the card from a PC and are fragmented below us.

## 2. Eviction — built, and the header is now a description

*(Historical note: this section used to read "EVICTION IS NOT IMPLEMENTED".
It is implemented; `tests/test_paging.py` drives all of it at tier 1 against a
RAM buffer standing in for the pagefile.)*

The design, for reference: `x86_pager.h` documents eviction-time dirty
detection by comparison against the tier's authoritative copy, and declares `x86_pg_evictions`,
`_drop_clean`, `_wb_zero`, `_wb_file`, `_wb_bytes`, `_wb_opens`, `_wb_us_worst`
and `_donated`.

All of them are live. MEASURED, WOLF3D gameplay under gwemu with 34 frames
donated: `_evictions`=39, `_drop_clean`=0, `_wb_file`=39, `_wb_zero`=0,
`_io_errors`=0, `_exhausted`=0 — and the invariant
`drop_clean + wb_file + io_errors == evictions` holds. `x86_livemap_holds()`
and the pin bitmap are both consulted by the victim picker.

The design, verbatim, because it is the reason the figures look like that:
**decide at EVICTION by comparing against the tier's authoritative copy** — scan a ZERO page
for a non-zero byte, compare a NOR page against its snapshot — so the answer is
EXACT rather than conservative, and a page that matches is DROPPED with no SD
I/O. The alternative (treat every resident page as dirty) is what produced the
previous project's 566 MB of pagefile traffic and 98.6% wasted writes. A 4 KB
compare is ~1 µs against a pagefile write measured in milliseconds.

FILE tier is the one conservative case — no in-memory authority to compare
against — and it is bounded, because FILE only ever holds pages that were
genuinely modified.

## 3. Measured pressure, and the read-mostly premise

gwemu tier 2, whole runs:

| title | resident | `_faults_r` | `_faults_w` | `_fill_zero` / `_nor` / `_file` |
|---|---:|---:|---:|---|
| WOLF3D (210 M insns) | 104/160 | **0** | 104 | 104 / 0 / 0 |
| KEEN4 | 134/160 | **0** | 134 | 134 / 0 / 0 |

`_frames_total=160` (the full 640 KB, no frame lost to alignment),
`_exhausted=0`, `_unmapped=0`, `_io_errors=0`.

**Those two rows are TITLE SCREENS, and they undersell the pressure.** Measured
since, on WOLF3D driven into GAMEPLAY (`timelines/wolf3d-ingame.tl`): resident
reaches **160/160** — the pool is full. The 104 figure is the credits screen and
quoting it as the working set is how "56 idle frames" was arrived at. Donation
still fits, because 126 frames is enough for the gameplay set; but size any
future donation against a title that is PLAYING, not one that is booting.

Two conclusions:

1. **No title has come close to pressure.** The pool never empties, so the
   failure arm is dead code in practice — and the NOR and FILE tiers have never
   been touched by a real guest (`x86_nor_attached=0`).
2. **The read-mostly premise that NOR-XIP rests on is FALSE for every guest we
   can run.** Zero pages in either working set are read-only; 100% became
   resident through a write. That is the same shape the prior project measured
   ("~100% of NOR-backable data granules get written anyway"), now confirmed
   here. Gate any NOR work on re-measuring this, not on the design's promise.

## 4. The arena

`x86_arena.c` / `x86_arena.h`. Display storage carved from the LCD bonus pool,
with fixed claim ids rather than an allocator, so a leaked claim is impossible
and a probe can name what holds it.

| id | use |
|---|---|
| `X86_ARENA_ID_CGA` | 0xB8000, 32 KB |
| `X86_ARENA_ID_VGA13` | 0xA0000, 64 KB linear |
| `X86_ARENA_ID_EGA` | 0xA0000, 4 planes |
| `X86_ARENA_ID_SPARE` | Mode Y's off-screen cache |
| `X86_ARENA_ID_MODEYP` | Mode Y's REAL display pages 1 and 2, 64 KB each |

**Modes are mutually exclusive, so peak demand is the LARGEST mode, not the
sum.** CGA VRAM and the 13h aperture used to be permanent allocations — 96 KB
spent to serve one mode at a time, which is why EGA 0Dh was unreachable. They
are claims now, and a planar mode can lend the CGA claim out.

Measured: `x86_arena_bytes = 143,360`. The layout is computed at **runtime** from
the pool's actual base, because the bonus pool arrives 2048-aligned and not
4096-aligned, and a compile-time map loses a whole page to that.

**Fragmentation is real**: claims sit in init order, so releasing one leaves
*two* holes and the aperture must be re-claimed to compact. Check
`x86_arena_largest`, not just `x86_arena_free`.

Every arena page is **pinned** — it is display storage, the blitter reads it
every frame, and evicting one is visible instantly. The pin bitmap is indexed by
guest page number so the victim picker can reject one in a single test.

`x86_guest_ptr()` hands out raw host pointers that **no caller releases**, so a
reference count cannot work; the livemap ring is a small set of
recently-handed-out pages that eviction must refuse. The prior project measured
**5 such evictions producing 26,329 wrong bytes** with its `cow_lost == 0`
invariant green throughout.

## 5. Donating guest frames to the arena — the route that was overlooked

`PLAN.md` Phase 3 states the intent: *"For EGA 256KB mode, the extra 224KB is
dynamically borrowed from the guest RAM AXI pool."* The mechanism is
`x86_pg_donated` ("frames given to the arena") plus `x86_arena_add_region()`.

This is what makes Keen 4's levels reachable, and the arithmetic is in
[`13_title_status.md`](13_title_status.md).

**Donation without eviction turns a later shortfall into a hard
`X86_EXIT_MEM_FAULT` instead of a page-out**, which is why the two are one piece
of work — and why donation stayed gated on a backing store existing until the
pagefile above landed.

### 5a. What donation is actually spent on: Mode Y's real pages

Wolf3D triple-buffers across display starts `0x0000`, `0x4100`, `0x8200`
(stride 16,640, learned). With one page's storage all three fold onto the same
bytes and the raycaster draws the next page over the one being shown. `vga_cell()`
in `x86_video_planar.c` is the one place that says where a plane byte lives; it
now takes page 1 and 2 from `X86_ARENA_ID_MODEYP` when they exist and falls back
to the alias when they do not, counting every fallback in
`x86_vga_modey_alias_hits`. `x86_vga_modey_pages` is 1 (aliased), 2 or 3.

MEASURED, WOLF3D gameplay under gwemu: `_donated`=34 frames / 139,264 B,
`_donate_refused`=0, `x86_vga_modey_pages`=**3**, `alias_hits`=0. Guest RAM
falls from 160 to 126 frames, which costs 39 one-off page-outs at level load
and nothing after that — the gameplay working set fits in 126.

**A single captured frame cannot see any of this, in either direction.** In
gameplay the panel is painted AT THE GUEST'S FLIP, synchronously, from a page it
has just finished; a before/after gwemu screenshot of the same moment is a clean
Wolf3D frame both ways. The property is asserted structurally instead, at tier 1,
by `tests/test_video.py`'s Mode Y real-page arm — with the alias, a write to
page 1 overwrites page 0, and that negative control is seen to fail in the same
run.

## 5b. The donation POLICY -- the reserve is measured, not compiled in

`X86_PG_DONATE_RESERVE` is evaluated ONCE, at mode adoption, and that single
fact is the whole of the "EGA card OR guest frames" dichotomy. It is not a
property of the memory: KEEN4's intro is idle in RAM and wants every plane
offset it can get, its gameplay wants its frames back, and both are true of the
same title ten seconds apart. So the decision is re-evaluated, once per frame,
against a working set the pager MEASURES:

    reserve = max(FLOOR 64, x86_pg_resident_peak + MARGIN 8, ratchet)

`x86_pg_resident_peak` is a compare on the fill path (after a 4 KB copy, so
free). The ratchet records what pressure has already taught the policy, so a
working set that dips does not invite the card to grow back into it.

**The shrink trigger is FREE FRAMES below MARGIN, not `x86_pg_evictions`.** An
eviction is the cost, not the warning -- by the time the counter moves the guest
has already paid a 4 KB pagefile write, up to 433 ms on silicon (§1a). Two
alternatives were rejected: *shrink at the first eviction* (fires one whole
writeback too late) and *a hysteresis loop on the eviction rate* (it has to
thrash to sample, so it converges by paying exactly the cost it exists to
avoid). The eviction counter is kept as a backstop for a burst inside one tick.

`x86_pager_policy_tick()` returns HOLD / GROW / SHRINK and is called from ONE
place: the top of `video_render_body()`, before anything has read the card and
before `mode_sync_from_bda()` can adopt a new mode. Never mid-frame, never per
access. A SHRINK releases the EGA claim and only then reclaims -- a reclaim with
a claim still inside the donated extent is refused (`x86_pg_reclaim_busy`) and
the frames stay lent for the session. **GROW is permission, not an
instruction**: a re-fit costs the card's contents (`x86_vga_planar_arm()` zeroes
planes and aperture), so the video side only spends that when the headroom
demonstrably funds a card at least one arena page bigger.

**A SHRINK BORROWS NOTHING.** The first draft re-donated inside the same refit
under the new, higher reserve; measured on KEEN4 that cost **9 evictions**
instead of 0, because each smaller donation was recalled again a tick later and
every hop paid its own writebacks. A shrink now goes straight to the arena-only
card.

### Measured, KEEN4, gwemu tier 2 AND hardware (same shape both tiers)

| t | card offsets | donated | frames_total | resident | reserve | evictions |
|---|---:|---:|---:|---:|---:|---:|
| before 0Dh | 0 | 0 | 158 | 68 | 76 | 0 |
| 0Dh adopted | **65,536** | 66 | 92 | 68 | 76 | 0 |
| the set grows | 65,536 | 66 | 92 | 78 | 86 | 0 |
| watermark fires | 35,840 | **0** | **158** | 86 | 94 | **0** |
| steady state | 35,840 | 0 | 158 | 134 | 142 | **0** |

So Keen gets the **65,536 offsets/plane the Galaxy engine wants** for its intro
-- a size no build-time reserve on this part can fund -- and its gameplay runs
on the whole 158-frame pool with `x86_pg_wb_us_worst` = **0**. At the static
reserve of 134 the card was 35,840 for the whole session; at 100 it was bigger
and cost 84 evictions and 82 writes. The dichotomy was the schedule, not the
memory.

**WOLF3D is untouched, on hardware**: `donated`=0, `frames_total`=158,
`evictions`=0, `wb_file`=0, `shrink_signals`=0, `modey_pages`=2. At
`X86_MODEY_EXTRA_PAGES=1` the arena places the extra page with no donation, so
the policy never fires and the golden stays at **0.000%**.

**What the knobs mean now.** `X86_PG_RESERVE_FLOOR` / `_MARGIN` /
`X86_PG_POLICY_QUIET` (x86_pager.h) are the owner's dials; `X86_PG_POLICY=0`
restores the old fixed-reserve behaviour byte for byte, and
`X86_PG_DONATE_RESERVE` is what governs in that build. The intro-rows-versus-
frames trade is not silently picked: it is resolved IN TIME, and both ends of
it are visible in `x86_vga_ega_card_offsets` and `x86_pg_frames_total`.

**Negative controls, seen to fail**: `scripts/run_paging_controls.py` builds
`X86_PG_POLICY_NO_SHRINK=1` (17 evictions appear, pagefile written) and
`X86_PG_POLICY_NO_GROW=1` (nothing is ever borrowed). `tests/test_paging.py`
grades the loop at tier 1, 149 checks.

## 6. XMS — a tier with NO fast path at all

`x86_xms.h` / the XMS block at the end of `x86_dos.c`. It is listed here because
it is a place guest bytes live, but it is unlike every other row above it:

**XMS is a BLOCK-COPY interface, not a mapped one.** A real-mode guest cannot
address extended memory. It allocates a handle (`AH=09h`) and moves bytes in
bulk with `AH=0Bh`, which we service. No instruction in the guest ever
dereferences an XMS byte, so XMS memory is **never in the fold table, never
resident in AXI, and never a page the victim picker can see**. There is no
`FOLD_INVALID` check, no seam, no `MEM_TRAP`, no eviction pressure. The cost is
paid inside `AH=0Bh`, at a moment the guest chose, and nowhere else.

That is why XMS comes before EMS in `PLAN.md` Queue C. EMS's page frame *is*
mapped and *is* read by ordinary instructions; XMS must not import that cost,
and implementing it by handing out pager frames would do exactly that for
nothing.

### 6a. Where the bytes are, and why they cannot collide

The same `x86page.swp`, at byte offset `X86_PG_PAGES * X86_PG_SIZE` (8 MB) and
above. The pager's `slot` IS the guest page number and guest page numbers stop
at `X86_PG_PAGES`, so the pager can never write past that offset. Collision-free
by arithmetic, not by convention. The backing ABI is **byte-ranged**, not
slot-ranged: a slot ABI would force a read-modify-write of a 4 KB slot for any
unaligned move.

### 6b. What `AH=08h` advertises, and the number that made it honest

`X86_XMS_DEFAULT_KB` = **4,096 KB**, and **0 when there is no pagefile** — with
no store there is nowhere to put a byte, so `INT 2Fh AX=4300h` answers "absent"
and nothing is promised. The size is not bounded by the 160-frame AXI pool
because no XMS byte is ever in a frame; it is bounded by the card and by
transfer time.

**MEASURED, WOLF3D driven into gameplay under gwemu, same timeline each way:**

| | frames | guest insns |
|---|---:|---:|
| XMS absent (the tree before this) | 1,104 | 194.5 M |
| XMS, `fopen`/`fclose` per move | 766 | 132.7 M |
| XMS, the file handle held open | **1,057** | 171.9 M |

Wolf3D takes the XMS path the moment it is offered — 278 moves, 430,636 bytes,
0 I/O errors — and at ~25 ms per open (§1a) the per-move `fopen` alone cost a
third of the title's frame rate. `src/main.c` therefore holds the handle open
for the life of the title and closes it at guest reset, before `pgf_open_file()`
truncates the file. `xms_opens` is **1** for a whole run and is the witness: if
it ever tracks the move count, that decision has been reverted.

### 6c. What is refused, and said out loud

* **HMA** (`AH=01h`/`02h`): `BL=90h`, "HMA does not exist". The sixteen pages at
  0x100000 are the A20 *wrap* window (§`x86_mem.c`); nothing owns them as memory
  when the gate is open.
* **Lock** (`AH=0Ch`): `BL=0ADh`, "lock failed". A lock's purpose is to yield a
  32-bit linear address the caller then dereferences. Our bytes are in a file.
* **A20** (`AH=03h`-`07h`) drives `x86_a20_set()` — the machine's own gate, the
  one the 8042 path in `x86_io.c` moves. There is no second A20 model.

## 6d. EMS — the same allocator problem with the OPPOSITE residency rule

§6 opens by saying XMS has no fast path *because no guest instruction ever
dereferences an XMS byte*. **EMS is the inverse and that one sentence is the
whole difference.** It puts a 64 KB page frame at `E000:0000`, four 16 KB
windows, and the guest maps a logical page in with `INT 67h AH=44h` and then
reads and writes it with `MOV`, `STOSW`, `REP MOVSB`. So:

* **A mapped page MUST be folded and AXI-resident.** Backing it the way XMS is
  backed would fault on every access — a cliff, not a slope.
* **But only 64 KB is live, and `AH=44h` says exactly when that changes.** The
  hot set is bounded at four logical pages and it is *explicitly signalled*,
  which is better information than the pager ever gets from faults: it arrives
  before the access instead of after it.

Hence **pin on map, release on unmap**, using `x86_pin_pages()` — the arena's
own bitmap and already the victim picker's first veto (§4). Not a second
mechanism: a mapped EMS page is the same kind of object as a VRAM page, memory
something outside the interpreter is reading.

Logical pages **are pager pages**, at guest page 1024 (linear 4 MB) in a SECOND
owned window (`x86_pager_own_aux`). Three arithmetic properties make that safe:
no real-mode `seg:off` can name it (`FFFF:FFFF` = 0x10FFEF), it is far above
guest RAM's pages 0..159, and (since §1b) it is mapped through the same slot
ring, whose 2 MB sits below the XMS store at 8 MB. A window is widened rather than shared so that nothing between
640 KB and 4 MB becomes ownable by accident.

An UNMAPPED logical page is cold by definition: an ordinary pager page that may
be evicted to the pagefile and paged back on the next `AH=44h`. That is the
cost curve — a slope for the cold set, zero for the hot one.

**Advertises 1024 KB, and 0 with no pagefile**, for the same honesty reason
§6b gives: with no store the pager cannot evict, so every page handed out would
have to stay resident forever out of 160 frames. With no store, `EMMXXXX0` does
not open and the `INT 67h` vector is left on the bare IRET.

**The page frame is CHECKED, not assumed.** The upper megabyte here is one 4 KB
`0xFF` page aliased everywhere except VRAM, the 13h aperture and the BIOS id
page, so a free window is sixteen fold entries that are mapped and all resolve
to the *same* host address. `x86_ems_set_backing()` tests exactly that and
refuses (`x86_ems_frame_refused`) rather than quietly overlaying the display.

## 7. Where things live

`src/main.c`'s `bonus_layout()`, computed at runtime:

| block | size |
|---|---:|
| 13h palette (absorbs the base's misalignment) | 2 KB |
| open-bus filler, aliased over 0xA0000-0xFFFFF | 4 KB |
| BIOS identification page at 0xFF000 | 4 KB |
| **the arena** — everything else | 143,360 B |

The `.dsk` sector cache is **not** here: it is `GW_MEM_AHB`, because it is a DMA
staging buffer and D2 SRAM is the only SRAM DMA1/DMA2 can reach, and because
`GW_MEM_AHB` is the only ABI pool with a real `free()`.

**RAM_EMU exhaustion is SILENT.** `ram_malloc()` bound-checks and returns NULL,
so nothing is corrupted — but a caller that tolerates NULL loses a feature with
no message, which is what happened to the open-bus filler. Measure
`__CORE_BSS_END__` before and after any core change; the core's TEXT is in this
region too, so code growth counts.

## 8. DTCM is a tier too, and its free space was over-reported by a stack

DTCM is 128 KB at `0x20000000`, handed out by the firmware's `dtc_malloc()`
bump pool. The core takes **43,228 B** of it, all of it on a per-access path
bar one entry:

| block | size | touched |
|---|---:|---|
| `X86CPUState` | 2,268 | every instruction |
| fold table (`R9`) | 16,384 | every memory access |
| mode records (`R2`) | 16,384 | every dispatch (8,192 + 8 KB align) |
| pager page table | 8,192 | **on a FAULT, not on an access** |

**The pool ends at `__dtc_padding_end__` = `0x2001A000`, not at `0x20020000`.**
The 24 KB above it is `_Min_Stack_Size` + `_Stack_Redzone_Size` — the main
stack (`STM32H7B0VBTx_SDCARD.ld`, `._dtcm_free`). `core_dtcm_headroom_max`
computed its bound against the end of the RAM instead and therefore
**over-reported free DTCM by 24,576 bytes on every reading this project has
ever taken** — the "96,068 B free" in `TOOLING.md` and the 87,844 quoted into a
task plan are both that error. Fixed in `src/main.c`; it now reads **63,268**.

### The 13h aperture does not fit here, and that is measured

Moving the 65,536-byte VGA aperture out of the arena and into DTCM was tried,
on the strength of the wrong number. `dtc_malloc()` returns NULL:

```
left   63,268     wanted   65,568     SHORT BY 2,300
```

The attempt is kept, not deleted — it costs one failed call at init, it is what
made the headroom bug visible, and `x86_vga_aperture_dtcm_fail` says on every
boot that it is still failing. The fallback to `x86_arena_claim()` is the old
path unchanged, so a picture is never at risk.

**The only route left is to evict the pager page table**, the one DTCM tenant
that is not on a per-access path. That leaves 71,460 and fits the aperture with
5,892 to spare, and it would free 65,536 B of arena — enough for one extra Mode
Y real page with **no donated frames**. It is not taken here because the memory
it moves to is not free either: RAM_EMU costs two pager frames
(`x86_pg_frames_total` 160 → 158) and the AHB heap's free space is not knowable
from a link map. That is a trade to decide, not a refactor to slip in.

### DMA2D can never reach the aperture if it moves here

DMA1/DMA2 and Chrom-ART **cannot address DTCM** — it is CPU-private, off the
AXI matrix. So an aperture in DTCM forecloses Queue B5's DMA2D A/B of the
chained 13h blit entirely: the engine would have nothing it could read. This is
recorded so it is not rediscovered as a bug in the DMA2D path. `X86_VIDEO_DMA2D`
defaults to 0, so there is no conflict today.
