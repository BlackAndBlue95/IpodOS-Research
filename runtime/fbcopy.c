/* --fb-damage: the screen layer's copy-back after a buffer swap (0x082817ac) copies only the rects drawn
   since the last swap, collected from Apple's bitmap damage notifier 0x0809f4d0, not the whole frame. */
#include "e.h"

#define NRECT 4
#define AMEMCPY ((void *(*)(void *, const void *, uint32_t))0x0802ced8)   /* Apple's memcpy veneer */

struct fbuf { int n; int32_t r[NRECT][4]; };         /* damage rects: top, left, bottom, right */
uint32_t fb_pix[3];                                   /* buffer 0, buffer 1, size: read by hk_fbdmg */
static struct fbuf fb[2];
static uint32_t fb_stride, fb_bpp, fb_frame;
static int fb_off, fb_unknown;

static inline uint32_t fb_irq_off(void) { uint32_t c, t; __asm__ volatile("mrs %0, cpsr\n orr %1, %0, #0xc0\n msr cpsr_c, %1" : "=r"(c), "=r"(t)); return c; }
static inline void fb_irq_on(uint32_t c) { __asm__ volatile("msr cpsr_c, %0" :: "r"(c)); }

static void add_rect(struct fbuf *f, int32_t t, int32_t l, int32_t b, int32_t r)
{
    int32_t h = fb_pix[2] / fb_stride, w = fb_stride * 8 / fb_bpp, best = 0, grow = 0x7fffffff;
    if (t < 0) t = 0;
    if (l < 0) l = 0;
    if (b > h) b = h;
    if (r > w) r = w;
    if (b <= t || r <= l) return;
    for (int i = 0; i < f->n; i++) {                  /* merge into the rect it overlaps or touches */
        int32_t *q = f->r[i];
        if (t <= q[2] && b >= q[0] && l <= q[3] && r >= q[1]) {
            if (t < q[0]) q[0] = t;
            if (l < q[1]) q[1] = l;
            if (b > q[2]) q[2] = b;
            if (r > q[3]) q[3] = r;
            return;
        }
    }
    if (f->n < NRECT) { int32_t *q = f->r[f->n++]; q[0] = t; q[1] = l; q[2] = b; q[3] = r; return; }
    for (int i = 0; i < NRECT; i++) {                 /* full: grow the one that grows least */
        int32_t *q = f->r[i];
        int32_t g = ((b > q[2] ? b : q[2]) - (t < q[0] ? t : q[0])) * ((r > q[3] ? r : q[3]) - (l < q[1] ? l : q[1]))
                    - (q[2] - q[0]) * (q[3] - q[1]);
        if (g < grow) { grow = g; best = i; }
    }
    int32_t *q = f->r[best];
    if (t < q[0]) q[0] = t;
    if (l < q[1]) q[1] = l;
    if (b > q[2]) q[2] = b;
    if (r > q[3]) q[3] = r;
}

/* bm: Apple bitmap {pixels, stride, bpp, ...}, rc: the rect about to be drawn in its coordinates */
static void __attribute__((used)) fbdmg_c(const uint32_t *bm, const int32_t *rc)
{
    for (int i = 0; i < 2; i++) {
        uint32_t off = bm[0] - fb_pix[i];
        if (off >= fb_pix[2]) continue;
        if (!fb_stride) {
            if (bm[1] < 32 || (bm[1] & 31) || fb_pix[2] % bm[1]) { fb_unknown = 1; return; }   /* the guard reads 32-byte chunks */
            fb_stride = bm[1]; fb_bpp = bm[2];
        }
        if (bm[1] != fb_stride || bm[2] != fb_bpp || (fb_bpp != 16 && fb_bpp != 32)) { fb_unknown = 1; return; }
        int32_t dy = off / fb_stride, dx = (off % fb_stride) * 8 / fb_bpp;
        uint32_t c = fb_irq_off();                     /* e_fbcopy takes the lists in another task */
        add_rect(&fb[i], rc[0] + dy, rc[1] + dx, rc[2] + dy, rc[3] + dx);
        fb_irq_on(c);
    }
}

