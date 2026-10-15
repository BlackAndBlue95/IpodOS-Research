#!/usr/bin/env python3
"""Video playback test in the joel emulator: boots IMAGE (.dec) on a clone of the episode disk,
plays the first episode of the first show and counts H.264 access units handed to the decoder
(code breakpoint on its front-end 0x081b6564). PASS when units arrive within 40 s.
    videotest.py IMAGE.dec [DISK.img]   (disk default build/qemu/run/Z-rb.img; its first show's
                                         first episode is played)"""
import os, socket, struct, subprocess, sys, time

R = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
RUN = os.path.join(R, 'build/qemu/run')
QEMU = os.path.join(R, 'build/qemu/src-joel/build/qemu-system-arm')
FRONT_END = 0x081b6564
FAT_OFF = 49160 * 4096
NEED, WAIT_S = 3, 40
_s = socket.socket(); _s.bind(('127.0.0.1', 0)); GDB_PORT = _s.getsockname()[1]; _s.close()   # a free port per run

img, disk = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else os.path.join(RUN, 'Z-rb.img')
show = int(sys.argv[3]) if len(sys.argv) > 3 else 0          # rows to move down in TV Shows
tag = 'Z-vt%d' % os.getpid()
clone, mon, kernel, qout = (os.path.join(RUN, tag + x) for x in ('.img', '.sock', '.bin', '.out'))

def kernel_from(dec):
    """the body without its header; the iTunesDB signature check off, as boot.sh does. Returns the
    image's region E range as its log prints it"""
    b = bytearray(open(dec, 'rb').read()[0x800:])
    off = 0x08387f78 - 0x08000000 + 0xaed8
    if struct.unpack_from('<I', b, off)[0] == 0x159f4024: struct.pack_into('<I', b, off, 0xe1a00000)
    open(kernel, 'wb').write(b)
    return 'region E 08b33000-%08x' % (0x08b33000 + len(b) - 0xb3ded8)

def rtlog():
    r = subprocess.run([sys.executable, os.path.join(R, 'tools/qemu/fatcat.py'), clone, 'FLAC\\rtlog.txt', str(FAT_OFF)],
                       capture_output=True, text=True, errors='replace')
    return r.stdout

def keys(*ks):
    for k in ks:
        if k == 'wait': time.sleep(2); continue
        s = socket.socket(socket.AF_UNIX); s.connect(mon)
        s.sendall(('sendkey %s%s\n' % (k, ' 400' if k in ('ret', 'w') else '')).encode()); time.sleep(0.2); s.close()
        time.sleep(1 if k in ('ret', 'w') else 0.6)

def shot(name):
    """screen into ./videotest-NAME.png"""
    ppm = os.path.join(RUN, '%s-%s.ppm' % (tag, name))
    s = socket.socket(socket.AF_UNIX); s.connect(mon); s.sendall(('screendump %s\n' % ppm).encode()); time.sleep(0.5); s.close()
    if os.path.exists(ppm):
        png = os.path.join(os.getcwd(), 'videotest-%s.png' % name)
        subprocess.run([sys.executable, os.path.join(R, 'tools/qemu/ppm2png.py'), ppm, png], capture_output=True)
        os.remove(ppm)
        return png

class Gdb:
    def __init__(self):
        for _ in range(100):
            try: self.s = socket.create_connection(('127.0.0.1', GDB_PORT)); break
            except OSError: time.sleep(0.1)
        self.buf = b''
    def recv(self, timeout=None):
        self.s.settimeout(timeout)
        while True:
            i = self.buf.find(b'$')
            if i >= 0:
                j = self.buf.find(b'#', i)
                if j >= 0 and len(self.buf) >= j + 3:
                    d = self.buf[i + 1:j]; self.buf = self.buf[j + 3:]; self.s.sendall(b'+'); return d
            c = self.s.recv(65536)
            if not c: raise EOFError
            self.buf += c
    def send(self, cmd, reply=True):
        self.s.sendall(b'$' + cmd + b'#' + b'%02x' % (sum(cmd) & 0xff))
        return self.recv() if reply else None

