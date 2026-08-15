/*
 * x86 CPU emulator
 * 
 * Copyright (c) 2011-2017 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "iomem.h"

typedef struct {
    uint16_t sel;
    uint16_t flags;
    uint32_t base;
    uint32_t limit;
} X86CPUSeg;

/* CS.flags bit 6 is DESC_D_MASK: it is bit 22 of the 32-bit word the
   interpreter loads at SEG_CS_SEL, and it selects the DEFAULT operand and
   address size. Zeroing the CPU struct therefore selects 16-bit, which is
   architecturally right for a real-mode reset but means any harness running
   32-bit guest code must set it explicitly. */
#define X86_DESC_D_FLAG  (1u << 6)

/* ---- descriptor `flags`, and why the layout is what it is ----------------
 *
 * `flags` is 16 bits and has to carry BOTH halves of the descriptor's second
 * dword that matter: the granularity byte (descriptor byte 6) and the access
 * byte (descriptor byte 5). It is the two of them concatenated:
 *
 *     flags == desc[6] | (desc[5] << 8)
 *           == ((w2 >> 16) & 0x00FF) | (w2 & 0xFF00)
 *
 *   bits 0-3   limit 19:16      bits 8-11  TYPE
 *   bit  4     AVL              bit  12    S  (0 = system, 1 = code/data)
 *   bit  5     reserved / L     bits 13-14 DPL
 *   bit  6     D/B              bit  15    P
 *   bit  7     G
 *
 * THE D BIT'S POSITION IS NOT A CHOICE. X86_DESC_D_FLAG above already shipped
 * as (1 << 6), and the interpreter reads it as bit 22 of the 32-bit word
 * `sel | (flags << 16)` at SEG_CS_SEL. Byte 6 of the descriptor carries D at
 * ITS bit 6, so putting byte 6 in the low half is the only packing that leaves
 * that instruction correct -- and the access byte then has nowhere to go but
 * the high half. A "just shift w2 right by 16" layout looks equivalent and
 * silently drops P, S, DPL and TYPE entirely; that was written here first and
 * every descriptor came back not-present.
 *
 * Neither reference uses this packing and neither can: tiny386 stores
 * (w2 >> 8) (references/tiny386/i386.c:1117), putting type at bits 0-3 and D
 * at bit 14, and DOSBox-X uses a bitfield struct (include/cpu.h:284). Where a
 * line from either is cited for a RULE, the rule is what is cited, never the
 * bit position.
 */
#define X86_DESC_G_FLAG    (1u << 7)
#define X86_DESC_S_FLAG    (1u << 12)
#define X86_DESC_P_FLAG    (1u << 15)
#define X86_DESC_TYPE(f)   (((f) >> 8) & 0xFu)
#define X86_DESC_DPL(f)    (((f) >> 13) & 3u)
/* TYPE bits, for a code/data descriptor (S=1). Names follow DOSBox-X's
   DESC_* enum (references/dosbox-x/include/cpu.h). */
#define X86_TYPE_CODE      0x8u   /* set = code, clear = data          */
#define X86_TYPE_DATA_ED   0x4u   /* data: expand-down                 */
#define X86_TYPE_CODE_CONF 0x4u   /* code: conforming                  */
#define X86_TYPE_WRITABLE  0x2u   /* data: writable                    */
#define X86_TYPE_READABLE  0x2u   /* code: readable                    */
#define X86_TYPE_ACCESSED  0x1u
/* A cached descriptor that may not be used. A null selector loaded into
   DS/ES/FS/GS is legal and faults only when the segment is USED, so the cache
   has to record "loaded, but unusable" -- P clear with a zero selector is
   exactly that state and needs no extra field. */
#define X86_SEG_USABLE(s)  (((s)->flags & X86_DESC_P_FLAG) != 0)

struct X86CPUState {
    uint32_t eip;
    uint32_t eflags;
    uint32_t regs[8];
    uint32_t idtr_base;
    uint16_t idtr_limit;
    X86CPUSeg segs[6];
    
    // 386 System Registers
    uint32_t cr0;
    uint32_t cr2;
    uint32_t cr3;
    uint32_t cr4;
    
    // Virtual to Host Physical TLB. Direct-mapped, 128 read + 128 write entries.
    // Each entry maps (Virtual Page Number) -> (Host Physical Base).
    // Tag array: bits 31:12 = VPN, bit 0 = Valid.
    //
    // RESERVED, NOT YET USED -- 2 KB of DTCM currently doing nothing. Kept
    // deliberately: this is the backing for the software TLB that will serve
    // addresses above the direct fold window, which is what removes the current
    // 8 MB aliasing bug (the 11-bit VPN in MEM_GET_PTR is untagged, so any
    // access at or above 8 MB silently wraps into the low 8 MB). See PLAN.md
    // Phase 1. Do not repurpose without reading that first.
    uint32_t tlb_read_tag[128];
    uint32_t tlb_read_host[128];

    uint32_t tlb_write_tag[128];
    uint32_t tlb_write_host[128];
    
    // Bare-metal PIT Channel 0 state
    uint32_t pit_counter;
    uint32_t pit_reload;
    uint32_t hard_int_pending;
    
    // CPU Loop Exit State
    uint32_t exit_reason;
    uint32_t exit_info;

