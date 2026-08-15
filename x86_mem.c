/*
 * Guest physical memory: a page pool and the fold-table mapping over it.
 *
 * This file owns the MAPPING -- fixed mappings, guest<->host copies, the A20
 * gate and the real-mode wrap. The PAGER is x86_pager.c and the display's AXI
 * is x86_arena.c; the split is deliberate, because "which host memory does
 * this guest address name" and "which guest pages are resident" fail in
 * different ways and want different counters.
 *
 * The one place they meet is x86_guest_ptr(), which resolves the fold itself
 * and therefore never touches the interpreter's trap stubs. It has to fault a
 * non-resident page in, or DOS would be unable to load a program: loading is a
 * copy into memory the guest has never touched, and under demand paging every
 * one of those pages is non-resident.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_cpu.h"
#include "x86_arena.h"
#include "x86_pager.h"

#ifndef X86_HMA_NEGCTL
#define X86_HMA_NEGCTL 0
#endif
#include "x86_video_planar.h"

#define PAGE_SIZE 4096u

/* Host-readable: any mapping we refused. Non-zero means some region is
   deliberately absent, which is a far better thing to read than a hardfault. */
volatile uint32_t x86_map_failures = 0;

static uint8_t *pool_base;
static uint32_t pool_bytes;
static uint32_t pool_used;

void x86_mem_pool_init(void *base, uint32_t bytes)
{
    pool_base = (uint8_t *)base;
    pool_bytes = bytes & ~(PAGE_SIZE - 1u);
    pool_used = 0;
}

uint32_t x86_mem_pool_free(void)
{
    return pool_bytes - pool_used;
}

static uint8_t *pool_alloc_page(void)
{
    if (pool_used + PAGE_SIZE > pool_bytes)
        return NULL;                 /* caller reports it; never silent */
    uint8_t *p = pool_base + pool_used;
    pool_used += PAGE_SIZE;
    return p;
}

/*
 * Map [guest, guest+bytes) to freshly allocated, zeroed pages.
 *
 * Returns 0 on success, or the number of pages it could not place. Exhaustion
 * is reported rather than swallowed: a pool that quietly stops mapping behaves
 * exactly like one that works until the guest touches the gap, and that is the
 * single most expensive class of bug this project has hit.
 */
int x86_map_range(uint32_t guest, uint32_t bytes)
{
    uint32_t first = guest >> FOLD_SHIFT;
    uint32_t count = (bytes + PAGE_SIZE - 1u) / PAGE_SIZE;
    int failed = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = first + i;
        if (idx >= FOLD_ENTRIES) {
            failed++;
            continue;
        }
        uint8_t *page = pool_alloc_page();
        if (!page) {
            failed++;
            x86_map_failures++;
            continue;
        }
        memset(page, 0, PAGE_SIZE);
        guest_page_table_ptr[idx] = FOLD_K(page, idx);
    }
    return failed;
}

/* Point a guest range at memory we already have -- the LCD-facing framebuffer,
   or a ROM image -- without taking pool pages for it. */
int x86_map_fixed(uint32_t guest, void *host, uint32_t bytes)
{
    uint32_t first = guest >> FOLD_SHIFT;
    uint32_t count = (bytes + PAGE_SIZE - 1u) / PAGE_SIZE;

    /* REFUSE A NULL BACKING. A fold entry built from NULL is worse than no
     * entry at all: FOLD_K(NULL, idx) yields a small positive host address, so
     * the sentinel check (ADDS/BMI) PASSES and the handler dereferences it --
     * a HARD FAULT inside the interpreter instead of a clean X86_EXIT_MEM_FAULT
     * the emulator can report. That is how a failed VRAM/filler allocation
     * surfaced as a hardfault at GRP5_MEM_16's `ldrh r3,[r2]`, with nothing
     * anywhere saying an allocation had failed.
     * Leaving the range unmapped keeps FOLD_INVALID, so the guest takes a
     * clean fault and x86_map_failures says why. */
    if (!host) {
        x86_map_failures++;
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = first + i;
        if (idx >= FOLD_ENTRIES)
            return -1;
        guest_page_table_ptr[idx] = FOLD_K((uint8_t *)host + i * PAGE_SIZE, idx);
    }
    /* A plain mapping over the VGA aperture DISARMS the fold trap, because the
       trap IS a fold entry. The store hook would stay armed and the read latch
       would quietly stop being recorded -- so ask the planar layer to put its
       sentinel back. Costs a call and a test on a path that runs at mode
       changes, not on any hot path. */
    if (guest < 0xB0000u && guest + bytes > 0xA0000u)
        x86_vga_planar_fold_refresh();
    return 0;
}

/*
 * Guest linear -> host pointer, for host-side code servicing an HLE call.
 *
 * Goes through the same fold table the interpreter uses, so an unmapped page
 * is reported rather than silently dereferenced, and a request that would
 * straddle out of its granule is refused: the fold answers for one 4 KB page,
 * and a service copying a filename or a disk buffer can easily cross one. The
 * caller must split or use x86_guest_copy.
 */
