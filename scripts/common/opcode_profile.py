"""
Read and present the on-target per-opcode DWT cycle profile.

The instrumentation lives in the fetch/dispatch tail of cpu_exec.S and is
compiled in only with X86_PROFILE=1; x86_profile.h owns the block layout. This
module is the host side: locate the block, reset it, read it back, resolve each
slot to a handler symbol, and render it as a table / JSON / CSV.

Two things it deliberately refuses to hide:

  * **Attribution is approximate.** A delta charged to opcode N is measured
    between two dispatches, so it carries the tail of N-1 and whatever cache and
    AXI state N inherited. The M7 is superscalar with an out-of-order memory
    system; there is no way to fence that out without changing what is being
    measured. Rank hogs with it. Do not quote a per-opcode cycle count as truth.
  * **gwemu cycles are meaningless.** gwemu does not model the pipeline
    (docs/06), so a gwemu profile proves the plumbing works and nothing else.
    Every rendering here says so when the target is gwemu.

The measurement overhead is calibrated ON the target by x86_profile_calibrate
and stored in the block; the corrected column subtracts it explicitly and both
columns are always printed. Nothing applies a fudge factor.

JSON follows the shape of tests/asm_baseline.json (a "_comment", the run
metadata, then a symbol/slot-keyed map) so cycle profiles diff the same way the
static per-handler counts already do.
"""

import csv
import json
import sys
import time

from .elf import nearest_symbol

HDR_WORDS = 4
ENTRY_WORDS = 4
SLOTS = 513
HOST_SLOT = 512
BLOCK_BYTES = 4 * (HDR_WORDS + SLOTS * ENTRY_WORDS)
MAGIC = 0x50524F46            # 'PROF'
DWT_CYCCNT = 0xE0001004