    // Active opcode dispatch table (opcode_table_16 or opcode_table_32).
    // Selected from CS.D at loop entry and reloaded from DTCM on every fetch.
    // Kept here rather than in a reserved register so handlers may use R12
    // freely as scratch (it is also the ARM intra-procedure scratch register).
    uint32_t opcode_table_base;
    /* REP state. Set by the F2/F3 prefix, consumed and cleared at the next
       instruction boundary -- see .fetch_decode_loop. */
    uint32_t rep_active;
    uint32_t rep_resume;      /* linear address of the prefix byte */
    uint32_t rep_zf_sense;    /* 1 = REP/REPE (stop on ZF==0), 0 = REPNE */
    /* Non-zero enables the 0F 0B hypercall. When 0 it raises a real #UD, so a
       guest can be run with HLE off and still behave like a plain 386. */
    uint32_t hle_enabled;
    /* String-op fast path. `rep_handler` is the dispatch-table entry the F2/F3
       prefix resolved for the string opcode that follows it, so `.str_exit` can
       re-enter that handler directly for the next element instead of rewinding
       EIP and paying a second trip through fetch/decode/dispatch. `rep_batch`
       is the remaining hard cap on how many elements one batch may run before
       it must go back through the fetch loop; the prefix reloads it. */
    uint32_t rep_handler;
    uint32_t rep_batch;
    /* Where the NOR cold-page trap stubs collect their answer.
     *
     * It lives in the CPU state, not in a global, for one mechanical reason:
     * the stub must restore R0-R3 before it can use the resolved pointer (the
     * destination register may be any of them, or R12), so the value cannot
     * come back in R0 -- and one LDR off R8 reaches DTCM, whereas a global
     * would need a literal pool inside the ITCM hot section. Zero means the
     * page is genuinely absent and the stub takes the .mem_fault exit. */
    uint32_t nor_ptr;
    /* VGA aperture hook. `vga_base` is the HOST base of the 64 KB guest
       aperture at 0xA0000 while a planar mode is armed, and 0 otherwise.
       Every converted guest-memory site tests `ptr - vga_base` unsigned
       against 0x10000: host pointers live at 0x24xxxxxx, so with a zero base
       the difference is never below 64 KB and the check can never fire. That
       is why 0 is the disarmed sentinel and why reset needs no special case.
       Only STORE sites test it now (VGA_ST): the READ side is the fold trap,
       which costs a non-planar title nothing at all -- see X86_FOLD_VGA_K
       below and the MEM_TRAP comment in cpu_exec.S.
       `vga_latch` is the HOST POINTER of the last guest READ that landed
       inside the aperture -- the adapter's four read latches are loaded from
       it and the planar write modes consume them. It is a pointer rather than
       a window offset because the trap stub recovers the pointer with one BIC
       and has no register to spare for subtracting a base; x86_video_planar.c
       does the subtraction on the cold side. Both sit next to nor_ptr so each
       is one LDR off R8 in DTCM. */
    uint32_t vga_base;
    uint32_t vga_latch;
    /* THE SEAM. How many granule boundaries the instruction in flight has
       synced -- see x86_seam.c. It lives here, not in x86_seam.c's .bss, for
       one reason: the interpreter tests it at EVERY loop head, and one LDR off
       R8 into DTCM costs a load, where a global would cost a literal pool
       inside the ITCM hot section and a second cache line. Zero is the resting
       state and the flush is behind a CBZ, so a guest that never straddles
       pays a load and a not-taken branch and nothing else. */
    uint32_t seam_n;
    /* THE CODE FOLD, threaded from the dispatch to every in-stream fetch.
     *
     * The dispatch already folds the opcode byte's linear address. Every other
     * byte of the same instruction -- the ModRM, the SIB, the displacement, the
     * immediate -- used to re-fold from scratch: UBFX + LDR + ADDS + the sign
     * trap, and on a multi-byte one the three-instruction SEAM_CHECK on top.
     * `code_k` carries the opcode granule's fold constant instead, so a site is
     * `LDR code_k` + `ADD R10`, and the SEAM_CHECK and the trap are gone.
     *
     * IT IS ONLY VALID INSIDE ONE GRANULE, which is the whole difficulty. The
     * dispatch therefore proves, once, that the opcode byte is at granule
     * offset <= 0xFF0 -- so all 16 bytes of the longest possible x86
     * instruction share the opcode's k -- and takes .code_reloc otherwise.
     * `code_buf` is where that cold path stages the instruction: 16 bytes of
     * DTCM filled by folding EACH byte separately, with `code_k` re-pointed at
     * the buffer. Every fetch site then works unchanged, at one address in 273
     * as at the other 272. See cpu_exec.S's dispatch and x86_code_reloc(). */
    uint32_t code_k;
    uint8_t  code_buf[16];
    /* ---- PROTECTED MODE -------------------------------------------------
     *
     * All of this is touched ONLY on a segment LOAD and on LGDT/LLDT, never on
     * a memory access and never per instruction. That is the whole design
     * constraint: 29.4% of wall is ordinary opcodes, and a protected-mode
     * check on the access path would tax every real-mode title for a feature
     * none of them use. The descriptor is walked once, decoded into the
     * segs[] cache, and every later access reads the cache exactly as real
     * mode does. tiny386 makes the same choice (i386.c:1076 set_seg).
     *
     * The GDT and LDT are kept as plain base/limit pairs rather than as
     * X86CPUSeg entries. tiny386 puts the LDT in seg[SEG_LDT] (i386.c:53-63)
     * so LLDT can reuse set_seg, but our segs[] is indexed by the assembly
     * through SEG_STRIDE arithmetic over exactly six registers, and widening
     * it would move every SEG_* offset the interpreter is built against. */
    uint32_t gdt_base;
    uint32_t gdt_limit;      /* BYTE limit, as LGDT loaded it */
    uint32_t ldt_base;
    uint32_t ldt_limit;      /* byte limit, G already applied */
    uint32_t ldt_sel;
    uint32_t cpl;            /* == CS.RPL; see x86_pmode.c */
    /* The last segmentation fault, recorded where a host can read it. There is
       no protected-mode IDT gate path yet, so a fault EXITS the interpreter
       with X86_EXIT_GPF rather than being delivered -- delivery is the next
       step and needs the gate walk. Recording the vector and error code keeps
       that a deferred DELIVERY rather than a lost fault. */
    uint32_t pmode_excp;
    uint32_t pmode_err;
    /* The host SP the interpreter's prologue left, so any exit can unwind to
       it no matter how deep in a handler's subroutine it was taken. See
       .save_state_and_exit in cpu_exec.S. */
    uint32_t host_sp;
};
typedef struct X86CPUState X86CPUState;

/* Why x86_cpu_interp_asm returned. Emitted into x86_offsets.inc by offset.c so
   the assembly and the C/Python consumers cannot disagree. */
#define X86_EXIT_NONE          0
#define X86_EXIT_TERMINATE     1  /* guest asked to exit (INT 21h AH=4Ch) */
#define X86_EXIT_HLE_INT       2  /* interrupt the host should service */
#define X86_EXIT_TIMER         3  /* PIT counter expired */
#define X86_EXIT_UNIMPLEMENTED 4  /* HLT, or an opcode with no handler */
#define X86_EXIT_MEM_FAULT     5  /* guest access to an unmapped page */
#define X86_EXIT_GPF           13 /* EXCP0D_GPF */
#define X86_EXIT_HALT          6  /* HLT: idle until an interrupt, not a stop */

