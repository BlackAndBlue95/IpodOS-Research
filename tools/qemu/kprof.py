#!/usr/bin/env python3
"""Reads kprof.c output.
    kprof.py phases KPROF.out                          per-phase instructions, IRQ share, memory traffic, L1 misses
    kprof.py funcs KPROF.out LISTING E_SYMS A:B [N]    functions between TB dumps A and B (e.g. t1:t2; ':t2' from boot)
    kprof.py copies KPROF.out LISTING E_SYMS A:B [N]   cp= callers between dumps A and B, ranked by modelled cycles
LISTING is an objdump -D of the OS body. Cycle model and environment overrides: tools/qemu/README.md."""
import bisect, os, re, sys
from collections import Counter

def load(path, cps=None):
    snaps, dumps, cur = [], {}, None
    for l in open(path):
        if cps is not None and l.startswith('CP ') and cur:
            p = l.split()
            cps.setdefault(cur, {})[(int(p[1], 16), int(p[2], 16), int(p[3]), int(p[6]))] = (
                int(p[4], 16), int(p[5], 16)) + tuple(int(x) for x in p[7:])
            continue
        if l.startswith('SNAP'):
            p = l.split(); d = {'label': p[1]}
            for kv in p[2:]:
                k, v = kv.split('='); d[k] = int(v, 16) if k == 'pc' else int(v)
            snaps.append(d)
        elif l.startswith('TBDUMP'):
            cur = l.split()[1]; dumps[cur] = {}
        elif l.startswith('TB ') and cur:
            p = l.split(); dumps[cur][int(p[1], 16)] = tuple(int(x) for x in p[2:])
    return snaps, dumps

def phases(path):
    snaps, _ = load(path)
    keys = [k for k in snaps[0] if k not in ('label', 'pc')]
    prev = dict.fromkeys(keys, 0); plabel = 'start'
    for s in snaps:
        d = {k: s[k] - prev[k] for k in keys}; prev = {k: s[k] for k in keys}
        w = max(1, d['ins'] - d['idle'])
        print('== %s -> %s: %.1f M instructions' % (plabel, s['label'], w / 1e6)); plabel = s['label']
        print('   IRQ handler %.1f%%, scheduler %.2f%%, IRAM code (uncached) %.1f%%, OS DRAM %.1f%%, region E %.1f%%' % (
            100 * d['irq'] / w, 100 * d['sched'] / w, 100 * d['ins_iram'] / w, 100 * d['ins_dram'] / w, 100 * d['ins_regionE'] / w))
        print('   cached DRAM %.1f M reads %.1f M writes; IRAM %.2f/%.2f M; IO %.2f/%.2f M; uncached DRAM alias %.3f M' % (
            (d['rd_dram'] + d['rd_regionE']) / 1e6, (d['wr_dram'] + d['wr_regionE']) / 1e6, d['rd_iram'] / 1e6, d['wr_iram'] / 1e6,
            d['rd_io'] / 1e6, d['wr_io'] / 1e6, (d['rd_dram-uncached'] + d['wr_dram-uncached']) / 1e6))
        print('   L1: I miss %.2f%% of line fetches (%.2f M), D read miss %.2f%% (%.2f M), write miss %.1f%% (%.2f M), writebacks %.2f M' % (
            100 * d['i_miss'] / max(1, d['i_acc']), d['i_miss'] / 1e6, 100 * d['d_rdmiss'] / max(1, d['d_rd']), d['d_rdmiss'] / 1e6,
            100 * d['d_wrmiss'] / max(1, d['d_wr']), d['d_wrmiss'] / 1e6, d['d_wb'] / 1e6))

IRAM_FUNCS = ((0x22000020, 0x2200027c, 'IRAM memcpy/memmove'), (0x2200027c, 0x22000318, 'IRAM memset/memclr'),
              (0x22001ee8, 0x22001f78, 'IRAM mdelay/elapsed'), (0x22003b64, 0x22004534, 'IRAM KS_* calls'),
              (0x22003080, 0x220030f8, 'IRAM IRQ entry'), (0x220034e8, 0x22003600, 'IRAM scheduler'))

