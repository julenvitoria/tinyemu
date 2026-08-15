OP .op_0f_0F
    /* The 0F table comes from the mode record, not a fixed symbol, and lands
       in R3 so R2 keeps pointing at the record -- the two-byte opcodes that
       take a ModRM still need its decoder pointer. */
    LDR.W R3, [R2, #REC_TABLE_0F]
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD R10, R10, #1
#if X86_PROFILE
    /* Second dispatch of a two-byte opcode, so it CLOSES slot 0x0F (the escape
       fetch itself) and opens slot 256+n. R2 and R3 must survive -- the record
       and the 0F table -- so the macro gets R0/R4/R5/R12, all dead here. */
    PROF_DISPATCH R1, R12, R0, R4, R5, bias=256
#endif
    /* NAME THE OPCODE, NOT THE ESCAPE.
     *
     * .exit_loop reports CPU_EXIT_INFO from R0, and R0 still held 0x0F here --
     * so EVERY unimplemented two-byte opcode reported the identical
     * exit_info=0x0F, and three unrelated titles "converged" on one byte that
     * was never the byte any of them executed. The escape is not the
     * instruction. (They really were on three different opcodes: 0F 01 /6,
     * 0F 01 /6 and 0F AF. docs/13.)
     *
     * R0 is dead across the whole 0F path -- the contract's "R0 is the opcode"
     * ends at this second dispatch, PROF_DISPATCH already takes R0 as scratch,
     * and every 0F handler writes R0 before reading it -- so it costs one
     * instruction to make it carry the truth: 0x0Fnn, the escape in the high
     * byte and the SECOND byte in the low one. exit_info > 0xFF therefore reads
     * as "two-byte opcode 0F nn" with no ambiguity against the one-byte table.
     *
     * The reported EIP is likewise PAST the whole two-byte opcode: R10 was
     * advanced by the dispatch and again just above, so the instruction that
     * exited starts at EIP-2. */
    ORR.W R0, R1, #0x0F00
    LDR.W PC, [R3, R1, LSL #2]
L(opcode_0f_dispatch_target):


/* 0F 00 -- group 6. SLDT (/0) and LLDT (/2) only.
 *
 * STR (/1) and LTR (/3) are NOT here and are not an oversight: PLAN.md records
 * that the TSS work is not needed because INT 21h is HLE'd, so there is no
 * task state to point a task register at. VERR (/4) and VERW (/5) wait for a
 * guest that asks for them. All four keep the .exit_loop they have today,
 * which names the opcode rather than inventing an answer.
 *
 * The operand is a 16-bit selector however the operand size is set. */
OP .op_0f_00
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    UBFX R3, R1, #3, #3
    CMP R3, #0
    BEQ.W L(OP_SLDT)
    CMP R3, #2
    BNE.W .exit_loop
    /* ---- LLDT ---- */
    BL L(LXXX_CPL_CHECK)
    CMP R1, #192                   @ mod==3: the selector is in a register
    BLO.W L(OP_LLDT_MEM)
    AND.W R5, R1, #7
    ADD.W R5, R8, R5, LSL #2
    LDRH.W R5, [R5, #8]
    B.W L(OP_LLDT_GO)
L(OP_LLDT_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    SEAM_CHECK R3, R7
    UBFX R3, R7, #12, #12
    LDR.W R3, [R9, R3, LSL #2]
    ADDS R3, R3, R7
    MEM_TRAP_R R3
    LDRH R5, [R3, #0]
L(OP_LLDT_GO):
    PUSH {R0-R3, R12, LR}
    MOV R0, R8
    UXTH R1, R5
    BL x86_pmode_lldt
    CBNZ R0, 1f
    POP {R0-R3, R12, LR}
    B.W .fetch_decode_loop
1:  POP {R0-R3, R12, LR}
    MOV.W R0, #X86_EXIT_GPF
    STR.W R0, [R8, #CPU_EXIT_REASON]
    LDR.W R0, [R8, #CPU_PMODE_ERR]
    STR.W R0, [R8, #CPU_EXIT_INFO]
    B.W .save_state_and_exit

/* SLDT: store the LDT selector. Unprivileged in both references. */
L(OP_SLDT):
    LDR.W R5, [R8, #CPU_LDT_SEL]
    CMP R1, #192
    BLO.W L(OP_SLDT_MEM)
    AND.W R3, R1, #7
    ADD.W R3, R8, R3, LSL #2
    STRH.W R5, [R3, #8]
    B.W .fetch_decode_loop
L(OP_SLDT_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    LDR.W R5, [R8, #CPU_LDT_SEL]
    MEM_GET_PTR R3, R7
    VGA_ST_H R5, R3, R12
    B.W .fetch_decode_loop

OP .op_0f_01
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    /* SMSW (/4) is decided BEFORE the ModRM walk, because it is the one member
     * of this group with a REGISTER form -- `0F 01 E0`, SMSW AX, which is what
     * a Microsoft/Borland C startup issues to read CR0.EM before it probes for
     * an x87. LGDT/LIDT are memory-only, so the walk was unconditional here and
     * a mod=11 encoding fell through it into .exit_loop: the whole title died
     * on the first instruction of its runtime's coprocessor detection.
     * Measured on MORTAL_KOMBAT (docs/13). */
    UBFX R3, R1, #3, #3
    CMP R3, #4
    BEQ.W L(OP_SMSW)
    /* LMSW (/6) is decided here for the SAME reason SMSW is: it has a register
     * form, `0F 01 F0` = LMSW AX, and that is the encoding a DOS extender
     * actually issues. Deciding it after the memory-only walk would send the
     * mod=11 form into .exit_loop exactly as SMSW's used to go. */
    CMP R3, #6
    BEQ.W L(OP_LMSW)
    /* The ADDRESS-SIZE decoder from the mode record, not a hardwired 32-bit
       one. `BL L(DECODE_MODRM_32)` was unconditional here, so in a 16-bit
       guest -- which is every guest in this tree -- `LIDT [disp16]` (ModRM
       0x16) decoded as the 32-bit grammar's [ESI] and loaded the IDT from
       whatever ESI pointed at. It never showed because nothing walked the IDT
       afterwards. Same defect the docs describe for the five handlers that
       ignore 0x67; this is a sixth. */
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    UBFX R3, R1, #3, #3
    CMP R3, #2
    BEQ.N L(OP_LGDT)
    CMP R3, #3
    BEQ.N L(OP_LIDT)
    /* A GROUP OPCODE IS NOT AN OPCODE. "0F 01 is unimplemented" is not a
       reportable fact -- /2, /3, /4 and /6 are implemented and /0, /1, /7 are
       not, and which one a guest issued is the entire question. R1 is the
       ModRM byte and is still live, so the exit carries it in the high half:
       exit_info reads 0xMM0F01. Cold path, one instruction. */
    ORR.W R0, R0, R1, LSL #16
    B.W .exit_loop

/* SMSW: store the low 16 bits of CR0. Unprivileged at every CPL on a 286 and
 * later (tiny386 i386.c, DOSBox-X src/cpu/cpu.cpp CPU_SMSW), which is the whole
 * point of it -- LMSW is the privileged half. A halfword in both operand sizes,
 * matching SLDT above; Intel's `SMSW r32` zero-extension buys nothing for a
 * guest that only ever tests the bottom four bits. */
L(OP_SMSW):
    LDR.W R5, [R8, #CPU_CR0]
    CMP R1, #192
    BLO.W L(OP_SMSW_MEM)
    AND.W R3, R1, #7
    ADD.W R3, R8, R3, LSL #2
    STRH.W R5, [R3, #CPU_REGS]
    B.W .fetch_decode_loop
L(OP_SMSW_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    LDR.W R5, [R8, #CPU_CR0]
    MEM_GET_PTR R3, R7
    VGA_ST_H R5, R3, R12
    B.W .fetch_decode_loop

/* LMSW -- 0F 01 /6. THE INSTRUCTION THAT TURNS PROTECTED MODE ON, and the one
 * that was really stopping Dark Forces and Mortal Kombat.
 *
 * Both were reported as "unimplemented opcode 0x0F" at one address shared with
 * a third title, which was an artefact of the exit reporting twice over:
 * .exit_loop named the ESCAPE byte rather than the opcode, and "0F 01" names a
 * GROUP rather than a member. Both are fixed above; what they were really
 * executing is `0F 01 F0` = LMSW AX -- the 286 entry sequence, which is what a
 * DOS extender uses because it must also run on a 286.
 *
 * SEMANTICS, and the two references AGREE here (they do not always):
 *   references/tiny386/i386.c:3292-3294  cr0 = (cr0 & ((~0xf) | 1)) | (src & 0xf)
 *   references/dosbox-x/src/cpu/cpu.cpp:2788-2795  same, with the rule spelled
 *   out: "low 4 bits only, cannot change PE ... stuck on, cannot exit
 *   protected mode, 286 style".
 * So it writes PE/MP/EM/TS and NOTHING above bit 3, and PE is SET-ONLY: the
 * `BIC #0xE` below keeps bit 0 of the old CR0, which is exactly tiny386's
 * `(~0xf)|1` mask. Clearing PE is MOV CR0's job, not this one.
 *
 * The operand is 16 bits in BOTH operand sizes -- LMSW has no 32-bit form --
 * so this is deliberately NOT written against the width macros.
 *
 * CPL 0, but only in protected mode: L(LXXX_CPL_CHECK) is the same predicate
 * LGDT/LIDT/LLDT use, and it is DOSBox-X's rule (`cpu.pmode && cpu.cpl>0`)
 * rather than tiny386's unconditional one -- in real mode there is no CPL.
 *
 * The PE-transition tail is MOV CR0's, byte for byte and for its reasons: the
 * segment caches are deliberately NOT reloaded (the far jump that follows is
 * what installs a real descriptor and flushes the pipeline), and
 * x86_pmode_sync_mode() is called only when PE actually MOVED, because the
 * interpreter picks its dispatch tables once from CS.D. */
L(OP_LMSW):
    BL L(LXXX_CPL_CHECK)
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(OP_LMSW_MEM)
    AND.W R3, R1, #7
    ADD.W R3, R8, R3, LSL #2
    LDRH.W R5, [R3, #CPU_REGS]
    B.N L(OP_LMSW_APPLY)
L(OP_LMSW_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN]
    BLX R12
    /* A HALFWORD load is a multi-byte fold site, so it needs SEAM_CHECK --
       CLAUDE.md's rule, and the reason is that the two bytes can straddle a
       4 KB granule whose neighbour is not the host memory after it. */
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LDRH R5, [R2, #0]
L(OP_LMSW_APPLY):
    LDR.W R4, [R8, #CPU_CR0]
    AND.W R5, R5, #0xF             @ PE, MP, EM, TS -- nothing else
    BIC.W R3, R4, #0xE             @ keep everything above bit 3, AND old PE
    ORR.W R5, R3, R5
    STR.W R5, [R8, #CPU_CR0]
    EOR.W R4, R4, R5
    TST.W R4, #1                   @ did PE actually move?
    BEQ.W .fetch_decode_loop
    PUSH {R0-R3, R12, LR}
    MOV R0, R8
    BL x86_pmode_sync_mode
    POP {R0-R3, R12, LR}
    B.W .fetch_decode_loop

/* LGDT and LIDT read the SAME six-byte operand -- a 16-bit limit then a 32-bit
 * base -- and differ only in where they put it. They shared no code, and LGDT
 * PARSED NOTHING AT ALL: it checked CPL and branched to the fetch loop, so
 * every LGDT a guest issued was silently discarded and the GDT stayed at
 * base 0 limit 0. That is invisible until something walks the table, which is
 * exactly what this work adds, so it is fixed here rather than left.
 *
 * Operand shape and the 0x66 rule: references/tiny386/i386.c:3239-3256. The
 * 32-bit dword is ALWAYS loaded from memory; the 16-bit form differs only by
 * masking the base to 24 bits (i386.c:3246, `if (opsz16) base &= 0xffffff`).
 * The limit is 16 bits in both. DOSBox-X masks in the same place
 * (src/cpu/core_full/op.h:437,441).
 *
 * CPL 0 is required and the fault is #GP(0) -- tiny386 i386.c:3240,
 * DOSBox-X requires it of LLDT/LTR at src/cpu/cpu.cpp:2432,2456. */
L(OP_LGDT):
    BL L(LXXX_CPL_CHECK)
    BL L(LXXX_OPERAND)             @ R3 = limit, R4 = base
    STR.W R3, [R8, #CPU_GDT_LIMIT]
    STR.W R4, [R8, #CPU_GDT_BASE]
    B.W .fetch_decode_loop

L(OP_LIDT):
    BL L(LXXX_CPL_CHECK)
    BL L(LXXX_OPERAND)
    STRH.W R3, [R8, #CPU_IDTR_LIMIT]
    STR.W R4, [R8, #CPU_IDTR_BASE]
    B.W .fetch_decode_loop

/* CPL 0, but only once protected mode is on: in real mode there is no CPL and
   these are unprivileged. Returns, or branches away to the fault.
   R12 ONLY. This is called from LLDT *before* its ModRM is decoded, so R2 is
   still the live MODE RECORD and R1 the ModRM byte -- the register contract in
   CLAUDE.md makes clobbering R2 there a real bug, not a style point. */
L(LXXX_CPL_CHECK):
    LDR.W R12, [R8, #CPU_CR0]
    TST.W R12, #1
    IT EQ
    BXEQ LR                        @ real mode: no privilege to check
    LDR.W R12, [R8, #CPU_CPL]
    CMP R12, #0
    IT EQ
    BXEQ LR
    B.W .raise_gp_fault

/* The shared six-byte read. R7 = effective address from DECODE_MODRM.
   Returns limit in R3 and base in R4. Clobbers R2, R5. */
L(LXXX_OPERAND):
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LDRH R3, [R2, #0]              @ limit, 16 bits in both forms
    LDRH R4, [R2, #2]
    LDRH R5, [R2, #4]
    BFI R4, R5, #16, #16           @ base, 32 bits read in both forms
#if OPSZ == 2
    BIC.W R4, R4, #0xFF000000      @ ...but masked to 24 for the 16-bit form
#endif
    BX LR

OP .op_int3_CC
    MOV.W R4, #3
    B.N L(HANDLE_INTERRUPT)

OP .op_into_CE
    UBFX R3, R11, #11, #1
    CMP R3, #1
    BNE.W .fetch_decode_loop
    MOV.W R4, #4
    B.N L(HANDLE_INTERRUPT)

OP .op_int_imm8_CD
    CODE_PTR R4
    LDRB R4, [R4, #0]
    ADD.W R10, R10, #1
    B.N L(HANDLE_INTERRUPT)
L(raise_gp_fault):
    MOV.W R4, #13

/* Real-mode interrupt dispatch.
 *
 * The previous version read 8-byte protected-mode gate descriptors and pushed
 * 32-bit frames unconditionally, pushed a literal 0 where CS belongs, and
 * never installed the new CS -- so no vector could ever have worked in real
 * mode, which is the only mode that exists today.
 *
 * Real mode: the IVT entry is 4 bytes, offset first then selector; the frame
 * is FLAGS, CS, IP at the operand size; IF and TF are cleared on entry so the
 * handler is not immediately re-interrupted.
 *
 * Protected mode traps out. The gate path needs descriptor validation and a
 * possible stack switch, and quietly treating a gate as an IVT entry would
 * jump somewhere plausible and fail much later.
 *
 * R4 = vector number.
 */
#ifdef USE_16_BIT
    /* Reachable from the shared exit code in cpu_exec.S, which is outside both
       passes. Needs the Thumb marking or the linker cannot build the branch. */
    .global HANDLE_INTERRUPT_16
    .type HANDLE_INTERRUPT_16, %function
    .thumb_func
#endif
L(HANDLE_INTERRUPT):
    LDR.W R12, [R8, #CPU_CR0]
    TST.W R12, #1
    BNE.W .exit_loop

    PUSH_V R11, R3, R12            @ FLAGS
    LDRH.W R0, [R8, #SEG_CS_SEL]
    PUSH_V R0, R3, R12             @ CS
    LDR.W R0, [R8, #SEG_CS_BASE]
    SUB.W R0, R10, R0              @ IP of the NEXT instruction
    PUSH_V R0, R3, R12

    BIC.W R11, R11, #(1 << 9)      @ IF
    BIC.W R11, R11, #(1 << 8)      @ TF

    LDR.W R3, [R8, #CPU_IDTR_BASE]
    ADD.W R3, R3, R4, LSL #2       @ 4-byte IVT entries, not 8-byte gates
    MEM_GET_PTR_R R12, R3
    LDRH R0, [R12]                 @ new IP
    ADD.W R3, R3, #2
    MEM_GET_PTR_R R12, R3
    LDRH R5, [R12]                 @ new CS selector

    /* Which vector, and what it is about to install. x86_cs_log says an
       interrupt happened; only this says which one, which is the question the
       moment a guest hooks a vector and then walks off.
       Behind X86_CS_LOG with the CS ring, and off by default for the same
       reason -- note the four-instruction IVT RE-READ below, which exists only
       because this block needed R0 as scratch. Twelve instructions of logging
       cost sixteen. */
#if X86_CS_LOG
    MOVW R3, #:lower16:x86_int_vec_log_idx
    MOVT R3, #:upper16:x86_int_vec_log_idx
    LDR R12, [R3]
    ADD.W R0, R12, #1
    STR R0, [R3]
    AND.W R12, R12, #15
    MOVW R3, #:lower16:x86_int_vec_log
    MOVT R3, #:upper16:x86_int_vec_log
    ORR.W R0, R5, R4, LSL #16
    STR.W R0, [R3, R12, LSL #2]

    LDR.W R3, [R8, #CPU_IDTR_BASE]
    ADD.W R3, R3, R4, LSL #2
    MEM_GET_PTR_R R12, R3
    LDRH R0, [R12]                 @ new IP (reloaded: R0 was used above)
#endif

    PUSH {R0, LR}
    MOV.W R4, #1                   @ CS
    BL L(SET_SREG)
    POP {R0, LR}
    LDR.W R3, [R8, #SEG_CS_BASE]
    ADD.W R10, R0, R3
    B.W .fetch_decode_loop

OP .op_iret_CF
    /* Pops IP, CS, FLAGS -- the exact inverse of the frame above. The old
       version popped 32-bit values, skipped a word, and never installed CS. */
    MOVW R3, #:lower16:x86_iret_log
    MOVT R3, #:upper16:x86_iret_log
    LDR.W R12, [R8, #CPU_ESP]
    STR R12, [R3, #12]             @ SP before the pops
    POP_V R0, R3, R12              @ IP
    MOVW R3, #:lower16:x86_iret_log
    MOVT R3, #:upper16:x86_iret_log
    STR R0, [R3]
    POP_V R5, R3, R12              @ CS
    UXTH R5, R5
    MOVW R3, #:lower16:x86_iret_log
    MOVT R3, #:upper16:x86_iret_log
    STR R5, [R3, #4]
    POP_V R11, R3, R12             @ FLAGS
    MOVW R3, #:lower16:x86_iret_log
    MOVT R3, #:upper16:x86_iret_log
    STR R11, [R3, #8]
    PUSH {R0, LR}
    MOV.W R4, #1
    BL L(SET_SREG)
    POP {R0, LR}
    LDR.W R3, [R8, #SEG_CS_BASE]
    ADD.W R10, R0, R3
    B.W .fetch_decode_loop
/* The former L(handle_pit_or_irq) / L(exit_loop) lived here. Both were defined
   and never referenced -- the fetch loop has always branched to the versions in
   cpu_exec.S. They are deleted rather than wired up because the "real" one was
   also wrong: it stored to [R8, #2180], which is not a field at all (it is the
   last word of tlb_write_host), and it abused pit_reload as a pending flag.
   In-guest IRQ injection needs the real-mode IVT dispatch that does not exist
   yet; see PLAN.md Phase 2. Until then the timer exits to the host. */

/* Near Jcc. The displacement follows the OPERAND size -- rel16 under 16-bit,
   rel32 under 32-bit -- and it is signed. The handler was dual-emitted but read
   a hard-coded four bytes, so at 16 bits it consumed two bytes too many and
   jumped by a number assembled out of the following instruction. */
OP .op_0f_jcc_rel32
    UBFX R2, R1, #0, #4
    CODE_PTR R4
    LDS_V R4, R4
    EIP_ADV_V
    BL L(EVAL_COND)
    CMP R3, #1
    IT EQ
    ADDEQ R10, R4
    EIP_WRAP
    B.W .fetch_decode_loop

OP .op_0f_setcc
    UBFX R2, R1, #0, #4
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(OP_0F_SETCC_MEM)
    PUSH {R2}
    BL L(EVAL_COND)
    POP {R2}
    AND.W R4, R1, #7
    AND.W R5, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R5, LSL #2
    ADD.W R4, R4, #8
    STRB.W R3, [R8, R4]
    B.W .fetch_decode_loop

L(OP_0F_SETCC_MEM):
    PUSH {R2}
    BL L(DECODE_MODRM_32)
    POP {R2}
    PUSH {R7}
    BL L(EVAL_COND)
    POP {R7}
    UBFX R12, R7, #12, #12
    LDR.W R12, [R9, R12, LSL #2]
    ADDS R12, R12, R7
    MEM_TRAP_W R12
    VGA_ST_B R3, R12, R4
    B.W .fetch_decode_loop

/* 0F 20 /r  MOV r32, CRn      0F 22 /r  MOV CRn, r32
 *
 * How a program tells a 386 from a 286: the 286 has no such instruction, so a
 * CPU benchmark runs this before anything else. TOPBENCH does, and without it
 * the interpreter stopped on the 0F escape.
 *
 * Both are ALWAYS 32-bit whatever the operand size, and the ModRM's mod field
 * is ignored -- the operand is a register however mod is encoded. Only CR0 is
 * modelled; the others read as zero, which is what a 386 with no paging and no
 * task switching would report anyway. Writing CR0 does not itself switch the
 * machine: a segment load with PE set is what traps, and that check already
 * exists in SET_SREG.
 */
/* 0F A2 -- CPUID. Leaf 0 and leaf 1, and nothing else.
 *
 * We ANSWER it rather than refusing it because the guest has already proved it
 * may: the same routine toggles EFLAGS.ID (bit 21) with `0F BA` above and finds
 * it writable, which on real silicon is the definition of "CPUID is present".
 * The alternative -- masking ID in POPF so the guest concludes 386 -- puts a
 * test on the hot flag path to buy the same outcome, and would then be a lie
 * about a machine whose 0F opcodes are all 486-era anyway.
 *
 * The signature is a 486SX: family 4, EDX feature bits ALL ZERO, and bit 0 of
 * EDX is the on-chip FPU bit, so this agrees with .op_esc_x87 -- one machine
 * with no coprocessor, described the same way twice. Do not set a feature bit
 * here without the instruction behind it.
 *
 * MEASURED on MORTAL_KOMBAT (docs/13): `66 B8 01 00 00 00 / 0F A2 / 8A C4 /
 * 25 0F 00` -- leaf 1, then AH masked to the family nibble. Its THIRD wall. */
OP_AGNOSTIC .op_0f_cpuid_A2
    LDR.W R3, [R8, #CPU_REGS]
    CBNZ R3, 1f
    MOV.W R4, #1                   @ EAX: highest leaf we answer
    STR.W R4, [R8, #(CPU_REGS + 0)]
    MOVW R4, #0x6547               @ EBX "Genu"
    MOVT R4, #0x756E
    STR.W R4, [R8, #(CPU_REGS + 12)]
    MOVW R4, #0x6E69               @ EDX "ineI"
    MOVT R4, #0x4965
    STR.W R4, [R8, #(CPU_REGS + 8)]
    MOVW R4, #0x746E               @ ECX "ntel"
    MOVT R4, #0x6C65
    STR.W R4, [R8, #(CPU_REGS + 4)]
    B.W .fetch_decode_loop
1:  MOVW R4, #0x0400               @ family 4, model 0, stepping 0
    STR.W R4, [R8, #(CPU_REGS + 0)]
    MOV.W R4, #0
    STR.W R4, [R8, #(CPU_REGS + 4)]
    STR.W R4, [R8, #(CPU_REGS + 8)]    @ no FPU, no TSC, no MSR, no CX8
    STR.W R4, [R8, #(CPU_REGS + 12)]
    B.W .fetch_decode_loop

/* 0F BA -- Group 8: BT/BTS/BTR/BTC with an IMMEDIATE bit number.
 *
 * The imm8 forms only. The register-source forms (0F A3/AB/B3/BB) address a bit
 * STRING -- a signed offset that walks off the effective address into adjacent
 * words -- and are deliberately left at .exit_loop rather than half-built.
 * These four take the offset MODULO the operand size (Intel SDM, Group 8), so
 * the address never moves and the whole opcode is a mask.
 *
 * /0../3 are undefined encodings and stay loud.
 *
 * Only CF is architecturally defined; OF/SF/ZF/AF/PF are undefined, so R11's
 * other bits are left exactly as they were rather than invented.
 *
 * MEASURED on MORTAL_KOMBAT (docs/13): its runtime's CPU identification is
 * `66 0F BA FA 15 / 66 0F BA FA 12` -- toggling EFLAGS.ID and EFLAGS.AC in EDX
 * to tell a 386 from a 486 from a 586 -- and it was the title's SECOND wall,
 * three instructions after the x87 detection. */
OP .op_0f_ba
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    UBFX R3, R1, #3, #3
    CMP R3, #4
    BLO.W .exit_loop
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.W L(BTGRP_MEM)
    AND.W R4, R1, #7
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #CPU_REGS
    LDREG_V R5, R4
    CODE_PTR R3
    LDRB R3, [R3, #0]
    ADD.W R10, R10, #1
    AND.W R3, R3, #(OPSZ * 8 - 1)
    LSR.W R12, R5, R3
    BFI R11, R12, #0, #1           @ CF = the bit as it was
    UBFX R12, R1, #3, #3
    CMP R12, #4
    BEQ.W .fetch_decode_loop       @ BT: no write-back
    MOV.W R0, #1
    LSL.W R0, R0, R3
    CMP R12, #5
    BEQ.N 1f
    CMP R12, #6
    BEQ.N 2f
    EOR.W R5, R5, R0               @ BTC
    B.N 3f
1:  ORR.W R5, R5, R0               @ BTS
    B.N 3f
2:  BIC.W R5, R5, R0               @ BTR
3:  STREG_V R5, R4
    B.W .fetch_decode_loop

L(BTGRP_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    CODE_PTR R3                    @ the imm8 comes AFTER the displacement
    LDRB R3, [R3, #0]
    ADD.W R10, R10, #1
    AND.W R3, R3, #(OPSZ * 8 - 1)
    UBFX R12, R1, #3, #3           @ R1 survives DECODE_MODRM; R2-R5 do not
    CMP R12, #4
    BNE.N 4f
    /* BT: a pure read, so it takes the READ walk. Going through the RMW walk
       would record a VGA latch for an access that never writes. */
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LD_V R5, R2
    LSR.W R12, R5, R3
    BFI R11, R12, #0, #1
    B.W .fetch_decode_loop
4:  MEM_GET_PTR_RMW R2, R7
    LD_V R5, R2
    LSR.W R4, R5, R3
    BFI R11, R4, #0, #1
    MOV.W R0, #1
    LSL.W R0, R0, R3
    CMP R12, #5
    BEQ.N 5f
    CMP R12, #6
    BEQ.N 6f
    EOR.W R5, R5, R0
    B.N 7f
5:  ORR.W R5, R5, R0
    B.N 7f
6:  BIC.W R5, R5, R0
7:  VGA_ST_V R5, R2, R7
    B.W .fetch_decode_loop

OP_AGNOSTIC .op_0f_mov_r32_cr_20
    MOV.W R4, #0
    B.N L(OP_0F_CR)
OP_AGNOSTIC .op_0f_mov_cr_r32_22
    MOV.W R4, #1
L(OP_0F_CR):
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    AND.W R2, R1, #7               @ the r32
    MOV.W R2, R2, LSL #2
    ADD.W R2, R2, #8
    UBFX R3, R1, #3, #3            @ which control register
    CBNZ R4, 1f
    MOV.W R5, #0
    CMP R3, #0
    IT EQ
    LDREQ.W R5, [R8, #CPU_CR0]
    STR.W R5, [R8, R2]
    B.W .fetch_decode_loop
1:  LDR.W R5, [R8, R2]
    CMP R3, #0
    BNE.W .fetch_decode_loop       @ CR2/CR3/CR4: still not modelled
    /* ---- THE CR0.PE TRANSITION ----------------------------------------
     * Writing CR0 does NOT reload or invalidate the segment caches, and that
     * is not an omission -- it is what makes the standard entry sequence work.
     * Between the `MOV CR0` that sets PE and the far jump that follows it, CS
     * still holds its real-mode cache (base = sel<<4, D=0) and the guest is
     * executing 16-bit code at a real-mode base; the far JUMP is what installs
     * a real descriptor and FLUSHES THE PIPELINE. tiny386 does exactly this
     * and nothing more (references/tiny386/i386.c:2251-2255): flush the TLB if
     * PG/WP/PE moved, then store. The same holds leaving protected mode.
     *
     * What DOES have to happen on the transition is re-deriving the dispatch
     * table. The interpreter picks its mode record once, at entry, from CS.D
     * (cpu_exec.S), so a guest that leaves protected mode from a 32-bit code
     * segment would keep running the 32-bit tables against 16-bit real-mode
     * code. Done here, on the PE change only, because this instruction is cold
     * and every alternative site is not. */
    LDR.W R4, [R8, #CPU_CR0]
    STR.W R5, [R8, #CPU_CR0]
    EOR.W R4, R4, R5
    TST.W R4, #1                   @ did PE actually move?
    BEQ.W .fetch_decode_loop
    PUSH {R0-R3, R12, LR}
    MOV R0, R8
    BL x86_pmode_sync_mode
    POP {R0-R3, R12, LR}
    B.W .fetch_decode_loop

OP .op_0f_movzx_gv_eb
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(OP_0F_MOVZX_GV_EB_MEM)
    AND.W R4, R1, #7
    AND.W R5, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R5, LSL #2
    ADD.W R4, R4, #8
    LDRB.W R4, [R8, R4]
    B.N L(OP_0F_MOVZX_GV_EB_WRITE)

L(OP_0F_MOVZX_GV_EB_MEM):
    BL L(DECODE_MODRM_32)
    UBFX R4, R7, #12, #12
    LDR.W R4, [R9, R4, LSL #2]
    ADDS R4, R4, R7
    MEM_TRAP_R R4
    LDRB R4, [R4, #0]

L(OP_0F_MOVZX_GV_EB_WRITE):
    UBFX R3, R1, #3, #3
    MOV.W R3, R3, LSL #2
    ADD.W R3, R3, #8
    STR.W R4, [R8, R3]
    B.W .fetch_decode_loop

OP .op_0f_movzx_gv_ew
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(OP_0F_MOVZX_GV_EW_MEM)
    AND.W R4, R1, #7
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDRH.W R4, [R8, R4]
    B.N L(OP_0F_MOVZX_GV_EW_WRITE)

L(OP_0F_MOVZX_GV_EW_MEM):
    BL L(DECODE_MODRM_32)
    SEAM_CHECK R4, R7
    UBFX R4, R7, #12, #12
    LDR.W R4, [R9, R4, LSL #2]
    ADDS R4, R4, R7
    MEM_TRAP_R R4
    LDRH R4, [R4, #0]

L(OP_0F_MOVZX_GV_EW_WRITE):
    UBFX R3, R1, #3, #3
    MOV.W R3, R3, LSL #2
    ADD.W R3, R3, #8
    STR.W R4, [R8, R3]
    B.W .fetch_decode_loop

OP .op_0f_movsx_gv_eb
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(OP_0F_MOVSX_GV_EB_MEM)
    AND.W R4, R1, #7
    AND.W R5, R4, #3
    MOV.W R4, R4, LSR #2
    ADD.W R4, R4, R5, LSL #2
    ADD.W R4, R4, #8
    LDRSB.W R4, [R8, R4]
    B.N L(OP_0F_MOVSX_GV_EB_WRITE)

L(OP_0F_MOVSX_GV_EB_MEM):
    BL L(DECODE_MODRM_32)
    UBFX R4, R7, #12, #12
    LDR.W R4, [R9, R4, LSL #2]
    ADDS R4, R4, R7
    MEM_TRAP_R R4
    LDRSB.W R4, [R4]

L(OP_0F_MOVSX_GV_EB_WRITE):
    UBFX R3, R1, #3, #3
    MOV.W R3, R3, LSL #2
    ADD.W R3, R3, #8
    STR.W R4, [R8, R3]
    B.W .fetch_decode_loop

OP .op_0f_movsx_gv_ew
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(OP_0F_MOVSX_GV_EW_MEM)
    AND.W R4, R1, #7
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDRSH.W R4, [R8, R4]
    B.N L(OP_0F_MOVSX_GV_EW_WRITE)

L(OP_0F_MOVSX_GV_EW_MEM):
    BL L(DECODE_MODRM_32)
    SEAM_CHECK R4, R7
    UBFX R4, R7, #12, #12
    LDR.W R4, [R9, R4, LSL #2]
    ADDS R4, R4, R7
    MEM_TRAP_R R4
    LDRSH.W R4, [R4]

L(OP_0F_MOVSX_GV_EW_WRITE):
    UBFX R3, R1, #3, #3
    MOV.W R3, R3, LSL #2
    ADD.W R3, R3, #8
    STR.W R4, [R8, R3]
    B.W .fetch_decode_loop

/* 0F AF /r -- IMUL Gv, Ev. The two-operand form: Gv = Gv * Ev, low half only.
 *
 * MEASURED on EPIC_PINBALL (docs/13), and it is the whole reason that title's
 * "same exit as the other two" reading was wrong -- with the exit naming the
 * opcode instead of the 0F escape, its exit_info reads 0x0FAF where Dark
 * Forces and Mortal Kombat read 0x0F01. Three titles, three different walls.
 *
 * Structurally .op_imul_imm32_69 with the immediate replaced by the
 * destination register's own current value, and it makes the same flag choice
 * the 69/6B forms already make: CF and OF are architecturally defined for
 * IMUL, and none of the three sets them. That is a KNOWN, SHARED gap for
 * docs/07 rather than something new here -- a guest that branches on the
 * overflow of a multiply is wrong on all three today, and fixing it is one
 * change across all three, not a rider on this one. */
OP .op_0f_imul_AF
    CODE_PTR R1
    LDRB R1, [R1, #0]
    ADD.W R10, R10, #1
    CMP R1, #192                   @ mod==3 iff modrm >= 0xC0
    BLO.N L(IMULAF_MEM)
    AND.W R2, R1, #7
    ADD.W R2, R8, R2, LSL #2
    ADD.W R2, R2, #8
    LD_V R3, R2
    B.N L(IMULAF_DO)
L(IMULAF_MEM):
    LDR.W R12, [R2, #REC_MODRM_FN] @ decoder for the ACTIVE address size
    BLX R12
    SEAM_CHECK R2, R7
    UBFX R2, R7, #12, #12
    LDR.W R2, [R9, R2, LSL #2]
    ADDS R2, R2, R7
    MEM_TRAP_R R2
    LD_V R3, R2
L(IMULAF_DO):
    UBFX R4, R1, #3, #3
    MOV.W R4, R4, LSL #2
    ADD.W R4, R4, #8
    LDREG_V R5, R4
    MUL.W R3, R3, R5
    STREG_V R3, R4
    B.W .fetch_decode_loop
#ifndef USE_16_BIT

.align 2
.set .op_0f_0F, .op_0f_0F_32
.set .op_0f_00, .op_0f_00_32
.set .op_0f_01, .op_0f_01_32
.set .op_0f_ba, .op_0f_ba_32
.set .op_int3_CC, .op_int3_CC_32
.set .op_into_CE, .op_into_CE_32
.set .op_int_imm8_CD, .op_int_imm8_CD_32
.set .op_iret_CF, .op_iret_CF_32
.set .op_0f_jcc_rel32, .op_0f_jcc_rel32_32
.set .op_0f_setcc, .op_0f_setcc_32
.set .op_0f_movzx_gv_eb, .op_0f_movzx_gv_eb_32
.set .op_0f_movzx_gv_ew, .op_0f_movzx_gv_ew_32
.set .op_0f_movsx_gv_eb, .op_0f_movsx_gv_eb_32
.set .op_0f_movsx_gv_ew, .op_0f_movsx_gv_ew_32
.set .op_0f_imul_AF, .op_0f_imul_AF_32
.set .op_alu_ev_gv, .op_alu_ev_gv_32
.set .op_alu_gv_ev, .op_alu_gv_ev_32
.set .op_alu_eax_imm32, .op_alu_eax_imm32_32
.set .op_alu_ev_imm32_81, .op_alu_ev_imm32_81_32
.set .op_alu_ev_imm8_83, .op_alu_ev_imm8_83_32
.set .op_mov_ev_gv_89, .op_mov_ev_gv_89_32
.set .op_mov_gv_ev_8B, .op_mov_gv_ev_8B_32
.set .op_mov_ev_imm32_C7, .op_mov_ev_imm32_C7_32
.set .op_mov_r32_imm32, .op_mov_r32_imm32_32
.set .op_mov_eax_moffs32_A1, .op_mov_eax_moffs32_A1_32
.set .op_mov_moffs32_eax_A3, .op_mov_moffs32_eax_A3_32
.set .op_push_r32, .op_push_r32_32
.set .op_pop_r32, .op_pop_r32_32
.set .op_push_imm32_68, .op_push_imm32_68_32
.set .op_push_imm8_6A, .op_push_imm8_6A_32
.set .op_pusha_60, .op_pusha_60_32
.set .op_popa_61, .op_popa_61_32
.set .op_pushfd_9C, .op_pushfd_9C_32
.set .op_popfd_9D, .op_popfd_9D_32
.set .op_pop_ev_8F, .op_pop_ev_8F_32
.set .op_inc_r32, .op_inc_r32_32
.set .op_dec_r32, .op_dec_r32_32
.set .op_lea_8D, .op_lea_8D_32
.set .op_grp3_ev_F7, .op_grp3_ev_F7_32
.set .op_grp5_FF, .op_grp5_FF_32
.set .op_call_rel32_E8, .op_call_rel32_E8_32
.set .op_jmp_rel32_E9, .op_jmp_rel32_E9_32
.set .op_ret_C3, .op_ret_C3_32
.set .op_ret_imm16_C2, .op_ret_imm16_C2_32
.set .op_leave_C9, .op_leave_C9_32
.set .op_enter_C8, .op_enter_C8_32
.set .op_movsd_A5, .op_movsd_A5_32
.set .op_cmpsd_A7, .op_cmpsd_A7_32
.set .op_lodsd_AD, .op_lodsd_AD_32
.set .op_cwde_98, .op_cwde_98_32
.set .op_cdq_99, .op_cdq_99_32
.set .op_imul_imm32_69, .op_imul_imm32_69_32
.set .op_imul_imm8_6B, .op_imul_imm8_6B_32
.set .op_test_ev_gv_85, .op_test_ev_gv_85_32
.set .op_test_eax_imm32_A9, .op_test_eax_imm32_A9_32
.set .op_shift_ev_imm8_C1, .op_shift_ev_imm8_C1_32
.set .op_shift_ev_1_D1, .op_shift_ev_1_D1_32
.set .op_shift_ev_cl_D3, .op_shift_ev_cl_D3_32
    .global opcode_table_32
.set .op_les_C4, .op_les_C4_32
.set .op_lds_C5, .op_lds_C5_32
.set .op_push_es_06, .op_push_es_06_32
.set .op_push_cs_0E, .op_push_cs_0E_32
.set .op_push_ss_16, .op_push_ss_16_32
.set .op_push_ds_1E, .op_push_ds_1E_32
.set .op_pop_es_07, .op_pop_es_07_32
.set .op_pop_ss_17, .op_pop_ss_17_32
.set .op_pop_ds_1F, .op_pop_ds_1F_32
.set .op_jmp_far_EA, .op_jmp_far_EA_32
.set .op_call_far_9A, .op_call_far_9A_32
.set .op_retf_CB, .op_retf_CB_32
.set .op_retf_imm16_CA, .op_retf_imm16_CA_32
.set .op_xchg_eax_r32, .op_xchg_eax_r32_32
.set .op_xchg_ev_gv_87, .op_xchg_ev_gv_87_32
.set .op_stosd_AB, .op_stosd_AB_32
.set .op_scasd_AF, .op_scasd_AF_32
.set .op_in_eax_dx_ED, .op_in_eax_dx_ED_32
.set .op_in_eax_imm8_E5, .op_in_eax_imm8_E5_32
.set .op_out_dx_eax_EF, .op_out_dx_eax_EF_32
.set .op_out_imm8_eax_E7, .op_out_imm8_eax_E7_32
.set .op_insv_6D, .op_insv_6D_32
.set .op_outsv_6F, .op_outsv_6F_32
opcode_table_32:
#include "opcode_table.S"

.align 2
    .global opcode_table_0f_32
opcode_table_0f_32:
#include "opcode_0f_table.S"

#else

.align 2
.set .op_0f_0F, .op_0f_0F_16
.set .op_0f_00, .op_0f_00_16
.set .op_0f_01, .op_0f_01_16
.set .op_0f_ba, .op_0f_ba_16
.set .op_int3_CC, .op_int3_CC_16
.set .op_into_CE, .op_into_CE_16
.set .op_int_imm8_CD, .op_int_imm8_CD_16
.set .op_iret_CF, .op_iret_CF_16
.set .op_0f_jcc_rel32, .op_0f_jcc_rel32_16
.set .op_0f_setcc, .op_0f_setcc_16
.set .op_0f_movzx_gv_eb, .op_0f_movzx_gv_eb_16
.set .op_0f_movzx_gv_ew, .op_0f_movzx_gv_ew_16
.set .op_0f_movsx_gv_eb, .op_0f_movsx_gv_eb_16
.set .op_0f_movsx_gv_ew, .op_0f_movsx_gv_ew_16
.set .op_0f_imul_AF, .op_0f_imul_AF_16
.set .op_alu_ev_gv, .op_alu_ev_gv_16
.set .op_alu_gv_ev, .op_alu_gv_ev_16
.set .op_alu_eax_imm32, .op_alu_eax_imm32_16
.set .op_alu_ev_imm32_81, .op_alu_ev_imm32_81_16
.set .op_alu_ev_imm8_83, .op_alu_ev_imm8_83_16
.set .op_mov_ev_gv_89, .op_mov_ev_gv_89_16
.set .op_mov_gv_ev_8B, .op_mov_gv_ev_8B_16
.set .op_mov_ev_imm32_C7, .op_mov_ev_imm32_C7_16
.set .op_mov_r32_imm32, .op_mov_r32_imm32_16
.set .op_mov_eax_moffs32_A1, .op_mov_eax_moffs32_A1_16
.set .op_mov_moffs32_eax_A3, .op_mov_moffs32_eax_A3_16
.set .op_push_r32, .op_push_r32_16
.set .op_pop_r32, .op_pop_r32_16
.set .op_push_imm32_68, .op_push_imm32_68_16
.set .op_push_imm8_6A, .op_push_imm8_6A_16
.set .op_pusha_60, .op_pusha_60_16
.set .op_popa_61, .op_popa_61_16
.set .op_pushfd_9C, .op_pushfd_9C_16
.set .op_popfd_9D, .op_popfd_9D_16
.set .op_pop_ev_8F, .op_pop_ev_8F_16
.set .op_inc_r32, .op_inc_r32_16
.set .op_dec_r32, .op_dec_r32_16
.set .op_lea_8D, .op_lea_8D_16
.set .op_grp3_ev_F7, .op_grp3_ev_F7_16
.set .op_grp5_FF, .op_grp5_FF_16
.set .op_call_rel32_E8, .op_call_rel32_E8_16
.set .op_jmp_rel32_E9, .op_jmp_rel32_E9_16
.set .op_ret_C3, .op_ret_C3_16
.set .op_ret_imm16_C2, .op_ret_imm16_C2_16
.set .op_leave_C9, .op_leave_C9_16
.set .op_enter_C8, .op_enter_C8_16
.set .op_movsd_A5, .op_movsd_A5_16
.set .op_cmpsd_A7, .op_cmpsd_A7_16
.set .op_lodsd_AD, .op_lodsd_AD_16
.set .op_cwde_98, .op_cwde_98_16
.set .op_cdq_99, .op_cdq_99_16
.set .op_imul_imm32_69, .op_imul_imm32_69_16
.set .op_imul_imm8_6B, .op_imul_imm8_6B_16
.set .op_test_ev_gv_85, .op_test_ev_gv_85_16
.set .op_test_eax_imm32_A9, .op_test_eax_imm32_A9_16
.set .op_shift_ev_imm8_C1, .op_shift_ev_imm8_C1_16
.set .op_shift_ev_1_D1, .op_shift_ev_1_D1_16
.set .op_shift_ev_cl_D3, .op_shift_ev_cl_D3_16
    .global opcode_table_16
.set .op_les_C4, .op_les_C4_16
.set .op_lds_C5, .op_lds_C5_16
.set .op_push_es_06, .op_push_es_06_16
.set .op_push_cs_0E, .op_push_cs_0E_16
.set .op_push_ss_16, .op_push_ss_16_16
.set .op_push_ds_1E, .op_push_ds_1E_16
.set .op_pop_es_07, .op_pop_es_07_16
.set .op_pop_ss_17, .op_pop_ss_17_16
.set .op_pop_ds_1F, .op_pop_ds_1F_16
.set .op_jmp_far_EA, .op_jmp_far_EA_16
.set .op_call_far_9A, .op_call_far_9A_16
.set .op_retf_CB, .op_retf_CB_16
.set .op_retf_imm16_CA, .op_retf_imm16_CA_16
.set .op_xchg_eax_r32, .op_xchg_eax_r32_16
.set .op_xchg_ev_gv_87, .op_xchg_ev_gv_87_16
.set .op_stosd_AB, .op_stosd_AB_16
.set .op_scasd_AF, .op_scasd_AF_16
.set .op_in_eax_dx_ED, .op_in_eax_dx_ED_16
.set .op_in_eax_imm8_E5, .op_in_eax_imm8_E5_16
.set .op_out_dx_eax_EF, .op_out_dx_eax_EF_16
.set .op_out_imm8_eax_E7, .op_out_imm8_eax_E7_16
.set .op_insv_6D, .op_insv_6D_16
.set .op_outsv_6F, .op_outsv_6F_16
opcode_table_16:
#include "opcode_table.S"

.align 2
    .global opcode_table_0f_16
opcode_table_0f_16:
#include "opcode_0f_table.S"

#endif
