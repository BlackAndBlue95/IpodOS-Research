#!/usr/bin/env python3
"""Pixels that differ between two screenshots (RGB PNGs from screens.py), below the status bar so
the clock does not count.
    pngdiff.py A.png B.png [FIRST_ROW]   (default 22)"""
import struct, sys, zlib

def rows(f):
    d = open(f, 'rb').read(); i = 8; idat = b''; w = h = 0
    while i < len(d):
        n = struct.unpack('>I', d[i:i + 4])[0]; t = d[i + 4:i + 8]; c = d[i + 8:i + 8 + n]
        if t == b'IHDR': w, h = struct.unpack('>II', c[:8])
        if t == b'IDAT': idat += c
        i += 12 + n
    raw = zlib.decompress(idat); st = w * 3 + 1
    return [raw[y * st + 1:(y + 1) * st] for y in range(h)]

a, b = rows(sys.argv[1]), rows(sys.argv[2])
top = int(sys.argv[3]) if len(sys.argv) > 3 else 22
print(sum(1 for y in range(top, min(len(a), len(b))) for x in range(0, len(a[y]), 3) if a[y][x:x + 3] != b[y][x:x + 3]))
