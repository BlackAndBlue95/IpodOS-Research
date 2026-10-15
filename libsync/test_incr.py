#!/usr/bin/env python3
"""Incremental-mode tests for libsync on a sparse mirror (see mkmirror.py).
usage: test_incr.py MIRROR WORKDIR START_DB"""
import hashlib, importlib.util, os, shutil, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('fs', os.path.expanduser('~/Downloads/ipod-flac-source/flacsync/flacsync-mac-current.py'))
fs = importlib.util.module_from_spec(spec); spec.loader.exec_module(fs)
u32 = fs.u32
mirror, work, start_db = sys.argv[1:4]
HOST = os.path.join(HERE, 'libsync-host')
fails = 0

def check(cond, what):
    global fails
    print(('  ok   ' if cond else '  FAIL ') + what)
    if not cond: fails += 1

def run(*args):
    out = subprocess.run([HOST, root] + list(args), capture_output=True, text=True).stdout.strip()
    print('  ' + out)
    d = dict(kv.split('=', 1) for kv in out.split(' msg=')[0].split())
    d['msg'] = out.split(' msg=', 1)[1] if ' msg=' in out else ''
    return d

def load(path):
    raw = open(path, 'rb').read()
    check(fs.h58(fs.FWID, raw) == raw[0x58:0x6c], 'signature valid (%d bytes)' % len(raw))
    return raw, fs.DB(raw)

def by_loc(db):
    return {t.s(2): t for t in db.tracks}

def validate(raw, db):
    """structure the iPod relies on: lengths, indexes are permutations, albums exist, items match tracks"""
    n = len(db.tracks)
    aids = {u32(a, 16) for a in db.albums}
    check(all(t.u(0x120) in aids for t in db.tracks), 'every track has its album entry')
    check(len(aids) == len(db.albums) and all(any(t.u(0x120) == a for t in db.tracks) for a in aids), 'no unused or duplicate albums')
    ids = [t.id for t in db.tracks]
    check(len(set(ids)) == n, 'track ids unique')
    check(ids == sorted(ids), 'track list in ascending id order')
    check(len({bytes(t.dbid) for t in db.tracks}) == n, 'dbids unique')
    o = u32(raw, 4)
    while o < len(raw):
        hl, tl, typ = u32(raw, o + 4), u32(raw, o + 8), u32(raw, o + 12)
        if typ in (2, 3):
            s = o + hl; p = s + u32(raw, s + 4)
            for _ in range(u32(raw, s + 8)):
                phl, ptl = u32(raw, p + 4), u32(raw, p + 8)
                if raw[p + 20] == 1:
                    q = p + phl; ok = True
                    for _ in range(u32(raw, p + 12)):
                        ml = u32(raw, q + 8)
                        if u32(raw, q + 12) == 52:
                            cnt = u32(raw, q + 28)
                            idx = struct.unpack_from('<%dI' % cnt, raw, q + 72)
                            ok &= sorted(idx) == list(range(n))
                        q += ml
                    items = [u32(raw, q + k * 0x78 + 24) for k in range(u32(raw, p + 16))]
                    check(ok and items == ids, 'master playlist type %d: indexes and items match the %d tracks' % (typ, n))
                p += ptl
        o += tl

def same_tags(a, b):
    fields = [0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x5c, 0x88, 0xbc, 0xc0]
    return all(a.s(k) == b.s(k) for k in (1, 2, 3, 4, 5, 6, 12, 22)) and all(a.hdr[f:f + 4] == b.hdr[f:f + 4] for f in fields)

def set_title(path, old, new):
    """rewrite a TITLE in place (same length), like a tag editor using padding"""
    d = bytearray(open(path, 'rb').read(1 << 20))
    i = d.find(b'TITLE=' + old.encode())
    assert i > 0 and len(new.encode()) == len(old.encode())
    with open(path, 'r+b') as f:
        f.seek(i + 6); f.write(new.encode())

# setup
root = os.path.join(work, 'root')
shutil.rmtree(work, ignore_errors=True); os.makedirs(work)
subprocess.run(['cp', '-cR', mirror, root], check=True)          # APFS clone, instant
dbp = os.path.join(root, 'iPod_Control/iTunes/iTunesDB')
shutil.copy(start_db, dbp)
start_raw, start = load(dbp)
NOW = int(time.time())
NFLAC = sum(f.lower().endswith('.flac') for dp, dn, fn in os.walk(os.path.join(root, 'Music')) for f in fn)
print('mirror has %d .flac files' % NFLAC)

