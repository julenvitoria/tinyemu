"""
Host side of the probe accumulator (x86_probe.h, X86_PROBE=1).

Locate the block, ask for a reset, read it back, and turn it into rows a report
can rank. The on-target design -- why an accumulator and not a tenth ring, and
where each "how we answered" bit comes from -- is documented at the top of
x86_probe.h and is not repeated here.

What lives here is the JOIN: runtime evidence from the target, combined with the
curated knowledge in probe_expect.py, into one status per row. The precedence
rule is the whole correctness argument of this module:

    RUNTIME EVIDENCE WINS. If the target says nobody answered, the row is
    `unanswered` no matter what the table claims. The table only gets to
    classify calls the target says WERE answered -- because that is exactly the
    case the target cannot see the difference in.
"""

from . import probe_expect as expect

MAGIC = 0x50524F42          # 'PROB'
VERSION = 1

# Header layout, mirroring x86_probe_t. Keep in step with x86_probe.h.
OFF_MAGIC = 0
OFF_VERSION = 4
OFF_RESET = 8
OFF_ENTS = 12
OFF_USED = 16
OFF_DROPPED = 20
OFF_CALLS = 24
OFF_HASH = 32
HASH_BYTES = 256
OFF_ENT = OFF_HASH + HASH_BYTES
ENT_BYTES = 20

KIND_INT, KIND_IN, KIND_OUT = 0, 1, 2
KIND_NAME = {KIND_INT: "INT", KIND_IN: "IN", KIND_OUT: "OUT"}

F_ANSWERED = 0x01
F_UNANSWERED = 0x02
F_MARKED = 0x04
F_RETRY = 0x08
F_CF_SET = 0x10
F_CF_CLEAR = 0x20
F_NOPORT = 0x40


class ProbeError(RuntimeError):
    pass


def block_address(backend, syms):
    if "x86_probe" not in syms:
        raise ProbeError(
            "x86_probe is not in the ELF: this build has no probe "
            "accumulator. Rebuild with X86_PROBE=1 -- and CLEAN, because build "
            "flags are not dependencies in this project.")
    return syms["x86_probe"]


def request_reset(backend, addr):
    """One flag word, read by the target before its next record.

    Not a host-side memset: the table and its hash index have to be zeroed
    together or a stale bucket points at a cleared entry, and the run after
    that silently merges two different services into one row.
    """
    backend.write_uint32(addr + OFF_RESET, 1)


def read(backend, addr):
    """-> dict with the header and one raw record per used entry."""
    head = backend.read_memory(addr, OFF_ENT)

    def w(off):
        return int.from_bytes(head[off:off + 4], "little")

    magic = w(OFF_MAGIC)
    if magic != MAGIC:
        # Zero is the honest common case: built with X86_PROBE=1, but the guest
        # never made a single service call or port access, so the header was
        # never stamped. That is a finding, not a decode failure.
        if magic == 0:
            return {"magic": 0, "used": 0, "dropped": 0, "calls": 0,
                    "ents": 0, "entries": [], "never_ran": True}
        raise ProbeError(
            f"x86_probe at 0x{addr:08x} has magic 0x{magic:08x}, expected "
            f"0x{MAGIC:08x}.")
    if w(OFF_VERSION) != VERSION:
        raise ProbeError(
            f"x86_probe version {w(OFF_VERSION)} but this decoder speaks "
            f"{VERSION}. Rebuild the core, or check out a matching tree.")

    used, ents = w(OFF_USED), w(OFF_ENTS)
    if used > ents:
        raise ProbeError(f"x86_probe claims {used} of {ents} entries used")
    raw = backend.read_memory(addr + OFF_ENT, used * ENT_BYTES) if used else b""
    entries = []
    for i in range(used):
        b = raw[i * ENT_BYTES:(i + 1) * ENT_BYTES]
        entries.append({
            "id": int.from_bytes(b[0:2], "little"),
            "kind": b[2],
            "ah": b[3],
            "sub": b[4],
            "flags": b[5],
            "ax": int.from_bytes(b[6:8], "little"),
            "count": int.from_bytes(b[8:12], "little"),
            "bx": int.from_bytes(b[12:14], "little"),
            "cx_dx": int.from_bytes(b[16:20], "little"),
        })
    return {"magic": magic, "used": used, "ents": ents,
            "dropped": w(OFF_DROPPED), "calls": w(OFF_CALLS),
            "entries": entries, "never_ran": False}


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------
def classify(ent):
    """-> (status, source, note). Runtime evidence outranks the table."""
    kind, flags = ent["kind"], ent["flags"]

    if kind == KIND_INT:
        sub = None if ent["sub"] == 0xFF else ent["sub"]
        if flags & (F_UNANSWERED | F_MARKED):
            e, _ = expect.lookup_int(ent["id"], ent["ah"], sub)
            what = e["ours"] if e else None
            why = ("rc==0" if flags & F_UNANSWERED else
                   "a handler moved x86_hle_unhandled_*")
            note = f"the TARGET observed this go unanswered ({why})"
            if e and e["status"] != "unanswered":
                # The table and the target disagree. Say so rather than pick a
                # winner silently: it means either the table is stale or a
                # subfunction fell through a handler the table calls complete.
                note += (f" -- but the table calls this `{e['status']}`, so "
                         f"either the table is stale or only SOME inputs are "
                         f"handled")
            return "unanswered", "runtime", (what and f"{what}. {note}") or note
        e, src = expect.lookup_int(ent["id"], ent["ah"], sub)
        if e is None:
            return ("unclassified", "no table entry",
                    "the target answered this, but nobody has vetted WHAT it "
                    "answered. Add an entry to scripts/common/probe_expect.py")
        note = e["ours"]
        if e.get("real"):
            note += f" | on real hardware: {e['real']}"
        return e["status"], src, note

    # Ports.
    if flags & F_NOPORT:
        e, _ = expect.lookup_port(ent["id"])
        note = "nothing in x86_io.c models this port"
        note += (" (reads float 0xFF; writes are dropped)"
                 if kind == KIND_IN else " (the write was dropped)")
        if e:
            note = f"{e['ours']}. {note}"
            if e.get("real"):
                note += f" | {e['real']}"
        return "unmodelled-port", "runtime", note
    e, src = expect.lookup_port(ent["id"])
    if e is None:
        return ("unclassified", "no table entry",
                "a modelled port with no vetted description. Add an entry to "
                "scripts/common/probe_expect.py")
    note = e["ours"]
    if e.get("real"):
        note += f" | on real hardware: {e['real']}"
    # A port the table calls unmodelled that the target did NOT flag is a table
    # error -- the switch grew an arm and nobody updated the note.
    if e["status"] == "unmodelled-port":
        return ("unclassified", "table disagrees with the target",
                f"the table says unmodelled, but x86_io.c answered it. "
                f"{note}")
    return e["status"], src, note


