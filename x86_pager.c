/*
 * Demand paging over the fold table. The design, the tiers and the
 * compare-at-eviction dirty rule are in x86_pager.h; this file is the
 * mechanism.
 *
 * Nothing here runs on the fast path. Every entry point is reached from a
 * trap the interpreter was already taking for an unmapped page, or from the
 * host side, so the cost of the whole subsystem to a resident access is zero
 * instructions -- which is the property that has to be preserved and is why
 * not one byte of assembly changes.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_pager.h"
#include "x86_arena.h"
#include "x86_cpu.h"

#ifndef X86_HMA_NEGCTL
#define X86_HMA_NEGCTL 0        /* see the Makefile; 0 ships */
#endif

volatile uint32_t x86_pg_frames_total = 0;
volatile uint32_t x86_pg_frames_free  = 0;
volatile uint32_t x86_pg_resident     = 0;
volatile uint32_t x86_pg_faults_r     = 0;
volatile uint32_t x86_pg_faults_w     = 0;
volatile uint32_t x86_pg_faults_host  = 0;
volatile uint32_t x86_pg_fill_zero    = 0;
volatile uint32_t x86_pg_fill_nor     = 0;
volatile uint32_t x86_pg_fill_file    = 0;
volatile uint32_t x86_pg_evictions    = 0;
volatile uint32_t x86_pg_drop_clean   = 0;
volatile uint32_t x86_pg_wb_zero      = 0;
volatile uint32_t x86_pg_wb_file      = 0;
volatile uint32_t x86_pg_wb_bytes     = 0;
volatile uint32_t x86_pg_wb_opens     = 0;
volatile uint32_t x86_pg_wb_us_worst  = 0;
/* TOTAL time in writeback. _worst says whether the cost is a tail; only a
   total can be divided into a frame budget, and attribution needed one. */
volatile uint32_t x86_pg_wb_us_total  = 0;
volatile uint32_t x86_pg_io_errors    = 0;
volatile uint32_t x86_pg_exhausted    = 0;
volatile uint32_t x86_pg_unmapped     = 0;
volatile uint32_t x86_pg_donated      = 0;
volatile uint32_t x86_pg_donate_bytes = 0;
volatile uint32_t x86_pg_donate_refused = 0;
volatile uint32_t x86_pg_reclaim_busy = 0;
/* ---- the donation POLICY's own state, all host-readable ------------------ */
volatile uint32_t x86_pg_resident_peak  = 0;
volatile uint32_t x86_pg_reserve_eff    = 0;
volatile uint32_t x86_pg_shrink_signals = 0;
volatile uint32_t x86_pg_grow_signals   = 0;
volatile uint32_t x86_pg_policy_ratchet = 0;

/* ---- per-page state ------------------------------------------------------
 *
 * Three arrays indexed by guest page number, 8 KB of DTCM for the whole 8 MB
 * space. Deliberately NOT packed into the fold entry: the fold is read on
 * every guest memory access and the one-add translation is what it buys, so
 * anything the fast path does not need stays out of it.
 */
#define PG_RESIDENT   0x01u
#define PG_TIER_MASK  0x06u
#define PG_TIER_ZERO  0x00u
#define PG_TIER_NOR   0x02u
#define PG_TIER_FILE  0x04u

/* The block is supplied by the caller, and it belongs in DTCM. It is read on
 * every fault and written on every hotness bump, and DTCM is the 0-wait-state
 * memory this project pins its hot state to. It is not declared here because a
 * retro-go core cannot place static .bss in DTCM -- that pool is handed out at
 * runtime through mem_ctl() -- and 8 KB of ordinary .bss overflowed RAM_EMU in
 * the tier-1 harness by 176 bytes, which is exactly the silent-overrun shape
 * this project keeps getting bitten by. */
static uint8_t  *pg_flags;
static uint8_t  *pg_hot;
static uint16_t *pg_frame;

int x86_pager_set_tables(void *block, uint32_t bytes)
{
    if (!block || bytes < X86_PG_TABLE_BYTES)
        return -1;
    uint8_t *b = (uint8_t *)block;
    pg_flags = b;
    pg_hot   = b + X86_PG_PAGES;
    pg_frame = (uint16_t *)(b + 2u * X86_PG_PAGES);
    x86_pager_reset();
    return 0;
}

#define NO_FRAME 0xFFFFu

/* ---- frames --------------------------------------------------------------
 *
 * A frame is one 4 KB page of AXI. They come from up to four extents, because
 * the AXI this part can offer is not one block: RAM_EMU past the core's BSS
 * and whatever the LCD bonus pool has left are disjoint.
 */
#define MAX_FRAMES 256u
#define MAX_EXTENTS 4u

typedef struct { uint8_t *base; uint32_t first, count; } extent_t;
static extent_t extents[MAX_EXTENTS];
static uint32_t extent_count;

static uint16_t frame_page[MAX_FRAMES];     /* which guest page holds it */
static uint16_t free_list[MAX_FRAMES];
static uint32_t frame_count;
static uint32_t free_count;

/* The NOR snapshot, mirrored from x86_nor.c so a fill can read it. */
static const uint8_t *nor_base;
static uint32_t nor_pages;
static uint32_t nor_first;

static x86_pg_read_fn  backing_read;
static x86_pg_write_fn backing_write;

static uint32_t own_first, own_count;
/* The extra windows; see x86_pager.h. Two of them: EMS at linear 4 MB and the
   HMA at 1 MB, which comes and goes with the A20 gate. */