/* Address-translation fold.
 *
 * Each entry is an OFFSET k, not a base: host = guest_linear + k. A mapped
 * page's k puts the result in RAM (0x20.. / 0x24..), so bit 31 is clear; an
 * unmapped page stores FOLD_INVALID, which drives the result negative. One
 * ADDS + BMI therefore validates every access at zero extra cost, replacing
 * the old BFC/BFI pair that trusted the entry blindly -- and a zero entry used
 * to alias address 0, which is ITCM, so a stray guest write corrupted the
 * interpreter's own code. */
/* Mode-record geometry. Mirrored by the .equ block in cpu_exec.S. */
/* Guest I/O device state. Lives outside X86CPUState: the interpreter never
   touches it, only the C port handlers do. */
typedef struct {
    /* THE FIRST FIVE BYTES OF THIS STRUCT ARE A PUBLISHED LAYOUT. They are
       mirrored by read_io_state() in tests/test_opcodes.py, which reads them
       by OFFSET -- inserting a field here silently breaks five speaker tests
       and nothing says why. Append to the end of the struct instead. */
    uint16_t pit2_reload;     /* channel 2 divisor -> speaker frequency */
    uint8_t  pit2_access;     /* latched access mode from port 0x43 */
    uint8_t  pit2_latched;    /* half-way through a lo/hi pair */
    uint8_t  port61;          /* bit0 = ch2 gate, bit1 = speaker data */
    uint32_t spk_phase;       /* 32-bit square-wave phase, carried across buffers */
    /* Speaker EVENTS, not a per-frame sample of the speaker.
     *
     * The renderer used to run once a frame and point-sample the CURRENT
     * state, so a tone that started and stopped between two calls was never
     * heard at all -- which is what "half the beeps are missing" was. Every
     * change to the audible frequency is timestamped in HOST milliseconds
     * (x86_io_time_ms) as the guest makes it, and the render then walks the
     * window and lays each segment down where it belongs.
     *
     * X86_SPK_EVENTS is small on purpose: a guest sweeping a siren changes the
     * divisor far faster than the output rate can show, and the overflow is
     * COUNTED (x86_spk_events_dropped) rather than papered over.
     *
     * The ring carries the PIT DIVISOR, not a frequency. It used to carry
     * `1193182 / divisor` as an integer, which threw away the pitch before the
     * generator ever saw it -- see x86_audio_wave.h. 0 means "gated off";
     * a guest reload of 0 is carried as 65536, so this cannot be uint16_t. */
#define X86_SPK_EVENTS 32u
    uint32_t spk_ev_ms[X86_SPK_EVENTS];
    uint32_t spk_ev_div[X86_SPK_EVENTS];
    uint8_t  spk_ev_head, spk_ev_tail;
    uint32_t spk_last_div;    /* divisor at the last logged event            */
    uint32_t spk_render_div;  /* divisor the renderer left off at            */
    uint32_t spk_window_ms;   /* host ms the next render's window starts at  */
    uint8_t  spk_was_on;      /* last rendered run was audible -> edge detect */
    uint8_t  cga_mode;        /* 0x3D8 mode control */
    uint8_t  cga_color_select;/* 0x3D9 background + palette + intensity */
    uint16_t cga_beam;        /* 0..0xFFFF through the frame, set by the core */
    uint32_t cga_status_reads;/* advances per 0x3DA read so poll loops progress */
    uint32_t input_keys;      /* host buttons, latched each frame */
    /* 8042-style scancode queue. Deliberately modelled at the port level:
       ports and IRQs behave identically in real and protected mode, whereas an
       INT 16h shim planted in the IVT is real-mode only. A game that installs
       its own INT 09h and reads 0x60 directly -- which most action games do --
       needs nothing above this layer. */
    uint8_t  kbd_queue[16];
    uint8_t  kbd_head, kbd_tail;
    uint8_t  kbd_last;        /* last byte handed out on port 0x60 */
    /* ---- the A20 gate ----------------------------------------------------
     * A20 is not a curiosity: it is the difference between "this machine has
     * memory above 1 MB" and "it does not". While the gate is MASKED, address
     * bit 20 is forced to zero and the 64 KB above 1 MB that segment:offset
     * can reach (0x100000-0x10FFEF, the HMA) aliases the bottom of memory --
     * which real-mode programs depend on. While it is OPEN, that range is its
     * own memory and the 8 MB space above 1 MB exists at all.
     *
     * Boot state is MASKED, which is what a real AT does and what preserves
     * the behaviour every guest in this tree has been tested against.
     *
     * Three doors, because guests use all three: the 8042 output port (0x64
     * command 0xD1 then a byte to 0x60, bit 1), the 8042's own 0xDD/0xDF
     * shortcuts, and the fast A20 gate at port 0x92 bit 1. */
    uint8_t  a20_open;        /* 0 = masked (HMA wraps), 1 = open */
    uint8_t  kbd_cmd;         /* 8042 command byte awaiting its 0x60 argument */
    uint8_t  kbd_outport;     /* 8042 output port latch; bit 1 IS A20 */
    uint8_t  port92;          /* System Control Port A latch */
    /* Channel 0 IS the system timer, and its divisor is the guest's whole
       sense of elapsed time. The boot value is 65536 (18.2065 Hz); a title
       that wants finer time programs its own -- Wolfenstein 3D writes 0x214B,
       i.e. 140 Hz, and drives its tic counter from the resulting ISR. Dropping
       that write (which is what happened until this was modelled) does not
       make the guest run slowly, it makes the guest's CLOCK run at 18.2/140 of
       real time, and everything the guest paces by it slows to match: turning,
       animation, menu key-repeat. See the clock-2 block in src/main.c.
       At the END of the struct deliberately -- see the note at the top. */
    uint16_t pit0_reload;     /* channel 0 divisor -> the system timer rate */
    uint8_t  pit0_access;     /* latched access mode from port 0x43 */
    uint8_t  pit0_latched;    /* half-way through a lo/hi pair */
} X86IOState;

extern X86IOState x86_io;

/* The fold table the interpreter dereferences. Defined in x86_cpu.c and
   repointed at runtime by the core, which allocates DTCM through mem_ctl. */
extern uint32_t *guest_page_table_ptr;
extern X86CPUState *cpu_state_ptr;

/* Dedicated CGA VRAM. Sized for the full 32 KB aperture at 0xB8000 even though
   mode 4 uses 16 KB, so text and the other CGA modes fit without remapping. */
#define X86_VRAM_BYTES 32768u

/* VGA mode 13h: 320x200x256 linear at guest 0xA0000, 64,000 displayed bytes.
   Backed with the whole 64 KB segment so 0xA0000-0xAFFFF is sixteen entire
   fold pages and the mapping needs no partial-page case. */
