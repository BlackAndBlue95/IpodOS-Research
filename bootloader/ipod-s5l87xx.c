/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2005 by Dave Chapman
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "config.h"

#include "inttypes.h"
#include "cpu.h"
#include "system.h"
#include "lcd.h"
#include "../kernel-internal.h"
#include "file_internal.h"
#include "storage.h"
#include "disk.h"
#include "font.h"
#include "backlight.h"
#include "backlight-target.h"
#include "button.h"
#include "panic.h"
#include "power.h"
#include "file.h"
#include "common.h"
#include "rb-loader.h"
#include "loader_strerror.h"
#include "version.h"
#include "powermgmt.h"
#include "usb.h"
#ifdef HAVE_SERIAL
#include "serial.h"
#endif

#include "s5l87xx.h"
#include "clocking-s5l8702.h"
#include "spi-s5l8702.h"
#include "i2c-s5l8702.h"
#include "gpio-s5l8702.h"
#include "pmu-target.h"
#ifdef OSCHAIN
#include "pcf5063x.h"
#endif
#if defined(IPOD_6G) || defined(IPOD_NANO3G)
#include "norboot-target.h"
#endif


#define ERR_RB      0
#define ERR_OF      1
#define ERR_STORAGE 2
#define ERR_LBA28   3

/* Safety measure - maximum allowed firmware image size.
   The largest known current (October 2009) firmware is about 6.2MB so
   we set this to 8MB.
*/
#define MAX_LOADSIZE (8*1024*1024)

#define LCD_RBYELLOW    LCD_RGBPACK(255,192,0)
#define LCD_REDORANGE   LCD_RGBPACK(255,70,0)
#define LCD_GREEN       LCD_RGBPACK(0,255,0)

extern void bss_init(void);
extern uint32_t _movestart;
extern uint32_t start_loc;

extern int line;

#ifndef S5L87XX_DEVELOPMENT_BOOTLOADER
#ifdef HAVE_BOOTLOADER_USB_MODE
static void usb_mode(void)
{
    int button;

    verbose = true;

    printf("Entering USB mode...");

    powermgmt_init();

    /* The code will ask for the maximum possible value */
    usb_charging_enable(USB_CHARGING_ENABLE);

    usb_init();
    usb_start_monitoring();

    /* Wait until USB is plugged */
    while (usb_detect() != USB_INSERTED)
    {
        printf("Plug USB cable");
        line--;
        sleep(HZ/10);
    }

    while(1)
    {
        button = button_get_w_tmo(HZ/10);

        if (button == SYS_USB_CONNECTED)
            break; /* Hit */

        if (usb_detect() == USB_EXTRACTED)
            break; /* Cable pulled */

        /* Wait for threads to connect or cable is pulled */
        printf("USB: Connecting...");
        line--;
    }

    if (button == SYS_USB_CONNECTED)
    {
        /* Got the message - wait for disconnect */
        printf("Bootloader USB mode");

        /* Ack the SYS_USB_CONNECTED polled from the button queue */
        usb_acknowledge(SYS_USB_CONNECTED_ACK, button_get_data());

        while(1)
        {
            button = button_get_w_tmo(HZ/2);
            if (button == SYS_USB_DISCONNECTED)
                break;
        }
    }

    /* We don't want the HDD to spin up if the USB is attached again */
    usb_close();
    printf("USB mode exit     ");
}
#endif /* HAVE_BOOTLOADER_USB_MODE */

void fatal_error(int err)
{
    verbose = true;

    /* System font is 6 pixels wide */
    line++;
    switch (err)
    {
        case ERR_RB:
#ifdef HAVE_BOOTLOADER_USB_MODE
            usb_mode();
            printf("Hold MENU+SELECT to reboot");
            break;
#endif
        case ERR_STORAGE:
            printf("Hold MENU+SELECT to reboot");
            printf("then SELECT+PLAY for disk mode");
            break;
        case ERR_OF:
            printf("Hold MENU+SELECT to reboot");
            printf("and enter Rockbox firmware");
            break;
        case ERR_LBA28:
            printf("Hold MENU+SELECT to reboot");
            printf("and LEFT if you are REALLY sure");
            break;
    }

#if (CONFIG_STORAGE & STORAGE_ATA)
    if (ide_powered())
        ata_sleepnow(); /* Immediately spindown the disk. */
#endif

    line++;
    lcd_set_foreground(LCD_REDORANGE);
    while (1) {
        lcd_puts(0, line, button_hold() ? "Hold switch on!"
                                        : "               ");
        lcd_update();
    }
}

#if (CONFIG_STORAGE & STORAGE_ATA)
extern unsigned short battery_level_disksafe;
static void battery_trap(void)
{
    int vbat, old_verb;
    int th = 50;

    old_verb = verbose;
    verbose = true;

    usb_charging_maxcurrent_change(100);

    while (1)
    {
        vbat = _battery_voltage();

        /*  Two reasons to use this threshold (may require adjustments):
         *  - when USB (or wall adaptor) is plugged/unplugged, Vbat readings
         *    differ as much as more than 200 mV when charge current is at
         *    maximum (~340 mA).
         *  - RB uses some sort of average/compensation for battery voltage
         *    measurements, battery icon blinks at battery_level_disksafe,
         *    when the HDD is used heavily (large database) the level drops
         *    to battery_level_shutoff quickly.
         */
        if (vbat >= battery_level_disksafe + th)
            break;
        th = 200;

        if (power_input_status() != POWER_INPUT_NONE) {
            lcd_set_foreground(LCD_RBYELLOW);
            printf("Low battery: %d mV, charging...     ", vbat);
            sleep(HZ*3);
        }
        else {
            /* Wait for the user to insert a charger */
            int tmo = 10;
            lcd_set_foreground(LCD_REDORANGE);
            while (1) {
                vbat = _battery_voltage();
                printf("Low battery: %d mV, power off in %d ", vbat, tmo);
                if (!tmo--) {
                    /* Raise Vsysok (hyst=0.02*Vsysok) to avoid PMU
                       standby<->active looping */
                    if (vbat < 3200)
                        pmu_write(PCF5063X_REG_SVMCTL, 0xA /*3200mV*/);
                    power_off();
                }
                sleep(HZ*1);
                if (power_input_status() != POWER_INPUT_NONE)
                    break;
                line--;
            }
        }
        line--;
    }

    verbose = old_verb;
    lcd_set_foreground(LCD_WHITE);
    printf("Battery status ok: %d mV            ", vbat);
}
#endif /* CONFIG_STORAGE & STORAGE_ATA */
#endif /* S5L87XX_DEVELOPMENT_BOOTLOADER */

