#!/usr/bin/env python3
"""Our files for the rsrc volume (ModernFonts, Icons, Theme/Package.bin), for ospatch/fwpart.py rsrc.

    rsrc_tree.py PARTITION.bin SFICONS_DIR OSOS.dec      # writes SFICONS_DIR/rsrc (gitignored)"""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'ospatch'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fwpart import entries
from rsrcvol import Fat16
from icon_sets import stock_index, PKG_DATA, HDR, DRAM_DELTA

part, sfdir, osos = sys.argv[1], sys.argv[2], sys.argv[3]
oimg, ix = stock_index(osos)
first, end = min(o for o, s in ix.values()), max(o + s for o, s in ix.values())
a = HDR + PKG_DATA + first - DRAM_DELTA
os.makedirs(os.path.join(sfdir, 'rsrc', 'Resources', 'Theme'), exist_ok=True)
open(os.path.join(sfdir, 'rsrc', 'Resources', 'Theme', 'Package.bin'), 'wb').write(
    struct.pack('<4sIII', b'FPKG', first, end - first, 0) + oimg[a:a + end - first])
print('Package.bin: Apple bitmaps %#x..%#x (%d bytes)' % (first, end, end - first))
img = open(part, 'rb').read()
e = next(e for e in entries(img) if e and e[1] == b'rsrc')
start = e[3] + 0x1000 + e[6]
vol = Fat16(img[start:start + e[4]])
fonts = vol.lookup(vol.lookup(0, 'Resources')[3], 'Fonts')[3]
out = os.path.join(sfdir, 'rsrc', 'Resources', 'ModernFonts')
os.makedirs(out, exist_ok=True)
for ent in vol.entries(fonts):
    name = ent[0]
    if name in ('.', '..') or ent[2] & 0x10: continue
    sf = os.path.join(sfdir, 'fonts', name)
    data = open(sf, 'rb').read() if os.path.exists(sf) else vol.read('Resources/Fonts/' + name)
    open(os.path.join(out, name), 'wb').write(data)
    print('%-28s %8d  %s' % (name, len(data), 'SF Compact' if os.path.exists(sf) else "Apple's"))
icons = os.path.join(sfdir, 'rsrc', 'Resources', 'Icons')
for n in ('Classic.bin', 'Modern.bin'):
    assert os.path.exists(os.path.join(icons, n)), 'run tools/sf/icon_sets.py (or gen_sficons.py) first: ' + n
