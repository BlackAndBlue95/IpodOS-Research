/* Theme engine: Light (stock), Dark (iOS-style grey) and an accent colour. It recolours the OS's
 * package resources and the few colours the OS keeps in code. Album art and photos are untouched.
 * Colour rules are in theme_rules.c (mirrored by host/theme_rules.py). */
#include "e.h"
#include "theme_rules.h"
#include "theme_data.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define T_BMAP FOURCC('B', 'M', 'a', 'p')
#define T_COLR FOURCC('C', 'O', 'L', 'R')

/* the resource package in the image: header, then per-type {fourcc, count, dense, index_off};
   index entries {id, data_off, size}; data at PKG_DATA + data_off */
#define PKG       ((const uint32_t *)0x08400258)
#define PKG_DATA  0x08417928u

/* OS functions: resource source (kind 1 has the override map), override insert, highlight cache, D-cache clean */
#define os_rsrc_src     ((uint8_t *(*)(void))0x08194ed8)
#define os_rsrc_insert  ((int (*)(void *, uint32_t, uint32_t, const void *, uint32_t))0x08111c2c)
#define os_hl_cache     ((uint8_t *(*)(void))0x0809349c)
#define os_dcache_clean ((void (*)(void))0x0802ce00)

/* list highlight gradient, style 0 (RW globals, {R,G,B,A}) */
#define HL_TOP    ((volatile uint8_t *)0x089cc890)
#define HL_BOTTOM ((volatile uint8_t *)0x089cc894)
/* style 1: colours are immediates in code (mov r0, #imm); the per-row step ((bottom - top) * 128)
   is in a literal pool. The bottom colour's and bottom edge's R come from a zero register, so
   only their G and B can change. The text cursor colour is three more immediates. */
static const uint32_t hl1_top_mov[3] = { 0x08261ed8, 0x08261ee0, 0x08261ee8 };
static const uint32_t hl1_step_lit[3] = { 0x0826259c, 0x082625a0, 0x082625a4 };
static const uint32_t hl1_bottom_gb[2] = { 0x08261ef0, 0x08261efc };
static const uint32_t hl1_edge_top[3] = { 0x08261f04, 0x08261f0c, 0x08261f14 };
static const uint32_t hl1_edge_bottom_gb[2] = { 0x08261f1c, 0x08261f2c };
static const uint32_t cursor_mov[3] = { 0x080e7ab8, 0x080e7ac0, 0x080e7ac8 };

/* The OS's five greys (white, #AA, #7F, #55, black) at 0x089cc8c0..d0, reached through 74 literals.
   mkpatch18.py points each literal at theme_pal_* / theme_text / theme_white by role; the colour
   is read at draw time, so views of any age follow the theme. */
uint8_t theme_pal_white[4] = { 0xff, 0xff, 0xff, 0xff };
uint8_t theme_pal_aa[4]    = { 0xaa, 0xaa, 0xaa, 0xff };
uint8_t theme_pal_7f[4]    = { 0x7f, 0x7f, 0x7f, 0xff };
uint8_t theme_pal_55[4]    = { 0x55, 0x55, 0x55, 0xff };
uint8_t theme_pal_black[4] = { 0x00, 0x00, 0x00, 0xff };
/* text colour, used where the OS draws with its black global: list rows (literal 0x08285a14),
   text views without a COLR (0x08193d88), state-styled text (0x080e27f0) */
uint8_t theme_text[4] = { 0x00, 0x00, 0x00, 0xff };
/* highlighted text keeps white whatever the theme: literals 0x08285a10, 0x081a8690, 0x08092c9c */
uint8_t theme_white[4] = { 0xff, 0xff, 0xff, 0xff };
extern uint32_t theme_clear_colour;
extern uint8_t theme_hl_r[2];
/* the LCD driver, captured at its init (entry hook 0x08143c04): slot 0x10 sets the clear
   colour the OS leaves white (0x08143dd4) */
void *theme_lcd;

static const uint8_t accents[][3] = {
    { 0x0A, 0x84, 0xFF }, { 0xBF, 0x5A, 0xF2 }, { 0xFF, 0x37, 0x5F }, { 0xFF, 0x45, 0x3A },
    { 0xFF, 0x9F, 0x0A }, { 0xFF, 0xD6, 0x0A }, { 0x30, 0xD1, 0x58 }, { 0x8E, 0x8E, 0x93 } };
static const char *const accent_names[] = { "blue", "purple", "pink", "red", "orange", "yellow", "green", "graphite" };
#define NACCENT 8

int theme_mode;          /* 0 light, 1 dark */
int theme_accent;        /* index into accents */
static int applied_mode = -1, applied_accent = -1, src_ok = -1;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static const uint8_t *pkg_find(uint32_t type, uint32_t id, uint32_t *size)
{
    uint32_t nt = PKG[2], i, k;
    for (i = 0; i < nt; i++) {
        const uint32_t *t = PKG + 3 + 4 * i;
        if (t[0] != type) continue;
        const uint32_t *ix = (const uint32_t *)((const uint8_t *)PKG + t[3]);
        for (k = 0; k < t[1]; k++)
            if (ix[3 * k] == id) { *size = ix[3 * k + 2]; return (const uint8_t *)(PKG_DATA + ix[3 * k + 1]); }
        return 0;
    }
    return 0;
}

#define os_rsrc_lookup  ((int (*)(void *, uint32_t, uint32_t, const void **))0x08111b80)
int e_memcmp(const void *a, const void *b, size_t n);   /* ossync.c */
static int rsrc_log_n;
int theme_rsrc_set(uint32_t type, uint32_t id, const void *data, uint32_t size)
{
    uint8_t *src = os_rsrc_src();
    const void *back = 0;
    int rc, found;
    /* a kind-1 source whose override map (src+4) exists; otherwise leave the UI alone */
    if (!src || src[0] != 1 || !rd32(src + 4)) {
        src_ok = 0;
        if (rsrc_log_n++ < 2) { log_s("   rsrc: source "); log_x((uint32_t)src, 8); log_s(" kind "); log_d(src ? src[0] : -1); log_s(" map "); log_x(src ? rd32(src + 4) : 0, 8); log_s(": no override\n"); }
        return -1;
    }
    src_ok = 1;
    rc = os_rsrc_insert(src, type, id, data, size);
    /* read it back through the OS's own lookup: the override took if it returns our data */
    found = os_rsrc_lookup(src, type, id, &back);
    if (rsrc_log_n++ < 4) {
        log_s("   rsrc: set "); log_x(type, 8); log_c('/'); log_x(id, 8); log_s(" size "); log_d((int)size);
        log_s(" -> insert "); log_d(rc); log_s(", lookup "); log_d(found); log_s(" data "); log_x((uint32_t)back, 8);
        log_s(back == data ? " (ours)\n" : back && !e_memcmp(back, data, size < 16 ? size : 16) ? " (copy of ours)\n" : " (NOT ours)\n");
    }
    return (back && !e_memcmp(back, data, size < 16 ? size : 16)) ? 0 : -1;
}

