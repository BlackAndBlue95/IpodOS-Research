/* Region E log: text collected in memory, written to FLAC\rtlog.txt on log_flush(). */
#include "e.h"

static char logb[16384];
static int logn;

void log_c(char c) { if (logn < (int)sizeof logb - 1) logb[logn++] = c; }
void log_s(const char *s) { while (*s) log_c(*s++); }
void log_x(uint32_t v, int digits) { while (digits--) log_c("0123456789abcdef"[(v >> (4 * digits)) & 15]); }
void log_d(int v)
{
    char t[12];
    int i = 0;
    uint32_t u = v < 0 ? -(uint32_t)v : (uint32_t)v;
    if (v < 0) log_c('-');
    do { t[i++] = '0' + u % 10; u /= 10; } while (u);
    while (i) log_c(t[--i]);
}
void log_name(const char *s)    /* printable bytes as is, others as <xx> */
{
    for (; *s; s++) {
        unsigned char c = *s;
        if (c >= 32 && c < 127) log_c(c);
        else { log_c('<'); log_x(c, 2); log_c('>'); }
    }
}

/* decoder trace words, 256 bytes per dump to FLAC\flaclogXY.bin, only with FLAC\debug.txt */
extern uint32_t flac_trace[64];
extern int e_debug;
void flac_dump(void)
{
    static int n;
    char name[] = "FLAC\\flaclog00.bin";
    if (!e_debug || n >= 200) return;
    n++;
    name[12] = 'a' + (n >> 4);
    name[13] = 'a' + (n & 15);
    file_write(name, flac_trace, sizeof flac_trace);
}

/* boot timeline: TIMER_E (1 MHz, running since the bootloader) at fixed points */
static uint32_t tl[12];
static const char *const tl_name[12] = { "appinit", "libload", "mount", "sync", "oslib", "art", "done", "ui", "render", 0, 0, 0 };
void log_t(int slot) { if (slot >= 0 && slot < 12) tl[slot] = TIMER_E; }
void log_timeline(void)
{
    int i;
    log_s("   timeline (ms): ");
    for (i = 0; i < 12 && tl_name[i]; i++) {
        if (!tl[i]) continue;
        if (i) log_s(", ");
        log_s(tl_name[i]); log_c(' ');
        if (i == 0) log_d((int)(tl[0] / 1000));
        else { log_c('+'); log_d((int)((tl[i] - tl[i - 1 - (i > 1 && !tl[i - 1])]) / 1000)); }
    }
    log_c('\n');
}

/* replace FLAC\rtlog.txt with the log so far */
int log_flush(void)
{
    static int first = 1;
    uint32_t n = logn;
    int rc;
    if (first) {
        /* the previous run's log is kept as rtlog.prev.txt. Copied, not renamed: creating a file
           under a name just renamed away fails in this file layer. */
        static char prev[16384];
        int got = file_read("FLAC\\rtlog.txt", prev, sizeof prev, 0);
        first = 0;
        vol_delete("FLAC\\rtlog.prev.txt");
        if (got > 0) file_write("FLAC\\rtlog.prev.txt", prev, (uint32_t)got);
    }
    vol_delete("FLAC\\rtlog.txt");
    rc = file_write("FLAC\\rtlog.txt", logb, n);
    return rc;
}
