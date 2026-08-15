/*
 * Phase/zone wall-time accounting for the emulator frame loop.
 *
 * OFF unless the build defines X86_ZONES=1 (Makefile.core). Every macro below
 * compiles to nothing otherwise, so a shipping core carries neither the code
 * nor the DTCM block.
 *
 * ---------------------------------------------------------------------------
 * THE QUESTION THIS ANSWERS, AND WHY NOTHING ELSE DID
 * ---------------------------------------------------------------------------
 * The project already measures per-OPCODE cycles (x86_profile.h), a host-PC
 * histogram, static per-handler instruction counts, and a dozen standing
 * counters. None of them says how ONE FRAME'S WALL TIME DIVIDES -- how much of
 * a 16 ms iteration was guest execution, how much was the blit, how much was a
 * blocking SD read, how much was idle waiting for the panel. "The game
 * stutters" is a statement about that division and there was no instrument for
 * it.
 *
 * ---------------------------------------------------------------------------
 * THE NESTING SCHEME: EXPLICIT PUSH/POP, EXCLUSIVE SELF-TIME
 * ---------------------------------------------------------------------------
 * Zones nest for real -- a blocking SD read happens inside an INT 21h service
 * which happens inside the frame loop -- so a scheme is not optional; without
 * one, DISK and HLE both count the same microseconds and the "breakdown" sums
 * to more than the frame.
 *
 * The scheme is a STACK, and every zone accumulates EXCLUSIVE SELF-TIME:
 *
 *   enter(z): charge (now - last_ts) to whatever is on top of the stack,
 *             then push z.
 *   exit():   charge (now - last_ts) to the top, then pop.
 *
 * So while DISK is on the stack, HLE accumulates nothing. HLE's figure is the
 * time it spent NOT in a child zone. The consequence to keep in mind when
 * reading a report: a zone's number is its OWN work, and a parent's total cost
 * is its self-time plus its children's. The host report says which is which.
 *
 * The frame itself is the ROOT zone (X86_ZONE_FRAME), pushed at the top of the
 * iteration and popped at the bottom. That makes the arithmetic honest for
 * free: every other zone's self-time is time inside the frame, so
 *
 *      X86_ZONE_FRAME's self-time IS THE UNACCOUNTED RESIDUAL.
 *
 * It is not derived, not inferred and not hidden -- it is the measured time
 * that fell through every named zone. A large residual is the report saying
 * "there is a phase here nobody instrumented", which is exactly the finding
 * this instrument exists to produce.
 *
 * Totals therefore satisfy, by construction and not by hope:
 *
 *      sum of all zones' self-time == total wall time of all frames measured
 *
 * ---------------------------------------------------------------------------
 * COST
 * ---------------------------------------------------------------------------
 * A transition is one DWT read, one subtract, one 64-bit accumulate and a
 * push or pop. Zones are per-FRAME and per-SERVICE-CALL -- never per guest
 * instruction, which is what x86_profile.h is for and what makes IT expensive.
 * The cost is calibrated ON THE TARGET (x86_zones_calibrate) against the same
 * macros and stored in the header, and scripts/profile_all.py prints it beside
 * the results rather than silently subtracting it.
 *
 * ---------------------------------------------------------------------------
 * gwemu
 * ---------------------------------------------------------------------------
 * gwemu does not model the M7 pipeline and its SD is free (docs/04, docs/06).
 * Under gwemu the zone COUNTS are exact and the zone CYCLES are an artefact of
 * the host -- in particular DISK reads as ~0 there and cannot. The host report
 * labels every run with its target for exactly this reason.
 */
#ifndef X86_ZONES_H
#define X86_ZONES_H

#include <stdbool.h>
#include <stdint.h>

/* Zone ids. 0 is the root/residual and must stay 0. Keep in step with
   ZONE_NAMES in scripts/common/zones.py -- the host names them by index. */
#define X86_ZONE_FRAME    0   /* root: the residual, see above              */
#define X86_ZONE_GUEST    1   /* x86_cpu_interp_asm()                       */
#define X86_ZONE_HLE      2   /* x86_hle_service(): BIOS/DOS, minus its I/O */
#define X86_ZONE_DISK     3   /* the SD backend itself (nested in HLE)      */
#define X86_ZONE_VIDEO    4   /* render + OSK draw + CLUT push              */
#define X86_ZONE_AUDIO    5   /* speaker render into the DMA half-buffer    */
#define X86_ZONE_INPUT    6   /* poll_input(): pad, retro-go menu, OSK      */
#define X86_ZONE_FWLOOP   7   /* the firmware's common_emu_frame_loop()     */
#define X86_ZONE_SWAP     8   /* idle: waiting for the panel swap           */
#define X86_ZONE_CALIB    9   /* scratch, used only by the calibration loop */
#define X86_ZONE_COUNT   10

#define X86_ZONE_STACK_MAX 8
#define X86_ZONES_MAGIC   0x5A4F4E45u   /* 'ZONE' */
#define X86_ZONES_VERSION 1

typedef struct {
    uint32_t cyc_lo;    /* 64-bit self-time: a 32-bit accumulator wraps in  */
    uint32_t cyc_hi;    /* 15 s of hot zone at 280 MHz                      */
    uint32_t count;     /* occurrences closed (pops)                        */
    uint32_t worst;     /* worst SINGLE occurrence, self cycles. Stutter is */
                        /* a tail property; a mean cannot show one          */
    uint32_t resv0;
    uint32_t resv1;
} x86_zone_t;

/*
 * The whole block, read by scripts/common/zones.py at fixed offsets. Laid out
 * as a struct rather than as a bare word array because unlike the opcode
 * profile it has no repeating body worth indexing by hand.
 */