/* ---- the resources themselves: patched in place (same sizes), pristine copies kept ----
   The OS's resource source here is kind 2, which the override insert refuses, so the theme
   writes straight into the package data in RAM. */
struct saved { uint32_t type, id, size; uint8_t *copy; };
static struct saved saved[160];
static int nsaved;
static const uint8_t *orig_of(uint32_t type, uint32_t id, uint32_t *size)
{
    const uint8_t *src;
    uint8_t *copy;
    int i;
    for (i = 0; i < nsaved; i++)
        if (saved[i].type == type && saved[i].id == id) { *size = saved[i].size; return saved[i].copy; }
    if (!(src = pkg_find(type, id, size))) return 0;
    if (nsaved < (int)(sizeof saved / sizeof saved[0]) && (copy = os_malloc(*size))) {
        memcpy(copy, src, *size);
        saved[nsaved].type = type; saved[nsaved].id = id; saved[nsaved].size = *size; saved[nsaved].copy = copy;
        nsaved++;
        return copy;
    }
    return src;
}
static int rsrc_write(uint32_t type, uint32_t id, const void *data, uint32_t size)
{
    uint32_t psz;
    uint8_t *dst = (uint8_t *)pkg_find(type, id, &psz);
    if (!dst || psz != size) return -1;
    if (dst != data) memcpy(dst, data, size);
    return 0;
}

/* ---- recolouring a BMap into a heap copy ---- */

struct stats { int cmax, lmin, lmax; };
static void stat_px(struct stats *s, int r, int g, int b, int a)
{
    int c, L;
    if (a < 128) return;
    c = tr_chroma(r, g, b); if (c > s->cmax) s->cmax = c;
    L = tr_lum(r, g, b); if (L < s->lmin) s->lmin = L; if (L > s->lmax) s->lmax = L;
}
static void theme_px(uint8_t rgb[3], int a, int cls, const struct stats *s, const uint8_t *acc, int mode)
{
    uint8_t o[3];
    if (!a) return;
    if (cls == 2) {                                /* accent icon: coloured pixels take the accent hue in both themes */
        if (tr_chroma(rgb[0], rgb[1], rgb[2]) > 24) tr_accent_px(rgb[0], rgb[1], rgb[2], acc, o);
        else if (mode) tr_chrome_px(rgb[0], rgb[1], rgb[2], acc, 0, o);
        else return;
        rgb[0] = o[0]; rgb[1] = o[1]; rgb[2] = o[2];
        return;
    }
    if (!mode) {                                   /* light theme with an accent: chrome blues only */
        if (!cls && acc && tr_is_apple_blue(rgb[0], rgb[1], rgb[2])) { tr_accent_px(rgb[0], rgb[1], rgb[2], acc, o); rgb[0] = o[0]; rgb[1] = o[1]; rgb[2] = o[2]; }
        return;
    }
    if (cls) tr_bg_px(tr_lum(rgb[0], rgb[1], rgb[2]), s->lmin, s->lmax, o);
    else tr_chrome_px(rgb[0], rgb[1], rgb[2], acc, s->cmax <= TR_GRAY_IMAGE_CHROMA, o);
    rgb[0] = o[0]; rgb[1] = o[1]; rgb[2] = o[2];
}

/* ---- Modern style: flat chrome. A chrome or background image that is a vertical gradient (rows
   nearly uniform, varying down the image) is flattened to its mean colour. The status bar and
   main menu backgrounds take the page colour. Accent icons are left alone. ---- */
int theme_style;                                       /* 0 classic, 1 modern (settings.c) */
static int bm_px(const uint8_t *d, uint32_t fmt, uint32_t stride, uint32_t x, uint32_t y, uint8_t out[4])
{
    const uint8_t *p;
    if (fmt == 0x64 || fmt == 0x65) {
        uint32_t n = rd32(d + 0x1c), base = 0x20 + 4 * n, i;
        i = fmt == 0x64 ? d[base + y * stride + x] : (uint32_t)(d[base + y * stride + 2 * x] | d[base + y * stride + 2 * x + 1] << 8);
        if (i >= n) return 0;
        p = d + 0x20 + 4 * i; out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = p[3]; return 1;
    }
    if (fmt == 0x1888) { p = d + 0x1c + y * stride + 4 * x; out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = p[3]; return 1; }
    { uint32_t c = d[0x1c + y * stride + 2 * x] | d[0x1c + y * stride + 2 * x + 1] << 8;
      out[0] = (c >> 11) << 3; out[1] = ((c >> 5) & 63) << 2; out[2] = (c & 31) << 3; out[3] = 255; return 1; }
}
static void flatten_bar(uint8_t *d, uint32_t fmt, uint32_t stride, uint32_t w, uint32_t h, uint32_t id)
{
    uint32_t x, y, n = 0, nrows = 0, spread = 0, sr = 0, sg = 0, sb = 0;
    int rowmin = 255, rowmax = 0, page;
    uint8_t px[4], flat[3];
    if (h < 3) return;
    for (y = 0; y < h; y++) {
        uint32_t cnt = 0, lsum = 0; int lmin = 255, lmax = 0, L;
        for (x = 0; x < w; x++) {
            if (!bm_px(d, fmt, stride, x, y, px) || px[3] < 128) continue;
            L = tr_lum(px[0], px[1], px[2]); cnt++; lsum += L; if (L < lmin) lmin = L; if (L > lmax) lmax = L;
            sr += px[0]; sg += px[1]; sb += px[2]; n++;
        }
        if (!cnt) continue;
        L = lsum / cnt; nrows++; spread += lmax - lmin; if (L < rowmin) rowmin = L; if (L > rowmax) rowmax = L;
    }
    if (!nrows || !n) return;
    page = id == 0x0dad00f6 || id == 0x0dad00f9;             /* StatusBarWhite_Background, Background_LightBlue */
    if (!page && (spread / nrows > 12 || rowmax - rowmin < 16)) return;
    if (page) { flat[0] = theme_pal_white[0]; flat[1] = theme_pal_white[1]; flat[2] = theme_pal_white[2]; }
    else { flat[0] = sr / n; flat[1] = sg / n; flat[2] = sb / n; }
    if (fmt == 0x64 || fmt == 0x65) {
        uint32_t cnt = rd32(d + 0x1c), i;
        for (i = 0; i < cnt; i++) { uint8_t *p = d + 0x20 + 4 * i; if (!p[3]) continue; p[2] = flat[0]; p[1] = flat[1]; p[0] = flat[2]; }
    } else if (fmt == 0x1888) {
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) { uint8_t *p = d + 0x1c + y * stride + 4 * x; if (!p[3]) continue; p[2] = flat[0]; p[1] = flat[1]; p[0] = flat[2]; }
    } else {
        uint32_t c = (flat[0] >> 3) << 11 | (flat[1] >> 2) << 5 | flat[2] >> 3;
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) { uint8_t *p = d + 0x1c + y * stride + 2 * x; p[0] = c; p[1] = c >> 8; }
    }
}

