# Kernel survey: Apple OS 2.0.4 (RTXC 3.2b) on the iPod Classic

A survey of the patched OS's kernel, CPU setup, primitives and waits for boot-time and run-time gains,
measured in the emulator or read from the image. Starting point: first frame 5.57 s on the device (R99).

`--sleep-delays`, `--fast-libc` and `--fast-dcache` (section 8) shipped in R100, with first frame at 4.58 s
together with `--heap-cache`, `--fast-prims` and `--ft-raster`. `--fb-damage` (section 10) shipped in R101.
`--iram-cache` and `--i2c-spin` have not been tried on the device.

## 1. Summary and recommended order

| # | Item | Where | Expected device gain (first frame) | Risk | Status |
|---|---|---|---|---|---|
| 1 | mdelay spins → sleep from task context | mdelay veneer 0x0802cf18; boot hit 0x0809f40c (75 ms) | **~75 ms** | low | `--sleep-delays` (8.1) |
| 2 | O(n) heap free lists (+ their lock) | 0x08191f50, 0x08192444, KS_lock on resource 25 | **~0.3 s** modelled | medium | `--heap-cache` (runtime/heapcache.c) |
| 3 | Apple's libc runs from uncached IRAM; region E's own primitives are byte/word loops | IRAM 0x22000020-0x22000317; flacblob.c, ossync.c | ~50-90 ms | low | `--fast-libc` (8.4) |
| 4 | IRAM code pages cacheable | MMU setup 0x22004534 | −0.01 to +0.3 s, depends on unmeasured IRAM latency (~0.14 s at 4 cycles) | medium | `--iram-cache` (8.2), needs one device A/B boot |
| 5 | DMA cache-maintenance loops | 0x08039d38 / 0x08039d70 / 0x0804e918 | ~20-30 ms | low | `--fast-dcache` (8.5) |
| 6 | I2C 100 µs gap enforced with a 1 ms sleep | 0x08360cac / 0x08360a78 | inconclusive in the emulator | low | `--i2c-spin` (8.3), device-only candidate |
| - | Tick rate, TLB lockdown, TCM, clock levels, SDRAM timing, PLD/LDRD, priorities | | ≤ ~20 ms or none | | not recommended (section 7) |

Items 1, 3 and 5 are low risk, and their emulator gains are measured (8.6). Item 2 is the largest single CPU
cost in the UI build; `--heap-cache` addresses it. Item 4 is the only remaining CPU-configuration lever. Its gain can only be measured on the device: one
boot with the flag and one without, comparing the `t: first frame` line already in rtlog.

Apart from item 4, the CPU setup is sound. DRAM is write-back cached, the page table sits in IRAM, the idle
loop uses WFI, the whole boot runs at L0 (216 MHz) and the division helpers already use `clz`. The kernel's own
overhead (IRQ path 0.5-1.7% of instructions, scheduler 0.16%) is too small to be worth tuning. What costs time
is the OS's own work: heap walks, dynamic arrays and FreeType, plus memory stalls (section 3).

## 2. Method

- **Instruction-level profile.** `tools/qemu/kprof.c` is a TCG plugin. For each translation block it counts
  executed instructions, IRQ-handler instructions and memory accesses by region. It also models a 16 KB 4-way
  32-byte L1 I and D cache that follows Apple's page table: DRAM write-back with read-allocate only, IRAM and
  IO uncached. Triggers on region E symbols split the boot: `e_libload_boot`, `hk_uiinit`, `render_c` (first
  frame), and the glyph pass entry and exit inside `pb_glyphs_now`. `tools/qemu/kprof.py` prints the phase
  table and a function table ranked by modelled cycles.
- **Image.** Rebuilt R99 (md5 d46c4a36…, byte-identical to the device build) on `EF-fonts.img`, run with
  `-icount shift=3,sleep=off`. The prototypes were measured against a control built from the same tree
  (`kfix.c` linked in, flags off).
- **Cross-checks.** The IRQ sampler (`PROF=1 EDEFS="-DPROF_UI -DPROF_NOPROBES"`) agrees with the plugin on the
  task split: UI task 60%, idle 33%, task 34 2%. Its hot IRAM PCs 0x22004c10 (DMA enable) and 0x22001d6c (IRQ
  unmask) are sampling artefacts: the sample is taken where the interrupt is finally accepted. The device
  numbers come from the R99 rtlog (`backups/r99-boot-20261015`).
