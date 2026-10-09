/***************************************************************************
 * rbjpeg.c - freestanding port of the Rockbox JPEG loader + scaler.
 *
 * Derived from Rockbox apps/recorder/jpeg_load.c, jpeg_common.h, resize.c:
 *
 * Copyright (C) 2009 Andrew Mahone fractional decode, split IDCT - 16-point
 *   IDCT based on IJG jpeg-7 pre-release
 * File scrolling addition (C) 2005 Alexander Spyridakis
 * Copyright (C) 2004 Jörg Hohensohn aka [IDC]Dragon
 * Heavily borrowed from the IJG implementation (C) Thomas G. Lane
 * Small & fast downscaling IDCT (C) 2002 by Guido Vollbeding  JPEGclub.org
 * Copyright (C) 2008 by Akio Idehara, Andrew Mahone (resize.c)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * Port notes:
 *  - JPEG_FROM_MEM path only, HAVE_LCD_COLOR behaviour, JPEG_IDCT_TRANSPOSE,
 *    HAVE_UPSCALER.  Generic C IDCTs (Rockbox's ARM .S file is not used).
 *  - All state lives in the caller's work buffer (no statics).
 *  - Scaler: alpha channel dropped, callbacks replaced by direct calls,
 *    output row writes RGB565 LE with Rockbox's rounding (no dither).
 *  - Extra input validation (see comments marked "port:").
 ****************************************************************************/
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "rbjpeg.h"

