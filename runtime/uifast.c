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

/* text element font (0x0817ab78): a NULL font instance from 0x0826a824 (a Modern font that cannot
 * give that style) skips the element, as a 0 result does, instead of calling through address 0 */
uint32_t font_skipped;
__attribute__((naked, section(".text.entry"), used)) void e_font_inst(void)
{
    __asm__ volatile(
        "ldr r0, [sp]\n"
        "bic r0, r0, #1\n"
        "cmp r0, #0\n"
        "ldrne pc, 1f\n"                 /* 0x0817ab80: ldr r1, [r0]; the method call */
        "ldr r1, 3f\n ldr r0, [r1]\n add r0, r0, #1\n str r0, [r1]\n"
        "mov r0, #0\n"
        "ldr pc, 2f\n"                   /* 0x0817ab8c: movs r4, r0 -> element skipped */
        "1: .word 0x0817ab80\n"
        "2: .word 0x0817ab8c\n"
        "3: .word font_skipped\n");
}

/* glyph member call (0x080e2810 -> 0x080a92f8): a font object that is not set up yet gives 0, the
 * pass at the end of the pre-build does that text */
uint32_t glyph_skipped;
__attribute__((naked, section(".text.entry"), used)) void e_glyph_call(void)
{
    __asm__ volatile(
        "push {r4, r5}\n"
        "ldr r4, [r2]\n"
        "bic r4, r4, #1\n"               /* the object */
        "cmp r4, #0x08000000\n blo 9f\n"
        "cmp r4, #0x0c000000\n bhs 9f\n"
        "ldr r5, 3f\n ldr r5, [r5, #4]\n" /* member pointer's this-adjust word */
        "tst r5, #1\n beq 8f\n"           /* not virtual: no vtable to check */
        "add r4, r4, r5, asr #1\n"
        "ldr r4, [r4]\n"                  /* vtable */
        "tst r4, #3\n bne 9f\n"
        "cmp r4, #0x08000000\n blo 9f\n"
        "cmp r4, #0x0c000000\n bhs 9f\n"
        "8: pop {r4, r5}\n"
        "ldr pc, 1f\n"
        "9: ldr r4, 2f\n ldr r5, [r4]\n add r5, r5, #1\n str r5, [r4]\n"
        "pop {r4, r5}\n"
        "mov r0, #0\n"
        "bx lr\n"
        "1: .word 0x080a92f8\n"
        "2: .word glyph_skipped\n"
        "3: .word 0x083f86ec\n");
}
/* The glyph pass renders, for each font instance in the list [0x089cc8dc], every character its
 * renderer (instance+0x98) has pending. At boot only instances 1 and 4 (creation order with the
 * English UI: the main menu's rows and preview label) are done, ~690 of ~1670 glyphs; the others
 * keep their pending characters for the background build's pass (one instance per tick). Apple's
 * full pass instead with BOOT_GLYPHS 0, or when the previous run of this build crashed. */