static int launch_onb(int clkdiv)
{
#if defined(IPOD_6G) || defined(IPOD_NANO3G)
    /* SPI clock = PClk/(clkdiv+1) */
    spi_clkdiv(SPI_PORT, clkdiv);

    /* Actually IRAM1_ORIG contains current RB bootloader IM3 header,
       it will be replaced by ONB IM3 header, so this function must
       be called once!!! */
    struct Im3Info *hinfo = (struct Im3Info*)IRAM1_ORIG;

    /* Loads ONB in IRAM0, exception vector table is destroyed !!! */
    int rc = im3_read(
            NORBOOT_OFF + im3_nor_sz(hinfo), hinfo, (void*)IRAM0_ORIG);

    if (rc != 0) {
        /* Restore exception vector table */
        memcpy((void*)IRAM0_ORIG, &_movestart, 4*(&start_loc-&_movestart));
        commit_discard_idcache();
        return rc;
    }

    /* Disable all external interrupts */
    eint_init();

    commit_discard_idcache();

    /* Branch to start of IRAM */
    asm volatile("mov pc, %0"::"r"(IRAM0_ORIG));
    while(1);
#elif defined(IPOD_NANO4G)
    (void) clkdiv;

    lcd_set_foreground(LCD_REDORANGE);
    printf("Not implemented");

    line++;
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);

    return 0;
#endif
}

/* Launch OF when kernel mode is running */
static int kernel_launch_onb(void)
{
    disable_irq();
    int rc = launch_onb(3); /* 54/4 = 13.5 MHz. */
    enable_irq();
    return rc;
}


#if defined(IPOD_6G) && defined(OSCHAIN) && !defined(S5L87XX_DEVELOPMENT_BOOTLOADER)
/*
 * Chainload our OS (Apple's decrypted RetailOS with region E) from the firmware partition or the FAT32 disk.
 *
 * Apple's NOR bootloader (an EFI build) normally decrypts osos into DRAM
 * and jumps to the start of the body in SVC mode with IRQ/FIQ masked. The
 * OS relocates itself (first 0xaed8 bytes to IRAM0, the rest to DRAM_ORIG),
 * so it can be loaded anywhere. The only thing it takes from the bootloader
 * is a mailbox at the end of IRAM1:
 *   0x2203ff10  boot flags (0 for a normal osos boot)
 *   0x2203ff18  'SysI' magic
 *   0x2203ff1c  pointer to a 0x120 byte SysInfo block, built by Apple's
 *               SystemConfig DXE module from the NOR SysCfg
 * The OS copies SysInfo before its heap is set up, and uses the DRAM base
 * and size in it for the heap.
 */
#define OSCHAIN_FILE        "/osos-v18.dec"
#define OSCHAIN_FILE2       "/osos-boot.dec"
#define OSCHAIN_HDR_SZ      0x800
#define OSCHAIN_LOADADDR    (DRAM_ORIG + 0x02000000) /* above bootloader BSS, below the SysInfo staging area */
#define OSCHAIN_MAXSIZE     0x01800000
#define OSCHAIN_SYSINFO     (DRAM_ORIG + 0x03f00000) /* staging, below TTB */
#define OSCHAIN_SYSINFO_SZ  0x120
#define OSCHAIN_SYSINFO_IRAM 0x22028cc0 /* where Apple's Bds puts it */
#define OSCHAIN_MAILBOX     0x2203ff00
#define OSCHAIN_MAX_ENTRIES 32

#define SYSINFO_MAGIC       0x53797349 /* 'SysI' */

static inline void put32(uint8_t *p, int off, uint32_t v)
{
    memcpy(p + off, &v, 4);
}