/* returns a heap copy of the BMap with the theme applied, or NULL (then the stock data is used) */
static uint8_t *bmap_themed(const uint8_t *src, uint32_t size, int cls, const uint8_t *acc, int mode, uint32_t id)
{
    uint8_t *d;
    uint32_t fmt, stride, h, w, n, i, x, y;
    struct stats s = { 0, 255, 0 };
    if (size < 0x1c) return 0;
    fmt = src[0] | src[1] << 8; stride = src[4] | src[5] << 8; h = rd32(src + 0x10); w = rd32(src + 0x14);
    if (fmt != 0x64 && fmt != 0x65 && fmt != 0x1888 && fmt != 0x565) return 0;
    if (!(d = os_malloc(size))) return 0;
    memcpy(d, src, size);
    if (fmt == 0x64 || fmt == 0x65) {
        uint8_t *pal = d + 0x20;
        n = rd32(d + 0x1c);
        if (0x20 + 4 * n > size) { os_free(d); return 0; }
        for (i = 0; i < n; i++) stat_px(&s, pal[4 * i + 2], pal[4 * i + 1], pal[4 * i], pal[4 * i + 3]);
        for (i = 0; i < n; i++) {
            uint8_t rgb[3] = { pal[4 * i + 2], pal[4 * i + 1], pal[4 * i] };
            theme_px(rgb, pal[4 * i + 3], cls, &s, acc, mode);
            pal[4 * i + 2] = rgb[0]; pal[4 * i + 1] = rgb[1]; pal[4 * i] = rgb[2];
        }
    } else if (fmt == 0x1888) {
        if (0x1c + stride * h > size) { os_free(d); return 0; }
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) { uint8_t *p = d + 0x1c + y * stride + 4 * x; stat_px(&s, p[2], p[1], p[0], p[3]); }
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            uint8_t *p = d + 0x1c + y * stride + 4 * x, rgb[3] = { p[2], p[1], p[0] };
            theme_px(rgb, p[3], cls, &s, acc, mode);
            p[2] = rgb[0]; p[1] = rgb[1]; p[0] = rgb[2];
        }
    } else {
        if (0x1c + stride * h > size) { os_free(d); return 0; }
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            uint32_t c = d[0x1c + y * stride + 2 * x] | d[0x1c + y * stride + 2 * x + 1] << 8;
            stat_px(&s, (c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3, 255);
        }
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            uint8_t *p = d + 0x1c + y * stride + 2 * x;
            uint32_t c = p[0] | p[1] << 8;
            uint8_t rgb[3] = { (c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3 };
            theme_px(rgb, 255, cls, &s, acc, mode);
            c = (rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | rgb[2] >> 3;
            p[0] = c; p[1] = c >> 8;
        }
    }
    if (theme_style && cls != 2) flatten_bar(d, fmt, stride, w, h, id);
    return d;
}

/* ---- the colours the OS keeps outside the package ---- */
static void icache_sync(void)
{
    os_dcache_clean();
    __asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c7, c5, 0" ::: "r0", "memory");
}

/* mov rX, #imm8 (rotation 0): replace the immediate */
static void patch_mov_imm(uint32_t at, uint8_t imm)
{
    volatile uint32_t *w = (volatile uint32_t *)at;
    if ((*w & 0x0ff00f00) != 0x03a00000) return;    /* not a mov rX, #imm8 */
    *w = (*w & 0xffffff00) | imm;
}

static void gradient_for(int mode, int accent, uint8_t top[3], uint8_t bottom[3])
{
    if (!mode && !accent) { top[0] = 0x58; top[1] = 0x98; top[2] = 0xe8; bottom[0] = 0x28; bottom[1] = 0x64; bottom[2] = 0xd0; return; }
    const uint8_t *a = accents[accent];
    int h = tr_hue(a[0], a[1], a[2]), v = a[0] > a[1] ? (a[0] > a[2] ? a[0] : a[2]) : (a[1] > a[2] ? a[1] : a[2]);
    int s = v ? tr_chroma(a[0], a[1], a[2]) * 255 / v : 0, vt = v * 108 / 100;
    tr_hsv_to_rgb(h, s, vt > 255 ? 255 : vt, top);
    tr_hsv_to_rgb(h, s, v * 78 / 100, bottom);
}

