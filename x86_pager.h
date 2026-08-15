/*
 * Demand paging over the fold table.
 *
 * ---- What makes this cost nothing on the fast path -----------------------
 *
 * A non-resident page holds FOLD_INVALID (0x80000000) in its fold entry, so
 * the check the interpreter ALREADY does -- `ADDS Rh, Rk, Rlin; BMI` -- catches
 * it. There is no extra test, no permission bits and no second table. The
 * whole pager lives behind a branch the fast path was taking anyway, which is
 * why not one byte of assembly changes to add it.
 *
 * The trap hands the slow path `bad`, the value the check produced, which for
 * an unmapped page is `FOLD_INVALID + lin`. **The faulting linear address
 * falls straight out of it**: lin = bad - 0x80000000. That is the property
 * that makes demand paging reachable without plumbing a fault address down
 * from ~181 trap sites, and it is why 0x80000000-0x8FFFFFFF (a fold miss) and
 * 0x90000000+ (a NOR-backed page) are distinguishable by inspection.
 *
 * ---- Tiers -------------------------------------------------------------
 *
 * A page that is not resident is in exactly one of three places:
 *
 *   ZERO  never written. Costs nothing anywhere. Most of an 8 MB space.
 *   NOR   a clean page of the boot image, read in place over XIP (x86_nor.c).
 *         Zero AXI, zero SD.
 *   FILE  a page that was modified and then evicted, in the pagefile on SD.
 *
 * ---- Dirty tracking, without permission bits ---------------------------
 *
 * The fold has no write bit and cannot get one without costing the fast path,
 * so a write to a page that is already resident cannot be seen. The usual
 * answer -- treat every resident page as dirty -- is what produced the
 * previous project's 566 MB of pagefile traffic and 98.6% wasted writes.
 *
 * This does it the other way round: **decide at EVICTION by comparing against
 * the tier's authoritative copy.** A ZERO-tier page is scanned for a non-zero
 * byte; a NOR-tier page is compared against its snapshot. Either way the
 * answer is EXACT -- not a heuristic, not a conservative approximation -- and
 * a page that matches is DROPPED and reverts to its tier, costing no SD I/O at
 * all. A 4 KB compare is ~1 us against a pagefile write measured in
 * milliseconds, so the comparison is free at the scale it is traded against.
 *
 * FILE-tier pages have no in-memory authoritative copy to compare with, so
 * they are written back unconditionally and x86_pg_wb_file counts them. That
 * is the one conservative case and it is bounded: FILE tier only ever contains
 * pages that were genuinely modified at some point, so it does not contain the
 * read-only code and tables that made the previous project's figures so bad --
 * those are NOR tier and are never written.
 *
 * ...with ONE exception, and it is free. The ZERO scan is one pass over 4 KB
 * and a FILE-tier page gets the same pass for nothing: a page the guest has
 * zeroed out again (a freed DOS block, a cleared buffer) REVERTS to ZERO tier
 * and is dropped with no write. x86_pg_wb_zero counts those -- "was dirty, but
 * became all-zero" -- and they are the only FILE-tier eviction that does not
 * reach the card.
 *
 * ---- Which counter says what -------------------------------------------
 *
 *   x86_pg_evictions   every victim, whatever happened to it.
 *   x86_pg_drop_clean  every eviction that cost NO backing-store write, i.e.
 *                      the ZERO/NOR match AND the FILE-became-zero revert.
 *   x86_pg_wb_zero     the FILE-became-zero subset of drop_clean.
 *   x86_pg_wb_file     evictions actually written to the pagefile.
 *   x86_pg_io_errors   a writeback that could not land. THE PAGE IS GONE.
 *   drop_clean + wb_file + io_errors == evictions, always. A run where that
 *   does not hold has lost a page somewhere without saying so, which is the
 *   shape of every memory bug this project has shipped.
 *
 * The victim picker refuses a page it cannot write when no backing store is
 * set, so io_errors should stay 0 -- exhaustion (a clean X86_EXIT_MEM_FAULT)
 * is the right answer there and losing a page is not. That rule is not
 * hypothetical: MEASURED on KEEN4 under gwemu before it existed, 38 evictions
 * and 38 io_errors, i.e. 38 pages of the guest's memory silently gone.
 *
 * ---- What a victim may NOT be -------------------------------------------
 *
 * Two vetoes, both from x86_arena.h and both paid for by the previous project:
 * a PINNED page is display storage the blitter reads every frame, and a page in
 * the LIVE-POINTER RING has a raw host pointer outstanding that no caller will
 * ever release. The prior port measured 5 evictions of the second kind
 * producing 26,329 wrong bytes with its own `cow_lost == 0` green throughout,
 * which is why "no counter complained" is not evidence here.
 *
 * ---- Policy: a clock, deliberately --------------------------------------
 *
 * PLAN.md Phase 1 records the measurement that settles this: eviction POLICY
 * was worth at most 2.1x in the prior project while the pool-SIZE cliff was
 * 350x. So this is a second-chance clock over the hotness byte the fault path
 * and the EIP sampler already maintain -- no list, no per-access bookkeeping,
 * no DTCM. The care goes into the dirty test being EXACT instead.
 */
