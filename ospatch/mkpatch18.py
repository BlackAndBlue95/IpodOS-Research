#!/usr/bin/env python3
"""Builds the patched OS image: Apple's 2.0.4 osos with region E appended and hooked in.
See HOOKS.md for every word changed and why.

  mkpatch18.py --base stock|v17 --e e.bin --syms e.syms --out osos-v18.dec [--hooks libload|all]

--base v17 keeps the earlier FLAC patch (stub and flacdec.bin) and adds only region E with the library load hooks."""
import argparse, hashlib, struct, sys

STOCK_MD5 = 'be2bca20064a5f6e3e2c474d9ec2819c'
V17_MD5 = 'f7c95a07bb970f622eeae9c037463a5e'
HDR = 0x800
DRAM_DELTA = 0x07ff5128           # DRAM addr = body offset + DRAM_DELTA
IRAM_BASE = 0x22000000            # body 0..0xaed8 runs here
RW_LOAD, RW_EXEC = 0xa1ab60, 0x08a0fc88
RW_END_BODY = 0xa1b5e4            # end of the RW data in the body (0xa1ab60 + 0xa84)
E_BASE, E_RESERVE = 0x08b33000, 0x80000
E_LOAD = RW_LOAD + (E_BASE - RW_EXEC)   # 0xb3ded8
BSS_END = 0x08b32b58

ap = argparse.ArgumentParser()
ap.add_argument('--base', choices=('stock', 'v17'), required=True)
ap.add_argument('--image', required=True, help='stock or v17 .dec matching --base')
ap.add_argument('--e', required=True)
ap.add_argument('--syms', required=True)
ap.add_argument('--out', required=True)
ap.add_argument('--hooks', choices=('libload', 'all'), default='all')
ap.add_argument('--governor-floor', action='store_true',
                help='with the backlight on, the OS governor floors at L1 (108 MHz) instead of pinning L0')
a = ap.parse_args()

img = bytearray(open(a.image, 'rb').read())
md5 = hashlib.md5(img).hexdigest()
assert md5 == (STOCK_MD5 if a.base == 'stock' else V17_MD5), 'unexpected base image ' + md5
e = open(a.e, 'rb').read()
sym = {}
for l in open(a.syms):
    p = l.split()
    if len(p) == 3: sym[p[2]] = int(p[0], 16)
E_END = E_BASE + len(e)
assert len(e) <= E_RESERVE, 'region E is %d bytes, more than the %d reserved' % (len(e), E_RESERVE)
assert sym['__e_start'] == E_BASE

def off(addr):
    """file offset of a DRAM or IRAM run address"""
    if IRAM_BASE <= addr < IRAM_BASE + 0xaed8: return HDR + addr - IRAM_BASE
    assert 0x08000000 <= addr < 0x08a1070c, hex(addr)
    return HDR + addr - DRAM_DELTA
def rd(addr): return struct.unpack_from('<I', img, off(addr))[0]
def wr(addr, new, old):
    cur = rd(addr)
    assert cur == old, 'at %08x: found %08x, expected %08x' % (addr, cur, old)
    struct.pack_into('<I', img, off(addr), new)
def branch_target(at, w):
    o = w & 0xffffff
    if o & 0x800000: o -= 0x1000000
    return (at + 8 + 4 * o) & 0xffffffff
def retarget(at, old_to, new_to):
    """keep the condition and link bit of the b/bl at 'at', change its target"""
    w = rd(at)
    assert (w & 0x0e000000) == 0x0a000000 and branch_target(at, w) == old_to, \
        'at %08x: %08x is not a branch to %08x' % (at, w, old_to)
    d = new_to - at - 8
    assert -(1 << 25) <= d < (1 << 25) and not d & 3, 'out of range %08x -> %08x' % (at, new_to)
    wr(at, (w & 0xff000000) | ((d >> 2) & 0xffffff), w)
def entry_hook(at, w0, w1, to):
    """b to region E over the first instruction; E runs w0 and returns to at+4"""
    assert rd(at + 4) == w1, 'at %08x+4: %08x' % (at, rd(at + 4))
    d = to - at - 8
    wr(at, 0xea000000 | ((d >> 2) & 0xffffff), w0)

