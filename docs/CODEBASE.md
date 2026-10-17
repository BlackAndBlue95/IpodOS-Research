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
| `runtime/ft/` | FreeType 2.14.3's anti-aliased rasterizer (`ftgrays.c`, `ftgrays.h`, `ftimage.h`, FTL licence) with the iPod changes marked `iPod:`; `ftgrays_e.c` builds it standalone. |
| `runtime/host/` | Mac-side tools: Settings resource generator, theme data generator, theme preview, C/Python colour-rule parity check, sun test. |
| `libsync/` | The iTunesDB sync (shared with the Rockbox plugin and the host build), host tools and tests. |
| `ospatch/` | Image patcher (`mkpatch18.py`), checker (`checkpatch.py`), firmware partition tool (`fwpart.py`), `HOOKS.md`. `mkpatch.py`/`stub.*` are the older pre-region-E patcher. |
| `bootloader/` | Built bootloaders (`boot-v6/` current), the stock source for reference. |
| `osos_boot/` | Rockbox plugins that booted the image before the NOR bootloader existed. |
| `osre/` | Reverse-engineering helper (`osre.py`) and its cached OS dumps. |
| `tools/qemu/` | Emulator patches, disk tools and scripts (`README.md`). `tools/osmap.py` makes `build/os204.map` (OS symbol names). |
| `tools/video/` | `ipodenc.sh`: episodes encoded for the iPod. |

## 3. Boot and library load (`runtime/libload.c`)

Called from the OS at boot (call site `0x0804d7c4`) and after disk mode (`0x081ae29c`, inside
TMusicLoadingTask `0x081ae280`). Both call Apple's loader `0x0805d62c` (LIBLOAD).

Order: headset chip re-init (boot only) → `os_vol_mount(0)` → settings → self-update check →
sync → `os_libload` → health line → art database → dose file → headset plug events → log flush.
At boot each stage is shown on the screen and the log is flushed after the update check, the sync
and Apple's load, so a stall names its stage (see "Boot screen" in section 9).

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
  OS restarts the timer, so it excludes the bootloader. `[N]` after a phase is the device read time
  inside it; the `step:` lines between UI init and the first frame carry the running total.
- **Boot glyph pass (`runtime/uifast.c`, `BOOT_GLYPHS`).** Apple's text engine is FreeType
  (`0x080c948c` rasterises). The glyph pass `0x081ae8c4` renders, for each font instance in the list
  `[0x089cc8dc]`, every pending character of its renderer (`instance+0x98`, bitset `+0x8c`): ~1670
  SF glyphs before the first frame, ~1.1 s on the device; no CJK glyph is drawn (those fonts are only
  searched for characters SF lacks). At boot only instances 1 and 4 (creation order with the English
  UI: the main menu's rows and preview label) are rendered, ~690 glyphs; the rest keep their pending
  characters for the background build's pass, which does one instance per tick. Without any boot pass
  the menu rows have no text. After a crash of the same build (`crash_prev`, crash.c) the next boot
  runs Apple's full pass.
- **Raw device handle and drive power.** Closing fatdir's block device lets storage power the drive
  down; the next write powers it up on the writing task: battery check (`0x08278aa0` → `0x082b1e54`),
  ATA reset pulse and drive supply (PMU register 0x52) with 10 + 10 + 50 ms sleeps (`0x082b1dec` →
  `0x08361388`/`0x08361124`), battery again, ~0.6 s on the device. So the handle stays open from the
  library load until the boot release (pre-build done with `--ui-lazy`, else the first frame); after
  that each session (`fatdir_begin`/`fatdir_end`, also osfile.c's free-space searches) closes it at its
  end. Volume unmount `0x0806aa08` (`hk_unmount`) closes it first, or at the end of a running session.
  The updater uses `fatdir_close`.
- **Boot reads (`runtime/uifast.c`).** The device read `0x082bb36c` (drive, lba, buf, count) is
  hooked from app init to the first frame (read time for the log). From the font scan (`e_font_dir`)
  on, reads on drives 0 and 1 are served from 128 KB blocks read once: the boot glyph pass reads the
  CJK fallback fonts (CJK11/CJK16) in ~440 scattered 8 KB pieces, with `Resources/Fonts` or
  `Resources/ModernFonts` alike; 38 block reads cover them. `0x082bb36c` counts 512-byte sectors on every drive, whatever the
  volume's sector size: drive → object `0x08ad1794[d]` (partition start `+0x0c`) → device (`+4`, vt
  `0x089a7378`) → `0x0828c08c`, which shifts by 9 and splits into the drive's blocks (`+0x1c` bytes,
  `+0x20` sectors each, bounce block `+0x50`). Only blocks inside a volume's data area
  are cached: some writes (FSInfo `0x080595b4`) go to the drive object directly, past `0x082bb418`.
  A fill that overlaps any write is dropped, a failed fill is remembered, the first 8 fills are
  compared with a direct read (a mismatch turns read-ahead off), and the blocks are freed at the
  first frame once no reader is inside.
