/*
 * Block device under our FatFs.
 *
 * Two backings, one interface:
 *
 *   memory  - the .dsk is already addressable (the bare-metal harness reads it
 *             memory-mapped out of extflash). Sector reads are memcpy.
 *   file    - the .dsk is a file on the SD card, read through the FIRMWARE's
 *             FatFs. This is the core's path: a 1.44 MB image costs no AXI,
 *             any .dsk in /roms/dos works without a rebuild, and we hold
 *             exactly one SD handle no matter how many files the guest opens.
 *
 * Note the two FatFs instances involved. Ours interprets the FAT12 inside the
 * .dsk; the firmware's reads the bytes of the .dsk off the card. They cannot
 * be the same instance: FF_VOLUMES is 1 in the firmware's build and its disk
 * I/O layer is compiled in, so it can hand us the image's contents but has no
 * way to mount them.
 */
#include <stdint.h>
#include <string.h>

#include "ff.h"
#include "diskio.h"

#define SECTOR_BYTES 512u

static const uint8_t *mem_base;          /* memory backing, or NULL */
static uint32_t disk_sectors;

/* --------------------------------------------------------------------------
 * Instrumentation. Host-readable through probe.py.
 *
 * The question this exists to answer is the one the stutter report could not:
 * is a guest read expensive because of PER-CALL overhead (the firmware reopens
 * the .dsk, walks its FAT and seeks on every request) or because of PER-BYTE
 * throughput (the SD transfer itself)? Caching and read-ahead fix opposite
 * halves of that, so guessing picks the wrong one half the time.
 *
 * Counts are exact under gwemu. CYCLES are not -- gwemu does not model SD busy
 * state (docs/04), so the SD is effectively free there and every time here
 * reads as near-zero. They are collected anyway because the same build then
 * answers the question on silicon with no rebuild.
 */
#ifdef X86_DISK_HOST_TEST
/* tests/test_disk_cache.py compiles this file for the HOST, where 0xE0001004
   is not a cycle counter but a segfault. The cache logic is pure arithmetic
   over a backend callback, so it is worth testing off-target where a boundary
   bug is found in milliseconds instead of a build-and-flash cycle. */
extern volatile uint32_t x86_disk_fake_cyccnt;
#define DWT_CYCCNT x86_disk_fake_cyccnt
#else
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004)
#endif

volatile uint32_t x86_disk_reads;          /* disk_read() calls              */
volatile uint32_t x86_disk_read_sectors;   /* sectors those calls asked for  */
volatile uint32_t x86_disk_backend_reads;  /* calls that reached the backend */
volatile uint32_t x86_disk_backend_sectors;
volatile uint32_t x86_disk_cycles_last;
volatile uint32_t x86_disk_cycles_worst;
volatile uint32_t x86_disk_us_total;
volatile uint32_t x86_disk_us_worst;
volatile uint32_t x86_disk_cycles_per_us = 280;   /* set by the core at init */
volatile uint32_t x86_disk_errors;
/* Sectors-per-call histogram: slot i is a call for i+1 sectors, slot 7 is 8+.
   This is what says whether FatFs asks for RUNS or one sector at a time --
   FF_FS_TINY changes that answer and no amount of reasoning substitutes for
   reading it off a running target. */
volatile uint32_t x86_disk_run_hist[8];