def verify(path, **kw):
    args = [HOST, '--verify', path] + sum(([k, str(v)] for k, v in kw.items()), [])
    out = subprocess.run(args, capture_output=True, text=True).stdout.strip()
    check(out.startswith('ok'), 'ls_verify: ' + out)

def item_ids(raw):
    """every playlist item id in the database, by section type"""
    out = {}
    o = u32(raw, 4)
    while o < len(raw):
        hl, tl, typ = u32(raw, o + 4), u32(raw, o + 8), u32(raw, o + 12)
        if typ in (2, 3):
            s = o + hl; p = s + u32(raw, s + 4); lst = []
            for _ in range(u32(raw, s + 8)):
                phl, ptl = u32(raw, p + 4), u32(raw, p + 8)
                q = p + phl
                for _ in range(u32(raw, p + 12)): q += u32(raw, q + 8)
                for _ in range(u32(raw, p + 16)):
                    lst.append(u32(raw, q + 20)); q += u32(raw, q + 8)
            out[typ] = lst
        o += tl
    return out

def marker_ok(db):
    return all(bytes(t.hdr[0x258:0x25c]) == b'FLSY' and t.hdr[0x25c] == 4 for t in db.tracks if t.u(24) == 0x464C4143)

def loc_last(db):
    return all(t.mhods[-1][0] == 2 for t in db.tracks if t.u(24) == 0x464C4143) if hasattr(db.tracks[0], 'mhods') else True

print('0. the starting database is refused by the verifier (older sync: no marker)')
out = subprocess.run([HOST, '--verify', dbp], capture_output=True, text=True).stdout.strip()
check(out.startswith('BAD verify:'), 'refused: ' + out)

print('1. first run: migration, every entry rebuilt once with its history kept')
r = run('--now', str(NOW))
raw1, db1 = load(dbp)
validate(raw1, db1)
verify(dbp)
check(r['wrote'] == '1' and r['verified'] == '1' and r['migrated'] == str(NFLAC) and r['kept'] == '0' and r['removed'] == '0' and r['added'] == '0',
      'all %d FLACs migrated, none added or removed, image verified' % NFLAC)
old = by_loc(start); new = by_loc(db1)
kept_ident = all(bytes(new[k].dbid) == bytes(old[k].dbid) and new[k].id == old[k].id for k in new if k in old)
check(kept_ident, 'every existing entry kept its id and dbid')
check(all(new[k].u(0x68) == old[k].u(0x68) and new[k].u(0x50) == old[k].u(0x50) and new[k].hdr[0x1f] == old[k].hdr[0x1f] for k in new if k in old), 'date added, play count and rating kept')
check(marker_ok(db1), 'every FLAC entry carries the v4 marker')
check(all(t.hdr[0xb2] in (1, 2) and t.u(0x100) == 1 and t.u(0x1e0) == 0x7f and t.u(0x1f4) == t.id + 1 for t in db1.tracks if t.u(24) == 0x464C4143), "Finder's stamp fields present")
check(all(u32(a, 16) != 0 for a in db1.albums), 'album ids nonzero')
check(os.path.exists(dbp + '.old') and open(dbp + '.old', 'rb').read() == start_raw, 'previous database kept as iTunesDB.old')
apple_old = [t for t in start.tracks if t.u(24) != 0x464C4143]; apple_new = [t for t in db1.tracks if t.u(24) != 0x464C4143]
check(len(apple_old) == len(apple_new) and all(bytes(a.hdr) == bytes(b.hdr) for a, b in zip(apple_old, apple_new)), "Apple's own entries untouched")
old_items = item_ids(start_raw); new_items = item_ids(raw1)
check(all(len(set(v)) == len(v) for v in new_items.values()), 'item ids unique per section')

print('2. second run, nothing changed')
r = run('--now', str(NOW + 60))
check(r['wrote'] == '0' and r['kept'] == str(NFLAC) and r['migrated'] == '0', 'no write')
check(open(dbp, 'rb').read() == raw1, 'database untouched')