- **Boot CPU (optional flags, `ospatch/HOOKS.md` "Boot CPU" and "Kernel prototypes").** Between UI init
  and the first frame the UI task's time went to Apple's heap (one size-sorted free list walked by every
  malloc and free), the dynamic-array methods `0x08299xxx` (several virtual calls per 4-byte fetch,
  bitsets scanned one call per bit, including the glyph pass's 64K-bit sets) and FreeType's 2007-era
  anti-aliased rasterizer. `runtime/heapcache.c` (`--heap-cache`) keeps freed small blocks per size;
  `runtime/prims.c` (`--fast-prims`) reads 4-byte arrays directly and scans bitsets by word;
  `runtime/ft/` (`--ft-raster`) is FreeType 2.14.3's `ftgrays.c` with its cells in the OS's render pool;
  `runtime/kfix.c` holds the kernel survey's fixes (`docs/KERNEL-SURVEY.md`). Measured with QEMU
  `-icount shift=3,sleep=off` as UI-window CPU (window minus drive-read time): 1540 ms → 1077 ms with
  all of them; disk times under icount are noise.

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
(title, artist, album artist, composer, album, genre, comment, type, location last).

**Artists.** The OS lists Artists by the artist string (type 4) and ignores the album artist
(type 22) for tracks, so the artist string holds the album artist (the song's artist when there is
none). The song's own artist, when it differs, goes in the comment (type 8, track record `+0x4c`),
and the per-song artist text (Now Playing, song rows: `0x08298f3c`) reads it first: `e_song_artist`
(`runtime/libload.c`) replaces the call to `0x0804a840` at `0x08298f80`.

### Entry layout written for FLAC tracks
`+0x20` file mtime, `+0x24` size, `+0x28` length ms, `+0x2c` track no, `+0x30` track total, `+0x34`
year, `+0x38` bitrate, `+0x3c` sample rate << 16, `+0x5c` disc, `+0x68` date added, `+0x70` dbid,
`+0x88` sample rate (float), `+0xb2` 2 = unplayed, `+0xbc` sample count (u64), `+0x100` = 1,
`+0x120` album id, `+0x12c` size, `+0x1e0` = 0x7f, `+0x1f4` = id + 1 (Finder's layout).
Kept from an existing entry: `0x10(4) 0x1f(1) 0x40(0x1c) 0x68(0x10) 0x79(1) 0x7c(8) 0x9c(9) 0xb2(1) 0x160(4)`.

**Entry marker** at `+0x258` (16 bytes in the always-zero tail `0x210..0x26f`): `'FLSY'`, u16
version (4), u16 flags (title from file name / no album / no artist), u32 sync time, u32 0. An
entry without the current marker is rebuilt once, keeping its history.

**The Music app** rewrites the database whenever the iPod connects to a Mac (and lists the library
from it). It keeps every entry's fields and string records but zeroes the marker, sets `+0x1e0` to
`0x81`, writes rates over 65535 Hz as `0xffffffff` and moves the comment record after the file
type. Each successful write therefore leaves `FLAC/libsync.state` (`'FLSY'`, u16 version, u16 0);
while it names the current version, an unchanged FLAC entry with an all-zero marker is kept as the
Music app wrote it (play counts it merged in included) and stamped at the next write. Without the
state file such entries are rebuilt as before. Test 19 in `test_incr.py` covers it.

### TV episodes
`.m4v`/`.mp4` files under `/TV Shows/<show>/[Season N/]` (optional folder; without it the episodes
are dropped) become entries of file type `'M4V '` (`0x4d345620`). Ours = a FLAC, or an M4V whose
location is outside `:iPod_Control:` (iTunes-synced videos are left alone). The season is the
folder's number, else the N of an `SnnEnn` in the name, else 1; the episode is the `Enn`, else the
number the name starts with, else the file's place in its folder in name order; the title is what
follows (past `" - "`, `". "`), else "Episode N". Length from `mvhd`, audio rate from the `mp4a`
entry. Records as iTunes writes an episode: title (1), the show as artist (4), album
`"<show>, Season N"` (3), show (19), `SnnEnn` (20), "MPEG-4 video file" (6), location (2).
Fields: `+0x2c` and `+0xd8` episode, `+0xd4` season, `+0xd0` media type `0x40` (TV show), `+0xb1`
video, `+0xb2` 2 = unplayed. Remember playback position (`+0xa6`) stays off: entries with it set
bootlooped the device once an episode had been played (R66). Tests 20 to 22 in
`test_incr.py`. Encoding: `tools/video/ipodenc.sh` (H.264 Baseline 3.0, 640x480 at most, AAC).

### Safety
- `ls_verify` re-parses every new image before it replaces the old one: lengths, section chain,
  unique album/track ids and dbids, ascending track ids, one title and one location per entry of ours,
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

Hearing (`runtime/flac/hearing.c`, settings in Settings > Hearing): with any filter on, frames go to
24-bit, through loudness shelves (ISO 226:2003 contour differences fitted by `host/gen_loudness.py`),
ear-fatigue dynamic cuts at 3.15/7.5 kHz, bs2b crossfeed and the loud limit (A-weighted block level
against 75..100 dB SPL), then dither to 16; all off is bit-exact. The SPL estimate is the codec's
headphone gain (volume byte at `0x089d0ef6`, formula of `0x08088bc4`) plus a headphone sensitivity.
The ITU-T H.870 dose is booked per day in `FLAC\dose.txt` (`runtime/dose.c`).

Trace words `TR(n)` (see the legends in `art.c` and `flacblob.c`) are debug counters dumped to
`FLAC\flaclogXY.bin` only when `FLAC\debug.txt` exists. Known: the `art.c` legend is stale for
TR2/TR6/TR7 (TR2 is decode time, TR6 low byte rc and high half KB read, TR7 read ticks).

### Video
Apple's video path needs no patch: the S5L8702's own decoder (`0x39800000`) decodes H.264 into a
5 MB frame pool, and the display task `0x081ce0ac` presents one frame (`0x080eaad8`) each time the
LCD's frame marker (FMARK, GPIO 55) signals; the marker pulses with our bootloader's panel setup.

An episode named like `S01E01.m4v` fits 8.3, so FAT keeps only its short name and the listing
(Apple's and `fatdir.c`) gives `S01E01.M4V`. Apple's media classifier (`0x080870ac`: mp3 aif afc
m4a m4b m4p m4v mp4 mov aax wav pcm aiff aifc aa, plus our flac) copies the last 5 characters but
lower-cases only 3 (`cmp r1, #3` at `0x080870f0`), so `.M4V` reads as `m4V`, an unknown type: the
player shows a black screen at 0:00, opens no file and submits no access unit. The patch makes the
loop cover all 5 characters (any extension case, any track), and libsync lower-cases `/TV Shows`
extensions when it scans them. libsync also leaves out, with a
`sync: <file>: <reason>  <-- CHECK` line, episodes the player cannot decode (not H.264 avc1 or
MPEG-4 mp4v, larger than 640x480, H.264 other than Baseline) and FLACs the decoder rejects
(outside 8-192 kHz, 4-24 bits, blocks under 16); H.264 above level 3.0 or non-AAC sound is
added with a warning. `tools/qemu/videotest.py` checks playback end to end in the emulator.

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
- Live switch (Light/Dark, Automatic at sunrise/sunset, accent, Modern on/off): `theme_apply`,
  then Apple's language-change reload walk: the root view's slot `0xd8` (`0x0814d394`, as
  `0x081e1444` runs it after switching the strings) makes every visible view re-read its resources
  and update in place, so the screens on the stack keep their place. Then the stored-colour walk
  (text views `+0xf4`, marquee tints) and a repaint of every layer through slot `0x70`. Modern's
  flattened bars get Apple's pixels back and its Now Playing layout words are put back in
  Classic. Only the font folder (`e_font_dir`, read when the fonts are scanned at boot) follows at
  the next start.
- Assets live on Apple's resource volume (`rsrc`, the OS's volume 4, section 11) beside Apple's
  own, never in region E: `Resources/Icons/Classic.bin` and `Modern.bin` (the 266 SF-replaced
  icons, `tools/sf/icon_sets.py`), `Resources/ModernFonts` (Apple's fonts with the SF Compact
  stand-ins for Helvetica) and `Resources/Theme/Package.bin` (Apple's bitmap data, 3.2 MB).
  `rsrc_open` (`osfile.c`) opens them as File::Open does for `Resources/Games/`: a stream on
  volume 4.
- Icons: only the active set is in RAM, in the package's own bytes: Classic where Apple put it,
  Modern packed into the same 1.74 MB (`sf_tab` column 3). `sf_apply` reads the other set's file
  into place on a style change and repoints the index (about 45 ms in the emulator). The first
  apply at boot runs before `rsrc` is mounted, so a Modern iPod gets its icons at the UI-init apply,
  before any screen is built.
- Recolouring needs no copies in RAM: a bitmap the theme or `modern_bars` has not written still
  holds its source's pixels in the package, so it is themed from there; a written one is read back
  (`bmap_source`) from `Package.bin`, or from the set file for an SF icon. Only a live switch reads.
  `applied_key` per bitmap lets the UI-init apply skip what already holds its pixels. Colours
  (`COLR`, 4 bytes) keep heap copies.
- Charging screen (Modern): the OS builds the battery from slices sharing one frame (body rows
  22..96, then a reflection): `ChargingMode_Image_{Empty,Green}_CapLeft`, `_Middle` repeated,
  `Green_Middle_Cap` where the charge ends, `_CapRight` with the nub. `gen_sficons.py` (`chg`
  special) redraws each slice as its share of one flat battery (grey outline, green fill, no
  reflection), `_Critical` (low battery) as the outline with a red sliver, and the bolt and plug
  (`_ToggleCharging/_ToggleCharged`, alpha masks the OS tints black) as white palette images. Its
  glossy top bar is `NowPlaying_StatusBar` (`0x0dad082e`), flattened by `modern_bars`.
- Games (not built): the catalog lists the folder named by the string at `0x089a8d1e` (stock
  `iPod_Control/games_RO/`), and every game file is reached through
  `0x08096ea4(out, kind, id, builtin, sub)`, which builds `iPod_Control/<folder>/<id>/<sub>` (kinds 0
  and 4 `games_RO`, 1 `gamedata_RW`, 2 `gamestats_WO`, 3 `gamedata_ShareRW`; kind 0 with `builtin`
  set is `Resources/Games`). R62 rewrote both to read games from `/Games`; with 49 games there, the
  iPod's disk mode stopped answering the Mac (the cause is not found), so it was taken out. Emulator
  test disks: `tools/qemu/fatadd.py` copies a folder into the FAT32 image.
- Cover Flow: its layout is a table of constant value nodes (vtable `0x08986998`, value at `+4`)
  built at start-up around `0x082a1300`: centre cover at (0, -9, 72.5) turned 180 (flat), first
  side cover at (55, -9, 40) turned 270 (`0x08a79840`, `0x08a79930`), side covers 15 apart (code
  immediates), size 35, the flip to the track list at (0, 9.65, 116.25) turned 360. The left
  side's turn is mirrored about the centre (`0x08147ee0`, patched from side - centre to
  2 * centre - side: the same 90 for stock). Modern (`theme.c` `coverflow_style`) turns the side
  covers flat and moves them out to 76, so the neighbours show beside the centre cover. Every
  cover has to stay inside the screen: Apple's renderer stalls on one past the edge (an offset of
  87 does that, and the watchdog restarts the iPod). The back side is a striped list (`+0x124` flag `0x40000`: every other row
  filled `#F2F2F2`, `mov r0, #242` at `0x0827aee8`, the rest on `CoverFlow_Backside_Background`);
  in Dark the stripe is `#262626`, the background `#1C1C1E`, the text white; Modern flattens its
  header (`CoverFlow_Backside_Top_Image`).
- Scrolling titles: the mask is drawn with black ink (`e_marq_ink` replaces the call at
  `0x081a7e70`) and the real colour restored into the line canvas tint.
- Modern Now Playing: the steady frame is drawn into a 320x240 RGB565 buffer by `0x0815c588`.
  `e_cover_angle` (`0x08270728`) keeps the cover flat, `e_refl_verts` (`0x08270adc`) collapses the
  reflection quad, and `e_np_present` (`art.c`, replaces `0x08268b7c` at `0x0815c5b8`, Apple's
  tilted-edge shading) lays the frame out again from the cover Apple drew at (18, 38): a blurred,
  darkened copy of it behind, the cover with rounded corners. Text positions are patched in the
  layout resources (View `0x0dad088e`, VLyt `0x0dad089a`, `modern_np_layout`). The title takes the
  accent on every marquee draw (`np_title_accent`); the progress bar and the option bar
  (`OptionBar_NowPlaying_*`, labels via `e_opbar_ink` at `0x081df3ec`/`0x081df414`) are flat.
- Colour math in `theme_rules.c` must match `host/theme_rules.py`; `host/rules_parity.py` checks it.
- Automatic mode: NOAA sunrise/sunset (`sun.c`) for the city nearest the time zone (`cities.h`),
  checked on the clock tick (`0x0817ce28`).

## 9. Settings rows (`runtime/host/gen_settings.py`, `runtime/settings.c`)

Theme, Accent Colour, Library and Hearing rows are cloned from Apple's own Settings records (Backlight
row, Radio Regions canvases), added to a relocated copy of the resource index and name table (3920 →
3936 names). Two vtable slots route actions (`e_settings_action`) and row text/checkmarks
(`e_settings_provider`). Library: "Sync library now" (restart; the boot syncs) and "Power state to
log". Hearing rows are value rows cloned from Clicker (kind 12, value text in SORC role 10). A Debug
row pushing Apple's `Debug_MainMenu_Screen` is available (`ROWS` in the generator) but off: its Unit
Tests asserts. Settings file: `FLAC\settings.txt` (`theme=`, `accent=`, `style=`, `limit=`,
`loudness=`, `fatigue=`, `crossfeed=`, `phones=`, optional `city=`).

