"""The dark-theme colour rules, in integer math identical to runtime/theme_rules.c (checked by
rules_parity.py). Keep the two in step.
  hue: 0..1535 (6 x 256), sat/val 0..255; Apple's list blue sits at hue ~904 (212 deg)."""

DARK_LO, DARK_HI = 0x1C, 0xFF          # dark_gray output range
BG_LO, BG_HI = 0x1C, 0x30              # background gradient range
GRAY_IMAGE_CHROMA = 40                 # an image with no pixel above this is grey throughout
PIXEL_CHROMA = 24                      # a pixel at or below this is grey
BLUE_HUE, BLUE_TOL = 904, 171          # +-40 degrees


def lum(r, g, b): return (77 * r + 150 * g + 29 * b) >> 8
def chroma(r, g, b): return max(r, g, b) - min(r, g, b)


def hue(r, g, b):
    c = chroma(r, g, b)
    if c == 0: return 0
    mx = max(r, g, b)
    if mx == r: h = ((g - b) * 256) // c
    elif mx == g: h = 512 + ((b - r) * 256) // c
    else: h = 1024 + ((r - g) * 256) // c
    return h % 1536


def hsv_to_rgb(h, s, v):
    """h 0..1535, s and v 0..255"""
    h %= 1536
    sector, f = h >> 8, h & 255
    p = v * (255 - s) // 255
    q = v * (255 - s * f // 255) // 255
    t = v * (255 - s * (255 - f) // 255) // 255
    return [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][sector]


def dark_gray(L): return DARK_LO + (255 - L) * (DARK_HI - DARK_LO) // 255
def gray_px(L): y = dark_gray(L); return (y, y, y + 2 if y < 254 else y)


def bg_px(L, lmin, lmax):
    L = min(max(L, lmin), lmax)
    y = BG_LO + (L - lmin) * (BG_HI - BG_LO) // max(lmax - lmin, 1)
    return (y, y, y + 2)


def is_apple_blue(r, g, b):
    if chroma(r, g, b) <= PIXEL_CHROMA: return False
    d = abs(hue(r, g, b) - BLUE_HUE)
    return min(d, 1536 - d) <= BLUE_TOL


def accent_px(r, g, b, ar, ag, ab):
    """an Apple-blue pixel recoloured to the accent: the accent's hue, the pixel's brightness,
    a saturation between the pixel's and the accent's"""
    v = max(r, g, b)
    c = chroma(r, g, b)
    s = c * 255 // v if v else 0
    av = max(ar, ag, ab)
    sa = chroma(ar, ag, ab) * 255 // av if av else 0
    s2 = s * (128 + sa // 2) // 255
    return hsv_to_rgb(hue(ar, ag, ab), s2, v)


def chrome_px(r, g, b, accent, gray_image):
    """one opaque pixel of a chrome image or a COLR entry. accent = (ar, ag, ab) or None."""
    if gray_image or chroma(r, g, b) <= PIXEL_CHROMA:
        return gray_px(lum(r, g, b))
    if accent and is_apple_blue(r, g, b):
        return accent_px(r, g, b, *accent)
    return (r, g, b)
