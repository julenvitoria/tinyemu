#!/usr/bin/env python3
"""Unpack a LucasArts floppy install set (the `LFG!` split archive) to files.

LucasArts shipped its VGA floppy titles (Indiana Jones and the Fate of Atlantis,
Monkey Island 2, ...) as an INSTALL.EXE plus one `*.ND4`/`*.LFL`-style part per
disk. Running INSTALL.EXE in the core means five disk swaps; this reads the
parts directly so a title becomes a folder for make_dsk.py in one step.

    python3 scripts/lucasarts_unpack.py --out build/run/g4/ext \
        build/run/g4/raw/PCVIQA_A.ND4 ... PCVIQA_E.ND4

Format (read off the Spanish Indy 4 set, 2026-10-31):

- every part starts `LFG!` + uint32 LE; the FIRST part's header runs to the
  first `FILE` chunk, later parts carry just those 8 bytes, which are dropped
  before the parts are joined;
- a chunk is `FILE` + uint32 LE length (of what follows those 8 bytes),
  a 13-byte NUL-padded name, one byte, uint32 LE unpacked size, three uint16,
  then a PKWARE DCL "implode" stream (the library INSTALL.EXE links);
- the decompressor is a port of Mark Adler's public-domain blast.c (zlib
  contrib), which is the reference for that format. Every file's unpacked size
  is checked against its header.

Pure Python, no dependencies. Writes only into --out.
"""

import argparse
import os
import struct
import sys

LITLEN = [11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8,
          9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
          7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12,
          8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
          44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45,
          44, 173]
LENLEN = [2, 35, 36, 53, 38, 23]
DISTLEN = [2, 20, 53, 230, 247, 151, 248]
BASE = [3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264]
EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8]


def construct(rep):
    """Canonical Huffman table from blast.c's run-length-coded lengths."""
    lengths = []
    for b in rep:
        lengths += [b & 15] * ((b >> 4) + 1)
    count = [0] * 14
    for n in lengths:
        count[n] += 1
    offs = [0] * 14
    for i in range(1, 13):
        offs[i + 1] = offs[i] + count[i]
    sym = [0] * len(lengths)
    for s, n in enumerate(lengths):
        if n:
            sym[offs[n]] = s
            offs[n] += 1
    return count, sym


LIT, LEN, DIST = construct(LITLEN), construct(LENLEN), construct(DISTLEN)


def explode(src):
    """Decompress one DCL stream; returns the bytes."""
    st = {"pos": 0, "buf": 0, "cnt": 0}

    def bits(n):
        v = st["buf"]
        while st["cnt"] < n:
            v |= src[st["pos"]] << st["cnt"]
            st["pos"] += 1
            st["cnt"] += 8
        st["buf"] = v >> n
        st["cnt"] -= n
        return v & ((1 << n) - 1)

    def decode(h):
        count, sym = h
        code = first = index = 0
        for n in range(1, 14):
            code |= bits(1) ^ 1           # the codes are stored inverted
            c = count[n]
            if code < first + c:
                return sym[index + code - first]
            index += c
            first = (first + c) << 1
            code <<= 1
        raise ValueError("bad DCL code")

    lit, dictbits = bits(8), bits(8)
    if lit not in (0, 1) or not 4 <= dictbits <= 6:
        raise ValueError(f"not a DCL stream (header {lit}, {dictbits})")
    out = bytearray()
    while True:
        if bits(1):
            s = decode(LEN)
            n = BASE[s] + bits(EXTRA[s])
            if n == 519:                  # end of stream
                return bytes(out)
            db = 2 if n == 2 else dictbits
            dist = (decode(DIST) << db) + bits(db) + 1
            for _ in range(n):
                out.append(out[-dist])
        else:
            out.append(decode(LIT) if lit else bits(8))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True, help="folder to write the files to")
    ap.add_argument("parts", nargs="+", help="the parts, IN DISK ORDER")
    a = ap.parse_args()

    data = b""
    for i, p in enumerate(a.parts):
        b = open(p, "rb").read()
        if b[:4] != b"LFG!":
            sys.exit(f"error: {p} is not an LFG! part")
        data += b if i == 0 else b[8:]
    os.makedirs(a.out, exist_ok=True)
    i = data.find(b"FILE")
    while 0 <= i < len(data) and data[i:i + 4] == b"FILE":
        ln, = struct.unpack_from("<I", data, i + 4)
        name = data[i + 8:i + 21].split(b"\0")[0].decode("ascii")
        size, = struct.unpack_from("<I", data, i + 22)
        if os.path.basename(name) != name or not name:
            sys.exit(f"error: bad name {name!r}")
        blob = explode(data[i + 32:i + 8 + ln])
        if len(blob) != size:
            sys.exit(f"error: {name}: {len(blob)} bytes, header says {size}")
        with open(os.path.join(a.out, name), "wb") as f:
            f.write(blob)
        print(f"{name:13s} {size:9d}")
        i += 8 + ln


if __name__ == "__main__":
    main()
