/***************************************************************************
 * osos_boot - boot the decrypted Apple OS (/osos-boot.dec) from Rockbox
 *
 * Loads the 0x800 byte IMG1 header plus decrypted body into the top of
 * the audio buffer, rebuilds the SysInfo block Apple's bootloader hands
 * over (from NOR SysCfg), fills the IRAM mailbox, turns off the MMU and
 * caches and jumps into the OS body. The OS relocates itself to
 * IRAM 0x22000000 and DRAM 0x08000000, so it must be loaded well above
 * that, and this plugin's code (in the plugin buffer at the top of DRAM)
 * is out of the way too.
 *
 * v12: before loading the OS, brings iTunesDB up to date with the .flac
 * files under /Music (libsync.c, the C port of flacsync.py), so the
 * library follows .flac files copied onto the disk without a computer.
 * Hold PLAY while starting the plugin to skip it.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 ****************************************************************************/
#include "plugin.h"
#include "s5l87xx.h"
#include "norboot-target.h"

#ifndef OS_FILE                  /* osos_boot_v18.c sets its own image */
#define OS_FILE         "/osos-boot.dec"
#endif
#define HDR_SZ          0x800
#define MAX_BODY        0x01800000
#define RELOC_END       (DRAM_ORIG + 0x01800000) /* keep the image above this */
#define SYSINFO_SZ      0x120
#define SYSINFO_IRAM    0x22028cc0
#define MAILBOX         0x2203ff00
#define SYSINFO_MAGIC   0x53797349
#define MAX_ENTRIES     32
#define CRASH_LOG       0x2203ff80
#define CRASH_MAGIC     0x44454144 /* 'DEAD' */
#define FLAC_TRACE      0x2203c000
#define FLAC_TRACE_N    48
#define FLAC_TRACE_MAGIC 0x52544c46 /* 'FLTR' */
#define FLAC_TRACE_FILE "/flac-trace.txt"
#define CANARY 0x43414e41 /* 'CANA', survives the reboot? */
static const uint32_t canary_at[3] = { 0x2203c0fc, 0x2203f800, 0x2201fff0 };

static int line;

/* v12b: no clock or voltage changes, the OS gets the clocks Rockbox has
   at the jump, as with v8, the last version that booted. v10/v11 boosted
   for the load and forced 54/54/27 before the jump; on rockpod, which was
   already at 216/108/54 (CLKCON1 0x00404101), the OS then died at start. */
static uint8_t sysinfo[SYSINFO_SZ] __attribute__((aligned(4)));

static void say(const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    rb->vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    rb->lcd_puts(0, line++, buf);
    rb->lcd_update();
}

/* ---- library sync: libsync.c on Rockbox's file API ---- */
#define LS_MEMCPY   rb->memcpy
#define LS_MEMSET   rb->memset
#define LS_MEMCMP   rb->memcmp
#define LS_STRLEN   rb->strlen
#define LS_SNPRINTF rb->snprintf
#include "libsync.c"

static int sync_line;

int ls_open_r(const char *p) { return rb->open(p, O_RDONLY); }
int ls_open_w(const char *p) { return rb->open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666); }
long ls_read(int fd, void *b, long n) { return rb->read(fd, b, n); }
long ls_write(int fd, const void *b, long n) { return rb->write(fd, b, n); }
long ls_seek(int fd, long off) { return rb->lseek(fd, off, SEEK_SET); }
long ls_fsize(int fd) { return rb->filesize(fd); }
int ls_close(int fd) { return rb->close(fd); }
int ls_remove(const char *p) { return rb->remove(p); }
int ls_rename(const char *a, const char *b) { return rb->rename(a, b); }
void *ls_opendir(const char *p) { return rb->opendir(p); }
int ls_readdir(void *d, struct ls_dirent *e)
{
    struct dirent *de = rb->readdir((DIR *)d);
    struct dirinfo info;
    if (!de)
        return 0;
    info = rb->dir_get_info((DIR *)d, de);
    e->name = (const char *)de->d_name;
    e->is_dir = (info.attribute & ATTR_DIRECTORY) != 0;
    e->size = (uint32_t)info.size;
    e->mtime = (uint32_t)info.mtime;
    return 1;
}
void ls_closedir(void *d) { rb->closedir((DIR *)d); }
void ls_qsort(void *b, size_t n, size_t s, int (*c)(const void *, const void *)) { rb->qsort(b, n, s, c); }
void ls_status(const char *m)
{
    char t[52];
    rb->snprintf(t, sizeof(t), "%-50s", m);
    rb->lcd_puts(0, sync_line, t);
    rb->lcd_update();
}

