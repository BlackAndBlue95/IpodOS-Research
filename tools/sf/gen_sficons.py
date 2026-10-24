#!/usr/bin/env python3
"""Modern-style icons: replaces the OS's icon bitmaps with Apple SF Symbols rendered on this Mac.

    gen_sficons.py OSOS.dec OUTDIR

Each replacement is drawn into the original's slot (its ink box, so layouts are unchanged) and
encoded in the original's own format and byte size, so the theme can copy it over the resource in
place. Writes OUTDIR/rsrc/Resources/Icons/{Classic,Modern}.bin for Apple's resource volume and their
table runtime/sficons_data.h (icon_sets.py), and OUTDIR/before_after.png.
The rendered symbols are Apple's: OUTDIR and the header are gitignored and must not be committed."""
import os, re, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'runtime', 'host'))
from uires import Package, Canvas, DATA, BASE

pkg = Package(sys.argv[1])
OUT = sys.argv[2]
RENDER = os.path.join(OUT, 'render')
bm_index = {pid: (off, size) for pid, off, size in pkg.index('BMap')}

# ---- what replaces what ----
# style: g = glyph in the original's ink colour, t = light placeholder tile, d = dark control tile,
# bat = battery (level/levels), pie = timer pie (fraction), txt = system font text, rot = rotated glyph
G, T, D = 'g', 't', 'd'
MAP = [
    # status bar
    (r'StatusBar(White|Black)_Lock_Image', G, 'lock.fill', 'semibold'),
    (r'StatusBarWhite_PlayStatus_Image', G, 'play.fill', 'semibold'),
    (r'StatusBar(White|Black)_Battery_Image_Charge', G, 'bolt.fill', 'semibold'),
    (r'StatusBar(White|Black)_Battery_Image_Plug', G, 'powerplug.fill', 'semibold'),
    # charging screen: the bolt and plug over the battery (stored white, see build)
    (r'ChargingMode_Image_ToggleCharging', G, 'bolt.fill', 'regular', (255, 255, 255)),
    (r'ChargingMode_Image_ToggleCharged', G, 'powerplug.fill', 'regular', (255, 255, 255)),
    # Now Playing and transport
    (r'NowPlaying_(Black|White)_FF_Image|TVOut_FF_Image', None, 'forward.fill', 'semibold'),
    (r'NowPlaying_(Black|White)_Rew_Image|TVOut_Rew_Image', None, 'backward.fill', 'semibold'),
    (r'NowPlaying_(Black|White)_Pause_Image|Stopwatch_SmallPause_Image', G, 'pause.fill', 'semibold'),
    (r'NowPlaying_(Black|White)_Play_Image|Stopwatch_SmallPlay_Image|NowPlaying_Idle_Play_Image', G, 'play.fill', 'semibold'),
    (r'NowPlaying_Black_Record_Image', G, 'record.circle.fill', 'semibold'),
    (r'NowPlaying_(Black|White)_Volume_Locked_Image', G, 'speaker.badge.exclamationmark.fill', 'semibold'),
    (r'NowPlaying_White_Volume_High_Image|System_Overlay_VolumeRight_Image|TVOut_Volume_Full_Image', G, 'speaker.wave.3.fill', 'semibold'),
    (r'NowPlaying_White_Volume_Low_Image|System_Overlay_VolumeLeft_Image', G, 'speaker.fill', 'semibold'),
    (r'TVOut_Volume_Off_Image', G, 'speaker.slash.fill', 'semibold'),
    (r'NowPlaying_(Blue_Dot|Dot)_Image|Page_White_(Active|Inactive)_Image|Podcasts_new_item(_white)?_Image', G, 'circle.fill', 'regular'),
    (r'NowPlaying_(Blue_Star|Large_Blue_Star|Small_Grey_Star|Small_White_Star|Star)_Image|Radio_Preset_Image', G, 'star.fill', 'semibold'),
    (r'NowPlaying_(Classic_|Large_)?Shuffle_Image', G, 'shuffle', 'semibold'),
    (r'NowPlaying_(Classic_)?Repeat_Image', G, 'repeat', 'semibold'),
    (r'NowPlaying_(Classic_)?Repeat_Once_Image', G, 'repeat.1', 'semibold'),
    (r'NowPlaying_Genius_Icon|Genius_Slider_Icon_Image', G, 'atom', 'semibold'),
    # Genius and playlist rows: accent images (ARGB or palette, same size); the highlighted ones are white
    (r'Media_Genius_Large_Image', G, 'atom', 'regular', 'accs'),
    (r'Media_Genius_Normal_Image|Playlist_Genius_Icon', G, 'atom', 'semibold', 'accs'),
    (r'Media_Genius_Selected_Image|Playlist_Genius_Icon_Hi', G, 'atom', 'semibold', (255, 255, 255)),
    (r'Playlist_Saved_Genius_Icon', G, 'sparkles', 'semibold', 'accs'),
    (r'Media_Genius_Refresh(_Hi)?_Image', G, 'arrow.clockwise', 'semibold'),
    (r'NowPlaying_Paused_Image|TVOut_Pause_Image|VoiceMemosImage_Pause', None, 'pause.fill', 'semibold'),
    (r'NowPlaying_Idle_Lock_Image', G, 'lock.fill', 'semibold'),
    (r'NowPlaying_Idle_Radio_Image|Radio_Signal_Indicator_(Light|Dark)_Image', G, 'dot.radiowaves.left.and.right', 'semibold'),
    # lists
    (r'Media_NowPlayingIcon_(Black|White)_Image', G, 'speaker.wave.2.fill', 'semibold'),
    (r'Media_NowPlayingIcon_Playlist_(Black|White)_Image', G, 'music.note.list', 'semibold'),
    (r'Playlist_Regular_Icon', G, 'music.note.list', 'semibold', 'accs'),
    (r'Media_VideoStatusIcon(_White)?_Image|Podcasts_(new_item_)?video(_white)?(_solo)?_Image', G, 'tv', 'semibold'),
    (r'Playlist_Folder_Icon', G, 'folder.fill', 'regular', 'accs'),
    (r'Playlist_Smart_Icon', G, 'music.note.list+gearshape.fill', 'semibold', 'accs'),   # base + badge
    (r'Settings_MainMenu_Checkmark(Black|White)_Image', G, 'checkmark', 'bold'),
    (r'System_Submenu_Image', G, 'chevron.right', 'semibold'),
    (r'System_Overlay_BrightnessLess_Image', G, 'sun.min.fill', 'semibold'),
    (r'System_Overlay_BrightnessMore_Image', G, 'sun.max.fill', 'semibold'),
    (r'Settings_VolumeLimitArrow_Image', G, 'arrowtriangle.up.fill', 'semibold'),
    (r'Settings_Legal_Image', G, 'doc.text', 'regular'),
    (r'Settings_DateTime_TimeZonePin_Image', G, 'mappin', 'semibold', (255, 59, 48)),
    (r'Calendar_(Small_)?Alarm', G, 'bell.fill', 'semibold'),
    (r'Calendar_Flag', G, 'flag.fill', 'semibold'),
    # main menu and Settings previews (white masks, reflections dropped)
    (r'MainMenu_Backlight_Image|Settings_Backlight_Image', G, 'sun.max.fill', 'regular'),
    (r'Settings_Brightness_Image', G, 'sun.max', 'regular'),
    (r'MainMenu_Shuffle_Image|Settings_ShuffleSongs_Image', G, 'shuffle', 'regular'),
    (r'MainMenu_Sleep_Image', G, 'moon.stars.fill', 'regular'),
    (r'Settings_Audiobooks_Image', G, 'book.fill', 'regular'),
    (r'Settings_ClickerOff_Image', G, 'speaker.slash.fill', 'regular'),
    (r'Settings_ClickerOn_Image', G, 'speaker.wave.2.fill', 'regular'),
    (r'Settings_MainMenu_Image', G, 'list.bullet', 'regular'),
    (r'Settings_Radio_Image', G, 'radio', 'regular'),
    (r'Settings_RepeatAll_Image', G, 'repeat', 'regular'),
    (r'Settings_RepeatOff_Image', G, 'arrow.right', 'regular'),
    (r'Settings_RepeatOne_Image', G, 'repeat.1', 'regular'),
    (r'Settings_Reset_Image', G, 'arrow.counterclockwise', 'regular'),
    (r'Settings_ShuffleOff_Image', G, 'list.number', 'regular'),
    (r'Settings_SortBy_Image', G, 'arrow.up.arrow.down', 'regular'),
    (r'Settings_SoundCheckOff_Image', G, 'speaker.fill', 'regular'),
    (r'Settings_SoundCheckOn_Image', G, 'waveform', 'regular'),
    (r'Settings_VolumeLimit_Image', G, 'slider.horizontal.3', 'regular'),
    # Settings > EQ previews: 4-bit alpha bars, redrawn flat at the stock heights; Language: the greetings
    (r'Settings_EQ_\w+_Image', 'eq', None, None),
    (r'Settings_Language_Image', 'lang', None, None),
    (r'Settings_Backlight_off_Image', G, 'circle', 'regular'),
    (r'Settings_Backlight_on_Image', G, 'circle.fill', 'regular'),
    (r'Settings_AppleLogo(Preview)?_Image', G, 'apple.logo', 'regular'),
    (r'Settings_About_Logo_Image', G, 'apple.logo', 'regular', 'acc'),
    (r'Speakers_Icon_Image', G, 'hifispeaker.fill', 'regular', 'acc'),
    (r'LockScreen_Padlock_Image', G, 'lock.fill', 'regular', 'acc'),
    (r'VoiceMemosImage_Microphone', G, 'mic.fill', 'regular', 'acc'),
    (r'Notes_Preview_Image', G, 'note.text', 'regular', 'acc'),
    (r'DiskModeImage_SyncIcon', G, 'circle.fill', 'regular', 'acc'),
    # empty-library previews (white masks)
    (r'No_Content_Audiobooks_Preview_Image', G, 'book.fill', 'regular'),
    (r'No_Content_Movies_Preview_Image', G, 'film', 'regular'),
    (r'No_Content_Music_Preview_Image', G, 'music.note', 'regular'),
    (r'No_Content_Music_Video_Preview_Image', G, 'music.note.tv', 'regular'),
    (r'No_Content_Photo_Preview_Image', G, 'photo.on.rectangle', 'regular'),
    (r'No_Content_Playlist_Preview_Image', G, 'music.note.list', 'regular'),
    (r'No_Content_Podcast_Preview_Image', G, 'antenna.radiowaves.left.and.right', 'regular'),
    (r'No_Content_TVShow_Preview_Image', G, 'tv', 'regular'),
    (r'No_Content_iTunesU_Preview_Image', G, 'graduationcap.fill', 'regular'),
    (r'No_Content_Contacts_Preview_Image', G, 'person.2.fill', 'regular'),
    (r'No_Content_Notes_Preview_Image', G, 'note.text', 'regular'),
    # placeholder tiles (no artwork)
    # Extras previews: accent app tiles with white symbols, a flat calendar page and clock faces
    (r'Alarms_Clock_Static_Image', 'A', 'alarm.fill', 'regular'),
    (r'Stopwatch_Clock_Image', 'A', 'stopwatch.fill', 'regular'),
    (r'Extras_Games_Preview_Image', 'A', 'gamecontroller.fill', 'regular'),
    (r'AddressViewer_Preview_Image', 'A', 'person.crop.circle.fill', 'regular'),
    (r'Settings_DateTime_Calendar_Image', 'cal', None, None),
    (r'Clock_(Large|Small)_Image', 'face', None, None, (255, 255, 255)),
    (r'Clock_Small_Night_Image', 'face', None, None, (28, 28, 30)),
    # the Settings previews' glossy black base: an iOS Settings style rounded square in the accent
    (r'Settings_BasePanel(Wide)?_Image', 'P', None, None),
    (r'Media_(All)?AlbumsProxy_Image|No_Content_Albums_Small_Image', T, 'square.stack.fill', 'regular'),
    (r'Media_ArtistsProxy_Image', T, 'music.mic', 'regular'),
    (r'Media_AudioBooksProxy_Image|No_Content_AudioBooks_Small_Image', T, 'book.fill', 'regular'),
    (r'Media_AudioProxy_Image', T, 'music.note', 'regular'),
    (r'Media_DocumentsProxy_Image', T, 'doc.fill', 'regular'),
    (r'Media_MusicVideosProxy_Image|No_Content_MusicVideos_Small_Image', T, 'music.note.tv', 'regular'),
    (r'Media_PhotosProxy_Image', T, 'photo', 'regular'),
    (r'Media_PlaylistsProxy_Image', T, 'music.note.list', 'regular'),
    (r'Media_PodcastsProxy_Image|No_Content_Podcasts_Small_Image', T, 'antenna.radiowaves.left.and.right', 'regular'),
    (r'Media_SearchProxy_Image', T, 'magnifyingglass', 'regular'),
    (r'Media_TVShowsProxy_Image|No_Content_TVShows_Small_Image', T, 'tv', 'regular'),
    (r'Media_VideosProxy_Image|No_Content_Videos_Small_Image', T, 'film', 'regular'),
    (r'No_Content_iTunesU_Small_Image', T, 'graduationcap.fill', 'regular'),
    (r'Games_Proxy_Image', T, 'gamecontroller.fill', 'regular'),
    (r'AddressViewer_NoContactPict_Image', T, 'person.fill', 'regular'),
    (r'AddressViewer_NoGroupPict_Image', T, 'person.2.fill', 'regular'),
]
SPECIAL = {}
for lv in range(23):
    for c in ('White', 'Black'):
        SPECIAL['StatusBar%s_Battery_Image_%d' % (c, lv)] = ('bat', lv / 22)
