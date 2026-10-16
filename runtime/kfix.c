/* Kernel-level fixes, each behind its own mkpatch18 flag (docs/KERNEL-SURVEY.md). */
#include "e.h"

#define os_mdelay    ((int (*)(uint32_t))0x22001f38)      /* timer spin, ms; 9 above 0x418937 */
#define os_sleep_ms  ((void (*)(int))0x080defd8)           /* task sleep (KS_delay on the running task) */
#define RTXC_TASK    (*(volatile uint32_t *)0x2200acf4)     /* running TCB; 0 in the image until the kernel sets it */
#define IRQ_NEST     (*(volatile uint8_t *)0x22008a50)      /* IRQ entry nesting count */

/* --sleep-delays: mdelay veneer 0x0802cf18. From the audited call sites below (codec power-up/down,
   library load) a task with interrupts on sleeps instead of spinning; other callers keep the spin. */
static const uint32_t mdelay_sleepers[] = { 0x0809f410, 0x0809f4bc, 0x0806dd70 };   /* return addresses */
E_ENTRY int e_mdelay(uint32_t ms)
{
    uint32_t cpsr, ra = (uint32_t)__builtin_return_address(0), i;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    for (i = 0; i < sizeof mdelay_sleepers / 4 && mdelay_sleepers[i] != ra; i++);
    if (i == sizeof mdelay_sleepers / 4 || ms < 2 || ms > 10000 || (cpsr & 0x9f) != 0x13 || IRQ_NEST || !RTXC_TASK)
        return os_mdelay(ms);
    os_sleep_ms((int)ms + 1);          /* one tick more: a tick-based sleep can end up to 1 ms early */
    return 0;
}

/* --fast-libc: mkpatch18 copies Apple's IRAM libc routines 0x22000020-0x22000317 (position-independent)
   here and points the OS's veneers at the copy, so they run from cached DRAM. Zeros without the flag. */
uint32_t kfix_libc[0x2f8 / 4];

/* --fast-dcache: 0x08039d38 clean+invalidate, 0x08039d70 clean, 0x0804e918 invalidate (addr, len). Apple calls
   a two-instruction function per 32-byte line; these do the same lines inline, and the whole cache from 32 KB. */
__attribute__((naked, section(".text.entry"), used)) void hk_dc_cinv(void)
{
    __asm__ volatile(
        "tst r0, #0x80000000\n bxne lr\n"                  /* the uncached alias, skipped as Apple does */
        "cmp r1, #0\n bxeq lr\n cmp r1, #0x8000\n bhs 2f\n add r1, r0, r1\n"
        "1: bic r2, r0, #31\n mcr p15, 0, r2, c7, c14, 1\n add r0, r0, #32\n cmp r0, r1\n blo 1b\n b 3f\n"
        "2: mrc p15, 0, r15, c7, c14, 3\n bne 2b\n"        /* test, clean and invalidate the whole D-cache */
        "3: mov r0, #0\n mcr p15, 0, r0, c7, c10, 4\n bx lr\n");
}
__attribute__((naked, section(".text.entry"), used)) void hk_dc_clean(void)
{
    __asm__ volatile(
        "cmp r1, #0\n bxeq lr\n cmp r1, #0x8000\n bhs 2f\n add r1, r0, r1\n"
        "1: bic r2, r0, #31\n mcr p15, 0, r2, c7, c10, 1\n add r0, r0, #32\n cmp r0, r1\n blo 1b\n b 3f\n"
        "2: mrc p15, 0, r15, c7, c10, 3\n bne 2b\n"        /* test and clean the whole D-cache */
        "3: mov r0, #0\n mcr p15, 0, r0, c7, c10, 4\n bx lr\n");
}
__attribute__((naked, section(".text.entry"), used)) void hk_dc_inv(void)
{
    __asm__ volatile(
        "cmp r1, #0\n bxeq lr\n add r1, r0, r1\n"
        "1: bic r2, r0, #31\n mcr p15, 0, r2, c7, c6, 1\n add r0, r0, #32\n cmp r0, r1\n blo 1b\n bx lr\n");
}

/* --iram-cache: replaces 0x0802cdc8 (control bit 23) in the MMU setup 0x22004534, MMU still off. IRAM's
   code pages (only code and constants after the startup copy) become write-through cacheable. */
static uint32_t iram_l2_room[512];    /* e.ld's SUBALIGN(4) drops alignment: the 1 KB table is aligned inside */
E_ENTRY void e_mmu_iram(void)
{
    volatile uint32_t *ttb = (volatile uint32_t *)0x2200c000;
    uint32_t *l2 = (uint32_t *)(((uint32_t)iram_l2_room + 1023) & ~1023u), i, c;
    if (ttb[0] == 0x22000c12 && ttb[0x220] == 0x22000c12) {
        for (i = 0; i < 256; i++) l2[i] = (0x22000000 + (i << 12)) | 0xff2 | (i < 8 ? 0x8 : 0);
        ttb[0] = ttb[0x220] = (uint32_t)l2 | 0x11;
    }
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0\n orr %0, %0, #0x800000\n mcr p15, 0, %0, c1, c0, 0" : "=r"(c));
}
