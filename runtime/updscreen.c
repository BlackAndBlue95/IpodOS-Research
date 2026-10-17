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

/* ---- boot console: lines added below the bootloader's, in a 5x7 font on its 6x8 grid ---- */
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
#define CON_X 0
#define CON_Y 64                               /* below the bootloader's lines (sysfont, 8 px each) */
#define CON_ROWS 22
#define CON_CHARS 53                           /* 6 px per character */
static int boot_text, con_row;
static char con_last[16];                      /* the line a status may rewrite in place */

/* from the library load at boot until Apple's LCD init: the bootloader's panel set-up is still in place */
void bootscreen_begin(void)
{
    int t = (PDAT6 & 0x30) >> 4;
    ok = 1; boot_text = 1;
    cmd16 = t >= 2;
    cmd_mode = cmd16 ? MODE_P18 : MODE_P8;
}
void bootscreen_end(void) { boot_text = 0; }

/* one console row: the text, the rest of the row cleared */
static void con_draw(int row, const char *s)
{
    int x0 = CON_X, y0 = CON_Y + row * 8, w = CON_CHARS * 6, h = 8, x, y, xe = x0 + w - 1, ye = y0 + h - 1;
    uint8_t idx[CON_CHARS];
    uint32_t saved;
    for (x = 0; x < CON_CHARS; x++) {
        char c = s && *s ? *s++ : ' ';
        int k;
        for (k = 0; glyph_chars[k] && glyph_chars[k] != c; k++);
        idx[x] = glyph_chars[k] ? (uint8_t)k : 0xff;
    }
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
            int ch = x / 6, gx = x % 6, on = 0;
            if (idx[ch] != 0xff && gx < 5 && y < 7) on = glyphs[idx[ch]][y] >> (4 - gx) & 1;
            dat(on ? 0xffff : 0x0000);
        }
    con(saved);
}

/* "[  1.23] text" on the next row, as a Linux boot does; the rows wrap to the top when full */
void bootscreen_print(const char *s)
{
    char line[CON_CHARS + 1];
    uint32_t cs = TIMER_E / 10000;                 /* hundredths of a second */
    int n = 0, k;
    char t[8];
    if (!boot_text) return;
    line[n++] = '[';
    for (k = 0; k < 7; k++) { t[6 - k] = k == 2 ? '.' : (k > 3 && !cs) ? ' ' : '0' + cs % 10; if (k != 2) cs /= 10; }
    t[7] = 0;
    for (k = 0; k < 7 && n < CON_CHARS; k++) line[n++] = t[k];
    line[n++] = ']'; line[n++] = ' ';
    while (*s && n < CON_CHARS) line[n++] = *s++;
    line[n] = 0;
    if (con_row >= CON_ROWS) con_row = 0;
    con_draw(con_row++, line);
    con_last[0] = 0;
}

/* a progress message: rewrites the previous row while it starts the same way ("Reading tags 9/52") */
void bootscreen_status(const char *s)
{
    int k;
    if (!boot_text) return;
    for (k = 0; k < 12 && s[k] && s[k] == con_last[k]; k++);
    if (con_last[0] && k == 12 && con_row) con_row--;
    bootscreen_print(s);
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
