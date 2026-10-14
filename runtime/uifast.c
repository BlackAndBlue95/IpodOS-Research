/* UI start-up fixes in Apple's code. */
#include "e.h"
#include "os.h"

/* --ui-lazy builds: the first PB_K screens are built before the first frame, the rest by the timer. */
#ifndef PB_K
#define PB_K 10
#endif

/* 0x08119c1c registers an object in a list kept sorted by a per-task value (field +16 of each
 * element, compared with r7 = 0x08089574()). A new entry goes before the first element with a
 * greater value, found by a linear scan from the start through the iterator (0x0814ae94, a
 * virtual GetAt per step). New entries are almost always the greatest, so each insert walks the
 * whole list: ~3500 inserts over ~450 entries while the first screen is built.
 * The new-entry path at 0x08119cf8 now branches here: when the last element is not greater than
 * r7 the scan would reach the end, so it jumps to the insert with r8 = 0x7fffffff (append), as
 * the scan would; otherwise the original scan runs. sp+24 is the scan's element slot, unused yet. */
__attribute__((naked, section(".text.entry"), used)) void e_lru_insert(void)
{
    __asm__ volatile(
        "ldr r0, [r5, #4]\n"          /* the list */
        "ldr r1, [r0, #4]\n"          /* count */
        "subs r1, r1, #1\n"
        "blt 1f\n"
        "add r2, sp, #24\n"
        "ldr r3, [r0]\n"
        "ldr r3, [r3, #0x3c]\n"       /* GetAt(list, count - 1, &element) */
        "blx r3\n"
        "cmp r0, #0\n"
        "beq 1f\n"
        "ldr r0, [sp, #24]\n"
        "ldr r0, [r0, #16]\n"
        "cmp r0, r7\n"
        "bhi 1f\n"
        "ldr pc, 2f\n"                /* append: the insert at 0x08119d48, r8 = 0x7fffffff */
        "1: ldr r1, [r5, #4]\n"       /* displaced instruction, then the original scan */
        "ldr pc, 3f\n"
        "2: .word 0x08119d48\n"
        "3: .word 0x08119cfc\n");
}

/* A text provider (0x08106b44, the case at 0x081074c0) formats the number of games for a label:
 * it calls the catalog getter 0x080f5384, which loads the whole catalog (every
 * games_RO/<id>/Manifest.plist) on first use, while the first screen is being built. The call at
 * 0x081074c4 now comes here: a loaded catalog gives its count ([mgr+0x2c]); otherwise the three
 * built-in games are reported and the catalog still loads when Games is opened. */
__attribute__((naked, section(".text.entry"), used)) void e_games_count(void)
{
    __asm__ volatile(
        "ldrb r1, [r0, #0x90]\n"      /* loaded flag */
        "cmp r1, #0\n"
        "ldrne r0, [r0, #0x2c]\n"
        "moveq r0, #3\n"
        "bx lr\n");
}

/* Screen pre-build (0x081ae9a0, state machine; r4 = builder, r5 = incremental flag). In its
 * incremental mode (the timer-driven path, app+0xec, used after disk mode and with --ui-lazy at
 * boot) state 1 still builds every remaining screen in one call: after each screen it branches
 * back (0x081aeaf8 -> 0x081aea58). Incremental calls now return after one screen through the
 * common exit 0x081aeb24 (state stays 1, done flag 0, the tick re-arms the timer); the blocking
 * call keeps looping as before. */
static inline uint32_t rd32(const uint8_t *p) { return *(volatile const uint32_t *)p; }
static inline void wr32(uint8_t *p, uint32_t v) { *(volatile uint32_t *)p = v; }
/* PB_K > 0: the boot builds the first PB_K screens of state 1 before it returns (blocking, as
   state 0), runs the glyph pass for them, and leaves the rest to the timer. */
uint32_t pb_k __attribute__((used)) = PB_K;
extern uint32_t pb_lazy;
static void __attribute__((used)) pb_glyphs_now(uint8_t *b)
{
    uint32_t tg = TIMER_E;
    wr32(b + 0x11c, 0);
    ((int (*)(void *, int, int))0x081ae8c4)(b, 0, 0);
    wr32(b + 0x11c, 0);
    log_s("   ui: boot glyph pass "); log_d((int)((TIMER_E - tg) / 1000)); log_s(" ms\n");
    log_s("   ui: first "); log_d((int)rd32(b + 0x114)); log_s(" screens and their glyphs built at "); log_d((int)(TIMER_E / 1000)); log_s(" ms, the rest in the background\n");
}
__attribute__((naked, section(".text.entry"), used)) void e_prebuild_yield(void)
{
    __asm__ volatile(
        "cmp r5, #0\n"
        "ldrne pc, 2f\n"                 /* incremental: one screen per tick */
        "ldr ip, 4f\n ldr ip, [ip]\n"     /* blocking: keep going, unless the boot cut-off is reached */
        "cmp ip, #0\n"
        "ldreq pc, 1f\n"
        "ldr r0, 6f\n ldr r0, [r0]\n cmp r0, #0\n ldreq pc, 1f\n"
        "ldr r0, [r4, #0x114]\n"
        "cmp r0, ip\n"
        "ldrlo pc, 1f\n"
        "mov r0, r4\n ldr ip, 5f\n mov lr, pc\n bx ip\n"   /* glyph pass for what is built so far */
        "mov r5, #1\n"
        "ldr pc, 2f\n"
        "1: .word 0x081aea58\n"
        "2: .word 0x081aeb24\n"
        "4: .word pb_k\n"
        "5: .word pb_glyphs_now\n"
        "6: .word pb_lazy\n");
}

