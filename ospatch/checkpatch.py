#!/usr/bin/env python3
"""Checks a patched image the way the OS will see it: replays the startup copy (0x220046e0) and BSS
zeroing (0x220047e8) from the image's own constants, then verifies region E landed at its link
address intact, the heap starts after it, and every hook branch lands on the right E symbol.
  checkpatch.py osos-v18.dec e.bin e.syms [all]"""
import struct, sys

img = open(sys.argv[1], 'rb').read()
e = open(sys.argv[2], 'rb').read()
sym = {}
for l in open(sys.argv[3]):
    p = l.split()
    if len(p) == 3: sym[p[2]] = int(p[0], 16)
full = len(sys.argv) > 4 and sys.argv[4] == 'all'
HDR = 0x800
body = img[HDR:]
fails = 0
def check(c, what):
    global fails
    print(('  ok   ' if c else '  FAIL ') + what)
    fails += not c

assert img[:4] == b'8702'
blen = struct.unpack_from('<I', img, 0x0c)[0]
check(blen == len(body), 'header body size %#x matches the file' % blen)

iram = bytearray(body[:0xaed8])
def iw(a): return struct.unpack_from('<I', iram, a - 0x22000000)[0]
# the startup copy's own constants
ro_len, rw_exec, rw_len = iw(0x220047a4), iw(0x220047a8), iw(0x220047ac)
zi_base, zi_len = iw(0x22004834), iw(0x22004838)
check((ro_len, rw_exec) == (0xa0fc88, 0x08a0fc88), 'code region constants unchanged')
mem = {}
dram = bytearray(0x00c00000)                       # 0x08000000..0x08c00000
dram[0:ro_len] = body[0xaed8:0xaed8 + ro_len]
src = 0xaed8 + ro_len
seg = body[src:src + rw_len]
check(len(seg) == rw_len, 'the image holds the whole data copy (%#x bytes)' % rw_len)
dram[rw_exec - 0x08000000:rw_exec - 0x08000000 + rw_len] = seg
dram[zi_base - 0x08000000:zi_base - 0x08000000 + zi_len] = b'\0' * zi_len
def dw(a): return struct.unpack_from('<I', dram, a - 0x08000000)[0]
E = sym['__e_start']
# region E in the image equals e.bin except the Settings index entries mkpatch18 fills in
e_expect = bytearray(e)
import json, os
plan = json.load(open(os.path.join(os.path.dirname(sys.argv[2]), 'settings_data.json')))
for _t, _p in plan['types'].items():
    for _pos in [_p['count'] + _k for _k in range(len(_p['new']))] + [_n['pos'] for _n in _p['replaced']]:
        _o = sym[_p['tab']] - E + 12 * _pos + 4
        e_expect[_o:_o + 8] = dram[E - 0x08000000 + _o:E - 0x08000000 + _o + 8]
fast_libc = 'kfix_libc' in sym and dw(0x0802cedc) == sym['kfix_libc']
if fast_libc:   # --fast-libc: kfix_libc holds Apple's IRAM libc routines
    _o = sym['kfix_libc'] - E
    e_expect[_o:_o + 0x2f8] = iram[0x20:0x318]
dither = 'dither565' in sym and dw(sym['dither565']) == 1
if dither:      # --dither: mkpatch18 sets the switch
    struct.pack_into('<I', e_expect, sym['dither565'] - E, 1)
np_accent = 'np_accent' in sym and dw(sym['np_accent']) == 1
if np_accent:   # --np-accent: likewise
    struct.pack_into('<I', e_expect, sym['np_accent'] - E, 1)
check(bytes(dram[E - 0x08000000:E - 0x08000000 + len(e)]) == bytes(e_expect), 'region E at %#x after the copy and BSS zeroing (index entries excepted)' % E)
check(rw_exec + rw_len == E + len(e), 'copy ends exactly at the end of E')
check(zi_base + zi_len <= E, 'BSS (%#x..%#x) stays below E' % (zi_base, zi_base + zi_len))
heap = dw(0x0804b44c)
check(heap == dw(0x0807bfac) and heap >= E + len(e), 'heap base %#x is above E' % heap)
check(dw(0x0802dcf8) == 0x08b32b58 and iw(0x220030f4) == 0x08b32b58, 'BSS-end stack tops unchanged')
check(dram[rw_exec - 0x08000000:rw_exec - 0x08000000 + 4] == body[0xa1ab60:0xa1ab64], 'RW data start unchanged')

