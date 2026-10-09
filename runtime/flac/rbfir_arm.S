/* rbfir_arm.S: folded symmetric FIR, 64-bit accumulate (smlal).
 * int64_t rbfir_fold_arm(const int32_t *x, const int32_t *hc, int half);
 *   returns sum_{k<half} hc[k] * (x[k] + x[2*half-1-k]);  half % 4 == 0, > 0
 * ARMv4+ ARM mode (smlal), position independent.  ~14 insns / 8 taps. */
    .syntax unified
    .arm
    .section .text.rbfir_fold_arm,"ax",%progbits
    .align 2
    .global rbfir_fold_arm
    .type rbfir_fold_arm, %function
rbfir_fold_arm:
    stmfd   sp!, {r4-r11, lr}
    add     r3, r0, r2, lsl #3      @ r3 = one past x[2*half-1]
    mov     r12, #0                 @ acc lo
    mov     lr, #0                  @ acc hi
1:
    ldmdb   r3!, {r8-r11}           @ r11 = newest of the back group
    ldmia   r0!, {r4-r7}
    add     r4, r4, r11
    add     r5, r5, r10
    add     r6, r6, r9
    add     r7, r7, r8
    ldmia   r1!, {r8-r11}
    smlal   r12, lr, r4, r8
    smlal   r12, lr, r5, r9
    smlal   r12, lr, r6, r10
    smlal   r12, lr, r7, r11
    subs    r2, r2, #4
    bne     1b
    mov     r0, r12
    mov     r1, lr
    ldmfd   sp!, {r4-r11, pc}
    .size rbfir_fold_arm, .-rbfir_fold_arm
