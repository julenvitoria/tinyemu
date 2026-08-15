"""Recover the image the LTDC is actually scanning out.

gwemu's headless screendump renders nothing -- Retro-Go's own menu comes back
black -- so a timeline screenshot proves neither success nor failure. gwemu
DOES model the LTDC registers, though, which is enough: Layer 1 says where the
framebuffer is, how wide a line is and how many lines there are, and the
firmware keeps the active CLUT in readable RAM. Reading those and the
framebuffer reconstructs exactly what the panel receives, on emulator and on
silicon alike.

Shared by tests/test_screen.py (which asserts a known pattern) and
scripts/screenshot.py (which just captures whatever is there).
"""

import struct
import zlib

LTDC_BASE = 0x50001000
LTDC_L1CR = LTDC_BASE + 0x84        # layer enable
LTDC_L1PFCR = LTDC_BASE + 0x94      # pixel format (5 = L8)
LTDC_L1CFBAR = LTDC_BASE + 0xAC     # colour frame buffer address
LTDC_L1CFBLR = LTDC_BASE + 0xB0     # pitch / line length
LTDC_L1CFBLNR = LTDC_BASE + 0xB4    # line count

SCREEN_W, SCREEN_H = 320, 240


def write_png(path, w, h, rgb):
    """Minimal truecolour PNG. Avoids a Pillow dependency in the test path."""
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def read_ltdc(backend):
    """Layer-1 geometry as a dict, or None if this target does not model it."""
    fb = backend.read_uint32(LTDC_L1CFBAR)
    if fb == 0:
        return None
    blr = backend.read_uint32(LTDC_L1CFBLR)
    lnr = backend.read_uint32(LTDC_L1CFBLNR)
    return {
        "cr": backend.read_uint32(LTDC_L1CR),
        "pf": backend.read_uint32(LTDC_L1PFCR),
        "fb": fb,
        "pitch": (blr >> 16) & 0x1FFF,
        "lines": lnr & 0x7FF,
    }


def read_clut(backend, fw_syms, core_syms, entries=256):
    """The CLUT the firmware handed the LTDC.

    The LTDC's own CLUT registers are write-only, so the firmware's copy is the
    last readable point in the chain -- and reading it, rather than our own,
    proves the handoff happened instead of only that we built the right table.
    """
    sym = fw_syms.get("active_clut")
    src = "firmware active_clut"
    if sym is None:
        sym = core_syms.get("core_clut")
        src = "core copy (firmware symbol not found)"
    if sym is None:
        return None, "no CLUT symbol"
    return [backend.read_uint32(sym + 4 * i) for i in range(entries)], src


def read_active_clut(backend, fw_syms, core_syms, entries=32):
    """The palette the LTDC is REALLY scanning with, 256-colour modes included.

    `read_clut()` above reads `active_clut`, which is the darkened-twin cache
    the 32-entry contract uses. A 256-colour mode does not go through it at
    all: `lcd_set_clut_ext()` parks a caller-owned table in `ext_clut` and
    `clut_push()` programs the controller from that. Reading `active_clut` for
    a mode-13h frame therefore recovers the PREVIOUS mode's 32 CGA entries and
    paints every pixel in the wrong colour -- a defect of the TOOL that is
    indistinguishable from a palette defect in the core, which is why the
    golden gate must not use the plain reader. Returns (clut, source).
    """
    ext_p = fw_syms.get("ext_clut")
    ext_n = fw_syms.get("ext_clut_count")
    if ext_p and ext_n:
        addr = backend.read_uint32(ext_p)
        count = backend.read_uint32(ext_n) & 0xFFFF
        if addr and count:
            return ([backend.read_uint32(addr + 4 * i) for i in range(count)],
                    f"ext_clut ({count} entries)")
    return read_clut(backend, fw_syms, core_syms, entries)


def capture(backend, fw_syms, core_syms):
    """Return (info, indices, clut, rgb) for the currently scanned frame.

    `indices` is the raw L8 framebuffer as SCREEN_W*SCREEN_H bytes; `rgb` is
    the same run through the CLUT, ready for write_png.
    """
    info = read_ltdc(backend)
    if info is None:
        return None, None, None, None
    pitch, lines = info["pitch"], info["lines"]
    fb_bytes = bytes(backend.read_memory(info["fb"], pitch * lines))
    clut, info["clut_src"] = read_clut(backend, fw_syms, core_syms)

    indices = bytearray(SCREEN_W * SCREEN_H)
    rgb = bytearray(SCREEN_W * SCREEN_H * 3)
    for y in range(min(lines, SCREEN_H)):
        row = y * pitch
        for x in range(min(pitch, SCREEN_W)):
            idx = fb_bytes[row + x]
            indices[y * SCREEN_W + x] = idx
            argb = clut[idx] if clut and idx < len(clut) else 0
            o = (y * SCREEN_W + x) * 3
            rgb[o] = (argb >> 16) & 0xFF
            rgb[o + 1] = (argb >> 8) & 0xFF
            rgb[o + 2] = argb & 0xFF
    return info, bytes(indices), clut, bytes(rgb)
