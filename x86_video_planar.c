/*
 * The planar VGA adapter -- EGA mode 0Dh and unchained 13h ("Mode Y").
 *
 * Every guest STORE that lands in the 64 KB aperture at 0xA0000 arrives here
 * instead of being written by the interpreter -- see the VGA_ST macro in
 * cpu_exec.S. The hook REPLACES the store; it is never a notification after
 * one. That is load-bearing: in an unchained write the byte the guest presents
 * is not the byte that reaches any plane, so letting the raw store happen first
 * would destroy a pixel written four stores earlier.
 *
 * ===========================================================================
 * WHAT THIS IS A PORT OF, AND THE TWO PLACES IT IMPROVES ON THE ORIGINAL
 * ===========================================================================
 *
 * The model, the arithmetic and every "do not re-propose this" note come from
 * references/8086tiny (dos_ega_cell, dos_ega_store, dos_vga_set_unchained) and
 * its design records, which are the thing to read before changing anything
 * here:
 *
 *   references/8086tiny/docs/video/11-ega-planar.md   the 0Dh write pipeline
 *   references/8086tiny/docs/video/15-mode-y.md       the unchained model
 *   docs/08_vga_mode13h.md                            our side of it
 *
 * TWO THINGS ARE BETTER HERE THAN IN THE REFERENCE, and both fall out of the
 * hook's shape rather than from cleverness:
 *
 *  1. THE LATCHES ARE EXACT FOR PLAIN READS, not only for read-modify-writes.
 *     The reference is a C interpreter whose read path never surfaced a plain
 *     load, so it could only latch where it could see a read: inside a
 *     read-modify-write store, from the DESTINATION. The APERTURE'S FOLD TRAP
 *     records the last guest read in `cpu->vga_latch` -- every load in the
 *     interpreter, at no cost to a title that never arms a planar mode, since
 *     the trap is the ADDS/BMI every access already runs (X86_FOLD_VGA_K in
 *     x86_cpu.h). It replaced VGA_LATCH, which was five instructions at 47
 *     load sites, armed or not. So `mov al,[si] / mov [di],al` -- the write
 *     mode 1 block copy that is most of the reason to unchain at all -- latches
 *     from SI, which is where the hardware latches from.
 *
 *  2. THERE IS NO SAVE/RESTORE DANCE. 15-mode-y.md section 5 ("the one thing
 *     with no analogue in 0Dh") describes saving the aperture bytes a raw store
 *     is about to destroy and putting them back, because in Mode Y the shadow
 *     address IS plane storage. We never write the raw byte at all -- the hook
 *     replaces the store -- so `dos_ega_wr_save[]` has no analogue here and is
 *     not needed. Do not reintroduce it.
 *
 * ===========================================================================
 * THE ADDRESS MODEL
 * ===========================================================================
 *
 * Planes are INTERLEAVED: plane p at plane offset a lives at `cell[4*a + p]`.
 * That is the QEMU/TinyEMU convention, reached via tiny386's vga.c, and it
 * earns its place twice -- you allocate four times what is USED rather than
 * four times the window, and all four planes of one group are a single aligned
 * 32-bit load, which is what makes the blit cheap on a Cortex-M7.
 *
 * Mode 0Dh: window offset == plane offset. A guest read is served from the
 * bottom 16 KB of the aperture, held permanently equal to what a read must
 * return (the "read shadow"), so reads cost nothing on the hot path.
 *
 * Mode Y: pixel (x,y) is plane x&3 at plane offset y*80 + (x>>2), and
 *
 *     4 * (y*80 + (x>>2)) + (x&3)  ==  320*y + x
 *
 * so the interleaved store IS what a chained blit already wanted, and the
 * de-interleaving blit is the identity at the standard geometry. The four
 * squashed copies were never in the blit; they were the core storing plane
 * data through a chained write path.
 *
 * A MODE-Y PAGE IS STILL 64,000 BYTES. Unchaining does not shrink a page, it
 * makes three more of them addressable -- and that is the thing that does not
 * fit. Two real pages is 195,072 bytes; a full card is 262,144. We have the
 * 64 KB aperture and 32 KB of arena. So plane offsets are FOLDED into one
 * page's worth of storage and a page flip is a no-op; see the fold in
 * vga_cell() and the limits recorded in docs/08_vga_mode13h.md section 4.
 *
 * DO NOT "FIX" THE ALIASING WITHOUT BRINGING ~192 KB WITH YOU.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "cutils.h"
#include "x86_cpu.h"
#include "x86_video_planar.h"
#include "x86_vprof.h"

/* ---------------------------------------------------------------------------
 * NEGATIVE CONTROLS. Each one compiles a specific bug back in and each is
 * REQUIRED to fail tests/test_video.py's Mode Y arm -- a check that still
 * passes with the bug present is not a check. Defaults are all 0; the two that
 * live in the blit are in x86_video.c.
 */
#ifndef X86_MODEY_CORE_NEGCTL
#define X86_MODEY_CORE_NEGCTL 0     /* chain-4 inert: the pre-fix core        */
#endif
#ifndef X86_MODEY_PAGE_NEGCTL
#define X86_MODEY_PAGE_NEGCTL 0     /* fixed 16 KB mask, cache inside the     */
#endif                              /* alias -- the state before the stride   */
                                    /* work                                   */
/* The presentation policy's control is the reference's own switch,
   -DX86_MODEY_PRESENT_ON_FLIP=0, defined with the policy further down. The
   three that used to live here -- TIMER, QUIET and BACKOFF -- were controls for
   three successive LOCAL inventions and went with them. */

/* The VGA aperture's fold trap. 0 is its NEGATIVE CONTROL: the sentinel is
   never installed, so no aperture read traps and cpu->vga_latch is never
   recorded -- every latch-fed write mode then draws from stale latches and
   tests/test_video.py's Mode Y and 0Dh arms must fail. See fold_trap_set(). */
#ifndef X86_VGA_FOLD_TRAP
#define X86_VGA_FOLD_TRAP 1
#endif

/* Plane offsets alias into one 16 KB page: 4 * 0x4000 == 0x10000, the aperture
   exactly. A 320x200 page needs 16,000 of those 16,384, so the top 384 are
   never displayed and act as scratch. */
#define X86_MODEY_PLANE  X86_MODEY_PLANE_OFFSETS
#define X86_MODEY_MASK   (X86_MODEY_PLANE - 1u)

/*
 * THE PAGE STRIDE IS NOT 16 KB, AND ASSUMING IT WAS IS WHAT BROKE WOLF3D.
 *
 * `a & 0x3FFF` aliases pages only if the guest spaces them 16,384 plane offsets
 * apart. Measured on the reference port, WOLF3D does not: its display start
 * cycles 0x0000 -> 0x4100 -> 0x8200, so its page is 80 x 208 = 16,640 offsets
 * -- the id Galaxy engine's SCREENSIZE -- and 0x4100 & 0x3FFF == 0x0100. Under
 * a mask every page lands 256 offsets (3.2 scanlines) off its neighbour, which
 * is both the "first frame was perfect and then it overwrote itself" report
 * (a full-page latch copy becomes a copy of the page onto itself, shifted) and
 * the "stepped vertical slabs" one (consecutive frames fold differently).
 *
 * So the stride is LEARNED from the only place a guest ever states it: the
 * distance between two display starts it programs. Bounded on both sides,
 * because a Sequencer-walking probe and a smooth-scroll title write that
 * register too. A page must hold the 200 displayed rows (16,000 offsets), and
 * past 20,480 we could not store the displayed part anyway -- which is what
 * rejects WOLF3D's own 0x0000 <-> 0x8000 signon pair while accepting 0x4100.
 * The SMALLEST accepted distance wins, so a first wrong guess is corrected
 * rather than latched.
 */
#define X86_MODEY_PGMIN  16000u     /* 200 rows x 80 offsets                  */
#define X86_MODEY_PGMAX  20480u     /* 256 rows; past this we could not store */

/*
 * THE OFF-SCREEN CACHE BASE IS A CONSTANT, AND THAT IS DELIBERATE.
 *
 * 0xC300 is 3 x 16,640 -- the first offset past a three-page 80x208 layout,
 * the number the Galaxy engines call FREESTART, and exactly where WOLF3D's
 * off-screen writes start. It is NOT derived from the learned stride: that was
 * built in the reference and it MANGLES THE STATUS BAR, because the tiles are
 * cached before the first flip and read back after, so the base moves under
 * stored data. And it is not 0xC000 either -- page 2 of a 16,640 layout runs to
 * 0xC300, so a cache starting at 0xC000 eats that page's last four displayed
 * rows, which the reference's own test caught as 512 wrong pixels.
 */
