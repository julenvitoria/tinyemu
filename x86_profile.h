/*
 * Per-opcode DWT cycle profiling for the Thumb-2 interpreter.
 *
 * OFF unless the build defines X86_PROFILE=1 (see Makefile / Makefile.core).
 * The instrumentation sits in the fetch/dispatch tail -- the hottest code in
 * the project -- so it must never be in a shipping build.
 *
 * What it measures: DWT->CYCCNT is read at every dispatch, and the delta from
 * the PREVIOUS dispatch is accumulated into that previous opcode's bucket.
 * CYCCNT counts real core cycles INCLUDING stalls, which is the number worth
 * having and the one gwemu cannot produce (it does not model the pipeline --
 * docs/06). Under gwemu the COUNTS are exact and the cycles are noise.
 *
 * Attribution is approximate BY CONSTRUCTION. The M7 is superscalar with an
 * out-of-order memory system, so a delta charged to opcode N carries some of
 * N-1's tail and whatever cache/AXI state N inherited. Use it to RANK hogs,
 * never as absolute per-opcode truth.
 *
 * Layout of the block x86_profile_ptr points at (all uint32, DTCM):
 *
 *   [0] prev_idx      slot whose bucket the next delta is charged to
 *   [1] prev_ts       CYCCNT at the last dispatch
 *   [2] overhead      calibrated cycles per instrumented dispatch (see below)
 *   [3] magic         X86_PROFILE_MAGIC, so a reader can tell it is live
 *   then X86_PROFILE_SLOTS entries of 4 words:
 *       [0] cycles_lo  [1] cycles_hi   64-bit: a 32-bit accumulator wraps in
 *                                      15 s of hot bucket at 280 MHz
 *       [2] count      [3] reserved
 *
 * Slots 0-255 are the one-byte opcodes, 256-511 the 0F two-byte space, and
 * slot 512 is HOST: everything outside the interpreter (the frame loop, the
 * blit, HLE work between two calls into x86_cpu_interp_asm). The interpreter
 * prologue opens slot 512, so the first dispatch of each entry charges the
 * host, not whichever opcode happened to run last.
 *
 * A slot is CLOSED (cycles accumulated, count incremented) by the NEXT
 * dispatch, so the last instruction before each exit from the interpreter is
 * never counted -- one instruction per interp call, and the HLT that ends the
 * synthetic benchmark is exactly it. That is why a benchmark of 1,000,001
 * instructions profiles as 1,000,000 dispatches in two slots. Do not "fix" it
 * by closing on exit: the exit path is the one place the delta would include
 * the host's re-entry.
 */
#ifndef X86_PROFILE_H
#define X86_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#define X86_PROFILE_HOST_SLOT   512
#define X86_PROFILE_SLOTS       513
#define X86_PROFILE_HDR_WORDS   4
#define X86_PROFILE_ENTRY_WORDS 4
#define X86_PROFILE_WORDS       (X86_PROFILE_HDR_WORDS + \
                                 X86_PROFILE_SLOTS * X86_PROFILE_ENTRY_WORDS)
#define X86_PROFILE_BYTES       (4 * X86_PROFILE_WORDS)
#define X86_PROFILE_MAGIC       0x50524F46u   /* 'PROF' */

/* The assembly reads this ONCE PER DISPATCH. Never NULL while the interpreter
   runs: init failure is reported to the caller, which refuses to start. */
extern uint32_t *x86_profile_ptr;

#if X86_PROFILE
/* Returns false if the DTCM block could not be allocated. Loud, not silent --
   see the RAM_EMU over-allocation defect in TOOLING.md for what silent looks
   like. */
bool x86_profile_init(void *dtcm_block_or_null);

/* Cycles the instrumentation itself costs per dispatch, measured on the target
   by running the identical macro against a scratch slot. Stored in header word
   [2] and reported by scripts/profile_opcodes.py; NOTHING subtracts it on the
   target, so raw numbers stay raw. */
uint32_t x86_profile_calibrate(uint32_t iterations);

void x86_profile_reset(void);
void x86_profile_reset_preserving_overhead(void);
#endif

#endif /* X86_PROFILE_H */
