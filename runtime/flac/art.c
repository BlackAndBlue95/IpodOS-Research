/* Album art for FLAC tracks (no ArtworkDB entry). art_find wraps the OS FindImageForTrack
 * (0x080457e4) and points thumbnails at the album's cover; art_load wraps ArtworkThumb::Load
 * (0x080f32ec) and decodes that cover to the requested size. Nothing is written to the iPod. */
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
void log_s(const char *s); void log_d(int v); void log_c(char c); void log_name(const char *s);
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
typedef struct Cov { struct Cov *next; uint32_t *blk, *nat; uint64_t key; uint32_t gen; char dir[1]; } Cov;
static Cov *covs;

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
static Cov *find_cover(const char *path, int dlen)
{
    Cov *c;
    for (c = covs; c; c = c->next)
        if (!mcmp(c->dir, path, dlen) && !c->dir[dlen]) {
            if (c->gen != covs_gen) resolve_cover(c, path, dlen);
            return c;
        }
    c = os_malloc(sizeof(Cov) + dlen + 1);
    if (!c) return 0;
    memcpy(c->dir, path, dlen); c->dir[dlen] = 0;
    c->key = artdb_key(c->dir);
    resolve_cover(c, path, dlen);
    c->next = covs; covs = c;
    return c;
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
static struct Bind { void *stream; const char *cov; struct CE *ce; uint32_t w, h, stride, size, pos, seq; } bind[NBIND];
static uint32_t bind_seq;
static void bind_free(struct Bind *s) { if (s->ce) ce_put(s->ce); s->ce = 0; s->stream = 0; }
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
    /* the first 16 requests are logged */
    logging = art_log < 16;
    if (logging) {
        art_log++;
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
    struct Bind *s;
    uint32_t *blk, g, i;
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
    s->size = s->h * s->stride; s->pos = 0; s->seq = ++bind_seq;
    TR(5)++;
    TR(4) = 3;
    r[0] = 0;
    return 1;
}
/* read(stream, len, buf, u32 *got, ...): served from the decoded cover */
static int __attribute__((used)) read_c(uint32_t *r)
{
    struct Bind *s = bind_of((void *)r[0]);
    uint32_t n;
    int rc;
    if (!s) return 0;
    TR(20) = (TR(20) & ~0xff00u) | cur_task() << 8;
    if (!s->ce) {
        s->ce = cache_get(s->cov, s->w, s->h, s->stride, &rc);
        TR(6) = (TR(6) & 0xffffff00) | rc;
        if (!s->ce) { bind_free(s); if (r[3]) *(uint32_t *)r[3] = 0; r[0] = 5; TR(4) = 0x10 | rc; flac_dump(); return 1; }
        ce_hold(s->ce);             /* keep it while this stream reads */
    }
    n = r[1];
    if (n > s->size - s->pos) n = s->size - s->pos;
    memcpy((void *)r[2], ce_px(s->ce) + s->pos, n);
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

static void *art_find(void *lib, uint8_t *track)
{
    void *r = os_art_find(lib, track);
    char path[300];
    const char *cov;
    uint8_t *img, *th;
    uint32_t *blk;
    int n, d, i, cl;
    TR(56)++;
    if (r || !lib || !track || *(uint32_t *)(track + 0xf0)) return r;
    n = track_path(track + 0xc0, path, sizeof(path));
    if (n < 6 || path[n - 5] != '.' || (path[n - 4] | 32) != 'f' || (path[n - 1] | 32) != 'c') return 0;
    TR(50)++;
    for (d = n; d > 0 && path[d - 1] != '\\'; d--);
    { Cov *c = find_cover(path, d); blk = c ? (c->nat ? c->nat : c->blk) : 0;
      if (art_log < 16) { art_log++; log_s("   art: find "); log_name(path); log_s(c ? (blk == (uint32_t *)c->nat && c->nat ? " -> covers.db\n" : (blk ? " -> jpeg\n" : " -> no cover\n")) : " -> no album\n"); } }
    if (!blk) { TR(54)++; return 0; }
    (void)cov; (void)cl;
    img = os_img_new(lib, 0);
    if (!img) return 0;
    *(uint32_t *)(img + 16) = *(uint32_t *)(track + 0x110);
    *(uint32_t *)(img + 20) = *(uint32_t *)(track + 0x114);
    for (i = 0; i < 4; i++) {
        th = os_thumb_new(img, afmt[i].fmt, 0);
        if (!th) continue;
        *(uint32_t *)(th + 16) = 0;                       /* vpad, hpad */
        *(uint16_t *)(th + 20) = afmt[i].side;            /* height */
        *(uint16_t *)(th + 22) = afmt[i].side;            /* width */
        *(uint32_t *)(th + 24) = (uint32_t)blk;           /* "ithmb offset": our cover */
        *(uint32_t *)(th + 28) = afmt[i].size;
    }
    TR(54) += 0x10000;
    return img;
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
