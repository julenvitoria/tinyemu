#!/usr/bin/env python3
"""
Tier-1 check of the CGA text-mode (mode 3) renderer.

The text renderer had no coverage at all, which is how it shipped drawing a
full-screen DOS UI as gibberish. This is the mode-3 counterpart of
`test_video.py`: the host seeds an 80x25 (char, attribute) pattern straight
into the text buffer -- the path a real DOS program takes, poking B800:0 rather
than calling INT 10h -- renders one frame on the target, and compares the
framebuffer against an image computed here in Python.

What it pins down:

  * cell layout -- 2 bytes per cell, character low, attribute high. A stride
    error or a char/attribute swap shows up as a mismatch instead of as
    plausible-looking noise next to correct text.
  * the full CP437 range. The pattern uses all 256 character codes; a font
    indexed with only 7 bits (the original defect) fails here.
  * attributes -- all 16 foregrounds, all 8 backgrounds, and the blink bit,
    which must be masked out of the background rather than selecting colours
    8-15.
  * the full-width window: all 80 columns must reach the panel. At an 8 px cell
    only 40 did, which sliced a centred DOS UI in half; the cell is 4 px now
    and 80 x 4 is exactly the 320 px panel.
  * INT 10h AH=06 scrolling, checked on the buffer itself.
  * the text-mode CLUT, which must hold all 16 CGA colours (mode 4 needs only
    four, and the text branch is a separate path that can rot unnoticed) --
    and must publish 32 entries, because retro-go's overlay reaches a darkened
    twin by OR-ing a FIXED +32 into the pixel byte and lcd_set_clut() only puts
    the twins there when count is exactly 32.
  * BOTH narrow font paths, selected through `x86_text_aa`: the 1-bit Everex
    table, and the anti-aliased one whose per-frame allocation of four-entry
    CLUT blocks to the (fg,bg) pairs actually on screen is the whole reason
    anti-aliasing fits at all. The seeded pattern uses far more than the eight
    pairs the budget buys, so it exercises the degradation path too -- which is
    where a renderer would otherwise silently draw in the wrong colours.

The font is read out of `font4x8.h` for the pixel comparison -- the point of
that comparison is layout, not glyph art -- but a handful of well-known CP437
shapes are asserted against hard-coded bitmaps in BOTH tables, so a font that
is the wrong size, wrong encoding or wrong bit order cannot pass, and neither
can a 4x8 table that has drifted from the 8x8 one it is condensed from.
"""
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols
from scripts.common import textmode

TEXT_COLS, TEXT_ROWS = textmode.TEXT_COLS, textmode.TEXT_ROWS
VIS_COLS = textmode.VIS_COLS
CGA_ARGB = textmode.CGA_ARGB
FB_STRIDE = 320

# Well-known CP437 cells, drawn leftmost column first. These are asserted
# against the header independently of anything the target does: they are the
# characters a DOS UI is actually made of, and they are exactly the ones a
# 128-glyph ASCII font gets wrong.
KNOWN = {
    0xC4: ["........", "........", "........", "........",
           "########", "........", "........", "........"],   # horizontal rule
    0xB3: ["...#....", "...#....", "...#....", "...#....",
           "...#....", "...#....", "...#....", "...#...."],   # vertical rule
    0xDB: ["########", "########", "########", "########",
           "########", "########", "########", "########"],   # full block
    0xB0: ["#.#.#.#.", "........", ".#.#.#.#", "........",
           "#.#.#.#.", "........", ".#.#.#.#", "........"],   # light shade
    0xDA: ["........", "........", "........", "........",
           "...#####", "...#....", "...#....", "...#...."],   # top-left corner
}

# The same characters in the 4-wide table the renderer draws from. These are
# hand-drawn in the generator on a fixed grid -- vertical rule at column 1,
# single horizontal at row 4 -- and that grid is the whole point: a rule that
# stopped short of a cell edge draws every frame on the screen dashed, and a
# rule that moved column between glyphs leaves every corner disconnected.
KNOWN4 = {
    0xC4: ["....", "....", "....", "....", "####", "....", "....", "...."],
    0xB3: [".#..", ".#..", ".#..", ".#..", ".#..", ".#..", ".#..", ".#.."],
    0xDB: ["####", "####", "####", "####", "####", "####", "####", "####"],
    0xB0: ["#.#.", "....", ".#.#", "....", "#.#.", "....", ".#.#", "...."],
    0xDA: ["....", "....", "....", "....", ".###", ".#..", ".#..", ".#.."],
}


