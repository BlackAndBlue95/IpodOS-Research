#!/usr/bin/env python3
"""Print a file from a FAT32 image (long names, any sector size).
    fatcat.py IMAGE 'FLAC\\rtlog.txt' [OFFSET_BYTES]"""
import struct, sys

img, path = sys.argv[1], sys.argv[2]
base = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
f = open(img, 'rb')
def rd(off, n): f.seek(base + off); return f.read(n)
bs = rd(0, 512)
bps, spc, res, nf = struct.unpack_from('<HBHB', bs, 11)
fatsz, root = struct.unpack_from('<I', bs, 36)[0], struct.unpack_from('<I', bs, 44)[0]
ds = (res + nf * fatsz) * bps; cl = bps * spc
def nxt(c): return struct.unpack('<I', rd(res * bps + 4 * c, 4))[0] & 0x0fffffff
def chain(c):
    out = []
    while 2 <= c < 0x0ffffff8: out.append(c); c = nxt(c)
    return out
def read(c, size=None):
    d = b''.join(rd(ds + (x - 2) * cl, cl) for x in chain(c))
    return d if size is None else d[:size]
def entries(c):
    d = read(c); lfn = []
    for i in range(0, len(d), 32):
        e = d[i:i + 32]
        if e[0] == 0: break
        if e[0] == 0xe5: lfn = []; continue
        if e[11] == 0x0f:
            lfn.insert(0, e[1:11] + e[14:26] + e[28:32]); continue
        name = b''.join(lfn).decode('utf-16-le', 'replace').split('\0')[0] if lfn else \
            (e[0:8].decode('latin1').rstrip() + ('.' + e[8:11].decode('latin1').rstrip() if e[8:11].strip() else ''))
        lfn = []
        yield name, e[11], struct.unpack_from('<H', e, 20)[0] << 16 | struct.unpack_from('<H', e, 26)[0], struct.unpack_from('<I', e, 28)[0]
c = root; size = None
for part in path.split('\\'):
    for name, attr, fc, sz in entries(c):
        if name.lower() == part.lower(): c, size = fc, sz; break
    else:
        sys.exit('not found: ' + part)
sys.stdout.buffer.write(read(c, size))
