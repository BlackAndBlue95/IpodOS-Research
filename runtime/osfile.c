/* File and volume calls into Apple's OS, used by every part of region E. */
#include "e.h"

/* creates a new file and writes n bytes; 0 ok, else the create rc or -1000..-1002 */
int file_write(const char *path, const void *data, uint32_t n)
{
    void *m = os_malloc(0x114);
    File *f;
    int rc;
    if (!m) return -1000;
    if (!(f = os_file_ctor(m, path, 1024, 1))) return -1001;
    rc = os_file_create(f);
    if (!rc) rc = os_file_write(f, data, &n) ? -1002 : 0;
    f->vt->del(f);
    return rc;
}

/* returns bytes read or a negative error, *size gets the file size */
int file_read(const char *path, void *buf, int max, int *size)
{
    void *m = os_malloc(0x114);
    File *f;
    int rc, got = -1;
    if (size) *size = -1;
    if (!m) return -1000;
    if (!(f = os_file_ctor(m, path, 1024, 1))) return -1001;
    rc = f->vt->open(f);
    if (!rc) {
        int a = f->vt->s6(f), b = f->vt->s7(f);
        if (size) *size = a > b ? a : b;
        got = max > 0 ? f->vt->read(f, buf, max, 2) : 0;
        f->vt->close(f);
    }
    f->vt->del(f);
    return rc ? (rc > 0 ? -rc : rc) - 2000 : got;
}

File *file_open(const char *path)
{
    void *m = os_malloc(0x114);
    File *f;
    if (!m) return 0;
    if (!(f = os_file_ctor(m, path, 1024, 1))) return 0;
    if (f->vt->open(f)) { f->vt->del(f); return 0; }
    return f;
}
int file_size(File *f) { int a = f->vt->s6(f), b = f->vt->s7(f); return a > b ? a : b; }

typedef int (*vslot1)(void *, struct os_str *);
typedef int (*vslot2)(void *, struct os_str *, struct os_str *);

static int vol_call(int slot, const char *path, const char *name2)
{
    uint32_t lk[8];
    struct os_str s, s2;
    void **vol;
    int rc;
    os_vol_lock(lk, 0);
    vol = os_vol_get(lk, 1);
    os_str_init(&s, path);
    if (name2) {
        os_str_init(&s2, name2);
        rc = ((vslot2)((void **)*vol)[slot / 4])(vol, &s, &s2);
        os_str_free(&s2);
    } else {
        rc = ((vslot1)((void **)*vol)[slot / 4])(vol, &s);
    }
    os_str_free(&s);
    os_vol_unlock(lk);
    return rc;
}
int vol_delete(const char *path) { return vol_call(VOL_DELETE, path, 0); }
int vol_rename(const char *path, const char *new_name) { return vol_call(VOL_RENAME, path, new_name); }

/* The OS's long-name conversion (0x082d95dc) breaks when a name has non-ASCII characters and
 * its entries straddle a sector: the second batch starts at dst + 13 * entries, one byte per
 * character, so extra UTF-8 bytes of the first batch are overwritten (".flac" can become
 * ".fla"). The find-record fill (0x082d9b14) and the open matcher (0x082d6314) both come here.
 * The converter 0x082ccb78(dst, cap, entries, n) walks n entries downwards and returns the end
 * of what it wrote, so the second batch continues there.
 * state: {entries, index in sector A, -, sector A, sector B}; a sector buffer has a 24-byte
 * header and 16 entries, the second batch starts at its last entry (+0x1f8). */