/* --------------------------------------------------------------------------
 * The sector cache, and why it has the shape it does.
 *
 * MEASURED FIRST (gwemu, TOPBENCH, tier 2): every single disk_read() is for
 * ONE sector -- x86_disk_run_hist is 115 in slot 0 and zero everywhere else.
 * FatFs never asks for a run, because INT 21h AH=3Fh services the guest through
 * a 512-byte bounce buffer, so f_read is handed exactly one sector at a time
 * and its multi-sector direct path (ff.c's `cc` case) is never taken.
 *
 * That matters because of what ONE of those calls costs on the other side. The
 * backend is abi->rg_storage_copy_file_range_to_ram, which is stdio and
 * RE-OPENS THE .dsk EVERY CALL: fopen -> f_open (a directory walk, LFN on),
 * fseek -> f_lseek, and with FF_USE_FASTSEEK 0 that walks the cluster chain
 * from the start of the file reading FAT sectors as it goes, then one f_read,
 * then fclose. The 512 bytes the guest asked for are the cheapest part of it.
 *
 * So the cost is PER-CALL, not per-byte, and that decides the mechanism. A
 * pure LRU cache would help the FAT/directory re-reads and do nothing for a
 * 124 KB stream; pure read-ahead would help the stream and thrash on the
 * metadata. This is both at once, and deliberately: a small set of large
 * aligned WINDOWS, LRU-replaced.
 *
 *   - The window IS the read-ahead. One miss fetches WINDOW_SECTORS in a
 *     single backend call, so the fixed open+seek cost is paid once for 16 KB
 *     instead of 32 times for 512 bytes.
 *   - The SET is the cache. FAT and root-directory sectors live at the front
 *     of the image and are re-read constantly; with more than one window they
 *     stay resident while the streaming read rotates through the others. With
 *     a single window they would evict each other on every alternation, which
 *     is the failure mode a "just add a buffer" design walks into.
 *
 * A request already at least a window long bypasses the cache entirely: it is
 * one backend call whether it is staged or not, and staging would only add a
 * copy.
 *
 * WHERE THE STORAGE COMES FROM: the AHB heap (GW_MEM_AHB), which is D2 SRAM.
 *
 * It used to come from the LCD bonus pool, and that was the wrong home on two
 * counts. This is a DISK STAGING BUFFER, and D2 is the only SRAM DMA1/DMA2 can
 * reach -- they cannot touch DTCM -- so it is where the buffer has to live the
 * day SPI1's polled HAL_SPI_TransmitReceive becomes a DMA transfer (docs/03
 * and STATUS's "asynchronous completion" note). GW_MEM_AHB is also the only
 * pool in the ABI with a real free(); every other one is a bump allocator.
 * And it hands the LCD bonus pool its 64 KB back, which is where the mode 13h
 * aperture now lives.
 *
 * The old comment here claimed the bonus pool was UNCACHED and priced a hit as
 * an uncached memcpy. That was false: the firmware's MPU covers only the
 * framebuffer footprint (154 KB in LUT8), and
 * mpu_set_lcd_pool_uncached_range() says in as many words that everything past
 * it "defaults to Normal cacheable, which is what we want for the LCD bonus
 * area". Both regions are cached; the move is about DMA reach and free(), not
 * about cache.
 *
 * It cannot come from RAM_EMU: measured headroom past __CORE_BSS_END__ is a
 * few KB and shrinking.
 *
 * The geometry is a build-time tunable so the trade can be MEASURED rather
 * than argued -- tests/test_disk_cache.py sweeps it. Defaults are the shipped
 * 4 x 16 KB, which measured 115 -> 7 backend calls on TOPBENCH.
 */
#ifndef X86_DISK_WINDOW_SECTORS
#define X86_DISK_WINDOW_SECTORS 32u              /* 16 KB per window */
#endif
#ifndef X86_DISK_WINDOW_COUNT
#define X86_DISK_WINDOW_COUNT   4u               /* 64 KB total      */
#endif
#define WINDOW_SECTORS X86_DISK_WINDOW_SECTORS
#define WINDOW_BYTES   (WINDOW_SECTORS * SECTOR_BYTES)
#define WINDOW_COUNT   X86_DISK_WINDOW_COUNT

volatile uint32_t x86_disk_cache_ok;        /* 0 = NOT backed. Loud, not silent */
volatile uint32_t x86_disk_cache_bytes;
volatile uint32_t x86_disk_cache_hits;      /* sectors served from a window     */
volatile uint32_t x86_disk_cache_misses;    /* sectors that forced a fetch      */
volatile uint32_t x86_disk_cache_fills;     /* window fetches                   */
volatile uint32_t x86_disk_cache_evictions;
volatile uint32_t x86_disk_cache_bypass;    /* requests too large to stage      */
volatile uint32_t x86_disk_cache_invalidates;

