#!/usr/bin/env python3
"""Top-down tree from sample.py records (stack scan): counts per call path from the outermost frame.
    stree.py SAMPLES.bin OS_LISTING IMAGE_BODY.bin T0_MS T1_MS STACK_BYTES [MIN_PCT] [DEPTH] [SP_LO SP_HI]"""
import struct, bisect, re, sys
from collections import defaultdict
smp, listing, body = sys.argv[1:4]
t0, t1, STK = int(sys.argv[4]) * 1000, int(sys.argv[5]) * 1000, int(sys.argv[6])
MINP = float(sys.argv[7]) if len(sys.argv) > 7 else 1.0
DEPTH = int(sys.argv[8]) if len(sys.argv) > 8 else 14
splo, sphi = (int(sys.argv[9], 16), int(sys.argv[10], 16)) if len(sys.argv) > 10 else (0, 1 << 32)
line = re.compile(r'^ ([0-9a-f]{7,8}):\t[0-9a-f]{8} \tpush')
starts = sorted(int(m.group(1), 16) for m in map(line.match, open(listing)) if m)
def fn(a): return starts[bisect.bisect_right(starts, a) - 1]
img = open(body, 'rb').read()
def code(a):
    o = a - 0x08000000 + 0xaed8
    return struct.unpack_from('<I', img, o)[0] if 0 <= o < len(img) - 4 else 0
def isret(v):
    if not (0x08000000 <= v < 0x08a0fc88): return False
    p = code(v - 4); return (p >> 24) & 0xf == 0xb or (p & 0x0ffffff0) == 0x012fff30
R = 16 + STK
d = open(smp, 'rb').read()
tree = lambda: defaultdict(tree)
counts = defaultdict(int); n = 0
for i in range(0, len(d) - R + 1, R):
    t, pc, lr, sp = struct.unpack_from('<4I', d, i)
    if not (t0 <= t <= t1) or not (splo <= sp < sphi): continue
    n += 1
    rets = [fn(x) for x in struct.unpack_from('<%dI' % (STK // 4), d, i + 16) if isret(x)]
    leaf = fn(pc) if 0x08000000 <= pc < 0x08a10000 else pc & ~0xff
    path = list(reversed(rets)) + [leaf]
    # drop immediate repeats (recursion / stale duplicates)
    p2 = []
    for f in path:
        if not p2 or p2[-1] != f: p2.append(f)
    for k in range(1, min(len(p2), DEPTH) + 1): counts[tuple(p2[:k])] += 1
print('%d samples' % n)
def show(prefix, depth):
    kids = sorted(((c, k) for k, c in counts.items() if len(k) == len(prefix) + 1 and k[:len(prefix)] == prefix), reverse=True)
    for c, k in kids:
        if 100 * c / n < MINP: continue
        print('  ' * depth + '%08x %5.1f%%' % (k[-1], 100 * c / n))
        show(k, depth + 1)
show((), 0)