/* Scan /Music and update iTunesDB before the OS reads it. Uses the whole
   audio buffer, the OS image is only loaded into it afterwards.
   Never stops the boot: problems are shown and the OS starts anyway. */
static void library_sync(uint8_t *buf, size_t bufsz)
{
    static struct ls_opts o;
    static struct ls_result r;
    size_t memsz = bufsz;
    long t0 = *rb->current_tick;
    int rc;

    rb->memset(&o, 0, sizeof(o));
    o.root = "";
    rb->memcpy(o.fwid, sysinfo + 0x38, 8);
    o.now = (uint32_t)rb->mktime(rb->get_time());
    for (int i = 0; i < 8; i++)
        o.seed[i] = sysinfo[0x38 + i] ^ (uint8_t)(o.now >> (8 * (i & 3))) ^
                    (uint8_t)((uint32_t)t0 >> (8 * (i & 3))) ^ (uint8_t)(i * 0x5b);
    sync_line = line++;
    rc = ls_sync(&o, buf, memsz, &r);
    if (rc < 0 && r.badsig) {
        /* SysCfg might hold the GUID the other way round */
        for (int i = 0; i < 8; i++)
            o.fwid[i] = sysinfo[0x38 + 7 - i];
        rc = ls_sync(&o, buf, memsz, &r);
    }
    long ms = (*rb->current_tick - t0) * 1000 / HZ;
    if (rc < 0) {
        ls_status("Library not updated:");
        say("  %s", r.msg);
        rb->sleep(HZ * 3);
        return;
    }
    if (!r.wrote) {
        char t[52];
        rb->snprintf(t, sizeof(t), "Library: %d FLACs, up to date (%ld ms)", r.flacs, ms);
        ls_status(t);
    } else {
        char t[52];
        rb->snprintf(t, sizeof(t), "Library: +%d new, %d changed, -%d gone (%ld ms)",
                     r.added, r.updated, r.removed, ms);
        ls_status(t);
        say("  %d tracks in iTunesDB%s", r.tracks, r.playcounts ? ", play counts merged" : "");
        /* Rockbox flushes the drive's write cache only when the drive
           sleeps (and at shutdown and ROLO). Let it sleep once now, so the
           new iTunesDB is on the flash before the OS takes the drive over. */
        rb->storage_sleep();
        rb->sleep(HZ * 3 / 2);
    }
    if (r.skipped)
        say("  %d .flac files not readable, left out", r.skipped);
}

/* ---- minimal polled SPI NOR read, same as firmware norboot/spi code ---- */
static void gate(int g, bool on)
{
    uint32_t bit = 1 << (g & 0x1f);
    if (on) PWRCON(g >> 5) &= ~bit;
    else    PWRCON(g >> 5) |= bit;
}

static void ce(bool on) { GPIOCMD = 0x0000e | (on ? 0 : 1); }

static uint32_t spi_xfer(uint32_t d)
{
    SPIRXLIMIT(0) = 1;
    while ((SPISTATUS(0) & 0x1f0) == 0x100);
    SPITXDATA(0) = d;
    while (!(SPISTATUS(0) & 0x3e00));
    return SPIRXDATA(0);
}

static void nor_read(uint32_t addr, uint32_t size, void *buf)
{
    uint8_t *b = buf;
    uint32_t pcon = PCON0;

    PCON0 = (pcon & ~0xffff) | 0x2222;
    ce(false);
    gate(CLOCKGATE_SPI0, true);
    SPISTATUS(0) = 0xf;
    SPICTRL(0) |= 0xc;
    SPICLKDIV(0) = 4;
    SPIPIN(0) = 6;
    SPISETUP(0) = 0x10618;
    SPICTRL(0) |= 0xc;
    SPICTRL(0) = 1;

    for (;;) {                  /* wait until not busy */
        ce(true);
        spi_xfer(5);
        uint32_t st = spi_xfer(0xff);
        ce(false);
        if (!(st & 1)) break;
    }
    ce(true);
    spi_xfer(3);
    spi_xfer((addr >> 16) & 0xff);
    spi_xfer((addr >> 8) & 0xff);
    spi_xfer(addr & 0xff);
    SPIRXLIMIT(0) = size;
    SPISETUP(0) |= 1;
    while (size--) {
        while (!(SPISTATUS(0) & 0x3e00));
        *b++ = SPIRXDATA(0);
    }
    SPISETUP(0) &= ~1;
    ce(false);
    gate(CLOCKGATE_SPI0, false);
    PCON0 = pcon;
}

