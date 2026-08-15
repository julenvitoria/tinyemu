/*
 * The pagefile slot ring. The measurement that forced it, and the two rules
 * that make it safe, are in x86_pgfile.h.
 *
 * It is a separate translation unit from src/main.c ON PURPOSE: src/main.c is
 * firmware-only and cannot be driven at tier 1, so the allocator that decides
 * where a guest page's bytes end up would have been the one part of the
 * writeback path with no test. Here it is ordinary C over a RAM array's worth
 * of state and tests/test_paging.py drives it directly.
 */
#include <string.h>

#include "x86_pgfile.h"

volatile uint32_t x86_pgf_slot_reuses = 0;
volatile uint32_t x86_pgf_slot_full   = 0;
volatile uint32_t x86_pgf_unpageable  = 0;

/* Sized for the shipping windows: 160 pages of conventional RAM and the 256
   of EMS backing. x86_pgf_map_init() may narrow it; it may not widen it.
   The tier-1 harness builds it small -- its .bss is 176 bytes from overflowing
   RAM_EMU and the ring's logic does not depend on either size. */
#ifndef X86_PGF_MAP_MAX
#define X86_PGF_MAP_MAX  416u
#endif
#define PGF_MAP_MAX  X86_PGF_MAP_MAX

static uint16_t map_slot[PGF_MAP_MAX];
static uint32_t own_pages, aux_first, aux_pages, map_entries;
static uint8_t  slot_live[(X86_PGF_RING_SLOTS + 7u) / 8u];
static uint32_t cursor;
/* The high-water mark of slots ever handed out, so "is this an append or a
   rewrite" is answerable without asking the filesystem. */
static uint32_t slots_used;

void x86_pgf_map_init(uint32_t op, uint32_t af, uint32_t ap)
{
    if (op + ap > PGF_MAP_MAX) {
        /* Refuse rather than write past the map. Every page then reads as
           unpageable, which is loud (x86_pgf_unpageable climbs and every
           writeback fails) instead of corrupting the slot next door. */
        own_pages = aux_pages = map_entries = 0;
        aux_first = 0;
        return;
    }
    own_pages = op;
    aux_first = af;
    aux_pages = ap;
    map_entries = op + ap;
    x86_pgf_map_reset();
}

void x86_pgf_map_reset(void)
{
    for (uint32_t i = 0; i < PGF_MAP_MAX; i++)
        map_slot[i] = X86_PGF_NO_SLOT;
    memset(slot_live, 0, sizeof(slot_live));
    cursor = 0;
    slots_used = 0;
    x86_pgf_slot_reuses = 0;
    x86_pgf_slot_full = 0;
    x86_pgf_unpageable = 0;
}

static uint32_t map_index(uint32_t page)
{
    if (page < own_pages)
        return page;
    if (aux_pages && page >= aux_first && page < aux_first + aux_pages)
        return own_pages + (page - aux_first);
    return map_entries ? map_entries : 1u;   /* never a valid index */
}

static int is_live(uint32_t s)
{
    return (slot_live[s >> 3] >> (s & 7u)) & 1u;
}

static void set_live(uint32_t s, int live)
{
    if (live) slot_live[s >> 3] |= (uint8_t)(1u << (s & 7u));
    else      slot_live[s >> 3] &= (uint8_t)~(1u << (s & 7u));
}

int x86_pgf_slot_live(uint32_t slot)
{
    return slot < X86_PGF_RING_SLOTS ? is_live(slot) : 0;
}

uint32_t x86_pgf_slot_of(uint32_t page)
{
    uint32_t i = map_index(page);
    if (i >= map_entries)
        return X86_PGF_NO_SLOT;
    return map_slot[i];
}

uint32_t x86_pgf_slot_claim(uint32_t page)
{
    uint32_t i = map_index(page);
    if (i >= map_entries) {
        x86_pgf_unpageable++;
        return X86_PGF_NO_SLOT;
    }

    /* Sweep once. The cursor is what makes a slot's next rewrite a whole
       revolution away, which is the entire point of the ring. */
    for (uint32_t n = 0; n < X86_PGF_RING_SLOTS; n++) {
        uint32_t s = cursor;
        cursor = (cursor + 1u) % X86_PGF_RING_SLOTS;
        if (!is_live(s)) {
            if (s < slots_used)
                x86_pgf_slot_reuses++;
            return s;
        }
    }

    /* Cannot happen while the ring is larger than the pageable space -- but
       "cannot happen" is how pages get lost, so it is counted, and the page's
       OWN slot is the only one it is safe to overwrite. */
    x86_pgf_slot_full++;
    return map_slot[i];
}

void x86_pgf_commit(uint32_t page, uint32_t slot, int ok)
{
    uint32_t i = map_index(page);
    if (i >= map_entries || slot >= X86_PGF_RING_SLOTS)
        return;

    uint32_t old = map_slot[i];
    if (!ok) {
        /* Rule 1: the page stays on the copy it already had. The claim is
           only released if it was not that copy. */
        if (slot != old)
            set_live(slot, 0);
        return;
    }
    if (old != X86_PGF_NO_SLOT && old != slot)
        set_live(old, 0);
    set_live(slot, 1);
    map_slot[i] = (uint16_t)slot;
    if (slot + 1u > slots_used)
        slots_used = slot + 1u;
}