#define X86_MODEY_CACHE  0xC300u

/*
 * THE STRIDE ASSUMED UNTIL THE GUEST FLIPS, AND WHY IT IS 16,640.
 *
 * Learning is TOO LATE FOR THE FURNITURE. WOLF3D's DrawPlayScreen paints the
 * border and the status bar into all three pages BEFORE PlayLoop flips for the
 * first time, so whatever stride is assumed at that moment is baked into stored
 * pixels that are never redrawn. Measured on the reference: at 16,384 the
 * status bar comes out as three copies stepped 3 rows and 64 pixels apart and
 * stays that way for the level; at 16,640 the frame is pixel-identical to a
 * 256 KB reference card. That is a measurement, not a principle -- a title
 * spacing its pages 16,384 apart is displaced until its first flip corrects the
 * value downward. -DX86_MODEY_PAGE0=<n> overrides it, and the POSITIVE control
 * in tests/test_video.py builds 16384 to prove the learning actually runs.
 */
#ifndef X86_MODEY_PAGE0
#define X86_MODEY_PAGE0 16640u
#endif

/* ---- state ---------------------------------------------------------------
 *
 * Small on purpose: this is .bss on a target with ~4.5 KB of headroom.
 */

/* Host base of the guest 0xA0000 aperture, or NULL. Owned here rather than in
   x86_video.c so the hook has no dependency on the renderer, but SET there --
   x86_video_set_aperture() is the single place the aperture is decided. */
uint8_t *x86_vga_planar_aperture;

volatile uint32_t x86_vga_planar_stores;
/*
 * Stores by WRITE CLASS. Two jobs, and it needs both.
 *
 * On a target it says which idiom a title's frame is actually made of, which
 * is the only honest way to argue that a fast path is worth its risk. At tier
 * 1 it is COVERAGE: tests/test_video.py requires all three to be non-zero, so
 * "Mode Y 64000/64000 exact" is a statement about the fast paths and not only
 * about the general one that happened to run. A fast path the suite never
 * enters is a fast path with no oracle, and this area has shipped those.
 */
volatile uint32_t x86_vga_planar_cls_plane;
volatile uint32_t x86_vga_planar_cls_plane1;
volatile uint32_t x86_vga_planar_cls_copy;
volatile uint32_t x86_vga_planar_cls_general;
volatile uint32_t x86_vga_planar_lost;
volatile uint32_t x86_vga_modey_flips;
volatile uint32_t x86_vga_modey_page = X86_MODEY_PAGE0;
volatile uint32_t x86_vga_modey_armed;
volatile uint32_t x86_vga_ega_armed;
volatile uint32_t x86_vga_ega_refused;
volatile uint32_t x86_vga_latch_loads;
/*
 * WHAT `x86_vga_planar_lost` IS ACTUALLY COUNTING. It conflated three unrelated
 * discards into one number, and 18,944 of them measured on hardware could not
 * be attributed to any of them. They are split now, because the reference's own
 * account of this region is a per-source measurement (15-mode-y.md, "Cost of
 * the discard: 2,688 of 59,616 cache latch loads, all in one cold one-off
 * region at 0xE100-0xECFF") and one lump sum cannot be compared with it.
 *
 *   _CACHE  a store past the end of the off-screen cache that was actually
 *           fitted (x86_vga_modey_cache_offs above 0xC300; 0xE300 with the old
 *           fixed 8,192). The reference measured this region as cold and
 *           one-off, and rebuilding with a 48 KB cache gave a byte-identical
 *           frame, so a LARGE number here that keeps CLIMBING during gameplay
 *           would contradict the reference and is the first thing to look at.
 *   _TAIL   a store in a page's rows 205+ (offsets 16,384..stride-1 of each of
 *           the three pages). 768 offsets per full pass over the card; never
 *           displayed by construction.
 *   _NOCACHE the arena refused the cache block, so the WHOLE 0xC300..0xE2FF
 *           region is being thrown away. This is a configuration failure, not a
 *           layout one, and it is silent without this counter.
 */
volatile uint32_t x86_vga_modey_lost_cache;
volatile uint32_t x86_vga_modey_lost_tail;
volatile uint32_t x86_vga_modey_lost_nocache;
/* Host-visible proof that the off-screen cache is CLAIMED and non-NULL: this is
   X86_MODEY_CACHE_OFFSETS when it is live and 0 when the arena refused it. */
volatile uint32_t x86_vga_modey_cache_offs;
/*
 * The HIGHEST window offset a store was thrown away at, and the lowest one
 * above the cache. "How much cache does this title need?" cannot be answered
 * by a discard COUNT -- 16,896 discards is the same number whether they are all
 * one offset past the end or spread over 8 KB -- and sizing the cache by
 * guessing is what left Wolf3D's pictures as noise through two attempts. These
 * two say exactly where to put the top of the cache: it must reach _hi + 1.
 */
/* How many of the three Mode Y display pages have storage of their OWN: 1 is
   the aliased fold (all three on top of each other), 3 is the real card. */
volatile uint32_t x86_vga_modey_pages = 1;
/* Cells resolved to page 1 or 2 that had to fall back onto page 0's storage.
   THE LOUD COUNTER: a run with x86_vga_modey_pages < 3 and this climbing is the
   guest drawing over the frame it is displaying, which is exactly the corrupted
   Wolf3D gameplay picture. Zero when the donation was funded. */
volatile uint32_t x86_vga_modey_alias_hits;
volatile uint32_t x86_vga_modey_lost_hi;
volatile uint32_t x86_vga_modey_lost_lo = 0xFFFFFFFFu;
volatile uint32_t x86_vga_latch_miss;

static uint8_t  *ega_planes;        /* 0Dh interleaved plane storage (arena) */
static uint32_t  ega_plane_size;    /* plane OFFSETS the card has fitted     */
static uint8_t  *modey_cache;       /* Mode Y off-screen store (arena)       */
static uint32_t  modey_cache_offs;  /* plane offsets it holds                */
/* Real storage for Mode Y display pages 1 and 2. Page 0 is always the aperture.
   NULL means that page is still ALIASED onto page 0 -- the old behaviour, and
   the defect this whole path exists to remove. */
static uint8_t  *modey_pgstore[2];

static uint32_t  modey_prev_start;
static int       modey_learned;

/* The Graphics Controller file, mirrored here because the write pipeline reads
   it on every store and an accessor call per byte is not free. x86_io.c remains
   the register file a GUEST reads back; this is fed from it and never the other
   way round, so there is one writer and no possibility of disagreement. */
static uint8_t   gc[16];
static uint8_t   map_mask = 0x0F;   /* Sequencer index 2                     */
static uint8_t   latch[4];

#define GC_SET_RESET     gc[0]
#define GC_EN_SET_RESET  gc[1]
#define GC_COLOR_CMP     gc[2]
#define GC_ROTATE        gc[3]
#define GC_READ_MAP      gc[4]
#define GC_MODE          gc[5]
#define GC_COLOR_DC      gc[7]
#define GC_BIT_MASK      gc[8]

/* ---------------------------------------------------------------------------
 * THE WRITE CLASS, DECIDED AT `OUT` TIME AND NOT PER STORE.
 *
 * The Sequencer and Graphics Controller only change on a port write, and a
 * guest changes them a few hundred times a frame against a few hundred
 * THOUSAND stores. The store path was re-reading five registers and branching
 * on them for every byte: measured under gwemu, WOLF3D's PC lands in
 * x86_vga_planar_store and its throughput halves the moment Mode Y arms
 * (11.6 M ips in the menus, 5.35 M in gameplay; on the device 3.0 M against
 * 1.4 M). So the whole register combination is collapsed into ONE index here,
 * recomputed only when something that feeds it is written.
 *
 * Two classes are worth naming, and they are the two idioms a planar title
 * actually spends its frame in:
 *
 *   PL_COPY   write mode 1, every plane enabled. The latched group goes back
 *             untouched -- `mov al,[si] / mov [di],al`, the page copy.
 *   PL_PLANE  write mode 0 with NO Set/Reset, Bit Mask 0xFF, no rotate and no
 *             ALU. Every plane's output is then just the CPU byte, so the
 *             store is a masked 32-bit merge of `v` into the enabled planes --
 *             and, crucially, it does not depend on the latches AT ALL, so it
 *             can skip latch_reload() and the vga_cell() lookup inside it.
 *             This is what Wolf3D's compiled scalers do, one plane at a time.
 *
 * Anything else is PL_GENERAL and goes through planar_byte() exactly as
 * before. That is the oracle: tests/test_video.py's Mode Y and 0Dh arms drive
 * all four write modes, set/reset, the ALU and partial bit masks through the
 * SAME 64,000 pixels, so a fast path that is wrong for a combination it should
 * not have claimed shows up as wrong pixels rather than as a silent win.
 */