#define os_lfn_conv ((char *(*)(char *, int, const uint8_t *, int))0x082ccb78)
#define os_sec_get  ((uint8_t *(*)(void *, uint32_t))0x082d90c0)
#define os_sec_put  ((void (*)(uint8_t *, int))0x082d69e4)
int names_rebuilt;      /* names that spanned two sectors, for the log */
E_ENTRY char *e_lfn_batch(void *ctx, const uint32_t *st, char *dst)
{
    uint32_t n = st[0], idx = st[1], n1;
    uint8_t *sec;
    char *p;
    dst[0] = 0;
    if (!n || !(sec = os_sec_get(ctx, st[3]))) return dst;
    n1 = n > idx + 1 ? idx + 1 : n;
    p = os_lfn_conv(dst, 256, sec + 24 + (idx << 5), (int)n1);
    os_sec_put(sec, 0);
    if (n1 < n && st[4] && (sec = os_sec_get(ctx, st[4]))) {
        os_lfn_conv(p, 256 - (int)(p - dst), sec + 0x1f8, (int)(n - n1));
        os_sec_put(sec, 0);
        names_rebuilt++;
    }
    return dst;
}

/* folder listing through the OS's FAT find calls. If cb returns nonzero the listing stops and
 * returns -3: a partial list would look like deleted files to the sync. */
int dir_list(const char *dir, int (*cb)(void *ctx, const char *name, int is_dir, uint32_t size,
                                        uint16_t date, uint16_t time), void *ctx)
{
    uint32_t lk[8];
    struct os_str s;
    void **vol;
    uint8_t *rec;
    char pat[300];
    int i = 0, n = 0, ok, stopped = 0;
    while (dir[i] && i < 290) { pat[i] = dir[i]; i++; }
    pat[i++] = '\\'; pat[i++] = '*'; pat[i++] = '.'; pat[i++] = '*'; pat[i] = 0;
    if (!(rec = os_malloc(FIND_SIZE))) return -1;
    for (i = 0; i < FIND_SIZE; i++) rec[i] = 0;
    os_vol_lock(lk, 0);
    vol = os_vol_get(lk, 1);
    os_str_init(&s, pat);
    os_vol_fatpath(vol, &s);
    ok = os_findfirst(rec, os_str_c(&s));
    if (ok) {
        while (ok) {
            const char *nm = (const char *)rec + FIND_NAME;
            uint8_t attr = rec[FIND_ATTR];
            if (!(attr & 0x08) && !(nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2])))) {
                n++;
                if (cb(ctx, nm, (attr & 0x10) != 0, *(uint32_t *)(rec + FIND_FSIZE),
                       *(uint16_t *)(rec + FIND_DATE), *(uint16_t *)(rec + FIND_TIME))) {
                    stopped = 1;
                    break;
                }
            }
            ok = os_findnext(rec);
        }
        os_findclose(rec);
    } else {
        n = -2;
    }
    os_str_free(&s);
    os_vol_unlock(lk);
    os_free(rec);
    return stopped ? -3 : n;
}

