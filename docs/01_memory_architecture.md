# Memory Architecture & Paging Strategy

## 1. System Constraints (STM32H7B0VB)
- **AXI SRAM**: 870KB total. (32KB reserved for VRAM, leaving ~840KB for guest RAM).
- **ITCM**: 64KB (Instruction Tightly Coupled Memory) - 0 wait states.
- **DTCM**: 128KB (Data Tightly Coupled Memory) - 0 wait states.
- **AHB SRAM**: Used as DMA buffers for SD card I/O.
- **NOR Flash**: Quad-SPI (QSPI), memory-mapped, read-only Execution-In-Place (XIP) capable.
- **Guest RAM Target**: Up to 8MB.

## 2. Page Directory & DTCM

**This section described a design that was never built, and the thing that WAS
built is better. Corrected against the code (`x86_cpu.h:447-450`, `cpu_exec.S`).**

The translation is the **fold table**, not a PTE table. One 32-bit entry per
4 KB granule holds `k = host - (index << 12)`, so translation is
`host = lin + k` — an add, with no shift, no mask and no field extraction. An
unmapped page holds `FOLD_INVALID = 0x80000000`, which drives bit 31 of the
result high, so `ADDS` + `BMI` validates every access as a side effect of
computing it. There are no permission bits to test on the fast path because
there is no fast-path test at all.

`FOLD_ENTRIES` is **4096, not 2048**: a **16 MB** window in **16 KB** of DTCM,
allocated at runtime through `mem_ctl(GW_MEM_DTC, …)`. The 8 MB guest space is
half of what already exists, so **it costs no new table and no fast-path
change**. The "8 KB / 2048 entries" figure this section used to give was a
plan, not a measurement, and it understated what was shipped.

Per-page state that does NOT fit in a fold entry — dirty, pinned, hotness,
backing tier, pagefile slot — lives in **side arrays indexed by the same page
number**, also in DTCM. Keeping them out of the fold entry is what preserves
the one-add translation; nothing on the fast path ever reads them, because a
non-resident page is already a bit-31 fold value and the trap does the reading.

### The backing tiers
A non-resident page is in exactly one of three places, and the distinction is
what keeps the SD card out of the common case:
- **ZERO** — never written. Costs nothing anywhere. Most of an 8 MB space.
- **NOR** — a clean page of the boot image, read in place over XIP. Zero AXI,
  zero SD. Already built (`x86_nor.c`).
- **PAGEFILE** — a dirty page on the SD card. The expensive tier, entered only
  by a page that was modified and then evicted.

## 3. The Copy-On-Write (COW) Paging Strategy
At boot, the guest OS snapshot is stored entirely in the memory-mapped NOR Flash. 
All 2048 Page Table entries are initialized to point to their respective NOR Flash offsets, with `LOCATION = 0` and `DIRTY = 0`.

### Read Operations
1. CPU attempts to read a guest address.
2. Emulator fetches the PTE from DTCM.
3. If `LOCATION == 0`, the emulator reads directly from the memory-mapped NOR Flash (XIP). 
4. If `LOCATION == 1`, it reads from AXI RAM.
5. If `SD_PAGED == 1`, it triggers a page fault to swap the page back in from the SD card.

### Write Operations (COW)
1. CPU attempts to write to a guest address.
2. Emulator fetches the PTE.
3. If `LOCATION == 0` (pointing to read-only NOR flash):
   - A **Page Fault** is triggered.
   - The memory controller allocates a free 4KB page from the ~840KB AXI pool.
   - The 4KB chunk is copied from NOR Flash into the new AXI page.
   - The write operation is performed on the AXI page.
   - The PTE is updated: `Host Address` updated to AXI, `LOCATION = 1`, `DIRTY = 1`.

## 4. AXI Pool Management & SD Card Swapping

**Measured 2026-08-14, this tree.** The "~840 KB / 210 pages" above is a budget
from before the core existed; the core's own text and BSS come out of the same
region.

| | bytes | 4 KB pages |
|---|---|---|
| RAM_EMU `0x2404B000`–`0x24100000` | 741,376 | |
| free past `__CORE_BSS_END__` (`0x240596E8`), page-aligned | 679,936 | **166** |
| bonus-pool arena (LUT8 release, after the map was realigned) | 45,056 | **11** |
| **resident set** | **724,992** | **177** |

That is the whole of it: **177 pages resident of a 2048-page space, 8.6%.**
Pinned claims (VRAM, the mode 13h aperture, EGA planes) come out of it first
and at top priority, so the guest-RAM resident set is what is left after the
display has been served — 135 pages while a 256 KB EGA mode is up.