/* provided by the blob / libc */
void *memset(void *s, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

#define MEMSET(p,v,c) memset(p,v,c)
#define MEMCPY(d,s,c) memcpy(d,s,c)
#define INLINE static inline
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define BIT_N(n) (1U << (n))
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/**************** begin JPEG code ********************/

/* 3-byte pixel as in upstream Rockbox. For YUV data Y is stored in blue,
 * U (Cb) in green and V (Cr) in red. */
struct uint8_rgb {
    uint8_t blue;
    uint8_t green;
    uint8_t red;
};
typedef struct uint8_rgb jpeg_pix_t;

#define JPEG_IDCT_TRANSPOSE
#define JPEG_PIX_SZ (sizeof(jpeg_pix_t))
#define COLOR_EXTRA_IDCT_WS 64
#define V_OUT(n) ws2[8*n]
#define V_IN_ST 1
#define TRANSPOSE_EXTRA_IDCT_WS 64
#define IDCT_WS_SIZE (64 + TRANSPOSE_EXTRA_IDCT_WS + COLOR_EXTRA_IDCT_WS)

#define HUFF_LOOKAHEAD 8 /* # of bits of lookahead */
struct derived_tbl
{
    /* Basic tables: (element [0] of each array is unused) */
    long mincode[17]; /* smallest code of length k */
    long maxcode[18]; /* largest code of length k (-1 if none) */
    /* (maxcode[17] is a sentinel to ensure huff_DECODE terminates) */
    int valptr[17]; /* huffval[] index of 1st symbol of length k */

    /* Back link to public Huffman table (needed only in slow_DECODE) */
    int* pub;

    /* Lookahead tables: indexed by the next HUFF_LOOKAHEAD bits of
    the input data stream.  If the next Huffman code is no more
    than HUFF_LOOKAHEAD bits long, we can obtain its length and
    the corresponding symbol directly from these tables.
    port: look_nbits is a byte array (was int) to save 3KB of cache. */
    unsigned char look_nbits[1<<HUFF_LOOKAHEAD]; /* # bits, or 0 if too long */
    unsigned char look_sym[1<<HUFF_LOOKAHEAD]; /* symbol, or unused */
};

#define QUANT_TABLE_LENGTH  64

/* for type of Huffman table */
#define DC_LEN 28
#define AC_LEN 178

struct huffman_table
{   /* length and code according to JFIF format */
    int huffmancodes_dc[DC_LEN];
    int huffmancodes_ac[AC_LEN];
};

struct frame_component
{
    int ID;
    int horizontal_sampling;
    int vertical_sampling;
    int quanttable_select;
};

struct scan_component
{
    int ID;
    int DC_select;
    int AC_select;
};

/* possible return flags for process_markers() */
#define HUFFTAB   0x0001 /* with huffman table */
#define QUANTTAB  0x0002 /* with quantization table */
#define APP0_JFIF 0x0004 /* with APP0 segment following JFIF standard */
#define SOF0      0x0010 /* with SOF0-Segment */
#define DHT       0x0020 /* with Definition of huffman tables */
#define SOS       0x0040 /* with Start-of-Scan segment */
#define DQT       0x0080 /* with definition of quantization table */

struct img_part {
    int len;
    struct uint8_rgb* buf;
};

struct jpeg
{
    const unsigned char *data;
    unsigned long len;
    unsigned long int bitbuf;
    int bitbuf_bits;
    int marker_ind;
    int marker_val;
    unsigned char marker;
    int x_size, y_size; /* size of image (can be less than block boundary) */
    int x_phys, y_phys; /* physical size, block aligned */
    int x_mbl; /* x dimension of MBL */
    int y_mbl; /* y dimension of MBL */
    int blocks; /* blocks per MB */
    int components; /* port: number of frame components */
    int scan_components; /* port: number of scan components */
    int restart_interval; /* number of MCUs between RSTm markers */
    int restart; /* blocks until next restart marker */
    int mcu_row; /* current row relative to first row of this row of MCUs */
    unsigned char *out_ptr; /* pointer to current row to output */
    int cur_row; /* current row relative to top of image */
    int set_rows;
    int store_pos[4]; /* for Y block ordering */
    int last_dc_val[3];
    int h_scale[2]; /* horizontal scalefactor = (2**N) / 8 */
    int v_scale[2]; /* same as above, for vertical direction */
    int k_need[2]; /* per component zig-zag index of last needed coefficient */
    int zero_need[2]; /* per compenent number of coefficients to zero */
    jpeg_pix_t *img_buf;

    int16_t quanttable[4][QUANT_TABLE_LENGTH];/* raw quantization tables 0-3 */

    struct huffman_table hufftable[2]; /* Huffman tables  */
    struct derived_tbl dc_derived_tbls[2]; /* Huffman-LUTs */
    struct derived_tbl ac_derived_tbls[2];

    struct frame_component frameheader[3]; /* Component descriptor */
    struct scan_component scanheader[3]; /* currently not used */

    int mcu_membership[6]; /* info per block */
    int tab_membership[6];
    int subsample_x[3]; /* info per component */
    int subsample_y[3];
    bool resize;
    struct img_part part;

    /* port: scratch for fix_huff_tbl, moved off the stack */
    char huffsize[257];
    unsigned int huffcode[257];
};

INLINE unsigned range_limit(int value)
{
#if defined(__arm__) && !defined(__thumb__)
    /* Note: Uses knowledge that only the low byte of the result is used */
    asm (
        "cmp     %[v], #255          \n"  /* out of range 0..255? */
        "mvnhi   %[v], %[v], asr #31 \n"  /* yes: set all bits to ~(sign_bit) */
        : /* outputs */
        [v]"+r"(value)
        : : "cc"
    );
    return value;
#else
    if ((unsigned)value <= 255)
        return value;

    if (value < 0)
        return 0;

    return 255;
#endif
}

INLINE unsigned scale_output(int value)
{
    return range_limit(value >> 18);
}

/* 16x16->32 multiply (smulbb on ARMv5E) */
#define MULTIPLY16(var,const)  (((short) (var)) * ((short) (const)))

/* IDCT implementation (verbatim Rockbox C code) */
#include "rbjpeg_idct.h"

static inline int clamp_component(int x)
{
    if(x > 255) return 255;
    if(x < 0)   return 0;
    return x;
}

static inline void yuv_to_rgb(int y, int u, int v, unsigned *r, unsigned *g, unsigned *b)
{
    int rv, guv, bu;
    y = y * YFAC + (YFAC >> 1);
    u = u - 128;
    v = v - 128;
    rv = RVFAC * v;
    guv = GUFAC * u + GVFAC * v;
    bu = BUFAC * u;
    *r = clamp_component((y + rv) / YFAC);
    *g = clamp_component((y + guv) / YFAC);
    *b = clamp_component((y + bu) / YFAC);
}

/* port: the idct_tbl function-pointer table is replaced by switches */
static const unsigned char idct_scale_tbl[5] = {
    PASS1_BITS, PASS1_BITS, 0, 0, 0
};

static inline void idct_v(int scale, int16_t *ws, int16_t *end)
{
    switch (scale) {
    case 1: jpeg_idct2v(ws, end); break;
    case 2: jpeg_idct4v(ws, end); break;
    case 3: jpeg_idct8v(ws, end); break;
    case 4: jpeg_idct16v(ws, end); break;
    default: break;
    }
}

static inline void idct_h(int scale, int16_t *ws, unsigned char *out,
                          int16_t *end, int rowstep)
{
    switch (scale) {
    case 0: jpeg_idct1h(ws, out, end, rowstep); break;
    case 1: jpeg_idct2h(ws, out, end, rowstep); break;
    case 2: jpeg_idct4h(ws, out, end, rowstep); break;
    case 3: jpeg_idct8h(ws, out, end, rowstep); break;
    default: jpeg_idct16h(ws, out, end, rowstep); break;
    }
}

/* JPEG decoder implementation */

INLINE const unsigned char *jpeg_getc(struct jpeg* p_jpeg)
{
    if (LIKELY(p_jpeg->len))
    {
        p_jpeg->len--;
        return p_jpeg->data++;
    } else
        return NULL;
}

INLINE bool skip_bytes(struct jpeg* p_jpeg, int count)
{
    if (count < 0) /* port: malformed segment length */
        return false;
    if (p_jpeg->len >= (unsigned)count)
    {
        p_jpeg->len -= count;
        p_jpeg->data += count;
        return true;
    } else {
        p_jpeg->data += p_jpeg->len;
        p_jpeg->len = 0;
        return false;
    }
}

INLINE void jpeg_putc(struct jpeg* p_jpeg)
{
    p_jpeg->len++;
    p_jpeg->data--;
}

#define e_skip_bytes(jpeg, count) \
do {\
    if (UNLIKELY(!skip_bytes((jpeg),(count)))) \
        return -1; \
} while (0)

#define e_getc(jpeg, code) \
({ \
    const unsigned char *c; \
    if (UNLIKELY(!(c = jpeg_getc(jpeg)))) \
        return (code); \
    *c; \
})

#define d_getc(jpeg, def) \
({ \
    const unsigned char *cp = jpeg_getc(jpeg); \
    unsigned char c = LIKELY(cp) ? *cp : (def); \
    c; \
})

/* Preprocess the JPEG JFIF file */
static int process_markers(struct jpeg* p_jpeg)
{
    unsigned char c;
    int marker_size; /* variable length of marker segment */
    int i, j, n;
    int ret = 0; /* returned flags */
    bool done = false;

    while (!done)
    {
        c = e_getc(p_jpeg, -1);
        if (c != 0xFF) /* no marker? */
        {
            continue; /* discard */
        }

        c = e_getc(p_jpeg, -1);
        switch (c)
        {
        case 0xFF: /* Previous FF was fill byte */
            jpeg_putc(p_jpeg); /* This FF could be start of a marker */
            continue;
        case 0x00: /* Zero stuffed byte */
            break; /* discard */

        case 0xC0: /* SOF Huff  - Baseline DCT */
        case 0xC1: /* port: extended sequential, Huffman: identical bitstream
                      for 8-bit samples and table ids 0/1 (checked below) */
            {
                ret |= SOF0;
                marker_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                marker_size |= e_getc(p_jpeg, -1); /* Lowbyte */
                n = e_getc(p_jpeg, -1); /* sample precision (= 8 or 12) */
                if (n != 8)
                {
                    return(-4); /* port: was -1. Unsupported sample precision */
                }
                p_jpeg->y_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                p_jpeg->y_size |= e_getc(p_jpeg, -1); /* Lowbyte */
                p_jpeg->x_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                p_jpeg->x_size |= e_getc(p_jpeg, -1); /* Lowbyte */

                n = (marker_size-2-6)/3;
                if (e_getc(p_jpeg, -1) != n || (n != 1 && n != 3))
                {
                    return(-2); /* Unsupported SOF0 component specification */
                }
                for (i=0; i<n; i++)
                {
                    /* Component info */
                    p_jpeg->frameheader[i].ID = e_getc(p_jpeg, -1);
                    p_jpeg->frameheader[i].horizontal_sampling =
                        (c = e_getc(p_jpeg, -1)) >> 4;
                    p_jpeg->frameheader[i].vertical_sampling = c & 0x0F;
                    p_jpeg->frameheader[i].quanttable_select =
                        e_getc(p_jpeg, -1);
                    if (p_jpeg->frameheader[i].horizontal_sampling > 2
                     || p_jpeg->frameheader[i].vertical_sampling > 2
                     /* port: reject 0 sampling and bad table ids */
                     || p_jpeg->frameheader[i].horizontal_sampling < 1
                     || p_jpeg->frameheader[i].vertical_sampling < 1
                     || p_jpeg->frameheader[i].quanttable_select > 3)
                    return -3; /* Unsupported SOF0 subsampling */
                }
                p_jpeg->blocks = n;
                p_jpeg->components = n;
            }
            break;

        case 0xC2: /* SOF Huff  - Progressive DCT*/
        case 0xC3: /* SOF Huff  - Spatial (sequential) lossless*/
        case 0xC5: /* SOF Huff  - Differential sequential DCT*/
        case 0xC6: /* SOF Huff  - Differential progressive DCT*/
        case 0xC7: /* SOF Huff  - Differential spatial*/
        case 0xC8: /* SOF Arith - Reserved for JPEG extensions*/
        case 0xC9: /* SOF Arith - Extended sequential DCT*/
        case 0xCA: /* SOF Arith - Progressive DCT*/
        case 0xCB: /* SOF Arith - Spatial (sequential) lossless*/
        case 0xCD: /* SOF Arith - Differential sequential DCT*/
        case 0xCE: /* SOF Arith - Differential progressive DCT*/
        case 0xCF: /* SOF Arith - Differential spatial*/
            {
                return (-4); /* other DCT model than baseline not implemented */
            }

        case 0xC4: /* Define Huffman Table(s) */
            {
                ret |= DHT;
                marker_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                marker_size |= e_getc(p_jpeg, -1); /* Lowbyte */
                marker_size -= 2;

                while (marker_size > 17) /* another table */
                {
                    c = e_getc(p_jpeg, -1);
                    marker_size--;
                    int sum = 0;
                    i = c & 0x0F; /* table index */
                    if (i > 1)
                    {
                        return (-5); /* Huffman table index out of range */
                    } else {
                        if (c & 0xF0) /* AC table */
                        {
                            for (j=0; j<16; j++)
                            {
                                p_jpeg->hufftable[i].huffmancodes_ac[j] =
                                    (c = e_getc(p_jpeg, -1));
                                sum += c;
                                marker_size -= 1;
                            }
                            if(16 + sum > AC_LEN)
                                return -10; /* longer than allowed */

                            for (; j < 16 + sum; j++)
                            {
                                p_jpeg->hufftable[i].huffmancodes_ac[j] =
                                    e_getc(p_jpeg, -1);
                                marker_size--;
                            }
                        }
                        else /* DC table */
                        {
                            for (j=0; j<16; j++)
                            {
                                p_jpeg->hufftable[i].huffmancodes_dc[j] =
                                    (c = e_getc(p_jpeg, -1));
                                sum += c;
                                marker_size--;
                            }
                            if(16 + sum > DC_LEN)
                                return -11; /* longer than allowed */

                            for (; j < 16 + sum; j++)
                            {
                                p_jpeg->hufftable[i].huffmancodes_dc[j] =
                                    e_getc(p_jpeg, -1);
                                marker_size--;
                            }
                        }
                    }
                } /* while */
                e_skip_bytes(p_jpeg, marker_size);
            }
            break;

        case 0xCC: /* Define Arithmetic coding conditioning(s) */
            return(-6); /* Arithmetic coding not supported */

        case 0xD8: /* Start of Image */
            break;
        case 0xD9: /* End of Image */
            break;
        case 0x01: /* for temp private use arith code */
            break; /* skip parameterless marker */
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: /* port: stray RSTn */
        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
            break;

        case 0xDA: /* Start of Scan */
            {
                ret |= SOS;
                marker_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                marker_size |= e_getc(p_jpeg, -1); /* Lowbyte */
                marker_size -= 2;

                n = (marker_size-1-3)/2;
                if (e_getc(p_jpeg, -1) != n || (n != 1 && n != 3))
                {
                    return (-7); /* Unsupported SOS component specification */
                }
                p_jpeg->scan_components = n;
                marker_size--;
                for (i=0; i<n; i++)
                {
                    p_jpeg->scanheader[i].ID = e_getc(p_jpeg, -1);
                    p_jpeg->scanheader[i].DC_select = (c = e_getc(p_jpeg, -1))
                        >> 4;
                    p_jpeg->scanheader[i].AC_select = c & 0x0F;
                    marker_size -= 2;
                }
                /* skip spectral information */
                e_skip_bytes(p_jpeg, marker_size);
                done = true;
            }
            break;

        case 0xDB: /* Define quantization Table(s) */
            {
                ret |= DQT;
                marker_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                marker_size |= e_getc(p_jpeg, -1); /* Lowbyte */
                marker_size -= 2;

                n = (marker_size)/(QUANT_TABLE_LENGTH+1); /* # of tables */
                for (i=0; i<n; i++)
                {
                    int id = e_getc(p_jpeg, -1); /* ID */
                    marker_size--;
                    if (id >= 4)
                    {
                        return (-8); /* Unsupported quantization table */
                    }
                    /* Read Quantisation table: */
                    for (j=0; j<QUANT_TABLE_LENGTH; j++)
                    {
                        p_jpeg->quanttable[id][j] = e_getc(p_jpeg, -1);
                        marker_size--;
                    }
                }
                e_skip_bytes(p_jpeg, marker_size);
            }
            break;

        case 0xDD: /* Define Restart Interval */
            {
                marker_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                marker_size |= e_getc(p_jpeg, -1); /* Lowbyte */
                marker_size -= 4;
                /* Highbyte */
                p_jpeg->restart_interval = e_getc(p_jpeg, -1) << 8;
                p_jpeg->restart_interval |= e_getc(p_jpeg, -1); /* Lowbyte */
                e_skip_bytes(p_jpeg, marker_size); /* skip segment */
            }
            break;

        case 0xDC: /* Define Number of Lines */
        case 0xDE: /* Define Hierarchical progression */
        case 0xDF: /* Expand Reference Component(s) */
        case 0xE0: /* Application Field 0*/
        case 0xE1: /* Application Field 1*/
        case 0xE2: /* Application Field 2*/
        case 0xE3: /* Application Field 3*/
        case 0xE4: /* Application Field 4*/
        case 0xE5: /* Application Field 5*/
        case 0xE6: /* Application Field 6*/
        case 0xE7: /* Application Field 7*/
        case 0xE8: /* Application Field 8*/
        case 0xE9: /* Application Field 9*/
        case 0xEA: /* Application Field 10*/
        case 0xEB: /* Application Field 11*/
        case 0xEC: /* Application Field 12*/
        case 0xED: /* Application Field 13*/
        case 0xEE: /* Application Field 14*/
        case 0xEF: /* Application Field 15*/
        case 0xFE: /* Comment */
            {
                marker_size = e_getc(p_jpeg, -1) << 8; /* Highbyte */
                marker_size |= e_getc(p_jpeg, -1); /* Lowbyte */
                marker_size -= 2;
                e_skip_bytes(p_jpeg, marker_size); /* skip segment */
            }
            break;

        default:
            return (-9); /* Unknown marker */
        } /* switch */
    } /* while */

    return (ret); /* return flags with seen markers */
}

static const struct huffman_table luma_table =
{
    {
        0x00,0x01,0x05,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B
    },
    {
        0x00,0x02,0x01,0x03,0x03,0x02,0x04,0x03,0x05,0x05,0x04,0x04,0x00,0x00,
        0x01,0x7D,0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,
        0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xA1,0x08,0x23,0x42,
        0xB1,0xC1,0x15,0x52,0xD1,0xF0,0x24,0x33,0x62,0x72,0x82,0x09,0x0A,0x16,
        0x17,0x18,0x19,0x1A,0x25,0x26,0x27,0x28,0x29,0x2A,0x34,0x35,0x36,0x37,
        0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x53,0x54,0x55,
        0x56,0x57,0x58,0x59,0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x73,
        0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
        0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0xA2,0xA3,0xA4,0xA5,
        0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,
        0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,
        0xD7,0xD8,0xD9,0xDA,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,
        0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA
    }
};

static const struct huffman_table chroma_table =
{
    {
        0x00,0x03,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,
        0x00,0x00,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B
    },
    {
        0x00,0x02,0x01,0x02,0x04,0x04,0x03,0x04,0x07,0x05,0x04,0x04,0x00,0x01,
        0x02,0x77,0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,
        0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xA1,0xB1,
        0xC1,0x09,0x23,0x33,0x52,0xF0,0x15,0x62,0x72,0xD1,0x0A,0x16,0x24,0x34,
        0xE1,0x25,0xF1,0x17,0x18,0x19,0x1A,0x26,0x27,0x28,0x29,0x2A,0x35,0x36,
        0x37,0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x53,0x54,
        0x55,0x56,0x57,0x58,0x59,0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,
        0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x82,0x83,0x84,0x85,0x86,0x87,
        0x88,0x89,0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0xA2,0xA3,
        0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,
        0xB9,0xBA,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,
        0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,
        0xEA,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA
    }
};

static void default_huff_tbl(struct jpeg* p_jpeg)
{

    MEMCPY(&p_jpeg->hufftable[0], &luma_table, sizeof(luma_table));
    MEMCPY(&p_jpeg->hufftable[1], &chroma_table, sizeof(chroma_table));

    return;
}

/* Compute the derived values for a Huffman table.
 * port: returns -1 on a malformed table (code space overflow), which in the
 * original could write past look_nbits[]. */
static int fix_huff_tbl(struct jpeg *p_jpeg, int* htbl, struct derived_tbl* dtbl)
{
    int p, i, l, si;
    int lookbits, ctr;
    char *huffsize = p_jpeg->huffsize;
    unsigned int *huffcode = p_jpeg->huffcode;
    unsigned int code;

    dtbl->pub = htbl; /* fill in back link */

    /* Figure C.1: make table of Huffman code length for each symbol */
    /* Note that this is in code-length order. */

    p = 0;
    for (l = 1; l <= 16; l++)
    {    /* all possible code length */
        for (i = 1; i <= (int) htbl[l-1]; i++)  /* all codes per length */
            huffsize[p++] = (char) l;
    }
    huffsize[p] = 0;

    /* Figure C.2: generate the codes themselves */
    /* Note that this is in code-length order. */

    code = 0;
    si = huffsize[0];
    p = 0;
    while (huffsize[p])
    {
        while (((int) huffsize[p]) == si)
        {
            huffcode[p++] = code;
            code++;
        }
        /* port: code must fit in si bits */
        if (code > (1u << si))
            return -1;
        code <<= 1;
        si++;
    }

    /* Figure F.15: generate decoding tables for bit-sequential decoding */

    p = 0;
    for (l = 1; l <= 16; l++)
    {
        if (htbl[l-1])
        {
            /* huffval[] index of 1st symbol of code length l */
            dtbl->valptr[l] = p;
            dtbl->mincode[l] = huffcode[p]; /* minimum code of length l */
            p += htbl[l-1];
            dtbl->maxcode[l] = huffcode[p-1]; /* maximum code of length l */
        }
        else
        {
            dtbl->maxcode[l] = -1;  /* -1 if no codes of this length */
        }
    }
    dtbl->maxcode[17] = 0xFFFFFL; /* ensures huff_DECODE terminates */

    /* Compute lookahead tables to speed up decoding.
    * First we set all the table entries to 0, indicating "too long";
    * then we iterate through the Huffman codes that are short enough and
    * fill in all the entries that correspond to bit sequences starting
    * with that code.
    */

    MEMSET(dtbl->look_nbits, 0, sizeof(dtbl->look_nbits));

    p = 0;
    for (l = 1; l <= HUFF_LOOKAHEAD; l++)
    {
        for (i = 1; i <= (int) htbl[l-1]; i++, p++)
        {
            /* l = current code's length, p = its index in huffcode[] &
             * huffval[]. Generate left-justified code followed by all possible
             * bit sequences
             */
            lookbits = huffcode[p] << (HUFF_LOOKAHEAD-l);
            for (ctr = 1 << (HUFF_LOOKAHEAD-l); ctr > 0; ctr--)
            {
                dtbl->look_nbits[lookbits] = l;
                dtbl->look_sym[lookbits] = htbl[16+p];
                lookbits++;
            }
        }
    }
    return 0;
}


/* zag[i] is the natural-order position of the i'th element of zigzag order. */
static const unsigned char zag[] =
{
      0,   8,   1,   2,   9,  16,  24,  17,
     10,   3,   4,  11,  18,  25,  32,  40,
     33,  26,  19,  12,   5,   6,  13,  20,
     27,  34,  41,  48,  56,  49,  42,  35,
     28,  21,  14,   7,  15,  22,  29,  36,
     43,  50,  57,  58,  51,  44,  37,  30,
     23,  31,  38,  45,  52,  59,  60,  53,
     46,  39,  47,  54,  61,  62,  55,  63,
      0,   1,   8,  16,   9,   2,   3,  10,
     17,  24,  32,  25,  18,  11,   4,   5,
     12,  19,  26,  33,  40,  48,  41,  34,
     27,  20,  13,   6,   7,  14,  21,  28,
     35,  42,  49,  56,  57,  50,  43,  36,
     29,  22,  15,  23,  30,  37,  44,  51,
     58,  59,  52,  45,  38,  31,  39,  46,
     53,  60,  61,  54,  47,  55,  62,  63,
};

/* zig[i] is the the zig-zag order position of the i'th element of natural
 * order, reading left-to-right then top-to-bottom.
 */
static const unsigned char zig[] =
{
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
};

/* Reformat some image header data so that the decoder can use it properly.
 * port: returns -1 for unsupported layouts instead of silently continuing. */
static int fix_headers(struct jpeg* p_jpeg)
{
    int i;

    for (i=0; i<4; i++)
        p_jpeg->store_pos[i] = i; /* default ordering */

    /* port: a single-component scan is never interleaved, whatever its
       sampling factors say; decode it as 1x1. Chroma must be 1x1. */
    if (p_jpeg->components == 1)
    {
        p_jpeg->frameheader[0].horizontal_sampling = 1;
        p_jpeg->frameheader[0].vertical_sampling = 1;
    }
    else
    {
        for (i = 1; i < 3; i++)
            if (p_jpeg->frameheader[i].horizontal_sampling != 1
             || p_jpeg->frameheader[i].vertical_sampling != 1)
                return -1;
    }
    if (p_jpeg->scan_components != p_jpeg->components)
        return -1; /* port: non-interleaved multi-scan files unsupported */

    /* assignments for the decoding of blocks */
    if (p_jpeg->frameheader[0].horizontal_sampling == 2
        && p_jpeg->frameheader[0].vertical_sampling == 1)
    {   /* 4:2:2 */
        p_jpeg->blocks = 4;
        p_jpeg->x_mbl = (p_jpeg->x_size+15) / 16;
        p_jpeg->x_phys = p_jpeg->x_mbl * 16;
        p_jpeg->y_mbl = (p_jpeg->y_size+7) / 8;
        p_jpeg->y_phys = p_jpeg->y_mbl * 8;
        p_jpeg->mcu_membership[0] = 0; /* Y1=Y2=0, U=1, V=2 */
        p_jpeg->mcu_membership[1] = 0;
        p_jpeg->mcu_membership[2] = 1;
        p_jpeg->mcu_membership[3] = 2;
        p_jpeg->tab_membership[0] = 0; /* DC, DC, AC, AC */
        p_jpeg->tab_membership[1] = 0;
        p_jpeg->tab_membership[2] = 1;
        p_jpeg->tab_membership[3] = 1;
        p_jpeg->subsample_x[0] = 1;
        p_jpeg->subsample_x[1] = 2;
        p_jpeg->subsample_x[2] = 2;
        p_jpeg->subsample_y[0] = 1;
        p_jpeg->subsample_y[1] = 1;
        p_jpeg->subsample_y[2] = 1;
    }
    else if (p_jpeg->frameheader[0].horizontal_sampling == 1
        && p_jpeg->frameheader[0].vertical_sampling == 2)
    {   /* 4:2:2 vertically subsampled */
        p_jpeg->store_pos[1] = 2; /* block positions are mirrored */
        p_jpeg->store_pos[2] = 1;
        p_jpeg->blocks = 4;
        p_jpeg->x_mbl = (p_jpeg->x_size+7) / 8;
        p_jpeg->x_phys = p_jpeg->x_mbl * 8;
        p_jpeg->y_mbl = (p_jpeg->y_size+15) / 16;
        p_jpeg->y_phys = p_jpeg->y_mbl * 16;
        p_jpeg->mcu_membership[0] = 0; /* Y1=Y2=0, U=1, V=2 */
        p_jpeg->mcu_membership[1] = 0;
        p_jpeg->mcu_membership[2] = 1;
        p_jpeg->mcu_membership[3] = 2;
        p_jpeg->tab_membership[0] = 0; /* DC, DC, AC, AC */
        p_jpeg->tab_membership[1] = 0;
        p_jpeg->tab_membership[2] = 1;
        p_jpeg->tab_membership[3] = 1;
        p_jpeg->subsample_x[0] = 1;
        p_jpeg->subsample_x[1] = 1;
        p_jpeg->subsample_x[2] = 1;
        p_jpeg->subsample_y[0] = 1;
        p_jpeg->subsample_y[1] = 2;
        p_jpeg->subsample_y[2] = 2;
    }
    else if (p_jpeg->frameheader[0].horizontal_sampling == 2
        && p_jpeg->frameheader[0].vertical_sampling == 2)
    {   /* 4:2:0 */
        p_jpeg->blocks = 6;
        p_jpeg->x_mbl = (p_jpeg->x_size+15) / 16;
        p_jpeg->x_phys = p_jpeg->x_mbl * 16;
        p_jpeg->y_mbl = (p_jpeg->y_size+15) / 16;
        p_jpeg->y_phys = p_jpeg->y_mbl * 16;
        p_jpeg->mcu_membership[0] = 0;
        p_jpeg->mcu_membership[1] = 0;
        p_jpeg->mcu_membership[2] = 0;
        p_jpeg->mcu_membership[3] = 0;
        p_jpeg->mcu_membership[4] = 1;
        p_jpeg->mcu_membership[5] = 2;
        p_jpeg->tab_membership[0] = 0;
        p_jpeg->tab_membership[1] = 0;
        p_jpeg->tab_membership[2] = 0;
        p_jpeg->tab_membership[3] = 0;
        p_jpeg->tab_membership[4] = 1;
        p_jpeg->tab_membership[5] = 1;
        p_jpeg->subsample_x[0] = 1;
        p_jpeg->subsample_x[1] = 2;
        p_jpeg->subsample_x[2] = 2;
        p_jpeg->subsample_y[0] = 1;
        p_jpeg->subsample_y[1] = 2;
        p_jpeg->subsample_y[2] = 2;
    }
    else if (p_jpeg->frameheader[0].horizontal_sampling == 1
        && p_jpeg->frameheader[0].vertical_sampling == 1)
    {   /* 4:4:4 */
        /* don't overwrite p_jpeg->blocks */
        p_jpeg->x_mbl = (p_jpeg->x_size+7) / 8;
        p_jpeg->x_phys = p_jpeg->x_mbl * 8;
        p_jpeg->y_mbl = (p_jpeg->y_size+7) / 8;
        p_jpeg->y_phys = p_jpeg->y_mbl * 8;
        p_jpeg->mcu_membership[0] = 0;
        p_jpeg->mcu_membership[1] = 1;
        p_jpeg->mcu_membership[2] = 2;
        p_jpeg->tab_membership[0] = 0;
        p_jpeg->tab_membership[1] = 1;
        p_jpeg->tab_membership[2] = 1;
        p_jpeg->subsample_x[0] = 1;
        p_jpeg->subsample_x[1] = 1;
        p_jpeg->subsample_x[2] = 1;
        p_jpeg->subsample_y[0] = 1;
        p_jpeg->subsample_y[1] = 1;
        p_jpeg->subsample_y[2] = 1;
    }
    else
    {
        return -1;
    }
    return 0;
}

INLINE int fix_huff_tables(struct jpeg *p_jpeg)
{
    if (fix_huff_tbl(p_jpeg, p_jpeg->hufftable[0].huffmancodes_dc,
        &p_jpeg->dc_derived_tbls[0]) ||
        fix_huff_tbl(p_jpeg, p_jpeg->hufftable[0].huffmancodes_ac,
        &p_jpeg->ac_derived_tbls[0]) ||
        fix_huff_tbl(p_jpeg, p_jpeg->hufftable[1].huffmancodes_dc,
        &p_jpeg->dc_derived_tbls[1]) ||
        fix_huff_tbl(p_jpeg, p_jpeg->hufftable[1].huffmancodes_ac,
        &p_jpeg->ac_derived_tbls[1]))
        return -1;
    return 0;
}

/* Because some of the IDCT routines never multiply by any constants, and
 * therefore do not produce shifted output, we add the shift into the
 * quantization table when one of these IDCT routines is used, rather than
 * have the IDCT shift each value it processes.
 */
INLINE void fix_quant_tables(struct jpeg *p_jpeg)
{
    int shift, i, j;
    const int k = 2;
    /* port: Rockbox always used table 0 for luma and 1 for chroma; honour
       the frame header's table selectors by copying them into slots 0/1. */
    int16_t tmp[2][QUANT_TABLE_LENGTH];
    int sel0 = p_jpeg->frameheader[0].quanttable_select & 3;
    int sel1 = p_jpeg->components > 1 ?
               (p_jpeg->frameheader[1].quanttable_select & 3) : 1;
    MEMCPY(tmp[0], p_jpeg->quanttable[sel0], sizeof(tmp[0]));
    MEMCPY(tmp[1], p_jpeg->quanttable[sel1], sizeof(tmp[1]));
    MEMCPY(p_jpeg->quanttable[0], tmp, sizeof(tmp));

    for (i = 0; i < k; i++)
    {
        shift = idct_scale_tbl[p_jpeg->v_scale[i]];
        if (shift)
        {
            for (j = 0; j < 64; j++)
                p_jpeg->quanttable[i][j] <<= shift;
        }
    }
}

/*
* These functions/macros provide the in-line portion of bit fetching.
* Use check_bit_buffer to ensure there are N bits in get_buffer
* before using get_bits, peek_bits, or drop_bits.
*/

static void fill_bit_buffer(struct jpeg* p_jpeg)
{
    unsigned char byte, marker;

    if (p_jpeg->marker_val)
        p_jpeg->marker_ind += 16;
    byte = d_getc(p_jpeg, 0);
    if (UNLIKELY(byte == 0xFF)) /* legal marker can be byte stuffing or RSTm */
    {   /* simplification: just skip the (one-byte) marker code */
        marker = d_getc(p_jpeg, 0);
        if ((marker & ~7) == 0xD0)
        {
            p_jpeg->marker_val = marker;
            p_jpeg->marker_ind = 8;
        }
    }
    p_jpeg->bitbuf = (p_jpeg->bitbuf << 8) | byte;

    byte = d_getc(p_jpeg, 0);
    if (UNLIKELY(byte == 0xFF)) /* legal marker can be byte stuffing or RSTm */
    {   /* simplification: just skip the (one-byte) marker code */
        marker = d_getc(p_jpeg, 0);
        if ((marker & ~7) == 0xD0)
        {
            p_jpeg->marker_val = marker;
            p_jpeg->marker_ind = 0;
        }
    }
    p_jpeg->bitbuf = (p_jpeg->bitbuf << 8) | byte;
    p_jpeg->bitbuf_bits += 16;
}

INLINE void check_bit_buffer(struct jpeg *p_jpeg, int nbits)
{
    if (nbits > p_jpeg->bitbuf_bits)
        fill_bit_buffer(p_jpeg);
}

INLINE int get_bits(struct jpeg *p_jpeg, int nbits)
{
    return ((int) (p_jpeg->bitbuf >> (p_jpeg->bitbuf_bits -= nbits))) &
        (BIT_N(nbits)-1);
}

INLINE int peek_bits(struct jpeg *p_jpeg, int nbits)
{
    return ((int) (p_jpeg->bitbuf >> (p_jpeg->bitbuf_bits - nbits))) &
        (BIT_N(nbits)-1);
}

INLINE void drop_bits(struct jpeg *p_jpeg, int nbits)
{
    p_jpeg->bitbuf_bits -= nbits;
}

/* re-synchronize to entropy data (skip restart marker) */
static void search_restart(struct jpeg *p_jpeg)
{
    if (p_jpeg->marker_val)
    {
        p_jpeg->marker_val = 0;
        p_jpeg->bitbuf_bits = p_jpeg->marker_ind;
        p_jpeg->marker_ind = 0;
        return;
    }
    unsigned char byte;
    p_jpeg->bitbuf_bits = 0;
    while ((byte = d_getc(p_jpeg, 0xFF)))
    {
        if (byte == 0xff)
        {
            byte = d_getc(p_jpeg, 0xD0);
            if ((byte & ~7) == 0xD0)
            {
                return;
            }
            else
                jpeg_putc(p_jpeg);
        }
    }
}

/* Figure F.12: extend sign bit.
 * port: written without left-shifting a negative value (same result). */
#define HUFF_EXTEND(x,s) \
({ \
    int x__ = x; \
    int s__ = s; \
    x__ & BIT_N(s__- 1) ? x__ : x__ - (int)BIT_N(s__) + 1; \
})

/* Decode a single value */
#define huff_decode_dc(p_jpeg, tbl, s, r) \
{ \
    int nb, look; \
\
    check_bit_buffer((p_jpeg), HUFF_LOOKAHEAD); \
    look = peek_bits((p_jpeg), HUFF_LOOKAHEAD); \
    if ((nb = (tbl)->look_nbits[look]) != 0) \
    { \
        drop_bits((p_jpeg), nb); \
        s = (tbl)->look_sym[look] & 15; /* port: & 15 */ \
        check_bit_buffer((p_jpeg), s); \
        r = get_bits((p_jpeg), s); \
    } else { \
        /*  slow_DECODE(s, HUFF_LOOKAHEAD+1)) < 0); */ \
        long code; \
        nb=HUFF_LOOKAHEAD+1; \
        check_bit_buffer((p_jpeg), nb); \
        code = get_bits((p_jpeg), nb); \
        while (code > (tbl)->maxcode[nb]) \
        { \
            code <<= 1; \
            check_bit_buffer((p_jpeg), 1); \
            code |= get_bits((p_jpeg), 1); \
            nb++; \
        } \
        if (nb > 16) /* error in Huffman */ \
        { \
            r = 0; s = 0; /* fake a zero, this is most safe */ \
        } else { \
            s = (tbl)->pub[16 + (tbl)->valptr[nb] + \
                ((int) (code - (tbl)->mincode[nb]))] & 15; /* port: & 15 */ \
            check_bit_buffer((p_jpeg), s); \
            r = get_bits((p_jpeg), s); \
        } \
    } /* end slow decode */ \
}

#define huff_decode_ac(p_jpeg, tbl, s) \
{ \
    int nb, look; \
\
    check_bit_buffer((p_jpeg), HUFF_LOOKAHEAD); \
    look = peek_bits((p_jpeg), HUFF_LOOKAHEAD); \
    if ((nb = (tbl)->look_nbits[look]) != 0) \
    { \
        drop_bits((p_jpeg), nb); \
        s = (tbl)->look_sym[look]; \
    } else { \
        /*  slow_DECODE(s, HUFF_LOOKAHEAD+1)) < 0); */ \
        long code; \
        nb=HUFF_LOOKAHEAD+1; \
        check_bit_buffer((p_jpeg), nb); \
        code = get_bits((p_jpeg), nb); \
        while (code > (tbl)->maxcode[nb]) \
        { \
            code <<= 1; \
            check_bit_buffer((p_jpeg), 1); \
            code |= get_bits((p_jpeg), 1); \
            nb++; \
        } \
        if (nb > 16) /* error in Huffman */ \
        { \
            s = 0; /* fake a zero, this is most safe */ \
        } else { \
            s = (tbl)->pub[16 + (tbl)->valptr[nb] + \
                ((int) (code - (tbl)->mincode[nb]))]; \
        } \
    } /* end slow decode */ \
}

static struct img_part *store_row_jpeg(struct jpeg *p_jpeg)
{
    int mcu_hscale = p_jpeg->h_scale[1];
    int mcu_vscale = p_jpeg->v_scale[1];
    unsigned int width = p_jpeg->x_mbl << mcu_hscale;
    unsigned int b_width = width * JPEG_PIX_SZ;
    int height = BIT_N(mcu_vscale);
    int x;
    if (!p_jpeg->mcu_row) /* Need to decode a new row of MCUs */
    {
        p_jpeg->out_ptr = (unsigned char *)p_jpeg->img_buf;
        int store_offs[4];
        unsigned mcu_width = BIT_N(mcu_hscale);
        int mcu_offset = JPEG_PIX_SZ << mcu_hscale;
        unsigned char *out = p_jpeg->out_ptr;
        store_offs[p_jpeg->store_pos[0]] = 0;
        store_offs[p_jpeg->store_pos[1]] = JPEG_PIX_SZ << p_jpeg->h_scale[0];
        store_offs[p_jpeg->store_pos[2]] = b_width << p_jpeg->v_scale[0];
        store_offs[p_jpeg->store_pos[3]] = store_offs[1] + store_offs[2];
        /* decoded DCT coefficients */
        int16_t block[IDCT_WS_SIZE] __attribute__((aligned(8)));
        for (x = 0; x < p_jpeg->x_mbl; x++)
        {
            int blkn;
            for (blkn = 0; blkn < p_jpeg->blocks; blkn++)
            {
                int ci = p_jpeg->mcu_membership[blkn]; /* component index */
                int ti = p_jpeg->tab_membership[blkn]; /* table index */
                bool transpose = p_jpeg->v_scale[!!ci] > 2;
                int k = 1; /* coefficient index */
                int s, r; /* huffman values */
                struct derived_tbl* dctbl = &p_jpeg->dc_derived_tbls[ti];
                struct derived_tbl* actbl = &p_jpeg->ac_derived_tbls[ti];

                /* Section F.2.2.1: decode the DC coefficient difference */
                huff_decode_dc(p_jpeg, dctbl, s, r);

                {
                    s = s ? HUFF_EXTEND(r, s) : 0; /* port: s==0 guard */
                    p_jpeg->last_dc_val[ci] += s;
                    /* output it (assumes zag[0] = 0) */
                    block[0] = MULTIPLY16(p_jpeg->last_dc_val[ci],
                        p_jpeg->quanttable[!!ci][0]);
                    /* coefficient buffer must be cleared
                       port: was zero_need * sizeof(int), clearing twice the
                       needed range */
                    MEMSET(block+1, 0, p_jpeg->zero_need[!!ci] * sizeof(int16_t));
                    /* Section F.2.2.2: decode the AC coefficients */
                    while(true)
                    {
                        huff_decode_ac(p_jpeg, actbl, s);
                        r = s >> 4;
                        s &= 15;
                        k += r;
                        if (s)
                        {
                            check_bit_buffer(p_jpeg, s);
                            if (k >= p_jpeg->k_need[!!ci])
                                goto skip_rest;
                            r = get_bits(p_jpeg, s);
                            r = HUFF_EXTEND(r, s);
                            r = MULTIPLY16(r, p_jpeg->quanttable[!!ci][k]);
                            block[zag[transpose ? k : k + 64]] = r ;
                        }
                        else
                        {
                            if (r != 15)
                                goto block_end;
                        }
                        if ((++k) & 64)
                            goto block_end;
                    }  /* for k */
                }
                for (; k < 64; k++)
                {
                    huff_decode_ac(p_jpeg, actbl, s);
                    r = s >> 4;
                    s &= 15;

                    if (s)
                    {
                        k += r;
                        check_bit_buffer(p_jpeg, s);
skip_rest:
                        drop_bits(p_jpeg, s);
                    }
                    else
                    {
                        if (r != 15)
                            break;
                        k += r;
                    }
                }  /* for k */
block_end:
                {
                    int idct_cols = BIT_N(MIN(p_jpeg->h_scale[!!ci], 3));
                    int idct_rows = BIT_N(p_jpeg->v_scale[!!ci]);
                    unsigned char *b_out = out + (ci ? ci : store_offs[blkn]);
                    if (p_jpeg->v_scale[!!ci])
                        idct_v(p_jpeg->v_scale[!!ci], block,
                            transpose ? block + 8 * idct_cols
                                      : block + idct_cols);
                    int16_t * h_block = transpose ? block + 64 : block;
                    idct_h(p_jpeg->h_scale[!!ci], h_block, b_out,
                        h_block + idct_rows * 8, b_width);
                }
            } /* for blkn */
            unsigned int xp;
            int yp;
            unsigned char *row = out;
            if (p_jpeg->blocks == 1)
            {
                for (yp = 0; yp < height; yp++, row += b_width)
                {
                    unsigned char *px = row;
                    for (xp = 0; xp < mcu_width; xp++, px += JPEG_PIX_SZ)
                    {
                        px[1] = px[2] = px[0];
                    }
                }
            }
            out += mcu_offset;
            if (p_jpeg->restart_interval && --p_jpeg->restart == 0)
            {   /* if a restart marker is due: */
                p_jpeg->restart = p_jpeg->restart_interval; /* count again */
                search_restart(p_jpeg); /* align the bitstream */
                p_jpeg->last_dc_val[0] = p_jpeg->last_dc_val[1] =
                                 p_jpeg->last_dc_val[2] = 0; /* reset decoder */
            }
        }
    } /* if !p_jpeg->mcu_row */
    p_jpeg->mcu_row = (p_jpeg->mcu_row + 1) & (height - 1);
    p_jpeg->part.len = width;
    p_jpeg->part.buf = (jpeg_pix_t *)p_jpeg->out_ptr;
    p_jpeg->out_ptr += b_width;
    return &(p_jpeg->part);
}

static int calc_scale(int in_size, int out_size)
{
    int scale = 0;
    out_size <<= 3;
    for (scale = 0; scale < 3; scale++)
    {
        if (out_size <= in_size)
            break;
        else
            in_size <<= 1;
    }
    return scale;
}

/**************** end JPEG code ********************/

/**************** begin scaler (resize.c) ********************/

#define SC_OUT(n, c) (((n) + (1 << 23)) >> 24)

/* intermediate type used by the scaler for color output.
   port: alpha channel removed */
struct uint32_argb {
    uint32_t r;
    uint32_t g;
    uint32_t b;
};

struct rowset {
    short rowstep;
    short rowstart;
    short rowstop;
};

struct scaler_context {
    uint32_t h_i_val;
    uint32_t h_o_val;
    uint32_t v_i_val;
    uint32_t v_o_val;
    int src_w, src_h; /* port: replaces struct dim *src */
    int dst_w, dst_h; /* port: replaces struct bitmap *bm */
    unsigned char *buf;
    struct jpeg *args;
    bool h_area;      /* port: replaces h_scaler pointer */
    bool fromyuv;     /* port: replaces output_row pointer */
    uint8_t *out;     /* port: RGB565 destination */
    int stride;
};

/* read new img_part unconditionally, return false on failure */
#define FILL_BUF_INIT(img_part, args) { \
    img_part = store_row_jpeg(args); \
    if (img_part == NULL) \
        return false; \
}

