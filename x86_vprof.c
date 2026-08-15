/*
 * Storage and calibration for the video-path cycle instrument. The design, the
 * nesting rule, the reason each field is its own symbol and the honest
 * statement of the instrument's cost are all in x86_vprof.h.
 */

#include "x86_vprof.h"

#define X86_VPROF_DEF(name)                     \
    volatile uint32_t x86_vprof_##name##_cyc;   \
    volatile uint32_t x86_vprof_##name##_hi;    \
    volatile uint32_t x86_vprof_##name##_n;     \
    volatile uint32_t x86_vprof_##name##_worst

X86_VPROF_DEF(frame);
X86_VPROF_DEF(store);
X86_VPROF_DEF(paint);
X86_VPROF_DEF(render);
X86_VPROF_DEF(osk);
X86_VPROF_DEF(cell);
#if X86_VPROF
X86_VPROF_DEF(reloc);
X86_VPROF_DEF(seam);
#endif

volatile uint32_t x86_vprof_paint_flip;
volatile uint32_t x86_vprof_paint_loop;
volatile uint32_t x86_vprof_ovh;
volatile uint32_t x86_vprof_reset;

#if X86_VPROF

#define X86_VPROF_ZERO(name)                    \
    do {                                        \
        x86_vprof_##name##_cyc = 0;             \
        x86_vprof_##name##_hi = 0;              \
        x86_vprof_##name##_n = 0;               \
        x86_vprof_##name##_worst = 0;           \
    } while (0)

void x86_vprof_clear(void)
{
    X86_VPROF_ZERO(frame);
    X86_VPROF_ZERO(store);
    X86_VPROF_ZERO(paint);
    X86_VPROF_ZERO(render);
    X86_VPROF_ZERO(osk);
    X86_VPROF_ZERO(cell);
#if X86_VPROF
    X86_VPROF_ZERO(reloc);
    X86_VPROF_ZERO(seam);
#endif
    x86_vprof_paint_flip = 0;
    x86_vprof_paint_loop = 0;
}

/*
 * An EMPTY bracket pair, in the same inline code, against the scratch phase.
 * This is a LOWER BOUND on the instrument's cost: the loop stays in the
 * I-cache and its accumulators stay in the D-cache, where the real STORE
 * bracket runs in code competing for both. Nothing on the target subtracts it
 * -- the host report prints raw and corrected side by side.
 */
void x86_vprof_calibrate(uint32_t iterations)
{
    uint32_t t0, i;

    if (!iterations)
        iterations = 1000u;
    t0 = X86_VPROF_CYCCNT;
    for (i = 0; i < iterations; i++) {
        X86_VPROF_BEGIN(cal);
        X86_VPROF_END(cal, osk);
    }
    x86_vprof_ovh = (X86_VPROF_CYCCNT - t0) / iterations;
    X86_VPROF_ZERO(osk);        /* the calibration is not an OSK measurement */
}

void x86_vprof_frame_boundary(void)
{
    if (x86_vprof_reset) {
        x86_vprof_reset = 0;
        x86_vprof_clear();
    }
}

#endif /* X86_VPROF */
