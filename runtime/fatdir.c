/* Folder listings read straight from the FAT32 volume: one device read per cluster, where the
 * OS's find calls take one transaction per sector and the FAT mutex per entry. Read-only, and
 * only used while nothing writes directories. Anything unexpected returns -1 and the caller
 * lists through the OS instead. Host build (-DFATDIR_HOST): the device is an image file. */
#include <stdint.h>
#include <stddef.h>

#ifdef FATDIR_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static FILE *h_img;
static uint32_t h_ss = 4096;
static void *fd_malloc(size_t n) { return malloc(n); }
static void fd_free(void *p) { free(p); }
static int dev_read(uint32_t lba, void *buf, uint32_t n)
{
    if (fseeko(h_img, (off_t)lba * h_ss, SEEK_SET)) return -1;
    return fread(buf, 1, (size_t)n * h_ss, h_img) == (size_t)n * h_ss ? 0 : -1;
}
int fatdir_host_open(const char *image, uint32_t ss) { h_img = fopen(image, "rb"); h_ss = ss; return h_img ? 0 : -1; }
#else
#include "e.h"
#include "os.h"
#define fd_malloc os_malloc
#define fd_free   os_free
/* block device class (as in update.c): ctor(obj, 0, 0) covers the whole disk. vt[4] is the
   synchronous READ (self, buf, lba, count, 0). DMA, so the cache lines are invalidated around it. */
typedef struct { void **vt; uint32_t f[8]; } Dev;
#define dev_ctor   ((void *(*)(void *, int, int))0x08270400)
#define dev_open(d)   (((int (*)(void *))(d)->vt[0x18 / 4])(d))
#define dev_close(d)  (((int (*)(void *))(d)->vt[0x1c / 4])(d))
#define dev_ssize(d)  (((uint32_t (*)(void *))(d)->vt[0x20 / 4])(d))
static Dev *dev;
static uint32_t dev_ss;
static void dcache_inval(const void *p, uint32_t n)
{
    uint32_t a = (uint32_t)p & ~31u, e = (uint32_t)p + n;
    for (; a < e; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a) : "memory");
    __asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c7, c10, 4" ::: "r0", "memory");
}
static int dev_read(uint32_t lba, void *buf, uint32_t n)
{
    int rc;
    if (!dev) return -1;
    dcache_inval(buf, n * dev_ss);
    rc = ((int (*)(void *, void *, uint32_t, uint32_t, uint32_t))dev->vt[4 / 4])(dev, buf, lba, n, 0);
    dcache_inval(buf, n * dev_ss);
    return rc ? -1 : 0;
}
#endif

typedef int (*fatdir_cb)(void *ctx, const char *name, int is_dir, uint32_t size, uint16_t date, uint16_t time);

/* DMA buffers on 32-byte lines (the raw pointer sits just below the aligned one) */
static void *amalloc(size_t n)
{
    uint8_t *raw = fd_malloc(n + 64), *p;
    if (!raw) return 0;
    p = (uint8_t *)(((uintptr_t)raw + 32 + 31) & ~(uintptr_t)31);
    ((void **)p)[-1] = raw;
    return p;
}
static void afree(void *p) { if (p) fd_free(((void **)p)[-1]); }

static struct {
    int ok;
    uint32_t ss;            /* device sector bytes (= bytes per FAT sector, required) */
    uint32_t part;          /* volume start, device LBA */
    uint32_t spc, reserved, fatsz, nfats, root, nclus;
    uint32_t clbytes;       /* bytes per cluster */
} V;
static uint8_t *fatc[4]; static uint32_t fatsec[4]; static int fatnext;   /* FAT sector cache */
/* the last two parent folders, whole, so "Music\A", "Music\B", ... resolve from memory */
#define NDC 2
#define DCMAX (512u * 1024)
static struct dc { char path[300]; uint8_t *data; uint32_t len; int used; } dcs[NDC];
static int seq;
int fatdir_reads;                           /* device reads this session, for the log */

