#!/usr/bin/env python3
"""Boot an image on a clone of a disk, wait for its first frame, then play keys and take screenshots.
    screens.py IMAGE.dec DISK.img OUT_PREFIX [KEY|waitN|shot ...]
Keys are monitor sendkey names (e/q wheel, ret select, w menu, a/d prev/next, s play); waitN sleeps
N seconds; shot saves OUT_PREFIX-NN.png. Without any shot, one screenshot OUT_PREFIX.png at the end.
Prints the run's first-frame, glyph-pass, pre-build, fbdmg and CRASH log lines (GREP=a,b adds more). Clone and kernel are removed."""
import os, socket, struct, subprocess, sys, time

R = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
RUN = os.path.join(R, 'build/qemu/run')
FAT_OFF = 49160 * 4096
img, disk, out = sys.argv[1], sys.argv[2], sys.argv[3]
keys = sys.argv[4:]
tag = 'Z-scr%d' % os.getpid()
clone, kernel, mon = (os.path.join(RUN, tag + x) for x in ('.img', '.bin', '.sock'))

b = bytearray(open(img, 'rb').read()[0x800:])
off = 0x08387f78 - 0x08000000 + 0xaed8                    # iTunesDB signature check off, as boot.sh does
if struct.unpack_from('<I', b, off)[0] == 0x159f4024: struct.pack_into('<I', b, off, 0xe1a00000)
open(kernel, 'wb').write(b)
region = 'region E 08b33000-%08x' % (0x08b33000 + len(b) - 0xb3ded8)
subprocess.run(['cp', '-c', disk, clone], check=True)
q = subprocess.Popen([os.path.join(R, 'build/qemu/src/build/qemu-system-arm'), '-M', 'iPod-Classic', '-kernel', kernel,
                      '-serial', 'null', '-display', 'none', '-drive', 'if=ide,format=raw,file=' + clone,
                      '-monitor', 'unix:%s,server,nowait' % mon],
                     env=dict(os.environ, IPOD_SYSINFO=os.path.join(RUN, 'sysinfo-real.bin')),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def rtlog():
    r = subprocess.run([sys.executable, os.path.join(R, 'tools/qemu/fatcat.py'), clone, 'FLAC\\rtlog.txt', str(FAT_OFF)],
                       capture_output=True, text=True, errors='replace').stdout
    i = r.rfind(region)
    return r[i:] if i >= 0 else ''

def monitor(cmd):
    s = socket.socket(socket.AF_UNIX); s.connect(mon); s.sendall((cmd + '\n').encode()); time.sleep(0.3); s.close()

def shot(name):
    monitor('screendump %s.ppm' % name); time.sleep(0.5)
    subprocess.run([sys.executable, os.path.join(R, 'tools/qemu/ppm2png.py'), name + '.ppm', name + '.png'], capture_output=True)
    if os.path.exists(name + '.ppm'): os.remove(name + '.ppm')

try:
    for _ in range(240):
        if 'first frame' in rtlog(): break
        time.sleep(0.25)
    n = 0
    for k in keys:
        if k.startswith('wait'): time.sleep(float(k[4:] or 1)); continue
        if k == 'shot': n += 1; shot('%s-%02d' % (out, n)); continue
        monitor('sendkey %s%s' % (k, ' 400' if k in ('ret', 'w') else '')); time.sleep(0.6)
    if not n: time.sleep(0.5); shot(out)
    time.sleep(1)
finally:
    q.kill(); q.wait()
    print(' | '.join(l.strip() for l in rtlog().splitlines()
                     if any(w in l for w in ('CRASH', 'first frame', 'pre-build done', 'glyph pass', 'fbdmg')
                            + tuple(w for w in os.environ.get('GREP', '').split(',') if w))))
    for f in (clone, kernel, mon):
        if os.path.exists(f): os.remove(f)
