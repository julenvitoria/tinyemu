/*
 * The VRAM arena and the pin state the pager's victim picker consults.
 *
 * The design, and the measurements behind "never partially satisfy" and behind
 * the live-pointer ring, are in x86_arena.h. This file is the arithmetic.
 *
 * The allocator is a page BITMAP with first-fit contiguous search. That is a
 * deliberate choice of the dullest thing that works: claims happen on a mode
 * change, which is a few times per title, and the whole arena is at most 44
 * pages. A free-list or a buddy allocator would be faster at a rate nothing
 * here runs at, and would add a failure mode (a corrupted list) that a bitmap
 * cannot have. The one property that IS needed is exact contiguity reporting,
 * because a refusal has to quote a real deficit.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_arena.h"
#include "x86_cpu.h"

volatile uint32_t x86_arena_bytes     = 0;
volatile uint32_t x86_arena_used      = 0;
volatile uint32_t x86_arena_free      = 0;
volatile uint32_t x86_arena_largest   = 0;
volatile uint32_t x86_arena_deficit   = 0;
volatile uint32_t x86_arena_refusals  = 0;
volatile uint32_t x86_arena_claims_ok = 0;
volatile uint32_t x86_arena_regions   = 0;
volatile uint32_t x86_pinned_pages    = 0;
volatile uint32_t x86_livemap_hits    = 0;

/* Enough pages for every extent this part can offer: the bonus-pool hole plus
   whatever RAM_EMU has left plus pages the pager may later donate. 256 pages
   is 1 MB of arena, comfortably past the 262,144 B an EGA mode wants. */
#define ARENA_MAX_PAGES 256u

typedef struct {
    uint8_t *base;
    uint32_t pages;
    uint32_t first;             /* index of its first page in the bitmap */
} arena_region_t;

static arena_region_t regions[X86_ARENA_REGIONS];
static uint32_t region_count;
static uint32_t total_pages;

/* One byte per page: 0 = free, else claim id + 1. A byte rather than a bit
   because it has to say WHO holds the page -- a release by id is the whole
   lifecycle, and a bitmap would need a parallel owner array to do it. */
static uint8_t page_owner[ARENA_MAX_PAGES];

typedef struct {
    uint32_t count;
    uint32_t block_pages;
    uint32_t first[X86_ARENA_BLOCKS];   /* bitmap index of each block */
    uint8_t  held;
} arena_claim_t;

static arena_claim_t claims[X86_ARENA_CLAIMS];

/* ---- pin state ---------------------------------------------------------- *
 *
 * One bit per guest page over the whole 16 MB fold window: 512 bytes. Sized
 * from FOLD_ENTRIES rather than from the 8 MB the guest space is today, so a
 * page index the fold can produce can always be asked about -- a bitmap that
 * is shorter than the index space is read past its end by exactly the stray
 * access it exists to catch.
 */
static uint8_t pin_bits[FOLD_ENTRIES / 8u];

/* The live-pointer ring. Eight entries: x86_guest_ptr() hands out at most a
   handful of pointers between one guest re-entry and the next, and a scan of
   eight is nothing on a path taken only when a page is evicted. 0xFFFFFFFF is
   the empty slot, because page 0 is a real and frequently used page. */
#define LIVEMAP_SLOTS 8u
static uint32_t livemap[LIVEMAP_SLOTS];
static uint32_t livemap_next;
static uint8_t  livemap_armed;      /* has anything ever been noted? */

static void arena_recount(void)
{
    uint32_t used = 0, run = 0, largest = 0;
    for (uint32_t r = 0; r < region_count; r++) {
        run = 0;
        for (uint32_t i = 0; i < regions[r].pages; i++) {
            if (page_owner[regions[r].first + i]) {
                used++;
                run = 0;
            } else {
                run++;
                if (run > largest)
                    largest = run;
            }
        }
    }
    x86_arena_used    = used * X86_ARENA_PAGE;
    x86_arena_free    = (total_pages - used) * X86_ARENA_PAGE;
    x86_arena_largest = largest * X86_ARENA_PAGE;
}

void x86_arena_reset(void)
{
    memset(regions, 0, sizeof(regions));
    memset(page_owner, 0, sizeof(page_owner));
    memset(claims, 0, sizeof(claims));
    region_count = 0;
    total_pages = 0;
    x86_arena_regions = 0;
    x86_arena_bytes = 0;
    x86_arena_deficit = 0;
    arena_recount();
}

