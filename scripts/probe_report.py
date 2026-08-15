#!/usr/bin/env python3
"""
One command, one title: every service and port it probed, and HOW WE ANSWERED.

    python3 scripts/probe_report.py --title keen4
    python3 scripts/probe_report.py --title wolf3d --settle 20
    python3 scripts/probe_report.py --rom path/to/GAME.dsk --name mygame
    python3 scripts/probe_report.py --compare before.json after.json

------------------------------------------------------------------------------
THE QUESTION THIS ANSWERS
------------------------------------------------------------------------------
"What is this title asking for that we are not answering?" -- without reading
trace rings and guessing. The report LEADS with the unanswered and the defaulted
probes, because those are the only actionable ones. A title that makes 400 calls
should produce a five-line "here is what we did not answer" section at the top
and put the 40 rows of things that worked underneath.

The distinction it exists to make is between "we answered" and "we answered
correctly-shaped nonsense". INT 21h AH=44h with AL>=2 returns CF=1 AX=0001
"invalid function" from a catch-all arm; a real handler returning the same thing
is indistinguishable in every existing trace in this tree. That cost one
investigation hours. Here it is a column.

------------------------------------------------------------------------------
WHAT IT DRIVES -- IT REIMPLEMENTS NOTHING
------------------------------------------------------------------------------
    x86_probe.h / x86_probe.c        the on-target accumulator (X86_PROBE=1)
    scripts/common/probe_acc.py      decode + the runtime/table join
    scripts/common/probe_expect.py   the curated "what SHOULD we return" table
    scripts/common/target.py         gwemu or hardware, same call

The existing rings are untouched and still the right tool for ORDER -- an
accumulator throws sequence away. `probe.py --dos-log` and `--io-log` remain
what you reach for when the question is "in what sequence"; this is for "what,
and did we answer it".

------------------------------------------------------------------------------
COST
------------------------------------------------------------------------------
X86_PROBE=1 is OFF by default and every report states its measured link delta.
On this tree it is +2,848 B of RAM_EMU .bss (the accumulator) and +696 B of
text, and NOTHING in the packed image's .data -- the header is stamped on first
use precisely so the block does not also occupy dos.bin. It is not on the guest
instruction dispatch path: one hash probe per INT service and per port access,
both of which already cost hundreds of cycles.
"""

import argparse
import datetime
import json
import os
import subprocess
import sys
import time

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from scripts.common import probe_acc as acc                          # noqa: E402
from scripts.common import probe_expect as expect                    # noqa: E402
from scripts.common.probe import load_symbol_maps, wait_for_value    # noqa: E402
from scripts.common.target import (Image, add_target_args,           # noqa: E402
                                   open_target, timeline_quit_time)

FW_DIR = "references/game-and-watch-retro-go-sd/build"
ROM_DIR = "references/game-and-watch-retro-go-sd/roms/dos"
CORE_MAGIC = 0x54494E59
CORE_ELF = "build/core/dos_core.elf"
OUT_DIR = "build/artifacts"

