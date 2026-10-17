/* Hearing: a loud-audio limiter on the A-weighted level of the ITU-T H.870 dose meter, ear-fatigue
 * control (dynamic cuts of the 3 kHz harshness and 7.5 kHz sibilance bands when they stand out),
 * loudness compensation from the ISO 226 contours at the level being heard, and bs2b crossfeed.
 * Runs in the decode task on 24-bit samples; coefficients are designed in double precision. */
#include <stdint.h>
#include "hearing.h"
#include "loudness_data.h"

#define QB 28
#define PI 3.14159265358979323846
#define LN2 0.69314718055994530942
#define LN10 2.30258509299404568402
#define HP_DBV 0.0            /* codec output for a 0 dBFS sine at 0 dB headphone gain, dBV (estimate) */
#define WEEK_PA2H 1.6         /* H.870: 80 dBA for 40 hours a week */
#define LVL_TAU 10.0          /* seconds: the programme level loudness follows */
#define LVL_FLOOR 1e-6        /* blocks below -60 dB (gaps, fades) leave it alone */

int hear_loud, hear_fatigue, hear_xfeed, hear_limit, hear_phones;
volatile uint32_t hear_gen;
volatile uint32_t hear_dose_acc;
#ifdef HOSTTEST
int hear_test_hp;
#endif
static const double phones_sens[4] = { 120, 122, 112, 98 };   /* dB SPL at 1 Vrms: earbuds, in-ear, over-ear, high-impedance */
/* ear fatigue per level: threshold over the typical balance (dB), share of the excess cut, most cut (dB) */
static const float fat_tab[3][3] = { { 4, 0.5f, 4 }, { 2, 0.7f, 6 }, { 0, 1.0f, 9 } };
/* typical commercial balance re the 1 kHz band: about -5 dB per octave above 1 kHz (Pestana et al. 2013) */
static const float fat_exp[2] = { -8.3f, -14.5f };
static const float fat_f[2] = { 3150, 7500 }, fat_q[2] = { 1.0f, 1.4f };

/* ---- math (no libm in region E) ---- */
static double m_exp(double x)
{
    int k = (int)(x / LN2 + (x < 0 ? -0.5 : 0.5)), i;
    double r = x - k * LN2, t = 1, s = 1;
    for (i = 1; i < 16; i++) { t *= r / i; s += t; }
    for (; k > 0; k--) s *= 2;
    for (; k < 0; k++) s *= 0.5;
    return s;
}
static double m_log10(double x)
{
    int e = 0, i;
    double t, y, s = 0;
    if (x <= 0) return -300;
    while (x >= 2) { x *= 0.5; e++; }
    while (x < 1) { x *= 2; e--; }
    t = (x - 1) / (x + 1); y = t * t;
    for (i = 15; i >= 1; i -= 2) s = s * y + 1.0 / i;
    return (2 * t * s + e * LN2) / LN10;
}
static double m_sin(double x)
{
    double t, s, x2;
    int i;
    while (x > PI) x -= 2 * PI;
    while (x < -PI) x += 2 * PI;
    if (x > PI / 2) x = PI - x;
    if (x < -PI / 2) x = -PI - x;
    t = s = x; x2 = x * x;
    for (i = 3; i < 22; i += 2) { t *= -x2 / ((i - 1) * i); s += t; }
    return s;
}
static double m_cos(double x) { return m_sin(x + PI / 2); }
static double m_sqrt(double x)
{
    double r = x > 1 ? x : 1;
    int i;
    if (x <= 0) return 0;
    for (i = 0; i < 40; i++) r = 0.5 * (r + x / r);
    return r;
}
static double db_amp(double db) { return m_exp(db * (LN10 / 20)); }
static int32_t q(double v) { return (int32_t)(v * (1 << QB) + (v < 0 ? -0.5 : 0.5)); }

