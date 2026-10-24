/* Progress bar for the self-update, drawn straight to the LCD interface (0x38300000) the way the
 * bootloader's driver does (Rockbox lcd-s5l8702.c): command mode, window, frame mode, pixels to the
 * data port. The update runs before Apple's LCD init (0x08143c04), so the panel and the interface
 * are still as the bootloader left them, showing its text. LCD_CON is restored after each draw.
 * Every status wait is bounded; one that times out turns the drawing off, never the update. */
#include "e.h"

#define LCD_CON    (*(volatile uint32_t *)0x38300000)
#define LCD_WCMD   (*(volatile uint32_t *)0x38300004)
#define LCD_STATUS (*(volatile uint32_t *)0x3830001c)
#define LCD_WDATA  (*(volatile uint32_t *)0x38300040)
#define PDAT6      (*(volatile uint32_t *)0x3cf000c4)
#define MODE_P8    0x80000c20u                 /* commands, 8-bit command set (LCD types 0, 1) */
#define MODE_P18   0x80000da8u                 /* commands, 16-bit command set (types 2, 3) */
#define MODE_P16   0x80100db0u                 /* RGB565 pixels */

#define BAR_X 20
#define BAR_Y 196
#define BAR_W 280
#define BAR_H 14

static int ok, cmd16, filled;
static uint32_t cmd_mode;

static int wait(uint32_t mask, int set)
{
    int n;
    for (n = 0; n < 100000; n++) if (!(LCD_STATUS & mask) == !set) return 1;
    ok = 0;
    return 0;
}
static void con(uint32_t v)
{
    volatile int d;
    if (!ok || !wait(2, 1)) return;
    for (d = 0; d < 300; d++) ;
    LCD_CON = v;
}
static void cmd(uint32_t c) { if (ok && wait(0x10, 0)) LCD_WCMD = c; }
static void dat(uint32_t v) { if (ok && wait(0x10, 0)) LCD_WDATA = v; }
static void reg(uint32_t r, uint32_t v) { cmd(r); dat(v); }

static void rect(int x, int y, int w, int h, uint16_t c)
{
    int xe = x + w - 1, ye = y + h - 1, n;
    uint32_t saved;
    if (!ok || w <= 0 || h <= 0) return;
    saved = LCD_CON;
    con(cmd_mode);
    if (cmd16) {
        reg(0x210, x); reg(0x211, xe); reg(0x212, y); reg(0x213, ye);
        reg(0x200, x); reg(0x201, y); cmd(0x202);
    } else {
        cmd(0x2a); dat(x >> 8); dat(x & 0xff); dat(xe >> 8); dat(xe & 0xff);
        cmd(0x2b); dat(y >> 8); dat(y & 0xff); dat(ye >> 8); dat(ye & 0xff);
        cmd(0x2c);
    }
    con(MODE_P16);
    for (n = w * h; n > 0 && ok; n--) dat(c);
    con(saved);
}

/* A boot progress bar drawn this way broke the display on the iPod: Apple's frame engine
   already streams frames there after UI init, and window commands sent then corrupt the screen
   and crash the OS. The LCD interface must not be touched once Apple's display is up. */

/* empty bar, outlined */
void updscreen_begin(void)
{
    int t = (PDAT6 & 0x30) >> 4;
    ok = 1; filled = 0;
    cmd16 = t >= 2;
    cmd_mode = cmd16 ? MODE_P18 : MODE_P8;
    rect(BAR_X - 2, BAR_Y - 2, BAR_W + 4, BAR_H + 4, 0x8410);
    rect(BAR_X, BAR_Y, BAR_W, BAR_H, 0x0000);
}

/* ---- boot status: the latest step centred on a black screen, its 5x7 font drawn at twice the
   size with smoothed corners, and a thin bar for the steps done (the full log is FLAC\rtlog.txt) ---- */
static const char glyph_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:/.-,()%"
                                  "abcdefghijklmnopqrstuvwxyz[]>_+'=!";
