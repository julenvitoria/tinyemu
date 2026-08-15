/* Host unit test for the PC-speaker square-wave generator (x86_audio_wave.h).
 *
 *   cc -O2 -Wall -Wextra -I.. -o host_audio host_audio.c -lm && ./host_audio
 *
 * or `python3 tests/test_audio.py`, which does exactly that.
 *
 * Ported from the author's earlier DOS core
 * (external/8086tiny/test286/host_audio.c in game-and-watch-retro-go-sd).
 *
 * Why this exists: the generator is pure arithmetic (PIT divisor -> frequency
 * -> samples), and a wrong constant there produces audio that is present, plays
 * at the right time, and is simply in the wrong key -- invisible in a
 * screenshot, unjudgeable under gwemu, and impossible to bisect on silicon.
 * So the relationship between divisor, sample rate and output period is
 * MEASURED here rather than assumed.
 *
 * It also carries the implementation this core USED to have, in `old_hz()`,
 * and prints its error next to the new one. That is the answer to "why does it
 * sound tinny", kept in the tree rather than in a commit message.
 */

#include "x86_audio_wave.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* The rate the core now runs at. The old core ran at 16000; OLD_RATE is what
 * the regression numbers below are measured against. */
#define RATE      48000u
#define OLD_RATE  16000u

static int failures;

static void check(int cond, const char *what)
{
    if (!cond) { printf("FAIL: %s\n", what); failures++; }
    else       { printf("ok:   %s\n", what); }
}

/* Measure the realised frequency by counting sign changes over ten seconds of
 * generated audio. This is the end-to-end number that matters: it goes through
 * the same phase accumulator the firmware runs, so it catches a wrong constant,
 * a wrong shift and an off-by-one in the top-bit test alike. */
static double measured_hz(uint32_t divisor, uint32_t rate)
{
    const uint32_t n = rate * 10u;      /* one edge of quantisation is then
                                           0.05 Hz, well under the 1-cent bound */
    int16_t *buf = malloc(n * sizeof(int16_t));
    uint32_t phase = 0;
    uint32_t inc = x86_spkr_phase_inc(divisor, rate);
    x86_spkr_render(buf, n, inc, 1000, &phase);

    unsigned edges = 0;
    for (uint32_t i = 1; i < n; i++)
        if ((buf[i] > 0) != (buf[i - 1] > 0)) edges++;
    free(buf);
    return edges / 2.0 / 10.0;          /* two sign changes per cycle */
}

/* THE IMPLEMENTATION THIS REPLACED, reproduced exactly (x86_io.c before this
 * change): an integer frequency, then a 16-bit accumulator whose step is
 * derived from it. Two truncations compound, and there is no Nyquist guard --
 * a step of 65536 or more simply wraps into an unrelated pitch. */
static double old_measured_hz(uint32_t divisor, uint32_t rate)
{
    uint32_t hz = X86_PIT_HZ / (divisor ? divisor : 65536u);
    uint32_t step = (uint32_t)(((uint64_t)hz << 16) / rate);
    if (step == 0) return 0.0;
    const uint32_t n = rate * 10u;
    uint32_t phase = 0;
    unsigned edges = 0; int prev = -1;
    for (uint32_t i = 0; i < n; i++) {
        int s = (phase & 0x8000u) ? 1 : 0;
        if (prev >= 0 && s != prev) edges++;
        prev = s;
        phase = (phase + step) & 0xFFFFu;
    }
    return edges / 2.0 / 10.0;
}

static double cents(double got, double want)
{
    if (got <= 0.0) return -9999.0;
    return 1200.0 * log2(got / want);
}

/* Duty cycle, as a fraction. A PC speaker in PIT mode 3 is a 50% square. */
static double measured_duty(uint32_t divisor)
{
    const uint32_t n = RATE;
    int16_t *buf = malloc(n * sizeof(int16_t));
    uint32_t phase = 0;
    x86_spkr_render(buf, n, x86_spkr_phase_inc(divisor, RATE), 1000, &phase);
    unsigned high = 0;
    for (uint32_t i = 0; i < n; i++) if (buf[i] > 0) high++;
    free(buf);
    return (double)high / n;
}

/* Divisors taken from real software. 1193 is the canonical ~1 kHz BIOS beep;
 * 2394 is BASIC's PLAY-middle-A region; 4560 is middle C; 543 is the high end
 * of what beep routines use; 9121 is two octaves below middle C, where the old
 * double truncation was worst. */
static const struct { uint32_t div; const char *what; } CASES[] = {
    {  543u, "high beep"     },
    { 1193u, "BIOS 1 kHz"    },
    { 1331u, "A#5-ish"       },
    { 2394u, "~498 Hz"       },
    { 4560u, "middle C"      },
    { 9121u, "C3"            },
    {18242u, "C2"            },
};
#define NCASES (sizeof CASES / sizeof CASES[0])

