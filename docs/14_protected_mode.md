# Protected mode

State: [`STATUS.md`](../STATUS.md). Roadmap: [`PLAN.md`](../PLAN.md) Queue C 4.
Code: `x86_pmode.c`, `L(SET_SREG)` in `cpu_exec_handlers.S`, group 6/7 in
`cpu_exec_handlers_0f.S`. Tests: `tests/test_pmode.py`,
`scripts/run_pmode_controls.py`.

## 1. What is built, and what is not

**Built and graded.** `LGDT`, `LIDT`, `LLDT`, `SLDT`; the GDT/LDT descriptor
fetch and its bound; the S/type/DPL/present rules with the correct vector and
error code for each; the per-segment descriptor cache (base, limit with the G
shift applied, rights); `MOV Sreg,Ev`, `POP Sreg`, `LES`/`LDS`; `CR0.PE` in
both directions; and a same-privilege far `JMP` — the transfer that installs CS
and flushes the pipeline.

**Not built, and not stubbed.** Call gates, `IRET` across privilege, stack
switches, task switches, paging, and per-access **segment limit enforcement**.
Each is its own step. V86 and the TSS are not needed at all: INT 21h is HLE'd.

**Nothing here is sized against a title.** No guest in this tree executes a
protected-mode instruction today. The ~1,225-line estimate that used to sit in
`PLAN.md` had no measured demand behind it, and Dark Forces was subsequently
measured never reaching protected mode at all — it dies at `INT 21h AH=4Bh`.
This is the foundation every protected-mode path needs; build the rest when
something asks.

## 2. Why real mode pays nothing

29.4% of wall is ordinary opcodes at ~57 cyc/insn. Segment loads and memory
accesses are in that path, so a protected-mode check placed carelessly taxes
every real-mode title for a feature none of them use.

It is avoided **structurally, not carefully**:

- Every segment register load in the machine — `MOV Sreg,Ev`, `POP Sreg`,
  `LES`/`LDS`, far `JMP`/`CALL`/`RETF`/`IRET`, interrupt dispatch — funnels
  through **one** routine, `L(SET_SREG)`. It already opened with
  `LDR CR0 / TST #1 / BNE` in order to trap protected mode out loudly. Only the
  branch target changed.
- Nothing in `x86_pmode.c` is reached from a memory **access**. The descriptor
  is decoded once, on load, into the `segs[]` cache the interpreter already
  reads for its base. That is `references/tiny386`'s shape too (i386.c:1076),
  and it is what the `R6` contract in `CLAUDE.md` is for: R6 carries a POINTER
  to the active descriptor, so limit and rights are one load from any site that
  eventually wants them.

**Proven by disassembly diff against master**, per function, mnemonics
normalised for address shifts:

| path | change |
|---|---|
| `.fetch_decode_loop` | byte-identical |
| every `.op_*` handler, `decode_modrm_addr16/32`, the ALU/SHIFT/string tables | byte-identical |
| `L(SET_SREG)` real-mode arm | one `STRH` → `STR` — same instruction, same cost |
| `x86_cpu_interp_asm` | +2 instructions, **per interp CALL** |
| `.save_state_and_exit` | +1 instruction, **per EXIT** |

Cost: **+912 B ITCM** (all cold), **+704 B RAM_EMU**.

**`tests/test_benchmark.py` cannot decide this and must not be quoted for it.**
Under gwemu its cycle count measured 10,583,983 / 10,302,184 / 11,884,810 for
1,000,000 instructions **on the unmodified baseline** — a 15% spread that
swallows anything this work could have cost. gwemu does not model the pipeline;
a real number is tier 3 only. Since nothing was added to any per-instruction
path, there is nothing per-instruction to measure.

## 3. Two references, for two different things

They do not agree, and taking the wrong one for the wrong job is the trap.

**`references/tiny386/i386.c` is the SHAPE.** `set_seg()` at :1076 is 55 lines:
a real-mode fast path on `cr0 & 1`, the granularity shift at :1115, D/B driving
`code16` and `sp_mask` at :1122/:1125, CPL from the CS selector's RPL at :1121.
All correct, all copied.

**It is NOT the rules.** Its own comment at :1097 reads
`// TODO: various permission checks`, and the single type check it has (:1103)
is annotated `to avoid win95 BSOD...` — a workaround. **There is no DPL/RPL/CPL
validation on a data segment load anywhere in that function.** An
implementation modelled on it inherits that hole, and so does any test suite
written alongside it: every privilege test passes because neither side
implements the rule.

**`references/dosbox-x` is the RULES** — `CPU_SetSegGeneral()` at
`src/cpu/cpu.cpp:2976-3076`, descriptor classes at `include/cpu.h:284/389/470`.
Several of its checks carry the name of the title that forced them
("Earth Siege 1" :3014, "extreme pinball" :3053, "gabriel knight" :3060).
Where the two disagree, DOSBox-X wins, and the comment at the check says so.

### Three places we are deliberately STRICTER than DOSBox-X

Because it is loose there, not because it disagrees:

1. **The descriptor-table bound.** DOSBox-X tests `address >= limit`
   (`include/cpu.h:481`, `:500`), so it accepts a descriptor whose last four
   bytes lie past the table limit. Intel requires all 8 bytes inside; tiny386
   implements that (`off + 7 > limit`, i386.c:1063) and so do we.