static int oschain_build_sysinfo(uint8_t *si)
{
    struct SysCfgHeader hdr;
    struct SysCfgEntry e;

    memset(si, 0, OSCHAIN_SYSINFO_SZ);
    put32(si, 0x00, SYSINFO_MAGIC);
    put32(si, 0x04, 4);              /* version */
    si[0x88] = 'N';
    si[0x89] = 'A';
    put32(si, 0xe0, 0x04000000);     /* DRAM size */
    put32(si, 0xe4, DRAM_ORIG);      /* DRAM base */
    put32(si, 0xe8, 0x00040000);     /* IRAM size */
    put32(si, 0xec, IRAM0_ORIG);     /* IRAM base */
    put32(si, 0xf0, NOR_SZ);         /* NOR size */
    put32(si, 0xf4, 0x24000000);     /* NOR base */
    put32(si, 0x118, 0x7672736e);    /* 'nsrv' */
    put32(si, 0x11c, 0x01708004);

    bootflash_init(SPI_PORT);
    bootflash_read(SPI_PORT, 0, sizeof(hdr), &hdr);
    if (hdr.magic != SYSCFG_MAGIC) {
        bootflash_close(SPI_PORT);
        printf("SysCfg not found");
        return -1;
    }

    unsigned n = MIN(hdr.num_entries, OSCHAIN_MAX_ENTRIES);
    for (unsigned i = 0; i < n; i++) {
        uint32_t w0, w1;
        uint16_t h0, h2;

        bootflash_read(SPI_PORT, sizeof(hdr) + i * sizeof(e), sizeof(e), &e);
        memcpy(&w0, e.data, 4);
        memcpy(&w1, e.data + 4, 4);
        memcpy(&h0, e.data, 2);
        memcpy(&h2, e.data + 4, 2);

        switch (e.tag) {
        case SYSCFG_TAG_SRNM: memcpy(si + 0x18, e.data, 16); break;
        case SYSCFG_TAG_FWID: memcpy(si + 0x38, e.data + 4, 8); break;
        case SYSCFG_TAG_HWVR: put32(si, 0x84, w1); break;
        case SYSCFG_TAG_MODN: memcpy(si + 0x98, e.data, 16); break;
        case SYSCFG_TAG_CODC: put32(si, 0x104, w0); break;
        case SYSCFG_TAG_SWVR: memcpy(si + 0x108, e.data, 16); break;
        case SYSCFG_TAG_REGN:
            if (h0 == 1) {
                memcpy(si + 0x92, &h2, 2);
                memcpy(si + 0x94, e.data + 6, 2);
            }
            break;
        }
    }
    bootflash_close(SPI_PORT);
    printf("SysCfg: %u entries", n);
    return 0;
}

static void __attribute__((noreturn)) oschain_jump(uint32_t entry)
{
    volatile uint32_t *mb = (volatile uint32_t *)OSCHAIN_MAILBOX;

    disable_interrupt(IRQ_FIQ_STATUS);
    VIC0INTENCLEAR = 0xffffffff;
    VIC1INTENCLEAR = 0xffffffff;
    eint_init();
    /* Clock gating and clock generators as Apple's bootloader hands them over (the OS
       only re-gates blocks it used itself). CLKCON1 and the core voltage stay as they are:
       the OS's own governor assumes the 216/108/54 it was started with. */
    PWRCON(0) = 0x2007cd45;
    PWRCON(1) = 0x0003efc9;
    CLKCON2 = 0x80008000;
    CLKCON3 = 0x80008000;
    CLKCON4 = 0x00008000;
    CLKCON5 = 0x00008000;

    /* Copy SysInfo to the same IRAM spot Apple uses. That overwrites
       bootloader code we no longer run, unless the cache flush below
       lives there, in which case the DRAM copy is passed instead. */
    uint32_t si = OSCHAIN_SYSINFO;
    uintptr_t flush = (uintptr_t)commit_discard_idcache, me = (uintptr_t)oschain_jump;
    if ((flush + 0x100 < OSCHAIN_SYSINFO_IRAM || flush >= OSCHAIN_SYSINFO_IRAM + OSCHAIN_SYSINFO_SZ) &&
        (me + 0x400 < OSCHAIN_SYSINFO_IRAM || me >= OSCHAIN_SYSINFO_IRAM + OSCHAIN_SYSINFO_SZ)) {
        volatile uint32_t *src = (volatile uint32_t *)OSCHAIN_SYSINFO;
        volatile uint32_t *dst = (volatile uint32_t *)OSCHAIN_SYSINFO_IRAM;
        for (int i = 0; i < OSCHAIN_SYSINFO_SZ / 4; i++)
            dst[i] = src[i];
        si = OSCHAIN_SYSINFO_IRAM;
    }

    mb[4] = 0;                       /* 0x2203ff10: boot flags */
    mb[6] = SYSINFO_MAGIC;           /* 0x2203ff18 */
    mb[7] = si;                      /* 0x2203ff1c */

    commit_discard_idcache();

    /* Leave the way Apple's bootloader does: SVC mode, IRQ/FIQ masked.
       Also turn off the MMU and caches Rockbox enabled. Code and data
       here are identity mapped, so this is safe. */
    asm volatile(
        "mrc    p15, 0, r0, c1, c0, 0   \n"
        "bic    r0, r0, #0x1000         \n" /* I-cache */
        "bic    r0, r0, #0x5            \n" /* MMU, D-cache */
        "mcr    p15, 0, r0, c1, c0, 0   \n"
        "mov    r0, #0                  \n"
        "mcr    p15, 0, r0, c7, c7, 0   \n" /* invalidate I and D cache */
        "mcr    p15, 0, r0, c8, c7, 0   \n" /* invalidate TLB */
        "mcr    p15, 0, r0, c7, c10, 4  \n" /* drain write buffer */
        "msr    cpsr_c, #0xd3           \n"
        "mov    r1, #0                  \n"
        "mov    r2, #0                  \n"
        "mov    r3, #0                  \n"
        "bx     %0                      \n"
        : : "r"(entry) : "r0", "r1", "r2", "r3", "memory");
    while (1);
}

/* Our OS in the firmware partition: MSE directory at +0x5000 (12 entries of 40 bytes:
   tag, type (both byte-reversed), u32, devOff, len, addr, entryOff, checksum, ...), section
   data at devOff + 0x1000, checksum = 32-bit byte sum. The MBR's LBAs are in the "virtual"
   sector unit the FAT uses (4096 on this iPod); storage sectors are the drive's logical ones. */