/* ---- biquads ---- */
static void bq_set(hear_bq *c, double b0, double b1, double b2, double a0, double a1, double a2)
{
    c->b0 = q(b0 / a0); c->b1 = q(b1 / a0); c->b2 = q(b2 / a0);
    c->a1 = q(-a1 / a0); c->a2 = q(-a2 / a0);
}
static void bq_peak_sc(hear_bq *c, double sn, double cs, double Q, double db)
{
    double A = db_amp(db / 2), al = sn / (2 * Q);
    bq_set(c, 1 + al * A, -2 * cs, 1 - al * A, 1 + al / A, -2 * cs, 1 - al / A);
}
static void bq_bp(hear_bq *c, double f, double Q, int sr)          /* RBJ band-pass, 0 dB at f */
{
    double w = 2 * PI * f / sr, al = m_sin(w) / (2 * Q);
    bq_set(c, al, 0, -al, 1 + al, -2 * m_cos(w), 1 - al);
}
static void bq_shelf(hear_bq *c, int high, double f, double S, double db, int sr)
{
    double A = db_amp(db / 2), w = 2 * PI * f / sr, cs = m_cos(w), sa;
    sa = m_sqrt(A) * m_sin(w) * m_sqrt((A + 1 / A) * (1 / S - 1) + 2);
    if (!high) bq_set(c, A * ((A + 1) - (A - 1) * cs + sa), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - sa),
                      (A + 1) + (A - 1) * cs + sa, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - sa);
    else bq_set(c, A * ((A + 1) + (A - 1) * cs + sa), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - sa),
                (A + 1) - (A - 1) * cs + sa, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - sa);
}
/* first-order sections by the bilinear transform, cutoffs prewarped: K = 2 fs */
static double warp(double f, int sr) { double w = PI * f / sr; if (w > 1.5) w = 1.5; return 2.0 * sr * m_sin(w) / m_cos(w); }
static void fo(double *o, double n1, double n0, double d0, int sr)  /* (n1 s + n0) / (s + d0) */
{
    double K = 2.0 * sr;
    o[0] = n1 * K + n0; o[1] = n0 - n1 * K; o[2] = K + d0; o[3] = d0 - K;
}
static void bq_fo2(hear_bq *c, const double *p, const double *r, double g)
{
    bq_set(c, g * p[0] * r[0], g * (p[0] * r[1] + p[1] * r[0]), g * p[1] * r[1], p[2] * r[2], p[2] * r[3] + p[3] * r[2], p[3] * r[3]);
}
static inline int32_t bq_step(const hear_bq *c, int32_t *h, int32_t in)
{
    int64_t acc = (int64_t)c->b0 * in + (int64_t)c->b1 * h[0] + (int64_t)c->b2 * h[1]
                + (int64_t)c->a1 * h[2] + (int64_t)c->a2 * h[3] + (1 << (QB - 1));
    int32_t y = (int32_t)(acc >> QB);
    h[1] = h[0]; h[0] = in; h[3] = h[2]; h[2] = y;
    return y;
}
static void bq_run(const hear_bq *c, int32_t *h, int32_t *x, int n, int st)
{
    int32_t x1 = h[0], x2 = h[1], y1 = h[2], y2 = h[3];
    for (; n > 0; n--, x += st) {
        int32_t in = *x, y;
        int64_t acc = (int64_t)c->b0 * in + (int64_t)c->b1 * x1 + (int64_t)c->b2 * x2
                    + (int64_t)c->a1 * y1 + (int64_t)c->a2 * y2 + (1 << (QB - 1));
        y = (int32_t)(acc >> QB);
        x2 = x1; x1 = in; y2 = y1; y1 = y;
        *x = y;
    }
    h[0] = x1; h[1] = x2; h[2] = y1; h[3] = y2;
}

/* ---- the codec's headphone gain: the volume (0..255) last sent by 0x080ca36c, through the dB
   formula 0x08088bc4 writes to CS42L55 HPxCTL, with the unit's calibration byte ---- */
