# VGA mode 13h, the firmware ABI change it needed, and where memory now lives

320x200x256, linear, one byte per pixel at guest `A000:0000`, 64,000 bytes.
Row `y` at `y * 320`. No banks, no interleave, no planes, no CRTC start
address. It is byte-for-byte the format of our LUT8 framebuffer, which is why
it is the 256-colour mode worth building first and why the blit is 200
`memcpy`s.

Companion reading: `references/8086tiny/docs/video/10-mode-13h.md` (the mode),
`15-mode-y.md` (unchained 13h, NOT built here — see the last section),
`12-vga13-direct-scanout.md` (the cache arithmetic this design leans on).

## 1. Why this needed a firmware change

`STATUS.md` used to list mode 13h as **undisplayable, not merely tight**, and
that was correct. `lcd_set_clut(clut, count)` clamps `count` to
`LCD_CLUT_CACHE_MAX = 32` and then writes a 40%-darkened **twin** of entry `i`
at `count + i`, because retro-go's overlay dims the screen by OR-ing
`LCD_DARKEN_BIT = 0x20` — a fixed +32 — into every framebuffer byte. The
staging array is 68 entries (32 cart + 32 twins + 4 overlay theme).

256 entries cannot be expressed in that model at all: the twins would need
**512 hardware slots against the LTDC's 256**. Raising the cap is not a bigger
buffer, it is an impossibility. So a 256-colour mode has to own the whole
table, and the only question is where the copy lives.

The firmware change is therefore a second entry point, not a bigger cache:

```c
void lcd_set_clut_ext(const uint32_t *clut, uint16_t count);   /* count <= 256 */
```

It takes a **caller-owned** array and costs the firmware 4 bytes of state
(`ext_clut`, `ext_clut_count`) instead of the ~1.8 KB of DTCM a 256-entry
`active_clut[]` would need — and DTCM is firmware's and tight. `clut_push()`
re-pushes the external palette instead of the cache, so an overlay theme change
cannot stamp `active_clut[0..67]` over the guest's first 68 colours.
`lcd_pack_color()` searches the external palette too, so menu colours still
resolve to a real nearest match rather than to a stale CGA index.

Exposed to external cores as **one appended ABI op**:

```c
GW_LCD_SET_CLUT_EXT = 11,   /* a=(uintptr_t)clut*, b=count */
```

Appended after `GW_LCD_BACKLIGHT_SET`, so cores built against an earlier v2
header are unaffected. **No version bump**: v2 is unpublished and already
appends freely.

The `gw_lcd.c` / `gw_lcd.h` half is a hand-application of the user's own commit
`ba73090b6` "video: VGA mode 13h -- firmware-side palette and blit"; the
`dos_video.c` and 8086tiny-submodule halves of that commit belong to a
different core and were deliberately left behind.

### The accepted caveat: `LCD_DARKEN_BIT` stops working

`odroid_overlay_darken_all()` ORs `0x20` into every pixel. Under a full-range
256-colour palette that maps a pixel to an **unrelated guest colour** rather
than to a dimmed one, so **retro-go's menu backdrop shifts colour instead of
dimming** while a 256-colour mode is up. The menu box and its text are drawn
*after* the darken pass and are unaffected, so the menu stays readable.

This is agreed and documented rather than solved. `gnw-doom` tried to fix it
with a bit-5-invariant palette; that collapses the palette to 128 entries and
mangles about half the colours. **Do not repeat it.**

The core hands the table back automatically: leaving 13h pushes the 32-entry
`GW_LCD_SET_CLUT` path again, and `tests/test_video.py` asserts exactly that —
`x86_video_clut256()` must return NULL outside a 256-colour mode, or the
firmware would keep a core-owned table that the overlay then draws through.

## 2. What the core does