#define AUX_WINDOWS 2
static struct { uint32_t first, count; } aux_win[AUX_WINDOWS];

/* The clock hand. One index, not a list: see the policy note in x86_pager.h. */
static uint32_t victim_hand;

/* The extent currently lent to the arena, so it can be taken back exactly. */
static uint8_t *donated_base;
static uint32_t donated_frames;
static uint32_t donated_lo;      /* its first frame index, kept for the return */
static uint32_t donate_reserve = X86_PG_DONATE_RESERVE;

#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004)

static uint8_t *frame_addr(uint32_t f)
{
    for (uint32_t e = 0; e < extent_count; e++) {
        if (f >= extents[e].first && f < extents[e].first + extents[e].count)
            return extents[e].base + (f - extents[e].first) * X86_PG_STRIDE;
    }
    return NULL;
}

void x86_pager_reset(void)
{
    if (pg_flags) {
        memset(pg_flags, 0, X86_PG_PAGES);
        memset(pg_hot, 0, X86_PG_PAGES);
        for (uint32_t i = 0; i < X86_PG_PAGES; i++)
            pg_frame[i] = NO_FRAME;
    }
    memset(extents, 0, sizeof(extents));
    extent_count = frame_count = free_count = 0;
    own_first = own_count = 0;
    memset(aux_win, 0, sizeof(aux_win));
    victim_hand = 0;
    donated_base = NULL;
    donated_frames = donated_lo = 0;
    x86_pg_frames_total = x86_pg_frames_free = x86_pg_resident = 0;
    x86_pg_donated = x86_pg_donate_bytes = 0;
    x86_pager_policy_reset();
}

int x86_pager_add_frames(void *base, uint32_t bytes)
{
    if (!base || extent_count >= MAX_EXTENTS)
        return -1;
    uint32_t start = ((uint32_t)(uintptr_t)base + X86_PG_SIZE - 1u)
                     & ~(X86_PG_SIZE - 1u);
    uint32_t end = (uint32_t)(uintptr_t)base + bytes;
    if (end <= start)
        return -1;
    /* STRIDE, not SIZE. Each frame owns X86_PG_SLACK bytes past its top so a
       straddling guest access has somewhere to land that belongs to nobody --
       see the seam block in x86_pager.h. A pool sized in whole pages therefore
       yields one fewer frame than it used to, which is why every caller was
       moved to X86_PG_POOL_BYTES() rather than left to lose a frame silently. */
    uint32_t n = (end - start) / X86_PG_STRIDE;
    if (frame_count + n > MAX_FRAMES)
        n = MAX_FRAMES - frame_count;
    if (!n)
        return -1;

    extents[extent_count].base  = (uint8_t *)(uintptr_t)start;
    extents[extent_count].first = frame_count;
    extents[extent_count].count = n;
    extent_count++;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t f = frame_count + i;
        frame_page[f] = NO_FRAME;
        free_list[free_count++] = (uint16_t)f;
    }
    frame_count += n;

    x86_pg_frames_total = frame_count;
    x86_pg_frames_free  = free_count;
    return 0;
}

void x86_pager_set_backing(x86_pg_read_fn rd, x86_pg_write_fn wr)
{
    backing_read = rd;
    backing_write = wr;
}

void x86_pager_own(uint32_t first_page, uint32_t count)
{
    own_first = first_page;
    own_count = count;
}

void x86_pager_own_aux(uint32_t first_page, uint32_t count)
{
    if (!count) {
        if (!first_page) {
            memset(aux_win, 0, sizeof(aux_win));   /* drop them all */
            return;
        }
        for (uint32_t i = 0; i < AUX_WINDOWS; i++)
            if (aux_win[i].count && aux_win[i].first == first_page)
                aux_win[i].count = 0;
        return;
    }
    for (uint32_t i = 0; i < AUX_WINDOWS; i++)
        if (aux_win[i].count && aux_win[i].first == first_page) {
            aux_win[i].count = count;              /* re-register, not a dup */
            return;
        }
    for (uint32_t i = 0; i < AUX_WINDOWS; i++)
        if (!aux_win[i].count) {
            aux_win[i].first = first_page;
            aux_win[i].count = count;
            return;
        }
    /* Out of windows. Silently dropping one would make a whole region of guest
       memory un-ownable with no counter to say so, so this refuses the NEW
       window and leaves the established ones alone -- the failure then shows
       up as x86_pg_unmapped on the region that was just asked for. */
}

void x86_pager_set_nor(const void *base, uint32_t bytes, uint32_t first_page)
{
    nor_base  = (const uint8_t *)base;
    nor_pages = bytes >> X86_PG_SHIFT;
    nor_first = first_page;
    /* Every page the snapshot covers is NOR tier from here on. Its fold entry
       is x86_nor.c's business; what this records is where a fill comes from
       once the page is pulled into AXI. */
    if (!pg_flags)
        return;
    for (uint32_t i = 0; i < nor_pages; i++) {
        uint32_t p = first_page + i;
        if (p < X86_PG_PAGES && !(pg_flags[p] & PG_RESIDENT))
            pg_flags[p] = (uint8_t)((pg_flags[p] & ~PG_TIER_MASK) | PG_TIER_NOR);
    }
}

static int owns(uint32_t page)
{
    if (!pg_flags || page >= X86_PG_PAGES)
        return 0;
    if (own_count && page >= own_first && page < own_first + own_count)
        return 1;
    for (uint32_t i = 0; i < AUX_WINDOWS; i++)
        if (aux_win[i].count && page >= aux_win[i].first
            && page < aux_win[i].first + aux_win[i].count)
            return 1;
    return 0;
}