#ifndef X86_PAGER_H
#define X86_PAGER_H

#include <stdint.h>

#define X86_PG_SIZE   4096u
#define X86_PG_SHIFT  12u
/* The guest space the pager owns, in pages. 8 MB. Sized for the space rather
   than for the 640 KB a real-mode DOS guest can address, so the structures do
   not have to be rebuilt when something can reach above 1 MB. */
#define X86_PG_PAGES  2048u

/* Backing-store callbacks. The pager does not know what an SD card is: the ABI
   lives in src/main.c and keeping it there is what lets the whole mechanism be
   driven at tier 1 against a plain RAM buffer standing in for the pagefile,
   with no card, no FatFs and no firmware. Both return 0 on success.
   `slot` is the page's index in the pagefile; the mapping is direct, so slot
   IS the guest page number and no allocator is needed. */
typedef int (*x86_pg_read_fn)(uint32_t slot, void *dst);
typedef int (*x86_pg_write_fn)(uint32_t slot, const void *src);

/* Per-page state: flags, hotness and frame index for every page of the space.
   8 KB, and it belongs in DTCM -- it is read on every fault. Supplied by the
   caller because a core gets its DTCM from mem_ctl() at runtime, and because
   8 KB of static .bss overflows RAM_EMU in the tier-1 harness. */
/*
 * THE SEAM SLACK. host = lin + k and k is per granule, so two adjacent guest
 * pages are only adjacent in HOST memory by luck: frames are handed out in
 * FAULT order. A guest 16- or 32-bit access whose bytes span the boundary
 * resolves one pointer from the LOW page's k and then runs off the top of that
 * frame -- into the next frame, which belongs to an unrelated guest page.
 *
 * Every frame therefore carries X86_PG_SLACK bytes past its top that belong to
 * nobody, and x86_seam.c pulls the neighbour's leading bytes into them before a
 * straddling access and pushes back whatever the instruction wrote. This is the
 * prior DOS port's mechanism (references/8086tiny/8086tiny.c:7176-7188,
 * DOS_COW_SLACK = 8) and the size is the same 8: the widest thing a handler
 * reads through one folded pointer is a 6-byte far pointer (LDS/LES), and 8
 * keeps the stride 8-aligned.
 *
 * THE PRICE IS THE STRIDE, NOT A SECOND TABLE: a frame costs 4,104 bytes to
 * cover 4,096 of guest space, so a pool sized for N pages must be N * 4,104.
 */
#define X86_PG_SLACK   8u
#define X86_PG_STRIDE  (X86_PG_SIZE + X86_PG_SLACK)
/* What a caller must hand x86_pager_add_frames() to get `pages` frames. */
#define X86_PG_POOL_BYTES(pages) ((pages) * X86_PG_STRIDE)

#define X86_PG_TABLE_BYTES (4u * X86_PG_PAGES)
int  x86_pager_set_tables(void *block, uint32_t bytes);

/* Hand the pager the AXI it may hand out, in whole pages. May be called more
   than once; extents need not be contiguous with each other. */
int  x86_pager_add_frames(void *base, uint32_t bytes);
/* Drop every frame and every page's state. */
void x86_pager_reset(void);
void x86_pager_set_backing(x86_pg_read_fn rd, x86_pg_write_fn wr);
/* Tell the pager which guest pages it owns. Pages outside this window are left
   exactly as they are -- VRAM, the mode 13h aperture, the open-bus filler and
   the BIOS id page are fixed mappings and must never be paged. */
