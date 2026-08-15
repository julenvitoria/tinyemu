/*
 * TinyEMU x86 — Retro-Go SD external core.
 *
 * The emulator loop: guest memory, CGA video, PC speaker and input, driven a
 * frame at a time. What it does NOT have yet is the DOS layer (#3), so the
 * guest it runs is a built-in real-mode program rather than a loaded .EXE --
 * see demo_guest[]. That program drives the same paths a real game does
 * (segment loads, REP STOSW into video memory, port writes), so video, audio
 * and input are exercised end to end rather than asserted to work.
 *
 * It also still proves the pieces the architecture rests on:
 *
 *   1. The hand-written Thumb-2 interpreter runs from a GNW_CORE_REGION_ITCM
 *      segment (see dos_core.ld) -- the entire performance thesis.
 *   2. CPU state and the guest page table can live in DTCM, which for a core
 *      means a runtime mem_ctl(GW_MEM_DTC, ...) allocation rather than a
 *      statically-placed section ("AHB/DTCM are firmware dynamic pools, not
 *      load targets" -- gnw_core_meta.h).
 *   3. The benchmark and the opcode suite produce the same numbers here as in
 *      the bare-metal harness, so tier 2 and tier 3 are comparable.
 *
 * Video, input, sound and the DOS layer are deliberately absent.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "common.h"
#include "gw_lcd.h"
#include "rom_manager.h"
#include "odroid_system.h"
#include "gw_malloc.h"

#include "gw_core_bridge.h"

#include "cutils.h"   /* BOOL/TRUE, needed by iomem.h via x86_cpu.h */
#include "x86_cpu.h"
#include "x86_profile.h"
#include "x86_zones.h"   /* ZONES: frame-loop phase accounting, X86_ZONES=1 */
#include "x86_vprof.h"   /* VPROF: video-path cycle attribution, X86_VPROF=1 */
#include "x86_osk.h"
#include "x86_opl.h"   /* OPL2 FM tone generation (x86_io.c owns the ports) */
#ifndef X86_OPL_FM
#define X86_OPL_FM 1      /* the harness builds do not pass the core's flags */
#endif
#ifndef X86_OPL_BENCH
#define X86_OPL_BENCH 0
#endif
#include "x86_arena.h"
#include "x86_video_planar.h"
#include "x86_pager.h"
#include "x86_pgfile.h"
#include "x86_xms.h"
#include "x86_ems.h"
#include "x86_settings.h" /* per-title settings: the format and the policy   */
#include "x86_cfg.h"      /* per-title settings: the DOSBox-syntax .cfg      */

/* Chrom-ART offload for the chained 13h blit. DEFAULT OFF and UNVERIFIED --
   the whole rationale, the price and what tier 2 can and cannot prove are on
   dma2d_blit_hook() further down. */
#ifndef X86_VIDEO_DMA2D
#define X86_VIDEO_DMA2D 0
#endif
#if X86_VIDEO_DMA2D
static int dma2d_blit_hook(uint8_t *dst, const uint8_t *src, uint32_t bytes);
static void dma2d_selftest(void);
#endif

#define APP_ID       100

/* Output rate, and the half-buffer that goes with it.
 *
 * 48 kHz because the SAI can be told to run at exactly that
 * (Core/Src/porting/odroid_audio.c reconfigures it from this number), and
 * because a PC-speaker square is all edge: at 16 kHz its period is a couple of
 * dozen samples, every edge is quantised to one of them, and the jitter is
 * audible as roughness on a tone whose pitch is otherwise correct. 16 kHz also
 * puts the Nyquist floor at PIT divisor 149 -- everything above 8 kHz aliases.
 *
 * SPEAKER_FRAMES is the half-buffer the DMA consumes, and it MUST be about one
 * frame's worth of audio: we refill the active half once per rendered frame,
 * and the firmware's default half-buffer is 1077 samples. At 16 kHz that was
 * 67 ms of audio refilled every ~17 ms, so the writer lapped the DMA reader
 * three times over and the tone the guest asked for came out as a stitch of
 * restarts -- present, in tune, and rough. 48000/60 = 800 is one panel period
 * exactly and is inside the firmware's 1077-sample ceiling
 * (audiobuffer_dma is not bounds-checked anywhere; see gw_audio.c). */
#define SAMPLE_RATE    48000
#define SPEAKER_FRAMES (SAMPLE_RATE / 60)

/* Guest RAM for the spike.
 *
 * AXI budget, measured against the built firmware rather than assumed:
 *   RAM_EMU      724 KB  (__RAM_EMU_LENGTH__ = 0xb5000) -- code + data + bss
 *   bonus pool  ~150 KB  reclaimed by running the LCD in LUT8
 *   total       ~874 KB  which is docs/01's "~870 KB AXI pool"
 *
 * The LCD pool is statically sized at 300 KB for the largest mode (2x RGB565).
 * Switching to LUT8 halves the framebuffer requirement and the firmware frees
 * the upper half as a bonus pool -- so the reclaimed AXI is NOT extra RAM_EMU,
 * it is a separate region fetched at runtime via lcd_get_bonus_pool().
 *
 * The spike's benchmark needs only a few KB (code at 0, IDT at 0x1000, ISR at
 * 0x2000, stack at 0x3000), so it runs in 64 KB; the bonus pool is claimed and
 * reported here purely to prove the mechanism and the real budget before the
 * paging architecture is built on top of it. */
/* Conventional memory. A DOS game wants 640 KB; VRAM is a separate dedicated
 * block so the video aperture never moves and never competes with the pool. */
#define GUEST_RAM_SIZE   (640 * 1024)
#define FRAME_W          320
#define FRAME_H          240

extern void x86_cpu_interp_asm(void);
static void emulator_loop(const gw_firmware_abi_t *abi);
static int mount_guest_disk(void);
/* Per-title settings. Defined beside the CPU-profile menu row, because that row
   is the only thing that WRITES them; app_main() only has to load them, and it
   has to do it before emulator_loop() (see the call site). */
static void settings_load(void);

/* The interpreter dereferences these; it never references the symbols
 * directly, which is what lets one assembly source serve both builds. */
extern X86CPUState *cpu_state_ptr;
extern uint32_t *guest_page_table_ptr;

static uint8_t *guest_ram;
static uint8_t *guest_vram;
static char dsk_path[256];
volatile uint32_t dsk_mounted = 0;
volatile uint32_t dsk_exec_rc = 0xFFFFFFFF;
volatile uint32_t dsk_startup_ok = 0;
/* 1 = honour AUTOEXEC.BAT's arguments, 0 = launch the program with none. */
volatile uint32_t guest_use_cmdtail = 0;
#define ODROID_PAD_BITS 13
/*
 * The LUT8 CLUT we hand the firmware. 32 entries, and the COUNT WE PUSH IS
 * ALWAYS 32, even though CGA graphics uses four of them and text sixteen.
 *
 * That is a hard requirement of the firmware's darkening contract, not
 * tidiness: lcd_set_clut() programs the darkened twin of entry i at i+count,
 * while odroid_overlay_darken_all() dims a pixel by ORing LCD_DARKEN_BIT
 * (0x20 == 32) into its index. The two only agree at count == 32. Pushing 4
 * put the twins at 4..7 while the overlay asked for 32..35 -- unprogrammed,
 * i.e. black -- so opening the menu blanked the guest picture underneath.
 * Pushing 32 also stops mode 3 from losing colours 4..15.
 */
#define CORE_CLUT_ENTRIES 32
static uint32_t core_clut[CORE_CLUT_ENTRIES];

/*
 * THE LCD BONUS POOL, carved by fixed offsets.
 *
 * 153,600 bytes of cached AXI that the firmware releases when the LCD runs in
 * LUT8 (the framebuffers need only 154 KB of the 300 KB pool). It is not an
 * allocator and does not need to be -- every consumer is known at build time
 * and none of them is ever freed. Keeping the map in ONE place is the point:
 * the offsets used to be open-coded 4096/8192 at four call sites, which is how
 * two of them came to disagree about whether the BIOS page or the sector cache
 * came first.
 *
 *   0    4,096   open-bus filler, aliased over 0xA0000-0xFFFFF
 *   4K   4,096   BIOS identification page at 0xFF000
 *   8K  65,536   VGA mode 13h aperture at 0xA0000
 *  72K   2,048   mode 13h host palette (768 B DAC + 1 KB ARGB CLUT)
 *  74K  32,768   CGA VRAM at 0xB8000
 *              ---------
 *             108,544 used, 45,056 free
 *
 * The .dsk sector cache is NOT here any more -- it is a DMA-reachable staging
 * buffer and lives in the AHB heap. That is what freed the 64 KB this map
 * spends on the aperture.
 *
 * Free space is deliberately left contiguous at the TOP: the next thing that
 * wants it is a VRAM arena that can balloon for an EGA mode and shrink again.
 */
/*
 * ORDERING IS ARITHMETIC HERE, not taste. Everything the pager will hand out
 * is handed out in 4 KB pages, so the free region has to START page-aligned
 * and END at the top of the pool or the odd bytes at each end are simply lost.
 *
 * The 2 KB palette used to sit in the MIDDLE of this map, which pushed the
 * free region to offset 108,544 -- not a multiple of 4,096. Aligning up cost a
 * page and left a 2,048-byte tail nobody could use: 10 usable pages out of a
 * 45,056-byte hole that is exactly 11.
 *
 * So the palette moves to the very TOP, above the free region, and the blocks
 * below it sum to 106,496 = 26 pages exactly. The hole between them is
 * 45,056 bytes = 11 whole pages with nothing wasted at either end. Same total
 * footprint, one more page of resident set.
 */
#define BONUS_POOL_BYTES   153600u      /* what LUT8 releases; checked at runtime */

/*
 * The map is computed at RUNTIME, and that is worth a page.
 *
 * MEASURED on target: the bonus pool comes back at 0x24025800 -- 2048-aligned,
 * NOT 4096-aligned -- while its END, 0x2404B000, is. A compile-time map that
 * starts the arena at a fixed offset therefore hands x86_arena_add_region() a
 * misaligned base, it rounds inward as it must, and a whole page disappears:
 * 139,264 bytes of arena where the pool has room for 143,360.
 *
 * So the 2 KB palette -- the one block whose size is not a page multiple --
 * goes FIRST and absorbs whatever the misalignment is. The two fixed 4 KB
 * pages follow on the first page boundary at or after it, and the arena takes
 * everything from there to the top of the pool. Nothing here assumes the base
 * is aligned any particular way, so a firmware that changes the LUT8 split
 * cannot silently cost a page again.
 *
 * Everything that is NOT one of those three fixed blocks is the ARENA. CGA
 * VRAM and the mode 13h aperture used to be fixed blocks too, 96 KB spent
 * permanently to serve one mode at a time -- which is why EGA 0Dh has never
 * been reachable. They are claims now, and the modes are mutually exclusive,
 * so the peak demand is the LARGEST mode rather than the sum. See x86_arena.h.
 */
#define BONUS_VGAPAL_OFF   0u           /* absorbs the base's misalignment */
#define BONUS_USED         BONUS_POOL_BYTES
/* Offsets of the two fixed pages and of the arena, filled in at startup from
   the pool's ACTUAL base so each lands on a page boundary. */
static uint32_t bonus_filler_off, bonus_biosid_off;
static uint32_t bonus_arena_off,  bonus_arena_bytes;

static void bonus_layout(const uint8_t *base, uint32_t size)
{
    uint32_t b = (uint32_t)(uintptr_t)base;
    /* The first page boundary at or after the palette. */
    uint32_t head = (((b + X86_VGA_PALETTE_BYTES) + 4095u) & ~4095u) - b;
    bonus_filler_off = head;
    bonus_biosid_off = head + 4096u;
    bonus_arena_off  = head + 8192u;
    bonus_arena_bytes = (size > bonus_arena_off) ? size - bonus_arena_off : 0u;
}
/* Which table the firmware currently holds: NULL = the 32-entry cached path,
   non-NULL = the core-owned 256-entry one. Compared by POINTER, so entering
   and leaving a 256-colour mode each push exactly once. */
static const uint32_t *clut256_pushed;
static uint8_t *upper_filler;
static uint8_t *bios_id_page;   /* 0xFF000: real storage, not an alias */
static uint8_t *vga_aperture;   /* 0xA0000: the mode 13h linear framebuffer */
/* The DTCM block dtcm_state_init() took for it, or NULL if the pool refused.
   Aligned; see the block comment there. */
static uint8_t *dtcm_aperture;
#define X86_VGA_APERTURE_ALIGN 32u

/* Host-readable so a failure to place either block is visible rather than a
   black screen. */
volatile uint32_t guest_ram_bytes = 0;
/* THE POOL IS SIZED AT RUNTIME, SO ITS SIZE IS A MEASUREMENT.
 *
 * guest_ram is the last big claim out of RAM_EMU, so it is what any growth in
 * the core -- an instrument's .bss above all -- comes out of. Publishing the
 * three numbers is what turns "the guest silently never ran" into a reading:
 *   guest_ram_pages        frames the pager actually got (160 = full 640 KB)
 *   guest_ram_pages_short  how many it did NOT get. NON-ZERO IS THE ALARM.
 *   guest_ram_free_at_claim RAM_EMU free bytes at the moment of the claim.
 * A short pool still runs -- it pages -- but nothing that reads a counter off
 * this build may treat it as equivalent to the shipping one. */
volatile uint32_t guest_ram_pages = 0;
volatile uint32_t guest_ram_pages_short = 0;
volatile uint32_t guest_ram_free_at_claim = 0;
volatile uint32_t guest_vram_ok = 0;
volatile uint32_t upper_filler_ok = 0;   /* 0xA0000-0xFFFFF backed at all */
/* Both allocations that can fail QUIETLY and cost a whole feature. The VGA
   aperture is 64 KB out of the bonus pool; the sector cache is 64 KB out of
   the AHB heap, which is shared with the firmware's own newlib allocations
   and so is the one whose free space cannot be read off a link map. */
volatile uint32_t vga_aperture_ok = 0;
/*
 * WHERE the aperture landed, and the witness for a DTCM allocation that failed.
 *
 * The 64 KB aperture is taken from the DTCM bump pool rather than the AXI
 * arena (see dtcm_aperture_init()). A NULL there is not fatal -- the arena
 * claim is still there as a fallback -- but it MUST be visible, because the
 * symptom of a silent failure is a black screen with no message, which is the
 * exact shape STATUS.md records for RAM_EMU exhaustion.
 *
 *   x86_vga_aperture_dtcm       1 = DTCM (the intended path), 0 = arena
 *   x86_vga_aperture_dtcm_fail  1 = DTCM was asked and said no
 */
volatile uint32_t x86_vga_aperture_dtcm = 0;
volatile uint32_t x86_vga_aperture_dtcm_fail = 0;
volatile uint32_t disk_cache_mem = 0;    /* the AHB pointer itself, or 0 */
volatile uint32_t frames_rendered = 0;

/* THE HOST AUDIO CHAIN'S OWN INSTRUMENTS. See the audio phase in the frame
   loop for why each exists; between them they say whether a buffer was
   submitted, whether it had anything in it, and at what volume. */
volatile uint32_t x86_audio_submits = 0;   /* buffers handed to the SAI DMA  */
volatile uint32_t x86_audio_peak_last = 0; /* loudest sample, newest buffer  */
volatile uint32_t x86_audio_peak_max = 0;  /* loudest sample, whole session  */
volatile uint32_t x86_audio_volume = 0;    /* the launcher's own 0..255      */
/*
 * ===========================================================================
 * THREE CLOCKS, and the fact that they are three.
 * ===========================================================================
 *
 * One constant used to do all three jobs (PIT_SLICE_INSNS: guest instructions
 * per iteration), which is why moving it broke unrelated things -- the frame
 * rate, the BIOS tick rate and the emulated machine's speed were the same
 * number wearing three hats. They are now separate, and none of them is a
 * hardcoded 60:
 *
 *   1. DISPLAY RATE -- follows the hardware. The loop renders, asks for a
 *      swap, and waits on lcd_is_swap_pending(). That is the panel's own
 *      signal, so an overclocked device or a different refresh rate paces us
 *      correctly with no constant to update. Nothing here assumes 60 fps.
 *
 *   2. GUEST WALL CLOCK -- the firmware's millisecond counter, see the tick
 *      block below. The 18.2065 Hz PIT/BIOS tick derives from elapsed real
 *      time and from nothing else, which is what makes music, delays and
 *      INT 1Ah correct at any frame rate and any host speed.
 *
 *   3. GUEST CPU RATE -- a configurable target, PACED AGAINST WALL TIME. The
 *      instruction budget is per elapsed millisecond:
 *
 *          budget = guest_kips * elapsed_ms          (kips = insns per ms)
 *
 *      so overclocking the H7 does not make the guest faster; it makes the
 *      budget arrive sooner and the leftover become idle, which the panel
 *      wait spends. That is the point: the emulated CPU's speed is a property
 *      of the emulated machine, not of ours.
 *
 * kips is "thousands of instructions per second", which is also exactly
 * "instructions per millisecond" -- so the budget is an integer multiply with
 * no remainder to carry and no division in the loop.
 *
 * WHEN WE CANNOT KEEP UP, IT IS LOUD. Unmet budget accumulates in insn_debt;
 * past GUEST_DEBT_MAX it is written off into guest_insn_deficit, a
 * host-readable count of instructions the emulated CPU owed and never ran.
 * A guest quietly running at half its configured speed is indistinguishable
 * from a correct one, which is the exact failure this project keeps hitting.
 *
 * TWO PACING MODES, one knob (guest_kips):
 *
 *   guest_kips > 0   PACED. The guest runs at that many thousand instructions
 *                    per second and NO FASTER, however fast the silicon is.
 *                    Leftover time becomes idle in the panel wait. This is
 *                    what makes a speed-sensitive title behave, and what makes
 *                    overclocking the H7 change nothing about game speed. The
 *                    number to watch is guest_insn_deficit: it is the answer
 *                    to "I asked for a 386 -- am I getting one?".
 *
 *   guest_kips == 0  UNTHROTTLED. No budget, no idle: run as much as fits
 *                    between panel frames, which is what the benchmark and
 *                    the optimisation work want to see. The slice sizes itself
 *                    from measured execution time against panel_ms_min -- the
 *                    shortest iteration the hardware has ever let us complete,
 *                    i.e. the display's own period -- so it tracks the panel
 *                    rather than a hardcoded 60. A deficit is meaningless
 *                    here; guest_ips_achieved is the number.
 *
 * The BIOS tick is wall-clock in BOTH modes. Unthrottling the CPU must not
 * unthrottle time -- that is the bug being fixed, not a mode.
 */
#define KIPS_MAX              0u   /* as fast as the silicon delivers */
#define KIPS_8088_477       330u   /* 8088 @ 4.77 MHz, ~0.33 MIPS     */
#define KIPS_286_8          800u   /* 286 @ 8 MHz                     */
#define KIPS_286_12        1200u   /* 286 @ 12 MHz                    */
#define KIPS_386SX_16      2500u   /* 386SX @ 16 MHz                  */
#define KIPS_386_33        6000u   /* 386DX @ 33 MHz                  */
/*
 * The 486 rungs are STRESS TARGETS, not period-correct emulation, and the
 * distinction matters when reading a score off them.
 *
 * Measured on silicon: ~5.54 MIPS, i.e. ~5500 kips. So KIPS_386_33 (6000) is
 * ALREADY above what the machine delivers, and every rung at or above it is
 * requesting more than exists. A paced profile only throttles when its target
 * is BELOW the achievable rate; above it, the budget is never met, the guest
 * runs flat out and the rung is indistinguishable from Max. That -- not a
 * broken profile table -- is why 386SX 16 / 386DX 33 / Max were reported
 * scoring the same, and guest_insn_deficit is the witness: it stays 0 in Max
 * and climbs on any rung the silicon cannot satisfy.
 *
 * They earn their place anyway. A rung the CPU cannot satisfy still changes
 * how the frame is SHAPED -- bigger per-frame budgets, longer slices, less
 * idle handed back -- so a workload that keeps up at one rung and stutters at
 * the next names the subsystem that could not follow (blit, audio refill,
 * input poll), which is the thing a single "Max" cannot tell apart.
 */
#define KIPS_486SX_25      9000u   /* 486SX @ 25 MHz  -- above the ceiling */
#define KIPS_486DX2_66    24000u   /* 486DX2 @ 66 MHz -- stress target     */
#define KIPS_486DX4_100   40000u   /* 486DX4 @ 100 MHz -- stress target    */
/*
 * DEFAULT: unthrottled. The emulated machine runs as fast as the silicon
 * delivers, and the number that describes it is guest_ips_achieved, not a
 * deficit -- with no target there is nothing to be short OF, so the deficit
 * counter is held at zero in this mode rather than left showing a stale value
 * from a paced one. A period-correct machine is one menu entry away
 * (cpu_profile_*, below), and the choice persists across a restart.
 */
volatile uint32_t guest_kips = KIPS_MAX;      /* 0 = unthrottled */
volatile uint32_t guest_slice_now  = 0;  /* instructions this iteration got   */
volatile uint32_t guest_insn_debt  = 0;  /* owed but not yet run              */
volatile uint32_t guest_insn_deficit = 0;/* owed and WRITTEN OFF -- the loud   */
volatile uint32_t guest_ips_achieved = 0;/* measured instructions per second  */
volatile uint32_t guest_fps_achieved = 0;/* measured iterations per second    */
volatile uint32_t guest_insns_total = 0; /* instructions run since boot       */

/*
 * WHERE THE FRAME LOOP IS RIGHT NOW.
 *
 * The watchdog is refreshed exactly ONCE per iteration, at the very bottom of
 * the body, so the WWDG window is a whole FRAME: any single stage that blocks
 * longer than the window resets the device, and every counter the loop banks
 * (guest_insns_total, frames_rendered, frame_ms_last) is banked at the bottom
 * too -- so a stage that never finishes freezes ALL of them and leaves nothing
 * behind that says which stage it was. Three sessions read that freeze as
 * evidence about the GUEST; it is not, the guest is not even running.
 *
 * x86_frame_phase is one store per stage and it is readable over the probe
 * WHILE the loop is stalled, which is the only moment the answer exists.
 * x86_frame_phase_ms is when the stage was entered, so "stalled" and "slow"
 * are distinguishable from a single sample, and x86_frame_phase_seq
 * increments per stage so a repeating INNER loop is visible as a sequence
 * still climbing while frames_rendered does not.
 */
volatile uint32_t x86_frame_phase = 0;   /* X86_PHASE_* below                 */
volatile uint32_t x86_frame_phase_ms = 0;/* HAL tick at which it was entered  */
volatile uint32_t x86_frame_phase_seq = 0;/* stage transitions since boot     */
volatile uint32_t x86_frame_phase_worst = 0;/* longest stage, host ms         */
volatile uint32_t x86_frame_phase_worst_id = 0;/* ...and which stage it was   */
/* Slices abandoned by the wall-clock escape below. Nonzero = a guest loop ran
   past its instruction budget in TIME, which is the defect the escape turns
   into a slow frame instead of a reset. */
volatile uint32_t guest_slice_overruns = 0;
/* The same defect measured at the only granularity that can actually starve
   the watchdog: ONE x86_cpu_interp_asm() call. The interpreter cannot be
   interrupted, so this is the true worst case the frame loop is exposed to. */
volatile uint32_t guest_asm_ms_worst = 0;
volatile uint32_t guest_asm_overruns = 0;
volatile uint32_t frame_ms_last    = 0;  /* last iteration, host ms           */
volatile uint32_t panel_ms_min     = 0;  /* shortest iteration seen = the panel*/
volatile uint32_t frame_10us_avg = 8u * 1667u; /* mean iteration, 8x, 10us units */
volatile uint32_t guest_exec_ms    = 0;  /* host ms spent RUNNING the guest    */

/*
 * Frame-time distribution. "Stutter" is a frame-deadline MISS, and neither a
 * mean (frame_10us_avg) nor a minimum (panel_ms_min) can show one: a run that
 * drops six frames to a 90 ms hitch and is otherwise perfect has the same mean
 * as a run that is uniformly 2 ms slow. Only the TAIL separates them, so the
 * tail is what is recorded -- a histogram plus the two worst iterations ever
 * seen and a count of iterations that overran the learned panel period.
 *
 * Buckets are in units of the panel period (frame_ms_last vs panel_ms_min), so
 * this stays meaningful if the panel rate changes or the device is overclocked:
 * 0 = on time (<=1.5x), 1 = <=2x, 2 = <=3x, 3 = <=4x, 4 = <=6x, 5 = <=8x,
 * 6 = <=16x, 7 = worse.
 */
volatile uint32_t frame_hist[8];
volatile uint32_t frame_ms_worst   = 0;  /* worst iteration ever               */
volatile uint32_t frame_ms_worst2  = 0;  /* second worst -- one hitch is noise */
volatile uint32_t frame_overruns   = 0;  /* iterations over 1.5x panel period  */
volatile uint32_t frame_hist_from  = 0;  /* frames_rendered when last reset    */
volatile uint32_t frame_stats_reset = 0; /* host writes 1 to zero the tail     */

/* ======================= ZONES: begin (host witness) =======================
   Is the phase/zone block backed? Read by scripts/profile_all.py, which
   otherwise cannot tell "this core has no zone instrumentation compiled in"
   from "the DTCM allocation failed". Always present so the host does not have
   to know which build it is talking to. ZONES: end ========================= */
volatile uint32_t zones_block_ok = 0;

static void frame_hist_add(uint32_t ms, uint32_t panel)
{
    if (!panel)
        panel = 16u;
    /* x10 so the 1.5x edge is integer arithmetic, not a float. */
    uint32_t r = (ms * 10u) / panel;
    uint32_t b;
    if      (r <= 15u)  b = 0;
    else if (r <= 20u)  b = 1;
    else if (r <= 30u)  b = 2;
    else if (r <= 40u)  b = 3;
    else if (r <= 60u)  b = 4;
    else if (r <= 80u)  b = 5;
    else if (r <= 160u) b = 6;
    else                b = 7;
    frame_hist[b]++;
    if (b)
        frame_overruns++;
    if (ms > frame_ms_worst) {
        frame_ms_worst2 = frame_ms_worst;
        frame_ms_worst = ms;
    } else if (ms > frame_ms_worst2) {
        frame_ms_worst2 = ms;
    }
}

/* Did the last slice end because the GUEST halted? Then guest_exec_ms is short
   because the guest asked to idle, NOT because we had spare capacity. Growing
   the slice on that reading is how the controller inflated to its ceiling and
   then ran a multi-second slice that tripped the watchdog. */
static uint32_t   last_slice_halted = 0;
/*
 * THE PACING CONTROLLER'S OWN STATE, published because it is otherwise
 * invisible and it is what decides throughput.
 *
 * "Busy 55.8% against a 90% target" has three candidate explanations that look
 * identical from outside: the slice is still ramping, the slice has settled
 * somewhere below the ceiling, or the guest keeps HALTING so the regulator
 * refuses to grow (idle is not headroom). These separate them:
 *
 *   guest_max_slice     where the regulator has actually settled
 *   guest_slice_halted  slices that ended on a guest HLT
 *   guest_grow_blocked  frames the halted arm suppressed a grow
 *
 * A ramp that keeps climbing for seconds is guest_max_slice still moving; a
 * ceiling that is not the ceiling is guest_max_slice parked below
 * MAX_SLICE_CEIL; and a guest that idles is guest_grow_blocked climbing while
 * guest_max_slice sits still.
 */
volatile uint32_t guest_max_slice    = 20000u;
volatile uint32_t guest_panel_ms_used = 0;  /* the period the budget is 90% OF */
volatile uint32_t guest_slice_halted = 0;
/* Where the guest was when it executed HLT: (CS << 16) | IP, 16 deep, plus the
   last linear EIP. See the capture site for why the LOCATION is the question
   and the count is not. */
