#!/usr/bin/env python3
"""Build an iPod Classic 6G disk image whose MBR is written in 4096-byte units.

The retailOS locates the firmware partition in 0x0808f8f4: it reads ONE device
block at block 0, checks 0xAA55 at +0x1FE, walks the four MBR entries looking
for a type byte of 0x00 or 0x3F, and takes that entry's start/size *in device
blocks*.  The device block on an iPod Classic is 4096 bytes (measured: the
first DMA read of every boot is 8 x 512-byte sectors at LBA 0), so the LBA
fields in the partition table count 4096-byte blocks, not sectors.

    mkdisk6g4k.py <out> <firmware-partition-image> <fat32-image>
"""
import struct
import sys
import os

BS = 4096
out, fwp, fat = sys.argv[1], sys.argv[2], sys.argv[3]
fwb = open(fwp, 'rb').read()
fatb = bytearray(open(fat, 'rb').read())


def mark_fat_volume(fatb):
    """Make the retailOS accept the data partition as a FAT volume.

    Measured (wiki/13, 2026-08-12): the firmware's generic volume classifier
    0x0808253c reads block 0 of the partition, sees the 0x55AA at +0x1FE that
    every FAT boot sector carries, and hands it to the "sector with 0xAA55"
    handler 0x0806a128 -- i.e. it treats the FAT boot sector as if it might be
    a nested MBR. That handler finds no valid partition entry (buf+0x1BE is
    zeros on a FAT VBR) and falls through to a last-resort ASCII probe:
    memcmp(boot+0x34,"FAT",3) then memcmp(boot+0x2b,"FAT",3). mkfs.vfat leaves
    both spots zero (the real "FAT32   " sits at +0x52, which this path never
    looks at), so both memcmps miss and 0x0806a218 rejects the volume with
    code 31 -> "No Music", the directory is never read.

    Writing the three bytes "FAT" into BPB_Reserved (+0x34, reserved-for-future,
    ignored by every real FAT driver) flips 0x0806a128 to success and the
    classifier from reject(31) to accept(0). VERIFIED end to end: with this
    marker a keyless boot reads the FAT, the root directory and iPod_Control
    (30 DMA reads past the boot sector, plus write-backs) instead of the single
    boot-sector read a rejected volume gets. Apply it to the primary boot sector
    and its backup (BkBootSec).
    """
    if len(fatb) < 0x54 or fatb[0x1fe:0x200] != b'\x55\xaa':
        return fatb
    bps = struct.unpack('<H', fatb[0x0b:0x0d])[0] or 512
    bk = struct.unpack('<H', fatb[0x32:0x34])[0]
    for base in {0, bk * bps}:
        if base + 0x37 <= len(fatb):
            fatb[base + 0x34:base + 0x37] = b'FAT'
    return fatb


def zero_itunesdb_hash58(fatb):
    """Blank every iTunesDB hash58 so THIS emulator accepts the music library.

    From the 2007 models on (Classic 6G, Nano 3G) the retailOS refuses to load
    an iTunesDB unless its hash58 field (mhbd+0x58, 20 bytes) equals an
    HMAC-SHA1 the firmware recomputes over the whole file, keyed off the
    device's FireWire GUID. VERIFIED byte for byte (wiki/13 s.8): the validator
    0x08073cf8 derives the key from the GUID zone, runs the HMAC, and does
    memcmp(computed, mhbd+0x58, 20) at 0x08328598; a miss returns 0xffff5bda
    and the library shows "No Music".

    The catch is not the algorithm or the GUID -- it is that the S5L8702 SHA-1
    is a hardware accelerator at SHA1_MEM_BASE (0x38000000, driver 0x08068b64)
    which QEMU does NOT model: it sits in the "peripherals we know the address
    of but have not modelled" table (hw/arm/ipod_classic/ipod_classic.c), so its
    reads return 0. With the SHA engine stuck at zero every digest is zero, the
    HMAC key is zero, and the signature the firmware EXPECTS collapses to 20
    zero bytes. MEASURED at the memcmp: expected = 00..00. So the one signature
    this firmware accepts today is 20 zeros -- which is exactly what libgpod does
    not write (it writes a real HMAC for the SysInfo GUID). Blank it here, in
    every copy (libgpod keeps the live DB plus a mirror).

    CONVENIENCE STUB, not hardware behaviour. Once 0x38000000 is modelled this
    must go and the DB be signed (libgpod) for the device's real GUID instead.
    Proven end to end: a disk built this way lists the track in Music -> Songs
    (wiki/13 s.8, ~/6g/song_listed/).
    """
    n, off = 0, 0
    while True:
        i = fatb.find(b'mhbd', off)
        if i < 0:
            break
        off = i + 4
        # Guard against a stray "mhbd" in audio data: a real header carries a
        # small header length at +4 and its hashing_scheme at +0x30.
        hdrlen = struct.unpack('<I', fatb[i + 4:i + 8])[0]
        if not (0x40 <= hdrlen <= 0x400) or i + 0x6c > len(fatb):
            continue
        fatb[i + 0x58:i + 0x58 + 20] = b'\0' * 20
        n += 1
    return n


fatb = mark_fat_volume(fatb)
n_hash = 0  # the SHA-1 engine is modelled: keep real signatures


def pad(b):
    return b + b'\0' * (-len(b) % BS)


fwb, fatb = pad(fwb), pad(fatb)
fw_start = 8                       # block 8 = byte 0x8000
fw_blocks = len(fwb) // BS
fat_start = fw_start + fw_blocks
fat_blocks = len(fatb) // BS

mbr = bytearray(BS)


def ent(i, ptype, lba, cnt):
    off = 0x1be + 16 * i
    mbr[off + 4] = ptype
    mbr[off + 8:off + 12] = struct.pack('<I', lba)
    mbr[off + 12:off + 16] = struct.pack('<I', cnt)


ent(0, 0x00, fw_start, fw_blocks)
ent(1, 0x0b, fat_start, fat_blocks)
mbr[0x1fe:0x200] = b'\x55\xaa'

with open(out, 'wb') as f:
    f.write(bytes(mbr))
    f.write(b'\0' * (BS * (fw_start - 1)))
    f.write(fwb)
    f.write(fatb)

print("4K-unit MBR: fw block %d (+%d), fat block %d (+%d); %d MB; "
      "hash58 blanked in %d iTunesDB cop%s"
      % (fw_start, fw_blocks, fat_start, fat_blocks,
         os.path.getsize(out) // (1024 * 1024),
         n_hash, "y" if n_hash == 1 else "ies"))