enum { PL_GENERAL = 0, PL_COPY = 1, PL_PLANE = 2 };
static uint8_t  planar_class = PL_GENERAL;
static uint32_t planar_plane_mask32;    /* 0xFF per plane the Map Mask enables */
/*
 * THE PLANE INDEX WHEN THE MAP MASK SELECTS EXACTLY ONE, ELSE -1.
 *
 * The 32-bit masked merge below writes FOUR BYTES to produce, in the case that
 * dominates Wolf3D's frame, ONE byte of result. The plane layout is
 * pixel-major -- vga_cell() returns `base + 4*off`, so plane k of an offset is
 * a single byte at `p[k]` -- which means a single-plane write is an address
 * computation and one STRB, not a read-modify-write.
 *
 * The read half is what costs: a scaler writes a vertical column, so successive
 * stores are 80 plane offsets = 320 BYTES apart and every one of them is its
 * own cache line in AXI SRAM. The load MUST complete before the merged value
 * can be stored, so the core stalls on a line fill it did not need; a lone STRB
 * posts to the store buffer and the core walks away. That is the difference
 * between the two paths, and it is far larger than the ALU work they differ by.
 *
 * Derived at OUT time with the class, because the Map Mask is written a few
 * hundred times a frame against a few hundred thousand stores -- the same
 * argument that put planar_class here in the first place.
 */
static int8_t   planar_plane_single = -1;

static void planar_reclass(void)
{
    uint32_t m = 0;
    int i;

    for (i = 0; i < 4; i++)
        if (map_mask & (1u << i))
            m |= 0xFFu << (8 * i);
    planar_plane_mask32 = m;

    /* Exactly one plane: a power of two, and not zero. map_mask == 0 writes
       nothing and must NOT take the single-plane path -- it stays at -1, where
       the merge below is a no-op against a zero mask, which is correct. */
    planar_plane_single = -1;
    if (map_mask != 0u && (map_mask & (map_mask - 1u)) == 0u) {
        for (i = 0; i < 4; i++)
            if (map_mask & (1u << i))
                planar_plane_single = (int8_t)i;
    }

    if ((GC_MODE & 3u) == 1u && map_mask == 0x0Fu) {
        planar_class = PL_COPY;
        return;
    }
    if ((GC_MODE & 3u) == 0u && GC_EN_SET_RESET == 0u
        && GC_BIT_MASK == 0xFFu && (GC_ROTATE & 0x1Fu) == 0u) {
        /* Rotate AND the ALU select both live in GC_ROTATE: bits 2-0 are the
           rotate count and bits 4-3 the ALU. Testing only the rotate would
           claim an AND/OR/XOR write for a path that ignores the latch, which
           is a silent corruption of exactly the kind this area keeps
           producing. */
        planar_class = PL_PLANE;
        return;
    }
    planar_class = PL_GENERAL;
}

int x86_vga_is_unchained(void)   { return (int)x86_vga_modey_armed; }
int x86_vga_planar_active(void)  { return (int)(x86_vga_modey_armed | x86_vga_ega_armed); }

const uint8_t *x86_vga_ega_planes(void)      { return ega_planes; }
uint32_t       x86_vga_ega_plane_offsets(void) { return ega_plane_size; }
/* For the harness only: the fit arm scribbles the cache so a text buffer that
   came back holding cache bytes cannot pass by being incidentally zero. */
const uint8_t *x86_vga_modey_cache_ptr(void) { return modey_cache; }

void x86_vga_ega_set_planes(void *p, uint32_t plane_offsets)
{
    ega_planes = (uint8_t *)p;
    ega_plane_size = p ? plane_offsets : 0u;
}

void x86_vga_modey_set_cache(void *p, uint32_t offsets)
{
    modey_cache = (uint8_t *)p;
    modey_cache_offs = p ? offsets : 0u;
    x86_vga_modey_cache_offs = modey_cache_offs;
}

/*
 * REAL PAGE STORAGE FOR PAGES 1 AND 2 -- what the alias never was.
 *
 * With one page's worth of storage, all three of WOLF3D's display starts fold
 * onto the same bytes: while the panel shows page N the raycaster is drawing
 * page N+1 over it, and the owner sees each wall rendered twice -- once in
 * perspective and once unstretched -- with the doors split and out of order.
 * The prior port measured the same thing against a real 256 KB card and wrote
 * the conclusion down: "the frame boundary composites a finished frame with a
 * half-drawn one. That is not tearing, and it is not fixed by getting the fold
 * right." Four rounds of presentation policy since have confirmed it from the
 * other side. It is not a presentation bug. It needs MEMORY, and the memory
 * comes from the pager's idle frames by way of x86_pager_donate_to_arena().
 *
 * Either or both may be NULL. A page with no storage keeps the old aliased
 * fold, which is a picture rather than a black screen -- but it is counted, in
 * x86_vga_modey_alias_hits, because "the guest silently got the alias again" is
 * indistinguishable from this defect returning.
 */
void x86_vga_modey_set_pages(void *p1, void *p2)
{
    /* Page 2 without page 1 would give a layout whose middle page is the alias
       and whose last is real -- more surprising than the plain alias and no
       cheaper. Take them in order or not at all. */
    modey_pgstore[0] = (uint8_t *)p1;
    modey_pgstore[1] = p1 ? (uint8_t *)p2 : NULL;
    /* Arena memory carries whatever the last claim left in it, and these are
       displayed storage: a page that is never fully drawn would show it. */
    if (modey_pgstore[0]) memset(modey_pgstore[0], 0, 4u * X86_MODEY_PLANE);
    if (modey_pgstore[1]) memset(modey_pgstore[1], 0, 4u * X86_MODEY_PLANE);
    x86_vga_modey_pages = 1u + (modey_pgstore[0] != NULL)
                             + (modey_pgstore[1] != NULL);
    x86_vga_modey_alias_hits = 0;
}

/* ---------------------------------------------------------------------------
 * THE ONE PLACE THAT SAYS WHERE A PLANE BYTE LIVES.
 *
 * Aperture, off-screen cache, or nowhere. Every consumer -- the store, the
 * latch reload and the blit (through x86_vga_modey_row) -- goes through this,
 * so they cannot disagree about where a row is. The reference reached the same
 * shape only after deleting two rival helpers that had drifted apart.
 *
 * Returns a pointer to the FOUR interleaved plane bytes of `a`, or NULL when
 * this card has no memory there. NULL is not an error: a missing off-screen
 * tile must draw nothing, where an aliased one corrupts the picture.
 */
/*
 * THE PAGE REDUCTION, factored out so the store path and the BLIT share it.
 *
 * Which of the three display pages window offset `a` is in, and where in that
 * page. Returns 0 when `a` has no page storage at all -- the off-screen cache,
 * or a page's tail rows 205+. Page-relative against the LEARNED stride, not an
 * AND mask: WOLF3D's is 16,640 and a mask puts every page 256 offsets off its
 * neighbour (see the stride block above).
 */
static int modey_split(uint32_t a, uint32_t *pg, uint32_t *off)
{
    uint32_t p = x86_vga_modey_page, n = 0;

    if (a >= X86_MODEY_CACHE)
        return 0;
    if (a >= 2u * p)      { a -= 2u * p; n = 2; }
    else if (a >= p)      { a -= p;      n = 1; }
    if (a >= X86_MODEY_PLANE)
        return 0;                       /* page tail, rows 205+: never shown */
    *pg = n;
    *off = a;
    return 1;
}

/* Where page `pg` actually lives. Page 0 is the aperture; 1 and 2 are their own
   storage when it was funded and page 0 again when it was not. */
static uint8_t *modey_page_base(uint32_t pg)
{
    if (pg && modey_pgstore[pg - 1u])
        return modey_pgstore[pg - 1u];
    return x86_vga_planar_aperture;
}

/* ---------------------------------------------------------------------------
 * ITCM PLACEMENT FOR THE HOT PLANAR PATH.
 *
 * MEASURED on hardware, Wolf3D in Mode Y: six of twenty-four PC samples landed
 * in x86_vga_planar_store / vga_cell / x86_vga_planar_active, all executing
 * from 0x2405xxxx -- AXI. CLAUDE.md says hot loops MUST be pinned to ITCM; the
 * interpreter is, and this path never was.
 *
 * The two move TOGETHER on purpose. ITCM is at 0x00000000 and AXI at
 * 0x24000000, far outside BL range, so every call across the boundary costs a
 * linker veneer. Moving the pair makes the interpreter's call INTO the store a
 * near call (the interpreter is already in ITCM) and keeps store -> vga_cell
 * near as well. Splitting them would trade one veneer for another.
 *
 * This is placement only: the PLANES stay in AXI, so it removes instruction
 * fetch cost, not data cost. If a measurement ever shows the remaining cost is
 * the plane accesses, this change is not the lever and should not be blamed.
 */
