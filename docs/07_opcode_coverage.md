# 07 — x86 Opcode Coverage Audit

Read-only audit of the hand-written Thumb-2 core as of the working tree at the time of
writing (`opcode_table.S`, `opcode_0f_table.S`, `cpu_exec_handlers.S`,
`cpu_exec_handlers_0f.S`, `cpu_exec.S`). Every entry below was produced by parsing the
two dispatch tables programmatically and then reading the body of each named handler.
Nothing here is inferred from a handler's name.

Target class is **386 real mode**. 486/Pentium+ instructions are listed as *out of
scope* rather than as gaps.

## Hard numbers

| Metric | Count |
|---|---|
| One-byte slots with a handler | **235 / 256** |
| One-byte slots dispatching to `.exit_loop` | **21 / 256** |
| Two-byte (0F) slots with a handler | **38 / 256** |
| Two-byte slots dispatching to `.exit_loop` | **218 / 256** (of which 158 are 486/Pentium+ and out of scope) |
| Group `/reg` sub-opcode slots audited | **122** |
| Group `/reg` slots reaching real code | **104** |
| Group `/reg` slots missing or stubbed | **18** |

### Defects by category (implemented but wrong — these fail *silently*)

| Category | Count of affected opcode slots | Examples |
|---|---|---|
| Missing flags entirely | 13 | `84`, `A8`, `A9`, `F6 /0`, `FE /0 /1`, `FF /0 /1`, `69`, `6B`, `D4`, `D5` |
| Silently 32-bit in 16-bit mode (`OP_AGNOSTIC` on a width-sensitive body, or bare `LDR`/`STR`/`#4`) | 21 | `87`, `91`–`97`, `98`, `99`, `8D`, `A1`, `CA`, `C1`, `D1`, `D3`, `0F 80`–`0F 8F` |
| Stack accessed as a raw linear address (no `SS.base`, hard-coded ±4) | 13 | `60`, `61`, `68`, `6A`, `8F`, `9C`, `9D`, `C2`, `C8`, `C9`, `CA`, `FF /2`, `FF /6` |
| Memory ModRM form missing (register form only) | 6 | `85`, `86`, `87`, `8F`, `C6`, `C7` |
| Non-stack memory operand used without a segment base | 5 | `A0`, `A1`, `A2`, `A3`, `D7` |
| Address-size axis ignored (uses full `ECX`/32-bit moffs) | 6 | `E0`, `E1`, `E2`, `E3`, `A0`, `A2` |
| CS.base mishandled on a control transfer | 4 | `C2`, `CA`, `FF /2`, `FF /4` |
| Implemented as a silent no-op | 1 | `84` (TEST). `0F 01 /2` LGDT is no longer a no-op. |

Slots can appear in more than one row. Counting distinct one-byte slots, **43 of the 235
implemented opcodes have at least one defect** and **7 more are partial** (register form
only). That leaves roughly **185 one-byte opcodes believed correct**.

## How the two operand-size passes work (and how to read the "dual/AGN" column)

`cpu_exec.S` includes `cpu_exec_handlers.S` **twice** — once with `OPSZ 4 / NSH 0` and
once with `OPSZ 2 / NSH 16`. A handler declared `OP` is emitted in both passes with a
`_32` / `_16` suffix and each dispatch table points at its own copy. A handler declared
`OP_AGNOSTIC` is emitted **once** and shared by both tables.

Therefore: **any width-sensitive instruction declared `OP_AGNOSTIC` is 32-bit in both
modes.** Every table row below carries the handler symbol and `(dual)` or `(AGN)` so this
is checkable at a glance. Cross-checking the `.set` alias list in
`cpu_exec_handlers_0f.S` confirms every `OP`-declared handler has both a `_32` and a
`_16` alias — there are no dangling aliases and no handler used by a table that is
undefined.

The width-correct primitives are `LD_V/ST_V/LDS_V`, `LDREG_V/STREG_V`, `EIP_ADV_V`,
`EIP_WRAP`, `PUSH_V/POP_V`, `SP_DEC/SP_INC/SP_WRAP`, `OPSZ`, `NSH`, `VNORM/VDENORM/
VMASK/VSEXT/VTEST`. A dual-emitted handler that still uses a bare `LDR`/`STR`/`#4` is
flagged below.

`PUSH_V`/`POP_V` are the only stack primitives that apply `SS.base` and wrap SP at the
stack width. Thirteen handlers still open-code the stack with `LDR R4,[R8,#24]` /
`SUB #4` and are listed as defects.

---

## One-byte opcodes — all 256

