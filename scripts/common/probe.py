"""
Sample what a target is actually doing: PC, watched globals, and pointer
dereferences, over time.

This is the "is it progressing, and if not where is it stuck" instrument. It
answers questions that a pass/fail harness cannot:

  * Is the CPU in our ITCM interpreter, in firmware, or in a fault handler?
  * Is the guest advancing (watch EAX count down) or wedged?
  * Did an allocation return a plausible pointer, or garbage that a non-NULL
    check happily accepted?

It found a real bug on first use: two DTCM allocations returning the same
address, which a `if (!ptr) fail` check reported as success.

Works against any Target, so gwemu and silicon are the same call.
"""

import time

from .elf import get_symbols, nearest_symbol

# Rough address-space map, for labelling PC without a symbol lookup.
REGIONS = (
    (0x00000000, 0x00010000, "ITCM"),
    (0x08000000, 0x08200000, "INTFLASH"),
    (0x20000000, 0x20020000, "DTCM"),
    (0x24000000, 0x24100000, "AXI"),
    (0x30000000, 0x30020000, "AHB"),
    (0x90000000, 0x94000000, "EXTFLASH"),
)


def region_of(addr):
    for lo, hi, name in REGIONS:
        if lo <= addr < hi:
            return name
    return "?"


def describe_pc(pc, symbol_maps):
    """'REGION sym+0xoff'.

    ITCM used to be reported as a bare "(interpreter)" because it held only the
    fetch loop. It now holds several hundred named handlers, and which one is
    running is the whole question when a guest misbehaves -- so resolve it like
    anywhere else.
    """
    region = region_of(pc)
    merged = {}
    for m in symbol_maps:
        merged.update(m)
    if not merged:
        return region
    return f"{region} {nearest_symbol(merged, pc)}"


# ARMv7-M fault status registers. Reading these is how you stop guessing why
# execution stopped: VECTACTIVE names the active exception, CFSR the cause.
SCB = {"ICSR": 0xE000ED04, "CFSR": 0xE000ED28, "HFSR": 0xE000ED2C,
       "MMFAR": 0xE000ED34, "BFAR": 0xE000ED38}
VECTORS = {0: "thread", 2: "NMI", 3: "HardFault", 4: "MemManage",
           5: "BusFault", 6: "UsageFault", 11: "SVCall", 14: "PendSV",
           15: "SysTick"}


def read_faults(backend, out=print, symbol_maps=()):
    """Report the CPU's exception/fault state, including WHERE it faulted.

    CFSR and BFAR say what went wrong and to which address; neither says which
    instruction did it, and the sampled PC is inside the fault handler by then.
    ARMv7-M stacks R0-R3, R12, LR, PC and xPSR on entry, so the faulting PC is
    six words up the stack the exception came from -- EXC_RETURN bit 2 picks
    MSP or PSP. Resolving that PC against the ELF is usually the whole answer.
    """
    from .faults import decode_cfsr
    vals = {k: backend.read_uint32(a) for k, a in SCB.items()}
    active = vals["ICSR"] & 0x1FF
    out(f"  ICSR  0x{vals['ICSR']:08x}  VECTACTIVE={active} "
        f"({VECTORS.get(active, 'IRQ' + str(active - 16))})")
    out(f"  CFSR  0x{vals['CFSR']:08x}  [{decode_cfsr(vals['CFSR'])}]")
    out(f"  HFSR  0x{vals['HFSR']:08x}")
    out(f"  BFAR  0x{vals['BFAR']:08x}   MMFAR 0x{vals['MMFAR']:08x}")
    if not active:
        return vals
    try:
        lr = backend.read_register("lr")
        sp_name = "psp" if (lr & 0x4) else "msp"
        try:
            sp = backend.read_register(sp_name)
        except Exception:
            sp = backend.read_register("sp")
        frame = [backend.read_uint32(sp + 4 * i) for i in range(8)]
    except Exception as e:                       # nothing to unwind
        out(f"  (could not read the stacked frame: {e})")
        return vals
    names = ["R0", "R1", "R2", "R3", "R12", "LR", "PC", "xPSR"]
    vals["frame"] = dict(zip(names, frame))
    out(f"  stacked frame via {sp_name}=0x{sp:08x}:")
    for n, v in zip(names, frame):
        note = ""
        if n in ("PC", "LR"):
            note = "  " + describe_pc(v & ~1, symbol_maps or ())
        out(f"    {n:<4} 0x{v:08x}{note}")
    return vals