#define PLANAR_HOT __attribute__((section(".itcram_hot.planar")))

PLANAR_HOT static uint8_t *vga_cell(uint32_t a)
{
    uint32_t pg, off;

    if (!x86_vga_modey_armed) {
        if (a < ega_plane_size)
            return ega_planes + 4u * a;
        return 0;                       /* past the fitted card: discarded */
    }

#if X86_MODEY_PAGE_NEGCTL
    return x86_vga_planar_aperture + 4u * (a & X86_MODEY_MASK);
#else
    if (a >= X86_MODEY_CACHE) {
        a -= X86_MODEY_CACHE;
        if (a >= modey_cache_offs)
            return 0;                   /* no storage: draws nothing */
        return modey_cache + 4u * a;
    }
    /* Three pages, because that is what a Galaxy-engine layout uses; a title
       that spaces them differently loses its cache to the discard path above
       rather than corrupting a page, which is the direction to fail in. */
    if (!modey_split(a, &pg, &off))
        return 0;
    if (pg && !modey_pgstore[pg - 1u])
        x86_vga_modey_alias_hits++;     /* LOUD: drawing over the shown page */
    return modey_page_base(pg) + 4u * off;
#endif
}

uint32_t x86_vga_modey_row(uint32_t a)
{
    const uint8_t *base = x86_vga_planar_aperture;
    const uint8_t *c;

    if (!base)
        return ~0u;
    c = vga_cell(a);
    if (!c || c < base || c >= base + X86_VGA_APERTURE_BYTES)
        return ~0u;
    return (uint32_t)(c - base) / 4u;
}

/*
 * The blit's half of the address model, in POINTER form.
 *
 * x86_vga_modey_row() cannot serve a real page-1 or page-2 row: it answers in
 * offsets from the aperture, and those pages are not in the aperture any more.
 * This is the same reduction -- modey_split(), shared with vga_cell() -- so the
 * blit and the store path still cannot disagree about where a row is, which is
 * the whole reason the reference deleted its two rival helpers.
 *
 * Returns 0 when the row has no page storage (off-screen cache, or a page tail
 * that is never displayed), which the blit paints BLACK so a mis-learned layout
 * is visible rather than merely plausible.
 */
int x86_vga_modey_row_at(uint32_t a, const uint8_t **base, uint32_t *off)
{
    uint32_t pg, o;

    if (!x86_vga_planar_aperture || !x86_vga_modey_armed)
        return 0;
#if X86_MODEY_PAGE_NEGCTL
    /* The fixed-mask negative control has to reach the BLIT too, or the arm
       that requires it to fail is testing only the store path. */
    (void)pg; (void)o;
    *base = x86_vga_planar_aperture;
    *off  = a & X86_MODEY_MASK;
    return 1;
#else
    if (!modey_split(a, &pg, &o))
        return 0;
    *base = modey_page_base(pg);
    *off  = o;
    return 1;
#endif
}

/* ---------------------------------------------------------------------------
 * READS (mode 0Dh only).
 *
 * What a guest read at plane offset `a` must return, for the CURRENT read mode.
 * This is the ONLY definition of what the shadow holds, so the shadow is
 * correct under read mode 1 (colour compare) rather than silently handing back
 * plane bytes.
 *
 * MODE Y HAS NO SHADOW and cannot have one: the planes ARE the aperture. A
 * guest data read of A000:a there returns aperture[a], i.e. plane a&3 at plane
 * offset a>>2 -- the wrong plane, by design and for want of 64 KB. Latch loads
 * stay exact, and latch copies are the idiom that matters; a title that reads
 * pixels back for collision detection would not be served.
 */
static uint8_t ega_read_byte(uint32_t a)
{
    static const uint8_t nocell[4] = { 0, 0, 0, 0 };
    const uint8_t *p = vga_cell(a);

    if (!p)
        p = nocell;
    if (GC_MODE & 8u) {
        /* Read mode 1: each returned bit is 1 where every plane selected by
           Colour Don't Care matches the corresponding bit of Colour Compare. */
        uint8_t r = 0xFF;
        for (int i = 0; i < 4; i++)
            if (GC_COLOR_DC & (1u << i))
                r &= (GC_COLOR_CMP & (1u << i)) ? p[i] : (uint8_t)~p[i];
        return r;
    }
    return p[GC_READ_MAP & 3u];
}

/*
 * Rebuild the whole read shadow -- the bottom 16 KB of the aperture, which is
 * what a guest read of A000:xxxx lands on.
 *
 * THE SHADOW IS AT THE BOTTOM OF THE APERTURE AND THAT IS LOAD-BEARING. A guest
 * access to the window has to reach it with NO change to the address fold, on
 * the single hottest path in the emulator. Putting the planes there instead
 * would make every guest memory access consult a global. Do not reorder this.
 *
 * Called only when something that changes what a read returns changes -- Read
 * Map Select, read mode, Colour Compare, Colour Don't Care -- and guarded on
 * the value ACTUALLY changing by its caller. The guard matters: the rebuild is
 * ega_plane_size byte computations, and in read mode 1 each is a four-plane
 * loop. On the reference this went unnoticed at 8 KB and then wedged a device
 * run at 64 KB.
 */
#define EGA_SHADOW_OFFSETS  0x4000u     /* 16 KB, the CPU-visible window */

/* ---------------------------------------------------------------------------
 * X86_EGA_SHPROF=1 -- the EGA read-shadow rebuild instrument. OFF by default.
 *
 * WHY NOT x86_vprof.h, WHICH ALREADY HAS DWT BRACKETS AND A WINDOW CONTROL.
 * Because X86_VPROF=1 DOES NOT FIT any more, and it fails SILENTLY in exactly
 * the way STATUS.md warns about: the flag adds ~1,080 B of code+bss to
 * RAM_EMU, guest_ram then comes back NULL, core_ready stays 0,
 * guest_insns_total stays 0 and the frame loop runs on painting nothing. That
 * was reproduced at tier 2 on STOCK master with no local changes (KEEN4:
 * core_ready=0 at X86_VPROF=1, core_ready=1 and 157 M insns at X86_VPROF=0),
 * so it is not this branch's doing -- but it does mean the whole instrument
 * cannot be switched on to measure one function.
 *
 * So this is the same two-CYCCNT-reads bracket in five words of bss and one
 * call site. Same wrapping rule as x86_vprof.h: CYCCNT is 32-bit and wraps
 * every ~15 s at 280 MHz, so a DELTA is taken (exact across a wrap by modular
 * arithmetic) and accumulated 64-bit.
 *
 *   _rebuilds   rebuilds actually performed
 *   _gc_writes  writes to a read-affecting GC register (2/4/5/7) that CHANGED
 *               it, i.e. calls reaching the rebuild guard. gc_writes minus
 *               rebuilds is what a tighter guard would save, so one instrument
 *               grades both the bug and any fix
 *   _cyc/_hi    64-bit total cycles inside the rebuild
 *   _worst      worst single rebuild -- a stall is a TAIL property and a mean
 *               cannot contain one
 */
#ifndef X86_EGA_SHPROF
#define X86_EGA_SHPROF 0
#endif

#if X86_EGA_SHPROF
volatile uint32_t x86_ega_shadow_rebuilds;
volatile uint32_t x86_ega_shadow_gc_writes;
volatile uint32_t x86_ega_shadow_cyc;
volatile uint32_t x86_ega_shadow_hi;
volatile uint32_t x86_ega_shadow_worst;

#define SHPROF_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define SHPROF_BEGIN()  uint32_t shprof_t0 = SHPROF_CYCCNT
#define SHPROF_END()                                                    \
    do {                                                                \
        uint32_t d = SHPROF_CYCCNT - shprof_t0;                         \
        uint32_t lo = x86_ega_shadow_cyc + d;                           \
        if (lo < x86_ega_shadow_cyc)                                    \
            x86_ega_shadow_hi++;                                        \
        x86_ega_shadow_cyc = lo;                                        \
        x86_ega_shadow_rebuilds++;                                      \
        if (d > x86_ega_shadow_worst)                                   \
            x86_ega_shadow_worst = d;                                   \
    } while (0)