#ifndef BOOT_GLYPHS
#define BOOT_GLYPHS 0x12
#endif
static int pb_glyph_some(uint8_t *b, uint32_t mask)
{
    extern int crash_prev;
    void **list = *(void ***)0x089cc8dc;
    uint32_t n, i;
    if (!mask || crash_prev || !list || (n = ((uint32_t *)list)[1]) < 5) return 0;
    for (i = 0; i < n && i < 32; i++) {
        uint8_t *e = 0, *f;
        void **vt;
        if (!(mask >> i & 1)) continue;
        if (!((int (*)(void *, uint32_t, void *))(*(void ***)list)[0x3c / 4])(list, i, &e) || !e) continue;
        if (!(f = *(uint8_t **)(e + 0x98))) continue;
        vt = *(void ***)f;
        ((void (*)(void *))vt[0x2c / 4])(f);
        ((void (*)(void *, int, void *))vt[0x1c / 4])(f, 0, b + 0x120);
    }
    wr32(b + 0x120, rd32(b + 0x120) + ((uint32_t (*)(int))0x0826a4e4)(0));   /* per-font-file step, as the pass ends */
    return 1;
}
static void __attribute__((used)) pb_glyphs_now(uint8_t *b)
{
    uint32_t rc_drive_ms(void);
    uint32_t tg = TIMER_E, dg = rc_drive_ms();
    int some;
    wr32(b + 0x11c, 0);
    if (!(some = pb_glyph_some(b, BOOT_GLYPHS))) ((int (*)(void *, int, int))0x081ae8c4)(b, 0, 0);
    wr32(b + 0x11c, 0);
    log_s("   ui: boot glyph pass");
    { extern int crash_prev; if (some) log_s(" (main menu fonts)"); else if (crash_prev) log_s(" (all fonts: the previous run crashed)"); }
    log_c(' '); log_d((int)((TIMER_E - tg) / 1000)); log_s(" ms ("); log_d((int)(rc_drive_ms() - dg)); log_s(" ms of drive reads)");
    if (glyph_skipped) { log_s(", "); log_d((int)glyph_skipped); log_s(" characters skipped (font not ready)"); }
    log_c('\n');
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
    { void fatdir_release(void); fatdir_release(); }   /* boot reads done: the drive may power down now */
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

/* ---- boot reads ----
 * The device read (0x082bb36c: drive, lba, buf, count, arg5 -> 1 ok, 0 failed) and write
 * (0x082bb418) come here; from app init to the first frame their time is counted for the log. */
static inline uint32_t irq_off(void) { uint32_t c, t; __asm__ volatile("mrs %0, cpsr\n orr %1, %0, #0xc0\n msr cpsr_c, %1" : "=r"(c), "=r"(t)); return c; }
static inline void irq_on(uint32_t c) { __asm__ volatile("msr cpsr_c, %0" :: "r"(c)); }
static int rc_on;
static uint32_t rc_calls, rc_d1, rc_t_d1, rc_t_other, rc_kb_d1;
uint32_t rc_drive_ms(void) { return (rc_t_d1 + rc_t_other) / 1000; }   /* device read time so far, for the step lines */
__attribute__((naked, used)) static int devread_orig(uint32_t d, uint32_t lba, void *buf, uint32_t n, uint32_t a5)
{
    __asm__ volatile("push {r3, r4, r5, r6, r7, r8, r9, lr}\n ldr pc, 1f\n 1: .word 0x082bb370\n");
}
__attribute__((naked, used)) static int devwrite_orig(uint32_t d, uint32_t lba, const void *buf, uint32_t n, uint32_t a5)
{
    __asm__ volatile("push {r3, r4, r5, r6, r7, r8, r9, lr}\n ldr pc, 1f\n 1: .word 0x082bb41c\n");
}

/* Read-ahead from the font scan to the first frame: the glyph pass reads the CJK fallback fonts in
 * ~440 scattered 8 KB pieces, each a seek; 128 KB blocks are read once and kept instead. Only the
 * volumes' data areas are cached (never the FAT, FSInfo or root folder); a fill that overlaps a
 * write is dropped, and the first fills are checked against a direct read. */
#define RA_BYTES   (128u << 10)
#define RA_MAX     48
#define RA_CHECK   8
#define RA_FAILED  0x80000000u    /* key flag: this block could not be read, read as asked */
#ifndef RA_UNIT
#define RA_UNIT    512            /* 0x082bb36c counts 512-byte sectors on every drive: its device layer
                                     0x0828c08c shifts by 9 and splits into the drive's 4 KB blocks */
#endif
struct ra_ent { volatile uint32_t key; uint8_t *raw; };      /* key: drive << 28 | block + 1, 0 = none */
static struct ra_ent ra[RA_MAX];
static volatile int ra_n, ra_on, ra_busy;
static volatile uint32_t ra_gen;                             /* writes so far */
static uint32_t ra_area0[2], ra_hits, ra_fills, ra_kb, ra_full, ra_checked, ra_bad, ra_dropped, ra_failed;
static inline uint8_t *ra_data(const struct ra_ent *e) { return (uint8_t *)(((uint32_t)e->raw + 31) & ~31u); }
static void ra_dcache(const void *p, uint32_t n)             /* clean + invalidate around a device read */
{
    uint32_t a = (uint32_t)p & ~31u, end = (uint32_t)p + n;
    for (; a < end; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a) : "memory");
    __asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c7, c10, 4" ::: "r0", "memory");
}
static uint32_t ra_time(uint32_t d, uint32_t t0) { uint32_t t = TIMER_E - t0; if (d == 1) rc_t_d1 += t; else rc_t_other += t; return t; }
/* first sector of the volume's data area (after reserved sectors, FATs and a FAT16 root folder),
   from its boot sector; 0xffffffff: unknown, nothing is cached */
