"""
What we SHOULD have returned: a curated expectations table for the probes DOS
titles actually make.

------------------------------------------------------------------------------
WHY A TABLE AND NOT A SECOND EMULATOR
------------------------------------------------------------------------------
The obvious way to answer "was that the right answer" is to ask something else
the same question. Three candidates existed and only one of them earns its cost:

  * **Extend scripts/common/refdos.py with port I/O.** refdos is a minimal
    reference DOS, deliberately not modelled on x86_dos.c so the two cannot
    agree on the same mistake -- but it has no hardware at all. Giving it a
    CRTC, a DAC, a PIT and a PIC to compare port answers against is writing
    emulator number two, and every disagreement then needs adjudicating between
    two implementations that are both ours. Rejected: highest cost, and it
    still cannot say what a REAL PC returns.

  * **Drive DOSBox and capture its INT/port log.** dosbox is on this machine
    (`/home/doug/.local/bin/dosbox`), so this is feasible. It answers "what
    does DOSBox do", which is close to but not the same as "what does the game
    need", and only for the code path DOSBox's own quirks led the title down --
    a title that takes the EGA branch under DOSBox never exercises the CGA
    branch we care about. It also needs a debug build, a matching floppy boot,
    and a log parser, and it re-runs on every investigation. Rejected as the
    PRIMARY oracle; it stays the right tool for a single stubborn disagreement.

  * **Annotate our own answers against documented behaviour (this file).**
    Costs nothing at runtime, needs no second implementation, is reviewable in
    a diff, and answers the question that is actually being asked -- "is the
    shape of this answer the shape a real machine gives". It is also the only
    option that can classify a STUB, because a stub and a real handler are
    behaviourally identical to any oracle right up to the moment the guest acts
    on the answer.

So: this table. It is RBIL-shaped -- vector, function, subfunction, what a real
BIOS/DOS returns -- plus the one thing RBIL cannot know, which is what OUR
handler does about it.

------------------------------------------------------------------------------
HOW IT STAYS HONEST
------------------------------------------------------------------------------
A curated table drifts from the source it describes. Two things stop that from
being silent:

  * **Runtime evidence outranks the table.** If the target observed rc==0 or saw
    x86_hle_unhandled_* move, the report says `unanswered` no matter what this
    file claims. The table can only classify calls the target says were
    answered.
  * **Absence is reported, not defaulted.** A (vector, AH) with no entry here is
    reported as `unclassified` and ranked HIGH, not quietly as `implemented`.
    A new probe appearing in a title is exactly the thing the report exists to
    surface, and a table that swallowed it would defeat the tool.

Status values, worst first in the report:

    unanswered        nobody claimed it; the guest got whatever was in AX
    default-arm       a catch-all produced a shaped, invented answer
    unmodelled-port   a port nothing in x86_io.c has heard of
    unclassified      answered, but nobody has vetted the answer (add an entry)
    stub              returns success and does nothing
    partial           right for some inputs, falls through for the rest
    absent-by-design  we answer "this hardware is not here", and that is TRUE
                      of the machine we claim to be. Not a bug -- but it IS the
                      answer a title may be failing on, so it is listed.
    implemented       modelled
"""

# Severity drives the ranking. The point of the report is the top section, and
# the top section is these numbers.
SEVERITY = {
    "unanswered": 100,
    "default-arm": 90,
    "unmodelled-port": 85,
    "unclassified": 70,
    "stub": 60,
    "partial": 50,
    "absent-by-design": 20,
    "implemented": 10,
}

ACTIONABLE = {"unanswered", "default-arm", "unmodelled-port", "unclassified"}


def _e(status, ours, real=None):
    return {"status": status, "ours": ours, "real": real}