### Disk access rules

- Apple's FAT driver keeps table and directory changes cached. Close, delete and File flush
  (`+0x20` → FAT `+0x2c` → volume sync `+0x30`) write them; rename does not. `vol_sync()` (osfile.c)
  flushes after the `iTunesDB` swap, the `covers.db` renames and the update's rename, and before our
  watchdog restarts. A crash reset still loses whatever is cached, as on stock.
- No file calls (nor log flushes or theme switches) from the minute tick `0x0817ce28`: it can run
  inside a FAT driver call (the volume mutex is recursive), and a write there hung playback and put
  a FAT sector over a directory. The dose is written at the library load only.
- Do not commit by unmounting (`0x0806aa08` → `0x0828c34c`): it waits without limit until it is the
  volume's only user, and hung the boot.
- The FAT allocator hooks (`hk_fatfree`, `hk_fatsearch`, `hk_fatrun`, `hk_fatinfo`) are required on a
  256 GB card: stock, the free count is a full scan of a 62 MB table and a low next-free hint makes a
  small write read 1.4M entries, long enough to stall the boot.
- Nothing writes into an OS image at run time.

### Boot screen

During the library load at boot the LCD still belongs to the bootloader's set-up (Apple's LCD init
comes after it), so `updscreen.c` prints a console there through the same interface as the update
bar: timestamped lines below the bootloader's own (5x7 font on its 6x8 grid, from y 64), one per
stage (`stage()` in libload.c), the sync's progress rewriting its row (`bootscreen_status` from
`ls_status`), its summary, the track count and update errors. Drawing stops before Apple's LCD
init; nothing is drawn after USB sessions.

