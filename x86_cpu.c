/*
 * x86 CPU emulator stub
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
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include "cutils.h"
#include "x86_cpu.h"

// Allocate the CPU state in 0-wait-state DTCM for maximum performance.
//
// Only the bare-metal harness can place this statically. As a retro-go core,
// DTCM is a firmware bump pool (mem_ctl(GW_MEM_DTC, ...)), not a load target,
// so the core allocates at runtime and repoints cpu_state_ptr. The assembly
// reaches both through the pointer, never the symbol.
X86CPUState cpu_state_dtcm __attribute__((section(".dtcm_bss")));

// What the interpreter actually dereferences. Kept in normal .bss so it is
// writable in every build.
X86CPUState *cpu_state_ptr = &cpu_state_dtcm;
uint32_t *guest_page_table_ptr;

// The assembly now gets its offsets from generated x86_offsets.inc, so it cannot
// drift. These asserts guard the remaining hand-written copies: the absolute
// offsets that tests/test_opcodes.py, test_game.py and test_benchmark.py poke
// over the debug backend. Change a field before one of these and the Python
// harness reads the wrong word silently -- so fail the build here instead.
_Static_assert(offsetof(X86CPUState, eflags) == 4, "EFLAGS_OFFSET");
_Static_assert(offsetof(X86CPUState, regs) == 8, "REGS_OFFSET");
_Static_assert(offsetof(X86CPUState, regs[4]) == 24, "REG_ESP_OFFSET");
_Static_assert(offsetof(X86CPUState, segs) == 48, "CS seg base");
_Static_assert(offsetof(X86CPUState, cr0) == 120, "CR0 offset");
_Static_assert(offsetof(X86CPUState, pit_counter) == 2184, "pit_counter offset");
_Static_assert(offsetof(X86CPUState, exit_reason) == 2196, "exit_reason offset");
_Static_assert(offsetof(X86CPUState, opcode_table_base) == 2204, "TABLE_BASE_OFFSET");

X86CPUState *x86_cpu_init(PhysMemoryMap *mem_map)
{
    // Return the statically allocated DTCM instance instead of dynamic allocation or KVM
    memset(&cpu_state_dtcm, 0, sizeof(X86CPUState));
    return &cpu_state_dtcm;
}

void x86_cpu_end(X86CPUState *s)
{
}

extern void x86_cpu_interp_asm(void);

/* Run the guest until the core exits, then leave s->exit_reason for the caller.
 *
 * There is deliberately no INT 21h handler here. The old int21_hle.c was a
 * skeleton (one working case, zero FatFs calls, a guest-memory accessor that
 * returned NULL) compiled by no Makefile target, and its premise -- serve INT
 * 21h on the host -- is superseded: the DOS model runs a real FreeDOS kernel in
 * the guest and high-level-emulates the drivers *below* it, observing INT 21h
 * rather than serving it. See PLAN.md.
 *
 * X86_EXIT_HLE_INT is not yet produced by the core either; the hypercall
 * mechanism (0F 0B imm8) does not exist yet. Callers should expect
 * X86_EXIT_TIMER and X86_EXIT_UNIMPLEMENTED today.
 */
void x86_cpu_interp(X86CPUState *s, int max_cycles1)
{
    s->exit_reason = X86_EXIT_NONE;
    /* max_cycles1 is the caller's execution budget; the core spends it via the
       PIT counter, which is the only exit budget the assembly has. */
    if (max_cycles1 > 0) {
        s->pit_counter = (uint32_t)max_cycles1;
    }
    x86_cpu_interp_asm();
}

void x86_cpu_set_irq(X86CPUState *s, BOOL set)
{
}

void x86_cpu_set_reg(X86CPUState *s, int reg, uint32_t val)
{
    if (reg >= 0 && reg < 8) {
        s->regs[reg] = val;
    } else if (reg == X86_CPU_REG_EIP) {
        s->eip = val;
    } else if (reg == X86_CPU_REG_CR0) {
        s->cr0 = val;
    } else if (reg == X86_CPU_REG_CR2) {
        s->cr2 = val;
    }
}

uint32_t x86_cpu_get_reg(X86CPUState *s, int reg)
{
    if (reg >= 0 && reg < 8) {
        return s->regs[reg];
    } else if (reg == X86_CPU_REG_EIP) {
        return s->eip;
    } else if (reg == X86_CPU_REG_CR0) {
        return s->cr0;
    } else if (reg == X86_CPU_REG_CR2) {
        return s->cr2;
    }
    return 0;
}

