#!/usr/bin/env python3
"""
Regenerate `font8x8.h` as a full 256-glyph CP437 table.

Why this exists: the renderer indexes the font with the raw character byte from
a text cell, and DOS text is CP437 -- box drawing at 0xB3-0xDA, shading at
0xB0-0xB2, blocks at 0xDB-0xDF. The tree previously carried only the 128 ASCII
glyphs of Daniel Hepper's public-domain `font8x8`, so `text_render` masked the
byte with 0x7F and every frame, shade and rule in a full-screen UI came out as
an unrelated lowercase letter. That is what made TOPBENCH's UI unreadable.

The table is assembled from Hepper's own blocks, which are indexed by Unicode:

    basic      U+0000-U+007F      ASCII
    ext_latin  U+00A0-U+00FF      accented latin, degree, plus-minus, ...
    greek      U+0390-U+03C9      alpha, beta, pi, sigma, Omega, ...
    box        U+2500-U+257F      single and double line drawing
    block      U+2580-U+259F      shades and half blocks
    misc       (two codepoints)   florin, pesetas

Python's `cp437` codec supplies the byte -> Unicode mapping for 0x20-0xFF. The
low 32 codes are control characters to the codec but graphics to a CGA, so they
are mapped explicitly here, and the 42 glyphs no Hepper block covers (the card
suits, arrows, and the maths operators at 0xEC-0xFE) are drawn below.

Bit order: the renderer takes the LEFTMOST pixel from bit 0, matching Hepper's
files. The hand-drawn glyphs are written most-significant-column-first as ASCII
art and reversed on emit, so what is written here reads the way it looks.

It also emits the two 4-pixel-wide tables the renderer draws from. 80 columns at
8 px is 640 px and the panel is 320, so an 8x8 cell can only ever show 40 of the
80 columns and a full-screen DOS UI -- which is centred on the 80 -- comes out
chopped in half. At 4 px the whole 80x25 screen lands exactly on 320x200.

`font4x8.h` -- 1 bit per pixel, four pixels in bits 0-3, leftmost in bit 0.
`font4x8aa.h` -- 2 bits per pixel, one byte per row, leftmost pixel in bits 0-1.

## Where the narrow glyphs come from

They are NOT a condensation of the 8x8 face. Condensing by OR-ing source column
pairs is what shipped first and it looks poor for an unavoidable reason: an 8x8
face encodes decisions -- stem placement, counters, the gap between `m`'s legs
-- that were made for eight columns, and halving the width destroys all of them
at once. Everything ends up two pixels of ink with the whitespace squeezed out.

So the letters come from **Bm437_EverexME_5x8**, the Everex Micro Enhancer face
in VileR's Int10h Oldschool PC Font Pack. That is a real character generator
that real hardware used to put 80 columns in a five-pixel cell, so its designer
already solved the narrow-face problem; 5 -> 4 is a small squeeze where 8 -> 4
is a different font. Measured over the whole 256: only 77 glyphs put ink in the
fifth column at all, and 48 of those are the line-drawing range that is drawn by
hand here anyway. For the rest the squeeze is exact -- column 4 is simply the
inter-character gap -- and where it is not, columns 3 and 4 are OR-ed, which
touches only a glyph's right edge and leaves the stems where the designer put
them.

**The line-drawing range 0xB0-0xDF is drawn here, not converted.** It decides
whether a narrow font is usable at all, because a DOS UI is mostly frames: a
horizontal rule MUST reach both cell edges or the frame comes out dashed, and a
vertical rule must sit in the same column in every glyph that contains one or
corners do not meet. `LINEART` below is that spec. Note the deliberate call it
records: **double vertical rules alias to single**. Two vertical strokes with a
one-pixel gap in a four-pixel cell is mush, and it makes every corner join
ambiguous. Double HORIZONTAL rules are kept, because vertically there are eight
rows to spend and two rules at rows 3 and 5 read clearly -- so a `╔═╗` frame
still looks different from a `┌─┐` one, just through its horizontals.

## The anti-aliased table

`font4x8aa.h` takes the *authentic 8x8* glyph and box-filters it 2:1
horizontally, keeping the intermediate level instead of thresholding: two lit
source pixels give level 3, one gives level 1 or 2, none gives 0. A stem that
falls between two output pixels becomes two half-lit pixels rather than
vanishing, which is the failure mode 1-bit condensation cannot avoid. The
line-drawing range is not filtered -- it is promoted from the hand-drawn bilevel
glyphs above, at full level -- because a frame wants to be crisp, and box
filtering a one-pixel rule would render every frame on the screen at half
brightness.

    python3 scripts/make_font.py                  # rewrite all three headers
    python3 scripts/make_font.py --check          # fail if any is out of date
    python3 scripts/make_font.py --cache build/fontwork
"""

