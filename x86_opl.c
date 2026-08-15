/*
 * x86_opl.c -- YM3812 (OPL2) FM tone generation: the ADAPTER.
 *
 * The synthesis is emu8950 (lib/emu8950/, vendored unmodified from the owner's
 * G&W Doom port; its README-tinyemu.md carries the provenance, the build
 * configuration and the measurements that chose it). This file is the whole of
 * what THIS core adds: the register forward, the "is anything sounding"
 * early-out, the resampler, the mix, and the counters the tests read.
 *
 * ---------------------------------------------------------------------------
 * WHY emu8950 AND NOT THE HAND-REDUCED NUKED-OPL3 THAT WAS HERE FIRST
 *
 * The first version of this file was Nuked-OPL3 cut down to an OPL2 by hand.
 * It worked -- WOLF3D played its music at tier 2 with the golden at 0.000% --
 * and it was replaced anyway, on measurement. Both compiled -O2, same nine-
 * voice patch, same 49,716 Hz, 994,320 chip samples:
 *
 *   9 channels   emu8950 36.5 ns/sample   Nuked-derived 88.0   2.41x
 *   5 channels   emu8950 26.6 ns/sample   Nuked-derived 48.8   1.84x
 *
 * A 2.4x on the axis that made FM "does not fit" in the first place is not a
 * close call. Two more reasons, both about risk rather than speed: emu8950 IS
 * a YM3812, which is the chip x86_io.c reports, where the other was an OPL3
 * core with the OPL3 removed BY ME -- 600 lines of my own transcription to be
 * subtly wrong in; and emu8950 is already running on this exact part under
 * retro-go in the owner's Doom port, so its behaviour on this silicon is
 * evidence rather than a hope.
 *
 * IT COST RAM AND THE NUMBER IS NOT SMALL: 22,951 B against 4,679 B, +18,272 B
 * of RAM_EMU, which is FOUR guest page frames. docs/03 4f has the accounting
 * and the reason it was still judged worth paying. `X86_OPL_FM=0` gives every
 * byte of it back.
 *
 * ---------------------------------------------------------------------------
 * THE RESAMPLER IS LINEAR, AND THAT IS NOT A PREFERENCE
 *
 * 49,716 Hz (the YM3812's own clk/72) into 48,000 Hz, 16.16 accumulator,
 * fraction carried across frame boundaries, and the two bracketing chip
 * samples LINEARLY INTERPOLATED. Taken whole from the owner's opl_gnw.c, which
 * records what the cheap version sounds like:
 *
 *     "nearest-neighbour resampling produced an audible ~1.7 kHz buzz on held
 *      notes"
 *
 * This file's first version used a zero-order hold -- exactly that scheme --
 * because x86_sb_render_mix() next door uses one and it seemed consistent. It
 * is not the same problem: the SB path holds a 6-11 kHz sample stream where
 * the ear is forgiving, and a sustained FM note is where it is not. One
 * multiply-add per output sample, 800 per frame, in host C.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS MODELLED, AND WHAT IS NOT
 *
 * Nine channels, two operators each, four waveforms, the ADSR envelope, key
 * scaling of level and rate, vibrato and tremolo, feedback and both connection
 * modes -- everything an AdLib melodic track uses.
 *
 * PERCUSSION MODE (register BDh bit 5) is NOT synthesised: emu8950 is built
 * with EMU8950_NO_PERCUSSION_MODE, following the owner's Doom configuration.
 * When a guest turns it on, x86_opl_rhythm_muted counts it and the three
 * affected channels go quiet rather than playing the rhythm registers as if
 * they were notes. THE TIMERS AND THE STATUS BYTE ARE NOT emu8950's EITHER
 * (EMU8950_NO_TIMER): x86_io.c has owned them since the detection work and
 * still does -- two chips answering the same status port is how a detection
 * probe gets an answer that agrees with neither.
 *
 * There is no per-instruction cost. The only clock is the frame loop, exactly
 * as for the DMA engine: x86_opl_render_mix() is called once per rendered
 * frame from src/main.c's audio phase and the chip advances by the samples
 * that call produced.
 */

#include <string.h>
#include "x86_opl.h"
#include "emu8950.h"

