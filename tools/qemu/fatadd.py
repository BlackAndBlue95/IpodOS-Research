#!/usr/bin/env python3
"""Copy a host file or folder into a FAT32 image (any sector size, long names), creating the
folders on the way. Existing entries of the same name are left alone (the copy stops with an
error), so use it on a disk clone.
    fatadd.py IMAGE HOST_PATH 'TV Shows/Show' [OFFSET_BYTES]   -> IMAGE:\\TV Shows\\Show\\<basename of HOST_PATH>"""
import os, struct, sys, time

img, src, dest = sys.argv[1], sys.argv[2], sys.argv[3]
base = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0
f = open(img, 'r+b')
def rd(off, n): f.seek(base + off); return f.read(n)
def wr(off, b): f.seek(base + off); f.write(b)
bs = rd(0, 512)
bps, spc, res, nf = struct.unpack_from('<HBHB', bs, 11)
fatsz, root = struct.unpack_from('<I', bs, 36)[0], struct.unpack_from('<I', bs, 44)[0]
ds = (res + nf * fatsz) * bps; cl = bps * spc
nclus = (struct.unpack_from('<I', bs, 32)[0] * bps - ds) // cl
fat = bytearray(rd(res * bps, fatsz * bps))
def nxt(c): return struct.unpack_from('<I', fat, 4 * c)[0] & 0x0fffffff
def setf(c, v): struct.pack_into('<I', fat, 4 * c, (struct.unpack_from('<I', fat, 4 * c)[0] & 0xf0000000) | v)
hint = 2
def alloc(n):
    """n free clusters chained together, the first returned"""
    global hint
    got, c = [], hint
    while len(got) < n:
        if c >= nclus + 2: sys.exit('volume full')
        if nxt(c) == 0: got.append(c)
        c += 1
    hint = c
    for a, b in zip(got, got[1:]): setf(a, b)
    setf(got[-1], 0x0fffffff)
    return got
def chain(c):
    out = []
    while 2 <= c < 0x0ffffff8: out.append(c); c = nxt(c)
    return out
def coff(c): return ds + (c - 2) * cl

def dir_entries(c):
    """(slot offset, raw 32 bytes, name, attr, first cluster) of every used entry"""
    lfn = []
    for x in chain(c):
        d = rd(coff(x), cl)
        for i in range(0, cl, 32):
            e = d[i:i + 32]
            if e[0] == 0: return
            if e[0] == 0xe5: lfn = []; continue
            if e[11] == 0x0f: lfn.insert(0, e[1:11] + e[14:26] + e[28:32]); continue
            name = b''.join(lfn).decode('utf-16-le', 'replace').split('\0')[0] if lfn else \
                (e[0:8].decode('latin1').rstrip() + ('.' + e[8:11].decode('latin1').rstrip() if e[8:11].strip() else ''))
            lfn = []
            yield coff(x) + i, e, name, e[11], struct.unpack_from('<H', e, 20)[0] << 16 | struct.unpack_from('<H', e, 26)[0]

def free_slots(c, n):
    """offsets of n consecutive free 32-byte slots in directory c (extended when full)"""
    run = []
    clusters = chain(c)
    for x in clusters:
        d = rd(coff(x), cl)
        for i in range(0, cl, 32):
            if d[i] in (0, 0xe5):
                run.append(coff(x) + i)
                if len(run) == n: return run
            else:
                run = []
    new = alloc(1)[0]
    setf(clusters[-1], new)
    wr(coff(new), bytes(cl))
    return run + [coff(new) + 32 * k for k in range(n - len(run))]

