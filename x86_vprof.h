/*
 * x86_vprof.h -- VIDEO-PATH cycle attribution. OFF unless X86_VPROF=1.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS, WHEN x86_zones.h AND x86_profile.h ALREADY DO
 * ---------------------------------------------------------------------------
 * Neither of them can answer "where do the cycles go in Mode Y".
 *
 *   x86_zones.h is per-FRAME and per-SERVICE-CALL. Its VIDEO zone brackets the
 *   frame loop's render -- but a Mode Y title paints itself from
 *   x86_video_page_flip(), which is called from INSIDE the interpreter, so two
 *   full 64,000-byte paints per present are charged to the GUEST zone and are
 *   invisible in the breakdown. That is precisely the cost under suspicion.
 *
 *   x86_profile.h charges cycles to the x86 OPCODE. A guest `stosw` into the
 *   Mode Y aperture and a `stosw` into ordinary RAM are the same opcode; the
 *   first costs the planar hook and the second does not, and the per-opcode
 *   table averages them into one meaningless mean.
 *
 * So this instrument brackets the FOUR video phases by name, with the DWT
 * cycle counter, and adds a whole-frame accumulator so every share has a
 * MEASURED denominator rather than an assumed core clock:
 *
 *   FRAME   one iteration of the emulator frame loop           (denominator)
 *   STORE   x86_vga_planar_store(), the planar write hook      (inside GUEST)
 *   PAINT   modey_paint_both() / one non-Mode-Y render+OSK     (both callers)
 *   RENDER  x86_video_render() alone -- the de-interleave blit (inside PAINT)
 *   OSK     x86_osk_draw() alone -- the overlay pass           (inside PAINT)
 *
 * PAINT is a PARENT of RENDER and OSK; the three are deliberately NOT
 * exclusive, because `PAINT - (RENDER + OSK)` is itself a wanted number (the
 * buffer-address and mode-check work around the two paints). Everything else
 * is exclusive. STORE is disjoint from PAINT.
 *
 * ---------------------------------------------------------------------------
 * COST, AND WHY IT IS STATED RATHER THAN SUBTRACTED
 * ---------------------------------------------------------------------------
 * A bracket is two reads of 0xE0001004 (the private peripheral bus: not
 * cached, not free), one subtract and one 64-bit accumulate. PAINT/RENDER/OSK/
 * FRAME fire tens of times a second and are noise. STORE fires ~180,000 times
 * a second in Mode Y gameplay and is NOT noise -- it is the one bracket that
 * perturbs its own measurement.
 *
 * x86_vprof_calibrate() measures an empty bracket pair on the target, in the
 * same inline code, and stores it in x86_vprof_ovh. NOTHING here subtracts it.
 * The host report prints the raw total, the corrected total
 * (total - n * ovh) and the calibration itself, and the corrected figure is a
 * LOWER BOUND: the calibration loop stays in cache and predicts perfectly.
 *
 * Grade the perturbation, do not assume it: guest_ips_achieved on this build
 * against the same workload on a clean build is the honest check, and the
 * report is required to quote both.
 *
 * ---------------------------------------------------------------------------
 * WRAPPING
 * ---------------------------------------------------------------------------
 * CYCCNT is 32-bit and wraps every ~15 s at 280 MHz, so a DELTA is taken (and
 * is exact across a wrap by modular arithmetic) and accumulated 64-bit. A
 * 32-bit accumulator would wrap inside one measurement window.
 *
 * Storage is plain BSS, not DTCM: these are read by the host with
 * probe.py --watch and the DTCM pool is the scarcest thing in the project
 * (STATUS.md). The accumulators are hot in the D-cache and the bracket's cost
 * is dominated by the PPB reads either way.
 */
#ifndef X86_VPROF_H
#define X86_VPROF_H

#include <stdint.h>

#ifndef X86_VPROF
#define X86_VPROF 0
#endif

/*
 * Host-visible even when the instrument is compiled out, so probe.py --watch
 * resolves every symbol on any build and reads a truthful zero on an
 * uninstrumented one. A host tool that cannot tell "instrument off" from
 * "symbol missing" is how a measurement gets silently skipped.
 *
 * FOUR SEPARATE SYMBOLS PER PHASE, not a struct, and that is the whole reason
 * this shape was chosen: probe.py's --watch takes a symbol NAME and reads one
 * word at it, so a struct is only readable with --dump, which prints once at
 * the END of a run. Differencing an accumulator across a measurement WINDOW --
 * which is the only way to profile gameplay rather than gameplay plus the
 * menus that preceded it -- needs each field to be its own watchable symbol.
 *
 *   _cyc    low 32 bits of the 64-bit self-time
 *   _hi     high 32 bits. CYCCNT wraps every ~15 s at 280 MHz, and a delta
 *           differenced across samples is exact modulo 2^32 as long as the
 *           sample interval keeps it under that
 *   _n      occurrences
 *   _worst  worst SINGLE occurrence. Stutter is a tail property and a mean
 *           cannot contain one -- the same reason x86_zones.h carries one
 */
