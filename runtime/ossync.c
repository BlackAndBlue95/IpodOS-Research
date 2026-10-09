/* Library sync for the OS: libsync (shared with the Rockbox plugin and the host build) on the
 * OS's File class, FAT listing, delete and rename. It runs before each library load, so the
 * iTunesDB matches the .flac files under /Music. libsync uses "/Music/..." paths; the OS wants
 * "Music\...", translated here. */
#include "e.h"

/* ---- freestanding bits libsync needs ---- */
int e_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}
size_t e_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

/* %s %d %u %x %c %% only, enough for libsync's paths and messages */
int e_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    __builtin_va_list ap;
    size_t n = 0;
    char tmp[12];
    __builtin_va_start(ap, fmt);
#define PUT(c) do { if (n + 1 < size) buf[n] = (c); n++; } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        if (*fmt == 's') { const char *s = __builtin_va_arg(ap, const char *); if (!s) s = "(null)"; while (*s) PUT(*s++); }
        else if (*fmt == 'd' || *fmt == 'u' || *fmt == 'x') {
            int v = __builtin_va_arg(ap, int), i = 0;
            uint32_t u = *fmt == 'd' && v < 0 ? -(uint32_t)v : (uint32_t)v, base = *fmt == 'x' ? 16 : 10;
            if (*fmt == 'd' && v < 0) PUT('-');
            do { tmp[i++] = "0123456789abcdef"[u % base]; u /= base; } while (u);
            while (i) PUT(tmp[--i]);
        }
        else if (*fmt == 'c') PUT((char)__builtin_va_arg(ap, int));
        else if (*fmt == '%') PUT('%');
        else { PUT('%'); if (*fmt) PUT(*fmt); }
        if (!*fmt) break;
    }
#undef PUT
    if (size) buf[n < size ? n : size - 1] = 0;
    __builtin_va_end(ap);
    return (int)n;
}

#define LS_MEMCPY   memcpy
#define LS_MEMSET   memset
#define LS_MEMCMP   e_memcmp
#define LS_STRLEN   e_strlen
#define LS_SNPRINTF e_snprintf
#include "libsync.c"

/* ---- paths ---- */
/* "/Music/a/b.flac" -> "Music\a\b.flac". Returns 0 if it does not fit: never truncated, since
   a cut path could name another file. */
static int ospath(char *d, const char *s, int cap)
{
    int i = 0;
    while (*s == '/') s++;
    while (*s && i < cap - 1) { d[i++] = *s == '/' ? '\\' : *s; s++; }
    d[i] = 0;
    return *s ? 0 : 1;
}

/* ---- files: a small table of OS File objects ---- */
#define NFD 8
static File *fds[NFD];
static uint8_t fd_rd[NFD];

static int fd_new(const char *path, int write)
{
    char p[300];
    void *m;
    File *f;
    int i, rc;
    for (i = 0; i < NFD && fds[i]; i++);
    if (i == NFD) return -1;
    if (!ospath(p, path, sizeof p)) return -1;
    if (!(m = os_malloc(0x114)) || !(f = os_file_ctor(m, p, 1024, 1))) { log_s("   sync: file object failed for "); log_s(p); log_c('\n'); return -1; }
    rc = write ? os_file_create(f) : f->vt->open(f);
    if (rc) {
        static int logged;
        if (logged < 4) { logged++; log_s("   sync: "); log_s(write ? "create" : "open"); log_s(" failed rc "); log_d(rc); log_c(' '); log_s(p); log_c('\n'); }
        f->vt->del(f); return -1;
    }
    fds[i] = f; fd_rd[i] = !write;
    return i;
}
int ls_open_r(const char *path) { return fd_new(path, 0); }
int ls_open_w(const char *path) { return fd_new(path, 1); }
long ls_read(int fd, void *buf, long n)
{
    long got = 0;
    if (fd < 0 || fd >= NFD || !fds[fd]) return -1;
    while (got < n) {
        int k = fds[fd]->vt->read(fds[fd], (char *)buf + got, (int)(n - got), 2);
        if (k <= 0) break;
        got += k;
    }
    return got;
}
long ls_write(int fd, const void *buf, long n)
{
    uint32_t len = (uint32_t)n;
    if (fd < 0 || fd >= NFD || !fds[fd]) return -1;
    return os_file_write(fds[fd], buf, &len) ? -1 : n;
}
long ls_seek(int fd, long off)
{
    if (fd < 0 || fd >= NFD || !fds[fd]) return -1;
    return fds[fd]->vt->seek(fds[fd], (int64_t)off, 0) ? -1 : off;
}
long ls_fsize(int fd)
{
    int a, b;
    if (fd < 0 || fd >= NFD || !fds[fd]) return -1;
    a = fds[fd]->vt->s6(fds[fd]); b = fds[fd]->vt->s7(fds[fd]);
    return a > b ? a : b;
}
int ls_close(int fd)
{
    if (fd < 0 || fd >= NFD || !fds[fd]) return -1;
    if (fd_rd[fd]) fds[fd]->vt->close(fds[fd]);
    fds[fd]->vt->del(fds[fd]);
    fds[fd] = 0;
    return 0;
}
int ls_remove(const char *path) { char p[300]; if (!ospath(p, path, sizeof p)) return -1; return vol_delete(p) ? -1 : 0; }
int ls_rename(const char *from, const char *to)
{
    /* the OS renames within a folder, so only the new name is passed */
    char p[300];
    const char *nm = to, *s;
    for (s = to; *s; s++) if (*s == '/' || *s == '\\') nm = s + 1;
    if (!ospath(p, from, sizeof p)) return -1;
    return vol_rename(p, nm) ? -1 : 0;
}

