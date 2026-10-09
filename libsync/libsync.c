/* libsync: see libsync.h. Field layout and sort rules follow flacsync.py exactly. */
#include "libsync.h"
#include "tables.h"

#ifndef LS_MEMCPY
#include <string.h>
#include <stdio.h>
#define LS_MEMCPY   memcpy
#define LS_MEMSET   memset
#define LS_MEMCMP   memcmp
#define LS_STRLEN   strlen
#define LS_SNPRINTF snprintf
#endif

#define FLAC_TYPE   0x464c4143u     /* mhit filetype 'FLAC' */
#define MAC_EPOCH   2082844800u
#define MAXFILES    32768
#define MAXDE       16384           /* entries on the scan stack: one folder and its ancestors */
#define FCHUNK      512
#define MAXDEPTH    32
#define PATHMAX     1024
#define TAGLIM      (1 << 20)       /* tags are read from the first 1 MB, as in flacsync.py */
#define MHIT_DEFLEN 0x270

/* Entry version marker at LS_MARK, in the zero tail of the 0x270-byte track header (0x210..0x26f
   is zero in entries written by the OS or iTunes): 'FLSY', u16 version, u16 flags, u32 sync
   generation, u32 reserved. Entries without the current marker are rebuilt once, keeping
   id, dbid, dates and play counts. */
#define LS_MARK       0x258
#define LS_VER        2
#define LS_F_FNTITLE  1             /* title taken from the file name */
#define LS_F_NOALBUM  2
#define LS_F_NOARTIST 4

/* ---------------- bytes ---------------- */
static uint32_t g32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t g16(const uint8_t *p) { return p[0] | p[1] << 8; }
static void p32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void p16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void p64(uint8_t *p, uint64_t v) { p32(p, (uint32_t)v); p32(p + 4, (uint32_t)(v >> 32)); }
static uint64_t g64(const uint8_t *p) { return (uint64_t)g32(p) | (uint64_t)g32(p + 4) << 32; }
static uint32_t mac_time(uint32_t t) { return t + MAC_EPOCH; }

/* ---------------- scratch memory ---------------- */
static uint8_t *m_base;
static size_t m_used, m_size, t_used;   /* bump allocation from the bottom, scan temporaries from the top */
static void *amem(size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if (n > m_size - m_used - t_used)
        return NULL;
    void *p = m_base + m_used;
    m_used += n;
    return p;
}
/* released per folder by the scan */
static void *tmem(size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if (n > m_size - m_used - t_used)
        return NULL;
    t_used += n;
    return m_base + m_size - t_used;
}

static struct ls_result *R;
static int fail(const char *m)
{
    if (!R->msg[0])
        LS_SNPRINTF(R->msg, sizeof R->msg, "%s", m);
    return -1;
}
#define NEW(p, n) do { if (!((p) = amem(n))) return fail("out of memory"); } while (0)

/* ---------------- SHA-1 and hash58 ---------------- */
struct sha1 { uint32_t h[5]; uint64_t n; uint8_t b[64]; };
static uint32_t rol(uint32_t x, int s) { return x << s | x >> (32 - s); }
static void sha1_blk(uint32_t *h, const uint8_t *p)
{
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f, k, t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8f1bbcdc; }
        else             { f = b ^ c ^ d;                    k = 0xca62c1d6; }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
