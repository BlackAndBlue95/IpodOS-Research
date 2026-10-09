/* rbflac: freestanding single-frame FLAC decoder, ported from Rockbox
 * libffmpegFLAC.  See rbflac.h and README.md.
 *
 * Derived from:
 *   decoder.c       (c) 2003 Alex Beregszaszi            LGPL 2.1+
 *   golomb.h        (c) 2003 Michael Niedermayer, 2004 A. Beregszaszi
 *   ffmpeg_get_bits.h (c) 2004 Michael Niedermayer       LGPL 2.1+
 *   arm.S           (c) 2006 Thom Johansen               GPL 2+
 *
 * Changes vs Rockbox:
 *  - no output-depth shift (native bit depth), no downmix, no yield
 *  - bounds-checked bit reader (corrupt data never reads more than
 *    RBFLAC_PAD bytes past buf+len), no UB on corrupt input
 *  - byte-wise header parse with the same acceptance rule as flacblob's
 *    parse_fh (CRC8, stream-param match), 64-bit sample numbers
 *  - 32-bit LPC path chosen by an exact overflow bound (sum|c| * 2^(bps-1)
 *    < 2^31) instead of bps+prec+log2(order) <= 32 using the *frame* bps,
 *    which ignored the side channel's extra bit
 *  - lpc_analyze_remodulate() (emulation of broken encoders) dropped
 *  - escape-coded partitions with 0 or > 25 bits fixed, k up to 30
 *  - optional CRC16 check
 */
#include <stdint.h>
#include <stddef.h>
#include "rbflac.h"

#if defined(__arm__) && !defined(__thumb__) && !defined(RBFLAC_NO_ASM)
#define RBFLAC_ARM_ASM 1
void rbflac_lpc_decode_arm(int n, int qlevel, int order, int32_t *data,
                           const int32_t *coeffs);
void rbflac_lpc_wide_arm(int n, int qlevel, int order, int32_t *data,
                         const int32_t *rc);
#endif

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* ---------------- bit reader (ffmpeg ALT_BITSTREAM_READER) ------------- */
typedef struct {
    const uint8_t *buf;
    unsigned idx;      /* bit index */
    unsigned size;     /* size in bits */
} GB;

static inline uint32_t rb32(const uint8_t *p)
{
    uint32_t a = p[0], b = p[1], c = p[2], d = p[3];
#ifdef __arm__
    /* stop gcc's bswap pass from turning this into 4 x ldrb + LE compose +
       4-insn byte swap (ARMv5 has no rev): 7 insns instead of 11 */
    __asm__("" : "+r"(a), "+r"(c));
#endif
    return (a << 24) | (b << 16) | (c << 8) | d;
}
/* cache with >= 25 valid bits at the top */
static inline uint32_t gb_cache(const GB *g)
{
    return rb32(g->buf + (g->idx >> 3)) << (g->idx & 7);
}
static inline unsigned get_bits(GB *g, int n)        /* 1..25 */
{
    uint32_t c = gb_cache(g);
    g->idx += n;
    return c >> (32 - n);
}
static inline int get_sbits(GB *g, int n)             /* 1..25 */
{
    uint32_t c = gb_cache(g);
    g->idx += n;
    return (int32_t)c >> (32 - n);
}
static inline unsigned get_bits1(GB *g)
{
    unsigned i = g->idx++;
    return (g->buf[i >> 3] >> (7 - (i & 7))) & 1;
}
static inline unsigned get_bits_long(GB *g, int n)   /* 0..32 */
{
    if (n <= 25) return n ? get_bits(g, n) : 0;
    { unsigned hi = get_bits(g, 16); return (hi << (n - 16)) | get_bits(g, n - 16); }
}
static inline int32_t get_sbits_long(GB *g, int n)   /* 1..32 */
{
    if (n <= 25) return get_sbits(g, n);
    return (int32_t)(get_bits_long(g, n) << (32 - n)) >> (32 - n);
}
static inline int gb_left(const GB *g, unsigned bits) /* 1 if bits available */
{
    return g->idx <= g->size && bits <= g->size - g->idx;
}

