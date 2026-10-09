#ifndef THEME_RULES_H
#define THEME_RULES_H
#include <stdint.h>
#define TR_GRAY_IMAGE_CHROMA 40   /* an image with no pixel above this is grey throughout */
int tr_lum(int r, int g, int b);
int tr_chroma(int r, int g, int b);
int tr_hue(int r, int g, int b);
void tr_hsv_to_rgb(int h, int s, int v, uint8_t out[3]);
int tr_dark_gray(int L);
int tr_light_of_dark(int y);
int tr_dark_lo(void);
void tr_gray_px(int L, uint8_t out[3]);
void tr_bg_px(int L, int lmin, int lmax, uint8_t out[3]);
int tr_is_apple_blue(int r, int g, int b);
void tr_accent_px(int r, int g, int b, const uint8_t acc[3], uint8_t out[3]);
void tr_chrome_px(int r, int g, int b, const uint8_t *acc, int gray_image, uint8_t out[3]);
#endif