static void sha1_init(struct sha1 *s)
{
    s->h[0] = 0x67452301; s->h[1] = 0xefcdab89; s->h[2] = 0x98badcfe;
    s->h[3] = 0x10325476; s->h[4] = 0xc3d2e1f0; s->n = 0;
}
/* data NULL feeds len zero bytes */
static void sha1_upd(struct sha1 *s, const void *data, uint32_t len)
{
    const uint8_t *p = data;
    while (len) {
        uint32_t k = (uint32_t)(s->n & 63), c = 64 - k;
        if (c > len)
            c = len;
        if (!k && c == 64 && p) {
            sha1_blk(s->h, p);
        } else {
            if (p) LS_MEMCPY(s->b + k, p, c);
            else   LS_MEMSET(s->b + k, 0, c);
            if (k + c == 64)
                sha1_blk(s->h, s->b);
        }
        s->n += c;
        len -= c;
        if (p)
            p += c;
    }
}
static void sha1_fin(struct sha1 *s, uint8_t out[20])
{
    uint64_t bits = s->n * 8;
    uint8_t pad = 0x80, len[8];
    int i;
    sha1_upd(s, &pad, 1);
    sha1_upd(s, NULL, (uint32_t)((120 - (s->n & 63)) & 63));
    for (i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_upd(s, len, 8);
    for (i = 0; i < 20; i++)
        out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

static uint32_t gcd(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }

void ls_hash58(const uint8_t fwid[8], const uint8_t *db, uint32_t len, uint8_t out[20])
{
    uint8_t y[16], k[64], pad[64], inner[20];
    struct sha1 s;
    int i;
    for (i = 0; i < 4; i++) {
        uint32_t a = fwid[2 * i], b = fwid[2 * i + 1];
        uint32_t l = (a && b) ? a * b / gcd(a, b) : 1, hi = (l >> 8) & 0xff, lo = l & 0xff;
        y[4 * i] = h58_t1[hi]; y[4 * i + 1] = h58_t2[hi];
        y[4 * i + 2] = h58_t1[lo]; y[4 * i + 3] = h58_t2[lo];
    }
    sha1_init(&s);
    sha1_upd(&s, h58_fixed, sizeof h58_fixed);
    sha1_upd(&s, y, 16);
    LS_MEMSET(k, 0, sizeof k);
    sha1_fin(&s, k);
    /* HMAC-SHA1 of the image with 0x18..0x1f, 0x32..0x45 and the signature (0x58..0x6b) zeroed,
       and 0x30 set to 1 */
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha1_init(&s);
    sha1_upd(&s, pad, 64);
    sha1_upd(&s, db, 0x18);
    sha1_upd(&s, NULL, 8);
    sha1_upd(&s, db + 0x20, 0x10);
    sha1_upd(&s, "\1\0", 2);
    sha1_upd(&s, NULL, 0x14);
    sha1_upd(&s, db + 0x46, 0x12);
    sha1_upd(&s, NULL, 0x14);
    sha1_upd(&s, db + 0x6c, len - 0x6c);
    sha1_fin(&s, inner);
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha1_init(&s);
    sha1_upd(&s, pad, 64);
    sha1_upd(&s, inner, 20);
    sha1_fin(&s, out);
}

/* New dbids: SHA-1 of the seed and a counter (flacsync.py uses random.getrandbits). */
static uint8_t r_seed[8];
static uint32_t r_ctr;
static void rnd8(uint8_t out[8])
{
    uint8_t c[4], d[20];
    struct sha1 s;
    p32(c, r_ctr++);
    sha1_init(&s);
    sha1_upd(&s, r_seed, 8);
    sha1_upd(&s, c, 4);
    sha1_fin(&s, d);
    LS_MEMCPY(out, d, 8);
}

/* ---------------- text ---------------- */
/* Next UTF-8 code point. Invalid bytes become U+FFFD, as in Python's decode('utf8', 'replace').
   Returns the bytes used. */
static int u8next(const uint8_t *s, int n, uint32_t *cp)
{
    uint8_t c = s[0], lo = 0x80, hi = 0xbf;
    uint32_t v;
    int need, i;
    if (c < 0x80) { *cp = c; return 1; }
    if (c >= 0xc2 && c <= 0xdf)      { need = 1; v = c & 0x1f; }
    else if (c >= 0xe0 && c <= 0xef) { need = 2; v = c & 0x0f; if (c == 0xe0) lo = 0xa0; if (c == 0xed) hi = 0x9f; }
    else if (c >= 0xf0 && c <= 0xf4) { need = 3; v = c & 0x07; if (c == 0xf0) lo = 0x90; if (c == 0xf4) hi = 0x8f; }
    else { *cp = 0xfffd; return 1; }
    for (i = 1; i <= need; i++) {
        if (i >= n || s[i] < lo || s[i] > hi) { *cp = 0xfffd; return i; }
        lo = 0x80; hi = 0xbf;
        v = v << 6 | (s[i] & 0x3f);
    }
    *cp = v;
    return need + 1;
}

/* UTF-8 to UTF-16LE, '/' to ':' if colon. out NULL only counts. Returns bytes. */
static uint32_t u8to16(const uint8_t *s, int n, uint8_t *out, int colon)
{
    uint32_t w = 0, cp;
    while (n > 0) {
        int k = u8next(s, n, &cp);
        s += k; n -= k;
        if (colon && cp == '/')
            cp = ':';
        if (cp >= 0x10000) {
            cp -= 0x10000;
            if (out) { p16(out + w, 0xd800 | cp >> 10); p16(out + w + 2, 0xdc00 | (cp & 0x3ff)); }
            w += 4;
        } else {
            if (out) p16(out + w, cp);
            w += 2;
        }
    }
    return w;
}

/* str.lower(), str.upper() for the scripts that occur in tags. Full Unicode tables
   are not needed for sorting. */
static uint32_t lc(uint32_t c)
{
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xc0 && c <= 0xde && c != 0xd7) return c + 32;
    if (c >= 0x100 && c <= 0x17f) {
        if (c == 0x130) return 'i';
        if (c == 0x178) return 0xff;
        if (c == 0x138 || c == 0x149 || c == 0x17f) return c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) return (c & 1) ? c + 1 : c;
        return (c & 1) ? c : c + 1;
    }
    if (c >= 0x391 && c <= 0x3ab && c != 0x3a2) return c + 32;
    if (c == 0x386) return 0x3ac;
    if (c >= 0x388 && c <= 0x38a) return c + 37;
    if (c == 0x38c) return 0x3cc;
    if (c == 0x38e || c == 0x38f) return c + 63;
    if (c >= 0x400 && c <= 0x40f) return c + 80;
    if (c >= 0x410 && c <= 0x42f) return c + 32;
    if (c >= 0x1e00 && c <= 0x1eff && !(c >= 0x1e96 && c <= 0x1e9f)) return (c & 1) ? c : c + 1;
    if (c >= 0x2160 && c <= 0x216f) return c + 16;
    if (c >= 0xff21 && c <= 0xff3a) return c + 32;
    return c;
}
static uint32_t uc(uint32_t c)
{
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c == 0xb5) return 0x39c;
    if (c >= 0xe0 && c <= 0xfe && c != 0xf7) return c - 32;
    if (c == 0xff) return 0x178;
    if (c >= 0x100 && c <= 0x17f) {
        if (c == 0x131) return 'I';
        if (c == 0x17f) return 'S';
        if (c == 0x138 || c == 0x149) return c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) return (c & 1) ? c : c - 1;
        return (c & 1) ? c - 1 : c;
    }
    if (c == 0x3c2) return 0x3a3;
    if (c >= 0x3b1 && c <= 0x3cb) return c - 32;
    if (c == 0x3ac) return 0x386;
    if (c >= 0x3ad && c <= 0x3af) return c - 37;
    if (c == 0x3cc) return 0x38c;
    if (c == 0x3cd || c == 0x3ce) return c - 63;
    if (c >= 0x430 && c <= 0x44f) return c - 32;
    if (c >= 0x450 && c <= 0x45f) return c - 80;
    if (c >= 0x1e00 && c <= 0x1eff && !(c >= 0x1e96 && c <= 0x1e9f)) return (c & 1) ? c - 1 : c;
    if (c >= 0x2170 && c <= 0x217f) return c - 16;
    if (c >= 0xff41 && c <= 0xff5a) return c - 32;
    return c;
}
static int is_digit(uint32_t c)
{
    return (c >= '0' && c <= '9') || c == 0xb2 || c == 0xb3 || c == 0xb9 ||
           (c >= 0x660 && c <= 0x669) || (c >= 0xff10 && c <= 0xff19);
}
static int is_alnum(uint32_t c)
{
    if (c < 0x80) return (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z');
    if (c < 0x100) return c == 0xaa || c == 0xb2 || c == 0xb3 || c == 0xb5 || c == 0xb9 || c == 0xba ||
                          (c >= 0xbc && c <= 0xbe) || (c >= 0xc0 && c != 0xd7 && c != 0xf7);
    return (c >= 0x100 && c <= 0x2af) ||
           (c >= 0x370 && c <= 0x3ff && c != 0x375 && c != 0x37e && c != 0x384 && c != 0x385 && c != 0x387 && c != 0x3f6) ||
           (c >= 0x400 && c <= 0x52f && !(c >= 0x482 && c <= 0x489)) ||
           (c >= 0x5d0 && c <= 0x5ea) || (c >= 0x620 && c <= 0x64a) || (c >= 0x660 && c <= 0x669) ||
           (c >= 0x1e00 && c <= 0x1fbc) || (c >= 0x2160 && c <= 0x2188) ||
           (c >= 0x3041 && c <= 0x3096) || (c >= 0x30a1 && c <= 0x30fa) || (c >= 0x3400 && c <= 0x4dbf) ||
           (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) ||
           (c >= 0xff10 && c <= 0xff19) || (c >= 0xff21 && c <= 0xff3a) || (c >= 0xff41 && c <= 0xff5a) ||
           (c >= 0xff66 && c <= 0xff9d);
}

/* Composes base + combining mark (NFC), so a name stored decomposed matches its precomposed form. */
static uint32_t nfc_pair(uint32_t b, uint32_t m)
{
    int lo = 0, hi = (int)(sizeof nfc_tab / sizeof nfc_tab[0]) - 1;
    uint32_t key = b << 16 | m;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t k = (uint32_t)nfc_tab[mid][0] << 16 | nfc_tab[mid][1];
        if (k == key) return nfc_tab[mid][2];
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* ---------------- tags ---------------- */
enum { T_TITLE, T_ARTIST, T_ALBUM, T_ALBUMARTIST, T_GENRE, T_TRACKNUMBER,
       T_TRACKTOTAL, T_DISCNUMBER, T_DATE, T_COMPOSER, NTAGS };
static const char *const tag_names[NTAGS] = {
    "TITLE", "ARTIST", "ALBUM", "ALBUMARTIST", "GENRE", "TRACKNUMBER",
    "TRACKTOTAL", "DISCNUMBER", "DATE", "COMPOSER" };
struct tags { const uint8_t *v[NTAGS]; int n[NTAGS]; };
static uint8_t *tagbuf;     /* TAGLIM bytes: the Vorbis comment block of the file being read */

static int key_is(const uint8_t *k, int kn, const char *name)
{
    int i;
    for (i = 0; i < kn && name[i]; i++) {
        uint8_t c = k[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != (uint8_t)name[i]) return 0;
    }
    return i == kn && !name[i];
}

/* As flacsync.py read_flac(): STREAMINFO and the first Vorbis comment of each tag, from the
   metadata blocks that end within the first 1 MB. */
static int read_flac(const char *path, uint32_t *sr, uint64_t *ns, struct tags *t)
{
    uint8_t h[4], si[18];
    long fsz, lim, p = 4;
    int got = 0, vc = 0, fd = ls_open_r(path);
    LS_MEMSET(t, 0, sizeof *t);
    if (fd < 0)
        return -1;
    fsz = ls_fsize(fd);
    lim = fsz < TAGLIM ? fsz : TAGLIM;
    if (ls_read(fd, h, 4) != 4 || LS_MEMCMP(h, "fLaC", 4)) { ls_close(fd); return -1; }
    while (p + 4 <= lim) {
        long l;
        if (ls_seek(fd, p) != p || ls_read(fd, h, 4) != 4) break;
        l = (long)h[1] << 16 | h[2] << 8 | h[3];
        if ((h[0] & 0x7f) == 0) {
            if (p + 4 + 18 <= lim && ls_read(fd, si, 18) == 18) {
                uint64_t x = 0;
                int i;
                for (i = 10; i < 18; i++) x = x << 8 | si[i];
                *sr = (uint32_t)(x >> 44);
                *ns = x & ((1ULL << 36) - 1);
                got = 1;
            }
        } else if ((h[0] & 0x7f) == 4 && p + 4 + l <= lim && !vc && l >= 8) {
            /* Only the first comment block is read. */
            vc = 1;
            if (ls_read(fd, tagbuf, l) == l) {
                uint64_t q = 4 + (uint64_t)g32(tagbuf), cnt, i;
                if (q + 4 <= (uint64_t)l) {
                    cnt = g32(tagbuf + q);
                    q += 4;
                    for (i = 0; i < cnt && q + 4 <= (uint64_t)l; i++) {
                        uint64_t m = g32(tagbuf + q), e = q + 4 + m;
                        const uint8_t *s = tagbuf + q + 4;
                        int sn, kn, j;
                        if (e > (uint64_t)l) e = (uint64_t)l;
                        sn = (int)(e - (q + 4));
                        for (kn = 0; kn < sn && s[kn] != '='; kn++);
                        for (j = 0; j < NTAGS; j++)
                            if (!t->v[j] && key_is(s, kn, tag_names[j])) {
                                t->v[j] = s + (kn < sn ? kn + 1 : sn);
                                t->n[j] = kn < sn ? sn - kn - 1 : 0;
                            }
                        q += 4 + m;
                    }
                }
            }
        }
        p += 4 + l;
        if (h[0] & 0x80) break;
    }
    ls_close(fd);
    return got && *sr ? 0 : -1;
}

/* As flacsync.py num(): digits of the part-th '/'-separated field, blanks trimmed. */
static uint32_t tag_num(const uint8_t *s, int n, int part)
{
    int a = 0, b, i;
    uint32_t v = 0;
    while (part--) {                         /* skip one '/' */
        while (a < n && s[a] != '/') a++;
        if (a >= n) return 0;
        a++;
    }
    for (b = a; b < n && s[b] != '/'; b++);
    while (a < b && (s[a] == ' ' || (s[a] >= 9 && s[a] <= 13))) a++;
    if (a + 1 < b && s[a] == 0xc2 && s[a + 1] == 0xa0) a += 2;
    while (b > a && (s[b - 1] == ' ' || (s[b - 1] >= 9 && s[b - 1] <= 13))) b--;
    if (b - 1 > a && s[b - 2] == 0xc2 && s[b - 1] == 0xa0) b -= 2;
    if (a == b) return 0;
    for (i = a; i < b; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}
/* Byte length of the first `chars` UTF-8 characters (DATE[:4]). */
static int first_chars(const uint8_t *s, int n, int chars)
{
    int i = 0;
    while (i < n && chars--) { uint32_t cp; i += u8next(s + i, n - i, &cp); }
    return i;
}

/* ---------------- the database ---------------- */
struct sec { uint32_t type; const uint8_t *h; uint32_t hl; const uint8_t *b; uint32_t bl; };
struct trk {
    uint8_t *h; uint32_t hl;            /* mhit header */
    const uint8_t *m; uint32_t ml, nm;  /* its mhods */
    int used;
    uint32_t oo;                        /* where build_db wrote it */
};
struct alb { const uint8_t *p; uint32_t len, id; const uint8_t *k[2]; uint32_t kn[2]; int mine; };
struct fent { const char *path; uint32_t size, mtime; int old; int renamed; };
typedef struct ls_dirent de_t;      /* directory entries, passed to album_cb unchanged */
struct fchunk { struct fchunk *next; int n; struct fent e[FCHUNK]; };

static const struct ls_opts *OP;
static uint8_t *DB; static uint32_t DBN;
static struct sec S[16]; static int NS;
static struct trk *OT; static int NOT;          /* tracks as read */
static struct trk *T; static int NT;            /* tracks as written */
static struct alb *AL; static int NAL, CAPAL;
static struct fent *F; static int NF;
static struct fchunk *FC0, *FCL;                /* files as the scan finds them, compacted into F */
static de_t *DE; static int DET;
static int ALS;                                 /* section holding the album list, -1 if none */
static uint32_t tmpl_hl;
static uint32_t next_item, item_base;           /* playlist item ids: above every existing one */
static char g_rel[PATHMAX], g_path[PATHMAX];

static int mhod_find(const uint8_t *m, uint32_t ml, uint32_t type, const uint8_t **s, uint32_t *n)
{
    uint32_t o = 0;
    while (o + 40 <= ml) {
        uint32_t l = g32(m + o + 8);
        if (l < 24 || o + l > ml) break;
        if (g32(m + o + 12) == type) {
            uint32_t sl = g32(m + o + 28);
            if (l < 40 || 40 + sl > l) break;
            *s = m + o + 40; *n = sl;
            return 1;
        }
        o += l;
    }
    *s = 0; *n = 0;
    return 0;
}

static int parse_db(void)
{
    uint32_t o = g32(DB + 4);
    NS = 0; NOT = 0;
    if (DBN < 0x100 || LS_MEMCMP(DB, "mhbd", 4) || o < 0x6c || o > DBN || g32(DB + 8) != DBN)
        return fail("iTunesDB header not understood");
    while (o < DBN) {
        uint32_t hl, tl;
        if (o + 16 > DBN || LS_MEMCMP(DB + o, "mhsd", 4) || NS == 16) return fail("iTunesDB section not understood");
        hl = g32(DB + o + 4); tl = g32(DB + o + 8);
        if (hl < 16 || tl < hl || tl > DBN - o) return fail("iTunesDB section size bad");
        S[NS].type = g32(DB + o + 12);
        S[NS].h = DB + o; S[NS].hl = hl;
        S[NS].b = DB + o + hl; S[NS].bl = tl - hl;
        NS++;
        o += tl;
    }
    ALS = -1;
    for (int i = 0; i < NS; i++) {
        const uint8_t *b = S[i].b;
        uint32_t bl = S[i].bl, n, p;
        if (S[i].type != 1 && S[i].type != 4) continue;
        if (bl < 12) return fail("iTunesDB list bad");
        n = g32(b + 8); p = g32(b + 4);
        if (S[i].type == 4) {
            if (LS_MEMCMP(b, "mhla", 4) || ALS >= 0) return fail("iTunesDB album list bad");
            ALS = i;
            continue;
        }
        {
            if (LS_MEMCMP(b, "mhlt", 4) || NOT) return fail("iTunesDB track list bad");
            NEW(OT, (n + 1) * sizeof *OT);
            for (uint32_t k = 0; k < n; k++) {
                uint32_t hl, tl;
                if (p + 16 > bl || LS_MEMCMP(b + p, "mhit", 4)) return fail("iTunesDB track bad");
                hl = g32(b + p + 4); tl = g32(b + p + 8);
                if (hl < 0x30 || tl < hl || tl > bl - p) return fail("iTunesDB track size bad");
                OT[k].h = (uint8_t *)(b + p); OT[k].hl = hl;
                OT[k].m = b + p + hl; OT[k].ml = tl - hl; OT[k].nm = g32(b + p + 12);
                OT[k].used = 0;
                p += tl;
            }
            NOT = (int)n;
        }
    }
    tmpl_hl = NOT ? OT[0].hl : MHIT_DEFLEN;
    if (tmpl_hl < 0x170) return fail("iTunesDB track header too short");
    return 0;
}

/* Album table sized for the albums present plus one per file (called after the scan). */
static int load_albums(void)
{
    const uint8_t *b;
    uint32_t bl, n, p;
    NAL = 0;
    if (ALS < 0) { CAPAL = NF + 1; NEW(AL, CAPAL * sizeof *AL); return 0; }
    b = S[ALS].b; bl = S[ALS].bl; n = g32(b + 8); p = g32(b + 4);
    CAPAL = (int)n + NF + 1;
    NEW(AL, CAPAL * sizeof *AL);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t hl, tl;
        if (p + 32 > bl || LS_MEMCMP(b + p, "mhia", 4)) return fail("iTunesDB album bad");
        hl = g32(b + p + 4); tl = g32(b + p + 8);
        if (hl < 32 || tl < hl || tl > bl - p) return fail("iTunesDB album size bad");
        AL[NAL].p = b + p; AL[NAL].len = tl; AL[NAL].id = g32(b + p + 16); AL[NAL].mine = 0;
        mhod_find(b + p + hl, tl - hl, 200, &AL[NAL].k[0], &AL[NAL].kn[0]);
        mhod_find(b + p + hl, tl - hl, 201, &AL[NAL].k[1], &AL[NAL].kn[1]);
        NAL++;
        p += tl;
    }
    return 0;
}

/* ---------------- scanning /Music ---------------- */
static int de_cmp(const void *a, const void *b)
{
    const uint8_t *x = (const uint8_t *)((const de_t *)a)->name;
    const uint8_t *y = (const uint8_t *)((const de_t *)b)->name;
    while (*x && *x == *y) { x++; y++; }
    return (int)*x - (int)*y;
}
static int is_flac_name(const char *n)
{
    size_t l = LS_STRLEN(n);
    static const char ext[] = ".flac";
    if (l < 5 || (n[0] == '.' && n[1] == '_')) return 0;
    for (int i = 0; i < 5; i++) {
        char c = n[l - 5 + i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != ext[i]) return 0;
    }
    return 1;
}
static int fadd(const char *p, uint32_t size, uint32_t mtime)
{
    struct fchunk *c = FCL;
    struct fent *e;
    if (!c || c->n == FCHUNK) {
        NEW(c, sizeof *c);
        c->next = 0; c->n = 0;
        if (FCL) FCL->next = c; else FC0 = c;
        FCL = c;
    }
    e = &c->e[c->n++];
    e->path = p; e->size = size; e->mtime = mtime; e->old = -1; e->renamed = 0;
    NF++;
    return 0;
}
static int compact_files(void)
{
    int k = 0;
    NEW(F, (NF + 1) * sizeof *F);
    for (struct fchunk *c = FC0; c; c = c->next) { LS_MEMCPY(F + k, c->e, c->n * sizeof *F); k += c->n; }
    return 0;
}
/* Scan order as os.walk: a folder's files (sorted), then its subfolders (sorted). */
static int scan(int rl, int depth)
{
    struct ls_dirent e;
    int start = DET, i, first = 0;
    size_t tsave = t_used;
    void *d;
    LS_SNPRINTF(g_path, sizeof g_path, "%s%s", OP->root, g_rel);
    d = ls_opendir(g_path);
    if (!d) {
        /* Abort on an unreadable folder: treating it as empty would look like deletions. */
        size_t l = LS_STRLEN(g_rel);
        LS_SNPRINTF(R->msg, sizeof R->msg, "can't open %s%s", l > 70 ? "..." : "", g_rel + (l > 70 ? l - 70 : 0));
        return -1;
    }
    while (ls_readdir(d, &e)) {
        size_t n = LS_STRLEN(e.name);
        char *nm;
        if (e.name[0] == '.' && (!e.name[1] || (e.name[1] == '.' && !e.name[2]))) continue;
        if (DET == MAXDE) { ls_closedir(d); return fail("too many entries in one folder"); }
        if (!(nm = tmem(n + 1))) { ls_closedir(d); return fail("out of memory"); }
        LS_MEMCPY(nm, e.name, n + 1);
        DE[DET].name = nm; DE[DET].size = e.size; DE[DET].mtime = e.mtime; DE[DET].is_dir = e.is_dir ? 1 : 0;
        DET++;
    }
    ls_closedir(d);
    ls_qsort(DE + start, DET - start, sizeof *DE, de_cmp);
    for (i = start; i < DET; i++) {
        size_t n = LS_STRLEN(DE[i].name);
        char *p;
        if (DE[i].is_dir || !is_flac_name(DE[i].name)) continue;
        if (NF == MAXFILES) return fail("too many .flac files");
        if (rl + 1 + n >= PATHMAX) return fail("path too long");
        NEW(p, rl + n + 2);
        LS_MEMCPY(p, g_rel, rl);
        p[rl] = '/';
        LS_MEMCPY(p + rl + 1, DE[i].name, n + 1);
        if (fadd(p, DE[i].size, DE[i].mtime) < 0) return -1;
        if (!first++ && OP->album_cb) OP->album_cb(OP->cb_ctx, g_rel, p, DE + start, DET - start);
    }
    for (i = start; i < DET; i++) {
        size_t n = LS_STRLEN(DE[i].name);
        if (!DE[i].is_dir) continue;
        if (depth >= MAXDEPTH) return fail("folders nested too deep");
        if (rl + 1 + n >= PATHMAX) return fail("path too long");
        g_rel[rl] = '/';
        LS_MEMCPY(g_rel + rl + 1, DE[i].name, n + 1);
        if (scan(rl + 1 + (int)n, depth + 1) < 0) return -1;
        g_rel[rl] = 0;
    }
    DET = start;
    t_used = tsave;
    return 0;
}

/* ---------------- matching files to entries ---------------- */
/* File location as UTF-16 units, NFC-composed, for comparison. */
static int loc_key(const uint8_t *u16, uint32_t n, uint16_t *out)
{
    int k = 0;
    for (uint32_t i = 0; i + 1 < n; i += 2) {
        uint32_t c = g16(u16 + i), m;
        if (k && c >= 0x300 && c <= 0x36f && (m = nfc_pair(out[k - 1], c))) { out[k - 1] = (uint16_t)m; continue; }
        out[k++] = (uint16_t)c;
    }
    return k;
}
static uint32_t key_hash(const uint16_t *k, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= k[i]; h *= 16777619u; }
    return h;
}
static int match_files(void)
{
    uint32_t cap = 64, mask;
    int *tab, i, nflac = 0;
    uint16_t *kb;
    uint8_t *u16;
    struct { uint16_t *k; int n; } *K;
    for (i = 0; i < NOT; i++) if (g32(OT[i].h + 24) == FLAC_TYPE) nflac++;
    while (cap < 2 * (uint32_t)nflac + 2) cap <<= 1;
    mask = cap - 1;
    NEW(tab, cap * sizeof *tab);
    NEW(K, (NOT + 1) * sizeof *K);
    for (uint32_t j = 0; j < cap; j++) tab[j] = -1;
    for (i = 0; i < NOT; i++) {
        const uint8_t *s; uint32_t n;
        K[i].k = 0; K[i].n = 0;
        if (g32(OT[i].h + 24) != FLAC_TYPE || !mhod_find(OT[i].m, OT[i].ml, 2, &s, &n)) continue;
        NEW(K[i].k, n + 2);
        K[i].n = loc_key(s, n, K[i].k);
        for (uint32_t h = key_hash(K[i].k, K[i].n) & mask; ; h = (h + 1) & mask)
            if (tab[h] < 0) { tab[h] = i; break; }
    }
    NEW(u16, 4 * PATHMAX);
    NEW(kb, 4 * PATHMAX);
    for (int f = 0; f < NF; f++) {
        uint32_t n = u8to16((const uint8_t *)F[f].path, (int)LS_STRLEN(F[f].path), u16, 1);
        int kn = loc_key(u16, n, kb);
        for (uint32_t h = key_hash(kb, kn) & mask; tab[h] >= 0; h = (h + 1) & mask) {
            int t = tab[h];
            if (K[t].n == kn && !LS_MEMCMP(K[t].k, kb, kn * 2) && !OT[t].used) {
                OT[t].used = 1;
                F[f].old = t;
                break;
            }
        }
    }
    return 0;
}

/* Title tag, or else the file name without extension (as os.path.splitext: last dot, unless
   only dots precede it). */
static void pick_title(const struct fent *f, const struct tags *tg, const uint8_t **title, int *titlen, int *fromname)
{
    *title = tg->v[T_TITLE]; *titlen = tg->n[T_TITLE]; *fromname = 0;
    if (!*title || !*titlen) {
        int l = (int)LS_STRLEN(f->path), base, dot, k;
        for (base = l; base > 0 && f->path[base - 1] != '/'; base--);
        for (dot = l - 1; dot > base && f->path[dot] != '.'; dot--);
        if (dot <= base || f->path[dot] != '.') dot = l;
        else {
            for (k = base; k < dot && f->path[k] == '.'; k++);
            if (k == dot) dot = l;
        }
        *title = (const uint8_t *)f->path + base; *titlen = dot - base; *fromname = 1;
    }
}

/* Finder writes the location mhod (type 2) last; entries are kept in that form. */
static int last_is_loc(const struct trk *t)
{
    uint32_t mo = 0, last = 0, k;
    for (k = 0; k < t->nm && mo + 16 <= t->ml; k++) {
        uint32_t l = g32(t->m + mo + 8);
        if (l < 16) break;
        last = g32(t->m + mo + 12);
        mo += l;
    }
    return last == 2;
}
static int has_marker(const struct trk *t)
{
    return t->hl >= LS_MARK + 16 && !LS_MEMCMP(t->h + LS_MARK, "FLSY", 4) && g16(t->h + LS_MARK + 4) == LS_VER;
}
/* Entries the OS would not resolve, or that come from an older sync, are rebuilt once. */
static int needs_rebuild(const struct trk *t)
{
    if (!last_is_loc(t)) return 1;
    if (t->hl >= LS_MARK + 16 && !has_marker(t)) return 1;
    return 0;
}

/* ---------------- renames ---------------- */
static int u16ieq(const uint16_t *a, const uint16_t *b, int n)
{
    for (int i = 0; i < n; i++) if (lc(a[i]) != lc(b[i])) return 0;
    return 1;
}
/* tag (UTF-8) against a string record (UTF-16): equal if both absent or the same text */
static int tag_eq(const struct trk *t, uint32_t type, const uint8_t *v, int n, uint8_t *u16)
{
    const uint8_t *s; uint32_t sn, w;
    int have = mhod_find(t->m, t->ml, type, &s, &sn);
    if (!v || !n) return !have || !sn;
    if (!have) return 0;
    w = u8to16(v, n, NULL, 0);
    if (w > 4 * PATHMAX - 2 || w != sn) return 0;
    u8to16(v, n, u16, 0);
    return !LS_MEMCMP(u16, s, sn);
}
/* Carries a removed entry over to a new file that moved or was renamed: same sample count, and
   either the same file name or the same title, album and artist. Length alone is not enough. */
static int match_renames(void)
{
    int nnew = 0, nrem = 0, *rem, i, f;
    uint16_t *kb, *kb2;
    uint8_t *u16;
    for (f = 0; f < NF; f++) if (F[f].old < 0) nnew++;
    for (i = 0; i < NOT; i++) if (g32(OT[i].h + 24) == FLAC_TYPE && !OT[i].used) nrem++;
    if (!nnew || !nrem) return 0;
    NEW(rem, nrem * sizeof *rem);
    nrem = 0;
    for (i = 0; i < NOT; i++) if (g32(OT[i].h + 24) == FLAC_TYPE && !OT[i].used) rem[nrem++] = i;
    NEW(u16, 4 * PATHMAX); NEW(kb, 4 * PATHMAX); NEW(kb2, 4 * PATHMAX);
    for (f = 0; f < NF; f++) {
        struct tags tg;
        uint32_t sr = 0;
        uint64_t ns = 0;
        const uint8_t *title;
        const char *base;
        int titlen, fromname, bn, kn;
        if (F[f].old >= 0) continue;
        LS_SNPRINTF(g_path, sizeof g_path, "%s%s", OP->root, F[f].path);
        if (read_flac(g_path, &sr, &ns, &tg) < 0) continue;
        pick_title(&F[f], &tg, &title, &titlen, &fromname);
        for (base = F[f].path, bn = 0; F[f].path[bn]; bn++) if (F[f].path[bn] == '/') base = F[f].path + bn + 1;
        bn = (int)LS_STRLEN(base);
        kn = loc_key(u16, u8to16((const uint8_t *)base, bn, u16, 1), kb);
        for (i = 0; i < nrem; i++) {
            const struct trk *t;
            const uint8_t *s; uint32_t n;
            int hit = 0;
            if (rem[i] < 0) continue;
            t = &OT[rem[i]];
            if (t->hl < 0xc4 || g64(t->h + 0xbc) != ns) continue;
            if (mhod_find(t->m, t->ml, 2, &s, &n)) {
                int on = loc_key(s, n, kb2), ks;
                for (ks = on; ks > 0 && kb2[ks - 1] != ':'; ks--);
                hit = on - ks == kn && u16ieq(kb2 + ks, kb, kn);
            }
            if (!hit && !fromname)
                hit = tag_eq(t, 1, title, titlen, u16) && tag_eq(t, 3, tg.v[T_ALBUM], tg.n[T_ALBUM], u16) &&
                      tag_eq(t, 4, tg.v[T_ARTIST], tg.n[T_ARTIST], u16);
            if (hit) {
                OT[rem[i]].used = 1;
                F[f].old = rem[i];
                F[f].renamed = 1;
                rem[i] = -1;
                break;
            }
        }
    }
    return 0;
}

/* ---------------- building entries ---------------- */
static uint32_t mhod_len(uint32_t n16) { return 40 + n16; }
static uint8_t *put_mhod(uint8_t *o, uint32_t type, const uint8_t *s8, int n8, int colon)
{
    uint32_t n = u8to16(s8, n8, o + 40, colon);
    LS_MEMCPY(o, "mhod", 4);
    p32(o + 4, 24); p32(o + 8, 40 + n); p32(o + 12, type);
    p32(o + 16, 0); p32(o + 20, 0); p32(o + 24, 1); p32(o + 28, n); p32(o + 32, 1); p32(o + 36, 0);
    return o + 40 + n;
}
static int u16eq(const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn)
{
    return an == bn && (!an || !LS_MEMCMP(a, b, an));
}
static uint32_t max_album_id(void)
{
    uint32_t m = 0;
    for (int i = 0; i < NAL; i++) if (AL[i].id > m) m = AL[i].id;
    return m;
}
/* album id for (album, album artist or artist), new mhia if needed */
static int album_for(const uint8_t *al, int aln, const uint8_t *ar, int arn, uint32_t *id)
{
    uint8_t *k0, *k1, *a, *q;
    uint32_t n0 = u8to16(al, aln, NULL, 0), n1 = u8to16(ar, arn, NULL, 0), len;
    int nm = (aln > 0) + (arn > 0);
    NEW(k0, n0 + 2); NEW(k1, n1 + 2);
    u8to16(al, aln, k0, 0); u8to16(ar, arn, k1, 0);
    for (int i = 0; i < NAL; i++) {
        if (OP->full && !AL[i].mine) continue;   /* full mode: flacsync.py builds a fresh album map */
        if (u16eq(AL[i].k[0], AL[i].kn[0], k0, n0) && u16eq(AL[i].k[1], AL[i].kn[1], k1, n1)) {
            *id = AL[i].id;
            return 0;
        }
    }
    if (NAL == CAPAL) return fail("too many albums");
    len = 88 + (aln > 0 ? mhod_len(n0) : 0) + (arn > 0 ? mhod_len(n1) : 0);
    NEW(a, len);
    LS_MEMSET(a, 0, 88);
    *id = max_album_id() + 1;
    LS_MEMCPY(a, "mhia", 4);
    p32(a + 4, 88); p32(a + 8, len); p32(a + 12, nm); p32(a + 16, *id);
    rnd8(a + 20);
    p32(a + 28, 2);
    q = a + 88;
    if (aln > 0) q = put_mhod(q, 200, al, aln, 0);
    if (arn > 0) q = put_mhod(q, 201, ar, arn, 0);
    AL[NAL].p = a; AL[NAL].len = len; AL[NAL].id = *id; AL[NAL].mine = 1;
    AL[NAL].k[0] = k0; AL[NAL].kn[0] = n0; AL[NAL].k[1] = k1; AL[NAL].kn[1] = n1;
    NAL++;
    return 0;
}

/* Builds the entry for f as flacsync.py add_track() does. old, if set, is the entry replaced:
   its id, dbid, dates, counts, rating and artwork carry over. Returns 1 added, 0 skipped, -1 error. */
static int make_track(const struct fent *f, uint32_t id, const struct trk *old)
{
    struct tags tg;
    uint32_t sr = 0, ms, kbps, aid = 0, n, hl = tmpl_hl;
    uint64_t ns = 0;
    const uint8_t *title, *aa;
    int titlen, aan, yl, fromname;
    uint8_t *h, *q;
    union { float f; uint32_t u; } fsr;
    struct trk *t = &T[NT];

    LS_SNPRINTF(g_path, sizeof g_path, "%s%s", OP->root, f->path);
    if (read_flac(g_path, &sr, &ns, &tg) < 0)
        return 0;
    ms = (uint32_t)(ns * 1000 / sr);
    kbps = (uint32_t)((uint64_t)f->size * 8 / (ms ? ms : 1));

    pick_title(f, &tg, &title, &titlen, &fromname);
    aa = tg.v[T_ALBUMARTIST]; aan = tg.n[T_ALBUMARTIST];
    if (!aa || !aan) { aa = tg.v[T_ARTIST]; aan = tg.n[T_ARTIST]; }
    if (!aa) aan = 0;

    /* mhods, in flacsync.py's order */
    n = mhod_len(u8to16(title, titlen, NULL, 0))
      + mhod_len(u8to16((const uint8_t *)f->path, (int)LS_STRLEN(f->path), NULL, 0))
      + mhod_len(30);
    for (int j = 0; j < NTAGS; j++)
        if ((j == T_ALBUM || j == T_ARTIST || j == T_GENRE || j == T_COMPOSER || j == T_ALBUMARTIST) && tg.n[j])
            n += mhod_len(u8to16(tg.v[j], tg.n[j], NULL, 0));
    NEW(h, hl + n);
    LS_MEMSET(h, 0, hl);

    if (!old) {
        uint8_t dbid[8];
        rnd8(dbid);
        LS_MEMCPY(h + 0x70, dbid, 8);
    }
    if (album_for(tg.v[T_ALBUM], tg.v[T_ALBUM] ? tg.n[T_ALBUM] : 0, aa, aan, &aid) < 0)
        return -1;

    LS_MEMCPY(h, "mhit", 4);
    p32(h + 4, hl); p32(h + 16, id); p32(h + 20, 1); p32(h + 24, FLAC_TYPE);
    h[28] = 1; h[29] = 1;
    p32(h + 32, mac_time(OP->full ? OP->now : f->mtime));
    p32(h + 36, f->size);
    p32(h + 40, ms);
    p32(h + 44, tag_num(tg.v[T_TRACKNUMBER], tg.n[T_TRACKNUMBER], 0));
    n = tag_num(tg.v[T_TRACKTOTAL], tg.n[T_TRACKTOTAL], 0);
    p32(h + 48, n ? n : tag_num(tg.v[T_TRACKNUMBER], tg.n[T_TRACKNUMBER], 1));
    yl = tg.v[T_DATE] ? first_chars(tg.v[T_DATE], tg.n[T_DATE], 4) : 0;
    p32(h + 52, tag_num(tg.v[T_DATE], yl, 0));
    p32(h + 56, kbps);
    p32(h + 60, (sr < 0xffff ? sr : 0xffff) << 16);
    p32(h + 0x5c, tag_num(tg.v[T_DISCNUMBER], tg.n[T_DISCNUMBER], 0));
    p32(h + 0x68, mac_time(OP->now));
    h[0x78] = 1;
    fsr.f = (float)sr;
    p32(h + 0x88, fsr.u);
    p64(h + 0xbc, ns);
    p32(h + 0xd0, 1);
    p32(h + 0x120, aid);
    LS_MEMCPY(h + 0x124, DB + 0x24, 8);
    p32(h + 0x12c, f->size);
    p64(h + 0x134, 0x808080808080ULL);
    p32(h + 0x168, 1);
    if (old && old->hl >= 0x164) {
        /* Keep the play and sync state the iPod and iTunes wrote. */
        static const uint16_t keep[][2] = {
            { 0x10, 4 }, { 0x1f, 1 }, { 0x40, 0x1c }, { 0x68, 0x10 }, { 0x79, 1 },
            { 0x7c, 8 }, { 0x9c, 9 }, { 0xb2, 1 }, { 0x160, 4 } };
        for (unsigned j = 0; j < sizeof keep / sizeof keep[0]; j++)
            LS_MEMCPY(h + keep[j][0], old->h + keep[j][0], keep[j][1]);
    }
    /* Fields as Finder writes them: the unplayed mark, the gapless flag and the tail stamps
       (the loader does not read these, but listed entries carry them). */
    if (!h[0xb2]) h[0xb2] = 2;          /* unplayed, unless the old entry said played */
    p32(h + 0x100, 1);
    if (hl >= 0x1f8) { p32(h + 0x1e0, 0x7f); p32(h + 0x1f4, id + 1); }
    if (hl >= LS_MARK + 16) {
        uint32_t fl = 0;
        if (fromname) fl |= LS_F_FNTITLE;
        if (!tg.n[T_ALBUM]) fl |= LS_F_NOALBUM;
        if (!tg.n[T_ARTIST] && !tg.n[T_ALBUMARTIST]) fl |= LS_F_NOARTIST;
        LS_MEMCPY(h + LS_MARK, "FLSY", 4);
        p16(h + LS_MARK + 4, LS_VER); p16(h + LS_MARK + 6, fl); p32(h + LS_MARK + 8, OP->now);
        if (fl & LS_F_NOALBUM) R->noalbum++;
    }

    /* Strings in Finder's order: title, artist, album artist, composer, album, genre, type, location. */
    q = h + hl;
    q = put_mhod(q, 1, title, titlen, 0);
    n = 1;
    if (tg.n[T_ARTIST])      { q = put_mhod(q, 4, tg.v[T_ARTIST], tg.n[T_ARTIST], 0); n++; }
    if (tg.n[T_ALBUMARTIST]) { q = put_mhod(q, 22, tg.v[T_ALBUMARTIST], tg.n[T_ALBUMARTIST], 0); n++; }
    if (tg.n[T_COMPOSER])    { q = put_mhod(q, 12, tg.v[T_COMPOSER], tg.n[T_COMPOSER], 0); n++; }
    if (tg.n[T_ALBUM])       { q = put_mhod(q, 3, tg.v[T_ALBUM], tg.n[T_ALBUM], 0); n++; }
    if (tg.n[T_GENRE])       { q = put_mhod(q, 5, tg.v[T_GENRE], tg.n[T_GENRE], 0); n++; }
    q = put_mhod(q, 6, (const uint8_t *)"FLAC audio file", 15, 0); n++;
    q = put_mhod(q, 2, (const uint8_t *)f->path, (int)LS_STRLEN(f->path), 1); n++;
    p32(h + 8, (uint32_t)(q - h));
    p32(h + 12, n);
    t->h = h; t->hl = hl; t->m = h + hl; t->ml = (uint32_t)(q - h) - hl; t->nm = n; t->used = 1;
    NT++;
    return 1;
}

/* ---------------- Play Counts ---------------- */
/* Merges the iPod's Play Counts file (one entry per track, in iTunesDB order) into the
   tracks, as libgpod does. Runs before the track order changes. */
static int merge_playcounts(const char *path)
{
    int fd = ls_open_r(path);
    long n;
    uint8_t *b;
    uint32_t hl, el, cnt;
    if (fd < 0) return 0;
    n = ls_fsize(fd);
    if (n < 16 || n > 8 * 1024 * 1024 || !(b = amem(n)) || ls_read(fd, b, n) != n) { ls_close(fd); return 0; }
    ls_close(fd);
    hl = g32(b + 4); el = g32(b + 8); cnt = g32(b + 12);
    if (LS_MEMCMP(b, "mhdp", 4) || el < 12 || cnt != (uint32_t)NOT || hl > (uint32_t)n ||
        (uint64_t)hl + (uint64_t)cnt * el > (uint64_t)n)
        return 0;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *e = b + hl + i * el;
        uint8_t *h = OT[i].h;
        uint32_t pc = g32(e), lp = g32(e + 4), bm = g32(e + 8);
        if (OT[i].hl < 0xb4) continue;
        if (el >= 16) h[0x1f] = (uint8_t)g32(e + 12);
        if (lp) p32(h + 0x58, lp);
        if (bm) p32(h + 0x6c, bm);
        p32(h + 0x50, g32(h + 0x50) + pc);
        p32(h + 0x54, g32(h + 0x54) + pc);
        if (pc) h[0xb2] = 1;
        if (el >= 0x1c) {
            p32(h + 0x9c, g32(h + 0x9c) + g32(e + 20));
            if (g32(e + 24)) p32(h + 0xa0, g32(e + 24));
        }
    }
    return 1;
}

