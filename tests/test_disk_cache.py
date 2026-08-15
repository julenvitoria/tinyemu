#!/usr/bin/env python3
"""
Unit test for the .dsk sector cache and its read-ahead (`x86_disk.c`).

WHY THIS ONE IS NOT A TARGET TEST. Every other harness here drives real
hardware or gwemu, because what they check is the interpreter, the blitter or
the firmware's behaviour and none of that exists off-target. The sector cache
is different: it is pure arithmetic over a backend callback -- window
alignment, LRU replacement, short windows at the end of the image, and the
byte-for-byte equality of the cached and uncached paths. Those are exactly the
bugs an off-target test finds in milliseconds and a build-and-flash cycle finds
in ten minutes, and none of them need a Cortex-M7 to be wrong.

The property that matters most is the LAST one: for any sequence of reads, the
bytes delivered through the cache must be identical to the bytes the uncached
path would have delivered. Everything else is performance; that one is
correctness, and a cache that is merely fast is worthless.

    python3 tests/test_disk_cache.py
    python3 tests/test_disk_cache.py --keep-temp
"""
import argparse
import ctypes
import os
import random
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SECTOR = 512
# Defaults must match x86_disk.c. --window-sectors/--window-count rebuild the
# cache with a different geometry and re-run everything against it, which is
# what makes "is 64 KB the right size" a MEASUREMENT rather than an opinion --
# see --sweep.
WINDOW_SECTORS = 32
WINDOW_COUNT = 4
CACHE_BYTES = WINDOW_SECTORS * SECTOR * WINDOW_COUNT
DISK_SECTORS = 2880            # a 1.44 MB floppy, which is what a .dsk is

# A C shim, not a second implementation of the cache. It supplies the two
# things x86_disk.c expects from its environment -- a backend reader and the
# DWT stand-in -- and nothing else, so what is under test is the shipping file
# rather than a copy of it that can drift.
SHIM = r"""
#include <stdint.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"

volatile uint32_t x86_disk_fake_cyccnt;

extern void x86_disk_set_reader(int (*)(void *, void *, uint32_t, uint32_t),
                                void *, uint32_t);
extern void x86_disk_set_writer(int (*)(void *, const void *, uint32_t,
                                        uint32_t));
extern int  x86_disk_cache_init(void *, uint32_t);
extern void x86_disk_stats_reset(void);

static uint8_t *image;
static uint32_t image_bytes;

/* Counts BACKEND traffic, which is the thing the cache exists to reduce and
   the thing gwemu cannot lie about. */
uint32_t shim_calls, shim_bytes;
int      shim_fail_next;

static int shim_read(void *ctx, void *dst, uint32_t off, uint32_t len)
{
    (void)ctx;
    if (shim_fail_next) { shim_fail_next = 0; return -1; }
    if (off + len > image_bytes) return -1;
    shim_calls++;
    shim_bytes += len;
    memcpy(dst, image + off, len);
    /* Advance the fake cycle counter so the timing arithmetic in the file is
       actually executed rather than short-circuited by a constant. */
    x86_disk_fake_cyccnt += 1000 + len;
    return 0;
}

static int shim_write(void *ctx, const void *src, uint32_t off, uint32_t len)
{
    (void)ctx;
    if (off + len > image_bytes) return -1;
    memcpy(image + off, src, len);
    return 0;
}

void shim_setup(uint8_t *img, uint32_t bytes, void *cache, uint32_t cache_bytes)
{
    image = img;
    image_bytes = bytes;
    x86_disk_set_reader(shim_read, 0, bytes);
    x86_disk_set_writer(shim_write);
    x86_disk_cache_init(cache, cache_bytes);
    x86_disk_stats_reset();
    shim_calls = shim_bytes = 0;
}

/* Same, with the cache deliberately absent: the reference path. */
void shim_setup_uncached(uint8_t *img, uint32_t bytes)
{
    image = img;
    image_bytes = bytes;
    x86_disk_set_reader(shim_read, 0, bytes);
    x86_disk_set_writer(shim_write);
    x86_disk_cache_init(0, 0);
    x86_disk_stats_reset();
    shim_calls = shim_bytes = 0;
}

int shim_read_sectors(uint8_t *dst, uint32_t sector, uint32_t count)
{
    return (int)disk_read(0, dst, sector, count);
}

int shim_write_sectors(const uint8_t *src, uint32_t sector, uint32_t count)
{
    return (int)disk_write(0, src, sector, count);
}
"""