- **Cycle model.** QEMU ignores caches, so cycles are modelled as instructions × 1.5 + (I + D read misses) × 40
  + write misses × 6 + uncached IRAM fetches × 3 extra. Check against the device:

  | Window | Instructions (emu) | Device | Implied CPI at 216 MHz | Model |
  |---|---|---|---|---|
  | Boot glyph pass (fixed work, same fonts) | 34.6 M | 449 ms − 13 ms reads = 436 ms | **2.7** | 0.36 s |
  | UI init → first frame | 192.1 M | 3057 ms − 155 ms reads − waits ≈ 2.85 s | ~3.2 (upper bound: the device library is larger) | 2.32 s |

  A CPI near 3 on an ARM926 means roughly half the time goes to memory stalls. Even so, the model is within
  ~20% of the device, which is enough to rank items but not to promise milliseconds.

## 3. Where the boot's CPU goes (emulator, R99 image)

| Phase | Instructions | IRQ handler | Scheduler | Code fetched from uncached IRAM | L1 misses (I / D read / write) |
|---|---|---|---|---|---|
| OS start → library load | 19.4 M | 0.6% | 0.16% | 21.6% | 0.02 / 0.46 / 4.95 M |
| library load → UI init | 80.8 M | 1.6% | 0.16% | 9.2% | 0.23 / 0.33 / 0.94 M |
| UI init → first frame | 192.1 M | 0.5% | 0.16% | 11.3% (6.9% of it the mdelay spin) | 1.54 / 1.81 / 2.32 M |

On the device, the first window is 611 ms, but 19.4 M instructions are only ~0.15-0.2 s of CPU. The rest is
waiting (section 6.4). The library-load window is not comparable: the emulator's disks have 512-byte FAT
sectors, so `fatdir` falls back to the OS listing.

UI init → first frame, by modelled cycles (`kprof.py funcs … t1:t2`):

| Cycles | Instructions | Function | Note |
|---|---|---|---|
| 11.9% | 6.9% | IRAM mdelay/elapsed 0x22001ee8-0x22001f78 | the 75 ms spin (item 1) |
| 5.4% | 7.0% | 0x08192444 | heap: coalesce + size-sorted free-list insert |
| 3.4% | 1.9% | IRAM KS_* 0x22003b64-0x22004534 | 72k KS_lock/KS_unlock pairs, 52k of them the heap lock |
| 3.3% | 4.1% | 0x08191f50 | heap: first-fit malloc walk |
| 2.9% | 1.0% | IRAM memcpy/memmove | 536k write misses |
| 2.5 / 2.2 / 1.6 / 1.6% | 3.4 / 3.8 / 2.7 / 2.3% | 0x08299d48, 0x08299e38, 0x08299fcc, 0x081e3260 | dynamic arrays and view iterator (parallel session) |
| 2.1% | 1.1% | E:memcpy | 673k write misses (item 3) |
| 1.0% | 0.4% | IRAM memset/memclr | |
| 0.9% | 1.2% | E:e_memcmp | read-ahead verification of the first 8 fills |

## 4. Scheduler, tasks and kernel calls

- **RTXC v3.2b**, built with ARM ADS 1.0.1 (string at 0x07ffda2f). Tick 1000 Hz. 55 live tasks, all in SVC
  mode. A lower number is more urgent: RTXCbug 0, drivers 3-10, TimeManager 49, **task 34: 49**,
  ATAWorkLoopTask 51, **UI (MP3ExampleTask, TCB 0x08a24c24): 52**, idle 127 (rtxcbug-session-a dump).
- **IRQ path.** IRQ entry 0x22003080 runs at the VA 0 alias (0x3080). It saves state, switches to SVC with
  IRQs masked, counts nesting at 0x22008a50 (a byte) and calls the dispatcher 0x220080ac, which can return
  another task's stack: preemption happens at IRQ exit 0x220030ec. Measured: ~1.4k IRQs/s at boot, averaging
  **377 instructions per IRQ**, 0.5-1.7% of all instructions. All of it is fetched from uncached IRAM.
- **Scheduler** 0x220034e8: about 2k entries/s, 0.16% of instructions.
- **Idle loop** 0x22003b3c already uses WFI (`mcr p15, 0, r0, c7, c0, 4` at 0x22003b4c). No change needed.
- **KS_lock 0x22003fd0 / KS_unlock 0x220042b4** (veneers 0x0802cf30/38). Each enters and leaves the kernel
  through 0x220034f8/0x22003500 (IRQs off and on). The uncontended path is ~25 instructions each way. During
  the UI build there are 72k pairs. Callers by lr: 0x081921d8 (31,934) and 0x08192638 (19,799), both the
  heap lock (resource 25), then 0x0808953c (resource 0x4a, 8,173). They cost 3.4% of modelled cycles, mostly
  uncached fetches. Making the heap lock-free for small blocks, or caching IRAM (item 4), removes most of it.
- **Kernel waits** (`kdump`, plugin histograms): the UI task blocks on semaphore 0x61 while ATAWorkLoopTask
  (task 31) serves its disk reads (lr 0x08130764 / 0x0804ba94). That is disk latency, which the read-ahead
  already addresses. No lock convoy or priority inversion turned up on the UI path except item 1.