int x86_arena_add_region(void *base, uint32_t bytes)
{
    if (region_count >= X86_ARENA_REGIONS || !base)
        return -1;
    /* Round INWARD at both ends. An arena that hands out a partial page at
       either end is handing out memory it does not own -- and the caller of a
       claim gets a whole page and will use all of it. */
    uint32_t start = ((uint32_t)(uintptr_t)base + X86_ARENA_PAGE - 1u)
                     & ~(X86_ARENA_PAGE - 1u);
    uint32_t end = ((uint32_t)(uintptr_t)base + bytes) & ~(X86_ARENA_PAGE - 1u);
    if (end <= start)
        return -1;
    uint32_t pages = (end - start) / X86_ARENA_PAGE;
    if (total_pages + pages > ARENA_MAX_PAGES)
        pages = ARENA_MAX_PAGES - total_pages;
    if (!pages)
        return -1;

    regions[region_count].base  = (uint8_t *)(uintptr_t)start;
    regions[region_count].pages = pages;
    regions[region_count].first = total_pages;
    total_pages += pages;
    region_count++;

    x86_arena_regions = region_count;
    x86_arena_bytes   = total_pages * X86_ARENA_PAGE;
    arena_recount();
    return 0;
}

int x86_arena_drop_last_region(void *base)
{
    if (!region_count)
        return -1;
    arena_region_t *r = &regions[region_count - 1u];
    if (base && (uint8_t *)base != r->base)
        return -1;              /* not the extent the caller lent us */
    for (uint32_t i = 0; i < r->pages; i++) {
        if (page_owner[r->first + i])
            return -1;          /* something is still reading it */
    }
    total_pages -= r->pages;
    memset(r, 0, sizeof(*r));
    region_count--;
    x86_arena_regions = region_count;
    x86_arena_bytes   = total_pages * X86_ARENA_PAGE;
    arena_recount();
    return 0;
}

/* Find a run of `pages` free pages inside ONE region. Returns the bitmap index
   or ARENA_MAX_PAGES if there is none. Blocks may not span regions: the
   regions are disjoint extents of AXI and a "contiguous" run across two of
   them would be contiguous only in the bitmap. */
static uint32_t find_run(uint32_t pages)
{
    for (uint32_t r = 0; r < region_count; r++) {
        uint32_t run = 0;
        for (uint32_t i = 0; i < regions[r].pages; i++) {
            uint32_t idx = regions[r].first + i;
            if (page_owner[idx]) {
                run = 0;
                continue;
            }
            if (++run == pages)
                return idx + 1u - pages;
        }
    }
    return ARENA_MAX_PAGES;
}

static uint8_t *page_addr(uint32_t idx)
{
    for (uint32_t r = 0; r < region_count; r++) {
        if (idx >= regions[r].first && idx < regions[r].first + regions[r].pages)
            return regions[r].base + (idx - regions[r].first) * X86_ARENA_PAGE;
    }
    return NULL;
}

void x86_arena_release(uint32_t id)
{
    if (id >= X86_ARENA_CLAIMS || !claims[id].held)
        return;
    for (uint32_t i = 0; i < total_pages; i++) {
        if (page_owner[i] == (uint8_t)(id + 1u))
            page_owner[i] = 0;
    }
    memset(&claims[id], 0, sizeof(claims[id]));
    arena_recount();
}

int x86_arena_held(uint32_t id)
{
    if (id >= X86_ARENA_CLAIMS)
        return 0;
    return claims[id].held ? 1 : 0;
}

/*
 * The whole claim, or none of it.
 *
 * Blocks are placed one at a time and UNWOUND on the first failure, which is
 * what makes "all or nothing" true rather than intended: a claim that placed
 * three of four EGA planes and returned an error would leave 192 KB of the
 * arena held by a claim that does not exist, and the next mode change would
 * refuse for a reason nothing could explain.
 */
