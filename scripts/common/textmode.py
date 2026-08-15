"""
Host-side model of the CGA text renderer.

`x86_video.c`'s text path turns an 80x25 buffer of (character, attribute) cells
into 8-bit palette indices. This recomputes the same thing in Python so a test
can assert the exact pixels the panel is being sent, rather than eyeballing 8x8
glyphs -- which is how a renderer that masked the character byte to 7 bits, and
so drew every box-drawing and shading code as a letter, went unnoticed.

Used by `tests/test_text.py` (tier 1, against a seeded pattern) and
`tests/test_screen.py` (tier 2, against whatever the real guest has drawn).
"""

import re

TEXT_COLS, TEXT_ROWS = 80, 25
VIS_COLS = 80                   # 320 px panel / 4 px cell -- the whole screen
CELL_W, CELL_H = 4, 8
CGA_HEIGHT = 200

# The 16 CGA colours in hardware index order, ARGB8888. Text attributes index
# all of them, which is why the CLUT pushed to the LCD must be 16 entries and
# not the four that mode 4 needs.
CGA_ARGB = [
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA,
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF,
]


TEXT_CLUT_ENTRIES = 32          # X86_TEXT_CLUT_ENTRIES
AA_LEVELS = 4
AA_BLOCKS = TEXT_CLUT_ENTRIES // AA_LEVELS
AA_NONE = 0xFF