def parse_watch(spec):
    """'a,b,c' -> ['a','b','c']"""
    return [s.strip() for s in spec.split(",") if s.strip()]


def parse_deref(spec):
    """'cpu_state_ptr+8=EAX' -> ('cpu_state_ptr', 8, 'EAX')"""
    label = None
    if "=" in spec:
        spec, label = spec.split("=", 1)
    off = 0
    if "+" in spec:
        spec, off_s = spec.split("+", 1)
        off = int(off_s, 0)
    sym = spec.strip()
    reject_guest_ram(sym)
    return sym, off, (label or f"*{sym}+{off:#x}")


GUEST_RAM_IS_NOT_LINEAR = (
    "*guest_ram+OFF no longer names guest linear memory: since demand paging "
    "landed, guest_ram is a FRAME POOL, so this reads an arbitrary resident "
    "page and returns plausible-looking wrong bytes rather than failing. "
    "Use the fold-table path instead -- probe.py --guest / --guest-save, or "
    "watch_memory.py --addr guest:0xLIN."
)


def reject_guest_ram(sym):
    """Refuse the old spelling LOUDLY. Silently returning unrelated pages is
    the exact failure class this project keeps losing hours to, and the old
    form appears in TOOLING.md and in several harnesses."""
    if sym.lstrip("*").split("+", 1)[0].strip() == "guest_ram":
        raise SystemExit("probe: " + GUEST_RAM_IS_NOT_LINEAR)


def parse_dump(spec):
    """'x86_cs_log:8x2=CSLOG' -> ('x86_cs_log', 8, 2, 'CSLOG')

    count:stride reads the symbol's OWN storage, which is what an array needs.
    --deref cannot: it follows the first word as a pointer. `x2` groups the
    words into pairs so a log of (a, b) records prints as pairs.
    """
    label = None
    if "=" in spec:
        spec, label = spec.split("=", 1)
    group = 1
    if "x" in spec.split(":")[-1]:
        head, g = spec.rsplit("x", 1)
        spec, group = head, int(g, 0)
    sym, _, cnt = spec.partition(":")
    sym = sym.strip()
    reject_guest_ram(sym)
    return sym, int(cnt or 8, 0), group, (label or sym)


def parse_bytes(spec):
    """'*vga_dac+0:768=DAC' -> ('*vga_dac+0', 768, 'DAC')

    A HOST-address byte range, in the same three shapes `_dump_base` accepts:
    `name`, `0xADDR`, or `*name+OFF`. This is the companion to --dump, which
    reads WORDS of a symbol's own storage: a `uint8_t *` palette, a ring of
    bytes or a struct field is not words, and a symbol that is itself a POINTER
    (`static uint8_t *vga_dac`) cannot be reached by --dump at all -- --dump
    would hexdump the four bytes of the pointer. --deref follows a pointer but
    reads exactly one uint32.

    NOT for guest memory: use --guest, which walks the fold table.
    """
    label = None
    if "=" in spec:
        spec, label = spec.split("=", 1)
    sym, _, cnt = spec.rpartition(":")
    if not sym:                       # no ':' -> the whole thing is the symbol
        sym, cnt = cnt, ""
    sym = sym.strip()
    reject_guest_ram(sym)
    return sym, int(cnt or 64, 0), (label or sym)


def read_bytes(target, syms, sym, nbytes):
    """Bytes at a host address spec, or None if the symbol is not in the ELF."""
    base = _dump_base(sym, syms, target.backend)
    if base is None:
        return None, None
    return base, target.backend.read_memory(base, nbytes)