/*
 * IS THIS FOLD ENTRY THE WRAP'S RATHER THAN OURS?
 *
 * While A20 is masked the sixteen entries at 1 MB hold the ALIAS onto low
 * memory, and that is the guest-visible truth for those addresses. A page of
 * the HMA may still be RESIDENT from an earlier open window -- its frame holds
 * the guest's HMA data and must survive the toggle -- but its fold entry is
 * not the pager's to write while the gate is shut. Without this, evicting such
 * a page writes FOLD_INVALID over a live alias and the real-mode wrap stops
 * working at one address in sixteen, silently.
 */
static int fold_is_wrapped(uint32_t page)
{
#if X86_HMA_NEGCTL == 1
    (void)page;
    return 0;                       /* NEGATIVE CONTROL: the wrap owns nothing */
#else
    return !x86_io.a20_open
           && page >= X86_HMA_FIRST_PAGE
           && page <  X86_HMA_FIRST_PAGE + X86_HMA_PAGES;
#endif
}

/*
 * Install a page's fold entry, and the HMA alias if the A20 gate is masked.
 *
 * The alias is why this is a function and not a store. x86_mem_wrap_1mb()
 * points the sixteen pages above 1 MB at the bottom of memory while A20 is
 * masked, so a page that faults in without fixing its alias leaves a guest
 * reaching the same memory through a wrapped address looking at FOLD_INVALID
 * -- an endless refault on a page that is already resident.
 */
static void fold_install(uint32_t page, uint8_t *host)
{
    if (!fold_is_wrapped(page))
        guest_page_table_ptr[page] = FOLD_K(host, page);
    if (page < X86_HMA_PAGES && !x86_io.a20_open) {
        uint32_t a = X86_HMA_FIRST_PAGE + page;
        if (a < FOLD_ENTRIES)
            guest_page_table_ptr[a] = FOLD_K(host, a);
    }
}

static void fold_clear(uint32_t page)
{
    if (!fold_is_wrapped(page))
        guest_page_table_ptr[page] = FOLD_INVALID;
    if (page < X86_HMA_PAGES && !x86_io.a20_open) {
        uint32_t a = X86_HMA_FIRST_PAGE + page;
        if (a < FOLD_ENTRIES)
            guest_page_table_ptr[a] = FOLD_INVALID;
    }
}

void x86_pager_refold(uint32_t page)
{
    if (!guest_page_table_ptr || page >= X86_PG_PAGES)
        return;
    if (pg_flags && (pg_flags[page] & PG_RESIDENT)
        && pg_frame[page] != NO_FRAME) {
        uint8_t *host = frame_addr(pg_frame[page]);
        if (host) {
            fold_install(page, host);
            return;
        }
    }
    fold_clear(page);
}

static void hot_bump(uint32_t page)
{
    if (pg_hot[page] < 255u)
        pg_hot[page]++;
}

void x86_pager_note_eip(uint32_t lin)
{
    uint32_t p = lin >> X86_PG_SHIFT;
    if (pg_hot && p < X86_PG_PAGES)
        hot_bump(p);
}

void x86_pager_decay(void)
{
    if (!pg_hot)
        return;
    for (uint32_t i = 0; i < X86_PG_PAGES; i++)
        pg_hot[i] >>= 1;
}

/* ---- filling a fresh frame ---------------------------------------------- */

static int fill_frame(uint32_t page, uint8_t *dst)
{
    switch (pg_flags[page] & PG_TIER_MASK) {
    case PG_TIER_NOR:
        if (nor_base && page >= nor_first && page < nor_first + nor_pages) {
            memcpy(dst, nor_base + (page - nor_first) * X86_PG_SIZE,
                   X86_PG_SIZE);
            x86_pg_fill_nor++;
            return 0;
        }
        /* The snapshot went away under us. Zero is the only safe answer, and
           it is counted as a zero fill rather than silently as a NOR one. */
        break;
    case PG_TIER_FILE:
        if (backing_read && backing_read(page, dst) == 0) {
            x86_pg_fill_file++;
            return 0;
        }
        /* A pagefile read that fails is DATA LOSS, not a slow path. It must be
           loud: the guest gets zeroes and the counter says the guest's memory
           is no longer what it wrote. */
        x86_pg_io_errors++;
        break;
    default:
        break;
    }
    memset(dst, 0, X86_PG_SIZE);
    x86_pg_fill_zero++;
    return 0;
}

static uint32_t take_free_frame(void)
{
    if (!free_count)
        return NO_FRAME;
    uint32_t f = free_list[--free_count];
    x86_pg_frames_free = free_count;
    return f;
}

/* ---- eviction ------------------------------------------------------------
 *
 * The dirty test is a COMPARISON, not a permission bit and not a guess. See
 * the header: this is the whole reason the previous project's 566 MB of
 * pagefile traffic and 98.6% wasted writes do not happen here.
 */

/* Word-at-a-time, because a frame is 4 KB aligned and the answer is almost
   always "yes, still zero" -- which is the case that has to be cheap. */
static int page_all_zero(const uint8_t *p)
{
    const uint32_t *w = (const uint32_t *)(const void *)p;
    for (uint32_t i = 0; i < X86_PG_SIZE / 4u; i++) {
        if (w[i])
            return 0;
    }
    return 1;
}

