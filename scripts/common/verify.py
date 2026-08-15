"""
Verify that what is loaded on the target matches what we built.

Two checks, both of which have caught real bugs:

  * `check_regions` -- do symbols land in the memory they were designed for?
    The hot loop was linked into flash and the CPU state into AXI for the whole
    project before anyone looked.
  * `compare_section` -- do the bytes on the target equal the bytes in the ELF?
    A core segment that loads partially, or not at all, produces an
    UNDEFINSTR HardFault far from the cause.
"""

import pathlib
import struct
import subprocess

REGIONS = {
    "ITCM": (0x00000000, 0x00010000),
    "INTFLASH": (0x08000000, 0x08200000),
    "DTCM": (0x20000000, 0x20020000),
    "AXI": (0x24000000, 0x24100000),
    "AHB": (0x30000000, 0x30020000),
    "EXTFLASH": (0x90000000, 0x94000000),
}


def region_of(addr):
    for name, (lo, hi) in REGIONS.items():
        if lo <= addr < hi:
            return name
    return "?"


def check_regions(syms, expectations, out=print):
    """expectations: {symbol: "ITCM"}. Returns True if all satisfied."""
    ok = True
    for sym, want in expectations.items():
        if sym not in syms:
            out(f"  {sym:<28} MISSING from ELF")
            ok = False
            continue
        got = region_of(syms[sym])
        mark = "ok" if got == want else "WRONG"
        if got != want:
            ok = False
        out(f"  {sym:<28} 0x{syms[sym]:08x}  {got:<9} want {want:<9} {mark}")
    return ok


def section_bytes(elf, section):
    """Raw contents of one ELF section, via objcopy."""
    res = subprocess.run(
        ["arm-none-eabi-objcopy", "-O", "binary", f"--only-section={section}",
         elf, "/dev/stdout"],
        capture_output=True)
    return res.stdout


def section_vma(elf, section):
    """(vma, size) for a section, or (None, None)."""
    out = subprocess.run(["arm-none-eabi-objdump", "-h", elf],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1] == section:
            return int(parts[3], 16), int(parts[2], 16)
    return None, None


def compare_section(backend, elf, section, out=print, chunk=1024):
    """Compare target memory against an ELF section. Reports the first
    mismatch, which is what tells you whether a segment loaded at all,
    partially, or is simply misplaced."""
    vma, size = section_vma(elf, section)
    if vma is None:
        out(f"  {section}: not in {elf}")
        return False
    data = section_bytes(elf, section)
    if not data:
        out(f"  {section}: no contents")
        return False
    size = min(size, len(data))

    # Compare the WHOLE section. Stopping at the first mismatch cannot tell a
    # single stray write from a wholesale load failure, and that difference is
    # the entire diagnosis.
    diffs = []          # (offset, elf_byte, target_byte)
    for off in range(0, size, chunk):
        n = min(chunk, size - off)
        live = bytes(backend.read_memory(vma + off, n))
        want = data[off:off + n]
        if live == want:
            continue
        for i in range(n):
            if live[i] != want[i]:
                diffs.append((off + i, want[i], live[i]))

    if not diffs:
        out(f"  {section}: {size} bytes at 0x{vma:08x} MATCH")
        return True

    out(f"  {section}: {len(diffs)}/{size} bytes differ "
        f"({100.0 * len(diffs) / size:.2f}%)")
    # Collapse into contiguous runs so the shape is visible at a glance.
    runs, start, prev = [], diffs[0][0], diffs[0][0]
    for off, _, _ in diffs[1:]:
        if off == prev + 1:
            prev = off
            continue
        runs.append((start, prev))
        start = prev = off
    runs.append((start, prev))
    out(f"  {len(runs)} differing run(s); first {min(6, len(runs))}:")
    for a, b in runs[:6]:
        out(f"    +0x{a:04x}..+0x{b:04x} ({b - a + 1}B)")
    off = diffs[0][0]
    lo = max(0, off - 8)
    e = " ".join(f"{x:02x}" for x in data[lo:off + 8])
    l = " ".join(f"{x:02x}" for x in bytes(backend.read_memory(vma + lo, 16)))
    out(f"    elf    +0x{lo:x}: {e}")
    out(f"    target +0x{lo:x}: {l}")
    return False


def packed_segments(packed_bin):
    """[(region, code_size, bss_size, bytes)] from a packed CORE .bin.

    Layout (sdk/tools/pack_core.py): "CORE" + header_version(u16) +
    header_length(u16) + header_data + payloads back to back. header_data is
    gnw_core_meta_t: abi_version, abi_min_size, flags, segments_count,
    segments[4] of {region, code_size, bss_size}.
    """

    data = pathlib.Path(packed_bin).read_bytes()
    if data[:4] != b"CORE":
        raise ValueError(f"{packed_bin} is not a packed core")
    hdr_len = struct.unpack_from("<H", data, 6)[0]
    count = struct.unpack_from("<I", data, 8 + 12)[0]
    segs, off = [], 8 + hdr_len
    for i in range(count):
        region, code, bss = struct.unpack_from("<III", data, 8 + 16 + i * 12)
        segs.append((region, code, bss, data[off:off + code]))
        off += code
    return segs


REGION_NAMES = {0: "ram_emu", 1: "itcm"}


def compare_packed_to_elf(packed_bin, elf, region, section, out=print):
    """Does the packed payload for `region` equal the ELF's `section`?

    Separates a packaging fault from a runtime one: if these agree, whatever the
    target holds differs because something changed it after load.
    """
    segs = [s for s in packed_segments(packed_bin) if s[0] == region]
    if not segs:
        out(f"  {packed_bin}: no {REGION_NAMES.get(region, region)} segment")
        return False
    _, code, _, payload = segs[0]
    want = section_bytes(elf, section)[:code]
    if payload == want:
        out(f"  {REGION_NAMES.get(region, region)} segment ({code}B) "
            f"MATCHES {elf} {section}")
        return True
    for i in range(min(len(payload), len(want))):
        if payload[i] != want[i]:
            out(f"  {REGION_NAMES.get(region, region)} segment DIFFERS from "
                f"{section} at +0x{i:x}: elf=0x{want[i]:02x} "
                f"packed=0x{payload[i]:02x}")
            return False
    out(f"  size differs: packed={len(payload)} elf={len(want)}")
    return False
