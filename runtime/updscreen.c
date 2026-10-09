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

/* A boot progress bar drawn this way (R26) broke the display on the iPod: Apple's frame engine
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