/* read new img_part if current one is empty, return false on failure */
#define FILL_BUF(img_part, args) { \
    if (img_part->len == 0) \
        img_part = store_row_jpeg(args); \
    if (img_part == NULL) \
        return false; \
}

/* horizontal area average scaler */
static bool scale_h_area(void *out_line_ptr,
                         struct scaler_context *ctx, bool accum)
{
    unsigned int ix, ox, oxe, mul;
    const uint32_t h_i_val = ctx->h_i_val,
                   h_o_val = ctx->h_o_val;
    struct uint32_argb rgbvalacc = { 0, 0, 0 },
                       rgbvaltmp = { 0, 0, 0 },
                      *out_line = (struct uint32_argb *)out_line_ptr;
    struct img_part *part;
    FILL_BUF_INIT(part,ctx->args);
    ox = 0;
    oxe = 0;
    mul = 0;
    for (ix = 0; ix < (unsigned int)ctx->src_w; ix++)
    {
        oxe += h_o_val;
        /* end of current area has been reached */
        /* fill buffer if needed */
        FILL_BUF(part,ctx->args);
        if (oxe >= h_i_val)
        {
            /* "reset" error, which now represents partial coverage of next
               pixel by the next area
            */
            oxe -= h_i_val;

/* generic C math */
            /* add saved partial pixel from start of area */
            rgbvalacc.r = rgbvalacc.r * h_o_val + rgbvaltmp.r * mul;
            rgbvalacc.g = rgbvalacc.g * h_o_val + rgbvaltmp.g * mul;
            rgbvalacc.b = rgbvalacc.b * h_o_val + rgbvaltmp.b * mul;

            /* get new pixel , then add its partial coverage to this area */
            rgbvaltmp.r = part->buf->red;
            rgbvaltmp.g = part->buf->green;
            rgbvaltmp.b = part->buf->blue;
            mul = h_o_val - oxe;
            rgbvalacc.r += rgbvaltmp.r * mul;
            rgbvalacc.g += rgbvaltmp.g * mul;
            rgbvalacc.b += rgbvaltmp.b * mul;
            rgbvalacc.r = (rgbvalacc.r + (1 << 21)) >> 22;
            rgbvalacc.g = (rgbvalacc.g + (1 << 21)) >> 22;
            rgbvalacc.b = (rgbvalacc.b + (1 << 21)) >> 22;
            /* store or accumulate to output row */
            if (accum)
            {
                rgbvalacc.r += out_line[ox].r;
                rgbvalacc.g += out_line[ox].g;
                rgbvalacc.b += out_line[ox].b;
            }
            out_line[ox].r = rgbvalacc.r;
            out_line[ox].g = rgbvalacc.g;
            out_line[ox].b = rgbvalacc.b;
            /* reset accumulator */
            rgbvalacc.r = 0;
            rgbvalacc.g = 0;
            rgbvalacc.b = 0;
            mul = oxe;
            ox += 1;
        /* inside an area */
        } else {
            /* add pixel value to accumulator */
            rgbvalacc.r += part->buf->red;
            rgbvalacc.g += part->buf->green;
            rgbvalacc.b += part->buf->blue;
        }
        part->buf++;
        part->len--;
    }
    return true;
}

