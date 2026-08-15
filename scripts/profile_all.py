#!/usr/bin/env python3
"""
One command, one named workload, one full report: where the frame time goes.

This is the driver over instruments that already exist. It does NOT reimplement
any of them -- it builds the right cores, boots a named workload, settles it,
opens a measurement window, collects everything, and writes ONE report that
leads with the answer:

    zone accounting   x86_zones.h + scripts/common/zones.py   (X86_ZONES=1)
    per-opcode cycles x86_profile.h + scripts/common/opcode_profile.py
                                                              (X86_PROFILE=1)
    standing counters src/main.c, x86_disk.c, x86_bios.c, read with probe.py
    frame tail        frame_hist / frame_ms_worst / frame_overruns
    memory headroom   __CORE_BSS_END__ from the link, plus the runtime *_ok
                      flags -- because an over-allocation here is SILENT
    host PC histogram scripts/common/pcsample.py, IF that instrument is present
                      in this tree (it is optional and reported as absent)

------------------------------------------------------------------------------
SEVERAL PASSES, NOT ONE -- AND WHY
------------------------------------------------------------------------------
The instruments perturb what they measure by very different amounts, and
merging their outputs from a single run would quietly attribute one
instrument's cost to the emulator:

  * X86_PROFILE reads DWT at EVERY dispatch. Measured on silicon at ~65 cycles
    on top of a ~50-cycle instruction -- it more than doubles guest execution.
    A zone breakdown taken from that build would say "guest execution is
    everything", which is true of the instrumented build and false of the real
    one.
  * X86_ZONES is per-frame and per-service-call. Its cost is calibrated on the
    target and reported; it is small, but it is not assumed to be zero.

So each instrument gets its OWN pass with its own clean build, and the report
states which pass every number came from. The zone pass is the one whose
timings are quoted as the emulator's; the opcode pass contributes RANKING, not
wall-clock shares. That is more honest than one fast run, and the cost is a
rebuild.

------------------------------------------------------------------------------
WHAT IS TRUSTWORTHY, PER TIER
------------------------------------------------------------------------------
Printed IN the report, not only here, so a gwemu report can never be mistaken
for a silicon one:

  gwemu     COUNTS are exact and reproducible. CYCLE TIMINGS ARE NOT
            MEANINGFUL: there is no Cortex-M7 pipeline model (docs/06) and the
            SD card has no busy state (docs/04), so the disk zone reads as
            nearly free and cannot read as anything else. Use gwemu to prove
            the instrumentation runs and to compare COUNTS across builds.
  hardware  Both counts and cycles are real. This is the only tier whose
            "where the time went" answer is the device's answer.

Usage:

    python3 scripts/profile_all.py --workload alleycat
    python3 scripts/profile_all.py --workload topbench-menu --window 15
    python3 scripts/profile_all.py --workload alleycat --passes zones
    python3 scripts/profile_all.py --rom path/to/GAME.dsk --name mygame

    # did that change help?
    python3 scripts/profile_all.py --compare before.json after.json
"""

import argparse
import datetime
import json
import os
import subprocess
import sys
import time

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from scripts.common import opcode_profile as prof                    # noqa: E402
from scripts.common import zones as zonelib                          # noqa: E402
from scripts.common.probe import (load_symbol_maps, read_faults,     # noqa: E402
                                  wait_for_value)
from scripts.common.target import (Image, add_target_args,           # noqa: E402
                                   open_target, timeline_quit_time)

FW_DIR = "references/game-and-watch-retro-go-sd/build"
ROM_DIR = "references/game-and-watch-retro-go-sd/roms/dos"
CORE_MAGIC = 0x54494E59
CORE_ELF = "build/core/dos_core.elf"
OUT_DIR = "build/artifacts"