# ---------------------------------------------------------------------------
# INT services. Key is (vector, AH) or (vector, AH, SUB) -- the more specific
# entry wins. SUB is AL, except INT 10h AH=12h where it is BL (x86_probe.c
# picks which byte, and this table has to agree with it).
# ---------------------------------------------------------------------------
INT = {
    # ---- INT 10h, video BIOS -------------------------------------------
    (0x10, 0x00): _e("partial",
                     "sets the mode if x86_video renders it; REFUSES and logs "
                     "event tag 5 otherwise, leaving the old mode up",
                     "a real BIOS programs whatever the adapter has"),
    (0x10, 0x00, 0x00): _e("implemented", "CGA text 40x25 mono"),
    (0x10, 0x00, 0x01): _e("implemented", "CGA text 40x25 colour"),
    (0x10, 0x00, 0x02): _e("implemented", "CGA text 80x25 mono"),
    (0x10, 0x00, 0x03): _e("implemented", "CGA text 80x25 colour -- the mode "
                                          "most DOS titles start in"),
    (0x10, 0x00, 0x04): _e("implemented", "CGA 320x200 4-colour, LUT8 blitter"),
    (0x10, 0x00, 0x05): _e("implemented", "CGA 320x200 4-colour, mono palette"),
    (0x10, 0x00, 0x06): _e("implemented", "CGA 640x200 2-colour"),
    (0x10, 0x00, 0x0D): _e("partial",
                           "ADOPTED: EGA 320x200 planar renders (mode log tag "
                           "1, value 0x0D). The CARD is 8,192 plane offsets "
                           "(32 KB); stores past that are discarded and "
                           "counted in x86_vga_planar_lost",
                           "a real EGA has 64 KB per plane. Keen 4's level "
                           "art lives above our card and draws as black"),
    (0x10, 0x00, 0x0E): _e("absent-by-design", "REFUSED: EGA 640x200 planar"),
    (0x10, 0x00, 0x10): _e("absent-by-design", "REFUSED: EGA 640x350 planar"),
    (0x10, 0x00, 0x12): _e("absent-by-design", "REFUSED: VGA 640x480 planar"),
    (0x10, 0x00, 0x13): _e("partial",
                           "MCGA 320x200x256 -- accepted only when the 64 KB "
                           "aperture is backed (x86_vga13_ok); REFUSED "
                           "otherwise, which is a whole mode silently missing"),
    (0x10, 0x01): _e("stub",
                     "accepted, does nothing: the cursor SHAPE is not modelled",
                     "sets the hardware cursor scanlines. Harmless to ignore "
                     "unless a title hides the cursor and expects it gone"),
    (0x10, 0x02): _e("implemented", "set cursor position"),
    (0x10, 0x03): _e("implemented", "get cursor position and shape"),
    (0x10, 0x05): _e("stub", "accepted, does nothing: one display page only",
                     "a real CGA has 4-8 text pages and titles page-flip"),
    (0x10, 0x06): _e("implemented", "scroll window up"),
    (0x10, 0x08): _e("implemented", "read char + attribute at cursor"),
    (0x10, 0x09): _e("implemented", "write char + attribute"),
    (0x10, 0x0A): _e("implemented", "write char, keep attribute"),
    (0x10, 0x0B): _e("implemented", "set CGA palette / background"),
    (0x10, 0x0C): _e("implemented", "write graphics pixel"),
    (0x10, 0x0E): _e("implemented", "teletype output"),
    (0x10, 0x0F): _e("implemented", "get current mode"),
    (0x10, 0x10): _e("partial",
                     "VGA palette/DAC, sharing the DAC the 0x3C8/0x3C9 ports "
                     "drive. Only some AL subfunctions are modelled; the rest "
                     "fall through to the unhandled arm"),
    (0x10, 0x11): _e("partial",
                     "AL=30h returns the 8x8 font at F000:F000. Every other "
                     "AL is unanswered",
                     "a CGA BIOS has no AH=11h at all"),
    (0x10, 0x11, 0x30): _e("implemented", "font pointer, ES:BP = F000:F000, "
                                          "CX=8, DL=24"),
    (0x10, 0x12): _e("absent-by-design",
                     "returns with BL UNCHANGED, which is exactly what a "
                     "machine with no EGA does",
                     "an EGA returns BL=memory size. 0Dh renders now, so this "
                     "is no longer a wall -- but the answer still describes "
                     "this machine, which has 32 KB of plane storage and not "
                     "the 256 KB the BL codes describe"),
    (0x10, 0x1A): _e("implemented",
                     "returns AL=1Ah, BX=0x0008 -- VGA with a colour analogue "
                     "display, since 5de7e70. Measured, not assumed: the "
                     "report prints AX=0x1a1a for Keen 4 and Wolf3D alike",
                     "AL=0 was the old 'not supported' answer; it cost Wolf3D "
                     "its VW_VideoID and is not what this BIOS says any more"),
    (0x10, 0x13): _e("unclassified", "write string -- not in the switch"),

    # ---- Hardware interrupts and INT 20h. AH carries nothing here, and
    # x86_probe.c forces it to 0 so these do not become a row per passing
    # register value.
    (0x08, 0x00): _e("implemented",
                     "the timer tick stub returns immediately; the BDA tick "
                     "COUNT is advanced by the frame loop, not here",
                     "a title that hooks INT 08h never reaches this stub at "
                     "all, and the count has to keep moving regardless"),
    (0x09, 0x00): _e("implemented", "keyboard IRQ: pushes the scancode's "
                                    "translation into the BDA ring"),
    (0x20, 0x00): _e("implemented", "terminate program"),

    # ---- INT 16h, keyboard BIOS ----------------------------------------
    (0x16, 0x00): _e("implemented",
                     "blocking read; returns rc=2 and REWINDS the guest IP so "
                     "the INT re-executes next slice rather than spinning"),
    (0x16, 0x01): _e("implemented", "peek, ZF=1 when the buffer is empty"),
    (0x16, 0x02): _e("implemented", "shift flags from the BDA"),
    (0x16, 0x10): _e("implemented", "extended blocking read (same path as 00)"),
    (0x16, 0x11): _e("implemented", "extended peek (same path as 01)"),
    (0x16, 0x03): _e("unclassified", "set typematic rate -- not in the switch"),
    (0x16, 0x05): _e("unclassified", "push keystroke -- not in the switch"),
    (0x16, 0x12): _e("unclassified", "extended shift flags -- not in the switch"),

    # ---- INT 1Ah, time -------------------------------------------------
    (0x1A, 0x00): _e("implemented", "read tick count from the BDA"),
    (0x1A, 0x01): _e("implemented", "set tick count"),
    (0x1A, 0x02): _e("unclassified", "read RTC time -- not in the switch"),
    (0x1A, 0x04): _e("unclassified", "read RTC date -- not in the switch"),

    # ---- INT 21h, DOS --------------------------------------------------
    (0x21, 0x00): _e("implemented", "terminate"),
    (0x21, 0x01): _e("implemented", "input with echo"),
    (0x21, 0x02): _e("implemented", "character output"),
    (0x21, 0x06): _e("implemented", "direct console I/O"),
    (0x21, 0x07): _e("implemented", "direct input, no echo"),
    (0x21, 0x08): _e("implemented", "input, no echo"),
    (0x21, 0x09): _e("implemented", "$-terminated string output"),
    (0x21, 0x0B): _e("implemented", "input status"),
    (0x21, 0x0C): _e("implemented", "flush then input"),
    (0x21, 0x0E): _e("implemented", "select drive"),
    (0x21, 0x19): _e("implemented", "get current drive"),
    (0x21, 0x1A): _e("implemented", "set DTA"),
    (0x21, 0x25): _e("implemented", "set interrupt vector"),
    (0x21, 0x2A): _e("implemented", "get date"),
    (0x21, 0x2C): _e("implemented", "get time"),
    (0x21, 0x2F): _e("implemented", "get DTA"),
    (0x21, 0x30): _e("implemented", "reports DOS 5.00"),
    (0x21, 0x35): _e("implemented", "get interrupt vector"),
    (0x21, 0x3C): _e("implemented", "create / truncate, via FatFs"),
    (0x21, 0x3D): _e("implemented", "open, via FatFs"),
    (0x21, 0x3E): _e("implemented", "close"),
    (0x21, 0x3F): _e("implemented", "read"),
    (0x21, 0x40): _e("implemented", "write"),
    (0x21, 0x41): _e("implemented", "delete"),
    (0x21, 0x42): _e("implemented", "seek"),
    (0x21, 0x43): _e("partial", "get/set file attributes"),
    (0x21, 0x43, 0x00): _e("implemented", "get attributes -> CX from f_stat"),
    (0x21, 0x43, 0x01): _e("stub",
                           "returns success and IGNORES the write: CX is not "
                           "applied to the file",
                           "DOS sets the attribute. A title that marks a save "
                           "file read-only and checks it back will disagree"),
    (0x21, 0x44): _e("partial", "IOCTL"),
    (0x21, 0x44, 0x00): _e("implemented",
                           "get device information: 0x80D3 for handles 0-2 "
                           "(CON), 0x0000 for a FatFs file on drive A"),
    (0x21, 0x44, 0x01): _e("stub", "set device information: accepted, ignored"),
    (0x21, 0x47): _e("implemented", "get current directory"),
    (0x21, 0x48): _e("implemented", "allocate memory"),
    (0x21, 0x49): _e("implemented", "free memory"),
    (0x21, 0x4A): _e("implemented", "resize memory block"),
    (0x21, 0x4C): _e("implemented", "terminate with return code"),
    # AH=52h is PARTIAL and the distinction matters to whoever reads this row:
    # the pointer and the first-MCB word at -2 are real, and the DPB, SFT,
    # CLOCK$, CON, buffer and CDS chains are deliberately 0xFFFF:0xFFFF, DOS's
    # end-of-chain marker. A guest that WALKS one stops; a guest that INDEXES
    # one faults. Neither is a fabricated structure, which is the point.
    (0x21, 0x52): _e("partial",
                     "list of lists (SysVars): ES:BX -> a real block with the "
                     "first MCB at -2; the DPB/SFT/CLOCK$/CON/buffer/CDS "
                     "chains are marked absent rather than invented"),
    (0x21, 0x29): _e("implemented",
                     "parse filename into FCB, per MS-DOS MakeFcb: AL = 0 / 1 "
                     "(wildcards) / 0xFF (bad drive), and SI advanced"),
    (0x21, 0x4E): _e("implemented", "find first"),
    (0x21, 0x4F): _e("implemented", "find next"),
    (0x21, 0x56): _e("implemented", "rename"),
    # Known-missing INT 21h functions that DOS titles genuinely call. Listed
    # even though the runtime marker will also catch them, because a name is
    # worth more than a number when the report is read.
    (0x21, 0x33): _e("unanswered", "get/set CTRL-BREAK flag"),
    (0x21, 0x38): _e("unanswered", "get country info / set country"),
    (0x21, 0x39): _e("unanswered", "mkdir"),
    (0x21, 0x3A): _e("unanswered", "rmdir"),
    (0x21, 0x3B): _e("unanswered", "chdir"),
    (0x21, 0x45): _e("unanswered", "dup handle"),
    (0x21, 0x46): _e("unanswered", "dup2 / force duplicate handle"),
    # AH=4Bh: AL=00h and AL=03h are implemented and X86_DOS_EXEC now ships as
    # 1, so both are live in the SHIPPING core. AL=01h (load, do not execute)
    # is still refused, which is what the bare (0x21, 0x4B) entry describes.
    (0x21, 0x4B): _e("unanswered", "EXEC: load and run a child program",
                     "a title that shells out to a setup program needs this"),
    (0x21, 0x4B, 0x00): _e("implemented",
                           "EXEC AL=00h load-and-go. Live in the shipping "
                           "core: EPIC_PINBALL's launcher gets past 'Please "
                           "run SETUP!' on it, loading an 83,912 B child with "
                           "x86_dos_load_short=0",
                           "load a child, build its PSP and transfer to it"),
    (0x21, 0x4B, 0x03): _e("implemented",
                           "EXEC AL=03h LOAD OVERLAY -- "
                           "the image is placed at the caller's segment and "
                           "relocated by the caller's factor; returns CF=0 "
                           "AX=DX=0 and transfers nothing",
                           "load an image into a caller-supplied segment; no "
                           "PSP, no allocation, no transfer of control"),
    (0x21, 0x4D): _e("unanswered", "get child return code"),
    (0x21, 0x50): _e("unanswered", "set current PSP"),
    (0x21, 0x51): _e("unanswered", "get current PSP"),
    (0x21, 0x59): _e("unanswered", "get extended error information",
                     "called right after a failure, so it appears in exactly "
                     "the traces one is already reading"),
    (0x21, 0x62): _e("unanswered", "get PSP address"),

    # ---- INT 15h, misc BIOS. NOT DISPATCHED AT ALL ---------------------
    # See NOTE_INT15 below: this vector never reaches x86_hle_dispatch, so a
    # guest calling it does not appear in the accumulator either.
    (0x15, 0x88): _e("unanswered", "get extended memory size"),
    (0x15, 0x87): _e("unanswered", "move extended memory block"),
    (0x15, 0xC0): _e("unanswered", "get system configuration"),

    # ---- INT 2Fh, the multiplex interrupt ------------------------------
    # ONLY AX=4300h/4310h is answered; every other subfunction returns with
    # the guest's registers untouched, which is what "no such driver" is
    # defined to look like. AH=43h therefore covers both the detection
    # (AL=80h) and the entry point (ES:BX = 0060:00C0, a RETF stub).
    (0x2F, 0x43): _e("implemented",
                     "XMS: detect (AL=80h) and hand out the control entry "
                     "point. MEASURED: WOLF3D takes it and moves 430,636 B "
                     "through AH=0Bh at level load"),
}