def main():
    region = kernel_from(img)
    subprocess.run(['cp', '-c', disk, clone], check=True)
    # the frame marker pulses from boot, as on the device (EIC_FMARK=panel waits for the panel's
    # R090h write, which the device does not need): without pulses the frame pool fills and stops
    env = dict(os.environ, DYLD_FALLBACK_LIBRARY_PATH=os.path.join(RUN, '../deps-av/lib'), LCDTYPE='2',
               IRAMSPILL='0x20000', SDO_FRAME_IRQ='1', IPOD_SYSINFO=os.path.join(RUN, 'sysinfo-real.bin'))
    env.pop('EIC_FMARK', None)
    q = subprocess.Popen([QEMU, '-M', 'iPod-Classic,semihosting=off', '-kernel', kernel, '-serial', 'null',
                          '-drive', 'if=ide,format=raw,file=' + clone, '-display', 'none',
                          '-monitor', 'unix:%s,server,nowait' % mon, '-gdb', 'tcp::%d' % GDB_PORT],
                         env=env, stdout=open(qout, 'w'), stderr=subprocess.STDOUT)
    try:
        for _ in range(240):                       # this image's own boot and first frame (the clone holds older logs)
            t = rtlog()
            if os.path.exists(mon) and region in t and 'first frame' in t.split(region)[-1]: break
            time.sleep(0.5)
        else:
            print('FAIL: no first frame (qemu %s)' % ('running' if q.poll() is None else 'exited %d' % q.returncode))
            print(''.join(open(qout).readlines()[-5:]), end='')
            print('log tail:', '\n'.join(rtlog().splitlines()[-4:]))
            return 1
        for _ in range(60):                        # lazy builds: the background screen build first
            t = rtlog().split(region)[-1]
            if 'pre-build done' in t or 'screens and their glyphs' not in t: break
            time.sleep(0.5)
        time.sleep(3)
        keys('e', 'ret', 'wait', 'wait', 'e', 'e', 'ret', 'wait', 'wait', *(['e'] * show), 'wait')
        shot('shows')                              # the show list with the one to be played selected
        keys('ret', 'wait', 'wait', 'ret', 'wait', 'wait')
        g = Gdb()
        g.send(b'?')                               # QEMU stops the guest when gdb connects; then a breakpoint
        g.send(b'Z0,%x,4' % FRONT_END)
        hits, end = 0, time.time() + WAIT_S
        while hits < NEED and time.time() < end:
            g.send(b'c', reply=False)
            try: r = g.recv(max(0.1, end - time.time()))
            except socket.timeout:
                g.s.sendall(b'\x03'); g.recv(10); break
            if r.startswith(b'T') or r.startswith(b'S'):
                hits += 1
                g.send(b'z0,%x,4' % FRONT_END); g.send(b's'); g.send(b'Z0,%x,4' % FRONT_END)
        g.send(b'z0,%x,4' % FRONT_END); g.send(b'D')
        end_png = shot('end')
        ok = hits >= NEED
        print('%s: %d access units reached the decoder (needed %d) within %d s' % ('PASS' if ok else 'FAIL', hits, NEED, WAIT_S))
        if not ok:
            for l in rtlog().split(region)[-1].splitlines():
                if 'sync:' in l or 'CRASH' in l or 'CHECK' in l: print(l)
        print('screens: videotest-shows.png (the show played), %s' % end_png)
        return 0 if ok else 1
    finally:
        q.kill(); q.wait()
        for f in (clone, mon, kernel, qout):
            if os.path.exists(f): os.remove(f)

rc = main()
if rc:                                             # a dropped key leaves it in a menu: one more boot
    print('retrying once (key timing)')
    _s = socket.socket(); _s.bind(('127.0.0.1', 0)); GDB_PORT = _s.getsockname()[1]; _s.close()
    rc = main()
sys.exit(rc)