static uint32_t ra_area(uint32_t d, uint32_t a5)
{
    uint8_t *raw, *b;
    uint32_t bps, res, nf, ent, fsz, a = 0xffffffffu, t;
    if (ra_area0[d]) return ra_area0[d];
    ra_area0[d] = 0xffffffffu;
    if (!(raw = os_malloc(RA_UNIT + 32))) return a;
    b = (uint8_t *)(((uint32_t)raw + 31) & ~31u);
    ra_dcache(b, RA_UNIT);
    t = TIMER_E;
    if (devread_orig(d, 0, b, 1, a5) == 1) {
        ra_dcache(b, RA_UNIT);
        bps = b[11] | b[12] << 8; res = b[14] | b[15] << 8; nf = b[16]; ent = b[17] | b[18] << 8;
        fsz = b[22] | b[23] << 8;
        if (!fsz) fsz = b[36] | b[37] << 8 | b[38] << 16 | (uint32_t)b[39] << 24;
        if (b[510] == 0x55 && b[511] == 0xaa && bps >= 512 && bps <= 4096 && !(bps & (bps - 1)) && res && nf && nf <= 2 && fsz)
            a = (res + nf * fsz + (ent * 32 + bps - 1) / bps) * (bps / RA_UNIT);
    }
    ra_time(d, t);
    os_free(raw);
    return ra_area0[d] = a;
}
int e_memcmp(const void *a, const void *b, size_t n);        /* ossync.c */
void ra_start(void) { if (rc_on && !ra_bad) ra_on = 1; }
static volatile int ra_free_due;
static void ra_free_all(void)
{
    int i;
    for (i = 0; i < ra_n; i++) { ra[i].key = 0; if (ra[i].raw) os_free(ra[i].raw); ra[i].raw = 0; }
    ra_n = 0;
}
/* the blocks are freed now, or by the last reader still inside ra_read when it leaves */
static void ra_stop(void)
{
    uint32_t c;
    int now;
    ra_on = 0;
    c = irq_off(); now = !ra_busy; if (!now) ra_free_due = 1; irq_on(c);
    if (now) ra_free_all();
}
static void ra_drop(struct ra_ent *e, uint32_t key)          /* an unpublished block: freed, key kept as a marker */
{
    if (e->raw) os_free(e->raw);
    e->raw = 0; e->key = key;
}
/* 1: served from a block, 0: not handled (the caller reads as before) */
static int ra_read(uint32_t d, uint32_t lba, void *buf, uint32_t n, uint32_t a5)
{
    uint32_t per = RA_BYTES / RA_UNIT, blk, off, key, c, k, m, t, gen;
    struct ra_ent *e;
    int i;
    if (d > 1) return 0;
    blk = lba / per; off = lba % per;
    if (off + n > per || blk >= 0x0fffffff || blk * per < ra_area(d, a5)) return 0;
    key = d << 28 | (blk + 1);
    for (i = 0; i < ra_n; i++) {
        if (ra[i].key == key) { memcpy(buf, ra_data(&ra[i]) + off * RA_UNIT, n * RA_UNIT); ra_hits++; return 1; }
        if (ra[i].key == (key | RA_FAILED)) return 0;
    }
    c = irq_off(); i = ra_n < RA_MAX ? ra_n++ : -1; irq_on(c);
    if (i < 0) { ra_full++; return 0; }
    e = &ra[i];
    if (!(e->raw = os_malloc(RA_BYTES + 32))) { ra_drop(e, key | RA_FAILED); ra_failed++; return 0; }
    gen = ra_gen;
    ra_dcache(ra_data(e), RA_BYTES);
    t = TIMER_E;
    for (k = 0; k < per; k += m) {
        m = per - k > 128 ? 128 : per - k;
        if (devread_orig(d, blk * per + k, ra_data(e) + k * RA_UNIT, m, a5) != 1) break;
    }
    ra_time(d, t);
    if (k < per) { ra_drop(e, key | RA_FAILED); ra_failed++; return 0; }   /* past the volume's end */
    ra_dcache(ra_data(e), RA_BYTES);
    if (ra_gen != gen) { ra_drop(e, 0); ra_dropped++; return 0; }        /* a write during the fill */
    if (ra_checked < RA_CHECK) {
        t = TIMER_E; c = devread_orig(d, lba, buf, n, a5); ra_time(d, t);
        if (c != 1) { ra_drop(e, 0); return 0; }
        if (e_memcmp(buf, ra_data(e) + off * RA_UNIT, n * RA_UNIT)) { ra_drop(e, 0); ra_on = 0; ra_bad++; return 1; }
        ra_checked++;
    } else memcpy(buf, ra_data(e) + off * RA_UNIT, n * RA_UNIT);
    e->key = key;
    ra_fills++; ra_kb += RA_BYTES >> 10;
    return 1;
}
static void ra_written(uint32_t d, uint32_t lba, uint32_t n)
{
    uint32_t per = RA_BYTES / RA_UNIT, b0, b1, c;
    int i;
    c = irq_off(); ra_gen++; irq_on(c);
    if (d > 1 || !n) return;
    b0 = lba / per; b1 = (lba + n - 1) / per;
    for (i = 0; i < ra_n; i++) {
        uint32_t k = ra[i].key;
        if (k && !(k & RA_FAILED) && k >> 28 == d && (k & 0x0fffffff) - 1 >= b0 && (k & 0x0fffffff) - 1 <= b1) ra[i].key = 0;
    }
}