# ---------------------------------------------------------------------------
# Presets. A title is a .dsk plus the timeline that gets it running and the
# settle time it needs -- the three things that have to agree, which is why
# this is a named preset and not three flags a caller has to get right.
#
# NOTE the settle times are LONGER here than in profile_all.py's equivalents and
# that is deliberate: a profile wants a steady state, this wants the STARTUP
# probes, which is where a title asks its detection questions. The accumulator
# never evicts, so a long window costs nothing -- unlike every ring in this tree
# it cannot lose the early entries to later noise.
# ---------------------------------------------------------------------------
TITLES = {
    "keen4": {
        "rom": f"{ROM_DIR}/KEEN4.dsk",
        "timeline": "timelines/start-preselected-game.tl",
        "settle": 22.0, "cmdtail": 1,
        "what": "Commander Keen 4: EGA-only. Its video detection cascade is "
                "the interesting part -- INT 10h AH=12h, then mode 0Dh.",
    },
    "wolf3d": {
        "rom": f"{ROM_DIR}/WOLF3D.dsk",
        "timeline": "timelines/start-preselected-game.tl",
        "settle": 22.0, "cmdtail": 1,
        "what": "Wolfenstein 3D: a long detection preamble (video, sound, "
                "mouse, joystick) before it opens a single data file.",
    },
    "topbench": {
        "rom": f"{ROM_DIR}/TOPBENCH.dsk",
        "timeline": "timelines/start-preselected-game.tl",
        "settle": 12.0, "cmdtail": 0,
        "what": "TOPBENCH's menu: CGA text mode 3, a database read, an INT 16h "
                "poll loop. It probes the machine deliberately and by design.",
    },
    "darkforces": {
        "rom": f"{ROM_DIR}/STAR_WARS_DARKFORCES.dsk",
        "timeline": "timelines/start-preselected-game.tl",
        "settle": 28.0, "cmdtail": 1,
        "what": "Star Wars: Dark Forces. A DOS/4GW title -- the question is "
                "which memory manager it asks for (XMS, EMS, VCPI, DPMI) "
                "before it refuses to start.",
    },
    "mortalkombat": {
        "rom": f"{ROM_DIR}/MORTAL_KOMBAT.dsk",
        "timeline": "timelines/start-preselected-game.tl",
        "settle": 22.0, "cmdtail": 1,
        "what": "Mortal Kombat. Its C-runtime startup probes the machine "
                "before it draws anything -- SMSW, the x87 detection, CPUID -- "
                "and then runs the classic A20 WRAP TEST (REPE CMPSW at "
                "FFFF:0030 against 0000:0020, linear 0x100020). On a 286+ with "
                "the gate open the two locations must DIFFER, which needs the "
                "HMA to be backed rather than merely unmapped. It had a preset "
                "added when that landed, because a title diagnosed once by hand "
                "gets re-measured badly.",
    },
    "epicpinball": {
        "rom": f"{ROM_DIR}/EPIC_PINBALL.dsk",
        "timeline": "timelines/start-preselected-game.tl",
        "settle": 22.0, "cmdtail": 1,
        "what": "Epic Pinball. PINBALL.EXE is a LAUNCHER: it shrinks its own "
                "block (16 x AH=4Ah) and EXECs the chosen table's EPn.EXE. It "
                "used to stop dead on INT 21h AH=4Bh with 'Please run SETUP!'; "
                "since X86_DOS_EXEC became the default the EXEC is ANSWERED "
                "(CF=0) and the child lands whole -- 83,912 B, "
                "x86_dos_load_short=0, x86_pg_exhausted=0, evictions=0. It now "
                "reaches mode 13h and dies past it on "
                "X86_EXIT_UNIMPLEMENTED. Its blocker is a plain missing "
                "two-byte OPCODE and has nothing to do with the other two "
                "titles that were once reported as sharing its exit: 0F AF "
                "(IMUL Gv,Ev) is now built, and it stops on 0F A0 (PUSH FS) "
                "next. Read cpu_exit_info, which names the opcode; the bare "
                "0x0F it used to report was the 0F ESCAPE. docs/13.",
    },
    "alleycat": {
        "rom": f"{ROM_DIR}/ALLEYCAT.dsk",
        "timeline": "timelines/profile-alleycat.tl",
        "settle": 28.0, "cmdtail": 1,
        "what": "Alley Cat past its joystick prompt and skill menu: CGA "
                "graphics mode 4 plus the PC speaker.",
    },
}


def sh(cmd, quiet=False):
    if not quiet:
        print(f"  $ {' '.join(cmd)}")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout[-4000:])
        sys.stderr.write(r.stderr[-4000:])
        raise SystemExit(f"probe_report: command failed: {' '.join(cmd)}")
    return r.stdout


def nm_syms(elf):
    out = sh(["arm-none-eabi-nm", "-S", elf], quiet=True)
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 4:
            syms[p[-1]] = (int(p[0], 16), int(p[1], 16))
        elif len(p) >= 3:
            syms[p[-1]] = (int(p[0], 16), 0)
    return syms


def build_core(rom, probe, quiet=False, core_flags=()):
    """Clean-build with X86_PROBE on or off, and lay the SD image down.

    CLEAN, always: build flags are not dependencies in this project, so an
    incremental rebuild after a flag change links objects from two different
    configurations and the report describes neither.

    core_flags are extra `NAME=VALUE` arguments passed straight to
    Makefile.core. The reason this exists is that the interesting question is
    often "what does the title ask for once we ANSWER something we currently
    refuse" -- Dark Forces' diagnosis needed X86_DOS_EXEC=1 back when that was
    0 in the shipping core (it is 1 now) -- and that cannot be measured by a
    runner that can only ever build the default configuration. It is also how
    the OPPOSITE question gets asked: --core-flag X86_DOS_EXEC=0 is what
    measured EPIC_PINBALL's before-and-after across the flip. Every flag is
    echoed and recorded in
    the report, because a probe of a non-default core that does not say so is
    a number nobody can place.
    """
    flags = [f"X86_PROBE={1 if probe else 0}"] + list(core_flags)
    print(f"[build] {' '.join(flags)}")
    sh(["make", "-f", "Makefile.core", "clean"], quiet=quiet)
    sh(["make", "-f", "Makefile.core", "pack"] + flags, quiet=quiet)
    if rom:
        sh(["python3", "scripts/sdcard.py", "--image", "sdcard_retrogo.img",
            "--rom", rom], quiet=quiet)
    s = nm_syms(CORE_ELF)
    end = s.get("__CORE_BSS_END__", (0, 0))[0]
    ram_end = s.get("__RAM_EMU_END__", (0, 0))[0]
    return {"core_bss_end": end, "ram_emu_end": ram_end,
            "region_left_bytes": (ram_end - end) if (end and ram_end) else None,
            "probe_block_bytes": s.get("x86_probe", (0, 0))[1],
            "dos_bin_bytes": (os.path.getsize("build/core/dos.bin")
                              if os.path.exists("build/core/dos.bin") else
                              os.path.getsize("dos.bin")
                              if os.path.exists("dos.bin") else 0)}