#define X86_VGA_APERTURE_BYTES 65536u
#define X86_VGA13_WIDTH        320u
#define X86_VGA13_HEIGHT       200u
#define X86_VGA13_BYTES        (X86_VGA13_WIDTH * X86_VGA13_HEIGHT)  /* 64000 */
/* Host state for the mode: 768 B of DAC (256 x RGB, 6 bits) then 1 KB of
   ARGB8888 CLUT. From the caller for the same reason as the aperture -- 66 KB
   of new .bss would break the guest allocation silently. */
#define X86_VGA_PALETTE_BYTES 2048u
/* Hand the video layer the aperture and its palette block. Either NULL means
   mode 13h has no storage, so a guest asking for it is REFUSED and logged
   like any other unrenderable mode rather than drawing into memory nothing
   reads. x86_vga13_ok is the witness. */
void x86_video_set_aperture(void *aperture, void *palette_block);

/* --- VGA aperture store hook (x86_video_planar.c) -------------------------
   The interpreter's converted store sites call this INSTEAD OF storing when
   cpu->vga_base is armed and the pointer lands in the 64 KB window. `off` is
   the window offset, `val` the value the guest wrote and `size` 1, 2 or 4.
   `x86_vga_planar_aperture` is the host base the offset is relative to; it is
   kept in step with the video layer's aperture by x86_video_set_aperture(). */
extern uint8_t *x86_vga_planar_aperture;
extern volatile uint32_t x86_vga_planar_stores;
void x86_vga_planar_store(uint32_t off, uint32_t val, uint32_t size);
extern volatile uint32_t x86_vga13_ok;
/* The EGA 0Dh card actually fitted, in PLANE OFFSETS (one offset is four
   interleaved bytes). Sized from the arena at the mode set rather than fixed,
   so "how big is the card" -- the first question a clipped picture raises -- is
   a number a probe can read. 0 while no planar mode is armed. */
extern volatile uint32_t x86_vga_ega_card_offsets;
extern volatile uint32_t x86_vga_dac_writes;
/* 256 ARGB8888 entries built from the guest's DAC. Non-NULL only while a
   256-colour mode is up; the frame loop pushes it through
   lcd_set_clut_ext() instead of the 32-entry lcd_set_clut(). */
const uint32_t *x86_video_clut256(void);
/* VGA DAC, ports 0x3C6-0x3C9. 0x3C9 is a STREAM, not a latch: a guest sets the
   index once and writes 768 consecutive bytes, so this cannot be modelled as
   "the last value written". */
void    x86_vga_dac_set_write_index(uint8_t idx);
void    x86_vga_dac_set_read_index(uint8_t idx);
void    x86_vga_dac_set_mask(uint8_t m);
uint8_t x86_vga_dac_get_mask(void);
void    x86_vga_dac_write(uint8_t v);
uint8_t x86_vga_dac_read(void);
void    x86_vga_dac_set_block(unsigned first, unsigned count, const uint8_t *rgb);

void x86_video_init(void *vram, void *lut8_palette);
void x86_video_refresh_palette(void);
extern uint32_t cga_clut_dirty;   /* frame loop re-pushes the CLUT when set */
extern uint8_t x86_video_mode_byte;   /* current BIOS video mode */
void x86_video_set_beam(uint32_t frac);
void x86_video_set_mode(uint8_t mode);
uint8_t x86_video_mode(void);
int x86_video_is_text(void);
/* Active text column count: 80 for modes 2/3, 40 for 0/1 and for the TTY grid
   of CGA mode 4/5. Selects the font (80 -> 4x8, 40 -> 8x8). */
unsigned x86_video_text_cols(void);
/* BDA 0040:0049 mode and 0040:004A columns; 0 if the page is not backed. */
int x86_bios_bda_video(uint8_t *mode, uint16_t *cols);
/* Ordered trace of everything that can change the active renderer; layout in
   x86_video.c. probe.py --dump x86_video_mode_log:16x4 */
extern volatile uint32_t x86_video_mode_log[16];
extern volatile uint32_t x86_video_mode_log_idx;
void x86_video_log_event(uint32_t tag, uint32_t aux, uint32_t value);
void x86_video_set_cursor(uint8_t col, uint8_t row);
/* The first MCB, i.e. where memory becomes the guest's to allocate. Anything
   the emulator plants for its own use has to live BELOW this, because a loaded
   program writes over whatever it finds above it. Shared so the HLE layer can
   check its stub placement against it at compile time. */
#define X86_DOS_ARENA_SEG 0x0070u
/* Where x86_hle.c plants the vector stubs (0x600..0x6FF). Shared because
   x86_dos.c builds the AH=52h SysVars block in the 256 bytes BELOW it and has
   to prove at compile time that the two do not overlap -- an interrupt whose
   stub had been overwritten by SysVars would execute it as code. */
#define X86_DOS_STUB_SEG  0x0060u

uint16_t x86_video_get_cursor(void);
uint16_t x86_video_read_cell(void);
void x86_video_scroll(uint8_t lines, uint8_t attr);
void x86_video_putc_attr(uint8_t ch, uint8_t attr, uint16_t count);
void x86_video_teletype(uint8_t ch);
void x86_video_putpixel(uint16_t x, uint16_t y, uint8_t colour);
void x86_video_set_palette_bios(uint8_t which, uint8_t value);
void x86_bios_init(void);
void x86_bios_tick(void);
void x86_kb_push(uint8_t scancode, uint8_t ascii);
void x86_kbd_scancode(uint8_t code);
int x86_kbd_pending(void);
/* Arm IRQ 1 for a queued scancode, refusing when the one-deep pending slot is
   taken and pulling the instruction boundary in only when IF allows delivery.
   See the comment on the definition -- both guards are measured defects. */
int x86_kbd_raise_irq(X86CPUState *cpu);
int x86_bios_kbd_irq(void);
/* Scancode -> ASCII, the same decode INT 09h uses. Shared with the on-screen
   keyboard, whose echo line must show what the guest was actually sent. */
uint8_t x86_kbd_ascii(uint8_t sc, int shift);
/* The 8x8 CP437 glyphs handed to the guest at F000:F000. One copy, borrowed by
   the OSK renderer -- see the accessor's comment in x86_hle.c. */
const unsigned char (*x86_font8x8(void))[8];
void x86_hle_set_zf(int set);
int x86_video_map(void);
uint8_t *x86_video_vram(void);
void x86_video_render(uint8_t *dst, uint32_t stride);
/* Once per frame, from the top of the render: re-fit the display card against
   the pager's MEASURED working set. x86_video.c has the safety argument. */