void x86_cpu_set_seg(X86CPUState *s, int seg, const X86CPUSeg *sd)
{
    if (seg >= 0 && seg < 6) {
        s->segs[seg] = *sd;
    } else if (seg == X86_CPU_SEG_GDT) {
        // Not yet implemented for GDT in C state properly if it expects different struct
        // Actually we don't have GDT descriptor in X86CPUState, just limit/base usually
    } else if (seg == X86_CPU_SEG_IDT) {
        s->idtr_base = sd->base;
        s->idtr_limit = sd->limit;
    }
}

void x86_cpu_set_get_hard_intno(X86CPUState *s,
                                int (*get_hard_intno)(void *opaque),
                                void *opaque)
{
}

void x86_cpu_set_get_tsc(X86CPUState *s,
                         uint64_t (*get_tsc)(void *opaque),
                         void *opaque)
{
}

typedef uint32_t DeviceReadFunc(void *opaque, uint32_t offset, int size_log2);
typedef void DeviceWriteFunc(void *opaque, uint32_t offset, uint32_t val, int size_log2);

DeviceReadFunc *io_port_read_table[1024] __attribute__((section(".dtcm_bss")));
DeviceWriteFunc *io_port_write_table[1024] __attribute__((section(".dtcm_bss")));
void *io_port_opaque_table[1024] __attribute__((section(".dtcm_bss")));

void x86_cpu_sync_port_map(X86CPUState *s, PhysMemoryMap *port_map)
{
    int i;
    for(i = 0; i < 1024; i++) {
        io_port_read_table[i] = NULL;
        io_port_write_table[i] = NULL;
        io_port_opaque_table[i] = NULL;
    }
    for(i = 0; i < port_map->n_phys_mem_range; i++) {
        PhysMemoryRange *pr = &port_map->phys_mem_range[i];
        if (pr->size > 0 && pr->addr < 1024) {
            uint32_t addr = pr->addr;
            uint32_t size = pr->size;
            if (addr + size > 1024) size = 1024 - addr;
            for(uint32_t j = 0; j < size; j++) {
                io_port_read_table[addr + j] = pr->read_func;
                io_port_write_table[addr + j] = pr->write_func;
                io_port_opaque_table[addr + j] = pr->opaque;
            }
        }
    }
}

void x86_cpu_set_port_io(X86CPUState *s, 
                         DeviceReadFunc *port_read, DeviceWriteFunc *port_write,
                         void *opaque)
{
    // The emulator currently uses the dtcm lookup table directly.
    // This function is retained for API compatibility or fallback.
}

int64_t x86_cpu_get_cycles(X86CPUState *s)
{
    return 0;
}

BOOL x86_cpu_get_power_down(X86CPUState *s)
{
    return FALSE;
}

void x86_cpu_flush_tlb_write_range_ram(X86CPUState *s,
                                       uint8_t *ram_ptr, size_t ram_size)
{
}

/* ---------------------------------------------------------------------------
 * Mode records.
 *
 * x86 carries operand size and address size as two INDEPENDENT axes: CS.D sets
 * the defaults and the 66/67 prefixes toggle them per instruction, in either
 * default mode. The interpreter models that with four records, each holding a
 * dispatch table plus the two pointers a handler needs, laid out so that a
 * single EOR on the live table pointer moves between them:
 *
 *   record i  =  base + i*2048,  i = opsize_is_32 + 2*addrsize_is_32
 *   EOR 2048 -> flip operand size      (prefix 66)
 *   EOR 4096 -> flip address size      (prefix 67)
 *
 * The base must be 8 KB aligned or those EORs would carry between records.
 *
 * Tables live here in DTCM rather than in ITCM with the code: they are data,
 * they are read once per instruction on the hottest path, and DTCM has a
 * second port so that read can pair with the page-table walk.
 */
uint8_t *mode_records_ptr;

extern const uint32_t opcode_table_16[256], opcode_table_32[256];
extern const uint32_t opcode_table_0f_16[256], opcode_table_0f_32[256];
extern void decode_modrm_addr16(void), decode_modrm_addr32(void);