static int hp_gain_db(void)
{
#ifdef HOSTTEST
    return hear_test_hp;
#else
    const volatile uint8_t *hp = (const volatile uint8_t *)0x089d0ef4;
    int v = hp[2], db = (int8_t)(-67 + (70 * v >> 8) + hp[1]);
    if (!v) return -100;                                       /* volume 0: silent */
    return db < -60 ? -60 : db > 12 ? 12 : db;
#endif
}
static double spl_offset(HEAR *s) { return phones_sens[s->phones & 3] + HP_DBV + s->hp_db; }   /* dB SPL of a 0 dBFS sine */

/* ---- dose and programme level, from a mean square relative to a full-scale sine ---- */
static double dose_rem;
static void dose_add(HEAR *s, double ms, uint32_t frames)
{
    double pa2 = 4e-10 * ms * m_exp(spl_offset(s) * (LN10 / 10)), a;
    uint32_t inc;
    dose_rem += pa2 * ((double)frames / s->sr / 3600) / WEEK_PA2H * 1e6;
    inc = (uint32_t)dose_rem;
    dose_rem -= inc;
    hear_dose_acc += inc;
    if (ms > LVL_FLOOR) {
        a = (double)frames / s->sr / LVL_TAU;
        s->lvl_db += (float)((10 * m_log10(ms) - s->lvl_db) * (a > 1 ? 1 : a));
    }
}

/* ---- loudness: the contour shelves for the level heard (spl offset + programme level) ---- */
static void loud_design(HEAR *s, int phon)
{
    int n = sizeof loud_tab / sizeof *loud_tab, i = 0;
    double t = 0, lf, lg, hf, hg;
    const float *a, *b;
    s->loud_phon = phon;
    if (phon >= loud_tab[n - 1][0]) { i = n - 2; t = 1; }
    else if (phon > loud_tab[0][0]) {
        while (phon >= loud_tab[i + 1][0]) i++;
        t = (phon - loud_tab[i][0]) / (loud_tab[i + 1][0] - loud_tab[i][0]);
    }
    a = loud_tab[i]; b = loud_tab[i + 1];
    lg = a[2] + t * (b[2] - a[2]); hg = a[4] + t * (b[4] - a[4]);
    lf = b[2] > 0 ? a[1] + t * (b[1] - a[1]) : a[1];          /* the flat row's corners mean nothing */
    hf = b[4] > 0 ? a[3] + t * (b[3] - a[3]) : a[3];
    if (hf > 0.4 * s->sr) hf = 0.4 * s->sr;
    s->loud_flat = lg < 0.05 && hg < 0.05;
    bq_shelf(&s->lc[0], 0, lf, 0.7, lg, s->sr);
    bq_shelf(&s->lc[1], 1, hf, 0.7, hg, s->sr);
    {   /* headroom for the boost, folded into the first shelf's numerator */
        double pre = db_amp(-(lg > hg ? lg : hg));
        s->lc[0].b0 = q(s->lc[0].b0 * pre / (1 << QB)); s->lc[0].b1 = q(s->lc[0].b1 * pre / (1 << QB)); s->lc[0].b2 = q(s->lc[0].b2 * pre / (1 << QB));
    }
}