/* Starting from state 0 the pre-build forces itself blocking (`moveq r5, #0` at 0x081ae9d0), so
 * the boot always built every screen before the first frame. With --ui-lazy that instruction
 * calls e_pb_entry, which remembers the caller's flag and still forces state 0 blocking (the first
 * screen and the full font-table pass, which text needs); the end of state 0 (0x081aea54,
 * `b 0x081aeb24`) goes to e_pb_state0_done, which restores the flag so the call returns there.
 * The remaining screens and the glyph pass then run one step per timer tick. */
uint32_t pb_lazy __attribute__((used));
extern uint32_t pb_k;
__attribute__((naked, section(".text.entry"), used)) void e_pb_entry(void)
{
    __asm__ volatile(
        "ldr ip, 1f\n"
        "str r5, [ip]\n"
        "cmp r0, #0\n"                /* state, flags as at 0x081ae9cc */
        "moveq r5, #0\n"
        "bx lr\n"
        "1: .word pb_lazy\n");
}
/* In background mode the glyph pass (0x081ae8c4 mode 0, normally state 2 after every screen) runs
 * here once, blocking, for the characters the first screen asked for, so its text is there on the
 * first frame; state 2 runs it again at the end for the screens built later. The pass's index
 * (builder+0x11c) is reset before it, and state 1 resets it again on entry. */
__attribute__((naked, section(".text.entry"), used)) void e_pb_state0_done(void)
{
    __asm__ volatile(
        "ldr ip, 1f\n"
        "ldr ip, [ip]\n"
        "cmp ip, #0\n"
        "ldreq pc, 2f\n"
        "ldr ip, 4f\n ldr ip, [ip]\n cmp ip, #0\n ldrne pc, 2f\n"   /* a boot cut-off: state 1 continues blocking */
        "mov r0, #0\n"
        "str r0, [r4, #0x11c]\n"
        "mov r0, r4\n"
        "mov r1, #0\n"
        "mov r2, #0\n"
        "ldr ip, 3f\n"
        "mov lr, pc\n"
        "bx ip\n"
        "mov r5, #1\n"
        "ldr pc, 2f\n"
        "1: .word pb_lazy\n"
        "2: .word 0x081aeb24\n"
        "3: .word 0x081ae8c4\n"
        "4: .word pb_k\n");
}

/* End of the pre-build (state 2 -> 3, 0x081aeb1c `mov r0, #3`). In background mode the home
 * screen's views were laid out before the glyphs were ready, so once, at the first completion,
 * the UI is reloaded as after a theme switch (0x0817cd30(app, 1)), which rebuilds the home screen. */
static int pb_refreshed;
static void __attribute__((used)) pb_done_c(void)
{
    log_s("   ui: screen pre-build done at "); log_d((int)(TIMER_E / 1000)); log_s(" ms");
    if (pb_lazy && !pb_refreshed && !pb_k) {
        pb_refreshed = 1;
        log_s(", home reloaded\n");
        ((void (*)(void *, int))0x0817cd30)(((void *(*)(int))0x0817d524)(0), 1);
    } else log_c('\n');
    log_flush();
}
__attribute__((naked, section(".text.entry"), used)) void e_pb_done(void)
{
    __asm__ volatile(
        "ldr ip, 1f\n"
        "mov lr, pc\n"
        "bx ip\n"
        "mov r0, #3\n"
        "ldr pc, 2f\n"
        "1: .word pb_done_c\n"
        "2: .word 0x081aeb20\n");
}

/* ---- boot read cache for the resource volume ----
 * The file layer keeps one cached block per open file (0x0826cdb8), and glyph rendering jumps
 * between a font's tables, so the same 8-64 KB blocks of the resource volume (drive 1, the
 * firmware partition's fonts and resources, never written in normal use) are read again and
 * again while the first screen is built. The device read (0x082bb36c: drive, lba, buf, count,
 * arg5 -> 1 ok, 0 failed) now comes here: drive 1 reads are served from a copy when the same (lba, count)
 * was read before. Active from app init until the first frame, then freed; a drive 1 write
 * (0x082bb418) empties it. */
