#!/usr/bin/env python3
"""Previews the theme rules before they go on the iPod: decodes the chrome images and colours
from the OS image, applies the same recolouring rules region E will use (theme.c must match
this file), and writes PNG sheets.
  theme_preview.py OSOS.dec OUTDIR"""
import colorsys, os, sys
MODERN, flattened = False, []
from uires import Package, Canvas, png
import theme_rules as T
from theme_rules import lum, chroma, dark_gray, is_apple_blue

# iOS dark palette and accents
DARK_BG, DARK_ROW, DARK_SEP, DARK_TEXT, DARK_TEXT2 = 0x1C1C1E, 0x2C2C2E, 0x38383A, 0xFFFFFF, 0x8E8E93
ACCENTS = [('Blue', 0x0A84FF), ('Purple', 0xBF5AF2), ('Pink', 0xFF375F), ('Red', 0xFF453A),
           ('Orange', 0xFF9F0A), ('Yellow', 0xFFD60A), ('Green', 0x30D158), ('Graphite', 0x8E8E93)]


def rgb_of(v): return ((v >> 16) & 255, (v >> 8) & 255, v & 255)
def val_of(r, g, b): return r << 16 | g << 8 | b


def recolor(r, g, b, accent):
    """one colour under the dark theme with the given accent (0 = keep Apple's blue);
    the same rule as theme_rules.c tr_chrome_px"""
    return T.chrome_px(r, g, b, rgb_of(accent) if accent else None, False)


def image_stats(rows, w, h):
    """(max chroma, min luminance, max luminance) over the opaque pixels"""
    cmax, lmin, lmax = 0, 255, 0
    for y in range(h):
        for x in range(w):
            R, G, B, A = rows[y][4 * x:4 * x + 4]
            if A < 128: continue
            cmax = max(cmax, max(R, G, B) - min(R, G, B))
            L = lum(R, G, B)
            lmin, lmax = min(lmin, L), max(lmax, L)
    return cmax, lmin, lmax


def recolor_bg(r, g, b, lmin, lmax):
    """a decorative background: squeeze its luminance range into the dark background range,
    keeping the direction of its gradient (theme_rules.c tr_bg_px)"""
    return T.bg_px(lum(r, g, b), lmin, lmax)


def recolor_rows(rows, w, h, accent, cls='chrome'):
    """cls 'chrome': greys invert (drawn on white, so they end up light on dark), Apple blue
    becomes the accent; 'bg': dark gradient. Images that are grey overall (max chroma <= 40)
    are treated as grey throughout, so a blue-grey ramp doesn't split into two rules."""
    cmax, lmin, lmax = image_stats(rows, w, h)
    gray_image = cmax <= T.GRAY_IMAGE_CHROMA
    acc = rgb_of(accent) if accent else None
    out = []
    for y in range(h):
        r = bytearray()
        for x in range(w):
            R, G, B, A = rows[y][4 * x:4 * x + 4]
            if A:
                if cls == 'bg':
                    R, G, B = recolor_bg(R, G, B, lmin, lmax)
                elif cls == 'accent' and chroma(R, G, B) > T.PIXEL_CHROMA:
                    R, G, B = T.accent_px(R, G, B, *(acc or rgb_of(ACCENTS[0][1])))
                elif cls == 'accent':
                    R, G, B = T.chrome_px(R, G, B, acc, False)
                else:
                    R, G, B = T.chrome_px(R, G, B, acc, gray_image)
            r += bytes([R, G, B, A])
        out.append(bytes(r))
    return out