# One-byte opcode mnemonics. Group entries (80-83, C0-C1, D0-D3, F6-F7, FE-FF)
# are named by their group because the profiler buckets by opcode byte, not by
# the ModRM /r field -- the dispatch table has one entry for all eight.
_ONE_BYTE = {
    0x00: "ADD Eb,Gb", 0x01: "ADD Ev,Gv", 0x02: "ADD Gb,Eb", 0x03: "ADD Gv,Ev",
    0x04: "ADD AL,Ib", 0x05: "ADD eAX,Iv", 0x06: "PUSH ES", 0x07: "POP ES",
    0x08: "OR Eb,Gb", 0x09: "OR Ev,Gv", 0x0A: "OR Gb,Eb", 0x0B: "OR Gv,Ev",
    0x0C: "OR AL,Ib", 0x0D: "OR eAX,Iv", 0x0E: "PUSH CS", 0x0F: "0F escape",
    0x10: "ADC Eb,Gb", 0x11: "ADC Ev,Gv", 0x12: "ADC Gb,Eb", 0x13: "ADC Gv,Ev",
    0x14: "ADC AL,Ib", 0x15: "ADC eAX,Iv", 0x16: "PUSH SS", 0x17: "POP SS",
    0x18: "SBB Eb,Gb", 0x19: "SBB Ev,Gv", 0x1A: "SBB Gb,Eb", 0x1B: "SBB Gv,Ev",
    0x1C: "SBB AL,Ib", 0x1D: "SBB eAX,Iv", 0x1E: "PUSH DS", 0x1F: "POP DS",
    0x20: "AND Eb,Gb", 0x21: "AND Ev,Gv", 0x22: "AND Gb,Eb", 0x23: "AND Gv,Ev",
    0x24: "AND AL,Ib", 0x25: "AND eAX,Iv", 0x26: "ES:", 0x27: "DAA",
    0x28: "SUB Eb,Gb", 0x29: "SUB Ev,Gv", 0x2A: "SUB Gb,Eb", 0x2B: "SUB Gv,Ev",
    0x2C: "SUB AL,Ib", 0x2D: "SUB eAX,Iv", 0x2E: "CS:", 0x2F: "DAS",
    0x30: "XOR Eb,Gb", 0x31: "XOR Ev,Gv", 0x32: "XOR Gb,Eb", 0x33: "XOR Gv,Ev",
    0x34: "XOR AL,Ib", 0x35: "XOR eAX,Iv", 0x36: "SS:", 0x37: "AAA",
    0x38: "CMP Eb,Gb", 0x39: "CMP Ev,Gv", 0x3A: "CMP Gb,Eb", 0x3B: "CMP Gv,Ev",
    0x3C: "CMP AL,Ib", 0x3D: "CMP eAX,Iv", 0x3E: "DS:", 0x3F: "AAS",
    0x60: "PUSHA", 0x61: "POPA", 0x62: "BOUND", 0x63: "ARPL",
    0x64: "FS:", 0x65: "GS:", 0x66: "opsize", 0x67: "addrsize",
    0x68: "PUSH Iv", 0x69: "IMUL Gv,Ev,Iv", 0x6A: "PUSH Ib",
    0x6B: "IMUL Gv,Ev,Ib", 0x6C: "INSB", 0x6D: "INSW", 0x6E: "OUTSB",
    0x6F: "OUTSW",
    0x80: "GRP1 Eb,Ib", 0x81: "GRP1 Ev,Iv", 0x82: "GRP1 Eb,Ib", 0x83: "GRP1 Ev,Ib",
    0x84: "TEST Eb,Gb", 0x85: "TEST Ev,Gv", 0x86: "XCHG Eb,Gb",
    0x87: "XCHG Ev,Gv", 0x88: "MOV Eb,Gb", 0x89: "MOV Ev,Gv",
    0x8A: "MOV Gb,Eb", 0x8B: "MOV Gv,Ev", 0x8C: "MOV Ew,Sw", 0x8D: "LEA",
    0x8E: "MOV Sw,Ew", 0x8F: "POP Ev",
    0x90: "NOP", 0x98: "CBW", 0x99: "CWD", 0x9A: "CALL far", 0x9B: "WAIT",
    0x9C: "PUSHF", 0x9D: "POPF", 0x9E: "SAHF", 0x9F: "LAHF",
    0xA0: "MOV AL,Ob", 0xA1: "MOV eAX,Ov", 0xA2: "MOV Ob,AL",
    0xA3: "MOV Ov,eAX", 0xA4: "MOVSB", 0xA5: "MOVSW", 0xA6: "CMPSB",
    0xA7: "CMPSW", 0xA8: "TEST AL,Ib", 0xA9: "TEST eAX,Iv", 0xAA: "STOSB",
    0xAB: "STOSW", 0xAC: "LODSB", 0xAD: "LODSW", 0xAE: "SCASB", 0xAF: "SCASW",
    0xC0: "GRP2 Eb,Ib", 0xC1: "GRP2 Ev,Ib", 0xC2: "RET Iw", 0xC3: "RET",
    0xC4: "LES", 0xC5: "LDS", 0xC6: "MOV Eb,Ib", 0xC7: "MOV Ev,Iv",
    0xC8: "ENTER", 0xC9: "LEAVE", 0xCA: "RETF Iw", 0xCB: "RETF",
    0xCC: "INT3", 0xCD: "INT Ib", 0xCE: "INTO", 0xCF: "IRET",
    0xD0: "GRP2 Eb,1", 0xD1: "GRP2 Ev,1", 0xD2: "GRP2 Eb,CL",
    0xD3: "GRP2 Ev,CL", 0xD4: "AAM", 0xD5: "AAD", 0xD6: "SALC", 0xD7: "XLAT",
    0xE0: "LOOPNZ", 0xE1: "LOOPZ", 0xE2: "LOOP", 0xE3: "JCXZ",
    0xE4: "IN AL,Ib", 0xE5: "IN eAX,Ib", 0xE6: "OUT Ib,AL", 0xE7: "OUT Ib,eAX",
    0xE8: "CALL Jv", 0xE9: "JMP Jv", 0xEA: "JMP far", 0xEB: "JMP Jb",
    0xEC: "IN AL,DX", 0xED: "IN eAX,DX", 0xEE: "OUT DX,AL", 0xEF: "OUT DX,eAX",
    0xF0: "LOCK", 0xF2: "REPNZ", 0xF3: "REP", 0xF4: "HLT", 0xF5: "CMC",
    0xF6: "GRP3 Eb", 0xF7: "GRP3 Ev", 0xF8: "CLC", 0xF9: "STC", 0xFA: "CLI",
    0xFB: "STI", 0xFC: "CLD", 0xFD: "STD", 0xFE: "GRP4 Eb", 0xFF: "GRP5 Ev",
}
_CC = ["O", "NO", "B", "NB", "Z", "NZ", "BE", "NBE",
       "S", "NS", "P", "NP", "L", "NL", "LE", "NLE"]
_R16 = ["AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI"]