import argparse
import os
import struct
import zipfile
import re
import sys
import urllib.request

BASE = "https://raw.githubusercontent.com/dhepper/font8x8/master"
BLOCKS = [
    ("font8x8_basic.h", 0x0000),
    ("font8x8_ext_latin.h", 0x00A0),
    ("font8x8_greek.h", 0x0390),
    ("font8x8_box.h", 0x2500),
    ("font8x8_block.h", 0x2580),
]
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "font8x8.h")
OUT4 = os.path.join(ROOT, "font4x8.h")
OUT4AA = os.path.join(ROOT, "font4x8aa.h")

# VileR's Int10h Oldschool PC Font Pack, vendored under references/.
FONT_PACK = os.path.join(ROOT, "references", "8086tiny",
                         "oldschool_pc_font_pack_v2.2_FULL.zip")
FONT_PACK_MEMBER = "fon - Bm (windows bitmap)/Bm437_EverexME_5x8.FON"

# CP437's low 32 codes, which the Python codec calls control characters.
CP437_LOW = ("\x00☺☻♥♦♣♠•◘○◙"
             "♂♀♪♫☼►◄↕‼¶"
             "§▬↨↑↓→←∟↔▲"
             "▼")

# Glyphs no Hepper block provides, drawn leftmost column first.
DRAWN = {
    0x263A: ".######.|#......#|#.#..#.#|#......#|#.####.#|#..##..#|#......#|.######.",
    0x263B: ".######.|########|##.##.##|########|##....##|###..###|########|.######.",
    0x2665: ".##.##..|#######.|#######.|#######.|.#####..|..###...|...#....|........",
    0x2666: "...#....|..###...|.#####..|#######.|.#####..|..###...|...#....|........",
    0x2663: "..###...|.#####..|..###...|#######.|#######.|...#....|...#....|..###...",
    0x2660: "...#....|..###...|.#####..|#######.|#######.|.#####..|...#....|..###...",
    0x2022: "........|........|...##...|..####..|..####..|...##...|........|........",
    0x25D8: "########|########|###..###|##....##|##....##|###..###|########|########",
    0x25CB: "........|..####..|.##..##.|.#....#.|.#....#.|.##..##.|..####..|........",
    0x25D9: "########|##....##|#..##..#|#.####.#|#.####.#|#..##..#|##....##|########",
    0x2642: "....####|.....###|....####|.#####.#|##..##..|##..##..|##..##..|.####...",
    0x2640: "..####..|.##..##.|.##..##.|.##..##.|..####..|...##...|.######.|...##...",
    0x266A: "..######|..##..##|..######|..##....|..##....|.###....|####....|###.....",
    0x266B: ".#######|.##...##|.#######|.##...##|.##...##|.##..###|###..##.|##......",
    0x263C: "#..##..#|.#.##.#.|..####..|###..###|###..###|..####..|.#.##.#.|#..##..#",
    0x25BA: "#.......|###.....|#####...|#######.|#####...|###.....|#.......|........",
    0x25C4: "......#.|.....###|..#####.|#######.|..#####.|.....###|......#.|........",
    0x2195: "...##...|..####..|.######.|...##...|...##...|.######.|..####..|...##...",
    0x203C: ".##..##.|.##..##.|.##..##.|.##..##.|.##..##.|........|.##..##.|........",
    0x25AC: "........|........|........|........|.######.|.######.|.######.|........",
    0x21A8: "...##...|..####..|.######.|...##...|.######.|..####..|...##...|########",
    0x2191: "...##...|..####..|.######.|...##...|...##...|...##...|...##...|........",
    0x2193: "...##...|...##...|...##...|...##...|.######.|..####..|...##...|........",
    0x2192: "........|...##...|....##..|#######.|....##..|...##...|........|........",
    0x2190: "........|..##....|.##.....|#######.|.##.....|..##....|........|........",
    0x221F: "........|........|##......|##......|##......|#######.|........|........",
    0x2194: "........|..#..#..|.##..##.|########|.##..##.|..#..#..|........|........",
    0x25B2: "........|...##...|..####..|.######.|########|########|........|........",
    0x25BC: "........|########|########|.######.|..####..|...##...|........|........",
    0x2302: "........|...#....|..###...|.##.##..|##...##.|##...##.|#######.|........",
    0x221E: "........|........|.###.##.|##.###..|##..#...|##.###..|.###.##.|........",
    0x2229: "..###...|.##.##..|##...##.|##...##.|##...##.|##...##.|........|........",
    0x2261: "........|#######.|........|#######.|........|#######.|........|........",
    0x2265: "##......|.##.....|..##....|...##...|..##....|.##.....|##......|#######.",
    0x2264: ".....##.|....##..|...##...|..##....|...##...|....##..|.....##.|#######.",
    0x2320: "....###.|...##.##|...##...|...##...|...##...|...##...|...##...|...##...",
    0x2321: "...##...|...##...|...##...|...##...|...##...|...##...|##.##...|.###....",
    0x2248: "........|.###.##.|##.###..|........|.###.##.|##.###..|........|........",
    0x2219: "........|........|........|...##...|...##...|........|........|........",
    0x221A: "....####|....##..|....##..|....##..|###.##..|.##.##..|..####..|...###..",
    0x207F: ".#####..|.##..##.|.##..##.|.##..##.|........|........|........|........",
    0x25A0: "........|........|..####..|..####..|..####..|..####..|........|........",
}