void rc_start(void) { rc_on = 1; }
void rc_stop(void)
{
    if (!rc_on) return;
    rc_on = 0;
    log_s("   boot reads: calls "); log_d((int)rc_calls); log_s(", drive 1 "); log_d((int)rc_d1);
    { extern uint32_t pmu_hits, pmu_miss; log_s("; PMU cache "); log_d((int)pmu_hits); log_s(" hits, "); log_d((int)pmu_miss); log_s(" reads"); }
    log_s("; device time: drive 1 "); log_d((int)(rc_t_d1 / 1000)); log_s(" ms for "); log_d((int)rc_kb_d1); log_s(" KB, other drives "); log_d((int)(rc_t_other / 1000)); log_s(" ms\n");
    log_s("   read-ahead: "); log_d((int)ra_hits); log_s(" reads served from "); log_d((int)ra_fills); log_s(" blocks ("); log_d((int)ra_kb);
    log_s(" KB), "); log_d((int)ra_full); log_s(" read directly when full, "); log_d((int)ra_failed); log_s(" failed, ");
    log_d((int)ra_dropped); log_s(" dropped for a write; "); log_d((int)ra_checked); log_s(" checked against the disk");
    if (ra_bad) log_s(", MISMATCH: read-ahead turned off  <-- CHECK");
    log_c('\n');
    ra_stop();
}
__attribute__((used, section(".text.entry"))) int e_devread(uint32_t d, uint32_t lba, void *buf, uint32_t n, uint32_t a5)
{
    int rc_rc;
    uint32_t c, t;
    rc_calls++; if (d == 1) rc_d1++;
    if (ra_on) {
        c = irq_off(); ra_busy++; irq_on(c);
        rc_rc = ra_on && ra_read(d, lba, buf, n, a5);
        { int last; c = irq_off(); last = !--ra_busy && ra_free_due; if (last) ra_free_due = 0; irq_on(c); if (last) ra_free_all(); }
        if (rc_rc) return 1;
    }
    t = TIMER_E;
    rc_rc = devread_orig(d, lba, buf, n, a5);
    ra_time(d, t);
    if (d == 1) rc_kb_d1 += n >> 1;
    return rc_rc;
}
__attribute__((used, section(".text.entry"))) int e_devwrite(uint32_t d, uint32_t lba, const void *buf, uint32_t n, uint32_t a5)
{
    int r;
    if (ra_on) ra_written(d, lba, n);
    r = devwrite_orig(d, lba, buf, n, a5);
    if (ra_on) ra_written(d, lba, n);                        /* a block filled during the write */
    return r;
}

/* ---- PMU reads, cached for a second ----
 * Building screens asks for the date and time (0x080593cc: RTC over I2C), the battery ADC
 * (0x082da8fc) and the charging state (0x082dac8c, 0x080a9fdc: PMU registers) over and over, and
 * each answer is an I2C transfer the UI thread waits for (most of the time between UI init and
 * the first frame). Apple's own time cache (0x080592b4) clears its valid flag after every read.
 * Each now keeps its last answer for 1 s; copies are made with interrupts off. */
#define PMU_TTL 1000000u
uint32_t pmu_hits, pmu_miss;

__attribute__((naked, used)) static int gettime_orig(uint8_t *out)
{ __asm__ volatile("push {r4, r5, r6, lr}\n ldr pc, 1f\n 1: .word 0x080593d0\n"); }
static uint8_t tm_val[12]; static int tm_rc; static uint32_t tm_at; static int tm_ok;
__attribute__((used, section(".text.entry"))) int e_gettime(uint8_t *out)
{
    CRUMB();
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
    CRUMB();
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
    CRUMB();
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
    CRUMB();
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
    CRUMB();
    uint32_t t = TIMER_E;
    wheelwait_orig(a);
    if (ww_n++ < 4) { log_s("   ui: waited "); log_d((int)((TIMER_E - t) / 1000)); log_s(" ms for the wheel power-up, from "); log_d((int)(t / 1000)); log_s(" ms\n"); }
}