static void apply_code_colours(int mode, int accent)
{
    uint8_t top[3], bottom[3], o[3];
    int i;
    gradient_for(mode, accent, top, bottom);
    if (theme_style) { const uint8_t *a = accents[accent]; for (i = 0; i < 3; i++) top[i] = bottom[i] = a[i]; }   /* Modern: flat accent */
    for (i = 0; i < 3; i++) { HL_TOP[i] = top[i]; HL_BOTTOM[i] = bottom[i]; }
    /* style 1 gradient: stock #09A4E1 -> #0063D3 with edges #96C5DA / #004386, else the accent */
    {
        uint8_t et[3], eb[3], cur[3];
        if (!mode && !accent && !theme_style) {
            top[0] = 0x09; top[1] = 0xa4; top[2] = 0xe1; bottom[0] = 0x00; bottom[1] = 0x63; bottom[2] = 0xd3;
            et[0] = 0x96; et[1] = 0xc5; et[2] = 0xda; eb[0] = 0x00; eb[1] = 0x43; eb[2] = 0x86;
            cur[0] = 0x31; cur[1] = 0x6b; cur[2] = 0xde;
        } else {
            const uint8_t *a = accents[accent];
            int h = tr_hue(a[0], a[1], a[2]), v = a[0] > a[1] ? (a[0] > a[2] ? a[0] : a[2]) : (a[1] > a[2] ? a[1] : a[2]);
            int s = v ? tr_chroma(a[0], a[1], a[2]) * 255 / v : 0, vt = v * 120 / 100;
            tr_hsv_to_rgb(h, s * 60 / 100, vt > 255 ? 255 : vt, et);     /* light edge: paler, brighter */
            tr_hsv_to_rgb(h, s, v * 55 / 100, eb);                        /* dark edge */
            cur[0] = a[0]; cur[1] = a[1]; cur[2] = a[2];
            if (theme_style) for (i = 0; i < 3; i++) { et[i] = a[i]; eb[i] = a[i]; top[i] = a[i]; bottom[i] = a[i]; }   /* no edge lines, no gradient */
        }
        for (i = 0; i < 3; i++) {
            patch_mov_imm(hl1_top_mov[i], top[i]);
            *(volatile int32_t *)hl1_step_lit[i] = ((int)bottom[i] - (int)top[i]) * 128;
            patch_mov_imm(hl1_edge_top[i], et[i]);
            patch_mov_imm(cursor_mov[i], cur[i]);
        }
        patch_mov_imm(hl1_bottom_gb[0], bottom[1]); patch_mov_imm(hl1_bottom_gb[1], bottom[2]);
        theme_hl_r[0] = bottom[0]; theme_hl_r[1] = eb[0];        /* the red channels, via e_hl_bottom_r / e_hl_edge_r */
        patch_mov_imm(hl1_edge_bottom_gb[0], eb[1]); patch_mov_imm(hl1_edge_bottom_gb[1], eb[2]);
    }
    /* the text renderer's emboss pass (0x08286b18, flag 0x1000 text such as the Now Playing title)
       is drawn first in a fixed light grey (mov r0, #0xdd at 0x08286b4c). In Dark that shows as a
       light block behind the title, so it takes the page grey. */
    patch_mov_imm(0x08286b4c, mode ? theme_pal_white[0] : 0xdd);
    icache_sync();
    /* the five greys: backgrounds and frames */
    static const uint8_t stock[5] = { 0xff, 0xaa, 0x7f, 0x55, 0x00 };
    uint8_t *pal[5] = { theme_pal_white, theme_pal_aa, theme_pal_7f, theme_pal_55, theme_pal_black };
    for (i = 0; i < 5; i++) {
        if (mode) { tr_gray_px(stock[i], o); pal[i][0] = o[0]; pal[i][1] = o[1]; pal[i][2] = o[2]; }
        else pal[i][0] = pal[i][1] = pal[i][2] = stock[i];
    }
    /* black fills stay black in both themes (Now Playing video, photos); black text is theme_text */
    theme_pal_black[0] = theme_pal_black[1] = theme_pal_black[2] = 0;
    if (mode) { tr_gray_px(0, o); theme_text[0] = o[0]; theme_text[1] = o[1]; theme_text[2] = o[2]; }
    else theme_text[0] = theme_text[1] = theme_text[2] = 0;
    theme_white[0] = theme_white[1] = theme_white[2] = 0xff;      /* referenced here so the linker keeps it */
    theme_clear_colour = mode ? 0x001c1c1e : 0x00ffffff;   /* read by the LCD init through e_lcd_clear */
    { void e_art_clear(uint32_t, uint32_t, uint32_t, uint32_t); e_art_clear(0, 0, 0, 0x10000); }   /* live: the art surface's clear colour */
    /* the cover reflection is generated in software (0x08268ef0). In the album view's mode
       (0x089d00d8 == 1) every pixel is blended toward the RGB565 literal 0x0000ffff at 0x082690d4,
       so that word is rewritten with the surface colour. */
    {
        static uint32_t refl_last = 0xffff;
        uint8_t *lit = (uint8_t *)0x082690d4;
        uint32_t v = (uint32_t)(theme_pal_white[0] >> 3) << 11 | (uint32_t)(theme_pal_white[1] >> 2) << 5 | (theme_pal_white[2] >> 3);
        if (rd32(lit) == 0xffff || rd32(lit) == refl_last) { wr32(lit, v); refl_last = v; }
    }
    /* Now Playing's surface colour: the resource provider (0x08221668) returns the static record at
       0x08ae4e0c, copied from 0x08a09db8 (#D0E1FF). Sets bit 0 of the guard 0x08a09db0 so the OS
       keeps it. */
    {
        uint8_t *np = (uint8_t *)0x08a09db8;
        if (mode) { np[0] = 0x1c; np[1] = 0x1c; np[2] = 0x1e; } else { np[0] = 0xd0; np[1] = 0xe1; np[2] = 0xff; }
        np[3] = 0xff;
        wr32((uint8_t *)0x08a09db0, rd32((const uint8_t *)0x08a09db0) | 1);
        memcpy((uint8_t *)0x08ae4e0c + 0x20, np, 4);
    }
    if (theme_lcd) {
        void **vt = *(void ***)theme_lcd;
        uint32_t c = theme_clear_colour;
        log_s("   theme: lcd clear colour "); log_x(c, 6); log_s(" via "); log_x((uint32_t)vt[4], 8); log_c('\n');
        ((void (*)(void *, uint32_t))vt[4])(theme_lcd, c);
    }
    /* the cached highlight bitmap is keyed on the row rect only: make it miss */
    { uint8_t *c = os_hl_cache(); if (c) { uint32_t k; for (k = 0; k < 4; k++) wr32(c + 0x98 + 4 * k, 0xffffffff); } }
}

/* Replaces the LCD driver's `mvn r1, #0xff000000` at 0x08143dd4 (white clear colour) with
   `bl e_lcd_clear`, which returns the theme's clear colour in r1. r0 and r4 are untouched, and
   lr is dead at that point. */
uint32_t theme_clear_colour = 0x00ffffff;
__attribute__((naked, section(".text.entry"), used)) void e_lcd_clear(void)
{
    __asm__ volatile("ldr r1, 1f\n ldr r1, [r1]\n bx lr\n 1: .word theme_clear_colour\n");
}

/* The album-art surface. The album view (0x08163a6c) and the main menu's art pane (0x08137dc0)
 * clear with glClearColorx(1, 1, 1, 1) (0x082c5f14) before blitting the art over the view. Both
 * calls are redirected here, to the theme's surface colour with the same alpha. */
#define os_clear_colour ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t))0x082c5f14)
__attribute__((used, noinline, section(".text.entry"))) void e_art_clear(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    (void)r; (void)g; (void)b;
    os_clear_colour(theme_pal_white[0] * 0x10000u / 255, theme_pal_white[1] * 0x10000u / 255, theme_pal_white[2] * 0x10000u / 255, a);
}

/* Modern style has no mirror reflection. The cover render (0x082706d0) draws the reflection mesh
   only when cover+0x94 holds a reflection texture. The load at 0x08270980 is now `bl e_refl_gate`,
   which returns that texture in r0, or 0 in Modern style (r4 = cover; ip is scratch, lr is dead). */
__attribute__((naked, section(".text.entry"), used)) void e_refl_gate(void)
{
    __asm__ volatile("ldr r0, [r4, #0x94]\n ldr ip, 1f\n ldr ip, [ip]\n cmp ip, #0\n movne r0, #0\n bx lr\n 1: .word theme_style\n");
}

/* The selection bar's bottom row and bottom edge store red from a zero register (0x08261ef4,
 * 0x08261f24; stock #0063D3 and #004386 have R = 0). Both stores are `bl` to these stubs, which
 * store theme_hl_r instead. */
