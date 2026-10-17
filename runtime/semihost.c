/* ARM semihosting (svc 0x123456), used by Apple's RTXCbug kernel debugger and a few debug prints.
   The SWI vector stub 0x0802dd14 calls e_swi(regs) with the caller's r0-r12, lr (restored after). */
#include "e.h"

#define SCRIPT    "FLAC\\rtxcbug.txt"
#define OUT_FILE  "FLAC\\semihost.txt"
#define OS_FILE   "FLAC\\semihost-os.txt"   /* Apple's prints outside a session */
#define SESSION   0x20000                /* output buffer while RTXCbug runs (the resource list alone is ~45 KB) */
#define MAX_READS 400                    /* input lines per session, then only the exit line */

#define KS_SIGNAL    ((void (*)(int))0x0802cfa0)
#define RTXCBUG_SEM  56                  /* task 0x0835d424 waits on it (priority 0), nothing in the OS signals it */
#define RTXCBUG_OBJ  ((void (*)(void))0x082b4bac)   /* RTXCbug's object menu (K): one snapshot per input line */

#define SPARE     64                     /* kept free for the 'output full' note */

static char idle[2048];                  /* prints outside a session */
static char *out = idle;
static int out_cap = sizeof idle - SPARE, out_n, out_lost;
static const char *in, *in_end;          /* script lines, then in_end forever */
static int in_n, in_pos, reads, active, direct;

static void put(const char *s, int n)
{
    for (; n > 0; n--, s++) {
        if (out_n < out_cap) out[out_n++] = *s;
        else out_lost++;
    }
}
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* the next script line (with its newline) into buf, echoed to the output; returns its length */
static int next_line(char *buf, int max)
{
    const char *s = in_end;
    int n = 0, len;
    if (++reads <= MAX_READS && in_pos < in_n) {
        s = in + in_pos;
        while (in_pos + n < in_n && s[n] != '\n') n++;
        in_pos += n + (in_pos + n < in_n);
        len = n;
        while (len && s[len - 1] == '\r') len--;
        /* direct: no block (!), task manager ($), unblock and exit (g, x); the caller's task must keep running */
        if (direct && len && (s[0] == '!' || s[0] == '$' || (s[0] | 0x20) == 'g' || (s[0] | 0x20) == 'x')) s = in_end, len = 1;
    } else len = slen(s) - 1;
    for (n = 0; n < len && n < max - 1; n++) buf[n] = s[n];
    buf[n++] = '\n';
    put(buf, n);
    return n;
}

E_ENTRY void e_swi(uint32_t *regs)
{
    uint32_t spsr, lr = regs[13], *a = (uint32_t *)regs[1];
    __asm__ volatile("mrs %0, spsr" : "=r"(spsr));
    if (spsr & 0x20 ? (*(uint16_t *)(lr - 2) & 0xff) != 0xab : (*(uint32_t *)(lr - 4) & 0xffffff) != 0x123456) return;
    switch (regs[0]) {
    case 0x03: put((const char *)a, 1); return;                   /* SYS_WRITEC */
    case 0x04: put((const char *)a, slen((const char *)a)); return;   /* SYS_WRITE0 */
    case 0x05: put((const char *)a[1], (int)a[2]); break;         /* SYS_WRITE {h, buf, len}: C stdio (_sys_write 0x080270e4), asserts */
    }
    if (!active) return;                 /* otherwise as stock: r0 comes back unchanged */
    switch (regs[0]) {
    case 0x01: regs[0] = 1; break;                                /* SYS_OPEN: ":tt" */
    case 0x02: regs[0] = 0; break;                                /* SYS_CLOSE */
    case 0x05: regs[0] = 0; break;                                /* SYS_WRITE: nothing left unwritten */
    case 0x06: {                                                  /* SYS_READ {h, buf, len}: left unread */
        int n = a[2] ? next_line((char *)a[1], (int)a[2]) : 0;
        regs[0] = a[2] - n;
        break;
    }
    case 0x07: put(" \n", 2); regs[0] = ' '; break;               /* SYS_READC: the pager's key, go on */
    case 0x09: regs[0] = 1; break;                                /* SYS_ISTTY */
    case 0x10: regs[0] = TIMER_E / 10000; break;                  /* SYS_CLOCK, centiseconds */
    }
}

static void begin(const char *script, int n, const char *end, int dir)
{
    char *b = os_malloc(SESSION);
    uint32_t irq, tmp;
    __asm__ volatile("mrs %0, cpsr\n orr %1, %0, #0x80\n msr cpsr_c, %1" : "=r"(irq), "=r"(tmp));
    if (b) {
        int i;
        for (i = 0; i < out_n; i++) b[i] = out[i];               /* earlier prints first */
        out = b; out_cap = SESSION - SPARE;
        if (out_lost) { out_lost = 0; put("[earlier prints cut]\n", 21); }   /* the idle buffer's loss, not the session's */
    }
    in = script; in_n = n; in_pos = 0; in_end = end; reads = 0; direct = dir; active = 1;
    __asm__ volatile("msr cpsr_c, %0" :: "r"(irq));
}

/* writes the output to file and goes back to the small buffer */
static void finish(const char *file)
{
    static const char full[] = "\n[output full, the rest was lost]\n";
    uint32_t irq, tmp;
    char *b = out;
    int n = out_n, i;
    active = 0;
    if (out_lost) for (i = 0; full[i]; i++) b[n++] = full[i];
    vol_delete(file);
    file_write(file, b, n);
    __asm__ volatile("mrs %0, cpsr\n orr %1, %0, #0x80\n msr cpsr_c, %1" : "=r"(irq), "=r"(tmp));
    out = idle; out_cap = sizeof idle - SPARE; out_n = out_lost = 0;
    __asm__ volatile("msr cpsr_c, %0" :: "r"(irq));
    if (b != idle) os_free(b);
}

/* The debugger task: all other tasks are blocked until it exits ('x'). Task context only. */
static int run_to(const char *script, int n, const char *file)
{
    int r;
    begin(script, n, "x\n", 0);
    put("== RTXCbug session\n", 19);
    KS_SIGNAL(RTXCBUG_SEM);              /* priority 0: it has run and waits again when this returns */
    r = reads;
    finish(file);
    return r;
}
int rtxcbug_run(const char *script, int n) { return run_to(script, n, OUT_FILE); }

/* The object menu called in this task with interrupts off, nothing blocked: 'u' leaves it. */
int rtxcbug_cmds(const char *script, int n)
{
    uint32_t irq, tmp;
    int r;
    begin(script, n, "u\n", 1);
    put("== RTXCbug object menu, direct\n", 31);
    __asm__ volatile("mrs %0, cpsr\n orr %1, %0, #0xc0\n msr cpsr_c, %1" : "=r"(irq), "=r"(tmp));
    RTXCBUG_OBJ();
    __asm__ volatile("msr cpsr_c, %0" :: "r"(irq));
    r = reads;
    finish(OUT_FILE);
    return r;
}

/* FLAC\rtxcbug.txt (RTXCbug input lines) runs once: it is deleted, the output is FLAC\semihost.txt.
   Without one, Apple's own prints so far (if any) go to FLAC\semihost-os.txt; transcripts stay. */
void rtxcbug_script(void)
{
    static char s[2048];
    int n = file_read(SCRIPT, s, sizeof s, 0), r;
    if (n < 0) { if (out_n) finish(OS_FILE); return; }
    vol_delete(SCRIPT);
    r = rtxcbug_run(s, n);
    log_s("   rtxcbug: "); log_d(n); log_s(" byte script, "); log_d(r); log_s(" input lines read -> " OUT_FILE "\n");
}
