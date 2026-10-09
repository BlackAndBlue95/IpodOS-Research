/* Native FLAC playback for the iPod Classic OS: a FLAC reader and decoder that take the place
 * of the OS WAV reader and PCM decoder for .flac tracks. Freestanding; all memory is from the OS heap. */
#include <stdint.h>
#include <stddef.h>

typedef struct File File;
struct FileVT {
    void *(*dtor)(File *);
    void  (*del)(File *);
    int   (*open)(File *);
    int   (*close)(File *);
    int   (*read)(File *, void *, int, int);
    int   (*seek)(File *, int64_t, int);
    int   (*s6)(File *);
    int   (*s7)(File *);
    int   (*s8)(File *);
};
struct File { const struct FileVT *vt; };

#include "rbflac.h"
#include "rbfir.h"
#include "fir.h"

#define MAXCH 8
#define FIRMAX 128
#define CHUNK 4096            /* output frames per post (16 KB of stereo frames: the output ring's chunk) */
#define SCR (128 * 1024)      /* reader scratch window for seeks and the truncation check */
#define SEEK_MAX 2048
#define OBJ_MAGIC 0x434c4652  /* 'RFLC' at obj+0x414, our state pointer at obj+0x418 */
#ifdef HOSTTEST
#define OBJ_FILE 0x08              /* 64-bit host pointers must not run into the path at +0x14 */
#else
#define OBJ_FILE 0x10              /* the WAV reader object's File pointer */
#endif
#define FD_MAGIC  0x44434c46  /* 'FLCD' */

