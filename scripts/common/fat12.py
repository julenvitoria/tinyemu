"""
Build a tiny FAT12 volume in memory, for tests that need a disk with KNOWN
contents.

The shipped `.dsk` images are the wrong instrument for a file-I/O test: they
are 1.44 MB, their contents are whatever the title ships, and a test that
asserts on them is asserting on someone else's data. This builds the smallest
valid FAT12 volume instead -- 128 sectors, 64 KB -- with exactly the files the
test names, so the expected bytes are written by the test itself.

It is deliberately not a general FAT writer: root directory only, 8.3 names,
one sector per cluster. That is what a DOS floppy's root looks like, which is
where a single-floppy title keeps its data.

The result is written straight into the bare-metal harness's `dosfile_img`
buffer over the debug link and mounted with `x86_disk_set_memory`, so nothing
about it depends on the SD card or on extflash.
"""

import struct

SECTOR = 512
RESERVED = 1
NUM_FATS = 2
ROOT_ENTRIES = 16
SEC_PER_CLUS = 1


def _fat12_set(fat: bytearray, n: int, val: int):
    off = n + (n // 2)
    if n & 1:
        fat[off] = (fat[off] & 0x0F) | ((val << 4) & 0xF0)
        fat[off + 1] = (val >> 4) & 0xFF
    else:
        fat[off] = val & 0xFF
        fat[off + 1] = (fat[off + 1] & 0xF0) | ((val >> 8) & 0x0F)


def _dir_entry(name: str, attrib: int, cluster: int, size: int) -> bytes:
    stem, _, ext = name.partition(".")
    field = (stem[:8].ljust(8) + ext[:3].ljust(3)).upper().encode("ascii")
    return (field + bytes([attrib]) + b"\0" * 10 +
            struct.pack("<HHHI", 0x6000, 0x1A21, cluster, size))


def build_image(files, sectors: int = 128) -> bytes:
    """files: [(name, bytes)] in the root. Returns the whole volume."""
    fat_sectors = 1
    root_sectors = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
    data_start = RESERVED + NUM_FATS * fat_sectors + root_sectors
    total_clusters = sectors - data_start
    assert 0 < total_clusters < 4085, "not a FAT12 geometry"

    img = bytearray(sectors * SECTOR)

    boot = bytearray(SECTOR)
    boot[0:3] = b"\xEB\x3C\x90"
    boot[3:11] = b"MSDOS5.0"
    struct.pack_into("<HBHBHHBHHHII", boot, 11,
                     SECTOR, SEC_PER_CLUS, RESERVED, NUM_FATS, ROOT_ENTRIES,
                     sectors, 0xF8, fat_sectors, 1, 1, 0, 0)
    boot[36] = 0x00                       # drive number
    boot[38] = 0x29                       # extended boot signature
    struct.pack_into("<I", boot, 39, 0x12345678)
    boot[43:54] = b"TESTDISK   "
    boot[54:62] = b"FAT12   "
    boot[510:512] = b"\x55\xAA"
    img[0:SECTOR] = boot

    fat = bytearray(fat_sectors * SECTOR)
    _fat12_set(fat, 0, 0xFF8)
    _fat12_set(fat, 1, 0xFFF)

    root = bytearray(root_sectors * SECTOR)
    next_cluster = 2
    slot = 0
    for name, data in files:
        first = next_cluster
        need = max(1, (len(data) + SECTOR - 1) // SECTOR)
        assert next_cluster + need - 2 <= total_clusters, "image too small"
        for i in range(need):
            c = first + i
            _fat12_set(fat, c, 0xFFF if i == need - 1 else c + 1)
            off = (data_start + (c - 2) * SEC_PER_CLUS) * SECTOR
            chunk = data[i * SECTOR:(i + 1) * SECTOR]
            img[off:off + len(chunk)] = chunk
        root[slot * 32:(slot + 1) * 32] = _dir_entry(name, 0x20, first,
                                                     len(data))
        slot += 1
        next_cluster += need

    for i in range(NUM_FATS):
        off = (RESERVED + i * fat_sectors) * SECTOR
        img[off:off + len(fat)] = fat
    off = (RESERVED + NUM_FATS * fat_sectors) * SECTOR
    img[off:off + len(root)] = root
    return bytes(img)
