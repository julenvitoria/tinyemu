#!/usr/bin/env python3
"""
Sample a running target: PC, watched globals, pointer dereferences.

The "what is it actually doing" tool. Use it when a harness reports a value that
makes no sense and you need to see whether the CPU is in our interpreter, in
firmware, or wedged -- and whether the guest is advancing at all.

Examples:

    # Is our core progressing inside retro-go? Follow cpu_state_ptr to EAX.
    python3 scripts/probe.py --retrogo \\
        --watch core_ready,core_dtcm_ok,benchmark_insns \\
        --deref cpu_state_ptr+8=EAX

    # Bare-metal harness, on silicon
    python3 scripts/probe.py --target hardware --watch run_flag,benchmark_cycles

Timelines used here must stay short -- see CLAUDE.md. If you need a long window
to observe something, that is a bug to find, not a wait to sit through.
"""

import argparse
import os
import sys

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.probe import (                       # noqa: E402
    dump_arrays, dump_bytes, dump_dos_log, dump_guest, dump_io_log,
    dump_symbol_name,
    save_bytes, save_guest,
    load_symbol_maps,
    parse_bytes, parse_deref, parse_dump, parse_guest,
    parse_watch,
    sample, wait_for_value,
)
from scripts.common.rglog import LogDrainer                # noqa: E402
from scripts.common.target import Image, add_target_args, open_target  # noqa: E402

FW_DIR = "references/game-and-watch-retro-go-sd/build"
CORE_MAGIC = 0x54494E59   # 'TINY', set by src/main.c



# Named watch sets, so a subsystem's whole story is one word on the command
# line instead of a seventeen-symbol paste that goes stale the moment a counter
# is added. Keep these in step with the tables in TOOLING.md.
WATCH_GROUPS = {
    "nor": [
        # did the snapshot happen, and what did the firmware's flash cache do
        "nor_snap_result", "nor_snap_ms", "nor_snap_addr", "nor_snap_cb_bytes",
        # the watchdog question: worst stretch with no kick, against ~472 ms
        "nor_snap_wdog_gap_ms", "x86_nor_heartbeats",
        # and what the slice regulator was handed back at (watchdog, post-attach)
        "guest_max_slice", "guest_exec_ms",
        # is the guest actually running out of NOR
        "x86_nor_attached", "x86_nor_attach_err",
        # the measurement: how much of guest RAM is genuinely read-only
        "x86_nor_pages_total", "x86_nor_pages_nor", "x86_nor_pages_axi",
        "x86_nor_axi_free",
        # what it costs, and who is paying it
        "x86_nor_reads", "x86_nor_writes", "x86_nor_cow_pulls",
        "x86_nor_host_cow", "x86_nor_slow_cycles",
        # the loud failures
        "x86_nor_cow_failures", "x86_nor_unmapped",
    ],
    "planar": [
        # is a planar mode up at all, and what is it costing
        "x86_video_mode_byte", "x86_vga_modey_armed", "x86_vga_ega_armed",
        "guest_ips_achieved", "guest_insns_total",
        # the store path: volume, and the write-class MIX that decides whether
        # a fast path is earning its risk. cls_general is the one to watch --
        # it is the only class that still pays the full pipeline
        "x86_vga_planar_stores", "x86_vga_planar_cls_plane",
        "x86_vga_planar_cls_copy", "x86_vga_planar_cls_general",
        # storage: what was discarded, and how far past the end it went
        "x86_vga_planar_lost", "x86_vga_latch_miss",
        "x86_vga_modey_cache_offs", "x86_vga_modey_lost_cache",
        "x86_vga_modey_lost_lo", "x86_vga_modey_lost_hi",
        "x86_vga_ega_card_offsets", "x86_arena_largest",
    ],
    "vprof": [
        # X86_VPROF=1 only; reads honest zeroes otherwise, which is
        # deliberate -- a host that cannot tell "instrument off" from "symbol
        # missing" is how a measurement gets silently skipped.
        #
        # Every field is its OWN symbol so that a window can be measured by
        # DIFFERENCING two samples. A run that starts at boot spends its first
        # 27 s in menus, and a cumulative total cannot be told apart from a
        # gameplay one -- which is exactly the mistake this group exists to
        # make impossible.
        "x86_vprof_frame_cyc", "x86_vprof_frame_n", "x86_vprof_frame_worst",
        "x86_vprof_store_cyc", "x86_vprof_store_n", "x86_vprof_store_worst",
        "x86_vprof_paint_cyc", "x86_vprof_paint_n", "x86_vprof_paint_worst",
        "x86_vprof_render_cyc", "x86_vprof_render_n",
        "x86_vprof_render_worst",
        "x86_vprof_osk_cyc", "x86_vprof_osk_n",
        "x86_vprof_cell_cyc", "x86_vprof_cell_n", "x86_vprof_cell_worst",
        "x86_vprof_paint_flip", "x86_vprof_paint_loop", "x86_vprof_ovh",
        # The gate on every one of these runs: presents must CLIMB, or the
        # numbers describe the menus.
        "x86_modey_presents", "x86_modey_fallbacks", "x86_vga_modey_armed",
        "guest_insns_total", "x86_vga_planar_stores", "frames_rendered",
        "x86_disk_cycles_per_us",
    ],
}