def dump_bytes(target, syms, specs, out=print, group=1):
    """Hexdump host byte ranges. `group` inserts a gap every G bytes, which is
    what makes a phase error in a strided stream (the DAC's r,g,b triples)
    readable rather than something you count on screen."""
    for sym, nbytes, label in specs:
        base, data = read_bytes(target, syms, sym, nbytes)
        if data is None:
            out(f"  {label}: symbol not found")
            continue
        out(f"  {label} @0x{base:08x} ({nbytes} bytes):")
        per = 16 if group <= 1 else group * max(1, 16 // group)
        for i in range(0, len(data), per):
            row = data[i:i + per]
            if group > 1:
                cells = [" ".join(f"{b:02x}" for b in row[j:j + group])
                         for j in range(0, len(row), group)]
                hexs = "  ".join(cells)
            else:
                hexs = " ".join(f"{b:02x}" for b in row)
            txt = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
            out(f"    +{i:04x}  {hexs:<50}  |{txt}|")


def save_bytes(target, syms, specs, out=print):
    """Write host byte ranges to files, for byte-comparing against a reference."""
    for sym, nbytes, path in specs:
        base, data = read_bytes(target, syms, sym, nbytes)
        if data is None:
            out(f"  {sym}: symbol not found")
            continue
        with open(path, "wb") as f:
            f.write(data)
        out(f"  wrote {len(data)} bytes at 0x{base:08x} ({sym}) to {path}")


def dump_symbol_name(sym):
    """The ELF symbol a dump spec depends on, or None for a literal address."""
    sym = sym.lstrip("*")
    sym = sym.split("+", 1)[0].strip()
    if sym.startswith("0x") or sym.startswith("0X"):
        return None
    return sym


def _dump_base(sym, syms, backend=None):
    """Resolve a dump target to an address.

    Three forms, because the interesting memory comes in three shapes:
      `name`          the symbol's own storage (an array)
      `0xADDR`        memory with no symbol (a peripheral, a stack frame)
      `*name+OFF`     through a runtime pointer, which is the only way to
                      reach guest RAM: `guest_ram` is a pointer the core is
                      handed at startup, so its value is not in the ELF.
    """
    reject_guest_ram(sym)
    deref = sym.startswith("*")
    sym = sym.lstrip("*")
    off = 0
    if "+" in sym:
        sym, off_s = sym.split("+", 1)
        off = int(off_s, 0)
    sym = sym.strip()
    if sym.startswith("0x") or sym.startswith("0X"):
        base = int(sym, 16)
    else:
        base = syms.get(sym)
        if base is None:
            return None
    if deref:
        if backend is None:
            return None
        base = backend.read_uint32(base)
    return base + off


def dump_arrays(target, syms, dumps, out=print, symbol_maps=()):
    """Print each requested array as words, grouped.

    Words that land inside a known function are annotated with it. Half of what
    these logs hold is code addresses -- a saved LR, a handler entry -- and
    resolving them by hand against nm output is slow and easy to get wrong.
    """
    maps = symbol_maps or [syms]
    for sym, count, group, label in dumps:
        base = _dump_base(sym, syms, target.backend)
        if base is None:
            out(f"  {label}: symbol not found")
            continue
        words = [target.backend.read_uint32(base + 4 * i) for i in range(count)]
        out(f"  {label} @0x{base:08x}:")
        for i in range(0, count, group):
            chunk = words[i:i + group]
            cells = []
            for w in chunk:
                note = describe_pc(w, maps) if w > 0x100 else ""
                # Only keep it when it actually resolved to a name.
                if note and "+" not in note and not note.endswith(")"):
                    note = ""
                cells.append(f"0x{w:08x}" + (f" <{note}>" if note else f" ({w})"))
            out(f"    [{i // group}] " + "  ".join(cells))


DOS_FUNCS = {
    0x0F: "FCB open", 0x10: "FCB close", 0x11: "FCB findfirst",
    0x12: "FCB findnext", 0x13: "FCB delete", 0x14: "FCB seqread",
    0x15: "FCB seqwrite", 0x16: "FCB create", 0x17: "FCB rename",
    0x1A: "set DTA", 0x21: "FCB randread", 0x22: "FCB randwrite",
    0x23: "FCB filesize", 0x27: "FCB blkread", 0x28: "FCB blkwrite",
    0x29: "parse filename", 0x2F: "get DTA",
    0x39: "mkdir", 0x3A: "rmdir", 0x3B: "chdir", 0x3C: "create",
    0x3D: "open", 0x3E: "close", 0x3F: "read", 0x40: "write",
    0x41: "delete", 0x42: "lseek", 0x43: "attrib", 0x44: "ioctl",
    0x45: "dup", 0x46: "dup2", 0x47: "getcwd", 0x4E: "findfirst",
    0x4F: "findnext", 0x56: "rename", 0x57: "file date",
    0x5A: "mktemp", 0x5B: "create new", 0x68: "commit", 0x6C: "extopen",
    0x48: "mem alloc", 0x49: "mem free", 0x4A: "mem resize",
}

# The memory calls share the file log's slots but not its field meanings:
# slot+3 is (ES << 16) | DX for these, because ES is the BLOCK for 49h/4Ah and
# a resize loop's question is "which block, to how many paragraphs".
DOS_MEM_FUNCS = (0x48, 0x49, 0x4A)

DOS_ERRS = {
    1: "invalid function", 2: "file not found", 3: "path not found",
    4: "too many open files", 5: "access denied", 6: "invalid handle",
    0x0F: "invalid drive", 0x12: "no more files", 0x19: "seek error",
}


def dump_dos_log(target, syms, out=print):
    """Decode the INT 21h file-call trace into one readable line per call.

    Four words per slot (`x86_dos.c`): (func<<8)|AL, (CF<<16)|AX, BX, CX:DX.
    Raw, this is 384 hex words and nobody reads it; decoded, the sequence
    "findfirst *.WL1 -> open MAPHEAD.WL1 -> handle 5 -> lseek BX=5" is legible
    at a glance, which is the only reason the extra fields were added.
    """
    need = ("x86_dos_file_log", "x86_dos_file_log_idx", "x86_dos_file_calls")
    if any(n not in syms for n in need):
        out("  dos log: symbols not in this build")
        return
    b = target.backend
    n = b.read_uint32(syms["x86_dos_file_log_idx"])
    total = b.read_uint32(syms["x86_dos_file_calls"])
    base = syms["x86_dos_file_log"]
    out(f"  INT 21h file calls: {total} total, {n} recorded "
        f"(log SATURATES; write x86_dos_file_log_idx=0 to re-arm)")
    for i in range(n):
        w = [b.read_uint32(base + 16 * i + 4 * k) for k in range(4)]
        func, al = (w[0] >> 8) & 0xFF, w[0] & 0xFF
        cf, ax = (w[1] >> 16) & 1, w[1] & 0xFFFF
        bx, cxdx = w[2], ((w[3] >> 16) & 0xFFFF) << 16 | (w[3] & 0xFFFF)
        name = DOS_FUNCS.get(func, "?")
        res = (f"CF=1 AX={ax:#06x} {DOS_ERRS.get(ax, '')}".rstrip()
               if cf else f"ok AX={ax:#06x}")
        if func in DOS_MEM_FUNCS:
            args = f"BX={bx:#06x} paras ES={(w[3] >> 16) & 0xFFFF:#06x}"
        else:
            args = f"BX={bx:#06x} CX:DX={cxdx:#010x}"
        out(f"    [{i:3d}] {func:02X}h/{al:02X} {name:<14} {args}  -> {res}")
    if "x86_dos_path_log" in syms:
        pbase = syms["x86_dos_path_log"]
        out("  paths (ring of 16, oldest position unknown):")
        for i in range(16):
            raw = b.read_memory(pbase + 32 * i, 32)
            s = raw.split(b"\0")[0].decode("latin-1")
            if s:
                out(f"    [{i:2d}] {s}")


def dump_io_log(target, syms, out=print):
    """Decode the coalesced port-I/O ring: one line per (dir, port) run.

    A guest's hardware conversation is a SHAPE -- which register files it
    programs, in what order -- and a raw ring drowns in the 768 consecutive
    0x3C9 writes of one palette upload. The on-target side coalesces runs, so
    this prints "OUT 0x3C9 x768" as a single line next to the CRTC and
    Sequencer writes around it, which is the part that carries information.
    """
    if "x86_io_log" not in syms:
        out("  io log: symbols not in this build")
        return
    b = target.backend
    n = b.read_uint32(syms["x86_io_log_idx"])
    total = b.read_uint32(syms["x86_io_log_total"])
    base = syms["x86_io_log"]
    slots = 32
    out(f"  port accesses: {total} total, last {min(n, slots)} runs "
        f"(oldest first)")
    first = max(0, n - slots)
    for i in range(first, n):
        s = i % slots
        head = b.read_uint32(base + 8 * s)
        val = b.read_uint32(base + 8 * s + 4)
        d = "IN " if (head >> 28) == 1 else "OUT"
        port, cnt = (head >> 12) & 0xFFFF, head & 0xFFF
        out(f"    [{i:4d}] {d} {port:#06x} x{cnt:<4d} last={val:#04x}")


GUEST_PAGE_SIZE = 4096
FOLD_INVALID = 0x80000000


def parse_guest(spec):
    """'0x10E90:64=CSIP' -> (0x10E90, 64, 'CSIP')

    A guest LINEAR address and a byte count. Not a symbol: guest memory has no
    ELF symbols, and since demand paging landed it has no flat host image
    either -- `*guest_ram+OFF` reads a frame POOL, not linear memory, and comes
    back as zeros or as an unrelated page. The fold table is the only correct
    translation and this is the only tool that walks it.
    """
    label = None
    if "=" in spec:
        spec, label = spec.split("=", 1)
    addr_s, _, len_s = spec.partition(":")
    addr = int(addr_s, 0)
    return addr, int(len_s or 64, 0), (label or f"guest:{addr:#x}")


def guest_read(backend, syms, lin, nbytes):
    """Read guest LINEAR memory through the fold table. Returns bytes, with
    0xFF for any page that is unmapped (FOLD_INVALID) -- which is exactly what
    the guest would fetch there, so a dump never silently invents zeros."""
    pt = syms.get("guest_page_table_ptr")
    if pt is None:
        raise KeyError("guest_page_table_ptr not in this ELF")
    base = backend.read_uint32(pt)
    out = bytearray()
    while len(out) < nbytes:
        a = lin + len(out)
        page, off = a // GUEST_PAGE_SIZE, a % GUEST_PAGE_SIZE
        n = min(GUEST_PAGE_SIZE - off, nbytes - len(out))
        k = backend.read_uint32(base + 4 * page)
        if k & FOLD_INVALID:
            out += b"\xff" * n
        else:
            out += backend.read_memory((a + k) & 0xFFFFFFFF, n)
    return bytes(out)


def save_guest(target, syms, specs, out=print):
    """Write guest linear ranges to files, for diffing against a reference.

    A hexdump is the wrong shape once the range is larger than a screen: the
    question then is "where does our image stop matching a known-good one",
    and that is a byte compare, not a read.
    """
    for lin, nbytes, path in specs:
        data = guest_read(target.backend, syms, lin, nbytes)
        with open(path, "wb") as f:
            f.write(data)
        out(f"  wrote {len(data)} bytes of guest linear "
            f"{lin:#07x}..{lin + nbytes:#07x} to {path}")


def dump_guest(target, syms, specs, out=print):
    for lin, nbytes, label in specs:
        try:
            data = guest_read(target.backend, syms, lin, nbytes)
        except KeyError as e:
            out(f"  {label}: {e}")
            continue
        out(f"  {label} guest linear {lin:#07x} ({lin >> 4:04x}:{lin & 15:04x}+):")
        for i in range(0, len(data), 16):
            row = data[i:i + 16]
            hexs = " ".join(f"{b:02x}" for b in row)
            txt = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
            out(f"    {lin + i:06x}  {hexs:<47}  |{txt}|")


def sample(target, syms, watches=(), derefs=(), count=10, interval=1.0,
           symbol_maps=(), out=print, on_sample=None):
    """Halt/read/resume `count` times. Returns the rows it printed.

    watches: symbol names read as uint32.
    derefs:  (symbol, offset, label) -- reads uint32 at (*symbol)+offset, which
             is how you follow a runtime-allocated pointer into its struct.
    """
    rows = []
    for i in range(count):
        time.sleep(interval)
        target.backend.halt()
        row = {"t": round(i * interval, 2)}
        pc = target.backend.read_register("pc")
        row["pc"] = pc
        row["where"] = describe_pc(pc, symbol_maps or [syms])

        for name in watches:
            row[name] = (target.backend.read_uint32(syms[name])
                         if name in syms else None)

        for sym, off, label in derefs:
            val = None
            if sym in syms:
                ptr = target.backend.read_uint32(syms[sym])
                row[f"{label}@"] = ptr
                if region_of(ptr) != "?":
                    val = target.backend.read_uint32(ptr + off)
            row[label] = val

        # While still halted: anything reading target memory (the log
        # drainer) must run before the resume.
        if on_sample is not None:
            on_sample(target.backend)

        target.backend.resume()
        rows.append(row)

        parts = [f"t+{row['t']:5.1f}s", f"PC=0x{pc:08x}", f"{row['where']:<34}"]
        for name in watches:
            parts.append(f"{name}={row[name]}")
        for _, _, label in derefs:
            ptr = row.get(f"{label}@")
            parts.append(f"{label}={row[label]}"
                         + (f" (@0x{ptr:08x})" if ptr is not None else ""))
        out("  " + " ".join(parts))
    return rows


def wait_for_value(backend, addr, want, timeout, poll=0.25):
    """Wait for an EXACT value. 'non-zero' is not a usable signal on RAM that
    holds firmware data until our code loads over it."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if backend.read_uint32(addr) == want:
            return True
        time.sleep(poll)
    return False


def load_symbol_maps(*elfs):
    """Merge symbol tables; later ELFs win on collisions."""
    merged = {}
    maps = []
    for e in elfs:
        if e:
            m = get_symbols(e)
            maps.append(m)
            merged.update(m)
    return merged, maps
