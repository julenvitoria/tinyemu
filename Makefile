CC = arm-none-eabi-gcc
AS = arm-none-eabi-as
LD = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size

CFLAGS = -mcpu=cortex-m7 -mthumb -O2 -Wall -g -ffunction-sections -fdata-sections
CFLAGS += -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_GNU_SOURCE
CFLAGS += -DCONFIG_X86EMU -DCONFIG_VERSION=\"1.0\"

# The Retro-Go SD external-core SDK is VENDORED into this repo at sdk/, per the
# template's own model ("headers, bridge trampolines, linker scripts and packers
# are vendored under sdk/; you do not need a firmware checkout to compile").
#
# NOTHING may link against references/ -- that tree is temporary and strictly
# for reading. To pick up upstream retro-go changes, re-vendor:
#     scripts/sync_from_firmware.sh <path-to-game-and-watch-retro-go-sd>
# and update SDK_VERSION. Staying current is a core owner's responsibility.
SDK = sdk
SDK_INC = $(SDK)/include

# Tier-2 flags. Deliberately free of any retro-go/SDK include path: the
# bare-metal CPU harness (test_core.elf) depends only on lib/FatFs (our own
# submodule) and our own headers, which is what keeps it a clean, isolated
# measurement tier.
CFLAGS += -DIS_LITTLE_ENDIAN -Ilib/FatFs

# emu8950 (lib/emu8950/) -- the FM synthesiser behind x86_opl.c. The flag set
# is the owner's, transcribed from gnw-doom/Makefile and explained in
# lib/emu8950/README-tinyemu.md; Makefile.core carries the same list and the
# two MUST stay in step, because a mismatch changes struct layouts rather than
# just behaviour. -fms-extensions is not optional: OPL_SLOT embeds
# `struct SLOT_RENDER` anonymously, which is a hard error in plain C11.
CFLAGS += -Ilib/emu8950 -fms-extensions \
  -DUSE_EMU8950_OPL=1 -DDOOMX=1 -DEMU8950_LINEAR=1 -DEMU8950_LINEAR_SKIP=1 \
  -DEMU8950_NO_RATECONV=1 -DEMU8950_NO_TLL=1 -DEMU8950_NO_TIMER=1 \
  -DEMU8950_NO_TEST_FLAG=1 -DEMU8950_NO_PERCUSSION_MODE=1
# The pagefile slot ring, small. This harness's .bss is 176 bytes from
# overflowing RAM_EMU, and the ring's logic is size-independent -- a 32-slot
# ring wraps in tests/main.c's pgfile arm sooner, which is if anything better
# evidence than the shipping 512.
CFLAGS += -DX86_PGF_RING_SLOTS=32u -DX86_PGF_MAP_MAX=16u
# The bare-metal harness is NOT constrained by RAM_EMU -- it is its own image --
# so it always builds the anti-aliased text path, and tests/test_text.py checks
# both renderers. The packed core defaults it OFF; see Makefile.core.
CFLAGS += -DX86_TEXT_AA=1

# Per-opcode DWT cycle profiling. OFF by default: the instrumentation sits in
# the fetch/dispatch tail, the hottest code in the project, and perturbs every
# timing-dependent thing in the emulator. Flags are not dependencies -- rebuild
# from clean when this changes. See x86_profile.h and scripts/profile_opcodes.py.
# Negative and positive controls for the planar VGA arms of tests/test_video.py.
# Set in the ENVIRONMENT by scripts/run_planar_controls.py, one build each, e.g.
#   X86_VIDEO_NEGCTL=-DX86_MODEY_CORE_NEGCTL=1 make clean test_core.elf
# Flags are not dependencies, so every arm builds from CLEAN -- same caveat as
# X86_PROFILE below.
X86_VIDEO_NEGCTL ?=
CFLAGS += $(X86_VIDEO_NEGCTL)

# THE GRANULE SEAM (x86_seam.c). 0 is the NEGATIVE CONTROL: no seam prologue is
# emitted at the fold sites and no flush runs at the instruction boundary, so a
# straddling 16/32-bit access reads and writes the slack past the low granule
# instead of its neighbour -- the defect itself. tests/test_seam.py --negative
# builds this and asserts the wrong answers, because a test that cannot be made
# to fail is not a test. Never ship 0.
# PSP SHIFT, in paragraphs, for the granule-seam acceptance test only. 0 is the
# shipping path. 15 puts the PSP 256 bytes off a 4 KB boundary, which is the
# placement that silently corrupted Wolfenstein 3-D before the seam was fixed.
# See x86_dos.c and tests/test_seam.py.
X86_DOS_PSP_SHIFT ?= 0
CFLAGS += -DX86_DOS_PSP_SHIFT=$(X86_DOS_PSP_SHIFT)

