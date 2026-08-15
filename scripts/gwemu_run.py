#!/usr/bin/env python3
"""Launch an INTERACTIVE gwemu session that stays up until the owner closes it.

Why this exists as its own entry point rather than a flag on probe.py:

`GwemuTarget` starts gwemu with `-S` (halted) and only the GDB backend's
`resume()` gets it running, then `atexit` tears the process down when the
harness exits. That combination is right for a MEASUREMENT -- boot, sample to a
deterministic point, stop -- and exactly wrong for a person sitting in front of
the window. Every sampling harness therefore kills the emulator at the moment it
finishes booting, which on a slow boot is the moment it becomes useful. That is
not a hypothetical: it ate two Commander Keen sessions.

So this script owns the process for as long as the WINDOW is open. It resumes
the CPU once and then blocks on the gwemu process itself, and the run directory
is removed in a `finally` (build/run/, never /tmp -- see CLAUDE.md).

With --watch it also samples counters while you play, which is the part probe.py
could not give you here: halting to read a symbol is fine, but owning the
lifetime is not.

    # boot to the retro-go menu and drive it yourself
    python3 scripts/gwemu_run.py --sd build/sd-keen4-noconfig.img

    # same, printing pager/video counters every 3 s while you play
    python3 scripts/gwemu_run.py --sd build/sd-keen4-noconfig.img \\
        --watch guest_insns_total,core_ready,x86_pg_exhausted --interval 3
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from scripts.common.target import Image, GwemuTarget  # noqa: E402
from scripts.common.probe import get_symbols  # noqa: E402

FW_DIR = "references/game-and-watch-retro-go-sd/build"


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--sd", required=True, help="SD card image to boot")
    p.add_argument("--firmware-dir", default=FW_DIR)
    p.add_argument("--core-elf", default="build/core/dos_core.elf",
                   help="ELF for --watch symbol lookup")
    p.add_argument("--watch", default="",
                   help="comma-separated globals to sample while you play")
    p.add_argument("--interval", type=float, default=3.0)
    p.add_argument("--audio", action="store_true")
    p.add_argument("--record", metavar="FILE.tl",
                   help="RECORD your session to a timeline. gwemu writes the "
                        "file when it EXITS CLEANLY, so close the window "
                        "normally -- a killed process writes nothing. Replay "
                        "it later on gwemu OR hardware with --timeline")
    p.add_argument("--keep-temp", action="store_true")
    args = p.parse_args()

    fw = args.firmware_dir
    image = Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                  bank2=os.path.join(fw, "qemu_bank2.bin"),
                  extflash=os.path.join(fw, "extflash.bin"),
                  sdcard=args.sd,
                  elf=os.path.join(fw, "gw_retro_go.elf"),
                  intflash_bank=1)

    target = GwemuTarget(image, display=True, audio=args.audio,
                         record=args.record, keep_temp=args.keep_temp)
    target.start()
    print("gwemu is up and RUNNING. It will stay up until you close the "
          "window (Ctrl-C here also stops it).", flush=True)
    if args.record:
        print(f"RECORDING -> {args.record}. Close the gwemu WINDOW to save it; "
              f"Ctrl-C here kills gwemu and writes nothing.", flush=True)

    if args.record and args.watch:
        # Sampling HALTS the CPU to read a symbol. A recorded timeline stores
        # wall-clock offsets, so every halt shifts every later event -- the
        # file would replay out of step with what you actually did.
        sys.exit("--record and --watch cannot be combined: sampling halts the "
                 "CPU and would skew the recorded timings")

    watches = [w.strip() for w in args.watch.split(",") if w.strip()]
    syms = get_symbols(args.core_elf) if watches else {}
    missing = [w for w in watches if w not in syms]
    if missing:
        # Loud, but do NOT abort: the window is the point of this script.
        print(f"warning: not in {args.core_elf}, skipping: {missing}",
              flush=True)
        watches = [w for w in watches if w in syms]

    t0 = time.time()
    try:
        while target.proc.poll() is None:
            if not watches:
                time.sleep(0.5)
                continue
            time.sleep(args.interval)
            if target.proc.poll() is not None:
                break
            try:
                b = target.backend
                b.halt()
                vals = " ".join(f"{w}={b.read_uint32(syms[w])}"
                                for w in watches)
                b.resume()
            except Exception as e:
                # gwemu closing mid-sample is the NORMAL exit path here, not
                # an error worth a traceback.
                print(f"(sampling stopped: {e})", flush=True)
                break
            print(f"  t+{time.time() - t0:6.1f}s {vals}", flush=True)
    except KeyboardInterrupt:
        print("\ninterrupted", flush=True)
    finally:
        target.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
