#!/bin/sh
# Boot one of our OS images in the iPod Classic emulator (build/qemu), in a window.
#   tools/qemu/boot.sh ospatch/builds/<image>.dec [disk.img]
# The disk defaults to build/qemu/run/E3.img (5 albums, real Preferences); a copy is used so
# the original stays clean. The image's 0x800 header is stripped, and the iTunesDB signature
# check is bypassed in that copy (the emulator's SHA-1 path does not match the hardware).
set -e
R=$(cd "$(dirname "$0")/../.." && pwd); Q=$R/build/qemu
IMG=${1:?image}; DISK=${2:-$Q/run/E3.img}
python3 - "$IMG" "$Q/run/boot-emu.bin" <<'PY'
import struct, sys
b = bytearray(open(sys.argv[1], 'rb').read()[0x800:])
off = 0x08387f78 - 0x08000000 + 0xaed8
if struct.unpack_from('<I', b, off)[0] == 0x159f4024: struct.pack_into('<I', b, off, 0xe1a00000)
open(sys.argv[2], 'wb').write(b)
PY
cp "$DISK" "$Q/run/boot-disk.img"
IPOD_SYSINFO=$Q/run/sysinfo-real.bin exec "$Q/src/build/qemu-system-arm" -M iPod-Classic \
    -kernel "$Q/run/boot-emu.bin" -serial null -drive if=ide,format=raw,file="$Q/run/boot-disk.img" \
    -display cocoa -monitor unix:$Q/run/mon.sock,server,nowait