X86_SEAM_FIX ?= 1
CFLAGS += -DX86_SEAM_FIX=$(X86_SEAM_FIX)
# The code-stream seam's NEGATIVE CONTROL. 1 threads the opcode granule's
# fold to every fetch site without first proving the instruction fits in it.
X86_CODE_THREAD_UNSAFE ?= 0
CFLAGS += -DX86_CODE_THREAD_UNSAFE=$(X86_CODE_THREAD_UNSAFE)

X86_PROFILE ?= 0
CFLAGS += -DX86_PROFILE=$(X86_PROFILE)

# Frame-loop zone accounting (x86_zones.h). The bare-metal harness has no frame
# loop, so this only ever compiles the storage; it exists here so the two builds
# agree on the flag. See Makefile.core and scripts/profile_all.py.
X86_ZONES ?= 0
CFLAGS += -DX86_ZONES=$(X86_ZONES)

# Probe accumulator (x86_probe.h): what the guest asked for and how we answered.
# OFF by default -- it emits ~2.8 KB of RAM_EMU storage, and RAM_EMU headroom in
# this tree is small enough to have gone negative unnoticed. Not on the guest
# dispatch path; see x86_probe.h for the cost and scripts/probe_report.py for
# the measured link delta.
X86_PROBE ?= 0
CFLAGS += -DX86_PROBE=$(X86_PROBE)

# The CS / interrupt-vector trace rings (x86_cs_log, x86_int_vec_log). These are
# DIAGNOSTIC and they sit inside L(SET_SREG), which every far CALL, RETF, IRET
# and interrupt goes through -- 14 instructions and five stores per far
# transfer, plus a redundant IVT re-read in the interrupt path that exists only
# because the logging clobbers R0. TOPBENCH takes 1.22 M far transfers in 25 s,
# so this was ~17 M instructions of pure logging in a 56.6 M-dispatch window.
# Off by default, like every other instrument here; the storage stays linked so
# probe.py still resolves the symbols (they simply read zero).
# scripts/trace_diff.py and `probe.py --dump x86_cs_log:16x4` need X86_CS_LOG=1.
X86_CS_LOG ?= 0
CFLAGS += -DX86_CS_LOG=$(X86_CS_LOG)

# Protected-mode NEGATIVE CONTROLS (x86_pmode.c). Compiles ONE descriptor-walk
# check out, so tests/test_pmode.py can be shown failing on it. 0 ships.
# Driven by scripts/run_pmode_controls.py; see the table in x86_pmode.c.
X86_PMODE_NEGCTL ?= 0
CFLAGS += -DX86_PMODE_NEGCTL=$(X86_PMODE_NEGCTL)

# HMA NEGATIVE CONTROLS. 1 breaks the A20 gating (the HMA window is left
# registered while the gate is masked, so the guest sees its HMA data instead
# of the low-memory alias -- silent corruption); 2 breaks the seam over the HMA
# (x86_pager_slack() refuses those pages, so a 16-bit access straddling a page
# boundary inside the HMA is served wrong and counted). Driven by
# scripts/run_hma_controls.py. 0 ships.
X86_HMA_NEGCTL ?= 0
CFLAGS += -DX86_HMA_NEGCTL=$(X86_HMA_NEGCTL)
# Negative control for tests/test_nor_wdog.py: compiles the watchdog heartbeat
# out of x86_nor_attach()'s readback loop, which is the defect the fix removes.
X86_NOR_WDOG_NEGCTL ?= 0
CFLAGS += -DX86_NOR_WDOG_NEGCTL=$(X86_NOR_WDOG_NEGCTL)

LDFLAGS = -mcpu=cortex-m7 -mthumb -T STM32H7B0VBTx_SDCARD.ld -Wl,--gc-sections --specs=nosys.specs

PROGS = test_core.elf

TEST_OBJS = x86_cpu.o x86_nor.o x86_profile.o x86_probe.o x86_io.o x86_opl.o emu8950.o x86_mem.o x86_arena.o x86_pager.o x86_pgfile.o x86_seam.o x86_pmode.o x86_video.o x86_video_planar.o x86_osk.o x86_hle.o x86_bios.o x86_dos.o x86_ems.o cga_blit.o cpu_exec.o tests/main.o hw_accel.o x86_disk.o lib/FatFs/ff.o lib/FatFs/ffunicode.o

