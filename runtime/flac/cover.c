/* cover.jpg -> RGB565 thumbnail of any size, area-averaged. Uses TJpgDec (C)ChaN. */
#include "tjpgd.h"
typedef struct {
    int (*rd)(void *ctx, void *buf, int n);   /* returns bytes read */
    int (*skip)(void *ctx, int n);
    void *ctx;
    int W, H, tw, th;
    uint32_t *acc;     /* tw*th*4: r, g, b, count */
} CV;
static size_t cv_in(JDEC *jd, uint8_t *buf, size_t n)
{
    CV *c = jd->device;
    if (buf) return c->rd(c->ctx, buf, (int)n);
    return c->skip(c->ctx, (int)n) ? 0 : n;
}
static int cv_out(JDEC *jd, void *bm, JRECT *r)
{
    CV *c = jd->device;
    const uint8_t *p = bm;
    int x, y;
    for (y = r->top; y <= r->bottom; y++) {
        int ty = y * c->th / c->H;
        uint32_t *row = c->acc + ty * c->tw * 4;
        for (x = r->left; x <= r->right; x++, p += 3) {
            uint32_t *a = row + (x * c->tw / c->W) * 4;
            a[0] += p[0]; a[1] += p[1]; a[2] += p[2]; a[3]++;
        }
    }
    return 1;
}
/* work: at least 3600 bytes. acc: tw*th*16 bytes, zeroed by the caller. Returns 0 on success. */
int cover_decode(int (*rd)(void *, void *, int), int (*skip)(void *, int), void *ctx,
                 int tw, int th, uint16_t *out, void *work, uint32_t *acc)
{
    JDEC jd; CV c; int s, i;
    c.rd = rd; c.skip = skip; c.ctx = ctx; c.tw = tw; c.th = th; c.acc = acc;
    if (jd_prepare(&jd, cv_in, work, 9800, &c) != JDR_OK) return 1;
    for (s = 3; s > 0 && ((jd.width >> s) < (unsigned)tw || (jd.height >> s) < (unsigned)th); s--);
    c.W = jd.width >> s; c.H = jd.height >> s;
    if (c.W < 1 || c.H < 1) return 2;
    if (jd_decomp(&jd, cv_out, s) != JDR_OK) return 3;
    for (i = 0; i < tw * th; i++) {
        uint32_t *a = acc + i * 4, n = a[3] ? a[3] : 1;
        uint32_t r = a[0] / n, g = a[1] / n, b = a[2] / n;
        out[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    return 0;
}
