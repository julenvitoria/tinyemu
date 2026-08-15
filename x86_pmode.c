/*
 * PROTECTED MODE: the descriptor walk.
 *
 * ===========================================================================
 * WHAT THIS COSTS REAL MODE: NOTHING. Read this before changing anything.
 * ===========================================================================
 *
 * 29.4% of wall is ordinary opcodes at ~57 cyc/insn (STATUS.md). Segment loads
 * and memory accesses live in that path, so the one thing this file must not
 * do is put a protected-mode test anywhere a real-mode guest will execute it.
 *
 * It does not, and the reason is structural rather than careful:
 *
 *   - Every segment register load in the interpreter -- MOV Sreg,Ev; POP Sreg;
 *     LES/LDS; far JMP/CALL/RETF/IRET; interrupt dispatch -- funnels through
 *     ONE routine, L(SET_SREG) in cpu_exec_handlers.S. It already opened with
 *     `LDR CR0 / TST #1 / BNE` in order to trap out of protected mode loudly.
 *     This work redirects that branch. The real-mode instruction sequence is
 *     BYTE FOR BYTE what it was.
 *
 *   - Nothing here is reached from a memory ACCESS. The descriptor is decoded
 *     once, on load, into the segs[] cache the interpreter already reads for
 *     its base. That is tiny386's shape too (references/tiny386/i386.c:1076,
 *     `set_seg`), and it is what makes protected mode nearly free: R6 carries
 *     a POINTER to the active descriptor, so base, limit and rights are one
 *     load away from any site that ever wants them.
 *
 * THE PART THAT IS DELIBERATELY NOT HERE, and how it stays free when it comes:
 * per-access SEGMENT LIMIT enforcement. A limit test on the access path would
 * be exactly the tax this design exists to avoid. The route is the MODE RECORD
 * (x86_cpu.c, x86_build_mode_records): the record already selects the ModRM
 * decoder by address size, at zero cost, through one EOR on R2. Protected mode
 * is another bit on the same selector -- a fifth and sixth record whose
 * REC_MODRM_FN points at a limit-checking decoder, with real mode dispatching
 * to the decoders it uses today and never executing the check. That doubles
 * the record block and needs a second DECODE_MODRM in ITCM (14 KB free), so it
 * is its own step. x86_pmode_limit_ok() below is that check written out, so
 * the semantics are settled and tested before the plumbing lands.
 *
 * ===========================================================================
 * WHERE THE SEMANTICS COME FROM
 * ===========================================================================
 *
 * TWO references, for two different things, and they do not agree.
 *
 * `references/tiny386/i386.c` is the SHAPE: set_seg() at :1076 is 55 lines
 * with a real-mode fast path on `cr0 & 1`, the granularity shift at :1115, the
 * D/B bit driving code16 and sp_mask at :1122 and :1125, and CPL taken from
 * the CS selector's RPL at :1121. All of that is right and all of it is copied.
 *
 * It is NOT the rules. Its own comment at :1097 reads
 *
 *     // TODO: various permission checks
 *
 * and the single type check it does have, at :1103, is annotated
 * "to avoid win95 BSOD..." -- a workaround, not semantics. There is no
 * DPL/RPL/CPL validation on a data segment load anywhere in that function.
 * Lifting its structure without noticing that yields checks that can never
 * fire, which is worse than no checks: a test suite goes green over them.
 *
 * `references/dosbox-x` is the RULES -- CPU_SetSegGeneral() at
 * src/cpu/cpu.cpp:2976-3076 and the descriptor classes at include/cpu.h:284
 * (S_Descriptor), :389 (Descriptor), :470 (DescriptorTable). It has been
 * beaten against thousands of real DOS programs -- several of its checks carry
 * the name of the title that forced them ("Earth Siege 1" at cpu.cpp:3014,
 * "extreme pinball" at :3053, "gabriel knight" at :3060), which is a better
 * provenance than any of these rules could have from memory. Where the two
 * disagree, DOSBox-X wins and the comment says so. Individual rules are cited
 * at their check below.
 *
 * NOTHING HERE IS SIZED AGAINST A PARTICULAR TITLE. No guest in this tree
 * executes a protected-mode instruction today; this is the foundation every
 * protected-mode path needs (DPMI first among them), and it is deliberately
 * narrow -- the walk and the loads, with every check tested. Far transfers,
 * call gates, IRET across privilege and paging are NOT here and each needs its
 * own step and its own proof.
 *
 * THREE PLACES WE ARE DELIBERATELY STRICTER THAN DOSBox-X, each because
 * DOSBox-X is loose there rather than because it disagrees:
 *   - the descriptor-table bound (see read_desc)
 *   - a null selector's effect on the cached limit (see the null block)
 *   - expand-down (see x86_pmode_limit_ok)
 *
 * The bit LAYOUT is ours and matches neither: see the X86_DESC_* block in
 * x86_cpu.h for why the shift is forced by a flag that already shipped.
 */