- **Priorities.** Task 34 (priority 49) runs the audio codec power-up 0x0809f1c8 and spins in mdelay(75)
  above the UI task. `--sleep-delays` fixes that case. The earlier "zen kernel" check put the total available
  from priority and tick tweaks at ≤ ~0.2 s, and the data here agrees.

## 5. CPU setup (ARM926EJ-S)

### 5.1 MMU and caches: 0x22004534 (called from boot init 0x22003a30, after the startup copy)
```
22004538  bl 0x22003178 / 0x220031a4 / 0x2200318c   MMU, D-cache, I-cache off
22004544  bl 0x220031b8 / 0x220031cc                invalidate I, D (+ drain WB)
2200454c  bl 0x22003670 -> 0x0802cddc               invalidate TLB
22004558  memclr(0x2200c000, 0x4000)                TTB: 16 KB in IRAM
22004578  map(0, 0x000fffff, 0x22000000, 0xc12)     VA 0..1 MB -> IRAM, uncached (vectors, IRQ path)
2200458c  map(0x22000000, 0x220fffff, 0x22000000, 0xc12)   IRAM, uncached
220045a8  map(dram, dram+size-1, dram, 0xc1e)       DRAM write-back (C=B=1)
220045c8  map(dram|0x80000000, …, dram, 0xc12)      uncached DRAM alias at 0x88000000
220045dc  map(0x38000000, 0x3fffffff, 0x38000000, 0xc12)   IO, strongly ordered
220045e4  TTB = 0x2200c000; DACR = 3 (domain 0 manager); FCSE PID 0; c1 |= 1<<23 (0x0802cdc8)
220045fc  I-cache on, D-cache on, MMU on
```
- The map helper 0x0805c210(va, va_end, pa, flags) writes 1 MB section descriptors (`pa | flags`), so the
  descriptor for both IRAM sections is 0x22000c12.
- Findings:
  - **IRAM is uncached for both instructions and data.** The kernel, IRQ path, memcpy/memset, governor and
    audio engine run from it. In the UI build 11.3% of instructions are fetched past the I-cache (21.7 M;
    8.5 M excluding the mdelay spin), plus 2.0 M uncached data reads. This is item 4.
  - DRAM is write-back. The D-cache allocates on reads only (ARM926), so write misses go to the 16-word write
    buffer: 2.3 M of them in the UI build, mostly from memcpy/memset destinations.
  - Only 1 MB sections are used. The TLB holds 64 entries and the boot's working set (DRAM sections + IRAM
    + IO) fits; misses walk a table in IRAM. TLB lockdown would gain nothing measurable.
  - No TCM: the image never touches c9. Whether the S5L8702 has TCM at all is unknown; reading the TCM
    status register (`mrc p15, 0, r0, c0, c0, 2`) once would settle it.
  - Replacement is random (control bit 14 clear). That is fine for working sets larger than the cache.
  - **Dead ARM940T code**: IRAM 0x220031e0-0x220033fc programs MPU regions (c6 by I/D, c2/c3/c5 as
    cacheable, bufferable and permission bits), and 0x22003114/0x22003128 set c1 bits 31:30 (ARM920
    clocking). It is never called on this path; it is leftover from an older SoC port.
- **Cache size.** Apple's set/way loops assume 128 sets × 32 B per way with the way field in bits 31:30,
  i.e. 16 KB 4-way. Both full-cache loops, clean 0x0802ce00 and clean+invalidate 0x0802ce34, step the way
  field by 0x20000000, which runs 8 passes instead of 4 (bit 29 should be zero), so every line is done
  twice. That is harmless but wasted. They run rarely: clean 3 times and clean+invalidate 0 times up to the
  first frame, plus the sleep path 0x2200053c.

### 5.2 Clocks and memory
- **Clock.** The whole boot runs at L0 (216/108/54 MHz). R99's governor line reads `time in L0-L3 20/0/0/0`
  and holds are armed around the sync and the loader. Clock levels are not a lever (never write CLKCON1).
- **SDRAM.** The OS has its own MIU routine (IRAM ~0x22002560: MIUSDPARA 0x1fb621, 0x1845, 0x790682b, EMRS
  0x8040, refresh derived from the clock). Its values match the ones Rockbox's bootloader programs, so the
  NOR bootloader swap did not change memory timing.

## 6. Primitives, interrupts, DMA and waits

