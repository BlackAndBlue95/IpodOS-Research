#!/usr/bin/env python3
"""Lays out an MP4/M4V like Apple's files: each track's samples in ~0.5 s chunks, audio and video
alternating, moov first. ffmpeg's muxer writes one sample per chunk. Samples are moved, not re-encoded.

  rechunk.py FILE.m4v ...         in place, streaming: the new moov is smaller, so writes stay behind reads
  rechunk.py IN.m4v -o OUT.m4v"""
import os, struct, sys

CONTAINERS = {b'moov', b'trak', b'mdia', b'minf', b'stbl', b'edts', b'dinf', b'udta'}
WINDOW = 0.5

def parse(b, o, e):
    out = []
    while o < e:
        n, t = struct.unpack('>I4s', b[o:o + 8])
        assert n >= 8 and o + n <= e, (t, n)
        out.append([t, parse(b, o + 8, o + n) if t in CONTAINERS else b[o + 8:o + n]])
        o += n
    return out

def build(boxes):
    out = b''
    for t, v in boxes:
        body = build(v) if isinstance(v, list) else v
        out += struct.pack('>I4s', 8 + len(body), t) + body
    return out

def find(boxes, *path):
    for t, v in boxes:
        if t == path[0]: return v if len(path) == 1 else find(v, *path[1:])
    return None

def replace(boxes, t, v):
    for k, (tt, _) in enumerate(boxes):
        if tt == t: boxes[k][1] = v; return
    raise KeyError(t)

def u32s(b, n, o=0): return struct.unpack('>%dI' % n, b[o:o + 4 * n])

def load_track(trak):
    stbl = find(trak, b'mdia', b'minf', b'stbl')
    mdhd = find(trak, b'mdia', b'mdhd')
    ts = u32s(mdhd, 1, 12)[0] if mdhd[0] == 0 else u32s(mdhd, 1, 20)[0]
    assert find(stbl, b'co64') is None, 'co64: files over 4 GB are not handled'
    stsz = find(stbl, b'stsz'); fixed, count = u32s(stsz, 2, 4)
    sizes = [fixed] * count if fixed else list(u32s(stsz, count, 12))
    stco = find(stbl, b'stco'); offs = u32s(stco, u32s(stco, 1, 4)[0], 8)
    stsc = find(stbl, b'stsc'); ne = u32s(stsc, 1, 4)[0]
    runs = [u32s(stsc, 3, 8 + 12 * i) for i in range(ne)]
    stts = find(stbl, b'stts'); nt = u32s(stts, 1, 4)[0]
    dts, t0 = [], 0
    for i in range(nt):
        c, d = u32s(stts, 2, 8 + 8 * i)
        for _ in range(c): dts.append(t0); t0 += d
    assert len(dts) == count, (len(dts), count)
    pos, s, ri = [], 0, 0                                   # each sample's offset, from the chunk table
    for ci, off in enumerate(offs):
        while ri + 1 < len(runs) and runs[ri + 1][0] <= ci + 1: ri += 1
        for _ in range(runs[ri][1]):
            pos.append(off); off += sizes[s]; s += 1
    assert s == count, (s, count)
    desc = set(r[2] for r in runs)
    assert len(desc) == 1, 'several sample descriptions'
    return dict(stbl=stbl, ts=ts, sizes=sizes, pos=pos, dts=dts, desc=desc.pop(), chunks=len(offs))

