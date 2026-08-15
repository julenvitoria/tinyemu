#!/usr/bin/env python3
"""
Timelines: the parser, and the shadow-word player that drives real hardware.

Two halves, because they fail for different reasons:

  * `--offline` (always run) checks the .tl parser and the event schedule --
    grammar, ordering, the fold into button masks, the vendored bit order.
    Pure arithmetic, no target, milliseconds.
  * the target half REPLAYS a timeline through the SAME player the hardware
    path uses -- ShadowWordPlayer writing 0x30001FF4 over the backend -- and
    watches the firmware's own counters to see the presses arrive. gwemu maps
    that SRAM, so the hardware injection path can be proven at tier 2 instead
    of first meeting silicon. gwemu's built-in GNW_TIMELINE replay is
    deliberately NOT used here: the point is to exercise the other player.

    What it measures is the achieved schedule: for every button, when its bit
    first appeared in `input_pad_seen` versus when the file said to press it.

Run:
    python3 tests/test_timeline.py --offline
    python3 tests/test_timeline.py                     # tier 2, under gwemu
"""

import argparse
import os
import sys
import time

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.elf import get_symbols
from scripts.common.target import (Image, GwemuTarget, add_target_args,
                                   open_target)
from scripts.common import timeline as tl

DEFAULT_TIMELINE = "timelines/alleycat-ingame.tl"

# timeline button -> odroid_gamepad_key_t bit, the whole chain in one table:
#   .tl name -> shadow bit (timeline.BUTTON_BITS) -> B_* (gw_buttons.c remote
#   decode) -> ODROID_INPUT_* (Core/Src/porting/odroid_input.c) -> the bit
#   src/main.c ORs into input_pad_seen.
# Three different orderings meet here; this table is the only place they do.
ODROID_BIT = {
    "up": 0, "right": 1, "down": 2, "left": 3,
    "time": 4,       # B_TIME  -> ODROID_INPUT_SELECT
    "game": 5,       # B_GAME  -> ODROID_INPUT_START
    "a": 6, "b": 7,
    "pause": 9,      # ODROID_INPUT_VOLUME
    "power": 10,
    "start": 11,     # B_START -> ODROID_INPUT_X
    "select": 12,    # B_SELECT-> ODROID_INPUT_Y
}


# --------------------------------------------------------------- offline ---
def offline(path, out=print):
    ok = True

    def check(cond, what):
        nonlocal ok
        out(f"  {'PASS' if cond else 'FAIL'}  {what}")
        ok = ok and bool(cond)

    out(f"--- parser, on {path} ---")
    events = tl.parse(path)
    check(len(events) > 0, f"{len(events)} events parsed")
    check(all(events[i].t <= events[i + 1].t for i in range(len(events) - 1)),
          "events are time-sorted")
    check(tl.quit_time(events) is not None, "the file ends with a quit")

    # Bit order is vendored from the firmware's proven remote_input.py; pin it
    # so a future edit cannot quietly rotate it.
    check(tl.BUTTON_BITS == {"up": 0, "down": 1, "left": 2, "right": 3,
                             "a": 4, "b": 5, "start": 6, "select": 7,
                             "pause": 8, "game": 9, "time": 10, "power": 11},
          "shadow bit order matches remote_input.py")

    st = tl.states(events)
    check(st and st[-1][1] == 0, "the schedule ends with nothing held")
    check(all(m == 0 or m.bit_length() <= 12 for _, m in st),
          "no mask has a bit outside the 12 buttons")
    q = tl.quit_time(events)
    check(all(t <= q for t, _ in st), "no state change after quit")

    # Held-button folding: masks must actually accumulate a chord.
    chord = tl.states(tl.parse_text(
        "1.0 down a\n1.5 down b\n2.0 release a\n2.5 release b\n3.0 quit\n"))
    check([m for _, m in chord] == [0x10, 0x30, 0x20, 0x00],
          f"a+b chord folds to masks {[hex(m) for _, m in chord]}")

    # Same-timestamp events collapse to ONE write.
    same = tl.states(tl.parse_text("1.0 down a\n1.0 down b\n2.0 quit\n"))
    check(same == [(1.0, 0x30)], "same-timestamp events become one write")

    presses = tl.presses(events)
    held = [(r - d) for _, d, r in presses if r is not None]
    check(True, f"{len(presses)} down/release pairs")
    if held:
        out(f"  note   shortest press in the file: {min(held)*1000:.0f}ms "
            f"({len(tl.short_presses_of(events))} under the "
            f"{tl.SHORT_PRESS_MS}ms input poll)")

    for bad, why in (("1.0 down\n", "down with no button"),
                     ("1.0 down nope\n", "unknown button"),
                     ("1.0 wiggle a\n", "unknown event"),
                     ("later down a\n", "non-numeric timestamp")):
        try:
            tl.parse_text(bad)
            check(False, f"rejects {why}")
        except tl.TimelineError:
            check(True, f"rejects {why}")
    return ok