int main(void)
{
    char msg[256];

    printf("-- constants --\n");
    /* 2^32 * 1193182 / 48000. Spelled out so a change to either constant that
     * silently retunes the whole core has to come past this line. */
    check(X86_PHASE_NUM(RATE) == 106764118086ULL,
          "X86_PHASE_NUM(48000) == 106764118086 (2^32 * 1193182 / 48000)");
    /* The upstream-constant trap, stated as a test: anyone hardcoding one rate
     * breaks the other. 8086tiny's `54` is 2*1193182/44100. */
    check(X86_PHASE_NUM(44100) != X86_PHASE_NUM(48000),
          "phase numerator is rate-dependent (a baked constant would not be)");
    check(X86_SPKR_MIN_DIVISOR(RATE) == 49,
          "Nyquist floor at 48 kHz is divisor 49");
    check(X86_SPKR_MIN_DIVISOR(OLD_RATE) == 149,
          "Nyquist floor at 16 kHz was divisor 149 (nothing enforced it)");

    printf("\n-- divisor -> frequency: NEW at 48 kHz vs OLD at 16 kHz --\n");
    printf("    %-6s %-12s %10s %8s %10s %8s\n",
           "div", "what", "new Hz", "cents", "old Hz", "cents");
    double worst_new = 0.0, worst_old = 0.0;
    for (unsigned i = 0; i < NCASES; i++) {
        double want = (double)X86_PIT_HZ / CASES[i].div;
        double gnew = measured_hz(CASES[i].div, RATE);
        double gold = old_measured_hz(CASES[i].div, OLD_RATE);
        double cnew = cents(gnew, want), cold = cents(gold, want);
        printf("    %-6u %-12s %10.2f %+8.2f %10.2f %+8.2f\n",
               CASES[i].div, CASES[i].what, gnew, cnew, gold, cold);
        if (fabs(cnew) > worst_new) worst_new = fabs(cnew);
        if (fabs(cold) > worst_old) worst_old = fabs(cold);

        /* One cent is far below the threshold of hearing (~5 cents), and the
         * ten-second window resolves 0.05 Hz, so this bound is measurement-
         * limited rather than arbitrary. */
        snprintf(msg, sizeof msg, "divisor %u (%s) within 1 cent",
                 CASES[i].div, CASES[i].what);
        check(fabs(cnew) < 1.0, msg);

        /* x86_spkr_freq_hz() is the reporting helper; it must agree with the
         * generator or probe.py and the logs will lie about what is playing. */
        snprintf(msg, sizeof msg, "x86_spkr_freq_hz(%u) agrees within 1 Hz",
                 CASES[i].div);
        check(fabs((double)x86_spkr_freq_hz(CASES[i].div) - want) <= 1.0, msg);
    }
    printf("    worst |error|: new %.2f cents, old %.2f cents\n",
           worst_new, worst_old);
    check(worst_old > 5.0,
          "the old maths WAS audibly off (>5 cents somewhere) -- regression recorded");

    printf("\n-- Nyquist guard --\n");
    /* Without the guard the accumulator folds an above-Nyquist tone down to an
     * arbitrary audible pitch, which is loud and in no key at all. */
    check(x86_spkr_phase_inc(0, RATE) == 0,  "divisor 0 (gated off) is silence");
    check(x86_spkr_phase_inc(1, RATE) == 0,  "divisor 1 (1.19 MHz) is silence");
    check(x86_spkr_phase_inc(10, RATE) == 0, "divisor 10 (119 kHz) is silence");
    check(x86_spkr_phase_inc(48, RATE) == 0, "divisor 48 (24.9 kHz) is silence");
    check(x86_spkr_phase_inc(49, RATE) != 0, "divisor 49 (24.4 kHz) still sounds");
    {
        int16_t buf[256];
        uint32_t phase = 0;
        memset(buf, 0x5A, sizeof buf);
        x86_spkr_render(buf, 256, x86_spkr_phase_inc(10, RATE), 8160, &phase);
        int all_zero = 1;
        for (int i = 0; i < 256; i++) if (buf[i]) all_zero = 0;
        check(all_zero, "divisor 10 renders SILENCE, not an alias");
        /* And show what the old code did with it, for the record. */
        double alias = old_measured_hz(10u, OLD_RATE);
        snprintf(msg, sizeof msg,
                 "old code turned divisor 10 (119 kHz) into a %.0f Hz alias", alias);
        check(alias > 20.0 && alias < 8000.0, msg);
    }

    printf("\n-- anti-click: the rising edge always starts at the same sign --\n");
    {
        /* The caller zeroes the phase on a rising gate edge; the generator's
         * job is to make phase 0 mean a fixed, repeatable starting sign. Run it
         * from several arbitrary mid-beep phases and confirm the reset lands on
         * the same sample value every time. */
        uint32_t inc = x86_spkr_phase_inc(1193, RATE);
        const uint32_t stale[] = { 0u, 0x12345678u, 0x9ABCDEF0u, 0xFFFFFFFFu };
        int ok = 1;
        for (unsigned i = 0; i < 4; i++) {
            int16_t buf[8];
            uint32_t phase = stale[i];
            phase = 0;                        /* what spk_run() does on the edge */
            x86_spkr_render(buf, 8, inc, 8160, &phase);
            if (buf[0] != 8160) ok = 0;
        }
        check(ok, "first sample after a rising edge is +amp, every time");

        /* Without the reset the first sample follows whatever the phase was, so
         * this is a real property and not a tautology: prove the same stale
         * phases DO give different first samples when the reset is skipped. */
        int differed = 0;
        int16_t first = 0;
        for (unsigned i = 0; i < 4; i++) {
            int16_t buf[8];
            uint32_t phase = stale[i];
            x86_spkr_render(buf, 8, inc, 8160, &phase);
            if (i == 0) first = buf[0];
            else if (buf[0] != first) differed = 1;
        }
        check(differed, "without the reset the first sample varies (the click)");
    }

    printf("\n-- waveform shape --\n");
    check(fabs(measured_duty(1193) - 0.5) < 0.01, "duty cycle is 50% (mode 3 square)");
    {
        int16_t buf[64]; uint32_t phase = 0;
        x86_spkr_render(buf, 64, x86_spkr_phase_inc(1193, RATE), 8160, &phase);
        int seen_low = 0;
        for (int i = 0; i < 64; i++) if (buf[i] == -8160) seen_low = 1;
        check(seen_low, "wave swings symmetrically to -amp");
    }

    printf("\n-- silence cases --\n");
    {
        int16_t buf[16]; uint32_t phase = 0x12345678u;
        memset(buf, 0x5A, sizeof buf);
        x86_spkr_render(buf, 16, 0, 8160, &phase);
        int all_zero = 1;
        for (int i = 0; i < 16; i++) if (buf[i]) all_zero = 0;
        check(all_zero, "inc==0 writes silence over the buffer");
        check(phase == 0x12345678u, "inc==0 leaves phase untouched");
        x86_spkr_render(buf, 16, x86_spkr_phase_inc(1193, RATE), 0, &phase);
        all_zero = 1;
        for (int i = 0; i < 16; i++) if (buf[i]) all_zero = 0;
        check(all_zero, "amp==0 (volume at minimum) writes silence");
    }

    printf("\n-- phase continuity across buffers --\n");
    {
        /* A beep spans many buffer fills. If the phase were reset per fill the
         * wave would restart 60 times a second, which is a buzz at the frame
         * rate rather than the programmed tone. */
        uint32_t inc = x86_spkr_phase_inc(1193, RATE);
        uint32_t phase = 0;
        const uint32_t chunk = RATE / 60;            /* 800, the shipping fill */
        int16_t big[16000];
        const int nb = 10;
        for (int b = 0; b < nb; b++)
            x86_spkr_render(big + b * chunk, chunk, inc, 1000, &phase);
        unsigned edges = 0;
        for (uint32_t i = 1; i < chunk * nb; i++)
            if ((big[i] > 0) != (big[i - 1] > 0)) edges++;
        double hz = (edges / 2.0) * ((double)RATE / (chunk * nb));
        snprintf(msg, sizeof msg,
                 "%d x %u-sample fills stay at %.1f Hz (want 1000.2)",
                 nb, chunk, hz);
        check(fabs(hz - (double)X86_PIT_HZ / 1193) < 5.0, msg);
    }

    printf("\n-- the half-buffer the DMA consumes --\n");
    {
        /* The firmware's audiobuffer_dma is int16_t[1077*2] and NOTHING bounds-
         * checks a half-buffer length handed to HAL_SAI_Transmit_DMA. And a
         * half-buffer longer than one frame is refilled before the DMA has
         * played it, which is what made a correct tone come out rough. */
        check(RATE / 60 == 800 && 800 <= 1077, "48 kHz at 60 Hz is 800 <= 1077");
        check(OLD_RATE / 60 == 266, "16 kHz at 60 Hz would have been 266 ...");
        check(1077 > 3 * (OLD_RATE / 60),
              "... but the core used 1077, i.e. 4 frames of audio per refill");
    }

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
