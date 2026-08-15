#!/usr/bin/env python3
"""
Tier 2: boot retro-go under gwemu, launch our packed core, read its results.

Proves the pieces the whole architecture rests on, inside the real firmware:
  * the core loads and its ITCM segment is placed at 0x00000000
  * DTCM comes from mem_ctl(GW_MEM_DTC, ...) -- core_dtcm_ok
  * LUT8 releases the framebuffer bonus pool -- bonus_pool_size
  * the interpreter runs and reports the same 1,000,001 instructions as tier 2

Run:  python3 tests/test_core_retrogo.py [--gui] [--icount 0]
"""

import argparse
import os
import sys
import time

sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from scripts.common.target import (Image, add_target_args, open_target,
                                   timeline_quit_time)
from scripts.common.elf import get_symbols
from scripts.common.rglog import dump_log
from scripts.common.core_abi import check as abi_check


CORE_MAGIC = 0x54494E59   # 'TINY', must match src/main.c

def wait_for_value(backend, addr, want, timeout, poll=0.25):
    """Wait for an EXACT value. Non-zero is not a usable signal here: until the
    firmware loads our core, these RAM_EMU addresses hold firmware data."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = backend.read_uint32(addr)
        if last == want:
            return True
        time.sleep(poll)
    return False


ODROID_INPUT_RIGHT = 1        # odroid_input.h order: UP, RIGHT, DOWN, LEFT...
PAD_SCANCODE_RIGHT = 0x4D     # src/main.c pad_scancode[]


def check_input(runner, syms, out=print):
    """A host key press must reach the emulated keyboard controller.

    Nothing else in the suite covers the stages between the firmware's gamepad
    and port 0x60, which is how a guest that ignored the keyboard went
    unnoticed. Injects a real press through gwemu's QMP and
    walks the chain: poll_input runs -> the firmware's overlay handler did not
    swallow the frame -> the button mapped to a scancode -> the scancode was
    queued and IRQ 1 raised.
    """
    b = runner.backend
    if not hasattr(runner, "press"):
        out("input: target cannot inject keys, skipped")
        return True

    before = {n: b.read_uint32(syms[n]) for n in
              ("input_polls", "input_pad_seen", "input_scancodes",
               "input_irqs", "input_kbd_ht", "x86_kbd_port60_reads")}
    runner.press("RIGHT", delay=0.0)
    time.sleep(2.0)
    after = {n: b.read_uint32(syms[n]) for n in before}

    out("\n--- input chain ---")
    for n in before:
        out(f"{n:24s} {before[n]} -> {after[n]}")

    ok = True
    if after["input_polls"] <= before["input_polls"]:
        out("FAIL: poll_input is not running every frame")
        ok = False
    if not (after["input_pad_seen"] & (1 << ODROID_INPUT_RIGHT)):
        out("FAIL: the press never reached the core's gamepad read "
            "(firmware overlay handler consuming it?)")
        ok = False
    if after["input_scancodes"] <= before["input_scancodes"]:
        out("FAIL: no scancode was produced from the pad state")
        ok = False
    if after["input_irqs"] <= before["input_irqs"]:
        out("FAIL: IRQ 1 was never raised")
        ok = False
    # The code is either still queued (tail moved) or the guest has already
    # taken it off port 0x60. Either proves it reached the controller; which
    # one happens depends on the guest, so accept both.
    if (after["input_kbd_ht"] == before["input_kbd_ht"]
            and after["x86_kbd_port60_reads"] == before["x86_kbd_port60_reads"]):
        out("FAIL: the scancode never reached the controller queue")
        ok = False
    out("input: PASS" if ok else "input: FAIL")
    return ok and check_game_opens_osk(runner, syms, out)


ODROID_INPUT_START = 5        # odroid_input.h; the firmware maps B_GAME here
PAD_SCANCODE_ENTER = 0x1C     # src/main.c pad_scancode[8] -- the X button now
OSK_KEY_A = 0x1E              # layer 0, cell 0: the letter 'a'


def check_game_opens_osk(runner, syms, out=print):
    """GAME opens the on-screen keyboard, and the keyboard reaches the guest.

    GAME USED TO SEND RETURN AND DELIBERATELY NO LONGER DOES. It is the OSK's
    only door, on every unit -- docs/input/02-button-mapping.md -- so
    pad_scancode[6] is 0 and this test changed with it. Return moved to the
    physical START button (ODROID_INPUT_X), which is what gwemu's "start" key
    drives, so both halves are checked here: GAME must produce NO scancode and
    open the keyboard, and the A button must then send the selected cell's key
    ('a', 0x1E) rather than its game-mode Space.

    The trap this function was originally written for still applies: gwemu's
    "start" key is B_START, which the firmware reports as ODROID_INPUT_X, and
    the physical GAME button is ODROID_INPUT_START. Press GAME to test GAME.
    """
    b = runner.backend
    if not hasattr(runner, "press"):
        return True

    def read_log():
        base = syms["x86_kbd_read_log"]
        return [b.read_uint32(base + 4 * i) for i in range(32)]

    def fresh_since(idx0):
        idx1 = b.read_uint32(syms["x86_kbd_read_log_idx"])
        log = read_log()
        return idx1, [log[i & 31] for i in range(idx0, idx1)]

    out("\n--- GAME opens the OSK ---")
    ok = True

    # Close it first if an earlier phase left it open, so this measures the
    # OPENING press and not whatever state the run had drifted into.
    for _ in range(3):
        if b.read_uint32(syms["x86_osk_mode"]) == 0:
            break
        runner.press("GAME", delay=0.0)
        time.sleep(1.0)

    idx = b.read_uint32(syms["x86_kbd_read_log_idx"])
    # The PAD's own counter, not the port log: the guest drains that log one
    # code per interrupt, so bytes from an EARLIER press are still arriving
    # inside this window and would read as GAME's. input_scancodes counts what
    # we pushed, which is the actual claim being made here.
    codes = b.read_uint32(syms["input_scancodes"])
    runner.press("GAME", delay=0.0)
    time.sleep(2.0)
    seen = b.read_uint32(syms["input_pad_seen"])
    mode = b.read_uint32(syms["x86_osk_mode"])
    codes_after = b.read_uint32(syms["input_scancodes"])
    idx, fresh = fresh_since(idx)

    out(f"pad_seen bit {ODROID_INPUT_START} (START/GAME): "
        f"{'set' if seen & (1 << ODROID_INPUT_START) else 'CLEAR'}")
    out(f"x86_osk_mode after GAME: {mode}   (expect 1 = keyboard)")
    out(f"input_scancodes {codes} -> {codes_after}   (expect no change)")
    if not (seen & (1 << ODROID_INPUT_START)):
        out("FAIL: GAME never reached the core's gamepad read")
        ok = False
    if mode != 1:
        out("FAIL: GAME did not open the keyboard")
        ok = False
    if codes_after != codes:
        out("FAIL: GAME pushed a pad scancode; it must push none")
        ok = False

    # A presses the selected cell. The counters say a key was pressed; the port
    # log says WHICH, which is the only thing that answers "did the character
    # reach the guest".
    presses = b.read_uint32(syms["x86_osk_presses"])
    runner.press("A", delay=0.0)
    time.sleep(2.0)
    idx, fresh = fresh_since(idx)
    presses_after = b.read_uint32(syms["x86_osk_presses"])
    last = b.read_uint32(syms["x86_osk_last_key"])
    out(f"x86_osk_presses {presses} -> {presses_after}, "
        f"last key 0x{last:04X}   (expect 0x1E61, 'a')")
    out("bytes the guest read: " + (" ".join(f"0x{v:02X}" for v in fresh)
                                    or "(none)"))
    if presses_after <= presses:
        out("FAIL: A did not press a grid key")
        ok = False
    if OSK_KEY_A not in fresh:
        out(f"FAIL: 0x{OSK_KEY_A:02X} ('a') never reached port 0x60")
        ok = False

    # Two more GAMEs walk mouse mode and come back to OFF, which is what makes
    # the cycle safe: there is always a way back to the game.
    runner.press("GAME", delay=0.0)
    time.sleep(1.0)
    mid = b.read_uint32(syms["x86_osk_mode"])
    runner.press("GAME", delay=0.0)
    time.sleep(1.0)
    end = b.read_uint32(syms["x86_osk_mode"])
    out(f"cycle: 1 -> {mid} -> {end}   (expect 1 -> 2 -> 0)")
    if (mid, end) != (2, 0):
        out("FAIL: the GAME cycle did not return to OFF")
        ok = False

    out("GAME/OSK: PASS" if ok else "GAME/OSK: FAIL")
    return ok


def wait_for(backend, addr, timeout, poll=0.25):
    deadline = time.time() + timeout
    while time.time() < deadline:
        v = backend.read_uint32(addr)
        if v:
            return v
        time.sleep(poll)
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    add_target_args(p)
    p.add_argument("--firmware-dir",
                   default="references/game-and-watch-retro-go-sd/build",
                   help="directory holding the built firmware media")
    p.add_argument("--elf", default="build/core/dos_core.elf",
                   help="core ELF, for symbol lookup")
    p.add_argument("--sd", default="sdcard_retrogo.img",
                   help="SD image carrying /cores/dos.bin and /CONFIG")
    p.add_argument("--firmware-elf",
                   default="references/game-and-watch-retro-go-sd/build/"
                           "gw_retro_go.elf",
                   help="firmware ELF, for reading its log ring buffer")
    # Must stay INSIDE the timeline's own quit or gwemu exits mid-wait and the
    # GDB link drops -- which surfaces as "Connection closed by remote" and
    # reads like a target fault. Default is derived from the timeline below,
    # not hardcoded: the previous hardcoded 120s/150s outlived the 35s
    # start-preselected-game.tl and turned every tier-2 run into a traceback.
    p.add_argument("--timeout", type=float, default=None,
                   help="seconds to wait for the core to report ready "
                        "(default: the timeline's quit time, minus a margin)")
    args = p.parse_args()

    fw = args.firmware_dir
    args.firmware_elf = os.path.join(fw, "gw_retro_go.elf")
    if args.timeline is None:
        args.timeline = "timelines/start-preselected-game.tl"

    quit_at = timeline_quit_time(args.timeline)
    if args.timeout is None:
        args.timeout = max(5.0, (quit_at or 60.0) - 3.0)
    elif quit_at is not None and args.timeout > quit_at:
        print(f"NOTE: --timeout {args.timeout}s exceeds the timeline's quit at "
              f"{quit_at}s; clamping. gwemu will not be alive past that.")
        args.timeout = max(5.0, quit_at - 3.0)

    for path in (os.path.join(fw, "qemu_bank1.bin"), args.sd, args.elf):
        if not os.path.exists(path):
            sys.exit(f"missing {path} -- run: make retrogo_media && "
                     f"make -f Makefile.core pack")

    # Fail loudly BEFORE booting: a core the firmware will not accept simply
    # never appears, which looks exactly like every other kind of failure.
    if not abi_check("dos.bin", os.path.join(fw, "gw_retro_go.elf")):
        return 1

    syms = get_symbols(args.elf)
    for name in ("core_ready", "core_dtcm_ok", "benchmark_cycles",
                 "benchmark_insns", "bonus_pool_size", "core_magic",
                 "run_benchmark_flag", "frames_rendered", "fb_sample",
                 "clut_sample", "guest_ram_bytes", "guest_vram_ok",
                 "hle_calls_seen", "hle_bad_vector", "hle_bad_func"):
        if name not in syms:
            sys.exit(f"symbol {name} not in {args.elf}")

    image = Image(bank1=os.path.join(fw, "qemu_bank1.bin"),
                  bank2=os.path.join(fw, "qemu_bank2.bin"),
                  extflash=os.path.join(fw, "extflash.bin"),
                  sdcard=args.sd, elf=args.elf, intflash_bank=1)
    runner = open_target(args, image)
    runner.start()
    ok = False
    try:
        # No launcher navigation. The SD image carries a /CONFIG that
        # preselects /roms/dos/CAT.EXE, and the timeline just presses A twice.
        # Navigating menus by button choreography is not tractable and is
        # explicitly not how this is meant to be driven.
        print("Waiting for the preselected ROM to launch our core...")
        if not wait_for_value(runner.backend, syms["core_magic"], CORE_MAGIC,
                              args.timeout):
            got = runner.backend.read_uint32(syms["core_magic"])
            print(f"FAIL: core never ran (core_magic=0x{got:08x}, "
                  f"expected 0x{CORE_MAGIC:08x})")
            dump_log(runner.backend, args.firmware_elf, tail=40)
            return 1
        ready = wait_for(runner.backend, syms["core_ready"],
                         min(10.0, args.timeout))
        if not ready:
            print("FAIL: core never reported ready.")
            print(f"  core_dtcm_ok = "
                  f"{runner.backend.read_uint32(syms['core_dtcm_ok'])}")
            dump_log(runner.backend, args.firmware_elf, tail=40)
            return 1

        # The emulator loop, checked through state rather than through a
        # screenshot: headless gwemu renders nothing at all -- even Retro-Go's
        # own menu comes back black -- so the picture cannot be verified here.
        # What CAN be verified is that the guest ran, the blitter produced the
        # right palette indices, and the CLUT reached the firmware.
        frames_a = runner.backend.read_uint32(syms["frames_rendered"])
        time.sleep(1.0)
        frames_b = runner.backend.read_uint32(syms["frames_rendered"])
        fb_sample = runner.backend.read_uint32(syms["fb_sample"])
        clut_sample = runner.backend.read_uint32(syms["clut_sample"])
        guest_ram = runner.backend.read_uint32(syms["guest_ram_bytes"])
        vram_ok = runner.backend.read_uint32(syms["guest_vram_ok"])

        hle_calls = runner.backend.read_uint32(syms["hle_calls_seen"])
        bad_vec = runner.backend.read_uint32(syms["hle_bad_vector"])
        bad_fn = runner.backend.read_uint32(syms["hle_bad_func"])

        print("\n--- emulator loop ---")
        print(f"frames           {frames_a} -> {frames_b}")
        print(f"guest RAM        {guest_ram} bytes")
        print(f"VRAM allocated   {vram_ok}")
        print(f"fb first 4 px    0x{fb_sample:08x}")
        print(f"clut[1]          0x{clut_sample:08x}")

        print(f"HLE calls        {hle_calls}")

        loop_ok = True
        # The demo guest makes exactly two service calls, INT 10h AH=0Eh and
        # INT 21h AH=30h, through planted 0F 0B stubs. Zero would mean the
        # stubs or the IVT never took.
        if hle_calls < 2:
            print(f"FAIL: expected at least 2 HLE calls, saw {hle_calls}")
            loop_ok = False
        if bad_vec or bad_fn:
            print(f"FAIL: unhandled HLE service: vector 0x{bad_vec:02x} "
                  f"function 0x{bad_fn:02x}")
            loop_ok = False
        if frames_b <= frames_a:
            print("FAIL: frame counter is not advancing")
            loop_ok = False
        if guest_ram != 640 * 1024:
            print(f"FAIL: expected 640 KB of guest RAM, got {guest_ram}")
            loop_ok = False
        if not vram_ok:
            print("FAIL: VRAM was not allocated")
            loop_ok = False
        # The demo guest fills VRAM with 0x6C1B, whose low byte 0x1B is
        # 00 01 10 11 -> palette indices 0,1,2,3 left to right.
        if fb_sample != 0x03020100:
            print(f"FAIL: blitter wrote 0x{fb_sample:08x}, expected 0x03020100")
            loop_ok = False
        # Palette 1 high intensity, colour 1 = light cyan.
        if clut_sample != 0xFF55FFFF:
            print(f"FAIL: clut[1] was 0x{clut_sample:08x}, expected 0xff55ffff")
            loop_ok = False

        loop_ok = check_input(runner, syms) and loop_ok

        dtcm_ok = runner.backend.read_uint32(syms["core_dtcm_ok"])
        bonus = runner.backend.read_uint32(syms["bonus_pool_size"])
        # The core does NOT self-start the benchmark: auto-running it at boot
        # faulted and rebooted the device, wiping the very globals this test
        # reads. The host arms it, which is also why this test was previously
        # reporting a figure the core had stopped producing.
        runner.backend.write_uint32(syms["run_benchmark_flag"], 1)
        cycles = wait_for(runner.backend, syms["benchmark_cycles"], args.timeout)
        insns = runner.backend.read_uint32(syms["benchmark_insns"])

        print("\n--- core reported ---")
        print(f"core_ready       {ready}")
        print(f"core_dtcm_ok     {dtcm_ok}")
        print(f"bonus_pool_size  {bonus} bytes")
        print(f"cycles           {cycles}")
        print(f"insns            {insns}")
        if insns:
            print(f"cyc/insn         {cycles / insns:.2f}")

        ok = loop_ok
        if not dtcm_ok:
            print("FAIL: DTCM allocation failed"); ok = False
        if insns != 1000001:
            print(f"FAIL: expected 1000001 instructions, got {insns}"); ok = False
        if bonus == 0:
            print("FAIL: LUT8 released no bonus pool"); ok = False
        print("PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        try:
            runner.collect_artifacts()
        except Exception as exc:
            print(f"(could not collect artifacts: {exc})")
        runner.stop()


if __name__ == "__main__":
    sys.exit(main())