static uint32_t g16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t g32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int ci_eq(const char *a, const char *b)   /* ASCII case-insensitive, other bytes exact */
{
    while (*a && *b) {
        unsigned char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return !*a && !*b;
}

static void drop_caches(void)
{
    int i;
    for (i = 0; i < NDC; i++) { if (dcs[i].data) afree(dcs[i].data); dcs[i].data = 0; dcs[i].used = 0; dcs[i].path[0] = 0; }
    for (i = 0; i < 4; i++) fatsec[i] = 0xffffffffu;
}

/* ---- the volume ---- */
int fatdir_begin(void)
{
    uint8_t *b;
    uint32_t i, bps, totsec;
    V.ok = 0;
#ifdef FATDIR_HOST
    V.ss = h_ss;
#else
    /* The device stays open between sessions until fatdir_release: closing it lets the storage
       driver power the card down, and the OS's next read then waits ~600 ms for it to come back. */
    if (!dev) {
        if (!(dev = fd_malloc(sizeof *dev))) return -1;
        dev_ctor(dev, 0, 0);
        dev_open(dev);               /* not a status (update.c ignores it too); the MBR read checks the device */
    }
    dev_ss = dev_ssize(dev);
    if (dev_ss != 512 && dev_ss != 4096) { dev_close(dev); fd_free(dev); dev = 0; return -1; }
    V.ss = dev_ss;
#endif
    fatdir_reads = 0;
    if (!(b = amalloc(V.ss))) return -1;
    /* the MBR: the first FAT32 partition; an image that starts with a boot sector has none */
    V.part = 0;
    if (dev_read(0, b, 1)) { afree(b); return -1; }
    fatdir_reads++;
    if (b[510] == 0x55 && b[511] == 0xaa && !(b[0x52] == 'F' && b[0x53] == 'A' && b[0x54] == 'T')) {
        for (i = 0; i < 4; i++) {
            const uint8_t *e = b + 0x1be + 16 * i;
            if ((e[4] == 0x0b || e[4] == 0x0c) && g32(e + 8)) { V.part = g32(e + 8); break; }
        }
        if (!V.part || dev_read(V.part, b, 1)) { afree(b); return -1; }
        fatdir_reads++;
    }
    bps = g16(b + 11); V.spc = b[13]; V.reserved = g16(b + 14); V.nfats = b[16];
    totsec = g32(b + 32); V.fatsz = g32(b + 36); V.root = g32(b + 44);
    if (b[510] != 0x55 || b[511] != 0xaa || bps != V.ss || !V.spc || (V.spc & (V.spc - 1)) || !V.reserved ||
        !V.nfats || V.nfats > 2 || !V.fatsz || V.root < 2 || g16(b + 17) != 0 || !totsec) { afree(b); return -1; }
    V.nclus = (totsec - V.reserved - V.nfats * V.fatsz) / V.spc;
    V.clbytes = V.spc * V.ss;
    afree(b);
    if (V.clbytes > 65536) return -1;
    for (i = 0; i < 4; i++) if (!fatc[i] && !(fatc[i] = amalloc(V.ss))) return -1;
    drop_caches();
    V.ok = 1;
    return 0;
}
/* free space from the FSInfo sector (kept current by macOS on unmount), in KB; 0 if unusable */
/* first free cluster in [start, end) from FAT 1 on the disk, 16 sectors per read; a candidate
   counts only when confirm() agrees (the FAT driver's cache can hold allocations not yet written).
   0: none in the range, 0xffffffff: not answered (the caller searches its own way) */
uint32_t fatdir_next_free(uint32_t start, uint32_t end, int (*confirm)(void *, uint32_t), void *ctx, int *reads)
{
    uint8_t *b;
    uint32_t per, c, sec, n, first, i;
    if (!V.ok) return 0xffffffffu;
    per = V.ss / 4;
    if (end > V.nclus + 2) end = V.nclus + 2;
    if (start < 2) start = 2;
    if (!(b = amalloc(16 * V.ss))) return 0xffffffffu;
    for (c = start; c < end; c = first + n * per) {
        sec = c / per;
        n = sec + 16 <= V.fatsz ? 16 : V.fatsz - sec;
        if (!n || dev_read(V.part + V.reserved + sec, b, n)) { afree(b); return 0xffffffffu; }
        (*reads)++;
        first = sec * per;
        for (i = c - first; i < n * per && first + i < end; i++)
            if (!(g32(b + 4 * i) & 0x0fffffff) && confirm(ctx, first + i)) { afree(b); return first + i; }
    }
    afree(b);
    return 0;
}
uint32_t fatdir_nclus(void) { return V.ok ? V.nclus : 0; }

uint32_t fatdir_fsinfo_next;        /* FSInfo next-free hint from the last read */
int fatdir_free_why;                 /* why the last fatdir_free_clusters gave 0: 1 state, 2 memory, 3 boot read, 4 FSInfo sector, 5 signatures, 6 count */
uint32_t fatdir_free_clusters(uint32_t *nclus)
{
    uint8_t *b, *bs;
    uint32_t sec, free = 0, n = 0;
    fatdir_free_why = 1;
    *nclus = V.nclus;
    if (!V.ok) return 0;
    fatdir_free_why = 2;
    if (!(bs = amalloc(V.ss))) return 0;
    if (!(b = amalloc(V.ss))) { afree(bs); return 0; }
    fatdir_free_why = 3;
    if (!dev_read(V.part, bs, 1)) {
        sec = g16(bs + 48);
        fatdir_free_why = 4;
        if (sec && sec < V.reserved && !dev_read(V.part + sec, b, 1)) {
            fatdir_free_why = 5;
            if (g32(b) == 0x41615252 && g32(b + 484) == 0x61417272 && g32(b + 508) == 0xaa550000) {
                free = g32(b + 488);
                fatdir_fsinfo_next = g32(b + 492);
                fatdir_free_why = 6;
                if (free && free != 0xffffffffu && free <= V.nclus) { n = free; fatdir_free_why = 0; }
            }
        }
    }
    afree(b); afree(bs);
    return n;
}

void fatdir_end(void)
{
    int i;
    drop_caches();
    for (i = 0; i < 4; i++) if (fatc[i]) { afree(fatc[i]); fatc[i] = 0; }
    V.ok = 0;
}

void fatdir_release(void)
{
#ifndef FATDIR_HOST
    if (dev) { dev_close(dev); fd_free(dev); dev = 0; }
#endif
}

static uint32_t next_cluster(uint32_t c)
{
    uint32_t sec = V.reserved + (c * 4) / V.ss, off = (c * 4) % V.ss;
    int i;
    for (i = 0; i < 4; i++) if (fatsec[i] == sec) break;
    if (i == 4) {
        i = fatnext; fatnext = (fatnext + 1) & 3;
        if (dev_read(V.part + sec, fatc[i], 1)) { fatsec[i] = 0xffffffffu; return 1; }   /* 1 = error (never a valid next) */
        fatdir_reads++;
        fatsec[i] = sec;
    }
    return g32(fatc[i] + off) & 0x0fffffff;
}
static int read_cluster(uint32_t c, uint8_t *dst)
{
    if (c < 2 || c - 2 >= V.nclus) return -1;
    if (dev_read(V.part + V.reserved + V.nfats * V.fatsz + (c - 2) * V.spc, dst, V.spc)) return -1;
    fatdir_reads++;
    return 0;
}

/* ---- entries ---- */
/* UTF-16LE units (n of them, no terminator) to UTF-8; returns bytes or -1 */
static int u16_to_u8(const uint16_t *u, int n, char *out, int cap)
{
    int i, w = 0;
    for (i = 0; i < n; i++) {
        uint32_t c = u[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < n && u[i + 1] >= 0xdc00 && u[i + 1] <= 0xdfff) { c = 0x10000 + ((c - 0xd800) << 10) + (u[i + 1] - 0xdc00); i++; }
        else if (c >= 0xd800 && c <= 0xdfff) return -1;
        if (c < 0x80) { if (w + 1 >= cap) return -1; out[w++] = (char)c; }
        else if (c < 0x800) { if (w + 2 >= cap) return -1; out[w++] = (char)(0xc0 | c >> 6); out[w++] = (char)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { if (w + 3 >= cap) return -1; out[w++] = (char)(0xe0 | c >> 12); out[w++] = (char)(0x80 | ((c >> 6) & 0x3f)); out[w++] = (char)(0x80 | (c & 0x3f)); }
        else { if (w + 4 >= cap) return -1; out[w++] = (char)(0xf0 | c >> 18); out[w++] = (char)(0x80 | ((c >> 12) & 0x3f)); out[w++] = (char)(0x80 | ((c >> 6) & 0x3f)); out[w++] = (char)(0x80 | (c & 0x3f)); }
    }
    out[w] = 0;
    return w;
}
static uint8_t short_sum(const uint8_t *e)
{
    uint8_t s = 0; int i;
    for (i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}
/* visit every live entry of a directory image; v returns nonzero to stop (that value is returned) */
typedef int (*visit_fn)(void *ctx, const char *name, const uint8_t *e);
static int walk_entries(const uint8_t *d, uint32_t len, visit_fn v, void *ctx)
{
    uint16_t lfn[260];
    char name[800];
    int lfn_n = 0, lfn_ok = 0, lfn_chk = 0, lfn_expect = 0, i, rc;
    uint32_t o;
    for (o = 0; o + 32 <= len; o += 32) {
        const uint8_t *e = d + o;
        if (e[0] == 0) return 0;                         /* end of the directory */
        if (e[0] == 0xe5) { lfn_ok = 0; lfn_n = 0; continue; }
        if ((e[11] & 0x3f) == 0x0f) {                    /* a long-name part */
            int ord = e[0] & 0x3f, k, pos;
            if (e[0] & 0x40) { lfn_ok = 1; lfn_n = ord * 13; lfn_chk = e[13]; lfn_expect = ord; if (ord < 1 || ord > 20) lfn_ok = 0; }
            else if (!lfn_ok || ord != lfn_expect - 1 || e[13] != lfn_chk) { lfn_ok = 0; continue; }
            if (!lfn_ok) continue;
            lfn_expect = ord;
            pos = (ord - 1) * 13;
            for (k = 0; k < 5; k++) lfn[pos + k] = (uint16_t)g16(e + 1 + 2 * k);
            for (k = 0; k < 6; k++) lfn[pos + 5 + k] = (uint16_t)g16(e + 14 + 2 * k);
            for (k = 0; k < 2; k++) lfn[pos + 11 + k] = (uint16_t)g16(e + 28 + 2 * k);
            continue;
        }
        if (e[11] & 0x08) { lfn_ok = 0; lfn_n = 0; continue; }   /* volume label */
        if (e[0] == '.') { lfn_ok = 0; lfn_n = 0; continue; }    /* . and .. */
        {
            /* not an 8.3 name (bytes outside the OEM set, reserved attribute bits): junk from an
               interrupted write, skipped as the OS's find skips it */
            int k, junk = (e[11] & 0xc0) != 0;
            for (k = 0; k < 11 && !junk; k++) if ((e[k] < 0x20 && !(k == 0 && e[k] == 0x05)) || e[k] >= 0x80) junk = 1;
            if (junk) { lfn_ok = 0; lfn_n = 0; continue; }
        }
        if (lfn_ok && lfn_expect == 1 && lfn_chk == short_sum(e)) {
            int n = lfn_n;
            while (n > 0 && (lfn[n - 1] == 0 || lfn[n - 1] == 0xffff)) n--;
            for (i = 0; i < n; i++) if (lfn[i] == 0) break;
            if (i < n || n == 0 || u16_to_u8(lfn, n, name, sizeof name) < 0 || slen(name) > 255) return -1;
        } else {
            /* 8.3 only: the name as stored, NT lower-case flags not applied. The OS lists a
               Mac-written cover.jpg as COVER.JPG, and the database locations were built from that. */
            int n = 0, k;
            for (k = 0; k < 8 && e[k] != ' '; k++) { uint8_t c = e[k]; name[n++] = (char)(k == 0 && c == 0x05 ? 0xe5 : c); }
            if (e[8] != ' ') { name[n++] = '.'; for (k = 8; k < 11 && e[k] != ' '; k++) name[n++] = (char)e[k]; }
            name[n] = 0;
            if (!n) return -1;
        }
        lfn_ok = 0; lfn_n = 0;
        if ((rc = v(ctx, name, e))) return rc;
    }
    return 0;
}

/* a whole directory (every cluster of the chain) into a new buffer */
static int load_dir(uint32_t cl, uint8_t **out, uint32_t *len)
{
    uint32_t cap = V.clbytes, n = 0, c = cl, guard = 0;
    uint8_t *buf = amalloc(cap), *nb;
    if (!buf) return -1;
    while (c >= 2 && c < 0x0ffffff8) {
        if (n + V.clbytes > cap) {
            if (cap >= DCMAX) { afree(buf); return -1; }
            cap *= 2;
            if (!(nb = amalloc(cap))) { afree(buf); return -1; }
            for (uint32_t i = 0; i < n; i++) nb[i] = buf[i];
            afree(buf); buf = nb;
        }
        if (read_cluster(c, buf + n)) { afree(buf); return -1; }
        n += V.clbytes;
        /* the end-of-directory mark inside this cluster ends the chain early */
        for (uint32_t o = n - V.clbytes; o < n; o += 32) if (buf[o] == 0) { *out = buf; *len = n; return 0; }
        if (++guard > DCMAX / V.clbytes) { afree(buf); return -1; }
        c = next_cluster(c);
        if (c == 1) { afree(buf); return -1; }
    }
    *out = buf; *len = n;
    return 0;
}

struct findctx { const char *want; uint32_t cl; int found; };
static int find_visit(void *ctx, const char *name, const uint8_t *e)
{
    struct findctx *f = ctx;
    if (!ci_eq(name, f->want)) return 0;
    if (!(e[11] & 0x10)) return -2;                      /* a file where a folder was asked for */
    f->cl = g16(e + 20) << 16 | g16(e + 26);
    f->found = 1;
    return 1;
}
/* the cluster of folder path ("Music\A" or "Music/A"), 0 on failure. The parent folder's image is cached. */
static uint32_t resolve(const char *path)
{
    char comp[300], parent[300];
    int i = 0, n = 0, pl;
    uint32_t cl = V.root;
    const char *p = path;
    while (*p == '\\' || *p == '/') p++;
    if (!*p) return cl;
    for (pl = 0; p[pl]; pl++);
    while (pl > 0 && (p[pl - 1] == '\\' || p[pl - 1] == '/')) pl--;
    if (pl >= (int)sizeof parent) return 0;
    for (i = 0; i < pl; i++) parent[i] = p[i];
    parent[pl] = 0;
    /* the last component is looked up in its parent (cached); earlier ones resolve recursively */
    for (i = pl - 1; i > 0 && parent[i] != '\\' && parent[i] != '/'; i--);
    if (i > 0) { parent[i] = 0; n = 0; for (int k = i + 1; k < pl; k++) comp[n++] = p[k]; comp[n] = 0; }
    else { n = pl; for (i = 0; i < pl; i++) comp[i] = p[i]; comp[n] = 0; parent[0] = 0; }
    {
        struct dc *d = 0;
        int k;
        for (k = 0; k < NDC; k++) if (dcs[k].data && ci_eq(dcs[k].path, parent)) d = &dcs[k];
        if (!d) {
            uint32_t pcl = parent[0] ? resolve(parent) : V.root;
            uint8_t *data; uint32_t len;
            if (!pcl || load_dir(pcl, &data, &len)) return 0;
            for (k = 1, d = &dcs[0]; k < NDC; k++) if (dcs[k].used < d->used) d = &dcs[k];
            if (d->data) afree(d->data);
            d->data = data; d->len = len;
            for (k = 0; parent[k] && k < (int)sizeof d->path - 1; k++) d->path[k] = parent[k];
            d->path[k] = 0;
        }
        d->used = ++seq;
        {
            struct findctx f = { comp, 0, 0 };
            if (walk_entries(d->data, d->len, find_visit, &f) < 0 || !f.found) return 0;
            cl = f.cl;
        }
    }
    return cl;
}

struct listctx { fatdir_cb cb; void *ctx; int n; int stopped; };
static int list_visit(void *ctx, const char *name, const uint8_t *e)
{
    struct listctx *l = ctx;
    l->n++;
    if (l->cb(l->ctx, name, (e[11] & 0x10) != 0, g32(e + 28), (uint16_t)g16(e + 24), (uint16_t)g16(e + 22))) { l->stopped = 1; return 2; }
    return 0;
}
/* the entries of folder path, in directory order; returns their number, -3 if the callback
   stopped, -1 when the listing can't be trusted (the caller lists through the OS instead) */
int fatdir_list(const char *path, fatdir_cb cb, void *ctx)
{
    uint32_t cl, len;
    uint8_t *data;
    int rc;
    struct listctx l = { cb, ctx, 0, 0 };
    if (!V.ok) return -1;
    cl = resolve(path);
    if (!cl) return -1;
    /* the whole chain first: a long name may straddle two clusters */
    if (load_dir(cl, &data, &len)) return -1;
    rc = walk_entries(data, len, list_visit, &l);
    afree(data);
    if (rc < 0) return -1;
    if (rc == 2) return -3;
    return l.n;
}