NOTE_INT15 = (
    "INT 15h is not in x86_hle_dispatch's switch and is not installed as a "
    "stub, so a guest calling it does NOT reach us and does NOT appear in this "
    "report. Absence of INT 15h rows is not evidence that nothing called it -- "
    "check x86_int_vec_log (`probe.py --dump x86_int_vec_log:16x4`) for vector "
    "0x15 landing on a bare IRET."
)


# ---------------------------------------------------------------------------
# Ports. Exact port first, then a range. Same status vocabulary.
# ---------------------------------------------------------------------------
PORT = {
    0x20: _e("partial", "8259A: EOI on write, IRR/ISR on read",
             "OCW3 selects which register the next read returns, which is how "
             "the Precision Zen Timer detects overflow -- unanswered, this "
             "port floated 0xFF and every TOPBENCH sub-timing came back 65535. "
             "The EOI itself still has nothing to model: delivery is a "
             "one-deep pending slot, not a priority stack"),
    0x21: _e("partial", "PIC interrupt mask: stored and read back",
             "code that saves the mask, masks a line and restores it reads "
             "this first, and 0xFF told it every line was already masked -- "
             "which it then wrote back. NOT enforced: the frame loop keeps "
             "delivering ticks and keys regardless"),
    0x40: _e("implemented", "PIT channel 0: the SYSTEM TIMER, counter and all",
             "the WRITE divisor drives how many IRQ 0s the frame loop owes, "
             "which is what makes Wolfenstein 3D's 140 Hz (0x214B) run at "
             "140 Hz instead of 18.2. The READ is the COUNTER, latchable and "
             "counting down in guest time -- it used to float 0xFF, a clock "
             "that never moves, and Alley Cat spun on it six million times in "
             "one run rather than the 49 k it takes now"),
    0x41: _e("unmodelled-port", "PIT channel 1 (DRAM refresh)"),
    0x43: _e("implemented", "PIT control word: access mode, counting mode and "
                            "the counter latch command, channels 0 and 2"),
    0x42: _e("implemented", "PIT channel 2: the speaker's tone, and a counter "
                            "that can be latched and read back"),
    0x60: _e("implemented", "8042 keyboard data"),
    0x61: _e("implemented", "speaker gate + data"),
    0x64: _e("partial", "reads status; writes are accepted and ignored"),
    0x201: _e("unmodelled-port", "game port: floats 0xFF",
              "0xFF means all four buttons UP and the axis timers still "
              "running -- a joystick probe reads that as 'present but stuck'. "
              "A title that calibrates here can hang"),
    0x330: _e("unmodelled-port", "MPU-401 MIDI"),
    0x388: _e("unmodelled-port", "AdLib / OPL2 status",
              "an AdLib detect writes the timer registers and reads 0x388 back "
              "expecting 0xC0. Floating 0xFF reads as 'no card', which is the "
              "safe answer -- but it IS the answer, and a title with no other "
              "sound path will be silent"),
    0x3B4: _e("absent-by-design", "MDA CRTC index alias: returns 0xFF unless "
                                  "the guest has selected mono decode",
              "answering this as if mono existed made a title conclude MONO "
              "and poll 0x3BA 32,000 times"),
    0x3B5: _e("absent-by-design", "MDA CRTC data alias: 0xFF unless mono"),
    0x3BA: _e("absent-by-design", "MDA status alias: 0xFF unless mono"),
    0x3C0: _e("implemented", "VGA attribute controller index (flip-flop)"),
    0x3C1: _e("implemented", "VGA attribute controller data"),
    0x3C2: _e("partial", "input status 0: returns 0x00",
              "bit 4 is the DAC switch-sense a detection routine reads; 0 is "
              "the safe answer, not a measured one"),
    0x3C4: _e("implemented", "VGA sequencer index"),
    0x3C5: _e("implemented", "VGA sequencer data"),
    0x3C6: _e("implemented", "DAC pixel mask"),
    0x3C7: _e("implemented", "DAC read index"),
    0x3C8: _e("implemented", "DAC write index"),
    0x3C9: _e("implemented", "DAC data -- the palette stream"),
    0x3CC: _e("implemented", "misc output register (read)"),
    0x3CE: _e("implemented", "VGA graphics controller index"),
    0x3CF: _e("implemented", "VGA graphics controller data"),
    0x3D4: _e("implemented", "CGA/VGA CRTC index"),
    0x3D5: _e("implemented", "CRTC data, with write-then-read-back"),
    0x3D8: _e("implemented", "CGA mode control"),
    0x3D9: _e("implemented", "CGA colour select"),
    0x3DA: _e("implemented", "CGA/VGA status: a MOVING retrace bit"),
}

