#!/usr/bin/env python3
"""
Run a guest .dsk under a REFERENCE PC (QEMU) and read its memory out.

    python3 scripts/ref_dos.py --dsk references/.../WOLF3D.dsk --settle 35 \\
        --save build/artifacts/ref_wolf3d.bin
    python3 scripts/ref_dos.py --dsk ... --screen build/artifacts/ref.ppm
    python3 scripts/ref_dos.py --dsk ... --no-hooks       # neutralise CONFIG.SYS

------------------------------------------------------------------------------
WHY THIS AND NOT refdos.py
------------------------------------------------------------------------------
`ref_trace.py`/`refdos.py` run the EXECUTABLE under Unicorn against a hand-built
minimal DOS. That works up to the first thing the shim does not implement, and
for a title like Wolfenstein 3D it stops immediately: refdos has no file I/O and
no PORT I/O at all, so it cannot follow a guest past its first VGA access. That
gap is what stalled an investigation for a whole session.

This takes the opposite route and implements NOTHING. The .dsk our titles ship
as is a bootable MS-DOS volume -- IO.SYS, MSDOS.SYS, COMMAND.COM, CONFIG.SYS and
an AUTOEXEC.BAT that launches the game -- so a real PC emulator boots it and
gets file I/O, port I/O, the BIOS and DOS itself for free, with none of it
written by us and none of it able to agree with our x86_dos.c on the same
mistake. What it gives back is the thing that actually settles arguments: the
reference machine's whole low megabyte, byte-comparable against
`probe.py --guest-save`.

------------------------------------------------------------------------------
THE TWO TRAPS, BOTH OF WHICH COST REAL TIME HERE
------------------------------------------------------------------------------
1. **CHS geometry.** These images are built with a BPB of 1 HEAD and 63
   sectors/track, and the MS-DOS boot record reads IO.SYS through INT 13h using
   those numbers. QEMU's IDE default geometry for a 3 MB disk is not that, so
   the VBR loads the wrong sectors and SeaBIOS sits at 0x7C04 forever with
   "Booting from Hard Disk..." on screen and no error. The geometry is therefore
   read OUT OF THE BPB and passed to `ide-hd` explicitly, never guessed.
2. **The project's own CONFIG.SYS hooks.** `XMSHOOK.SYS`/`EMSHOOK.SYS` are
   written for OUR emulator; on a reference machine they can hang the boot
   before AUTOEXEC.BAT ever runs, which reads identically to "the game crashed".
   `--no-hooks` rewrites CONFIG.SYS/AUTOEXEC.BAT in a COPY of the image (the
   original .dsk is never modified) down to the minimum that still launches the
   title.

------------------------------------------------------------------------------
TWO ENGINES, AND WHY -- QEMU CANNOT RUN WOLFENSTEIN 3D AT ALL
------------------------------------------------------------------------------
`--engine qemu` (default) is the machine described above. `--engine dosbox`
runs the SAME image under DOSBox-X. That is not redundancy: it was forced by a
measurement.

**QEMU's VGA cannot satisfy Wolf3D's `VL_SetScreen`, and no option changes it.**
That routine waits for "active display, not in vertical retrace" and then for
the end of active display, with a TimeCount timeout as its only escape:

    mov cx,[TimeCount] / add cx,2
  L1: in al,3DA / test al,1 / jnz L1     ; wait for DISPLAY ENABLE
  L2: sti / cli / cmp [TimeCount],cx / jae done
      in al,3DA / test al,8 / jnz L1     ; back to L1 on VERTICAL RETRACE
                 test al,1 / jz  L2

QEMU's `vga_dumb_retrace()` returns `st01 ^ (ST01_V_RETRACE|ST01_DISP_ENABLE)`,
i.e. it flips BOTH bits on every read, so port 0x3DA alternates 0x00 <-> 0x09
and NEVER shows 0x01 or 0x08. **Measured** by single-stepping the guest over
QEMU's gdbstub: `eip=56 al=0 ... eip=65 al=9 -> back to 56`, forever. The
TimeCount escape cannot fire either, because TimeCount is only advanced by the
game's own INT 08h handler, which `SD_Startup()` installs LATER. `retrace=on`
(`vga_precise_retrace`) is dead code in modern QEMU -- the property is not
exposed on `VGA`, `isa-vga`, `cirrus-vga` or any other model in 10.1. So this
is not a tuning problem; the reference simply cannot get past the signon
screen, and **that stall is QEMU's, not the guest's and not ours**.

DOSBox-X models display-enable and vertical-retrace independently and runs the
title correctly. Measured with the shipped `WOLF3D.dsk`, unmodified apart from
`--no-hooks`: signon screen with MOUSE / JOYSTICK / ADLIB / SOUND BLASTER /
SOUND SOURCE detected, then the main menu, then episode select. INT 08h is
hooked by the game (`2aee:0095`) and INT 09h too (`26b7:0009`), which is the
direct proof that `SDL_SetTimer0()` and `IN_Startup()` ran.

The DOSBox-X engine boots the image's OWN MS-DOS (`imgmount 2 ... -fs none` +
`boot -l c`), not DOSBox-X's built-in DOS, so the software stack matches the
QEMU engine and our emulator. State comes out through DOSBox-X's `autosave=`
save states, which are ZIPs carrying the whole `Memory` and `Vga` components --
so `--save` still writes a byte-comparable low megabyte and `--screen` writes a
real PNG of the mode 13h/Mode-Y framebuffer.

Four DOSBox-X traps, each of which produced silence rather than an error:

1. **`[sdl] priority`'s second field is the INACTIVE priority and defaults to
   pausing.** Under `SDL_VIDEODRIVER=dummy` the window is never focused, so the
   emulator starts, prints its banner and then STOPS. `priority=normal,normal`.
2. **DOSBox-X's working directory is not yours.** Under the flatpak it is
   `~/.var/app/com.dosbox_x.DOSBox-X/config/dosbox-x`, so every relative path in
   `[autoexec]` silently resolves somewhere else and `imgmount` fails with no
   host-visible trace. All paths written into the config are ABSOLUTE.
3. **A host directory mounted with `mount d <path>` is not writable from the
   guest here**, so the shell cannot be used as an output channel. Everything
   observable comes from the save state and the `[log]` file instead.
4. **The signon screen waits for a key**, and a reference sitting there looks
   exactly like one that hung. `--keys` is delivered with `autotype -w`, which
   is asynchronous and survives the `boot`.

------------------------------------------------------------------------------
WHAT IT IS NOT
------------------------------------------------------------------------------
It is not a timing reference and not a claim about our silicon. And a reference
that is merely RUNNING is not a reference that has reached the same place the
title reaches under us -- check `--screen` and the register sample before
trusting a comparison, or the two images are snapshots of different moments and
every data difference between them is noise.
"""

