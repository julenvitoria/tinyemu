/*
 * The planar VGA adapter: EGA mode 0Dh and unchained 13h ("Mode Y").
 *
 * This header is the whole interface between the three files that share the
 * planar model, and the split is deliberate:
 *
 *   x86_video_planar.c  owns the ADAPTER -- planes, latches, write modes, the
 *                       page fold. It is the only file that knows where a
 *                       plane byte lives.
 *   x86_io.c            owns the REGISTER FILES (Sequencer, Graphics
 *                       Controller, CRTC, Attribute Controller). It feeds the
 *                       adapter the writes that have an effect and answers
 *                       geometry questions; the adapter keeps NO second copy.
 *   x86_video.c         owns the BLIT and the arena claims.
 *
 * Nothing here is duplicated state. That is not tidiness: the previous port's
 * record (references/8086tiny/docs/video/11-ega-planar.md) is that a second
 * copy of the Map Mask or of the CRTC Offset is how a write path and a blit
 * come to disagree about the same screen.
 */
#ifndef X86_VIDEO_PLANAR_H
#define X86_VIDEO_PLANAR_H

#include <stdint.h>

/* ---- geometry, all of it derived and none of it stored twice -------------
 *
 * ROW PITCH IS 2 x THE CRTC OFFSET REGISTER. The Offset is in WORDS -- a real
 * EGA BIOS writes 0x14 (20) for mode 0Dh and mode 0Dh is 40 bytes per row, and
 * writes 0x28 (40) for a Mode Y page whose row is 80 plane offsets. Getting
 * this wrong shreds the picture and it has been got wrong before, complete
 * with a confident autocorrelation measurement that agreed with the mistake:
 * 11-ega-planar.md, "The Offset unit -- WORDS".
 */
unsigned       x86_vga_crtc_start(void);    /* display start, plane offsets   */
unsigned       x86_vga_row_pitch(void);     /* 2 x CRTC Offset                */
unsigned       x86_vga_pel_panning(void);   /* ATC index 0x13, low 3 bits     */
const uint8_t *x86_vga_attr_palette(void);  /* ATC 0x00-0x0F, 16 entries      */

/* Plant the CRTC geometry a real VGA BIOS writes for `mode`. Chained 13h does
   not need it; a title that unchains without programming the Offset itself
   does, or it inherits mode 0Dh's 20 and renders at half the stride. */
void x86_vga_crtc_preset(uint8_t mode);

/* ---- register writes with an effect, fed from x86_io.c ------------------- */

/* Sequencer. Index 2 is the Map Mask; index 4 bit 3 is Chain-4, and clearing
   it is what arms Mode Y. */
void x86_vga_planar_seq_write(unsigned idx, uint8_t val);
/* Graphics Controller: Set/Reset, Enable Set/Reset, Colour Compare, Data
   Rotate, Read Map Select, Mode, Miscellaneous, Colour Don't Care, Bit Mask. */
void x86_vga_planar_gc_write(unsigned idx, uint8_t val);
/* CRTC display start (index 0x0C/0x0D) has been reprogrammed. Feeds the Mode Y
   stride learning and, on a real change, the present-on-page-flip edge. */
void x86_vga_planar_start_write(void);

/* ---- arming -------------------------------------------------------------- */

/* Unchained 13h. Idempotent; does NOT clear the planes (they are the aperture,
   which the 13h mode set just cleared). */
void x86_vga_modey_arm(int on);
/* EGA 0Dh. Returns 0 if the plane storage could not be claimed, in which case
   nothing is armed and the mode must be refused. */
int  x86_vga_planar_arm(int on);

int  x86_vga_is_unchained(void);
int  x86_vga_planar_active(void);

/* ---- storage, handed in by x86_video.c out of the arena ----------------- */

/* Sizes are in PLANE OFFSETS, not bytes: one plane offset is four interleaved
   bytes, so a block of N offsets costs 4N bytes of arena. Quoting the two
   quantities in the same units is how a card comes out four times too small. */
