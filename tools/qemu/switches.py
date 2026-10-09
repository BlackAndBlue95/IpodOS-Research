#!/usr/bin/env python3
"""Record the OS's task switches made through a kernel call, between two guest times.
The kernel call entry (0x0802dca8) pushes the caller's context on its stack and calls the
scheduler 0x0802cf08(old_frame) -> new_frame. Breakpoints: 0x0802dcd4 (r0 = frame of the task
going to sleep) and 0x0802dcd8 (r0 = frame of the task resuming).

    switches.py PORT T0_MS T1_MS OUT.bin

OUT.bin records: kind (0 out, 1 in), time_us, frame, then 96 words from the frame (out only)."""
import socket, struct, sys, time
port, t0, t1, out = int(sys.argv[1]), int(sys.argv[2]) * 1000, int(sys.argv[3]) * 1000, sys.argv[4]
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
    return b'\0' * n if d.startswith(b'E') else bytes.fromhex(d.decode())
def now(): return struct.unpack('<I', mem(0x3c7000b4, 4))[0]
send(b'?')
while True:                                   # run to t0 in short slices
    send(b'c', reply=False); time.sleep(0.02); s.sendall(b'\x03'); recv()
    if now() >= t0: break
send(b'Z0,802dcd4,4'); send(b'Z0,802dcd8,4')
f = open(out, 'wb'); n = 0
first = True
while True:
    if not first:
        send(b'z0,802dcd4,4'); send(b'z0,802dcd8,4'); send(b's'); send(b'Z0,802dcd4,4'); send(b'Z0,802dcd8,4')
    first = False
    send(b'c', reply=False); recv()
    r = bytes.fromhex(send(b'g').decode())
    pc, r0 = struct.unpack_from('<I', r, 60)[0], struct.unpack_from('<I', r, 0)[0]
    t = now()
    if pc == 0x802dcd4:
        f.write(struct.pack('<III', 0, t, r0) + mem(r0, 384))
    else:
        f.write(struct.pack('<III', 1, t, r0) + b'\0' * 384)
    n += 1
    if t >= t1: break
send(b'z0,802dcd4,4'); send(b'z0,802dcd8,4'); send(b'c', reply=False)
f.close()
print(n, 'switches')
