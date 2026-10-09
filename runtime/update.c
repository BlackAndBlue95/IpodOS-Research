/* Self-update: /osos-update.dec on the FAT volume is written to the firmware partition's osfl
 * section (layout per fwpart.py: directory at 0x5000, 40-byte entries, type bytes reversed; data
 * at devOff + 0x1000; checksum = 32-bit sum of the data), then renamed and the iPod restarts. */
#include "e.h"
#include "os.h"

#define UPD_FILE   "osos-update.dec"
#define UPD_DONE   "osos-update.done"
#define UPD_TRY    "FLAC\\update.try"
#define FW_SIZE    0x0c000000u
#define FW_DIR     0x5000u
#define FW_OSFL    0x5944000u            /* fwpart.py OSFL_OFF */
#define CHUNK      0x10000u

/* Block device class at 0x08270400, the one the USB mass-storage LUN wraps. ctor(obj, 0, 0)
 * covers the whole disk. vt[0x18] open, vt[0x1c] close, vt[0x20] sector size, vt[0x24] sector
 * count. vt[0x08] read and vt[0x0c] write are asynchronous: (self, buf, lba, count <= 256, ctx a,
 * ctx b, completion(request)). The completion runs on the storage thread; the request status is
 * at +0x40, and the completion must release the request as the OS's own (0x0808543c) does. */
typedef struct { void **vt; uint32_t f[8]; } Dev;
#define dev_ctor   ((void *(*)(void *, int, int))0x08270400)
#define dev_open(d)   (((int (*)(void *))(d)->vt[0x18 / 4])(d))
#define dev_close(d)  (((int (*)(void *))(d)->vt[0x1c / 4])(d))
#define dev_ssize(d)  (((uint32_t (*)(void *))(d)->vt[0x20 / 4])(d))
#define dev_nsect(d)  (((uint32_t (*)(void *))(d)->vt[0x24 / 4])(d))
#define os_delay_ms    ((void (*)(int))0x0802cf18)   /* busy wait (timer spin): short gaps only */
#define os_sleep_ms    ((void (*)(int))0x080defd8)   /* task sleep, as the Mikey task uses it */
#define WDTCON (*(volatile uint32_t *)0x3c800000)

/* Synchronous transfers: (self, buf, lba, count <= 256, 0) -> 0 ok. With the fifth argument 0,
   vt[4] is the READ (primitive vt[0xa8] 0x0826e9e4, ATA 0xC8 READ DMA) and vt[0] is the WRITE
   (vt[0x98] 0x0826ef00, ATA 0xCA WRITE DMA). Do not swap them: the USB write path also reads
   through vt[4], before it merges, so the two calls look reversed there. */
/* DMA: the data cache is cleaned to RAM before a write and invalidated around a read, so the
   CPU does not keep stale lines. ARM926: 32-byte lines, cp15 c7. */
static void dcache_clean(const void *p, uint32_t n)
{
    uint32_t a = (uint32_t)p & ~31u, e = (uint32_t)p + n;
    for (; a < e; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c7, c10, 4" ::: "r0", "memory");   /* drain write buffer */
}
static void dcache_inval(const void *p, uint32_t n)
{
    uint32_t a = (uint32_t)p & ~31u, e = (uint32_t)p + n;
    for (; a < e; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a) : "memory");   /* clean+invalidate line */
    __asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c7, c10, 4" ::: "r0", "memory");
}
typedef int (*sync_xfer_t)(void *, void *, uint32_t, uint32_t, uint32_t);
static int xfer(Dev *d, int write, void *buf, uint32_t lba, uint32_t n)
{
    int rc;
    /* cache range sized for 4 KiB sectors; with 512-byte ones it only over-covers (clean+invalidate is harmless) */
    uint32_t bytes = n * 4096;
    if (write) dcache_clean(buf, bytes); else dcache_inval(buf, bytes);
    rc = ((sync_xfer_t)d->vt[(write ? 0 : 4) / 4])(d, buf, lba, n, 0);
    if (!write) dcache_inval(buf, bytes);
    return rc ? -3 : 0;
}
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void st32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static int fail(const char *what, int rc)
{
    log_s("   update: FAILED, "); log_s(what); log_s(" rc "); log_d(rc); log_c('\n'); log_flush();
    return rc;
}