static const uint8_t glyphs[][7] = {
    { 14, 17, 17, 31, 17, 17, 17 }, { 30, 17, 17, 30, 17, 17, 30 }, { 14, 17, 16, 16, 16, 17, 14 }, { 30, 17, 17, 17, 17, 17, 30 },
    { 31, 16, 16, 30, 16, 16, 31 }, { 31, 16, 16, 30, 16, 16, 16 }, { 14, 17, 16, 23, 17, 17, 15 }, { 17, 17, 17, 31, 17, 17, 17 },
    { 14, 4, 4, 4, 4, 4, 14 },      { 7, 2, 2, 2, 2, 18, 12 },      { 17, 18, 20, 24, 20, 18, 17 }, { 16, 16, 16, 16, 16, 16, 31 },
    { 17, 27, 21, 21, 17, 17, 17 }, { 17, 17, 25, 21, 19, 17, 17 }, { 14, 17, 17, 17, 17, 17, 14 }, { 30, 17, 17, 30, 16, 16, 16 },
    { 14, 17, 17, 17, 21, 18, 13 }, { 30, 17, 17, 30, 20, 18, 17 }, { 15, 16, 16, 14, 1, 1, 30 },  { 31, 4, 4, 4, 4, 4, 4 },
    { 17, 17, 17, 17, 17, 17, 14 }, { 17, 17, 17, 17, 17, 10, 4 },  { 17, 17, 17, 21, 21, 21, 10 }, { 17, 17, 10, 4, 10, 17, 17 },
    { 17, 17, 10, 4, 4, 4, 4 },     { 31, 1, 2, 4, 8, 16, 31 },
    { 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 },      { 14, 17, 1, 2, 4, 8, 31 },     { 31, 2, 4, 2, 1, 17, 14 },
    { 2, 6, 10, 18, 31, 2, 2 },     { 31, 16, 30, 1, 1, 17, 14 },   { 6, 8, 16, 30, 17, 17, 14 },   { 31, 1, 2, 4, 8, 8, 8 },
    { 14, 17, 17, 14, 17, 17, 14 }, { 14, 17, 17, 15, 1, 2, 12 },
    { 0, 12, 12, 0, 12, 12, 0 },    { 1, 2, 2, 4, 8, 8, 16 },       { 0, 0, 0, 0, 0, 12, 12 },      { 0, 0, 0, 31, 0, 0, 0 },
    { 0, 0, 0, 0, 12, 4, 8 },       { 2, 4, 8, 8, 8, 4, 2 },        { 8, 4, 2, 2, 2, 4, 8 },        { 24, 25, 2, 4, 8, 19, 3 },
    { 0b00000, 0b00000, 0b01110, 0b00001, 0b01111, 0b10001, 0b01111 }, { 0b10000, 0b10000, 0b10110, 0b11001, 0b10001, 0b10001, 0b11110 },
    { 0b00000, 0b00000, 0b01110, 0b10000, 0b10000, 0b10001, 0b01110 }, { 0b00001, 0b00001, 0b01101, 0b10011, 0b10001, 0b10001, 0b01111 },
    { 0b00000, 0b00000, 0b01110, 0b10001, 0b11111, 0b10000, 0b01110 }, { 0b00110, 0b01001, 0b01000, 0b11100, 0b01000, 0b01000, 0b01000 },
    { 0b00000, 0b01111, 0b10001, 0b10001, 0b01111, 0b00001, 0b01110 }, { 0b10000, 0b10000, 0b10110, 0b11001, 0b10001, 0b10001, 0b10001 },
    { 0b00100, 0b00000, 0b01100, 0b00100, 0b00100, 0b00100, 0b01110 }, { 0b00010, 0b00000, 0b00110, 0b00010, 0b00010, 0b10010, 0b01100 },
    { 0b10000, 0b10000, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010 }, { 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 },
    { 0b00000, 0b00000, 0b11010, 0b10101, 0b10101, 0b10001, 0b10001 }, { 0b00000, 0b00000, 0b10110, 0b11001, 0b10001, 0b10001, 0b10001 },
    { 0b00000, 0b00000, 0b01110, 0b10001, 0b10001, 0b10001, 0b01110 }, { 0b00000, 0b00000, 0b11110, 0b10001, 0b11110, 0b10000, 0b10000 },
    { 0b00000, 0b00000, 0b01101, 0b10011, 0b01111, 0b00001, 0b00001 }, { 0b00000, 0b00000, 0b10110, 0b11001, 0b10000, 0b10000, 0b10000 },
    { 0b00000, 0b00000, 0b01110, 0b10000, 0b01110, 0b00001, 0b11110 }, { 0b01000, 0b01000, 0b11100, 0b01000, 0b01000, 0b01001, 0b00110 },
    { 0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b10011, 0b01101 }, { 0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100 },
    { 0b00000, 0b00000, 0b10001, 0b10001, 0b10101, 0b10101, 0b01010 }, { 0b00000, 0b00000, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001 },
    { 0b00000, 0b00000, 0b10001, 0b10001, 0b01111, 0b00001, 0b01110 }, { 0b00000, 0b00000, 0b11111, 0b00010, 0b00100, 0b01000, 0b11111 },
    { 0b01110, 0b01000, 0b01000, 0b01000, 0b01000, 0b01000, 0b01110 }, { 0b01110, 0b00010, 0b00010, 0b00010, 0b00010, 0b00010, 0b01110 },
    { 0b01000, 0b00100, 0b00010, 0b00001, 0b00010, 0b00100, 0b01000 }, { 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11111 },
    { 0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000 }, { 0b00100, 0b00100, 0b01000, 0b00000, 0b00000, 0b00000, 0b00000 },
    { 0b00000, 0b00000, 0b11111, 0b00000, 0b11111, 0b00000, 0b00000 }, { 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100 },
};
#define CON_Y 140                              /* the status line: 16 px, 12 px per character */
#define CON_CHARS 26
#define STEP_X 100                             /* the steps bar */
#define STEP_Y 184
#define STEP_W 120
#define STEP_N 10                              /* about as many steps as a boot prints */
static int boot_text, steps;
static char con_last[16];                      /* the line a status may rewrite in place */