/* horizontal linear scaler */
static bool scale_h_linear(void *out_line_ptr, struct scaler_context *ctx,
                           bool accum)
{
    unsigned int ix, ox, ixe;
    const uint32_t h_i_val = ctx->h_i_val,
                   h_o_val = ctx->h_o_val;
    /* port: explicit zero init instead of the "x = x" warning hack */
    struct uint32_argb rgbval = { 0, 0, 0 }, rgbinc = { 0, 0, 0 },
                      *out_line = (struct uint32_argb*)out_line_ptr;
    struct img_part *part;
    FILL_BUF_INIT(part,ctx->args);
    ix = 0;
    /* The error is set so that values are initialized on the first pass. */
    ixe = h_o_val;
    for (ox = 0; ox < (uint32_t)ctx->dst_w; ox++)
    {
        if (ixe >= h_o_val)
        {
            /* Store the new "current" pixel value in rgbval, and the color
               step value in rgbinc.
            */
            ixe -= h_o_val;
            rgbinc.r = -(part->buf->red);
            rgbinc.g = -(part->buf->green);
            rgbinc.b = -(part->buf->blue);
/* generic C math */
            rgbval.r = (part->buf->red) * h_o_val;
            rgbval.g = (part->buf->green) * h_o_val;
            rgbval.b = (part->buf->blue) * h_o_val;
            ix += 1;
            /* If this wasn't the last pixel, add the next one to rgbinc. */
            if (LIKELY(ix < (uint32_t)ctx->src_w)) {
                part->buf++;
                part->len--;
                /* Fetch new pixels if needed */
                FILL_BUF(part,ctx->args);
                rgbinc.r += part->buf->red;
                rgbinc.g += part->buf->green;
                rgbinc.b += part->buf->blue;
                /* Add a partial step to rgbval, in this pixel isn't precisely
                   aligned with the new source pixel
                */
/* generic C math */
                rgbval.r += rgbinc.r * ixe;
                rgbval.g += rgbinc.g * ixe;
                rgbval.b += rgbinc.b * ixe;
            }
            /* Now multiply the color increment to its proper value */
            rgbinc.r *= h_i_val;
            rgbinc.g *= h_i_val;
            rgbinc.b *= h_i_val;
        } else {
            rgbval.r += rgbinc.r;
            rgbval.g += rgbinc.g;
            rgbval.b += rgbinc.b;
        }
        /* round and scale values, and accumulate or store to output */
        if (accum)
        {
            out_line[ox].r += (rgbval.r + (1 << 21)) >> 22;
            out_line[ox].g += (rgbval.g + (1 << 21)) >> 22;
            out_line[ox].b += (rgbval.b + (1 << 21)) >> 22;
        } else {
            out_line[ox].r = (rgbval.r + (1 << 21)) >> 22;
            out_line[ox].g = (rgbval.g + (1 << 21)) >> 22;
            out_line[ox].b = (rgbval.b + (1 << 21)) >> 22;
        }
        ixe += h_i_val;
    }
    return true;
}