/* The NOR snapshot page backing `page`, or NULL if the snapshot does not cover
   it (a NOR-tier page whose snapshot went away has no authority to compare
   against and must be treated as dirty). */
static const uint8_t *nor_page_of(uint32_t page)
{
    if (!nor_base || page < nor_first || page >= nor_first + nor_pages)
        return NULL;
    return nor_base + (page - nor_first) * X86_PG_SIZE;
}

static void writeback(uint32_t page, const uint8_t *host)
{
    if (!backing_write) {
        /* No pagefile at all. The bytes are GONE, and that is data loss rather
           than a slow path -- exactly the same rule fill_frame() applies to a
           failed read. The tier is left alone so a refill produces zeroes, and
           the counter is the only thing that can say the guest's memory is no
           longer what it wrote. */
        x86_pg_io_errors++;
        return;
    }
    uint32_t t0 = DWT_CYCCNT;
    int rc = backing_write(page, host);
    uint32_t dt = DWT_CYCCNT - t0;

    x86_pg_wb_opens++;
    if (rc != 0) {
        x86_pg_io_errors++;
        return;
    }
    /* Only now is FILE tier true. Marking it before the write succeeds would
       point a later fill at a slot that was never written. */
    pg_flags[page] = (uint8_t)((pg_flags[page] & ~PG_TIER_MASK) | PG_TIER_FILE);
    x86_pg_wb_file++;
    x86_pg_wb_bytes += X86_PG_SIZE;
    {
        uint32_t per_us = x86_disk_cycles_per_us;
        uint32_t us = per_us ? (dt / per_us) : 0u;
        x86_pg_wb_us_total += us;
        if (us > x86_pg_wb_us_worst)
            x86_pg_wb_us_worst = us;
    }
}

/* Take `f` away from whatever page holds it. The frame is NOT returned to the
   free list -- the caller is about to use it, and a round trip through the list
   is a window in which something else could take it. */
/*
 * Is this page byte-identical to its tier's authoritative copy?
 *
 * The exact test, shared by the eviction itself and by the VICTIM PICKER --
 * which needs it because a pool with no pagefile may only ever evict a page it
 * can drop. Two implementations of "clean" that could disagree is how a page
 * gets chosen as droppable and then written, or worse, chosen and then lost.
 */
static int page_is_clean(uint32_t page, const uint8_t *host)
{
    uint32_t tier = pg_flags[page] & PG_TIER_MASK;

    if (tier == PG_TIER_NOR) {
        const uint8_t *snap = nor_page_of(page);
        return snap && memcmp(host, snap, X86_PG_SIZE) == 0;
    }
    /* ZERO is clean while it is still zero. FILE has no in-memory authority,
       but the same scan gives its one free case away: a page the guest has
       zeroed out again reverts to ZERO tier and never reaches the card. */
    return page_all_zero(host);
}

/* Take `f` away from whatever page holds it. The frame is NOT returned to the
   free list -- the caller is about to use it, and a round trip through the list
   is a window in which something else could take it. */
static void evict_frame(uint32_t f)
{
    uint32_t page = frame_page[f];
    uint8_t *host = frame_addr(f);

    /* Unmap FIRST. Everything below reads the frame as the page's final
       contents, and an interpreter that could still reach it through the fold
       would be racing its own writeback. */
    fold_clear(page);

    if (host) {
        if (page_is_clean(page, host)) {
            if ((pg_flags[page] & PG_TIER_MASK) == PG_TIER_FILE) {
                /* It was dirty once and is all zeroes again: back to ZERO tier,
                   and the pagefile slot it used to own is simply abandoned. */
                pg_flags[page] &= (uint8_t)~PG_TIER_MASK;
                x86_pg_wb_zero++;
            }
            x86_pg_drop_clean++;        /* reverts to its tier, no SD I/O */
        } else {
            writeback(page, host);
        }
    }

    frame_page[f]  = NO_FRAME;
    pg_frame[page] = NO_FRAME;
    pg_flags[page] &= (uint8_t)~PG_RESIDENT;
    pg_hot[page] = 0;
    if (x86_pg_resident)
        x86_pg_resident--;
    x86_pg_evictions++;
}

/*
 * Choose a victim and free its frame. NO_FRAME means every resident page is
 * vetoed, which is the only remaining route to x86_pg_exhausted.
 *
 * Two passes of the clock. The first gives a page with any hotness left a
 * second chance and halves it; the second takes the first page the vetoes
 * allow, so a pool in which everything is hot still makes progress instead of
 * reporting exhaustion for a page it could have had.
 */
static uint32_t evict_one(uint32_t exclude_page)
{
    if (!frame_count)
        return NO_FRAME;
    if (victim_hand >= frame_count)
        victim_hand = 0;

    for (uint32_t pass = 0; pass < 2u; pass++) {
        for (uint32_t i = 0; i < frame_count; i++) {
            uint32_t f = victim_hand;
            victim_hand = victim_hand + 1u;
            if (victim_hand >= frame_count)
                victim_hand = 0;

            uint32_t p = frame_page[f];
            if (p == NO_FRAME || p >= X86_PG_PAGES)
                continue;
            if (p == exclude_page || !(pg_flags[p] & PG_RESIDENT))
                continue;
            /* The two vetoes. Pinned is display storage; the ring is a raw host
               pointer nobody will hand back. Both are in x86_arena.h. */
            if (x86_page_is_pinned(p))
                continue;
            if (pass == 0 && pg_hot[p]) {
                pg_hot[p] >>= 1;            /* second chance */
                continue;
            }
            if (x86_livemap_holds(p))
                continue;
            /* WITHOUT A PAGEFILE, ONLY A DROPPABLE PAGE MAY GO.
             *
             * src/main.c wires no backing store today, and evicting a modified
             * page with nowhere to put it is not a slow path -- it is silent
             * data loss. MEASURED on KEEN4 under gwemu before this check
             * existed: 38 evictions, 38 x86_pg_io_errors, 38 pages of the
             * guest's memory gone. So the picker asks the same exact question
             * the eviction would, and a pool with nothing droppable left
             * reports exhaustion -- a clean X86_EXIT_MEM_FAULT and a counter
             * rather than a guest quietly reading rubbish. */
            if (!backing_write) {
                uint8_t *h = frame_addr(f);
                if (!h || !page_is_clean(p, h))
                    continue;
            }
            evict_frame(f);
            return f;
        }
    }
    return NO_FRAME;
}

