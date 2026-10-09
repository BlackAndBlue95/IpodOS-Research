/* Art database FLAC\covers.db: one block of the four thumbnail formats per album folder.
 * artdb_update() after each library load brings it in step with the sync's album scan.
 * art.c serves the OS's thumbnail reads from the blocks. */
#include "e.h"

#define ADB_VER   1
#define ADB_ESIZE 128
#define ADB_TAB   64
#define ADB_TMP   "FLAC\\covers.tmp"
#define ADB_NEW   "FLAC\\covers.db.new"
#define ADB_OLD   "FLAC\\covers.db.old"
#define TIMER_E   (*(volatile uint32_t *)0x3c7000b4)

/* File layout, little-endian:
 *   header, 64 bytes: 'FCDB', version 1, entry count, entry size 128, table offset 64, pixel
 *     base (4096 aligned), block size ARTDB_BLOCK, generation (TIMER_E at the update), zero.
 *   table: entry count entries of 128 bytes, sorted by key, each one a struct ent:
 *     key u64 (artdb_key), pixoff u32 (absolute, 0 = no pixels), state u8 (0 ok, 1 decode
 *     failed, 3 no source), kind u8 (1..5 cover file, 6 picture inside the first FLAC),
 *     pad u16, source size, source mtime, picture offset, picture length, source name (96 bytes).
 *   blocks: ARTDB_BLOCK bytes each, in table order: the four thumbnail formats in art.c's afmt
 *     order (the bytes a cover.ithmb held). One per entry with pixels. */
struct ent {
    uint64_t key; uint32_t pixoff; uint8_t state, kind; uint16_t pad;
    uint32_t srcsize, srcmtime, picoff, piclen; char src[96];
};
/* kind 1..5 is ossync.c cover_names[kind - 1], in order of preference */

/* the table in RAM, for lookups: adopted from artdb_update (no re-read of the file) */
static struct ent *tab; static int ntab;
static void adopt(struct ent *t, int n)
{
    int i;
    for (i = 0; i < n; i++) t[i].pad = 0;
    if (tab) os_free(tab);               /* lookups resolve by key at read time: nothing holds entry pointers */
    tab = t; ntab = n;
}