### 6.1 libc-style primitives
| Routine | Address | Implementation | Verdict |
|---|---|---|---|
| memcpy | veneer 0x0802ced8 → IRAM 0x22000020 (788 callers) | RVCT `__rt_memcpy`: 32 B `ldm/stm` of 4+4 registers when aligned, shift-merge words otherwise | good code, but runs from uncached IRAM |
| memmove | 0x0802cf00 → 0x220000d4 | same, backwards | as above |
| memclr4 / memset / memclr | 0x0802cee0 → 0x2200027c; 0x0802cee8 → 0x220002d8 `(dst, n, c)`; 0x0802cef0 → 0x220002d4 (113 callers) | 32 B `stm` loop | as above |
| division and fixed point | 0x0802c90c, 0x0802cad0, 0x0802cbac, … | `clz`, then a jump into an unrolled subtract table | already ARMv5TE |
| kernel | e.g. KS_delay 0x22003d44 | compiled for v5TE (`strd`) | fine |

ARMv5TE instructions would not help further on this core: the ARM926EJ-S executes PLD as a no-op,
LDRD/STRD are no faster than LDM/STM, and there is no branch predictor.

Region E's own primitives are the weak ones (item 3). `memcpy` in `flacblob.c` copies 16 B per loop with
single `str` and falls back to bytes when misaligned; `memset` and `memmove` are byte loops; `e_memcmp`
compares bytes. In the UI build E:memcpy is 2.1% of modelled cycles (the read-ahead's 128 KB copies into the
OS buffers, 673k write misses) and e_memcmp 0.9%. A 32-byte `ldm/stm` copy, a word-wise compare and an `stm`
memset (or calls to Apple's IRAM routines) are a few dozen lines with no behaviour change.

### 6.2 Interrupts
See section 4: 377 instructions per IRQ at ~1.4 kHz. Dropping the tick to 100 Hz would save under 1% of CPU,
and every tick-based timeout in the OS would change with it, so it is not recommended. `--iram-cache` puts
the IRQ path (VA 0x3080…) in the I-cache instead.

### 6.3 DMA cache maintenance (item 5)
- Range helpers: clean+invalidate 0x08039d38 (24 callers; skips 0x8xxxxxxx alias addresses), clean 0x08039d70
  (7 callers) and invalidate 0x0804e918. Each line costs a `bl` to a two-instruction function
  (0x0802cdf8/0x0802cdf0/0x0802cde8): about 8 instructions per 32-byte line.
- From boot to first frame + 1 s there were 2551 clean+invalidate calls covering 563k lines (18 MB, an
  average of 7 KB per call), plus 41k clean and 32k invalidate lines. That is about 0.65% of UI-build
  instructions.
- Fix: an inline loop (3 instructions per line). For ranges of 32 KB or more, clean the whole D-cache by
  set/way instead (512 operations with the way step corrected), or use the ARM926's
  `mrc p15, 0, r15, c7, c14, 3` test-clean-invalidate loop. That is an entry hook on 0x08039d38/0x08039d70
  in region E. Expected ~10-25 ms on the boot, plus less CPU per LCD frame at run time.

### 6.4 Busy-waits and fixed waits
- **mdelay 0x22001f38** (veneer 0x0802cf18) spins on TIMER_E (0x3c7000b4). It has 15 call sites. On the
  boot: **75 ms at 0x0809f40c** (codec power-up in 0x0809f1c8, task 34, priority 49, during the UI build),
  10 ms at 0x0806dd6c (library-load phase) and four 1 ms waits. The other sites, such as 150 ms at
  0x0809f4b8, 25 ms at 0x08362cc8 and 20 ms at 0x080d1008, run at other times; they freeze every
  lower-priority task, the UI included, while they spin. No lock is held at either boot site (each codec
  register access takes and releases PMU lock 5 itself). → item 1.
- **I2C bus gap.** The read 0x08360cac and write 0x08360a78 paths wait until 100 µs have passed since the
  last transfer (`elapsed(last, 100)` 0x0802cfe0), sleeping one tick (`KS_delay(0, 1)`) per check whenever
  IRQs are on. So back-to-back transfers cost 1-2 ms instead of 0.1 ms: 25 / 91 / 91 such sleeps in the
  three boot phases. Completion itself is IRQ-driven (event wait, 100-tick timeout). Spinning out the gap
  (`--i2c-spin`) did not move the emulator's first frame, so I2C latency is not on its critical path. → item 6.
- **Fixed sleeps before the library load** (KS_delay, plugin histogram): ATA reset 10 + 10 ms (0x083613a8 /
  0x083613bc) and PMU drive supply 50 ms (0x08361150), already known; 60 ms at 0x08388b98 in an unidentified
  task entry (skipped when 0x080d837c returns 200); and the governor thread's periodic 100 ms (0x22002dc8),
  which gates nothing.
- **Device-only waiting before the library load.** The device takes 611 ms from OS start to the library load
  (appinit 179, then +432 with 16 ms of reads). The emulator spends 19.4 M instructions there, about
  0.15-0.2 s of device CPU. About 0.4 s on the device is therefore waiting the emulator cannot show:
  hardware ready bits, the drive power-up after the bootloader's `ata_sleepnow`, PMU transactions. This is
  the largest unexplained block before the UI. The next step is two or three `step:` crumbs between app init
  and the library load (normal log lines, not a probe build) to see which of these it is.

## 7. Considered and not recommended

| Idea | Why not |
|---|---|
| Tick rate 1 kHz → lower, tickless idle | IRQ path is 0.5-1.7% of instructions; every tick-based timeout in the OS changes. Battery-only interest. |
| Task priority changes | ≤ ~0.2 s available in total (earlier zen check), and the one real inversion (item 1) has a targeted fix. |
| TLB lockdown, page-table placement | Sections only, table already in IRAM; misses are rare. |
| TCM | Not used by the OS; existence on the S5L8702 unverified. |
| Faster clock level / SDRAM timing | Already L0 for the whole boot; SDRAM values equal Apple's. |
| PLD / LDRD rewrites of Apple's primitives | No effect on the ARM926; Apple's routines already use `ldm/stm` and `clz`. |
| Bufferable IO mapping | IO writes (LCD, PMU) need ordering. The frame engine uses DMA anyway. |
| Write-allocate for memset/memcpy | Not available on the ARM926 (read-allocate only). |

## 8. Prototypes (emulator only, behind new flags)

`runtime/kfix.c` holds `e_mdelay` and `e_mmu_iram`. The flags are in `ospatch/mkpatch18.py`, `checkpatch.py`
verifies each one (stock or E target) and they are documented in `ospatch/HOOKS.md`. All three combined pass
`checkpatch.py … all`.

### 8.1 `--sleep-delays`: the mdelay veneer → `e_mdelay`
Only for the audited call sites 0x0809f40c (codec power-up, 75 ms), 0x0809f4b8 (power-down, 150 ms) and
0x0806dd6c (10 ms), in a task, with IRQs on, no IRQ nesting (0x22008a50 = 0), a running TCB
(0x2200acf4 ≠ 0) and 2 ≤ ms ≤ 10000: it calls `sleep(ms + 1)` (0x080defd8 → KS_delay). Every other caller
spins as before. The extra tick keeps the "at least ms" guarantee.
- 9 boots each, UI init → first frame: control **2321 ± 57 ms**, `--sleep-delays` **2231 ± 43 ms** (−90 ms).
  First frame overall 5234 → 5106 ms. UI-build instructions 192.1 → 178.5 M (the spin is gone).
- Expected on the device: about the spin's length (75 ms), since the UI task gets that CPU back. The 150 ms
  codec power-down also stops freezing the UI at run time.
- The other sites (25/20/10/5 ms, I2C and LCD paths) were not audited and keep the spin.

### 8.2 `--iram-cache`: the MMU setup's bit-23 helper → `e_mmu_iram`
This runs while the MMU and caches are still off. It checks that both IRAM descriptors are the stock
0x22000c12, builds a 1 KB coarse table in region E's `.bss` (pages 0x22000000-0x22007fff write-through
cacheable `|0xffa`, the rest uncached `|0xff2`, identity) and points L1 entries 0x000 and 0x220 at it. Then
it sets bit 23 as the original helper did.
- **Safety evidence.** Code pages hold only code and constants after the startup copy. The plugin counted
  every store into 0x22000000-0x22007fff and VA 0-0x7fff across boot, 2 s of UI and a full video playback
  (AAC + H.264 in the joel tree). The only one is the reset-vector write at 0x22003a20, made before the MMU
  setup. Kernel data starts at 0x22008a00 and stays uncached; DMA buffers in IRAM are untouched.