import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import time

SECTOR = 512


def part_offset(data):
    """Byte offset of the first partition, or 0 for a bare volume."""
    if data[510:512] != b"\x55\xaa":
        return 0
    for i in range(4):
        e = data[446 + 16 * i:446 + 16 * i + 16]
        if e[4]:
            return struct.unpack_from("<I", e, 8)[0] * SECTOR
    return 0


def geometry(path):
    """(cyls, heads, secs) from the volume's own BPB -- see trap 1."""
    with open(path, "rb") as f:
        data = f.read(SECTOR * 64)
    base = part_offset(data)
    secs = struct.unpack_from("<H", data, base + 24)[0]
    heads = struct.unpack_from("<H", data, base + 26)[0]
    total = os.path.getsize(path) // SECTOR
    if not secs or not heads:
        return None
    return max(1, total // (heads * secs)), heads, secs


def rewrite_boot_files(path, launch):
    """Replace CONFIG.SYS/AUTOEXEC.BAT in-place in a COPY -- see trap 2."""
    d = bytearray(open(path, "rb").read())
    base = part_offset(bytes(d))
    spc = d[base + 13]
    res = struct.unpack_from("<H", d, base + 14)[0]
    nfat = d[base + 16]
    rde = struct.unpack_from("<H", d, base + 17)[0]
    spf = struct.unpack_from("<H", d, base + 22)[0]
    root = res + nfat * spf
    rsec = (rde * 32 + SECTOR - 1) // SECTOR
    dstart = root + rsec
    r0 = base + root * SECTOR

    def entry(name):
        for i in range(0, rsec * SECTOR, 32):
            e = d[r0 + i:r0 + i + 32]
            if not e[0] or e[0] == 0xE5:
                continue
            n = e[0:8].decode("latin1").rstrip()
            x = e[8:11].decode("latin1").rstrip()
            if ((n + "." + x) if x else n).upper() == name:
                return r0 + i, struct.unpack_from("<H", e, 26)[0]
        return None, None

    for name, body in (("CONFIG.SYS", b"FILES=20\r\nBUFFERS=15\r\n"),
                       ("AUTOEXEC.BAT",
                        b"@echo off\r\n" + launch.encode() + b"\r\n")):
        ent, clus = entry(name)
        if ent is None:
            continue
        off = base + (dstart + (clus - 2) * spc) * SECTOR
        d[off:off + SECTOR] = body + b"\x1a" * (SECTOR - len(body))
        struct.pack_into("<I", d, ent + 28, len(body))
    open(path, "wb").write(bytes(d))


def state_memory(path):
    """The low megabyte out of a DOSBox-X save state.

    The `Memory` component is a short header followed by raw RAM, and the
    header's length is not documented anywhere we can rely on. Rather than
    hardcode the 136 bytes this build happens to use, the base is FOUND and
    then VALIDATED against the BDA: 0x413 is the conventional-memory size in KB
    and must be 640 (or at least plausible), which no other offset in the
    search window satisfies. A wrong base would hand back plausible-looking
    bytes for the wrong address, which is the failure class this tree keeps
    losing hours to, so it raises instead.
    """
    import zipfile
    z = zipfile.ZipFile(path)
    m = z.read("Memory")
    for k in range(0, 8192):
        if struct.unpack_from("<H", m, k + 0x413)[0] in range(512, 641) \
                and m[k + 0x449] <= 0x13:
            return m[k:k + 0x100000]
    raise SystemExit(f"ref_dos: no BDA signature in {path} -- save state "
                     f"layout changed, do not guess a base")


# DOSBox-X's `Vga` component is `SerializeVga::getBytes()`
# (references/dosbox-x/src/hardware/vga.cpp:2050). Two structures in it are
# self-describing enough to be FOUND AND PROVEN rather than guessed at, and
# those are the only two we need: `vga.dac` (vga_dac.cpp:481, one WRITE_POD of
# the whole VGA_Dac) and, right after the VRAM blob, the `vgapages` struct and
# `vga.mem.memsize` (vga_memory.cpp:3018 then vga.cpp:2131) which pin down
# where the VRAM blob started.
#
# VGA_Dac (include/vga.h:573) on the 64-bit build:
DAC_BITS = 0             # uint8_t bits -- 6 or 8
DAC_PEL_MASK = 1         # uint8_t pel_mask -- 0xff in every DOS mode
DAC_COMBINE = 16         # uint8_t combine[16], after the 8-aligned Bitu
DAC_RGB = 32             # RGBEntry rgb[256]
DAC_XLAT16 = 800         # uint16_t xlat16[256]
DAC_XLAT32 = 1312        # uint32_t xlat32[256]
DAC_SIZE = 2344          # + hidac_counter, reg02, tail padding


def find_dac(v):
    """(offset, palette) of the LIVE DAC in a `Vga` component, or raise.

    THE DEFECT THIS REPLACES. The old reader scanned for the first 768-byte
    window that was all 6-bit and varied, and called that the palette. That
    does not identify the DAC -- it identifies anything DAC-SHAPED, and it
    matched `vga.dac` + 6, i.e. the true table shifted by ten bytes of
    `combine[]`. Ten bytes is 3 entries and one channel, so every index
    rendered as its neighbour's colour with the channels rotated, and Wolf3D's
    grey `BKGDCOLOR` menu background came out saturated blue. A rotation alone
    leaves greys grey; the index shift is what made it visible, and nothing in
    the old scan could tell the two apart -- it could not fail.

    So the table is not merely FOUND, it is PROVEN: DOSBox-X keeps a host-pixel
    cache `xlat32[]` next to `rgb[]` and rebuilds it from `rgb[]` on load
    (vga_dac.cpp:501), so `xlat32[i] == pack(rgb[i] scaled to 8 bits)` holds
    for all 256 entries of the real DAC and for no shifted copy of it, no
    stale fade buffer and no second palette the game happens to keep in VRAM.
    """
    best = None
    for d in range(0, len(v) - DAC_SIZE):
        if v[d + DAC_BITS] not in (6, 8) or v[d + DAC_PEL_MASK] != 0xFF:
            continue
        bits = v[d + DAC_BITS]
        rgb = v[d + DAC_RGB:d + DAC_XLAT16]
        if bits == 6 and max(rgb) > 63:
            continue
        # 6->8 bits by REPLICATION, `(c<<2)|(c>>4)`, which is what DOSBox-X
        # does (vga_dac.cpp) and is not the same as `c*255//63`: 59 becomes
        # 0xef, not 0xee. The cross-check below only closes because this
        # matches DOSBox-X exactly, which is the point -- it is proof the
        # bytes are the DAC and not a lookalike.
        scale = (lambda c: (c << 2) | (c >> 4)) if bits == 6 else (lambda c: c)
        x32 = struct.unpack_from("<256I", v, d + DAC_XLAT32)
        pal = []
        ok = True
        for i in range(256):
            r, g, b = (scale(c) for c in rgb[3 * i:3 * i + 3])
            pal += [r, g, b]
            # RGB or BGR host order -- DOSBox-X packs whichever the host
            # surface uses; both are checked so a byte order does not read as
            # a missing DAC.
            if x32[i] not in ((r << 16) | (g << 8) | b,
                              (b << 16) | (g << 8) | r):
                ok = False
                break
        if not ok or len(set(pal)) < 4:
            continue
        if best is not None:
            raise SystemExit("ref_dos: two structures in the Vga component "
                             f"validate as the DAC ({best[0]} and {d}) -- "
                             "refusing to guess which is live")
        best = (d, pal)
    if best is None:
        raise SystemExit(
            "ref_dos: no VGA DAC in this save state. The rgb[]/xlat32[] "
            "cross-check failed at every offset, which means DOSBox-X's "
            "VGA_Dac layout changed (references/dosbox-x/include/vga.h) -- "
            "fix the offsets, do NOT fall back to a palette-shaped run of "
            "bytes, that is exactly the bug this check exists to catch")
    return best


def find_vram(v, after):
    """Offset of the VRAM blob, from the `vgapages`/`memsize` pair after it.

    `POD_Save_VGA_Memory` writes `memsize` bytes of VRAM and then `vgapages`
    (two Bitu: base page and mask), and `SerializeVga` writes `vga.mem.memsize`
    immediately after that. `vgapages.base` is the A000/B000 page number and
    the size is a power of two, so the pair is identifiable and gives the VRAM
    START as an ARITHMETIC result rather than an assumption.

    The old reader assumed VRAM was simply the last 2 MB of the component. It
    is not -- `vgapages`, `memsize`, `vga.lfb` and the Paradise/Tseng/XGA
    blocks follow it (vga.cpp:2126-2168) -- so every frame was read 1296 bytes
    late, i.e. rolled up by four scanlines.
    """
    for p in range(len(v) - 4, max(after, len(v) - 65536), -1):
        size = struct.unpack_from("<I", v, p)[0]
        if size & (size - 1) or not 0x10000 <= size <= 0x1000000:
            continue
        base, mask = struct.unpack_from("<QQ", v, p - 16)
        if base not in (0xA0, 0xB0, 0xB8) or mask not in (0x7FFF, 0xFFFF,
                                                          0x1FFFF, 0x3FFFF):
            continue
        start = p - 16 - size
        if start >= after:
            return start, size
    raise SystemExit("ref_dos: no vgapages/memsize pair after the DAC in this "
                     "save state -- the VRAM blob cannot be located, and "
                     "guessing its base renders a rolled frame")


def state_screen(path, png):
    """Render mode 13h / Mode Y out of a save state's `Vga` component.

    Wolf3D runs UNCHAINED (Mode Y), so a pixel is not a byte at a linear
    offset: plane = x & 3 and the offset is y*80 + (x >> 2), and DOSBox-X
    stores planar VRAM interleaved four bytes per address. Reading it linearly
    gives a picture that is a quarter as wide, repeated -- which looks like a
    rendering bug rather than a decoding one.

    Both the DAC and the VRAM base are read from where DOSBox-X actually puts
    them and verified there; see `find_dac`/`find_vram`. Neither falls back to
    something plausible, because a reference that renders the wrong colours
    without complaining invalidates every colour judgement made against it.
    """
    import zipfile
    from PIL import Image
    v = zipfile.ZipFile(path).read("Vga")
    dac, pal = find_dac(v)
    vb, size = find_vram(v, dac + DAC_SIZE)
    if size < 320 * 200:
        raise SystemExit(f"ref_dos: {path} VRAM blob is only {size} bytes")
    px = bytearray(320 * 200)
    for y in range(200):
        row = y * 80
        for x in range(320):
            px[y * 320 + x] = v[vb + (row + (x >> 2)) * 4 + (x & 3)]
    im = Image.frombytes("P", (320, 200), bytes(px))
    im.putpalette(pal)
    os.makedirs(os.path.dirname(os.path.abspath(png)) or ".", exist_ok=True)
    im.convert("RGB").save(png)
    return len(set(px))


def run_dosbox(args, img):
    """Boot `img`'s own MS-DOS under DOSBox-X and sample it via save states."""
    work = os.path.abspath(args.work)
    saves = os.path.join(work, "saves")
    shutil.rmtree(saves, ignore_errors=True)
    os.makedirs(saves, exist_ok=True)
    log = os.path.join(work, "dosbox.log")
    conf = os.path.join(work, "dosbox.conf")
    cyls, heads, secs = geometry(img)
    n = max(1, min(args.samples, 100))
    keys = " ".join(k.strip() for k in args.keys.split(",") if k.strip())
    auto = (f"autotype -w {args.settle:.0f} -p {args.key_pace} {keys}\n"
            if keys else "")
    # Every path absolute (trap 2); priority's second field not 'pause'
    # (trap 1); autosave gives us `n` states `--interval` apart.
    open(conf, "w").write(
        "[sdl]\npriority=normal,normal\n"
        f"[dosbox]\nmachine={args.machine}\nmemsize={args.mem}\n"
        f"startbanner=false\nautosave={args.interval:.0f} 1-{n}\n"
        f"[log]\nlogfile={log}\nint10=true\nsblaster=true\n"
        "[autoexec]\n"
        f"imgmount 2 {img} -t hdd -fs none -size 512,{secs},{heads},{cyls}\n"
        f"{auto}boot -l c\n")
    limit = args.settle + args.interval * (n + 1)
    subprocess.run([args.dosbox, "-conf", conf, "-nogui", "-nomenu",
                    "-savedir", saves, "-time-limit", str(int(limit))],
                   env=dict(os.environ, SDL_VIDEODRIVER="dummy"),
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
                   timeout=limit + 90)
    slots = sorted((f for f in os.listdir(saves) if f.endswith(".sav")),
                   key=lambda f: int(f.split(".")[0]))
    if not slots:
        raise SystemExit("ref_dos: DOSBox-X produced no save state -- check "
                         f"{log}; an [autoexec] that never ran leaves it "
                         "ending right after COMMAND.COM starts")
    last = None
    for i, s in enumerate(slots):
        p = os.path.join(saves, s)
        d = state_memory(p)
        last = d
        i8 = struct.unpack_from("<HH", d, 8 * 4)
        i9 = struct.unpack_from("<HH", d, 9 * 4)
        print(f"  slot {s:>7} mode=0x{d[0x449]:02x} "
              f"INT08={i8[1]:04x}:{i8[0]:04x} INT09={i9[1]:04x}:{i9[0]:04x} "
              f"tick={struct.unpack_from('<I', d, 0x46C)[0]}")
        if args.screen:
            out = (args.screen if len(slots) == 1 else
                   f"{os.path.splitext(args.screen)[0]}_{i + 1}.png")
            print(f"      screen -> {out} "
                  f"({state_screen(p, out)} distinct indexes)")
    if args.save:
        path = os.path.abspath(args.save)
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        open(path, "wb").write(last)
        print(f"wrote {args.save} ({len(last)} bytes)")
    return 0


class Monitor:
    """QEMU's human monitor over a UNIX socket.

    Commands are read to the PROMPT, not slept on. The monitor echoes every
    character it receives, so a fixed sleep leaves part of one command's echo in
    the buffer and the next command is delivered with that garbage prepended --
    which QEMU then rejects with something as unhelpful as
    `invalid char 'h' in expression`, having silently mangled a path. Measured:
    that is why pmemsave produced no file while reporting nothing wrong.
    """

    PROMPT = b"(qemu) "

    def __init__(self, sock_path, timeout=20.0):
        deadline = time.time() + timeout
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(sock_path)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.5)
        self._read_to_prompt(5.0)

    def _read_to_prompt(self, timeout):
        buf = b""
        self.s.settimeout(0.5)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = self.s.recv(1 << 20)
            except socket.timeout:
                if buf.endswith(self.PROMPT):
                    break
                continue
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            if buf.endswith(self.PROMPT):
                break
        return buf.decode("latin1")

    def cmd(self, c, wait=20.0):
        self.s.sendall((c + "\n").encode())
        return self._read_to_prompt(wait)


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dsk", required=True)
    p.add_argument("--settle", type=float, default=35.0,
                   help="seconds to let the reference boot and run")
    p.add_argument("--save", help="write its low megabyte here")
    p.add_argument("--screen", help="write a screendump (.ppm) here")
    p.add_argument("--no-hooks", action="store_true",
                   help="neutralise CONFIG.SYS's emulator-specific drivers")
    p.add_argument("--launch", default=None,
                   help="AUTOEXEC line to use with --no-hooks "
                        "(default: the image's own)")
    p.add_argument("--samples", type=int, default=1,
                   help="take this many snapshots, --interval apart. A "
                        "reference that is merely RUNNING is not one that has "
                        "reached the same place; the sequence is what shows "
                        "whether it is progressing or spinning")
    p.add_argument("--interval", type=float, default=5.0)
    p.add_argument("--display", default="vnc=:99",
                   help="QEMU -display/-vnc setting. NOT 'none' by default and "
                        "that is load-bearing: QEMU derives the VGA status bits "
                        "at port 0x3DA from its display refresh, so under "
                        "-display none a title polling for vertical retrace "
                        "spins forever. Measured: WOLF3D sat in "
                        "`IN AL,0x3DA / TEST AL,1 / JNZ` indefinitely, which "
                        "reads exactly like the title hanging")
    p.add_argument("--text", action="store_true",
                   help="also print the 80x25 text buffer at each sample")
    p.add_argument("--keys", default="",
                   help="comma-separated monitor sendkey names pressed before "
                        "sampling, e.g. ret,spc -- a title waiting on a signon "
                        "keypress is indistinguishable from one that has hung")
    p.add_argument("--cmd", action="append", default=[],
                   help="extra QEMU monitor command, run and printed at every "
                        "sample. Repeatable. e.g. --cmd 'x/16i $cs*16+$eip'")
    p.add_argument("--qemu-arg", action="append", default=[],
                   help="extra argument passed straight to qemu-system-i386. "
                        "Repeatable; use --qemu-arg=-M --qemu-arg=isapc")
    p.add_argument("--mem", type=int, default=16)
    p.add_argument("--engine", choices=("qemu", "dosbox"), default="qemu",
                   help="qemu is the default; QEMU CANNOT run Wolfenstein 3D "
                        "(its port 0x3DA flips display-enable and vertical "
                        "retrace together -- see the module docstring), so use "
                        "dosbox for any title that polls for retrace")
    p.add_argument("--dosbox", default="dosbox",
                   help="the DOSBox-X binary for --engine dosbox")
    p.add_argument("--machine", default="svga_s3",
                   help="DOSBox-X [dosbox] machine type")
    p.add_argument("--key-pace", type=float, default=3.0,
                   help="seconds between --keys presses under --engine dosbox")
    p.add_argument("--work", default="build/run/refdos")
    args = p.parse_args()

    os.makedirs(args.work, exist_ok=True)
    img = os.path.join(args.work, "ref.img")
    shutil.copyfile(args.dsk, img)          # never touch the shipped .dsk
    if args.no_hooks:
        launch = args.launch or os.path.basename(args.dsk).upper().replace(
            ".DSK", ".EXE")
        rewrite_boot_files(img, launch)

    geo = geometry(img)
    if geo is None:
        sys.exit("could not read a BPB geometry out of the image")
    cyls, heads, secs = geo
    print(f"geometry from the BPB: cyls={cyls} heads={heads} secs={secs}")

    if args.engine == "dosbox":
        return run_dosbox(args, os.path.abspath(img))

    sock = os.path.join(args.work, "mon.sock")
    if os.path.exists(sock):
        os.unlink(sock)
    proc = subprocess.Popen(
        ["qemu-system-i386",
         "-drive", f"file={img},format=raw,if=none,id=d0,snapshot=on",
         "-device", f"ide-hd,drive=d0,cyls={cyls},heads={heads},secs={secs}",
         "-m", str(args.mem)]
        + (["-vnc", args.display[4:]] if args.display.startswith("vnc=")
           else ["-display", args.display])
        + [
         "-monitor", f"unix:{sock},server,nowait"] + list(args.qemu_arg),
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        print(f"letting the reference run {args.settle:.0f}s...")
        time.sleep(args.settle)
        mon = Monitor(sock)
        for k in [x for x in args.keys.split(",") if x.strip()]:
            print(f"  sendkey {k.strip()}")
            mon.cmd(f"sendkey {k.strip()}")
        for n in range(args.samples):
            if n:
                time.sleep(args.interval)
            regs = mon.cmd("info registers")
            bits = {}
            for line in regs.splitlines():
                for k in ("EIP", "CS ", "DS ", "SS ", "ESP"):
                    if line.startswith(k):
                        bits[k.strip()] = line.split("=")[1].split()[0]
            probe = os.path.join(args.work, "probe.bin")
            if os.path.exists(probe):
                os.unlink(probe)
            mon.cmd(f'pmemsave 0 0x100000 "{os.path.abspath(probe)}"')
            for _ in range(20):          # the monitor acks before it finishes
                if os.path.exists(probe) and os.path.getsize(probe) >= 0x100000:
                    break
                time.sleep(0.5)
            mode = pixels = "?"
            if os.path.exists(probe) and os.path.getsize(probe) >= 0x100000:
                d = open(probe, "rb").read()
                # 0040:0049 is the BIOS video mode, and A0000 having more than
                # one distinct byte is the difference between "a graphics mode
                # is set" and "a graphics mode is set and something drew".
                mode = f"0x{d[0x449]:02x}"
                pixels = len(set(d[0xA0000:0xA0000 + 64000]))
                if args.text:
                    t = d[0xB8000:0xB8000 + 80 * 25 * 2]
                    for r in range(25):
                        line = "".join(
                            chr(t[(r * 80 + c) * 2])
                            if 32 <= t[(r * 80 + c) * 2] < 127 else " "
                            for c in range(80)).rstrip()
                        if line:
                            print(f"      |{line}")
            for c in args.cmd:
                out = mon.cmd(c)
                for line in out.splitlines():
                    line = line.rstrip()
                    if line and not line.startswith("(qemu)") and line != c:
                        print(f"      {line}")
            print(f"  t+{n * args.interval:5.0f}s CS:IP={bits.get('CS')}:"
                  f"{bits.get('EIP')} DS={bits.get('DS')} SS:SP="
                  f"{bits.get('SS')}:{bits.get('ESP')} "
                  f"mode={mode} A000-distinct={pixels}")
        # VERIFY the file appeared. The monitor acknowledges a command it has
        # not finished, so a terminate() straight after silently produces no
        # file at all and the caller diffs against a stale image -- which is
        # worse than an error, because the diff still "works".
        # The path is QUOTED. QEMU's monitor parses the size argument as an
        # arithmetic EXPRESSION and `/` is DIVISION, so an unquoted absolute
        # path makes `0x100000 /home/...` a division by `home` and the command
        # dies with `invalid char 'h' in expression` -- having written nothing.
        # Relative paths happen to work, which is what hid this.
        for flag, cmd, want in ((args.save, "pmemsave 0 0x100000", 0x100000),
                                (args.screen, "screendump", 0)):
            if not flag:
                continue
            path = os.path.abspath(flag)
            os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
            if os.path.exists(path):
                os.unlink(path)
            mon.cmd(f'{cmd} "{path}"')
            for _ in range(20):
                if os.path.exists(path) and os.path.getsize(path) >= want:
                    break
                time.sleep(0.5)
            else:
                raise SystemExit(f"ref_dos: {cmd} produced no usable {path}")
            print(f"wrote {flag} ({os.path.getsize(path)} bytes)")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