volatile uint32_t x86_hlt_log[16];
volatile uint32_t x86_hlt_log_idx = 0;
volatile uint32_t x86_hlt_last_eip = 0;
volatile uint32_t guest_grow_blocked = 0;
static uint32_t   max_slice = 20000u;    /* unthrottled mode's working slice  */
static uint32_t   rate_window_ms;        /* start of the achieved-rate window */
static uint32_t   rate_window_insns;
static uint32_t   rate_window_frames;
/* One iteration's work is bounded so the render, the input poll and the audio
   refill still happen when a guest stretch is expensive: a frame that owes
   200 ms of instructions must not spend 200 ms before drawing anything. */
#define GUEST_SLICE_MAX_MS  33u
#define GUEST_DEBT_MAX_MS  100u
#define MAX_SLICE_FLOOR   1000u
/* A TIME bound expressed in instructions. 50k at ~2 M ips is ~25 ms, inside
   the watchdog window, and still allows 3 M ips at 60 iterations/s. It was
   4,000,000 -- two SECONDS in a single slice with no return to the frame loop,
   which is exactly what tripped the watchdog once the controller had inflated
   to it. Do NOT derive this from measured throughput: the slice limits
   throughput, so a cap computed from throughput ratchets both down to the
   floor (measured: 59,642 ips, pinned there). */
/*
 * RAISED 50,000 -> 200,000. The old value was not a safety bound, it was the
 * throughput limit: at ~5.5 M insn/s a 50,000-instruction slice finishes in
 * ~9 ms of a 16.7 ms frame, so the machine sat ~54% busy with the regulator
 * asking for more every single frame.
 *
 * The watchdog argument that produced 50,000 was about a slice running
 * 4,000,000 instructions -- TWO SECONDS -- with no return to the frame loop.
 * 200,000 is nowhere near that: at the measured ~50 cycles/insn it is ~10 M
 * cycles, ~36 ms at 280 MHz, and one order of magnitude inside the watchdog
 * window. It is a backstop for a guest that has gone pathological, not the
 * thing that decides throughput -- GUEST_EXEC_PCT decides that, on measured
 * time, and shrinks the slice by itself when instructions turn expensive.
 */
#define MAX_SLICE_CEIL  200000u
/*
 * Share of a display period the guest may execute in, percent. The rest is the
 * render, the audio refill and the input poll. 90 leaves ~1.7 ms of a 16.7 ms
 * frame; the blit is 200 memcpys of 320 B and the audio refill is bounded, so
 * that is breathing room rather than a gamble -- and a frame that does overrun
 * shows up as dropped frames, which is a number we already publish rather than
 * a silent stutter.
 */
/*
 * 85, not 90. Measured on silicon at 90: 59.0 fps but 4.2 dropped and busy
 * 56.5% -- the drops are the cliff, not saturation. A slice that overruns the
 * panel period does not cost the overrun, it costs a WHOLE frame, because the
 * swap then waits for the next vsync and the MCU sits idle for the remainder.
 * So the optimum is just INSIDE the deadline and the penalty for being just
 * outside it is enormous; 85% of 16 ms is ~13.6 ms with ~2.4 ms of margin for
 * the blit, the audio refill and the input poll.
 *
 * THE REMAINING IDLE IS NOT OURS TO RECLAIM. Measured in the same run:
 * guest_exec_ms reads 0 on most frames and guest_slice_halted climbs by ~253
 * over 660 frames -- ~38% of frames end on a guest HLT -- while guest_ticks
 * runs at 18.6/s, the stock 18.2 Hz BIOS tick. The guest halts waiting for a
 * timer interrupt ~55 ms away, i.e. 3.3 frames. A real 386 idles there too.
 * No value of this constant recovers it; the lever is the guest's tick rate,
 * which is the guest's business.
 */
#define GUEST_EXEC_PCT       85u
/* Bounds on the display period the budget is a percentage OF. 16 ms is 60 Hz;
   the window is deliberately narrow because anything outside it means the
   measurement is wrong rather than the panel is unusual, and a budget derived
   from a wrong period is exactly the bug this replaced. */
#define PANEL_MS_MIN_SANE    14u
#define PANEL_MS_MAX_SANE    20u

/*
 * The guest's clock, and the fact that it is REAL time.
 *
 * 18.2065 Hz is 1193182/65536 -- the PIT input divided by channel 0's boot
 * divisor. A guest counts note durations, delays and animation frames in these
 * ticks, so if the tick rate moves, everything the user hears and sees moves
 * with it. Two earlier drivers both got that wrong, in opposite directions and
 * at the same time (they were merged into one loop):
 *
 *   - one derived the tick from RENDERED FRAMES (182/600 per frame, exact only
 *     at 60 fps). At the measured ~10 fps it fired at ~3 Hz, six times slow.
 *   - one derived it from the instruction SLICE (TICKS_PER_SLICE = 10 per
 *     iteration), which fired at ~100 Hz and moved with our throughput.
 *
 * Both are gone. The tick is now driven from the firmware's millisecond
 * counter, with the remainder carried in `tick_acc` so the long-run rate is
 * exact rather than accumulating a rounding error:
 *
 *      tick_acc += elapsed_ms * 1193182;  while (tick_acc >= 65536000) tick.
 *
 * The rejected alternative was to keep the slice-derived clock because a
 * benchmark divides fixed work by elapsed ticks and a too-slow clock reads as
 * zero elapsed ticks (a #DE). That is a real hazard, but the answer to it is
 * enough guest throughput per real second, not a clock that lies -- a clock
 * slaved to our own speed makes every benchmark measure 1.0 by construction.
 *
 * CATCH-UP IS CAPPED. A debugger halt or a long SD read owes hundreds of ticks
 * at once, and emitting them is worse than not: a guest hooked to INT 08h
 * would run its music routine hundreds of times back to back. Anything past
 * TICK_CATCHUP_MAX is dropped -- and COUNTED, in guest_ticks_dropped, because
 * time silently going missing is precisely the class of bug this whole file
 * exists to make visible.
 */
#define TICK_NUM          1193182u    /* PIT input, Hz                       */
#define TICK_DEN_MS      65536000u    /* 65536 (divisor) * 1000 (ms per s)   */
#define TICK_CATCHUP_MAX        4u    /* ~220 ms of arrears, then drop       */
/* IRQ 0s one frame may deliver. Each costs a sub-slice boundary and a guest
   ISR entry, so a frame that owes hundreds (a debugger halt) must not try to
   run them all back to back -- the arrears cap above is the time bound and
   this is the per-frame one. Eight covers 140 Hz down to 17 fps. */
#define TICK_IRQ_MAX            8u
#define TICK_STALL_MS        1000u    /* longer than this is a stall, not lag */
static uint32_t tick_acc;             /* ms * TICK_NUM, below one IRQ 0      */
static uint32_t bda_acc;              /* the same, for the 18.2 Hz BDA count */
static uint32_t tick_irqs_owed;       /* IRQ 0s this frame still has to give */
static uint32_t tick_last_ms;
volatile uint32_t guest_ticks = 0;          /* ticks handed to the guest     */
volatile uint32_t guest_ticks_dropped = 0;  /* owed but refused (see above)  */
volatile uint32_t guest_tick_irq_lost = 0;  /* tick IRQ the 1-deep slot ate  */
volatile uint32_t guest_tick_divisor = 65536; /* PIT ch0 divisor in force    */
volatile uint32_t guest_uptime_ms = 0;      /* host ms since the loop started */

volatile uint32_t guest_exit_reason = 0;
volatile uint32_t guest_halted = 0;
volatile uint32_t fb_addr = 0;        /* the buffer the blit was handed */
volatile uint32_t fb_sample = 0;      /* first 4 pixels of the picture area */
volatile uint32_t clut_sample = 0;    /* colour 1, to prove the CLUT took */
volatile uint32_t hle_calls_seen = 0;
/* Input chain, made observable end to end. The failure mode this exists for is
   "nothing happens when I press a key", which has five candidate breaks and no
   way to tell them apart from outside: the poll not running, the firmware
   handing us an empty pad, the mapping producing no bits, no scancode reaching
   the controller queue, or the IRQ never being delivered. One counter per
   stage, read with scripts/probe.py --watch. */
volatile uint32_t input_polls = 0;      /* poll_input() calls               */
volatile uint32_t input_pad_raw = 0;    /* last raw pad, bit N = values[N]  */
volatile uint32_t input_pad_seen = 0;   /* sticky OR of the above           */
volatile uint32_t input_keys_now = 0;   /* our 8-bit mapped key mask        */
volatile uint32_t input_scancodes = 0;  /* codes handed to the controller   */
volatile uint32_t input_irqs = 0;       /* IRQ 1 raises                     */
volatile uint32_t input_kbd_ht = 0;     /* controller head | tail<<8        */
volatile uint32_t hle_bad_vector = 0;
volatile uint32_t hle_bad_func = 0;
volatile uint32_t guest_ax = 0;

/*
 * Built-in guest, 16-bit real mode, entered at 0000:0100.
 *
 * Sets the CGA colour-select register, fills the video aperture through
 * ES:DI with REP STOSW, programs PIT channel 2 and opens the speaker gate,
 * then spins. Every one of those goes through a path this core implements
 * from scratch, so if the screen or the tone is wrong the failure is here and
 * not in a missing DOS layer.
 */
static const uint8_t demo_guest[] = {
    /* INT 10h AH=0Eh: teletype a character through the BIOS */
    0xB4, 0x0E,                   /* mov ah, 0x0E        */
    0xB0, 0x41,                   /* mov al, 'A'         */
    0xCD, 0x10,                   /* int 0x10            */
    /* INT 21h AH=30h: ask DOS its version */
    0xB4, 0x30,                   /* mov ah, 0x30        */
    0xCD, 0x21,                   /* int 0x21            */
    /* colour select: palette 1, high intensity */
    0xBA, 0xD9, 0x03,             /* mov dx, 0x03D9      */
    0xB0, 0x30,                   /* mov al, 0x30        */
    0xEE,                         /* out dx, al          */
    /* mode: graphics, video enabled */
    0xBA, 0xD8, 0x03,             /* mov dx, 0x03D8      */
    0xB0, 0x0A,                   /* mov al, 0x0A        */
    0xEE,                         /* out dx, al          */
    /* fill 16 KB of VRAM: ES = 0xB800, DI = 0, CX = 8192, AX = pattern */
    0xB8, 0x00, 0xB8,             /* mov ax, 0xB800      */
    0x8E, 0xC0,                   /* mov es, ax          */
    0x31, 0xFF,                   /* xor di, di          */
    0xB9, 0x00, 0x20,             /* mov cx, 8192        */
    0xB8, 0x1B, 0x6C,             /* mov ax, 0x6C1B      */
    0xF3, 0xAB,                   /* rep stosw           */
    /* speaker: 0xB6 to the PIT control port, divisor 0x0497 (~1 kHz) */
    0xB0, 0xB6,                   /* mov al, 0xB6        */
    0xE6, 0x43,                   /* out 0x43, al        */
    0xB0, 0x97,                   /* mov al, 0x97        */
    0xE6, 0x42,                   /* out 0x42, al        */
    0xB0, 0x04,                   /* mov al, 0x04        */
    0xE6, 0x42,                   /* out 0x42, al        */
    0xE4, 0x61,                   /* in  al, 0x61        */
    0x0C, 0x03,                   /* or  al, 3           */
    0xE6, 0x61,                   /* out 0x61, al        */
    0xEB, 0xFE,                   /* jmp $               */
};

/* Host-visible results, read over the debug backend exactly as in the
 * bare-metal harness so the same Python instruments work against both. */
/* Signature, not a flag. Until the firmware loads us, these addresses hold
 * whatever the firmware left in RAM_EMU, so a "non-zero means ready" test
 * latches onto garbage. The host waits for exactly this value. */
#define CORE_MAGIC 0x54494E59u   /* 'TINY' */
volatile uint32_t core_magic = 0;

/* ITCM integrity checkpoints.
 *
 * One byte of our loaded interpreter (offset 0x120, inside MEM_GET_PTR's BFI)
 * differs from the ELF on the target, and the CPU dies on UNDEFINSTR. These
 * bracket the suspects so the answer is measured rather than argued: if the
 * sum changes across init, our own startup did it; across the benchmark, the
 * interpreter did it to itself. */
#define ITCM_BASE   ((const volatile uint32_t *)0x00000000u)
#define ITCM_WORDS  (21832u / 4u)
volatile uint32_t itcm_sum_entry = 0;
volatile uint32_t itcm_sum_pre_bench = 0;
volatile uint32_t itcm_sum_post_bench = 0;
volatile uint32_t itcm_byte_120_entry = 0;
volatile uint32_t itcm_byte_120_pre = 0;
volatile uint32_t itcm_byte_120_post = 0;

static uint32_t itcm_sum(void)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < ITCM_WORDS; i++) {
        sum += ITCM_BASE[i];
    }
    return sum;
}

static uint32_t itcm_byte_120(void)
{
    return *(const volatile uint8_t *)0x00000120u;
}
volatile uint32_t core_ready = 0;
volatile uint32_t core_dtcm_ok = 0;
volatile uint32_t bonus_pool_base = 0;   /* AXI reclaimed by running LUT8 */
volatile uint32_t bonus_pool_size = 0;
volatile uint32_t benchmark_cycles = 0;
volatile uint32_t benchmark_insns = 0;
volatile uint32_t run_benchmark_flag = 0;

#define DWT_CTRL        (*(volatile uint32_t *)0xE0001000)
#define DWT_CYCCNT      (*(volatile uint32_t *)0xE0001004)
#define CoreDebug_DEMCR (*(volatile uint32_t *)0xE000EDFC)

/*
 * THE TOP OF THE DTCM BUMP POOL -- WHICH IS NOT THE TOP OF DTCM.
 *
 * This read 0x20020000 (0x20000000 + 128 KB) and was WRONG BY 24,608 BYTES,
 * and the error was not academic: it is the number a task was planned against.
 * `core_dtcm_headroom_max` reported 87,844 free, the 65,536-byte VGA aperture
 * was moved here on the strength of it, and dtc_malloc() returned NULL --
 * because the firmware's pool ends at `__dtc_padding_end__`, not at the end of
 * the RAM:
 *
 *   ._dtcm_free : __dtc_padding_end__ = ORIGIN(DTCMRAM) + 128K
 *                                       - _Min_Stack_Size      (24K - 32)
 *                                       - _Stack_Redzone_Size  (32)
 *   references/game-and-watch-retro-go-sd/STM32H7B0VBTx_SDCARD.ld:462-471
 *
 * so the pool tops out at 0x2001A000, confirmed against the firmware ELF
 * (`__dtc_padding_end__` = 2001a000). The 24 KB above it is the MAIN STACK and
 * its redzone; handing it out would corrupt the machine rather than fail.
 *
 * The name still says `_max`, because it still is a bound and not an exact
 * figure -- the ABI has no GW_MEM_OP_FREE_SIZE for DTCM ("RAM only today") --
 * but it is now a bound that a caller can plan against instead of one that
 * over-promises by a stack.
 */
#define DTCM_TOP 0x2001A000u        /* __dtc_padding_end__, NOT 0x20020000 */
/* Where the DTCM bump pool had got to after the core's own allocations, and an
   UPPER BOUND on what is left above it. See dtcm_state_init(). */
volatile uint32_t core_dtcm_next = 0;
volatile uint32_t core_dtcm_headroom_max = 0;
/* Alignment padding the mode records actually paid. Was an unconditional 8,192
   before they were allocated first; a non-zero value here means the firmware
   allocated DTCM ahead of us and the saving is smaller than expected. */
volatile uint32_t core_dtcm_mode_pad = 0;

/* Allocate the 0-wait-state state from the firmware's DTCM bump pool. */
static bool dtcm_state_init(void)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();

    /*
     * MODE RECORDS FIRST, AND EXACTLY. They are the one tenant with an
     * alignment requirement (8 KB, which the 66/67 EOR toggles depend on), and
     * the old order paid for it twice: allocated LAST of the three at an
     * arbitrary bump position, over-allocated by a whole 8 KB record so the
     * base could be aligned INSIDE the block, and the padding was then dead
     * for the session -- 8,192 bytes of a 106,496-byte pool, unconditionally.
     *
     * The bump pool's base is 8 KB-aligned, so asking FIRST usually needs no
     * padding at all. "Usually" is not an invariant, though -- the firmware
     * may have allocated before us -- so PROBE the pointer with a 1-byte ask
     * and pay only the padding that is really needed. Same trick the guest-RAM
     * pool uses for the same reason: a bump allocator will not tell you where
     * it is, but it will hand you a byte.
     *
     * This is what funds the VGA aperture below. It was 2,300 bytes short.
     */
    /*
     * DO NOT PROBE FOR THE ALIGNMENT. A 1-byte probe CONSUMES the aligned
     * position it was meant to discover: the pool base is 8 KB-aligned, the
     * probe takes byte 0, and the records then need 8,191 bytes of padding to
     * reach the next boundary. Measured exactly that -- core_dtcm_mode_pad
     * came back 8191 and the saving was ONE BYTE.
     *
     * Ask for the exact size and CHECK what came back instead. Being first,
     * the answer is the pool base, which is aligned, so the common path pays
     * nothing. Only if the firmware allocated ahead of us is the aligned
     * ask needed, and only then is a block wasted.
     */
    uint8_t *mr = (uint8_t *)abi->mem_ctl(
        GW_MEM_OP_ALLOC, GW_MEM_DTC, 1, X86_MODE_RECORDS_BYTES);
    if (mr && ((uintptr_t)mr & (X86_MODE_RECORDS_ALIGN - 1u)) != 0u) {
        /* Not aligned: the first block is unusable and stays leaked. Take the
           over-allocated one and align inside it, which is the old behaviour. */
        core_dtcm_mode_pad = X86_MODE_RECORDS_ALIGN;
        mr = (uint8_t *)abi->mem_ctl(
            GW_MEM_OP_ALLOC, GW_MEM_DTC, 1,
            X86_MODE_RECORDS_BYTES + X86_MODE_RECORDS_ALIGN);
    } else {
        core_dtcm_mode_pad = 0;
    }

    X86CPUState *st = (X86CPUState *)abi->mem_ctl(
        GW_MEM_OP_ALLOC, GW_MEM_DTC, 1, sizeof(X86CPUState));
    /* The full index range, not just the mapped pages: the 12-bit lookup can
     * produce any of FOLD_ENTRIES, and a short table is read past its end. */
    uint32_t *pt = (uint32_t *)abi->mem_ctl(
        GW_MEM_OP_ALLOC, GW_MEM_DTC, FOLD_ENTRIES, sizeof(uint32_t));

    if (!st || !pt || !mr) {
        return false;   /* loud, not silent: core_dtcm_ok stays 0 */
    }

    cpu_state_ptr = st;
    guest_page_table_ptr = pt;
    x86_build_mode_records(mr);

#if X86_PROFILE
    /* Profiling build only. DTCM is a firmware bump pool here, not a load
       target, so the block is allocated like everything else -- and a failure
       must reach core_dtcm_ok, because the dispatch tail dereferences
       x86_profile_ptr on EVERY instruction. */
    if (!x86_profile_init((void *)abi->mem_ctl(GW_MEM_OP_ALLOC, GW_MEM_DTC, 1,
                                               X86_PROFILE_BYTES))) {
        return false;
    }
#endif
#if X86_ZONES
    /* ---- ZONES ---- Same pattern, same reason: DTCM is a firmware bump pool
       in a core, so the block is allocated rather than placed. A failure here
       is NOT fatal -- x86_zones_ptr stays NULL and every macro turns into a
       null test -- but it is reported, because an instrument that silently
       measures nothing is worse than one that is absent. */
    zones_block_ok = x86_zones_init((void *)abi->mem_ctl(
        GW_MEM_OP_ALLOC, GW_MEM_DTC, 1, sizeof(x86_zones_t))) ? 1 : 0;
#endif


    /* The pager's per-page state: 8 KB, read on every fault, so DTCM like the
       fold table it sits beside. A failure here is not fatal -- the pager
       simply owns nothing and every guest page stays unmapped -- but it would
       be a guest that cannot run, so it reaches core_dtcm_ok. */
    if (x86_pager_set_tables((void *)abi->mem_ctl(GW_MEM_OP_ALLOC, GW_MEM_DTC,
                                                  1, X86_PG_TABLE_BYTES),
                             X86_PG_TABLE_BYTES) != 0) {
        return false;
    }

    /*
     * ---- THE VGA APERTURE, IN DTCM --------------------------------------
     *
     * 64 KB of real storage behind guest 0xA0000-0xAFFFF. It used to come out
     * of the 143,360-byte AXI arena, and it was the single largest thing in
     * there; moving it here does two things at once.
     *
     * 1. The planar path is the hottest data path a graphics title has. Every
     *    guest store into a planar mode reads and writes this block, and the
     *    blitter reads all 64,000 displayed bytes every frame. DTCM is
     *    0-wait-state and, unlike AXI, is not shared with the LTDC's scanout
     *    traffic. The CODE half of this landed at 486332a (planar_store and
     *    vga_cell pinned to ITCM); this is the DATA half.
     *
     * 2. It hands 65,536 bytes back to the arena, which is what pays for a
     *    second Mode Y real page WITHOUT donating any pager frames. See
     *    planar_storage_follow() in x86_video.c.
     *
     * The FOLD SENTINEL IS UNAFFECTED, and that is a correctness precondition,
     * not a hope: X86_FOLD_VGA_OK(host) is (host & 0xE0000000) == 0x20000000,
     * which DTCM at 0x20000000 satisfies exactly as AXI at 0x24000000 does.
     * Arming refuses the sentinel outright on any other region, so a mistake
     * here would be loud rather than subtly wrong.
     *
     * IT DOES NOT FIT TODAY, AND THE ARITHMETIC IS WHY -- MEASURED, tier 2,
     * with x86_vga_aperture_dtcm_fail reading 1 on every boot:
     *
     *   pool        0x20000000 .. 0x2001A000   106,496 B  (DTCM_TOP above)
     *   core takes                              43,228 B  (core_dtcm_next)
     *     X86CPUState             2,268
     *     fold table             16,384   R9, every memory access
     *     mode records           16,384   R2, every dispatch (8,192 + align)
     *     pager page table        8,192   read on a FAULT, not on an access
     *   left                                    63,268 B
     *   wanted                                  65,568 B
     *   SHORT BY                                 2,300 B
     *
     * The plan this was written to said 87,844 B were free "with ~22 KB to
     * spare". That figure was core_dtcm_headroom_max measured against a
     * DTCM_TOP of 0x20020000, which is the end of the RAM and not the end of
     * the POOL -- see DTCM_TOP. The 24 KB it over-promised is the main stack.
     *
     * The only tenant here that is not on a per-access path is the 8,192-byte
     * PAGER PAGE TABLE, and evicting it would leave 71,460 and fit this with
     * 5,892 to spare. It is not done here because the memory it would move to
     * is not free either: RAM_EMU costs two pager frames (x86_pg_frames_total
     * 160 -> 158) and the AHB heap's free space is not knowable from a link
     * map. That is a trade for the owner, not a side effect of this comment.
     *
     * THE COST IF IT EVER DOES FIT, STATED PLAINLY: the DTCM pool is a BUMP
     * ALLOCATOR WITH NO FREE, so the 64 KB would be gone for the whole session
     * -- taken even by a text-mode title that will never set a graphics mode.
     * Making it lazy buys nothing: a bump pool cannot take the memory back
     * when the guest leaves the mode either, so laziness only moves the
     * failure somewhere harder to read.
     *
     * Over-allocated by 32 bytes so the block can be aligned up. Alignment is
     * not required for correctness -- x86_map_fixed() builds each granule's k
     * by arithmetic and DTCM has no cache lines to straddle -- but a 4-byte
     * block already cost this project a pager frame once (STATUS.md), and 32
     * bytes is not worth the doubt.
     *
     * THE ASK IS KEPT, not deleted. It costs one failed call at init, it is
     * the thing that made the headroom bug visible at all, and it starts
     * working by itself the day a tenant moves or the firmware's stack reserve
     * shrinks. A NULL IS NOT FATAL: dtcm_aperture stays NULL, the caller falls
     * back to the arena claim, and x86_vga_aperture_dtcm_fail says it
     * happened.
     */
    {
        uint8_t *ap = (uint8_t *)abi->mem_ctl(
            GW_MEM_OP_ALLOC, GW_MEM_DTC, 1,
            X86_VGA_APERTURE_BYTES + X86_VGA_APERTURE_ALIGN);
        if (ap) {
            dtcm_aperture = (uint8_t *)(uintptr_t)
                (((uint32_t)(uintptr_t)ap + X86_VGA_APERTURE_ALIGN - 1u)
                 & ~(uint32_t)(X86_VGA_APERTURE_ALIGN - 1u));
        } else {
            dtcm_aperture = NULL;
            x86_vga_aperture_dtcm_fail = 1;
        }
    }

    /*
     * A WITNESS FOR DTCM, because there is not one anywhere else.
     *
     * GW_MEM_OP_FREE_SIZE is documented "RAM only today", so the ABI cannot be
     * asked how much DTCM is left -- and the pager about to land here wants
     * ~8 KB of per-page metadata on top of the 16 KB fold table. Spending it
     * blind is exactly the shape that has cost this project days: a bump
     * allocator returns NULL, a caller tolerates it, and a whole subsystem is
     * quietly absent.
     *
     * So: allocate a 4-byte marker LAST and publish where it landed. DTCM is
     * 128 KB at 0x20000000, the pool bumps upward, and the firmware's own
     * state sits below us, so (top - marker) BOUNDS what remains. It is a
     * bound and not an exact figure -- the firmware reserves a stack up there
     * and this cannot see it -- which is why the name says so.
     */
    {
        uint32_t *marker = (uint32_t *)abi->mem_ctl(GW_MEM_OP_ALLOC,
                                                    GW_MEM_DTC, 1, 4);
        core_dtcm_next = (uint32_t)(uintptr_t)marker;
        core_dtcm_headroom_max =
            (marker && (uint32_t)(uintptr_t)marker < DTCM_TOP)
                ? DTCM_TOP - (uint32_t)(uintptr_t)marker : 0;
    }
    return true;
}

/* Same synthetic benchmark as tests/main.c: 500,000 iterations of
 * DEC EAX / JNZ, then HLT. Exactly 1,000,001 instructions. */
static void run_benchmark(void)
{
    memset(cpu_state_ptr, 0, sizeof(X86CPUState));
    for (int i = 0; i < 6; i++) {
        cpu_state_ptr->segs[i].limit = 0xFFFFFFFF;
    }
    /* CS.D: this benchmark's guest code is 32-bit. A zeroed struct means a
       16-bit code segment, which is now genuinely different. */
    cpu_state_ptr->segs[1].flags |= X86_DESC_D_FLAG;

    /* guest_ram_bytes, not GUEST_RAM_SIZE: the pool is sized at runtime. */
    memset(guest_ram, 0, guest_ram_bytes);
    guest_ram[0] = 0x48;   /* DEC EAX      */
    guest_ram[1] = 0x75;   /* JNZ  -3      */
    guest_ram[2] = 0xFD;
    guest_ram[3] = 0xF4;   /* HLT          */

    /* Unmapped everywhere, then the guest RAM window. Entries are offsets:
     * host = guest_linear + k. Anything outside the window now faults with
     * X86_EXIT_MEM_FAULT instead of writing into our own ITCM code. */
    for (uint32_t i = 0; i < FOLD_ENTRIES; i++) {
        guest_page_table_ptr[i] = FOLD_INVALID;
    }
    for (uint32_t i = 0; i < guest_ram_pages; i++) {
        guest_page_table_ptr[i] = FOLD_K((uint32_t)guest_ram + i * 4096, i);
    }

    /* Hold the PIT off so the guest loop runs to its own HLT and the
     * instruction count is real rather than a host-side assumption. */
    cpu_state_ptr->pit_reload = 0xFFFFFFFF;
    cpu_state_ptr->pit_counter = 0xFFFFFFFF;
    cpu_state_ptr->regs[0] = 500000;   /* EAX: iterations */
    cpu_state_ptr->regs[4] = 0x3000;   /* ESP             */

    uint32_t start = DWT_CYCCNT;
    x86_cpu_interp_asm();
    uint32_t end = DWT_CYCCNT;

    benchmark_cycles = end - start;
    benchmark_insns = 2u * (500000u - cpu_state_ptr->regs[0]) + 1u;
}