print('3. album removed, album added, one title edited')
music = os.path.join(root, 'Music')
albums = sorted(d for d in os.listdir(music) if os.path.isdir(os.path.join(music, d)))
gone, src = albums[3], albums[5]
ngone = sum(f.lower().endswith('.flac') for f in os.listdir(os.path.join(music, gone)))
shutil.rmtree(os.path.join(music, gone))
dup = os.path.join(music, 'zz New Album Copy')
subprocess.run(['cp', '-cR', os.path.join(music, src), dup], check=True)
nadd = sum(f.lower().endswith('.flac') for f in os.listdir(dup))
edit_dir = os.path.join(music, albums[0])
edit = os.path.join(edit_dir, sorted(f for f in os.listdir(edit_dir) if f.endswith('.flac'))[0])
edit_loc = ':' + os.path.relpath(edit, root).replace('/', ':')
t_before = new[edit_loc]
oldt = t_before.s(1); newt = oldt[:-1] + ('X' if oldt[-1] != 'X' else 'Y')
set_title(edit, oldt, newt)
os.utime(edit, (NOW + 100, NOW + 100))
r = run('--now', str(NOW + 120))
raw3, db3 = load(dbp)
validate(raw3, db3)
check(r['added'] == str(nadd) and r['removed'] == str(ngone) and r['updated'] == '1', 'counts: +%d -%d ~1' % (nadd, ngone))
t3 = by_loc(db3)
check(t3[edit_loc].s(1) == newt and bytes(t3[edit_loc].dbid) == bytes(t_before.dbid) and t3[edit_loc].id == t_before.id, 'edited title read, id and dbid kept')
check(not any(k.startswith(':Music:' + gone + ':') for k in t3), 'removed album gone')
dupt = [t for k, t in t3.items() if k.startswith(':Music:zz New Album Copy:')]
srct = [t for k, t in t3.items() if k.startswith(':Music:' + src + ':')]
check(len(dupt) == nadd and {t.u(0x120) for t in dupt} == {t.u(0x120) for t in srct}, 'copied album joins the existing album entry (same tags)')
check(min(t.id for t in dupt) > max(t.id for t in start.tracks), 'new ids above every old id')
items3 = item_ids(raw3); new_item_ids = set(items3[3]) - set(new_items[3])
check(new_item_ids and min(new_item_ids) > max(max(v) for v in new_items.values()), 'new playlist item ids above every existing item id in any section')
verify(dbp)

print('4. play counts merged when writing')
pc_db = db3
n = len(pc_db.tracks)
entries = b''.join(struct.pack('<7I', i % 3, (fs._mac_time(NOW) if i % 3 else 0), 0, 60 if i == 2 else 0, 0, i % 2, 0) for i in range(n))
open(os.path.join(root, 'iPod_Control/iTunes/Play Counts'), 'wb').write(b'mhdp' + struct.pack('<III', 0x60, 0x1c, n) + b'\0' * 0x50 + entries)
os.utime(edit, (NOW + 200, NOW + 200))     # force a write
r = run('--now', str(NOW + 220))
raw4, db4 = load(dbp)
validate(raw4, db4)
t4 = by_loc(db4)
ok = all(t4[t.s(2)].u(0x50) == t.u(0x50) + i % 3 and t4[t.s(2)].u(0x9c) == t.u(0x9c) + i % 2 for i, t in enumerate(pc_db.tracks) if t.s(2) in t4)
check(r['playcounts'] == '1' and ok, 'play and skip counts added to the right tracks')
check(t4[pc_db.tracks[2].s(2)].hdr[0x1f] == 60, 'rating from Play Counts applied')
check(not os.path.exists(os.path.join(root, 'iPod_Control/iTunes/Play Counts')), 'Play Counts removed after merging')

print('5. interrupted replace is finished on the next run')
os.rename(dbp, dbp + '.new')
r = run('--now', str(NOW + 300))
check(os.path.exists(dbp) and not os.path.exists(dbp + '.new') and r['rc'] == '0' and r['wrote'] == '0', 'iTunesDB.new put in place, nothing else to do')

print('6. wrong FireWire GUID: database left alone')
os.utime(edit, (NOW + 400, NOW + 400))
before = open(dbp, 'rb').read()
r = run('--now', str(NOW + 420), '--fwid', '000A27002108AFDF')
check(r['rc'] == '-1' and open(dbp, 'rb').read() == before, 'refused, file untouched')

print('7. a changed file that can\'t be read keeps its entry, no rewrite')
before = open(dbp, 'rb').read()
os.chmod(edit, 0)
os.utime(edit, (NOW + 500, NOW + 500))
r = run('--now', str(NOW + 520))
os.chmod(edit, 0o644)
check(r['rc'] == '0' and r['wrote'] == '0' and r['skipped'] == '1' and open(dbp, 'rb').read() == before, 'entry kept, database untouched')