/* ---------------- writing ---------------- */
static uint8_t *O; static uint32_t ON, OCAP; static int OERR;
static uint8_t *oput(const void *d, uint32_t n)
{
    uint8_t *p;
    if (OERR || n > OCAP - ON) { OERR = 1; return NULL; }
    p = O + ON;
    if (d) LS_MEMCPY(p, d, n); else LS_MEMSET(p, 0, n);
    ON += n;
    return p;
}
static void oput32(uint32_t v) { uint8_t b[4]; p32(b, v); oput(b, 4); }
static void ofix32(uint32_t at, uint32_t v) { if (!OERR) p32(O + at, v); }

struct key { const uint8_t *s[6]; uint32_t n[6]; uint32_t cd, tn; };
static struct key *KY;
static int sort_st;
static const char *sort_fields(int st)
{
    switch (st) {
    case 3: return "T";
    case 4: return "LDNT";
    case 5: return "RLDNT";
    case 7: return "GRLDNT";
    case 0x12: return "CLDNT";
    case 0x23: case 0x24: return "ALDNT";
    }
    return "";
}
static int scmp(const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn)
{
    uint32_t i;
    for (i = 0; i + 1 < an && i + 1 < bn; i += 2) {
        uint32_t x = lc(g16(a + i)), y = lc(g16(b + i));
        if (x != y) return x < y ? -1 : 1;
    }
    return (an > bn) - (an < bn);
}
static int idx_cmp(const void *pa, const void *pb)
{
    uint32_t a = *(const uint32_t *)pa, b = *(const uint32_t *)pb;
    const struct key *x = &KY[a], *y = &KY[b];
    for (const char *f = sort_fields(sort_st); *f; f++) {
        int c = 0, i;
        switch (*f) {
        case 'D': if (x->cd != y->cd) return x->cd < y->cd ? -1 : 1; continue;
        case 'N': if (x->tn != y->tn) return x->tn < y->tn ? -1 : 1; continue;
        case 'A': {
            int ix = x->n[5] ? 5 : 2, iy = y->n[5] ? 5 : 2;
            c = scmp(x->s[ix], x->n[ix], y->s[iy], y->n[iy]);
            break; }
        default:
            i = *f == 'T' ? 0 : *f == 'L' ? 1 : *f == 'R' ? 2 : *f == 'G' ? 3 : 4;
            c = scmp(x->s[i], x->n[i], y->s[i], y->n[i]);
        }
        if (c) return c;
    }
    return (a > b) - (a < b);   /* stable, like Python's sort */
}
static uint32_t letter(const struct key *k, int st)
{
    int i = st == 3 ? 0 : st == 4 ? 1 : st == 5 ? 2 : st == 7 ? 3 : st == 0x12 ? 4 : -1;
    if (i < 0) return 0;
    for (uint32_t o = 0; o + 1 < k->n[i]; o += 2) {
        uint32_t c = g16(k->s[i] + o);
        if (c >= 0xd800 && c <= 0xdbff && o + 3 < k->n[i]) {
            c = 0x10000 + ((c - 0xd800) << 10) + (g16(k->s[i] + o + 2) - 0xdc00);
            o += 2;
        }
        if (is_alnum(c)) return is_digit(c) ? 0 : (uc(c) & 0xffff);
    }
    return 0;
}
static int put_index(int st, uint32_t *ix)
{
    uint32_t nj = 0, L = 0, i;
    for (i = 0; i < (uint32_t)NT; i++) ix[i] = i;
    sort_st = st;
    if (*sort_fields(st))
        ls_qsort(ix, NT, sizeof *ix, idx_cmp);
    oput("mhod", 4); oput32(24); oput32(4 * NT + 72); oput32(52); oput32(0); oput32(0);
    oput32(st); oput32(NT); oput(NULL, 40);
    for (i = 0; i < (uint32_t)NT; i++) oput32(ix[i]);
    if (st != 3 && st != 5 && st != 4 && st != 7 && st != 0x12 && st != 0x1d)
        return 0;
    for (i = 0; i < (uint32_t)NT; i++) {
        uint32_t l = letter(&KY[ix[i]], st);
        if (!i || l != L) { nj++; L = l; }
    }
    oput("mhod", 4); oput32(24); oput32(12 * nj + 40); oput32(53); oput32(0); oput32(0);
    oput32(st); oput32(nj); oput(NULL, 8);
    for (i = 0; i < (uint32_t)NT; ) {
        uint32_t j = i;
        L = letter(&KY[ix[i]], st);
        while (j < (uint32_t)NT && letter(&KY[ix[j]], st) == L) j++;
        uint8_t e[12];
        p16(e, L); p16(e + 2, 0); p32(e + 4, i); p32(e + 8, j - i);
        oput(e, 12);
        i = j;
    }
    return 0;
}