void app_main(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state; (void)start_paused; (void)save_slot;

    ram_start = (uint32_t)&__CORE_BSS_END__;
    core_magic = CORE_MAGIC;      /* we are alive and this is our RAM_EMU */
    itcm_sum_entry = itcm_sum();
    itcm_byte_120_entry = itcm_byte_120();
    odroid_system_init(APP_ID, SAMPLE_RATE);

    CoreDebug_DEMCR |= (1 << 24);
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1;

    core_dtcm_ok = dtcm_state_init() ? 1 : 0;

    /* VPROF needs no allocation -- its accumulators are BSS -- but it does
       need its own cost measured on THIS target at THIS clock, and it must be
       measured AFTER DWT_CTRL's counter is enabled above or every bracket
       reads zero and the instrument silently reports a free video path. */
    x86_vprof_calibrate(1000u);

    const gw_firmware_abi_t *abi = gw_firmware_abi();

    /* Calibrate DWT cycles against the firmware's millisecond counter so the
       disk timers report microseconds without anyone hardcoding a core clock.
       An overclocked device changes that number and nothing here should have
       to be rebuilt for it. Measured over a whole tick edge so the ms counter's
       own granularity does not dominate. */
    {
        uint32_t t = abi->HAL_GetTick();
        while (abi->HAL_GetTick() == t) { }        /* align to a tick edge */
        uint32_t c0 = DWT_CYCCNT, m0 = abi->HAL_GetTick();
        while ((uint32_t)(abi->HAL_GetTick() - m0) < 4u) { }
        uint32_t per_us = (DWT_CYCCNT - c0) / (4u * 1000u);
        /* A stopped DWT (or a host that does not model it) must not turn every
           later division into a fault or a nonsense number. */
        x86_disk_cycles_per_us = per_us ? per_us : 1u;
    }

    /* Run the LCD in LUT8 so the upper half of the 300 KB framebuffer pool is
     * released as a bonus pool. This is where the AXI that docs/01 budgets as
     * "~870 KB" actually comes from -- RAM_EMU alone is only 724 KB. */
    abi->lcd_setup_framebuffers(LCD_MODE_LUT8);

    uint8_t *bonus = NULL;
    size_t bonus_sz = 0;
    abi->lcd_get_bonus_pool(&bonus, &bonus_sz);
    bonus_pool_base = (uint32_t)bonus;
    bonus_pool_size = (uint32_t)bonus_sz;

    /*
     * VRAM from the BONUS POOL, not RAM_EMU -- and this reversed a decision
     * that was made on a false premise.
     *
     * It used to say "the bonus region is uncached, and the blitter reads all
     * of VRAM every frame". MEASURED against the firmware source, that is not
     * true: mpu_set_lcd_pool_uncached_range() covers exactly the framebuffer
     * footprint (154 KB in LUT8) and its own comment says everything past it
     * "defaults to Normal cacheable, which is what we want for the LCD bonus
     * area". The uncached window is the two framebuffers; the bonus area above
     * them is cached AXI, the same memory class RAM_EMU is.
     *
     * So this costs nothing and buys 32,768 bytes of RAM_EMU, which is the
     * region that is actually scarce -- headroom past __CORE_BSS_END__ went
     * NEGATIVE (-2,024 B) when mode 13h landed, and the only symptom of that
     * is core_ready staying 0 with no message.
     *
     * Ordering matters: lcd_setup_framebuffers() above zeroes the pool and rewrites the
     * MPU regions, so nothing may be placed in the bonus area before it.
     */
    /* The arena gets the whole bonus pool between the two fixed 4 KB pages and
       the palette at the top: 143,360 bytes, 35 whole pages. Both ends are
       page-aligned because bonus_layout() derived them from the pool's real
       base rather than from a compile-time offset -- which is what recovers
       the page a 2048-aligned base would otherwise cost. */
#if X86_VIDEO_DMA2D
    /* Offered, not imposed: the hook itself refuses anything it cannot encode
       and vga13_blit falls back to the row loop, so an ABI or a peripheral that
       will not start costs one branch a frame rather than a black screen. */
    x86_video_blit_hook = dma2d_blit_hook;
    dma2d_selftest();
#endif

    bonus_layout(bonus, (uint32_t)bonus_sz);
    x86_arena_reset();
    if (bonus && bonus_arena_bytes)
        x86_arena_add_region(bonus + bonus_arena_off, bonus_arena_bytes);

    /* CGA VRAM is a CLAIM now, one block of 32 KB. The fallback is unchanged
       in spirit -- a core with no bonus pool still gets VRAM out of RAM_EMU --
       but it goes through the arena so there is one accounting of display
       storage rather than two that can disagree. */
    {
        void *blk = NULL;
        if (x86_arena_claim(X86_ARENA_ID_CGA, 1, X86_VRAM_BYTES, &blk) == 0)
            guest_vram = (uint8_t *)blk;
        else
            guest_vram = (uint8_t *)abi->mem_ctl(GW_MEM_OP_ALLOC, GW_MEM_RAM,
                                                 1, X86_VRAM_BYTES);
    }
    guest_vram_ok = guest_vram ? 1 : 0;

    /* Guest RAM is a pool of 4 KB frames, so the block must be 4 KB ALIGNED --
       ram_malloc() only aligns to 4, and MEASURED on target an unaligned block
       cost a whole frame (x86_pg_frames_total read 159 instead of 160).

       It used to over-allocate a full spare page and align up inside it. That
       page is no longer affordable: upstream's SPI1 SD DMA (5e19e392e) takes
       SPI_DMA_BLOCK*2+31 = 1,055 bytes of RAM_EMU on the first sector read,
       which happens while the CORE ITSELF is being loaded -- before any of our
       code runs -- and this request is the last big claim in a bump pool with
       ~660 KB in it. That left us 488 bytes short and guest_ram came back NULL:
       core_ready 0, black screen, no error. So PAY ONLY THE REAL PADDING: one
       1-byte probe reveals where the bump pointer is, and the pad after it is
       whatever alignment actually costs (792 bytes as measured, not 4,096).

       AND IT IS NOW SIZED TO WHAT IS THERE, NOT TO WHAT WE WANT.
       ------------------------------------------------------------------
       All-or-nothing was the second half of the same bug. Any instrument that
       adds a few kilobytes of RAM_EMU .bss -- X86_PROBE=1 costs ~2.8 KB --
       pushes this, the LAST claim in the pool, past the free space. The
       full-size request then returned NULL, x86_pg_frames_total was 0, every
       guest access was X86_EXIT_MEM_FAULT and the guest halted before its
       first instruction, while core_dtcm_ok, pgf_result and x86_xms_total_kb
       all still read healthy. probe_report.py duly reported "every service
       this title touched was answered" about a guest that had never run.

       So ASK how much is free and take what fits. A pool one frame short of
       160 is an honest, measurable degradation -- the pager evicts to the
       pagefile, which is what it is for. ZERO frames is a lie. The shortfall
       is published in guest_ram_pages_short, which is the number to read when
       an instrumented build behaves differently from the shipping one.

       GW_MEM_OP_FREE_SIZE is documented "RAM only today" and RAM is exactly
       the pool this claim comes from, so it is answerable here (unlike DTCM).
       A zero answer from an older firmware falls through to asking for the
       full size -- i.e. precisely the old behaviour. */
    {
        uint8_t *probe = (uint8_t *)abi->mem_ctl(GW_MEM_OP_ALLOC,
                                                 GW_MEM_RAM, 1, 1);
        uint8_t *raw = NULL;
        uint32_t pages = GUEST_RAM_SIZE / 4096u;
        if (probe) {
            /* ram_malloc() rounds its bump up to 4, so the next block starts
               here; pad that up to the frame boundary and take it. */
            uint32_t next = ((uint32_t)(uintptr_t)probe + 1u + 3u) & ~3u;
            uint32_t pad  = (0u - next) & 4095u;
            if (!pad || abi->mem_ctl(GW_MEM_OP_ALLOC, GW_MEM_RAM, 1, pad)) {
                /* Read AFTER the pad, so it is the space this request can
                   really have rather than an over-estimate by up to a frame. */
                uint32_t freeb = (uint32_t)abi->mem_ctl(GW_MEM_OP_FREE_SIZE,
                                                        GW_MEM_RAM, 0, 0);
                guest_ram_free_at_claim = freeb;
                if (freeb && freeb < X86_PG_POOL_BYTES(pages))
                    pages = freeb / X86_PG_STRIDE;
                if (pages)
                    raw = (uint8_t *)abi->mem_ctl(
                        GW_MEM_OP_ALLOC, GW_MEM_RAM, 1,
                        X86_PG_POOL_BYTES(pages));
            }
        }
        /* The align-up is a no-op when the pad worked and the only recovery
           when it did not, so it stays. */
        guest_ram = raw ? (uint8_t *)(((uint32_t)(uintptr_t)raw + 4095u)
                                      & ~4095u) : NULL;
        guest_ram_pages = guest_ram ? pages : 0;
        guest_ram_pages_short = (GUEST_RAM_SIZE / 4096u) - guest_ram_pages;
    }
    guest_ram_bytes = guest_ram_pages * 4096u;

    /* The open-bus filler comes from the BONUS POOL, not from RAM_EMU.
     *
     * RAM_EMU headroom past our BSS is a few hundred bytes (see TOOLING.md),
     * and this page is the last thing allocated, so it is the first casualty
     * of any growth in the core -- silently, because the region it backs is
     * only ever PROBED by option-ROM scans. The bonus pool is 150 KB of AXI we
     * claim and do not use, and uncached is irrelevant for a page that is
     * written once and read rarely.
     *
     * ram_malloc() returns NULL rather than overrunning, so a failure here is
     * a missing mapping, not corruption -- but it must still be visible. */
    if (bonus && bonus_sz >= bonus_filler_off + 4096u)
        upper_filler = bonus + bonus_filler_off;
    else
        upper_filler = (uint8_t *)abi->mem_ctl(GW_MEM_OP_ALLOC, GW_MEM_RAM,
                                               1, 4096);
    upper_filler_ok = upper_filler ? 1 : 0;

    /* The BIOS identification page at 0xFF000 needs REAL, PERSISTENT storage:
     * it is written once and read by every program that identifies the
     * machine, so it cannot alias the open-bus filler. It used to come from
     * x86_map_range()'s page pool -- but that pool is never initialised
     * (x86_mem_pool_init() is called from nowhere), so the request always
     * failed and the page was simply absent. From the bonus pool, like the
     * filler, for the same reasons. */
    if (bonus && bonus_sz >= bonus_biosid_off + 4096u)
        bios_id_page = bonus + bonus_biosid_off;
    else
        bios_id_page = (uint8_t *)abi->mem_ctl(GW_MEM_OP_ALLOC, GW_MEM_RAM,
                                               1, 4096);

    /*
     * The .dsk sector cache, from the AHB HEAP -- not from the bonus pool.
     *
     * MEASURED (tier 2, TOPBENCH): every disk_read is for ONE sector, and each
     * one costs the firmware an fopen + an fseek that walks the .dsk's cluster
     * chain from byte zero (FF_USE_FASTSEEK is 0) before a single byte of the
     * guest's data moves. The cost is per-CALL, so the fix is to make fewer,
     * larger calls -- which is what a window of read-ahead is.
     *
     * AHB (GW_MEM_AHB) is D2 SRAM. This is a disk staging buffer, and D2 is
     * the only SRAM DMA1/DMA2 can reach, so it is where the buffer must live
     * if SPI1's polled transfer ever becomes a DMA one -- which is the
     * asynchronous-completion fix STATUS lists as the deeper answer to the
     * stutter. It is also the ONLY ABI pool with a real free(); ITC/RAM/DTC
     * are bump allocators that can never hand anything back.
     *
     * x86_disk_cache_ok is the witness: a refused allocation here is not a
     * fault, the reads simply go straight to the card again, and the ONLY
     * symptom is the stutter this was built to remove. That is precisely the
     * silent-allocation-failure shape this project keeps getting bitten by --
     * and the AHB heap is SHARED with the firmware's own newlib allocations,
     * so unlike a bump pool its free space is not knowable from the link map.
     */
    {
        /* DEGRADE, do not vanish. The full size failed on the device -- the AHB
           heap is shared with the firmware's newlib, so 64 KB was simply not
           there -- and a NULL here disables the cache entirely, which is SLOWER
           than before it existed: every read goes to the backend and re-walks
           the file. Halve until something fits; a 16 KB cache is worth far more
           than none. x86_disk_cache_bytes says what we actually got. */
        void *dc = NULL;
        uint32_t want = X86_DISK_CACHE_BYTES;
        while (!dc && want >= 8192u) {
            dc = (void *)abi->mem_ctl(GW_MEM_OP_ALLOC, GW_MEM_AHB, 1, want);
            if (!dc)
                want /= 2u;
        }
        disk_cache_mem = (uint32_t)(uintptr_t)dc;
        x86_disk_cache_bytes = dc ? want : 0;
        x86_disk_cache_init(dc, dc ? want : 0);
    }

    /*
     * The VGA mode 13h aperture: 64 KB of REAL storage behind guest
     * 0xA0000-0xAFFFF, from the bonus pool -- the 64 KB the sector cache just
     * vacated, plus room to spare.
     *
     * It must be AXI and it must be CACHED. The blitter reads all 64,000
     * displayed bytes every frame and the guest writes them a byte at a time,
     * and references/8086tiny/docs/video/12-vga13-direct-scanout.md prices
     * emulated VRAM behind a strongly-ordered region at ~5.4-6.5 cycles per
     * guest byte store -- 4x the blit it would save. The bonus pool qualifies:
     * the firmware's mpu_set_lcd_pool_uncached_range() covers only the 154 KB
     * LUT8 framebuffer footprint and leaves the bonus area Normal cacheable,
     * in those words. (The older comment above about VRAM avoiding the bonus
     * pool because it is "uncached" was mistaken; it is kept in RAM_EMU only
     * because it already fits there.)
     *
     * 64 KB, not 64,000: 0xA0000-0xAFFFF is sixteen whole 4 KB fold pages, so
     * the mapping needs no partial page and no special case against the
     * open-bus filler that aliases the rest of the upper megabyte.
     */
    /* Per-BLOCK guards, not one guard on the whole map: the palette now sits
       at the top of the pool, so testing against BONUS_USED would refuse the
       aperture on any firmware whose bonus pool came back a byte short. Each
       block is placed if its own extent fits and reported if it does not. */
    /* DTCM FIRST, arena as the fallback. dtcm_state_init() took the block; if
       the pool refused it, x86_vga_aperture_dtcm_fail is already 1 and the
       arena claim below is the old behaviour, unchanged -- a slower aperture
       and one fewer Mode Y page, but a picture. */
    if (dtcm_aperture) {
        vga_aperture = dtcm_aperture;
        x86_vga_aperture_dtcm = 1;
    } else {
        void *blk = NULL;
        vga_aperture = (x86_arena_claim(X86_ARENA_ID_VGA13, 1,
                                        X86_VGA_APERTURE_BYTES, &blk) == 0)
                       ? (uint8_t *)blk
                       : NULL;          /* mode 13h refuses itself; see below */
    }
    vga_aperture_ok = vga_aperture ? 1 : 0;

    if (guest_vram) {
        /* The CLUT the LCD scans out through. LUT8 gives 256 entries; CGA
         * mode 4 uses four and the palette registers pick which. */
        /* ARGB8888; lcd_set_clut() caps the count at 32 and reads 32-bit
         * entries. All THIRTY-TWO are pushed, not four and not sixteen.
         *
         * Mode 4 uses only entries 0-3, but text attributes index the whole
         * CGA palette, and pushing four left 4-15 holding whatever retro-go's
         * menu had -- so every bright or coloured attribute drew in a stale
         * colour or in black.
         *
         * Sixteen is not enough either, for a reason that is invisible until
         * someone opens retro-go's menu: lcd_set_clut() writes a darkened twin
         * of entry i at `count + i`, but the overlay reaches those twins by
         * OR-ing LCD_DARKEN_BIT = 0x20 into each framebuffer byte, a fixed
         * +32. The two only agree at count == 32. At 16 the twins sat at 16-31
         * while the overlay read 32-47, which are zeros, so the menu dimmed
         * the game to solid black. Pushing 32 also gives the anti-aliased text
         * renderer the eight four-entry blocks it allocates per frame. */
        x86_video_init(guest_vram, core_clut);
        /* The mode 13h aperture and its palette block, both out of the bonus
           pool. NULL is legal and means 13h refuses itself loudly rather than
           the guest drawing into the open-bus filler. */
        x86_video_set_aperture(vga_aperture,
                               vga_aperture ? bonus + BONUS_VGAPAL_OFF : NULL);
        abi->lcd_set_clut(core_clut, CORE_CLUT_ENTRIES);
        cga_clut_dirty = 0;
    }

    core_ready = (core_dtcm_ok && guest_ram && guest_vram) ? 1 : 0;
#if X86_OPL_BENCH
    /* MEASUREMENT BUILD ONLY: hold the FM chip at its worst case for the whole
       run so x86_opl_frame_cycles is a BOUND and not a sample of whatever the
       title happened to be playing. Never ship at 1 -- it drowns the title. */
    x86_opl_bench_all_on();
#endif

    itcm_sum_pre_bench = itcm_sum();
    itcm_byte_120_pre = itcm_byte_120();

    /* The benchmark is NOT run automatically. It faults, the device reboots,
     * and the reboot wipes the very globals we need to read -- so polling from
     * the host races a reset. Staying alive here makes the checkpoints
     * readable; the host triggers the run deliberately via run_benchmark_flag. */

    mount_guest_disk();
    /* BEFORE emulator_loop(), which calls cpu_profile_init() and then
       guest_reset() -- and guest_reset() is where the XMS and EMS backing is
       chosen and the first slice is sized. A setting applied after the machine
       is up takes effect at some later, unpredictable moment; see the comment
       on settings_load(). It needs dsk_path, so it cannot move above
       mount_guest_disk(), and it must not move below emulator_loop(), which
       never returns. */
    settings_load();
    emulator_loop(abi);
}

/*
 * Set the guest up in real mode with conventional RAM mapped flat, VRAM at the
 * video aperture, and the demo program at 0000:0100.
 */
/*
 * Sector reads for our FatFs, served straight off the SD card.
 *
 * rg_storage_copy_file_range_to_ram reads a byte range of a file, which is
 * exactly a sector fetch, and it holds no handle open between calls -- so the
 * 1.44 MB image costs no AXI at all and any .dsk in /roms/dos works without a
 * rebuild.
 */
/*
 * THE .dsk HANDLE IS HELD OPEN. This used to be
 * abi->rg_storage_copy_file_range_to_ram, which opens and closes the image on
 * EVERY call -- and x86_disk.c already recorded what that costs in as many
 * words: f_open is a directory walk with LFN on, f_lseek with FF_USE_FASTSEEK 0
 * walks the cluster chain from byte zero reading FAT sectors, and only then are
 * the bytes the guest asked for read.
 *
 * It is the same correction the pagefile took, for the same reasons and with
 * the same evidence behind it (see the pagefile header, and `xms_opens` as its
 * witness): FF_FS_TINY 1 means a FIL carries no private sector window, so a
 * held handle costs the FIL struct and nothing else, and FF_FS_LOCK 0 means
 * nothing was refusing the duplicate open this path took while the WRITE path
 * below had its own open on the same file.
 *
 * dsk_us_open / dsk_us_seek / dsk_us_read split the cost the way pgf_us_* does
 * for the pagefile, so "which of the three is it" stays a measurement.
 */
static uint32_t pgf_us(uint32_t cycles);
static void pgf_acc(volatile uint32_t *total, volatile uint32_t *worst,
                    uint32_t cycles);

volatile uint32_t dsk_opens        = 0;   /* ONE per title if this is working */
volatile uint32_t dsk_us_open      = 0;
volatile uint32_t dsk_us_seek      = 0;
volatile uint32_t dsk_us_read      = 0;
volatile uint32_t dsk_us_write     = 0;
volatile uint32_t dsk_seek_us_worst = 0;
volatile uint32_t dsk_read_us_worst = 0;
volatile uint32_t dsk_errors       = 0;

static FILE *dsk_fp;

static void dsk_close_file(void)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    if (dsk_fp) {
        X86_ZONE_ENTER(X86_ZONE_DISK);
        abi->fclose(dsk_fp);
        X86_ZONE_EXIT();
        dsk_fp = NULL;
    }
}

/* "r+b": ONE handle for reads and writes both. Two handles on a file one of
   them writes is exactly what FF_FS_LOCK 0 fails to refuse. */
static FILE *dsk_file(const char *path)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    if (dsk_fp)
        return dsk_fp;
    if (!path || !path[0])
        return NULL;
    X86_ZONE_ENTER(X86_ZONE_DISK);
    uint32_t t0 = DWT_CYCCNT;
    dsk_fp = abi->fopen(path, "r+b");
    if (!dsk_fp)                       /* a read-only card still has to boot */
        dsk_fp = abi->fopen(path, "rb");
    dsk_us_open += pgf_us(DWT_CYCCNT - t0);
    X86_ZONE_EXIT();
    if (dsk_fp) dsk_opens++;
    return dsk_fp;
}

static int sd_sector_read(void *ctx, void *dst, uint32_t offset, uint32_t len)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
#if X86_DSK_OPEN_PER_CALL
    /* THE A/B ARM, and the only reason it still exists: this is what the path
       did before the handle was held -- rg_storage_copy_file_range_to_ram,
       which f_opens and f_closes the image around every 512-byte sector. Build
       X86_DSK_OPEN_PER_CALL=1 to put the old cost back and read
       x86_disk_us_total against it. */
    X86_ZONE_ENTER(X86_ZONE_DISK);
    size_t got = abi->rg_storage_copy_file_range_to_ram(
        (char *)ctx, (uint8_t *)dst, offset, len, NULL);
    X86_ZONE_EXIT();
    if (got != len) dsk_errors++;
    return (got == len) ? 0 : -1;
#else
    FILE *f = dsk_file((const char *)ctx);
    int rc = -1;

    if (!f) { dsk_errors++; return -1; }
    /* ZONES: the DISK zone is the SD ROUND TRIP and nothing else. It nests
       inside whatever asked for it (normally HLE), so FatFs's own cluster-chain
       walk stays charged to HLE -- which is correct, that walk is CPU work. */
    X86_ZONE_ENTER(X86_ZONE_DISK);
    uint32_t t0 = DWT_CYCCNT;
    int sought = abi->fseek(f, (long)offset, SEEK_SET);
    uint32_t t1 = DWT_CYCCNT;
    pgf_acc(&dsk_us_seek, &dsk_seek_us_worst, t1 - t0);
    if (sought == 0 && abi->fread(dst, 1, len, f) == len)
        rc = 0;
    pgf_acc(&dsk_us_read, &dsk_read_us_worst, DWT_CYCCNT - t1);
    X86_ZONE_EXIT();
    if (rc != 0) dsk_errors++;
    return rc;
#endif
}

/*
 * The write side of the same aperture. There is no ranged SD write in the ABI,
 * so this goes through the firmware's stdio, opening and closing per call to
 * match the read path -- the core holds no handle on the .dsk between
 * requests, which is what lets any image in /roms/dos be swapped in.
 *
 * The guest's floppy really is written. That is what a floppy does: TOPBENCH
 * leaves a `topbench.log` on it and a game leaves its save, and telling the
 * guest a write succeeded while dropping it is the failure mode this project
 * refuses everywhere else.
 */
static int sd_sector_write(void *ctx, const void *src, uint32_t offset,
                           uint32_t len)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    FILE *f = dsk_file((const char *)ctx);
    int rc = -1;

    if (!f) { dsk_errors++; return -1; }
    X86_ZONE_ENTER(X86_ZONE_DISK);          /* ZONES: see sd_sector_read */
    uint32_t t0 = DWT_CYCCNT;
    if (abi->fseek(f, (long)offset, SEEK_SET) == 0 &&
        abi->fwrite(src, 1, len, f) == len)
        rc = 0;
    /* The guest was told the write succeeded, so it has to be on the card
       before the next thing that could read it -- the handle is shared with
       the read path above, and fflush is what makes the two agree through
       stdio's buffer. */
    if (rc == 0 && abi->fflush(f) != 0)
        rc = -1;
    dsk_us_write += pgf_us(DWT_CYCCNT - t0);
    X86_ZONE_EXIT();
    if (rc != 0) dsk_errors++;
    return rc;
}

/* ===========================================================================
 * THE PAGEFILE -- the pager's FILE tier, and the last missing link.
 *
 * x86_pager.h is explicit that this belongs HERE and not in the pager: the ABI
 * lives in src/main.c so the whole eviction mechanism can be driven at tier 1
 * against a plain RAM buffer (tests/main.c's pgf_read/pgf_write), with no card,
 * no FatFs and no firmware. This is the same two functions over real storage.
 *
 * `slot` IS the guest page number -- the mapping is direct, so there is no
 * allocator and no free list, and a slot is simply `slot * 4096` bytes into the
 * file. 640 KB of guest space is a 640 KB file at most, and it is sparse in
 * practice because only pages that were genuinely modified and then evicted are
 * ever written.
 *
 * ONE HANDLE, HELD OPEN, SHARED WITH XMS -- and that is a correction, not a
 * preference. This used to be OPEN-PER-CALL, on the stated grounds that
 * "holding a FILE* across a guest frame would pin a FatFs handle and its
 * sector buffer for the life of the title". Both halves of that were false on
 * this build:
 *
 *   - The sector buffer is not pinned. The firmware builds FatFs with
 *     FF_FS_TINY 1 (references/.../porting/lib/FatFs/ffconf.h:227), so a FIL
 *     carries NO private window -- it shares FATFS.win. A held handle costs
 *     the FIL struct and nothing else.
 *   - The handle was already held ANYWAY. xms_open() below has held a second
 *     FILE* on THIS EXACT FILE since the XMS work landed, and WOLF3D takes the
 *     XMS path unprompted. So the cost was being paid AND the per-slot fopen
 *     was being paid on top of it.
 *
 * That second point was also a live CORRECTNESS bug, not just a cost. The
 * firmware builds with FF_FS_LOCK 0 (ffconf.h:266), so nothing refuses a
 * duplicated open, and FatFs prohibits opening a file twice when either handle
 * writes. Every WOLF3D level load had the pager writing x86page.swp through
 * one handle while XMS held another open on it.
 *
 * What the open actually cost, and why it dominated a level load: fopen ->
 * f_open is a directory walk with LFN on; fseek -> f_lseek with
 * FF_USE_FASTSEEK 0 (ffconf.h:37) walks the cluster chain from byte zero,
 * reading FAT sectors as it goes; and fclose -> f_sync writes the FAT back and
 * updates the directory entry, which is two SD WRITES before the 4 KB of page
 * data is even counted. The 4 KB was never the expensive part.
 *
 * The same A/B was already run on the XMS side of this file and is recorded
 * below: fopen per operation cost WOLF3D 31% of its frame rate, and holding
 * the handle cut that to 4%. The pager was paying the identical toll 36 times
 * at a level load. x86_pg_wb_opens still counts writebacks and
 * x86_pg_wb_us_worst still says what one costs; `xms_opens` is now the witness
 * for the WHOLE file -- it is 1 for a run, and if it ever climbs with the
 * writeback count, this reverted.
 *
 * Ordering is unchanged and there is no queue: every writeback is still
 * synchronous and still completes before the frame it came from is reused, so
 * a page can never be read back before its write has landed. Reads and writes
 * share one handle, which is what makes that true through the buffering as
 * well -- a page written and immediately refilled is served from the same
 * FatFs window rather than from a stale sector under a second handle.
 *
 * Nothing needs an f_sync. The file is truncated at every guest reset and no
 * slot is ever read before this run wrote it, so what reaches the card after a
 * crash is not a property anything depends on.
 *
 * The file lives next to the .dsk, the one directory this core has demonstrated
 * it can write (the disk write-back path opens the image there "r+b" and the
 * NOR snapshot places its placeholder there). It is TRUNCATED at every guest
 * reset: slots are guest page numbers, so a previous title's page 37 would sit
 * exactly where this title's page 37 goes. Nothing can read it -- a page is
 * only fetched from FILE tier after this run wrote it -- but a stale 640 KB of
 * another game's memory on the card is not a thing to leave lying around.
 */