uint8_t theme_hl_r[2];
__attribute__((naked, section(".text.entry"), used)) void e_hl_bottom_r(void)
{ __asm__ volatile("ldr ip, 1f\n ldrb ip, [ip]\n strb ip, [sp, #0x118]\n bx lr\n 1: .word theme_hl_r\n"); }
__attribute__((naked, section(".text.entry"), used)) void e_hl_edge_r(void)
{ __asm__ volatile("ldr ip, 1f\n ldrb ip, [ip]\n strb ip, [sp, #0x110]\n bx lr\n 1: .word theme_hl_r + 1\n"); }

/* ---- entry hooks: 0x081135a0 creates the root view (the theme is applied before it, in
 * settings_early); 0x08143c04 is the LCD driver's init, which remembers the driver object. ---- */
void settings_early(void);
static void __attribute__((used)) uiinit_c(uint32_t *r)
{
    static int done;
    (void)r;
    if (done) return;
    done = 1;
    settings_early();
}
static void __attribute__((used)) lcdinit_c(uint32_t *r) { void power_resume_check(void); theme_lcd = (void *)r[0]; power_resume_check(); }
__attribute__((naked, section(".text.entry"), used)) void hk_uiinit(void)
{
    __asm__ volatile(
        "push {r0-r5, ip, lr}\n mov r0, sp\n ldr ip, 1f\n mov lr, pc\n bx ip\n"
        "pop {r0-r5, ip, lr}\n"
        ".word 0xe92d40f8\n ldr pc, 2f\n"
        "1: .word uiinit_c\n 2: .word 0x081135a4\n");
}
void settings_boot(int final);
static void __attribute__((used)) appinit_c(uint32_t *r) { (void)r; log_t(0); settings_boot(0); }
__attribute__((naked, section(".text.entry"), used)) void hk_appinit(void)
{
    __asm__ volatile(
        "push {r0-r5, ip, lr}\n mov r0, sp\n ldr ip, 1f\n mov lr, pc\n bx ip\n"
        "pop {r0-r5, ip, lr}\n"
        ".word 0xe92d4070\n ldr pc, 2f\n"
        "1: .word appinit_c\n 2: .word 0x0827ee10\n");
}
__attribute__((naked, section(".text.entry"), used)) void hk_lcdinit(void)
{
    __asm__ volatile(
        "push {r0-r5, ip, lr}\n mov r0, sp\n ldr ip, 1f\n mov lr, pc\n bx ip\n"
        "pop {r0-r5, ip, lr}\n"
        ".word 0xe92d41f0\n ldr pc, 2f\n"
        "1: .word lcdinit_c\n 2: .word 0x08143c08\n");
}

/* COLRs that colour text or icons drawn over panels (by name: *_White, *_Text_Color, *Foreground*,
   *_Color_Gray, Icon): dark ones invert to light, light ones stay light */
static int colr_is_fg(uint32_t id)
{
    static const uint32_t fg[] = { 0x0dad0097, 0x0dad0235, 0x0dad0236, 0x0dad0237, 0x0dad0684, 0x0dad0685, 0x0dad0b3e, 0x0dad0b40,
                                   0x0dad0b41, 0x0dad0c80, 0x0dad0c81, 0x0dad0c82, 0x0dad0c83, 0x0dad0c88, 0x0dad0312, 0x0dad0314, 0x0dad0315 };
    unsigned i;
    for (i = 0; i < sizeof fg / sizeof fg[0]; i++) if (fg[i] == id) return 1;
    return 0;
}

/* The OS's named colour objects: 16 bytes {type 0x80be, id, RGBA at +8, flags}, built at start-up
 * by 0x080f8e94. Code-drawn text and fills read these (media lists, Now Playing, charging screen).
 * They are re-coloured like COLRs; stock values are saved on first use. */
static uint8_t *const named_colour[12] = {
    (uint8_t *)0x08ae5444, (uint8_t *)0x08ae5454, (uint8_t *)0x08ae5464, (uint8_t *)0x08ae5474, (uint8_t *)0x08ae5484, (uint8_t *)0x08ae5494,
    (uint8_t *)0x08ae54a4, (uint8_t *)0x08ae54b4, (uint8_t *)0x08ae54c4, (uint8_t *)0x08ae54d4, (uint8_t *)0x08ad2cc8, (uint8_t *)0x08ad2cd8 };
static uint8_t named_stock[12][4];
static int named_saved;
static void theme_named(int mode, int accent, const uint8_t *acc)
{
    int i;
    for (i = 0; i < 12; i++) {
        uint8_t *c = named_colour[i] + 8, rgb[3];
        const uint8_t *st;
        if (!named_saved) { named_stock[i][0] = c[0]; named_stock[i][1] = c[1]; named_stock[i][2] = c[2]; named_stock[i][3] = c[3]; }
        st = named_stock[i];
        if (mode) tr_chrome_px(st[0], st[1], st[2], acc, 0, rgb);
        else if (accent && tr_is_apple_blue(st[0], st[1], st[2])) tr_accent_px(st[0], st[1], st[2], acc, rgb);
        else { rgb[0] = st[0]; rgb[1] = st[1]; rgb[2] = st[2]; }
        c[0] = rgb[0]; c[1] = rgb[1]; c[2] = rgb[2];
    }
    if (!named_saved) {
        named_saved = 1;
        log_s("   theme: named colours stock");
        for (i = 0; i < 12; i++) { log_c(' '); log_x(named_stock[i][0] << 16 | named_stock[i][1] << 8 | named_stock[i][2], 6); }
        log_c('\n');
    }
}

/* After a switch, the live view tree is walked. Each stored colour that is a known variant of a
 * themed COLR (stock, dark, or light with accent) becomes the new theme's variant. Children are
 * the list at view+0xa8, iterated with 0x081e32a4 / 0x081e3260 / 0x081e32b4. */