print('8. a folder that can\'t be opened stops the sync')
locked = os.path.join(music, albums[1])
os.chmod(locked, 0)
shutil.rmtree(dup)                      # a real change that would otherwise be written
r = run('--now', str(NOW + 620))
os.chmod(locked, 0o755)
check(r['rc'] == '-1' and r['msg'].startswith("can't open /Music/" + albums[1]), 'refused, names the folder')
check(open(dbp, 'rb').read() == before, 'database untouched while a folder is unreadable')
r = run('--now', str(NOW + 700))
raw8, db8 = load(dbp)
validate(raw8, db8)
check(r['removed'] == str(nadd) and r['wrote'] == '1', 'once readable again, the deleted copy is removed')

def our_playlists(raw):
    """{section type: [(name, [track ids], timestamp)]} for playlists with our id tag, plus the master's item count"""
    out = {}
    o = u32(raw, 4)
    while o < len(raw):
        hl, tl, typ = u32(raw, o + 4), u32(raw, o + 8), u32(raw, o + 12)
        if typ in (2, 3):
            s = o + hl; p = s + u32(raw, s + 4); lst = []
            for _ in range(u32(raw, s + 8)):
                phl, ptl = u32(raw, p + 4), u32(raw, p + 8)
                if raw[p + 20] != 1 and raw[p + 0x1c + 7] == 0xe5:
                    q = p + phl; name = ''
                    for _ in range(u32(raw, p + 12)):
                        if u32(raw, q + 12) == 1: name = raw[q + 40:q + 40 + u32(raw, q + 28)].decode('utf-16le')
                        q += u32(raw, q + 8)
                    ids = [u32(raw, q + k * 0x78 + 24) for k in range(u32(raw, p + 16))]
                    lst.append((name, ids, u32(raw, p + 0x18)))
                p += ptl
            out[typ] = (lst, u32(raw, s + 8))
        o += tl
    return out

def section5(raw):
    o = u32(raw, 4)
    while o < len(raw):
        hl, tl, typ = u32(raw, o + 4), u32(raw, o + 8), u32(raw, o + 12)
        if typ == 5: return raw[o:o + tl]
        o += tl

print('9. a Rockbox playlist in /Playlists becomes an iPod playlist')
pldir = os.path.join(root, 'Playlists'); os.makedirs(pldir, exist_ok=True)
tracks = [t for t in db8.tracks if t.u(24) == 0x464C4143][:40]
chosen = [tracks[5], tracks[17], tracks[30]]
lines = ['/<HDD0>' + t.s(2).replace(':', '/') for t in chosen] + ['/<HDD0>/Music/Nope/missing.flac']
m3u = os.path.join(pldir, 'Road Trip.m3u8')
open(m3u, 'wb').write(b'\xef\xbb\xbf' + '\r\n'.join(lines).encode('utf-8') + b'\r\n')
os.utime(m3u, (NOW + 800, NOW + 800))
sec5_before = section5(open(dbp, 'rb').read())
r = run('--now', str(NOW + 820))
raw9, db9 = load(dbp)
validate(raw9, db9)
pls = our_playlists(raw9)
check(r['wrote'] == '1' and r['playlists'] == '1', 'written with one playlist')
check(all(len(pls[t][0]) == 1 and pls[t][0][0][0] == 'Road Trip' and pls[t][0][0][1] == [x.id for x in chosen] for t in (2, 3)),
      'playlist "Road Trip" with the 3 matching tracks in both playlist sections (bad line skipped)')
check(all(pls[t][1] == 2 for t in (2, 3)), 'playlist list count = master + ours')
check(section5(raw9) == sec5_before, "Finder's special playlists (section 5) untouched")

print('10. unchanged playlist, unchanged library: no write')
r = run('--now', str(NOW + 900))
check(r['wrote'] == '0', 'no write')

print('11. edited playlist (new mtime) is rebuilt')
open(m3u, 'ab').write(('/<HDD0>' + tracks[2].s(2).replace(':', '/') + '\r\n').encode('utf-8'))
os.utime(m3u, (NOW + 1000, NOW + 1000))
r = run('--now', str(NOW + 1020))
raw11, db11 = load(dbp)
pls = our_playlists(raw11)
check(r['wrote'] == '1' and pls[3][0][0][1] == [x.id for x in chosen] + [tracks[2].id], '4 tracks now')
check(pls[3][0][0][2] == fs._mac_time(NOW + 1000), "playlist timestamp = the file's mtime")

print('12. deleted playlist file: playlist removed')
os.remove(m3u)
r = run('--now', str(NOW + 1100))
raw12, db12 = load(dbp)
validate(raw12, db12)
pls = our_playlists(raw12)
check(r['wrote'] == '1' and all(pls[t][0] == [] and pls[t][1] == 1 for t in (2, 3)), 'gone, counts back to the master only')