| Piece | Where |
|---|---|
| Mode table entry `{ 0x13, VM_VGA8, 40 }`, refusal when unbacked | `x86_video.c`, `vmodes[]` / `vmode_find()` |
| The blit — 200 rows of `memcpy` into the letterbox | `x86_video.c`, `vga13_blit()` |
| DAC (ports `0x3C6`-`0x3C9`), 6-bit storage, 6→8 expansion | `x86_video.c`, `x86_vga_dac_*` |
| Port dispatch | `x86_io.c` |
| `INT 10h AH=10h` AL=`10h`/`12h`/`15h`/`17h` | `x86_bios.c` — **calls the same DAC**, so there is one palette model, not two |
| 256-entry CLUT push / withdraw | `src/main.c`, the frame loop |

Three things are worth stating because each is a failure that hides:

- **The DAC cannot be a port latch.** `0x3C9` is a *stream*: a game writes the
  index once to `0x3C8` and then 768 consecutive bytes, three per entry,
  against a counter that auto-increments after every third. Modelled as "the
  last value written is the state", 767 of those 768 are lost and the entire
  screen comes out one colour — while the pixel *indices* stay perfect, so a
  blit test cannot see it. `tests/test_video.py` streams the palette through
  the real ports for this reason.
- **Values are stored as the guest wrote them**, 6 bits per channel, because
  read-modify-write fade loops read the DAC back. The consumer expands with
  `(v << 2) | (v >> 4)`, so 63 maps to 255 rather than 252 — a plain `<< 2`
  leaves every white 1.2% grey and every fade-to-white short of it.
- **Nothing is added to the store path.** The aperture is linear, so the blit
  reads it once per frame. That is the whole reason this mode is cheap and the
  planar modes are not, and it is why Mode Y is not in this commit (§4).

`x86_vga13_ok` is the witness: mode 13h with no aperture is **refused and
logged** like any other unrenderable mode (event tag 5), rather than adopted so
the guest can draw into the open-bus filler.

## 3. Memory — what moved, and the belief that was wrong

Three allocations changed homes. Two of them were justified by a claim about
the MPU that **is not true of this firmware**.

`src/main.c` used to say the LCD bonus pool is uncached, and kept VRAM out of
it on that basis. Read out of the firmware source instead:
`mpu_set_lcd_pool_uncached_range()` covers **exactly the framebuffer
footprint** — 154 KB in LUT8 — and its own comment says everything past it
"defaults to Normal cacheable, which is what we want for the LCD bonus area".
The uncached window is the two framebuffers. **The bonus area above them is
cached AXI, the same memory class as RAM_EMU.**

That matters because
`references/8086tiny/docs/video/12-vga13-direct-scanout.md` prices emulated
VRAM behind a strongly-ordered region at ~5.4-6.5 cycles per guest **byte
store** — 4x the blit it would save. Had the old belief been true, putting the
13h aperture there would have been actively wrong rather than a trade.

### The bonus pool, now one map in one place

153,600 bytes, carved by fixed offsets (`BONUS_*_OFF` in `src/main.c`). It is
not an allocator and does not need to be: every consumer is known at build time
and none is ever freed. The offsets used to be open-coded `4096`/`8192` at four
call sites.

| Offset | Bytes | What |
|---|---|---|
| 0 | 4,096 | open-bus filler, aliased over 0xA0000-0xFFFFF |
| 4 K | 4,096 | BIOS identification page at 0xFF000 |
| 8 K | 65,536 | **VGA mode 13h aperture at 0xA0000** |
| 72 K | 2,048 | mode 13h host palette (768 B DAC + 1 KB ARGB CLUT) |
| 74 K | 32,768 | **CGA VRAM at 0xB8000** (moved out of RAM_EMU) |
| | **108,544 used, 45,056 free** | |

Free space is deliberately left **contiguous at the top**: the next thing that
wants it is a VRAM arena that can balloon for an EGA mode and shrink again.

64 KB and not 64,000: `0xA0000-0xAFFFF` is sixteen whole 4 KB fold pages, so the
mapping needs no partial page and no special case against the filler aliases.
The aperture is mapped **always**, not on the mode set, because a guest may
write pixels before it switches.

### The sector cache moved to the AHB heap