def load_font(path="font4x8.h"):
    """The generated CP437 table: 256 glyphs of 8 rows, leftmost pixel bit 0.

    Defaults to the 4-wide table, which is what `text_render` draws from: 80
    columns of 8 px would be 640, so an 8x8 cell showed only half the screen.
    Pass `font8x8.h` for the 8-wide table the BIOS hands to the guest.
    """
    txt = open(path).read()
    rows = [[int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
            for body in re.findall(r"\{([^}]*)\}", txt)]
    return [r for r in rows if len(r) == 8]


def _trunc(a, b):
    """C's integer division: truncate towards zero, not Python's floor."""
    return -(-a // b) if (a < 0) != (b < 0) else a // b


def glyph_flat(font_aa):
    """Which glyphs paint a single flat colour -- space, NUL, a solid block.

    They need no intermediate levels, so `text_alloc_palette` excludes them
    from the ranking; counting them would spend blocks on pairs that cannot
    use one.
    """
    return [all(r == g[0] for r in g) and g[0] in (0x00, 0xFF) for g in font_aa]


def alloc_palette(cells, font_aa):
    """Model of `text_alloc_palette`: rank the (fg,bg) pairs a frame uses and
    give the top eight a four-entry CLUT block each.

    Returns `(base, flat, clut, letterbox, keys)`. Every tie-break here matches
    the C deliberately -- the ranking scans keys ascending with a strict `>`,
    so the lowest key wins a tie, and a model that disagreed would report a
    palette bug on every screen with two equally common attributes.
    """
    flat_glyph = glyph_flat(font_aa)
    count = [0] * 128
    for i in range(TEXT_COLS * TEXT_ROWS):
        ch, attr = cells[i * 2], cells[i * 2 + 1]
        fg, bg = attr & 0x0F, (attr >> 4) & 0x07
        if fg == bg or flat_glyph[ch]:
            continue
        count[(bg << 4) | fg] += 1

    keys = []
    for _ in range(AA_BLOCKS):
        best = max(count)
        if best == 0:
            break
        keys.append(count.index(best))
        count[keys[-1]] = 0

    base = [AA_NONE] * 128
    clut = [0] * TEXT_CLUT_ENTRIES
    for b, k in enumerate(keys):
        base[k] = b * AA_LEVELS
        f, g = CGA_ARGB[k & 15], CGA_ARGB[(k >> 4) & 7]
        for lv in range(AA_LEVELS):
            v = 0xFF000000
            for s in (0, 8, 16):
                a, z = (g >> s) & 0xFF, (f >> s) & 0xFF
                v |= (a + _trunc((z - a) * lv, AA_LEVELS - 1)) << s
            clut[b * AA_LEVELS + lv] = v

    n = len(keys) * AA_LEVELS
    flat = []
    for c in range(16):
        best, besti = None, 0
        for e in range(n):
            d = sum((((clut[e] >> s) & 0xFF) - ((CGA_ARGB[c] >> s) & 0xFF)) ** 2
                    for s in (0, 8, 16))
            if best is None or d < best:
                best, besti = d, e
        flat.append(besti)
    for e in range(n, TEXT_CLUT_ENTRIES):
        clut[e] = CGA_ARGB[0]
    return base, flat, clut, (flat[0] if keys else 0), keys


def render(cells, font, left, stride=320, height=240, font_aa=None):
    """The 8-bit palette-index image `text_render` should produce.

    `left` is the leftmost visible column. All 80 fit now, so it is clamped to
    0; the argument survives because the window machinery does. The picture is
    centred vertically in the panel, so there is a blank band above and below.

    Pass `font_aa` (the 2-bits-per-pixel table) to model the ANTI-ALIASED path:
    pairs that won a CLUT block are drawn as levels offset by the block base,
    and everything else degrades to bilevel in the nearest published colour.
    """
    left = min(left, TEXT_COLS - VIS_COLS)
    top = ((height - CGA_HEIGHT) // 2) * stride
    if font_aa is None:
        base = flat = None
        clear = 0
    else:
        base, flat, _clut, clear, _k = alloc_palette(cells, font_aa)
    img = bytearray([clear]) * (stride * height)
    for r in range(TEXT_ROWS):
        for vc in range(VIS_COLS):
            col = left + vc
            if col >= TEXT_COLS:
                ch, attr = 0x20, 0x07
            else:
                o = (r * TEXT_COLS + col) * 2
                ch, attr = cells[o], cells[o + 1]
            fg = attr & 0x0F
            bg = (attr >> 4) & 0x07         # bit 7 is blink, not a colour
            blk = base[(bg << 4) | fg] if base else AA_NONE
            for gy in range(CELL_H):
                o = top + (r * CELL_H + gy) * stride + vc * CELL_W
                if blk != AA_NONE:
                    bits = font_aa[ch][gy]
                    for gx in range(CELL_W):
                        img[o + gx] = blk + ((bits >> (2 * gx)) & 3)
                else:
                    bits = font[ch][gy]
                    f = flat[fg] if flat else fg
                    g = flat[bg] if flat else bg
                    for gx in range(CELL_W):
                        img[o + gx] = f if bits & (1 << gx) else g
    # The 20-row bands above and below the 200-row picture are already `clear`.
    return bytes(img)


def scrolled(cells, lines, attr):
    """INT 10h AH=06: shift up by `lines`, fill the vacated rows with spaces."""
    if lines == 0 or lines >= TEXT_ROWS:
        return bytes(bytearray([0x20, attr] * (TEXT_COLS * TEXT_ROWS)))
    out = bytearray(cells)
    keep = (TEXT_ROWS - lines) * TEXT_COLS * 2
    out[0:keep] = cells[lines * TEXT_COLS * 2:]
    out[keep:] = bytearray([0x20, attr] * (lines * TEXT_COLS))
    return bytes(out)


def dump(cells, cols=TEXT_COLS, rows=TEXT_ROWS):
    """The buffer as readable CP437 lines, blank rows dropped. Decoding as
    CP437 rather than ASCII matters: a DOS UI is mostly box drawing, and
    printing only 0x20-0x7E hides all of it."""
    out = []
    for r in range(rows):
        line = "".join(
            "." if cells[(r * cols + c) * 2] in (0x00, 0x20)
            else bytes([cells[(r * cols + c) * 2]]).decode("cp437")
            for c in range(cols))
        if line.strip("."):
            out.append((r, line))
    return out
