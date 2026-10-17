/* Hearing DSP on the decoder output (hearing.c): loudness, ear fatigue, crossfeed, loud limit, dose */
#include <stdint.h>

typedef struct { int32_t b0, b1, b2, a1, a2; } hear_bq;   /* Q28; a1, a2 negated */

typedef struct {
    int sr, och, hp_db, fatigue, loud, xfeed, limit, phones, active;
    uint32_t gen;
    hear_bq lc[2]; int32_t lh[2][2][4]; int loud_phon;                        /* loudness: shelves, the level they were made for */
    hear_bq bp[3]; int32_t bph[3][4];                                          /* ear fatigue: 1 kHz, 3.15 kHz, 7.5 kHz analysis bands */
    hear_bq fc[2]; int32_t fh[2][2][4]; float cut[2], cut_set[2]; float fsin[2], fcos[2];
    int32_t xlo_a0, xlo_b1, xhi_a0, xhi_a1, xhi_b1, xgain;                    /* bs2b crossfeed, Q28 */
    int32_t xlo[2], xhi[2], xin[2];
    hear_bq ac[2]; int msr; int32_t ah[2][2][4]; float gain_db; int32_t gain_q;   /* A-weighting; the limiter's gain */
    int loud_flat;                                                             /* loudness at or above the reference: shelves off */
    uint32_t mblk, mt; float lvl_db; int dose_done;                           /* meter; programme level, dBFS(A) */
} HEAR;

/* settings (settings.c); hear_gen changes after each */
extern int hear_loud, hear_fatigue, hear_xfeed, hear_limit, hear_phones;
extern volatile uint32_t hear_gen;
extern volatile uint32_t hear_dose_acc;                       /* dose in millionths of the weekly allowance, only grows */
#define HEAR_LIMIT_DB(k) (70 + 5 * (k))                       /* hear_limit 1..6: 75..100 dB SPL */

void hear_reset(HEAR *s);                                     /* a seek or a new track: filter history to zero */
int hear_prepare(HEAR *s, int sr, int och);                   /* per decoded frame; 1 if hear_run has work */
void hear_run(HEAR *s, int32_t *x, int n);                    /* n frames, interleaved, 24-bit scale, in place */
void hear_meter(HEAR *s, const int16_t *o, int n);            /* the 16-bit output, for the dose */
