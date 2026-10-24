/* Album art for FLAC tracks (no ArtworkDB entry). art_find wraps the OS FindImageForTrack
 * (0x080457e4) and points thumbnails at the album's cover; art_load wraps ArtworkThumb::Load
 * (0x080f32ec) and decodes that cover to the requested size. Nothing is written to the iPod. */
#include "dither565.h"
#define os_art_find   ((void *(*)(void *, void *))0x080457e4)
#define os_art_load   ((void (*)(void *, void *))0x080f32ec)
#define os_img_new    ((uint8_t *(*)(void *, int))0x08053194)
#define os_thumb_new  ((uint8_t *(*)(void *, int, int))0x0805320c)
#define os_geo_init   ((void (*)(void *, int, int))0x08133b14)
#define os_geo_get    ((void (*)(void *, void *, int))0x08133a80)
#define os_geo_free   ((void (*)(void *))0x0824f150)
#define os_alloc_a    ((void *(*)(void *, int, int, int))0x082648a8)
#define os_alloc_b    ((void *(*)(int, int))0x080e07a4)
#define ARTMAGIC 0x54524146   /* 'FART': blk = magic, cover file path */
#define ARTMAGIC2 0x45524146  /* 'FARE': picture embedded in the FLAC, blk = magic, offset, length, FLAC path */
#define ARTMAGIC3 0x44524146  /* 'FARD': thumbnails in covers.db, blk = magic, jpeg blk address, key lo, key hi, covers.db path */
static int is_art_blk(const uint32_t *blk) { return blk[0] == ARTMAGIC || blk[0] == ARTMAGIC2 || blk[0] == ARTMAGIC3; }
static const char *blk_cov(const uint32_t *blk) { return (const char *)(blk + (blk[0] == ARTMAGIC ? 1 : blk[0] == ARTMAGIC3 ? 4 : 3)); }
static uint64_t blk_key(const uint32_t *blk) { return (uint64_t)blk[2] | (uint64_t)blk[3] << 32; }
uint64_t artdb_key(const char *dir);
int artdb_lookup(uint64_t key, uint32_t *pixoff, uint32_t *srcoff, uint32_t *srclen, char *src, int srccap);
static int art_gen;          /* set while decoding for covers.db: the master may use TJpgDec too */
static uint32_t covs_gen;    /* bumped at each library load: folder entries resolve again */
/* the cover block a covers.db block (ARTMAGIC3) points at; decoded when its thumbnail is unusable */
static const uint32_t *blk_jpeg(const uint32_t *blk) { return blk[0] == ARTMAGIC3 ? (const uint32_t *)blk[1] : blk; }
/* defined in region E (osfile.c, power.c, log.c) */
int dir_list(const char *dir, int (*cb)(void *ctx, const char *name, int is_dir, uint32_t size, uint16_t date, uint16_t time), void *ctx);
int file_write(const char *path, const void *data, uint32_t n);
void boost_hold(uint32_t ms);
void log_x(uint32_t v, int digits); void log_s(const char *s); void log_d(int v); void log_c(char c); void log_name(const char *s);
/* the jpeg's offset and length in the file named by cov: the two words before the path,
 * or the whole file (ARTMAGIC) */
static void cov_span(const char *cov, uint32_t *off, uint32_t *len)
{
    const uint32_t *b = (const uint32_t *)cov;
    if (b[-1] == ARTMAGIC) { *off = 0; *len = 0; } else { *off = b[-2]; *len = b[-1]; }
}

int boost_begin(void);
void boost_end(int boosted);
int cover_decode(int (*rd)(void *, void *, int), int (*skip)(void *, int), void *ctx,
                 int tw, int th, uint16_t *out, void *work, uint32_t *acc);

/* track location (track+0xc0) -> full path, or 0 when loc[36] is clear or the record is bad.
 * loc[0] == 1: a pointer to an FLPT record holding the path. Otherwise the path is relative to
 * the Music folder (iPod_Control\Music\). */
static int track_path(const uint8_t *loc, char *d, int cap)
{
    static const char ipm[] = "iPod_Control\\Music\\";
    const char *s; int n = 0, i;
    if (!loc[36]) return 0;
    if (loc[0] == 1) {
        uint32_t a = loc[1] | loc[2] << 8 | loc[3] << 16 | (uint32_t)loc[4] << 24;
        if ((a & 3) || (a >> 26) != 2 || *(uint32_t *)a != 0x54504c46) return 0;
        s = (const char *)a + 4;
    } else {
        for (; ipm[n]; n++) d[n] = ipm[n];
        s = (const char *)loc;
    }
    for (i = 0; s[i] && n < cap - 1; i++) d[n++] = s[i];
    d[n] = 0;
    return n;
}

static int mcmp(const void *a, const void *b, int n)
{ const uint8_t *x = a, *y = b; while (n--) if (*x++ != *y++) return 1; return 0; }

static File *open_file(const char *p)
{
    File *f = os_file_ctor(os_malloc(0x114), p, 1024, 1);
    if (f && f->vt->open(f)) { f->vt->del(f); f = 0; }
    return f;
}

/* The jpeg PICTURE block inside a FLAC, for folders without a cover file (as Rockbox does).
 * The front cover (type 3) wins, else the first jpeg. */
static int flac_picture(const char *path, uint32_t *off, uint32_t *len)
{
    uint32_t hb[12];
    uint8_t *h = (uint8_t *)hb;
    uint32_t pos = 4, bl, p, ml, dl, ptype;
    int last = 0, ok = 0;
    File *f = open_file(path);
    if (!f) return 0;
    if (rd(f, h, 4) != 4 || h[0] != 'f' || h[1] != 'L' || h[2] != 'a' || h[3] != 'C') goto out;
    while (!last) {
        if (f->vt->seek(f, (int64_t)pos, 0) || rd(f, h, 4) != 4) break;
        last = h[0] & 0x80; bl = (uint32_t)h[1] << 16 | h[2] << 8 | h[3];
        if ((h[0] & 0x7f) == 6 && bl > 32) {
            if (rd(f, h, 8) != 8) break;
            ptype = be32(h); ml = be32(h + 4);
            if (ml >= 9 && ml <= 32 && rd(f, h, ml) == (int)ml && (h[0] | 32) == 'i' && (h[6] | 32) == 'j'
                && (h[7] | 32) == 'p' && ((h[8] | 32) == 'e' || (h[8] | 32) == 'g')) {
                p = pos + 4 + 8 + ml;
                if (rd(f, h, 4) != 4) break;
                dl = be32(h);
                p += 4 + dl + 16;
                if (f->vt->seek(f, (int64_t)p, 0) || rd(f, h, 4) != 4) break;
                if (be32(h) > 0 && p + 4 + be32(h) <= pos + 4 + bl && (!ok || ptype == 3)) {
                    *off = p + 4; *len = be32(h); ok = 1;
                    if (ptype == 3) break;
                }
            }
        }
        pos += 4 + bl;
    }
out:
    f->vt->close(f); f->vt->del(f);
    return ok;
}

/* Album folders seen, with their art. Entries are never freed: the OS may still hold an offset
 * into one of their blocks. After a library load each entry resolves again against covers.db;
 * the old blocks leak, a few bytes each. */
typedef struct Cov { struct Cov *next; uint32_t *blk, *nat; uint64_t key; uint32_t gen, id; char dir[1]; } Cov;
static Cov *covs;
static uint32_t ncovs;
/* artwork image id of each folder's album image: above the ids an ArtworkDB holds */
#define ART_ID0 0x7f000000u

/* Art for a folder path (dlen bytes, with the trailing backslash): covers.db first, else a
   cover file in the folder, decoded on request. */
