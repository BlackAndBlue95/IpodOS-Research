/***************************************************************************
 * rbjpeg_idct.h - IDCT routines extracted verbatim (C paths) from Rockbox
 * apps/recorder/jpeg_load.c.  Included only by rbjpeg.c.
 *
 * Copyright (C) 2009 Andrew Mahone fractional decode, split IDCT - 16-point
 *   IDCT based on IJG jpeg-7 pre-release
 * Copyright (C) 2004 Jörg Hohensohn aka [IDC]Dragon
 * Heavily borrowed from the IJG implementation (C) Thomas G. Lane
 * Small & fast downscaling IDCT (C) 2002 by Guido Vollbeding  JPEGclub.org
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 ****************************************************************************/
#define CONST_BITS 13
#define PASS1_BITS 2


/* Some C compilers fail to reduce "FIX(constant)" at compile time, thus
* causing a lot of useless floating-point operations at run time.
* To get around this we use the following pre-calculated constants.
* If you change CONST_BITS you may want to add appropriate values.
* (With a reasonable C compiler, you can just rely on the FIX() macro...)
*/
#define FIX_0_298631336  2446 /* FIX(0.298631336) */
#define FIX_0_390180644  3196 /* FIX(0.390180644) */
#define FIX_0_541196100  4433 /* FIX(0.541196100) */
#define FIX_0_765366865  6270 /* FIX(0.765366865) */
#define FIX_0_899976223  7373 /* FIX(0.899976223) */
#define FIX_1_175875602  9633 /* FIX(1.175875602) */
#define FIX_1_501321110 12299 /* FIX(1.501321110) */
#define FIX_1_847759065 15137 /* FIX(1.847759065) */
#define FIX_1_961570560 16069 /* FIX(1.961570560) */
#define FIX_2_053119869 16819 /* FIX(2.053119869) */
#define FIX_2_562915447 20995 /* FIX(2.562915447) */
#define FIX_3_072711026 25172 /* FIX(3.072711026) */



/* Multiply an long variable by an long constant to yield an long result.
* For 8-bit samples with the recommended scaling, all the variable
* and constant values involved are no more than 16 bits wide, so a
* 16x16->32 bit multiply can be used instead of a full 32x32 multiply.
* For 12-bit samples, a full 32-bit multiplication will be needed.
*/
#define MULTIPLY(var1, var2) ((var1) * (var2))


/*
 * Macros for handling fixed-point arithmetic; these are used by many
 * but not all of the DCT/IDCT modules.
 *
 * All values are expected to be of type INT32.
 * Fractional constants are scaled left by CONST_BITS bits.
 * CONST_BITS is defined within each module using these macros,
 * and may differ from one module to the next.
 */
#define ONE ((long)1)
#define CONST_SCALE (ONE << CONST_BITS)

/* Convert a positive real constant to an integer scaled by CONST_SCALE.
 * Caution: some C compilers fail to reduce "FIX(constant)" at compile time,
 * thus causing a lot of useless floating-point operations at run time.
 */
#define FIX(x) ((long) ((x) * CONST_SCALE + 0.5))
#define RIGHT_SHIFT(x,shft)     ((x) >> (shft))

/* Descale and correctly round an int value that's scaled by N bits.
* We assume RIGHT_SHIFT rounds towards minus infinity, so adding
* the fudge factor is correct for either sign of X.
*/
#define DESCALE(x,n) (((x) + (1l << ((n)-1))) >> (n))

#define DS_OUT ((CONST_BITS)+(PASS1_BITS)+3)

/*
 * Conversion of full 0-255 range YCrCb to RGB:
 *   |R|   |1.000000 -0.000001  1.402000| |Y'|
 *   |G| = |1.000000 -0.334136 -0.714136| |Pb|
 *   |B|   |1.000000  1.772000  0.000000| |Pr|
 * Scaled (yields s15-bit output):
 *   |R|   |128    0  179| |Y       |
 *   |G| = |128  -43  -91| |Cb - 128|
 *   |B|   |128  227    0| |Cr - 128|
 */
#define YFAC            128
#define RVFAC           179
#define GUFAC           (-43)
#define GVFAC           (-91)
#define BUFAC           227
#define COMPONENT_SHIFT  15

