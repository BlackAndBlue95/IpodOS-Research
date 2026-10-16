"""FAT16 volume (the firmware's rsrc section) grown in place, keeping every cluster number.

    vol = Fat16(bytes); vol.add('Resources/Icons/Modern.bin', data); new = vol.build()"""
import struct, time


def _lfn_sum(sfn):
    s = 0
    for c in sfn:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xff
    return s


def _dostime(t):
    lt = time.localtime(t)
    return (lt.tm_hour << 11 | lt.tm_min << 5 | lt.tm_sec // 2,
            (lt.tm_year - 1980) << 9 | lt.tm_mon << 5 | lt.tm_mday)


class Fat16:
    def __init__(self, vol):
        self.boot = bytearray(vol[:512])
        b = self.boot
        assert b[0x1fe:0x200] == b'\x55\xaa' and b[0x36:0x3b] == b'FAT16', 'not a FAT16 volume'
        self.bps, self.spc, self.res, self.nf, self.nroot = struct.unpack_from('<HBHBH', b, 11)
        tot = struct.unpack_from('<H', b, 19)[0] or struct.unpack_from('<I', b, 32)[0]
        self.fatsz = struct.unpack_from('<H', b, 22)[0]
        self.cb = self.bps * self.spc
        self.reserved = bytes(vol[512:self.res * self.bps])
        fat = self.res * self.bps
        self.root_off = fat + self.nf * self.fatsz * self.bps
        self.rootsecs = (self.nroot * 32 + self.bps - 1) // self.bps
        data = self.root_off + self.rootsecs * self.bps
        self.nclus = (tot - data // self.bps) // self.spc
        self.fat = list(struct.unpack_from('<%dH' % (self.nclus + 2), vol, fat))
        self.root = bytearray(vol[self.root_off:self.root_off + self.nroot * 32])
        self.data = bytearray(vol[data:data + self.nclus * self.cb])
        self.extra = tot - data // self.bps - self.nclus * self.spc    # sectors past the last cluster
        self.tail = bytes(vol[data + self.nclus * self.cb:])
        self.now = time.time()

    def getc(self, c):
        return bytes(self.data[(c - 2) * self.cb:(c - 1) * self.cb])

    def setc(self, c, v):
        self.data[(c - 2) * self.cb:(c - 1) * self.cb] = bytes(v).ljust(self.cb, b'\0')

    # ---- clusters
    def free_count(self):
        return sum(1 for c in range(2, self.nclus + 2) if not self.fat[c])

    def grow(self, add):
        self.nclus += add
        assert self.nclus <= 65524, 'more clusters than FAT16 allows'
        self.fat += [0] * add
        self.data += bytes(add * self.cb)
        self.tail = bytes(len(self.tail))           # Apple's tail is the next section's header page

    def alloc(self, n):
        """n clusters, contiguous when possible, chained"""
        run, start = 0, None
        for c in range(2, self.nclus + 2):
            run = run + 1 if not self.fat[c] else 0
            if run == n:
                start = c - n + 1
                break
        cl = list(range(start, start + n)) if start else [c for c in range(2, self.nclus + 2) if not self.fat[c]][:n]
        assert len(cl) == n, 'volume full'
        for a, b in zip(cl, cl[1:] + [0xffff]):
            self.fat[a] = b
        return cl

    def chain(self, c):
        out = []
        while 2 <= c < 0xfff8:
            out.append(c)
            c = self.fat[c]
        return out

    def release(self, c):
        for k in self.chain(c):
            self.fat[k] = 0

    # ---- directories: (bytes, writer) for the root or a cluster chain
    def _dir(self, first):
        if first == 0:
            return bytearray(self.root), None
        cl = self.chain(first)
        return bytearray(b''.join(self.getc(c) for c in cl)), cl

    def _store(self, first, buf, cl):
        if first == 0:
            self.root[:] = buf
            return
        for i, c in enumerate(cl):
            self.setc(c, buf[i * self.cb:(i + 1) * self.cb])

    def entries(self, first):
        """(long name, short name, attr, cluster, size, index of the first slot, slots)"""
        buf, _ = self._dir(first)
        out, lfn, start = [], {}, None
        for i in range(len(buf) // 32):
            e = buf[32 * i:32 * i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xe5:
                lfn, start = {}, None
                continue
            if e[11] == 0x0f:
                if start is None:
                    start = i
                seq = e[0] & 0x1f
                lfn[seq] = (e[1:11] + e[14:26] + e[28:32]).decode('utf-16-le', 'replace')
                continue
            sfn = bytes(e[:11])
            name = ''.join(lfn[k] for k in sorted(lfn)).split('\0')[0] if lfn else None
            base, ext = sfn[:8].decode('latin1').rstrip(), sfn[8:].decode('latin1').rstrip()
            out.append((name or (base + ('.' + ext if ext else '')), sfn, e[11],
                        struct.unpack_from('<H', e, 26)[0], struct.unpack_from('<I', e, 28)[0],
                        start if start is not None else i, i))
            lfn, start = {}, None
        return out

    def lookup(self, first, name):
        for e in self.entries(first):
            if e[0].lower() == name.lower() and e[0] not in ('.', '..'):
                return e
        return None

    def _sfn(self, first, name):
        taken = {e[1] for e in self.entries(first)}
        stem, _, ext = name.rpartition('.') if '.' in name[1:] else (name, '', '')
        clean = lambda s, n: ''.join(c for c in s.upper() if c.isalnum() or c in '_-$')[:n]
        base, ex = clean(stem, 8), clean(ext, 3)
        cand = (base.ljust(8) + ex.ljust(3)).encode()
        if base == stem and ex == ext and cand not in taken:
            return cand
        for k in range(1, 1000):
            t = '~%d' % k
            cand = (clean(stem, 8 - len(t)) + t).ljust(8).encode() + ex.ljust(3).encode()
            if cand not in taken:
                return cand
        raise AssertionError('no short name for ' + name)

    def _put(self, first, name, attr, cluster, size):
        sfn = self._sfn(first, name)
        units = name.encode('utf-16-le')
        n = (len(units) // 2 + 12) // 13
        chars = list(struct.unpack('<%dH' % (len(units) // 2), units)) + [0] + [0xffff] * 13
        slots = []
        for k in range(n, 0, -1):
            part = chars[(k - 1) * 13:k * 13]
            e = bytearray(32)
            e[0] = k | (0x40 if k == n else 0)
            e[11], e[13] = 0x0f, _lfn_sum(sfn)
            struct.pack_into('<5H', e, 1, *part[:5])
            struct.pack_into('<6H', e, 14, *part[5:11])
            struct.pack_into('<2H', e, 28, *part[11:13])
            slots.append(bytes(e))
        e = bytearray(32)
        e[:11], e[11] = sfn, attr
        tm, dt = _dostime(self.now)
        struct.pack_into('<HHHHHHHI', e, 14, tm, dt, dt, 0, tm, dt, cluster, size)
        slots.append(bytes(e))
        buf, cl = self._dir(first)
        need, free, i = len(slots), 0, 0
        while True:
            if i == len(buf) // 32:                       # the folder grows by a cluster
                assert first, 'root directory full'
                cl.append(self.alloc(1)[0])
                self.fat[cl[-2]] = cl[-1]
                buf += b'\0' * self.cb
            if buf[32 * i] in (0, 0xe5):
                free += 1
                if free == need:
                    at = i - need + 1
                    break
            else:
                free = 0
            i += 1
        for k, s in enumerate(slots):
            buf[32 * (at + k):32 * (at + k + 1)] = s
        self._store(first, buf, cl)

    def _drop(self, first, ent):
        buf, cl = self._dir(first)
        for i in range(ent[5], ent[6] + 1):
            buf[32 * i] = 0xe5
        self._store(first, buf, cl)

    def mkdir(self, first, name):
        e = self.lookup(first, name)
        if e:
            assert e[2] & 0x10, name + ' is a file'
            return e[3]
        c = self.alloc(1)[0]
        d = bytearray(self.cb)
        tm, dt = _dostime(self.now)
        for k, (n, cc) in enumerate(((b'.          ', c), (b'..         ', first))):
            d[32 * k:32 * k + 11] = n
            d[32 * k + 11] = 0x10
            struct.pack_into('<HHHHHHHI', d, 32 * k + 14, tm, dt, dt, 0, tm, dt, cc, 0)
        self.setc(c, d)
        self._put(first, name, 0x10, c, 0)
        return c

    def add(self, path, data):
        """path with '/' separators; replaces a file of that name"""
        parts = path.strip('/').split('/')
        d = 0
        for p in parts[:-1]:
            d = self.mkdir(d, p)
        old = self.lookup(d, parts[-1])
        if old:
            assert not old[2] & 0x10, path + ' is a folder'
            self.release(old[3])
            self._drop(d, old)
        n = (len(data) + self.cb - 1) // self.cb
        cl = self.alloc(n) if n else []
        for i, c in enumerate(cl):
            self.setc(c, data[i * self.cb:(i + 1) * self.cb])
        self._put(d, parts[-1], 0x20, cl[0] if cl else 0, len(data))

    def read(self, path):
        d, e = 0, None
        for p in path.strip('/').split('/'):
            e = self.lookup(d, p)
            if not e:
                return None
            d = e[3]
        return b''.join(self.getc(c) for c in self.chain(e[3]))[:e[4]]

    def clusters_for(self, files):
        """clusters a set of {path: data} needs beyond what is free (folders and slack counted)"""
        need = sum((len(d) + self.cb - 1) // self.cb for d in files.values()) + 8 + len(files) // 16
        return need

    def build(self):
        b = self.boot
        fatsz = ((self.nclus + 2) * 2 + self.bps - 1) // self.bps
        data = self.res + self.nf * fatsz + self.rootsecs
        tot = data + self.nclus * self.spc + self.extra
        struct.pack_into('<H', b, 22, fatsz)
        if tot < 0x10000:
            struct.pack_into('<H', b, 19, tot)
            struct.pack_into('<I', b, 32, 0)
        else:
            struct.pack_into('<H', b, 19, 0)
            struct.pack_into('<I', b, 32, tot)
        fat = bytearray(fatsz * self.bps)
        struct.pack_into('<%dH' % len(self.fat), fat, 0, *self.fat)
        out = bytearray(b) + self.reserved + bytes(fat) * self.nf + self.root
        out += b'\0' * (data * self.bps - len(out))
        return bytes(out + self.data + self.tail)