## 10. Power and hibernate (`runtime/power.c`)

- Apple's governor thread (IRAM `0x22002a58`): L0 216 MHz/1200 mV … L3 18 MHz. `boost_hold` asks it
  for full speed (`0x0802d060`, ≤ 10 s). Never write CLKCON1 or the core voltage directly.
- **Hibernate works with this OS.** After 30 min of sleep the OS saves IRAM to DRAM, writes a
  `'hibe'` header with resume entry `0x080d24d0` at DRAM `0x08000000`, puts RAM in self-refresh and
  sets PMU `0x16 = 0`, `0x0c = 2`. On power-on the bootloader sees the PMU flag and hands over to
  Apple's NOR resume code, which jumps back into the OS (header becomes `'used'`). The OS never
  restores PMU `0x16`, and the hand-over leaves the CPU slow for ~10 s; `power_resume_check`
  (LCD init hook and minute tick) holds full speed and sets `0x16 = 7`.
- Battery level: there is no fuel gauge. Apple reads the raw PMU ADC count (`0x082b1ac0`) and
  counts it against 15 thresholds (`0x089cae8e`), so its icon has 16 real steps. In Modern,
  `battery.c` interpolates the same count to 0..100 % (8 readings, one every 5 s) and draws it into
  status bar images 0 and 1, alternating, from the battery icon's own level call (`0x08193444` in
  `0x081933f8`, which asks only on battery power; other users of Apple's level are untouched). Red
  at 20 % and below; redrawn after each theme apply. `0x08047460` returns 1 on battery (Apple
  slows its updates while discharging), not on external power.