volatile uint32_t pgf_ready  = 0;   /* 1 once the file exists and is writable  */
volatile uint32_t pgf_result = 0;   /* why not, if not; see PGF_* below        */
volatile uint32_t pgf_reads  = 0;   /* slots read back                         */
volatile uint32_t pgf_writes = 0;   /* slots written                           */
volatile uint32_t pgf_errors = 0;   /* I/O that did not land                   */

#define PGF_OK          1
#define PGF_NO_PATH     2
#define PGF_NO_CREATE   3

static char pgf_path[sizeof(dsk_path) + 16];

/*
 * THE one handle on x86page.swp. The pager and XMS share it; see the header
 * above for why there is exactly one and not two, and why it is held.
 * `xms_opens` counts the fopen and is the witness that it stayed at one.
 */
static FILE *pgf_fp;
extern volatile uint32_t xms_opens;
extern volatile uint32_t xms_file_errors;
static void pgf_close_file(void);

/*
 * WHERE A PAGEFILE WRITE ACTUALLY SPENDS ITS TIME.
 *
 * x86_pg_wb_us_worst measured 433,509 us for ONE 4 KB write on silicon, against
 * 23,531 us for an ordinary sector read -- 20-80x, and GROWING over a session.
 * A single number cannot say which of fopen / fseek / fwrite that is, and the
 * three have completely different fixes (a held handle, the cluster-chain walk,
 * the card itself). These split it, so the next run localises it instead of
 * arguing about it.
 *
 * fseek is the one to read first: the firmware builds FF_USE_FASTSEEK 0, so
 * f_lseek walks the cluster chain from byte zero on EVERY call, and a seek past
 * EOF in write mode ALLOCATES the intervening clusters -- which is the only
 * candidate on the list that grows with offset and with session length.
 *
 * _total is what attribution needs (a worst-case cannot be divided into a frame
 * budget); _worst is what says whether the cost is a tail or a constant.
 */
volatile uint32_t pgf_us_open  = 0;
volatile uint32_t pgf_us_seek  = 0;
volatile uint32_t pgf_us_write = 0;
volatile uint32_t pgf_us_read  = 0;
volatile uint32_t pgf_seek_us_worst  = 0;
volatile uint32_t pgf_write_us_worst = 0;

/*
 * ALLOCATING vs IN-PLACE -- the split that says WHICH of the two candidate
 * stories is true, and it is the only one the totals above cannot separate.
 *
 * A write whose offset is at or past the file's high-water mark has to make the
 * cluster chain longer: create_chain() searches the FAT for a free cluster and
 * writes the FAT back, and when the offset is far past EOF f_lseek stretches
 * the chain over the WHOLE hole first. A write below the high-water mark
 * touches an already-allocated cluster and is just data.
 *
 * If the allocating writes are the expensive ones, the fix is to stop making
 * holes (a dense, append-ordered slot map). If both cost the same, the cost is
 * the card's random-write behaviour and no amount of layout will move it.
 */
volatile uint32_t pgf_ext_writes    = 0;   /* at/past the high-water mark */
volatile uint32_t pgf_ext_us        = 0;
volatile uint32_t pgf_ext_us_worst  = 0;
volatile uint32_t pgf_inp_writes    = 0;   /* below it -- pure data */
volatile uint32_t pgf_inp_us        = 0;
volatile uint32_t pgf_inp_us_worst  = 0;
volatile uint32_t pgf_high_water    = 0;   /* bytes, the largest offset+len */

extern volatile uint32_t x86_disk_cycles_per_us;

/* Cycles -> us against the SAME calibration x86_pg_wb_us_worst uses, so the
   split adds up to the total the pager reports rather than to a second scale. */
static uint32_t pgf_us(uint32_t cycles)
{
    uint32_t per_us = x86_disk_cycles_per_us;
    return per_us ? (cycles / per_us) : 0u;
}

static void pgf_acc(volatile uint32_t *total, volatile uint32_t *worst,
                    uint32_t cycles)
{
    uint32_t us = pgf_us(cycles);
    *total += us;
    if (worst && us > *worst)
        *worst = us;
}

/* The shared handle, opened on first use. NULL means the file is not usable;
   every caller must check, exactly as they checked the per-call fopen. */
static FILE *pgf_file(void)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    if (!pgf_ready)
        return NULL;
    if (pgf_fp)
        return pgf_fp;
    X86_ZONE_ENTER(X86_ZONE_DISK);
    uint32_t t0 = DWT_CYCCNT;
    pgf_fp = abi->fopen(pgf_path, "r+b");
    pgf_acc(&pgf_us_open, NULL, DWT_CYCCNT - t0);
    X86_ZONE_EXIT();
    if (!pgf_fp) { xms_file_errors++; return NULL; }
    xms_opens++;
    return pgf_fp;
}

#if X86_PGF_BENCH
/*
 * WHAT A 4 KB PAGEFILE WRITE IS MADE OF, measured against the card itself.
 *
 * The counters above already settled that it is NOT the cluster-chain walk:
 * KEEN4 on silicon wrote 86 pages of which 85 landed in already-allocated
 * clusters, and those still cost 47 ms mean / 405 ms worst while every seek
 * together came to 340 ms. So the cost is in f_write, and the only things that
 * can be varied from THIS side of the ABI are how many bytes per call and
 * where they land. These arms vary exactly that and nothing else.
 *
 * Source bytes come from intflash (0x08000000): 32 KB of guaranteed-readable
 * memory at a point in the boot where no buffer that big exists yet.
 *
 * ~1 s at title load, and it TRUNCATES the file again afterwards, so it can
 * only ever be a build flag.
 */
volatile uint32_t pgfb_grow4k    = 0;   /* 8 x 4 KB, each extending the file  */
volatile uint32_t pgfb_inplace4k = 0;   /* 8 x 4 KB, sequential, allocated    */
volatile uint32_t pgfb_inplace1k = 0;   /* 32 x 1 KB over the same 32 KB      */
volatile uint32_t pgfb_inplace512= 0;   /* 64 x 512 B over the same 32 KB     */
volatile uint32_t pgfb_inplace32k= 0;   /* 1 x 32 KB -- the batching question */
volatile uint32_t pgfb_random4k  = 0;   /* 8 x 4 KB, scattered, allocated     */
volatile uint32_t pgfb_read4k    = 0;   /* 8 x 4 KB reads, for scale          */
/* ORDERING CONTROLS. The first arm runs on a freshly truncated file with an
   idle card, so "extending is cheaper" could just as well be "first is
   cheaper". These repeat both shapes at the END, after ~2 s of traffic. */
volatile uint32_t pgfb_grow4k_late = 0;   /* 24 x 4 KB extending, run LAST    */
volatile uint32_t pgfb_inplace_late = 0;  /* 8 x 4 KB in-place, run last-1    */

#define PGFB_SRC ((const void *)0x08000000u)

static uint32_t pgf_bench_run(FILE *f, uint32_t off, uint32_t chunk,
                              uint32_t count, uint32_t stride, int read)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    static uint8_t sink[1024];
    uint32_t t0 = DWT_CYCCNT;
    for (uint32_t i = 0; i < count; i++) {
        if (abi->fseek(f, (long)(off + i * stride), SEEK_SET) != 0)
            break;
        if (read) {
            uint32_t left = chunk;
            while (left) {
                uint32_t n = left > sizeof(sink) ? sizeof(sink) : left;
                if (abi->fread(sink, 1, n, f) != n) break;
                left -= n;
            }
        } else if (abi->fwrite(PGFB_SRC, 1, chunk, f) != chunk) {
            break;
        }
    }
    return pgf_us(DWT_CYCCNT - t0);
}

static void pgf_bench(void)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    FILE *f = pgf_file();
    if (!f)
        return;
    X86_ZONE_ENTER(X86_ZONE_DISK);
    /* Grow to 96 KB first: every later arm then writes into allocated
       clusters, which is the case the KEEN4 measurement showed the pager is
       actually in, and the scattered arm has somewhere to scatter. */
    pgfb_grow4k     = pgf_bench_run(f, 0, 4096, 24, 4096, 0);
    pgfb_inplace4k  = pgf_bench_run(f, 0, 4096,  8, 4096, 0);
    pgfb_inplace1k  = pgf_bench_run(f, 0, 1024, 32, 1024, 0);
    pgfb_inplace512 = pgf_bench_run(f, 0,  512, 64,  512, 0);
    pgfb_inplace32k = pgf_bench_run(f, 0, 32768, 1, 32768, 0);
    pgfb_random4k   = pgf_bench_run(f, 0, 4096,  8, 4096u * 3u, 0);
    pgfb_read4k     = pgf_bench_run(f, 0, 4096,  8, 4096, 1);
    pgfb_inplace_late = pgf_bench_run(f, 0, 4096, 8, 4096, 0);
    pgfb_grow4k_late  = pgf_bench_run(f, 98304, 4096, 24, 4096, 0);
    X86_ZONE_EXIT();

    /* Put the file back to empty -- the guest must not inherit any of this. */
    pgf_close_file();
    FILE *t = abi->fopen(pgf_path, "wb");
    if (t) abi->fclose(t);
    pgf_reads = pgf_writes = pgf_errors = 0;
    pgf_us_open = pgf_us_seek = pgf_us_write = pgf_us_read = 0;
    pgf_seek_us_worst = pgf_write_us_worst = 0;
    pgf_ext_writes = pgf_ext_us = pgf_ext_us_worst = 0;
    pgf_inp_writes = pgf_inp_us = pgf_inp_us_worst = 0;
    pgf_high_water = 0;
    x86_pgf_map_reset();
    xms_opens = 0;
}
#endif  /* X86_PGF_BENCH */

static int pgf_open_file(void)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    size_t dir = 0;

    pgf_ready = 0;
    /* The shared handle must go before this truncates the file under it. */
    pgf_close_file();
    if (!dsk_path[0]) { pgf_result = PGF_NO_PATH; return -1; }
    for (size_t i = 0; dsk_path[i]; i++)
        if (dsk_path[i] == '/')
            dir = i + 1;
    if (dir + 13 >= sizeof(pgf_path)) { pgf_result = PGF_NO_PATH; return -1; }
    memcpy(pgf_path, dsk_path, dir);
    strcpy(pgf_path + dir, "x86page.swp");

    /* "wb" creates it and truncates whatever was there. Zero length is the
       right starting state: a slot that has never been written is never read,
       so the file only has to grow as far as the highest evicted page. */
    FILE *f = abi->fopen(pgf_path, "wb");
    if (!f) { pgf_result = PGF_NO_CREATE; return -1; }
    abi->fclose(f);

    pgf_reads = pgf_writes = pgf_errors = 0;
    pgf_us_open = pgf_us_seek = pgf_us_write = pgf_us_read = 0;
    pgf_seek_us_worst = pgf_write_us_worst = 0;
    pgf_ext_writes = pgf_ext_us = pgf_ext_us_worst = 0;
    pgf_inp_writes = pgf_inp_us = pgf_inp_us_worst = 0;
    pgf_high_water = 0;
    x86_pgf_map_reset();
    pgf_result = PGF_OK;
    pgf_ready = 1;
#if X86_PGF_BENCH
    pgf_bench();
#endif
    return 0;
}

/*
 * THE SLOT RING lives in x86_pgfile.c, and the measurement that forced it is
 * in x86_pgfile.h. Short version: on this card a 4 KB append past EOF costs
 * 12-14 ms and a 4 KB rewrite of recently-written sectors costs 60-67 ms, so a
 * page's slot is no longer its page number -- it comes from a cursor sweeping
 * a ring, and the map remembers where each page's copy went. It is a separate
 * file because this one cannot be driven at tier 1 and that allocator is the
 * part that decides where a guest's bytes end up.
 */

/*
 * Both of these do an ABSOLUTE fseek before every access, which is not
 * decoration: one handle is shared by the pager and by XMS, so the file
 * position on entry belongs to whoever ran last. It is also what C requires
 * between a read and a write on the same update-mode stream.
 */
static int pgf_write(uint32_t page, const void *src)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    int rc = -1;
    FILE *f = pgf_file();

    if (!f)
        return -1;
    uint32_t slot = x86_pgf_slot_claim(page);
    if (slot == X86_PGF_NO_SLOT) { pgf_errors++; return -1; }

    uint32_t off = slot * X86_PG_SIZE;
    int extending = (off + X86_PG_SIZE > pgf_high_water);
    X86_ZONE_ENTER(X86_ZONE_DISK);          /* ZONES: see sd_sector_read */
    uint32_t t0 = DWT_CYCCNT;
    int sought = abi->fseek(f, (long)off, SEEK_SET);
    uint32_t t1 = DWT_CYCCNT;
    pgf_acc(&pgf_us_seek, &pgf_seek_us_worst, t1 - t0);
    if (sought == 0 && abi->fwrite(src, 1, X86_PG_SIZE, f) == X86_PG_SIZE)
        rc = 0;
    pgf_acc(&pgf_us_write, &pgf_write_us_worst, DWT_CYCCNT - t1);
    pgf_acc(extending ? &pgf_ext_us : &pgf_inp_us,
            extending ? &pgf_ext_us_worst : &pgf_inp_us_worst,
            DWT_CYCCNT - t0);           /* seek+write: what the frame pays */
    if (extending) pgf_ext_writes++; else pgf_inp_writes++;
    X86_ZONE_EXIT();

    /* The map moves ONLY on success -- see rule 1 in x86_pgfile.h. A failed
       writeback now leaves the page on its previous copy instead of on a slot
       that was half-written over the only one it had. */
    x86_pgf_commit(page, slot, rc == 0);
    if (rc == 0) {
        if (off + X86_PG_SIZE > pgf_high_water)
            pgf_high_water = off + X86_PG_SIZE;
        pgf_writes++;
    } else {
        pgf_errors++;
    }
    return rc;
}

static int pgf_read(uint32_t page, void *dst)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    int rc = -1;
    FILE *f = pgf_file();

    if (!f)
        return -1;
    uint32_t slot = x86_pgf_slot_of(page);
    if (slot == X86_PGF_NO_SLOT) {
        /* FILE tier with no slot. The pager only sets FILE tier after a write
           SUCCEEDED, so this is unreachable by construction -- and it is
           counted rather than papered over with zeros, because "the page was
           never written" and "the card failed" must not read the same. */
        pgf_errors++;
        return -1;
    }
    X86_ZONE_ENTER(X86_ZONE_DISK);
    uint32_t t0 = DWT_CYCCNT;
    int sought = abi->fseek(f, (long)(slot * X86_PG_SIZE), SEEK_SET);
    uint32_t t1 = DWT_CYCCNT;
    pgf_acc(&pgf_us_seek, &pgf_seek_us_worst, t1 - t0);
    if (sought == 0 && abi->fread(dst, 1, X86_PG_SIZE, f) == X86_PG_SIZE)
        rc = 0;
    pgf_acc(&pgf_us_read, NULL, DWT_CYCCNT - t1);
    X86_ZONE_EXIT();
    if (rc == 0) pgf_reads++; else pgf_errors++;
    return rc;
}

/* ===========================================================================
 * THE XMS STORE -- the same file, a range the pager provably cannot reach.
 *
 * x86_xms.h explains why XMS memory is NOT a pager frame and costs the
 * interpreter nothing. What is decided HERE is only where the bytes physically
 * are, and the one property that has to hold is that a guest page and an XMS
 * byte cannot land on the same byte of x86page.swp.
 *
 * They cannot, by arithmetic rather than by convention. The pager's `slot` IS
 * the guest page number and guest page numbers stop at X86_PG_PAGES, so the
 * pager can never write past X86_PG_PAGES * X86_PG_SIZE. XMS starts exactly
 * there. Nothing has to be allocated, reserved or checked at run time.
 *
 * The file is sparse in practice: a slot that was never written is never read,
 * and the same holds for the XMS range -- an unwritten region reads back as a
 * short fread, which xms_read() turns into zeros (that IS what freshly
 * allocated extended memory contains as far as any guest can tell) and counts.
 *
 * ONE OPEN PER TITLE, and it is the SAME open the pager uses -- pgf_file()
 * above. There used to be a second handle here, on this same file, while the
 * pager opened and closed a third per evicted page; see the pagefile header for
 * why that was both slow and, with FF_FS_LOCK 0, unsound.
 */
#define XMS_FILE_BASE  ((uint32_t)X86_PG_PAGES * X86_PG_SIZE)   /* 8 MB */

volatile uint32_t xms_bytes_read = 0;
volatile uint32_t xms_bytes_written = 0;
volatile uint32_t xms_holes = 0;        /* reads of a region never written */
volatile uint32_t xms_file_errors = 0;
volatile uint32_t xms_opens = 0;    /* file opens -- ONE per title, not per move */

/*
 * THE HANDLE IS HELD OPEN ACROSS MOVES, and that is a MEASURED decision, not a
 * preference. A title's moves arrive in bursts at level load.
 *
 * MEASURED, WOLF3D driven into gameplay under gwemu, same timeline both ways:
 *
 *   XMS absent (the tree before this)      1,104 frames / 194.5 M insns
 *   XMS, fopen+fclose per move               766 frames / 132.7 M insns  -31%
 *   XMS, this -- the handle held open      1,057 frames / 171.9 M insns   -4%
 *
 * Wolf3D takes the XMS path the moment it is offered and moves 430,636 bytes
 * through it in 278 moves at level load; at ~25 ms per open (docs/12 §1a) the
 * per-move open ALONE was a third of the title's frame rate. Nothing about the
 * interpreter changed between those rows -- XMS costs nothing per access by
 * construction -- the whole difference is fopen. `xms_opens` is 1 for the run,
 * and it is the witness: if it ever climbs with the move count, this reverted.
 *
 * The price is one FIL for the life of the title -- and NOT a sector buffer:
 * the firmware builds FatFs with FF_FS_TINY 1, so a FIL has no private window.
 * It is closed at guest reset, before pgf_open_file() truncates the file.
 */
static int xms_open(int write)
{
    (void)write;
    return pgf_file() ? 0 : -1;
}

/* End of one move. The handle stays; see above. */
static void xms_close(void)
{
}

/* The real close: at guest reset, and only there. */
static void pgf_close_file(void)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    if (pgf_fp) {
        X86_ZONE_ENTER(X86_ZONE_DISK);
        abi->fclose(pgf_fp);
        X86_ZONE_EXIT();
        pgf_fp = NULL;
    }
}

static int xms_read(uint32_t off, void *dst, uint32_t bytes)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    if (!pgf_fp)
        return -1;
    X86_ZONE_ENTER(X86_ZONE_DISK);
    size_t got = 0;
    int rc = -1;
    if (abi->fseek(pgf_fp, (long)(XMS_FILE_BASE + off), SEEK_SET) == 0) {
        got = abi->fread(dst, 1, bytes, pgf_fp);
        rc = 0;
    }
    X86_ZONE_EXIT();
    if (rc != 0) { xms_file_errors++; return -1; }
    if (got < bytes) {
        /* Never written. Zero is the honest content of extended memory the
           guest has not stored to, and it is COUNTED so that "the read was
           short" is never mistaken for "the card failed". */
        memset((uint8_t *)dst + got, 0, bytes - got);
        xms_holes++;
    }
    xms_bytes_read += bytes;
    return 0;
}

static int xms_write(uint32_t off, const void *src, uint32_t bytes)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    if (!pgf_fp)
        return -1;
    X86_ZONE_ENTER(X86_ZONE_DISK);
    int rc = -1;
    if (abi->fseek(pgf_fp, (long)(XMS_FILE_BASE + off), SEEK_SET) == 0 &&
        abi->fwrite(src, 1, bytes, pgf_fp) == bytes)
        rc = 0;
    X86_ZONE_EXIT();
    if (rc != 0) { xms_file_errors++; return -1; }
    if (XMS_FILE_BASE + off + bytes > pgf_high_water)
        pgf_high_water = XMS_FILE_BASE + off + bytes;
    xms_bytes_written += bytes;
    return 0;
}

/*
 * Work out what to run. The disk images are built to boot FreeDOS, so the
 * program to launch is named in AUTOEXEC.BAT rather than being guessable from
 * the .dsk name -- ALLEYCAT.dsk runs CAT.EXE. Reading it keeps this working
 * for the other images too instead of hardcoding one game.
 */
/* "DF" + "DF.EXE" -> "DF/DF.EXE". snprintf pulls in ~600 B of newlib for this. */
static void path_join(char *dst, unsigned max, const char *dir, const char *name)
{
    unsigned i = 0;
    if (dir && dir[0]) {
        while (dir[i] && i < max - 2) { dst[i] = dir[i]; i++; }
        dst[i++] = '/';
    }
    unsigned j = 0;
    while (name[j] && i < max - 1) dst[i++] = name[j++];
    dst[i] = 0;
}

static int parse_batch(const char *batpath, char *out, unsigned max,
                       char *tail, unsigned tail_max, int depth)
{
    FIL f;
    if (f_open(&f, batpath, FA_READ) != FR_OK)
        return -1;

    /* Read it whole and split by hand: f_gets needs FF_USE_STRFUNC, which this
       FatFs build does not enable, and AUTOEXEC.BAT is never large. */
    static char buf[1024];
    UINT got = 0;
    FRESULT rc = f_read(&f, buf, sizeof(buf) - 1, &got);
    f_close(&f);
    if (rc != FR_OK || got == 0)
        return -1;
    buf[got] = 0;

    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        while (*p == '\r' || *p == '\n') *p++ = 0;

        char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '@') q++;
        while (*q == ' ' || *q == '\t') q++;
        if (!*q || *q == ';' || *q == ':')
            continue;
        /*
         * `CD` is not scaffolding. STAR_WARS_DARKFORCES.dsk's AUTOEXEC.BAT is
         * `CD \DF` then `DFDEMO.BAT`, and skipping the CD left us opening
         * "DFDEMO.BAT" in the ROOT -- which does not exist. Record the
         * directory instead: it is both where the next name is looked up and
         * the CWD the program's own relative opens must resolve against.
         */
        if (!strncasecmp(q, "CD", 2) &&
            (q[2] == ' ' || q[2] == '\t' || q[2] == '\\')) {
            const char *d = q + 2;
            while (*d == ' ' || *d == '\t') d++;
            if (*d && strcmp(d, "\\") != 0 && strcmp(d, "/") != 0)
                x86_dos_set_cwd(d);
            continue;
        }
        /* Skip the rest of the scaffolding; the first real command is it. */
        if (!strncasecmp(q, "REM", 3) || !strncasecmp(q, "ECHO", 4) ||
            !strncasecmp(q, "PROMPT", 6) || !strncasecmp(q, "SET", 3) ||
            !strncasecmp(q, "PATH", 4) || !strncasecmp(q, "CD", 2) ||
            !strncasecmp(q, "CLS", 3) || !strncasecmp(q, "VERIFY", 6))
            continue;

        unsigned i = 0;
        while (q[i] && q[i] != ' ' && q[i] != '\t' && i < max - 1) {
            out[i] = q[i];
            i++;
        }
        out[i] = 0;
        /* The COMMAND TAIL, which was silently dropped. TOPBENCH.EXE -l and
           TOPBENCH.EXE are different programs -- one scores and exits, the
           other draws a menu and waits for a key that a headless run never
           sends -- so throwing away everything after the first token quietly
           changed what the disk was asked to do. */
        const char *rest = q + i;
        while (*rest == ' ' || *rest == '\t') rest++;
        unsigned j = 0;
        while (rest[j] && j < tail_max - 1) {
            tail[j] = rest[j];
            j++;
        }
        tail[j] = 0;
        if (!i)
            return -1;

        /*
         * A .BAT is not a program. DFDEMO.BAT is a two-line wrapper around
         * `df.exe %1 %2 ...`; handed to the MZ loader it fails the signature
         * check. Follow it ONCE -- one level is what these generated disks
         * use, and unbounded recursion on a guest-authored file is not
         * something a loader should offer. The %n placeholders expand to
         * nothing, which is what COMMAND.COM does with no arguments.
         */
        unsigned n = 0;
        while (out[n]) n++;
        if (depth < 1 && n > 4 && !strncasecmp(out + n - 4, ".BAT", 4)) {
            char sub[96];
            path_join(sub, sizeof(sub), x86_dos_get_cwd(), out);
            return parse_batch(sub, out, max, tail, tail_max, depth + 1);
        }
        /* Strip the unexpanded %n placeholders a wrapper .BAT passes on. */
        for (unsigned k = 0; tail[k]; k++)
            if (tail[k] == '%') { tail[k] = 0; break; }
        return 0;
    }
    return -1;
}

/*
 * Work out what to run. The disk images are built to boot FreeDOS, so the
 * program to launch is named in AUTOEXEC.BAT rather than being guessable from
 * the .dsk name -- ALLEYCAT.dsk runs CAT.EXE.
 */
static int find_startup_program(char *out, unsigned max,
                                char *tail, unsigned tail_max)
{
    x86_dos_set_cwd("");
    return parse_batch("AUTOEXEC.BAT", out, max, tail, tail_max, 0);
}

static FATFS dsk_fs;

static int mount_guest_disk(void)
{
    if (!ACTIVE_FILE || !ACTIVE_FILE->path[0])
        return -1;
    /* A different title means a different image: the held handle goes first. */
    dsk_close_file();
    strncpy(dsk_path, ACTIVE_FILE->path, sizeof(dsk_path) - 1);
    x86_disk_set_reader(sd_sector_read, dsk_path, ACTIVE_FILE->size);
    x86_disk_set_writer(sd_sector_write);
    if (f_mount(&dsk_fs, "", 1) != FR_OK)
        return -1;
    dsk_mounted = 1;
    return 0;
}