_ONE_BYTE.update({0x40 + i: f"INC {_R16[i]}" for i in range(8)})
_ONE_BYTE.update({0x48 + i: f"DEC {_R16[i]}" for i in range(8)})
_ONE_BYTE.update({0x50 + i: f"PUSH {_R16[i]}" for i in range(8)})
_ONE_BYTE.update({0x58 + i: f"POP {_R16[i]}" for i in range(8)})
_ONE_BYTE.update({0x70 + i: f"J{_CC[i]} Jb" for i in range(16)})
_ONE_BYTE.update({0x91 + i: f"XCHG {_R16[i + 1]},AX" for i in range(7)})
_ONE_BYTE.update({0xB0 + i: f"MOV {r}L,Ib" for i, r in enumerate("ACDB")})
_ONE_BYTE.update({0xB4 + i: f"MOV {r}H,Ib" for i, r in enumerate("ACDB")})
_ONE_BYTE.update({0xB8 + i: f"MOV {_R16[i]},Iv" for i in range(8)})
_ONE_BYTE.update({0xD8 + i: f"ESC {i} (x87)" for i in range(8)})

_TWO_BYTE = {
    0x00: "GRP6", 0x01: "GRP7", 0x02: "LAR", 0x03: "LSL", 0x06: "CLTS",
    0x0B: "UD2 / HLE hypercall",
    0x20: "MOV Rd,Cd", 0x22: "MOV Cd,Rd", 0x23: "MOV Dd,Rd",
    0xA0: "PUSH FS", 0xA1: "POP FS", 0xA2: "CPUID", 0xA3: "BT Ev,Gv",
    0xA4: "SHLD Ib", 0xA5: "SHLD CL", 0xA8: "PUSH GS", 0xA9: "POP GS",
    0xAB: "BTS", 0xAC: "SHRD Ib", 0xAD: "SHRD CL", 0xAF: "IMUL Gv,Ev",
    0xB2: "LSS", 0xB3: "BTR", 0xB4: "LFS", 0xB5: "LGS", 0xB6: "MOVZX Gv,Eb",
    0xB7: "MOVZX Gv,Ew", 0xBB: "BTC", 0xBC: "BSF", 0xBD: "BSR",
    0xBE: "MOVSX Gv,Eb", 0xBF: "MOVSX Gv,Ew", 0xC8: "BSWAP",
}
_TWO_BYTE.update({0x80 + i: f"J{_CC[i]} Jv" for i in range(16)})
_TWO_BYTE.update({0x90 + i: f"SET{_CC[i]}" for i in range(16)})


def slot_name(slot):
    """Human name for a profile slot."""
    if slot == HOST_SLOT:
        return "(host: outside the interpreter)"
    if slot < 256:
        return _ONE_BYTE.get(slot, "-")
    return _TWO_BYTE.get(slot - 256, "-")


def slot_label(slot):
    if slot == HOST_SLOT:
        return "HOST"
    return f"{slot:02X}" if slot < 256 else f"0F {slot - 256:02X}"


class ProfileError(RuntimeError):
    pass


def block_address(backend, syms):
    """Follow x86_profile_ptr to the DTCM block, checking the magic.

    A zero pointer or a wrong magic means the target was not built with
    X86_PROFILE=1 (or never got as far as x86_profile_init), which is worth
    saying plainly -- otherwise it reads as "the guest executed nothing".
    """
    if "x86_profile_ptr" not in syms:
        raise ProfileError(
            "x86_profile_ptr is not in the ELF: this build has no profiler. "
            "Rebuild with X86_PROFILE=1 (and --clean: flags are not "
            "dependencies).")
    addr = backend.read_uint32(syms["x86_profile_ptr"])
    if not addr:
        raise ProfileError(
            "x86_profile_ptr is NULL: the core did not initialise the "
            "profiler. Built with X86_PROFILE=1?")
    magic = backend.read_uint32(addr + 12)
    if magic != MAGIC:
        raise ProfileError(
            f"profile block at 0x{addr:08x} has magic 0x{magic:08x}, "
            f"expected 0x{MAGIC:08x} -- not an initialised profile block.")
    return addr


def _words(backend, addr, count):
    data = backend.read_memory(addr, 4 * count)
    return [int.from_bytes(data[4 * i:4 * i + 4], "little") for i in range(count)]