/* horizontal-pass 1-point IDCT */
static void jpeg_idct1h(int16_t *ws, unsigned char *out, int16_t *end, int rowstep)
{
    for (; ws < end; ws += 8)
    {
        *out = range_limit(128 + (int) DESCALE(*ws, 3 + PASS1_BITS));
        out += rowstep;
    }
}

/* vertical-pass 2-point IDCT */
static void jpeg_idct2v(int16_t *ws, int16_t *end)
{
    for (; ws < end; ws++)
    {
        int tmp1 = ws[0*8];
        int tmp2 = ws[1*8];
        ws[0*8] = tmp1 + tmp2;
        ws[1*8] = tmp1 - tmp2;
    }
}

/* horizontal-pass 2-point IDCT */
static void jpeg_idct2h(int16_t *ws, unsigned char *out, int16_t *end, int rowstep)
{
    for (; ws < end; ws += 8, out += rowstep)
    {
        int tmp1 = ws[0] + (ONE << (PASS1_BITS + 2))
                   + (128 << (PASS1_BITS + 3));
        int tmp2 = ws[1];
        out[JPEG_PIX_SZ*0] = range_limit((int) RIGHT_SHIFT(tmp1 + tmp2,
            PASS1_BITS + 3));
        out[JPEG_PIX_SZ*1] = range_limit((int) RIGHT_SHIFT(tmp1 - tmp2,
            PASS1_BITS + 3));
    }
}

/* vertical-pass 4-point IDCT */
static void jpeg_idct4v(int16_t *ws, int16_t *end)
{
    for (; ws < end; ws++)
    {
        int tmp0, tmp2, tmp10, tmp12;
        int z1, z2, z3;
        /* Even part */

        tmp0 = ws[8*0];
        tmp2 = ws[8*2];

        tmp10 = (tmp0 + tmp2) << PASS1_BITS;
        tmp12 = (tmp0 - tmp2) << PASS1_BITS;

        /* Odd part */
        /* Same rotation as in the even part of the 8x8 LL&M IDCT */

        z2 = ws[8*1];
        z3 = ws[8*3];

        z1 = MULTIPLY16(z2 + z3, FIX_0_541196100) +
            (ONE << (CONST_BITS - PASS1_BITS - 1));
        tmp0 = RIGHT_SHIFT(z1 + MULTIPLY16(z3, - FIX_1_847759065),
            CONST_BITS-PASS1_BITS);
        tmp2 = RIGHT_SHIFT(z1 + MULTIPLY16(z2, FIX_0_765366865),
            CONST_BITS-PASS1_BITS);

        /* Final output stage */
        ws[8*0] = (int) (tmp10 + tmp2);
        ws[8*3] = (int) (tmp10 - tmp2);
        ws[8*1] = (int) (tmp12 + tmp0);
        ws[8*2] = (int) (tmp12 - tmp0);
    }
}

/* horizontal-pass 4-point IDCT */
static void jpeg_idct4h(int16_t *ws, unsigned char *out, int16_t *end, int rowstep)
{
    for (; ws < end; out += rowstep, ws += 8)
    {
        int tmp0, tmp2, tmp10, tmp12;
        int z1, z2, z3;
        /* Even part */

        tmp0 = (int) ws[0] + (ONE << (PASS1_BITS + 2))
               + (128 << (PASS1_BITS + 3));
        tmp2 = (int) ws[2];

        tmp10 = (tmp0 + tmp2) << CONST_BITS;
        tmp12 = (tmp0 - tmp2) << CONST_BITS;

        /* Odd part */
        /* Same rotation as in the even part of the 8x8 LL&M IDCT */

        z2 = (int) ws[1];
        z3 = (int) ws[3];

        z1 = MULTIPLY16(z2 + z3, FIX_0_541196100);
        tmp0 = z1 - MULTIPLY16(z3, FIX_1_847759065);
        tmp2 = z1 + MULTIPLY16(z2, FIX_0_765366865);

        /* Final output stage */

        out[JPEG_PIX_SZ*0] = range_limit((int) RIGHT_SHIFT(tmp10 + tmp2,
            DS_OUT));
        out[JPEG_PIX_SZ*3] = range_limit((int) RIGHT_SHIFT(tmp10 - tmp2,
            DS_OUT));
        out[JPEG_PIX_SZ*1] = range_limit((int) RIGHT_SHIFT(tmp12 + tmp0,
            DS_OUT));
        out[JPEG_PIX_SZ*2] = range_limit((int) RIGHT_SHIFT(tmp12 - tmp0,
            DS_OUT));
    }
}