# ---------------------------------------------------------------------------
# Workloads. A preset is a .dsk plus the timeline that gets it running and the
# settle time it needs -- the three things that have to agree, and the reason
# this is a named preset rather than three flags a caller has to get right.
#
# `cmdtail` mirrors guest_use_cmdtail: TOPBENCH.EXE and TOPBENCH.EXE -l are
# different programs (a menu that waits for a key, versus a scoring loop that
# exits), so the workload has to say which one it means.
# ---------------------------------------------------------------------------
WORKLOADS = {
    "alleycat": {
        # Its OWN timeline, and the reason is written out in that file: Alley
        # Cat has TWO gates before it runs -- a joystick prompt and a
        # skill-level menu that wants a LETTER, which only the on-screen
        # keyboard can send. Until both are answered the game is a
        # two-instruction spin loop, and profiling that came back 56%
        # `CMP Gv,Ev` + 44% `JZ Jb`: an exact measurement of a game that had
        # not started. The settle here is long BECAUSE it has to outlast the
        # timeline's last keypress. Verified with screenshot.py -- CGA mode 4,
        # full screen -- not assumed.
        "rom": f"{ROM_DIR}/ALLEYCAT.dsk",
        "timeline": "timelines/profile-alleycat.tl",
        "settle": 28.0,
        "cmdtail": 1,
        "what": "Alley Cat past its joystick prompt: CGA graphics mode, the "
                "PC speaker, a real game loop. The steady-state workload.",
    },
    "alleycat-boot": {
        "rom": f"{ROM_DIR}/ALLEYCAT.dsk",
        "timeline": "timelines/start-preselected-game.tl",
        "settle": 0.0,
        "cmdtail": 1,
        "what": "Alley Cat from the first instruction: DOS startup, the MZ "
                "load and the unpacker. Disk-heavy, and NOT a steady state.",
    },
    "topbench-menu": {
        "rom": f"{ROM_DIR}/TOPBENCH.dsk",
        "timeline": "timelines/start-preselected-game.tl",
        "settle": 8.0,
        "cmdtail": 0,
        "what": "TOPBENCH's full-screen menu: CGA TEXT mode 3, a database read "
                "off the .dsk, and an INT 16h poll loop. The text/disk "
                "counterpart to alleycat's graphics/audio.",
    },
    "topbench-score": {
        "rom": f"{ROM_DIR}/TOPBENCH.dsk",
        "timeline": "timelines/start-preselected-game.tl",
        "settle": 0.0,
        "cmdtail": 1,
        "what": "TOPBENCH.EXE -l, the scoring loop. --settle 0 deliberately: "
                "it halts on INSB a few seconds in (see STATUS.md), so a "
                "window that starts later profiles a dead guest.",
    },
}

# Standing counters worth having in every report. Grouped so the report can
# print them under headings rather than as one wall of names.
COUNTER_GROUPS = [
    # x86_modey_presents and x86_vga_modey_armed are in the FIRST group on
    # purpose. A Mode Y title's menus and its gameplay differ by ~2x in
    # instructions per second and by ~18x in planar stores, and a report of the
    # menus is indistinguishable from a report of gameplay in every other row
    # here. presents CONSTANT means the timeline never reached the game --
    # which has produced two full 57-second runs of confident wrong data.
    ("throughput", ["guest_ips_achieved", "guest_fps_achieved", "guest_kips",
                    "guest_slice_now", "guest_insn_deficit", "guest_exec_ms",
                    "guest_insns_total", "frames_rendered", "guest_uptime_ms",
                    "x86_modey_presents", "x86_vga_modey_armed",
                    "x86_vga_planar_stores"]),
    ("frame tail", ["frame_ms_last", "frame_ms_worst", "frame_ms_worst2",
                    "frame_overruns", "panel_ms_min", "frame_10us_avg",
                    "frame_hist_from"]),
    ("guest clock", ["guest_ticks", "guest_ticks_dropped",
                     "guest_tick_irq_lost"]),
    ("disk", ["x86_disk_reads", "x86_disk_backend_reads",
              "x86_disk_cache_hits", "x86_disk_cache_misses",
              "x86_disk_cache_fills", "x86_disk_cache_evictions",
              "x86_disk_cache_bypass", "x86_disk_us_total",
              "x86_disk_us_worst", "x86_disk_errors"]),
    ("services", ["hle_calls_seen", "x86_bios_int16_calls", "x86_kb_pushes",
                  "x86_kb_pops", "x86_map_failures", "hle_bad_vector",
                  "hle_bad_func", "guest_exit_reason", "guest_halted"]),
    ("input", ["input_polls", "input_scancodes", "input_irqs"]),
    ("backing", ["core_ready", "core_dtcm_ok", "guest_vram_ok",
                 "upper_filler_ok", "x86_disk_cache_ok", "x86_disk_cache_bytes",
                 "guest_ram_bytes", "bonus_pool_base", "bonus_pool_size",
                 "zones_block_ok", "dsk_mounted", "dsk_startup_ok",
                 "dsk_exec_rc"]),
]

# Counters that must be RESET at the start of the window, so the report
# describes the window and not everything since boot. Anything without a reset
# is labelled cumulative in the report rather than quietly presented as if it
# were windowed.
RESETTABLE = ["frame_stats_reset"]