/* Rockbox's dostime_mktime(): FAT local date and time as if UTC, seconds since 1970 */
uint32_t dos_mktime(uint16_t date, uint16_t time)
{
    static const uint16_t cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    uint32_t y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    uint32_t days;
    if (m < 1) m = 1;
    if (m > 12) m = 12;
    days = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400
         + cum[m - 1] + (d ? d - 1 : 0);
    if (m > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
    return days * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
}

/* FAT free-cluster count 0x082d75c8(drive). Apple's driver starts each mount with its cached
 * count (fs+0x1f4) at 0 and fills it by reading the whole FAT: 62 MB and ~4.5 s on a 256 GB card.
 * It never reads FSInfo, but it writes the cached count back into FSInfo (0x082d6618), so the
 * count on disk stays right only if the cache was filled. Here an empty cache is seeded from
 * FSInfo when FSInfo is valid for the same volume; otherwise the scan runs once and writes a
 * good FSInfo for the next mount. */
#define os_fat_fs ((uint8_t *(*)(int))0x082d581c)
static uint8_t *music_fs;
/* the FAT driver's allocation state: search hints, cached free count, cluster count */
void fat_state_log(const char *when)
{
    uint8_t *fs = music_fs;
    if (!fs) return;
    log_s("   fat state "); log_s(when); log_s(": hints "); log_d((int)*(uint32_t *)(fs + 0x1ec)); log_c(' '); log_d((int)*(uint32_t *)(fs + 0x1f0));
    log_s(", free "); log_d((int)*(uint32_t *)(fs + 0x1f4)); log_s(", clusters "); log_d((int)*(uint32_t *)(fs + 0x7c)); log_c('\n');
}
/* seed the driver's cached free count from FSInfo when it is still empty (0 at mount) */
static void fat_seed(uint8_t *fs, const char *who)
{
    static int logged;
    extern uint32_t fatdir_fsinfo_next;
    uint32_t t, clus, nclus = 0;
    int rc, d;
    if (!fs || *(uint32_t *)(fs + 0x1f4) || *(uint16_t *)(fs + 0x6c) != 8) return;   /* cached already, or not FAT32 */
    t = TIMER_E;
    rc = fatdir_begin();
    clus = rc == 0 ? fatdir_free_clusters(&nclus) : 0;
    fatdir_end();
    d = (int)(*(uint32_t *)(fs + 0x7c) - 1 - nclus);   /* the driver counts one more; same volume within rounding */
    if (clus && d >= -2 && d <= 2) { *(uint32_t *)(fs + 0x1f4) = clus; music_fs = fs; }
    if (logged++) return;
    if (*(uint32_t *)(fs + 0x1f4)) { log_s("   free space: "); log_d((int)clus); log_s(" clusters, next free "); log_d((int)fatdir_fsinfo_next); log_s(" from FSInfo, at the "); log_s(who); log_s(" ("); log_d((int)(TIMER_E - t)); log_s(" us)\n"); }
    else { log_s("   free space: FSInfo not used at the "); log_s(who); log_s(" (begin rc "); log_d(rc); log_s(", reason "); log_d(fatdir_free_why); log_s(", clusters "); log_d((int)nclus); log_s(" vs "); log_d((int)*(uint32_t *)(fs + 0x7c)); log_s("), the driver scans the FAT once\n"); }
}
static void __attribute__((used)) fatfree_c(uint32_t *r)
{
    uint8_t *fs = os_fat_fs((int)r[0]);
    if (fs && *(uint16_t *)(fs + 0x6c) == 8) music_fs = fs;
    fat_seed(fs, "free-space query");
}
/* FSInfo write 0x082d6618(fs): the driver writes its cached count back; seeded first, so a write
   before any free-space query (a file write at boot, the self-update) cannot store 0 */
static void __attribute__((used)) fatinfo_c(uint32_t *r) { fat_seed((uint8_t *)r[0], "FSInfo write"); }
__attribute__((naked, section(".text.entry"), used)) void hk_fatinfo(void)
{
    __asm__ volatile(
        "push {r0-r5, ip, lr}\n mov r0, sp\n ldr ip, 1f\n mov lr, pc\n bx ip\n"
        "pop {r0-r5, ip, lr}\n"
        ".word 0xe92d40f8\n ldr pc, 2f\n"
        "1: .word fatinfo_c\n 2: .word 0x082d661c\n");
}
__attribute__((naked, section(".text.entry"), used)) void hk_fatfree(void)
{
    __asm__ volatile(
        "push {r0-r5, ip, lr}\n mov r0, sp\n ldr ip, 1f\n mov lr, pc\n bx ip\n"
        "pop {r0-r5, ip, lr}\n"
        ".word 0xe92d40f8\n ldr pc, 2f\n"
        "1: .word fatfree_c\n 2: .word 0x082d75cc\n");
}

/* Free-cluster search 0x082d61c0(fs, start, end): the allocator's linear scan, one FAT entry per
 * call through the driver's sector cache. When its start hint sits before a long run of used
 * clusters it reads thousands of sectors (4.2 s for one log write at boot). Long ranges are
 * scanned here from the raw FAT in 64 KB reads; every candidate is confirmed through the driver
 * (0x082d5dd4), so a cluster allocated in its cache but not yet on disk is never handed out. */
#define os_fat_get ((int (*)(void *, uint32_t, uint32_t *))0x082d5dd4)
static int fat_confirm(void *fs, uint32_t c) { uint32_t v = 1; return os_fat_get(fs, c, &v) && v == 0; }
static int __attribute__((used)) fatsearch_c(uint32_t *r)
{
    static int logged;
    uint8_t *fs = (uint8_t *)r[0];
    uint32_t start = r[1], end = r[2], c, t;
    int reads = 0, d;
    if (end <= start || end - start < 1024 || fatdir_begin()) { fatdir_end(); return 0; }
    d = (int)(*(uint32_t *)(fs + 0x7c) - 1 - fatdir_nclus());
    if (d < -2 || d > 2) { fatdir_end(); return 0; }
    t = TIMER_E;
    c = fatdir_next_free(start, end, fat_confirm, fs, &reads);
    fatdir_end();
    if (logged < 6 || TIMER_E - t > 50000) {
        logged++;
        log_s("   fat search: "); log_d((int)start); log_c('-'); log_d((int)end); log_s(" -> ");
        log_d(c == 0xffffffffu ? -1 : (int)c); log_s(" ("); log_d(reads); log_s(" reads, "); log_d((int)(TIMER_E - t)); log_s(" us)\n");
    }
    if (c == 0xffffffffu) return 0;
    r[0] = c;
    return 1;
}
__attribute__((naked, section(".text.entry"), used)) void hk_fatsearch(void)
{
    __asm__ volatile(
        "push {r0-r5, ip, lr}\n mov r0, sp\n ldr ip, 1f\n mov lr, pc\n bx ip\n"
        "cmp r0, #0\n pop {r0-r5, ip, lr}\n bxne lr\n"
        ".word 0xe92d40f8\n ldr pc, 2f\n"
        "1: .word fatsearch_c\n 2: .word 0x082d61c4\n");
}

/* Free-run search 0x082d6080(fs, start, &run, want, mode): the file-extend path's search for a
 * run of free clusters, one FAT entry per call from start to the last cluster. The start comes
 * from FSInfo's next-free hint, which the Mac sets low (12 after copying a file), so a log write
 * read 1.4 million entries. Here the start moves to the first cluster that is free on disk:
 * every skipped cluster is in use on disk, so no run the search could return starts there (one
 * freed in the driver's cache but not yet written is merely not chosen). */
static int accept_all(void *ctx, uint32_t c) { (void)ctx; (void)c; return 1; }
static void __attribute__((used)) fatrun_c(uint32_t *r)
{
    static int logged;
    uint8_t *fs = (uint8_t *)r[0];
    uint32_t start = r[1], end, f, t;
    int reads = 0, d;
    if (!fs) return;
    end = *(uint32_t *)(fs + 0x7c);
    if (start >= end || end - start < 1024 || fatdir_begin()) { fatdir_end(); return; }
    d = (int)(end - 1 - fatdir_nclus());
    if (d < -2 || d > 2) { fatdir_end(); return; }
    t = TIMER_E;
    f = fatdir_next_free(start, end + 1, accept_all, 0, &reads);
    fatdir_end();
    if (f == 0xffffffffu) return;
    if (f == 0) f = end;
    if (f > start) r[1] = f;
    if (logged < 6 || TIMER_E - t > 50000) {
        logged++;
        log_s("   fat run search: start "); log_d((int)start); log_s(" -> "); log_d((int)f); log_s(" ("); log_d(reads); log_s(" reads, ");
        log_d((int)(TIMER_E - t)); log_s(" us)\n");
    }
}
__attribute__((naked, section(".text.entry"), used)) void hk_fatrun(void)
{
    __asm__ volatile(
        "push {r0-r5, ip, lr}\n mov r0, sp\n ldr ip, 1f\n mov lr, pc\n bx ip\n"
        "pop {r0-r5, ip, lr}\n"
        ".word 0xe92d4fff\n ldr pc, 2f\n"
        "1: .word fatrun_c\n 2: .word 0x082d6084\n");
}