/* ---- folders: listed whole, then iterated (the OS's FAT find holds the volume lock) ---- */
struct dent { char *name; uint32_t size, mtime; uint8_t dir; };
struct edir { struct dent *e; int n, cap, i, err; };
/* Listings come from the volume (fatdir.c) when the block device opens. Mode 2 (no FLAC\fatdir.ok
   yet) also lists through the OS and compares; a clean compare writes FLAC\fatdir.ok, which
   selects mode 1, no comparison. A failed raw listing falls back to the OS's, so the scan is
   correct but slower. Mode 0: block device unavailable, OS listing only. */
static int fatdir_mode;
static int fatdir_n, fatdir_fallback, fatdir_checked, fatdir_mismatch;
static uint32_t fatdir_us, osdir_us;
static int dent_cmp(const void *a, const void *b)
{
    const uint8_t *x = (const uint8_t *)((const struct dent *)a)->name, *y = (const uint8_t *)((const struct dent *)b)->name;
    while (*x && *x == *y) { x++; y++; }
    return (int)*x - (int)*y;
}
static char why_buf[200];
static int edir_same(struct edir *a, struct edir *b, const char **why)
{
    int i;
    *why = why_buf;
    if (a->n != b->n) { e_snprintf(why_buf, sizeof why_buf, "entry count raw %d os %d", a->n, b->n); return 0; }
    ls_qsort(a->e, a->n, sizeof *a->e, dent_cmp);
    ls_qsort(b->e, b->n, sizeof *b->e, dent_cmp);
    for (i = 0; i < a->n; i++) {
        if (e_strlen(a->e[i].name) != e_strlen(b->e[i].name) || e_memcmp(a->e[i].name, b->e[i].name, e_strlen(a->e[i].name))) {
            e_snprintf(why_buf, sizeof why_buf, "name raw '%s' os '%s'", a->e[i].name, b->e[i].name); return 0;
        }
        if (a->e[i].dir != b->e[i].dir) { e_snprintf(why_buf, sizeof why_buf, "folder flag of '%s'", a->e[i].name); return 0; }
        if (!a->e[i].dir && (a->e[i].size != b->e[i].size || a->e[i].mtime != b->e[i].mtime)) {
            e_snprintf(why_buf, sizeof why_buf, "'%s' raw %u/%u os %u/%u", a->e[i].name, a->e[i].size, a->e[i].mtime, b->e[i].size, b->e[i].mtime); return 0;
        }
    }
    return 1;
}
static int collect_cb(void *ctx, const char *name, int is_dir, uint32_t size, uint16_t date, uint16_t time)
{
    struct edir *d = ctx;
    size_t len = e_strlen(name);
    char *nm;
    if (d->n == d->cap) {
        struct dent *ne = os_malloc(sizeof *ne * (d->cap * 2 + 64));
        if (!ne) { d->err = 1; return 1; }
        if (d->e) { memcpy(ne, d->e, sizeof *ne * d->n); os_free(d->e); }
        d->e = ne; d->cap = d->cap * 2 + 64;
    }
    if (!(nm = os_malloc(len + 1))) { d->err = 1; return 1; }
    memcpy(nm, name, len + 1);
    d->e[d->n].name = nm; d->e[d->n].size = size; d->e[d->n].mtime = dos_mktime(date, time); d->e[d->n].dir = is_dir != 0;
    d->n++;
    return 0;
}
void *ls_opendir(const char *path)
{
    char p[300];
    struct edir *d = os_malloc(sizeof *d);
    if (!d) return 0;
    memset(d, 0, sizeof *d);
    if (!ospath(p, path, sizeof p)) { os_free(d); return 0; }
    if (fatdir_mode) {
        uint32_t t0 = TIMER_E;
        int rc = fatdir_list(p, collect_cb, d);
        fatdir_us += TIMER_E - t0;
        if (rc >= 0 && !d->err) {
            fatdir_n++;
            if (fatdir_mode == 2) {
                struct edir *o = os_malloc(sizeof *o);
                const char *why = "memory";
                if (o) {
                    memset(o, 0, sizeof *o);
                    t0 = TIMER_E;
                    rc = dir_list(p, collect_cb, o);
                    osdir_us += TIMER_E - t0;
                    if (rc >= 0 && !o->err) {
                        fatdir_checked++;
                        if (edir_same(d, o, &why)) { ls_closedir(o); d->i = 0; return d; }
                        fatdir_mismatch++;
                        if (fatdir_mismatch <= 5) { log_s("   fatdir: MISMATCH "); log_name(p); log_s(": "); log_name(why); log_c('\n'); }
                        ls_closedir(d);
                        o->i = 0;
                        return o;                      /* the OS's view wins */
                    }
                    ls_closedir(o);
                }
            }
            return d;
        }
        /* the raw listing could not be trusted: through the OS instead */
        fatdir_fallback++;
        { int i; for (i = 0; i < d->n; i++) os_free(d->e[i].name); if (d->e) os_free(d->e); memset(d, 0, sizeof *d); }
    }
    if (dir_list(p, collect_cb, d) < 0 || d->err) { ls_closedir(d); return 0; }   /* a partial list is no list */
    return d;
}
int ls_readdir(void *v, struct ls_dirent *e)
{
    struct edir *d = v;
    if (d->i >= d->n) return 0;
    e->name = d->e[d->i].name; e->is_dir = d->e[d->i].dir; e->size = d->e[d->i].size; e->mtime = d->e[d->i].mtime;
    d->i++;
    return 1;
}
void ls_closedir(void *v)
{
    struct edir *d = v;
    int i;
    for (i = 0; i < d->n; i++) os_free(d->e[i].name);
    if (d->e) os_free(d->e);
    os_free(d);
}