- **Emulator.** It boots; the monitor shows both L1 entries = 0x08cf6c11 (coarse, table at 0x08cf6c00).
  Screens match the control (one screen differs by ~700 px, as control-vs-control runs do); the Classic style
  is identical; `videotest.py` PASS.
- **Gain (model only).** In the UI build it removes ~8.5 M uncached fetches (excluding the spin) plus the
  literal-pool reads, and adds ~0.35 M I-cache misses (`kprof iramc=1`). Net: −5 M cycles if IRAM answers in
  2 cycles (break-even), +15 M (~70 ms) at 4 cycles, +35 M (~160 ms) at 6. The IRQ path, kernel calls and
  the audio engine also get cheaper at run time.
- **Risks.** The hibernate resume restores CP15 and IRAM; the table lives in DRAM, which self-refresh keeps,
  but this was not testable in the emulator. A future patch that writes IRAM code at run time would need an
  I-cache invalidate. Recommended next step: one device boot with and without the flag.

### 8.3 `--i2c-spin`: the I2C gap loop's `blne KS_delay` → `nop` (0x08360ce4, 0x08360ab0)
The loop then spins on `elapsed(last, 100 µs)`, which it already does when IRQs are off. Emulator: 2309 ± 47
vs 2321 ± 57 ms, no measurable gain. Keep it only if device logs show the UI waiting on PMU/I2C (lock 5/17).

