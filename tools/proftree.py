#!/usr/bin/env python3
"""Expand a profiler dump (loadprofN.bin) into a call tree using the static call graph of the
OS listing: each node's children are its direct bl targets that were probed, with their totals.
    proftree.py DUMP OS_LISTING ROOT_HEX [MIN_MS] [DEPTH]"""
import struct, sys, re, bisect
dump, listing, root = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
MIN = float(sys.argv[4]) if len(sys.argv) > 4 else 30
DEPTH = int(sys.argv[5]) if len(sys.argv) > 5 else 8
d = open(dump, 'rb').read(); n = struct.unpack_from('<I', d, 4)[0]
st = {}
for i in range(n):
    a, c, inc, exc, mx, tk = struct.unpack_from('<6I', d, 32 + i * 24)
    st[a] = (c, inc / 1000, exc / 1000)
line = re.compile(r'^ ([0-9a-f]{7,8}):\t[0-9a-f]{8} \t(\S+)\t?(.*)$')
addrs, bls = [], []
starts = []
for l in open(listing):
    m = line.match(l)
    if not m: continue
    a = int(m.group(1), 16)
    if a >= 0x08a0fc88: break
    if m.group(2) == 'push': starts.append(a)
    if m.group(2) in ('bl', 'b') and m.group(3).startswith('0x'):
        addrs.append(a); bls.append(int(m.group(3).split()[0], 16))
starts.sort()
def body(f):
    k = bisect.bisect_right(starts, f)
    end = starts[k] if k < len(starts) else f + 0x1000
    i = bisect.bisect_left(addrs, f); out = []
    while i < len(addrs) and addrs[i] < end:
        out.append(bls[i]); i += 1
    return out
def show(f, depth, seen):
    c, inc, exc = st.get(f, (0, 0, 0))
    print('  ' * depth + '%08x  total %7.0f ms  own %6.0f ms  calls %d' % (f, inc, exc, c))
    if depth >= DEPTH: return
    kids = sorted({k for k in body(f) if k in st and k not in seen and st[k][1] >= MIN}, key=lambda k: -st[k][1])
    for k in kids: show(k, depth + 1, seen | {f})
show(root, 0, set())