#include <stdint.h>
#include <string.h>
#include "cutils.h"
#include "x86_cpu.h"

volatile uint32_t x86_pmode_loads;
volatile uint32_t x86_pmode_faults;

/* ---- NEGATIVE CONTROLS ---------------------------------------------------
 *
 * A check that still passes its test with the check removed is not a check,
 * and this file is nothing but checks. X86_PMODE_NEGCTL=n compiles check n
 * OUT, so tests/test_pmode.py can be shown FAILING on each one in turn --
 * scripts/run_pmode_controls.py drives that, the way
 * scripts/run_planar_controls.py already does for the planar write classes.
 *
 * 0 (the default and the only shipping value) disables nothing.
 *
 *   1  the descriptor-table bound in read_desc
 *   2  null selector into SS
 *   3  S=0 (system descriptor) into a segment register
 *   4  the SS privilege rule (RPL == DPL == CPL)
 *   5  the SS writable-data type rule
 *   6  the DS/ES/FS/GS DPL >= max(CPL,RPL) rule
 *   7  the present bit
 *   8  the limit granularity (G) shift
 *   9  CS must be a code segment
 *  10  LLDT's descriptor type check
 */
#ifndef X86_PMODE_NEGCTL
#define X86_PMODE_NEGCTL 0
#endif
#define NEGCTL(n) (X86_PMODE_NEGCTL == (n))

/* Vectors. X86_EXIT_GPF already names 13; the other two have no exit code
   because they are not exits -- they are recorded in cpu->pmode_excp. */
#define EX_NP 11
#define EX_SS 12
#define EX_GP 13

/* The error code for a SELECTOR fault is the selector with RPL cleared --
 * index and TI preserved. Uniform across both references: tiny386 uses
 * `sel & ~0x3` at i386.c:1065, :1105, :1109 and everywhere else, and DOSBox-X
 * forms the same value. The EXT bit is not set: neither reference sets it for
 * a fault outside interrupt delivery, and we have no delivery path yet.
 */
#define SEL_ERR(sel) ((sel) & 0xFFFCu)

/* A selector is NULL when index and TI are both zero -- i.e. RPL is masked out
 * first, so 0x0003 is null. tiny386 tests exactly this (`sel & ~0x3`,
 * i386.c:1100) and so does DOSBox-X. */
#define SEL_IS_NULL(sel) (((sel) & 0xFFFCu) == 0)

/* ------------------------------------------------------------------------ */

static int fault(X86CPUState *cpu, uint32_t vec, uint32_t err)
{
    cpu->pmode_excp = vec;
    cpu->pmode_err = err;
    x86_pmode_faults++;
    return (int)vec;
}

/*
 * Fetch the 8 descriptor bytes selector `sel` names.
 *
 * Follows tiny386's read_desc (i386.c:1048-1074) exactly on the two points
 * that matter:
 *   - the TI bit selects the CACHED LDT base/limit, not a re-walk of the GDT
 *     (i386.c:1055-1061). An LDT whose own descriptor was never loaded has
 *     limit 0, so every selector through it fails the bound below, which is
 *     the correct behaviour and needs no separate "no LDT" case.
 *   - the bound is `off + 7 > limit` with off = index*8 (i386.c:1063), so a
 *     descriptor STRADDLING the end of the table is refused. A limit of 0
 *     admits nothing; a limit of 7 admits exactly entry 0.
 *
 * TINY386 IS FOLLOWED HERE AND DOSBOX-X IS NOT, which is the one place that
 * happens. DOSBox-X tests `address >= limit` (include/cpu.h:481 and :500,
 * `selector &= ~7` first), so it ACCEPTS a descriptor whose last four bytes
 * lie past the table limit. Intel requires the whole 8 bytes to be within the
 * limit, tiny386 implements that, and the looser test is not a semantic
 * disagreement to defer to -- it is a bound check that admits a descriptor
 * half-read from whatever follows the table.
 *
 * Returns 0 and fills w1/w2, or the vector. #GP(sel & ~3) on a bound failure.
 */