/* ---- donating frames to the arena --------------------------------------- */

/* Whole pages inside [start, start+span): both ends rounded INWARD, which is
   exactly what x86_arena_add_region() will do to the same extent. Computing it
   the same way here is what makes "did we donate enough" answerable before
   anything moves. */
static uint32_t usable_bytes(uint32_t start, uint32_t span)
{
    uint32_t lo = (start + X86_PG_SIZE - 1u) & ~(X86_PG_SIZE - 1u);
    uint32_t hi = (start + span) & ~(X86_PG_SIZE - 1u);
    return (hi > lo) ? (hi - lo) : 0u;
}

/* Frames whose index is no longer covered by any extent are LENT OUT. They
   keep their index -- renumbering would repoint every frame in the pool -- and
   frame_addr() is the single place that knows. */
static int frame_live(uint32_t f)
{
    return frame_addr(f) != NULL;
}

static void rebuild_free_list(void)
{
    free_count = 0;
    for (uint32_t f = 0; f < frame_count; f++) {
        if (frame_page[f] == NO_FRAME && frame_live(f))
            free_list[free_count++] = (uint16_t)f;
    }
    x86_pg_frames_free = free_count;
    x86_pg_frames_total = frame_count - donated_frames;
}

/* Merge extents that are adjacent in BOTH frame index and address. A mode
   change donates and reclaims on every entry to and exit from 0Dh, and without
   this the pool would fragment into MAX_EXTENTS after four of them and then
   refuse to take its own memory back. */
static void extents_normalize(void)
{
    int again = 1;
    while (again) {
        again = 0;
        for (uint32_t a = 0; a < extent_count && !again; a++) {
            for (uint32_t b = 0; b < extent_count; b++) {
                if (a == b)
                    continue;
                if (extents[a].first + extents[a].count != extents[b].first)
                    continue;
                if (extents[a].base + extents[a].count * X86_PG_STRIDE
                    != extents[b].base)
                    continue;
                extents[a].count += extents[b].count;
                for (uint32_t i = b + 1u; i < extent_count; i++)
                    extents[i - 1u] = extents[i];
                extent_count--;
                again = 1;
                break;
            }
        }
    }
}

void x86_pager_set_donate_reserve(uint32_t frames)
{
    donate_reserve = frames;
}

/*
 * Donate one contiguous run of frames.
 *
 * The run may be ANYWHERE inside an extent, not just at its top, and that is
 * not generality for its own sake. Frames are handed out from the top of the
 * free list, so after a title has settled the resident frames are the HIGH
 * ones and the idle ones are LOW -- a donor that could only take the top would
 * have to evict a live working set to reach memory that was free all along.
 * Taking a run from the middle splits the extent in two, which costs one
 * extent slot and no renumbering.
 *
 * The run is scored by how many resident pages it would displace and the
 * cheapest wins, so a donation costs nothing at all when enough of the pool is
 * idle -- which is the case this exists for.
 */