class Fail(Exception):
    pass


def build(tmp, tag=""):
    shim_c = os.path.join(tmp, "shim.c")
    with open(shim_c, "w") as f:
        f.write(SHIM)
    so = os.path.join(tmp, f"diskcache{tag}.so")
    cmd = ["cc", "-std=c11", "-O1", "-g", "-fPIC", "-shared", "-Wall",
           "-DX86_DISK_HOST_TEST=1",
           f"-DX86_DISK_WINDOW_SECTORS={WINDOW_SECTORS}u",
           f"-DX86_DISK_WINDOW_COUNT={WINDOW_COUNT}u",
           "-I" + ROOT, "-I" + os.path.join(ROOT, "lib", "FatFs"),
           os.path.join(ROOT, "x86_disk.c"), shim_c, "-o", so]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise Fail("compiling x86_disk.c for the host failed:\n" + r.stderr)
    return so


class Disk:
    def __init__(self, so, image):
        self.lib = ctypes.CDLL(so)
        self.image = (ctypes.c_uint8 * len(image)).from_buffer_copy(image)
        self.ref = bytes(image)
        self.cache = (ctypes.c_uint8 * CACHE_BYTES)()
        self.lib.shim_read_sectors.restype = ctypes.c_int
        self.lib.shim_write_sectors.restype = ctypes.c_int

    def _u32(self, name):
        return ctypes.c_uint32.in_dll(self.lib, name).value

    def enable_cache(self, cache_bytes=CACHE_BYTES):
        self.lib.shim_setup(self.image, len(self.ref), self.cache, cache_bytes)

    def disable_cache(self):
        self.lib.shim_setup_uncached(self.image, len(self.ref))

    def read(self, sector, count):
        buf = (ctypes.c_uint8 * (count * SECTOR))()
        rc = self.lib.shim_read_sectors(buf, sector, count)
        return rc, bytes(buf)

    def write(self, sector, data):
        buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        return self.lib.shim_write_sectors(buf, sector, len(data) // SECTOR)

    def expect(self, sector, count):
        return self.ref[sector * SECTOR:(sector + count) * SECTOR]

    @property
    def backend_calls(self):
        return self._u32("shim_calls")

    @property
    def backend_bytes(self):
        return self._u32("shim_bytes")

    @property
    def hits(self):
        return self._u32("x86_disk_cache_hits")

    @property
    def misses(self):
        return self._u32("x86_disk_cache_misses")

    @property
    def fills(self):
        return self._u32("x86_disk_cache_fills")

    @property
    def cache_ok(self):
        return self._u32("x86_disk_cache_ok")


def check(cond, msg):
    if not cond:
        raise Fail(msg)
    print(f"  ok   {msg}")


def make_image():
    # Deterministic, and every sector distinguishable from every other one --
    # a cache that returns the wrong window must not be able to hide behind
    # repeated bytes.
    rng = random.Random(0x5EC7)
    image = bytearray()
    for s in range(DISK_SECTORS):
        image += s.to_bytes(4, "little") + bytes(rng.getrandbits(8)
                                                 for _ in range(SECTOR - 4))
    return bytes(image)


def run(so):
    d = Disk(so, make_image())

    print("allocation")
    d.enable_cache()
    check(d.cache_ok == 1, "a sufficient buffer is accepted")
    d.enable_cache(CACHE_BYTES - 1)
    check(d.cache_ok == 0, "an UNDERSIZED buffer is refused, loudly, not "
                           "silently half-used")
    rc, got = d.read(10, 1)
    check(rc == 0 and got == d.expect(10, 1),
          "with the cache refused, reads still work (straight to the backend)")

    print("\nread-ahead: the measured workload, 1 sector at a time")
    d.enable_cache()
    for i in range(WINDOW_SECTORS):
        rc, got = d.read(100 + i, 1)
        if rc != 0 or got != d.expect(100 + i, 1):
            raise Fail(f"sequential read of sector {100 + i} returned wrong data")
    # Sectors 100..131 span two aligned windows (96..127 and 128..159), so a
    # correct implementation fetches exactly two.
    check(d.backend_calls == 2,
          f"32 single-sector reads cost {d.backend_calls} backend calls, not 32")
    check(d.fills == 2, "two windows filled (the request straddles an "
                        "alignment boundary)")
    check(d.hits == WINDOW_SECTORS - 2 and d.misses == 2,
          f"hits={d.hits} misses={d.misses}: only the sectors that FORCED a "
          "fetch are misses")

    print("\nboundary crossing, and the never-worse-than-uncached rule")
    d.enable_cache()
    base = WINDOW_SECTORS * 4          # aligned
    straddle = base + WINDOW_SECTORS - 2
    rc, got = d.read(straddle, 4)
    check(rc == 0 and got == d.expect(straddle, 4),
          "a 4-sector read straddling a window boundary returns correct bytes")
    check(d.backend_calls == 1 and d.fills == 0,
          "and with NEITHER window resident it went straight through in one "
          "backend call, rather than paying two fills to place a read-ahead "
          "bet it has already lost")

    d.enable_cache()
    d.read(base, 1)                    # make the first window resident
    calls_before = d.backend_calls
    rc, got = d.read(straddle, 4)
    check(rc == 0 and got == d.expect(straddle, 4),
          "the same straddling read is correct when one window IS resident")
    check(d.backend_calls == calls_before + 1,
          "and then it takes the cached path, costing one fill for the half it "
          "was missing -- still never more calls than uncached")

    print("\nlarge request bypasses staging")
    d.enable_cache()
    rc, got = d.read(200, WINDOW_SECTORS)
    check(rc == 0 and got == d.expect(200, WINDOW_SECTORS),
          "a request one window long is served correctly")
    check(d.fills == 0 and d.backend_calls == 1,
          "and it went straight through -- staging it would only add a copy")

    print("\nend of the image: a SHORT window")
    d.enable_cache()
    last = DISK_SECTORS - 1
    rc, got = d.read(last, 1)
    check(rc == 0 and got == d.expect(last, 1),
          "the final sector reads correctly (window clamped to the image)")
    check(d.backend_bytes <= (DISK_SECTORS - (last & ~(WINDOW_SECTORS - 1))) * SECTOR,
          "and the fetch did not run past the end of the image")
    rc, _ = d.read(DISK_SECTORS, 1)
    check(rc != 0, "a read past the end is refused")

    print("\nthe SET is doing its job (metadata stays resident under a stream)")
    d.enable_cache()
    d.read(1, 1)                        # a 'FAT' sector, window 0
    for i in range(WINDOW_SECTORS * 2):         # stream past it
        d.read(1000 + i, 1)
    fills_after_stream = d.fills
    rc, got = d.read(1, 1)              # the FAT sector again
    check(rc == 0 and got == d.expect(1, 1), "the metadata sector reads back")
    check(d.fills == fills_after_stream,
          f"and cost NO new fill (fills stayed at {d.fills}): the stream did "
          "not evict it. A single-window buffer would have thrashed here, "
          "which is why this is a SET and not one big staging buffer")

    print("\nwrites invalidate")
    d.enable_cache()
    d.read(300, 1)                                   # cache the window
    new = bytes((i * 7 + 3) & 0xFF for i in range(SECTOR))
    check(d.write(300, new) == 0, "the write is accepted")
    rc, got = d.read(300, 1)
    check(rc == 0 and got == new,
          "the read AFTER the write returns the NEW bytes, not the cached ones")

    print("\na failed fetch does not leave a window claiming to be valid")
    d.enable_cache()
    ctypes.c_int.in_dll(d.lib, "shim_fail_next").value = 1
    rc, _ = d.read(400, 1)
    check(rc != 0, "the backend error is reported")
    rc, got = d.read(400, 1)
    check(rc == 0 and got == d.expect(400, 1),
          "and the retry re-fetches instead of serving an uninitialised window")

    print("\nequivalence: cached bytes == uncached bytes, 3000 random reads")
    rng = random.Random(99)
    ops = []
    for _ in range(3000):
        count = rng.choice([1] * 12 + [2, 3, 4, 8, 17, WINDOW_SECTORS,
                                       WINDOW_SECTORS + 5])
        sector = rng.randrange(0, DISK_SECTORS - count)
        ops.append((sector, count))

    d.disable_cache()
    reference = [d.read(s, c) for s, c in ops]
    uncached_calls = d.backend_calls

    d.enable_cache()
    cached = [d.read(s, c) for s, c in ops]
    cached_calls = d.backend_calls

    for (s, c), a, b in zip(ops, reference, cached):
        if a != b:
            raise Fail(f"cached and uncached disagree at sector {s} x{c}")
    check(True, f"all {len(ops)} reads identical to the uncached path")
    check(cached_calls < uncached_calls,
          f"and cost fewer backend calls: {cached_calls} vs {uncached_calls}")

    print("\nsequential streaming, which is what a 124 KB read IS")
    d.enable_cache()
    for i in range(248):               # 124 KB in 512-byte pieces
        rc, got = d.read(500 + i, 1)
        if rc != 0 or got != d.expect(500 + i, 1):
            raise Fail(f"streaming read failed at sector {500 + i}")
    # 500..747 spans however many aligned windows the geometry gives.
    first = 500 // WINDOW_SECTORS
    last = 747 // WINDOW_SECTORS
    expected_fills = last - first + 1
    check(d.backend_calls == expected_fills,
          f"248 single-sector guest reads -> {d.backend_calls} backend calls "
          f"({248 / d.backend_calls:.0f}x fewer)")


def sweep(tmp):
    """What the cache geometry actually buys, measured on the two access
    patterns that matter: a stream (a 124 KB file read a sector at a time,
    which is what INT 21h AH=3Fh's 512-byte bounce buffer turns every read
    into) and a metadata-ish random walk.

    The point is the SHAPE of the curve, not one number. The stream is served
    by the WINDOW -- one fetch per window, so bigger windows always win it and
    the count is irrelevant. Random access is served by the SET -- more windows
    keep FAT and directory sectors resident while a stream rotates through the
    others. A single number cannot show that the two pull in opposite
    directions, so both are printed for every geometry.
    """
    global WINDOW_SECTORS, WINDOW_COUNT, CACHE_BYTES
    saved = (WINDOW_SECTORS, WINDOW_COUNT, CACHE_BYTES)
    image = make_image()

    rng = random.Random(99)
    ops = []
    for _ in range(3000):
        count = rng.choice([1] * 12 + [2, 3, 4, 8, 17])
        ops.append((rng.randrange(0, DISK_SECTORS - count), count))

    print(f"{'geometry':>18} {'KB':>5} | {'stream calls':>12} {'x':>5} |"
          f" {'random calls':>12} {'hit%':>6}")
    print("-" * 72)
    rows = []
    for count, secs in [(4, 32), (2, 32), (4, 16), (2, 16), (4, 8), (1, 32)]:
        WINDOW_COUNT, WINDOW_SECTORS = count, secs
        CACHE_BYTES = WINDOW_SECTORS * SECTOR * WINDOW_COUNT
        so = build(tmp, tag=f"_{count}x{secs}")
        d = Disk(so, image)

        d.enable_cache()
        for i in range(248):
            d.read(500 + i, 1)
        stream = d.backend_calls

        d.enable_cache()
        for s_, c_ in ops:
            d.read(s_, c_)
        rnd = d.backend_calls
        hits, misses = d.hits, d.misses
        rate = 100.0 * hits / (hits + misses) if (hits + misses) else 0.0

        kb = CACHE_BYTES // 1024
        print(f"{count} x {secs * SECTOR // 1024:>2} KB{'':>7} {kb:>5} |"
              f" {stream:>12} {248 / stream:>4.0f}x |"
              f" {rnd:>12} {rate:>5.1f}%")
        rows.append((count, secs, kb, stream, rnd, rate))

    WINDOW_SECTORS, WINDOW_COUNT, CACHE_BYTES = saved
    return rows


def main():
    global WINDOW_SECTORS, WINDOW_COUNT, CACHE_BYTES
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep-temp", action="store_true",
                    help="keep the compiled shared object for debugging")
    ap.add_argument("--window-sectors", type=int, default=WINDOW_SECTORS,
                    help="sectors per window (x86_disk.c default 32 = 16 KB)")
    ap.add_argument("--window-count", type=int, default=WINDOW_COUNT,
                    help="number of windows (x86_disk.c default 4)")
    ap.add_argument("--sweep", action="store_true",
                    help="measure backend calls and hit rate across several "
                         "cache geometries instead of running the checks")
    args = ap.parse_args()

    WINDOW_SECTORS = args.window_sectors
    WINDOW_COUNT = args.window_count
    CACHE_BYTES = WINDOW_SECTORS * SECTOR * WINDOW_COUNT

    tmp = tempfile.mkdtemp(prefix="diskcache-", dir=os.path.join(ROOT, "build"))
    try:
        if args.sweep:
            sweep(tmp)
            return 0
        so = build(tmp)
        run(so)
    except Fail as e:
        print(f"\nFAIL: {e}")
        return 1
    finally:
        if args.keep_temp:
            print(f"\nkept {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    print("\nPASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