/* Published counters. */
volatile uint32_t x86_opl_samples;
volatile uint32_t x86_opl_keyons;
volatile uint32_t x86_opl_chip_steps;
volatile uint32_t x86_opl_slots_run;
volatile uint32_t x86_opl_clips;
volatile uint32_t x86_opl_rhythm_muted;
volatile uint32_t x86_opl_frame_cycles;

/* The chip. OPL_new() returns the single static instance under -DDOOMX and
   allocates nothing -- see lib/emu8950/README-tinyemu.md, and note that this
   core has no malloc to give it if that flag ever comes off. */
static OPL *opl;

/* Chip samples are pulled one chunk at a time rather than one at a time:
   OPL_calc_buffer_linear() amortises its per-call setup over the chunk, which
   is most of what EMU8950_LINEAR buys. It does NOT have to hold a whole
   frame: 256 int32 is 1,024 B where a frame's 829 chip samples would be 3,316,
   and on this part a kilobyte of RAM_EMU is a quarter of a guest page frame.
   Four refills per frame amortise the per-call setup perfectly well; sizing
   this to the frame would buy a few hundred cycles and cost the guest most of
   a page. The resampler needs the chip samples either side of each output
   sample, which is what s0/s1 are -- they carry across the refill boundary so
   the chunking is inaudible. */
#define OPL_CHUNK 256u
static int32_t chunk[OPL_CHUNK];
static unsigned chunk_len, chunk_pos;

static uint32_t frac;          /* the 16.16 resampler accumulator */
static int16_t  s0, s1;        /* the two chip samples it interpolates between */
static uint8_t  inited;
static uint8_t  rhythm_on;

/* EG_MAX is emu8950's "this operator contributes nothing"; a slot at or above
   it is silent whatever its phase. Kept here rather than reaching into the
   core's private header, and asserted against it by the shape of the test:
   if this constant were wrong the idle-channel early-out would either never
   engage (x86_opl_slots_run == 2 x chip_steps, which tests/test_sound.py
   FAILS on) or engage while a note was still sounding (the pitch arm fails). */
#define OPL_EG_MAX 0x1FF

void x86_opl_reset(void)
{
    if (!opl)
        opl = OPL_new(X86_OPL_CLOCK_HZ * 72u, X86_OPL_CLOCK_HZ);
    if (opl)
        OPL_reset(opl);
    chunk_len = chunk_pos = 0;
    frac = 0;
    s0 = s1 = 0;
    rhythm_on = 0;
    inited = 1;
}

/* How many operators could sound. Also the render's early-out: a title that
   never writes an FM register pays this and nothing else. */
static unsigned opl_live_slots(void)
{
    unsigned live = 0;
    if (!opl)
        return 0;
    for (unsigned i = 0; i < 18; i++)
        if (opl->slot[i].eg_out < OPL_EG_MAX)
            live++;
    return live;
}

int x86_opl_active(void)
{
    return opl && (opl->slot_key_status || opl_live_slots());
}

void x86_opl_write(uint8_t reg, uint8_t val)
{
    /* BSS zero is not this chip's reset state and x86_io.c has no init entry
       point to hang a call off. One predictable-not-taken branch on a path
       that runs a few hundred times a second; it must NOT become a pattern on
       the per-sample path. */
    if (!inited)
        x86_opl_reset();
    if (!opl)
        return;

    /* Register 4 is the TIMER control and register 2/3 the presets: x86_io.c
       owns those and emu8950 is built without them. Forwarding them anyway
       would be harmless today and a second opinion on the status byte the day
       EMU8950_NO_TIMER comes off, so they stop here. */
    if (reg == 0x02u || reg == 0x03u || reg == 0x04u)
        return;

    if (reg == 0xBDu) {
        uint8_t want = (uint8_t)((val >> 5) & 1u);
        if (want && !rhythm_on)
            x86_opl_rhythm_muted++;   /* asked for drums; see the header */
        rhythm_on = want;
    }

    if ((reg & 0xF0u) == 0xB0u && (reg & 0x0Fu) < 9u) {
        unsigned ci = reg & 0x0Fu;
        uint8_t was = (uint8_t)((opl->reg[0xB0u + ci] >> 5) & 1u);
        if (((val >> 5) & 1u) && !was)
            x86_opl_keyons++;
    }

    OPL_writeReg(opl, reg, val);
}

