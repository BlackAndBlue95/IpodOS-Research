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
if full:
    br(0x0828a96c, 'e_flac_open')
    for at in (0x0804647c, 0x080464b0, 0x080464cc, 0x08092e40, 0x080931c0, 0x080c70b8, 0x08299628): br(at, 'e_art_find')
    for at in (0x081aa97c, 0x08264ab4, 0x08264b78, 0x08264d20, 0x082bd214): br(at, 'e_art_load')
    for at in (0x082d6314, 0x082d9b14): br(at, 'e_lfn_batch')
    br(0x080da098, 'e_loc_db')
    for at in (0x0805f738, 0x080d448c): br(at, 'e_loc_set')
    for at in (0x08048288, 0x080cc33c, 0x080d7fa0, 0x080d7fdc, 0x080da17c): br(at, 'e_loc_path')
    for at, name in ((0x08133980, 'hk_gtl'), (0x0826d8e0, 'hk_seek'), (0x0826d5fc, 'hk_read'), (0x0828a5ac, 'hk_parse')):
        br(at, name)
    check(iw(0x22003824) == sym['flac_select_shim'], 'selector literal -> flac_select_shim')
    check(dw(0x0899bf4c) == sym['e_settings_action'] and dw(0x0899fe64) == sym['e_settings_action'], 'HandleAction slots -> e_settings_action')
    check(dw(0x0898fedc) == sym['e_settings_provider'], 'settings provider slot -> e_settings_provider')
    br(0x0817ce28, 'hk_tick')
    br(0x081135a0, 'hk_uiinit')
    br(0x0827ee0c, 'hk_appinit')
    br(0x08143c04, 'hk_lcdinit')
    br(0x08143dd4, 'e_lcd_clear')
    check(dw(0x08142140) == 0xe3a02000, 'USB mass storage data view starts at LBA 0 (whole disk)')
    br(0x08261ef4, 'e_hl_bottom_r')
    br(0x08261f24, 'e_hl_edge_r')
    br(0x08270980, 'e_refl_gate')
    for at in (0x08163a6c, 0x08137dc0): br(at, 'e_art_clear')
    br(0x08113470, 'hk_render')
    br(0x081a8444, 'hk_marq')
    br(0x081a7e70, 'e_marq_ink')
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
print('ALL OK' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