__attribute__((naked, section(".text.entry"), used)) void hk_fbdmg(void)
{
    __asm__ volatile(                                  /* r2, r3 are free at 0x0809f4d0's entry */
        "ldr ip, 1f\n ldr r2, [r0]\n ldr r3, [ip]\n sub r3, r2, r3\n ldr ip, [ip, #8]\n cmp r3, ip\n blo 8f\n"
        "ldr ip, 1f\n ldr r3, [ip, #4]\n sub r3, r2, r3\n ldr ip, [ip, #8]\n cmp r3, ip\n bhs 9f\n"
        "8: push {r0, r1, r4, lr}\n ldr ip, 2f\n mov lr, pc\n bx ip\n pop {r0, r1, r4, lr}\n"
        "9: .word 0xe92d41f0\n ldr pc, 3f\n"
        "1: .word fb_pix\n 2: .word fbdmg_c\n 3: .word 0x0809f4d4\n");
}

static int covered(const struct fbuf *f, int32_t y, int32_t x0, int32_t x1)
{
    for (int i = 0; i < f->n; i++)
        if (y >= f->r[i][0] && y < f->r[i][2] && x1 > f->r[i][1] && x0 < f->r[i][3]) return 1;
    return 0;
}

/* one 32-byte chunk per row outside the damage, rotating each frame: equal if nothing drew unseen */
static int guard(const uint8_t *dst, const uint8_t *src, const struct fbuf *f)
{
    uint32_t h = fb_pix[2] / fb_stride, chunks = fb_stride / 32, ppc = 256 / fb_bpp;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t c = (y * 7 + fb_frame * 5) % chunks;
        if (covered(f, y, c * ppc, c * ppc + ppc)) continue;
        const uint32_t *a = (const uint32_t *)(src + y * fb_stride + c * 32), *b = (const uint32_t *)(dst + y * fb_stride + c * 32);
        for (int k = 0; k < 8; k++)
            if (a[k] != b[k]) return y + 1;
    }
    return 0;
}

/* replaces the tail call memcpy(dst, src, n) at 0x08281824: dst = the new current buffer */
E_ENTRY void *e_fbcopy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    int s = (uint32_t)src == fb_pix[0] ? 0 : (uint32_t)src == fb_pix[1] ? 1 : -1, d = 1 - s;
    uint32_t c;
    { void power_trail_frame(void); power_trail_frame(); }   /* DIAG: frames after a resume */
    if (fb_off || s < 0 || (uint32_t)dst != fb_pix[d] || n != fb_pix[2] || !fb_stride || fb_unknown || n % fb_stride) {
        if (!fb_off) {                                 /* (re)start tracking; rects drawn during the copy count */
            c = fb_irq_off();
            fb_pix[0] = (uint32_t)src; fb_pix[1] = (uint32_t)dst; fb_pix[2] = n;
            fb[0].n = fb[1].n = 0; fb_stride = 0; fb_unknown = 0;
            fb_irq_on(c);
        }
        return AMEMCPY(dst, src, n);
    }
    /* take both lists and start new ones at once: a rect drawn during the copy goes to the next swap */
    struct fbuf fs, fd, *f = &fs;
    c = fb_irq_off();
    fs = fb[s]; fd = fb[d]; fb[0].n = fb[1].n = 0;
    fb_irq_on(c);
    for (int i = 0; i < fd.n; i++) add_rect(f, fd.r[i][0], fd.r[i][1], fd.r[i][2], fd.r[i][3]);
    int bad = guard(dst, src, f);
    if (bad) {
        fb_off = 1;
        log_s("fbdmg: off, unseen write at row "); log_d(bad - 1); log_s("\n");
        return AMEMCPY(dst, src, n);
    }
    uint32_t bpp8 = fb_bpp / 8, w = fb_stride / bpp8;
    for (int i = 0; i < f->n; i++) {
        int32_t *q = f->r[i];
        uint32_t o = q[0] * fb_stride + q[1] * bpp8;
        if (q[1] == 0 && (uint32_t)q[3] == w) AMEMCPY(dst + o, src + o, (q[2] - q[0]) * fb_stride);
        else for (int32_t y = q[0]; y < q[2]; y++, o += fb_stride) AMEMCPY(dst + o, src + o, (q[3] - q[1]) * bpp8);
    }
    fb_frame++;
    return dst;
}