void x86_build_mode_records(void *dtcm_block)
{
    uint8_t *base = (uint8_t *)(((uintptr_t)dtcm_block + 8191u) & ~8191u);
    mode_records_ptr = base;

    for (int i = 0; i < 4; i++) {
        int op32   = i & 1;
        int addr32 = (i >> 1) & 1;
        uint32_t *table = (uint32_t *)(base + i * X86_MODE_REC_STRIDE +
                                       X86_MODE_REC_TABLE_OFF);

        const uint32_t *src    = op32 ? opcode_table_32    : opcode_table_16;
        const uint32_t *src_0f = op32 ? opcode_table_0f_32 : opcode_table_0f_16;
        for (int e = 0; e < 256; e++)
            table[e] = src[e];

        /* Header, immediately below the table so handlers reach it with the
           small negative offsets LDR.W allows. */
        table[-1] = (uint32_t)(addr32 ? decode_modrm_addr32
                                      : decode_modrm_addr16) | 1u;
        table[-2] = (uint32_t)src_0f;
    }
}

uint32_t x86_mode_table(int op32, int addr32)
{
    return (uint32_t)(mode_records_ptr
                      + (op32 + 2 * addr32) * X86_MODE_REC_STRIDE
                      + X86_MODE_REC_TABLE_OFF);
}

/* Diagnostic: the last 16 CS loads, oldest-first modulo 16.
 * Each entry is (linear EIP at the moment of the load, new CS selector), which
 * names whatever transferred control -- far jump, far call, RETF or interrupt,
 * since all of them install CS through the same path. */
uint32_t x86_cs_log[64];   /* 16 entries of 4 words */
uint32_t x86_cs_log_idx;

/* Diagnostic: the last 16 interrupt VECTORS dispatched through the real-mode
 * IVT, paired one-for-one with the CS loads x86_cs_log records for them.
 * x86_cs_log names an interrupt only as "LR was HANDLE_INTERRUPT_16" -- it
 * cannot say WHICH one, and a guest that derails after installing a handler is
 * exactly the case where that is the whole question. Each entry is
 * (vector << 16) | new CS selector, so a vector whose IVT slot has been
 * clobbered reads as its own diagnosis. */
uint32_t x86_int_vec_log[16];
uint32_t x86_int_vec_log_idx;

/* Diagnostic: what the last IRET popped, and the SP it popped from.
 * [0]=IP [1]=CS [2]=FLAGS [3]=SP before the pops. */
uint32_t x86_iret_log[4];

/* Diagnostic sampler backing store; written only when the interpreter is built
   with X86_TRACE_EIP (see x86_trace.inc). 256 bytes, always present so the
   symbol exists for probe.py whether or not the sampler is compiled in. */
uint32_t x86_eip_log[64];
uint32_t x86_eip_log_idx;

/* First-odd-SP trap (X86_TRACE_ODDSP). A 16-bit DOS guest's SP is even for its
   whole life -- every PUSH/POP/CALL/RET/INT moves it by two -- so SP going ODD
   is a handler bug caught at the instant it happens, and catching an EVENT is
   the only way to name it: sampling cannot, because the run is not
   bit-reproducible and the culprit is one instruction in millions.
   X86_TRACE_ODDREG picks which register is watched (REG_ESP by default; point
   it at REG_EBP and the same trap names whatever made BP odd, which is the next
   question every time the answer is a `MOV SP,BP` epilogue).
   The EIP ring FREEZES on the catch rather than snapshotting into a second
   buffer: the sixteen instructions that led up to the event are the whole
   answer, and a ring that keeps running past the event overwrites them with
   whatever the guest did while dying.
     [0] caught flag  [1] instruction count at the catch  [2] EIP now
     [3] watched reg  [4] SS base  [5] ESP  [6] EBP
     [7] ring write index at the catch    [72] instruction count now
     [8..71] the frozen ring, oldest at (idx & 63)
     [73] CS base   [74..81] eight guest stack words from SS:(SP-8)
   The stack words are captured AT THE CATCH and not read afterwards on purpose:
   between a wild transfer and the halt the guest runs a few hundred thousand
   more instructions and writes straight over the frame that explains it.  */
/* x87 ESCAPE opcodes (D8-DF) ignored, because no coprocessor is fitted. See
   .op_esc_x87 in cpu_exec_handlers.S: a handful means a C runtime's detection
   ran; millions means a title is computing in floating point and getting
   nothing, which no picture would tell you. */
volatile uint32_t x86_x87_escapes = 0;

uint32_t x86_oddsp_log[82];