void *x86_guest_ptr(uint32_t lin, uint32_t bytes)
{
    if (bytes == 0)
        return NULL;
    uint32_t idx = lin >> FOLD_SHIFT;
    if (idx >= FOLD_ENTRIES)
        return NULL;
    if (((lin + bytes - 1u) >> FOLD_SHIFT) != idx)
        return NULL;                 /* would cross a granule */

    uint32_t k = guest_page_table_ptr[idx];
    if (k == FOLD_INVALID) {
        /* HOST-SIDE DEMAND FAULT. The interpreter reaches the pager through its
         * trap stubs; everything on this side -- the BDA tick, INT 21h filling
         * a DTA, a disk read landing in a guest buffer -- arrives here, and
         * returning NULL would make a non-resident page indistinguishable from
         * an unmapped one. That is not a theoretical difference: DOS loads a
         * program by copying into memory the guest has never touched, so with
         * demand paging every one of those pages is non-resident and nothing
         * would load at all. */
        uint32_t host = x86_pager_fault(lin, 1);
        if (!host)
            return NULL;
        x86_pg_faults_host++;
        x86_livemap_note(idx);
        return (void *)host;
    }

    /* A NOR-backed page must be pulled back into AXI BEFORE the pointer
     * escapes, because the caller may write through it.
     *
     * This resolves the fold itself and so never goes near the interpreter's
     * trap stubs. Every HLE service that writes into guest memory arrives here
     * -- the BDA tick, INT 21h filling a DTA, a disk read landing in a guest
     * buffer -- and without this those stores would go straight at the XIP
     * window. Under gwemu that appears to work, because its OCTOSPI model
     * backs the region with ordinary memory; on silicon the store is dropped
     * and the guest diverges with nothing anywhere saying why. It was found by
     * reading x86_nor_pages_axi = 1 after 25 s of a real title, which is far
     * fewer written pages than any DOS guest can plausibly have.
     *
     * It COWs on read as well as on write, deliberately: this is the same
     * conservative rule the unqualified MEM_GET_PTR follows, and for the same
     * reason -- pessimistic is a cost, wrong is a corrupted guest. The host
     * side is a few thousand calls per second, not millions. */
    /* This pointer is about to escape into host code that may hold it across
       another call that can fault -- x86_guest_copy_in() walks granules and
       each step can pull a page in. Note the page so the pager's victim picker
       cannot choose it. See the live-pointer hazard in x86_arena.h: the
       previous project measured five such evictions producing 26,329 wrong
       bytes, and `cow_lost == 0` was green throughout. */
    x86_livemap_note(idx);

    uint32_t host = lin + k;
    /* The VGA aperture's fold sentinel (x86_cpu.h). A host-side service that
       resolves an aperture address -- an INT 21h read landing in VRAM, a
       screenshot -- wants the memory, not the trap: strip the tag. It is
       checked BEFORE the NOR branch because a tagged pointer would otherwise
       be handed to x86_nor_cow() as an offset into the snapshot.
       Host-side stores through this pointer are RAW, exactly as they were
       before the trap existed: the planar write modes are the interpreter's
       business and no HLE service writes planes. */
    if (X86_FOLD_IS_VGA(host))
        return (void *)X86_FOLD_VGA_HOST(host);
    if (x86_nor_attached && (host & 0x80000000u)) {
        uint32_t axi = x86_nor_cow(host);
        if (!axi)
            return NULL;
        return (void *)axi;
    }
    return (void *)host;
}

/* Copy across granules, one page at a time. Returns bytes actually moved, so
   a short result means the guest range ran into unmapped memory. */
uint32_t x86_guest_copy_out(void *dst, uint32_t lin, uint32_t bytes)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t done = 0;
    while (done < bytes) {
        uint32_t off = (lin + done) & (PAGE_SIZE - 1u);
        uint32_t run = PAGE_SIZE - off;
        if (run > bytes - done)
            run = bytes - done;
        void *p = x86_guest_ptr(lin + done, run);
        if (!p)
            break;
        memcpy(d + done, p, run);
        done += run;
    }
    return done;
}

uint32_t x86_guest_copy_in(uint32_t lin, const void *src, uint32_t bytes)
{
    const uint8_t *s = (const uint8_t *)src;
    uint32_t done = 0;
    while (done < bytes) {
        uint32_t off = (lin + done) & (PAGE_SIZE - 1u);
        uint32_t run = PAGE_SIZE - off;
        if (run > bytes - done)
            run = bytes - done;
        void *p = x86_guest_ptr(lin + done, run);
        if (!p)
            break;
        memcpy(p, s + done, run);
        done += run;
    }
    return done;
}