#define RC_MAX     1024
#define RC_BYTES   (6u << 20)
struct rc_ent { uint32_t lba, count; uint8_t *data; };
static struct rc_ent rc[RC_MAX];
static int rc_n, rc_next, rc_on;
static uint32_t rc_bytes, rc_hits, rc_miss, rc_saved, rc_calls, rc_d1, rc_last_d, rc_t_d1, rc_t_other, rc_kb_d1;
uint32_t rc_us;
__attribute__((naked, used)) static int devread_orig(uint32_t d, uint32_t lba, void *buf, uint32_t n, uint32_t a5)
{
    __asm__ volatile("push {r3, r4, r5, r6, r7, r8, r9, lr}\n ldr pc, 1f\n 1: .word 0x082bb370\n");
}
__attribute__((naked, used)) static int devwrite_orig(uint32_t d, uint32_t lba, const void *buf, uint32_t n, uint32_t a5)
{
    __asm__ volatile("push {r3, r4, r5, r6, r7, r8, r9, lr}\n ldr pc, 1f\n 1: .word 0x082bb41c\n");
}
static void rc_clear(void)
{
    int i;
    for (i = 0; i < rc_n; i++) if (rc[i].data) os_free(rc[i].data);
    memset(rc, 0, sizeof rc); rc_n = rc_next = 0; rc_bytes = 0;
}
void rc_start(void) { rc_clear(); rc_on = 1; rc_hits = rc_miss = rc_saved = 0; }
void rc_stop(void)
{
    if (!rc_on) return;
    rc_on = 0;
    log_s("   read cache: "); log_d((int)rc_hits); log_s(" hits ("); log_d((int)(rc_saved >> 10)); log_s(" KB not re-read), ");
    log_d((int)rc_miss); log_s(" misses, "); log_d(rc_n); log_s(" blocks kept, "); log_d((int)(rc_bytes >> 10)); log_s(" KB; calls "); log_d((int)rc_calls); log_s(", drive 1 "); log_d((int)rc_d1);
    { extern uint32_t pmu_hits, pmu_miss; log_s("; PMU cache "); log_d((int)pmu_hits); log_s(" hits, "); log_d((int)pmu_miss); log_s(" reads"); }
    log_s("; device time: drive 1 "); log_d((int)(rc_t_d1 / 1000)); log_s(" ms for "); log_d((int)rc_kb_d1); log_s(" KB, other drives "); log_d((int)(rc_t_other / 1000)); log_s(" ms\n");
    rc_clear();
}
__attribute__((used, section(".text.entry"))) int e_devread(uint32_t d, uint32_t lba, void *buf, uint32_t n, uint32_t a5)
{
    int i, rc_rc;
    uint32_t sz = n * 512;
    rc_calls++; rc_last_d = d; if (d == 1) rc_d1++;
    if (rc_on && d == 1) {
        for (i = 0; i < rc_n; i++)
            if (rc[i].lba == lba && rc[i].count == n && rc[i].data) {
                memcpy(buf, rc[i].data, sz); rc_hits++; rc_saved += sz; return 1;
            }
    }
    { uint32_t t = TIMER_E;
      rc_rc = devread_orig(d, lba, buf, n, a5);
      t = TIMER_E - t; if (d == 1) { rc_t_d1 += t; rc_kb_d1 += sz >> 10; } else rc_t_other += t; }
    if (rc_on && d == 1 && rc_rc == 1 && sz && sz <= 0x10000) {
        struct rc_ent *e;
        rc_miss++;
        if (rc_n < RC_MAX && rc_bytes + sz <= RC_BYTES) e = &rc[rc_n++];
        else { e = &rc[rc_next]; rc_next = (rc_next + 1) % (rc_n ? rc_n : 1); if (e->data) { os_free(e->data); rc_bytes -= e->count * 512; } e->data = 0; }
        if ((e->data = os_malloc(sz))) { memcpy(e->data, buf, sz); e->lba = lba; e->count = n; rc_bytes += sz; }
    }
    return rc_rc;
}
__attribute__((used, section(".text.entry"))) int e_devwrite(uint32_t d, uint32_t lba, const void *buf, uint32_t n, uint32_t a5)
{
    if (rc_on && d == 1) rc_clear();
    return devwrite_orig(d, lba, buf, n, a5);
}