def rechunk(src, dst):
    """(chunks before, chunks after); in place, a file that is already chunked is left alone"""
    rf = open(src, 'rb', buffering=1 << 20)
    end, top, o = os.fstat(rf.fileno()).st_size, [], 0
    while o < end:
        rf.seek(o); n, t = struct.unpack('>I4s', rf.read(8))
        assert n >= 8 and o + n <= end, (t, n)
        top.append((t, o, n)); o += n
    types = [t for t, o, n in top]
    assert types[-1] == b'mdat' and types.count(b'mdat') == 1 and set(types) <= {b'ftyp', b'moov', b'free', b'mdat'}, types
    def body(t):
        _, o, n = next(x for x in top if x[0] == t); rf.seek(o + 8); return rf.read(n - 8)
    ftyp, mb = body(b'ftyp'), body(b'moov')
    moov = parse(mb, 0, len(mb))
    tracks = [load_track(trak) for t, trak in moov if t == b'trak']
    was = sum(tr['chunks'] for tr in tracks)

    last = max(tr['dts'][-1] / tr['ts'] for tr in tracks) + WINDOW
    nxt, order, k = [0] * len(tracks), [], 0                # order: (track, first sample, count) in file order
    while k * WINDOW < last:
        lim = (k + 1) * WINDOW
        for ti, tr in enumerate(tracks):
            a = nxt[ti]
            while nxt[ti] < len(tr['dts']) and tr['dts'][nxt[ti]] / tr['ts'] < lim: nxt[ti] += 1
            if nxt[ti] > a: order.append((ti, a, nxt[ti] - a))
        k += 1
    assert all(nxt[i] == len(tr['dts']) for i, tr in enumerate(tracks))
    if src == dst and len(order) >= was: return was, was

    def tables(base):
        off, per = base, [[] for _ in tracks]
        for ti, a, n in order:
            per[ti].append((off, n))
            off += sum(tracks[ti]['sizes'][a:a + n])
        for ti, tr in enumerate(tracks):
            ch, ent = per[ti], []
            for i, (o, n) in enumerate(ch):
                if not ent or ent[-1][1] != n: ent.append((i + 1, n, tr['desc']))
            replace(tr['stbl'], b'stco', struct.pack('>II', 0, len(ch)) + b''.join(struct.pack('>I', o) for o, n in ch))
            replace(tr['stbl'], b'stsc', struct.pack('>II', 0, len(ent)) + b''.join(struct.pack('>III', *e) for e in ent))
        return off

    head = build([[b'ftyp', ftyp]])
    tables(0)                                               # sizes only: the offsets do not change them
    base = len(head + build([[b'moov', moov]])) + 8
    total = tables(base) - base
    head += build([[b'moov', moov]]) + struct.pack('>I4s', 8 + total, b'mdat')
    assert len(head) == base

    samples = sorted((tr['pos'][s], tr['sizes'][s], ti, s) for ti, tr in enumerate(tracks) for s in range(len(tr['sizes'])))
    mdat = top[-1][1] + 8
    assert samples[0][0] >= mdat and samples[-1][0] + samples[-1][1] <= end
    assert all(p + n <= q for (p, n, _, _), (q, _, _, _) in zip(samples, samples[1:])), 'overlapping samples'
    assert src != dst or base <= mdat, 'new moov larger than the old'
    entry, left = [[0] * len(tr['sizes']) for tr in tracks], []
    for e, (ti, a, n) in enumerate(order):
        entry[ti][a:a + n] = [e] * n; left.append(n)

    wf = open(dst, 'r+b' if src == dst else 'wb', buffering=1 << 20)
    wf.write(head)                                          # the old ftyp+moov are already read
    pend, e = {}, 0
    for p, n, ti, s in samples:                             # read in file order, write each chunk once complete
        if rf.tell() != p: rf.seek(p)
        pend[ti, s] = rf.read(n)
        left[entry[ti][s]] -= 1
        while e < len(order) and left[e] == 0:
            ti2, a, m = order[e]
            for x in range(a, a + m): wf.write(pend.pop((ti2, x)))
            e += 1
    assert e == len(order) and not pend
    wf.truncate(); wf.flush(); os.fsync(wf.fileno()); wf.close(); rf.close()
    return was, len(order)

if __name__ == '__main__':
    a = sys.argv[1:]
    for src, dst in [(a[0], a[2])] if len(a) == 3 and a[1] == '-o' else [(p, p) for p in a]:
        was, now = rechunk(src, dst)
        print('%s: %d chunks (was %d)%s' % (dst, now, was, ', left as is' if src == dst and now >= was else ''), flush=True)
