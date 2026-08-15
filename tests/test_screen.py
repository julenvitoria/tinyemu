#!/usr/bin/env python3
"""
Visual check of the CGA path through the display controller.

Rather than trusting the pointer the core drew into, this reads the LTDC's own
Layer-1 configuration -- the framebuffer address the display controller is
actually scanning out, its pixel format, and its line geometry -- then reads
that memory and the firmware's active CLUT and reconstructs the exact image the
panel receives. The result is written as a PNG and compared against the picture
the guest was asked to draw.

Under gwemu this proves everything up to the LTDC registers. gwemu's headless
display renders nothing at all (Retro-Go's own menu screenshots come back
black), so the panel itself can only be confirmed on hardware -- run with
--target hardware for that.

It checks whichever mode the core is actually in. With no bootable guest that
is the built-in mode 4 demo; with one on the card the guest has usually taken
the screen into text mode 3 by now, and the expected image is then recomputed
from the live 80x25 buffer. Asserting the demo unconditionally is what made
this test go red the moment a DOS program got far enough to draw its own UI --
a pass would have meant the guest had failed to start.
"""
import argparse
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, ".")
from scripts.common.target import Image, add_target_args, open_target
from scripts.common.elf import get_symbols
from scripts.common import textmode

LTDC_BASE = 0x50001000
LTDC_L1CR     = LTDC_BASE + 0x84    # layer enable
LTDC_L1PFCR   = LTDC_BASE + 0x94    # pixel format (5 = L8)
LTDC_L1CFBAR  = LTDC_BASE + 0xAC    # colour frame buffer address
LTDC_L1CFBLR  = LTDC_BASE + 0xB0    # pitch / line length
LTDC_L1CFBLNR = LTDC_BASE + 0xB4    # line count