def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_target_args(p)
    p.add_argument("--retrogo", action="store_true",
                   help="boot retro-go + our packed core (default: the "
                        "bare-metal CPU harness)")
    p.add_argument("--firmware-dir", default=FW_DIR)
    p.add_argument("--sd", default="sdcard_retrogo.img")
    p.add_argument("--elf", default=None,
                   help="ELF for symbols (default depends on --retrogo)")
    p.add_argument("--watch", default="",
                   help="comma-separated globals to read as uint32; a GROUP "
                        f"name expands to a whole set ({', '.join(WATCH_GROUPS)})")
    p.add_argument("--dump", action="append", default=[],
                   metavar="SYM[:N][xG][=LABEL]",
                   help="hexdump N words of a symbol's own storage, grouped "
                        "G per line (arrays; --deref follows a pointer instead)")
    p.add_argument("--bytes", action="append", default=[],
                   metavar="SYM|*SYM+OFF|0xADDR[:N][=LABEL]",
                   help="hexdump N BYTES at a host address, following a "
                        "pointer with the *SYM+OFF form. --dump reads WORDS of "
                        "a symbol's own storage and cannot reach the buffer "
                        "behind a `uint8_t *`; --deref reads only one uint32. "
                        "Repeatable. For guest RAM use --guest instead")
    p.add_argument("--bytes-group", type=int, default=1, metavar="G",
                   help="group --bytes output G bytes at a time (e.g. 3 for "
                        "the VGA DAC's r,g,b triples, so a phase error in the "
                        "stream is visible instead of counted by hand)")
    p.add_argument("--bytes-save", action="append", default=[],
                   metavar="SYM[:N]:FILE",
                   help="write a host byte range to a file, for byte-comparing "
                        "against a reference (e.g. the DOSBox-X DAC)")
    p.add_argument("--deref", action="append", default=[],
                   metavar="SYM[+OFF][=LABEL]",
                   help="read uint32 at (*SYM)+OFF; repeatable")
    p.add_argument("--set", action="append", default=[], metavar="SYM=VALUE",
                   help="write a uint32 before sampling, e.g. to trigger work "
                        "on the target; repeatable")
    p.add_argument("--zero", action="append", default=[], metavar="ADDR:BYTES",
                   help="write zeros over a memory range before sampling, e.g. "
                        "--zero 0x30000000:0x48000. Repeatable. For clearing "
                        "RETAINED state: retro-go keeps persistent_config_ram "
                        "in D2 AHB SRAM (0x30000000), which survives every "
                        "reset and reflash and is only lost on real power "
                        "removal -- so a bad value there outlives every "
                        "recovery step short of this")
    p.add_argument("--guest", action="append", default=[],
                   metavar="LINADDR[:BYTES][=LABEL]",
                   help="hexdump guest LINEAR memory through the fold table, "
                        "e.g. --guest 0x10eac:64=CSIP. This is the only correct "
                        "way to read guest RAM: under demand paging it is a "
                        "frame pool, so *guest_ram+OFF is NOT linear memory")
    p.add_argument("--guest-save", action="append", default=[],
                   metavar="LINADDR:BYTES:FILE",
                   help="write a guest LINEAR range to a file, for byte-"
                        "comparing our image against a reference emulator's")
    p.add_argument("--dos-log", action="store_true",
                   help="decode the INT 21h file-call trace "
                        "(x86_dos_file_log) into one line per call")
    p.add_argument("--io-log", action="store_true",
                   help="decode the coalesced port-I/O ring (x86_io_log)")
    p.add_argument("--faults", action="store_true",
                   help="also dump ARMv7-M fault/exception state")
    p.add_argument("--artifacts", nargs="?", const="build/artifacts",
                   default=None, metavar="DIR",
                   help="copy the .png/.ppm a timeline's `screenshot` events "
                        "wrote out of the run directory before it is deleted "
                        "(default build/artifacts)")
    p.add_argument("--samples", type=int, default=10)
    p.add_argument("--interval", type=float, default=1.0)
    p.add_argument("--wait-magic", action="store_true",
                   help="wait for core_magic before sampling (retro-go)")
    args = p.parse_args()

    if args.retrogo:
        fw = args.firmware_dir
        elf = args.elf or "build/core/dos_core.elf"
        image = Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                      bank2=os.path.join(fw, "qemu_bank2.bin"),
                      extflash=os.path.join(fw, "extflash.bin"),
                      sdcard=args.sd, elf=elf, intflash_bank=1,
                      intflash=os.path.join(fw, "gw_retro_go_intflash.bin"))
        if args.timeline is None:
            args.timeline = "timelines/start-preselected-game.tl"
        fw_elf = os.path.join(fw, "gw_retro_go.elf")
    else:
        elf = args.elf or "test_core.elf"
        image = Image(bank1="qemu_bank1.bin", bank2="qemu_bank2.bin",
                      extflash="extflash.bin", sdcard="sdcard.img",
                      elf=elf, intflash_bank=1)
        fw_elf = None

    syms, maps = load_symbol_maps(fw_elf, elf)
    watches = []
    for w in parse_watch(args.watch):
        watches.extend(WATCH_GROUPS.get(w, [w]))
    derefs = [parse_deref(d) for d in args.deref]
    dumps = [parse_dump(d) for d in args.dump]
    guests = [parse_guest(g) for g in args.guest]
    byte_dumps = [parse_bytes(b) for b in args.bytes]
    byte_saves = []
    for spec in args.bytes_save:
        head, _, path = spec.rpartition(":")
        sym, nbytes, _ = parse_bytes(head)
        byte_saves.append((sym, nbytes, path))

    missing = [w for w in watches if w not in syms]
    missing += [s for s, _, _ in derefs if s not in syms]
    missing += [n for n in (dump_symbol_name(s) for s, _, _, _ in dumps)
                if n is not None and n not in syms]
    missing += [n for n in (dump_symbol_name(s)
                            for s, _, _ in byte_dumps + byte_saves)
                if n is not None and n not in syms]
    if missing:
        sys.exit(f"symbols not found in {elf}: {missing}")

    target = open_target(args, image)
    target.start()
    try:
        if args.wait_magic and "core_magic" in syms:
            print("waiting for core_magic...")
            if not wait_for_value(target.backend, syms["core_magic"],
                                  CORE_MAGIC, 30.0):
                print("core never started; sampling anyway")
            else:
                print("core running")
        for spec in args.zero:
            addr_s, _, len_s = spec.partition(":")
            addr, nbytes = int(addr_s, 0), int(len_s, 0)
            target.backend.halt()
            chunk = bytes(4096)
            done = 0
            while done < nbytes:
                n = min(len(chunk), nbytes - done)
                target.backend.write_memory(addr + done, chunk[:n])
                done += n
            target.backend.resume()
            print(f"zeroed 0x{addr:08x}..0x{addr + nbytes:08x} ({nbytes} bytes)")

        for spec in args.set:
            name, _, val = spec.partition("=")
            name = name.strip()
            if name not in syms:
                sys.exit(f"--set: symbol {name} not found")
            target.backend.write_uint32(syms[name], int(val, 0))
            print(f"set {name} = {int(val, 0)}")

        drainer = LogDrainer(fw_elf) if fw_elf else None
        if drainer and not drainer.available:
            drainer = None

        sample(target, syms, watches=watches, derefs=derefs,
               count=args.samples, interval=args.interval, symbol_maps=maps,
               on_sample=(drainer.drain if drainer else None))

        # After sampling, not before: an array read at t=0 shows the state
        # before the target has done anything, which is how a full log reads
        # as empty.
        if dumps:
            target.backend.halt()
            dump_arrays(target, syms, dumps, symbol_maps=maps)
            target.backend.resume()
        if byte_dumps:
            target.backend.halt()
            dump_bytes(target, syms, byte_dumps, group=args.bytes_group)
            target.backend.resume()
        if byte_saves:
            target.backend.halt()
            save_bytes(target, syms, byte_saves)
            target.backend.resume()
        if args.guest_save:
            target.backend.halt()
            save_guest(target, syms,
                       [(int(a, 0), int(b, 0), f) for a, b, f in
                        (g.split(":", 2) for g in args.guest_save)])
            target.backend.resume()
        if guests:
            target.backend.halt()
            dump_guest(target, syms, guests)
            target.backend.resume()
        if args.dos_log:
            print("INT 21h file trace:")
            target.backend.halt()
            dump_dos_log(target, syms)
            target.backend.resume()
        if args.io_log:
            print("port I/O trace:")
            target.backend.halt()
            dump_io_log(target, syms)
            target.backend.resume()
        if args.faults:
            from scripts.common.probe import read_faults
            print("fault state:")
            target.backend.halt()
            read_faults(target.backend, symbol_maps=maps)
            target.backend.resume()
    finally:
        # BEFORE stop(): the run directory is deleted there, and a timeline's
        # `screenshot` events write into it (gwemu resolves those paths against
        # its own cwd). Without this the pictures a timeline was written to
        # take are produced and then thrown away -- which reads exactly like
        # gwemu ignoring the events.
        if args.artifacts and hasattr(target, "collect_artifacts"):
            try:
                target.collect_artifacts(args.artifacts)
            except Exception as exc:                       # noqa: BLE001
                print(f"collecting artifacts failed: {exc}")
        target.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