static int read_desc(X86CPUState *cpu, uint32_t sel, uint32_t *w1, uint32_t *w2)
{
    uint32_t off = sel & 0xFFF8u;
    uint32_t base, limit;

    if (sel & 4u) {
        base = cpu->ldt_base;
        limit = cpu->ldt_limit;
    } else {
        base = cpu->gdt_base;
        limit = cpu->gdt_limit;
    }
    if (!NEGCTL(1) && off + 7u > limit)
        return fault(cpu, EX_GP, SEL_ERR(sel));

    /* The table lives in guest memory, so the read goes through the pager the
       same way any host-side guest access does. A descriptor may not straddle
       a granule from the caller's point of view -- x86_guest_copy_out splits
       it -- and a table in unmapped memory yields a short copy, which is a
       #GP rather than a silent zero descriptor. */
    uint8_t raw[8];
    if (x86_guest_copy_out(raw, base + off, 8) != 8)
        return fault(cpu, EX_GP, SEL_ERR(sel));

    *w1 = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) |
          ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
    *w2 = (uint32_t)raw[4] | ((uint32_t)raw[5] << 8) |
          ((uint32_t)raw[6] << 16) | ((uint32_t)raw[7] << 24);
    return 0;
}

/* The flags packing. See the X86_DESC_* block in x86_cpu.h: the D bit's
   position is fixed by an already-shipped instruction, which leaves the access
   byte the high half and the granularity byte the low half. */
static uint32_t desc_flags(uint32_t w2)
{
    return ((w2 >> 16) & 0x00FFu) | (w2 & 0xFF00u);
}

/* Decode w1/w2 into the cache. The granularity shift is tiny386's
 * (i386.c:1114-1116): the limit is stored ALREADY EXPANDED, so nothing
 * downstream re-applies G and a limit comparison is a bare unsigned compare.
 * G multiplies by 4096 and fills the low 12 bits with ones, so a G=1 limit
 * field of 0 means 4095 bytes, not 0. */
static void decode_desc(X86CPUSeg *s, uint32_t sel, uint32_t w1, uint32_t w2)
{
    uint32_t limit = (w1 & 0xFFFFu) | (w2 & 0x000F0000u);
    if (!NEGCTL(8) && (w2 & 0x00800000u))       /* G */
        limit = (limit << 12) | 0xFFFu;
    s->sel = (uint16_t)sel;
    s->base = ((w1 >> 16) & 0xFFFFu) | ((w2 & 0xFFu) << 16) | (w2 & 0xFF000000u);
    s->limit = limit;
    s->flags = (uint16_t)desc_flags(w2);        /* see x86_cpu.h */
}

/*
 * Re-derive the active dispatch table from CS.D.
 *
 * The interpreter picks its mode record ONCE, at entry, from CS.D
 * (cpu_exec.S, `.equ TABLE_BASE_OFFSET`). That was sound while the only way
 * into a 32-bit code segment was to set CS up before the call. It is not
 * sound the moment a far jump can install a code segment with a different D
 * bit mid-run, which is precisely what the PE-entry sequence does.
 *
 * Done HERE and not in the assembly on purpose: this runs only on a
 * protected-mode CS load. Real mode never reaches it, so the far-jump path
 * keeps the instruction count it has today.
 */
void x86_pmode_sync_mode(X86CPUState *cpu)
{
    int d = (cpu->segs[X86_CPU_SEG_CS].flags & X86_DESC_D_FLAG) != 0;
    cpu->opcode_table_base = x86_mode_table(d, d);
}