uint32_t x86_pager_donate_to_arena(uint32_t bytes)
{
    uint32_t best_lo = 0, best_n = 0, best_cost = 0xFFFFFFFFu, best_got = 0;
    uint32_t best_ext = 0;

    if (!bytes || !extent_count || donated_frames || extent_count >= MAX_EXTENTS)
        goto refuse;
    /*
     * NO PAGEFILE, NO DONATION. MEASURED, KEEN4, gwemu tier 2.
     *
     * With a backing store wired this is the whole point of the feature: the
     * donation placed a 245,760 B EGA card (60 KB/plane, against 8 KB/plane
     * shipped) out of 60 idle frames, x86_pg_io_errors stayed 0 and the two
     * evictions it cost were both clean drops.
     *
     * Without one it is unsafe, and not subtly: guest RAM fell to 100 frames,
     * Keen 4's working set does not fit in that, only 2 pages were ever
     * droppable, and the guest took X86_EXIT_MEM_FAULT with x86_pg_exhausted=1
     * -- loud and non-corrupting, but the title stops. No reserve fixes it,
     * because a card big enough to be worth having (the reference wants
     * 65,536/plane; nothing between 8 KB and 64 KB renders,
     * references/8086tiny/docs/video/11-ega-planar.md:299-330) costs more
     * frames than the guest can spare.
     *
     * So the mechanism ships DORMANT and becomes live the moment src/main.c
     * calls x86_pager_set_backing() with an SD pagefile. That is one call in a
     * file this does not own, and it is the only thing between here and the
     * card.
     */
    if (!backing_write)
        goto refuse;
    {
        /* The smallest run that can possibly cover `bytes` after BOTH ends are
           rounded inward. Each end can lose up to a page. */
        uint32_t n = (bytes + 2u * X86_PG_SIZE - 2u + X86_PG_STRIDE - 1u)
                     / X86_PG_STRIDE;
        uint32_t live = frame_count - donated_frames;
        /* The reserve is the POLICY's, which with the policy off is the static
           constant this used to read directly. One call, at a mode set. */
        uint32_t reserve = x86_pager_policy_reserve();
        if (!n || live < reserve || n > live - reserve)
            goto refuse;

        for (uint32_t e = 0; e < extent_count; e++) {
            if (extents[e].count < n)
                continue;
            for (uint32_t lo = extents[e].first;
                 lo + n <= extents[e].first + extents[e].count; lo++) {
                uint32_t start = (uint32_t)(uintptr_t)extents[e].base
                                 + (lo - extents[e].first) * X86_PG_STRIDE;
                uint32_t got = usable_bytes(start, n * X86_PG_STRIDE);
                if (got < bytes)
                    continue;
                uint32_t cost = 0;
                int ok = 1;
                for (uint32_t f = lo; f < lo + n; f++) {
                    uint32_t p = frame_page[f];
                    if (p == NO_FRAME)
                        continue;
                    /* The two vetoes, and one more: with no pagefile an
                       eviction of a dirty page is DATA LOSS, so a pool with no
                       backing store may only donate frames that are already
                       free. src/main.c has no pagefile today, which makes this
                       the rule that actually governs on the target. */
                    if (!backing_write
                        || (p < X86_PG_PAGES
                            && (x86_page_is_pinned(p) || x86_livemap_holds(p)))) {
                        ok = 0;
                        break;
                    }
                    cost++;
                }
                if (!ok || cost >= best_cost)
                    continue;
                best_cost = cost; best_lo = lo; best_n = n;
                best_got = got; best_ext = e;
                if (!cost)
                    break;              /* free run: nothing beats it */
            }
            if (!best_cost)
                break;
        }
        if (best_cost == 0xFFFFFFFFu)
            goto refuse;

        uint8_t *base = extents[best_ext].base
                        + (best_lo - extents[best_ext].first) * X86_PG_STRIDE;
        if (x86_arena_add_region(base, best_n * X86_PG_STRIDE) != 0)
            goto refuse;

        for (uint32_t f = best_lo; f < best_lo + best_n; f++) {
            if (frame_page[f] != NO_FRAME)
                evict_frame(f);
        }

        /* Split the extent around the run. The tail becomes a new extent with
           its own base and its ORIGINAL frame indices, so frame_addr() keeps
           answering the same question for every frame that stayed. */
        extent_t *e = &extents[best_ext];
        uint32_t tail_first = best_lo + best_n;
        uint32_t tail_count = e->first + e->count - tail_first;
        e->count = best_lo - e->first;
        if (tail_count) {
            extents[extent_count].base  = base + best_n * X86_PG_STRIDE;
            extents[extent_count].first = tail_first;
            extents[extent_count].count = tail_count;
            extent_count++;
        }
        if (!e->count) {
            for (uint32_t i = best_ext + 1u; i < extent_count; i++)
                extents[i - 1u] = extents[i];
            extent_count--;
        }

        victim_hand = 0;
        donated_base = base;
        donated_frames = best_n;
        donated_lo = best_lo;
        x86_pg_donated = best_n;
        x86_pg_donate_bytes = best_got;
        rebuild_free_list();
        return best_got;
    }
refuse:
    x86_pg_donate_refused++;
    return 0;
}

/*
 * Donate the largest run between `max_bytes` and `min_bytes`, stepping down a
 * page at a time. The EGA card is sized by what fits, so asking once for the
 * maximum and giving up would leave a smaller card unbuilt -- and putting the
 * ladder here rather than in the video path keeps the refusal counter honest:
 * one refusal for a donation that could not be made at all, not one per rung.
 */
uint32_t x86_pager_donate_upto(uint32_t max_bytes, uint32_t min_bytes)
{
    uint32_t refused = x86_pg_donate_refused;
    for (uint32_t want = max_bytes; want >= min_bytes; want -= X86_PG_SIZE) {
        uint32_t got = x86_pager_donate_to_arena(want);
        if (got) {
            x86_pg_donate_refused = refused;
            return got;
        }
        if (want < min_bytes + X86_PG_SIZE)
            break;
    }
    x86_pg_donate_refused = refused + 1u;
    return 0;
}

int x86_pager_reclaim_from_arena(void)
{
    if (!donated_frames)
        return 0;
    /* The arena rounded the extent's start UP to a page, so the base it holds
       is not the base we lent it. Round the same way rather than hand it an
       address it will not recognise. */
    uint8_t *arena_base = (uint8_t *)(uintptr_t)
        (((uint32_t)(uintptr_t)donated_base + X86_PG_SIZE - 1u)
         & ~(X86_PG_SIZE - 1u));
    if (x86_arena_drop_last_region(arena_base) != 0) {
        /* The display still holds pages in there. Refusing is the only safe
           answer and the frames stay lent -- with eviction that costs the guest
           speed, where taking them back would cost it correctness. */
        x86_pg_reclaim_busy++;
        return -1;
    }
    if (extent_count < MAX_EXTENTS) {
        extents[extent_count].base  = donated_base;
        extents[extent_count].first = donated_lo;
        extents[extent_count].count = donated_frames;
        extent_count++;
        for (uint32_t i = 0; i < donated_frames; i++)
            frame_page[donated_lo + i] = NO_FRAME;
        donated_base = NULL;
        donated_frames = 0;
        extents_normalize();
        rebuild_free_list();
    }
    x86_pg_donated = 0;
    x86_pg_donate_bytes = 0;
    return 0;
}

