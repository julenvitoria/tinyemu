/*
 * The VRAM arena: AXI pages claimed at TOP PRIORITY and never evicted.
 *
 * ---- Why an arena and not four permanent allocations --------------------
 *
 * The display's storage was three fixed blocks that all existed at once: 32 KB
 * of CGA VRAM at 0xB8000, a 64 KB mode 13h aperture at 0xA0000, and nothing at
 * all for EGA. That is 96 KB permanently spent to serve ONE mode at a time,
 * and it is why EGA 0Dh has never been reachable: Commander Keen 4 wants
 * 262,144 bytes of plane storage and the tree has 45,056 free.
 *
 * The modes are mutually exclusive. A guest is in mode 3 or mode 13h or mode
 * 0Dh, never two, and a mode set does not preserve the previous mode's
 * contents. So the storage can be one pool that is re-claimed on every mode
 * change, and the peak demand is the LARGEST mode rather than the SUM.
 *
 * ---- The rule that makes this safe: never partially satisfy ---------------
 *
 * A claim is ALL OR NOTHING. If every block cannot be placed, none is, the
 * claim fails, and the exact deficit is recorded for a probe to read.
 *
 * This is load-bearing and it is not caution for its own sake. The previous
 * port MEASURED a partial EGA card as producing worse output than no EGA at
 * all: 8 KB per plane truncates cleanly, but 16 KB and 32 KB alias the
 * floating pages onto each other and the screen fills with noise that looks
 * like a rendering defect rather than a memory shortage. A mode that cannot be
 * satisfied must be REFUSED and logged -- which x86_video.c already knows how
 * to do for a mode nothing renders.
 *
 * ---- Blocks, not bytes ----------------------------------------------------
 *
 * A claim asks for `count` blocks of `block_bytes`, and each BLOCK is
 * contiguous while the blocks need not be contiguous with each other. That is
 * exactly the shape of the hardware: an EGA claim is four independent 64 KB
 * planes, and requiring all 256 KB in one run would refuse a claim that four
 * separate runs can satisfy. CGA is one block of 32 KB, mode 13h one of 64 KB.
 *
 * ---- Pinning, and the hazard it has to cover ------------------------------
 *
 * Every arena page is pinned: it is display storage, the blitter reads it every
 * frame, and evicting it would be visible instantly. Pins are expressed as a
 * bitmap indexed by GUEST PAGE NUMBER so the victim picker in the pager can
 * reject one in a single test, and so a page donated out of the guest RAM pool
 * carries its pin with it.
 *
 * The second half is the hazard the previous project paid for: `cow_lost == 0`
 * is not a sufficient invariant, because it misses evicting a page that a HOST
 * POINTER is still pointing into -- 5 such events produced 26,329 wrong bytes.
 * x86_guest_ptr() hands those pointers out and no caller releases them, so a
 * reference count cannot work. What does work is a small ring of the pages
 * most recently handed out: eviction refuses anything in it, and the ring is
 * cleared each time the emulator re-enters the guest, which is the point at
 * which no host pointer can still be live. One store on the host path, an
 * eight-entry scan on the eviction path, and no bookkeeping for callers to
 * forget.
 */
#ifndef X86_ARENA_H
#define X86_ARENA_H

#include <stdint.h>

#define X86_ARENA_PAGE     4096u
#define X86_ARENA_REGIONS  4u     /* disjoint AXI extents the arena may use */
#define X86_ARENA_CLAIMS   5u     /* one per consumer; see the ids below */
#define X86_ARENA_BLOCKS   8u     /* blocks in a single claim (EGA needs 4) */

/* Claim ids. One per consumer, fixed rather than allocated, so a leaked claim
   is impossible and a probe can name what holds the arena. */
#define X86_ARENA_ID_CGA    0u    /* 0xB8000, 32 KB */
#define X86_ARENA_ID_VGA13  1u    /* 0xA0000, 64 KB linear */
#define X86_ARENA_ID_EGA    2u    /* 0xA0000, 4 planes x 64 KB */
#define X86_ARENA_ID_SPARE  3u    /* Mode Y's off-screen cache */
/* Mode Y's REAL display pages 1 and 2, 64 KB each, out of frames donated by
   the pager. Its own id rather than SPARE's second block because the cache is
   sized by a fitting walk and these are fixed -- one claim that could be
   partially satisfied is exactly what the all-or-nothing rule forbids. */
