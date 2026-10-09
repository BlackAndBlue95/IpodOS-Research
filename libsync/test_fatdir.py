#!/usr/bin/env python3
"""runtime/fatdir.c against a FAT32 volume written by macOS: names (long, non-ASCII, 8.3-only,
NT-case), sizes, mtimes, folders spanning several clusters, deleted entries, subfolders.
usage: test_fatdir.py WORKDIR   (needs hdiutil; builds libsync/fatdir_host)"""
import datetime, os, random, subprocess, sys, time, unicodedata, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
work = sys.argv[1]; os.makedirs(work, exist_ok=True)
img = os.path.join(work, 'fdtest.dmg'); vol = '/Volumes/FDTEST'
subprocess.run(['clang', '-std=c99', '-O2', '-Wall', '-Wextra', '-DFATDIR_HOST', '-o', os.path.join(HERE, 'fatdir_host'),
                os.path.join(HERE, 'fatdir_host.c'), os.path.join(HERE, '..', 'runtime', 'fatdir.c')], check=True)
if os.path.exists(vol): subprocess.run(['hdiutil', 'detach', vol, '-force'], capture_output=True)
if os.path.exists(img): os.remove(img)
subprocess.run(['hdiutil', 'create', '-size', '64m', '-fs', 'MS-DOS FAT32', '-volname', 'FDTEST', '-layout', 'NONE', '-o', img], check=True, capture_output=True)
subprocess.run(['hdiutil', 'attach', '-nobrowse', img], check=True, capture_output=True)
random.seed(7)
def os_style(n):
    """what the iPod OS lists: names that fit 8.3 come back as the stored upper-case short name"""
    b, _, e = n.partition('.')
    uni = lambda p: p == p.upper() or p == p.lower()          # mixed case within a part gets a long-name entry instead
    fits = n.isascii() and ' ' not in n and n.count('.') <= 1 and 1 <= len(b) <= 8 and len(e) <= 3 and all(c not in '+,;=[]' for c in n) and uni(b) and uni(e)
    return n.upper() if fits else n
fails = 0
def check(c, what):
    global fails
    print(('  ok   ' if c else '  FAIL ') + what); fails += 0 if c else 1
try:
    music = os.path.join(vol, 'Music'); os.makedirs(music)
    albums = ['ABBA - Gold_ Greatest Hits (1992)', 'Marcin Przybyłowicz - The Witcher 3', 'Daft Punk - Discovery',
              "Alina Gingertail - The Wolven Storm (Priscilla's Song) (2020)", 'Tako Tsubo - L’Impératrice', '東京事変 - 大人', 'SHORT', 'a lower 8.3']
    expect = {}
    for i, a in enumerate(albums):
        d = os.path.join(music, a); os.makedirs(d)
        nfiles = 300 if i == 0 else random.randint(3, 20)          # the first spans several clusters
        for k in range(nfiles):
            name = '%02d - %s - Track %d%s.flac' % (k + 1, a.split(' - ')[0], k, ' ü' if k % 7 == 0 else '')
            if a == 'SHORT': name = 'T%02d.FLA' % k                  # 8.3 only (upper case, no LFN)
            if a == 'a lower 8.3': name = 'tr%02d.fla' % k           # 8.3 with NT lower-case bits
            p = os.path.join(d, name); open(p, 'wb').write(os.urandom(random.randint(1, 70000)))
            t = 1600000000 + random.randint(0, 150000000) // 2 * 2
            os.utime(p, (t, t))
        if i == 2:
            open(os.path.join(d, 'cover.jpg'), 'wb').write(b'x' * 1234); os.makedirs(os.path.join(d, 'Scans'))
            gone = os.path.join(d, 'deleted me.flac'); open(gone, 'wb').write(b'y'); os.remove(gone)
    os.sync(); time.sleep(1)
    for a in albums:
        d = os.path.join(music, a)
        ents = {}
        for n in os.listdir(d):
            st = os.stat(os.path.join(d, n))
            # FAT stores local time; the device reads the stamps as UTC
            local = int(st.st_mtime) + time.localtime().tm_gmtoff        # msdosfs applies the current offset to every stamp
            ents[unicodedata.normalize('NFC', n)] = (os.path.isdir(os.path.join(d, n)), 0 if os.path.isdir(os.path.join(d, n)) else st.st_size, local)
        expect[unicodedata.normalize('NFC', a)] = {os_style(k): v for k, v in ents.items()}
    top = {os_style(unicodedata.normalize('NFC', n)): os.path.isdir(os.path.join(music, n)) for n in os.listdir(music)}
finally:
    subprocess.run(['hdiutil', 'detach', vol], check=True, capture_output=True)
# sector size of the image
import struct
b = open(img, 'rb').read(512); bps = struct.unpack_from('<H', b, 11)[0]
print('image: %d bytes/sector' % bps)
def listing(path):
    out = subprocess.run([os.path.join(HERE, 'fatdir_host'), img, str(bps), path], capture_output=True, text=True).stdout.splitlines()
    ents = {}; summary = out[-1]
    for l in out[:-1]:
        name, kind, size, mtime = l.split('\t'); ents[unicodedata.normalize('NFC', name)] = (kind == 'D', int(size), int(mtime))
    return ents, summary
ents, summ = listing('Music')
check(set(ents) == set(top) and all(ents[n][0] == top[n] for n in top), 'Music: %d folders listed (%s)' % (len(ents), summ))
for a, want in expect.items():
    got, summ = listing('Music\\' + a)
    same = set(got) == set(want)
    diff = [n for n in want if n in got and (got[n][0] != want[n][0] or (not want[n][0] and got[n][1] != want[n][1]) or abs(got[n][2] - want[n][2]) > 1)]
    check(same and not diff, '%s: %d entries, sizes and mtimes equal (%s)' % (a, len(want), summ) if same else
          '%s: names differ: missing %s extra %s' % (a, sorted(set(want) - set(got))[:3], sorted(set(got) - set(want))[:3]))
    if diff: print('       differing:', [(n, got[n], want[n]) for n in diff[:3]])
got, summ = listing('Music\\' + albums[2] + '\\Scans')
check(got == {} and summ.endswith('0 entries, reads so far ' + summ.split()[-1]), 'empty subfolder lists as empty')
got, summ = listing('Music\\nope')
check('-1 entries' in summ, 'missing folder -> -1')
got, summ = listing('Music\\' + albums[2] + '\\cover.jpg')
check('-1 entries' in summ, 'a file as a folder -> -1')
print('\n%s' % ('ALL PASSED' if not fails else '%d FAILED' % fails))
sys.exit(1 if fails else 0)
