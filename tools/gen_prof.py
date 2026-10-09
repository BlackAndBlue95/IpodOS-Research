#!/usr/bin/env python3
"""Pick OS functions to time during the library load and write the region E trampolines.
Walks direct calls from the given roots (breadth first, depth limited) through an objdump
listing of the OS, keeps functions whose first instruction is a push (safe to displace),
and writes runtime/prof_tramp.S plus a JSON list the patcher installs.
usage: gen_prof.py OS.S IMAGE.dec OUT.S OUT.json"""
import json, re, struct, sys

listing, image, out_s, out_json = sys.argv[1:5]
ROOTS = [0x0827ee0c, 0x0804d97c, 0x0804d6b8, 0x081135a0, 0x08113470, 0x08107d44, 0x081ae280,
         0x0805d62c, 0x08058ac0, 0x0808a3ec, 0x08089700, 0x080380fc, 0x0826cd9c]   # boot, UI start, library load
EXTRA = [0x08052304, 0x080522fc, 0x0805230c, 0x08052f44, 0x08052198, 0x0829fefc, 0x0829fe4c,
         0x08191f28, 0x0807037c, 0x082bb36c, 0x080defd8, 0x0802cf18, 0x082d90c0, 0x0826e9e4,
         0x082d5dd4, 0x082d500c]
DEPTH, LIMIT = 6, 3000
import os as _os
if _os.environ.get('PROF_ROOTS'):   # e.g. the UI build: these roots first, depth from PROF_DEPTH
    ROOTS = [int(x, 16) for x in _os.environ['PROF_ROOTS'].split(',')] + ROOTS
    DEPTH = int(_os.environ.get('PROF_DEPTH', DEPTH))
# hot tiny functions from an earlier dump: the hook cost would dwarf their own time
import os
SKIP = {int(l, 16) for l in open(os.path.join(os.path.dirname(__file__), 'prof_skip.txt')) if l.strip()}

img = open(image, 'rb').read()
def word(a): return struct.unpack_from('<I', img, a - 0x07ff4928)[0]

line = re.compile(r'^ ([0-9a-f]{7,8}):\t([0-9a-f]{8}) \t(\S+)\t?(.*)$')
addrs, ops = [], []
for l in open(listing):
    m = line.match(l)
    if not m: continue
    a = int(m.group(1), 16)
    if a >= 0x08a0fc88: break
    addrs.append(a); ops.append((m.group(3), m.group(4)))
index = {a: i for i, a in enumerate(addrs)}
entries = set()
for mn, arg in ops:
    if mn == 'bl':
        t = arg.split()[0]
        if t.startswith('0x'): entries.add(int(t, 16))
entries.update(ROOTS); entries.update(EXTRA)
sorted_entries = sorted(entries)
import bisect
def callees(f):
    i = index.get(f)
    if i is None: return []
    k = bisect.bisect_right(sorted_entries, f)
    end = sorted_entries[k] if k < len(sorted_entries) else f + 0x4000
    out = []
    while i < len(addrs) and addrs[i] < end and addrs[i] < f + 0x8000:
        mn, arg = ops[i]
        if mn == 'bl' and arg.startswith('0x'): out.append(int(arg.split()[0], 16))
        i += 1
    return out

seen, order, frontier = set(), [], [(r, 0) for r in ROOTS] + [(e, 0) for e in EXTRA]
while frontier and len(order) < LIMIT * 3:
    nxt = []
    for f, d in frontier:
        if f in seen: continue
        seen.add(f); order.append((f, d))
        if d < DEPTH: nxt += [(c, d + 1) for c in callees(f)]
    frontier = nxt
def displaceable(w):
    """safe to run out of place: unconditional, not a branch, no pc operand or pc in a register list"""
    if w >> 28 != 0xe or (w >> 25) & 7 == 5 or (w & 0x0ffffff0) == 0x012fff10: return False
    if (w >> 25) & 7 == 4: return not (w & 0x8000)            # ldm/stm: pc not in the list
    if (w >> 26) & 3 == 3: return False                       # coprocessor / swi
    return all(((w >> sh) & 15) != 15 for sh in (16, 12, 0))
