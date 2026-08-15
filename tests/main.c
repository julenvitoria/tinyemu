#include <stdint.h>
#include <string.h>
#include "../lib/FatFs/ff.h"

#include "../cutils.h"
#include "../x86_cpu.h"
#include "../x86_arena.h"
#include "../x86_video_planar.h"
#include "../x86_pager.h"
#include "../x86_pgfile.h"
#include "../x86_xms.h"
#include "../x86_ems.h"
#include "../x86_profile.h"
#include "../x86_osk.h"
#include "../x86_opl.h"

// Address-translation fold. Entries are OFFSETS (host = guest_linear + k), and
// every index the 12-bit lookup can produce must exist -- a short table meant
// the walk read past its end and a zero entry aliased address 0, which is ITCM.
uint32_t guest_page_table[FOLD_ENTRIES] __attribute__((section(".dtcm_bss")));
/* The pager's per-page state, in DTCM as it is in a real core -- there it is
   handed out by mem_ctl(GW_MEM_DTC), here it is placed. */
uint8_t pager_tables[X86_PG_TABLE_BYTES] __attribute__((section(".dtcm_bss")));
/* Mode records: four dispatch tables plus their headers. Over-allocated by one
   record so x86_build_mode_records can align the base up to 8 KB, which the
   66/67 EORs require. */
static uint8_t mode_record_block[X86_MODE_RECORDS_BYTES + X86_MODE_RECORDS_ALIGN]
    __attribute__((section(".dtcm_bss")));
extern uint32_t *guest_page_table_ptr;   // what the interpreter dereferences
extern X86CPUState cpu_state_dtcm;
extern void x86_cpu_interp_asm(void);
extern void vdisk_init(void *base, uint32_t size);

// We need to map up to 0xB8000 + 16KB (CGA memory)
// 0xB8000 + 16KB = 753664 bytes (736KB + 16KB = 752KB)
uint8_t test_ram[752 * 1024] __attribute__((aligned(4096)));

extern uint32_t _estack; // Typically defined in linker script, or we can just hardcode end of DTCM
#define DTCM_END 0x20020000

void Reset_Handler(void);
void Fault_Handler(void);

// Cortex-M Vector Table.
//
// This used to be two entries (SP + Reset). With no fault handler a bus error
// escalated to HardFault with nowhere to go, the device wedged, and the Python
// harness saw an indistinguishable timeout -- which is exactly how the
// unconfigured-OCTOSPI bug stayed invisible. Faults are now latched and readable.
__attribute__((section(".isr_vector"), used))
uint32_t vector_table[] = {
    DTCM_END,                   //  0 Initial SP
    (uint32_t)&Reset_Handler,   //  1 Reset
    (uint32_t)&Fault_Handler,   //  2 NMI
    (uint32_t)&Fault_Handler,   //  3 HardFault
    (uint32_t)&Fault_Handler,   //  4 MemManage
    (uint32_t)&Fault_Handler,   //  5 BusFault
    (uint32_t)&Fault_Handler,   //  6 UsageFault
    0, 0, 0, 0,                 //  7-10 reserved
    (uint32_t)&Fault_Handler,   // 11 SVCall
    (uint32_t)&Fault_Handler,   // 12 DebugMonitor
    0,                          // 13 reserved
    (uint32_t)&Fault_Handler,   // 14 PendSV
    (uint32_t)&Fault_Handler,   // 15 SysTick
};

volatile uint32_t run_flag = 0;
volatile uint32_t run_benchmark_flag = 0;
volatile uint32_t benchmark_cycles = 0;
volatile uint32_t benchmark_insns = 0;   // measured, not assumed by the host
volatile uint32_t benchmark_filesize = 0;
volatile uint32_t debug_log[256];
volatile uint32_t debug_log_idx = 0;

// Host-triggered. The guest image lives in memory-mapped external flash, which
// requires OCTOSPI setup this bare-metal firmware does not do, so loading it is
// NOT attempted at reset -- on silicon that bus-faults. The host sets this only
// on targets where extflash is known good. Opcode tests and the benchmark need
// no disk and run regardless.
volatile uint32_t load_guest_flag = 0;

/* Video self-test. The host writes a pattern into cga_test_vram, raises the
   flag, and reads cga_test_out back to check the expansion independently. */
/* DOS arena self-test. The host arms it and reads the results back; the MCB
   chain lives in guest memory so the host can also walk it independently. */
volatile uint32_t run_dos_flag = 0;
volatile uint32_t dos_r[32];

/* HLE stub planting. The stubs and the bare-IRET default sit in guest memory;
   the host checks they land BELOW the DOS arena, because anything at or above
   it is handed to a program and overwritten. */
volatile uint32_t run_hle_flag = 0;
volatile int32_t hle_r = -99;

/* Keyboard self-test: the BDA ring and INT 16h, driven from guest code.
   The producer is the SAME function INT 09h dispatches to, so this exercises
   the real path from a scancode on port 0x60 to a key in AX. */
volatile uint32_t run_kbd_flag = 0;
volatile uint32_t kbd_r[20];

/* On-screen keyboard self-test. The OSK is a front end over the SAME injection
   path the buttons use, so what has to be proved is that a grid press turns
   into the right SCANCODES and that the BIOS decodes them into the right BDA
   entry -- shift included, which is the part with no other witness. */
volatile uint32_t run_osk_flag = 0;
volatile uint32_t osk_r[20];

/* DOS file I/O self-test: INT 21h 3Dh/3Fh/42h/3Eh/43h/4Eh/4Fh, driven from
   GUEST code against a FAT12 volume the HOST built in dosfile_img.

   It needs its own disk rather than the shipped .dsk: the expected bytes have
   to be written by the test, not read out of a title's data file. 64 KB is the
   smallest valid FAT12 geometry (see scripts/common/fat12.py). */
#define DOSFILE_IMG_BYTES (128u * 512u)
uint8_t dosfile_img[DOSFILE_IMG_BYTES] __attribute__((aligned(4)));
volatile uint32_t run_dosfile_flag = 0;
volatile uint32_t dosfile_r[40];

/* MZ LOAD self-test: x86_dos_exec() end to end, against an .EXE the host built
   on dosfile_img.

   This is the gap that let a corrupted program load ship. Nothing else in the
   suite runs a real MZ image through the PSP/environment path, so every DOS
   test passed both before and after a change that loaded WOLF3D.EXE as
   garbage. What matters is not that exec RETURNS 0 -- it did, throughout -- it
   is that the IMAGE IS INTACT AT THE ENTRY POINT and that the PSP the program
   is about to read describes the block it was actually given. The host does
   the comparison: it wrote the file, so it knows every byte that must appear
   at load_seg, and it can walk the PSP fields itself rather than trusting a
   checksum computed by the code under test. */
volatile uint32_t run_dosexec_flag = 0;
volatile uint32_t dosexec_r[16];

/* EXEC self-test: INT 21h AH=4Bh, driven by a real parent program that EXECs a
   real child, both built by the host on dosfile_img.

   Nothing may be asserted from the host here except by reading what the GUESTS
   wrote. The load half is already covered above; what this arm exists for is
   the half that cannot be checked without running both programs -- that the
   child gets its own PSP with the parent's segment in it, that the child's
   relocations were applied, and above all THAT THE PARENT COMES BACK. A
   re-entrant exec that loads perfectly and never returns looks identical from
   the host to one that works, right up until the title derails into the IVT.

   The two programs leave their evidence at linear 0x500 -- the DOS scratch
   area, below the arena at 0x700, so no allocation can ever land on it. */
#define DOSEXEC2_MARK  0x0500u
volatile uint32_t run_dosexec2_flag = 0;
volatile uint32_t dosexec2_r[16];

/* OVERLAY self-test: INT 21h AH=4Bh AL=03h.

   The claim an overlay load makes is narrower than EXEC's and so is the test:
   the bytes landed at the segment THE CALLER CHOSE, the relocation table was
   applied with THE FACTOR THE CALLER SUPPLIED (which is not necessarily that
   segment -- that separation is the entire content of AL=03h), and control
   came straight back to the caller with CF clear.

   The host cannot assert any of that from a counter. So a real HOST.EXE picks
   a segment, overlay-loads a real OVL.EXE into it, and then FAR CALLS into the
   loaded image; the overlay computes a sentinel FROM ITS OWN RELOCATED WORD
   and returns it. A sentinel that comes back right proves the image is intact,
   correctly relocated and reachable -- three facts one byte can carry only
   because the overlay had to execute to produce it.

   Evidence goes at linear 0x520, above the EXEC arm's 32 bytes at 0x500 and
   still inside the DOS scratch area below the arena at 0x700. */
#define DOSOVL_MARK  0x0520u
volatile uint32_t run_dosovl_flag = 0;
volatile uint32_t dosovl_r[16];

volatile uint32_t run_nor_flag = 0;
volatile uint32_t nor_r[40];

volatile uint32_t run_norwd_flag = 0;
volatile uint32_t norwd_r[12];

volatile uint32_t run_a20_flag = 0;
volatile uint32_t a20_r[24];

/* IN DTCM, NOT .bss, AND THAT IS NOT A PREFERENCE. The harness's RAM_EMU has
   no slack at all -- 128 bytes of new result array overflowed it by 176 the
   first time this arm was built, the same 176 the pager's own table note
   records. DTCM is where the harness keeps its page table and CPU state and it
   has room; a result block is host-readable wherever it lives. */
volatile uint32_t run_hma_flag __attribute__((section(".dtcm_bss")));
volatile uint32_t hma_r[32] __attribute__((section(".dtcm_bss")));

/* The arena runs against a region of the harness's OWN choosing -- 40 pages,
   so every expected number below is arithmetic this test can state, not
   whatever the LCD bonus pool happened to release.
   It is carved out of the TOP of test_ram rather than declared: 160 KB of new
   .bss does not fit in any region of this harness (RAM_EMU, RAM_UC and AHBRAM
   were each tried and each overflowed), and the arena arm runs no guest code,
   so the guest's memory is idle while it does. test_ram is 4096-aligned, so
   the offset keeps the region page-aligned. */
#define ARENA_TEST_PAGES 40u
#define ARENA_TEST_OFF   (512u * 1024u)
#define arena_test_mem   (test_ram + ARENA_TEST_OFF)
volatile uint32_t run_arena_flag = 0;
volatile uint32_t arena_r[32];

volatile uint32_t run_pager_flag = 0;
volatile uint32_t pager_r[96];

/* The pagefile, as a plain RAM buffer.
 *
 * This is the whole point of x86_pg_read_fn/x86_pg_write_fn living in
 * x86_pager.h rather than the pager knowing what an SD card is: eviction and
 * page-in are exercised end to end at tier 1 with no card, no FatFs and no
 * firmware. Carved out of the TOP of test_ram for the same reason the arena
 * region is -- 32 KB of new .bss does not fit in any region of this harness.
 * 8 slots, one per page of the pager window. */
#define PGF_OFF    (720u * 1024u)
#define PGF_SLOTS  8u
static uint32_t pgf_first_slot;         /* slot 0 of the buffer, in pages */
static uint32_t pgf_writes, pgf_reads, pgf_fail;
static int pgf_read(uint32_t slot, void *dst)
{
    if (slot - pgf_first_slot >= PGF_SLOTS) { pgf_fail++; return -1; }
    memcpy(dst, test_ram + PGF_OFF + (slot - pgf_first_slot) * 4096u, 4096u);
    pgf_reads++;
    return 0;
}
static int pgf_write(uint32_t slot, const void *src)
{
    if (slot - pgf_first_slot >= PGF_SLOTS) { pgf_fail++; return -1; }
    memcpy(test_ram + PGF_OFF + (slot - pgf_first_slot) * 4096u, src, 4096u);
    pgf_writes++;
    return 0;
}


/* ===========================================================================
 * THE PAGEFILE SLOT RING -- x86_pgfile.c, driven against a RAM file.
 *
 * The allocator that decides WHERE an evicted page's bytes land is the one
 * part of the writeback path that used to live in src/main.c, where nothing
 * below tier 3 can reach it. It is here now, and this arm drives it the way
 * the pager does: claim a slot, write, commit -- and then read the page back
 * and compare, which is the only check that matters.
 *
 * The store is a plain array of slots, so a slot number that is wrong shows up
 * as WRONG BYTES rather than as a counter. THE TWO NEGATIVE CONTROLS ARE THE
 * POINT: one corrupts the bytes under the mapped slot, the other reads the
 * page from the slot it occupied BEFORE its last write -- exactly what a
 * read that overtook a pending write would see -- and both must be caught by
 * the same comparison that passes above them. A test that cannot fail is not
 * evidence.
 */
volatile uint32_t run_pgfile_flag = 0;
/* .test_bss (RAM_UC): this harness's RAM_EMU .bss has no room -- see TEST.ld. */
volatile uint32_t pgfile_r[24] __attribute__((section(".test_bss")));

#define PGR_SLOTS   X86_PGF_RING_SLOTS
#define PGR_OWN     8u
#define PGR_AUXF    64u
#define PGR_AUXN    4u

/* One byte per slot is enough to tell copies apart: every page writes a
   distinct pattern and the comparison is over that byte. It lives in the same
   corner of test_ram the pager arm's RAM pagefile uses -- .bss in this harness
   is 176 bytes from overflowing and a new array is not free. */
#define pgr_store  ((uint8_t *)(test_ram + PGF_OFF))
static uint32_t pgr_write_fail;         /* forces the next write to fail */

/* The writeback, exactly as src/main.c sequences it. */
static int pgr_write(uint32_t page, uint8_t val)
{
    uint32_t slot = x86_pgf_slot_claim(page);
    if (slot == X86_PGF_NO_SLOT)
        return -1;
    int ok = !pgr_write_fail;
    if (ok)
        pgr_store[slot] = val;          /* the 4 KB, in one byte */
    x86_pgf_commit(page, slot, ok);
    return ok ? (int)slot : -1;
}

/* The refill. -1 for a page with no copy at all. */
static int pgr_read(uint32_t page)
{
    uint32_t slot = x86_pgf_slot_of(page);
    if (slot == X86_PGF_NO_SLOT)
        return -1;
    return pgr_store[slot];
}

static void pgfile_test_run(void)
{
    for (uint32_t i = 0; i < 24; i++)
        pgfile_r[i] = 0;
    memset(pgr_store, 0, PGR_SLOTS);
    pgr_write_fail = 0;
    x86_pgf_map_init(PGR_OWN, PGR_AUXF, PGR_AUXN);

    /* ---- a page written, evicted and read back is byte-identical -------- */
    int s0 = pgr_write(3u, 0xA5);
    pgfile_r[0] = (s0 >= 0);                                  /* 1 */
    pgfile_r[1] = (pgr_read(3u) == 0xA5);                     /* 1 */

    /* ---- a REWRITE APPENDS: a different slot, and the old one freed ----- */
    int s1 = pgr_write(3u, 0x5A);
    pgfile_r[2] = (s1 >= 0 && s1 != s0);                      /* 1 */
    pgfile_r[3] = (x86_pgf_slot_live((uint32_t)s0) == 0);     /* 1 */
    pgfile_r[4] = (x86_pgf_slot_live((uint32_t)s1) == 1);     /* 1 */
    pgfile_r[5] = (pgr_read(3u) == 0x5A);                     /* 1 */

    /* NEGATIVE CONTROL 1 -- the bytes under the mapped slot are corrupted.
       If this does not read 1, the comparison above proves nothing. */
    pgr_store[s1] ^= 0xFFu;
    pgfile_r[6] = (pgr_read(3u) != 0x5A);                     /* 1 */
    pgr_store[s1] ^= 0xFFu;
    pgfile_r[7] = (pgr_read(3u) == 0x5A);                     /* 1: restored */

    /* NEGATIVE CONTROL 2 -- READ THE OLD COPY. This is what a refill that
       overtook a pending writeback would get, and it must NOT be what the map
       hands back. The old slot still holds the previous generation. */
    pgfile_r[8] = (pgr_store[s0] == 0xA5);                    /* 1 */
    pgfile_r[9] = (pgr_store[s0] != pgr_store[s1]);           /* 1: caught */

    /* ---- a FAILED write leaves the page on the copy it already had ------ */
    pgr_write_fail = 1;
    int sf = pgr_write(3u, 0x11);
    pgr_write_fail = 0;
    pgfile_r[10] = (sf < 0);                                  /* 1 */
    pgfile_r[11] = (x86_pgf_slot_of(3u) == (uint32_t)s1);     /* 1: unmoved */
    pgfile_r[12] = (pgr_read(3u) == 0x5A);                    /* 1: intact  */

    /* ---- a live slot is NEVER handed out, all the way round the ring ---- */
    /* Every pageable page gets a copy; then every page is rewritten four
       times, which is comfortably more claims than the ring has slots, so the
       cursor wraps and has to step over the live ones. */
    uint32_t collisions = 0, live_pages = 0;
    for (uint32_t pg = 0; pg < PGR_OWN; pg++)
        if (pgr_write(pg, (uint8_t)(0x40u + pg)) >= 0) live_pages++;
    for (uint32_t pg = 0; pg < PGR_AUXN; pg++)
        if (pgr_write(PGR_AUXF + pg, (uint8_t)(0x80u + pg)) >= 0) live_pages++;
    pgfile_r[13] = live_pages;                                /* 12 */

    for (uint32_t round = 0; round < 4u; round++) {
        for (uint32_t pg = 0; pg < PGR_OWN; pg++) {
            uint32_t before = x86_pgf_slot_of(pg);
            uint32_t claim = x86_pgf_slot_claim(pg);
            /* The claim may only be a slot no OTHER page owns. */
            for (uint32_t q = 0; q < PGR_OWN; q++)
                if (q != pg && x86_pgf_slot_of(q) == claim)
                    collisions++;
            for (uint32_t q = 0; q < PGR_AUXN; q++)
                if (x86_pgf_slot_of(PGR_AUXF + q) == claim)
                    collisions++;
            (void)before;
            pgr_store[claim] = (uint8_t)(0x40u + pg);
            x86_pgf_commit(pg, claim, 1);
        }
    }
    pgfile_r[14] = collisions;                                /* 0 */
    pgfile_r[15] = x86_pgf_slot_full;                         /* 0 */

    /* Every page still reads back its own pattern after all that churn. */
    uint32_t wrong = 0;
    for (uint32_t pg = 0; pg < PGR_OWN; pg++)
        if (pgr_read(pg) != (int)(0x40u + pg)) wrong++;
    for (uint32_t pg = 0; pg < PGR_AUXN; pg++)
        if (pgr_read(PGR_AUXF + pg) != (int)(0x80u + pg)) wrong++;
    pgfile_r[16] = wrong;                                     /* 0 */

    /* ---- the ring DID wrap, or the arm above proved nothing ------------- */
    pgfile_r[17] = (x86_pgf_slot_reuses > 0u);                /* 1 */

    /* ---- a page outside both windows is refused, loudly ----------------- */
    uint32_t before_unp = x86_pgf_unpageable;
    pgfile_r[18] = (x86_pgf_slot_claim(PGR_OWN + 1u) == X86_PGF_NO_SLOT);  /* 1 */
    pgfile_r[19] = (x86_pgf_unpageable == before_unp + 1u);   /* 1 */
    pgfile_r[20] = (x86_pgf_slot_of(PGR_OWN + 1u) == X86_PGF_NO_SLOT); /* 1 */

    /* ---- reset forgets every copy -------------------------------------- */
    x86_pgf_map_reset();
    pgfile_r[21] = (x86_pgf_slot_of(3u) == X86_PGF_NO_SLOT);  /* 1 */
    pgfile_r[22] = (pgr_read(3u) == -1);                      /* 1 */
    pgfile_r[23] = x86_pgf_slot_live(0u) ? 0u : 1u;           /* 1 */
}

volatile uint32_t run_seam_flag = 0;
volatile uint32_t seam_r[24];
/* Which fold the seam arm installs: 0 = NON-CONTIGUOUS (the defect), 1 = the
   two granules laid out back to back. The second is the NEGATIVE CONTROL --
   the same guest program, the same expectations, a fold in which no seam can
   exist. If it ever fails, the arm is wrong about something other than the
   seam. Driven from the host so one build answers both. */
volatile uint32_t seam_contiguous = 0;
/* THE CODE-STREAM SEAM. Which straddle the arm places at the granule boundary:
   0 = the displacement, 1 = the immediate, 2 = the opcode itself (so the ModRM
   and its displacement are both on the far side). Driven from the host so one
   build answers all three -- see code_seam_test_run(). */
volatile uint32_t code_seam_case = 0;
volatile uint32_t run_code_seam_flag = 0;
volatile uint32_t code_seam_r[16];

volatile uint32_t run_video_flag = 0;
volatile uint32_t video_blit_cycles = 0;
uint8_t cga_test_vram[X86_VRAM_BYTES] __attribute__((section(".test_bss")));
uint8_t cga_test_out[320 * 240] __attribute__((section(".test_bss")));
/* 32-bit ARGB entries, matching lcd_set_clut(). */
uint32_t cga_test_palette[32] __attribute__((section(".test_bss")));
                                /* X86_TEXT_CLUT_ENTRIES: 32 is what the
                                    firmware needs for its darkened twins to
                                    line up with LCD_DARKEN_BIT, and what the
                                    anti-aliased text path allocates from */
/* Text-mode arms. run_video_flag 4 selects a mode (so the host can put the
   renderer into mode 3 and get the CLUT reloaded), 5 runs a scroll. The cell
   pattern itself is written straight into cga_test_vram, which IS the text
   buffer -- that is the path a real DOS program takes, poking B800:0 rather
   than going through INT 10h. */
/* VGA mode 13h. The aperture and the host palette block come from the caller
   in the real core (the LCD bonus pool); here they are just two buffers, which
   is the whole point of taking them as arguments rather than as .bss.
   vga_dac_arg is streamed through the DAC PORTS by arm 6, not memcpy'd into
   the DAC -- 0x3C9 is a stream and a test that bypassed it would not see the
   failure that models it as a latch. */
uint8_t  vga_test_aperture[X86_VGA_APERTURE_BYTES] __attribute__((section(".ahb_bss")));
uint8_t  vga_test_pal[X86_VGA_PALETTE_BYTES] __attribute__((section(".ahb_bss")));
uint8_t  vga_dac_arg[768] __attribute__((section(".ahb_bss")));
uint32_t vga_clut_out[256] __attribute__((section(".ahb_bss")));
volatile uint32_t vga_dac_readback[6];
/* Filler bytes arm 9 pushes into 0x3C9 AFTER indexing and BEFORE the palette.
   1 is a deliberate one-byte PHASE error -- the negative control, because a
   palette check that cannot fail is not a check. 768 is Wolf3D's real shape:
   it streams TWO full palettes, and the second is indexed only by the write
   cursor having wrapped, so a wrap that does not land on a multiple of 3
   rotates every channel of the second palette. */
volatile uint32_t vga_dac_prewrite = 0;
volatile uint32_t vga_regs_out[9];   /* CRTC/Seq/GC/Attr write-then-read-back */   /* first two entries, read back via 0x3C7/0x3C9 */

volatile uint32_t text_mode_arg = 0x03;
volatile uint32_t text_scroll_arg = 0;      /* (attr << 8) | lines */

/* ---------------------------------------------------------------------------
 * PLANAR VGA -- unchained 13h ("Mode Y") and EGA 0Dh.
 *
 * These arms run GUEST CODE, and that is the whole point. The planar pipeline
 * hangs off the store hook in cpu_exec.S and off word `OUT dx,ax` writes to the
 * Sequencer and the Graphics Controller; a host-side test that called
 * x86_vga_planar_store() directly would exercise neither, and the reference
 * port's record is exactly that -- a 64,000-pixel test that drove the adapter
 * the way a TEST does passed unchanged through all five bugs that made a real
 * game render garbage (11-ega-planar.md, "Why the first test passed").
 *
 * So the host writes a program into guest memory and this runs it. The program
 * itself lives in tests/test_video.py, where it can carry its own commentary
 * next to the expectation it is supposed to satisfy.
 */
#define PLANAR_CODE_BASE 0x3000u
volatile uint32_t run_planar_flag = 0;
volatile uint32_t planar_r[20];

/*
 * MODE Y PRESENTATION POLICY -- which frames the loop paints.
 *
 * This is not a pixel question and no pixel test can ask it: at 64000/64000
 * exact, WHICH FRAMES the loop paints is still free. The policy is a PORT of
 * references/dos_port_prior/dos_video.c:1529-1656 -- present on the guest's
 * page flip, hold for at most 8 frames, then paint every frame -- so these arms
 * grade that and nothing more.
 *
 * The harness drives x86_video_modey_paint_due() directly, one call per
 * modelled frame, against a modelled guest: a flip is x86_video_modey_presented
 * (1) and "the guest drew this frame" is an increment of x86_vga_planar_stores.
 * No LCD, no CPU, no timing -- five scenarios and a count of paints.
 */
volatile uint32_t run_present_flag = 0;
volatile uint32_t present_r[8];

// Latched fault state, for the host to read over the debug backend.
volatile uint32_t fault_active = 0;   // 0 = none, else VECTACTIVE
volatile uint32_t fault_cfsr = 0;
volatile uint32_t fault_hfsr = 0;
volatile uint32_t fault_bfar = 0;
volatile uint32_t fault_pc = 0;
volatile uint32_t fault_lr = 0;

#define SCB_ICSR (*(volatile uint32_t *)0xE000ED04)
#define SCB_CFSR (*(volatile uint32_t *)0xE000ED28)
#define SCB_HFSR (*(volatile uint32_t *)0xE000ED2C)
#define SCB_BFAR (*(volatile uint32_t *)0xE000ED38)

__attribute__((naked, used)) void Fault_Handler(void)
{
    // Recover the stacked frame pointer (EXC_RETURN bit 2 selects MSP/PSP),
    // then hand it to C so the faulting PC/LR can be latched.
    __asm__ volatile(
        "tst  lr, #4            \n"
        "ite  eq                \n"
        "mrseq r0, msp          \n"
        "mrsne r0, psp          \n"
        "b    Fault_Handler_C   \n");
}

__attribute__((used)) void Fault_Handler_C(uint32_t *frame)
{
    fault_cfsr = SCB_CFSR;
    fault_hfsr = SCB_HFSR;
    fault_bfar = SCB_BFAR;
    fault_lr = frame[5];
    fault_pc = frame[6];
    fault_active = SCB_ICSR & 0x1FF;

    // Release any pending host wait so the harness reports a fault rather than
    // timing out with no information.
    run_flag = 0;
    run_benchmark_flag = 0;
    while (1) {
    }
}

#define DWT_CTRL (*(volatile uint32_t *)0xE0001000)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004)
#define CoreDebug_DEMCR (*(volatile uint32_t *)0xE000EDFC)

typedef struct {
    uint16_t signature;
    uint16_t bytes_in_last_block;
    uint16_t blocks_in_file;
    uint16_t num_relocs;
    uint16_t header_paragraphs;
    uint16_t min_extra_paragraphs;
    uint16_t max_extra_paragraphs;
    uint16_t ss;
    uint16_t sp;
    uint16_t checksum;
    uint16_t ip;
    uint16_t cs;
    uint16_t reloc_table_offset;
    uint16_t overlay_number;
} MZHeader;

// Load MZ executable into test_ram (which we assume starts at linear address 0)
// We will place the PSP at segment 0x0100 (linear 0x1000)
// The code will be loaded at segment 0x0110 (linear 0x1100)
int load_mz_executable(FIL *file) {
    MZHeader mz;
    UINT br;
    if (f_read(file, &mz, sizeof(MZHeader), &br) != FR_OK) return -1;
    if (mz.signature != 0x5A4D && mz.signature != 0x4D5A) return -1; // "MZ" or "ZM"
    
    uint32_t header_size = mz.header_paragraphs * 16;
    uint32_t load_size = mz.blocks_in_file * 512;
    if (mz.bytes_in_last_block) {
        load_size -= (512 - mz.bytes_in_last_block);
    }
    load_size -= header_size;
    
    uint16_t psp_segment = 0x0100;
    uint16_t load_segment = psp_segment + 0x10;
    uint32_t load_linear = load_segment * 16;
    
    if (load_linear + load_size > sizeof(test_ram)) {
        return -2; // File too large for our test_ram (16KB? Wait, test_ram is 16KB! ALLEYCAT is 55KB!)
    }
    
    // Seek to start of code
    f_lseek(file, header_size);
    f_read(file, &test_ram[load_linear], load_size, &br);
    
    // Read and apply relocations
    f_lseek(file, mz.reloc_table_offset);
    for (int i = 0; i < mz.num_relocs; i++) {
        uint16_t reloc[2]; // [offset, segment]
        if (f_read(file, reloc, 4, &br) != FR_OK) break;
        uint32_t reloc_addr = load_linear + (reloc[1] * 16) + reloc[0];
        uint16_t *target = (uint16_t *)&test_ram[reloc_addr];
        *target += load_segment;
    }
    
    // Initialize CPU state
    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    
    // Set segments (ES=0, CS=1, SS=2, DS=3, FS=4, GS=5)
    cpu_state_dtcm.segs[1].sel = mz.cs + load_segment;
    cpu_state_dtcm.segs[1].base = (mz.cs + load_segment) * 16;
    
    cpu_state_dtcm.segs[2].sel = mz.ss + load_segment;
    cpu_state_dtcm.segs[2].base = (mz.ss + load_segment) * 16;
    
    cpu_state_dtcm.segs[3].sel = psp_segment;
    cpu_state_dtcm.segs[3].base = psp_segment * 16;
    cpu_state_dtcm.segs[0].sel = psp_segment;
    cpu_state_dtcm.segs[0].base = psp_segment * 16;
    
    cpu_state_dtcm.eip = mz.ip;
    cpu_state_dtcm.regs[4] = mz.sp; // ESP=4
    
    // Set up PSP (CD 20 at offset 0)
    test_ram[psp_segment * 16] = 0xCD;
    test_ram[psp_segment * 16 + 1] = 0x20;
    
    return 0;
}

/* --------------------------------------------------------------------------
 * Keyboard / INT 16h self-test.
 *
 * A scancode is queued on the controller and handed to x86_bios_kbd_irq() --
 * the exact function INT 09h dispatches to -- so the producer side is the
 * shipping one, not a re-implementation. The consumer side is GUEST code:
 * INT 16h AH=01 (peek, ZF reports empty) and AH=00 (blocking read), with the
 * results and the FLAGS the IRET actually delivered stored into guest memory
 * where the host can read them.
 */
#define KBD_CODE_BASE   0x2000u
#define KBD_RES_BASE    0x4000u

static const uint8_t kbd_guest_code[] = {
    /* --- phase A at 0x2000: peek an EMPTY buffer ------------------------ */
    0xB8, 0x00, 0x00,             /* MOV AX,0000                            */
    0xB4, 0x01,                   /* MOV AH,01                              */
    0xCD, 0x16,                   /* INT 16h                                */
    0xA3, 0x00, 0x40,             /* MOV [4000],AX  r0: AX on an empty peek */
    0x9C,                         /* PUSHF                                  */
    0x58,                         /* POP AX                                 */
    0xA3, 0x02, 0x40,             /* MOV [4002],AX  r1: FLAGS, ZF must be 1 */
    0xF4,                         /* HLT                                    */
};

/* Phase B is placed at a fixed offset so the host and the harness agree. */
#define KBD_PHASE_B     0x2020u
static const uint8_t kbd_guest_code_b[] = {
    0xB4, 0x01,                   /* MOV AH,01   peek with a key waiting    */
    0xCD, 0x16,                   /* INT 16h                                */
    0xA3, 0x04, 0x40,             /* MOV [4004],AX  r2: the key             */
    0x9C, 0x58,                   /* PUSHF ; POP AX                         */
    0xA3, 0x06, 0x40,             /* MOV [4006],AX  r3: FLAGS, ZF must be 0 */
    0xB4, 0x01,                   /* MOV AH,01   peek again                 */
    0xCD, 0x16,                   /* INT 16h                                */
    0xA3, 0x08, 0x40,             /* MOV [4008],AX  r4: non-destructive     */
    0xB8, 0x00, 0x00,             /* MOV AX,0000                            */
    0xCD, 0x16,                   /* INT 16h AH=00: dequeue                 */
    0xA3, 0x0A, 0x40,             /* MOV [400A],AX  r5: the key             */
    0xB4, 0x01,                   /* MOV AH,01   drained now                */
    0xCD, 0x16,                   /* INT 16h                                */
    0x9C, 0x58,                   /* PUSHF ; POP AX                         */
    0xA3, 0x0C, 0x40,             /* MOV [400C],AX  r6: FLAGS, ZF must be 1 */
    0xF4,                         /* HLT                                    */
};

/* Phase C: the same read, used after the ring has been walked to its end so
   the push and the pop both have to wrap. */
#define KBD_PHASE_C     0x2080u
static const uint8_t kbd_guest_code_c[] = {
    0xB8, 0x00, 0x00,             /* MOV AX,0000                            */
    0xCD, 0x16,                   /* INT 16h AH=00                          */
    0xA3, 0x0E, 0x40,             /* MOV [400E],AX  r7: the wrapped key     */
    0xF4,                         /* HLT                                    */
};

static void kbd_test_reset_cpu(uint32_t eip)
{
    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    for (int i = 0; i < 6; i++)
        cpu_state_dtcm.segs[i].limit = 0xFFFF;   /* real mode, CS.D clear */
    cpu_state_dtcm.eip = eip;
    cpu_state_dtcm.regs[4] = 0x1F00;             /* ESP */
    cpu_state_dtcm.eflags = 0x0002;
    cpu_state_dtcm.idtr_base = 0;
    cpu_state_dtcm.idtr_limit = 0x3FF;
    cpu_state_dtcm.hle_enabled = 1;
    cpu_state_dtcm.pit_counter = 20000;
    cpu_state_dtcm.pit_reload = 20000;
}

/* Run until the guest HLTs, servicing HLE exits the way the core's frame loop
   does. Returns the exit reason it stopped on. */
static uint32_t kbd_test_run_to_halt(void)
{
    /* Bounded: a guest that never halts must FAIL the test, not hang the
       harness. 200 x 20,000 instructions is far more than any phase needs. */
    for (int guard = 0; guard < 200; guard++) {
        x86_cpu_interp_asm();
        if (cpu_state_dtcm.exit_reason == X86_EXIT_HLE_INT) {
            /* CLEARED FIRST, exactly as the shipping frame loop does it
               (src/main.c). A service is allowed to set exit_reason itself --
               INT 21h AH=4Ch does, that is how a program stops the machine --
               and clearing it AFTERWARDS threw that away. The guest then ran
               straight past its own terminate into whatever followed it. No
               arm noticed because every other guest here ends in HLT; the EXEC
               arm ends in AH=4Ch, and saw its parent run a second time. */
            cpu_state_dtcm.exit_reason = 0;
            x86_hle_service();
            if (cpu_state_dtcm.exit_reason != 0)
                return cpu_state_dtcm.exit_reason;
            continue;
        }
        if (cpu_state_dtcm.exit_reason == X86_EXIT_TIMER) {
            cpu_state_dtcm.exit_reason = 0;
            cpu_state_dtcm.pit_counter = 20000;
            continue;
        }
        return cpu_state_dtcm.exit_reason;
    }
    return 0xFFFFFFFFu;
}

static uint16_t kbd_bda_r16(uint32_t off)
{
    return (uint16_t)(test_ram[0x400 + off] | (test_ram[0x401 + off] << 8));
}
static void kbd_bda_w16(uint32_t off, uint16_t v)
{
    test_ram[0x400 + off] = (uint8_t)v;
    test_ram[0x401 + off] = (uint8_t)(v >> 8);
}

/* ---------------------------------------------------------------------------
 * Mode switching, driven by GUEST code through INT 10h.
 *
 * The other video arms call x86_video_set_mode() from the host, which proves
 * the renderer but says nothing about the path a real program takes. Alley
 * Cat's defect lived on that path: the mode was set through the BIOS, followed
 * correctly, and the CHARACTERS printed afterwards were still stored as
 * char/attr pairs into pixel memory. So this arm executes the guest's own
 *
 *      MOV AX,000m / INT 10h        (set mode)
 *      MOV AH,0E / MOV AL,ch / INT 10h   (teletype one character)
 *
 * and the host then renders and compares. Only this can catch a regression in
 * which the mode is adopted and the character path is not told.
 */
#define MSW_CODE_BASE   0x2600u
#define MSW_TTY_BASE    0x2620u

volatile uint32_t run_modeswitch_flag = 0;
volatile uint32_t msw_mode_arg = 3;         /* AL for the mode set   */
volatile uint32_t msw_char_arg = 'A';       /* AL for the teletype   */
volatile uint32_t msw_r[6];

/* 0: mode after   1: is_text   2: text cols   3: BDA 0x449   4: BDA 0x44A
   5: guest exit reason */
static void msw_snapshot(void)
{
    msw_r[0] = x86_video_mode();
    msw_r[1] = (uint32_t)x86_video_is_text();
    msw_r[2] = x86_video_text_cols();
    msw_r[3] = test_ram[0x449];
    msw_r[4] = (uint32_t)(test_ram[0x44A] | (test_ram[0x44B] << 8));
}

static void modeswitch_test_init(void)
{
    static const uint8_t hle_vectors[] = { 0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21 };
    static const uint8_t set_mode_code[] = {
        0xB8, 0x03, 0x00,             /* MOV AX,0003 (AL patched by the host) */
        0xCD, 0x10,                   /* INT 10h                              */
        0xF4,                         /* HLT                                  */
    };
    static const uint8_t tty_code[] = {
        0xB4, 0x0E,                   /* MOV AH,0E                            */
        0xB0, 0x41,                   /* MOV AL,'A'  (patched by the host)    */
        0xB7, 0x00,                   /* MOV BH,0                             */
        0xB3, 0x03,                   /* MOV BL,3    (graphics fg colour)     */
        0xCD, 0x10,                   /* INT 10h                              */
        0xF4,                         /* HLT                                  */
    };

    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));
    memcpy(&test_ram[MSW_CODE_BASE], set_mode_code, sizeof(set_mode_code));
    memcpy(&test_ram[MSW_TTY_BASE], tty_code, sizeof(tty_code));
    msw_snapshot();
}

static void modeswitch_test_set(void)
{
    test_ram[MSW_CODE_BASE + 1] = (uint8_t)msw_mode_arg;
    kbd_test_reset_cpu(MSW_CODE_BASE);
    msw_r[5] = kbd_test_run_to_halt();
    msw_snapshot();
}

/*
 * Bring the machine up the way the real core does for a planar mode.
 *
 * THE FOLD HAS TO BE REMAPPED, and forgetting it is the failure this comment
 * exists to prevent. test_ram is identity-mapped from guest 0, so guest
 * 0xA0000 lands in test_ram by default -- NOT in vga_test_aperture, which is
 * what x86_video_set_aperture() gave the store hook. The hook compares the
 * resolved host pointer against cpu->vga_base, so with the default mapping it
 * never fires and every planar store lands raw in plain RAM: a screen of
 * nothing, from a test that ran to completion. x86_video_map() is the same call
 * the core makes and it is what makes the two agree.
 *
 * THE ARENA HAS TO EXIST, for the same class of reason. Mode Y's off-screen
 * cache at 0xC300 is an arena claim; with no arena the claim is refused, the
 * cache-staged rows are discarded, and 25 rows come out wrong for a reason that
 * has nothing to do with the code under test.
 */
static void planar_test_init(void)
{
    static const uint8_t hle_vectors[] = { 0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21 };

    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_video_map();
    x86_arena_reset();
    /* EXACTLY 8 PAGES, and the number is the point. The EGA card is sized from
       the arena now (x86_video.c, planar_storage_follow), so an arena of
       whatever size would give this arm whatever card -- and the arm's whole
       lower band, plus its read-back at window offset 0x5000, asserts that
       stores PAST the fitted card are discarded. 32,768 bytes is one block of
       4 x 8,192 offsets, the classic card these expectations are written
       against, and it is also exactly Mode Y's off-screen cache. The
       enlargement itself is tested by the egafit arm, against the layout the
       real core claims. */
    x86_arena_add_region(arena_test_mem, 8u * 4096u);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));
    for (unsigned i = 0; i < 20; i++)
        planar_r[i] = 0;
    /* Reset the write-class mix with the arm, not with the process: the
       coverage assertion is about the stores THIS arm makes, and an earlier
       arm's leftovers would make it pass without entering anything. */
    x86_vga_planar_cls_plane = 0;
    x86_vga_planar_cls_copy = 0;
    x86_vga_planar_cls_general = 0;
}

static void planar_test_run(void)
{
    kbd_test_reset_cpu(PLANAR_CODE_BASE);
    planar_r[7] = kbd_test_run_to_halt();
    planar_r[0] = x86_video_mode();
    planar_r[1] = test_ram[0x449];
    planar_r[2] = (uint32_t)x86_vga_is_unchained();
    planar_r[3] = x86_vga_modey_page;
    planar_r[4] = x86_vga_planar_stores;
    planar_r[5] = x86_vga_planar_lost;
    planar_r[6] = x86_vga_modey_flips;
    planar_r[8] = x86_vga_latch_loads;
    planar_r[9] = x86_vga_latch_miss;
    planar_r[10] = x86_vga_ega_armed;
    planar_r[11] = x86_arena_free;
    /* The off-screen cache, in the only two terms that matter: how far up the
       window it reaches, and whether anything was thrown away past it. A
       discard COUNT alone cannot say how much bigger the cache needs to be --
       _hi and _lo can. */
    planar_r[12] = x86_vga_modey_cache_offs;
    planar_r[13] = x86_vga_modey_lost_cache;
    planar_r[14] = x86_vga_modey_lost_hi;
    planar_r[15] = x86_vga_modey_lost_lo;
    /* Write-class mix. The host asserts all three are non-zero: that is what
       makes "64000/64000 exact" a statement about the two FAST paths and not
       only about the general one. */
    planar_r[16] = x86_vga_planar_cls_plane;
    planar_r[17] = x86_vga_planar_cls_copy;
    planar_r[18] = x86_vga_planar_cls_general;
}

/*
 * Model one frame-loop iteration: the guest ran a slice (storing `stores`
 * bytes into the aperture and flipping or not), then the loop asked the policy.
 * Returns 1 if the loop would have painted.
 */
static int present_frame(unsigned stores, int flipped)
{
    int paint;

    x86_vga_planar_stores += stores;
    if (flipped)
        x86_video_modey_presented(1);   /* the device also paints both buffers */
    paint = x86_video_modey_paint_due();
    if (paint)
        x86_video_modey_presented(0);   /* exactly what src/main.c does */
    return paint;
}

/*
 * THE ARMS, AND WHAT EACH ONE PINS.
 *
 * The policy under test is the reference's (dos_video.c:1529-1656), so the
 * arms grade the reference's behaviour and not an invention's: present on the
 * guest's flip, hold for at most X86_MODEY_IDLE_FRAMES frames, then paint every
 * frame. There is no inference about whether the guest "has finished drawing"
 * -- that inference is what four rounds of local policy got wrong -- so no arm
 * asserts one.
 *
 * The previous suite's arms (a 300-frame flip gap, a bursty raycaster, a
 * six-screen intro ratchet) were models of the INVENTED rules' failure modes
 * and went with them. What survives is the property the hold actually has:
 * nothing is painted in the 8 frames after a flip, and everything after that.
 */
#define PRESENT_HOLD 8u     /* must equal X86_MODEY_IDLE_FRAMES */

static void present_test_run(uint32_t which)
{
    unsigned i, paints = 0;

    for (i = 0; i < 8; i++)
        present_r[i] = 0;
    /* The witnesses are cumulative on the device and must not be cumulative
       ACROSS scenarios here, or each arm reads the one before it. */
    x86_modey_fallbacks = 0;
    x86_modey_flip_gap = 0;
    x86_modey_flip_gap_max = 0;
    x86_modey_mid_draw = 0;
    x86_modey_mid_draw_max = 0;
    x86_vga_modey_arm(0);
    x86_vga_modey_arm(1);
    present_r[7] = x86_vga_modey_armed;

    if (which == 1) {
        /* GAMEPLAY, flipping every 20 frames with stores in every one. The
           guest is slower than the hold, so the reference tears: the 8 frames
           after each flip are held and the remaining 11 are painted. The HOLD
           is what is graded -- a paint inside it is a page the guest has only
           just started redrawing -- plus the tearing itself, as a number. */
        unsigned in_hold = 0;
        for (i = 0; i < 200; i++) {
            int flipped = (i % 20u) == 19u;
            int paint = present_frame(1000u, flipped);
            paints += (unsigned)paint;
            /* The 8 frames after a flip are i%20 == 0..7, once the first flip
               has happened at i == 19. */
            if (i > 19u && (i % 20u) < PRESENT_HOLD && paint)
                in_hold++;
        }
        present_r[0] = paints;
        present_r[1] = in_hold;                 /* must be 0                 */
        present_r[2] = x86_modey_fallbacks;     /* == paints                 */
        present_r[3] = x86_modey_flip_gap_max;  /* 20                        */
        present_r[4] = x86_modey_mid_draw;      /* the tearing, counted      */
    } else if (which == 4) {
        /* A TITLE FLIPPING FASTER THAN THE HOLD -- the case the hold exists
           for, and the one the FLIP negative control must fail. 5 frames per
           flip against a hold of 8, so the loop must paint EXACTLY NEVER and
           every frame on the panel is one the guest called complete. */
        /* One cycle of warm-up: before the FIRST flip the hold has never been
           armed and the loop correctly paints, which is the anti-freeze
           property, not a defect. Graded on the flipping steady state. */
        for (i = 0; i < 5; i++)
            (void)present_frame(1000u, (i % 5u) == 4u);
        x86_modey_fallbacks = 0;
        x86_modey_mid_draw = 0;
        for (i = 0; i < 200; i++)
            paints += (unsigned)present_frame(1000u, (i % 5u) == 4u);
        present_r[0] = paints;                  /* must be 0                 */
        present_r[1] = x86_modey_mid_draw;      /* must be 0                 */
        present_r[2] = x86_modey_flip_gap_max;  /* 5                         */
    } else if (which == 6) {
        /* PROMPTNESS. WOLF3D's intro and menus DO NOT FLIP -- flips=2 across
           the whole signon/title phase against 94 by the end of a level -- so
           they are drawn straight into the page being displayed and the hatch
           is their only presentation path. Its latency is their frame time,
           and a policy that refuses to paint them is the reported "press a
           button to skip and it freezes, then jumps to where you tried to get
           to". A level is played first, so the menu inherits whatever the level
           left behind: under the ported policy that is nothing but the hold,
           which is precisely the property being pinned. */
        for (i = 0; i < 100; i++)
            (void)present_frame(1000u, (i % 20u) == 19u);
        /* The guest stops flipping and keeps drawing in pieces. */
        present_r[0] = 0xFFFFFFFFu;
        for (i = 0; i < 60; i++)
            if (present_frame((i % 4u) < 2u ? 500u : 0u, 0)) {
                paints++;
                if (present_r[0] == 0xFFFFFFFFu)
                    present_r[0] = i + 1u;      /* frames to the first reveal */
            }
        present_r[1] = paints;                  /* it keeps being revealed    */
    } else if (which == 2) {
        /* A MENU: four frames of drawing, then quiet, and it never flips. The
           reference reveals it AS IT DRAWS (modey_idle starts at the limit, so
           the very first frame paints) and goes on repainting while it is
           quiet. Both halves matter: the first is the anti-freeze property,
           the second is that no state accumulates to suppress a later screen. */
        for (i = 0; i < 4; i++)
            paints += (unsigned)present_frame(500u, 0);
        present_r[0] = paints;              /* must be 4: revealed as drawn   */
        paints = 0;
        for (i = 0; i < 30; i++)
            paints += (unsigned)present_frame(0u, 0);
        present_r[1] = paints;              /* must be 30                     */
        present_r[2] = x86_modey_mid_draw;  /* must be 0: it never flipped    */
    } else {
        /* PATHOLOGICAL. A title that stores forever and never flips must not be
           a frozen screen -- under this policy it is simply painted every
           frame, which is what every non-Mode-Y mode does. */
        for (i = 0; i < 180; i++)
            paints += (unsigned)present_frame(1u, 0);
        present_r[0] = paints;              /* must be 180                    */
        present_r[1] = x86_modey_fallbacks; /* must be 180                    */
    }
    x86_vga_modey_arm(0);
}

/* ---------------------------------------------------------------------------
 * THE ENLARGED EGA CARD: the loan, the compaction, and the refusal.
 *
 * Run against the layout the REAL core claims, not one this arm invents: 35
 * pages of arena, CGA (32,768) then the 13h aperture (65,536), in that order,
 * because the order is what leaves the free space fragmented and is therefore
 * half of what is under test. Expected, and every number is arithmetic:
 *
 *   before   143,360 - 32,768 - 65,536 = 45,056 free, all of it in one run
 *   0Dh      the CGA claim is lent out and the aperture is pulled down into
 *            the vacated bottom, leaving 77,824 contiguous = 19,456 offsets
 *   back     the CGA claim is returned and the cell buffer is blank
 *
 * The negative control is 24 pages -- exactly CGA plus the aperture, nothing
 * spare. 0Dh must be REFUSED and counted, not quietly given a card of zero:
 * a mode adopted with nowhere for a pixel to go is a black screen with no
 * cause, which is the failure the arena's all-or-nothing rule exists to stop.
 */
volatile uint32_t run_egafit_flag = 0;
volatile uint32_t egafit_r[16];

static void egafit_test_run(void)
{
    void *cga = NULL, *v13 = NULL;
    uint32_t refused0;

    for (unsigned i = 0; i < 16; i++)
        egafit_r[i] = 0;

    x86_arena_reset();
    x86_arena_add_region(arena_test_mem, 35u * 4096u);
    egafit_r[0] = (x86_arena_claim(X86_ARENA_ID_CGA, 1, X86_VRAM_BYTES,
                                   &cga) == 0);
    egafit_r[1] = (x86_arena_claim(X86_ARENA_ID_VGA13, 1,
                                   X86_VGA_APERTURE_BYTES, &v13) == 0);
    egafit_r[2] = x86_arena_largest;            /* 45,056 */
    x86_video_init(cga, cga_test_palette);
    x86_video_set_aperture(v13, vga_test_pal);
    x86_video_map();
    /* Text FIRST, always. mode_select() short-circuits a set to the mode
       already up, and the arm that ran before this one leaves 0Dh up -- so
       without this the whole test measures the PREVIOUS arm's card. */
    x86_video_set_mode(0x03);

    refused0 = x86_vga_ega_refused;
    x86_video_set_mode(0x0D);
    egafit_r[3] = x86_video_mode();                         /* 0x0D */
    egafit_r[4] = x86_vga_ega_card_offsets;                 /* 19,456 */
    egafit_r[5] = x86_arena_held(X86_ARENA_ID_CGA);         /* 0: lent */
    egafit_r[6] = x86_vga_ega_armed;                        /* 1 */
    egafit_r[7] = x86_vga_ega_refused - refused0;           /* 0 */

    /* Scribble the whole card, so a text buffer that came back holding plane
       bytes rather than blanks cannot pass by being incidentally zero. */
    if (x86_vga_ega_planes())
        memset((void *)x86_vga_ega_planes(), 0xEE,
               4u * x86_vga_ega_plane_offsets());

    x86_video_set_mode(0x03);
    egafit_r[8]  = x86_video_mode();                        /* 3 */
    egafit_r[9]  = x86_arena_held(X86_ARENA_ID_CGA);        /* 1: returned */
    egafit_r[10] = *(const uint16_t *)x86_video_vram();     /* 0x0720 */
    egafit_r[11] = x86_vga_ega_armed;                       /* 0 */

    /* ---- the negative control: no spare page anywhere. */
    x86_arena_reset();
    x86_arena_add_region(arena_test_mem, 24u * 4096u);
    x86_arena_claim(X86_ARENA_ID_CGA, 1, X86_VRAM_BYTES, &cga);
    x86_arena_claim(X86_ARENA_ID_VGA13, 1, X86_VGA_APERTURE_BYTES, &v13);
    x86_video_init(cga, cga_test_palette);
    x86_video_set_aperture(v13, vga_test_pal);
    x86_video_map();
    x86_video_set_mode(0x03);
    refused0 = x86_vga_ega_refused;
    x86_video_set_mode(0x0D);
    egafit_r[12] = x86_video_mode();                        /* NOT 0x0D */
    egafit_r[13] = x86_vga_ega_refused - refused0;          /* >= 1 */
    egafit_r[14] = x86_vga_ega_armed;                       /* 0 */
    egafit_r[15] = x86_arena_deficit;                       /* > 0 */

    /* Leave nothing behind: the standard buffers and an empty arena. */
    x86_arena_reset();
    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_video_map();
}

/*
 * The Mode Y OFF-SCREEN CACHE, fitted against the layout the real core claims.
 *
 * The cache was a fixed 8,192 offsets covering 0xC300..0xE2FF, and Wolfenstein
 * 3D's cached pictures came out as noise because it writes them right up to
 * 0xFFFF: measured on the device, 16,896 stores discarded and 4,928 latch
 * loads reading an absent cell; measured under gwemu after the fit was made
 * adaptive, x86_vga_modey_lost_lo/_hi read 0xEF00 and 0xFFFF with 11,264
 * offsets fitted, which is what said the shortfall is the TAIL of the window
 * rather than some interior hole.
 *
 * This arm runs the same fit the core runs, on the same 143,360-byte arena,
 * and asserts the cache reaches 0x10000 -- because the ARITHMETIC working out
 * is not evidence that a contiguous run exists. It has its own negative
 * control (a small arena, where the fit must give something smaller and must
 * still not refuse), and -DX86_MODEY_CACHE_NEGCTL=1 grades the check itself.
 */
volatile uint32_t run_modeyfit_flag = 0;
volatile uint32_t modeyfit_r[16];

static void modeyfit_test_run(void)
{
    void *cga = NULL, *v13 = NULL;

    for (unsigned i = 0; i < 16; i++)
        modeyfit_r[i] = 0;

    x86_arena_reset();
    x86_arena_add_region(arena_test_mem, 35u * 4096u);
    modeyfit_r[0] = (x86_arena_claim(X86_ARENA_ID_CGA, 1, X86_VRAM_BYTES,
                                     &cga) == 0);
    modeyfit_r[1] = (x86_arena_claim(X86_ARENA_ID_VGA13, 1,
                                     X86_VGA_APERTURE_BYTES, &v13) == 0);
    modeyfit_r[2] = x86_arena_largest;              /* 45,056 */
    x86_video_init(cga, cga_test_palette);
    x86_video_set_aperture(v13, vga_test_pal);
    x86_video_map();
    /* Text FIRST: mode_select() short-circuits a set to the mode already up,
       and the arm before this one leaves a graphics mode behind. */
    x86_video_set_mode(0x03);

    x86_video_set_mode(0x13);
    modeyfit_r[3] = x86_video_mode();                       /* 0x13 */
    modeyfit_r[4] = x86_vga_modey_cache_offs;               /* 15,616 */
    modeyfit_r[5] = x86_arena_held(X86_ARENA_ID_CGA);       /* 0: lent */
    modeyfit_r[6] = x86_arena_largest;

    /* Scribble the cache, so a text buffer that came back holding cache bytes
       rather than blanks cannot pass by being incidentally zero. */
    if (x86_vga_modey_cache_offs)
        memset((void *)x86_vga_modey_cache_ptr(), 0xEE,
               4u * x86_vga_modey_cache_offs);

    x86_video_set_mode(0x03);
    modeyfit_r[7]  = x86_video_mode();                      /* 3 */
    modeyfit_r[8]  = x86_arena_held(X86_ARENA_ID_CGA);      /* 1: returned */
    modeyfit_r[9]  = *(const uint16_t *)x86_video_vram();   /* 0x0720 */
    modeyfit_r[10] = x86_vga_modey_cache_offs;              /* 0: released */

    /* ---- the negative control: an arena with no room for the full cache.
       A SMALLER cache is the right answer here, not a refusal -- 13h itself
       does not need the cache to render, only Mode Y's off-screen writes do,
       and refusing the mode over it would lose a whole mode to a cache. */
    x86_arena_reset();
    x86_arena_add_region(arena_test_mem, 26u * 4096u);
    x86_arena_claim(X86_ARENA_ID_CGA, 1, X86_VRAM_BYTES, &cga);
    x86_arena_claim(X86_ARENA_ID_VGA13, 1, X86_VGA_APERTURE_BYTES, &v13);
    x86_video_init(cga, cga_test_palette);
    x86_video_set_aperture(v13, vga_test_pal);
    x86_video_map();
    x86_video_set_mode(0x03);
    x86_video_set_mode(0x13);
    modeyfit_r[11] = x86_video_mode();                      /* still 0x13 */
    modeyfit_r[12] = x86_vga_modey_cache_offs;              /* < 15,616, > 0 */

    x86_arena_reset();
    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_video_map();
}

/* ===========================================================================
 * MODE Y REAL PAGE STORAGE -- the property a pixel test cannot see.
 *
 * Every pixel arm in this suite renders ONE frame from ONE page, and passes
 * 64000/64000 with all three of Wolf3D's pages folded onto the same bytes. The
 * defect is invisible to it by construction: it only shows when the guest DRAWS
 * page N+1 while the panel is showing page N, which is what the raycaster does
 * every frame and what the owner sees as walls rendered twice, once in
 * perspective and once unstretched.
 *
 * So the arm asserts the property directly and in both directions:
 *   with real pages   a write at page 1 leaves page 0's bytes alone;
 *   with the alias    the SAME write lands on top of page 0.
 * The second half is the negative control, and it is the half that makes the
 * first half a check rather than a tautology -- it is seen to fail here, in
 * the same run, rather than being reasoned about.
 *
 * Storage is carved out of test_ram (256 KB..384 KB) for the same reason the
 * arena region is: 128 KB of new .bss does not fit in any region of this
 * harness, and this arm runs no guest code above 64 KB.
 */
static void planar_test_unmap(void);

#define MODEYPG_OFF   (256u * 1024u)
volatile uint32_t run_modeypage_flag = 0;
volatile uint32_t modeypage_r[24];

/* One plane offset of page `pg`, read back through the SAME cell lookup the
   store used -- x86_vga_modey_row_at(), which is modey_split() and therefore
   the store path's own answer. Reading page 0 out of the aperture by hand
   would be a second opinion about where a row lives, which is the exact drift
   the reference deleted two helpers to stop. */
static uint32_t modeypg_peek(uint32_t win_off)
{
    const uint8_t *base;
    uint32_t off;
    if (!x86_vga_modey_row_at(win_off, &base, &off))
        return 0xFFFFFFFFu;
    return base[4u * off];      /* plane 0 of that cell */
}

static void modeypage_test_run(void)
{
    uint8_t *p1 = test_ram + MODEYPG_OFF;
    uint8_t *p2 = p1 + X86_MODEY_PAGE_BYTES;
    const uint32_t stride = 16640u;     /* WOLF3D's, and the shipping default */
    unsigned i;

    for (i = 0; i < 24; i++)
        modeypage_r[i] = 0;

    planar_test_init();
    /* Mode Y with no guest: the register file the arm needs is exactly what
       x86_vga_modey_arm() plants (write mode 0, bit mask 0xFF, map mask 0x0F),
       so a plain store is one byte into all four planes. */
    x86_vga_modey_arm(0);
    x86_vga_modey_set_pages(NULL, NULL);
    x86_vga_modey_arm(1);
    modeypage_r[0] = x86_vga_modey_page;         /* 16,640 */
    modeypage_r[1] = x86_vga_modey_pages;        /* 1: aliased */

    /* ---- NEGATIVE CONTROL FIRST, so a broken positive cannot be read as a
       pass on stale state. Page 0 gets 0x11, then page 1 gets 0x22 at the same
       offset within its page -- and page 0 must come back 0x22. */
    x86_vga_planar_store(0u, 0x11u, 1);
    x86_vga_planar_store(stride, 0x22u, 1);
    modeypage_r[2] = modeypg_peek(0u);           /* 0x22: CLOBBERED */
    modeypage_r[3] = modeypg_peek(stride);       /* 0x22 */
    modeypage_r[4] = x86_vga_modey_alias_hits;   /* > 0: the loud counter */

    /* ---- the positive: the same sequence with real storage. */
    x86_vga_modey_set_pages(p1, p2);
    modeypage_r[5] = x86_vga_modey_pages;        /* 3 */
    modeypage_r[6] = x86_vga_modey_alias_hits;   /* 0, reset by set_pages */

    x86_vga_planar_store(0u, 0x11u, 1);
    x86_vga_planar_store(stride, 0x22u, 1);
    x86_vga_planar_store(2u * stride, 0x33u, 1);
    modeypage_r[7]  = modeypg_peek(0u);          /* 0x11: UNDISTURBED */
    modeypage_r[8]  = modeypg_peek(stride);      /* 0x22 */
    modeypage_r[9]  = modeypg_peek(2u * stride); /* 0x33 */
    modeypage_r[10] = x86_vga_modey_alias_hits;  /* still 0 */

    /* The three pages must be three DIFFERENT places, not three names for one.
       Comparing the returned bases is the structural statement behind the byte
       checks above. */
    {
        const uint8_t *b0, *b1, *b2;
        uint32_t o;
        x86_vga_modey_row_at(0u, &b0, &o);
        x86_vga_modey_row_at(stride, &b1, &o);
        x86_vga_modey_row_at(2u * stride, &b2, &o);
        modeypage_r[11] = (b0 != b1) && (b1 != b2) && (b0 != b2);
        modeypage_r[12] = (b0 == x86_vga_planar_aperture);   /* page 0 is it */
    }

    /* A whole page of stores, the way a raycaster fills one: page 1 covered end
       to end must not touch one byte of page 0. A single-offset check passes
       against an off-by-a-few fold; this does not. */
    for (i = 0; i < X86_MODEY_PLANE_OFFSETS; i++)
        x86_vga_planar_store(stride + i, 0xA5u, 1);
    {
        uint32_t bad = 0;
        for (i = 0; i < X86_MODEY_PLANE_OFFSETS; i++)
            if (x86_vga_planar_aperture[4u * i] == 0xA5u)
                bad++;
        modeypage_r[13] = bad;                   /* 0 */
    }
    modeypage_r[14] = modeypg_peek(0u);          /* still 0x11 */
    modeypage_r[15] = x86_vga_modey_alias_hits;  /* still 0 */

    /* ---- HALF FUNDING. One real page is the rung that still removes the
       hazard WOLF3D actually hits: it never draws into the page it displays,
       only into the next one. Page 2 must then be the one that aliases, and it
       must SAY SO. */
    x86_vga_modey_set_pages(p1, NULL);
    modeypage_r[16] = x86_vga_modey_pages;       /* 2 */
    x86_vga_planar_store(0u, 0x44u, 1);
    x86_vga_planar_store(stride, 0x55u, 1);
    modeypage_r[17] = modeypg_peek(0u);          /* 0x44: undisturbed */
    modeypage_r[18] = x86_vga_modey_alias_hits;  /* 0 so far */
    x86_vga_planar_store(2u * stride, 0x66u, 1);
    modeypage_r[19] = modeypg_peek(0u);          /* 0x66: page 2 aliases */
    modeypage_r[20] = (x86_vga_modey_alias_hits > 0);   /* loud */
    /* Page 2 without page 1 is refused rather than half-adopted. */
    x86_vga_modey_set_pages(NULL, p2);
    modeypage_r[21] = x86_vga_modey_pages;       /* 1 */

    x86_vga_modey_set_pages(NULL, NULL);
    x86_vga_modey_arm(0);
    planar_test_unmap();
}

/* Put guest 0xA0000/0xB8000 back on test_ram, so an arm that runs after this
   one sees the identity map the harness set up at reset. */
static void planar_test_unmap(void)
{
    for (uint32_t g = 0xA0000u; g < 0xC0000u; g += 4096u) {
        uint32_t i = g >> FOLD_SHIFT;
        guest_page_table[i] = FOLD_K((uint32_t)test_ram + g, i);
    }
}

static void modeswitch_test_tty(void)
{
    test_ram[MSW_TTY_BASE + 3] = (uint8_t)msw_char_arg;
    kbd_test_reset_cpu(MSW_TTY_BASE);
    msw_r[5] = kbd_test_run_to_halt();
    msw_snapshot();
}

/* ---------------------------------------------------------------------------
 * The machine DESCRIPTION services, driven by guest code.
 *
 * INT 11h (equipment), INT 12h (conventional KB) and INT 15h AH=88h (extended
 * KB) are how a DOS program asks what machine it is on, and every one of them
 * was reaching the bare IRET stub -- which does not clear a register, so the
 * guest read back its OWN poison value and believed it. That failure has no
 * symptom the host can see: the call "returns", the counters all move, and the
 * program simply configures itself for a machine that does not exist.
 *
 * So the arm POISONS AX before every call and sets CF the wrong way round
 * before the two that carry one. An unserviced vector then reads back the
 * poison, which is the only way to tell "answered 0xFFFF" from "answered
 * nothing" -- and 0xFFFF KB of conventional memory is exactly the shape of
 * answer a program cannot survive.
 *
 * CF comes off the IRET FRAME (PUSHF after the INT), not from a live register,
 * for the same reason the INT 16h arm reads ZF that way.
 */
#define SYS_CODE_BASE   0x2700u
#define SYS_RES_BASE    0x4C00u

volatile uint32_t run_sysinfo_flag = 0;
volatile uint32_t sys_r[16];

static const uint8_t sys_guest_code[] = {
    /* INT 11h -- equipment list. AX poisoned; a bare IRET returns the poison. */
    0xB8, 0xFF, 0xFF,                   /* MOV AX,FFFF                     */
    0xCD, 0x11,                         /* INT 11h                         */
    0xA3, 0x00, 0x4C,                   /* MOV [4C00],AX                   */

    /* INT 12h -- conventional memory in KB. */
    0xB8, 0xFF, 0xFF,                   /* MOV AX,FFFF                     */
    0xCD, 0x12,                         /* INT 12h                         */
    0xA3, 0x02, 0x4C,                   /* MOV [4C02],AX                   */

    /* INT 15h AH=88h -- extended memory in KB. CF poisoned SET; a machine
       with no extended memory answers AX=0 with CF CLEAR. */
    0xB8, 0x00, 0x88,                   /* MOV AX,8800                     */
    0xF9,                               /* STC                             */
    0xCD, 0x15,                         /* INT 15h                         */
    0xA3, 0x04, 0x4C,                   /* MOV [4C04],AX                   */
    0x9C,                               /* PUSHF                           */
    0x58,                               /* POP AX                          */
    0xA3, 0x06, 0x4C,                   /* MOV [4C06],AX                   */

    /* INT 15h AH=C0h -- get system configuration. We have no configuration
       table, and the honest answer is the one a PC/XT BIOS gives: CF SET,
       AH=86h "unsupported function". CF poisoned CLEAR. */
    0xB8, 0x00, 0xC0,                   /* MOV AX,C000                     */
    0xF8,                               /* CLC                             */
    0xCD, 0x15,                         /* INT 15h                         */
    0xA3, 0x08, 0x4C,                   /* MOV [4C08],AX                   */
    0x9C,                               /* PUSHF                           */
    0x58,                               /* POP AX                          */
    0xA3, 0x0A, 0x4C,                   /* MOV [4C0A],AX                   */

    /*
     * The PC speaker, probed the way a program CONFIRMS one is there: program
     * PIT channel 2, ungate it, and watch port 0x61 report back. Bit 5 is the
     * channel's OUT line and bit 4 the DRAM refresh toggle, and both used to
     * be whatever the guest last WROTE to 0x61 -- so both were constants, and
     * a constant is not a wrong value here, it is a hang: the standard idiom
     * is "read 0x61 until the bit flips".
     *
     * BL accumulates OR of every sample and BH accumulates AND, so one word
     * says whether each bit was ever high AND ever low. A stuck bit fails
     * whichever way it is stuck; a single sample could not tell.
     */
    0xB0, 0xB6, 0xE6, 0x43,             /* MOV AL,B6 / OUT 43,AL (ch2 mode3)*/
    0xB0, 0xFB, 0xE6, 0x42,             /* MOV AL,FB / OUT 42,AL (lo)       */
    0xB0, 0x11, 0xE6, 0x42,             /* MOV AL,11 / OUT 42,AL (hi)       */
    0xE4, 0x61, 0x0C, 0x03, 0xE6, 0x61, /* IN 61 / OR AL,3 / OUT 61 (ungate)*/
    0xBB, 0x00, 0xFF,                   /* MOV BX,FF00  BL=OR 0, BH=AND FF  */
    /*
     * THE SAMPLE COUNT IS A TIME, and it was 1024 while bit 5 was a SHAPE
     * fitted to the instruction budget rather than a counter. It is a counter
     * now (x86_io.c), so the loop has to be long enough to cover a full period
     * of the divisor it just programmed: 0x11FB = 4603 PIT ticks, and a PIT
     * tick is worth ~4.6 guest instructions at the emulated CPU's nominal
     * speed. 8192 iterations x 4 instructions is ~7100 ticks -- more than one
     * whole period, so the bit is seen both high and low whatever phase the
     * loop starts in. This is the emulated machine being FAST, not the probe
     * being wrong: real detection code loops until the bit flips.
     */
    0xB9, 0x00, 0x20,                   /* MOV CX,2000h (8192)              */
    0xE4, 0x61,                         /* .loop: IN AL,61                  */
    0x08, 0xC3,                         /*        OR BL,AL                  */
    0x20, 0xC7,                         /*        AND BH,AL                 */
    0xE2, 0xF8,                         /*        LOOP .loop                */
    0x89, 0x1E, 0x0C, 0x4C,             /* MOV [4C0C],BX                    */
    0xE4, 0x61, 0x24, 0xFC, 0xE6, 0x61, /* gate the speaker back off        */

    /* INT 10h AH=1Ah -- the display combination code, which is how a VGA
       program asks WHICH adapter it is talking to. AL=1Ah back means "this
       BIOS has the call"; BL is the active display's code and BH the
       alternate's. BX is poisoned FFFF, because a BIOS that answers AL=1Ah
       and leaves BX alone has told the caller it has a display type of
       0xFF -- and the table lookup that follows is off the end of the
       caller's own array. Both halves have to be checked. */
    0xB8, 0x00, 0x1A,                   /* MOV AX,1A00                     */
    0xBB, 0xFF, 0xFF,                   /* MOV BX,FFFF                     */
    0xCD, 0x10,                         /* INT 10h                         */
    0xA3, 0x0E, 0x4C,                   /* MOV [4C0E],AX                   */
    0x89, 0x1E, 0x10, 0x4C,             /* MOV [4C10],BX                   */

    0xF4,                               /* HLT                             */
};

static void sysinfo_test_run(void)
{
    static const uint8_t hle_vectors[] = {
        0x08, 0x09, 0x10, 0x11, 0x12, 0x15, 0x16, 0x1A, 0x20, 0x21,
    };
    memset(&test_ram[SYS_RES_BASE], 0, 20);
    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));
    memcpy(&test_ram[SYS_CODE_BASE], sys_guest_code, sizeof(sys_guest_code));

    kbd_test_reset_cpu(SYS_CODE_BASE);
    sys_r[8] = kbd_test_run_to_halt();

    for (unsigned i = 0; i < 8; i++)
        sys_r[i] = (uint16_t)(test_ram[SYS_RES_BASE + i * 2] |
                              (test_ram[SYS_RES_BASE + i * 2 + 1] << 8));
    /* sys_r[8] is the exit reason, so the AH=1Ah BX lands at the top. */
    sys_r[14] = (uint16_t)(test_ram[SYS_RES_BASE + 16] |
                           (test_ram[SYS_RES_BASE + 17] << 8));

    /* The same facts as MEMORY, which is the other half of the contract: a
       program is free to peek the BDA instead of calling, and the two must not
       be able to disagree. */
    sys_r[9]  = kbd_bda_r16(0x10);     /* equipment word            */
    sys_r[10] = kbd_bda_r16(0x13);     /* conventional memory in KB */
    sys_r[11] = kbd_bda_r16(0x63);     /* CRTC base port            */
    sys_r[12] = kbd_bda_r16(0x80);     /* keyboard ring start       */
    sys_r[13] = kbd_bda_r16(0x82);     /* keyboard ring end         */
}

/* ---------------------------------------------------------------------------
 * PIT channel 0 -- the SYSTEM TIMER's divisor, programmed by GUEST code.
 *
 * Channel 0 decides how fast the guest's clock runs, and until it was modelled
 * every write to it was dropped: a title that programmed 140 Hz got 18.2 Hz and
 * its whole sense of time -- movement, animation, menu key-repeat -- ran at 13%
 * of real. Wolfenstein 3D is exactly that title (OUT 43h,36h then 4Bh/21h).
 *
 * The arm executes the guest's own OUTs rather than calling the port handler
 * from the host, because the question is what a program's instruction stream
 * does to the machine. Four phases, each ending in HLT:
 *
 *   A  36h + lo/hi   -> 0x214B, the real Wolf3D program
 *   B  14h + one lo  -> lo-byte-only access must leave the HIGH byte alone
 *   C  a channel 2   -> programming the SPEAKER must not touch the timer
 *   D  36h + 00/00   -> a reload of 0 is 65536, not a divide by zero
 */
#define PIT_CODE_A      0x2A00u
#define PIT_CODE_B      0x2A20u
#define PIT_CODE_C      0x2A40u
#define PIT_CODE_D      0x2A60u

volatile uint32_t run_pit_flag = 0;
volatile uint32_t pit_r[10];
/* Phase E's results, read back out of GUEST memory. A separate array so the
   published pit_r[10] layout tests/test_pit.py already reads stays untouched. */
volatile uint32_t ztimer_r[8];

static const uint8_t pit_code_a[] = {
    0xB0, 0x36,                   /* MOV AL,36h  ch0, lo/hi, mode 3         */
    0xE6, 0x43,                   /* OUT 43h,AL                             */
    0xB0, 0x4B,                   /* MOV AL,4Bh  divisor low                */
    0xE6, 0x40,                   /* OUT 40h,AL                             */
    0xB0, 0x21,                   /* MOV AL,21h  divisor high -> 0x214B     */
    0xE6, 0x40,                   /* OUT 40h,AL                             */
    0xF4,                         /* HLT                                    */
};
static const uint8_t pit_code_b[] = {
    0xB0, 0x14,                   /* MOV AL,14h  ch0, LO BYTE ONLY, mode 2  */
    0xE6, 0x43,                   /* OUT 43h,AL                             */
    0xB0, 0xFF,                   /* MOV AL,FFh                             */
    0xE6, 0x40,                   /* OUT 40h,AL  -> 0x21FF, high byte kept  */
    0xF4,                         /* HLT                                    */
};
static const uint8_t pit_code_c[] = {
    0xB0, 0xB6,                   /* MOV AL,B6h  ch2, lo/hi, mode 3         */
    0xE6, 0x43,                   /* OUT 43h,AL                             */
    0xB0, 0x34,                   /* MOV AL,34h                             */
    0xE6, 0x42,                   /* OUT 42h,AL                             */
    0xB0, 0x12,                   /* MOV AL,12h                             */
    0xE6, 0x42,                   /* OUT 42h,AL  -> speaker 0x1234          */
    0xF4,                         /* HLT                                    */
};
static const uint8_t pit_code_d[] = {
    0xB0, 0x36,                   /* MOV AL,36h                             */
    0xE6, 0x43,                   /* OUT 43h,AL                             */
    0xB0, 0x00,                   /* MOV AL,00                              */
    0xE6, 0x40,                   /* OUT 40h,AL                             */
    0xE6, 0x40,                   /* OUT 40h,AL  -> reload 0 == 65536       */
    0xF4,                         /* HLT                                    */
};

/*
 * PHASE E -- THE PRECISION ZEN TIMER'S ACTUAL SEQUENCE.
 *
 * This is not a paraphrase of what a timing library might do: it is what
 * references/TOPBENCH/ZTIMER11/PZTIMER.ASM does, in the order it does it, and
 * it is what TOPBENCH runs five times per benchmark. Reading it wrong is what
 * put 65535 microseconds in every sub-timing on the panel.
 *
 *   ch2  program mode 2 / count 0 (= 65536), ungate at port 0x61 bit 0, then
 *        LATCH (OUT 43h,80h) and read the pair back off 0x42 -- three times,
 *        with work in between. The counter must MOVE, and move DOWN.
 *   ch0  PZTimerOn: OUT 43h,34h then a 0/0 count (PZTIMER.ASM:186-224).
 *        PZTimerOff: latch with OUT 43h,00h, read lo/hi off 0x40 and NEG AX to
 *        turn "counts remaining" into "counts elapsed" (PZTIMER.ASM:246-266).
 *   PIC  PZTimerOff's OVERFLOW test: OCW3 0x0A to port 0x20 selects the IRR,
 *        and IN AL,20h reads it. Bit 0 set means IRQ 0 is pending, which
 *        PZTimerCount() reports as 0xFFFFFFFF (PZTIMER.ASM:488-493) -- 65535
 *        once TOPBENCH truncates it to a word (TOPB_DAT.PAS:31-35). An
 *        unmodelled port floats to 0xFF and declares overflow every time.
 */
#define PIT_CODE_E      0x2A80u
#define ZT_RES          0x2B00u

static const uint8_t pit_code_e[] = {
    /* -- channel 2: mode 2, count 65536, gate on ------------------------- */
    0xB0, 0xB4,                   /* MOV AL,B4h   ch2, lo/hi, mode 2        */
    0xE6, 0x43,                   /* OUT 43h,AL                             */
    0xB0, 0x00,                   /* MOV AL,0                               */
    0xE6, 0x42,                   /* OUT 42h,AL   count lo                  */
    0xE6, 0x42,                   /* OUT 42h,AL   count hi -> 65536         */
    0xB0, 0x01,                   /* MOV AL,1     gate on, speaker data off */
    0xE6, 0x61,                   /* OUT 61h,AL                             */
    /* -- latch + read pair #1 -------------------------------------------- */
    0xB0, 0x80,                   /* MOV AL,80h   latch ch2                 */
    0xE6, 0x43,                   /* OUT 43h,AL                             */
    0xE4, 0x42,                   /* IN  AL,42h   lsb                       */
    0x8A, 0xE0,                   /* MOV AH,AL                              */
    0xE4, 0x42,                   /* IN  AL,42h   msb                       */
    0x86, 0xE0,                   /* XCHG AH,AL   -> AX = count             */
    0xA3, 0x00, 0x2B,             /* MOV [2B00h],AX                         */
    /* -- work ------------------------------------------------------------ */
    0xB9, 0xA0, 0x0F,             /* MOV CX,4000                            */
    0xE2, 0xFE,                   /* LOOP $                                 */
    /* -- latch + read pair #2 -------------------------------------------- */
    0xB0, 0x80,                   /* MOV AL,80h                             */
    0xE6, 0x43,                   /* OUT 43h,AL                             */
    0xE4, 0x42,                   /* IN  AL,42h                             */
    0x8A, 0xE0,                   /* MOV AH,AL                              */
    0xE4, 0x42,                   /* IN  AL,42h                             */
    0x86, 0xE0,                   /* XCHG AH,AL                             */
    0xA3, 0x02, 0x2B,             /* MOV [2B02h],AX                         */
    /* -- PZTimerOn, on channel 0 ----------------------------------------- */
    0xB0, 0x34,                   /* MOV AL,34h   ch0, lo/hi, mode 2        */
    0xE6, 0x43,                   /* OUT 43h,AL                             */
    0xB0, 0x00,                   /* MOV AL,0                               */
    0xE6, 0x40,                   /* OUT 40h,AL                             */
    0xE6, 0x40,                   /* OUT 40h,AL   -> 65536                  */
    0xB9, 0x40, 0x1F,             /* MOV CX,8000                            */
    0xE2, 0xFE,                   /* LOOP $       the "timed" code          */
    /* -- PZTimerOff ------------------------------------------------------ */
    0xB0, 0x00,                   /* MOV AL,0     latch ch0                 */
    0xE6, 0x43,                   /* OUT 43h,AL                             */
    0xE4, 0x40,                   /* IN  AL,40h   lsb                       */
    0x8A, 0xE0,                   /* MOV AH,AL                              */
    0xE4, 0x40,                   /* IN  AL,40h   msb                       */
    0x86, 0xE0,                   /* XCHG AH,AL                             */
    0xF7, 0xD8,                   /* NEG AX       remaining -> elapsed      */
    0xA3, 0x04, 0x2B,             /* MOV [2B04h],AX                         */
    /* -- the overflow test ----------------------------------------------- */
    0xB0, 0x0A,                   /* MOV AL,0Ah   OCW3: read the IRR        */
    0xE6, 0x20,                   /* OUT 20h,AL                             */
    0xE4, 0x20,                   /* IN  AL,20h                             */
    0x30, 0xE4,                   /* XOR AH,AH                              */
    0xA3, 0x06, 0x2B,             /* MOV [2B06h],AX                         */
    /* -- a third ch2 read: the clock must still be running ---------------- */
    0xB9, 0xA0, 0x0F,             /* MOV CX,4000                            */
    0xE2, 0xFE,                   /* LOOP $                                 */
    0xB0, 0x80,                   /* MOV AL,80h                             */
    0xE6, 0x43,                   /* OUT 43h,AL                             */
    0xE4, 0x42,                   /* IN  AL,42h                             */
    0x8A, 0xE0,                   /* MOV AH,AL                              */
    0xE4, 0x42,                   /* IN  AL,42h                             */
    0x86, 0xE0,                   /* XCHG AH,AL                             */
    0xA3, 0x08, 0x2B,             /* MOV [2B08h],AX                         */
    0xF4,                         /* HLT                                    */
};

static uint32_t zt_r16(uint32_t off)
{
    return (uint32_t)(test_ram[ZT_RES + off] |
                      ((uint32_t)test_ram[ZT_RES + off + 1] << 8));
}

static void pit_test_run(void)
{
    static const uint8_t hle_vectors[] = { 0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21 };

    /* x86_bios_init() puts the renderer into mode 3, which needs a VRAM
       pointer -- the real core calls x86_video_init() first and so must this.
       Omitting it is a fault inside the arm, which reads as "never completed". */
    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));
    memcpy(&test_ram[PIT_CODE_A], pit_code_a, sizeof(pit_code_a));
    memcpy(&test_ram[PIT_CODE_B], pit_code_b, sizeof(pit_code_b));
    memcpy(&test_ram[PIT_CODE_C], pit_code_c, sizeof(pit_code_c));
    memcpy(&test_ram[PIT_CODE_D], pit_code_d, sizeof(pit_code_d));

    /* A cold machine: the boot divisor, which is 0 stored and 65536 meant. */
    x86_io.pit0_reload = 0;
    x86_io.pit0_access = 0;
    x86_io.pit0_latched = 0;
    x86_pit0_writes = 0;
    pit_r[0] = x86_pit0_divisor();          /* expect 65536 before anything */

    kbd_test_reset_cpu(PIT_CODE_A);
    pit_r[8] = kbd_test_run_to_halt();
    pit_r[1] = x86_pit0_divisor();          /* expect 0x214B = 8523         */
    pit_r[2] = x86_pit0_writes;             /* expect 2                     */

    kbd_test_reset_cpu(PIT_CODE_B);
    pit_r[9] = kbd_test_run_to_halt();
    pit_r[3] = x86_pit0_divisor();          /* expect 0x21FF = 8703         */

    kbd_test_reset_cpu(PIT_CODE_C);
    (void)kbd_test_run_to_halt();
    pit_r[4] = x86_pit0_divisor();          /* UNCHANGED: still 0x21FF      */
    /* The RELOAD, not x86_speaker_divisor(): that answers 0 while the gate at
       port 0x61 is shut, which it is here -- this arm programs a tone, it does
       not sound one. The question is only whether channel 2 still latched. */
    pit_r[5] = x86_io.pit2_reload;          /* expect 0x1234 = 4660         */

    kbd_test_reset_cpu(PIT_CODE_D);
    (void)kbd_test_run_to_halt();
    pit_r[6] = x86_pit0_divisor();          /* expect 65536, never 0        */
    pit_r[7] = x86_pit0_writes;             /* expect 5 across the run      */

    /* ---- phase E: the Zen Timer's own sequence ------------------------- */
    memcpy(&test_ram[PIT_CODE_E], pit_code_e, sizeof(pit_code_e));
    memset(&test_ram[ZT_RES], 0, 16);
    x86_io.port61 = 0;
    kbd_test_reset_cpu(PIT_CODE_E);
    ztimer_r[5] = kbd_test_run_to_halt();
    ztimer_r[0] = zt_r16(0);                /* ch2 count, read 1            */
    ztimer_r[1] = zt_r16(2);                /* ch2 count, read 2            */
    ztimer_r[2] = zt_r16(4);                /* ch0 elapsed counts (NEG AX)  */
    ztimer_r[3] = zt_r16(6);                /* the 8259 IRR                 */
    ztimer_r[4] = zt_r16(8);                /* ch2 count, read 3            */
    ztimer_r[6] = x86_pit_ticks();
}

/* ---------------------------------------------------------------------------
 * SOUND CARD DETECTION -- the Sound Blaster DSP reset handshake and the AdLib
 * timer test, run as GUEST INSTRUCTIONS because that is how a title asks.
 *
 * These are the two sequences measured on this tree's own titles: Prince of
 * Persia writes 0x226 and then polls 0x22E for the 0xAA at 0x22A (32 polls, 33
 * reads, in the run where nothing answered), and Wolfenstein 3D and Keen 4 read
 * 0x388 348 times around six writes to 0x388/0x389 -- an AdLib timer test.
 *
 * Results at SND_RES, one word each, read back out of GUEST memory:
 *   0  data-available status polled at 0x22E (bit 7 must come up)
 *   2  polls left in CX (0 means it timed out)
 *   4  the byte at 0x22A               MUST BE 0xAA
 *   6  the write-buffer status at 0x22C (bit 7 must be CLEAR = ready)
 *   8  DSP version major               2
 *  10  DSP version minor               1
 *  12  OPL status after both timers masked and the flags reset  &0xE0 == 0
 *  14  OPL status after timer 1 has expired                     &0xE0 == 0xC0
 *  16  OPL status after the flags are reset again               &0xE0 == 0
 */
#define SND_CODE        0x2C00u
#define SND_RES         0x2D00u

volatile uint32_t run_sound_flag = 0;
volatile uint32_t snd_r[16];

static const uint8_t sound_code[] = {
    /* -- the DSP reset handshake: write 1, write 0, expect 0xAA ---------- */
    0xBA, 0x26, 0x02,             /* MOV DX,0226h  DSP reset               */
    0xB0, 0x01,                   /* MOV AL,1                              */
    0xEE,                         /* OUT DX,AL                             */
    0xB0, 0x00,                   /* MOV AL,0                              */
    0xEE,                         /* OUT DX,AL                             */
    0xBA, 0x2E, 0x02,             /* MOV DX,022Eh  read-buffer status      */
    0xB9, 0x64, 0x00,             /* MOV CX,100    a BOUNDED poll          */
    0xEC,                         /* IN  AL,DX   <-+                       */
    0xA8, 0x80,                   /* TEST AL,80h   |                       */
    0x75, 0x02,                   /* JNZ  +2 ------|--> ready              */
    0xE2, 0xF9,                   /* LOOP ---------+                       */
    0xA2, 0x00, 0x2D,             /* MOV [2D00h],AL   the status byte      */
    0x89, 0x0E, 0x02, 0x2D,       /* MOV [2D02h],CX   polls left           */
    0xBA, 0x2A, 0x02,             /* MOV DX,022Ah  DSP read data           */
    0xEC,                         /* IN  AL,DX                             */
    0xA2, 0x04, 0x2D,             /* MOV [2D04h],AL   MUST BE 0xAA         */
    /* -- E1h: the DSP version, the next thing every driver asks ---------- */
    0xBA, 0x2C, 0x02,             /* MOV DX,022Ch  write-buffer status     */
    0xEC,                         /* IN  AL,DX                             */
    0xA2, 0x06, 0x2D,             /* MOV [2D06h],AL   bit 7 clear = ready  */
    0xB0, 0xE1,                   /* MOV AL,E1h                            */
    0xEE,                         /* OUT DX,AL                             */
    0xBA, 0x2A, 0x02,             /* MOV DX,022Ah                          */
    0xEC,                         /* IN  AL,DX                             */
    0xA2, 0x08, 0x2D,             /* MOV [2D08h],AL   major                */
    0xEC,                         /* IN  AL,DX                             */
    0xA2, 0x0A, 0x2D,             /* MOV [2D0Ah],AL   minor                */
    /* -- the AdLib timer test: mask both timers, reset the flags --------- */
    0xBA, 0x88, 0x03,             /* MOV DX,0388h                          */
    0xB0, 0x04, 0xEE,             /* MOV AL,4  / OUT DX,AL   index 4       */
    0xBA, 0x89, 0x03,             /* MOV DX,0389h                          */
    0xB0, 0x60, 0xEE,             /* MOV AL,60h / OUT DX,AL  mask T1+T2    */
    0xBA, 0x88, 0x03,             /* MOV DX,0388h                          */
    0xB0, 0x04, 0xEE,             /* MOV AL,4  / OUT DX,AL                 */
    0xBA, 0x89, 0x03,             /* MOV DX,0389h                          */
    0xB0, 0x80, 0xEE,             /* MOV AL,80h / OUT DX,AL  IRQ reset     */
    0xBA, 0x88, 0x03,             /* MOV DX,0388h                          */
    0xEC,                         /* IN  AL,DX                             */
    0xA2, 0x0C, 0x2D,             /* MOV [2D0Ch],AL   status 1: &E0h == 0  */
    /* -- preset timer 1 to FFh (one 80 us step) and start it ------------- */
    0xB0, 0x02, 0xEE,             /* MOV AL,2  / OUT DX,AL   index 2       */
    0xBA, 0x89, 0x03,             /* MOV DX,0389h                          */
    0xB0, 0xFF, 0xEE,             /* MOV AL,FFh / OUT DX,AL                */
    0xBA, 0x88, 0x03,             /* MOV DX,0388h                          */
    0xB0, 0x04, 0xEE,             /* MOV AL,4  / OUT DX,AL                 */
    0xBA, 0x89, 0x03,             /* MOV DX,0389h                          */
    0xB0, 0x21, 0xEE,             /* MOV AL,21h / OUT DX,AL  start T1      */
    /* -- spin on the status port, which is what the real driver does ----- */
    0xBA, 0x88, 0x03,             /* MOV DX,0388h                          */
    0xB9, 0x00, 0x08,             /* MOV CX,2048                           */
    0xEC,                         /* IN  AL,DX   <-+                       */
    0xE2, 0xFD,                   /* LOOP ---------+                       */
    0xA2, 0x0E, 0x2D,             /* MOV [2D0Eh],AL   status 2: &E0h == C0 */
    /* -- and the flags must CLEAR again, or they are not flags ----------- */
    0xB0, 0x04, 0xEE,             /* MOV AL,4  / OUT DX,AL                 */
    0xBA, 0x89, 0x03,             /* MOV DX,0389h                          */
    0xB0, 0x60, 0xEE,             /* MOV AL,60h / OUT DX,AL  mask both     */
    0xBA, 0x88, 0x03,             /* MOV DX,0388h                          */
    0xB0, 0x04, 0xEE,             /* MOV AL,4  / OUT DX,AL                 */
    0xBA, 0x89, 0x03,             /* MOV DX,0389h                          */
    0xB0, 0x80, 0xEE,             /* MOV AL,80h / OUT DX,AL  IRQ reset     */
    0xBA, 0x88, 0x03,             /* MOV DX,0388h                          */
    0xEC,                         /* IN  AL,DX                             */
    0xA2, 0x10, 0x2D,             /* MOV [2D10h],AL   status 3: &E0h == 0  */
    0xF4,                         /* HLT                                   */
};

static uint32_t snd_r8(uint32_t off)
{
    return (uint32_t)test_ram[SND_RES + off];
}

static void sound_test_run(void)
{
    static const uint8_t hle_vectors[] = { 0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21 };

    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));

    memset(&test_ram[SND_RES], 0, 32);
    memcpy(&test_ram[SND_CODE], sound_code, sizeof(sound_code));
    kbd_test_reset_cpu(SND_CODE);
    snd_r[15] = kbd_test_run_to_halt();

    for (unsigned i = 0; i < 9; i++)
        snd_r[i] = snd_r8(i * 2);
    snd_r[9]  = x86_sb_resets;
    snd_r[10] = x86_sb_cmds;
    snd_r[11] = x86_sb_dma_refused;
    snd_r[12] = x86_opl_writes;
    snd_r[13] = x86_opl_status_reads;
    snd_r[14] = x86_opl_timer_expiry;
}

/* ---------------------------------------------------------------------------
 * SOUND BLASTER DIGITISED PLAYBACK -- the DMA transfer, the terminal count, the
 * IRQ 5 that closes it, and the samples that come out the other end.
 *
 * The arm above proves a card is FOUND. This one proves it PLAYS, and it is
 * written so that no part of that claim rests on a counter alone:
 *
 *   the guest WRITES a 256-byte ramp into its own memory, programs the 8237 to
 *   point at it, programs the DSP for 6,000 Hz 8-bit single-cycle, starts the
 *   transfer, and then WAITS FOR THE INTERRUPT -- which is exactly the wait
 *   that hangs a title today, so if the interrupt does not arrive this arm
 *   spins to its bound and fails rather than quietly passing.
 *
 *   the host then checks the AUDIO. 6,000 Hz into a 48,000 Hz buffer is exactly
 *   eight output samples per source byte, so sample k must be the guest's byte
 *   k/8, decoded from unsigned 8-bit. That single assertion covers the DMA
 *   address walk, the page register, the decode and the rate conversion at
 *   once, and no counter can fake it.
 *
 * The host loop here IS the frame loop in miniature and deliberately so: the
 * engine has no other clock (x86_io.c), so a harness that never rendered would
 * see a transfer that never advances -- which is a real property of the design
 * and not a defect to work around.
 */
#define SBD_CODE    0x3000u
#define SBD_ISR     0x3200u
#define SBD_RES     0x3300u
#define SBD_BUF     0x4000u
#define SBD_FRAMES  64u              /* small on purpose: this harness has
                                         176 BYTES of RAM_EMU slack and a
                                         800-sample buffer overflows it */
#define SBD_RATE    48000u

volatile uint32_t run_sbdma_flag = 0;
volatile uint32_t sbd_r[28];
/* The first 64 rendered samples, PACKED TWO PER WORD. This harness has a few
   hundred bytes of RAM_EMU and an int32 per sample overflows the region -- see
   TEST.ld. Low half is the even sample. */
volatile uint32_t sbd_pcm[32] __attribute__((section(".test_bss")));
/* HOST scratch, not guest memory -- so it belongs in .test_bss (RAM_UC),
   which is exactly the lesson TEST.ld's comment records: RAM_EMU has no
   slack and a new object in front of the 4 KB-aligned test_ram costs a
   whole page. */
static int16_t sbd_audio[SBD_FRAMES] __attribute__((section(".test_bss")));

static const uint8_t sbdma_code[] = {
    /* -- fill 4000h..40FFh with a ramp: the guest's OWN data -------------- */
    0xBF, 0x00, 0x40,             /* MOV DI,4000h                          */
    0xB0, 0x00,                   /* MOV AL,0                              */
    0xB9, 0x00, 0x01,             /* MOV CX,256                            */
    0x88, 0x05,                   /* fill: MOV [DI],AL                     */
    0x47,                         /*       INC DI                          */
    0xFE, 0xC0,                   /*       INC AL                          */
    0xE2, 0xF9,                   /*       LOOP fill                       */
    /* -- DSP reset ------------------------------------------------------- */
    0xBA, 0x26, 0x02,             /* MOV DX,0226h                          */
    0xB0, 0x01, 0xEE,             /* MOV AL,1  / OUT DX,AL                 */
    0xB0, 0x00, 0xEE,             /* MOV AL,0  / OUT DX,AL                 */
    0xBA, 0x2A, 0x02,             /* MOV DX,022Ah                          */
    0xEC,                         /* IN AL,DX                              */
    0xA2, 0x00, 0x33,             /* MOV [3300h],AL   MUST BE 0xAA         */
    /* -- E1h: the version. 4.05 is what puts a title on the SB16 path ---- */
    0xBA, 0x2C, 0x02,             /* MOV DX,022Ch                          */
    0xB0, 0xE1, 0xEE,             /* MOV AL,E1h / OUT DX,AL                */
    0xBA, 0x2A, 0x02,             /* MOV DX,022Ah                          */
    0xEC, 0xA2, 0x01, 0x33,       /* IN AL,DX / MOV [3301h],AL  major = 4  */
    0xEC, 0xA2, 0x02, 0x33,       /* IN AL,DX / MOV [3302h],AL  minor = 5  */
    /* -- the mixer's IRQ and DMA select registers ------------------------ */
    0xBA, 0x24, 0x02,             /* MOV DX,0224h                          */
    0xB0, 0x80, 0xEE,             /* MOV AL,80h / OUT DX,AL   IRQ select   */
    0x42, 0xEC,                   /* INC DX / IN AL,DX                     */
    0xA2, 0x0C, 0x33,             /* MOV [330Ch],AL   expect 02h = IRQ 5   */
    0x4A,                         /* DEC DX                                */
    0xB0, 0x81, 0xEE,             /* MOV AL,81h / OUT DX,AL   DMA select   */
    0x42, 0xEC,                   /* INC DX / IN AL,DX                     */
    0xA2, 0x0D, 0x33,             /* MOV [330Dh],AL   expect 22h = D1 + H5 */
    /* -- install the INT 0Dh (IRQ 5) vector ------------------------------ */
    0xC7, 0x06, 0x34, 0x00, 0x00, 0x32,   /* MOV WORD [0034h],3200h        */
    0xC7, 0x06, 0x36, 0x00, 0x00, 0x00,   /* MOV WORD [0036h],0000h        */
    /* -- unmask IRQ 5 at the 8259, then enable interrupts ---------------- */
    0xE4, 0x21,                   /* IN AL,21h                             */
    0x24, 0xDF,                   /* AND AL,DFh                            */
    0xE6, 0x21,                   /* OUT 21h,AL                            */
    0xFB,                         /* STI                                   */
    /* -- DSP F2h: ASK FOR AN IRQ WITH NO TRANSFER BEHIND IT.
          This is how a driver finds which vector the card is wired to, and it
          is a LATENCY test, not just a delivery test: the window below is TEN
          iterations of a five-instruction loop, about the few hundred
          microseconds a real detect routine allows before it re-masks the
          line. The host loop under this harness only offers IRQ 5 when the
          interpreter EXITS (every 1,200 PIT ticks), so an F2 that is merely
          queued for the next slice boundary CANNOT be seen here -- which is
          the point. PRINCE OF PERSIA's entire DSP conversation is E0 E0 E1 F2
          and then silence, because the answer arrived after it had given up. */
    0xBA, 0x2C, 0x02,             /* MOV DX,022Ch                          */
    0xB0, 0xF2, 0xEE,             /* MOV AL,F2h / OUT DX,AL                */
    0xB9, 0x0A, 0x00,             /* MOV CX,10       the SHORT window      */
    0xA0, 0x08, 0x33,             /* f2w: MOV AL,[3308h]  the ISR's count  */
    0x84, 0xC0,                   /*      TEST AL,AL                       */
    0x75, 0x02,                   /*      JNZ f2done                       */
    0xE2, 0xF7,                   /*      LOOP f2w                         */
    0x89, 0x0E, 0x11, 0x33,       /* f2done: MOV [3311h],CX  polls left    */
    0xA0, 0x08, 0x33,             /* MOV AL,[3308h]                        */
    0xA2, 0x10, 0x33,             /* MOV [3310h],AL   the ISR entry count  */
    0xC6, 0x06, 0x08, 0x33, 0x00, /* MOV BYTE [3308h],0  rearm for the DMA */
    /* -- program 8237 channel 1 for a 256-byte read at 0:4000 ------------ */
    0xB0, 0x05, 0xE6, 0x0A,       /* MOV AL,05h / OUT 0Ah,AL   mask ch1    */
    0xB0, 0x00, 0xE6, 0x0C,       /* MOV AL,00h / OUT 0Ch,AL   clear ff    */
    0xB0, 0x49, 0xE6, 0x0B,       /* MOV AL,49h / OUT 0Bh,AL   single/inc/ */
                                  /*                           read/ch1    */
    0xB0, 0x00, 0xE6, 0x02,       /* MOV AL,00h / OUT 02h,AL   addr lo     */
    0xB0, 0x40, 0xE6, 0x02,       /* MOV AL,40h / OUT 02h,AL   addr hi     */
    0xB0, 0x00, 0xE6, 0x83,       /* MOV AL,00h / OUT 83h,AL   page (ch1!) */
    0xB0, 0xFF, 0xE6, 0x03,       /* MOV AL,FFh / OUT 03h,AL   count-1 lo  */
    0xB0, 0x00, 0xE6, 0x03,       /* MOV AL,00h / OUT 03h,AL   count-1 hi  */
    0xB0, 0x01, 0xE6, 0x0A,       /* MOV AL,01h / OUT 0Ah,AL   unmask ch1  */
    /* -- speaker on, 6,000 Hz, and GO ------------------------------------ */
    0xBA, 0x2C, 0x02,             /* MOV DX,022Ch                          */
    0xB0, 0xD1, 0xEE,             /* MOV AL,D1h / OUT DX,AL   speaker on   */
    0xB0, 0x41, 0xEE,             /* MOV AL,41h / OUT DX,AL   set rate     */
    0xB0, 0x17, 0xEE,             /* MOV AL,17h / OUT DX,AL   6000 >> 8    */
    0xB0, 0x70, 0xEE,             /* MOV AL,70h / OUT DX,AL   6000 & FF    */
    0xB0, 0x14, 0xEE,             /* MOV AL,14h / OUT DX,AL   8-bit single */
    0xB0, 0xFF, 0xEE,             /* MOV AL,FFh / OUT DX,AL   len-1 lo     */
    0xB0, 0x00, 0xEE,             /* MOV AL,00h / OUT DX,AL   len-1 hi     */
    /* -- WAIT FOR THE COMPLETION INTERRUPT. This is the wait that hangs a
          title today; it is BOUNDED so a missing IRQ fails instead of hanging
          the harness, and the residual count says how close it came. ----- */
    0xB9, 0x40, 0x9C,             /* MOV CX,40000                          */
    0xA0, 0x08, 0x33,             /* wait: MOV AL,[3308h]  the ISR's count */
    0x84, 0xC0,                   /*       TEST AL,AL                      */
    0x75, 0x02,                   /*       JNZ done                        */
    0xE2, 0xF7,                   /*       LOOP wait                       */
    0x89, 0x0E, 0x04, 0x33,       /* done: MOV [3304h],CX  polls left      */
    /* -- the 8237's own view: TC for channel 1 in the status register ---- */
    0xE4, 0x08,                   /* IN AL,08h                             */
    0xA2, 0x09, 0x33,             /* MOV [3309h],AL                        */
    /* -- and the DSP's, after the handler acknowledged it ---------------- */
    0xBA, 0x2E, 0x02,             /* MOV DX,022Eh                          */
    0xEC,                         /* IN AL,DX                              */
    0xA2, 0x0A, 0x33,             /* MOV [330Ah],AL                        */
    0xF4,                         /* HLT                                   */
};

/* The IRQ 5 handler. It does the two acknowledgements a real one does -- the
   DSP's (a read of 0x22E) and the 8259's (an EOI) -- and records the mixer's
   IRQ-status byte on the way, which is what an SB16 driver reads to find out
   whether the 8-bit or the 16-bit line fired. */
static const uint8_t sbdma_isr[] = {
    0x50, 0x52,                   /* PUSH AX / PUSH DX                     */
    0xFE, 0x06, 0x08, 0x33,       /* INC BYTE [3308h]                      */
    0xBA, 0x24, 0x02,             /* MOV DX,0224h                          */
    0xB0, 0x82, 0xEE,             /* MOV AL,82h / OUT DX,AL                */
    0x42, 0xEC,                   /* INC DX / IN AL,DX                     */
    0xA2, 0x0B, 0x33,             /* MOV [330Bh],AL   expect bit 0 set     */
    0xBA, 0x2E, 0x02,             /* MOV DX,022Eh                          */
    0xEC,                         /* IN AL,DX      the DSP acknowledge     */
    0xB0, 0x20, 0xE6, 0x20,       /* MOV AL,20h / OUT 20h,AL   the EOI     */
    0x5A, 0x58,                   /* POP DX / POP AX                       */
    0xCF,                         /* IRET                                  */
};

static void sbdma_test_run(void)
{
    /* 0x0D is DELIBERATELY NOT in this list: IRQ 5 must go through the guest's
       own vector, which is the path being graded. */
    static const uint8_t hle_vectors[] = { 0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21 };

    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));

    memset(&test_ram[SBD_RES], 0, 64);
    memset(&test_ram[SBD_BUF], 0, 256);
    memcpy(&test_ram[SBD_CODE], sbdma_code, sizeof(sbdma_code));
    memcpy(&test_ram[SBD_ISR],  sbdma_isr,  sizeof(sbdma_isr));
    for (unsigned i = 0; i < 32; i++)
        sbd_pcm[i] = 0;

    kbd_test_reset_cpu(SBD_CODE);
    cpu_state_dtcm.pit_counter = cpu_state_dtcm.pit_reload = 1200;

    unsigned captured = 0;
    uint32_t reason = 0xFFFFFFFFu;
    int guard;
    for (guard = 0; guard < 600; guard++) {
        x86_cpu_interp_asm();
        uint32_t er = cpu_state_dtcm.exit_reason;
        if (er == X86_EXIT_HLE_INT) {
            cpu_state_dtcm.exit_reason = 0;
            x86_hle_service();
            if (cpu_state_dtcm.exit_reason != 0) {
                reason = cpu_state_dtcm.exit_reason;
                break;
            }
        } else if (er == X86_EXIT_TIMER) {
            cpu_state_dtcm.exit_reason = 0;
            cpu_state_dtcm.pit_counter = 1200;
        } else {
            reason = er;
            break;
        }
        /* THE FRAME LOOP, in miniature: render (which is what advances the
           transfer) and then offer the interrupt, in that order and at every
           boundary -- exactly what src/main.c does. */
        memset(sbd_audio, 0, sizeof(sbd_audio));
        unsigned got = x86_sb_render_mix(sbd_audio, SBD_FRAMES, SBD_RATE, 32767);
        if (got && !captured) {
            for (unsigned i = 0; i < 32; i++)
                sbd_pcm[i] = (uint32_t)(uint16_t)sbd_audio[i * 2] |
                             ((uint32_t)(uint16_t)sbd_audio[i * 2 + 1] << 16);
            captured = 1;
        }
        x86_sb_raise_irq(&cpu_state_dtcm);
    }

    sbd_r[0]  = test_ram[SBD_RES + 0];                  /* the 0xAA        */
    sbd_r[1]  = test_ram[SBD_RES + 1];                  /* version major   */
    sbd_r[2]  = test_ram[SBD_RES + 2];                  /* version minor   */
    sbd_r[3]  = (uint32_t)(test_ram[SBD_RES + 4] |
                           (test_ram[SBD_RES + 5] << 8));   /* polls left  */
    sbd_r[4]  = test_ram[SBD_RES + 8];                  /* ISR entry count */
    sbd_r[5]  = test_ram[SBD_RES + 9];                  /* 8237 status     */
    sbd_r[6]  = test_ram[SBD_RES + 10];                 /* 0x22E, post-ack */
    sbd_r[7]  = test_ram[SBD_RES + 11];                 /* mixer 0x82      */
    sbd_r[8]  = test_ram[SBD_RES + 12];                 /* mixer 0x80      */
    sbd_r[9]  = test_ram[SBD_RES + 13];                 /* mixer 0x81      */
    sbd_r[24] = test_ram[SBD_RES + 0x10];               /* F2: ISR count   */
    sbd_r[25] = (uint32_t)(test_ram[SBD_RES + 0x11] |
                           (test_ram[SBD_RES + 0x12] << 8)); /* polls left */
    sbd_r[26] = x86_sb_irq_requests;
    sbd_r[27] = x86_sb_cmds_unknown;
    sbd_r[10] = x86_sb_starts;
    sbd_r[11] = x86_sb_blocks;
    sbd_r[12] = x86_sb_dma_bytes;
    sbd_r[13] = x86_sb_samples;
    sbd_r[14] = x86_sb_irqs;
    sbd_r[15] = x86_sb_irqs_delivered;
    sbd_r[16] = x86_sb_irqs_acked;
    sbd_r[17] = x86_sb_eois;
    sbd_r[18] = x86_sb_underruns;
    sbd_r[19] = x86_sb_rate;
    sbd_r[20] = x86_sb_dma_maps;
    sbd_r[21] = (uint32_t)guard;
    sbd_r[22] = captured;
    sbd_r[23] = reason;
}


/* ---------------------------------------------------------------------------
 * OPL2 FM SYNTHESIS -- and this arm grades the SAMPLES, not a counter.
 *
 * The guest programs one voice through its own OUT instructions to 0x388 /
 * 0x389, so the whole chain is under test: the port decode in x86_io.c, the
 * forward into x86_opl.c, the register parse, the envelope, the phase
 * generator and the resampler. Then the host loop renders, exactly as
 * src/main.c's audio phase does, and 512 output samples are shipped back for
 * tests/test_sound.py to measure.
 *
 * THE VOICE IS CHOSEN SO THE ANSWER IS ARITHMETIC. Operator 1's total level is
 * 63 (silent), so with CON=0 the carrier is modulated by nothing and the
 * output is a pure sine at the channel's own frequency:
 *
 *     f = f_num * 49716 / 2^(20 - block) = 512 * 49716 / 2^13 = 3107.25 Hz
 *
 * which the test computes from the register bytes IT chose and NOT from a
 * second copy of this core's synthesis. EG-TYPE is 1 (sustaining) so the note
 * holds instead of decaying out from under the measurement, and the release
 * rate is 15 so the key-off half of the arm completes inside twenty frames.
 */
#define FM_CODE      0x3000u
#define FM_CODE_OFF  0x3100u
#define FM_FRAMES    512u
#define FM_RATE      48000u
#define FM_FNUM      0x200u
#define FM_BLOCK     7u

/* emu8950 (lib/emu8950/, vendored BYTE-IDENTICAL -- see its README) names
   free(), puts() and __assert_func() from three paths this build never takes:
   OPL_delete(), its OPL_DEBUG slot dump, and its assert()s. They are satisfied
   HERE rather than edited out of the vendored file, because newlib's free()
   drags in _sbrk and a heap and TEST.ld deliberately provides neither -- the
   bare-metal harness having no heap is a property worth keeping, and patching
   a vendored file is how it stops being the file that was tested on the
   owner's hardware. The CORE build needs none of this: it links against
   retro-go's libc. */
void free(void *p) { (void)p; }
int puts(const char *s) { (void)s; return 0; }
void __assert_func(const char *file, int line, const char *fn, const char *e)
{
    (void)file; (void)line; (void)fn; (void)e;
    for (;;) { }
}

volatile uint32_t run_fm_flag = 0;
volatile uint32_t fm_r[12];
/* Cycles and operator updates for the ONE captured render call; see below. */
volatile uint32_t fm_cycles = 0, fm_slots = 0;
/* 512 samples, PACKED TWO PER WORD, for the same RAM reason sbd_pcm is. */
volatile uint32_t fm_pcm[FM_FRAMES / 2] __attribute__((section(".test_bss")));
static int16_t fm_audio[FM_FRAMES] __attribute__((section(".test_bss")));

/* MOV DX,388h then, per register, MOV AL,reg / OUT DX,AL / MOV AL,val /
   INC DX / OUT DX,AL / DEC DX. Written out rather than looped so the bytes
   below ARE the register list and can be read against the comment. */
static const uint8_t fm_code[] = {
    0xBA, 0x88, 0x03,                                /* MOV DX,0388h       */
    0xB0, 0x01, 0xEE, 0xB0, 0x20, 0x42, 0xEE, 0x4A,  /* 01 <- 20 wavesel   */
    0xB0, 0x20, 0xEE, 0xB0, 0x21, 0x42, 0xEE, 0x4A,  /* 20 <- 21 op1 EGT/M1*/
    0xB0, 0x23, 0xEE, 0xB0, 0x21, 0x42, 0xEE, 0x4A,  /* 23 <- 21 op2 EGT/M1*/
    0xB0, 0x40, 0xEE, 0xB0, 0x3F, 0x42, 0xEE, 0x4A,  /* 40 <- 3F op1 SILENT*/
    0xB0, 0x43, 0xEE, 0xB0, 0x00, 0x42, 0xEE, 0x4A,  /* 43 <- 00 op2 FULL  */
    0xB0, 0x60, 0xEE, 0xB0, 0xF0, 0x42, 0xEE, 0x4A,  /* 60 <- F0 AR15 DR0  */
    0xB0, 0x63, 0xEE, 0xB0, 0xF0, 0x42, 0xEE, 0x4A,  /* 63 <- F0           */
    0xB0, 0x80, 0xEE, 0xB0, 0x0F, 0x42, 0xEE, 0x4A,  /* 80 <- 0F SL0 RR15  */
    0xB0, 0x83, 0xEE, 0xB0, 0x0F, 0x42, 0xEE, 0x4A,  /* 83 <- 0F           */
    0xB0, 0xC0, 0xEE, 0xB0, 0x00, 0x42, 0xEE, 0x4A,  /* C0 <- 00 FB0 CON0  */
    0xB0, 0xA0, 0xEE, 0xB0, 0x00, 0x42, 0xEE, 0x4A,  /* A0 <- 00 fnum lo   */
    0xB0, 0xB0, 0xEE, 0xB0, 0x3E, 0x42, 0xEE, 0x4A,  /* B0 <- 3E KEY ON,   */
                                                     /*    block 7, fnum hi*/
    0xF4,                                            /* HLT                */
};

/* KEY OFF: the same B0h byte with bit 5 clear. */
static const uint8_t fm_code_off[] = {
    0xBA, 0x88, 0x03,                                /* MOV DX,0388h       */
    0xB0, 0xB0, 0xEE, 0xB0, 0x1E, 0x42, 0xEE, 0x4A,  /* B0 <- 1E KEY OFF   */
    0xF4,                                            /* HLT                */
};

static uint32_t fm_render_peak(unsigned frames)
{
    uint32_t peak = 0;
    for (unsigned i = 0; i < frames && i < FM_FRAMES; i++)
        fm_audio[i] = 0;
    x86_opl_render_mix(fm_audio, frames, FM_RATE, 4080);
    for (unsigned i = 0; i < frames && i < FM_FRAMES; i++) {
        int32_t a = fm_audio[i] < 0 ? -fm_audio[i] : fm_audio[i];
        if ((uint32_t)a > peak)
            peak = (uint32_t)a;
    }
    return peak;
}

static void fm_test_run(void)
{
    static const uint8_t hle_vectors[] = { 0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21 };

    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));

    /* Independent of whatever the detection arm left in the chip. */
    x86_opl_reset();
    for (unsigned i = 0; i < FM_FRAMES / 2; i++)
        fm_pcm[i] = 0;

    memcpy(&test_ram[FM_CODE], fm_code, sizeof(fm_code));
    memcpy(&test_ram[FM_CODE_OFF], fm_code_off, sizeof(fm_code_off));

    kbd_test_reset_cpu(FM_CODE);
    fm_r[0] = kbd_test_run_to_halt();

    /* Four frames of warm-up so the attack is over, then ONE captured block.
       Capturing during the attack would grade the envelope's ramp as if it
       were the tone. */
    for (unsigned f = 0; f < 4; f++)
        (void)fm_render_peak(800u > FM_FRAMES ? FM_FRAMES : 800u);

    for (unsigned i = 0; i < FM_FRAMES; i++)
        fm_audio[i] = 0;
    /* THE COST, IN CYCLES, ON WHATEVER THIS IS RUNNING ON. Bracketing the
       render with DWT->CYCCNT is the only way to get a cycles-per-operator
       number that is the DEVICE's rather than the host's: gwemu will report
       something here and it must not be quoted (CLAUDE.md). Divided by the
       operator updates the same call performed, it is the one number the FM
       rate decision rests on -- see docs/03 4g. */
    {
        uint32_t s0 = x86_opl_slots_run;
        uint32_t c0 = DWT_CYCCNT;
        fm_r[1] = x86_opl_render_mix(fm_audio, FM_FRAMES, FM_RATE, 4080);
        fm_cycles = DWT_CYCCNT - c0;
        fm_slots = x86_opl_slots_run - s0;
    }
    {
        uint32_t peak = 0;
        for (unsigned i = 0; i < FM_FRAMES; i++) {
            int32_t a = fm_audio[i] < 0 ? -fm_audio[i] : fm_audio[i];
            if ((uint32_t)a > peak)
                peak = (uint32_t)a;
        }
        fm_r[2] = peak;
        for (unsigned i = 0; i < FM_FRAMES / 2; i++)
            fm_pcm[i] = (uint32_t)(uint16_t)fm_audio[i * 2] |
                        ((uint32_t)(uint16_t)fm_audio[i * 2 + 1] << 16);
    }
    fm_r[3] = (uint32_t)x86_opl_active();

    /* KEY OFF, then twenty frames for a release rate of 15 to finish. */
    kbd_test_reset_cpu(FM_CODE_OFF);
    fm_r[4] = kbd_test_run_to_halt();
    for (unsigned f = 0; f < 20; f++)
        (void)fm_render_peak(FM_FRAMES);
    fm_r[5] = fm_render_peak(FM_FRAMES);
    fm_r[6] = (uint32_t)x86_opl_active();

    fm_r[7]  = x86_opl_keyons;
    fm_r[8]  = x86_opl_chip_steps;
    fm_r[9]  = x86_opl_slots_run;
    fm_r[10] = x86_opl_rhythm_muted;
    fm_r[11] = x86_opl_writes;
}

static void kbd_test_run(void)
{
    static const uint8_t hle_vectors[] = {
        0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21,
    };

    memset(&test_ram[KBD_RES_BASE], 0, 32);
    /* x86_bios_init() puts the renderer into mode 3, which needs a VRAM
       pointer -- the real core calls x86_video_init() first and so must this. */
    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));

    memcpy(&test_ram[KBD_CODE_BASE], kbd_guest_code, sizeof(kbd_guest_code));
    memcpy(&test_ram[KBD_PHASE_B], kbd_guest_code_b, sizeof(kbd_guest_code_b));
    memcpy(&test_ram[KBD_PHASE_C], kbd_guest_code_c, sizeof(kbd_guest_code_c));

    /* What a freshly initialised BDA says. Zero here means the first key is
       written over the IVT and INT 16h compares head==tail forever. */
    kbd_r[8] = kbd_bda_r16(0x1A);
    kbd_r[9] = kbd_bda_r16(0x1C);

    /* Phase A: nothing buffered. */
    kbd_test_reset_cpu(KBD_CODE_BASE);
    kbd_r[19] = 0xC0DE0003;   /* progress marker: a hang must say WHERE */
    kbd_r[16] = kbd_test_run_to_halt();

    /* Inject a key exactly as INT 09h does: a scancode on the controller,
       then the BIOS keyboard interrupt handler. 0x1C is Enter. */
    x86_kbd_scancode(0x1C);
    kbd_r[10] = (uint32_t)x86_bios_kbd_irq();
    kbd_r[11] = kbd_bda_r16(0x1A);
    kbd_r[12] = kbd_bda_r16(0x1C);
    /* The word the producer left in the ring, read straight out of guest
       memory at the offset the head points to -- head is an offset within
       segment 0x40, not an index. */
    kbd_r[13] = (uint16_t)(test_ram[0x400 + kbd_r[11]] |
                           (test_ram[0x401 + kbd_r[11]] << 8));
    /* Drain the break code the controller still holds, so it cannot leak into
       a later phase. */
    x86_kbd_scancode(0x9C);
    x86_bios_kbd_irq();

    /* Phase B: peek, peek again, read, peek empty. */
    kbd_test_reset_cpu(KBD_PHASE_B);
    kbd_r[19] = 0xC0DE0004;
    kbd_r[17] = kbd_test_run_to_halt();

    /* Phase C: walk head and tail to the LAST entry of the ring so the push
       and the pop each have to wrap back to 0x1E. */
    kbd_bda_w16(0x1A, 0x3C);
    kbd_bda_w16(0x1C, 0x3C);
    x86_kbd_scancode(0x39);          /* Space */
    x86_bios_kbd_irq();
    kbd_r[14] = kbd_bda_r16(0x1C);   /* tail must have wrapped to 0x1E */
    kbd_r[15] = (uint16_t)(test_ram[0x400 + 0x3C] |
                           (test_ram[0x401 + 0x3C] << 8));
    x86_kbd_scancode(0xB9);
    x86_bios_kbd_irq();
    kbd_test_reset_cpu(KBD_PHASE_C);
    kbd_r[19] = 0xC0DE0005;
    kbd_r[18] = kbd_test_run_to_halt();
    kbd_r[19] = kbd_bda_r16(0x1A);   /* head must have wrapped to 0x1E */

    /* The seven words the guest stored. */
    for (int i = 0; i < 8; i++)
        kbd_r[i] = (uint16_t)(test_ram[KBD_RES_BASE + i * 2] |
                              (test_ram[KBD_RES_BASE + i * 2 + 1] << 8));
}


/* --------------------------------------------------------------------------
 * On-screen keyboard.
 *
 * Driven exactly as src/main.c drives it: a button BITMASK per frame, then the
 * BIOS keyboard interrupt for every code the controller holds -- so this covers
 * the OSK, x86_kbd_scancode(), the 8042 queue and INT 09h's shift decode in one
 * chain. Nothing is asserted against the OSK's own state; the answers come out
 * of the guest's BDA ring, which is where a program looks.
 */
static void osk_drain_irqs(void)
{
    /* Bounded: a controller that never empties must fail, not hang. */
    for (int guard = 0; guard < 32 && x86_kbd_pending(); guard++)
        x86_bios_kbd_irq();
}

static uint16_t osk_ring_pop(void)
{
    uint16_t head = kbd_bda_r16(0x1A);
    if (head == kbd_bda_r16(0x1C))
        return 0xFFFF;                  /* empty */
    uint16_t w = (uint16_t)(test_ram[0x400 + head] |
                            (test_ram[0x401 + head] << 8));
    head = (uint16_t)(head + 2);
    if (head >= 0x3E)
        head = 0x1E;
    kbd_bda_w16(0x1A, head);
    return w;
}

/* One frame of buttons, then whatever the controller produced. */
static uint16_t osk_frame(uint32_t buttons)
{
    x86_osk_input(buttons);
    osk_drain_irqs();
    return osk_ring_pop();
}

static void osk_test_run(void)
{
    static const uint8_t hle_vectors[] = {
        0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21,
    };
    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));
    x86_osk_reset();

    /* The layout's own arithmetic. draw_glyph() clips silently, so an
       over-long row loses its OFF key with no fault to notice. */
    osk_r[0] = x86_osk_row_cells_max;
    osk_r[1] = x86_osk_row_overflow;

    /* GAME opens it. The press is consumed by the transition -- the frame the
       OSK opens on emits nothing. */
    osk_r[2] = (uint32_t)x86_osk_visible();      /* expect 0 */
    x86_osk_cycle();
    osk_r[3] = (uint32_t)x86_osk_visible();      /* expect 1 */

    /* Layer 0, row 0, column 0 is 'a'. A presses it. Two frames: the release
       of the buttons has to be seen or the next press is not an edge. */
    (void)osk_frame(0);
    osk_r[4] = osk_frame(X86_OSK_A);             /* expect 0x1E1E ('a')      */
    (void)osk_frame(0);

    /* RIGHT then A is 'b' -- navigation, not just the first cell. */
    (void)osk_frame(X86_OSK_RIGHT);
    (void)osk_frame(0);
    osk_r[5] = osk_frame(X86_OSK_A);             /* expect 0x3062 ('b')      */
    (void)osk_frame(0);

    /* B is Backspace wherever the cursor is. */
    osk_r[6] = osk_frame(X86_OSK_B);             /* expect 0x0E08            */
    (void)osk_frame(0);

    /* TIME cycles the layer and DOWN flips the row, neither of which moves the
       column -- so the cursor is still on column 1 from the 'b' press above,
       and NINE more RIGHTs reach column 10. That cell is '!', a SHIFTED one:
       the BDA must show the shifted ASCII, which is only possible if a real
       Left Shift make reached INT 09h ahead of the key. (Ten RIGHTs lands on
       '@' -- also shifted, and it passed, which is exactly why the column is
       spelled out here rather than counted from zero in a comment.) */
    (void)osk_frame(X86_OSK_TIME);
    (void)osk_frame(0);
    (void)osk_frame(X86_OSK_DOWN);
    (void)osk_frame(0);
    for (int i = 0; i < 9; i++) {
        (void)osk_frame(X86_OSK_RIGHT);
        (void)osk_frame(0);
    }
    osk_r[7] = osk_frame(X86_OSK_A);             /* expect 0x0221 ('!')      */
    osk_r[8] = (uint32_t)test_ram[0x400 + 0x17]; /* BDA shift flags: 0 again */
    (void)osk_frame(0);

    /* Drawing must not touch the guest's framebuffer, in either mode, and must
       put ink in the bars. Rendered into the harness's own output buffer. */
    memset(cga_test_out, 0, 320 * 240);
    {
        uint32_t vram_before = 0, vram_after = 0;
        for (unsigned i = 0; i < X86_VRAM_BYTES; i += 64)
            vram_before += cga_test_vram[i];
        x86_video_render(cga_test_out, 320);
        x86_osk_draw(cga_test_out, 320);
        for (unsigned i = 0; i < X86_VRAM_BYTES; i += 64)
            vram_after += cga_test_vram[i];
        osk_r[9] = (vram_before == vram_after) ? 1 : 0;

        uint32_t top = 0, bottom = 0, picture = 0;
        for (unsigned y = 0; y < 20; y++)
            for (unsigned x = 0; x < 320; x++)
                top += cga_test_out[y * 320 + x] ? 1 : 0;
        for (unsigned y = 220; y < 240; y++)
            for (unsigned x = 0; x < 320; x++)
                bottom += cga_test_out[y * 320 + x] ? 1 : 0;
        for (unsigned y = 20; y < 220; y++)
            for (unsigned x = 0; x < 320; x++)
                picture += cga_test_out[y * 320 + x] ? 1 : 0;
        osk_r[10] = top;
        osk_r[11] = bottom;
        osk_r[12] = picture;
    }

    /* GAME again: mouse mode still owns the buttons; a third closes it. */
    x86_osk_cycle();
    osk_r[13] = x86_osk_mode;                    /* expect 2 (mouse)         */
    osk_r[14] = (uint32_t)x86_osk_visible();     /* expect 1                 */
    x86_osk_cycle();
    osk_r[15] = (uint32_t)x86_osk_visible();     /* expect 0                 */

    /* ---- IRQ 1 arming, which is what decides WHEN a cell press is visible.
     *
     * The grid checks above prove a code reaches the guest's ring; they cannot
     * see the defect that made an OSK keypress take SECONDS, because they call
     * x86_bios_kbd_irq() themselves instead of going through the frame loop's
     * arming rule. That rule is x86_kbd_raise_irq(), and both of its guards
     * were measured failures under gwemu -- see the comment on the definition.
     *
     * The IF=0 case is the one that mattered: the guest is inside its own INT
     * 09h handler, so pulling the instruction boundary in cannot deliver
     * anything and only ends the slice. Doing it once a frame ran the guest at
     * one instruction per frame and the OSK's codes piled up in the queue
     * until the keyboard was closed. */
    while (x86_kbd_pending())
        (void)x86_bios_kbd_irq();

    cpu_state_dtcm.hard_int_pending = 0;
    cpu_state_dtcm.pit_counter = 0x5A5A;
    cpu_state_dtcm.eflags = 0x0002;              /* IF CLEAR */
    x86_kbd_scancode(0x1E);
    osk_r[16] = (uint32_t)x86_kbd_raise_irq(&cpu_state_dtcm);
    osk_r[17] = cpu_state_dtcm.pit_counter;      /* expect 0x5A5A, untouched */

    /* The one-deep pending slot belongs to whoever got there first. An
       unguarded store here discarded the 18.2 Hz tick every time a key was
       held. */
    cpu_state_dtcm.hard_int_pending = 0x08;
    osk_r[18] = (uint32_t)((x86_kbd_raise_irq(&cpu_state_dtcm) << 8) |
                           cpu_state_dtcm.hard_int_pending);  /* expect 0x08 */

    /* With IF set the boundary CAN deliver, so it is pulled in -- the property
       that makes a keypress land this frame rather than at the end of a slice. */
    cpu_state_dtcm.hard_int_pending = 0;
    cpu_state_dtcm.pit_counter = 0x5A5A;
    cpu_state_dtcm.eflags = 0x0202;              /* IF SET */
    osk_r[19] = (uint32_t)((x86_kbd_raise_irq(&cpu_state_dtcm) << 16) |
                           (cpu_state_dtcm.hard_int_pending << 8) |
                           (cpu_state_dtcm.pit_counter == 1u));  /* 0x10901 */

    while (x86_kbd_pending())
        (void)x86_bios_kbd_irq();
}

/* --------------------------------------------------------------------------
 * DOS file I/O, from guest code.
 *
 * Every call goes through INT 21h the way a DOS program issues it, so the CF
 * and AX the test reads are the ones the guest's IRET actually delivered --
 * calling x86_dos_int21() from the host would miss the whole flags-on-stack
 * half of the contract, which is where "file not found" lives.
 */
#define DOSF_CODE   0x3000u
#define DOSF_STR    0x4000u
#define DOSF_RES    0x4100u     /* 24 result words                          */
#define DOSF_NAME1  0x4900u     /* FindFirst's name, copied out of the DTA   */
#define DOSF_NAME2  0x4910u     /* FindNext's                                */
#define DOSF_DTA    0x4800u
#define DOSF_BUF    0x5000u     /* whole-file read                           */
#define DOSF_BUF3   0x6100u     /* 16 bytes read after a seek                */

static const uint8_t dosfile_guest_code[] = {
    /* open TEST.INI, AL=0 read-only */
    0xB8, 0x00, 0x3D,  0xBA, 0x00, 0x40,  0xCD, 0x21,
    0x89, 0xC3,                       /* handle -> BX, before FLAGS eats AX */
    0xA3, 0x00, 0x41,  0x9C, 0x58, 0xA3, 0x02, 0x41,
    /* read 0x400 bytes: the file is shorter, so this is the EOF short read */
    0xB4, 0x3F,  0xB9, 0x00, 0x04,  0xBA, 0x00, 0x50,  0xCD, 0x21,
    0xA3, 0x04, 0x41,  0x9C, 0x58, 0xA3, 0x06, 0x41,
    /* read again, now AT eof: 0 bytes and CF CLEAR, not an error */
    0xB4, 0x3F,  0xB9, 0x00, 0x04,  0xBA, 0x00, 0x60,  0xCD, 0x21,
    0xA3, 0x08, 0x41,  0x9C, 0x58, 0xA3, 0x0A, 0x41,
    /* seek to 0x0100 from the start */
    0xB8, 0x00, 0x42,  0xB9, 0x00, 0x00,  0xBA, 0x00, 0x01,  0xCD, 0x21,
    0xA3, 0x0C, 0x41,  0x89, 0x16, 0x0E, 0x41,
    /* 16 bytes from there */
    0xB4, 0x3F,  0xB9, 0x10, 0x00,  0xBA, 0x00, 0x61,  0xCD, 0x21,
    0xA3, 0x10, 0x41,
    /* seek to end: DX:AX is the file size */
    0xB8, 0x02, 0x42,  0xB9, 0x00, 0x00,  0xBA, 0x00, 0x00,  0xCD, 0x21,
    0xA3, 0x12, 0x41,  0x89, 0x16, 0x14, 0x41,
    /* close */
    0xB4, 0x3E,  0xCD, 0x21,  0x9C, 0x58, 0xA3, 0x16, 0x41,
    /* open a file that is not there: CF set, AX = 02 */
    0xB8, 0x00, 0x3D,  0xBA, 0x10, 0x40,  0xCD, 0x21,
    0xA3, 0x18, 0x41,  0x9C, 0x58, 0xA3, 0x1A, 0x41,
    /* AH=43h get attributes */
    0xB8, 0x00, 0x43,  0xBA, 0x00, 0x40,  0xCD, 0x21,
    0x89, 0x0E, 0x1C, 0x41,  0x9C, 0x58, 0xA3, 0x1E, 0x41,
    /* DTA, then FindFirst "*.INI" */
    0xB4, 0x1A,  0xBA, 0x00, 0x48,  0xCD, 0x21,
    0xB8, 0x00, 0x4E,  0xB9, 0x00, 0x00,  0xBA, 0x20, 0x40,  0xCD, 0x21,
    0xA3, 0x20, 0x41,  0x9C, 0x58, 0xA3, 0x22, 0x41,
    0xBE, 0x1E, 0x48,  0xBF, 0x00, 0x49,  0xB9, 0x0D, 0x00, 0xFC, 0xF3, 0xA4,
    /* FindNext */
    0xB4, 0x4F,  0xCD, 0x21,
    0xA3, 0x24, 0x41,  0x9C, 0x58, 0xA3, 0x26, 0x41,
    0xBE, 0x1E, 0x48,  0xBF, 0x10, 0x49,  0xB9, 0x0D, 0x00, 0xFC, 0xF3, 0xA4,
    /* the same file behind a drive letter: "A:TEST.INI" */
    0xB8, 0x00, 0x3D,  0xBA, 0x30, 0x40,  0xCD, 0x21,
    0x89, 0xC3,  0xA3, 0x28, 0x41,  0x9C, 0x58, 0xA3, 0x2A, 0x41,
    0xB4, 0x3E,  0xCD, 0x21,
    /* and in lower case: "test.ini" */
    0xB8, 0x00, 0x3D,  0xBA, 0x40, 0x40,  0xCD, 0x21,
    0x89, 0xC3,  0xA3, 0x2C, 0x41,  0x9C, 0x58, 0xA3, 0x2E, 0x41,
    0xB4, 0x3E,  0xCD, 0x21,
    /*
     * The same FindFirst/FindNext again, with the DTA STRADDLING a 4 KB
     * granule boundary (0x6FE0: the 43-byte result record ends at 0x700B).
     *
     * This is not a corner case dressed up as one. x86_guest_ptr() refuses any
     * span that crosses a granule, dos_find_report() asked it for all 43 bytes
     * at once, and a refusal there was indistinguishable from "no more files"
     * -- so a program whose DTA happens to sit near the end of a page sees an
     * EMPTY DISK. WOLF3D's DTA is at DS:0xFFBA and that is exactly what
     * happened to its findfirst("*.WL1").
     */
    0xB4, 0x1A,  0xBA, 0xE0, 0x6F,  0xCD, 0x21,
    0xB8, 0x00, 0x4E,  0xB9, 0x00, 0x00,  0xBA, 0x20, 0x40,  0xCD, 0x21,
    0xA3, 0x30, 0x41,  0x9C, 0x58, 0xA3, 0x32, 0x41,
    0xBE, 0xFE, 0x6F,  0xBF, 0x20, 0x49,  0xB9, 0x0D, 0x00, 0xFC, 0xF3, 0xA4,
    0xB4, 0x4F,  0xCD, 0x21,
    0xA3, 0x34, 0x41,  0x9C, 0x58, 0xA3, 0x36, 0x41,
    0xBE, 0xFE, 0x6F,  0xBF, 0x30, 0x49,  0xB9, 0x0D, 0x00, 0xFC, 0xF3, 0xA4,
    /*
     * AH=42h on handle 0 -- a CHARACTER DEVICE, not a file. DOS succeeds with
     * DX:AX = 0; it does not report "invalid handle". 3Eh/3Fh/40h/44h all
     * route through dos_handle_device() and 42h did not, so a seek on stdin
     * failed, and a program that treats that as retryable never leaves the
     * retry. WOLF3D issued 81 in a row.
     */
    0xB8, 0x00, 0x42,  0xBB, 0x00, 0x00,  0xB9, 0x00, 0x00,  0xBA, 0x00, 0xFC,
    0xCD, 0x21,
    0xA3, 0x38, 0x41,  0x89, 0x16, 0x3A, 0x41,  0x9C, 0x58, 0xA3, 0x3C, 0x41,
    0xF4,                                                       /* HLT */
};

static FATFS dosfile_fs;

/* --------------------------------------------------------------------------
 * AH=52h (list of lists) and AH=29h (parse filename into FCB), from GUEST code.
 *
 * Both are pointer-producing calls, and that is why the guest has to drive
 * them. AH=52h's answer is a FAR POINTER in ES:BX and the only thing that
 * proves it is a real one is dereferencing it -- so the guest reads the first
 * MCB segment out of ES:BX-2 and then WALKS THE CHAIN from it, with DS pointed
 * at each header in turn. The host walks the same chain independently and the
 * two must agree; a host-side call to x86_dos_int21() would never touch ES at
 * all. AH=29h's answer is half in AL and half in an ADVANCED SI, and a caller
 * that parses argument after argument off one line depends entirely on the
 * second half, so every case records both.
 */
#define DOSS_CODE   0x3000u
#define DOSS_STR    0x4000u     /* the command lines, planted by the host  */
#define DOSS_RES    0x4100u     /* 7 words from AH=52h, then 9 parse pairs */
#define DOSS_PRES   0x4110u     /* per-case (AL word, SI word)             */
#define DOSS_FCB    0x4200u     /* eight 16-byte FCBs                      */
#define DOSS_SPLIT  0x4FF8u     /* a ninth, STRADDLING the 4 KB granule    */
#define DOSS_SYS    0x4400u     /* the whole SysVars block, copied by guest */
#define DOSS_SYS_BYTES 0x70u
#define DOSS_CASES  9u

/* One AH=29h case: DS:SI = string, ES:DI = FCB, AL = parse control. */
#define DOSS_PARSE(str, fcb, al, res)                                     \
    0xBE, (uint8_t)(str), (uint8_t)((str) >> 8),        /* mov si,str  */ \
    0xBF, (uint8_t)(fcb), (uint8_t)((fcb) >> 8),        /* mov di,fcb  */ \
    0xB8, (al), 0x29,                                   /* mov ax,29xx */ \
    0xCD, 0x21,                                         /* int 21h     */ \
    0xA2, (uint8_t)(res), (uint8_t)((res) >> 8),        /* mov [res],al */ \
    0x89, 0x36, (uint8_t)((res) + 2), (uint8_t)(((res) + 2) >> 8)

static const uint8_t dossys_guest_code[] = {
    /* ---- AH=52h. Record BX, ES and the returned FLAGS. ---- */
    0xB8, 0x00, 0x52,                       /* mov ax,5200h            */
    0xCD, 0x21,                             /* int 21h                 */
    0x9C, 0x58, 0xA3, 0x04, 0x41,           /* pushf/pop ax/mov [4104],ax */
    0x89, 0x1E, 0x00, 0x41,                 /* mov [4100h],bx          */
    0x8C, 0xC0,  0xA3, 0x02, 0x41,          /* mov ax,es/mov [4102],ax */

    /*
     * Copy the whole block, ES:BX-0x22 for 0x70 bytes, down to DS:4400 so the
     * HOST can check every field against the reference layout. MOVSB's
     * destination is ES:DI and cannot be overridden, so DS and ES are swapped
     * for the copy rather than an override being used on the source.
     */
    0x8C, 0xC0,                             /* mov ax,es  (SysVars seg) */
    0x31, 0xD2,                             /* xor dx,dx               */
    0x8E, 0xC2,                             /* mov es,dx  (ES = 0)     */
    0x8E, 0xD8,                             /* mov ds,ax  (DS = SysVars) */
    0x89, 0xDE,                             /* mov si,bx               */
    0x81, 0xEE, 0x22, 0x00,                 /* sub si,22h              */
    0xBF, 0x00, 0x44,                       /* mov di,4400h            */
    0xB9, 0x70, 0x00,                       /* mov cx,70h              */
    0xFC,  0xF3, 0xA4,                      /* cld / rep movsb         */
    0x31, 0xC0,  0x8E, 0xD8,                /* xor ax,ax / mov ds,ax   */

    /* ---- Walk the MCB chain from the copied firstMCB word (block +0x20). */
    0xA1, 0x20, 0x44,                       /* mov ax,[4420h]          */
    0xA3, 0x06, 0x41,                       /* mov [4106h],ax          */
    0x89, 0xC5,                             /* mov bp,ax   (cursor)    */
    0x31, 0xC9,                             /* xor cx,cx   (count)     */
    0x31, 0xFF,                             /* xor di,di   (last type) */
    /* .walk: */
    0x8E, 0xDD,                             /* mov ds,bp               */
    0x31, 0xF6,                             /* xor si,si               */
    0x8A, 0x04,                             /* mov al,[si]   type      */
    0x8B, 0x54, 0x03,                       /* mov dx,[si+3] size      */
    0x41,                                   /* inc cx                  */
    0xB4, 0x00,  0x89, 0xC7,                /* mov ah,0 / mov di,ax    */
    0x3C, 0x5A,  0x74, 0x0C,                /* cmp al,'Z' / je .done   */
    0x3C, 0x4D,  0x75, 0x08,                /* cmp al,'M' / jne .done  */
    0x01, 0xD5,  0x45,                      /* add bp,dx / inc bp      */
    0x83, 0xF9, 0x40,  0x72, 0xE2,          /* cmp cx,64 / jb .walk    */
    /* .done: */
    0x31, 0xC0,  0x8E, 0xD8,                /* xor ax,ax / mov ds,ax   */
    0x89, 0x0E, 0x08, 0x41,                 /* mov [4108h],cx  blocks  */
    0x89, 0x2E, 0x0A, 0x41,                 /* mov [410Ah],bp  last seg */
    0x89, 0x3E, 0x0C, 0x41,                 /* mov [410Ch],di  last type */

    /* ---- AH=29h. Nine cases; the strings are at DOSS_STR, planted by the
     *      host, and every expected answer is computed there too.
     *
     *  0  "TEST.INI"          plain
     *  1  "*.TXT"             star expansion, AL=1
     *  2  "B:FOO.BAR baz"     drive letter, and a space terminates
     *  3  " ;X.Y"   AL=01     bit 0 scans ONE leading separator off
     *  4  " ;X.Y"   AL=00     ... and without it the ';' stops the parse dead
     *  5  "1:FOO.TXT"         a non-alphabetic drive: AL=0FFh
     *  6  "VERYLONGNAME.EXT"  the deliberate over-long-name behaviour
     *  7  ";"       AL=0Eh    every "leave the field" bit, over a filled FCB
     *  8  "TEST.INI"          the same as case 0 with the FCB STRADDLING 4 KB
     */
    DOSS_PARSE(0x4000, 0x4200, 0x00, 0x4110),
    DOSS_PARSE(0x4010, 0x4210, 0x00, 0x4114),
    DOSS_PARSE(0x4020, 0x4220, 0x00, 0x4118),
    DOSS_PARSE(0x4030, 0x4230, 0x01, 0x411C),
    DOSS_PARSE(0x4030, 0x4240, 0x00, 0x4120),
    DOSS_PARSE(0x4040, 0x4250, 0x00, 0x4124),
    DOSS_PARSE(0x4050, 0x4260, 0x00, 0x4128),
    DOSS_PARSE(0x4070, 0x4270, 0x0E, 0x412C),
    DOSS_PARSE(0x4000, 0x4FF8, 0x00, 0x4130),

    0xF4,                                                       /* HLT */
};

volatile uint32_t run_dossys_flag = 0;
volatile uint32_t dossys_r[8];

static void dossys_test_run(void)
{
    static const uint8_t hle_vectors[] = { 0x20, 0x21 };

    for (int i = 0; i < 8; i++)
        dossys_r[i] = 0;
    memset(&test_ram[DOSS_STR], 0, 0x100);
    memset(&test_ram[DOSS_RES], 0, 0x100);
    memset(&test_ram[DOSS_FCB], 0, 0x100);
    memset(&test_ram[DOSS_SYS], 0, DOSS_SYS_BYTES);
    memset(&test_ram[DOSS_SPLIT], 0, 16);

    x86_hle_install(hle_vectors, sizeof(hle_vectors));
    x86_dos_arena_init();

    /* Two allocations, so the chain the guest walks has more than one link and
       "walked to the end" is a real claim rather than a single 'Z' header. */
    uint16_t largest = 0;
    dossys_r[2] = dos_test_alloc(0x100, &largest);
    dossys_r[3] = dos_test_alloc(0x200, &largest);

    memcpy(&test_ram[DOSS_STR + 0x00], "TEST.INI", 9);
    memcpy(&test_ram[DOSS_STR + 0x10], "*.TXT", 6);
    memcpy(&test_ram[DOSS_STR + 0x20], "B:FOO.BAR baz", 14);
    memcpy(&test_ram[DOSS_STR + 0x30], " ;X.Y", 6);
    memcpy(&test_ram[DOSS_STR + 0x40], "1:FOO.TXT", 10);
    memcpy(&test_ram[DOSS_STR + 0x50], "VERYLONGNAME.EXT", 17);
    memcpy(&test_ram[DOSS_STR + 0x70], ";", 2);

    /* Case 7's FCB is pre-filled: with AL=0Eh nothing in the first twelve
       bytes may change, and the two words after them must be zeroed anyway. */
    memset(&test_ram[DOSS_FCB + 7 * 16], 0x5A, 16);

    memcpy(&test_ram[DOSS_CODE], dossys_guest_code, sizeof(dossys_guest_code));

    uint32_t sv0 = x86_dos_sysvars_calls, p0 = x86_dos_parse_calls;
    kbd_test_reset_cpu(DOSS_CODE);
    dossys_r[0] = kbd_test_run_to_halt();
    dossys_r[1] = X86_DOS_ARENA_SEG;        /* what -2 has to come back as */
    dossys_r[4] = x86_dos_sysvars_calls - sv0;
    dossys_r[5] = x86_dos_parse_calls - p0;
    dossys_r[6] = x86_xms_total_kb;
    dossys_r[7] = (uint32_t)sizeof(dossys_guest_code);
}

/* ---------------------------------------------------------------------- *
 * NOR cold-page backing, driven by GUEST code.
 *
 * The "NOR" here is the extflash window at 0x90000000 itself, written with
 * plain stores. Under gwemu that region is real backing memory, which is
 * exactly what makes this a tier-1 test: no flash programming, no OCTOSPI, no
 * SD card and no firmware -- but the ADDRESS is genuine, so bit 31 is set and
 * the interpreter's fold check traps for the real reason it will on silicon.
 * A buffer in .bss would have bit 31 CLEAR and would never trap at all, so it
 * would test nothing.
 *
 * The guest program is what exercises it. Reading the counters after a host-C
 * memcpy would only prove that C works; a read served out of NOR and a write
 * that pulls a page back into AXI are properties of the trap stubs, and only
 * executing guest instructions can show them.
 * ---------------------------------------------------------------------- */
#define NOR_TEST_PAGES  8u
#define NOR_TEST_BYTES  (NOR_TEST_PAGES * 4096u)
#define NOR_TEST_FIRST  200u                       /* guest page, clear of test_ram */
#define NOR_TEST_SEG    (NOR_TEST_FIRST * 4096u / 16u)   /* 0xC800 */
#define NOR_FAKE_BASE   ((uint8_t *)0x90000000u)

/* A window for the NOR-TIER EVICTION arm (section 4 below). Distinct from the
   COW window above: that one is x86_nor.c's, this one is the pager's, and
   sharing them would make a failure ambiguous between two mechanisms. */
#define NORPG_FIRST     (0xC0000u >> 12)
#define NORPG_PAGES     4u
#define NORPG_MEM_OFF   (700u * 1024u)

/* HARNESS-ONLY: a RAM stand-in for the NOR part, 32 KB. It belongs in
   .test_bss (RAM_UC, the firmware's framebuffer pool, which the bare-metal
   harness never uses) for the same reason the video test's buffers do — it is
   the HOST's scratch, not guest memory, and leaving it in RAM_EMU spends space
   the pager's frames are measured against. RAM_EMU overflowed by 176 bytes
   when a session's work landed together; this is where the slack lives. */
static uint8_t nor_test_ram[NOR_TEST_BYTES]
    __attribute__((aligned(4096), section(".test_bss")));

static uint8_t nor_pattern_byte(uint32_t off)
{
    return (uint8_t)(off * 7u + (off >> 8) * 31u + 1u);   /* never 0, never uniform */
}

/* ---------------------------------------------------------------------- *
 * The WATCHDOG HEARTBEAT across x86_nor_attach()'s readback.
 *
 * Separate arm, separate flag, separate buffer, and deliberately NOT built on
 * NOR_FAKE_BASE. nor_test_run() below cannot run in this firmware at all: it
 * opens by storing to 0x90000000, nothing here configures OCTOSPI
 * memory-mapped mode, and the store takes a precise bus fault (BFAR
 * 0x90000000) into the spin in Fault_Handler_C -- which is why test_nor.py
 * reports "target never ran" rather than a failure. That is a pre-existing
 * defect in the harness's premise, and hanging a NEW check off it would make
 * the new check untestable for an unrelated reason.
 *
 * What this arm needs is not the TRAP behaviour but the READBACK LOOP, and
 * that runs against any pointer. So "NOR" here is an ordinary aligned .bss
 * buffer: bit 31 is clear, nothing traps, and the loop that has to kick the
 * watchdog once per page is exercised exactly as it is on silicon.
 *
 * The property under test: on silicon this loop is 160 memcmps of 4 KB each
 * out of memory-mapped OCTOSPI, it is the one stretch of the snapshot that
 * neither the flash driver nor the SD driver kicks the watchdog inside of
 * (gw_flash.c's OSPI_Program kicks per 256-byte page and says why: the ~472 ms
 * WWDG window), and the frame loop is not running because the snapshot is a
 * menu callback. A hook that is silently never called is the failure that
 * matters, so the COUNT is what is asserted.
 * ---------------------------------------------------------------------- */
/* NO NEW .bss. RAM_EMU in this harness has under 4 KB of headroom -- a pair of
   fresh 32 KB buffers overflows the region by 61,616 bytes and the link fails.
   nor_test_ram is already allocated, already 4096-aligned, and is 8 pages; it
   is split down the middle to play both sides here. 16384 is a multiple of
   4096, so the "NOR" half is aligned too, which x86_nor_attach() checks. */
#define NORWD_PAGES  4u
#define NORWD_BYTES  (NORWD_PAGES * 4096u)
#define NORWD_FIRST  220u              /* guest page, clear of the other arms */

#define norwd_ram  (nor_test_ram)
#define norwd_nor  (nor_test_ram + NORWD_BYTES)

static uint32_t norwd_hook_calls;
static void norwd_hook(void) { norwd_hook_calls++; }

static void norwd_test_run(void)
{
    for (uint32_t i = 0; i < 12; i++)
        norwd_r[i] = 0;

    for (uint32_t i = 0; i < NORWD_BYTES; i++)
        norwd_ram[i] = nor_pattern_byte(i);
    memcpy(norwd_nor, norwd_ram, NORWD_BYTES);

    for (uint32_t i = 0; i < NORWD_PAGES; i++) {
        guest_page_table[NORWD_FIRST + i] =
            FOLD_K((uint32_t)norwd_ram + i * 4096u, NORWD_FIRST + i);
    }

    /* 1. A REFUSED attach must still have kicked for every page it read.
     * The refusal path is the SLOW one -- it walks the whole readback before
     * it can know the bytes disagree -- so a heartbeat that only ran on
     * success would leave the longest stretch uncovered. Corrupt the LAST
     * page so the mismatch is found as late as possible. */
    norwd_hook_calls = 0;
    x86_nor_heartbeat = norwd_hook;
    norwd_nor[NORWD_BYTES - 1] ^= 0xFFu;
    norwd_r[0] = (uint32_t)x86_nor_attach(norwd_nor, NORWD_BYTES,
                                          norwd_ram, NORWD_BYTES, NORWD_FIRST);
    norwd_r[1] = x86_nor_attach_err;        /* 7 = readback disagreed */
    norwd_r[2] = x86_nor_heartbeats;        /* must be NORWD_PAGES */
    norwd_r[3] = norwd_hook_calls;          /* the hook really ran */
    norwd_r[4] = x86_nor_attached;          /* 0: nothing moved */

    /* 2. And on the ACCEPTED path, once per page, with the hook installed. */
    norwd_nor[NORWD_BYTES - 1] ^= 0xFFu;    /* put it back */
    norwd_hook_calls = 0;
    norwd_r[5] = (uint32_t)x86_nor_attach(norwd_nor, NORWD_BYTES,
                                          norwd_ram, NORWD_BYTES, NORWD_FIRST);
    norwd_r[6] = x86_nor_heartbeats;
    norwd_r[7] = norwd_hook_calls;
    norwd_r[8] = x86_nor_attached;

    /* 3. A NULL hook is the tier-1 default and must be a no-op, not a branch
     * to zero -- the exact shape that has cost this project days elsewhere
     * (retro-go's unguarded repaint callback, CLAUDE.md). The counter still
     * advances, which is what lets coverage be asserted with no hook at all. */
    x86_nor_heartbeat = NULL;
    x86_nor_attached = 0;                   /* allow one more attach */
    norwd_hook_calls = 0;
    norwd_r[9]  = (uint32_t)x86_nor_attach(norwd_nor, NORWD_BYTES,
                                           norwd_ram, NORWD_BYTES, NORWD_FIRST);
    norwd_r[10] = x86_nor_heartbeats;       /* still counted */
    norwd_r[11] = norwd_hook_calls;         /* 0: no hook installed */

    /* Leave nothing attached: later arms share the fold table. */
    x86_nor_attached = 0;
}

static void nor_test_run(void)
{
    for (uint32_t i = 0; i < 24; i++)
        nor_r[i] = 0;

    for (uint32_t i = 0; i < NOR_TEST_BYTES; i++)
        nor_test_ram[i] = nor_pattern_byte(i);

    for (uint32_t i = 0; i < NOR_TEST_PAGES; i++) {
        guest_page_table[NOR_TEST_FIRST + i] =
            FOLD_K((uint32_t)nor_test_ram + i * 4096u, NOR_TEST_FIRST + i);
    }

    /* ---- 1. A snapshot that does not match must be REFUSED outright.
     * This is the invariant the whole design turns on: a half-applied remap
     * must be impossible, so the fold table is not touched until the bytes in
     * NOR have been read back and verified. Corrupt one byte and the attach
     * has to fail with the guest still running out of AXI. */
    memcpy(NOR_FAKE_BASE, nor_test_ram, NOR_TEST_BYTES);
    NOR_FAKE_BASE[0x2345] ^= 0xFF;
    nor_r[0] = (uint32_t)x86_nor_attach(NOR_FAKE_BASE, NOR_TEST_BYTES,
                                        nor_test_ram, NOR_TEST_BYTES,
                                        NOR_TEST_FIRST);
    nor_r[1] = x86_nor_attach_err;
    nor_r[2] = x86_nor_attached;
    nor_r[3] = (guest_page_table[NOR_TEST_FIRST]
                == FOLD_K((uint32_t)nor_test_ram, NOR_TEST_FIRST));

    /* ---- 2. The real thing. */
    NOR_FAKE_BASE[0x2345] ^= 0xFF;
    nor_r[4] = (uint32_t)x86_nor_attach(NOR_FAKE_BASE, NOR_TEST_BYTES,
                                        nor_test_ram, NOR_TEST_BYTES,
                                        NOR_TEST_FIRST);
    nor_r[5] = x86_nor_attached;
    nor_r[6] = x86_nor_pages_total;
    nor_r[7] = x86_nor_pages_nor;
    nor_r[8] = x86_nor_axi_free;

    /* ---- 3. Guest code. 16-bit real mode, CS = 0, DS = the snapshot.
     *
     *   MOV AX, NOR_TEST_SEG      B8 lo hi
     *   MOV DS, AX                8E D8
     *   MOV AL, [3000h]           A0 00 30     read, served out of NOR
     *   MOV [5000h], AL           A2 00 50     write, pulls page 5 into AXI
     *   INC AL                    FE C0
     *   MOV [5001h], AL           A2 01 50     write again, now the fast path
     *   MOV AL, [5000h]           A0 00 50     read back through the fold
     *   HLT                       F4
     *
     * The read-back lands in AL and is checked there. An extra store to park
     * it somewhere would have gone through DS -- i.e. into the snapshot -- and
     * COWed a SECOND page, which is exactly what the first version of this
     * test did and why its counters read 2 instead of 1.
     */
    static const uint8_t prog[] = {
        0xB8, (uint8_t)(NOR_TEST_SEG & 0xFF), (uint8_t)(NOR_TEST_SEG >> 8),
        0x8E, 0xD8,
        0xA0, 0x00, 0x30,
        0xA2, 0x00, 0x50,
        0xFE, 0xC0,
        0xA2, 0x01, 0x50,
        0xA0, 0x00, 0x50,
        0xF4,
    };
    memcpy(test_ram, prog, sizeof(prog));

    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    for (int i = 0; i < 6; i++)
        cpu_state_dtcm.segs[i].limit = 0xFFFFFFFF;
    cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
    cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
    cpu_state_dtcm.regs[4] = 0x3000;

    uint32_t reads0 = x86_nor_reads;
    x86_cpu_interp_asm();

    nor_r[9]  = cpu_state_dtcm.exit_reason;      /* 6 = HLT, i.e. it finished */
    nor_r[10] = x86_nor_reads - reads0;
    nor_r[11] = x86_nor_writes;
    nor_r[12] = x86_nor_cow_pulls;
    nor_r[13] = x86_nor_cow_failures;
    nor_r[14] = x86_nor_pages_nor;
    nor_r[15] = x86_nor_pages_axi;
    nor_r[16] = x86_nor_axi_free;
    nor_r[17] = x86_nor_unmapped;

    /* What the guest actually moved. The byte at 0x3000 came out of NOR, was
       stored into page 5 and read straight back; AL must hold it. */
    nor_r[18] = nor_pattern_byte(0x3000);
    nor_r[19] = cpu_state_dtcm.regs[0] & 0xFFu;

    /* The COW page took the write; the NOR copy must be UNTOUCHED. If a store
       ever reached NOR this is the only thing that would notice. */
    nor_r[20] = NOR_FAKE_BASE[0x5000];
    nor_r[21] = NOR_FAKE_BASE[0x5001];
    nor_r[22] = nor_pattern_byte(0x5000);

    /* Page 5 is on the fast path now: its fold entry must be an AXI address
       from the COW pool, i.e. bit 31 CLEAR. */
    {
        uint32_t idx = NOR_TEST_FIRST + 5;
        uint32_t host = guest_page_table[idx] + (idx << 12);
        nor_r[23] = (host >> 31) & 1u;
    }

    /*
     * ---- 4. NOR-TIER EVICTION: the compare against the snapshot ------------
     *
     * The other half of x86_nor.c's life. Sections 1-3 cover a page coming OUT
     * of NOR by copy-on-write; this covers a page that was pulled into a pager
     * FRAME and is then evicted -- where the snapshot is the authoritative
     * copy, so the eviction is a memcmp and a page that still matches costs no
     * SD I/O at all. It is the branch of the dirty test that x86_pager.h's
     * "a NOR-tier page is compared against its snapshot" names, and nothing
     * else in the tree exercises it.
     *
     * The pager's own window, not the COW pool: two mechanisms, two pools, and
     * the point here is the pager's.
     */
    x86_pager_reset();
    x86_livemap_clear();
    x86_pager_own(NORPG_FIRST, NORPG_PAGES);
    x86_pager_add_frames(test_ram + NORPG_MEM_OFF, X86_PG_POOL_BYTES(2));
    x86_pager_set_nor(NOR_FAKE_BASE, NORPG_PAGES * 4096u, NORPG_FIRST);
    for (uint32_t i = 0; i < NORPG_PAGES; i++)
        guest_page_table[NORPG_FIRST + i] = FOLD_INVALID;
    {
        uint32_t fn0 = x86_pg_fill_nor, ev0 = x86_pg_evictions;
        uint32_t dc0 = x86_pg_drop_clean, wf0 = x86_pg_wb_file;

        uint32_t h0 = x86_pager_fault(NORPG_FIRST << 12, 0);
        uint32_t h1 = x86_pager_fault((NORPG_FIRST + 1u) << 12, 0);
        nor_r[24] = x86_pg_fill_nor - fn0;              /* 2: both from NOR */
        /* The frame really holds the snapshot's bytes. */
        nor_r[25] = (h0 && memcmp((void *)(uintptr_t)h0, NOR_FAKE_BASE,
                                  4096u) == 0);
        nor_r[26] = (h1 && memcmp((void *)(uintptr_t)h1,
                                  NOR_FAKE_BASE + 4096u, 4096u) == 0);

        /* Neither was written, so the victim still equals its snapshot and
           must be DROPPED. */
        x86_livemap_clear();
        x86_pager_fault((NORPG_FIRST + 2u) << 12, 0);
        nor_r[27] = x86_pg_evictions - ev0;             /* 1 */
        nor_r[28] = x86_pg_drop_clean - dc0;            /* 1 */
        nor_r[29] = x86_pg_wb_file - wf0;               /* 0: no SD I/O */
    }

    /* The negative control: one byte differs from the snapshot, so neither
       page can be dropped -- and with no pagefile wired there is nowhere to
       put it. The fault must be REFUSED, loudly, rather than the page being
       evicted and lost. Same setup, same pressure, one byte changed. */
    x86_pager_reset();
    x86_livemap_clear();
    x86_pager_own(NORPG_FIRST, NORPG_PAGES);
    x86_pager_add_frames(test_ram + NORPG_MEM_OFF, X86_PG_POOL_BYTES(2));
    x86_pager_set_nor(NOR_FAKE_BASE, NORPG_PAGES * 4096u, NORPG_FIRST);
    for (uint32_t i = 0; i < NORPG_PAGES; i++)
        guest_page_table[NORPG_FIRST + i] = FOLD_INVALID;
    {
        uint32_t ev0 = x86_pg_evictions, dc0 = x86_pg_drop_clean;
        uint32_t io0 = x86_pg_io_errors, ex1 = x86_pg_exhausted;
        uint32_t h0 = x86_pager_fault(NORPG_FIRST << 12, 1);
        uint32_t h1 = x86_pager_fault((NORPG_FIRST + 1u) << 12, 1);
        if (h0) *(uint8_t *)(uintptr_t)(h0 + 4095u) ^= 0xFF;
        if (h1) *(uint8_t *)(uintptr_t)(h1 + 4095u) ^= 0xFF;
        x86_livemap_clear();
        nor_r[30] = (x86_pager_fault((NORPG_FIRST + 2u) << 12, 0) == 0);
        nor_r[31] = x86_pg_evictions - ev0;             /* 0: nothing went */
        nor_r[32] = x86_pg_drop_clean - dc0;            /* 0: NOT clean */
        nor_r[33] = x86_pg_io_errors - io0;             /* 0: nothing was lost */
        nor_r[34] = x86_pg_exhausted - ex1;             /* 1: loud instead */
        /* The pages the guest modified are STILL THERE. That is the property
           the whole check exists for -- a MEM_FAULT the guest can see beats a
           page it cannot. */
        nor_r[35] = (h0 && *(uint8_t *)(uintptr_t)(h0 + 4095u)
                     == (uint8_t)~NOR_FAKE_BASE[4095]);
    }

    /* Leave nothing behind for the arms that run after this one. */
    x86_pager_set_nor(NULL, 0, 0);
    for (uint32_t i = 0; i < NORPG_PAGES; i++)
        guest_page_table[NORPG_FIRST + i] = FOLD_INVALID;
    x86_livemap_clear();
    x86_pager_reset();
    nor_r[36] = x86_pg_frames_total;                    /* 0 */
}

/*
 * The A20 gate and the wrap it controls.
 *
 * Two properties are being separated here, and no existing test could see
 * either. First, the wrap must cover exactly the 64 KB above 1 MB that
 * segment:offset can reach -- the old code aliased all 15 MB above 1 MB onto
 * the low megabyte, which is an 8086's address space and means there is no
 * memory above 1 MB at all. Second, the wrap must be CONDITIONAL: while A20 is
 * masked the HMA reads the bottom of memory, and while it is open it does not.
 *
 * The wrap arms are driven by GUEST code through a segment override, because
 * that is the only way an address above 0xFFFF is formed in real mode and it is
 * the path a program that relies on the wrap actually takes. The gate arms go
 * through the PORTS -- all three doors -- for the same reason: a guest opens
 * A20 by talking to hardware, and a host-side call to x86_a20_set() would test
 * the function while leaving the ports, which is where every real failure is,
 * completely uncovered.
 */

/* =========================================================================
 * PROTECTED MODE: the descriptor walk.
 *
 * Every arm below runs GUEST CODE, and that is not decoration. The walk is
 * reached from exactly one place -- L(SET_SREG) in cpu_exec_handlers.S -- and
 * only when a guest instruction loads a segment register with CR0.PE set. A
 * host-side call to x86_pmode_load_seg() would test the function while leaving
 * the path that reaches it, which is where a register-contract violation or a
 * wrong branch target would live, completely uncovered.
 *
 * The guests stay in 16-bit code and switch PE on with MOV CR0. That is the
 * real entry sequence's first half and it is enough to exercise every data
 * segment load: between the MOV CR0 and a far jump, CS keeps its real-mode
 * cache and 16-bit code keeps running, which both references implement
 * deliberately (references/tiny386/i386.c:2251-2255 -- writing CR0 does not
 * touch the segment caches). Far transfers are a later step and are not
 * tested here because they are not built.
 *
 * GUEST MEMORY MAP for these arms (test_ram is identity-mapped from 0):
 *   0x00000  code
 *   0x01000  the LGDT/LIDT six-byte operands
 *   0x02000  GDT, 8 entries, limit 0x3F
 *   0x02800  LDT, 2 entries, limit 0x0F
 *   0x03010  0x77  -- reached in REAL mode through DS=0x0300
 *   0x30010  0x5A  -- reached through GDT selector 0x08 (base 0x30000)
 *   0x32010  0xA5  -- reached through LDT selector 0x0C (base 0x32000)
 * ========================================================================= */
volatile uint32_t run_pmode_flag = 0;
volatile uint32_t pmode_r[80];

#define PM_GDT   0x2000u
#define PM_LDT   0x2800u
#define PM_OPND  0x1000u

/* Build one 8-byte descriptor. `access` is the byte at offset 5 (P/DPL/S/type)
   and `gran` the high nibble of the byte at offset 6 (G/D/AVL) -- both written
   in raw descriptor terms so the table below reads like the manual, not like
   our flags layout. */
static void pm_desc(uint32_t at, uint32_t base, uint32_t limit,
                    uint8_t access, uint8_t gran)
{
    uint8_t *d = test_ram + at;
    d[0] = (uint8_t)(limit & 0xFF);
    d[1] = (uint8_t)((limit >> 8) & 0xFF);
    d[2] = (uint8_t)(base & 0xFF);
    d[3] = (uint8_t)((base >> 8) & 0xFF);
    d[4] = (uint8_t)((base >> 16) & 0xFF);
    d[5] = access;
    d[6] = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    d[7] = (uint8_t)((base >> 24) & 0xFF);
}

/* Reset the CPU and run a program placed at guest 0. Returns exit_reason. */
static uint32_t pm_run(const uint8_t *prog, uint32_t len)
{
    memcpy(test_ram, prog, len);
    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    for (int i = 0; i < 6; i++)
        cpu_state_dtcm.segs[i].limit = 0xFFFFu;
    cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
    cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
    /* The GDT is normally installed by the guest's own LGDT, and one arm below
       proves LGDT does that. The rest start with it already in place so a
       failure in a later arm cannot be an LGDT failure wearing a disguise. */
    cpu_state_dtcm.gdt_base = PM_GDT;
    cpu_state_dtcm.gdt_limit = 0x3F;
    x86_cpu_interp_asm();
    return cpu_state_dtcm.exit_reason;
}

/* The three instructions that turn PE on, and the three that turn it off.
     0F 20 C0   MOV EAX,CR0
     0C 01      OR AL,1          (24 FE = AND AL,0FEh to leave)
     0F 22 C0   MOV CR0,EAX                                            */
#define PM_ENTER  0x0F, 0x20, 0xC0,  0x0C, 0x01,  0x0F, 0x22, 0xC0
#define PM_LEAVE  0x0F, 0x20, 0xC0,  0x24, 0xFE,  0x0F, 0x22, 0xC0

static void pmode_test_run(void)
{
    for (uint32_t i = 0; i < 80; i++)
        pmode_r[i] = 0;

    /* Undo whatever the A20 and pager arms left behind: they unmap everything
       above the HMA and hand frames around. These arms need the harness's
       plain identity map back. */
    for (uint32_t i = 0; i < FOLD_ENTRIES; i++)
        guest_page_table[i] = FOLD_INVALID;
    for (uint32_t i = 0; i < sizeof(test_ram) / 4096; i++)
        guest_page_table[i] = FOLD_K((uint32_t)test_ram + i * 4096, i);

    /* ---- the tables ---------------------------------------------------- */
    memset(test_ram + PM_GDT, 0, 0x40);
    memset(test_ram + PM_LDT, 0, 0x10);
    /* 0x08  data RW, base 0x30000, limit 0xFFFF, DPL 0, present, G=0     */
    pm_desc(PM_GDT + 0x08, 0x30000, 0xFFFF, 0x92, 0x00);
    /* 0x10  the SAME but G=1 with a limit FIELD of 0 -- the cached limit
       must come back 0xFFF, which is the whole granularity rule           */
    pm_desc(PM_GDT + 0x10, 0x31000, 0x00000, 0x92, 0x80);
    /* 0x18  data RW, DPL 0, limit 0x100 -- the plain limit, and the target
       of the wrong-DPL arm (loaded with RPL 3)                            */
    pm_desc(PM_GDT + 0x18, 0x00000, 0x00100, 0x92, 0x00);
    /* 0x20  data RW but NOT PRESENT                                       */
    pm_desc(PM_GDT + 0x20, 0x30000, 0xFFFF, 0x12, 0x00);
    /* 0x28  an LDT descriptor: S=0, type 2, present                       */
    pm_desc(PM_GDT + 0x28, PM_LDT, 0x000F, 0x82, 0x00);
    /* 0x30  EXECUTE-ONLY code (type 8): unreadable, so #GP into DS        */
    pm_desc(PM_GDT + 0x30, 0x30000, 0xFFFF, 0x98, 0x00);
    /* 0x38  READ-ONLY data (type 0): not writable, so #GP into SS         */
    pm_desc(PM_GDT + 0x38, 0x30000, 0xFFFF, 0x90, 0x00);
    /* LDT entry 1 -> selector 0x0C (index 1, TI set)                      */
    pm_desc(PM_LDT + 0x08, 0x32000, 0xFFFF, 0x92, 0x00);
    /* 0x40  A PERFECTLY VALID DESCRIPTOR, ONE ENTRY PAST THE LIMIT.
       This is what makes the table-bound check gradeable. With junk or zeros
       up here, removing the bound merely moves the fault to the S-bit check
       and the arm still "passes" -- which is exactly what the first version of
       this test did, and the negative control caught it. */
    pm_desc(PM_GDT + 0x40, 0x33000, 0xFFFF, 0x92, 0x00);

    /* A HLT at the base of the code descriptor at selector 0x30, so a far
       jump that installs it has somewhere to land. */
    test_ram[0x30000] = 0xF4;

    test_ram[0x03010] = 0x77;
    test_ram[0x30010] = 0x5A;
    test_ram[0x32010] = 0xA5;

    x86_pmode_loads = 0;
    x86_pmode_faults = 0;

    /* ==================================================================
     * A. LGDT and LIDT actually PARSE their operand.
     *
     * LGDT parsed nothing at all before this work -- it checked CPL and
     * branched to the fetch loop -- so the GDT stayed at base 0 limit 0
     * however many times a guest loaded it. Nothing could see that until
     * something walked the table.
     * ================================================================== */
    {
        /* limit 0x0027, base 0x00002000 at 0x1000; and 0x1006 for the IDT */
        uint8_t *o = test_ram + PM_OPND;
        o[0] = 0x27; o[1] = 0x00;
        o[2] = 0x00; o[3] = 0x20; o[4] = 0x00; o[5] = 0x00;
        o[6] = 0xFF; o[7] = 0x01;
        o[8] = 0x00; o[9] = 0x40; o[10] = 0x00; o[11] = 0x00;
        static const uint8_t prog[] = {
            0x0F, 0x01, 0x16, 0x00, 0x10,   /* LGDT [0x1000] */
            0x0F, 0x01, 0x1E, 0x06, 0x10,   /* LIDT [0x1006] */
            0xF4,
        };
        pmode_r[0] = pm_run(prog, sizeof(prog));            /* 6 = HLT */
        pmode_r[1] = cpu_state_dtcm.gdt_base;               /* 0x2000  */
        pmode_r[2] = cpu_state_dtcm.gdt_limit;              /* 0x27    */
        pmode_r[3] = cpu_state_dtcm.idtr_base;              /* 0x4000  */
        pmode_r[4] = cpu_state_dtcm.idtr_limit;             /* 0x1FF   */
        /* Real mode ran an LGDT and did NOT walk a descriptor. */
        pmode_r[5] = x86_pmode_loads;                       /* 0       */
    }

    /* ==================================================================
     * B. The BASE takes effect. A load through selector 0x08 must read
     *    linear 0x30010, not 0x00010 and not 0x00080.
     * ================================================================== */
    {
        static const uint8_t prog[] = {
            PM_ENTER,
            0xB8, 0x08, 0x00,               /* MOV AX,0x08      */
            0x8E, 0xD8,                     /* MOV DS,AX        */
            0x8A, 0x06, 0x10, 0x00,         /* MOV AL,[0x0010]  */
            0xF4,
        };
        pmode_r[6] = pm_run(prog, sizeof(prog));            /* 6 = HLT  */
        pmode_r[7] = cpu_state_dtcm.regs[0] & 0xFF;         /* 0x5A     */
        pmode_r[8] = cpu_state_dtcm.segs[X86_CPU_SEG_DS].base;   /* 0x30000 */
        pmode_r[9] = cpu_state_dtcm.segs[X86_CPU_SEG_DS].limit;  /* 0xFFFF  */
        pmode_r[10] = cpu_state_dtcm.segs[X86_CPU_SEG_DS].sel;   /* 0x08    */
        /* The rights the descriptor carried, decoded. */
        pmode_r[11] = X86_SEG_USABLE(&cpu_state_dtcm.segs[X86_CPU_SEG_DS]);
        pmode_r[12] = X86_DESC_TYPE(cpu_state_dtcm.segs[X86_CPU_SEG_DS].flags);
        pmode_r[13] = X86_DESC_DPL(cpu_state_dtcm.segs[X86_CPU_SEG_DS].flags);
        pmode_r[14] = x86_pmode_loads;                      /* exactly 1 */
        pmode_r[15] = x86_pmode_faults;                     /* 0         */
    }

    /* ==================================================================
     * C. The LIMIT takes effect, and the G bit expands it.
     *    Selector 0x18 has a raw limit of 0x100 and G=0 -> 0x100.
     *    Selector 0x10 has a raw limit of 0 and G=1     -> 0xFFF.
     *    A G that was ignored gives 0 for the second, which is exactly
     *    the value a null selector leaves, so both are read.
     * ================================================================== */
    {
        static const uint8_t prog[] = {
            PM_ENTER,
            0xB8, 0x18, 0x00,  0x8E, 0xD8,  /* MOV DS,0x18 */
            0xB8, 0x10, 0x00,  0x8E, 0xC0,  /* MOV ES,0x10 */
            0xF4,
        };
        pmode_r[16] = pm_run(prog, sizeof(prog));
        pmode_r[17] = cpu_state_dtcm.segs[X86_CPU_SEG_DS].limit;  /* 0x100 */
        pmode_r[18] = cpu_state_dtcm.segs[X86_CPU_SEG_ES].limit;  /* 0xFFF */
        pmode_r[19] = cpu_state_dtcm.segs[X86_CPU_SEG_ES].base;   /* 0x31000 */
    }

    /* ==================================================================
     * D. NULL SELECTORS. Three different behaviours; collapsing them is
     *    a real bug and neither reference records the DS case cleanly.
     * ================================================================== */
    {
        static const uint8_t prog_ds[] = {
            PM_ENTER,
            0x31, 0xC0,                     /* XOR AX,AX  */
            0x8E, 0xD8,                     /* MOV DS,AX  */
            0xF4,
        };
        pmode_r[20] = pm_run(prog_ds, sizeof(prog_ds));     /* 6 = HLT: legal */
        pmode_r[21] = X86_SEG_USABLE(&cpu_state_dtcm.segs[X86_CPU_SEG_DS]);
        pmode_r[22] = cpu_state_dtcm.segs[X86_CPU_SEG_DS].limit;  /* 0 */

        static const uint8_t prog_ss[] = {
            PM_ENTER,
            0x31, 0xC0,
            0x8E, 0xD0,                     /* MOV SS,AX  */
            0xF4,
        };
        pmode_r[23] = pm_run(prog_ss, sizeof(prog_ss));     /* 13 = GPF exit */
        pmode_r[24] = cpu_state_dtcm.pmode_excp;            /* 13 = #GP  */
        pmode_r[25] = cpu_state_dtcm.pmode_err;             /* 0, not the sel */
    }

    /* ==================================================================
     * E. A selector past the DESCRIPTOR TABLE LIMIT. GDT limit is 0x3F,
     *    so 0x38 is the last legal selector and 0x40 is one past it.
     * ================================================================== */
    {
        static const uint8_t prog[] = {
            PM_ENTER,
            0xB8, 0x40, 0x00,  0x8E, 0xD8,  /* MOV DS,0x40 */
            0xF4,
        };
        pmode_r[26] = pm_run(prog, sizeof(prog));           /* 13 */
        pmode_r[27] = cpu_state_dtcm.pmode_excp;            /* 13 = #GP */
        pmode_r[28] = cpu_state_dtcm.pmode_err;             /* 0x40 */

        /* And the one INSIDE the table: 0x38 is legal. */
        static const uint8_t prog_ok[] = {
            PM_ENTER,
            0xB8, 0x38, 0x00,  0x8E, 0xC0,  /* MOV ES,0x38 (read-only data) */
            0xF4,
        };
        pmode_r[29] = pm_run(prog_ok, sizeof(prog_ok));     /* 6 = HLT */
    }

    /* ==================================================================
     * F. WRONG DPL. Selector 0x18 has DPL 0; CPL is 0; loading it with
     *    RPL 3 makes max(CPL,RPL) = 3 > DPL, which is #GP(0x18).
     *    This is the check tiny386 does not have at all.
     * ================================================================== */
    {
        static const uint8_t prog[] = {
            PM_ENTER,
            0xB8, 0x1B, 0x00,  0x8E, 0xD8,  /* MOV DS,0x1B (0x18|RPL3) */
            0xF4,
        };
        pmode_r[30] = pm_run(prog, sizeof(prog));           /* 13 */
        pmode_r[31] = cpu_state_dtcm.pmode_excp;            /* 13 */
        pmode_r[32] = cpu_state_dtcm.pmode_err;             /* 0x18: RPL cleared */
        /* ...and the same descriptor at RPL 0 is fine, so the arm above
           failed for the PRIVILEGE and not for the descriptor. */
        static const uint8_t prog_ok[] = {
            PM_ENTER,
            0xB8, 0x18, 0x00,  0x8E, 0xD8,
            0xF4,
        };
        pmode_r[33] = pm_run(prog_ok, sizeof(prog_ok));     /* 6 = HLT */
    }

    /* ==================================================================
     * G. NOT PRESENT -> #NP, and it is a different vector from #GP.
     * ================================================================== */
    {
        static const uint8_t prog[] = {
            PM_ENTER,
            0xB8, 0x20, 0x00,  0x8E, 0xD8,
            0xF4,
        };
        pmode_r[34] = pm_run(prog, sizeof(prog));           /* 13 = the exit */
        pmode_r[35] = cpu_state_dtcm.pmode_excp;            /* 11 = #NP     */
        pmode_r[36] = cpu_state_dtcm.pmode_err;             /* 0x20         */
    }

    /* ==================================================================
     * H. TYPE rules.
     *      - a SYSTEM descriptor (S=0) into DS is #GP
     *      - EXECUTE-ONLY code into DS is #GP (unreadable)
     *      - READ-ONLY data into SS is #GP (unwritable)
     * ================================================================== */
    {
        static const uint8_t sysd[] = {
            PM_ENTER, 0xB8, 0x28, 0x00, 0x8E, 0xD8, 0xF4,
        };
        pmode_r[37] = pm_run(sysd, sizeof(sysd));
        pmode_r[38] = cpu_state_dtcm.pmode_excp;            /* 13 */
        pmode_r[39] = cpu_state_dtcm.pmode_err;             /* 0x28 */

        static const uint8_t xonly[] = {
            PM_ENTER, 0xB8, 0x30, 0x00, 0x8E, 0xD8, 0xF4,
        };
        pmode_r[40] = pm_run(xonly, sizeof(xonly));
        pmode_r[41] = cpu_state_dtcm.pmode_excp;            /* 13 */
        pmode_r[42] = cpu_state_dtcm.pmode_err;             /* 0x30 */

        static const uint8_t rossp[] = {
            PM_ENTER, 0xB8, 0x38, 0x00, 0x8E, 0xD0, 0xF4,   /* MOV SS,0x38 */
        };
        pmode_r[43] = pm_run(rossp, sizeof(rossp));
        pmode_r[44] = cpu_state_dtcm.pmode_excp;            /* 13 */
        pmode_r[45] = cpu_state_dtcm.pmode_err;             /* 0x38 */

        /* SS with a WRITABLE data selector at the right privilege works, so
           the three above failed for their types and not for being SS. */
        static const uint8_t ssok[] = {
            PM_ENTER, 0xB8, 0x08, 0x00, 0x8E, 0xD0, 0xF4,
        };
        pmode_r[46] = pm_run(ssok, sizeof(ssok));           /* 6 = HLT */
        pmode_r[47] = cpu_state_dtcm.segs[X86_CPU_SEG_SS].base;  /* 0x30000 */
    }

    /* ==================================================================
     * I. LLDT, and a selector resolved through the LDT.
     *    Reaching 0xA5 at linear 0x32010 means the LDT descriptor was
     *    read out of the GDT, its base cached, and selector 0x0C's TI bit
     *    routed the second walk through it.
     * ================================================================== */
    {
        static const uint8_t prog[] = {
            PM_ENTER,
            0xB8, 0x28, 0x00,               /* MOV AX,0x28       */
            0x0F, 0x00, 0xD0,               /* LLDT AX           */
            0xB8, 0x0C, 0x00,               /* MOV AX,0x0C (TI)  */
            0x8E, 0xD8,                     /* MOV DS,AX         */
            0x8A, 0x06, 0x10, 0x00,         /* MOV AL,[0x0010]   */
            0xF4,
        };
        pmode_r[48] = pm_run(prog, sizeof(prog));           /* 6 = HLT */
        pmode_r[49] = cpu_state_dtcm.ldt_base;              /* 0x2800  */
        pmode_r[50] = cpu_state_dtcm.ldt_limit;             /* 0x0F    */
        pmode_r[51] = cpu_state_dtcm.regs[0] & 0xFF;        /* 0xA5    */
        pmode_r[52] = cpu_state_dtcm.segs[X86_CPU_SEG_DS].base;  /* 0x32000 */

        /* LLDT with a NON-LDT descriptor is #GP. 0x08 is a data segment. */
        static const uint8_t bad[] = {
            PM_ENTER,
            0xB8, 0x08, 0x00,  0x0F, 0x00, 0xD0,
            0xF4,
        };
        pmode_r[53] = pm_run(bad, sizeof(bad));             /* 13 */
        pmode_r[54] = cpu_state_dtcm.pmode_err;             /* 0x08 */
    }

    /* ==================================================================
     * J. LEAVING protected mode.
     *
     *    Load DS in protected mode, clear PE, load DS again in real mode
     *    and read through it. Two things must hold and both were broken
     *    before this work: the real-mode base must be sel<<4 again, and
     *    the cached FLAGS must be cleared -- the real-mode store was an
     *    STRH that wrote only the selector, so a stale D bit from the
     *    protected-mode descriptor survived and would have left the
     *    interpreter decoding 32-bit operands against 16-bit code.
     * ================================================================== */
    {
        static const uint8_t prog[] = {
            PM_ENTER,
            0xB8, 0x08, 0x00,  0x8E, 0xD8,  /* MOV DS,0x08 (base 0x30000) */
            PM_LEAVE,
            0xB8, 0x00, 0x03,  0x8E, 0xD8,  /* MOV DS,0x0300 -> 0x3000    */
            0x8A, 0x06, 0x10, 0x00,         /* MOV AL,[0x0010] -> 0x3010  */
            0xF4,
        };
        pmode_r[55] = pm_run(prog, sizeof(prog));           /* 6 = HLT */
        pmode_r[56] = cpu_state_dtcm.regs[0] & 0xFF;        /* 0x77    */
        pmode_r[57] = cpu_state_dtcm.segs[X86_CPU_SEG_DS].base;  /* 0x3000 */
        pmode_r[58] = cpu_state_dtcm.segs[X86_CPU_SEG_DS].flags; /* 0      */
        pmode_r[59] = cpu_state_dtcm.cr0 & 1u;              /* 0       */
    }

    /* ==================================================================
     * K. The segment-limit predicate. Host-side, because it is NOT on the
     *    access path yet -- see x86_pmode.c. Settling the semantics now
     *    is the point; the plumbing that calls it is a later step.
     * ================================================================== */
    {
        X86CPUSeg s;
        /* expand-up, limit 0x100: the last legal byte is at 0x100 */
        s.flags = X86_DESC_P_FLAG | X86_DESC_S_FLAG | (2u << 8);
        s.base = 0; s.limit = 0x100; s.sel = 8;
        pmode_r[60] = (uint32_t)(x86_pmode_limit_ok(&s, 0x100, 1) == 1 &&
                                 x86_pmode_limit_ok(&s, 0x101, 1) == 0 &&
                                 /* a 4-byte access ENDING past the limit */
                                 x86_pmode_limit_ok(&s, 0x0FE, 4) == 0 &&
                                 x86_pmode_limit_ok(&s, 0x0FD, 4) == 1);
        /* the wrap: an offset near 2^32 must not pass by overflowing */
        s.limit = 0xFFFFFFFFu;
        pmode_r[61] = (uint32_t)(x86_pmode_limit_ok(&s, 0xFFFFFFFEu, 4) == 0);
        /* expand-down, B=0: valid is (limit, 0xFFFF] */
        s.flags = X86_DESC_P_FLAG | X86_DESC_S_FLAG | (6u << 8);
        s.limit = 0x1000;
        pmode_r[62] = (uint32_t)(x86_pmode_limit_ok(&s, 0x1000, 1) == 0 &&
                                 x86_pmode_limit_ok(&s, 0x1001, 1) == 1 &&
                                 x86_pmode_limit_ok(&s, 0xFFFF, 1) == 1 &&
                                 x86_pmode_limit_ok(&s, 0xFFFF, 2) == 0);
        /* ...and B=1 raises the ceiling, which DOSBox-X's version ignores */
        s.flags |= X86_DESC_D_FLAG;
        pmode_r[63] = (uint32_t)(x86_pmode_limit_ok(&s, 0x10000, 1) == 1);
    }

    /* ==================================================================
     * L. The SS PRIVILEGE rule, on its own.
     *
     * Selector 0x08 is writable data and present, so it passes every OTHER
     * rule SS has; the only thing wrong with 0x0B is its RPL. Arm H's
     * read-only and code selectors cannot grade this -- they fail on TYPE
     * first, so removing the privilege rule leaves them still failing.
     * The negative control caught precisely that.
     * ================================================================== */
    {
        static const uint8_t prog[] = {
            PM_ENTER,
            0xB8, 0x0B, 0x00,  0x8E, 0xD0,  /* MOV SS,0x0B (0x08|RPL3) */
            0xF4,
        };
        pmode_r[64] = pm_run(prog, sizeof(prog));           /* 13 */
        pmode_r[65] = cpu_state_dtcm.pmode_excp;            /* 13 = #GP */
        pmode_r[66] = cpu_state_dtcm.pmode_err;             /* 0x08 */
    }

    /* ==================================================================
     * M. THE TABLE BOUND, graded against a VALID descriptor past the end.
     *    Selector 0x40 names a well-formed present data segment at
     *    GDT + 0x40, which the limit of 0x3F puts outside the table.
     * ================================================================== */
    {
        static const uint8_t prog[] = {
            PM_ENTER,
            0xB8, 0x40, 0x00,  0x8E, 0xD8,
            0xF4,
        };
        pmode_r[67] = pm_run(prog, sizeof(prog));           /* 13 */
        pmode_r[68] = cpu_state_dtcm.pmode_excp;            /* 13 */
        pmode_r[69] = cpu_state_dtcm.pmode_err;             /* 0x40 */
        /* The base it WOULD have loaded, so a survivor is unambiguous. */
        pmode_r[70] = cpu_state_dtcm.segs[X86_CPU_SEG_DS].base;  /* NOT 0x33000 */
        /* Raise the limit and the very same selector loads: the arm above
           failed for the BOUND and not for the descriptor. */
        {
            static const uint8_t prog2[] = {
                PM_ENTER,
                0xB8, 0x40, 0x00,  0x8E, 0xD8,
                0xF4,
            };
            memcpy(test_ram, prog2, sizeof(prog2));
            memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
            for (int i = 0; i < 6; i++)
                cpu_state_dtcm.segs[i].limit = 0xFFFFu;
            cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
            cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
            cpu_state_dtcm.gdt_base = PM_GDT;
            cpu_state_dtcm.gdt_limit = 0x47;       /* one entry further */
            x86_cpu_interp_asm();
            pmode_r[71] = cpu_state_dtcm.exit_reason;            /* 6 */
            pmode_r[72] = cpu_state_dtcm.segs[X86_CPU_SEG_DS].base;  /* 0x33000 */
        }
    }

    /* ==================================================================
     * N. THE FAR JUMP -- the transfer that installs CS and flushes the
     *    pipeline, which is the last step of every real entry sequence.
     *
     *    Positive: JMP 0x30:0x0000 with 0x30 a code descriptor based at
     *    0x30000, where a HLT is waiting. Reaching it means CS was
     *    installed from the descriptor and the linear EIP rebuilt from
     *    the new base.
     *
     *    Negative: the same jump to selector 0x08, which is DATA. Without
     *    the CS type check that loads and executes the data segment --
     *    and since 0x08 is based at 0x30000 too, it would find the same
     *    HLT and look like a success. That is why the two selectors share
     *    a base: it makes the control unambiguous rather than accidental.
     *
     *    NOTE what is NOT tested here, because it is NOT BUILT: call
     *    gates, privilege changes, stack switches and IRET across rings.
     *    A far jump between same-privilege segments is the whole scope.
     * ================================================================== */
    {
        static const uint8_t prog[] = {
            PM_ENTER,
            0xEA, 0x00, 0x00, 0x30, 0x00,   /* JMP FAR 0x0030:0x0000 */
        };
        pmode_r[73] = pm_run(prog, sizeof(prog));           /* 6 = HLT */
        pmode_r[74] = cpu_state_dtcm.segs[X86_CPU_SEG_CS].base;  /* 0x30000 */
        pmode_r[75] = cpu_state_dtcm.segs[X86_CPU_SEG_CS].sel;   /* 0x30    */
        pmode_r[76] = cpu_state_dtcm.eip;                   /* 0 -- the HLT */
        pmode_r[77] = cpu_state_dtcm.cpl;                   /* 0 = CS.RPL   */

        static const uint8_t bad[] = {
            PM_ENTER,
            0xEA, 0x00, 0x00, 0x08, 0x00,   /* JMP FAR 0x0008:0x0000 -- DATA */
        };
        pmode_r[78] = pm_run(bad, sizeof(bad));             /* 13 */
        pmode_r[79] = cpu_state_dtcm.pmode_err;             /* 0x08 */
    }
}

static void a20_test_run(void)
{
    for (uint32_t i = 0; i < 24; i++)
        a20_r[i] = 0;

    /* Start from the state a machine boots in: gate masked, low memory mapped,
       everything above the HMA unmapped -- and NO PAGER, so the arms below see
       the HMA with nothing behind it. The HMA is backed now (hma_test_run());
       what this arm still grades is the WRAP, and a pool left over from
       another arm would hand it a frame and change the question. */
    x86_pager_reset();
    x86_io.a20_open = 0;
    x86_io.kbd_cmd = 0;
    x86_io.port92 = 0;
    x86_a20_changes = 0;
    for (uint32_t i = X86_HMA_FIRST_PAGE; i < FOLD_ENTRIES; i++)
        guest_page_table[i] = FOLD_INVALID;
    x86_mem_wrap_1mb();

    a20_r[0] = x86_io.a20_open;                       /* masked at boot */
    /* The alias, at both ends of the window. */
    a20_r[1] = (guest_page_table[X86_HMA_FIRST_PAGE]
                == guest_page_table[0] - (X86_HMA_FIRST_PAGE << FOLD_SHIFT));
    a20_r[2] = (guest_page_table[X86_HMA_FIRST_PAGE + X86_HMA_PAGES - 1]
                == guest_page_table[X86_HMA_PAGES - 1]
                   - (X86_HMA_FIRST_PAGE << FOLD_SHIFT));
    /* And the page just PAST it, which is the arm the old whole-space wrap
       fails: 0x110000 is extended memory and must not alias anything. */
    a20_r[3] = (guest_page_table[X86_HMA_FIRST_PAGE + X86_HMA_PAGES]
                == FOLD_INVALID);
    /* Nor may anything further up. 2 MB is where an 8086-style wrap would put
       another copy of low memory and a 286+ puts real RAM. */
    a20_r[4] = (guest_page_table[0x200000u >> FOLD_SHIFT] == FOLD_INVALID);

    /*
     * Guest arm 1: write a byte at 0000:0400 and read it back through
     * FFFF:0410, which is linear 0x100400. With the gate masked those are the
     * same byte.
     *
     * The marker is at 0x400 and not at 0x10 for a reason worth keeping: the
     * first version of this put it at 0x10, which is INSIDE this program, so
     * the store overwrote the 0x26 segment-override prefix of the very
     * instruction being tested. The read then ran unprefixed against DS and
     * returned a byte of the program -- a test that failed for a reason that
     * had nothing to do with A20.
     *
     *   MOV AX,0         B8 00 00      MOV AX,0FFFFh    B8 FF FF
     *   MOV ES,AX        8E C0         MOV ES,AX        8E C0
     *   MOV AL,5Ah       B0 5A         MOV AL,ES:[410h] 26 A0 10 04
     *   MOV ES:[400h],AL 26 A2 00 04   HLT              F4
     */
    static const uint8_t prog_wrap[] = {
        0xB8, 0x00, 0x00,  0x8E, 0xC0,
        0xB0, 0x5A,        0x26, 0xA2, 0x00, 0x04,
        0xB8, 0xFF, 0xFF,  0x8E, 0xC0,
        0x26, 0xA0, 0x10, 0x04,
        0xF4,
    };
    memcpy(test_ram, prog_wrap, sizeof(prog_wrap));
    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    for (int i = 0; i < 6; i++)
        cpu_state_dtcm.segs[i].limit = 0xFFFFFFFF;
    cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
    cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
    x86_cpu_interp_asm();
    a20_r[5] = cpu_state_dtcm.exit_reason;            /* 6 = HLT: it finished */
    /* AL is read straight out of the CPU state rather than stored somewhere in
       guest memory first: a store would go through DS and could only confuse
       the question this arm asks. */
    a20_r[6] = cpu_state_dtcm.regs[0] & 0xFFu;        /* must be 0x5A */
    a20_r[7] = test_ram[0x400];

    /*
     * Guest arm 2: open A20 through the FAST GATE at port 0x92, then reach for
     * the same HMA address. The alias is gone, nothing owns the HMA yet, so
     * this is a clean X86_EXIT_MEM_FAULT rather than a silent read of the
     * interrupt vector table.
     *
     *   MOV AL,2        B0 02
     *   OUT 92h,AL      E6 92
     *   MOV AX,0FFFFh   B8 FF FF
     *   MOV ES,AX       8E C0
     *   MOV AL,ES:[20h] 26 A0 20 00
     *   HLT             F4
     */
    static const uint8_t prog_open[] = {
        0xB0, 0x02,  0xE6, 0x92,
        0xB8, 0xFF, 0xFF,  0x8E, 0xC0,
        0x26, 0xA0, 0x20, 0x00,
        0xF4,
    };
    memcpy(test_ram, prog_open, sizeof(prog_open));
    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    for (int i = 0; i < 6; i++)
        cpu_state_dtcm.segs[i].limit = 0xFFFFFFFF;
    cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
    cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
    x86_cpu_interp_asm();
    a20_r[8]  = x86_io.a20_open;                      /* the OUT took */
    a20_r[9]  = x86_a20_changes;                      /* exactly one transition */
    a20_r[10] = cpu_state_dtcm.exit_reason;           /* 5 = MEM_FAULT */
    a20_r[11] = (guest_page_table[X86_HMA_FIRST_PAGE] == FOLD_INVALID);
    a20_r[12] = x86_port_read(0x92, 1) & 0x02u;       /* reads back as open */

    /* Masking it again must RESTORE the alias -- a gate that only works in one
       direction passes every test written in the opening direction only. */
    x86_port_write(0x92, 0x00, 1);
    a20_r[13] = x86_io.a20_open;
    a20_r[14] = x86_a20_changes;                      /* two transitions now */
    a20_r[15] = (guest_page_table[X86_HMA_FIRST_PAGE]
                 == guest_page_table[0] - (X86_HMA_FIRST_PAGE << FOLD_SHIFT));

    /* The 8042 output-port door: 0x64 = D1, then the value to 0x60. */
    x86_port_write(0x64, 0xD1, 1);
    x86_port_write(0x60, 0x02, 1);
    a20_r[16] = x86_io.a20_open;
    /* ... and read it back through 0x64 = D0, whose answer arrives on 0x60. */
    x86_port_write(0x64, 0xD0, 1);
    a20_r[17] = x86_port_read(0x60, 1) & 0x02u;

    /* The 0xDD / 0xDF shortcuts. */
    x86_port_write(0x64, 0xDD, 1);
    a20_r[18] = x86_io.a20_open;                      /* masked */
    x86_port_write(0x64, 0xDF, 1);
    a20_r[19] = x86_io.a20_open;                      /* open */

    /* A command that is NOT an output-port write must clear the pending latch.
       A stale 0xD1 turns the guest's next keyboard command byte into an
       output-port write and masks A20 in the middle of a running program --
       silently, which is the whole reason this arm exists. */
    x86_port_write(0x64, 0xD1, 1);
    x86_port_write(0x64, 0xAE, 1);       /* enable keyboard: not D1 */
    x86_port_write(0x60, 0x00, 1);       /* would mask A20 if the latch stuck */
    a20_r[20] = x86_io.a20_open;                      /* still open */

    /* Probing an already-open gate is not a transition. */
    uint32_t changes = x86_a20_changes;
    x86_port_write(0x92, 0x02, 1);
    a20_r[21] = (x86_a20_changes == changes);

    /* Leave the machine as the rest of the harness expects to find it. */
    x86_a20_set(0);
    a20_r[22] = x86_io.a20_open;
    a20_r[23] = x86_a20_state;
}

/*
 * The VRAM arena, its all-or-nothing rule, and the pin state the pager will
 * consult.
 *
 * The rule under test is the one that unblocks EGA: a claim that cannot be
 * fully placed must change NOTHING and must say by how much it missed. The
 * previous port measured a partial EGA card as producing worse output than no
 * EGA at all -- 8 KB per plane truncates cleanly, but 16 KB and 32 KB alias
 * the floating pages onto each other and fill the screen with noise that
 * reads as a rendering defect rather than a memory shortage. So "refused, and
 * here is the deficit" is the behaviour, and a half-placed claim is the bug.
 */
#define KB(n) ((n) * 1024u)
static void arena_test_run(void)
{
    for (uint32_t i = 0; i < 32; i++)
        arena_r[i] = 0;

    void *cga = NULL, *v13 = NULL, *ega[4] = { NULL, NULL, NULL, NULL };

    x86_arena_reset();
    arena_r[0] = (x86_arena_add_region(arena_test_mem,
                                       ARENA_TEST_PAGES * 4096u) == 0);
    arena_r[1] = x86_arena_bytes;                       /* 163,840 */
    arena_r[2] = x86_arena_free;

    /* ---- the two claims a real core makes at boot */
    arena_r[3] = (x86_arena_claim(X86_ARENA_ID_CGA, 1, KB(32), &cga) == 0);
    arena_r[4] = x86_arena_used;                        /* 32,768 */
    arena_r[5] = (x86_arena_claim(X86_ARENA_ID_VGA13, 1, KB(64), &v13) == 0);
    arena_r[6] = x86_arena_used;                        /* 98,304 */
    arena_r[7] = x86_arena_free;                        /* 65,536 */
    /* The two blocks must not overlap, which is the one way a bitmap
       allocator fails silently: both callers write and each sees the other's
       bytes appear in its own buffer. */
    arena_r[8] = (((uint8_t *)v13 >= (uint8_t *)cga + KB(32))
                  || ((uint8_t *)cga >= (uint8_t *)v13 + KB(64)));

    /* ---- EGA: four planes of 64 KB against 64 KB free. REFUSED. */
    uint32_t used_before = x86_arena_used;
    arena_r[9] = (x86_arena_claim(X86_ARENA_ID_EGA, 4, KB(64), ega) != 0);
    arena_r[10] = x86_arena_deficit;                    /* 3 planes: 196,608 */
    arena_r[11] = (x86_arena_used == used_before);      /* nothing moved */
    arena_r[12] = (ega[0] == NULL);                     /* out untouched */
    /* And the arena still WORKS after a refusal -- a failed claim that left
       the bitmap half-marked would show up here and nowhere else. */
    void *probe = NULL;
    arena_r[13] = (x86_arena_claim(X86_ARENA_ID_SPARE, 1, KB(64), &probe) == 0);
    x86_arena_release(X86_ARENA_ID_SPARE);

    /* ---- release the display claims and try again. Still refused: 163,840
       bytes cannot hold 262,144, and the deficit says so exactly. */
    x86_arena_release(X86_ARENA_ID_CGA);
    x86_arena_release(X86_ARENA_ID_VGA13);
    arena_r[14] = x86_arena_free;                       /* the whole region */
    arena_r[15] = (x86_arena_claim(X86_ARENA_ID_EGA, 4, KB(64), ega) != 0);
    arena_r[16] = x86_arena_deficit;                    /* 2 planes: 131,072 */

    /* ---- would_fit agrees with claim and does not change anything. Two
       answers that can disagree is how a mode gets ADOPTED and then fails to
       allocate, which is the exact failure the refusal rule exists to stop. */
    arena_r[17] = (x86_arena_would_fit(X86_ARENA_ID_EGA, 4, KB(64)) == 0);
    arena_r[18] = (x86_arena_would_fit(X86_ARENA_ID_EGA, 2, KB(64)) == 1);
    arena_r[19] = x86_arena_used;                       /* still 0 */

    /* ---- a re-claim releases the old one rather than leaking it */
    arena_r[20] = (x86_arena_claim(X86_ARENA_ID_CGA, 1, KB(32), &cga) == 0);
    arena_r[21] = (x86_arena_claim(X86_ARENA_ID_CGA, 1, KB(32), &cga) == 0);
    arena_r[22] = x86_arena_used;                       /* 32,768, not 65,536 */

    /* ---- pins, which is what the victim picker will test */
    uint32_t pinned0 = x86_pinned_pages;
    x86_pin_pages(0xB8000u >> 12, 8);
    arena_r[23] = (x86_pinned_pages - pinned0);         /* 8 */
    arena_r[24] = x86_page_is_pinned(0xB8000u >> 12);
    arena_r[25] = x86_page_is_pinned((0xB8000u >> 12) - 1u);   /* 0 */
    x86_pin_pages(0xB8000u >> 12, 8);                   /* idempotent */
    arena_r[26] = (x86_pinned_pages - pinned0);         /* still 8 */
    x86_unpin_pages(0xB8000u >> 12, 8);
    arena_r[27] = (x86_pinned_pages - pinned0);         /* 0 */

    /* ---- the live-pointer ring.
     *
     * The first check is the one that matters and it is not obvious: the ring
     * is .bss, so it starts as a row of zeroes, and page 0 is a real and
     * constantly used page. Without an armed flag every eviction of page 0
     * would be vetoed from boot by a pointer that was never handed out. */
    x86_livemap_clear();
    arena_r[28] = (x86_livemap_holds(0) == 0);
    x86_livemap_note(0x42);
    x86_livemap_note(0x43);
    arena_r[29] = (x86_livemap_holds(0x42) && x86_livemap_holds(0x43)
                   && !x86_livemap_holds(0x44));
    x86_livemap_clear();
    arena_r[30] = (x86_livemap_holds(0x42) == 0);

    /* Leave the arena empty so nothing downstream inherits a claim. */
    x86_arena_reset();
    arena_r[31] = x86_arena_used;
}

/*
 * Demand paging, driven by GUEST instructions.
 *
 * The window is 0xC0000-0xC7FFF -- eight pages that the harness leaves
 * unmapped, inside what real-mode segment:offset can reach -- and the pager is
 * given FOUR frames for it. That 2:1 overcommit is the point: the first four
 * pages fault in and the fifth cannot, so both the working case and the
 * exhaustion case are exercised by one program.
 *
 * Exhaustion has to be LOUD. A pool that silently eats stores is
 * indistinguishable from one that works, and this project has hit silent
 * allocation failure five times; here the guest takes a clean
 * X86_EXIT_MEM_FAULT and x86_pg_exhausted says exactly why.
 */
#define PGT_SEG    0xC000u
#define PGT_FIRST  (0xC0000u >> 12)
#define PGT_PAGES  8u
#define PGT_FRAMES 4u
#define PGT_MEM_OFF (700u * 1024u)      /* page-aligned inside test_ram */

/*
 * KEEN4's measured peak, in frames, out of the 160 the shipping pool has. It
 * is quoted in docs/13 and in x86_pager.h's donation comment, and it is the
 * number X86_PG_DONATE_RESERVE has to clear -- see pager_donate_working_set()
 * below for why a reserve under it is a HANG and not a slow frame.
 */
#define KEEN4_PEAK_FRAMES 134u

/*
 * ---- A DONATION MAY NOT BE FUNDED OUT OF THE WORKING SET -------------------
 *
 * Donate with `reserve` frames held back, then run a `wset`-page working set
 * that is DIRTY -- every page written, twice round, exactly what a title does
 * between frames -- and return how many pagefile writes it cost.
 *
 * The answer must be ZERO. A reserve at or above the working set means every
 * page stays resident and nothing is ever written; a reserve below it means
 * each pass round the set evicts and re-reads, and each of those evictions is
 * a 4 KB SD write. Under gwemu, and here, that write is free and the run looks
 * healthy -- which is exactly how the shipping reserve of 100 against KEEN4's
 * 134-frame peak survived: x86_pg_exhausted stayed 0, the title reached its
 * MAIN MENU, and the only witness was a counter nobody was reading.
 *
 * On silicon the same write costs up to 433,509 us (x86_pg_wb_us_worst,
 * measured on hardware), it happens inside the emulator slice with no return
 * to the frame loop, and MAX_SLICE_CEIL cannot bound it because it bounds
 * INSTRUCTIONS and this time is spent in f_write. At reserve 100 KEEN4 paid 84
 * evictions and 82 such writes in its first 15 seconds; at 134 it pays none,
 * on hardware as well as under gwemu.
 *
 * This does NOT fix KEEN4's watchdog wedge -- that was tested on silicon with
 * the pagefile provably untouched and the wedge still happens. See
 * x86_pager.h. What it fixes is the guest running 34 frames short of its own
 * measured working set, which is a defect whether or not anything else is.
 *
 * So this returns a WRITE COUNT and not a pass/fail: the caller asserts 0 for
 * a reserve that covers the set and NON-ZERO for one that does not, and the
 * second is the negative control for the first inside the same test.
 */
static uint32_t pager_donate_working_set(uint32_t pool, uint32_t wset,
                                         uint32_t reserve, uint32_t ask)
{
    uint32_t wf0;

    x86_arena_reset();
    x86_pager_reset();
    x86_livemap_clear();
    x86_pager_set_donate_reserve(reserve);
    x86_pager_set_backing(pgf_read, pgf_write);
    pgf_first_slot = PGT_FIRST;
    x86_pager_own(PGT_FIRST, PGT_PAGES);
    x86_pager_add_frames(test_ram + PGT_MEM_OFF, X86_PG_POOL_BYTES(pool));
    for (uint32_t i = 0; i < PGT_PAGES; i++)
        guest_page_table[PGT_FIRST + i] = FOLD_INVALID;

    /* The donation the 0Dh mode set makes: take what the reserve allows. */
    x86_pager_donate_upto(ask, X86_PG_SIZE);

    /* Now the working set, dirty, twice. One pass could be served by frames
       that happened to be free; two cannot. */
    wf0 = x86_pg_wb_file;
    for (uint32_t pass = 0; pass < 2u; pass++) {
        for (uint32_t p = 0; p < wset; p++) {
            uint32_t h = x86_pager_fault(0xC0000u + (p << 12), 1);
            x86_livemap_clear();
            if (h)
                *(uint8_t *)(uintptr_t)(h + 4095u) = (uint8_t)(p + 1u);
        }
    }
    return x86_pg_wb_file - wf0;
}


/* ===========================================================================
 * THE DONATION POLICY -- the loop, both ways, and its negative controls.
 *
 * Everything above this point tests the MECHANISM against a FIXED reserve.
 * What is graded here is the DECISION: frames borrowed while they are
 * genuinely idle, and handed back under pressure BEFORE the guest starts
 * evicting. The pool is 8 frames rather than 160 and the floor/margin are
 * scaled to it, but the shape is KEEN4's -- an idle phase that wants a big
 * card, a working set that then grows into the frames it lent.
 *
 * The two negative controls are RUN, not argued: the same walk with the shrink
 * IGNORED must produce evictions (and does), and the same walk with the policy
 * off must produce no donation at all.
 */
#define POL_FLOOR   2u
#define POL_MARGIN  2u
#define POL_QUIET   3u

/* The eviction counter is CUMULATIVE across every arm in this file, so the
   policy arms read deltas from here. An absolute read passes or fails on what
   an unrelated arm did earlier, which is not a test of anything. */
static uint32_t pol_ev0, pol_wb0;

static void pol_setup(uint32_t pool)
{
    x86_arena_reset();
    x86_pager_reset();
    x86_livemap_clear();
    x86_pager_policy_enable(1);
    x86_pager_policy_config(POL_FLOOR, POL_MARGIN, POL_QUIET);
    x86_pager_set_backing(pgf_read, pgf_write);
    pgf_first_slot = PGT_FIRST;
    x86_pager_own(PGT_FIRST, PGT_PAGES);
    x86_pager_add_frames(test_ram + PGT_MEM_OFF, X86_PG_POOL_BYTES(pool));
    for (uint32_t i = 0; i < PGT_PAGES; i++)
        guest_page_table[PGT_FIRST + i] = FOLD_INVALID;
    x86_pager_policy_reset();
    pol_ev0 = x86_pg_evictions;
    pol_wb0 = x86_pg_wb_file;
}

/* Dirty pages [0,n) of the window, the way a guest would. */
static void pol_touch(uint32_t n)
{
    for (uint32_t p = 0; p < n; p++) {
        uint32_t h = x86_pager_fault(0xC0000u + (p << 12), 1);
        x86_livemap_clear();
        if (h)
            *(uint8_t *)(uintptr_t)(h + 4095u) = (uint8_t)(p + 1u);
    }
}

/* Quiet ticks until the policy offers to lend, bounded so a policy that never
   grows fails the arm rather than hanging the harness. */
static int pol_tick_until_grow(void)
{
    for (uint32_t i = 0; i < POL_QUIET + 4u; i++) {
        if (x86_pager_policy_tick() == X86_PG_POLICY_GROW)
            return 1;
    }
    return 0;
}

static void pager_policy_arms(void)
{
    uint32_t donated_first, ev_at_shrink;

    /* ---- 1. GROW WHILE IDLE ------------------------------------------- */
    pol_setup(8u);
    pol_touch(2u);                      /* a small working set: peak = 2 */
    pager_r[77] = pol_tick_until_grow();                        /* 1 */
    pager_r[78] = (x86_pager_policy_headroom() > 0u);           /* 1 */
    /* Borrow ONLY on the policy's permission, exactly as the video path does.
       Donating unconditionally here would leave the grow's negative control
       with nothing to catch -- the card would still get built. */
    if (pager_r[77])
        x86_pager_donate_upto(4u * 4096u, 4096u);
    donated_first = x86_pg_donated;
    pager_r[79] = (donated_first > 0u);                         /* 1 */
    pager_r[80] = x86_pg_evictions - pol_ev0;  /* idle frames cost NOTHING */
    pager_r[81] = (x86_pg_frames_total < 8u);                   /* 1 */

    /* ---- 2. HAND THEM BACK BEFORE THE GUEST THRASHES ------------------ */
    /* Grow the working set into what was lent. The watermark must fire while
       the pool still has runway -- the eviction counter at that moment is the
       whole point of the arm, because an eviction is a 4 KB pagefile write and
       on silicon that is up to 433 ms INSIDE the frame loop. */
    {
        int shrank = 0;
        for (uint32_t p = 2u; p < PGT_PAGES && !shrank; p++) {
            pol_touch(p + 1u);
            if (x86_pager_policy_tick() == X86_PG_POLICY_SHRINK)
                shrank = 1;
        }
        pager_r[82] = shrank;                                   /* 1 */
    }
    ev_at_shrink = x86_pg_evictions - pol_ev0;
    pager_r[83] = ev_at_shrink;         /* 0: BEFORE the first writeback */
    pager_r[84] = x86_pg_shrink_signals;                        /* 1 */
    /* What the video path does with a SHRINK: release, then reclaim. */
    x86_arena_release(X86_ARENA_ID_EGA);
    pager_r[85] = (x86_pager_reclaim_from_arena() == 0);        /* 1 */
    pager_r[86] = x86_pg_frames_total;                          /* 8: whole */
    pager_r[87] = (x86_pg_policy_ratchet
                   >= x86_pg_resident_peak + POL_MARGIN);       /* 1 */
    /* THE RE-EVALUATION ITSELF: the second time round, the same call lends
       FEWER frames, because the reserve now carries a working set that has
       been measured rather than guessed. A policy that re-lent the same amount
       would pass every arm above this one. */
    x86_pager_donate_upto(4u * 4096u, 4096u);
    pager_r[88] = (x86_pg_donated < donated_first);             /* 1 */
    x86_arena_release(X86_ARENA_ID_EGA);
    x86_pager_reclaim_from_arena();

    /* ---- 3. NEGATIVE CONTROL: the shrink IGNORED ----------------------- */
    /* Identical walk, and the SHRINK is thrown away instead of acted on. The
       evictions the arm above proves absent must now appear, or that arm is
       measuring nothing. */
    pol_setup(8u);
    pol_touch(2u);
    (void)pol_tick_until_grow();
    x86_pager_donate_upto(4u * 4096u, 4096u);
    for (uint32_t pass = 0; pass < 2u; pass++)
        pol_touch(PGT_PAGES);
    pager_r[89] = (x86_pg_evictions - pol_ev0 > 0u);                      /* 1 */
    pager_r[90] = (x86_pg_wb_file > pol_wb0);                        /* 1 */

    /* ---- 4. NEGATIVE CONTROL: the grow DISABLED ------------------------ */
    /* The policy off, with the static reserve at the pool size -- which is
       what the tree did before this work whenever the constant was set for
       the working set. Nothing is lent and the card stays the arena's. */
    pol_setup(8u);
    x86_pager_policy_enable(0);
    x86_pager_set_donate_reserve(8u);
    pol_touch(2u);
    (void)pol_tick_until_grow();
    x86_pager_donate_upto(4u * 4096u, 4096u);
    pager_r[91] = x86_pg_donated;                               /* 0 */
    pager_r[92] = x86_pg_frames_total;                          /* 8 */

    x86_pager_policy_enable(1);
    x86_pager_policy_config(X86_PG_RESERVE_FLOOR, X86_PG_RESERVE_MARGIN,
                            X86_PG_POLICY_QUIET);
    x86_arena_reset();
    x86_pager_reset();
}

static void pager_test_run(void)
{
    for (uint32_t i = 0; i < 96; i++)
        pager_r[i] = 0;

    /* THE STATIC ARMS RUN WITH THE POLICY OFF, and that is the header's own
       rule: with it on, the reserve is the pager's MEASURED working set and
       every arm below that states the arithmetic of a fixed reserve would be
       asserting the policy's answer instead of its own. The policy has its own
       arm at the end of this function, and it turns it back on itself. */
    x86_pager_policy_enable(0);
    x86_pager_reset();
    pgf_first_slot = PGT_FIRST;
    pgf_writes = pgf_reads = pgf_fail = 0;
    memset(test_ram + PGF_OFF, 0xEE, PGF_SLOTS * 4096u);
    x86_pager_set_backing(pgf_read, pgf_write);
    x86_livemap_clear();
    x86_pager_own(PGT_FIRST, PGT_PAGES);
    pager_r[0] = (x86_pager_add_frames(test_ram + PGT_MEM_OFF,
                                       X86_PG_POOL_BYTES(PGT_FRAMES)) == 0);
    pager_r[1] = x86_pg_frames_total;
    pager_r[2] = x86_pg_frames_free;
    for (uint32_t i = 0; i < PGT_PAGES; i++)
        guest_page_table[PGT_FIRST + i] = FOLD_INVALID;

    uint32_t fr0 = x86_pg_faults_r, fw0 = x86_pg_faults_w;
    uint32_t fz0 = x86_pg_fill_zero, ex0 = x86_pg_exhausted;

    /*
     *   MOV AX,0C000h    B8 00 C0
     *   MOV DS,AX        8E D8
     *   MOV BYTE [0],5Ah C6 06 00 00 5A     <- write fault, page fills as ZERO
     *   MOV AL,[0]       A0 00 00           <- resident now: no second fault
     *   HLT              F4
     */
    static const uint8_t prog_one[] = {
        0xB8, 0x00, 0xC0,  0x8E, 0xD8,
        0xC6, 0x06, 0x00, 0x00, 0x5A,
        0xA0, 0x00, 0x00,
        0xF4,
    };
    memcpy(test_ram, prog_one, sizeof(prog_one));
    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    for (int i = 0; i < 6; i++)
        cpu_state_dtcm.segs[i].limit = 0xFFFFFFFF;
    cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
    cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
    x86_cpu_interp_asm();

    pager_r[3] = cpu_state_dtcm.exit_reason;             /* 6 = HLT */
    pager_r[4] = cpu_state_dtcm.regs[0] & 0xFFu;         /* 0x5A read back */
    pager_r[5] = x86_pg_faults_w - fw0;                  /* exactly one */
    pager_r[6] = x86_pg_faults_r - fr0;                  /* zero: it is resident */
    pager_r[7] = x86_pg_fill_zero - fz0;                 /* filled from ZERO */
    pager_r[8] = x86_pg_resident;                        /* 1 */
    pager_r[9] = x86_pg_frames_free;                     /* 3 */
    /* The fold entry now points into the frame pool, i.e. the page really is
       on the FAST path and the next access will not trap at all. */
    {
        uint32_t k = guest_page_table[PGT_FIRST];
        uint32_t host = (PGT_FIRST << 12) + k;
        pager_r[10] = (k != FOLD_INVALID)
                      && (host >= (uint32_t)(uintptr_t)(test_ram + PGT_MEM_OFF))
                      && (host <  (uint32_t)(uintptr_t)(
                                        test_ram + PGT_MEM_OFF
                                        + X86_PG_POOL_BYTES(PGT_FRAMES)));
    }

    /*
     * Five pages against four frames. The fifth used to be a hard
     * X86_EXIT_MEM_FAULT; with a victim picker it is an EVICTION, and the page
     * that leaves is written to the pagefile because the guest put 0x5A in it.
     *
     *   MOV AX,0C000h  / MOV DS,AX
     *   MOV [1000h],AL   A2 00 10
     *   MOV [2000h],AL   A2 00 20
     *   MOV [3000h],AL   A2 00 30    <- the last free frame
     *   MOV [4000h],AL   A2 00 40    <- evicts one
     *   HLT
     */
    uint32_t ev0 = x86_pg_evictions, dc0 = x86_pg_drop_clean;
    uint32_t wf0 = x86_pg_wb_file,   wb0 = x86_pg_wb_bytes;
    uint32_t ff0 = x86_pg_fill_file, io0 = x86_pg_io_errors;
    static const uint8_t prog_full[] = {
        0xB8, 0x00, 0xC0,  0x8E, 0xD8,
        0xB0, 0x5A,                         /* MOV AL,5Ah -- every page DIRTY */
        0xA2, 0x00, 0x10,
        0xA2, 0x00, 0x20,
        0xA2, 0x00, 0x30,
        0xA2, 0x00, 0x40,
        0xF4,
    };
    memcpy(test_ram, prog_full, sizeof(prog_full));
    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    for (int i = 0; i < 6; i++)
        cpu_state_dtcm.segs[i].limit = 0xFFFFFFFF;
    cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
    cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
    x86_cpu_interp_asm();

    pager_r[11] = cpu_state_dtcm.exit_reason;            /* 6 = HLT, not 5 */
    pager_r[12] = x86_pg_resident;                       /* 4: the pool size */
    pager_r[13] = x86_pg_frames_free;                    /* 0 */
    pager_r[14] = x86_pg_exhausted - ex0;                /* 0: nothing faulted */
    pager_r[15] = x86_pg_evictions - ev0;                /* exactly one */
    /* It held 0x5A, so it was DIRTY and had to be written. A drop here would
       be the bug the whole compare-at-eviction rule exists to avoid getting
       wrong in the other direction. */
    pager_r[16] = x86_pg_wb_file - wf0;                  /* 1 */
    pager_r[17] = x86_pg_drop_clean - dc0;               /* 0 */
    pager_r[18] = x86_pg_wb_bytes - wb0;                 /* 4096 */
    pager_r[19] = pgf_writes;                            /* 1 */
    pager_r[20] = x86_pg_io_errors - io0;                /* 0 */
    /* The victim really is unmapped -- a page still on the fast path would be
       a frame handed to two pages at once. */
    /* Frames come off the free list from the top, so page C0 took frame 3 and
       page C3 took frame 0 -- and frame 0 is where the clock hand starts. The
       victim is C3, and naming it is the point: a check against "some page"
       cannot tell an eviction from a frame handed out twice. */
    pager_r[21] = (guest_page_table[PGT_FIRST + 3u] == FOLD_INVALID
                   && guest_page_table[PGT_FIRST] != FOLD_INVALID);

    /*
     * The ROUND TRIP. Read the evicted page back through the guest and the
     * byte the guest wrote must still be there. This is the arm that fails if
     * the writeback wrote the wrong slot, the fill read the wrong slot, or the
     * tier was moved to FILE before the write succeeded.
     */
    static const uint8_t prog_back[] = {
        0xB8, 0x00, 0xC0,  0x8E, 0xD8,
        0xA0, 0x00, 0x30,                   /* MOV AL,[3000h] -- the victim */
        0xF4,
    };
    memcpy(test_ram, prog_back, sizeof(prog_back));
    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    for (int i = 0; i < 6; i++)
        cpu_state_dtcm.segs[i].limit = 0xFFFFFFFF;
    cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
    cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
    x86_cpu_interp_asm();
    pager_r[22] = cpu_state_dtcm.exit_reason;            /* 6 = HLT */
    pager_r[23] = cpu_state_dtcm.regs[0] & 0xFFu;        /* 0x5A, from the file */
    pager_r[24] = x86_pg_fill_file - ff0;                /* 1: it came from FILE */
    pager_r[25] = pgf_reads;                             /* 1 */

    /*
     * A ZERO-TIER PAGE THAT IS STILL ZERO IS DROPPED WITH NO WRITE.
     *
     * This is the rule the whole design is for. A fresh pool, one page touched
     * by a READ (so it fills from ZERO and is never modified), then enough
     * pressure to evict it: the pagefile must not be touched at all.
     */
    x86_pager_reset();
    x86_livemap_clear();
    x86_pager_own(PGT_FIRST, PGT_PAGES);
    x86_pager_add_frames(test_ram + PGT_MEM_OFF, X86_PG_POOL_BYTES(2));
    for (uint32_t i = 0; i < PGT_PAGES; i++)
        guest_page_table[PGT_FIRST + i] = FOLD_INVALID;
    pgf_writes = pgf_reads = 0;
    ev0 = x86_pg_evictions; dc0 = x86_pg_drop_clean; wf0 = x86_pg_wb_file;

    /* Two reads fill two frames from ZERO and leave them untouched; the third
       read has to evict one of them. */
    x86_pager_fault(0xC0000u, 0);
    x86_pager_fault(0xC1000u, 0);
    x86_livemap_clear();
    x86_pager_fault(0xC2000u, 0);
    pager_r[26] = x86_pg_evictions - ev0;                /* 1 */
    pager_r[27] = x86_pg_drop_clean - dc0;               /* 1: dropped */
    pager_r[28] = x86_pg_wb_file - wf0;                  /* 0 */
    pager_r[29] = pgf_writes;                            /* 0: no SD I/O AT ALL */

    /* ...and the NEGATIVE CONTROL for exactly that arm. Same pool, same
       pressure, but one byte of the page is non-zero. A dirty-detection that
       always said "clean" would pass the arm above; it cannot pass this one. */
    x86_pager_reset();
    x86_livemap_clear();
    x86_pager_own(PGT_FIRST, PGT_PAGES);
    x86_pager_add_frames(test_ram + PGT_MEM_OFF, X86_PG_POOL_BYTES(2));
    for (uint32_t i = 0; i < PGT_PAGES; i++)
        guest_page_table[PGT_FIRST + i] = FOLD_INVALID;
    pgf_writes = 0;
    ev0 = x86_pg_evictions; dc0 = x86_pg_drop_clean; wf0 = x86_pg_wb_file;
    {
        /* BOTH pages, so the arm does not depend on which one the clock picks
           -- and the LAST byte of each, not the first: a scan that only looked
           at the head would still call these clean. */
        uint32_t h = x86_pager_fault(0xC0000u, 1);
        if (h)
            *(uint8_t *)(uintptr_t)(h + 4095u) = 0x01;
        h = x86_pager_fault(0xC1000u, 1);
        if (h)
            *(uint8_t *)(uintptr_t)(h + 4095u) = 0x01;
    }
    x86_livemap_clear();
    x86_pager_fault(0xC2000u, 0);
    pager_r[30] = x86_pg_evictions - ev0;                /* 1 */
    pager_r[31] = x86_pg_drop_clean - dc0;               /* 0: NOT clean */
    pager_r[32] = x86_pg_wb_file - wf0;                  /* 1 */
    pager_r[33] = pgf_writes;                            /* 1 */

    /*
     * The two VETOES, which are the reason x86_livemap_holds() exists. The
     * prior port measured 5 evictions of a live-mapped page producing 26,329
     * wrong bytes with cow_lost == 0 green throughout.
     */
    x86_pager_reset();
    x86_livemap_clear();
    x86_pager_own(PGT_FIRST, PGT_PAGES);
    x86_pager_add_frames(test_ram + PGT_MEM_OFF, X86_PG_POOL_BYTES(2));
    for (uint32_t i = 0; i < PGT_PAGES; i++)
        guest_page_table[PGT_FIRST + i] = FOLD_INVALID;
    ex0 = x86_pg_exhausted;
    uint32_t lh0 = x86_livemap_hits;
    x86_pager_fault(0xC0000u, 0);
    x86_pager_fault(0xC1000u, 0);
    /* Pin one, hand a host pointer into the other: no legal victim exists. */
    x86_pin_pages(PGT_FIRST, 1);
    x86_livemap_clear();
    x86_livemap_note(PGT_FIRST + 1u);
    pager_r[34] = (x86_pager_fault(0xC2000u, 0) == 0);   /* refused */
    pager_r[35] = x86_pg_exhausted - ex0;                /* 1: loud */
    pager_r[36] = (x86_livemap_hits > lh0);              /* the ring vetoed */
    pager_r[37] = x86_pg_resident;                       /* still 2 */
    /* Lift ONE veto and the same fault succeeds -- which is what says the two
       arms above measured the veto and not a broken pool. */
    x86_unpin_pages(PGT_FIRST, 1);
    pager_r[38] = (x86_pager_fault(0xC2000u, 0) != 0);
    pager_r[39] = x86_pg_exhausted - ex0;                /* still 1 */

    /* The HOST side faults the same way. DOS loads a program by copying into
       memory the guest has never touched, so this path carries every program
       load in the system. */
    x86_livemap_clear();
    pager_r[40] = (x86_guest_ptr(0xC5000u, 16) != NULL);

    /* An address outside the window is NOT a page fault -- it is a genuinely
       unmapped address, and conflating the two would turn a guest's own wild
       pointer into a silent allocation. */
    uint32_t un0 = x86_pg_unmapped;
    pager_r[41] = (x86_pager_fault(0x00500000u, 1) == 0);
    pager_r[42] = (x86_pg_unmapped - un0);               /* 1 */

    /* Leave nothing behind: the window goes back to unmapped and the pager
       lets go of the frames it was lent out of test_ram. */
    x86_unpin_pages(PGT_FIRST, PGT_PAGES);
    x86_livemap_clear();
    x86_pager_set_backing(NULL, NULL);
    for (uint32_t i = 0; i < PGT_PAGES; i++)
        guest_page_table[PGT_FIRST + i] = FOLD_INVALID;
    x86_pager_reset();
    pager_r[43] = x86_pg_frames_total;
    pager_r[44] = pgf_fail;                              /* 0: no stray slot */

    /*
     * ---- DONATION: spare guest frames become an EGA card --------------------
     *
     * Five frames, a reserve of one. 8,192 bytes needs a run of four (each end
     * of a run can lose a page to inward rounding), which is 16,416 bytes of
     * stride and yields 16,384 usable -- the donation says what it GOT rather
     * than what it was asked for, and the arithmetic is the test's own.
     */
    x86_arena_reset();
    x86_pager_reset();
    x86_livemap_clear();
    x86_pager_set_donate_reserve(1);
    x86_pager_set_backing(pgf_read, pgf_write);
    pgf_first_slot = PGT_FIRST;
    x86_pager_own(PGT_FIRST, PGT_PAGES);
    x86_pager_add_frames(test_ram + PGT_MEM_OFF, X86_PG_POOL_BYTES(5));
    for (uint32_t i = 0; i < PGT_PAGES; i++)
        guest_page_table[PGT_FIRST + i] = FOLD_INVALID;

    /* PINNED pages inside every candidate run refuse the whole donation.
       Frames are handed out from the top of the free list, so these two land
       in frames 4 and 3 -- and both runs of four contain frame 3. */
    uint32_t dr0 = x86_pg_donate_refused;
    x86_pager_fault(0xC0000u, 0);
    x86_pager_fault(0xC1000u, 0);
    x86_pin_pages(PGT_FIRST, 2);
    x86_livemap_clear();
    pager_r[45] = (x86_pager_donate_to_arena(8192u) == 0);
    pager_r[46] = x86_pg_donate_refused - dr0;           /* 1: loud */
    pager_r[47] = x86_pg_frames_total;                   /* 5: nothing moved */
    x86_unpin_pages(PGT_FIRST, 2);

    /* Asking for more than the reserve leaves is refused OUTRIGHT, not shrunk.
       Half a donation is a shrunken guest for a card that was never placed --
       the arena's own never-partially-satisfy rule. */
    pager_r[48] = (x86_pager_donate_to_arena(200000u) == 0);
    pager_r[49] = x86_pg_donate_refused - dr0;           /* 2 */

    /* WITH NO PAGEFILE, ONLY FREE FRAMES MAY BE DONATED. src/main.c wires no
       backing store today, so on the target this is the rule that actually
       governs: evicting a dirty page with nowhere to put it is data loss, and
       a bigger EGA card is not worth the guest's memory. */
    x86_pager_set_backing(NULL, NULL);
    pager_r[50] = (x86_pager_donate_to_arena(8192u) == 0);
    pager_r[51] = x86_pg_donate_refused - dr0;           /* 3 */
    x86_pager_set_backing(pgf_read, pgf_write);

    /* And now the real thing. The run with the FEWEST resident frames wins, so
       it takes frames 0-3 and evicts the one live page in it. */
    x86_livemap_clear();
    ev0 = x86_pg_evictions;
    pager_r[52] = x86_pager_donate_to_arena(8192u);      /* 16384 */
    pager_r[53] = x86_pg_donated;                        /* 4 frames */
    pager_r[54] = x86_pg_evictions - ev0;                /* 1: the cheapest run */
    pager_r[55] = x86_pg_frames_total;                   /* 1 left for the guest */
    pager_r[56] = x86_arena_bytes;                       /* 16384 */
    /* The guest still WORKS on what is left -- which is the whole safety
       argument for donating at all. */
    pager_r[57] = (x86_pager_fault(0xC3000u, 1) != 0);
    {
        void *blk = NULL;
        pager_r[58] = (x86_arena_claim(X86_ARENA_ID_EGA, 1, 16384u, &blk) == 0);
        /* The claim really lands in the donated extent -- an arena that grew
           its byte count without the memory being there would pass every
           check above. */
        pager_r[59] = (blk != NULL
                       && (uint8_t *)blk >= test_ram + PGT_MEM_OFF
                       && (uint8_t *)blk <  test_ram + PGT_MEM_OFF
                                            + X86_PG_POOL_BYTES(5));
        /* Reclaiming while the display holds it must FAIL, not take the pages
           out from under the blitter. */
        uint32_t rb0 = x86_pg_reclaim_busy;
        pager_r[60] = (x86_pager_reclaim_from_arena() != 0);
        pager_r[61] = x86_pg_reclaim_busy - rb0;         /* 1 */
        pager_r[62] = x86_pg_frames_total;               /* still 1 */
        x86_arena_release(X86_ARENA_ID_EGA);
    }
    /* Released: it comes back, and guest RAM is whole again. */
    pager_r[63] = (x86_pager_reclaim_from_arena() == 0);
    pager_r[64] = x86_pg_frames_total;                   /* 5 */
    pager_r[65] = x86_arena_bytes;                       /* 0 */
    pager_r[66] = x86_pg_donated;                        /* 0 */
    /* One extent again, not two: a mode change donates and reclaims every time
       0Dh comes and goes, and without the merge the pool would fragment into
       its four slots and refuse to take its own memory back. */
    pager_r[67] = (x86_pager_donate_to_arena(8192u) == 16384u);
    pager_r[68] = (x86_pager_reclaim_from_arena() == 0);
    pager_r[69] = (x86_pager_donate_to_arena(8192u) == 16384u);
    pager_r[70] = (x86_pager_reclaim_from_arena() == 0);
    pager_r[71] = x86_pg_frames_total;                   /* still 5 */

    /*
     * ---- THE RESERVE IS THE WORKING SET, NOT A CARD SIZE -------------------
     *
     * The KEEN4 wedge, scaled to what test_ram can hold: 8 frames, a 6-page
     * dirty working set, and a donation that asks for everything. The pool is
     * small and the numbers are not KEEN4's, but the RATIO is and so is the
     * failure -- a reserve under the working set turns every pass round it
     * into pagefile writes, which cost nothing here and up to 433 ms each on
     * silicon. See pager_donate_working_set().
     */
    pager_r[72] = pager_donate_working_set(8u, 6u, 6u, 200000u);   /* 0 */
    pager_r[73] = x86_pg_frames_total;                             /* 8 */
    /* THE NEGATIVE CONTROL, in the test rather than in a reviewer's head: the
       same pool and the same working set, with the reserve set BELOW it the
       way the shipping constant used to be. If this reads 0 the arm above is
       measuring nothing. */
    pager_r[74] = (pager_donate_working_set(8u, 6u, 4u, 200000u) > 0u);  /* 1 */
    pager_r[75] = (x86_pg_frames_total < 6u);                      /* 1 */
    /*
     * And the SHIPPING constant against the title that reaches this path. This
     * is the check that was absent: everything above passed with
     * X86_PG_DONATE_RESERVE at 100 because nothing compared it to the peak
     * recorded three paragraphs away from it in x86_pager.h.
     */
    pager_r[76] = (X86_PG_DONATE_RESERVE >= KEEN4_PEAK_FRAMES);    /* 1 */

    pager_policy_arms();

    x86_pager_set_backing(NULL, NULL);
    x86_pager_set_donate_reserve(X86_PG_DONATE_RESERVE);
    for (uint32_t i = 0; i < PGT_PAGES; i++)
        guest_page_table[PGT_FIRST + i] = FOLD_INVALID;
    x86_arena_reset();
    x86_pager_reset();
}

/* ==========================================================================
 * THE HMA, AND WHO OWNS IT ACROSS AN A20 TOGGLE.
 *
 * x86_mem_wrap_1mb() used to write FOLD_INVALID over the sixteen pages at 1 MB
 * whenever the gate was OPEN -- the HMA existed as an address range and as
 * nothing else. Two shipped titles stop exactly there: DOS4GW's memory hunt
 * opens A20 itself and reads through ES=0xFFFF, and Mortal Kombat's C runtime
 * runs the classic wrap test (REPE CMPSW against FFFF:0030) which on a 286+
 * with the gate open REQUIRES the two locations to differ. Both took
 * X86_EXIT_MEM_FAULT.
 *
 * What is graded here is not "the HMA is readable" -- that is one arm of six.
 * It is the OWNERSHIP RULE, which is the part that corrupts memory silently if
 * it is wrong:
 *
 *   gate OPEN    the sixteen pages are the pager's, through the same second
 *                window EMS uses. Ordinary pageable pages, filled from the
 *                ZERO tier on first touch, evictable, NOT pinned.
 *   gate MASKED  the ALIAS is the truth and the pager may not touch those
 *                entries at all -- not even to unmap a page it is evicting.
 *                The frames keep their HMA data behind the alias.
 *   reopened     every page that was resident goes back to ITS OWN frame.
 *
 * A guest that writes the HMA, masks A20 and reads low memory must see the
 * ALIAS. Getting that backwards is not a fault, it is wrong data, so the arm
 * that grades it reads a byte the two stories disagree about.
 *
 * AND THE SEAM APPLIES INSIDE THE HMA. Sixteen pages in FAULT order are
 * sixteen unrelated frames, so a 16-bit access straddling a boundary inside
 * the window is exactly docs/10's defect. The 1 MB line ITSELF is the other
 * half: page 255 is a FIXED mapping with no slack of its own, so a straddle
 * there is REFUSED and counted in x86_seam_overflow rather than served -- the
 * honest degradation, and the arm below proves the counter moves rather than
 * the access being silently wrong.
 * ========================================================================== */
#define HMA_FRAMES 4u

static uint32_t hma_run(const uint8_t *prog, uint32_t len)
{
    memcpy(test_ram, prog, len);
    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    for (int i = 0; i < 6; i++)
        cpu_state_dtcm.segs[i].limit = 0xFFFFFFFF;
    cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
    cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
    x86_cpu_interp_asm();
    return cpu_state_dtcm.exit_reason;
}

static void hma_test_run(void)
{
    for (uint32_t i = 0; i < 32; i++)
        hma_r[i] = 0;

    /* A machine at boot: gate masked, low memory identity-mapped, nothing
       above the HMA mapped at all. */
    x86_io.a20_open = 0;
    x86_io.kbd_cmd = 0;
    x86_io.port92 = 0;
    for (uint32_t i = X86_HMA_FIRST_PAGE; i < FOLD_ENTRIES; i++)
        guest_page_table[i] = FOLD_INVALID;

    x86_pager_reset();
    x86_livemap_clear();
    pgf_first_slot = X86_HMA_FIRST_PAGE;
    pgf_writes = pgf_reads = pgf_fail = 0;
    x86_pager_set_backing(pgf_read, pgf_write);
    /* Low memory stays the harness's fixed identity mapping; only the HMA is
       the pager's here, which is what the shipping wiring does too (the 640 KB
       window is x86_pager_own()'s and is not what this arm is about). */
    x86_pager_add_frames(test_ram + PGT_MEM_OFF, X86_PG_POOL_BYTES(HMA_FRAMES));
    x86_mem_wrap_1mb();

    uint32_t fw0 = x86_pg_faults_w, fz0 = x86_pg_fill_zero;

    /*
     *   MOV AL,11h / MOV [200h],AL      the low-memory marker -- and it is at
     *                                   0x200 because that is the byte
     *                                   0x100200 ALIASES ONTO when the gate is
     *                                   masked. The two arms must name the
     *                                   same byte through the two routes or
     *                                   the corruption arm grades nothing.
     *   MOV AL,2   / OUT 92h,AL         open the gate
     *   MOV AX,0FFFFh / MOV ES,AX
     *   MOV AL,22h / MOV ES:[210h],AL   linear 0x100200 -- the HMA
     *   MOV AL,ES:[210h] / MOV [201h],AL read it back
     *   HLT
     */
    static const uint8_t prog_open[] = {
        0xB0, 0x11,  0xA2, 0x00, 0x02,
        0xB0, 0x02,  0xE6, 0x92,
        0xB8, 0xFF, 0xFF,  0x8E, 0xC0,
        0xB0, 0x22,  0x26, 0xA2, 0x10, 0x02,
        0x26, 0xA0, 0x10, 0x02,  0xA2, 0x01, 0x02,
        0xF4,
    };
    hma_r[0] = hma_run(prog_open, sizeof(prog_open));      /* 6 = HLT, not 5 */
    hma_r[1] = test_ram[0x201];                            /* 0x22: it stuck */
    /* THE CORRUPTION ARM. If the HMA were still aliased with the gate OPEN,
       this byte would now be 0x22 instead of the 0x11 the guest wrote. */
    hma_r[2] = test_ram[0x200];                            /* 0x11 */
    hma_r[3] = x86_pg_faults_w - fw0;                      /* 1 */
    hma_r[4] = x86_pg_fill_zero - fz0;                     /* 1: ZERO tier */
    hma_r[5] = x86_pg_resident;                            /* 1 */
    hma_r[6] = (guest_page_table[X86_HMA_FIRST_PAGE] != FOLD_INVALID);
    hma_r[7] = x86_a20_state;                              /* 1 */

    /*
     * Mask the gate again. The alias must be back -- and a write through it
     * must land in LOW memory, not in the HMA frame sitting behind it.
     *
     *   MOV AL,0 / OUT 92h,AL
     *   MOV AX,0FFFFh / MOV ES,AX
     *   MOV AL,ES:[210h] / MOV [202h],AL    must read 0x11, the ALIAS
     *   MOV AL,33h / MOV ES:[210h],AL       write through the alias
     *   MOV AL,[200h] / MOV [203h],AL       must read 0x33: it went to low RAM
     *   HLT
     */
    fw0 = x86_pg_faults_w;
    static const uint8_t prog_mask[] = {
        0xB0, 0x00,  0xE6, 0x92,
        0xB8, 0xFF, 0xFF,  0x8E, 0xC0,
        0x26, 0xA0, 0x10, 0x02,  0xA2, 0x02, 0x02,
        0xB0, 0x33,  0x26, 0xA2, 0x10, 0x02,
        0xA0, 0x00, 0x02,  0xA2, 0x03, 0x02,
        0xF4,
    };
    hma_r[8]  = hma_run(prog_mask, sizeof(prog_mask));     /* 6 = HLT */
    hma_r[9]  = test_ram[0x202];                           /* 0x11 -- the ALIAS */
    hma_r[10] = test_ram[0x203];                           /* 0x33 */
    hma_r[11] = x86_pg_faults_w - fw0;                     /* 0: nothing faulted */
    hma_r[12] = x86_pg_resident;                           /* 1: the frame stays */

    /*
     * Reopen. The HMA page goes back to ITS OWN frame with the byte still in
     * it -- not refilled, not lost, and not the 0x33 the alias took.
     *
     *   MOV AL,2 / OUT 92h,AL / ES=0FFFFh / MOV AL,ES:[210h] / MOV [204h],AL
     */
    fz0 = x86_pg_fill_zero;
    static const uint8_t prog_reopen[] = {
        0xB0, 0x02,  0xE6, 0x92,
        0xB8, 0xFF, 0xFF,  0x8E, 0xC0,
        0x26, 0xA0, 0x10, 0x02,  0xA2, 0x04, 0x02,
        0xF4,
    };
    hma_r[13] = hma_run(prog_reopen, sizeof(prog_reopen));
    hma_r[14] = test_ram[0x204];                           /* 0x22, still */
    hma_r[15] = x86_pg_fill_zero - fz0;                    /* 0: never refilled */
    hma_r[16] = test_ram[0x200];                           /* 0x33 -- low RAM kept
                                                              what the alias wrote */

    /*
     * THE SEAM, INSIDE THE HMA. A 16-bit access at linear 0x100FFF spans pages
     * 256 and 257, which hold two unrelated frames (frames come off the free
     * list from the top, so 257's host is BELOW 256's -- genuinely not
     * contiguous, which is the only condition under which the seam is visible).
     *
     *   ES=0FFFFh / MOV AX,1234h / MOV ES:[100Fh],AX     -> 0x100FFF
     *   MOV AX,0 / MOV AX,ES:[100Fh] / MOV [205h],AX
     *   MOV AL,ES:[1010h] / MOV [207h],AL                -> 0x101000, page 257
     */
    uint32_t s0 = x86_seam_syncs, o0 = x86_seam_overflow;
    static const uint8_t prog_seam[] = {
        0xB8, 0xFF, 0xFF,  0x8E, 0xC0,
        0xB8, 0x34, 0x12,  0x26, 0xA3, 0x0F, 0x10,
        0xB8, 0x00, 0x00,  0x26, 0xA1, 0x0F, 0x10,  0xA3, 0x05, 0x02,
        0x26, 0xA0, 0x10, 0x10,  0xA2, 0x07, 0x02,
        0xF4,
    };
    hma_r[17] = hma_run(prog_seam, sizeof(prog_seam));
    hma_r[18] = (uint32_t)(test_ram[0x205] | (test_ram[0x206] << 8)); /* 0x1234 */
    /* The high byte physically in the NEXT page's frame, read back through a
       separate byte access: a straddle "served" out of the low frame's slack
       and never pushed would pass the arm above and fail this one. */
    hma_r[19] = test_ram[0x207];                           /* 0x12 */
    hma_r[20] = (x86_seam_syncs > s0);                     /* it was SERVED */
    hma_r[21] = x86_seam_overflow - o0;                    /* 0: and not refused */
    hma_r[22] = x86_pg_resident;                           /* 2 */

    /*
     * THE 1 MB LINE ITSELF. Page 255 is a FIXED mapping on the real machine
     * (BIOS/open-bus), so it has no slack of its own and its neighbour is not
     * the seam's to write. A 16-bit read at 0xFFFFF is therefore REFUSED and
     * COUNTED -- the honest degradation docs/10 specifies, and the arm exists
     * so that "counted" is a fact rather than an assumption.
     */
    guest_page_table[255] = FOLD_K((uint32_t)test_ram + 100u * 4096u, 255);
    test_ram[100u * 4096u + 0xFFFu] = 0xA5;
    o0 = x86_seam_overflow;
    static const uint8_t prog_line[] = {
        0xB8, 0x00, 0xF0,  0x8E, 0xC0,
        0x26, 0xA1, 0xFF, 0xFF,  0xA3, 0x08, 0x02,
        0xF4,
    };
    hma_r[23] = hma_run(prog_line, sizeof(prog_line));     /* 6 = HLT, degraded */
    hma_r[24] = x86_seam_overflow - o0;                    /* 1: refused, LOUDLY */
    hma_r[25] = test_ram[0x208];                           /* 0xA5: the low byte
                                                              is still right */

    /* The window itself, asked directly. Open: the HMA is ours. Masked: it is
       not, and a fault there is x86_pg_unmapped, not a frame. */
    hma_r[26] = (x86_pager_fault(0x101000u, 0) != 0);      /* 1 */
    x86_a20_set(0);
    uint32_t um0 = x86_pg_unmapped;
    hma_r[27] = (x86_pager_fault(0x101000u, 0) == 0);      /* 1 */
    hma_r[28] = x86_pg_unmapped - um0;                     /* 1 */
    /* And the alias survived every one of those transitions. */
    hma_r[29] = (guest_page_table[X86_HMA_FIRST_PAGE]
                 == guest_page_table[0] - (X86_HMA_FIRST_PAGE << FOLD_SHIFT));

    /* Put the machine back the way the rest of the harness expects it. */
    guest_page_table[255] = FOLD_INVALID;
    x86_pager_reset();
    x86_livemap_clear();
    x86_pager_set_backing(NULL, NULL);
    for (uint32_t i = X86_HMA_FIRST_PAGE; i < FOLD_ENTRIES; i++)
        guest_page_table[i] = FOLD_INVALID;
    x86_mem_wrap_1mb();
    hma_r[30] = x86_io.a20_open;                           /* 0 */
    hma_r[31] = x86_a20_state;                             /* 0 */
}


/* ===========================================================================
 * XMS, driven by GUEST CODE doing the real sequence.
 *
 * INT 2Fh AX=4300h -> AX=4310h -> CALL FAR the entry point -> allocate -> move
 * a pattern up -> move it back down -> free -> query. Nothing here calls
 * x86_xms_control() directly, because the thing most likely to be wrong is not
 * the arithmetic inside a function -- it is the entry point being a real
 * address with a stub that returns by RETF, and a guest is the only witness to
 * that.
 *
 * THE BUFFERS STRADDLE A GRANULE BOUNDARY, IN BOTH DIRECTIONS, and they are in
 * PAGER-OWNED pages whose frames are primed OUT OF ORDER on purpose. `host =
 * lin + k` with one k per granule, so with the frames shuffled guest page N+1
 * is genuinely not the host memory after page N -- which is the only condition
 * under which the seam can be seen at all (see the seam arm's note below on why
 * every other tier-1 fold hides it). The source of the up-move spans one
 * boundary and the destination of the down-move spans another.
 *
 * The store is a plain RAM buffer standing in for the pagefile, exactly as the
 * pager arm's pgf_read/pgf_write do, and for the same reason: the whole
 * interface is provable with no card, no FatFs and no firmware.
 * ===========================================================================
 */
volatile uint32_t run_xms_flag = 0;
volatile uint32_t xms_r[48];

/* Reuses the pagefile buffer's 32 KB -- the arms never run at the same time,
   and test_ram has no spare region (it is 752 KB and fully spoken for). */
#define XMS_STORE_OFF  PGF_OFF
#define XMS_STORE_KB   32u
#define XMS_STORE_BYTES (XMS_STORE_KB * 1024u)

static uint32_t xmsb_opens, xmsb_fail;
static int xmsb_open(int write)   { (void)write; xmsb_opens++; return 0; }
static void xmsb_close(void)      { }
static int xmsb_read(uint32_t off, void *dst, uint32_t bytes)
{
    if (off > XMS_STORE_BYTES || XMS_STORE_BYTES - off < bytes)
        { xmsb_fail++; return -1; }
    memcpy(dst, test_ram + XMS_STORE_OFF + off, bytes);
    return 0;
}
static int xmsb_write(uint32_t off, const void *src, uint32_t bytes)
{
    if (off > XMS_STORE_BYTES || XMS_STORE_BYTES - off < bytes)
        { xmsb_fail++; return -1; }
    memcpy(test_ram + XMS_STORE_OFF + off, src, bytes);
    return 0;
}

#define XMS_CODE_A   0x3000u
#define XMS_CODE_B   0x3100u
#define XMS_CODE_C   0x3180u
#define XMS_RES      0x3200u        /* 32 result words */
#define XMS_ENTRY    0x3300u        /* the far pointer the guest CALLs         */
#define XMS_S1       0x3310u        /* move: conventional -> XMS               */
#define XMS_S2       0x3330u        /* move: XMS -> conventional               */
#define XMS_S3       0x3350u        /* the two moves that must FAIL            */

/* The guest buffers. 0xC0FE0 runs into page 0xC1, 0xC2FE0 into page 0xC3. */
#define XMS_SRC_LIN  0xC0FE0u
#define XMS_DST_LIN  0xC2FE0u
#define XMS_PAT_LEN  64u

static const uint8_t xms_code_a[] = {
    0xB8, 0x00, 0x43,             /* MOV AX,4300h                            */
    0xCD, 0x2F,                   /* INT 2Fh    -- detect                    */
    0xA3, 0x00, 0x32,             /* MOV [3200],AX   r0: AL must be 80h      */
    0xB8, 0x10, 0x43,             /* MOV AX,4310h                            */
    0xCD, 0x2F,                   /* INT 2Fh    -- get the entry point       */
    0x89, 0x1E, 0x00, 0x33,       /* MOV [3300],BX   the far pointer we CALL */
    0x8C, 0x06, 0x02, 0x33,       /* MOV [3302],ES                           */
    0x8C, 0x06, 0x02, 0x32,       /* MOV [3202],ES   r1                      */
    0x89, 0x1E, 0x04, 0x32,       /* MOV [3204],BX   r2                      */
    0xB4, 0x00,                   /* MOV AH,00h -- version                   */
    0xFF, 0x1E, 0x00, 0x33,       /* CALL FAR [3300]                         */
    0xA3, 0x06, 0x32,             /* r3: 0200h                               */
    0x89, 0x16, 0x08, 0x32,       /* r4: HMA flag, 0                         */
    0xB4, 0x08,                   /* MOV AH,08h -- free memory BEFORE        */
    0xFF, 0x1E, 0x00, 0x33,
    0xA3, 0x0A, 0x32,             /* r5: largest KB                          */
    0x89, 0x16, 0x0C, 0x32,       /* r6: total KB                            */
    0xB4, 0x09,                   /* MOV AH,09h -- allocate 8 KB             */
    0xBA, 0x08, 0x00,
    0xFF, 0x1E, 0x00, 0x33,
    0xA3, 0x0E, 0x32,             /* r7: 1                                   */
    0x89, 0x16, 0x10, 0x32,       /* r8: the handle                          */
    0x89, 0x16, 0x1A, 0x33,       /* patch S1 destination handle             */
    0x89, 0x16, 0x34, 0x33,       /* patch S2 source handle                  */
    0xB4, 0x0B,                   /* MOV AH,0Bh -- move UP                   */
    0xBE, 0x10, 0x33,             /* MOV SI,3310h                            */
    0xFF, 0x1E, 0x00, 0x33,
    0xA3, 0x12, 0x32,             /* r9: 1                                   */
    0x88, 0x1E, 0x14, 0x32,       /* r10: BL, 0                              */
    0xB4, 0x0B,                   /* MOV AH,0Bh -- move DOWN                 */
    0xBE, 0x30, 0x33,             /* MOV SI,3330h                            */
    0xFF, 0x1E, 0x00, 0x33,
    0xA3, 0x16, 0x32,             /* r11: 1                                  */
    0x88, 0x1E, 0x18, 0x32,       /* r12: BL, 0                              */
    0xB4, 0x0E,                   /* MOV AH,0Eh -- handle information        */
    0x8B, 0x16, 0x10, 0x32,
    0xFF, 0x1E, 0x00, 0x33,
    0xA3, 0x1A, 0x32,             /* r13: 1                                  */
    0x89, 0x1E, 0x1C, 0x32,       /* r14: BH locks, BL free handles          */
    0x89, 0x16, 0x1E, 0x32,       /* r15: block size, 8 KB                   */
    0xB4, 0x08,                   /* MOV AH,08h -- free memory WHILE held    */
    0xFF, 0x1E, 0x00, 0x33,
    0xA3, 0x20, 0x32,             /* r16                                     */
    0x89, 0x16, 0x22, 0x32,       /* r17                                     */
    0xB4, 0x03,                   /* MOV AH,03h -- global enable A20         */
    0xFF, 0x1E, 0x00, 0x33,
    0xB4, 0x07,                   /* MOV AH,07h -- query A20                 */
    0xFF, 0x1E, 0x00, 0x33,
    0xA3, 0x24, 0x32,             /* r18: 1                                  */
    0xB4, 0x04,                   /* MOV AH,04h -- global disable A20        */
    0xFF, 0x1E, 0x00, 0x33,
    0xB4, 0x07,
    0xFF, 0x1E, 0x00, 0x33,
    0xA3, 0x26, 0x32,             /* r19: 0                                  */
    0xF4,                         /* HLT                                     */
};

/* Phase B: the SAME down-move again, after the host has corrupted one byte of
   the store. The negative control -- it must come back different. */
static const uint8_t xms_code_b[] = {
    0xB4, 0x0B,
    0xBE, 0x30, 0x33,
    0xFF, 0x1E, 0x00, 0x33,
    0xA3, 0x28, 0x32,             /* r20: still reports success              */
    0xF4,
};

/* Phase C: the refusals, then free, then the queries that must show the memory
   come back, then an allocation bigger than the store. */
static const uint8_t xms_code_c[] = {
    0xC7, 0x06, 0x50, 0x33, 0x03, 0x00,   /* S3 length = 3 -- ODD           */
    0xB4, 0x0B,
    0xBE, 0x50, 0x33,
    0xFF, 0x1E, 0x00, 0x33,
    0xA3, 0x2A, 0x32,             /* r21: 0                                  */
    0x88, 0x1E, 0x2C, 0x32,       /* r22: A7h invalid length                 */
    0xC7, 0x06, 0x50, 0x33, 0x40, 0x00,   /* S3 length = 64                  */
    0xC7, 0x06, 0x54, 0x33, 0x09, 0x00,   /* S3 source handle = 9, unissued  */
    0xB4, 0x0B,
    0xBE, 0x50, 0x33,
    0xFF, 0x1E, 0x00, 0x33,
    0xA3, 0x2E, 0x32,             /* r23: 0                                  */
    0x88, 0x1E, 0x30, 0x32,       /* r24: A3h invalid source handle          */
    0xB4, 0x0C,                   /* MOV AH,0Ch -- lock, which we refuse     */
    0x8B, 0x16, 0x10, 0x32,
    0xFF, 0x1E, 0x00, 0x33,
    0xA3, 0x32, 0x32,             /* r25: 0                                  */
    0x88, 0x1E, 0x34, 0x32,       /* r26: ADh lock failed                    */
    0xB4, 0x0A,                   /* MOV AH,0Ah -- free                      */
    0x8B, 0x16, 0x10, 0x32,
    0xFF, 0x1E, 0x00, 0x33,
    0xA3, 0x36, 0x32,             /* r27: 1                                  */
    0xB4, 0x08,                   /* MOV AH,08h -- free memory AFTER         */
    0xFF, 0x1E, 0x00, 0x33,
    0xA3, 0x38, 0x32,             /* r28: largest, back to the whole store   */
    0x89, 0x16, 0x3A, 0x32,       /* r29: total                              */
    0xB4, 0x09,                   /* MOV AH,09h -- ask for more than exists  */
    0xBA, 0x00, 0x10,             /* MOV DX,4096                             */
    0xFF, 0x1E, 0x00, 0x33,
    0xA3, 0x3C, 0x32,             /* r30: 0                                  */
    0x88, 0x1E, 0x3E, 0x32,       /* r31: A0h all extended memory allocated  */
    0xF4,
};

static void xms_put32(uint32_t off, uint32_t v)
{
    test_ram[off + 0] = (uint8_t)v;
    test_ram[off + 1] = (uint8_t)(v >> 8);
    test_ram[off + 2] = (uint8_t)(v >> 16);
    test_ram[off + 3] = (uint8_t)(v >> 24);
}
static void xms_put16(uint32_t off, uint16_t v)
{
    test_ram[off + 0] = (uint8_t)v;
    test_ram[off + 1] = (uint8_t)(v >> 8);
}

/* Fault a page in, in an order the caller chooses. Frames are handed out in
   FAULT order, so this is how the shuffle is arranged. */
static void xms_prime(uint32_t lin)
{
    uint8_t *p = (uint8_t *)x86_guest_ptr(lin, 1);
    if (p)
        *p = 0;
}

static void xms_test_run(void)
{
    static const uint8_t hle_vectors[] = {
        0x08, 0x09, 0x10, 0x11, 0x12, 0x15, 0x16, 0x1A, 0x20, 0x21, 0x2F,
    };
    uint8_t pat[XMS_PAT_LEN], got[XMS_PAT_LEN];

    for (uint32_t i = 0; i < 48; i++)
        xms_r[i] = 0;

    /* ---- the store, and the guest pages the moves run against ---------- */
    memset(test_ram + XMS_STORE_OFF, 0xEE, XMS_STORE_BYTES);
    xmsb_opens = xmsb_fail = 0;
    x86_xms_set_backing(xmsb_open, xmsb_close, xmsb_read, xmsb_write,
                        XMS_STORE_KB);

    x86_pager_reset();
    x86_pager_set_backing(NULL, NULL);    /* four frames, four pages: no
                                             eviction is possible, so nothing
                                             can reach the store but XMS */
    x86_livemap_clear();
    x86_pager_own(PGT_FIRST, PGT_PAGES);
    x86_pager_add_frames(test_ram + PGT_MEM_OFF, X86_PG_POOL_BYTES(4));
    for (uint32_t i = 0; i < PGT_PAGES; i++)
        guest_page_table[PGT_FIRST + i] = FOLD_INVALID;

    /* OUT OF ORDER, deliberately: 0xC1 before 0xC0 and 0xC3 before 0xC2, so
       each buffer's second granule sits BELOW its first in host memory. */
    xms_prime(0xC1000u);
    xms_prime(0xC0000u);
    xms_prime(0xC3000u);
    xms_prime(0xC2000u);
    /* The premise this whole arm rests on, asserted rather than assumed. */
    xms_r[32] = (guest_page_table[0xC0] != guest_page_table[0xC1]
                 && guest_page_table[0xC2] != guest_page_table[0xC3]);

    for (uint32_t i = 0; i < XMS_PAT_LEN; i++)
        pat[i] = (uint8_t)(i * 7u + 3u);
    memset(got, 0, sizeof(got));
    x86_guest_copy_in(XMS_SRC_LIN, pat, XMS_PAT_LEN);
    x86_guest_copy_in(XMS_DST_LIN, got, XMS_PAT_LEN);

    /* ---- low memory: the stubs, the structures, the code --------------- */
    memset(test_ram + XMS_RES, 0, 0x180);
    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));

    /* S1: 64 bytes, conventional 0xC0FE:0000 -> handle (patched), offset 0. */
    xms_put32(XMS_S1 + 0,  XMS_PAT_LEN);
    xms_put16(XMS_S1 + 4,  0);
    xms_put32(XMS_S1 + 6,  ((XMS_SRC_LIN >> 4) << 16));
    xms_put16(XMS_S1 + 10, 0);
    xms_put32(XMS_S1 + 12, 0);
    /* S2: 64 bytes, handle (patched) offset 0 -> conventional 0xC2FE:0000. */
    xms_put32(XMS_S2 + 0,  XMS_PAT_LEN);
    xms_put16(XMS_S2 + 4,  0);
    xms_put32(XMS_S2 + 6,  0);
    xms_put16(XMS_S2 + 10, 0);
    xms_put32(XMS_S2 + 12, ((XMS_DST_LIN >> 4) << 16));
    /* S3: filled in by the guest for each refusal it provokes. */
    memset(test_ram + XMS_S3, 0, 16);
    xms_put32(XMS_S3 + 12, ((XMS_DST_LIN >> 4) << 16));

    memcpy(&test_ram[XMS_CODE_A], xms_code_a, sizeof(xms_code_a));
    memcpy(&test_ram[XMS_CODE_B], xms_code_b, sizeof(xms_code_b));
    memcpy(&test_ram[XMS_CODE_C], xms_code_c, sizeof(xms_code_c));

    uint32_t a20_0 = x86_a20_changes;

    /* ---- phase A ------------------------------------------------------- */
    kbd_test_reset_cpu(XMS_CODE_A);
    xms_r[33] = 0;
    if (kbd_test_run_to_halt() != X86_EXIT_HALT)
        return;                            /* every result stays 0: LOUD */

    /* THE ASSERTION THIS ARM EXISTS FOR: the pattern went up and came back
       down byte for byte, across a granule boundary at each end. */
    x86_guest_copy_out(got, XMS_DST_LIN, XMS_PAT_LEN);
    xms_r[33] = (memcmp(got, pat, XMS_PAT_LEN) == 0);
    /* And nothing outside the 64 bytes was written -- a move that overran its
       block would pass the compare above and still be wrong. */
    {
        int clean = 1;
        for (uint32_t i = XMS_PAT_LEN; i < 256u; i++)
            if (test_ram[XMS_STORE_OFF + i] != 0xEE)
                clean = 0;
        xms_r[44] = clean;
    }

    /* ---- phase B: the negative control --------------------------------- */
    /* One byte of the store, flipped. If the compare in xms_r[33] is real,
       the same move must now bring back something that does NOT match. */
    test_ram[XMS_STORE_OFF + 40] ^= 0xFF;
    kbd_test_reset_cpu(XMS_CODE_B);
    if (kbd_test_run_to_halt() == X86_EXIT_HALT) {
        x86_guest_copy_out(got, XMS_DST_LIN, XMS_PAT_LEN);
        xms_r[34] = (memcmp(got, pat, XMS_PAT_LEN) != 0);
    }
    test_ram[XMS_STORE_OFF + 40] ^= 0xFF;

    /* ---- phase C ------------------------------------------------------- */
    kbd_test_reset_cpu(XMS_CODE_C);
    (void)kbd_test_run_to_halt();

    /*
     * THE SECOND NEGATIVE CONTROL, and it is the bug this implementation had to
     * avoid: resolve ONE host pointer for the whole 64 bytes and memcpy through
     * it, exactly as `memcpy(guest_ram + offset, ...)` would. With the frames
     * shuffled the tail of that copy lands in another page's frame, so reading
     * the range back through the granule-aware accessor CANNOT match. If this
     * ever reports a match, the arm's fold is contiguous and every seam claim
     * above it is vacuous.
     */
    {
        uint8_t zero[XMS_PAT_LEN];
        memset(zero, 0, sizeof(zero));
        x86_guest_copy_in(XMS_DST_LIN, zero, XMS_PAT_LEN);
        uint8_t *p = (uint8_t *)x86_guest_ptr(XMS_DST_LIN, 1);
        if (p) {
            memcpy(p, pat, XMS_PAT_LEN);
            x86_guest_copy_out(got, XMS_DST_LIN, XMS_PAT_LEN);
            xms_r[35] = (memcmp(got, pat, XMS_PAT_LEN) != 0);
        }
    }

    for (uint32_t i = 0; i < 32; i++)
        xms_r[i] = (uint32_t)(test_ram[XMS_RES + i * 2]
                              | (test_ram[XMS_RES + i * 2 + 1] << 8));

    xms_r[36] = x86_xms_moves;
    xms_r[37] = x86_xms_move_up;
    xms_r[38] = x86_xms_move_down;
    xms_r[39] = x86_xms_io_errors + xmsb_fail;
    xms_r[40] = x86_xms_guest_errors;
    xms_r[41] = xmsb_opens;
    xms_r[42] = x86_a20_changes - a20_0;
    xms_r[43] = x86_xms_total_kb;
    xms_r[45] = x86_xms_handles_used;
    xms_r[46] = x86_xms_detects;
    xms_r[47] = x86_xms_entry_reqs;

    x86_xms_set_backing(NULL, NULL, NULL, NULL, 0);
    for (uint32_t i = 0; i < PGT_PAGES; i++)
        guest_page_table[PGT_FIRST + i] = FOLD_INVALID;
    x86_pager_reset();
}

/* ===========================================================================
 * EMS -- the MAPPED interface, driven by GUEST CODE doing the real sequence.
 *
 * x86_ems.h states the design point: XMS is a COPY interface and its bytes are
 * never in the fold table, EMS is a MAPPED one and the four windows' bytes must
 * be. This arm is the proof of the second half, and the only witness that can
 * give it is a guest executing ordinary STOSW and MOV AX,ES:[SI] against
 * E000:0000 -- nothing here calls x86_ems_int67() directly.
 *
 * THE PINNING PROPERTY IS ASSERTED, NOT ASSUMED, AND IT HAS A CONTROL. The pool
 * is twelve frames for twenty-four backing pages, so pressure MUST evict. With
 * logical page 0 mapped into window 0 its four granules are pinned and must
 * come through untouched; logical page 1 is allocated but UNMAPPED, so it is an
 * ordinary pager page and must be evicted by the same pressure. If the pin were
 * a no-op the two would behave the same, which is what makes ems_r[32] a
 * control rather than a restatement of ems_r[31].
 * ===========================================================================
 */
volatile uint32_t run_ems_flag = 0;
volatile uint32_t ems_r[40];

/* All three regions come out of the arena arm's 160 KB (512-672 KB): the arms
   never run at the same time and test_ram has no spare, which is the same
   trade the XMS arm makes with the pagefile buffer. */
#define EMS_FILL_OFF    (496u * 1024u)      /* the open-bus filler page      */
#define EMS_POOL_OFF    (512u * 1024u)
#define EMS_FRAMES      12u
#define EMS_STORE_OFF   (576u * 1024u)
#define EMS_TEST_PAGES  6u                  /* logical pages advertised      */
#define EMS_SUBS        X86_EMS_SUBPAGES
#define EMS_STORE_SLOTS (EMS_TEST_PAGES * EMS_SUBS)           /* 24 */
#define EMS_FRAME_IDX   (X86_EMS_FRAME_LIN >> 12)

static uint32_t emsb_fail;
static int emsb_read(uint32_t slot, void *dst)
{
    uint32_t i = slot - X86_EMS_BACK_PAGE;
    if (i >= EMS_STORE_SLOTS) { emsb_fail++; return -1; }
    memcpy(dst, test_ram + EMS_STORE_OFF + i * 4096u, 4096u);
    return 0;
}
static int emsb_write(uint32_t slot, const void *src)
{
    uint32_t i = slot - X86_EMS_BACK_PAGE;
    if (i >= EMS_STORE_SLOTS) { emsb_fail++; return -1; }
    memcpy(test_ram + EMS_STORE_OFF + i * 4096u, src, 4096u);
    return 0;
}

#define EMS_RES     0x5000u
#define EMS_NAME    0x4F00u
#define EMS_CODE_A  0x4000u
#define EMS_CODE_B  0x4400u
#define EMS_CODE_C  0x4600u
#define EMS_CODE_D  0x4800u

/* ---- phase A: detect, allocate, map, WRITE THROUGH THE WINDOW, remap ----
 * Results are words at EMS_RES + 2*n; the index in each comment is that n.
 */
static const uint8_t ems_code_a[] = {
    0xB4, 0x3D, 0xB0, 0x00,             /* MOV AH,3Dh / MOV AL,0            */
    0xBA, 0x00, 0x4F,                   /* MOV DX,EMS_NAME                  */
    0xCD, 0x21,                         /* INT 21h -- open EMMXXXX0         */
    0xA3, 0x00, 0x50,                   /* 0: the DOS handle                */
    0x19, 0xC0,                         /* SBB AX,AX -- capture CF          */
    0xA3, 0x02, 0x50,                   /* 1: 0 = CF clear = it opened      */
    0xB4, 0x3E, 0x8B, 0x1E, 0x00, 0x50, /* MOV AH,3Eh / MOV BX,[handle]     */
    0xCD, 0x21,                         /* INT 21h -- close it again        */

    0xB4, 0x40, 0xCD, 0x67,             /* AH=40h manager status            */
    0x88, 0x26, 0x04, 0x50,             /* 2: status, must be 0             */
    0xB4, 0x41, 0xCD, 0x67,             /* AH=41h page frame segment        */
    0x89, 0x1E, 0x06, 0x50,             /* 3: BX -- the frame               */
    0x88, 0x26, 0x08, 0x50,             /* 4: status                        */
    0xB4, 0x42, 0xCD, 0x67,             /* AH=42h pages free / total        */
    0x89, 0x1E, 0x0A, 0x50,             /* 5: free                          */
    0x89, 0x16, 0x0C, 0x50,             /* 6: total                         */
    0xB4, 0x43, 0xBB, 0x04, 0x00,       /* AH=43h allocate 4 pages          */
    0xCD, 0x67,
    0x88, 0x26, 0x0E, 0x50,             /* 7: status                        */
    0x89, 0x16, 0x10, 0x50,             /* 8: the EMS handle                */

    /* map logical page 0 into physical window 0 */
    0xB8, 0x00, 0x44, 0xBB, 0x00, 0x00,
    0x8B, 0x16, 0x10, 0x50, 0xCD, 0x67,
    0x88, 0x26, 0x12, 0x50,             /* 9: status                        */

    /* ---- THE POINT OF THE ARM: 16 KB written with ORDINARY STORES ----
       The value at each offset is (offset & 3FFFh) ^ key, so the same loop
       verifies it in ANY window -- which is what phase C needs. 16 KB is four
       granules, so this crosses three seams in each direction. */
    0xB8, 0x00, 0xE0, 0x8E, 0xC0,       /* MOV AX,E000h / MOV ES,AX         */
    0xBF, 0x00, 0x00,                   /* MOV DI,0                         */
    0xB9, 0x00, 0x20,                   /* MOV CX,8192                      */
    0x8B, 0xC7, 0x25, 0xFF, 0x3F,       /* MOV AX,DI / AND AX,3FFFh         */
    0x35, 0x5A, 0x5A, 0xAB,             /* XOR AX,5A5Ah / STOSW             */
    0xE2, 0xF5,                         /* LOOP                             */

    /* remap the SAME window to logical page 1 and write a different key */
    0xB8, 0x00, 0x44, 0xBB, 0x01, 0x00,
    0x8B, 0x16, 0x10, 0x50, 0xCD, 0x67,
    0x88, 0x26, 0x14, 0x50,             /* 10: status                       */
    0xBF, 0x00, 0x00, 0xB9, 0x00, 0x20,
    0x8B, 0xC7, 0x25, 0xFF, 0x3F,
    0x35, 0xA3, 0xA3, 0xAB,
    0xE2, 0xF5,

    /* map logical page 0 BACK, and read it with ordinary loads */
    0xB8, 0x00, 0x44, 0xBB, 0x00, 0x00,
    0x8B, 0x16, 0x10, 0x50, 0xCD, 0x67,
    0x88, 0x26, 0x16, 0x50,             /* 11: status                       */
    0xBE, 0x00, 0x00,                   /* MOV SI,0                         */
    0xB9, 0x00, 0x20,                   /* MOV CX,8192                      */
    0xBB, 0x00, 0x00,                   /* MOV BX,0 -- the accumulator      */
    0x26, 0x8B, 0x04,                   /* MOV AX,ES:[SI]                   */
    0x8B, 0xEE, 0x81, 0xE5, 0xFF, 0x3F, /* MOV BP,SI / AND BP,3FFFh         */
    0x33, 0xC5, 0x35, 0x5A, 0x5A,       /* XOR AX,BP / XOR AX,5A5Ah         */
    0x0B, 0xD8, 0x46, 0x46,             /* OR BX,AX / INC SI / INC SI       */
    0xE2, 0xEC,                         /* LOOP                             */
    0x89, 0x1E, 0x18, 0x50,             /* 12: 0 iff every word matched     */
    0xF4,
};

/* ---- phase B: the same verify, re-run after the pool has been squeezed --- */
static const uint8_t ems_code_b[] = {
    0xB8, 0x00, 0xE0, 0x8E, 0xC0,
    0xBE, 0x00, 0x00, 0xB9, 0x00, 0x20, 0xBB, 0x00, 0x00,
    0x26, 0x8B, 0x04,
    0x8B, 0xEE, 0x81, 0xE5, 0xFF, 0x3F,
    0x33, 0xC5, 0x35, 0x5A, 0x5A,
    0x0B, 0xD8, 0x46, 0x46,
    0xE2, 0xEC,
    0x89, 0x1E, 0x1A, 0x50,             /* 13                               */
    0xF4,
};

/* ---- phase C: logical page 1 into window 1, and it must have SURVIVED ----
 * It was unmapped throughout the pressure above, so it was evicted to the
 * store and has to come back from it. That is the whole cost curve this design
 * exists for: cold pages pay, mapped ones do not.
 */
static const uint8_t ems_code_c[] = {
    0xB8, 0x01, 0x44, 0xBB, 0x01, 0x00,
    0x8B, 0x16, 0x10, 0x50, 0xCD, 0x67,
    0x88, 0x26, 0x1C, 0x50,             /* 14: status                       */
    0xB8, 0x00, 0xE0, 0x8E, 0xC0,
    0xBE, 0x00, 0x40,                   /* MOV SI,4000h -- window 1         */
    0xB9, 0x00, 0x20, 0xBB, 0x00, 0x00,
    0x26, 0x8B, 0x04,
    0x8B, 0xEE, 0x81, 0xE5, 0xFF, 0x3F,
    0x33, 0xC5, 0x35, 0xA3, 0xA3,
    0x0B, 0xD8, 0x46, 0x46,
    0xE2, 0xEC,
    0x89, 0x1E, 0x1E, 0x50,             /* 15                               */
    0xF4,
};

/* ---- phase D: the refusals, the queries, and the memory coming back ----- */
static const uint8_t ems_code_d[] = {
    0xB8, 0x04, 0x44,                   /* AH=44h AL=4 -- no such window    */
    0x8B, 0x16, 0x10, 0x50, 0xBB, 0x00, 0x00, 0xCD, 0x67,
    0x88, 0x26, 0x20, 0x50,             /* 16: 8Bh physical page out of range */
    0xB8, 0x00, 0x44,                   /* AH=44h BX=99 -- no such page     */
    0x8B, 0x16, 0x10, 0x50, 0xBB, 0x63, 0x00, 0xCD, 0x67,
    0x88, 0x26, 0x22, 0x50,             /* 17: 8Ah logical page out of range */
    0xB4, 0x4C, 0xBA, 0x09, 0x00, 0xCD, 0x67,
    0x88, 0x26, 0x24, 0x50,             /* 18: 83h invalid handle           */
    0xB4, 0x46, 0xCD, 0x67,
    0xA3, 0x26, 0x50,                   /* 19: AX -- 0032h, LIM 3.2         */
    0xB4, 0x4B, 0xCD, 0x67,
    0x89, 0x1E, 0x28, 0x50,             /* 20: handles, incl. the driver's  */
    0xB4, 0x4C, 0x8B, 0x16, 0x10, 0x50, 0xCD, 0x67,
    0x89, 0x1E, 0x2A, 0x50,             /* 21: pages owned                  */
    0xB4, 0x42, 0xCD, 0x67,
    0x89, 0x1E, 0x2C, 0x50,             /* 22: free while held              */
    0xB4, 0x45, 0x8B, 0x16, 0x10, 0x50, 0xCD, 0x67,
    0x88, 0x26, 0x2E, 0x50,             /* 23: deallocate status            */
    0xB4, 0x42, 0xCD, 0x67,
    0x89, 0x1E, 0x30, 0x50,             /* 24: free after                   */
    0xB4, 0x99, 0xCD, 0x67,
    0x88, 0x26, 0x32, 0x50,             /* 25: 84h -- loud, not silent      */
    0xF4,
};

/* Squeeze the pool. Logical pages 2..5 only: 0 is the one under test and 1 is
   the unpinned control, and touching either would rewrite the very bytes the
   arm is about to check. */
static void ems_pressure(void)
{
    for (uint32_t p = 2u * EMS_SUBS; p < EMS_TEST_PAGES * EMS_SUBS; p++) {
        /* The ring is the victim picker's OTHER veto and it would mask the pin
           we are trying to prove, so it is cleared before every touch. */
        x86_livemap_clear();
        uint8_t *q = (uint8_t *)x86_guest_ptr((X86_EMS_BACK_PAGE + p) << 12, 1);
        if (q)
            *q = (uint8_t)(p + 1u);     /* non-zero: it must really be written */
    }
    x86_livemap_clear();
}

static void ems_test_run(void)
{
    static const uint8_t hle_vectors[] = {
        0x08, 0x09, 0x10, 0x11, 0x12, 0x15, 0x16, 0x1A, 0x20, 0x21, 0x2F,
    };
    uint32_t r13_clean = 0xFFFFu;

    for (uint32_t i = 0; i < 40; i++)
        ems_r[i] = 0;
    emsb_fail = 0;

    /* ---- the machine: a page frame region that looks like the real one ---
       Sixteen fold entries over 0xE0000 all pointing at ONE 0xFF page, which is
       exactly what src/main.c's open-bus filler does, and exactly what
       x86_ems_set_backing() checks for before it claims the region. */
    memset(test_ram + EMS_FILL_OFF, 0xFF, 4096);
    for (uint32_t i = 0; i < X86_EMS_FRAME_BYTES / 4096u; i++)
        guest_page_table[EMS_FRAME_IDX + i] =
            FOLD_K((uint32_t)test_ram + EMS_FILL_OFF, EMS_FRAME_IDX + i);

    /* ---- the pager: TWELVE frames for TWENTY-FOUR backing pages ---------- */
    memset(test_ram + EMS_STORE_OFF, 0, EMS_STORE_SLOTS * 4096u);
    x86_pager_reset();
    x86_livemap_clear();
    x86_pager_own(0, 0);                 /* only the aux window is ours here */
    x86_pager_own_aux(X86_EMS_BACK_PAGE, EMS_TEST_PAGES * EMS_SUBS);
    x86_pager_set_backing(emsb_read, emsb_write);
    x86_pager_add_frames(test_ram + EMS_POOL_OFF, X86_PG_POOL_BYTES(EMS_FRAMES));
    for (uint32_t i = 0; i < EMS_TEST_PAGES * EMS_SUBS; i++)
        guest_page_table[X86_EMS_BACK_PAGE + i] = FOLD_INVALID;

    /* ---- low memory: the stubs, the name, the code ---------------------- */
    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));
    /* AFTER the frame is mapped and after the IVT is planted -- both are
       preconditions x86_ems_set_backing() checks or writes. */
    ems_r[34] = (x86_ems_set_backing(EMS_TEST_PAGES) == 0);
    ems_r[35] = x86_ems_frame_refused;

    /* The DIRECT detection route, which some titles use instead of opening the
       device: the INT 67h vector's SEGMENT, then eight bytes at offset 0x0A. */
    {
        uint8_t v[4], nm[8];
        x86_guest_copy_out(v, 0x67u * 4u, 4);
        uint32_t seg = (uint32_t)(v[2] | (v[3] << 8));
        x86_guest_copy_out(nm, (seg << 4) + 0x0Au, 8);
        ems_r[36] = (memcmp(nm, "EMMXXXX0", 8) == 0);
    }

    memcpy(&test_ram[EMS_NAME], "EMMXXXX0", 9);
    memset(&test_ram[EMS_RES], 0, 0x80);
    memcpy(&test_ram[EMS_CODE_A], ems_code_a, sizeof(ems_code_a));
    memcpy(&test_ram[EMS_CODE_B], ems_code_b, sizeof(ems_code_b));
    memcpy(&test_ram[EMS_CODE_C], ems_code_c, sizeof(ems_code_c));
    memcpy(&test_ram[EMS_CODE_D], ems_code_d, sizeof(ems_code_d));

    /* ---- phase A -------------------------------------------------------- */
    kbd_test_reset_cpu(EMS_CODE_A);
    if (kbd_test_run_to_halt() != X86_EXIT_HALT)
        return;                             /* every result stays 0: LOUD */

    /* The premise the pinning claim rests on: the four granules of one logical
       page are NOT contiguous host memory, so a pin that protected only the
       first would be visible. Frames come off a LIFO free list, so they
       descend -- asserted rather than assumed. */
    ems_r[28] = (guest_page_table[EMS_FRAME_IDX + 0]
                 != guest_page_table[EMS_FRAME_IDX + 1]);
    ems_r[26] = (x86_page_is_pinned(X86_EMS_BACK_PAGE)
                 && x86_page_is_pinned(X86_EMS_BACK_PAGE + 1)
                 && x86_page_is_pinned(X86_EMS_BACK_PAGE + 2)
                 && x86_page_is_pinned(X86_EMS_BACK_PAGE + 3));
    ems_r[27] = x86_ems_pinned_pages;

    /* ---- squeeze the pool, with window 0 still mapped -------------------- */
    {
        uint32_t fold0[EMS_SUBS];
        for (uint32_t i = 0; i < EMS_SUBS; i++)
            fold0[i] = guest_page_table[EMS_FRAME_IDX + i];
        uint32_t ev0 = x86_pg_evictions;

        ems_pressure();

        ems_r[29] = (x86_pg_evictions > ev0);
        ems_r[30] = (x86_pg_exhausted == 0);
        int held = 1;
        for (uint32_t i = 0; i < EMS_SUBS; i++)
            if (guest_page_table[EMS_FRAME_IDX + i] != fold0[i]
                || guest_page_table[X86_EMS_BACK_PAGE + i] == FOLD_INVALID)
                held = 0;
        ems_r[31] = held;

        /* THE CONTROL. Logical page 1's four granules are allocated but NOT
           mapped, so nothing pins them and the same pressure must have taken at
           least one. If this reads 0 the pressure never bit and ems_r[31] above
           is proving nothing. */
        int lost = 0;
        for (uint32_t i = 0; i < EMS_SUBS; i++)
            if (guest_page_table[X86_EMS_BACK_PAGE + EMS_SUBS + i]
                == FOLD_INVALID)
                lost = 1;
        ems_r[32] = lost;
    }

    /* ---- phase B: the mapped page still reads back byte for byte -------- */
    kbd_test_reset_cpu(EMS_CODE_B);
    (void)kbd_test_run_to_halt();
    r13_clean = (uint32_t)(test_ram[EMS_RES + 26]
                           | (test_ram[EMS_RES + 27] << 8));

    /* ---- THE NEGATIVE CONTROL ON THE COMPARE ITSELF ---------------------
       One byte of the mapped page, flipped through the window, and the same
       verify re-run. If phase B's accumulator were vacuous this would still
       come back zero. */
    {
        uint8_t *p = (uint8_t *)x86_guest_ptr(X86_EMS_FRAME_LIN + 100u, 1);
        if (p) {
            *p ^= 0xFF;
            kbd_test_reset_cpu(EMS_CODE_B);
            (void)kbd_test_run_to_halt();
            ems_r[33] = ((test_ram[EMS_RES + 26]
                          | (test_ram[EMS_RES + 27] << 8)) != 0);
            p = (uint8_t *)x86_guest_ptr(X86_EMS_FRAME_LIN + 100u, 1);
            if (p)
                *p ^= 0xFF;
        }
        x86_livemap_clear();
    }

    /* ---- phase C: the evicted, unmapped page comes back from the store --- */
    kbd_test_reset_cpu(EMS_CODE_C);
    (void)kbd_test_run_to_halt();

    /* ---- phase D: the refusals and the accounting ------------------------ */
    ems_r[37] = x86_ems_maps;
    kbd_test_reset_cpu(EMS_CODE_D);
    (void)kbd_test_run_to_halt();

    for (uint32_t i = 0; i < 26; i++)
        ems_r[i] = (uint32_t)(test_ram[EMS_RES + i * 2]
                              | (test_ram[EMS_RES + i * 2 + 1] << 8));
    /* The loop above has just re-read slot 13 from the guest's LAST run of
       phase B -- the CORRUPTED one. The clean reading was saved before the
       byte was flipped; the corrupted one is ems_r[33]'s business alone. */
    ems_r[13] = r13_clean;

    ems_r[38] = x86_ems_pinned_pages;   /* deallocate released every pin */
    ems_r[39] = x86_pg_io_errors + emsb_fail;

    x86_ems_set_backing(0);
    x86_pager_own_aux(0, 0);
    x86_pager_set_backing(NULL, NULL);
    for (uint32_t i = 0; i < EMS_TEST_PAGES * EMS_SUBS; i++)
        guest_page_table[X86_EMS_BACK_PAGE + i] = FOLD_INVALID;
    for (uint32_t i = 0; i < X86_EMS_FRAME_BYTES / 4096u; i++)
        guest_page_table[EMS_FRAME_IDX + i] = FOLD_INVALID;
    x86_pager_reset();
}

/* ---- the granule seam ----------------------------------------------------
 *
 * host = lin + k, and k is per 4 KB granule. Two adjacent guest granules are
 * therefore only adjacent in HOST memory by luck -- the pager hands out frames
 * in fault order, copy-on-write moves a granule into the pool, and a fixed
 * mapping (VRAM, the 13h aperture, the open-bus filler) has neighbours that are
 * not memory at all. An access whose bytes span the boundary resolves ONE
 * pointer, from the LOW granule's k, and then reads or writes straight past the
 * top of that host page -- into whatever happens to follow it.
 *
 * The prior DOS port called this "the seam" and it is the same bug: a
 * straddling 16-bit DATA read is what made KEEN4 and TOPBENCH diverge there
 * (references/8086tiny/8086tiny.c:7176-7183).
 *
 * WHY THE EXISTING TIER-1 ARMS CANNOT SEE IT. Everything else here runs on a
 * fold that is either identity over test_ram or a pager whose four frames come
 * out of one extent in order -- in both, granule N+1 IS the memory after
 * granule N, so the defect is invisible by construction. This arm installs the
 * fold BY HAND, with the two granules deliberately 48 KB apart, and paints the
 * eight bytes after the low granule with 0xEE so a broken access returns
 * something recognisable rather than a plausible zero.
 */
#define SEAM_LOW_OFF   (672u * 1024u)   /* page-aligned, past the arena's 160K */
#define SEAM_HIGH_OFF  (720u * 1024u)   /* deliberately NOT low + 4096         */
#define SEAM_SEG       0xC000u
#define SEAM_FIRST     (0xC0000u >> 12)

/* Put one byte into guest memory through the host accessor, which faults the
   page in exactly as a program load does. */
static void seam_poke(uint32_t lin, uint8_t v)
{
    uint8_t *p = (uint8_t *)x86_guest_ptr(lin, 1);
    if (p)
        *p = v;
}

static uint32_t seam_peek(uint32_t lin)
{
    uint8_t *p = (uint8_t *)x86_guest_ptr(lin, 1);
    return p ? *p : 0xFFFFFFFFu;
}

static void seam_test_run(void)
{
    for (uint32_t i = 0; i < 24; i++)
        seam_r[i] = 0;

    uint32_t s0 = x86_seam_syncs, w0 = x86_seam_writes, o0 = x86_seam_overflow;

    x86_pager_reset();
    guest_page_table[SEAM_FIRST]     = FOLD_INVALID;
    guest_page_table[SEAM_FIRST + 1] = FOLD_INVALID;

    if (seam_contiguous) {
        /* THE CONTIGUITY CONTROL, and it CANNOT use the pager. A frame is
           4,104 bytes now -- 4,096 of guest space and eight of slack -- so no
           two pager frames are ever contiguous, by construction. What is
           contiguous is a FIXED mapping: identity over test_ram, which is what
           every other arm in this harness runs on and what x86_map_fixed()
           installs for VRAM and the aperture. The fold entries then compare
           EQUAL, x86_seam_sync() takes its early-out, and the straddling
           accesses are correct with no seam machinery involved at all.
           This arm must therefore pass in BOTH builds -- including
           -DX86_SEAM_FIX=0 -- and a failure here is the guest program or the
           harness, never the seam. */
        guest_page_table[SEAM_FIRST] =
            FOLD_K((uint32_t)(uintptr_t)(test_ram + SEAM_LOW_OFF), SEAM_FIRST);
        guest_page_table[SEAM_FIRST + 1] =
            FOLD_K((uint32_t)(uintptr_t)(test_ram + SEAM_LOW_OFF + 4096u),
                   SEAM_FIRST + 1);
        memset(test_ram + SEAM_LOW_OFF, 0, 2u * 4096u);
    } else {
        /* THE DEFECT. Two extents 48 KB apart and one frame each, so granule
           N+1 is emphatically not the memory after granule N -- which is what
           the shipping pager produces anyway, because it hands frames out in
           FAULT order over a single pool. This is that shape, made explicit
           and made deterministic. */
        x86_pager_own(SEAM_FIRST, 2);
        x86_pager_add_frames(test_ram + SEAM_LOW_OFF,  X86_PG_POOL_BYTES(1));
        x86_pager_add_frames(test_ram + SEAM_HIGH_OFF, X86_PG_POOL_BYTES(1));
        seam_poke(SEAM_FIRST << 12, 0);
        seam_poke((SEAM_FIRST + 1u) << 12, 0);
    }

    uint32_t lo = SEAM_FIRST << 12;
    uint32_t hi = (SEAM_FIRST + 1u) << 12;
    seam_poke(lo + 0xFFE, 0x99);
    seam_poke(lo + 0xFFF, 0xAA);
    seam_poke(hi + 0,     0xBB);
    seam_poke(hi + 1,     0xCC);
    seam_poke(hi + 2,     0xDD);

    /* Paint the low granule's slack with something recognisable. A broken
       access reads or writes THESE eight bytes instead of the neighbour, and
       0xEE is visible on sight where a zero would read as plausible data. */
    uint8_t *slack = x86_pager_slack(SEAM_FIRST);
    seam_r[11] = (slack != NULL);
    if (slack)
        memset(slack, 0xEE, X86_PG_SLACK);
    seam_r[12] = (guest_page_table[SEAM_FIRST]
                  != guest_page_table[SEAM_FIRST + 1]);   /* NOT contiguous */

    /*
     *   mov ax,0C000h        B8 00 C0
     *   mov ds,ax            8E D8
     *   mov bx,0FFFh         BB FF 0F
     *   mov ax,[bx]          8B 07        <- 16-bit read across the seam
     *   mov [0010h],ax       A3 10 00
     *   mov eax,[bx-1]       66 8B 47 FF  <- 32-bit read at 0FFEh, 2 bytes over
     *   mov [0012h],eax      66 A3 12 00
     *   mov ax,1234h         B8 34 12
     *   mov [bx],ax          89 07        <- 16-bit WRITE across the seam
     *   hlt                  F4
     *
     * The results land at offsets 10h/12h, well inside the low granule, so
     * reading them back cannot itself straddle anything.
     */
    static const uint8_t prog[] = {
        0xB8, 0x00, 0xC0,  0x8E, 0xD8,
        0xBB, 0xFF, 0x0F,
        0x8B, 0x07,
        0xA3, 0x10, 0x00,
        0x66, 0x8B, 0x47, 0xFF,
        0x66, 0xA3, 0x12, 0x00,
        0xB8, 0x34, 0x12,
        0x89, 0x07,
        0xF4,
    };
    memcpy(test_ram, prog, sizeof(prog));
    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    for (int i = 0; i < 6; i++)
        cpu_state_dtcm.segs[i].limit = 0xFFFFFFFF;
    cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
    cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
    x86_cpu_interp_asm();

    seam_r[0] = cpu_state_dtcm.exit_reason;                 /* 6 = HLT       */
    seam_r[1] = seam_peek(lo + 0x10) | (seam_peek(lo + 0x11) << 8);
                                                            /* 0xBBAA        */
    seam_r[2] = seam_peek(lo + 0x12) | (seam_peek(lo + 0x13) << 8)
                | (seam_peek(lo + 0x14) << 16) | (seam_peek(lo + 0x15) << 24);
                                                            /* 0xCCBBAA99    */
    seam_r[3] = seam_peek(lo + 0xFFF);                      /* 0x34          */
    seam_r[4] = seam_peek(hi + 0);                          /* 0x12          */
    seam_r[5] = seam_peek(hi + 1);                          /* 0xCC          */
    slack = x86_pager_slack(SEAM_FIRST);
    seam_r[6] = slack ? slack[0] : 0xFFFFFFFFu;
    seam_r[7] = seam_contiguous;
    seam_r[8] = x86_seam_syncs - s0;
    seam_r[9] = x86_seam_writes - w0;
    seam_r[10] = x86_seam_overflow - o0;

    /* Put the two granules back the way the rest of the harness leaves them --
       0xC0000 is past the end of test_ram, so UNMAPPED, exactly as the pager
       arm restores them. Leaving our scratch mapped there would hand the next
       arm this one's buffers as guest memory. */
    x86_pager_reset();
    guest_page_table[SEAM_FIRST]     = FOLD_INVALID;
    guest_page_table[SEAM_FIRST + 1] = FOLD_INVALID;
}

/* ---- the CODE-STREAM seam -------------------------------------------------
 *
 * The arm above straddles the boundary with an OPERAND. This one straddles it
 * with the INSTRUCTION, which is a different mechanism and, until the dispatch
 * started threading cpu->code_k, a case no tier-1 arm could reach.
 *
 * The threading is what makes it matter. The dispatch folds the opcode byte's
 * granule and hands that one k to every later fetch of the same instruction --
 * ModRM, SIB, displacement, immediate. `host = lin + k` holds for ONE granule,
 * so an instruction that crosses the boundary would take its displacement or
 * its immediate out of the LOW granule's frame: eight bytes of slack, and then
 * an unrelated guest page. The dispatch proves the instruction fits before it
 * threads anything and stages it into DTCM when it does not (x86_code_reloc).
 *
 * THIS ARM IS THE ONLY THING THAT CAN SEE EITHER BEHAVIOUR. Every other tier-1
 * fold is identity over test_ram or a pager walking one extent in order, and
 * under both `ptr + 1` is right across a boundary by accident. Here the two
 * granules are 48 KB apart and the low one's slack is painted 0xEE, so a
 * threaded-but-unproven fetch reads 0xEE and says so.
 *
 * Three cases, one per position the straddle can occupy, plus the fourth shape
 * folded into case 2 -- an opcode that is the LAST byte of its granule, whose
 * ModRM and displacement are therefore both on the far side.
 *
 *   0  DISPLACEMENT.  8B 06 20 00  mov ax,[0020h]   at 0FFDh
 *                     disp16 lands on 0FFFh/1000h.
 *   1  IMMEDIATE.     B8 34 12     mov ax,1234h     at 0FFEh
 *                     imm16 lands on 0FFFh/1000h.
 *   2  OPCODE.        89 06 40 00  mov [0040h],ax   at 0FFFh
 *                     the opcode is the granule's last byte; ModRM and disp16
 *                     are all in the neighbour.
 *
 * Each stores its answer at 0040h, well inside the low granule, so reading it
 * back cannot itself straddle. Run tests/test_seam.py --code-negative for the
 * build that threads without proving, in which every one of these must fail.
 */
static void code_seam_poke_prog(void)
{
    uint32_t lo = SEAM_FIRST << 12;

    /* The word the displacement case loads, and a known-clear result slot. */
    seam_poke(lo + 0x0020, 0x78);
    seam_poke(lo + 0x0021, 0x56);
    seam_poke(lo + 0x0040, 0x00);
    seam_poke(lo + 0x0041, 0x00);

    switch (code_seam_case) {
    case 0:                                  /* displacement across */
        seam_poke(lo + 0x0FFD, 0x8B);        /* mov ax,[disp16]     */
        seam_poke(lo + 0x0FFE, 0x06);
        seam_poke(lo + 0x0FFF, 0x20);        /* disp low  -- last byte  */
        seam_poke(lo + 0x1000, 0x00);        /* disp high -- NEIGHBOUR  */
        seam_poke(lo + 0x1001, 0xA3);        /* mov [0040h],ax */
        seam_poke(lo + 0x1002, 0x40);
        seam_poke(lo + 0x1003, 0x00);
        seam_poke(lo + 0x1004, 0xF4);        /* hlt */
        break;
    case 1:                                  /* immediate across */
        seam_poke(lo + 0x0FFE, 0xB8);        /* mov ax,imm16 */
        seam_poke(lo + 0x0FFF, 0x34);        /* imm low  -- last byte  */
        seam_poke(lo + 0x1000, 0x12);        /* imm high -- NEIGHBOUR  */
        seam_poke(lo + 0x1001, 0xA3);
        seam_poke(lo + 0x1002, 0x40);
        seam_poke(lo + 0x1003, 0x00);
        seam_poke(lo + 0x1004, 0xF4);
        break;
    default:                                 /* opcode is the last byte */
        seam_poke(lo + 0x0FFF, 0x89);        /* mov [disp16],ax        */
        seam_poke(lo + 0x1000, 0x06);        /* ModRM    -- NEIGHBOUR  */
        seam_poke(lo + 0x1001, 0x40);        /* disp low -- NEIGHBOUR  */
        seam_poke(lo + 0x1002, 0x00);
        seam_poke(lo + 0x1003, 0xF4);
        break;
    }
}

static void code_seam_test_run(void)
{
    static const uint16_t start_ip[3] = { 0x0FFD, 0x0FFE, 0x0FFF };
    static const uint16_t expect[3]   = { 0x5678, 0x1234, 0xABCD };
    uint32_t c = code_seam_case < 3 ? code_seam_case : 2;

    for (uint32_t i = 0; i < 16; i++)
        code_seam_r[i] = 0;

    uint32_t rel0 = x86_code_relocs, o0 = x86_seam_overflow;

    /* The same deliberately non-contiguous fold the operand arm installs: one
       frame from each of two extents 48 KB apart, so granule N+1 is emphatically
       not the host memory after granule N. */
    x86_pager_reset();
    guest_page_table[SEAM_FIRST]     = FOLD_INVALID;
    guest_page_table[SEAM_FIRST + 1] = FOLD_INVALID;
    x86_pager_own(SEAM_FIRST, 2);
    x86_pager_add_frames(test_ram + SEAM_LOW_OFF,  X86_PG_POOL_BYTES(1));
    x86_pager_add_frames(test_ram + SEAM_HIGH_OFF, X86_PG_POOL_BYTES(1));
    seam_poke(SEAM_FIRST << 12, 0);
    seam_poke((SEAM_FIRST + 1u) << 12, 0);

    code_seam_poke_prog();

    /* Paint the low granule's slack. An instruction fetched with an unproven
       threaded k reads THESE bytes for everything past the boundary, and 0xEE
       is visible on sight where a zero would read as a plausible encoding. */
    uint8_t *slack = x86_pager_slack(SEAM_FIRST);
    code_seam_r[6] = (slack != NULL);
    if (slack)
        memset(slack, 0xEE, X86_PG_SLACK);
    code_seam_r[7] = (guest_page_table[SEAM_FIRST]
                      != guest_page_table[SEAM_FIRST + 1]);

    memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
    for (int i = 0; i < 6; i++) {
        cpu_state_dtcm.segs[i].limit = 0xFFFFFFFF;
        cpu_state_dtcm.segs[i].sel   = SEAM_SEG;
        cpu_state_dtcm.segs[i].base  = (uint32_t)SEAM_SEG << 4;
    }
    cpu_state_dtcm.regs[0] = 0xABCD;   /* EAX -- case 2's stored value */
    cpu_state_dtcm.eip = start_ip[c];
    cpu_state_dtcm.pit_reload  = 0xFFFFFFFF;
    cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
    x86_cpu_interp_asm();

    uint32_t lo = SEAM_FIRST << 12;
    code_seam_r[0] = cpu_state_dtcm.exit_reason;                 /* 6 = HLT */
    code_seam_r[1] = seam_peek(lo + 0x0040)
                     | (seam_peek(lo + 0x0041) << 8);
    code_seam_r[2] = expect[c];
    code_seam_r[3] = cpu_state_dtcm.regs[0] & 0xFFFF;   /* AX */
    code_seam_r[4] = x86_code_relocs - rel0;
    code_seam_r[5] = x86_seam_overflow - o0;
    code_seam_r[8] = code_seam_case;
    code_seam_r[9] = slack ? slack[0] : 0xFFFFFFFFu;
    code_seam_r[10] = cpu_state_dtcm.eip;

    /* Leave the two granules the way the rest of the harness expects them --
       0xC0000 is past the end of test_ram, so UNMAPPED. */
    x86_pager_reset();
    guest_page_table[SEAM_FIRST]     = FOLD_INVALID;
    guest_page_table[SEAM_FIRST + 1] = FOLD_INVALID;
}

static void dosfile_test_run(void)
{
    static const uint8_t hle_vectors[] = {
        0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21,
    };

    for (int i = 0; i < 40; i++)
        dosfile_r[i] = 0;
    memset(&test_ram[DOSF_RES], 0, 128);
    memset(&test_ram[DOSF_BUF], 0, 1024);
    memset(&test_ram[DOSF_NAME1], 0, 64);
    memset(&test_ram[0x6FE0], 0, 64);

    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));

    /* The image the host wrote into dosfile_img IS the guest's floppy. */
    x86_disk_set_memory(dosfile_img, DOSFILE_IMG_BYTES);
    dosfile_r[36] = (uint32_t)f_mount(&dosfile_fs, "", 1);

    memcpy(&test_ram[DOSF_STR + 0x00], "TEST.INI", 9);
    memcpy(&test_ram[DOSF_STR + 0x10], "NOPE.INI", 9);
    memcpy(&test_ram[DOSF_STR + 0x20], "*.INI", 6);
    memcpy(&test_ram[DOSF_STR + 0x30], "A:TEST.INI", 11);
    memcpy(&test_ram[DOSF_STR + 0x40], "test.ini", 9);
    memcpy(&test_ram[DOSF_CODE], dosfile_guest_code,
           sizeof(dosfile_guest_code));

    kbd_test_reset_cpu(DOSF_CODE);
    dosfile_r[37] = kbd_test_run_to_halt();

    for (int i = 0; i < 31; i++)
        dosfile_r[i] = (uint16_t)(test_ram[DOSF_RES + i * 2] |
                                  (test_ram[DOSF_RES + i * 2 + 1] << 8));
}

/*
 * Load a real MZ .EXE through x86_dos_exec() and report where everything went.
 *
 * Deliberately reports rather than asserts: the host built the file, so the
 * host is the only side that can say what the loaded image MUST contain, and a
 * check computed by the same code that did the load proves nothing about it.
 */
static void dosexec_test_run(void)
{
    static const uint8_t hle_vectors[] = {
        0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21,
    };
    for (int i = 0; i < 16; i++)
        dosexec_r[i] = 0;

    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));

    x86_disk_set_memory(dosfile_img, DOSFILE_IMG_BYTES);
    dosexec_r[0] = (uint32_t)f_mount(&dosfile_fs, "", 1);

    kbd_test_reset_cpu(0);
    x86_dos_arena_init();
    dosexec_r[1] = (uint32_t)x86_dos_exec("LOAD.EXE", "-x");

    dosexec_r[2]  = cpu_state_dtcm.segs[1].sel;      /* CS  */
    dosexec_r[3]  = cpu_state_dtcm.eip;              /* IP  */
    dosexec_r[4]  = cpu_state_dtcm.segs[2].sel;      /* SS  */
    dosexec_r[5]  = cpu_state_dtcm.regs[4] & 0xFFFF; /* SP  */
    dosexec_r[6]  = cpu_state_dtcm.segs[3].sel;      /* DS, which DOS sets to
                                                        the PSP on entry */
    dosexec_r[7]  = cpu_state_dtcm.segs[0].sel;      /* ES, likewise */
    dosexec_r[8]  = x86_dos_psp();
    dosexec_r[9]  = dos_test_walk();                 /* chain still consistent */
    dosexec_r[10] = cpu_state_dtcm.segs[1].base;
}

/*
 * AH=4Bh, end to end: PARENT.EXE EXECs CHILD.EXE, the child writes its
 * sentinels and exits with a code, and the parent has to resume after its own
 * INT 21h to read that code back with AH=4Dh.
 *
 * Run to HALT, once: the child's AH=4Ch must NOT stop the machine (it returns
 * to the parent), and the parent's must. A run that stops on the first AH=4Ch
 * reports exit_reason 1 with the parent's sentinels still zero, which is
 * exactly the failure this arm is for.
 */
static void dosexec2_test_run(void)
{
    static const uint8_t hle_vectors[] = {
        0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21,
    };
    for (int i = 0; i < 16; i++)
        dosexec2_r[i] = 0;
    memset(&test_ram[DOSEXEC2_MARK], 0, 64);

    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));

    x86_disk_set_memory(dosfile_img, DOSFILE_IMG_BYTES);
    dosexec2_r[0] = (uint32_t)f_mount(&dosfile_fs, "", 1);

    kbd_test_reset_cpu(0);
    x86_dos_arena_init();
    x86_dos_execs = 0;
    x86_dos_exec_fails = 0;
    x86_dos_exec_depth_max = 0;
    x86_dos_exec_last_err = 0;

    dosexec2_r[1] = (uint32_t)x86_dos_exec("PARENT.EXE", "");
    dosexec2_r[2] = x86_dos_psp();              /* the parent's PSP */
    dosexec2_r[3] = kbd_test_run_to_halt();     /* 1 = the PARENT terminated */
    dosexec2_r[4] = x86_dos_execs;
    dosexec2_r[5] = x86_dos_exec_fails;
    dosexec2_r[6] = x86_dos_exec_last_err;
    dosexec2_r[7] = x86_dos_exec_depth_max;
    dosexec2_r[8] = dos_test_walk();            /* chain still consistent */
    dosexec2_r[9] = x86_dos_psp();              /* the PARENT's again */
    /* The arena after the child is gone. A child that leaks its block leaves
       this well below what the same chain offered before the EXEC. */
    {
        uint16_t largest = 0;
        (void)dos_test_alloc(0xFFFF, &largest);
        dosexec2_r[10] = largest;
    }
}

/*
 * AH=4Bh AL=03h. Same shape as the EXEC arm: mount, load a real HOST.EXE with
 * x86_dos_exec(), run to HLT, and read back what the guests wrote. The arena
 * is checked afterwards because an overlay that allocated anything at all is
 * already wrong -- AL=03h has no MCB of its own.
 */
static void dosovl_test_run(void)
{
    static const uint8_t hle_vectors[] = {
        0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21,
    };
    for (int i = 0; i < 16; i++)
        dosovl_r[i] = 0;
    memset(&test_ram[DOSOVL_MARK], 0, 32);

    x86_video_init(cga_test_vram, cga_test_palette);
    x86_video_set_aperture(vga_test_aperture, vga_test_pal);
    x86_bios_init();
    x86_hle_install(hle_vectors, sizeof(hle_vectors));

    x86_disk_set_memory(dosfile_img, DOSFILE_IMG_BYTES);
    dosovl_r[0] = (uint32_t)f_mount(&dosfile_fs, "", 1);

    kbd_test_reset_cpu(0);
    x86_dos_arena_init();
    x86_dos_execs = 0;
    x86_dos_exec_fails = 0;
    x86_dos_exec_last_err = 0;
    x86_dos_overlays = 0;
    x86_dos_overlay_seg = 0;
    x86_dos_overlay_reloc = 0;

    dosovl_r[1] = (uint32_t)x86_dos_exec("HOST.EXE", "");
    dosovl_r[2] = x86_dos_psp();                /* the host program's PSP */
    dosovl_r[3] = kbd_test_run_to_halt();       /* 1 = HOST.EXE terminated */
    dosovl_r[4] = x86_dos_overlays;
    dosovl_r[5] = x86_dos_exec_fails;
    dosovl_r[6] = x86_dos_exec_last_err;
    dosovl_r[7] = x86_dos_overlay_seg;
    dosovl_r[8] = x86_dos_overlay_reloc;
    dosovl_r[9] = dos_test_walk();              /* chain still consistent */
    /* An overlay allocates NOTHING, so the arena must be exactly what it was
       with only HOST.EXE resident. A load that quietly took a block shows up
       here and nowhere else. */
    {
        uint16_t largest = 0;
        (void)dos_test_alloc(0xFFFF, &largest);
        dosovl_r[10] = largest;
    }
    dosovl_r[11] = x86_dos_execs;               /* must stay 0: no process */
}

// External arrays from x86_cpu.c
extern uint32_t (*io_port_read_table[1024])(void *opaque, uint32_t offset, int size_log2);
extern void *io_port_opaque_table[1024];

uint32_t cga_status_read(void *opaque, uint32_t offset, int size_log2) {
    static uint32_t vblank = 0;
    vblank ^= 8; // Toggle bit 3 (VBLANK)
    return vblank;
}

// Dummy Reset_Handler for QEMU execution
void Reset_Handler(void) {
    // Copy .data from its flash load address (nothing did this before, so any
    // non-zero initializer silently read as garbage).
    extern uint32_t _sidata, _sdata, _edata;
    for (uint32_t *src = &_sidata, *dst = &_sdata; dst < &_edata; src++, dst++) {
        *dst = *src;
    }

    // Copy the hot execution loop into ITCM before anything branches into it.
    extern uint32_t _sitcm_load, _sitcm, _eitcm;
    for (uint32_t *src = &_sitcm_load, *dst = &_sitcm; dst < &_eitcm; src++, dst++) {
        *dst = *src;
    }

    // Zero out BSS
    extern uint32_t _sbss;
    extern uint32_t _ebss;
    for (uint32_t *p = &_sbss; p < &_ebss; p++) {
        *p = 0;
    }

    // The harness's own scratch lives in RAM_UC, outside .bss, so it needs
    // zeroing here or a test reads whatever the previous run left.
    extern uint32_t _stestbss, _etestbss;
    for (uint32_t *p = &_stestbss; p < &_etestbss; p++) {
        *p = 0;
    }
    // Same for the mode 13h buffers, which are in AHBRAM for want of room in
    // either AXI region. AHB SRAM's clock is on -- SystemInit enables it.
    extern uint32_t _sahbbss, _eahbbss;
    for (uint32_t *p = &_sahbbss; p < &_eahbbss; p++) {
        *p = 0;
    }

    // Enable DWT Cycle Counter
    CoreDebug_DEMCR |= (1 << 24); 
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1;

#if X86_PROFILE
    /* After DWT is running, before anything dispatches: the profiler reads
       CYCCNT on every dispatch and calibrates itself here. */
    x86_profile_init(0);
#endif

    io_port_read_table[0x3DA] = cga_status_read;

    // Initialize BDA at 0x0400
    // 0x0413: Memory size in KB (uint16_t)
    uint16_t *bda_mem_size = (uint16_t *)&test_ram[0x0413];
    *bda_mem_size = 640;
    
    // Point the interpreter at our statically-placed DTCM state. A retro-go
    // core sets these to dtc_malloc'd memory instead; the assembly is the same.
    guest_page_table_ptr = guest_page_table;
    x86_pager_set_tables(pager_tables, sizeof(pager_tables));
    x86_build_mode_records(mode_record_block);

    // Every page starts unmapped, so a stray access faults loudly instead of
    // landing in ITCM. test_ram is identity-mapped from guest 0, which makes
    // every k identical -- k = host - (idx << 12) = test_ram.
    for (uint32_t i = 0; i < FOLD_ENTRIES; i++) {
        guest_page_table[i] = FOLD_INVALID;
    }
    for (uint32_t i = 0; i < sizeof(test_ram) / 4096; i++) {
        guest_page_table[i] = FOLD_K((uint32_t)test_ram + i * 4096, i);
    }

    // Disable PIT during opcode tests to prevent unexpected IRQs
    cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
    cpu_state_dtcm.pit_counter = 0xFFFFFFFF;

    while (1) {
        // Load the guest from memory-mapped external flash. Host-triggered
        // because reaching 0x90000000 needs OCTOSPI configuration this firmware
        // does not perform -- doing it unconditionally at reset bus-faults on
        // silicon while passing under gwemu, which models extflash as plain RAM.
        if (load_guest_flag) {
            vdisk_init((void*)0x90000000, 1474560);

            FATFS fs;
            FRESULT res = f_mount(&fs, "0:", 1);
            if (res == FR_OK) {
                FIL file;
                res = f_open(&file, "0:CAT.EXE", FA_READ);
                if (res == FR_OK) {
                    int load_res = load_mz_executable(&file);
                    benchmark_filesize =
                        (load_res == 0) ? 0x1337 : (0xDD000000 | -load_res);
                    f_close(&file);
                } else {
                    benchmark_filesize = 0xEEEE0000 | res;
                }
            } else {
                benchmark_filesize = 0xFFFF0000 | res;
            }
            load_guest_flag = 0;
        }

        if (run_flag) {
            x86_cpu_interp_asm();
            run_flag = 0;
        }

        if (run_benchmark_flag == 2) {
            // Run the game!
            // Set limits for all segments to 0xFFFF
            for (int i = 0; i < 6; i++) {
                cpu_state_dtcm.segs[i].limit = 0xFFFF;
            }
            
            // Set a dummy IDTR
            cpu_state_dtcm.idtr_base = 0x0000;
            cpu_state_dtcm.idtr_limit = 0x3FF;
            
            // Limit execution to 1,000,000 instructions before yielding
            cpu_state_dtcm.pit_counter = 1000000;
            cpu_state_dtcm.pit_reload = 1000000;
            
            while (1) {
                x86_cpu_interp_asm();
                if (cpu_state_dtcm.exit_reason == 2) {
                    uint32_t int_num = cpu_state_dtcm.exit_info;
                    uint32_t ah = (cpu_state_dtcm.regs[0] >> 8) & 0xFF; // EAX=0
                    uint32_t al = cpu_state_dtcm.regs[0] & 0xFF;
                    
                    if (debug_log_idx < 256) {
                        debug_log[debug_log_idx++] = (int_num << 16) | (ah << 8) | al;
                    }
                    
                    if (int_num == 0x20) {
                        // Terminate
                        break;
                    } else if (int_num == 0x21) {
                        if (ah == 0x4C || ah == 0x00) {
                            // Terminate
                            break; 
                        } else if (ah == 0x09) {
                            // Print string (DS:DX)
                        }
                    } else if (int_num == 0x10) {
                        if (ah == 0x00) {
                            // Set video mode
                        }
                    } else if (int_num == 0x08) {
                        // Timer tick
                        uint32_t *bda_timer = (uint32_t *)&test_ram[0x046C];
                        *bda_timer += 1;
                        // Clear pending
                        cpu_state_dtcm.regs[2192/4] = 0; // wait, hard_int_pending is offset 2192, so we need to clear it! Wait, cpu_state_dtcm is a struct, let's just use the pointer
                        *(uint32_t*)((uint8_t*)&cpu_state_dtcm + 2192) = 0;
                    }
                    
                    // After handling, reset exit_reason and resume
                    cpu_state_dtcm.exit_reason = 0;
                } else if (cpu_state_dtcm.exit_reason == 3) { // timer exit
                    cpu_state_dtcm.exit_reason = 0;
                    cpu_state_dtcm.pit_counter = 1000000;
                    // Allow python script to read EIP!
                } else {
                    // Halting or other exit reason
                    break;
                }
            }
            run_benchmark_flag = 0;
        }
        
        if (run_nor_flag == 1) {
            nor_test_run();
            run_nor_flag = 0;
        }

        if (run_norwd_flag == 1) {
            norwd_test_run();
            run_norwd_flag = 0;
        }

        if (run_a20_flag == 1) {
            a20_test_run();
            run_a20_flag = 0;
        }

        if (run_pmode_flag == 1) {
            pmode_test_run();
            run_pmode_flag = 0;
        }

        if (run_arena_flag == 1) {
            arena_test_run();
            run_arena_flag = 0;
        }

        if (run_xms_flag == 1) {
            xms_test_run();
            run_xms_flag = 0;
        }

        if (run_ems_flag == 1) {
            ems_test_run();
            run_ems_flag = 0;
        }

        if (run_pager_flag == 1) {
            pager_test_run();
            run_pager_flag = 0;
        }

        if (run_hma_flag == 1) {
            hma_test_run();
            run_hma_flag = 0;
        }

        if (run_pgfile_flag == 1) {
            pgfile_test_run();
            run_pgfile_flag = 0;
        }

        if (run_seam_flag == 1) {
            seam_test_run();
            run_seam_flag = 0;
        }

        if (run_code_seam_flag == 1) {
            code_seam_test_run();
            run_code_seam_flag = 0;
        }

        if (run_dos_flag == 1) {
            /* Exercise the arena the way a program does: allocate, allocate,
               free the first, allocate something that only fits if the freed
               block was really reclaimed, then resize. */
            x86_dos_arena_init();
            uint16_t largest0 = 0;
            uint16_t a = dos_test_alloc(0x100, &largest0);
            uint16_t b = dos_test_alloc(0x100, &largest0);
            dos_r[0] = a;
            dos_r[1] = b;
            dos_r[2] = (b > a) ? (b - a) : 0;      /* spacing: 0x101 paras */
            dos_r[3] = dos_test_free(a);
            uint16_t c = dos_test_alloc(0x80, &largest0);
            dos_r[4] = c;
            dos_r[5] = (c == a) ? 1 : 0;           /* reused the hole */
            uint16_t largest = 0;
            dos_r[6] = dos_test_resize(b, 0x40, &largest);
            dos_r[7] = dos_test_alloc(0xFFFF, &largest);
            dos_r[8] = largest;                    /* must report a real size */
            dos_r[9] = dos_test_walk();            /* chain still consistent */
            run_dos_flag = 0;
        }

        /*
         * Arena hardening, driven in STAGES so the host can photograph the
         * whole chain between them. A failed grow has to leave the chain
         * byte-identical, and the only way to prove that is to compare the
         * bytes on either side of the one call -- which means the call has to
         * be a stage of its own.
         *
         * Layout after stage 2:  A(0x100 owned) B(0x100 FREE) C(0x100 owned)
         *                        then the rest of conventional memory, free.
         */
        if (run_dos_flag >= 2 && run_dos_flag <= 7) {
            static uint16_t A, B, C;
            uint16_t l = 0;
            switch (run_dos_flag) {
            case 2:
                x86_dos_arena_init();
                A = dos_test_alloc(0x100, &l);
                B = dos_test_alloc(0x100, &l);
                C = dos_test_alloc(0x100, &l);
                dos_test_free(B);
                dos_r[10] = A;
                dos_r[11] = B;
                dos_r[12] = C;
                break;
            case 3:
                /* THE failed grow. A is followed by free B (0x100) and then by
                   OWNED C, so 0xFFFF cannot be satisfied. It must fail without
                   swallowing B, and report what A could actually grow to:
                   0x100 + 1 + 0x100 = 0x201. */
                dos_r[13] = dos_test_resize(A, 0xFFFF, &l);
                dos_r[14] = l;
                break;
            case 4:
                /* A failed AH=48h must name the TRUE largest free block. The
                   host cross-checks this against its own walk of the chain. */
                dos_r[15] = dos_test_alloc(0xFFFF, &l);
                dos_r[16] = l;
                break;
            case 5:
                /* Coalescing: free C, so B and C and the tail are three
                   adjacent free blocks. A request for exactly B+header+C must
                   land at B -- it only fits if they were merged. */
                dos_r[17] = dos_test_free(C);
                dos_r[18] = dos_test_alloc(0x201, &l);
                dos_r[19] = (dos_r[18] == B) ? 1 : 0;
                break;
            case 6: {
                /* owner 0 is FREE. A block allocated by a caller that passes 0
                   must NOT be recorded as free, and must not be handed out
                   again by the next allocation. */
                x86_dos_arena_init();
                uint16_t z = dos_test_alloc_owner(0x40, 0, &l);
                dos_r[20] = z;
                dos_r[21] = dos_test_owner_of(z);      /* must not be 0 */
                uint16_t y = dos_test_alloc(0x40, &l);
                dos_r[22] = y;
                dos_r[23] = (y != z) ? 1 : 0;
                dos_r[24] = dos_test_walk();
                break;
            }
            case 7: {
                /* The DOS/4GW shape, in miniature: probe for the largest block
                   with 4Ah BX=0xFFFF, then try 48h for it, over and over,
                   never freeing. This is the loop Dark Forces was measured in
                   -- 17,114 allocations and 34,234 resizes, zero frees, the
                   arena marched to segment 0x896A. What must be true is that
                   the loop is a fixed point: the same answer every time, the
                   chain no size bigger, and a bounded number of header reads
                   per call. */
                x86_dos_arena_init();
                uint16_t P = dos_test_alloc(0x100, &l);
                uint16_t Q = dos_test_alloc(0x100, &l);
                (void)dos_test_alloc(0x100, &l);        /* the wall */
                dos_test_free(Q);
                uint16_t blocks0 = dos_test_walk();
                x86_mcb_reads = 0;
                uint16_t big = 0, first = 0;
                int stable = 1;
                for (int i = 0; i < 256; i++) {
                    big = 0;
                    (void)dos_test_resize(P, 0xFFFF, &big);
                    if (i == 0)
                        first = big;
                    else if (big != first)
                        stable = 0;
                    (void)dos_test_alloc(0xFFFF, &big);
                }
                dos_r[25] = x86_mcb_reads;              /* for 512 calls */
                dos_r[26] = blocks0;
                dos_r[27] = dos_test_walk();            /* must equal blocks0 */
                dos_r[28] = stable;
                dos_r[29] = first;
                break;
            }
            }
            run_dos_flag = 0;
        }

        if (run_hle_flag == 1) {
            /* Plant the same vector set the real core does, so the host can
               check every IVT entry lands in memory DOS will never hand out. */
            static const uint8_t hle_vectors[] = {
                0x08, 0x09, 0x10, 0x16, 0x1A, 0x20, 0x21,
            };
            hle_r = x86_hle_install(hle_vectors, sizeof(hle_vectors));
            run_hle_flag = 0;
        }

        if (run_kbd_flag == 1) {
            kbd_test_run();
            run_kbd_flag = 0;
        }

        if (run_pit_flag == 1) {
            pit_test_run();
            run_pit_flag = 0;
        }

        if (run_sound_flag == 1) {
            sound_test_run();
            run_sound_flag = 0;
        }

        if (run_sbdma_flag == 1) {
            sbdma_test_run();
            run_sbdma_flag = 0;
        }

        if (run_fm_flag == 1) {
            fm_test_run();
            run_fm_flag = 0;
        }

        if (run_sysinfo_flag == 1) {
            sysinfo_test_run();
            run_sysinfo_flag = 0;
        }

        if (run_osk_flag == 1) {
            osk_test_run();
            run_osk_flag = 0;
        }

        if (run_dosfile_flag == 1) {
            dosfile_test_run();
            run_dosfile_flag = 0;
        }

        if (run_dossys_flag == 1) {
            dossys_test_run();
            run_dossys_flag = 0;
        }

        if (run_dosexec_flag == 1) {
            dosexec_test_run();
            run_dosexec_flag = 0;
        }

        if (run_dosexec2_flag == 1) {
            dosexec2_test_run();
            run_dosexec2_flag = 0;
        }

        if (run_dosovl_flag == 1) {
            dosovl_test_run();
            run_dosovl_flag = 0;
        }

        if (run_video_flag == 1) {
            x86_video_init(cga_test_vram, cga_test_palette);
            x86_video_set_aperture(vga_test_aperture, vga_test_pal);
            /* init clears VRAM; the host's pattern is written after the flag
               is armed, so re-arm ordering matters -- see tests/test_video.py */
            run_video_flag = 2;
        }
        if (run_modeswitch_flag == 1) {
            modeswitch_test_init();
            run_modeswitch_flag = 0;
        }
        if (run_modeswitch_flag == 2) {
            modeswitch_test_set();
            run_modeswitch_flag = 0;
        }
        if (run_modeswitch_flag == 3) {
            modeswitch_test_tty();
            run_modeswitch_flag = 0;
        }
        if (run_planar_flag == 1) {
            planar_test_init();
            run_planar_flag = 0;
        }
        if (run_planar_flag == 2) {
            planar_test_run();
            run_planar_flag = 0;
        }
        if (run_egafit_flag == 1) {
            egafit_test_run();
            run_egafit_flag = 0;
        }
        if (run_modeyfit_flag == 1) {
            modeyfit_test_run();
            run_modeyfit_flag = 0;
        }
        if (run_modeypage_flag == 1) {
            modeypage_test_run();
            run_modeypage_flag = 0;
        }
        if (run_planar_flag == 3) {
            planar_test_unmap();
            run_planar_flag = 0;
        }
        if (run_present_flag) {
            present_test_run(run_present_flag);
            run_present_flag = 0;
        }
        if (run_video_flag == 4) {
            x86_video_set_mode((uint8_t)text_mode_arg);
            run_video_flag = 0;
        }
        if (run_video_flag == 5) {
            x86_video_scroll((uint8_t)text_scroll_arg,
                             (uint8_t)(text_scroll_arg >> 8));
            run_video_flag = 0;
        }
        if (run_video_flag == 6) {
            /* Stream the palette exactly as a guest does: index once to
               0x3C8, then 768 consecutive bytes to 0x3C9. Then read the first
               two entries back through 0x3C7/0x3C9, which is what a fade loop
               does and what a write-only DAC would fail. */
            unsigned i;
            x86_port_write(0x3C8, 0, 1);
            for (i = 0; i < 768; i++)
                x86_port_write(0x3C9, vga_dac_arg[i], 1);
            x86_port_write(0x3C7, 0, 1);
            for (i = 0; i < 6; i++)
                vga_dac_readback[i] = x86_port_read(0x3C9, 1);
            run_video_flag = 0;
        }
        if (run_video_flag == 9) {
            /* Index once, push vga_dac_prewrite filler bytes, then the 768.
               No second 0x3C8: where the palette lands is decided entirely by
               the auto-incrementing write cursor, which is the only thing that
               can rotate r/g/b into each other. */
            unsigned i;
            x86_port_write(0x3C8, 0, 1);
            for (i = 0; i < vga_dac_prewrite; i++)
                x86_port_write(0x3C9, 0, 1);
            for (i = 0; i < 768; i++)
                x86_port_write(0x3C9, vga_dac_arg[i], 1);
            run_video_flag = 0;
        }
        if (run_video_flag == 8) {
            /* The VGA register FILES, probed the way a DOS program detects a
               display: write a register, read it back. WOLF3D does exactly
               `OUT 3D4,0F / OUT 3D5,66 / IN 3D5` and then repeats it against
               the mono alias 3B4/3B5 -- measured off x86_io_log. With no
               register file both readbacks were 0xFF and its detection walked
               off into its own heap. */
            x86_port_write(0x3D4, 0x0F, 1);
            x86_port_write(0x3D5, 0x66, 1);
            vga_regs_out[0] = x86_port_read(0x3D5, 1);        /* 0x66 */
            /* The mono pair must be SILENT while Misc Output bit 0 selects
               the colour addresses -- a card decodes one pair, not both, and
               that silence is what tells a probe which display it has. */
            x86_port_write(0x3B4, 0x0F, 1);
            vga_regs_out[1] = x86_port_read(0x3B5, 1);        /* 0xFF: silent */
            x86_port_write(0x3C4, 0x04, 1);
            x86_port_write(0x3C5, 0x06, 1);
            vga_regs_out[2] = x86_port_read(0x3C5, 1);        /* 0x06 */
            x86_port_write(0x3CE, 0x05, 1);
            x86_port_write(0x3CF, 0x40, 1);
            vga_regs_out[3] = x86_port_read(0x3CF, 1);        /* 0x40 */
            /* The attribute controller: ONE port, sequenced by a flip-flop
               that a read of 0x3DA resets to "index". Without that reset the
               two halves swap roles permanently. */
            (void)x86_port_read(0x3DA, 1);
            x86_port_write(0x3C0, 0x10, 1);                   /* index */
            x86_port_write(0x3C0, 0x41, 1);                   /* data  */
            (void)x86_port_read(0x3DA, 1);
            x86_port_write(0x3C0, 0x10, 1);
            vga_regs_out[4] = x86_port_read(0x3C1, 1);        /* 0x41 */
            /* And the index latch itself reads back, which is what a probe
               that writes only the index relies on. */
            vga_regs_out[5] = x86_port_read(0x3C4, 1);        /* 0x04 */
            /* Clear Misc Output bit 0 and the mono pair takes over. */
            x86_port_write(0x3C2, 0x66, 1);
            x86_port_write(0x3B4, 0x0F, 1);
            vga_regs_out[6] = x86_port_read(0x3B5, 1);        /* 0x66 now */
            vga_regs_out[7] = x86_port_read(0x3D5, 1);        /* same file */
            x86_port_write(0x3C2, 0x67, 1);                   /* colour again */
            vga_regs_out[8] = x86_port_read(0x3B5, 1);        /* silent again */
            run_video_flag = 0;
        }
        if (run_video_flag == 7) {
            const uint32_t *c = x86_video_clut256();
            unsigned i;
            for (i = 0; i < 256; i++)
                vga_clut_out[i] = c ? c[i] : 0xDEADBEEFu;
            run_video_flag = 0;
        }
        if (run_video_flag == 3) {
            uint32_t t0 = DWT_CYCCNT;
            x86_video_render(cga_test_out, 320);
            video_blit_cycles = DWT_CYCCNT - t0;
            run_video_flag = 0;
        }

        if (run_benchmark_flag == 1) {
            // Setup benchmark state
            memset(&cpu_state_dtcm, 0, sizeof(cpu_state_dtcm));
            for (int i = 0; i < 6; i++) {
                cpu_state_dtcm.segs[i].limit = 0xFFFFFFFF;
            }
            /* This benchmark's guest code is 32-bit flat. A zeroed struct
               leaves CS.D clear, which now really means a 16-bit code
               segment. */
            cpu_state_dtcm.segs[1].flags |= X86_DESC_D_FLAG;
            
            // Setup IDT at guest 0x1000
            uint16_t idt_base = 0x1000;
            cpu_state_dtcm.idtr_base = idt_base;
            cpu_state_dtcm.idtr_limit = 0x3FF;
            
            // Set EFLAGS.IF
            cpu_state_dtcm.eflags = 0x200; // IF enabled
            
            // Write IDT entry for INT 8 (PIT) -> ISR at 0x2000
            // cpu_exec.S uses 8-byte gate descriptors
            uint16_t isr_addr = 0x2000;
            uint32_t idt_entry_addr = idt_base + 8 * 8;
            test_ram[idt_entry_addr + 0] = isr_addr & 0xFF;
            test_ram[idt_entry_addr + 1] = (isr_addr >> 8) & 0xFF;
            test_ram[idt_entry_addr + 2] = 0x00;
            test_ram[idt_entry_addr + 3] = 0x00;
            test_ram[idt_entry_addr + 4] = 0x00;
            test_ram[idt_entry_addr + 5] = 0x00;
            test_ram[idt_entry_addr + 6] = 0x00;
            test_ram[idt_entry_addr + 7] = 0x00;
            
            // Write ISR: INC EBX (0x43), IRET (0xCF)
            test_ram[isr_addr + 0] = 0x43;
            test_ram[isr_addr + 1] = 0xCF;
            
            // Write execution loop at 0: 
            // 0: DEC EAX (0x48)
            // 1: JNZ rel8 (0x75 0xFD)
            // 3: HLT (0xF4)
            test_ram[0] = 0x48;
            test_ram[1] = 0x75;
            test_ram[2] = 0xFD;
            test_ram[3] = 0xF4;
            
            // The PIT is held off so the guest loop runs to its own HLT and the
            // instruction count is a real number. It used to be set to 20,
            // which exited after ~20 instructions -- while the Python side
            // divided by a hardcoded 1,000,000 and printed the result as MIPS.
            // (20 was necessary back when DEC/JNZ could not terminate, because
            // JNZ read a ZF that nothing ever set. The flag layer fixed that.)
            cpu_state_dtcm.pit_reload = 0xFFFFFFFF;
            cpu_state_dtcm.pit_counter = 0xFFFFFFFF;
            
            // Initialize EAX (iterations)
            cpu_state_dtcm.regs[0] = 500000; 
            // Initialize EBX (interrupt counter)
            cpu_state_dtcm.regs[3] = 0;
            // Initialize ESP (Stack Pointer) for INT 8 to push onto
            cpu_state_dtcm.regs[4] = 0x3000;
            
            uint32_t start_cycles = DWT_CYCCNT;
            x86_cpu_interp_asm();
            uint32_t end_cycles = DWT_CYCCNT;

            benchmark_cycles = end_cycles - start_cycles;
            // Report what actually executed rather than letting the host assume:
            // each loop iteration is DEC EAX + JNZ, plus the terminating HLT.
            benchmark_insns = 2u * (500000u - cpu_state_dtcm.regs[0]) + 1u;
            run_benchmark_flag = 0;
        }
        __asm__ volatile ("nop");
    }
}
