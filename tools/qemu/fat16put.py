#!/usr/bin/env python3
"""Overwrite a file of the same size in a FAT16 volume inside a disk image (the firmware partition's
resource volume in the emulator's disk), following its cluster chain.
    fat16put.py IMAGE VOLUME_OFFSET 'Resources/Fonts/Helvetica.ttf' NEWFILE"""
import struct, sys
img, vo, path, src = sys.argv[1], int(sys.argv[2], 0), sys.argv[3], sys.argv[4]
f = open(img, 'r+b')
f.seek(vo); bs = f.read(512)
bps, spc, rsv, nfat, nroot, _, _, spf = struct.unpack_from('<HBHBHHBH', bs, 11)
fat_off = vo + rsv * bps
root_off = fat_off + nfat * spf * bps
data_off = root_off + nroot * 32
csize = bps * spc
f.seek(fat_off); fat = f.read(spf * bps)
def chain(c):
    while 2 <= c < 0xfff8:
        yield c; c = struct.unpack_from('<H', fat, 2 * c)[0]
def read_dir(c):
    if c is None: f.seek(root_off); return f.read(nroot * 32), [root_off + 32 * i for i in range(nroot)]
    data, offs = b'', []
    for cl in chain(c):
        o = data_off + (cl - 2) * csize; f.seek(o); data += f.read(csize); offs += [o + 32 * i for i in range(csize // 32)]
    return data, offs
def lookup(d, name):
    data, offs = d; lfn = ''
    for i in range(0, len(data), 32):
        e = data[i:i + 32]
        if e[0] == 0: break
        if e[0] == 0xe5: lfn = ''; continue
        if e[11] == 0x0f:
            part = (e[1:11] + e[14:26] + e[28:32]).decode('utf-16le', 'ignore').split('\0')[0]
            lfn = part + lfn if e[0] & 0x40 == 0 else part; continue
        short = (e[0:8].decode('latin-1').rstrip() + ('.' + e[8:11].decode('latin-1').rstrip() if e[8:11].strip() else ''))
        nm = lfn or short; lfn = ''
        if nm.lower() == name.lower() or short.lower() == name.lower():
            return struct.unpack_from('<H', e, 26)[0], struct.unpack_from('<I', e, 28)[0]
    raise SystemExit('not found: ' + name)
parts = path.split('/'); c = None
for p in parts[:-1]: c, _ = lookup(read_dir(c), p)
c, size = lookup(read_dir(c), parts[-1])
new = open(src, 'rb').read()
assert len(new) == size, 'size differs: %d vs %d' % (len(new), size)
pos = 0
for cl in chain(c):
    f.seek(data_off + (cl - 2) * csize); f.write(new[pos:pos + csize]); pos += csize
    if pos >= size: break
print('wrote', size, 'bytes')