struct mip { uint32_t id, pos; const uint8_t *p; uint32_t len; };
static struct mip *MP;      /* allocated before the output buffer, which takes all memory left */
static int mip_cmp(const void *a, const void *b)
{
    const struct mip *x = a, *y = b;
    if (x->id != y->id) return x->id < y->id ? -1 : 1;
    return (x->pos > y->pos) - (x->pos < y->pos);
}
/* As flacsync.py mk_master(): new sort indexes and one playlist item per track. A track reuses
   the old item with its id, else one copied from the first old item. */
static int put_master(const uint8_t *pl, uint32_t pll, uint32_t *ix)
{
    uint32_t hl = g32(pl + 4), nmh = g32(pl + 12), nit = g32(pl + 16), o = hl, start = ON, kept = 0, i;
    static const int SORTS[] = { 3, 5, 4, 7, 0x12, 0x23, 0x24, 0x1d, 0x1e, 0x1f };
    struct mip *mp;
    uint8_t tmpl_buf[0x78];
    const uint8_t *tmpl;
    uint32_t tl;
    if (hl < 0x14 || hl > pll) return fail("master playlist bad");
    oput(pl, hl);
    for (i = 0; i < nmh; i++) {
        uint32_t l;
        if (o + 16 > pll || (l = g32(pl + o + 8)) < 16 || l > pll - o) return fail("master playlist bad");
        if (g32(pl + o + 12) != 52 && g32(pl + o + 12) != 53) { oput(pl + o, l); kept++; }
        o += l;
    }
    for (i = 0; i < sizeof SORTS / sizeof SORTS[0]; i++) put_index(SORTS[i], ix);
    mp = MP;
    for (i = 0; i < nit; i++) {
        uint32_t l;
        if (o + 28 > pll || (l = g32(pl + o + 8)) < 28 || l > pll - o) return fail("playlist item bad");
        mp[i].id = g32(pl + o + 24); mp[i].pos = i; mp[i].p = pl + o; mp[i].len = l;
        o += l;
    }
    if (nit) {
        tmpl = mp[0].p; tl = mp[0].len;
        if (tl < 0x34 || g32(tmpl + 4) + 28 > tl) return fail("playlist item too short");
    } else {
        /* no item to copy: the layout Finder writes */
        LS_MEMSET(tmpl_buf, 0, sizeof tmpl_buf);
        LS_MEMCPY(tmpl_buf, "mhip", 4);
        p32(tmpl_buf + 4, 0x4c); p32(tmpl_buf + 8, 0x78); p32(tmpl_buf + 12, 1);
        p32(tmpl_buf + 0x1c, mac_time(OP->now));
        LS_MEMCPY(tmpl_buf + 0x4c, "mhod", 4);
        p32(tmpl_buf + 0x50, 0x18); p32(tmpl_buf + 0x54, 0x2c); p32(tmpl_buf + 0x58, 100);
        tmpl = tmpl_buf; tl = sizeof tmpl_buf;
    }
    ls_qsort(mp, nit, sizeof *mp, mip_cmp);
    /* Playlist item ids (mhip +20 and the mhod inside) must be unique in the whole database,
       or the OS drops those tracks from its lists. next_item starts above every existing item. */
    for (int t = 0; t < NT; t++) {
        uint32_t id = g32(T[t].h + 16);
        int lo = 0, hi = (int)nit - 1, found = -1;
        while (lo <= hi) {                    /* last item with this id, as a dict keeps it */
            int mid = (lo + hi) / 2;
            if (mp[mid].id <= id) { if (mp[mid].id == id) found = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        if (found >= 0) {
            oput(mp[found].p, mp[found].len);
        } else {
            uint8_t *p = oput(tmpl, tl);
            if (p) {
                next_item++;
                p32(p + 20, next_item);
                p32(p + 24, id);
                LS_MEMCPY(p + 0x2c, T[t].h + 0x70, 8);
                p32(p + g32(p + 4) + 24, next_item);
            }
        }
    }
    ofix32(start + 8, ON - start);
    ofix32(start + 12, kept + 16);
    ofix32(start + 16, NT);
    return 0;
}

/* Largest item count of any master playlist (sizes put_master's work), and the highest item id
   in any playlist (new items are numbered above it). */
static uint32_t max_master_items(void)
{
    uint32_t m = 0;
    item_base = 0;
    for (int s = 0; s < NS; s++) {
        const uint8_t *b = S[s].b;
        uint32_t o, n;
        if ((S[s].type != 2 && S[s].type != 3) || S[s].bl < 12) continue;
        o = g32(b + 4); n = g32(b + 8);
        for (uint32_t i = 0; i < n && o + 24 <= S[s].bl; i++) {
            uint32_t l = g32(b + o + 8), hl = g32(b + o + 4), nmh = g32(b + o + 12), nit = g32(b + o + 16), q;
            if (l < 24 || l > S[s].bl - o) break;
            if (b[o + 20] == 1 && nit > m) m = nit;
            q = o + hl;
            for (uint32_t k = 0; k < nmh + nit && q + 24 <= o + l; k++) {
                uint32_t ml = g32(b + q + 8);
                if (ml < 24 || ml > o + l - q) break;
                if (k >= nmh && !LS_MEMCMP(b + q, "mhip", 4) && g32(b + q + 20) > item_base) item_base = g32(b + q + 20);
                q += ml;
            }
            o += l;
        }
    }
    return m;
}

/* ---------------- playlists: the .m3u files in /Playlists ---------------- */
/* Each .m3u/.m3u8 in /Playlists becomes an iPod playlist named after the file. Our playlist ids
 * have the top byte PL_TAG, so a resync replaces only those and leaves Finder's playlists alone.
 * The timestamp field holds the file's mtime. "#" lines are skipped ("#PLAYLIST:name" sets the
 * name). Paths may start with /<HDD0> and may use backslashes. Tracks are matched by location. */
#define PL_TAG   0xe5u
#define MAXPL    64
#define PLBUF    (256 * 1024)
struct plf { const char *path; const char *name; int nameln; uint32_t mtime, size; uint64_t id; int seen, ntix; uint16_t *tix; };
static struct plf PL[MAXPL]; static int NPL, pl_changed;

static uint64_t pl_id(const char *path)
{
    uint64_t h = 14695981039346656037ULL;
    for (; *path; path++) { h ^= (uint8_t)*path; h *= 1099511628211ULL; }
    return (h & 0x00ffffffffffffffULL) | (uint64_t)PL_TAG << 56;
}
static int is_m3u(const char *n)
{
    size_t l = LS_STRLEN(n);
    if (n[0] == '.' && n[1] == '_') return 0;
    if (l > 4 && (n[l - 4] | 32) == '.' && (n[l - 3] | 32) == 'm' && n[l - 2] == '3' && (n[l - 1] | 32) == 'u') return 4;
    if (l > 5 && n[l - 5] == '.' && (n[l - 4] | 32) == 'm' && n[l - 3] == '3' && (n[l - 2] | 32) == 'u' && n[l - 1] == '8') return 5;
    return 0;
}
static int plf_cmp(const void *a, const void *b)
{
    const uint8_t *x = (const uint8_t *)((const struct plf *)a)->path, *y = (const uint8_t *)((const struct plf *)b)->path;
    while (*x && *x == *y) { x++; y++; }
    return (int)*x - (int)*y;
}
static int pl_scan(void)
{
    struct ls_dirent e;
    void *d;
    NPL = 0; pl_changed = 0;
    LS_SNPRINTF(g_path, sizeof g_path, "%s/Playlists", OP->root);
    if (!(d = ls_opendir(g_path))) return 0;             /* no folder: no playlists */
    while (ls_readdir(d, &e)) {
        int ext = e.is_dir ? 0 : is_m3u(e.name);
        size_t n = LS_STRLEN(e.name);
        char *p, *nm;
        if (!ext) continue;
        if (NPL == MAXPL) break;
        if (!(p = amem(n + 12)) || !(nm = amem(n + 1))) { ls_closedir(d); return fail("out of memory"); }
        LS_SNPRINTF(p, n + 12, "/Playlists/%s", e.name);
        LS_MEMCPY(nm, e.name, n - ext); nm[n - ext] = 0;
        PL[NPL].path = p; PL[NPL].name = nm; PL[NPL].nameln = (int)(n - ext);
        PL[NPL].mtime = e.mtime; PL[NPL].size = e.size; PL[NPL].id = pl_id(p);
        PL[NPL].seen = 0; PL[NPL].ntix = 0; PL[NPL].tix = 0;
        NPL++;
    }
    ls_closedir(d);
    ls_qsort(PL, NPL, sizeof *PL, plf_cmp);
    return 0;
}
/* Sets pl_changed if our playlists in the database differ from the files. */
static void pl_check(void)
{
    int ours = 0, i;
    for (int s = 0; s < NS; s++) {
        const uint8_t *b = S[s].b;
        uint32_t hl, n, o;
        if (S[s].type != 3 || S[s].bl < 12) continue;
        hl = g32(b + 4); n = g32(b + 8); o = hl;
        for (uint32_t k = 0; k < n && o + 0x28 <= S[s].bl; k++) {
            uint32_t l = g32(b + o + 8);
            if (l < 0x28 || l > S[s].bl - o) break;
            if (b[o + 20] != 1 && b[o + 0x1c + 7] == PL_TAG) {
                uint64_t id = g64(b + o + 0x1c);
                int found = 0;
                ours++;
                for (i = 0; i < NPL; i++)
                    if (PL[i].id == id) { found = 1; if (g32(b + o + 0x18) == mac_time(PL[i].mtime)) PL[i].seen = 1; }
                if (!found) pl_changed = 1;
            }
            o += l;
        }
    }
    for (i = 0; i < NPL; i++) if (!PL[i].seen) pl_changed = 1;
    (void)ours;
}
/* Resolves each playlist line to a track index (T must be final). */
static int pl_resolve(void)
{
    uint32_t cap = 64, mask, *tab;
    struct { uint16_t *k; int n; } *K;
    uint16_t *kb;
    uint8_t *u16, *buf;
    if (!NPL) return 0;
    while (cap < 2 * (uint32_t)NT + 2) cap <<= 1;
    mask = cap - 1;
    NEW(tab, cap * sizeof *tab);
    NEW(K, (NT + 1) * sizeof *K);
    for (uint32_t j = 0; j < cap; j++) tab[j] = 0xffffffffu;
    for (int t = 0; t < NT; t++) {
        const uint8_t *s; uint32_t n;
        K[t].k = 0; K[t].n = 0;
        if (!mhod_find(T[t].m, T[t].ml, 2, &s, &n)) continue;
        NEW(K[t].k, n + 2);
        K[t].n = loc_key(s, n, K[t].k);
        for (uint32_t h = key_hash(K[t].k, K[t].n) & mask; ; h = (h + 1) & mask)
            if (tab[h] == 0xffffffffu) { tab[h] = (uint32_t)t; break; }
    }
    NEW(u16, 4 * PATHMAX); NEW(kb, 4 * PATHMAX); NEW(buf, PLBUF);
    for (int i = 0; i < NPL; i++) {
        char *line;
        long n, p;
        int fd;
        uint16_t *tix;
        NEW(tix, 4096 * sizeof *tix);
        PL[i].tix = tix; PL[i].ntix = 0;
        LS_SNPRINTF(g_path, sizeof g_path, "%s%s", OP->root, PL[i].path);
        if ((fd = ls_open_r(g_path)) < 0) continue;
        n = ls_read(fd, buf, PLBUF - 1); ls_close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        p = 0;
        if (n >= 3 && buf[0] == 0xef && buf[1] == 0xbb && buf[2] == 0xbf) p = 3;
        while (p < n) {
            long e = p, q;
            char path[PATHMAX];
            int len = 0, kn;
            while (e < n && buf[e] != '\n') e++;
            line = (char *)buf + p; q = e;
            while (q > p && (buf[q - 1] == '\r' || buf[q - 1] == ' ')) q--;
            if (q > p && line[0] == '#') {
                if (q - p > 10 && !LS_MEMCMP(line, "#PLAYLIST:", 10)) {
                    char *nm = amem(q - p - 10 + 1);
                    if (nm) { LS_MEMCPY(nm, line + 10, q - p - 10); nm[q - p - 10] = 0; PL[i].name = nm; PL[i].nameln = (int)(q - p - 10); }
                }
                p = e + 1; continue;
            }
            if (q == p) { p = e + 1; continue; }
            if (q - p >= 7 && !LS_MEMCMP(line, "/<HDD0>", 7)) { line += 7; }
            if (line[0] != '/' && line[0] != '\\') { LS_MEMCPY(path, "/Music/", 7); len = 7; }
            for (long k = 0; line + k < (char *)buf + q && len < PATHMAX - 1; k++) path[len++] = line[k] == '\\' ? '/' : line[k];
            path[len] = 0;
            kn = loc_key(u16, u8to16((const uint8_t *)path, len, u16, 1), kb);
            for (uint32_t h = key_hash(kb, kn) & mask; tab[h] != 0xffffffffu; h = (h + 1) & mask) {
                int t = (int)tab[h];
                if (K[t].n == kn && !LS_MEMCMP(K[t].k, kb, kn * 2)) { if (PL[i].ntix < 4096) tix[PL[i].ntix++] = (uint16_t)t; break; }
            }
            p = e + 1;
        }
    }
    return 0;
}
/* Writes one of our playlists, using the master's item layout. */
static int put_playlist(const uint8_t *master, uint32_t ml, const struct plf *pl)
{
    uint32_t hl = g32(master + 4), nmh = g32(master + 12), nit = g32(master + 16), o = hl, start = ON;
    uint8_t namebuf[40 + 2 * 256], *h;
    const uint8_t *tmpl = 0;
    uint32_t tl = 0, n16;
    uint8_t tmpl_buf[0x78];
    if (hl < 0x28 || hl > ml) return fail("master playlist bad");
    h = oput(master, hl);
    if (!h) return 0;
    h[0x14] = 0; p32(h + 0x0c, 3); p32(h + 0x10, pl->ntix); p32(h + 0x18, mac_time(pl->mtime));
    p32(h + 0x1c, (uint32_t)pl->id); p32(h + 0x20, (uint32_t)(pl->id >> 32));
    n16 = u8to16((const uint8_t *)pl->name, pl->nameln > 255 ? 255 : pl->nameln, NULL, 0);
    put_mhod(namebuf, 1, (const uint8_t *)pl->name, pl->nameln > 255 ? 255 : pl->nameln, 0);
    oput(namebuf, 40 + n16);
    for (uint32_t i = 0; i < nmh; i++) {
        uint32_t l;
        if (o + 16 > ml || (l = g32(master + o + 8)) < 16 || l > ml - o) return fail("master playlist bad");
        if (g32(master + o + 12) == 100 || g32(master + o + 12) == 102) oput(master + o, l);
        o += l;
    }
    for (uint32_t i = 0; i < nit; i++) {
        uint32_t l;
        if (o + 28 > ml || (l = g32(master + o + 8)) < 28 || l > ml - o) return fail("playlist item bad");
        if (!tmpl) { tmpl = master + o; tl = l; }
        o += l;
    }
    if (!tmpl || tl < 0x34 || g32(tmpl + 4) + 28 > tl) {
        LS_MEMSET(tmpl_buf, 0, sizeof tmpl_buf);
        LS_MEMCPY(tmpl_buf, "mhip", 4);
        p32(tmpl_buf + 4, 0x4c); p32(tmpl_buf + 8, 0x78); p32(tmpl_buf + 12, 1);
        p32(tmpl_buf + 0x1c, mac_time(OP->now));
        LS_MEMCPY(tmpl_buf + 0x4c, "mhod", 4);
        p32(tmpl_buf + 0x50, 0x18); p32(tmpl_buf + 0x54, 0x2c); p32(tmpl_buf + 0x58, 100);
        tmpl = tmpl_buf; tl = sizeof tmpl_buf;
    }
    for (int k = 0; k < pl->ntix; k++) {
        uint8_t *p = oput(tmpl, tl);
        uint32_t gid, id = g32(T[pl->tix[k]].h + 16);
        if (!p) break;
        gid = ++next_item;
        p32(p + 0x14, gid);
        p32(p + 0x18, id);
        LS_MEMCPY(p + 0x2c, T[pl->tix[k]].h + 0x70, 8);
        p32(p + g32(p + 4) + 24, gid);
    }
    ofix32(start + 8, ON - start);
    return 0;
}


/* ---------------- verifying an image ---------------- */
/* Checks the rules the OS relies on before a new image replaces the old database: lengths,
   string order, unique ids, existing albums, sort indexes that are permutations with contiguous
   letter tables, playlist items that match the track list, item ids unique per section.
   Stops at the first violation. */
struct vs { uint8_t *b; size_t n, u; };
static void *vmem(struct vs *v, size_t n)
{
    void *p;
    n = (n + 7) & ~(size_t)7;
    if (n > v->n - v->u) return NULL;
    p = v->b + v->u; v->u += n;
    return p;
}
static int u32cmp(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return (x > y) - (x < y); }
static int u64cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }
static int vfail(char *msg, size_t sz, const char *what, uint32_t at)
{
    LS_SNPRINTF(msg, sz, "verify: %s at 0x%x", what, (unsigned)at);
    return -1;
}
static int in_sorted(const uint32_t *a, uint32_t n, uint32_t v)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (a[m] < v) lo = m + 1; else hi = m; }
    return lo < n && a[lo] == v;
}
#define VF(cond, what, at) do { if (cond) return vfail(msg, msgsz, what, (uint32_t)(at)); } while (0)
int ls_verify(const uint8_t *db, uint32_t len, const uint8_t fwid[8], int marker, int legacy_ok, int npl,
              void *mem, size_t memsz, char *msg, size_t msgsz)
{
    struct vs v = { mem, memsz, 0 };
    uint32_t o, hl, nt = 0, na = 0, *ids = 0, *sids = 0, *aids = 0, *sorted_a = 0;
    uint64_t *dbids = 0;
    uint8_t *aref = 0;
    int legacy = 0, nsec = 0;
    static const uint32_t SORTS[10] = { 3, 5, 4, 7, 0x12, 0x23, 0x24, 0x1d, 0x1e, 0x1f };

    VF(len < 0x100 || LS_MEMCMP(db, "mhbd", 4), "no mhbd header", 0);
    hl = g32(db + 4);
    VF(hl < 0x6c || hl > len || g32(db + 8) != len, "header length", 4);
    VF(g16(db + 0x30) != 1, "header +0x30", 0x30);
    if (fwid) {
        uint8_t hs[20];
        ls_hash58(fwid, db, len, hs);
        VF(LS_MEMCMP(hs, db + 0x58, 20), "signature", 0x58);
    }
    /* pass 1: the section chain, the album list */
    for (o = hl; o < len; nsec++) {
        uint32_t shl, stl, type;
        VF(o + 16 > len || LS_MEMCMP(db + o, "mhsd", 4), "section header", o);
        shl = g32(db + o + 4); stl = g32(db + o + 8); type = g32(db + o + 12);
        VF(shl < 16 || stl < shl || stl > len - o, "section length", o);
        if (type == 4) {
            uint32_t b = o + shl, p, n;
            VF(stl - shl < 12 || LS_MEMCMP(db + b, "mhla", 4), "album list header", b);
            VF(aids != 0, "two album lists", b);
            p = b + g32(db + b + 4); n = g32(db + b + 8);
            aids = vmem(&v, (n + 1) * 4); sorted_a = vmem(&v, (n + 1) * 4); aref = vmem(&v, n + 1);
            VF(!aref, "out of memory", 0);
            for (uint32_t k = 0; k < n; k++) {
                uint32_t ahl, atl;
                VF(p + 32 > o + stl || LS_MEMCMP(db + p, "mhia", 4), "album header", p);
                ahl = g32(db + p + 4); atl = g32(db + p + 8);
                VF(ahl < 32 || atl < ahl || atl > o + stl - p, "album length", p);
                aids[k] = sorted_a[k] = g32(db + p + 16); aref[k] = 0;
                VF(!aids[k], "album id 0", p);
                p += atl;
            }
            na = n;
            ls_qsort(sorted_a, na, 4, u32cmp);
            for (uint32_t k = 1; k < na; k++) VF(sorted_a[k] == sorted_a[k - 1], "duplicate album id", b);
        }
        o += stl;
    }
    VF(o != len, "sections don't cover the file", o);
    /* pass 2: the tracks */
    for (o = hl; o < len; ) {
        uint32_t shl = g32(db + o + 4), stl = g32(db + o + 8), type = g32(db + o + 12);
        if (type == 1) {
            uint32_t b = o + shl, p, n;
            VF(stl - shl < 12 || LS_MEMCMP(db + b, "mhlt", 4), "track list header", b);
            VF(ids != 0, "two track lists", b);
            p = b + g32(db + b + 4); n = g32(db + b + 8);
            ids = vmem(&v, (n + 1) * 4); sids = vmem(&v, (n + 1) * 4); dbids = vmem(&v, (n + 1) * 8);
            VF(!dbids, "out of memory", 0);
            for (uint32_t k = 0; k < n; k++) {
                uint32_t thl, ttl, nm, q, sum = 0, ntitle = 0, nloc = 0, last = 0, flac;
                VF(p + 0x30 > o + stl || LS_MEMCMP(db + p, "mhit", 4), "track header", p);
                thl = g32(db + p + 4); ttl = g32(db + p + 8); nm = g32(db + p + 12);
                VF(thl < 0x30 || ttl < thl || ttl > o + stl - p, "track length", p);
                flac = g32(db + p + 24) == FLAC_TYPE;
                ids[k] = sids[k] = g32(db + p + 16);
                VF(!ids[k], "track id 0", p);
                VF(k && ids[k] <= ids[k - 1], "track ids not ascending", p);
                dbids[k] = thl >= 0x78 ? g64(db + p + 0x70) : 0;
                for (q = p + thl; sum < nm; sum++) {
                    uint32_t ml, mt;
                    VF(q + 40 > p + ttl || LS_MEMCMP(db + q, "mhod", 4), "string record header", q);
                    ml = g32(db + q + 8); mt = g32(db + q + 12);
                    VF(ml < 24 || ml > p + ttl - q, "string record length", q);
                    if (mt == 1 || mt == 2 || mt == 3 || mt == 4 || mt == 5 || mt == 6 || mt == 12 || mt == 22)
                        VF(ml < 40 || 40 + g32(db + q + 28) > ml, "string length", q);
                    if (mt == 1) ntitle++;
                    if (mt == 2) nloc++;
                    last = mt;
                    q += ml;
                }
                if (flac) {
                    VF(q != p + ttl, "string records don't fill the entry", p);
                    VF(ntitle != 1, "title records", p);
                    VF(nloc != 1, "location records", p);
                    if (last != 2 || (marker && thl >= LS_MARK + 16 &&
                        (LS_MEMCMP(db + p + LS_MARK, "FLSY", 4) || g16(db + p + LS_MARK + 4) != LS_VER))) {
                        legacy++;
                        VF(legacy > legacy_ok, last != 2 ? "location not last" : "entry marker", p);
                    }
                    VF(thl < 0x124, "FLAC entry header short", p);
                    if (na) {
                        uint32_t lo = 0, hi = na, aid = g32(db + p + 0x120);
                        while (lo < hi) { uint32_t m = (lo + hi) / 2; if (sorted_a[m] < aid) lo = m + 1; else hi = m; }
                        VF(lo >= na || sorted_a[lo] != aid, "album id not in the album list", p);
                    } else {
                        VF(1, "FLAC entry without an album list", p);
                    }
                }
                if (na && thl >= 0x124) {
                    uint32_t aid = g32(db + p + 0x120);
                    for (uint32_t a = 0; a < na; a++) if (aids[a] == aid) { aref[a] = 1; break; }
                }
                p += ttl;
            }
            nt = n;
            ls_qsort(sids, nt, 4, u32cmp);
            for (uint32_t k = 1; k < nt; k++) VF(sids[k] == sids[k - 1], "duplicate track id", b);
            ls_qsort(dbids, nt, 8, u64cmp);
            for (uint32_t k = 1; k < nt; k++) VF(dbids[k] && dbids[k] == dbids[k - 1], "duplicate dbid", b);
        }
        o += stl;
    }
    VF(!ids, "no track list", 0);
    for (uint32_t a = 0; a < na; a++) VF(!aref[a], "album entry no track uses", a);
    /* pass 3: the playlists */
    for (o = hl; o < len; ) {
        uint32_t shl = g32(db + o + 4), stl = g32(db + o + 8), type = g32(db + o + 12);
        if (type == 2 || type == 3) {
            uint32_t b = o + shl, p, n, nmaster = 0, nours = 0, nitems = 0, *iids, ni = 0;
            VF(stl - shl < 12 || LS_MEMCMP(db + b, "mhlp", 4), "playlist list header", b);
            p = b + g32(db + b + 4); n = g32(db + b + 8);
            for (uint32_t k = 0, q = p; k < n; k++) {         /* items in this section, for the id table */
                uint32_t ptl;
                VF(q + 24 > o + stl || LS_MEMCMP(db + q, "mhyp", 4), "playlist header", q);
                ptl = g32(db + q + 8);
                VF(ptl < 24 || ptl > o + stl - q, "playlist length", q);
                nitems += g32(db + q + 16);
                q += ptl;
            }
            iids = vmem(&v, (nitems + 1) * 4);
            VF(!iids, "out of memory", 0);
            for (uint32_t k = 0; k < n; k++) {
                uint32_t phl = g32(db + p + 4), ptl = g32(db + p + 8), nmh = g32(db + p + 12), nit = g32(db + p + 16), q;
                int master = db[p + 20] == 1, sorts = 0;
                VF(phl < 0x24 || phl > ptl, "playlist header length", p);
                if (master) nmaster++;
                else if (ptl >= 0x28 && db[p + 0x1c + 7] == PL_TAG) nours++;
                q = p + phl;
                for (uint32_t m = 0; m < nmh; m++) {
                    uint32_t ml, mt;
                    VF(q + 24 > p + ptl || LS_MEMCMP(db + q, "mhod", 4), "playlist record header", q);
                    ml = g32(db + q + 8); mt = g32(db + q + 12);
                    VF(ml < 24 || ml > p + ptl - q, "playlist record length", q);
                    if (master && mt == 52) {
                        uint32_t st = g32(db + q + 24), cnt = g32(db + q + 28), si;
                        uint8_t *seen;
                        VF(ml < 72 || cnt != nt || 72 + 4 * cnt > ml, "sort index size", q);
                        for (si = 0; si < 10 && SORTS[si] != st; si++);
                        VF(si == 10 || (sorts & (1 << si)), "sort index type", q);
                        sorts |= 1 << si;
                        seen = vmem(&v, nt + 1);
                        VF(!seen, "out of memory", 0);
                        LS_MEMSET(seen, 0, nt + 1);
                        for (uint32_t i = 0; i < cnt; i++) {
                            uint32_t ix = g32(db + q + 72 + 4 * i);
                            VF(ix >= nt || seen[ix], "sort index not a permutation", q);
                            seen[ix] = 1;
                        }
                        v.u -= (nt + 1 + 7) & ~(size_t)7;
                    } else if (master && mt == 53) {
                        uint32_t nj = g32(db + q + 28), at = 0;
                        VF(ml < 40 || 40 + 12 * nj > ml, "letter table size", q);
                        for (uint32_t i = 0; i < nj; i++) {
                            VF(g32(db + q + 40 + 12 * i + 4) != at, "letter table not contiguous", q);
                            at += g32(db + q + 40 + 12 * i + 8);
                        }
                        VF(nj ? at != nt : nt != 0, "letter table doesn't cover the tracks", q);
                    }
                    q += ml;
                }
                VF(master && sorts != 0x3ff, "master playlist sort indexes", p);
                VF(master && nit != nt, "master playlist item count", p);
                for (uint32_t i = 0; i < nit; i++) {
                    uint32_t il, ihl, iid, tid;
                    VF(q + 28 > p + ptl || LS_MEMCMP(db + q, "mhip", 4), "item header", q);
                    ihl = g32(db + q + 4); il = g32(db + q + 8);
                    VF(il < 28 || il > p + ptl - q || ihl < 28 || ihl > il, "item length", q);
                    iid = g32(db + q + 20); tid = g32(db + q + 24);
                    VF(!iid, "item id 0", q);
                    /* The type-100 record inside is not checked: it holds whatever iTunes left there. */
                    if (master) VF(tid != ids[i], "master item out of step with the track list", q);
                    else VF(!in_sorted(sids, nt, tid), "item for a track that doesn't exist", q);
                    iids[ni++] = iid;
                    q += il;
                }
                VF(q != p + ptl, "playlist records don't fill it", p);
                p += ptl;
            }
            VF(nmaster != 1, "master playlists", b);
            VF(npl >= 0 && (int)nours != npl, "our playlist count", b);
            ls_qsort(iids, ni, 4, u32cmp);
            for (uint32_t i = 1; i < ni; i++) VF(iids[i] == iids[i - 1], "duplicate item id", b);
        }
        o += stl;
    }
    (void)nsec;
    return 0;
}
#undef VF

