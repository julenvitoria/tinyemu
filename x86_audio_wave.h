#ifndef _X86_AUDIO_WAVE_H_
#define _X86_AUDIO_WAVE_H_

/* PC-speaker square-wave generation -- the pure arithmetic half of x86_io.c.
 *
 * Ported from the author's earlier 8086tiny DOS core
 * (Core/Src/porting/dos/dos_audio_wave.h in game-and-watch-retro-go-sd), whose
 * design notes are docs/audio/01-pc-speaker.md and 02-output-bridge.md there.
 * VENDORED, not referenced: references/ is gitignored, so this file is the
 * copy the build depends on.
 *
 * Split into its own header for one reason: it is the only part of the audio
 * path that can be tested without hardware. It depends on nothing but
 * <stdint.h> -- no firmware headers, no globals, no x86_cpu.h -- so
 * tests/host_audio.c compiles it natively and checks real PIT divisors against
 * known frequencies. KEEP IT THAT WAY; the moment it includes x86_cpu.h the
 * only test that can catch a silently-retuned core stops building.
 *
 * What it replaced, and why (all measured by tests/host_audio.c):
 *
 *   - A 16-bit phase accumulator whose increment was derived from an ALREADY
 *     TRUNCATED integer frequency: hz = 1193182 / divisor, then
 *     step = hz * 65536 / rate. Two floors compound, and the error grows as the
 *     tone falls -- middle C came out 4.5 cents flat and the octave below it
 *     12.5 cents. Here the divisor goes into a 32-bit accumulator directly and
 *     no integer frequency exists in the signal path at all.
 *   - No Nyquist guard. At 16 kHz output every divisor below 149 (above 8 kHz)
 *     folded back down as a loud, tuneless alias, and a divisor small enough to
 *     make the 16-bit step exceed 0xFFFF wrapped into an unrelated pitch.
 */

#include <stdint.h>

/* PIT input clock. 14.31818 MHz / 12, the number every PC tone table divides. */
#define X86_PIT_HZ 1193182u

/* Numerator of the phase increment, in 32-bit-fraction-per-sample units:
 *
 *     inc = 2^32 * f_hz / rate,   f_hz = X86_PIT_HZ / divisor
 *         = (2^32 * X86_PIT_HZ / rate) / divisor
 *
 * The sample rate is a visible argument, never a magic constant. Upstream
 * 8086tiny's `54 * counter / divisor` is 2*1193182/44100 rounded -- baked to
 * SDL's 44.1 kHz, and ~1.5 semitones sharp anywhere else.
 *
 * The 64-bit divide runs once per constant-frequency RUN (a handful of times a
 * second), not per sample.
 */
#define X86_PHASE_NUM(rate) (((uint64_t)X86_PIT_HZ << 32) / (uint64_t)(rate))

/* Below this divisor the tone is above Nyquist and the accumulator would fold
 * it back down as a loud, tuneless alias. Real speakers could not reproduce
 * these either, so they are silence. 2*1193182/48000 = 49; at 16000, 149. */
#define X86_SPKR_MIN_DIVISOR(rate) (uint32_t)((2u * X86_PIT_HZ) / (uint32_t)(rate))

/* Phase increment for one PIT divisor, or 0 for "no tone".
 * divisor 0 means the speaker is gated off / channel 2 never programmed.
 * A guest reload of 0 means the full 16-bit period and must be passed as
 * 65536, which is why this takes a uint32_t. */
static inline uint32_t x86_spkr_phase_inc(uint32_t divisor, uint32_t rate)
{
    if (rate == 0 || divisor == 0 || divisor < X86_SPKR_MIN_DIVISOR(rate))
        return 0;
    return (uint32_t)(X86_PHASE_NUM(rate) / (uint64_t)divisor);
}

/* The frequency a divisor represents, for tests and logging only -- never in
 * the signal path, which is the whole point of the rewrite. */
static inline uint32_t x86_spkr_freq_hz(uint32_t divisor)
{
    return divisor ? (X86_PIT_HZ + divisor / 2) / divisor : 0;
}

/* Fill `len` samples of a 50% square wave, advancing *phase.
 *
 * Sign comes from the top bit of the accumulator, so the wave starts HIGH at
 * phase 0 -- which is what makes the anti-click reset work: the caller zeroes
 * *phase on every rising edge of the speaker gate, so a beep always begins at
 * the same point of the cycle instead of mid-swing. That is option (2) from
 * 02-output-bridge.md; no amplitude ramp, because real PC speakers clicked.
 *
 * `amp` is the peak magnitude, already attenuated by the caller.
 */
static inline void x86_spkr_render(int16_t *buf, uint32_t len,
                                   uint32_t inc, int16_t amp, uint32_t *phase)
{
    uint32_t p = *phase;
    if (inc == 0 || amp == 0) {
        for (uint32_t i = 0; i < len; i++)
            buf[i] = 0;
        return;                 /* phase deliberately left where it was */
    }
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = (p & 0x80000000u) ? (int16_t)-amp : amp;
        p += inc;
    }
    *phase = p;
}

#endif /* _X86_AUDIO_WAVE_H_ */
