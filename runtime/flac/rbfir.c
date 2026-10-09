/* rbfir.c: see rbfir.h */
#include <stdint.h>
#include <stddef.h>
#include "rbfir.h"
#include "rbfir_tab.h"

static const rbfir fir_tab[4] = {
    { fir2_half_exact, 32, FIR2_CORR_IDX, FIR2_CORR_VAL },
    { fir4_half_exact, 64, FIR4_CORR_IDX, FIR4_CORR_VAL },
    { fir2_half_sym,   32, 0, 0 },
    { fir4_half_sym,   64, 0, 0 },
};

const rbfir *rbfir_get(int dec, int exact)
{
    if (dec != 2 && dec != 4) return NULL;
    return &fir_tab[(dec == 4) + (exact ? 0 : 2)];
}

#if defined(__arm__) && !defined(__thumb__) && !defined(RBFIR_NO_ASM)
int64_t rbfir_fold_arm(const int32_t *x, const int32_t *hc, int half);

int64_t rbfir_dot(const rbfir *f, const int32_t *x)
{
    return rbfir_fold_arm(x, f->hc, f->half) + (int64_t)f->corr_val * x[f->corr_idx];
}
#else
int64_t rbfir_dot(const rbfir *f, const int32_t *x)
{
    const int32_t *a = x, *b = x + 2 * f->half - 1, *c = f->hc;
    int64_t acc = (int64_t)f->corr_val * x[f->corr_idx];
    int k;
    for (k = f->half; k > 0; k -= 4) {
        acc += (int64_t)c[0] * (a[0] + b[0]);
        acc += (int64_t)c[1] * (a[1] + b[-1]);
        acc += (int64_t)c[2] * (a[2] + b[-2]);
        acc += (int64_t)c[3] * (a[3] + b[-3]);
        a += 4; b -= 4; c += 4;
    }
    return acc;
}
#endif

int64_t rbfir_dot_ref(const int16_t *fir, int L, const int32_t *x)
{
    int64_t acc = 0; int k;
    for (k = 0; k < L; k++) acc += (int64_t)x[k] * fir[k];
    return acc;
}
