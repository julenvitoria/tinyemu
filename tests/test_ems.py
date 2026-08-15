#!/usr/bin/env python3
"""
Tier-1 checks for EMS -- the LIM expanded-memory interface, driven by GUEST
CODE doing the real sequence a title does.

Open EMMXXXX0 with INT 21h AH=3Dh -> INT 67h AH=41h for the page frame ->
AH=43h allocate -> AH=44h map a logical page into a window -> WRITE 16 KB
THROUGH THAT WINDOW WITH ORDINARY STOSW -> remap the window to another logical
page -> write it too -> map the first one back -> READ IT BACK WITH ORDINARY
MOV AX,ES:[SI]. Nothing here pokes x86_ems_int67() directly.

WHY THAT MATTERS AND WHY IT IS DIFFERENT FROM test_xms.py. XMS is a COPY
interface: no guest instruction ever dereferences an XMS byte, so its memory is
never in the fold table. EMS is the opposite -- the page frame IS mapped, and
the only witness that the four windows really resolve to resident AXI is a
guest executing plain stores and loads against E000:0000. Each logical page is
16 KB, i.e. FOUR 4 KB granules, so every pass crosses three granule seams in
each direction.

THE PINNING PROPERTY, AND ITS CONTROL. The harness gives the pager TWELVE
frames for TWENTY-FOUR backing pages, so pressure must evict. Logical page 0 is
MAPPED, so its four granules are pinned and must survive untouched; logical
page 1 is allocated but UNMAPPED, so it is an ordinary pager page and the same
pressure must take it. If the pin were a no-op the two would behave identically
-- which is what makes "an UNPINNED EMS page WAS evicted" a control and not a
restatement.

THE TWO NEGATIVE CONTROLS, both of which must be SEEN to fail if the thing they
guard is vacuous:
  * "control: corruption through the window IS detected" -- one byte of the
    mapped page is flipped and the same verify loop re-run. If the guest's
    accumulator were vacuous this would come back clean.
  * "control: an UNPINNED EMS page WAS evicted" -- above.
plus the guest's own refusals: a physical page out of range, a logical page out
of range, an unissued handle and an unimplemented function each have to come
back with the documented AH code.

The store is a plain RAM buffer standing in for the pagefile, exactly as the
pager and XMS arms' do, so the whole interface is proved with no card, no FatFs
and no firmware.
"""
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

PAGES = 6           # what the harness advertises
ALLOC = 4           # what the guest allocates
FRAME = 0xE000      # the page frame segment
DEV_HANDLE = 0x00F3  # DOS_DEV_HANDLE(DOS_DEV_EMS)


def run_arm(b, syms, flag, result, count):
    b.write_uint32(syms[flag], 1)
    for _ in range(600):
        if b.read_uint32(syms[flag]) == 0:
            break
        time.sleep(0.05)
    else:
        return None
    return [b.read_uint32(syms[result] + 4 * i) for i in range(count)]


def main(args):
    runner = build_and_create_runner(args)
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols(getattr(args, "elf", None) or "test_core.elf")
        for n in ("run_ems_flag", "ems_r"):
            if n not in syms:
                print(f"missing symbol {n}")
                return False

        time.sleep(0.5)
        r = run_arm(b, syms, "run_ems_flag", "ems_r", 40)
        if r is None:
            print("FAIL: target never ran the EMS test")
            return False

        checks = [
            # ---- the interface came up at all, and it CHECKED the frame
            ("EMS claimed the page frame",             r[34], 1),
            ("  ... and did not have to refuse it",    r[35], 0),

            # ---- detection, both routes a title uses
            ("INT 21h AH=3Dh opened EMMXXXX0",         r[0], DEV_HANDLE),
            ("  ... with CF clear",                    r[1], 0),
            ("the INT 67h device header says EMMXXXX0", r[36], 1),

            # ---- the interface answers
            ("AH=40h manager status",                  r[2], 0x00),
            ("AH=41h page frame segment",              r[3], FRAME),
            ("  ... status",                           r[4], 0x00),
            ("AH=42h pages free, before",              r[5], PAGES),
            ("AH=42h pages total",                     r[6], PAGES),
            ("AH=43h allocated 4 pages",               r[7], 0x00),
            ("  ... handle 1",                         r[8], 1),
            ("AH=46h version is 3.2",                  r[19], 0x0032),

            # ---- THE POINT OF THE ARM
            ("the granules really are shuffled",       r[28], 1),
            ("AH=44h mapped page 0 into window 0",     r[9], 0x00),
            ("AH=44h remapped window 0 to page 1",     r[10], 0x00),
            ("AH=44h mapped page 0 back",              r[11], 0x00),
            ("16 KB WRITTEN AND READ THROUGH THE FRAME"
             " -- byte exact across 3 seams",          r[12], 0),
            # three in phase A, plus phase C's remap of the evicted page
            ("four mappings were made",                r[37], 4),

            # ---- the residency rule, asserted directly
            ("all four granules of a mapped page are pinned", r[26], 1),
            ("  ... and that is 4 pages, not 1",       r[27], 4),
            ("the squeeze really evicted something",   r[29], 1),
            ("x86_pg_exhausted stayed 0",              r[30], 1),
            ("A MAPPED PAGE WAS NEVER A VICTIM",       r[31], 1),
            ("  ... and still reads back byte exact",  r[13], 0),

            # ---- the negative controls
            ("control: an UNPINNED EMS page WAS evicted", r[32], 1),
            ("control: corruption through the window IS detected", r[33], 1),

            # ---- the evicted page comes BACK from the store
            ("AH=44h mapped page 1 into window 1",     r[14], 0x00),
            ("the EVICTED page round-tripped through the store", r[15], 0),

            # ---- the refusals, each with its documented code
            ("a physical page out of range is refused", r[16], 0x8B),
            ("a logical page out of range is refused",  r[17], 0x8A),
            ("an unissued handle is refused",           r[18], 0x83),
            ("an unimplemented function is LOUD",       r[25], 0x84),

            # ---- accounting, and the memory coming back
            ("AH=4Bh handle count (incl. the driver's)", r[20], 2),
            ("AH=4Ch pages owned",                     r[21], ALLOC),
            ("AH=42h pages free while held",           r[22], PAGES - ALLOC),
            ("AH=45h deallocated",                     r[23], 0x00),
            ("AH=42h pages free after",                r[24], PAGES),
            ("deallocate released EVERY pin",          r[38], 0),
            ("no store I/O errors",                    r[39], 0),
        ]

        bad = 0
        for name, got, want in checks:
            ok = got == want
            bad += not ok
            print(f"  {'PASS' if ok else 'FAIL'}: {name}: {got}"
                  + ("" if ok else f" (expected {want})"))

        if bad:
            print(f"FAIL: {bad} of {len(checks)} checks")
            return False
        print(f"PASS: {len(checks)} checks")
        return True
    finally:
        runner.stop()


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    add_runner_args(p)
    args = p.parse_args()
    sys.exit(0 if main(args) else 1)
