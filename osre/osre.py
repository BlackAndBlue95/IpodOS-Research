"""Cross-reference index for the iPod Classic 2.0.4 OS (ARM, DRAM part at 0x08000000).
from osre import *; then calls_to(a), refs_to(a), func_start(a), dis(a, n), strings_near(a)."""
import os, struct, subprocess, pickle
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = 0x08000000
D = open(os.path.join(HERE, 'dram.bin'), 'rb').read()
N = len(D) // 4
W = struct.unpack('<%dI' % N, D[:N * 4])
OBJDUMP = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'arm-gnu-toolchain-13.3.rel1-darwin-arm64-arm-none-eabi', 'bin', 'arm-none-eabi-objdump')

def u32(a): return W[(a - BASE) >> 2]
def ror(v, r): return ((v >> r) | (v << (32 - r))) & 0xffffffff

def _build():
    calls, branches, adrs, lits = defaultdict(list), defaultdict(list), defaultdict(list), defaultdict(list)
    for i, w in enumerate(W):
        a = BASE + 4 * i
        if (w >> 28) == 0xf: continue
        op = (w >> 25) & 7
        if op == 5:                                   # B / BL
            off = w & 0xffffff
            if off & 0x800000: off -= 0x1000000
            t = (a + 8 + 4 * off) & 0xffffffff
            (calls if w & 0x01000000 else branches)[t].append(a)
        elif (w & 0x0fef0000) == 0x028f0000:          # ADD Rd, PC, #imm (adr)
            imm = ror(w & 0xff, 2 * ((w >> 8) & 0xf))
            adrs[(a + 8 + imm) & 0xffffffff].append(a)
        elif (w & 0x0fef0000) == 0x024f0000:          # SUB Rd, PC, #imm
            imm = ror(w & 0xff, 2 * ((w >> 8) & 0xf))
            adrs[(a + 8 - imm) & 0xffffffff].append(a)
        elif (w & 0x0f3f0000) == 0x051f0000:          # LDR Rd, [PC, #+-imm]
            imm = w & 0xfff
            p = a + 8 + (imm if w & 0x00800000 else -imm)
            if BASE <= p < BASE + 4 * N:
                lits[u32(p)].append(a)
    return dict(calls=calls, branches=branches, adrs=adrs, lits=lits)

_cache = os.path.join(HERE, 'xref.pickle')
if os.path.exists(_cache) and os.path.getmtime(_cache) > os.path.getmtime(os.path.join(HERE, 'dram.bin')):
    X = pickle.load(open(_cache, 'rb'))
else:
    X = _build(); pickle.dump(X, open(_cache, 'wb'))

def calls_to(t): return sorted(X['calls'].get(t, []))
def branches_to(t): return sorted(X['branches'].get(t, []))
def refs_to(t):
    """adr or literal-pool references to address t (also t|1, Thumb)"""
    return sorted(set(X['adrs'].get(t, []) + X['lits'].get(t, []) + X['lits'].get(t | 1, [])))

def func_start(a, limit=0x4000):
    """nearest preceding push {.., lr} (stmdb sp!, {...lr})"""
    p = a & ~3
    while p > a - limit:
        w = u32(p)
        if (w & 0xffff4000) == 0xe92d4000: return p
        p -= 4
    return None

def dis(a, n=40):
    o = a - BASE
    open('/tmp/_osre.bin', 'wb').write(D[o:o + 4 * n])
    out = subprocess.run([OBJDUMP, '-D', '-b', 'binary', '-marm', '--adjust-vma=0x%x' % a, '/tmp/_osre.bin'],
                         capture_output=True, text=True).stdout
    lines = [l for l in out.splitlines() if ':\t' in l]
    return '\n'.join(lines)

def cstr(a, n=96):
    o = a - BASE
    s = D[o:o + n]
    return s.split(b'\0')[0].decode('latin-1')

def strings_near(a, span=0x200):
    """C strings referenced by adr/ldr inside [a, a+span)"""
    out = []
    for i in range(a, a + span, 4):
        w = u32(i)
        t = None
        if (w & 0x0fef0000) == 0x028f0000: t = i + 8 + ror(w & 0xff, 2 * ((w >> 8) & 0xf))
        elif (w & 0x0f3f0000) == 0x051f0000:
            imm = w & 0xfff; p = i + 8 + (imm if w & 0x00800000 else -imm)
            if BASE <= p < BASE + len(D): t = u32(p)
        if t and BASE <= t < BASE + len(D):
            s = cstr(t, 64)
            if len(s) >= 4 and all(32 <= ord(c) < 127 for c in s): out.append((hex(i), hex(t), s))
    return out