typedef struct {
    uint32_t magic;      /* X86_ZONES_MAGIC once initialised               */
    uint32_t version;
    uint32_t reset;      /* HOST WRITES 1; the target zeroes at the next    */
                         /* frame boundary. Never zero this from the host   */
                         /* directly -- a reset mid-frame leaves the stack  */
                         /* marks pointing at counters that no longer exist */
    uint32_t overflow;   /* pushes refused: the stack was full              */
    uint32_t unbalanced; /* frames that ended with a zone still open        */
    uint32_t overhead;   /* calibrated cycles per enter+exit PAIR           */
    uint32_t frames;     /* frames closed since the last reset              */
    uint32_t last_ts;    /* CYCCNT at the last transition                   */
    uint32_t sp;         /* current stack depth (0 = root)                  */
    uint32_t lost;       /* enters that could not push and are being        */
                         /* balanced away by their own exit -- see below    */
    uint32_t resv[2];
    uint32_t stk[X86_ZONE_STACK_MAX];    /* zone id per level              */
    uint32_t mark[X86_ZONE_STACK_MAX];   /* that zone's cyc_lo at push     */
    x86_zone_t z[X86_ZONE_COUNT];
} x86_zones_t;

/* NULL until x86_zones_init succeeds. Every macro below tests it, so a core
   that failed to allocate the block runs correctly and simply reports
   nothing -- an instrument must never be the reason a build dies. */
extern x86_zones_t *x86_zones_ptr;

#if X86_ZONES

#define X86_ZONES_DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)

bool x86_zones_init(void *dtcm_block_or_null);
/* Cycles one enter/exit pair costs, measured on the target with the same
   inline code against the scratch zone. Stored in the header; NOTHING on the
   target subtracts it. */
uint32_t x86_zones_calibrate(uint32_t iterations);
void x86_zones_clear(x86_zones_t *b);

static inline void x86_zone_charge(x86_zones_t *b, uint32_t now)
{
    uint32_t d = now - b->last_ts;
    b->last_ts = now;
    x86_zone_t *z = &b->z[b->stk[b->sp]];
    uint32_t lo = z->cyc_lo + d;
    if (lo < z->cyc_lo)
        z->cyc_hi++;
    z->cyc_lo = lo;
}

static inline void x86_zone_enter(unsigned id)
{
    x86_zones_t *b = x86_zones_ptr;
    if (!b)
        return;
    x86_zone_charge(b, X86_ZONES_DWT_CYCCNT);
    if (b->sp + 1u >= X86_ZONE_STACK_MAX) {
        /* Cannot push. The matching exit() must NOT pop, or every zone above
           this point is charged to the wrong parent for the rest of the frame
           -- an accounting that is silently wrong is worse than one that is
           loudly incomplete. `lost` is the pairing counter that makes the
           refusal balanced; `overflow` is the report's witness that it
           happened. The real nesting is three deep (frame -> hle -> disk)
           against a stack of eight, so neither should ever be non-zero. */
        b->overflow++;
        b->lost++;
        return;
    }
    b->sp++;
    b->stk[b->sp] = id;
    b->mark[b->sp] = b->z[id].cyc_lo;
}

static inline void x86_zone_exit(void)
{
    x86_zones_t *b = x86_zones_ptr;
    if (!b || b->sp == 0u)
        return;                 /* never pop the root */
    if (b->lost) {
        b->lost--;              /* balance an enter() that could not push */
        return;
    }
    x86_zone_charge(b, X86_ZONES_DWT_CYCCNT);
    x86_zone_t *z = &b->z[b->stk[b->sp]];
    uint32_t self = z->cyc_lo - b->mark[b->sp];
    z->count++;
    if (self > z->worst)
        z->worst = self;
    b->sp--;
}

/* Open a frame. Deliberately RE-ROOTS the stack rather than assuming the
   previous frame left it balanced: one missing exit() must cost one frame's
   accounting, not every frame after it. */
static inline void x86_zone_frame_begin(void)
{
    x86_zones_t *b = x86_zones_ptr;
    if (!b)
        return;
    if (b->reset) {
        x86_zones_clear(b);
    }
    b->sp = 0;
    b->lost = 0;
    b->stk[0] = X86_ZONE_FRAME;
    b->mark[0] = b->z[X86_ZONE_FRAME].cyc_lo;
    b->last_ts = X86_ZONES_DWT_CYCCNT;
}

static inline void x86_zone_frame_end(void)
{
    x86_zones_t *b = x86_zones_ptr;
    if (!b)
        return;
    if (b->sp != 0u) {
        /* Counted, not corrected. An unbalanced frame's breakdown is wrong and
           the report has to be able to say so. */
        b->unbalanced++;
        while (b->sp != 0u)
            x86_zone_exit();
    }
    x86_zone_charge(b, X86_ZONES_DWT_CYCCNT);
    x86_zone_t *z = &b->z[X86_ZONE_FRAME];
    uint32_t self = z->cyc_lo - b->mark[0];
    z->count++;
    if (self > z->worst)
        z->worst = self;
    b->frames++;
}

#define X86_ZONE_ENTER(id)   x86_zone_enter(id)
#define X86_ZONE_EXIT()      x86_zone_exit()
#define X86_ZONE_FRAME_BEGIN() x86_zone_frame_begin()
#define X86_ZONE_FRAME_END()   x86_zone_frame_end()

#else  /* !X86_ZONES */

#define X86_ZONE_ENTER(id)     do { } while (0)
#define X86_ZONE_EXIT()        do { } while (0)
#define X86_ZONE_FRAME_BEGIN() do { } while (0)
#define X86_ZONE_FRAME_END()   do { } while (0)

#endif /* X86_ZONES */

#endif /* X86_ZONES_H */
