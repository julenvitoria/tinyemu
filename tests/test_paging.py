#!/usr/bin/env python3
"""
Tier-1 checks for the guest memory ARCHITECTURE: the A20 gate, the real-mode
wrap it controls, and (as later stages land) the resident set, demand paging
and eviction.

Why this is its own harness rather than more arms on test_nor.py: test_nor
covers one BACKING TIER and its copy-on-write. What is checked here is the
address space those tiers live in -- which pages exist at all, and which page a
guest address resolves to. Those fail in different ways and the counters that
name them are different.

The A20 arms exist because of a specific defect. x86_mem_wrap_1mb() used to
alias every fold entry from 1 MB to 16 MB onto the low megabyte -- an 8086's
20-bit address space. That is not merely inaccurate: with all 3,840 entries
above 1 MB held as aliases there IS no memory above 1 MB, and the 8 MB guest
space cannot exist. It was invisible because nothing had ever mapped anything
up there, so no test could tell a correct wrap from a total one.
"""
import sys
import time

sys.path.insert(0, ".")
from scripts.common.runner import add_runner_args, build_and_create_runner
from scripts.common.elf import get_symbols

MEM_FAULT = 5   # X86_EXIT_MEM_FAULT
HALT = 6        # X86_EXIT_HALT -- the guest ran to its HLT


