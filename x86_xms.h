/*
 * XMS 2.0 -- the HIMEM.SYS interface, and why it costs the interpreter nothing.
 *
 * ---- The one design fact that shapes everything below ------------------
 *
 * XMS IS A BLOCK-COPY INTERFACE, NOT A MAPPED ONE. A real-mode guest cannot
 * address extended memory: it allocates a handle (AH=09h) and then MOVES bytes
 * in bulk through AH=0Bh, which we service. There is no instruction anywhere in
 * the guest that ever dereferences an XMS byte.
 *
 * So XMS memory is never in the fold table, never resident in AXI, and never a
 * page the victim picker can see. It has NO fast path to be on: no FOLD_INVALID
 * check, no seam, no MEM_TRAP, no eviction pressure, no residency. It lives
 * wherever storage is cheapest -- here, in the SD pagefile -- and the price is
 * paid only inside AH=0Bh, at a moment the guest chose.
 *
 * That is the whole reason XMS comes before EMS in PLAN.md's Queue C. EMS's
 * page frame IS mapped and IS read by ordinary instructions, so it must import
 * the paged path's cost. XMS must not, and handing out pager frames for it --
 * the obvious implementation -- would do exactly that for no gain.
 *
 * ---- Where the bytes live, and how they cannot collide -----------------
 *
 * The backing store is BYTE-RANGED, not slot-ranged, unlike x86_pg_read_fn.
 * That difference is deliberate and it is a cost decision: a slot ABI would
 * force a read-modify-write of a whole 4 KB slot for any move that is not page
 * aligned, and with the pagefile's open-per-call design (~25 ms measured, see
 * docs/12 §1a) a 102 KB DOS move would become 26 opens instead of one.
 * `open`/`close` bracket a whole move so the file is opened ONCE for it.
 *
 * `off` is a byte offset into the XMS store and NOTHING ELSE -- the caller
 * decides where that store physically is. src/main.c puts it in the same
 * x86page.swp at byte offset X86_PG_PAGES * X86_PG_SIZE (8 MB), i.e. strictly
 * above every slot the pager can ever address, because the pager's slot IS the
 * guest page number and guest page numbers stop at X86_PG_PAGES. A guest page
 * and an XMS byte therefore cannot land on the same byte of the file, and that
 * is an arithmetic property rather than a convention.
 *
 * ---- What AH=08h advertises, and why that is honest --------------------
 *
 * X86_XMS_DEFAULT_KB, and it is 0 WHEN THERE IS NO BACKING STORE. That second
 * half is the honest part: with no pagefile there is nowhere to put a byte, so
 * we advertise nothing and AH=09h fails with 0A0h rather than handing out
 * memory we would silently lose. A guest told it has memory it cannot get back
 * fails later and somewhere unrelated, which is the exact shape of failure
 * this project keeps hitting.
 *
 * The size itself is not bounded by the 160-frame AXI pool -- no XMS byte is
 * ever in a frame -- it is bounded by the card and by transfer time. 4 MB is
 * one open plus a linear read/write per move; it covers MS-DOS's own ~102 KB,
 * every XMS-using title of the period, and the DPMI pool Queue C item 3 will
 * draw on. Raise it by changing one constant here; the cost is file space on
 * the card and nothing else.
 *
 * ---- What we do NOT have, said out loud --------------------------------
 *
 * HMA (AH=01h/02h): refused with BL=90h, "HMA does not exist". The sixteen
 * pages at 0x100000 are the A20 WRAP window (x86_mem.c) and nothing owns them
 * as memory when the gate is open; claiming to manage an HMA we do not have
 * would be answered by a guest relocating code into it.
 *
 * LOCK (AH=0Ch): refused with BL=0ADh, "lock failed". A lock's whole purpose is
 * to yield a 32-bit LINEAR address the caller may then dereference. Our bytes
 * are in a file; there is no such address, and inventing one produces a guest
 * that reads garbage instead of a guest that takes the documented failure path.
 */
#ifndef X86_XMS_H
#define X86_XMS_H

#include <stdint.h>