/* ---- heapsort, no recursion, no allocation ---- */
void ls_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    uint8_t *b = base, tmp[64];
    size_t i, start, end, root, child;
    if (n < 2 || size > sizeof tmp) return;
#define SWAP(x, y) do { memcpy(tmp, b + (x) * size, size); memcpy(b + (x) * size, b + (y) * size, size); memcpy(b + (y) * size, tmp, size); } while (0)
    for (start = n / 2; start > 0; ) {
        start--;
        for (root = start; ; ) {
            child = 2 * root + 1;
            if (child >= n) break;
            if (child + 1 < n && cmp(b + child * size, b + (child + 1) * size) < 0) child++;
            if (cmp(b + root * size, b + child * size) >= 0) break;
            SWAP(root, child); root = child;
        }
    }
    for (end = n - 1; end > 0; end--) {
        SWAP(0, end);
        for (root = 0; ; ) {
            child = 2 * root + 1;
            if (child >= end) break;
            if (child + 1 < end && cmp(b + child * size, b + (child + 1) * size) < 0) child++;
            if (cmp(b + root * size, b + child * size) >= 0) break;
            SWAP(root, child); root = child;
        }
    }
#undef SWAP
    (void)i;
}

void ls_status(const char *msg)
{
    boost_hold(10000);                 /* full speed while the sync works */
    log_s("   sync: "); log_s(msg); log_c('\n');
}

/* ---- time ---- */
/* the iPod's local date and time as seconds since 1970, computed as Rockbox does */
#define os_get_datetime ((int (*)(uint8_t *))0x08083b08)
static uint32_t now_unix(void)
{
    uint8_t dt[16];
    int y, m, d;
    long days;
    os_get_datetime(dt);
    y = dt[6] | dt[7] << 8; m = dt[4]; d = dt[3];
    if (m <= 2) { y--; m += 12; }
    days = 365L * y + y / 4 - y / 100 + y / 400 + (153L * (m - 3) + 2) / 5 + d - 719469;
    return (uint32_t)(days * 86400L + dt[2] * 3600L + dt[1] * 60L + dt[0]);
}