/* vertical-pass 8-point IDCT */
static void jpeg_idct8v(int16_t *ws, int16_t *end)
{
    long tmp0, tmp1, tmp2, tmp3;
    long tmp10, tmp11, tmp12, tmp13;
    long z1, z2, z3, z4, z5;
#ifdef JPEG_IDCT_TRANSPOSE
    int16_t *ws2 = ws + 64;
    for (; ws < end; ws += 8, ws2++)
    {
#else
    for (; ws < end; ws++)
    {
#endif
    /* Due to quantization, we will usually find that many of the input
    * coefficients are zero, especially the AC terms.  We can exploit this
    * by short-circuiting the IDCT calculation for any column in which all
    * the AC terms are zero.  In that case each output is equal to the
    * DC coefficient (with scale factor as needed).
    * With typical images and quantization tables, half or more of the
    * column DCT calculations can be simplified this way.
    */
        if ((ws[V_IN_ST*1] | ws[V_IN_ST*2] | ws[V_IN_ST*3]
           | ws[V_IN_ST*4] | ws[V_IN_ST*5] | ws[V_IN_ST*6] | ws[V_IN_ST*7]) == 0)
        {
            /* AC terms all zero */
            int dcval = ws[V_IN_ST*0] << PASS1_BITS;

            V_OUT(0) = V_OUT(1) = V_OUT(2) = V_OUT(3) = V_OUT(4) = V_OUT(5) =
                       V_OUT(6) = V_OUT(7) = dcval;
            continue;
        }

        /* Even part: reverse the even part of the forward DCT. */
        /* The rotator is sqrt(2)*c(-6). */

        z2 = ws[V_IN_ST*2];
        z3 = ws[V_IN_ST*6];

        z1 = MULTIPLY16(z2 + z3, FIX_0_541196100);
        tmp2 = z1 + MULTIPLY16(z3, - FIX_1_847759065);
        tmp3 = z1 + MULTIPLY16(z2, FIX_0_765366865);

        z2 = ws[V_IN_ST*0] << CONST_BITS;
        z2 += ONE << (CONST_BITS - PASS1_BITS - 1);
        z3 = ws[V_IN_ST*4] << CONST_BITS;

        tmp0 = (z2 + z3);
        tmp1 = (z2 - z3);

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        /* Odd part per figure 8; the matrix is unitary and hence its
           transpose is its inverse.  i0..i3 are y7,y5,y3,y1 respectively. */

        tmp0 = ws[V_IN_ST*7];
        tmp1 = ws[V_IN_ST*5];
        tmp2 = ws[V_IN_ST*3];
        tmp3 = ws[V_IN_ST*1];

        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        z4 = tmp1 + tmp3;
        z5 = MULTIPLY16(z3 + z4, FIX_1_175875602); /* sqrt(2) * c3 */

        tmp0 = MULTIPLY16(tmp0, FIX_0_298631336); /* sqrt(2) * (-c1+c3+c5-c7) */
        tmp1 = MULTIPLY16(tmp1, FIX_2_053119869); /* sqrt(2) * ( c1+c3-c5+c7) */
        tmp2 = MULTIPLY16(tmp2, FIX_3_072711026); /* sqrt(2) * ( c1+c3+c5-c7) */
        tmp3 = MULTIPLY16(tmp3, FIX_1_501321110); /* sqrt(2) * ( c1+c3-c5-c7) */
        z1 = MULTIPLY16(z1, - FIX_0_899976223); /* sqrt(2) * (c7-c3) */
        z2 = MULTIPLY16(z2, - FIX_2_562915447); /* sqrt(2) * (-c1-c3) */
        z3 = MULTIPLY16(z3, - FIX_1_961570560); /* sqrt(2) * (-c3-c5) */
        z4 = MULTIPLY16(z4, - FIX_0_390180644); /* sqrt(2) * (c5-c3) */

        z3 += z5;
        z4 += z5;

        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        /* Final output stage: inputs are tmp10..tmp13, tmp0..tmp3 */

        V_OUT(0) = (int) RIGHT_SHIFT(tmp10 + tmp3, CONST_BITS-PASS1_BITS);
        V_OUT(7) = (int) RIGHT_SHIFT(tmp10 - tmp3, CONST_BITS-PASS1_BITS);
        V_OUT(1) = (int) RIGHT_SHIFT(tmp11 + tmp2, CONST_BITS-PASS1_BITS);
        V_OUT(6) = (int) RIGHT_SHIFT(tmp11 - tmp2, CONST_BITS-PASS1_BITS);
        V_OUT(2) = (int) RIGHT_SHIFT(tmp12 + tmp1, CONST_BITS-PASS1_BITS);
        V_OUT(5) = (int) RIGHT_SHIFT(tmp12 - tmp1, CONST_BITS-PASS1_BITS);
        V_OUT(3) = (int) RIGHT_SHIFT(tmp13 + tmp0, CONST_BITS-PASS1_BITS);
        V_OUT(4) = (int) RIGHT_SHIFT(tmp13 - tmp0, CONST_BITS-PASS1_BITS);
    }
}

/* horizontal-pass 8-point IDCT */
static void jpeg_idct8h(int16_t *ws, unsigned char *out, int16_t *end, int rowstep)
{
    long tmp0, tmp1, tmp2, tmp3;
    long tmp10, tmp11, tmp12, tmp13;
    long z1, z2, z3, z4, z5;
    for (; ws < end; out += rowstep, ws += 8)
    {
        /* Rows of zeroes can be exploited in the same way as we did with
         * columns. However, the column calculation has created many nonzero AC
         * terms, so the simplification applies less often (typically 5% to 10%
         * of the time). On machines with very fast multiplication, it's
         * possible that the test takes more time than it's worth.  In that
         * case this section may be commented out.
        */

#ifndef NO_ZERO_ROW_TEST
        if ((ws[1] | ws[2] | ws[3]
           | ws[4] | ws[5] | ws[6] | ws[7]) == 0)
        {
            /* AC terms all zero */
            unsigned char dcval = range_limit(128 + (int) DESCALE((long) ws[0],
                PASS1_BITS+3));

            out[JPEG_PIX_SZ*0] = dcval;
            out[JPEG_PIX_SZ*1] = dcval;
            out[JPEG_PIX_SZ*2] = dcval;
            out[JPEG_PIX_SZ*3] = dcval;
            out[JPEG_PIX_SZ*4] = dcval;
            out[JPEG_PIX_SZ*5] = dcval;
            out[JPEG_PIX_SZ*6] = dcval;
            out[JPEG_PIX_SZ*7] = dcval;
            continue;
        }
#endif

        /* Even part: reverse the even part of the forward DCT. */
        /* The rotator is sqrt(2)*c(-6). */

        z2 = (long) ws[2];
        z3 = (long) ws[6];

        z1 = MULTIPLY16(z2 + z3, FIX_0_541196100);
        tmp2 = z1 + MULTIPLY16(z3, - FIX_1_847759065);
        tmp3 = z1 + MULTIPLY16(z2, FIX_0_765366865);

        z4 = (long) ws[0] + (ONE << (PASS1_BITS + 2))
             + (128 << (PASS1_BITS + 3));
        z4 <<= CONST_BITS;
        z5 = (long) ws[4] << CONST_BITS;
        tmp0 = z4 + z5;
        tmp1 = z4 - z5;

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        /* Odd part per figure 8; the matrix is unitary and hence its
        * transpose is its inverse. i0..i3 are y7,y5,y3,y1 respectively. */

        tmp0 = (long) ws[7];
        tmp1 = (long) ws[5];
        tmp2 = (long) ws[3];
        tmp3 = (long) ws[1];

        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        z4 = tmp1 + tmp3;
        z5 = MULTIPLY16(z3 + z4, FIX_1_175875602); /* sqrt(2) * c3 */

        tmp0 = MULTIPLY16(tmp0, FIX_0_298631336); /* sqrt(2) * (-c1+c3+c5-c7) */
        tmp1 = MULTIPLY16(tmp1, FIX_2_053119869); /* sqrt(2) * ( c1+c3-c5+c7) */
        tmp2 = MULTIPLY16(tmp2, FIX_3_072711026); /* sqrt(2) * ( c1+c3+c5-c7) */
        tmp3 = MULTIPLY16(tmp3, FIX_1_501321110); /* sqrt(2) * ( c1+c3-c5-c7) */
        z1 = MULTIPLY16(z1, - FIX_0_899976223); /* sqrt(2) * (c7-c3) */
        z2 = MULTIPLY16(z2, - FIX_2_562915447); /* sqrt(2) * (-c1-c3) */
        z3 = MULTIPLY16(z3, - FIX_1_961570560); /* sqrt(2) * (-c3-c5) */
        z4 = MULTIPLY16(z4, - FIX_0_390180644); /* sqrt(2) * (c5-c3) */

        z3 += z5;
        z4 += z5;

        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        /* Final output stage: inputs are tmp10..tmp13, tmp0..tmp3 */

        out[JPEG_PIX_SZ*0] = range_limit((int) RIGHT_SHIFT(tmp10 + tmp3,
            DS_OUT));
        out[JPEG_PIX_SZ*7] = range_limit((int) RIGHT_SHIFT(tmp10 - tmp3,
            DS_OUT));
        out[JPEG_PIX_SZ*1] = range_limit((int) RIGHT_SHIFT(tmp11 + tmp2,
            DS_OUT));
        out[JPEG_PIX_SZ*6] = range_limit((int) RIGHT_SHIFT(tmp11 - tmp2,
            DS_OUT));
        out[JPEG_PIX_SZ*2] = range_limit((int) RIGHT_SHIFT(tmp12 + tmp1,
            DS_OUT));
        out[JPEG_PIX_SZ*5] = range_limit((int) RIGHT_SHIFT(tmp12 - tmp1,
            DS_OUT));
        out[JPEG_PIX_SZ*3] = range_limit((int) RIGHT_SHIFT(tmp13 + tmp0,
            DS_OUT));
        out[JPEG_PIX_SZ*4] = range_limit((int) RIGHT_SHIFT(tmp13 - tmp0,
            DS_OUT));
    }
}


/* vertical-pass 16-point IDCT */
static void jpeg_idct16v(int16_t *ws, int16_t *end)
{
    long tmp0, tmp1, tmp2, tmp3, tmp10, tmp11, tmp12, tmp13;
    long tmp20, tmp21, tmp22, tmp23, tmp24, tmp25, tmp26, tmp27;
    long z1, z2, z3, z4;
#ifdef JPEG_IDCT_TRANSPOSE
    int16_t *ws2 = ws + 64;
    for (; ws < end; ws += 8, ws2++)
    {
#else
    for (; ws < end; ws++)
    {
#endif
        /* Even part */

        tmp0 = ws[V_IN_ST*0] << CONST_BITS;
        /* Add fudge factor here for final descale. */
        tmp0 += 1 << (CONST_BITS-PASS1_BITS-1);

        z1 = ws[V_IN_ST*4];
        tmp1 = MULTIPLY(z1, FIX(1.306562965));      /* c4[16] = c2[8] */
        tmp2 = MULTIPLY(z1, FIX_0_541196100);       /* c12[16] = c6[8] */

        tmp10 = tmp0 + tmp1;
        tmp11 = tmp0 - tmp1;
        tmp12 = tmp0 + tmp2;
        tmp13 = tmp0 - tmp2;

        z1 = ws[V_IN_ST*2];
        z2 = ws[V_IN_ST*6];
        z3 = z1 - z2;
        z4 = MULTIPLY(z3, FIX(0.275899379));        /* c14[16] = c7[8] */
        z3 = MULTIPLY(z3, FIX(1.387039845));        /* c2[16] = c1[8] */

        /* (c6+c2)[16] = (c3+c1)[8] */
        tmp0 = z3 + MULTIPLY(z2, FIX_2_562915447);
        /* (c6-c14)[16] = (c3-c7)[8] */
        tmp1 = z4 + MULTIPLY(z1, FIX_0_899976223);
        /* (c2-c10)[16] = (c1-c5)[8] */
        tmp2 = z3 - MULTIPLY(z1, FIX(0.601344887));
        /* (c10-c14)[16] = (c5-c7)[8] */
        tmp3 = z4 - MULTIPLY(z2, FIX(0.509795579));

        tmp20 = tmp10 + tmp0;
        tmp27 = tmp10 - tmp0;
        tmp21 = tmp12 + tmp1;
        tmp26 = tmp12 - tmp1;
        tmp22 = tmp13 + tmp2;
        tmp25 = tmp13 - tmp2;
        tmp23 = tmp11 + tmp3;
        tmp24 = tmp11 - tmp3;

        /* Odd part */

        z1 = ws[V_IN_ST*1];
        z2 = ws[V_IN_ST*3];
        z3 = ws[V_IN_ST*5];
        z4 = ws[V_IN_ST*7];

        tmp11 = z1 + z3;

        tmp1  = MULTIPLY(z1 + z2, FIX(1.353318001));   /* c3 */
        tmp2  = MULTIPLY(tmp11,   FIX(1.247225013));   /* c5 */
        tmp3  = MULTIPLY(z1 + z4, FIX(1.093201867));   /* c7 */
        tmp10 = MULTIPLY(z1 - z4, FIX(0.897167586));   /* c9 */
        tmp11 = MULTIPLY(tmp11,   FIX(0.666655658));   /* c11 */
        tmp12 = MULTIPLY(z1 - z2, FIX(0.410524528));   /* c13 */
        tmp0  = tmp1 + tmp2 + tmp3 -
            MULTIPLY(z1, FIX(2.286341144));        /* c7+c5+c3-c1 */
        tmp13 = tmp10 + tmp11 + tmp12 -
            MULTIPLY(z1, FIX(1.835730603));        /* c9+c11+c13-c15 */
        z1    = MULTIPLY(z2 + z3, FIX(0.138617169));   /* c15 */
        tmp1  += z1 + MULTIPLY(z2, FIX(0.071888074));  /* c9+c11-c3-c15 */
        tmp2  += z1 - MULTIPLY(z3, FIX(1.125726048));  /* c5+c7+c15-c3 */
        z1    = MULTIPLY(z3 - z2, FIX(1.407403738));   /* c1 */
        tmp11 += z1 - MULTIPLY(z3, FIX(0.766367282));  /* c1+c11-c9-c13 */
        tmp12 += z1 + MULTIPLY(z2, FIX(1.971951411));  /* c1+c5+c13-c7 */
        z2    += z4;
        z1    = MULTIPLY(z2, - FIX(0.666655658));      /* -c11 */
        tmp1  += z1;
        tmp3  += z1 + MULTIPLY(z4, FIX(1.065388962));  /* c3+c11+c15-c7 */
        z2    = MULTIPLY(z2, - FIX(1.247225013));      /* -c5 */
        tmp10 += z2 + MULTIPLY(z4, FIX(3.141271809));  /* c1+c5+c9-c13 */
        tmp12 += z2;
        z2    = MULTIPLY(z3 + z4, - FIX(1.353318001)); /* -c3 */
        tmp2  += z2;
        tmp3  += z2;
        z2    = MULTIPLY(z4 - z3, FIX(0.410524528));   /* c13 */
        tmp10 += z2;
        tmp11 += z2;

        /* Final output stage */
        V_OUT(0)  = (int) RIGHT_SHIFT(tmp20 + tmp0,  CONST_BITS-PASS1_BITS);
        V_OUT(15) = (int) RIGHT_SHIFT(tmp20 - tmp0,  CONST_BITS-PASS1_BITS);
        V_OUT(1)  = (int) RIGHT_SHIFT(tmp21 + tmp1,  CONST_BITS-PASS1_BITS);
        V_OUT(14) = (int) RIGHT_SHIFT(tmp21 - tmp1,  CONST_BITS-PASS1_BITS);
        V_OUT(2)  = (int) RIGHT_SHIFT(tmp22 + tmp2,  CONST_BITS-PASS1_BITS);
        V_OUT(13) = (int) RIGHT_SHIFT(tmp22 - tmp2,  CONST_BITS-PASS1_BITS);
        V_OUT(3)  = (int) RIGHT_SHIFT(tmp23 + tmp3,  CONST_BITS-PASS1_BITS);
        V_OUT(12) = (int) RIGHT_SHIFT(tmp23 - tmp3,  CONST_BITS-PASS1_BITS);
        V_OUT(4)  = (int) RIGHT_SHIFT(tmp24 + tmp10, CONST_BITS-PASS1_BITS);
        V_OUT(11) = (int) RIGHT_SHIFT(tmp24 - tmp10, CONST_BITS-PASS1_BITS);
        V_OUT(5)  = (int) RIGHT_SHIFT(tmp25 + tmp11, CONST_BITS-PASS1_BITS);
        V_OUT(10) = (int) RIGHT_SHIFT(tmp25 - tmp11, CONST_BITS-PASS1_BITS);
        V_OUT(6)  = (int) RIGHT_SHIFT(tmp26 + tmp12, CONST_BITS-PASS1_BITS);
        V_OUT(9)  = (int) RIGHT_SHIFT(tmp26 - tmp12, CONST_BITS-PASS1_BITS);
        V_OUT(7)  = (int) RIGHT_SHIFT(tmp27 + tmp13, CONST_BITS-PASS1_BITS);
        V_OUT(8)  = (int) RIGHT_SHIFT(tmp27 - tmp13, CONST_BITS-PASS1_BITS);
    }
}

/* horizontal-pass 16-point IDCT */
static void jpeg_idct16h(int16_t *ws, unsigned char *out, int16_t *end, int rowstep)
{
    long tmp0, tmp1, tmp2, tmp3, tmp10, tmp11, tmp12, tmp13;
    long tmp20, tmp21, tmp22, tmp23, tmp24, tmp25, tmp26, tmp27;
    long z1, z2, z3, z4;
    for (; ws < end; out += rowstep, ws += 8)
    {
        /* Even part */

        /* Add fudge factor here for final descale. */
        tmp0 = (long) ws[0] + (ONE << (PASS1_BITS+2))
               + (128 << (PASS1_BITS + 3));
        tmp0 <<= CONST_BITS;

        z1 = (long) ws[4];
        tmp1 = MULTIPLY(z1, FIX(1.306562965));      /* c4[16] = c2[8] */
        tmp2 = MULTIPLY(z1, FIX_0_541196100);       /* c12[16] = c6[8] */

        tmp10 = tmp0 + tmp1;
        tmp11 = tmp0 - tmp1;
        tmp12 = tmp0 + tmp2;
        tmp13 = tmp0 - tmp2;

        z1 = (long) ws[2];
        z2 = (long) ws[6];
        z3 = z1 - z2;
        z4 = MULTIPLY(z3, FIX(0.275899379));        /* c14[16] = c7[8] */
        z3 = MULTIPLY(z3, FIX(1.387039845));        /* c2[16] = c1[8] */

        /* (c6+c2)[16] = (c3+c1)[8] */
        tmp0 = z3 + MULTIPLY(z2, FIX_2_562915447);
        /* (c6-c14)[16] = (c3-c7)[8] */
        tmp1 = z4 + MULTIPLY(z1, FIX_0_899976223);
        /* (c2-c10)[16] = (c1-c5)[8] */
        tmp2 = z3 - MULTIPLY(z1, FIX(0.601344887));
        /* (c10-c14)[16] = (c5-c7)[8] */
        tmp3 = z4 - MULTIPLY(z2, FIX(0.509795579));

        tmp20 = tmp10 + tmp0;
        tmp27 = tmp10 - tmp0;
        tmp21 = tmp12 + tmp1;
        tmp26 = tmp12 - tmp1;
        tmp22 = tmp13 + tmp2;
        tmp25 = tmp13 - tmp2;
        tmp23 = tmp11 + tmp3;
        tmp24 = tmp11 - tmp3;

        /* Odd part */

        z1 = (long) ws[1];
        z2 = (long) ws[3];
        z3 = (long) ws[5];
        z4 = (long) ws[7];

        tmp11 = z1 + z3;

        tmp1  = MULTIPLY(z1 + z2, FIX(1.353318001));   /* c3 */
        tmp2  = MULTIPLY(tmp11,   FIX(1.247225013));   /* c5 */
        tmp3  = MULTIPLY(z1 + z4, FIX(1.093201867));   /* c7 */
        tmp10 = MULTIPLY(z1 - z4, FIX(0.897167586));   /* c9 */
        tmp11 = MULTIPLY(tmp11,   FIX(0.666655658));   /* c11 */
        tmp12 = MULTIPLY(z1 - z2, FIX(0.410524528));   /* c13 */
        tmp0  = tmp1 + tmp2 + tmp3 -
            MULTIPLY(z1, FIX(2.286341144));        /* c7+c5+c3-c1 */
        tmp13 = tmp10 + tmp11 + tmp12 -
            MULTIPLY(z1, FIX(1.835730603));        /* c9+c11+c13-c15 */
        z1    = MULTIPLY(z2 + z3, FIX(0.138617169));   /* c15 */
        tmp1  += z1 + MULTIPLY(z2, FIX(0.071888074));  /* c9+c11-c3-c15 */
        tmp2  += z1 - MULTIPLY(z3, FIX(1.125726048));  /* c5+c7+c15-c3 */
        z1    = MULTIPLY(z3 - z2, FIX(1.407403738));   /* c1 */
        tmp11 += z1 - MULTIPLY(z3, FIX(0.766367282));  /* c1+c11-c9-c13 */
        tmp12 += z1 + MULTIPLY(z2, FIX(1.971951411));  /* c1+c5+c13-c7 */
        z2    += z4;
        z1    = MULTIPLY(z2, - FIX(0.666655658));      /* -c11 */
        tmp1  += z1;
        tmp3  += z1 + MULTIPLY(z4, FIX(1.065388962));  /* c3+c11+c15-c7 */
        z2    = MULTIPLY(z2, - FIX(1.247225013));      /* -c5 */
        tmp10 += z2 + MULTIPLY(z4, FIX(3.141271809));  /* c1+c5+c9-c13 */
        tmp12 += z2;
        z2    = MULTIPLY(z3 + z4, - FIX(1.353318001)); /* -c3 */
        tmp2  += z2;
        tmp3  += z2;
        z2    = MULTIPLY(z4 - z3, FIX(0.410524528));   /* c13 */
        tmp10 += z2;
        tmp11 += z2;

        /* Final output stage */

        out[JPEG_PIX_SZ*0]  = scale_output(tmp20 + tmp0);
        out[JPEG_PIX_SZ*15] = scale_output(tmp20 - tmp0);
        out[JPEG_PIX_SZ*1]  = scale_output(tmp21 + tmp1);
        out[JPEG_PIX_SZ*14] = scale_output(tmp21 - tmp1);
        out[JPEG_PIX_SZ*2]  = scale_output(tmp22 + tmp2);
        out[JPEG_PIX_SZ*13] = scale_output(tmp22 - tmp2);
        out[JPEG_PIX_SZ*3]  = scale_output(tmp23 + tmp3);
        out[JPEG_PIX_SZ*12] = scale_output(tmp23 - tmp3);
        out[JPEG_PIX_SZ*4]  = scale_output(tmp24 + tmp10);
        out[JPEG_PIX_SZ*11] = scale_output(tmp24 - tmp10);
        out[JPEG_PIX_SZ*5]  = scale_output(tmp25 + tmp11);
        out[JPEG_PIX_SZ*10] = scale_output(tmp25 - tmp11);
        out[JPEG_PIX_SZ*6]  = scale_output(tmp26 + tmp12);
        out[JPEG_PIX_SZ*9]  = scale_output(tmp26 - tmp12);
        out[JPEG_PIX_SZ*7]  = scale_output(tmp27 + tmp13);
        out[JPEG_PIX_SZ*8]  = scale_output(tmp27 - tmp13);
    }
}