#define SHPROF_GC_WRITE()  (x86_ega_shadow_gc_writes++)
#else
#define SHPROF_BEGIN()     ((void)0)
#define SHPROF_END()       ((void)0)
#define SHPROF_GC_WRITE()  ((void)0)
#endif


/*
 * WHAT A READ RETURNS, reduced to one value -- the rebuild's real precondition.
 *
 * The four GC registers that can affect a read do not all affect it AT THE SAME
 * TIME, and "the register changed" was standing in for "a read changed" at a
 * cost of 16,384 byte computations a time:
 *
 *   read mode 0 (GC5 bit 3 clear)  a read is p[GC_READ_MAP & 3]. Colour Compare
 *                                  and Colour Don't Care are not consulted, so
 *                                  writing either changes NOTHING.
 *   read mode 1                    a read is the colour compare. GC_READ_MAP is
 *                                  not consulted, so writing it changes nothing.
 *   either mode                    only bit 3 of GC5 is read state. Bits 0-1 are
 *                                  the WRITE mode, and a title that alternates
 *                                  write mode 0 and 1 -- which is the latch-copy
 *                                  idiom, and is what KEEN4's scroll loop does --
 *                                  was rebuilding the whole shadow on every one.
 *
 * Colour Compare is folded through Don't Care because a compare bit whose Don't
 * Care bit is clear is never consulted either.
 *
 * MEASURED, silicon, KEEN4: one rebuild is 846,000 cycles = 3.0 ms at 280 MHz,
 * and the wedging slice spent 76 of them -- 230 ms, half of the ~472 ms WWDG
 * window -- inside this function. See docs/13.
 */
/* Graded by scripts/run_planar_controls.py, which compiles each bug back IN
   and requires the 0Dh read arm to notice:
     1  a CONSTANT signature -- the shadow is built at the mode set and never
        again, i.e. the guard is unconditionally too coarse
     2  the read-MODE bit is dropped, which is the plausible partial version:
        read mode 1 keeps answering with the read map's plane byte */
#ifndef X86_EGA_SIG_NEGCTL
#define X86_EGA_SIG_NEGCTL 0
#endif

static uint32_t ega_read_sig(void)
{
#if X86_EGA_SIG_NEGCTL == 1
    return 0u;
#elif X86_EGA_SIG_NEGCTL == 2
    return GC_READ_MAP & 3u;
#else
    if (GC_MODE & 8u)
        return 0x100u | ((uint32_t)GC_COLOR_DC << 4) | (GC_COLOR_CMP & GC_COLOR_DC);
    return GC_READ_MAP & 3u;
#endif
}

/* The signature the shadow currently HOLDS. Set wherever the shadow is built. */
static uint32_t ega_shadow_sig;

static void ega_shadow_rebuild(void)
{
    uint8_t *sh = x86_vga_planar_aperture;
    uint32_t a;

    /* Mode Y has no shadow: "rebuilding" it would overwrite the picture with
       itself decoded as something else. */
    if (!sh || x86_vga_modey_armed || !x86_vga_ega_armed)
        return;
    SHPROF_BEGIN();
    for (a = 0; a < ega_plane_size && a < EGA_SHADOW_OFFSETS; a++)
        sh[a] = ega_read_byte(a);
    /* Offsets past the modelled planes read as zero rather than as stale bytes
       from whatever mode ran before. */
    for (; a < EGA_SHADOW_OFFSETS; a++)
        sh[a] = 0;
    SHPROF_END();
}

/* ---------------------------------------------------------------------------
 * THE LATCHES.
 *
 * Reloaded from the offset of the last guest read that landed in the aperture,
 * which cpu->vga_latch carries exactly (see the header comment). The reference
 * could only do this on a read-modify-write; here it is right for a plain
 * `mov al,[si]` too, which is the whole of the write-mode-1 block copy.
 */
static void latch_reload(void)
{
    const X86CPUState *cpu = cpu_state_ptr;
    const uint8_t *p;

    if (!cpu)
        return;
    x86_vga_latch_loads++;
    /* cpu->vga_latch is the HOST POINTER the trap recovered; the window offset
       is its distance from the aperture base. In 0Dh that offset is the plane
       offset; in Mode Y the window offset and the plane offset are the same
       number too, because the guest addresses planes directly there.
       A pointer from before the aperture existed, or from another mode, lands
       outside the window and is a latch MISS rather than a wild index. */
    {
        uint32_t off = cpu->vga_latch - (uint32_t)(uintptr_t)x86_vga_planar_aperture;
        if (!x86_vga_planar_aperture || off >= X86_VGA_APERTURE_BYTES) {
            x86_vga_latch_miss++;
            if (x86_vga_modey_armed)
                latch[0] = latch[1] = latch[2] = latch[3] = 0;
            return;
        }
        p = vga_cell(off);
    }
    if (p) {
        latch[0] = p[0]; latch[1] = p[1];
        latch[2] = p[2]; latch[3] = p[3];
        return;
    }
    x86_vga_latch_miss++;
    if (x86_vga_modey_armed) {
        /* A latch load from an offset with no storage is a MISS on the
           off-screen cache, and the copy that follows must draw NOTHING rather
           than replicate whatever tile was copied last. 0Dh keeps the latches
           unchanged instead, deliberately -- that is the reference's behaviour
           and its tests pin it. */
        latch[0] = latch[1] = latch[2] = latch[3] = 0;
    }
}

/* Data Rotate register bits 4-3: how the source is combined with the latch. */
static inline uint8_t ega_alu(uint8_t src, uint8_t lat)
{
    switch ((GC_ROTATE >> 3) & 3u) {
    case 1:  return (uint8_t)(src & lat);
    case 2:  return (uint8_t)(src | lat);
    case 3:  return (uint8_t)(src ^ lat);
    default: return src;
    }
}

/* A store this card has nowhere to put. Counted by SOURCE -- see the block on
   x86_vga_modey_lost_cache -- because one lump sum cannot be reconciled with
   the reference's per-region measurement. */
static void modey_lost(uint32_t a)
{
    x86_vga_planar_lost++;
    if (!x86_vga_modey_armed)
        return;                         /* 0Dh: past the fitted card */
    if (a >= X86_MODEY_CACHE) {
        if (!modey_cache_offs)
            x86_vga_modey_lost_nocache++;
        else
            x86_vga_modey_lost_cache++;
        if (a > x86_vga_modey_lost_hi)
            x86_vga_modey_lost_hi = a;
        if (a < x86_vga_modey_lost_lo)
            x86_vga_modey_lost_lo = a;
    } else {
        x86_vga_modey_lost_tail++;
    }
}

/* Apply one guest byte write at window offset `a`. The latches have already
   been reloaded for this instruction. */
static void planar_byte(uint32_t a, uint8_t v)
{
    uint8_t *p = vga_cell(a);
    uint8_t out[4], rot, bitmask;
    int i;

    if (!p) {
        modey_lost(a);
        return;                         /* card has no memory there */
    }

    switch (GC_MODE & 3u) {
    case 1:
        /* Write mode 1: the latches go back untouched. This is the fast block
           copy every planar title leans on and the reason the latch
           bookkeeping above has to be right. */
        out[0] = latch[0]; out[1] = latch[1];
        out[2] = latch[2]; out[3] = latch[3];
        break;

    case 2:
        /* Write mode 2: the low nibble of the CPU byte is a colour; bit i of it
           fills plane i, under the Bit Mask. */
        bitmask = GC_BIT_MASK;
        for (i = 0; i < 4; i++) {
            uint8_t src = (v & (1u << i)) ? 0xFFu : 0x00u;
            src = ega_alu(src, latch[i]);
            out[i] = (uint8_t)((src & bitmask) | (latch[i] & (uint8_t)~bitmask));
        }
        break;

    case 3:
        /* Write mode 3: Set/Reset supplies the colour and the rotated CPU byte
           ANDs into the Bit Mask. */
        rot = GC_ROTATE & 7u;
        v = (uint8_t)((v >> rot) | (v << (8 - rot)));
        bitmask = (uint8_t)(GC_BIT_MASK & v);
        for (i = 0; i < 4; i++) {
            uint8_t src = (GC_SET_RESET & (1u << i)) ? 0xFFu : 0x00u;
            out[i] = (uint8_t)((src & bitmask) | (latch[i] & (uint8_t)~bitmask));
        }
        break;

    default:
        /* Write mode 0: rotate, optionally substitute Set/Reset per plane,
           combine with the latch, then merge under the Bit Mask. */
        rot = GC_ROTATE & 7u;
        v = (uint8_t)((v >> rot) | (v << (8 - rot)));
        bitmask = GC_BIT_MASK;
        for (i = 0; i < 4; i++) {
            uint8_t src = (GC_EN_SET_RESET & (1u << i))
                        ? ((GC_SET_RESET & (1u << i)) ? 0xFFu : 0x00u)
                        : v;
            src = ega_alu(src, latch[i]);
            out[i] = (uint8_t)((src & bitmask) | (latch[i] & (uint8_t)~bitmask));
        }
        break;
    }

    /* Map Mask decides which planes the write actually reaches. */
    for (i = 0; i < 4; i++)
        if (map_mask & (1u << i))
            p[i] = out[i];

    /* Keep the read shadow true. This is what buys the free read path.
       NOT IN MODE Y: there the shadow address IS plane storage -- byte `a` of
       the aperture is plane a&3 at plane offset a>>2 -- so this would scribble
       a decoded byte over a pixel four stores younger than itself. */
    if (!x86_vga_modey_armed && a < EGA_SHADOW_OFFSETS)
        x86_vga_planar_aperture[a] = ega_read_byte(a);
}