def _name_int(vector, ah, sub):
    s = f"INT {vector:02X}h AH={ah:02X}h"
    if sub != 0xFF:
        s += f" AL={sub:02X}h" if not (vector == 0x10 and ah == 0x12) \
             else f" BL={sub:02X}h"
    return s


def _distinct_values(mask):
    """The OR-fold of (1 << (value & 31)) the target keeps for ports."""
    return [i for i in range(32) if mask & (1 << i)]


def rows(block):
    """-> a list of dicts, ranked worst-first. This ordering IS the report."""
    out = []
    for ent in block["entries"]:
        status, source, note = classify(ent)
        kind = ent["kind"]
        if kind == KIND_INT:
            name = _name_int(ent["id"], ent["ah"], ent["sub"])
            cf = ent["flags"] & F_CF_SET
            cfc = ent["flags"] & F_CF_CLEAR
            cf_s = ("CF=1" if cf and not cfc else
                    "CF=0" if cfc and not cf else
                    "CF=0/1" if cf and cfc else "-")
            answer = (f"{cf_s} AX={ent['ax']:#06x} BX={ent['bx']:#06x} "
                      f"CX:DX={ent['cx_dx']:#010x}")
            if ent["flags"] & F_RETRY:
                answer += " (blocking: re-executed)"
        else:
            name = f"{KIND_NAME[kind]} {ent['id']:#06x}"
            vals = _distinct_values(ent["cx_dx"])
            answer = f"last={ent['ax']:#04x}"
            # Low-5-bit fold: enough to separate a polled status register from
            # a programmed register file, which is the question a port row
            # raises. It is a FOLD and the report must not imply otherwise.
            answer += (f", {len(vals)} distinct low-5-bit value(s)"
                       if ent["count"] > 1 else "")
        out.append({
            "kind": KIND_NAME[kind],
            "name": name,
            "id": ent["id"],
            "ah": ent["ah"],
            "sub": None if ent["sub"] == 0xFF else ent["sub"],
            "count": ent["count"],
            "saturated": ent["count"] == 0xFFFFFFFF,
            "answer": answer,
            "status": status,
            "source": source,
            "note": note,
            "severity": expect.SEVERITY.get(status, 50),
            "actionable": status in expect.ACTIONABLE,
        })
    out.sort(key=lambda r: (-r["severity"], -r["count"], r["kind"], r["id"],
                            r["ah"], r["sub"] if r["sub"] is not None else -1))
    return out