### 8.4 `--fast-libc`: Apple's libc routines run from a cached copy in region E
IRAM 0x22000020-0x22000317 (memcpy, memmove, their word-aligned entries, memclr4, memclr, memset) has no
literal pool and no absolute branches. mkpatch copies those 760 bytes verbatim into `kfix_libc` (region E,
cached DRAM) and points the nine DRAM veneers at the same offsets in the copy: 0x0802cedc, cee4, ceec,
cef4, cefc, cf04, cf24, cf2c, d09c (1,670 call sites). The code is identical; only the fetch path changes.
Region E's own `memcpy`, `memmove` and `memset` (flacblob.c) call the copy once it is filled, and
`e_memcmp` (ossync.c) compares words. Without the flag `kfix_libc` stays zero and E keeps its old loops.
checkpatch verifies the copy against the image's IRAM bytes and every veneer.

### 8.5 `--fast-dcache`: inline D-cache range maintenance
`hk_dc_cinv`, `hk_dc_clean` and `hk_dc_inv` replace 0x08039d38, 0x08039d70 and 0x0804e918 entirely
(entry hooks, return straight to the caller). They operate on the same lines as Apple's loops
(`(addr + i) & ~31` for i < len, so an unaligned tail line is left out exactly as before) at 5 instructions
per line instead of ~10 with a call. Clean+invalidate and clean of 32 KB or more use the ARM926's whole-cache
test-and-clean loop (`mrc p15, 0, r15, c7, c14, 3` / `c7, c10, 3`), a superset that is always safe.
Invalidate never goes whole-cache (that would drop other dirty data). The clean variants drain the write
buffer at the end, which Apple's did not.

### 8.6 Measured results (emulator, icount shift=3, EF-fonts.img)
The control is the same tree with all flags off (E 0x8b33000-0x8d02ca0). All variants pass `checkpatch.py …
all`. With all five flags: home, Music and navigation screens (SF and Classic disks) pixel-identical to the
control, `videotest.py` PASS (also `--fast-libc` and `--fast-dcache` alone), sync and read-ahead lines
identical (8 fills checked against the disk), no crash records.

| Flags | UI-window instructions (CPU ms at 8 ns) | Same, spin excluded | Model Mcycles UI, spin excluded (F = 2 / 4 / 6) | Model Mcycles boot → first frame (F = 4) | Timed UI init → first frame (7 boots) | Timed first frame |
|---|---|---|---|---|---|---|
| control | 192.1 M (1537) | 178.9 M | 426 / 443 / 460 | 692 | 2359 ± 19 ms | 5266 ± 10 |
| `--sleep-delays` | 178.5 M (1428) | 178.4 M | 422 / 439 / 456 | 688 | 2284 ± 31 (−75) | 5165 ± 32 (−101) |
| `--fast-libc` | 188.7 M (1510) | 175.5 M | 420 / 431 / 443 | 676 | 2308 ± 30 (−51) | 5248 ± 69 |
| `--fast-dcache` | 189.9 M (1519) | 176.7 M | 422 / 439 / 456 | 686 | 2305 ± 55 (−54) | 5242 ± 88 |
| `--iram-cache` | 191.7 M (1534) | 178.5 M | 432 / 432 / 433 | 662 | 2362 ± 56 (0: QEMU has no caches) | 5285 ± 99 |
| `--i2c-spin` | 178.9 M (1431)* | 178.7 M | 426 / 443 / 460 | 696 | 2313 ± 31 (−46?) | 5254 ± 49 |
| all five | 175.0 M (1400) | 174.3 M | 424 / 424 / 424 | 652 | 2244 ± 40 (−115) | 5085 ± 64 (−181) |

\* `--i2c-spin` shifts the codec task's 75 ms spin out of the UI window instead of removing it (boot total
+3 M instructions). Its timed gain is not reproducible: an earlier 9-boot set gave −12 ms.

How to read it:
- `--sleep-delays` removes 13.6 M spin instructions from the UI window. What it buys on the device is wall
  time, the spin's 75 ms.
- `--fast-libc` and `--fast-dcache` save 3.4 M and 2.2 M real instructions (−27 / −18 ms of icount CPU). In the
  cycle model they save 12 M and 4 M UI cycles at F = 4, about 55 and 20 ms at 216 MHz, or 16 M and 6 M cycles
  from boot to the first frame. The −50 ms timed figures are within ~2 standard errors, so treat them as an
  upper bound.
- `--iram-cache` alone: −11 M UI cycles at F = 4 (−6 at F = 2 is a loss, −27 at F = 6), −30 M from boot to
  the first frame (~0.14 s). Combined with `--fast-libc` part of that is shared, since both stop libc being
  fetched uncached.