def flatten_rows(rows, w, h, name, page_rgb):
    """Modern style (theme.c flatten_bar): a vertical-gradient bar becomes its mean colour; the status
    bar and main menu backgrounds take the page colour. Returns (rows, flattened?)"""
    if h < 3: return rows, False
    means, spread, n, sr, sg, sb = [], 0, 0, 0, 0, 0
    for y in range(h):
        Ls = []
        for x in range(w):
            R, G, B, A = rows[y][4 * x:4 * x + 4]
            if A < 128: continue
            Ls.append(lum(R, G, B)); sr += R; sg += G; sb += B; n += 1
        if Ls: means.append(sum(Ls) // len(Ls)); spread += max(Ls) - min(Ls)
    if not means or not n: return rows, False
    page = name in ('StatusBarWhite_Background_Image', 'Background_LightBlue_Image')
    if not page and (spread // len(means) > 12 or max(means) - min(means) < 16): return rows, False
    flat = page_rgb if page else (sr // n, sg // n, sb // n)
    out = []
    for row in rows:
        r = bytearray()
        for x in range(w):
            A = row[4 * x + 3]
            r += bytes(list(flat) + [A]) if A else row[4 * x:4 * x + 4]
        out.append(bytes(r))
    return out, True


# image classes by name prefix (alpha-only masks are never recoloured: the OS tints them)
IMG_BG = ('Background_LightBlue', 'GeniusMixes_Background', 'DateTimePicker_Bar', 'DateTimePicker_Well')
# accent icons: every coloured pixel takes the accent hue in both themes (the status bar battery is Apple green)
IMG_ACCENT = ('StatusBarWhite_Battery_Image',)


def image_class(name):
    return 'bg' if name.startswith(IMG_BG) else 'accent' if name.startswith(IMG_ACCENT) else 'chrome'


# which COLR entries the dark theme changes (already-dark screens are left alone)
COLR_KEEP_PREFIX = ('NowPlaying_Text', 'NowPlaying_StatusBar', 'NowPlaying_Background', 'Photos_', 'Games_',
                    'Genius_', 'CoverFlow_Backside', 'DiskMode_', 'ChargingMode_', 'Radio_', 'RemoteUI_',
                    'Search_Keyboard', 'DemoMode_', 'WorldClock_', 'Alarms_Status', 'PreviewArea_', 'QuickScroll_',
                    'Global_SplitView', 'DateTimePicker_Color', 'Settings_DateTime_Text', 'Calendar_Hilite')
# images the dark theme recolours (by name prefix)
BMAP_PREFIX = ('StatusBarWhite_', 'System_Scrollbar', 'System_ActiveButton', 'System_NormalButton',
               'Background_LightBlue', 'OptionBar_White_', 'Settings_MainMenu', 'DateTimePicker_',
               'Media_Genius_Selected', 'GeniusMixes_Background',
               'NowPlaying_ProgressBar_', 'NowPlaying_ProgressFill_', 'NowPlaying_White_')


def selection_bar(w, h, top, bottom):
    rows = []
    for y in range(h):
        c = [top[i] + (bottom[i] - top[i]) * y // max(h - 1, 1) for i in range(3)]
        rows.append(bytes(c + [255]) * w)
    return rows


def main():
    pkg = Package(sys.argv[1])
    out = sys.argv[2]
    global MODERN, flattened
    MODERN = len(sys.argv) > 3 and sys.argv[3] == 'modern'
    flattened = []
    os.makedirs(out, exist_ok=True)

    # 1. colour sheet: stock vs dark for each changed COLR entry
    changed = [(pid, n, v) for pid, n, v in pkg.colr() if not n.startswith(COLR_KEEP_PREFIX)]
    sheet = Canvas(2 * 160 + 30, 14 * len(changed) + 10)
    print('COLR overrides (stock -> dark):')
    for i, (pid, n, v) in enumerate(changed):
        d = val_of(*recolor(*rgb_of(v), ACCENTS[0][1]))
        print('  %08x %-44s #%06X -> #%06X' % (pid, n, v, d))
        sheet.fill(10, 10 + 14 * i, 150, 12, rgb_of(v))
        sheet.fill(170, 10 + 14 * i, 150, 12, rgb_of(d))
    sheet.save(os.path.join(out, 'colors.png'))

    # 2. chrome images: stock | dark, and the blue ones in every accent
    names = sorted(n for n in pkg.ids if n.startswith(BMAP_PREFIX) and pkg.ids[n] in {p for p, o, s in pkg.index('BMap')})
    sheets = []
    for n in names:
        fmt, w, h, stride, pal, rows = pkg.bmap(pkg.ids[n])
        if fmt in (4, 8):
            continue                                   # alpha mask, tinted by the OS at draw time
        cls = image_class(n)
        sc = 2 if w <= 160 else 1
        cmax = image_stats(rows, w, h)[0]
        blue = cls == 'accent' or cls == 'chrome' and cmax > 40 and any(is_apple_blue(*rows[y][4 * x:4 * x + 3]) for y in range(h) for x in range(w))
        variants = [('stock', rows)] + [('dark', recolor_rows(rows, w, h, ACCENTS[0][1], cls))]
        if blue:
            variants += [(an, recolor_rows(rows, w, h, av, cls)) for an, av in ACCENTS[1:]]
        if MODERN and cls != 'accent':
            flat_any = False
            nv = []
            for vn, vr in variants:
                if vn == 'stock': nv.append((vn, vr)); continue
                fr, fl = flatten_rows(vr, w, h, n, rgb_of(DARK_BG)); flat_any |= fl; nv.append((vn, fr))
            variants = nv
            if flat_any: flattened.append(n)
        cw = max(w * sc for _ in variants) + 10
        c = Canvas(cw * len(variants) + 10, h * sc + 20)
        for k, (vn, vr) in enumerate(variants):
            c.paste(vr, w, h, 10 + k * cw, 10, sc)
        p = os.path.join(out, 'bmap_%s.png' % n)
        c.save(p)
        sheets.append((n, fmt, w, h, blue, [v[0] for v in variants]))
    for n, fmt, w, h, blue, vs in sheets:
        print('  %-44s fmt %04x %3dx%-3d %s' % (n, fmt, w, h, ', '.join(vs)))
    if MODERN: print('  flattened in Modern style: ' + ', '.join(flattened))

    # 3. the list selection bar: Apple's gradient and the accent gradients
    c = Canvas(340, 30 * (len(ACCENTS) + 1) + 10)
    c.paste(selection_bar(320, 20, (0x58, 0x98, 0xE8), (0x28, 0x64, 0xD0)), 320, 20, 10, 10)
    for i, (an, av) in enumerate(ACCENTS):
        h_, s, v = colorsys.rgb_to_hsv(*[x / 255 for x in rgb_of(av)])
        top = tuple(int(x * 255 + 0.5) for x in colorsys.hsv_to_rgb(h_, s, min(1.0, v * 1.08)))
        bot = tuple(int(x * 255 + 0.5) for x in colorsys.hsv_to_rgb(h_, s, v * 0.78))
        c.paste(selection_bar(320, 20, top, bot), 320, 20, 10, 40 + 30 * i)
    c.save(os.path.join(out, 'selection.png'))

    # 4. a mock list screen: status bar + rows + selection, stock vs dark
    def mock(dark, accent):
        c = Canvas(320, 240)
        bg = rgb_of(DARK_BG) if dark else (255, 255, 255)
        c.fill(0, 0, 320, 240, bg)
        fmt, w, h, stride, pal, rows = pkg.bmap(pkg.ids['StatusBarWhite_Background_Image'])
        c.paste(recolor_rows(rows, w, h, accent) if dark else rows, w, h, 0, 0)
        for i in range(10):
            y = 20 + 22 * i
            if i == 2:
                top, bot = ((0x58, 0x98, 0xE8), (0x28, 0x64, 0xD0))
                if dark and accent:
                    h_, s, v = colorsys.rgb_to_hsv(*[x / 255 for x in rgb_of(accent)])
                    top = tuple(int(x * 255 + 0.5) for x in colorsys.hsv_to_rgb(h_, s, min(1.0, v * 1.08)))
                    bot = tuple(int(x * 255 + 0.5) for x in colorsys.hsv_to_rgb(h_, s, v * 0.78))
                c.paste(selection_bar(320, 22, top, bot), 320, 22, 0, y)
                c.fill(10, y + 8, 90 + 10 * i, 6, (255, 255, 255))
            else:
                c.fill(10, y + 8, 90 + 10 * i, 6, rgb_of(DARK_TEXT) if dark else (0, 0, 0))
                c.fill(0, y + 21, 320, 1, rgb_of(DARK_SEP) if dark else (0xD8, 0xD8, 0xD8))
        fmt, w, h, stride, pal, rows = pkg.bmap(pkg.ids['System_ScrollbarTop_Image'])
        c.paste(recolor_rows(rows, w, h, accent) if dark else rows, w, h, 312, 22)
        return c
    m = Canvas(3 * 330 + 10, 250)
    for k, (dark, acc) in enumerate([(False, 0), (True, ACCENTS[0][1]), (True, ACCENTS[1][1])]):
        mm = mock(dark, acc)
        m.paste([bytes(r) for r in mm.px], 320, 240, 10 + 330 * k, 5)
    m.save(os.path.join(out, 'mock_list.png'))
    print('wrote', out)


if __name__ == '__main__':
    main()
