/* Storage and calibration for the frame-loop zone accounting. The layout, the
   nesting scheme and the reason the root zone IS the residual are all in
   x86_zones.h; this file owns only the block and the on-target calibration. */

#include <string.h>

#include "x86_zones.h"

x86_zones_t *x86_zones_ptr;

#if X86_ZONES

/* Bare-metal harness (tier 1): DTCM is a load target, so the block is a plain
   section placement. The packed core has no such section -- DTCM there is a
   firmware bump pool -- and passes a block in instead. Same split as
   x86_profile.c, for the same reason. */
#ifndef PROJECT_KIND_CORE
static x86_zones_t x86_zones_block __attribute__((section(".dtcm_bss"),
                                                  aligned(8)));
#endif

void x86_zones_clear(x86_zones_t *b)
{
    if (!b)
        return;
    memset(b->z, 0, sizeof(b->z));
    b->frames = 0;
    b->overflow = 0;
    b->unbalanced = 0;
    b->reset = 0;
    b->sp = 0;
    b->lost = 0;
    b->stk[0] = X86_ZONE_FRAME;
    b->mark[0] = 0;
    b->last_ts = X86_ZONES_DWT_CYCCNT;
}

/*
 * What one enter/exit PAIR costs, measured with the very macros being
 * measured, against the scratch zone so no reported zone is polluted.
 *
 * It is a LOWER BOUND, and honestly so: this loop is tiny, perfectly
 * predicted, and everything it touches is in DTCM and hot. A transition taken
 * from cold code in the middle of the frame loop costs more. The number is
 * still worth having -- it bounds the perturbation from below, and the host
 * report multiplies it by the transition count to state the instrument's own
 * share of the frame.
 */
uint32_t x86_zones_calibrate(uint32_t iterations)
{
    if (!x86_zones_ptr || !iterations)
        return 0;

    /* Calibration must not appear in the results. Run it, then wipe. */
    x86_zones_ptr->sp = 0;
    x86_zones_ptr->stk[0] = X86_ZONE_FRAME;
    x86_zones_ptr->last_ts = X86_ZONES_DWT_CYCCNT;

    uint32_t t0 = X86_ZONES_DWT_CYCCNT;
    for (uint32_t i = 0; i < iterations; i++) {
        x86_zone_enter(X86_ZONE_CALIB);
        x86_zone_exit();
    }
    uint32_t t1 = X86_ZONES_DWT_CYCCNT;

    uint32_t per_pair = (t1 - t0) / iterations;
    x86_zones_clear(x86_zones_ptr);
    return per_pair;
}

bool x86_zones_init(void *dtcm_block_or_null)
{
    x86_zones_t *b = (x86_zones_t *)dtcm_block_or_null;
#ifndef PROJECT_KIND_CORE
    if (!b)
        b = &x86_zones_block;
#endif
    if (!b)
        return false;

    memset(b, 0, sizeof(*b));
    b->version = X86_ZONES_VERSION;
    x86_zones_ptr = b;
    x86_zones_clear(b);
    uint32_t overhead = x86_zones_calibrate(4096);
    b->overhead = overhead;
    /* Magic LAST: a host that reads a half-initialised block must see it as
       absent rather than as empty results. */
    b->magic = X86_ZONES_MAGIC;
    return true;
}

#endif /* X86_ZONES */