#define os_view_iter_init ((void (*)(void *, void *))0x081e32a4)
#define os_view_iter_next ((int (*)(void *, void **))0x081e3260)
#define os_view_iter_end  ((void (*)(void *))0x081e32b4)
static int walk_mode, walk_accent, walk_n, walk_live;
static void colr_variants(uint32_t v, int accent, uint8_t dark[3], uint8_t light[3])
{
    const uint8_t *acc = accents[accent];
    int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
    tr_chrome_px(r, g, b, acc, 0, dark);
    if (accent && tr_is_apple_blue(r, g, b)) tr_accent_px(r, g, b, acc, light);
    else { light[0] = r; light[1] = g; light[2] = b; }
}
static void remap_colour(uint8_t *c)
{
    uint32_t i, size;
    for (i = 0; i < sizeof theme_colr / 4; i++) {
        const uint8_t *src = orig_of(T_COLR, theme_colr[i], &size);
        uint8_t dark[3], light[3], *want;
        uint32_t v;
        if (!src || size < 4) continue;
        v = rd32(src);
        colr_variants(v, walk_accent, dark, light);
        if (!((c[0] == ((v >> 16) & 255) && c[1] == ((v >> 8) & 255) && c[2] == (v & 255)) ||
              (c[0] == dark[0] && c[1] == dark[1] && c[2] == dark[2]) ||
              (c[0] == light[0] && c[1] == light[1] && c[2] == light[2]))) continue;
        want = walk_mode ? dark : light;                 /* a stored background: always the background rule */
        c[0] = want[0]; c[1] = want[1]; c[2] = want[2];
        walk_n++;
        return;
    }
    /* not a themed COLR's value: the OS derives some panel greys itself (#353535, #1e1e1e). A
       neutral grey moves along the dark ramp: down when light (stock), back through its inverse
       when in the dark range. Blacks and mid greys are left alone. */
    if (tr_chroma(c[0], c[1], c[2]) > 24) return;
    {
        int L = tr_lum(c[0], c[1], c[2]), y;
        if (walk_mode) {
            if (L < 200) return;
            y = tr_dark_gray(L); c[0] = y; c[1] = y; c[2] = y < 254 ? y + 2 : y;
        } else {
            if (L < tr_dark_lo() || L > 0x60) return;
            y = tr_light_of_dark(L); c[0] = y; c[1] = y; c[2] = y;
        }
        walk_n++;
    }
}
static int ptr_ok(uint32_t p) { return !(p & 3) && p >= 0x08a00000 && p < 0x0a000000; }
/* text colours copied into views: a neutral dark one becomes the dark theme's text, a neutral
   light one goes back to black (accent/coloured text is left alone) */
static void remap_text(uint8_t *c)
{
    int L;
    if (!c[3] || tr_chroma(c[0], c[1], c[2]) > 24) return;
    L = tr_lum(c[0], c[1], c[2]);
    if (walk_mode && L < 0x60) { c[0] = theme_text[0]; c[1] = theme_text[1]; c[2] = theme_text[2]; walk_n++; }
    else if (!walk_mode && L >= 200) { c[0] = c[1] = c[2] = 0; walk_n++; }
}
/* A view's children are an array object embedded at view+0xa8. Its vtable slot 0x3c is
   GetAt(index, &out), which the OS iterator 0x081e3260 calls. A view has children when that word
   equals the root's children vtable; a smaller leaf object holds something else there. */
static uint32_t children_vt;
static int walk_nodes, dump_on;
static uint8_t *theme_screen;                       /* the object the render pass runs on */
static void walk_view(uint8_t *view, int depth);
/* calls fn(layer root view) for every layer the render pass draws */
static void for_each_layer_root(void (*fn)(uint8_t *, int), int arg)
{
    uint8_t *list;
    uint32_t n, i;
    if (!theme_screen) return;
    list = (uint8_t *)rd32(theme_screen + 0xe8);
    if (!ptr_ok((uint32_t)list)) return;
    n = rd32(list + 0x84);
    for (i = 0; i < n && i < 16; i++) {
        uint32_t *slot = ((uint32_t *(*)(void *, uint32_t))0x08299e20)(list + 0x80, i);
        uint8_t *group = ptr_ok((uint32_t)slot) ? (uint8_t *)*slot : 0, *layer;
        uint32_t iter[16];
        if (!group || !ptr_ok((uint32_t)group)) continue;
        ((void (*)(void *, void *))0x081ae5a0)(iter, group);
        while (((int (*)(void *, void *))0x081ae40c)(iter, &layer)) {
            uint8_t *lroot;
            if (!ptr_ok((uint32_t)layer)) break;
            lroot = (uint8_t *)rd32(layer + 0x78);
            log_s("   theme: layer "); log_x((uint32_t)layer, 8); log_s(" hidden "); log_d(layer[0x3e]); log_s(" root "); log_x((uint32_t)lroot, 8); log_c('\n');
            if (lroot && ptr_ok((uint32_t)lroot)) fn(lroot, arg);
        }
    }
}
static void layer_dump(uint8_t *lroot, int arg) { (void)arg; walk_nodes = 0; walk_view(lroot, 0); }
/* The main menu's right pane (class 0x0898597c) renders into a cached surface. Its slot 0x210
   (0x08137c48, called with a non-zero argument as the OS's refresh 0x081c6c9c does) re-renders
   it. Only after a live switch: at boot the pane is not ready. */
static void pane_find(uint8_t *view, int depth)
{
    uint32_t iter[2];
    void *child;
    if (!ptr_ok((uint32_t)view) || depth > 12) return;
    if (rd32(view) == 0x0898597c) {
        ((void (*)(void *, int))((void **)rd32(view))[0x210 / 4])(view, 1);
        log_s("   theme: menu pane re-rendered ("); log_x((uint32_t)view, 8); log_s(")\n");
        return;
    }
    if (rd32(view + 0xa8) != children_vt) return;
    os_view_iter_init(iter, view + 0xa8);
    while (os_view_iter_next(iter, &child)) pane_find((uint8_t *)child, depth + 1);
    os_view_iter_end(iter);
}
void theme_refresh_panes(void)
{
    uint8_t *root = ((uint8_t *(*)(void))0x0811340c)();
    if (!root || !ptr_ok((uint32_t)root)) return;
    children_vt = rd32(root + 0xa8);
    pane_find(root, 0);
}
static void layer_draw(uint8_t *lroot, int arg)
{
    int32_t full[4] = { 0, 0, 240, 320 };
    (void)arg;
    ((void (*)(void *, int, void *))((void **)rd32(lroot))[0x70 / 4])(lroot, 0, full);
}
static int light_left, dump_lines;
/* style 7 views draw the RGBA stored at +0x90; the style is (flags >> 15) & 7 at +0x48 */
static void remap_view(uint8_t *view)
{
    uint8_t *c = view + 0x90;
    if (((rd32(view + 0x48) >> 15) & 7) != 7) return;
    remap_colour(c);
    if (walk_mode && c[3] && tr_lum(c[0], c[1], c[2]) >= 200) light_left++;
}
static void walk_view(uint8_t *view, int depth)
{
    uint32_t iter[2];
    void *child;
    if (!ptr_ok((uint32_t)view) || depth > 12 || ++walk_nodes > 8000) return;
    if (dump_on && depth <= 10 && dump_lines < 260 &&
        (int)rd32(view + 0x88) - (int)rd32(view + 0x80) >= 8 && (int)rd32(view + 0x8c) - (int)rd32(view + 0x84) >= 8) {
        dump_lines++;                                     /* every sizeable view: class, flags, rect, colour, type/id */
        log_s("   v"); log_d(depth); log_c(' '); log_x((uint32_t)view, 8); log_c(' '); log_x(rd32(view), 8); log_c(' '); log_x(rd32(view + 0x48), 8);
        log_c(' '); log_d((int)rd32(view + 0x80)); log_c(','); log_d((int)rd32(view + 0x84)); log_c(' '); log_d((int)rd32(view + 0x88)); log_c('x'); log_d((int)rd32(view + 0x8c));
        log_c(' '); log_x(rd32(view + 0x90), 8);
        if (rd32(view + 0x44)) { log_c(' '); log_x(rd32(view + 0x40), 8); log_c('/'); log_x(rd32(view + 0x44), 8); }
        if (rd32(view) == 0x089a7178 || rd32(view) == 0x0898b884) { log_s(" txt "); log_x(rd32(view + 0xf0), 8); log_c('/'); log_x(rd32(view + 0xf4), 8); }
        log_c('\n');
    }
    remap_view(view);
    /* text views (plain 0x089a7178, marquee 0x0898b884) keep their own colour at +0xf4 when style
       flag 0x800 is set. Otherwise they use a named colour, which theme_named re-colours. */
    if ((rd32(view) == 0x089a7178 || rd32(view) == 0x0898b884) && (rd32(view + 0xf0) & 0x800)) remap_text(view + 0xf4);
    /* a scrolling-text view also keeps the colour as the tint of its canvas (+0xa4 + 0x11) and of
       each line's canvas (+0x248 + 108 i + 12 + 0x11), set when the mask was built (e_marq_ink) */
    if (rd32(view) == 0x0898b884) {
        uint32_t nl = rd32(view + 0x244), i;
        remap_text(view + 0xa4 + 0x11);
        for (i = 0; i < nl && i < 8; i++) remap_text(view + 0x248 + 108 * i + 12 + 0x11);
    }
    if (rd32(view + 0xa8) != children_vt) return;
    os_view_iter_init(iter, view + 0xa8);
    while (os_view_iter_next(iter, &child)) walk_view((uint8_t *)child, depth + 1);
    os_view_iter_end(iter);
}
/* After a live switch the screen repaints only what a view invalidated, so the status bar and
   other long-lived windows are repainted here. Slot 0x70 (0x08263b24) draws a view and its
   children within a rectangle. Called on the root with the full screen, it repaints everything. */