static uint8_t  *win_data;                  /* WINDOW_COUNT * WINDOW_BYTES      */
static uint32_t  win_base[WINDOW_COUNT];    /* first sector, window-aligned     */
static uint32_t  win_len[WINDOW_COUNT];     /* sectors valid, 0 = empty         */
static uint32_t  win_age[WINDOW_COUNT];     /* LRU stamp                        */
static uint32_t  win_clock;

/*
 * Hand the cache its storage. Returns 0 on failure, and the caller is expected
 * to report that -- a NULL a caller tolerates is a feature silently missing,
 * which is the trap upper_filler_ok already exists for. Passing NULL (or too
 * little) is not an error: the cache stays off and every read goes straight to
 * the backend, exactly as before.
 */
int x86_disk_cache_init(void *mem, uint32_t bytes)
{
    win_data = 0;
    x86_disk_cache_ok = 0;
    x86_disk_cache_bytes = 0;
    for (unsigned i = 0; i < WINDOW_COUNT; i++) {
        win_base[i] = 0;
        win_len[i] = 0;
        win_age[i] = 0;
    }
    win_clock = 0;

    if (!mem || bytes < WINDOW_COUNT * WINDOW_BYTES)
        return 0;
    win_data = (uint8_t *)mem;
    x86_disk_cache_bytes = WINDOW_COUNT * WINDOW_BYTES;
    x86_disk_cache_ok = 1;
    return 1;
}

/* Any window overlapping [sector, sector+count) is stale. Called on every
   write: the guest floppy really is written (TOPBENCH leaves a topbench.log),
   and serving a pre-write copy afterwards is silent corruption, not a
   performance bug. */
static void cache_invalidate(uint32_t sector, uint32_t count)
{
    if (!win_data)
        return;
    for (unsigned i = 0; i < WINDOW_COUNT; i++) {
        if (!win_len[i])
            continue;
        if (sector < win_base[i] + win_len[i] && win_base[i] < sector + count) {
            win_len[i] = 0;
            x86_disk_cache_invalidates++;
        }
    }
}

void x86_disk_cache_flush(void)
{
    cache_invalidate(0, 0xFFFFFFFFu);
}

void x86_disk_stats_reset(void)
{
    x86_disk_cache_hits = 0;
    x86_disk_cache_misses = 0;
    x86_disk_cache_fills = 0;
    x86_disk_cache_evictions = 0;
    x86_disk_cache_bypass = 0;
    x86_disk_cache_invalidates = 0;
    x86_disk_reads = 0;
    x86_disk_read_sectors = 0;
    x86_disk_backend_reads = 0;
    x86_disk_backend_sectors = 0;
    x86_disk_cycles_last = 0;
    x86_disk_cycles_worst = 0;
    x86_disk_us_total = 0;
    x86_disk_us_worst = 0;
    x86_disk_errors = 0;
    for (unsigned i = 0; i < 8; i++)
        x86_disk_run_hist[i] = 0;
}

static inline void disk_stat_run(unsigned count)
{
    unsigned i = (count >= 8u) ? 7u : (count - 1u);
    x86_disk_run_hist[i]++;
}

/* File backing. The reader is supplied by whoever owns the firmware's FatFs,
   so this file never has to know how to reach it. */
static int (*file_read)(void *ctx, void *dst, uint32_t offset, uint32_t len);
static int (*file_write)(void *ctx, const void *src, uint32_t offset,
                         uint32_t len);
static void *file_ctx;

void x86_disk_set_memory(const void *base, uint32_t bytes)
{
    mem_base = (const uint8_t *)base;
    disk_sectors = bytes / SECTOR_BYTES;
    x86_disk_cache_flush();      /* a different image behind the same windows */
    file_read = 0;
    file_write = 0;
    file_ctx = 0;
}

void x86_disk_set_reader(int (*rd)(void *, void *, uint32_t, uint32_t),
                         void *ctx, uint32_t bytes)
{
    file_read = rd;
    file_ctx = ctx;
    disk_sectors = bytes / SECTOR_BYTES;
    x86_disk_cache_flush();      /* a different image behind the same windows */
    mem_base = 0;
}