/*
 * Real-mode address wrap, and the A20 gate that decides whether it happens.
 *
 * WHAT THIS USED TO DO, AND WHY IT WAS WRONG. It aliased every fold entry from
 * 1 MB to 16 MB onto its counterpart in the low 1 MB. That is an 8086's 20-bit
 * address space, not this machine's, and it has a consequence far bigger than
 * an inaccuracy: with all 3,840 entries above 1 MB held as aliases, there is
 * no memory above 1 MB at all, and the 8 MB guest space this project is being
 * built toward cannot exist. It was harmless only because nothing had ever
 * mapped anything up there.
 *
 * What a real AT wraps is ONE 64 KB window. Segment:offset reaches at most
 * 0xFFFF:0xFFFF = 0x10FFEF, so only 0x100000-0x110000 is reachable past the
 * megabyte, and masking A20 forces address bit 20 low so that window reads the
 * bottom of memory. Everything from 0x110000 up is ordinary extended memory
 * and was never part of the wrap.
 *
 * So: sixteen pages, conditional on the gate. A block-copy loop walking
 * segments off the top still comes back round, which is the property programs
 * depend on and the reason this exists.
 */
volatile uint32_t x86_a20_changes = 0;
volatile uint32_t x86_a20_state = 0;

void x86_mem_wrap_1mb(void)
{
    if (x86_io.a20_open) {
        /*
         * THE GATE IS OPEN, SO THE HMA IS ITS OWN MEMORY -- AND IT IS BACKED.
         *
         * It used to be left FOLD_INVALID, on the reasoning that nothing owned
         * it. Something does now: the pager's second-window mechanism, the one
         * EMS already uses for its logical pages at linear 4 MB. Sixteen
         * ordinary pageable guest pages, filled from the ZERO tier on first
         * touch and evictable like any other -- 64 KB of a 640 KB pool, and
         * only if the guest actually touches it.
         *
         * The old comment argued a clean fault was the honest answer. It was
         * honest about the implementation and WRONG about the machine: a 286+
         * with A20 open HAS memory there, and two shipped titles ask. DOS4GW's
         * memory hunt opens A20 itself and reads through ES=0xFFFF; Mortal
         * Kombat's C runtime runs the classic A20 wrap test (REPE CMPSW against
         * FFFF:0030) and requires the two locations to DIFFER. Both got
         * X86_EXIT_MEM_FAULT instead of an answer.
         *
         * WHO OWNS THESE PAGES ACROSS A TOGGLE. The window, and only the
         * window, while the gate is open. Masking it drops the window and
         * writes the alias back over these entries, so a guest that writes the
         * HMA, masks A20 and reads low memory sees the ALIAS -- the frames keep
         * their HMA data untouched behind it, and x86_pager_refold() points the
         * entries back at them when the gate reopens. The pager is told not to
         * write these entries while masked (fold_is_wrapped(), x86_pager.c),
         * which is what stops an eviction from erasing a live alias.
         */
        x86_pager_own_aux(X86_HMA_FIRST_PAGE, X86_HMA_PAGES);
        for (uint32_t i = 0; i < X86_HMA_PAGES; i++) {
            uint32_t dst = X86_HMA_FIRST_PAGE + i;
            if (dst >= FOLD_ENTRIES)
                break;
            /* Resident from an earlier open window -> back to its frame;
               otherwise FOLD_INVALID, which is a demand fault, not a refusal. */
            x86_pager_refold(dst);
        }
        x86_a20_state = 1u;
        return;
    }

#if X86_HMA_NEGCTL == 1
    /* NEGATIVE CONTROL: the window is left registered and the entries are left
       pointing at the HMA's own frames. A guest that masks A20 then reads low
       memory through a wrapped address gets its HMA data instead of the alias,
       which is the silent corruption the gating exists to prevent. */
    for (uint32_t i = 0; i < X86_HMA_PAGES; i++)
        x86_pager_refold(X86_HMA_FIRST_PAGE + i);
    x86_a20_state = 0u;
    return;
#endif
    /* Masked: the wrap owns these entries. Drop the window FIRST so a fault
       inside it cannot hand out a frame while the alias is what the guest
       sees. */
    x86_pager_own_aux(X86_HMA_FIRST_PAGE, 0);
    for (uint32_t i = 0; i < X86_HMA_PAGES; i++) {
        uint32_t dst = X86_HMA_FIRST_PAGE + i;
        if (dst >= FOLD_ENTRIES)
            break;
        uint32_t k = guest_page_table_ptr[i];
        if (k == FOLD_INVALID) {
            guest_page_table_ptr[dst] = FOLD_INVALID;
            continue;
        }
        /* k is host - guest_index<<12, so re-bias it for this index. */
        guest_page_table_ptr[dst] = k - ((dst - i) << FOLD_SHIFT);
    }
    x86_a20_state = x86_io.a20_open ? 1u : 0u;
}

void x86_a20_set(int open)
{
    uint8_t want = open ? 1u : 0u;
    if (x86_io.a20_open == want)
        return;                       /* a probe is not a transition */
    x86_io.a20_open = want;
    x86_a20_changes++;
    x86_mem_wrap_1mb();
}

void x86_unmap_all(void)
{
    for (uint32_t i = 0; i < FOLD_ENTRIES; i++)
        guest_page_table_ptr[i] = FOLD_INVALID;
}
