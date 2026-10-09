#!/usr/bin/env python3
"""Add a folder of files to an existing FAT32 volume inside a disk image (the emulator's music volume:
4096-byte sectors). Creates PARENT\\NAME with a long name and copies every file of SRC_DIR into it.
Fails if PARENT\\NAME already exists.

    fat32add.py IMAGE VOLUME_OFFSET 'FLAC' 'Fonts' SRC_DIR"""
import os, struct, sys, time

img, vo, parent, name, src = sys.argv[1], int(sys.argv[2], 0), sys.argv[3], sys.argv[4], sys.argv[5]
f = open(img, 'r+b')
f.seek(vo); bs = f.read(512)
bps, spc, rsv, nfat = struct.unpack_from('<HBHB', bs, 11)
fatsz, root = struct.unpack_from('<I', bs, 36)[0], struct.unpack_from('<I', bs, 44)[0]
csize = bps * spc
fat_off = vo + rsv * bps
data_off = fat_off + nfat * fatsz * bps
f.seek(fat_off); fat = bytearray(f.read(fatsz * bps))
nclus = len(fat) // 4

def get(c): return struct.unpack_from('<I', fat, 4 * c)[0] & 0x0fffffff
def put(c, v): struct.pack_into('<I', fat, 4 * c, v)
def chain(c):
    while 2 <= c < 0x0ffffff8:
        yield c; c = get(c)
def coff(c): return data_off + (c - 2) * csize
hint = 2
def alloc(n):
    """n free clusters, chained (not necessarily contiguous)"""
    global hint
    got = []
    c = hint
    while len(got) < n:
        if c >= nclus: raise SystemExit('volume full')
        if get(c) == 0: got.append(c)
        c += 1
    hint = c
    for a, b in zip(got, got[1:]): put(a, b)
    put(got[-1], 0x0fffffff)
    return got

def dir_entries(c):
    out = []
    for cl in chain(c):
        f.seek(coff(cl)); d = f.read(csize)
        for i in range(0, csize, 32): out.append((coff(cl) + i, d[i:i + 32]))
    return out

def find(c, nm):
    lfn = ''
    for off, e in dir_entries(c):
        if e[0] == 0: break
        if e[0] == 0xe5: lfn = ''; continue
        if e[11] == 0x0f:
            part = (e[1:11] + e[14:26] + e[28:32]).decode('utf-16le', 'ignore').split('\0')[0].rstrip('￿')
            lfn = part + lfn if not e[0] & 0x40 else part; continue
        short = e[0:8].decode('latin-1').rstrip() + ('.' + e[8:11].decode('latin-1').rstrip() if e[8:11].strip() else '')
        n = lfn or short; lfn = ''
        if n.lower() == nm.lower() or short.lower() == nm.lower():
            return struct.unpack_from('<H', e, 20)[0] << 16 | struct.unpack_from('<H', e, 26)[0]
    return None

def dos_dt():
    t = time.localtime()
    return ((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2), ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday)

def entries(lname, sname, attr, clus, size):
    """LFN entries then the 8.3 entry"""
    s11 = sname.encode('latin-1')
    ck = 0
    for b in s11: ck = (((ck & 1) << 7) + (ck >> 1) + b) & 0xff
    u = lname.encode('utf-16le') + b'\0\0'
    chunks = [u[i:i + 26] for i in range(0, len(u), 26)]
    out = []
    for k in range(len(chunks), 0, -1):
        c = chunks[k - 1].ljust(26, b'\xff')
        e = bytes([k | (0x40 if k == len(chunks) else 0)]) + c[0:10] + bytes([0x0f, 0, ck]) + c[10:22] + b'\0\0' + c[22:26]
        out.append(e)
    tm, dt = dos_dt()
    out.append(s11 + bytes([attr, 0, 0]) + struct.pack('<HHHHHHHI', tm, dt, dt, clus >> 16, tm, dt, clus & 0xffff, size))
    return out

def add_entries(dirc, ents):
    """write ents into the first run of free slots of the directory at cluster dirc (extends it)"""
    slots = dir_entries(dirc)
    run = []
    for off, e in slots:
        if e[0] in (0, 0xe5): run.append(off)
        else: run = []
        if len(run) == len(ents): break
    if len(run) < len(ents):
        last = list(chain(dirc))[-1]
        new = alloc(1)[0]; put(last, new)
        f.seek(coff(new)); f.write(b'\0' * csize)
        return add_entries(dirc, ents)
    for off, e in zip(run, ents):
        f.seek(off); f.write(e)

nshort = [0]
def short(lname, ext=''):
    nshort[0] += 1
    base = ''.join(ch for ch in lname.upper() if ch.isalnum())[:6] or 'F'
    return '%-8s%-3s' % ('%s~%d' % (base, nshort[0]), ext[:3].upper())

pc = root
for p in parent.split('\\'):
    pc = find(pc, p)
    if pc is None: raise SystemExit('no folder ' + p)
if find(pc, name) is not None: raise SystemExit(name + ' exists')
dc = alloc(1)[0]
f.seek(coff(dc)); f.write(b'\0' * csize)
tm, dt = dos_dt()
dot = b'.          ' + bytes([0x10, 0, 0]) + struct.pack('<HHHHHHHI', tm, dt, dt, dc >> 16, tm, dt, dc & 0xffff, 0)
dotdot = b'..         ' + bytes([0x10, 0, 0]) + struct.pack('<HHHHHHHI', tm, dt, dt, (pc if pc != root else 0) >> 16, tm, dt, (pc if pc != root else 0) & 0xffff, 0)
f.seek(coff(dc)); f.write(dot + dotdot)
add_entries(pc, entries(name, short(name), 0x10, dc, 0))
for fn in sorted(os.listdir(src)):
    path = os.path.join(src, fn)
    if fn.startswith('.') or not os.path.isfile(path): continue
    data = open(path, 'rb').read()
    cl = alloc(max(1, -(-len(data) // csize)))
    for i, c in enumerate(cl):
        f.seek(coff(c)); f.write(data[i * csize:(i + 1) * csize].ljust(csize, b'\0'))
    ext = fn.rsplit('.', 1)[1] if '.' in fn else ''
    add_entries(dc, entries(fn, short(fn.rsplit('.', 1)[0], ext), 0x20, cl[0], len(data)))
    print('added', fn, len(data))
for k in range(nfat):
    f.seek(fat_off + k * fatsz * bps); f.write(fat)
# FSInfo free count is now stale: mark it unknown
fsi = struct.unpack_from('<H', bs, 48)[0]
f.seek(vo + fsi * bps + 488); f.write(struct.pack('<II', 0xffffffff, 0xffffffff))
f.close()