/*
 * The segment limit test, for one access of `bytes` at segment offset `off`.
 *
 * NOT CALLED FROM THE ACCESS PATH -- see the header comment. It is here, and
 * tested, so that the semantics are settled before the mode-record axis that
 * will inline it.
 *
 * Expand-down is the case that is easy to get backwards, and BOTH references
 * are unsatisfactory on it:
 *
 *   - tiny386 has it as dead code (i386.c:645-648, inside `#if 0`), so it has
 *     never run against anything.
 *   - DOSBox-X's is live but gated behind a global that is OFF BY DEFAULT
 *     (`bool do_seg_limits = false`, src/cpu/cpu.cpp:78), and its expand-down
 *     arm (src/cpu/core_normal/table_ea.h:204-211) tests only the START
 *     address, `eaa <= limit`, ignoring the access SIZE and never consulting
 *     the B bit for the upper bound.
 *
 * The rule is: for an expand-down data segment the limit is the LOWER bound
 * and the valid range is (limit, ceiling], with the ceiling 0xFFFF or
 * 0xFFFFFFFF according to B. So the test is inverted, not offset. Checking the
 * last byte as well as the first is ours, and it is the difference between
 * catching an access that starts inside the segment and runs off the top and
 * not catching it.
 *
 * Returns 1 if the access is within the segment.
 */
int x86_pmode_limit_ok(const X86CPUSeg *s, uint32_t off, uint32_t bytes)
{
    uint32_t f = s->flags;
    uint32_t type = X86_DESC_TYPE(f);
    uint32_t last;

    if (bytes == 0)
        return 1;
    /* A wrap in the offset arithmetic is itself a violation, and computing
       `off + bytes - 1` without checking it is how a limit test passes an
       access that ran off the top of the address space. */
    last = off + bytes - 1u;
    if (last < off)
        return 0;

    if ((f & X86_DESC_S_FLAG) && !(type & X86_TYPE_CODE) &&
        (type & X86_TYPE_DATA_ED)) {
        /* Expand-down: valid is limit+1 .. ceiling, ceiling from B. */
        uint32_t ceiling = (f & X86_DESC_D_FLAG) ? 0xFFFFFFFFu : 0xFFFFu;
        return off > s->limit && last <= ceiling;
    }
    return last <= s->limit;
}

/*
 * THE WALK. Called from L(SET_SREG) with CR0.PE already known to be set.
 *
 * Order of checks below is DOSBox-X's CPU_SetSegGeneral, not tiny386's, for
 * the reason in the header comment. Each check names what it enforces.
 */