print('13. an album folder renamed: entries carried over, nothing added or removed')
music_albums = sorted(d for d in os.listdir(music) if os.path.isdir(os.path.join(music, d)))
ren = music_albums[2]
before12 = by_loc(db12)
nren = sum(f.lower().endswith('.flac') for f in os.listdir(os.path.join(music, ren)))
os.rename(os.path.join(music, ren), os.path.join(music, ren + ' (2020 remaster)'))
r = run('--now', str(NOW + 1200))
raw13, db13 = load(dbp)
validate(raw13, db13); verify(dbp)
t13 = by_loc(db13)
check(r['renamed'] == str(nren) and r['added'] == '0' and r['removed'] == '0' and r['wrote'] == '1', '%d renamed, 0 new, 0 gone' % nren)
moved = {k: t for k, t in t13.items() if k.startswith(':Music:' + ren + ' (2020 remaster):')}
ok = True
for k, t in moved.items():
    oldk = k.replace(ren + ' (2020 remaster)', ren)
    o = before12[oldk]
    ok &= t.id == o.id and bytes(t.dbid) == bytes(o.dbid) and t.u(0x68) == o.u(0x68) and t.u(0x50) == o.u(0x50) and t.hdr[0x1f] == o.hdr[0x1f]
check(len(moved) == nren and ok, 'ids, dbids, dates added, play counts and ratings follow the files')

print('14. one file renamed inside an album (tags unchanged): carried over by title/album/artist')
d14 = os.path.join(music, ren + ' (2020 remaster)')
f14 = sorted(f for f in os.listdir(d14) if f.lower().endswith('.flac'))[0]
os.rename(os.path.join(d14, f14), os.path.join(d14, 'renamed track.flac'))
r = run('--now', str(NOW + 1300))
raw14, db14 = load(dbp); validate(raw14, db14); verify(dbp)
t14 = by_loc(db14)
k14 = ':Music:' + ren + ' (2020 remaster):renamed track.flac'
oldk = ':Music:' + ren + ' (2020 remaster):' + f14
check(r['renamed'] == '1' and r['added'] == '0' and r['removed'] == '0' and k14 in t14 and t14[k14].id == t13[oldk].id, 'renamed file keeps its entry')

print('15. a copy under a new name with a different title is a new track, the old entry goes')
d15 = os.path.join(music, music_albums[4])
f15 = sorted(f for f in os.listdir(d15) if f.lower().endswith('.flac'))[0]
p15 = os.path.join(d15, f15)
t_old = t14[':Music:' + music_albums[4] + ':' + f15]
oldt = t_old.s(1); newt = oldt[:-1] + ('Q' if oldt[-1] != 'Q' else 'W')
os.rename(p15, os.path.join(d15, 'other name.flac'))
set_title(os.path.join(d15, 'other name.flac'), oldt, newt)
r = run('--now', str(NOW + 1400))
raw15, db15 = load(dbp); validate(raw15, db15); verify(dbp)
t15 = by_loc(db15)
check(r['renamed'] == '0' and r['added'] == '1' and r['removed'] == '1' and t15[':Music:' + music_albums[4] + ':other name.flac'].id != t_old.id, 'not matched: +1 -1, new id')

print('16. every injected fault is refused, database untouched, image kept for a look')
os.makedirs(os.path.join(root, 'FLAC'), exist_ok=True)
before = open(dbp, 'rb').read()
for fault in range(1, 7):
    os.utime(p15.replace(f15, 'other name.flac'), (NOW + 1500 + fault, NOW + 1500 + fault))
    rej = os.path.join(root, 'FLAC', 'iTunesDB.rejected')
    if os.path.exists(rej): os.remove(rej)
    env = dict(os.environ, LS_FAULT=str(fault))
    out = subprocess.run([HOST, root, '--now', str(NOW + 1520 + fault)], capture_output=True, text=True, env=env).stdout.strip()
    msg = out.split(' msg=', 1)[1] if ' msg=' in out else ''
    check(out.startswith('rc=-1') and msg.startswith('verify:') and open(dbp, 'rb').read() == before and os.path.exists(rej), 'fault %d: %s' % (fault, msg))
r = run('--now', str(NOW + 1600))
check(r['wrote'] == '1' and r['verified'] == '1', 'the same change goes through without the fault')

