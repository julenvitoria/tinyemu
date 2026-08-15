/* Per-opcode DWT cycle profiler. See x86_profile.h for the layout and the
   caveats; this file only owns the storage and the calibration. */

#include <string.h>

#include "x86_profile.h"

uint32_t *x86_profile_ptr;

#if X86_PROFILE

/* Bare-metal harness (tier 1): DTCM is a load target, so the block is a plain
   section placement. The packed core has no such section -- DTCM there is a
   firmware bump pool -- and passes a block in instead. */
#ifndef PROJECT_KIND_CORE
static uint32_t x86_profile_block[X86_PROFILE_WORDS]
    __attribute__((section(".dtcm_bss"), aligned(8)));
#endif

void x86_profile_reset(void)
{
    if (!x86_profile_ptr) {
        return;
    }
    memset(x86_profile_ptr, 0, X86_PROFILE_BYTES);
    x86_profile_ptr[0] = X86_PROFILE_HOST_SLOT;
    x86_profile_ptr[3] = X86_PROFILE_MAGIC;
}

bool x86_profile_init(void *dtcm_block_or_null)
{
    uint32_t *block = (uint32_t *)dtcm_block_or_null;
#ifndef PROJECT_KIND_CORE
    if (!block) {
        block = x86_profile_block;
    }
#endif
    if (!block) {
        return false;      /* caller reports it; the core then refuses to run */
    }
    x86_profile_ptr = block;
    x86_profile_reset();
    /* Calibrated once, here, so the number is always present in the block and
       a profile is never read without it. 4096 iterations is ~0.4 ms. */
    x86_profile_ptr[2] = x86_profile_calibrate(4096);
    x86_profile_reset_preserving_overhead();
    return true;
}

void x86_profile_reset_preserving_overhead(void)
{
    uint32_t overhead = x86_profile_ptr ? x86_profile_ptr[2] : 0;
    x86_profile_reset();
    if (x86_profile_ptr) {
        x86_profile_ptr[2] = overhead;
    }
}

#endif /* X86_PROFILE */
