/* Pair kerning and fractional advances for TrueType fonts that carry a version 0 'kern' table
 * (the Modern SF fonts); Apple's own fonts and CJK fallbacks keep the stock layout. */
#include "e.h"

/* 26.6 x 16.16 -> 26.6, rounded (FT_MulFix) */
static int32_t mulfix(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b + 0x8000) >> 16);
}

/* Each text run (the measure and draw loops' prologue call 0x0809334c) leaves its return address
 * here; the single-character measure 0x080702e8 (return 0x08070304) gets stock advances. */
uint32_t kern_tok;
__attribute__((naked, section(".text.entry"), used)) void e_kern_run(void)
{
    __asm__ volatile("ldr ip, 1f\n str lr, [ip]\n ldr pc, 2f\n"
                     "1: .word kern_tok\n 2: .word 0x0809334c\n");
}

static struct {
    uint8_t *rend;          /* renderer of the run so far, 0: none */
    int32_t acc;            /* exact pen since the run started (26.6) */
    int32_t pen;            /* whole pixels handed to Apple's pen so far */
    uint16_t prev;          /* previous glyph, 0: none */
    uint8_t on;
} ks;

/* TrueType renderer's glyph metrics (vtable 0x08983e84 slot 0x24, 0x0828f948): the cached record
 * {.., +0xc advance, +0xe left} is shifted so glyphs sit at the rounded exact pen, kerned */
__attribute__((section(".text.entry"), used)) int e_kern_glyph(uint8_t *r, uint32_t g, uint8_t *rec, void *bm)
{
    int ok = ((int (*)(void *, uint32_t, void *, void *))0x0828f948)(r, g, rec, bm);
    uint32_t t = kern_tok;
    uint8_t *face, *size;
    uint16_t adv_fu = 0;
    int16_t bear;
    int32_t adv, k = 0, start, end, xs;
    if (t) { kern_tok = 0; ks.on = t != 0x08070304; ks.rend = 0; }
    if (!ok || !ks.on || __builtin_return_address(0) != (void *)0x080a93e4 || *(int32_t *)(r + 0x64) == -1) return ok;
    face = ((uint8_t *(*)(uint32_t))0x0819292c)(*(uint32_t *)(r + 0x64));   /* FT_Face */
    size = *(uint8_t **)(r + 0x68);                                          /* FT_Size */
    if (!face || !size || !*(uint32_t *)(face + 0x2e4)) { ks.rend = 0; return ok; }   /* no kern table: stock */
    if (r != ks.rend) { ks.rend = r; ks.acc = ks.pen = 0; ks.prev = 0; }
    xs = *(int32_t *)(size + 0x10);                                          /* metrics.x_scale */
    ((int (*)(void *, int, uint32_t, int16_t *, uint16_t *))0x080a7120)(face, 0, g, &bear, &adv_fu);
    adv = mulfix(adv_fu, xs);
    if ((uint32_t)(((adv + 32) >> 6) - *(int16_t *)(rec + 0xc) + 1) > 2) { ks.rend = 0; return ok; }   /* not the hmtx advance */
    if (ks.prev) k = mulfix(((int (*)(void *, uint32_t, uint32_t))0x080a6fd4)(face, ks.prev, g), xs);
    start = (ks.acc + k + 32) >> 6;
    end = (ks.acc + k + adv + 32) >> 6;
    *(int16_t *)(rec + 0xe) += start - ks.pen;
    *(int16_t *)(rec + 0xc) = end - ks.pen;
    ks.acc += k + adv;
    ks.pen = end;
    ks.prev = g;
    return ok;
}
