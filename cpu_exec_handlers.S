
#ifndef MACROS_DEFINED
#define MACROS_DEFINED

/* ------------------------------------------------------------------------
 * x86 arithmetic flags.
 *
 * R11 holds a materialized x86 EFLAGS word: CF=0 PF=2 AF=4 ZF=6 SF=7 OF=11.
 * Every consumer already reads that format (EVAL_COND, SETcc, PUSHF/POPF,
 * SAHF/LAHF, the string ops' DF check), so keeping it means the flag layer
 * lands without touching any of them.
 *
 * Flags come out of the host PSR: after an S-form ARM op, N/Z/C/V are exactly
 * the x86 SF/ZF/CF/OF provided the operands were normalized to the TOP of the
 * word (LSL #NSH: 24 for byte ops, 0 for dword). That is the same trick the
 * ALU-core collapse will use to serve 8/16/32 from one core.
 *
 * Note on cost: PF and AF are computed eagerly here. That is deliberate for a
 * first correct implementation -- it keeps every consumer unchanged and is
 * verifiable by the FLAGS cases in tests/test_opcodes.py. It is also the
 * expensive choice (~9 of the ~16 instructions below), which is why the plan
 * converts PF/AF to lazy evaluation during the ALU-core collapse. Doing that
 * conversion is a change to these macros, not to their ~30 call sites.
 * ------------------------------------------------------------------------ */

/* PF = even parity of the low 8 bits of the result. Bits above 7 fold out of
   bit 0 on their own, so no masking is needed. */
.macro X86_PF res, t
    EOR  \t, \res, \res, LSR #4
    EOR  \t, \t, \t, LSR #2
    EOR  \t, \t, \t, LSR #1
    MVN  \t, \t
    BFI  R11, \t, #2, #1
.endm

/* SF/ZF from N/Z. LSR #30 puts Z at bit0 and N at bit1, which BFI drops
   straight into EFLAGS[7:6]. */
.macro X86_SF_ZF t0, t1
    LSR  \t1, \t0, #30
    BFI  R11, \t1, #6, #2
.endm

/* AF = carry out of bit 3 = (a ^ b ^ result) bit 4. \afaux must already hold
   that XOR; the caller computes a^b before the op and XORs the result after. */
.macro X86_AF afaux, t, sh=0
    /* The aux word is a^b^result. AF is the carry out of bit 3, i.e. bit 4 of
       the OPERAND -- which sits at bit 4+sh once operands are normalized to the
       top of the word. Reading bit 4 unconditionally made AF wrong for every
       8-bit op. */
    LSR  \t, \afaux, #(4 + \sh)
    BFI  R11, \t, #4, #1
.endm

/* ADD/SUB/CMP/ADC/SBB/NEG. cf_inv=1 for subtract-like ops, because ARM's C is
   "no borrow" where x86's CF is "borrow". */
.macro X86_FLAGS_ARITH res, afaux, t0, t1, cf_inv, sh=0
    MRS  \t0, APSR
    X86_SF_ZF \t0, \t1
    LSR  \t1, \t0, #29
.if \cf_inv
    EOR  \t1, \t1, #1
.endif
    BFI  R11, \t1, #0, #1          @ CF
    LSR  \t1, \t0, #28
    BFI  R11, \t1, #11, #1         @ OF
    X86_AF \afaux, \t1, \sh
    X86_PF \res, \t1
.endm

/* Narrow ADC/SBB.
 *
 * The normalize-to-the-top trick cannot carry: ADCS injects the host carry at
 * bit 0, but a normalized operand's LSB is at bit `sh`, so the carry-in landed
 * in dead bits and was discarded by the shift back down. 8-bit ADC/SBB
 * therefore ignored CF entirely, and nothing tested it.
 *
 * Compute the sum at full width instead -- a and b are below 2^W, so a+b+c
 * cannot truncate -- then normalize the RESULT with one LSLS, which lands
 * N=SF, Z=ZF and C=carry-out-of-bit-W together. For subtract, C is the borrow
 * directly (a negative s leaves bits W..31 set), so unlike the SUBS path this
 * needs no CF inversion. Only OF is built by hand.
 *
 * a: R3 (becomes result), b: R5 (dead after), scratch R0/R1/R4.
 * R2 is the destination pointer and must stay live.
 */
    .macro X86_ADC_NARROW sh, sub
    UBFX R0, R11, #0, #1           @ CF in
    EOR.W R4, R3, R5               @ a ^ b
.if \sub
    SUB.W R1, R3, R5
    SUB.W R1, R1, R0               @ s = a - b - c
    EOR.W R0, R3, R1               @ a ^ s
    AND.W R0, R0, R4               @ OF(sub) = (a^b) & (a^s)
.else
    ADD.W R1, R3, R5
    ADD.W R1, R1, R0               @ s = a + b + c
    EOR.W R0, R3, R1               @ a ^ s
    EOR.W R3, R5, R1               @ b ^ s
    AND.W R0, R0, R3               @ OF(add) = (a^s) & (b^s)
.endif
    EOR.W R4, R4, R1               @ a ^ b ^ s; AF at bit 4, operands unshifted
    LSLS R3, R1, #\sh              @ N=SF, Z=ZF, C=carry/borrow out of bit W
    MRS  R5, APSR
    X86_SF_ZF R5, R1
    LSR  R1, R5, #29
    BFI  R11, R1, #0, #1           @ CF
    LSR  R0, R0, #(31 - \sh)
    BFI  R11, R0, #11, #1          @ OF
    X86_AF R4, R1, 0
    LSR  R3, R3, #\sh              @ result back down
    X86_PF R3, R1
    .endm

/* AND/OR/XOR/TEST: x86 defines CF=0 and OF=0; AF is undefined and left alone. */
.macro X86_FLAGS_LOGIC res, t0, t1
    MRS  \t0, APSR
    X86_SF_ZF \t0, \t1
    BIC  R11, R11, #1              @ CF = 0
    BIC  R11, R11, #(1 << 11)      @ OF = 0
    X86_PF \res, \t1
.endm

/* Shifts. ARM's S-form shifts set C to the last bit shifted out, which is
   exactly x86's CF. OF is architecturally defined only for a count of 1 and is
   NOT implemented here -- see the shift entry in STATUS.md. */
.macro X86_FLAGS_SHIFT res, t0, t1
    MRS  \t0, APSR
    X86_SF_ZF \t0, \t1
    LSR  \t1, \t0, #29
    BFI  R11, \t1, #0, #1          @ CF = last bit shifted out
    X86_PF \res, \t1
.endm

/* INC/DEC are ADD/SUB that must leave CF untouched. */
.macro X86_FLAGS_INCDEC res, afaux, t0, t1, sh=0
    MRS  \t0, APSR
    X86_SF_ZF \t0, \t1
    LSR  \t1, \t0, #28
    BFI  R11, \t1, #11, #1         @ OF
    X86_AF \afaux, \t1, \sh
    X86_PF \res, \t1
.endm

.macro ALU_TABLE_32 name dispatch target_suffix
\name:
    .short (ALU\target_suffix\()_ADD_32 - (\dispatch + 4)) / 2
    .short (ALU\target_suffix\()_OR_32 - (\dispatch + 4)) / 2
    .short (ALU\target_suffix\()_ADC_32 - (\dispatch + 4)) / 2
    .short (ALU\target_suffix\()_SBB_32 - (\dispatch + 4)) / 2
    .short (ALU\target_suffix\()_AND_32 - (\dispatch + 4)) / 2
    .short (ALU\target_suffix\()_SUB_32 - (\dispatch + 4)) / 2
    .short (ALU\target_suffix\()_XOR_32 - (\dispatch + 4)) / 2
    .short (ALU\target_suffix\()_CMP_32 - (\dispatch + 4)) / 2
.endm

.macro ALU_TABLE_16 name dispatch target_suffix
\name:
    .short (ALU\target_suffix\()_ADD_16 - (\dispatch + 4)) / 2
    .short (ALU\target_suffix\()_OR_16 - (\dispatch + 4)) / 2
    .short (ALU\target_suffix\()_ADC_16 - (\dispatch + 4)) / 2
    .short (ALU\target_suffix\()_SBB_16 - (\dispatch + 4)) / 2
    .short (ALU\target_suffix\()_AND_16 - (\dispatch + 4)) / 2
    .short (ALU\target_suffix\()_SUB_16 - (\dispatch + 4)) / 2
    .short (ALU\target_suffix\()_XOR_16 - (\dispatch + 4)) / 2
    .short (ALU\target_suffix\()_CMP_16 - (\dispatch + 4)) / 2
.endm

.macro SHIFT_TABLE_32 name dispatch target_suffix
\name:
    .short (SHIFT\target_suffix\()_ROL_32 - (\dispatch + 4)) / 2
    .short (SHIFT\target_suffix\()_ROR_32 - (\dispatch + 4)) / 2
    .short (SHIFT\target_suffix\()_RCL_32 - (\dispatch + 4)) / 2
    .short (SHIFT\target_suffix\()_RCR_32 - (\dispatch + 4)) / 2
    .short (SHIFT\target_suffix\()_SHL_32 - (\dispatch + 4)) / 2
    .short (SHIFT\target_suffix\()_SHR_32 - (\dispatch + 4)) / 2
    .short (SHIFT\target_suffix\()_SHL_32 - (\dispatch + 4)) / 2
    .short (SHIFT\target_suffix\()_SAR_32 - (\dispatch + 4)) / 2
.endm

.macro SHIFT_TABLE_16 name dispatch target_suffix
\name:
    .short (SHIFT\target_suffix\()_ROL_16 - (\dispatch + 4)) / 2
    .short (SHIFT\target_suffix\()_ROR_16 - (\dispatch + 4)) / 2
    .short (SHIFT\target_suffix\()_RCL_16 - (\dispatch + 4)) / 2
    .short (SHIFT\target_suffix\()_RCR_16 - (\dispatch + 4)) / 2
    .short (SHIFT\target_suffix\()_SHL_16 - (\dispatch + 4)) / 2
    .short (SHIFT\target_suffix\()_SHR_16 - (\dispatch + 4)) / 2
    .short (SHIFT\target_suffix\()_SHL_16 - (\dispatch + 4)) / 2
    .short (SHIFT\target_suffix\()_SAR_16 - (\dispatch + 4)) / 2
.endm