def measure_cost(rom, quiet, core_flags=()):
    """Build BOTH ways and report the difference.

    A claimed cost is not a measured one, and RAM_EMU headroom in this tree has
    gone negative silently more than once. Two clean builds is the only way to
    get a number that is not an estimate; --skip-cost exists for when the
    caller has already paid for it.
    """
    off = build_core(rom, probe=False, quiet=quiet, core_flags=core_flags)
    on = build_core(rom, probe=True, quiet=quiet, core_flags=core_flags)
    return {
        "off": off, "on": on,
        "ram_emu_delta_bytes": (off["region_left_bytes"] -
                                on["region_left_bytes"])
        if off["region_left_bytes"] is not None else None,
        "dos_bin_delta_bytes": on["dos_bin_bytes"] - off["dos_bin_bytes"],
        "accumulator_bytes": on["probe_block_bytes"],
    }


def boot(args, rom):
    fw = args.firmware_dir
    image = Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                  bank2=os.path.join(fw, "qemu_bank2.bin"),
                  extflash=os.path.join(fw, "extflash.bin"),
                  sdcard=args.sd, elf=CORE_ELF, intflash_bank=1,
                  intflash=os.path.join(fw, "gw_retro_go_intflash.bin"))
    target = open_target(args, image)
    target.start()
    syms, maps = load_symbol_maps(os.path.join(fw, "gw_retro_go.elf"), CORE_ELF)
    if "core_magic" in syms:
        print("  waiting for core_magic...")
        if not wait_for_value(target.backend, syms["core_magic"], CORE_MAGIC,
                              40.0):
            target.stop()
            raise SystemExit("probe_report: the core never started; there is "
                             "nothing to report. Check with scripts/probe.py.")
        print("  core running")
    return target, syms, maps


# Counters worth carrying so a report is self-contained about whether the run
# was healthy. A probe table from a guest that never got going is a table of
# nothing, and that must be visible in the report rather than inferred.
CONTEXT = ["core_ready", "core_dtcm_ok", "hle_calls_seen", "x86_hle_calls",
           "hle_bad_vector", "hle_bad_func", "guest_ips_achieved",
           "guest_insns_total", "frames_rendered", "x86_disk_reads",
           "x86_vga13_ok", "vga_aperture_ok", "x86_vga_dac_writes",
           "guest_exit_reason", "guest_halted", "x86_dos_file_calls",
           # The guest's CLOCK, which is a different question from its speed
           # and has its own failure mode: a title that programs PIT channel 0
           # and does not get that rate has every timed thing it does slowed by
           # the ratio. guest_ticks/guest_uptime_ms must read back as
           # 1193182/guest_tick_divisor.
           "guest_tick_divisor", "x86_pit0_writes", "guest_ticks",
           "guest_ticks_dropped", "guest_tick_irq_lost", "guest_uptime_ms",
           # THE LIVENESS SET. Everything above describes what the guest did;
           # these four say whether there WAS a guest. x86_pg_frames_total is
           # the one that catches this instrument killing its own subject: the
           # accumulator's .bss comes out of RAM_EMU and guest_ram is the last
           # claim in that pool, so an instrumented build can hand the pager
           # zero frames. guest_ram_pages_short is the target's own alarm for
           # a pool that was shortened rather than refused.
           "x86_pg_frames_total", "guest_ram_pages", "guest_ram_pages_short",
           "guest_ram_free_at_claim",
           # SOUND, and it needs its own set because the port table cannot tell
           # "programmed a transfer" from "played one". x86_sb_starts vs
           # x86_sb_samples is exactly that difference; x86_sb_irqs vs
           # _delivered separates "the card asked" from "the guest was given
           # it", which is the hang this path exists to remove; and
           # x86_dos_env_seg says whether BLASTER was reachable at all.
           "x86_dos_env_seg", "x86_sb_resets", "x86_sb_cmds",
           "x86_sb_cmds_unknown", "x86_sb_dma_refused", "x86_sb_starts",
           "x86_sb_blocks", "x86_sb_dma_bytes", "x86_sb_samples",
           "x86_sb_rate", "x86_sb_irqs", "x86_sb_irqs_delivered",
           "x86_sb_irqs_masked", "x86_sb_irqs_acked", "x86_sb_underruns",
           "x86_sb_adpcm_silenced", "x86_opl_timer_expiry",
           "x86_opl_t1_gap", "x86_io_isa_ticks"]

# How long to watch guest_insns_total to decide it is ADVANCING. A guest that
# halted at instruction 1 has a non-zero count forever, so a single sample
# cannot tell "running" from "died after the first instruction" -- and that is
# exactly the state this guard exists to catch.
LIVENESS_WINDOW_S = 1.5