static inline void put32(uint8_t *p, int off, uint32_t v)
{
    rb->memcpy(p + off, &v, 4);
}

static int build_sysinfo(uint8_t *si)
{
    struct SysCfgHeader hdr;
    struct SysCfgEntry e;
    int flags;

    rb->memset(si, 0, SYSINFO_SZ);
    put32(si, 0x00, SYSINFO_MAGIC);
    put32(si, 0x04, 4);
    si[0x88] = 'N';
    si[0x89] = 'A';
    put32(si, 0xe0, 0x04000000);     /* DRAM size */
    put32(si, 0xe4, DRAM_ORIG);
    put32(si, 0xe8, 0x00040000);     /* IRAM size */
    put32(si, 0xec, 0x22000000);
    put32(si, 0xf0, NOR_SZ);
    put32(si, 0xf4, 0x24000000);
    put32(si, 0x118, 0x7672736e);
    put32(si, 0x11c, 0x01708004);

    flags = disable_irq_save();
    nor_read(0, sizeof(hdr), &hdr);
    restore_irq(flags);
    if (hdr.magic != SYSCFG_MAGIC) {
        say("SysCfg magic bad: %08lx", (unsigned long)hdr.magic);
        return -1;
    }

    unsigned n = MIN(hdr.num_entries, MAX_ENTRIES);
    for (unsigned i = 0; i < n; i++) {
        uint32_t w0, w1;
        uint16_t h0, h2;

        flags = disable_irq_save();
        nor_read(sizeof(hdr) + i * sizeof(e), sizeof(e), &e);
        restore_irq(flags);
        rb->memcpy(&w0, e.data, 4);
        rb->memcpy(&w1, e.data + 4, 4);
        rb->memcpy(&h0, e.data, 2);
        rb->memcpy(&h2, e.data + 4, 2);

        switch (e.tag) {
        case SYSCFG_TAG_SRNM: rb->memcpy(si + 0x18, e.data, 16); break;
        case SYSCFG_TAG_FWID: rb->memcpy(si + 0x38, e.data + 4, 8); break;
        case SYSCFG_TAG_HWVR: put32(si, 0x84, w1); break;
        case SYSCFG_TAG_MODN: rb->memcpy(si + 0x98, e.data, 16); break;
        case SYSCFG_TAG_CODC: put32(si, 0x104, w0); break;
        case SYSCFG_TAG_SWVR: rb->memcpy(si + 0x108, e.data, 16); break;
        case SYSCFG_TAG_REGN:
            if (h0 == 1) {
                rb->memcpy(si + 0x92, &h2, 2);
                rb->memcpy(si + 0x94, e.data + 6, 2);
            }
            break;
        }
    }
    char mod[17];
    rb->memcpy(mod, si + 0x98, 16);
    mod[16] = 0;
    say("SysCfg: %u entries, model %s", n, mod);
    return 0;
}

/* Everything from here runs with interrupts off and touches nothing
   outside this plugin, IRAM1 and the hardware registers. */
