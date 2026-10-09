#!/usr/bin/env python3
"""Log hits of code breakpoints between two guest times: time, pc, r0-r3, lr, sp words and the
bytes at r0-r3 when they point into RAM (strings).
    bptrace.py PORT T0_MS T1_MS ADDR_HEX[,ADDR_HEX...] [MAXHITS]"""
import socket, struct, sys, time
port, t0, t1 = int(sys.argv[1]), int(sys.argv[2]) * 1000, int(sys.argv[3]) * 1000
bps = [int(x[2:], 16) if x.startswith('w:') else int(x, 16) for x in sys.argv[4].split(',')]
kinds = [b'2' if x.startswith('w:') else b'0' for x in sys.argv[4].split(',')]
maxhits = int(sys.argv[5]) if len(sys.argv) > 5 else 5000
import os
DEREF = os.environ.get('DEREF') == '1'
for _ in range(100):
    try: s = socket.create_connection(('127.0.0.1', port)); break
    except OSError: time.sleep(0.1)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
buf = b''
def recv():
    global buf
    while True:
        i = buf.find(b'$')
        if i >= 0:
            j = buf.find(b'#', i)
            if j >= 0 and len(buf) >= j + 3:
                d = buf[i + 1:j]; buf = buf[j + 3:]; s.sendall(b'+'); return d
        c = s.recv(65536)
        if not c: raise EOFError
        buf += c
def send(cmd, reply=True):
    s.sendall(b'$' + cmd + b'#' + b'%02x' % (sum(cmd) & 0xff))
    return recv() if reply else None
def mem(a, n):
    d = send(b'm%x,%x' % (a, n))
    return b'' if d.startswith(b'E') else bytes.fromhex(d.decode())
def now(): return struct.unpack('<I', mem(0x3c7000b4, 4))[0]
def txt(a):
    if not (0x08000000 <= a < 0x0c000000): return ''
    b = mem(a, 64)
    z = b.split(b'\0')[0]
    if len(z) >= 3 and all(32 <= c < 127 for c in z): return '"%s"' % z.decode()
    # UTF-16?
    try:
        u = b.decode('utf-16-le').split('\0')[0]
        if len(u) >= 3 and all(32 <= ord(c) < 127 for c in u): return 'u"%s"' % u
    except Exception: pass
    return ''
send(b'?')
while True:
    send(b'c', reply=False); time.sleep(0.02); s.sendall(b'\x03'); recv()
    if now() >= t0: break
for b, k in zip(bps, kinds): send(b'Z' + k + b',%x,4' % b)
n = 0
first = True
while n < maxhits:
    if not first:                             # step off the breakpoint, then re-arm it
        for b, k in zip(bps, kinds): send(b'z' + k + b',%x,4' % b)
        send(b's')
        for b, k in zip(bps, kinds): send(b'Z' + k + b',%x,4' % b)
    first = False
    send(b'c', reply=False); recv()
    r = struct.unpack('<16I', bytes.fromhex(send(b'g').decode())[:64])
    t = now()
    stk = struct.unpack('<8I', mem(r[13], 32) or b'\0' * 32)
    extra = ''
    for spec in os.environ.get('DUMP', '').split(','):
        if not spec: continue
        rg, off, ln = spec.split(':'); base = r[int(rg[1:])] + int(off, 16)
        if 0x08000000 <= base < 0x0c000000:
            ws = struct.unpack('<%dI' % (int(ln, 16) // 4), mem(base, int(ln, 16)) or b'\0' * int(ln, 16))
            extra += ' %s+%s:' % (rg, off) + ' '.join('%08x' % x for x in ws)
    if DEREF and 0x08000000 <= r[0] < 0x0c000000:
        w0 = struct.unpack('<8I', mem(r[0], 32) or b'\0' * 32)
        extra = ' [r0]:' + ' '.join('%08x' % w for w in w0)
        if 0x08000000 <= w0[0] < 0x0c000000:
            w1 = struct.unpack('<16I', mem(w0[0], 64) or b'\0' * 64)
            extra += ' [[r0]]:' + ' '.join('%08x' % w for w in w1)
    print('%d %08x r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x sp:%s %s' % (t // 1000, r[15], r[0], r[1], r[2], r[3], r[14],
          ' '.join('%08x' % w for w in stk), ' '.join(x for x in (txt(r[0]), txt(r[1]), txt(r[2]), txt(r[3])) if x)) + extra, flush=True)
    n += 1
    if t >= t1: break
for b, k in zip(bps, kinds): send(b'z' + k + b',%x,4' % b)
send(b'c', reply=False)
