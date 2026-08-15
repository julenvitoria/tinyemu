/*
 * LIM EMS 3.2 -- the expanded-memory interface, and why it is the OPPOSITE
 * shape to XMS.
 *
 * ---- The one design fact that shapes everything below ------------------
 *
 * EMS IS A MAPPED INTERFACE, NOT A COPY ONE. x86_xms.h opens by saying that no
 * instruction anywhere in the guest ever dereferences an XMS byte, and that is
 * the whole reason XMS memory is never in the fold table, never AXI-resident,
 * and costs the interpreter nothing.
 *
 * EMS is the exact inverse. It puts a 64 KB PAGE FRAME inside the real-mode
 * address space -- E000:0000 here -- divided into four 16 KB physical pages,
 * and the guest maps a logical page into one of those windows with INT 67h
 * AH=44h and then reads and writes it with ORDINARY INSTRUCTIONS. MOV, STOSW,
 * a REP MOVSB straight out of a decompressor. Nothing is routed through us.
 *
 * So the four mapped pages MUST be resident in AXI and MUST be in the fold
 * table. Giving EMS the XMS treatment -- bytes in the pagefile, fetched when
 * asked for -- would mean every guest access to the page frame faulted. That
 * is not a slow path, it is a cliff: a decompressor writing 16 KB through the
 * window would take four thousand faults where it should take four.
 *
 * And giving XMS the EMS treatment would be equally wrong in the other
 * direction -- pinning AXI for memory no instruction can address. One
 * allocator, two residency rules, and the rule is chosen by whether the guest
 * can name the byte.
 *
 * ---- Pin on map, release on unmap ---------------------------------------
 *
 * The saving grace is that ONLY 64 KB IS LIVE AT A TIME, and AH=44h tells us
 * exactly when that changes. The hot set is bounded at four logical pages and
 * it is EXPLICITLY SIGNALLED -- which is strictly better information than a
 * pager ever gets from faults, because it arrives before the access instead of
 * after it.
 *
 * So a map does this and nothing else:
 *
 *   for each of the logical page's four 4 KB granules
 *       fault it into a frame   (x86_guest_ptr -> x86_pager_fault)
 *       PIN it                  (x86_pin_pages -- the arena's bitmap)
 *       point the window's fold entry at that frame  (x86_map_fixed)
 *
 * and an unmap is the same three steps run backwards. The pin is taken
 * IMMEDIATELY after each fault, not after all four, because faulting granule 2
 * can evict granule 1 if the pool is tight -- and the window's fold entry would
 * then point at another page's memory with nothing saying so.
 *
 * THE PIN BITMAP IS THE ARENA'S, NOT A SECOND MECHANISM. x86_pin_pages() /
 * x86_page_is_pinned() already exist for display storage, they are already the
 * victim picker's first veto (x86_pager.c), and they are already indexed by
 * guest page. A mapped EMS page is exactly the same kind of thing as a VRAM
 * page -- memory something outside the interpreter's control is reading -- so
 * it uses the same bitmap and inherits the same proof.
 *
 * An UNMAPPED logical page is cold by definition. It is an ordinary pager page
 * from that moment: it may sit in a frame until the pool wants it back, it may
 * be evicted to the pagefile, and it costs nothing while it waits. That is the
 * cost curve this design exists for -- a slope, not a cliff.
 *
 * ---- Where the bytes live ------------------------------------------------
 *
 * IN THE PAGER'S OWN SPACE, at guest pages X86_EMS_BACK_PAGE and up -- linear
 * 4 MB. Not in a byte range of the pagefile the way XMS is, because these pages
 * must be able to BE frames; the pager's slot-ranged store is the right ABI
 * precisely when a page is a page.
 *
 * Three properties make that address safe, and all three are arithmetic:
 *
 *   * A REAL-MODE GUEST CANNOT REACH IT. The widest real-mode address is
 *     FFFF:FFFF = 0x10FFEF, ten megabytes below. There is no segment:offset
 *     pair that names an EMS backing page, which is what makes it legitimate
 *     to hand its FRAME to the guest at 0xE0000 and nothing else.
 *   * IT CANNOT COLLIDE WITH GUEST RAM. The pager's guest-RAM window is
 *     [0, 640 KB) -- pages 0..159. This window starts at page 1024.
 *   * IT CANNOT COLLIDE WITH XMS IN THE FILE. The pager's slot IS the guest
 *     page number, so these pages occupy file bytes 4 MB..5 MB, and
 *     src/main.c puts the XMS store at 8 MB (X86_PG_PAGES * X86_PG_SIZE).
 *
 * The pager owns a SECOND window for this (x86_pager_own_aux), rather than one
 * widened window: widening would make every page between 640 KB and 4 MB
 * ownable, and a stray access up there would then quietly get a frame instead
 * of the clean fault it deserves.
 *
 * ---- The page frame, and why 0xE0000 ------------------------------------
 *
 * E000:0000 is the LIM conventional frame and it is FREE ON THIS MACHINE. The
 * upper megabyte here is (src/main.c):
 *
 *   0xA0000-0xAFFFF   VGA mode 13h aperture -- real storage
 *   0xB8000-0xBFFFF   CGA VRAM              -- real storage
 *   0xFF000-0xFFFFF   BIOS identification page
 *   everything else   ONE 4 KB page of 0xFF, aliased across the whole region
 *
 * so 0xE0000-0xEFFFF is sixteen fold entries all pointing at the same open-bus
 * filler page. That is CHECKED, not assumed: x86_ems_set_backing() resolves all
 * sixteen and refuses the whole interface unless they are mapped and all
 * resolve to the SAME host page. VRAM, the aperture and the id page each
 * resolve somewhere of their own, so any of them straying into the window makes
 * the check fail rather than making the guest fail. x86_ems_frame_refused says
 * it happened.
 *
 * The idle fold entries are SAVED at that moment and restored on unmap, so an
 * unmapped window reads 0xFF -- open bus, which is what an unmapped EMS window
 * is on real hardware -- instead of faulting.
 *
 * ---- What AH=42h advertises, and why that is honest --------------------
 *
 * X86_EMS_PAGES logical pages, and ZERO WHEN THERE IS NO PAGEFILE. That second
 * half is the same honesty rule x86_xms.h states and for the same reason: with
 * no backing store the pager cannot evict, so the only pages EMS could hand out
 * are ones it would have to keep resident forever, and a guest told it has a
 * megabyte it will lose fails much later and somewhere unrelated. With no
 * store, EMMXXXX0 does not open and INT 67h is left at the bare IRET.
 *
 * 1024 KB is bounded by TRANSFER, not by the AXI pool. Four mapped logical
 * pages pin sixteen of the 160 frames, leaving 144 -- and KEEN4's measured peak
 * is 134 (x86_pager.h), so the pinned set does not by itself push the pool into
 * exhaustion. The other 60 logical pages are ZERO tier until written and FILE
 * tier after, exactly like guest RAM.
 *
 * ---- What we do NOT have, said out loud --------------------------------
 *
 * LIM 4.0 (AH=50h-58h): absent, and AH=46h says 3.2 so nothing asks. 4.0's
 * point is mappable regions OUTSIDE the page frame and alternate map registers,
 * i.e. more windows to keep pinned; claiming it and refusing the calls is worse
 * than a version number that tells the truth.
 *
 * AH=4Fh/51h (reallocate pages): 4.0. A 3.2 guest deallocates and reallocates.
 */