/* ---- ear fatigue: band balance per block against the typical one, cut what stands out ---- */
static void fat_peak(HEAR *s, int k)
{
    bq_peak_sc(&s->fc[k], s->fsin[k], s->fcos[k], fat_q[k], -s->cut[k]);
    s->cut_set[k] = s->cut[k];
}
static void fat_design(HEAR *s)
{
    int k;
    bq_bp(&s->bp[0], 1000, 1.4, s->sr);
    for (k = 0; k < 2; k++) {
        double w = 2 * PI * fat_f[k] / s->sr;
        bq_bp(&s->bp[k + 1], fat_f[k], 1.4, s->sr);
        s->fsin[k] = (float)m_sin(w); s->fcos[k] = (float)m_cos(w);
        fat_peak(s, k);
    }
}
/* identity filter state after the block: the cut can start next block without a step */
static void bq_idle(int32_t *h, const int32_t *x, int n, int st)
{
    h[0] = h[2] = n > 0 ? x[(n - 1) * st] : 0;
    h[1] = h[3] = n > 1 ? x[(n - 2) * st] : h[0];
}
static void fat_run(HEAR *s, int32_t *x, int n)
{
    const float *ft = fat_tab[(s->fatigue - 1) & 3];
    double dt = (double)n / s->sr;
    uint64_t e[3] = { 0, 0, 0 };
    int och = s->och, i, k, c, a = n > 1024 ? n - 1024 : 0;
    /* the bands from the last 1024 frames; the first 64 settle (Q 1.4 rings for well under 1 ms) */
    for (k = 0; k < 3; k++) s->bph[k][0] = s->bph[k][1] = s->bph[k][2] = s->bph[k][3] = 0;
    for (i = a; i < n; i++) {
        int32_t m = och == 2 ? (x[2 * i] >> 1) + (x[2 * i + 1] >> 1) : x[i];
        for (k = 0; k < 3; k++) {
            int32_t y = bq_step(&s->bp[k], s->bph[k], m) >> 4;
            if (i >= a + 64) e[k] += (uint64_t)((int64_t)y * y);
        }
    }
    for (k = 0; k < 2; k++) {
        double target = 0, d;
        if (e[0] > (uint64_t)(n - a) * 16) {                   /* the 1 kHz band above about -100 dB */
            double r = 10 * m_log10((double)e[k + 1] / (double)e[0]) - fat_exp[k] - ft[0];
            if (r > 0) target = r * ft[1];
            if (target > ft[2]) target = ft[2];
        }
        if (target > s->cut[k]) { s->cut[k] += (float)(40 * dt); if (s->cut[k] > target) s->cut[k] = (float)target; }   /* 40 dB/s down */
        else { s->cut[k] -= (float)(4 * dt); if (s->cut[k] < target) s->cut[k] = (float)target; }                       /* 4 dB/s back */
        d = s->cut[k] - s->cut_set[k];
        if (d >= 0.25 || d <= -0.25 || (s->cut[k] == 0 && s->cut_set[k] != 0)) fat_peak(s, k);
    }
    for (k = 0; k < 2; k++)
        for (c = 0; c < och; c++) {
            if (s->cut_set[k] != 0) bq_run(&s->fc[k], s->fh[k][c], x + c, n, och);
            else bq_idle(s->fh[k][c], x + c, n, och);           /* usual for balanced music: nothing to run */
        }
}

/* ---- bs2b crossfeed (700 Hz, 4.5 dB): G_lo, G_hi, Fc_hi and gain worked out from bs2b's init ---- */
static void xfeed_design(HEAR *s)
{
    double x = m_exp(-2 * PI * 700.0 / s->sr), y = m_exp(-2 * PI * 995.1 / s->sr);
    const double g_lo = 0.459727, g_hi = 0.227974, gain = 1 / (1 - g_hi + g_lo);
    s->xlo_b1 = q(x); s->xlo_a0 = q(g_lo * (1 - x));
    s->xhi_b1 = q(y); s->xhi_a0 = q(1 - g_hi * (1 - y)); s->xhi_a1 = q(-y);
    s->xgain = q(gain);
}
static void xfeed_run(HEAR *s, int32_t *x, int n)
{
    int32_t lo0 = s->xlo[0], lo1 = s->xlo[1], hi0 = s->xhi[0], hi1 = s->xhi[1], in0 = s->xin[0], in1 = s->xin[1];
    int i;
    for (i = 0; i < n; i++, x += 2) {
        int32_t l = x[0], r = x[1];
        lo0 = (int32_t)(((int64_t)s->xlo_a0 * l + (int64_t)s->xlo_b1 * lo0) >> QB);
        lo1 = (int32_t)(((int64_t)s->xlo_a0 * r + (int64_t)s->xlo_b1 * lo1) >> QB);
        hi0 = (int32_t)(((int64_t)s->xhi_a0 * l + (int64_t)s->xhi_a1 * in0 + (int64_t)s->xhi_b1 * hi0) >> QB);
        hi1 = (int32_t)(((int64_t)s->xhi_a0 * r + (int64_t)s->xhi_a1 * in1 + (int64_t)s->xhi_b1 * hi1) >> QB);
        in0 = l; in1 = r;
        x[0] = (int32_t)(((int64_t)(hi0 + lo1) * s->xgain) >> QB);
        x[1] = (int32_t)(((int64_t)(hi1 + lo0) * s->xgain) >> QB);
    }
    s->xlo[0] = lo0; s->xlo[1] = lo1; s->xhi[0] = hi0; s->xhi[1] = hi1; s->xin[0] = in0; s->xin[1] = in1;
}