def sh(cmd, quiet=False):
    if not quiet:
        print(f"  $ {' '.join(cmd)}")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout[-4000:])
        sys.stderr.write(r.stderr[-4000:])
        raise SystemExit(f"profile_all: command failed: {' '.join(cmd)}")
    return r.stdout


def build_core(flags, rom, quiet=False):
    """Clean-build the packed core with `flags` and lay the SD image down.

    CLEAN, always. Build flags are not dependencies in this project (see
    TOOLING.md); an incremental rebuild after a flag change links objects from
    two different configurations, and the resulting report describes neither.
    """
    print(f"[build] X86 flags: {flags or '(none)'}")
    sh(["make", "-f", "Makefile.core", "clean"], quiet=quiet)
    sh(["make", "-f", "Makefile.core", "pack"] + flags, quiet=quiet)
    sh(["python3", "scripts/sdcard.py", "--image", "sdcard_retrogo.img",
        "--rom", rom], quiet=quiet)


def link_headroom(elf=CORE_ELF):
    """RAM_EMU headroom, straight out of the link.

    Running out is SILENT -- guest_ram comes back NULL, core_ready stays 0 and
    the emulator never starts (TOOLING.md). The core's TEXT lives in this
    region too, so CODE growth counts. Every report carries this number.
    """
    out = sh(["arm-none-eabi-nm", elf], quiet=True)
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            syms[parts[-1]] = int(parts[0], 16)
    bss_end = syms.get("__CORE_BSS_END__")
    ram_end = syms.get("__RAM_EMU_END__")
    if not bss_end or not ram_end:
        return {"note": "__CORE_BSS_END__/__RAM_EMU_END__ not in the ELF"}
    guest = 640 * 1024 + 32 * 1024        # GUEST_RAM_SIZE + X86_VRAM_BYTES
    return {
        "core_bss_end": bss_end,
        "ram_emu_end": ram_end,
        "region_left_bytes": ram_end - bss_end,
        "guest_request_bytes": guest,
        "headroom_bytes": ram_end - bss_end - guest,
    }


def read_counters(backend, syms):
    out = {}
    for _, names in COUNTER_GROUPS:
        for n in names:
            if n in syms:
                try:
                    out[n] = backend.read_uint32(syms[n])
                except Exception:
                    pass
    return out


def read_frame_hist(backend, syms):
    if "frame_hist" not in syms:
        return []
    data = backend.read_memory(syms["frame_hist"], 32)
    return [int.from_bytes(data[4 * i:4 * i + 4], "little") for i in range(8)]


FRAME_HIST_EDGES = ["<=1.5x", "<=2x", "<=3x", "<=4x", "<=6x", "<=8x",
                    "<=16x", ">16x"]


def make_image(args, rom):
    fw = args.firmware_dir
    return Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                 bank2=os.path.join(fw, "qemu_bank2.bin"),
                 extflash=os.path.join(fw, "extflash.bin"),
                 sdcard=args.sd, elf=CORE_ELF, intflash_bank=1,
                 intflash=os.path.join(fw, "gw_retro_go_intflash.bin"))


def boot(args, rom):
    """Boot the workload and wait for OUR core, not just for the firmware."""
    image = make_image(args, rom)
    target = open_target(args, image)
    target.start()
    syms, maps = load_symbol_maps(os.path.join(args.firmware_dir,
                                               "gw_retro_go.elf"), CORE_ELF)
    if "core_magic" in syms:
        print("  waiting for core_magic...")
        if not wait_for_value(target.backend, syms["core_magic"], CORE_MAGIC,
                              40.0):
            target.stop()
            raise SystemExit("profile_all: the core never started; nothing to "
                             "profile. Check with scripts/probe.py.")
        print("  core running")
    return target, syms, maps


