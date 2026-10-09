/*
 * rbjpeg - freestanding port of the Rockbox JPEG loader (jpeg_load.c) and
 * area/linear scaler (resize.c).  GPL v2 or later, like Rockbox.
 *
 * int rbjpeg_decode(jpg, len, tw, th, stride_bytes, out_rgb565, work, work_size)
 *
 *   jpg/len       whole baseline JPEG file in memory (8-bit, 1 or 3 components,
 *                 luma sampling 1x1, 2x1, 1x2 or 2x2, chroma 1x1).
 *   tw, th        output size (1..32767). The image is stretched to fill it.
 *   stride_bytes  output row pitch in bytes, >= tw*2. Bytes past tw*2 in each
 *                 row are zeroed.
 *   out_rgb565    output, RGB565 little-endian (r<<11 | g<<5 | b), any alignment.
 *   work          scratch memory (any alignment; it is aligned internally).
 *   work_size     bytes of scratch. See RBJPEG_WORK_SIZE below.
 *
 * Returns 0 on success, a negative error code otherwise (progressive and other
 * non-baseline JPEGs return RBJPEG_ERR_UNSUPPORTED_SOF = -4).
 * No global or static mutable state is used; the function is reentrant.
 */
#ifndef RBJPEG_H
#define RBJPEG_H
#include <stdint.h>

int rbjpeg_decode(const uint8_t *jpg, uint32_t len, int tw, int th,
                  int stride_bytes, uint8_t *out_rgb565,
                  void *work, uint32_t work_size);

/* error codes (Rockbox's own codes are kept for -1..-11) */
#define RBJPEG_ERR_EOF            (-1)   /* truncated headers / bad marker */
#define RBJPEG_ERR_SOF_COMPONENTS (-2)
#define RBJPEG_ERR_SUBSAMPLING    (-3)
#define RBJPEG_ERR_UNSUPPORTED_SOF (-4)  /* progressive, lossless, arithmetic, 12-bit */
#define RBJPEG_ERR_HUFF_INDEX     (-5)
#define RBJPEG_ERR_ARITH          (-6)
#define RBJPEG_ERR_SOS            (-7)
#define RBJPEG_ERR_QUANT          (-8)
#define RBJPEG_ERR_MARKER         (-9)
#define RBJPEG_ERR_HUFF_LEN       (-10)
#define RBJPEG_ERR_HUFF_LEN_DC    (-11)
#define RBJPEG_ERR_ARGS           (-20)  /* bad tw/th/stride/pointers */
#define RBJPEG_ERR_NO_FRAME       (-21)  /* no SOF0 / DQT before SOS */
#define RBJPEG_ERR_DIMENSIONS     (-22)  /* zero or > 32767 image size */
#define RBJPEG_ERR_HUFF_TABLE     (-23)  /* malformed Huffman table */
#define RBJPEG_ERR_WORK_SMALL     (-24)  /* work_size too small */

/*
 * Minimum work_size.  The exact need is
 *     align slack (8) + sizeof(struct jpeg) (<= RBJPEG_STATE_BYTES)
 *   + decode buffer: 3 * (x_mbl << chroma_hscale) << chroma_vscale bytes
 *     (one row of MCUs at the reduced IDCT scale, 3 bytes/pixel)
 *   + scaler rows:   36 * tw bytes (3 rows of 3 x uint32), only when the
 *     reduced-scale size differs from tw x th.
 * The decoded width is at most max(ceil(W/16)*2, 2*tw + 16) pixels and the
 * MCU row is at most 16 pixels high, so for source width <= max_src_w:
 */
#define RBJPEG_STATE_BYTES 8192u
#define RBJPEG_MAX_(a, b) ((a) > (b) ? (a) : (b))
#define RBJPEG_WORK_SIZE_FOR(tw, max_src_w) \
    (RBJPEG_STATE_BYTES + 16u \
     + 48u * (uint32_t)RBJPEG_MAX_((((uint32_t)(max_src_w) + 15u) / 16u) * 2u, \
                                   2u * (uint32_t)(tw) + 16u) \
     + 36u * (uint32_t)(tw))
/* sources up to 4000 pixels wide */
#define RBJPEG_WORK_SIZE(tw) RBJPEG_WORK_SIZE_FOR(tw, 4000)

#endif
