#!/usr/bin/env python3
"""Sparse test copy of an iPod's Music folder: same tree, sizes and mtimes, names in NFC (as the
FAT disk holds them), real STREAMINFO and Vorbis comment blocks, everything else left as holes.
usage: mkmirror.py SRC_ROOT DST_ROOT"""
import os, shutil, sys, unicodedata

src, dst = sys.argv[1], sys.argv[2]
n = 0
for dp, dn, fn in os.walk(os.path.join(src, 'Music')):
    rel = os.path.relpath(dp, src)
    out = os.path.join(dst, unicodedata.normalize('NFC', rel))
    os.makedirs(out, exist_ok=True)
    for f in fn:
        if f.startswith('._') or not f.lower().endswith('.flac'): continue
        sp = os.path.join(dp, f)
        st = os.stat(sp)
        with open(sp, 'rb') as fi:
            head = fi.read(4)
            blocks = []
            p = 4
            if head == b'fLaC':
                while True:
                    h = fi.read(4)
                    if len(h) < 4: break
                    l = int.from_bytes(h[1:4], 'big')
                    body = fi.read(l) if h[0] & 0x7f in (0, 4) else None
                    if body is None: fi.seek(l, 1)
                    blocks.append((p, h, body))
                    p += 4 + l
                    if h[0] & 0x80: break
        dp2 = os.path.join(out, unicodedata.normalize('NFC', f))
        with open(dp2, 'wb') as fo:
            fo.write(head)
            for off, h, body in blocks:
                fo.seek(off); fo.write(h)
                if body: fo.write(body)
            fo.truncate(st.st_size)
        os.utime(dp2, (st.st_atime, st.st_mtime))
        n += 1
for d in ('iPod_Control/iTunes',):
    os.makedirs(os.path.join(dst, d), exist_ok=True)
print(n, 'files mirrored')