void x86_pager_own(uint32_t first_page, uint32_t count);
/*
 * A SECOND, disjoint window, for storage the guest cannot address directly.
 *
 * EMS needs its logical pages to BE pager pages -- they are mapped into the
 * page frame and read by ordinary instructions, so they have to be able to
 * occupy a frame (x86_ems.h) -- but they live at linear 4 MB, ten megabytes
 * above anything a real-mode segment:offset can name.
 *
 * A second window rather than one widened one, deliberately. Widening
 * x86_pager_own() to cover [0, 5 MB) would make every page between 640 KB and
 * 4 MB ownable, and a stray guest access up there would quietly be handed a
 * frame instead of the clean X86_EXIT_MEM_FAULT it has earned. The windows are
 * checked separately and nothing between them is ours.
 *
 * THERE IS MORE THAN ONE OF THEM, and the second user is the HMA. EMS lives at
 * linear 4 MB; the sixteen pages at 1 MB are the other storage that is neither
 * inside the 640 KB window nor a fixed mapping, and they come and go with the
 * A20 gate (x86_mem_wrap_1mb()). So this is an ADD, not a store:
 *
 *   count != 0   register (or replace, matching on first_page) a window.
 *   count == 0   drop the window that starts at first_page.
 *   (0, 0)       drop them all -- the "no aux windows" reset the harness uses.
 *
 * A window is not a claim on frames: pages inside it are ordinary pageable
 * guest pages, filled on fault and evictable like any other.
 */
void x86_pager_own_aux(uint32_t first_page, uint32_t count);

/*
 * Re-install one page's fold entry from the pager's own record: the frame's
 * host address if it is resident, FOLD_INVALID if it is not.
 *
 * This exists for the A20 gate. While the gate is MASKED the sixteen HMA fold
 * entries belong to the WRAP -- they alias low memory -- and the pager must
 * not write them even for a page it still holds a frame for. When the gate
 * OPENS those entries become the HMA's own again, and every page that was
 * resident from an earlier open window has to be pointed back at its frame
 * rather than left FOLD_INVALID to fault in a second, unrelated copy.
 */
void x86_pager_refold(uint32_t page);

/*
 * Service a fold miss. `lin` is the faulting guest linear address; `write` is
 * 1 for a store. Returns the host address of that byte, or 0 if the page is
 * genuinely not ours (which is a real X86_EXIT_MEM_FAULT, not a page fault).
 *
 * Called from the interpreter's trap stubs by way of x86_nor_slow_r/_w, and
 * from x86_guest_ptr() on the host side.
 */
uint32_t x86_pager_fault(uint32_t lin, int write);

/*
 * The seam's hook into the pager. Returns the address of `page`'s eight slack
 * bytes -- the memory immediately past the top of its frame -- or NULL if the
 * page is not resident in a frame this pager owns. NULL is the honest answer
 * for a FIXED mapping (VRAM, the 13h aperture, the open-bus filler) and for a
 * NOR-backed page: those have no slack, their neighbour is not ours to write,
 * and x86_seam.c counts the refusal rather than guessing.
 */
uint8_t *x86_pager_slack(uint32_t page);

/* Where the NOR snapshot is, so a fill can read from it. Mirrored from
   x86_nor.c rather than reached into: the two files own different halves of
   the same page's life and a shared global would hide which. */
void x86_pager_set_nor(const void *base, uint32_t bytes, uint32_t first_page);

/*
 * ---- Donating frames to the VRAM arena ----------------------------------
 *
 * Measured on this tree: the arena is 143,360 B and 100% used in EGA 0Dh,
 * while KEEN4 peaks at 134 of 160 guest frames. The idle frames are the only
 * place a bigger EGA card can come from, and PLAN.md Phase 3 always intended
 * this -- "the extra 224 KB is dynamically borrowed from the guest RAM AXI
 * pool".
 *
 * The catch is CONTIGUITY. An EGA claim is one run of 4*offsets bytes and a
 * block may not span arena regions, so a donation is a run of frames taken off
 * the TOP OF THE LAST EXTENT -- frames f..f+n-1 are n*X86_PG_STRIDE contiguous
 * bytes -- handed to x86_arena_add_region(), which rounds both ends inward to a
 * page. Scattered frames would be useless.
 *
 * It is REVERSIBLE and it is SAFE ONLY BECAUSE EVICTION EXISTS. Donating
 * shrinks guest RAM below a working set that was measured once; without a
 * victim picker the next page beyond it is a hard X86_EXIT_MEM_FAULT, with one
 * it is a page-in. That is why the two landed together.
 *
 * x86_pager_donate_to_arena() gives back the bytes actually donated (0 = it
 * refused, and x86_pg_donate_refused says so). Frames in the run are evicted
 * first; a frame that cannot be evicted -- pinned, or live-mapped -- refuses
 * the whole donation rather than shrinking it, because a caller that asked for
 * 196,608 B and silently got 106,496 gets the noise the arena's
 * never-partially-satisfy rule exists to prevent.
 */
