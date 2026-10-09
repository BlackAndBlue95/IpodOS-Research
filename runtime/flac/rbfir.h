/* rbfir: symmetric-folded decimation FIR dot product for flacblob's
 * resampler.  Bit-exact replacement for
 *     for (k = 0; k < L; k++) acc += (int64_t)x[k] * fir[k];
 * with fir = fir2[64] / fir4[128] from fir.h.  Freestanding, const data only. */
#ifndef RBFIR_H
#define RBFIR_H
#include <stdint.h>

typedef struct {
    const int32_t *hc;   /* hc[k] multiplies x[k] + x[L-1-k], k < half */
    int half;            /* L/2, multiple of 4 */
    int corr_idx;        /* one extra tap: acc += corr_val * x[corr_idx] */
    int32_t corr_val;    /* (0 for the symmetrised tables) */
} rbfir;

/* dec = 2 or 4.  exact = 1: identical output to the original tables
 * (they are symmetric except for the centre pair, fixed up by one MAC).
 * exact = 0: centre pair replaced by its mean (truly linear phase).
 * Returns NULL for other dec. */
const rbfir *rbfir_get(int dec, int exact);

/* x: 2*half samples, oldest first (flacblob: w->hist[c] + hpos).
 * |x[k]| must be < 2^30 (flacblob keeps them at 24-bit scale). */
int64_t rbfir_dot(const rbfir *f, const int32_t *x);

/* reference: plain loop over the original int16 table (for tests) */
int64_t rbfir_dot_ref(const int16_t *fir, int L, const int32_t *x);
#endif