print('17. an empty /Music is refused, a missing one too')
before = open(dbp, 'rb').read()
os.rename(music, music + '.away'); os.makedirs(music)
r = run('--now', str(NOW + 1700))
check(r['rc'] == '-1' and r['msg'].startswith('no .flac files found') and open(dbp, 'rb').read() == before, 'empty: ' + r['msg'])
os.rmdir(music)
r = run('--now', str(NOW + 1710))
check(r['rc'] == '-1' and r['msg'].startswith("can't open /Music") and open(dbp, 'rb').read() == before, 'missing: ' + r['msg'])
os.rename(music + '.away', music)
r = run('--now', str(NOW + 1720))
check(r['rc'] == '0' and r['wrote'] == '0', 'back: nothing to do')

print('18. the album callback names every folder with FLACs and its first file')
out = subprocess.run([HOST, root, '--dry', '--albums', '--now', str(NOW + 1800)], capture_output=True, text=True).stdout
albums_cb = [l.split(' | ') for l in out.splitlines() if l.startswith('album ')]
folders = sorted({os.path.dirname(t.s(2).replace(':', '/')) for t in db15.tracks if t.u(24) == 0x464C4143})
check(sorted(a[0][6:] for a in albums_cb) == folders and all(a[1].startswith(a[0][6:] + '/') for a in albums_cb), '%d albums reported' % len(albums_cb))
# the listing handed over: entry count, and the art source chosen by the runtime's rule
ok = True
for a in albums_cb:
    d = os.path.join(root, a[0][6:].lstrip('/')); ents = sorted(x for x in os.listdir(d) if x not in ('.', '..'))
    n, kind, src, size, mtime = int(a[2]), int(a[3]), a[4], int(a[5]), int(a[6])
    names = ['cover.jpg', 'folder.jpg', 'front.jpg', 'cover.jpeg', 'albumart.jpg']
    want = next(((k + 1, e) for k, nm in enumerate(names) for e in ents if e.lower() == nm and not os.path.isdir(os.path.join(d, e))), None)
    if not want: want = (6, os.path.basename(a[1]))
    st = os.stat(os.path.join(d, want[1]))
    ok &= n == len(ents) and kind == want[0] and src == want[1] and size == st.st_size and mtime == int(st.st_mtime)
check(ok, 'listing counts and art sources match the rule (cover file by name, else the first .flac)')

def music_app(raw):
    """the database as the Music app rewrites it on connect: FLAC entry markers zeroed, 0x1e0 set
    to 0x81, the comment record moved after the file type, signed again"""
    d = bytearray(raw)
    o = 0
    while True:
        o = d.find(b'mhit', o)
        if o < 0: break
        hl, tl, nm = u32(d, o + 4), u32(d, o + 8), u32(d, o + 12)
        if hl == 0x270 and u32(d, o + 24) == 0x464C4143:
            d[o + 0x258:o + 0x268] = bytes(16)
            struct.pack_into('<I', d, o + 0x1e0, 0x81)
            recs, q = [], o + hl
            for _ in range(nm):
                ml = u32(d, q + 8); recs.append((u32(d, q + 12), bytes(d[q:q + ml]))); q += ml
            types = [t for t, _ in recs]
            if 8 in types and 6 in types and types.index(8) < types.index(6):
                c = recs.pop(types.index(8)); recs.insert([t for t, _ in recs].index(6) + 1, c)
                d[o + hl:q] = b''.join(r for _, r in recs)
        o += 4
    return fs.h58sign(fs.FWID, bytes(d))

print('19. the Music app rewrites the database: unchanged entries stay as it wrote them')
state = os.path.join(root, 'FLAC', 'libsync.state')
check(os.path.exists(state) and open(state, 'rb').read()[:6] == b'FLSY\x04\x00', 'state file written for v4')
ours = open(dbp, 'rb').read()
app = music_app(ours)
open(dbp, 'wb').write(app)
r = run('--now', str(NOW + 1900))
NF19 = int(r['flacs'])
check(r['rc'] == '0' and r['wrote'] == '0' and r['musicapp'] == str(NF19) and r['migrated'] == '0' and r['updated'] == '0',
      'all %d kept, nothing read or written' % NF19)