# ---------------------------------------------------------------- replay ---
def replay(args, out=print):
    fw = args.firmware_dir
    for path in (os.path.join(fw, "qemu_bank1.bin"), args.sd, args.elf):
        if not os.path.exists(path):
            sys.exit(f"missing {path} -- run: python3 scripts/build_retrogo.py "
                     f"&& make sdcard")

    syms = get_symbols(args.elf)
    events = tl.parse(args.timeline)
    quit_at = tl.quit_time(events) or 40.0

    image = Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                  bank2=os.path.join(fw, "qemu_bank2.bin"),
                  extflash=os.path.join(fw, "extflash.bin"),
                  sdcard=args.sd, elf=args.elf, intflash_bank=1)

    hardware = getattr(args, "target", "gwemu") == "hardware"
    if hardware:
        # On silicon this is just a normal run: HardwareTarget owns the player.
        image.intflash = os.path.join(fw, "gw_retro_go_intflash.bin")
        target = open_target(args, image)
    else:
        # Under gwemu, deliberately NOT GNW_TIMELINE: the point of this run is
        # to exercise the player the HARDWARE path uses, against the same SRAM
        # cell, before it ever meets a device.
        target = GwemuTarget(image, display=args.gui, audio=args.audio,
                             icount=args.icount, keep_temp=args.keep_temp)
    target.start()
    first_seen = {}
    try:
        b = target.backend
        addr = tl.SHADOW_ADDR
        if hardware:
            player = target.player
            if player is None:
                out("FAIL: the hardware target started no timeline player")
                return False
        else:
            # Write a PATTERN, not zero: zero reads back correctly from a cell
            # that does not exist, from one the firmware just cleared, and from
            # one nothing is mapped to. Only a value we chose proves the write
            # landed.
            b.write_uint32(addr, 0xA5)
            got = b.read_uint32(addr)
            b.write_uint32(addr, 0)
            out(f"shadow cell 0x{addr:08X}: wrote 0xA5, read back 0x{got:02X}")
            if got != 0xA5:
                out("FAIL: the shadow cell is not writable on this target")
                return False
            player = tl.ShadowWordPlayer(b, events)
            player.start()
        t0 = player.t0

        # Watch the firmware's sticky OR of raw gamepad bits and note when each
        # button FIRST appears. That, against the file, is the delivery time as
        # the guest-facing code saw it -- not as the host believes it wrote it.
        seen = 0
        polls_seen = 0
        deadline = t0 + quit_at + 1.0
        while time.time() < deadline:
            try:
                pad = b.read_uint32(syms["input_pad_seen"])
                polls_seen = b.read_uint32(syms["input_polls"])
            except Exception as exc:
                out(f"backend read failed: {exc}")
                break
            new = pad & ~seen
            if new:
                now = time.time() - t0
                for bit in range(13):
                    if new & (1 << bit):
                        first_seen[bit] = now
                seen = pad
            time.sleep(0.05)
        player.join(timeout=5)
        scancodes = b.read_uint32(syms["input_scancodes"])
        irqs = b.read_uint32(syms["input_irqs"])
        magic = b.read_uint32(syms["core_magic"])
    finally:
        target.stop()

    out("\n--- what the device received ---")
    out(player.report())
    out(f"core_magic 0x{magic:08x} (0x54494e59 = our core is running), "
        f"input_polls {polls_seen}")
    if not polls_seen:
        out("NOTE: poll_input never ran -- the core did not launch in this "
            "window, so nothing downstream of the launcher can be judged.")

    want = {}
    for name, dt, _ in tl.presses(events):
        bit = ODROID_BIT[name]
        want.setdefault(bit, (name, dt))

    ok = True
    rows = []
    for bit, (name, dt) in sorted(want.items(), key=lambda kv: kv[1][1]):
        got = first_seen.get(bit)
        rows.append((name, dt, got))
    out(f"{'button':8s} {'file@s':>8s} {'seen@s':>8s} {'delta':>8s}")
    for name, dt, got in rows:
        d = f"{(got-dt)*1000:+.0f}ms" if got is not None else "  MISSED"
        out(f"{name:8s} {dt:8.3f} "
            f"{(f'{got:8.3f}' if got is not None else '       -')} {d:>8s}")

    # Order, not just presence: the buttons must arrive in the file's order.
    ordered = [n for n, _, g in rows if g is not None]
    by_time = [n for n, _, g in sorted([r for r in rows if r[2] is not None],
                                       key=lambda r: r[2])]
    if ordered != by_time:
        out(f"FAIL: arrival order {by_time} != file order {ordered}")
        ok = False
    missed = [n for n, _, g in rows if g is None]
    if missed:
        out(f"FAIL: never observed: {', '.join(missed)}")
        ok = False
    late = [(n, g - dt) for n, dt, g in rows if g is not None]
    if late:
        worst = max(late, key=lambda p: abs(p[1]))
        out(f"worst first-press delivery error: {worst[0]} "
            f"{worst[1]*1000:+.0f}ms")
        if abs(worst[1]) > args.tolerance:
            out(f"FAIL: outside the {args.tolerance*1000:.0f}ms tolerance")
            ok = False
    out(f"scancodes produced: {scancodes}, IRQs: {irqs}")
    out("timeline replay: PASS" if ok else "timeline replay: FAIL")
    return ok


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    add_target_args(p)
    p.add_argument("--offline", action="store_true",
                   help="parser and schedule checks only, no target")
    p.add_argument("--firmware-dir",
                   default="references/game-and-watch-retro-go-sd/build")
    p.add_argument("--elf", default="build/core/dos_core.elf")
    p.add_argument("--sd", default="sdcard_retrogo.img")
    p.add_argument("--tolerance", type=float, default=1.0, metavar="S",
                   help="allowed error between the file's press time and when "
                        "the device was seen to have it (default 1s: the "
                        "observation itself is a 50ms poll of a counter that "
                        "only advances while the core runs)")
    args = p.parse_args()
    if args.timeline is None:
        args.timeline = DEFAULT_TIMELINE

    ok = offline(args.timeline)
    if not args.offline:
        ok = replay(args) and ok
    print("\nOK" if ok else "\nFAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