/* ---- PMU reads, cached for a second ----
 * Building screens asks for the date and time (0x080593cc: RTC over I2C), the battery ADC
 * (0x082da8fc) and the charging state (0x082dac8c, 0x080a9fdc: PMU registers) over and over, and
 * each answer is an I2C transfer the UI thread waits for (most of the time between UI init and
 * the first frame). Apple's own time cache (0x080592b4) clears its valid flag after every read.
 * Each now keeps its last answer for 1 s; copies are made with interrupts off. */
#define PMU_TTL 1000000u
static inline uint32_t irq_off(void) { uint32_t c, t; __asm__ volatile("mrs %0, cpsr\n orr %1, %0, #0xc0\n msr cpsr_c, %1" : "=r"(c), "=r"(t)); return c; }
static inline void irq_on(uint32_t c) { __asm__ volatile("msr cpsr_c, %0" :: "r"(c)); }
uint32_t pmu_hits, pmu_miss;

__attribute__((naked, used)) static int gettime_orig(uint8_t *out)
{ __asm__ volatile("push {r4, r5, r6, lr}\n ldr pc, 1f\n 1: .word 0x080593d0\n"); }
static uint8_t tm_val[12]; static int tm_rc; static uint32_t tm_at; static int tm_ok;
__attribute__((used, section(".text.entry"))) int e_gettime(uint8_t *out)
{
    uint32_t c, now;
    c = irq_off(); now = TIMER_E;
    int rc;
    if (tm_ok && now - tm_at < PMU_TTL) { memcpy(out, tm_val, 12); rc = tm_rc; irq_on(c); pmu_hits++; return rc; }
    irq_on(c);
    rc = gettime_orig(out);
    c = irq_off(); memcpy(tm_val, out, 12); tm_rc = rc; tm_at = now; tm_ok = 1; irq_on(c); pmu_miss++;
    return rc;
}

__attribute__((naked, used)) static int adc_orig(int type, uint32_t *out)
{ __asm__ volatile("push {r4, r5, r6, lr}\n ldr pc, 1f\n 1: .word 0x082da900\n"); }
static uint32_t adc_val[5], adc_at[5]; static int adc_rc[5], adc_ok[5];
__attribute__((used, section(".text.entry"))) int e_adc(int type, uint32_t *out)
{
    uint32_t c, now = TIMER_E;
    int rc;
    if (type < 1 || type > 4) return adc_orig(type, out);
    c = irq_off();
    if (adc_ok[type] && now - adc_at[type] < PMU_TTL) { *out = adc_val[type]; rc = adc_rc[type]; irq_on(c); pmu_hits++; return rc; }
    irq_on(c);
    rc = adc_orig(type, out);
    c = irq_off(); adc_val[type] = *out; adc_rc[type] = rc; adc_at[type] = now; adc_ok[type] = 1; irq_on(c); pmu_miss++;
    return rc;
}

__attribute__((naked, used)) static int chg_orig(void)
{ __asm__ volatile("push {r3, r4, r5, lr}\n ldr pc, 1f\n 1: .word 0x082dac90\n"); }
static int chg_val, chg_ok; static uint32_t chg_at;
__attribute__((used, section(".text.entry"))) int e_chg(void)
{
    uint32_t now = TIMER_E;
    if (chg_ok && now - chg_at < PMU_TTL) { pmu_hits++; return chg_val; }
    chg_val = chg_orig(); chg_at = now; chg_ok = 1; pmu_miss++;
    return chg_val;
}

__attribute__((naked, used)) static int ext_orig(void)
{ __asm__ volatile("push {r3, r4, r5, lr}\n ldr pc, 1f\n 1: .word 0x080a9fe0\n"); }
static int ext_val, ext_ok; static uint32_t ext_at;
__attribute__((used, section(".text.entry"))) int e_ext(void)
{
    uint32_t now = TIMER_E;
    if (ext_ok && now - ext_at < PMU_TTL) { pmu_hits++; return ext_val; }
    ext_val = ext_orig(); ext_at = now; ext_ok = 1; pmu_miss++;
    return ext_val;
}

/* Measurement only: the UI thread's wait for the click wheel's power-up (0x0815a5b4, message 25:
   polls every 10 ms for the ready flag that the power task sets 2 s after switching LDO6). */
__attribute__((naked, used)) static void wheelwait_orig(void *a)
{ __asm__ volatile("push {r4, lr}\n ldr pc, 1f\n 1: .word 0x0815a5b8\n"); }
static int ww_n;
__attribute__((used, section(".text.entry"))) void e_wheelwait(void *a)
{
    uint32_t t = TIMER_E;
    wheelwait_orig(a);
    if (ww_n++ < 4) { log_s("   ui: waited "); log_d((int)((TIMER_E - t) / 1000)); log_s(" ms for the wheel power-up, from "); log_d((int)(t / 1000)); log_s(" ms\n"); }
}
