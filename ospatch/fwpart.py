#!/usr/bin/env python3
"""Puts our OS image into the iPod's firmware partition as a new 'osfl' section, leaving Apple's
stock osos untouched (the Rockbox bootloader boots osfl; MENU still boots stock through
Apple's bootloader).

  fwpart.py add  PARTITION.bin OS.dec OUT.bin    new partition image with the osfl section
  fwpart.py show PARTITION.bin                   list the MSE directory
  fwpart.py verify PARTITION.bin OS.dec          check the osfl section holds OS.dec

Partition layout (S5L8702 "MSE"): ']ih[' at +0x100, directory at +0x5000, 12 entries of 40
bytes {tag 'ATA!' (bytes reversed), type (reversed), u32, devOff, len, addr, entryOff,
checksum, version, load}. A section's data starts at devOff + 0x1000. The checksum is the
32-bit sum of the section's bytes over [devOff + 0x1000, +len).
Write the result with:  sudo dd if=OUT.bin of=/dev/rdisk<N>s2 bs=1m   (iPod in disk mode)."""
import struct, sys

DIR = 0x5000
ENT = 40
NENT = 12
OSFL_OFF = 0x5944000          # first 4 KiB block after the hash section's data
TYPE = b'osfl'


def entries(img):
    out = []
    for i in range(NENT):
        e = img[DIR + i * ENT:DIR + (i + 1) * ENT]
        if e[:4] in (b'\0\0\0\0', b'\xff\xff\xff\xff'):
            out.append(None); continue
        tag, typ = e[:4][::-1], e[4:8][::-1]
        out.append((tag, typ) + struct.unpack_from('<8I', e, 8))
    return out


def checksum(data):
    return sum(data) & 0xffffffff


def show(img):
    for i, e in enumerate(entries(img)):
        if e is None: print('  %2d (empty)' % i); continue
        tag, typ, u, dev, ln, addr, eoff, chk, ver, load = e
        print('  %2d %s %s devOff %08x len %08x addr %08x entryOff %08x chk %08x ver %08x load %08x' % (i, tag, typ, dev, ln, addr, eoff, chk, ver, load))


def main():
    cmd = sys.argv[1]
    img = bytearray(open(sys.argv[2], 'rb').read())
    assert img[0x100:0x104] == b']ih[', 'not a firmware partition image'
    if cmd == 'show':
        show(img); return
    os_img = open(sys.argv[3], 'rb').read()
    assert os_img[:4] == b'8702', 'not an IMG1 OS image'
    ents = entries(img)
    end = max(e[3] + 0x1000 + e[4] for e in ents if e and e[1] != TYPE)
    assert OSFL_OFF >= (end + 0xfff) & ~0xfff, 'osfl offset overlaps a section'
    if cmd == 'verify':
        e = next((e for e in ents if e and e[1] == TYPE), None)
        assert e, 'no osfl section'
        data = bytes(img[e[3] + 0x1000:e[3] + 0x1000 + e[4]])
        ok = data == os_img and e[7] == checksum(data)
        print('osfl at %08x, %d bytes, checksum %08x: %s' % (e[3], e[4], e[7], 'matches OS.dec' if ok else 'MISMATCH'))
        sys.exit(0 if ok else 1)
    assert cmd == 'add'
    assert OSFL_OFF + 0x1000 + len(os_img) <= len(img), 'image too big for the partition'
    slot = next((i for i, e in enumerate(ents) if e and e[1] == TYPE), None)
    if slot is None:
        slot = next(i for i, e in enumerate(ents) if e is None)
    tmpl = img[DIR + 1 * ENT:DIR + 2 * ENT]          # the osos entry: same tag, addr, version
    e = bytearray(tmpl)
    e[4:8] = TYPE[::-1]
    struct.pack_into('<I', e, 8, 0)
    struct.pack_into('<I', e, 0x0c, OSFL_OFF)
    struct.pack_into('<I', e, 0x10, len(os_img))
    struct.pack_into('<I', e, 0x18, 0)
    struct.pack_into('<I', e, 0x1c, checksum(os_img))
    img[DIR + slot * ENT:DIR + (slot + 1) * ENT] = e
    img[OSFL_OFF:OSFL_OFF + 0x1000] = b'\0' * 0x1000
    img[OSFL_OFF + 0x1000:OSFL_OFF + 0x1000 + len(os_img)] = os_img
    open(sys.argv[4], 'wb').write(img)
    print('wrote %s: osfl in slot %d at %08x, %d bytes, checksum %08x' % (sys.argv[4], slot, OSFL_OFF, len(os_img), checksum(os_img)))
    show(img)


if __name__ == '__main__':
    main()
