# v18 hook table (Apple OS 2.0.4, stock Firmware-35.9.0.4.osos.dec md5 be2bca20)

DRAM addr = body offset + 0x07ff5128; IRAM addr = 0x22000000 + body offset (body < 0xaed8);
.dec file offset = 0x800 + body offset. `mkpatch18.py` asserts every "stock" word before writing.

## Image layout (startup copy 0x220046e0, BSS zero 0x220047e8)
| What | Where | Stock | v18 |
|---|---|---|---|
| DRAM data copy length | IRAM 0x220047ac | 0x00000a84 | E_end - 0x08a0fc88 |
| heap base (real heap, 0x0804b41c) | 0x0804b44c | 0x08b32b58 | HEAP_BASE = 0x08b33000 + 512 KB |
| heap base (ADS init, no-op backends) | 0x0807bfac | 0x08b32b58 | HEAP_BASE |
| body size | IMG1 header +0x0c | 0x00a1b5f0 | new body length |
| region E | body 0xb3ded8 (zero padding from 0xa1b5e4) | - | E image, runs at 0x08b33000 |

Untouched on purpose: stack tops 0x0802dcf8 and IRAM 0x220030f4 (= 0x08b32b58, stacks inside BSS,
grow down); 0x0809d944/0x0809d948 (bounds of a record list inside the data region).

## Kept from v17 (plain data/instruction changes, no code of ours)
| Addr | Stock | v18 | Purpose |
|---|---|---|---|
| 0x08087260 | 0x0a000005 beq 0x0808727c | beq 0x08087214 | ".flac" classifies as type 3 (WAV reader) |
| 0x08087264 | 0xe3a02004 | 0xe3a02003 | "aifc" compare 3 chars, .aiff/.aifc stay AIFF |
| 0x080872f0 | "aiff" | "flac" | 4-char extension string |
| 0x0804d434 | 0x0a000004 | 0x0a000000 | missing ArtworkDB (-43) keeps an empty artwork library |
| 0x0804d3d0 | 0x1a00001d | nop | stat Artwork dir failed: carry on |
| 0x0804d410 | 0x1a00000d | nop | stat ArtworkDB failed: still try the load |
| 0x087f3fb4 (in "Shuffle Songs" 0x087f3fac) | "Shuffle Songs" | "Shuffle FLACs" | visible marker |

Not applied in v18: v17's PCM reader kill at 0x081ff0ac and the stub over 0x08277bb8..0x08278147
(E replaces both; the PCM reader stays stock).

## Call sites retargeted into E (same condition and link bits, new target)
| Sites | Stock target | E entry |
|---|---|---|
| 0x0828a96c (WAV Open) | bl 0x081ea554 File ctor | `e_flac_open` |
| 0x0804647c, 0x080464b0, 0x080464cc, 0x08092e40, 0x080931c0, 0x080c70b8, 0x08299628 | 0x080457e4 FindImageForTrack | `e_art_find` |
| 0x081aa97c, 0x08264ab4, 0x08264b78, 0x08264d20, 0x082bd214 | 0x080f32ec ArtworkThumb::Load | `e_art_load` |
| 0x080da098 | bl 0x080dc410 (DB location into record) | `e_loc_db` |
| 0x0805f738, 0x080d448c | bl 0x0804fde0 (path into record) | `e_loc_set` |
| 0x08048288, 0x080cc33c, 0x080d7fa0, 0x080d7fdc, 0x080da17c | bl 0x080605c8 (record to path) | `e_loc_path` |
| 0x0804d7c4 (boot) | bl 0x0805d62c library load | `e_libload_boot` |
| 0x081ae29c (TMusicLoadingTask::Run, disk-mode reload) | bl 0x0805d62c library load | `e_libload_task` |

## Function entries: `b E_hook` over the first instruction (E runs it and returns to entry+4)
| Entry | First word (asserted, second word also checked) | E hook | Was |
|---|---|---|---|
| 0x08133980 GetThumbLocation | 0xe92d47f0 (0xe24dde31) | `hk_gtl` | blob runtime hook |
| 0x0826d8e0 stream seek | 0xe92d43f8 (0xe1a04000) | `hk_seek` | blob runtime hook |
| 0x0826d5fc stream read | 0xe92d47f0 (0xe24dd020) | `hk_read` | blob runtime hook |
| 0x0828a5ac WAV parser | 0xe92d43f0 (0xe1a04000) | `hk_parse` | blob runtime hook |

## Data
| Addr | Stock | v18 | Purpose |
|---|---|---|---|
| IRAM 0x22003824 | 0x081a4900 (PCM decoder getter, called by the codec selector 0x22005d60) | `flac_select_shim` | FLAC decoder selection |

## FAT long names (region E, `--hooks all`)
| Addr | Stock | v18 | Purpose |
|---|---|---|---|
| 0x082d6314, 0x082d9b14 | bl 0x082d95dc (long-name batch converter) | `bl e_lfn_batch` | the OS starts a name's second-sector batch at dst + 13 × entries (one byte per character) and so corrupts non-ASCII names that straddle a sector; E continues at the end the converter 0x082ccb78 returns. Both the find name fill and the directory matcher (open) use it |