static uint8_t oschain_sec[4096] __attribute__((aligned(32)));
static long oschain_load_osfl(uint8_t *dst, uint32_t maxsz)
{
    /* the MBR's LBAs are in the FAT's "virtual" sector unit (4096 here) and storage reads
       are in the drive's logical sectors (4096 or 512): try the combinations and keep the
       one that shows the partition's magic */
    static const uint32_t ss_try[3] = { 4096, 512, 512 }, mult_try[3] = { 1, 8, 1 };
    uint32_t ss = 0, lba, part = 0, dev = 0, len = 0, chk = 0, sum = 0, done = 0, i;
    int found = 0;

    if (storage_read_sectors(IF_MD(0,) 0, 1, oschain_sec) < 0) return -11;
    if (oschain_sec[510] != 0x55 || oschain_sec[511] != 0xaa) return -12;
    memcpy(&lba, oschain_sec + 0x1ce + 8, 4);                 /* partition 2 */
    if (!lba) return -13;
    for (i = 0; i < 3 && !ss; i++) {
        if (storage_read_sectors(IF_MD(0,) lba * mult_try[i], 1, oschain_sec) < 0) continue;
        if (!memcmp(oschain_sec + 0x100, "]ih[", 4)) { ss = ss_try[i]; part = lba * mult_try[i]; }
    }
    if (!ss) return -15;
    if (storage_read_sectors(IF_MD(0,) part + 0x5000 / ss, 1, oschain_sec) < 0) return -16;
    for (i = 0; i < 12; i++) {
        const uint8_t *e = oschain_sec + 40 * i;
        if (!memcmp(e + 4, "lfso", 4)) {
            memcpy(&dev, e + 0x0c, 4); memcpy(&len, e + 0x10, 4); memcpy(&chk, e + 0x1c, 4);
            found = 1; break;
        }
    }
    if (!found) return -17;
    if (len < 0x10000 || len + ss > maxsz || (dev + 0x1000) % ss) return -18;
    while (done < len) {
        uint32_t n = (len - done + ss - 1) / ss;
        if (n > 64) n = 64;
        if (storage_read_sectors(IF_MD(0,) part + (dev + 0x1000) / ss + done / ss, n, dst + done) < 0) return -19;
        done += n * ss;
    }
    /* byte sum, four bytes per word: the same value, a quarter of the loop (12 MB) */
    {
        const uint32_t *w = (const uint32_t *)dst;
        uint32_t nw = len / 4;
        for (i = 0; i < nw; i++) {
            uint32_t v = w[i];
            sum += (v & 0xff) + ((v >> 8) & 0xff) + ((v >> 16) & 0xff) + (v >> 24);
        }
        for (i = nw * 4; i < len; i++) sum += dst[i];
    }
    if (sum != chk) { printf("osfl checksum %08lx != %08lx", (unsigned long)sum, (unsigned long)chk); return -20; }
    return (long)len;
}

/* the FAT volume is mounted only when something has to be read from it (the osfl path reads
   raw sectors): one less thing between power-on and the OS */
static int oschain_mounted;
static int oschain_mount(void)
{
    if (!oschain_mounted) oschain_mounted = disk_mount_all() > 0 ? 1 : -1;
    return oschain_mounted;
}
static long oschain_load_file(const char *name, uint8_t *dst, uint32_t maxsz)
{
    int fd;
    off_t sz;
    ssize_t rd;
    if (oschain_mount() < 0) return -4;
    fd = open(name, O_RDONLY);
    if (fd < 0) return -1;
    sz = filesize(fd);
    if (sz < OSCHAIN_HDR_SZ + 0x10000 || sz > (off_t)maxsz) { close(fd); printf("%s: bad size %ld", name, (long)sz); return -2; }
    rd = read(fd, dst, sz);
    close(fd);
    if (rd != sz) { printf("%s: read error %ld", name, (long)rd); return -3; }
    return (long)sz;
}

/* Returns only on failure */
static int oschain_boot(void)
{
    uint8_t *hdr = (uint8_t *)(OSCHAIN_LOADADDR - OSCHAIN_HDR_SZ);
    uint32_t *body = (uint32_t *)OSCHAIN_LOADADDR;
    uint32_t body_sz;
    long sz;

#ifdef OSCHAIN_FILEFIRST
    /* RAM-only variant: the file on the FAT volume first (a new OS can then install itself) */
    printf("Loading OS from " OSCHAIN_FILE "...");
    sz = oschain_load_file(OSCHAIN_FILE, hdr, OSCHAIN_HDR_SZ + OSCHAIN_MAXSIZE);
    if (sz < 0) { printf("  not there (%ld), trying the firmware partition", sz); sz = oschain_load_osfl(hdr, OSCHAIN_HDR_SZ + OSCHAIN_MAXSIZE); }
#else
    printf("Loading OS from firmware partition...");
    sz = oschain_load_osfl(hdr, OSCHAIN_HDR_SZ + OSCHAIN_MAXSIZE);
#endif
    if (sz < 0) {
        printf("  not there (%ld), trying " OSCHAIN_FILE, sz);
        sz = oschain_load_file(OSCHAIN_FILE, hdr, OSCHAIN_HDR_SZ + OSCHAIN_MAXSIZE);
    }
    if (sz < 0) {
        printf("  trying " OSCHAIN_FILE2);
        sz = oschain_load_file(OSCHAIN_FILE2, hdr, OSCHAIN_HDR_SZ + OSCHAIN_MAXSIZE);
    }
    if (sz < 0) {
        printf("No OS image found");
        return -1;
    }

    memcpy(&body_sz, hdr + 0x0c, 4);
    if (memcmp(hdr, "8702", 4) || body_sz > (uint32_t)(sz - OSCHAIN_HDR_SZ)) {
        printf("Not a decrypted IMG1");
        return -4;
    }
    /* first vector must be an ARM branch, else it is still encrypted */
    if ((body[0] & 0xff000000) != 0xea000000) {
        printf("Body not decrypted: %08lx", (unsigned long)body[0]);
        return -5;
    }

    printf("OS body: %lu bytes", (unsigned long)body_sz);
    if (oschain_build_sysinfo((uint8_t *)OSCHAIN_SYSINFO) < 0)
        return -6;
#ifdef OSCHAIN_RAMTEST
    printf("CLKCON0 %08lx CLKCON1 %08lx", (unsigned long)CLKCON0, (unsigned long)CLKCON1);
    printf("PWRCON %08lx %08lx", (unsigned long)PWRCON(0), (unsigned long)PWRCON(1));
    printf("DOWN1OUT %02x (%d mV)", pmu_read(0x1e), 625 + 25 * pmu_read(0x1e));
    printf("RAM test: jumping in 3 s");
    lcd_update();
    sleep(3 * HZ);
#endif
    printf("Starting OS...");
    lcd_update();

#if (CONFIG_STORAGE & STORAGE_ATA)
    /* Flush the drive's write cache and put it to sleep (as the OF path does with ata_sleepnow):
       the OS must take over an idle drive (2026-10-07). On the flash card behind the iFlash this
       only gates the controller clock; no settle time is needed after it (the headset remote,
       which the old 2.5 s wait here was for, is handled by the OS itself since D43). */
    ata_sleepnow();
#endif

    oschain_jump(OSCHAIN_LOADADDR);
}
#endif /* IPOD_6G && !S5L87XX_DEVELOPMENT_BOOTLOADER */