The `.dsk` window cache (4 x 16 KB) is now `GW_MEM_AHB`, not the bonus pool.
This is **not** primarily about making room:

- It is a **disk staging buffer**, and AHB is D2 SRAM — the only SRAM DMA1/DMA2
  can reach (they cannot touch DTCM). That is where it has to live the day
  SPI1's polled `HAL_SPI_TransmitReceive` becomes a DMA transfer, which is the
  asynchronous-completion fix `STATUS.md` names as the deeper answer to the
  stutter.
- `GW_MEM_AHB` is the **only ABI pool with a real `free()`**. ITC/RAM/DTC are
  bump allocators that can never hand anything back.

Measured on the target: it lands at `0x30005DF8`, inside AHB SRAM
(`_heap_start = 0x30005DC0`, `_heap_end = 0x3001E000`, so ~98 KB of heap
exists), `x86_disk_cache_ok = 1`, and TOPBENCH's **115 guest reads → 7 backend
calls** is unchanged by the move.

Its free space is the one number that **cannot be read off a link map**,
because the AHB heap is shared with the firmware's own newlib allocations.
`disk_cache_mem` publishes the pointer for that reason.

### RAM_EMU

Mode 13h cost 2,896 bytes of core **text** (its data is all caller-owned), which
took headroom from +872 to **-2,024 bytes** — i.e. `guest_ram` would have
returned NULL, `core_ready` would have stayed 0 and the emulator would simply
never have started, with no message. Moving the 32 KB of CGA VRAM to the bonus
pool took it to **+30,728 bytes**.

Measure it after any change to the core:
`arm-none-eabi-nm build/core/dos_core.elf | grep __CORE_BSS_END__`.

## 4. Mode Y (unchained 13h) and EGA 0Dh — BUILT

Wolfenstein 3-D runs in Mode Y, not chained 13h: Sequencer index 4 bit 3
(Chain-4) cleared, four planes at 80 bytes per scanline, page flips through the
CRTC start address at `0x0C`/`0x0D`. It is now implemented, together with the
0Dh planar path it shares a write pipeline with.

This section previously listed three missing subsystems. All three exist:

| was missing | is now |
|---|---|
| a store-side hook | `VGA_ST` in `cpu_exec.S` driving `x86_vga_planar_store()`, plus the aperture's FOLD TRAP for the read side |
| a Sequencer/CRTC register file with effects | `x86_io.c`'s register file, which was already a faithful store, now feeds `x86_video_planar.c` through `x86_video_planar.h`. Nothing is duplicated |
| page storage that does not exist | it still does not exist. Pages are FOLDED. See "what that costs" below |

The de-interleaving blit is nearly free, as this section always said:
`4*(y*80 + (x>>2)) + (x&3)` is exactly `320*y + x`, so at the standard geometry
the correct de-interleave *is* the linear read. `vga13_blit()` gained only a
per-row source offset; the chained path is byte-identical and
`tests/test_video.py` asserts that.

### Where the code is

| what | where |
|---|---|
| the adapter: cell lookup, latches, four write modes, the page fold | `x86_video_planar.c` |
| the interface, and the rule that no state is held twice | `x86_video_planar.h` |
| register writes with an effect; the word-`OUT` split | `x86_io.c` |
| the blits, the arena claims, the mode table | `x86_video.c` |
| present-on-page-flip | `src/main.c`, `x86_video_page_flip()` |
| the tier-1 evidence | `tests/test_video.py`, `scripts/run_planar_controls.py` |

### Presentation: WHEN the screen is painted — a PORT, after four inventions

This is `references/dos_port_prior/dos_video.c:1529-1656` transcribed, and
`references/8086tiny/docs/video/15-mode-y.md` §4 item 4 is its rationale. The
whole model:

* **The guest's page flip presents the screen.** A CRTC display-start change is
  the guest saying "the page I was displaying is complete", and with every page
  aliased into one page of storage that is the only instant it is known good.
  `x86_vga_planar_start_write()` (`x86_video_planar.c:~700`) calls
  `x86_video_page_flip()`, weak in the core and overridden by the frame loop
  (`src/main.c:1635`) — the same weak-symbol shape `dos_video.c` uses.