static void guest_reset(void)
{
    memset(cpu_state_ptr, 0, sizeof(X86CPUState));
    /* A reset machine's system timer is at its boot divisor again. Leaving a
       previous guest's 140 Hz in place would run the next one's clock 7.7x
       fast, which is the same defect in the opposite direction. */
    x86_io.pit0_reload = 0;
    x86_io.pit0_access = 0;
    x86_io.pit0_latched = 0;
    for (int i = 0; i < 6; i++)
        cpu_state_ptr->segs[i].limit = 0xFFFF;   /* real mode: 64 KB segments */
    /* CS.D stays clear: real mode is 16-bit, which is now genuinely different
       from 32-bit rather than the same handlers under another name. */

    x86_unmap_all();

    /*
     * Guest RAM is DEMAND PAGED now, not mapped 1:1 up front.
     *
     * The pager gets exactly the same AXI the eager mapping used, so at this
     * size nothing can be evicted and the guest sees identical memory -- the
     * pages simply bind on first touch instead of at reset. That is the point
     * of doing it at this size first: the whole fault path is exercised by
     * every title, under conditions where a shortage is impossible, before the
     * pool is ever made smaller than the working set.
     *
     * The window is [0, 640 KB). Everything above it -- VRAM, the mode 13h
     * aperture, the open-bus filler, the BIOS id page -- is a FIXED mapping to
     * real hardware storage and must never be paged, which is expressed by
     * simply not giving those pages to the pager.
     */
    x86_pager_reset();
    x86_pager_own(0, GUEST_RAM_SIZE / 4096u);
    /* The slot map covers exactly the pages the pager can hand to a
       backing-store callback -- the same two windows, and nothing else. */
    x86_pgf_map_init(GUEST_RAM_SIZE / 4096u,
                     X86_EMS_BACK_PAGE, X86_EMS_BACK_PAGES);
    /*
     * The SECOND window: EMS's logical pages, at linear 4 MB.
     *
     * They have to be pager pages rather than a byte range of the file the way
     * XMS is, because an EMS page mapped into the page frame is read by
     * ORDINARY GUEST INSTRUCTIONS and therefore has to be able to occupy a
     * frame (x86_ems.h). No real-mode segment:offset can name linear 4 MB --
     * FFFF:FFFF is 0x10FFEF -- so nothing but x86_ems.c ever reaches it, and
     * its pagefile slots (4 MB..5 MB of x86page.swp) sit below the XMS store at
     * 8 MB by the same arithmetic that keeps XMS off the pager's slots.
     */
    x86_pager_own_aux(X86_EMS_BACK_PAGE, X86_EMS_BACK_PAGES);
    if (guest_ram)
        /* POOL BYTES, not guest bytes. A frame is 4,104 bytes now -- 4,096 of
           guest space plus the eight the seam writes into (x86_pager.h). Handing
           this GUEST_RAM_SIZE would yield 159 frames for 160 pages and quietly
           turn a pool that cannot evict into one that must. */
        /* guest_ram_pages, NOT GUEST_RAM_SIZE/4096: the pool is sized to the
           RAM_EMU that was actually free (see the claim). Handing this the
           nominal count would describe frames that do not exist. */
        x86_pager_add_frames(guest_ram, X86_PG_POOL_BYTES(guest_ram_pages));
    /*
     * The FILE tier. Without this call there is no backing store at all: the
     * victim picker may then only ever evict a page it can DROP, donation is
     * refused outright (x86_pager.h: "SAFE ONLY BECAUSE EVICTION EXISTS"), and
     * a pool smaller than the working set is a hard X86_EXIT_MEM_FAULT. This is
     * the one call the whole mechanism was waiting on.
     *
     * If the file cannot be made the pager is left with no backing store, which
     * is the SAME state the tree was in before -- donation refuses, nothing is
     * lost, and pgf_result says why.
     */
    if (pgf_open_file() == 0) {
        x86_pager_set_backing(pgf_read, pgf_write);
        /* And XMS, out of the same file above every slot the pager owns.
           X86_XMS_DEFAULT_KB is what AH=08h will advertise; it is bounded by
           the card and by transfer time, NOT by the AXI pool -- no XMS byte is
           ever in a frame. See x86_xms.h. */
        /* PER TITLE, when the .cfg says so. `[dos] xms = false` or
           `xms_kb = N`. "Unspecified" is base_is_set() answering 0, NOT the
           value 0 -- which is exactly what lets `xms = false` mean "advertise
           none" and still be distinguishable from saying nothing at all. So a
           title with no .cfg gets X86_XMS_DEFAULT_KB, unchanged, as it does
           today. The pagefile still has the final veto: no file, no XMS,
           whatever a .cfg asked for (the else branch below). */
        x86_xms_set_backing(xms_open, xms_close, xms_read, xms_write,
                            x86_settings_base_is_set(X86_SET_XMS_KB)
                                ? (uint32_t)x86_settings_get(X86_SET_XMS_KB)
                                : X86_XMS_DEFAULT_KB);
    } else {
        x86_pager_set_backing(NULL, NULL);
        /* No file, no extended memory, and INT 2Fh AX=4300h then answers
           "absent". Advertising XMS with nowhere to put a byte is the failure
           this project keeps hitting: the guest believes it, allocates, moves,
           and dies somewhere unrelated. */
        x86_xms_set_backing(NULL, NULL, NULL, NULL, 0);
    }
    x86_video_map();

    /* What INT 12h and 0040:0013 will report, taken from the allocation that
       actually happened rather than from a constant that can drift away from
       it. */
    x86_bios_conv_kb = GUEST_RAM_SIZE / 1024u;
    x86_bios_init();
    /* Take over the vectors we serve. The guest reaches us through a planted
       0F 0B stub, so a program that hooks or chains one still works.
       0x11/0x12/0x15 are the machine-description calls: they were reaching the
       bare IRET, which hands the guest back its own AX and reads as a
       successful answer -- see x86_bios.c and tests/test_sysinfo.py. */
    static const uint8_t hle_vectors[] = {
        0x08, 0x09, 0x10, 0x11, 0x12, 0x15, 0x16, 0x1A, 0x20, 0x21, 0x2F,
    };
    x86_hle_install(hle_vectors, sizeof(hle_vectors));

    /*
     * Upper memory. A real machine has ROM and adapter space from 0xA0000 up,
     * and a system tool scans it looking for option-ROM signatures. Leaving it
     * unmapped makes every one of those probes fault.
     *
     * Backing it properly would cost 384 KB of pool. Instead one page of 0xFF
     * -- open bus, which is what unpopulated memory reads as -- is ALIASED
     * across the whole region: the fold maps each granule independently, so 96
     * entries can share one page for 4 KB total. A scan reads 0xFF, finds no
     * signature, and moves on.
     */
    if (upper_filler) {
        memset(upper_filler, 0xFF, 4096);
        for (uint32_t lin = 0xA0000u; lin < 0x100000u; lin += 4096u)
            x86_map_fixed(lin, upper_filler, 4096u);
    }
    x86_video_map();                 /* VRAM overlays its part of that range */

    /* The identification bytes need their own page: they must persist, and
       everything else in the region is a shared alias. */
    if (bios_id_page) {
        memset(bios_id_page, 0xFF, 4096);
        x86_map_fixed(0xFF000u, bios_id_page, 4096u);
    }
    x86_bios_rom_install();

    /*
     * EMS, and it is set up HERE rather than beside XMS above for one reason:
     * it CHECKS that its page frame is free before it claims it, and the upper
     * megabyte is not mapped until the block just above. Moving this call back
     * up would make the check read sixteen FOLD_INVALID entries and refuse the
     * interface on a machine where the frame is perfectly free.
     *
     * The size is honest the same way X86_XMS_DEFAULT_KB is: EMS pages are
     * pager pages, and with no pagefile the pager cannot evict, so every page
     * EMS handed out would have to stay resident forever out of a 160-frame
     * pool. So no pagefile means ZERO pages, which means EMMXXXX0 does not
     * open and the INT 67h vector is left on the bare IRET -- absent, rather
     * than present and about to lose the guest's memory.
     */
    /* PER TITLE, same rule and same reason as XMS above: `[dos] ems = false`
       or `ems_kb = N`, in 16 KB logical pages. The .cfg parser has already
       refused anything that is not a whole number of pages -- a reservation
       that cannot hold one leaves a physical frame page pointing at scratch,
       which is the silent-discard failure x86_ems.c is written around. Capped
       at X86_EMS_PAGES because that is the aux window the pager was actually
       given at reset; asking for more could only be honoured by giving less. */
    {
        uint32_t pages = X86_EMS_PAGES;
        if (x86_settings_base_is_set(X86_SET_EMS_KB)) {
            pages = (uint32_t)x86_settings_get(X86_SET_EMS_KB) / 16u;
            if (pages > X86_EMS_PAGES)
                pages = X86_EMS_PAGES;
        }
        x86_ems_set_backing(pgf_ready ? pages : 0u);
    }

    /* Everything above 1 MB wraps to the bottom, as it does with A20 masked. */
    x86_mem_wrap_1mb();

    x86_dos_arena_init();

    /* Load the real program if the disk mounted; fall back to the built-in
       demo so a missing or unreadable .dsk shows a picture rather than a black
       screen with no explanation. */
    char startup[64];
    char cmdtail[64];
    dsk_startup_ok = 0;
    dsk_exec_rc = 0xFFFFFFFF;

    /*
     * ---- THE .cfg's LAUNCH PARAMETERS ---------------------------------------
     *
     * `[autoexec]` (or `entry`/`args`/`cwd` written out) OVERRIDES what the
     * guest's own AUTOEXEC.BAT names. Applied here, between the mount and the
     * exec, because that is the only point where both the disk is readable and
     * nothing has been launched yet.
     *
     * `use_cmdtail` is the pre-existing knob and keeps its pre-existing meaning
     * -- it decides whether the GUEST's tail is passed on. It is deliberately
     * NOT consulted for a tail this file named: an `args` line is somebody
     * stating the whole launch, and making it conditional on a second setting
     * would mean a .cfg that reads as complete and silently is not.
     */
    if (dsk_mounted && x86_settings_str_is_set(X86_STR_ENTRY)) {
        const char *e = x86_settings_str(X86_STR_ENTRY);
        const char *a = x86_settings_str(X86_STR_ARGS);
        const char *d = x86_settings_str(X86_STR_CWD);

        /* find_startup_program() would otherwise set the CWD itself, so the
           .cfg's has to be installed instead of it, not before it. */
        x86_dos_set_cwd(d);
        snprintf(startup, sizeof(startup), "%s", e);
        snprintf(cmdtail, sizeof(cmdtail), "%s", a);
        dsk_startup_ok = 1;
        /* A tail this file named is passed WHATEVER use_cmdtail says -- see
           above -- so force the flag the exec below reads rather than adding a
           second branch to the call. */
        guest_use_cmdtail = 1;
        printf("DOS: cfg launch '%s' '%s' in '%s'\n", startup, cmdtail, d);
    } else if (dsk_mounted && find_startup_program(startup, sizeof(startup),
                                            cmdtail, sizeof(cmdtail)) == 0) {
        dsk_startup_ok = 1;
        /* PER TITLE now, as well as host-overridable: `use_cmdtail` in the
           .cfg reaches the same flag the debug backend writes. A .cfg that
           says nothing leaves it exactly where it was. */
        if (x86_settings_base_is_set(X86_SET_USE_CMDTAIL))
            guest_use_cmdtail = (uint32_t)x86_settings_get(X86_SET_USE_CMDTAIL);
        /* Host-overridable command tail. AUTOEXEC.BAT decides by default --
           TOPBENCH.dsk asks for "-l", its continuous scoring loop. Clear
           guest_use_cmdtail (over the debug backend, or here) to launch the
           program bare, which for TOPBENCH is its interactive menu UI. */
        /* The program lives in the CWD the batch file changed to, and
           x86_dos_exec takes a FatFs path rather than a guest one, so the
           prefix is applied here rather than in dos_path_from_guest(). */
        char full[96];
        int rooted = 0;
        for (const char *c = startup; *c; c++)
            if (*c == '/' || *c == '\\') { rooted = 1; break; }
        path_join(full, sizeof(full), rooted ? "" : x86_dos_get_cwd(), startup);
        dsk_exec_rc = (uint32_t)x86_dos_exec(full,
                                             guest_use_cmdtail ? cmdtail : "");
    }
    if (dsk_exec_rc != 0) {
        /*
         * guest_ram is a page POOL, not a flat image of the low megabyte, so
         * `guest_ram + 0x100` was the pool's first frame and NOT guest linear
         * 0x100. The demo was written somewhere the guest never executes, and
         * the guest instead started at 0000:0100 -- inside the IVT -- and ran
         * table bytes as code. Write it through the pager like everything
         * else, and pin CS to 0 so 0000:0100 is where the bytes actually are.
         */
        x86_guest_copy_in(0x100, demo_guest, sizeof(demo_guest));
        cpu_state_ptr->segs[1].sel = 0;
        cpu_state_ptr->segs[1].base = 0;
        cpu_state_ptr->eip = 0x100;
        cpu_state_ptr->regs[4] = 0xFFFE;
    }
    cpu_state_ptr->eflags = 0x0202;              /* IF set, bit 1 reserved-1 */
    cpu_state_ptr->hle_enabled = 1;

    /* A first slice, replaced on the very next iteration by the wall-clock
       budget (see the three-clock block). The PIT exit is what ends a slice.
       It is NOT the clock -- the BIOS tick comes from real time -- it is how
       often we come up for air to render, poll and feed audio. The CGA
       retrace bit is derived from how far this counter has run down, so a
       slice is also the emulated machine's notion of a display frame. */
    x86_osk_reset();
    cpu_state_ptr->pit_reload = guest_slice_now ? guest_slice_now : 20000u;
    x86_pit_counter_forced(cpu_state_ptr->pit_reload);
}

/*
 * Gamepad -> scancodes.
 *
 * Edge-triggered: a make code when a button goes down, a break code when it
 * comes up, exactly as a keyboard controller reports. Level-triggering would
 * flood the queue and leave a game seeing an endless key repeat.
 *
 * Modelled at this level on purpose: ports and IRQs behave identically in real
 * and protected mode, so nothing here has to be revisited for protected mode,
 * and a game that installs its own INT 09h and reads port 0x60 directly -- as
 * most action games do -- needs no BIOS layer at all.
 */
/*
 * The machine has SIX buttons a guest may use: the d-pad, A, B, GAME and TIME.
 * The firmware reports GAME as ODROID_INPUT_START and TIME as
 * ODROID_INPUT_SELECT (odroid_input.c), so "Start" and "Game" are one physical
 * button and so are "Select" and "Time" -- there is no fourth face button to
 * give away.
 *
 * Which key each one sends is not cosmetic. Alley Cat opens with
 * "Do you want to use a joystick (Y/N)?" and reads port 0x60 for the answer:
 * with the previous table (Ctrl / Enter / Esc) NO button could produce a Y or
 * an N, so the game sat on that prompt forever and every later press went to a
 * program that had not started. That is the whole of "the guest ignores the
 * keyboard" -- the delivery chain underneath was already working.
 *
 * N, not Y, on the shared TIME/SELECT button: answering "no joystick" is what
 * puts the game on the keyboard we actually emulate. Y is unreachable until
 * the GAME button's on-screen keyboard exists.
 */
/* Indexed by the bit numbering poll_input builds below from the ODROID_INPUT_*
   enum -- the compiler resolves the enum, so this order is ours and is correct
   by construction. Do NOT "fix" it against odroid_input.h.
   0 means the button is deliberately bound to nothing. */
static const uint8_t pad_scancode[10] = {
    0x48,  /* UP                      -> Up arrow    */
    0x50,  /* DOWN                    -> Down arrow  */
    0x4B,  /* LEFT                    -> Left arrow  */
    0x4D,  /* RIGHT                   -> Right arrow */
    0x39,  /* A                       -> Space       */
    0x01,  /* B                       -> Esc         */
    0x00,  /* ODROID_INPUT_START = GAME button: reserved for the on-screen
              keyboard toggle (8086tiny docs/input/02 decided this deliberately:
              PAUSE belongs to retro-go, so GAME is the only usable door) */
    0x15,  /* ODROID_INPUT_SELECT = TIME button -> Y  */
    0x1C,  /* ODROID_INPUT_X = physical START   -> Return. Zelda-only in
              8086tiny's notes, but THIS unit reports it (raw pad bit 11). */
    0x31,  /* ODROID_INPUT_Y = physical SELECT  -> N   */
};

/*
 * ===========================================================================
 * The emulated CPU's speed, as a Retro-Go game-options entry.
 * ===========================================================================
 *
 * common_emu_input_loop() takes an options array and shows it in the PAUSE/SET
 * menu; we used to pass NULL, so guest_kips was reachable only over the debug
 * probe. The rungs are the KIPS_* profiles above, MAX first because that is the
 * default.
 *
 * Three things here are load-bearing, each of them a hardfault or a wrong
 * reading if got wrong:
 *
 *   - `value` must point at MUTABLE, caller-owned storage. The callback writes
 *     the rendered value into option->value, so a string literal there is a
 *     write to flash. It is static rather than automatic so it also outlives
 *     the dialog, which keeps the pointer.
 *   - the array is terminated with ODROID_DIALOG_CHOICE_LAST, and the repaint
 *     callback is NEVER NULL (see core_repaint).
 *   - the value is re-rendered on EVERY event, including the plain repaint,
 *     because the dialog draws from the buffer rather than from our state.
 */
/*
 * The settings key is NOT free-form. odroid_settings_app_int32_get/set match
 * the key against a fixed whitelist ("Palette", "SpriteLimit", "Region",
 * "DispScaling", "DispFilter", "DispOverscan", "DistRotation") and SILENTLY
 * ignore anything else -- a set with our own name would store nothing and the
 * get would hand back the default forever, which reads exactly like "the
 * setting does not persist". The slot is per-app (core_cfg_slot()), so
 * "Palette" is ours alone; nothing in this core has a palette setting.
 */
#define CPU_PROFILE_KEY   "Palette"
#define CPU_OPTION_ID     100
#define NOR_OPTION_ID     101

/*
 * The label carries the rung's OWN kips number, because the name alone cannot
 * be compared against anything. "386DX 33" says nothing about whether the
 * silicon can serve it; "386DX 33 (6000k)" sits next to a measured
 * guest_ips_achieved of ~5500k and the answer is immediate. Max has no number
 * because it has no target -- that is the whole meaning of unthrottled.
 */
static const uint16_t cpu_profile_kips[] = {
    KIPS_MAX, KIPS_8088_477, KIPS_286_8, KIPS_286_12, KIPS_386SX_16, KIPS_386_33,
    KIPS_486SX_25, KIPS_486DX2_66, KIPS_486DX4_100,
};
static const char *const cpu_profile_name[] = {
    "Max", "8088 4.77 (330)", "286 8MHz (800)", "286 12MHz (1.2k)",
    "386SX 16 (2.5k)", "386DX 33 (6k)",
    "486SX 25 (9k)", "486DX2 66 (24k)", "486DX4 100 (40k)",
};
#define CPU_PROFILE_COUNT ((int)(sizeof(cpu_profile_kips) / sizeof(cpu_profile_kips[0])))
/* The two arrays are indexed by the same variable; a mismatch reads off the
   end of one of them and the menu shows a wild pointer. */
_Static_assert(sizeof(cpu_profile_name) / sizeof(cpu_profile_name[0])
               == (size_t)CPU_PROFILE_COUNT,
               "cpu_profile_name and cpu_profile_kips must stay the same length");

/*
 * ===========================================================================
 * PER-TITLE SETTINGS -- the retro-go binding. The other half is x86_settings.c
 * (the format and the policy) and x86_cfg.c (the DOSBox-syntax text parser),
 * neither of which does any I/O at all.
 * ===========================================================================
 *
 * Brought over from the prior DOS port, whose split this keeps: THAT is what
 * lets tests/test_settings.py grade the whole format natively, with no target,
 * while this file stays small enough to read in one sitting. All of it is COLD
 * PATH -- once at launch, once per menu change.
 *
 * ---- FILE HANDLES. OPEN, READ, CLOSE. NEVER HELD. -------------------------
 *
 * MAX_OPEN_FILES is 8 across the WHOLE firmware. This core already holds the
 * pagefile's handle for its entire run (pgf_fp, and the comment above it says
 * what re-opening it cost), the launcher holds its own, and EXHAUSTION IS
 * SILENT: every other fopen() starts returning NULL and the UI degrades with
 * nothing logged. The prior port records its whole launcher rendering as
 * DIAMONDS because of exactly this. Each of the three functions below takes at
 * most ONE handle and gives it back before it returns, and none of them runs
 * while the pagefile is open -- the two loads happen before guest_reset().
 *
 * ---- WHICH FILE IS WHICH ---------------------------------------------------
 *
 *   <TITLE>.cfg      HUMAN-AUTHORED, never written by the device. DOSBox
 *                    syntax. Sits beside the .dsk in /roms/dos, which is where
 *                    scripts/common/sdcard.py's compose() already copies it.
 *                    It sets this title's DEFAULTS.
 *   <TITLE>.dosset   THE DEVICE'S, written when the PAUSE/SET menu changes a
 *                    row. Binary, CRC'd, and it records ONLY what differs from
 *                    the default above.
 *
 * NOT /CONFIG. That is retro-go's own global config -- it holds the preselected
 * title and the overclock level and a compose rewrites it wholesale. Per-title
 * settings are a different file with a different lifetime and a different
 * owner, and putting one in the other is how a repack silently eats a user's
 * choices.
 */

/* Read a whole small file into `buf`, and CLOSE IT. Returns the byte count, 0
   for "no file" -- which is not an error and is the common case. */
static size_t settings_slurp(const char *path, unsigned char *buf, size_t cap)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    FILE *f = abi->fopen(path, "rb");
    size_t n = 0;

    if (!f)
        return 0;
    n = abi->fread(buf, 1, cap, f);
    abi->fclose(f);                 /* OPEN, READ, CLOSE -- never held */
    return n;
}

/*
 * `<TITLE>.dsk` -> `<TITLE><suffix>`, in the same directory. The extension is
 * REPLACED, not appended, which is the derivation the prior port used and the
 * one the sidecars already on the card were named by: "KEEN4.dsk" -> "KEEN4.cfg".
 * Deliberately the same, so the files a title can have have names a user can
 * predict.
 *
 * KEYED ON THE PATH AND NOTHING ELSE -- no size, no CRC. The .dosmeta sidecar
 * next door is keyed on the image's bytes so that regenerating a .dsk
 * INVALIDATES it, which is correct for a MEASUREMENT and catastrophic for a
 * PREFERENCE: rebuilding the disk image must not throw away what the user
 * chose. The prior port lost a day to that failure in the other direction.
 *
 * The cost of the choice, stated honestly: rename a .dsk and the settings do
 * not follow it. That is what its savestates and its cover art already do.
 */
static int settings_path(char *out, size_t cap, const char *suffix)
{
    size_t n = 0, cut;

    if (!dsk_path[0])
        return -1;
    while (dsk_path[n])
        n++;
    cut = n;
    for (size_t i = n; i > 0; i--) {
        if (dsk_path[i - 1] == '/')
            break;
        if (dsk_path[i - 1] == '.') { cut = i - 1; break; }
    }
    {
        size_t sl = 0;
        while (suffix[sl])
            sl++;
        if (cut + sl + 1 > cap)
            return -1;
        memcpy(out, dsk_path, cut);
        memcpy(out + cut, suffix, sl + 1);
    }
    return 0;
}

/* Instrumentation. The whole point of a settings layer is that it is invisible
   when it works, which is exactly what makes a broken one indistinguishable
   from a working one -- so what it did is readable over the probe. */
volatile uint32_t settings_cfg_status  = 0xFF; /* x86_cfg_status_t             */
volatile uint32_t settings_cfg_bytes   = 0;    /* .cfg bytes read              */
volatile uint32_t settings_cfg_applied = 0;    /* keys it applied              */
volatile uint32_t settings_dset_status = 0xFF; /* x86_settings_status_t        */
volatile uint32_t settings_dset_bytes  = 0;    /* .dosset bytes read           */
volatile uint32_t settings_saves       = 0;    /* files written                */
volatile uint32_t settings_save_errors = 0;    /* writes that failed           */

/*
 * Read this title's two files and bring them into force, in THIS order.
 *
 * The .cfg FIRST because it sets the DEFAULTS, and the .dosset second because
 * x86_settings_load() resets onto those defaults and then overlays only what
 * the user changed. The other order would make a file on the card overrule the
 * user's own menu choices, which is backwards.
 *
 * MUST RUN BEFORE guest_reset(). xms/ems backing is chosen inside it, and the
 * CPU profile feeds guest_kips before the first slice -- a setting applied
 * after the machine is up takes effect at some later, unpredictable moment,
 * which is the kind of half-applied state that makes a feature look
 * intermittent rather than broken. That is the prior port's warning, and it
 * cost it a session.
 *
 * CANNOT FAIL IN A WAY THAT MATTERS, so there is no return value: a missing
 * file, an unreadable card, a corrupt header and a truncated write all mean
 * "defaults", and none of them is a reason not to start the title.
 */
static void settings_load(void)
{
    /* Both buffers are on THIS frame and nowhere else: .overlay/RAM_EMU
       headroom is the tightest budget in this core (STATUS.md's Budgets), and
       2 KB that lives for the length of one cold call has no business being
       static. */
    char path[sizeof(dsk_path) + 8];
    x86_cfg_result_t res;
    x86_cfg_status_t st = X86_CFG_EABSENT;

    {
        /* ON THE STACK, NOT STATIC, and that is worth 2,049 bytes of RAM_EMU --
           which is HALF A PAGER FRAME (stride 4,104), i.e. the difference
           between this feature costing the guest a page of memory and not.
           RAM_EMU is the tightest budget in this core and the core's own code
           and bss come out of the same pool the guest's pages do.

           Safe because this runs exactly once, from app_main(), at the shallow
           end of a 24 KB stack, and the parse copies out everything it keeps
           (five ints and three short strings in x86_settings.c) before
           returning. */
        unsigned char cfgbuf[X86_CFG_MAX_BYTES + 1];
        size_t n = 0;

        /* Unconditionally, before anything below can go wrong: no .cfg is in
           force. x86_cfg_parse(NULL, 0, ...) is the documented way to say that
           and it clears the base itself, so there is no path out of here that
           leaves a previous title's .cfg behind. */
        x86_cfg_parse(NULL, 0, &res);

        if (settings_path(path, sizeof(path), ".cfg") == 0) {
            /* ONE BYTE MORE than the cap, so a file exactly one byte too long
               is reported TOOBIG rather than silently read as the largest legal
               file. Truncating is the failure x86_cfg.h refuses. */
            n = settings_slurp(path, cfgbuf, sizeof(cfgbuf));
            settings_cfg_bytes = (uint32_t)n;
            st = x86_cfg_parse(cfgbuf, (unsigned long)n, &res);
        }
        settings_cfg_status = (uint32_t)st;
        settings_cfg_applied = res.applied;
        {
            /* The BASE NAME only. The log ring is small and wraps, so the full
               path is not affordable and is not what a user needs read back. */
            const char *base = path, *p;
            for (p = path; *p; p++)
                if (*p == '/') base = p + 1;
            x86_cfg_report(base, st, &res);
        }
    }

    {
        unsigned char dbuf[X86_SETTINGS_MAX_BYTES];
        size_t n = 0;

        if (settings_path(path, sizeof(path), ".dosset") == 0)
            n = settings_slurp(path, dbuf, sizeof(dbuf));
        settings_dset_bytes = (uint32_t)n;
        /* n == 0 covers both "no file" and "empty file", and load() treats that
           as ABSENT and prints nothing -- a title nobody has configured is the
           common case, not an event worth a line in a wrapping log ring. */
        settings_dset_status = (uint32_t)x86_settings_load(dbuf, (unsigned long)n);
    }
}

/*
 * Write the .dosset, but ONLY if something actually differs from what was
 * loaded.
 *
 * THE `dirty` GUARD IS NOT AN OPTIMISATION. Without it every title anyone ever
 * launched would grow a .dosset recording nothing, and -- worse -- a title that
 * had merely been LAUNCHED would be pinned to today's defaults for ever, since
 * a written default is indistinguishable from a chosen one. Changing a default
 * in firmware has to keep moving every title that never overrode it.
 */
static void settings_save(void)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    char path[sizeof(dsk_path) + 8];
    unsigned char buf[X86_SETTINGS_MAX_BYTES];
    unsigned long n;
    FILE *f;

    if (!x86_settings_dirty())
        return;
    n = x86_settings_save(buf, sizeof(buf));
    if (!n || settings_path(path, sizeof(path), ".dosset") != 0)
        return;

    f = abi->fopen(path, "wb");
    if (!f) {
        /* LOUD. This is the one failure a user can actually observe -- they
           changed a setting and it did not stick -- so it must not be silent,
           and the likely cause is worth naming at the point of failure rather
           than in a document nobody is reading at the time. */
        settings_save_errors++;
        printf("DOS: settings -- cannot write %s"
               " (card full, or out of file handles?)\n", path);
        return;
    }
    if (abi->fwrite(buf, 1, n, f) != n) {
        settings_save_errors++;
        printf("DOS: settings -- short write to %s\n", path);
    } else {
        settings_saves++;
    }
    abi->fclose(f);                 /* one handle, and it goes back here */
}

volatile uint32_t cpu_profile_persisted = 0xFF;  /* what the settings store kept */
volatile uint32_t cpu_profile_events = 0;        /* dialog callbacks seen      */
volatile uint32_t cpu_profile_last_event = 0xFF; /* last odroid_dialog_event_t */
static int  cpu_profile_idx;
/* Sized for the LONGEST name plus its NUL, and asserted rather than eyeballed:
   cpu_profile_apply() strcpy()s into this, so a name that outgrows it is a
   silent stack smash in the menu path. "486DX4 100 (40000k)" is 19 + NUL. */
#define CPU_PROFILE_VALUE_MAX 24
static char cpu_profile_value[CPU_PROFILE_VALUE_MAX];

/* Changing the emulated CPU's speed invalidates every number measured against
   the old one. Clearing them is what keeps "am I getting what I asked for?"
   answerable straight after a change instead of a minute later. */