/*  The boot sequence is executed on power-on or reset. After power-up
 *  the device could come from a state of hibernation, OF hibernates
 *  the iPod after an inactive period of ~30 minutes, on this state the
 *  SDRAM is in self-refresh mode.
 *
 *  t0 = 0
 *     S5L8702 BOOTROM loads an IM3 image located at NOR:
 *     - IM3 header (first 0x800 bytes) is loaded at IRAM1_ORIG
 *     - IM3 body (decrypted RB bootloader) is loaded at IRAM0_ORIG
 *     The time needed to load the RB bootloader (~100 Kb) is estimated
 *     on 200~250 ms. Once executed, RB booloader moves itself from
 *     IRAM0_ORIG to IRAM1_ORIG+0x800, preserving current IM3 header
 *     that contains the NOR offset where the ONB (original NOR boot),
 *     is located (see dualboot.c for details).
 *
 *  t1 = ~250 ms.
 *     If the PMU is hibernated, decrypted ONB (size 128Kb) is loaded
 *       and executed, it takes ~120 ms. Then the ONB restores the
 *       iPod to the state prior to hibernation.
 *     If not, initialize system and RB kernel, wait for t2.
 *
 *  t2 = ~650 ms.
 *     Check user button selection.
 *     If OF, diagmode, or diskmode is selected then launch ONB.
 *     If not, wait for LCD initialization.
 *
 *  t3 = ~700,~900 ms. (lcd_type_01,lcd_type_23)
 *     LCD is initialized, baclight ON.
 *     Wait for HDD spin-up.
 *
 *  t4 = ~2600,~2800 ms.
 *     HDD is ready.
 *     If hold switch is locked, then load and launch ONB.
 *     If not, load rockbox.ipod file from HDD.
 *
 *  t5 = ~2800,~3000 ms.
 *     rockbox.ipod is executed.
 */

#ifdef S5L87XX_DEVELOPMENT_BOOTLOADER
#include "piezo.h"
#include "lcd-s5l8702.h"
extern int lcd_type;

static uint16_t alive[] = { 500,100,0, 0 };
static uint16_t alivelcd[] = { 2000,200,0, 0 };

#ifdef HAVE_LCD_SLEEP
static void sleep_test(void)
{
    int sleep_tmo = 5;
    int awake_tmo = 3;

    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;

    printf("Entering LCD sleep mode in %d seconds,", sleep_tmo);
    printf("during sleep mode you will see a white");
    printf("screen for about %d seconds.", awake_tmo);
    while (sleep_tmo--) {
        printf("Sleep in %d...", sleep_tmo);
        sleep(HZ*1);
    }
    lcd_sleep();
    sleep(HZ*awake_tmo);
    lcd_awake();

    line++;
    printf("Awake!");

    line++;
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);
}
#endif

