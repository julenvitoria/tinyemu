    .syntax unified
    .cpu cortex-m7
    .thumb

/*
 * CGA mode 4 (320x200, 2bpp) -> LUT8 framebuffer.
 *
 * Two bits per pixel become one palette byte each. The expansion is a 256-entry
 * word table in DTCM: one source byte indexes it and yields all four output
 * pixels already packed in order, so the entire per-pixel shift/mask chain
 * collapses to a single load. That is what takes this from ~4 instructions per
 * pixel to 0.59.
 *
 * Scheduling, because this runs on every frame:
 *   - Source arrives via LDM, eight bytes at a time -- one bus transaction
 *     rather than eight LDRBs for the same data.
 *   - Every UBFX is issued TWO instructions ahead of the LDR consuming it, so
 *     no table load takes a back-to-back RAW stall. UBFX is ALU and LDR is
 *     load, so those pairs sit on different issue slots and can dual-issue.
 *   - Output leaves through a four-register STM: 16 pixels per store.
 *   - The table is 1 KB in DTCM. DTCM's second port keeps the lookups from
 *     contending with source reads coming out of AXI.
 *   - The row body is fully unrolled, so the only branch in the whole blit is
 *     the row-pair loop: one backward branch per 640 pixels, statically
 *     predicted taken. A per-chunk counter would have cost a SUBS and a BNE
 *     every 32 pixels and stolen the register that makes the 4-word STM
 *     possible.
 *
 * CGA interlaces: even rows at +0x0000, odd rows at +0x2000, 80 bytes per row.
 * Both bank cursors live in registers and are auto-advanced by their own LDMs,
 * so nothing ever branches on row parity -- a branch alternating every row is
 * precisely what the BTAC cannot learn.
 *
 * void cga_mode4_blit(uint8_t *dst, const uint8_t *even, const uint8_t *odd,
 *                     const uint32_t *tab, uint32_t dst_advance);
 *
 * dst_advance is the destination stride MINUS 320, i.e. what is left to skip
 * after a row has been written. Precomputed by the caller so the inner loop
 * does not redo it 400 times.
 *   r0 dst, r1 even bank, r2 odd bank, r3 table, [sp,#36] dst_advance
 */

    /* One 80-byte source row -> 320 pixels. \src is auto-incremented by its
       own LDMs; r0 walks the destination. */
    .macro CGA_ROW src
    .rept 10
    LDMIA   \src!, {r4, r5}         @ 8 source bytes = 32 pixels

    UBFX    r6, r4, #0, #8
    UBFX    r7, r4, #8, #8
    LDR     r6, [r3, r6, LSL #2]
    UBFX    r8, r4, #16, #8
    LDR     r7, [r3, r7, LSL #2]
    UBFX    r9, r4, #24, #8
    LDR     r8, [r3, r8, LSL #2]
    LDR     r9, [r3, r9, LSL #2]
    STMIA   r0!, {r6, r7, r8, r9}

    UBFX    r6, r5, #0, #8
    UBFX    r7, r5, #8, #8
    LDR     r6, [r3, r6, LSL #2]
    UBFX    r8, r5, #16, #8
    LDR     r7, [r3, r7, LSL #2]
    UBFX    r9, r5, #24, #8
    LDR     r8, [r3, r8, LSL #2]
    LDR     r9, [r3, r9, LSL #2]
    STMIA   r0!, {r6, r7, r8, r9}
    .endr
    .endm

    .section .itcram_hot, "ax", %progbits
    .global cga_mode4_blit
    .type cga_mode4_blit, %function
    .align 2
cga_mode4_blit:
    PUSH    {r4-r11, lr}
    LDR     r12, [sp, #36]          @ dst_advance = stride - 320
    MOV     r10, #100               @ 100 row pairs

.Lpair:
    CGA_ROW r1                      @ even scanline
    ADD     r0, r0, r12
    CGA_ROW r2                      @ odd scanline
    ADD     r0, r0, r12
    SUBS    r10, r10, #1
    BNE     .Lpair

    POP     {r4-r11, pc}
    .size cga_mode4_blit, . - cga_mode4_blit