static inline bool h_scaler(void *out_line_ptr, struct scaler_context *ctx,
                            bool accum)
{
    return ctx->h_area ? scale_h_area(out_line_ptr, ctx, accum)
                       : scale_h_linear(out_line_ptr, ctx, accum);
}

/* port: zero the padding after tw pixels */
static void pad_row(uint8_t *dest, int used, int stride)
{
    for (; used < stride; used++)
        dest[used] = 0;
}

static inline void put565(uint8_t *dest, unsigned r, unsigned g, unsigned b)
{
    /* LCD_DEPTH < 24 packing from resize.c / bmp.c, delta = 127 (no dither) */
    r = (31 * r + (r >> 3) + 127) >> 8;
    g = (63 * g + (g >> 2) + 127) >> 8;
    b = (31 * b + (b >> 3) + 127) >> 8;
    unsigned v = (r << 11) | (g << 5) | b;
    dest[0] = (uint8_t)v;
    dest[1] = (uint8_t)(v >> 8);
}

/* output_row_32_native / output_row_32_native_fromyuv */
static void output_row_32(uint32_t row, void *row_in,
                          struct scaler_context *ctx)
{
    int col;
    struct uint32_argb *qp = (struct uint32_argb *)row_in;
    uint8_t *dest = ctx->out + (uint32_t)ctx->stride * row;
    unsigned r, g, b, y, u, v;

