/* CPU exceptions: Apple's vector stubs call CPU_exception_handler 0x0803930c(regs, kind), which
   resets at once; so does exit/abort (0x082a71c8, kind 8, regs = the caller's sp). e_cpu_exc
   saves a record first; the next boot logs it. */
#include "e.h"

#define CRASH_MAGIC 0x48535243                       /* 'CRSH' */
#define APPLE_RESET 0x0835d028                       /* the handler's reset, without its svc print */
#define KIND_EXIT   8
#define ERRS        32

struct crash {
    uint32_t magic, kind;
    uint32_t r[13], lr_exc, spsr, sp, lr, dfsr, ifsr, far, ms;
    uint32_t e_end;                                  /* the crashed build: its region E end */
    uint32_t stack[256];                             /* from the interrupted mode's sp */
    char tail[512];                                  /* end of the in-memory log */
    char note[96];                                   /* the assert's file and line, if any */
    uint32_t err[ERRS][4];                           /* Apple's error trail, oldest first: packed, a3, a4, ms */
    uint32_t sum;
};

/* just below the heap (mkpatch18 keeps 0x1000 free there): not loaded, zeroed or
   used by the bootloader or the OS, so it survives the reset */
#define CRASH_ROOM  0xc00
#define REC ((struct crash *)(*(volatile uint32_t *)0x0804b44c - CRASH_ROOM))   /* heap base literal */
extern char __e_start[], __e_end[];

static uint32_t rec_sum(const struct crash *c)
{
    const uint32_t *w = (const uint32_t *)c;
    uint32_t s = 0x5a17c0de;
    int i;
    for (i = 0; i < (int)(offsetof(struct crash, sum) / 4); i++) s = (s << 5 | s >> 27) ^ w[i];
    return s;
}

static int readable(uint32_t a, uint32_t n)
{
    if (a & 3) return 0;
    return (a >= 0x08000000 && a + n <= 0x0c000000) || (a >= 0x22000000 && a + n <= 0x22040000);
}

static char note[96];                                /* set by e_assert_print */
static uint32_t errs[ERRS][4];                       /* filled by hk_errtrail */
static unsigned errn;

static void crash_save(uint32_t *regs, int kind, uint32_t ret)
{
    struct crash *c = REC;
    uint32_t spsr, m, cur, sp, lr, a;
    int i;
    if (kind == KIND_EXIT) {                         /* a call, not an exception: no register frame */
        __asm__ volatile("mrs %0, cpsr" : "=r"(spsr));
        sp = (uint32_t)regs;
        lr = ret;
        for (i = 0; i < 13; i++) c->r[i] = 0;
        c->lr_exc = ret;
    } else {
        __asm__ volatile("mrs %0, spsr" : "=r"(spsr));
        m = spsr & 0x1f;
        if (m == 0x10) m = 0x1f;                     /* user: its sp/lr through system mode */
        sp = lr = 0;
        if (m == 0x11 || m == 0x12 || m == 0x13 || m == 0x17 || m == 0x1b || m == 0x1f)
            __asm__ volatile("mrs %0, cpsr\n\tmsr cpsr_c, %3\n\tmov %1, sp\n\tmov %2, lr\n\tmsr cpsr_c, %0"
                             : "=&r"(cur), "=&r"(sp), "=&r"(lr) : "r"(m | 0xc0) : "lr", "memory");
        for (i = 0; i < 13; i++) c->r[i] = regs[i];
        c->lr_exc = regs[13];
    }
    c->kind = kind;
    c->spsr = spsr;
    c->sp = sp;
    c->lr = lr;
    for (i = 0; i < (int)sizeof note; i++) c->note[i] = note[i];
    c->note[sizeof c->note - 1] = 0;
    for (i = 0; i < ERRS; i++) {
        unsigned k = errn < ERRS ? (unsigned)i : errn + i;   /* oldest first */
        int j;
        for (j = 0; j < 4; j++) c->err[i][j] = errs[k % ERRS][j];
    }
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(c->dfsr));
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(c->ifsr));
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(c->far));
    c->ms = TIMER_E / 1000;
    c->e_end = (uint32_t)__e_end;
    for (i = 0; i < 256; i++) c->stack[i] = readable(sp + 4 * i, 4) ? ((uint32_t *)sp)[i] : 0;
    log_tail(c->tail, sizeof c->tail);
    c->magic = CRASH_MAGIC;
    c->sum = rec_sum(c);
    /* the reset drops dirty cache lines: clean the record to DRAM */
    for (a = (uint32_t)c & ~31u; a < (uint32_t)c + sizeof *c; a += 32)
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");   /* drain write buffer */
}

