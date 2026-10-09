#!/usr/bin/env python3
"""Analyse the in-guest IRQ sampler (FLAC\\samples.bin from PROF builds): guest-time-uniform samples
of the interrupted PC, time, running RTXC TCB and 48 words of SVC stack.
    irqsamp.py SAMPLES.bin OS_LISTING IMAGE_BODY.bin E_SYMS [T0_MS T1_MS] [UI_TCB_HEX]"""
import struct, sys, re, bisect
from collections import Counter, defaultdict
smp, listing, body, esyms = sys.argv[1:5]
t0 = int(sys.argv[5]) * 1000 if len(sys.argv) > 5 else 0
t1 = int(sys.argv[6]) * 1000 if len(sys.argv) > 6 else 1 << 32
ui = int(sys.argv[7], 16) if len(sys.argv) > 7 else None
line = re.compile(r'^ ([0-9a-f]{7,8}):\t[0-9a-f]{8} \tpush')
starts = sorted(int(m.group(1), 16) for m in map(line.match, open(listing)) if m)
syms = []
for l in open(esyms):
    p = l.split()
    if len(p) == 3 and p[1] in 'tT': syms.append((int(p[0], 16), p[2]))
syms.sort(); sa = [a for a, _ in syms]
img = open(body, 'rb').read()
def code(a):
    o = a - 0x08000000 + 0xaed8
    return struct.unpack_from('<I', img, o)[0] if 0 <= o < len(img) - 4 else 0
def isret(v):
    if not (0x08000000 <= v < 0x08a0fc88): return False
    p = code(v - 4); return (p >> 24) & 0xf == 0xb or (p & 0x0ffffff0) == 0x012fff30
def name(pc):
    if 0x08000000 <= pc < 0x08a10000: return '%08x' % starts[bisect.bisect_right(starts, pc) - 1]
    if 0x08b33000 <= pc < 0x08c00000: k = bisect.bisect_right(sa, pc) - 1; return 'E:' + (syms[k][1] if k >= 0 else '?')
    if 0x22003b3c <= pc < 0x22003b64: return 'IDLE'
    if 0x22000000 <= pc < 0x22040000: return 'IRAM:%05x' % ((pc - 0x22000000) & ~0x3f)
    return 'other:%x' % (pc & ~0xfff)
d = open(smp, 'rb').read()
cnt = struct.unpack_from('<I', d, 0)[0]
N = 3072; W = 53
ents = []
for i in range(min(cnt, N)):
    w = struct.unpack_from('<%dI' % W, d, 8 + i * W * 4)
    if t0 <= w[1] <= t1: ents.append(w)
ents.sort(key=lambda w: w[1])
print('%d samples in window (%d interrupts total), %.0f ms' % (len(ents), cnt, (ents[-1][1] - ents[0][1]) / 1000 if ents else 0))
tasks = Counter(w[2] for w in ents)
print('by running task:', ', '.join('%08x %d%%' % (t, 100 * c // len(ents)) for t, c in tasks.most_common(8)))
sel = [w for w in ents if ui is None or w[2] == ui]
ex = Counter(); par = defaultdict(Counter)
for w in sel:
    k = name(w[0]); ex[k] += 1
    rets = [x for x in w[5:] if isret(x)]
    if rets: par[k][name(rets[0])] += 1
print('\n%d samples%s; exclusive:' % (len(sel), ' of the UI task' if ui else ''))
for k, v in ex.most_common(30):
    print('%5.1f%% %-26s %s' % (100 * v / len(sel), k, '  '.join('%s:%d' % x for x in par[k].most_common(2))))
