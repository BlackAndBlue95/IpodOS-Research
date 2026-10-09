#!/usr/bin/env python3
"""Sampling profiler for the emulated iPod: pauses the guest through the emulator's GDB port at a
fixed rate and records pc, lr, sp, the guest timer (0x3c7000b4) and 128 words of stack.

    sample.py PORT SECONDS OUT.bin [HZ] [STACK_BYTES]

OUT.bin: records of (time_us, pc, lr, sp, 128 stack words), little endian."""
import socket, struct, sys, time

port, secs, out = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
hz = float(sys.argv[4]) if len(sys.argv) > 4 else 200.0
STK = int(sys.argv[5]) if len(sys.argv) > 5 else 512      # bytes of stack per sample

s = None
for _ in range(100):
    try:
        s = socket.create_connection(('127.0.0.1', port)); break
    except OSError:
        time.sleep(0.1)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
buf = b''

def recv_packet():
    global buf
    while True:
        i = buf.find(b'$')
        if i >= 0:
            j = buf.find(b'#', i)
            if j >= 0 and len(buf) >= j + 3:
                data = buf[i + 1:j]
                buf = buf[j + 3:]
                s.sendall(b'+')
                return data
        chunk = s.recv(65536)
        if not chunk:
            raise EOFError
        buf += chunk

def send(cmd, reply=True):
    pkt = b'$' + cmd + b'#' + b'%02x' % (sum(cmd) & 0xff)
    s.sendall(pkt)
    return recv_packet() if reply else None

def regs():
    r = bytes.fromhex(send(b'g').decode())
    return struct.unpack_from('<16I', r, 0)

def mem(addr, n):
    d = send(b'm%x,%x' % (addr, n))
    if d.startswith(b'E'):
        return b'\0' * n
    return bytes.fromhex(d.decode())

send(b'?')                      # initial stop reason (the guest is stopped at connect)
f = open(out, 'wb')
n = 0
t_end = time.time() + secs
period = 1.0 / hz
while time.time() < t_end:
    send(b'c', reply=False)
    time.sleep(period)
    s.sendall(b'\x03')
    recv_packet()               # stop reply
    r = regs()
    pc, lr, sp = r[15], r[14], r[13]
    t = struct.unpack('<I', mem(0x3c7000b4, 4))[0]
    stk = mem(sp, STK) if 0x08000000 <= sp < 0x0c000000 or 0x22000000 <= sp < 0x22040000 else b'\0' * STK
    f.write(struct.pack('<4I', t, pc, lr, sp) + stk.ljust(STK, b'\0'))
    n += 1
send(b'c', reply=False)
f.close()
print('%d samples' % n)