# ---------------------------------------------------------------------------
# Pass 1: zones + counters. The build whose TIMINGS the report quotes.
# ---------------------------------------------------------------------------
def pass_zones(args, wl):
    build_core(["X86_ZONES=1"], wl["rom"], quiet=args.quiet)
    headroom = link_headroom()
    target, syms, maps = boot(args, wl["rom"])
    try:
        if wl.get("cmdtail") is not None and "guest_use_cmdtail" in syms:
            target.backend.write_uint32(syms["guest_use_cmdtail"],
                                        wl["cmdtail"])
        settle = args.settle if args.settle is not None else wl["settle"]
        print(f"  settling {settle:.1f}s (discarded)")
        time.sleep(settle)

        target.backend.halt()
        addr = zonelib.block_address(target.backend, syms)
        zonelib.request_reset(target.backend, addr)
        for n in RESETTABLE:
            if n in syms:
                target.backend.write_uint32(syms[n], 1)
        target.backend.resume()

        print(f"  measuring {args.window:.1f}s")
        time.sleep(args.window)

        target.backend.halt()
        block = zonelib.read(target.backend, addr)
        counters = read_counters(target.backend, syms)
        hist = read_frame_hist(target.backend, syms)
        cyc_per_us = counters.get("x86_disk_cycles_per_us") or \
            (target.backend.read_uint32(syms["x86_disk_cycles_per_us"])
             if "x86_disk_cycles_per_us" in syms else 0)
        faults = read_faults(target.backend, out=lambda *a: None,
                             symbol_maps=maps)
        target.backend.resume()

        summary = zonelib.summarise(block, cycles_per_us=cyc_per_us or None)
        return {
            "block": block, "summary": summary, "counters": counters,
            "frame_hist": hist, "headroom": headroom,
            "cycles_per_us": cyc_per_us,
            "faults": {k: v for k, v in faults.items()
                       if isinstance(v, int)},
            "settle_s": settle, "window_s": args.window,
        }
    finally:
        target.stop()


# ---------------------------------------------------------------------------
# Pass 2: per-opcode cycles. Contributes a RANKING; its wall-clock shares are
# the instrumented build's, not the emulator's.
# ---------------------------------------------------------------------------
def pass_opcodes(args, wl):
    build_core(["X86_PROFILE=1"], wl["rom"], quiet=args.quiet)
    target, syms, maps = boot(args, wl["rom"])
    try:
        if wl.get("cmdtail") is not None and "guest_use_cmdtail" in syms:
            target.backend.write_uint32(syms["guest_use_cmdtail"],
                                        wl["cmdtail"])
        settle = args.settle if args.settle is not None else wl["settle"]
        print(f"  settling {settle:.1f}s (discarded)")
        time.sleep(settle)

        target.backend.halt()
        addr = prof.block_address(target.backend, syms)
        prof.reset(target.backend, addr)
        target.backend.resume()

        print(f"  measuring {args.window:.1f}s")
        time.sleep(args.window)

        target.backend.halt()
        profile = prof.read_profile(target.backend, addr)
        handlers = prof.resolve_handlers(target.backend, syms, maps,
                                         opsize=args.opsize)
        target.backend.resume()
        rows, total = prof.rank(profile, handlers, sort="total")
        return {"rows": rows, "total_cycles": total,
                "overhead_cycles_per_dispatch": profile["overhead"],
                "dispatches": sum(r["count"] for r in rows)}
    finally:
        target.stop()


# ---------------------------------------------------------------------------
# Pass 3: host PC histogram. OPTIONAL -- this instrument does not exist in
# every checkout, and a driver that dies because a sibling tool is absent is
# worse than one that reports its absence.
# ---------------------------------------------------------------------------
def pass_pcsample(args, wl):
    try:
        from scripts.common import pcsample as pcs      # noqa: F401
    except ImportError:
        return {"available": False,
                "note": "scripts/common/pcsample.py is not in this tree, so "
                        "no host-PC histogram was collected. The zone "
                        "breakdown answers WHICH PHASE; a PC histogram answers "
                        "WHICH FUNCTION inside it."}
    return {"available": False,
            "note": "pcsample module found but profile_all has no adapter for "
                    "it yet -- run scripts/pcsample.py directly."}


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
TRUST = {
    "gwemu": (
        "**TARGET IS gwemu. THE CYCLE AND MICROSECOND COLUMNS ARE NOT A "
        "MEASUREMENT OF THE DEVICE.**\n\n"
        "gwemu models no Cortex-M7 pipeline (`docs/06`) and gives the SD card "
        "no busy state (`docs/04`), so:\n\n"
        "* **COUNTS are exact** -- frames, occurrences per frame, dispatches, "
        "disk calls, cache hits. These are reproducible and worth diffing "
        "across builds.\n"
        "* **CYCLES are an artefact of the host.** In particular the `disk` "
        "zone reads as nearly free here and *cannot* read as anything else; on "
        "silicon it is a blocking SPI transfer.\n"
        "* The **shape** of the breakdown is a hypothesis to check on silicon, "
        "not a result.\n"),
    "hardware": (
        "**TARGET IS HARDWARE.** Counts and cycles are both real: DWT->CYCCNT "
        "counts core cycles including stalls, and the SD card really is busy. "
        "This is the tier whose answer to \"where did the time go\" is the "
        "device's answer.\n"),
}