    if (ctx->fromyuv) {
        for (col = 0; col < ctx->dst_w; col++, qp++, dest += 2) {
            y = SC_OUT(qp->b, ctx);
            u = SC_OUT(qp->g, ctx);
            v = SC_OUT(qp->r, ctx);
            yuv_to_rgb(y, u, v, &r, &g, &b);
            put565(dest, r, g, b);
        }
    } else {
        for (col = 0; col < ctx->dst_w; col++, qp++, dest += 2) {
            r = SC_OUT(qp->r, ctx);
            g = SC_OUT(qp->g, ctx);
            b = SC_OUT(qp->b, ctx);
            put565(dest, r, g, b);
        }
    }
    pad_row(ctx->out + (uint32_t)ctx->stride * row, ctx->dst_w * 2,
            ctx->stride);
}

/* vertical area average scaler */
static inline bool scale_v_area(struct rowset *rset, struct scaler_context *ctx)
{
    uint32_t mul, oy, iy, oye;
    const uint32_t v_i_val = ctx->v_i_val,
                   v_o_val = ctx->v_o_val;

    /* Set up rounding and scale factors */
    mul = 0;
    oy = rset->rowstart;
    oye = 0;
    uint32_t *rowacc = (uint32_t *) ctx->buf,
             *rowtmp = rowacc + ctx->dst_w * 3,
             *rowacc_px, *rowtmp_px;
    memset((void *)ctx->buf, 0, ctx->dst_w * 2 * sizeof(struct uint32_argb));
    /* zero the accumulator and temp rows */
    for (iy = 0; iy < (unsigned int)ctx->src_h; iy++)
    {
        oye += v_o_val;
        /* end of current area has been reached */
        if (oye >= v_i_val)
        {
            /* "reset" error, which now represents partial coverage of the next
               row by the next area
            */
            oye -= v_i_val;
            /* add stored partial row to accumulator */
            for(rowacc_px = rowacc, rowtmp_px = rowtmp; rowacc_px != rowtmp;
                rowacc_px++, rowtmp_px++)
                *rowacc_px = *rowacc_px * v_o_val + *rowtmp_px * mul;
            /* store new scaled row in temp row */
            if(!h_scaler(rowtmp, ctx, false))
                return false;
            /* add partial coverage by new row to this area, then round and
               scale to final value
            */
            mul = v_o_val - oye;
            for(rowacc_px = rowacc, rowtmp_px = rowtmp; rowacc_px != rowtmp;
                rowacc_px++, rowtmp_px++)
                *rowacc_px += mul * *rowtmp_px;
            output_row_32(oy, (void*)rowacc, ctx);
            /* clear accumulator row, store partial coverage for next row */
            memset((void *)rowacc, 0, ctx->dst_w * sizeof(struct uint32_argb));
            mul = oye;
            oy += rset->rowstep;
        /* inside an area */
        } else {
            /* accumulate new scaled row to rowacc */
            if (!h_scaler(rowacc, ctx, true))
                return false;
        }
    }
    return true;
}

