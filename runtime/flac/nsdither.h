/* 24 to 16 bits: TPDF dither under a 9-tap error-feedback noise shaper (coefficients from
 * host/gen_nsdither.py). Input in Q8: 256 per output LSB. */
#include <stdint.h>

#define NS_TAPS 9
#define NS_EMAX 384           /* |error| bound without clipping (1 LSB dither + 1/2 LSB rounding) */

typedef struct {
    int16_t e[2 * NS_TAPS];       /* errors twice over: e[pos..pos+8] newest first */
    int pos;
} NSCH;
typedef struct {
    NSCH ch[2];
    const int16_t *h;             /* Q12 feedback taps; the flat table gives plain TPDF */
    uint32_t rng;
} NSQ;

/* minimax noise-to-threshold per critical band, unweighted noise +12 dB over TPDF */
static const int16_t ns_h44[NS_TAPS] = { 6838, -7824, 7702, -7405, 4481, -2432, 398, 559, -729 };
static const int16_t ns_h48[NS_TAPS] = { 7646, -8825, 7797, -5305, 711, 2035, -3292, 2631, -1200 };
static const int16_t ns_flat[NS_TAPS];

static inline void nsq_reset(NSQ *q)
{
    int c, k;
    for (c = 0; c < 2; c++) {
        for (k = 0; k < 2 * NS_TAPS; k++) q->ch[c].e[k] = 0;
        q->ch[c].pos = 0;
    }
    if (!q->rng) q->rng = 0x2545f491;
}

/* the curve for an output rate: shaped at 44.1 and 48 kHz only, other rates get flat TPDF */
static inline void nsq_rate(NSQ *q, int sr, int shaped)
{
    const int16_t *h = !shaped ? ns_flat : sr == 44100 ? ns_h44 : sr == 48000 ? ns_h48 : ns_flat;
    if (h != q->h) { q->h = h; nsq_reset(q); }
}

static inline int16_t nsq_run(NSQ *q, int c, int32_t v)
{
    NSCH *s = &q->ch[c];
    const int16_t *h = q->h, *e = s->e + s->pos;
    uint32_t r = q->rng;
    int32_t acc = 0, y, err;
    int k;
    r ^= r << 13; r ^= r >> 17; r ^= r << 5;   /* xorshift32 */
    q->rng = r;
#pragma GCC unroll 9
    for (k = 0; k < NS_TAPS; k++) acc += h[k] * e[k];
    v -= (acc + 2048) >> 12;
    /* two 8-bit uniforms from the top half: TPDF of +-255/256 LSB, then round */
    y = (v + (int32_t)(r >> 24) + (int32_t)((r >> 16) & 255) - 127) >> 8;
    err = (y << 8) - v;
    if ((uint32_t)(y + 32768) > 65535) {       /* a clip: the error is held to its unclipped bound */
        y = (y >> 31) ^ 32767;
        err = (y << 8) - v;
        if (err > NS_EMAX) err = NS_EMAX;
        else if (err < -NS_EMAX) err = -NS_EMAX;
    }
    k = s->pos - 1;
    if (k < 0) k = NS_TAPS - 1;
    s->pos = k;
    s->e[k] = s->e[k + NS_TAPS] = (int16_t)err;
    return (int16_t)y;
}
