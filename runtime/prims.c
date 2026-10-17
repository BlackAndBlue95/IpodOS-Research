/* Fast paths for the OS's dynamic-array element fetch and bitset scan (0x08299xxx). Arrays of
 * 4-byte elements are read directly; anything else runs Apple's code. */
#include "e.h"

/* array get(this, idx, out) 0x08299e38: bounds-checked copy of element idx (0x7fffffff: last) to out */
__attribute__((naked, section(".text.entry"), used)) void hk_aget(void)
{
    __asm__ volatile(
        "ldr ip, 3f\n ldr ip, [ip]\n cmp ip, #0\n bne 2f\n"                     /* kstock (kfix.c): Apple's code */
        "push {r4, lr}\n ldr ip, [r0]\n"
        "ldr r3, [ip, #0xa4]\n ldr r4, 5f\n cmp r3, r4\n bne 9f\n"             /* copy method is Apple's */
        "ldr r3, [ip, #0x18]\n ldr r4, 6f\n cmp r3, r4\n beq 1f\n"             /* element size 4 */
        "ldr r4, 7f\n cmp r3, r4\n bne 9f\n ldr r3, [r0, #16]\n cmp r3, #4\n bne 9f\n"   /* or size field == 4 */
        "1: ldr r3, [r0, #4]\n cmn r1, #0x80000001\n subeq r1, r3, #1\n"
        "cmp r1, #0\n blt 8f\n cmp r3, r1\n ble 8f\n"
        "ldr r3, [r0, #8]\n ldr r3, [r3, r1, lsl #2]\n str r3, [r2]\n mov r0, #1\n pop {r4, pc}\n"
        "8: mov r0, #0\n pop {r4, pc}\n"
        "9: pop {r4, lr}\n 2: .word 0xe92d41f0\n ldr pc, 4f\n"
        "3: .word kstock\n 4: .word 0x08299e3c\n 5: .word 0x08299d48\n 6: .word 0x08299af0\n 7: .word 0x082901d8\n");
}

/* array copy(this, idx, out) 0x08299d48: element idx to out, no bounds check */
__attribute__((naked, section(".text.entry"), used)) void hk_acopy(void)
{
    __asm__ volatile(
        "ldr ip, 3f\n ldr ip, [ip]\n cmp ip, #0\n bne 9f\n"                     /* kstock: Apple's code */
        "ldr ip, [r0]\n ldr r3, [ip, #0x18]\n ldr ip, 6f\n cmp r3, ip\n beq 1f\n"
        "ldr ip, 7f\n cmp r3, ip\n bne 9f\n ldr r3, [r0, #16]\n cmp r3, #4\n bne 9f\n"
        "1: ldr r3, [r0, #8]\n ldr r0, [r3, r1, lsl #2]\n str r0, [r2]\n bx lr\n"
        "9: .word 0xe92d41f0\n ldr pc, 4f\n"
        "3: .word kstock\n 4: .word 0x08299d4c\n 6: .word 0x08299af0\n 7: .word 0x082901d8\n");
}

/* bitset {nbits, ?, words} next set bit at or after i, else nbits (0x08299fcc tests one bit per call) */
__attribute__((section(".text.entry"), used)) uint32_t hk_bsnext(const uint32_t *bs, uint32_t i)
{
    uint32_t n = bs[0], x;
    const uint32_t *w = (const uint32_t *)bs[2];
    while (i < n) {
        x = w[i >> 5] >> (i & 31);
        if (x) { i += __builtin_ctz(x); return i < n ? i : n; }
        i = (i | 31) + 1;
        if (!i) break;
    }
    return n;
}

/* per-bit test calls in the glyph-bitset loops (0x0829bcc0: index r5, limit r7; 0x0828f9cc: limit sl):
 * r5 moves to the next set bit and 1 is returned, or r5 = limit - 1 and 0 ends the loop */
#define BS_SCAN(name, lim) \
__attribute__((naked, section(".text.entry"), used)) void name(void) \
{ __asm__ volatile( \
    "push {" lim ", lr}\n mov r1, r5\n bl hk_bsnext\n cmp r0, " lim "\n" \
    "movcc r5, r0\n movcc r0, #1\n subcs r5, " lim ", #1\n movcs r0, #0\n pop {" lim ", pc}\n"); }
BS_SCAN(hk_bsscan7, "r7")
BS_SCAN(hk_bsscansl, "sl")
