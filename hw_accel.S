    .syntax unified
    .cpu cortex-m7
    .thumb

    .global pic_get_irq_asm
    .type pic_get_irq_asm, %function

    // PICState memory layout assumptions (must match C struct):
    // offset 1: irr
    // offset 2: imr
    // offset 3: isr
    // offset 4: priority_add
    
    // int pic_get_irq_asm(PICState *s)
    // R0 = s
pic_get_irq_asm:
    LDRB R1, [R0, #1] // irr
    LDRB R2, [R0, #2] // imr
    BIC R3, R1, R2    // mask = irr & ~imr
    
    CMP R3, #0
    ITT EQ
    MOVEQ R0, #-1
    BXEQ LR           // return -1 if mask == 0

    LDRB R12, [R0, #4] // priority_add
    
    // Rotate right by priority_add (simulated for 8-bit using 32-bit ROR)
    // Actually, (mask >> p) | ((mask << (8-p)) & 0xFF)
    // We can just use LSL and LSR
    LSR R1, R3, R12
    RSB R2, R12, #8
    LSL R2, R3, R2
    ORR R3, R1, R2
    AND R3, R3, #0xFF
    
    CLZ R3, R3
    RSB R3, R3, #31   // priority = 31 - clz(rotated)
    
    // Now compute cur_priority
    LDRB R1, [R0, #3] // isr
    CMP R1, #0
    ITT EQ
    MOVEQ R2, #-1     // cur_priority = -1
    BEQ .compare_priorities
    
    LSR R2, R1, R12
    RSB R4, R12, #8
    LSL R4, R1, R4
    ORR R1, R2, R4
    AND R1, R1, #0xFF
    
    CLZ R1, R1
    RSB R2, R1, #31   // cur_priority = 31 - clz(rotated)

.compare_priorities:
    CMP R3, R2
    ITE GT
    MOVGT R0, R3
    MOVLE R0, #-1
    BX LR

    .global pit_get_out_asm
    .type pit_get_out_asm, %function

    .global pit_update_irq_asm
    .type pit_update_irq_asm, %function