static int streq_ci(const char *a, const char *b)
{
    while (*a && *b) { char x = *a++, y = *b++; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; }
    return !*a && !*b;
}
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scpy(char *d, const char *s, int cap) { int i = 0; while (s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static int path_join(char *d, int cap, const char *dir, const char *name)
{
    int n = slen(dir), m = slen(name);
    if (n + 1 + m >= cap) return 0;
    memcpy(d, dir, n); d[n] = '\\'; memcpy(d + n + 1, name, m + 1);
    return 1;
}

/* "Music\A\B" or "Music\A\B\" -> FNV-1a 64 over "A/B" */
uint64_t artdb_key(const char *dir)
{
    uint64_t h = 14695981039346656037ULL;
    int i = 0, start = 0, end;
    if ((dir[0] | 32) == 'm' && (dir[1] | 32) == 'u' && (dir[2] | 32) == 's' && (dir[3] | 32) == 'i' && (dir[4] | 32) == 'c' && (dir[5] == '\\' || dir[5] == '/')) start = 6;
    end = slen(dir);
    while (end > start && (dir[end - 1] == '\\' || dir[end - 1] == '/')) end--;
    for (i = start; i < end; i++) { uint8_t c = dir[i] == '\\' ? '/' : (uint8_t)dir[i]; h ^= c; h *= 1099511628211ULL; }
    return h;
}

static struct ent *find(uint64_t key)
{
    int lo = 0, hi = ntab;
    while (lo < hi) { int m = (lo + hi) / 2; if (tab[m].key < key) lo = m + 1; else hi = m; }
    return lo < ntab && tab[lo].key == key ? &tab[lo] : 0;
}
int artdb_lookup(uint64_t key, uint32_t *pixoff, uint32_t *srcoff, uint32_t *srclen, char *src, int srccap)
{
    struct ent *e = find(key);
    if (!e) return 0;
    if (e->state || !e->pixoff) return -1;
    if (pixoff) *pixoff = e->pixoff;
    if (srcoff) *srcoff = e->kind == 6 ? e->picoff : 0;
    if (srclen) *srclen = e->kind == 6 ? e->piclen : 0;
    if (src) scpy(src, e->src, srccap);
    return 1;
}

/* ---- the file ---- */
static int read_table(const char *path, struct ent **out, int *n, uint32_t *pixbase)
{
    File *f = file_open(path);
    uint32_t h[16];
    int sz, got;
    struct ent *t = 0;
    *out = 0; *n = 0;
    if (!f) return -1;
    sz = file_size(f);
    if (sz < 64 || f->vt->read(f, h, 64, 2) != 64 || h[0] != 0x42444346u || h[1] != ADB_VER || h[3] != ADB_ESIZE || h[4] != ADB_TAB ||
        h[2] > 4096 || (int)(ADB_TAB + h[2] * ADB_ESIZE) > sz) { f->vt->close(f); f->vt->del(f); return -2; }
    if (h[2] && !(t = os_malloc(h[2] * ADB_ESIZE))) { f->vt->close(f); f->vt->del(f); return -3; }
    got = h[2] ? f->vt->read(f, t, (int)(h[2] * ADB_ESIZE), 2) : 0;
    f->vt->close(f); f->vt->del(f);
    if (got != (int)(h[2] * ADB_ESIZE)) { if (t) os_free(t); return -4; }
    *out = t; *n = (int)h[2]; if (pixbase) *pixbase = h[5];
    return 0;
}

void artdb_load(void)
{
    struct ent *t; int n;
    if (read_table(ARTDB_FILE, &t, &n, 0) < 0) { t = 0; n = 0; }
    adopt(t, n);
}

/* ---- a folder's art source ---- */
int art_flac_picture(const char *path, uint32_t *off, uint32_t *len);   /* art.c */

/* Fills e from the scan's source for the folder (ossync.c album_cb picks it with the same rule),
   and the embedded picture span when the old entry is not current. Returns 1 when old still
   applies (copied into e), 0 when the folder must be decoded. */
static int source_of(const struct sync_album *a, struct ent *e, const struct ent *old)
{
    const char *dir = a->dir;
    char p[300];
    e->state = 3; e->kind = a->kind; e->srcsize = a->srcsize; e->srcmtime = a->srcmtime; e->picoff = e->piclen = 0; e->pixoff = 0;
    scpy(e->src, a->src, sizeof e->src);
    if (e->kind >= 1 && e->kind <= 5) e->state = 0;
    if (old && old->kind == e->kind && old->srcsize == e->srcsize && old->srcmtime == e->srcmtime && streq_ci(old->src, e->src)) {
        *e = *old;
        return 1;
    }
    if (e->kind == 6) {
        if (path_join(p, sizeof p, dir, e->src) && art_flac_picture(p, &e->picoff, &e->piclen)) e->state = 0;
        else { e->state = 3; e->piclen = 0; }          /* no picture: remembered with the file's size and mtime */
    }
    return 0;
}

static int ent_cmp_key(const struct ent *a, const struct ent *b) { return a->key < b->key ? -1 : a->key > b->key; }
static void sort_ents(struct ent *e, int n)
{
    int i, j;
    for (i = 1; i < n; i++) { struct ent t = e[i]; for (j = i; j > 0 && ent_cmp_key(&e[j - 1], &t) > 0; j--) e[j] = e[j - 1]; e[j] = t; }
}

/* copy n bytes of src (from off) to dst, through buf */
static int copy_block(File *dst, File *src, uint32_t off, uint8_t *buf, uint32_t n)
{
    uint32_t done = 0;
    if (src->vt->seek(src, (int64_t)off, 0)) return -1;
    while (done < n) {
        uint32_t want = n - done > 0x40000 ? 0x40000 : n - done, w;
        int got = src->vt->read(src, buf, (int)want, 2);
        if (got <= 0) return -2;
        w = (uint32_t)got;
        if (os_file_write(dst, buf, &w)) return -3;
        done += (uint32_t)got;
    }
    return 0;
}

/* Brings the file in step with the scanned folders. An entry stays current while its source
   (kind, size, mtime) is unchanged, so a boot without changes writes nothing. Changed folders are
   decoded into covers.tmp, removed ones dropped; the new file goes to covers.db.new and is renamed
   over covers.db, the old one kept as covers.db.old. */
void artdb_update(void)
{
    const struct sync_album *al;
    int n, i, made = 0, kept = 0, failed = 0, none = 0, dropped = 0, changed = 0, rc = 0;
    struct ent *old = 0, *ne = 0, *newtab = 0; int nold = 0;
    uint32_t oldbase = 0, t0 = TIMER_E, tmpoff = 0, pixbase, blocks = 0, ntmp = 0;
    uint8_t *buf = 0;
    File *tmp = 0, *out = 0, *oldf = 0;
    void *m;
    char p[300];
    al = sync_albums(&n);
    if (n < 0) { log_s("   art: scan incomplete, covers.db kept\n"); return; }
    if (n == 0) { log_s("   art: no album folders, covers.db kept\n"); return; }
    read_table(ARTDB_FILE, &old, &nold, &oldbase);
    if (!(ne = os_malloc((n + 1) * sizeof *ne))) { log_s("   art: no memory\n"); if (old) os_free(old); return; }
    /* every folder's entry, current ones carried over */
    for (i = 0; i < n; i++) {
        struct ent *o = 0, *e = &ne[i];
        uint64_t key = artdb_key(al[i].dir);
        int lo = 0, hi = nold;
        while (lo < hi) { int mid = (lo + hi) / 2; if (old[mid].key < key) lo = mid + 1; else hi = mid; }
        if (lo < nold && old[lo].key == key) { o = &old[lo]; o->pad = 1; }   /* pad: seen */
        e->key = key; e->pad = 0;
        if (source_of(&al[i], e, o)) { e->pad = 1; if (e->state == 0 && e->pixoff) kept++; else none++; continue; }   /* pad: pixels in the old file */
        changed = 1;
        if (e->state == 3) { none++; continue; }
        /* decode into the temporary file */
        if (!buf && !(buf = os_malloc(ARTDB_BLOCK))) { failed++; e->state = 1; continue; }
        if (!tmp) {
            vol_delete(ADB_TMP);
            if (!(m = os_malloc(0x114)) || !(tmp = os_file_ctor(m, ADB_TMP, 1024, 1)) || os_file_create(tmp)) { log_s("   art: can't create covers.tmp\n"); tmp = 0; failed++; e->state = 1; continue; }
        }
        if (!path_join(p, sizeof p, al[i].dir, e->src)) { e->state = 1; failed++; continue; }
        boost_hold(10000);
        if (art_decode_set(p, e->kind == 6 ? e->picoff : 0, e->kind == 6 ? e->piclen : 0, buf)) {
            e->state = 1; failed++;
            log_s("   art: "); log_name(al[i].dir); log_s(": can't decode "); log_s(e->src); log_c('\n');
            continue;
        }
        { uint32_t w = ARTDB_BLOCK; if (os_file_write(tmp, buf, &w)) { e->state = 1; failed++; continue; } }
        e->state = 0; e->pixoff = tmpoff; e->pad = 2;        /* pad: pixels in the temporary file */
        tmpoff += ARTDB_BLOCK; ntmp++; made++;
    }
    for (i = 0; i < nold; i++) if (!old[i].pad) { dropped++; changed = 1; }
    if (tmp) { tmp->vt->del(tmp); tmp = 0; }
    if (!changed) {
        log_s("   art: "); log_d(n); log_s(" albums, "); log_d(kept); log_s(" with art, "); log_d(none); log_s(" without, up to date ("); log_d((int)((TIMER_E - t0) / 1000)); log_s(" ms)\n");
        sort_ents(ne, n);
        adopt(ne, n); ne = 0;                /* the entries as they are, with the old file's offsets */
        if (old) os_free(old);
        if (buf) os_free(buf);
        return;
    }
    /* the new file: header, table, blocks in table order */
    sort_ents(ne, n);
    for (i = 0; i < n; i++) if (ne[i].state == 0 && (ne[i].pad == 2 || ne[i].pixoff)) blocks++;
    pixbase = (ADB_TAB + (uint32_t)n * ADB_ESIZE + 4095) & ~4095u;
    if (!buf && !(buf = os_malloc(ARTDB_BLOCK))) { rc = -1; goto out; }
    vol_delete(ADB_NEW);
    if (!(m = os_malloc(0x114)) || !(out = os_file_ctor(m, ADB_NEW, 1024, 1)) || os_file_create(out)) { rc = -2; out = 0; goto out; }
    {
        uint32_t h[16], w, b = 0;
        struct ent *table = os_malloc(n * ADB_ESIZE + 4096);
        if (!table) { rc = -3; goto out; }
        memset(h, 0, sizeof h);
        h[0] = 0x42444346u; h[1] = ADB_VER; h[2] = (uint32_t)n; h[3] = ADB_ESIZE; h[4] = ADB_TAB; h[5] = pixbase; h[6] = ARTDB_BLOCK; h[7] = t0;
        memcpy(table, ne, n * ADB_ESIZE);
        for (i = 0; i < n; i++) {
            struct ent *e = &table[i];
            uint32_t src = e->pixoff;
            e->pad = ne[i].pad;                            /* where its pixels are now (1 old file, 2 tmp) */
            e->picoff = ne[i].picoff;
            if (e->state == 0 && (e->pad == 2 || src)) { e->pixoff = pixbase + b * ARTDB_BLOCK; b++; }
            else e->pixoff = 0;
        }
        w = 64; if (os_file_write(out, h, &w)) { rc = -4; os_free(table); goto out; }
        for (i = 0; i < n; i++) {
            struct ent e2 = table[i];
            e2.pad = 0;
            w = ADB_ESIZE; if (os_file_write(out, &e2, &w)) { rc = -5; os_free(table); goto out; }
        }
        memset(buf, 0, 4096);
        w = pixbase - ADB_TAB - (uint32_t)n * ADB_ESIZE;
        if (w && os_file_write(out, buf, &w)) { rc = -6; os_free(table); goto out; }
        if (ntmp) tmp = file_open(ADB_TMP);
        if (nold && blocks) oldf = file_open(ARTDB_FILE);
        for (i = 0; i < n && !rc; i++) {
            struct ent *e = &table[i];
            if (!e->pixoff) continue;
            if (e->pad == 2) rc = tmp ? copy_block(out, tmp, ne[i].pixoff, buf, ARTDB_BLOCK) : -7;
            else rc = oldf ? copy_block(out, oldf, ne[i].pixoff, buf, ARTDB_BLOCK) : -8;
        }
        if (rc) os_free(table); else newtab = table;
    }
out:
    if (tmp) tmp->vt->del(tmp);
    if (oldf) { oldf->vt->close(oldf); oldf->vt->del(oldf); }
    if (out) {
        out->vt->del(out);
        if (!rc) {
            vol_delete(ADB_OLD);
            vol_rename(ARTDB_FILE, "covers.db.old");      /* whatever is there, readable or not */
            if (vol_rename(ADB_NEW, "covers.db")) rc = -9;
        }
        if (rc) vol_delete(ADB_NEW);
    }
    if (ntmp) vol_delete(ADB_TMP);
    if (newtab && !rc) adopt(newtab, n);
    else { if (newtab) os_free(newtab); artdb_load(); }
    if (changed) {
        log_s("   art: "); log_d(made); log_s(" made, "); log_d(kept); log_s(" kept, "); log_d(failed); log_s(" failed, ");
        log_d(none); log_s(" without art, "); log_d(dropped); log_s(" dropped");
        if (rc) { log_s(", NOT written, rc "); log_d(rc); }
        log_s(" ("); log_d((int)((TIMER_E - t0) / 1000)); log_s(" ms)\n");
    }
    if (buf) os_free(buf);
    if (old) os_free(old);
    if (ne) os_free(ne);
}
