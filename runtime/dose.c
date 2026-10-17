/* Weekly sound dose (ITU-T H.870): the decoder's running total (flac/hearing.c) is booked to the
 * day it was heard on the minute tick; the last 7 days are the weekly figure. Kept in FLAC\dose.txt,
 * written only from the library load: the minute tick can run inside a FAT driver call. */
#include "e.h"
#include "flac/hearing.h"

#define os_get_datetime ((int (*)(uint8_t *))0x08083b08)      /* sec, min, hour, mday, month, -, u16 year, wday */
#define DOSE_FILE "FLAC\\dose.txt"

static int32_t day_no[7];
static uint32_t day_ppm[7], seen, unsaved;
static int loaded;

static int32_t today(void)
{
    uint8_t dt[16];
    int i, y, m, d;
    for (i = 0; i < 16; i++) dt[i] = 0;
    os_get_datetime(dt);
    y = dt[6] | dt[7] << 8; m = dt[4]; d = dt[3];
    if (m < 1 || m > 12 || d < 1) return 0;
    y -= m <= 2;                                               /* days from civil (H. Hinnant) */
    {
        int era = y / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        return era * 146097 + (yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
    }
}

static void dose_save(void)
{
    char buf[7 * 24];
    int n = 0, k;
    if (!loaded) return;
    for (k = 0; k < 7; k++) {
        uint32_t v[2] = { (uint32_t)day_no[k], day_ppm[k] };
        int j;
        for (j = 0; j < 2; j++) {
            char t[12]; int m = 0;
            do t[m++] = '0' + v[j] % 10; while (v[j] /= 10);
            while (m) buf[n++] = t[--m];
            buf[n++] = j ? '\n' : ' ';
        }
    }
    vol_delete(DOSE_FILE);
    file_write(DOSE_FILE, buf, n);
    unsaved = 0;
}

/* at the settings load (the file system is up) */
void dose_load(void)
{
    char buf[256];
    int n, sz, i = 0, k = 0;
    seen = hear_dose_acc;
    loaded = 1;
    n = file_read(DOSE_FILE, buf, sizeof buf - 1, &sz);
    if (n <= 0) return;
    buf[n] = 0;
    while (k < 7 && i < n) {
        uint32_t a = 0, b = 0;
        while (buf[i] >= '0' && buf[i] <= '9') a = a * 10 + buf[i++] - '0';
        if (buf[i++] != ' ') break;
        while (buf[i] >= '0' && buf[i] <= '9') b = b * 10 + buf[i++] - '0';
        if (buf[i] == '\r') i++;
        if (buf[i++] != '\n') break;
        day_no[a % 7] = (int32_t)a; day_ppm[a % 7] = b;
        k++;
    }
}

uint32_t dose_week_ppm(void)
{
    int32_t t = today();
    uint32_t s = 0;
    int k;
    for (k = 0; k < 7; k++) if (day_no[k] <= t && day_no[k] > t - 7) s += day_ppm[k];
    return s;
}

/* minute tick */
void dose_tick(void)
{
    uint32_t acc = hear_dose_acc, inc;
    int32_t t;
    int slot;
    if (!loaded) return;
    inc = acc - seen; seen = acc;
    t = today(); slot = (t % 7 + 7) % 7;
    if (day_no[slot] != t) { day_no[slot] = t; day_ppm[slot] = 0; }
    day_ppm[slot] += inc;
    unsaved += inc;
}

/* library load: what was heard since the last write, if anything */
void dose_commit(void) { if (unsaved) dose_save(); }

/* "Sound dose 37% this week" */
const char *dose_text(void)
{
    static char buf[40];
    uint32_t p = (dose_week_ppm() + 500) / 1000;               /* tenths of a percent */
    int n = 0;
    const char *s;
    char t[12]; int m = 0;
    for (s = "Sound dose "; *s; s++) buf[n++] = *s;
    if (p < 100) { buf[n++] = '0' + p / 10; buf[n++] = '.'; buf[n++] = '0' + p % 10; }
    else { p /= 10; do t[m++] = '0' + p % 10; while (p /= 10); while (m) buf[n++] = t[--m]; }
    for (s = "% this week"; *s; s++) buf[n++] = *s;
    buf[n] = 0;
    return buf;
}