# region E and the memory layout
body_len = struct.unpack_from('<I', img, 0x0c)[0]
assert body_len == 0xa1b5e8 and len(img) == HDR + 0xa1b5f0, (hex(body_len), hex(len(img)))  # padded to 16
assert img[HDR + RW_END_BODY:] == b'\0' * (len(img) - HDR - RW_END_BODY), 'unexpected data after RW'
img = img[:HDR + RW_END_BODY] + b'\0' * (E_LOAD - RW_END_BODY) + e
wr(IRAM_BASE + 0x47ac, E_END - RW_EXEC, 0xa84)                 # RW copy length: the data copy now reaches E
wr(0x0804b44c, E_BASE + E_RESERVE, BSS_END)                    # heap base
wr(0x0807bfac, E_BASE + E_RESERVE, BSS_END)                    # ADS heap init (no-op backends)
struct.pack_into('<I', img, 0x0c, len(img) - HDR)              # body size, checked by osos_boot

# hooks
LIBLOAD = 0x0805d62c             # Apple's library loader
retarget(0x0804d7c4, LIBLOAD, sym['e_libload_boot'])
retarget(0x081ae29c, LIBLOAD, sym['e_libload_task'])

if a.hooks == 'all':
    assert a.base == 'stock', '--hooks all needs the stock base'
    def bl_at(addr, to): wr(addr, 0xeb000000 | (((to - addr - 8) >> 2) & 0xffffff), rd(addr))
    # FLAC classifier and ArtworkDB tolerance
    wr(0x08087260, (0x0 << 28) | 0x0a000000 | (((0x08087214 - 0x08087260 - 8) >> 2) & 0xffffff), 0x0a000005)   # '.flac' match -> WAV type 3
    wr(0x08087264, 0xe3a02003, 0xe3a02004)                     # aif/aifc compare: 3 characters
    o = off(0x080872f0); assert img[o:o + 5] == b'aiff\0'; img[o:o + 4] = b'flac'   # 'aiff' type string -> 'flac'
    wr(0x0804d434, 0x0a000000, 0x0a000004)                     # no ArtworkDB (-43): keep the artwork library empty, not discarded
    wr(0x0804d3d0, 0xe1a00000, 0x1a00001d)                     # missing Artwork folder: carry on
    wr(0x0804d410, 0xe1a00000, 0x1a00000d)                     # missing ArtworkDB: still try the load
    # call sites
    retarget(0x0828a96c, 0x081ea554, sym['e_flac_open'])       # WAV open: File constructor -> FLAC open
    for at in (0x0804647c, 0x080464b0, 0x080464cc, 0x08092e40, 0x080931c0, 0x080c70b8, 0x08299628):
        retarget(at, 0x080457e4, sym['e_art_find'])            # artwork lookup
    for at in (0x081aa97c, 0x08264ab4, 0x08264b78, 0x08264d20, 0x082bd214):
        retarget(at, 0x080f32ec, sym['e_art_load'])            # artwork load
    for at in (0x082d6314, 0x082d9b14): retarget(at, 0x082d95dc, sym['e_lfn_batch'])   # FAT long names across sectors
    for at in (0x08163a6c, 0x08137dc0): retarget(at, 0x082c5f14, sym['e_art_clear'])   # white clear: album view, main menu art pane
    wr(0x08270980, 0xeb000000 | (((sym['e_refl_gate'] - 0x08270980 - 8) >> 2) & 0xffffff), 0xe5940094)   # cover reflection load -> e_refl_gate, 0 in Modern style
    wr(0x08261ef4, 0xeb000000 | (((sym['e_hl_bottom_r'] - 0x08261ef4 - 8) >> 2) & 0xffffff), 0xe5cd6118)   # selection bar bottom row: store the accent's red
    wr(0x08261f24, 0xeb000000 | (((sym['e_hl_edge_r'] - 0x08261f24 - 8) >> 2) & 0xffffff), 0xe5cd6110)     # selection bar edge: store the accent's red
    wr(0x08142140, 0xe3a02000, 0xe3a02001)                     # USB mass storage: parse_mbr 0, so the data view starts at LBA 0 (whole disk)
    wr(0x08143dd4, 0xeb000000 | (((sym['e_lcd_clear'] - 0x08143dd4 - 8) >> 2) & 0xffffff), 0xe3e014ff)   # LCD clear (white) -> e_lcd_clear, sets r1
    retarget(0x080da098, 0x080dc410, sym['e_loc_db'])          # db location -> record
    for at in (0x0805f738, 0x080d448c): retarget(at, 0x0804fde0, sym['e_loc_set'])   # path -> record
    for at in (0x08048288, 0x080cc33c, 0x080d7fa0, 0x080d7fdc, 0x080da17c):
        retarget(at, 0x080605c8, sym['e_loc_path'])            # record -> path
    # function entries
    entry_hook(0x08133980, 0xe92d47f0, 0xe24dde31, sym['hk_gtl'])
    entry_hook(0x0826d8e0, 0xe92d43f8, 0xe1a04000, sym['hk_seek'])
    entry_hook(0x0826d5fc, 0xe92d47f0, 0xe24dd020, sym['hk_read'])
    entry_hook(0x0828a5ac, 0xe92d43f0, 0xe1a04000, sym['hk_parse'])
    # FLAC decoder selector literal (IRAM)
    wr(IRAM_BASE + 0x3824, sym['flac_select_shim'], 0x081a4900)
    # theme: literals pointing at the OS grey globals (white, #AA, #7F, #55, black) point at region E's
    # copies, read through the pointer at draw time. Text drawn black -> theme_text, text drawn white ->
    # theme_white, fills and frames -> theme_pal_*. KEEP lists the literals left on the OS globals.
    GREYS = {0x089cc8c0: 'theme_pal_white', 0x089cc8c4: 'theme_pal_aa', 0x089cc8c8: 'theme_pal_7f',
             0x089cc8cc: 'theme_pal_55', 0x089cc8d0: 'theme_pal_black'}
    TEXT_BLACK = (0x08092ca0, 0x08285a14, 0x08193d88, 0x080e27f0, 0x081383d8, 0x08082b94, 0x08085898,
                  0x0827b104, 0x08196608, 0x081515dc)
    TEXT_WHITE = (0x08285a10, 0x081a8690, 0x08092c9c, 0x080e27f4, 0x081383e8, 0x08193d84)   # 0x08082b8c is a SetBg fill under text: theme_pal_white
    KEEP = (0x0813dde0, 0x0813ddec, 0x0811f184, 0x0826587c, 0x08265b98, 0x082a31cc, 0x082a5d88, 0x08286914,
        0x081a7e8c,   # marquee scroll bitmap fill: the OS white, the mask's paper level
        0x082a6494,   # 0x082a6350 fills the five OS greys at start-up: it must write the OS globals, not E's copies
                      # if retargeted, the OS white stays 0 and the scroll mask's paper becomes ink
        0x08092c9c, 0x08092ca0,   # 0x08092bac expands 2-bpp bitmaps such as the marquee mask through this pair
                                  # a palette, not text: themed, the mask came out light, a white block behind titles
)
    GREY_LITERALS = (
        0x08082b8c, 0x080858a4, 0x08092c9c, 0x080e27f4, 0x0810025c, 0x0811d8ec, 0x0811def0, 0x0811eb30, 0x081383e8, 0x0813ddec,
        0x081515e0, 0x08151844, 0x081639e8, 0x0817bee8, 0x08193d84, 0x081a7e8c, 0x081a8690, 0x081a90c4, 0x081ad740, 0x081adcb0,
        0x081df44c, 0x08203794, 0x08258600, 0x08258990, 0x08258a34, 0x08258cd8, 0x08258f94, 0x08262594, 0x08262bc8, 0x0828191c,
        0x08285a10, 0x082a4c0c, 0x082a57c8, 0x082a6494,
        0x08082b90, 0x080e27fc, 0x080e7b88, 0x081383e4, 0x081515e4, 0x0815184c, 0x08262590, 0x08262bcc,
        0x08082b94, 0x08085898, 0x08092ca0, 0x080e27f0, 0x0811f184, 0x081383d8, 0x0813dde0, 0x081515dc, 0x08193d88, 0x08196608,
        0x08262584, 0x08262bc0, 0x0826587c, 0x08265b98, 0x0827b104, 0x08285a14, 0x08286914, 0x082a31cc, 0x082a5d88,
        0x080e2804, 0x081383dc, 0x08151848, 0x081beb0c, 0x08258604, 0x0825898c, 0x08258a30, 0x08258cdc, 0x08258f90, 0x0826258c, 0x08262bc4,
        0x081383e0, 0x082625a8)
    for at in GREY_LITERALS:
        old = rd(at)
        assert old in GREYS, '%08x does not hold a grey global' % at
        if at in KEEP: continue
        target = 'theme_text' if at in TEXT_BLACK else 'theme_white' if at in TEXT_WHITE else GREYS[old]
        wr(at, sym[target], old)
    # Settings resources (gen_settings.py): each changed type's index is copied into region E, with our
    # entries appended and replaced entries changed in that copy; the UI name table moves there too.
    import json, os
    plan = json.load(open(os.path.join(os.path.dirname(a.e), 'settings_data.json')))
    PKG_DATA, PKG = 0x08417928, 0x08400258
    for t, p in plan['types'].items():
        base = sym[p['tab']]
        for pos, n in [(p['count'] + k, n) for k, n in enumerate(p['new'])] + [(n['pos'], n) for n in p['replaced']]:
            eoff = E_LOAD + (base - E_BASE) + 12 * pos                  # the entry's offset in the image
            assert struct.unpack_from('<I', img, HDR + eoff)[0] == n['id'], (t, n)
            struct.pack_into('<II', img, HDR + eoff + 4, sym[n['sym']] - PKG_DATA, n['size'])
        wr(p['typerec'] + 4, p['count'] + len(p['new']), p['count'])   # type record: count, index offset
        wr(p['typerec'] + 12, base - PKG, rd(p['typerec'] + 12))
    nm = plan['names']
    for lit in nm['literals']: wr(lit, sym[nm['sym']], nm['table'])
    for at, old, new in nm['cmps']: wr(at, new, old)
    # settings row actions and value text (vtable slots)
    wr(0x0899bf4c, sym['e_settings_action'], 0x0821c690)       # HandleAction slots -> e_settings_action
    wr(0x0899fe64, sym['e_settings_action'], 0x0821c690)
    wr(0x0898fedc, sym['e_settings_provider'], 0x081e03ec)     # settings provider slot -> e_settings_provider
    entry_hook(0x0817ce28, 0xe92d4010, 0xe1a04000, sym['hk_tick'])   # clock minute tick
    # root UI init, before the first view
    entry_hook(0x081135a0, 0xe92d40f8, 0xe59d5018, sym['hk_uiinit'])
    entry_hook(0x0827ee0c, 0xe92d4070, 0xe3a03000, sym['hk_appinit'])   # Music app init: theme before anything of the app exists
    entry_hook(0x08143c04, 0xe92d41f0, 0xe24dde42, sym['hk_lcdinit'])   # LCD init: captures the LCD driver
    entry_hook(0x08113470, 0xe92d4070, 0xe24dd038, sym['hk_render'])   # UI render pass: deferred theme walk
    entry_hook(0x081a8444, 0xe92d4ff7, 0xe24dd038, sym['hk_marq'])     # marquee draw
    wr(0x081a7e70, 0xeb000000 | (((sym['e_marq_ink'] - 0x081a7e70 - 8) >> 2) & 0xffffff), 0xeb0000ec)   # scroll mask text ink: black


if a.governor_floor:
    # PowerMgmt (IRAM 0x220029ac): with the backlight timer armed the level is forced to 0 (216 MHz, 1200 mV).
    # Floor it at level 1 (108 MHz, 1100 mV) instead; load still raises it.
    wr(IRAM_BASE + 0x2a4c, 0xe3a00001, 0xe3a00000)

open(a.out, 'wb').write(img)
print('ok %s: %d bytes, body %#x, E %#x-%#x (%d bytes of %d), heap from %#x%s, md5 %s' % (
    a.out, len(img), len(img) - HDR, E_BASE, E_END, len(e), E_RESERVE, E_BASE + E_RESERVE,
    ', governor floor L1' if a.governor_floor else '', hashlib.md5(img).hexdigest()))