# THE DEATH SITE. `guest_exit_reason` says the guest stopped; it does not say
# WHERE or ON WHAT, and every one of those questions used to need a separate
# hand-rolled probe.py invocation against offsets copied out of x86_offsets.inc
# by hand. Three titles were reported as "converging on one address" partly
# because nobody could cheaply read the rest of the state next to it.
#
# These come from cpu_state_ptr, which is a POINTER to the DTCM context, so the
# read is a deref and not a symbol read. Offsets are x86_offsets.inc; they are
# asserted against it by tests/test_probe_offsets.py (8 checks, no target needed) so a struct change breaks
# the test rather than silently reporting a neighbouring field.
#
# cpu_exit_info is the one to read first on guest_exit_reason=4: >= 0x100 means
# a TWO-BYTE opcode and the low byte is the second byte (0x0F is the escape,
# not the instruction). cpu_cr0 bit 0 answers "did it enter protected mode"
# without a build flag.
CPU_FIELDS = {
    "cpu_eip":         0,
    "cpu_esp":        24,
    "cpu_cs_sel":     60,      # 16-bit
    "cpu_cs_base":    64,
    "cpu_ss_sel":     72,      # 16-bit
    "cpu_cr0":       120,
    "cpu_exit_reason": 2196,
    "cpu_exit_info":  2200,
}
CPU_FIELDS_16 = ("cpu_cs_sel", "cpu_ss_sel")


def _read_death_site(backend, syms, ctx):
    if "cpu_state_ptr" not in syms:
        return
    try:
        base = backend.read_uint32(syms["cpu_state_ptr"])
    except Exception:
        return
    if not base:
        return
    for name, off in CPU_FIELDS.items():
        try:
            v = backend.read_uint32(base + off)
        except Exception:
            continue
        ctx[name] = (v & 0xFFFF) if name in CPU_FIELDS_16 else v
    if "cpu_cs_base" in ctx and "cpu_eip" in ctx:
        ctx["cpu_lin_eip"] = (ctx["cpu_cs_base"] + ctx["cpu_eip"]) & 0xFFFFFFFF


def health(ctx, blk):
    """-> (fatal, stopped). Fatal reasons refuse the report outright; stopped
    reasons band it and still print, because the table is the evidence.

    THE TWO STATES ARE NOT THE SAME AND CONFLATING THEM COSTS EITHER WAY.

    A guest that never ran has an EMPTY accumulator, and printing its table is
    what produced "every service this title touched was answered" about Dark
    Forces -- true, worthless, and read as a clean bill of health. That is a
    refusal.

    A guest that ran, asked 67 things and then latched on an unimplemented
    opcode has a FULL accumulator and guest_insns_total = 0 all the same (the
    counter only accrues while !guest_halted, src/main.c). Refusing there
    throws away the only record of what it asked for before it died -- which is
    precisely what the tool is for. That is a banner, not a refusal.

    Either way the exit status is non-zero: neither is a healthy run.
    """
    bad = []
    stopped = []
    if not ctx.get("core_ready"):
        bad.append(f"`core_ready` = {ctx.get('core_ready')} — the core never "
                   f"finished coming up. `core_dtcm_ok`="
                   f"{ctx.get('core_dtcm_ok')}, `guest_ram_pages`="
                   f"{ctx.get('guest_ram_pages')}.")
    if ctx.get("x86_pg_frames_total") == 0:
        bad.append("`x86_pg_frames_total` = 0 — the demand pager owns NO "
                   "frames, so every guest access is X86_EXIT_MEM_FAULT. This "
                   "is the RAM_EMU exhaustion signature.")
    if not ctx.get("guest_insns_delta"):
        why = (f"`guest_insns_total` = {ctx.get('guest_insns_total', 0):,} and "
               f"did not move over {LIVENESS_WINDOW_S:.1f}s — THE GUEST IS "
               f"STOPPED (`guest_exit_reason`="
               f"{ctx.get('guest_exit_reason')}, `guest_halted`="
               f"{ctx.get('guest_halted')}). Note that counter only accrues "
               f"while the guest is not halted, so a title that died inside "
               f"its first frame reads 0 however much it did first.")
        # Did it ask anything before it stopped? That is what decides whether
        # there is a report to write at all.
        if blk.get("calls"):
            stopped.append(why + f" It recorded {blk['calls']:,} events across "
                                 f"{blk.get('used', 0)} probes BEFORE it "
                                 f"stopped, and those are real.")
        else:
            bad.append(why + " The accumulator is EMPTY, so it stopped before "
                             "asking for anything: there is nothing to report.")
    return bad, stopped