static void resolve_cover(Cov *c, const char *path, int dlen)
{
    static const char *const names[] = { "cover.jpg", "folder.jpg", "front.jpg", "cover.jpeg", "albumart.jpg", 0 };
    static const char dbpath[] = "FLAC\\covers.db";
    uint32_t pixoff, soff, slen;
    char src[100], buf[300];
    int i, j, k, rc;
    c->blk = 0; c->nat = 0; c->gen = covs_gen;
    rc = artdb_lookup(c->key, &pixoff, &soff, &slen, src, sizeof src);
    if (rc < 0) return;                                   /* the folder is known to hold no art */
    if (rc > 0) {
        memcpy(buf, path, dlen);
        for (j = 0, i = dlen; src[j] && i < (int)sizeof(buf) - 1; j++) buf[i++] = src[j];
        buf[i] = 0;
        if (slen) { if ((c->blk = os_malloc(16 + i + 1))) { c->blk[0] = ARTMAGIC2; c->blk[1] = soff; c->blk[2] = slen; memcpy(c->blk + 3, buf, i + 1); } }
        else if ((c->blk = os_malloc(8 + i + 1))) { c->blk[0] = ARTMAGIC; memcpy(c->blk + 1, buf, i + 1); }
        if (c->blk && (c->nat = os_malloc(16 + sizeof dbpath))) {
            c->nat[0] = ARTMAGIC3; c->nat[1] = (uint32_t)c->blk; c->nat[2] = (uint32_t)c->key; c->nat[3] = (uint32_t)(c->key >> 32);
            memcpy(c->nat + 4, dbpath, sizeof dbpath);
        }
        return;
    }
    /* not in covers.db (not built yet, or its listing failed): use a cover file */
    for (k = 0; names[k]; k++) {
        File *f;
        memcpy(buf, path, dlen);
        for (j = 0, i = dlen; names[k][j] && i < (int)sizeof(buf) - 1; j++) buf[i++] = names[k][j];
        buf[i] = 0;
        if ((f = open_file(buf))) {
            f->vt->close(f); f->vt->del(f);
            if ((c->blk = os_malloc(8 + i + 1))) { c->blk[0] = ARTMAGIC; memcpy(c->blk + 1, buf, i + 1); }
            break;
        }
    }
}
/* the folder's entry, made unresolved if new */
static Cov *cov_of(const char *path, int dlen)
{
    static Cov *last;                        /* an album's tracks come one after another */
    Cov *c;
    if (last && !mcmp(last->dir, path, dlen) && !last->dir[dlen]) return last;
    for (c = covs; c; c = c->next)
        if (!mcmp(c->dir, path, dlen) && !c->dir[dlen]) return last = c;
    if (ncovs >= 0xffffff || !(c = os_malloc(sizeof(Cov) + dlen + 1))) return 0;
    memcpy(c->dir, path, dlen); c->dir[dlen] = 0;
    c->key = artdb_key(c->dir);
    c->blk = c->nat = 0; c->gen = covs_gen - 1; c->id = ART_ID0 + ncovs++;
    c->next = covs; covs = c;
    return last = c;
}
static void cov_fresh(Cov *c)
{
    int n = 0;
    if (c->gen == covs_gen) return;
    while (c->dir[n]) n++;
    resolve_cover(c, c->dir, n);
}
static Cov *find_cover(const char *path, int dlen)
{
    Cov *c = cov_of(path, dlen);
    if (c) cov_fresh(c);
    return c;
}
static Cov *cov_by_id(uint32_t id)
{
    Cov *c;
    for (c = covs; c; c = c->next) if (c->id == id) return c;
    return 0;
}
int art_flac_picture(const char *path, uint32_t *off, uint32_t *len) { return flac_picture(path, off, len); }

static const struct { int16_t fmt, side; int32_t size; } afmt[] = {
    { 1055, 128, 128 * 256 }, { 1060, 320, 320 * 640 }, { 1061, 55, 55 * 112 }, { 1068, 128, 128 * 256 }
};
#define ITHMB_TOTAL (128 * 256 + 320 * 640 + 55 * 112 + 128 * 256)
static int afmt_index(int fmt) { int i; for (i = 0; i < 4; i++) if (afmt[i].fmt == fmt) return i; return -1; }
static uint32_t afmt_off(int i) { uint32_t o = 0; int k; for (k = 0; k < i; k++) o += afmt[k].size; return o; }

/* Decodes the four thumbnail sizes of one cover for covers.db (artdb.c). Goes through cache_get
 * as the OS requests do; the master may use TJpgDec too, since memory is free while the library
 * is loaded. */
static int decode_cover(const char *path, int w, int h, int stride, uint8_t *dst);
static void cache_flush(void);
int art_decode_set(const char *src, uint32_t srcoff, uint32_t srclen, uint8_t *out)
{
    uint32_t *blk; int i, n, rc = 0;
    for (n = 0; src[n]; n++);
    if (!(blk = os_malloc(16 + n + 1))) return -2;
    if (srclen) { blk[0] = ARTMAGIC2; blk[1] = srcoff; blk[2] = srclen; memcpy(blk + 3, src, n + 1); }
    else { blk[0] = ARTMAGIC; memcpy(blk + 1, src, n + 1); }
    art_gen = 1;
    for (i = 0; i < 4 && !rc; i++)
        rc = decode_cover(blk_cov(blk), afmt[i].side, afmt[i].side, afmt[i].size / afmt[i].side, out + afmt_off(i));
    art_gen = 0;
    cache_flush();
    os_free(blk);
    return rc;
}
/* covers.db may have changed: cached pixels that no stream holds are dropped */
void art_new_library(void)
{
    covs_gen++;
    cache_flush();
}

/* Entry hooks for OS functions (see HOOK below). The OS has no MMU and its code is writable. */
static int is_ours_img(uint8_t *img)
{
    uint8_t *th; uint32_t *blk;
    if (!img || ((uint32_t)img >> 26) != 2) return 0;
    th = *(uint8_t **)(img + 36);
    if (!th || ((uint32_t)th >> 26) != 2) return 0;
    blk = *(uint32_t **)(th + 24);
    return !((uint32_t)blk & 3) && ((uint32_t)blk >> 26) == 2 && is_art_blk(blk);
}
#define os_thumb_iter ((uint8_t *(*)(void *, void *, int))0x08048c3c)
static int decode_cover(const char *path, int w, int h, int stride, uint8_t *dst);
struct CE;
static struct CE *cache_get(const char *cov, uint32_t w, uint32_t h, uint32_t stride, int *rc);
static void ce_put(struct CE *e);
static void ce_hold(struct CE *e);
static void cache_reset(void);
static const uint8_t *ce_px(struct CE *e);
/* Browse and Now Playing images do not call ArtworkThumb::Load. They ask the library's
 * GetThumbLocation (0x08133980) for a file, offset and size of raw RGB565, then seek (0x0826d8e0)
 * and read (0x0826d5fc) that file. For a FLAC album the answer is the album's cover file with a
 * marker offset. Once the cache seeks to the marker, the stream is bound to a slot and its reads
 * are served from the cover decoded in memory at the requested size. Nothing is written anywhere.
 * Trace (TR words nothing else writes): TR1 GTL calls|ours<<16, TR2 fmt|w<<16, TR4 stage,
 * TR5 seeks bound|reads served<<16, TR6 decode rc|h<<16, TR7 0x80000000 hooked | mismatch bits. */
#define os_img_by_id  ((uint8_t *(*)(void *, uint32_t))0x08045540)
#define os_str_set    ((void (*)(void *, const char *, int))0x0826b6d0)
/* The offset handed back encodes the answer: 0x7 | geometry index (4 bits) | FART block
 * address (24 bits, word aligned in DRAM). Nothing needs recycling, so scrolling requests cannot
 * make a later seek pick up another album or size. */