/* One chunk of chip samples at 49,716 Hz. */
static void opl_refill(void)
{
    unsigned n = OPL_CHUNK;
    unsigned live = opl_live_slots();
    OPL_calc_buffer_linear(opl, chunk, n);
    chunk_len = n;
    chunk_pos = 0;
    x86_opl_chip_steps += n;
    /* What the render ACTUALLY did, so the idle-channel skip is observed and
       not assumed. emu8950's EMU8950_LINEAR_SKIP drops finished slots inside
       the loop; this is that count, sampled at the chunk boundary. */
    x86_opl_slots_run += live * n;
}

unsigned x86_opl_render_mix(int16_t *dst, unsigned frames,
                            unsigned sample_rate, int16_t amplitude)
{
    uint32_t step;

    if (!opl || !frames || !sample_rate || !dst || !x86_opl_active()) {
        s0 = s1 = 0;               /* never hold a DC offset over silence */
        return 0;
    }

    step = (uint32_t)(((uint64_t)X86_OPL_CLOCK_HZ << 16) / sample_rate);
    if (!step)
        step = 1;

    for (unsigned i = 0; i < frames; i++) {
        int32_t v, o;

        frac += step;
        while (frac >= 0x10000u) {
            int32_t s;
            frac -= 0x10000u;
            s0 = s1;
            if (chunk_pos >= chunk_len)
                opl_refill();
            s = chunk[chunk_pos++];        /* the sum of up to nine voices */
            if (s > 32767) s = 32767;
            else if (s < -32768) s = -32768;
            s1 = (int16_t)s;
        }
        /* LINEAR, not a hold. See the header: the hold buzzes. */
        v = s0 + (((int32_t)(s1 - s0) * (int32_t)(frac >> 8)) >> 8);
        /* THE MUSIC GAIN IS THE OWNER'S: opl_gnw.c keeps emu8950's raw level
           (no upstream << 3) and "the mixer applies the music*2 balance gain",
           which is this >> 14 against a 1.15 amplitude. Chosen there against a
           real mix of Doom's music and its digitised sound, which is exactly
           the balance question here, so it is taken rather than re-derived.
           One operator at total level 0 lands at ~1,012 of full scale, a
           nine-voice chord at ~8,000, against the ~4,080 a full-scale Sound
           Blaster sample reaches at maximum volume. */
        v = (v * amplitude) >> 14;

        o = (int32_t)dst[i] + v;
        if (o > 32767) { o = 32767; x86_opl_clips++; }
        else if (o < -32768) { o = -32768; x86_opl_clips++; }
        dst[i] = (int16_t)o;
    }
    x86_opl_samples += frames;
    return frames;
}

/* ---------------------------------------------------------------------------
 * MEASUREMENT ARM (X86_OPL_BENCH=1, never shipped at 1). See x86_opl.h.
 */
void x86_opl_bench_all_on(void)
{
    x86_opl_reset();
    x86_opl_write(0x01, 0x20);
    for (unsigned c = 0; c < 9; c++) {
        uint8_t o0 = (uint8_t)((c / 3) * 8 + (c % 3));
        for (unsigned k = 0; k < 2; k++) {
            uint8_t o = (uint8_t)(o0 + 3 * k);
            x86_opl_write((uint8_t)(0x20 + o), 0x21);  /* sustaining, mult 1 */
            x86_opl_write((uint8_t)(0x40 + o), 0x00);  /* total level 0      */
            x86_opl_write((uint8_t)(0x60 + o), 0xF0);  /* AR 15, DR 0        */
            x86_opl_write((uint8_t)(0x80 + o), 0x00);  /* SL 0, RR 0         */
        }
        x86_opl_write((uint8_t)(0xC0 + c), 0x01);      /* AM: both heard     */
        x86_opl_write((uint8_t)(0xA0 + c), (uint8_t)(0x40 + c * 8));
        x86_opl_write((uint8_t)(0xB0 + c), (uint8_t)(0x20 | (4 << 2) | 1));
    }
}