/* ---------------- tables ---------------- */
static const uint8_t table_crc8[256] = {
    0x00,0x07,0x0e,0x09,0x1c,0x1b,0x12,0x15,0x38,0x3f,0x36,0x31,0x24,0x23,0x2a,0x2d,
    0x70,0x77,0x7e,0x79,0x6c,0x6b,0x62,0x65,0x48,0x4f,0x46,0x41,0x54,0x53,0x5a,0x5d,
    0xe0,0xe7,0xee,0xe9,0xfc,0xfb,0xf2,0xf5,0xd8,0xdf,0xd6,0xd1,0xc4,0xc3,0xca,0xcd,
    0x90,0x97,0x9e,0x99,0x8c,0x8b,0x82,0x85,0xa8,0xaf,0xa6,0xa1,0xb4,0xb3,0xba,0xbd,
    0xc7,0xc0,0xc9,0xce,0xdb,0xdc,0xd5,0xd2,0xff,0xf8,0xf1,0xf6,0xe3,0xe4,0xed,0xea,
    0xb7,0xb0,0xb9,0xbe,0xab,0xac,0xa5,0xa2,0x8f,0x88,0x81,0x86,0x93,0x94,0x9d,0x9a,
    0x27,0x20,0x29,0x2e,0x3b,0x3c,0x35,0x32,0x1f,0x18,0x11,0x16,0x03,0x04,0x0d,0x0a,
    0x57,0x50,0x59,0x5e,0x4b,0x4c,0x45,0x42,0x6f,0x68,0x61,0x66,0x73,0x74,0x7d,0x7a,
    0x89,0x8e,0x87,0x80,0x95,0x92,0x9b,0x9c,0xb1,0xb6,0xbf,0xb8,0xad,0xaa,0xa3,0xa4,
    0xf9,0xfe,0xf7,0xf0,0xe5,0xe2,0xeb,0xec,0xc1,0xc6,0xcf,0xc8,0xdd,0xda,0xd3,0xd4,
    0x69,0x6e,0x67,0x60,0x75,0x72,0x7b,0x7c,0x51,0x56,0x5f,0x58,0x4d,0x4a,0x43,0x44,
    0x19,0x1e,0x17,0x10,0x05,0x02,0x0b,0x0c,0x21,0x26,0x2f,0x28,0x3d,0x3a,0x33,0x34,
    0x4e,0x49,0x40,0x47,0x52,0x55,0x5c,0x5b,0x76,0x71,0x78,0x7f,0x6a,0x6d,0x64,0x63,
    0x3e,0x39,0x30,0x37,0x22,0x25,0x2c,0x2b,0x06,0x01,0x08,0x0f,0x1a,0x1d,0x14,0x13,
    0xae,0xa9,0xa0,0xa7,0xb2,0xb5,0xbc,0xbb,0x96,0x91,0x98,0x9f,0x8a,0x8d,0x84,0x83,
    0xde,0xd9,0xd0,0xd7,0xc2,0xc5,0xcc,0xcb,0xe6,0xe1,0xe8,0xef,0xfa,0xfd,0xf4,0xf3
};

static const int sample_rate_table[12] =
{ 0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000 };
static const int8_t sample_size_table[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };

/* ---------------- header ---------------- */
int rbflac_parse_header(const rbflac_stream *st, const uint8_t *p, int len,
                        rbflac_frame *h)
{
    int i, n, bsc, src, chc, bpc, ones, bs, sr, nch, bps, crc;
    uint64_t v;
    /* each length test below is exact: ESHORT only when the header really
       does not fit (the old parse_fh demanded 16 bytes up front) */
    if (len < 2) return RBFLAC_ESHORT;
    if (p[0] != 0xff || (p[1] & 0xfe) != 0xf8) return RBFLAC_ESYNC;
    if (len < 6) return RBFLAC_ESHORT;
    bsc = p[2] >> 4; src = p[2] & 15; chc = p[3] >> 4; bpc = (p[3] >> 1) & 7;
    if (bsc == 0 || src == 15 || chc > 10 || bpc == 3 || (p[3] & 1))
        return RBFLAC_ESYNC;
    i = 4;
    v = p[i++];
    ones = 0;
    while (ones < 8 && (v & (0x80 >> ones))) ones++;
    if (ones == 1 || ones > 7) return RBFLAC_ESYNC;
    /* utf8 tail + up to 2 bs + 2 sr bytes + crc8 */
    n = i + (ones ? ones - 1 : 0) + (bsc == 6 ? 1 : bsc == 7 ? 2 : 0) +
        (src == 12 ? 1 : src >= 13 ? 2 : 0) + 1;
    if (len < n) return RBFLAC_ESHORT;
    if (ones) {
        v &= (0x7f >> ones);
        for (n = 1; n < ones; n++) {
            if ((p[i] & 0xc0) != 0x80) return RBFLAC_ESYNC;
            v = (v << 6) | (p[i++] & 0x3f);
        }
    }
    if (bsc == 1) bs = 192;
    else if (bsc <= 5) bs = 576 << (bsc - 2);
    else if (bsc == 6) bs = p[i++] + 1;
    else if (bsc == 7) { bs = ((p[i] << 8) | p[i + 1]) + 1; i += 2; }
    else bs = 256 << (bsc - 8);
    if (src == 0) sr = st->sr;
    else if (src < 12) sr = sample_rate_table[src];
    else if (src == 12) sr = p[i++] * 1000;
    else if (src == 13) { sr = (p[i] << 8) | p[i + 1]; i += 2; }
    else { sr = ((p[i] << 8) | p[i + 1]) * 10; i += 2; }
    crc = 0;
    for (n = 0; n < i; n++) crc = table_crc8[crc ^ p[n]];
    if (crc != p[i]) return RBFLAC_ESYNC;
    i++;
    nch = chc < 8 ? chc + 1 : 2;
    bps = bpc ? sample_size_table[bpc] : st->bps;
    if (nch != st->ch || bps != st->bps || sr != st->sr || bs > st->maxbs)
        return RBFLAC_ESYNC;
    h->blocksize = bs;
    h->chmode = chc;
    h->nch = nch;
    h->bps = bps;
    h->hdrlen = i;
    h->sample = (p[1] & 1) ? v : v * (uint64_t)st->maxbs;
    return i;
}

/* Header fields only, no comparison with a stream: for a decoder that configures
 * itself from the packets. *sr / *bps are 0 when the header defers to STREAMINFO.
 * *fixed is 1 when the frame carries a frame number (fixed block size stream).
 * Returns the header length, or RBFLAC_ESYNC / RBFLAC_ESHORT. */
int rbflac_peek_header(const uint8_t *p, int len, int *sr, int *nch, int *bps, int *bs, int *fixed)
{
    int i, n, bsc, src, chc, bpc, ones, crc;
    uint64_t v;
    if (len < 2) return RBFLAC_ESHORT;
    if (p[0] != 0xff || (p[1] & 0xfe) != 0xf8) return RBFLAC_ESYNC;
    if (len < 6) return RBFLAC_ESHORT;
    bsc = p[2] >> 4; src = p[2] & 15; chc = p[3] >> 4; bpc = (p[3] >> 1) & 7;
    if (bsc == 0 || src == 15 || chc > 10 || bpc == 3 || (p[3] & 1))
        return RBFLAC_ESYNC;
    i = 4;
    v = p[i++];
    ones = 0;
    while (ones < 8 && (v & (0x80 >> ones))) ones++;
    if (ones == 1 || ones > 7) return RBFLAC_ESYNC;
    n = i + (ones ? ones - 1 : 0) + (bsc == 6 ? 1 : bsc == 7 ? 2 : 0) +
        (src == 12 ? 1 : src >= 13 ? 2 : 0) + 1;
    if (len < n) return RBFLAC_ESHORT;
    if (ones) {
        v &= (0x7f >> ones);
        for (n = 1; n < ones; n++) {
            if ((p[i] & 0xc0) != 0x80) return RBFLAC_ESYNC;
            v = (v << 6) | (p[i++] & 0x3f);
        }
    }
    if (bsc == 1) *bs = 192;
    else if (bsc <= 5) *bs = 576 << (bsc - 2);
    else if (bsc == 6) *bs = p[i++] + 1;
    else if (bsc == 7) { *bs = ((p[i] << 8) | p[i + 1]) + 1; i += 2; }
    else *bs = 256 << (bsc - 8);
    if (src == 0) *sr = 0;
    else if (src < 12) *sr = sample_rate_table[src];
    else if (src == 12) *sr = p[i++] * 1000;
    else if (src == 13) { *sr = (p[i] << 8) | p[i + 1]; i += 2; }
    else { *sr = ((p[i] << 8) | p[i + 1]) * 10; i += 2; }
    crc = 0;
    for (n = 0; n < i; n++) crc = table_crc8[crc ^ p[n]];
    if (crc != p[i]) return RBFLAC_ESYNC;
    i++;
    *nch = chc < 8 ? chc + 1 : 2;
    *bps = bpc ? sample_size_table[bpc] : 0;
    *fixed = !(p[1] & 1);
    return i;
}