#ifdef LS_FAULTS
/* Test hooks: corrupt the built image on purpose, to check that ls_verify catches it. */
int ls_fault;
static uint8_t *first_flac(uint8_t *img, uint32_t len)
{
    for (uint32_t o = g32(img + 4); o + 16 <= len; o += g32(img + o + 8)) {
        if (g32(img + o + 12) == 1) {
            uint32_t b = o + g32(img + o + 4), p = b + g32(img + b + 4), n = g32(img + b + 8);
            for (uint32_t k = 0; k < n; k++, p += g32(img + p + 8))
                if (g32(img + p + 24) == FLAC_TYPE) return img + p;
        }
        if (!g32(img + o + 8)) break;
    }
    return 0;
}
static uint8_t *first_master(uint8_t *img, uint32_t len)
{
    for (uint32_t o = g32(img + 4); o + 16 <= len; o += g32(img + o + 8)) {
        if (g32(img + o + 12) == 2 || g32(img + o + 12) == 3) {
            uint32_t b = o + g32(img + o + 4), p = b + g32(img + b + 4), n = g32(img + b + 8);
            for (uint32_t k = 0; k < n; k++, p += g32(img + p + 8))
                if (img[p + 20] == 1) return img + p;
        }
        if (!g32(img + o + 8)) break;
    }
    return 0;
}
static void inject(uint8_t *img, uint32_t len)
{
    uint8_t *e = first_flac(img, len), *m = first_master(img, len), *q;
    uint32_t hl, tl, nm, pl, nmh, nit;
    if (!ls_fault || !e || !m) return;
    hl = g32(e + 4); tl = g32(e + 8); nm = g32(e + 12);
    if (ls_fault == 1 && nm >= 2) {         /* location record moved before the one preceding it */
        uint32_t a = hl, b = hl, i;
        for (i = 0; i + 1 < nm; i++) { a = b; b += g32(e + b + 8); }
        uint32_t la = g32(e + a + 8), lb = g32(e + b + 8);
        uint8_t *t = amem(la + lb);
        if (!t) return;
        LS_MEMCPY(t, e + b, lb); LS_MEMCPY(t + lb, e + a, la);
        LS_MEMCPY(e + a, t, la + lb);
    } else if (ls_fault == 5) {
        p32(e + 0x120, 0xfffffff0u);
    } else if (ls_fault == 6) {
        p32(e + hl + 8, g32(e + hl + 8) + 1);
    } else if (ls_fault == 2 || ls_fault == 3) {
        pl = g32(m + 4); nmh = g32(m + 12); nit = g32(m + 16);
        q = m + pl;
        for (uint32_t i = 0; i < nmh; i++) {
            if (ls_fault == 3 && g32(q + 12) == 52 && g32(q + 28) >= 2) { p32(q + 76, g32(q + 72)); return; }
            q += g32(q + 8);
        }
        if (ls_fault == 2 && nit >= 2) {
            uint8_t *q2 = q + g32(q + 8);
            p32(q2 + 20, g32(q + 20));
            p32(q2 + g32(q2 + 4) + 24, g32(q + 20));
        }
    }
    (void)tl;
}
#endif