all: $(PROGS)

test_core.elf: $(TEST_OBJS) TEST.ld
	$(CC) -mcpu=cortex-m7 -mthumb -T TEST.ld -Wl,--gc-sections --specs=nosys.specs -o $@ $(TEST_OBJS)
	$(SIZE) $@

# X86CPUState field offsets, generated on the host so the Thumb-2 core and the
# C struct cannot drift apart. Built with the host compiler, not $(CC).
x86_offsets.inc: offset.c x86_cpu.h cutils.h
	cc -I. -o offset_gen offset.c
	./offset_gen > $@
	rm -f offset_gen

cpu_exec.o: x86_offsets.inc cpu_exec_handlers.S cpu_exec_handlers_0f.S \
            opcode_table.S opcode_0f_table.S

# Header dependencies. Without these, editing a header leaves every .o that
# included it stale and the harness measures the PREVIOUS build -- which is how
# a rewritten font8x8.h produced a "fixed" renderer that still drew the old
# glyphs. sdk/Makefile has done this all along; this build had not.
emu8950.o: lib/emu8950/emu8950.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(TEST_OBJS:.o=.d)

qemu_bank1.bin: test_core.elf
	$(OBJCOPY) -O binary $< test_core_intflash.bin
	dd if=test_core_intflash.bin of=qemu_bank1.bin bs=256k count=1 conv=sync 2>/dev/null

qemu_bank2.bin: qemu_bank1.bin
	dd if=/dev/zero of=qemu_bank2.bin bs=256k count=1 2>/dev/null

# The guest DISK IMAGE. DOS titles ship as .dsk floppy images, not bare
# executables -- a .dsk is a FAT12 filesystem, which is exactly what vdisk.c
# already mounts, and it carries the game's own files rather than one .EXE
# lifted out of context. The sibling .cfg/.dosmeta describe it to the launcher.
GUEST_DSK ?= references/game-and-watch-retro-go-sd/roms/dos/ALLEYCAT.dsk
GUEST_DSK_NAME := $(notdir $(GUEST_DSK))

# Tier-2 extflash: the .dsk IS a FAT12 image, so it is used directly as the
# backing store that vdisk_init() maps -- no repackaging.
extflash.bin: $(GUEST_DSK)
	dd if=/dev/zero of=extflash.bin bs=1M count=64 2>/dev/null
	dd if=$(GUEST_DSK) of=extflash.bin conv=notrunc 2>/dev/null
# Bare 1 MB unformatted card, enough for the tier-2 bare-metal harness which
# never touches it.
sdcard.img:
	dd if=/dev/zero of=sdcard.img bs=1M count=1 2>/dev/null

# ---------------------------------------------------------------------------
# SD card content is NOT built here.
#
# make compiles; Python composes and deploys. The card has to reach three
# destinations -- a gwemu image, a mounted card, and the device over the probe
# -- and that is one recipe with three backends, not an mtools pipeline.
#
#   python3 scripts/sdcard.py --image sdcard_retrogo.img
#   python3 scripts/sdcard.py --mount /media/.../RETROGO
#   python3 scripts/sdcard.py --probe
#
# dos.bin is built by Makefile.core. It is declared PHONY-ish here so the SD
# image can never be assembled around a stale core -- the previous split let
# `make retrogo_media` happily package a core you had not rebuilt.
# ---------------------------------------------------------------------------
RETROGO_SD ?= sdcard_retrogo.img

# Ordered steps, not a dependency graph. `dos.bin: core` looked right but was
# a trap: the dos.bin target had no recipe of its own, so make ran `core`
# (which rewrites dos.bin) yet never re-stat'd it -- and happily packaged a
# stale core into the SD image. sdcard.py rebuilds the image unconditionally,
# so there is nothing for timestamps to save here anyway.
.PHONY: core sdcard
core:
	$(MAKE) -f Makefile.core pack

sdcard: core
	python3 scripts/sdcard.py --image $(RETROGO_SD)

gwemu_media: qemu_bank1.bin qemu_bank2.bin extflash.bin sdcard.img

clean:
	rm -f *.o *.d *~ tests/*.o $(PROGS) *.bin *.img