/* ---------------- residuals (decode_residuals + golomb.h) ------------- */
/* One partition of Rice codes with parameter k (0..30), n samples.
 * Returns 0 or -1.  Per-sample bounds check keeps over-read < 12 bytes. */
static int rice_part(GB *g, int32_t *d, int n, int k)
{
    const uint8_t *buf = g->buf;
    unsigned idx = g->idx, lim = g->size;
    if (k <= 24) {
        const uint32_t fast = 1u << (7 + k); /* clz(c)+k <= 24 <=> c >= fast */
        while (n-- > 0) {
            uint32_t c, u;
            if (UNLIKELY(idx > lim)) return -1;
            c = rb32(buf + (idx >> 3)) << (idx & 7);
            if (LIKELY(c >= fast)) {
                /* whole code in cache: get_ur_golomb_jpegls fast path */
                int q = __builtin_clz(c);
                u = (c >> (31 - q - k)) + ((uint32_t)(q - 1) << k);
                idx += q + 1 + k;
            } else {
                uint32_t q = 0;
                for (;;) {
                    c &= 0xffffff80u;           /* 25 valid bits */
                    if (c) { int z = __builtin_clz(c); q += z; idx += z + 1; break; }
                    q += 25; idx += 25;
                    if (UNLIKELY(idx > lim)) return -1;
                    c = rb32(buf + (idx >> 3)) << (idx & 7);
                }
                if (UNLIKELY(q > (0xffffffffu >> k))) return -1;
                u = q << k;
                if (k) {
                    c = rb32(buf + (idx >> 3)) << (idx & 7);
                    u |= c >> (32 - k);
                    idx += k;
                }
            }
            *d++ = (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
        }
    } else {
        GB t; t.buf = buf; t.size = lim;
        while (n-- > 0) {
            uint32_t c, q = 0, u;
            if (UNLIKELY(idx > lim)) return -1;
            c = rb32(buf + (idx >> 3)) << (idx & 7);
            for (;;) {
                c &= 0xffffff80u;
                if (c) { int z = __builtin_clz(c); q += z; idx += z + 1; break; }
                q += 25; idx += 25;
                if (UNLIKELY(idx > lim)) return -1;
                c = rb32(buf + (idx >> 3)) << (idx & 7);
            }
            if (UNLIKELY(q > (0xffffffffu >> k))) return -1;
            t.idx = idx;
            u = (q << k) | get_bits_long(&t, k);
            idx = t.idx;
            *d++ = (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
        }
    }
    g->idx = idx;
    return 0;
}

static int decode_residuals(GB *g, int32_t *decoded, int bs, int pred_order)
{
    int i, partition, method_type, rice_order, rice_bits, rice_esc, samples;

    if (!gb_left(g, 6)) return -1;
    method_type = get_bits(g, 2);
    rice_order  = get_bits(g, 4);
    samples   = bs >> rice_order;
    rice_bits = 4 + method_type;
    rice_esc  = (1 << rice_bits) - 1;
    if (method_type > 1 || (samples << rice_order) != bs || pred_order > samples)
        return -1;

    decoded += pred_order;
    i = pred_order;
    for (partition = 0; partition < (1 << rice_order); partition++) {
        int n = samples - i, tmp;
        if (!gb_left(g, rice_bits + 5)) return -1;
        tmp = get_bits(g, rice_bits);
        if (tmp == rice_esc) {
            tmp = get_bits(g, 5);
            if (!gb_left(g, (unsigned)n * tmp)) return -1;
            if (tmp == 0) {
                for (; n > 0; n--) *decoded++ = 0;
            } else if (tmp <= 25) {
                for (; n > 0; n--) *decoded++ = get_sbits(g, tmp);
            } else {
                for (; n > 0; n--) *decoded++ = get_sbits_long(g, tmp);
            }
        } else {
            if (rice_part(g, decoded, n, tmp)) return -1;
            decoded += n;
        }
        i = 0;
    }
    return 0;
}

/* ---------------- subframes ---------------- */
static int decode_subframe_fixed(GB *g, int32_t *decoded, int bs,
                                 int pred_order, int bps)
{
    uint32_t a, b, c, d;
    int i;
    if (pred_order > bs || !gb_left(g, (unsigned)pred_order * bps)) return -1;
    for (i = 0; i < pred_order; i++)
        decoded[i] = get_sbits(g, bps);
    if (decode_residuals(g, decoded, bs, pred_order) < 0)
        return -1;
    /* Rockbox running-difference form; unsigned = wraps, no UB */
    switch (pred_order) {
    case 0:
        break;
    case 1:
        a = decoded[0];
        for (i = 1; i < bs; i++)
            decoded[i] = a += decoded[i];
        break;
    case 2:
        a = decoded[1];
        b = a - decoded[0];
        for (i = 2; i < bs; i++)
            decoded[i] = a += b += decoded[i];
        break;
    case 3:
        a = decoded[2];
        b = a - decoded[1];
        c = b - decoded[1] + decoded[0];
        for (i = 3; i < bs; i++)
            decoded[i] = a += b += c += decoded[i];
        break;
    case 4:
        a = decoded[3];
        b = a - decoded[2];
        c = b - decoded[2] + decoded[1];
        d = c - decoded[2] + 2U * decoded[1] - decoded[0];
        for (i = 4; i < bs; i++)
            decoded[i] = a += b += c += d += decoded[i];
        break;
    }
    return 0;
}

#ifndef RBFLAC_ARM_ASM
/* C equivalent of lpc_decode_arm: 32-bit wrapping accumulate.
 * data points at the first sample to predict; coeffs[0] pairs with the
 * newest history sample. */
static void lpc_decode_c32(int n, int qlevel, int order, int32_t *data,
                           const int32_t *coeffs)
{
    const int32_t *h = data - order;
    int i, j;
    for (i = 0; i < n; i++, h++) {
        uint32_t s = 0;
        for (j = 0; j < order; j++)
            s += (uint32_t)coeffs[order - 1 - j] * (uint32_t)h[j];
        data[i] = (int32_t)((uint32_t)data[i] + (uint32_t)((int32_t)s >> qlevel));
    }
}
#endif

#ifndef RBFLAC_ARM_ASM
/* 64-bit accumulate (flac_lpc_32_c).  rc[] is coeffs reversed: rc[0] pairs
 * with the oldest history sample. */
static void lpc_decode_wide(int n, int qlevel, int order, int32_t *data,
                            const int32_t *rc)
{
    const int32_t *h = data - order;
    int i, j;
    for (i = 0; i < n; i++, h++) {
        int64_t s = 0;
        for (j = 0; j < order; j++)
            s += (int64_t)rc[j] * h[j];
        data[i] = (int32_t)((uint32_t)data[i] + (uint32_t)(int32_t)(s >> qlevel));
    }
}
#else
#define lpc_decode_wide rbflac_lpc_wide_arm
#endif

static int decode_subframe_lpc(GB *g, int32_t *decoded, int bs,
                               int pred_order, int bps)
{
    int i, coeff_prec, qlevel;
    int32_t coeffs[32];
    uint32_t sumabs = 0;

    if (pred_order > bs || !gb_left(g, (unsigned)pred_order * bps + 9))
        return -1;
    for (i = 0; i < pred_order; i++)
        decoded[i] = get_sbits(g, bps);
    coeff_prec = get_bits(g, 4) + 1;
    if (coeff_prec == 16) return -1;
    qlevel = get_sbits(g, 5);
    if (qlevel < 0) return -1;
    if (!gb_left(g, (unsigned)pred_order * coeff_prec)) return -1;
    for (i = 0; i < pred_order; i++) {
        int32_t c = get_sbits(g, coeff_prec);
        coeffs[i] = c;
        sumabs += c < 0 ? -c : c;
    }
    if (decode_residuals(g, decoded, bs, pred_order) < 0)
        return -1;

    /* exact bound: |sum c*x| <= sumabs * 2^(bps-1) must fit int32 */
    if (sumabs < (1u << (32 - bps))) {
#ifdef RBFLAC_ARM_ASM
        rbflac_lpc_decode_arm(bs - pred_order, qlevel, pred_order,
                              decoded + pred_order, coeffs);
#else
        lpc_decode_c32(bs - pred_order, qlevel, pred_order,
                       decoded + pred_order, coeffs);
#endif
    } else {
        int32_t rc[32];
        for (i = 0; i < pred_order; i++) rc[i] = coeffs[pred_order - 1 - i];
        lpc_decode_wide(bs - pred_order, qlevel, pred_order,
                        decoded + pred_order, rc);
    }
    return 0;
}

static int decode_subframe(GB *g, int32_t *decoded, int bs, int bps)
{
    int type, wasted = 0, i;

    if (!gb_left(g, 8)) return -1;
    if (get_bits1(g)) return -1;
    type = get_bits(g, 6);
    if (get_bits1(g)) {
        wasted = 1;
        for (;;) {
            if (wasted >= bps || !gb_left(g, 1)) return -1;
            if (get_bits1(g)) break;
            wasted++;
        }
        bps -= wasted;
    }

    if (type == 0) {
        int32_t v;
        if (!gb_left(g, bps)) return -1;
        v = get_sbits(g, bps);
        for (i = 0; i < bs; i++) decoded[i] = v;
    } else if (type == 1) {
        if (!gb_left(g, (unsigned)bs * bps)) return -1;
        for (i = 0; i < bs; i++) decoded[i] = get_sbits(g, bps);
    } else if (type >= 8 && type <= 12) {
        if (decode_subframe_fixed(g, decoded, bs, type & 7, bps) < 0) return -1;
    } else if (type >= 32) {
        if (decode_subframe_lpc(g, decoded, bs, (type & 31) + 1, bps) < 0) return -1;
    } else {
        return -1;
    }

    if (wasted) {
        for (i = 0; i < bs; i++)
            decoded[i] = (int32_t)((uint32_t)decoded[i] << wasted);
    }
    return 0;
}

/* ---------------- CRC16 (poly 0x8005) ---------------- */
static const uint16_t crc16_tab[256] = {
    0x0000,0x8005,0x800f,0x000a,0x801b,0x001e,0x0014,0x8011,
    0x8033,0x0036,0x003c,0x8039,0x0028,0x802d,0x8027,0x0022,
    0x8063,0x0066,0x006c,0x8069,0x0078,0x807d,0x8077,0x0072,
    0x0050,0x8055,0x805f,0x005a,0x804b,0x004e,0x0044,0x8041,
    0x80c3,0x00c6,0x00cc,0x80c9,0x00d8,0x80dd,0x80d7,0x00d2,
    0x00f0,0x80f5,0x80ff,0x00fa,0x80eb,0x00ee,0x00e4,0x80e1,
    0x00a0,0x80a5,0x80af,0x00aa,0x80bb,0x00be,0x00b4,0x80b1,
    0x8093,0x0096,0x009c,0x8099,0x0088,0x808d,0x8087,0x0082,
    0x8183,0x0186,0x018c,0x8189,0x0198,0x819d,0x8197,0x0192,
    0x01b0,0x81b5,0x81bf,0x01ba,0x81ab,0x01ae,0x01a4,0x81a1,
    0x01e0,0x81e5,0x81ef,0x01ea,0x81fb,0x01fe,0x01f4,0x81f1,
    0x81d3,0x01d6,0x01dc,0x81d9,0x01c8,0x81cd,0x81c7,0x01c2,
    0x0140,0x8145,0x814f,0x014a,0x815b,0x015e,0x0154,0x8151,
    0x8173,0x0176,0x017c,0x8179,0x0168,0x816d,0x8167,0x0162,
    0x8123,0x0126,0x012c,0x8129,0x0138,0x813d,0x8137,0x0132,
    0x0110,0x8115,0x811f,0x011a,0x810b,0x010e,0x0104,0x8101,
    0x8303,0x0306,0x030c,0x8309,0x0318,0x831d,0x8317,0x0312,
    0x0330,0x8335,0x833f,0x033a,0x832b,0x032e,0x0324,0x8321,
    0x0360,0x8365,0x836f,0x036a,0x837b,0x037e,0x0374,0x8371,
    0x8353,0x0356,0x035c,0x8359,0x0348,0x834d,0x8347,0x0342,
    0x03c0,0x83c5,0x83cf,0x03ca,0x83db,0x03de,0x03d4,0x83d1,
    0x83f3,0x03f6,0x03fc,0x83f9,0x03e8,0x83ed,0x83e7,0x03e2,
    0x83a3,0x03a6,0x03ac,0x83a9,0x03b8,0x83bd,0x83b7,0x03b2,
    0x0390,0x8395,0x839f,0x039a,0x838b,0x038e,0x0384,0x8381,
    0x0280,0x8285,0x828f,0x028a,0x829b,0x029e,0x0294,0x8291,
    0x82b3,0x02b6,0x02bc,0x82b9,0x02a8,0x82ad,0x82a7,0x02a2,
    0x82e3,0x02e6,0x02ec,0x82e9,0x02f8,0x82fd,0x82f7,0x02f2,
    0x02d0,0x82d5,0x82df,0x02da,0x82cb,0x02ce,0x02c4,0x82c1,
    0x8243,0x0246,0x024c,0x8249,0x0258,0x825d,0x8257,0x0252,
    0x0270,0x8275,0x827f,0x027a,0x826b,0x026e,0x0264,0x8261,
    0x0220,0x8225,0x822f,0x022a,0x823b,0x023e,0x0234,0x8231,
    0x8213,0x0216,0x021c,0x8219,0x0208,0x820d,0x8207,0x0202
};
static unsigned crc16(const uint8_t *p, int n)
{
    unsigned c = 0;
    while (n--) c = ((c << 8) & 0xffff) ^ crc16_tab[(c >> 8) ^ *p++];
    return c;
}

/* ---------------- frame ---------------- */
int rbflac_decode_frame(const rbflac_stream *st, const uint8_t *buf, int len,
                        int32_t *const *out, rbflac_frame *fr)
{
    GB g;
    int r, ch, bs, bytes;

    if (st->ch < 1 || st->ch > RBFLAC_MAXCH || st->bps < 4 || st->bps > 24 ||
        st->maxbs < 16 || st->maxbs > 65535)
        return RBFLAC_EPARAM;
    r = rbflac_parse_header(st, buf, len, fr);
    if (r < 0) return r;
    bs = fr->blocksize;

    g.buf = buf;
    g.idx = (unsigned)r * 8;
    g.size = (unsigned)len * 8;

    for (ch = 0; ch < fr->nch; ch++) {
        int bps = fr->bps;
        if ((fr->chmode == 8 && ch == 1) || (fr->chmode == 9 && ch == 0) ||
            (fr->chmode == 10 && ch == 1))
            bps++;
        if (decode_subframe(&g, out[ch], bs, bps) < 0)
            return g.idx > g.size ? RBFLAC_EOVER : RBFLAC_EDATA;
        if (g.idx > g.size) return RBFLAC_EOVER;
    }
    bytes = (int)((g.idx + 7) >> 3) + 2;     /* align + CRC16 */
    if (bytes > len) return RBFLAC_EOVER;
    if (st->flags & RBFLAC_F_CRC16) {
        if (crc16(buf, bytes - 2) != (((unsigned)buf[bytes - 2] << 8) | buf[bytes - 1]))
            return RBFLAC_ECRC;
    }

    if (fr->chmode >= 8) {
        int32_t *L = out[0], *R = out[1];
        int i;
        switch (fr->chmode) {
        case 8:  /* left/side */
            for (i = 0; i < bs; i++) R[i] = (int32_t)((uint32_t)L[i] - (uint32_t)R[i]);
            break;
        case 9:  /* side/right */
            for (i = 0; i < bs; i++) L[i] = (int32_t)((uint32_t)L[i] + (uint32_t)R[i]);
            break;
        case 10: /* mid/side */
            for (i = 0; i < bs; i++) {
                int32_t b = R[i];
                uint32_t a = (uint32_t)L[i] - (uint32_t)(b >> 1);
                L[i] = (int32_t)(a + (uint32_t)b);
                R[i] = (int32_t)a;
            }
            break;
        }
    }
    return bytes;
}