#define AMARK 0x70000000u
#define NGEO 16
#define NBIND 32
static struct Geo { uint16_t w, h, stride; } geos[NGEO];
static uint32_t ngeo;
static struct Bind { void *stream; const char *cov, *db; struct CE *ce; uint32_t w, h, stride, size, pos, seq, dboff, t; } bind[NBIND];
static uint32_t bind_seq;
static void bind_free(struct Bind *s)
{
    if (s->ce) ce_put(s->ce);
    s->ce = 0; s->db = 0; s->stream = 0;
}
/* a stream Apple stopped reading keeps its cover pinned until the binding is reused: after 5 s
   unread the cover goes back to the cache (a later read finds or decodes it again) */
#define BIND_NOW (*(volatile uint32_t *)0x3c7000b4)   /* timer E, us */
static void bind_sweep(void)
{
    uint32_t now = BIND_NOW, i;
    for (i = 0; i < NBIND; i++)
        if (bind[i].stream && bind[i].ce && now - bind[i].t > 5000000u) { ce_put(bind[i].ce); bind[i].ce = 0; }
}
static int geo_index(uint32_t w, uint32_t h, uint32_t stride)
{
    uint32_t i;
    for (i = 0; i < ngeo; i++) if (geos[i].w == w && geos[i].h == h && geos[i].stride == stride) return i;
    if (ngeo >= NGEO) return -1;
    geos[ngeo].w = w; geos[ngeo].h = h; geos[ngeo].stride = stride;
    return ngeo++;
}
static int art_log;
int log_flush(void);
static int __attribute__((used)) gtl_c(uint32_t *r)
{
    CRUMB();
    uint8_t *img, *th;
    uint32_t *blk, geo[8], tmp[24], h, w, stride, i;
    int g, logging;
    const char *cov;
    TR(1)++;
    TR(20) = (TR(20) & ~0xff0000u) | cur_task() << 16;
    { void theme_dump_views(void); theme_dump_views(); }   /* dumps the views on screen, once, when art is first asked for */
    if (!r[0]) return 0;
    img = os_img_by_id(*(void **)(r[0] + 64), r[1]);
    if (!is_ours_img(img)) return 0;
    TR(1) += 0x10000;
    r[0] = 0;                                         /* "no art" unless we finish */
    th = *(uint8_t **)(img + 36);
    blk = *(uint32_t **)(th + 24);
    os_geo_init(tmp, 0, 0);
    os_geo_get(geo, tmp, (int16_t)r[2]);
    os_geo_free(tmp);
    h = geo[0]; w = geo[1]; stride = geo[2];
    TR(4) = 1;
    /* the first 16 requests are logged (their own count: the finds use art_log) */
    { static int gtl_log; logging = gtl_log < 16; if (logging) gtl_log++; }
    if (logging) {
        log_s("   art: gtl fmt "); log_d((int16_t)r[2]); log_s(" geo "); log_d(w); log_c('x'); log_d(h); log_s(" stride "); log_d(stride);
        log_s(blk[0] == ARTMAGIC3 ? " ithmb" : " jpeg"); log_s(" task "); log_d(cur_task());
    }
    if (!h || !w || w > 640 || h > 640 || stride < w * 2) { if (logging) log_s(" -> bad geometry\n"); return 1; }
    /* The OS never opens the file name given here. Each answer is a stream that the seek and
       read hooks serve; cache_get fills it from covers.db when the size is one of the four stored there. */
    if (blk[0] == ARTMAGIC3) blk = (uint32_t *)blk_jpeg(blk);
    cov = blk_cov(blk);
    if ((g = geo_index(w, h, stride)) < 0) { if (logging) log_s(" -> no geometry slot\n"); return 1; }
    for (i = 0; cov[i]; i++);
    os_str_set((void *)r[3], cov, i);                 /* the album's cover file */
    *(uint32_t *)r[8] = AMARK | (uint32_t)g << 24 | (((uint32_t)blk - 0x08000000u) >> 2);
    *(uint32_t *)r[9] = h * stride;
    r[0] = 1;
    TR(4) = 2;
    if (logging) { log_s(" -> decode "); log_name(cov); log_c('\n'); }
    return 1;
}
static struct Bind *bind_of(void *stream)
{
    int i;
    for (i = 0; i < NBIND; i++) if (bind[i].stream == stream) return &bind[i];
    return 0;
}
/* seek(stream, ?, off_lo, off_hi, whence): 1 = handled (r0 = 0), 0 = run the original */
static int __attribute__((used)) seek_c(uint32_t *r)
{
    CRUMB();
    struct Bind *s;
    uint32_t *blk, g, i;
    bind_sweep();
    s = bind_of((void *)r[0]);
    if (s) bind_free(s);                              /* any seek ends the previous binding */
    if ((r[2] & 0xf0000000u) != AMARK || r[3]) return 0;
    g = (r[2] >> 24) & 15;
    blk = (uint32_t *)(0x08000000u + ((r[2] & 0xffffff) << 2));
    if (g >= ngeo || !is_art_blk(blk)) return 0;
    if (!s) {                                         /* a free binding, else the oldest */
        for (i = 0; i < NBIND && bind[i].stream; i++);
        if (i == NBIND) {
            for (s = &bind[0], i = 1; i < NBIND; i++) if (bind[i].seq < s->seq) s = &bind[i];
            bind_free(s); TR(36) += 0x10000;
        } else s = &bind[i];
    }
    s->stream = (void *)r[0]; s->cov = blk_cov(blk_jpeg(blk)); s->ce = 0;
    s->w = geos[g].w; s->h = geos[g].h; s->stride = geos[g].stride;
    s->size = s->h * s->stride; s->pos = 0; s->seq = ++bind_seq; s->t = BIND_NOW;
    /* 320x320 from covers.db: read straight into the caller's buffer, no cache copy */
    if (blk[0] == ARTMAGIC3 && s->w == 320 && s->h == 320 && s->size == (uint32_t)afmt[1].size
        && artdb_lookup(blk_key(blk), &s->dboff, 0, 0, 0, 0) > 0) { s->db = blk_cov(blk); s->dboff += afmt_off(1); }
    TR(5)++;
    TR(4) = 3;
    r[0] = 0;
    return 1;
}
/* read(stream, len, buf, u32 *got, ...): served from the decoded cover */
static int __attribute__((used)) read_c(uint32_t *r)
{
    CRUMB();
    struct Bind *s = bind_of((void *)r[0]);
    uint32_t n;
    int rc;
    if (!s) return 0;
    s->t = BIND_NOW;
    TR(20) = (TR(20) & ~0xff00u) | cur_task() << 8;
    if (s->db && !s->ce) {
        File *f = open_file(s->db);                  /* per call: a stream Apple drops keeps no handle */
        int got = 0, k;
        n = r[1];
        if (n > s->size - s->pos) n = s->size - s->pos;
        if (f && !f->vt->seek(f, (int64_t)(s->dboff + s->pos), 0))
            while (got < (int)n && (k = f->vt->read(f, (uint8_t *)r[2] + got, n - got, 2)) > 0) got += k;
        if (f) { f->vt->close(f); f->vt->del(f); }
        if (got == (int)n) {
            s->pos += n;
            if (r[3]) *(uint32_t *)r[3] = n;
            TR(5) += 0x10000;
            if (s->pos >= s->size) bind_free(s);
            r[0] = 0;
            return 1;
        }
        s->db = 0;                  /* the file failed: the cache path decodes the cover */
    }
    if (!s->ce) {
        s->ce = cache_get(s->cov, s->w, s->h, s->stride, &rc);
        TR(6) = (TR(6) & 0xffffff00) | rc;
        if (!s->ce) { bind_free(s); if (r[3]) *(uint32_t *)r[3] = 0; r[0] = 5; TR(4) = 0x10 | rc; flac_dump(); return 1; }
        ce_hold(s->ce);             /* keep it while this stream reads */
    }
    n = r[1];
    if (n > s->size - s->pos) n = s->size - s->pos;
    { struct CE *e = s->ce; if (!e) { if (r[3]) *(uint32_t *)r[3] = 0; r[0] = 5; return 1; }   /* swept just now */
      memcpy((void *)r[2], ce_px(e) + s->pos, n); }
    ((void (*)(void))0x0802ce00)();   /* clean D-cache: the reader may hand this to DMA */
    s->pos += n;
    if (r[3]) *(uint32_t *)r[3] = n;
    TR(5) += 0x10000;
    TR(4) = 4;
    if ((TR(5) >> 16) <= 2) flac_dump();
    if (s->pos >= s->size) bind_free(s);
    r[0] = 0;
    return 1;
}
/* Entry hooks. The OS patch puts `b hk_x` over an entry's first instruction (HOOKS.md). cfn
 * gets the saved registers and returns nonzero when it handled the call (its r0 is the result).
 * Otherwise the displaced instruction runs here and the original continues at entry+4. */
