"""Reader for the UI resource package inside Apple's iPod Classic OS 2.0.4 image.
Layout (DRAM addresses, DRAM = body offset + 0x07ff5128 in the .dec):
  package header 0x08400258: {u32 3, u32 index_size, u32 ntypes}, then ntypes x {fourcc (byte
  reversed), count, dense, index_off}; index entries {id, data_off, size}; data at
  0x08417928 + data_off; names table 0x083f8728: 3920 x {char *name, u32 id}.
BMap: 0x1C header {u16 fmt, u16 -, u16 stride, u16 bpp, u32 0, u32 0, u32 h, u32 w, u32 size}.
  fmt 0x64/0x65: u32 n + n x u32 ARGB palette, then stride*h 8/16-bit indices;
  0x04/0x08: 4/8-bit alpha; 0x565: RGB565; 0x1888: u32 ARGB."""
import struct, zlib

BASE = 0x08000000
PKG = 0x08400258
DATA = 0x08417928
NAMES = 0x083f8728


class Package:
    def __init__(self, dec_path):
        img = open(dec_path, 'rb').read()
        self.D = img[0x800 + 0xaed8:]            # the DRAM part, D[a - BASE]
        D = self.D
        self.u32 = lambda a: struct.unpack_from('<I', D, a - BASE)[0]
        self.u16 = lambda a: struct.unpack_from('<H', D, a - BASE)[0]
        ver, isz, nt = struct.unpack_from('<III', D, PKG - BASE)
        self.types = {}
        for i in range(nt):
            fcc, cnt, dense, ioff = struct.unpack_from('<4sIII', D, PKG - BASE + 12 + 16 * i)
            self.types[fcc[::-1].decode('latin-1')] = (cnt, dense, ioff)
        self.names, self.ids = {}, {}
        for k in range(3920):
            p, pid = struct.unpack_from('<II', D, NAMES - BASE + 8 * k)
            s = D[p - BASE:D.index(b'\0', p - BASE)].decode('latin-1')
            self.names[pid] = s
            self.ids[s] = pid

    def index(self, t):
        cnt, dense, ioff = self.types[t]
        return [struct.unpack_from('<III', self.D, PKG - BASE + ioff + 12 * k) for k in range(cnt)]

    def colr(self):
        """[(pid, name, rgb)]"""
        return [(pid, self.names.get(pid, '?'), self.u32(DATA + off) & 0xffffff) for pid, off, sz in self.index('COLR')]

    def bmap(self, pid):
        """(fmt, w, h, stride, palette or None, rows of RGBA bytes)"""
        off, sz = {p: (o, s) for p, o, s in self.index('BMap')}[pid]
        a = DATA + off
        D, u32, u16 = self.D, self.u32, self.u16
        fmt, _, stride, bpp = struct.unpack_from('<HHHH', D, a - BASE)
        h, w = u32(a + 0x10), u32(a + 0x14)
        p = a + 0x1c
        rows, pal = [], None
        if fmt in (0x64, 0x65):
            n = u32(p)
            pal = [u32(p + 4 + 4 * k) for k in range(n)]
            px = p + 4 + 4 * n
            for y in range(h):
                r = bytearray()
                for x in range(w):
                    i = D[px + y * stride + x - BASE] if fmt == 0x64 else u16(px + y * stride + 2 * x)
                    c = pal[i] if i < n else 0
                    r += bytes([(c >> 16) & 255, (c >> 8) & 255, c & 255, (c >> 24) & 255])
                rows.append(bytes(r))
        elif fmt == 0x565:
            for y in range(h):
                r = bytearray()
                for x in range(w):
                    c = u16(p + y * stride + 2 * x)
                    r += bytes([(c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3, 255])
                rows.append(bytes(r))
        elif fmt == 0x1888:
            for y in range(h):
                r = bytearray()
                for x in range(w):
                    c = u32(p + y * stride + 4 * x)
                    r += bytes([(c >> 16) & 255, (c >> 8) & 255, c & 255, (c >> 24) & 255])
                rows.append(bytes(r))
        elif fmt in (4, 8):
            for y in range(h):
                r = bytearray()
                for x in range(w):
                    if fmt == 8:
                        v = D[p + y * stride + x - BASE]
                    else:
                        b = D[p + y * stride + x // 2 - BASE]
                        v = (b >> 4 if x % 2 == 0 else b & 15) * 17
                    r += bytes([255, 255, 255, v])
                rows.append(bytes(r))
        else:
            raise ValueError('BMap format %#x' % fmt)
        return fmt, w, h, stride, pal, rows


def png(path, w, h, rows):
    raw = b''.join(b'\0' + r for r in rows)
    def ch(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))


class Canvas:
    """RGBA canvas for montages"""
    def __init__(self, w, h, bg=(0x40, 0x40, 0x40, 255)):
        self.w, self.h = w, h
        self.px = [bytearray(bytes(bg) * w) for _ in range(h)]

    def paste(self, rows, w, h, x0, y0, scale=1):
        for y in range(h):
            for sy in range(scale):
                yy = y0 + y * scale + sy
                if yy >= self.h: continue
                row = self.px[yy]
                for x in range(w):
                    r, g, b, a = rows[y][4 * x:4 * x + 4]
                    for sx in range(scale):
                        xx = x0 + x * scale + sx
                        if xx >= self.w: continue
                        if a == 255:
                            row[4 * xx:4 * xx + 4] = bytes([r, g, b, 255])
                        elif a:
                            dr, dg, db = row[4 * xx], row[4 * xx + 1], row[4 * xx + 2]
                            row[4 * xx:4 * xx + 4] = bytes([(r * a + dr * (255 - a)) // 255, (g * a + dg * (255 - a)) // 255,
                                                            (b * a + db * (255 - a)) // 255, 255])

    def fill(self, x0, y0, w, h, rgb):
        for y in range(y0, min(y0 + h, self.h)):
            for x in range(x0, min(x0 + w, self.w)):
                self.px[y][4 * x:4 * x + 4] = bytes([rgb[0], rgb[1], rgb[2], 255])

    def save(self, path):
        png(path, self.w, self.h, [bytes(r) for r in self.px])
