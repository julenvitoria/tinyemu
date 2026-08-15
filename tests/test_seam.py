#!/usr/bin/env python3
"""
Tier-1 check for THE GRANULE SEAM.

host = lin + k, one k per 4 KB granule, so two adjacent GUEST granules are
adjacent in HOST memory only by luck. A guest 16- or 32-bit access whose bytes
span the boundary resolves one pointer from the LOW granule and then runs off
the top of that host page. Silently.

WHY THIS HARNESS EXISTS AND test_paging.py CANNOT DO IT. Every other tier-1 arm
runs on a fold that is either identity over test_ram or a pager whose frames
come out of one extent in order. In both, granule N+1 IS the memory after
granule N, so the defect cannot occur and no test built on them can fail. The
target arm here installs the fold BY HAND with the two granules 48 KB apart.

THREE ARMS, and the second and third are the controls:

  1. NON-CONTIGUOUS, fix on   -- the real case. Must read and write the
     neighbour's bytes.
  2. CONTIGUOUS, fix on       -- the same guest program on a fold where no seam
     exists. Proves the program and the expectations are right about something
     other than the seam.
  3. NON-CONTIGUOUS, fix OFF  -- built with -DX86_SEAM_FIX=0, which emits no
     prologue and no flush. This is the NEGATIVE CONTROL and it MUST FAIL:
     run it with --negative and every straddling check is inverted. If it
     passes, the arm is not testing what it says it is.

THE CODE-STREAM SEAM is the fourth arm and runs first. The dispatch threads
the opcode granule's fold constant to every later fetch of the SAME instruction
(cpu->code_k), so an instruction crossing the boundary would take its ModRM, its
displacement or its immediate off the low granule's frame. Three cases put the
straddle at each of those positions, the third with the opcode as the granule's
LAST byte. Its negative control is a DIFFERENT build to arm 3's:
-DX86_CODE_THREAD_UNSAFE=1 keeps the threading and drops the proof.

Run:
    python3 tests/test_seam.py                # arms 1 and 2 + the code arm
    python3 tests/test_seam.py --negative     # arm 3, needs a -DX86_SEAM_FIX=0
                                              # build (--clean is forced)
    python3 tests/test_seam.py --code-negative
                                              # the code-stream control, needs a
                                              # -DX86_CODE_THREAD_UNSAFE=1 build
"""
import argparse
import os
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

HALT = 6   # X86_EXIT_HALT


def run_arm(b, syms, contiguous):
    b.write_uint32(syms["seam_contiguous"], 1 if contiguous else 0)
    b.write_uint32(syms["run_seam_flag"], 1)
    for _ in range(400):
        if b.read_uint32(syms["run_seam_flag"]) == 0:
            break
        time.sleep(0.05)
    else:
        return None
    return [b.read_uint32(syms["seam_r"] + 4 * i) for i in range(24)]


def check(name, got, want, ok=True):
    """`ok=False` inverts the sense -- the negative control asserts the value is
    WRONG, and a negative control that cannot say what wrong looks like is not
    a control."""
    good = (got == want) if ok else (got != want)
    print(f"  {'PASS' if good else 'FAIL'}: {name}: "
          f"{got:#x} ({'want' if ok else 'must not be'} {want:#x})")
    return good


CODE_CASES = (
    (0, "DISPLACEMENT across the boundary  (8B 06 20 00 at 0FFDh)"),
    (1, "IMMEDIATE across the boundary     (B8 34 12 at 0FFEh)"),
    (2, "OPCODE is the granule's last byte (89 06 40 00 at 0FFFh)"),
)


def run_code_arm(b, syms, case):
    b.write_uint32(syms["code_seam_case"], case)
    b.write_uint32(syms["run_code_seam_flag"], 1)
    for _ in range(400):
        if b.read_uint32(syms["run_code_seam_flag"]) == 0:
            break
        time.sleep(0.05)
    else:
        return None
    return [b.read_uint32(syms["code_seam_r"] + 4 * i) for i in range(16)]


def code_seam(b, syms, negative):
    """THE CODE-STREAM SEAM.

    The dispatch threads the opcode granule's fold constant to every later
    fetch of the same instruction. `host = lin + k` holds for ONE granule, so
    an instruction that crosses the boundary would take its displacement, its
    immediate or its very ModRM out of the low granule's frame. These three
    place the straddle at each of those positions in turn, on the SAME
    deliberately non-contiguous fold the operand arm uses.

    `negative` is for a -DX86_CODE_THREAD_UNSAFE=1 build, which threads without
    proving the instruction fits. Every straddling assertion is then inverted:
    if the arm still passes, it is not testing the straddle."""
    ok = True
    print("code-stream seam, non-contiguous fold:")
    for case, name in CODE_CASES:
        r = run_code_arm(b, syms, case)
        if r is None:
            print(f"  FAIL: target never ran code case {case}")
            return False
        print(f"  case {case}: {name}")
        ok &= check("   the low granule has slack at all", r[6], 1)
        ok &= check("   the two granules are NOT contiguous", r[7], 1)
        # THE EXECUTED RESULT is the assertion, and the only one inverted.
        # Whether the guest still reaches HLT is NOT a property of the defect:
        # a corrupted displacement lands on an unmapped page and faults, but a
        # corrupted immediate executes perfectly and merely computes the wrong
        # number -- case 1 does exactly that. Inverting the exit reason would
        # have made the control fail for the wrong reason on two cases out of
        # three and pass on none, which is not a control, it is a coincidence.
        if negative:
            print(f"       exit reason {r[0]:#x} "
                  f"({'HLT' if r[0] == HALT else 'not HLT'}) -- informational")
        else:
            ok &= check("   guest ran to HLT", r[0], HALT)
        ok &= check("   the executed result", r[1], r[2], not negative)
        if negative:
            print(f"     (AX={r[3]:#x}, eip={r[10]:#x}, slack[0]={r[9]:#x}; "
                  f"0xEE is the painted slack, so an 0xEE in the operand is "
                  f"the instruction stream being read off the wrong page)")
        else:
            # The staging path is what makes the straddle correct, so it has
            # to have RUN. A case that passes without relocating passed by
            # accident and the control below would not catch it.
            good = r[4] > 0
            print(f"  {'PASS' if good else 'FAIL'}:    the instruction was "
                  f"STAGED: x86_code_relocs moved by {r[4]}")
            ok &= good
    return ok