void theme_redraw_all(void)
{
    uint8_t *root = ((uint8_t *(*)(void))0x0811340c)();
    int32_t full[4] = { 0, 0, 240, 320 };
    if (root && ptr_ok((uint32_t)root)) ((void (*)(void *, int, void *))((void **)rd32(root))[0x70 / 4])(root, 0, full);
    for_each_layer_root(layer_draw, 0);
    log_s("   theme: full redraw of every layer requested\n");
}
void theme_walk(int mode, int accent)
{
    uint8_t *root = ((uint8_t *(*)(void))0x0811340c)();
    walk_live = applied_mode >= 0;           /* a switch after boot: the theme was applied before */
    walk_mode = mode; walk_accent = accent; walk_n = 0; walk_nodes = 0; light_left = 0;
    if (!root || !ptr_ok((uint32_t)root)) return;
    children_vt = rd32(root + 0xa8);
    walk_view(root, 0);
    log_s("   theme: live views remapped "); log_d(walk_n); log_s(" of "); log_d(walk_nodes); log_c('\n');
}
/* Now Playing lives outside the view tree until it is shown, so a switch cannot reach it. The
   remap runs again when its art is first requested after a switch. */
int theme_walk_pending;
/* Called from art.c when art is requested. The walk waits for the next render pass (hk_render),
   when Now Playing's screen is in the tree. */
void theme_dump_views(void)
{
    if (theme_walk_pending == 1) theme_walk_pending = 2;
}
static int first_render_logged;
static void __attribute__((used)) render_c(uint32_t *r)
{
    uint8_t *root;
    if (theme_walk_pending != 2) return;
    theme_walk_pending = 0;
    /* the render pass draws layers: screen+0xe8 is a list whose array (+0x80, count +0x84) holds
     * layer groups, walked with 0x081ae5a0 / 0x081ae40c. A layer's root view is at +0x78. */
    theme_screen = ptr_ok(r[0]) ? (uint8_t *)r[0] : 0;
    root = ((uint8_t *(*)(void))0x0811340c)();
    if (!root || !ptr_ok((uint32_t)root)) return;
    /* the walk remaps Now Playing's stored colours; the view dump runs only with e_debug (FLAC\debug.txt) */
    { extern int e_debug; dump_on = e_debug; }
    dump_lines = 0;
    log_t(8);
    if (dump_on) { log_s("   theme: views at the first render after art was asked (mode "); log_d(applied_mode); log_s("):\n"); }
    theme_walk(applied_mode < 0 ? 0 : applied_mode, applied_accent < 0 ? 0 : applied_accent);
    if (dump_on) { log_s("   theme: layers:\n"); for_each_layer_root(layer_dump, 0); }
    dump_on = 0;
    if (!first_render_logged) { first_render_logged = 1; log_s("   theme: first render walk, light stored colours left: "); log_d(light_left); log_c('\n'); }
}
/* 0x08113470 is the UI render pass (first instruction push {r4-r6, lr}) */
__attribute__((naked, section(".text.entry"), used)) void hk_render(void)
{
    __asm__ volatile(
        "push {r0-r5, ip, lr}\n mov r0, sp\n ldr ip, 1f\n mov lr, pc\n bx ip\n"
        "pop {r0-r5, ip, lr}\n"
        ".word 0xe92d4070\n ldr pc, 2f\n"
        "1: .word render_c\n 2: .word 0x08113474\n");
}

/* Diagnostics for the scrolling-text (marquee) draw 0x081a8444. Line count at +0x244; 108-byte line
 * structs from +0x248, each with a bitmap at +8 and a canvas at +12 (tint RGBA at +0x11). */