void x86_video_policy_tick(void);
/* Optional 2-D engine offload for the CHAINED mode 13h blit only; NULL (the
   default, and what the host tests build) keeps the CPU row loop. Contract and
   the reason it is a hook rather than a call: x86_video.c. */
extern int (*x86_video_blit_hook)(uint8_t *dst, const uint8_t *src,
                                  uint32_t bytes);

void x86_mem_pool_init(void *base, uint32_t bytes);
uint32_t x86_mem_pool_free(void);
int x86_map_range(uint32_t guest, uint32_t bytes);
int x86_map_fixed(uint32_t guest, void *host, uint32_t bytes);
/* Mappings REFUSED (NULL backing, or the page pool ran dry). Non-zero means a
   guest region is deliberately absent; the guest then takes a clean
   X86_EXIT_MEM_FAULT instead of hard-faulting the interpreter. */
extern volatile uint32_t x86_map_failures;

/* NOR cold-page backing (x86_nor.c). x86_nor_attach() takes bytes that are
   ALREADY durable and byte-verifies them before it moves the fold table, so a
   failed snapshot leaves the guest running unchanged out of AXI. The two
   slow_* entries are called from the trap stubs in cpu_exec.S, never from C. */
int  x86_nor_attach(void *nor, uint32_t nor_bytes, void *ram, uint32_t ram_bytes,
                    uint32_t first_page);
uint32_t x86_nor_cow(uint32_t bad);   /* host-side pull-back; 0 = not NOR */
void x86_nor_slow_r(uint32_t bad);
void x86_nor_slow_w(uint32_t bad);
extern volatile uint32_t x86_nor_attached, x86_nor_base_addr, x86_nor_bytes;
extern volatile uint32_t x86_nor_pages_total, x86_nor_pages_nor, x86_nor_pages_axi;
extern volatile uint32_t x86_nor_axi_free, x86_nor_reads, x86_nor_writes;
extern volatile uint32_t x86_nor_cow_pulls, x86_nor_cow_failures;
extern volatile uint32_t x86_nor_slow_cycles, x86_nor_unmapped, x86_nor_attach_err;
extern volatile uint32_t x86_nor_host_cow;
/* Watchdog heartbeat for x86_nor_attach()'s 640 KB XIP readback -- the one
   stretch of the snapshot with no incidental kick from the flash or SD
   drivers. A hook rather than a direct wdog_refresh() so this file keeps
   knowing nothing about the firmware ABI; NULL (the tier-1 case) is a no-op.
   x86_nor_heartbeats counts the ticks whether or not a hook is installed. */
extern void (*x86_nor_heartbeat)(void);
extern volatile uint32_t x86_nor_heartbeats;
void x86_unmap_all(void);
/*
 * The real-mode address wrap, scoped to what it is actually FOR.
 *
 * This used to alias every fold entry from 1 MB to 16 MB onto the low 1 MB,
 * which is an 8086's 20-bit address space and not this machine's. The cost was
 * not theoretical: with every index above 1 MB an alias, there IS no memory
 * above 1 MB, and an 8 MB guest space cannot exist at all.
 *
 * What a real AT wraps is one 64 KB window -- segment:offset reaches at most
 * 0x10FFEF -- and it wraps it only while the A20 gate is MASKED. So this
 * aliases exactly the sixteen pages 0x100000-0x110000 onto 0x000000-0x010000,
 * and only when x86_io.a20_open is 0. Call it after the low megabyte is
 * mapped, and again from x86_a20_set() whenever a guest moves the gate.
 */
#define X86_HMA_FIRST_PAGE  (0x100000u >> FOLD_SHIFT)   /* 256 */
#define X86_HMA_PAGES       16u                          /* 64 KB */
void x86_mem_wrap_1mb(void);
/* Move the A20 gate. Re-aliases (or un-aliases) the HMA and nothing else.
   x86_a20_changes counts every ACTUAL transition -- a guest that opens an
   already-open gate is not a transition, and a counter that said it was could
   not distinguish "the guest is probing" from "the guest is toggling". */
void x86_a20_set(int open);
extern volatile uint32_t x86_a20_changes;
extern volatile uint32_t x86_a20_state;   /* host-readable mirror of the gate */
void *x86_guest_ptr(uint32_t lin, uint32_t bytes);
uint32_t x86_guest_copy_out(void *dst, uint32_t lin, uint32_t bytes);
uint32_t x86_guest_copy_in(uint32_t lin, const void *src, uint32_t bytes);

/* HLE services. */
int x86_hle_install(const uint8_t *vectors, unsigned count);
int x86_bios_rom_install(void);
int x86_hle_dispatch(uint8_t vector);
int x86_hle_service(void);
extern uint32_t x86_hle_calls;
void x86_hle_set_cf(int set);
void x86_set_ax(uint16_t v);
void x86_set_bx(uint16_t v);
void x86_set_cx(uint16_t v);
void x86_set_dx(uint16_t v);
void x86_set_al(uint8_t v);
void x86_set_ah(uint8_t v);
int x86_bios_video(uint8_t func);
int x86_bios_keyboard(uint8_t func);
int x86_bios_equipment(void);
int x86_bios_memsize(void);
int x86_bios_system(uint8_t func);
/* Conventional memory in KB, published through INT 12h and 0040:0013. The core
   sets it from the size it really allocated; see x86_bios.c. */
extern uint32_t x86_bios_conv_kb;
int x86_kb_get(uint16_t *key);
int x86_kb_peek(uint16_t *key);
int x86_bios_time(uint8_t func);
int x86_dos_int21(uint8_t func);
int x86_dos_terminate(void);
/* Terminate with a return code, which AH=4Dh reports to the parent. Plain
   x86_dos_terminate() is this with code 0 -- what INT 20h and AH=00h mean. */
int x86_dos_terminate_code(uint16_t code);
/* EXEC (AH=4Bh) counters. "Did the EXEC happen at all" is the first question
   asked of any run that gets this far, and it must be answerable from the host
   without a trace. x86_dos_exec_last_err carries the DOS error of the last
   refusal, or 0x1xx for an unimplemented SUBfunction (AL). */
extern volatile uint32_t x86_dos_execs;
/* The ROOT program's environment segment. It was always 0 -- dos_env_build()
   was reachable only from EXEC and every shipped title is the root -- which is
   why BLASTER could not have been seen by anything even had it been written. */