def reset(backend, addr):
    """Zero the buckets and re-open the HOST slot, keeping the calibration.

    Done with the target HALTED, and it re-stamps prev_ts from CYCCNT: leaving
    the old timestamp would charge the whole halted interval -- seconds of it --
    to whichever opcode was mid-flight.
    """
    overhead = backend.read_uint32(addr + 8)
    backend.write_memory(addr, bytes(BLOCK_BYTES))
    backend.write_uint32(addr + 0, HOST_SLOT)
    backend.write_uint32(addr + 4, backend.read_uint32(DWT_CYCCNT))
    backend.write_uint32(addr + 8, overhead)
    backend.write_uint32(addr + 12, MAGIC)


def read_profile(backend, addr):
    """-> dict with the header and a list of populated slot records."""
    hdr = _words(backend, addr, HDR_WORDS)
    body = _words(backend, addr + 4 * HDR_WORDS, SLOTS * ENTRY_WORDS)
    rows = []
    for slot in range(SLOTS):
        lo, hi, count = body[slot * 4], body[slot * 4 + 1], body[slot * 4 + 2]
        cycles = (hi << 32) | lo
        if count:
            rows.append({"slot": slot, "count": count, "cycles": cycles})
    return {"addr": addr, "prev_slot": hdr[0], "overhead": hdr[2], "rows": rows}


def resolve_handlers(backend, syms, symbol_maps, opsize=16):
    """slot -> handler symbol, read from the LIVE dispatch tables.

    The tables are the interpreter's own (opcode_table_{16,32}), so this cannot
    drift from what actually ran, and it reuses probe.py's symbol machinery
    rather than re-deriving handler addresses. Handlers exist per operand size,
    hence the argument: TOPBENCH is 16-bit real mode.
    """
    out = {}
    tables = ((0, f"opcode_table_{opsize}"), (256, f"opcode_table_0f_{opsize}"))
    merged = {}
    for m in symbol_maps or []:
        merged.update(m)
    merged = merged or syms
    for base, name in tables:
        if name not in syms:
            continue
        for i, word in enumerate(_words(backend, syms[name], 256)):
            if word:
                out[base + i] = nearest_symbol(merged, word & ~1)
    return out


def rank(profile, handlers, sort="total", include_host=False):
    rows = [dict(r) for r in profile["rows"]
            if include_host or r["slot"] != HOST_SLOT]
    guest_cycles = sum(r["cycles"] for r in profile["rows"]
                       if r["slot"] != HOST_SLOT)
    overhead = profile["overhead"]
    for r in rows:
        r["mnemonic"] = slot_name(r["slot"])
        r["label"] = slot_label(r["slot"])
        r["handler"] = handlers.get(r["slot"], "")
        r["mean"] = r["cycles"] / r["count"]
        corrected = max(0, r["cycles"] - overhead * r["count"])
        r["cycles_corrected"] = corrected
        r["mean_corrected"] = corrected / r["count"]
        r["pct"] = 100.0 * r["cycles"] / guest_cycles if guest_cycles else 0.0
    key = {"total": lambda r: -r["cycles"],
           "mean": lambda r: -r["mean"],
           "count": lambda r: -r["count"]}[sort]
    rows.sort(key=key)
    return rows, guest_cycles


CAVEAT = (
    "Attribution is APPROXIMATE: a delta is measured between two dispatches on a\n"
    "superscalar M7 with an out-of-order memory system, so an opcode's figure\n"
    "carries part of its predecessor and of the cache/AXI state it inherited.\n"
    "Use this to RANK cycle hogs, not as absolute per-opcode truth. Profiling\n"
    "also slows the emulator, so anything timing-dependent in the guest is\n"
    "perturbed -- that is not corrected for."
)
GWEMU_WARNING = (
    "*** TARGET IS gwemu: THE CYCLE COLUMNS ARE MEANINGLESS. ***\n"
    "gwemu does not model the Cortex-M7 pipeline (docs/06), so its DWT deltas\n"
    "are an artefact of the host, not of the silicon. The COUNTS are exact and\n"
    "the plumbing is what a gwemu run proves. Run on hardware for cycles."
)


