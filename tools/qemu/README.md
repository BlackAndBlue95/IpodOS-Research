# Emulator (iPod Classic 7G, firmware 2.0.4)

Based on davidmonterocrespo24/qemu-ipod-classic (6G). `ipod-classic-7g.patch` adds:
- `IPOD_SYSINFO=<file>`: SysInfo from a real iPod (`FLAC/sysinfo.bin`, written by our OS at boot);
  2.0.4 refuses a 6G hardware version (`IPOD_HWID` overrides only that field).
- ATA interrupt latched in bit 2 as well as bit 0 (the 7G driver waits with mask 4).
- A SHA-1 engine model at 0x38000000 (Rockbox's register usage). 2.0.4's iTunesDB check does not use
  it, so `boot.sh` bypasses that check in its copy of the image.

Build: clone the fork into `build/qemu/src`, apply the patch, get glib/pixman/openssl/sdl2 with
micromamba into `build/qemu/deps`, then `../configure --target-list=arm-softmmu --enable-sdl` and
`ninja qemu-system-arm` (about a minute). Disk: `mkdisk7g.py <out> <firmware partition dump> <FAT32 image>`.
Keys: q/e wheel, Enter select, w menu, a/d prev/next, s play.

Now Playing and video need the display compositor and video decoder of
[joelheaps/qemu-ipod-classic](https://github.com/joelheaps/qemu-ipod-classic) (`build/qemu/src-joel`). The other
screens work with either. For that tree use `ipod-classic-7g-joel.patch` (made against its commit 95bd22d):
the changes above (its ATA already latches bit 2) plus, for video:
- `ipod_classic_eic.c`: the external interrupt controller at 0x39A00000 (level +0x80, status +0xA0 write 1
  to clear, enable +0xC0, type +0xE0; group g = 6 - pin/32 raises VIC EINT(6 - g)) and the panel's frame
  marker on GPIO 55 (group 5 bit 15) at `EIC_FMARK_HZ` (60). `EIC_FMARK=0` turns it off; `EIC_FMARK=panel`
  pulses only once the guest has set FMKM in the panel's R090h, as on the real type 2 panel (the video
  player shows no frame without the pulses; see `runtime/panel.c`). `EICTRACE=1` logs the group's accesses.
- `ipod_classic_lcd.c`: records the guest's R090h writes for that gate.
- `ipod_classic_disp.c`: a 640x480 video plane scaled 2:1 to the panel; plane addresses masked to DRAM.
- `meson.build`: `-Dsm1_decoder=enabled` really requires libavcodec/libavutil.

Video build: libavcodec/libavutil from micromamba in `build/qemu/deps-av`, then configure with
`-Dsm1_decoder=enabled` and `PKG_CONFIG_PATH` including `deps-av/lib/pkgconfig`; `make libpctrace.so` in
`build/contrib/plugins`. The model decodes the access units it is given, not guest memory: dump them from
the H.264 front-end (0x081b6564, buffer r1, length r2) with the plugin, and give it the episode's avcC
payload (the bytes after the `avcC` box type).

    DYLD_FALLBACK_LIBRARY_PATH=build/qemu/deps-av/lib LCDTYPE=2 EIC_FMARK=panel IRAMSPILL=0x20000 \
    IPOD_SYSINFO=sysinfo-real.bin VDEC_DECODE=1 VDEC_AU_FILE=$R/au.bin VDEC_CFG_FILE=$R/cfg.bin SDO_FRAME_IRQ=1 \
    qemu-system-arm -M iPod-Classic -kernel <OS .bin> -drive if=ide,format=raw,file=<disk> \
      -plugin build/contrib/plugins/libpctrace.so,audump=0x081b6564:1:0:2:$R/au.bin

`IRAMSPILL` covers an audio ring overrun at 0x0802e6b0 that only the emulator hits. Navigate to an episode
with `tour.sh`'s key sequence once the run log shows "pre-build done".