def main(args):
    runner = build_and_create_runner(args)
    ok = True
    try:
        runner.start()
        b = runner.backend
        syms = get_symbols(getattr(args, "elf", None) or "test_core.elf")
        for n in ("run_seam_flag", "seam_r", "seam_contiguous",
                  "run_code_seam_flag", "code_seam_r", "code_seam_case"):
            if n not in syms:
                print(f"missing symbol {n}")
                return False
        time.sleep(0.5)

        # ---- the code-stream seam -------------------------------------
        # Runs first: it reinstalls the fold itself and leaves the granules
        # unmapped again, so the operand arms below are unaffected.
        ok &= code_seam(b, syms, args.code_negative)
        if args.code_negative:
            # The operand arms are not what this build perturbs, and running
            # them would only add noise to a control's output.
            print("PASS" if ok else "FAIL")
            return ok

        # ---- arm 1: the real case -------------------------------------
        r = run_arm(b, syms, contiguous=False)
        if r is None:
            print("FAIL: target never ran the seam arm")
            return False
        want = not args.negative
        print("non-contiguous fold, straddling accesses:")
        ok &= check("the low granule has slack at all", r[11], 1)
        ok &= check("the two granules are NOT contiguous", r[12], 1)
        ok &= check("guest ran to HLT", r[0], HALT)
        # 0xAA is the low granule's last byte, 0xBB the neighbour's first.
        ok &= check("16-bit read across the boundary", r[1], 0xBBAA, want)
        # 0x99 at 0xFFE, then the neighbour's 0xBB 0xCC 0xDD.
        ok &= check("32-bit read, two bytes over", r[2], 0xCCBBAA99, want)
        ok &= check("straddling write, low half landed", r[3], 0x34)
        ok &= check("straddling write, high half landed", r[4], 0x12, want)
        ok &= check("the neighbour's next byte untouched", r[5], 0xCC)
        if args.negative:
            # Say what the defect actually did, so the control is a
            # measurement and not just an inverted assertion.
            print(f"  (slack after the low granule now holds {r[6]:#x}; "
                  f"0xEE means the read saw it, 0x12 means the write went "
                  f"there instead of into the neighbour)")
        else:
            ok &= check("the seam was SERVED, not refused", r[10], 0)
            good = r[8] > 0
            print(f"  {'PASS' if good else 'FAIL'}: x86_seam_syncs moved: {r[8]}")
            ok &= good
            good = r[9] > 0
            print(f"  {'PASS' if good else 'FAIL'}: x86_seam_writes moved: {r[9]}")
            ok &= good

        # ---- arm 2: the contiguous control ----------------------------
        # Always asserted POSITIVELY, in both builds: with the granules laid
        # back to back there is no seam, so this must pass even with the fix
        # compiled out. A failure here is the program or the harness, never
        # the seam.
        r = run_arm(b, syms, contiguous=True)
        print("contiguous fold (control -- no seam can exist, "
              "asserted POSITIVELY in both builds):")
        ok &= check("guest ran to HLT", r[0], HALT)
        ok &= check("16-bit read across the boundary", r[1], 0xBBAA)
        ok &= check("32-bit read, two bytes over", r[2], 0xCCBBAA99)
        ok &= check("straddling write, low half landed", r[3], 0x34)
        ok &= check("straddling write, high half landed", r[4], 0x12)
    finally:
        runner.stop()
    print("PASS" if ok else "FAIL")
    return ok


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    add_runner_args(p)
    p.add_argument("--negative", action="store_true",
                   help="expect the DEFECT: for a -DX86_SEAM_FIX=0 build")
    p.add_argument("--code-negative", action="store_true",
                   help="expect the CODE-STREAM defect: builds with "
                        "-DX86_CODE_THREAD_UNSAFE=1, which threads the opcode "
                        "granule's fold to every fetch WITHOUT proving the "
                        "instruction fits in it (--clean is forced)")
    a = p.parse_args()
    if a.code_negative:
        os.environ["X86_CODE_THREAD_UNSAFE"] = "1"
        a.clean = True
    if a.negative:
        # The control has to be a different BUILD, and a stale object file
        # would silently make it the same one.
        os.environ["X86_SEAM_FIX"] = "0"
        a.clean = True
    sys.exit(0 if main(a) else 1)