static void pmu_info(void)
{
    int loop = 0;

    lcd_clear_display();
    lcd_update();
    while (button_status() != BUTTON_NONE);

    while (1)
    {
        lcd_set_foreground(LCD_WHITE);
        lcd_clear_display();
        line = 0;
        printf("loop: %d", loop++);

        for (int i = 0; i < 128; i += 8)
        {
            unsigned char buf[8];

#if defined(IPOD_NANO3G)
            if (i == 0) {
                static int flip = 0;
                if (flip) {
                    pmu_write(6, 0xff);
                    pmu_write(7, 0xff);
                }
                else {
                    pmu_write(6, 0xe7);
                    pmu_write(7, 0xfe);
                }
                flip ^= 1;
            }
#elif defined(IPOD_NANO4G)
            if (i == 120)
                for (int j = 0; j < 8; j++)
                    pmu_write(i+j, j);
#endif
            for (int j = 0; j < 8; j++)
                buf[j] = pmu_read(i+j);

            printf(" %2x: %2x %2x %2x %2x %2x %2x %2x %2x", i,
                    buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
        }
        line++;
        printf("USB: %s    ", (usb_detect() == USB_INSERTED) ? "inserted" : "not inserted");
#if CONFIG_CHARGING
        printf("Firewire: %s    ", pmu_firewire_present() ? "inserted" : "not inserted");
#endif
#ifdef IPOD_ACCESSORY_PROTOCOL
        printf("Accessory: %s    ", pmu_accessory_present() ? "inserted" : "not inserted");
#endif
        printf("Hold Switch: %s  ", pmu_holdswitch_locked() ? "locked" : "unlocked");
        line++;
        lcd_set_foreground(LCD_RBYELLOW);
        printf("Press SELECT to continue");
        if (button_status() == BUTTON_SELECT)
            break;
        sleep(HZ/2);
    }
}

static void gpio_info(void)
{
    int loop = 0;

    lcd_clear_display();

    while (1)
    {
        lcd_set_foreground(LCD_WHITE);
        lcd_clear_display();
        line = 0;
        printf("loop: %d", loop++);
        for (int i = 0; i < GPIO_N_GROUPS; i ++)
        {
            printf(" %x: %8x %2x %4x %2x %2x", i,
                    PCON(i), PDAT(i), PUNA(i), PUNB(i), PUNC(i));
        }
        line++;
        lcd_set_foreground(LCD_RBYELLOW);
        printf("Press SELECT to continue");
        if (button_status() == BUTTON_SELECT)
            break;
        sleep(HZ/5);
    }
}

static void run_of(void)
{
    int tmo = 5;
    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;
    while (tmo--) {
        printf("Booting OF in %d...", tmo);
        sleep(HZ*1);
    }

    int rc = kernel_launch_onb();
    printf("Load OF error: %d", rc);
    sleep(HZ*10);
}

#if defined(IPOD_6G) || defined(IPOD_NANO3G)
static void print_syscfg(void)
{
    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;

    struct SysCfg syscfg;
    const ssize_t result = syscfg_read(&syscfg);

    if (result == -1) {
        printf("SCfg magic not found. NOR flash is corrupted.");
        goto end;
    }

    printf("Total size: %lu bytes, %lu entries", syscfg.header.size, syscfg.header.num_entries);

    if (result > 0) {
        printf("Wrong size: expected %ld, got %lu", result, syscfg.header.size);
    }

    if (syscfg.header.num_entries > SYSCFG_MAX_ENTRIES) {
        printf("Too many entries, showing only first %u", SYSCFG_MAX_ENTRIES);
    }

    const size_t syscfg_num_entries = MIN(syscfg.header.num_entries, SYSCFG_MAX_ENTRIES);

    for (size_t i = 0; i < syscfg_num_entries; i++) {
        const struct SysCfgEntry* entry = &syscfg.entries[i];
        const char* tag = (char *)&entry->tag;
        const uint32_t* data32 = (uint32_t *)entry->data;

        switch (entry->tag) {
        case SYSCFG_TAG_SRNM:
            printf("Serial number (SrNm): %s", entry->data);
            break;
        case SYSCFG_TAG_FWID:
            printf("Firmware ID (FwId): %07lX", data32[1] & 0x0FFFFFFF);
            break;
        case SYSCFG_TAG_HWID:
            printf("Hardware ID (HwId): %08lX", data32[0]);
            break;
        case SYSCFG_TAG_HWVR:
            printf("Hardware version (HwVr): %06lX", data32[1]);
            break;
        case SYSCFG_TAG_CODC:
            printf("Codec (Codc): %s", entry->data);
            break;
        case SYSCFG_TAG_SWVR:
            printf("Software version (SwVr): %s", entry->data);
            break;
        case SYSCFG_TAG_MLBN:
            printf("Logic board serial number (MLBN): %s", entry->data);
            break;
        case SYSCFG_TAG_MODN:
            printf("Model number (Mod#): %s", entry->data);
            break;
        case SYSCFG_TAG_REGN:
            printf("Sales region (Regn): %08lX %08lX", data32[0], data32[1]);
            break;
        default:
            printf("%c%c%c%c: %08lX %08lX %08lX %08lX",
                tag[3], tag[2], tag[1], tag[0],
                data32[0], data32[1], data32[2], data32[3]
            );
            break;
        }
    }

end:
    line++;
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);
}

static void print_bootloader_hash(void)
{
    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;

    struct Im3Info hinfo;
    int rc = im3_read(NORBOOT_OFF, &hinfo, NULL);

    if (rc != 0) {
        printf("Error loading the primary bootloader: %d", rc);
        goto end;
    }

    unsigned char primary_hash[SIGN_SZ];

    memcpy(primary_hash, hinfo.u.enc12.data_sign, SIGN_SZ);
    hwkeyaes(HWKEYAES_DECRYPT, HWKEYAES_UKEY, primary_hash, SIGN_SZ);

    unsigned bl_nor_sz = im3_nor_sz(&hinfo);
    rc = im3_read(NORBOOT_OFF + bl_nor_sz, &hinfo, NULL);

    if (rc == 0) {
        // Rockbox bootloader is installed as primary
        // Stock bootloader is backed up
        unsigned char backup_hash[SIGN_SZ];
        memcpy(backup_hash, hinfo.u.enc12.data_sign, SIGN_SZ);
        hwkeyaes(HWKEYAES_DECRYPT, HWKEYAES_UKEY, backup_hash, SIGN_SZ);

        printf("Rockbox bootloader hash:");

        for (int i = 0; i < SIGN_SZ; i++) {
            lcd_putsf(i * 2, line, "%02X", primary_hash[i]);
        }

        line += 2;
        lcd_update();

        printf("Stock bootloader hash:");

        for (int i = 0; i < SIGN_SZ; i++) {
            lcd_putsf(i * 2, line, "%02X", backup_hash[i]);
        }

        line++;
        lcd_update();
    }
    else {
        // Stock bootloader is installed as primary
        // No backup bootloader
        printf("Rockbox bootloader is not installed!");
        line++;

        printf("Stock bootloader hash:");

        for (int i = 0; i < SIGN_SZ; i++) {
            lcd_putsf(i * 2, line, "%02X", primary_hash[i]);
        }

        line++;
        lcd_update();
    }

end:
    line++;
    while (button_status() != BUTTON_NONE)
        sleep(HZ/100);
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);
}

#ifdef HAVE_SERIAL

#define FLASH_PAGES (FLASH_SIZE >> 12)
#define FLASH_PAGE_SIZE (FLASH_SIZE >> 8)

