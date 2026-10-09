/* CPU speed for heavy work, through the OS's own governor (the "PowerMgmt" thread).
 * os_hold_fast(ms) asks for full speed for about ms, then the level falls back by itself.
 * Do not write CLKCON1 or the core voltage here: the OS would fall out of step. */
#include "e.h"

#define os_hold_fast  ((void (*)(uint32_t))0x0802d060)    /* ms, thread context only */
#define os_lock17     ((void (*)(void))0x0805f5d8)         /* PMU lock order: 17, then 5 */
#define os_lock5      ((void (*)(void))0x0805f5c8)
#define os_unlock5    ((void (*)(void))0x080596d0)
#define os_unlock17   ((void (*)(void))0x0805972c)
#define os_pmu_read   ((int (*)(int, int, uint8_t *))0x083624e0)   /* 0 = ok */
#define PMU_DOWN1OUT  0x1e                                 /* core rail: 625 mV + 25 mV steps */
/* levels: L0 216/108/54 MHz at 1200 mV, L1 108/54/27 MHz at 1100 mV, L2 36 MHz, L3 18 MHz.
   L0 is forced while the backlight timer is armed or a hold is pending. */
#define GOV_LEVEL     (*(volatile uint32_t *)0x2200ad14)
#define GOV_STATE     ((volatile uint32_t *)0x2200aebc)    /* flags, busy, idle, hold, hint, load level */
#define GOV_RESIDENCY ((volatile uint32_t *)0x22010308)    /* time spent in L0..L3 */

static uint32_t n_holds;

/* full speed for about ms (10 s at most per call; re-arm during long jobs) */
void boost_hold(uint32_t ms)
{
    if (ms > 10000) ms = 10000;
    os_hold_fast(ms);
    n_holds++;
}

/* for the art.c decodes: a cover decode takes well under 5 s at 216 MHz */
int boost_begin(void) { boost_hold(5000); return 1; }
void boost_end(int boosted) { (void)boosted; }

uint32_t power_residency(int level) { return level >= 0 && level < 4 ? GOV_RESIDENCY[level] : 0; }

/* one PMU register, -1 if unreadable; takes the same locks as power_log */
int power_pmu_reg(int reg)
{
    uint8_t v = 0;
    int rc;
    os_lock17();
    os_lock5();
    rc = os_pmu_read(reg, 1, &v);
    os_unlock5();
    os_unlock17();
    return rc ? -1 : v;
}

/* ---- hibernate (Apple's instant-on): the OS writes 'hibe' and a resume entry at DRAM 0x08000000,
   and sets PMU GPIO3CFG (0x16) = 0 with OOCSHDWN (0x0c) = 2. The NOR boot jumps to [0x08000010]
   with DRAM intact. Sleep timeout: [0x089caf88] minutes (default 30). ---- */
#define HIB_HDR     ((volatile uint32_t *)0x08000000)
#define SLEEP_MIN   ((volatile uint32_t *)0x089caf88)
#define SLEEP_MIN2  ((volatile uint32_t *)0x089caf8c)
#define os_rtc_time ((uint32_t (*)(void))0x0808d9e8)
void power_state_log(const char *when)
{
    log_s("   power: "); log_s(when); log_s(": PMU 0c "); log_x((uint32_t)power_pmu_reg(0x0c), 2); log_s(" 16 "); log_x((uint32_t)power_pmu_reg(0x16), 2);
    log_s(", header "); log_x(HIB_HDR[0], 8); log_c(' '); log_x(HIB_HDR[1], 8); log_c(' '); log_x(HIB_HDR[2], 8); log_c(' '); log_x(HIB_HDR[4], 8);
    log_s(", sleep minutes "); log_d((int)*SLEEP_MIN); log_c('/'); log_d((int)*SLEEP_MIN2);
    log_s(", timer "); log_d((int)(TIMER_E / 1000)); log_s(" ms\n");
}
#define os_pmu_write ((int (*)(int, int, const uint8_t *))0x0836264c)   /* (reg, n, data), caller locks */
int power_pmu_set(int reg, int val)
{
    uint8_t v = (uint8_t)val;
    int rc;
    os_lock17();
    os_lock5();
    rc = os_pmu_write(reg, 1, &v);
    os_unlock5();
    os_unlock17();
    return rc;
}
/* After a resume (header 'used') the OS skips two steps: full speed (the NOR hand-over leaves the
 * clock at 13.5 MHz) and clearing the PMU hibernate flag (GPIO3CFG back to 7, as a cold boot does).
 * Called from the LCD init hook and the minute tick; acts once per resume. */
static uint32_t resume_seen;
void power_resume_check(void)
{
    if (HIB_HDR[0] != 0x75736564u || resume_seen == HIB_HDR[2] + 1) return;
    resume_seen = HIB_HDR[2] + 1;
    boost_hold(10000);
    power_pmu_set(0x16, 7);
    power_state_log("RESUMED from hibernate: full speed held, hibernate flag cleared");
}

void power_log(void)
{
    uint8_t volt = 0;
    int rc, i;
    os_lock17();
    os_lock5();
    rc = os_pmu_read(PMU_DOWN1OUT, 1, &volt);
    os_unlock5();
    os_unlock17();
    log_s("   governor: level "); log_d((int)GOV_LEVEL);
    log_s(", flags "); log_x(GOV_STATE[0], 8);
    log_s(", hold "); log_d((int)GOV_STATE[3]);
    log_s(", load level "); log_d((int)GOV_STATE[5]);
    log_s(", time in L0-L3 ");
    for (i = 0; i < 4; i++) { if (i) log_c('/'); log_d((int)GOV_RESIDENCY[i]); }
    log_s(", core rail ");
    if (rc) log_s("unreadable"); else { log_d(625 + 25 * volt); log_s(" mV"); }
    log_s(", holds "); log_d((int)n_holds);
    log_c('\n');
}