## Theme and Settings (region E, `--hooks all`)
| Addr | Stock | v18 | Purpose |
|---|---|---|---|
| 74 literals (list in mkpatch18.py `GREY_LITERALS`) | addresses of the OS grey globals 0x089cc8c0..d0 (white #AA #7F #55 black) | `theme_pal_*`, `theme_text`, `theme_white` in E, by role | the OS reads these colours through the pointer at draw time (e.g. the 5-level text style 0x4d80..84 at 0x08138210, view backgrounds 0x08262584.., frames 0x08262bc0.., row text 0x08092ca0, marquee 0x081a8690), so retargeting them themes views of any age live. Text drawn black → `theme_text` (light in dark); text drawn white → `theme_white` (always white); fills/frames → `theme_pal_*` (black stays black). Kept on the OS globals: the LCD debug overlay 0x0813dde0/ec and colour copies of unknown use (`KEEP`) |
| 0x0899bf4c, 0x0899fe64 | 0x0821c690 HandleAction | `e_settings_action` | the Theme / Accent submenu choices (`SetTheme_k`, `SetAccent_k`), else the original |
| 0x0898fedc | 0x081e03ec settings value provider | `e_settings_provider` | value text for keys 0x8930/0x8931, checkmark BMap for keys 0x8940+k / 0x8950+k (SORC role 0xc), else the original |
| 0x082856bc, 0x082855f0 | 0x083f8728 (UI name table, 3920 × {char *, pid}) | `settings_names` in E | the table gains our screen/layout names (E_Theme_Screen, E_Theme_Layout, E_Accent_*, E_Settings_*_Layout) so navigator.PushScreen / SwitchLayout resolve them |
| 0x0828564c, 0x082855e0 | cmp #3920 | cmp #3936 | name-table length (3936 = 0xf60 is encodable; 10 entries are padding copies of entry 0) |
| resource index (0x08400258) | Apple's type records and entries | see `settings_data.json` | ITEM 0x41, CEVT and SLst 0x0dad0c2e entries point at E (rows, events, layouts added); the index tables of 14 types (CEVT SCST SLst VSlt SEVT SLyt VCrv SSin VCvs VLyt TEVT ITEM SORC Str) are relocated to E with our ids appended: two submenu screens, four layouts, four canvases with 16 sub-instances, two info panes, 21 SORC, 13 Str, ITEM 0x54/0x55 |
| 0x0817ce28 (entry) | e92d4010 e1a04000 | `b hk_tick` | the clock's minute tick: Automatic notes sunset/sunrise (the switch needs a restart) |
| 0x081135a0 (entry) | e92d40f8 e59d5018 | `b hk_uiinit` | root UI init (creates the root view and window): the theme is applied here, before the first view exists, from `theme_boot` (the choice stored inside the image: file offset 0x800 + E_LOAD + (addr − E_BASE), magic "THME"; `embed_write()` updates it) |
| 0x08143c04 (entry) | e92d41f0 e24dde42 | `b hk_lcdinit` | LCD driver init: the driver object is captured so its slot 0x10 (clear colour, white at 0x08143dd4) can follow the theme |

Runtime (no image bytes): the themed COLR/BMap resources are patched in place in the package
(the OS's override map is useless here: 0x08194ed8() is a kind-2 source, so 0x08111c2c refuses);
the highlight gradient globals 0x089cc890/894, the style-1 gradient immediates (0x08261ed8..,
steps 0x0826259c..) and the text cursor immediates (0x080e7ab8..) are rewritten in RAM by
`theme.c`. The OS's own grey globals are never written: their 74 readers are retargeted (table above).
**Views copy their colours when they are built and the OS never rebuilds them** (the status bar
and Now Playing exist before the library loads; lists are built once and kept; pop-to-main and
the root view's slot 0xd8 do not help, verified on the device), so the theme must be in place
before the first view: the root UI init hook. A Light/Dark/Automatic change takes effect at the
next start (Theme submenu: "Restart to apply" = watchdog reset, WDTCON 0x3c800000 = 0x100000);
the accent is live (the highlight bar is drawn from globals each frame). A live switch also re-colours and then calls 0x0817cd30(0x0817d524(), 1) (what the library reload task does when the library is in), so the UI thread rebuilds the home screen the way it does after disk mode. After a theme or accent choice `settings.c` (views copy colours when
built): PopToMainScreen 0x08287e30, then pushes Settings and the submenu again the way Apple's
code does (screen = 0x081d4bd8(0x081d4b48(), screen, layout, 0); 0x08104b58()->vt[1](stack,
screen)). The submenu rows are static ITEM rows whose SORC has {0x80 Str, role 1} + {0x8900 key,
role 0xc}; role 0xc is asked for as a BMap and gets Settings_MainMenu_CheckmarkBlack_Image
(0x0dad0bcd) for the current option only, as Apple's list provider 0x081e2284 does.

## Power (optional, `--governor-floor`)
| Addr | Stock | v18 | Purpose |
|---|---|---|---|
| IRAM 0x22002a4c | 0xe3a00000 mov r0,#0 | 0xe3a00001 mov r0,#1 | PowerMgmt: a lit screen floors the level at L1 (108 MHz, 1100 mV) instead of pinning L0 (216 MHz, 1200 mV); load still raises it to L0 |

Region E never writes CLKCON1 or the core voltage; it asks for speed through the OS's hold API
(0x0802d060 → IRAM 0x2200441c). `osos_boot` v12c hands over Apple's clock gating
(PWRCON0 0x2007cd45, PWRCON1 0x0003efc9, CLKCON2/3 0x80008000, CLKCON4/5 0x00008000).

## Path encoding
Paths are 8-bit strings end to end inside the OS: the track record location (+0xc0), the FileRef
`{u16 len; char path[255]}` filled by 0x080605c8 / 0x0804f9dc (0x0804fc6c is a path join, not an
encoding conversion), the PathString class, and the FAT driver. E treats them as opaque bytes.
Whether they are UTF-8 is checked on the device with paths containing `ł` (not in Latin-1) and `í`.