/* ---- album folders for the art database (dir "/Music/x" -> "Music\x") ---- */
/* The art source is picked here from the folder's listing, with artdb.c's rule: a cover file
   by name, else the first .flac (its embedded picture). */
static struct sync_album *albums; static int nalbums, capalbums, albums_bad;
static const char *const cover_names[5] = { "cover.jpg", "folder.jpg", "front.jpg", "cover.jpeg", "albumart.jpg" };
static int streq_ci(const char *a, const char *b)
{
    while (*a && *b) { char x = *a++, y = *b++; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; }
    return !*a && !*b;
}
static void album_cb(void *ctx, const char *dir, const char *first, const struct ls_dirent *e, int n)
{
    size_t dl = e_strlen(dir), fl = e_strlen(first);
    char *d, *f;
    struct sync_album *a;
    int i, k, pick = -1;
    (void)ctx;
    if (albums_bad) return;
    if (nalbums == capalbums) {
        struct sync_album *na = os_malloc(sizeof *na * (capalbums * 2 + 64));
        if (!na) { albums_bad = 1; return; }
        if (albums) { memcpy(na, albums, sizeof *na * nalbums); os_free(albums); }
        albums = na; capalbums = capalbums * 2 + 64;
    }
    if (!(d = os_malloc(dl + 1)) || !(f = os_malloc(fl + 1))) { if (d) os_free(d); albums_bad = 1; return; }
    ospath(d, dir, (int)dl + 1); ospath(f, first, (int)fl + 1);
    a = &albums[nalbums];
    a->dir = d; a->first = f; a->kind = 0; a->src[0] = 0; a->srcsize = a->srcmtime = 0;
    for (k = 0; k < 5 && pick < 0; k++)
        for (i = 0; i < n; i++) if (!e[i].is_dir && streq_ci(e[i].name, cover_names[k])) { pick = i; a->kind = (uint8_t)(k + 1); break; }
    if (pick < 0) {
        const char *fn = first + dl + 1;                 /* the first .flac's own name */
        for (i = 0; i < n; i++) if (!e[i].is_dir && streq_ci(e[i].name, fn)) { pick = i; a->kind = 6; break; }
    }
    if (pick >= 0) {
        for (i = 0; e[pick].name[i] && i < (int)sizeof a->src - 1; i++) a->src[i] = e[pick].name[i];
        a->src[i] = 0;
        a->srcsize = e[pick].size; a->srcmtime = e[pick].mtime;
    }
    nalbums++;
}
static void albums_free(void)
{
    int i;
    for (i = 0; i < nalbums; i++) { os_free(albums[i].dir); os_free(albums[i].first); }
    if (albums) os_free(albums);
    albums = 0; nalbums = capalbums = albums_bad = 0;
}
/* the list for artdb_update(): NULL with *n = -1 when the scan did not complete */
const struct sync_album *sync_albums(int *n) { *n = albums_bad ? -1 : nalbums; return albums_bad ? 0 : albums; }

/* the last result, for the Settings pane and the health line */
struct ls_result sync_last;
int sync_rc = 1;                                   /* 1: not run yet */
int sync_no_db;                                    /* the database could not be opened at all */
char sync_summary[64] = "Not synced yet";
static void summarize(int rc, const struct ls_result *r)
{
    int n = 0;
    const char *p;
#define ADD(s) for (p = (s); *p && n < 60; p++) sync_summary[n++] = *p
#define NUM(v) do { char t[12]; int k = 0, v2 = (v); if (!v2) t[k++] = '0'; while (v2) { t[k++] = '0' + v2 % 10; v2 /= 10; } while (k && n < 60) sync_summary[n++] = t[--k]; } while (0)
    if (rc < 0) { ADD("Failed: "); ADD(r->msg); }
    else if (!r->wrote) { NUM(r->tracks); ADD(" tracks, up to date"); }
    else {
        if (r->added) { ADD("+"); NUM(r->added); ADD(" new "); }
        if (r->removed) { ADD("-"); NUM(r->removed); ADD(" gone "); }
        if (r->updated) { NUM(r->updated); ADD(" changed "); }
        if (r->renamed) { NUM(r->renamed); ADD(" renamed "); }
        if (r->migrated) { NUM(r->migrated); ADD(" migrated "); }
        if (r->reordered && !n) { ADD("reordered "); }
        if (!n) { ADD("playlists updated "); }
        sync_summary[n - 1] = 0; n--;
    }
#undef ADD
#undef NUM
    sync_summary[n] = 0;
}