#define HOOK(name, cfn, w0, back) \
__attribute__((naked, section(".text.entry"), used)) void name(void) \
{ __asm__ volatile( \
    "push {r0-r5, ip, lr}\n mov r0, sp\n ldr ip, 1f\n mov lr, pc\n bx ip\n" \
    "cmp r0, #0\n pop {r0-r5, ip, lr}\n bxne lr\n" \
    ".word " #w0 "\n ldr pc, 2f\n" \
    "1: .word " #cfn "\n 2: .word " #back "\n"); }
HOOK(hk_gtl, gtl_c, 0xe92d47f0, 0x08133984)
HOOK(hk_seek, seek_c, 0xe92d43f8, 0x0826d8e4)
HOOK(hk_read, read_c, 0xe92d47f0, 0x0826d600)
HOOK(hk_parse, parse_c, 0xe92d43f0, 0x0828a5b0)   /* WAV parser: .flac becomes our reader */

/* One artwork image per album folder, with the folder's id (img+12), shared by its tracks */
/* Apple's image-by-id past its entry hook (hk_imgid) */
__attribute__((naked, section(".text.entry"), used)) static void *img_by_id_orig(void *lib, uint32_t id)
{
    __asm__ volatile(".word 0xe92d41f0\n ldr pc, 1f\n 1: .word 0x08045544\n");
}
/* The album image with the folder's id: made on first use (track: whose dbid it takes, or 0),
   its thumbnails pointed at the folder's current cover block */
static uint8_t *cov_image(void *lib, Cov *c, const uint8_t *track)
{
    uint32_t *blk = c->nat ? c->nat : c->blk;
    uint8_t *img = img_by_id_orig(lib, c->id), *th;
    int i;
    if (!blk) return 0;
    if (img) {
        if (!is_ours_img(img)) return 0;
        for (i = 0; i < 4; i++)
            if ((th = os_thumb_iter(img, 0, afmt[i].fmt))) *(uint32_t *)(th + 24) = (uint32_t)blk;
        return img;
    }
    if (!(img = os_img_new(lib, c->id))) return 0;
    if (track) { *(uint32_t *)(img + 16) = *(uint32_t *)(track + 0x110); *(uint32_t *)(img + 20) = *(uint32_t *)(track + 0x114); }
    for (i = 0; i < 4; i++) {
        th = os_thumb_new(img, afmt[i].fmt, 0);
        if (!th) continue;
        *(uint32_t *)(th + 16) = 0;                       /* vpad, hpad */
        *(uint16_t *)(th + 20) = afmt[i].side;            /* height */
        *(uint16_t *)(th + 22) = afmt[i].side;            /* width */
        *(uint32_t *)(th + 24) = (uint32_t)blk;           /* "ithmb offset": our cover */
        *(uint32_t *)(th + 28) = afmt[i].size;
    }
    return img;
}
/* image by id (0x08045540): one of our album ids gets its image made if the OS asks first */
#define ART_ALIAS 0x01000000u                    /* id - ART_ALIAS: the same album again (Now Playing row) */
static int __attribute__((used)) imgid_c(uint32_t *r)
{
    Cov *c;
    uint32_t id = r[1] - ART_ID0 + ART_ALIAS < ncovs ? r[1] + ART_ALIAS : r[1];
    if (id - ART_ID0 >= ncovs || !r[0] || !(c = cov_by_id(id))) return 0;
    cov_fresh(c);
    r[0] = (uint32_t)cov_image((void *)r[0], c, 0);
    return 1;
}
HOOK(hk_imgid, imgid_c, 0xe92d41f0, 0x08045544)
static void *art_find(void *lib, uint8_t *track)
{
    void *r = os_art_find(lib, track);
    char path[300];
    uint32_t *blk;
    Cov *c;
    int n, d;
    TR(56)++;
    if (r || !lib || !track || *(uint32_t *)(track + 0xf0)) return r;
    n = track_path(track + 0xc0, path, sizeof(path));
    if (n < 6 || path[n - 5] != '.' || (path[n - 4] | 32) != 'f' || (path[n - 1] | 32) != 'c') return 0;
    TR(50)++;
    for (d = n; d > 0 && path[d - 1] != '\\'; d--);
    c = find_cover(path, d); blk = c ? (c->nat ? c->nat : c->blk) : 0;
    if (art_log < 16) { art_log++; log_s("   art: find "); log_name(path);
        log_s(c ? (blk == (uint32_t *)c->nat && c->nat ? " -> covers.db\n" : (blk ? " -> jpeg\n" : " -> no cover\n")) : " -> no album\n"); }
    if (!blk) { TR(54)++; return 0; }
    TR(54) += 0x10000;
    return cov_image(lib, c, track);
}
/* the database load's location of a FLAC track (path.c, after its isflac): its album folder gets an image id */
void art_note(const uint8_t *track)
{
    char path[300];
    int n = track_path(track + 0xc0, path, sizeof(path)), d;
    if (n <= 0) return;
    for (d = n; d > 0 && path[d - 1] != '\\'; d--);
    cov_of(path, d);
}
/* Main menu art pane (0x081ac9b8): image ids per media kind from the tracks' ArtworkDB links;
   FLAC tracks have none, so each album folder with art adds its id to the music list. */
#define os_sc_build  ((void (*)(void *))0x081ac9b8)
#define os_sc_reset  ((void (*)(void *))0x081ac804)                 /* library unload's reset (0x0805de90) */
#define os_sc_lists  ((void (*)(void *, void **))0x081ac710)        /* (pane, &holder): the loader hands its lists over */
#define os_arr_ctor  ((void *(*)(void *, void *, int, int))0x082a035c)
#define os_sp_get    ((void *(*)(void *))0x083cb184)
#define os_ids_add   ((void (*)(void *, int, uint32_t))0x08142254)
static void add_folders(void *ids)
{
    Cov *c;
    int n = 0, k = 0;
    for (c = covs; c; c = c->next, k++) {
        cov_fresh(c);
        if (c->blk) { os_ids_add(ids, 0, c->id); n++; }
    }
    log_s("   art: "); log_d(n); log_s(" of "); log_d(k); log_s(" album folders to the menu art pane\n");
}
/* --ui-lazy: the pane starts with Apple's lists and gets the album folders 0.5 s after the first
   frame, past the boot's heap low point */
static uint8_t *sc_late;
static uint32_t np_shown;                                        /* the album in category 2 (Now Playing row) */
static uint32_t *sc_list[4], sc_nlist[4];                        /* the pane's own lists (the build empties +0xf4) */
static void sc_remember(const void *ids)
{
    int cat;
    for (cat = 0; cat < 4; cat++) {
        const uint32_t *v = (const uint32_t *)((const uint8_t *)ids + 12 * cat);
        uint32_t n = (v[1] - v[0]) / 4;
        if (sc_list[cat]) { os_free(sc_list[cat]); sc_list[cat] = 0; }
        sc_nlist[cat] = 0;
        if (n && (sc_list[cat] = os_malloc(4 * n))) { memcpy(sc_list[cat], (void *)v[0], 4 * n); sc_nlist[cat] = n; }
    }
}
/* 4 id lists as the loader makes them, from the pane's own; np: category 2 holds that album instead */
static void *sc_new_lists(uint32_t np)
{
    void *src = os_malloc(48);
    uint32_t i;
    int cat;
    if (!src) return 0;
    os_arr_ctor(src, (void *)0x0802df28, 12, 4);
    for (cat = 0; cat < 4; cat++)
        if (!np || cat != 2) for (i = 0; i < sc_nlist[cat]; i++) os_ids_add(src, cat, sc_list[cat][i]);
    if (np) {
        os_ids_add(src, 2, np);
        if (np - ART_ID0 < ncovs) os_ids_add(src, 2, np - ART_ALIAS);   /* one album alone stays still; two ids keep it panning */
    }
    return src;
}
/* hands the pane new lists as a library reload does: reset, lists, build (own: they become the pane's own) */
static int sc_relist(uint8_t *sc, void *src, int own)
{
    void **holder = os_malloc(12);
    if (!holder) return 0;
    if (own) sc_remember(src);
    holder[0] = src; ((uint32_t *)holder)[1] = 1; ((uint32_t *)holder)[2] = 0;
    os_sc_reset(sc);
    os_sc_lists(sc, (void **)&holder);                           /* takes a reference in state 0, which the reset gives */
    if (os_sp_get(sc + 0xf4) != src) { log_s("   art: the menu art pane refused its lists  <-- CHECK\n"); os_free(holder); return 0; }
    ((uint32_t *)holder)[1]--;                                   /* the pane keeps its own reference */
    os_sc_build(sc);
    np_shown = 0;
    return 1;
}
__attribute__((section(".text.entry"), used, noinline)) void e_showcase(uint8_t *sc)
{
    CRUMB();
    void *ids = os_sp_get(sc + 0xf4);
    extern int pb_finished;
    np_shown = 0;
    if (!ids) { log_s("   art: no art pane list\n"); os_sc_build(sc); return; }
    if (*(const uint32_t *)0x081aeb1c != 0xe3a00003 && !pb_finished) {      /* --ui-lazy (e_pb_done there), pre-build to come */
        sc_remember(ids);                                        /* Apple's lists, the folders added later */
        sc_late = sc;
        log_s("   art: menu art pane lists after the first frame\n");
    } else { add_folders(ids); sc_remember(ids); }
    os_sc_build(sc);
}
/* a pre-build tick 0.5 s after the first frame, or its end (uifast.c, UI task) */
void art_showcase_late(void)
{
    uint8_t *sc = sc_late;
    void *src;
    if (!sc) return;
    sc_late = 0;
    if (!(src = sc_new_lists(0))) return;
    add_folders(src);
    if (!sc_relist(sc, src, 1)) return;
    /* the pane's own change notice (0x081ac540): a listening main menu redoes its hilite */
    (*(void (**)(void *, uint32_t, uint32_t))(*(uint8_t **)sc + 0x58))(sc, 0x55693332, 0x7f82);
    log_s("   art: menu art pane rebuilt at "); log_d((int)(*(volatile uint32_t *)0x3c7000b4 / 1000)); log_s(" ms, item "); log_d(*(int8_t *)(sc + 0xec)); log_s("\n");
    if (*(int8_t *)(sc + 0xec) != -1) ((void (*)(void *))0x081ac010)(sc);   /* the menu picked an item: resume, as its activation does */
}

/* Now Playing row (0x08224c10): with art, the playing album goes in the pane's unused category 2
   and pans like the Music row; without, Apple's no-art layout */
static const char np_pane[] = "MediaContentHilited", np_noart[] = "NoArtNowPlayingHilited";
static const char * __attribute__((used)) np_state_c(void)
{
    uint8_t *sc = ((uint8_t *(*)(void))0x081ad164)();
    uint32_t id = ((uint32_t (*)(void))0x081eca94)();       /* the playing song's image id, 0 without art */
    void *src;
    if (!id || sc_late) return np_noart;
    if (id != np_shown) {                                         /* the pane's own lists stay as they are */
        if (!(src = sc_new_lists(id)) || !sc_relist(sc, src, 0)) return np_noart;
        np_shown = id;
        ((void (*)(void *))0x081ac010)(sc);                       /* resume, as the menu's activation does */
    }
    if (!((int (*)(void *, int))0x081abeb0)(sc, 2)) return np_noart;
    ((void (*)(void *, int))0x081abf00)(sc, 2);
    return np_pane;
}
__attribute__((naked, section(".text.entry"), used)) void e_np_state(void)
{
    __asm__ volatile("push {r0, lr}\n bl np_state_c\n mov r1, r0\n pop {r0, pc}\n");
}

#include "rbjpeg.h"
#define JRBUF 65536
#define USEC (*(volatile uint32_t *)0x3c7000b4)   /* timer E count, 1 MHz under Rockbox; raw ticks here */
/* buffered, DMA-safe file reader for the jpeg decoder */
typedef struct { File *f; uint8_t *b; int pos, len; } JR;
static uint32_t jr_bytes, jr_ticks;
static int jr_fill(JR *r)
{
    uint32_t t = USEC;
    int n = r->f->vt->read(r->f, r->b, JRBUF, 2);
    if (n > 0) dinv(r->b, n);
    jr_ticks += USEC - t; if (n > 0) jr_bytes += n;
    r->pos = 0; r->len = n > 0 ? n : 0;
    return r->len;
}
static int jr_read(void *ctx, void *buf, int n)
{
    JR *r = ctx; int done = 0;
    while (done < n) {
        int k;
        if (r->pos >= r->len && !jr_fill(r)) break;
        k = r->len - r->pos; if (k > n - done) k = n - done;
        if (buf) memcpy((uint8_t *)buf + done, r->b + r->pos, k);
        r->pos += k; done += k;
    }
    return done;
}
static int jr_skip(void *ctx, int n) { return jr_read(ctx, 0, n) == n ? 0 : 1; }

static int decode_tj(const char *path, int w, int h, int stride, uint8_t *dst)
{
    JR r; void *work, *braw; uint32_t *acc; uint16_t *pix; int rc = 9, y;
    uint32_t off, len;
    cov_span(path, &off, &len);
    r.f = open_file(path);
    if (!r.f) return 8;
    if (off && r.f->vt->seek(r.f, (int64_t)off, 0)) { r.f->vt->close(r.f); r.f->vt->del(r.f); return 8; }
    braw = os_malloc(JRBUF + 32); work = os_malloc(9800);
    acc = os_malloc(w * h * 16); pix = os_malloc(w * h * 2);
    if (braw && work && acc && pix) {
        r.b = (uint8_t *)(((uintptr_t)braw + 31) & ~(uintptr_t)31); r.pos = r.len = 0;
        memset(acc, 0, w * h * 16);
        { int bst = boost_begin();
          rc = cover_decode(jr_read, jr_skip, &r, w, h, pix, work, acc);
          boost_end(bst); }
        if (!rc) for (y = 0; y < h; y++) {
            memcpy(dst + y * stride, pix + y * w, w * 2);
            if (stride > w * 2) memset(dst + y * stride + w * 2, 0, stride - w * 2);
        }
    }
    if (braw) os_free(braw);
    if (work) os_free(work);
    if (acc) os_free(acc);
    if (pix) os_free(pix);
    r.f->vt->close(r.f); r.f->vt->del(r.f);
    return rc;
}

/* Rockbox-style jpeg decode: the whole file is read, then decoded and scaled in RAM. */
#define JPG_MAX (2 << 20)   /* bigger files go to TJpgDec instead, which keeps the heap free for audio */
static int decode_file(const char *path, int w, int h, int stride, uint8_t *dst, int tj_ok)
{
    File *f; uint8_t *raw = 0, *buf; void *work = 0; int rc = 8, n, len = 0, sz;
    uint32_t start = USEC, t;
    uint32_t off, plen;
    cov_span(path, &off, &plen);
    f = open_file(path);
    if (!f) return 8;
    { int a = f->vt->s6(f), b = f->vt->s7(f); sz = a > b ? a : b; }
    if (plen) { sz = (int)plen; if (f->vt->seek(f, (int64_t)off, 0)) { rc = 0x23; goto out; } }
    if (sz <= 0 || sz > JPG_MAX) { rc = 0x20; goto out; }
    raw = os_malloc(((sz + 31) & ~31) + 64);
    if (!raw) { rc = 0x21; goto out; }
    buf = (uint8_t *)(((uintptr_t)raw + 31) & ~(uintptr_t)31);
    t = USEC;
    while (len < sz) {
        int want = sz - len; if (want > 0x40000) want = 0x40000;
        n = f->vt->read(f, buf + len, (want + 31) & ~31, 2);
        if (n <= 0) break;
        len += n;
    }
    if (len > sz) len = sz;
    dinv(buf, (len + 31) & ~31);
    jr_ticks = USEC - t; jr_bytes = len;
    work = os_malloc(RBJPEG_WORK_SIZE_FOR(w, 8192));
    if (!work) { rc = 0x22; goto out; }
    { int bst = boost_begin();
      rc = rbjpeg_decode(buf, len, w, h, stride, dst, work, RBJPEG_WORK_SIZE_FOR(w, 8192));
      boost_end(bst); }
    if (rc) rc = 0x40 | (-rc & 0x3f);
out:
    if (work) os_free(work);
    if (raw) os_free(raw);
    f->vt->close(f); f->vt->del(f);
    if (rc) { TR(6) = (TR(6) & ~0xff) | (rc & 0xff); if (tj_ok) rc = decode_tj(path, w, h, stride, dst); }
    TR(2) = USEC - start;
    TR(52) += TR(2) / 1000;                                       /* all cover decoding, ms */
    TR(36) = (TR(36) & 0xffff0000) | ((TR(36) + 1) & 0xffff);   /* decodes | bindings evicted<<16 */
    if (TR(2) > TR(37)) TR(37) = TR(2);                          /* slowest decode */
    TR(7) = (TR(7) & 0x80000000) | (jr_ticks & 0x7fffffff);
    TR(6) = (TR(6) & 0xffff) | (jr_bytes >> 10) << 16;
    return rc;
}

/* ---------- decoded covers, kept in RAM only ----------
 * Big sizes (Now Playing, Cover Flow) decode once per album at MASTER x MASTER (or at the requested
 * size, if that is bigger), and smaller ones are scaled from it. Huffman decoding costs the same
 * at any output size. Least recently used entries are dropped past CACHE_MAX bytes. */
typedef struct CE { struct CE *next; const char *cov; uint32_t w, h, stride, size, used, refs; uint8_t px[]; } CE;
#define CACHE_MAX (1024 * 1024)
#define MASTER 320
#define MASTER_MIN 100   /* list thumbnails (55 px) decode alone: a master for each would evict the cache within a
                          * few albums of scrolling */
static CE *cache;
static uint32_t cache_bytes, cache_tick;
static void box_scale(const CE *src, uint32_t w, uint32_t h, uint32_t stride, uint8_t *dst)
{
    uint32_t x, y, sx, sy, x0, x1, y0, y1, r, g, b, n;
    for (y = 0; y < h; y++) {
        uint16_t *o = (uint16_t *)(dst + y * stride);
        y0 = y * src->h / h; y1 = (y + 1) * src->h / h; if (y1 <= y0) y1 = y0 + 1;
        for (x = 0; x < w; x++) {
            x0 = x * src->w / w; x1 = (x + 1) * src->w / w; if (x1 <= x0) x1 = x0 + 1;
            r = g = b = n = 0;
            for (sy = y0; sy < y1; sy++) {
                const uint16_t *p = (const uint16_t *)(src->px + sy * src->stride);
                for (sx = x0; sx < x1; sx++) { uint16_t c = p[sx]; r += c >> 11; g += (c >> 5) & 63; b += c & 31; n++; }
            }
            if (dither565) {
                uint32_t t = dith_t(x, y);
                o[x] = (uint16_t)(((r << 8) / n + t) >> 8 << 11 | ((g << 8) / n + t) >> 8 << 5 | ((b << 8) / n + t) >> 8);
            } else
            o[x] = (uint16_t)((r + n / 2) / n << 11 | (g + n / 2) / n << 5 | (b + n / 2) / n);   /* rounded */
        }
        if (stride > w * 2) memset(dst + y * stride + w * 2, 0, stride - w * 2);
    }
}
static void cache_trim(void)
{
    while (cache_bytes > CACHE_MAX) {
        CE **pp, **lru = 0;
        for (pp = &cache; *pp; pp = &(*pp)->next)
            if (!(*pp)->refs && (!lru || (*pp)->used < (*lru)->used)) lru = pp;
        if (!lru) return;
        { CE *e = *lru; *lru = e->next; cache_bytes -= e->size; os_free(e); }
    }
}
/* covers the Rockbox decoder can't take (progressive, too big) get no master: TJpgDec runs per
 * size, since at master size it would need w*h*16 bytes of heap. */
static const char *nomaster[8];
static uint32_t nomaster_n;
/* TR53 = ms timestamp when a Now Playing size cover was last first served (compare with
 * TR30, the last track open). One log per such cover, at most every 20 s. */
static const char *last_big;
static void big_served(CE *e)
{
    uint32_t now = USEC / 1000;
    if (e->cov == last_big) return;
    last_big = e->cov;
    if (now - TR(53) > 20000) { TR(53) = now; flac_dump(); } else TR(53) = now;
}
/* new cache entry, scaled from big or decoded from the jpeg */
static CE *ce_new(const char *cov, uint32_t w, uint32_t h, uint32_t stride, const CE *big, int *rc)
{
    CE *e = os_malloc(sizeof(CE) + h * stride);
    if (!e) { *rc = 9; return 0; }
    e->cov = cov; e->w = w; e->h = h; e->stride = stride; e->size = sizeof(CE) + h * stride; e->refs = 0;
    if (big) { box_scale(big, w, h, stride, e->px); *rc = 0; TR(6) += 0x100; }
    else {
        uint32_t r31;
        gs_init(); if (gs.reading) TR(21)++;          /* art decode started inside an audio read */
        gs.arting++; r31 = TR(31);
        *rc = decode_file(cov, w, h, stride, e->px, art_gen || w != MASTER || h != MASTER || stride != MASTER * 2);
        gs.arting--; if (TR(31) != r31) TR(21) += 0x10000;   /* audio reads ran during it */
    }
    if (*rc) { os_free(e); return 0; }
    e->used = ++cache_tick;
    e->next = cache; cache = e; cache_bytes += e->size;
    return e;
}
/* the album's covers.db block for this size, if it has one: no decoding */
static CE *ce_from_ithmb(const char *cov, uint32_t w, uint32_t h, uint32_t stride)
{
    Cov *c; CE *e; File *f; int k, got = 0; uint32_t pixoff;
    for (c = covs; c && (!c->blk || blk_cov(c->blk) != cov); c = c->next);
    if (!c || !c->nat || artdb_lookup(c->key, &pixoff, 0, 0, 0, 0) <= 0) return 0;
    for (k = 0; k < 4; k++) if (w == (uint32_t)afmt[k].side && h == (uint32_t)afmt[k].side && stride * h == (uint32_t)afmt[k].size) break;
    if (k == 4) return 0;
    if (!(f = open_file(blk_cov(c->nat)))) return 0;
    e = os_malloc(sizeof(CE) + h * stride);
    if (e && !f->vt->seek(f, (int64_t)(pixoff + afmt_off(k)), 0))
        while (got < afmt[k].size) { int n = f->vt->read(f, e->px + got, afmt[k].size - got, 2); if (n <= 0) break; got += n; }
    f->vt->close(f); f->vt->del(f);
    if (!e) return 0;
    if (got != afmt[k].size) { os_free(e); return 0; }
    e->cov = cov; e->w = w; e->h = h; e->stride = stride; e->size = sizeof(CE) + h * stride; e->refs = 0;
    e->used = ++cache_tick;
    e->next = cache; cache = e; cache_bytes += e->size;
    TR(6) += 0x10000;
    return e;
}
/* A cache entry for cover at this size, decoded or scaled if needed; 0 with *rc set on failure. */
static CE *cache_get(const char *cov, uint32_t w, uint32_t h, uint32_t stride, int *rc)
{
    CE *e, *big = 0;
    for (e = cache; e; e = e->next) {
        if (e->cov != cov) continue;
        if (e->w == w && e->h == h && e->stride == stride) { e->used = ++cache_tick; *rc = 0; return e; }
        if (e->w >= w && e->h >= h && (!big || e->w < big->w)) big = e;
    }
    if ((e = ce_from_ithmb(cov, w, h, stride))) { *rc = 0; e->refs++; cache_trim(); e->refs--; return e; }
    if (!big && w >= MASTER_MIN && h >= MASTER_MIN && w <= MASTER && h <= MASTER && (w < MASTER || h < MASTER)) {
        uint32_t i;
        for (i = 0; i < 8 && nomaster[i] != cov; i++);
        if (i == 8 && !(big = ce_new(cov, MASTER, MASTER, MASTER * 2, 0, rc)))
            nomaster[nomaster_n++ & 7] = cov;               /* decode the size asked for instead */
    }
    e = ce_new(cov, w, h, stride, big, rc);
    if (e) { e->refs++; cache_trim(); e->refs--; }
    if (e && w >= MASTER_MIN) big_served(e);
    return e;
}
static int decode_cover(const char *path, int w, int h, int stride, uint8_t *dst)
{
    int rc;
    CE *e = cache_get(path, w, h, stride, &rc);
    if (e) memcpy(dst, e->px, h * stride);
    return rc;
}
static void ce_put(CE *e) { if (e->refs) e->refs--; }
static void ce_hold(CE *e) { e->refs++; }
static void cache_reset(void) { cache = 0; cache_bytes = 0; cache_tick = 0; nomaster_n = 0; memset(nomaster, 0, sizeof(nomaster)); last_big = 0; }
/* drop every entry no stream is reading */
static void cache_flush(void)
{
    CE **pp = &cache;
    while (*pp) {
        CE *e = *pp;
        if (e->refs) { pp = &e->next; continue; }
        *pp = e->next; cache_bytes -= e->size; os_free(e);
    }
    nomaster_n = 0; memset(nomaster, 0, sizeof(nomaster)); last_big = 0;
}
static const uint8_t *ce_px(CE *e) { return e->px; }

static void art_load(uint8_t *t, void *alloc)
{
    uint8_t *th = *(uint8_t **)(t + 20), *buf, *hdr;
    uint32_t *blk, size, geo[8], tmp[24];
    if (th) {
        blk = *(uint32_t **)(th + 24);
        if (((uint32_t)blk & 3) || ((uint32_t)blk >> 26) != 2 || !is_art_blk(blk)) th = 0;
    }
    if (!th) { os_art_load(t, alloc); return; }
    TR(52)++;
    if (*(void **)(t + 4)) { TR(53) = 0xee; return; }
    size = *(uint32_t *)(th + 28);
    buf = 0;
    if (alloc && (buf = os_alloc_a(alloc, size + 44, 0, 1))) {
        *(uint8_t **)(t + 8) = buf; *(void **)(t + 16) = alloc; t[12] = 1;
    } else if (!(buf = *(uint8_t **)(t + 8))) {
        buf = os_alloc_b(size + 44, 1);
        *(uint8_t **)(t + 8) = buf; t[12] = 0;
        if (!buf) return;
    }
    hdr = (uint8_t *)((((uintptr_t)buf + 43) & ~(uintptr_t)15) - 28);
    os_geo_init(tmp, 0, 0);
    os_geo_get(geo, tmp, *(int16_t *)(t + 24));
    os_geo_free(tmp);
    if (!geo[0] || !geo[1] || geo[2] * geo[0] > size || geo[2] < geo[1] * 2) { TR(53) = 7 | geo[1] << 8 | geo[0] << 20; flac_dump(); return; }
    if (blk[0] == ARTMAGIC3) {
        int k = afmt_index(*(int16_t *)(t + 24));
        File *f = 0;
        int rc = 1;
        uint32_t pixoff = 0;
        if (k >= 0 && geo[1] == (uint32_t)afmt[k].side && geo[0] == (uint32_t)afmt[k].side && geo[2] * geo[0] == (uint32_t)afmt[k].size
            && artdb_lookup(blk_key(blk), &pixoff, 0, 0, 0, 0) > 0 && (f = open_file(blk_cov(blk)))) {
            int got = 0;
            if (!f->vt->seek(f, (int64_t)(pixoff + afmt_off(k)), 0)) {
                while (got < afmt[k].size) { int n = f->vt->read(f, hdr + 28 + got, afmt[k].size - got, 2); if (n <= 0) break; got += n; }
                rc = got == afmt[k].size ? 0 : 2;
            }
            f->vt->close(f); f->vt->del(f);
        }
        if (rc) rc = decode_cover(blk_cov(blk_jpeg(blk)), geo[1], geo[0], geo[2], hdr + 28);
        TR(53) = rc | geo[1] << 8 | geo[0] << 20;
    } else
    TR(53) = decode_cover(blk_cov(blk), geo[1], geo[0], geo[2], hdr + 28) | geo[1] << 8 | geo[0] << 20;
    if (TR(52) <= 3) flac_dump();
    if (TR(53) & 0xff) return;
    ((uint32_t *)hdr)[2] = 0; ((uint32_t *)hdr)[3] = 0;
    ((uint32_t *)hdr)[4] = geo[0]; ((uint32_t *)hdr)[5] = geo[1];
    ((uint16_t *)hdr)[2] = geo[2]; ((uint16_t *)hdr)[3] = geo[6];
    ((uint16_t *)hdr)[0] = 0x565;
    ((uint32_t *)hdr)[6] = size;
    ((void (*)(void))0x0802ce00)();   /* clean D-cache: pixels go to the LCD by DMA */
    *(uint8_t **)(t + 4) = hdr;
}

/* Modern style Now Playing. Its steady-state frame comes from 0x0815c588: the cover render
 * 0x082706d0 draws the album cover (flat: e_cover_angle) into a full-screen 320x240 RGB565 buffer,
 * then 0x08268b7c draws Apple's edge shading for the 9-degree tilted cover over it (two slanted
 * lines, (9, 42.7)-(145, 32.3) and (9, 162.5)-(145, 168.2)) and the buffer goes to the display. In
 * Modern the shading is skipped and the frame is laid out again from the cover Apple just drew at
 * (18, 38): a blurred, darkened copy of it fills the screen and the cover is drawn back in place
 * with rounded corners. Other paths (the flip and transition animations, 0x08147a50) are left alone. */
#define os_cover_present ((void (*)(uint32_t *))0x08268b7c)
extern int theme_style;
#define NP_SIDE 128
#define NP_X 18                                 /* where Apple's flat cover lands */
#define NP_Y 38
#define NP_R 12                                 /* corner radius */
static uint16_t *np_bg;                         /* 320x240 blurred background */
static uint16_t rgb565(int r, int g, int b) { return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); }
static void unpack565(uint16_t c, int *r, int *g, int *b) { *r = (c >> 11) << 3; *g = ((c >> 5) & 63) << 2; *b = (c & 31) << 3; }
static uint16_t mix565(uint16_t f, uint16_t o, int a)    /* a 0..256 */
{
    int fr, fg, fb, orr, og, ob;
    unpack565(f, &fr, &fg, &fb); unpack565(o, &orr, &og, &ob);
    return rgb565(orr + ((fr - orr) * a >> 8), og + ((fg - og) * a >> 8), ob + ((fb - ob) * a >> 8));
}
/* the cover, scaled to fill 320 wide (rows 16..111 of the 128), averaged into 40x30 cells, box
   blurred twice, darkened to 40 %, then bilinear back to 320x240 */
extern uint8_t theme_pal_white[4];
static void np_make_bg(const uint8_t *cov565)
{
    static int16_t g[3][30][40];
    int x, y, c, i, j, k;
    if (!np_bg && !(np_bg = os_malloc(320 * 240 * 2))) return;
    for (y = 0; y < 30; y++) for (x = 0; x < 40; x++) {
        int sr = 0, sg = 0, sb = 0, n = 0;
        for (j = 16 + y * 96 / 30; j < 16 + (y + 1) * 96 / 30; j++)
            for (i = x * 128 / 40; i < (x + 1) * 128 / 40; i++) {
                int r, gg, b; unpack565(cov565[2 * (j * 128 + i)] | cov565[2 * (j * 128 + i) + 1] << 8, &r, &gg, &b);
                sr += r; sg += gg; sb += b; n++;
            }
        if (!n) n = 1;
        g[0][y][x] = sr / n; g[1][y][x] = sg / n; g[2][y][x] = sb / n;
    }
    for (k = 0; k < 2; k++) for (c = 0; c < 3; c++) {
        static int16_t t[30][40];
        for (y = 0; y < 30; y++) for (x = 0; x < 40; x++) {
            int s = 0, n = 0;
            for (j = y - 1; j <= y + 1; j++) for (i = x - 1; i <= x + 1; i++)
                if (j >= 0 && j < 30 && i >= 0 && i < 40) { s += g[c][j][i]; n++; }
            t[y][x] = s / n;
        }
        memcpy(g[c], t, sizeof t);
    }
    /* the bar colour in 8.8, rounded up so a full fade dithers to exactly its RGB565 value */
    uint16_t bar = rgb565(theme_pal_white[0], theme_pal_white[1], theme_pal_white[2]);
    uint32_t bt[3] = { ((bar >> 11) * 0x1000000u + 7967) / 7968, (((bar >> 5) & 63) * 0x1000000u + 16191) / 16192,
                       ((bar & 31) * 0x1000000u + 7967) / 7968 };
    int theme_dark(void);
    int lift = theme_dark() ? 0 : 153;          /* Light: 40% cover over white, so dark text reads; Dark: 40% over black */
    for (y = 0; y < 240; y++) {
        int fy = y * 29 * 256 / 239, y0 = fy >> 8, wy = fy & 255, y1 = y0 < 29 ? y0 + 1 : 29;
        int fade = y < 20 ? 256 : y < 52 ? 256 - (y - 20) * 256 / 32 : 0;
        for (x = 0; x < 320; x++) {
            int fx = x * 39 * 256 / 319, x0 = fx >> 8, wx = fx & 255, x1 = x0 < 39 ? x0 + 1 : 39, v[3];
            for (c = 0; c < 3; c++) {
                int top = g[c][y0][x0] * (256 - wx) + g[c][y0][x1] * wx, bot = g[c][y1][x0] * (256 - wx) + g[c][y1][x1] * wx;
                if (!dither565) { v[c] = ((top * (256 - wy) + bot * wy) >> 16) * 40 / 100 + lift; continue; }
                v[c] = ((top * (256 - wy) + bot * wy) >> 8) * 40 / 100 + (lift << 8);   /* 8.8 */
                v[c] += ((int)bt[c] - v[c]) * fade >> 8;
            }
            np_bg[y * 320 + x] = dither565 ? dith565_q8(v[0], v[1], v[2], dith_t(x, y)) : rgb565(v[0], v[1], v[2]);
        }
    }
    /* the top fades into the status bar's colour (the page colour) */
    if (!dither565) {
        for (y = 0; y < 52; y++) {
            int a = y < 20 ? 256 : 256 - (y - 20) * 256 / 32;
            for (x = 0; x < 320; x++) np_bg[y * 320 + x] = mix565(bar, np_bg[y * 320 + x], a);
        }
    }
}
__attribute__((section(".text.entry"), used, noinline)) void e_np_present(uint32_t *t)
{
    CRUMB();
    static uint16_t src[NP_SIDE * NP_SIDE];
    static uint32_t src_sum;
    static int bg_gen = -1;
    extern int theme_accent;
    void theme_cover(const uint16_t *p, int stride, int w, int h);
    const uint16_t *wh = (const uint16_t *)(t + 1);
    int x, y, i, bar = theme_pal_white[0] << 16 | theme_pal_white[1] << 8 | theme_pal_white[2];
    uint32_t sum = 0;
    if (theme_style && ((t[0] >> 26) == 2) && wh[0] == 320 && wh[1] == 240) {
        uint16_t *buf = (uint16_t *)t[0];
        for (y = 0; y < NP_SIDE; y++)                   /* the cover as Apple drew it */
            memcpy(src + y * NP_SIDE, buf + (NP_Y + y) * 320 + NP_X, NP_SIDE * 2);
        for (i = 0; i < NP_SIDE * NP_SIDE; i += 97) sum = sum * 31 + src[i];
        if (sum != src_sum || bg_gen != bar) theme_cover(src, NP_SIDE, NP_SIDE, NP_SIDE);   /* new cover or theme */
        if (!np_bg || sum != src_sum || bg_gen != bar) { np_make_bg((const uint8_t *)src); src_sum = sum; bg_gen = bar; }   /* its top fades into the bar colour */
        if (np_bg) memcpy(buf, np_bg, 320 * 240 * 2);
        for (y = 0; y < NP_SIDE; y++) {                 /* the cover, corners rounded over the background */
            uint16_t *d = buf + (NP_Y + y) * 320 + NP_X;
            const uint16_t *sp = src + y * NP_SIDE;
            int cy = y < NP_R ? NP_R : y > NP_SIDE - 1 - NP_R ? NP_SIDE - 1 - NP_R : y;
            for (x = 0; x < NP_SIDE; x++) {
                int cx = x < NP_R ? NP_R : x > NP_SIDE - 1 - NP_R ? NP_SIDE - 1 - NP_R : x;
                if (cx == x || cy == y) { d[x] = sp[x]; continue; }
                {
                    int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy, r2o = (NP_R * 16 + 8) * (NP_R * 16 + 8) / 256, r2i = (NP_R * 16 - 8) * (NP_R * 16 - 8) / 256;
                    if (d2 <= r2i) d[x] = sp[x];
                    else if (d2 < r2o) d[x] = mix565(sp[x], d[x], (r2o - d2) * 256 / (r2o - r2i));
                }
            }
        }
        ((void (*)(void))0x0802ce00)();     /* clean D-cache before the buffer goes to the display */
        return;
    }
    if (theme_accent == 8 && ((t[0] >> 26) == 2) && wh[0] == 320 && wh[1] == 240) {   /* Classic, Album accent: the tilted cover's middle */
        const uint16_t *mid = (const uint16_t *)t[0] + 52 * 320 + 30;
        for (i = 0; i < 96 * 96; i += 97) sum = sum * 31 + mid[i / 96 * 320 + i % 96];
        if (sum != src_sum) { theme_cover(mid, 320, 96, 96); src_sum = sum; }
    }
    os_cover_present(t);
}