int x86_pmode_load_seg(X86CPUState *cpu, uint32_t seg, uint32_t sel)
{
    X86CPUSeg *s;
    uint32_t w1, w2, flags, type, dpl, rpl, cpl;
    int rc;

    sel &= 0xFFFFu;
    if (seg >= 6)
        return fault(cpu, EX_GP, SEL_ERR(sel));
    s = &cpu->segs[seg];
    x86_pmode_loads++;

    rpl = sel & 3u;
    cpl = cpu->cpl & 3u;

    /* ---- null selectors -------------------------------------------------
     *
     * The three cases are genuinely different and collapsing them is a real
     * bug. DOSBox-X splits them and so does the manual:
     *
     *   DS/ES/FS/GS  legal (dosbox-x src/cpu/cpu.cpp:3037-3041). The register
     *                is marked unusable and the fault is deferred to the first
     *                USE. NEITHER reference records the unusability cleanly:
     *                tiny386 infers it later from "index 0 and cached limit 0"
     *                (i386.c:639), which cannot tell a null selector from a
     *                legitimately loaded zero-limit segment, and DOSBox-X
     *                stores the selector and a zero base but LEAVES THE OLD
     *                LIMIT in Segs.limit[]. We clear P, so X86_SEG_USABLE() is
     *                exact and needs no extra field.
     *   SS           #GP(0) -- error code literally zero, not the selector
     *                (dosbox-x src/cpu/cpu.cpp:2993-2998). A stack segment has
     *                no deferred-use case; the very next push would have
     *                nowhere to go.
     *   CS           #GP(0). Not reachable through a plain load in a correct
     *                guest, but a far jump to a null selector must not fall
     *                through into read_desc and index the GDT at zero.
     */
    if (SEL_IS_NULL(sel)) {
        if (!NEGCTL(2) && (seg == X86_CPU_SEG_SS || seg == X86_CPU_SEG_CS))
            return fault(cpu, EX_GP, 0);
        s->sel = (uint16_t)sel;
        s->base = 0;
        s->limit = 0;
        s->flags = 0;                  /* P clear: X86_SEG_USABLE() is false */
        return 0;
    }

    rc = read_desc(cpu, sel, &w1, &w2);
    if (rc)
        return rc;

    flags = desc_flags(w2);
    type = X86_DESC_TYPE(flags);
    dpl = X86_DESC_DPL(flags);

    /* ---- it must be a code/data descriptor ------------------------------
     * S=0 is a system descriptor -- an LDT, a TSS, a gate. None of those may
     * be loaded into a segment register by MOV/POP/LxS. A CS load through a
     * CALL GATE is the one legitimate S=0 case and it is NOT this path: gates
     * come with the far-transfer work, which is deliberately a later step. */
    if (!NEGCTL(3) && !(flags & X86_DESC_S_FLAG))
        return fault(cpu, EX_GP, SEL_ERR(sel));

    if (seg == X86_CPU_SEG_SS) {
        /* ---- SS: the strictest rules on the chip ------------------------
         * RPL, DPL and CPL must ALL be equal, and it must be a WRITABLE DATA
         * segment. tiny386 checks none of this -- its `// TODO: various
         * permission checks` (i386.c:1097) covers exactly this gap. DOSBox-X
         * checks all of it at src/cpu/cpu.cpp:3004-3016, and the type switch
         * there admits only the four writable data types
         * (DESC_DATA_{EU,ED}_RW_{NA,A}) -- no code type and no read-only type.
         * That is what catches a guest putting a code or read-only selector in
         * SS, which otherwise fails much later and much more confusingly.
         *
         * Order follows DOSBox-X: privilege first (:3004), then type (:3009).
         * Both raise #GP with the same error code, so the order is not
         * observable -- it is matched so a future divergence is visible. */
        if (!NEGCTL(4) && (rpl != cpl || dpl != cpl))
            return fault(cpu, EX_GP, SEL_ERR(sel));
        if (!NEGCTL(5) && ((type & X86_TYPE_CODE) || !(type & X86_TYPE_WRITABLE)))
            return fault(cpu, EX_GP, SEL_ERR(sel));
        /* Present is checked LAST and raises #SS rather than #NP for this
           register only (dosbox-x src/cpu/cpu.cpp:3018-3020). tiny386 gets
           the vector split right even though it skips everything above
           (i386.c:1109). */
        if (!NEGCTL(7) && !(flags & X86_DESC_P_FLAG))
            return fault(cpu, EX_SS, SEL_ERR(sel));
    } else if (seg == X86_CPU_SEG_CS) {
        /* ---- CS: must be a code segment ---------------------------------
         * The full privilege rules for a far transfer -- conforming vs not,
         * RPL forced to CPL, gates, stack switches -- belong to the transfer
         * path (tiny386 puts them in pmcall, i386.c:4295, NOT in set_seg) and
         * are a later step. What must be here is the check that stops a DATA
         * selector becoming CS, because that one silently executes whatever
         * the data segment happens to contain. */
        if (!NEGCTL(9) && !(type & X86_TYPE_CODE))
            return fault(cpu, EX_GP, SEL_ERR(sel));
        if (!NEGCTL(7) && !(flags & X86_DESC_P_FLAG))
            return fault(cpu, EX_NP, SEL_ERR(sel));
    } else {
        /* ---- DS/ES/FS/GS ------------------------------------------------
         * Must be READABLE: any data segment is, but a code segment is only
         * readable when its type carries the readable bit. An execute-only
         * code selector in DS is a #GP.
         *
         * DOSBox-X's allowed type list is src/cpu/cpu.cpp:3046-3063: the eight
         * data types, plus readable code (DESC_CODE_R_*). Execute-only code
         * falls to the `default:` at :3059 and is #GP.
         *
         * THE PRIVILEGE RULE, and it is the one tiny386 does not have at all:
         * for a data segment or a NON-CONFORMING code segment, DPL must be
         * numerically >= max(CPL, RPL). DOSBox-X writes it as two comparisons,
         * `((value & 3) > desc.DPL()) || (cpu.cpl > desc.DPL())`
         * (src/cpu/cpu.cpp:3052), which is the same predicate.
         *
         * A readable CONFORMING code segment is exempt and DOSBox-X gives it
         * its own case that breaks without testing anything (:3057-3058).
         * The exemption is what conforming means; omitting it would reject
         * legitimate guests rather than merely admit bad ones. */
        if ((type & X86_TYPE_CODE) && !(type & X86_TYPE_READABLE))
            return fault(cpu, EX_GP, SEL_ERR(sel));
        if (!((type & X86_TYPE_CODE) && (type & X86_TYPE_CODE_CONF))) {
            uint32_t need = rpl > cpl ? rpl : cpl;
            if (!NEGCTL(6) && dpl < need)
                return fault(cpu, EX_GP, SEL_ERR(sel));
        }
        if (!NEGCTL(7) && !(flags & X86_DESC_P_FLAG))
            return fault(cpu, EX_NP, SEL_ERR(sel));
    }

    decode_desc(s, sel, w1, w2);

    if (seg == X86_CPU_SEG_CS) {
        /* CPL IS CS.RPL. Not the descriptor's DPL -- tiny386 has this right
           (i386.c:1121) and it matters: the DPL/RPL rules above are all
           relative to it, so getting it from the wrong field makes every one
           of them wrong in the same direction. */
        cpu->cpl = rpl;
        /* And the D bit changes the DEFAULT operand and address size, which is
           the mode record. See x86_pmode_sync_mode. */
        x86_pmode_sync_mode(cpu);
    }
    return 0;
}