/* ---- the donation POLICY -------------------------------------------------
 *
 * Everything above this line is MECHANISM: donate a run, take it back. What
 * was missing is the decision, and until now that decision was a build-time
 * constant (X86_PG_DONATE_RESERVE) evaluated ONCE, at mode adoption. A
 * constant cannot serve a title whose working set varies -- KEEN4's intro
 * wants a big card and few frames, its gameplay the opposite -- so the pick
 * between "EGA" and "frames" was an artefact of when the question was asked,
 * not a property of the memory.
 *
 * The rule here, in one line: THE RESERVE IS THE MEASURED WORKING SET PLUS A
 * RUNWAY, and it only ever goes up within a session.
 *
 *   reserve = max(FLOOR, resident_peak + MARGIN, ratchet)
 *
 * x86_pg_resident_peak is measured on the fill path (free -- a compare after
 * a 4 KB copy). The ratchet records what pressure has already taught us, so a
 * working set that dips does not invite the card to grow back into it and
 * re-learn the same lesson at 433 ms a page.
 *
 * WHY A WATERMARK AND NOT "SHRINK AT THE FIRST EVICTION". An eviction is the
 * cost, not the warning: by the time x86_pg_evictions moves, the guest has
 * already paid one 4 KB pagefile write, and on silicon that is up to 433 ms
 * (x86_pg_wb_us_worst) -- a dropped frame at best. So the primary signal is
 * FREE FRAMES falling below the same MARGIN the reserve carries, which fires
 * while the pool still has runway and hands the frames back BEFORE the first
 * write. The eviction counter is kept as a BACKSTOP for the case the
 * watermark cannot see (a burst inside one tick), not as the trigger.
 *
 * WHY NOT A HYSTERESIS LOOP ON THE EVICTION RATE. It has to thrash to sample:
 * every probe upward costs real writebacks, and the loop converges by paying
 * the exact cost it exists to avoid. The peak is free to measure and is
 * strictly better information.
 *
 * NOTHING HERE IS ON A HOT PATH. x86_pager_policy_tick() is called once per
 * FRAME by the video path, and acts only at a moment the renderer tolerates.
 */
#ifndef X86_PG_POLICY
#define X86_PG_POLICY 1
#endif
/* The two negative controls, built as arms rather than argued: with the shrink
   compiled out the evictions must appear, and with the grow compiled out the
   card must stay at the arena's size. run_paging_controls.py builds both. */
#ifndef X86_PG_POLICY_NO_SHRINK
#define X86_PG_POLICY_NO_SHRINK 0
#endif
#ifndef X86_PG_POLICY_NO_GROW
#define X86_PG_POLICY_NO_GROW 0
#endif

static uint8_t  policy_on = X86_PG_POLICY;
static uint32_t pol_ev_seen;
static uint32_t pol_quiet;
/* The three numbers, runtime rather than #define, for the same reason
   x86_pager_set_donate_reserve() exists: the tier-1 harness runs an 8-frame
   pool and must state its own arithmetic instead of the shipping part's. */
static uint32_t pol_floor  = X86_PG_RESERVE_FLOOR;
static uint32_t pol_margin = X86_PG_RESERVE_MARGIN;
static uint32_t pol_quiet_n = X86_PG_POLICY_QUIET;

void x86_pager_policy_config(uint32_t floor, uint32_t margin, uint32_t quiet)
{
    pol_floor = floor;
    pol_margin = margin ? margin : 1u;
    pol_quiet_n = quiet;
}

void x86_pager_policy_enable(int on)
{
    policy_on = (uint8_t)(on && X86_PG_POLICY);
}

void x86_pager_policy_reset(void)
{
    pol_ev_seen = x86_pg_evictions;
    pol_quiet = 0;
    x86_pg_policy_ratchet = 0;
    x86_pg_resident_peak = x86_pg_resident;
    x86_pg_shrink_signals = x86_pg_grow_signals = 0;
}

uint32_t x86_pager_policy_reserve(void)
{
    uint32_t r;

    if (!policy_on)
        return donate_reserve;
    r = x86_pg_resident_peak + pol_margin;
    if (r < pol_floor)
        r = pol_floor;
    if (r < x86_pg_policy_ratchet)
        r = x86_pg_policy_ratchet;
    if (r > frame_count)
        r = frame_count;
    x86_pg_reserve_eff = r;
    return r;
}

/*
 * What a donation could take RIGHT NOW without breaking the reserve, in bytes.
 *
 * Two pages are held back because x86_arena_add_region() rounds BOTH ends of
 * the run inward and the caller is about to compare this against a card size:
 * an over-estimate here turns into a refit that releases a good card and
 * cannot rebuild it, which is a visible defect rather than a missed
 * optimisation.
 */
uint32_t x86_pager_policy_headroom(void)
{
    uint32_t reserve = x86_pager_policy_reserve();
    uint32_t live = frame_count - donated_frames;

    if (live <= reserve + 2u)
        return 0;
    return (live - reserve - 2u) * X86_PG_STRIDE;
}