static void cpu_profile_apply(int idx)
{
    if (idx < 0 || idx >= CPU_PROFILE_COUNT)
        idx = 0;
    cpu_profile_idx = idx;
    guest_kips = cpu_profile_kips[idx];
    guest_insn_debt = 0;
    guest_insn_deficit = 0;
    /* Bounded: the buffer is sized and asserted above, but a truncating copy
       is a wrong menu label rather than a smashed stack if a name ever grows
       past it. */
    snprintf(cpu_profile_value, sizeof(cpu_profile_value), "%s",
             cpu_profile_name[idx]);
}

static bool cpu_profile_cb(odroid_dialog_choice_t *option,
                           odroid_dialog_event_t event, uint32_t repeat)
{
    (void)repeat;
    /* The menu path has no other witness: a malformed array simply never
       reaches us, which looks identical to a menu that never opened. */
    cpu_profile_events++;
    cpu_profile_last_event = (uint32_t)event;

    int idx = cpu_profile_idx;
    int max = CPU_PROFILE_COUNT - 1;

    if (event == ODROID_DIALOG_PREV) idx = idx > 0 ? idx - 1 : max;
    if (event == ODROID_DIALOG_NEXT) idx = idx < max ? idx + 1 : 0;

    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
        const gw_firmware_abi_t *abi = gw_firmware_abi();
        cpu_profile_apply(idx);

        /*
         * PER TITLE, and this is the change the whole feature exists for. The
         * app-scoped store below keys on "%.12s.%d" % (key, app->id) -- the
         * APP, not the ROM -- so it held ONE CPU speed shared by every DOS
         * title on the card. Setting Wolf3D to 486DX2 set it for Alley Cat
         * too. The .dosset is keyed on the title's path and does not.
         *
         * PERSIST IMMEDIATELY, in the callback, rather than at exit. There is
         * no core-exit hook this core reliably reaches -- the launcher can tear
         * a core down through a savestate, a sleep or a reset -- and a setting
         * that survives one of those three exits is WORSE than one that
         * survives none, because it is intermittent. One fopen/fwrite/fclose
         * per keypress on a menu row is affordable on a cold path.
         */
        x86_settings_set(X86_SET_CPU_PROFILE, idx);
        settings_save();

        /* The app-scoped store is still written, and NOT because it is needed:
           cpu_profile_persisted is the existing witness that a menu press
           reached a store at all (TOOLING.md's counter table names it), and a
           settings key the firmware does not know is ignored in SILENCE, so
           dropping the round trip would remove the only evidence that the menu
           path runs. It is now a fallback, one layer under the .dosset. */
        abi->odroid_settings_app_int32_set(CPU_PROFILE_KEY, idx);
        cpu_profile_persisted =
            (uint32_t)abi->odroid_settings_app_int32_get(CPU_PROFILE_KEY, 0xFF);
    }
    strcpy(option->value, cpu_profile_name[cpu_profile_idx]);
    return event == ODROID_DIALOG_ENTER;
}

/*
 * `cycles` -> the nearest rung, and WHY THE RESOLUTION LIVES HERE.
 *
 * x86_settings.c is forbidden to carry a copy of cpu_profile_kips[] -- a second
 * copy of another file's table is a second thing to keep in step, and a table
 * that gains or reorders a row turns a stored INDEX into a different answer
 * with no way for the user to tell. So the .cfg stores a self-describing RATE
 * and this file, which owns the table, picks the rung. That is the one place
 * this design departs from the prior port's, and its header flagged the
 * question and left it open.
 *
 * The units line up with DOSBox by luck and it is worth using: DOSBox counts
 * `cycles` as guest instructions per MILLISECOND and guest_kips is thousands
 * per SECOND, which is the same number. So a figure copied out of somebody's
 * dosbox.conf means what its author meant.
 *
 * KIPS_MAX is 0 = unthrottled, so index 0 is excluded from the search: it would
 * otherwise be "nearest" to everything by being infinitely far in the wrong
 * direction. `cycles = max` does not come through here at all -- x86_cfg.c
 * writes it to the PROFILE as rung 0, because "as fast as it goes" is a rung.
 */
static int cpu_profile_from_kips(int kips)
{
    int best = 1;
    uint32_t best_d = 0xFFFFFFFFu;

    for (int i = 1; i < CPU_PROFILE_COUNT; i++) {
        int k = (int)cpu_profile_kips[i];
        uint32_t d = (uint32_t)(k > kips ? k - kips : kips - k);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static void cpu_profile_init(const gw_firmware_abi_t *abi)
{
    /*
     * THE FOUR LAYERS, lowest first, and each only speaks when the one above it
     * did not:
     *
     *   1. the app-scoped store -- what this core did before per-title existed,
     *      kept as the fallback so an existing user's choice is not silently
     *      dropped by the upgrade;
     *   2. `[cpu] cycles` in the .cfg, resolved to a rung here;
     *   3. `[cpu] cpu_profile` in the .cfg, which names a rung outright and so
     *      beats a rate that had to be approximated;
     *   4. the .dosset -- what the user actually chose on the device.
     *
     * Layers 2-4 all arrive through x86_settings_get(), because the .cfg is
     * expressed as a DEFAULT rather than as a value; that is the whole design
     * and it is why there is no precedence code here for 3 over 4.
     */
    int idx = abi->odroid_settings_app_int32_get(CPU_PROFILE_KEY, 0);

    if (x86_settings_base_is_set(X86_SET_CPU_CYCLES)
        && !x86_settings_base_is_set(X86_SET_CPU_PROFILE))
        idx = cpu_profile_from_kips(x86_settings_get(X86_SET_CPU_CYCLES));
    if (x86_settings_base_is_set(X86_SET_CPU_PROFILE)
        || x86_settings_get(X86_SET_CPU_PROFILE)
           != x86_settings_builtin(X86_SET_CPU_PROFILE))
        idx = x86_settings_get(X86_SET_CPU_PROFILE);

    /* VALIDATED AGAINST THIS TABLE, not against the settings range -- which is
       deliberately looser (0..15) so that x86_settings.c need not know how many
       rungs there are. cpu_profile_apply() clamps a bad index to 0 rather than
       indexing off the end of two arrays. */
    cpu_profile_apply(idx);

    /* Seed the live table with what actually took, so the menu row starts where
       the machine is and `dirty` does not fire on a value nobody chose. */
    if (idx >= 0 && idx <= 15)
        x86_settings_set(X86_SET_CPU_PROFILE, idx);
}

/* ====================================================================== *
 * Moving guest RAM into NOR.
 *
 * x86_nor.c owns the mechanism and knows nothing about flash; this is the
 * transport that gets the bytes there, and it is the only part that needs the
 * firmware ABI. Splitting it this way is what keeps the mechanism testable at
 * tier 1 with no flash at all (tests/test_nor.py), and it leaves
 * x86_nor_attach() as the seam a real savestate system would reuse.
 *
 * ---- The one door the firmware gives us ------------------------------------
 *
 * There is exactly ONE flash-write entry in the ABI:
 * odroid_overlay_cache_file_in_flash_relocate(). It is the ROM cache -- it
 * takes a PATH, erases, programs, and hands back the XIP address. There is no
 * buffer-to-flash call, no OSPI_Erase, no OSPI_Program and no way to query the
 * flash size or invalidate the cache.
 *
 * So the snapshot goes in through the RELOCATE CALLBACK. That callback gets
 * each 16 KB chunk in RAM, after the final flash address is known and before
 * anything is programmed, and is explicitly allowed to modify it in place. We
 * overwrite it with live guest RAM. The file on the SD card therefore only has
 * to be the right SIZE -- its contents never reach flash, so there is no
 * 640 KB dump of guest memory to the card and no window where the card holds a
 * half-written copy of the guest.
 *
 * ---- The stale-hit hazard, and the two things that close it ---------------
 *
 * The cache keys on crc32(path) + crc32(mtime). ON A HIT NOTHING IS
 * PROGRAMMED AND THE CALLBACK NEVER RUNS -- and the call still returns a
 * perfectly valid-looking address, pointing at whatever an earlier snapshot
 * left there. Remapping onto that would corrupt the guest with no error
 * anywhere, which is the worst failure shape this whole feature has.
 *
 * Two independent guards, either of which alone would be enough:
 *   1. a fresh filename per snapshot, so a hit cannot be reached; and
 *   2. the callback's coverage is COUNTED. If it did not fill the whole file
 *      we refuse, whatever the returned address looks like.
 * And behind both, x86_nor_attach() reads the flash back and byte-compares it
 * against guest RAM before it moves a single fold entry.
 * ====================================================================== */
volatile uint32_t nor_snap_events   = 0;  /* menu presses that reached us     */
volatile uint32_t nor_snap_result   = 0;  /* 0 = never run, 1 = ok, else why  */
volatile uint32_t nor_snap_cb_bytes = 0;  /* bytes the relocate cb filled     */
volatile uint32_t nor_snap_ms       = 0;  /* wall time the whole thing took   */
volatile uint32_t nor_snap_addr     = 0;  /* XIP address the firmware chose   */

#define NOR_SNAP_OK            1
#define NOR_SNAP_NO_RAM        2
#define NOR_SNAP_NO_PATH       3
#define NOR_SNAP_PLACEHOLDER   4
#define NOR_SNAP_CACHE_NULL    5
#define NOR_SNAP_NOT_NOR       6
#define NOR_SNAP_SIZE          7
#define NOR_SNAP_STALE_HIT     8
#define NOR_SNAP_ATTACH        9
#define NOR_SNAP_ALREADY      10

static char nor_snap_path[sizeof(dsk_path) + 32];

/*
 * The watchdog heartbeat for the whole snapshot, and the instrument that says
 * whether it was needed.
 *
 * nor_snap_wdog_gap_ms is the WORST interval between two kicks over one
 * snapshot. That is the measurement the watchdog question actually turns on:
 * with the kicks in, it reports the longest stretch that WOULD have been
 * un-kicked without them, so it is read against the ~472 ms WWDG window
 * directly. Anything approaching that window says this path can only run
 * chunked, and says which chunk is the offender.
 *
 * The frame loop is not running while any of this happens -- the snapshot is a
 * menu callback -- so its once-per-frame abi->wdog_refresh() is not covering
 * us here.
 */
volatile uint32_t nor_snap_wdog_gap_ms = 0;   /* worst gap between kicks, ms */
static uint32_t   nor_snap_last_kick_ms;

static void nor_snap_kick(void)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    uint32_t now = abi->HAL_GetTick();
    uint32_t gap = now - nor_snap_last_kick_ms;
    if (gap > nor_snap_wdog_gap_ms)
        nor_snap_wdog_gap_ms = gap;
    nor_snap_last_kick_ms = now;
    abi->wdog_refresh();
}

static void nor_snap_fill(uint8_t *buf, uint32_t len, uint32_t off,
                          uint8_t *file_addr, uint32_t file_size)
{
    (void)file_addr;
    (void)file_size;
    if (!guest_ram || off >= GUEST_RAM_SIZE)
        return;
    uint32_t n = len;
    if (off + n > GUEST_RAM_SIZE)
        n = GUEST_RAM_SIZE - off;
    /* Through the FOLD, not `guest_ram + off`.
     *
     * That shortcut was correct only while guest RAM was mapped 1:1, and it is
     * not any more: a demand-paged page lives in whichever frame was free when
     * it was first touched, so `guest_ram + off` names an unrelated page's
     * contents. It would have snapshotted a shuffled image of guest memory and
     * then VERIFIED it against itself successfully, because the readback
     * compares flash against the same wrong pointer. x86_guest_copy_out()
     * resolves each granule properly and faults in anything not resident. */
    nor_snap_cb_bytes += x86_guest_copy_out(buf, off, n);
    /* This callback is NOT the cheap in-place transform the relocate contract
       has in mind (gw_flash_alloc.c: "last look at the data while it is still
       in RAM"): resolving 16 KB through the fold can fault pages in, and a
       fault can evict one, and an eviction is a pagefile write measured at up
       to 433 ms on silicon. That is a whole WWDG window inside one callback,
       between two of OSPI_Program()'s kicks. */
    nor_snap_kick();
}

/* Installed as x86_nor_heartbeat for the duration of the attach: see x86_nor.c
   on why that file takes a hook instead of calling the ABI itself. */
static void nor_snap_wdog(void)
{
    nor_snap_kick();
}

/* A file of the right length and nothing else. 512 bytes of zeros pushed
   1280 times: the contents are irrelevant (the callback replaces every byte on
   the way to flash) and this needs no allocation, which matters in a region
   with single-digit kilobytes of headroom. */
static int nor_snap_make_placeholder(const gw_firmware_abi_t *abi,
                                     const char *path)
{
    static const uint8_t zeros[512];
    FILE *f = abi->fopen(path, "wb");
    if (!f)
        return -1;
    for (uint32_t off = 0; off < GUEST_RAM_SIZE; off += sizeof(zeros)) {
        /* 1,280 writes, and the frame loop -- the only thing that normally
           kicks the watchdog -- is not running: this is a menu callback. The
           SD driver does refresh inside its own busy-waits, so this loop is
           probably survivable on its own, but "probably" is not a property to
           rest a reset on and the kick costs nothing on a path that already
           writes 640 KB. */
        nor_snap_kick();
        if (abi->fwrite(zeros, 1, sizeof(zeros), f) != sizeof(zeros)) {
            abi->fclose(f);
            abi->remove(path);
            return -1;
        }
    }
    abi->fclose(f);
    return 0;
}

static void nor_snapshot_run(void)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    uint32_t t0 = abi->HAL_GetTick();

    nor_snap_wdog_gap_ms = 0;
    nor_snap_last_kick_ms = t0;

    if (x86_nor_attached)  { nor_snap_result = NOR_SNAP_ALREADY;  return; }
    if (!guest_ram)        { nor_snap_result = NOR_SNAP_NO_RAM;   return; }
    if (!dsk_path[0])      { nor_snap_result = NOR_SNAP_NO_PATH;  return; }

    /* Next to the .dsk: that directory demonstrably exists and is writable
       (the disk write-back path opens the image there "r+b"). The name carries
       the millisecond tick so no two snapshots in a session can collide, which
       is what keeps the ROM cache from serving a hit. */
    size_t dir = 0;
    for (size_t i = 0; dsk_path[i]; i++)
        if (dsk_path[i] == '/')
            dir = i + 1;
    if (dir + 24 >= sizeof(nor_snap_path)) { nor_snap_result = NOR_SNAP_NO_PATH; return; }
    memcpy(nor_snap_path, dsk_path, dir);
    {
        static const char hex[] = "0123456789ABCDEF";
        uint32_t tag = abi->HAL_GetTick();
        char *p = nor_snap_path + dir;
        const char *pre = "norsnap";
        while (*pre) *p++ = *pre++;
        for (int i = 28; i >= 0; i -= 4)
            *p++ = hex[(tag >> i) & 0xF];
        *p++ = '.'; *p++ = 'b'; *p++ = 'i'; *p++ = 'n'; *p = 0;
    }

    if (nor_snap_make_placeholder(abi, nor_snap_path) != 0) {
        nor_snap_result = NOR_SNAP_PLACEHOLDER;
        return;
    }

    nor_snap_cb_bytes = 0;
    uint32_t size = 0;
    uint8_t *xip = abi->odroid_overlay_cache_file_in_flash_relocate(
        nor_snap_path, &size, false, nor_snap_fill);

    /* The placeholder has done its job either way; leaving 640 KB files behind
       on the card would be its own bug. */
    abi->remove(nor_snap_path);

    if (!xip)                        { nor_snap_result = NOR_SNAP_CACHE_NULL; return; }
    nor_snap_addr = (uint32_t)xip;
    if ((uint32_t)xip < 0x90000000u || (uint32_t)xip >= 0x94000000u)
                                     { nor_snap_result = NOR_SNAP_NOT_NOR;   return; }
    if (size != GUEST_RAM_SIZE)      { nor_snap_result = NOR_SNAP_SIZE;      return; }
    /* The cache-hit guard: a hit programs nothing and never calls us back. */
    if (nor_snap_cb_bytes != GUEST_RAM_SIZE)
                                     { nor_snap_result = NOR_SNAP_STALE_HIT; return; }

    /* Only now, and x86_nor_attach() still reads the flash back and compares
       it byte for byte before it touches the fold table. If anything above was
       wrong the guest is still running, unchanged, out of AXI. */
    /* The COW pool x86_nor_attach() takes over is the AXI block itself, so it
       must BE GUEST_RAM_SIZE bytes. A runtime-shortened pool (see the claim)
       is not, and handing it the nominal size would walk off the end. Refuse,
       loudly, rather than attach to memory we do not own. */
    if (guest_ram_pages != GUEST_RAM_SIZE / 4096u) {
        nor_snap_result = NOR_SNAP_NO_RAM;
        return;
    }
    x86_nor_heartbeat = nor_snap_wdog;
    int attached = x86_nor_attach(xip, size, guest_ram, GUEST_RAM_SIZE, 0);
    x86_nor_heartbeat = NULL;
    if (attached != 0) {
        nor_snap_result = NOR_SNAP_ATTACH;
        return;
    }
    /* The AXI block has changed hands: x86_nor.c has taken it over as its COW
       pool, one page at a time as writes demand. The pager must let go of it
       in the same breath or two owners would hand out the same page. Its state
       is dropped rather than migrated because after an attach there is nothing
       left for it to own -- every guest page is served from NOR. */
    x86_pager_reset();

    /*
     * Hand the slice regulator back to its floor, and forget the debt.
     *
     * THIS is the half of the watchdog story that survives the snapshot. The
     * regulator (see max_slice, above) is an integrator: it moves max_slice by
     * one eighth per frame to keep guest_exec_ms near 85% of the panel period,
     * and MAX_SLICE_CEIL's own comment sizes it against the watchdog window.
     * That works because the cost of an instruction changes slowly.
     *
     * An attach changes it in ONE STEP and by a large factor: every guest page
     * is in NOR the instant this returns, so every access -- data AND
     * instruction fetch -- fails the fold check, traps to C, and is served out
     * of OCTOSPI instead of AXI. The first slice after the menu closes would
     * otherwise run the max_slice that was tuned for AXI at the new NOR price,
     * and an integrator that corrects by an eighth per frame cannot help with
     * a frame it does not survive. Same for the debt: the menu blocked the
     * frame loop for the whole snapshot, so dt on resume is enormous and the
     * throttled path would ask for a GUEST_DEBT_MAX_MS catch-up burst as its
     * opening move.
     *
     * Starting from the floor costs a few frames of ramp and lets the
     * regulator discover what an instruction now costs, which is the number
     * nobody has measured. It does not paper over a hang: the floor is a
     * SMALLER slice, so the interpreter still has to return.
     */
    max_slice = MAX_SLICE_FLOOR;
    guest_max_slice = max_slice;
    guest_insn_debt = 0;

    /* Closes the last interval, so nor_snap_wdog_gap_ms covers the tail after
       the readback (the repoint pass and x86_pager_reset()) too. */
    nor_snap_kick();

    nor_snap_result = NOR_SNAP_OK;
    nor_snap_ms = abi->HAL_GetTick() - t0;
}

/* The menu entry, next to CPU Speed. The value buffer must be static: the
   options array lives on poll_input's stack but the firmware keeps the value
   pointer, and it is what the entry shows after our callback returns. */
static char nor_snap_value[20] = "Off";

static bool nor_snap_cb(odroid_dialog_choice_t *option,
                        odroid_dialog_event_t event, uint32_t repeat)
{
    (void)repeat;
    nor_snap_events++;

    if (event == ODROID_DIALOG_ENTER && !x86_nor_attached)
        nor_snapshot_run();

    if (x86_nor_attached) {
        /* Pages still in NOR is the number worth showing: it is the answer to
           "how much of this guest is genuinely read-only". */
        strcpy(nor_snap_value, "NOR ");
        uint32_t n = x86_nor_pages_nor, i = 4;
        if (n >= 100) nor_snap_value[i++] = '0' + (n / 100) % 10;
        if (n >= 10)  nor_snap_value[i++] = '0' + (n / 10) % 10;
        nor_snap_value[i++] = '0' + n % 10;
        nor_snap_value[i++] = 'p';
        nor_snap_value[i] = 0;
    } else if (nor_snap_result && nor_snap_result != NOR_SNAP_OK) {
        strcpy(nor_snap_value, "Failed");
    } else {
        strcpy(nor_snap_value, "Off");
    }
    strcpy(option->value, nor_snap_value);
    return event == ODROID_DIALOG_ENTER;
}

/*
 * Repaint the guest frame under the overlay.
 *
 * NOT optional. Retro-Go's repaint_overlay() calls this callback
 * UNCONDITIONALLY -- `repaint()` with no NULL test -- so passing NULL to
 * common_emu_input_loop() branches to address 0 the moment PAUSE/SET opens the
 * menu: CFSR INVSTATE, HardFault, BSOD. (The save/load-state handlers ARE
 * NULL-guarded by odroid_system_emu_save_state/load_state; this one is not.)
 *
 * The menu draws into the ACTIVE buffer and calls us whenever it needs the
 * game picture back underneath, so render into the active buffer and do not
 * swap -- the menu owns the swap.
 */
static void dma2d_sync(void);       /* defined with the hook, below */

static void core_repaint(void)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    uint8_t *fb = (uint8_t *)abi->lcd_get_active_buffer();
    if (fb) {
        x86_video_render(fb, FRAME_W);
        /* We are inside the retro-go overlay here, which is also the ONLY other
           user of DMA2D on this device (the JPEG cover-art path). Never leave a
           transfer in flight across the return into it. */
        dma2d_sync();
        /* The OSK coexists with retro-go's overlay rather than fighting it:
           the overlay draws ON TOP of whatever we leave in the active buffer
           and asks us for the picture back underneath, so the keyboard is part
           of that picture. */
        x86_osk_draw(fb, FRAME_W);
    }
}

/* ---------------------------------------------------------------------------
 * PRESENT ON THE GUEST'S PAGE FLIP.
 *
 * Mode Y aliases every page into one page's worth of storage (there is no room
 * for a second: x86_video_planar.c), so at any instant that storage holds
 * whatever the guest is drawing RIGHT NOW. Painting at the emulator's own frame
 * boundary composites a finished frame with a half-drawn one -- measured on the
 * reference port against a 256 KB card, and reported as "vertical slabs from
 * different frames". A display-start change is the guest stating that the page
 * it just left is COMPLETE, which is the only instant an aliased page is known
 * good, so that is when the screen is painted.
 *
 * It costs NO MEMORY -- the alternative, a second real page, is 66,560 bytes
 * that do not exist -- and FEWER blits than before whenever the guest runs
 * below the panel rate, which on this device it does.
 *
 * BOTH BUFFERS ARE PAINTED, and that is not belt and braces. The firmware
 * double-buffers and the swap alternates between them, so painting only the
 * active one makes the panel flip between this frame and the one before it --
 * the failure the previous port documented for its letterbox bars.
 *
 * We are called from inside an OUT instruction, i.e. from inside the guest
 * slice, so we do NOT swap: the frame loop's own swap presents what we leave.
 */
#ifndef X86_MODEY_PRESENT_ON_FLIP
#define X86_MODEY_PRESENT_ON_FLIP 1     /* 0 takes it back to the tearing model */
#endif
/* When the frame loop may paint a Mode Y screen itself is decided by
   x86_video_modey_paint_due() in x86_video_planar.c -- a QUIET aperture, not a
   timer. A timer is what caught the page mid-redraw; the reasoning is there. */

volatile uint32_t x86_modey_presents;   /* host-visible: flips actually painted */

/*
 * Paint BOTH buffers. This is the whole of Mode Y's presentation and it is not
 * belt and braces.
 *
 * The frame loop calls lcd_swap() unconditionally, every iteration, whether or
 * not it painted -- and it must, because the swap is also the loop's pacing.
 * So the panel alternates between the two buffers at the panel rate, and any
 * Mode Y paint that touched only the ACTIVE one leaves the two holding
 * DIFFERENT PICTURES. That is the reported "the selection appears doubled and
 * pops back and forth before settling": not a compositing artefact at all, but
 * two buffers disagreeing and the swap showing each in turn. The flip path
 * always painted both; the frame loop's fallback painted one, which is the bug.
 */
/* ---------------------------------------------------------------------------
 * CHROM-ART (DMA2D) FOR THE CHAINED 13h BLIT.  DEFAULT OFF -- UNVERIFIED.
 *
 * WHY THIS IS WORTH ANYTHING AT ALL, AND IT IS NOT THE WORD "PARALLEL".
 *
 * The LCD framebuffer is mapped by the firmware's MPU regions 3-6 as TEX level
 * 0, NOT cacheable, NOT bufferable (references/game-and-watch-retro-go-sd/
 * Core/Src/main.c:1358-1366). On ARMv7-M that encoding -- TEX=000, C=0, B=0 --
 * is STRONGLY-ORDERED memory: no line fills, no write merging, and no store
 * buffer, so every store the blit issues stalls until AXI SRAM acknowledges it.
 * The 64,000-byte identity copy is therefore not a memcpy at memcpy speed; it
 * is 16,000 serialised round trips to the bus. That is why the `video` zone
 * reads 0.32-0.51% under gwemu (STATUS.md:404) and cannot possibly read that on
 * silicon: gwemu charges one cycle for a strongly-ordered store.
 *
 * DMA2D is a separate AXI master. It does not care that the destination is
 * strongly-ordered -- that attribute is the CPU's view through the MPU, not a
 * property of the memory -- so the same 64,000 bytes move at engine speed. The
 * saving is the SUBSTITUTION, and it is roughly an order of magnitude. The
 * overlap with the audio refill below is worth a few microseconds and is taken
 * because it is free, but it is NOT the reason this exists. If the wait had to
 * happen immediately the change would still pay; if the framebuffer were merely
 * Normal-non-cacheable instead of strongly-ordered, it largely would not.
 *
 * WHAT IS AND IS NOT VERIFIED. gwemu models DMA2D (references/gwemu/hw/display/
 * gnw_h7b0_dma2d.c) and its M2M path is a raw byte move with the foreground
 * bpp, OPFCCR ignored, exactly as the reference manual says (:415-434) -- so
 * tier 2 can prove the PIXELS. It runs the transfer SYNCHRONOUSLY (:307), so
 * tier 2 can prove neither the timing nor that the wait below is correctly
 * placed: a missing poll would tear on silicon and be invisible under gwemu.
 * And CLAUDE.md's rule applies in full -- peripheral setup is not modelled.
 * Hence default 0. Flip it to 1 to measure on hardware; nothing else changes.
 *
 * RGB565 IS A LIE WE ARE ALLOWED TO TELL. The firmware's op hardcodes RGB565
 * in and out (gw_firmware_abi.c:95,104) and DMA2D has no L8 OUTPUT format at
 * all (sdk/include/Drivers/STM32H7xx_HAL_Driver/Inc/stm32h7xx_hal_dma2d.h:
 * 222-226). But DMA2D_M2M does no conversion -- it moves bytes -- so two L8
 * pixels are declared as one RGB565 pixel and 320x200 L8 becomes 160x200. This
 * only works because both offsets are hardcoded 0, which is only correct when
 * the rows abut; the hook contract in x86_video.c enforces that.
 */
#if X86_VIDEO_DMA2D
/* Same raw-SCS idiom as DWT_CYCCNT above: the core does not link CMSIS. */
#define SCB_DCCMVAC     (*(volatile uint32_t *)0xE000EF68)

volatile uint32_t x86_dma2d_blits;     /* host-visible: transfers started */
volatile uint32_t x86_dma2d_fallback;  /* host-visible: START refused     */
volatile uint32_t x86_dma2d_waits;     /* host-visible: polls that waited */
static int dma2d_pending;

/*
 * The aperture is CACHEABLE write-back -- the bonus pool above the framebuffer
 * footprint is left Normal cacheable on purpose (main.c:734-740, and the
 * firmware's own comment at Core/Src/main.c:1378-1380). The guest wrote those
 * pixels through the D-cache, so an engine reading AXI directly sees STALE
 * bytes unless they are cleaned out first. Clean, not clean-and-invalidate:
 * invalidating would throw away the guest's own working set for nothing.
 */