| Op | Mnemonic | Status | Handler | Notes |
|---|---|---|---|---|
| 00 | ADD Eb,Gb | implemented | `.op_alu_eb_gb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 01 | ADD Ev,Gv | implemented | `.op_alu_ev_gv` (dual) | Dual-emitted (OP); LD_V/LDREG_V/ST_V throughout. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 02 | ADD Gb,Eb | implemented | `.op_alu_gb_eb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 03 | ADD Gv,Ev | implemented | `.op_alu_gv_ev` (dual) | Dual-emitted; width-correct. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 04 | ADD AL,Ib | implemented | `.op_alu_al_imm8` (AGN) | 8-bit immediate; full flags. |
| 05 | ADD eAX,Iv | implemented | `.op_alu_eax_imm32` (dual) | Dual-emitted; immediate read with LD_V + EIP_ADV_V, so imm16 in 16-bit mode. Full flags. |
| 06 | PUSH ES | implemented | `.op_push_es_06` (dual) | Dual-emitted; PUSH_V, so SS.base + width-correct SP. Selector zero-extended to the operand size. |
| 07 | POP ES | implemented | `.op_pop_es_07` (dual) | Dual-emitted; POP_V + SET_SREG (real mode only; #GP-free trap to host if CR0.PE=1). |
| 08 | OR Eb,Gb | implemented | `.op_alu_eb_gb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 09 | OR Ev,Gv | implemented | `.op_alu_ev_gv` (dual) | Dual-emitted (OP); LD_V/LDREG_V/ST_V throughout. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 0A | OR Gb,Eb | implemented | `.op_alu_gb_eb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 0B | OR Gv,Ev | implemented | `.op_alu_gv_ev` (dual) | Dual-emitted; width-correct. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 0C | OR AL,Ib | implemented | `.op_alu_al_imm8` (AGN) | 8-bit immediate; full flags. |
| 0D | OR eAX,Iv | implemented | `.op_alu_eax_imm32` (dual) | Dual-emitted; immediate read with LD_V + EIP_ADV_V, so imm16 in 16-bit mode. Full flags. |
| 0E | PUSH CS | implemented | `.op_push_cs_0E` (dual) | Dual-emitted; PUSH_V, so SS.base + width-correct SP. Selector zero-extended to the operand size. |
| 0F | two-byte escape 0F | implemented | `.op_0f_0F` (dual) | Escape: loads the 0F table from the mode record and re-dispatches. Dual-emitted so the 0F table follows operand size. |
| 10 | ADC Eb,Gb | implemented | `.op_alu_eb_gb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 11 | ADC Ev,Gv | implemented | `.op_alu_ev_gv` (dual) | Dual-emitted (OP); LD_V/LDREG_V/ST_V throughout. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 12 | ADC Gb,Eb | implemented | `.op_alu_gb_eb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 13 | ADC Gv,Ev | implemented | `.op_alu_gv_ev` (dual) | Dual-emitted; width-correct. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 14 | ADC AL,Ib | implemented | `.op_alu_al_imm8` (AGN) | 8-bit immediate; full flags. |
| 15 | ADC eAX,Iv | implemented | `.op_alu_eax_imm32` (dual) | Dual-emitted; immediate read with LD_V + EIP_ADV_V, so imm16 in 16-bit mode. Full flags. |
| 16 | PUSH SS | implemented | `.op_push_ss_16` (dual) | Dual-emitted; PUSH_V, so SS.base + width-correct SP. Selector zero-extended to the operand size. |
| 17 | POP SS | implemented | `.op_pop_ss_17` (dual) | Dual-emitted; POP_V + SET_SREG (real mode only; #GP-free trap to host if CR0.PE=1). POP SS does NOT inhibit interrupts for the following instruction. |
| 18 | SBB Eb,Gb | implemented | `.op_alu_eb_gb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 19 | SBB Ev,Gv | implemented | `.op_alu_ev_gv` (dual) | Dual-emitted (OP); LD_V/LDREG_V/ST_V throughout. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 1A | SBB Gb,Eb | implemented | `.op_alu_gb_eb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 1B | SBB Gv,Ev | implemented | `.op_alu_gv_ev` (dual) | Dual-emitted; width-correct. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 1C | SBB AL,Ib | implemented | `.op_alu_al_imm8` (AGN) | 8-bit immediate; full flags. |
| 1D | SBB eAX,Iv | implemented | `.op_alu_eax_imm32` (dual) | Dual-emitted; immediate read with LD_V + EIP_ADV_V, so imm16 in 16-bit mode. Full flags. |
| 1E | PUSH DS | implemented | `.op_push_ds_1E` (dual) | Dual-emitted; PUSH_V, so SS.base + width-correct SP. Selector zero-extended to the operand size. |
| 1F | POP DS | implemented | `.op_pop_ds_1F` (dual) | Dual-emitted; POP_V + SET_SREG (real mode only; #GP-free trap to host if CR0.PE=1). |
| 20 | AND Eb,Gb | implemented | `.op_alu_eb_gb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 21 | AND Ev,Gv | implemented | `.op_alu_ev_gv` (dual) | Dual-emitted (OP); LD_V/LDREG_V/ST_V throughout. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 22 | AND Gb,Eb | implemented | `.op_alu_gb_eb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 23 | AND Gv,Ev | implemented | `.op_alu_gv_ev` (dual) | Dual-emitted; width-correct. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 24 | AND AL,Ib | implemented | `.op_alu_al_imm8` (AGN) | 8-bit immediate; full flags. |
| 25 | AND eAX,Iv | implemented | `.op_alu_eax_imm32` (dual) | Dual-emitted; immediate read with LD_V + EIP_ADV_V, so imm16 in 16-bit mode. Full flags. |
| 26 | ES: prefix | implemented | `.op_seg_es_26` (AGN) | Points R6 at the descriptor and re-enters at .fetch_next_byte_no_reset, so the override lives exactly one instruction. No PIT double-decrement. |
| 27 | DAA | unimplemented | `.exit_loop` | BCD adjust. Not implemented; exits to host as UNIMPLEMENTED. |
| 28 | SUB Eb,Gb | implemented | `.op_alu_eb_gb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 29 | SUB Ev,Gv | implemented | `.op_alu_ev_gv` (dual) | Dual-emitted (OP); LD_V/LDREG_V/ST_V throughout. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 2A | SUB Gb,Eb | implemented | `.op_alu_gb_eb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 2B | SUB Gv,Ev | implemented | `.op_alu_gv_ev` (dual) | Dual-emitted; width-correct. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 2C | SUB AL,Ib | implemented | `.op_alu_al_imm8` (AGN) | 8-bit immediate; full flags. |
| 2D | SUB eAX,Iv | implemented | `.op_alu_eax_imm32` (dual) | Dual-emitted; immediate read with LD_V + EIP_ADV_V, so imm16 in 16-bit mode. Full flags. |
| 2E | CS: prefix | implemented | `.op_seg_cs_2E` (AGN) | Points R6 at the descriptor and re-enters at .fetch_next_byte_no_reset, so the override lives exactly one instruction. No PIT double-decrement. |
| 2F | DAS | unimplemented | `.exit_loop` | BCD adjust. Not implemented; exits to host as UNIMPLEMENTED. |
| 30 | XOR Eb,Gb | implemented | `.op_alu_eb_gb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 31 | XOR Ev,Gv | implemented | `.op_alu_ev_gv` (dual) | Dual-emitted (OP); LD_V/LDREG_V/ST_V throughout. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 32 | XOR Gb,Eb | implemented | `.op_alu_gb_eb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 33 | XOR Gv,Ev | implemented | `.op_alu_gv_ev` (dual) | Dual-emitted; width-correct. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 34 | XOR AL,Ib | implemented | `.op_alu_al_imm8` (AGN) | 8-bit immediate; full flags. |
| 35 | XOR eAX,Iv | implemented | `.op_alu_eax_imm32` (dual) | Dual-emitted; immediate read with LD_V + EIP_ADV_V, so imm16 in 16-bit mode. Full flags. |
| 36 | SS: prefix | implemented | `.op_seg_ss_36` (AGN) | Points R6 at the descriptor and re-enters at .fetch_next_byte_no_reset, so the override lives exactly one instruction. No PIT double-decrement. |
| 37 | AAA | unimplemented | `.exit_loop` | BCD adjust. Not implemented; exits to host as UNIMPLEMENTED. |
| 38 | CMP Eb,Gb | implemented | `.op_alu_eb_gb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 39 | CMP Ev,Gv | implemented | `.op_alu_ev_gv` (dual) | Dual-emitted (OP); LD_V/LDREG_V/ST_V throughout. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 3A | CMP Gb,Eb | implemented | `.op_alu_gb_eb` (AGN) | 8-bit, OP_AGNOSTIC and correctly so. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 3B | CMP Gv,Ev | implemented | `.op_alu_gv_ev` (dual) | Dual-emitted; width-correct. reg+mem forms; full flags via the ALU8/ALU cores; memory path uses the mode record ModRM decoder, so the segment base is applied. |
| 3C | CMP AL,Ib | implemented | `.op_alu_al_imm8` (AGN) | 8-bit immediate; full flags. |
| 3D | CMP eAX,Iv | implemented | `.op_alu_eax_imm32` (dual) | Dual-emitted; immediate read with LD_V + EIP_ADV_V, so imm16 in 16-bit mode. Full flags. |
| 3E | DS: prefix | implemented | `.op_seg_ds_3E` (AGN) | Points R6 at the descriptor and re-enters at .fetch_next_byte_no_reset, so the override lives exactly one instruction. No PIT double-decrement. |
| 3F | AAS | unimplemented | `.exit_loop` | BCD adjust. Not implemented; exits to host as UNIMPLEMENTED. |
| 40 | INC eAX | implemented | `.op_inc_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 41 | INC eCX | implemented | `.op_inc_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 42 | INC eDX | implemented | `.op_inc_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 43 | INC eBX | implemented | `.op_inc_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 44 | INC eSP | implemented | `.op_inc_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 45 | INC eBP | implemented | `.op_inc_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 46 | INC eSI | implemented | `.op_inc_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 47 | INC eDI | implemented | `.op_inc_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 48 | DEC eAX | implemented | `.op_dec_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 49 | DEC eCX | implemented | `.op_dec_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 4A | DEC eDX | implemented | `.op_dec_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 4B | DEC eBX | implemented | `.op_dec_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 4C | DEC eSP | implemented | `.op_dec_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 4D | DEC eBP | implemented | `.op_dec_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 4E | DEC eSI | implemented | `.op_dec_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 4F | DEC eDI | implemented | `.op_dec_r32` (dual) | Dual-emitted; LDREG_V/STREG_V; X86_FLAGS_INCDEC leaves CF alone and sets OF/SF/ZF/AF/PF. |
| 50 | PUSH eAX | implemented | `.op_push_r32` (dual) | Dual-emitted; PUSH_V. PUSH eSP pushes the pre-decrement value, as the ISA requires. |
| 51 | PUSH eCX | implemented | `.op_push_r32` (dual) | Dual-emitted; PUSH_V. PUSH eSP pushes the pre-decrement value, as the ISA requires. |
| 52 | PUSH eDX | implemented | `.op_push_r32` (dual) | Dual-emitted; PUSH_V. PUSH eSP pushes the pre-decrement value, as the ISA requires. |
| 53 | PUSH eBX | implemented | `.op_push_r32` (dual) | Dual-emitted; PUSH_V. PUSH eSP pushes the pre-decrement value, as the ISA requires. |
| 54 | PUSH eSP | implemented | `.op_push_r32` (dual) | Dual-emitted; PUSH_V. PUSH eSP pushes the pre-decrement value, as the ISA requires. |
| 55 | PUSH eBP | implemented | `.op_push_r32` (dual) | Dual-emitted; PUSH_V. PUSH eSP pushes the pre-decrement value, as the ISA requires. |
| 56 | PUSH eSI | implemented | `.op_push_r32` (dual) | Dual-emitted; PUSH_V. PUSH eSP pushes the pre-decrement value, as the ISA requires. |
| 57 | PUSH eDI | implemented | `.op_push_r32` (dual) | Dual-emitted; PUSH_V. PUSH eSP pushes the pre-decrement value, as the ISA requires. |
| 58 | POP eAX | implemented | `.op_pop_r32` (dual) | Dual-emitted; POP_V + STREG_V (16-bit pop preserves the high half of the 32-bit reg). |
| 59 | POP eCX | implemented | `.op_pop_r32` (dual) | Dual-emitted; POP_V + STREG_V (16-bit pop preserves the high half of the 32-bit reg). |
| 5A | POP eDX | implemented | `.op_pop_r32` (dual) | Dual-emitted; POP_V + STREG_V (16-bit pop preserves the high half of the 32-bit reg). |
| 5B | POP eBX | implemented | `.op_pop_r32` (dual) | Dual-emitted; POP_V + STREG_V (16-bit pop preserves the high half of the 32-bit reg). |
| 5C | POP eSP | implemented | `.op_pop_r32` (dual) | Dual-emitted; POP_V + STREG_V (16-bit pop preserves the high half of the 32-bit reg). |
| 5D | POP eBP | implemented | `.op_pop_r32` (dual) | Dual-emitted; POP_V + STREG_V (16-bit pop preserves the high half of the 32-bit reg). |
| 5E | POP eSI | implemented | `.op_pop_r32` (dual) | Dual-emitted; POP_V + STREG_V (16-bit pop preserves the high half of the 32-bit reg). |
| 5F | POP eDI | implemented | `.op_pop_r32` (dual) | Dual-emitted; POP_V + STREG_V (16-bit pop preserves the high half of the 32-bit reg). |
| 60 | PUSHA/PUSHAD | DEFECT | `.op_pusha_60` (dual) | PUSHA: raw ESP used as a linear address (no SS.base) and a hard-coded 4-byte stride, while the value is stored with ST_V. In the 16-bit pass it therefore strides 4 bytes but writes 2 -- every other slot is garbage - and it breaks entirely when SS.base != 0. |
| 61 | POPA/POPAD | DEFECT | `.op_popa_61` (dual) | POPA: same raw-ESP/no-SS.base and hard-coded +4 stride as PUSHA; additionally writes each register back with a full 32-bit STR.W, clobbering the high halves in 16-bit mode. ESP slot correctly skipped. |
| 62 | BOUND Gv,Ma | unimplemented | `.exit_loop` | BOUND. Not implemented. |
| 63 | ARPL Ew,Gw | unimplemented | `.exit_loop` | ARPL. Protected mode only; not implemented. |
| 64 | FS: prefix | implemented | `.op_seg_fs_64` (AGN) | Points R6 at the descriptor and re-enters at .fetch_next_byte_no_reset, so the override lives exactly one instruction. No PIT double-decrement. |
| 65 | GS: prefix | implemented | `.op_seg_gs_65` (AGN) | Points R6 at the descriptor and re-enters at .fetch_next_byte_no_reset, so the override lives exactly one instruction. No PIT double-decrement. |
| 66 | operand-size prefix | implemented | `.op_pfx_66` (AGN) | Single EOR of the mode record with OPSIZE_XOR, then re-enter the fetch below the reset. Applying it twice cancels, as on hardware. |
| 67 | address-size prefix | implemented | `.op_pfx_67` (AGN) | Single EOR with ADDRSIZE_XOR. Independent of 66. Note: handlers that call the ModRM decoder by symbol rather than through REC_MODRM_FN ignore this prefix -- see the defects list. |
| 68 | PUSH Iv | DEFECT | `.op_push_imm32_68` (dual) | PUSH Iv: immediate is width-correct (LD_V/EIP_ADV_V) but the push is open-coded with raw ESP, no SS.base and a hard-coded -4. |
| 69 | IMUL Gv,Ev,Iv | DEFECT | `.op_imul_imm32_69` (dual) | IMUL Gv,Ev,Iv: reg and mem forms present and width-correct, but NO FLAGS are written. CF/OF must be set when the result does not fit the destination; software uses JC/JO after IMUL to detect overflow. |
| 6A | PUSH Ib | DEFECT | `.op_push_imm8_6A` (dual) | PUSH Ib: sign-extended immediate is correct; the push is open-coded with raw ESP, no SS.base and a hard-coded -4. |
| 6B | IMUL Gv,Ev,Ib | DEFECT | `.op_imul_imm8_6B` (dual) | IMUL Gv,Ev,Ib: as 0x69 -- no CF/OF. |
| 6C | INSB | unimplemented | `.exit_loop` | String port I/O. Not implemented. |
| 6D | INSW/INSD | unimplemented | `.exit_loop` | String port I/O. Not implemented. |
| 6E | OUTSB | unimplemented | `.exit_loop` | String port I/O. Not implemented. |
| 6F | OUTSW/OUTSD | unimplemented | `.exit_loop` | String port I/O. Not implemented. |
| 70 | JO Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 71 | JNO Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 72 | JB Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 73 | JNB Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 74 | JZ Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 75 | JNZ Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 76 | JBE Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 77 | JNBE Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 78 | JS Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 79 | JNS Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 7A | JP Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 7B | JNP Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 7C | JL Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 7D | JNL Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 7E | JLE Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 7F | JNLE Jb | implemented | `.op_jcc_rel8_7X` (AGN) | OP_AGNOSTIC, correct (rel8 is byte-sized in both modes). EVAL_COND covers all 16 conditions. Minor: no EIP_WRAP, so a taken branch that crosses 0xFFFF in a 16-bit segment does not wrap. |
| 80 | Grp1 Eb,Ib | implemented | `.op_alu_eb_imm8_80` (AGN) | Grp1 Eb,Ib: all eight /reg implemented, reg+mem forms, full flags. See the group table. |
| 81 | Grp1 Ev,Iv | implemented | `.op_alu_ev_imm32_81` (dual) | Grp1 Ev,Iv: all eight /reg, reg+mem, dual-emitted, immediate follows operand size. |
| 82 | Grp1 Eb,Ib (alias of 80) | implemented | `.op_alu_eb_imm8_80` (AGN) | Aliased to the 0x80 handler, which is the correct 386 behaviour. |
| 83 | Grp1 Ev,Ib | implemented | `.op_alu_ev_imm8_83` (dual) | Grp1 Ev,Ib: all eight /reg, reg+mem, immediate sign-extended with LDRSB. |
| 84 | TEST Eb,Gb | DEFECT | `.op_test_eb_gb_84` (AGN) | TEST Eb,Gb: the register form fetches the ModRM and then falls straight through to the next instruction -- no AND, NO FLAGS AT ALL. The memory form exits as unimplemented. This is a silent wrong-answer on one of the most common instructions in DOS code. |
| 85 | TEST Ev,Gv | partial | `.op_test_ev_gv_85` (dual) | TEST Ev,Gv: register form correct with full logic flags; the MEMORY form goes to .exit_loop. |
| 86 | XCHG Eb,Gb | partial | `.op_xchg_eb_gb_86` (AGN) | XCHG Eb,Gb: register form only; memory form exits. |
| 87 | XCHG Ev,Gv | DEFECT | `.op_xchg_ev_gv_87` (AGN) | XCHG Ev,Gv: register form only (memory exits), AND declared OP_AGNOSTIC with bare LDR.W/STR.W -- so it is a 32-bit exchange in 16-bit mode. |
| 88 | MOV Eb,Gb | implemented | `.op_mov_eb_gb_88` (AGN) | Both reg and mem forms; byte-sized so OP_AGNOSTIC is correct; mem path via the decoder (segment base applied). |
| 89 | MOV Ev,Gv | implemented | `.op_mov_ev_gv_89` (dual) | Dual-emitted, both forms, LDREG_V/ST_V. Source register deliberately read after the decode call. |
| 8A | MOV Gb,Eb | implemented | `.op_mov_gb_eb_8A` (AGN) | Both forms; byte-sized; correct. |
| 8B | MOV Gv,Ev | implemented | `.op_mov_gv_ev_8B` (dual) | Dual-emitted, both forms, LD_V/STREG_V. |
| 8C | MOV Ew,Sw | implemented | `.op_mov_Ew_seg_8C` (AGN) | MOV Ew,Sreg: always a 16-bit store, both forms, correct. |
| 8D | LEA Gv,M | DEFECT | `.op_lea_8D` (dual) | LEA Gv,M: register ModRM correctly rejected. But the address is written with a bare `STR R7,[R4]` instead of STREG_V, so in 16-bit mode LEA writes a 32-bit value into the destination register instead of 16 bits. |
| 8E | MOV Sw,Ew | implemented | `.op_mov_seg_Ew_8E` (AGN) | MOV Sreg,Ew: both forms; goes through SET_SREG (real-mode base = sel<<4). Traps to host if CR0.PE=1. |
| 8F | POP Ev | DEFECT | `.op_pop_ev_8F` (dual) | POP Ev: register form only (memory exits); pop is open-coded with raw ESP as a linear address (no SS.base) and a hard-coded +4, so it is wrong in 16-bit mode and wrong whenever SS.base != 0. |
| 90 | NOP (XCHG eAX,eAX) | implemented | `.op_nop` (AGN) | NOP; OP_AGNOSTIC, correct. |
| 91 | XCHG eAX,eCX | DEFECT | `.op_xchg_eax_r32` (AGN) | XCHG eAX,r: OP_AGNOSTIC with bare LDR.W/STR.W -- always a 32-bit exchange, so `XCHG AX,BX` in 16-bit mode also swaps the high halves of EAX/EBX. |
| 92 | XCHG eAX,eDX | DEFECT | `.op_xchg_eax_r32` (AGN) | XCHG eAX,r: OP_AGNOSTIC with bare LDR.W/STR.W -- always a 32-bit exchange, so `XCHG AX,BX` in 16-bit mode also swaps the high halves of EAX/EBX. |
| 93 | XCHG eAX,eBX | DEFECT | `.op_xchg_eax_r32` (AGN) | XCHG eAX,r: OP_AGNOSTIC with bare LDR.W/STR.W -- always a 32-bit exchange, so `XCHG AX,BX` in 16-bit mode also swaps the high halves of EAX/EBX. |
| 94 | XCHG eAX,eSP | DEFECT | `.op_xchg_eax_r32` (AGN) | XCHG eAX,r: OP_AGNOSTIC with bare LDR.W/STR.W -- always a 32-bit exchange, so `XCHG AX,BX` in 16-bit mode also swaps the high halves of EAX/EBX. |
| 95 | XCHG eAX,eBP | DEFECT | `.op_xchg_eax_r32` (AGN) | XCHG eAX,r: OP_AGNOSTIC with bare LDR.W/STR.W -- always a 32-bit exchange, so `XCHG AX,BX` in 16-bit mode also swaps the high halves of EAX/EBX. |
| 96 | XCHG eAX,eSI | DEFECT | `.op_xchg_eax_r32` (AGN) | XCHG eAX,r: OP_AGNOSTIC with bare LDR.W/STR.W -- always a 32-bit exchange, so `XCHG AX,BX` in 16-bit mode also swaps the high halves of EAX/EBX. |
| 97 | XCHG eAX,eDI | DEFECT | `.op_xchg_eax_r32` (AGN) | XCHG eAX,r: OP_AGNOSTIC with bare LDR.W/STR.W -- always a 32-bit exchange, so `XCHG AX,BX` in 16-bit mode also swaps the high halves of EAX/EBX. |
| 98 | CBW/CWDE | DEFECT | `.op_cwde_98` (dual) | CBW/CWDE: dual-emitted but the body is fixed -- SXTH of EAX then a full 32-bit store. In the 16-bit pass 0x98 must be CBW (AL->AX); it executes CWDE instead, and clobbers the high half of EAX. |
| 99 | CWD/CDQ | DEFECT | `.op_cdq_99` (dual) | CWD/CDQ: dual-emitted but the body is fixed -- ASR #31 of the full EAX into a full 32-bit EDX. In the 16-bit pass 0x99 must be CWD (sign of AX into DX only). |
| 9A | CALL FAR Ap | implemented | `.op_call_far_9A` (dual) | CALL FAR ptr16:v: offset at operand size, selector 16-bit, return CS:IP pushed with PUSH_V, CS installed via SET_SREG, EIP rebuilt from the new base. Real mode only. |
| 9B | WAIT/FWAIT | unimplemented | `.exit_loop` | WAIT/FWAIT. Not implemented; would be safe as a NOP. |
| 9C | PUSHF/PUSHFD | DEFECT | `.op_pushfd_9C` (dual) | PUSHF/PUSHFD: value stored with ST_V (width-correct) but the stack pointer is raw ESP with a hard-coded -4 and no SS.base. Mismatched stride vs store width in 16-bit mode. |
| 9D | POPF/POPFD | DEFECT | `.op_popfd_9D` (dual) | POPF/POPFD: raw ESP, no SS.base, full 32-bit LDR and a hard-coded +4. Also loads EFLAGS wholesale with no mask, so a guest can set reserved/IOPL/VM bits directly. |
| 9E | SAHF | implemented | `.op_sahf_9E` (AGN) | SAHF: BFI of AH into EFLAGS[7:0]. |
| 9F | LAHF | implemented | `.op_lahf_9F` (AGN) | LAHF. |
| A0 | MOV AL,Ob | DEFECT | `.op_mov_al_moffs8_A0` (AGN) | MOV AL,Ob: the moffs is read as a fixed 32-bit value (address size ignored -- it must be 16-bit under 16-bit addressing) and is used as a LINEAR address with no DS base and no segment override support. |
| A1 | MOV eAX,Ov | DEFECT | `.op_mov_eax_moffs32_A1` (dual) | MOV eAX,Ov: moffs read with LD_V, i.e. it follows OPERAND size when it must follow ADDRESS size; no DS base/override; and the destination is written with a full 32-bit STR.W, clobbering the high half of EAX in 16-bit mode. |
| A2 | MOV Ob,AL | DEFECT | `.op_mov_moffs8_al_A2` (AGN) | MOV Ob,AL: as 0xA0 -- fixed 32-bit moffs, no DS base, no override. |
| A3 | MOV Ov,eAX | DEFECT | `.op_mov_moffs32_eax_A3` (dual) | MOV Ov,eAX: moffs follows operand size instead of address size; no DS base/override. |
| A4 | MOVSB | implemented | `.op_movsb_A4` (AGN) | MOVSB: STR_SRC applies DS (or an override) + address-size wrap; STR_DST applies ES and correctly ignores overrides. DF-driven delta. Repeats via the REP machinery. |
| A5 | MOVSW/MOVSD | implemented | `.op_movsd_A5` (dual) | MOVSW/MOVSD: dual-emitted, LD_V/ST_V, delta = OPSZ. Segments and wrap as A4. |
| A6 | CMPSB | implemented | `.op_cmpsb_A6` (AGN) | CMPSB: full arith flags, normalized to bit 24 so SF/ZF/CF/OF/AF/PF are byte-correct. |
| A7 | CMPSW/CMPSD | implemented | `.op_cmpsd_A7` (dual) | CMPSW/CMPSD: dual-emitted with VNORM/VDENORM, full flags at the right width. |
| A8 | TEST AL,Ib | DEFECT | `.op_test_al_imm8_A8` (AGN) | TEST AL,Ib: computes the AND and then writes NO FLAGS. Silently wrong; `TEST AL,imm8 / JZ` is everywhere in DOS code. |
| A9 | TEST eAX,Iv | DEFECT | `.op_test_eax_imm32_A9` (dual) | TEST eAX,Iv: computes the AND and writes NO FLAGS. |
| AA | STOSB | implemented | `.op_stosb_AA` (AGN) | STOSB: ES:(E)DI with base and wrap; no flags (correct). |
| AB | STOSW/STOSD | implemented | `.op_stosd_AB` (dual) | STOSW/STOSD: dual-emitted, LDREG_V/ST_V, delta = OPSZ. |
| AC | LODSB | implemented | `.op_lodsb_AC` (AGN) | LODSB. |
| AD | LODSW/LODSD | implemented | `.op_lodsd_AD` (dual) | LODSW/LODSD: dual-emitted, STREG_V so the high half of EAX survives in 16-bit mode. |
| AE | SCASB | implemented | `.op_scasb_AE` (AGN) | SCASB: full arith flags at byte width. |
| AF | SCASW/SCASD | implemented | `.op_scasd_AF` (dual) | SCASW/SCASD: dual-emitted, VNORM/VDENORM, full flags. |
| B0 | MOV AL,Ib | implemented | `.op_mov_r8_imm8` (AGN) | 8-bit; correct high/low byte register indexing. |
| B1 | MOV CL,Ib | implemented | `.op_mov_r8_imm8` (AGN) | 8-bit; correct high/low byte register indexing. |
| B2 | MOV DL,Ib | implemented | `.op_mov_r8_imm8` (AGN) | 8-bit; correct high/low byte register indexing. |
| B3 | MOV BL,Ib | implemented | `.op_mov_r8_imm8` (AGN) | 8-bit; correct high/low byte register indexing. |
| B4 | MOV AH,Ib | implemented | `.op_mov_r8_imm8` (AGN) | 8-bit; correct high/low byte register indexing. |
| B5 | MOV CH,Ib | implemented | `.op_mov_r8_imm8` (AGN) | 8-bit; correct high/low byte register indexing. |
| B6 | MOV DH,Ib | implemented | `.op_mov_r8_imm8` (AGN) | 8-bit; correct high/low byte register indexing. |
| B7 | MOV BH,Ib | implemented | `.op_mov_r8_imm8` (AGN) | 8-bit; correct high/low byte register indexing. |
| B8 | MOV eAX,Iv | implemented | `.op_mov_r32_imm32` (dual) | Dual-emitted; LD_V immediate + EIP_ADV_V + STREG_V. |
| B9 | MOV eCX,Iv | implemented | `.op_mov_r32_imm32` (dual) | Dual-emitted; LD_V immediate + EIP_ADV_V + STREG_V. |
| BA | MOV eDX,Iv | implemented | `.op_mov_r32_imm32` (dual) | Dual-emitted; LD_V immediate + EIP_ADV_V + STREG_V. |
| BB | MOV eBX,Iv | implemented | `.op_mov_r32_imm32` (dual) | Dual-emitted; LD_V immediate + EIP_ADV_V + STREG_V. |
| BC | MOV eSP,Iv | implemented | `.op_mov_r32_imm32` (dual) | Dual-emitted; LD_V immediate + EIP_ADV_V + STREG_V. |
| BD | MOV eBP,Iv | implemented | `.op_mov_r32_imm32` (dual) | Dual-emitted; LD_V immediate + EIP_ADV_V + STREG_V. |
| BE | MOV eSI,Iv | implemented | `.op_mov_r32_imm32` (dual) | Dual-emitted; LD_V immediate + EIP_ADV_V + STREG_V. |
| BF | MOV eDI,Iv | implemented | `.op_mov_r32_imm32` (dual) | Dual-emitted; LD_V immediate + EIP_ADV_V + STREG_V. |
| C0 | Grp2 Eb,Ib | implemented | `.op_shift_eb_imm8_C0` (AGN) | Grp2 Eb,Ib: all eight /reg, reg+mem, CF/OF/SF/ZF/PF handled; count masked to 5 bits before the zero test so a masked-to-zero count leaves flags alone. |
| C1 | Grp2 Ev,Ib | DEFECT | `.op_shift_ev_imm8_C1` (dual) | Grp2 Ev,Ib: dual-emitted, all eight /reg, reg+mem -- but the operand is loaded with a bare 32-bit LDR on BOTH paths. In the 16-bit pass this reads 4 bytes of memory for a 16-bit operand and the whole 32-bit register, so SHR/SAR/ROR/RCR results and the flags are wrong whenever the upper half is non-zero. Store-back is ST_V, so only the low half is written. |
| C2 | RET Iw | DEFECT | `.op_ret_imm16_C2` (dual) | RET Iw: raw ESP as a linear address (no SS.base), hard-coded 32-bit pop and +4 regardless of operand size, and the popped offset becomes EIP WITHOUT adding CS.base. Correct only when CS.base = SS.base = 0 and the stack is 32-bit. |
| C3 | RET | implemented | `.op_ret_C3` (dual) | RET: POP_V then CS.base re-added. Correct in both widths. |
| C4 | LES Gv,Mp | implemented | `.op_les_C4` (dual) | LES: memory-only (register form correctly #UD-exits); offset at operand size, selector 16-bit; SET_SREG. |
| C5 | LDS Gv,Mp | implemented | `.op_lds_C5` (dual) | LDS: as LES. |
| C6 | MOV Eb,Ib | partial | `.op_mov_eb_imm8_C6` (AGN) | MOV Eb,Ib: register form only. The MEMORY form goes to .exit_loop -- `MOV BYTE PTR [bx],imm8` does not run. |
| C7 | MOV Ev,Iv | partial | `.op_mov_ev_imm32_C7` (dual) | MOV Ev,Iv: register form only. The MEMORY form goes to .exit_loop -- `MOV WORD PTR [bx],imm16` does not run. One of the most common instructions in DOS code. |
| C8 | ENTER Iw,Ib | DEFECT | `.op_enter_C8` (dual) | ENTER: nesting level fetched and discarded (documented); frame push uses raw ESP/EBP with no SS.base and a hard-coded -4 while storing with ST_V. |
| C9 | LEAVE | DEFECT | `.op_leave_C9` (dual) | LEAVE: raw ESP/EBP with no SS.base, hard-coded +4, and EBP written back with a full 32-bit STR.W. |
| CA | RETF Iw | DEFECT | `.op_retf_imm16_CA` (AGN) | RETF Iw: OP_AGNOSTIC, so it is a 32-bit far return in both modes; raw ESP with no SS.base; the popped CS is loaded into R3 and THROWN AWAY (no SET_SREG), so the segment never changes; and the new EIP is installed without CS.base. Compare 0xCB, which does all of this correctly. |
| CB | RETF | implemented | `.op_retf_CB` (dual) | RETF: POP_V for IP and CS, SET_SREG installs CS and recomputes the base, EIP rebuilt. Correct. |
| CC | INT3 | implemented | `.op_int3_CC` (dual) | INT3: real-mode IVT dispatch (4-byte entries, FLAGS/CS/IP at operand size, IF+TF cleared). Traps to host if CR0.PE=1. |
| CD | INT Ib | implemented | `.op_int_imm8_CD` (dual) | INT Ib: as INT3. |
| CE | INTO | implemented | `.op_into_CE` (dual) | INTO: checks OF then the same dispatch. |
| CF | IRET | implemented | `.op_iret_CF` (dual) | IRET: pops IP/CS/FLAGS with POP_V and installs CS via SET_SREG. Note: EFLAGS restored unmasked, and the handler still contains debug instrumentation writing to x86_iret_log on every execution. |
| D0 | Grp2 Eb,1 | implemented | `.op_shift_eb_1_D0` (AGN) | Grp2 Eb,1: all eight /reg, reg+mem, flags correct. |
| D1 | Grp2 Ev,1 | DEFECT | `.op_shift_ev_1_D1` (dual) | Grp2 Ev,1: same bare-32-bit-LDR operand fetch as 0xC1 -- see that entry. |
| D2 | Grp2 Eb,CL | implemented | `.op_shift_eb_cl_D2` (AGN) | Grp2 Eb,CL: all eight /reg, reg+mem, count masked to 5 bits. |
| D3 | Grp2 Ev,CL | DEFECT | `.op_shift_ev_cl_D3` (dual) | Grp2 Ev,CL: same bare-32-bit-LDR operand fetch as 0xC1. |
| D4 | AAM Ib | DEFECT | `.op_aam_D4` (AGN) | AAM: divides correctly but sets NO FLAGS (SF/ZF/PF are architecturally defined). An imm8 of 0 exits to the host instead of raising #DE. |
| D5 | AAD Ib | DEFECT | `.op_aad_D5` (AGN) | AAD: computes correctly but sets NO FLAGS (SF/ZF/PF defined). |
| D6 | SALC (undocumented) | implemented | `.op_salc_D6` (AGN) | SALC (undocumented): AL = CF ? 0xFF : 0. Correct. |
| D7 | XLAT | DEFECT | `.op_xlat_D7` (AGN) | XLAT: uses raw EBX + AL as a LINEAR address -- no DS base, no segment-override support, and no 16-bit masking of BX. Wrong whenever DS.base != 0, which is always in real mode. |
| D8 | x87 ESC 0 | unimplemented | `.exit_loop` | x87 escape. No FPU is modelled at all; all eight escape opcodes exit to the host. |
| D9 | x87 ESC 1 | unimplemented | `.exit_loop` | x87 escape. No FPU is modelled at all; all eight escape opcodes exit to the host. |
| DA | x87 ESC 2 | unimplemented | `.exit_loop` | x87 escape. No FPU is modelled at all; all eight escape opcodes exit to the host. |
| DB | x87 ESC 3 | unimplemented | `.exit_loop` | x87 escape. No FPU is modelled at all; all eight escape opcodes exit to the host. |
| DC | x87 ESC 4 | unimplemented | `.exit_loop` | x87 escape. No FPU is modelled at all; all eight escape opcodes exit to the host. |
| DD | x87 ESC 5 | unimplemented | `.exit_loop` | x87 escape. No FPU is modelled at all; all eight escape opcodes exit to the host. |
| DE | x87 ESC 6 | unimplemented | `.exit_loop` | x87 escape. No FPU is modelled at all; all eight escape opcodes exit to the host. |
| DF | x87 ESC 7 | unimplemented | `.exit_loop` | x87 escape. No FPU is modelled at all; all eight escape opcodes exit to the host. |
| E0 | LOOPNZ Jb | DEFECT | `.op_loopnz_E0` (AGN) | LOOPNZ: decrements the full 32-bit ECX regardless of address size -- must be CX under 16-bit addressing. No EIP_WRAP. |
| E1 | LOOPZ Jb | DEFECT | `.op_loopz_E1` (AGN) | LOOPZ: as LOOPNZ. |
| E2 | LOOP Jb | DEFECT | `.op_loop_E2` (AGN) | LOOP: as LOOPNZ. This is the single most common loop construct in 16-bit DOS code; it works only because the high half of ECX is usually already zero. |
| E3 | JCXZ/JECXZ Jb | DEFECT | `.op_jcxz_E3` (AGN) | JCXZ/JECXZ: tests the full 32-bit ECX regardless of address size. |
| E4 | IN AL,Ib | implemented | `.op_in_al_imm8_E4` (AGN) | IN AL,Ib: IOPL checked, dispatches to x86_port_read. |
| E5 | IN eAX,Ib | implemented | `.op_in_eax_imm8_E5` (dual) | IN eAX,Ib: dual-emitted, width passed as OPSZ, result stored with STREG_V. |
| E6 | OUT Ib,AL | implemented | `.op_out_imm8_al_E6` (AGN) | OUT Ib,AL. |
| E7 | OUT Ib,eAX | implemented | `.op_out_imm8_eax_E7` (dual) | OUT Ib,eAX: dual-emitted, LDREG_V + OPSZ. |
| E8 | CALL Jv | implemented | `.op_call_rel32_E8` (dual) | CALL Jv: LDS_V (signed displacement at operand size), PUSH_V of the segment-relative return IP, EIP_WRAP. Correct in both widths. |
| E9 | JMP Jv | implemented | `.op_jmp_rel32_E9` (dual) | JMP Jv: LDS_V + EIP_WRAP. |
| EA | JMP FAR Ap | implemented | `.op_jmp_far_EA` (dual) | JMP FAR ptr16:v: offset at operand size, 16-bit selector, SET_SREG. |
| EB | JMP Jb | implemented | `.op_jmp_rel8_EB` (AGN) | JMP Jb: correct; minor -- no EIP_WRAP. |
| EC | IN AL,DX | implemented | `.op_in_al_dx_EC` (AGN) | IN AL,DX (reads DX from REG_EDX). |
| ED | IN eAX,DX | implemented | `.op_in_eax_dx_ED` (dual) | IN eAX,DX: dual-emitted. |
| EE | OUT DX,AL | implemented | `.op_out_dx_al_EE` (AGN) | OUT DX,AL. |
| EF | OUT DX,eAX | implemented | `.op_out_dx_eax_EF` (dual) | OUT DX,eAX: dual-emitted. |
| F0 | LOCK prefix | partial | `.op_lock_F0` (AGN) | LOCK: accepted and ignored (correct for a uniprocessor). Minor: it branches to .fetch_decode_loop rather than the no-reset re-entry, so it clears a preceding segment override and decrements the PIT a second time. |
| F1 | ICEBP/INT1 | unimplemented | `.exit_loop` | ICEBP/INT1. Not implemented. |
| F2 | REPNE prefix | implemented | `.op_repne_F2` (AGN) | REPNE: arms a one-element-per-iteration rewind (CPU_REP_*), counter is CX/ECX by ADDRESS size, ZF sense 0. Ignored on non-string opcodes, as on hardware. |
| F3 | REP/REPE prefix | implemented | `.op_rep_F3` (AGN) | REP/REPE: as F2 with ZF sense 1; sense 2 (no ZF test) selected for MOVS/STOS/LODS. |
| F4 | HLT | unimplemented | `.exit_loop` | HLT: the opcode table entry is .exit_loop. A .op_hlt_F4 handler with the CR0/CPL check EXISTS in cpu_exec_handlers.S but is not referenced by either table -- dead code. Behaviour is the same exit-to-host either way, minus the #GP check. |
| F5 | CMC | implemented | `.op_cmc_F5` (AGN) | CMC. |
| F6 | Grp3 Eb | partial | `.op_grp3_eb_F6` (AGN) | Grp3 Eb: /1 unimplemented and /0 TEST is a no-op -- see the group table. |
| F7 | Grp3 Ev | partial | `.op_grp3_ev_F7` (dual) | Grp3 Ev: /1 unimplemented; the rest are implemented -- see the group table. |
| F8 | CLC | implemented | `.op_clc_F8` (AGN) | CLC. |
| F9 | STC | implemented | `.op_stc_F9` (AGN) | STC. |
| FA | CLI | implemented | `.op_cli_FA` (AGN) | CLI: IOPL checked when CR0.PE=1. |
| FB | STI | implemented | `.op_sti_FB` (AGN) | STI: IOPL checked when CR0.PE=1. Does not implement the one-instruction interrupt shadow. |
| FC | CLD | implemented | `.op_cld_FC` (AGN) | CLD. |
| FD | STD | implemented | `.op_std_FD` (AGN) | STD. |
| FE | Grp4 Eb | DEFECT | `.op_grp4_FE` (AGN) | Grp4 Eb: /0 INC and /1 DEC are implemented for both reg and mem, but write NO FLAGS. /2../7 correctly exit as invalid. |
| FF | Grp5 Ev | DEFECT | `.op_grp5_FF` (dual) | Grp5 Ev: /0../6 dispatch, /7 correctly invalid -- but /0 INC and /1 DEC write no flags, and /2 CALL near and /4 JMP near mishandle CS.base and use raw ESP. See the group table. |
---

## Two-byte (0F) opcodes — all 256

"Out of scope" means the instruction did not exist on a 386 and is not required by this
project; the slot is nevertheless accounted for individually.

| Op | Mnemonic | Era | Status | Handler | Notes |
|---|---|---|---|---|---|
| 0F 00 | Grp6 (SLDT/STR/LLDT/LTR/VERR/VERW) | 286 | unimplemented | `.exit_loop` | Entire group unimplemented. |
| 0F 01 | Grp7 (SGDT/SIDT/LGDT/LIDT/SMSW/LMSW) | 286 | partial | `.op_0f_01` (dual) | /2 LGDT, /3 LIDT, /4 SMSW and **/6 LMSW** are in; /0, /1, /7 exit, naming the ModRM. See group table. |
| 0F 02 | LAR/LSL | 286 | unimplemented | `.exit_loop` | Not implemented. |
| 0F 03 | LAR/LSL | 286 | unimplemented | `.exit_loop` | Not implemented. |
| 0F 04 | (reserved) / LOADALL | 286 | unimplemented | `.exit_loop` | Not implemented. |
| 0F 05 | (reserved) / LOADALL | 286 | unimplemented | `.exit_loop` | Not implemented. |
| 0F 06 | CLTS | 286 | unimplemented | `.exit_loop` | Not implemented. |
| 0F 07 | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 08 | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 09 | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 0A | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 0B | UD2 -> HLE hypercall | 386 | implemented | `.op_hypercall_0F0B` (AGN) | IMPLEMENTED as the project hypercall escape (0F 0B imm8). Exits with X86_EXIT_HLE_INT; raises #UD via the IVT when HLE is disabled. |
| 0F 0C | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 0D | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 0E | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 0F | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 10 | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 11 | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 12 | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 13 | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 14 | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 15 | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 16 | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 17 | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 18 | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 19 | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 1A | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 1B | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 1C | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 1D | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 1E | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 1F | (reserved / SSE on later CPUs) | P3+ | out of scope | `.exit_loop` | Out of scope for a 386-class target. |
| 0F 20 | MOV r32,CRn / MOV r32,DRn | 386 | unimplemented | `.exit_loop` | Not implemented. Without 0F 22 the guest cannot enter protected mode, which is consistent with the real-mode-only segment layer. |
| 0F 21 | MOV r32,CRn / MOV r32,DRn | 386 | unimplemented | `.exit_loop` | Not implemented. Without 0F 22 the guest cannot enter protected mode, which is consistent with the real-mode-only segment layer. |
| 0F 22 | MOV CRn,r32 / MOV DRn,r32 | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F 23 | MOV CRn,r32 / MOV DRn,r32 | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F 24 | MOV TRn (386/486 test regs) | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F 25 | MOV TRn (386/486 test regs) | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F 26 | MOV TRn (386/486 test regs) | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F 27 | MOV TRn (386/486 test regs) | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F 28 | (reserved / SSE) | P3+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 29 | (reserved / SSE) | P3+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 2A | (reserved / SSE) | P3+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 2B | (reserved / SSE) | P3+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 2C | (reserved / SSE) | P3+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 2D | (reserved / SSE) | P3+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 2E | (reserved / SSE) | P3+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 2F | (reserved / SSE) | P3+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 30 | WRMSR | P5 | out of scope | `.exit_loop` | Out of scope (Pentium). |
| 0F 31 | RDTSC | P5 | out of scope | `.exit_loop` | Out of scope (Pentium). |
| 0F 32 | RDMSR/RDPMC/SYSENTER | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 33 | RDMSR/RDPMC/SYSENTER | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 34 | RDMSR/RDPMC/SYSENTER | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 35 | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 36 | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 37 | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 38 | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 39 | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 3A | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 3B | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 3C | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 3D | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 3E | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 3F | (reserved) | - | unimplemented | `.exit_loop` | Not implemented. |
| 0F 40 | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 41 | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 42 | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 43 | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 44 | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 45 | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 46 | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 47 | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 48 | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 49 | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 4A | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 4B | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 4C | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 4D | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 4E | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 4F | CMOVcc | P6 | out of scope | `.exit_loop` | Out of scope (Pentium Pro). |
| 0F 50 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 51 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 52 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 53 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 54 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 55 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 56 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 57 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 58 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 59 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 5A | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 5B | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 5C | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 5D | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 5E | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 5F | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 60 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 61 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 62 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 63 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 64 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 65 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 66 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 67 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 68 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 69 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 6A | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 6B | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 6C | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 6D | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 6E | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 6F | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 70 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 71 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 72 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 73 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 74 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 75 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 76 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 77 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 78 | (reserved / MMX) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 79 | (reserved / MMX) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 7A | (reserved / MMX) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 7B | (reserved / MMX) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 7C | (reserved / MMX) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 7D | (reserved / MMX) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 7E | (reserved / MMX) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 7F | (reserved / MMX) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F 80 | JO Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 81 | JNO Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 82 | JB Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 83 | JNB Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 84 | JZ Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 85 | JNZ Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 86 | JBE Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 87 | JNBE Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 88 | JS Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 89 | JNS Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 8A | JP Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 8B | JNP Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 8C | JL Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 8D | JNL Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 8E | JLE Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 8F | JNLE Jv (near) | 386 | DEFECT | `.op_0f_jcc_rel32` (dual) | DEFECT: implemented and dual-emitted, but the displacement is read with a hard-coded `LDR` + `ADD R10,#4` instead of LDS_V/EIP_ADV_V. 0F 8x takes a rel16 under 16-bit operand size, so in 16-bit mode it consumes 4 bytes and uses a 32-bit displacement. Minor: no EIP_WRAP. |
| 0F 90 | SETO Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F 91 | SETNO Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F 92 | SETB Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F 93 | SETNB Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F 94 | SETZ Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F 95 | SETNZ Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F 96 | SETBE Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F 97 | SETNBE Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F 98 | SETS Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F 99 | SETNS Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F 9A | SETP Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F 9B | SETNP Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F 9C | SETL Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F 9D | SETNL Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F 9E | SETLE Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F 9F | SETNLE Eb | 386 | implemented | `.op_0f_setcc` (dual) | IMPLEMENTED. Both register and memory forms; byte destination; EVAL_COND shared with Jcc. Memory path goes through the ModRM decoder so the segment base is applied. Minor: it calls the decoder by symbol, so a 0x67 prefix is ignored. |
| 0F A0 | PUSH FS / POP FS | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F A1 | PUSH FS / POP FS | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F A2 | CPUID | 486+ | out of scope | `.exit_loop` | Out of scope. |
| 0F A3 | BT Ev,Gv | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F A4 | SHLD | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F A5 | SHLD | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F A6 | (CMPXCHG 486 early) | 486 | out of scope | `.exit_loop` | Out of scope. |
| 0F A7 | (CMPXCHG 486 early) | 486 | out of scope | `.exit_loop` | Out of scope. |
| 0F A8 | PUSH GS / POP GS | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F A9 | PUSH GS / POP GS | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F AA | RSM | 486 | out of scope | `.exit_loop` | Out of scope. |
| 0F AB | BTS Ev,Gv | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F AC | SHRD | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F AD | SHRD | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F AE | Grp15 (FXSAVE etc.) | P2+ | out of scope | `.exit_loop` | Out of scope. |
| 0F AF | IMUL Gv,Ev | 386 | implemented | `.op_0f_imul_AF` (dual) | **EPIC PINBALL's wall** (docs/13). Low half only, like the `69`/`6B` forms; CF/OF unset, which is the same known gap all three share. `tests/test_opcodes.py` 281-283. |
| 0F B0 | CMPXCHG | 486 | out of scope | `.exit_loop` | Out of scope. |
| 0F B1 | CMPXCHG | 486 | out of scope | `.exit_loop` | Out of scope. |
| 0F B2 | LSS Gv,Mp | 386 | unimplemented | `.exit_loop` | Not implemented. LES/LDS exist; LSS does not. |
| 0F B3 | BTR Ev,Gv | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F B4 | LFS / LGS Gv,Mp | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F B5 | LFS / LGS Gv,Mp | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F B6 | MOVZX Gv,Eb | 386 | DEFECT | `.op_0f_movzx_gv_eb` (dual) | IMPLEMENTED, both forms. DEFECT: the destination is written with a full 32-bit STR.W, so in 16-bit mode MOVZX clobbers the high half of the destination register instead of writing 16 bits. Also calls the ModRM decoder by symbol, ignoring a 0x67 prefix. |
| 0F B7 | MOVZX Gv,Ew | 386 | DEFECT | `.op_0f_movzx_gv_ew` (dual) | IMPLEMENTED, both forms. Same 32-bit-destination-store defect as 0F B6. |
| 0F B8 | (reserved) / Grp10 UD | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F B9 | (reserved) / Grp10 UD | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F BA | Grp8 BT/BTS/BTR/BTC Ev,Ib | 386 | unimplemented | `.exit_loop` | Not implemented -- all four defined /reg (4..7) are missing. |
| 0F BB | BTC Ev,Gv | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F BC | BSF / BSR Gv,Ev | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F BD | BSF / BSR Gv,Ev | 386 | unimplemented | `.exit_loop` | Not implemented. |
| 0F BE | MOVSX Gv,Eb | 386 | DEFECT | `.op_0f_movsx_gv_eb` (dual) | IMPLEMENTED, both forms. Same 32-bit-destination-store defect as 0F B6. |
| 0F BF | MOVSX Gv,Ew | 386 | DEFECT | `.op_0f_movsx_gv_ew` (dual) | IMPLEMENTED, both forms. Same 32-bit-destination-store defect as 0F B6. |
| 0F C0 | XADD | 486 | out of scope | `.exit_loop` | Out of scope. |
| 0F C1 | XADD | 486 | out of scope | `.exit_loop` | Out of scope. |
| 0F C2 | (SSE / CMPXCHG8B) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F C3 | (SSE / CMPXCHG8B) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F C4 | (SSE / CMPXCHG8B) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F C5 | (SSE / CMPXCHG8B) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F C6 | (SSE / CMPXCHG8B) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F C7 | (SSE / CMPXCHG8B) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F C8 | BSWAP r32 | 486 | out of scope | `.exit_loop` | Out of scope (486). |
| 0F C9 | BSWAP r32 | 486 | out of scope | `.exit_loop` | Out of scope (486). |
| 0F CA | BSWAP r32 | 486 | out of scope | `.exit_loop` | Out of scope (486). |
| 0F CB | BSWAP r32 | 486 | out of scope | `.exit_loop` | Out of scope (486). |
| 0F CC | BSWAP r32 | 486 | out of scope | `.exit_loop` | Out of scope (486). |
| 0F CD | BSWAP r32 | 486 | out of scope | `.exit_loop` | Out of scope (486). |
| 0F CE | BSWAP r32 | 486 | out of scope | `.exit_loop` | Out of scope (486). |
| 0F CF | BSWAP r32 | 486 | out of scope | `.exit_loop` | Out of scope (486). |
| 0F D0 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F D1 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F D2 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F D3 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F D4 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F D5 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F D6 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F D7 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F D8 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F D9 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F DA | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F DB | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F DC | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F DD | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F DE | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F DF | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F E0 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F E1 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F E2 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F E3 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F E4 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F E5 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F E6 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F E7 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F E8 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F E9 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F EA | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F EB | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F EC | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F ED | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F EE | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F EF | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F F0 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F F1 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F F2 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F F3 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F F4 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F F5 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F F6 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F F7 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F F8 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F F9 | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F FA | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F FB | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F FC | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F FD | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F FE | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
| 0F FF | (MMX/SSE) | P5+ | out of scope | `.exit_loop` | Out of scope. |
---

## Group opcodes — every `/reg` individually

Dispatch mechanism matters here. `Grp1` and `Grp2` use a `TBH` jump table with exactly
eight entries indexed by a 3-bit field, so **no `/reg` value can fall through** — all
eight always reach code. `Grp3`/`Grp4`/`Grp5` use a `CMP`/`BEQ` chain and anything the
chain does not name falls to `.exit_loop`.

### Grp1 — `0x80` Eb,Ib / `0x81` Ev,Iv / `0x82` (alias of 80) / `0x83` Ev,Ib

Dispatch: `L(ALU_DISPATCH_*)` → `ALU_TABLE_M`, 8-entry `TBH`.

| /reg | Op | 80 | 81 | 82 | 83 | Flags | Forms |
|---|---|---|---|---|---|---|---|
| /0 | ADD | yes | yes | yes | yes | full (CF OF SF ZF AF PF) | reg + mem |
| /1 | OR  | yes | yes | yes | yes | CF=0 OF=0, SF ZF PF; AF undefined, left alone | reg + mem |
| /2 | ADC | yes | yes | yes | yes | full; narrow path uses `X86_ADC_NARROW` so CF-in is honoured at 8/16 bits | reg + mem |
| /3 | SBB | yes | yes | yes | yes | full, same narrow path | reg + mem |
| /4 | AND | yes | yes | yes | yes | CF=0 OF=0, SF ZF PF | reg + mem |
| /5 | SUB | yes | yes | yes | yes | full | reg + mem |
| /6 | XOR | yes | yes | yes | yes | CF=0 OF=0, SF ZF PF | reg + mem |
| /7 | CMP | yes | yes | yes | yes | full, result discarded | reg + mem |

32 of 32 sub-opcode slots implemented. `0x81`/`0x83` are dual-emitted and width-correct;
`0x80`/`0x82` are byte-sized and correctly `OP_AGNOSTIC`.

### Grp2 — `0xC0`/`0xC1` E,Ib · `0xD0`/`0xD1` E,1 · `0xD2`/`0xD3` E,CL

Dispatch: `L(SHIFT_TBH_8)` / `L(SHIFT_TBH_32)`, 8-entry `TBH`.

| /reg | Op | C0 | C1 | D0 | D1 | D2 | D3 | Flags |
|---|---|---|---|---|---|---|---|---|
| /0 | ROL | yes | yes | yes | yes | yes | yes | CF + OF only; SF/ZF/AF/PF correctly untouched |
| /1 | ROR | yes | yes | yes | yes | yes | yes | CF + OF only |
| /2 | RCL | yes | yes | yes | yes | yes | yes | CF + OF; true 9-/17-/33-bit rotate with the correct modulus |
| /3 | RCR | yes | yes | yes | yes | yes | yes | CF + OF |
| /4 | SHL | yes | yes | yes | yes | yes | yes | CF, OF = MSB^CF, SF ZF PF; AF undefined |
| /5 | SHR | yes | yes | yes | yes | yes | yes | CF, OF = original MSB, SF ZF PF |
| /6 | SAL (= SHL) | yes | yes | yes | yes | yes | yes | correctly aliased to the SHL body |
| /7 | SAR | yes | yes | yes | yes | yes | yes | CF, OF cleared, SF ZF PF |

48 of 48 sub-opcode slots implemented, reg + mem on every one. The count is masked to 5
bits *before* the zero test, so a masked-to-zero count leaves all flags alone — correct.
**But the three v-width entry points (`C1`, `D1`, `D3`) fetch the operand with a bare
32-bit `LDR`** — see the defects section.

### Grp3 — `0xF6` Eb / `0xF7` Ev

Dispatch: `L(GRP3_F6_DISPATCH)` / `L(GRP3_F7_DISPATCH)`, `CMP` chain.

| /reg | Op | F6 (byte) | F7 (v) |
|---|---|---|---|
| /0 | TEST E,I | **BROKEN** — fetches the immediate, then branches to the fetch loop. No AND, no flags. | implemented, `ANDS` + `X86_FLAGS_LOGIC`, immediate at operand size |
| /1 | TEST (undocumented alias of /0) | **missing** — not in the `CMP` chain, falls to `.exit_loop` | **missing** |
| /2 | NOT | implemented; correctly writes no flags | implemented, `ST_V` |
| /3 | NEG | implemented, full flags, CF = (operand != 0) | implemented, `VNORM`-normalised, full flags |
| /4 | MUL | implemented; AX = AL*r/m8; CF/OF = "did not fit in AL"; SF/ZF/AF/PF undefined and left alone (correct) | implemented; 16-bit path writes DX:AX, 32-bit path uses `UMULL` for EDX:EAX; CF/OF correct |
| /5 | IMUL | implemented, CF/OF = "does not fit in AL" | implemented, `SMULL` / 16-bit path, CF/OF correct |
| /6 | DIV | implemented; #DE via the IVT on divide-by-zero *and* on quotient overflow | implemented; **32-bit form raises #DE whenever EDX != 0** — a true 64/32 divide is not modelled |
| /7 | IDIV | implemented, #DE on zero and on signed-quotient overflow | implemented; 16-bit form assembles DX:AX correctly; 32-bit form ignores EDX entirely (see defects) |

F6: 7 of 8 reach code, 1 of those 7 is a no-op. F7: 7 of 8 reach code and are correct.

### Grp4 — `0xFE` Eb

| /reg | Op | Status |
|---|---|---|
| /0 | INC Eb | implemented, reg + mem — **writes NO FLAGS** (OF SF ZF AF PF required, CF preserved) |
| /1 | DEC Eb | implemented, reg + mem — **writes NO FLAGS** |
| /2../7 | (invalid) | correctly `.exit_loop` |

### Grp5 — `0xFF` Ev

| /reg | Op | Status |
|---|---|---|
| /0 | INC Ev | implemented, reg + mem, `LD_V`/`ST_V` — **writes NO FLAGS** |
| /1 | DEC Ev | implemented, reg + mem — **writes NO FLAGS** |
| /2 | CALL near Ev | implemented, but pushes `R10` (the **linear** EIP, CS.base included) and then sets `R10` to the raw target offset **without** adding CS.base; stack is open-coded on raw ESP with a hard-coded −4. Self-consistent only when CS.base = SS.base = 0. |
| /3 | CALL far m16:16 | implemented (`L(GRP5_FAR_ENTRY)`): reads offset then selector from memory, pushes return CS:IP with `PUSH_V`, installs CS via `SET_SREG`. Offset always read with `LDRH`, so `m16:32` is not supported. |
| /4 | JMP near Ev | implemented, but sets `R10` to the raw offset with no CS.base — same defect as /2. |
| /5 | JMP far m16:16 | implemented, same path as /3. |
| /6 | PUSH Ev | implemented, but open-coded stack on raw ESP with a hard-coded −4. |
| /7 | (invalid) | correctly `.exit_loop` |

### `0F 00` — Grp6

| /reg | Op | Status |
|---|---|---|
| /0 | SLDT Ew | unimplemented |
| /1 | STR Ew | unimplemented |
| /2 | LLDT Ew | unimplemented |
| /3 | LTR Ew | unimplemented |
| /4 | VERR Ew | unimplemented |
| /5 | VERW Ew | unimplemented |
| /6, /7 | (invalid) | unimplemented |

The whole `0F 00` table slot is `.exit_loop`, so none of these can be reached. Consistent
with a real-mode-only core.

### `0F 01` — Grp7

Handler `.op_0f_01`. **A GROUP IS NOT AN OPCODE, and the exit says so**: the
`/0`, `/1` and `/7` arms carry the ModRM byte into `exit_info`'s high half, so a
refusal reads `0xMM0F01` and names the member. It reported a bare `0x0F` for
years — see the retraction in [`docs/13`](13_title_status.md).

**The two members with a REGISTER form (`/4` SMSW, `/6` LMSW) are decided BEFORE
the ModRM walk.** LGDT/LIDT are memory-only, so the walk used to be
unconditional and a `mod=11` encoding fell straight through the group into
`.exit_loop`. That single structural detail cost two titles their startup
(`SMSW AX` for Mortal Kombat's C runtime) and two more their entry into
protected mode (`LMSW AX`).

| /reg | Op | Status |
|---|---|---|
| /0 | SGDT Ms | **missing** — `.exit_loop`, reporting `0xMM0F01` |
| /1 | SIDT Ms | **missing** — same |
| /2 | LGDT Ms | implemented — shares `L(LXXX_OPERAND)` with LIDT. (The "STUB that loads nothing" recorded here previously is fixed.) |
| /3 | LIDT Ms | implemented: 16-bit limit and 32-bit base through the ModRM effective address. |
| /4 | SMSW Ew | implemented, register **and** memory form. Unprivileged. |
| /5 | (invalid) | correctly unreachable |
| /6 | LMSW Ew | **implemented** — `cr0 = (cr0 & ((~0xf)\|1)) \| (src & 0xf)`, i.e. PE/MP/EM/TS only and **PE is set-only** (tiny386 `i386.c:3292`, DOSBox-X `cpu.cpp:2788`, which agree). CPL 0 in protected mode only. Shares MOV CR0's PE-transition tail, so `x86_pmode_sync_mode()` runs on the edge. **This is the instruction that puts a DOS extender into protected mode** — it uses the 286 sequence, not `MOV CR0`, so nothing else here substitutes for it. `tests/test_opcodes.py` 276-278. |
| /7 | INVLPG | **missing** (486 anyway) |

Also: `.op_0f_01` reaches the decoder through `[R2, #REC_MODRM_FN]`, so `0x67` is
honoured. (An earlier note here said it called `L(DECODE_MODRM_32)` by symbol
and ignored the prefix; that is fixed.)

### `0F BA` — Grp8

| /reg | Op | Status |
|---|---|---|
| /0../3 | (invalid) | n/a |
| /4 | BT Ev,Ib | **unimplemented** (table slot is `.exit_loop`) |
| /5 | BTS Ev,Ib | **unimplemented** |
| /6 | BTR Ev,Ib | **unimplemented** |
| /7 | BTC Ev,Ib | **unimplemented** |

---

## Prefixes

| Prefix | Status | How |
|---|---|---|
| `26` ES: | implemented | `ADD R6, R8, #SEG_ES_SEL` then re-enter at `.fetch_next_byte_no_reset`. `R6` is reset to the `R8` sentinel at each instruction boundary, so the override lives exactly one instruction. The sentinel (rather than "R6 = DS") is what lets the ModRM decoder distinguish an absent prefix from an explicit `3E` on a `[BP]` form. |
| `2E` CS: | implemented | as above |
| `36` SS: | implemented | as above |
| `3E` DS: | implemented | as above |
| `64` FS: | implemented | as above |
| `65` GS: | implemented | as above |
| `66` operand size | implemented | `EOR R2, R2, #OPSIZE_XOR` — one instruction, switches the whole dispatch table (and the 0F table, at `[R2,#-8]`) to the other pass. Applying it twice cancels. |
| `67` address size | implemented | `EOR R2, R2, #ADDRSIZE_XOR`, independent axis. Honoured by every handler that reaches the decoder via `[R2, #REC_MODRM_FN]`; **ignored** by the five handlers that call `L(DECODE_MODRM_32)` by symbol (`0F 01`, `0F 9x` memory form, `0F B6/B7/BE/BF` memory form). |
| `F0` LOCK | implemented as a no-op | Correct for a uniprocessor, but it branches to `.fetch_decode_loop`, which resets the segment-override register and decrements the PIT again. `ES: LOCK ...` loses the `ES:`. |
| `F2` REPNE | implemented | Arms `CPU_REP_ACTIVE`/`CPU_REP_ZF_SENSE`/`CPU_REP_RESUME`; one element per pass through the main loop, then EIP is rewound to the prefix. Keeps the PIT serviceable inside a long REP. Counter is CX or ECX **by address size**. Ignored on non-string opcodes, as on hardware. |
| `F3` REP/REPE | implemented | Same machinery, ZF sense 1; sense 2 ("no ZF test") selected for `A4/A5` MOVS and `AA..AD` STOS/LODS. |

Segment override coverage on the *destination* of string ops is correctly refused
(`STR_DST` always uses ES), and correctly honoured on the source (`STR_SRC`).

---

## x87 and other whole-block gaps

| Block | Slots | Status |
|---|---|---|
| `D8`–`DF` x87 escapes | 8 | **Entirely unimplemented.** No FPU state exists in the CPU struct; all eight escape opcodes exit to the host as UNIMPLEMENTED. Any program that probes for a coprocessor or emits floating point will stop here. `9B` WAIT/FWAIT is also unimplemented, so even the "no FPU present" idiom (`FNINIT` / `FNSTSW`) cannot run. |
| BCD adjusts `27 2F 37 3F` (DAA/DAS/AAA/AAS) | 4 | Unimplemented. `D4`/`D5` (AAM/AAD) *are* implemented but set no flags. |
| String port I/O `6C`–`6F` | 4 | Unimplemented. |
| `62` BOUND, `63` ARPL | 2 | Unimplemented. |
| `9B` WAIT | 1 | Unimplemented. |
| `F1` ICEBP, `F4` HLT | 2 | Unimplemented in the table. A `.op_hlt_F4` handler exists in the source but is referenced by neither dispatch table — dead code. |
| 386 bit instructions `0F A3/AB/B3/BB`, `0F BA`, `0F BC/BD` | 7 | Unimplemented: BT/BTS/BTR/BTC/BSF/BSR. |
| 386 double shifts `0F A4/A5/AC/AD` | 4 | Unimplemented: SHLD/SHRD. |
| 386 segment loads `0F B2/B4/B5` | 3 | Unimplemented: LSS/LFS/LGS. |
| 386 `0F AF` IMUL Gv,Ev | 0 | Implemented. |
| 386 `0F A0/A1/A8/A9` PUSH/POP FS,GS | 4 | Unimplemented. |
| 386 control/debug regs `0F 20`–`0F 27` | 8 | Unimplemented — consistent with a real-mode-only core. |
| 286 protected mode `0F 00`, `0F 02`, `0F 03`, `0F 06` | 4 | Unimplemented. |
| 486/Pentium+ (`0F 31` RDTSC, `0F A2` CPUID, `0F C8`–`CF` BSWAP, CMPXCHG, XADD, MMX/SSE) | 158 | **Out of scope** for a 386-class target. |

---

## Defects, prioritised

Ordered by likely impact on real DOS software. These matter more than the unimplemented
opcodes because an unimplemented opcode stops the interpreter loudly with
`X86_EXIT_UNIMPLEMENTED` and the offending byte in `exit_info`, whereas everything below
produces a wrong answer and keeps running.

**HOW TO READ `exit_info`** — it is not always one byte, and it used to lie:

| value | means |
|---|---|
| `0x00`–`0xFF` | a one-byte opcode |
| `0x0Fnn` | the TWO-byte opcode `0F nn`. The 0F escape keeps the second byte in R1 and `.exit_loop` reports R0, so R0 is now loaded with `0x0F00\|nn` at the second dispatch. **Before that, every unimplemented two-byte opcode reported a bare `0x0F`**, which made three unrelated titles look like they shared one blocker for several passes ([`docs/13`](13_title_status.md)). |
| `0xMM0F01` | group 7 member: `MM` is the ModRM byte, so `/reg` names which. |

Read it with `scripts/probe_report.py`, which now derefs `cpu_state_ptr` and
prints `cpu_exit_info` alongside `cpu_cs_sel`/`cpu_eip`/`cpu_cr0` — the reported
`CS` used to come from somewhere else entirely.

### P0 — silent wrong answers on ubiquitous instructions

1. **`0x84` TEST Eb,Gb is a no-op.** The register form fetches the ModRM and branches
   straight to the fetch loop: no `AND`, no flags. The memory form exits. `TEST AL,AL /
   JZ` is one of the most common two-instruction sequences in existence; here it
   branches on whatever flags were left over.
2. **`0xA8` / `0xA9` TEST AL,Ib / TEST eAX,Iv compute the AND and discard it without
   writing flags.** Same class of failure as 1.
3. **`0xF6 /0` TEST Eb,Ib fetches the immediate and does nothing else.** Third member of
   the same family. (`0xF7 /0` is correct — the fix pattern is right there.)
4. **`0xC6` / `0xC7` MOV E,imm have no memory form.** `MOV WORD PTR [BX],1234h` exits as
   unimplemented. This is loud rather than silent, but it is so common that it belongs at
   this priority.
5. **`0xFE /0 /1` and `0xFF /0 /1` INC/DEC memory-or-register write no flags.** `INC` /
   `JNZ` and `DEC` / `JNZ` loop idioms are everywhere. (The register-direct forms `40`–`4F`
   *do* set flags correctly, so the bug only shows on the ModRM forms.)

### P1 — wrong whenever segments are not flat, i.e. always in real mode

6. **Thirteen handlers use raw `ESP` as a linear address** with a hard-coded ±4:
   `60` PUSHA, `61` POPA, `68`/`6A` PUSH imm, `8F` POP Ev, `9C` PUSHF, `9D` POPF,
   `C2` RET imm, `C8` ENTER, `C9` LEAVE, `CA` RETF imm, `FF /2` CALL near, `FF /6` PUSH Ev.
   They ignore `SS.base` *and* the operand size. In 16-bit mode `PUSHA` strides 4 bytes
   while storing 2. `PUSH_V`/`POP_V` already do this correctly and are used by
   `50`–`5F`, `06`/`07`, `E8`, `C3`, `CB`, `9A`, `EA`, `FF /3`, `FF /5`.
7. **`0xCA` RETF imm16 pops CS and throws it away.** It loads the selector into `R3` and
   never calls `SET_SREG`, so a far return with a stack-adjust keeps executing in the
   caller's segment. It is also `OP_AGNOSTIC` (always a 32-bit far return) and installs
   the new IP without `CS.base`. `0xCB` RETF does all three correctly — the contrast is
   the strongest evidence this is an oversight, not a design choice.
8. **`0xC2` RET imm16** installs the popped offset as EIP without adding `CS.base`, on
   top of the raw-ESP problem.
9. **`0xFF /2` CALL near / `0xFF /4` JMP near** push the *linear* EIP and set EIP to a
   *segment-relative* offset. The two errors cancel only when `CS.base == 0`.
10. **`0xD7` XLAT** uses `EBX + AL` as a linear address: no `DS` base, no override
    support, and no 16-bit masking of BX.
11. **`0xA0`–`0xA3` MOV moffs** use the moffs as a linear address with no `DS` base and
    no override. `A0`/`A2` additionally read a fixed 32-bit moffs regardless of address
    size; `A1`/`A3` read it at *operand* size when it must follow *address* size.

### P2 — silently 32-bit in 16-bit mode

12. **`0xC1` / `0xD1` / `0xD3` (v-width shifts)** are dual-emitted, but both the register
    and the memory operand fetch use a bare `LDR`. In the 16-bit pass this reads 4 bytes
    for a 16-bit memory operand (which can also fault at a page boundary) and the whole
    32-bit register. `SHR`, `SAR`, `ROR` and `RCR` then mix the upper half into the
    result and into CF/OF/SF/ZF/PF. The store-back is `ST_V`, so the damage is confined
    to the value and the flags rather than to neighbouring memory.
13. **`0x91`–`0x97` XCHG eAX,r** and **`0x87` XCHG Ev,Gv** are `OP_AGNOSTIC` with bare
    `LDR.W`/`STR.W`: `XCHG AX,BX` also swaps the high halves of EAX/EBX.
14. **`0x98` CBW/CWDE and `0x99` CWD/CDQ are dual-emitted but have fixed 32-bit bodies.**
    In the 16-bit pass, `98` must be CBW (AL→AX) and executes CWDE (AX→EAX); `99` must be
    CWD (sign of AX into DX) and executes CDQ, overwriting all of EDX.
15. **`0x0F 80`–`0x0F 8F` near Jcc** read the displacement with a hard-coded `LDR` and
    `ADD R10,#4`. Under 16-bit operand size these take a **rel16**, so the handler
    consumes two bytes too many and jumps by a displacement assembled from the following
    instruction's bytes. Any 386-aware 16-bit code (very common in DOS extenders and
    later-era games) that uses a near conditional jump will derail.
16. **`0x8D` LEA** writes the computed address with a bare 32-bit `STR` rather than
    `STREG_V`.
17. **`0x0F B6/B7/BE/BF` MOVZX/MOVSX** write the destination with a full 32-bit `STR.W`,
    clobbering the high half of the destination register in 16-bit mode.
18. **`0xA1` MOV eAX,Ov** writes EAX with a full 32-bit store.
19. **`0x61` POPA** writes every restored register with a full 32-bit store.

### P3 — missing memory forms and address-size gaps

20. **Register-form-only handlers**: `85` TEST Ev,Gv, `86`/`87` XCHG, `8F` POP Ev,
    `C6`/`C7` MOV E,imm. The MOV family (`88/89/8A/8B`) had exactly this bug and has been
    fixed; these five were not swept in the same pass.
21. **`E0`–`E3` LOOP/LOOPZ/LOOPNZ/JCXZ use the full 32-bit ECX** regardless of address
    size. In 16-bit code they must use CX. They happen to work whenever the upper half of
    ECX is already zero, which is most of the time — a classic latent failure.
22. **Five handlers call `L(DECODE_MODRM_32)` by symbol** instead of through
    `[R2, #REC_MODRM_FN]`, so they follow the *pass's* address size and ignore a `0x67`
    prefix: `0F 01`, `0F 9x` SETcc (memory form), `0F B6/B7/BE/BF` (memory form).

### P4 — missing flags on less-common instructions

23. **`0x69` / `0x6B` IMUL Gv,Ev,I** set no CF/OF. Software tests these to detect
    overflow.
24. **`0xD4` AAM / `0xD5` AAD** set no SF/ZF/PF (all three are architecturally defined
    for these instructions). AAM with an imm8 of 0 exits to the host instead of raising
    `#DE`.
25. **`0xF7 /6` DIV, 32-bit form**, raises `#DE` whenever `EDX != 0` rather than
    performing a 64/32 divide. `0xF7 /7` IDIV, 32-bit form, ignores EDX entirely and
    divides only EAX — a genuine wrong answer rather than a refusal.

### P5 — architectural stubs and leftovers

26. **`0F 01 /2` LGDT validates and then does nothing.** It succeeds silently without
    loading GDTR. A guest that sets up a GDT and then tries to enter protected mode gets
    no error here and fails later somewhere unrelated. Failing loudly would be better
    than a silent success.
27. **`0x9D` POPF loads EFLAGS with no mask**, so a guest can set reserved bits, IOPL and
    VM directly.
28. **`0xCF` IRET still contains debug instrumentation**, writing four words to
    `x86_iret_log` on every single execution.
29. **`0xFB` STI does not implement the one-instruction interrupt shadow**, and
    **`0x17` POP SS does not inhibit interrupts** for the following instruction. The
    classic `MOV SS,ax / MOV SP,imm` sequence can therefore take an interrupt on a
    half-updated stack. The PIT check only fires at counter expiry, which makes this rare
    and non-deterministic — the worst combination to debug.
30. **`0xF4` HLT's handler exists but is unreachable**; the table entry is `.exit_loop`.
    Net behaviour is nearly the same, minus the CR0/CPL `#GP` check.
31. **`0xF0` LOCK clears a preceding segment override** and double-decrements the PIT.

---

## What a real DOS game or utility actually needs

Ordered by what will bite first when a real binary is pointed at this core.

**Will stop the interpreter immediately (loud):**

- **`C6`/`C7` MOV mem,imm.** Almost nothing gets past initialisation without storing a
  constant to memory. This is the single highest-value fix in the report.
- **x87 (`D8`–`DF`) and `9B` WAIT.** Any program built by a compiler with floating point
  linked in, and anything that probes for a coprocessor. There is no partial credit here
  — an FPU is a substantial subsystem. Deciding early whether the target set is
  integer-only matters more than any individual opcode.
- **`0F BC/BD` BSF/BSR.** Standard compiler output. (`0F AF` IMUL Gv,Ev is now implemented.)
- **`0F B2/B4/B5` LSS/LFS/LGS.** `LSS` in particular is how 32-bit-aware code switches
  stacks atomically.
- **`27/2F/37/3F` DAA/DAS/AAA/AAS.** Old-school BCD arithmetic; rarer in games, common in
  business utilities and in hand-written assembly from the era.
- **`6C`–`6F` INS/OUTS.** Used by disk and video BIOS-style code.

**Will produce wrong output while appearing to work (quiet, and therefore worse):**

- **`84`/`A8`/`A9`/`F6 /0` TEST with no flags** and **`FE`/`FF` INC/DEC with no flags**.
  These four defects alone will make almost any real program take wrong branches. They
  are cheap to fix — `X86_FLAGS_LOGIC` and `X86_FLAGS_INCDEC` already exist and are used
  correctly a few hundred lines away.
- **The raw-ESP stack handlers.** Real-mode DOS programs essentially never run with
  `SS.base == 0`; a `.COM` or `.EXE` gets a segmented stack. `PUSHA`/`POPA`/`PUSHF`/
  `POPF`/`ENTER`/`LEAVE`/`RET imm`/`RETF imm` are all in this set, so the failure will
  show up as stack corruption on the first interrupt-heavy or frame-using code path,
  which is very hard to trace back to its cause.
- **`CA` RETF imm16 not reloading CS.** Any far call into an overlay or a resident driver
  that returns with a stack adjust silently continues in the wrong segment.
- **`E0`–`E3` LOOP using ECX.** Latent: correct until something leaves garbage in the
  upper half of ECX, then a loop runs an astronomically wrong number of times.
- **`C1`/`D1`/`D3` v-shifts reading 32 bits.** The source comment already documents
  exactly this failure mode for a previous variant of the bug (`SHL SI,CL` leaving
  overflow in the high half of ESI, which a 16-bit relocator then uses as an address) —
  the operand *fetch* was not converted when the rest of the handler was.
- **`0F 8x` near Jcc consuming 4 bytes in 16-bit mode.** Not universal, but when it
  happens the instruction stream desynchronises and the failure looks like memory
  corruption rather than a decode bug.

**Probably safe to defer:**

- The `0F 00` / `0F 01` protected-mode group, `0F 20`–`0F 27` control registers, `62`
  BOUND, `63` ARPL. A real-mode-only core does not need them, and the segment layer traps
  loudly on `CR0.PE = 1` rather than pretending. The one exception is the **LGDT stub**,
  which should be made to fail loudly instead of succeeding silently.
- All 486/Pentium+ opcodes.

## Things this audit could not determine by reading the code

- Whether the `[R8,#40]` / `[R8,#44]` literals in the LIDT path actually correspond to
  `idtr.base` / `idtr.limit`. `x86_offsets.inc` is generated at build time and was not
  read (no builds were run, per the read-only constraint). The other handlers use
  symbolic offsets, so these two literals are at minimum a style inconsistency worth
  checking.
- Whether the `SHIFT`/`ALU` `TBH` tables assemble to the offsets intended. They are built
  from `.short (target - (dispatch + 4)) / 2` expressions; correctness was assumed from
  the expressions, not verified against a disassembly.
- Runtime behaviour of any of it. This is a static reading of the source only — no build
  and no test run was performed.