check(open(dbp, 'rb').read() == app, 'the database is left as the Music app wrote it')
db19 = fs.DB(app); t19 = by_loc(db19)
edit19 = os.path.join(edit_dir, sorted(f for f in os.listdir(edit_dir) if f.endswith('.flac'))[1])
loc19 = ':' + os.path.relpath(edit19, root).replace('/', ':')
o19 = t19[loc19].s(1); n19 = o19[:-1] + ('X' if o19[-1] != 'X' else 'Y')
set_title(edit19, o19, n19)
os.utime(edit19, (NOW + 1950, NOW + 1950))
r = run('--now', str(NOW + 2000))
raw19, db19b = load(dbp); validate(raw19, db19b); verify(dbp)
check(r['wrote'] == '1' and r['updated'] == '1' and r['musicapp'] == str(NF19 - 1) and r['migrated'] == '0',
      'one changed file rebuilt, the other %d kept and stamped' % (NF19 - 1))
check(marker_ok(db19b), 'every FLAC entry carries the v4 marker again')
a19, b19 = by_loc(db19), by_loc(db19b)
check(all(bytes(b19[k].hdr[:0x258]) == bytes(a19[k].hdr[:0x258]) and bytes(b19[k].hdr[0x268:]) == bytes(a19[k].hdr[0x268:]) and b19[k].mhods == a19[k].mhods
          for k in a19 if k != loc19 and k in b19 and a19[k].u(24) == 0x464C4143),
      'kept entries are the Music app\'s bytes but for the marker')
check(b19[loc19].s(1) == n19, 'the changed file has its new title')
os.remove(state)
app = music_app(open(dbp, 'rb').read())
open(dbp, 'wb').write(app)
r = run('--now', str(NOW + 2100))
check(r['wrote'] == '1' and r['migrated'] == str(NF19) and r['musicapp'] == '0', 'without the state file every entry is rebuilt, as before')
check(os.path.exists(state), 'and the state file is written again')

def box(typ, body): return struct.pack('>I', 8 + len(body)) + typ + body

def mp4(path, secs, rate=44100, video=None):
    """the boxes read_mp4 reads: mvhd (600 ticks a second), a sound track's mp4a entry and, with
    video = (codec, width, height, profile, level), a video track's entry with its avcC"""
    mvhd = box(b'mvhd', bytes(12) + struct.pack('>II', 600, 600 * secs) + bytes(80))
    mp4a = struct.pack('>I', 36) + b'mp4a' + bytes(6) + struct.pack('>H', 1) + bytes(8) + struct.pack('>HHHHI', 2, 16, 0, 0, rate << 16)
    stbl = box(b'stbl', box(b'stsd', struct.pack('>II', 0, 1) + mp4a))
    traks = box(b'trak', box(b'mdia', box(b'minf', stbl)))
    if video:
        codec, w, h, prof, lvl = video
        avcc = box(b'avcC', bytes([1, prof, 0, lvl]) + bytes(4))
        ent = codec + bytes(6) + struct.pack('>H', 1) + bytes(16) + struct.pack('>HH', w, h) + bytes(50) + avcc
        vstbl = box(b'stbl', box(b'stsd', struct.pack('>II', 0, 1) + struct.pack('>I', 4 + len(ent)) + ent))
        traks = box(b'trak', box(b'mdia', box(b'minf', vstbl))) + traks
    moov = box(b'moov', mvhd + traks)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'wb').write(box(b'ftyp', b'M4V \0\0\0\1') + moov + box(b'mdat', bytes(4096)))

M4V = 0x4D345620
def videos(db): return {t.s(2): t for t in db.tracks if t.u(24) == M4V}

print('20. TV episodes under /TV Shows')
tv = os.path.join(root, 'TV Shows')
EPS = {'Show A/Season 2/S02E03 - Pilot.m4v': (90, 2, 3, 'Pilot', 'Show A'),
       'Show A/Season 2/S02E04.M4V': (95, 2, 4, 'Episode 4', 'Show A'),   # an 8.3 name as FAT lists it
       'Show B/Intro.mp4': (60, 1, 1, 'Intro', 'Show B'),
       'Show B/Second.mp4': (61, 1, 2, 'Second', 'Show B')}
for rel, (secs, *_) in EPS.items(): mp4(os.path.join(tv, rel), secs)
r = run('--now', str(NOW + 2200))
raw20, db20 = load(dbp); validate(raw20, db20); verify(dbp)
check(r['wrote'] == '1' and r['videos'] == '4' and r['added'] == '4' and r['removed'] == '0', 'four episodes added')
v20 = videos(db20)
ok = len(v20) == 4
for rel, (secs, season, ep, title, show) in EPS.items():
    t = v20.get(':TV Shows:' + (rel[:-4] + rel[-4:].lower()).replace('/', ':'))   # extension in lower case
    ok &= bool(t) and t.u(40) == secs * 1000 and t.u(0xd4) == season and t.u(0xd8) == ep and t.u(0xd0) == 0x40 \
        and t.hdr[0xa6] == 0 and t.hdr[0xaa] == 0 and t.hdr[0xb1] == 1 and t.s(1) == title and t.s(4) == show \
        and t.s(3) == '%s, Season %d' % (show, season) and t.s(19) == show and t.s(20) == 'S%02dE%02d' % (season, ep) \
        and struct.unpack('<f', bytes(t.hdr[0x88:0x8c]))[0] == 44100
