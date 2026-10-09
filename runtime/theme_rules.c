/* Dark-theme colour rules. Integer math identical to host/theme_rules.py; host/rules_parity.py
 * checks the two agree on every colour the theme touches. Hue is 0..1535, sat/val 0..255. */
#include "theme_rules.h"

#define DARK_LO 0x1C
#define DARK_HI 0xFF
#define BG_LO   0x1C
#define BG_HI   0x30
#define PIXEL_CHROMA 24
#define BLUE_HUE 904
#define BLUE_TOL 171

static int imax3(int a, int b, int c) { int m = a > b ? a : b; return m > c ? m : c; }
static int imin3(int a, int b, int c) { int m = a < b ? a : b; return m < c ? m : c; }

int tr_lum(int r, int g, int b) { return (77 * r + 150 * g + 29 * b) >> 8; }
int tr_chroma(int r, int g, int b) { return imax3(r, g, b) - imin3(r, g, b); }

/* floor division, as Python's // (C's / rounds toward zero); tr_hue must match theme_rules.py */
static int fdiv(int a, int b) { int q = a / b; if ((a % b) && ((a < 0) != (b < 0))) q--; return q; }

int tr_hue(int r, int g, int b)
{
    int c = tr_chroma(r, g, b), mx = imax3(r, g, b), h;
    if (!c) return 0;
    if (mx == r) h = fdiv((g - b) * 256, c);
    else if (mx == g) h = 512 + fdiv((b - r) * 256, c);
    else h = 1024 + fdiv((r - g) * 256, c);
    h %= 1536;
    if (h < 0) h += 1536;
    return h;
}

void tr_hsv_to_rgb(int h, int s, int v, uint8_t out[3])
{
    int sector, f, p, q, t;
    h %= 1536;
    if (h < 0) h += 1536;
    sector = h >> 8; f = h & 255;
    p = v * (255 - s) / 255;
    q = v * (255 - s * f / 255) / 255;
    t = v * (255 - s * (255 - f) / 255) / 255;
    switch (sector) {
    case 0: out[0] = v; out[1] = t; out[2] = p; break;
    case 1: out[0] = q; out[1] = v; out[2] = p; break;
    case 2: out[0] = p; out[1] = v; out[2] = t; break;
    case 3: out[0] = p; out[1] = q; out[2] = v; break;
    case 4: out[0] = t; out[1] = p; out[2] = v; break;
    default: out[0] = v; out[1] = p; out[2] = q; break;
    }
}

int tr_dark_gray(int L) { return DARK_LO + (255 - L) * (DARK_HI - DARK_LO) / 255; }
/* the inverse: the stock luminance a dark-ramp grey came from */
int tr_light_of_dark(int y) { int L = 255 - (y - DARK_LO) * 255 / (DARK_HI - DARK_LO); return L < 0 ? 0 : L > 255 ? 255 : L; }
int tr_dark_lo(void) { return DARK_LO; }

void tr_gray_px(int L, uint8_t out[3])
{
    int y = tr_dark_gray(L);
    out[0] = y; out[1] = y; out[2] = y < 254 ? y + 2 : y;
}

void tr_bg_px(int L, int lmin, int lmax, uint8_t out[3])
{
    int d = lmax - lmin, y;
    if (L < lmin) L = lmin;
    if (L > lmax) L = lmax;
    if (d < 1) d = 1;
    y = BG_LO + (L - lmin) * (BG_HI - BG_LO) / d;
    out[0] = y; out[1] = y; out[2] = y + 2;
}

int tr_is_apple_blue(int r, int g, int b)
{
    int d;
    if (tr_chroma(r, g, b) <= PIXEL_CHROMA) return 0;
    d = tr_hue(r, g, b) - BLUE_HUE;
    if (d < 0) d = -d;
    if (1536 - d < d) d = 1536 - d;
    return d <= BLUE_TOL;
}

void tr_accent_px(int r, int g, int b, const uint8_t acc[3], uint8_t out[3])
{
    int v = imax3(r, g, b), c = tr_chroma(r, g, b);
    int s = v ? c * 255 / v : 0;
    int av = imax3(acc[0], acc[1], acc[2]);
    int sa = av ? tr_chroma(acc[0], acc[1], acc[2]) * 255 / av : 0;
    int s2 = s * (128 + sa / 2) / 255;
    tr_hsv_to_rgb(tr_hue(acc[0], acc[1], acc[2]), s2, v, out);
}

/* one opaque pixel of a chrome image or a COLR entry; acc NULL keeps Apple's blue */
void tr_chrome_px(int r, int g, int b, const uint8_t *acc, int gray_image, uint8_t out[3])
{
    if (gray_image || tr_chroma(r, g, b) <= PIXEL_CHROMA) { tr_gray_px(tr_lum(r, g, b), out); return; }
    if (acc && tr_is_apple_blue(r, g, b)) { tr_accent_px(r, g, b, acc, out); return; }
    out[0] = r; out[1] = g; out[2] = b;
}