def run_arm(b, syms, flag, result, count):
    """Trigger one target-side arm and read its result block back."""
    b.write_uint32(syms[flag], 1)
    for _ in range(400):
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
        for n in ("run_a20_flag", "a20_r"):
            if n not in syms:
                print(f"missing symbol {n}")
                return False

        time.sleep(0.5)
        r = run_arm(b, syms, "run_a20_flag", "a20_r", 24)
        if r is None:
            print("FAIL: target never ran the A20 test")
            return False

        checks = [
            # ---- the wrap's SHAPE, which is where the defect was
            ("gate masked at boot",                     r[0], 0),
            ("HMA page 0 aliases linear 0",             r[1], 1),
            ("HMA page 15 aliases linear 0xF000",       r[2], 1),
            # These two fail on the old whole-space wrap and are the reason
            # the harness exists: 0x110000 and 0x200000 are extended memory,
            # not another copy of the bottom of RAM.
            ("0x110000 is NOT aliased",                 r[3], 1),
            ("0x200000 is NOT aliased",                 r[4], 1),

            # ---- the wrap WORKS, from guest code through a segment override
            ("guest ran to HLT",                        r[5], HALT),
            ("FFFF:0020 read the byte at 0000:0010",    r[6], 0x5A),
            ("  ... and that byte really was written",  r[7], 0x5A),

            # ---- the gate, opened by the guest through port 0x92
            ("guest OUT 92h opened the gate",           r[8], 1),
            ("exactly one transition",                  r[9], 1),
            # With NO POOL behind it, an HMA access is still a clean fault --
            # that is the pager having nothing to give, not the wrap. The HMA
            # arms below run the same access WITH a pool and require it to
            # succeed; the two together say the memory comes from the pager and
            # from nowhere else.
            ("HMA access faults with no pool",          r[10], MEM_FAULT),
            ("  ... because the HMA is unmapped",       r[11], 1),
            ("port 0x92 reads back open",               r[12], 0x02),

            # ---- and CLOSES again. A gate tested only in the opening
            # direction passes while the closing half does nothing.
            ("masking it again",                        r[13], 0),
            ("two transitions",                         r[14], 2),
            ("the alias is restored",                   r[15], 1),

            # ---- the 8042 doors
            ("0x64=D1 then 0x60=02 opens it",           r[16], 1),
            ("0x64=D0 reports it on 0x60",              r[17], 0x02),
            ("0x64=DD masks it",                        r[18], 0),
            ("0x64=DF opens it",                        r[19], 1),
            ("a non-D1 command clears the latch",       r[20], 1),
            ("probing an open gate is not a change",    r[21], 1),

            ("left masked for the next test",           r[22], 0),
            ("host-readable mirror agrees",             r[23], 0),
        ]

        # ---- the VRAM arena, its all-or-nothing rule, and the pin state ----
        a = run_arm(b, syms, "run_arena_flag", "arena_r", 32)
        if a is None:
            print("FAIL: target never ran the arena test")
            return False

        KB = 1024
        REGION = 40 * 4096          # 163,840 -- the harness picks this, not the
                                    # bonus pool, so every figure below is the
                                    # test's own arithmetic
        checks += [
            ("arena took the region",               a[0], 1),
            ("arena size",                          a[1], REGION),
            ("all of it free",                      a[2], REGION),

            ("CGA claim (32 KB)",                   a[3], 1),
            ("  ... used",                          a[4], 32 * KB),
            ("mode 13h claim (64 KB)",              a[5], 1),
            ("  ... used",                          a[6], 96 * KB),
            ("  ... free",                          a[7], REGION - 96 * KB),
            ("the two blocks do not overlap",       a[8], 1),

            # The rule that unblocks EGA. 4 planes of 64 KB against 64 KB free.
            ("EGA refused",                         a[9], 1),
            ("  ... deficit is 3 planes",           a[10], 3 * 64 * KB),
            ("  ... and NOTHING was allocated",     a[11], 1),
            ("  ... out[] untouched",               a[12], 1),
            ("the arena still works after it",      a[13], 1),

            ("display claims released",             a[14], REGION),
            ("EGA still refused on an empty arena", a[15], 1),
            ("  ... deficit is 2 planes",           a[16], 2 * 64 * KB),

            ("would_fit agrees: 4 planes no",       a[17], 1),
            ("would_fit agrees: 2 planes yes",      a[18], 1),
            ("  ... and allocated nothing",         a[19], 0),

            ("re-claim: first",                     a[20], 1),
            ("re-claim: second",                    a[21], 1),
            ("  ... released the old one",          a[22], 32 * KB),

            ("pinning 8 pages",                     a[23], 8),
            ("  ... the page is pinned",            a[24], 1),
            ("  ... its neighbour is not",          a[25], 0),
            ("  ... pinning is idempotent",         a[26], 8),
            ("  ... and unpins",                    a[27], 0),

            # .bss zeroes make page 0 look live from boot unless the ring is
            # armed. Page 0 is real and constantly used, so this is not
            # hypothetical -- it would veto its eviction forever.
            ("empty ring holds nothing (page 0)",   a[28], 1),
            ("the ring holds what it was told",     a[29], 1),
            ("  ... and clears",                    a[30], 1),
            ("arena left empty",                    a[31], 0),
        ]

        # ---- demand paging, driven by guest instructions -------------------
        p = run_arm(b, syms, "run_pager_flag", "pager_r", 96)
        if p is None:
            print("FAIL: target never ran the pager test")
            return False

        checks += [
            ("pager took 4 frames",                 p[0], 1),
            ("  ... frames total",                  p[1], 4),
            ("  ... all free",                      p[2], 4),

            # A guest store to a page that does not exist yet.
            ("guest ran to HLT",                    p[3], HALT),
            ("the byte read back",                  p[4], 0x5A),
            ("exactly one write fault",             p[5], 1),
            ("no read fault: it was resident",      p[6], 0),
            ("filled from the ZERO tier",           p[7], 1),
            ("one page resident",                   p[8], 1),
            ("one frame taken",                     p[9], 3),
            ("the page is on the FAST path now",    p[10], 1),

            # Five pages against four frames. This used to be the exhaustion
            # arm; with a victim picker it is an EVICTION, and the page that
            # leaves held a byte the guest wrote, so it must be WRITTEN.
            ("the 5th page pages in, no fault",     p[11], HALT),
            ("four resident: the pool size",        p[12], 4),
            ("no frames left",                      p[13], 0),
            ("nothing was exhausted",               p[14], 0),
            ("exactly one eviction",                p[15], 1),
            ("  ... written back: it was dirty",    p[16], 1),
            ("  ... NOT dropped",                   p[17], 0),
            ("  ... 4096 bytes",                    p[18], 4096),
            ("  ... one backing-store write",       p[19], 1),
            ("  ... no I/O errors",                 p[20], 0),
            ("the victim is off the fast path",     p[21], 1),

            # The round trip. Everything above can pass with a writeback that
            # wrote the wrong slot; this cannot.
            ("read the evicted page back",          p[22], HALT),
            ("  ... the guest's byte survived",     p[23], 0x5A),
            ("  ... filled from the FILE tier",     p[24], 1),
            ("  ... one backing-store read",        p[25], 1),

            # THE RULE THE WHOLE DESIGN IS FOR: a ZERO page that is still zero
            # is dropped with no SD I/O at all.
            ("still-zero page: one eviction",       p[26], 1),
            ("  ... DROPPED clean",                 p[27], 1),
            ("  ... no writeback",                  p[28], 0),
            ("  ... the pagefile was not touched",  p[29], 0),

            # The negative control for it. One non-zero byte, at the far END of
            # the page, and the same eviction must write.
            ("dirtied page: one eviction",          p[30], 1),
            ("  ... NOT dropped",                   p[31], 0),
            ("  ... written back",                  p[32], 1),
            ("  ... the pagefile WAS touched",      p[33], 1),

            # The two vetoes. 5 evictions of a live-mapped page produced 26,329
            # wrong bytes in the prior port with its own counters green.
            ("no legal victim: refused",            p[34], 1),
            ("  ... exhaustion counted",            p[35], 1),
            ("  ... the ring vetoed one",           p[36], 1),
            ("  ... nothing was evicted",           p[37], 2),
            ("lift the pin and it succeeds",        p[38], 1),
            ("  ... and does not re-count",         p[39], 1),

            # The host path carries every DOS program load.
            ("host fault now pages in",             p[40], 1),

            # An address outside the window is not a page fault at all.
            ("outside the window is unmapped",      p[41], 1),
            ("  ... counted separately",            p[42], 1),

            ("frames handed back",                  p[43], 0),
            ("no stray pagefile slot",              p[44], 0),

            # ---- donation: spare guest frames become an EGA card -----------
            ("a pinned run refuses donation",       p[45], 1),
            ("  ... loudly",                        p[46], 1),
            ("  ... and nothing moved",             p[47], 5),
            ("past the reserve is refused",         p[48], 1),
            ("  ... loudly",                        p[49], 2),
            # No pagefile means eviction is data loss, so only FREE frames may
            # go. src/main.c wires no backing store, so this is the rule that
            # governs on the target.
            ("no backing store: resident refused",  p[50], 1),
            ("  ... loudly",                        p[51], 3),
            ("donated bytes",                       p[52], 16384),
            ("  ... four frames",                   p[53], 4),
            ("  ... one eviction: cheapest run",    p[54], 1),
            ("  ... guest RAM shrank to 1",         p[55], 1),
            ("  ... the arena grew",                p[56], 16384),
            ("the guest still pages on what is left", p[57], 1),
            ("the arena can claim it",              p[58], 1),
            ("  ... in the donated extent",         p[59], 1),
            ("reclaim refused while claimed",       p[60], 1),
            ("  ... counted",                       p[61], 1),
            ("  ... frames stay lent",              p[62], 1),
            ("released: reclaim succeeds",          p[63], 1),
            ("  ... guest RAM whole again",         p[64], 5),
            ("  ... arena empty",                   p[65], 0),
            ("  ... nothing donated",               p[66], 0),
            # A mode change donates and reclaims every time 0Dh comes and goes.
            ("donate/reclaim round 2",              p[67], 1),
            ("  ... reclaimed",                     p[68], 1),
            ("donate/reclaim round 3",              p[69], 1),
            ("  ... reclaimed",                     p[70], 1),
            ("  ... the pool did not fragment",     p[71], 5),

            # ---- the reserve is the WORKING SET, not a card size -----------
            # KEEN4's wedge, scaled to test_ram. A reserve at or above the
            # working set means the donation costs no pagefile I/O at all;
            # a reserve below it means every pass round the set writes 4 KB to
            # the SD card, which is free here and up to 433 ms on silicon.
            ("reserve >= working set: no SD writes", p[72], 0),
            ("  ... and the pool is whole",          p[73], 8),
            # The negative control for that arm, in the test rather than in a
            # reviewer's head.
            ("reserve < working set: it thrashes",   p[74], 1),
            ("  ... because the pool shrank",        p[75], 1),
            # The check that was absent: the SHIPPING constant against the
            # peak recorded in the same header. It was 100 against 134.
            ("shipping reserve covers KEEN4's peak", p[76], 1),

            # ---- THE POLICY: the same memory, decided again every frame ----
            # Everything above grades the MECHANISM against a fixed reserve.
            # These grade the DECISION -- borrow while the frames are idle,
            # hand them back before the guest evicts -- on an 8-frame pool with
            # the floor and margin scaled to it (tests/main.c).
            ("idle pool: the policy offers to lend", p[77], 1),
            ("  ... and says how much",              p[78], 1),
            ("frames borrowed",                      p[79], 1),
            ("  ... for NOTHING: no eviction",       p[80], 0),
            ("  ... guest RAM shrank",               p[81], 1),

            # The half that did not exist: the working set grows into what was
            # lent, and the watermark fires while the pool still has runway.
            ("the working set grows: SHRINK",        p[82], 1),
            ("  ... BEFORE the first eviction",      p[83], 0),
            ("  ... counted",                        p[84], 1),
            ("released, then reclaimed",             p[85], 1),
            ("  ... the pool is whole again",        p[86], 8),
            ("  ... and it LEARNED the working set", p[87], 1),
            ("re-evaluated: it now lends less",      p[88], 1),

            # NEGATIVE CONTROL for the shrink, run rather than argued: the same
            # walk with the SHRINK thrown away. The evictions the arm above
            # proves absent must appear here, or that arm measures nothing.
            ("shrink ignored: it evicts",            p[89], 1),
            ("  ... and writes to the pagefile",     p[90], 1),
            # NEGATIVE CONTROL for the grow: policy off, static reserve at the
            # pool size -- the tree's behaviour before this work.
            ("grow disabled: nothing is lent",       p[91], 0),
            ("  ... the pool is untouched",          p[92], 8),
        ]

        # ---- the HMA: backed when A20 is open, aliased when it is not ------
        h = run_arm(b, syms, "run_hma_flag", "hma_r", 32)
        if h is None:
            print("FAIL: target never ran the HMA test")
            return False

        checks += [
            # Gate OPEN: the sixteen pages at 1 MB are real memory. This is
            # what DOS4GW's memory hunt and Mortal Kombat's wrap test hit.
            ("A20 open: the guest ran to HLT",      h[0], HALT),
            ("  ... the HMA byte read back",        h[1], 0x22),
            # THE CORRUPTION ARM: an HMA still aliased with the gate open
            # would have overwritten the low-memory marker with 0x22.
            ("  ... low memory is NOT the same byte", h[2], 0x11),
            ("  ... exactly one write fault",       h[3], 1),
            ("  ... filled from the ZERO tier",     h[4], 1),
            ("  ... one page resident",             h[5], 1),
            ("  ... and on the fast path",          h[6], 1),
            ("  ... the mirror says open",          h[7], 1),

            # Gate MASKED: the ALIAS is what the guest sees, and a write
            # through it lands in LOW memory, not in the HMA frame behind it.
            ("masked: the guest ran to HLT",        h[8], HALT),
            ("  ... it reads the ALIAS, not the HMA", h[9], 0x11),
            ("  ... and writes through it to low RAM", h[10], 0x33),
            ("  ... nothing faulted: no frame handed out", h[11], 0),
            ("  ... the HMA frame is still held",   h[12], 1),

            # Reopened: the page goes back to ITS OWN frame, unrefilled.
            ("reopened: the guest ran to HLT",      h[13], HALT),
            ("  ... the HMA data survived the toggle", h[14], 0x22),
            ("  ... and was never refilled",        h[15], 0),
            ("  ... low RAM kept what the alias wrote", h[16], 0x33),

            # The seam INSIDE the HMA: 16 pages in fault order are 16
            # unrelated frames, so 0x100FFF straddles two of them.
            ("straddle inside the HMA: ran to HLT", h[17], HALT),
            ("  ... the 16-bit value is whole",     h[18], 0x1234),
            ("  ... the high byte is in the NEXT page", h[19], 0x12),
            ("  ... the seam SERVED it",            h[20], 1),
            ("  ... and refused nothing",           h[21], 0),
            ("  ... two pages resident",            h[22], 2),

            # The 1 MB line itself. Page 255 is a fixed mapping with no slack,
            # so a straddle there is refused -- and COUNTED, which is the whole
            # point of x86_seam_overflow existing.
            ("straddle across 1 MB: ran to HLT",    h[23], HALT),
            ("  ... refused, and COUNTED",          h[24], 1),
            ("  ... the low byte is still right",   h[25], 0xA5),

            # The window comes and goes with the gate.
            ("open: the HMA is the pager's",        h[26], 1),
            ("masked: it is not ours",              h[27], 1),
            ("  ... counted as unmapped",           h[28], 1),
            ("  ... and the alias is intact",       h[29], 1),
            ("left masked for the next test",       h[30], 0),
            ("host-readable mirror agrees",         h[31], 0),
        ]

        # ---- the pagefile SLOT RING (x86_pgfile.c) -------------------------
        # WHERE an evicted page's bytes land, which stopped being "at its own
        # page number" once the card was measured: a 4 KB append past EOF is
        # 12-14 ms on silicon and a 4 KB rewrite of recently-written sectors is
        # 60-67 ms. See x86_pgfile.h for the whole table.
        g = run_arm(b, syms, "run_pgfile_flag", "pgfile_r", 24)
        if g is None:
            print("FAIL: target never ran the pagefile-ring test")
            return False

        checks += [
            ("pagefile: a page claims a slot",       g[0], 1),
            ("  ... and reads back byte-identical",  g[1], 1),
            # The append property itself. Same page, second write, new slot.
            ("rewrite APPENDS to a new slot",        g[2], 1),
            ("  ... the old slot is freed",          g[3], 1),
            ("  ... the new slot is live",           g[4], 1),
            ("  ... and the NEW bytes come back",    g[5], 1),
            # NEGATIVE CONTROL: corrupt the writeback under the mapped slot.
            ("CONTROL: corrupted slot is CAUGHT",    g[6], 1),
            ("  ... and uncorrupting it passes",     g[7], 1),
            # NEGATIVE CONTROL: read the copy the page had BEFORE its last
            # write -- what a refill that overtook a pending write would see.
            ("CONTROL: the stale copy still exists", g[8], 1),
            ("CONTROL: stale != current, so a read " "that took it is CAUGHT",
                                                     g[9], 1),
            # Ordering under failure: the map moves only on success.
            ("a FAILED write reports failure",       g[10], 1),
            ("  ... the page does not move",         g[11], 1),
            ("  ... its previous copy is intact",    g[12], 1),
            # The ring's one hard rule, over a full wrap.
            ("every pageable page got a slot",       g[13], 12),
            ("a LIVE slot is never handed out",      g[14], 0),
            ("  ... and the ring never ran dry",     g[15], 0),
            ("every page still reads back its own",  g[16], 0),
            ("  ... having actually wrapped",        g[17], 1),
            # Outside both windows: refused, and loudly.
            ("an unpageable page is refused",        g[18], 1),
            ("  ... and counted",                    g[19], 1),
            ("  ... and has no slot",                g[20], 1),
            ("reset forgets the map",                g[21], 1),
            ("  ... so a refill has nothing to read", g[22], 1),
            ("  ... and every slot is free",         g[23], 1),
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