2. **A null selector's effect on the cache.** tiny386 infers "null" later from
   `index == 0 && limit == 0` (i386.c:639), which cannot distinguish a null
   selector from a legitimately loaded zero-limit segment; DOSBox-X stores the
   selector but **leaves the previous limit** in `Segs.limit[]`. We clear P, so
   `X86_SEG_USABLE()` is exact and costs no extra field.
3. **Expand-down.** tiny386's is dead code (`#if 0`, i386.c:645). DOSBox-X's is
   live but behind `do_seg_limits`, **off by default**
   (`src/cpu/cpu.cpp:78`), and its arm (`core_normal/table_ea.h:204`) tests only
   the START address and ignores the B bit. Ours checks the last byte too and
   takes the ceiling from B.

## 4. The `flags` packing, and the bug it already caused

`X86CPUSeg.flags` is 16 bits and must carry both the descriptor's granularity
byte (byte 6) and its access byte (byte 5):

```
flags == desc[6] | (desc[5] << 8)
  bits 0-3 limit 19:16   bit 6 D/B   bit 7 G
  bits 8-11 TYPE   bit 12 S   bits 13-14 DPL   bit 15 P
```

**The D bit's position is not a choice.** `X86_DESC_D_FLAG` already shipped as
`1 << 6` and the interpreter reads it as bit 22 of the 32-bit word
`sel | (flags << 16)` at `SEG_CS_SEL`. Descriptor byte 6 carries D at its own
bit 6, so byte 6 must be the low half — and the access byte then has nowhere to
go but the high half.

The obvious-looking `flags = (w2 >> 16) & 0xFFFF` keeps D in the right place and
**silently drops P, S, DPL and TYPE entirely**. It was written here first and
every descriptor came back not-present. Neither reference uses this packing and
neither can (tiny386 uses `w2 >> 8`, DOSBox-X a bitfield struct), so when a line
from either is cited for a rule, the rule is what is cited — never a bit index.

## 5. Faults, and why they EXIT rather than dispatch

There is no protected-mode IDT gate walk yet, so a segmentation fault cannot be
delivered. It is recorded — vector in `cpu->pmode_excp`, error code in
`cpu->pmode_err` — and the interpreter exits with `X86_EXIT_GPF` carrying the
error code in `exit_info`. That makes an undelivered fault loud and readable
rather than lost, and delivery becomes a later step rather than a silent gap.

Error codes are the selector with RPL cleared (`sel & 0xFFFC`), uniform across
both references, except the null-SS and privileged-instruction cases which are
a literal 0 (`src/cpu/cpu.cpp:2997`).

### The stack-pointer bug this uncovered

An exit taken from inside a handler's subroutine call leaves that handler's own
pushes on the stack: every caller of `L(SET_SREG)` does
`PUSH {LR} / BL / POP {LR}`, and a fault leaves without reaching the `POP`. The
final `POP {R4-R11, PC}` then reads one word too high and branches to whatever
the handler saved — a wild jump that presents as *the interpreter never
returning*, which is exactly how it was found.

**This was already latent**: the old `TST CR0,#1 / BNE .exit_loop` at the top of
`L(SET_SREG)` had the identical imbalance and simply never fired, because no
guest in the tree ever set PE. The fix is general rather than local — the
prologue records SP in `cpu->host_sp` and `.save_state_and_exit` restores it, so
every exit path is stack-safe however deep the handler was. One store per
interp call, one load per exit.

## 6. The route for per-access LIMIT enforcement

`x86_pmode_limit_ok()` is written and tested (expand-up, expand-down, the
offset wrap, the B-bit ceiling) but **is not on the access path**. Putting it
there naively is precisely the tax §2 exists to avoid.

The route is the **mode record** (`x86_build_mode_records`, `x86_cpu.c`). The
record already selects the ModRM decoder by address size at zero cost — one
`EOR` on `R2` for `66`/`67`. Protected mode is another bit on the same
selector: a fifth and sixth record whose `REC_MODRM_FN` points at a
limit-checking decoder, with real mode dispatching to the decoders it uses today
and never executing the check.

Cost of that step, which is why it is a step and not part of this one: the
record block doubles (8 KB → 16 KB of DTCM, against 63,268 B free) and a second
`DECODE_MODRM` must fit in ITCM (~12.7 KB free). Both are affordable; neither is
free enough to do without measuring.

## 7. Negative controls

`scripts/run_pmode_controls.py`. Ten checks, each compiled out individually by
`X86_PMODE_NEGCTL=n`, each required to make `tests/test_pmode.py` FAIL. It runs
a baseline arm first — with everything on the suite must PASS, or every arm
below "fails" for an unrelated reason.

**Three arms survived on the first run**, which is the argument for the whole
exercise:

- **Table bound.** The test used a selector past the limit with *nothing* at
  that address, so removing the bound merely moved the fault to the S-bit check
  and the arm still passed. Fixed by placing a well-formed present descriptor
  one entry past the limit.
- **SS privilege.** Every SS arm used a selector that also failed the *type*
  rule, so removing the privilege rule changed nothing. Fixed by loading SS with
  a writable, present data selector whose only defect is its RPL.
- **CS type.** No arm loaded CS in protected mode at all. Fixed by adding the
  far `JMP` arms — and the data selector it jumps to deliberately shares the
  code selector's base, so a survivor would find the same `HLT` and could not be
  mistaken for a pass.

A fourth, granularity, was caught only because the test uses a limit **field of
zero** with G set: with a non-zero field an ignored G still yields a distinct
number and the check looks live.