def fmt_us(cycles, per_us):
    return f"{cycles / per_us:,.0f}" if per_us else "-"


def render_report(doc):
    L = []
    a = L.append
    meta = doc["meta"]
    target = "hardware" if meta["target"] == "hardware" else "gwemu"
    z = doc.get("zones") or {}
    summary = dict(z["totals"], rows=z["rows"]) if z.get("rows") else None
    per_us = z.get("cycles_per_us") or 0

    a(f"# Profile: {meta['workload']} on {meta['target']}")
    a("")
    a(f"Generated {meta['generated']} by `scripts/profile_all.py`. "
      f"Window {meta['window_s']:.0f}s after {meta['settle_s']:.0f}s settle.")
    a("")
    a(meta["workload_what"])
    a("")
    a(TRUST[target])
    a("")
    if meta.get("failed_passes"):
        a("> **THIS REPORT IS INCOMPLETE.** These passes did not produce data, "
          "so whatever they would have contributed is absent — not zero:")
        a("")
        for k, v in meta["failed_passes"].items():
            a(f"> * `{k}`: {v}")
        a("")

    # ---- lead with the answer -------------------------------------------
    a("## Where the time went")
    a("")
    if not summary:
        a("No zone data: this run had no `X86_ZONES=1` pass.")
    else:
        rows = summary["rows"]
        frames = summary["frames"]
        total = summary["total_cycles"]
        busy = summary["busy_cycles"]
        idle = total - busy
        a(f"**{frames:,} frames** measured, "
          f"{total:,} cycles of frame-loop wall time in total"
          + (f" ({total / per_us / 1000:,.0f} ms)." if per_us else "."))
        a("")
        a(f"**{100.0 * idle / total if total else 0:.1f}% of it was idle** "
          f"waiting for the panel swap -- that is the headroom. The remaining "
          f"{100.0 * busy / total if total else 0:.1f}% is work, and it "
          f"divides like this:")
        a("")
        top = [r for r in rows if r["id"] not in zonelib.IDLE_ZONES
               and r["id"] != zonelib.RESIDUAL][:4]
        for r in top:
            share = 100.0 * r["cycles"] / busy if busy else 0
            a(f"* **{r['name']} {share:.1f}% of the work** "
              f"({r['pct']:.1f}% of the frame) -- {r['what']}")
        a("")
        res = summary["residual_pct"]
        if res >= zonelib.RESIDUAL_LOUD_PCT:
            a(f"> **UNACCOUNTED: {res:.1f}% of frame time is in no named "
              f"zone.** That is above the {zonelib.RESIDUAL_LOUD_PCT:.0f}% "
              f"threshold and it is a finding, not noise: there is a phase in "
              f"the frame loop that nothing brackets. Find it before trusting "
              f"the shares above.")
        else:
            a(f"> Unaccounted residual **{res:.1f}%** of frame time -- inside "
              f"the threshold, i.e. the named zones really do partition the "
              f"frame. (The residual is the ROOT zone's own self-time, "
              f"measured, not a subtraction.)")
        a("")
        a(f"> Instrumentation cost: **{summary['overhead_cycles']:,} cycles — "
          f"{summary['overhead_pct']:.2f}% of frame time, "
          f"{summary['overhead_pct_busy']:.2f}% of the WORK** (the second "
          f"number is the one to beat: measuring against a mostly-idle frame "
          f"flatters the instrument). "
          f"{summary['transitions']:,} transitions x "
          f"{summary['overhead_cycles_per_pair']} calibrated cycles per "
          f"enter/exit pair). A LOWER bound -- the calibration loop is hot and "
          f"perfectly predicted; see `x86_zones_calibrate`.")
        if summary["overflow"] or summary["unbalanced"]:
            a("")
            a(f"> **The accounting is suspect:** stack overflows "
              f"{summary['overflow']}, unbalanced frames "
              f"{summary['unbalanced']}.")
        a("")

        # ---- detail ------------------------------------------------------
        a("## Zone breakdown")
        a("")
        a("Self-time, exclusive: while a nested zone is open its parent "
          "accumulates nothing (`x86_zones.h`). So `hle` excludes the `disk` "
          "time under it, and a parent's *total* cost is its own row plus its "
          "children's. `residual` is the root zone and needs no explanation "
          "beyond its name.")
        a("")
        head = "| zone | % frame | cycles | calls | calls/frame | mean cyc | worst cyc |"
        if per_us:
            head = ("| zone | % frame | cycles | us/frame | calls | calls/frame"
                    " | mean cyc | worst cyc | worst us |")
        a(head)
        a("|" + "---|" * (head.count("|") - 1))
        for r in rows:
            if per_us:
                a(f"| `{r['name']}` | {r['pct']:.2f} | {r['cycles']:,} | "
                  f"{r['us_per_frame']:,.1f} | {r['count']:,} | "
                  f"{r['calls_per_frame']:.2f} | {r['mean']:,.0f} | "
                  f"{r['worst']:,} | {r['us_worst']:,.0f} |")
            else:
                a(f"| `{r['name']}` | {r['pct']:.2f} | {r['cycles']:,} | "
                  f"{r['count']:,} | {r['calls_per_frame']:.2f} | "
                  f"{r['mean']:,.0f} | {r['worst']:,} |")
        a("")
        a("`worst` is the worst SINGLE occurrence. It matters more than the "
          "mean: a stutter is one long frame, and a mean cannot contain one.")
        a("")

    # ---- frame tail ------------------------------------------------------
    hist = z.get("frame_hist")
    counters = z.get("counters", {})
    if hist:
        a("## Frame-time tail")
        a("")
        a("Buckets are multiples of the LEARNED panel period "
          "(`panel_ms_min`), so bucket 0 is on time and every other bucket is "
          "a missed deadline.")
        a("")
        a("| bucket | period | frames |")
        a("|---|---|---|")
        for i, v in enumerate(hist):
            a(f"| {i} | {FRAME_HIST_EDGES[i]} | {v:,} |")
        a("")
        a(f"`frame_overruns` {counters.get('frame_overruns', '-')}, "
          f"worst {counters.get('frame_ms_worst', '-')} ms, "
          f"second worst {counters.get('frame_ms_worst2', '-')} ms, "
          f"panel period {counters.get('panel_ms_min', '-')} ms.")
        a("")

    # ---- opcodes ---------------------------------------------------------
    op = doc.get("opcodes")
    if op:
        a("## Hottest opcodes (separate pass, `X86_PROFILE=1`)")
        a("")
        a(f"{op['dispatches']:,} dispatches; calibrated instrumentation "
          f"overhead {op['overhead_cycles_per_dispatch']} cycles per dispatch. "
          "**This pass's build is not the build the zone timings came from** — "
          "the per-dispatch instrumentation more than doubles guest execution, "
          "so take the RANKING from here and the wall-clock shares from the "
          "zone pass.")
        a("")
        a("| op | mnemonic | handler | count | cycles | mean | mean-ovh | % |")
        a("|---|---|---|---|---|---|---|---|")
        for r in op["rows"][:20]:
            a(f"| `{r['label']}` | {r['mnemonic']} | `{r['handler']}` | "
              f"{r['count']:,} | {r['cycles']:,} | {r['mean']:.1f} | "
              f"{r['mean_corrected']:.1f} | {r['pct']:.2f} |")
        a("")

    # ---- pcsample --------------------------------------------------------
    pc = doc.get("pcsample")
    if pc and not pc.get("available"):
        a("## Host PC histogram")
        a("")
        a(pc["note"])
        a("")

    # ---- memory ----------------------------------------------------------
    hr = z.get("headroom")
    if hr and "headroom_bytes" in hr:
        a("## Memory headroom")
        a("")
        a("Running out of RAM_EMU is QUIET: `guest_ram` comes back NULL, "
          "`core_ready` stays 0 and the emulator never starts. The core's TEXT "
          "is in this region too, so code growth counts.")
        a("")
        a(f"* `__CORE_BSS_END__` **0x{hr['core_bss_end']:08x}**, region ends "
          f"0x{hr['ram_emu_end']:08x}")
        a(f"* {hr['region_left_bytes']:,} B left, guest asks "
          f"{hr['guest_request_bytes']:,} B "
          f"-> **headroom {hr['headroom_bytes']:,} B**")
        a("")
        a("Runtime backing flags (a 0 here is a silent allocation failure):")
        a("")
        for n in COUNTER_GROUPS[-1][1]:
            if n in counters:
                a(f"* `{n}` = {counters[n]}")
        a("")

    # ---- counters --------------------------------------------------------
    a("## Standing counters")
    a("")
    a("Cumulative since boot unless noted. `frame_*` were reset at the start "
      "of the window; everything else was not, so read them as totals.")
    a("")
    for title, names in COUNTER_GROUPS[:-1]:
        present = [(n, counters[n]) for n in names if n in counters]
        if not present:
            continue
        a(f"**{title}**")
        a("")
        a("| counter | value |")
        a("|---|---|")
        for n, v in present:
            a(f"| `{n}` | {v:,} |")
        a("")

    f = z.get("faults") or {}
    if f:
        dirty = f.get("CFSR") or f.get("HFSR")
        a("## Fault state")
        a("")
        a("**NOT CLEAN -- the numbers above are not a profile.**" if dirty
          else "Clean.")
        a("")
        a("| reg | value |")
        a("|---|---|")
        for k, v in f.items():
            a(f"| `{k}` | 0x{v:08x} |")
        a("")

    a("---")
    a("")
    a("How to read this, what each zone means and what is trustworthy per "
      "tier: `TOOLING.md`, section `profile_all.py`.")
    a("")
    return "\n".join(L)