## 9. Reproducing
- Heap figures (before `--heap-cache`): about 26 nodes per malloc and 41 per sorted insert, 17.1 M of the
  heap's 24.3 M UI-build instructions in those two loops, and 220k D-cache misses.
- Profile: build the image, run with `-plugin libkprof.so,out=k.out,trig=<e_libload_boot>,trig=<hk_uiinit>,trig=<render_c>`
  under `-icount shift=3,sleep=off`, then `kprof.py phases k.out` and `kprof.py funcs k.out <objdump of the
  body> runtime/e.syms t1:t2`. Trigger addresses change with every region E build; take them from `e.syms`.

## 10. memcpy/memmove while browsing menus (`--fb-damage`)

Question: which OS paths drive the 8.7% of modelled cycles (12.4 M write misses) that Apple's IRAM
memcpy/memmove cost in a key-driven browsing run. Answer: one call site. After each buffer swap, the
screen layer copies the whole 300 KB frame back into the new back buffer. The prototype
`--fb-damage` copies only what was drawn, which halves the copy-back per frame on average.

### 10.1 Method
- `tools/qemu/kprof.c` gained `cp=ADDR[:R]`, which keeps a record per {copy entry, caller lr, size in rR}
  with calls, first dst/src, and the instructions and D-cache misses spent inside IRAM libc until the call
  returns. Internal branches between entries count as one call. It also gained `wr=LO:HI`, which counts
  stores into a range by store PC, and phase marks taken by creating `OUT.mark` (QEMU owns SIGUSR1/2).
  `kprof.py copies` ranks the callers.
- R99-identical control (md5 d46c4a36…), EF-fonts.img, no icount (navigation needs real time). The window
  starts after "pre-build done" and covers wheel scrolling, Select/Menu through the main menu, Music,
  Settings and Extras. Entries profiled: 0x22000020, 0x220000d4, 0x22000188, 0x220001f4 (and memset).
- What each copy changes: gdb breakpoints at the copy compared src and dst row by row and logged the
  rects passed to the damage notifier 0x0809f4d0 (section 10.3).

### 10.2 Ranked callers (control)
Short run (wheel and Select around the main menu and Music, 223 M instructions, 575 M modelled cycles):

| Rank | Caller lr (function) | Entry | Calls | Size | Bytes | Share of window cycles | Write misses |
|---|---|---|---|---|---|---|---|
| 1 | 0x08282de0 (compositor 0x08282c38 → swap 0x082817ac, tail call at 0x08281824) | memcpy | 74 | 307,200 each | 22.2 MB | **14.2%** | 5.68 M |
| 2 | 0x083cf804 (0x083cf7cc) | memcpy | 5,917 | 4-25 B | 68 KB | 0.17% | 1k |
| 3 | 0x0828d108 (0x0828d058) | 0x22000188 | 9,230 | 20 B | 180 KB | 0.11% | 0 |
| 4 | 0x08299e90 (0x08299e38, dynamic array) | memcpy | 3,896 | 8-12 B | 33 KB | 0.06% | 0 |
| 5 | 0x083cd91c (0x083cd784) | memcpy | 2,240 | 20-34 B | 54 KB | 0.06% | 12k |
| 6 | 0x083cdd38 / 0x083cfe94 | memcpy | 72 / 59 | 4 B-8 KB | 69 KB each | 0.05 / 0.04% | 17k each |
| - | 59 other callers | | | ≤ 48 B mostly | | < 0.03% each | |

Long run (also Settings, Extras and the list screens, 1238 M instructions, 2903 M cycles): rank 1 had 138
calls, 41.4 MB, 5.25% of cycles and 10.6 M write misses. Ranks 2-12 together came to under 0.9%. The
0x0818cbe0 site makes 22k zero-length calls.

So the copy cost in browsing is rank 1. The rest are small record and string copies that are not worth
touching.

### 10.3 What rank 1 copies
- 0x082817ac(layer): the screen layer has two surfaces (+0x54, +0x58; 320×240, 32 bpp, stride 1280) and
  a current one (+0x5c). After the frame is presented it switches +0x5c to the other surface and copies the
  whole frame from the one just presented into it (`memcpy(new+4, old+4, height × stride)`), so that the
  next frame can draw only what changed. Here the buffers are 0x08a7a274 (static) and a heap block
  (0x097d03xx). The compositor 0x08282c38 (via 0x0826c7f0) and the layer draw 0x08282970 both reach the
  swap. One copy is about 1.1 M modelled cycles, about 5 ms at 216 MHz.
- The actual difference between src and dst at each copy, from gdb row compares:
  - a status-bar clock tick changes rows 4-15, x 6-60;
  - a wheel step changes 50 rows of the left list (x 0-160);
  - a preview-pane crossfade changes all 240 rows of the right half;
  - a slide transition changes rows 20-188 of the list pane;
  - some swaps change nothing.
  The rest of every 300 KB copy rewrites identical pixels.
