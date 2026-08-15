# emu8950, vendored

`emu8950.c` / `emu8950.h` / `slot_render.h`, copied **byte for byte** from the
owner's own G&W Doom port:

    ~/Nerd/git/gnw-doom/gnw-doom/rp2040-doom/opl/

Upstream is <https://github.com/digital-sound-antiques/emu8950> (Mitsutaka
Okazaki, MIT), by way of Graham Sanderson's rp2040-doom and then the owner's
G&W port. **There are no local modifications.** Everything this core needs to
change about it is a `-D`, which is the point: a vendored file with a patch on
top stops being the thing that was tested on the hardware.

## Why this core and not a hand-reduced Nuked-OPL3

Measured, both compiled `-O2`, same nine-voice patch, same 49,716 Hz, same
994,320 chip samples (20 s of audio):

| | 9 channels | 5 channels (a real tune) | RAM_EMU |
|---|---|---|---|
| emu8950 | 36.5 ns/sample | 26.6 ns/sample | 22,951 B |
| hand-reduced Nuked-OPL3 | 88.0 ns/sample | 48.8 ns/sample | 4,679 B |
| | **emu8950 2.41x faster** | **1.84x faster** | **+18,272 B** |

It is also the **YM3812 natively**, which is the chip `x86_io.c` reports, and it
is proven on this exact part with retro-go. See `docs/03` §4f for the decision
and what the extra RAM cost.

## The build configuration IS the valuable part

Taken from `gnw-doom/Makefile` (lines 133-192) and set in `Makefile.core`:

    -DUSE_EMU8950_OPL=1     the file is entirely inside this guard
    -DDOOMX=1               a single STATIC OPL instance -- no calloc, and this
                            core has no malloc to give it
    -DEMU8950_LINEAR=1      the doomgeneric-proven render path
    -DEMU8950_LINEAR_SKIP=1 skip slots whose envelope has finished
    -DEMU8950_NO_RATECONV=1 no internal resampler: run at clk/72 and resample
                            outside (see the note below, it matters)
    -DEMU8950_NO_TLL=1      drops tll_table -- 8*16 x 64 x 4 uint32 = 128 KB
    -DEMU8950_NO_TIMER=1    x86_io.c owns the timers and the status byte
    -DEMU8950_NO_TEST_FLAG=1
    -DEMU8950_NO_PERCUSSION_MODE=1

and the compiler needs `-std=gnu11 -fms-extensions`: `OPL_SLOT` embeds
`struct SLOT_RENDER` anonymously, which is an MS/plan9 extension and a hard
error in plain C.

**`EMU8950_ASM` and `EMU8950_SLOT_RENDER` are deliberately NOT set**, following
the owner: they are armv6-m assembly and an alternative renderer that is not
bit-exact with the default path. `EMU8950_NO_WAVE_TABLE_MAP` is likewise not
set -- it is coupled to `SLOT_RENDER` and does not compile on its own, so
`wave_table_map` (8,192 B, the largest single item here) is not optional.

## The hardware-validated detail: DO NOT resample with nearest-neighbour

`src/gnw/opl_gnw.c` records it in one line and it cost somebody real time:

> nearest-neighbour resampling produced an audible ~1.7 kHz buzz on held notes

so 49,716 Hz -> 48,000 Hz is **16.16 LINEAR INTERPOLATION**, carried across
frame boundaries, in `x86_opl.c`. The first version of this core used a
zero-order hold -- i.e. exactly the scheme that buzzes -- because the Sound
Blaster path next door uses one. The SB path is resampling a 6-11 kHz sample
stream and gets away with it; a held FM note does not.