E_ENTRY void e_cpu_exc(uint32_t *regs, int kind)
{
    static int busy;
    uint32_t c, t;
    __asm__ volatile("mrs %0, cpsr\n orr %1, %0, #0xc0\n msr cpsr_c, %1" : "=r"(c), "=r"(t));   /* exit runs in a task */
    if (!busy++) crash_save(regs, kind, (uint32_t)__builtin_return_address(0));   /* a fault in here goes straight to the reset */
    ((void (*)(void))APPLE_RESET)();                 /* no svc: it would fault again on a broken SVC stack */
}

/* entries of exit 0x08026054 and abort 0x0802606c: their stdio teardown can block the task for
   good (seen in QEMU), so the record is taken first; lr is still the caller's */
__attribute__((naked, section(".text.entry"), used)) void hk_exit(void)
{
    __asm__ volatile("mov r0, sp\n mov r1, #8\n b e_cpu_exc\n");
}

/* the assert 0x0804327c prints through vfprintf 0x0802477c(stderr, fmt, ap = {line, file}) before exit(1) */
E_ENTRY int e_assert_print(void *f, const char *fmt, uint32_t *ap)
{
    static const char pre[] = "assert: line ";
    const char *file = (const char *)ap[1];
    char d[10];
    int i = 0, n = 0, k;
    uint32_t line = ap[0];
    for (k = 0; pre[k]; k++) note[i++] = pre[k];
    do d[n++] = '0' + line % 10; while ((line /= 10) && n < 10);
    while (n) note[i++] = d[--n];
    if (file && (uint32_t)file >= 0x08000000 && (uint32_t)file < 0x0c000000) {
        note[i++] = ' '; note[i++] = 'o'; note[i++] = 'f'; note[i++] = ' ';
        for (k = 0; file[k] && i < (int)sizeof note - 1; k++) note[i++] = file[k];
    }
    note[i] = 0;
    return ((int (*)(void *, const char *, uint32_t *))0x0802477c)(f, fmt, ap);
}

/* entry of 0x0803ebac(module, code, line, a3, a4): Apple's per-task error trail; mirrored here */
static void __attribute__((used)) errtrail_c(uint32_t *r)
{
    uint32_t *e = errs[errn++ % ERRS];
    e[0] = r[0] << 24 | (r[1] & 0xfff) << 12 | (r[2] & 0xfff);
    e[1] = r[3];
    e[2] = r[8];                                     /* fifth argument, above the 8 saved registers */
    e[3] = TIMER_E / 1000;
}
__attribute__((naked, section(".text.entry"), used)) void hk_errtrail(void)
{
    __asm__ volatile(
        "push {r0-r5, ip, lr}\n mov r0, sp\n ldr ip, 1f\n mov lr, pc\n bx ip\n"
        "pop {r0-r5, ip, lr}\n"
        ".word 0xe92d47f0\n ldr pc, 2f\n"
        "1: .word errtrail_c\n 2: .word 0x0803ebb0\n");
}

/* Apple's IRAM audio engine (0x22004eec, 0x22005298) calls a listener at 0x22008c98 that only the
   music player sets (0x220056d0). Video pause/stop before any song since boot called through 0
   (pc 0x22004f88). Until a song sets it, it points at an object whose methods all return 0. */
static uint32_t ret0(void) { return 0; }
static void *const nl_vt[64] = { [0 ... 63] = (void *)ret0 };   /* slots 0xbc and 0xc4 are used */
static const void *const null_listener = nl_vt;
void audio_listener_guard(void)
{
    volatile uint32_t *l = (volatile uint32_t *)0x22008c98;
#ifndef NO_LISTENER_GUARD                        /* emulator test of the unguarded crash */
    if (!*l) *l = (uint32_t)&null_listener;
#endif
}

static void words(const char *pre, const uint32_t *w, int n)
{
    int i;
    log_s(pre);
    for (i = 0; i < n; i++) { log_c(' '); log_x(w[i], 8); }
    log_c('\n');
}

/* a return address: in OS or region E code, just after a bl or blx */
static int ret_addr(uint32_t w)
{
    uint32_t prev;
    if (w & 3) return 0;
    if (!((w > 0x08000004 && w < 0x08a0fc88) || (w > (uint32_t)__e_start + 4 && w < (uint32_t)__e_end))) return 0;
    prev = *(uint32_t *)(w - 4);
    return (prev & 0x0f000000) == 0x0b000000 || (prev & 0x0ffffff0) == 0x012fff30 ||
           *(uint32_t *)(w - 8) == 0xe1a0e00f;      /* bl, blx reg, mov lr, pc */
}

#define CRASH_FILE "FLAC\\crash.txt"
#define CRASH_CAP  65536

