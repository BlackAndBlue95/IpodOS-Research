#!/usr/bin/env python3
"""Analyse switches.py output: per task (stack region), run time and the call chains it blocks in.
    swan.py SW.bin OS_LISTING IMAGE_BODY.bin [T0_MS T1_MS]"""
import bisect, re, struct, sys
from collections import Counter, defaultdict
sw, listing, body = sys.argv[1:4]
T0 = int(sys.argv[4]) * 1000 if len(sys.argv) > 4 else 0
T1 = int(sys.argv[5]) * 1000 if len(sys.argv) > 5 else 1 << 32
img = open(body, 'rb').read()
line = re.compile(r'^ ([0-9a-f]{7,8}):\t[0-9a-f]{8} \tpush')
starts = sorted(int(m.group(1), 16) for m in map(line.match, open(listing)) if m)
def fn(a): return starts[bisect.bisect_right(starts, a) - 1] if 0x08000000 <= a < 0x08a10000 else a
def code(a):
    o = a - 0x08000000 + 0xaed8 if a >= 0x08000000 else a - 0x22000000
    return struct.unpack_from('<I', img, o)[0] if 0 <= o < len(img) - 4 else 0
def isret(v):
    if not (0x08000000 <= v < 0x08a0fc88 or 0x22000000 <= v < 0x2200aed8): return False
    p = code(v - 4); return (p >> 24) & 0xf == 0xb or (p & 0x0ffffff0) == 0x012fff30
d = open(sw, 'rb').read(); R = 12 + 384
ev = []
for i in range(0, len(d) - R + 1, R):
    k, t, fr = struct.unpack_from('<III', d, i)
    ev.append((k, t, fr, d[i + 12:i + R]))
task = lambda fr: fr >> 12           # stacks are separate 4 KB+ regions
running, since = None, None
run = Counter(); blocked = defaultdict(Counter); waits = defaultdict(list); last_out = {}
names = {}
for k, t, fr, m in ev:
    tk = task(fr)
    if k == 1:                       # resumes
        if tk in last_out:
            t_out, chain = last_out.pop(tk)
            if T0 <= t_out <= T1: waits[tk].append((t - t_out, chain, t_out))
        running, since = tk, t
    else:                            # goes to sleep (kernel call)
        if running == tk and since is not None and T0 <= t <= T1: run[tk] += t - since
        words = struct.unpack('<96I', m)
        chain = tuple(fn(w) for w in words[15:] if isret(w))[:8]
        last_out[tk] = (t, chain)
        running = None
span = (min(T1, ev[-1][1]) - max(T0, ev[0][1])) / 1000
print('window %.0f ms, %d events' % (span, len(ev)))
for tk, us in run.most_common(12):
    w = waits[tk]
    tot = sum(x[0] for x in w)
    print('\ntask stack %05x000: ran %.0f ms, %d waits totalling %.0f ms' % (tk, us / 1000, len(w), tot / 1000))
    agg = defaultdict(lambda: [0, 0])
    for dur, chain, t in w:
        key = ' > '.join('%08x' % c for c in chain[:5]); agg[key][0] += 1; agg[key][1] += dur
    for key, (n, dur) in sorted(agg.items(), key=lambda kv: -kv[1][1])[:6]:
        print('   %6.0f ms in %5d waits at %s' % (dur / 1000, n, key))