def render_void(doc, bad):
    """The whole report when the run cannot support one. Deliberately short:
    there is one finding, and burying it under 200 rows of probe table is how
    this failed silently for three investigations."""
    m = doc["meta"]
    ctx = doc["context"]
    L = [f"# Probe report: {m['title']} on {m['target']} — **REFUSED**", "",
         f"Generated {m['generated']} by `scripts/probe_report.py`.", "",
         "## THIS RUN CANNOT BE REPORTED ON. THE GUEST DID NOT RUN.", "",
         "No probe table is printed, because one would be evidence about "
         "nothing. An empty \"what we did NOT answer\" section from a dead "
         "guest means nobody asked anything — not that we answered "
         "everything.", ""]
    for b in bad:
        L.append(f"* {b}")
    L += ["",
          "### First suspect: `X86_PROBE=1` itself", "",
          "The accumulator is ~2.8 KB of RAM_EMU `.bss`, and `guest_ram` is "
          "the LAST claim out of that pool. The core now sizes that claim to "
          "the free space instead of taking it all-or-nothing, so a squeezed "
          "pool shows as `guest_ram_pages_short` > 0 rather than as zero "
          "frames — check it below. If it IS zero and the guest still did not "
          "run, the cause is elsewhere: `scripts/probe.py --watch` the "
          "counters, and re-measure with the rings (`--dos-log`, `--io-log`) "
          "against a build WITHOUT the accumulator.", "",
          "| counter | value |", "|---|---:|"]
    for k, v in sorted(ctx.items()):
        L.append(f"| `{k}` | {v:,} |")
    L.append("")
    return "\n".join(L)


def collect(args, title):
    target, syms, maps = boot(args, title["rom"])
    try:
        if title.get("cmdtail") is not None and "guest_use_cmdtail" in syms:
            target.backend.write_uint32(syms["guest_use_cmdtail"],
                                        title["cmdtail"])
        settle = args.settle if args.settle is not None else title["settle"]
        # NOT reset after the settle. The startup probes ARE the subject: a
        # title asks its detection questions in the first second and never
        # again, so a window that opens later shows a title that has already
        # given up. The accumulator never evicts, so there is no reason to.
        print(f"  running {settle:.1f}s (probes from the FIRST instruction)")
        time.sleep(settle)

        # LIVENESS, before anything else is read. Two samples of
        # guest_insns_total across a wall-clock window: a guest that faulted at
        # its first instruction has a non-zero count that never moves, and a
        # single sample cannot tell that from a healthy run.
        insns0 = insns1 = None
        if "guest_insns_total" in syms:
            insns0 = target.backend.read_uint32(syms["guest_insns_total"])
            time.sleep(LIVENESS_WINDOW_S)
            insns1 = target.backend.read_uint32(syms["guest_insns_total"])

        target.backend.halt()
        addr = acc.block_address(target.backend, syms)
        block = acc.read(target.backend, addr)
        ctx = {}
        for n in CONTEXT:
            if n in syms:
                try:
                    ctx[n] = target.backend.read_uint32(syms[n])
                except Exception:
                    pass
        if insns0 is not None:
            ctx["guest_insns_delta"] = (insns1 - insns0) & 0xFFFFFFFF
        _read_death_site(target.backend, syms, ctx)
        target.backend.resume()
        return {"block": block, "context": ctx,
                "run_s": settle + (LIVENESS_WINDOW_S if insns0 is not None
                                   else 0.0)}
    finally:
        target.stop()


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
TRUST = (
    "**Target is gwemu.** COUNTS here are exact and reproducible -- how many "
    "times a service was called, which ports were touched, what was returned. "
    "That is all this report claims; it makes no timing claim, so `docs/06`'s "
    "missing pipeline model and `docs/04`'s free SD do not weaken it. What "
    "gwemu CANNOT show is a probe that only a real peripheral would provoke.")

TRUST_HW = (
    "**Target is hardware.** Everything here is the device's answer.")