#define X86_EGA_PLANE_OFFSETS   0x2000u   /* 8,192 -> a 32 KB card, 32,768 B  */
/* The Mode Y off-screen cache is FITTED, not fixed: x86_video.c walks down
   from what the mode can address until the arena takes the claim, so read
   x86_vga_modey_cache_offs for what actually exists. This constant survives
   only as the old shipping size, which is the size Wolf3D's cached pictures
   came out as noise at (16,896 stores discarded above 0xE300 on hardware). */
#define X86_MODEY_CACHE_OFFSETS 0x2000u   /* 8,192 off-screen, 32,768 B       */
/* The aliased Mode Y page: 4 * 0x4000 == 0x10000, the aperture exactly. */
#define X86_MODEY_PLANE_OFFSETS 0x4000u

void x86_vga_ega_set_planes(void *planes, uint32_t plane_offsets);
void x86_vga_modey_set_cache(void *cache, uint32_t offsets);
/* REAL storage for Mode Y display pages 1 and 2, each X86_MODEY_PAGE_BYTES.
   NULL leaves that page ALIASED onto page 0 -- the old behaviour, and the cause
   of Wolf3D's corrupted gameplay picture: the raycaster draws the next page
   into the bytes the panel is showing. Page 2 is ignored without page 1. */
#define X86_MODEY_PAGE_BYTES (4u * X86_MODEY_PLANE_OFFSETS)   /* 65,536 */
void x86_vga_modey_set_pages(void *p1, void *p2);

const uint8_t *x86_vga_ega_planes(void);
const uint8_t *x86_vga_modey_cache_ptr(void);
uint32_t       x86_vga_ega_plane_offsets(void);

/* ---- the blit's half of the address model -------------------------------
 *
 * The aperture plane offset a DISPLAY address maps to, or ~0u when it has
 * none (off-screen cache, or a page tail that is never shown). Expressed
 * THROUGH the same cell lookup the store path uses, so the two cannot drift.
 */
uint32_t x86_vga_modey_row(uint32_t a);

/* The same answer in POINTER form, which is what a real multi-page card needs:
   pages 1 and 2 have their own storage and are not aperture offsets any more.
   Fills *base with the page's storage and *off with the plane offset inside it
   (always < X86_MODEY_PLANE_OFFSETS); returns 0 when the row has no storage,
   which the blit must paint black. */
int x86_vga_modey_row_at(uint32_t a, const uint8_t **base, uint32_t *off);

/* "The page you were displaying is finished; show it." Called from the CRTC
   display-start write, which is the only instant an aliased Mode Y page is
   known good. Weak in x86_video_planar.c so the tier-1 harness and any link
   with no LCD still resolve; the frame loop overrides it. */
void x86_video_page_flip(void);

/* ---- the presentation policy, and why it is HERE and not in the frame loop -
 *
 * It has to be testable without an LCD: WHICH FRAMES ARE PAINTED is a question
 * no pixel test can ask and no device can be asked safely. So the decision
 * lives in the core beside the store counter it reads, and the frame loop only
 * obeys it. tests/test_video.py drives it directly.
 *
 * The policy itself is a PORT of references/dos_port_prior/dos_video.c:1529
 * -1656; see the block comment on it in x86_video_planar.c.
 */
/* Called once per frame-loop iteration. Non-zero: the loop must paint. In
   Mode Y that is only after DOS_MODEY_IDLE_FRAMES (8) frames with no page
   flip -- the reference's escape hatch, ported verbatim; the guest's own flip
   is what normally presents. Every other mode: always. */
int  x86_video_modey_paint_due(void);
/* "A frame has just been put on the panel." Only from_flip != 0 changes policy
   state; a loop paint is deliberately inert, exactly as in dos_video.c, which
   keeps painting every frame until the guest flips again. */
void x86_video_modey_presented(int from_flip);
/* Forget the previous title's flip state and start with the hold ABANDONED, so
   a title that never flips is painted from its first frame. From modey_arm(). */
void x86_video_modey_policy_reset(void);

