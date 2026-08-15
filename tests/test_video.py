#!/usr/bin/env python3
"""
Tier-1 check of the CGA mode 4 blitter.

The expected image is computed here, independently of the target: the host
writes a pattern into VRAM, triggers the blit, reads the framebuffer back and
compares it against an expansion done in Python. Nothing on the device
contributes to the expectation, so a wrong table or a wrong interlace shows up
as a mismatch rather than as agreement between two copies of the same bug.
"""
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

CGA_ROW_BYTES = 80
CGA_WIDTH = 320
CGA_HEIGHT = 200
PLANE_ODD = 0x2000
FB_STRIDE = 320
FB_TOP = ((240 - CGA_HEIGHT) // 2) * FB_STRIDE
# Graphics pixels index their own four-entry CLUT block; 0-15 stay the fixed
# CGA palette in every mode. See x86_video_refresh_palette().
GFX_CLUT_BASE = 16
TEXT_ROWS = 25
CELL_H = 8


def make_pattern():
    """A pattern where every byte differs from its neighbours and all four
    2-bit codes appear, so a shift/order error cannot alias into a pass."""
    vram = bytearray(32768)
    for row in range(CGA_HEIGHT):
        plane = PLANE_ODD if (row & 1) else 0
        base = plane + (row // 2) * CGA_ROW_BYTES
        for col in range(CGA_ROW_BYTES):
            vram[base + col] = (row * 31 + col * 7) & 0xFF
    return bytes(vram)


def expected_image(vram):
    """Expand 2bpp -> 8bpp exactly as the hardware defines it: leftmost pixel
    in the HIGH bits of each byte."""
    img = bytearray(FB_STRIDE * 240)
    for row in range(CGA_HEIGHT):
        plane = PLANE_ODD if (row & 1) else 0
        src = plane + (row // 2) * CGA_ROW_BYTES
        dst = FB_TOP + row * FB_STRIDE
        for col in range(CGA_ROW_BYTES):
            b = vram[src + col]
            img[dst + col * 4 + 0] = GFX_CLUT_BASE + ((b >> 6) & 3)
            img[dst + col * 4 + 1] = GFX_CLUT_BASE + ((b >> 4) & 3)
            img[dst + col * 4 + 2] = GFX_CLUT_BASE + ((b >> 2) & 3)
            img[dst + col * 4 + 3] = GFX_CLUT_BASE + ((b >> 0) & 3)
    return bytes(img)


def expected_mode6(vram):
    """640x200 at 1 bpp, decimated to the 320 px panel by OR-ing pixel PAIRS:
    a set bit in either lights the output pixel, so a one-pixel rule survives
    where dropping every other column would erase half of them."""
    img = bytearray(FB_STRIDE * 240)
    for row in range(CGA_HEIGHT):
        plane = PLANE_ODD if (row & 1) else 0
        src = plane + (row // 2) * CGA_ROW_BYTES
        dst = FB_TOP + row * FB_STRIDE
        for col in range(CGA_ROW_BYTES):
            b = vram[src + col]
            for p in range(4):
                # Source bits 7-2p and 6-2p, leftmost first.
                lit = ((b >> (7 - 2 * p)) | (b >> (6 - 2 * p))) & 1
                img[dst + col * 4 + p] = GFX_CLUT_BASE + lit
    return bytes(img)


def expected_glyph_mode4(glyph, colour, col, row):
    """What one INT 10h AH=0Eh character must look like in VRAM in mode 4.

    This is the whole reported bug in one expectation: in a graphics mode the
    BIOS rasterises the 8x8 glyph as PIXELS, and storing a char/attr pair here
    instead is what turned Alley Cat's joystick prompt into confetti. Computed
    from the font the target hands the guest, so a wrong bit order or a missed
    interleave bank cannot agree with a matching mistake in the renderer."""
    vram = bytearray(32768)
    pix = colour & 3 or 3
    for line in range(CELL_H):
        y = row * CELL_H + line
        off = (PLANE_ODD if (y & 1) else 0) + (y // 2) * CGA_ROW_BYTES + col * 2
        bits = glyph[line]
        for half in range(2):
            b = 0
            for p in range(4):
                if bits & (1 << (half * 4 + p)):   # bit 0 is the LEFTMOST pixel
                    b |= pix << (6 - 2 * p)
            vram[off + half] = b
    return bytes(vram)


def wait_flag(b, addr, want, what, tries=200):
    for _ in range(tries):
        if b.read_uint32(addr) == want:
            return True
        time.sleep(0.05)
    print(f"FAIL: {what}")
    return False


def render(b, syms):
    b.write_uint32(syms["run_video_flag"], 3)
    if not wait_flag(b, syms["run_video_flag"], 0, "blit did not complete"):
        return None
    return bytes(b.read_memory(syms["cga_test_out"], FB_STRIDE * 240))


def guest_set_mode(b, syms, mode):
    """Set the mode the way a program does: MOV AX,000m / INT 10h in GUEST
    code. Returns (mode, is_text, cols, bda_mode, bda_cols)."""
    b.write_uint32(syms["msw_mode_arg"], mode)
    b.write_uint32(syms["run_modeswitch_flag"], 2)
    if not wait_flag(b, syms["run_modeswitch_flag"], 0, f"mode {mode:#x} set hung"):
        return None
    return [b.read_uint32(syms["msw_r"] + 4 * i) for i in range(6)]


def mode_switch_checks(b, syms):
    """A guest switching modes at RUNTIME, in both directions, plus the modes
    brought over from the previous DOS core.

    The reported bug ("Alley Cat doesn't switch from CGA to text mode
    correctly, it just becomes a bunch of pixels") was measured to be none of
    mode detection, palette or blit: the mode switch was followed exactly, and
    the CHARACTERS printed afterwards were still stored as char/attr pairs into
    2 bpp pixel memory. So this walks 3 -> 4 -> 3 and checks the rendering after
    each, and then checks the character path IN a graphics mode, which is where
    the defect actually was."""
    for n in ("run_modeswitch_flag", "msw_mode_arg", "msw_char_arg", "msw_r"):
        if n not in syms:
            print(f"missing symbol {n} -- rebuild tests/main.c")
            return False

    b.write_uint32(syms["run_modeswitch_flag"], 1)
    if not wait_flag(b, syms["run_modeswitch_flag"], 0, "mode-switch init hung"):
        return False

    # The bare-metal harness links BOTH text renderers and defaults to the
    # anti-aliased one; the per-frame CLUT allocation under it is test_text.py's
    # subject, not this file's. Pin it off so a text expectation here is the
    # bilevel one.
    b.write_uint32(syms["x86_text_aa"], 0)

    import scripts.common.textmode as textmode
    font = textmode.load_font()                     # 4x8, the 80-column face
    font8 = textmode.load_font("font8x8.h")         # 8x8, 40 columns and the
    ok = True                                       # graphics-mode rasteriser

    # ---- text 3 -> graphics 4 -> text 3, each rendered and compared --------
    cells = bytearray()
    for i in range(80 * 25):
        cells += bytes([0x41 + (i % 26), 0x07 if (i % 3) else 0x1E])
    pattern = make_pattern()

    for step, (mode, kind) in enumerate(((3, "text"), (4, "cga"), (3, "text"))):
        r = guest_set_mode(b, syms, mode)
        if r is None:
            return False
        got_mode, is_text, cols, bda_mode, bda_cols = r[0], r[1], r[2], r[3], r[4]
        want_text = 1 if kind == "text" else 0
        if got_mode != mode or is_text != want_text:
            print(f"FAIL: step {step}: INT 10h AX={mode:04x} left mode "
                  f"{got_mode:#x} is_text={is_text} (want {mode:#x}/{want_text})")
            return False
        if bda_mode != mode or bda_cols not in (40, 80):
            print(f"FAIL: step {step}: BDA says mode {bda_mode:#x} cols "
                  f"{bda_cols} -- the renderer re-reads these every frame")
            return False

        if kind == "text":
            b.write_memory(syms["cga_test_vram"], bytes(cells))
            want = textmode.render(bytes(cells), font, 0)
        else:
            b.write_memory(syms["cga_test_vram"], pattern)
            want = expected_image(pattern)
        got = render(b, syms)
        if got is None:
            return False
        if got != want:
            diff = sum(1 for g, w in zip(got, want) if g != w)
            print(f"FAIL: step {step} (mode {mode:#x}, {kind}): "
                  f"{diff} of {len(want)} bytes differ")
            return False
        print(f"PASS: step {step}: guest INT 10h -> mode {mode:#x} "
              f"({cols} cols), rendered exact")

    # ---- the actual defect: a character printed in a GRAPHICS mode ---------
    guest_set_mode(b, syms, 4)
    b.write_memory(syms["cga_test_vram"], bytes(32768))
    b.write_uint32(syms["msw_char_arg"], ord("A"))
    b.write_uint32(syms["run_modeswitch_flag"], 3)
    if not wait_flag(b, syms["run_modeswitch_flag"], 0, "graphics teletype hung"):
        return False
    got_vram = bytes(b.read_memory(syms["cga_test_vram"], 32768))
    want_vram = expected_glyph_mode4(font8[ord("A")], 3, 0, 0)
    if got_vram != want_vram:
        pair = got_vram[0] | (got_vram[1] << 8)
        extra = ""
        if pair == (0x0741 if got_vram[1] == 7 else pair) or got_vram[0] == 0x41:
            extra = ("  -- VRAM holds the CHARACTER BYTE, so the glyph was "
                     "never rasterised: this is the Alley Cat defect")
        print(f"FAIL: INT 10h AH=0Eh in mode 4 did not rasterise the glyph{extra}")
        return False
    print("PASS: INT 10h AH=0Eh in mode 4 rasterises an 8x8 glyph into both "
          "interleave banks")

    # ---- mode 6: 640x200 at 1 bpp -----------------------------------------
    r = guest_set_mode(b, syms, 6)
    if r is None:
        return False
    if r[0] != 6 or r[1] != 0:
        print(f"FAIL: mode 6 not adopted (mode {r[0]:#x} is_text={r[1]})")
        return False
    b.write_memory(syms["cga_test_vram"], pattern)
    got = render(b, syms)
    want = expected_mode6(pattern)
    if got != want:
        diff = sum(1 for g, w in zip(got, want) if g != w)
        print(f"FAIL: mode 6: {diff} of {len(want)} bytes differ")
        return False
    print("PASS: mode 6 (640x200x1, OR-decimated to 320) pixel-exact")

    # ---- mode 0: 40 columns, which selects the 8x8 font --------------------
    r = guest_set_mode(b, syms, 0)
    if r is None:
        return False
    if r[0] != 0 or r[1] != 1 or r[2] != 40:
        print(f"FAIL: mode 0 not adopted as 40-column text "
              f"(mode {r[0]:#x} is_text={r[1]} cols={r[2]})")
        return False
    cells40 = bytearray()
    for i in range(40 * 25):
        cells40 += bytes([0x30 + (i % 40), 0x0F if (i % 2) else 0x47])
    b.write_memory(syms["cga_test_vram"], bytes(cells40))
    got = render(b, syms)
    want = expected_text40(bytes(cells40), font8)
    if got != want:
        diff = sum(1 for g, w in zip(got, want) if g != w)
        print(f"FAIL: 40-column text: {diff} of {len(want)} bytes differ")
        return False
    print("PASS: mode 0 (40x25 text at the 8x8 cell) pixel-exact")

    # ---- a mode we cannot render must be REFUSED, not adopted -------------
    # 0x11 (640x480x2 VGA) is the stand-in now that 13h IS rendered. It has to
    # be a mode nothing in vmodes[] claims, or this arm silently tests nothing.
    r = guest_set_mode(b, syms, 0x11)
    if r is None:
        return False
    if r[0] != 0x00:
        print(f"FAIL: unsupported mode 11h was adopted ({r[0]:#x}); the "
              f"previous mode must stay up instead")
        return False
    print("PASS: mode 11h refused, mode 0 stays up (nothing renders it)")

    if not refusal_is_side_effect_free(b, syms):
        return False

    if not vga_reg_file_checks(b, syms):
        return False
    if not vga13_checks(b, syms):
        return False
    return ok


def read_mode_log(b, syms):
    """The mode ring, oldest-to-newest, as (tag, aux, value) triples."""
    idx = b.read_uint32(syms["x86_video_mode_log_idx"])
    raw = b.read_memory(syms["x86_video_mode_log"], 16 * 4)
    words = [int.from_bytes(raw[i * 4:i * 4 + 4], "little") for i in range(16)]
    n = min(idx, 16)
    order = [(idx - n + i) & 15 for i in range(n)]
    return [((words[o] >> 24) & 0xFF, (words[o] >> 8) & 0xFFFF, words[o] & 0xFF)
            for o in order]


def refusal_is_side_effect_free(b, syms):
    """A REFUSED mode set must leave the previous screen BYTE-IDENTICAL.

    The whole point of refusing a mode we cannot render is that the previous
    one stays up -- docs/08 states it and x86_bios.c's AH=1Ah comment leans on
    it. That promise is only worth what it is tested for: a refusal that
    cleared VRAM on the way out would leave the same picture a renderer bug
    does (2000 cells of 0x0720), and nothing else in this file would notice.

    Mode 0Dh is the case that matters -- it is what Commander Keen 4 asks for
    -- and it must come back as tag 5 in the mode ring AND leave both the
    guest's VRAM and the rendered frame exactly as they were.
    """
    for n in ("x86_video_mode_log", "x86_video_mode_log_idx", "cga_test_vram"):
        if n not in syms:
            print(f"missing symbol {n} -- rebuild tests/main.c")
            return False

    r = guest_set_mode(b, syms, 3)
    if r is None or r[0] != 3:
        print("FAIL: could not get back to text mode 3 for the refusal check")
        return False

    cells = bytearray()
    for i in range(80 * 25):
        cells += bytes([0x21 + (i % 90), 0x5D if (i % 7) else 0x5F])
    b.write_memory(syms["cga_test_vram"], bytes(cells))
    before_vram = bytes(b.read_memory(syms["cga_test_vram"], 80 * 25 * 2))
    before_frame = render(b, syms)
    if before_frame is None:
        return False

    r = guest_set_mode(b, syms, 0x0D)
    if r is None:
        return False
    if r[0] != 3 or r[1] != 1 or r[3] != 3:
        print(f"FAIL: mode 0Dh was not refused cleanly (mode {r[0]:#x} "
              f"is_text={r[1]} bda_mode={r[3]:#x})")
        return False
    log = read_mode_log(b, syms)
    if not any(tag == 5 and aux == 0x0D and value == 0x03 for tag, aux, value
               in log):
        print(f"FAIL: no tag-5 refusal of 0Dh in the mode ring: {log}")
        return False

    after_vram = bytes(b.read_memory(syms["cga_test_vram"], 80 * 25 * 2))
    after_frame = render(b, syms)
    if after_frame is None:
        return False
    if after_vram != before_vram:
        diff = sum(1 for g, w in zip(after_vram, before_vram) if g != w)
        blanked = after_vram[:4] == bytes([0x20, 0x07, 0x20, 0x07])
        extra = ("  -- VRAM holds 0x0720 spaces, i.e. the refusal ran the "
                 "mode-set CLEAR" if blanked else "")
        print(f"FAIL: a REFUSED mode set changed VRAM: {diff} of "
              f"{len(before_vram)} bytes{extra}")
        return False
    if after_frame != before_frame:
        print("FAIL: a REFUSED mode set changed the rendered frame")
        return False

    # Negative control. The check above can only be trusted if it FAILS on the
    # damage it claims to detect, so do that damage deliberately -- the exact
    # 0x0720 fill x86_video_set_mode() writes -- and require the comparison to
    # see it. A test that passes on a wiped screen is not a test.
    b.write_memory(syms["cga_test_vram"], bytes([0x20, 0x07]) * (80 * 25))
    poisoned = bytes(b.read_memory(syms["cga_test_vram"], 80 * 25 * 2))
    if poisoned == before_vram:
        print("FAIL: negative control -- a fully blanked VRAM compares EQUAL "
              "to the drawn screen, so the refusal check cannot fail")
        return False
    b.write_memory(syms["cga_test_vram"], before_vram)
    print("PASS: mode 0Dh refused (tag 5), previous text screen byte-identical "
          "(negative control: a 0x0720 wipe is detected)")
    return True


def expected_vga13(aperture):
    """Mode 13h is an IDENTITY copy: the guest's 320x200x8bpp aperture already
    IS the panel's LUT8 format. Computed here anyway rather than asserted as
    'the same bytes', because the letterbox offset and the stride are exactly
    what a wrong blit gets wrong."""
    img = bytearray(FB_STRIDE * 240)
    for y in range(200):
        img[FB_TOP + y * FB_STRIDE : FB_TOP + y * FB_STRIDE + 320] = \
            aperture[y * 320 : y * 320 + 320]
    return bytes(img)


def vga_reg_file_checks(b, syms):
    """Write-then-read-back on the CRTC/Sequencer/GC/Attribute register files.

    This is how a DOS program finds its display, and it is the probe WOLF3D
    failed: with no register file every readback was 0xFF, its detection
    concluded nothing was there, and the engine walked off into its own heap
    and halted on 0xFF filler -- three subsystems away from anything that
    looked like video. Measured with `probe.py --io-log`.
    """
    if "vga_regs_out" not in syms:
        print("missing symbol vga_regs_out -- rebuild tests/main.c")
        return False
    b.write_uint32(syms["run_video_flag"], 8)
    if not wait_flag(b, syms["run_video_flag"], 0, "VGA register-file probe"):
        return False
    got = [b.read_uint32(syms["vga_regs_out"] + 4 * i) for i in range(9)]
    want = [0x66, 0xFF, 0x06, 0x40, 0x41, 0x04, 0x66, 0x66, 0xFF]
    names = ["CRTC 0x0F via 0x3D5",
             "the 0x3B5 MONO alias while Misc Output selects colour "
             "(must be silent: answering both is what made WOLF3D poll a "
             "monochrome retrace that never comes)",
             "Sequencer 0x04", "Graphics Controller 0x05",
             "Attribute 0x10 (flip-flop resynced by 0x3DA)",
             "the Sequencer index latch itself",
             "the mono alias AFTER clearing Misc Output bit 0",
             "the colour address, same register file",
             "the mono alias once Misc Output selects colour again"]
    ok = True
    for g, w, n in zip(got, want, names):
        if g != w:
            print(f"FAIL: {n} read back {g:#04x}, expected {w:#04x}"
                  + (" -- 0xff is the no-device default, i.e. the register "
                     "file is missing" if g == 0xFF else ""))
            ok = False
    if ok:
        print("PASS: CRTC/Sequencer/GC/Attribute register files read back "
              "what was written, on both the colour and mono CRTC addresses")
    return ok


def vga13_checks(b, syms):
    """VGA mode 13h: 320x200x256 linear at 0xA0000, and its 256-entry DAC.

    Two things are checked and they fail differently. The BLIT is pixel-exact
    against a host-computed image, so a stride or letterbox error shows up as a
    slant or an offset. The PALETTE is compared against a host expansion of the
    same DAC bytes -- and it is streamed through the PORTS (0x3C8 then 768
    writes to 0x3C9), because a DAC modelled as a latch loses 767 of those 768
    and the whole screen comes out one colour. That failure is invisible in a
    blit check: the pixel INDICES would still be perfect.
    """
    for n in ("vga_test_aperture", "vga_dac_arg", "vga_clut_out",
              "vga_dac_readback", "x86_vga13_ok"):
        if n not in syms:
            print(f"FAIL: harness has no {n}; mode 13h arm cannot run")
            return False
    if b.read_uint32(syms["x86_vga13_ok"]) != 1:
        print("FAIL: x86_vga13_ok is 0 -- the aperture was never handed over, "
              "so 13h would be refused rather than rendered")
        return False

    r = guest_set_mode(b, syms, 0x13)
    if r is None:
        return False
    # (mode, is_text, cols, bda_mode, bda_cols)
    if r[0] != 0x13:
        print(f"FAIL: mode 13h not adopted (renderer says {r[0]:#x})")
        return False
    if r[1] != 0:
        print("FAIL: mode 13h reported as a TEXT mode")
        return False
    if r[3] != 0x13 or r[4] != 40:
        print(f"FAIL: BDA disagrees with the renderer: 0x449={r[3]:#x} "
              f"0x44A={r[4]} (want 0x13 / 40). The BDA is re-read every frame, "
              f"so a disagreement here undoes the mode set on the next one")
        return False
    print("PASS: mode 13h adopted, BDA 0x449=0x13 0x44A=40")

    # ---- the DAC, streamed through the ports ------------------------------
    # Index readable off the colour: R=i>>2, G=i&0x3F, B=63-(i>>2). No two
    # indices share a colour, so a wrong entry names itself.
    dac = bytearray(768)
    for i in range(256):
        dac[i * 3 + 0] = i >> 2
        dac[i * 3 + 1] = i & 0x3F
        dac[i * 3 + 2] = 63 - (i >> 2)
    b.write_memory(syms["vga_dac_arg"], bytes(dac))
    b.write_uint32(syms["run_video_flag"], 6)
    if not wait_flag(b, syms["run_video_flag"], 0, "DAC stream hung"):
        return False

    # Read-back: a fade loop read-modify-writes the DAC and must get its own
    # 6-bit values back, not the 8-bit expansion.
    got_rb = [b.read_uint32(syms["vga_dac_readback"] + 4 * i) for i in range(6)]
    if got_rb != [dac[i] for i in range(6)]:
        print(f"FAIL: DAC read-back {got_rb} != written {[dac[i] for i in range(6)]}"
              " -- a read-modify-write fade loop would corrupt the palette")
        return False
    print("PASS: DAC read-back exact through 0x3C7/0x3C9 (6-bit, as written)")

    b.write_uint32(syms["run_video_flag"], 7)
    if not wait_flag(b, syms["run_video_flag"], 0, "CLUT snapshot hung"):
        return False
    raw = bytes(b.read_memory(syms["vga_clut_out"], 256 * 4))
    got_clut = [int.from_bytes(raw[i * 4:i * 4 + 4], "little") for i in range(256)]

    def expand(v):
        # 63 must map to 255, not 252: a plain << 2 leaves every white 1.2%
        # grey and every fade-to-white short of it.
        return ((v << 2) | (v >> 4)) & 0xFF

    want_clut = [(expand(dac[i * 3]) << 16) | (expand(dac[i * 3 + 1]) << 8)
                 | expand(dac[i * 3 + 2]) for i in range(256)]
    bad = [i for i in range(256) if got_clut[i] != want_clut[i]]
    if bad:
        i = bad[0]
        print(f"FAIL: CLUT entry {i} is {got_clut[i]:#08x}, want {want_clut[i]:#08x}"
              f" ({len(bad)} of 256 wrong)")
        if len(bad) > 200:
            print("  nearly all wrong -- the classic shape of a DAC modelled "
                  "as a port LATCH, which keeps only the last of 768 bytes")
        return False
    print("PASS: 256 CLUT entries exact (6->8 bit as (v<<2)|(v>>4))")

    # ---- the write CURSOR: phase, wrap, and a negative control ------------
    #
    # 0x3C9 is a STREAM against an auto-incrementing byte cursor, and the whole
    # palette's meaning rests on that cursor's PHASE. Shift it by one byte and
    # every entry after it has r,g,b ROTATED -- red becomes blue -- while the
    # bytes themselves stay perfectly valid 6-bit values and every pixel INDEX
    # stays correct. That is the one palette failure a blit check, a read-back
    # check and a byte-count check all pass through.
    #
    # Wolf3D is why the wrap arm exists: VL_SetPalette is `mov cx,768 /
    # rep outsb`, and it runs TWICE with only one 0x3C8 between them, so the
    # second palette is positioned by the cursor wrapping at 768. If the wrap
    # landed anywhere but a multiple of 3, the SECOND palette would be rotated
    # and the first would not -- which reads as a defect that comes and goes.
    def stream_with_prewrite(n):
        b.write_uint32(syms["vga_dac_prewrite"], n)
        b.write_uint32(syms["run_video_flag"], 9)
        if not wait_flag(b, syms["run_video_flag"], 0, "DAC prewrite stream hung"):
            return None
        b.write_uint32(syms["run_video_flag"], 7)
        if not wait_flag(b, syms["run_video_flag"], 0, "CLUT snapshot hung"):
            return None
        raw2 = bytes(b.read_memory(syms["vga_clut_out"], 256 * 4))
        return [int.from_bytes(raw2[i * 4:i * 4 + 4], "little") for i in range(256)]

    if "vga_dac_prewrite" not in syms:
        print("FAIL: harness has no vga_dac_prewrite; the cursor arm cannot run")
        return False

    # 768 filler bytes: the cursor must wrap to exactly 0, so the palette lands
    # byte-for-byte where a fresh 0x3C8 would have put it.
    wrapped = stream_with_prewrite(768)
    if wrapped is None:
        return False
    if wrapped != want_clut:
        i = next(i for i in range(256) if wrapped[i] != want_clut[i])
        print(f"FAIL: after a 768-byte wrap the palette is mis-phased -- entry "
              f"{i} is {wrapped[i]:#08x}, want {want_clut[i]:#08x}. Wolf3D "
              f"streams two palettes back to back; the second would render in "
              f"rotated channels (red as blue)")
        return False
    print("PASS: the 768-byte write cursor wraps to phase 0 (two back-to-back "
          "palettes stay aligned)")

    # NEGATIVE CONTROL. One filler byte is a deliberate one-byte phase error;
    # the check above MUST reject it, or it is not checking anything.
    shifted = stream_with_prewrite(1)
    if shifted is None:
        return False
    if shifted == want_clut:
        print("FAIL: a deliberate ONE-BYTE cursor shift produced an identical "
              "CLUT -- the palette check cannot detect a channel rotation and "
              "therefore proves nothing")
        return False
    # Name the rotation rather than just "differs": a one-byte shift puts
    # (b[i-1], r[i], g[i]) in entry i, so red really does arrive as green and
    # the previous entry's blue as red. Asserting the exact shape stops the
    # control passing for some unrelated reason (a hung arm, a zeroed CLUT).
    def ch(v, s):
        return (v >> s) & 0xFF
    rot = [(ch(want_clut[(i - 1) % 256], 0) << 16)
           | (ch(want_clut[i], 16) << 8) | ch(want_clut[i], 8)
           for i in range(1, 256)]
    if shifted[1:] != rot:
        print(f"FAIL: the one-byte shift did change the CLUT, but not into the "
              f"expected channel rotation -- entry 1 is {shifted[1]:#08x}, a "
              f"rotation would give {rot[0]:#08x}. The control is not "
              f"exercising the cursor")
        return False
    print("PASS: negative control -- a one-byte cursor shift rotates every "
          "entry to (b[i-1], r[i], g[i]) and the CLUT check rejects it")

    # Put the correctly phased palette back for the arms below.
    if stream_with_prewrite(768) is None:
        return False

    # ---- the blit ---------------------------------------------------------
    # Vertical bars over the top half, diagonals over the bottom. A bar is
    # constant down a column, so a stride error slants it; a diagonal is
    # constant ALONG a diagonal, so the same error changes its slope instead.
    # One wrong stride cannot satisfy both.
    ap = bytearray(320 * 200)
    for y in range(200):
        for x in range(320):
            ap[y * 320 + x] = (x & 0xFF) if y < 100 else ((x + y) & 0xFF)
    b.write_memory(syms["vga_test_aperture"], bytes(ap))
    got = render(b, syms)
    if got is None:
        return False
    want = expected_vga13(bytes(ap))
    if got != want:
        for i, (g, w) in enumerate(zip(got, want)):
            if g != w:
                row, col = divmod(i, FB_STRIDE)
                print(f"FAIL: mode 13h first mismatch at ({col},{row}) "
                      f"got {g} expected {w}")
                break
        diff = sum(1 for g, w in zip(got, want) if g != w)
        print(f"  {diff} of {len(want)} bytes differ")
        return False
    print(f"PASS: mode 13h {320 * 200} pixels exact (bars + diagonals)")

    # The letterbox must be untouched black -- 200 rows into a 240-row panel.
    if any(got[:FB_TOP]) or any(got[FB_TOP + 200 * FB_STRIDE:]):
        print("FAIL: mode 13h wrote outside rows 20..219 (the letterbox bars "
              "are the OSK's surface and are not the guest's to touch)")
        return False
    print("PASS: letterbox bars untouched")

    # Leaving 13h must hand the 32-entry table back, or every later mode draws
    # through a palette nothing maintains.
    r = guest_set_mode(b, syms, 0x03)
    if r is None or r[0] != 0x03:
        print("FAIL: could not get back out of mode 13h into text mode 3")
        return False
    b.write_uint32(syms["run_video_flag"], 7)
    if not wait_flag(b, syms["run_video_flag"], 0, "CLUT snapshot hung"):
        return False
    raw = bytes(b.read_memory(syms["vga_clut_out"], 4))
    if int.from_bytes(raw, "little") != 0xDEADBEEF:
        print("FAIL: x86_video_clut256() still returns a palette outside a "
              "256-colour mode; the firmware would keep the core-owned table "
              "and retro-go's overlay would draw through it")
        return False
    print("PASS: 256-entry CLUT withdrawn on leaving 13h")
    return True


def expected_text40(cells, font):
    """40 columns of the authentic 8x8 cell: 40 x 8 = 320, the whole panel."""
    img = bytearray(FB_STRIDE * 240)
    for row in range(TEXT_ROWS):
        for col in range(40):
            cell = cells[(row * 40 + col) * 2] | (cells[(row * 40 + col) * 2 + 1] << 8)
            g = font[cell & 0xFF]
            fg, bg = (cell >> 8) & 0x0F, (cell >> 12) & 0x07
            for gy in range(CELL_H):
                base = FB_TOP + (row * CELL_H + gy) * FB_STRIDE + col * 8
                for p in range(8):
                    img[base + p] = fg if (g[gy] >> p) & 1 else bg
    return bytes(img)


# ===========================================================================
# UNCHAINED VGA ("Mode Y") -- mode 13h with the Sequencer's chain-4 bit clear.
#
# This is what Wolfenstein 3-D runs in, and reading it as a chained mode is what
# put four horizontally-squashed copies side by side. Pixel (x,y) lives in PLANE
# x&3 at PLANE OFFSET y*80 + (x>>2), so each plane holds 80 bytes per scanline
# rather than 320.
#
# Ported from references/8086tiny/test286/{tmodey.asm,runmodey.sh,modeycheck.py}.
# Read references/8086tiny/docs/video/15-mode-y.md section 6 before changing the
# pattern -- several of its choices look arbitrary and are each the residue of a
# mutation that survived.
#
# THE FOUR BANDS. Each has a different geometric signature, chosen so that no
# single wrong stride, wrong plane order or wrong start address can satisfy more
# than one of them:
#
#   rows   0..49   x & 0xFF        VERTICAL bars. Written PLANE AT A TIME (four
#                                  Map Mask writes, each painting every fourth
#                                  column), so a wrong plane-to-column mapping
#                                  reorders the bars and a stride error slants
#                                  them.
#   rows  50..99   (x+y) & 0xFF    DIAGONALS. A stride error changes the SLOPE
#                                  rather than destroying it, so it disagrees
#                                  with band A in a DIFFERENT direction.
#   rows 100..149  y & 0xFF        HORIZONTAL bars -- the exact transpose of
#                                  band A. Written with the Map Mask at 0x0F, so
#                                  one store paints four pixels: this is the
#                                  band that proves the Map Mask is not ignored.
#   rows 150..199  (y-50) & 0xFF   A LATCH COPY of band C through write mode 1.
#                                  Latch copies are how unchained software
#                                  actually moves video memory, and they are the
#                                  one read path that must stay EXACT in Mode Y,
#                                  where a data read returns the wrong plane by
#                                  design.
#
# THE PAGE FLIPS. Between bands C and D the display start moves to 0x4100 and
# then 0x8200 and is LEFT there, and band D is addressed THROUGH PAGE 2. Without
# that last part the write-side fold is never exercised -- the reference's
# mutation that deletes it SURVIVED until band D was moved up there.
#
# THE STRIDE IS 16,640 AND THE NUMBER IS LOAD-BEARING. At 16,384 an AND mask is
# the correct fold BY ACCIDENT, so the page-fold control cannot fail and the
# shipped bug is invisible to the test written to catch it. 80 x 208 is what
# real Mode Y software uses.
#
# Rows 175..199 are staged through the OFF-SCREEN CACHE at 0xC300 and blitted
# back down, because that region has separate storage and an untested path is
# not a path.
# ===========================================================================
PLANAR_CODE_BASE = 0x3000
MODEY_STRIDE = 16640            # 80 x 208, the id Galaxy engine's SCREENSIZE
MODEY_PAGE2 = 2 * MODEY_STRIDE  # 33280
MODEY_CACHE = 0xC300            # 3 x 16,640: FREESTART


def modey_program():
    """The guest program, assembled here so it sits next to its expectation.

    Hand-encoded rather than run through an assembler because the harness has no
    assembler in the loop; every instruction carries its mnemonic. Only backward
    jumps are used, so a label is always known by the time it is referenced.
    """
    c = bytearray()

    def emit(*b):
        c.extend(b)

    def back(op, target):
        """A backward rel8 branch. Fails loudly rather than silently wrapping if
        a body ever grows past the reach of a short jump."""
        emit(op)
        d = target - (len(c) + 1)
        assert -128 <= d <= -1, f"rel8 out of range: {d}"
        emit(d & 0xFF)

    # ---- mode 13h through the BIOS. INT 10h and not a host call because the
    # chain-4 arm is GATED on BDA 0040:0049 being 0x13, and only the BIOS mode
    # set publishes that -- a blind Sequencer probe must not be able to
    # reinterpret some other mode as planar.
    emit(0xB8, 0x13, 0x00)                  # mov ax,0013h
    emit(0xCD, 0x10)                        # int 10h

    # ---- unchain. Sequencer index 4 = 0x06 clears Chain-4 and leaves Extended
    # Memory and Odd/Even set as a real Mode Y setup does -- byte for byte what
    # Wolfenstein 3-D writes. A WORD `out dx,ax`, which is how real software
    # programs these registers and is where a byte-only port path loses AH.
    emit(0xBA, 0xC4, 0x03)                  # mov dx,3C4h
    emit(0xB8, 0x04, 0x06)                  # mov ax,0604h
    emit(0xEF)                              # out dx,ax
    emit(0xB8, 0x00, 0xA0)                  # mov ax,0A000h
    emit(0x8E, 0xC0)                        # mov es,ax
    emit(0xFC)                              # cld

    # ---- band A: rows 0..49, pixel = x & 0xFF, one plane at a time.
    emit(0x31, 0xF6)                        # xor si,si        ; plane 0..3
    emit(0xB3, 0x01)                        # mov bl,1         ; Map Mask
    a_plane = len(c)
    emit(0xBA, 0xC4, 0x03)                  # mov dx,3C4h
    emit(0xB0, 0x02)                        # mov al,2
    emit(0xEE)                              # out dx,al
    emit(0x42)                              # inc dx
    emit(0x88, 0xD8)                        # mov al,bl
    emit(0xEE)                              # out dx,al
    emit(0x31, 0xFF)                        # xor di,di        ; plane offset 0
    emit(0xBD, 0x32, 0x00)                  # mov bp,50        ; rows to go
    a_row = len(c)
    emit(0x89, 0xF0)                        # mov ax,si        ; column 0's value
    emit(0xB9, 0x50, 0x00)                  # mov cx,80        ; offsets per row
    a_col = len(c)
    emit(0xAA)                              # stosb
    emit(0x04, 0x04)                        # add al,4         ; four pixels on
    back(0xE2, a_col)                       # loop a_col
    emit(0x4D)                              # dec bp
    back(0x75, a_row)                       # jnz a_row
    emit(0x46)                              # inc si
    emit(0xD0, 0xE3)                        # shl bl,1
    emit(0x83, 0xFE, 0x04)                  # cmp si,4
    back(0x72, a_plane)                     # jb a_plane

    # ---- band B: rows 50..99, pixel = (x + y) & 0xFF.
    emit(0x31, 0xF6)                        # xor si,si
    emit(0xB3, 0x01)                        # mov bl,1
    b_plane = len(c)
    emit(0xBA, 0xC4, 0x03)                  # mov dx,3C4h
    emit(0xB0, 0x02)                        # mov al,2
    emit(0xEE)                              # out dx,al
    emit(0x42)                              # inc dx
    emit(0x88, 0xD8)                        # mov al,bl
    emit(0xEE)                              # out dx,al
    emit(0xBF, 0xA0, 0x0F)                  # mov di,4000      ; row 50
    emit(0xBD, 0x32, 0x00)                  # mov bp,50        ; bp doubles as y
    b_row = len(c)
    emit(0x89, 0xF0)                        # mov ax,si
    emit(0x01, 0xE8)                        # add ax,bp        ; plane + y
    emit(0xB9, 0x50, 0x00)                  # mov cx,80
    b_col = len(c)
    emit(0xAA)                              # stosb
    emit(0x04, 0x04)                        # add al,4
    back(0xE2, b_col)                       # loop b_col
    emit(0x45)                              # inc bp
    emit(0x83, 0xFD, 0x64)                  # cmp bp,100
    back(0x72, b_row)                       # jb b_row
    emit(0x46)                              # inc si
    emit(0xD0, 0xE3)                        # shl bl,1
    emit(0x83, 0xFE, 0x04)                  # cmp si,4
    back(0x72, b_plane)                     # jb b_plane

    # ---- band C: rows 100..149, pixel = y & 0xFF, Map Mask 0x0F.
    # Every store paints all four planes, i.e. four pixels of the same value.
    # Constant ALONG a row, so this band is the transpose of band A and no
    # single wrong stride satisfies both.
    emit(0xBA, 0xC4, 0x03)                  # mov dx,3C4h
    emit(0xB8, 0x02, 0x0F)                  # mov ax,0F02h
    emit(0xEF)                              # out dx,ax
    emit(0xBF, 0x40, 0x1F)                  # mov di,8000      ; row 100
    emit(0xBB, 0x64, 0x00)                  # mov bx,100       ; y
    c_row = len(c)
    emit(0x88, 0xD8)                        # mov al,bl
    emit(0xB9, 0x50, 0x00)                  # mov cx,80
    emit(0xF3, 0xAA)                        # rep stosb
    emit(0x43)                              # inc bx
    emit(0x81, 0xFB, 0x96, 0x00)            # cmp bx,150
    back(0x72, c_row)                       # jb c_row

    # ---- the page flips, to 0x4100 and then 0x8200, and LEFT there.
    # Their DIFFERENCE is the only place a guest ever states its page stride,
    # which is what the POSITIVE control (a deliberately wrong X86_MODEY_PAGE0)
    # makes the core learn. Word OUTs again.
    emit(0xBA, 0xD4, 0x03)                  # mov dx,3D4h
    emit(0xB8, 0x0C, 0x41)                  # mov ax,410Ch     ; start hi = 0x41
    emit(0xEF)                              # out dx,ax
    emit(0xB8, 0x0D, 0x00)                  # mov ax,000Dh     ; start lo = 0
    emit(0xEF)                              # out dx,ax
    emit(0xB8, 0x0C, 0x82)                  # mov ax,820Ch     ; start hi = 0x82
    emit(0xEF)                              # out dx,ax
    emit(0xB8, 0x0D, 0x00)                  # mov ax,000Dh
    emit(0xEF)                              # out dx,ax

    # ---- band D: rows 150..199, a LATCH COPY of band C through write mode 1.
    # The READ is what loads the latches, and in Mode Y that read is the only
    # exact one there is -- the byte it returns in AL deliberately is not.
    emit(0xBA, 0xCE, 0x03)                  # mov dx,3CEh
    emit(0xB8, 0x05, 0x01)                  # mov ax,0105h     ; write mode 1
    emit(0xEF)                              # out dx,ax

    def latch_copy(src, dst, count):
        emit(0xBE, src & 0xFF, src >> 8)      # mov si,src
        emit(0xBF, dst & 0xFF, dst >> 8)      # mov di,dst
        emit(0xB9, count & 0xFF, count >> 8)  # mov cx,count
        top = len(c)
        emit(0x26, 0x8A, 0x04)              # mov al,[es:si]  ; loads the latches
        emit(0x26, 0x88, 0x05)              # mov [es:di],al  ; latches go back
        emit(0x46)                          # inc si
        emit(0x47)                          # inc di
        back(0xE2, top)                     # loop

    # Rows 100..124 -> rows 150..174, both addressed THROUGH PAGE 2,
    # deliberately: every plane offset folds into one page of storage, so page 2
    # + k must reach the same bytes as k -- on the READ and on the WRITE alike.
    # Addressing band D at its unaliased offset instead leaves the write-side
    # fold untested, and in the reference it did: the mutation that deletes the
    # fold SURVIVED until this copy was moved up here.
    latch_copy(MODEY_PAGE2 + 8000, MODEY_PAGE2 + 12000, 2000)
    # Rows 125..149 -> the OFF-SCREEN CACHE -> rows 175..199 of page 2. That
    # region has its own storage; folding it into the visible page is what
    # painted a band of garbage across Wolfenstein's setup screen.
    latch_copy(10000, MODEY_CACHE, 2000)
    latch_copy(MODEY_CACHE, MODEY_PAGE2 + 14000, 2000)

    emit(0xBA, 0xCE, 0x03)                  # mov dx,3CEh
    emit(0xB8, 0x05, 0x00)                  # mov ax,0005h     ; write mode 0
    emit(0xEF)                              # out dx,ax
    emit(0xF4)                              # hlt
    return bytes(c)


def modey_want_index(x, y):
    if y < 50:
        return x & 0xFF                     # A vertical bars, plane at a time
    if y < 100:
        return (x + y) & 0xFF               # B diagonals
    if y < 150:
        return y & 0xFF                     # C horizontal bars, Map Mask 0x0F
    return (y - 50) & 0xFF                  # D latch copy of band C


def expected_modey():
    """Computed from the PATTERN, never from a second copy of the emulator's
    decode -- two copies of one mistake agree with each other."""
    img = bytearray(FB_STRIDE * 240)
    for y in range(200):
        row = FB_TOP + y * FB_STRIDE
        for x in range(320):
            img[row + x] = modey_want_index(x, y)
    return bytes(img)


def modey_checks(b, syms, quiet=False):
    """Run the Mode Y program on the target and compare all 64,000 pixels."""
    def say(*a):
        if not quiet:
            print(*a)

    for n in ("run_planar_flag", "planar_r", "test_ram"):
        if n not in syms:
            print(f"FAIL: harness has no {n} -- rebuild tests/main.c")
            return False

    b.write_uint32(syms["run_planar_flag"], 1)
    if not wait_flag(b, syms["run_planar_flag"], 0, "planar init hung"):
        return False

    b.write_memory(syms["test_ram"] + PLANAR_CODE_BASE, modey_program())
    b.write_uint32(syms["run_planar_flag"], 2)
    if not wait_flag(b, syms["run_planar_flag"], 0, "Mode Y program hung",
                     tries=600):
        return False

    r = [b.read_uint32(syms["planar_r"] + 4 * i) for i in range(20)]
    mode, bda, unchained, page = r[0], r[1], r[2], r[3]
    stores, lost, flips = r[4], r[5], r[6]
    say(f"  mode={mode:#04x} bda449={bda:#04x} unchained={unchained} "
        f"page={page} stores={stores} lost={lost} flips={flips} "
        f"latch={r[8]}/{r[9]} exit={r[7]:#x}")
    say(f"  write classes: plane={r[16]} copy={r[17]} general={r[18]}")

    ok = True
    # COVERAGE, and it gates. The store path has two fast classes decided at
    # OUT time; if this guest program never entered one of them, then
    # "64000/64000 exact" says nothing about it and the fast path has no
    # oracle. A fast path with no oracle is how a wrong register combination
    # becomes silent corruption, which is the failure this area repeats.
    # This arm is where the two FAST classes are graded -- a Mode Y title's
    # frame is made of exactly these two idioms and nothing else, which is why
    # `general` is 0 here and is graded by the EGA 0Dh arm instead.
    for name, n in (("plane", r[16]), ("copy", r[17])):
        if n == 0:
            print(f"FAIL: not one store took the '{name}' write class, so the "
                  f"64,000-pixel comparison does not grade it. Either the "
                  f"guest program stopped exercising that idiom or the "
                  f"classification in planar_reclass() no longer selects it. "
                  f"A fast path with no oracle is how a wrong register "
                  f"combination becomes silent corruption.")
            ok = False
    if mode != 0x13 or bda != 0x13:
        print(f"FAIL: mode 13h was never adopted (mode {mode:#x}, BDA {bda:#x})")
        ok = False
    if not unchained:
        print("FAIL: the guest cleared Chain-4 and the adapter is still CHAINED."
              " Either the Sequencer write never reached "
              "x86_vga_planar_seq_write() or the BDA gate rejected it. This is "
              "the four-copies bug.")
        ok = False
    if page != MODEY_STRIDE:
        print(f"FAIL: page stride learned as {page}, want {MODEY_STRIDE}. The "
              f"guest's two flips are {MODEY_STRIDE} apart and that is the only "
              f"place it ever states the number.")
        ok = False
    if flips < 2:
        print(f"FAIL: only {flips} display-start changes seen; the guest made "
              f"two. Present-on-page-flip hangs off that edge.")
        ok = False
    if stores == 0:
        print("FAIL: not one store took the aperture hook -- cpu->vga_base was "
              "never armed, so every planar write landed raw in the aperture.")
        ok = False
    if not ok:
        return False

    b.write_uint32(syms["run_video_flag"], 3)
    if not wait_flag(b, syms["run_video_flag"], 0, "Mode Y blit did not finish"):
        return False
    got = bytes(b.read_memory(syms["cga_test_out"], FB_STRIDE * 240))
    want = expected_modey()

    bad = [i for i in range(FB_TOP, FB_TOP + 200 * FB_STRIDE)
           if got[i] != want[i]]
    if bad:
        i = bad[0]
        y, x = divmod(i - FB_TOP, FB_STRIDE)
        band = ["A vertical-bars/map-mask", "B diagonals", "C horizontal-bars",
                "D latch-copy"][min(y // 50, 3)]
        print(f"FAIL: Mode Y {len(bad)}/64000 pixels wrong; first ({x},{y}) "
              f"got {got[i]} want {want[i]}  [band {band}]")
        return False

    say("PASS: Mode Y 64000/64000 pixels exact (chain-4 off + map mask + "
        "interleave + CRTC start/stride + page fold + latch copy + off-screen "
        "cache)")

    # Put the fold back the way the harness set it up, so any arm that runs
    # after this one is not looking at the aperture through 0xA0000.
    b.write_uint32(syms["run_planar_flag"], 3)
    wait_flag(b, syms["run_planar_flag"], 0, "planar unmap hung")
    return True


# ===========================================================================
# EGA mode 0Dh -- 320x200, 16 colours, four bitplanes at 0xA0000.
#
# Ported from references/8086tiny/test286/tega.asm AND tega2.asm, MERGED,
# because the pair exists for a reason that is recorded and general
# (11-ega-planar.md, "Why the first test passed while a real game rendered
# garbage"). tega.asm compared all 64,000 pixels, was mutation-tested when it
# was written, and passed unchanged through ALL FIVE of the bugs that made
# Commander Keen 4 render as horizontal streaks of coloured dashes -- because it
# drove the adapter the way a TEST does:
#
#   tega.asm did                  real software does            bug it hid
#   byte OUT for index then data  word `out dx,ax`, one insn    word OUT drops AH
#   ATC index 0x01                ATC index 0x21 (bit 5 set)    index masked 0x3F
#   start 0, stride 40, no pan    reprograms all three a frame  blit ignores CRTC
#   stayed inside the first 16 KB uses the whole 64 KB window   writes land raw
#   never probed unknown ports    FreeDOS walks 0x3C0-0x3CF     the probe armed it
#
# So this program does the right-hand column: EVERY register write is a word
# OUT, the ATC index carries bit 5, the CRTC Offset is 22 (a 352-pixel virtual
# screen, not the default 40 bytes), the display start is 0x0100, there are
# three pixels of pel panning, there is a write above the 16 KB window whose
# read-back must be ZERO, and there is a blind 0xFF to port 0x3C2.
#
# The virtual screen is also deliberately WIDER THAN THE CARD CAN HOLD: at 8,192
# plane offsets the bottom right of the display runs past fitted plane memory and
# must come back BLACK. The card-size boundary is then visible in the picture
# instead of being an internal detail.
#
# FOUR BANDS, each isolating one mechanism so a wrong answer in one cannot be
# produced by a bug in another. u is the SOURCE pixel index within the virtual
# row, i.e. displayed x plus the pel pan:
#
#   rows   0..49   Map Mask     each plane filled separately    colour = u & 15
#   rows  50..99   Bit Mask +   repaint under mask 0x18, six    15 where u%8 is
#                  latches      bits coming from the latches    3 or 4, else 1
#   rows 100..149  Set/Reset    CPU byte is 0xAA and must be    colour = y & 15
#                               ignored entirely
#   rows 150..199  Write mode 1 a pure latch copy of rows 0-49  colour = u & 15
#
# The bands are chosen so a plausible-looking wrong answer is still a detectable
# one: if write mode 1 were unimplemented the bottom band would mirror AL --
# a pattern, not a blank -- which is why the comparison is pixel-exact rather
# than "does something appear".
# ===========================================================================
EGA_START = 0x0100      # non-zero display start, in plane offsets
EGA_OFFSET = 22         # CRTC 0x13, in WORDS -> 44 bytes per row
EGA_PITCH = 2 * EGA_OFFSET
EGA_PAN = 3             # ATC 0x13, horizontal pel pan
EGA_PLANE = 0x2000      # X86_EGA_PLANE_OFFSETS: 8,192 -> a 32 KB card
EGA_BAND = EGA_PITCH * 50
EGA_PROBE = 0x5000      # a window offset above the 16 KB CPU-visible shadow
EGA_PROBE_SLOT = 0x2F00  # guest DS:offset the read-back is parked in
EGA_READ_SLOT = 0x2F10   # six read-path answers, one byte each (see below)


def ega_program():
    """The guest program. Same hand-encoding as modey_program(); see there."""
    c = bytearray()

    def emit(*b):
        c.extend(b)

    def back(op, target):
        emit(op)
        d = target - (len(c) + 1)
        assert -128 <= d <= -1, f"rel8 out of range: {d}"
        emit(d & 0xFF)

    def wout(port, index, data):
        """`mov dx,port / mov ax,(data<<8)|index / out dx,ax` -- the word form,
        which is the only form real software uses and the one that loses AH if
        the port layer treats a 16-bit OUT as a byte."""
        emit(0xBA, port & 0xFF, port >> 8)          # mov dx,port
        emit(0xB8, index, data)                     # mov ax,(data<<8)|index
        emit(0xEF)                                  # out dx,ax

    emit(0xB8, 0x0D, 0x00)                  # mov ax,000Dh
    emit(0xCD, 0x10)                        # int 10h
    emit(0xB8, 0x00, 0xA0)                  # mov ax,0A000h
    emit(0x8E, 0xC0)                        # mov es,ax
    emit(0xFC)                              # cld

    # ---- CRTC: a non-default Offset and a non-zero display start.
    wout(0x3D4, 0x13, EGA_OFFSET)           # Offset = 22 words = 44 bytes
    wout(0x3D4, 0x0C, EGA_START >> 8)       # start high
    wout(0x3D4, 0x0D, EGA_START & 0xFF)     # start low

    # ---- ATC pel panning, through the real sequence: read 0x3DA to clear the
    # flip-flop, then index and data on the SAME port, with bit 5 of the index
    # set (Palette Address Source) exactly as real software leaves it.
    emit(0xBA, 0xDA, 0x03)                  # mov dx,3DAh
    emit(0xEC)                              # in al,dx
    emit(0xBA, 0xC0, 0x03)                  # mov dx,3C0h
    emit(0xB0, 0x13 | 0x20)                 # mov al,33h
    emit(0xEE)                              # out dx,al
    emit(0xB0, EGA_PAN)                     # mov al,3
    emit(0xEE)                              # out dx,al

    # ---- a blind 0xFF to Miscellaneous Output, which is what FreeDOS's boot
    # probe does and what armed planar and wiped VRAM on the reference port.
    emit(0xBA, 0xC2, 0x03)                  # mov dx,3C2h
    emit(0xB0, 0xFF)                        # mov al,0FFh
    emit(0xEE)                              # out dx,al

    # =======================================================================
    # THE READ PATH, which the four bands below do not touch and which nothing
    # in this file asserted before. It runs FIRST, at plane offset 0 -- below
    # the display start, so the picture never sees it -- and it exists because
    # the read shadow is rebuilt under a GUARD, and a guard is exactly the kind
    # of thing that can be wrong in only one of its arms.
    #
    # plane 0 = 0xF0, plane 1 = 0x0F, planes 2/3 = 0 at offset 0. Then, in
    # order, with the answer parked in guest memory each time:
    #
    #   [0] Read Map Select 0        -> 0xF0    plane select
    #   [1] Read Map Select 1        -> 0x0F    ... and it CHANGED the answer
    #   [2] read mode 1, DC=3 CMP=1  -> 0xF0    p0 & ~p1
    #   [3]      ... CMP=2           -> 0x0F    ~p0 & p1     Colour Compare
    #   [4]      ... DC=1  CMP=2     -> 0x0F    ~p0          Colour Don't Care
    #   [5] back to read mode 0      -> 0x0F    Read Map Select 1 again
    #
    # Every one of the four registers the rebuild guard keys on moves an answer
    # here, so a guard that drops any of them is caught by a VALUE and not by a
    # timing. scripts/run_planar_controls.py drives the negative controls.
    # =======================================================================
    def rd_probe(slot):
        emit(0x26, 0xA0, 0x00, 0x00)        # mov al,[es:0000]
        emit(0xA2, slot & 0xFF, slot >> 8)  # mov [slot],al

    wout(0x3C4, 0x02, 0x01)                 # Map Mask = plane 0
    emit(0x31, 0xFF)                        # xor di,di
    emit(0xB0, 0xF0)                        # mov al,0F0h
    emit(0x26, 0x88, 0x05)                  # mov [es:di],al
    wout(0x3C4, 0x02, 0x02)                 # Map Mask = plane 1
    emit(0xB0, 0x0F)                        # mov al,0Fh
    emit(0x26, 0x88, 0x05)                  # mov [es:di],al
    wout(0x3C4, 0x02, 0x0F)                 # Map Mask back to all four

    wout(0x3CE, 0x04, 0x00)                 # Read Map Select = plane 0
    rd_probe(EGA_READ_SLOT + 0)
    wout(0x3CE, 0x04, 0x01)                 # Read Map Select = plane 1
    rd_probe(EGA_READ_SLOT + 1)
    wout(0x3CE, 0x07, 0x03)                 # Colour Don't Care = planes 0,1
    wout(0x3CE, 0x02, 0x01)                 # Colour Compare = plane 0 set
    wout(0x3CE, 0x05, 0x08)                 # Mode = READ MODE 1
    rd_probe(EGA_READ_SLOT + 2)
    wout(0x3CE, 0x02, 0x02)                 # Colour Compare = plane 1 set
    rd_probe(EGA_READ_SLOT + 3)
    wout(0x3CE, 0x07, 0x01)                 # Colour Don't Care = plane 0 only
    rd_probe(EGA_READ_SLOT + 4)
    wout(0x3CE, 0x05, 0x00)                 # back to read mode 0 (Read Map 1)
    rd_probe(EGA_READ_SLOT + 5)
    wout(0x3CE, 0x04, 0x00)                 # Read Map Select back to plane 0
    wout(0x3CE, 0x07, 0x0F)                 # Colour Don't Care back to default
    wout(0x3CE, 0x02, 0x00)                 # Colour Compare back to default

    def fill(mapmask, base, count, value):
        wout(0x3C4, 0x02, mapmask)          # Sequencer Map Mask
        emit(0xBF, base & 0xFF, base >> 8)  # mov di,base
        emit(0xB9, count & 0xFF, count >> 8)  # mov cx,count
        emit(0xB0, value)                   # mov al,value
        emit(0xF3, 0xAA)                    # rep stosb

    # ---- band A: colour = u & 15, one plane at a time.
    # Bit p of (u & 15) over eight consecutive u is a CONSTANT byte for planes
    # 0-2 (0x55, 0x33, 0x0F) and alternates 0x00/0xFF per group for plane 3.
    # Filling each plane separately is what makes this the Map Mask band: a Map
    # Mask that is ignored puts every constant into every plane.
    fill(0x01, EGA_START, EGA_BAND, 0x55)
    fill(0x02, EGA_START, EGA_BAND, 0x33)
    fill(0x04, EGA_START, EGA_BAND, 0x0F)
    wout(0x3C4, 0x02, 0x08)                 # Map Mask = plane 3 only
    emit(0xBF, EGA_START & 0xFF, EGA_START >> 8)    # mov di,start
    emit(0xB9, EGA_BAND & 0xFF, EGA_BAND >> 8)      # mov cx,2200
    emit(0xB0, 0x00)                        # mov al,0
    a_alt = len(c)
    emit(0xAA)                              # stosb
    emit(0x34, 0xFF)                        # xor al,0FFh
    back(0xE2, a_alt)                       # loop a_alt

    # ---- band B: colour 1 everywhere, then repaint under Bit Mask 0x18.
    # The six bits the mask does NOT cover have to come back from the LATCHES,
    # so this band fails if a read-modify-write does not load them.
    b_base = EGA_START + EGA_BAND
    fill(0x01, b_base, EGA_BAND, 0xFF)      # plane 0 only -> colour 1
    wout(0x3C4, 0x02, 0x0F)                 # every plane enabled
    wout(0x3CE, 0x08, 0x18)                 # Bit Mask = 0x18
    emit(0xBE, b_base & 0xFF, b_base >> 8)  # mov si,b_base
    emit(0xB9, EGA_BAND & 0xFF, EGA_BAND >> 8)      # mov cx,2200
    b_loop = len(c)
    emit(0x26, 0x8A, 0x1C)                  # mov bl,[es:si]  ; loads the latches
    emit(0xB0, 0xFF)                        # mov al,0FFh
    emit(0x26, 0x88, 0x04)                  # mov [es:si],al
    emit(0x46)                              # inc si
    back(0xE2, b_loop)                      # loop b_loop
    wout(0x3CE, 0x08, 0xFF)                 # Bit Mask back to 0xFF

    # ---- band C: Set/Reset supplies the colour and the CPU byte is 0xAA,
    # which must be ignored ENTIRELY. A path that quietly ORs the CPU byte in
    # renders 0xAA's bit pattern through it and this band says so.
    c_base = EGA_START + 2 * EGA_BAND
    wout(0x3CE, 0x01, 0x0F)                 # Enable Set/Reset, all four planes
    emit(0xBF, c_base & 0xFF, c_base >> 8)  # mov di,c_base
    emit(0xB3, 100)                         # mov bl,100      ; y
    c_row = len(c)
    emit(0xBA, 0xCE, 0x03)                  # mov dx,3CEh
    emit(0xB0, 0x00)                        # mov al,0        ; Set/Reset index
    emit(0x88, 0xDC)                        # mov ah,bl
    emit(0x80, 0xE4, 0x0F)                  # and ah,0Fh      ; colour = y & 15
    emit(0xEF)                              # out dx,ax
    emit(0xB0, 0xAA)                        # mov al,0AAh     ; must be ignored
    emit(0xB9, EGA_PITCH & 0xFF, EGA_PITCH >> 8)    # mov cx,44
    emit(0xF3, 0xAA)                        # rep stosb
    emit(0xFE, 0xC3)                        # inc bl
    emit(0x80, 0xFB, 150)                   # cmp bl,150
    back(0x72, c_row)                       # jb c_row
    wout(0x3CE, 0x01, 0x00)                 # Enable Set/Reset off

    # ---- band D: a PURE LATCH COPY of band A through write mode 1.
    d_base = EGA_START + 3 * EGA_BAND
    wout(0x3CE, 0x05, 0x01)                 # Mode register = write mode 1
    emit(0xBE, EGA_START & 0xFF, EGA_START >> 8)    # mov si,start
    emit(0xBF, d_base & 0xFF, d_base >> 8)          # mov di,d_base
    emit(0xB9, EGA_BAND & 0xFF, EGA_BAND >> 8)      # mov cx,2200
    d_loop = len(c)
    emit(0x26, 0x8A, 0x04)                  # mov al,[es:si]
    emit(0x26, 0x88, 0x05)                  # mov [es:di],al
    emit(0x46)                              # inc si
    emit(0x47)                              # inc di
    back(0xE2, d_loop)                      # loop d_loop
    wout(0x3CE, 0x05, 0x00)                 # back to write mode 0

    # ---- a write ABOVE the 16 KB CPU-visible window, then a read-back of it.
    # It must return ZERO. A non-zero answer means the access was treated as
    # plain memory: the raw guest byte landed somewhere the blit will render,
    # which is precisely the "horizontal streaks of coloured dashes" defect.
    emit(0xBF, EGA_PROBE & 0xFF, EGA_PROBE >> 8)    # mov di,5000h
    emit(0xB0, 0x5A)                        # mov al,5Ah
    emit(0x26, 0x88, 0x05)                  # mov [es:di],al
    emit(0x26, 0x8A, 0x05)                  # mov al,[es:di]
    emit(0xA2, EGA_PROBE_SLOT & 0xFF, EGA_PROBE_SLOT >> 8)   # mov [imm16],al
    emit(0xF4)                              # hlt
    return bytes(c)


def ega_want(x, y):
    """The colour index displayed at (x, y), from the PATTERN. u is the source
    pixel: the pel pan shifts the whole picture left by three, so displayed x
    reads source pixel x + 3."""
    u = x + EGA_PAN
    a = EGA_START + y * EGA_PITCH + (u >> 3)
    if a >= EGA_PLANE:
        # Past the plane memory this card has fitted. Reads as zero, exactly as
        # the store path discarded the writes there, so the card-size boundary
        # is a visible edge in the picture rather than an internal detail.
        return 0
    if y < 50 or y >= 150:
        return u & 15                       # A map mask; D is a latch copy of A
    if y < 100:
        return 15 if (u & 7) in (3, 4) else 1   # B bit mask + latches
    return y & 15                           # C set/reset


def expected_ega():
    img = bytearray(FB_STRIDE * 240)
    for y in range(200):
        row = FB_TOP + y * FB_STRIDE
        for x in range(320):
            img[row + x] = ega_want(x, y)
    return bytes(img)


def ega_checks(b, syms):
    """EGA 0Dh: the four write mechanisms, the CRTC window, and the card bound."""
    b.write_uint32(syms["run_planar_flag"], 1)
    if not wait_flag(b, syms["run_planar_flag"], 0, "planar init hung"):
        return False

    b.write_memory(syms["test_ram"] + PLANAR_CODE_BASE, ega_program())
    b.write_uint32(syms["run_planar_flag"], 2)
    if not wait_flag(b, syms["run_planar_flag"], 0, "EGA program hung",
                     tries=600):
        return False

    r = [b.read_uint32(syms["planar_r"] + 4 * i) for i in range(20)]
    print(f"  mode={r[0]:#04x} bda449={r[1]:#04x} ega_armed={r[10]} "
          f"stores={r[4]} lost={r[5]} arena_free={r[11]} exit={r[7]:#x}")
    print(f"  write classes: plane={r[16]} copy={r[17]} general={r[18]}")
    # THE GENERAL PATH'S coverage lives here, not in the Mode Y arm: 0Dh is
    # where set/reset, the ALU, write modes 2 and 3 and partial Bit Masks are
    # driven, and those are precisely the combinations planar_reclass() must
    # NOT claim for a fast path. A zero here would mean the classification had
    # started claiming them, and the pixels would be the only thing left to
    # notice -- which is the same as noticing nothing until a title breaks.
    if r[18] == 0:
        print("FAIL: not one store took the 'general' write class. 0Dh drives "
              "set/reset, the ALU and partial bit masks, and planar_reclass() "
              "must never claim those for a fast path -- a 0 here means it "
              "just did.")
        return False
    if r[0] != 0x0D or r[1] != 0x0D:
        print(f"FAIL: mode 0Dh was not adopted (mode {r[0]:#x}, BDA {r[1]:#x}). "
              f"If the arena could not place 32,768 bytes the mode is REFUSED, "
              f"which is the intended behaviour but not a passing test.")
        return False
    if not r[10]:
        print("FAIL: mode 0Dh adopted but the planar adapter is not armed, so "
              "every store landed raw in the aperture.")
        return False
    if r[5] == 0:
        print("FAIL: no store was discarded, yet the program deliberately writes "
              "past the fitted card (offset 0x5000 and the bottom of band D). "
              "Nothing is enforcing the card bound.")
        return False

    probe = b.read_memory(syms["test_ram"] + EGA_PROBE_SLOT, 1)[0]
    if probe != 0:
        print(f"FAIL: a write at window offset {EGA_PROBE:#x} -- above the 16 KB "
              f"CPU-visible shadow -- read back as {probe:#04x}, not 0. The "
              f"access was treated as plain memory, so the raw guest byte is "
              f"sitting where the blit will render it.")
        return False
    print(f"PASS: a write above the 16 KB window is discarded and reads back 0")

    # The read path. See the block in ega_program() for what each answer is and
    # which of the four guard registers it pins down.
    want_rd = [0xF0, 0x0F, 0xF0, 0x0F, 0x0F, 0x0F]
    why_rd = ["Read Map Select 0 -> plane 0",
              "Read Map Select 1 -> plane 1 (the SELECT itself)",
              "read mode 1, DC=3 CMP=1 -> p0 & ~p1",
              "read mode 1, CMP=2 -> ~p0 & p1 (Colour Compare)",
              "read mode 1, DC=1 CMP=2 -> ~p0 (Colour Don't Care)",
              "back to read mode 0 -> plane 1 again (leaving read mode 1)"]
    got_rd = list(b.read_memory(syms["test_ram"] + EGA_READ_SLOT, 6))
    for i, (g, w, why) in enumerate(zip(got_rd, want_rd, why_rd)):
        if g != w:
            print(f"FAIL: EGA read path [{i}] got {g:#04x} want {w:#04x} -- {why}. "
                  f"A guest read of A000:0 comes from the READ SHADOW, so this "
                  f"is either ega_read_byte() or the guard that decides when to "
                  f"rebuild the shadow.")
            return False
    print("PASS: EGA read path 6/6 (Read Map Select, read mode 1, Colour "
          "Compare, Colour Don't Care, and the return to read mode 0)")

    b.write_uint32(syms["run_video_flag"], 3)
    if not wait_flag(b, syms["run_video_flag"], 0, "EGA blit did not finish"):
        return False
    got = bytes(b.read_memory(syms["cga_test_out"], FB_STRIDE * 240))
    want = expected_ega()
    bad = [i for i in range(FB_TOP, FB_TOP + 200 * FB_STRIDE)
           if got[i] != want[i]]
    if bad:
        i = bad[0]
        y, x = divmod(i - FB_TOP, FB_STRIDE)
        band = ["A map-mask", "B bit-mask/latches", "C set/reset",
                "D write-mode-1"][min(y // 50, 3)]
        print(f"FAIL: EGA 0Dh {len(bad)}/64000 pixels wrong; first ({x},{y}) "
              f"got {got[i]} want {want[i]}  [band {band}]")
        return False
    print("PASS: EGA 0Dh 64000/64000 pixels exact (map mask + bit mask/latches "
          "+ set/reset + write mode 1 + word OUTs + CRTC Offset/start + pel pan "
          "+ the card bound)")

    b.write_uint32(syms["run_planar_flag"], 3)
    wait_flag(b, syms["run_planar_flag"], 0, "planar unmap hung")
    return True


MODEY_CACHE_BASE = 0xC300       # X86_MODEY_CACHE
WINDOW_OFFSETS = 0x10000        # the A segment decodes 64 KB per plane


def modeyfit_checks(b, syms):
    """The Mode Y off-screen cache, fitted against the real arena layout.

    Wolfenstein 3D caches its pictures off-screen and blits them out with latch
    copies, so text (drawn straight to the visible page) stayed perfect while
    every cached picture turned to noise: the cache was a fixed 8,192 offsets
    ending at 0xE2FF and the title writes up to 0xFFFF. The core fits it now,
    and this asserts the fit on the layout the core actually claims -- the
    arithmetic working out is NOT evidence that a contiguous run exists,
    which is exactly the fragmentation trap the EGA work found.
    """
    for n in ("run_modeyfit_flag", "modeyfit_r"):
        if n not in syms:
            print(f"missing symbol {n}")
            return False
    b.write_uint32(syms["run_modeyfit_flag"], 1)
    if not wait_flag(b, syms["run_modeyfit_flag"], 0, "Mode Y fit arm hung"):
        return False
    r = [b.read_uint32(syms["modeyfit_r"] + 4 * i) for i in range(16)]
    top = MODEY_CACHE_BASE + r[4]
    print(f"  claims={r[0]}{r[1]} largest_before={r[2]} mode={r[3]:#04x} "
          f"cache={r[4]} (0x{MODEY_CACHE_BASE:04x}..0x{top - 1:04x}) "
          f"cga_held={r[5]} largest_after={r[6]} | back: mode={r[7]:#04x} "
          f"cga_held={r[8]} cell={r[9]:#06x} cache={r[10]} | "
          f"control: mode={r[11]:#04x} cache={r[12]}")

    if not (r[0] and r[1]):
        print("FAIL: the harness could not make the core's own two claims")
        return False
    if r[2] != 45056:
        print(f"FAIL: expected 45,056 bytes free before the loan, got {r[2]}")
        return False
    if r[3] != 0x13:
        print(f"FAIL: mode 13h was not adopted (mode {r[3]:#x})")
        return False
    if top != WINDOW_OFFSETS:
        print(f"FAIL: the cache is {r[4]} plane offsets and reaches "
              f"0x{top:04x}, not 0x{WINDOW_OFFSETS:04x}. Everything from there "
              f"to the top of the window is DISCARDED, and Wolfenstein 3D "
              f"writes cached pictures over that whole range -- measured "
              f"0xEF00..0xFFFF under gwemu. {4 * r[4]} bytes is not enough; "
              f"{4 * (WINDOW_OFFSETS - MODEY_CACHE_BASE)} is.")
        return False
    if r[5]:
        print("FAIL: the CGA claim is still held while 13h is up, so the "
              "cache got only what was left over. Lending it is what makes "
              "the full window fit.")
        return False
    if r[7] != 0x03 or not r[8] or r[10]:
        print(f"FAIL: returning to text did not give the CGA claim back and "
              f"release the cache (mode {r[7]:#x}, held {r[8]}, cache {r[10]})")
        return False
    if r[9] != 0x0720:
        print(f"FAIL: the text buffer came back as {r[9]:#06x}, not blank "
              f"(0x0720). Its pages held cache bytes and were not cleared, so "
              f"the guest's screen is cache data read as glyphs.")
        return False
    # ---- the negative control: a small arena. A cache is not worth refusing a
    # whole mode over, so the right answer is a SMALLER cache, not a refusal.
    if r[11] != 0x13:
        print(f"FAIL: 13h was refused ({r[11]:#x}) because the full cache did "
              f"not fit. 13h renders from the aperture and does not need the "
              f"cache at all; only Mode Y's off-screen writes do.")
        return False
    if not (0 < r[12] < r[4]):
        print(f"FAIL: against a small arena the fit gave {r[12]} offsets, "
              f"expected something smaller than {r[4]} and non-zero. The walk "
              f"down is what makes a tight arena degrade instead of refuse.")
        return False
    print(f"PASS: Mode Y cache 8,192 -> {r[4]} plane offsets ({4 * r[4]} B) "
          f"covering 0x{MODEY_CACHE_BASE:04x}..0x{top - 1:04x}, the whole "
          f"off-screen window; CGA lent and returned blank; a tight arena "
          f"degrades to {r[12]} offsets rather than refusing the mode")
    b.write_uint32(syms["run_planar_flag"], 3)
    wait_flag(b, syms["run_planar_flag"], 0, "planar unmap hung")
    return True


def modeypage_checks(b, syms):
    """Mode Y's REAL page storage, and the property no pixel test can see.

    Every pixel arm in this suite renders one frame from one page and passes
    64000/64000 with all three of Wolf3D's display starts folded onto the same
    bytes. The defect only shows when the guest DRAWS page N+1 while the panel
    is showing page N -- which is the raycaster's every frame, and what the
    owner sees as walls rendered twice, once in perspective and once
    unstretched, with the doors split and out of order.

    So this asserts the property itself: a write to page 1 must not disturb
    page 0. Its negative control -- the same write with the alias -- is SEEN TO
    FAIL in the same run, which is what makes the positive half a check.
    """
    for n in ("run_modeypage_flag", "modeypage_r"):
        if n not in syms:
            print(f"missing symbol {n}")
            return False
    b.write_uint32(syms["run_modeypage_flag"], 1)
    if not wait_flag(b, syms["run_modeypage_flag"], 0, "Mode Y page arm hung"):
        return False
    r = [b.read_uint32(syms["modeypage_r"] + 4 * i) for i in range(24)]
    print(f"  stride={r[0]} aliased_pages={r[1]} -> real={r[5]} | "
          f"alias: page0={r[2]:#04x} (clobbered) hits={r[4]} | "
          f"real: page0={r[7]:#04x} page1={r[8]:#04x} page2={r[9]:#04x} "
          f"hits={r[10]} spill={r[13]} | half: pages={r[16]} "
          f"page0={r[17]:#04x} then {r[19]:#04x} loud={r[20]}")

    checks = [
        ("the learned stride is Wolf3D's",            r[0], 16640),
        ("one page: the aliased fold",                r[1], 1),
        # ---- NEGATIVE CONTROL. Without this failing, everything below is a
        # statement about a test that cannot fail.
        ("alias: page 1 CLOBBERS page 0",             r[2], 0x22),
        ("  ... page 1 holds it too",                 r[3], 0x22),
        ("  ... and the loud counter fired",          r[4] > 0, True),
        # ---- the property being bought
        ("three real pages",                          r[5], 3),
        ("  ... the counter reset with them",         r[6], 0),
        ("page 0 UNDISTURBED by a page 1 write",      r[7], 0x11),
        ("  ... page 1 holds its own byte",           r[8], 0x22),
        ("  ... page 2 holds its own byte",           r[9], 0x33),
        ("  ... nothing aliased",                     r[10], 0),
        ("three pages are three PLACES",              r[11], 1),
        ("  ... page 0 is still the aperture",        r[12], 1),
        ("a whole page 1 fill spills nothing",        r[13], 0),
        ("  ... page 0's byte survived it",           r[14], 0x11),
        ("  ... still nothing aliased",               r[15], 0),
        # ---- the half-funded rung, which is still worth taking
        ("one extra page: two real",                  r[16], 2),
        ("  ... page 0 undisturbed by page 1",        r[17], 0x44),
        ("  ... quiet so far",                        r[18], 0),
        ("  ... page 2 aliases onto page 0",          r[19], 0x66),
        ("  ... LOUDLY",                              r[20], 1),
        ("page 2 without page 1 is refused",          r[21], 1),
    ]
    bad = 0
    for name, got, want in checks:
        ok = got == want
        bad += not ok
        print(f"  {'PASS' if ok else 'FAIL'}: {name}: {got}"
              + ("" if ok else f" (expected {want})"))
    if bad:
        print(f"FAIL: Mode Y real pages: {bad} of {len(checks)} checks")
        return False
    print(f"PASS: Mode Y real page storage ({len(checks)} checks): a write to "
          f"page 1 leaves page 0 alone with real pages and OVERWRITES it with "
          f"the alias (negative control seen to fail); the half-funded rung "
          f"still protects the displayed page and says which page aliased")
    b.write_uint32(syms["run_planar_flag"], 3)
    wait_flag(b, syms["run_planar_flag"], 0, "planar unmap hung")
    return True


def egafit_checks(b, syms):
    """The ENLARGED EGA card: the CGA loan, the compaction, and the refusal.

    Keen 4 programs a CRTC Offset of 0x20 -- 64 bytes per row -- so an 8,192
    offset plane is 128 rows and the bottom three quarters of the intro was
    black. The card is sized from the arena now, and this arm asserts the
    arithmetic on the layout the real core claims rather than on whatever the
    bonus pool happened to release.
    """
    for n in ("run_egafit_flag", "egafit_r"):
        if n not in syms:
            print(f"missing symbol {n}")
            return False
    b.write_uint32(syms["run_egafit_flag"], 1)
    if not wait_flag(b, syms["run_egafit_flag"], 0, "EGA fit arm hung"):
        return False
    r = [b.read_uint32(syms["egafit_r"] + 4 * i) for i in range(16)]
    print(f"  claims={r[0]}{r[1]} largest_before={r[2]} mode={r[3]:#04x} "
          f"card={r[4]} cga_held={r[5]} armed={r[6]} | back: mode={r[8]:#04x} "
          f"cga_held={r[9]} cell={r[10]:#06x} | control: mode={r[12]:#04x} "
          f"refused={r[13]} deficit={r[15]}")

    if not (r[0] and r[1]):
        print("FAIL: the harness could not make the core's own two claims")
        return False
    if r[2] != 45056:
        print(f"FAIL: expected 45,056 bytes free before the loan, got {r[2]}")
        return False
    if r[3] != 0x0D or not r[6] or r[7]:
        print(f"FAIL: mode 0Dh was not adopted (mode {r[3]:#x}, armed {r[6]}, "
              f"refusals {r[7]})")
        return False
    if r[4] != 19456:
        print(f"FAIL: the card is {r[4]} plane offsets, expected 19,456. "
              f"77,824 contiguous bytes / 4 planes is the whole point of "
              f"lending the CGA claim and compacting the aperture; "
              f"{r[4] * 4} bytes means one of the two did not happen.")
        return False
    if r[5]:
        print("FAIL: the CGA claim is still held while 0Dh is up, so the card "
              "got only the tail of the arena")
        return False
    if r[8] != 0x03 or not r[9] or r[11]:
        print(f"FAIL: returning to text did not give the CGA claim back "
              f"(mode {r[8]:#x}, held {r[9]}, ega_armed {r[11]})")
        return False
    if r[10] != 0x0720:
        print(f"FAIL: the text buffer came back as {r[10]:#06x}, not blank "
              f"(0x0720). Its pages held EGA planes and the contents were not "
              f"cleared, so the guest's screen is plane bytes read as glyphs.")
        return False
    # ---- the negative control
    if r[12] == 0x0D:
        print("FAIL: 0Dh was ADOPTED against an arena with no spare page. A "
              "mode whose stores have nowhere to land must be refused.")
        return False
    if r[13] < 1 or r[14] or r[15] == 0:
        print(f"FAIL: the refusal was silent -- refusals {r[13]}, armed "
              f"{r[14]}, deficit {r[15]}. A refusal must be counted and must "
              f"quote what it could not place.")
        return False
    print(f"PASS: EGA card 8,192 -> {r[4]} plane offsets ({r[4] * 4} B, "
          f"{r[4] // 64} rows at Keen's 64-byte pitch); CGA lent and returned "
          f"blank; a card that does not fit is refused and counted")
    b.write_uint32(syms["run_planar_flag"], 3)
    wait_flag(b, syms["run_planar_flag"], 0, "planar unmap hung")
    return True


def present_checks(b, syms):
    """The Mode Y PRESENTATION POLICY -- which frames the loop paints.

    This grades a PORT. The policy is references/dos_port_prior/dos_video.c
    lines 1529-1656 (`modey_presented`, `modey_idle`, DOS_MODEY_IDLE_FRAMES=8),
    and 15-mode-y.md section 4 item 4 is its rationale:

      - the guest's page flip presents the screen, painting BOTH LCD buffers,
        because a display-start change is the only instant the one aliased page
        is a complete picture;
      - the frame after that the loop paints nothing and the idle count starts
        over;
      - after 8 frame-loop iterations with no flip the hold is ABANDONED and the
        loop paints every frame, exactly as every other mode does.

    That is the whole model. It makes no attempt to infer whether the guest has
    finished drawing -- nothing observable says so -- so a title slower than 8
    loop frames per flip TEARS, deliberately and by the reference's own account.
    The arms below grade the HOLD (nothing painted for 8 frames after a flip),
    the ANTI-FREEZE property (a title that never flips is painted from its very
    first frame, and revealed as it draws), and count the tearing rather than
    asserting it away.

    Four rounds of local invention replaced this policy -- a timer, a staleness
    bound, a doubling back-off, a cadence estimator with decay -- and the arms
    that graded them modelled their failure modes and went with them.
    """
    if "run_present_flag" not in syms:
        print("FAIL: no run_present_flag; the target predates the policy test")
        return False

    def run(which):
        b.write_uint32(syms["run_present_flag"], which)
        if not wait_flag(b, syms["run_present_flag"], 0, "present policy hung"):
            return None
        return [b.read_uint32(syms["present_r"] + 4 * i) for i in range(8)]

    HOLD = 8

    # ---- GAMEPLAY: 200 frames, stores in every one, a flip every 20th.
    # The guest is slower than the hold, so this is the tearing case. What must
    # hold exactly is the HOLD: no paint in the 8 frames after a flip.
    r = run(1)
    if r is None:
        return False
    if not r[7]:
        print("FAIL: Mode Y did not arm for the presentation policy test")
        return False
    if r[1] != 0:
        print(f"FAIL: the frame loop painted {r[1]} frames inside the {HOLD}-"
              f"frame hold that follows a page flip. The hold is the whole of "
              f"the reference's policy: the guest has just said its page is "
              f"complete and has started redrawing it, so a paint there is the "
              f"one aliased page caught at its emptiest.")
        return False
    # 19 frames before the first flip, then 11 of every 20 (20 - 1 flip frame
    # - 8 held). Exact, because the model is deterministic; a different number
    # means the hold length or the flip edge moved.
    if r[0] != 118 or r[2] != 118:
        print(f"FAIL: {r[0]} loop paints ({r[2]} fallbacks) over 200 frames at "
              f"a 20-frame flip gap; expected 118 = 19 before the first flip "
              f"plus 9 cycles of 11.")
        return False
    if r[3] != 20:
        print(f"FAIL: flip gap measured {r[3]}, expected 20")
        return False
    print(f"  gameplay at a 20-frame flip gap: {r[0]}/200 loop paints, "
          f"{r[4]} of them mid-draw (the reference's accepted tearing), "
          f"0 inside the hold")

    # ---- A TITLE FLIPPING FASTER THAN THE HOLD. This is the case the hold
    # exists for and it must be perfect: every frame on the panel is one the
    # guest called complete. -DX86_MODEY_PRESENT_ON_FLIP=0 must fail here.
    r = run(4)
    if r is None:
        return False
    if r[0] != 0 or r[1] != 0:
        print(f"FAIL: a guest flipping every 5 frames -- faster than the "
              f"{HOLD}-frame hold -- had {r[0]} frames painted by the loop "
              f"({r[1]} mid-draw). Present-on-flip is not holding at all; this "
              f"is the tearing model.")
        return False
    if r[2] != 5:
        print(f"FAIL: flip gap measured {r[2]}, expected 5")
        return False

    # ---- PROMPTNESS. The arm whose absence let a half-second freeze ship, and
    # the one the reverted commit's flicker was traded against. WOLF3D's intro
    # and menus do not flip (flips=2 across the whole signon/title phase against
    # 94 by the end of a level): they draw straight into the page they are
    # displaying, so the hatch is their only presentation path and its latency
    # is their frame time. A level is played first, because it is inheriting the
    # level's state that broke every previous version of this.
    r = run(6)
    if r is None:
        return False
    print(f"  promptness: first reveal {r[0]} frames after a level, "
          f"{r[1]} paints over the 60 that follow")
    if r[0] > HOLD + 1:
        print(f"FAIL: a guest that stopped flipping and drew an intro screen "
              f"in pieces showed nothing for {r[0]} frames (limit {HOLD + 1}). "
              f"The panel holds the OLD screen for the whole draw and then "
              f"swaps -- 'it freezes the image for a moment and then jumps to "
              f"where you tried to get to'.")
        return False
    if r[1] < 40:
        print(f"FAIL: only {r[1]} of the 60 frames after the hatch fired were "
              f"painted; once the hold is abandoned the loop paints EVERY "
              f"frame, which is what reveals an intro as it draws.")
        return False

    # ---- A MENU that never flips: revealed as it draws, and still repainted
    # while it is quiet. The second half is not an efficiency question -- it is
    # the assertion that NO state accumulates to suppress a later screen, which
    # is exactly how each invented policy regressed the one before it.
    r = run(2)
    if r is None:
        return False
    if r[0] != 4:
        print(f"FAIL: a menu drawing over 4 frames was painted on {r[0]} of "
              f"them; expected all 4. A non-flipping screen is revealed AS IT "
              f"DRAWS -- there is no flip coming to present it.")
        return False
    if r[1] != 30:
        print(f"FAIL: a quiet, non-flipping screen was painted {r[1]} times in "
              f"30 frames; expected 30. Anything less means state survived the "
              f"draw and is suppressing the paint.")
        return False
    if r[2] != 0:
        print(f"FAIL: {r[2]} mid-draw paints counted for a title that never "
              f"flipped; 'half-drawn' is measured against the last frame the "
              f"guest called complete and it never called one.")
        return False

    # ---- PATHOLOGICAL: stores forever, never flips. Not a frozen screen.
    r = run(3)
    if r is None:
        return False
    if r[0] != 180 or r[1] != 180:
        print(f"FAIL: 180 frames of unflipped drawing gave {r[0]} paints "
              f"({r[1]} fallbacks), expected 180 -- once the hold is abandoned "
              f"the loop paints every frame. Fewer is a frozen screen.")
        return False

    print("PASS: Mode Y presentation policy, ported from dos_video.c:1529 "
          "(nothing painted inside the 8-frame hold after a flip; a guest "
          "flipping faster than the hold is never painted by the loop; a "
          "non-flipping screen is revealed from its first frame and never "
          "freezes)")
    return True


def main(args):
    runner = build_and_create_runner(args)
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols(getattr(args, "elf", None) or "test_core.elf")
        if getattr(args, "present_only", False):
            # The presentation-policy arm on its own, for the FLIP control in
            # scripts/run_planar_controls.py. It touches no pixels and needs no
            # guest program, so nothing else has to run first.
            time.sleep(0.5)
            return present_checks(b, syms)
        if getattr(args, "modeyfit_only", False):
            # The fit arm alone, for the CACHE control. It builds its own arena
            # and its own video buffers, so nothing has to run before it.
            time.sleep(0.5)
            return modeyfit_checks(b, syms)
        if getattr(args, "ega_only", False):
            # The 0Dh arm alone, for the EGASIG/EGAMODE controls in
            # scripts/run_planar_controls.py. It arms its own mode and draws
            # its own picture, so nothing has to run before it.
            time.sleep(0.5)
            return ega_checks(b, syms)
        if getattr(args, "modey_only", False):
            # The single arm scripts/run_planar_controls.py builds and re-runs
            # once per control. Nothing else runs, so a control's verdict is
            # about the Mode Y pipeline and not about some other arm.
            time.sleep(0.5)
            return modey_checks(b, syms, quiet=getattr(args, "quiet", False))
        for n in ("run_video_flag", "cga_test_vram", "cga_test_out",
                  "video_blit_cycles"):
            if n not in syms:
                print(f"missing symbol {n}")
                return False

        # Let the C init settle before touching anything.
        time.sleep(0.5)

        # 1 -> the target initialises video (which zeroes VRAM) and answers 2.
        b.write_uint32(syms["run_video_flag"], 1)
        for _ in range(100):
            if b.read_uint32(syms["run_video_flag"]) == 2:
                break
            time.sleep(0.05)
        else:
            print("FAIL: target never acknowledged video init")
            return False

        vram = make_pattern()
        b.write_memory(syms["cga_test_vram"], vram)

        # 3 -> blit.
        b.write_uint32(syms["run_video_flag"], 3)
        for _ in range(200):
            if b.read_uint32(syms["run_video_flag"]) == 0:
                break
            time.sleep(0.05)
        else:
            print("FAIL: blit did not complete")
            return False

        got = bytes(b.read_memory(syms["cga_test_out"], FB_STRIDE * 240))
        want = expected_image(vram)
        cycles = b.read_uint32(syms["video_blit_cycles"])

        if got != want:
            for i, (g, w) in enumerate(zip(got, want)):
                if g != w:
                    row, col = divmod(i, FB_STRIDE)
                    print(f"FAIL: first mismatch at ({col},{row}) "
                          f"got {g} expected {w}")
                    break
            diff = sum(1 for g, w in zip(got, want) if g != w)
            print(f"  {diff} of {len(want)} bytes differ")
            return False

        px = CGA_WIDTH * CGA_HEIGHT
        print(f"PASS: {px} pixels exact")
        print(f"  blit: {cycles} cycles, {cycles / px:.2f} cyc/pixel")

        if not mode_switch_checks(b, syms):
            return False
        # LAST, deliberately: the planar arms remap guest 0xA0000/0xB8000 onto
        # the real aperture and VRAM for the duration, which is what the core
        # does and what the other arms do not expect.
        if not modey_checks(b, syms):
            return False
        if not present_checks(b, syms):
            return False
        # Right after the Mode Y arms, while the planar remap is up: this one
        # is the only arm that asserts what the pixel arms structurally cannot
        # see -- that the three display pages are three PLACES.
        if not modeypage_checks(b, syms):
            return False
        if not ega_checks(b, syms):
            return False
        # LAST: it re-points the video layer at arena blocks of its own and
        # rebuilds the arena around them, so nothing may run after it that
        # expects the harness's standard buffers still to be claimed.
        if not egafit_checks(b, syms):
            return False
        # LAST for the same reason egafit is: it rebuilds the arena around
        # blocks of its own.
        return modeyfit_checks(b, syms)
    finally:
        runner.stop()


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    p.add_argument("--ega-only", action="store_true",
                   help="only the EGA 0Dh arm (the read-shadow-guard controls)")
    p.add_argument("--modey-only", action="store_true",
                   help="run only the Mode Y arm (the controls driver uses this)")
    p.add_argument("--present-only", action="store_true",
                   help="run only the Mode Y presentation-policy arm")
    p.add_argument("--modeyfit-only", action="store_true",
                   help="run only the Mode Y off-screen-cache FIT arm (the "
                        "CACHE control in scripts/run_planar_controls.py)")
    p.add_argument("--quiet", action="store_true",
                   help="suppress the PASS chatter; the exit code is the verdict")
    args = p.parse_args()
    sys.exit(0 if main(args) else 1)