/* ---- A-weighting without its 20.6 Hz pole pair (it only matters below 50 Hz, where the weight is
   under -30 dB): s/(s+w2) * s/(s+w3) * w4^2/(s+w4)^2, 0 dB at 1 kHz ---- */
static void meter_design(HEAR *s, int sr)
{
    double w2 = warp(107.65265, sr), w3 = warp(737.86223, sr), w4 = warp(12194.217, sr);
    double c[4], d[4], e[4], f[4], mag = 1;
    int i;
    fo(c, 1, 0, w2, sr); fo(d, 1, 0, w3, sr);
    fo(e, 0, w4, w4, sr); fo(f, 0, w4, w4, sr);
    {   /* |H| at 1 kHz over the four first-order factors */
        double w = 2 * PI * 1000 / sr, cr = m_cos(w), ci = -m_sin(w);   /* z^-1 */
        const double *fs[4] = { c, d, e, f };
        for (i = 0; i < 4; i++) {
            double nr = fs[i][0] + fs[i][1] * cr, ni = fs[i][1] * ci, dr = fs[i][2] + fs[i][3] * cr, di = fs[i][3] * ci;
            mag *= m_sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
        }
    }
    bq_fo2(&s->ac[0], c, d, 1); bq_fo2(&s->ac[1], e, f, 1 / mag);
    s->msr = sr;
}

/* ---- loud limit: the A-weighted level of each block (about 0.1 s, a sound level meter's
   'fast'; measured on its last 1024 frames) against the ceiling; down at once, back at 3 dB/s ---- */
static void limit_run(HEAR *s, int32_t *x, int n)
{
    int och = s->och, i, c, k, ramp, a = n > 1024 + 128 ? n - 1024 - 128 : 0;
    uint64_t sum = 0;
    double ms, target, dt = (double)n / s->sr;
    int32_t q0 = s->gain_q, q1, step;
    for (c = 0; c < och; c++) {
        int32_t (*h)[4] = s->ah[c];
        for (k = 0; k < 2; k++) h[k][0] = h[k][1] = h[k][2] = h[k][3] = 0;
        h[0][0] = h[0][1] = x[a * och + c] << 4;              /* held first sample: no step into the high-pass */
        for (i = a; i < n; i++) {
            int32_t v = x[i * och + c] << 4;                   /* 4 fraction bits for the 107 Hz poles */
            for (k = 0; k < 2; k++) v = bq_step(&s->ac[k], h[k], v);
            v >>= 4;
            if (i >= a + 128) sum += (uint64_t)((int64_t)v * v);   /* 128 frames to settle (3 ms) */
        }
    }
    ms = (double)sum / ((double)(n - a - 128 > 0 ? n - a - 128 : 1) * och) / (8388607.0 * 8388607.0 / 2);
    target = HEAR_LIMIT_DB(s->limit) - (10 * m_log10(ms + 1e-20) + spl_offset(s));
    if (target > 0) target = 0;
    if (target < s->gain_db) { s->gain_db = (float)target; ramp = n < 256 ? n : 256; }
    else { s->gain_db += (float)(3 * dt); if (s->gain_db > target) s->gain_db = (float)target; ramp = n; }
    q1 = s->gain_q = q(db_amp(s->gain_db));
    step = (q1 - q0) / (ramp ? ramp : 1);
    if (q0 != q1 || q1 != 1 << QB)
        for (i = 0; i < n; i++) {
            int32_t g = i < ramp ? q0 + step * i : q1;
            for (c = 0; c < och; c++) x[i * och + c] = (int32_t)(((int64_t)x[i * och + c] * g) >> QB);
        }
    dose_add(s, ms * m_exp(s->gain_db * (LN10 / 10)), n);     /* the dose of what is played */
    s->dose_done = 1;
}

