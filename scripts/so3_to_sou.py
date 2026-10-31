#!/usr/bin/env python3
"""Rebuild a LucasArts speech file (MONSTER.SOU) from a ScummVM `.so3`.

The talkie CD releases keep every spoken line in MONSTER.SOU; ScummVM-ready
copies ship it MP3-compressed as `monster.so3`, which the original DOS
interpreter cannot read. This puts it back: each record lands at its ORIGINAL
offset (the game's scripts address lines by offset), as

    'VCTL' + uint32 BE (8 + n) + the n lip-sync bytes
    a Creative Voice File: 26-byte header, ONE 8-bit block, terminator

with the MP3 decoded to unsigned 8-bit mono at 11025 Hz (time constant 165)
and padded with silence or cut so the next record starts where it did.

    python3 scripts/so3_to_sou.py --src .../monster.so3 --out MONSTER.SOU

The `.so3` layout (from the compressed file itself): uint32 BE table size, then
16-byte entries (orig_offset, new_offset, lip-sync byte count, mp3 size; all
BE), then per entry the lip-sync bytes followed by the MP3.

Needs ffmpeg. The interpreter must be the talkie one from the CD: a floppy
ATLANTIS.EXE never opens MONSTER.SOU (it has no speech code at all), checked
2026-10-31 with the Spanish floppy EXE on the CD data.
"""

import argparse
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

RATE, TC = 11025, 165        # 1e6/(256-165) = 10989 Hz, the VOC's nearest
DELAY = 1105                 # LAME's encoder delay, 576 + 529 samples


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--src", required=True, help="monster.so3")
    ap.add_argument("--out", required=True, help="MONSTER.SOU to write")
    ap.add_argument("--jobs", type=int, default=4, help="parallel ffmpeg runs")
    a = ap.parse_args()

    d = open(a.src, "rb").read()
    n, = struct.unpack(">I", d[:4])
    ents = [struct.unpack_from(">IIII", d, 4 + 16 * i) for i in range(n // 16)]
    base = 4 + n

    def pcm(e):
        _, new, ntag, size = e
        mp3 = d[base + new + ntag:base + new + ntag + size]
        r = subprocess.run(["ffmpeg", "-v", "quiet", "-f", "mp3", "-i", "pipe:0",
                            "-ac", "1", "-ar", str(RATE), "-f", "u8", "pipe:1"],
                           input=mp3, capture_output=True)
        return r.stdout

    with ThreadPoolExecutor(a.jobs) as ex:
        pcms = list(ex.map(pcm, ents))

    out = bytearray(b"SOU " + bytes(4))
    padded = cut = 0
    for i, (org, new, ntag, _) in enumerate(ents):
        if len(out) != org:
            sys.exit(f"error: record {i} would start at {len(out)}, not {org}")
        rec = b"VCTL" + struct.pack(">I", 8 + ntag) + d[base + new:base + new + ntag]
        voc = b"Creative Voice File\x1a" + struct.pack("<HHH", 26, 0x010A, 0x1129)
        p = pcms[i]
        if i + 1 < len(ents):
            room = ents[i + 1][0] - org - len(rec) - len(voc) - 6 - 1
            if room < 0:
                sys.exit(f"error: record {i}: no room for its headers")
            # an MP3 starts with the encoder's delay (~1105 samples of
            # silence at 11 kHz, measured 1416-2806 of lead-in here) and is
            # ~2000 samples longer than the slot: drop the delay first, so a
            # cut takes padding off the end rather than the end of a word
            p = p[max(0, min(DELAY, len(p) - room)):]
            padded += len(p) < room
            cut += len(p) > room
            p = (p + b"\x80" * room)[:room]
        blk = b"\x01" + struct.pack("<I", len(p) + 2)[:3] + bytes([TC, 0]) + p
        out += rec + voc + blk + b"\x00"
    with open(a.out, "wb") as f:
        f.write(out)
    print(f"{a.out}: {len(out)} bytes, {len(ents)} lines "
          f"({padded} padded, {cut} trimmed to their original slot)")


if __name__ == "__main__":
    main()