/* The measured body. x86_vga_planar_store() below is the bracket; with
   X86_VPROF=0 it is a plain tail call the compiler folds away entirely. */
PLANAR_HOT static void planar_store_body(uint32_t off, uint32_t val, uint32_t size)
{
    uint8_t *ap = x86_vga_planar_aperture;
    uint32_t i;

    x86_vga_planar_stores++;
    if (!ap)
        return;                         /* armed with no storage: drop it */
    off &= (X86_VGA_APERTURE_BYTES - 1u);

    if (!x86_vga_planar_active()) {
        /* Belt and braces. Nothing arms cpu->vga_base outside a planar mode, so
           a chained 13h store never reaches here at all and mode 13h keeps its
           original zero-cost store path -- but if it ever did, a plain store is
           what it must do. */
        for (i = 0; i < size; i++)
            ap[(off + i) & (X86_VGA_APERTURE_BYTES - 1u)] = (uint8_t)(val >> (8u * i));
        return;
    }

    /*
     * FAST PATH 1: the CPU byte straight into the enabled planes.
     *
     * Write mode 0, no Set/Reset, Bit Mask 0xFF, no rotate, no ALU -- decided
     * at OUT time, so this is one load of a byte rather than five register
     * tests. Every plane's output is then exactly `v`, which means THE LATCHES
     * DO NOT PARTICIPATE: latch_reload() is skipped entirely, and with it the
     * vga_cell() lookup and four loads it does. That is the single biggest
     * item in the old per-store cost, and skipping it is safe because the
     * latches are reloaded from cpu->vga_latch by whichever later store
     * actually needs them, not carried between stores.
     *
     * Wolf3D's compiled scalers write one plane at a time and this is their
     * whole inner loop.
     */
    if (planar_class == PL_PLANE) {
        const uint32_t m = planar_plane_mask32;
        const uint32_t mm = map_mask;
        const int      one = planar_plane_single;
        /* A word or dword store is still ONE ADAPTER OPERATION PER BYTE, each
           with its own cell -- folding them is how a `stosw` comes out half
           right. But it is the same operation each time, so the loop stays in
           the fast path rather than falling back to the general pipeline:
           measured on WOLF3D in gameplay, multi-byte stores were 20% of all
           planar stores and every one of them was paying the full chain. */
        for (i = 0; i < size; i++) {
            uint32_t a = (off + i) & (X86_VGA_APERTURE_BYTES - 1u);
            uint8_t *p = vga_cell(a);
            uint32_t q;
            x86_vga_planar_cls_plane++;
            if (!p) {
                modey_lost(a);
                continue;
            }
            /* One masked 32-bit merge over the four interleaved plane bytes
               rather than a four-way test of the Map Mask. `p` is 4-byte
               aligned by construction (every cell is `base + 4*a`), but
               memcpy says so without relying on it. */
            /* VPROF: the CELL bracket -- the read-modify-write of the four
               interleaved plane bytes and NOTHING else. This is the one split
               that decides what the store path actually costs: everything
               outside it is code (the call, vga_cell's page reduction, the
               class test) and everything inside it is a single 4-byte access
               to plane storage. A scaler writes a vertical column, so
               consecutive stores are 80 plane offsets = 320 BYTES apart and
               each one is its own cache line. If the cost is here, no amount
               of moving code to ITCM can reach it -- and two such moves have
               already been measured as no help. */
            X86_VPROF_BEGIN(cl);
            /* NO READ, FOR ANY MASK. The merge this replaces preserved exactly
               the bytes the Map Mask does not enable -- and writing only the
               ENABLED bytes preserves them too, without ever loading the cell.
               Every plane the mask selects gets the same byte `v`, so there is
               nothing to compute from the old contents. */
            if (m == 0xFFFFFFFFu) {
                q = ((val >> (8u * i)) & 0xFFu) * 0x01010101u;
                memcpy(p, &q, 4);       /* all four: one aligned STR */
                x86_vga_planar_cls_plane1++;
            } else {
                const uint8_t v8 = (uint8_t)(val >> (8u * i));
                if (mm & 1u) p[0] = v8;
                if (mm & 2u) p[1] = v8;
                if (mm & 4u) p[2] = v8;
                if (mm & 8u) p[3] = v8;
                if (one >= 0)
                    x86_vga_planar_cls_plane1++;
            }
            X86_VPROF_END(cl, cell);
            if (!x86_vga_modey_armed && a < EGA_SHADOW_OFFSETS)
                ap[a] = ega_read_byte(a);
        }
        return;
    }

    latch_reload();

    /* FAST PATH 2: write mode 1 with every plane enabled, one byte. That is the
       page-copy idiom -- `mov al,[si] / mov [di],al` -- and it is what a Mode Y
       title spends its frame doing, so it is one 32-bit copy of the latched
       group rather than twenty instructions of register file. */
    if (planar_class == PL_COPY) {
        /* The latches are reloaded ONCE for the instruction, above, exactly as
           the general path does -- a multi-byte store latches from one source
           address, not from each destination byte in turn. */
        for (i = 0; i < size; i++) {
            uint32_t a = (off + i) & (X86_VGA_APERTURE_BYTES - 1u);
            uint8_t *p = vga_cell(a);
            x86_vga_planar_cls_copy++;
            if (!p) {
                modey_lost(a);
                continue;
            }
            p[0] = latch[0]; p[1] = latch[1];
            p[2] = latch[2]; p[3] = latch[3];
            if (!x86_vga_modey_armed && a < EGA_SHADOW_OFFSETS)
                ap[a] = ega_read_byte(a);
        }
        return;
    }

    /* A word or dword store covers CONSECUTIVE window offsets, and each of them
       is a separate adapter operation with its own cell, its own Map Mask pass
       and its own Bit Mask merge. Folding them into one is how a `stosw` comes
       out half right. */
    x86_vga_planar_cls_general++;
    for (i = 0; i < size; i++)
        planar_byte((off + i) & (X86_VGA_APERTURE_BYTES - 1u),
                    (uint8_t)(val >> (8u * i)));
}

/*
 * The interpreter's entry point. This is the ONE bracket in the project that
 * fires per guest store rather than per frame or per service call -- ~180,000
 * times a second in Mode Y gameplay -- so its cost is not noise and the header
 * says exactly how it is reported (raw, corrected by x86_vprof_ovh, and graded
 * against guest_ips_achieved on a clean build).
 *
 * With X86_VPROF=0 this is a bare tail call into a static function that has
 * exactly one caller, which the compiler inlines away: the shipping path keeps
 * the code it had.
 */
PLANAR_HOT void x86_vga_planar_store(uint32_t off, uint32_t val, uint32_t size)
{
    X86_VPROF_BEGIN(st);
    planar_store_body(off, val, size);
    X86_VPROF_END(st, store);
}

/* ---------------------------------------------------------------------------
 * ARMING
 */

/*
 * Arming, in two halves.
 *
 * THE STORE HALF is cpu->vga_base, tested per store site by VGA_ST: 0 disarms
 * it, and 0 is safe because host pointers are 0x24xxxxxx. One store, no
 * special case anywhere else.
 *
 * THE READ HALF IS THE FOLD TABLE. The sixteen entries covering 0xA0000 are
 * re-pointed at the sentinel X86_FOLD_VGA_K, so an aperture access resolves to
 * `0x80000000 | host` and trips the ADDS/BMI every access already runs; the
 * trap stub recovers the pointer and records the read latch. A title that
 * never enters a planar mode therefore pays NOTHING for the aperture -- the
 * old VGA_LATCH check was five instructions at every guest load in the
 * interpreter, armed or not.
 *
 * If the aperture is not where the sentinel encoding needs it (AXI or DTCM,
 * 0x2xxxxxxx), the sentinel is REFUSED and counted rather than installed: a
 * colliding tag would be read as a NOR address by x86_nor_slow_r. The store
 * half still arms, so a refusal costs the read latch on unhooked read sites,
 * not correctness of the picture.
 *
 * x86_seam.c's x86_pager_slack() already answers NULL for the aperture (it is
 * a fixed mapping, not a pager frame), so a straddling access is refused and
 * counted exactly as it was before.
 */