static int claim_inner(uint32_t id, uint32_t count, uint32_t block_bytes,
                       void **out, int commit)
{
    if (id >= X86_ARENA_CLAIMS || !count || count > X86_ARENA_BLOCKS
        || !block_bytes) {
        x86_arena_deficit = 0;
        x86_arena_refusals++;
        return -1;
    }

    uint32_t block_pages = (block_bytes + X86_ARENA_PAGE - 1u) / X86_ARENA_PAGE;
    uint8_t saved[ARENA_MAX_PAGES];
    memcpy(saved, page_owner, sizeof(saved));

    /* A re-claim releases the OLD claim first, so a mode change does not have
       to be sequenced by the caller and cannot leak. On failure the saved
       bitmap puts it back exactly as it was, including the old claim. */
    if (claims[id].held) {
        for (uint32_t i = 0; i < total_pages; i++) {
            if (page_owner[i] == (uint8_t)(id + 1u))
                page_owner[i] = 0;
        }
    }

    uint32_t placed = 0;
    uint32_t first[X86_ARENA_BLOCKS];
    for (; placed < count; placed++) {
        uint32_t idx = find_run(block_pages);
        if (idx >= ARENA_MAX_PAGES)
            break;
        first[placed] = idx;
        for (uint32_t p = 0; p < block_pages; p++)
            page_owner[idx + p] = (uint8_t)(id + 1u);
    }

    if (placed < count) {
        /* Unwind completely, then say by how much it missed. The deficit is
           what a refusal message quotes, and it is the number that turns "EGA
           failed" into "EGA needs 118,784 bytes more than exist". */
        memcpy(page_owner, saved, sizeof(saved));
        x86_arena_deficit = (count - placed) * block_pages * X86_ARENA_PAGE;
        x86_arena_refusals++;
        arena_recount();
        return -1;
    }

    if (!commit) {
        memcpy(page_owner, saved, sizeof(saved));
        arena_recount();
        return 0;
    }

    claims[id].count = count;
    claims[id].block_pages = block_pages;
    claims[id].held = 1;
    for (uint32_t i = 0; i < count; i++) {
        claims[id].first[i] = first[i];
        if (out)
            out[i] = page_addr(first[i]);
    }
    x86_arena_deficit = 0;
    x86_arena_claims_ok++;
    arena_recount();
    return 0;
}

int x86_arena_claim(uint32_t id, uint32_t count, uint32_t block_bytes,
                    void **out)
{
    return claim_inner(id, count, block_bytes, out, 1);
}

int x86_arena_would_fit(uint32_t id, uint32_t count, uint32_t block_bytes)
{
    /* A dry run through the same code, not a separate estimator. Two answers
       that can disagree is how a mode gets adopted and then refuses to
       allocate -- which is the failure the refusal rule exists to prevent. */
    uint32_t refusals = x86_arena_refusals;
    uint32_t deficit = x86_arena_deficit;
    int rc = claim_inner(id, count, block_bytes, NULL, 0);
    if (rc == 0) {
        x86_arena_refusals = refusals;
        x86_arena_deficit = deficit;
    }
    return rc == 0;
}

/* ---- pins --------------------------------------------------------------- */

void x86_pin_pages(uint32_t first_page, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t p = first_page + i;
        if (p >= FOLD_ENTRIES)
            return;
        uint8_t bit = (uint8_t)(1u << (p & 7u));
        if (!(pin_bits[p >> 3] & bit)) {
            pin_bits[p >> 3] |= bit;
            x86_pinned_pages++;
        }
    }
}

void x86_unpin_pages(uint32_t first_page, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t p = first_page + i;
        if (p >= FOLD_ENTRIES)
            return;
        uint8_t bit = (uint8_t)(1u << (p & 7u));
        if (pin_bits[p >> 3] & bit) {
            pin_bits[p >> 3] &= (uint8_t)~bit;
            x86_pinned_pages--;
        }
    }
}

int x86_page_is_pinned(uint32_t page)
{
    if (page >= FOLD_ENTRIES)
        return 1;              /* out of range is never a victim */
    return (pin_bits[page >> 3] >> (page & 7u)) & 1u;
}

/* ---- the live-pointer ring ---------------------------------------------- */

void x86_livemap_note(uint32_t page)
{
    livemap[livemap_next] = page;
    livemap_next = (livemap_next + 1u) % LIVEMAP_SLOTS;
    livemap_armed = 1;
}

void x86_livemap_clear(void)
{
    if (!livemap_armed)
        return;
    for (uint32_t i = 0; i < LIVEMAP_SLOTS; i++)
        livemap[i] = 0xFFFFFFFFu;
    livemap_next = 0;
    livemap_armed = 0;
}

int x86_livemap_holds(uint32_t page)
{
    /* The ring is .bss, so it starts as sixteen zeroes -- and page 0 is a real,
       constantly used page. Without this guard every eviction of page 0 would
       be vetoed from boot by a pointer that was never handed out. */
    if (!livemap_armed)
        return 0;
    for (uint32_t i = 0; i < LIVEMAP_SLOTS; i++) {
        if (livemap[i] == page) {
            x86_livemap_hits++;
            return 1;
        }
    }
    return 0;
}
