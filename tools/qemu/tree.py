#!/usr/bin/env python3
"""Turn sample.py records into per-function inclusive/exclusive counts for a guest-time window.
    tree.py SAMPLES.bin OS_LISTING.s IMAGE_BODY.bin T0_MS T1_MS [N]"""
import bisect, re, struct, sys
from collections import Counter
smp, listing, body, t0, t1 = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]) * 1000, int(sys.argv[5]) * 1000
N = int(sys.argv[6]) if len(sys.argv) > 6 else 40
img = open(body, 'rb').read()
def code(a):
    o = a - 0x08000000 + 0xaed8
    return struct.unpack_from('<I', img, o)[0] if 0 <= o < len(img) - 4 else 0
line = re.compile(r'^ ([0-9a-f]{7,8}):\t[0-9a-f]{8} \tpush')
starts = sorted(int(m.group(1), 16) for m in map(line.match, open(listing)) if m)
def fn(a):
    k = bisect.bisect_right(starts, a) - 1
    return starts[k] if k >= 0 else a
def isret(v):
    if not (0x08000000 <= v < 0x08a0fc88): return False
    p = code(v - 4)
    return (p >> 24) & 0xf == 0xb or (p & 0x0ffffff0) == 0x012fff30
d = open(smp, 'rb').read(); R = 16 + 512
incl, excl, total, idle = Counter(), Counter(), 0, 0
for i in range(0, len(d) - R + 1, R):
    t, pc, lr, sp = struct.unpack_from('<4I', d, i)
    if not (t0 <= t <= t1): continue
    total += 1
    if 0x22003b3c <= pc < 0x22003b64: idle += 1; continue
    chain = [fn(pc)] if 0x08000000 <= pc < 0x08a0fc88 else ['%08x' % pc]
    if isret(lr): chain.append(fn(lr))
    for k in range(128):
        v = struct.unpack_from('<I', d, i + 16 + 4 * k)[0]
        if isret(v): chain.append(fn(v))
    excl[chain[0]] += 1
    for f in set(chain): incl[f] += 1
print('window %d-%d ms: %d samples, idle %d (%.0f%%)' % (t0 // 1000, t1 // 1000, total, idle, 100 * idle / max(total, 1)))
def nm(f): return f if isinstance(f, str) else '%08x' % f
print('by total (samples that have the function anywhere on the stack):')
for f, k in incl.most_common(N): print('  %-10s %5d  %4.1f%%   own %4d' % (nm(f), k, 100 * k / max(total, 1), excl[f]))
# the most common call chains (outermost first), for the non-idle samples in the window
chains = Counter()
for i in range(0, len(d) - R + 1, R):
    t, pc, lr, sp = struct.unpack_from('<4I', d, i)
    if not (t0 <= t <= t1) or 0x22003b3c <= pc < 0x22003b64: continue
    ch = []
    for k in range(128):
        v = struct.unpack_from('<I', d, i + 16 + 4 * k)[0]
        if isret(v): ch.append(fn(v))
    ch = list(reversed(ch))[:7]
    chains[tuple(ch)] += 1
print('top call chains (outermost first, 7 levels):')
for ch, k in chains.most_common(12): print('  %3d  %s' % (k, ' > '.join(nm(f) for f in ch)))