PORT_RANGES = [
    (0x220, 0x22F, _e("unmodelled-port", "Sound Blaster base",
                      "SB detect writes 0x226 (reset), reads 0x22A expecting "
                      "0xAA. Floating 0xFF reads as 'no card'")),
    (0x240, 0x24F, _e("unmodelled-port", "Sound Blaster alternate base")),
    (0x278, 0x27F, _e("unmodelled-port", "LPT2")),
    (0x2F8, 0x2FF, _e("unmodelled-port", "COM2 -- a mouse probe lands here")),
    (0x378, 0x37F, _e("unmodelled-port", "LPT1")),
    (0x3F0, 0x3F7, _e("unmodelled-port", "floppy controller",
                      "we bypass the block layer entirely (FatFs HLE), so a "
                      "title driving the FDC directly gets nothing")),
    (0x3F8, 0x3FF, _e("unmodelled-port", "COM1 -- a mouse probe lands here")),
]


def lookup_int(vector, ah, sub):
    """Most specific entry wins; None when nobody has vetted this call."""
    if sub is not None:
        e = INT.get((vector, ah, sub))
        if e:
            return e, "table (vector, AH, sub)"
    e = INT.get((vector, ah))
    if e:
        return e, "table (vector, AH)"
    return None, None


def lookup_port(port):
    e = PORT.get(port)
    if e:
        return e, "table (port)"
    for lo, hi, ent in PORT_RANGES:
        if lo <= port <= hi:
            return ent, f"table (range {lo:#06x}-{hi:#06x})"
    return None, None
