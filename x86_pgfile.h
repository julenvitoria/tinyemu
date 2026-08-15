/*
 * WHERE A PAGE'S COPY LIVES IN THE PAGEFILE -- the slot ring.
 *
 * THIS EXISTS BECAUSE OF ONE MEASUREMENT, taken on silicon with
 * `X86_PGF_BENCH=1` (src/main.c, pgf_bench), every arm 4 KB at a time and each
 * shape run twice in opposite order so warm-up cannot explain it:
 *
 *   4 KB APPENDED past end-of-file        12.2 - 13.8 ms
 *   4 KB rewritten IN PLACE               60.1 - 67.3 ms
 *   32 KB rewritten in ONE call           60.4 ms per 4 KB
 *   512 B x 64 over the same 32 KB        62.0 ms per 4 KB
 *   4 KB READ                              3.8 ms
 *
 * The cost is not the call size and not the number of calls -- 32 KB in one
 * write costs exactly what 64 writes of 512 B cost, which is why a write-behind
 * queue would buy nothing and is not here. It is not the cluster-chain walk
 * either: of KEEN4's 86 writebacks on hardware, 85 landed in already-allocated
 * clusters, and all the seeks together came to 340 ms against 4.0 s inside
 * f_write. The cost is REWRITING SECTORS THAT WERE WRITTEN RECENTLY -- the
 * card's own garbage collection -- and the one thing a core above the ABI can
 * choose is WHERE a page lands.
 *
 * So a page's slot is no longer its page number. Slots come from a cursor
 * sweeping a ring, so a writeback lands on file space that has not been touched
 * for a whole revolution, and the map remembers where each page's copy went.
 *
 * ---- The two rules that make it safe ------------------------------------
 *
 *  1. THE MAP MOVES ONLY ON A WRITE THAT SUCCEEDED. Until then the page still
 *     points at its previous copy, so a failed writeback loses the newest
 *     contents and not, as the in-place scheme did, the only copy.
 *  2. A SLOT THAT IS LIVE IS NEVER HANDED OUT. The ring is larger than the
 *     number of pages that can exist, so a free slot always exists; the sweep
 *     checks rather than assuming, and says so if it ever fails.
 *
 * Ordering is untouched: a writeback is still synchronous and still completes
 * before the frame it came from is reused, so no read can overtake a write.
 * This module has no queue and no deferred state for that reason.
 *
 * ---- Why the ring is BOUNDED -------------------------------------------
 *
 * The card would rather the file kept growing forever. FatFs would not: the
 * firmware builds FF_USE_FASTSEEK 0, so the backward seek a refill does walks
 * the cluster chain from byte zero, and that cost grows with the offset. The
 * ring is the compromise -- big enough that an offset is not rewritten until
 * 512 other writes have happened, small enough (2 MB) that a seek into it is
 * the 4 ms one measured rather than a walk over a session's worth of log.
 */
#ifndef X86_PGFILE_H
#define X86_PGFILE_H

#include <stdint.h>

/* 512 slots = 2 MB, against at most 416 pages that can ever be live (160 of
   conventional guest RAM + 256 of EMS backing). The margin is what guarantees
   rule 2 above, and x86_pgf_slot_full is the witness if it is ever wrong. */
#ifndef X86_PGF_RING_SLOTS
#define X86_PGF_RING_SLOTS  512u
#endif

#define X86_PGF_NO_SLOT     0xFFFFu

/* The pageable pages, compacted. [0, own) is conventional RAM
   (x86_pager_own) and [aux, aux+aux_pages) is the EMS backing window
   (x86_pager_own_aux); nothing else is ever handed to a backing-store
   callback, so the map is hundreds of entries rather than the 2,048 a
   page-indexed one would need -- 4 KB of RAM_EMU is a whole guest frame. */
void     x86_pgf_map_init(uint32_t own_pages, uint32_t aux_first,
                          uint32_t aux_pages);
void     x86_pgf_map_reset(void);
/* Where `page`'s copy is, or X86_PGF_NO_SLOT if it has never been written. */
uint32_t x86_pgf_slot_of(uint32_t page);
/* The slot the next write of `page` should use. X86_PGF_NO_SLOT if the page is
   not pageable at all. Nothing is committed until x86_pgf_commit(). */
uint32_t x86_pgf_slot_claim(uint32_t page);
/* `ok` != 0 moves the page onto `slot` and frees its old one; `ok` == 0 puts
   the claim back and leaves the page on the copy it already had. */
void     x86_pgf_commit(uint32_t page, uint32_t slot, int ok);
int      x86_pgf_slot_live(uint32_t slot);   /* for the tests and nothing else */

extern volatile uint32_t x86_pgf_slot_reuses; /* claims onto a used slot */
extern volatile uint32_t x86_pgf_slot_full;   /* the ring had NO free slot */
extern volatile uint32_t x86_pgf_unpageable;  /* a page outside both windows */

#endif /* X86_PGFILE_H */