static void dcache_clean_range(const void *p, uint32_t bytes)
{
    uint32_t a = (uint32_t)(uintptr_t)p & ~31u;
    uint32_t end = (uint32_t)(uintptr_t)p + bytes;
    __asm volatile ("dsb 0xF" ::: "memory");
    for (; a < end; a += 32u)
        SCB_DCCMVAC = a;
    __asm volatile ("dsb 0xF" ::: "memory");
}

static int dma2d_blit_hook(uint8_t *dst, const uint8_t *src, uint32_t bytes)
{
    /* Two L8 pixels per declared RGB565 pixel, so an odd width has no honest
       encoding. It cannot happen at 320, but the fallback is one branch. */
    if (dma2d_pending || (bytes % (2u * X86_VGA13_HEIGHT)) != 0)
        return -1;
    /* DMA2D fetches halfwords here; both ends must be even. The aperture is
       page-aligned and the framebuffer row offset is 20*320, but assert it
       rather than inherit it. */
    if ((((uint32_t)(uintptr_t)dst | (uint32_t)(uintptr_t)src) & 1u) != 0)
        return -1;

    dcache_clean_range(src, bytes);

    uint32_t px = bytes / (2u * X86_VGA13_HEIGHT);   /* 160 */
    if (gw_firmware_abi()->dma2d_m2m_rgb565_start((uint32_t)(uintptr_t)src,
                                                  (uint32_t)(uintptr_t)dst,
                                                  (uint16_t)px,
                                                  X86_VGA13_HEIGHT) != 0) {
        x86_dma2d_fallback++;
        return -1;                       /* caller does the row loop instead */
    }
    dma2d_pending = 1;
    x86_dma2d_blits++;
    return 0;
}

/*
 * NOTHING may read or write the framebuffer between the start above and this.
 * The engine owns those 64,000 bytes until it says otherwise, and the SWAP is
 * the hard deadline: presenting a buffer with a transfer still in it is the
 * mid-draw artefact docs/08 spent four rounds removing, arriving by a new road.
 *
 * The timeout is not belt and braces either. gwemu completes synchronously, so
 * this returns immediately there and the bound is only ever load-bearing on
 * silicon -- where a wedged engine must cost one frame, not the machine.
 */
static void dma2d_sync(void)
{
    if (!dma2d_pending)
        return;
    dma2d_pending = 0;
    x86_dma2d_waits++;
    (void)gw_firmware_abi()->dma2d_poll(20u);
}

/*
 * ONE 64-BYTE TRANSFER AT INIT, BECAUSE THE REAL ONE IS NOT REACHABLE TO TEST.
 *
 * Measured, not assumed: with this built and the counters watched under gwemu,
 * WOLF3D reaches Mode Y within one second (x86_modey_presents=2 at t+1.0s) and
 * PRINCE_OF_PERSIA never leaves text (x86_video_mode_byte=0x0F03), so
 * x86_dma2d_blits stayed 0 for the whole window on both. There is no workload
 * in this tree that puts a CHAINED 13h screen up long enough to sample, which
 * means an A/B of the rendered picture proves only that the FALLBACK still
 * works -- it never exercised the engine at all.
 *
 * So the wiring gets its own witness. This proves the ABI call is reachable,
 * that START accepts the two-L8-pixels-as-one-RGB565 encoding, that POLL
 * returns, and that the bytes actually move -- on gwemu AND on silicon, with no
 * guest and no video mode involved. It does NOT prove the timing, and it does
 * not prove the wait below is placed correctly against the swap; nothing short
 * of hardware can. x86_dma2d_selftest: 0 = not run, 1 = pass, 2 = START
 * refused, 3 = bytes wrong.
 */
volatile uint32_t x86_dma2d_selftest;
static uint8_t dma2d_st_src[64];
static uint8_t dma2d_st_dst[64];

static void dma2d_selftest(void)
{
    for (unsigned i = 0; i < sizeof dma2d_st_src; i++) {
        dma2d_st_src[i] = (uint8_t)(i * 7u + 1u);
        dma2d_st_dst[i] = 0;
    }
    dcache_clean_range(dma2d_st_src, sizeof dma2d_st_src);
    if (gw_firmware_abi()->dma2d_m2m_rgb565_start(
            (uint32_t)(uintptr_t)dma2d_st_src,
            (uint32_t)(uintptr_t)dma2d_st_dst, 32u, 1u) != 0) {
        x86_dma2d_selftest = 2;
        return;
    }
    (void)gw_firmware_abi()->dma2d_poll(20u);
    /* The engine wrote AXI behind the D-cache, so this core's view of the
       destination is stale until it is invalidated. DCIMVAC, same raw-SCS
       idiom; the buffer is ours and was never dirty, so nothing is lost. */
    {
        uint32_t a = (uint32_t)(uintptr_t)dma2d_st_dst & ~31u;
        uint32_t end = (uint32_t)(uintptr_t)dma2d_st_dst + sizeof dma2d_st_dst;
        __asm volatile ("dsb 0xF" ::: "memory");
        for (; a < end; a += 32u)
            *(volatile uint32_t *)0xE000EF5C = a;      /* SCB->DCIMVAC */
        __asm volatile ("dsb 0xF" ::: "memory");
    }
    for (unsigned i = 0; i < sizeof dma2d_st_src; i++)
        if (dma2d_st_dst[i] != dma2d_st_src[i]) {
            x86_dma2d_selftest = 3;
            return;
        }
    x86_dma2d_selftest = 1;
}
#else
static void dma2d_sync(void) { }
#endif

static void modey_paint_both(const gw_firmware_abi_t *abi)
{
    /* VPROF: the PARENT bracket. x86_video_render() and x86_osk_draw() carry
       their own, so `paint - 2*(render + osk)` is the buffer-address and
       mode-check work around them. See x86_vprof.h on why these three are
       deliberately not exclusive. */
    X86_VPROF_BEGIN(pt);
    for (unsigned which = 0; which < 2; which++) {
        uint8_t *fb = (uint8_t *)(which ? abi->lcd_get_inactive_buffer()
                                        : abi->lcd_get_active_buffer());
        if (!fb)
            continue;
        x86_video_render(fb, FRAME_W);
        /* Unchained only, so the hook cannot have fired -- but a mode change
           between the two paints is the guest's to make, not ours to assume. */
        dma2d_sync();
        {
            X86_VPROF_BEGIN(ok);
            x86_osk_draw(fb, FRAME_W);
            X86_VPROF_END(ok, osk);
        }
    }
    X86_VPROF_END(pt, paint);
}

void x86_video_page_flip(void)
{
#if X86_MODEY_PRESENT_ON_FLIP
    /* VPROF: this call is reached from INSIDE x86_cpu_interp_asm(), so every
       cycle it costs is charged to the GUEST zone by x86_zones.h. That is the
       whole reason this instrument exists -- see x86_vprof.h. */
    X86_VPROF_COUNT(x86_vprof_paint_flip);
    modey_paint_both(gw_firmware_abi());
    x86_modey_presents++;
#endif
}

/* Does the frame loop still owe this frame a paint? The DECISION lives in the
   core (x86_video_modey_paint_due), beside the store counter it reads and where
   tests/test_video.py can drive it without an LCD; this is only the switch that
   takes the whole scheme back to the tearing model. */
static int frame_owes_paint(void)
{
#if X86_MODEY_PRESENT_ON_FLIP
    return x86_video_modey_paint_due();
#else
    return 1;
#endif
}

static void poll_input(const gw_firmware_abi_t *abi)
{
    odroid_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    abi->odroid_input_read_gamepad(&pad);

    /* Hand the frame to the firmware before consuming it ourselves. This is
       what makes PAUSE/SET open the Retro-Go overlay, and it owns save states,
       brightness, volume and exit. Reading the gamepad directly and stopping
       there -- which is what this did -- leaves the core running inside
       Retro-Go with no way to get back out of it. */
    /* The array lives on this stack frame and the menu runs INSIDE the call,
       so it is alive for exactly as long as it is used; only the value buffer
       has to outlive the call, and it is static. */
    odroid_dialog_choice_t options[] = {
        {CPU_OPTION_ID, "CPU Speed", cpu_profile_value, 1, &cpu_profile_cb},
        {NOR_OPTION_ID, "Cold RAM->NOR", nor_snap_value, 1, &nor_snap_cb},
        ODROID_DIALOG_CHOICE_LAST,
    };
    abi->common_emu_input_loop(&pad, options, core_repaint);

    input_polls++;
    {
        uint32_t raw = 0;
        for (unsigned i = 0; i < ODROID_INPUT_MAX && i < 32; i++)
            if (pad.values[i]) raw |= 1u << i;
        input_pad_raw = raw;
        input_pad_seen |= raw;
    }

    uint32_t keys = 0;
    if (pad.values[ODROID_INPUT_UP])     keys |= 1u << 0;
    if (pad.values[ODROID_INPUT_DOWN])   keys |= 1u << 1;
    if (pad.values[ODROID_INPUT_LEFT])   keys |= 1u << 2;
    if (pad.values[ODROID_INPUT_RIGHT])  keys |= 1u << 3;
    if (pad.values[ODROID_INPUT_A])      keys |= 1u << 4;
    if (pad.values[ODROID_INPUT_B])      keys |= 1u << 5;
    if (pad.values[ODROID_INPUT_START])  keys |= 1u << 6;
    if (pad.values[ODROID_INPUT_SELECT]) keys |= 1u << 7;
    if (pad.values[ODROID_INPUT_X])      keys |= 1u << 8;
    if (pad.values[ODROID_INPUT_Y])      keys |= 1u << 9;

    uint32_t prev = x86_io.input_keys;
    uint32_t changed = keys ^ prev;
    x86_io.input_keys = keys;
    input_keys_now = keys;

    /*
     * ---- The on-screen keyboard's door -------------------------------------
     *
     * GAME cycles OFF -> keyboard -> mouse -> OFF (x86_osk.c). This function
     * does not know that: it asks x86_osk_visible() whether the OSK owns the
     * buttons and routes on that alone, so growing the cycle changes nothing
     * here. GAME is bound to nothing in pad_scancode[] for exactly this.
     *
     * The frame the OSK OPENS on and the frame it CLOSES on both belong to the
     * OSK, and neither emits a guest key: a button still held across the
     * transition must not read as a fresh press on either side of it.
     *
     * Anything the guest still has DOWN is released first. Our controller is
     * edge-driven from this table, so a key whose button is "let go" by a mode
     * switch rather than by the user would stay down inside the guest forever
     * -- a break code is the only thing that can clear it.
     */
    {
        const uint32_t game = 1u << 6;
        const int was_up = x86_osk_visible();

        if ((changed & game) && (keys & game)) {
            if (!was_up) {
                for (unsigned i = 0; i < 10; i++) {
                    uint8_t sc = pad_scancode[i];
                    if (sc && (prev & (1u << i))) {
                        x86_kbd_scancode((uint8_t)(sc | 0x80));
                        input_scancodes++;
                    }
                }
            }
            x86_osk_cycle();
        }

        if (x86_osk_visible() || was_up) {
            if (x86_osk_visible() && was_up) {
                /* Real host time, so held-direction repeat runs at a fixed
                   rate rather than a frame-rate-dependent one. */
                x86_osk_set_now_ms(abi->HAL_GetTick());
                x86_osk_input(keys);
            }
            goto deliver;
        }
    }

    if (!changed)
        return;

    for (unsigned i = 0; i < 10; i++) {
        if (!(changed & (1u << i)))
            continue;
        uint8_t sc = pad_scancode[i];
        if (!sc)
            continue;              /* bit deliberately unbound */
        x86_kbd_scancode((keys & (1u << i)) ? sc : (uint8_t)(sc | 0x80));
        input_scancodes++;
    }
deliver:
    input_kbd_ht = (uint32_t)x86_io.kbd_head | ((uint32_t)x86_io.kbd_tail << 8);

    /* Raise IRQ 1 for anything the controller is holding. The OSK reaches the
       guest through this SAME path a button does -- x86_kbd_scancode() then
       IRQ 1 -- which is the whole point of porting only the front end.

       The decision lives in x86_kbd_raise_irq() because this site and the one
       in the slice loop below used to disagree about it, and the unguarded
       version here is what made an OSK keypress invisible until the keyboard
       was closed: it re-armed the one-deep pending slot and forced
       pit_counter to 1 every frame, including while the guest was inside its
       own INT 09h handler with IF clear and could not take the interrupt at
       all -- which does not deliver anything, it just ends the slice after one
       instruction. */
    input_irqs += (uint32_t)x86_kbd_raise_irq(cpu_state_ptr);
}

/*
 * Hand the guest one IRQ 0, if one is owed. The one-deep hard_int_pending slot
 * is the emulated 8259A's whole model of priority: a keyboard IRQ already in it
 * wins and the tick is COUNTED as lost rather than silently dropped, because a
 * guest whose music or movement runs off INT 08h loses a beat here and how
 * often that happens is exactly what nobody could see before.
 */
static void arm_tick_irq(void)
{
    if (!tick_irqs_owed)
        return;
    if (cpu_state_ptr->hard_int_pending) {
        guest_tick_irq_lost++;
        return;
    }
    cpu_state_ptr->hard_int_pending = 0x08;
    x86_pit_counter_forced(1);       /* NOT a raw store: see x86_pit_sync() */
    tick_irqs_owed--;
    guest_ticks++;
}

/* Stage identifiers for x86_frame_phase. Kept dense and in body order so a
   sample reads as a position in the iteration, not as a bare tag. */
#define X86_PHASE_FWLOOP   1
#define X86_PHASE_INPUT    2
#define X86_PHASE_PACING   3
#define X86_PHASE_GUEST    4
#define X86_PHASE_VIDEO    5
#define X86_PHASE_AUDIO    6
#define X86_PHASE_SWAP     7
#define X86_PHASE_TAIL     8

/*
 * Enter a stage: publish it, and REFRESH THE WATCHDOG.
 *
 * The refresh is the host-side guard, and it is deliberately here rather than
 * only at the bottom of the body. One refresh per iteration means the window
 * bounds the WHOLE frame, so a stage that overruns takes the device down with
 * a reset -- which destroys the evidence and, on a title that does it every
 * run, makes the machine unusable. Refreshing per stage bounds each STAGE
 * instead: a slow stage becomes a slow frame (visible in
 * x86_frame_phase_worst), and only a stage that genuinely never returns still
 * resets. That is the difference between a hang we can read and a reboot loop.
 */
#define FRAME_PHASE(id)                                                       \
    do {                                                                      \
        uint32_t now__ = abi->HAL_GetTick();                                  \
        uint32_t took__ = now__ - x86_frame_phase_ms;                         \
        if (took__ > x86_frame_phase_worst) {                                 \
            x86_frame_phase_worst = took__;                                   \
            x86_frame_phase_worst_id = x86_frame_phase;                       \
        }                                                                     \
        x86_frame_phase = (id);                                               \
        x86_frame_phase_ms = now__;                                           \
        x86_frame_phase_seq++;                                                \
        if (X86_PHASE_WDOG)                                                   \
            abi->wdog_refresh();                                              \
    } while (0)

/*
 * The guest slice's WALL-CLOCK escape.
 *
 * MAX_SLICE_CEIL bounds a slice in INSTRUCTIONS, and that bound is sound --
 * pit_counter is decremented once per instruction and the interpreter exits
 * when it reaches zero. What it cannot bound is TIME: an instruction is 55
 * cycles or, through the planar aperture with a shadow rebuild behind it, tens
 * of thousands, so a budget that finishes in 12 ms of ordinary code can take a
 * second of that. This turns the worst case into a dropped frame rather than a
 * stage that outlives the watchdog window.
 */
#ifndef GUEST_SLICE_WALL_MS
#define GUEST_SLICE_WALL_MS 120u
#endif

/* The per-stage refresh, separable so the two halves of the guard can be
   graded independently. X86_PHASE_WDOG=0 leaves the single bottom-of-body
   refresh master had, which is the negative control for the refresh half. */
#ifndef X86_PHASE_WDOG
#define X86_PHASE_WDOG 1
#endif

/* Wall-clock ceiling on ONE uninterruptible interpreter call. Well under the
   ~472 ms watchdog window, and above the 65-100 ms whole FRAMES a healthy
   title produces, so it fires on the pathological mix and nothing else. */
#ifndef GUEST_ASM_MS_MAX
#define GUEST_ASM_MS_MAX 60u
#endif