def target(at, w):
    o = w & 0xffffff
    if o & 0x800000: o -= 0x1000000
    return (at + 8 + 4 * o) & 0xffffffff
def br(at, name):
    w = dw(at)
    check((w & 0x0e000000) == 0x0a000000 and target(at, w) == sym[name], '%08x -> %s' % (at, name))
br(0x0804d7c4, 'e_libload_boot')
br(0x081ae29c, 'e_libload_task')
for at in (0x0802dd08, 0x0802dd34, 0x0802dd4c, 0x0802dd64, 0x0802dd7c, 0x082a71d0): br(at, 'e_cpu_exc')
br(0x0804328c, 'e_assert_print')
br(0x08026054, 'hk_exit')
br(0x0802606c, 'hk_exit')
br(0x0803ebac, 'hk_errtrail')
br(0x0802dd1c, 'e_swi')
check(heap - 0xc00 >= E + len(e), 'crash record %#x is above E' % (heap - 0xc00))
if full:
    br(0x0828a96c, 'e_flac_open')
    for at in (0x0804647c, 0x080464b0, 0x080464cc, 0x08092e40, 0x080931c0, 0x080c70b8, 0x08299628): br(at, 'e_art_find')
    for at in (0x081aa97c, 0x08264ab4, 0x08264b78, 0x08264d20, 0x082bd214): br(at, 'e_art_load')
    if dw(0x08109d88) == 0xeb028b0a and dw(0x0810b444) == 0xeb02855b:
        check(True, 'menu art pane lists stock (no --art-pane)')
        check(dw(0x08224c18) == 0xe59f102c, 'Now Playing row pane state stock (no --art-pane)')
    else:
        for at in (0x08109d88, 0x0810b444): br(at, 'e_showcase')
        br(0x08224c18, 'e_np_state')
    br(0x08045540, 'hk_imgid')
    for at in (0x082d6314, 0x082d9b14): br(at, 'e_lfn_batch')
    br(0x080da098, 'e_loc_db')
    for at in (0x0805f738, 0x080d448c): br(at, 'e_loc_set')
    for at in (0x08048288, 0x080cc33c, 0x080d7fa0, 0x080d7fdc, 0x080da17c): br(at, 'e_loc_path')
    for at, name in ((0x08133980, 'hk_gtl'), (0x0826d8e0, 'hk_seek'), (0x0826d5fc, 'hk_read'), (0x0828a5ac, 'hk_parse')):
        br(at, name)
    check(iw(0x22003824) == sym['flac_select_shim'], 'selector literal -> flac_select_shim')
    check(dw(0x0899bf4c) == sym['e_settings_action'] and dw(0x0899fe64) == sym['e_settings_action'], 'HandleAction slots -> e_settings_action')
    check(dw(0x080870f0) == 0xe3510005, 'media classifier lower-cases the whole extension')
    br(0x0806aa08, 'hk_unmount')
    check(dw(0x0898fedc) == sym['e_settings_provider'], 'settings provider slot -> e_settings_provider')
    br(0x082d75c8, 'hk_fatfree')
    br(0x082d61c0, 'hk_fatsearch')
    br(0x082d6080, 'hk_fatrun')
    br(0x082d6618, 'hk_fatinfo')
    check(dw(0x0804d4d0) == 0xeb0c4daf, 'I2C init at interrupt setup -> full reset 08360b94')
    br(0x0817ce28, 'hk_tick')
    br(0x081135a0, 'hk_uiinit')
    br(0x0827ee0c, 'hk_appinit')
    br(0x08143c04, 'hk_lcdinit')
    br(0x08143dd4, 'e_lcd_clear')
    check(dw(0x08142140) == 0xe3a02000, 'USB mass storage data view starts at LBA 0 (whole disk)')
    br(0x08261ef4, 'e_hl_bottom_r')
    br(0x08261f24, 'e_hl_edge_r')
    br(0x08270adc, 'e_refl_verts')
    br(0x08193444, 'e_batt_index')
    br(0x08298f80, 'e_song_artist')
    br(0x081df3ec, 'e_opbar_ink')
    br(0x081df414, 'e_opbar_ink')
    br(0x0815c5b8, 'e_np_present')
    for at in (0x08163a6c, 0x08137dc0): br(at, 'e_art_clear')
    br(0x08113470, 'hk_render')
    br(0x081a8444, 'hk_marq')
    br(0x081a7e70, 'e_marq_ink')
    if dw(0x08281824) == 0xeaf6adab and dw(0x0809f4d0) == 0xe92d41f0: check(True, 'screen copy-back stock (no --fb-damage)')
    else: br(0x08281824, 'e_fbcopy'); br(0x0809f4d0, 'hk_fbdmg')
    # the grey literals: all point into E except the ones kept on the OS globals
    import re as _re
    _mk = open(__file__.replace('checkpatch.py', 'mkpatch18.py')).read()
    def _tuple(name):
        m = _re.search(name + r' = \((.*?)\)', _mk, _re.S); return [int(x, 16) for x in _re.findall(r'0x([0-9a-f]{8})', m.group(1))]
    _lits, _keep, _tb, _tw = _tuple('GREY_LITERALS'), _tuple('KEEP'), _tuple('TEXT_BLACK'), _tuple('TEXT_WHITE')
    _pal = {0x089cc8c0: 'theme_pal_white', 0x089cc8c4: 'theme_pal_aa', 0x089cc8c8: 'theme_pal_7f', 0x089cc8cc: 'theme_pal_55', 0x089cc8d0: 'theme_pal_black'}
    _stock = {int(x, 16): v for x, v in _re.findall(r'(0x[0-9a-f]{8}): \'(theme_pal_\w+)\'', _mk)}
    _ok, _n = True, 0
    _stockimg = None
    for at in _lits:
        v = dw(at)
        if at in _keep: _ok = _ok and v in _pal; continue
        want = 'theme_text' if at in _tb else 'theme_white' if at in _tw else None
        _ok = _ok and (v == sym[want] if want else v in (sym[n] for n in _pal.values()))
        _n += 1
    check(_ok, '%d grey literals -> E (theme_pal_*/theme_text/theme_white), %d kept on the OS globals' % (_n, len(_keep)))
