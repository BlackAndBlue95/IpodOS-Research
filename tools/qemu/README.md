# Emulator (iPod Classic 7G, firmware 2.0.4)

Based on davidmonterocrespo24/qemu-ipod-classic (6G). `ipod-classic-7g.patch` adds:
- `IPOD_SYSINFO=<file>`: SysInfo from a real iPod (`FLAC/sysinfo.bin`, written by our OS at boot);
  2.0.4 refuses a 6G hardware version (`IPOD_HWID` overrides only that field).
- ATA interrupt latched in bit 2 as well as bit 0 (the 7G driver waits with mask 4).
- A SHA-1 engine model at 0x38000000 (Rockbox's register usage). 2.0.4's iTunesDB check does not use
  it, so `boot.sh` bypasses that check in its copy of the image.
- Display layers composed front to back with alpha by default, so the menu sits over the main menu's
  art pane as on the device (`DISP_ALPHA=0`: the old back-to-front opaque order).

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

Deterministic screen checks (tourcmp drifts once navigation timing differs between builds):

    tools/qemu/screens.py ospatch/builds/<image>.dec build/qemu/run/<disk>.img out/home wait1 shot ret wait1 shot
    tools/qemu/pngdiff.py out/a-01.png out/b-01.png        # differing pixels below the status bar

`screens.py` waits for the first frame, plays the keys (`waitN` sleeps, `shot` saves a numbered PNG)
and prints the run's first-frame, glyph-pass, pre-build, fbdmg and CRASH lines (`GREP=a,b` adds more).
`PMUCHARGER=1` reports USB power. Test disks: `EF.img` (stock fonts), `EF-fonts.img` (`style=modern`),
`EF-classic.img` (`style=classic`). Their firmware partition (block 8, 192 MB) needs the grown `rsrc`
for Modern's icons and fonts (docs/CODEBASE.md section 11): `tools/sf/rsrc_tree.py PART.bin sficons
OSOS.dec`, `ospatch/fwpart.py rsrc PART.bin sficons/rsrc NEW.bin`, then write NEW.bin at byte 0x8000
of a `cp -c` clone of the disk.

The PMU model (`ipod_classic_pmu.c`) reads BVMCTL (0x18) bit 0 as clear: as plain storage it kept
whatever Apple's writes left there, its LowBattDebounceTask took that for a flat battery during
navigation, showed "Connect to Power" and froze in the power-down (polling 0x39A00000, not modelled).

Instruction and cache profile (`kprof.c`, a TCG plugin). Counts every executed
instruction per translation block, the IRQ-handler share, memory traffic by region, and misses in a 16 KB
4-way L1 model of Apple's page table (DRAM write-back, IRAM and IO uncached). `trig=` addresses (region E
symbols such as `e_libload_boot`, `hk_uiinit`, `render_c`) split the boot into phases:

    qemu-system-arm ... -icount shift=3,sleep=off -plugin libkprof.so,out=k.out,trig=0x...,trig=0x...
    tools/qemu/kprof.py phases k.out
    tools/qemu/kprof.py funcs k.out os.s runtime/e.syms t1:t2     # functions ranked by modelled ARM926 cycles

QEMU ignores caches, so cycle figures are a model, not timing. Modelled cycles are ins×BASE + (I+D read
misses)×P + write misses×W + uncached IRAM fetches×(F−1), with `BASE`, `P`, `W` and `F` overridable in
the environment. Set `IRAMC=1` for runs made with the plugin's `iramc=1`; `NOSPIN=1` leaves out the
mdelay/elapsed spins (fixed wall time). Function starts in LISTING are push instructions.

    cc -O2 -fPIC -I<deps>/include/glib-2.0 -I<deps>/lib/glib-2.0/include -I<qemu src>/include/qemu \
       -bundle -Wl,-undefined,dynamic_lookup -o libkprof.so tools/qemu/kprof.c

Plugin arguments:
- `out=FILE`
- `trig=0xADDR` (repeatable): the first run of that TB writes a snapshot and a TB dump.
- `lr=0xADDR` (repeatable): {lr, r0 ^ r1 << 16} histogram at that TB head, per phase.
- `iramc=1`: model IRAM 0x22000000-0x22007fff and its VA 0 alias as cacheable.
- `cp=0xADDR[:R]` (repeatable): copy-routine entry. Per {entry, lr, size in rR (default r2)}: calls, first dst/src, and the instructions and misses inside IRAM libc; analyse with `kprof.py copies`.
- `wr=0xLO:0xHI` (up to 4): stores into [LO, HI) per store PC and phase.
- Creating `FILE.mark` writes a snapshot labelled mN and starts the next phase.

Playback test (run it before staging anything that touches video, the sync or the classifier):

    tools/qemu/videotest.py ospatch/builds/<image>.dec [disk.img] [show row]

Boots the image in the joel tree on a clone of the disk (default `Z-rb.img`, one Courage episode),
opens Videos > TV Shows > the show at that row > its first episode and counts H.264 access units at
the front-end 0x081b6564 through the gdb stub: PASS at 3 within 40 s. `videotest-shows.png` shows
which show was played. It leaves `EIC_FMARK` unset (marker pulses from boot, as on the device):
with `EIC_FMARK=panel` the frame pool fills after a few units and stops, since nothing writes R090h.
The plain `build/qemu/src` tree never starts video (selecting an episode does nothing).