extern volatile uint32_t x86_dos_env_seg;
extern volatile uint32_t x86_dos_exec_fails;
extern volatile uint32_t x86_dos_exec_depth_max;
extern volatile uint32_t x86_dos_exec_last_err;
/* AL=03h (LOAD OVERLAY) counters. An overlay allocates nothing and terminates
   never, so the EXEC counters above cannot describe it: these say how many
   landed, and where the last one went. */
/* The MZ loader's honesty counters. A guest access can be unresolvable
   mid-copy, so an image can land PARTIALLY; these say whether the last one
   did. x86_dos_load_shorts != 0 means some program is only half resident. */
extern volatile uint32_t x86_dos_load_bytes;
extern volatile uint32_t x86_dos_load_short;
extern volatile uint32_t x86_dos_load_shorts;
extern volatile uint32_t x86_dos_overlays;
extern volatile uint32_t x86_dos_overlay_seg;
extern volatile uint32_t x86_dos_overlay_reloc;
/* AH=52h (list of lists) and AH=29h (parse filename into FCB). Call counts
   only: the SysVars block is ordinary guest memory reached through the fold
   table, so WHICH offsets a title reads cannot be seen without faulting the
   granule, which the pager has no read-side mechanism for. See dos_sysvars(). */
extern volatile uint32_t x86_dos_sysvars_calls;
extern volatile uint32_t x86_dos_parse_calls;
void x86_dos_arena_init(void);
void x86_disk_set_memory(const void *base, uint32_t bytes);
void x86_disk_set_reader(int (*rd)(void *, void *, uint32_t, uint32_t),
                         void *ctx, uint32_t bytes);
/* Optional: makes the guest floppy writable. Without it disk_write refuses. */
void x86_disk_set_writer(int (*wr)(void *, const void *, uint32_t, uint32_t));
int x86_disk_ready(void);
/*
 * Disk instrumentation and the sector cache. Counts are exact everywhere;
 * TIMES are meaningless under gwemu, which does not model SD busy state.
 */
void x86_disk_stats_reset(void);
extern volatile uint32_t x86_disk_reads;           /* disk_read() calls       */
extern volatile uint32_t x86_disk_read_sectors;
extern volatile uint32_t x86_disk_backend_reads;   /* reached the SD          */
extern volatile uint32_t x86_disk_backend_sectors;
extern volatile uint32_t x86_disk_cycles_last;
extern volatile uint32_t x86_disk_cycles_worst;
extern volatile uint32_t x86_disk_us_total;
extern volatile uint32_t x86_disk_us_worst;
extern volatile uint32_t x86_disk_cycles_per_us;   /* measured by the core    */
extern volatile uint32_t x86_disk_errors;
extern volatile uint32_t x86_disk_run_hist[8];     /* sectors per call        */
/*
 * Sector cache + read-ahead. `mem` must be at least x86_disk_cache_bytes and
 * comes from the AHB heap (GW_MEM_AHB) -- D2 SRAM, the only SRAM the DMA
 * controllers can reach and the one ABI pool with a real free(). RAM_EMU has
 * no room, and the LCD bonus pool is now the mode 13h aperture's.
 * Returns 0 if it could not be backed, and the caller MUST report that:
 * the cache silently absent is a stutter nobody can explain.
 *
 * The geometry is tunable at build time (see x86_disk.c); keep this byte count
 * derived from it rather than restating 64 KB.
 */
#ifndef X86_DISK_WINDOW_SECTORS
#define X86_DISK_WINDOW_SECTORS 32u
#endif
#ifndef X86_DISK_WINDOW_COUNT
#define X86_DISK_WINDOW_COUNT   4u
#endif
#define X86_DISK_CACHE_BYTES \
    (X86_DISK_WINDOW_SECTORS * 512u * X86_DISK_WINDOW_COUNT)
int  x86_disk_cache_init(void *mem, uint32_t bytes);
void x86_disk_cache_flush(void);
extern volatile uint32_t x86_disk_cache_ok;        /* 0 = NOT backed          */
extern volatile uint32_t x86_disk_cache_bytes;
extern volatile uint32_t x86_disk_cache_hits;      /* sectors                 */
extern volatile uint32_t x86_disk_cache_misses;
extern volatile uint32_t x86_disk_cache_fills;
extern volatile uint32_t x86_disk_cache_evictions;
extern volatile uint32_t x86_disk_cache_bypass;
extern volatile uint32_t x86_disk_cache_invalidates;
int x86_dos_exec(const char *path, const char *cmdline);
/* The guest's current directory, as a FatFs-shaped prefix ("DF", no trailing
   separator). FF_FS_RPATH is 0, so FatFs cannot hold this for us; every
   relative path the guest opens is resolved against it in x86_dos.c. */
void x86_dos_set_cwd(const char *dir);
const char *x86_dos_get_cwd(void);
uint16_t x86_dos_psp(void);
extern uint32_t x86_dos_last_error;
/* Last path the guest opened or created. Diagnostic only: an unserviced file
   request is far easier to place with the name in hand. */
extern char x86_dos_last_path[32];
/*
 * File-family INT 21h trace. x86_hle_log is useless for a file bug: a guest
 * polling the keyboard fills all 32 slots between one open and the next. This
 * ring records ONLY the file calls, as pairs of words --
 *   [2i]   (func << 8) | AL
 *   [2i+1] (CF << 16) | AX-returned
 * -- plus the path of the last four opens, so "which call, on which name, with
 * what answer" is one dump instead of a rebuild.
 */
extern uint32_t x86_dos_file_log[96 * 4];
extern uint32_t x86_dos_file_log_idx;
extern uint32_t x86_dos_file_calls;
extern volatile uint32_t x86_io_log[64];
extern volatile uint32_t x86_io_log_idx;
extern volatile uint32_t x86_io_log_total;
extern char x86_dos_path_log[16][32];
uint16_t *x86_hle_flags_on_stack(void);
uint16_t dos_test_alloc(uint16_t paras, uint16_t *largest);
uint16_t dos_test_free(uint16_t seg);
uint16_t dos_test_resize(uint16_t seg, uint16_t paras, uint16_t *largest);
uint16_t dos_test_walk(void);
extern uint32_t x86_mcb_reads;      /* MCB header reads: the arena's cost */
uint16_t dos_test_alloc_owner(uint16_t paras, uint16_t owner, uint16_t *largest);
uint16_t dos_test_owner_of(uint16_t data_seg);
extern uint32_t x86_hle_unhandled_vector, x86_hle_unhandled_func;