def compare(a_path, b_path):
    with open(a_path) as fh:
        A = json.load(fh)
    with open(b_path) as fh:
        B = json.load(fh)
    L = []
    add = L.append
    add(f"# Compare: {os.path.basename(a_path)} -> {os.path.basename(b_path)}")
    add("")
    ta, tb = A["meta"]["target"], B["meta"]["target"]
    add(f"A: {A['meta']['workload']} on {ta}, window {A['meta']['window_s']}s")
    add(f"B: {B['meta']['workload']} on {tb}, window {B['meta']['window_s']}s")
    add("")
    if A["meta"]["workload"] != B["meta"]["workload"] or ta != tb:
        add("> **These runs are not comparable**: different workload or "
            "different target. A diff of them is arithmetic, not evidence.")
        add("")
    if ta != "hardware":
        add("> Under gwemu compare the COUNTS. The cycle deltas below are "
            "host artefacts (`docs/06`).")
        add("")

    za = {r["name"]: r for r in (A.get("zones", {}).get("rows") or [])}
    zb = {r["name"]: r for r in (B.get("zones", {}).get("rows") or [])}
    if za and zb:
        add("## Zones (per frame, so a different frame count is not a delta)")
        add("")
        add("| zone | A cyc/frame | B cyc/frame | delta | A calls/frame | "
            "B calls/frame |")
        add("|---|---|---|---|---|---|")
        for name in sorted(set(za) | set(zb),
                           key=lambda n: -(zb.get(n, {}).get("per_frame", 0))):
            ra, rb = za.get(name, {}), zb.get(name, {})
            pa = ra.get("per_frame", 0.0)
            pb = rb.get("per_frame", 0.0)
            d = pb - pa
            pct = (100.0 * d / pa) if pa else 0.0
            add(f"| `{name}` | {pa:,.0f} | {pb:,.0f} | {d:+,.0f} "
                f"({pct:+.1f}%) | {ra.get('calls_per_frame', 0):.2f} | "
                f"{rb.get('calls_per_frame', 0):.2f} |")
        add("")

    ca = A.get("zones", {}).get("counters", {})
    cb = B.get("zones", {}).get("counters", {})
    keys = [k for k in cb if k in ca and ca[k] != cb[k]]
    if keys:
        add("## Counters that moved")
        add("")
        add("| counter | A | B | delta |")
        add("|---|---|---|---|")
        for k in sorted(keys):
            add(f"| `{k}` | {ca[k]:,} | {cb[k]:,} | {cb[k] - ca[k]:+,} |")
        add("")

    ha = A.get("zones", {}).get("headroom", {})
    hb = B.get("zones", {}).get("headroom", {})
    if "headroom_bytes" in ha and "headroom_bytes" in hb:
        add("## Memory")
        add("")
        add(f"RAM_EMU headroom {ha['headroom_bytes']:,} B -> "
            f"{hb['headroom_bytes']:,} B "
            f"({hb['headroom_bytes'] - ha['headroom_bytes']:+,} B)")
        add("")
    return "\n".join(L)


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    add_target_args(p)
    p.add_argument("--workload", choices=sorted(WORKLOADS), default="alleycat",
                   help="named preset: a .dsk, its timeline and its settle "
                        "time, which have to agree with each other")
    p.add_argument("--rom", default=None,
                   help="a .dsk of your own instead of a preset")
    p.add_argument("--name", default=None, help="name for a --rom workload")
    p.add_argument("--passes", default="zones,opcodes",
                   help="comma-separated: zones,opcodes,pcsample. Each is its "
                        "OWN clean build -- see the module docstring for why "
                        "they are not merged into one run")
    p.add_argument("--settle", type=float, default=None,
                   help="override the preset's settle seconds")
    p.add_argument("--window", type=float, default=10.0,
                   help="measurement window, seconds")
    p.add_argument("--opsize", type=int, choices=[16, 32], default=16,
                   help="which pass's handler symbols to name in the opcode "
                        "table (real-mode DOS guests are 16)")
    p.add_argument("--firmware-dir", default=FW_DIR)
    p.add_argument("--sd", default="sdcard_retrogo.img")
    p.add_argument("--out", default=None, help="report path without extension")
    p.add_argument("--quiet", action="store_true",
                   help="do not echo build commands")
    p.add_argument("--compare", nargs=2, metavar=("A.json", "B.json"),
                   help="diff two reports and exit; runs nothing")
    args = p.parse_args()

    if args.compare:
        print(compare(*args.compare))
        return 0

    if args.rom:
        name = args.name or os.path.splitext(os.path.basename(args.rom))[0]
        wl = {"rom": args.rom, "timeline": "timelines/start-preselected-game.tl",
              "settle": 8.0, "cmdtail": 1,
              "what": f"Custom workload: `{args.rom}`."}
    else:
        name = args.workload
        wl = WORKLOADS[name]
    if args.timeline is None:
        args.timeline = wl["timeline"]

    # A run that outlives its timeline loses gwemu mid-read, and the dropped
    # GDB link reads as a target fault. Derive the budget; never hardcode it.
    settle = args.settle if args.settle is not None else wl["settle"]
    if args.target != "hardware" and args.timeline:
        quit_at = timeline_quit_time(args.timeline)
        if quit_at and settle + args.window > quit_at - 3:
            raise SystemExit(
                f"profile_all: settle + window ({settle + args.window:.0f}s) "
                f"outlives the timeline's quit at {quit_at:.0f}s "
                f"({args.timeline}). Shorten the window or pick a longer "
                f"timeline.")

    passes = [s.strip() for s in args.passes.split(",") if s.strip()]
    doc = {"meta": {
        "workload": name,
        "workload_what": wl["what"],
        "rom": wl["rom"],
        "timeline": args.timeline,
        "target": "hardware" if args.target == "hardware" else "gwemu",
        "settle_s": settle,
        "window_s": args.window,
        "passes": passes,
        "generated": datetime.datetime.now().isoformat(timespec="seconds"),
        "cycles_meaningful": args.target == "hardware",
    }}

    # A pass that fails must not throw away the passes that worked. Losing a
    # completed 10 s zone measurement because a later build's core did not come
    # up costs the whole run for no reason, and the report can simply say which
    # pass is missing and why.
    failures = {}

    def run_pass(label, fn):
        try:
            return fn(args, wl)
        except SystemExit as exc:
            failures[label] = str(exc)
        except Exception as exc:                          # noqa: BLE001
            failures[label] = f"{type(exc).__name__}: {exc}"
        print(f"  !! the {label} pass FAILED: {failures[label]}")
        return None

    if "zones" in passes:
        print("[pass] zones + counters (X86_ZONES=1)")
        r = run_pass("zones", pass_zones)
    else:
        r = None
    if r:
        # rows + totals, NOT the summary dict as well: the JSON is meant to be
        # diffed, and a document that carries the same rows twice diffs twice.
        doc["zones"] = {
            "rows": r["summary"]["rows"],
            "totals": {k: v for k, v in r["summary"].items() if k != "rows"},
            "counters": r["counters"],
            "frame_hist": r["frame_hist"],
            "headroom": r["headroom"],
            "cycles_per_us": r["cycles_per_us"],
            "faults": r["faults"],
        }
    if "opcodes" in passes:
        print("[pass] per-opcode cycles (X86_PROFILE=1)")
        doc["opcodes"] = run_pass("opcodes", pass_opcodes)
    if "pcsample" in passes:
        print("[pass] host PC histogram")
        doc["pcsample"] = run_pass("pcsample", pass_pcsample)
    doc["meta"]["failed_passes"] = failures

    base = args.out or os.path.join(
        OUT_DIR, f"profile_{name}_{doc['meta']['target']}")
    os.makedirs(os.path.dirname(base) or ".", exist_ok=True)
    md = render_report(doc)
    with open(base + ".md", "w") as fh:
        fh.write(md)
    with open(base + ".json", "w") as fh:
        json.dump(doc, fh, indent=1, sort_keys=True)
        fh.write("\n")
    print("")
    print(md)
    print(f"\nwrote {base}.md and {base}.json")
    # The report is written either way -- but a run with a missing pass must
    # not exit 0, or CI and the shell both read "incomplete" as "fine".
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