int x86_pager_policy_tick(void)
{
    uint32_t ev, live, want;
    int pressure;

    if (!policy_on)
        return X86_PG_POLICY_HOLD;

    ev = x86_pg_evictions;
    live = frame_count - donated_frames;
    /* The watermark first (it fires BEFORE a write), the eviction counter
       second (it fires after one, and only catches what the watermark could
       not see inside a single tick). */
    pressure = (x86_pg_frames_free < pol_margin)
               || (ev != pol_ev_seen);
    pol_ev_seen = ev;

    if (pressure) {
        pol_quiet = 0;
        /* Learn. The set is at least this big, so never lend into it again. */
        want = x86_pg_resident_peak + pol_margin;
        if (want > x86_pg_policy_ratchet)
            x86_pg_policy_ratchet = want;
        (void)x86_pager_policy_reserve();
        if (X86_PG_POLICY_NO_SHRINK || !donated_frames)
            return X86_PG_POLICY_HOLD;
        x86_pg_shrink_signals++;
        return X86_PG_POLICY_SHRINK;
    }

    (void)x86_pager_policy_reserve();
    if (X86_PG_POLICY_NO_GROW)
        return X86_PG_POLICY_HOLD;
    /* Quiet for a whole second of frames before lending anything: a title
       loading a level is idle between bursts, and growing into the gap is how
       a policy manages to be wrong in both directions at once. */
    if (++pol_quiet < pol_quiet_n)
        return X86_PG_POLICY_HOLD;
    if (live <= x86_pg_reserve_eff)
        return X86_PG_POLICY_HOLD;
    /* PERMISSION, not an instruction. The caller still has to find that a
       bigger card actually fits -- a refit costs the card's contents, so it
       may not be done speculatively. */
    x86_pg_grow_signals++;
    return X86_PG_POLICY_GROW;
}

uint8_t *x86_pager_slack(uint32_t page)
{
#if X86_HMA_NEGCTL == 2
    /* NEGATIVE CONTROL: no slack over the HMA, so a straddling access inside
       it cannot be served. The seam counts the refusal and the guest gets the
       wrong high byte -- which is exactly what the shipping build must not do
       now that those sixteen pages hold real, separately-framed memory. */
    if (page >= X86_HMA_FIRST_PAGE && page < X86_HMA_FIRST_PAGE + X86_HMA_PAGES)
        return NULL;
#endif
    if (!pg_flags || page >= X86_PG_PAGES || !(pg_flags[page] & PG_RESIDENT))
        return NULL;
    uint8_t *host = frame_addr(pg_frame[page]);
    return host ? host + X86_PG_SIZE : NULL;
}

uint32_t x86_pager_fault(uint32_t lin, int write)
{
    uint32_t page = lin >> X86_PG_SHIFT;

    if (!owns(page)) {
        /* Not a page fault at all -- a genuinely unmapped address. The caller
           takes X86_EXIT_MEM_FAULT, which is the honest answer and the one
           that names the guest's own bug rather than ours. */
        x86_pg_unmapped++;
        return 0;
    }

    if (write)
        x86_pg_faults_w++;
    else
        x86_pg_faults_r++;
    hot_bump(page);

    if (pg_flags[page] & PG_RESIDENT) {
        /* Already resident: the fold entry disagreed with our own state, which
           means something repointed it behind us. Reinstall rather than
           allocate a second frame for the same page. */
        uint8_t *host = frame_addr(pg_frame[page]);
        if (host) {
            fold_install(page, host);
            return (uint32_t)(uintptr_t)host + (lin & (X86_PG_SIZE - 1u));
        }
    }

    uint32_t f = take_free_frame();
    if (f == NO_FRAME)
        f = evict_one(page);        /* make room; see the header for the rules */
    if (f == NO_FRAME) {
        /* LOUD, and now it means something much narrower than "the pool is
           full": every resident page is PINNED or LIVE-MAPPED, so there was no
           legal victim. A pool that silently eats stores is indistinguishable
           from one that works, and this project has hit silent allocation
           failure five times. The guest takes a clean memory fault and the
           counter says exactly why. */
        x86_pg_exhausted++;
        return 0;
    }

    uint8_t *host = frame_addr(f);
    if (!host) {
        x86_pg_exhausted++;
        return 0;
    }
    fill_frame(page, host);
    /* Prime the slack from the neighbour AS IT STANDS NOW. A page becoming
       resident takes a fresh snapshot; storing and restoring an old one is the
       stale-neighbour bug the prior port traced
       (references/8086tiny/8086tiny.c:10182-10190). A straddling access still
       goes through x86_seam_sync(), which pulls live bytes -- this only means
       the eight bytes are never uninitialised. */
    {
        uint32_t nk = (page + 1u < FOLD_ENTRIES)
                      ? guest_page_table_ptr[page + 1u] : FOLD_INVALID;
        if (nk != FOLD_INVALID)
            memcpy(host + X86_PG_SIZE,
                   x86_fold_ptr((page + 1u) << X86_PG_SHIFT, nk),
                   X86_PG_SLACK);
        else
            memset(host + X86_PG_SIZE, 0, X86_PG_SLACK);
    }

    frame_page[f] = (uint16_t)page;
    pg_frame[page] = (uint16_t)f;
    pg_flags[page] |= PG_RESIDENT;
    x86_pg_resident++;
    /* The high-water mark IS the working set, measured rather than assumed,
       and it is the input the donation policy runs on. One compare on a path
       that has just done a 4 KB fill. */
    if (x86_pg_resident > x86_pg_resident_peak)
        x86_pg_resident_peak = x86_pg_resident;
    fold_install(page, host);

    return (uint32_t)(uintptr_t)host + (lin & (X86_PG_SIZE - 1u));
}