#ifndef X86_EMS_H
#define X86_EMS_H

#include <stdint.h>

/* ---- geometry ----------------------------------------------------------- */

/* The page frame. Four 16 KB physical pages at E000:0000. */
#define X86_EMS_FRAME_SEG    0xE000u
#define X86_EMS_FRAME_LIN    ((uint32_t)X86_EMS_FRAME_SEG << 4)
#define X86_EMS_PAGE_SIZE    16384u
#define X86_EMS_WINDOWS      4u
#define X86_EMS_FRAME_BYTES  (X86_EMS_PAGE_SIZE * X86_EMS_WINDOWS)
/* 4 KB granules per 16 KB logical page -- the unit the pager and the pin
   bitmap actually deal in. */
#define X86_EMS_SUBPAGES     (X86_EMS_PAGE_SIZE / 4096u)

/* Logical pages, and where they live in the pager's space. 64 pages = 1024 KB
   = 256 pager pages at guest page 1024, i.e. linear 4 MB..5 MB. See the header
   comment for the three reasons that address is safe. */
#define X86_EMS_PAGES        64u
#define X86_EMS_BACK_PAGE    1024u
#define X86_EMS_BACK_PAGES   (X86_EMS_PAGES * X86_EMS_SUBPAGES)
#define X86_EMS_BACK_LIN     ((uint32_t)X86_EMS_BACK_PAGE << 12)

/* Handles. Handle 0 is the driver's own in the LIM spec and is never issued,
   so AH=4Bh reports one more than the number in use here. */
#define X86_EMS_HANDLES      16u

