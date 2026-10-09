#!/usr/bin/env python3
"""While the emulated iPod runs in real time, stop it at a fixed host rate; learn the UI task's
TCB (RTXC running TCB at 0x2200acf4 whenever the stack pointer is in the UI stack), and when the
CPU is idle or another task runs, record the UI task's saved stack (blocked call chain).
    blocked.py PORT SECONDS OUT.bin [HZ]   records: time, kind(0 ui running,1 ui blocked), sp, 1024 bytes"""
import socket, struct, sys, time
port, secs, out = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
hz = float(sys.argv[4]) if len(sys.argv) > 4 else 200
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
    out = b''
    while n:
        k = min(n, 1024); d = send(b'm%x,%x' % (a, k))
        out += b'\0' * k if d.startswith(b'E') else bytes.fromhex(d.decode()); a += k; n -= k
    return out
UILO, UIHI = 0x0bedc000, 0x0bee0000
send(b'?')
f = open(out, 'wb'); ui_tcb = None; spoff = None; n = 0
t_end = time.time() + secs
while time.time() < t_end:
    send(b'c', reply=False); time.sleep(1.0 / hz); s.sendall(b'\x03'); recv()
    r = struct.unpack('<16I', bytes.fromhex(send(b'g').decode())[:64])
    t = struct.unpack('<I', mem(0x3c7000b4, 4))[0]
    tcb = struct.unpack('<I', mem(0x2200acf4, 4))[0]
    if UILO <= r[13] < UIHI:
        if ui_tcb != tcb: ui_tcb = tcb
        f.write(struct.pack('<III', t, 0, r[13]) + mem(r[13], 1024)); n += 1
    elif ui_tcb:
        words = struct.unpack('<64I', mem(ui_tcb, 256))
        if spoff is None:
            for k, w in enumerate(words):
                if UILO <= w < UIHI: spoff = k; break
        if spoff is not None:
            sp = words[spoff]
            f.write(struct.pack('<III', t, 1, sp) + mem(sp, 1024)); n += 1
send(b'c', reply=False)
print(n, 'records, UI TCB', hex(ui_tcb or 0), 'sp field', spoff)