/* called from libload() before anything else uses the disk */
void update_check(void)
{
    uint8_t hdr[0x810], *buf = 0, *buf2 = 0;
    int size, n, rc, i;
    uint32_t ss, nsect, part = 0, psize = 0, data_lba, dir_lba, dir_off, nchunks, sum = 0, body, k;
    Dev *d = 0;
    File *f = 0;
    void *m = 0;

    n = file_read(UPD_FILE, hdr, sizeof hdr, &size);
    if (n < (int)sizeof hdr) return;                                   /* no update file */
    log_s("== update: "); log_s(UPD_FILE); log_s(" found, "); log_d(size); log_s(" bytes\n");
    log_flush();
    /* one attempt only: a marker is written before the first try. If it is still there, the last
       try did not finish, so the update file is removed. */
    n = file_read(UPD_TRY, hdr, 4, &i);
    if (n >= 0) {
        log_s("   update: a previous attempt did not finish: removing the file, not retrying\n");
        if (vol_delete(UPD_FILE)) vol_rename(UPD_FILE, UPD_DONE);
        vol_delete(UPD_TRY);
        log_flush();
        return;
    }
    file_write(UPD_TRY, "try", 3);
    body = le32(hdr + 0x0c);
    if (hdr[0] != '8' || hdr[1] != '7' || hdr[2] != '0' || hdr[3] != '2' || size < 0x800 + 0x10000 ||
        body > (uint32_t)size - 0x800 || (le32(hdr + 0x800) & 0xff000000) != 0xea000000) {
        fail("not a decrypted IMG1 OS image", -10); return;
    }
    d = os_malloc(sizeof *d); buf = os_malloc(CHUNK); buf2 = os_malloc(CHUNK); m = os_malloc(0x114);
    if (!d || !buf || !buf2 || !m) { fail("memory", -11); return; }
    for (k = 0; k < CHUNK; k++) { buf[k] = 0; buf2[k] = 0; }
    dev_ctor(d, 0, 0);
    dev_open(d);
    ss = dev_ssize(d); nsect = dev_nsect(d);
    log_s("   update: disk "); log_d((int)nsect); log_s(" sectors of "); log_d((int)ss); log_c('\n');
    if (ss != 512 && ss != 4096) { fail("sector size", -12); goto out; }
    log_s("   update: device open, sync read/write\n"); log_flush();
    /* direction probe on the last sector, in the gap after the firmware partition: a read changes
       the buffer and returns the same data twice; a write leaves the pattern alone. */
    for (k = 0; k < ss; k++) buf[k] = 0xa5;
    if ((rc = xfer(d, 0, buf, nsect - 1, 1))) { fail("probe read", rc); goto out; }
    for (k = 0; k < ss && buf[k] == 0xa5; k++) ;
    if (k == ss) { fail("probe: the buffer kept its pattern, the call did not read: refusing", -19); goto out; }
    for (k = 0; k < ss; k++) buf2[k] = 0x5a;
    if ((rc = xfer(d, 0, buf2, nsect - 1, 1))) { fail("probe read 2", rc); goto out; }
    for (k = 0; k < ss && buf[k] == buf2[k]; k++) ;
    if (k != ss) { fail("probe: two reads of one sector differ: refusing", -20); goto out; }
    log_s("   update: direction probe passed\n"); log_flush();
    if ((rc = xfer(d, 0, buf, 0, 1))) { fail("MBR read", rc); goto out; }
    if (buf[510] != 0x55 || buf[511] != 0xaa) { fail("no MBR", -13); goto out; }
    for (i = 0; i < 4; i++) {
        const uint8_t *e = buf + 0x1be + 16 * i;
        uint32_t st = le32(e + 8), sz = le32(e + 12);
        log_s("   update: partition "); log_d(i); log_s(" type "); log_x(e[4], 2); log_s(" start "); log_d((int)st); log_s(" size "); log_d((int)sz); log_c('\n');
        if (sz == FW_SIZE / ss && (e[4] == 0x3f || e[4] == 0 || !part)) { part = st; psize = sz; }
    }
    if (!psize) { fail("firmware partition not found", -14); goto out; }
    /* read self-test before any write: ]ih[ must be at byte 0x100 of the firmware partition's first sector */
    if ((rc = xfer(d, 0, buf2, part, 1))) { fail("firmware header read", rc); goto out; }
    if (!(buf2[0x100] == ']' && buf2[0x101] == 'i' && buf2[0x102] == 'h' && buf2[0x103] == '[')) { fail("no ]ih[ header at the firmware partition: reads are not trustworthy", -18); goto out; }
    log_s("   update: read self-test passed (MBR valid, ]ih[ at sector "); log_d((int)part); log_s(")\n"); log_flush();
    data_lba = part + (FW_OSFL + 0x1000) / ss;
    dir_lba = part + FW_DIR / ss; dir_off = FW_DIR % ss;
    nchunks = ((uint32_t)size + CHUNK - 1) / CHUNK;
    if (data_lba + nchunks * (CHUNK / ss) > part + psize) { fail("image too big for the slot", -15); goto out; }
    /* already installed (same length and checksum as the osfl entry): the file is removed, nothing is written */
    if ((rc = xfer(d, 0, buf, dir_lba, 1))) { fail("directory read", rc); goto out; }
    for (i = 0; i < 12; i++) {
        uint8_t *e = buf + dir_off + 40 * i;
        if (e[4] == 'l' && e[5] == 'f' && e[6] == 's' && e[7] == 'o') break;
    }
    if (i == 12) { fail("no osfl entry in the directory", -17); goto out; }
    if (le32(buf + dir_off + 40 * i + 0x0c) != FW_OSFL) { fail("osfl entry offset differs", (int)le32(buf + dir_off + 40 * i + 0x0c)); goto out; }
    if (le32(buf + dir_off + 40 * i + 0x10) == (uint32_t)size) {
        uint32_t want_sum = le32(buf + dir_off + 40 * i + 0x1c), fsum = 0, left = (uint32_t)size, j;
        if (!(f = os_file_ctor(m, UPD_FILE, 1024, 1)) || f->vt->open(f)) { fail("open", -16); goto out; }
        while (left) {
            uint32_t want = left > CHUNK ? CHUNK : left;
            n = f->vt->read(f, buf2, (int)want, 2);
            if (n != (int)want) break;
            for (j = 0; j < want; j++) fsum += buf2[j];
            left -= want;
        }
        f->vt->close(f); f->vt->del(f); f = 0;
        if (!left && fsum == want_sum) {
            log_s("   update: this image is already installed (checksum "); log_x(fsum, 8); log_s("): removing the file\n");
            vol_delete(UPD_TRY);
            if (vol_delete(UPD_FILE)) vol_rename(UPD_FILE, UPD_DONE);
            log_flush();
            dev_close(d);
            return;
        }
    }
    log_s("   update: writing "); log_d((int)nchunks); log_s(" chunks at sector "); log_d((int)data_lba); log_c('\n'); log_flush();

    if (!(f = os_file_ctor(m, UPD_FILE, 1024, 1)) || f->vt->open(f)) { fail("open", -16); goto out; }
    for (k = 0; k < nchunks; k++) {
        uint32_t want = (uint32_t)size - k * CHUNK, j;
        if (want > CHUNK) want = CHUNK;
        for (j = 0; j < CHUNK; j++) buf[j] = 0;
        n = f->vt->read(f, buf, (int)want, 2);
        if (n != (int)want) { f->vt->close(f); fail("file read", n); goto out; }
        for (j = 0; j < want; j++) sum += buf[j];
        if ((rc = xfer(d, 1, buf, data_lba + k * (CHUNK / ss), CHUNK / ss))) { f->vt->close(f); fail("write", rc); goto out; }
        if ((rc = xfer(d, 0, buf2, data_lba + k * (CHUNK / ss), CHUNK / ss))) { f->vt->close(f); fail("read back", rc); goto out; }
        for (j = 0; j < CHUNK; j++) if (buf[j] != buf2[j]) { f->vt->close(f); fail("verify", (int)(k * CHUNK + j)); goto out; }
    }
    f->vt->close(f);
    /* the directory entry: length and checksum of the osfl section */
    if ((rc = xfer(d, 0, buf, dir_lba, 1))) { fail("directory read", rc); goto out; }
    for (i = 0; i < 12; i++) {
        uint8_t *e = buf + dir_off + 40 * i;
        if (e[4] == 'l' && e[5] == 'f' && e[6] == 's' && e[7] == 'o') break;
    }
    if (i == 12) { fail("no osfl entry in the directory", -17); goto out; }
    if (le32(buf + dir_off + 40 * i + 0x0c) != FW_OSFL) { fail("osfl entry offset differs", (int)le32(buf + dir_off + 40 * i + 0x0c)); goto out; }
    st32(buf + dir_off + 40 * i + 0x10, (uint32_t)size);
    st32(buf + dir_off + 40 * i + 0x1c, sum);
    if ((rc = xfer(d, 1, buf, dir_lba, 1))) { fail("directory write", rc); goto out; }
    if ((rc = xfer(d, 0, buf2, dir_lba, 1))) { fail("directory read back", rc); goto out; }
    for (k = 0; k < ss; k++) if (buf[k] != buf2[k]) { fail("directory verify", (int)k); goto out; }
    dev_close(d);
    d = 0;
    vol_delete(UPD_TRY);
    rc = vol_rename(UPD_FILE, UPD_DONE);
    if (rc) { log_s("   update: rename rc "); log_d(rc); log_s(", deleting the file instead\n"); rc = vol_delete(UPD_FILE); }
    log_s("   update: osfl written, "); log_d(size); log_s(" bytes, checksum "); log_x(sum, 8);
    if (rc) { log_s(", the file could NOT be removed (rc "); log_d(rc); log_s("): no restart, remove it by hand\n"); log_flush(); return; }
    log_s(", file renamed, restart\n");
    log_flush();
    os_sleep_ms(500);
    __asm__ volatile("msr CPSR_c, #0xd3");
    WDTCON = 0x100000;
    for (;;) ;
out:
    if (d) dev_close(d);
    if (f) f->vt->del(f);
    vol_delete(UPD_TRY);
    if (vol_delete(UPD_FILE)) vol_rename(UPD_FILE, UPD_DONE);        /* a failed update is not retried */
    log_s("   update: file removed after the failure\n"); log_flush();
}