- Rescue if a resume ever loops: DFU and `bootloader/boot-v6/rescue-noresume.dfu` (built with
  `-DOSCHAIN_NORESUME`).

## 11. Firmware partition and self-update (`ospatch/fwpart.py`, `runtime/update.c`)

- Partition (type 0x3f, LBA 63): `]ih[` at +0x100, directory at +0x5000 (12 × 40-byte entries),
  section data at devOff + 0x1000, checksum = 32-bit byte sum. Sections in order: `rsrc`, `osos`,
  `aupd`, `hash` (empty), our `osfl`; each starts where the previous one's data ends. Apple's
  `osos` stays so MENU still boots stock. The bootloader, the OS and `update.c` find every section
  through the directory.
- `rsrc` is a FAT16 volume at devOff + 0x1000 + entryOff (0xe00), mounted by the OS as volume 4
  (`0x080bb914`, tag `rsrc`). Its `len` must be whole 4096-byte blocks (`0x0806eb80` refuses
  others, and the OS then has no fonts). `fwpart.py rsrc PARTITION.bin TREE OUT.bin` adds TREE's
  files (`tools/sf/rsrc_tree.py` assembles it) to Apple's, growing the volume in place
  (`rsrcvol.py`: Apple's files keep their clusters) and moving the later sections up by the growth.
  It is a one-time rewrite of the whole partition (dump, `fwpart.py rsrc`, `fwpart.py add`, `dd`),
  then images go on through `/osos-update.dec` as before.
- Self-update: copy a new image to `/osos-update.dec`; at boot it is validated, written in 64 KB
  chunks through the block device class, read back and compared, the directory entry updated, the
  file renamed to `osos-update.done`, then a watchdog restart. A marker (`FLAC\update.try`) stops a
  second attempt after a crash.
- The image carries its own check: mkpatch18.py stores `ESUM` and the byte sum of the body at
  header offset 0x7f8. The updater reads the whole file and compares before touching the partition,
  and sums again during the copy; a mismatch at the end leaves the section invalid, so the
  bootloader boots `/osos-v18.dec`. On the device a read once returned one cluster for every later
  one, and the old updater installed that. Images without `ESUM` are refused (install them by `dd`).
- During an update `kfix_stock(1)` (kfix.c) turns every kernel speedup off at run time: the
  D-cache, heap-cache and array hooks fall through to Apple's code, `e_mdelay` spins, and the libc
  veneers point back at Apple's IRAM routines. The FAT allocator hooks stay on (section 9).
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
    --e runtime/e.bin --syms runtime/e.syms --out ospatch/osos-v18.dec --hooks all --governor-floor \
    --ui-fast --ui-lazy --games-defer
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

CPU exceptions (`runtime/crash.c`): the IRAM vectors (undef 0x04, prefetch 0x0c, data abort 0x10,
reserved 0x14, FIQ 0x1c) reach stubs at `0x0802dcfc..` that call `0x0803930c(regs, kind)`, Apple's
CPU_exception_handler; it resets at once (`0x0835d028`: watchdog off, SWRCON = 0xaa5). The C
library's exit `0x08026054` and abort `0x0802606c` end there too (`0x082a71c8`, kind 8), but only
after a stdio teardown that can block the task for good: an assert (`0x0804327c`, 28 callers:
message to stderr, then exit(1)) froze the task in QEMU instead of resetting. `e_cpu_exc` (also
entered from both entries, `hk_exit`) first writes a record 0xc00 below the heap base (registers,
SPSR, banked sp/lr, DFSR/IFSR/FAR, 1 KB of stack, the last 512 bytes logged, the assert's file and
line from `e_assert_print`, the last 32 entries of Apple's error trail, the build's region E end),
cleans it from the D-cache, then jumps to the reset `0x0835d028` (skipping the handler's svc, which
would fault again on a broken SVC stack). Apple's error trail: `0x0803ebac(module, code, line, a3,
a4)` (197 callers, every error path) packs `module<<24 | code<<12 | line` into a 16-entry ring per
task; `hk_errtrail` mirrors each call into a global ring. Bootloader BSS (0x08800000..0x088aa440)
and the staged image (0x0a000000..) miss the record. The next boot appends the report to
`FLAC\crash.txt` and logs a `CRASH in the previous run` line. Emulator tests: `EDEFS=-DCRASH_TEST`
(undefined instruction after the first frame), `-DCRASH_TEST=2` (an error trail entry, then an
assert), `-DCRASH_TEST=3` (audio engine event 4 with no song played; add `-DNO_LISTENER_GUARD`
to see the crash). DRAM keeps its contents across the reset on the device.

Audio engine listener (`audio_listener_guard`, crash.c): Apple's IRAM audio engine (task loop
`0x220047b0`, event handler `0x22004eec`, also `0x22005298`) reports pause/stop/end of stream to a
listener at `0x22008c98` (vt[0xc4], vt[0xbc]), set only by the music player's start
(`0x081f6798` → `0x220056d0`, the listener is that player object). The video player
(`MeCCA_VideoBufferMgr`, stop `0x081b09b4` → `0x220054a8`, EndOfStream `0x080ab90c`) drives the same
engine without setting it, so pausing or stopping a video before any song since boot called through 0
(device: pc `0x22004f88`, far `0xea0000c6`). At boot an empty slot gets an object whose methods all
return 0; the music player's start replaces it as before.

RTXCbug (`runtime/semihost.c`): Apple left the Quadros RTXC v3.2b kernel debugger in the OS. Its
task (0x0835d424, priority 0) waits on semaphore 56, which nothing in the OS signals; on wake it
blocks every other task (KS_block 1..n), runs the menu 0x082b4248, and unblocks on exit. All its
I/O is ARM semihosting (`svc 0x123456`): output SYS_WRITE0, input SYS_OPEN ":tt" + SYS_READ
(0x082cb834), the `-- More --` pager SYS_READC. The SWI stub's call (0x0802dd1c) goes to `e_swi`,
which keeps output in a buffer (128 KB during a session, 2 KB otherwise; overflow is cut and
marked) and answers reads from a script. Usage: put
RTXCbug input lines in `FLAC\rtxcbug.txt`; at the first frame (and after a USB session) it is
deleted, run through `KS_signal(56)`, and the transcript lands in `FLAC\semihost.txt` (a line
"rtxcbug: ..." in rtlog). Without a script, Apple's prints since the last write (if any) go to
`FLAC\semihost-os.txt`, so a transcript is never overwritten by them. Those prints include C stdio
(SYS_WRITE from `_sys_write` 0x080270e4: printf, fprintf, assert messages), captured at all times. After the script, `x` is fed, so a session always ends. From E code:
`rtxcbug_run(script, n)` (the task, everything else blocked) or `rtxcbug_cmds(script, n)` (the
object menu called in place with IRQs off, nothing blocked; `! $ g x` refused, `u` ends it).
Main menu: `K` objects, `G`/`X` leave. Object menu (`H` lists it): `T` tasks (number, name,
priority, CPU time, state, what it waits on, timeout), `S` semaphores (name, PEND/WAIT/DONE,
waiter; names look shifted, the debugger's own sem 56 shows as S_DISPLAY and S_RTXCBUG is 53),
`Q` queues (depth, worst, count), `M` mailboxes, `R` resources (lock counts, conflicts, owner;
~1000 rows), `C` clock (1000 Hz tick, the timer list), `K` stack use per task plus the kernel
stack and worst interrupt nesting, `#` then a task number: its saved registers (tasks run in
SVC32), `Z` zeroes partition/queue/resource statistics, `$` task manager (suspend, resume,
terminate, execute, priority, block, unblock, time slice: changes the kernel, use with care),
`U` back, `X` leave. QEMU's iPod machine answers semihosting itself unless started with
`-M iPod-Classic,semihosting=off`.

## 14. Open items
- `art.c` trace legend partly stale (see §6).
- Skipping tracks with the headset remote occasionally stopped playback (not reproduced since).
