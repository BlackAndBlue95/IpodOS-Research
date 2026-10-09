/* rbflac_arm.S: ARM LPC filter, from Rockbox libffmpegFLAC/arm.S
 * Copyright (C) 2006 by Thom Johansen.  GPL v2 or later.
 * Changes: symbol renamed, own section for -gc-sections, ldmpc macro expanded.
 *
 * void rbflac_lpc_decode_arm(int n, int qlevel, int order, int32_t *data,
 *                            const int32_t *coeffs);
 * data[0..n-1] holds residuals and is overwritten with samples; the
 * 'order' history samples precede data.  32-bit wrapping accumulate (mla).
 * ARMv4+ ARM mode; position independent (pc-relative jump tables).
 */
    .syntax unified
    .arm
    .section .text.rbflac_lpc_decode_arm,"ax",%progbits
    .align 2
    .global rbflac_lpc_decode_arm
    .type rbflac_lpc_decode_arm, %function
rbflac_lpc_decode_arm:
    stmdb sp!, { r4-r11, lr }
    ldr r4, [sp, #36]
    /* r0 = blocksize, r1 = qlevel, r2 = pred_order
       r3 = data, r4 = coeffs
     */
     
    /* the data pointer always lags behind history pointer by 'pred_order'
       samples. since we have one loop for each order, we can hard code this
       and free a register by not saving data pointer. 
     */ 
    sub r3, r3, r2, lsl #2    @ r3 = history
    cmp r0, #0                @ no samples to process
    beq .exit
    cmp r2, #9                @ check if order is too high for unrolled loops
    addls pc, pc, r2, lsl #2  @ jump to our unrolled decode loop if it exists
@ jumptable:
    b .default                @ order too high, go to default routine
    b .exit                   @ zero order filter isn't possible, exit function
    b .order1
    b .order2
    b .order3
    b .order4
    b .order5
    b .order6
    b .order7
    b .order8

@ last jump table entry coincides with target, so leave it out
.order9:
    ldmia r4, { r5-r12, r14 } @ fetch coefs
.loop9:
    ldr r4, [r3], #4          @ load first history sample
    mul r2, r4, r14           @ multiply with last coef
    ldr r4, [r3], #4          @ rinse and repeat while accumulating sum in r2
    mla r2, r4, r12, r2
    ldr r4, [r3], #4
    mla r2, r4, r11, r2
    ldr r4, [r3], #4
    mla r2, r4, r10, r2
    ldr r4, [r3], #4
    mla r2, r4, r9, r2
    ldr r4, [r3], #4
    mla r2, r4, r8, r2
    ldr r4, [r3], #4
    mla r2, r4, r7, r2
    ldr r4, [r3], #4
    mla r2, r4, r6, r2
    ldr r4, [r3], #4
    mla r2, r4, r5, r2
    ldr r4, [r3]              @ r4 = residual
    add r2, r4, r2, asr r1    @ shift sum by qlevel bits and add residual 
    str r2, [r3], #-8*4       @ save result and wrap history pointer back
    subs r0, r0, #1           @ check if we're done
    bne .loop9                @ nope, jump back
    b .exit
    
.order8:
    ldmia r4, { r5-r12 }
.loop8:
    @ we have more registers to spare here, so start block reading
    ldmia r3!, { r4, r14 }
    mul r2, r4, r12
    mla r2, r14, r11, r2
    ldmia r3!, { r4, r14 }
    mla r2, r4, r10, r2
    mla r2, r14, r9, r2
    ldmia r3!, { r4, r14 }
    mla r2, r4, r8, r2
    mla r2, r14, r7, r2
    ldmia r3!, { r4, r14 }
    mla r2, r4, r6, r2
    mla r2, r14, r5, r2
    ldr r4, [r3]
    add r2, r4, r2, asr r1
    str r2, [r3], #-7*4
    subs r0, r0, #1
    bne .loop8
    b .exit

.order7:
    ldmia r4, { r5-r11 }
.loop7:
    ldmia r3!, { r4, r12, r14 }
    mul r2, r4, r11
    mla r2, r12, r10, r2
    mla r2, r14, r9, r2
    ldmia r3!, { r4, r12, r14 }
    mla r2, r4, r8, r2
    mla r2, r12, r7, r2
    mla r2, r14, r6, r2
    ldr r4, [r3], #4
    mla r2, r4, r5, r2
    ldr r4, [r3]
    add r2, r4, r2, asr r1
    str r2, [r3], #-6*4
    subs r0, r0, #1
    bne .loop7
    b .exit

.order6:
    ldmia r4, { r5-r10 }
.loop6:
    ldmia r3!, { r4, r11-r12, r14 }
    mul r2, r4, r10
    mla r2, r11, r9, r2
    mla r2, r12, r8, r2
    mla r2, r14, r7, r2
    ldmia r3!, { r4, r11 }
    mla r2, r4, r6, r2
    mla r2, r11, r5, r2
    ldr r4, [r3]
    add r2, r4, r2, asr r1
    str r2, [r3], #-5*4
    subs r0, r0, #1
    bne .loop6
    b .exit

.order5:
    ldmia r4, { r5-r9 }
.loop5:
    ldmia r3!, { r4, r10-r12, r14 }
    mul r2, r4, r9
    mla r2, r10, r8, r2
    mla r2, r11, r7, r2
    mla r2, r12, r6, r2
    mla r2, r14, r5, r2
    ldr r4, [r3]
    add r2, r4, r2, asr r1
    str r2, [r3], #-4*4
    subs r0, r0, #1
    bne .loop5
    b .exit

.order4:
    ldmia r4, { r5-r8 }
.loop4:
    ldmia r3!, { r4, r11-r12, r14 }
    mul r2, r4, r8
    mla r2, r11, r7, r2
    mla r2, r12, r6, r2
    mla r2, r14, r5, r2
    ldr r4, [r3]
    add r2, r4, r2, asr r1
    str r2, [r3], #-3*4
    subs r0, r0, #1
    bne .loop4
    b .exit

.order3:
    ldmia r4, { r5-r7 }
.loop3:
    ldmia r3!, { r4, r12, r14 }
    mul r2, r4, r7
    mla r2, r12, r6, r2
    mla r2, r14, r5, r2
    ldr r4, [r3]
    add r2, r4, r2, asr r1
    str r2, [r3], #-2*4
    subs r0, r0, #1
    bne .loop3
    b .exit

.order2:
    ldmia r4, { r5-r6 }
.loop2:
    ldmia r3!, { r4, r14 }
    mul r2, r4, r6
    mla r2, r14, r5, r2
    ldr r4, [r3]
    add r2, r4, r2, asr r1
    str r2, [r3], #-1*4
    subs r0, r0, #1
    bne .loop2
    b .exit

.order1:
    ldr r5, [r4]            @ load the one coef we need
    ldr r4, [r3], #4        @ load one history sample, r3 now points to residual
.loop1:
    mul r2, r4, r5          @ multiply coef by history sample
    ldr r4, [r3]            @ load residual
    add r4, r4, r2, asr r1  @ add result to residual
    str r4, [r3], #4        @ place r3 at next residual, we already have 
    subs r0, r0, #1         @ the current sample in r4 for the next iteration
    bne .loop1
    b .exit

.default:
    /* we do the filtering in an unrolled by 4 loop as far as we can, and then
       do the rest by jump table. */
    add r5, r4, r2, lsl #2   @ need to start in the other end of coefs
    mov r7, r2, lsr #2       @ r7 = coefs/4
    mov r14, #0              @ init accumulator
.dloop1:
    ldmdb r5!, { r8-r11 }
    ldmia r3!, { r6, r12 }
    mla r14, r6, r11, r14
    mla r14, r12, r10, r14
    ldmia r3!, { r6, r12 }
    mla r14, r6, r9, r14
    mla r14, r12, r8, r14
    subs r7, r7, #1
    bne .dloop1

    and r7, r2, #3            @ get remaining samples to be filtered
    add pc, pc, r7, lsl #2    @ jump into accumulator chain
@ jumptable:
    b .dsave @ padding
    b .dsave
    b .oneleft
    b .twoleft
@ implicit .threeleft 
    ldr r12, [r5, #-4]!
    ldr r8, [r3], #4
    mla r14, r12, r8, r14  
.twoleft:
    ldr r12, [r5, #-4]!
    ldr r8, [r3], #4
    mla r14, r12, r8, r14  
.oneleft:
    ldr r12, [r5, #-4]!
    ldr r8, [r3], #4
    mla r14, r12, r8, r14  

.dsave:
    ldr r12, [r3]             @ load residual
    add r14, r12, r14, asr r1 @ shift sum by qlevel bits and add residual
    str r14, [r3], #4         @ store result
    sub r3, r3, r2, lsl #2    @ and wrap history pointer back to next first pos
    subs r0, r0, #1           @ are we done?
    bne .default              @ no, prepare for next sample

.exit:
    ldmfd sp!, { r4-r11, pc }
    .size rbflac_lpc_decode_arm, .-rbflac_lpc_decode_arm


/* ---------------------------------------------------------------------
 * rbflac addition (not from Rockbox): 64-bit accumulate LPC for streams
 * where the 32-bit bound does not hold (all 24-bit audio).
 * void rbflac_lpc_wide_arm(int n, int qlevel, int order, int32_t *data,
 *                          const int32_t *rc);
 * rc[] = coefficients reversed (rc[0] pairs with the oldest history sample
 * data[-order]).  data[i] += (int32_t)(sum >> qlevel), qlevel 0..15.
 * Unrolled by 4 (ldm), remainder 0..3 taps one at a time; ~6 cycles/tap
 * on ARM926 vs ~10 for the gcc -O2 loop.
 * --------------------------------------------------------------------- */
    .section .text.rbflac_lpc_wide_arm,"ax",%progbits
    .align 2
    .global rbflac_lpc_wide_arm
    .type rbflac_lpc_wide_arm, %function
rbflac_lpc_wide_arm:
    stmfd   sp!, {r4-r11, lr}
    ldr     r4, [sp, #36]           @ rc
    sub     r3, r3, r2, lsl #2      @ r3 = oldest history sample
    cmp     r0, #0
    beq     .wexit
    stmfd   sp!, {r0, r1}           @ [sp] = n, [sp+4] = qlevel
.wsample:
    mov     r12, #0                 @ acc lo
    mov     lr, #0                  @ acc hi
    mov     r5, r4                  @ coefficient pointer
    movs    r6, r2, lsr #2
    beq     .wrem
.wloop4:
    ldmia   r3!, {r7-r10}
    ldmia   r5!, {r0, r1, r11}
    smlal   r12, lr, r7, r0
    ldr     r0, [r5], #4
    smlal   r12, lr, r8, r1
    smlal   r12, lr, r9, r11
    smlal   r12, lr, r10, r0
    subs    r6, r6, #1
    bne     .wloop4
.wrem:
    ands    r6, r2, #3
    beq     .wdone
.wloop1:
    ldr     r7, [r3], #4
    ldr     r0, [r5], #4
    subs    r6, r6, #1
    smlal   r12, lr, r7, r0
    bne     .wloop1
.wdone:
    ldr     r1, [sp, #4]            @ qlevel
    ldr     r7, [r3]                @ residual; r3 = current sample
    rsb     r0, r1, #32
    mov     r12, r12, lsr r1
    orr     r12, r12, lr, lsl r0    @ low word of acc >> q (lsl #32 = 0)
    add     r7, r7, r12
    str     r7, [r3]
    sub     r3, r3, r2, lsl #2
    add     r3, r3, #4              @ oldest for the next sample
    ldr     r0, [sp]
    subs    r0, r0, #1
    str     r0, [sp]
    bne     .wsample
    add     sp, sp, #8
.wexit:
    ldmfd   sp!, {r4-r11, pc}
    .size rbflac_lpc_wide_arm, .-rbflac_lpc_wide_arm