if full:
    # the Settings resources: the OS index must point into region E
    PKG_DATA, PKG = 0x08417928, 0x08400258
    for t, p in plan['types'].items():
        rec, tab, cnt, new = p['typerec'], sym[p['tab']], p['count'], p['new']
        ok = dw(rec + 12) + PKG == tab and dw(rec + 4) == cnt + len(new)
        for pos, n in [(cnt + k, n) for k, n in enumerate(new)] + [(n['pos'], n) for n in p['replaced']]:
            ent = tab + 12 * pos
            ok = ok and dw(ent) == n['id'] and dw(ent + 4) + PKG_DATA == sym[n['sym']] and dw(ent + 8) == n['size']
        # the rest of the copy is Apple's table, still in the image at its old place
        skip = set(n['pos'] for n in p['replaced'])
        for k in range(cnt):
            if k in skip: ok = ok and dw(tab + 12 * k) == dw(PKG + p['orig_index'] + 12 * k)
            else: ok = ok and dram[tab - 0x08000000 + 12 * k:tab - 0x08000000 + 12 * k + 12] == dram[PKG + p['orig_index'] - 0x08000000 + 12 * k:PKG + p['orig_index'] - 0x08000000 + 12 * k + 12]
        check(ok, '%-5s type record %d -> %d entries, index copy in E = Apple\'s + %d new, %d replaced -> E' % (t, cnt, dw(rec + 4), len(new), len(p['replaced'])))
    nm = plan['names']
    check(all(dw(l) == sym[nm['sym']] for l in nm['literals']) and all(dw(at) == new for at, old, new in nm['cmps']),
          'UI name table -> E, %d entries (%d new names)' % (nm['new_count'], nm['new_count'] - nm['count']))
    names_ok = all(dw(sym[nm['sym']] + 8 * k) == dw(nm['table'] + 8 * k) and dw(sym[nm['sym']] + 8 * k + 4) == dw(nm['table'] + 8 * k + 4) for k in range(nm['count']))
    check(names_ok, 'name table copy matches the original %d entries' % nm['count'])