def funcs(path, listing, esyms, ph, n):
    base, P, W, F = (float(os.environ.get(k, d)) for k, d in (('BASE', 1.5), ('P', 40), ('W', 6), ('F', 4)))
    iramc = os.environ.get('IRAMC') == '1'          # with the plugin's iramc=1: IRAM code pages fetch through the cache
    nospin = os.environ.get('NOSPIN') == '1'        # leave out the timer spins: their wall time is fixed
    _, dumps = load(path)
    a, b = ph.split(':')
    A, B = dumps.get(a, {}), dumps[b]
    push = re.compile(r'^ ([0-9a-f]{7,8}):\t[0-9a-f]{8} \tpush')
    starts = sorted(int(m.group(1), 16) for m in map(push.match, open(listing)) if m)
    es = sorted((int(p[0], 16), p[2]) for p in (l.split() for l in open(esyms)) if len(p) == 3 and p[1] in 'tT')
    ea = [x for x, _ in es]
    def fn(va):
        if va < 0x100000: va += 0x22000000
        if 0x08b33000 <= va < 0x08e00000: return 'E:' + es[bisect.bisect_right(ea, va) - 1][1]
        if 0x08000000 <= va < 0x08a10000: return '%08x' % starts[bisect.bisect_right(starts, va) - 1]
        if 0x22000000 <= va < 0x2200b000:
            for lo, hi, nm in IRAM_FUNCS:
                if lo <= va < hi: return nm
            return 'IRAM %05x' % ((va - 0x22000000) & ~0xfff)
        return 'other'
    ins, cyc, im, dm, wm = Counter(), Counter(), Counter(), Counter(), Counter()
    for va, (k, c, irq, i1, d1, w1) in B.items():
        o = A.get(va, (k, 0, 0, 0, 0, 0))
        c -= o[1]; i1 -= o[3]; d1 -= o[4]; w1 -= o[5]
        f = fn(va)
        if nospin and f == 'IRAM mdelay/elapsed': continue
        unc = k * c if (va < 0x100000 or 0x22000000 <= va < 0x2200b000) else 0
        if iramc and (va < 0x8000 or 0x22000000 <= va < 0x22008000): unc = 0
        ins[f] += k * c; im[f] += i1; dm[f] += d1; wm[f] += w1
        cyc[f] += k * c * base + (i1 + d1) * P + w1 * W + unc * (F - 1)
    T, TI = sum(cyc.values()), sum(ins.values())
    print('%s: %.1f M instructions, model %.0f M cycles = %.2f s at 216 MHz (BASE %g P %g W %g F %g)' % (ph, TI / 1e6, T / 1e6, T / 216e6, base, P, W, F))
    print('  I miss %.2f M, D read miss %.2f M, write miss %.2f M' % (sum(im.values()) / 1e6, sum(dm.values()) / 1e6, sum(wm.values()) / 1e6))
    for f, v in cyc.most_common(n):
        print('  %5.1f%% cyc %5.1f%% ins  %-24s I %5.0fk D %5.0fk W %5.0fk' % (100 * v / T, 100 * ins[f] / TI, f, im[f] / 1e3, dm[f] / 1e3, wm[f] / 1e3))
    return T, fn

def copies(path, listing, esyms, ph, n):
    """cp= records (plugin) between two dumps, by caller lr; cycles as in funcs, libc fetched uncached"""
    base, P, W, F = (float(os.environ.get(k, d)) for k, d in (('BASE', 1.5), ('P', 40), ('W', 6), ('F', 4)))
    T, fn = funcs(path, listing, esyms, ph, 0)
    cps = {}
    load(path, cps)
    a, b = ph.split(':')
    A, B = cps.get(a, {}), cps[b]
    by = {}
    for key, v in B.items():
        o = A.get(key, v[:2] + (0, 0, 0, 0))
        k, ins, dm, wm = (v[i] - o[i] for i in range(2, 6))
        if not k: continue
        pc, lr, size, _ = key
        r = by.setdefault(lr, dict(n=0, bytes=0, ins=0, dm=0, wm=0, sizes=Counter(), pcs=set(), dst=v[0], src=v[1]))
        r['n'] += k; r['bytes'] += k * size; r['ins'] += ins; r['dm'] += dm; r['wm'] += wm
        r['sizes'][size] += k; r['pcs'].add(pc)
    cyc = {lr: r['ins'] * (base + F - 1) + r['dm'] * P + r['wm'] * W for lr, r in by.items()}
    C = sum(cyc.values())
    print('copies: %d callers, %.1f M modelled cycles = %.1f%% of the window; %.1f MB copied' % (
        len(by), C / 1e6, 100 * C / T, sum(r['bytes'] for r in by.values()) / 1e6))
    for lr in sorted(cyc, key=cyc.get, reverse=True)[:n]:
        r = by[lr]
        sz = ' '.join('%d×%d' % (s, c) for s, c in r['sizes'].most_common(4))
        print('  %5.2f%% lr %08x in %-22s entry %-17s calls %6d  %7.0f KB  ins %6.0fk D %5.0fk W %5.0fk  dst %08x src %08x  sizes %s' % (
            100 * cyc[lr] / T, lr, fn(lr), ','.join('%x' % p for p in sorted(r['pcs'])), r['n'], r['bytes'] / 1024,
            r['ins'] / 1e3, r['dm'] / 1e3, r['wm'] / 1e3, r['dst'], r['src'], sz))

if sys.argv[1] == 'phases': phases(sys.argv[2])
elif sys.argv[1] == 'copies': copies(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], int(sys.argv[6]) if len(sys.argv) > 6 else 30)
else: funcs(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], int(sys.argv[6]) if len(sys.argv) > 6 else 30)