#define X86_VPROF_DECL(name)                    \
    extern volatile uint32_t x86_vprof_##name##_cyc;   \
    extern volatile uint32_t x86_vprof_##name##_hi;    \
    extern volatile uint32_t x86_vprof_##name##_n;     \
    extern volatile uint32_t x86_vprof_##name##_worst

X86_VPROF_DECL(frame);
X86_VPROF_DECL(store);
X86_VPROF_DECL(paint);
X86_VPROF_DECL(render);
X86_VPROF_DECL(osk);
X86_VPROF_DECL(cell);

/* NOT video, and here anyway because this is the instrument that already has
   64-bit accumulators, a calibrated bracket and a host-side window control.
   Both are per-GUEST-INSTRUCTION costs that no other instrument can see: the
   opcode profiler charges them to whichever opcode happened to pay, and the
   zone profiler is per-frame.

   RELOC  x86_code_reloc() -- staging an instruction that straddles a granule
          into DTCM. ~5% of instructions in Mode Y gameplay were staged and
          the COST of one had never been measured, only the rate.
   SEAM   x86_seam_sync() -- the multi-byte-access straddle prologue. Fires on
          every SEAM_CHECK that trips, which is a false positive most of the
          time (offset 0xFFC with a 4-byte operand does not really straddle),
          so its mean is dominated by the call that finds nothing to do. */
/* Guarded, unlike the five above. Those are unconditional so a host tool can
   read a truthful zero off any build; these two would ADD BSS to the shipping
   core, and adding BSS shifts the link, which shifts address literals, which
   makes the shipping dos.bin differ from master by 303 scattered bytes. A
   profiling flag must not change the shipping binary at all, so on a clean
   build these symbols simply do not exist and probe.py fails to resolve them
   -- which is the honest signal that the instrument is not compiled in. */
#if X86_VPROF
X86_VPROF_DECL(reloc);
X86_VPROF_DECL(seam);
#endif

/* Paints reached from the guest's own page flip vs from the frame loop's
   fallback. x86_modey_presents counts only the first, which is why "10 frames
   per second" has never been the whole paint count. */
extern volatile uint32_t x86_vprof_paint_flip;
extern volatile uint32_t x86_vprof_paint_loop;
extern volatile uint32_t x86_vprof_ovh;    /* calibrated cycles per bracket */
extern volatile uint32_t x86_vprof_reset;  /* host writes 1; cleared at frame */

#if X86_VPROF

#define X86_VPROF_CYCCNT (*(volatile uint32_t *)0xE0001004u)

#define X86_VPROF_BEGIN(name)  uint32_t vprof_t_##name = X86_VPROF_CYCCNT
#define X86_VPROF_END(name, acc)                                        \
    do {                                                                \
        uint32_t vprof_d = X86_VPROF_CYCCNT - vprof_t_##name;           \
        uint32_t vprof_lo = x86_vprof_##acc##_cyc + vprof_d;            \
        if (vprof_lo < x86_vprof_##acc##_cyc)                           \
            x86_vprof_##acc##_hi++;                                     \
        x86_vprof_##acc##_cyc = vprof_lo;                               \
        x86_vprof_##acc##_n++;                                          \
        if (vprof_d > x86_vprof_##acc##_worst)                          \
            x86_vprof_##acc##_worst = vprof_d;                          \
    } while (0)
#define X86_VPROF_COUNT(c)     ((c)++)

void x86_vprof_calibrate(uint32_t iterations);
void x86_vprof_clear(void);
/* Call once per frame, at the frame boundary. Honours x86_vprof_reset so a
   host can open a measurement window without racing a half-open bracket. */
void x86_vprof_frame_boundary(void);

#else

#define X86_VPROF_BEGIN(name)      ((void)0)
#define X86_VPROF_END(name, acc)   ((void)0)
#define X86_VPROF_COUNT(c)         ((void)0)
#define x86_vprof_calibrate(n)     ((void)0)
#define x86_vprof_clear()          ((void)0)
#define x86_vprof_frame_boundary() ((void)0)

#endif /* X86_VPROF */

#endif /* X86_VPROF_H */
