/* Panel frame marker (FMARK, GPIO 55): video presents a frame per pulse. Rockbox's type 2/3 panel
 * setup leaves it off (R090h = 0x0021); set FMKM whenever the LCD driver registers for it. */
#include "e.h"

#define LCD_CON       (*(volatile uint32_t *)0x38300000)
#define LCD_WCMD      (*(volatile uint32_t *)0x38300004)
#define LCD_STATUS    (*(volatile uint32_t *)0x3830001c)
#define LCD_WDATA     (*(volatile uint32_t *)0x38300040)
#define PDAT6         (*(volatile uint32_t *)0x3cf000c4)   /* GPIO port 6 data: panel type in bits 4-5 */
#define TIMER_US      (*(volatile uint32_t *)0x3c7000b4)   /* TIMER_E, 1 MHz */
#define LCD_MODE_P18  0x80000da8u                          /* 16-bit command set, 18-bit bus */
#define R_FMARK       0x090
#define R_GRAM_WRITE  0x202
#define FMARK_ON      0x8021u                              /* FMKM, every frame, line 33 */
#define WAIT_US       2000

/* until (LCD_STATUS & mask) == want, at most WAIT_US; 0 on time-out */
static int wait_status(uint32_t mask, uint32_t want)
{
    uint32_t t0 = TIMER_US;
    while ((LCD_STATUS & mask) != want)
        if (TIMER_US - t0 > WAIT_US) return 0;
    return 1;
}

static int written, timeouts;

static void panel_fmark_on(void)
{
    uint32_t type = (PDAT6 >> 4) & 3, con, cpsr;
    int ok;
    if (type < 2) return;
    __asm__ volatile("mrs %0, cpsr\n orr r12, %0, #0xc0\n msr cpsr_c, r12" : "=r"(cpsr) : : "r12", "memory");
    ok = wait_status(2, 2);
    if (ok) {
        con = LCD_CON;
        LCD_CON = LCD_MODE_P18;
        ok = wait_status(0x10, 0);
        if (ok) { LCD_WCMD = R_FMARK; ok = wait_status(0x10, 0); }
        if (ok) { LCD_WDATA = FMARK_ON; ok = wait_status(0x10, 0); }
        if (ok) { LCD_WCMD = R_GRAM_WRITE; ok = wait_status(0x10, 0); }
        wait_status(2, 2);
        LCD_CON = con;
    }
    __asm__ volatile("msr cpsr_c, %0" : : "r"(cpsr) : "memory");
    if (ok ? written++ : timeouts++) return;     /* logged once each */
    log_s("   panel: type "); log_d((int)type);
    log_s(ok ? ", frame marker on (R090h = 0x8021)\n" : ", controller not ready, frame marker left as it was\n");
}

/* 0x081447fc(driver, sem): sem set = a registration for the frame marker */
static int __attribute__((used)) fmark_reg_c(uint32_t *r)
{
    if (r[1]) panel_fmark_on();
    return 0;
}
/* entry of 0x081447fc: fmark_reg_c gets the saved registers, then the displaced
   push {r0-r6, lr} runs and the original continues */
__attribute__((naked, section(".text.entry"), used)) void hk_fmark_reg(void)
{
    __asm__ volatile(
        "push {r0-r5, ip, lr}\n mov r0, sp\n ldr ip, 1f\n mov lr, pc\n bx ip\n"
        "pop {r0-r5, ip, lr}\n"
        ".word 0xe92d407f\n ldr pc, 2f\n"
        "1: .word fmark_reg_c\n 2: .word 0x08144800\n");
}
