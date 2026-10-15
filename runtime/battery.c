/* Battery percentage in the status bar icon (Modern style).
 *
 * The iPod has no fuel gauge: Apple's level is the battery voltage, read as a raw 10-bit PMU ADC
 * count (0x082b1ac0) and counted against 15 thresholds (0x089cae8e, about 3.57 to 4.01 V), so its
 * icon has 16 real steps. Here the same count is interpolated between those thresholds to 0..100,
 * averaged over the last 8 readings (one every 5 s), and only allowed to rise again by 3 or more
 * (the voltage recovers a little when the load drops). At 20 % and below the icon is red.
 *
 * The battery icon's image function (0x081933f8) asks for the level index at 0x08193444
 * (0x08106aec: the app's provider, Apple's 0x081db234, index = level * 22 / 256, 23 images); that
 * call comes here. Other users of the provider are untouched. The icon asks only on battery power:
 * plugged in, 0x081933f8 picks Apple's charging images (0x0810aae4, 0x08109e58). Images 0 and 1
 * (white and black bar versions) are generated as one-colour 64-alpha images (gen_sficons.py) and
 * drawn here: the percentage is drawn into the other slot each time it changes and that index is
 * returned, so the status bar sees a new index and repaints. A theme apply restores the images'
 * stock pixels, so battery_redraw() draws them again at once. */
#include "e.h"

#if __has_include("sfdigits_data.h")
#include "sfdigits_data.h"

extern int theme_style, theme_gen;
const uint8_t *theme_bitmap(uint32_t id, uint32_t *size);

#define os_batt_raw     ((int (*)(uint32_t *))0x082b1ac0)    /* 0 = ok */
#define os_batt_index   ((int (*)(void *, int))0x08106aec)     /* (app, arg): the provider's level index */
#define batt_thresholds ((const uint16_t *)0x089cae8e)       /* 15 raw counts, ascending */
#define IMG_WHITE0      0x0dad010f                           /* StatusBarWhite_Battery_Image_0 */
#define IMG_BLACK0      0x0dad0128                           /* StatusBarBlack_Battery_Image_0 */

static uint16_t samples[8];
static int nsamples, shown = -1, drawn = -1, drawn_gen = -1, slot;
static uint32_t last_read;
/* each image's ink (its palette colour) as the theme left it: white bar images are re-themed at
   every apply, black bar ones never; the low-battery red replaces it in the palette */
#define LOW_PCT 20
static uint8_t ink[4][3];
static int ink_gen[4] = { -1, -1, -1, -1 };
static const uint8_t low_red[3] = { 255, 59, 48 };

static int raw_to_pct(uint32_t raw)
{
    const uint16_t *t = batt_thresholds;
    int i;
    if (raw <= t[0]) return 0;
    if (raw >= t[14]) return 100;
    for (i = 0; i < 13 && raw >= t[i + 1]; i++);
    return (100 * i + 100 * (int)(raw - t[i]) / (t[i + 1] - t[i])) / 14;
}

/* coverage (0..16) of a rounded rectangle at pixel (x, y), 4x4 samples */
static int rrect_cov(int x, int y, int rx, int ry, int rw, int rh, int r4)
{
    int i, j, n = 0;
    for (j = 0; j < 4; j++) for (i = 0; i < 4; i++) {
        int px = x * 4 + i, py = y * 4 + j, cx, cy;       /* quarter pixels */
        if (px < rx * 4 || px >= (rx + rw) * 4 || py < ry * 4 || py >= (ry + rh) * 4) continue;
        cx = px < rx * 4 + r4 ? rx * 4 + r4 : px >= (rx + rw) * 4 - r4 ? (rx + rw) * 4 - r4 - 1 : px;
        cy = py < ry * 4 + r4 ? ry * 4 + r4 : py >= (ry + rh) * 4 - r4 ? (ry + rh) * 4 - r4 - 1 : py;
        if ((px - cx) * (px - cx) + (py - cy) * (py - cy) <= r4 * r4) n++;
    }
    return n;
}

/* one 0x64 image whose palette entry k is the ink at alpha k * 255 / 63: body faint, the charge
   solid up to pct, the number cut out of the solid part and solid over the faint part */