for lv in range(8):
    SPECIAL['NowPlaying_Idle_Battery_%d_Image' % lv] = ('bat', lv / 7)
for d in range(10):
    SPECIAL['NowPlaying_Idle_Digit_%d_Image' % d] = ('txt', str(d))
SPECIAL['NowPlaying_Idle_Colon_Image'] = ('txt', ':')
for s, f in (('2s', 2), ('5s', 5), ('10s', 10), ('20s', 20), ('30s', 30)):
    SPECIAL['Settings_Backlight_%s_Image' % s] = ('pie', f / 30)
for k in range(1, 36, 2):
    SPECIAL['DiskModeImage_SyncArrow%d' % k] = ('rot', (k - 1) // 2 * 10)
# Now Playing's playback bar, Apple Music style: a thin track, the fill in the accent, a round knob
SPECIAL['NowPlaying_ProgressBar_Inactive_Background_Image'] = ('band', (255, 255, 255), 0.3)
SPECIAL['NowPlaying_ProgressBar_Active_Background_Image'] = ('band', (255, 255, 255), 0.3)
SPECIAL['NowPlaying_ProgressBar_Inactive_Fill_Image'] = ('band', (0, 122, 255), 1.0)
SPECIAL['NowPlaying_ProgressBar_Active_Fill_Image'] = ('band', (0, 122, 255), 1.0)
SPECIAL['NowPlaying_ProgressBar_Scrub_Image'] = ('knob', (255, 255, 255), 1.0)
# the charging screen's battery, built by the OS from slices: left cap, middles repeated, right cap;
# up to the level it uses the Green slices, with Green_Middle_Cap where the charge ends
CHG_TOP, CHG_BOT, CHG_LEFT = 22, 97, 22          # body rows and its left edge (left cap x 22)
CHG_LINE, CHG_RADIUS, CHG_INSET = 4, 14, 8        # outline width, corner radius, fill inset
CHG_NUB, CHG_NUB_W = (45, 29), 7                  # nub rows (from, height) and width
CHG_OUTLINE, CHG_GREEN, CHG_RED = (142, 142, 147), (48, 209, 88), (255, 69, 58)
CHG_MID, CHG_RIGHT = 100, 200                     # battery x of a middle slice; body right edge
CHG_RCAP = CHG_RIGHT - 14                         # the right cap's body ends 14 px in
CHG_FILL_FROM = CHG_LEFT + CHG_INSET
CHG_SLICE = {                                     # slice: (battery x, body right edge, fill (from, to) or None, fill colour)
    'Empty_CapLeft':    (0, CHG_RIGHT, None, None),
    'Empty_Middle':     (CHG_MID, CHG_RIGHT, None, None),
    'Empty_CapRight':   (CHG_RCAP, CHG_RIGHT, None, None),
    'Green_CapLeft':    (0, CHG_RIGHT, (CHG_FILL_FROM, CHG_RIGHT + 50), CHG_GREEN),       # the fill runs on past the slice
    'Green_Middle':     (CHG_MID, CHG_RIGHT, (CHG_FILL_FROM, CHG_RIGHT + 50), CHG_GREEN),
    'Green_Middle_Cap': (CHG_MID, CHG_RIGHT, (CHG_FILL_FROM, CHG_MID + 6), CHG_GREEN),     # the charge ends 6 px in
    'Green_CapRight':   (CHG_RCAP, CHG_RIGHT, (CHG_FILL_FROM, CHG_RIGHT - CHG_INSET), CHG_GREEN),
    'Critical':         (0, 175, (CHG_FILL_FROM, CHG_FILL_FROM + 16), CHG_RED),            # whole battery, body to x 175
}
for part in CHG_SLICE:
    SPECIAL['ChargingMode_Image_' + part] = ('chg', part)

ACCENT = set()                                   # images that take the accent colour at boot
SAME = set()                                     # accent images that keep their own format and size (palette or ARGB)
RECODE = set()                                   # alpha masks stored as 8-bit palette images instead
# Settings > Language: the stock boxes (ink, x y w h) and weights; the bright one is alpha 1, the others the
# stock's dim 102/255. Willkommen is clipped at the right edge in the stock, so its box moves left to fit.
LANG = [
    ('Bienvenue', (8, 31, 105, 17), 'regular', 0.4),
    ('欢迎', (90, 60, 69, 39), 'regular', 0.4),
    ('Welcome', (31, 112, 105, 19), 'bold', 1.0),
    ('Willkommen', (30, 171, 125, 17), 'regular', 0.4),
]
TILE_FILL, TILE_GLYPH = (229, 229, 234), (142, 142, 147)        # iOS systemGray6-ish / systemGray
DARK_TILE = (0, 0, 0, 150)

# ---- originals ----
def original(name):
    pid = pkg.ids[name]
    fmt, w, h, stride, pal, rows = pkg.bmap(pid)
    return pid, fmt, w, h, stride, pal, rows

def ink_box(rows, w, h, thr=160):
    xs, ys = [], []
    for y in range(h):
        r = rows[y]
        for x in range(w):
            if r[4 * x + 3] >= thr: xs.append(x); ys.append(y)
    if not xs: return (0, 0, w, h)
    return (min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)

def ink_colour(rows, w, h):
    n = sr = sg = sb = 0
    for y in range(h):
        r = rows[y]
        for x in range(w):
            if r[4 * x + 3] >= 200: sr += r[4 * x]; sg += r[4 * x + 1]; sb += r[4 * x + 2]; n += 1
    if not n: return (255, 255, 255)
    c = (sr // n, sg // n, sb // n)
    chroma = max(c) - min(c)
    L = (c[0] * 299 + c[1] * 587 + c[2] * 114) // 1000
    if chroma < 40:                                # greys go to the iOS pure ends
        return (255, 255, 255) if L > 150 else (0, 0, 0) if L < 100 else c
    return c

# ---- masks ----
jobs, masks = [], {}
def sf(key, W, H, box, weight, what, rot=0):
    if key not in masks:
        jobs.append('%s %d %d %.2f %.2f %.2f %.2f %s %d %s' % (key, W, H, box[0], box[1], box[2], box[3], weight, rot, what))
        masks[key] = None
    return key

def load(key, W, H):
    d = open(os.path.join(OUT, key + '.a'), 'rb').read()
    return [v / 255 for v in d]

def ss_mask(W, H, inside, n=4):
    """supersampled coverage of inside(x, y) (pixel units)"""
    m = []
    for y in range(H):
        for x in range(W):
            c = 0
            for j in range(n):
                for i in range(n):
                    if inside(x + (i + 0.5) / n, y + (j + 0.5) / n): c += 1
            m.append(c / (n * n))
    return m

def rrect(x0, y0, w, h, r):
    def f(x, y):
        if x < x0 or y < y0 or x > x0 + w or y > y0 + h: return False
        cx = min(max(x, x0 + r), x0 + w - r); cy = min(max(y, y0 + r), y0 + h - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return f

def topround(x0, y0, x1, y1, r):
    """a box [x0, x1] x [y0, y1] with its two top corners rounded by r (the bottom stays square)"""
    def f(x, y):
        if x < x0 or y < y0 or x > x1 or y > y1: return False
        if y < y0 + r:
            cx = min(max(x, x0 + r), x1 - r)
            return (x - cx) ** 2 + (y - y0 - r) ** 2 <= r * r
        return True
    return f

def eq_bars(rows, w, h):
    """(x0, x1, top, bottom) of each EQ bar: a run of columns with ink; the top is the first row of ink in
    the run's centre column (the stock's edges are whole pixels), the bottom is the run's last row + 1"""
    cover = [max(rows[y][4 * x + 3] for y in range(h)) for x in range(w)]
    bars, x = [], 0
    while x < w:
        if cover[x] < 128: x += 1; continue
        s = x
        while x < w and cover[x] >= 128: x += 1
        c = (s + x - 1) // 2
        ys = [y for y in range(h) if rows[y][4 * c + 3] >= 128]
        bars.append((s, x, min(ys), max(ys) + 1))
    return bars

# ---- one image ----
def build(name, pass2):
    pid, fmt, w, h, stride, pal, rows = original(name)
    box = ink_box(rows, w, h)
    col = ink_colour(rows, w, h)
    layers = []                                     # (mask, (r, g, b), alpha scale)
    if sum(1 for r in rows for x in range(w) if r[4 * x + 3] == 255) >= 0.98 * w * h:
        # opaque picture (the idle screen's black and white glyphs): keep its background (the
        # corner colour), the glyph takes the colour farthest from it, fitted to that colour's box
        from collections import Counter
        bg = tuple(rows[0][0:3])
        cnt = Counter(tuple(r[4 * x:4 * x + 3]) for r in rows for x in range(w))
        col = max(cnt, key=lambda c: (sum(abs(a - b) for a, b in zip(c, bg)) > 200, cnt[c]))
        far = [bytes(v for x in range(w) for v in ((0, 0, 0, 255) if sum(abs(a - b) for a, b in zip(r[4 * x:4 * x + 3], bg)) > 200 else (0, 0, 0, 0))) for r in rows]
        box = ink_box(far, w, h)
        bg_layer = ([1.0] * (w * h), bg, 1.0)
        layers.append(bg_layer)
    sp = SPECIAL.get(name)
    if sp and sp[0] == 'bat':
        x0, y0, bw, bh = box
        if name.startswith('StatusBar') and name[-2:] in ('_0', '_1'):
            RECODE.add(pid)                          # battery percentage slots: drawn at run time (battery.c)
        if name.startswith('StatusBarBlack'): col = (255, 255, 255)
        elif name.startswith('StatusBarWhite'): col = (0, 0, 0)
        nub = max(2, round(bw * 0.08)); body = bw - nub - 1
        t = max(1, round(bh / 12))                   # outline thickness
        two = name.startswith('NowPlaying_Idle')     # 2-colour images: solid outline, no translucency
        oa = 1.0 if two else 0.45
        r = bh * 0.28
        outer = rrect(x0 + 0.5, y0 + 0.5, body - 1, bh - 1, r)
        inner = rrect(x0 + 0.5 + t, y0 + 0.5 + t, body - 1 - 2 * t, bh - 1 - 2 * t, max(r - t, 0.5))
        layers.append((ss_mask(w, h, lambda x, y: outer(x, y) and not inner(x, y)), col, oa))
        layers.append((ss_mask(w, h, rrect(x0 + bw - nub, y0 + bh * 0.32, nub, bh * 0.36, nub * 0.45)), col, oa))
        g = 2 * t + 0.5                             # fill inset: outline plus a gap
        fw = (body - 2 * g) * sp[1]
        if fw > 0.3:
            fill = (255, 59, 48) if sp[1] <= 0.2 and not two else col
            layers.append((ss_mask(w, h, rrect(x0 + g, y0 + g, fw, bh - 2 * g, min(max(r - g, 0.5), fw / 2))), fill, 1.0))
    elif sp and sp[0] == 'band':                     # rows 5..8 of the 14-row bar, no reflection
        ACCENT.add(pid)
        layers.append(([1.0 if 5 <= i // w <= 8 else 0.0 for i in range(w * h)], sp[1], sp[2]))
    elif sp and sp[0] == 'knob':
        import math
        ACCENT.add(pid)
        cx, cy, R = (w - 1) / 2, 6.5, 5.5
        layers.append((ss_mask(w, h, lambda x, y: math.hypot(x - 0.5 - cx, y - 0.5 - cy) <= R), sp[1], sp[2]))
    elif sp and sp[0] == 'chg':
        # Apple's slices share one frame: the body spans rows 22..96 (a reflection follows, now
        # dropped), from x 22 of the left cap to x 14 of the right cap, the nub beyond it. Each
        # slice draws its share of one flat battery laid out in battery x (CHG_SLICE: where the
        # slice sits, the body's right edge, the fill's extent, the fill colour).
        x0, right, fill, fill_c = CHG_SLICE[sp[1]]
        top, bot, t, r = CHG_TOP, CHG_BOT, CHG_LINE, CHG_RADIUS
        outer = rrect(CHG_LEFT - x0, top, right - CHG_LEFT, bot - top, r)
        inner = rrect(CHG_LEFT - x0 + t, top + t, right - CHG_LEFT - 2 * t, bot - top - 2 * t, r - t)
        layers.append((ss_mask(w, h, lambda x, y: outer(x, y) and not inner(x, y)), CHG_OUTLINE, 1.0))
        ny0, nh = CHG_NUB
        nub = rrect(right - x0 - 2, ny0, CHG_NUB_W + 2, nh, 3)
        layers.append((ss_mask(w, h, lambda x, y: nub(x, y) and x >= right - x0), CHG_OUTLINE, 1.0))
        if fill:
            g = CHG_INSET
            f = rrect(fill[0] - x0, top + g, fill[1] - fill[0], bot - top - 2 * g, r - g)
            layers.append((ss_mask(w, h, f), fill_c, 1.0))
    elif sp and sp[0] == 'pie':
        if fmt in (4, 8):                           # Settings previews: bigger, an accent image (see recode)
            box = (5, 5, 68, 68); col = (0, 122, 255); ACCENT.add(pid); RECODE.add(pid)
        x0, y0, bw, bh = box
        d = min(bw, bh); cx, cy, R = x0 + bw / 2, y0 + d / 2, d / 2 - 0.5
        import math
        ring = lambda x, y: R - max(1.5, d * 0.07) <= math.hypot(x - cx, y - cy) <= R
        def wedge(x, y):
            if math.hypot(x - cx, y - cy) > R - d * 0.14: return False
            a = (math.degrees(math.atan2(x - cx, cy - y)) + 360) % 360
            return a <= 360 * sp[1]
        layers.append((ss_mask(w, h, lambda x, y: ring(x, y) or wedge(x, y)), col, 1.0))
    elif sp and sp[0] == 'txt':
        k = sf(name, w, h, box, 'medium', 'text:' + sp[1])
        if pass2: layers.append((load(k, w, h), col, 1.0))
    elif sp and sp[0] == 'rot':
        k = sf(name, w, h, box, 'regular', 'arrow.triangle.2.circlepath', sp[1])
        if pass2: layers.append((load(k, w, h), col, 1.0))
    else:
        spec = next((m for m in MAP if re.fullmatch(m[0], name)), None)
        if not spec: return None
        style, sym, weight = spec[1], spec[2], spec[3]
        if len(spec) > 4: col = spec[4]
        if col in ('acc', 'accs'):                  # recoloured to the accent at boot ('accs': same format and size)
            if col == 'accs': SAME.add(pid)
            col = (0, 122, 255); ACCENT.add(pid)
        if name.startswith('ChargingMode_Image_Toggle'):
            RECODE.add(pid)                         # alpha masks the OS tints black (ChargingMode_Toggle_Color)
        big = re.fullmatch(r'Settings_\w+_Image', name) and (w, h) in ((78, 121), (78, 122))
        if big and style == G: box = (5, 5, 68, 68)  # Settings previews: no base panel, bigger symbol
        if style == G and fmt in (4, 8) and max(w, h) >= 60:
            # preview images are white masks the OS draws in a fixed white: stored as accent images
            col = (0, 122, 255); ACCENT.add(pid); RECODE.add(pid)
        if style is None: style = D if fmt == 0x65 and w > 40 else G
        if style == 'A':                            # a large symbol in the accent
            x0, y0, bw, bh = box
            d = min(bw, bh); x0 += (bw - d) / 2; y0 += (bh - d) / 2
            k = sf(name, w, h, (x0 + d * 0.06, y0 + d * 0.06, d * 0.88, d * 0.88), weight, sym)
            if pass2: layers.append((load(k, w, h), (0, 122, 255), 1.0))
            ACCENT.add(pid)
        elif style == 'cal':
            x0, y0, bw, bh = box
            r = min(bw, bh) * 0.16
            page = rrect(x0, y0, bw, bh, r)
            layers.append((ss_mask(w, h, page), (255, 255, 255), 1.0))
            layers.append((ss_mask(w, h, lambda x, y: page(x, y) and y < y0 + 22), (255, 59, 48), 1.0))
        elif style == 'face':
            import math
            x0, y0, bw, bh = box
            d = min(bw, bh); cx, cy, R = x0 + bw / 2, y0 + bh / 2, d / 2
            ink = (0, 0, 0) if col[0] > 128 else (255, 255, 255)
            layers.append((ss_mask(w, h, lambda x, y: math.hypot(x - cx, y - cy) <= R), col, 1.0))
            def tick(x, y):
                dx, dy = x - cx, y - cy; rr = math.hypot(dx, dy)
                if not (R * 0.80 <= rr <= R * 0.93): return False
                a = math.degrees(math.atan2(dx, -dy)) % 30
                half = math.degrees(max(0.6, d * 0.012) / rr)
                return a <= half or a >= 30 - half
            layers.append((ss_mask(w, h, tick), ink, 1.0))
        elif style == 'P':
            pass                                    # the Settings base panel: empty
        elif style == 'eq':
            # each bar: the stock's column run and its top (the centre column's first row of ink), flat with
            # rounded top corners, at full alpha; the stock gloss band is dropped
            for x0, x1, top, bot in eq_bars(rows, w, h):
                layers.append((ss_mask(w, h, topround(x0, top, x1, bot, 3)), (255, 255, 255), 1.0))
        elif style == 'lang':
            for i, (text, bx, wt, a) in enumerate(LANG):
                k = sf('lang_%d' % i, w, h, bx, wt, 'ui:' + text)
                if pass2: layers.append((load(k, w, h), (255, 255, 255), a))
        elif style == T and fmt != 0x565:           # art placeholders: the symbol in the accent, no tile
            x0, y0, bw, bh = ink_box(rows, w, h, 40)
            k = sf(name, w, h, (x0 + bw * 0.15, y0 + bh * 0.15, bw * 0.7, bh * 0.7), weight, sym)
            if pass2: layers.append((load(k, w, h), (0, 122, 255), 1.0))
            ACCENT.add(pid)
        elif style == T:
            x0, y0, bw, bh = ink_box(rows, w, h, 40)
            layers.append((ss_mask(w, h, rrect(x0, y0, bw, bh, min(bw, bh) * 0.2)), TILE_FILL, 1.0))
            k = sf(name, w, h, (x0 + bw * 0.25, y0 + bh * 0.25, bw * 0.5, bh * 0.5), weight, sym)
            if pass2: layers.append((load(k, w, h), TILE_GLYPH, 1.0))
        elif style == D:
            x0, y0, bw, bh = box
            layers.append((ss_mask(w, h, rrect(x0, y0, bw, bh, min(bw, bh) * 0.22)), DARK_TILE[:3], DARK_TILE[3] / 255))
            k = sf(name, w, h, (x0 + bw * 0.3, y0 + bh * 0.3, bw * 0.4, bh * 0.4), weight, sym)
            if pass2: layers.append((load(k, w, h), (255, 255, 255), 1.0))
        else:
            base, _, badge = sym.partition('+')
            if badge:                               # a badge over the lower right of the base symbol
                x0, y0, bw, bh = box
                k = sf(name, w, h, (x0, y0, bw * 0.7, bh * 0.7), weight, base)
                kb = sf(name + '_badge', w, h, (x0 + bw * 0.45, y0 + bh * 0.45, bw * 0.55, bh * 0.55), weight, badge)
                if pass2: layers.append((load(kb, w, h), col, 1.0))
            else:
                k = sf(name, w, h, box, weight, sym)
            if pass2: layers.append((load(k, w, h), col, 1.0))
    if not pass2: return None
    if pid in ACCENT:                               # accent symbols stand alone: no opaque tile behind them
        layers = [l for l in layers if l[0] is not (bg_layer[0] if 'bg_layer' in locals() else None)]
    # composite (straight alpha)
    px = [[0.0, 0.0, 0.0, 0.0] for _ in range(w * h)]
    for m, c, s in layers:
        for i, a in enumerate(m):
            a *= s
            if a <= 0: continue
            p = px[i]; na = a + p[3] * (1 - a)
            for ch in range(3):
                p[ch] = (c[ch] * a + p[ch] * p[3] * (1 - a)) / na
            p[3] = na
    rgba = [(round(p[0]), round(p[1]), round(p[2]), round(p[3] * 255)) for p in px]
    return pid, fmt, w, h, stride, rgba

# ---- encode in the original's format and size ----
def quantise(rgba, n):
    uniq = set(rgba)
    step = 1
    q = lambda v: tuple(min(255, (c + step // 2) // step * step) for c in v)
    while True:
        m = {v: (q(v) if v[3] else (0, 0, 0, 0)) for v in uniq}
        if len(set(m.values())) <= n: return m
        step += 1

def recode(w, h, rgba):
    """an 8-bit palette image (fmt 0x64) of one colour at 64 alpha levels"""
    n, stride = 64, (w + 3) & ~3
    c = next((v[:3] for v in rgba if v[3]), (0, 122, 255))
    data = struct.pack('<I', n) + b''.join(struct.pack('<I', (k * 255 // (n - 1)) << 24 | c[0] << 16 | c[1] << 8 | c[2]) for k in range(n))
    px = bytearray(stride * h)
    for y in range(h):
        for x in range(w): px[y * stride + x] = (rgba[y * w + x][3] * (n - 1) + 127) // 255
    data += bytes(px)
    return struct.pack('<HHHHIIIII', 0x64, 0xffff, stride, 8, 0, 0, h, w, len(data)) + data

def encode(pid, fmt, w, h, stride, rgba, orig_rows):
    if pid in RECODE or (pid in ACCENT and pid not in SAME): return recode(w, h, rgba)    # accent images: their own one-colour palette
    off, size = bm_index[pid]
    blob = bytearray(pkg.D[DATA + off - BASE:DATA + off - BASE + size])
    p = 0x1c
    if fmt in (0x64, 0x65) and struct.unpack_from('<I', blob, p)[0] <= 16:
        # small palettes keep their entries and order: the OS addresses some by index (the
        # hold-screen glyphs are [white, black]); each pixel takes the nearest entry
        n = struct.unpack_from('<I', blob, p)[0]
        pal = [struct.unpack_from('<I', blob, p + 4 + 4 * i)[0] for i in range(n)]
        pal = [((v >> 16) & 255, (v >> 8) & 255, v & 255, v >> 24) for v in pal]
        def near(c):
            if c[3] == 0:
                z = [i for i, q in enumerate(pal) if q[3] == 0]
                if z: return z[0]
            return min(range(n), key=lambda i: sum((a - b) ** 2 for a, b in zip(c, pal[i])) if pal[i][3] else 1 << 30)
        cache = {}
        base = p + 4 + 4 * n
        for y in range(h):
            for x in range(w):
                c = rgba[y * w + x]
                k = cache.get(c)
                if k is None: k = cache[c] = near(c)
                if fmt == 0x64: blob[base + y * stride + x] = k
                else: struct.pack_into('<H', blob, base + y * stride + 2 * x, k)
    elif fmt in (0x64, 0x65):
        n = struct.unpack_from('<I', blob, p)[0]
        m = quantise(rgba, n)
        pal = sorted(set(m.values()), key=lambda v: (v[3], v))
        pal += [(0, 0, 0, 0)] * (n - len(pal))
        idx = {v: i for i, v in enumerate(pal) if i == pal.index(v)}
        for i, v in enumerate(pal):
            struct.pack_into('<I', blob, p + 4 + 4 * i, v[3] << 24 | v[0] << 16 | v[1] << 8 | v[2])
        base = p + 4 + 4 * n
        for y in range(h):
            for x in range(w):
                k = idx[m[rgba[y * w + x]]]
                if fmt == 0x64: blob[base + y * stride + x] = k
                else: struct.pack_into('<H', blob, base + y * stride + 2 * x, k)
    elif fmt == 0x1888:
        for y in range(h):
            for x in range(w):
                r, g, b, a = rgba[y * w + x]
                struct.pack_into('<I', blob, p + y * stride + 4 * x, a << 24 | r << 16 | g << 8 | b)
    elif fmt == 0x565:
        bg = orig_rows[0][0:3]
        for y in range(h):
            for x in range(w):
                r, g, b, a = rgba[y * w + x]
                r, g, b = [(c * a + bc * (255 - a)) // 255 for c, bc in zip((r, g, b), bg)]
                struct.pack_into('<H', blob, p + y * stride + 2 * x, (r >> 3) << 11 | (g >> 2) << 5 | b >> 3)
    elif fmt == 8:
        for y in range(h):
            for x in range(w): blob[p + y * stride + x] = rgba[y * w + x][3]
    elif fmt == 4:
        for y in range(h):
            for x in range(0, w, 2):
                a = (rgba[y * w + x][3] + 8) // 17
                b = (rgba[y * w + x + 1][3] + 8) // 17 if x + 1 < w else 0
                blob[p + y * stride + x // 2] = a << 4 | b
    else:
        return None
    return bytes(blob)

def lz(b):
    """token t < 0x80: t+1 literal bytes follow; t >= 0x80: copy (t & 0x7f) + 3 bytes from u16 distance back"""
    out, lit, i, heads = bytearray(), bytearray(), 0, {}
    def flush():
        while lit:
            k = lit[:128]; out.append(len(k) - 1); out.extend(k); del lit[:128]
    while i < len(b):
        best, bd = 0, 0
        if i + 3 <= len(b):
            key = b[i:i + 3]
            for j in reversed(heads.get(key, [])[-48:]):
                if i - j > 65535: break
                k = 3
                while k < 130 and i + k < len(b) and b[j + k] == b[i + k]: k += 1
                if k > best: best, bd = k, i - j
                if k == 130: break
            for d in (1, 2, 4):                 # runs of 1-, 2- and 4-byte pixels
                if i - d >= 0:
                    k = 0
                    while k < 130 and i + k < len(b) and b[i - d + k] == b[i + k]: k += 1
                    if k > best: best, bd = k, d
        if best >= 3:
            flush(); out.append(0x80 + best - 3); out += struct.pack('<H', bd)
            for t in range(i, i + best):
                if t + 3 <= len(b): heads.setdefault(b[t:t + 3], []).append(t)
            i += best
        else:
            if i + 3 <= len(b): heads.setdefault(b[i:i + 3], []).append(i)
            lit.append(b[i]); i += 1
    flush()
    return bytes(out)

def unlz(c, n):
    o, i = bytearray(), 0
    while len(o) < n:
        t = c[i]; i += 1
        if t < 0x80: o += c[i:i + t + 1]; i += t + 1
        else:
            d = c[i] | c[i + 1] << 8; i += 2
            for _ in range((t & 0x7f) + 3): o.append(o[-d])
    return bytes(o)

names = [n for n in pkg.ids if pkg.ids[n] in bm_index and (n in SPECIAL or any(re.fullmatch(m[0], n) for m in MAP))]
names.sort()
for n in names: build(n, False)
# battery percentage digits (runtime/battery.c): SF Compact Bold, 7 px tall, cropped to their ink
DIG_W, DIG_H = 8, 9
for d in range(10): sf('digit_%d' % d, DIG_W, DIG_H, (0, 1, DIG_W, 7), 'bold', 'text:%d' % d)
open(os.path.join(OUT, 'jobs.list'), 'w').write('\n'.join(jobs) + '\n')
r = subprocess.run([RENDER, os.path.join(OUT, 'jobs.list'), OUT], capture_output=True, text=True)
if r.stderr: print(r.stderr.strip())
tab, blob, sheet = [], bytearray(), []
for n in names:
    try:
        res = build(n, True)
    except FileNotFoundError:
        print('skipped (no symbol):', n); continue
    if not res: continue
    pid, fmt, w, h, stride, rgba = res
    orig_rows = pkg.bmap(pid)[5]
    e = encode(pid, fmt, w, h, stride, rgba, orig_rows)
    if not e: print('skipped (format %#x): %s' % (fmt, n)); continue
    tab.append((pid, len(e), len(blob), n, bm_index[pid][1])); blob += e + b'\0' * (-len(e) & 3)
    sheet.append((n, w, h, orig_rows, [bytes(v for px in rgba[y * w:(y + 1) * w] for v in px) for y in range(h)]))

with open(os.path.join(HERE, '..', '..', 'runtime', 'sfdigits_data.h'), 'w') as f:
    f.write('/* generated by tools/sf/gen_sficons.py: SF Compact digits, personal use, do not commit */\n')
    f.write('#define SF_DIGIT_H %d\n#define SF_DIGIT_W %d\nstatic const uint8_t sf_digit_w[10] = {' % (DIG_H, DIG_W))
    cells = []
    for d in range(10):
        a = open(os.path.join(OUT, 'digit_%d.a' % d), 'rb').read()
        cols = [x for x in range(DIG_W) if any(a[y * DIG_W + x] > 24 for y in range(DIG_H))]
        if not cols: raise SystemExit('digit %d rendered empty: check render.swift output' % d)
        x0, x1 = cols[0], cols[-1] + 1
        cells.append((x1 - x0, bytes(a[y * DIG_W + x] if x < x1 else 0 for y in range(DIG_H) for x in range(x0, x0 + DIG_W))))
    f.write(', '.join(str(c[0]) for c in cells) + ' };\nstatic const uint8_t sf_digit[10][%d] = {\n' % (DIG_W * DIG_H))
    for w, c in cells: f.write('    { ' + ', '.join(str(v) for v in c) + ' },\n')
    f.write('};\n')
# both sets go on Apple's resource volume (Resources/Icons, ospatch/fwpart.py rsrc); region E keeps the table
import icon_sets
icon_sets.write(sys.argv[1], [(pid, n) for pid, size, off, n, osz in tab], {pid: bytes(blob[off:off + size]) for pid, size, off, n, osz in tab},
                ACCENT, os.path.join(HERE, '..', '..', 'runtime', 'sficons_data.h'), os.path.join(OUT, 'rsrc', 'Resources', 'Icons'))
raw = sum(t[1] for t in tab)
print('%d icons, %d bytes raw, %d bytes in the image' % (len(tab), raw, len(blob)))

# before/after sheet on two backgrounds
W, x, y, rowh, place = 1000, 4, 4, 0, []
for n, w, h, o, s in sheet:
    if x + 2 * w + 12 > W: x = 4; y += rowh + 8; rowh = 0
    place.append((x, y, n, w, h, o, s)); x += 2 * w + 16; rowh = max(rowh, h)
H = y + rowh + 4
cv = Canvas(W, H, (128, 128, 128, 255))
for x, y, n, w, h, o, s in place:
    cv.paste(o, w, h, x, y); cv.paste(s, w, h, x + w + 2, y)
cv.save(os.path.join(OUT, 'before_after.png'))

# the small ones (and the idle screen) at 3x
W, x, y, rowh, place = 1000, 4, 4, 0, []
for n, w, h, o, s in sheet:
    if not (max(w, h) <= 30 or n.startswith('NowPlaying_Idle')): continue
    if x + 6 * w + 12 > W: x = 4; y += rowh + 8; rowh = 0
    place.append((x, y, w, h, o, s)); x += 6 * w + 16; rowh = max(rowh, 3 * h)
cv = Canvas(W, y + rowh + 4, (128, 128, 128, 255))
for x, y, w, h, o, s in place:
    cv.paste(o, w, h, x, y, 3); cv.paste(s, w, h, x + 3 * w + 4, y, 3)
cv.save(os.path.join(OUT, 'small.png'))