/* ---------- OS glue ---------- */
#ifdef HOSTTEST
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
File *host_file_ctor(void *mem, const char *path, int a, int b);
#define os_malloc malloc
#define os_free free
#define os_file_ctor host_file_ctor
#define flac_dump() ((void)0)
#include <time.h>
uint32_t host_usec;
static uint32_t host_now(void)
{
    struct timespec t;
#ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000000u + t.tv_nsec / 1000u);
#else
    (void)t; return host_usec++;
#endif
}
#define HTIMER host_now()
static uint32_t cur_task(void) { return 1; }
static void dinv(const void *p, int n) { (void)p; (void)n; }
static void os_strncpy(void *d, const char *s, int n) { strncpy(d, s, n); }
/* host stand-ins for the player, see hostr.c */
int host_src_get(void *trk, int *len, uint8_t **ptr, uint32_t *ms, uint32_t *fl);
void host_src_rel(void *trk, int len, uint8_t *ptr, uint32_t ms);
int host_out_get(void **buf, int keep);
void host_out_post(void *buf, int frames, uint32_t ms, int flags, void *trk);
void *host_cur_track(void);
#define src_get host_src_get
#define src_rel host_src_rel
#define out_get host_out_get
#define out_post host_out_post
#define CUR_TRACK host_cur_track()
static void os_kick(void) {}
static void os_end_signal(void) {}
static int os_wav_close(void *obj) { File *f = *(File **)((uint8_t *)obj + OBJ_FILE); if (f) { f->vt->close(f); f->vt->del(f); } *(File **)((uint8_t *)obj + OBJ_FILE) = 0; return 0; }
static void *os_wav_dtor(void *obj) { os_wav_close(obj); return obj; }
static void os_wav_del(void *obj) { os_wav_dtor(obj); free(obj); }
#else
#define os_malloc ((void *(*)(unsigned))0x0829fefc)
#define os_free   ((void (*)(void *))0x0829fe4c)
#define os_file_ctor ((File *(*)(void *, const char *, int, int))0x081ea554)
void flac_dump(void);                            /* region E (log.c), debug builds only */
#define HTIMER (*(volatile uint32_t *)0x3c7000b4)    /* timer E count, 1 MHz under Rockbox */
#define os_strncpy ((void (*)(void *, const char *, int))0x080261fc)
/* RTXC globals at 0x2200acf0: +4 is the running task's TCB, and TCB+32 its task number */
static uint32_t cur_task(void)
{
    uint32_t t = *(volatile uint32_t *)0x2200acf4;
    if ((t & 3) || ((t >> 24) != 0x22 && (t >> 26) != 2)) return 0xff;
    return *(volatile uint32_t *)(t + 32) & 0xff;
}
static void dinv(const void *p, int n)   /* clean + invalidate the D-cache lines of a DMA buffer */
{
    uintptr_t a = (uintptr_t)p & ~31u, e = (uintptr_t)p + n;
    for (; a < e; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a) : "memory");
}
/* the player: packet source singleton and the output buffer manager */
#define os_source   ((void *(*)(void))0x081ff130)
#define os_outmgr   ((void *(*)(void))0x08201460)
#define os_out_get  ((int (*)(void *, int, void **, int))0x08201518)
#define os_out_post ((void (*)(void *, int, int, void *, int, int, uint32_t, int, void *))0x08201570)
#define os_signal   ((void (*)(uint32_t, int))0x080cfe50)
#define os_sem_give ((void (*)(uint32_t, int))0x080bb884)
#define os_pcm_dec  ((void *(*)(void))0x081a4900)      /* the PCM pass-through decoder singleton */
#define CUR_TRACK   (*(void *volatile *)0x08a09d28)    /* track the decoder is being set up for */
#define os_wav_close ((int (*)(void *))0x0828a9c0)
#define os_wav_dtor  ((void *(*)(void *))0x0828aadc)
#define os_wav_del   ((void (*)(void *))0x0828aac4)
typedef int (*src_get_t)(void *, void *, int, int, int *, uint8_t **, uint32_t *, uint32_t *);
typedef void (*src_rel_t)(void *, void *, int, int, uint8_t *, uint32_t);
static int src_get(void *trk, int *len, uint8_t **ptr, uint32_t *ms, uint32_t *fl)
{ void *s = os_source(); return ((src_get_t)(*(void ***)s)[7])(s, trk, 0, 0, len, ptr, ms, fl); }
static void src_rel(void *trk, int len, uint8_t *ptr, uint32_t ms)
{ void *s = os_source(); ((src_rel_t)(*(void ***)s)[8])(s, trk, 0, len, ptr, ms); }
static int out_get(void **buf, int keep) { return os_out_get(os_outmgr(), 0, buf, keep); }
static void out_post(void *buf, int frames, uint32_t ms, int flags, void *trk)
{ os_out_post(os_outmgr(), 0, 0, buf, frames, 0, ms, flags, trk); }
/* bumps the decode task's work count and gives its semaphore, as the PCM decoder does */
static void os_kick(void) { (*(volatile uint32_t *)0x22008cac)++; os_sem_give(0x220106d0, 0); }
static void os_end_signal(void) { os_signal(0x22008cb0, 0); }
void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *a = d; const uint8_t *b = s;
    if ((((uintptr_t)a | (uintptr_t)b) & 3) == 0) {
        uint32_t *wa = (uint32_t *)a; const uint32_t *wb = (const uint32_t *)b;
        while (n >= 16) { wa[0] = wb[0]; wa[1] = wb[1]; wa[2] = wb[2]; wa[3] = wb[3]; wa += 4; wb += 4; n -= 16; }
        while (n >= 4) { *wa++ = *wb++; n -= 4; }
        a = (uint8_t *)wa; b = (const uint8_t *)wb;
    }
    while (n--) *a++ = *b++;
    return d;
}
void *memset(void *d, int c, size_t n)
{ uint8_t *a = d; while (n--) *a++ = c; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *a = d; const uint8_t *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
#endif

uint32_t flac_trace[64];
#define trace_buf flac_trace
#define TR(i) (flac_trace[i])
/* Trace words (art.c owns 1,2,4,5,6,7,20,21,23,36,37,50,52,53,54,56; 8-15 path tail;
 * 60-63 the stub):
 * reader  24 open stage|rc<<8, 25 sr, 26 ch|bps<<8|dec<<16|maxbs<<20, 27 file size, 28 duration ms,
 *         29 reads|seeks<<16, 30 packets, 31 Read calls, 32 bytes delivered, 33 buffer size,
 *         34 aborts|read errors<<16, 35 last seek ms, 38 bytes skipped (no frame), 39 false
 *         syncs rejected, 40 seek points|truncated<<16, 41 opens|closes<<16
 * decoder 42 decode ticks, 43 input samples, 44 frames, 45 failed frames|last err<<16,
 *         46 posts, 47 slowest frame ticks (42-44,47 reset per track), 48 selector calls|ours<<16, 49 inits|0x10000|task<<24,
 *         51 wall ticks Init..end, 55 carried chunks, 57 end posts, 58 out_get failures,
 *         59 hook flags (0x100 selector patched, bits 0-3 hook mismatches), 62 stream from the frame headers: rate|bits<<20|ch<<25|dec<<29 */

static uint32_t ms_of(uint64_t sample, uint32_t sr) { return (uint32_t)(sample * 1000u / sr); }

/* ---------- the FLAC reader (lives behind the OS WAV reader object) ---------- */
typedef struct { uint64_t sample; uint32_t off; } SeekPt;
typedef struct {
    File *f;
    int sr, ch, bps, minbs, maxbs, maxfs;
    uint64_t total;
    uint32_t first_off, fsize, dur_ms, bitrate;
    int och, dec, osr;
    rbflac_stream st;
    uint8_t *scr_raw, *scr;         /* 32-byte aligned scratch window */
    SeekPt *seek; int nseek;
    /* delivery position */
    uint32_t pos;                   /* file offset of the next frame to hand out */
    uint64_t pos_sample;            /* its first sample, when pos_ok */
    int pos_ok, eof;
    uint32_t reads, packets;
} FR;

static FR *fr_of(void *obj)
{
    uint8_t *o = obj;
    if (!o || *(uint32_t *)(o + 0x414) != OBJ_MAGIC) return 0;
    return *(FR **)(o + 0x418);
}
static int rd(File *f, void *b, int n) { return f->vt->read(f, b, n, 2); }

/* read up to n bytes at off into dst (32-byte aligned, DMA safe); 0 at EOF or error */
static int rd_at(FR *w, uint32_t off, uint8_t *dst, int n)
{
    int r;
    if (w->f->vt->seek(w->f, (int64_t)off, 0)) return 0;
    dinv(dst, (n + 31) & ~31);
    r = rd(w->f, dst, n);
    TR(29)++;
    if (r <= 0) return 0;
    dinv(dst, (r + 31) & ~31);
    return r;
}

/* Finds a frame header in b[0..n) whose next header continues it (first sample == sample +
 * blocksize). A header with no follower counts only at end of file. Returns its offset or -1. */
static int find_frame(FR *w, const uint8_t *b, int n, int at_eof, rbflac_frame *h)
{
    int q, q2;
    rbflac_frame h2;
    for (q = 0; q + 2 <= n; q++) {
        if (b[q] != 0xff || (b[q + 1] & 0xfe) != 0xf8) continue;
        if (rbflac_parse_header(&w->st, b + q, n - q, h) <= 0) continue;
        for (q2 = q + h->hdrlen; q2 + 2 <= n; q2++) {
            if (b[q2] != 0xff || (b[q2 + 1] & 0xfe) != 0xf8) continue;
            if (rbflac_parse_header(&w->st, b + q2, n - q2, &h2) <= 0) continue;
            if (h2.sample == h->sample + (uint64_t)h->blocksize) return q;
            if (w->maxfs && q2 - q > w->maxfs + 64) break;   /* too far for one frame: false sync */
        }
        if (at_eof) return q;
    }
    return -1;
}

/* make w->pos point at a confirmed frame header (after a seek or a bad spot) */
static int fr_locate(FR *w)
{
    rbflac_frame h;
    uint32_t off = w->pos;
    while (off < w->fsize) {
        int n = rd_at(w, off, w->scr, SCR), q;
        if (n <= 0) break;
        q = find_frame(w, w->scr, n, off + (uint32_t)n >= w->fsize || n < SCR, &h);
        if (q >= 0) {
            TR(38) += (off + q) - w->pos;
            w->pos = off + q; w->pos_sample = h.sample; w->pos_ok = 1;
            return 1;
        }
        if (n < 64) break;
        off += n - 32;
    }
    w->eof = 1;
    return 0;
}

/* A file cut short (interrupted copy) still claims its full length in STREAMINFO. Checks the
 * last frames on disk; if they stop well short of the total, the track ends there. Needs two
 * consecutive headers, or the first frame right after the metadata. */
static void trunc_check(FR *w)
{
    uint32_t win = w->maxfs ? w->maxfs * 2 + 4096 : 65536, from;
    uint64_t s = 0, keep = 0;
    int bs = 0, have = 0, conf = 0, n;
    const uint8_t *p, *e;
    rbflac_frame h;
    if (!w->total || w->fsize == 0x7fffffff) return;
    if (w->fsize <= w->first_off + 16) { w->total = (uint32_t)w->maxbs; TR(40) += 0x10000; return; }
    if (win > SCR) win = SCR;
    from = w->fsize > w->first_off + win ? w->fsize - win : w->first_off;
    n = rd_at(w, from, w->scr, (int)win);
    if (n < 16) return;
    for (p = w->scr, e = w->scr + n - 16; p < e; p++) {
        if (p[0] != 0xff || (p[1] & 0xfe) != 0xf8 || rbflac_parse_header(&w->st, p, 16, &h) <= 0) continue;
        if (!have) {
            if (h.sample + 2u * (uint32_t)w->maxbs >= w->total) return;   /* reaches the end: normal */
            if (from + (p - w->scr) == w->first_off && h.sample == 0) conf = 1;
        } else if (h.sample == s + (uint32_t)bs) { conf = 1; keep = h.sample; }
        have = 1; s = h.sample; bs = h.blocksize;
    }
    if (!conf || keep + 2u * (uint32_t)w->maxbs >= w->total) return;
    w->total = keep ? keep : (uint32_t)w->maxbs;    /* no whole frame: keep one block */
    TR(40) += 0x10000;
}

static void fr_free(FR *w)
{
    if (!w) return;
    if (w->scr_raw) os_free(w->scr_raw);
    if (w->seek) os_free(w->seek);
    os_free(w);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* Parses the metadata of the file at OBJ_FILE and attaches our state to obj. 0 ok, 3 fail. */
static int fr_setup(uint8_t *obj, const char *path)
{
    File *f = *(File **)(obj + OBJ_FILE);
    FR *w;
    uint8_t h[38];
    uint32_t off;
    int last = 0;
    (void)path;
    TR(41)++; TR(24) = 1;
    if (!f) return 3;
    w = os_malloc(sizeof(FR));
    if (!w) return 3;
    memset(w, 0, sizeof(*w));
    w->f = f;
    if (f->vt->seek(f, 0, 0) || rd(f, h, 4) != 4 || h[0] != 'f' || h[1] != 'L' || h[2] != 'a' || h[3] != 'C') { TR(24) = 0x20; fr_free(w); return 3; }
    TR(24) = 2;
    off = 4;
    while (!last) {
        uint32_t len;
        if (rd(f, h, 4) != 4) { fr_free(w); return 3; }
        last = h[0] & 0x80;
        len = (h[1] << 16) | (h[2] << 8) | h[3];
        off += 4;
        if ((h[0] & 0x7f) == 0) {
            if (len < 34 || rd(f, h, 34) != 34) { fr_free(w); return 3; }
            w->minbs = (h[0] << 8) | h[1];
            w->maxbs = (h[2] << 8) | h[3];
            w->maxfs = (h[7] << 16) | (h[8] << 8) | h[9];
            w->sr = (h[10] << 12) | (h[11] << 4) | (h[12] >> 4);
            w->ch = ((h[12] >> 1) & 7) + 1;
            w->bps = (((h[12] & 1) << 4) | (h[13] >> 4)) + 1;
            w->total = ((uint64_t)(h[13] & 15) << 32) | ((uint32_t)h[14] << 24) | (h[15] << 16) | (h[16] << 8) | h[17];
        } else if ((h[0] & 0x7f) == 3 && len >= 18 && !w->seek) {
            /* SEEKTABLE: 18-byte points, placeholders have sample ~0 */
            uint32_t np = len / 18, i, k = 0;
            uint8_t e[18];
            if (np > SEEK_MAX) np = SEEK_MAX;
            w->seek = os_malloc(np * sizeof(SeekPt));
            for (i = 0; w->seek && i < np; i++) {
                if (rd(f, e, 18) != 18) break;
                if (be64(e) == ~(uint64_t)0) continue;
                if (k && be64(e) <= w->seek[k - 1].sample) continue;   /* must be ascending */
                w->seek[k].sample = be64(e); w->seek[k].off = (uint32_t)be64(e + 8); k++;
            }
            w->nseek = k;
        }
        off += len;
        if (f->vt->seek(f, (int64_t)off, 0)) { fr_free(w); return 3; }
    }
    if (!w->sr || w->bps < 4 || w->bps > 24 || w->maxbs < 16 || w->ch < 1 || w->ch > MAXCH) { TR(24) = 0x83; fr_free(w); return 3; }
    w->first_off = off;
    {
        int a = f->vt->s6(f), b = f->vt->s7(f);
        uint32_t m = (uint32_t)(a > b ? a : b);
        w->fsize = m > off ? m : m ? off : 0x7fffffff;   /* m <= off: cut inside the metadata */
    }
    w->och = w->ch >= 2 ? 2 : 1;
    w->dec = 1; w->osr = w->sr;
    while (w->osr > 48000 && w->dec < 4) { w->dec *= 2; w->osr /= 2; }
    if (w->osr > 48000 || w->osr < 8000) { TR(24) = 0x84; fr_free(w); return 3; }
    w->st.sr = w->sr; w->st.ch = w->ch; w->st.bps = w->bps; w->st.maxbs = w->maxbs; w->st.flags = RBFLAC_F_CRC16;
    w->scr_raw = os_malloc(SCR + 64);
    if (!w->scr_raw) { TR(24) = 0x85; fr_free(w); return 3; }
    w->scr = (uint8_t *)(((uintptr_t)w->scr_raw + 31) & ~(uintptr_t)31);
    TR(24) = 5;
    trunc_check(w);
    w->dur_ms = w->total ? (uint32_t)(w->total * 1000u / (uint32_t)w->sr) : 86400000u;
    {   /* average bit rate. Low bits 101 mark FLAC (a real WAV's rate is a multiple of 8). */
        uint32_t secs = w->dur_ms / 1000; if (!secs) secs = 1;
        w->bitrate = w->fsize == 0x7fffffff ? 0 : (uint32_t)(((uint64_t)w->fsize * 8) / secs);
        w->bitrate = (w->bitrate & ~7u) | 5;
    }
    w->pos = w->first_off; w->pos_sample = 0; w->pos_ok = 1; w->eof = 0;
    *(uint32_t *)(obj + 0x414) = OBJ_MAGIC;
    *(FR **)(obj + 0x418) = w;
    TR(24) = 6;
    TR(25) = w->sr; TR(26) = w->ch | (w->bps << 8) | (w->dec << 16) | ((uint32_t)w->maxbs << 20);
    TR(27) = w->fsize; TR(28) = w->dur_ms; TR(40) = (TR(40) & 0xffff0000) | (uint32_t)w->nseek;
    return 0;
}

static void fr_detach(uint8_t *obj)
{
    FR *w = fr_of(obj);
    if (w) { fr_free(w); *(uint32_t *)(obj + 0x414) = 0; *(FR **)(obj + 0x418) = 0; }
}

/* Read(obj, buf, size, flag, abortp): fill the pool buffer with whole frames.
 * Layout, E = buf + (size & ~3): [E-4] 0, [E-8] packet count, [E-12] ms of the first packet,
 * [E-16] ms covered, entries {ptr, len, ms} of 12 bytes from E-28 downwards, data from buf
 * upwards. 0 = more to come, 1 = file done, 2 = aborted, 3 = error. flag set = close. */
static volatile int dump_req;   /* set by the decoder (its task must not touch files); the reader serves it */
/* audio reads and cover decodes in flight; bss is not reliably zero, so gs_init sets it */
static struct { uint32_t magic; int reading, arting; } gs;
static void gs_init(void) { if (gs.magic != 0x53534147) { gs.magic = 0x53534147; gs.reading = gs.arting = 0; } }

static int fr_read2(void *obj, uint8_t *buf, int size, int flag, volatile uint8_t *abortp);
static int fr_read(void *obj, uint8_t *buf, int size, int flag, volatile uint8_t *abortp)
{
    int rc;
    gs_init(); gs.reading++;
    TR(20) = (TR(20) & ~0xffu) | cur_task();
    rc = fr_read2(obj, buf, size, flag, abortp);
    gs.reading--;
    if (dump_req) { dump_req = 0; flac_dump(); }
    return rc;
}
static int fr_read2(void *obj, uint8_t *buf, int size, int flag, volatile uint8_t *abortp)
{
    FR *w = fr_of(obj);
    uint8_t *E = buf + (size & ~3);
    uint32_t *trail = (uint32_t *)E;
    uint32_t *ent;
    int limit, got = 0, file_end = 0, p, q, count = 0, have = 0, rc;
    rbflac_frame h, hq;
    uint64_t ms0 = 0;
    TR(31)++; TR(33) = (uint32_t)size;
    if (!w) return 3;
    if (flag) { ((int (*)(void *))(*(void ***)obj)[9])(obj); return 3; }
    trail[-1] = 0; trail[-2] = 0; trail[-3] = 0; trail[-4] = 0;
    if (abortp && *abortp == 1) { TR(34)++; return 2; }
    if (w->eof) return 1;
    if (!w->pos_ok && !fr_locate(w)) return 1;
    trail[-3] = ms_of(w->pos_sample, (uint32_t)w->sr);
    /* read in 64 KB pieces, leaving the trailer and room for the index */
    limit = (int)(E - buf) - 16 - 12 * 16;
    limit &= ~31;
    while (got < limit) {
        int want = limit - got, r;
        if (want > 65536) want = 65536;
        if (w->pos + (uint32_t)got >= w->fsize) { file_end = 1; break; }
        if (w->f->vt->seek(w->f, (int64_t)(w->pos + (uint32_t)got), 0)) { TR(34) += 0x10000; file_end = 1; break; }
        dinv(buf + got, want);
        r = rd(w->f, buf + got, want);
        TR(29)++;
        if (r <= 0) { file_end = 1; break; }
        dinv(buf + got, (r + 31) & ~31);
        got += r;
        if (r < want) { file_end = 1; break; }
        if (abortp && *abortp == 1) { TR(34)++; return 2; }
    }
    if (got < 16) { w->eof = 1; return 1; }
    /* the frame at pos should be here; if its bytes are bad, find the next confirmed frame */
    p = 0;
    if (rbflac_parse_header(&w->st, buf, got, &h) <= 0 || h.sample != w->pos_sample) {
        p = find_frame(w, buf, got, file_end, &h);
        if (p < 0) {
            if (file_end) { w->eof = 1; return 1; }
            w->pos += (uint32_t)(got - 64); w->pos_ok = 0;   /* nothing usable, look further on */
            return 0;
        }
        TR(38) += (uint32_t)p;
    }
    ent = (uint32_t *)(E - 28);
    for (;;) {
        /* next header that continues the frame at p */
        for (q = p + h.hdrlen; q + 2 <= got; q++) {
            if (buf[q] != 0xff || (buf[q + 1] & 0xfe) != 0xf8) continue;
            if (rbflac_parse_header(&w->st, buf + q, got - q, &hq) <= 0) continue;
            if (hq.sample == h.sample + (uint64_t)h.blocksize) break;
            TR(39)++;
        }
        if (q + 2 > got) {
            if (!file_end) break;            /* last frame of this buffer, delivered next time */
            q = got;                         /* last frame of the file */
        }
        if ((uint8_t *)(ent - 1) < buf + q) break;   /* index would run into the data */
#ifdef HOSTTEST
        ent[0] = (uint32_t)p;                      /* 64-bit host: offset, the harness adds buf */
#else
        ent[0] = (uint32_t)(uintptr_t)(buf + p);
#endif
        ent[1] = (uint32_t)(q - p); ent[2] = ms_of(h.sample, (uint32_t)w->sr);
        ent -= 3; count++;
        if (!have) { ms0 = h.sample; have = 1; }
        if (q >= got) { w->eof = 1; p = q; break; }
        p = q; h = hq;
        if (w->total && h.sample >= w->total) { w->eof = 1; break; }   /* past the (clamped) end */
    }
    trail[-2] = (uint32_t)count;
    if (count) {
        trail[-3] = ms_of(ms0, (uint32_t)w->sr);
        uint64_t end_sample = w->eof && p >= got ? h.sample + (uint64_t)h.blocksize : h.sample;
        trail[-4] = ms_of(end_sample, (uint32_t)w->sr) - ms_of(ms0, (uint32_t)w->sr);
    }
    w->packets += (uint32_t)count; TR(30) = w->packets; TR(32) += (uint32_t)(p);
    if (w->eof) rc = 1;
    else {
        if (!count) {   /* one frame bigger than the whole buffer: broken file */
            w->eof = 1; rc = 1;
        } else { w->pos += (uint32_t)p; w->pos_sample = h.sample; rc = 0; }
    }
    return rc;
}

/* Seek(obj, ms): the next Read starts at the frame holding that time. 3 if past the end. */
static int fr_seek(void *obj, uint32_t ms)
{
    FR *w = fr_of(obj);
    uint64_t T, lo_s = 0;
    uint32_t lo, hi;
    rbflac_frame h;
    TR(29) += 0x10000; TR(35) = ms;
    if (!w) return 3;
    if (ms >= w->dur_ms) return 3;
    T = (uint64_t)ms * (uint32_t)w->sr / 1000u;
    w->eof = 0;
    lo = w->first_off; hi = w->fsize;
    if (w->nseek) {
        int a = 0, b = w->nseek - 1, m;
        while (a < b) { m = (a + b + 1) / 2; if (w->seek[m].sample <= T) a = m; else b = m - 1; }
        if (w->seek[a].sample <= T && w->first_off + w->seek[a].off < w->fsize) { lo = w->first_off + w->seek[a].off; lo_s = w->seek[a].sample; }
    } else if (T >= (uint64_t)w->maxbs) {
        /* bisection on frame headers */
        while (hi > lo + 65536) {
            uint32_t mid = lo + (hi - lo) / 2;
            int n = rd_at(w, mid, w->scr, SCR), q;
            if (n <= 0) { hi = mid; continue; }
            q = find_frame(w, w->scr, n, mid + (uint32_t)n >= w->fsize, &h);
            if (q < 0 || h.sample > T) hi = mid;
            else { lo = mid + (uint32_t)q; lo_s = h.sample; }
        }
    }
    /* walk forward to the frame that holds T */
    {
        uint32_t off = lo;
        int tries = 0;
        while (tries++ < 64) {
            int n = rd_at(w, off, w->scr, SCR), q, q2, at_eof;
            rbflac_frame h2;
            if (n <= 0) break;
            at_eof = off + (uint32_t)n >= w->fsize;
            q = find_frame(w, w->scr, n, at_eof, &h);
            if (q < 0) { off += (uint32_t)(n - 64); continue; }
            for (;;) {
                if (h.sample + (uint64_t)h.blocksize > T) { w->pos = off + (uint32_t)q; w->pos_sample = h.sample; w->pos_ok = 1; return 0; }
                for (q2 = q + h.hdrlen; q2 + 2 <= n; q2++) {
                    if (w->scr[q2] != 0xff || (w->scr[q2 + 1] & 0xfe) != 0xf8) continue;
                    if (rbflac_parse_header(&w->st, w->scr + q2, n - q2, &h2) <= 0) continue;
                    if (h2.sample == h.sample + (uint64_t)h.blocksize) break;
                }
                if (q2 + 2 > n) break;       /* next frame is outside the window */
                q = q2; h = h2;
            }
            if (at_eof) { w->pos = off + (uint32_t)q; w->pos_sample = h.sample; w->pos_ok = 1; return 0; }
            off += (uint32_t)q;             /* window starts at the last known frame */
        }
    }
    (void)lo_s;
    w->pos = lo; w->pos_ok = 0;              /* let Read find a frame from here */
    return 0;
}

/* GetFormat(obj, info): what the track and the decoder selector see */
static int fr_getfmt(void *obj, uint8_t *info)
{
    FR *w = fr_of(obj);
    int fpp;
    if (!w) return 3;
    fpp = w->maxbs / w->dec; if (fpp > CHUNK) fpp = CHUNK;
    *(uint32_t *)(info + 0) = 1;
    *(uint32_t *)(info + 4) = w->dur_ms;
    info[8] = 0;
    info[9] = 2;                                   /* "PCM" as far as the OS is concerned */
    *(uint32_t *)(info + 0xc) = w->bitrate;        /* real average bit rate, low bits 101 */
    *(uint32_t *)(info + 0x10) = 16;
    info[0x14] = 0xf1; info[0x15] = 0xac;          /* FLAC marker (bytes the PCM codec ignores) */
    *(uint32_t *)(info + 0x2c) = (uint32_t)w->och;
    *(uint32_t *)(info + 0x30) = (uint32_t)w->osr;
    *(uint32_t *)(info + 0x34) = (uint32_t)fpp;
    info[0x38] = 0;
    return 0;
}

static int fr_close(void *obj)
{
    TR(41) += 0x10000;
    fr_detach(obj);
    flac_dump();
    return os_wav_close(obj);
}
static void *fr_dtor(void *obj) { fr_detach(obj); return os_wav_dtor(obj); }
static void fr_del(void *obj) { fr_detach(obj); os_wav_del(obj); }

/* Open(obj, path): like the OS WAV Open, reopens the file and then parses it. */
static int fr_open(uint8_t *obj, const char *path)
{
    File *f = *(File **)(obj + OBJ_FILE);
    int rc;
    fr_detach(obj);
    if (f) { f->vt->del(f); *(File **)(obj + OBJ_FILE) = 0; }
    f = os_file_ctor(os_malloc(0x114), path, 0x400, 1);
    *(File **)(obj + OBJ_FILE) = f;
    if (!f) return 3;
    rc = f->vt->open(f);
    if (rc) { TR(24) = 0x81; goto fail; }
    os_strncpy(obj + 0x14, path, 0x400);
    rc = fr_setup(obj, path);
    if (!rc) return 0;
fail:
    flac_dump();
    os_wav_close(obj);
    return 3;
}

static void *const fr_vt[20] = {
    (void *)fr_dtor, (void *)fr_del, (void *)fr_open, 0, (void *)fr_seek, (void *)fr_read, (void *)fr_getfmt,
    (void *)0x0826493c, (void *)0x08264964, (void *)fr_close, (void *)0x08264934, (void *)0x0826495c,
    (void *)0x08264944, (void *)0x0826494c, (void *)0x08264954, (void *)0x0826492c, 0, 0, 0, (void *)0x0828c580
};

static int isflac(const char *p)
{
    int n = 0;
    while (p[n]) n++;
    return n > 5 && p[n - 5] == '.' && (p[n - 4] | 32) == 'f' && (p[n - 3] | 32) == 'l' &&
        (p[n - 2] | 32) == 'a' && (p[n - 1] | 32) == 'c';
}

/* Hook on the WAV parser (0x0828a5ac), run after WAV Open opens the file and copies the path
 * to obj+0x14. For a .flac the object becomes our reader. r[0] is the result WAV Open returns
 * (0 ok; otherwise it closes the file and fails the track). */
static int __attribute__((used)) parse_c(uint32_t *r)
{
    uint8_t *obj = (uint8_t *)(uintptr_t)r[0];
    int rc;
    if (!obj || !isflac((const char *)(obj + 0x14))) return 0;
    rc = fr_setup(obj, (const char *)(obj + 0x14));
    if (rc == 0) *(void *const **)obj = fr_vt;
    else flac_dump();
    r[0] = (uint32_t)rc;
    return 1;
}

/* ---------- the FLAC decoder (takes the PCM decoder's place for marked formats) ---------- */
/* Decoder state. Offsets up to +0x48 match the PCM decoder's Init fields. */
typedef struct {
    void *const *vt;
    uint8_t b4, b5, b6, b7;
    uint32_t total;        /* +0x08 frames posted */
    uint32_t ms;           /* +0x0c ms of the last packet */
    uint32_t r10;
    uint32_t ch;           /* +0x14 */
    void *trk;             /* +0x18 */
    uint32_t end;          /* +0x1c end ms */
    uint32_t r20, r24, r28, r2c, r30, r34, r38;
    uint32_t shift;        /* +0x3c log2(bytes per frame) */
    uint8_t f40, f41, f42, f43, b44, b45, b46, b47;
    uint32_t state;        /* +0x48: 0 fresh, 1 running, 2 ended */
    /* ours: stream parameters come from each frame header; nothing is shared with the reader */
    uint32_t magic;
    int cfg;               /* 0 until the first frame header has been seen */
    int sr_desc;           /* the rate we reported (after decimation), for headers that defer to STREAMINFO */
    int sr, nch, bps, maxbs, och, dec, firlen;
    rbflac_stream st;
    int32_t *dec_buf[MAXCH]; int buf_bs, buf_ch;
    int16_t *pcm; int pcm_cap;
    int pend_n, pend_pos, chunk; uint64_t pend_out;   /* output frames waiting; pend_out is the first one's sample index */
    int32_t *hist[2]; int hpos; const rbfir *rfir;
    uint32_t rng;
    uint64_t next_sample;
    uint32_t t_init, t_last, ticks;
    int first_done;
} FD;
static FD fd;

static void fd_free_bufs(FD *d)
{
    int c;
    for (c = 0; c < d->buf_ch; c++) if (d->dec_buf[c]) os_free(d->dec_buf[c]);
    for (c = 0; c < MAXCH; c++) d->dec_buf[c] = 0;
    if (d->pcm) os_free(d->pcm);
    d->pcm = 0; d->buf_bs = 0; d->buf_ch = 0; d->pcm_cap = 0;
}
static int fd_alloc(FD *d, int bs, int ch, int och)
{
    int c, n = ch > 2 ? 3 : ch;   /* channels past 2 are decoded into one scratch buffer */
    if (d->buf_bs >= bs && d->buf_ch >= n && d->pcm_cap >= bs * och) return 1;
    fd_free_bufs(d);
    for (c = 0; c < n; c++) if (!(d->dec_buf[c] = os_malloc(bs * 4 + 16))) { fd_free_bufs(d); return 0; }
    for (c = n; c < ch; c++) d->dec_buf[c] = d->dec_buf[2];
    if (!(d->pcm = os_malloc(bs * och * 2 + 16))) { fd_free_bufs(d); return 0; }
    d->buf_bs = bs; d->buf_ch = n; d->pcm_cap = bs * och;
    return 1;
}
static void fd_once(FD *d)
{
    int c;
    if (d->magic == FD_MAGIC) return;
    memset(d, 0, sizeof(*d));
    d->magic = FD_MAGIC;
    d->b5 = 1;
    for (c = 0; c < 2; c++) d->hist[c] = os_malloc(2 * FIRMAX * 4);
}

static inline int16_t sat16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* decode one frame (packet) into d->pcm; 1 ok, 0 bad frame */
static int fd_decode(FD *d, const uint8_t *ptr, int len)
{
    rbflac_frame h;
    int r, bs, i, c, och = d->och, n = 0;
    int16_t *o = d->pcm;
    uint32_t t0 = HTIMER, dt;
    {   /* the frame header gives the format: configure on the first frame and on any change */
        int sr, nch, bps, fbs, fixed;
        r = rbflac_peek_header(ptr, len, &sr, &nch, &bps, &fbs, &fixed);
        if (r <= 0) { TR(45) = ((TR(45) + 1) & 0xffff) | (uint32_t)(-r & 0xff) << 16; return 0; }
        if (!sr) sr = d->cfg ? d->sr : d->sr_desc * d->dec;   /* rare: rate only in STREAMINFO */
        if (!bps) bps = d->cfg ? d->bps : 16;
        if (!d->cfg || sr != d->sr || nch != d->nch || bps != d->bps) {
            d->sr = sr; d->nch = nch; d->bps = bps;
            d->dec = 1;
            while (sr / d->dec > 48000 && d->dec < 4) d->dec *= 2;
            d->firlen = d->dec == 2 ? 64 : 128;
            d->rfir = rbfir_get(d->dec, 1);
            d->st.sr = sr; d->st.ch = nch; d->st.bps = bps;
            for (c = 0; c < 2; c++) memset(d->hist[c], 0, 2 * FIRMAX * 4);
            d->hpos = 0;
            d->cfg = 1;
            TR(62) = (uint32_t)sr | (uint32_t)bps << 20 | (uint32_t)nch << 25 | (uint32_t)d->dec << 29;
        }
        if (fbs > d->st.maxbs) d->st.maxbs = fbs;   /* fixed-size streams: the frame number scales by this */
        if (!fd_alloc(d, d->st.maxbs, nch, och)) { TR(45) = ((TR(45) + 1) & 0xffff) | 0x200000; return 0; }
    }
    r = rbflac_decode_frame(&d->st, ptr, len, d->dec_buf, &h);
    if (r <= 0) { TR(45) = ((TR(45) + 1) & 0xffff) | (uint32_t)(-r & 0xff) << 16; return 0; }
    o = d->pcm;                          /* allocated above, possibly just now */
    bs = h.blocksize;
    if (h.sample != d->next_sample) {      /* a jump (seek or lost frame): the filter starts over */
        for (c = 0; c < 2; c++) memset(d->hist[c], 0, 2 * FIRMAX * 4);
        d->hpos = 0;
    }
    if (d->dec == 1) {
        int sh = d->bps - 16;
        for (i = 0; i < bs; i++)
            for (c = 0; c < och; c++) {
                int32_t v = d->dec_buf[c][i];
                if (sh > 0) {   /* TPDF dither, +-1 LSB, then round */
                    uint32_t m = (1u << sh) - 1, a, b;
                    d->rng = d->rng * 1664525u + 1013904223u; a = (d->rng >> 8) & m;
                    d->rng = d->rng * 1664525u + 1013904223u; b = (d->rng >> 8) & m;
                    v = (v + (int32_t)(a + b) - (int32_t)m + (1 << (sh - 1))) >> sh;
                } else if (sh < 0) v <<= -sh;
                o[n++] = sat16(v);
            }
        d->pend_out = h.sample;
        d->pend_n = bs;
    } else {
        int D = d->dec, L = d->firlen, sh24 = 24 - d->bps;
        uint64_t s0 = h.sample;
        for (i = 0; i < bs; i++) {
            int hp = d->hpos;
            for (c = 0; c < och; c++) {
                int32_t v = d->dec_buf[c][i];
                v = sh24 >= 0 ? v << sh24 : v >> -sh24;
                d->hist[c][hp] = d->hist[c][hp + L] = v;
            }
            d->hpos = hp = (hp + 1 == L) ? 0 : hp + 1;
            if (((uint32_t)(s0 + i) % D) == 0) {
                for (c = 0; c < och; c++) {
                    const int32_t *x = d->hist[c] + hp;  /* oldest..newest */
                    int64_t acc = rbfir_dot(d->rfir, x);
                    uint32_t a, b;   /* TPDF dither, +-1 output LSB (2^23) */
                    d->rng = d->rng * 1664525u + 1013904223u; a = d->rng >> 9;
                    d->rng = d->rng * 1664525u + 1013904223u; b = d->rng >> 9;
                    acc += (int64_t)a + b - (1 << 23);
                    o[n++] = sat16((int32_t)((acc + (1 << 22)) >> 23));
                }
            }
        }
        d->pend_out = (s0 + D - 1) / D;
        d->pend_n = n / och;
    }
    d->pend_pos = 0;
    d->next_sample = h.sample + bs;
    dt = HTIMER - t0;
    TR(42) += dt; TR(43) += bs; TR(44)++;
    if (dt > TR(47)) TR(47) = dt;
    return 1;
}

/* DecodeNext: called by the decode task when the output ring has room. Same state machine as
 * the OS PCM decoder: one post per call, flag 1 on the first post after Init, flag 2 on the final
 * (empty) post, -1 when there is nothing to do. */
static int fd_decode_next(FD *d)
{
    int first = 0, n;
    void *buf;
    if (d->state == 2) { os_end_signal(); return -1; }
    if (d->state == 0) { d->state = 1; first = 1; }
    if (d->b44) return -1;
    if (!d->pend_n) {
        int tries = 0;
        for (;;) {
            int len = 0, rc, ok;
            uint8_t *ptr = 0;
            uint32_t ms = 0, fl = 0;
            rc = src_get(d->trk, &len, &ptr, &ms, &fl);
            if (!(rc == 0 && d->ms < d->end && len > 0)) {
#ifdef HOSTTEST
                if (getenv("DBG")) printf("end path: rc %d ms %u end %u len %d\n", rc, (unsigned)d->ms, (unsigned)d->end, len);
#endif
                /* end of the track: an empty post with the end flag, as the PCM decoder does */
                d->b4 = d->b5 = 1; d->state = 2;
                TR(57)++; TR(51) = HTIMER - d->t_init;
                if (out_get(&buf, 0)) { TR(58)++; return -1; }
                out_post(0, 0, d->ms, first | 2, d->trk);
                if (rc == 0) src_rel(d->trk, len, ptr, ms);
                dump_req = 1;
                return 0;
            }
            d->ms = ms;
            ok = fd_decode(d, ptr, len);
            src_rel(d->trk, len, ptr, ms);
            if (ok) break;
            if (++tries >= 32) {   /* nothing decodes: a block of silence keeps the player moving */
                n = (d->st.maxbs ? d->st.maxbs : 4096) / d->dec; if (n > CHUNK) n = CHUNK;
                if (!d->pcm && !fd_alloc(d, n, 2, d->och)) return -1;
                memset(d->pcm, 0, n * d->och * 2);
                d->pend_n = n; d->pend_pos = 0;
                d->pend_out = d->next_sample / d->dec;
                break;
            }
        }
        {   /* a frame bigger than one post goes out in equal parts */
            int k = (d->pend_n + CHUNK - 1) / CHUNK;
            d->chunk = (d->pend_n + k - 1) / k;
        }
    } else TR(55)++;
    n = d->pend_n - d->pend_pos;
    if (n > d->chunk) n = d->chunk;
    if (out_get(&buf, 1) || !buf) { TR(58)++; return -1; }
    memcpy(buf, d->pcm + d->pend_pos * d->och, n * d->och * 2);
    out_post(buf, n, ms_of(d->pend_out + (uint64_t)d->pend_pos, (uint32_t)d->sr / d->dec), first, d->trk);
    d->total += (uint32_t)n;
    d->pend_pos += n;
    if (d->pend_pos >= d->pend_n) d->pend_n = d->pend_pos = 0;
    TR(46)++;
    d->t_last = HTIMER;
    return 0;
}

/* Init(desc): called by the selector with the stream description (rate, bits and channels as
 * reported, end time). Nothing else is needed: each frame header carries the rate, channels,
 * depth and block size. */
static int fd_init(FD *d, const uint8_t *desc)
{
    uint8_t *trk = CUR_TRACK;
    int ch = (int)*(const uint32_t *)(desc + 0xc), c;
    TR(49) = ((TR(49) + 1) & 0xffff) | 0x10000 | cur_task() << 24;
    TR(42) = TR(43) = TR(44) = TR(47) = 0;       /* per-track decode counters */
    TR(62) = 0;
    d->och = ch >= 2 ? 2 : 1;
    d->sr_desc = (int)*(const uint32_t *)(desc + 4);
    d->cfg = 0; d->sr = 0; d->nch = 0; d->bps = 0; d->maxbs = 0; d->dec = 1;
    d->firlen = 128; d->rfir = 0;
    memset(&d->st, 0, sizeof d->st);
    d->st.flags = RBFLAC_F_CRC16;
    for (c = 0; c < 2; c++) if (d->hist[c]) memset(d->hist[c], 0, 2 * FIRMAX * 4);
    d->hpos = 0;
    d->rng = 0x1234567;
    d->next_sample = ~(uint64_t)0;
    d->pend_n = d->pend_pos = 0;
    /* the PCM decoder's Init, field for field */
    d->trk = trk;
    d->end = *(const uint32_t *)(desc + 0x24);
    d->shift = 1 + (d->och == 2);
    d->b44 = 0;
    d->f40 = 0; d->f41 = d->och == 1; d->f42 = d->f43 = 0;
    d->ch = (uint32_t)d->och;
    d->total = 0; d->ms = 0;
    d->b5 = 0; d->b4 = 0;
    d->state = 0;
    d->b45 = 1; d->b46 = 0;
    d->t_init = HTIMER;
    os_kick();
    return 0;
}

#ifndef HOSTTEST
/* the PCM decoder's vtable with Init and DecodeNext replaced */
static void *const fd_vt[16] = {
    (void *)0x081a4ea4, 0, (void *)0x081a4e68, (void *)0x081a4e2c, (void *)fd_init, (void *)0x081a4c88,
    (void *)0x081a48f0, (void *)0x081a4e04, (void *)fd_decode_next, 0, 0, (void *)0x081a4e28,
    (void *)0x081a48e4, (void *)0x081a4ddc, (void *)0x081a4de0, (void *)0x081a4968
};
/* The selector (IRAM 0x22005d60) sends codecs 2/4/6 through the veneer word at 0x22003824
 * to the PCM decoder getter; that word is repointed to flac_select_shim. r5 holds the description. */
void *__attribute__((used)) flac_select(const uint8_t *desc)
{
    TR(48)++;
    if (desc[0] == 2 && desc[0x10] == 0xf1 && desc[0x11] == 0xac && (*(const uint32_t *)(desc + 0x14) & 7) == 5) {
        TR(48) += 0x10000;
        fd_once(&fd);
        fd.vt = fd_vt;
        return &fd;
    }
    return os_pcm_dec();
}
/* target of the selector literal at 0x22003824 (OS patch, see HOOKS.md) */
__attribute__((naked, section(".text.entry"), used)) void flac_select_shim(void)
{ __asm__ volatile("mov r0, r5\n b flac_select"); }
#else
void *flac_select(const uint8_t *desc) { (void)desc; fd_once(&fd); return &fd; }
int host_fd_init(const uint8_t *desc) { fd_once(&fd); return fd_init(&fd, desc); }
int host_fd_next(void) { return fd_decode_next(&fd); }
int host_fd_pending(void) { return fd.pend_n; }
#endif

/* ---------- entries from the OS patch ---------- */
#ifndef HOSTTEST
#include "art.c"
#endif

/* called in place of the File ctor for .flac paths, inside WAV Open */
File *flac_file_new(void *mem, const char *path, int a2, int a3)
{
    {   /* keeps the end of the path (album tail and file name) in TR8-15 for the logs */
        volatile char *tp = (volatile char *)&TR(8); int i, n;
        for (n = 0; path[n]; n++);
        i = n > 31 ? n - 31 : 0;
        for (n = 0; path[i]; i++, n++) tp[n] = path[i];
        for (; n < 32; n++) tp[n] = 0;
    }
    TR(20) = (TR(20) & 0xffffffu) | cur_task() << 24;
    return os_file_ctor(mem, path, a2, a3);
}

#ifndef HOSTTEST
/* WAV Open (0x0828a96c) calls this in place of the File ctor */
__attribute__((section(".text.entry"), used, noinline)) File *e_flac_open(void *mem, const char *path, int a2, int a3)
{
    File *r;
    if (!isflac(path)) return os_file_ctor(mem, path, a2, a3);
    r = flac_file_new(mem, path, a2, a3);
    flac_dump();
    return r;
}
/* FindImageForTrack and ArtworkThumb::Load call sites */
__attribute__((section(".text.entry"), used, noinline)) void *e_art_find(void *lib, uint8_t *track) { return art_find(lib, track); }
__attribute__((section(".text.entry"), used, noinline)) void e_art_load(void *th, void *alloc) { art_load(th, alloc); }
#else
uint32_t *host_trace(void) { return trace_buf; }
int host_fr_open(void *obj, const char *path) { return fr_open(obj, path); }
int host_fr_getfmt(void *obj, uint8_t *info) { return fr_getfmt(obj, info); }
int host_fr_seek(void *obj, uint32_t ms) { return fr_seek(obj, ms); }
int host_fr_read(void *obj, uint8_t *buf, int size, int flag, volatile uint8_t *abortp) { return fr_read(obj, buf, size, flag, abortp); }
int host_fr_close(void *obj) { return fr_close(obj); }
#endif
