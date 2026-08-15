#!/usr/bin/env python3
"""
Capture what the display controller is actually scanning out, as a PNG.

gwemu's own headless screendump comes back black -- Retro-Go's menu does too,
so those images prove nothing either way. Its LTDC registers ARE modelled,
though, so the real picture can be recovered: read Layer 1's framebuffer
address, pitch and line count from the controller, read that memory and the
firmware's active CLUT, and rebuild the frame the panel receives.

    # let the guest run for 20 s, then grab the screen
    python3 scripts/screenshot.py --wait 20 --out build/artifacts/topbench.png

    # the same on silicon, where the panel can also be believed
    python3 scripts/screenshot.py --target hardware --wait 20

`tests/test_screen.py` does this too, but only for the built-in CGA mode 4
demo and only to assert one exact image. This is the general form: any mode,
any guest, no expected image, and it prints a coarse text map of the frame so
a black or garbage capture is obvious without opening the file.
"""

import argparse
import os
import struct
import sys
import time
import zlib

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.elf import get_symbols                # noqa: E402
from scripts.common.target import Image, add_target_args, open_target  # noqa: E402

FW_DIR = "references/game-and-watch-retro-go-sd/build"
CORE_MAGIC = 0x54494E59

LTDC_BASE = 0x50001000
LTDC_L1CR = LTDC_BASE + 0x84
LTDC_L1PFCR = LTDC_BASE + 0x94
LTDC_L1CFBAR = LTDC_BASE + 0xAC
LTDC_L1CFBLR = LTDC_BASE + 0xB0
LTDC_L1CFBLNR = LTDC_BASE + 0xB4

SCREEN_W, SCREEN_H = 320, 240


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
    return len(png)


def text_map(indices, w, h, cols=64, rows=24):
    """A coarse map of which cells contain non-background pixels. Enough to see
    at a glance whether the capture is black, uniform, or has structure."""
    out = []
    for r in range(rows):
        line = []
        for c in range(cols):
            x0, x1 = c * w // cols, (c + 1) * w // cols
            y0, y1 = r * h // rows, (r + 1) * h // rows
            hit = any(indices[y * w + x]
                      for y in range(y0, y1) for x in range(x0, x1))
            line.append("#" if hit else ".")
        out.append("".join(line))
    return out


