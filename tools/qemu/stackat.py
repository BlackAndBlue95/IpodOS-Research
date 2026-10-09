#!/usr/bin/env python3
"""Stop the emulated iPod at given guest times (ms, timer 0x3c7000b4) and dump memory ranges
(e.g. a blocked task's stack) for offline call-chain recovery.
    stackat.py PORT OUT_PREFIX T1_MS[,T2_MS...] START:END[,START:END...]"""
import socket, struct, sys, time
port, pre = int(sys.argv[1]), sys.argv[2]
times = [int(x) * 1000 for x in sys.argv[3].split(',')]
ranges = [tuple(int(v, 16) for v in r.split(':')) for r in sys.argv[4].split(',')]
for _ in range(100):
    try: s = socket.create_connection(('127.0.0.1', port)); break
    except OSError: time.sleep(0.1)
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
    out = b''
    while n:
        k = min(n, 2048); d = send(b'm%x,%x' % (a, k))
        out += b'\0' * k if d.startswith(b'E') else bytes.fromhex(d.decode()); a += k; n -= k
    return out
send(b'?')
for k, t in enumerate(times):
    while True:
        send(b'c', reply=False); time.sleep(0.02); s.sendall(b'\x03'); recv()
        now = struct.unpack('<I', mem(0x3c7000b4, 4))[0]
        if now >= t: break
    r = bytes.fromhex(send(b'g').decode())
    with open('%s-%d.bin' % (pre, t // 1000), 'wb') as f:
        f.write(struct.pack('<I', now) + r[:64])
        for a, b in ranges: f.write(struct.pack('<II', a, b) + mem(a, b - a))
    print('dumped at', now // 1000, 'ms')
send(b'c', reply=False)
