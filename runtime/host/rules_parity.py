#!/usr/bin/env python3
"""Checks theme_rules.c against theme_rules.py on every colour the theme can meet: all 85 COLR
values and every palette entry / pixel of the recoloured images, under each accent, in the
chrome, grey-image and background rules.  rules_parity.py OSOS.dec"""
import os, random, subprocess, sys, tempfile
import theme_rules as T
from uires import Package

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS = r'''
#include <stdio.h>
#include "theme_rules.h"
int main(void) {
    int r, g, b, ar, ag, ab, mode, gi, lmin, lmax; uint8_t o[3];
    while (scanf("%d %d %d %d %d %d %d %d %d %d", &r, &g, &b, &ar, &ag, &ab, &mode, &gi, &lmin, &lmax) == 10) {
        uint8_t acc[3] = { ar, ag, ab };
        if (mode == 0) tr_chrome_px(r, g, b, ar < 0 ? 0 : acc, gi, o);
        else tr_bg_px(tr_lum(r, g, b), lmin, lmax, o);
        printf("%d %d %d\n", o[0], o[1], o[2]);
    }
    return 0;
}
'''
tmp = tempfile.mkdtemp()
open(os.path.join(tmp, 'h.c'), 'w').write(HARNESS)
subprocess.run(['clang', '-O2', '-I', os.path.join(HERE, '..'), '-o', os.path.join(tmp, 'h'),
                os.path.join(tmp, 'h.c'), os.path.join(HERE, '..', 'theme_rules.c')], check=True)

pkg = Package(sys.argv[1])
colors = set((v >> 16 & 255, v >> 8 & 255, v & 255) for _, _, v in pkg.colr())
bm = {p for p, o, s in pkg.index('BMap')}
for n, pid in pkg.ids.items():
    if pid in bm and n.startswith(('StatusBar', 'System_', 'Background_', 'OptionBar_', 'Settings_MainMenu', 'DateTimePicker_', 'Media_Genius', 'GeniusMixes')):
        fmt, w, h, stride, pal, rows = pkg.bmap(pid)
        if fmt in (4, 8): continue
        if pal: colors.update((c >> 16 & 255, c >> 8 & 255, c & 255) for c in pal)
        else:
            for y in range(h):
                for x in range(w): colors.add(tuple(rows[y][4 * x:4 * x + 3]))
random.seed(1)
colors.update((random.randrange(256), random.randrange(256), random.randrange(256)) for _ in range(3000))
accents = [None, (0x0A, 0x84, 0xFF), (0xBF, 0x5A, 0xF2), (0xFF, 0x37, 0x5F), (0xFF, 0x45, 0x3A),
           (0xFF, 0x9F, 0x0A), (0xFF, 0xD6, 0x0A), (0x30, 0xD1, 0x58), (0x8E, 0x8E, 0x93)]
cases, expect = [], []
for (r, g, b) in sorted(colors):
    for acc in accents:
        for gi in (0, 1):
            cases.append((r, g, b) + (acc or (-1, -1, -1)) + (0, gi, 0, 0))
            expect.append(T.chrome_px(r, g, b, acc, gi))
    for lmin, lmax in ((0, 255), (80, 154), (200, 201)):
        cases.append((r, g, b, -1, -1, -1, 1, 0, lmin, lmax))
        expect.append(T.bg_px(T.lum(r, g, b), lmin, lmax))
inp = '\n'.join(' '.join(map(str, c)) for c in cases) + '\n'
out = subprocess.run([os.path.join(tmp, 'h')], input=inp, capture_output=True, text=True).stdout.split('\n')
bad = [(c, e, o) for c, e, o in zip(cases, expect, out) if ' '.join(map(str, e)) != o]
print('%d colours, %d cases, %d mismatches' % (len(colors), len(cases), len(bad)))
for c, e, o in bad[:10]: print('  case', c, 'python', e, 'c', o)
sys.exit(1 if bad else 0)