def check_font(font, known=KNOWN4, name="font4x8.h", width=4):
    if len(font) != 256:
        print(f"FAIL: {name} has {len(font)} glyphs, need 256 (CP437). "
              f"A 128-entry font forces the renderer to mask the character "
              f"byte, so every box-drawing and shading code draws a letter.")
        return False
    ok = True
    for code, art in known.items():
        assert all(len(r) == width for r in art), code
        want = [sum(1 << i for i, c in enumerate(r) if c == "#") for r in art]
        if font[code] != want:
            print(f"FAIL: {name} glyph 0x{code:02X} is "
                  f"{[hex(v) for v in font[code]]}, expected "
                  f"{[hex(v) for v in want]}")
            ok = False
    return ok


def check_lineart(font4):
    """Frames must be continuous. A horizontal rule has to reach BOTH cell
    edges and a vertical rule has to occupy the same column in every glyph that
    contains one, or a box drawn out of these codes comes out dashed and its
    corners miss. Checked over the whole line-drawing range rather than the
    five samples above, because it is a property of the set, not of a glyph."""
    ok = True
    horiz = {0xC4, 0xC5, 0xC1, 0xC2, 0xCD, 0xCE, 0xCA, 0xCB, 0xD7, 0xD8}
    for code in horiz:
        rows = font4[code]
        if not any(r == 0x0F for r in rows):
            print(f"FAIL: 0x{code:02X} has no rule spanning all 4 columns, so "
                  f"a horizontal run drawn with it comes out dashed")
            ok = False
    vert = {0xB3, 0xBA, 0xC3, 0xB4, 0xC5, 0xCE, 0xCC, 0xB9, 0xD7, 0xD8}
    for code in vert:
        rows = font4[code]
        if sum(1 for r in rows if r & 0x02) < 6:
            print(f"FAIL: 0x{code:02X} does not carry a vertical rule at "
                  f"column 1 through the cell")
            ok = False
    return ok


def make_cells():
    """Every character code and a wide spread of attributes, arranged so a row
    or column stride error cannot alias into a pass."""
    cells = bytearray(TEXT_COLS * TEXT_ROWS * 2)
    for r in range(TEXT_ROWS):
        for c in range(TEXT_COLS):
            ch = (r * TEXT_COLS + c) & 0xFF
            attr = (r * 29 + c * 11) & 0xFF      # includes the blink bit
            o = (r * TEXT_COLS + c) * 2
            cells[o] = ch
            cells[o + 1] = attr
    return bytes(cells)


def wait_flag(b, sym, want, what, tries=200):
    for _ in range(tries):
        if b.read_uint32(sym) == want:
            return True
        time.sleep(0.05)
    print(f"FAIL: {what}")
    return False


def compare(got, want, label):
    if got == want:
        return True
    for i, (gv, wv) in enumerate(zip(got, want)):
        if gv != wv:
            row, col = divmod(i, FB_STRIDE)
            print(f"FAIL: {label}: first mismatch at ({col},{row}) "
                  f"got {gv} expected {wv}")
            break
    print(f"  {sum(1 for a, bb in zip(got, want) if a != bb)} of {len(want)} "
          f"bytes differ")
    return False


