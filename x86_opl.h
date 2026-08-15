/*
 * x86_opl.h -- YM3812 (OPL2) FM TONE GENERATION.
 *
 * The synthesis is emu8950 (lib/emu8950/, vendored unmodified from the owner's
 * G&W Doom port); x86_opl.c is the ADAPTER and carries the reasoning and the
 * measurements. The register file, the TIMERS and the detection answer stay in
 * x86_io.c and are not duplicated: its opl_write() forwards every accepted
 * (index, value) pair to x86_opl_write() and keeps the status byte, the sticky
 * timer flags and the ISA bus charge exactly where they were.
 *
 * WHAT IT IS, AND WHAT IT IS NOT. Nine channels, two operators each, four
 * waveforms, the ADSR envelope generator, key scaling of both level and rate,
 * feedback on operator 1 and the two connection modes -- i.e. everything an
 * AdLib melodic track uses. It is an OPL2 and it says so: x86_io.c reports
 * 0x06 in the status low bits (the YM3812 answer) and nothing here implements
 * an OPL3 register. PERCUSSION (register BDh bit 5) is NOT synthesised; see
 * the note on x86_opl_rhythm_muted below for what happens instead and why.
 *
 * There is no per-instruction cost. Like the DMA engine, the only clock is the
 * frame loop: x86_opl_render_mix() is called once per rendered frame and the
 * chip advances by exactly the samples that call produced.
 */
#ifndef X86_OPL_H
#define X86_OPL_H

#include <stdint.h>

/* The YM3812's own sample clock: 14.31818 MHz / 288. Every rate constant in
   the envelope generator is expressed against this and not against our output
   rate, which is why the resampler below exists at all. */
#define X86_OPL_CLOCK_HZ 49716u

/* Power-on state. Safe to call at any time; kills every voice. */
void x86_opl_reset(void);

/* One accepted register write. `reg` is the index latch, `val` the data byte.
   Cheap: this is on the guest's OUT path and does no synthesis. */
void x86_opl_write(uint8_t reg, uint8_t val);

/* Non-zero when at least one channel could produce a sample. The render call
   early-outs on this, so a title with no music pays nothing. */
int x86_opl_active(void);

/*
 * Render `frames` output samples at `sample_rate` and MIX them into `dst`,
 * which already holds the speaker and (after this) the Sound Blaster. Returns
 * the number of samples contributed.
 *
 * `amplitude` is a 1.15 gain applied as (sample * amplitude) >> 14 -- the
 * "music * 2" balance the owner's own G&W port settled on (opl_gnw.c) against
 * a real mix of music and digitised sound, taken rather than re-derived. One
 * operator at total level 0 lands near 1,012 of full scale and a nine-voice
 * chord near 8,000, against the ~4,080 a full-scale Sound Blaster sample
 * reaches at maximum volume -- so music sits under a sound effect but is not
 * buried by it. Clipping IS possible and the mix is CLAMPED rather than
 * allowed to wrap, because a wrap is a loud click and a clamp is not;
 * x86_opl_clips counts it.
 */
unsigned x86_opl_render_mix(int16_t *dst, unsigned frames,
                            unsigned sample_rate, int16_t amplitude);

/* Published for tests/ and probe.py --watch. */
extern volatile uint32_t x86_opl_samples;     /* output samples contributed  */
extern volatile uint32_t x86_opl_keyons;      /* key-on edges seen           */
extern volatile uint32_t x86_opl_chip_steps;  /* chip samples generated      */
extern volatile uint32_t x86_opl_slots_run;   /* per-sample slot updates     */
extern volatile uint32_t x86_opl_clips;       /* samples the mix had to clamp*/
/* Channels 6-8 silenced because the guest asked for PERCUSSION MODE, which is
   not synthesised. A non-zero here is the honest signal that a title wanted
   drums and got nothing -- as against getting three melodic voices playing the
   rhythm registers as if they were notes, which is what ignoring BDh bit 5
   would produce and which sounds like a fault rather than an omission. */
extern volatile uint32_t x86_opl_rhythm_muted;

/*
 * MEASUREMENT ARM (X86_OPL_BENCH=1, never shipped at 1). Keys all NINE
 * channels on with a sustaining full-level patch, so the render does the
 * chip's WORST CASE -- 18 operator updates every chip sample -- for as long as
 * the core runs. Paired with x86_opl_frame_cycles in src/main.c it is how the
 * per-frame cost is measured ON SILICON in the REAL core: the bare-metal
 * harness in tests/main.c runs its C from internal flash with the caches off
 * and over-reports this by an order of magnitude, so its DWT number is a
 * harness number and must never be quoted as the core's.
 */
void x86_opl_bench_all_on(void);

/* Cycles the LAST frame's x86_opl_render_mix() took, published by src/main.c
   when X86_OPL_BENCH=1. Read it with probe.py --watch. */
extern volatile uint32_t x86_opl_frame_cycles;

#endif /* X86_OPL_H */