static void emulator_loop(const gw_firmware_abi_t *abi)
{
    /* Give the I/O layer the host clock BEFORE the guest runs: it is what
       timestamps speaker events, and without it every beep collapses onto the
       start of its buffer (the old point-sampled behaviour). */
    x86_io_time_ms = abi->HAL_GetTick;

    cpu_profile_init(abi);

    guest_reset();
    guest_halted = 0;
    tick_last_ms = abi->HAL_GetTick();
    tick_acc = 0;
    bda_acc = 0;
    tick_irqs_owed = 0;
    x86_io.spk_window_ms = tick_last_ms;
    rate_window_ms = tick_last_ms;
    rate_window_insns = guest_insns_total;
    rate_window_frames = frames_rendered;

    abi->odroid_audio_init(SAMPLE_RATE);
    /* START sets the half-buffer length; GET_LENGTH only reports whatever it
       was last set to (1077 by default), so asking first and starting with the
       answer is how the mismatch above stayed invisible. Start with the length
       we want, then read it back so the number we render is the firmware's. */
    abi->audio_start_playing(SPEAKER_FRAMES);
    uint32_t audio_frames = (uint32_t)abi->audio_get_buffer_length();
    if (audio_frames > SPEAKER_FRAMES)
        audio_frames = SPEAKER_FRAMES;

    /*
     * =======================================================================
     * WHO PACES THE DISPLAY -- one answer, deliberately.
     * =======================================================================
     *
     * CLOCK 1 (the panel's swap signal, at the bottom of this loop) OWNS
     * display pacing. It is the hardware's own period, it is what the learned
     * panel_ms_min is measured from, and it is what sizes the unthrottled
     * slice. Nothing here replaces it.
     *
     * The firmware's common_emu_frame_loop() is a full pacer too -- it decides
     * draw/skip from its own frame_time_10us integrator. Running both would be
     * exactly the two-driver bug that wrecked the BIOS tick. So it DEFERS:
     *
     *   - we call it once per iteration for its BOOKKEEPING half, which is the
     *     only reason the overlay's fps and busy% readouts exist at all
     *     (odroid_system_tick(!draw_frame, 0, cpumon_stats.busy_ms)). Never
     *     calling it is why they read 0.0 (0.0) fps and busy 0.0%.
     *   - we IGNORE its draw_frame verdict; we always draw, and the panel wait
     *     is what regulates us.
     *   - we feed it the MEASURED display period, so its integrator agrees
     *     with the pacer that is actually running and it does not report
     *     phantom skipped frames into the fps the user reads.
     *
     * busy%: cpumon_stats.busy_ms must accumulate BETWEEN frame_loop calls
     * (frame_loop cpumon_reset()s immediately after reading it). cpumon_sleep()
     * is the idle marker -- it closes the busy interval, WFIs, and opens a
     * sleep interval -- so it goes in the panel wait, the only place this loop
     * is genuinely idle. Calling neither leaves busy_ms 0 forever.
     */
    abi->common_emu_frame_loop_reset();

    while (1) {
        /* ZONES: open the frame. The root zone IS the unaccounted residual --
           see x86_zones.h. Everything from here to X86_ZONE_FRAME_END() that
           is not inside a named zone lands there, on purpose. */
        X86_ZONE_FRAME_BEGIN();
        /* VPROF: the DENOMINATOR. Every share this instrument reports is a
           share of MEASURED frame cycles, not of an assumed core clock -- the
           device is overclocked to a level the core is not told. Honouring the
           host's reset here, at the boundary, is what keeps a window from
           opening across a half-open bracket. */
        x86_vprof_frame_boundary();
        X86_VPROF_BEGIN(fr);

        /* Feed the observer the period we are ACTUALLY running at, smoothed.
           NOT panel_ms_min: that is the shortest iteration ever completed, and
           handing the firmware a period shorter than the real one makes its
           integrator saturate and mark three frames in four as skipped -- which
           we then render anyway, so the overlay's skipped-fps figure was pure
           fiction (measured: 768 "skipped" of 1032 drawn). */
        common_emu_state.frame_time_10us = (int16_t)(frame_10us_avg / 8u);
        /* Bookkeeping only -- see above. The verdict is deliberately unused. */
        FRAME_PHASE(X86_PHASE_FWLOOP);
        X86_ZONE_ENTER(X86_ZONE_FWLOOP);            /* ZONES */
        (void)abi->common_emu_frame_loop();
        X86_ZONE_EXIT();                            /* ZONES */

        /* The host can still ask for the CPU benchmark: it is how tiers 2 and
         * 3 stay comparable with tier 1, and it must not disappear just
         * because there is now a frame loop around it. It reprograms guest
         * state, so put the guest back afterwards. */
        if (run_benchmark_flag && core_ready) {
            run_benchmark();
            itcm_sum_post_bench = itcm_sum();
            itcm_byte_120_post = itcm_byte_120();
            run_benchmark_flag = 0;
            guest_reset();
            /* The benchmark is seconds of wall clock the guest did not live
               through. Re-base rather than let it read as owed ticks. */
            tick_last_ms = abi->HAL_GetTick();
            tick_acc = 0;
            bda_acc = 0;
            tick_irqs_owed = 0;
        }

        uint32_t frame_start_ms = abi->HAL_GetTick();

        FRAME_PHASE(X86_PHASE_INPUT);
        X86_ZONE_ENTER(X86_ZONE_INPUT);             /* ZONES */
        poll_input(abi);
        X86_ZONE_EXIT();                            /* ZONES */

        /*
         * CLOCKS 2 AND 3, both off the same reading of the host millisecond
         * counter: the 18.2065 Hz PIT/BIOS tick, and the guest's instruction
         * budget. See the three-clock block near the top of this file.
         *
         * The BDA count is advanced HERE rather than inside the INT 08h
         * service, because a game that hooks INT 08h (most do) replaces our
         * stub and the count would stop dead. IRQ 0 is raised alongside it so
         * a hooked handler still runs, and the instruction boundary is pulled
         * in so it lands now instead of at the end of the slice.
         */
        uint32_t slice;
        {
            uint32_t now = abi->HAL_GetTick();
            uint32_t dt  = now - tick_last_ms;      /* wraps correctly */
            tick_last_ms = now;
            guest_uptime_ms += dt;

            uint32_t stalled_ms = 0;
            if (dt > TICK_STALL_MS) {
                stalled_ms = dt - TICK_STALL_MS;
                dt = TICK_STALL_MS;   /* keeps dt * TICK_NUM inside 32 bits */
            }

            /* --- clock 3: how many instructions this iteration may run. */
            uint32_t kips = guest_kips;
            if (kips > 100000u)
                kips = 100000u;                /* keeps dt*kips in 32 bits */
            /* The PIT counters convert instructions into 1.193182 MHz
               ticks at exactly this rate, so the machine's clock and the
               speed it claims to run at are one number. */
            x86_pit_kips = kips;

            /*
             * How many instructions FIT in one display period, regulated from
             * measurement. This runs in both modes: unthrottled uses it as the
             * slice, paced uses it as a ceiling so an impossible target
             * degrades into a deficit instead of into a slideshow.
             *
             * It regulates on the time spent EXECUTING, not on the iteration
             * period: at 60 fps with idle to spare the iteration period equals
             * the display period whatever the slice is, so a controller
             * watching that has no gradient and hunts between its rails --
             * which it did, swinging the slice 20x frame to frame.
             *
             * The target is GUEST_EXEC_PCT of a display period, the remainder
             * being the render, the audio refill and the input poll.
             * panel_ms_min is the shortest iteration the hardware has ever let
             * us finish, i.e. the panel's own period -- learned, so an
             * overclocked or differently-timed display needs no constant here.
             *
             * IT WAS 75%, AND THAT WAS LEAVING A QUARTER OF THE MACHINE IDLE.
             * Measured on silicon: ~54% busy, 0 dropped frames, 60 fps, with
             * max_slice PINNED at the old 50,000 ceiling -- the controller's
             * "under target" arm firing every frame against a clamp it could
             * not pass. One iteration is one frame (the loop swaps
             * unconditionally and the swap paces to the panel), so throughput
             * was 50,000 x 60 = 3.0 M insn/s however fast the core really is.
             * That is also why 386SX 16 / 386DX 33 / Max scored alike: an
             * effective ~3000 cycles sits between the 2500 and 6000 rungs.
             *
             * The regulator is on TIME, not instruction count, which is what
             * makes raising this safe: it settles wherever GUEST_EXEC_PCT of a
             * frame actually lands, so a guest on a slow path (a planar store
             * storm, a REP through the fold) shrinks the slice by itself. The
             * instruction ceiling is the backstop for the pathological case
             * only -- see MAX_SLICE_CEIL.
             */
            {
                /*
                 * THE REFERENCE IS THE DISPLAY PERIOD, AND AN UNFLOORED
                 * panel_ms_min IS NOT IT. Measured on silicon during TOPBENCH:
                 * panel_ms_min read 6 ms and leaked up to 11 over 42 s, while
                 * guest_fps_achieved sat at 59 -- a real period of ~16.9 ms.
                 * It is a MINIMUM with
                 * a decay (`panel_ms_min++` every 512 frames), so it latches
                 * on the shortest iteration ever completed -- which happens
                 * during boot, in menus, and any frame the guest spent halted
                 * -- and then crawls back at 1 ms per ~8.5 s.
                 *
                 * The consequence was invisible because the controller was
                 * working perfectly: it drove guest_exec_ms to 10 ms against a
                 * 10 ms target and held it there. The target was simply 40%
                 * short, so the machine sat ~56% busy with zero dropped frames
                 * and the score crept upward as the leak raised the budget.
                 * That crawl is what looked "uncanny" from outside.
                 *
                 * DERIVING IT FROM guest_fps_achieved WAS TRIED AND IS WRONG.
                 * The measured rate is corrupted by the very overrun it would
                 * be used to correct: budget too big -> frames dropped -> fps
                 * falls -> the "period" reads longer -> budget grows again.
                 * Measured on silicon: fps 59 -> 43, guest_panel_ms_used
                 * pinned at its 20 ms clamp, exec 20 ms against a 16.9 ms
                 * frame. The clamp bounded the runaway; it did not stop it.
                 *
                 * panel_ms_min is the right source after all -- its ONLY fault
                 * was latching 6 ms during boot, when iterations really are
                 * short. A sane floor fixes that completely, and it does not
                 * feed back on anything we are regulating. Measured reading
                 * once the guest is running: 16 ms, which is the panel.
                 */
                uint32_t panel = panel_ms_min ? panel_ms_min : 16u;
                if (panel < PANEL_MS_MIN_SANE) panel = PANEL_MS_MIN_SANE;
                if (panel > PANEL_MS_MAX_SANE) panel = PANEL_MS_MAX_SANE;
                uint32_t target = panel * GUEST_EXEC_PCT;   /* x100 */
                guest_panel_ms_used = panel;
                if (last_slice_halted) {
                    /* Idle is not headroom: leave max_slice alone. */
                    guest_grow_blocked++;
                } else if (guest_exec_ms * 100u < target) {
                    max_slice += max_slice / 8u;
                    if (max_slice > MAX_SLICE_CEIL)
                        max_slice = MAX_SLICE_CEIL;
                } else if (guest_exec_ms * 100u > target) {
                    max_slice -= max_slice / 8u;
                    if (max_slice < MAX_SLICE_FLOOR)
                        max_slice = MAX_SLICE_FLOOR;
                }
                guest_max_slice = max_slice;
            }

            if (kips == 0u) {
                slice = max_slice;
                guest_insn_debt = 0;           /* no target, so nothing owed */
                /* And nothing CAN be owed, so the deficit is not merely
                   uninteresting here, it is undefined. Held at zero so the
                   readout means the same thing in both modes: in MAX the
                   number to read is guest_ips_achieved. */
                guest_insn_deficit = 0;
            } else {
                uint32_t debt = guest_insn_debt + dt * kips;
                uint32_t debt_max = kips * GUEST_DEBT_MAX_MS;
                if (debt > debt_max) {
                    /* Written off, and COUNTED. This is the emulated CPU
                       running slower than it was asked to, which is otherwise
                       completely invisible. */
                    guest_insn_deficit += debt - debt_max;
                    debt = debt_max;
                }
                slice = debt;
                uint32_t slice_max = kips * GUEST_SLICE_MAX_MS;
                if (slice > slice_max)
                    slice = slice_max;
                if (slice < kips)
                    slice = kips;              /* always make a millisecond's
                                                  worth of progress */
                guest_insn_debt = debt;
            }
            /* The ceiling is what actually FITS in a display period.
               GUEST_SLICE_MAX_MS bounds the slice in the EMULATED machine's
               milliseconds, and ask for a 100 MIPS CPU and 33 of those are
               3.3 M instructions -- a quarter of a second here, with no
               render, no input and no audio in it. */
            if (slice > max_slice)
                slice = max_slice;
            guest_slice_now = slice;

            /* --- clock 2: the BIOS tick, at the divisor the GUEST programmed.
             *
             * Channel 0 of the PIT is the system timer and the guest owns its
             * divisor. 65536 (18.2065 Hz) is the boot value and what every
             * guest that never touches port 0x40 keeps; a title that wants
             * finer time programs its own and drives everything it paces --
             * animation, movement, menu key-repeat -- off the resulting ISR.
             * Wolfenstein 3D writes 0x214B, i.e. 140 Hz, which is 7.7x the
             * boot rate: with the write dropped its whole sense of time ran at
             * 13% of real, which is what "rotating is slow and the menu is
             * sluggish" was. It is not a keyboard defect and not raw MIPS.
             *
             * The BDA 0x46C count stays on the 18.2065 Hz clock regardless --
             * it is DOS's time of day, and it must not accelerate because a
             * game reprogrammed the timer for itself. */
            uint32_t div = x86_pit0_divisor();
            guest_tick_divisor = div;

            bda_acc += dt * TICK_NUM;
            uint32_t bda_owed = bda_acc / TICK_DEN_MS;
            bda_acc -= bda_owed * TICK_DEN_MS;
            bda_owed += (stalled_ms * TICK_NUM) / TICK_DEN_MS;
            if (bda_owed > TICK_CATCHUP_MAX)
                bda_owed = TICK_CATCHUP_MAX;
            for (uint32_t i = 0; i < bda_owed; i++)
                x86_bios_tick();

            uint32_t den = div * 1000u;         /* <= 65,536,000: fits */
            tick_acc += dt * TICK_NUM;
            uint32_t owed = tick_acc / den;
            tick_acc -= owed * den;
            /* Time lost to a stall is still time: count it as dropped rather
               than pretending the target was never away. */
            owed += (stalled_ms * TICK_NUM) / den;

            /* The arrears cap is a TIME, not a count: 4 ticks was ~220 ms at
               the boot divisor, and holding it at 4 while the guest asks for
               140 Hz would throw away three quarters of its clock. */
            uint32_t cap = (TICK_CATCHUP_MAX * 65536u) / div;
            if (cap < TICK_CATCHUP_MAX)
                cap = TICK_CATCHUP_MAX;
            if (cap > TICK_IRQ_MAX)
                cap = TICK_IRQ_MAX;
            /* Anything last frame could not place (the one-deep slot was held
               by the keyboard) is time the guest never got. Count it here
               rather than letting the assignment below swallow it. */
            guest_ticks_dropped += tick_irqs_owed;
            tick_irqs_owed = owed > cap ? cap : owed;
            guest_ticks_dropped += owed - tick_irqs_owed;
        }

        /*
         * Run guest instructions until the slice's PIT budget expires. Other
         * exits are real events: a memory fault or an unimplemented opcode
         * stops the guest rather than looping on the same instruction forever,
         * which would look exactly like a hang.
         */
        /*
         * ONE IRQ 0 PER FRAME IS A CEILING ON THE GUEST'S CLOCK, and at the
         * boot divisor it was invisible: 18.2 Hz against a 60 Hz panel never
         * needs two in a frame. A guest asking for 140 Hz needs two to eight,
         * and the hard_int_pending slot is one deep -- so the frame's budget is
         * cut into that many sub-slices and one interrupt is armed before each.
         * The guest exits at every sub-slice boundary, which costs a handful of
         * host cycles and nothing in the interpreter (delivery itself still
         * happens IN-GUEST, see .handle_pit_or_irq).
         */
        uint32_t subs = tick_irqs_owed ? tick_irqs_owed : 1u;
        if (subs > TICK_IRQ_MAX)
            subs = TICK_IRQ_MAX;
        uint32_t sub_slice = slice / subs;
        if (sub_slice == 0)
            sub_slice = slice;
        /* The PIT clock is charged for everything run under the OLD reload
           before the new one lands, and the emulated CPU's declared speed is
           what converts instructions into 1.193182 MHz ticks. */
        x86_pit_sync();
        cpu_state_ptr->pit_reload = sub_slice;
        arm_tick_irq();

        FRAME_PHASE(X86_PHASE_GUEST);
        uint32_t exec_start_ms = abi->HAL_GetTick();
        while (!guest_halted) {
            /* THE WALL-CLOCK ESCAPE. See GUEST_SLICE_WALL_MS. Checked at the
               sub-slice boundary, which costs one tick read per boundary and
               nothing per instruction. */
            if ((uint32_t)(abi->HAL_GetTick() - exec_start_ms)
                    > GUEST_SLICE_WALL_MS) {
                guest_slice_overruns++;
                break;
            }
            /*
             * Keep the controller draining. A BUTTON produces at most two
             * codes, so one IRQ per frame was enough; an OSK keypress produces
             * up to six (Ctrl/Alt/Shift makes, the key, and their breaks) and
             * the BIOS handler takes exactly ONE code per interrupt. At one
             * IRQ per frame a shifted keystroke would take 100 ms to arrive.
             *
             * Re-raising here costs nothing: delivery happens IN-GUEST at the
             * next boundary (cpu_exec.S .handle_pit_or_irq reloads the PIT and
             * dispatches without exiting), so it does not shorten the slice. A
             * pending tick keeps priority -- the one-deep slot is the emulated
             * 8259A's whole model of one.
             */
            input_irqs += (uint32_t)x86_kbd_raise_irq(cpu_state_ptr);
            /* IRQ 5, on the same terms and in the same place. A sound driver
               that starts a transfer and waits for the completion interrupt is
               the failure mode this whole path exists to remove, and the wait
               happens INSIDE a slice -- so the retry has to be here, at every
               sub-slice boundary, and not once per frame. The card's line stays
               asserted until the guest reads 0x22E, so a refusal costs nothing
               but a retry. */
            x86_sb_raise_irq(cpu_state_ptr);
            /* Re-entering the guest is the one moment at which no host pointer
               into guest memory can still be live: every HLE service that took
               one has returned. Clearing the ring here is what keeps it from
               growing stale and vetoing evictions forever. */
            x86_livemap_clear();
            X86_ZONE_ENTER(X86_ZONE_GUEST);         /* ZONES */
            /* THE WATCHDOG WINDOW IS PER FRAME, AND ONE CALL CAN OUTLIVE IT.
             * Refresh here as well: this loop can run several sub-slices and
             * each is bounded only in INSTRUCTIONS. Measured on KEEN4 --
             * without the per-stage refreshes the device BSODs with
             * BSOD_WATCHDOG (boot_magic 0xbad00005) inside this stage. */
            uint32_t asm_start_ms = abi->HAL_GetTick();
            abi->wdog_refresh();
            x86_cpu_interp_asm();
            X86_ZONE_EXIT();                        /* ZONES */
            /*
             * WHAT ONE CALL COST IN TIME, and the only bound on it.
             *
             * The instruction budget is not a time budget: an ordinary opcode
             * is ~55 cycles and one that lands on the EGA aperture behind a
             * shadow rebuild is 846,000 (docs/13), so the same 50,000-insn
             * budget is 18 ms of one mix and most of a second of another. The
             * interpreter cannot be interrupted mid-call, so the only lever is
             * the budget the NEXT call gets: halve it whenever a call overran,
             * and let the frame-loop regulator grow it back when it stops.
             */
            uint32_t asm_ms = abi->HAL_GetTick() - asm_start_ms;
            if (asm_ms > guest_asm_ms_worst)
                guest_asm_ms_worst = asm_ms;
            if (asm_ms > GUEST_ASM_MS_MAX) {
                guest_asm_overruns++;
                if (max_slice > MAX_SLICE_FLOOR) {
                    max_slice /= 2u;
                    if (max_slice < MAX_SLICE_FLOOR)
                        max_slice = MAX_SLICE_FLOOR;
                    guest_max_slice = max_slice;
                }
                if (sub_slice > MAX_SLICE_FLOOR) {
                    sub_slice /= 2u;
                    if (sub_slice < MAX_SLICE_FLOOR)
                        sub_slice = MAX_SLICE_FLOOR;
                    cpu_state_ptr->pit_reload = sub_slice;
                }
            }
            /* Bank the slice's guest time while pit_counter still holds what
               the guest left it at -- every host store to it below is a
               forced one and would otherwise be charged as elapsed time. */
            x86_pit_sync();
            uint32_t reason = cpu_state_ptr->exit_reason;
            last_slice_halted = (reason == X86_EXIT_HALT);
            if (last_slice_halted)
                guest_slice_halted++;
            guest_exit_reason = reason;
            if (reason == X86_EXIT_TIMER) {
                cpu_state_ptr->exit_reason = 0;
                /* The budget was spent, so that much of the debt is paid. */
                guest_insn_debt -= (guest_insn_debt < sub_slice)
                                 ? guest_insn_debt : sub_slice;
                if (--subs == 0)
                    break;
                /* Another sub-slice: arm the next timer interrupt and go back
                   in. Nothing else about the frame changes. */
                arm_tick_irq();
                continue;
            }
            if (reason == X86_EXIT_HLE_INT) {
                cpu_state_ptr->exit_reason = 0;
                X86_ZONE_ENTER(X86_ZONE_HLE);       /* ZONES */
                x86_hle_service();
                X86_ZONE_EXIT();                    /* ZONES */
                continue;
            }
            /*
             * HLT means "idle until the next interrupt", which is how a timing
             * loop waits for the periodic tick -- not "this machine is dead".
             * Latching it stopped TOPBENCH the moment it started benchmarking.
             * Raise the tick the guest is waiting for and pull the boundary
             * check forward so it lands immediately; EIP is already past the
             * HLT, which is where the handler's IRET belongs.
             */
            if (reason == X86_EXIT_HALT) {
                /* WHERE the guest halts decides whether the idle is real.
                   A benchmark's timing loop and a DOS keyboard wait produce
                   the same counter and mean completely different things: the
                   first is work we cannot have, the second is the guest
                   sitting at a prompt and nothing to do with throughput.
                   One ring of (CS<<16 | IP), plus a linear EIP, answers it. */
                x86_hlt_log[x86_hlt_log_idx & 15u] =
                    ((uint32_t)cpu_state_ptr->segs[1].sel << 16)
                    | (cpu_state_ptr->eip & 0xFFFFu);
                x86_hlt_log_idx++;
                x86_hlt_last_eip = cpu_state_ptr->eip;
                cpu_state_ptr->exit_reason = 0;
                if (!(cpu_state_ptr->eflags & 0x200u))
                    break;            /* interrupts masked: nothing can wake it */
                /* A tick still owed for this frame is exactly what a HLT is
                   waiting for, so give it one now rather than making it wait
                   for the next frame -- at 140 Hz the frame is several ticks
                   long and idling through them is time the guest never gets. */
                arm_tick_irq();
                if (cpu_state_ptr->hard_int_pending) {
                    /* Something is genuinely waiting. Pull the boundary in so
                       it lands now; EIP is already past the HLT, which is
                       where the handler's IRET belongs. */
                    x86_pit_counter_forced(1);
                    continue;
                }
                /* Nothing is due YET. This used to fabricate an INT 08h on
                   the spot, which is the frame-rate bug in its purest form: a
                   guest that HLTs in a wait loop got a timer interrupt every
                   time it asked, so its music ran at our loop rate instead of
                   at 18.2 Hz. Go and draw a frame instead; the real-time
                   driver raises the tick when it is actually owed. */
                break;
            }
            /* Fault, HLT or unimplemented: latch it. Re-entering re-executes
               the same faulting instruction forever, spinning the frame loop
               thousands of times a second and telling us nothing new. */
            guest_halted = 1;
            break;
        }

        guest_exec_ms = abi->HAL_GetTick() - exec_start_ms;

        /* Video. The beam fraction drives the CGA status register, so a guest
           polling for vertical retrace sees it arrive about once per frame. */
        FRAME_PHASE(X86_PHASE_VIDEO);
        X86_ZONE_ENTER(X86_ZONE_VIDEO);             /* ZONES */
        x86_video_set_beam(0xE000);
        uint8_t *fb = (uint8_t *)abi->lcd_get_active_buffer();
        fb_addr = (uint32_t)fb;
        if (fb) {
            /* A Mode Y title paints itself on its own page flips, so the frame
               boundary owes it nothing until the guest goes quiet -- which is
               the only moment the ONE aliased page is a complete picture, and
               is what keeps a title that STOPS flipping from being a frozen
               screen. Every other mode owes a paint every frame.
               When we do paint a Mode Y screen we paint BOTH buffers, for the
               reason on modey_paint_both(); every other mode paints the active
               one and lets the swap carry it, which is correct because it
               repaints unconditionally every frame. */
            if (frame_owes_paint()) {
                if (x86_vga_is_unchained()) {
                    /* VPROF: the FALLBACK paint. x86_modey_presents counts
                       only the guest's own flips, so this is the half of the
                       paint budget no counter has ever shown. */
                    X86_VPROF_COUNT(x86_vprof_paint_loop);
                    modey_paint_both(abi);
                    x86_video_modey_presented(0);
                } else {
                    x86_video_render(fb, FRAME_W);
                    /* x86_osk_draw() writes the framebuffer even with the
                       keyboard closed -- the toast band -- so this cannot be
                       conditioned on x86_osk_visible(). Correctness over the
                       few microseconds of overlap that would buy: the win here
                       is the SUBSTITUTION, not the parallelism (see the block
                       on dma2d_blit_hook). */
                    dma2d_sync();
                    x86_osk_draw(fb, FRAME_W);
                }
            }
            fb_sample = *(volatile uint32_t *)(fb + 20 * FRAME_W);
        }
        clut_sample = core_clut[1];
        hle_calls_seen = x86_hle_calls;
        hle_bad_vector = x86_hle_unhandled_vector;
        hle_bad_func = x86_hle_unhandled_func;
        guest_ax = cpu_state_ptr->regs[0] & 0xFFFF;
        x86_video_set_beam(0x0000);

        /* A guest write to 0x3D9 (or to the VGA DAC) changes the palette; the
         * firmware holds its own copy, so push ours again when it moves.
         *
         * WHICH op is not a count argument, it is a different firmware path.
         * lcd_set_clut() stages into a 68-entry cache and writes a 40%
         * darkened TWIN of entry i at count+i, which is how retro-go's overlay
         * dims the screen (it ORs LCD_DARKEN_BIT = 0x20 into every pixel). 256
         * entries would need 512 hardware slots against the LTDC's 256, so a
         * 256-colour mode has to own the whole table: lcd_set_clut_ext()
         * takes a CALLER-OWNED array and bypasses the cache entirely.
         *
         * Accepted cost, and it is visible: while a 256-colour mode is up the
         * darken does nothing useful -- OR-ing 0x20 picks an unrelated guest
         * colour -- so retro-go's menu backdrop SHIFTS COLOUR instead of
         * dimming. The menu box and its text are drawn after the darken pass
         * and stay readable. lcd_pack_color() searches the 256 entries, so
         * menu colours still resolve to a real nearest match.
         *
         * The array must stay valid until we hand the table back, which the
         * mode change below does by pushing the 32-entry path again. */
        const uint32_t *ext = x86_video_clut256();
        if (ext != clut256_pushed) {
            /* Entering or leaving a 256-colour mode. */
            if (ext)
                abi->lcd_set_clut_ext(ext, 256);
            else
                abi->lcd_set_clut(core_clut, CORE_CLUT_ENTRIES);
            clut256_pushed = ext;
            cga_clut_dirty = 0;
        } else if (cga_clut_dirty) {
            if (ext)
                abi->lcd_set_clut_ext(ext, 256);
            else
                abi->lcd_set_clut(core_clut, CORE_CLUT_ENTRIES);
            cga_clut_dirty = 0;
        }
        X86_ZONE_EXIT();                            /* ZONES: end VIDEO */

        /* Audio: one buffer of speaker output per frame.
         *
         * Amplitude follows the launcher's volume rather than a fixed 6000, so
         * the user's volume keys reach the speaker at all.
         *
         * >> 10 IS THE REFERENCE CORE'S SHIFT, AND THE >> 11 THAT WAS HERE WAS
         * 6 dB BELOW EVERY OTHER CORE ON THE DEVICE. The argument for the
         * extra halving -- "a PC-speaker square is full-swing, so it carries
         * more energy" -- ignored what common_emu_sound_get_volume() actually
         * returns: it is volume_tbl[level], ALREADY a fraction of full scale,
         * and the launcher's DEFAULT level is 4 of 9, i.e. 31. The two
         * attenuations compounded to 31/2048 = 1.5% of full scale at the
         * setting the device ships on, about -36 dBFS into a tiny mono
         * speaker. tama's main_tama.c uses >> 10 with the comment "adjust max
         * volume to 25% as 100% is REALLY, REALLY loud", and it is the core
         * that documented the choice, so match it rather than second-guess it.
         * MEASURED, not argued: x86_audio_peak_max reads 495 at >> 11 and 991
         * at >> 10 for the same PRINCE OF PERSIA speaker workload. */
        FRAME_PHASE(X86_PHASE_AUDIO);
        X86_ZONE_ENTER(X86_ZONE_AUDIO);             /* ZONES */
        int16_t *abuf = abi->audio_get_active_buffer();
        if (abuf && audio_frames) {
            int16_t amp = (int16_t)(((int32_t)abi->common_emu_sound_get_volume()
                                     * INT16_MAX) >> 10);
#if X86_AUDIO_TONE
            /* THE PARTITION TEST. Every emulated source is bypassed: a 440 Hz
               square at the launcher's own volume, written straight into the
               half-buffer the SAI DMA is reading. Nothing downstream of here
               is ours, so this separates "the guest never played anything"
               from "nothing we write is ever heard". */
            {
                static uint32_t phase;              /* 16.16, survives frames */
                uint32_t step = (uint32_t)(((uint64_t)440u << 16) / SAMPLE_RATE);
                for (unsigned i = 0; i < audio_frames; i++) {
                    abuf[i] = (phase & 0x8000u) ? amp : (int16_t)-amp;
                    phase = (phase + step) & 0xFFFFu;
                }
            }
#else
            x86_speaker_render(abuf, audio_frames, SAMPLE_RATE, amp);
            /* THE SOUND BLASTER IS MIXED ON TOP, AND THIS CALL IS ALSO WHAT
               ADVANCES THE DMA TRANSFER -- there is no per-instruction engine
               anywhere (x86_io.c's engine block says why). It pulls exactly the
               guest bytes this frame's samples needed, so the transfer runs at
               the sample clock, which is what the hardware does. A card at half
               amplitude against the speaker keeps a beep audible under music
               rather than being swamped by it.

               THE >> 1 THAT WAS HERE IS GONE, and the reason it was here is
               the reason it had to go: it ducked the card under the music, and
               at the time THERE WAS NO MUSIC. There is now -- x86_opl.c --
               so the collision it guarded against is real again; it is still
               not reinstated, because the measurement that removed it stands
               (silicon, PRINCE OF PERSIA: x86_audio_peak_max 495 -> 991 for
               the same 18 transfers) and the FM gain below was chosen against
               THIS card level rather than against the ducked one. The mix
               clamps either way. */
            x86_sb_render_mix(abuf, audio_frames, SAMPLE_RATE, amp);
#endif
            /* AND THE FM SYNTHESISER ON TOP OF BOTH, on the same clock and for
               the same reason: a title can play a digitised effect over its
               AdLib music and the three must SUM, not replace one another.
               It early-outs to nothing when no voice is sounding, so a title
               with no music pays one load and a test. Placed BEFORE the peak
               scan below on purpose -- x86_audio_peak_max is meant to answer
               "did anything reach the DMA", and FM is now one of the things
               that can. */
#if X86_OPL_FM
#if X86_OPL_BENCH
            /* MEASUREMENT ONLY. The cost of the synthesis in the REAL core --
               its code in RAM_EMU, the caches on, the frame loop around it --
               which is the only environment whose cycle count describes what
               the owner will feel. x86_opl_bench_all_on() holds all nine
               channels sounding so this is the chip's worst case. */
            {
                uint32_t t0 = DWT_CYCCNT;
                x86_opl_render_mix(abuf, audio_frames, SAMPLE_RATE, amp);
                x86_opl_frame_cycles = DWT_CYCCNT - t0;
            }
#else
            x86_opl_render_mix(abuf, audio_frames, SAMPLE_RATE, amp);
#endif
#endif
            /* WHAT WE ACTUALLY SUBMITTED, on every tier and every build. One
               pass over the buffer we just wrote: how many frames were handed
               to the DMA, and the loudest sample in the newest of them. A peak
               of 0 with submits climbing says the chain runs and the sources
               are silent; submits stuck at 0 says the frame loop never reaches
               the audio phase at all. Neither is inferable from the SB
               counters, and the difference is the whole question. */
            x86_audio_submits++;
            int32_t pk = 0;
            for (unsigned i = 0; i < audio_frames; i++) {
                int32_t s = abuf[i];
                if (s < 0) s = -s;
                if (s > pk) pk = s;
            }
            x86_audio_peak_last = (uint32_t)pk;
            if ((uint32_t)pk > x86_audio_peak_max)
                x86_audio_peak_max = (uint32_t)pk;
            x86_audio_volume = (uint32_t)abi->common_emu_sound_get_volume();
        }
        X86_ZONE_EXIT();                            /* ZONES: end AUDIO */

        /* Pace the loop. lcd_wait_for_vblank() is NOT usable here: gwemu does
           not model vblank, so it never returns and the core never draws --
           the screen stays black and nothing reaches the host. Wait on the
           swap the firmware has already queued instead, which is modelled and
           bounds the loop to the panel rate on both targets. */
        /* CLOCK 1: the DISPLAY rate, and it is the panel's, not a constant.
           Whether we had to wait here is also the only honest measure of
           headroom, so it is what sizes the unthrottled slice. */
        /* ZONES: SWAP is the loop's only genuine IDLE. A big number here is
           HEADROOM, not a cost -- which is why it must be its own zone and not
           part of the residual. */
        FRAME_PHASE(X86_PHASE_SWAP);
        X86_ZONE_ENTER(X86_ZONE_SWAP);
        abi->lcd_swap();
        {
            /* This wait is the loop's ONLY idle, so it is where the CPU
               monitor is told so -- cpumon_sleep() closes the busy interval
               (which is what the overlay's busy% reads) and WFIs until the
               next interrupt. A WFI can rest until the next SysTick, so the
               bound here is wall clock rather than a spin count: a swap that
               never completes must cost one frame, not 100000 sleeps. */
            uint32_t wait_start = abi->HAL_GetTick();
            while (abi->lcd_is_swap_pending()) {
                if ((uint32_t)(abi->HAL_GetTick() - wait_start) > 100u)
                    break;   /* never trade a black screen for a hang */
                abi->cpumon_sleep();
            }
        }
        X86_ZONE_EXIT();                            /* ZONES: end SWAP */
        frames_rendered++;
        /* Only if the guest actually ran. This is the slice BUDGET, not a
         * retired-instruction count, and it used to be added every frame
         * unconditionally -- so a guest latched at X86_EXIT_UNIMPLEMENTED went
         * on "executing" 3.0 M insn/s forever, and guest_ips_achieved (which is
         * derived from it) agreed. Measured on Commander Keen 4: guest_halted=1,
         * guest_exit_reason=4 and guest_ips_achieved=2,982,107 in the same
         * sample. An instrument that reports full speed for a dead guest is
         * worse than no instrument; the rate now falls to 0, which is the
         * signal. */
        if (!guest_halted)
            guest_insns_total += slice;
        FRAME_PHASE(X86_PHASE_TAIL);
        /* Master's ONE refresh per iteration. Unconditional, so
           X86_PHASE_WDOG=0 is a control on the EXTRA refreshes and not a build
           with no watchdog service at all. */
        abi->wdog_refresh();

        /* What this iteration really cost. */
        uint32_t frame_end_ms = abi->HAL_GetTick();
        frame_ms_last = frame_end_ms - frame_start_ms;

        /* The AVERAGE period, 8x oversampled, for the frame-loop observer. A
           mean, unlike panel_ms_min, is what an integrator comparing elapsed
           time against a period needs to sit at zero. */
        frame_10us_avg += (frame_ms_last * 100u) - (frame_10us_avg / 8u);
        if (frame_10us_avg > 8u * 30000u)
            frame_10us_avg = 8u * 30000u;   /* int16_t 10us units: 300 ms cap */

        /* The display period, learned rather than declared. It creeps upward
           so a panel that slows down (or an overclock that goes away) is
           re-learned instead of pinning us to a number that was true once. */
        /* The TAIL, which is what "stutter" means. Recorded against the panel
           period as learned SO FAR, and resettable from the host so a window
           can be opened around a specific event (the database load) instead of
           being diluted by everything since boot. */
        if (frame_stats_reset) {
            frame_stats_reset = 0;
            for (unsigned i = 0; i < 8; i++)
                frame_hist[i] = 0;
            frame_ms_worst = frame_ms_worst2 = frame_overruns = 0;
            frame_hist_from = frames_rendered;
            x86_disk_stats_reset();
        }
        frame_hist_add(frame_ms_last, panel_ms_min);

        /*
         * FLOORED AT THE LATCH, not just where the budget reads it.
         *
         * This is "the shortest iteration ever completed" standing in for the
         * PANEL PERIOD, and during boot -- and any frame the guest spent
         * halted, or a mode with nothing to draw -- an iteration genuinely
         * finishes in 6-9 ms. It then leaks back up at 1 ms per 512 frames and
         * everything derived from it is wrong in the meantime.
         *
         * Measured on silicon: a fresh boot re-latched 7 ms, so frame_overruns
         * (iterations over 1.5x this) counted 1,733 "overruns" in 20 s while
         * guest_fps_achieved sat at a steady 59 -- ordinary 16.7 ms frames
         * being reported as dropped. frame_hist's buckets are in units of this
         * value too, so the whole tail was skewed by the same factor.
         *
         * Flooring it here fixes the statistic AND the budget at once, and the
         * floor is safe because no panel this firmware drives runs faster than
         * PANEL_MS_MIN_SANE.
         */
        if (!panel_ms_min || frame_ms_last < panel_ms_min)
            panel_ms_min = frame_ms_last < PANEL_MS_MIN_SANE
                         ? PANEL_MS_MIN_SANE : frame_ms_last;
        else if ((frames_rendered & 511u) == 0u)
            panel_ms_min++;

        /* Achieved rates, over a one-second window. In unthrottled mode this
           is THE number -- there is no target to be short of, so "what did we
           actually get" is the only report that means anything. */
        if ((uint32_t)(frame_end_ms - rate_window_ms) >= 1000u) {
            uint32_t elapsed = frame_end_ms - rate_window_ms;
            guest_ips_achieved =
                (uint32_t)(((uint64_t)(guest_insns_total - rate_window_insns)
                            * 1000u) / elapsed);
            guest_fps_achieved =
                ((frames_rendered - rate_window_frames) * 1000u) / elapsed;
            rate_window_ms = frame_end_ms;
            rate_window_insns = guest_insns_total;
            rate_window_frames = frames_rendered;
        }

        /* ZONES: close the frame. Charges the last stretch to the root, which
           is the residual, and closes the frame occurrence. */
        X86_VPROF_END(fr, frame);
        X86_ZONE_FRAME_END();
    }
}