static void dump_bootflash(void)
{
    lcd_clear_display();
    lcd_set_foreground(LCD_WHITE);
    line = 0;

    uint8_t page[FLASH_PAGE_SIZE];
    printf("Total pages: %d", FLASH_PAGES);

    bootflash_init(SPI_PORT);

    for (int i = 0; i < FLASH_PAGES; i++) {
        printf("Reading flash... %d", i + 1);
        bootflash_read(SPI_PORT, i << 12, FLASH_PAGE_SIZE, page);

        printf("Sending over UART... %d", i + 1);
        serial_tx_raw(page, FLASH_PAGE_SIZE);
        line -= 2;
    }

    bootflash_close(SPI_PORT);

    line += 2;
    printf("Done!");
    piezo_seq(alive);

    line++;
    lcd_set_foreground(LCD_RBYELLOW);
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);
}
#endif /* HAVE_SERIAL */
#endif /* IPOD_6G || IPOD_NANO3G */

static void devel_menu(void)
{
    const char *items[] = {
#ifdef HAVE_LCD_SLEEP
        "LCD sleep/awake test",
#endif
        "PMU info",
        "GPIO info",
#if defined(IPOD_6G) || defined(IPOD_NANO3G)
        "Show SysCfg",
        "Show bootloader hash",
#ifdef HAVE_SERIAL
        "Dump bootflash to UART",
#endif
#endif
        "Launch OF",
        //"Launch Rockbox",
        "Restart",
        "Power off",
    };
    void (*handlers[])(void) = {
#ifdef HAVE_LCD_SLEEP
        sleep_test,
#endif
        pmu_info,
        gpio_info,
#if defined(IPOD_6G) || defined(IPOD_NANO3G)
        print_syscfg,
        print_bootloader_hash,
#ifdef HAVE_SERIAL
        dump_bootflash,
#endif
#endif
        run_of,
        //run_rockbox,
        system_reboot,
        power_off,
    };
    const size_t items_count = sizeof(items) / sizeof(items[0]);
    unsigned char selected_item = 0;

    while (1)
    {
        lcd_clear_display();
        lcd_set_foreground(LCD_RBYELLOW);
        line = 0;
        printf("Development menu");

        for (size_t i = 0; i < items_count; i++) {
            lcd_set_foreground(i == selected_item ? LCD_GREEN : LCD_WHITE);
            printf(items[i]);
        }

        while (button_status() != BUTTON_NONE);

        bool done = false;
        while (!done)
        {
            switch (button_status())
            {
                case BUTTON_MENU:
                case BUTTON_LEFT:
                    if (selected_item > 0) {
                        selected_item--;
                        done = true;
                    }
                    else {
                        sleep(HZ/100);
                    }
                    break;
                case BUTTON_PLAY:
                case BUTTON_RIGHT:
                    if (selected_item < items_count - 1) {
                        selected_item++;
                        done = true;
                    }
                    else {
                        sleep(HZ/100);
                    }
                    break;
                case BUTTON_SELECT:
                    handlers[selected_item]();
                    done = true;
                    break;
                default:
                    sleep(HZ/100);
                    break;
            }
        }
    }
}
#endif /* S5L87XX_DEVELOPMENT_BOOTLOADER */