void hear_reset(HEAR *s)
{
    int32_t *p[] = { s->lh[0][0], s->bph[0], s->fh[0][0], s->ah[0][0] };
    int len[] = { 16, 12, 16, 16 }, k, j;
    for (k = 0; k < 4; k++) for (j = 0; j < len[k]; j++) p[k][j] = 0;
    s->xlo[0] = s->xlo[1] = s->xhi[0] = s->xhi[1] = s->xin[0] = s->xin[1] = 0;
}

int hear_prepare(HEAR *s, int sr, int och)
{
    if (!s->gain_q) { s->gain_q = 1 << QB; s->lvl_db = -14; }   /* fresh state: a typical programme level */
    s->hp_db = hp_gain_db();
    if (sr != s->sr || och != s->och || s->gen != hear_gen) {
        s->sr = sr; s->och = och; s->gen = hear_gen;
        s->fatigue = hear_fatigue; s->loud = hear_loud; s->xfeed = hear_xfeed; s->limit = hear_limit; s->phones = hear_phones;
        hear_reset(s);
        if (s->msr != sr) meter_design(s, sr);
        if (s->fatigue) fat_design(s);
        if (s->xfeed && och == 2) xfeed_design(s);
        if (!s->limit) { s->gain_db = 0; s->gain_q = 1 << QB; }
        s->loud_phon = -1000;
        s->active = s->loud || s->fatigue || (s->xfeed && och == 2) || s->limit;
    }
    if (s->loud) {
        int phon = (int)(spl_offset(s) + s->lvl_db + 1000.5) - 1000, d = phon - s->loud_phon;
        if (d >= 2 || d <= -2) loud_design(s, phon);           /* 2 phon of hysteresis */
    }
    return s->active;
}

void hear_run(HEAR *s, int32_t *x, int n)
{
    int k, c, och = s->och;
    if (s->loud) {
        for (k = 0; k < 2; k++)
            for (c = 0; c < och; c++) {
                if (!s->loud_flat) bq_run(&s->lc[k], s->lh[k][c], x + c, n, och);
                else bq_idle(s->lh[k][c], x + c, n, och);
            }
    }
    if (s->fatigue) fat_run(s, x, n);
    if (s->xfeed && och == 2) xfeed_run(s, x, n);
    if (s->limit) limit_run(s, x, n);
}

/* Dose without the limiter: one decoded frame in four is A-weighted (history primed as if the first
 * sample had held, its first half dropped while the filter settles) and stands for every frame
 * since the last measure. */
void hear_meter(HEAR *s, const int16_t *o, int n)
{
    int och = s->och, c, i, k, skip = n / 2;
    uint64_t sum = 0;
    if (!s->sr || och < 1) return;
    if (s->dose_done) { s->dose_done = 0; return; }           /* the limiter measured this one */
    s->mt += n;
    if (s->mblk++ & 3) return;
    if (n < 1024) { s->mblk--; return; }   /* too short to settle: the next one is measured */
    if (s->msr != s->sr) meter_design(s, s->sr);
    for (c = 0; c < och; c++) {
        /* 12 fraction bits: the high-pass poles amplify rounding at 16-bit scale */
        int32_t h[2][4], x0 = o[c] << 12;
        for (k = 0; k < 2; k++) h[k][0] = h[k][1] = h[k][2] = h[k][3] = 0;
        h[0][0] = h[0][1] = x0;            /* high-pass input held at the first sample: no step */
        for (i = 0; i < n; i++) {
            int32_t v = o[i * och + c] << 12;
            for (k = 0; k < 2; k++) v = bq_step(&s->ac[k], h[k], v);
            if (i >= skip) { v >>= 6; sum += (uint64_t)((int64_t)v * v); }
        }
    }
    dose_add(s, (double)sum / 4096 / ((double)(n - skip) * och) / (32767.0 * 32767.0 / 2), s->mt);
    s->mt = 0;
}
