# Codebase reference

Everything the source comments leave out on purpose: how the pieces fit, what each Apple OS
address is, the iTunesDB rules the OS enforces, and why non-obvious choices were made.
Hook-by-hook byte changes are listed in `ospatch/HOOKS.md`.

## 1. What runs where

```
power-on
  └─ NOR bootloader (Rockbox-derived, build/rockpod-src/bootloader/ipod-s5l87xx.c, -DOSCHAIN)
       ├─ PMU says hibernated → Apple's NOR resume code → OS continues from DRAM
       ├─ PLAY held → Rockbox
       └─ default → read the 'osfl' section of the firmware partition (fallback /osos-v18.dec)
            └─ Apple OS 2.0.4, patched (ospatch/mkpatch18.py) with region E linked in
                 ├─ early hooks: LCD init, Music app init, root UI init (theme applied)
                 ├─ library load hook (runtime/libload.c): mount, settings, self-update,
                 │    sync (libsync), Apple's loader, health line, art database, headset events
                 └─ runtime hooks: FLAC reader/decoder, album art, paths, theme, settings rows
```

Region E is our C code, linked at `0x08b33000` (reserve `0x80000`) and appended to the image. The
OS's startup copy is extended to copy it, and the heap base moves past it.

## 2. Directory map

| Path | Contents |
|---|---|
| `runtime/` | Region E (C, ARM926). `build.sh` produces `e.bin` and `e.syms`. |
| `runtime/flac/` | FLAC reader/decoder glue (`flacblob.c`), album art (`art.c`, included by `flacblob.c`), cover scaling (`cover.c`). `rbflac*`, `rbfir*`, `rbjpeg*` are adapted from Rockbox; `tjpgd*` is ChaN's TJpgDec. |
| `runtime/host/` | Mac-side tools: Settings resource generator, theme data generator, theme preview, C/Python colour-rule parity check, sun test. |
| `libsync/` | The iTunesDB sync (shared with the Rockbox plugin and the host build), host tools and tests. |
| `ospatch/` | Image patcher (`mkpatch18.py`), checker (`checkpatch.py`), firmware partition tool (`fwpart.py`), `HOOKS.md`. `mkpatch.py`/`stub.*` are the older pre-region-E patcher. |
| `bootloader/` | Built bootloaders (`boot-v6/` current), the stock source for reference. |
| `osos_boot/` | Rockbox plugins that booted the image before the NOR bootloader existed. |
| `osre/` | Reverse-engineering helper (`osre.py`) and its cached OS dumps. |

## 3. Boot and library load (`runtime/libload.c`)

Called from the OS at boot (call site `0x0804d7c4`) and after disk mode (`0x081ae29c`, inside
TMusicLoadingTask `0x081ae280`). Both call Apple's loader `0x0805d62c` (LIBLOAD).

Order: headset chip re-init (boot only) → `os_vol_mount(0)` → settings → self-update check →
sync → `os_libload` → health line → art database → headset plug events → log flush.

- **Mount.** Entering disk mode the OS unmounts FAT volume 0; LIBLOAD remounts it itself 12
  instructions in (`0x08058584` → `0x080e45dc(0)`). Until then every File open returns 3 (the
  volume falls back to a null filesystem whose every call returns 2). `0x080e45dc` is idempotent,
  so calling it first makes the sync work after USB. The unmount (`0x0828c34c`) waits for every
  open File: region E never keeps a File open across calls.
- **Headset (Mikey, I2C 0x39).** The OS reads the headset only on a plug event. A headset present
  since power-on stayed "plain headphones". Fix: chip init `0x080e9ce4` at entry, then at the end
  plug change `0x0802cf78(0x3f)` and a 2 s timer posting attention `0x40` (`0x0802d040/48`).
- **Health line.** `library: N tracks, N files, N unresolved (...)`; `unresolved` = tracks the OS
  loaded without an album beyond those whose files have no ALBUM tag. Non-zero, or a track count
  different from the files, ends the line with `<-- CHECK`.
- **Timeline.** `timeline (ms): appinit A, libload +B, ...` from TIMER_E (`0x3c7000b4`, 1 MHz). The
  OS restarts the timer, so it excludes the bootloader.

Measured on a 746-track library: OS start → library load 1.8 s, our sync 0.33 s (unchanged
library), Apple's loader 5.2 to 5.8 s (pure CPU at L0, no per-track file access), art update 9 ms.

## 4. Library sync (`libsync/`, `runtime/ossync.c`)

`libsync.c` is a C port of the Mac script `flacsync.py`; `--full` mode reproduces it byte for byte
(`libsync/ref.py`). Incremental mode (the device) keeps unchanged entries byte for byte, re-reads
changed files keeping id, dbid, dates, play counts and rating, adds new files, drops missing ones.