static int trk_id_cmp(const void *a, const void *b)
{
    uint32_t x = g32(((const struct trk *)a)->h + 16), y = g32(((const struct trk *)b)->h + 16);
    return (x > y) - (x < y);
}
static int build_db(void)
{
    uint32_t *ix;
    uint8_t *vscratch;
    size_t vsz;
    NEW(KY, (NT + 1) * sizeof *KY);
    NEW(ix, (NT + 1) * sizeof *ix);
    NEW(MP, (max_master_items() + 1) * sizeof *MP);
    vsz = 64 * 1024 + 48 * (size_t)(NT + NAL + 1);
    for (int i = 0; i < NPL; i++) vsz += 4 * (size_t)PL[i].ntix;
    NEW(vscratch, vsz);
    for (int t = 0; t < NT; t++) {
        static const uint32_t types[6] = { 1, 3, 4, 5, 12, 22 };
        for (int j = 0; j < 6; j++)
            mhod_find(T[t].m, T[t].ml, types[j], &KY[t].s[j], &KY[t].n[j]);
        KY[t].cd = g32(T[t].h + 0x5c);
        KY[t].tn = g32(T[t].h + 0x2c);
    }
    O = m_base + m_used; OCAP = (uint32_t)(m_size - m_used); ON = 0; OERR = 0;
    oput(DB, g32(DB + 4));
    for (int s = 0; s < NS; s++) {
        uint32_t start = ON;
        const uint8_t *b = S[s].b;
        oput(S[s].h, S[s].hl);
        if (S[s].type == 1) {
            oput("mhlt", 4); oput32(92); oput32(NT); oput(NULL, 80);
            for (int t = 0; t < NT; t++) {
                uint8_t *h;
                T[t].oo = ON;
                h = oput(T[t].h, T[t].hl);
                if (h) { p32(h + 8, T[t].hl + T[t].ml); p32(h + 12, T[t].nm); }
                oput(T[t].m, T[t].ml);
            }
        } else if (S[s].type == 4) {
            uint8_t *h = oput(b, g32(b + 4));
            int n = 0;
            for (int a = 0; a < NAL; a++) {
                int used = 0;
                for (int t = 0; t < NT && !used; t++) used = g32(T[t].h + 0x120) == AL[a].id;
                if (used) { oput(AL[a].p, AL[a].len); n++; }
            }
            if (h && !OERR) p32(h + 8, n);
        } else if (S[s].type == 2 || S[s].type == 3) {
            uint32_t hl = g32(b + 4), n = g32(b + 8), o = hl, kept = 0, lstart = ON;
            const uint8_t *master = 0;
            uint32_t master_len = 0;
            if (hl > S[s].bl) return fail("playlist list bad");
            next_item = item_base;      /* both playlist sections number new items the same way */
            oput(b, hl);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t l;
                if (o + 24 > S[s].bl || (l = g32(b + o + 8)) < 24 || l > S[s].bl - o) return fail("playlist bad");
                if (b[o + 20] == 1) { if (put_master(b + o, l, ix) < 0) return -1; master = b + o; master_len = l; kept++; }
                else if (l >= 0x28 && b[o + 0x1c + 7] == PL_TAG) { /* ours from last time: rebuilt below */ }
                else { oput(b + o, l); kept++; }
                o += l;
            }
            if (master)
                for (int i = 0; i < NPL; i++) { if (put_playlist(master, master_len, &PL[i]) < 0) return -1; kept++; }
            ofix32(lstart + 8, kept);
        } else {
            oput(b, S[s].bl);
        }
        ofix32(start + 8, ON - start);
    }
    ofix32(8, ON);
    if (OERR) return fail("out of memory writing iTunesDB");
    p16(O + 0x30, 1);
    ls_hash58(OP->fwid, O, ON, O + 0x58);
    m_used += ON;
