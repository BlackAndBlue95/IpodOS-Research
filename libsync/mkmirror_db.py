#!/usr/bin/env python3
"""Synthetic test tree from an iTunesDB: for every FLAC entry a sparse .flac under DST/Music with
the entry's path, size and mtime, a STREAMINFO (sample rate, sample count) and a Vorbis comment
block carrying the entry's tags. Lets the sync be tested without the iPod (mkmirror.py needs it).
usage: mkmirror_db.py ITUNESDB DST_ROOT"""
import os, struct, sys, unicodedata

MAC_EPOCH = 2082844800
db, dst = sys.argv[1], sys.argv[2]
raw = open(db, 'rb').read()
u32 = lambda o: struct.unpack_from('<I', raw, o)[0]
u64 = lambda o: struct.unpack_from('<Q', raw, o)[0]

def mhods(p, hl, n):
    out = {}
    q = p + hl
    for _ in range(n):
        l, t = u32(q + 8), u32(q + 12)
        if t in (1, 2, 3, 4, 5, 6, 12, 22):
            out.setdefault(t, raw[q + 40:q + 40 + u32(q + 28)].decode('utf-16le'))
        q += l
    return out

def block(kind, body, last):
    return bytes([kind | (0x80 if last else 0)]) + len(body).to_bytes(3, 'big') + body

n = 0
o = u32(4)
while o < len(raw):
    hl, tl, typ = u32(o + 4), u32(o + 8), u32(o + 12)
    if typ == 1:
        b = o + hl; p = b + u32(b + 4)
        for _ in range(u32(b + 8)):
            thl, ttl = u32(p + 4), u32(p + 8)
            if u32(p + 24) == 0x464C4143:
                m = mhods(p, thl, u32(p + 12))
                loc = m[2]
                assert loc.startswith(':Music:'), loc
                rel = unicodedata.normalize('NFC', loc[1:].replace(':', '/'))
                path = os.path.join(dst, rel)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                size, mtime = u32(p + 0x24), u32(p + 0x20) - MAC_EPOCH
                sr = int(struct.unpack_from('<f', raw, p + 0x88)[0]) or (u32(p + 0x3c) >> 16)
                ns = u64(p + 0xbc)
                tags = []
                for t, name in ((1, 'TITLE'), (4, 'ARTIST'), (22, 'ALBUMARTIST'), (12, 'COMPOSER'), (3, 'ALBUM'), (5, 'GENRE')):
                    if m.get(t): tags.append('%s=%s' % (name, m[t]))
                if u32(p + 0x2c): tags.append('TRACKNUMBER=%d' % u32(p + 0x2c))
                if u32(p + 0x30): tags.append('TRACKTOTAL=%d' % u32(p + 0x30))
                if u32(p + 0x5c): tags.append('DISCNUMBER=%d' % u32(p + 0x5c))
                if u32(p + 0x34): tags.append('DATE=%d' % u32(p + 0x34))
                si = struct.pack('>HH', 4096, 4096) + b'\0\0\0' * 2 + ((sr << 44) | (1 << 41) | (15 << 36) | ns).to_bytes(8, 'big') + b'\0' * 16
                vendor = b'mkmirror_db'
                vc = struct.pack('<I', len(vendor)) + vendor + struct.pack('<I', len(tags))
                for t in tags:
                    e = t.encode('utf-8'); vc += struct.pack('<I', len(e)) + e
                head = b'fLaC' + block(0, si, False) + block(4, vc, True)
                with open(path, 'wb') as f:
                    f.write(head)
                    f.truncate(max(size, len(head)))
                os.utime(path, (mtime, mtime))
                n += 1
            p += ttl
    o += tl
os.makedirs(os.path.join(dst, 'iPod_Control/iTunes'), exist_ok=True)
print(n, 'files written')
