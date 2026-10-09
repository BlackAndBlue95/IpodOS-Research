#!/usr/bin/env python3
"""Turns the oschain patch (already applied to bootloader/ipod-s5l87xx.c) into the final chainload:
- everything under #ifdef OSCHAIN, so a build without it is the stock Rockbox bootloader;
- loads our OS from the firmware partition's 'osfl' section (checksum verified), then
  /osos-v18.dec, then /osos-boot.dec;
- boots it unless PLAY is held (PLAY = Rockbox); MENU/hold switch keep going to Apple's bootloader;
- hands over Apple's clock gating (PWRCON, CLKCON2-5) and never touches CLKCON1 or the voltage;
- loads at DRAM_ORIG + 0x02000000, aborts without SysCfg, keeps SysInfo out of its own code;
- OSCHAIN_RAMTEST prints the clock/power registers and pauses before the jump.
  oschain_mod.py bootloader/ipod-s5l87xx.c"""
import sys

p = sys.argv[1]
s = open(p).read()
def rep(a, b, count=1):
    global s
    assert s.count(a) == count, (a[:80], s.count(a))
    s = s.replace(a, b)

rep('#if defined(IPOD_6G) && !defined(S5L87XX_DEVELOPMENT_BOOTLOADER)\n/*\n * Chainload Apple\'s decrypted RetailOS ("osos") from the FAT32 disk.',
    '#if defined(IPOD_6G) && defined(OSCHAIN) && !defined(S5L87XX_DEVELOPMENT_BOOTLOADER)\n/*\n * Chainload our OS (Apple\'s decrypted RetailOS with region E) from the firmware partition or the FAT32 disk.')
rep('#define OSCHAIN_FILE        "/osos-boot.dec"\n', '#define OSCHAIN_FILE        "/osos-v18.dec"\n#define OSCHAIN_FILE2       "/osos-boot.dec"\n')
rep('#define OSCHAIN_LOADADDR    (DRAM_ORIG + 0x01000000) /* above bootloader BSS */',
    '#define OSCHAIN_LOADADDR    (DRAM_ORIG + 0x02000000) /* above bootloader BSS, below the SysInfo staging area */')

# SysCfg must be there
rep('static void oschain_build_sysinfo(uint8_t *si)\n{', 'static int oschain_build_sysinfo(uint8_t *si)\n{')
rep('''    if (hdr.magic != SYSCFG_MAGIC) {
        bootflash_close(SPI_PORT);
        printf("SysCfg not found");
        return;
    }''', '''    if (hdr.magic != SYSCFG_MAGIC) {
        bootflash_close(SPI_PORT);
        printf("SysCfg not found");
        return -1;
    }''')
rep('''    bootflash_close(SPI_PORT);
    printf("SysCfg: %u entries", n);
}''', '''    bootflash_close(SPI_PORT);
    printf("SysCfg: %u entries", n);
    return 0;
}''')

# the jump: Apple's clock gating, SysInfo kept away from this function too
rep('''    disable_interrupt(IRQ_FIQ_STATUS);
    VIC0INTENCLEAR = 0xffffffff;
    VIC1INTENCLEAR = 0xffffffff;
    eint_init();
''', '''    disable_interrupt(IRQ_FIQ_STATUS);
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
''')
rep('''    uintptr_t flush = (uintptr_t)commit_discard_idcache;
    if (flush + 0x100 < OSCHAIN_SYSINFO_IRAM ||
            flush >= OSCHAIN_SYSINFO_IRAM + OSCHAIN_SYSINFO_SZ) {''',
    '''    uintptr_t flush = (uintptr_t)commit_discard_idcache, me = (uintptr_t)oschain_jump;
    if ((flush + 0x100 < OSCHAIN_SYSINFO_IRAM || flush >= OSCHAIN_SYSINFO_IRAM + OSCHAIN_SYSINFO_SZ) &&
        (me + 0x400 < OSCHAIN_SYSINFO_IRAM || me >= OSCHAIN_SYSINFO_IRAM + OSCHAIN_SYSINFO_SZ)) {''')

# loading: firmware partition first, then the two files
rep('''/* Returns only on failure */
static int oschain_boot(void)
{
    uint8_t *hdr = (uint8_t *)(OSCHAIN_LOADADDR - OSCHAIN_HDR_SZ);
    uint32_t *body = (uint32_t *)OSCHAIN_LOADADDR;
    uint32_t body_sz;

    int fd = open(OSCHAIN_FILE, O_RDONLY);
    if (fd < 0) {
        printf("No " OSCHAIN_FILE);
        return -1;
    }

    printf("Loading Apple OS...");
    off_t sz = filesize(fd);
    if (sz < OSCHAIN_HDR_SZ + 0x10000 || sz > OSCHAIN_HDR_SZ + OSCHAIN_MAXSIZE) {
        close(fd);
        printf("Bad size: %ld", (long)sz);
        return -2;
    }

    ssize_t rd = read(fd, hdr, sz);
    close(fd);
    if (rd != sz) {
        printf("Read error: %ld", (long)rd);
        return -3;
    }
''', '''/* Our OS in the firmware partition: MSE directory at +0x5000 (12 entries of 40 bytes:
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
    for (i = 0; i < len; i++) sum += dst[i];
    if (sum != chk) { printf("osfl checksum %08lx != %08lx", (unsigned long)sum, (unsigned long)chk); return -20; }
    return (long)len;
}

static long oschain_load_file(const char *name, uint8_t *dst, uint32_t maxsz)
{
    int fd = open(name, O_RDONLY);
    off_t sz;
    ssize_t rd;
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

    printf("Loading OS from firmware partition...");
    sz = oschain_load_osfl(hdr, OSCHAIN_HDR_SZ + OSCHAIN_MAXSIZE);
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
''')
rep('''    printf("OS body: %lu bytes", (unsigned long)body_sz);
    oschain_build_sysinfo((uint8_t *)OSCHAIN_SYSINFO);
    printf("Starting Apple OS...");
    lcd_update();
''', '''    printf("OS body: %lu bytes", (unsigned long)body_sz);
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
''')

# boot decision: PLAY = Rockbox, otherwise our OS
rep('''#ifdef IPOD_6G
    /* PLAY at power-on, or every boot in the chainload test build */
#ifdef OSCHAIN_ALWAYS
    (void)boot_btn;
    if (1) {
#else
    if (boot_btn == BUTTON_PLAY) {
#endif
        rc = oschain_boot();
        printf("Apple OS boot failed: %d", rc);
        sleep(3*HZ);
    }
#else
    (void)boot_btn;
#endif
''', '''#if defined(IPOD_6G) && defined(OSCHAIN)
    /* Our OS is the default. PLAY at power-on (or now) boots Rockbox instead. */
    if (!((boot_btn | button_read_device()) & BUTTON_PLAY)) {
        rc = oschain_boot();
        printf("OS boot failed: %d", rc);
        sleep(3*HZ);
    } else {
        printf("PLAY held: Rockbox");
    }
#else
    (void)boot_btn;
#endif
''')
open(p, 'w').write(s)
print('chainload modifications applied')