volatile uint32_t x86_vga_fold_trap_refused;
volatile uint32_t x86_vga_fold_trap_armed;

static void fold_trap_set(int on)
{
    const uint32_t first = 0xA0000u >> FOLD_SHIFT;
    const uint32_t n = X86_VGA_APERTURE_BYTES >> FOLD_SHIFT;
    uint8_t *ap = x86_vga_planar_aperture;

    if (!guest_page_table_ptr || !ap)
        return;
#if X86_VGA_FOLD_TRAP
    if (on && !X86_FOLD_VGA_OK(ap)) {
        x86_vga_fold_trap_refused++;
        on = 0;
    }
#else
    /* The NEGATIVE CONTROL for the whole mechanism: never install the
       sentinel, so no read ever traps and the latch is never recorded.
       tests/test_video.py's latch arms must FAIL under it. */
    on = 0;
#endif
    for (uint32_t i = 0; i < n; i++)
        guest_page_table_ptr[first + i] =
            on ? X86_FOLD_VGA_K(ap + (i << FOLD_SHIFT), first + i)
               : FOLD_K(ap + (i << FOLD_SHIFT), first + i);
    x86_vga_fold_trap_armed = (uint32_t)on;
}

/* Re-install the sentinel after something has rewritten the aperture's fold
   entries. x86_map_fixed() calls this: a mode set or an arena re-claim maps
   0xA0000 afresh, and without this the sentinel is silently dropped while the
   store hook stays armed -- reads then stop trapping, cpu->vga_latch freezes
   and every latch-fed write mode draws from a stale latch. That is not a
   theoretical ordering worry: it is what took Wolfenstein 3-D's Mode Y screen
   out on the first attempt at this. */
void x86_vga_planar_fold_refresh(void)
{
    if (x86_vga_fold_trap_armed)
        fold_trap_set(1);
}

static void hook_set(int on)
{
    if (cpu_state_ptr)
        cpu_state_ptr->vga_base =
            (on && x86_vga_planar_aperture)
                ? (uint32_t)(uintptr_t)x86_vga_planar_aperture : 0u;
    fold_trap_set(on && x86_vga_planar_aperture != NULL);
}

/*
 * Unchained 13h. Driven from Sequencer index 4 bit 3 by x86_io.c.
 *
 * Unlike 0Dh this is NOT keyed off the mode set -- it is the real register the
 * real hardware uses, because unchaining is something software does to itself
 * long after the BIOS mode set is over, without asking anyone. That is also why
 * the CALLER gates it on BDA 0040:0049 already being 0x13: a blind I/O probe
 * walking the Sequencer would otherwise reinterpret whatever mode is running as
 * planar.
 */
void x86_vga_modey_arm(int on)
{
#if X86_MODEY_CORE_NEGCTL
    (void)on;
    return;                             /* chain-4 inert: the pre-Mode-Y core */
#else
    if (!on) {
        if (!x86_vga_modey_armed)
            return;
        x86_vga_modey_armed = 0;
        hook_set(x86_vga_ega_armed);
        return;
    }
    if (x86_vga_modey_armed)
        return;                         /* idempotent */

    /* Mode 13h's mode set leaves the Graphics Controller alone -- it has no
       reason to touch it -- so a game that unchains and draws immediately would
       otherwise write through a ZERO Bit Mask and paint nothing at all. Put the
       register file in the state a planar mode set guarantees. */
    memset(gc, 0, sizeof gc);
    GC_BIT_MASK = 0xFF;
    GC_COLOR_DC = 0x0F;
    map_mask = 0x0F;
    latch[0] = latch[1] = latch[2] = latch[3] = 0;

    /* The planes are NOT cleared. They ARE the aperture, and the mode 13h set
       that just ran already cleared it; clearing again is 64 KB of work to
       reach the state we are in. It also lets a title that sets its screen up
       chained and then unchains keep what it drew. */

    /* The stride is re-learned per arming: it is a property of the software
       that unchained, not of the adapter. */
    x86_vga_modey_page = X86_MODEY_PAGE0;
    modey_prev_start = x86_vga_crtc_start();
    modey_learned = 0;

    /* The off-screen cache is separate storage and, unlike the aperture,
       nothing has just cleared it. Its contents as tiles would be noise. */
    if (modey_cache)
        memset(modey_cache, 0, 4u * modey_cache_offs);

    /* The presentation policy is per-arming state too: a stale quiet run from
       whatever ran before must not authorise a paint of a page this mode has
       not drawn yet. */
    x86_video_modey_policy_reset();

    x86_vga_modey_armed = 1;
    hook_set(1);
#endif
}

/*
 * EGA 0Dh. Keyed off the MODE SET, not off a write to Miscellaneous Output.
 *
 * The reference armed 0Dh by having its BIOS write port 0x3C2, and its own
 * design record calls that an abuse of a real register -- with the known limit
 * that a game programming the adapter directly never arms planar, and the
 * measured accident that FreeDOS walking 0x3C0-0x3CF with 0xFF armed it and
 * wiped VRAM. We have a mode table; the mode set is the honest edge, and it is
 * still before the first store.
 *
 * Returns 0 when there is no plane storage, so the mode can be REFUSED rather
 * than adopted-and-not-rendered.
 */
int x86_vga_planar_arm(int on)
{
    if (!on) {
        if (!x86_vga_ega_armed)
            return 1;
        x86_vga_ega_armed = 0;
        hook_set(x86_vga_modey_armed);
        return 1;
    }
    if (x86_vga_ega_armed)
        return 1;                       /* idempotent */
    if (!ega_planes || !ega_plane_size || !x86_vga_planar_aperture) {
        x86_vga_ega_refused++;
        return 0;
    }

    /* Any 0Dh arm ends an unchained 13h; the two are mutually exclusive and
       share the arena block. */
    x86_vga_modey_armed = 0;

    memset(gc, 0, sizeof gc);
    GC_BIT_MASK = 0xFF;
    GC_COLOR_DC = 0x0F;
    map_mask = 0x0F;
    latch[0] = latch[1] = latch[2] = latch[3] = 0;
    memset(ega_planes, 0, 4u * ega_plane_size);
    /* The WHOLE aperture, not just the shadow. An EGA in a planar mode decodes
       all 64 KB at A000:0000 and software uses it -- Keen 4's first act after
       the mode set is a `rep stosw` with CX=8000h over the lot. Only the bottom
       16 KB is the shadow; the rest must read back as ZERO, because a write up
       there reaches a plane offset this card has not fitted and is discarded.
       Leaving stale bytes would have the guest read back data it never stored,
       which is exactly the `z`-marker failure tega2.asm exists to catch. */
    memset(x86_vga_planar_aperture, 0, X86_VGA_APERTURE_BYTES);

    x86_vga_ega_armed = 1;
    ega_shadow_sig = ega_read_sig();
    ega_shadow_rebuild();
    hook_set(1);
    return 1;
}

/* ---------------------------------------------------------------------------
 * REGISTER WRITES WITH AN EFFECT. x86_io.c owns the files; these are the
 * writes that change what the adapter DOES.
 */

void x86_vga_planar_seq_write(unsigned idx, uint8_t val)
{
    if ((idx & 7u) == 2u) {
        map_mask = (uint8_t)(val & 0x0Fu);
        planar_reclass();
        return;
    }
    if ((idx & 7u) == 4u) {
        /* Bit 3 is Chain-4. Clearing it is Mode Y; setting it is chained 13h.
           The CALLER has already checked we are in mode 13h. */
        x86_vga_modey_arm((val & 0x08u) == 0u);
    }
}

void x86_vga_planar_gc_write(unsigned idx, uint8_t val)
{
    uint8_t before;

    idx &= 15u;
    before = gc[idx];
    gc[idx] = val;
    if (before == val)
        return;
    /* Every GC register can move the write class, so this is unconditional --
       unlike the read-shadow rebuild below, which only four of them affect. */
    planar_reclass();
    /* Only four registers change what a READ returns, and the rebuild is
       expensive enough that the guard is the point -- see ega_shadow_rebuild().
       2 Colour Compare, 4 Read Map Select, 5 Mode, 7 Colour Don't Care.
       "The register changed" is NOT the same question as "what a read returns
       changed", and the difference is the whole cost: see ega_read_sig(). */
    if (idx == 2u || idx == 4u || idx == 5u || idx == 7u) {
        uint32_t sig;

        SHPROF_GC_WRITE();
        sig = ega_read_sig();
        if (sig == ega_shadow_sig)
            return;
        ega_shadow_sig = sig;
        ega_shadow_rebuild();
    }
}