def print_table(rows, guest_cycles, profile, target, top=40, sort="total",
                out=print):
    if target != "hardware":
        out(GWEMU_WARNING)
        out("")
    out(f"profile block   0x{profile['addr']:08x}")
    out(f"slots with data {len(profile['rows'])}")
    out(f"total cycles    {guest_cycles} (guest dispatches only)")
    host = [r for r in profile["rows"] if r["slot"] == HOST_SLOT]
    if host:
        out(f"host cycles     {host[0]['cycles']} in {host[0]['count']} "
            f"interpreter entries (frame loop, blit, HLE)")
    out(f"measured overhead {profile['overhead']} cyc/dispatch "
        f"(calibrated on target; a lower bound -- see x86_profile.h)")
    out(f"sorted by       {sort}")
    out("")
    out(f"{'op':<6}{'mnemonic':<18}{'handler':<30}{'count':>11}"
        f"{'cycles':>13}{'mean':>8}{'mean-ovh':>10}{'%':>7}")
    for r in rows[:top]:
        out(f"{r['label']:<6}{r['mnemonic'][:17]:<18}{r['handler'][:29]:<30}"
            f"{r['count']:>11}{r['cycles']:>13}{r['mean']:>8.1f}"
            f"{r['mean_corrected']:>10.1f}{r['pct']:>7.2f}")
    out("")
    out(CAVEAT)


def write_json(path, rows, guest_cycles, profile, target, meta=None):
    doc = {
        "_comment": "Generated by scripts/profile_opcodes.py. Cycle figures are "
                    "meaningful only from --target hardware; see the caveat.",
        "_caveat": CAVEAT,
        "target": target,
        "cycles_meaningful": target == "hardware",
        "overhead_cycles_per_dispatch": profile["overhead"],
        "totals": {
            "guest_cycles": guest_cycles,
            "guest_dispatches": sum(r["count"] for r in rows),
            "slots": len(rows),
        },
        "meta": meta or {},
        "slots": {r["label"]: {k: r[k] for k in
                               ("slot", "mnemonic", "handler", "count",
                                "cycles", "mean", "cycles_corrected",
                                "mean_corrected", "pct")}
                  for r in rows},
    }
    with open(path, "w") as fh:
        json.dump(doc, fh, indent=1, sort_keys=True)
        fh.write("\n")
    return path


def write_csv(path, rows):
    cols = ["label", "slot", "mnemonic", "handler", "count", "cycles", "mean",
            "cycles_corrected", "mean_corrected", "pct"]
    with open(path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)
    return path


def wait(seconds, what, out=print):
    out(f"  {what}: {seconds:.1f}s")
    time.sleep(seconds)


# ---------------------------------------------------------------------------
# GATES -- the check that says WHICH WORKLOAD was profiled
#
# A profile is a table of numbers that looks equally confident whether the
# guest was raycasting or sitting on a menu. Two full 57 s hardware runs of
# Wolf3D data were menus before anyone checked, because nothing in the tool
# ever looked at whether the screen was flipping. A gate reads named counters
# at the two ends of the measurement window and asserts the direction each one
# had to move for the run to be the workload it claims.
# ---------------------------------------------------------------------------

def parse_gates(specs):
    """['x86_modey_presents', 'foo=const'] -> [(sym, 'climb'|'const'), ...]"""
    gates = []
    for spec in specs or []:
        for item in spec.split(","):
            item = item.strip()
            if not item:
                continue
            sym, _, want = item.partition("=")
            want = want or "climb"
            if want not in ("climb", "const"):
                raise ProfileError(f"gate '{item}': want climb or const")
            gates.append((sym.strip(), want))
    return gates


def read_gates(backend, syms, gates):
    out = {}
    for sym, _ in gates:
        if sym not in syms:
            raise ProfileError(f"gate symbol '{sym}' is not in the ELF")
        out[sym] = backend.read_uint32(syms[sym])
    return out


def check_gates(gates, before, after, out=print):
    """Report every gate, return False if any one was violated."""
    if not gates:
        return True
    ok = True
    out("workload gates (what the window actually ran):")
    for sym, want in gates:
        lo, hi = before.get(sym, 0), after.get(sym, 0)
        delta = (hi - lo) & 0xFFFFFFFF
        if delta > 0x7FFFFFFF:            # wrapped backwards: treat as no move
            delta = hi - lo
        good = delta > 0 if want == "climb" else delta == 0
        ok = ok and good
        verdict = "OK" if good else f"FAILED (wanted {want})"
        out(f"  {sym:<28} {lo:>12} -> {hi:>12}  delta {delta:>+12}  {verdict}")
    if not ok:
        out("")
        out("*** A GATE FAILED. The numbers above are from a DIFFERENT workload")
        out("*** than the one this run claims. Do not use them.")
    out("")
    return ok