/* from the library load at boot until Apple's LCD init: the bootloader's panel set-up is still in place */
void bootscreen_begin(void)
{
    int t = (PDAT6 & 0x30) >> 4;
    ok = 1; boot_text = 1;
    cmd16 = t >= 2;
    cmd_mode = cmd16 ? MODE_P18 : MODE_P8;
    rect(0, 0, 320, 240, 0x0000);              /* the bootloader's lines go */
    rect(STEP_X, STEP_Y, STEP_W, 3, 0x2945);
}
void bootscreen_end(void) { boot_text = 0; }

static int glyph_on(int k, int gx, int gy) { return k != 0xff && gx >= 0 && gx < 5 && gy >= 0 && gy < 7 && (glyphs[k][gy] >> (4 - gx) & 1); }
/* the status line, centred; a doubled pixel's outer quarter is grey where two of its neighbours meet */
static void con_line(int row, const char *s, int len)
{
    int n = 0, x0, y0 = CON_Y + 18 * row, w = CON_CHARS * 12, h = 16, x, y, xe, ye;
    uint8_t idx[CON_CHARS];
    uint32_t saved;
    while (n < CON_CHARS && n < len && s[n]) {
        int k;
        for (k = 0; glyph_chars[k] && glyph_chars[k] != s[n]; k++);
        idx[n++] = glyph_chars[k] ? (uint8_t)k : 0xff;
    }
    x0 = (320 - w) / 2; xe = x0 + w - 1; ye = y0 + h - 1;
    if (!ok || !boot_text) return;
    saved = LCD_CON;
    con(cmd_mode);
    if (cmd16) {
        reg(0x210, x0); reg(0x211, xe); reg(0x212, y0); reg(0x213, ye);
        reg(0x200, x0); reg(0x201, y0); cmd(0x202);
    } else {
        cmd(0x2a); dat(x0 >> 8); dat(x0 & 0xff); dat(xe >> 8); dat(xe & 0xff);
        cmd(0x2b); dat(y0 >> 8); dat(y0 & 0xff); dat(ye >> 8); dat(ye & 0xff);
        cmd(0x2c);
    }
    con(MODE_P16);
    for (y = 0; y < h && ok; y++)
        for (x = 0; x < w && ok; x++) {
            int c = (x - (w - n * 12) / 2), ch = c / 12, gx = c % 12 / 2, gy = y / 2 - 1, dx = c % 2 ? 1 : -1, dy = y % 2 ? 1 : -1, k;
            uint16_t px = 0x0000;
            if (c >= 0 && ch < n && c % 12 < 10) {
                k = idx[ch];
                if (glyph_on(k, gx, gy)) px = 0xe71c;
                else if (glyph_on(k, gx + dx, gy) && glyph_on(k, gx, gy + dy)) px = 0x738e;
            }
            dat(px);
        }
    con(saved);
}
/* two lines at most, broken at the last space that fits */
static void con_draw(const char *s)
{
    int n = 0, cut;
    while (s[n]) n++;
    cut = n;
    if (n > CON_CHARS) for (cut = CON_CHARS; cut > 0 && s[cut] != ' '; cut--);
    if (!cut) cut = CON_CHARS;
    con_line(0, s, cut);
    con_line(1, s + cut + (s[cut] == ' '), n);
}

/* the next step: shown alone, and one more of the bar */
void bootscreen_print(const char *s)
{
    int x;
    if (!boot_text) return;
    con_draw(s);
    con_last[0] = 0;
    if (steps < STEP_N) steps++;
    x = STEP_W * steps / STEP_N;
    rect(STEP_X, STEP_Y, x, 3, 0xe71c);
}

/* a progress message: rewrites the line, the bar stays, while it starts the same way ("Reading tags 9/52") */
void bootscreen_status(const char *s)
{
    int k;
    if (!boot_text) return;
    for (k = 0; k < 12 && s[k] && s[k] == con_last[k]; k++);
    if (con_last[0] && k == 12) con_draw(s);
    else bootscreen_print(s);
    for (k = 0; k < 15 && s[k]; k++) con_last[k] = s[k];
    con_last[k] = 0;
}

/* k of n done, in colour c: only the new part is drawn; a new colour restarts from the left */
void updscreen_progress(uint32_t k, uint32_t n, uint16_t c)
{
    static uint16_t last_c;
    int x = n ? (int)((uint64_t)BAR_W * k / n) : BAR_W;
    if (c != last_c) { if (filled) rect(BAR_X, BAR_Y, BAR_W, BAR_H, 0x0000); filled = 0; last_c = c; }
    if (x > filled) { rect(BAR_X + filled, BAR_Y, x - filled, BAR_H, c); filled = x; }
}

/* the whole bar in one colour (done, failed) */
void updscreen_fill(uint16_t c) { rect(BAR_X, BAR_Y, BAR_W, BAR_H, c); filled = BAR_W; }

int updscreen_ok(void) { return ok; }
