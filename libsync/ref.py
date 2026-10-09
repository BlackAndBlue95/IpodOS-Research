#!/usr/bin/env python3
"""Reference: flacsync-mac-current.py's main() with its randomness and clock pinned, so libsync --full
can be compared byte for byte. Reads --in, writes --out, scans ROOT/Music.
usage: ref.py ROOT --in DB --out DB --now N --seed HEX"""
import hashlib, importlib.util, os, struct, sys

FLACSYNC = os.path.expanduser('~/Downloads/ipod-flac-source/flacsync/flacsync-mac-current.py')
spec = importlib.util.spec_from_file_location('flacsync', FLACSYNC)
fs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fs)

a = sys.argv[1:]
root = a[0]
arg = lambda k: a[a.index(k) + 1]
dbin, dbout, now, seed = arg('--in'), arg('--out'), int(arg('--now')), bytes.fromhex(arg('--seed'))

ctr = [0]
def getrandbits(k):
    assert k == 64
    d = hashlib.sha1(seed + struct.pack('<I', ctr[0])).digest()
    ctr[0] += 1
    return int.from_bytes(d[:8], 'little')
fs.random.getrandbits = getrandbits

# main() from flacsync-mac-current.py, paths and time parametrised
raw = open(dbin, 'rb').read()
if fs.h58(fs.FWID, raw) != raw[0x58:0x6c]: sys.exit('iTunesDB checksum does not match this iPod, not touching it')
db = fs.DB(raw)
db.tmpl_hdr = bytes(db.tracks[0].hdr)
keep = [t for t in db.tracks if t.u(24) != 0x464C4143]
removed = len(db.tracks) - len(keep)
db.tracks = keep
used = {t.u(0x120) for t in keep}
db.albums = [x for x in db.albums if fs.u32(x, 16) in used]
files = []
for dp, dn, fn in os.walk(os.path.join(root, 'Music')):
    dn.sort()
    for f in sorted(fn):
        if f.lower().endswith('.flac') and not f.startswith('._'): files.append(os.path.join(dp, f))
added = 0
for fp in files:
    r = fs.read_flac(fp)
    if not r: print('skip', fp); continue
    (sr, ns), t = r
    size = os.path.getsize(fp); ms = int(ns * 1000 // sr)
    rel = '/' + os.path.relpath(fp, root)
    fs.add_track(db, rel, size, ms, sr, int(size * 8 / max(ms, 1)), t.get('TITLE') or os.path.splitext(os.path.basename(fp))[0],
                 artist=t.get('ARTIST', ''), album=t.get('ALBUM', ''), albumartist=t.get('ALBUMARTIST', ''),
                 genre=t.get('GENRE', ''), track_nr=fs.num(t.get('TRACKNUMBER')),
                 track_total=fs.num(t.get('TRACKTOTAL')) or fs.num((t.get('TRACKNUMBER', '') + '/').split('/')[1]),
                 disc_nr=fs.num(t.get('DISCNUMBER')), year=fs.num(t.get('DATE', '')[:4]), composer=t.get('COMPOSER', ''),
                 samplecount=ns, now=now)
    added += 1
out = fs.h58sign(fs.FWID, db.write())
open(dbout, 'wb').write(out)
print('removed %d old FLAC entries, added %d, total %d tracks, %d bytes' % (removed, added, len(db.tracks), len(out)))