SCREEN_W, SCREEN_H = 320, 240
CGA_W, CGA_H = 320, 200
CGA_TOP = (SCREEN_H - CGA_H) // 2


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def expected_indices():
    """What the built-in guest draws: VRAM filled with 0x6C1B, so every byte is
    0x1B or 0x6C. 0x1B -> pixels 0,1,2,3 and 0x6C -> 1,2,3,0, laid out over the
    interlaced planes exactly as the hardware reads them."""
    vram = bytes([0x1B, 0x6C] * (16384 // 2))
    img = bytearray(SCREEN_W * SCREEN_H)
    for row in range(CGA_H):
        src = (0x2000 if (row & 1) else 0) + (row // 2) * 80
        dst = (CGA_TOP + row) * SCREEN_W
        for col in range(80):
            b = vram[src + col]
            for p in range(4):
                img[dst + col * 4 + p] = (b >> (6 - 2 * p)) & 3
    return bytes(img)


def main(args):
    fw = args.firmware_dir
    image = Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                  bank2=os.path.join(fw, "qemu_bank2.bin"),
                  extflash=os.path.join(fw, "qemu_extflash.bin"),
                  sdcard=args.sd,
                  elf=os.path.join(fw, "gw_retro_go.elf"),
                  intflash_bank=1)
    if args.timeline is None:
        args.timeline = "timelines/start-preselected-game.tl"

    target = open_target(args, image)
    target.start()
    try:
        b = target.backend
        core = get_symbols(args.core_elf)

        # Wait for the core to be up and drawing.
        deadline = time.time() + 25
        while time.time() < deadline:
            if b.read_uint32(core["core_magic"]) == 0x54494E59 and \
               b.read_uint32(core["frames_rendered"]) > 2:
                break
            time.sleep(0.25)
        else:
            print("FAIL: core never started drawing")
            return False

        # Pin the 40-column window. Left free it follows the cursor, and the
        # host cannot know where that is, so the comparison would be against a
        # window the renderer did not use.
        if b.read_memory(core["x86_video_mode_byte"], 1)[0] in (0, 1, 2, 3):
            b.write_uint32(core["x86_video_pan"], 0)
            start = b.read_uint32(core["frames_rendered"])
            deadline = time.time() + 5
            while time.time() < deadline:
                if b.read_uint32(core["frames_rendered"]) > start + 2:
                    break
                time.sleep(0.05)

        # Halt for the reads: the framebuffer and the text buffer behind it
        # must be sampled from the same instant or the comparison is a race.
        b.halt()
        cr = b.read_uint32(LTDC_L1CR)
        pf = b.read_uint32(LTDC_L1PFCR)
        fb = b.read_uint32(LTDC_L1CFBAR)
        blr = b.read_uint32(LTDC_L1CFBLR)
        lnr = b.read_uint32(LTDC_L1CFBLNR)
        print(f"LTDC L1CR=0x{cr:08x} PFCR=0x{pf:08x} CFBAR=0x{fb:08x} "
              f"CFBLR=0x{blr:08x} CFBLNR=0x{lnr:08x}")

        if fb == 0:
            print("SKIP: this target does not model the LTDC (all registers "
                  "read back 0), so the scanned-out image cannot be recovered "
                  "here. Run with --target hardware.")
            return None

        pitch = (blr >> 16) & 0x1FFF
        lines = lnr & 0x7FF
        print(f"  pitch {pitch} bytes, {lines} lines, "
              f"format {'L8' if (pf & 7) == 5 else pf & 7}")

        fb_bytes = bytes(b.read_memory(fb, pitch * lines))

        # Read the FIRMWARE's active CLUT, not our copy. The LTDC's own CLUT
        # registers are write-only, so this is the last readable point in the
        # chain -- and it proves the handoff actually happened rather than that
        # we built the right table and dropped it.
        fw_syms = get_symbols(image.elf)
        clut_sym = fw_syms.get("active_clut")
        if clut_sym:
            clut = [b.read_uint32(clut_sym + 4 * i)
                    for i in range(textmode.TEXT_CLUT_ENTRIES)]
            src = "firmware active_clut"
        else:
            clut = [b.read_uint32(core["core_clut"] + 4 * i)
                    for i in range(textmode.TEXT_CLUT_ENTRIES)]
            src = "core copy (firmware symbol not found)"
        print(f"  CLUT via {src}: " + " ".join(f"0x{c:08x}" for c in clut[:8]))

        # Which renderer is on the screen? A bootable guest normally has the
        # display in text mode by now; without one the core draws its mode 4
        # demo. Checking the wrong one is not a stricter test, it is a false
        # failure.
        mode = b.read_memory(core["x86_video_mode_byte"], 1)[0]
        is_text = mode in (0x00, 0x01, 0x02, 0x03)
        print(f"  video mode 0x{mode:02x} ({'text' if is_text else 'graphics'})")

        # Absent from a core built without the anti-aliased renderer at all;
        # that is itself the answer, so do not make it an error.
        aa_sym = core.get("x86_text_aa")
        aa = b.read_memory(aa_sym, 1)[0] if (is_text and aa_sym) else 0
        if is_text and not aa:
            # Text attributes index the whole 16-colour CGA palette, so all 16
            # entries must have reached the firmware -- pushing only the four
            # mode 4 needs left every bright attribute drawing in a stale
            # colour from retro-go's menu. Under the anti-aliased renderer the
            # 32 entries are eight per-frame (fg,bg) blocks instead, and the
            # expectation is computed from the guest's own buffer below.
            if clut != textmode.CGA_ARGB * 2:
                print("FAIL: text mode but the CLUT is not the 16 CGA colours")
                for i, (g, w) in enumerate(zip(clut, textmode.CGA_ARGB * 2)):
                    if g != w:
                        print(f"  entry {i}: 0x{g:08x} expected 0x{w:08x}")
                return False
        elif not is_text:
            want_clut = [0xFF000000, 0xFF55FFFF, 0xFFFF55FF, 0xFFFFFFFF]
            if clut[:4] != want_clut:
                print("FAIL: CLUT is not the CGA palette the guest selected; "
                      "expected " + " ".join(f"0x{c:08x}" for c in want_clut))
                return False

        # Reconstruct exactly what the panel receives.
        rgb = bytearray(SCREEN_W * SCREEN_H * 3)
        for y in range(min(lines, SCREEN_H)):
            for x in range(SCREEN_W):
                idx = fb_bytes[y * pitch + x]
                argb = clut[idx] if idx < len(clut) else 0
                o = (y * SCREEN_W + x) * 3
                rgb[o] = (argb >> 16) & 0xFF
                rgb[o + 1] = (argb >> 8) & 0xFF
                rgb[o + 2] = argb & 0xFF
        write_png(args.out, SCREEN_W, SCREEN_H, bytes(rgb))
        print(f"  wrote {args.out}")

        if is_text:
            # The expectation comes from the guest's OWN text buffer, read
            # back here and rendered in Python: this is the end-to-end check
            # that what the panel scans out is what the guest wrote.
            vram = b.read_uint32(core["guest_vram"])
            cells = bytes(b.read_memory(vram, 80 * 25 * 2))
            b.resume()
            for r, line in textmode.dump(cells):
                print(f"  {r:2d} |{line}|")
            font_aa = textmode.load_font("font4x8aa.h") if aa else None
            if aa:
                want_clut = textmode.alloc_palette(cells, font_aa)[2]
                if clut != want_clut:
                    print("FAIL: the per-frame anti-aliased palette on the "
                          "target is not the one this buffer implies")
                    for i, (g, w) in enumerate(zip(clut, want_clut)):
                        if g != w:
                            print(f"  entry {i}: 0x{g:08x} expected 0x{w:08x}")
                    return False
                print(f"  AA palette: {len(textmode.alloc_palette(cells, font_aa)[4])}"
                      f" (fg,bg) blocks, exact")
            want = textmode.render(cells, textmode.load_font(), 0,
                                   SCREEN_W, SCREEN_H, font_aa=font_aa)
        else:
            b.resume()
            want = expected_indices()
        got = bytes(fb_bytes[y * pitch + x]
                    for y in range(SCREEN_H) for x in range(SCREEN_W))
        if got != want:
            bad = sum(1 for g, w in zip(got, want) if g != w)
            for i, (g, w) in enumerate(zip(got, want)):
                if g != w:
                    print(f"FAIL: first mismatch at ({i % SCREEN_W},"
                          f"{i // SCREEN_W}) got {g} expected {w}")
                    break
            print(f"  {bad} of {len(want)} pixels differ")
            return False

        nonblack = sum(1 for i in got if i)
        print(f"PASS: the LTDC is scanning the expected image "
              f"({nonblack} non-background pixels)")
        return True
    finally:
        target.stop()


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    add_target_args(p)
    p.add_argument("--firmware-dir",
                   default="references/game-and-watch-retro-go-sd/build")
    p.add_argument("--sd", default="sdcard_retrogo.img")
    p.add_argument("--core-elf", default="build/core/dos_core.elf")
    p.add_argument("--out", default="build/artifacts/screen.png")
    args = p.parse_args()
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    r = main(args)
    sys.exit(0 if r is not False else 1)
