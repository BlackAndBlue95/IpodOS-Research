#!/bin/sh
# builds region E: e.bin (raw, runs at 0x08b33000) and e.syms (nm output for mkpatch18.py)
cd "$(dirname "$0")" || exit 1
T=${ARM_TOOLCHAIN:-$(cd .. && pwd)/build/arm-gnu-toolchain-13.3.rel1-darwin-arm64-arm-none-eabi/bin}
SRC="libload.c log.c osfile.c update.c updscreen.c uifast.c artdb.c fatdir.c path.c power.c theme.c theme_rules.c settings.c sun.c ossync.c
     flac/flacblob.c flac/cover.c flac/tjpgd.c flac/rbjpeg.c flac/rbflac.c flac/rbflac_arm.S
     flac/rbfir.c flac/rbfir_arm.S"
# PROF=1: the library-load profiler (tools/gen_prof.py writes prof_tramp.S)
DEFS="$EDEFS"
[ -n "$PROF" ] && { SRC="$SRC prof.c prof_tramp.S"; DEFS="$DEFS -DPROF"; }
$T/arm-none-eabi-gcc $DEFS -O2 -Wall -Wno-unused-function -Wno-misleading-indentation \
    -marm -mcpu=arm926ej-s -ffreestanding -nostdlib -fno-builtin -fno-common \
    -ffunction-sections -fdata-sections -I. -Iflac -I../libsync \
    -Wl,--gc-sections -Wl,--no-warn-rwx-segments -T e.ld $SRC -lgcc -o e.elf || exit 1
$T/arm-none-eabi-objcopy -O binary -j .e e.elf e.bin || exit 1
$T/arm-none-eabi-nm e.elf | sort > e.syms
ls -l e.bin
