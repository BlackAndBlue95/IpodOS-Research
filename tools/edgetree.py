#!/usr/bin/env python3
"""Call tree from the profiler's edge dump (FLAC\\edges.bin) and function list (loadprofN.bin).
    edgetree.py LOADPROF EDGES [ROOT_HEX|top] [MIN_MS] [DEPTH]"""
import struct, sys
from collections import defaultdict
lp, ed = sys.argv[1], sys.argv[2]
root = sys.argv[3] if len(sys.argv) > 3 else 'top'
MIN = float(sys.argv[4]) if len(sys.argv) > 4 else 50
DEPTH = int(sys.argv[5]) if len(sys.argv) > 5 else 12
d = open(lp, 'rb').read(); n = struct.unpack_from('<I', d, 4)[0]
addr = [struct.unpack_from('<I', d, 32 + i * 24)[0] for i in range(n)]
e = open(ed, 'rb').read()
kids = defaultdict(list); incl_by = defaultdict(int); cnt_by = defaultdict(int)
for i in range(0, len(e), 12):
    p, c, k, t = struct.unpack_from('<HHII', e, i)
    if not k: continue
    kids[p].append((t / 1000, k, c)); incl_by[c] += t; cnt_by[c] += k
WAITS = {0x080e2b04, 0x0804baa4, 0x082dd198, 0x0807c520, 0x0807c518, 0x0806f410, 0x080833d8, 0x0804ba10}
def name(i): return 'ROOT' if i == 0xffff else '%08x' % addr[i]
def show(i, depth, ms, k, path):
    own = ms - sum(t for t, _, _ in kids[i])
    print('  ' * depth + '%s  %7.0f ms  calls %-6d own %6.0f' % (name(i), ms, k, own))
    if depth >= DEPTH: return
    for t, kk, c in sorted(kids[i], reverse=True):
        if t < MIN or c in path: continue
        if c != 0xffff and addr[c] in WAITS: 
            print('  ' * (depth + 1) + '%s  %7.0f ms  calls %-6d (wait)' % (name(c), t, kk)); continue
        show(c, depth + 1, t, kk, path | {c})
if root == 'top':
    for t, k, c in sorted(kids[0xffff], reverse=True)[:12]:
        if t >= MIN and addr[c] not in WAITS: show(c, 0, t, k, {c})
else:
    r = addr.index(int(root, 16))
    show(r, 0, incl_by[r] / 1000, cnt_by[r], {r})