def compare(path_a, path_b, top=30, sort="delta", out=print):
    """Diff two profiles: is B slower because it runs DIFFERENT instructions,
    or the SAME instructions more slowly? An average cannot tell those apart
    and they are completely different problems."""
    with open(path_a) as fh:
        a = json.load(fh)
    with open(path_b) as fh:
        b = json.load(fh)

    def norm(doc):
        tot = doc["totals"]
        n = tot["guest_dispatches"] or 1
        cyc = tot["guest_cycles"] or 1
        return doc["slots"], n, cyc

    sa, na, ca = norm(a)
    sb, nb, cb = norm(b)
    out(f"A = {path_a}   {na} dispatches, {ca} cycles, "
        f"{ca / na:.1f} cyc/insn")
    out(f"B = {path_b}   {nb} dispatches, {cb} cycles, "
        f"{cb / nb:.1f} cyc/insn")
    out("")

    # The whole point: split the change in cycles-per-instruction into the part
    # explained by a changed MIX (same per-opcode cost, different shares) and
    # the part that is a genuine per-opcode SLOWDOWN.
    labels = set(sa) | set(sb)
    mix = same = 0.0
    rows = []
    for lab in labels:
        ra, rb = sa.get(lab), sb.get(lab)
        share_a = (ra["count"] / na) if ra else 0.0
        share_b = (rb["count"] / nb) if rb else 0.0
        mean_a = ra["mean"] if ra else 0.0
        mean_b = rb["mean"] if rb else 0.0
        # mix term: B's share at A's cost. cost term: B's share x cost change.
        mix_c = (share_b - share_a) * (mean_a if ra else mean_b)
        cost_c = share_b * (mean_b - mean_a) if (ra and rb) else 0.0
        mix += mix_c
        same += cost_c
        rows.append({"label": lab,
                     "mnemonic": (rb or ra)["mnemonic"],
                     "share_a": 100 * share_a, "share_b": 100 * share_b,
                     "mean_a": mean_a, "mean_b": mean_b,
                     "mix": mix_c, "cost": cost_c})
    total = (cb / nb) - (ca / na)
    out("cycles/instruction, A -> B, decomposed:")
    out(f"  total change            {total:>9.2f} cyc/insn")
    out(f"  from a CHANGED MIX      {mix:>9.2f} cyc/insn  "
        f"({100 * mix / total if total else 0:.0f}%)")
    out(f"  from PER-OPCODE COST    {same:>9.2f} cyc/insn  "
        f"({100 * same / total if total else 0:.0f}%)")
    out(f"  unexplained             {total - mix - same:>9.2f} cyc/insn")
    out("")
    key = {"delta": lambda r: -abs(r["mix"] + r["cost"]),
           "mix": lambda r: -abs(r["mix"]),
           "cost": lambda r: -abs(r["cost"])}[sort]
    rows.sort(key=key)
    out(f"{'op':<6}{'mnemonic':<18}{'%A':>7}{'%B':>7}{'meanA':>9}{'meanB':>9}"
        f"{'mix':>9}{'cost':>9}")
    for r in rows[:top]:
        out(f"{r['label']:<6}{r['mnemonic'][:17]:<18}{r['share_a']:>7.2f}"
            f"{r['share_b']:>7.2f}{r['mean_a']:>9.1f}{r['mean_b']:>9.1f}"
            f"{r['mix']:>9.2f}{r['cost']:>9.2f}")
    out("")
    out("mix  = cycles/insn added because B runs MORE of this opcode\n"
        "cost = cycles/insn added because this opcode got SLOWER in B\n"
        "A big mix column and a small cost column means the guest asked for\n"
        "more expensive work, not that the interpreter regressed.")
    return {"total": total, "mix": mix, "cost": same, "rows": rows}


def check_clean(target, symbol_maps, out=print):
    """Fault state after a run. A profile from a hardfaulted target is a lie,
    and the failure mode of a bad dispatch patch is exactly a hardfault."""
    from .probe import read_faults
    target.backend.halt()
    try:
        vals = read_faults(target.backend, out=out, symbol_maps=symbol_maps)
    finally:
        target.backend.resume()
    dirty = vals["CFSR"] or (vals["HFSR"] & ~0x0)
    if dirty:
        out("!! FAULT STATE IS NOT CLEAN -- the numbers above are not a profile.")
    return not dirty


def die(msg):
    sys.exit(f"profile_opcodes: {msg}")