/* vertical linear scaler */
static inline bool scale_v_linear(struct rowset *rset,
                                  struct scaler_context *ctx)
{
    uint32_t iy, iye;
    int32_t oy;
    const uint32_t v_i_val = ctx->v_i_val,
                   v_o_val = ctx->v_o_val;
    /* Set up our buffers, to store the increment and current value for each
       column, and one temp buffer used to read in new rows.
    */
    uint32_t *rowinc = (uint32_t *)(ctx->buf),
             *rowval = rowinc + ctx->dst_w * 3,
             *rowtmp = rowval + ctx->dst_w * 3,
             *rowinc_px, *rowval_px, *rowtmp_px;

    iy = 0;
    iye = v_o_val;
    /* get first scaled row in rowtmp */
    if(!h_scaler((void*)rowtmp, ctx, false))
        return false;
    for (oy = rset->rowstart; oy != rset->rowstop; oy += rset->rowstep)
    {
        if (iye >= v_o_val)
        {
            iye -= v_o_val;
            iy += 1;
            for(rowinc_px = rowinc, rowtmp_px = rowtmp, rowval_px = rowval;
                rowinc_px < rowval; rowinc_px++, rowtmp_px++, rowval_px++)
            {
                *rowinc_px = -*rowtmp_px;
                *rowval_px = *rowtmp_px * v_o_val;
            }
            if (iy < (uint32_t)ctx->src_h)
            {
                if (!h_scaler((void*)rowtmp, ctx, false))
                    return false;
                for(rowinc_px = rowinc, rowtmp_px = rowtmp, rowval_px = rowval;
                    rowinc_px < rowval; rowinc_px++, rowtmp_px++, rowval_px++)
                {
                    *rowinc_px += *rowtmp_px;
                    *rowval_px += *rowinc_px * iye;
                    *rowinc_px *= v_i_val;
                }
            }
        } else
            for(rowinc_px = rowinc, rowval_px = rowval; rowinc_px < rowval;
                rowinc_px++, rowval_px++)
                *rowval_px += *rowinc_px;
        output_row_32(oy, (void*)rowval, ctx);
        iye += v_i_val;
    }
    return true;
}