uint32_t x86_port_read(uint32_t port, uint32_t size);
void x86_port_write(uint32_t port, uint32_t value, uint32_t size);
/* PIT channel 0: the SYSTEM TIMER's divisor, 65536 until a guest programs it.
   The frame loop divides the PIT input by this to decide how many IRQ 0s a
   millisecond of host time owes -- see the three-clock block in src/main.c. */
uint32_t x86_pit0_divisor(void);
extern volatile uint32_t x86_pit0_writes;
/* The PIT's free-running counter, in 1.193182 MHz ticks of GUEST time. See the
   block comment in x86_io.c: guest time is instructions at x86_pit_kips. */
extern uint32_t x86_pit_kips;         /* main.c publishes guest_kips here */
void     x86_pit_sync(void);          /* accrue what the guest has just run */
void     x86_pit_counter_forced(uint32_t v);  /* host write to pit_counter  */
uint32_t x86_pit_ticks(void);         /* reporting and tier-1 assertions    */
uint32_t x86_speaker_divisor(void);   /* the signal path's pitch source */
uint32_t x86_speaker_hz(void);        /* reporting only */
void x86_speaker_render(int16_t *dst, unsigned frames, unsigned sample_rate,
                        int16_t amplitude);
/* Host millisecond clock, for timestamping speaker events. The core points
   this at the firmware's HAL_GetTick; the bare-metal harness leaves it NULL,
   which collapses every event onto the start of the window -- i.e. exactly the
   old point-sampled behaviour, so tier-1 expectations are unchanged. */
extern uint32_t (*x86_io_time_ms)(void);
extern volatile uint32_t x86_spk_events;          /* frequency changes logged */
extern volatile uint32_t x86_spk_events_dropped;  /* ring overflows           */
extern volatile uint32_t x86_spk_log[32];         /* (port<<24)|value, last 32 */
extern volatile uint32_t x86_spk_log_idx;
/* Port 0x60: how often the guest asked, and the last 32 bytes it was given. */
extern volatile uint32_t x86_kb_pushes;
extern volatile uint32_t x86_kb_pops;
extern volatile uint32_t x86_kb_peek_hits;
extern volatile uint32_t x86_kbd_port60_reads;
extern volatile uint32_t x86_kbd_read_log[32];
extern volatile uint32_t x86_kbd_read_log_idx;
/* SOUND. Detection (OPL2 timers, the DSP reset handshake) AND DIGITISED
   PLAYBACK: an 8237 pair, IRQ 5 through the 8259, the CT1745 mixer and the
   SB16 command block. See the sound block in x86_io.c.

   FM SYNTHESIS IS STILL ABSENT -- the OPL timers answer a detection probe and
   the register file is deliberately not stored, so a title using AdLib MUSIC
   is silent while its digitised effects play.

   x86_sb_dma_refused now means what its name says and no more: a transfer this
   model CANNOT run (record, or a direct-DAC write with no DMA behind it). It
   is no longer the count of "asked for audio and got silence" -- for that, read
   x86_sb_samples against x86_sb_starts. */
extern volatile uint32_t x86_opl_writes;
extern volatile uint32_t x86_opl_status_reads;
extern volatile uint32_t x86_opl_timer_expiry;
extern volatile uint32_t x86_opl_t1_gap;
extern volatile uint32_t x86_io_isa_ticks;
extern volatile uint32_t x86_sb_resets;
extern volatile uint32_t x86_sb_cmds;
extern volatile uint32_t x86_pic_imr_now;
extern volatile uint32_t x86_pic_imr_ever;
extern volatile uint32_t x86_pic_imr_writes;
extern volatile uint32_t x86_sb_irq_requests;
extern volatile uint32_t x86_sb_cmd_hist_lo;
extern volatile uint32_t x86_sb_cmd_hist_hi;
extern volatile uint32_t x86_sb_cmds_unknown;
extern volatile uint32_t x86_sb_dma_refused;
extern volatile uint32_t x86_sb_reads_empty;
extern volatile uint32_t x86_sb_starts;
extern volatile uint32_t x86_sb_blocks;
extern volatile uint32_t x86_sb_dma_bytes;
extern volatile uint32_t x86_sb_samples;
extern volatile uint32_t x86_sb_irqs;
extern volatile uint32_t x86_sb_irqs_delivered;
extern volatile uint32_t x86_sb_irqs_masked;
extern volatile uint32_t x86_sb_irqs_lost;
extern volatile uint32_t x86_sb_irqs_acked;
extern volatile uint32_t x86_sb_eois;
extern volatile uint32_t x86_sb_underruns;
extern volatile uint32_t x86_sb_adpcm_silenced;
extern volatile uint32_t x86_sb_rate;
extern volatile uint32_t x86_sb_dma_maps;
extern volatile uint32_t x86_dma_writes;

/* Mix one frame's worth of Sound Blaster output ON TOP of `dst`, and by doing
   so advance the DMA transfer -- there is no other clock. Call it AFTER
   x86_speaker_render() has filled the buffer. Returns the samples contributed.
   See the engine block in x86_io.c for why this is the only driver. */
unsigned x86_sb_render_mix(int16_t *dst, unsigned frames, unsigned sample_rate,
                           int16_t amplitude);
int x86_sb_playing(void);
/* Hand the guest IRQ 5 if the card is asking for one and the 8259 can take it.
   Same contract as x86_kbd_raise_irq(); call it from the same places. */
int x86_sb_raise_irq(X86CPUState *cpu);

#define X86_MODE_REC_STRIDE     2048
#define X86_MODE_REC_TABLE_OFF  1024
#define X86_MODE_RECORDS_BYTES  (4 * X86_MODE_REC_STRIDE)
#define X86_MODE_RECORDS_ALIGN  8192

/* ---- the protected-mode descriptor walk (x86_pmode.c) --------------------
 *
 * Called from L(SET_SREG) in cpu_exec_handlers.S, and ONLY when CR0.PE is set
 * -- the test that reaches it is the one that already stood there to trap out,
 * so real mode pays nothing new for this existing.
 *
 * Returns 0 on success. On failure returns the EXCEPTION VECTOR (13 #GP,
 * 11 #NP, 12 #SS) and leaves the error code in cpu->pmode_err; the caller
 * exits the interpreter. `seg` is X86_CPU_SEG_*.
 */
int x86_pmode_load_seg(X86CPUState *cpu, uint32_t seg, uint32_t sel);
/* LLDT. Same return convention. */
int x86_pmode_lldt(X86CPUState *cpu, uint32_t sel);
/* Re-derive the dispatch table from CS.D after a protected-mode CS load.
   Exposed for the tier-1 harness, which sets segments up by hand. */
