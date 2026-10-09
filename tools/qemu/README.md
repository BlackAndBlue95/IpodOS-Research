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
