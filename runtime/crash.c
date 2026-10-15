/* CPU exceptions: Apple's vector stubs call CPU_exception_handler 0x0803930c(regs, kind), which
   resets at once. e_cpu_exc saves a record first; the next boot logs it. */
#include "e.h"

#define CRASH_MAGIC 0x48535243                       /* 'CRSH' */
#define APPLE_RESET 0x0835d028                       /* the handler's reset, without its svc print */

struct crash {
    uint32_t magic, kind;
    uint32_t r[13], lr_exc, spsr, sp, lr, dfsr, ifsr, far, ms;
    uint32_t stack[256];                             /* from the interrupted mode's sp */
    char tail[512];                                  /* end of the in-memory log */
    uint32_t sum;
};

/* just below the heap (mkpatch18 keeps 0x1000 free there): not loaded, zeroed or
   used by the bootloader or the OS, so it survives the reset */
#define CRASH_ROOM  0x800
#define REC ((struct crash *)(*(volatile uint32_t *)0x0804b44c - CRASH_ROOM))   /* heap base literal */

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

static void crash_save(uint32_t *regs, int kind)
{
    struct crash *c = REC;
    uint32_t spsr, m, cur, sp, lr, a;
    int i;
    __asm__ volatile("mrs %0, spsr" : "=r"(spsr));
    m = spsr & 0x1f;
    if (m == 0x10) m = 0x1f;                         /* user: its sp/lr through system mode */
    sp = lr = 0;
    if (m == 0x11 || m == 0x12 || m == 0x13 || m == 0x17 || m == 0x1b || m == 0x1f)
        __asm__ volatile("mrs %0, cpsr\n\tmsr cpsr_c, %3\n\tmov %1, sp\n\tmov %2, lr\n\tmsr cpsr_c, %0"
                         : "=&r"(cur), "=&r"(sp), "=&r"(lr) : "r"(m | 0xc0) : "lr", "memory");
    c->kind = kind;
    for (i = 0; i < 13; i++) c->r[i] = regs[i];
    c->lr_exc = regs[13];
    c->spsr = spsr;
    c->sp = sp;
    c->lr = lr;
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(c->dfsr));
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(c->ifsr));
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(c->far));
    c->ms = TIMER_E / 1000;
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
    if (!busy++) crash_save(regs, kind);             /* a fault in here goes straight to the reset */
    ((void (*)(void))APPLE_RESET)();                 /* no svc: it would fault again on a broken SVC stack */
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
    extern char __e_start[], __e_end[];
    uint32_t prev;
    if (w & 3) return 0;
    if (!((w > 0x08000004 && w < 0x08a0fc88) || (w > (uint32_t)__e_start + 4 && w < (uint32_t)__e_end))) return 0;
    prev = *(uint32_t *)(w - 4);
    return (prev & 0x0f000000) == 0x0b000000 || (prev & 0x0ffffff0) == 0x012fff30 ||
           *(uint32_t *)(w - 8) == 0xe1a0e00f;      /* bl, blx reg, mov lr, pc */
}

/* at boot: the previous run's exception, if any, into the log; then forgotten */
void crash_report(void)
{
    static const char *const kinds[8] = { "?", "undefined instruction", "?", "prefetch abort",
                                          "data abort", "reserved vector", "?", "FIQ" };
    struct crash *c = REC;
    uint32_t pc;
    int i, n;
    if (c->magic != CRASH_MAGIC) return;
    if (c->sum != rec_sum(c)) { log_s("   crash record: bad checksum, ignored\n"); c->magic = 0; return; }
    pc = c->lr_exc - (c->kind == 4 ? 8 : c->kind == 1 && (c->spsr & 0x20) ? 2 : 4);   /* Thumb undef: lr - 2 */
    log_s("   CRASH in the previous run: "); log_s(kinds[c->kind & 7]);
    log_s(" at pc "); log_x(pc, 8); log_s(", "); log_d((int)c->ms); log_s(" ms after OS start  <-- CHECK\n");
    log_s("     cpsr "); log_x(c->spsr, 8); log_s(" sp "); log_x(c->sp, 8); log_s(" lr "); log_x(c->lr, 8);
    log_s(" dfsr "); log_x(c->dfsr, 8); log_s(" ifsr "); log_x(c->ifsr, 8); log_s(" far "); log_x(c->far, 8); log_c('\n');
    words("     r0-r6 ", c->r, 7);
    words("     r7-r12", c->r + 7, 6);
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
    log_c('\n');
    c->magic = 0;
}