#define X86_ARENA_ID_MODEYP 4u

/* Give the arena an extent of AXI. Both ends are rounded INWARD to a page --
   an arena that hands out a partial page at either end is an arena that hands
   out memory it does not own. Returns 0 on success, -1 if there is no region
   slot left or the extent holds no whole page. */
int x86_arena_add_region(void *base, uint32_t bytes);

/*
 * Give the LAST region back. Returns 0 if it went, -1 if it did not.
 *
 * Only the last, and only when nothing in it is claimed. Regions index a single
 * page_owner[] by a running `first`, so removing one from the middle would
 * renumber every region above it and silently repoint every live claim -- and
 * "the display is now reading someone else's pages" is not a failure a counter
 * would catch. The pager's donated extent is always the last one, which is the
 * only caller this needs to serve.
 */
int x86_arena_drop_last_region(void *base);

/* Drop every region and every claim. The core calls it once before it builds
   the arena so a re-init cannot inherit half a map, and the tier-1 harness
   calls it to run against an arena whose size IT chose -- a test that asserts
   on whatever the bonus pool happened to be is asserting on someone else's
   number. */
void x86_arena_reset(void);

/*
 * Claim `count` blocks of `block_bytes` for `id`. All or nothing.
 *
 * On success the block host pointers are written to out[0..count-1] and 0 is
 * returned. On failure nothing is allocated, out is untouched, -1 is returned
 * and x86_arena_deficit holds the bytes that could not be placed -- which is
 * the number a refusal message should quote, because "EGA needs 118,784 more
 * bytes than exist" is actionable and "EGA failed" is not.
 *
 * Re-claiming an id that is already held releases the old claim FIRST, so a
 * mode change cannot leak and does not need the caller to sequence it.
 */
int x86_arena_claim(uint32_t id, uint32_t count, uint32_t block_bytes,
                    void **out);
void x86_arena_release(uint32_t id);

/* Is `id` currently holding arena pages? The mode path has to know: CGA VRAM
   falls back to a plain RAM_EMU allocation when the arena could not place it
   (src/main.c), and lending storage the arena never owned would hand the guest
   a pointer into memory the arena is about to give to someone else. */
int x86_arena_held(uint32_t id);

/* Would this claim succeed, without making it? For the mode table, which has
   to decide whether to adopt a mode before it commits to it. */
int x86_arena_would_fit(uint32_t id, uint32_t count, uint32_t block_bytes);

/* ---- pinning, consulted by the pager's victim picker -------------------- */

/* Pin/unpin a range of GUEST page numbers. Pinned pages are never evictable.
   Idempotent: pinning a pinned page is not an error and not a change. */
void x86_pin_pages(uint32_t first_page, uint32_t count);
void x86_unpin_pages(uint32_t first_page, uint32_t count);
int  x86_page_is_pinned(uint32_t page);

/* Note that a host pointer into `page` has just been handed out. Called from
   x86_guest_ptr(); see the header comment for why this is a ring and not a
   refcount. */
void x86_livemap_note(uint32_t page);
/* Clear the ring. Called immediately before re-entering the guest, which is
   the only moment at which no host pointer can still be live. */
void x86_livemap_clear(void);
/* Is a host pointer into this page possibly still live? The victim picker's
   second veto, after the pin bitmap. */
int  x86_livemap_holds(uint32_t page);

/* ---- host-readable state; all of it is the deliverable ------------------ */
extern volatile uint32_t x86_arena_bytes;      /* total the arena was given   */
extern volatile uint32_t x86_arena_used;       /* currently claimed           */
extern volatile uint32_t x86_arena_free;       /* claimable right now         */
extern volatile uint32_t x86_arena_largest;    /* biggest contiguous run      */
extern volatile uint32_t x86_arena_deficit;    /* bytes the last refusal wanted */
extern volatile uint32_t x86_arena_refusals;   /* claims refused outright     */
extern volatile uint32_t x86_arena_claims_ok;  /* claims satisfied            */
extern volatile uint32_t x86_arena_regions;    /* extents actually held       */
extern volatile uint32_t x86_pinned_pages;     /* guest pages pinned          */
extern volatile uint32_t x86_livemap_hits;     /* evictions the ring vetoed   */

#endif /* X86_ARENA_H */