def render(doc):
    L = []
    a = L.append
    m = doc["meta"]
    rows = doc["rows"]
    blk = doc["block_meta"]
    ctx = doc.get("context", {})

    a(f"# Probe report: {m['title']} on {m['target']}")
    a("")
    a(f"Generated {m['generated']} by `scripts/probe_report.py`. "
      f"Guest ran {m['run_s']:.0f}s from its first instruction.")
    a("")
    a(m["title_what"])
    a("")
    a(TRUST_HW if m["target"] == "hardware" else TRUST)
    a("")

    # ---- THE CORE HAS TO HAVE RUN, and X86_PROBE=1 is why it might not -----
    #
    # X86_PROBE=1 costs ~3.5 KB of RAM_EMU, and RAM_EMU's last claim is
    # guest_ram -- the pool the demand pager lives in. When that claim fails,
    # ram_malloc() returns NULL, x86_pg_frames_total is 0, EVERY guest access
    # is X86_EXIT_MEM_FAULT and the guest halts before its first instruction.
    # The core still sets core_dtcm_ok, still opens the pagefile and still
    # advertises XMS, so a reader skimming the context table sees a healthy
    # machine. This report then printed "Nothing. Every service and port this
    # title touched was answered", which is TRUE and completely misleading.
    # Measured on Dark Forces and reproduced on WOLF3D as a control.
    # The liveness banner that used to live here is now a REFUSAL: health()
    # gates render() entirely and render_void() is what a dead run produces.
    # One source of truth -- a banner above a full table was still read as a
    # report, which is how this failed three times.
    if doc.get("stopped"):
        a("> # THE GUEST IS STOPPED. THIS IS A POST-MORTEM.")
        a(">")
        for s in doc["stopped"]:
            a(f"> {s}")
        a(">")
        a("> Everything below is what it asked for BEFORE it stopped, which is "
          "the useful part -- but it is not a picture of the title running, "
          "and the last rows are where it died, not where it settled.")
        a("")

    if ctx.get("guest_ram_pages_short"):
        a(f"> **THE GUEST RAM POOL IS {ctx['guest_ram_pages_short']} FRAMES "
          f"SHORT** ({ctx.get('guest_ram_pages')} of 160). RAM_EMU could not "
          f"fund the whole pool -- the accumulator's own `.bss` is the usual "
          f"reason -- so this guest pages more than the shipping build does. "
          f"The COUNTS below are still the guest's own; anything about "
          f"TIMING or memory pressure is not comparable to a shipping run.")
        a("")

    if blk.get("never_ran"):
        a("> **THE ACCUMULATOR WAS NEVER TOUCHED.** The build has "
          "`X86_PROBE=1` and the block exists, but no INT service and no port "
          "access ever reached it. The guest did not get as far as asking for "
          "anything -- that is the finding. Check `core_ready` and "
          "`guest_insns_total` below, then `scripts/probe.py`.")
        a("")

    actionable = [r for r in rows if r["actionable"]]

    # ---- lead with the answer -------------------------------------------
    a("## What we did NOT answer")
    a("")
    if not actionable and doc.get("stopped"):
        a("Nothing — of what it ASKED. Read that against the banner above: "
          "this guest stopped, so it died on something it did not ask us for "
          "at all (an unimplemented opcode, a fault, a wall it hit directly). "
          "The absence of an unanswered probe is not the absence of a "
          "problem here; it narrows where the problem is.")
    elif not actionable:
        a("Nothing. Every service and port this title touched was answered by "
          "a handler the expectations table has vetted. That is a strong "
          "statement and it is worth distrusting once: check that the run "
          "actually reached the title (`guest_insns_total` below) and that the "
          "settle was long enough to include its detection preamble.")
    else:
        a(f"{len(actionable)} of {len(rows)} distinct probes. Worst first.")
        a("")
        for r in actionable[:12]:
            a(f"* **{r['name']} — `{r['status']}`** ×{r['count']:,} — "
              f"we returned {r['answer']}")
            a(f"  {r['note']}")
        if len(actionable) > 12:
            a("")
            a(f"...and {len(actionable) - 12} more in the full table below.")
    a("")

    # ---- the second-most actionable thing, and it is NOT a bug ------------
    absent = [r for r in rows if r["status"] == "absent-by-design"]
    if absent:
        a("## Hardware we told this title is not here")
        a("")
        a("These answers are CORRECT for the machine we claim to be, so they "
          "are not defects and are not ranked above. They are listed second "
          "because a title that stops working after one of them has not hit a "
          "bug -- it has been told no, and believed us. That is the most "
          "common way a title dies here and the hardest to see in a trace.")
        a("")
        for r in absent:
            a(f"* **{r['name']}** ×{r['count']:,} — we returned {r['answer']}")
            a(f"  {r['note']}")
        a("")

    if blk.get("dropped"):
        a(f"> **{blk['dropped']:,} probes were DROPPED**: the accumulator's "
          f"{blk.get('ents', '?')} entries were all claimed. The table below "
          f"is not the whole conversation. Rebuild with a larger "
          f"`X86_PROBE_ENTS` (each entry is 16 B of RAM_EMU).")
        a("")
    sat = [r for r in rows if r["saturated"]]
    if sat:
        a(f"> {len(sat)} row(s) hit the 32-bit count ceiling and stopped "
          f"counting: {', '.join(r['name'] for r in sat)}. The counts for "
          f"those are a floor.")
        a("")

    # ---- the full table --------------------------------------------------
    a("## Every service")
    a("")
    a("| service | calls | status | how we answered | source |")
    a("|---|---:|---|---|---|")
    for r in rows:
        if r["kind"] != "INT":
            continue
        a(f"| `{r['name']}` | {r['count']:,} | **{r['status']}** | "
          f"{r['answer']} | {r['source']} |")
    a("")
    a("Notes, in the same order:")
    a("")
    for r in rows:
        if r["kind"] == "INT":
            a(f"* `{r['name']}` — {r['note']}")
    a("")
    a(f"> {expect.NOTE_INT15}")
    a("")

    a("## Every port")
    a("")
    a("| port | dir | accesses | status | value | source |")
    a("|---|---|---:|---|---|---|")
    for r in rows:
        if r["kind"] == "INT":
            continue
        a(f"| `{r['id']:#06x}` | {r['kind']} | {r['count']:,} | "
          f"**{r['status']}** | {r['answer']} | {r['source']} |")
    a("")
    a("Notes, in the same order:")
    a("")
    for r in rows:
        if r["kind"] != "INT":
            a(f"* `{r['id']:#06x}` {r['kind']} — {r['note']}")
    a("")

    # ---- was the run healthy --------------------------------------------
    a("## Was this run healthy")
    a("")
    a(f"{blk.get('calls', 0):,} recorded events across "
      f"{blk.get('used', 0)} distinct probes "
      f"(table capacity {blk.get('ents', '?')}).")
    a("")
    a("| counter | value |")
    a("|---|---:|")
    for k, v in sorted(doc["context"].items()):
        a(f"| `{k}` | {v:,} |")
    a("")

    # ---- what it cost ----------------------------------------------------
    c = doc.get("cost")
    a("## What the instrument cost")
    a("")
    if not c:
        a("Not measured this run (`--skip-cost`). The instrument is off by "
          "default; a build without `X86_PROBE=1` carries none of it.")
    else:
        a(f"Measured, by building the core BOTH ways in this run -- not "
          f"estimated:")
        a("")
        a(f"* **RAM_EMU: {c['ram_emu_delta_bytes']:+,} B.** "
          f"`__CORE_BSS_END__` moved from "
          f"{c['off']['core_bss_end']:#010x} to "
          f"{c['on']['core_bss_end']:#010x}; the region has "
          f"{c['on']['region_left_bytes']:,} B left after the core with the "
          f"instrument in.")
        a(f"* **The accumulator itself is {c['accumulator_bytes']:,} B** of "
          f"`.bss` — deliberately uninitialised, so it does not also occupy "
          f"the packed image.")
        a(f"* **dos.bin: {c['dos_bin_delta_bytes']:+,} B** (code only).")
        a("* Not on the guest instruction dispatch path: one hash probe per "
          "INT service and per port access.")
    a("")
    a("## How a status is decided")
    a("")
    a("Runtime evidence outranks the table. A row is `unanswered` because the "
      "TARGET observed `rc==0` or saw `x86_hle_unhandled_*` move, and "
      "`unmodelled-port` because the access fell off the end of the switch in "
      "`x86_io.c` — neither can drift. The table in "
      "`scripts/common/probe_expect.py` only classifies calls the target says "
      "were answered, because that is exactly the case the target cannot see "
      "the difference in. Anything the table does not cover is `unclassified` "
      "and ranked HIGH, never quietly `implemented`.")
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
    add(f"A: {A['meta']['title']} on {A['meta']['target']}, "
        f"{A['meta']['run_s']:.0f}s")
    add(f"B: {B['meta']['title']} on {B['meta']['target']}, "
        f"{B['meta']['run_s']:.0f}s")
    add("")
    if A["meta"]["title"] != B["meta"]["title"]:
        add("> **These runs are not comparable**: different title. A diff of "
            "them is arithmetic, not evidence.")
        add("")

    ra = {r["name"]: r for r in A["rows"]}
    rb = {r["name"]: r for r in B["rows"]}

    fixed = [n for n in rb
             if n in ra and ra[n]["actionable"] and not rb[n]["actionable"]]
    broke = [n for n in rb
             if n in ra and not ra[n]["actionable"] and rb[n]["actionable"]]
    changed = [n for n in rb if n in ra
               and ra[n]["status"] != rb[n]["status"]
               and n not in fixed and n not in broke]
    gone = sorted(set(ra) - set(rb))
    new = sorted(set(rb) - set(ra))

    add("## Answers that changed")
    add("")
    if not (fixed or broke or changed):
        add("None. Every probe both runs made got the same class of answer. "
            "If a fix was expected to show here and did not, the fix did not "
            "reach a path this title exercises.")
    for n in fixed:
        add(f"* **FIXED** `{n}`: `{ra[n]['status']}` -> `{rb[n]['status']}` "
            f"— now {rb[n]['answer']}")
    for n in broke:
        add(f"* **REGRESSED** `{n}`: `{ra[n]['status']}` -> "
            f"`{rb[n]['status']}` — now {rb[n]['answer']}")
    for n in changed:
        add(f"* `{n}`: `{ra[n]['status']}` -> `{rb[n]['status']}`")
    add("")

    if new or gone:
        add("## Probes that appeared or vanished")
        add("")
        add("A probe that VANISHED usually means the title took a different "
            "branch, not that it stopped caring — read it as a change of "
            "path, not a fix.")
        add("")
        for n in new:
            add(f"* **new** `{n}` ×{rb[n]['count']:,} — `{rb[n]['status']}`")
        for n in gone:
            add(f"* **gone** `{n}` (was ×{ra[n]['count']:,}, "
                f"`{ra[n]['status']}`)")
        add("")

    moved = [(n, ra[n]["count"], rb[n]["count"]) for n in rb
             if n in ra and ra[n]["count"] != rb[n]["count"]]
    if moved:
        add("## Counts that moved")
        add("")
        add("| probe | A | B | delta |")
        add("|---|---:|---:|---:|")
        for n, x, y in sorted(moved, key=lambda t: -abs(t[2] - t[1]))[:30]:
            add(f"| `{n}` | {x:,} | {y:,} | {y - x:+,} |")
        add("")
    return "\n".join(L)


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    add_target_args(p)
    p.add_argument("--title", choices=sorted(TITLES), default="keen4",
                   help="named preset: a .dsk, its timeline and the settle "
                        "time that agrees with them")
    p.add_argument("--rom", default=None, help="a .dsk of your own")
    p.add_argument("--name", default=None, help="name for a --rom run")
    p.add_argument("--settle", type=float, default=None,
                   help="override the preset's run seconds. The accumulator "
                        "never evicts, so longer only ever adds")
    p.add_argument("--firmware-dir", default=FW_DIR)
    p.add_argument("--sd", default="sdcard_retrogo.img")
    p.add_argument("--out", default=None, help="report path without extension")
    p.add_argument("--skip-build", action="store_true",
                   help="use the core already built. ONLY valid if it was "
                        "built with X86_PROBE=1; the run will say so if not")
    p.add_argument("--skip-cost", action="store_true",
                   help="do not build the core both ways to measure the "
                        "instrument's RAM_EMU cost (saves one clean build)")
    p.add_argument("--core-flag", action="append", default=[],
                   metavar="NAME=VALUE",
                   help="extra Makefile.core flag, repeatable "
                        "(e.g. --core-flag X86_DOS_EXEC=1). Recorded in the "
                        "report's meta so a non-default core is never "
                        "mistaken for the shipping one")
    p.add_argument("--quiet", action="store_true")
    p.add_argument("--compare", nargs=2, metavar=("A.json", "B.json"),
                   help="diff two reports and exit; runs nothing")
    args = p.parse_args()

    if args.compare:
        print(compare(*args.compare))
        return 0

    if args.rom:
        name = args.name or os.path.splitext(os.path.basename(args.rom))[0]
        title = {"rom": args.rom,
                 "timeline": "timelines/start-preselected-game.tl",
                 "settle": 20.0, "cmdtail": 1,
                 "what": f"Custom title: `{args.rom}`."}
    else:
        name = args.title
        title = TITLES[name]
    if args.timeline is None:
        args.timeline = title["timeline"]

    # A run that outlives its timeline loses gwemu mid-read and the dropped GDB
    # link reads as a target fault. Derive the budget; never hardcode it.
    settle = args.settle if args.settle is not None else title["settle"]
    if args.target != "hardware" and args.timeline:
        quit_at = timeline_quit_time(args.timeline)
        if quit_at and settle > quit_at - 3:
            raise SystemExit(
                f"probe_report: a {settle:.0f}s run outlives the timeline's "
                f"quit at {quit_at:.0f}s ({args.timeline}). Shorten it or pick "
                f"a longer timeline.")

    cost = None
    if args.skip_build:
        print("[build] skipped (--skip-build)")
        sh(["python3", "scripts/sdcard.py", "--image", "sdcard_retrogo.img",
            "--rom", title["rom"]], quiet=args.quiet)
    elif args.skip_cost:
        build_core(title["rom"], probe=True, quiet=args.quiet,
                   core_flags=args.core_flag)
    else:
        cost = measure_cost(title["rom"], args.quiet, core_flags=args.core_flag)

    r = collect(args, title)
    blk = r["block"]
    rows = acc.rows(blk)

    doc = {
        "meta": {
            "title": name,
            "title_what": title["what"],
            "rom": title["rom"],
            "timeline": args.timeline,
            "target": "hardware" if args.target == "hardware" else "gwemu",
            # A report on a non-default core must say so on its face.
            "core_flags": list(args.core_flag),
            "run_s": r["run_s"],
            "generated": datetime.datetime.now().isoformat(timespec="seconds"),
        },
        "block_meta": {k: v for k, v in blk.items() if k != "entries"},
        "rows": rows,
        "context": r["context"],
        "cost": cost,
    }

    base = args.out or os.path.join(
        OUT_DIR, f"probe_{name}_{doc['meta']['target']}")
    os.makedirs(os.path.dirname(base) or ".", exist_ok=True)
    # THE GUARD. It runs before render(), and it EXITS NON-ZERO -- a caller
    # scripting this must not be able to treat a dead guest as a clean result.
    bad, stopped = health(r["context"], blk)
    doc["void"] = bad
    doc["stopped"] = stopped
    md = render_void(doc, bad) if bad else render(doc)
    with open(base + ".md", "w") as fh:
        fh.write(md + "\n")
    with open(base + ".json", "w") as fh:
        json.dump(doc, fh, indent=1, sort_keys=True)
        fh.write("\n")
    print("")
    print(md)
    print(f"\nwrote {base}.md and {base}.json")
    if bad:
        sys.stderr.write("\nprobe_report: REFUSED to report -- the guest did "
                         "not run:\n")
    elif stopped:
        sys.stderr.write("\nprobe_report: the table is real but THE GUEST IS "
                         "STOPPED -- read it as a post-mortem, not as a "
                         "steady state:\n")
    for b in bad + stopped:
        sys.stderr.write("  * " + b.replace("`", "") + "\n")
    return 2 if (bad or stopped) else 0


if __name__ == "__main__":
    sys.exit(main())