def loops_to_entry(f):
    """a branch inside the body back to the entry would re-enter the trampoline"""
    i = index.get(f)
    if i is None: return True
    k = bisect.bisect_right(sorted_entries, f)
    end = sorted_entries[k] if k < len(sorted_entries) else f + 0x4000
    while i < len(addrs) and addrs[i] < end and addrs[i] < f + 0x8000:
        mn, arg = ops[i]
        if mn.startswith('b') and not mn.startswith('bl') and not mn.startswith('bx') and not mn.startswith('bic'):
            t = arg.split()[0] if arg else ''
            if t.startswith('0x') and int(t, 16) == f: return True
        i += 1
    return False
def reads_lr(f):
    """uses its return address as data before overwriting it (switch-table helpers read the
    table at lr); the swapped lr would point into the profiler"""
    i = index.get(f)
    if i is None: return True
    for k in range(i, min(i + 12, len(addrs))):
        mn, arg = ops[k]
        if mn.startswith('bl') and not mn.startswith('bic'): return False
        if mn.startswith('push') or mn.startswith('stmdb') or mn.startswith('stmfd'): continue
        if mn.startswith('bx') or (mn.startswith('mov') and arg.startswith('pc, lr')): return False
        regs = arg.split(',')
        if regs and regs[0].strip() == 'lr' and mn.startswith('str') and 'lr' not in ','.join(regs[1:]): continue
        if regs and regs[0].strip() == 'lr' and not mn.startswith(('str', 'cmp', 'tst', 'teq', 'cmn')): return False
        if re.search(r'\blr\b', arg): return True
    return False
def follow(f):
    """a function that is only 'b target' is timed at its target"""
    if not (0x08000000 <= f < 0x08a0fc88): return f
    w = word(f)
    if w >> 24 == 0xea:
        off = w & 0xffffff
        if off & 0x800000: off -= 0x1000000
        return f + 8 + 4 * off
    return f
order = [(follow(f), d) for f, d in order]
picked, skipped, done = [], 0, set()
for f, d in order:
    if f in done or not (0x08000000 <= f < 0x08a0fc88): continue
    done.add(f)
    w0 = word(f)
    if f in SKIP or not displaceable(w0) or loops_to_entry(f) or reads_lr(f):
        skipped += 1; continue
    picked.append({'addr': f, 'w0': w0, 'depth': d})
    if len(picked) >= LIMIT: break

with open(out_s, 'w') as s:
    s.write('@ generated by tools/gen_prof.py: one timing trampoline per profiled OS function\n')
    s.write('.section .text.entry,"ax"\n.arm\n.global prof_ret\n')
    # condition flags are preserved across the profiler calls (some OS routines read flags set by their caller)
    s.write('prof_ret:\n push {r0-r3, ip, lr}\n mrs r0, cpsr\n push {r0, r1}\n add r0, sp, #32\n bl prof_exit\n str r0, [sp, #28]\n'
            ' pop {r0, r1}\n msr cpsr_f, r0\n pop {r0-r3, ip, lr}\n bx lr\n')
    for i, p in enumerate(picked):
        s.write('.global hk_p%d\nhk_p%d:\n push {r0-r3, ip, lr}\n mrs r2, cpsr\n push {r2, r3}\n mov r0, #%d\n orr r0, r0, #%d\n'
                ' add r1, sp, #8\n bl prof_enter\n pop {r2, r3}\n msr cpsr_f, r2\n pop {r0-r3, ip, lr}\n .word 0x%08x\n ldr pc, 1f\n1: .word 0x%08x\n'
                % (i, i, i & 0xff, i & 0xff00, p['w0'], p['addr'] + 4))
    # installed at run time by prof.c (address, original first word, trampoline)
    s.write('.section .rodata\n.global prof_addr\n.global prof_w0\n.global prof_hk\n.global prof_n\n.align 2\nprof_n: .word %d\nprof_addr:\n' % len(picked))
    for p in picked: s.write(' .word 0x%08x\n' % p['addr'])
    s.write('prof_w0:\n')
    for p in picked: s.write(' .word 0x%08x\n' % p['w0'])
    s.write('prof_hk:\n')
    for i in range(len(picked)): s.write(' .word hk_p%d\n' % i)
json.dump(picked, open(out_json, 'w'), indent=0)
print('%d functions picked (%d not safely hookable), depth histogram %s' % (
    len(picked), skipped, {d: sum(1 for p in picked if p['depth'] == d) for d in range(DEPTH + 1)}))