BAD = set(b'"*+,./:;<=>?[\\]|')
def short_name(name, taken):
    """the 8.3 name, and whether a long name is needed beside it"""
    stem, ext = name.rsplit('.', 1) if '.' in name.strip('.') else (name, '')
    clean = lambda s: bytes(c for c in s.upper().encode('ascii', 'replace') if 0x20 < c < 0x7f and c not in BAD)
    s, x = clean(stem), clean(ext)
    if name == name.upper() and s == stem.encode() and x == ext.encode() and 0 < len(s) <= 8 and len(x) <= 3:
        sn = s.ljust(8) + x.ljust(3)
        if sn not in taken: return sn, False
    x = x[:3]
    for k in range(1, 1000):
        tail = b'~%d' % k
        sn = (s[:8 - len(tail)] + tail).ljust(8) + x.ljust(3)
        if sn not in taken: return sn, True
    sys.exit('no short name for ' + name)

def lfn_entries(name, sn):
    ck = 0
    for b in sn: ck = (((ck & 1) << 7) | (ck >> 1)) + b & 0xff
    u = name.encode('utf-16-le') + b'\0\0'
    u += b'\xff' * (-len(u) % 26)
    parts = [u[i:i + 26] for i in range(0, len(u), 26)]
    out = []
    for k, p in enumerate(parts, 1):
        e = bytearray(32)
        e[0] = k | (0x40 if k == len(parts) else 0)
        e[1:11] = p[0:10]; e[11] = 0x0f; e[13] = ck; e[14:26] = p[10:22]; e[28:32] = p[22:26]
        out.append(bytes(e))
    return out[::-1]

def stamp():
    t = time.localtime()
    return (t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec // 2), ((t.tm_year - 1980) << 9 | t.tm_mon << 5 | t.tm_mday)

def add_entry(dirc, name, attr, first, size):
    ents = list(dir_entries(dirc))
    if any(n.lower() == name.lower() for _, _, n, _, _ in ents): sys.exit('already there: ' + name)
    sn, need_lfn = short_name(name, {e[0:11] for _, e, _, _, _ in ents})
    recs = lfn_entries(name, sn) if need_lfn else []
    e = bytearray(32)
    e[0:11] = sn; e[11] = attr
    tm, dt = stamp()
    struct.pack_into('<HHHHHHHI', e, 14, tm, dt, dt, first >> 16, tm, dt, first & 0xffff, size)
    recs.append(bytes(e))
    for off, r in zip(free_slots(dirc, len(recs)), recs): wr(off, r)

def find(dirc, name):
    for _, _, n, attr, fc in dir_entries(dirc):
        if n.lower() == name.lower(): return fc, attr
    return None, None

def mkdir(parent, name):
    c, attr = find(parent, name)
    if c is not None:
        if not attr & 0x10: sys.exit(name + ' is a file')
        return c
    c = alloc(1)[0]
    wr(coff(c), bytes(cl))
    add_entry(parent, name, 0x10, c, 0)
    for nm, fc in ((b'.          ', c), (b'..         ', 0 if parent == root else parent)):
        e = bytearray(32); e[0:11] = nm; e[11] = 0x10
        struct.pack_into('<HH', e, 20, fc >> 16, 0); struct.pack_into('<H', e, 26, fc & 0xffff)
        wr(coff(c) + (0 if nm[1] == 0x20 else 32), bytes(e))
    return c

def add_file(dirc, path):
    data = open(path, 'rb').read()
    first = 0
    if data:
        cs = alloc((len(data) + cl - 1) // cl)
        for k, c in enumerate(cs):
            wr(coff(c), data[k * cl:(k + 1) * cl].ljust(cl, b'\0'))
        first = cs[0]
    add_entry(dirc, os.path.basename(path), 0x20, first, len(data))

def add_tree(dirc, path):
    if os.path.isdir(path):
        d = mkdir(dirc, os.path.basename(path))
        for n in sorted(os.listdir(path)):
            if not n.startswith('._') and n != '.DS_Store': add_tree(d, os.path.join(path, n))
    else:
        add_file(dirc, path)

c = root
for part in [p for p in dest.replace('\\', '/').split('/') if p]: c = mkdir(c, part)
add_tree(c, src.rstrip('/'))
for k in range(nf): wr(res * bps + k * fatsz * bps, fat)
f.close()
print('added %s to %s' % (os.path.basename(src.rstrip('/')), dest))