- Who writes the two buffers (`wr=` over a broad tour: menus, transitions, Now Playing): only the copy-back
  itself, fill 0x080699c0 → 0x080e62cc, and blit 0x080e6728 → 0x080a7b68 / 0x080e1554 / 0x080732f8 /
  0x080cd2e4. Both primitives call the bitmap damage notifier 0x0809f4d0(bitmap, clipped rect) before
  writing. Bitmap = surface+4: {pixels, stride, bpp}; rect = {top, left, bottom, right}. In every logged
  copy the union of the notified rects covered the actual difference.

### 10.4 Options judged
| Option | Verdict |
|---|---|
| Copy only the drawn rects (damage from 0x0809f4d0) | **prototyped** (`--fb-damage`): pure software, checkable in the emulator bit for bit |
| PL080 DMA for the 300 KB copy | not now. QEMU has no PL080 model and no caches, so neither the channel setup nor the cache maintenance (clean the source, invalidate the 300 KB destination, wait before the next draw) can be checked before the device. It would also need a free channel next to the OS's LCD, audio and ATA use. Worth it only if the remaining full-frame copies (screen pushes) still matter after `--fb-damage` |
| Single buffering (+0x58 = 0 makes 0x082817ac return early) | no. The next frame would draw while the LCD transfer still reads the buffer, and the tearing is device-only |
| Avoid the copy-back entirely | no. Partial redraws depend on the back buffer holding the last frame |

### 10.5 `--fb-damage` (runtime/fbcopy.c)
- `hk_fbdmg` is an entry hook on 0x0809f4d0. It is about 12 instructions when the bitmap is not a screen
  buffer. For a screen buffer it keeps up to 4 merged rects per buffer, in that buffer's coordinates, so a
  bitmap that points inside the buffer counts too.
- `e_fbcopy` replaces the tail call at 0x08281824. It copies the union of both buffers' rects since the
  last swap, as one memcpy for full-width rects and one per row otherwise.
- It falls back to a full copy when tracking (re)starts (new buffer pair or size), when stride or bpp are
  unknown or change, or when the guard fails.
- Guard: before each partial copy, one 32-byte chunk per row outside the damage is compared between src
  and dst, rotating each frame. Any difference means some writer bypassed the notifier. The full frame is
  then copied, the feature turns off for the session, and `fbdmg: off, unseen write at row N` is logged.
  This costs about 400 D misses per swap.
- Emulator checks:
  - A build with `-DFB_VERIFY` compares the whole buffers after every partial copy and logs any mismatch.
    No mismatches in about 1,100 swaps:
    - main menu panes, Music, Videos, Photos, Podcasts, Extras (clocks, calendars, games), Settings
      (About, Repeat, EQ, Main Menu, Audiobooks) and Now Playing during playback (plain tree);
    - TV episode playback and menus (joel tree).
  - `-DFB_TEST_DROP` drops every other rect. The guard turned the feature off on the 3rd swap and the
    screen afterwards was correct.
  - Screens pixel-identical to the control wherever both runs reached the same screen. Real-time
    navigation drops Select presses at random in both builds; `PMUCHARGER=1` stops the emulated battery
    going critical mid-tour.
  - `videotest.py` PASS; `checkpatch.py … all` ALL OK (it checks both sites, stock or E).
- Measured (kprof, same browse keys, per swap): control 300 KB and 1.10 M modelled cycles; `--fb-damage`
  131 KB on average and 0.53 M cycles, the guard included. In that run the copy-back's share of the window
  fell from 5.25% to 2.9%.
  - Full-size copies remain where the OS redraws the whole screen (screen pushes, 48 of 156 swaps).
  - A wheel step copies about 32 KB instead of 300 KB.
  - Expected on the device: about 2.5 ms less CPU per frame on average, about 4.5 ms on scroll frames.
    Now Playing's once-a-second updates drop to the changed rows.
- Risks:
  - A writer that bypasses the notifier would leave stale pixels until the guard catches it, usually
    within a few frames. Then it is stock for the rest of the session.
  - Not exercised: Cover Flow's live path and the device-only NP path (0x08147a50), games, USB/sync screens.
  - Region E's own drawing (art.c, theme.c) writes images, not these buffers, as far as the tours showed.
  - In R101 on the device: no flicker seen while browsing.

### 10.6 Next costs seen in the same browse windows (not memcpy)
The heap (0x08192444 + 0x08191f50) is 20-31% of browsing cycles without `--heap-cache`.
Next comes the per-pixel blend fill: 0x080699c0 mode 34 calls 0x080e2564, 0x08266f0c and 0x08267210 for
every pixel (loop 0x08069ab0), 10-12% of instructions. The blit worker 0x080a7b68 is about 10%.