/* ---- the sync, before a library load ---- */
void sync_library(int task)
{
    static struct ls_opts o;
    static struct ls_result r;
    static const unsigned sizes[3] = { 8u << 20, 6u << 20, 4u << 20 };
    size_t memsz = 0;
    void *mem = 0;
    const uint8_t *si = os_sysinfo();
    uint32_t t0 = TIMER_E, seed;
    int i, rc;
    for (i = 0; i < 3 && !mem; i++) { memsz = sizes[i]; mem = os_malloc(memsz); }
    albums_free();
    if (!mem) {
        log_s("   sync: not done: no memory\n");
        memset(&r, 0, sizeof r); e_snprintf(r.msg, sizeof r.msg, "no memory");
        sync_rc = -1; sync_last = r; summarize(-1, &r);
        return;
    }
    memset(&o, 0, sizeof o);
    o.root = "";
    o.album_cb = album_cb;
    for (i = 0; i < 8; i++) o.fwid[i] = si[0x38 + 7 - i];     /* SysInfo keeps the FireWire GUID (+0x38) reversed */
    o.now = now_unix();
    seed = t0 ^ (uint32_t)o.now;
    for (i = 0; i < 8; i++) o.seed[i] = (uint8_t)(seed >> (8 * (i & 3))) ^ o.fwid[i] ^ (uint8_t)(i * 0x5b);
    boost_hold(10000);
    fatdir_n = fatdir_fallback = fatdir_checked = fatdir_mismatch = 0; fatdir_us = osdir_us = 0;
    fatdir_mode = fatdir_begin() == 0 ? (file_read("FLAC\\fatdir.ok", 0, 0, &i) >= 0 ? 1 : 2) : 0;
    rc = ls_sync(&o, mem, memsz, &r);
    if (rc < 0 && r.badsig) {
        for (i = 0; i < 8; i++) o.fwid[i] = si[0x38 + i];
        albums_free();
        rc = ls_sync(&o, mem, memsz, &r);
    }
    fatdir_end();
    log_s("   fatdir: "); log_s(fatdir_mode ? "" : "block device not available, ");
    log_d(fatdir_n); log_s(" folders raw ("); log_d((int)fatdir_us); log_s(" us, "); log_d(fatdir_reads); log_s(" reads), ");
    log_d(fatdir_fallback); log_s(" through the OS");
    if (fatdir_mode == 2) {
        log_s("; checked "); log_d(fatdir_checked); log_s(" against the OS ("); log_d((int)osdir_us); log_s(" us), "); log_d(fatdir_mismatch); log_s(" mismatches");
        if (fatdir_checked && !fatdir_mismatch && !fatdir_fallback) { file_write("FLAC\\fatdir.ok", "ok", 2); log_s(", raw listings from now on"); }
    }
    log_c('\n');
    fatdir_mode = 0;
    os_free(mem);
    sync_rc = rc; sync_last = r; summarize(rc, &r);
    sync_no_db = rc < 0 && r.msg[0] == 'n' && r.msg[1] == 'o' && r.msg[2] == ' ' && r.msg[3] == 'i';
    log_s("   sync: ");
    if (rc < 0) { log_s("not done: "); log_s(r.msg); }
    else if (!r.wrote) { log_d(r.flacs); log_s(" FLACs, up to date"); }
    else {
        log_s("+"); log_d(r.added); log_s(" new, "); log_d(r.updated); log_s(" changed, -"); log_d(r.removed);
        log_s(" gone, "); log_d(r.renamed); log_s(" renamed, "); log_d(r.migrated); log_s(" migrated, ");
        log_d(r.tracks); log_s(" tracks, verify ok"); if (r.reordered) log_s(", list put in id order"); if (r.playcounts) log_s(", play counts merged");
    }
    if (r.skipped) { log_s(", "); log_d(r.skipped); log_s(" unreadable"); }
    if (r.noalbum) { log_s(", "); log_d(r.noalbum); log_s(" without album tag"); }
    { extern int names_rebuilt; log_s(", "); log_d(names_rebuilt); log_s(" long names rebuilt"); names_rebuilt = 0; }
    log_s(", "); log_d(nalbums); log_s(" album folders, "); log_d((int)(memsz >> 20)); log_s(" MB");
    log_s(" ("); log_d((int)(TIMER_E - t0)); log_s(" ticks)\n");
}
