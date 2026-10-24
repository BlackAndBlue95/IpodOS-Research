# iPod Classic FLAC OS

Apple's iPod Classic 7G firmware 2.0.4, extended with C code linked into the image:

- FLAC playback (up to 24-bit/192 kHz, decimated and dithered to 16/48)
- an on-device library sync: copy album folders into `Music/`, eject, and the library updates,
  play counts and ratings survive renames
- album art from cover files or embedded pictures, kept in one database file, also in the main
  menus' preview pane and the Now Playing row
- Light, Dark and Automatic (sunset/sunrise) themes, switched live, in Apple's Classic look or a
  flat Modern style: SF fonts with kerning, SF icons, and every screen's chrome redone for both themes
- eight accent colours, or Album: the accent follows the playing album's cover (grey covers keep a
  chosen colour); Now Playing's title, progress bar and option bar take the cover's colour
- Theme, Accent Colour, Library and Hearing rows in Apple's own Settings menu; Hearing adds a
  loudness limit, loudness compensation, crossfeed, ear-fatigue filters and a weekly sound dose
- TV episodes and video playback
- first frame 4.6 s after the bootloader, with a boot status line and progress bar on screen
- a self-updater: drop a new image on the disk and restart
- boots straight from the firmware partition through a Rockbox-derived bootloader; Apple's
  hibernate (instant-on) works with it

How it works, address by address: [docs/CODEBASE.md](docs/CODEBASE.md).
Every byte changed in Apple's image: [ospatch/HOOKS.md](ospatch/HOOKS.md).

## Screenshots

A boot, then the main menu's album art, an album into Now Playing, the Now Playing row with the
playing album's cover, Accent Colour > Album taking the cover's colour live, and a live switch to
Light:

![Boot, menus with album art, Now Playing, the Album accent and a live theme switch](docs/screenshots/tour-r124.webp)

The Modern style: flat bars, large accent icons, SF text, covers from the library panning in the
preview pane. Dark in four of the eight accent colours, Album (the playing cover's colour), and
Light (bottom right):

![The main menu in four accent colours, Album and Light](docs/screenshots/accents-r124.png)

Around the menus: the Music menu, Albums and Songs with their covers, Settings > Hearing, the main
menu while a song plays, a boot stage, and Light with the Theme choices:

![Music, Albums, Songs, Hearing, the Now Playing row, the boot status, Light and Theme](docs/screenshots/tour-r124.png)

Now Playing: the album cover on a blurred copy of itself, a thin progress bar that becomes a
scrubber, the title and bar in a colour picked from the cover, and Light:

![Now Playing](docs/screenshots/now-playing-r124.png)

Single screens at twice the iPod's resolution are in [docs/screenshots](docs/screenshots). They
are taken in the QEMU model (tools/qemu), based on
[joelheaps/qemu-ipod-classic](https://github.com/joelheaps/qemu-ipod-classic), with its display
layers composed as on the device.

## Layout
- `runtime/` the code linked into the OS ("region E"); `runtime/flac/` playback and art;
  `runtime/host/` Mac-side generators and checks
- `libsync/` the iTunesDB sync, its host build and tests
- `ospatch/` the image patcher and checker, firmware partition tool
- `bootloader/` our bootloader source (`ipod-s5l87xx.c`, a modified Rockbox file; `.stock` is the original)

## Requirements
Apple's decrypted `Firmware-35.9.0.4.osos.dec` (not included), the Arm GNU toolchain
(`arm-none-eabi`; `runtime/build.sh` looks in `build/` or `$ARM_TOOLCHAIN`), Python 3, clang, `mks5lboot` for the bootloader.

## Build
```
PYTHONPATH=runtime/host python3 runtime/host/gen_settings.py ospatch/Firmware-35.9.0.4.osos.dec \
    runtime/settings_data.h runtime/settings_data.json
runtime/build.sh
python3 ospatch/mkpatch18.py --base stock --image ospatch/Firmware-35.9.0.4.osos.dec \
    --e runtime/e.bin --syms runtime/e.syms --out ospatch/osos-v18.dec --hooks all --governor-floor \
    --ui-fast --ui-lazy --games-defer --heap-cache --fast-prims --ft-raster --sleep-delays \
    --fast-libc --fast-dcache --fb-damage --art-pane --kern --dither --np-accent
python3 ospatch/checkpatch.py ospatch/osos-v18.dec runtime/e.bin runtime/e.syms all
```

## Install an update
Copy `ospatch/osos-v18.dec` to the iPod as `/osos-update.dec` and `/osos-v18.dec`, eject, restart.
The OS writes itself into the firmware partition and restarts once.

## Using it
- Music goes in `Music/<album>/`, m3u playlists in `Playlists/`. The library updates at boot and
  after every USB session; Settings > Library > Sync library now forces a pass.
- At power-on: PLAY held = Rockbox, MENU = Apple's stock OS.
- `FLAC\rtlog.txt` on the disk is the log (sync results, a library health line, boot timeline).

## Tests
See section 13 of [docs/CODEBASE.md](docs/CODEBASE.md); none of them need the iPod.