extern volatile uint32_t x86_modey_fallbacks;     /* frames the LOOP painted   */
extern volatile uint32_t x86_modey_flip_gap;
extern volatile uint32_t x86_modey_flip_gap_max;
/* Loop paints taken with stores banked since the last flip -- half-drawn pages
   on the panel. Under the ported policy these are EXPECTED above 8 idle frames
   (15-mode-y.md section 4: "the visible artefact is TEARING"); the counter says
   how much, and is the baseline any future change is graded against. */
extern volatile uint32_t x86_modey_mid_draw;
extern volatile uint32_t x86_modey_mid_draw_max;

/* ---- host-visible witnesses; all of this is the deliverable -------------- */
/* Put the aperture's fold sentinel back after something remapped 0xA0000.
   Called by x86_map_fixed(); a no-op unless the trap is armed. */
void x86_vga_planar_fold_refresh(void);
/* The fold trap's state, for a harness to read: refused means the aperture was
   not where the sentinel encoding needs it (x86_cpu.h, X86_FOLD_VGA_OK). */
extern volatile uint32_t x86_vga_fold_trap_armed;
extern volatile uint32_t x86_vga_fold_trap_refused;

extern volatile uint32_t x86_vga_planar_stores;   /* stores that took the hook */
/* Stores by write class: the CPU byte straight into the enabled planes, the
   write-mode-1 latched page copy, and everything else. The mix is what says
   whether a title's frame is worth a fast path, and at tier 1 it is the
   coverage proof that the pixel arms enter all three. */
extern volatile uint32_t x86_vga_planar_cls_plane;
/* ...of which the Map Mask selected exactly ONE plane, so the store was a lone
   STRB and not a four-byte read-modify-write. The ratio of these two is what
   says whether the single-plane path is the whole frame or a corner case. */
extern volatile uint32_t x86_vga_planar_cls_plane1;
extern volatile uint32_t x86_vga_planar_cls_copy;
extern volatile uint32_t x86_vga_planar_cls_general;
extern volatile uint32_t x86_vga_planar_lost;     /* stores with no storage    */
extern volatile uint32_t x86_vga_modey_flips;     /* display-start changes     */
extern volatile uint32_t x86_vga_modey_page;      /* the learned page stride   */
extern volatile uint32_t x86_vga_modey_armed;     /* 1 while unchained         */
extern volatile uint32_t x86_vga_ega_armed;       /* 1 while 0Dh is armed      */
extern volatile uint32_t x86_vga_ega_refused;     /* 0Dh claims refused        */
extern volatile uint32_t x86_vga_latch_loads;     /* latch reloads at a store  */
extern volatile uint32_t x86_vga_latch_miss;      /* ...from an absent cell    */
/* x86_vga_planar_lost, split by SOURCE -- one lump sum cannot be reconciled
   with the reference's per-region measurement. See x86_video_planar.c. */
extern volatile uint32_t x86_vga_modey_lost_cache;   /* stores at/above 0xE300 */
extern volatile uint32_t x86_vga_modey_lost_tail;    /* a page's rows 205+     */
extern volatile uint32_t x86_vga_modey_lost_nocache; /* arena refused the cache*/
/* Plane offsets of off-screen cache actually claimed -- whatever the arena
   would give, 0 when it refused. The one read that settles both "is the
   0xC300 cache genuinely there" and "how far up does it reach": the cache
   covers 0xC300 .. 0xC300 + this - 1, and a store past that is counted in
   x86_vga_modey_lost_cache and lost. */
extern volatile uint32_t x86_vga_modey_cache_offs;
/* Display pages with storage of their OWN: 1 = the aliased fold (the defect),
   2 = the draw-over-displayed hazard removed, 3 = the hardware layout. */
extern volatile uint32_t x86_vga_modey_pages;
/* Cells that had to fall back onto page 0. THE LOUD ONE: donation refused is
   otherwise indistinguishable from this bug coming back. */
extern volatile uint32_t x86_vga_modey_alias_hits;
/* Highest and lowest window offset a cache store was discarded at. A count
   cannot say how much bigger the cache needs to be; these can. */
extern volatile uint32_t x86_vga_modey_lost_hi;
extern volatile uint32_t x86_vga_modey_lost_lo;

#endif /* X86_VIDEO_PLANAR_H */