/* rep appended to FLAC\crash.txt, oldest reports dropped to keep it under CRASH_CAP */
static void crash_file(const char *rep, int n)
{
    char *b = os_malloc(CRASH_CAP + n);
    int got, from = 0, i, k;
    if (!b) return;
    got = file_read(CRASH_FILE, b, CRASH_CAP, 0);
    if (got < 0) got = 0;
    while (got - from + n > CRASH_CAP) {          /* drop the oldest report */
        for (i = from + 1; i + 3 < got && !(b[i - 1] == '\n' && b[i] == '=' && b[i + 1] == '=' && b[i + 2] == ' '); i++);
        from = i + 3 < got ? i : got;
    }
    for (k = 0; from + k < got; k++) b[k] = b[from + k];
    for (i = 0; i < n; i++) b[k + i] = rep[i];
    vol_delete(CRASH_FILE);
    file_write(CRASH_FILE, b, (uint32_t)(k + n));
    os_free(b);
}

int crash_prev;                          /* the previous run of this build crashed: boot the safe way */
/* at boot: the previous run's exception, if any, appended to FLAC\crash.txt with a line in the
   log; then forgotten */
void crash_report(void)
{
    static const char *const kinds[16] = { "?", "undefined instruction", "?", "prefetch abort",
                                           "data abort", "reserved vector", "?", "FIQ",
                                           "exit/abort (assert, C++ terminate)", "?", "?", "?", "?", "?", "?", "?" };
    char *rep;
    struct crash *c = REC;
    uint32_t pc;
    int i, n, mark;
    if (c->magic != CRASH_MAGIC) return;
    if (c->sum != rec_sum(c)) { log_s("   crash record: bad checksum, ignored\n"); c->magic = 0; return; }
    pc = c->lr_exc - (c->kind == 4 ? 8 : c->kind == 1 && (c->spsr & 0x20) ? 2 : 4);   /* Thumb undef: lr - 2; exit: its call */
    crash_prev = c->e_end == (uint32_t)__e_end;
    mark = log_mark();
    log_s("== crash: "); log_s(kinds[c->kind & 15]);
    log_s(" at pc "); log_x(pc, 8); log_s(", "); log_d((int)c->ms); log_s(" ms after OS start; build with region E ");
    log_x((uint32_t)__e_start, 8); log_c('-'); log_x(c->e_end, 8);
    log_s(c->e_end == (uint32_t)__e_end ? " (this build)\n" : " (not this build)\n");
    log_s("     cpsr "); log_x(c->spsr, 8); log_s(" sp "); log_x(c->sp, 8); log_s(" lr "); log_x(c->lr, 8);
    log_s(" dfsr "); log_x(c->dfsr, 8); log_s(" ifsr "); log_x(c->ifsr, 8); log_s(" far "); log_x(c->far, 8); log_c('\n');
    if (c->kind != KIND_EXIT) {
        words("     r0-r6 ", c->r, 7);
        words("     r7-r12", c->r + 7, 6);
    }
    if (c->note[0]) { log_s("     "); log_name(c->note); log_c('\n'); }
    log_s("     Apple error trail (oldest first): module code line, args, ms\n");
    for (i = n = 0; i < ERRS; i++) {
        const uint32_t *e = c->err[i];
        if (!e[0] && !e[3]) continue;
        log_s("      "); log_d((int)(e[0] >> 24)); log_c(' '); log_x(e[0] >> 12 & 0xfff, 3); log_c(' '); log_d((int)(e[0] & 0xfff));
        log_s(", "); log_x(e[1], 8); log_c(' '); log_x(e[2], 8); log_s(", "); log_d((int)e[3]); log_c('\n');
        n++;
    }
    if (!n) log_s("      (none)\n");
    log_s("     calls (return addresses on the stack, innermost first):\n     ");
    for (i = n = 0; i < 256 && n < 24; i++) {
        if (!ret_addr(c->stack[i])) continue;
        if (n && !(n % 8)) log_s("\n     ");
        log_c(' '); log_x(c->stack[i], 8); n++;
    }
    log_c('\n');
    for (i = 0; i < 256; i += 8) {
        log_s(i ? "      +" : "     stack +"); log_x(4 * i, 3);
        words("", c->stack + i, 8);
    }
    log_s("     log before it:\n     | ");
    for (i = 0; i < (int)sizeof c->tail && c->tail[i]; i++) {
        unsigned char ch = c->tail[i];
        if (ch == '\n') { if (i + 1 < (int)sizeof c->tail && c->tail[i + 1]) log_s("\n     | "); }
        else if (ch >= 32 && ch < 127) log_c(ch);
        else log_c('?');
    }
    log_s("\n\n");
    c->magic = 0;
    if ((rep = os_malloc(8192))) {                     /* only after a crash: from the heap, not region E */
        n = log_take(mark, rep, 8192);
        crash_file(rep, n);
        os_free(rep);
    }
    log_s("   CRASH in the previous run: "); log_s(kinds[c->kind & 15]); log_s(" at pc "); log_x(pc, 8);
    log_s(", "); log_d((int)c->ms); log_s(" ms after OS start -> " CRASH_FILE "  <-- CHECK\n");
}