/*
 * Optional write-back for the file backing. Without it the guest floppy is
 * read-only, and a DOS program that creates a file gets an error where it
 * expects a handle -- TOPBENCH creates `topbench.log` before drawing its
 * full-screen UI and dies with a Borland `Runtime error` if it cannot.
 * Separate from the reader so a caller that cannot write (or does not want
 * to) simply does not install one.
 */
void x86_disk_set_writer(int (*wr)(void *, const void *, uint32_t, uint32_t))
{
    file_write = wr;
}

int x86_disk_ready(void)
{
    return (mem_base || file_read) && disk_sectors;
}

/* Compatibility with the harness's existing call site. */
void vdisk_init(void *base, uint32_t size)
{
    x86_disk_set_memory(base, size);
}

/* --------------------------------------------------------------------------
 * FatFs disk I/O
 */
DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0 || !x86_disk_ready())
        return STA_NOINIT;
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

/* The one place a backend transfer happens, so the one place it is timed. */
static int backend_read(void *dst, uint32_t sector, uint32_t count)
{
    uint32_t t0 = DWT_CYCCNT;
    int rc = file_read(file_ctx, dst, sector * SECTOR_BYTES,
                       count * SECTOR_BYTES);
    uint32_t dt = DWT_CYCCNT - t0;

    x86_disk_backend_reads++;
    x86_disk_backend_sectors += count;
    x86_disk_cycles_last = dt;
    if (dt > x86_disk_cycles_worst)
        x86_disk_cycles_worst = dt;
    {
        uint32_t per_us = x86_disk_cycles_per_us;
        uint32_t us = per_us ? (dt / per_us) : 0u;
        x86_disk_us_total += us;
        if (us > x86_disk_us_worst)
            x86_disk_us_worst = us;
    }
    if (rc != 0)
        x86_disk_errors++;
    return rc;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !x86_disk_ready())
        return RES_NOTRDY;
    if (sector + count > disk_sectors)
        return RES_PARERR;

    x86_disk_reads++;
    x86_disk_read_sectors += count;
    disk_stat_run(count);

    if (mem_base) {
        /* Already addressable. A cache in front of a memcpy would only add a
           second memcpy, so the memory backing never takes this path. */
        memcpy(buff, mem_base + (uint32_t)sector * SECTOR_BYTES,
               count * SECTOR_BYTES);
        return RES_OK;
    }

    /* No cache (allocation refused, or the harness never installed one): the
       original behaviour, byte for byte. One call for the whole run --
       consecutive sectors are the common case and seeking per sector would
       multiply the SD latency by count. */
    if (!win_data) {
        if (backend_read(buff, (uint32_t)sector, count) != 0)
            return RES_ERROR;
        return RES_OK;
    }

    /* Already at least a window's worth: staging it would buy nothing and cost
       a copy. Straight through, and the windows it crosses stay valid because
       nothing was written. */
    if (count >= WINDOW_SECTORS) {
        x86_disk_cache_bypass++;
        if (backend_read(buff, (uint32_t)sector, count) != 0)
            return RES_ERROR;
        return RES_OK;
    }

    /*
     * NEVER MORE BACKEND CALLS THAN THE UNCACHED PATH. This was measured, not
     * predicted: tests/test_disk_cache.py's random-access arm had the cache
     * issuing 3,024 backend calls where the uncached path issued 3,000.
     *
     * The cause is straddling. A request shorter than a window still spans TWO
     * windows when it crosses an alignment boundary, and if neither is
     * resident, serving it through the cache costs two fills where reading it
     * directly costs one call. Read-ahead is a bet that the next sector is
     * nearby; paying two SD round trips up front to place that bet is a bet
     * that has already lost.
     *
     * So: a multi-sector request that straddles a boundary with BOTH windows
     * missing goes straight through. Everything else -- the single-sector case
     * that dominates this workload, and any request with a window already
     * resident -- takes the cached path.
     */
    if (count > 1u) {
        uint32_t first = (uint32_t)sector & ~(WINDOW_SECTORS - 1u);
        uint32_t last  = ((uint32_t)sector + count - 1u) & ~(WINDOW_SECTORS - 1u);
        if (first != last) {
            unsigned resident = 0;
            for (unsigned k = 0; k < WINDOW_COUNT; k++) {
                if (win_len[k] && (win_base[k] == first || win_base[k] == last))
                    resident++;
            }
            if (resident == 0) {
                x86_disk_cache_bypass++;
                if (backend_read(buff, (uint32_t)sector, count) != 0)
                    return RES_ERROR;
                return RES_OK;
            }
        }
    }

    {
        uint32_t s = (uint32_t)sector;
        uint32_t left = count;
        uint8_t *out = buff;

        while (left) {
            uint32_t base = s & ~(WINDOW_SECTORS - 1u);
            unsigned i, victim = 0;
            int filled = 0;

            /* Resident? */
            for (i = 0; i < WINDOW_COUNT; i++) {
                if (win_len[i] && win_base[i] == base)
                    break;
            }

            if (i == WINDOW_COUNT) {
                /* Miss. Pick the least recently used window -- an empty one
                   first, since win_age of an unused slot is 0. */
                uint32_t oldest = 0xFFFFFFFFu;
                for (unsigned k = 0; k < WINDOW_COUNT; k++) {
                    if (!win_len[k]) { victim = k; oldest = 0; break; }
                    if (win_age[k] < oldest) { oldest = win_age[k]; victim = k; }
                }
                if (win_len[victim])
                    x86_disk_cache_evictions++;

                /* Do not read past the end of the image: the backend would
                   return short and every sector in the window would then be
                   served from whatever was left in the buffer. */
                uint32_t len = WINDOW_SECTORS;
                if (base + len > disk_sectors)
                    len = disk_sectors - base;

                win_len[victim] = 0;   /* stale until the fetch succeeds */
                if (backend_read(win_data + victim * WINDOW_BYTES, base,
                                 len) != 0)
                    return RES_ERROR;
                win_base[victim] = base;
                win_len[victim] = len;
                x86_disk_cache_fills++;
                i = victim;
                filled = 1;
            }

            win_age[i] = ++win_clock;

            {
                uint32_t off = s - base;              /* sectors into window  */
                uint32_t avail = win_len[i] - off;    /* to the window's end  */
                uint32_t n = (left < avail) ? left : avail;

                /* A short window that does not even cover the first requested
                   sector means the image is smaller than the FAT believes. The
                   bounds check at the top of this function already rejects
                   that, so reaching it would be a cache bug, not a guest one --
                   fail rather than serve stale bytes. */
                if (win_len[i] <= off || n == 0)
                    return RES_ERROR;

                memcpy(out, win_data + i * WINDOW_BYTES + off * SECTOR_BYTES,
                       n * SECTOR_BYTES);
                /* Sectors that forced a fetch are MISSES; sectors found
                   already resident are HITS. Charging a fill's own sectors as
                   hits would make read-ahead look free and the hit rate would
                   answer nothing. */
                if (filled)
                    x86_disk_cache_misses += n;
                else
                    x86_disk_cache_hits += n;

                out += n * SECTOR_BYTES;
                s += n;
                left -= n;
            }
        }
    }
    return RES_OK;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !x86_disk_ready())
        return RES_NOTRDY;
    if (sector + count > disk_sectors)
        return RES_PARERR;
    /* Still refused rather than silently dropped when there is nowhere to put
       the bytes: a guest that saves and is told it succeeded will lose the
       save and blame something else. The memory backing is a const mapping of
       extflash, so it is always in that position. */
    if (!file_write)
        return RES_WRPRT;
    /* Invalidate BEFORE the write, not after: if the write fails halfway the
       windows must still not claim to know what is on the card. */
    cache_invalidate((uint32_t)sector, count);
    if (file_write(file_ctx, buff, (uint32_t)sector * SECTOR_BYTES,
                   count * SECTOR_BYTES) != 0)
        return RES_ERROR;
    return RES_OK;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0 || !x86_disk_ready())
        return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = disk_sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = SECTOR_BYTES;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

/*
 * FatFs asks the host for a timestamp when it writes. There is no RTC here and
 * the disk is read-only anyway, so return a fixed, valid DOS date rather than
 * zero -- zero is not a legal encoding and some tools reject it.
 */
DWORD get_fattime(void)
{
    /* 1990-01-01 00:00:00 */
    return ((DWORD)(1990 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}