#ifdef LS_FAULTS
    inject(O, ON);
    if (ls_fault == 4) O[0x58] ^= 1;
    else if (ls_fault) ls_hash58(OP->fwid, O, ON, O + 0x58);
#endif
    /* every entry carried over is in the image byte for byte */
    for (int t = 0; t < NT; t++)
        if (T[t].h >= DB && T[t].h < DB + DBN && LS_MEMCMP(O + T[t].oo, T[t].h, T[t].hl + T[t].ml))
            return fail("verify: a kept entry changed");
    if (ls_verify(O, ON, OP->fwid, tmpl_hl >= LS_MARK + 16, R->skipped, NPL,
                  vscratch, vsz, R->msg, sizeof R->msg) < 0)
        return -1;
    R->verified = 1;
    return 0;
}

static int write_file(const char *path, const uint8_t *d, uint32_t n)
{
    int fd = ls_open_w(path);
    long w = 0;
    if (fd < 0) return -1;
    while ((uint32_t)w < n) {
        long k = ls_write(fd, d + w, (long)(n - w) > 0x40000 ? 0x40000 : (long)(n - w));
        if (k <= 0) break;
        w += k;
    }
    if (ls_close(fd) < 0 || (uint32_t)w != n) return -1;
    fd = ls_open_r(path);
    if (fd < 0) return -1;
    w = ls_fsize(fd);
    ls_close(fd);
    return (uint32_t)w == n ? 0 : -1;
}