check(ok, 'show, season, episode (from SnnEnn, else the place in the folder), title, length, rate and flags')
def nflac(db): return sum(t.u(24) == 0x464C4143 for t in db.tracks)
check(nflac(db20) == int(r['flacs']) and int(r['kept']) == int(r['flacs']), 'the FLAC entries are all still there')
r = run('--now', str(NOW + 2300))
check(r['rc'] == '0' and r['wrote'] == '0' and r['kept'] == str(int(r['flacs']) + 4), 'again: nothing to do')

# an episode with remember-position (0xa6) set, as R66 wrote them
loc = ':TV Shows:Show B:Intro.mp4'
d = bytearray(raw20); o = 0; t = v20[loc]
while (o := d.find(b'mhit', o)) >= 0:
    if u32(d, o + 16) == t.id: d[o + 0xa6] = 1; break
    o += 4
open(dbp, 'wb').write(fs.h58sign(fs.FWID, bytes(d)))
r = run('--now', str(NOW + 2400))
raw20b, db20b = load(dbp); validate(raw20b, db20b); verify(dbp)
t2 = videos(db20b)[loc]
check(r['wrote'] == '1' and r['migrated'] == '1' and r['added'] == '0', 'that entry rebuilt once')
check(t2.hdr[0xa6] == 0 and t2.id == t.id and bytes(t2.dbid) == bytes(t.dbid), 'with 0xa6 clear, id and dbid kept')

print('21. the Music app rewrites the database with episodes in it')
d = bytearray(open(dbp, 'rb').read()); o = 0
while (o := d.find(b'mhit', o)) >= 0:
    if u32(d, o + 4) == 0x270 and u32(d, o + 24) == M4V: d[o + 0x258:o + 0x268] = bytes(16)
    o += 4
app = music_app(fs.h58sign(fs.FWID, bytes(d)))
open(dbp, 'wb').write(app)
r = run('--now', str(NOW + 2500))
check(r['rc'] == '0' and r['wrote'] == '0' and r['videos'] == '4' and r['added'] == '0' and r['removed'] == '0',
      'episode entries without our marker are kept, nothing written')

print('22. episodes removed')
os.remove(os.path.join(tv, 'Show A/Season 2/S02E04.m4v'))
r = run('--now', str(NOW + 2600))
raw22, db22 = load(dbp); validate(raw22, db22); verify(dbp)
check(r['wrote'] == '1' and r['removed'] == '1' and r['videos'] == '3' and len(videos(db22)) == 3, 'one file gone: its entry removed')
shutil.rmtree(tv)
r = run('--now', str(NOW + 2700))
raw22b, db22b = load(dbp); validate(raw22b, db22b); verify(dbp)
check(r['wrote'] == '1' and r['removed'] == '3' and r['videos'] == '0' and not videos(db22b), 'no /TV Shows: the episodes dropped')
check(nflac(db22b) == int(r['flacs']) and r['kept'] == r['flacs'], 'and every FLAC entry kept')

print('23. episodes the iPod cannot play are not added')
mp4(os.path.join(tv, 'Show C/S01E01.m4v'), 30, video=(b'avc1', 640, 480, 66, 30))
mp4(os.path.join(tv, 'Show C/S01E02.m4v'), 30, video=(b'avc1', 640, 480, 100, 30))    # High profile
mp4(os.path.join(tv, 'Show C/S01E03.m4v'), 30, video=(b'avc1', 1280, 720, 66, 31))    # too large
mp4(os.path.join(tv, 'Show C/S01E04.m4v'), 30, video=(b'hvc1', 640, 480, 0, 0))       # HEVC
r = run('--now', str(NOW + 2800))
raw23, db23 = load(dbp); validate(raw23, db23); verify(dbp)
v23 = videos(db23)
check(r['added'] == '1' and r['skipped'] == '3' and list(v23) == [':TV Shows:Show C:S01E01.m4v'],
      'Baseline 640x480 added; High profile, 1280x720 and HEVC skipped')

print('\n%s' % ('ALL PASSED' if not fails else '%d FAILED' % fails))
sys.exit(1 if fails else 0)