void x86_pmode_sync_mode(X86CPUState *cpu);
/* Segment-limit test for one access, in the shape a future protected-mode
   ModRM decoder will inline. NOT on the access path today -- see x86_pmode.c
   on why enforcing it costs real mode nothing only via a mode-record axis. */
int x86_pmode_limit_ok(const X86CPUSeg *s, uint32_t off, uint32_t bytes);
extern volatile uint32_t x86_pmode_loads;      /* descriptor walks performed */
extern volatile uint32_t x86_pmode_faults;     /* walks that raised          */

void x86_build_mode_records(void *dtcm_block);
uint32_t x86_mode_table(int op32, int addr32);
extern uint8_t *mode_records_ptr;

#define FOLD_SHIFT       12                 /* 4 KB granules */
#define FOLD_ENTRIES     4096               /* 16 MB direct window */
#define FOLD_INVALID     0x80000000u        /* forces bit 31 of the result */
/* ---- the granule seam (x86_seam.c) -------------------------------------- */
void x86_seam_sync(uint32_t lin);
void x86_seam_flush(void);
void x86_seam_reset(void);
extern volatile uint32_t x86_seam_syncs;    /* straddles SERVED             */
extern volatile uint32_t x86_seam_writes;   /* bytes a straddling store put back */
extern volatile uint32_t x86_seam_overflow; /* straddles NOT served -- LOUD */
extern volatile uint32_t x86_seam_flushes;

/* The code-stream seam. Called from the dispatch when the opcode byte sits in
   the last fifteen bytes of its granule, so cpu->code_k cannot cover the whole
   instruction; stages sixteen bytes into DTCM and re-points code_k. */
void *x86_code_reloc(uint32_t lin);
extern volatile uint32_t x86_x87_escapes; /* D8-DF ignored: no x87 fitted */

extern volatile uint32_t x86_code_relocs;       /* instructions staged        */
extern volatile uint32_t x86_code_reloc_faults; /* bytes that would not fold  */

#define FOLD_K(host, idx)  ((uint32_t)(host) - ((uint32_t)(idx) << FOLD_SHIFT))

/* ---- the VGA aperture's fold sentinel ------------------------------------
 *
 * While a planar mode is armed the aperture's sixteen entries fold to
 * `0x80000000 | host`, so every access to 0xA0000-0xAFFFF trips the ADDS/BMI
 * the interpreter already runs and NOTHING is spent on the aperture by a title
 * that never enters a planar mode. The three trap causes stay separable by a
 * single unsigned compare -- fold miss 0x80xxxxxx, NOR 0x9xxxxxxx, aperture
 * >= 0xA0000000 -- which is only true while the aperture is in 0x20000000-
 * 0x3FFFFFFF (AXI/DTCM). X86_FOLD_VGA_OK() is that condition, and arming
 * REFUSES the sentinel rather than installing a colliding one.
 */
#define X86_FOLD_VGA_TAG   0x80000000u
#define X86_FOLD_VGA_MIN   0xA0000000u
#define X86_FOLD_VGA_OK(host) \
    (((uint32_t)(uintptr_t)(host) & 0xE0000000u) == 0x20000000u)
#define X86_FOLD_VGA_K(host, first_idx) \
    (FOLD_K(host, first_idx) + X86_FOLD_VGA_TAG)
/* Is a trapped pointer an aperture access, and what host address is it? */
#define X86_FOLD_IS_VGA(bad)   ((uint32_t)(bad) >= X86_FOLD_VGA_MIN)
#define X86_FOLD_VGA_HOST(bad) ((uint32_t)(bad) & ~X86_FOLD_VGA_TAG)
/* Resolve `lin` through fold constant `k` the way the interpreter's trap stubs
   do, tag stripped. EVERY C-side walk of the fold table must use this: a raw
   `lin + k` over an aperture page yields 0xAxxxxxxx, which is not memory, and
   dereferencing it is a hard fault. That is not hypothetical -- the pager's
   slack prime (x86_pager.c) and the seam's neighbour pull (x86_seam.c) both do
   exactly this walk, and the pager one took Wolfenstein 3-D down. */
static inline void *x86_fold_ptr(uint32_t lin, uint32_t k)
{
    uint32_t host = lin + k;
    if (X86_FOLD_IS_VGA(host))
        host = X86_FOLD_VGA_HOST(host);
    return (void *)(uintptr_t)host;
}

/* get_reg/set_reg additional constants */
#define X86_CPU_REG_EIP 8
#define X86_CPU_REG_CR0 9
#define X86_CPU_REG_CR2 10

#define X86_CPU_SEG_ES 0
#define X86_CPU_SEG_CS 1
#define X86_CPU_SEG_SS 2
#define X86_CPU_SEG_DS 3
#define X86_CPU_SEG_FS 4
#define X86_CPU_SEG_GS 5
#define X86_CPU_SEG_LDT 6
#define X86_CPU_SEG_TR 7
#define X86_CPU_SEG_GDT 8
#define X86_CPU_SEG_IDT 9

X86CPUState *x86_cpu_init(PhysMemoryMap *mem_map);
void x86_cpu_end(X86CPUState *s);
void x86_cpu_interp(X86CPUState *s, int max_cycles1);
void x86_cpu_set_irq(X86CPUState *s, BOOL set);
void x86_cpu_set_reg(X86CPUState *s, int reg, uint32_t val);
uint32_t x86_cpu_get_reg(X86CPUState *s, int reg);
void x86_cpu_set_seg(X86CPUState *s, int seg, const X86CPUSeg *sd);
void x86_cpu_set_get_hard_intno(X86CPUState *s,
                                int (*get_hard_intno)(void *opaque),
                                void *opaque);
void x86_cpu_set_get_tsc(X86CPUState *s,
                         uint64_t (*get_tsc)(void *opaque),
                         void *opaque);
void x86_cpu_set_port_io(X86CPUState *s, 
                         DeviceReadFunc *port_read, DeviceWriteFunc *port_write,
                         void *opaque);
void x86_cpu_sync_port_map(X86CPUState *s, PhysMemoryMap *port_map);
int64_t x86_cpu_get_cycles(X86CPUState *s);
BOOL x86_cpu_get_power_down(X86CPUState *s);
void x86_cpu_flush_tlb_write_range_ram(X86CPUState *s,
                                       uint8_t *ram_ptr, size_t ram_size);
