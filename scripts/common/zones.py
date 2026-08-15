"""
Host side of the frame-loop phase/zone accounting (x86_zones.h, X86_ZONES=1).

The on-target half brackets the emulator loop with an explicit push/pop stack
and accumulates EXCLUSIVE SELF-TIME per zone; the root zone is the frame, so
its self-time is the unaccounted residual by construction rather than by
subtraction. All of that is documented at the top of x86_zones.h and is not
repeated here -- what lives here is: locate the block, ask for a reset, read it
back, and turn it into rows a report can print.

Two things this module refuses to smooth over:

  * **The residual is a first-class row.** It is printed even when it is small,
    and flagged when it is not. A breakdown that quietly normalises to 100% can
    never tell you about the phase nobody instrumented -- which is the single
    most valuable thing an accounting like this produces.
  * **Cycles are target-dependent and the rows say so.** Under gwemu the COUNTS
    are exact and the CYCLES are an artefact of the host: there is no pipeline
    model (docs/06) and the SD is free (docs/04), so DISK reads as ~0 there and
    could not read as anything else.
"""

# ---- layout, mirroring x86_zones_t. Keep in step with x86_zones.h. ---------
MAGIC = 0x5A4F4E45          # 'ZONE'
STACK_MAX = 8
ZONE_COUNT = 10
ZONE_WORDS = 6
ZONE_BYTES = 4 * ZONE_WORDS

OFF_MAGIC = 0
OFF_VERSION = 4
OFF_RESET = 8
OFF_OVERFLOW = 12
OFF_UNBALANCED = 16
OFF_OVERHEAD = 20
OFF_FRAMES = 24
OFF_LAST_TS = 28
OFF_SP = 32
OFF_STK = 48
OFF_MARK = OFF_STK + 4 * STACK_MAX
OFF_ZONES = OFF_MARK + 4 * STACK_MAX
BLOCK_BYTES = OFF_ZONES + ZONE_COUNT * ZONE_BYTES

# Index -> (short name, what it contains). The order IS the zone id order in
# x86_zones.h; a mismatch here silently mislabels a report, so if you add a
# zone add it in both places in the same commit.
ZONES = [
    ("residual", "UNACCOUNTED -- frame-loop time inside no named zone"),
    ("guest", "x86_cpu_interp_asm(): emulated instructions"),
    ("hle", "INT 10h/16h/1Ah/21h service, minus the disk I/O under it"),
    ("disk", "the SD backend round trip (nested inside hle)"),
    ("video", "CGA render + OSK draw + CLUT push"),
    ("audio", "PC-speaker render into the DMA half-buffer"),
    ("input", "gamepad poll, retro-go menu, on-screen keyboard"),
    ("fwloop", "the firmware's common_emu_frame_loop() bookkeeping"),
    ("swap", "IDLE: waiting for the panel swap (this is headroom)"),
    ("calib", "calibration scratch -- never populated in a real run"),
]
RESIDUAL = 0
IDLE_ZONES = {8}            # swap: time we had spare, not time we spent


class ZoneError(RuntimeError):
    pass


def block_address(backend, syms):
    """Follow x86_zones_ptr to the DTCM block, checking the magic."""
    if "x86_zones_ptr" not in syms:
        raise ZoneError(
            "x86_zones_ptr is not in the ELF: this build has no zone "
            "accounting. Rebuild with X86_ZONES=1 (and clean -- flags are not "
            "dependencies in this project).")
    addr = backend.read_uint32(syms["x86_zones_ptr"])
    if not addr:
        raise ZoneError(
            "x86_zones_ptr is NULL: the core did not initialise the zone "
            "block (check zones_block_ok). Built with X86_ZONES=1?")
    magic = backend.read_uint32(addr + OFF_MAGIC)
    if magic != MAGIC:
        raise ZoneError(
            f"zone block at 0x{addr:08x} has magic 0x{magic:08x}, expected "
            f"0x{MAGIC:08x} -- not an initialised zone block.")
    return addr