/*
 * LLDT. tiny386 routes this through set_seg into a seventh segs[] slot
 * (i386.c:3258) and consequently applies no type check at all. We keep the LDT
 * out of segs[] (see x86_cpu.h) and check the type here, because an LDT
 * selector naming a code segment would otherwise install a descriptor table
 * pointing at executable memory.
 *
 * The checks are DOSBox-X's GDTDescriptorTable::LLDT (include/cpu.h:528-543):
 * null clears the LDT (:529-532), a non-DESC_LDT type is #GP (:535), and
 * non-present is #NP (:536). Its error code there is the RAW selector, not
 * `sel & ~3` -- inconsistent with its own CPU_SetSegGeneral, which masks. We
 * mask, matching that function and tiny386's uniform `sel & ~0x3`.
 *
 * Both references also require CPL 0 for LLDT itself; that check belongs at
 * the instruction, not here, and lives in cpu_exec_handlers_0f.S with the
 * other privileged-instruction gates.
 *
 * A null LDT selector is legal and means "no LDT": limit 0, so every
 * subsequent LDT-relative selector fails read_desc's bound. That is the
 * architectural behaviour and it falls out rather than needing a case.
 */
int x86_pmode_lldt(X86CPUState *cpu, uint32_t sel)
{
    uint32_t w1, w2, flags;
    int rc;

    sel &= 0xFFFFu;
    if (SEL_IS_NULL(sel)) {
        cpu->ldt_sel = sel;
        cpu->ldt_base = 0;
        cpu->ldt_limit = 0;
        return 0;
    }
    /* An LDT descriptor always lives in the GDT; TI=1 here is a #GP. */
    if (sel & 4u)
        return fault(cpu, EX_GP, SEL_ERR(sel));

    rc = read_desc(cpu, sel, &w1, &w2);
    if (rc)
        return rc;
    flags = desc_flags(w2);
    /* System descriptor (S=0) of type 2 = LDT. */
    if (!NEGCTL(10) && ((flags & X86_DESC_S_FLAG) || X86_DESC_TYPE(flags) != 2u))
        return fault(cpu, EX_GP, SEL_ERR(sel));
    if (!(flags & X86_DESC_P_FLAG))
        return fault(cpu, EX_NP, SEL_ERR(sel));

    X86CPUSeg tmp;
    decode_desc(&tmp, sel, w1, w2);
    cpu->ldt_sel = sel;
    cpu->ldt_base = tmp.base;
    cpu->ldt_limit = tmp.limit;
    return 0;
}