gov = iw(0x22002a4c)
check(gov in (0xe3a00000, 0xe3a00001), 'governor word %08x (%s)' % (gov, 'L1 floor with the backlight on' if gov == 0xe3a00001 else 'stock, pins L0'))
md = dw(0x0802cf1c)
check(md in (0x22001f38, sym.get('e_mdelay')), 'mdelay veneer -> %s' % ('e_mdelay (sleep delays)' if md != 0x22001f38 else 'the timer spin (stock)'))
mm = iw(0x2200369c)
check(mm in (0x0802cdc8, sym.get('e_mmu_iram')), 'MMU setup helper -> %s' % ('e_mmu_iram (IRAM cached)' if mm != 0x0802cdc8 else 'stock'))
i2c = (dw(0x08360ce4), dw(0x08360ab0))
check(i2c in ((0x1bf330b1, 0x1bf3313e), (0xe1a00000, 0xe1a00000)), 'I2C bus gap: %s' % ('spin' if i2c[0] == 0xe1a00000 else 'stock 1 ms sleep'))
LIBC_VENEERS = ((0x0802cedc, 0x020), (0x0802cee4, 0x27c), (0x0802ceec, 0x2d8), (0x0802cef4, 0x2d4), (0x0802cefc, 0x020),
                (0x0802cf04, 0x0d4), (0x0802cf24, 0x188), (0x0802cf2c, 0x0d4), (0x0802d09c, 0x1f4))
lib_to = [sym['kfix_libc'] + o - 0x20 if fast_libc else 0x22000000 + o for _l, o in LIBC_VENEERS]
check(all(dw(l) == t for (l, _o), t in zip(LIBC_VENEERS, lib_to)), '%d libc veneers -> %s' % (len(LIBC_VENEERS), 'the cached copy in E (fast libc)' if fast_libc else 'IRAM (stock)'))
dc = [dw(a) for a in (0x08039d38, 0x08039d70, 0x0804e918)]
if dc == [0xe92d4070] * 3: check(True, 'D-cache range helpers stock')
else:
    for at, name in ((0x08039d38, 'hk_dc_cinv'), (0x08039d70, 'hk_dc_clean'), (0x0804e918, 'hk_dc_inv')): br(at, name)
def opt(label, sites):
    """optional hooks: all stock (first word given) or all to region E"""
    if all(dw(at) == w for at, w, _n in sites): check(True, '%s stock' % label)
    else:
        for at, _w, name in sites: br(at, name)
opt('heap cache', [(0x080e07a4, 0xe92d4070, 'hk_hcmalloc'), (0x080dca98, 0xe92d4070, 'hk_hcfree'),
    (0x081927c0, 0xebfffe6a, 'hc_alloc'), (0x08191f48, 0xeb000088, 'hc_alloc'), (0x08192434, 0xebffff4d, 'hc_alloc'), (0x081927ec, 0xebfffe5f, 'hc_alloc')])
opt('fast prims', [(0x08299e38, 0xe92d41f0, 'hk_aget'), (0x08299d48, 0xe92d41f0, 'hk_acopy'), (0x08299fcc, 0xe92d4070, 'hk_bsnext'),
                   (0x0829bcc0, 0xebfff8d2, 'hk_bsscan7'), (0x0828f9cc, 0xeb002993, 'hk_bsscansl')])
ras = (dw(0x0890d914), dw(0x0890d950))
check(ras in ((0x0890d8c4,) * 2, (sym.get('ft_grays_raster'),) * 2), 'smooth renderers raster_class -> %s' % ('FreeType 2.14 ftgrays' if ras[0] != 0x0890d8c4 else 'stock'))
KERN_RUNS = ((0x08070300, 0xeb008c11), (0x0807033c, 0xeb008c02), (0x0807ca1c, 0xeb005a4a), (0x0809cbf4, 0xebffd9d4),
             (0x0806c2c0, 0xeb009c21), (0x080e7960, 0xebfeae79), (0x080e7bb4, 0xebfeade4))
if dw(0x08983ea8) == 0x0828f948 and all(dw(at) == w for at, w in KERN_RUNS): check(True, 'kerning stock')
else:
    check(dw(0x08983ea8) == sym.get('e_kern_glyph'), 'TrueType glyph metrics slot 08983ea8 -> e_kern_glyph')
    for at, _w in KERN_RUNS: br(at, 'e_kern_run')
check(True, 'RGB565 dithering %s' % ('on' if dither else 'off'))
check(True, 'Now Playing cover accent %s' % ('on' if np_accent else 'off'))
print('ALL OK' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