static void draw(uint32_t id, int pct, int bx, int by, int bw, int bh, int which)
{
    uint32_t size, w, h, stride, x, y;
    uint8_t *d = (uint8_t *)theme_bitmap(id, &size), *px;
    const uint8_t *col;
    int body = bw - 3, r4 = bh * 4 * 28 / 100, fillx = bx + (body * pct + 50) / 100;
    int n = pct >= 100 ? 3 : pct >= 10 ? 2 : 1, digit[3], i, tw = 0, tx, ty;
    if (!d || (d[0] | d[1] << 8) != 0x64 || *(uint32_t *)(d + 0x1c) != 64) return;
    stride = d[4] | d[5] << 8; h = *(uint32_t *)(d + 0x10); w = *(uint32_t *)(d + 0x14);
    if (0x20 + 4 * 64 + stride * h > size) return;
    px = d + 0x20 + 4 * 64;
    if (ink_gen[which] < 0 || (which < 2 && ink_gen[which] != theme_gen)) {
        ink[which][0] = d[0x20 + 4 * 63 + 2]; ink[which][1] = d[0x20 + 4 * 63 + 1]; ink[which][2] = d[0x20 + 4 * 63];
        ink_gen[which] = theme_gen;
    }
    col = pct <= LOW_PCT ? low_red : ink[which];
    for (x = 0; x < 64; x++) { uint8_t *p = d + 0x20 + 4 * x; p[2] = col[0]; p[1] = col[1]; p[0] = col[2]; }
    digit[0] = pct / 100; digit[1] = pct / 10 % 10; digit[2] = pct % 10;
    for (i = 3 - n; i < 3; i++) tw += sf_digit_w[digit[i]] + (i < 2);
    tx = bx + (body - tw + 1) / 2; ty = by + (bh - SF_DIGIT_H) / 2;
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        int cov = rrect_cov(x, y, bx, by, body, bh, r4), a = 0;    /* alpha 0..255 */
        if (cov) {
            int solid = (int)x < fillx, g = 0, gx = tx;
            for (i = 3 - n; i < 3; i++) {                           /* digit ink at this pixel */
                int dx = (int)x - gx, dy = (int)y - ty;
                if (dx >= 0 && dx < sf_digit_w[digit[i]] && dy >= 0 && dy < SF_DIGIT_H) g = sf_digit[digit[i]][dy * SF_DIGIT_W + dx];
                gx += sf_digit_w[digit[i]] + 1;
            }
            a = solid ? 255 - g : 100 + (155 * g) / 255;
            a = a * cov / 16;
        } else if ((int)x >= bx + body + 1 && (int)x < bx + bw && (int)y >= by + bh * 32 / 100 && (int)y < by + bh - bh * 32 / 100)
            a = pct >= 100 ? 255 : 100;                            /* the nub */
        px[y * stride + x] = (uint8_t)((a * 63 + 127) / 255);
    }
}

static void draw_slot(int k)
{
    draw(IMG_WHITE0 + k, shown, 0, 0, 26, 13, k);
    draw(IMG_BLACK0 + k, shown, 1, 1, 24, 11, 2 + k);
    ((void (*)(void))0x0802ce00)();                                 /* clean D-cache */
}
/* after a theme apply (theme.c): both images hold stock pixels again */
void battery_redraw(void)
{
    if (!theme_style || shown < 0) return;
    draw_slot(slot);
    drawn = shown; drawn_gen = theme_gen;
}
E_ENTRY int e_batt_index(void *app, int arg)
{
    CRUMB();
    uint32_t now = TIMER_E, raw, sum = 0;
    int i, pct;
    if (!theme_style) return os_batt_index(app, arg);
    /* readings older than 10 minutes (asleep, or charging, when the icon is not asked) are dropped;
       what is shown stays, so the next value still has to rise by 3 to replace it */
    if (nsamples && now - last_read >= 600000000) nsamples = 0;
    if (!nsamples || now - last_read >= 5000000) {
        last_read = now;
        if (!os_batt_raw(&raw)) {
            for (i = 7; i > 0; i--) samples[i] = samples[i - 1];
            samples[0] = (uint16_t)raw;
            if (nsamples < 8) nsamples++;
        }
    }
    if (!nsamples) return os_batt_index(app, arg);
    for (i = 0; i < nsamples; i++) sum += samples[i];
    pct = raw_to_pct(sum / nsamples);
    if (shown < 0 || pct < shown || pct >= shown + 3) shown = pct;
    if (shown != drawn || theme_gen != drawn_gen) {
        slot ^= 1;
        draw_slot(slot);
        drawn = shown; drawn_gen = theme_gen;
    }
    return slot;
}
#else
E_ENTRY int e_batt_index(void *app, int arg) { return ((int (*)(void *, int))0x08106aec)(app, arg); }
void battery_redraw(void) { }
#endif