/* String-operation primitives.
 *
 * The source is DS:(E)SI and may be overridden; the destination is ES:(E)DI
 * and may NOT be. Index registers are used at the ADDRESS size, which is a
 * separate axis from the operand size -- R2 still points at the mode record
 * here (string ops take no ModRM), so bit ADDRSIZE_XOR of it is the live
 * address size. The old handlers used raw ESI/EDI as linear addresses with no
 * base and no wrap, which is only correct in a flat 32-bit model.
 */
    .macro STR_SRC out, tmp
        LDR.W \out, [R8, #REG_ESI]
        TST.W R2, #ADDRSIZE_XOR
        IT EQ
        UXTHEQ \out, \out
        SUBS \tmp, R6, R8             @ segment override in play?
        IT EQ
        ADDEQ R6, R8, #SEG_DS_SEL
        LDR.W \tmp, [R6, #4]
        ADD.W \out, \out, \tmp
    .endm

    .macro STR_DST out, tmp
        LDR.W \out, [R8, #REG_EDI]
        TST.W R2, #ADDRSIZE_XOR
        IT EQ
        UXTHEQ \out, \out
        LDR.W \tmp, [R8, #SEG_ES_BASE]
        ADD.W \out, \out, \tmp
    .endm

    .macro STR_ADV off, delta, tmp
        LDR.W \tmp, [R8, #\off]
        ADD.W \tmp, \tmp, \delta
        TST.W R2, #ADDRSIZE_XOR
        IT EQ
        UXTHEQ \tmp, \tmp            @ SI/DI wrap at the address size
        STR.W \tmp, [R8, #\off]
    .endm

    /* A `moffs` is an OFFSET in DS (or an override), taken at the ADDRESS
       size -- not at the operand size, and not a linear address. A0-A3 read a
       fixed 32 bits and used the result raw: at 16-bit address size that
       mis-decoded the instruction length AND indexed the fold table far past
       its end, so the handler dereferenced whatever lay beyond it. */
    .macro MOFFS_ADDR out, tmp
        UBFX \tmp, R10, #12, #12
        LDR.W \tmp, [R9, \tmp, LSL #2]
        ADDS \tmp, \tmp, R10
        MEM_TRAP_R \tmp                @ instruction stream: always a read
        TST.W R2, #ADDRSIZE_XOR
        BNE 9f
        LDRH \out, [\tmp]             @ 16-bit addressing: moffs16
        ADD.W R10, R10, #2
        B.N 8f
9:      LDR \out, [\tmp]              @ 32-bit addressing: moffs32
        ADD.W R10, R10, #4
8:      SUBS \tmp, R6, R8             @ segment override in play?
        IT EQ
        ADDEQ R6, R8, #SEG_DS_SEL
        LDR.W \tmp, [R6, #4]
        ADD.W \out, \out, \tmp
    .endm

    /* LOOP/JCXZ count in CX or ECX by ADDRESS size, exactly like the string
       ops' index registers -- R2 is the live mode record here too. They used
       the full 32-bit ECX unconditionally, which is correct only while the
       high half happens to be zero. */
    .macro CNT_LOAD out
        LDR.W \out, [R8, #REG_ECX]
        TST.W R2, #ADDRSIZE_XOR
        IT EQ
        UXTHEQ \out, \out
    .endm
    .macro CNT_STORE val
        TST.W R2, #ADDRSIZE_XOR
        ITE EQ
        STRHEQ.W \val, [R8, #REG_ECX]
        STRNE.W \val, [R8, #REG_ECX]
    .endm

    /* +w or -w according to DF, in two instructions plus the scale. */
    .macro STR_DELTA out, w
        SBFX \out, R11, #10, #1
        ORR.W \out, \out, #1
    .if \w == 2
        LSL.W \out, \out, #1
    .endif
    .if \w == 4
        LSL.W \out, \out, #2
    .endif
    .endm

/* Stack primitives. Both widths, both passes.
   PUSH_V: val is stored; tsp/tptr are scratch and are clobbered.
   The linear address is SS.base + SP, with SP wrapped to the stack width
   BEFORE the base is added -- a 16-bit stack cannot walk out of its segment. */
    .macro PUSH_V val, tsp, tptr
        LDR.W \tsp, [R8, #CPU_ESP]
        SP_DEC \tsp
        SP_WRAP \tsp
        STR.W \tsp, [R8, #CPU_ESP]
        LDR.W \tptr, [R8, #SEG_SS_BASE]
        ADD.W \tptr, \tptr, \tsp
        UBFX \tsp, \tptr, #12, #12
        LDR.W \tsp, [R9, \tsp, LSL #2]
        ADDS \tsp, \tsp, \tptr
        MEM_TRAP_W \tsp
        ST_V \val, \tsp
    .endm

    .macro POP_V dest, tsp, tptr
        LDR.W \tsp, [R8, #CPU_ESP]
        LDR.W \tptr, [R8, #SEG_SS_BASE]
        ADD.W \tptr, \tptr, \tsp
        UBFX \dest, \tptr, #12, #12
        LDR.W \dest, [R9, \dest, LSL #2]
        ADDS \dest, \dest, \tptr
        MEM_TRAP_R \dest
        LD_V \dest, \dest
        SP_INC \tsp
        SP_WRAP \tsp
        STR.W \tsp, [R8, #CPU_ESP]
    .endm

#endif

/* Width normalization. These MUST live outside the MACROS_DEFINED guard: that
   guard is processed once, so a definition inside it would freeze the first
   pass's (32-bit, empty) form and silently make every 16-bit ALU flag wrong. */
#undef VNORM
#undef VDENORM
#if NSH
#define VNORM(r)   LSL.W r, r, #NSH
#define VDENORM(r) LSR.W r, r, #NSH
#define VSEXT(r)   SXTH r, r
#define VMASK(r)   UXTH r, r
#define VTEST(d,s) LSLS d, s, #NSH
#define VBITS      16
#define VMSB       15
#else
#define VNORM(r)
#define VDENORM(r)
#define VSEXT(r)
#define VMASK(r)
#define VTEST(d,s) MOVS d, s
#define VBITS      32
#define VMSB       31
#endif

#define ALU_TABLE_M CONCAT(ALU_TABLE, OP_SUFFIX)
#define SHIFT_TABLE_M CONCAT(SHIFT_TABLE, OP_SUFFIX)

OP_AGNOSTIC .op_nop
    B.W .fetch_decode_loop

/* AF is bit 4 of (a ^ b ^ result). With b == 1 that aux word is a ^ 1, NOT
   ~a -- MVN inverts every bit including the one the macro reads, so AF came
   out exactly backwards on every INC and DEC of a register. The ModRM forms
   already use the right aux. */
/* THE ARITHMETIC IS NORMALIZED, exactly as L(GRP5_INC)/L(GRP5_DEC) below are.
   Without VNORM the operand sits in the LOW half of the register at 16-bit
   operand size and SF, ZF and OF then come off bit 31 of a 32-bit add -- so
   `dec ax` with AX=0xFFFF reported SF=0 (it is 0xFFFE, SF=1) and `inc ax` with
   AX=0xFFFF reported ZF=0. These are opcodes 0x40-0x4F, the single most common
   16-bit instructions there are; the defect is invisible to a test that only
   ever decrements a small number, which is why 273 hand-written cases and the
   `DEC r16 AF` case right next to it all passed over it. MEASURED: it is what
   corrupted Wolfenstein 3-D's `finetangent[]` table -- built through Borland's
   software 80x87 emulator, whose normalisation loops branch on exactly these
   flags -- and with it the whole raycast scene. See tests/test_alu_fuzz.py. */
OP .op_inc_r32
    AND.W R2, R0, #7
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    LDREG_V R3, R2
    VNORM(R3)
    EOR.W R4, R3, #(1 << NSH)      @ AF aux: a ^ b, with b == 1
    ADDS R3, R3, #(1 << NSH)
    EOR.W R4, R4, R3
    VDENORM(R3)
    X86_FLAGS_INCDEC R3, R4, R0, R1, NSH   @ INC must not disturb CF
    B.N L(WRITE_REG)

OP .op_dec_r32
    AND.W R2, R0, #7
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    LDREG_V R3, R2
    VNORM(R3)
    EOR.W R4, R3, #(1 << NSH)
    SUBS R3, R3, #(1 << NSH)
    EOR.W R4, R4, R3
    VDENORM(R3)
    X86_FLAGS_INCDEC R3, R4, R0, R1, NSH   @ DEC must not disturb CF
L(WRITE_REG):
    STREG_V R3, R2
    B.W .fetch_decode_loop

OP .op_push_r32
    AND.W R2, R0, #7
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    LDREG_V R3, R2
    PUSH_V R3, R4, R12
    B.W .fetch_decode_loop

OP .op_pop_r32
    AND.W R2, R0, #7
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    POP_V R3, R4, R12
    STREG_V R3, R2
    B.W .fetch_decode_loop

OP .op_mov_r32_imm32
    AND.W R2, R0, #7
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    CODE_PTR R3
    LD_V R3, R3
    EIP_ADV_V
    STREG_V R3, R2
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_alu_eb_gb
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(ALU_EB_GB_MEM)
    UBFX R4, R1, #3, #3
    AND.W R5, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R5, LSL #2
    ADD.W R4, R4, #8
    LDRB.W R5, [R8, R4]
    AND.W R2, R1, #7
    AND.W R3, R2, #3
    ADD.W R3, R8, R3, LSL #2
    ADD.W R2, R3, R2, LSR #2
    ADD.W R2, R2, #8
    LDRB R3, [R2, #0]
    B.N L(ALU_DISPATCH_8_SUBOP)
L(ALU_EB_GB_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    UBFX R4, R1, #3, #3
    AND.W R5, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R5, LSL #2
    ADD.W R4, R4, #8
    LDRB.W R5, [R8, R4]
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LDRB R3, [R2, #0]
L(ALU_DISPATCH_8_SUBOP):
    UBFX R12, R0, #3, #3
L(ALU_DISPATCH_8):
    TBH [PC, R12, LSL #1]
L(ALU_TABLE_EB_GB):
    ALU_TABLE_M L(ALU_TABLE_EB_GB), L(ALU_DISPATCH_8), 8

OP .op_alu_ev_gv
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(ALU_EV_GV_MEM)
    UBFX R4, R1, #3, #3
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDREG_V R5, R4
    AND.W R2, R1, #7
    ADD.W R2, R8, R2, LSL #2
    ADD.W R2, R2, #8
    LD_V R3, R2
    B.N L(ALU_DISPATCH_32_SUBOP)
L(ALU_EV_GV_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    UBFX R4, R1, #3, #3
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDREG_V R5, R4
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LD_V R3, R2
L(ALU_DISPATCH_32_SUBOP):
    UBFX R12, R0, #3, #3
L(ALU_DISPATCH_32):
    TBH [PC, R12, LSL #1]
L(ALU_TABLE_EV_GV):
    ALU_TABLE_M L(ALU_TABLE_EV_GV), L(ALU_DISPATCH_32), 

OP_AGNOSTIC .op_alu_gb_eb
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(ALU_GB_EB_MEM)
    AND.W R2, R1, #7
    AND.W R3, R2, #3
    MOV.W R2, R2, LSR #2
    ADD.W R2, R2, R3, LSL #2
    ADD.W R2, R2, #8
    LDRB.W R5, [R8, R2]
    UBFX R4, R1, #3, #3
    AND.W R3, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R3, LSL #2
    ADD.W R4, R4, #8
    ADD.W R2, R8, R4
    LDRB R3, [R2, #0]
    B.N L(ALU_DISPATCH_8_GB_EB_SUBOP)
L(ALU_GB_EB_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LDRB R5, [R2, #0]
    UBFX R4, R1, #3, #3
    AND.W R3, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R3, LSL #2
    ADD.W R4, R4, #8
    ADD.W R2, R8, R4
    LDRB R3, [R2, #0]
L(ALU_DISPATCH_8_GB_EB_SUBOP):
    UBFX R12, R0, #3, #3
L(ALU_DISPATCH_8_GB_EB):
    TBH [PC, R12, LSL #1]
L(ALU_TABLE_GB_EB):
    ALU_TABLE_M L(ALU_TABLE_GB_EB), L(ALU_DISPATCH_8_GB_EB), 8

OP .op_alu_gv_ev
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(ALU_GV_EV_MEM)
    AND.W R2, R1, #7
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    LDREG_V R5, R2
    UBFX R4, R1, #3, #3
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    ADD.W R2, R8, R4
    LD_V R3, R2
    B.N L(ALU_DISPATCH_32_GV_EV_SUBOP)
L(ALU_GV_EV_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LD_V R5, R2
    UBFX R4, R1, #3, #3
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    ADD.W R2, R8, R4
    LD_V R3, R2
L(ALU_DISPATCH_32_GV_EV_SUBOP):
    UBFX R12, R0, #3, #3
L(ALU_DISPATCH_32_GV_EV):
    TBH [PC, R12, LSL #1]
L(ALU_TABLE_GV_EV):
    ALU_TABLE_M L(ALU_TABLE_GV_EV), L(ALU_DISPATCH_32_GV_EV), 

OP_AGNOSTIC .op_alu_al_imm8
    CODE_PTR R5
    LDRB R5, [R5, #0]
    ADD.W R10, R10, #1
    ADD.W R2, R8, #8
    LDRB R3, [R2, #0]
L(ALU_DISPATCH_8_AL_SUBOP):
    UBFX R12, R0, #3, #3
L(ALU_DISPATCH_8_AL):
    TBH [PC, R12, LSL #1]
L(ALU_TABLE_AL):
    ALU_TABLE_M L(ALU_TABLE_AL), L(ALU_DISPATCH_8_AL), 8

OP .op_alu_eax_imm32
    CODE_PTR R5
    LD_V R5, R5
    EIP_ADV_V
    ADD.W R2, R8, #8
    LD_V R3, R2
L(ALU_DISPATCH_32_EAX_SUBOP):
    UBFX R12, R0, #3, #3
L(ALU_DISPATCH_32_EAX):
    TBH [PC, R12, LSL #1]
L(ALU_TABLE_EAX):
    ALU_TABLE_M L(ALU_TABLE_EAX), L(ALU_DISPATCH_32_EAX), 

OP_AGNOSTIC .op_alu_eb_imm8_80
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    UBFX R12, R1, #3, #3
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(ALU_EB_IMM8_80_MEM)
    CODE_PTR R5
    LDRB R5, [R5, #0]
    ADD.W R10, R10, #1
    AND.W R2, R1, #7
    AND.W R3, R2, #3
    ADD.W R3, R8, R3, LSL #2
    ADD.W R2, R3, R2, LSR #2
    ADD.W R2, R2, #8
    LDRB R3, [R2, #0]
    B.N L(ALU_DISPATCH_EB_IMM8_80_SUBOP)
L(ALU_EB_IMM8_80_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    CODE_PTR R5
    LDRB R5, [R5, #0]
    ADD.W R10, R10, #1
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LDRB R3, [R2, #0]
L(ALU_DISPATCH_EB_IMM8_80_SUBOP):
    UBFX R12, R1, #3, #3
L(ALU_DISPATCH_EB_IMM8_80):
    TBH [PC, R12, LSL #1]
L(ALU_TABLE_EB_IMM8_80):
    ALU_TABLE_M L(ALU_TABLE_EB_IMM8_80), L(ALU_DISPATCH_EB_IMM8_80), 8

OP .op_alu_ev_imm32_81
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    UBFX R12, R1, #3, #3
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(ALU_EV_IMM32_81_MEM)
    CODE_PTR R5
    LD_V R5, R5
    EIP_ADV_V
    AND.W R2, R1, #7
    ADD.W R2, R8, R2, LSL #2
    ADD.W R2, R2, #8
    LD_V R3, R2
    B.N L(ALU_DISPATCH_EV_IMM32_81_SUBOP)
L(ALU_EV_IMM32_81_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    CODE_PTR R5
    LD_V R5, R5
    EIP_ADV_V
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LD_V R3, R2
L(ALU_DISPATCH_EV_IMM32_81_SUBOP):
    UBFX R12, R1, #3, #3
L(ALU_DISPATCH_EV_IMM32_81):
    TBH [PC, R12, LSL #1]
L(ALU_TABLE_EV_IMM32_81):
    ALU_TABLE_M L(ALU_TABLE_EV_IMM32_81), L(ALU_DISPATCH_EV_IMM32_81), 

OP .op_alu_ev_imm8_83
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    UBFX R12, R1, #3, #3
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(ALU_EV_IMM8_83_MEM)
    CODE_PTR R5
    LDRSB.W R5, [R5]
    ADD.W R10, R10, #1
    AND.W R2, R1, #7
    ADD.W R2, R8, R2, LSL #2
    ADD.W R2, R2, #8
    LD_V R3, R2
    B.N L(ALU_DISPATCH_EV_IMM8_83_SUBOP)
L(ALU_EV_IMM8_83_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    CODE_PTR R5
    LDRSB.W R5, [R5]
    ADD.W R10, R10, #1
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LD_V R3, R2
L(ALU_DISPATCH_EV_IMM8_83_SUBOP):
    UBFX R12, R1, #3, #3
L(ALU_DISPATCH_EV_IMM8_83):
    TBH [PC, R12, LSL #1]
L(ALU_TABLE_EV_IMM8_83):
    ALU_TABLE_M L(ALU_TABLE_EV_IMM8_83), L(ALU_DISPATCH_EV_IMM8_83), 

/* TEST is an AND that keeps the flags and throws away the result. All three
   forms -- 84, A8 and A9 -- computed the AND and then set no flag at all, so a
   program branching on a bit test always took the same path. 84 did not even
   compute it: the handler decoded the ModRM and returned. */
OP_AGNOSTIC .op_test_eb_gb_84
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(TEST_EB_GB_84_MEM)
    UBFX R4, R1, #3, #3            @ Gb: high byte registers are regs 4-7
    AND.W R5, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R5, LSL #2
    ADD.W R4, R4, #8
    LDRB.W R5, [R8, R4]
    AND.W R2, R1, #7               @ Eb
    AND.W R3, R2, #3
    MOV.W R2, R2, LSR #2
    ADD.W R2, R2, R3, LSL #2
    ADD.W R2, R2, #8
    LDRB.W R3, [R8, R2]
L(TEST_EB_GB_84_FLAGS):
    AND.W R3, R3, R5
    LSLS.W R0, R3, #24             @ set N/Z for the byte width
    X86_FLAGS_LOGIC R3, R0, R1
    B.W .fetch_decode_loop
/* Gb is read AFTER the decoder: DECODE_MODRM uses R4 and R5 as scratch. */
L(TEST_EB_GB_84_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    MEM_GET_PTR_R R12, R7
    LDRB R3, [R12]
    UBFX R4, R1, #3, #3
    AND.W R5, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R5, LSL #2
    ADD.W R4, R4, #8
    LDRB.W R5, [R8, R4]
    B.N L(TEST_EB_GB_84_FLAGS)

OP .op_test_ev_gv_85
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(TEST_EV_GV_85_MEM)
    UBFX R4, R1, #3, #3            @ reg operand
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDREG_V R5, R4
    AND.W R2, R1, #7               @ r/m operand (register form)
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    LDREG_V R3, R2
L(TEST_EV_GV_85_FLAGS):
    AND.W R3, R3, R5               @ TEST sets flags and discards the result
    VTEST(R0, R3)
    X86_FLAGS_LOGIC R3, R0, R1
    B.W .fetch_decode_loop
/* The ModRM decoder uses R4 and R5 as scratch, so the register operand has to
   be read AFTER the call, not before. R1 (the ModRM byte) does survive it. */
L(TEST_EV_GV_85_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    MEM_GET_PTR_R R12, R7
    LD_V R3, R12
    UBFX R4, R1, #3, #3
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDREG_V R5, R4
    B.N L(TEST_EV_GV_85_FLAGS)

OP_AGNOSTIC .op_xchg_eb_gb_86
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(XCHG_EB_GB_86_MEM)
    UBFX R4, R1, #3, #3
    AND.W R5, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R5, LSL #2
    ADD.W R4, R4, #8
    LDRB.W R5, [R8, R4]
    AND.W R2, R1, #7
    AND.W R3, R2, #3
    MOV.W R2, R2, LSR #2
    ADD.W R2, R2, R3, LSL #2
    ADD.W R2, R2, #8
    LDRB.W R3, [R8, R2]
    STRB.W R5, [R8, R2]
    STRB.W R3, [R8, R4]
    B.W .fetch_decode_loop
L(XCHG_EB_GB_86_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    UBFX R4, R1, #3, #3            @ Gb, decoded after the call
    AND.W R5, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R5, LSL #2
    ADD.W R4, R4, #8
    LDRB.W R5, [R8, R4]
    MEM_GET_PTR_RMW R12, R7
    LDRB R3, [R12]
    VGA_ST_B R5, R12, R7
    STRB.W R3, [R8, R4]
    B.W .fetch_decode_loop

OP .op_xchg_ev_gv_87
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(XCHG_EV_GV_87_MEM)
    UBFX R4, R1, #3, #3
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDREG_V R5, R4
    AND.W R2, R1, #7
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    LDREG_V R3, R2
    STREG_V R5, R2
    STREG_V R3, R4
    B.W .fetch_decode_loop
L(XCHG_EV_GV_87_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    UBFX R4, R1, #3, #3            @ Gv, decoded after the call
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDREG_V R5, R4
    MEM_GET_PTR_RMW R12, R7
    LD_V R3, R12
    VGA_ST_V R5, R12, R7
    STREG_V R3, R4
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_mov_eb_gb_88
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.W L(MOV_EB_GB_MEM)
    UBFX R4, R1, #3, #3
    AND.W R5, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R5, LSL #2
    ADD.W R4, R4, #8
    LDRB.W R5, [R8, R4]
    AND.W R2, R1, #7
    AND.W R3, R2, #3
    MOV.W R2, R2, LSR #2
    ADD.W R2, R2, R3, LSL #2
    ADD.W R2, R2, #8
    STRB.W R5, [R8, R2]
    B.W .fetch_decode_loop

/* MOV Eb, Gb -- memory form. The whole 88/89/8A/8B family had none: the
   non-register ModRM went straight to .exit_loop, and no test used a memory
   operand with MOV, so the suite was green over a hole. */
L(MOV_EB_GB_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    UBFX R4, R1, #3, #3
    AND.W R5, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R5, LSL #2
    ADD.W R4, R4, #8
    LDRB.W R5, [R8, R4]
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_W R2
    VGA_ST_B R5, R2, R12
    B.W .fetch_decode_loop

OP .op_mov_ev_gv_89
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.W L(MOV_EV_GV_MEM)
    UBFX R4, R1, #3, #3
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDREG_V R5, R4
    AND.W R2, R1, #7
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    STREG_V R5, R2
    B.W .fetch_decode_loop

L(MOV_EV_GV_MEM):
    /* MOV Ev, Gv -- memory form. The source register is read AFTER the decode:
       DECODE_MODRM uses R5 as scratch, so hoisting the load above the call to
       fill the walk's shadow silently corrupts the stored value. R1 survives
       the call, so the reg field is still available here. */
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    UBFX R4, R1, #3, #3
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDREG_V R5, R4
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_W R2
    VGA_ST_V R5, R2, R12
    B.W .fetch_decode_loop

OP .op_lea_8D
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BHS.W .exit_loop
    UBFX R4, R1, #3, #3
    ADD.W R4, R8, R4, LSL #2
    ADD.W R4, R4, #8
    PUSH {R2, R4}
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    POP {R2, R4}
    /* The decoder returns a LINEAR address -- L(MODRM_SEG) folds the segment
       base in, because that is what every memory operand needs. LEA is the one
       instruction that wants the offset instead, so take the base back out and
       wrap at the address size. Storing R7 raw made LEA return base+offset,
       which is invisible while DS.base is 0 and catastrophic the moment it is
       not: TOPBENCH's unpacker turns an overflowing DI into a segment bump with
       LEA, and got 0x967 paragraphs instead of 0x5FE. The store is ST_V so a
       16-bit LEA leaves the high half of the destination alone. */
    LDR.W R5, [R6, #4]             @ the base MODRM_SEG added
    SUB.W R7, R7, R5
    TST.W R2, #ADDRSIZE_XOR
    IT EQ
    UXTHEQ R7, R7
    ST_V R7, R4
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_mov_gb_eb_8A
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.W L(MOV_GB_EB_MEM)
    AND.W R4, R1, #7
    AND.W R5, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R5, LSL #2
    ADD.W R4, R4, #8
    LDRB.W R5, [R8, R4]
    UBFX R2, R1, #3, #3
    AND.W R3, R2, #3
    MOV.W R2, R2, LSR #2
    ADD.W R2, R2, R3, LSL #2
    ADD.W R2, R2, #8
    STRB.W R5, [R8, R2]
    B.W .fetch_decode_loop

L(MOV_GB_EB_MEM):
    /* MOV Gb, Eb -- memory form. */
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LDRB R5, [R2, #0]
    UBFX R2, R1, #3, #3
    AND.W R3, R2, #3
    MOV.W R2, R2, LSR #2
    ADD.W R2, R2, R3, LSL #2
    ADD.W R2, R2, #8
    STRB.W R5, [R8, R2]
    B.W .fetch_decode_loop

OP .op_mov_gv_ev_8B
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.W L(MOV_GV_EV_MEM)
    AND.W R4, R1, #7
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDREG_V R5, R4
    UBFX R2, R1, #3, #3
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    STREG_V R5, R2
    B.W .fetch_decode_loop

L(MOV_GV_EV_MEM):
    /* MOV Gv, Ev -- memory form. */
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LD_V R5, R2
    UBFX R2, R1, #3, #3
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    STREG_V R5, R2
    B.W .fetch_decode_loop

OP .op_pop_ev_8F
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(POP_EV_8F_MEM)
    AND.W R2, R1, #7
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    POP_V R3, R4, R12
    STREG_V R3, R2
    B.W .fetch_decode_loop
L(POP_EV_8F_MEM):
    /* Address first, then the pop: on a [BP] or [SP] form the 8086 computes
       the effective address before SP moves. */
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    PUSH {R7}
    POP_V R3, R4, R12
    POP {R7}
    MEM_GET_PTR R12, R7
    VGA_ST_V R3, R12, R4
    B.W .fetch_decode_loop

/* XCHG at the operand size. Both this and 0x87 swapped the full 32 bits in
   16-bit mode: the low halves still ended up in the right places, so it looked
   harmless, but the HIGH halves were swapped too. That is how EAX's high half
   reaches BP in `LODSW ; XCHG BP,AX`, and a 16-bit SHR that reads the operand
   at 32 bits then shifts it straight back down into BP. */
OP .op_xchg_eax_r32
    AND.W R2, R0, #7
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    LDREG_V R4, #REG_EAX
    LDREG_V R3, R2
    STREG_V R4, R2
    STREG_V R3, #REG_EAX
    B.W .fetch_decode_loop

/* CBW/CWDE and CWD/CDQ differ by operand size in what they read AND what they
   write. Both were emitted for each pass but carried the 32-bit body, so a
   16-bit guest got CWDE and CDQ -- CBW then destroyed AH instead of filling it
   from AL's sign, and CWD left DX untouched. */
OP .op_cwde_98
#if NSH
    LDRSB.W R3, [R8, #REG_EAX]     @ CBW: AL -> AX
    STRH.W R3, [R8, #REG_EAX]
#else
    LDR.W R3, [R8, #REG_EAX]
    SXTH R3, R3
    STR.W R3, [R8, #REG_EAX]
#endif
    B.W .fetch_decode_loop

OP .op_cdq_99
#if NSH
    LDRSH.W R3, [R8, #REG_EAX]     @ CWD: the sign of AX fills DX
    MOV.W R4, R3, ASR #31
    STRH.W R4, [R8, #REG_EDX]
#else
    LDR.W R3, [R8, #REG_EAX]
    MOV.W R4, R3, ASR #31
    STR.W R4, [R8, #REG_EDX]
#endif
    B.W .fetch_decode_loop

OP .op_pushfd_9C
    PUSH_V R11, R4, R12
    B.W .fetch_decode_loop

OP .op_popfd_9D
    POP_V R3, R4, R12
#if NSH
    BFI R11, R3, #0, #16           @ POPF at 16 bits leaves EFLAGS[31:16] alone
#else
    MOV.W R11, R3
#endif
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_mov_al_moffs8_A0
    MOFFS_ADDR R4, R3
    MEM_GET_PTR_RB R3, R4
    LDRB R3, [R3, #0]
    STRB.W R3, [R8, #REG_EAX]
    B.W .fetch_decode_loop

OP .op_mov_eax_moffs32_A1
    MOFFS_ADDR R4, R3
    MEM_GET_PTR_R R3, R4
    LD_V R3, R3
    STREG_V R3, #REG_EAX           @ 16-bit MOV must not clear the high half
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_mov_moffs8_al_A2
    MOFFS_ADDR R4, R3
    LDRB.W R3, [R8, #REG_EAX]
    MEM_GET_PTR_WB R12, R4
    VGA_ST_B R3, R12, R4
    B.W .fetch_decode_loop

OP .op_mov_moffs32_eax_A3
    MOFFS_ADDR R4, R3
    LDREG_V R3, #REG_EAX
    MEM_GET_PTR R12, R4
    VGA_ST_V R3, R12, R4
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_test_al_imm8_A8
    CODE_PTR R5
    LDRB R5, [R5, #0]
    ADD.W R10, R10, #1
    LDRB.W R3, [R8, #8]
    AND.W R3, R3, R5
    LSLS.W R0, R3, #24
    X86_FLAGS_LOGIC R3, R0, R1
    B.W .fetch_decode_loop

OP .op_test_eax_imm32_A9
    CODE_PTR R5
    LD_V R5, R5
    EIP_ADV_V
    LDREG_V R3, #REG_EAX
    AND.W R3, R3, R5
    VTEST(R0, R3)
    X86_FLAGS_LOGIC R3, R0, R1
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_movsb_A4
    STR_SRC R4, R3
    MEM_GET_PTR_RB R3, R4
    LDRB R3, [R3]
    STR_DST R5, R12
    MEM_GET_PTR_WB R12, R5
    VGA_ST_B R3, R12, R5
    STR_DELTA R7, 1
    STR_ADV REG_ESI, R7, R4
    STR_ADV REG_EDI, R7, R5
    B.W .str_exit

OP .op_movsd_A5
    STR_SRC R4, R3
    MEM_GET_PTR_R R3, R4
    LD_V R3, R3
    STR_DST R5, R12
    MEM_GET_PTR R12, R5
    VGA_ST_V R3, R12, R5
    STR_DELTA R7, OPSZ
    STR_ADV REG_ESI, R7, R4
    STR_ADV REG_EDI, R7, R5
    B.W .str_exit

OP_AGNOSTIC .op_cmpsb_A6
    STR_SRC R4, R3
    MEM_GET_PTR_RB R3, R4
    LDRB R3, [R3]
    STR_DST R5, R12
    MEM_GET_PTR_RB R12, R5
    LDRB R5, [R12]
    LSL.W R3, R3, #24
    LSL.W R5, R5, #24
    EOR.W R4, R3, R5
    SUBS R3, R3, R5
    EOR.W R4, R4, R3
    LSR.W R3, R3, #24
    X86_FLAGS_ARITH R3, R4, R0, R1, 1, 24
    STR_DELTA R7, 1
    STR_ADV REG_ESI, R7, R4
    STR_ADV REG_EDI, R7, R5
    B.W .str_exit

OP .op_cmpsd_A7
    STR_SRC R4, R3
    MEM_GET_PTR_R R3, R4
    LD_V R3, R3
    STR_DST R5, R12
    MEM_GET_PTR_R R12, R5
    LD_V R5, R12
    VNORM(R3)
    VNORM(R5)
    EOR.W R4, R3, R5
    SUBS R3, R3, R5
    EOR.W R4, R4, R3
    VDENORM(R3)
    X86_FLAGS_ARITH R3, R4, R0, R1, 1, NSH
    STR_DELTA R7, OPSZ
    STR_ADV REG_ESI, R7, R4
    STR_ADV REG_EDI, R7, R5
    B.W .str_exit

OP_AGNOSTIC .op_stosb_AA
    STR_DST R5, R12
    MEM_GET_PTR_WB R12, R5
    LDRB.W R3, [R8, #REG_EAX]
    VGA_ST_B R3, R12, R5
    STR_DELTA R7, 1
    STR_ADV REG_EDI, R7, R5
    B.W .str_exit

OP .op_stosd_AB
    STR_DST R5, R12
    MEM_GET_PTR R12, R5
    LDREG_V R3, #REG_EAX
    VGA_ST_V R3, R12, R5
    STR_DELTA R7, OPSZ
    STR_ADV REG_EDI, R7, R5
    B.W .str_exit

OP_AGNOSTIC .op_lodsb_AC
    STR_SRC R4, R3
    MEM_GET_PTR_RB R3, R4
    LDRB R3, [R3]
    STRB.W R3, [R8, #REG_EAX]
    STR_DELTA R7, 1
    STR_ADV REG_ESI, R7, R4
    B.W .str_exit

OP .op_lodsd_AD
    STR_SRC R4, R3
    MEM_GET_PTR_R R3, R4
    LD_V R3, R3
    STREG_V R3, #REG_EAX
    STR_DELTA R7, OPSZ
    STR_ADV REG_ESI, R7, R4
    B.W .str_exit

OP_AGNOSTIC .op_scasb_AE
    STR_DST R5, R12
    MEM_GET_PTR_RB R12, R5
    LDRB R5, [R12]
    LDRB.W R3, [R8, #REG_EAX]
    LSL.W R3, R3, #24
    LSL.W R5, R5, #24
    EOR.W R4, R3, R5
    SUBS R3, R3, R5
    EOR.W R4, R4, R3
    LSR.W R3, R3, #24
    X86_FLAGS_ARITH R3, R4, R0, R1, 1, 24
    STR_DELTA R7, 1
    STR_ADV REG_EDI, R7, R5
    B.W .str_exit

OP .op_scasd_AF
    STR_DST R5, R12
    MEM_GET_PTR_R R12, R5
    LD_V R5, R12
    LDREG_V R3, #REG_EAX
    VNORM(R3)
    VNORM(R5)
    EOR.W R4, R3, R5
    SUBS R3, R3, R5
    EOR.W R4, R4, R3
    VDENORM(R3)
    X86_FLAGS_ARITH R3, R4, R0, R1, 1, NSH
    STR_DELTA R7, OPSZ
    STR_ADV REG_EDI, R7, R5
    B.W .str_exit

/* ---------------------------------------------------------------------------
 * INS / OUTS (6C-6F) -- string I/O.
 *
 * These are the last string ops and they were the last opcodes reaching
 * .exit_loop on two real titles at once: TOPBENCH halts ~3 s in on `INSB`, and
 * WOLF3D's VL_SetPalette is literally `mov cx,768 / rep outsb` to 0x3C9, so
 * the whole 256-colour palette went through ONE opcode we did not have. The
 * screen was black with x86_vga_dac_writes = 0 and the guest halted with
 * exit_reason 4 -- which reads exactly like a video bug and is not one.
 *
 * They are ordinary string ops with the memory operand on one side and a PORT
 * on the other, so they get .str_exit and REP for free. The one thing that is
 * NOT free is R2: `x86_port_read`/`x86_port_write` are C calls and R2 is
 * caller-saved, but R2 is the MODE RECORD (CLAUDE.md) and .str_exit needs it
 * for the address-size test and for the batched re-entry. R4 carries it across
 * the call -- R4-R11 are callee-saved, so the C side cannot touch it -- and
 * every use of R2 for the index advance happens BEFORE the call, never after.
 *
 * INS takes ES:DI and ignores a segment override (architectural); OUTS takes
 * DS:SI through the override, which STR_SRC already implements.
 * ------------------------------------------------------------------------- */
OP_AGNOSTIC .op_insb_6C
    MOV R4, R2                     @ the mode record, across the C call
    CHECK_IOPL R2, R3
    LDRH.W R0, [R8, #REG_EDX]
    MOV.W R1, #1
    BL x86_port_read
    MOV R2, R4
    MOV R1, R0
    STR_DST R5, R12
    MEM_GET_PTR_WB R12, R5
    VGA_ST_B R1, R12, R4
    STR_DELTA R7, 1
    STR_ADV REG_EDI, R7, R5
    B.W .str_exit

OP .op_insv_6D
    MOV R4, R2
    CHECK_IOPL R2, R3
    LDRH.W R0, [R8, #REG_EDX]
    MOV.W R1, #OPSZ
    BL x86_port_read
    MOV R2, R4
    MOV R1, R0
    STR_DST R5, R12
    MEM_GET_PTR R12, R5
    VGA_ST_V R1, R12, R4
    STR_DELTA R7, OPSZ
    STR_ADV REG_EDI, R7, R5
    B.W .str_exit

OP_AGNOSTIC .op_outsb_6E
    STR_SRC R4, R3
    MEM_GET_PTR_RB R3, R4
    LDRB R1, [R3]
    STR_DELTA R7, 1
    STR_ADV REG_ESI, R7, R4        @ the index advance, while R2 is still live
    MOV R4, R2
    CHECK_IOPL R2, R3
    LDRH.W R0, [R8, #REG_EDX]
    MOV.W R2, #1
    BL x86_port_write
    MOV R2, R4
    B.W .str_exit

OP .op_outsv_6F
    STR_SRC R4, R3
    MEM_GET_PTR_R R3, R4
    LD_V R1, R3
    STR_DELTA R7, OPSZ
    STR_ADV REG_ESI, R7, R4
    MOV R4, R2
    CHECK_IOPL R2, R3
    LDRH.W R0, [R8, #REG_EDX]
    MOV.W R2, #OPSZ
    BL x86_port_write
    MOV R2, R4
    B.W .str_exit

OP_AGNOSTIC .op_mov_r8_imm8
    AND.W R2, R0, #7
    AND.W R3, R2, #3
    MOV.W R2, R2, LSR #2
    ADD.W R2, R2, R3, LSL #2
    ADD.W R2, R2, #8
    CODE_PTR R3
    LDRB R3, [R3, #0]
    ADD.W R10, R10, #1
    STRB.W R3, [R8, R2]
    B.W .fetch_decode_loop

OP .op_ret_C3
    POP_V R4, R0, R12
    LDR.W R3, [R8, #SEG_CS_BASE]
    ADD.W R10, R4, R3
    B.W .fetch_decode_loop

OP .op_ret_imm16_C2
    CODE_PTR R5
    LDRH R5, [R5, #0]              @ bytes to drop; always 16 bits
    POP_V R4, R0, R12
    LDR.W R3, [R8, #SEG_CS_BASE]
    ADD.W R10, R4, R3              @ the popped value is an OFFSET
    LDR.W R4, [R8, #REG_ESP]
    ADD R4, R5
    SP_WRAP R4
    STR.W R4, [R8, #REG_ESP]
    B.W .fetch_decode_loop

OP .op_leave_C9
    LDR.W R3, [R8, #REG_EBP]
    SP_WRAP R3
    STR.W R3, [R8, #REG_ESP]
    POP_V R3, R4, R12
    STREG_V R3, #REG_EBP
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_mov_eb_imm8_C6
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(MOV_EB_IMM8_C6_MEM)
    CODE_PTR R5
    LDRB R5, [R5, #0]
    ADD.W R10, R10, #1
    AND.W R2, R1, #7
    AND.W R3, R2, #3
    MOV.W R2, R2, LSR #2
    ADD.W R2, R2, R3, LSL #2
    ADD.W R2, R2, #8
    STRB.W R5, [R8, R2]
    B.W .fetch_decode_loop
/* The memory form. The decoder has to run FIRST, because the immediate follows
   the displacement -- reading it before the ModRM is consumed takes bytes out
   of the middle of the instruction. */
L(MOV_EB_IMM8_C6_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    CODE_PTR R5
    LDRB R5, [R5, #0]
    ADD.W R10, R10, #1
    MEM_GET_PTR R12, R7
    VGA_ST_B R5, R12, R4
    B.W .fetch_decode_loop

OP .op_mov_ev_imm32_C7
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(MOV_EV_IMM_C7_MEM)
    CODE_PTR R5
    LD_V R5, R5
    EIP_ADV_V
    AND.W R2, R1, #7
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    STREG_V R5, R2
    B.W .fetch_decode_loop
L(MOV_EV_IMM_C7_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    CODE_PTR R5
    LD_V R5, R5
    EIP_ADV_V
    MEM_GET_PTR R12, R7
    VGA_ST_V R5, R12, R4
    B.W .fetch_decode_loop

/* C1 / D1 / D3 shift a v-width operand, so they are NOT pass-agnostic: they
   were, which meant the single 32-bit-pass body served both opcode tables and
   every 16-bit shift ran at 32 bits. The result was right whenever it happened
   to fit in 16 bits and silently wrong when it did not -- `SHL SI,CL` with a
   carry out of bit 15 left the overflow in the high half of ESI, which a
   16-bit relocator then used as an address. */
OP .op_shift_ev_imm8_C1
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(SHIFT_EV_IMM8_C1_MEM)
    AND.W R2, R1, #7
    ADD.W R2, R8, R2, LSL #2
    ADD.W R2, R2, #8
    LD_V R3, R2
    CODE_PTR R5
    LDRB R5, [R5, #0]
    ADD.W R10, R10, #1
    AND.W R5, R5, #31
    B.N L(SHIFT_DISPATCH_32)
L(SHIFT_EV_IMM8_C1_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LD_V R3, R2
    CODE_PTR R5
    LDRB R5, [R5, #0]
    ADD.W R10, R10, #1
    AND.W R5, R5, #31
    B.N L(SHIFT_DISPATCH_32)

OP .op_shift_ev_1_D1
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    MOV.W R5, #1
    B.N L(SHIFT_EV_DECODE)

OP .op_shift_ev_cl_D3
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    LDRB.W R5, [R8, #12]
    AND.W R5, R5, #31
L(SHIFT_EV_DECODE):
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(SHIFT_EV_MEM)
    AND.W R2, R1, #7
    ADD.W R2, R8, R2, LSL #2
    ADD.W R2, R2, #8
    LD_V R3, R2
    B.N L(SHIFT_DISPATCH_32)
L(SHIFT_EV_MEM):
    PUSH {R5}
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    POP {R5}
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LD_V R3, R2
L(SHIFT_DISPATCH_32):
    UBFX R12, R1, #3, #3
    /* The table offsets are relative to the TBH's own PC, so the label the
       table is computed against must sit HERE, not on the UBFX above. */
L(SHIFT_TBH_32):
    TBH [PC, R12, LSL #1]
L(SHIFT_TABLE):
    SHIFT_TABLE_M L(SHIFT_TABLE), L(SHIFT_TBH_32), 
/* v-width shifts and rotates, 16- or 32-bit by pass.
 *
 * Same defects as the 8-bit set had: the rotates set no flags, RCL/RCR were
 * empty stubs, OF was never written, the count was not masked to five bits,
 * and every one of them operated on the full 32-bit register regardless of
 * operand size. In the 16-bit pass that last one alone made every result wrong
 * above bit 15.
 *
 * The count is masked BEFORE the zero test so a masked-to-zero count leaves
 * the flags alone, which is what a program relies on when it shifts by a
 * variable that may be zero while carrying a CF it still needs.
 */
L(SHIFT_ROL):
    ANDS R5, R5, #31
    BEQ.W L(SHIFT_NOFLAGS)
    AND.W R5, R5, #(VBITS - 1)
    CMP R5, #0
    BEQ.W L(SHIFT_ROL_CF_ONLY)
    RSB R12, R5, #VBITS
    LSL.W R4, R3, R5
    LSR.W R3, R3, R12
    ORR.W R3, R3, R4
    VMASK(R3)
    AND.W R0, R3, #1
    B.W L(SHIFT_ROT_FLAGS)

L(SHIFT_ROR):
    ANDS R5, R5, #31
    BEQ.W L(SHIFT_NOFLAGS)
    AND.W R5, R5, #(VBITS - 1)
    CMP R5, #0
    BEQ.W L(SHIFT_ROR_CF_ONLY)
    RSB R12, R5, #VBITS
    LSR.W R4, R3, R5
    LSL.W R3, R3, R12
    ORR.W R3, R3, R4
    VMASK(R3)
    UBFX R0, R3, #VMSB, #1
    B.W L(SHIFT_ROTR_FLAGS)

/* A count that is a multiple of the width leaves the value alone, but CF is
   still DEFINED and it is not the same bit for the two directions: ROL takes
   the bit that arrived in bit 0, ROR the one that arrived in the MSB. Sharing
   one epilogue gave ROL the MSB. */
L(SHIFT_ROL_CF_ONLY):
    AND.W R0, R3, #1
    B.W L(SHIFT_ROT_FLAGS)
L(SHIFT_ROR_CF_ONLY):
    UBFX R0, R3, #VMSB, #1
/* RIGHT rotates: OF is the XOR of the TWO most significant bits of the result,
   not MSB ^ CF. For ROR, CF *is* the MSB of the result, so the shared left-hand
   epilogue computed MSB ^ MSB == 0 and OF was never set at all. */
L(SHIFT_ROTR_FLAGS):
    BFI R11, R0, #0, #1             @ CF
    UBFX R1, R3, #VMSB, #1
    UBFX R12, R3, #(VMSB - 1), #1
    EOR.W R1, R1, R12
    BFI R11, R1, #11, #1            @ OF, defined for a count of 1
    B.W L(SHIFT_DONE)

/* LEFT rotates: OF = MSB(result) ^ CF-out. */
L(SHIFT_ROT_FLAGS):
    BFI R11, R0, #0, #1             @ CF
    UBFX R1, R3, #VMSB, #1
    EOR.W R1, R1, R0
    BFI R11, R1, #11, #1            @ OF, defined for a count of 1
    B.W L(SHIFT_DONE)

L(SHIFT_RCL):
    ANDS R5, R5, #31
    BEQ.W L(SHIFT_NOFLAGS)
    /* Rotating through carry is a VBITS+1 rotate, so the modulus is not a
       power of two. A 33-bit value does not fit a register, so this is built
       from explicit shifts rather than by parking CF above the operand:
       result = (v << c) | (cf << (c-1)) | (v >> (VBITS+1-c)), which works for
       both widths because an ARM register shift of 32 or more yields zero. */
    MOV.W R4, #(VBITS + 1)
    UDIV R12, R5, R4
    MLS R5, R12, R4, R5
    CMP R5, #0
    BEQ.W L(SHIFT_DONE)
    UBFX R0, R11, #0, #1            @ CF in
    RSB R1, R5, #VBITS
    LSR.W R1, R3, R1
    AND.W R1, R1, #1                @ CF out = bit (VBITS - c)
    RSB R12, R5, #(VBITS + 1)
    LSR.W R4, R3, R12
    SUB.W R12, R5, #1
    LSL.W R0, R0, R12
    LSL.W R3, R3, R5
    ORR.W R3, R3, R4
    ORR.W R3, R3, R0
    VMASK(R3)
    MOV R0, R1
    B.W L(SHIFT_ROT_FLAGS)

L(SHIFT_RCR):
    ANDS R5, R5, #31
    BEQ.W L(SHIFT_NOFLAGS)
    MOV.W R4, #(VBITS + 1)
    UDIV R12, R5, R4
    MLS R5, R12, R4, R5
    CMP R5, #0
    BEQ.W L(SHIFT_DONE)
    UBFX R0, R11, #0, #1
    SUB.W R12, R5, #1
    LSR.W R1, R3, R12
    AND.W R1, R1, #1                @ CF out = bit (c - 1)
    RSB R12, R5, #VBITS
    LSL.W R4, R0, R12
    RSB R12, R5, #(VBITS + 1)
    LSL.W R0, R3, R12
    LSR.W R3, R3, R5
    ORR.W R3, R3, R4
    ORR.W R3, R3, R0
    VMASK(R3)
    MOV R0, R1
    B.W L(SHIFT_ROTR_FLAGS)

L(SHIFT_SHL):
    ANDS R5, R5, #31
    BEQ.W L(SHIFT_NOFLAGS)
    VNORM(R3)
    LSLS R3, R3, R5                 @ C = last bit out, N = SF, Z = ZF
    MRS R0, APSR
    VDENORM(R3)
    UBFX R1, R0, #29, #1
    BFI R11, R1, #0, #1             @ CF
    UBFX R4, R3, #VMSB, #1
    EOR.W R4, R4, R1
    BFI R11, R4, #11, #1            @ OF = MSB(result) ^ CF
    X86_SF_ZF R0, R1
    X86_PF R3, R1
    B.W L(SHIFT_DONE)

L(SHIFT_SHR):
    ANDS R5, R5, #31
    BEQ.W L(SHIFT_NOFLAGS)
    UBFX R4, R3, #VMSB, #1          @ OF is the ORIGINAL msb
    LSRS R3, R3, R5
    MRS R0, APSR
    UBFX R0, R0, #29, #1
    BFI R11, R0, #0, #1             @ CF
    BFI R11, R4, #11, #1            @ OF
    VTEST(R1, R3)
    MRS R12, APSR
    X86_SF_ZF R12, R1
    X86_PF R3, R1
    B.W L(SHIFT_DONE)

L(SHIFT_SAR):
    ANDS R5, R5, #31
    BEQ.W L(SHIFT_NOFLAGS)
    VSEXT(R3)
    ASRS R3, R3, R5
    MRS R0, APSR
    UBFX R0, R0, #29, #1
    BFI R11, R0, #0, #1             @ CF
    BIC.W R11, R11, #(1 << 11)      @ SAR always clears OF
    VMASK(R3)
    VTEST(R1, R3)
    MRS R12, APSR
    X86_SF_ZF R12, R1
    X86_PF R3, R1
    B.W L(SHIFT_DONE)

L(SHIFT_NOFLAGS):
    /* Masked count of zero: value unchanged, no flag moves. */
L(SHIFT_DONE):
    VGA_ST_V R3, R2, R12
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_shift_eb_imm8_C0
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(SHIFT_EB_IMM8_C0_MEM)
    AND.W R2, R1, #7
    AND.W R3, R2, #3
    ADD.W R3, R8, R3, LSL #2
    ADD.W R2, R3, R2, LSR #2
    ADD.W R2, R2, #8
    LDRB R3, [R2, #0]
    CODE_PTR R5
    LDRB R5, [R5, #0]
    ADD.W R10, R10, #1
    AND.W R5, R5, #31
    B.N L(SHIFT_DISPATCH_8)
L(SHIFT_EB_IMM8_C0_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LDRB R3, [R2, #0]
    CODE_PTR R5
    LDRB R5, [R5, #0]
    ADD.W R10, R10, #1
    AND.W R5, R5, #31
    B.N L(SHIFT_DISPATCH_8)

OP_AGNOSTIC .op_shift_eb_1_D0
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    MOV.W R5, #1
    B.N L(SHIFT_EB_DECODE)

OP_AGNOSTIC .op_shift_eb_cl_D2
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    LDRB.W R5, [R8, #12]
    AND.W R5, R5, #31
L(SHIFT_EB_DECODE):
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(SHIFT_EB_MEM)
    AND.W R2, R1, #7
    AND.W R3, R2, #3
    ADD.W R3, R8, R3, LSL #2
    ADD.W R2, R3, R2, LSR #2
    ADD.W R2, R2, #8
    LDRB R3, [R2, #0]
    B.N L(SHIFT_DISPATCH_8)
L(SHIFT_EB_MEM):
    PUSH {R5}
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    POP {R5}
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LDRB R3, [R2, #0]
L(SHIFT_DISPATCH_8):
    UBFX R12, R1, #3, #3
L(SHIFT_TBH_8):
    TBH [PC, R12, LSL #1]
L(SHIFT8_TABLE):
    SHIFT_TABLE_M L(SHIFT8_TABLE), L(SHIFT_TBH_8), 8
/* 8-bit shifts and rotates.
 *
 * These set no flags at all before now, and RCL/RCR did not rotate: they were
 * empty stubs that fell straight through to the store. Nothing tested them.
 *
 * The count is masked to five bits FIRST, because a masked count of zero must
 * leave every flag untouched -- including CF, which is the case a program uses
 * to shift "maybe zero" times without disturbing a carry it is carrying
 * around. Testing the raw count instead would treat 32 as non-zero and clobber
 * the flags.
 *
 * Operands are normalized to the top of the word where that lets the ARM shift
 * produce the x86 flags directly: after LSLS on a value sitting at bit 31, C is
 * the last bit shifted out, N is the result's MSB and Z covers exactly the byte.
 */
L(SHIFT8_ROL):
    ANDS R5, R5, #31
    BEQ.W L(SHIFT8_NOFLAGS)
    AND.W R5, R5, #7
    CBZ R5, L(SHIFT8_ROL_CF_ONLY)   @ count%8==0: value unchanged, CF = bit 0
    RSB R12, R5, #8
    LSL.W R4, R3, R5
    LSR.W R3, R3, R12
    ORR.W R3, R3, R4
    UXTB R3, R3
    AND.W R0, R3, #1                @ CF = the bit rotated into bit 0
    B.N L(SHIFT8_ROT_FLAGS)

L(SHIFT8_ROR):
    ANDS R5, R5, #31
    BEQ.W L(SHIFT8_NOFLAGS)
    AND.W R5, R5, #7
    CBZ R5, L(SHIFT8_ROR_CF_ONLY)
    RSB R12, R5, #8
    LSR.W R4, R3, R5
    LSL.W R3, R3, R12
    ORR.W R3, R3, R4
    UXTB R3, R3
    UBFX R0, R3, #7, #1             @ CF = the bit rotated into bit 7
    B.N L(SHIFT8_ROTR_FLAGS)

L(SHIFT8_ROL_CF_ONLY):
    AND.W R0, R3, #1
    B.N L(SHIFT8_ROT_FLAGS)
L(SHIFT8_ROR_CF_ONLY):
    UBFX R0, R3, #7, #1
/* RIGHT rotates: OF is the XOR of the two most significant bits of the RESULT.
   The left-hand form (MSB ^ CF) is identically zero for ROR, whose CF is that
   same MSB -- so OF was never set. See L(SHIFT_ROTR_FLAGS). */
L(SHIFT8_ROTR_FLAGS):
    BFI R11, R0, #0, #1
    UBFX R1, R3, #7, #1
    UBFX R12, R3, #6, #1
    EOR.W R1, R1, R12
    BFI R11, R1, #11, #1
    B.N L(SHIFT8_DONE)

L(SHIFT8_ROT_FLAGS):
    /* Rotates touch only CF and OF; SF, ZF, AF and PF are left alone. OF is
       architecturally defined for a count of 1 only, as MSB(result) ^ CF. */
    BFI R11, R0, #0, #1
    UBFX R1, R3, #7, #1
    EOR.W R1, R1, R0
    BFI R11, R1, #11, #1
    B.N L(SHIFT8_DONE)

L(SHIFT8_RCL):
    ANDS R5, R5, #31
    BEQ.W L(SHIFT8_NOFLAGS)
    /* 9-bit rotate: CF is part of the value. Build it, rotate within 9 bits,
       then split it apart again. */
    MOV.W R4, #9
    UDIV R12, R5, R4
    MLS R5, R12, R4, R5             @ count %= 9
    CMP R5, #0
    BEQ.W L(SHIFT8_DONE)
    UBFX R0, R11, #0, #1
    ORR.W R3, R3, R0, LSL #8        @ 9-bit value
    RSB R12, R5, #9
    LSL.W R4, R3, R5
    LSR.W R3, R3, R12
    ORR.W R3, R3, R4
    UBFX R0, R3, #8, #1             @ new CF
    UXTB R3, R3
    B.N L(SHIFT8_ROT_FLAGS)

L(SHIFT8_RCR):
    ANDS R5, R5, #31
    BEQ.W L(SHIFT8_NOFLAGS)
    MOV.W R4, #9
    UDIV R12, R5, R4
    MLS R5, R12, R4, R5
    CMP R5, #0
    BEQ.W L(SHIFT8_DONE)
    UBFX R0, R11, #0, #1
    ORR.W R3, R3, R0, LSL #8
    RSB R12, R5, #9
    LSR.W R4, R3, R5
    LSL.W R3, R3, R12
    ORR.W R3, R3, R4
    UBFX R0, R3, #8, #1
    UXTB R3, R3
    B.N L(SHIFT8_ROTR_FLAGS)

L(SHIFT8_SHL):
    ANDS R5, R5, #31
    BEQ.W L(SHIFT8_NOFLAGS)
    LSL.W R3, R3, #24               @ to the top, so the ARM flags are the x86 ones
    LSLS R3, R3, R5                 @ C = last bit out, N = SF, Z = ZF
    MRS R0, APSR
    LSR.W R3, R3, #24
    UBFX R1, R0, #29, #1            @ CF
    BFI R11, R1, #0, #1
    UBFX R4, R3, #7, #1
    EOR.W R4, R4, R1                @ OF = MSB(result) ^ CF
    BFI R11, R4, #11, #1
    X86_SF_ZF R0, R1
    X86_PF R3, R1
    B.N L(SHIFT8_DONE)

L(SHIFT8_SHR):
    ANDS R5, R5, #31
    BEQ.W L(SHIFT8_NOFLAGS)
    UBFX R4, R3, #7, #1             @ OF is the ORIGINAL msb, so grab it first
    LSRS R3, R3, R5                 @ C = last bit out
    MRS R0, APSR
    UBFX R0, R0, #29, #1
    BFI R11, R0, #0, #1             @ CF
    BFI R11, R4, #11, #1            @ OF
    LSLS R1, R3, #24                @ N and Z for the byte, not the word
    MRS R12, APSR
    X86_SF_ZF R12, R1
    X86_PF R3, R1
    B.N L(SHIFT8_DONE)

L(SHIFT8_SAR):
    ANDS R5, R5, #31
    BEQ.W L(SHIFT8_NOFLAGS)
    SXTB R3, R3
    ASRS R3, R3, R5                 @ C = last bit out
    MRS R0, APSR
    UBFX R0, R0, #29, #1
    BFI R11, R0, #0, #1             @ CF
    BIC.W R11, R11, #(1 << 11)      @ SAR always clears OF
    UXTB R3, R3
    LSLS R1, R3, #24
    MRS R12, APSR
    X86_SF_ZF R12, R1
    X86_PF R3, R1
    B.N L(SHIFT8_DONE)

L(SHIFT8_NOFLAGS):
    /* A masked count of zero: the value is unchanged and NO flag moves. */
L(SHIFT8_DONE):
    VGA_ST_B R3, R2, R12
    B.W .fetch_decode_loop

/* BCD adjust: DAA (27), DAS (2F), AAA (37), AAS (3F).
 *
 * Not exotic: a Pascal or C runtime formats decimal numbers with them, so a
 * program that prints a number at all is liable to execute one. TOPBENCH stops
 * on DAA the moment it has a score to print.
 *
 * Straight from the Intel pseudocode, including the parts that look wrong:
 * DAA's second test has an ELSE that clears CF, so its final carry depends only
 * on the ORIGINAL AL and carry, while DAS's has no ELSE and keeps the carry the
 * first step produced. AAA/AAS leave SF, ZF and PF undefined and so leave them
 * alone; DAA/DAS define them and set them from the result.
 */
OP_AGNOSTIC .op_daa_27
    LDRB.W R3, [R8, #REG_EAX]      @ AL
    MOV.W R4, R3                   @ old AL
    UBFX R1, R11, #0, #1           @ old CF
    AND.W R5, R3, #0x0F
    CMP R5, #9
    BHI.N 1f
    TST.W R11, #0x10               @ AF
    BEQ.N 2f
1:  ADD.W R3, R3, #6
    ORR.W R11, R11, #0x10          @ AF = 1
    B.N 3f
2:  BIC.W R11, R11, #0x10          @ AF = 0
3:  CMP R4, #0x99
    BHI.N 4f
    CBZ R1, 5f
4:  ADD.W R3, R3, #0x60
    MOV.W R0, #1
    B.N 6f
5:  MOV.W R0, #0
6:  BFI R11, R0, #0, #1            @ CF
    UXTB R3, R3
    STRB.W R3, [R8, #REG_EAX]
    B.N L(BCD_RESULT_FLAGS)

OP_AGNOSTIC .op_das_2F
    LDRB.W R3, [R8, #REG_EAX]
    MOV.W R4, R3                   @ old AL
    UBFX R1, R11, #0, #1           @ old CF
    MOV.W R0, #0                   @ new CF starts clear
    AND.W R5, R3, #0x0F
    CMP R5, #9
    BHI.N 1f
    TST.W R11, #0x10
    BEQ.N 2f
1:  CMP R3, #6
    IT LO
    MOVLO R0, #1                   @ borrow out of AL - 6
    ORR.W R0, R0, R1               @ CF = old_CF OR that borrow
    SUB.W R3, R3, #6
    ORR.W R11, R11, #0x10          @ AF = 1
    B.N 3f
2:  BIC.W R11, R11, #0x10          @ AF = 0
3:  CMP R4, #0x99
    BHI.N 4f
    CBZ R1, 5f
4:  SUB.W R3, R3, #0x60
    MOV.W R0, #1
5:  BFI R11, R0, #0, #1
    UXTB R3, R3
    STRB.W R3, [R8, #REG_EAX]
L(BCD_RESULT_FLAGS):
    LSLS.W R5, R3, #24             @ N/Z for the byte
    MRS R1, APSR
    X86_SF_ZF R1, R5
    X86_PF R3, R5
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_aaa_37
    LDRH.W R3, [R8, #REG_EAX]      @ AX
    AND.W R5, R3, #0x0F
    CMP R5, #9
    BHI.N 1f
    TST.W R11, #0x10
    BEQ.N 2f
1:  ADD.W R3, R3, #0x106           @ AL += 6, AH += 1
    ORR.W R11, R11, #0x11          @ AF and CF
    B.N 3f
2:  BIC.W R11, R11, #0x11
3:  BIC.W R3, R3, #0xF0            @ AL &= 0x0F, AH untouched
    STRH.W R3, [R8, #REG_EAX]
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_aas_3F
    LDRH.W R3, [R8, #REG_EAX]
    AND.W R5, R3, #0x0F
    CMP R5, #9
    BHI.N 1f
    TST.W R11, #0x10
    BEQ.N 2f
1:  SUB.W R3, R3, #6               @ AX -= 6, then AH -= 1
    SUB.W R3, R3, #0x100
    ORR.W R11, R11, #0x11
    B.N 3f
2:  BIC.W R11, R11, #0x11
3:  BIC.W R3, R3, #0xF0
    UXTH R3, R3
    STRH.W R3, [R8, #REG_EAX]
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_cld_FC
    BIC.W R11, R11, #1024
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_std_FD
    ORR.W R11, R11, #1024
    B.W .fetch_decode_loop
    /* R3 = dest value / result, R5 = src, R2 = dest pointer.
       R0/R1/R4 are dead here and used as scratch. */
/* v-width ALU cores. One source, two passes: NSH is 0 for the 32-bit pass and
   16 for the 16-bit pass. Operands are normalized to the TOP of the word, so
   the host N/Z/C/V ARE the x86 SF/ZF/CF/OF for this width with no extra work
   -- that is the whole reason the collapse is worth doing. At NSH=0 the shifts
   vanish and the 32-bit code is byte-identical to what it was. */
L(ALU_ADD):
    VNORM(R3)
    VNORM(R5)
    EOR.W R4, R3, R5               @ AF aux: a ^ b
    ADDS R3, R3, R5
    EOR.W R4, R4, R3               @ a ^ b ^ result
    VDENORM(R3)
    X86_FLAGS_ARITH R3, R4, R0, R1, 0, NSH
    B.N L(WRITEBACK_REG)
L(ALU_OR):
    VNORM(R3)
    VNORM(R5)
    ORRS.W R3, R3, R5
    VDENORM(R3)
    X86_FLAGS_LOGIC R3, R0, R1
    B.N L(WRITEBACK_REG)
L(ALU_ADC):
#if NSH
    X86_ADC_NARROW NSH, 0
#else
    EOR.W R4, R3, R5
    LSRS R0, R11, #1               @ EFLAGS bit0 (CF) -> host carry
    ADCS R3, R3, R5
    EOR.W R4, R4, R3
    X86_FLAGS_ARITH R3, R4, R0, R1, 0, 0
#endif
    B.N L(WRITEBACK_REG)
L(ALU_SBB):
#if NSH
    X86_ADC_NARROW NSH, 1
#else
    EOR.W R4, R3, R5
    EOR.W R0, R11, #1              @ ARM SBC subtracts NOT(C), so feed it !CF
    LSRS R0, R0, #1                @ ...shifted into the host carry
    SBCS R3, R3, R5
    EOR.W R4, R4, R3
    X86_FLAGS_ARITH R3, R4, R0, R1, 1, 0
#endif
    B.N L(WRITEBACK_REG)
L(ALU_AND):
    VNORM(R3)
    VNORM(R5)
    ANDS.W R3, R3, R5
    VDENORM(R3)
    X86_FLAGS_LOGIC R3, R0, R1
    B.N L(WRITEBACK_REG)
L(ALU_SUB):
    VNORM(R3)
    VNORM(R5)
    EOR.W R4, R3, R5
    SUBS R3, R3, R5
    EOR.W R4, R4, R3
    VDENORM(R3)
    X86_FLAGS_ARITH R3, R4, R0, R1, 1, NSH
    B.N L(WRITEBACK_REG)
L(ALU_XOR):
    VNORM(R3)
    VNORM(R5)
    EORS.W R3, R3, R5
    VDENORM(R3)
    X86_FLAGS_LOGIC R3, R0, R1
    B.N L(WRITEBACK_REG)
L(ALU_CMP):
    VNORM(R3)
    VNORM(R5)
    EOR.W R4, R3, R5
    SUBS R3, R3, R5
    EOR.W R4, R4, R3
    VDENORM(R3)
    X86_FLAGS_ARITH R3, R4, R0, R1, 1, NSH
    B.W .fetch_decode_loop         @ CMP discards the result
L(WRITEBACK_REG):
    VGA_ST_V R3, R2, R12
    B.W .fetch_decode_loop
    /* 8-bit cores. Same normalization with sh=24. */
L(ALU8_ADD):
    LSL.W R3, R3, #24
    LSL.W R5, R5, #24
    EOR.W R4, R3, R5
    ADDS R3, R3, R5
    EOR.W R4, R4, R3
    LSR.W R3, R3, #24
    X86_FLAGS_ARITH R3, R4, R0, R1, 0, 24
    B.N L(WRITEBACK_REG8)
L(ALU8_OR):
    ORR.W R3, R3, R5
    LSLS.W R0, R3, #24             @ set N/Z for the byte width
    X86_FLAGS_LOGIC R3, R0, R1
    B.N L(WRITEBACK_REG8)
L(ALU8_ADC):
    X86_ADC_NARROW 24, 0
    B.N L(WRITEBACK_REG8)
L(ALU8_SBB):
    X86_ADC_NARROW 24, 1
    B.N L(WRITEBACK_REG8)
L(ALU8_AND):
    AND.W R3, R3, R5
    LSLS.W R0, R3, #24
    X86_FLAGS_LOGIC R3, R0, R1
    B.N L(WRITEBACK_REG8)
L(ALU8_SUB):
    LSL.W R3, R3, #24
    LSL.W R5, R5, #24
    EOR.W R4, R3, R5
    SUBS R3, R3, R5
    EOR.W R4, R4, R3
    LSR.W R3, R3, #24
    X86_FLAGS_ARITH R3, R4, R0, R1, 1, 24
    B.N L(WRITEBACK_REG8)
L(ALU8_XOR):
    EOR.W R3, R3, R5
    LSLS.W R0, R3, #24
    X86_FLAGS_LOGIC R3, R0, R1
    B.N L(WRITEBACK_REG8)
L(ALU8_CMP):
    LSL.W R3, R3, #24
    LSL.W R5, R5, #24
    EOR.W R4, R3, R5
    SUBS R3, R3, R5
    EOR.W R4, R4, R3
    LSR.W R3, R3, #24
    X86_FLAGS_ARITH R3, R4, R0, R1, 1, 24
    B.W .fetch_decode_loop         @ CMP discards the result
L(WRITEBACK_REG8):
    VGA_ST_B R3, R2, R12
    B.W .fetch_decode_loop
#ifndef USE_16_BIT
    .global decode_modrm_addr32
decode_modrm_addr32:
L(DECODE_MODRM_32):
    /* R12 = offset of the DEFAULT segment descriptor for this addressing form.
       DS unless the base register is (E)BP or (E)SP, which default to SS. An
       explicit override prefix has already put its own descriptor in R6 and
       wins over both -- see L(MODRM_SEG). */
    /* `mod` is NOT extracted. The ModRM byte is a byte, so mod==0 is
       `modrm < 0x40`, mod==1 is `< 0x80` and mod==3 is `>= 0xC0` -- the AND
       that used to build it in R2 is dead weight, and comparing R1 directly
       encodes as 16-bit CMP instead of 32-bit AND.W + CMP. R1 survives the
       decoder by contract, so every later mod test can use it too.
       `rm` is kept UNSCALED in R3 so the BP test is `CMP R3,#5` and the base
       load folds the scale into the addressing mode. */
    MOV.W R12, #SEG_DS_SEL
    AND.W R3, R1, #7
    CMP R3, #4
    BEQ.N L(MODRM_SIB)
    ADD.W R4, R8, R3, LSL #2
    LDR R7, [R4, #8]
    CMP R1, #64
    BLO.N L(MODRM_MOD0)
    CMP R3, #5                     @ rm==5 -> [(E)BP+disp], default SS
    IT EQ
    MOVEQ R12, #SEG_SS_SEL
    CMP R1, #128
    BLO.N L(MODRM_MOD1)
    CODE_PTR R5
    LDR R5, [R5, #0]
    ADD.W R10, R10, #4
    ADD R7, R5
    B.N L(MODRM_SEG)
L(MODRM_MOD1):
    /* The rm==5 -> SS default is hoisted above the mod1/mod2 split: both arms
       need it and neither mod0 nor the SIB path does. */
    CODE_PTR R5
    LDRSB.W R5, [R5]
    ADD.W R10, R10, #1
    ADD R7, R5
    B.N L(MODRM_SEG)
L(MODRM_MOD0):
    CMP R3, #5                     @ rm still unscaled in R3
    BNE.N L(MODRM_SEG)
    CODE_PTR R7
    LDR R7, [R7, #0]
    ADD.W R10, R10, #4
    B.N L(MODRM_SEG)
L(MODRM_SIB):
    CODE_PTR R3
    LDRB R3, [R3, #0]
    ADD.W R10, R10, #1
    AND.W R4, R3, #7
    CMP R4, #5
    BNE.N L(SIB_HAS_BASE)
    CMP R1, #64                    @ mod==0 -> no base, disp32 instead
    BLO.N L(SIB_BASE_DISP32)
L(SIB_HAS_BASE):
    SUB R5, R4, #4                 @ 0 for ESP, 1 for EBP
    CMP R5, #2
    IT LO
    MOVLO R12, #SEG_SS_SEL         @ stack-relative base -> default SS
    ADD.W R4, R8, R4, LSL #2
    LDR R7, [R4, #8]
    B.N L(SIB_DO_INDEX)
L(SIB_BASE_DISP32):
    CODE_PTR R7
    LDR R7, [R7, #0]
    ADD.W R10, R10, #4
L(SIB_DO_INDEX):
    UBFX R5, R3, #3, #3
    CMP R5, #4
    BEQ.N L(SIB_ADD_DISP)
    ADD.W R5, R8, R5, LSL #2
    LDR R5, [R5, #8]
    UBFX R4, R3, #6, #2
    LSL.W R5, R5, R4
    ADD R7, R5
L(SIB_ADD_DISP):
    CMP R1, #64                    @ mod straight off the ModRM byte
    BLO.N L(MODRM_SEG)             @ mod==0: no displacement
    CMP R1, #128
    BLO.N L(SIB_DISP8)
    B.N L(SIB_DISP32)
L(SIB_DISP8):
    CODE_PTR R5
    LDRSB.W R5, [R5]
    ADD.W R10, R10, #1
    ADD R7, R5
    B.N L(MODRM_SEG)
L(SIB_DISP32):
    CODE_PTR R5
    LDR R5, [R5, #0]
    ADD.W R10, R10, #4
    ADD R7, R5
    B.N L(MODRM_SEG)


#else
/* ---------------------------------------------------------------------------
 * 16-bit ModRM. A DIFFERENT GRAMMAR, not a narrower one -- this is why the old
 * MEM_READ_SZ/STACK_INC scheme could never have worked even if it had been
 * wired up. There is no SIB byte, disp is 8 or 16 bits, and rm selects one of
 * eight fixed base+index pairs:
 *
 *   0 [BX+SI]  1 [BX+DI]  2 [BP+SI]  3 [BP+DI]  4 [SI]  5 [DI]  6 [BP]  7 [BX]
 *
 * mod=00 rm=6 is the disp16 special case (no BP). Any form whose base is BP
 * defaults to SS -- rm 2, 3 and 6 -- which is the same rule as 32-bit, applied
 * to a different encoding. The computed offset wraps at 16 bits BEFORE the
 * segment base is added; a real 8086 cannot address past 0xFFFF within a
 * segment and DOS code relies on that wrap.
 * ------------------------------------------------------------------------ */
    .global decode_modrm_addr16
decode_modrm_addr16:
L(DECODE_MODRM_32):
    /* mod is read straight off the ModRM byte: it is a byte, so mod==0 is
       `< 0x40` and mod==1 is `< 0x80`. See the 32-bit decoder. */
    MOV.W R12, #SEG_DS_SEL
    AND.W R4, R1, #7               @ rm

    CMP R4, #6
    BNE.N L(M16_NOT_RM6)
    CMP R1, #64
    BHS.N L(M16_RM6_BP)
    /* mod=00 rm=110: disp16, no base register at all, segment stays DS. */
    CODE_PTR R7
    LDRH R7, [R7, #0]
    ADD.W R10, R10, #2
    B.N L(MODRM_SEG)
L(M16_RM6_BP):
    LDRH.W R7, [R8, #REG_EBP]      @ [BP+disp]
    MOV.W R12, #SEG_SS_SEL
    B.N L(M16_DISP)

L(M16_NOT_RM6):
    /* rm 0..5,7. Base: BX for 0,1,7; BP for 2,3 (-> SS); none for 4,5.
       Index: SI for 0,2,4; DI for 1,3,5; none for 6,7. */
    MOV.W R7, #0
    CMP R4, #4
    BLO.N L(M16_PAIR)              @ 0..3 have both a base and an index
    CMP R4, #7
    BEQ.N L(M16_BX_ONLY)
    /* rm 4 = [SI], rm 5 = [DI] */
    SUB.W R5, R4, #4
    ADD.W R5, R8, R5, LSL #2
    LDRH R7, [R5, #REG_ESI]
    B.N L(M16_DISP)
L(M16_BX_ONLY):
    LDRH.W R7, [R8, #REG_EBX]
    B.N L(M16_DISP)
L(M16_PAIR):
    TST R4, #2                     @ rm 2,3 are BP-based -> SS
    BEQ.N L(M16_BASE_BX)
    LDRH.W R7, [R8, #REG_EBP]
    MOV.W R12, #SEG_SS_SEL
    B.N L(M16_INDEX)
L(M16_BASE_BX):
    LDRH.W R7, [R8, #REG_EBX]
L(M16_INDEX):
    /* Odd rm uses DI, even SI, and REG_EDI is REG_ESI+4 -- so the choice is
       address arithmetic, not an IT block. docs/02 s4: IT serializes on the M7
       and this one gated a load. */
    AND.W R5, R4, #1
    ADD.W R5, R8, R5, LSL #2
    LDRH R5, [R5, #REG_ESI]
    ADD R7, R7, R5

L(M16_DISP):
    CMP R1, #64
    BLO.N L(M16_WRAP)
    CMP R1, #128
    BHS.N L(M16_DISP16)
    CODE_PTR R5
    LDRSB.W R5, [R5]
    ADD.W R10, R10, #1
    ADD R7, R7, R5
    B.N L(M16_WRAP)
L(M16_DISP16):
    CODE_PTR R5
    LDRH R5, [R5, #0]
    ADD.W R10, R10, #2
    ADD R7, R7, R5
L(M16_WRAP):
    UXTH R7, R7                    @ offset wraps inside the segment
    B.N L(MODRM_SEG)
#endif
/* Effective address -> linear address. Two instructions in the common case;
   the descriptor pointer was chosen over a base value precisely so this stays
   a single load. */
L(MODRM_SEG):
    SUBS R5, R6, R8                @ 0 iff no override prefix
    IT EQ
    ADDEQ R6, R8, R12              @ take this form's default (DS, or SS for BP/SP)
    LDR.W R5, [R6, #4]             @ descriptor base (SEL 0, FLAGS 2, BASE 4)
    ADD R7, R7, R5
    BX LR

OP .op_call_rel32_E8
    CODE_PTR R5
    LDS_V R5, R5                   @ displacement is SIGNED
    EIP_ADV_V
    /* Push IP, not the linear EIP: R10 carries CS.base folded in, and the
       stack must hold the segment-relative offset or the matching RET lands
       CS.base too far along. */
    LDR.W R3, [R8, #SEG_CS_BASE]
    SUB.W R4, R10, R3
    PUSH_V R4, R0, R12
    ADD R10, R10, R5
    EIP_WRAP
    B.W .fetch_decode_loop

OP .op_jmp_rel32_E9
    CODE_PTR R5
    LDS_V R5, R5                   @ displacement is SIGNED
    EIP_ADV_V
    ADD R10, R10, R5
    EIP_WRAP
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_jmp_rel8_EB
    CODE_PTR R5
    LDRSB.W R5, [R5]
    ADD.W R10, R10, #1
    ADD R10, R5
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_grp4_FE
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(GRP4_MEM)
    AND.W R2, R1, #7
    AND.W R3, R2, #3
    ADD.W R3, R8, R3, LSL #2
    ADD.W R2, R3, R2, LSR #2
    ADD.W R2, R2, #8
    LDRB R3, [R2, #0]
    B.N L(GRP4_DISPATCH)
L(GRP4_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LDRB R3, [R2, #0]
L(GRP4_DISPATCH):
    UBFX R12, R1, #3, #3
    CMP R12, #0
    BEQ.N L(GRP4_INC)
    CMP R12, #1
    BEQ.N L(GRP4_DEC)
    B.W .exit_loop
/* INC/DEC r/m8 set SF, ZF, AF, PF and OF and leave CF alone. Both arms used to
   set NO flag at all, which is silent until a program branches on the result --
   `DEC DH ; JNE` then reads a stale ZF and never leaves its loop. The operand
   is normalized to the top of the word so the host N/Z/V are already the x86
   SF/ZF/OF for a byte, and the AF aux is the true a^b^result rather than ~a. */
L(GRP4_INC):
    LSL.W R3, R3, #24
    EOR.W R4, R3, #(1 << 24)       @ a ^ b, with b == 1
    ADDS R3, R3, #(1 << 24)
    EOR.W R4, R4, R3               @ a ^ b ^ result, for AF
    LSR.W R3, R3, #24
    X86_FLAGS_INCDEC R3, R4, R0, R1, 24
    B.N L(GRP4_STORE)
L(GRP4_DEC):
    LSL.W R3, R3, #24
    EOR.W R4, R3, #(1 << 24)
    SUBS R3, R3, #(1 << 24)
    EOR.W R4, R4, R3
    LSR.W R3, R3, #24
    X86_FLAGS_INCDEC R3, R4, R0, R1, 24
L(GRP4_STORE):
    VGA_ST_B R3, R2, R12
    B.W .fetch_decode_loop

OP .op_grp5_FF
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(GRP5_MEM)
    /* Register form. /3 (CALL FAR) and /5 (JMP FAR) have NO register
       encoding: their operand is a POINTER IN MEMORY, so mod=11 is #UD.
       Falling through into the far handlers instead let them read the far
       pointer through R7 -- the effective address left behind by whichever
       EARLIER instruction last decoded a ModRM -- and then install that
       garbage as CS:IP. That is the interpreter taking its next fetch address
       from an arbitrary place in guest memory, which is how a run ends up
       executing data. R12 is scratch and is recomputed at GRP5_DISPATCH. */
    UBFX R12, R1, #3, #3
    CMP R12, #3
    BEQ.W .exit_loop
    CMP R12, #5
    BEQ.W .exit_loop
    AND.W R2, R1, #7
    ADD.W R2, R8, R2, LSL #2
    ADD.W R2, R2, #8
    LD_V R3, R2
    B.N L(GRP5_DISPATCH)
L(GRP5_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LD_V R3, R2
L(GRP5_DISPATCH):
    UBFX R12, R1, #3, #3
    CMP R12, #0
    BEQ.N L(GRP5_INC)
    CMP R12, #1
    BEQ.N L(GRP5_DEC)
    CMP R12, #2
    BEQ.N L(GRP5_CALL)
    CMP R12, #3
    BEQ.W L(GRP5_CALLF)
    CMP R12, #4
    BEQ.N L(GRP5_JMP)
    CMP R12, #5
    BEQ.W L(GRP5_JMPF)
    CMP R12, #6
    BEQ.N L(GRP5_PUSH)
    B.W .exit_loop
/* INC/DEC Ev set the same five flags their r/m8 twins do, and set none
   before. CALL/JMP near indirect take an OFFSET in CS, not a linear address,
   and the return address pushed is likewise an offset -- R10 carries CS.base
   folded in, so both directions need the base added or removed. PUSH Ev goes
   through SS like every other push. */
L(GRP5_INC):
    VNORM(R3)
    EOR.W R4, R3, #(1 << NSH)      @ a ^ b, with b == 1
    ADDS R3, R3, #(1 << NSH)
    EOR.W R4, R4, R3
    VDENORM(R3)
    X86_FLAGS_INCDEC R3, R4, R0, R1, NSH
    VGA_ST_V R3, R2, R12
    B.W .fetch_decode_loop
L(GRP5_DEC):
    VNORM(R3)
    EOR.W R4, R3, #(1 << NSH)
    SUBS R3, R3, #(1 << NSH)
    EOR.W R4, R4, R3
    VDENORM(R3)
    X86_FLAGS_INCDEC R3, R4, R0, R1, NSH
    VGA_ST_V R3, R2, R12
    B.W .fetch_decode_loop
L(GRP5_CALL):
    LDR.W R4, [R8, #SEG_CS_BASE]
    SUB.W R5, R10, R4              @ return address, as an offset
    ADD.W R10, R3, R4
    PUSH_V R5, R0, R12
    B.W .fetch_decode_loop
L(GRP5_JMP):
    LDR.W R4, [R8, #SEG_CS_BASE]
    ADD.W R10, R3, R4
    B.W .fetch_decode_loop
L(GRP5_PUSH):
    PUSH_V R3, R4, R12
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_jcc_rel8_7X
    CODE_PTR R5
    LDRSB.W R5, [R5]
    ADD.W R10, R10, #1
    AND.W R2, R0, #15
    BL L(EVAL_COND)
    CMP R3, #1
    BNE.W .fetch_decode_loop
    ADD R10, R5
    B.W .fetch_decode_loop
L(EVAL_COND):
    MOV.W R3, #0
    CMP R2, #0
    BNE.N L(COND_1)
    UBFX R0, R11, #11, #1
    MOV R3, R0
    BX LR
L(COND_1):
    CMP R2, #1
    BNE.N L(COND_2)
    UBFX R0, R11, #11, #1
    EOR.W R3, R0, #1
    BX LR
L(COND_2):
    CMP R2, #2
    BNE.N L(COND_3)
    UBFX R3, R11, #0, #1
    BX LR
L(COND_3):
    CMP R2, #3
    BNE.N L(COND_4)
    UBFX R0, R11, #0, #1
    EOR.W R3, R0, #1
    BX LR
L(COND_4):
    CMP R2, #4
    BNE.N L(COND_5)
    UBFX R3, R11, #6, #1
    BX LR
L(COND_5):
    CMP R2, #5
    BNE.N L(COND_6)
    UBFX R0, R11, #6, #1
    EOR.W R3, R0, #1
    BX LR
L(COND_6):
    CMP R2, #6
    BNE.N L(COND_7)
    UBFX R3, R11, #0, #1
    UBFX R0, R11, #6, #1
    ORR.W R3, R3, R0
    BX LR
L(COND_7):
    CMP R2, #7
    BNE.N L(COND_8)
    UBFX R3, R11, #0, #1
    UBFX R0, R11, #6, #1
    ORR.W R3, R3, R0
    EOR.W R3, R3, #1
    BX LR
L(COND_8):
    CMP R2, #8
    BNE.N L(COND_9)
    UBFX R3, R11, #7, #1
    BX LR
L(COND_9):
    CMP R2, #9
    BNE.N L(COND_A)
    UBFX R0, R11, #7, #1
    EOR.W R3, R0, #1
    BX LR
L(COND_A):
    CMP R2, #10
    BNE.N L(COND_B)
    UBFX R3, R11, #2, #1
    BX LR
L(COND_B):
    CMP R2, #11
    BNE.N L(COND_C)
    UBFX R0, R11, #2, #1
    EOR.W R3, R0, #1
    BX LR
L(COND_C):
    CMP R2, #12
    BNE.N L(COND_D)
    UBFX R3, R11, #7, #1
    UBFX R0, R11, #11, #1
    EOR.W R3, R3, R0
    BX LR
L(COND_D):
    CMP R2, #13
    BNE.N L(COND_E)
    UBFX R3, R11, #7, #1
    UBFX R0, R11, #11, #1
    EOR.W R3, R3, R0
    EOR.W R3, R3, #1
    BX LR
L(COND_E):
    CMP R2, #14
    BNE.N L(COND_F)
    UBFX R3, R11, #7, #1
    UBFX R0, R11, #11, #1
    EOR.W R3, R3, R0
    UBFX R0, R11, #6, #1
    ORR.W R3, R3, R0
    BX LR
L(COND_F):
    UBFX R3, R11, #7, #1
    UBFX R0, R11, #11, #1
    EOR.W R3, R3, R0
    UBFX R0, R11, #6, #1
    ORR.W R3, R3, R0
    EOR.W R3, R3, #1
    BX LR

OP .op_pusha_60
    /* Eight pushes through SS at the operand size. This was open-coded with a
       hard-wired 4-byte slot and ESP used as a linear address, so PUSHA in a
       16-bit real-mode program wrote 32 bytes at the wrong place; it also
       pushed only three of the eight registers. The SP that gets pushed is the
       one from BEFORE the sequence, which is what POPA and a debugger expect. */
    LDR.W R5, [R8, #REG_ESP]
    LDR.W R3, [R8, #REG_EAX]
    PUSH_V R3, R4, R12
    LDR.W R3, [R8, #REG_ECX]
    PUSH_V R3, R4, R12
    LDR.W R3, [R8, #REG_EDX]
    PUSH_V R3, R4, R12
    LDR.W R3, [R8, #REG_EBX]
    PUSH_V R3, R4, R12
    PUSH_V R5, R4, R12             @ the ORIGINAL SP
    LDR.W R3, [R8, #REG_EBP]
    PUSH_V R3, R4, R12
    LDR.W R3, [R8, #REG_ESI]
    PUSH_V R3, R4, R12
    LDR.W R3, [R8, #REG_EDI]
    PUSH_V R3, R4, R12
    B.W .fetch_decode_loop

OP .op_popa_61
    /* The mirror image, and the stored SP is DISCARDED rather than loaded --
       loading it would undo the pops still to come. */
    POP_V R3, R4, R12
    STREG_V R3, #REG_EDI
    POP_V R3, R4, R12
    STREG_V R3, #REG_ESI
    POP_V R3, R4, R12
    STREG_V R3, #REG_EBP
    LDR.W R4, [R8, #REG_ESP]       @ step over the saved SP without using it
    SP_INC R4
    SP_WRAP R4
    STR.W R4, [R8, #REG_ESP]
    POP_V R3, R4, R12
    STREG_V R3, #REG_EBX
    POP_V R3, R4, R12
    STREG_V R3, #REG_EDX
    POP_V R3, R4, R12
    STREG_V R3, #REG_ECX
    POP_V R3, R4, R12
    STREG_V R3, #REG_EAX
    B.W .fetch_decode_loop

OP .op_push_imm32_68
    CODE_PTR R5
    LD_V R5, R5
    EIP_ADV_V
    PUSH_V R5, R4, R12
    B.W .fetch_decode_loop

OP .op_imul_imm32_69
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(IMUL69_MEM)
    AND.W R2, R1, #7
    ADD.W R2, R8, R2, LSL #2
    ADD.W R2, R2, #8
    LD_V R3, R2
    B.N L(IMUL69_DO)
L(IMUL69_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LD_V R3, R2
L(IMUL69_DO):
    CODE_PTR R5
    LD_V R5, R5
    EIP_ADV_V
    MUL.W R3, R3, R5
    UBFX R4, R1, #3, #3
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    STREG_V R3, R4
    B.W .fetch_decode_loop

OP .op_push_imm8_6A
    CODE_PTR R5
    LDRSB.W R5, [R5]
    ADD.W R10, R10, #1
    PUSH_V R5, R4, R12
    B.W .fetch_decode_loop

OP .op_imul_imm8_6B
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(IMUL6B_MEM)
    AND.W R2, R1, #7
    ADD.W R2, R8, R2, LSL #2
    ADD.W R2, R2, #8
    LD_V R3, R2
    B.N L(IMUL6B_DO)
L(IMUL6B_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LD_V R3, R2
L(IMUL6B_DO):
    CODE_PTR R5
    LDRSB.W R5, [R5]
    ADD.W R10, R10, #1
    MUL.W R3, R3, R5
    UBFX R4, R1, #3, #3
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    STREG_V R3, R4
    B.W .fetch_decode_loop

OP .op_enter_C8
    CODE_PTR R5
    LDRH R5, [R5, #0]
    ADD.W R10, R10, #2
    /* ENTER's nesting-level byte. The value is discarded (level nesting is not
       implemented), but the fetch stays: it advances EIP and it faults on an
       unmapped page, which is what the real instruction does. */
    CODE_PTR R12
    LDRB R12, [R12, #0]
    ADD.W R10, R10, #1
    LDR.W R3, [R8, #REG_EBP]
    PUSH_V R3, R4, R12
    LDR.W R3, [R8, #REG_ESP]
    STREG_V R3, #REG_EBP
    SUB.W R3, R3, R5
    SP_WRAP R3
    STR.W R3, [R8, #REG_ESP]
    B.W .fetch_decode_loop

OP .op_retf_CB
    /* RETF popped CS and threw it away, so a far return kept executing in the
       caller's segment. Pop IP then CS, install CS properly (which recomputes
       the base), and rebuild the linear EIP from it. */
    POP_V R4, R3, R12              @ IP
    POP_V R5, R3, R12              @ CS selector
    UXTH R5, R5
    PUSH {R4, LR}
    MOV.W R4, #1                   @ CS
    BL L(SET_SREG)
    POP {R4, LR}
    LDR.W R3, [R8, #SEG_CS_BASE]
    ADD.W R10, R4, R3
    B.W .fetch_decode_loop

OP .op_retf_imm16_CA
    /* RETF imm16. It popped CS and discarded it -- the same defect CB had --
       and used raw ESP as a linear address with a 4-byte slot. Pop IP then CS,
       install CS so the base is recomputed, rebuild the linear EIP from it,
       and only then drop the caller's arguments. */
    CODE_PTR R5
    LDRH R5, [R5, #0]              @ bytes to drop; always 16 bits
    PUSH {R5, LR}
    POP_V R4, R3, R12              @ IP
    POP_V R5, R3, R12              @ CS selector
    UXTH R5, R5
    PUSH {R4, R6}
    MOV.W R4, #1                   @ CS
    BL L(SET_SREG)
    POP {R4, R6}
    LDR.W R3, [R8, #SEG_CS_BASE]
    ADD.W R10, R4, R3
    POP {R5, LR}
    LDR.W R4, [R8, #REG_ESP]
    ADD R4, R5
    SP_WRAP R4
    STR.W R4, [R8, #REG_ESP]
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_aam_D4
    CODE_PTR R5
    LDRB R5, [R5, #0]
    ADD.W R10, R10, #1
    CMP R5, #0
    BEQ.W .exit_loop
    LDRB.W R3, [R8, #8]
    UDIV R4, R3, R5
    MLS R3, R4, R5, R3
    STRB.W R3, [R8, #8]
    STRB.W R4, [R8, #9]
    /* SF, ZF and PF are DEFINED for AAM and AAD and come from AL. Setting no
       flag at all left whatever the previous instruction wrote, so a guest's
       `aam / jz` branched on a stale ZF. CF, OF and AF are undefined and are
       left alone. */
    LSLS.W R0, R3, #24
    MRS  R0, APSR
    X86_SF_ZF R0, R1
    X86_PF R3, R1
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_aad_D5
    CODE_PTR R5
    LDRB R5, [R5, #0]
    ADD.W R10, R10, #1
    LDRB.W R3, [R8, #8]
    LDRB.W R4, [R8, #9]
    MUL.W R4, R4, R5
    ADD R3, R4
    AND.W R3, R3, #255
    STRB.W R3, [R8, #8]
    MOV.W R4, #0
    STRB.W R4, [R8, #9]
    LSLS.W R0, R3, #24             @ SF/ZF/PF from AL, as for AAM above
    MRS  R0, APSR
    X86_SF_ZF R0, R1
    X86_PF R3, R1
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_salc_D6
    UBFX R3, R11, #0, #1
    CMP R3, #1
    ITE EQ
    MOVEQ R4, #255
    MOVNE R4, #0
    STRB.W R4, [R8, #8]
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_xlat_D7
    LDR.W R3, [R8, #20]
    LDRB.W R4, [R8, #8]
    ADD R3, R4
    UBFX R4, R3, #12, #12
    LDR.W R4, [R9, R4, LSL #2]
    ADDS R4, R4, R3
    MEM_TRAP_R R4
    LDRB R4, [R4, #0]
    STRB.W R4, [R8, #8]
    B.W .fetch_decode_loop

/* D8-DF, the x87 ESCAPE group, ON A MACHINE WITH NO COPROCESSOR.
 *
 * This is NOT an x87 implementation and must not be mistaken for one. It is the
 * OTHER honest answer: a 386-class PC with no 387 fitted and CR0.EM/MP clear
 * ignores every ESC opcode outright -- the bus cycle goes to a socket nobody
 * populated -- and that is precisely what makes the standard C-runtime
 * detection conclude "no coprocessor":
 *
 *     6A FF      PUSH -1            ; [BP-2] = 0xFFFF
 *     0F 01 E0   SMSW AX
 *     A8 04      TEST AL,4          ; CR0.EM?
 *     75 19      JNZ  no_fpu
 *     DB E3      FNINIT             ; a real 387 would zero the status word
 *     DD 7E FE   FNSTSW [BP-2]      ; ...here
 *     38 4E FE   CMP  [BP-2],CL     ; still 0xFFFF -> no coprocessor
 *
 * Reaching .exit_loop instead KILLED THE GUEST on the third line of its own
 * startup, which is what MORTAL_KOMBAT did (docs/13): every title built with a
 * Microsoft or Borland runtime runs this sequence whether or not it ever uses a
 * float. `x86_x87_escapes` is the number that says which: a detection costs a
 * HANDFUL, and a title that actually computes in floating point runs millions
 * and is silently producing wrong arithmetic. READ IT before trusting a picture
 * from a title that is new to this tree.
 *
 * The only real work is advancing EIP correctly. mod!=11 has a full ModRM/SIB/
 * displacement to step over, so the address-size decoder from the mode record
 * runs (its result is then discarded); mod=11 is the whole instruction and must
 * NOT be walked -- L(OP_SLDT) avoids the same call for the same reason. */
OP_AGNOSTIC .op_esc_x87
    MOVW R3, #:lower16:x86_x87_escapes
    MOVT R3, #:upper16:x86_x87_escapes
    LDR R4, [R3]
    ADD R4, R4, #1
    STR R4, [R3]
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192
    BLO.N 1f
    B.W .fetch_decode_loop
1:  LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_loopnz_E0
    CODE_PTR R5
    LDRSB.W R5, [R5]
    ADD.W R10, R10, #1
    CNT_LOAD R3
    SUB.W R3, R3, #1
    TST.W R2, #ADDRSIZE_XOR
    IT EQ
    UXTHEQ R3, R3
    CNT_STORE R3
    CMP R3, #0
    BEQ.W .fetch_decode_loop
    UBFX R4, R11, #6, #1
    CMP R4, #0
    BEQ.N L(LOOP_TAKEN)
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_loopz_E1
    CODE_PTR R5
    LDRSB.W R5, [R5]
    ADD.W R10, R10, #1
    CNT_LOAD R3
    SUB.W R3, R3, #1
    TST.W R2, #ADDRSIZE_XOR
    IT EQ
    UXTHEQ R3, R3
    CNT_STORE R3
    CMP R3, #0
    BEQ.W .fetch_decode_loop
    UBFX R4, R11, #6, #1
    CMP R4, #1
    BEQ.N L(LOOP_TAKEN)
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_loop_E2
    CODE_PTR R5
    LDRSB.W R5, [R5]
    ADD.W R10, R10, #1
    CNT_LOAD R3
    SUB.W R3, R3, #1
    TST.W R2, #ADDRSIZE_XOR
    IT EQ
    UXTHEQ R3, R3
    CNT_STORE R3
    CMP R3, #0
    BNE.N L(LOOP_TAKEN)
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_jcxz_E3
    CODE_PTR R5
    LDRSB.W R5, [R5]
    ADD.W R10, R10, #1
    CNT_LOAD R3
    CMP R3, #0
    BEQ.N L(LOOP_TAKEN)
    B.W .fetch_decode_loop
L(LOOP_TAKEN):
    ADD R10, R5
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_lock_F0
    B.W .fetch_decode_loop

/* HLT is not a stop, it is "idle until the next interrupt" -- which is how a
   timing loop waits for the periodic tick. Reported with its own exit reason so
   the frame loop can raise that interrupt and let the guest continue, rather
   than latching it as a dead machine the way an unimplemented opcode is. EIP is
   already past the HLT, which is where the IRET from the handler belongs. */
OP_AGNOSTIC .op_hlt_F4
    LDR.W R2, [R8, #CPU_CR0]        // CR0
    TST.W R2, #1                // Check PE bit
    BEQ.N L(HLT_IDLE)           // Real mode: HLT allowed
    LDRH.W R2, [R8, #SEG_CS_SEL]  // CS.sel
    AND.W R2, R2, #3            // CPL = CS.sel[1:0]
    CMP R2, #0
    BNE.W .raise_gp_fault       // #GP if not ring 0
L(HLT_IDLE):
    MOV.W R2, #X86_EXIT_HALT
    STR.W R2, [R8, #CPU_EXIT_REASON]
    B.W .save_state_and_exit

OP_AGNOSTIC .op_cmc_F5
    EOR.W R11, R11, #1
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_clc_F8
    BIC.W R11, R11, #1
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_stc_F9
    ORR.W R11, R11, #1
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_cli_FA
    LDR.W R2, [R8, #CPU_CR0]
    TST.W R2, #1
    BEQ.N L(OP_CLI_FA_OK)
    LDRH.W R2, [R8, #SEG_CS_SEL]  // CS.sel
    AND.W R2, R2, #3
    UBFX R3, R11, #12, #2
    CMP R2, R3
    BHI.W .raise_gp_fault
L(OP_CLI_FA_OK):
    BIC.W R11, R11, #512
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_sti_FB
    LDR.W R2, [R8, #CPU_CR0]
    TST.W R2, #1
    BEQ.N L(OP_STI_FB_OK)
    LDRH.W R2, [R8, #SEG_CS_SEL]  // CS.sel
    AND.W R2, R2, #3
    UBFX R3, R11, #12, #2
    CMP R2, R3
    BHI.W .raise_gp_fault
L(OP_STI_FB_OK):
    ORR.W R11, R11, #512
    B.W .fetch_decode_loop

/* ===========================================================================
 * Port I/O.
 *
 * DX is regs[2] at REG_EDX. Every one of these handlers used to read
 * [R8, #24], which is regs[4] -- ESP. So `OUT DX, AL` sent the byte to
 * whatever port number the stack pointer happened to look like.
 *
 * Dispatch is a direct call into C. AAPCS makes R4-R11 callee-saved, so the
 * interpreter's R6/R8/R9/R10/R11 survive it; R2 (the mode record) does not,
 * but no port instruction takes a ModRM so it is already dead.
 * =========================================================================== */
OP_AGNOSTIC .op_in_al_dx_EC
    LDRH.W R0, [R8, #REG_EDX]
    B.N L(DO_IN_B)

OP_AGNOSTIC .op_in_al_imm8_E4
    CODE_PTR R0
    LDRB R0, [R0, #0]
    ADD.W R10, R10, #1
L(DO_IN_B):
    CHECK_IOPL R2, R3
    MOV.W R1, #1
    BL x86_port_read
    STRB.W R0, [R8, #REG_EAX]
    B.W .fetch_decode_loop

OP .op_in_eax_dx_ED
    LDRH.W R0, [R8, #REG_EDX]
    B.N L(DO_IN_V)

OP .op_in_eax_imm8_E5
    CODE_PTR R0
    LDRB R0, [R0, #0]
    ADD.W R10, R10, #1
L(DO_IN_V):
    CHECK_IOPL R2, R3
    MOV.W R1, #OPSZ
    BL x86_port_read
    STREG_V R0, #REG_EAX
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_out_dx_al_EE
    LDRH.W R0, [R8, #REG_EDX]
    B.N L(DO_OUT_B)

OP_AGNOSTIC .op_out_imm8_al_E6
    CODE_PTR R0
    LDRB R0, [R0, #0]
    ADD.W R10, R10, #1
L(DO_OUT_B):
    CHECK_IOPL R2, R3
    LDRB.W R1, [R8, #REG_EAX]
    MOV.W R2, #1
    BL x86_port_write
    B.W .fetch_decode_loop

OP .op_out_dx_eax_EF
    LDRH.W R0, [R8, #REG_EDX]
    B.N L(DO_OUT_V)

OP .op_out_imm8_eax_E7
    CODE_PTR R0
    LDRB R0, [R0, #0]
    ADD.W R10, R10, #1
L(DO_OUT_V):
    CHECK_IOPL R2, R3
    LDREG_V R1, #REG_EAX
    MOV.W R2, #OPSZ
    BL x86_port_write
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_sahf_9E
    LDRB.W R3, [R8, #9]
    BFI R11, R3, #0, #8
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_lahf_9F
    UBFX R3, R11, #0, #8
    STRB.W R3, [R8, #9]
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_grp3_eb_F6
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(GRP3_F6_MEM)
    AND.W R2, R1, #7
    AND.W R3, R2, #3
    ADD.W R3, R8, R3, LSL #2
    ADD.W R2, R3, R2, LSR #2
    ADD.W R2, R2, #8
    LDRB R3, [R2, #0]
    B.N L(GRP3_F6_DISPATCH)
L(GRP3_F6_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LDRB R3, [R2, #0]
L(GRP3_F6_DISPATCH):
    UBFX R12, R1, #3, #3
    CMP R12, #0
    BEQ.W L(GRP3_F6_TEST)
    CMP R12, #2
    BEQ.W L(GRP3_F6_NOT)
    CMP R12, #3
    BEQ.W L(GRP3_F6_NEG)
    CMP R12, #4
    BEQ.W L(GRP3_F6_MUL)
    CMP R12, #5
    BEQ.W L(GRP3_F6_IMUL)
    CMP R12, #6
    BEQ.W L(GRP3_F6_DIV)
    CMP R12, #7
    BEQ.W L(GRP3_F6_IDIV)
    B.W .exit_loop
/* TEST r/m8, imm8. It fetched the immediate and then set NO FLAGS -- the whole
   point of the instruction. Everything downstream ran on whatever the previous
   instruction had left behind, which is how Alley Cat's INT 09h handler
   (AND AL,7Fh / TEST AH,80h / JNZ) took the JNZ on every make code and never
   bumped the counter its wait loops poll: keys arrived, nothing happened.
   Normalized to bit 31 like the A8 byte form so SF/ZF describe the byte. */
L(GRP3_F6_TEST):
    CODE_PTR R5
    LDRB R5, [R5, #0]
    ADD.W R10, R10, #1
    AND.W R3, R3, R5               @ TEST sets flags and discards the result
    LSLS.W R0, R3, #24
    X86_FLAGS_LOGIC R3, R0, R1
    B.W .fetch_decode_loop
L(GRP3_F6_NOT):
    MVN.W R3, R3
    VGA_ST_B R3, R2, R12
    B.W .fetch_decode_loop
L(GRP3_F6_NEG):
    /* CF is "the operand was non-zero", which ARM gives as NOT-borrow, hence
       the inversion. Normalized to the top so SF/ZF/OF and the AF nibble all
       describe the byte rather than the register. */
    LSL.W R3, R3, #24
    MOV.W R4, R3                   @ AF aux: 0 ^ operand
    RSBS R3, R3, #0
    EOR.W R4, R4, R3
    LSR.W R3, R3, #24
    X86_FLAGS_ARITH R3, R4, R0, R1, 1, 24
    VGA_ST_B R3, R2, R12
    B.W .fetch_decode_loop

L(GRP3_F6_MUL):
    /* AX = AL * r/m8. CF and OF both mean "the product did not fit in AL",
       which is the only way a program can tell; SF/ZF/AF/PF are undefined. */
    LDRB.W R4, [R8, #REG_EAX]
    MUL.W R3, R3, R4
    STRH.W R3, [R8, #REG_EAX]
    UBFX R0, R3, #8, #8
    CMP R0, #0
    IT NE
    MOVNE R0, #1
    BFI R11, R0, #0, #1
    BFI R11, R0, #11, #1
    B.W .fetch_decode_loop

L(GRP3_F6_IMUL):
    /* Signed: CF/OF mean the 16-bit product is not the sign extension of its
       own low byte, i.e. the answer does not fit in AL. */
    LDRSB.W R4, [R8, #REG_EAX]
    SXTB R3, R3
    MUL.W R3, R3, R4
    STRH.W R3, [R8, #REG_EAX]
    SXTB R0, R3
    UXTH R0, R0
    UXTH R1, R3
    CMP R0, R1
    ITE EQ
    MOVEQ R0, #0
    MOVNE R0, #1
    BFI R11, R0, #0, #1
    BFI R11, R0, #11, #1
    B.W .fetch_decode_loop

L(GRP3_F6_DIV):
    LDRH.W R4, [R8, #REG_EAX]
    CMP R3, #0
    BEQ.W L(DIVIDE_ERROR)
    UDIV R5, R4, R3
    CMP R5, #255                   @ quotient must fit in AL
    BHI.W L(DIVIDE_ERROR)
    MLS R12, R5, R3, R4
    STRB.W R5, [R8, #REG_EAX]
    STRB.W R12, [R8, #(REG_EAX + 1)]
    B.W .fetch_decode_loop

L(GRP3_F6_IDIV):
    LDRSH.W R4, [R8, #REG_EAX]
    SXTB R3, R3
    CMP R3, #0
    BEQ.W L(DIVIDE_ERROR)
    SDIV R5, R4, R3
    SXTB R0, R5
    CMP R0, R5                     @ quotient must fit in a signed byte
    BNE.W L(DIVIDE_ERROR)
    MLS R12, R5, R3, R4
    STRB.W R5, [R8, #REG_EAX]
    STRB.W R12, [R8, #(REG_EAX + 1)]
    B.W .fetch_decode_loop

/* Vector 0. A divide error is an exception, not an exit: DOS installs a
   handler for it and programs rely on that. */
L(DIVIDE_ERROR):
    MOV.W R4, #0
    B.W L(HANDLE_INTERRUPT)

OP .op_grp3_ev_F7
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(GRP3_F7_MEM)
    AND.W R2, R1, #7
    ADD.W R2, R8, R2, LSL #2
    ADD.W R2, R2, #8
    LD_V R3, R2
    B.N L(GRP3_F7_DISPATCH)
L(GRP3_F7_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LD_V R3, R2
L(GRP3_F7_DISPATCH):
    UBFX R12, R1, #3, #3
    CMP R12, #0
    BEQ.W L(GRP3_F7_TEST)
    CMP R12, #2
    BEQ.W L(GRP3_F7_NOT)
    CMP R12, #3
    BEQ.W L(GRP3_F7_NEG)
    CMP R12, #4
    BEQ.W L(GRP3_F7_MUL)
    CMP R12, #5
    BEQ.W L(GRP3_F7_IMUL)
    CMP R12, #6
    BEQ.W L(GRP3_F7_DIV)
    CMP R12, #7
    BEQ.W L(GRP3_F7_IDIV)
    B.W .exit_loop
L(GRP3_F7_TEST):
    CODE_PTR R5
    LD_V R5, R5
    EIP_ADV_V
    /* VTEST, not a bare ANDS: at 16-bit operand size ARM's N flag is bit 31 of
       a value that only occupies the low half, so `test ax,imm16` reported
       SF=0 for every negative result. Every other TEST in this file already
       goes through VTEST/LSLS -- this one was the outlier. */
    AND.W R3, R3, R5               @ TEST sets flags and discards the result
    VTEST(R0, R3)
    X86_FLAGS_LOGIC R3, R0, R1
    B.W .fetch_decode_loop
L(GRP3_F7_NOT):
    MVN.W R3, R3
    VGA_ST_V R3, R2, R12
    B.W .fetch_decode_loop
L(GRP3_F7_NEG):
    VNORM(R3)
    MOV.W R4, R3                   @ AF aux: 0 ^ operand
    RSBS R3, R3, #0
    EOR.W R4, R4, R3
    VDENORM(R3)
    X86_FLAGS_ARITH R3, R4, R0, R1, 1, NSH   @ CF = (operand != 0)
    VGA_ST_V R3, R2, R12
    B.W .fetch_decode_loop

L(GRP3_F7_MUL):
    /* DX:AX = AX * r/m16, or EDX:EAX = EAX * r/m32. CF and OF say the product
       needed the high half. The 32-bit form needs UMULL for a 64-bit result;
       the 16-bit form fits a single register, and writing a 32-bit product
       into AX -- which is what this did -- both loses DX and corrupts the high
       half of EAX. */
#if NSH
    LDRH.W R4, [R8, #REG_EAX]
    MUL.W R3, R3, R4
    STRH.W R3, [R8, #REG_EAX]
    LSR.W R0, R3, #16
    STRH.W R0, [R8, #REG_EDX]
#else
    LDR.W R4, [R8, #REG_EAX]
    UMULL R3, R0, R4, R3
    STR.W R3, [R8, #REG_EAX]
    STR.W R0, [R8, #REG_EDX]
#endif
    CMP R0, #0
    IT NE
    MOVNE R0, #1
    BFI R11, R0, #0, #1
    BFI R11, R0, #11, #1
    B.W .fetch_decode_loop

L(GRP3_F7_IMUL):
#if NSH
    LDRSH.W R4, [R8, #REG_EAX]
    SXTH R3, R3
    MUL.W R3, R3, R4
    STRH.W R3, [R8, #REG_EAX]
    ASR.W R0, R3, #16
    STRH.W R0, [R8, #REG_EDX]
    SXTH R1, R3
    CMP R1, R3                     @ fits in 16 bits?
#else
    LDR.W R4, [R8, #REG_EAX]
    SMULL R3, R0, R4, R3
    STR.W R3, [R8, #REG_EAX]
    STR.W R0, [R8, #REG_EDX]
    ASR.W R1, R3, #31              @ the high half must be the sign extension
    CMP R1, R0
#endif
    ITE EQ
    MOVEQ R0, #0
    MOVNE R0, #1
    BFI R11, R0, #0, #1
    BFI R11, R0, #11, #1
    B.W .fetch_decode_loop

L(GRP3_F7_DIV):
#if NSH
    LDRH.W R4, [R8, #REG_EAX]
    LDRH.W R0, [R8, #REG_EDX]
    ORR.W R4, R4, R0, LSL #16      @ DX:AX
#else
    LDR.W R4, [R8, #REG_EAX]       @ 64-bit DIV is not modelled; EDX must be 0
    LDR.W R0, [R8, #REG_EDX]
    CMP R0, #0
    BNE.W L(DIVIDE_ERROR)
#endif
    CMP R3, #0
    BEQ.W L(DIVIDE_ERROR)
    UDIV R5, R4, R3
    MLS R12, R5, R3, R4
#if NSH
    CMP R5, #0x10000
    BHS.W L(DIVIDE_ERROR)          @ quotient must fit in AX
    STRH.W R5, [R8, #REG_EAX]
    STRH.W R12, [R8, #REG_EDX]
#else
    STR.W R5, [R8, #REG_EAX]
    STR.W R12, [R8, #REG_EDX]
#endif
    B.W .fetch_decode_loop

L(GRP3_F7_IDIV):
#if NSH
    LDRH.W R4, [R8, #REG_EAX]
    LDRH.W R0, [R8, #REG_EDX]
    ORR.W R4, R4, R0, LSL #16
    SXTH R3, R3
#else
    LDR.W R4, [R8, #REG_EAX]
#endif
    CMP R3, #0
    BEQ.W L(DIVIDE_ERROR)
    SDIV R5, R4, R3
    MLS R12, R5, R3, R4
#if NSH
    SXTH R0, R5
    CMP R0, R5
    BNE.W L(DIVIDE_ERROR)
    STRH.W R5, [R8, #REG_EAX]
    STRH.W R12, [R8, #REG_EDX]
#else
    STR.W R5, [R8, #REG_EAX]
    STR.W R12, [R8, #REG_EDX]
#endif
    B.W .fetch_decode_loop

#include "cpu_exec_handlers_0f.S"

/* ---------------------------------------------------------------
 * Segment override prefixes (26 ES, 2E CS, 36 SS, 3E DS, 64 FS, 65 GS)
 *
 * Two instructions, no store and no conditional branch: point R6 at the
 * descriptor and re-enter the fetch BELOW the R6 reset, so the override
 * survives into its own instruction and dies at the next boundary. Re-entry
 * also skips the PIT decrement, which is correct -- a prefixed instruction is
 * one instruction, not two.
 * --------------------------------------------------------------- */
OP_AGNOSTIC .op_seg_es_26
    ADD.W R6, R8, #SEG_ES_SEL
    B.W .fetch_next_byte_no_reset

OP_AGNOSTIC .op_seg_cs_2E
    ADD.W R6, R8, #SEG_CS_SEL
    B.W .fetch_next_byte_no_reset

OP_AGNOSTIC .op_seg_ss_36
    ADD.W R6, R8, #SEG_SS_SEL
    B.W .fetch_next_byte_no_reset

OP_AGNOSTIC .op_seg_ds_3E
    ADD.W R6, R8, #SEG_DS_SEL
    B.W .fetch_next_byte_no_reset

OP_AGNOSTIC .op_seg_fs_64
    ADD.W R6, R8, #SEG_FS_SEL
    B.W .fetch_next_byte_no_reset

OP_AGNOSTIC .op_seg_gs_65
    ADD.W R6, R8, #SEG_GS_SEL
    B.W .fetch_next_byte_no_reset

/* ---------------------------------------------------------------
 * Operand-size (66) and address-size (67) prefixes.
 *
 * Two instructions, no store, no branch beyond the re-entry, and NO
 * per-instruction cost to pay for them: R2 already holds the active mode
 * record when the prefix handler is entered -- it is what dispatched to it --
 * and .fetch_decode_loop reloads the default on the next instruction boundary,
 * so the override cannot leak. The records are 2 KB apart precisely so each
 * axis is a single EOR.
 *
 * The two axes are independent, which is the point: 66 in a 16-bit segment
 * gives a 32-bit operand with 16-bit addressing, and 67 in a 32-bit segment
 * gives the reverse. Applying a prefix twice cancels, as on real hardware.
 * --------------------------------------------------------------- */
OP_AGNOSTIC .op_pfx_66
    EOR.W R2, R2, #OPSIZE_XOR
    B.W .fetch_next_byte_no_reset

OP_AGNOSTIC .op_pfx_67
    EOR.W R2, R2, #ADDRSIZE_XOR
    B.W .fetch_next_byte_no_reset

/* ===========================================================================
 * Segment register loads.
 *
 * Real mode: base = sel<<4, limit = 0xFFFF. Protected mode walks the GDT/LDT
 * and validates the descriptor -- x86_pmode_load_seg() in x86_pmode.c.
 *
 * THIS IS THE ONE PLACE PROTECTED MODE ENTERS THE INTERPRETER, and that is the
 * whole reason it costs real mode nothing. Every segment load in the machine
 * funnels here -- MOV Sreg,Ev; POP Sreg; LES/LDS; far JMP/CALL/RETF/IRET; the
 * interrupt dispatch -- and the three instructions below already stood here to
 * trap protected mode out. Only the BRANCH TARGET changed. A real-mode guest
 * executes exactly the instruction sequence it executed before this work, and
 * nothing was added to any memory-access or dispatch path anywhere.
 *
 * R4 = segment index (0=ES 1=CS 2=SS 3=DS 4=FS 5=GS), R5 = selector.
 * Clobbers R3, R12.
 * =========================================================================== */
L(SET_SREG):
    LDR.W R12, [R8, #CPU_CR0]
    TST.W R12, #1
    BNE.W L(SET_SREG_PMODE)
    /* A selector is 16 bits. The store below truncates, but the base is built
       by shifting, so an unmasked register produced a base above the 1 MB real
       mode limit while the stored selector still looked correct. */
    UXTH R5, R5
    /* Trace CS loads. Every far transfer and every interrupt installs CS
       here, so this one place names them all. LR is logged too: it is the
       return address into whichever handler called us, which identifies the
       instruction type without a tag at each call site.
       ...which is also why it is BEHIND A FLAG and off by default: "every far
       transfer" is 1.22 M of them per 25 s of TOPBENCH, each paying fourteen
       instructions and five stores for a sixteen-entry ring nobody is reading.
       Build with X86_CS_LOG=1 to get it back (see the Makefile). */
#if X86_CS_LOG
    CMP R4, #1
    BNE.N L(SET_SREG_NOLOG)
    MOVW R3, #:lower16:x86_cs_log_idx
    MOVT R3, #:upper16:x86_cs_log_idx
    LDR R0, [R3]
    ADD.W R12, R0, #1
    STR R12, [R3]
    AND.W R0, R0, #15              @ 16 entries of 4 words
    MOVW R3, #:lower16:x86_cs_log
    MOVT R3, #:upper16:x86_cs_log
    ADD.W R3, R3, R0, LSL #4
    STR R10, [R3]                  @ linear EIP at the transfer
    STR R5, [R3, #4]               @ the CS being installed
    STR LR, [R3, #8]               @ who called us
    LDR.W R12, [R8, #CPU_ESP]
    STR R12, [R3, #12]             @ SP, to tell a stack pop from an immediate
L(SET_SREG_NOLOG):
#endif
    MOV.W R3, #SEG_STRIDE
    MLA R3, R4, R3, R8
    /* STR, not STRH: the word at SEG_ES_SEL is `sel | (flags << 16)`, and R5
       is already zero-extended by the UXTH above, so this stores the selector
       AND CLEARS THE CACHED FLAGS in one instruction -- the same instruction
       the STRH was, at the same cost.
       That matters now that flags can be non-zero. A guest LEAVING protected
       mode reloads its segments in real mode, and an STRH left the descriptor
       rights from the protected-mode load in place -- including D, which
       selects the operand size. A real-mode segment has no rights and D=0, so
       the correct real-mode cache is a zero flags field. */
    STR.W R5, [R3, #SEG_ES_SEL]
    LSL.W R12, R5, #4
    STR.W R12, [R3, #(SEG_ES_SEL + 4)]     @ base
    MOV.W R12, #0xFFFF
    STR.W R12, [R3, #(SEG_ES_SEL + 8)]     @ limit
    BX LR

/* The protected-mode arm of the above. COLD, and out of line so the real-mode
 * fall-through keeps its straight-line layout.
 *
 * Everything is done in C (x86_pmode.c): the walk reads guest memory through
 * the pager, decodes eight bytes and runs a dozen predicates, none of which is
 * anything Thumb-2 would do better and all of which is off the hot path by
 * construction -- it runs once per segment LOAD in a protected-mode guest and
 * never at all in a real-mode one.
 *
 * R4 = segment index, R5 = selector, LR = the caller's return. R4/R5 and the
 * reserved R6/R8-R11 all survive the call: AAPCS makes R4-R11 callee-saved, so
 * only R0-R3 and R12 need saving, and they are saved rather than assumed dead
 * because the callers do not agree about what is live here (LOAD_FAR_PTR has
 * already popped R0/R1 but MOV_SREG_SET still holds the ModRM in R1).
 *
 * A FAULT EXITS THE INTERPRETER. There is no protected-mode IDT gate walk yet,
 * so the vector cannot be delivered; it is recorded in cpu->pmode_excp and the
 * error code in cpu->pmode_err, and the exit carries X86_EXIT_GPF with the
 * error code in exit_info. That makes an undelivered fault LOUD and readable
 * rather than lost. Delivery is the next step and needs the gate walk.
 *
 * The exit's EIP is sound: x86_pmode_load_seg commits nothing to the segment
 * cache until every check has passed, so CS.base is untouched on a fault and
 * .save_state_and_exit recovers EIP from it correctly. */
L(SET_SREG_PMODE):
    PUSH {R0-R3, R12, LR}
    MOV R0, R8
    MOV R1, R4
    UXTH R2, R5
    BL x86_pmode_load_seg
    CBNZ R0, 1f
    POP {R0-R3, R12, PC}
1:  POP {R0-R3, R12, LR}
    MOV.W R0, #X86_EXIT_GPF
    STR.W R0, [R8, #CPU_EXIT_REASON]
    LDR.W R0, [R8, #CPU_PMODE_ERR]
    STR.W R0, [R8, #CPU_EXIT_INFO]
    B.W .save_state_and_exit

/* MOV Sreg, Ew (8E). The source is always 16 bits regardless of operand size. */
OP_AGNOSTIC .op_mov_seg_Ew_8E
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(MOV_SREG_MEM)
    AND.W R5, R1, #7
    MOV.W R5, R5, LSL #2
    ADD.W R5, R5, #8
    LDRH.W R5, [R8, R5]
    B.N L(MOV_SREG_SET)
L(MOV_SREG_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    SEAM_CHECK R3, R7
    UBFX R3, R7, #12, #12
    LDR.W R3, [R9, R3, LSL #2]
    ADDS R3, R3, R7
    MEM_TRAP_R R3
    LDRH R5, [R3, #0]
L(MOV_SREG_SET):
    UBFX R4, R1, #3, #3
    PUSH {LR}
    BL L(SET_SREG)
    POP {LR}
    B.W .fetch_decode_loop

/* MOV Ew, Sreg (8C). Always a 16-bit store. */
OP_AGNOSTIC .op_mov_Ew_seg_8C
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(MOV_EW_SREG_MEM)
    UBFX R4, R1, #3, #3
    MOV.W R3, #SEG_STRIDE
    MLA R3, R4, R3, R8
    LDRH.W R5, [R3, #SEG_ES_SEL]
    AND.W R4, R1, #7
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    STRH.W R5, [R8, R4]
    B.W .fetch_decode_loop
/* The selector has to be read AFTER the decoder runs: DECODE_MODRM uses R4 and
   R5 as scratch, so hoisting the load above the call stored its leftovers
   instead of the segment register. MOV Ev,Gv had the identical defect. */
L(MOV_EW_SREG_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    UBFX R4, R1, #3, #3
    MOV.W R3, #SEG_STRIDE
    MLA R3, R4, R3, R8
    LDRH.W R5, [R3, #SEG_ES_SEL]
    MEM_GET_PTR R3, R7
    VGA_ST_H R5, R3, R12
    B.W .fetch_decode_loop

/* LES (C4) / LDS (C5): Gv <- [mem], then Sreg <- [mem + operand size].
   The selector is always 16 bits; the offset follows the operand size. */
OP .op_les_C4
    MOV.W R0, #0                   @ ES
    B.N L(LOAD_FAR_PTR)
OP .op_lds_C5
    MOV.W R0, #3                   @ DS
L(LOAD_FAR_PTR):
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BHS.W .exit_loop               @ a register source is #UD for these
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    PUSH {R0, R1}
    SEAM_CHECK R3, R7
    UBFX R3, R7, #12, #12
    LDR.W R3, [R9, R3, LSL #2]
    ADDS R3, R3, R7
    MEM_TRAP_R R3
    LD_V R4, R3                    @ offset, operand-size wide
    ADD.W R7, R7, #OPSZ
    SEAM_CHECK R3, R7
    UBFX R3, R7, #12, #12
    LDR.W R3, [R9, R3, LSL #2]
    ADDS R3, R3, R7
    MEM_TRAP_R R3
    LDRH R5, [R3, #0]              @ selector, always 16 bits
    POP {R0, R1}
    UBFX R3, R1, #3, #3
    MOV.W R3, R3, LSL #2
    ADD.W R3, R3, #8
    STREG_V R4, R3                 @ Gv <- offset
    MOV R4, R0
    PUSH {LR}
    BL L(SET_SREG)
    POP {LR}
    B.W .fetch_decode_loop

/* PUSH/POP Sreg. The stack slot is operand-size wide, but a segment register
   is only ever 16 bits, so the pushed value is zero-extended and the popped
   one truncated. */
OP .op_push_es_06
    MOV.W R4, #0
    B.N L(PUSH_SREG)
OP .op_push_cs_0E
    MOV.W R4, #1
    B.N L(PUSH_SREG)
OP .op_push_ss_16
    MOV.W R4, #2
    B.N L(PUSH_SREG)
OP .op_push_ds_1E
    MOV.W R4, #3
L(PUSH_SREG):
    MOV.W R3, #SEG_STRIDE
    MLA R3, R4, R3, R8
    LDRH.W R3, [R3, #SEG_ES_SEL]
    PUSH_V R3, R0, R12
    B.W .fetch_decode_loop

OP .op_pop_es_07
    MOV.W R0, #0
    B.N L(POP_SREG)
OP .op_pop_ss_17
    MOV.W R0, #2
    B.N L(POP_SREG)
OP .op_pop_ds_1F
    MOV.W R0, #3
L(POP_SREG):
    POP_V R5, R3, R12
    UXTH R5, R5
    MOV R4, R0
    PUSH {LR}
    BL L(SET_SREG)
    POP {LR}
    B.W .fetch_decode_loop

/* Far JMP (EA) and far CALL (9A): an immediate ptr16:v -- offset first at the
   operand size, then the selector. Both reload CS, so EIP is rebuilt from the
   new base rather than adjusted. */
OP .op_jmp_far_EA
    MOV.W R0, #0
    B.N L(FAR_IMM)
OP .op_call_far_9A
    MOV.W R0, #1
L(FAR_IMM):
    CODE_PTR R3
    LD_V R4, R3                    @ new offset
    EIP_ADV_V
    CODE_PTR R3
    LDRH R5, [R3, #0]              @ new selector
    ADD.W R10, R10, #2
    CMP R0, #0
    BEQ.N L(FAR_SET)
    /* CALL: push the RETURN CS:IP, which is the state before the transfer. */
    LDRH.W R3, [R8, #SEG_CS_SEL]
    PUSH_V R3, R0, R12
    LDR.W R3, [R8, #SEG_CS_BASE]
    SUB.W R3, R10, R3
    PUSH_V R3, R0, R12
L(FAR_SET):
    PUSH {R4}
    MOV.W R4, #1                   @ CS
    PUSH {LR}
    BL L(SET_SREG)
    POP {LR}
    POP {R4}
    LDR.W R3, [R8, #SEG_CS_BASE]
    ADD.W R10, R4, R3
    B.W .fetch_decode_loop

/* ===========================================================================
 * REP (F3) / REPNE (F2).
 *
 * The counter is CX or ECX by ADDRESS size, which is live in R2.
 *
 * TWO paths, and the slow one is the fallback, not the exception to be avoided.
 *
 * REWIND (original): one element per pass through the main fetch loop; if the
 * run is not finished the string op re-points EIP at the prefix and the pair
 * executes again. That is what keeps interrupts serviceable in the middle of a
 * long REP MOVS -- an unbounded inner loop would stall the PIT for its whole
 * duration, which is precisely how a DOS game's timing comes apart -- and it
 * means each string op has ONE implementation rather than a plain and a
 * repeating variant.
 *
 * BATCH (fast path): the price of the rewind is a full fetch/decode/dispatch
 * for the prefix AND for the string opcode on EVERY element. Measured on
 * silicon against TOPBENCH, F3 alone was 17% of all cycles and the string ops
 * plus their prefixes ~59% -- 7 million dispatches of F3 that do no guest work.
 * So when the run is going to continue anyway, re-enter the string handler
 * DIRECTLY (`rep_handler`, resolved off the live dispatch table by the prefix)
 * instead of going round the loop. Nothing about an element changes: the
 * handler re-walks the fold per element, so a granule straddle, a segment
 * wrap and a 16-bit index wrap all behave exactly as they do on the slow path,
 * and the ZF test above still runs between elements, so REPZ/REPNZ terminate on
 * the same element they would have.
 *
 * The batch is BOUNDED, by two things that are both the real quantity rather
 * than a proxy for it:
 *   - `rep_batch`, a hard cap reloaded by the prefix, so the loop cannot run
 *     away no matter what the guest put in CX;
 *   - `pit_counter`, which IS the slice budget: a batched element decrements it
 *     exactly as a trip round .fetch_decode_loop would, and the batch stops
 *     while at least one tick is left. That single test covers the PIT slice
 *     AND a pending hardware interrupt, because the host raises one by writing
 *     the vector and dropping pit_counter to 1 -- so an IRQ lands on the same
 *     element boundary it would have landed on before.
 * Decrementing pit_counter per element is not bookkeeping hygiene: main.c pays
 * down guest_insn_debt by the slice and counts guest_insns_total from it, so a
 * batch that ran elements for free would silently make the guest faster than
 * guest_kips asked for.
 * =========================================================================== */
#ifndef REP_BATCH_MAX
/* Elements per batch. Large enough that a 64 KB REP MOVSW collapses from 65535
   dispatch pairs to 256, small enough that the interpreter is never away from
   its instruction boundary for long. */
#define REP_BATCH_MAX 256
#endif

#ifndef USE_16_BIT
.str_exit:
    LDR.W R3, [R8, #CPU_REP_ACTIVE]
#if defined(X86_TRACE_EIP) || defined(X86_TRACE_ODDSP)
    /* CBZ reaches +126 bytes and the counter bumps below do not fit inside
       that. Widened ONLY in a tracing build, so a shipping core keeps the
       two-byte encoding on this hot path. */
    CMP R3, #0
    BEQ.W 1f
#else
    CBZ R3, 1f
#endif
    LDR.W R5, [R8, #REG_ECX]
    TST.W R2, #ADDRSIZE_XOR
    IT EQ
    UXTHEQ R5, R5
    CBZ R5, 9f                         @ count exhausted -> done
    LDR.W R4, [R8, #CPU_REP_ZF_SENSE]
    CMP R4, #2
    BEQ.N 2f                           @ MOVS/STOS/LODS ignore ZF
    UBFX R3, R11, #6, #1               @ ZF
    CMP R3, R4
    BNE.N 9f                           @ REPE saw ZF=0, or REPNE saw ZF=1
2:  /* The run continues. Batch it if both bounds allow, else rewind. */
    LDR.W R12, [R8, #CPU_REP_BATCH]
    CMP R12, #0
    BEQ.N 3f                           @ cap spent
    LDR.W R3, [R8, #CPU_PIT_COUNTER]
    CMP R3, #2
    BLO.N 3f                           @ slice expiring, or an IRQ is armed
    SUB.W R12, R12, #1
    STR.W R12, [R8, #CPU_REP_BATCH]
    SUB.W R3, R3, #1                   @ a batched element still costs a tick
    STR.W R3, [R8, #CPU_PIT_COUNTER]   @ of the slice
    SUB.W R5, R5, #1
    TST.W R2, #ADDRSIZE_XOR
    IT EQ
    UXTHEQ R5, R5
    STR.W R5, [R8, #REG_ECX]
#ifdef X86_TRACE_ODDSP
    /* The predicate trap's own counter needs the same correction, or its
       X86_TRACE_SKIP arming point is measured on a different (and, before the
       fix below, non-reproducible) scale from the sampler's. */
    PUSH {R0, R3}
    MOVW R0, #:lower16:x86_oddsp_log
    MOVT R0, #:upper16:x86_oddsp_log
    LDR R3, [R0, #288]
    ADD.W R3, R3, #1
    STR R3, [R0, #288]
    POP {R0, R3}
#endif
#ifdef X86_TRACE_EIP
    /* A batched element bypasses .resume_fetch, so without this the sampler's
       index counts DISPATCHES, not guest instructions -- and where a batch gets
       cut moves with the wall-clock-driven slice, so the index differed by
       ~1500 between identical builds while guest_insns_total was bit-identical.
       That is what made every count-based bisection unsound; it is the sampler
       that was non-deterministic, not the guest. */
    PUSH {R0, R3}
    MOVW R0, #:lower16:x86_eip_log_idx
    MOVT R0, #:upper16:x86_eip_log_idx
    LDR R3, [R0]
    ADD.W R3, R3, #1
    STR R3, [R0]
    POP {R0, R3}
#endif
    LDR.W R3, [R8, #CPU_REP_HANDLER]
    BX R3                              @ straight back into the string handler
    /* rep_active must NOT survive into another instruction: an interrupt can be
       delivered at the boundary below, and a stale arming would make the ISR's
       own first string op repeat. Both slow paths clear it; the prefix re-arms. */
3:  MOV.W R3, #0
    STR.W R3, [R8, #CPU_REP_ACTIVE]
    LDR.W R10, [R8, #CPU_REP_RESUME]   @ rewind to the prefix
    B.W .fetch_decode_loop
9:  MOV.W R3, #0
    STR.W R3, [R8, #CPU_REP_ACTIVE]
1:  B.W .fetch_decode_loop
#endif

OP_AGNOSTIC .op_repne_F2
    MOV.W R4, #0                   @ REPNE: stop once ZF becomes 1
    B.N L(REP_ENTER)
OP_AGNOSTIC .op_rep_F3
    MOV.W R4, #1                   @ REP/REPE: stop once ZF becomes 0
L(REP_ENTER):
    /* Peek at the string opcode without consuming it. */
    CODE_PTR R3
    LDRB R0, [R3, #0]

    /* Only string opcodes repeat; on anything else the prefix is ignored,
       which is what the hardware does. 6C..6F are INS/OUTS, A4..A7 are
       MOVS/CMPS, AA..AF are STOS/LODS/SCAS. Only the comparing forms consult
       ZF -- sense 2 means "no ZF test", and INS/OUTS never compare, so they
       take it unconditionally. Without this arm `REP OUTSB` would run exactly
       ONE byte and drop the other 767 of a VGA palette, with no error
       anywhere -- strictly worse than the missing opcode it replaced. */
    SUB.W R3, R0, #0x6C
    CMP R3, #4
    ITT LO
    MOVLO R4, #2
    BLO.N L(REP_COUNT)
    SUB.W R3, R0, #0xA4
    CMP R3, #4
    BLO.N L(REP_STRING)
    SUB.W R3, R0, #0xAA
    CMP R3, #6
    BHS.W .fetch_next_byte_no_reset
    CMP R0, #0xAE
    IT LO
    MOVLO R4, #2                   @ AA..AD: STOS/LODS, no ZF test
    B.N L(REP_COUNT)
L(REP_STRING):
    CMP R0, #0xA6
    IT LO
    MOVLO R4, #2                   @ A4/A5: MOVS, no ZF test
L(REP_COUNT):
    LDR.W R5, [R8, #REG_ECX]
    TST.W R2, #ADDRSIZE_XOR
    IT EQ
    UXTHEQ R5, R5
    CBNZ R5, L(REP_ONE)
    ADD.W R10, R10, #1             @ count 0: skip the string opcode entirely
    B.W .fetch_decode_loop

L(REP_ONE):
    SUB.W R5, R5, #1
    TST.W R2, #ADDRSIZE_XOR
    IT EQ
    UXTHEQ R5, R5
    STR.W R5, [R8, #REG_ECX]
    STR.W R4, [R8, #CPU_REP_ZF_SENSE]
    SUB.W R3, R10, #1              @ this prefix, for the rewind
    STR.W R3, [R8, #CPU_REP_RESUME]
    /* The batch re-entry point, taken from the LIVE dispatch table with the
       opcode still in R0, so it is by construction the same handler the fetch
       below is about to reach -- including the right operand-size variant. */
    LDR.W R12, [R2, R0, LSL #2]
    STR.W R12, [R8, #CPU_REP_HANDLER]
    MOV.W R12, #REP_BATCH_MAX
    STR.W R12, [R8, #CPU_REP_BATCH]
    MOV.W R3, #1
    STR.W R3, [R8, #CPU_REP_ACTIVE]
    B.W .fetch_next_byte_no_reset

/* ===========================================================================
 * 0F 0B imm8 -- the HLE hypercall.
 *
 * UD2 is architecturally #UD on every x86 since the 286, in BOTH modes, and it
 * is not a vector, so no mode switch and no guest IVT edit can alias it. That
 * is why the escape is an opcode rather than a software interrupt: a guest is
 * free to hook INT 21h, and it should keep working. The loader plants a
 * three-byte stub (0F 0B nn / CF) at each vector we serve, so a guest that
 * chains one still reaches us, and a guest that replaces one takes over
 * cleanly.
 *
 * The imm8 selects the service. Control leaves the interpreter entirely --
 * the host reads exit_info, does the work against FatFs or the LCD, writes
 * registers back and re-enters. EIP is already past the imm8, so the IRET in
 * the stub runs on re-entry and unwinds the guest's INT frame.
 * =========================================================================== */
OP_AGNOSTIC .op_hypercall_0F0B
    CODE_PTR R3
    LDRB R3, [R3, #0]              @ service selector
    ADD.W R10, R10, #1
    LDR.W R4, [R8, #CPU_HLE_ENABLED]   @ CBZ needs a low register
    CBZ R4, L(HYPERCALL_UD)
    STR.W R3, [R8, #CPU_EXIT_INFO]
    MOV.W R3, #X86_EXIT_HLE_INT
    STR.W R3, [R8, #CPU_EXIT_REASON]
    B.W .save_state_and_exit
L(HYPERCALL_UD):
    /* HLE off: behave like the real instruction. Rewind past the imm8 so the
       #UD frame points at the 0F 0B itself, as hardware would. */
    SUB.W R10, R10, #3
    MOV.W R4, #6                   @ #UD
    B.W L(HANDLE_INTERRUPT)

/* FF /3 CALL FAR m16:v and FF /5 JMP FAR m16:v.
 *
 * Indirect far transfers: the target is a pointer IN MEMORY, offset first then
 * selector -- not an immediate. Every relocating program uses one to reach its
 * relocated copy, so their absence stopped TOPBENCH dead with an unimplemented
 * 0xFF. R7 holds the effective address from the ModRM decode.
 *
 * The OFFSET is operand-size wide and the SELECTOR is always 16 bits, so the
 * selector sits at +2 in a 16-bit segment and at +4 in a 32-bit one. Both
 * loads used to be hardcoded LDRH at +0/+2, which in a 32-bit segment threw
 * away the top half of EIP and then read the offset's own high half as the
 * selector -- an arbitrary CS built out of the target address. LES/LDS
 * (L(LOAD_FAR_PTR)) already reads the pair this way; this now matches it. */
L(GRP5_FAR_ENTRY):
    SEAM_CHECK R3, R7
    UBFX R3, R7, #12, #12
    LDR.W R3, [R9, R3, LSL #2]
    ADDS R3, R3, R7
    MEM_TRAP_R R3
    LD_V R4, R3                    @ new IP/EIP, operand-size wide
    ADD.W R7, R7, #OPSZ
    SEAM_CHECK R3, R7
    UBFX R3, R7, #12, #12
    LDR.W R3, [R9, R3, LSL #2]
    ADDS R3, R3, R7
    MEM_TRAP_R R3
    LDRH R5, [R3, #0]              @ new CS selector
    CMP R0, #0
    BEQ.N L(GRP5_FAR_SET)
    /* CALL: push the return CS:IP before switching. */
    LDRH.W R3, [R8, #SEG_CS_SEL]
    PUSH_V R3, R0, R12
    LDR.W R3, [R8, #SEG_CS_BASE]
    SUB.W R3, R10, R3
    PUSH_V R3, R0, R12
L(GRP5_FAR_SET):
    PUSH {R4, LR}
    MOV.W R4, #1                   @ CS
    BL L(SET_SREG)
    POP {R4, LR}
    LDR.W R3, [R8, #SEG_CS_BASE]
    ADD.W R10, R4, R3
    B.W .fetch_decode_loop

L(GRP5_CALLF):
    MOV.W R0, #1
    B.W L(GRP5_FAR_ENTRY)
L(GRP5_JMPF):
    MOV.W R0, #0
    B.W L(GRP5_FAR_ENTRY)