/* ---------------- the sync ---------------- */
static char dbp[PATHMAX], tmp[PATHMAX], bak[PATHMAX], pcp[PATHMAX];  /* static, off the plugin's small stack */

int ls_sync(const struct ls_opts *o, void *mem, size_t memsz, struct ls_result *r)
{
    int fd, i, nflac_old = 0, todo = 0, done = 0, unordered = 0;
    uint32_t next_id = 0;
    long n;

    LS_MEMSET(r, 0, sizeof *r);
    R = r; OP = o;
    m_base = mem; m_used = 0; m_size = memsz;
    LS_MEMCPY(r_seed, o->seed, 8); r_ctr = 0;
    NT = NOT = NAL = NF = DET = 0; AL = 0; OT = 0; FC0 = FCL = 0; t_used = 0; O = 0; ON = 0;

    if (o->db_in) LS_SNPRINTF(dbp, sizeof dbp, "%s", o->db_in);
    else LS_SNPRINTF(dbp, sizeof dbp, "%s/iPod_Control/iTunes/iTunesDB", o->root);
    LS_SNPRINTF(tmp, sizeof tmp, "%s.new", dbp);
    LS_SNPRINTF(bak, sizeof bak, "%s.old", dbp);
    LS_SNPRINTF(pcp, sizeof pcp, "%s/iPod_Control/iTunes/Play Counts", o->root);

    /* A replace cut short (new file complete, old one moved away) is finished first. */
    fd = ls_open_r(dbp);
    if (fd < 0 && !o->db_out && !o->dry && ls_rename(tmp, dbp) == 0) fd = ls_open_r(dbp);
    if (fd < 0) return fail("no iTunesDB");
    if (!o->db_out && !o->dry) ls_remove(tmp);
    n = ls_fsize(fd);
    if (n < 0x100 || n > 64L * 1024 * 1024 || !(DB = amem(n))) { ls_close(fd); return fail("iTunesDB size bad"); }
    if (ls_read(fd, DB, n) != n) { ls_close(fd); return fail("can't read iTunesDB"); }
    ls_close(fd);
    DBN = (uint32_t)n;
    if (parse_db() < 0) return -1;
    for (i = 0; i < NOT; i++) {
        if (g32(OT[i].h + 16) > next_id) next_id = g32(OT[i].h + 16);
        if (g32(OT[i].h + 24) == FLAC_TYPE) nflac_old++;
    }

    NEW(DE, MAXDE * sizeof *DE);
    NEW(tagbuf, TAGLIM);
    ls_status("Scanning /Music");
    LS_SNPRINTF(g_rel, sizeof g_rel, "/Music");
    if (scan(6, 0) < 0) return -1;
    if (compact_files() < 0) return -1;
    r->flacs = NF;
    if (!o->full && !NF && nflac_old) {
        /* An empty or missing /Music is more likely a read failure than an intent to drop the library. */
        LS_SNPRINTF(r->msg, sizeof r->msg, "no .flac files found, keeping %d tracks", nflac_old);
        return -1;
    }
    if (load_albums() < 0) return -1;
    if (pl_scan() < 0) return -1;

    if (o->full) {
        /* As flacsync.py: new ids follow the highest non-FLAC id; albums are kept only for
           non-FLAC tracks. */
        int k = 0;
        next_id = 0;
        for (i = 0; i < NOT; i++)
            if (g32(OT[i].h + 24) != FLAC_TYPE && g32(OT[i].h + 16) > next_id) next_id = g32(OT[i].h + 16);
        for (int a = 0; a < NAL; a++) {
            int used = 0;
            for (i = 0; i < NOT && !used; i++)
                used = g32(OT[i].h + 24) != FLAC_TYPE && g32(OT[i].h + 0x120) == AL[a].id;
            if (used) AL[k++] = AL[a];
        }
        NAL = k;
        r->removed = nflac_old;
        todo = NF;
    } else {
        if (match_files() < 0) return -1;
        if (match_renames() < 0) return -1;
        for (int f = 0; f < NF; f++) {
            const struct trk *t = F[f].old >= 0 ? &OT[F[f].old] : NULL;
            if (!t || F[f].renamed) todo++;
            else if (g32(t->h + 0x24) != F[f].size || g32(t->h + 0x20) != mac_time(F[f].mtime)) todo++;
            else if (needs_rebuild(t)) todo++;
        }
        r->removed = nflac_old;
        for (i = 0; i < NOT; i++) if (OT[i].used) r->removed--;
        pl_check();
        /* a list not in ascending id order is rewritten even when no file changed */
        for (i = 1; i < NOT; i++) if (g32(OT[i].h + 16) <= g32(OT[i - 1].h + 16)) { unordered = 1; break; }
        if (!todo && !r->removed && !pl_changed && !unordered) {
            r->kept = NF;
            r->tracks = NOT;
            return 0;
        }
    }

    /* Refuse to write over a database signed for another iPod. */
    {
        uint8_t hs[20];
        ls_hash58(o->fwid, DB, DBN, hs);
        if (LS_MEMCMP(hs, DB + 0x58, 20)) { r->badsig = 1; return fail("iTunesDB signature doesn't match this iPod"); }
    }
    if (!o->full && !o->db_out)
        r->playcounts = merge_playcounts(pcp);

    NEW(T, (NOT + NF + 1) * sizeof *T);
    for (i = 0; i < NOT; i++)
        if (g32(OT[i].h + 24) != FLAC_TYPE) T[NT++] = OT[i];
    for (int f = 0; f < NF; f++) {
        const struct trk *old = (!o->full && F[f].old >= 0) ? &OT[F[f].old] : NULL;
        int rc, same = old && !F[f].renamed && g32(old->h + 0x24) == F[f].size && g32(old->h + 0x20) == mac_time(F[f].mtime);
        if (same && !needs_rebuild(old)) {
            T[NT++] = *old;
            r->kept++;
            if (has_marker(old) && (g16(old->h + LS_MARK + 6) & LS_F_NOALBUM)) r->noalbum++;
            continue;
        }
        if (!(done++ & 7)) {
            char m[48];
            LS_SNPRINTF(m, sizeof m, "Reading tags %d/%d", done, todo);
            ls_status(m);
        }
        rc = make_track(&F[f], old ? g32(old->h + 16) : next_id + 1, old);
        if (rc < 0) return -1;
        if (!rc) {
            /* not readable now: an entry it already had stays as it was */
            r->skipped++;
            if (old) { T[NT++] = *old; r->kept++; }
            continue;
        }
        if (!old) { r->added++; next_id++; }
        else if (F[f].renamed) r->renamed++;
        else if (same) r->migrated++;
        else r->updated++;
    }
    r->tracks = NT;
    if (!o->full && !r->added && !r->updated && !r->renamed && !r->migrated && !r->removed && !pl_changed && !unordered)
        return 0;   /* only unreadable files differed: nothing to write */
    if (unordered) r->reordered = 1;
    /* The track list is written in ascending id order, as iTunes and Finder write it. The sort
       indexes and playlist items are built from this order. */
    ls_qsort(T, NT, sizeof *T, trk_id_cmp);

    if (pl_resolve() < 0) return -1;
    r->playlists = NPL;
    ls_status("Writing iTunesDB");
    if (build_db() < 0) {
        /* Keep a rejected image for inspection; the old database stays. */
        if (!o->dry && O && ON && !LS_MEMCMP(r->msg, "verify:", 7)) {
            LS_SNPRINTF(tmp, sizeof tmp, "%s/FLAC/iTunesDB.rejected", o->root);
            ls_remove(tmp);
            write_file(tmp, O, ON);
        }
        return -1;
    }
    if (o->dry) return 0;
    if (o->db_out) {
        if (write_file(o->db_out, O, ON) < 0) return fail("can't write the new iTunesDB");
    } else {
        if (write_file(tmp, O, ON) < 0) { ls_remove(tmp); return fail("can't write iTunesDB.new"); }
        ls_remove(bak);
        if (ls_rename(dbp, bak) < 0) { ls_remove(tmp); return fail("can't move the old iTunesDB"); }
        if (ls_rename(tmp, dbp) < 0) { ls_rename(bak, dbp); return fail("can't put the new iTunesDB in place"); }
        if (r->playcounts) ls_remove(pcp);
    }
    r->wrote = 1;
    return 0;
}