static int marq_logged = 6;      /* 6 turns the dump off; the hook stays in place */
static void __attribute__((used)) marq_c(uint32_t *r)
{
    uint8_t *m = (uint8_t *)r[0];
    uint32_t n, i;
    if (marq_logged >= 6 || !ptr_ok((uint32_t)m)) return;
    n = rd32(m + 0x244);
    if (n < 1 || !rd32(m + 0x248 + 8)) return;              /* no scroll mask built: a plain draw */
    marq_logged++;
    log_s("   marq "); log_x((uint32_t)m, 8); log_s(" class "); log_x(rd32(m), 8); log_s(" flags "); log_x(rd32(m + 0x48), 8);
    log_s(" lines "); log_d((int)n); log_s(" style "); log_x(rd32(m + 0xf0), 8); log_s(" col "); log_x(rd32(m + 0xf4), 8);
    log_s(" canvas tint "); log_x(rd32(m + 0xa4 + 0x11), 8); log_s(" bg "); log_x(rd32(m + 0xa4 + 0x15), 8); log_c('\n');
    for (i = 0; i < n && i < 3; i++) {
        uint8_t *line = m + 0x248 + 108 * i, *bmp = (uint8_t *)rd32(line + 8), *cv = line + 12, *img, *d;
        uint32_t k, hist[4] = { 0, 0, 0, 0 }, bpp, len;
        log_s("    line "); log_d((int)i); log_s(" bmp "); log_x((uint32_t)bmp, 8);
        log_s(" cv tint "); log_x(rd32(cv + 0x11), 8); log_s(" bg "); log_x(rd32(cv + 0x15), 8); log_s(" ref "); log_x(rd32(cv + 0x1c), 8);
        if (!ptr_ok((uint32_t)bmp)) { log_c('\n'); continue; }
        img = bmp + 4; d = (uint8_t *)rd32(img); bpp = rd32(img + 8);
        log_s(" fmt "); log_x(rd32(bmp + 0xa8), 2); log_s(" bpp "); log_d((int)bpp); log_s(" iflags "); log_x(rd32(img + 0x10) & 0xffff, 4);
        log_s(" rect "); log_d((int)rd32(bmp + 0x98)); log_c(','); log_d((int)rd32(bmp + 0x9c)); log_c(' '); log_d((int)rd32(bmp + 0xa0)); log_c('x'); log_d((int)rd32(bmp + 0xa4));
        log_s(" data "); log_x((uint32_t)d, 8);
        if (ptr_ok((uint32_t)d)) {
            len = (rd32(bmp + 0xa0) - rd32(bmp + 0x98)) * (rd32(bmp + 0xa4) - rd32(bmp + 0x9c)) * (bpp ? bpp : 8) / 8;
            if (len > 4096) len = 4096;
            for (k = 0; k < len; k++) { uint8_t b = d[k]; hist[b == 0 ? 0 : b == 0xff ? 3 : b < 0x80 ? 1 : 2]++; }
            log_s(" hist 00:"); log_d((int)hist[0]); log_s(" lo:"); log_d((int)hist[1]); log_s(" hi:"); log_d((int)hist[2]); log_s(" ff:"); log_d((int)hist[3]);
            log_s(" head"); for (k = 0; k < 16; k++) { log_c(' '); log_x(d[k], 2); }
        }
        log_c('\n');
    }
    log_flush();
}
/* Scrolling-text ink, called in place of 0x081a8228 (`bl` at 0x081a7e70). The blit takes the ink
 * from the white-filled mask and the colour from the line canvas tint, so the ink is forced to black
 * during the draw and the real colour is put back into the tint afterwards. */
typedef void (*marq_text_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t *);
E_ENTRY void e_marq_ink(uint32_t view, uint32_t canvas, uint32_t rect, uint32_t str, uint32_t s0, uint32_t s1, uint32_t s2, uint8_t *col)
{
    uint8_t save[4], *t = (uint8_t *)canvas + 0x11;
    save[0] = col[0]; save[1] = col[1]; save[2] = col[2]; save[3] = col[3];
    col[0] = col[1] = col[2] = 0; col[3] = 0xff;
    ((marq_text_t)0x081a8228)(view, canvas, rect, str, s0, s1, s2, col);
    t[0] = save[0]; t[1] = save[1]; t[2] = save[2]; t[3] = save[3];
    col[0] = save[0]; col[1] = save[1]; col[2] = save[2]; col[3] = save[3];
}
/* 0x081a8444 is the marquee draw (first instruction push {r0-r2, r4-fp, lr}) */
__attribute__((naked, section(".text.entry"), used)) void hk_marq(void)
{
    __asm__ volatile(
        "push {r0-r5, ip, lr}\n mov r0, sp\n ldr ip, 1f\n mov lr, pc\n bx ip\n"
        "pop {r0-r5, ip, lr}\n"
        ".word 0xe92d4ff7\n ldr pc, 2f\n"
        "1: .word marq_c\n 2: .word 0x081a8448\n");
}
/* ---- apply ---- */
int theme_is_applied(int mode, int accent) { return applied_mode == mode && applied_accent == accent; }
int theme_apply(int mode, int accent, int rebuild_ui)
{
    const uint8_t *acc;
    uint32_t i, size, v;
    int bad = 0, done = 0;
    uint8_t rgb[3];
    if (accent < 0 || accent >= NACCENT) accent = 0;
    acc = accents[accent];
    for (i = 0; i < sizeof theme_bmap_chrome / 4 + sizeof theme_bmap_bg / 4 + sizeof theme_bmap_accent / 4; i++) {
        int cls = i < sizeof theme_bmap_chrome / 4 ? 0 : i < sizeof theme_bmap_chrome / 4 + sizeof theme_bmap_bg / 4 ? 1 : 2;
        uint32_t id = cls == 0 ? theme_bmap_chrome[i] : cls == 1 ? theme_bmap_bg[i - sizeof theme_bmap_chrome / 4]
                    : theme_bmap_accent[i - sizeof theme_bmap_chrome / 4 - sizeof theme_bmap_bg / 4];
        const uint8_t *src = orig_of(T_BMAP, id, &size);
        uint8_t *d;
        if (!src) { bad++; continue; }
        d = (mode || accent || cls == 2 || theme_style) ? bmap_themed(src, size, cls, acc, mode, id) : 0;
        if (rsrc_write(T_BMAP, id, d ? d : src, size)) bad++; else done++;
        if (d) os_free(d);
    }
    for (i = 0; i < sizeof theme_colr / 4; i++) {
        const uint8_t *src = orig_of(T_COLR, theme_colr[i], &size);
        if (!src || size < 4) { bad++; continue; }
        v = rd32(src);
        if (mode && colr_is_fg(theme_colr[i]) && tr_lum((v >> 16) & 255, (v >> 8) & 255, v & 255) >= 128) {
            /* light text or icon on a panel: stays light */
        } else if (mode) {
            tr_chrome_px((v >> 16) & 255, (v >> 8) & 255, v & 255, acc, 0, rgb);
            v = (v & 0xff000000) | (uint32_t)rgb[0] << 16 | rgb[1] << 8 | rgb[2];
        } else if (accent && tr_is_apple_blue((v >> 16) & 255, (v >> 8) & 255, v & 255)) {
            tr_accent_px((v >> 16) & 255, (v >> 8) & 255, v & 255, acc, rgb);
            v = (v & 0xff000000) | (uint32_t)rgb[0] << 16 | rgb[1] << 8 | rgb[2];
        }
        if (rsrc_write(T_COLR, theme_colr[i], &v, 4)) bad++; else done++;
    }
    theme_named(mode, accent, acc);
    apply_code_colours(mode, accent);
    applied_mode = mode; applied_accent = accent;
    log_s("   theme: "); log_s(mode ? "dark, accent " : "light"); if (mode) log_s(accent_names[accent]);
    log_s(", resources patched "); log_d(done); log_s(", failed "); log_d(bad); log_c('\n');
    (void)rebuild_ui;
    return bad;
}
