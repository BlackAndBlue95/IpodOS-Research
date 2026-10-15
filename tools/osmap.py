#!/usr/bin/env python3
"""osmap.py STOCK.dec INVENTORY.md OUT.map: a symbol map of Apple's OS 2.0.4 from a string inventory.

The inventory (giek2000/ipod-classic-firmware-research, specs/iPod_Classic_7G_35_2_0_4.md) lists
strings by file offset in the decrypted image. Each becomes a runtime address (the image body is
loaded so that file offset 0x800 sits at 0x07ff5128). For every string the map adds:
  - the code that loads its address from a literal pool (the enclosing function's entry);
  - for class names, the data words that point at the name (type info) and the vtables after them.
Lines: ADDRESS KIND NAME [-> details]. Sorted by address."""
import re, struct, sys

LOAD = 0x07ff5128 - 0x800          # runtime address = file offset + LOAD
img = open(sys.argv[1], 'rb').read()
rows = re.findall(r'\|\s*0x([0-9A-Fa-f]+)\s*\|\s*`([^`]+)`(?:\s*\|\s*([^|]*)\|)?', open(sys.argv[2], encoding='utf-8').read())
words = struct.unpack_from('<%dI' % ((len(img) - 0x800) // 4), img, 0x800)
base = LOAD + 0x800                # where the body's first word runs

def addr_of(i): return base + 4 * i
refs = {}                          # value -> word indices holding it
for i, w in enumerate(words):
    if base <= w < base + len(img): refs.setdefault(w, []).append(i)

def func_start(a):
    """entry of the ARM function containing a: the nearest push {..., lr} at or before it"""
    i = (a - base) // 4
    for k in range(i, max(i - 2048, 0), -1):
        if words[k] & 0xffff4000 == 0xe92d4000: return addr_of(k)
    return None

def loaders(pool_index):
    """code that loads the literal at pool_index with ldr rX, [pc, #imm]"""
    out = []
    lit = addr_of(pool_index)
    for k in range(max(pool_index - 1024, 0), pool_index):
        w = words[k]
        if w & 0x0f7f0000 == 0x051f0000:              # ldr rX, [pc, #+/-imm]
            off = w & 0xfff
            tgt = addr_of(k) + 8 + (off if w & 0x00800000 else -off)
            if tgt == lit: out.append(addr_of(k))
    return out

def rot_imm(w):
    v, r = w & 0xff, ((w >> 8) & 0xf) * 2
    return ((v >> r) | (v << (32 - r))) & 0xffffffff
adr = {}                           # address -> code that forms it with add/sub rX, pc, #imm
for k, w in enumerate(words):
    if w & 0x0fff0000 in (0x028f0000, 0x024f0000):    # add / sub rX, pc, #imm
        t = addr_of(k) + 8 + (rot_imm(w) if w & 0x00f00000 == 0x00800000 else -rot_imm(w))
        adr.setdefault(t & 0xffffffff, []).append(addr_of(k))

lines = []
for off, name, cls in rows:
    a = int(off, 16) + LOAD
    kind = (cls or '').strip() or 'string'
    detail = []
    if a in adr:
        detail.append('code ' + ','.join('%08x' % f for f in sorted({func_start(x) or x for x in adr[a]})))
    for pi in refs.get(a, []):
        ld = loaders(pi)
        if ld:
            fns = sorted({f for f in (func_start(x) for x in ld) if f})
            detail.append('code ' + ','.join('%08x' % f for f in fns))
        else:
            # a data word pointing at the name: type info; the words before a vtable point at it
            ti = addr_of(pi)
            vts = [addr_of(v) + 4 for v in refs.get(ti - 4, []) + refs.get(ti, [])]
            detail.append('data %08x' % ti + (' vtable ' + ','.join('%08x' % v for v in vts[:4]) if vts else ''))
    lines.append((a, '%08x %-12s %s%s' % (a, kind.replace(' ', '_')[:12], name.strip(), ('  -> ' + '; '.join(detail[:6])) if detail else '')))
with open(sys.argv[3], 'w') as f:
    f.write('# Apple OS 2.0.4 (iPod classic 6G/7G): names from %s, addresses at run time\n' % sys.argv[2].rsplit('/', 1)[-1])
    for _, l in sorted(set(lines)): f.write(l + '\n')
print('%d names, %d with code or data references' % (len(lines), sum(1 for _, l in lines if '->' in l)))