/*
 * THE RESERVE IS KEEN4'S MEASURED PEAK, NOT A CARD SIZE. It was 100, chosen to
 * fund a 63 KB/plane card, while the paragraph above already recorded that
 * KEEN4 peaks at 134 frames -- so the shipping constant knowingly left the one
 * title that reaches this path 34 frames short of its own working set.
 *
 * Under gwemu that costs nothing and reads as a healthy run: KEEN4 reaches the
 * MAIN MENU, x86_pg_exhausted stays 0, and the only symptom is a counter. On
 * SILICON a dirty eviction is a 4 KB pagefile write costing up to 433,509 us
 * (x86_pg_wb_us_worst, measured), and at reserve 100 KEEN4 paid 84 evictions
 * and 82 of those writes in the first 15 seconds. At 134 it pays ZERO --
 * measured at tier 2 and again on HARDWARE: donated=0, frames_total=160,
 * evictions=0, wb_file=0, wb_us_worst=0.
 *
 * MAX_SLICE_CEIL bounds a slice in INSTRUCTIONS and cannot bound that cost:
 * the time is spent in f_write, not in the interpreter. The reserve is the
 * only thing that decides whether those writes happen at all.
 *
 * THIS IS NOT THE KEEN4 WEDGE, AND THAT WAS CHECKED, NOT ASSUMED. The obvious
 * story -- 0Dh donates, the guest thrashes, the writeback storm stalls the
 * frame loop past the watchdog -- is wrong. With this reserve the donation is
 * refused outright and the pagefile is never touched, and KEEN4 STILL wedges
 * on silicon: guest_insns_total and frames_rendered both freeze (measured at
 * 14.9 M/347, 19.9 M/352, 31.4 M/640 frames on three runs) and WWDG resets it
 * with boot_magic=0xbad00005. The wedge is a genuine hang elsewhere; do not
 * re-derive it from these counters.
 *
 * AND THE DONATION CANNOT HELP KEEN4 AT ALL, structurally. An arena claim may
 * not span regions, so the card is the BEST SINGLE RUN -- either the arena's
 * 143,360 B (35,840 plane offsets) or the donated run, never their sum. Beating
 * the arena therefore takes ~35 frames, and Keen's 160-134 leaves 26. So above
 * a reserve that respects the working set the feature is inert for this title,
 * and the card is the arena. Shrinking the reserve to make it fire again buys
 * 61,440 offsets by running the guest 34 frames short of its own peak.
 *
 * So the floor is the working set and the card is what is left over, never the
 * other way round. A smaller card is a RENDERING defect (KEEN4's golden is
 * already known-bad for one); a card funded out of the working set is SD I/O
 * the frame loop pays for. Raise this only against a re-measured peak.
 */
#ifndef X86_PG_DONATE_RESERVE
/* -D-overridable ONLY so a measurement build can force eviction (the pagefile
   I/O investigation needs a workload that actually evicts). The DEFAULT is
   unchanged and remains the sized value below. */
#define X86_PG_DONATE_RESERVE 134u   /* frames guest RAM keeps, come what may */
#endif
/* Change the floor. The default is sized for THIS part (160 frames, KEEN4's
   134-frame peak); the tier-1 harness sets its own so the arms below are
   arithmetic the test can state rather than the shipping constant. */
void x86_pager_set_donate_reserve(uint32_t frames);
uint32_t x86_pager_donate_to_arena(uint32_t bytes);
/* The ladder: the largest run between the two, or 0. One refusal, not one per
   rung -- the EGA card is sized by what fits, so asking once for the maximum
   and giving up would leave a smaller card unbuilt. */
uint32_t x86_pager_donate_upto(uint32_t max_bytes, uint32_t min_bytes);
/* Take the donated extent back. No-op (and counted) if the arena still has a
   claim inside it -- the frames stay lent until the display lets go. */
int x86_pager_reclaim_from_arena(void);

/* ---- the donation POLICY -------------------------------------------------
 *
 * X86_PG_DONATE_RESERVE above is a BUILD-TIME constant evaluated ONCE, at mode
 * adoption, and that is the whole of the "EGA or frames" dichotomy: KEEN4's
 * intro wants a big card and few frames, its gameplay wants the opposite, and
 * one number cannot be both. The policy re-evaluates instead, once per FRAME,
 * off a working set it MEASURES:
 *
 *     reserve = max(FLOOR, x86_pg_resident_peak + MARGIN, ratchet)
 *
 * The card grows while frames are genuinely idle and the frames go back
 * BEFORE the guest evicts -- the trigger is free frames falling below MARGIN,
 * not x86_pg_evictions, because an eviction is the COST (up to 433 ms of
 * pagefile write on silicon), not a warning. See x86_pager.c for the two
 * alternatives that were rejected and why.
 *
 * With the policy disabled (X86_PG_POLICY=0, or x86_pager_policy_enable(0))
 * the reserve is exactly X86_PG_DONATE_RESERVE and the behaviour is the
 * tree's previous behaviour, byte for byte. That is the negative-control
 * build AND the tier-1 harness's mode, so the arms that state the static
 * reserve's arithmetic keep stating it.
 */
