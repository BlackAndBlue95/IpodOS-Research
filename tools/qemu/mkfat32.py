#!/usr/bin/env python3
"""Build a FAT32 image from a folder, laid out like the iPod's card: 4096-byte sectors, 4 sectors
per cluster, long file names, files and folders in contiguous clusters.

    mkfat32.py SRC_DIR OUT.img SIZE_MB"""
import os, struct, sys, time

src, out, size_mb = sys.argv[1], sys.argv[2], int(sys.argv[3])
BPS, SPC, RES, NFATS = 4096, 4, 8, 2
CL = BPS * SPC
tot = size_mb * 1024 * 1024 // BPS
fatsz = 1
while True:
    nclus = (tot - RES - NFATS * fatsz) // SPC
    need = (nclus + 2) * 4 // BPS + 1
    if need <= fatsz: break
    fatsz = need
data_start = RES + NFATS * fatsz            # first data sector (cluster 2)
fat = [0] * (nclus + 2)
fat[0], fat[1] = 0x0ffffff8, 0x0fffffff
img = open(out, 'wb'); img.truncate(tot * BPS)
next_free = 2

def alloc(n):
    global next_free
    first = next_free
    for c in range(first, first + n):
        fat[c] = c + 1
    fat[first + n - 1] = 0x0fffffff
    next_free += n
    return first

def write_clusters(first, data):
    img.seek((data_start + (first - 2) * SPC) * BPS)
    img.write(data)

def dos_time(t):
    lt = time.localtime(t)
    return ((lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec // 2),
            ((lt.tm_year - 1980) << 9) | (lt.tm_mon << 5) | lt.tm_mday)

def short_names(names):
    used, res = set(), {}
    for n in names:
        base, ext = os.path.splitext(n)
        b = ''.join(c for c in base.upper() if c.isalnum())[:8] or 'X'
        e = ''.join(c for c in ext[1:].upper() if c.isalnum())[:3]
        fits = n.upper() == n and len(base) <= 8 and len(ext) <= 4 and b == base.upper() and e == ext[1:].upper()
        if fits and (b, e) not in used:
            res[n] = (b, e, False); used.add((b, e)); continue
        k = 1
        while True:
            tail = '~%d' % k
            cand = (b[:8 - len(tail)] + tail, e)
            if cand not in used: break
            k += 1
        res[n] = (cand[0], cand[1], True); used.add(cand)
    return res

def lfn_entries(name, sname):
    raw = (sname[0].ljust(8) + sname[1].ljust(3)).encode('ascii')
    ck = 0
    for b in raw: ck = (((ck & 1) << 7) + (ck >> 1) + b) & 0xff
    u = name.encode('utf-16-le') + b'\0\0'
    while len(u) % 26: u += b'\xff\xff'
    parts = [u[i:i + 26] for i in range(0, len(u), 26)]
    ents = []
    for i, p in enumerate(parts):
        seq = i + 1 | (0x40 if i == len(parts) - 1 else 0)
        e = bytes([seq]) + p[0:10] + bytes([0x0f, 0, ck]) + p[10:22] + b'\0\0' + p[22:26]
        ents.append(e)
    return list(reversed(ents))

def entry(sname, attr, clus, size, mtime):
    t, d = dos_time(mtime)
    raw = (sname[0].ljust(8) + sname[1].ljust(3)).encode('ascii')
    return raw + struct.pack('<BBBHHHHHHHI', attr, 0, 0, t, d, d, clus >> 16, t, d, clus & 0xffff, size)

# pass 1: every folder's entry count gives its size; folders get clusters before any file
dirs = {}
def scan(path):
    names = sorted(n for n in os.listdir(path) if not n.startswith('.'))
    sn = short_names(names)
    n_ent = 2 if path != src else 0
    for n in names:
        n_ent += 1 + (len(lfn_entries(n, sn[n][:2])) if sn[n][2] else 0)
    dirs[path] = {'names': names, 'sn': sn, 'nclus': max(1, (n_ent * 32 + CL - 1) // CL)}
    for n in names:
        if os.path.isdir(os.path.join(path, n)): scan(os.path.join(path, n))
scan(src)
for p in dirs: dirs[p]['clus'] = alloc(dirs[p]['nclus'])

# pass 2: files, then each folder's entries
for p, info in dirs.items():
    ents = []
    if p != src:
        parent = os.path.dirname(p)
        pc = 0 if parent == src else dirs[parent]['clus']
        ents.append(entry(('.', ''), 0x10, info['clus'], 0, os.path.getmtime(p)))
        ents.append(entry(('..', ''), 0x10, pc, 0, os.path.getmtime(p)))
    for n in info['names']:
        fp = os.path.join(p, n)
        sname = info['sn'][n][:2]
        if info['sn'][n][2]: ents += lfn_entries(n, sname)
        if os.path.isdir(fp):
            ents.append(entry(sname, 0x10, dirs[fp]['clus'], 0, os.path.getmtime(fp)))
        else:
            data = open(fp, 'rb').read()
            c = alloc((len(data) + CL - 1) // CL) if data else 0
            if data: write_clusters(c, data)
            ents.append(entry(sname, 0x20, c, len(data), os.path.getmtime(fp)))
    blob = b''.join(ents)
    write_clusters(info['clus'], blob.ljust(info['nclus'] * CL, b'\0'))

# boot sector, FSInfo, backups, FATs
bs = bytearray(BPS)
bs[0:3] = b'\xeb\x58\x90'; bs[3:11] = b'MSWIN4.1'
struct.pack_into('<HBHBHHBHHHII', bs, 11, BPS, SPC, RES, NFATS, 0, 0, 0xf8, 0, 63, 255, 0, tot)
struct.pack_into('<IHHIHH', bs, 36, fatsz, 0, 0, dirs[src]['clus'], 1, 6)
bs[64] = 0x80; bs[66] = 0x29; struct.pack_into('<I', bs, 67, 0x1234abcd)
bs[71:82] = b'IPOD       '; bs[82:90] = b'FAT32   '; bs[510:512] = b'\x55\xaa'
fsi = bytearray(BPS)
struct.pack_into('<I', fsi, 0, 0x41615252); struct.pack_into('<I', fsi, 484, 0x61417272)
struct.pack_into('<II', fsi, 488, nclus - (next_free - 2), next_free); struct.pack_into('<I', fsi, 508, 0xaa550000)
for sec, b in ((0, bs), (1, fsi), (6, bs), (7, fsi)):
    img.seek(sec * BPS); img.write(b)
fatb = struct.pack('<%dI' % len(fat), *fat).ljust(fatsz * BPS, b'\0')
for k in range(NFATS):
    img.seek((RES + k * fatsz) * BPS); img.write(fatb)
img.close()
print('%s: %d MB, %d clusters, %d used, %d folders' % (out, size_mb, nclus, next_free - 2, len(dirs)))