/*
 * The display start has been reprogrammed. Two consequences, and they are
 * separate: the stride learning, and the present edge.
 */
void x86_vga_planar_start_write(void)
{
    uint32_t s, d;

    if (!x86_vga_modey_armed)
        return;
    s = x86_vga_crtc_start();
    if (s == modey_prev_start)
        return;

    d = (s > modey_prev_start) ? s - modey_prev_start : modey_prev_start - s;
    modey_prev_start = s;
    if (d >= X86_MODEY_PGMIN && d <= X86_MODEY_PGMAX
        && (!modey_learned || d < x86_vga_modey_page)) {
        modey_learned = 1;
        x86_vga_modey_page = d;
    }

    /*
     * PRESENT ON THE GUEST'S PAGE FLIP.
     *
     * Aliasing the pages means the single page of storage holds, at any
     * instant, whatever the guest is drawing RIGHT NOW. Blitting at the
     * emulator's own frame boundary composites a finished frame with a
     * half-drawn one -- measured on the reference against a 256 KB card, and
     * reported as "vertical slabs from different frames". A display-start
     * change is the guest stating that the page it just left is COMPLETE, which
     * is the only instant an aliased page is known good.
     */
    x86_vga_modey_flips++;
    x86_video_page_flip();
    x86_video_modey_presented(1);
}

/* ---------------------------------------------------------------------------
 * WHEN THE FRAME LOOP MAY PAINT A MODE Y SCREEN BY ITSELF.
 *
 * THIS IS A PORT, NOT A DESIGN. It is references/dos_port_prior/dos_video.c
 * lines 1529-1656 -- `modey_presented`, `modey_idle` and DOS_MODEY_IDLE_FRAMES
 * -- transcribed, and 15-mode-y.md section 4 item 4 is its rationale. Four
 * rounds of local invention replaced it (an 8-frame timer, a 60-frame staleness
 * bound, a doubling back-off, then a cadence estimator with decay); each fixed
 * the bug the last one introduced, and the last one FLICKERED on hardware and
 * was reverted. What is left is the thing that was already known to work.
 *
 * The whole model is three lines of state:
 *
 *   - the screen is presented when the GUEST FLIPS (x86_vga_planar_start_write
 *     -> x86_video_page_flip), because a display-start change is the guest
 *     saying "the page I was displaying is complete", and with every page
 *     aliased into one page of storage that is the only instant it is known
 *     good. BOTH LCD buffers are painted there, or the firmware's unconditional
 *     swap alternates between this frame and the last one;
 *   - the frame after such a present the loop paints NOTHING, and starts the
 *     idle count over;
 *   - after DOS_MODEY_IDLE_FRAMES (8) frame-loop iterations with no flip, the
 *     hold is ABANDONED and the loop paints every frame exactly as every other
 *     mode does. ~130 ms at 60 Hz: long enough that a title pausing between
 *     frames does not flicker, short enough that a title which has stopped
 *     flipping altogether -- an intro screen or a menu drawn straight into the
 *     page it is displaying, which is what WOLF3D's do -- is not a frozen
 *     screen, and is REVEALED AS IT DRAWS rather than appearing all at once.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO, and it is the point: it does not try to
 * infer whether the guest has finished drawing. Nothing observable separates
 * "quiet because the frame is finished" from "quiet in the middle of a frame",
 * so every rule built on that inference was wrong in one direction or the
 * other, and the elaborations needed to patch each case are what produced the
 * flicker. A title slower than 8 loop frames per flip TEARS here. That is the
 * reference's known, accepted trade -- 15-mode-y.md section 4, "the visible
 * artefact is TEARING" -- and x86_modey_mid_draw counts it rather than
 * pretending it is gone.
 *
 * -DX86_MODEY_PRESENT_ON_FLIP=0 is the reference's own -DDOS_MODEY_PRESENT_ON_
 * FLIP=0: no hold at all, paint every frame, the pure tearing model. It is the
 * negative control for this file (scripts/run_planar_controls.py, arm FLIP).
 */
#ifndef X86_MODEY_PRESENT_ON_FLIP
#define X86_MODEY_PRESENT_ON_FLIP 1
#endif

/* dos_video.c:1532. Frames with no page flip after which the hold is abandoned. */
#define X86_MODEY_IDLE_FRAMES   8u

volatile uint32_t x86_modey_fallbacks;    /* frames the LOOP painted          */
volatile uint32_t x86_modey_flip_gap;     /* frames between the last 2 flips  */
volatile uint32_t x86_modey_flip_gap_max;
/*
 * THE WITNESS, and under this policy it is a measurement rather than an alarm.
 * A loop paint inside a title that flips, with stores banked since that flip,
 * put a half-drawn page on the panel. The reference accepts those above 8 idle
 * frames; counting them is how "how much does this title tear" stops being an
 * argument and how a future change is graded against this baseline.
 */
volatile uint32_t x86_modey_mid_draw;     /* loop paints with stores pending  */
volatile uint32_t x86_modey_mid_draw_max; /* worst store count at such a paint*/

static uint32_t modey_presented;   /* a flip painted the screen last frame    */
static uint32_t modey_idle = X86_MODEY_IDLE_FRAMES;
static uint32_t modey_since_flip;  /* frames since the last flip -- witness   */
static uint32_t modey_flipped;     /* this title has flipped at least once    */
static uint32_t modey_stores_flip; /* x86_vga_planar_stores at the last flip  */

/* Per-ARMING policy state. The witnesses above are cumulative and host-visible;
   this is not. The reference gets this for free -- its `modey_idle` is forced
   to the limit by every non-Mode-Y blit -- but our paint_due() is only called
   at all while armed, so the arm does it explicitly. Starting AT the limit is
   what makes a title that never flips paint from its very first frame. */
void x86_video_modey_policy_reset(void)
{
    modey_presented = 0;
    modey_idle = X86_MODEY_IDLE_FRAMES;
    modey_since_flip = 0;
    modey_flipped = 0;
    modey_stores_flip = x86_vga_planar_stores;
}

void x86_video_modey_presented(int from_flip)
{
    if (!from_flip)
        return;         /* a loop paint changes no policy state: dos_video.c
                           falls through to the blit and leaves modey_idle at
                           the limit, so it keeps painting until the next flip */
    x86_modey_flip_gap = modey_since_flip;
    if (modey_since_flip > x86_modey_flip_gap_max)
        x86_modey_flip_gap_max = modey_since_flip;
    modey_since_flip = 0;
    modey_stores_flip = x86_vga_planar_stores;
    modey_flipped = 1;
    modey_presented = 1;
}

int x86_video_modey_paint_due(void)
{
    /* Every other mode owes a paint every frame: its storage is not aliased,
       so there is no half-drawn page to catch. dos_video.c spells this as the
       `else` that parks modey_idle at the limit. */
    if (!x86_vga_modey_armed) {
        modey_idle = X86_MODEY_IDLE_FRAMES;
        return 1;
    }

    modey_since_flip++;

#if !X86_MODEY_PRESENT_ON_FLIP
    return 1;                       /* THE NEGATIVE CONTROL: the tearing model */
#else
    /* dos_video.c:1645 */
    if (modey_presented) {
        modey_presented = 0;
        modey_idle = 0;
        return 0;
    }
    if (modey_idle < X86_MODEY_IDLE_FRAMES) {
        modey_idle++;
        return 0;
    }

    x86_modey_fallbacks++;
    /* Only meaningful for a title that FLIPS: "half-drawn" is measured against
       the last frame the guest called complete, and a title that never flips
       has never called one complete. Counting those would make the witness read
       high for exactly the screens the hatch is there to reveal. */
    if (modey_flipped && x86_vga_planar_stores != modey_stores_flip) {
        uint32_t n = x86_vga_planar_stores - modey_stores_flip;
        x86_modey_mid_draw++;
        if (n > x86_modey_mid_draw_max)
            x86_modey_mid_draw_max = n;
    }
    return 1;
#endif
}

/* "The page you were displaying is finished; show it." Overridden by the
   frame loop on the device; a no-op in the tier-1 harness and anywhere else
   with no LCD, so every link resolves. */
__attribute__((weak)) void x86_video_page_flip(void) { }