void main(void)
{
    int rc = 0;
    int boot_btn = 0;

    usec_timer_init();

#ifdef S5L87XX_DEVELOPMENT_BOOTLOADER
    piezo_seq(alive);
#endif

    /* Configure I2C0 */
    i2c_preinit(0);

#ifndef OSCHAIN_NORESUME
    if (pmu_is_hibernated()) {
        rc = launch_onb(1); /* 27/2 = 13.5 MHz. */
    }
#endif
    /* OSCHAIN_NORESUME (rescue RAM image): no hand-over to Apple's NOR boot on a hibernated
       flag; system_preinit -> pmu_preinit then clears it, which ends a resume loop. */

    system_preinit();
    memory_init();
    /*
     * XXX: BSS is initialized here, do not use .bss before this line
     */
    bss_init();

    system_init();
    kernel_init();
    i2c_init();
    power_init();

    enable_irq();
#ifdef OSCHAIN
    /* The headphone remote chip (Mikey, I2C0 0x72) hangs off the PMU's accessory 3.3 V rail (HCLDO),
       which pmu_preinit leaves off. Full Rockbox switches it on at start-up and the remote then works
       in the OS; switched on this early so it has been up for seconds by the OS start. */
    accessory_supply_set(true);
#endif

#ifdef HAVE_SERIAL
    serial_setup();
#endif

    button_init();
    if (rc == 0) {
        /* User button selection timeout */
        /* 0.2 s is enough for a combination held since power-on to read steadily (was 0.4 s) */
        while (USEC_TIMER < 200000);
        int btn = button_read_device();
        boot_btn = btn;
        /* This prevents HDD spin-up when the user enters DFU */
        if (btn == (BUTTON_SELECT|BUTTON_MENU)) {
            while (button_read_device() == (BUTTON_SELECT|BUTTON_MENU))
                sleep(HZ/10);
            sleep(HZ);
            btn = button_read_device();
        }
        /* Enter OF, diagmode and diskmode using ONB */
        if ((btn == BUTTON_MENU)
                || (btn == (BUTTON_SELECT|BUTTON_LEFT))
                || (btn == (BUTTON_SELECT|BUTTON_PLAY))) {
            rc = kernel_launch_onb();
        }
    }

    lcd_init();
    lcd_set_foreground(LCD_WHITE);
    lcd_set_background(LCD_BLACK);
    lcd_clear_display();
    font_init();
    lcd_setfont(FONT_SYSFIXED);

    // TODO: see if removing this causes the nano3g LCD to initialize properly
#ifdef S5L87XX_DEVELOPMENT_BOOTLOADER
    sleep(HZ);
    for (int i = 0; i < lcd_type+1; i++) {
        sleep(HZ/2);
        piezo_seq(alivelcd);
    }
#endif

    lcd_update();
    sleep(HZ/40);  /* wait for lcd update */

    verbose = true;

    printf("Rockbox boot loader");
    printf("Version: %s", rbversion);

    backlight_init(); /* Turns on the backlight */

#ifdef S5L87XX_DEVELOPMENT_BOOTLOADER
    line++;
    printf("lcd type: %d", lcd_type);
#ifdef S5L_LCD_WITH_READID
    extern unsigned char lcd_id[4];
    uint32_t* lcd_id_32 = (uint32_t *)lcd_id;
    printf("lcd id: 0x%x", *lcd_id_32);
#endif
#ifdef IPOD_NANO4G
    printf("boot cfg: 0x%x", pmu_read(0x7f));
#endif
    line++;
    printf("Press SELECT to continue");
    while (button_status() != BUTTON_SELECT)
        sleep(HZ/100);

    devel_menu();
#endif /* S5L87XX_DEVELOPMENT_BOOTLOADER */

#ifndef S5L87XX_DEVELOPMENT_BOOTLOADER
    if (rc == 0) {
#if (CONFIG_STORAGE & STORAGE_ATA)
        /* Wait until there is enought power to spin-up HDD */
        battery_trap();
#endif

        rc = storage_init();
        if (rc != 0) {
            printf("Storage error: %d", rc);
            fatal_error(ERR_STORAGE);
        }

        filesystem_init();

        /* We wait until HDD spins up to check for hold button */
        if (button_hold()) {
#ifdef SYSCFG_MAX_ENTRIES
            bool lba48 = false;
            struct SysCfg syscfg;
            const ssize_t result = syscfg_read(&syscfg);
            if (result != -1) {
                const size_t syscfg_num_entries = MIN(syscfg.header.num_entries, SYSCFG_MAX_ENTRIES);
                for (size_t i = 0; i < syscfg_num_entries; i++) {
                    const struct SysCfgEntry* entry = &syscfg.entries[i];
                    const uint32_t* data32 = (uint32_t *)entry->data;
                    if (entry->tag == SYSCFG_TAG_HWVR) {
                        lba48 = (data32[1] >= 0x130200);
                        break;
                    }
                }

                int btn = button_read_device();

                struct storage_info sinfo;
                storage_get_info(0, &sinfo);
                if (sinfo.num_sectors < (1 << 28) || lba48 || btn & BUTTON_LEFT) {
                    printf("Executing OF...");
#if (CONFIG_STORAGE & STORAGE_ATA)
                    ata_sleepnow();
#endif
                    rc = kernel_launch_onb();
                } else {
                    printf("OF does not support LBA48");
                    fatal_error(ERR_LBA28);
                }
            }
#else
            printf("Executing OF...");
#if (CONFIG_STORAGE & STORAGE_ATA)
            ata_sleepnow();
#endif
            rc = kernel_launch_onb();
#endif /* SYSCFG_MAX_ENTRIES */
        }
    }

    if (rc != 0) {
        printf("Load OF error: %d", rc);
        fatal_error(ERR_OF);
    }

#ifdef HAVE_BOOTLOADER_USB_MODE
    /* Enter USB mode if SELECT+RIGHT are pressed */
    if (button_read_device() == (BUTTON_SELECT|BUTTON_RIGHT)) {
#if defined(MAX_VIRT_SECTOR_SIZE) && defined(DEFAULT_VIRT_SECTOR_SIZE)
#ifdef HAVE_MULTIDRIVE
            for (int i = 0 ; i < NUM_DRIVES ; i++)
#endif
                disk_set_sector_multiplier(IF_MD(i,) DEFAULT_VIRT_SECTOR_SIZE/SECTOR_SIZE);
#endif
        usb_mode();
    }
#endif

#if defined(IPOD_6G) && defined(OSCHAIN)
    /* Our OS is the default. PLAY at power-on (or now) boots Rockbox instead. */
    if (!((boot_btn | button_read_device()) & BUTTON_PLAY)) {
        rc = oschain_boot();
        printf("OS boot failed: %d", rc);
        sleep(3*HZ);
    } else {
        printf("PLAY held: Rockbox");
    }
    rc = oschain_mount() > 0 ? 1 : 0;
#else
    (void)boot_btn;
    rc = disk_mount_all();
#endif
    if (rc <= 0) {
#ifdef STORAGE_GET_INFO
        struct storage_info sinfo;
        storage_get_info(0, &sinfo);
#ifdef MAX_PHYS_SECTOR_SIZE
        printf("id: '%s' s:%u*%u", sinfo.product, sinfo.sector_size, sinfo.phys_sector_mult);
#else
        printf("id: '%s' s:%u", sinfo.product, sinfo.sector_size);
#endif
#endif
        struct partinfo pinfo;
        printf("No partition found");
        for (int i = 0 ; i < NUM_VOLUMES ; i++) {
            disk_partinfo(i, &pinfo);
            if (pinfo.type)
                printf("P%d T%02x S%llx",
                       i, pinfo.type, (unsigned long long)pinfo.size);
        }
        fatal_error(ERR_RB);
    }

    printf("Loading Rockbox...");
    unsigned char *loadbuffer = (unsigned char *)DRAM_ORIG;
    rc = load_firmware(loadbuffer, BOOTFILE, MAX_LOADSIZE);

    if (rc <= EFILE_EMPTY) {
        printf("Error!");
        printf("Can't load " BOOTFILE ": ");
        printf(loader_strerror(rc));
        fatal_error(ERR_RB);
    }

    printf("Rockbox loaded.");

    /* If we get here, we have a new firmware image at 0x08000000, run it */
    disable_irq();

    int (*kernel_entry)(void) = (void*)loadbuffer;
    commit_discard_idcache();
    rc = kernel_entry();

    /* End stop - should not get here */
    enable_irq();
    printf("ERR: Failed to boot");
    while(1);
#endif
}