def drawn_rows(art):
    """ASCII art (leftmost column first) -> 8 bytes with bit 0 = leftmost."""
    rows = art.split("|")
    assert len(rows) == 8, art
    out = []
    for r in rows:
        assert len(r) == 8, r
        out.append(sum(1 << i for i, c in enumerate(r) if c == "#"))
    return out


def fetch(name, cache):
    path = os.path.join(cache, name)
    if not os.path.exists(path):
        os.makedirs(cache, exist_ok=True)
        with urllib.request.urlopen(f"{BASE}/{name}", timeout=30) as r:
            open(path, "wb").write(r.read())
    return open(path).read()


def parse_block(text, first):
    """dhepper's blocks are contiguous arrays starting at `first`."""
    out, n = {}, 0
    for body in re.findall(r"\{([^}]*)\}", text):
        vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
        if len(vals) != 8:
            continue
        out[first + n] = vals
        n += 1
    return out


def build(cache):
    glyphs = {}
    for name, first in BLOCKS:
        glyphs.update(parse_block(fetch(name, cache), first))
    misc = fetch("font8x8_misc.h", cache)
    cps = [int(m, 16) for m in re.findall(r"U\+([0-9A-F]{4})", misc)]
    rows = [[int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", b)]
            for b in re.findall(r"\{([^}]*)\}", misc)]
    for cp, row in zip(cps, rows):
        if len(row) == 8:
            glyphs[cp] = row
    for cp, art in DRAWN.items():
        glyphs[cp] = drawn_rows(art)

    table, names = [], []
    for byte in range(256):
        ch = CP437_LOW[byte] if byte < 0x20 else \
            ("⌂" if byte == 0x7F else bytes([byte]).decode("cp437"))
        cp = ord(ch)
        if cp not in glyphs:
            raise SystemExit(f"no glyph for 0x{byte:02X} U+{cp:04X}")
        table.append(glyphs[cp])
        names.append(f"U+{cp:04X}")
    return table, names


HEADER = '''/*
 * 8x8 monochrome bitmap font, CP437 -- all 256 codes, not just ASCII.
 *
 * GENERATED by scripts/make_font.py. Do not edit by hand.
 *
 * Glyph data from font8x8 by Daniel Hepper <daniel@hepper.net>, released into
 * the Public Domain (https://github.com/dhepper/font8x8), reindexed from
 * Unicode to CP437. The 42 codes no font8x8 block covers -- the card suits and
 * arrows below 0x20, and the maths operators at 0xEC-0xFE -- are drawn in the
 * generator and are likewise unencumbered.
 *
 * Indexed by the raw character byte of a text cell. DOS text is CP437: masking
 * to 7 bits turns every box-drawing rule and shading block in a full-screen UI
 * into an unrelated letter, which is exactly how this shipped broken.
 *
 * The leftmost pixel of a row is bit 0.
 */
#ifndef X86_FONT8X8_H
#define X86_FONT8X8_H

static const unsigned char font8x8_basic[256][8] = {
'''


# ---------------------------------------------------------------------------
# Bm437_EverexME_5x8: a Windows 2.x .FON, i.e. an NE executable whose only
# RT_FONT resource is a version-2 FNT. Parsed here rather than shelled out to a
# converter so the pack stays the single source of truth.

def ne_font_resource(data):
    """Byte range of the one RT_FONT resource in an NE .FON."""
    ne = struct.unpack_from("<I", data, 0x3C)[0]
    if data[ne:ne + 2] != b"NE":
        raise SystemExit("not an NE executable; is the font pack intact?")
    rt = ne + struct.unpack_from("<H", data, ne + 0x24)[0]
    shift = struct.unpack_from("<H", data, rt)[0]
    p = rt + 2
    while True:
        tid = struct.unpack_from("<H", data, p)[0]
        if tid == 0:
            raise SystemExit("no RT_FONT resource in the .FON")
        count = struct.unpack_from("<H", data, p + 2)[0]
        p += 8
        for _ in range(count):
            off, ln = struct.unpack_from("<HH", data, p)
            if tid == 0x8008:                       # RT_FONT | 0x8000
                return off << shift, ln << shift
            p += 12


def everex5x8(path=FONT_PACK, member=FONT_PACK_MEMBER):
    """The 256 CP437 glyphs of Everex Micro Enhancer, as 8 rows of 5 bits.

    Bit 4 is the LEFTMOST pixel here -- the FNT packs a glyph row into the high
    bits of a byte -- and it is converted to this tree's bit-0-is-leftmost
    convention by `squeeze4`, not here, so the raw shape stays recognisable
    against a hex dump of the resource.
    """
    with zipfile.ZipFile(path) as z:
        data = z.read(member)
    off, _ = ne_font_resource(data)
    ver = struct.unpack_from("<H", data, off)[0]
    if ver != 0x0200:
        raise SystemExit(f"unexpected FNT version 0x{ver:04X}")
    width = struct.unpack_from("<H", data, off + 0x56)[0]
    height = struct.unpack_from("<H", data, off + 0x58)[0]
    first, last = data[off + 0x5F], data[off + 0x60]
    if (width, height, first, last) != (5, 8, 0x00, 0xFF):
        raise SystemExit(f"expected a 5x8 CP437 face, got {width}x{height} "
                         f"0x{first:02X}-0x{last:02X}")
    table = []
    for byte in range(256):
        w, o = struct.unpack_from("<HH", data, off + 0x76 + 4 * (byte - first))
        assert w == 5, (byte, w)
        table.append([(data[off + o + y] >> 3) & 0x1F for y in range(height)])
    return table


def squeeze4(rows):
    """5 wide -> 4 wide, and MSB-left -> bit-0-left.

    Column 4 is the inter-character gap for all but 77 of the 256 glyphs, so
    dropping it is exact; where it does carry ink, columns 3 and 4 are OR-ed.
    That touches a glyph's right edge only and never moves a stem, which is the
    whole reason a 5 px face is a better source than an 8 px one.
    """
    out = []
    for v in rows:
        cols = [(v >> (4 - i)) & 1 for i in range(5)]   # cols[0] = leftmost
        cols = cols[:3] + [cols[3] | cols[4]]
        out.append(sum(1 << i for i, c in enumerate(cols) if c))
    return out


# ---------------------------------------------------------------------------
# Hand-drawn line art, 0xB0-0xDF.

"""The dithered shades. OR-ing or box-filtering a dither doubles its density
and collapses 25%/50%/75% together, which flattens the background of every DOS
UI that uses them -- TOPBENCH fills its whole screen with 0xB0."""
SHADES = {
    0xB0: ["#.#.", "....", ".#.#", "....",
           "#.#.", "....", ".#.#", "...."],          # light shade, 25%
    0xB1: ["#.#.", ".#.#", "#.#.", ".#.#",
           "#.#.", ".#.#", "#.#.", ".#.#"],          # medium shade, 50%
    0xB2: ["####", "#.#.", "####", ".#.#",
           "####", "#.#.", "####", ".#.#"],          # dark shade, 75%
}

"""Solid blocks. Exact at 4x8, so there is nothing to decide."""
SOLIDS = {
    0xDB: ("####", 0, 8), 0xDC: ("####", 4, 8), 0xDF: ("####", 0, 4),
    0xDD: ("##..", 0, 8), 0xDE: ("..##", 0, 8),
}

# The line-drawing spec. Every glyph is (vertical extents, horizontal style,
# horizontal extents) over a 4x8 cell:
#
#   the vertical rule is ALWAYS column 1, the single horizontal rule ALWAYS
#   row 4, the double horizontal rules ALWAYS rows 3 and 5.
#
# Holding those fixed is the whole point: adjacent cells then join without a
# jog, and a rule that runs to column 3 meets the next cell's column 0 with no
# gap, so a frame is continuous instead of dashed.
#
#   V   list of (row_start, row_end) the vertical rule spans, exclusive end
#   H   "" none, "s" single (row 4), "d" double (rows 3 and 5)
#   HX  (col_start, col_end) the horizontal rule spans, exclusive end
#
# A stub stops at column 2 (exclusive) so it covers the vertical at column 1
# and reaches the LEFT edge, or starts at column 1 so it reaches the RIGHT
# edge. `(0, 4)` is a through rule.
#
# Double VERTICALS alias to single -- see the module docstring. That makes
# several pairs identical by construction (╨ == ┴, ╥ == ┬, ╟ == ├ ...), which
# is intended and is why they are written out rather than aliased by name: the
# table stays readable as a picture of what each code draws.
FULL, LEFT, RIGHT = (0, 4), (0, 2), (1, 4)
DOWN, UPS, UPD, THRU = [(4, 8)], [(0, 5)], [(0, 6)], [(0, 8)]
LINEART = {
    0xB3: (THRU,     "",  FULL),   # |  single vertical
    0xB4: (THRU,     "s", LEFT),   # -|
    0xB5: (THRU,     "d", LEFT),   # =|
    0xB6: (THRU,     "s", LEFT),   # -||   double vert -> single
    0xB7: ([(4, 8)], "s", LEFT),   # -,|
    0xB8: ([(3, 8)], "d", LEFT),   # =,
    0xB9: (THRU,     "d", LEFT),   # =||
    0xBA: (THRU,     "",  FULL),   # ||  double vertical -> single
    0xBB: ([(3, 8)], "d", LEFT),   # =\
    0xBC: (UPD,      "d", LEFT),   # =/
    0xBD: (UPS,      "s", LEFT),   # -/|
    0xBE: (UPD,      "d", LEFT),   # =/
    0xBF: ([(4, 8)], "s", LEFT),   # -\
    0xC0: (UPS,      "s", RIGHT),  # |_-
    0xC1: (UPS,      "s", FULL),   # -+-
    0xC2: (DOWN,     "s", FULL),   # -T-
    0xC3: (THRU,     "s", RIGHT),  # |-
    0xC4: ([],       "s", FULL),   # -  single horizontal
    0xC5: (THRU,     "s", FULL),   # -+-
    0xC6: (THRU,     "d", RIGHT),  # |=
    0xC7: (THRU,     "s", RIGHT),  # ||-
    0xC8: (UPD,      "d", RIGHT),  # ||=
    0xC9: ([(3, 8)], "d", RIGHT),  # /=
    0xCA: (UPD,      "d", FULL),   # =+=
    0xCB: ([(5, 8)], "d", FULL),   # =T=
    0xCC: (THRU,     "d", RIGHT),  # ||=
    0xCD: ([],       "d", FULL),   # =  double horizontal
    0xCE: ([(0, 4), (5, 8)], "d", FULL),   # =+=
    0xCF: (UPD,      "d", FULL),
    0xD0: (UPS,      "s", FULL),
    0xD1: ([(5, 8)], "d", FULL),
    0xD2: (DOWN,     "s", FULL),
    0xD3: (UPS,      "s", RIGHT),
    0xD4: (UPD,      "d", RIGHT),
    0xD5: ([(3, 8)], "d", RIGHT),
    0xD6: (DOWN,     "s", RIGHT),
    0xD7: (THRU,     "s", FULL),
    0xD8: ([(0, 4), (5, 8)], "d", FULL),
    0xD9: (UPS,      "s", LEFT),   # -/
    0xDA: ([(4, 8)], "s", RIGHT),  # ,-
}


def art_rows(art):
    """A list of 4-character rows -> 8 bytes, bit 0 = leftmost pixel."""
    assert len(art) == 8, art
    return [sum(1 << i for i, c in enumerate(r) if c == "#") for r in art]


def lineart4():
    """0xB0-0xDF as 4x8 bitmaps, bit 0 = leftmost pixel."""
    out = {}
    for byte, art in SHADES.items():
        out[byte] = art_rows(art)
    for byte, (pat, y0, y1) in SOLIDS.items():
        out[byte] = art_rows([pat if y0 <= y < y1 else "...."
                              for y in range(8)])
    for byte, (vext, hstyle, (x0, x1)) in LINEART.items():
        rows = [0] * 8
        for a, b in vext:
            for y in range(a, b):
                rows[y] |= 1 << 1                    # the vertical rule
        hbits = sum(1 << x for x in range(x0, x1))
        for y in ((4,) if hstyle == "s" else (3, 5) if hstyle == "d" else ()):
            rows[y] |= hbits
        out[byte] = rows
    missing = [b for b in range(0xB0, 0xE0) if b not in out]
    assert not missing, [hex(b) for b in missing]
    return out


def narrow(everex, art):
    """The shipping 4x8 bilevel table: Everex letters, hand-drawn line art."""
    return [art[b] if b in art else squeeze4(everex[b]) for b in range(256)]


# ---------------------------------------------------------------------------
# The anti-aliased table.

def antialias(table8, art):
    """8x8 -> 4x8 at 2 bits per pixel, one byte per row.

    Output pixel i box-filters source columns 2i and 2i+1: two lit -> 3, one
    lit -> 2 or 1, none -> 0. The two one-pixel cases are NOT the same level:
    the LEFT source column weighs 2 and the right 1, so a stem sitting on an
    odd source column leans the right way instead of averaging into a blur.

    The line-drawing range is promoted from the hand-drawn bilevel glyphs at
    full level rather than filtered, so frames stay crisp.
    """
    out = []
    for byte in range(256):
        rows = []
        if byte in art:
            for v in art[byte]:
                rows.append(sum(3 << (2 * i) for i in range(4)
                                if (v >> i) & 1))
        else:
            for v in table8[byte]:
                lv = []
                for i in range(4):
                    left = (v >> (2 * i)) & 1        # bit 0 is leftmost
                    right = (v >> (2 * i + 1)) & 1
                    lv.append(3 if left and right else
                              2 if left else 1 if right else 0)
                rows.append(sum(l << (2 * i) for i, l in enumerate(lv)))
        out.append(rows)
    return out


HEADER4 = '''/*
 * 4x8 monochrome bitmap font, CP437 -- all 256 codes.
 *
 * GENERATED by scripts/make_font.py. Do not edit by hand.
 *
 * Why 4 px wide: mode 3 is 80x25, and 80 cells of 8 px is 640 -- twice the
 * panel. An 8x8 cell can therefore only ever show 40 of the 80 columns, and a
 * DOS UI centred on the 80 comes out chopped down the middle. At 4 px the
 * whole screen is 320x200, so nothing is cut off and no panning is needed.
 *
 * Each byte holds four pixels in bits 0-3, leftmost pixel in bit 0.
 *
 * Letters, digits and punctuation are Bm437_EverexME_5x8 from VileR's Int10h
 * Oldschool PC Font Pack squeezed 5 -> 4 -- a face real hardware used to show
 * 80 columns in a five-pixel cell, so it was DESIGNED narrow instead of being
 * crushed from eight. The line-drawing range 0xB0-0xDF is drawn by hand in the
 * generator, on a fixed grid (vertical rule at column 1, single horizontal at
 * row 4, double horizontals at rows 3 and 5) so frames join across cells;
 * double VERTICAL rules deliberately alias to single, which four pixels cannot
 * render distinctly.
 */
#ifndef X86_FONT4X8_H
#define X86_FONT4X8_H

static const unsigned char font4x8_cp437[256][8] = {
'''

HEADER4AA = '''/*
 * 4x8 ANTI-ALIASED bitmap font, CP437 -- all 256 codes, 2 bits per pixel.
 *
 * GENERATED by scripts/make_font.py from font8x8.h. Do not edit by hand.
 *
 * One byte per glyph row holds four pixels of two bits each, leftmost pixel in
 * bits 0-1: 0 is background, 3 is full foreground, 1 and 2 are intermediate.
 * At four pixels a 1-bit glyph has only four binary decisions per row, which is
 * not enough to place a stem -- a stem falling between two output pixels either
 * vanishes or doubles in weight. Box-filtering the authentic 8x8 glyph 2:1 and
 * KEEPING the intermediate level turns that into two half-lit pixels instead.
 *
 * The renderer pairs this with a CLUT block of four contiguous entries per
 * distinct (foreground, background) attribute pair on screen, so the palette
 * does the colour mixing and the inner loop stays one load, one add, one store.
 *
 * The line-drawing range 0xB0-0xDF is NOT filtered: it is the hand-drawn
 * bilevel art of font4x8.h promoted to level 3, because box filtering a
 * one-pixel rule would draw every frame on the screen at half brightness.
 *
 * Same provenance and licence as font8x8.h (Daniel Hepper, Public Domain).
 */
#ifndef X86_FONT4X8AA_H
#define X86_FONT4X8AA_H

static const unsigned char font4x8aa_cp437[256][8] = {
'''


def render(table, names, header=HEADER):
    lines = [header]
    for i, (rows, nm) in enumerate(zip(table, names)):
        body = ", ".join(f"0x{v:02X}" for v in rows)
        lines.append(f"    {{{body}}},   /* 0x{i:02X} {nm} */\n")
    lines.append("};\n\n#endif\n")
    return "".join(lines)


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cache", default="build/fontwork",
                   help="where the upstream font8x8_*.h blocks are cached")
    p.add_argument("--out", default=os.path.normpath(OUT))
    p.add_argument("--out4", default=os.path.normpath(OUT4))
    p.add_argument("--out4aa", default=os.path.normpath(OUT4AA))
    p.add_argument("--font-pack", default=os.path.normpath(FONT_PACK),
                   help="VileR's Oldschool PC Font Pack, source of the "
                        "Everex Micro Enhancer 5x8 face the narrow glyphs use")
    p.add_argument("--check", action="store_true",
                   help="exit non-zero if any file on disk is not what this "
                        "generator would produce")
    p.add_argument("--show", metavar="0xNN",
                   help="print one glyph at 8x8, 4x8 and 4x8 AA and exit -- "
                        "the only practical way to judge line art by eye")
    a = p.parse_args()

    table, names = build(a.cache)
    art = lineart4()
    narrow4 = narrow(everex5x8(a.font_pack), art)
    aa4 = antialias(table, art)
    if a.show:
        b = int(a.show, 0)
        print(f"0x{b:02X} {names[b]}")
        for y in range(8):
            wide = "".join("#" if table[b][y] >> i & 1 else "." for i in range(8))
            thin = "".join("#" if narrow4[b][y] >> i & 1 else "." for i in range(4))
            lvl = "".join(" .+#"[aa4[b][y] >> (2 * i) & 3] for i in range(4))
            print(f"  {wide}   {thin}   {lvl}")
        return 0
    want = [(a.out, render(table, names), "8x8"),
            (a.out4, render(narrow4, names, HEADER4), "4x8"),
            (a.out4aa, render(aa4, names, HEADER4AA), "4x8 AA")]
    if a.check:
        rc = 0
        for path, text, what in want:
            cur = open(path).read() if os.path.exists(path) else ""
            if cur != text:
                print(f"{path} is out of date; run scripts/make_font.py")
                rc = 1
            else:
                print(f"{path} up to date (256 {what} glyphs)")
        return rc
    for path, text, what in want:
        open(path, "w").write(text)
        print(f"wrote {path}: 256 CP437 {what} glyphs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