**No LRU list, and that is a decision rather than an omission.** The prior
project measured eviction policy as worth at most 2.1x while the pool-SIZE
cliff was 350x (48 → 64 pages). A doubly-linked list costs 8 bytes per page in
DTCM and touching it on every access costs the fast path — which is precisely
the wrong place to spend. Hotness comes from a **SysTick sampler reading the
guest EIP out of R10** (free: the hardware does not stack R10) plus fault-driven
counts, both of which cost nothing per access.

On eviction:
1. The coldest unpinned page with no live host pointer into it is chosen.
2. **Clean pages are DROPPED**, not written — re-read from NOR or refilled with
   zeroes. The prior project measured 566 MB of pagefile traffic and 98.6%
   wasted writes for want of this, and it is the single most important rule here.
3. Only a **dirty** page reaches the pagefile.

**The pagefile is expensive in a way that shapes the design.** The ABI's only
write path is `fopen`/`fseek`/`fwrite`/`fclose` per call, synchronous, with the
firmware's `fseek` walking the cluster chain (`FF_USE_FASTSEEK 0`). There is no
ranged write, no DMA to SPI1 and no asynchronous completion reachable from a
core, so the DMA pipelining this section used to promise **cannot be built from
here**. What can be, and is: never write a clean page, and prefer contiguous
runs of victims so one `fopen` carries several pages.

## 4a. A20, and the wrap that used to eat the address space

`x86_mem_wrap_1mb()` aliased **every** fold entry from 1 MB to 16 MB onto the
low megabyte. That is an 8086's 20-bit address space, and the consequence is
bigger than an inaccuracy: with all 3,840 entries above 1 MB held as aliases
there **is no memory above 1 MB**, and an 8 MB guest space cannot exist. It was
invisible because nothing had ever mapped anything up there.

What a real AT wraps is one 64 KB window — segment:offset reaches at most
`0x10FFEF` — and it wraps it only while the **A20 gate is masked**. So the wrap
is now exactly sixteen pages (`X86_HMA_FIRST_PAGE`, `X86_HMA_PAGES`) and
conditional on `x86_io.a20_open`. Everything from `0x110000` up is ordinary
extended memory and is never aliased.

A20 boots **masked**, which is what an AT does and what preserves the behaviour
every guest in this tree was tested against. Three doors move it, because
guests use all three: the 8042 output port (`0x64` command `D1`, then the byte
to `0x60`, bit 1), the 8042's `DD`/`DF` shortcuts, and the fast gate at port
`0x92` bit 1. `0x64` command `D0` reads the port back — and its answer arrives
on `0x60`, which is the detail that makes a read-modify-write sequence work
instead of writing a scancode into the output port. Port `0x92` bit 0 is fast
RESET and is deliberately **not** modelled as a reset.

With the gate open the HMA is left **unmapped** rather than aliased: nothing
owns it yet (there is no XMS driver), so a guest that opens A20 and reads up
there takes a clean fault instead of silently reading the interrupt vector
table and believing it. `tests/test_paging.py` covers all of it, including the
two arms — `0x110000` and `0x200000` not aliased — that fail on the old wrap.

## 5. Physical Memory Mapping (Linker Script)
To implement this architecture, we define the exact physical boundaries of the STM32H7B0VB in our linker script. Crucially, while the AXI RAM is 1024KB total, we reclaim unused RGB565 framebuffer space (reserving only 154KB for LUT8 mode), giving the emulator an expanded ~870KB AXI pool.
```ld
MEMORY {
  ITCM (rx)     : ORIGIN = 0x00000000, LENGTH = 64K
  DTCM (rwx)    : ORIGIN = 0x20000000, LENGTH = 128K
  AXI  (rwx)    : ORIGIN = 0x24000000, LENGTH = 870K   /* 1024K - 154K (LUT8 framebuffer) */
  INTFLASH (rx) : ORIGIN = 0x08000000, LENGTH = 1024K
  EXTFLASH (rx) : ORIGIN = 0x90000000, LENGTH = 64M
}
```
*Note: To ensure seamless integration with the `retro-go` framework, we mirror its `Makefile.common` variable naming conventions (`EXTFLASH_SIZE_MB`, `EXTFLASH_OFFSET`, `INTFLASH_BANK`).*

## 6. Page Table Implementation
The 8KB flat page table must strictly reside in DTCM to guarantee 0-wait-state lookups. This is enforced via GCC section attributes tied to the linker script.
```c
// 2048 entries of 32-bits (8MB / 4KB) = 8192 bytes.
// Forced into the DTCM via linker section.
uint32_t guest_page_table[2048] __attribute__((section(".dtcm_bss")));
```