static void __attribute__((noreturn, noinline)) jump(uint32_t entry)
{
    volatile uint32_t *mb = (volatile uint32_t *)MAILBOX;
    volatile uint32_t *dst = (volatile uint32_t *)SYSINFO_IRAM;
    const uint32_t *src = (const uint32_t *)sysinfo;

    disable_interrupt(IRQ_FIQ_STATUS);
    /* Clock gating and clock generators exactly as Apple's bootloader
       (PreEfi) hands them to the OS. The OS only re-gates blocks it used
       itself, so anything left running here would stay on and cost
       battery. SM1 (IRAM1), the LCD, SDRAM and ATA stay running in these
       values. CLKCON1 (CPU/bus dividers) is not touched. */
    PWRCON(0) = 0x2007cd45;
    PWRCON(1) = 0x0003efc9;
    *(volatile uint32_t *)0x3c500008 = 0x80008000;   /* CLKCON2 */
    *(volatile uint32_t *)0x3c50000c = 0x80008000;   /* CLKCON3 */
    *(volatile uint32_t *)0x3c500010 = 0x00008000;   /* CLKCON4 */
    *(volatile uint32_t *)0x3c500014 = 0x00008000;   /* CLKCON5 */
    VIC0INTENCLEAR = 0xffffffff;
    VIC1INTENCLEAR = 0xffffffff;
    for (int i = 0; i < EIC_N_GROUPS; i++) {
        EIC_INTEN(i) = 0;
        EIC_INTLEVEL(i) = 0;
        EIC_INTTYPE(i) = 0;
        EIC_INTSTAT(i) = ~0;
    }

    for (int i = 0; i < SYSINFO_SZ / 4; i++)
        dst[i] = src[i];
    mb[4] = 0;                   /* 0x2203ff10 boot flags */
    mb[6] = SYSINFO_MAGIC;       /* 0x2203ff18 */
    mb[7] = SYSINFO_IRAM;        /* 0x2203ff1c */

    asm volatile(
        "1: mrc   p15, 0, r15, c7, c14, 3 \n" /* clean+invalidate D-cache */
        "   bne   1b                      \n"
        "   mov   r0, #0                  \n"
        "   mcr   p15, 0, r0, c7, c10, 4  \n" /* drain write buffer */
        "   mrc   p15, 0, r0, c1, c0, 0   \n"
        "   bic   r0, r0, #0x1000         \n" /* I-cache off */
        "   bic   r0, r0, #0x5            \n" /* MMU, D-cache off */
        "   mcr   p15, 0, r0, c1, c0, 0   \n"
        "   nop                           \n"
        "   nop                           \n"
        "   mov   r0, #0                  \n"
        "   mcr   p15, 0, r0, c7, c7, 0   \n" /* invalidate I and D */
        "   mcr   p15, 0, r0, c8, c7, 0   \n" /* invalidate TLB */
        "   msr   cpsr_c, #0xd3           \n"
        "   mov   r1, #0                  \n"
        "   mov   r2, #0                  \n"
        "   mov   r3, #0                  \n"
        "   bx    %0                      \n"
        : : "r"(entry) : "r0", "r1", "r2", "r3", "memory", "cc");
    while (1);
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    size_t bufsz;
    uint8_t *buf, *img;
    uint32_t body_sz, *body;
    long btn;

    bool skip_sync = (rb->button_status() & BUTTON_PLAY) != 0;

    rb->lcd_clear_display();
    rb->lcd_setfont(FONT_SYSFIXED);
    line = 0;
    say("osos_boot v12d (library sync, clock gating, safe hand-over)");

    /* Rockbox clock-gates IRAM1 (SM1) at boot, and any access to it
       then hangs. The OS needs it (SysInfo, mailbox), and so does the
       reboot log. Turn it back on. */
    gate(CLOCKGATE_SM1, true);
    say("CLKCON1 %08lx (not changed)", (unsigned long)CLKCON1);
    long t0 = *rb->current_tick;
    say("USB check...");
    if (rb->usb_inserted()) {
        say("Unplug USB first.");
        goto wait_exit;
    }

    /* SysInfo first: the library sync signs iTunesDB with its FireWire GUID */
    if (build_sysinfo(sysinfo) < 0)
        goto wait_exit;
    rb->audio_stop();
    /* v12d: audio_stop only queues the stop; the playback thread may still be streaming from
       the disk. Wait until it is idle: jumping mid-transfer left the OS's first reads of the
       root folder shifted by 16 bytes (2026-10-07, root folder destroyed). */
    for (int i = 0; i < 50 && rb->audio_status(); i++)
        rb->sleep(HZ / 10);
    say(rb->audio_status() ? "Playback still busy!" : "Playback stopped");
    buf = rb->plugin_get_audio_buffer(&bufsz);
    say("Buffer %08lx +%lx", (unsigned long)buf, (unsigned long)bufsz);
    if (skip_sync)
        say("Library sync skipped (PLAY held)");
    else
        library_sync(buf, bufsz);

    say("Opening " OS_FILE "...");
    int fd = rb->open(OS_FILE, O_RDONLY);
    if (fd < 0) {
        say("Can't open " OS_FILE);
        goto wait_exit;
    }
    off_t sz = rb->filesize(fd);
    say("File: %ld bytes", (long)sz);
    if (sz < HDR_SZ + 0x10000 || sz > HDR_SZ + MAX_BODY) {
        rb->close(fd);
        say("Bad size");
        goto wait_exit;
    }

    /* load at the very top of the buffer, 4K aligned */
    img = (uint8_t *)(((uintptr_t)buf + bufsz - (size_t)sz) & ~0xfffUL);
    if ((uint8_t *)img < buf || (uintptr_t)img < RELOC_END) {
        rb->close(fd);
        say("Buffer too low/small for image");
        goto wait_exit;
    }
    say("Loading to %08lx... (%ld ms)", (unsigned long)img, (long)((*rb->current_tick - t0) * 1000 / HZ));
    long tl = *rb->current_tick;
    ssize_t rd = 0;
    int pline = line++;
    while (rd < sz) {
        ssize_t n = sz - rd > 0x100000 ? 0x100000 : sz - rd;
        ssize_t r = rb->read(fd, img + rd, n);
        if (r <= 0) break;
        rd += r;
        char t[48];
        rb->snprintf(t, sizeof(t), "%ld KB, %ld ms", (long)(rd >> 10), (long)((*rb->current_tick - tl) * 1000 / HZ));
        rb->lcd_puts(0, pline, t);
        rb->lcd_update();
    }
    rb->close(fd);
    if (rd != sz) {
        say("Read error %ld", (long)rd);
        goto wait_exit;
    }

    rb->memcpy(&body_sz, img + 0x0c, 4);
    body = (uint32_t *)(img + HDR_SZ);
    if (rb->memcmp(img, "8702", 4) || body_sz > (uint32_t)(sz - HDR_SZ)) {
        say("Not a decrypted IMG1");
        goto wait_exit;
    }
    if ((body[0] & 0xff000000) != 0xea000000) {
        say("Still encrypted? %08lx", (unsigned long)body[0]);
        goto wait_exit;
    }
    say("Body %lu bytes, entry %08lx", (unsigned long)body_sz,
        (unsigned long)body);

    /* Optional stop point: replace one instruction with "b ." so the
       OS freezes there. A freeze shows we got that far, a reboot shows
       it died earlier. Offsets are into the body. */
    static const struct { uint32_t off; const char *name; } stops[] = {
        { 0,      "none, full boot" },
        { 0x88c4, "1 entry (before relocation)" },
        { 0x3a18, "2 after relocation to IRAM" },
        { 0x3a70, "3 after early init" },
        { 0x3ae4, "4 end of reset code" },
        { 0x3682fc, "5 main task start" },
        { 1,      "6 log the reboot" },
    };
    const int nstops = sizeof(stops) / sizeof(stops[0]);
    int sel = 0, sline;

    say("Ready in %ld ms", (long)((*rb->current_tick - t0) * 1000 / HZ));
    if (!(rb->button_status() & (BUTTON_MENU | BUTTON_PLAY)))
        goto boot;
    say("");
    say("Wheel = choose stop point");
    say("SELECT = boot, MENU = cancel");
    sline = line++;
    rb->button_clear_queue();
    for (;;) {
        char t[48];
        rb->snprintf(t, sizeof(t), "Stop: %-30s", stops[sel].name);
        rb->lcd_puts(0, sline, t);
        rb->lcd_update();
        btn = rb->button_get(true);
        if (btn == BUTTON_MENU || btn == (BUTTON_MENU|BUTTON_REL))
            return PLUGIN_OK;
        if (btn == BUTTON_SCROLL_FWD || btn == (BUTTON_SCROLL_FWD|BUTTON_REPEAT))
            sel = (sel + 1) % nstops;
        else if (btn == BUTTON_SCROLL_BACK || btn == (BUTTON_SCROLL_BACK|BUTTON_REPEAT))
            sel = (sel + nstops - 1) % nstops;
        else if (btn == BUTTON_SELECT)
            break;
    }
    if (stops[sel].off == 1) {
        /* Hook Apple's reboot function (body 0x367f00): store r0-r3, sp,
           lr, 8 stack words and cpsr at 0x2203ff80, then freeze. The
           stub overwrites an unused SQLite error string just before it. */
        static const uint32_t stub[] = {
            0xe59fc028, 0xe8ac600f, 0xe89d00ff, 0xe8ac00ff, 0xe10f0000,
            0xe59f1018, 0xe88c0003, 0xee17ff7a, 0x1afffffd, 0xe3a00000,
            0xee070f9a, 0xeafffffe, 0x2203ff80, CRASH_MAGIC,
        };
        rb->memcpy(&body[0x367ec8 / 4], stub, sizeof(stub));
        body[0x367f00 / 4] = 0xeafffff0;         /* b 0x367ec8 */
        say("Reboot logger installed");
    } else if (stops[sel].off) {
        body[stops[sel].off / 4] = 0xeafffffe;   /* b . */
        say("Patched stop at body+%lx", (unsigned long)stops[sel].off);
    }

boot:
    /* v12d: always flush the drive's write cache and let it settle before the OS takes it over
       (Rockbox flushes only when the drive sleeps, at shutdown and at ROLO) */
    rb->storage_sleep();
    rb->sleep(HZ / 2);
    say("Jumping, CLKCON1 %08lx", (unsigned long)CLKCON1);
    rb->commit_discard_idcache();
    jump((uint32_t)body);

wait_exit:
    say("Press any button");
    rb->button_clear_queue();
    rb->button_get(true);
    return PLUGIN_OK;
}