static int resize_on_load(struct scaler_context *ctx, struct rowset *rset)
{
    const int sw = ctx->src_w;
    const int sh = ctx->src_h;
    const int dw = ctx->dst_w;
    const int dh = ctx->dst_h;
    int ret;

    /* port: dw == 1 / dh == 1 would divide by zero in the linear scaler */
    if (sw > dw || dw == 1)
    {
        ctx->h_area = true;
        uint32_t h_div = (1U << 24) / sw;
        ctx->h_i_val = sw * h_div;
        ctx->h_o_val = dw * h_div;
    } else {
        ctx->h_area = false;
        uint32_t h_div = (1U << 24) / (dw - 1);
        ctx->h_i_val = (sw - 1) * h_div;
        ctx->h_o_val = (dw - 1) * h_div;
    }
    if (sh > dh || dh == 1)
    {
        uint32_t v_div = (1U << 22) / sh;
        ctx->v_i_val = sh * v_div;
        ctx->v_o_val = dh * v_div;
        ret = scale_v_area(rset, ctx);
    }
    else
    {
        uint32_t v_div = (1U << 22) / dh;
        ctx->v_i_val = (sh - 1) * v_div;
        ctx->v_o_val = (dh - 1) * v_div;
        ret = scale_v_linear(rset, ctx);
    }
    if (!ret)
        return 0;
    return 1;
}

/**************** end scaler ********************/

typedef char rbjpeg_state_fits[(sizeof(struct jpeg) + 16 <= RBJPEG_STATE_BYTES)
                               ? 1 : -1];

/* decode_jpeg_mem() adapted: no struct bitmap, fixed output format. */
int rbjpeg_decode(const uint8_t *jpg, uint32_t len, int tw, int th,
                  int stride_bytes, uint8_t *out_rgb565,
                  void *work, uint32_t work_size)
{
    bool resize = false;
    struct rowset rset;
    int status;
    int src_w, src_h;
    struct jpeg *p_jpeg;
    uintptr_t wp, wend;

    if (!jpg || !out_rgb565 || !work || tw < 1 || th < 1 ||
        tw > 32767 || th > 32767 || stride_bytes < tw * 2)
        return RBJPEG_ERR_ARGS;

    wp = ((uintptr_t)work + 7) & ~(uintptr_t)7;
    wend = (uintptr_t)work + work_size;
    if (wp + sizeof(struct jpeg) > wend)
        return RBJPEG_ERR_WORK_SMALL;
    p_jpeg = (struct jpeg *)wp;
    wp += (sizeof(struct jpeg) + 7) & ~(uintptr_t)7;

    memset(p_jpeg, 0, sizeof(struct jpeg));
    p_jpeg->len = len;
    p_jpeg->data = jpg;

    status = process_markers(p_jpeg);
    if (status < 0)
        return status;
    if ((status & (DQT | SOF0)) != (DQT | SOF0))
        return RBJPEG_ERR_NO_FRAME; /* port: was -(status * 16) */
    if (!(status & DHT)) /* if no Huffman table present: */
        default_huff_tbl(p_jpeg); /* use default */
    /* port: reject empty images */
    if (p_jpeg->x_size < 1 || p_jpeg->y_size < 1)
        return RBJPEG_ERR_DIMENSIONS;
    if (fix_headers(p_jpeg)) /* derive Huffman and other lookup-tables */
        return RBJPEG_ERR_SUBSAMPLING;

    /*the dim array in rockbox is limited to 2^15-1 pixels, so we cannot resize
      images larger than this without overflowing */
    if(p_jpeg->x_size > 32767 || p_jpeg->y_size > 32767)
        return RBJPEG_ERR_DIMENSIONS;

    /* stretch to tw x th (no FORMAT_KEEP_ASPECT) */
    resize = true;
    p_jpeg->h_scale[0] = calc_scale(p_jpeg->x_size, tw);
    p_jpeg->v_scale[0] = calc_scale(p_jpeg->y_size, th);
    if ((p_jpeg->x_size << p_jpeg->h_scale[0]) >> 3 == tw &&
        (p_jpeg->y_size << p_jpeg->v_scale[0]) >> 3 == th)
        resize = false;
    p_jpeg->h_scale[1] = p_jpeg->h_scale[0] +
        p_jpeg->frameheader[0].horizontal_sampling - 1;
    p_jpeg->v_scale[1] = p_jpeg->v_scale[0] +
        p_jpeg->frameheader[0].vertical_sampling - 1;
    fix_quant_tables(p_jpeg);
    int decode_w = BIT_N(p_jpeg->h_scale[0]) - 1;
    int decode_h = BIT_N(p_jpeg->v_scale[0]) - 1;
    src_w = (p_jpeg->x_size << p_jpeg->h_scale[0]) >> 3;
    src_h = (p_jpeg->y_size << p_jpeg->v_scale[0]) >> 3;
    /* port: tiny images at 1/8 scale could round to 0 */
    if (src_w < 1) src_w = 1;
    if (src_h < 1) src_h = 1;
    if (p_jpeg->v_scale[0] > 2)
        p_jpeg->zero_need[0] = (decode_w << 3) + decode_h;
    else
        p_jpeg->zero_need[0] = (decode_h << 3) + decode_w;
    p_jpeg->k_need[0] = zig[(decode_h << 3) + decode_w];
    decode_w = BIT_N(MIN(p_jpeg->h_scale[1],3)) - 1;
    decode_h = BIT_N(MIN(p_jpeg->v_scale[1],3)) - 1;
    if (p_jpeg->v_scale[1] > 2)
        p_jpeg->zero_need[1] = (decode_w << 3) + decode_h;
    else
        p_jpeg->zero_need[1] = (decode_h << 3) + decode_w;
    p_jpeg->k_need[1] = zig[(decode_h << 3) + decode_w];

    uint32_t decode_buf_size = ((uint32_t)p_jpeg->x_mbl << p_jpeg->h_scale[1])
        << p_jpeg->v_scale[1];
    decode_buf_size *= JPEG_PIX_SZ;
    uint32_t scaler_size = resize ? sizeof(struct uint32_argb) * 3 * (uint32_t)tw
                                  : 0;
    if (wend < wp || wend - wp < decode_buf_size + 8 + scaler_size)
        return RBJPEG_ERR_WORK_SMALL;

    if (fix_huff_tables(p_jpeg))
        return RBJPEG_ERR_HUFF_TABLE;

    p_jpeg->img_buf = (jpeg_pix_t *)wp;
    wp += (decode_buf_size + 7) & ~7u;
    memset(p_jpeg->img_buf, 0, decode_buf_size);
    p_jpeg->mcu_row = 0;
    p_jpeg->restart = p_jpeg->restart_interval;
    rset.rowstart = 0;
    rset.rowstop = th;
    rset.rowstep = 1;
    p_jpeg->resize = resize;

    struct scaler_context ctx;
    ctx.src_w = src_w;
    ctx.src_h = src_h;
    ctx.dst_w = tw;
    ctx.dst_h = th;
    ctx.buf = (unsigned char *)wp;
    ctx.args = p_jpeg;
    ctx.fromyuv = p_jpeg->blocks > 1;
    ctx.out = out_rgb565;
    ctx.stride = stride_bytes;
    ctx.h_area = true;
    ctx.h_i_val = ctx.h_o_val = ctx.v_i_val = ctx.v_o_val = 0;
    if (resize)
    {
        if (!resize_on_load(&ctx, &rset))
            return RBJPEG_ERR_EOF;
    } else {
        int row;
        struct img_part *part;
        for (row = 0; row < th; row++)
        {
            part = store_row_jpeg(p_jpeg);
            struct uint8_rgb *qp = part->buf;
            struct uint8_rgb *end = qp + tw;
            uint8_t *dest = out_rgb565 + (uint32_t)stride_bytes * row;
            unsigned r, g, b;
            if (p_jpeg->blocks > 1)
            {
                for (; qp < end; qp++, dest += 2)
                {
                    yuv_to_rgb(qp->blue, qp->green, qp->red, &r, &g, &b);
                    put565(dest, r, g, b);
                }
            } else {
                for (; qp < end; qp++, dest += 2)
                    put565(dest, qp->red, qp->green, qp->blue);
            }
            pad_row(out_rgb565 + (uint32_t)stride_bytes * row, tw * 2,
                    stride_bytes);
        }
    }
    return 0;
}