* **BOTH LCD buffers are painted there** (`modey_paint_both()`,
  `src/main.c:1610`). Not defensive: the frame loop swaps unconditionally, so a
  one-buffer paint makes the panel alternate between this frame and the last.
  `dos_video.c:1544-1545` paints both for the same reason, and `dos_osk.c`
  documents the same failure for its letterbox bars.
* **The frame after a flip the loop paints nothing**, and the idle count
  restarts.
* **After `X86_MODEY_IDLE_FRAMES` (8) frames with no flip the hold is
  ABANDONED** and the loop paints every frame, exactly as every other mode does.
  ~130 ms at 60 Hz. This is `DOS_MODEY_IDLE_FRAMES` and it is an **anti-freeze
  escape hatch and nothing else**.

**What this deliberately does not do**, and it is the point: it makes no attempt
to infer whether the guest has finished drawing. **Nothing observable separates
"quiet because the frame is finished" from "quiet in the middle of a frame."**
A title slower than 8 loop frames per flip therefore **tears** — the reference's
own stated, accepted trade (`15-mode-y.md` §4: "the visible artefact is
TEARING"). `x86_modey_mid_draw` / `_max` count it rather than pretending it is
gone.

`-DX86_MODEY_PRESENT_ON_FLIP=0` is the reference's `-DDOS_MODEY_PRESENT_ON_FLIP
=0`: no hold, paint every frame, the pure tearing model. It is the FLIP arm of
`scripts/run_planar_controls.py`.

#### What this replaced, and why the record is kept

Four rounds of **local invention** stood here, none of them in the reference:

| round | rule | what it fixed | what it broke |
|---|---|---|---|
| 1 | an 8-frame timer | nothing (it *was* the artefact) | blitted mid-redraw |
| 2 | "a quiet page is a finished page", + a 60-frame staleness bound | the menus | gameplay: a raycaster is never quiet, so only the bound fired, mid-redraw |
| 3 | a doubling back-off on both | gameplay | the intro froze ~0.5 s — "it freezes the image and then jumps to where you tried to get to" |
| 4 | a measured in-frame-gap window, with `modey_flipper` decay | the intro | **visible flickering on hardware**; reverted at `a465f09` |

Each round fixed the previous round's bug and introduced another, because each
was built on the inference the reference explicitly does not make. The ported
policy is **328 bytes of `RAM_EMU` smaller and 612 bytes of text smaller** than
round 4, and `x86_video_planar.c` lost ~420 lines.

Gone with them: `x86_modey_stale_paints`, `x86_modey_inframe_gap`, and the
`TIMER` / `QUIET` / `BACKOFF` negative controls, which were controls for those
inventions.

#### How it is graded

`tests/test_video.py --present-only` drives `x86_video_modey_paint_due()`
directly against modelled guests — no LCD, no CPU, no timing — because "which
frames are painted" is not a pixel question: the suite was at 64000/64000 with
every one of the four defects above shipping. Five arms:

| arm | guest | asserted |
|---|---|---|
| 1 | flips every 20 frames, stores in every one | **0** paints inside the 8-frame hold; exactly 118/200 loop paints (19 before the first flip + 9 x 11); 99 counted as mid-draw — the tearing, measured |
| 4 | flips every 5 frames — faster than the hold | the loop paints **exactly never**; this is what `PRESENT_ON_FLIP=0` fails |
| 6 | a level, then a guest that stops flipping and draws an intro in pieces | first reveal within **9** frames, and 52 of the next 60 painted — an intro is revealed *as it draws* |
| 2 | a menu: 4 frames drawing, then quiet, never flips | painted on all 4 drawing frames and all 30 quiet ones; **no state accumulates** to suppress a later screen |
| 3 | stores forever, never flips | 180/180 — not a frozen screen |

Arm 2's second half is not an efficiency question. It is the assertion that
nothing is learned and carried forward, which is exactly how each invented rule
regressed the one before it.

#### The witnesses

`x86_vga_planar_lost` used to lump three unrelated discards into one number, and
**18,944 measured on hardware could not be attributed to any of them**. It is
split now, because the reference's account of this region is a *per-region*
measurement and one lump sum cannot be compared with it:

| counter | what |
|---|---|
| `x86_vga_modey_lost_cache` | a store at or above `0xE300` — past the 8,192 off-screen offsets that exist. The reference measured this region as **cold and one-off** and a 48 KB cache gave a byte-identical frame, so a number here that keeps **climbing during gameplay** contradicts the reference and is the first thing to look at |
| `x86_vga_modey_lost_tail` | a page's rows 205+ (offsets 16,384..stride-1 of each of three pages): 768 per full pass, never displayed by construction |
| `x86_vga_modey_lost_nocache` | the arena **refused** the cache block, so the whole `0xC300..0xE2FF` region is being thrown away. A configuration failure, silent without this counter |
| `x86_vga_modey_cache_offs` | plane offsets of cache actually claimed — `0x2000` when live, **0** when refused. The one read that settles "is the `0xC300` cache genuinely there" |

### The arena arithmetic that actually held

Modes are mutually exclusive, so the peak demand is the largest mode and not the
sum. Against a 143,360-byte arena with CGA (32,768) and the 13h aperture
(65,536) held, 45,056 bytes are free, and one 32,768-byte block serves either
planar mode — 12,288 to spare, whichever is up:

* **Mode Y**: the 64 KB aperture *is* the plane storage (one aliased page,
  16,384 plane offsets x 4 = 65,536). The arena block is the OFF-SCREEN cache at
  `0xC300`, 8,192 plane offsets, claimed under `X86_ARENA_ID_SPARE`.
* **0Dh**: the aperture's bottom 16 KB is the CPU read shadow, and the arena
  block is the planes — 8,192 offsets, a 32 KB card, under `X86_ARENA_ID_EGA`.

`vmode_find()` gates 0Dh on `x86_arena_would_fit()`, so the mode is REFUSED
before it is adopted rather than accepted and not rendered, and the refusal is
loud: `x86_video_mode_log` tag 5 plus `x86_vga_ega_refused`, with
`x86_arena_deficit` holding the byte count.

### The aperture is a FOLD TRAP, not a range check (Queue B3)

The read side used to be `VGA_LATCH`: `LDR cpu->vga_base` + `SUBS` + `CMP` +
`IT/STRLO`, five instructions at **47 load sites**, executed by every title
whether a planar mode was armed or not. It is gone. While a planar mode is
armed the sixteen fold entries covering `0xA0000`-`0xAFFFF` hold
`X86_FOLD_VGA_K` = `0x80000000 | host - 0xA0000`, so an aperture access comes
out NEGATIVE and trips the `ADDS`/`BMI` that every guest access already runs.
A non-planar title now pays **nothing** for the aperture.

The three trap causes stay separable by ONE unsigned compare with no scratch
register and no table load, because their encodings are ordered: fold miss
`0x80000000+lin` (lin < 16 MB), NOR `0x9xxxxxxx`, aperture `>= 0xA0000000`
(host is `0x2xxxxxxx`, so the tag lands in `0xA0000000`-`0xBFFFFFFF`). Arming
refuses the sentinel rather than installing a colliding one if the aperture is
not in AXI/DTCM (`X86_FOLD_VGA_OK`), and `x86_vga_fold_trap_refused` says so.

**The store side is UNCHANGED and that is not an oversight.** The trap fires at
pointer RESOLUTION, where neither the stored value nor the operand size exists;
`VGA_ST` is per-site and has both at assembly time. The trap stub therefore
hands the site the untagged pointer and `VGA_ST` completes the store exactly as
before. A read-modify-write site asks for `MEM_GET_PTR_RMW`, which is the write
trap *with* the latch record — a pure store must never record the latch, or the
`mov al,[si] / mov [di],al` copy would latch from its own destination.

**Two things this cost, both found by running it:**

1. **Every C-side walk of the fold table must strip the tag.** `x86_fold_ptr()`
   in `x86_cpu.h` is that walk. The pager's slack prime
   (`x86_pager.c`, "prime the slack from the neighbour") dereferences the
   NEIGHBOUR's fold constant, so faulting in guest page `0x9F` dereferenced
   `0xA4xxxxxx` and **hard-faulted Wolfenstein 3-D**, with the tier-1 suite
   entirely green. The seam's neighbour pull and push-back are the same shape.
2. **`x86_map_fixed()` over `0xA0000` silently disarms the trap**, because the
   trap IS a fold entry: a mode set or an arena re-claim maps the aperture
   afresh. It calls `x86_vga_planar_fold_refresh()` now.

Cost: **+608 instructions / +2,184 B of ITCM** (48,016 -> 50,200), all of it in
out-of-line stubs that only an aperture access executes, against five
instructions removed from the inline path of every guest load in the
interpreter. Negative control: `-DX86_VGA_FOLD_TRAP=0` never installs the
sentinel; `tests/test_video.py`'s Mode Y arm then reports `latch=6000/6000`
misses and fails with 16,000 of 64,000 pixels wrong in the latch-copy band.

### What it does NOT yet buy, and what would

Queue B4 wants the 64 KB aperture back: in 0Dh the aperture is only a **read
shadow** (its bottom 16 KB) and the planes live in the arena, so if a read were
SERVED BY THE TRAP -- `ega_read_byte()` into an eight-byte staging buffer,
which is the widest thing one folded pointer is ever read through -- the 65,536
bytes would be free for the EGA card. That is 77,824 + 65,536 = **143,360 B =
35,840 plane offsets**, against the 28,160 (start 15,360 + 200 rows x 64)
Keen 4's intro needs. **Keen's intro would fit, with 7,680 offsets to spare.**
The work is per-site: the trap must own the LOAD (its width is a property of
the site), which is the same fusion the store side needs and the same reason it
has not happened yet -- roughly half the store sites are shared tails where the
effective address is already dead. Mode Y is NOT affected: there the aperture
IS the plane storage.

### Two improvements over the reference, both from the hook's shape

1. **Latches are exact for PLAIN reads**, not only for read-modify-writes. The
   reference is a C interpreter whose read path never surfaced a plain load, so
   it could only latch from a store's destination. The aperture's FOLD TRAP
   records the last guest read in `cpu->vga_latch` at every load in the
   interpreter, so `mov al,[si] / mov [di],al` — the write-mode-1 block copy
   that is most of the reason to unchain at all — latches from SI, which is where
   the hardware latches from. Measured on Wolf3D: 107,721 latch reloads, **zero**
   misses through the setup and title screens.
2. **No save/restore dance.** `15-mode-y.md` §5 describes saving the aperture
   bytes a raw store is about to destroy and putting them back, because in Mode Y
   the shadow address IS plane storage. Our hook REPLACES the store, so the raw
   byte is never written and `dos_ega_wr_save[]` has no analogue here. Do not
   reintroduce it.

### What the fold costs — carried forward verbatim, because these are the record

A Mode Y page is still 64,000 bytes. Unchaining does not shrink a page, it makes
three more of them addressable. Two real pages is 195,072 B; a full card is
262,144 B. So every plane offset is folded into one page's worth of storage, and:

* **A page flip is a no-op; pages alias. The artefact is TEARING** — against the
  four-copy stacked-frame garbage a chained blit produces. Every Mode Y title
  redraws the whole frame each pass, which is why it double-buffers.
* **Data reads return the wrong plane in Mode Y.** There is no room for a read
  shadow, because the planes *are* the aperture, so a guest read of `A000:a`
  returns `aperture[a]`, i.e. plane `a&3` at plane offset `a>>2`. **Latch loads
  stay exact**, and latch copies are the idiom that matters; a title that reads
  pixels back for collision detection would not be served.
* **`0xE300` upward is discarded.** Measured on the reference: 2,688 of 59,616
  cache latch loads, all in one cold one-off region, and rebuilding with a 48 KB
  cache produced a **byte-identical frame**. 8,192 offsets is sufficient, not a
  compromise.
* **Do not fix the aliasing without bringing ~192 KB with you.** The sums above
  are the record that stops that being re-proposed.
* **Commander Keen 4's level backgrounds need a 256 KB card and are NOT fixable
  in this device's memory.** Its write high-water is plane offset 65,536, 97.2%
  of its latch copies source from beyond an 8 KB plane, and nothing between 8 KB
  and 64 KB per plane buys a partial picture — at 16 KB the frame is *worse* than
  at 8 KB. **0Dh gets Keen's banner and its menus, not its levels.**

Two further limits are ours rather than the reference's:

* **The default stride is a guess.** 16,640 (80 x 208) is what the one Mode Y
  title we have uses, and learning is too late for the furniture a title paints
  before its first flip. A title spacing pages 16,384 apart is displaced until it
  flips. The durable fix is a per-title `.dosmeta` hint.
* **The off-screen base assumes THREE pages.** A title that double-buffers and
  caches two pages up has its cache aliased onto page 3's storage. Nothing
  observable states the page count.

## 5. What WOLF3D actually does today — measured

It reaches video, it unchains, and it renders. This section used to say the
guest never got past a DOS-layer seek defect and that the screen was "black
before this change and black after it"; that is no longer the state.

Under gwemu, `scripts/probe.py --retrogo`, with the ROM preselected:

| counter | value | what it settles |
|---|---:|---|
| `x86_vga_modey_armed` | 1 | the guest cleared Chain-4 and we followed it |
| `x86_vga_modey_page` | **16,640** | learned from the guest's OWN flips, not assumed |
| `x86_vga_modey_flips` | 2 | CRTC display-start changes seen |
| `x86_modey_presents` | 2 | frames painted on a page flip rather than at a frame boundary |
| `x86_vga_planar_stores` | 136,168 | stores that took the adapter |
| `x86_vga_latch_loads` / `_miss` | 136,168 / 256 | latch reloads, and how many found no cell |
| `x86_vga_planar_lost` | 17,920 | stores past the fitted card, discarded rather than aliased |
| `x86_modey_fallbacks` | 3 | frames the LOOP painted, i.e. the 8-frame anti-freeze hatch firing |
| `x86_modey_flip_gap_max` | 14 | loop frames between two flips. **Above 8**, so this phase runs the hatch, which is correct: WOLF3D's signon and title screens draw straight into the page they are displaying and never flip, and the hatch is their only presentation path |

> These were captured under the round-2 policy. `fallbacks` and `flip_gap_max`
> carry over; `stale_paints` and `inframe_gap` no longer exist.

**16,640 is the whole design confirming itself against the real title.** It is
the id Galaxy engine's `SCREENSIZE`, it is what the reference measured, and our
core arrives at it from nothing but the distance between two display starts the
guest programmed.

`scripts/screenshot.py` through the menus shows the setup screen (the one whose
off-screen tile cache at `0xC300` produced a band of garbage across the top two
thirds of the reference's screen before that region got its own storage), the
title screen, the Options menu and the difficulty screens, all rendering
correctly. Gameplay itself is past the 60 s timeline cap at gwemu's ~3.0 M
insn/s — the level load does not finish inside it. That is a speed observation,
not a rendering one, and the counters above are the evidence that the pipeline
is live.

**gwemu is still not a witness for what the picture looks like on silicon**
(`15-mode-y.md` says the same, from the other direction — the reference saw a
black Wolf3D under gwemu with unmodified baseline sources). The pixel evidence
is `tests/test_video.py`, which compares all 64,000 pixels for chained 13h, for
Mode Y and for 0Dh against expectations computed host-side from the pattern, and
`scripts/run_planar_controls.py`, which requires four negative controls to fail
and the stride learning to work from a deliberately wrong default.