### iTunesDB rules the OS enforces (verified on the device)
- **Track list sorted by ascending track id.** The OS resolves master-playlist items to tracks by
  searching the list by id. Tracks added in folder order with the highest ids sat mid-list: About
  counted them, no menu showed them. The sync sorts by id before writing, and rewrites a list that
  is out of order even when nothing else changed.
- **Playlist item ids unique across the database.** Items are numbered above every existing item of
  every playlist (an earlier "track id + 4" scheme collided).
- **Signed database.** `hash58` (HMAC-SHA1 keyed from the FireWire GUID) at `+0x58`; any byte change
  needs re-signing. SysInfo `+0x38` holds the GUID reversed; the sync retries the other order on a
  bad signature.
- **Sort indexes are positional** (mhod 52 arrays index the track list), so the list and the
  arrays must be written together.
- The OS keeps 35 characters of a location; `runtime/path.c` supplies full paths (FLPT, below).

Not a rule, despite earlier belief: the OS loader (`0x0808a3ec`, jump table on mhod type at
`0x0808accc`) reads string records in any order. Entries are still written in Finder's order
(title, artist, album artist, composer, album, genre, type, location last).

### Entry layout written for FLAC tracks
`+0x20` file mtime, `+0x24` size, `+0x28` length ms, `+0x2c` track no, `+0x30` track total, `+0x34`
year, `+0x38` bitrate, `+0x3c` sample rate << 16, `+0x5c` disc, `+0x68` date added, `+0x70` dbid,
`+0x88` sample rate (float), `+0xb2` 2 = unplayed, `+0xbc` sample count (u64), `+0x100` = 1,
`+0x120` album id, `+0x12c` size, `+0x1e0` = 0x7f, `+0x1f4` = id + 1 (Finder's layout).
Kept from an existing entry: `0x10(4) 0x1f(1) 0x40(0x1c) 0x68(0x10) 0x79(1) 0x7c(8) 0x9c(9) 0xb2(1) 0x160(4)`.

**Entry marker** at `+0x258` (16 bytes in the always-zero tail `0x210..0x26f`): `'FLSY'`, u16
version (2), u16 flags (title from file name / no album / no artist), u32 sync time, u32 0. An
entry without the current marker is rebuilt once, keeping its history.

### Safety
- `ls_verify` re-parses every new image before it replaces the old one: lengths, section chain,
  unique album/track ids and dbids, ascending track ids, one title and one location per FLAC entry,
  album ids present and all used, the 10 sort indexes as permutations, contiguous letter tables,
  master items equal to the track list, items pointing at real tracks, unique item ids, the
  expected number of our playlists. A rejected image is saved as `FLAC\iTunesDB.rejected`.
- Write: `iTunesDB.new`, rename old to `iTunesDB.old`, rename new in place; a cut-short replace is
  finished on the next run. Play Counts are merged then deleted after a successful write.
- Refuses to write when the signature doesn't match, when `/Music` has no FLACs but the database
  does, and when any folder can't be listed completely (a partial listing would look like deletions).
- **Renames** carry stats when the sample count matches and either the file name or title+album+
  artist match (Rockbox's rule; length alone never counts). Names compare NFC-composed (`tables.h`).
- **Playlists** from `/Playlists/*.m3u[8]`: id top byte `0xe5` + 56-bit FNV-1a of the path, so a
  resync replaces only ours; the playlist timestamp holds the file mtime.

### Port layer (`runtime/ossync.c`)
Paths `/Music/x` ↔ `Music\x`. Heap tries 8, 6, then 4 MB. The scan's per-folder listing is passed
to `album_cb`, which picks each album's art source (cover.jpg, folder.jpg, front.jpg, cover.jpeg,
albumart.jpg, else the first FLAC's embedded picture) so the art database needs no listing.

### Raw folder listings (`runtime/fatdir.c`)
The OS find API costs one SD transaction per directory sector (~16 ms per album folder). `fatdir.c`
reads whole directory cluster chains through the block device class (one transaction per cluster;
FAT32 on this card: 4096-byte sectors, 4 sectors/cluster, partition at LBA 49278). It matches the
OS's naming exactly: 8.3-fitting names come back as the stored upper-case short name (the OS
ignores the NT lower-case bits, e.g. `COVER.JPG`). First boot after install lists every folder both
ways and compares; a clean run writes `FLAC\fatdir.ok`. Any inconsistency falls back to the OS
listing. Host test: `libsync/test_fatdir.py` on an `hdiutil` FAT32 image.

## 5. Album art (`runtime/artdb.c`, `runtime/flac/art.c`)

`FLAC\covers.db`: 64-byte header (`'FCDB'`, version, count, entry size 128, table offset, pixel base
aligned to 4096, block size, generation), 128-byte entries sorted by key (FNV-1a 64 of the folder
path under Music, `/`-separated), then one 276,496-byte block per album holding the four thumbnail
formats back to back (1055: 128², 1060: 320², 1061: 55², 1068: 128², RGB565). An entry is current
while (source kind, name, size, mtime) are unchanged; changed albums are decoded (rbjpeg for files
≤ 2 MB, TJpgDec otherwise), the file is rewritten via `covers.db.new`, the old one kept as `.old`.

`art.c` answers the OS: FindImageForTrack `0x080457e4` (builds an image whose thumbnails point at an
art block), ArtworkThumb::Load `0x080f32ec`, and the browse cache path GetThumbLocation
`0x08133980` → stream seek `0x0826d8e0` / read `0x0826d5fc`, served from covers.db blocks. The
offset handed back encodes itself: `0x7 | geometry (4 bits) | block address >> 2 (24 bits)`.
Block magics: `FART` cover file, `FARE` picture inside a FLAC, `FARD` covers.db entry.

## 6. FLAC playback (`runtime/flac/flacblob.c`)

A `.flac` is classified as WAV (type 3); the WAV parser hook `0x0828a5ac` attaches our reader, and
the codec selector (IRAM `0x22005d60`, veneer word `0x22003824`) routes codec 2 with our marker to
our decoder (Init and DecodeNext replaced in a copy of the PCM decoder's vtable). Seeks use the
SEEKTABLE, else bisect on frame headers. Rates above 48 kHz are decimated (64/128-tap FIR); 24-bit
is TPDF-dithered to 16. Truncated files end at the last whole frame.

Trace words `TR(n)` (see the legends in `art.c` and `flacblob.c`) are debug counters dumped to
`FLAC\flaclogXY.bin` only when `FLAC\debug.txt` exists. Known: the `art.c` legend is stale for
TR2/TR6/TR7 (TR2 is decode time, TR6 low byte rc and high half KB read, TR7 read ticks).

## 7. Paths (`runtime/path.c`, `runtime/osfile.c`)

- A track record keeps 36 bytes of location at `+0xc0`. Longer locations are copied to the heap:
  marker byte 1, then the address of `'FLPT' + path`. Copies are freed two loads later.
- The OS loader converts UTF-16 locations to UTF-8 (`0x0803be14`) but passes the UTF-16 unit count
  as the length (`0x080da08c`); `loc_len()` restores the real length.
- OS long-name bug: the LFN converter `0x082d95dc` loses bytes when a non-ASCII name's entries
  straddle a sector. `e_lfn_batch` replaces both callers (`0x082d9b14`, `0x082d6314`).

## 8. Theme (`runtime/theme.c`, `theme_rules.c`, `settings.c`)

- Resource package at `0x08400258` (data `0x08417928`). The theme recolours BMap/COLR resources in
  place in RAM, keeping pristine copies.
- The five OS greys (`0x089cc8c0..d0`: white, #AA, #7F, #55, black) are reached through 74 code
  literals; `mkpatch18.py` retargets them by role to region E copies read at draw time. Some must
  stay on the OS globals (the `KEEP` list), e.g. the start-up grey fill `0x082a6494` and the 2-bpp
  palette pair `0x08092c9c/ca0` used to expand the scrolling-title mask.
- Views copy colours when built, so the theme is applied before the first view (root UI init
  `0x081135a0`), and again at UI init after the library load reloads resources. The choice is
  also stored inside the image (`theme_boot`) for the early path.
- Live Light/Dark switch: pop to main (`0x08287e30`), app reload (`0x0817cd30`), walk every view
  remapping stored colours (text views `+0xf4`, marquee tints), repaint every layer through slot
  `0x70`. Re-rendering the main menu's right pane (class `0x0898597c` slot `0x210`) from the
  settings action crashes the OS, so the pane keeps its old colours until restart.
- Scrolling titles: the mask is drawn with black ink (`e_marq_ink` replaces the call at
  `0x081a7e70`) and the real colour restored into the line canvas tint.
- Colour math in `theme_rules.c` must match `host/theme_rules.py`; `host/rules_parity.py` checks it.
- Automatic mode: NOAA sunrise/sunset (`sun.c`) for the city nearest the time zone (`cities.h`),
  checked on the clock tick (`0x0817ce28`).

## 9. Settings rows (`runtime/host/gen_settings.py`, `runtime/settings.c`)

Theme, Accent Colour and Library rows are cloned from Apple's own Settings records (Backlight row,
Radio Regions canvases), added to a relocated copy of the resource index and name table (3920 →
3936 names). Two vtable slots route actions (`e_settings_action`) and row text/checkmarks
(`e_settings_provider`). Library: "Sync library now" (restart; the boot syncs) and "Power state to
log". Settings file: `FLAC\settings.txt` (`theme=`, `accent=`, `style=`, optional `city=`).

## 10. Power and hibernate (`runtime/power.c`)

- Apple's governor thread (IRAM `0x22002a58`): L0 216 MHz/1200 mV … L3 18 MHz. `boost_hold` asks it
  for full speed (`0x0802d060`, ≤ 10 s). Never write CLKCON1 or the core voltage directly.
- **Hibernate works with this OS.** After 30 min of sleep the OS saves IRAM to DRAM, writes a
  `'hibe'` header with resume entry `0x080d24d0` at DRAM `0x08000000`, puts RAM in self-refresh and
  sets PMU `0x16 = 0`, `0x0c = 2`. On power-on the bootloader sees the PMU flag and hands over to
  Apple's NOR resume code, which jumps back into the OS (header becomes `'used'`). The OS never
  restores PMU `0x16`, and the hand-over leaves the CPU slow for ~10 s; `power_resume_check`
  (LCD init hook and minute tick) holds full speed and sets `0x16 = 7`.
- Rescue if a resume ever loops: DFU and `bootloader/boot-v6/rescue-noresume.dfu` (built with
  `-DOSCHAIN_NORESUME`).

## 11. Firmware partition and self-update (`ospatch/fwpart.py`, `runtime/update.c`)

- Partition (type 0x3f, LBA 63): `]ih[` at +0x100, directory at +0x5000 (12 × 40-byte entries),
  section data at devOff + 0x1000, checksum = 32-bit byte sum. Our image is the `osfl` section at
  0x5944000; Apple's `osos` stays so MENU still boots stock.
- Self-update: copy a new image to `/osos-update.dec`; at boot it is validated, written in 64 KB
  chunks through the block device class, read back and compared, the directory entry updated, the
  file renamed to `osos-update.done`, then a watchdog restart. A marker (`FLAC\update.try`) stops a
  second attempt after a crash.
- Block device class `0x08270400`: synchronous transfers are **vt[4] = read, vt[0] = write** (fifth
  argument 0). Swapping them once overwrote the partition table. DMA needs the D-cache cleaned
  before writes and cleaned+invalidated around reads. A direction probe on the last sector runs
  before any write.

## 12. Bootloader (`build/rockpod-src/bootloader/ipod-s5l87xx.c`)

Rockbox bootloader with `-DOSCHAIN`: loads `osfl` (word-wise byte-sum check), builds SysInfo,
`ata_sleepnow()`, jumps. v6 changes: 200 ms button window, FAT mounted only for the file fallback,
no post-sleep delay, v3 to v5 headset experiments removed. Install with
`mks5lboot --bl-inst bootloader/boot-v6/bootloader-ipod6g.ipod -p 1223` from DFU; RAM-test first
with `--dfusend .../ramtest-v6.dfu`.

## 13. Build and test

```
PYTHONPATH=runtime/host python3 runtime/host/gen_settings.py ospatch/Firmware-35.9.0.4.osos.dec \
    runtime/settings_data.h runtime/settings_data.json
runtime/build.sh
python3 ospatch/mkpatch18.py --base stock --image ospatch/Firmware-35.9.0.4.osos.dec \
    --e runtime/e.bin --syms runtime/e.syms --out ospatch/osos-v18.dec --hooks all --governor-floor
python3 ospatch/checkpatch.py ospatch/osos-v18.dec runtime/e.bin runtime/e.syms all
```

Install: copy `ospatch/osos-v18.dec` to the iPod as `/osos-update.dec` (and `/osos-v18.dec` as the
fallback), eject, restart.

Tests (no iPod needed):
- `clang -std=c99 -O2 -DLS_FAULTS -o libsync/libsync-host libsync/host.c libsync/libsync.c`
- `python3 libsync/mkmirror_db.py <iTunesDB> <dir>` then `python3 libsync/test_incr.py <dir> <work> <iTunesDB>`
- `python3 libsync/test_fatdir.py <work>`; `libsync/libsync-host --verify <iTunesDB>`
- `runtime/host/rules_parity.py`, `runtime/host/sun_test.c`, `runtime/flac/hostr.c` (decoder on the Mac)

The device log is `FLAC\rtlog.txt` (previous run in `rtlog.prev.txt`).

## 14. Open items
- Main-menu right pane and status bar keep the old theme after a live switch until restart.
- `art.c` trace legend partly stale (see §6).
- Skipping tracks with the headset remote occasionally stopped playback (not reproduced since).