def request_reset(backend, addr):
    """Ask the TARGET to zero its own counters at the next frame boundary.

    Deliberately not a host-side memset. The stack carries per-level marks into
    the very counters being zeroed, so wiping them from outside mid-frame makes
    the frame in flight compute a nonsense self-time -- and it would be a
    nonsense that looks like a real measurement. One flag word, checked once per
    frame, costs nothing and cannot race.
    """
    backend.write_uint32(addr + OFF_RESET, 1)


def reset_done(backend, addr):
    return backend.read_uint32(addr + OFF_RESET) == 0


def read(backend, addr):
    """-> dict with the header and one record per zone."""
    data = backend.read_memory(addr, BLOCK_BYTES)

    def w(off):
        return int.from_bytes(data[off:off + 4], "little")

    zones = []
    for i in range(ZONE_COUNT):
        base = OFF_ZONES + i * ZONE_BYTES
        lo, hi = w(base), w(base + 4)
        count, worst = w(base + 8), w(base + 12)
        zones.append({
            "id": i,
            "name": ZONES[i][0],
            "what": ZONES[i][1],
            "cycles": (hi << 32) | lo,
            "count": count,
            "worst": worst,
        })
    return {
        "addr": addr,
        "version": w(OFF_VERSION),
        "overflow": w(OFF_OVERFLOW),
        "unbalanced": w(OFF_UNBALANCED),
        "overhead_cycles_per_pair": w(OFF_OVERHEAD),
        "frames": w(OFF_FRAMES),
        "sp": w(OFF_SP),
        "zones": zones,
    }


def summarise(block, cycles_per_us=None):
    """Derive the report's numbers, including the instrument's own share.

    `total` is the sum of every zone's self-time, which by the root-zone scheme
    IS the wall time of every frame measured -- no separate frame timer is read
    and no residual is inferred by subtracting one total from another.
    """
    zones = [z for z in block["zones"] if z["count"] or z["cycles"]]
    total = sum(z["cycles"] for z in zones)
    frames = block["frames"] or 0
    busy = sum(z["cycles"] for z in zones if z["id"] not in IDLE_ZONES)

    # Transitions: every occurrence of a non-root zone is one enter/exit pair,
    # and every frame is one begin/end pair. Multiplying by the calibrated
    # per-pair cost gives the instrument's own share of what it measured -- a
    # LOWER bound (see x86_zones_calibrate), and reported as one.
    pairs = sum(z["count"] for z in zones if z["id"] != RESIDUAL) + frames
    overhead = block["overhead_cycles_per_pair"] * pairs

    rows = []
    for z in zones:
        if z["id"] == 9 and not z["count"]:
            continue
        r = dict(z)
        r["pct"] = 100.0 * z["cycles"] / total if total else 0.0
        r["mean"] = z["cycles"] / z["count"] if z["count"] else 0.0
        r["per_frame"] = z["cycles"] / frames if frames else 0.0
        r["calls_per_frame"] = z["count"] / frames if frames else 0.0
        if cycles_per_us:
            r["us_total"] = z["cycles"] / cycles_per_us
            r["us_per_frame"] = r["per_frame"] / cycles_per_us
            r["us_worst"] = z["worst"] / cycles_per_us
        rows.append(r)
    rows.sort(key=lambda r: -r["cycles"])

    residual = next((r for r in rows if r["id"] == RESIDUAL), None)
    return {
        "rows": rows,
        "total_cycles": total,
        "busy_cycles": busy,
        "frames": frames,
        "transitions": pairs,
        "overhead_cycles": overhead,
        "overhead_pct": 100.0 * overhead / total if total else 0.0,
        # Against BUSY time as well, because measuring against a frame that is
        # mostly idle flatters the instrument: what a change to the emulator
        # has to be bigger than is the overhead's share of the WORK.
        "overhead_pct_busy": 100.0 * overhead / busy if busy else 0.0,
        "residual_cycles": residual["cycles"] if residual else 0,
        "residual_pct": residual["pct"] if residual else 0.0,
        "overflow": block["overflow"],
        "unbalanced": block["unbalanced"],
        "overhead_cycles_per_pair": block["overhead_cycles_per_pair"],
    }


# A residual above this is the report telling you a phase is missing, not a
# rounding artefact. 2% is roughly where per-transition jitter stops being able
# to explain it on this loop.
RESIDUAL_LOUD_PCT = 2.0