/* Handles. 16 is what HIMEM.SYS defaults to (/NUMHANDLES=), so a guest that
   was written against a stock HIMEM sees a familiar number in AH=0Eh BL. */
#define X86_XMS_HANDLES   16u

/* What src/main.c advertises when the pagefile came up. See the header
   comment: this is a storage and transfer-time decision, not an AXI one. */
#define X86_XMS_DEFAULT_KB  4096u

/*
 * The far-call control entry point, returned by INT 2Fh AX=4310h in ES:BX.
 *
 * It is a 0F 0B stub like every other HLE entry (x86_hle.c), with ONE
 * difference that matters: XMS is entered by CALL FAR and leaves by RETF, so
 * the stub ends 0xCB and NOT 0xCF. There is no FLAGS word on the stack, which
 * is why nothing in this interface reports through CF -- x86_hle_set_cf()
 * would edit the caller's own stack. XMS reports in AX/BL, as the spec says.
 *
 * The offset sits above the vector stubs and below the bare IRET at 0xFC;
 * x86_hle.c static-asserts both ends.
 */
#define X86_XMS_ENTRY_SEG   0x0060u
#define X86_XMS_ENTRY_OFF   0x00C0u
#define X86_HLE_SEL_XMS     0xFBu    /* 0F 0B FB -- not a vector number */

/*
 * Backing store. Byte ranged; see the header comment for why.
 * open(write) is called once per operation and may be NULL; close() likewise.
 * All of them return 0 on success.
 */
typedef int  (*x86_xms_open_fn)(int write);
typedef void (*x86_xms_close_fn)(void);
typedef int  (*x86_xms_rd_fn)(uint32_t off, void *dst, uint32_t bytes);
typedef int  (*x86_xms_wr_fn)(uint32_t off, const void *src, uint32_t bytes);

/* Hand XMS its store and its size in KB. kb == 0, or rd/wr NULL, means there is
   no extended memory and every allocation is refused -- honestly. */
void x86_xms_set_backing(x86_xms_open_fn op, x86_xms_close_fn cl,
                         x86_xms_rd_fn rd, x86_xms_wr_fn wr, uint32_t kb);
/* Drop every handle. Called at guest reset; the store's contents are stale by
   definition at that point and the pagefile is truncated under us anyway. */
void x86_xms_reset(void);

/* INT 2Fh. Answers AX=4300h and AX=4310h and NOTHING else -- every other
   multiplex subfunction is left with the guest's own registers untouched,
   which is what "no such driver" looks like and what the bare IRET did. */
int x86_xms_int2f(void);
/* The far-call control function; reached as 0F 0B FB. */
int x86_xms_control(void);

/* ---- host-readable state ------------------------------------------------
 * Every one of these is a number a probe can name. "XMS is present" is not a
 * measurement; "the guest moved 106,496 bytes up and 106,496 back with zero
 * I/O errors" is.
 */
extern volatile uint32_t x86_xms_total_kb;    /* what AH=08h advertises      */
extern volatile uint32_t x86_xms_used_kb;     /* sum of live handles         */
extern volatile uint32_t x86_xms_handles_used;
extern volatile uint32_t x86_xms_detects;     /* INT 2Fh AX=4300h answered   */
extern volatile uint32_t x86_xms_entry_reqs;  /* INT 2Fh AX=4310h answered   */
extern volatile uint32_t x86_xms_calls;       /* far calls into the control  */
extern volatile uint32_t x86_xms_moves;       /* AH=0Bh, successful          */
extern volatile uint32_t x86_xms_move_up;     /* bytes conventional -> XMS   */
extern volatile uint32_t x86_xms_move_down;   /* bytes XMS -> conventional   */
extern volatile uint32_t x86_xms_move_xx;     /* bytes XMS -> XMS            */
extern volatile uint32_t x86_xms_io_errors;   /* a store read/write refused  */
extern volatile uint32_t x86_xms_guest_errors;/* a guest range we could not
                                                 reach: an unmapped page      */
extern volatile uint32_t x86_xms_last_err;    /* last BL returned, if failing */
extern volatile uint32_t x86_xms_unhandled;   /* last AH we do not implement */

#endif /* X86_XMS_H */