def main(args):
    font = textmode.load_font()
    font8 = textmode.load_font("font8x8.h")
    font_aa = textmode.load_font("font4x8aa.h")
    if not check_font(font) or not check_font(font8, KNOWN, "font8x8.h", 8):
        return False
    if len(font_aa) != 256:
        print(f"FAIL: font4x8aa.h has {len(font_aa)} glyphs, need 256")
        return False
    if not check_lineart(font):
        return False
    print(f"font: {len(font)} CP437 glyphs at 4x8 (renderer), 4x8 AA and 8x8 "
          f"(BIOS); known shapes exact, line art continuous")

    runner = build_and_create_runner(args)
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols(getattr(args, "elf", None) or "test_core.elf")
        for n in ("run_video_flag", "cga_test_vram", "cga_test_out",
                  "cga_test_palette", "text_mode_arg", "text_scroll_arg",
                  "x86_video_pan", "x86_text_aa"):
            if n not in syms:
                print(f"missing symbol {n}")
                return False
        time.sleep(0.5)

        b.write_uint32(syms["run_video_flag"], 1)
        if not wait_flag(b, syms["run_video_flag"], 2, "no video init"):
            return False

        # Mode 3. This clears the buffer and reloads the CLUT, so it must
        # happen before the pattern is written.
        b.write_uint32(syms["text_mode_arg"], 3)
        b.write_uint32(syms["run_video_flag"], 4)
        if not wait_flag(b, syms["run_video_flag"], 0, "mode 3 not selected"):
            return False

        # 32 entries, in ONE layout that every mode shares: 0-15 the fixed CGA
        # palette that text attributes index directly, 16-19 the four currently
        # selected graphics colours, 20-31 a mirror of 4-15 so no index lands on
        # an uninitialised entry.
        #
        # 32 and not 16 is not decoration: the firmware puts a darkened twin of
        # entry i at `count + i`, while the overlay finds it by OR-ing a fixed
        # +32, so anything short of 32 leaves retro-go's menu dimming the screen
        # to solid black. And the split is what makes a mode SWITCH cheap --
        # entries 0-15 never move, so text colours cannot be left holding a
        # graphics palette (or the reverse), which is the stale-CLUT half of the
        # "a bunch of pixels" report.
        clut = [b.read_uint32(syms["cga_test_palette"] + 4 * i)
                for i in range(textmode.TEXT_CLUT_ENTRIES)]
        want_clut = CGA_ARGB * (textmode.TEXT_CLUT_ENTRIES // 16)
        # Power-on 0x3D9 is 0x30: palette 1, high intensity -> cyan/magenta/grey
        # over background 0.
        want_clut[16:20] = [CGA_ARGB[0], CGA_ARGB[11], CGA_ARGB[13], CGA_ARGB[15]]
        if clut != want_clut:
            print("FAIL: text-mode CLUT is not 16 CGA + 4 graphics + mirror")
            for i, (g, w) in enumerate(zip(clut, want_clut)):
                if g != w:
                    print(f"  entry {i}: {g:08x} expected {w:08x}")
            return False
        print(f"CLUT: {len(clut)} entries, 0-15 CGA + 16-19 graphics, exact")

        cells = make_cells()
        ok = True
        # Both a pinned window and an out-of-range one: with 80 visible columns
        # every value must clamp to 0 and render the whole screen. A renderer
        # that still windowed would fail the second.
        for aa in (0, 1):
            b.write_uint32(syms["x86_text_aa"], aa)
            for left in (0, 40):
                b.write_memory(syms["cga_test_vram"], cells)
                b.write_uint32(syms["x86_video_pan"], left)
                b.write_uint32(syms["run_video_flag"], 3)
                if not wait_flag(b, syms["run_video_flag"], 0, "blit stalled"):
                    return False
                got = bytes(b.read_memory(syms["cga_test_out"],
                                          FB_STRIDE * 240))
                want = textmode.render(cells, font, left,
                                       font_aa=font_aa if aa else None)
                label = "AA" if aa else "bilevel"
                if compare(got, want, f"{label} pan={left}"):
                    print(f"PASS: {label} pan={left}: "
                          f"{VIS_COLS * TEXT_ROWS} cells pixel-exact")
                else:
                    ok = False
            if aa:
                # The per-frame allocation IS the feature; check the palette it
                # published, not just that the indices are self-consistent.
                _b, _f, want_clut, _lb, keys = textmode.alloc_palette(
                    cells, font_aa)
                got_clut = [b.read_uint32(syms["cga_test_palette"] + 4 * i)
                            for i in range(textmode.TEXT_CLUT_ENTRIES)]
                if got_clut == want_clut:
                    print(f"PASS: AA palette: {len(keys)} (fg,bg) pairs got a "
                          f"4-entry block, all 32 entries exact")
                else:
                    print("FAIL: AA palette mismatch")
                    for i, (g, w) in enumerate(zip(got_clut, want_clut)):
                        if g != w:
                            print(f"  entry {i}: {g:08x} expected {w:08x}")
                    ok = False
        b.write_uint32(syms["x86_text_aa"], 1)

        # Scrolling, checked on the buffer rather than the picture: it is a
        # memmove plus a fill and the picture would only obscure which.
        for lines, attr in ((1, 0x07), (3, 0x1E), (TEXT_ROWS, 0x4F)):
            b.write_memory(syms["cga_test_vram"], cells)
            b.write_uint32(syms["text_scroll_arg"], (attr << 8) | lines)
            b.write_uint32(syms["run_video_flag"], 5)
            if not wait_flag(b, syms["run_video_flag"], 0, "scroll stalled"):
                return False
            got = bytes(b.read_memory(syms["cga_test_vram"],
                                      TEXT_COLS * TEXT_ROWS * 2))
            want = textmode.scrolled(cells, lines, attr)
            if got == want:
                print(f"PASS: scroll {lines} line(s), attr 0x{attr:02X}")
            else:
                bad = next(i for i in range(len(want)) if got[i] != want[i])
                r, rem = divmod(bad // 2, TEXT_COLS)
                print(f"FAIL: scroll {lines}: first mismatch at row {r} "
                      f"col {rem} ({'char' if bad % 2 == 0 else 'attr'}) "
                      f"got 0x{got[bad]:02X} expected 0x{want[bad]:02X}")
                ok = False

        b.write_uint32(syms["x86_video_pan"], 0xFFFFFFFF)
        return ok
    finally:
        runner.stop()


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    args = p.parse_args()
    sys.exit(0 if main(args) else 1)