/* ---- the INT 67h entry, and the device header --------------------------- */
/*
 * Both live in the HLE stub block at 0x600-0x6FF (x86_hle.c), and the DEVICE
 * HEADER is why this needs a segment of its own rather than another vector
 * stub. Some titles do not open EMMXXXX0 at all: they read the INT 67h vector,
 * take its SEGMENT, and compare the eight bytes at offset 0x0A of that segment
 * against "EMMXXXX0" -- the name field of a DOS character-device header. Offset
 * 0x0A of STUB_SEG is inside the third vector stub, so the vector points at
 * 0064:0020 instead and the header sits at 0064:0000 = linear 0x640.
 *
 * Layout, all of it inside the 0x600-0x6FF block x86_hle.c static-asserts:
 *   0x600..0x62F   the eleven vector stubs
 *   0x640..0x651   the EMMXXXX0 device header      <- 0064:0000
 *   0x660..0x663   0F 0B 67 CF, the INT 67h stub   <- 0064:0020
 *   0x6C0..0x6C3   the XMS control entry (RETF)
 *   0x6FC          the bare IRET
 */
#define X86_EMS_ENTRY_SEG    0x0064u
#define X86_EMS_HDR_OFF      0x0000u   /* relative to X86_EMS_ENTRY_SEG */
#define X86_EMS_ENTRY_OFF    0x0020u
#define X86_EMS_HDR_LIN      (((uint32_t)X86_EMS_ENTRY_SEG << 4) + X86_EMS_HDR_OFF)
#define X86_EMS_ENTRY_LIN    (((uint32_t)X86_EMS_ENTRY_SEG << 4) + X86_EMS_ENTRY_OFF)

/* ---- status codes, returned in AH (INT 67h does not use CF) ------------- */
#define X86_EMM_OK           0x00u
#define X86_EMM_SOFT_ERR     0x80u
#define X86_EMM_HARD_ERR     0x81u
#define X86_EMM_BAD_HANDLE   0x83u
#define X86_EMM_BAD_FUNC     0x84u
#define X86_EMM_NO_HANDLES   0x85u
#define X86_EMM_SAVE_STATE   0x86u
#define X86_EMM_TOO_MANY     0x87u   /* more pages than exist at all        */
#define X86_EMM_NO_PAGES     0x88u   /* more pages than are free            */
#define X86_EMM_ZERO_PAGES   0x89u
#define X86_EMM_BAD_LOGICAL  0x8Au
#define X86_EMM_BAD_PHYSICAL 0x8Bu
#define X86_EMM_SAVE_FULL    0x8Cu
#define X86_EMM_SAVE_DONE    0x8Du
#define X86_EMM_NO_SAVE      0x8Eu
#define X86_EMM_BAD_SUBFUNC  0x8Fu

/* ---- wiring ------------------------------------------------------------- */
/*
 * Turn EMS on. `pages` is how many 16 KB logical pages to advertise, capped at
 * X86_EMS_PAGES; ZERO means there is no expanded memory and the whole interface
 * is absent -- honestly, exactly as x86_xms_set_backing(.., 0) does.
 *
 * MUST be called AFTER the upper megabyte has been mapped, because it checks
 * the page frame is free before it claims it, and after the pager has been
 * given its frames and its backing store.
 *
 * Returns 0 if EMS came up, non-zero if it refused; x86_ems_frame_refused and
 * x86_ems_present say which and why.
 */
int  x86_ems_set_backing(uint32_t pages);
/* Drop every handle and unmap every window. Called at guest reset. */
void x86_ems_reset(void);
/* Is EMS actually there? Consulted by x86_dos.c for the EMMXXXX0 open. */
int  x86_ems_present(void);
/* The INT 67h service, reached as 0F 0B 67 through the stub at 0064:0020. */
int  x86_ems_int67(void);

/* ---- host-readable state ------------------------------------------------
 * Every one of these is a number a probe can name. "EMS is present" is not a
 * measurement; "the guest mapped logical page 5 into window 2 and the sixteen
 * kilobytes it wrote there were still correct after two remaps" is.
 */
extern volatile uint32_t x86_ems_present_v;      /* 1 once the frame is ours */
extern volatile uint32_t x86_ems_frame_refused;  /* the frame was NOT free   */
extern volatile uint32_t x86_ems_total_pages;    /* what AH=42h advertises   */
extern volatile uint32_t x86_ems_used_pages;     /* sum of live handles      */
extern volatile uint32_t x86_ems_handles_used;
extern volatile uint32_t x86_ems_device_opens;   /* EMMXXXX0 opened          */
extern volatile uint32_t x86_ems_calls;          /* INT 67h calls serviced   */
extern volatile uint32_t x86_ems_maps;           /* AH=44h, a page mapped    */
extern volatile uint32_t x86_ems_unmaps;         /* AH=44h with BX=FFFFh     */
extern volatile uint32_t x86_ems_map_fails;      /* a map we could not back  */
extern volatile uint32_t x86_ems_pinned_pages;   /* 4 KB pages pinned NOW    */
extern volatile uint32_t x86_ems_last_err;       /* last AH returned failing */
extern volatile uint32_t x86_ems_unhandled;      /* last AH we do not have   */

#endif /* X86_EMS_H */
