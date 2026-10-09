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