def report_pairs(cells, cols=80, rows=25):
    """Distinct (fg,bg) attribute pairs in an 80x25 buffer, by cell count.

    Anti-aliased text spends a CONTIGUOUS block of CLUT entries per distinct
    pair, so the budget is consumed by this number and not by the 256 pairs an
    attribute byte can express. Cells that draw only one colour are excluded:
    a space, a NUL and a solid block (0xDB) each paint a single flat colour, so
    they need no intermediate levels and no block of their own -- counting them
    overstates the requirement, which is the easy way to talk yourself out of a
    feature that fits.
    """
    flat = {0x00, 0x20, 0xDB, 0xFF}
    hist, flat_hist = {}, {}
    for i in range(cols * rows):
        ch, attr = cells[i * 2], cells[i * 2 + 1]
        key = (attr & 0x0F, (attr >> 4) & 0x07)
        if ch in flat or key[0] == key[1]:
            flat_hist[key] = flat_hist.get(key, 0) + 1
        else:
            hist[key] = hist.get(key, 0) + 1
    order = sorted(hist.items(), key=lambda kv: -kv[1])
    print(f"distinct (fg,bg) pairs needing intermediate levels: {len(order)}"
          f"  (flat-only cells use {len(flat_hist)} more)")
    for (fg, bg), n in order:
        print(f"  fg={fg:2d} bg={bg:2d}  {n:5d} cells")
    return len(order)


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_target_args(p)
    p.add_argument("--firmware-dir", default=FW_DIR)
    p.add_argument("--sd", default="sdcard_retrogo.img")
    p.add_argument("--core-elf", default="build/core/dos_core.elf")
    p.add_argument("--out", default="build/artifacts/screen.png")
    p.add_argument("--wait", type=float, default=15.0,
                   help="seconds to let the guest run before capturing")
    p.add_argument("--pan", type=int, default=None,
                   help="pin the text window's leftmost column. All 80 columns "
                        "fit the panel now (4 px cells), so this clamps to 0 "
                        "and changes nothing; kept for a future mode wider "
                        "than the panel")
    p.add_argument("--text", action="store_true",
                   help="also dump the 80x25 character buffer as CP437, which "
                        "is much easier to judge than condensed glyphs")
    p.add_argument("--attr", action="store_true",
                   help="dump the ATTRIBUTE byte of each cell alongside the "
                        "characters, as two hex nibbles (bg,fg) per column. "
                        "A --text dump alone is a trap on any screen where "
                        "the SHADING carries the meaning: Commander Keen's "
                        "startup screen lists every video mode, input device "
                        "and sound card it knows about and marks which ones "
                        "it DETECTED purely by brightening the attribute, so "
                        "a character dump of a screen detecting nothing is "
                        "byte-identical to one detecting everything")
    p.add_argument("--text-aa", type=int, choices=(0, 1), default=None,
                   help="select the text renderer before capturing: 0 is the "
                        "1-bit Everex font, 1 the anti-aliased one. Both live "
                        "in one build so the SAME screen can be captured each "
                        "way -- rebuilding between them would change the guest "
                        "run as well as the font, and the comparison with it")
    p.add_argument("--pairs", action="store_true",
                   help="count the DISTINCT (fg,bg) attribute pairs actually "
                        "used by the 80x25 buffer. Anti-aliased text needs a "
                        "contiguous block of CLUT entries per pair, so this "
                        "number -- not the 256 possible pairs -- is what "
                        "decides whether AA fits the 32-entry budget")
    p.add_argument("--clut-entries", type=int, default=32,
                   help="palette entries to read (4 for CGA graphics, 16 for "
                        "text mode's attribute colours)")
    args = p.parse_args()

    fw = args.firmware_dir
    image = Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                  bank2=os.path.join(fw, "qemu_bank2.bin"),
                  extflash=os.path.join(fw, "extflash.bin"),
                  sdcard=args.sd,
                  elf=os.path.join(fw, "gw_retro_go.elf"),
                  intflash_bank=1)
    if args.timeline is None:
        args.timeline = "timelines/start-preselected-game.tl"

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    target = open_target(args, image)
    target.start()
    try:
        b = target.backend
        core = get_symbols(args.core_elf)
        deadline = time.time() + 25
        while time.time() < deadline:
            if b.read_uint32(core["core_magic"]) == CORE_MAGIC and \
                    b.read_uint32(core["frames_rendered"]) > 2:
                break
            time.sleep(0.25)
        else:
            print("core never started drawing")
            return 1
        print(f"core drawing; letting it run {args.wait:g}s")
        time.sleep(args.wait)
        if args.pan is not None or args.text_aa is not None:
            b.halt()
            if args.pan is not None:
                b.write_uint32(core["x86_video_pan"], args.pan & 0xFFFFFFFF)
            if args.text_aa is not None:
                b.write_memory(core["x86_text_aa"], bytes([args.text_aa]))
            b.resume()
            # Let the core draw at least one frame with the new window.
            start = b.read_uint32(core["frames_rendered"])
            deadline = time.time() + 5
            while time.time() < deadline:
                if b.read_uint32(core["frames_rendered"]) > start + 2:
                    break
                time.sleep(0.05)
            print(f"panned the text window to column {args.pan}")

        b.halt()
        fb = b.read_uint32(LTDC_L1CFBAR)
        blr = b.read_uint32(LTDC_L1CFBLR)
        lnr = b.read_uint32(LTDC_L1CFBLNR)
        pf = b.read_uint32(LTDC_L1PFCR)
        frames = b.read_uint32(core["frames_rendered"])
        if fb == 0:
            print("this target does not model the LTDC; nothing to recover")
            return 1
        pitch = (blr >> 16) & 0x1FFF
        lines = lnr & 0x7FF
        print(f"LTDC CFBAR=0x{fb:08x} pitch={pitch} lines={lines} "
              f"fmt={pf & 7} frames={frames}")

        data = bytes(b.read_memory(fb, pitch * lines))
        core_syms = core
        fw_syms = get_symbols(image.elf)
        # A 256-colour mode does NOT use active_clut. lcd_set_clut_ext() parks
        # a CALLER-OWNED palette in ext_clut and clut_push() programs the LTDC
        # from that, bypassing the darkened-twin cache entirely -- so reading
        # active_clut here recovers the previous 32-entry CGA table and paints
        # every mode-13h frame in the wrong colours. That is a defect of THIS
        # TOOL that looks exactly like a palette defect in the core.
        ext_p = fw_syms.get("ext_clut")
        ext_n = fw_syms.get("ext_clut_count")
        ext_addr = b.read_uint32(ext_p) if ext_p else 0
        ext_count = (b.read_uint32(ext_n) & 0xFFFF) if ext_n else 0
        if ext_addr and ext_count:
            clut = [b.read_uint32(ext_addr + 4 * i) for i in range(ext_count)]
            print(f"external palette: {ext_count} entries at 0x{ext_addr:08x} "
                  f"(a 256-colour mode owns the whole table)")
        else:
            sym = fw_syms.get("active_clut") or core["core_clut"]
            clut = [b.read_uint32(sym + 4 * i) for i in range(args.clut_entries)]
        b.resume()

        print(f"CLUT ({len(clut)} entries, first 32): "
              + " ".join(f"{c:08x}" for c in clut[:32]))
        w, h = min(pitch, SCREEN_W), min(lines, SCREEN_H)
        indices = bytearray(w * h)
        rgb = bytearray(w * h * 3)
        for y in range(h):
            for x in range(w):
                idx = data[y * pitch + x]
                indices[y * w + x] = idx
                argb = clut[idx] if idx < len(clut) else 0
                o = (y * w + x) * 3
                rgb[o] = (argb >> 16) & 0xFF
                rgb[o + 1] = (argb >> 8) & 0xFF
                rgb[o + 2] = argb & 0xFF
        size = write_png(args.out, w, h, bytes(rgb))
        if args.text or args.pairs or args.attr:
            # In text mode the framebuffer is a rendering of an 80x25 character
            # buffer; reading the characters back is far easier to judge than
            # looking at 8x8 glyphs, and it shows the 40 columns the panel
            # cannot fit as well as the 40 it can.
            vram = b.read_uint32(core["guest_vram"])
            cells = bytes(b.read_memory(vram, 80 * 25 * 2))
            # ...and in a GRAPHICS mode those same bytes are pixels. Decoding
            # them as characters produces a screenful of plausible-looking
            # CP437 rubbish, which is worse than no dump: it reads as
            # corruption in a screen that is perfectly correct. The mode says
            # which, and it is one byte away.
            mode = 0xFF
            if "x86_video_mode_byte" in core:
                mode = b.read_uint32(core["x86_video_mode_byte"]) & 0xFF
            if mode not in (0x00, 0x01, 0x02, 0x03, 0x07, 0xFF):
                print(f"video mode {mode:#04x} is GRAPHICS -- 0xB8000 holds "
                      f"pixels, not characters, so there is no text buffer to "
                      f"dump (text printed in this mode is rasterised into the "
                      f"pixels by the BIOS)")
                args.text = args.pairs = args.attr = False
            # Decode as CP437, not ASCII. Printing only 0x20-0x7E hid every
            # box-drawing and shading code behind a '.', which made a
            # perfectly good full-screen UI read as four rows of stray
            # letters on an otherwise empty screen.
            if args.text:
                print("text buffer (80x25), CP437, '.' = space:")
                for r in range(25):
                    raw = bytes(cells[(r * 80 + c) * 2] for c in range(80))
                    line = "".join("." if ch == 0x20 else
                                   bytes([ch]).decode("cp437") if ch else "."
                                   for ch in raw)
                    if line.strip("."):
                        print(f"  {r:2d} |{line}|")
            if args.attr:
                print("attribute buffer (80x25), one hex digit pair per cell "
                      "(bg,fg); rows with only the dominant attribute omitted:")
                counts = {}
                for i in range(80 * 25):
                    a = cells[i * 2 + 1]
                    counts[a] = counts.get(a, 0) + 1
                dom = max(counts, key=counts.get)
                print(f"  dominant attribute {dom:#04x} "
                      f"({counts[dom]} of 2000 cells); "
                      f"{len(counts)} distinct attributes: "
                      + " ".join(f"{a:#04x}x{n}" for a, n in
                                 sorted(counts.items(), key=lambda kv: -kv[1])))
                for r in range(25):
                    row = [cells[(r * 80 + c) * 2 + 1] for c in range(80)]
                    if all(a == dom for a in row):
                        continue
                    print(f"  {r:2d} |" + "".join("--" if a == dom
                                                  else f"{a:02x}" for a in row)
                          + "|")
            if args.pairs:
                report_pairs(cells)
        nz = sum(1 for v in indices if v)
        print(f"wrote {args.out} ({w}x{h}, {size} bytes); "
              f"{nz} of {w * h} pixels non-zero")
        for line in text_map(indices, w, h):
            print("  " + line)
    finally:
        target.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