#define X86_PG_RESERVE_FLOOR   64u   /* frames the guest keeps come what may */
#define X86_PG_RESERVE_MARGIN   8u   /* runway: the shrink fires on this      */
#define X86_PG_POLICY_QUIET    60u   /* frames of calm before lending again   */

#define X86_PG_POLICY_HOLD    0
#define X86_PG_POLICY_GROW    1
#define X86_PG_POLICY_SHRINK (-1)

/* Once per frame, off the video path. GROW is PERMISSION (the caller must
   still find that a bigger card fits -- a refit costs the card's contents);
   SHRINK is an instruction, and the caller must release its claims inside the
   donated extent before the reclaim or the frames stay lent. */
int x86_pager_policy_tick(void);
uint32_t x86_pager_policy_reserve(void);
uint32_t x86_pager_policy_headroom(void);   /* bytes a donation could take now */
void x86_pager_policy_enable(int on);
void x86_pager_policy_reset(void);
/* Floor, margin and quiet period. The harness scales them to an 8-frame pool;
   the shipping core never calls it and runs on the three constants above. */
void x86_pager_policy_config(uint32_t floor, uint32_t margin, uint32_t quiet);

/* Hotness. Fed by faults (free) and by guest-EIP samples taken at each
   interpreter exit. Decayed by halving, which is one pass over 2 KB. */
void x86_pager_note_eip(uint32_t lin);
void x86_pager_decay(void);

/* ---- host-readable state ------------------------------------------------ */
extern volatile uint32_t x86_pg_frames_total;   /* AXI pages the pager holds  */
extern volatile uint32_t x86_pg_frames_free;
extern volatile uint32_t x86_pg_resident;       /* guest pages resident       */
extern volatile uint32_t x86_pg_faults_r;       /* read faults serviced       */
extern volatile uint32_t x86_pg_faults_w;
extern volatile uint32_t x86_pg_faults_host;    /* faults from x86_guest_ptr  */
extern volatile uint32_t x86_pg_fill_zero;      /* filled from the ZERO tier  */
extern volatile uint32_t x86_pg_fill_nor;       /* filled from NOR            */
extern volatile uint32_t x86_pg_fill_file;      /* read back from the pagefile */
extern volatile uint32_t x86_pg_evictions;
extern volatile uint32_t x86_pg_drop_clean;     /* evicted with NO SD write   */
extern volatile uint32_t x86_pg_wb_zero;        /* dirty, but became all-zero */
extern volatile uint32_t x86_pg_wb_file;        /* written to the pagefile    */
extern volatile uint32_t x86_pg_wb_bytes;
extern volatile uint32_t x86_pg_wb_opens;       /* fopen/fseek/fwrite/fclose  */
extern volatile uint32_t x86_pg_wb_us_worst;
extern volatile uint32_t x86_pg_wb_us_total;    /* TOTAL time in writeback    */
extern volatile uint32_t x86_pg_io_errors;
extern volatile uint32_t x86_pg_exhausted;      /* no victim could be found   */
extern volatile uint32_t x86_pg_unmapped;       /* faults outside our window  */
extern volatile uint32_t x86_pg_donated;        /* frames given to the arena  */
extern volatile uint32_t x86_pg_donate_bytes;   /* bytes the arena took       */
extern volatile uint32_t x86_pg_donate_refused; /* donations refused outright */
extern volatile uint32_t x86_pg_reclaim_busy;   /* reclaims the arena vetoed  */
extern volatile uint32_t x86_pg_resident_peak;  /* the MEASURED working set   */
extern volatile uint32_t x86_pg_reserve_eff;    /* the reserve in force now   */
extern volatile uint32_t x86_pg_shrink_signals; /* times frames were recalled */
extern volatile uint32_t x86_pg_grow_signals;   /* times lending was permitted */
extern volatile uint32_t x86_pg_policy_ratchet; /* what pressure has taught it */

#endif /* X86_PAGER_H */
