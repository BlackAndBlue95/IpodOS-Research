/* RGB565 with a 16x16 blue-noise threshold: when mkpatch --dither sets dither565, generated
 * gradients (Now Playing background, decoded covers, re-themed bitmaps) quantise without banding. */
#include <stdint.h>

extern uint32_t dither565;
extern const uint8_t bluenoise[256];

/* threshold 0..255 for pixel (x, y) */
static inline uint32_t dith_t(uint32_t x, uint32_t y) { return bluenoise[(y & 15) << 4 | (x & 15)]; }

/* 8.8 fixed-point channels (0..0xff00), scaled to 5/6/5 bits (0xff00 -> 31/63) and dithered by t */
static inline uint16_t dith565_q8(uint32_t r, uint32_t g, uint32_t b, uint32_t t)
{
    return (uint16_t)(((r * 7968 + (t << 16)) >> 24) << 11 | ((g * 16192 + (t << 16)) >> 24) << 5 | (b * 7968 + (t << 16)) >> 24);
}

/* 8-bit channels */
static inline uint16_t dith565(uint32_t r, uint32_t g, uint32_t b, uint32_t t) { return dith565_q8(r << 8, g << 8, b << 8, t); }
