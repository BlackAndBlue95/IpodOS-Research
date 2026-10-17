/* Library load hook, called from the OS at boot (0x0804d7c4) and at the reload after disk
 * mode (0x081ae29c, TMusicLoadingTask). Runs the sync and the art database around the OS loader. */
#include "e.h"
#include "libsync.h"

extern char __e_start[], __e_end[];
void path_new_library(void);
extern int path_fixed, lib_tracks, lib_noartist, lib_noalbum, path_calls_set, path_calls_path, names_rebuilt;
void power_log(void);
void settings_boot(int final);
void sync_library(int task);
extern struct ls_result sync_last;
extern int sync_rc, sync_no_db;
void art_new_library(void);
void artdb_update(void);
int e_debug;                       /* FLAC\debug.txt exists: decoder trace logs (flac_dump) */

static int loads;
static int mikey_pending;

static void mikey_events(void)
{
    ((void (*)(int))0x0802cf78)(0x3f);                      /* plug change (kernel event, code 3) */
    { void *tm = ((void *(*)(void))0x0802d040)(); ((void (*)(void *, int, int, int))0x0802d048)(tm, 2000, 0, 0x40); }   /* attention, in 2 s */
    log_s("   accessory: plug change posted, attention in 2 s\n");
}

/* render pass hook: the first frame on screen is the end of the boot */
void libload_first_frame(void)
{
    static int seen;
    extern uint32_t first_frame_us;
    if (seen) return;
    seen = 1;
    first_frame_us = TIMER_E;
    crumb_state(2);                       /* DIAG */
    log_s("   t: first frame at "); log_d((int)(TIMER_E / 1000)); log_s(" ms\n");
    { extern uint32_t font_skipped; if (font_skipped) { log_s("   ui: "); log_d((int)font_skipped); log_s(" text elements skipped, font not available\n"); } }
    { void rc_stop(void); rc_stop(); }
    { void rtxcbug_script(void); rtxcbug_script(); }   /* FLAC\rtxcbug.txt, if any */
#ifdef RTXCBUG_TEST
    { int rtxcbug_cmds(const char *, int); static const char t[] = "t\nz\n"; rtxcbug_cmds(t, sizeof t - 1); }
#endif
#ifdef PROF_UI
    { void prof_mark(const char *); void prof_stop(int); prof_mark("first frame"); prof_stop(1); }
#endif
    log_flush();
    { extern uint32_t pb_lazy; void fatdir_release(void); if (!pb_lazy) fatdir_release(); }   /* lazy: at pre-build done */
#if CRASH_TEST == 2                              /* emulator only: exercises crash.c */
    log_s("   crash test: an Apple error trail entry, then an assert\n");
    ((void (*)(int, int, int, int, int))0x0803ebac)(13, 0x71, 105, 0x1111, 0x2222);
    ((void (*)(const char *, int, const char *))0x0804327c)("assertion failed on line %d of file %s", 123, "crash_test.c");
#elif CRASH_TEST == 3
    log_s("   crash test: audio engine event 4 (play/pause) with no song played\n");
    {
        void *eng = ((void *(*)(void))0x0802d0b0)();
        ((void (*)(void *, int, int))0x0802d0b8)(eng, 0, 4);
        log_s("   crash test: engine event returned\n");
        log_flush();
    }
#elif defined CRASH_TEST
    log_s("   crash test: undefined instruction\n");
    __asm__ volatile(".word 0xe7f000f0");
#endif
}

/* the boot stage on screen (updscreen.c), before Apple's display takes the LCD; not after USB */
void bootscreen_begin(void);
void bootscreen_end(void);
void bootscreen_line(int line, const char *s);
static void stage(int task, const char *s) { if (!task) { bootscreen_line(0, s); bootscreen_line(1, ""); } }

static void libload(int task, void *a0, void *a1, void *a2, void *a3)
{
    uint32_t t, res0;
    int sz, rc;
    if (!task) { bootscreen_begin(); stage(task, "Starting"); }
    if (!loads) e_debug = file_read("FLAC\\debug.txt", 0, 0, &sz) >= 0;
    loads++;
#ifdef PROF
    { void prof_mark(const char *); prof_mark(task ? "reload start" : "library load start"); }
#endif
    log_t(1);
    log_s(task ? "== library reload after disk mode #" : "== library load at boot #");
    log_d(loads); log_s(", region E "); log_x((uint32_t)__e_start, 8); log_c('-'); log_x((uint32_t)__e_end, 8);
    log_s(" at "); log_d((int)(TIMER_E / 1000)); log_s(" ms");
    log_s(e_debug ? ", debug\n" : "\n");
    if (!task) { void crash_report(void); crash_report(); }
    if (!task) { void power_trail_report(void); power_trail_report(); }   /* DIAG: resume trail */
    if (!task) { void audio_listener_guard(void); audio_listener_guard(); }   /* crash.c */
    path_new_library();
    art_new_library();
    if (!task) {
        ((void (*)(void))0x080e9ce4)();  /* headset chip init; its plug events follow the load */
        mikey_pending = 1;
    }
    /* After a USB session the OS leaves this volume unmounted. Its own loader mounts it 12
       instructions into LIBLOAD (0x080e45dc through 0x08058584); until then every file open
       returns 3. Mounting here is idempotent: at boot it does nothing. */
    t = TIMER_E;
    stage(task, "Mounting disk");
    rc = os_vol_mount(0);
    log_s("   mount: volume 0 rc "); log_d(rc); log_s(" ("); log_d((int)(TIMER_E - t)); log_s(" us)\n");
    log_t(2);
    stage(task, "Loading settings");
    if (loads == 1) settings_boot(1);  /* settings file and theme, unless the app-init hook already did it */
    if (loads == 1) { void update_check(void); update_check(); }   /* /osos-update.dec -> firmware partition, restart */
    log_s("   boot: settings and update check done, sync next\n"); log_flush();   /* a stall after here names its stage */
#ifdef PROF
    { void prof_mark(const char *); prof_mark("sync start"); }
#endif
    stage(task, "Syncing library");
    sync_library(task);                /* iTunesDB follows the .flac files before the OS reads it */
#ifdef PROF
    { void prof_mark(const char *); prof_mark("os_libload start"); }
#endif
    if (sync_no_db) log_s("   sync: database not openable after the mount  <-- CHECK\n");
    log_s("   boot: sync done, Apple's library load next\n"); log_flush();
    stage(task, "Loading library");
    log_t(3);
    res0 = power_residency(0);
    t = TIMER_E;
    path_calls_set = path_calls_path = 0; names_rebuilt = 0;
    boost_hold(10000);                 /* Apple's loader at full speed, however long the sync took */
    os_libload(a0, a1, a2, a3);
    log_s("   t: os_libload "); log_d((int)((TIMER_E - t) / 1000)); log_s(" ms, L0 residency +"); log_d((int)(power_residency(0) - res0));
    log_s("; during it: "); log_d(path_calls_path); log_s(" record->path, "); log_d(path_calls_set); log_s(" path->record, "); log_d(names_rebuilt); log_s(" long names rebuilt\n");
    log_t(4);
#ifdef PROF
    { void prof_mark(const char *); prof_mark("os_libload done"); }
#endif
    if (sync_rc < 0) {
        log_s("   library: "); log_d(lib_tracks); log_s(" tracks, sync FAILED ("); log_s(sync_last.msg); log_s(")  <-- CHECK\n");
    } else {
        /* tracks the OS dropped on the way: a FLAC without an album is expected only when its
           file has no ALBUM tag */
        int unresolved = lib_noalbum - sync_last.noalbum;
        if (unresolved < 0) unresolved = 0;
        log_s("   library: "); log_d(lib_tracks); log_s(" tracks, "); log_d(sync_last.flacs); log_s(" files, ");
        log_d(unresolved); log_s(" unresolved ("); log_d(lib_noalbum); log_s(" without album, "); log_d(lib_noartist);
        log_s(" without artist)"); log_s(unresolved || lib_tracks != sync_last.flacs - sync_last.skipped ? "  <-- CHECK\n" : "\n");
    }
    log_s("   boot: library loaded, art next\n"); log_flush();
    stage(task, "Updating album art");
    artdb_update();                    /* FLAC\covers.db follows the album folders */
    { void dose_commit(void); dose_commit(); }   /* FLAC\dose.txt: never from the minute tick */
    log_t(5);
#ifndef NO_MIKEY
    if (mikey_pending) { mikey_pending = 0; mikey_events(); }
#endif
    log_s("   "); log_d(path_fixed); log_s(" non-ASCII locations given their full length\n");
    path_fixed = 0;
    power_log();
    stage(task, "Starting UI");
    if (!task) bootscreen_end();       /* Apple's LCD init follows: the interface is its from here */
    log_t(6);
    log_timeline();
#ifdef PROF
#ifndef PROF_UI
    { void prof_mark(const char *); prof_mark(task ? "reload done" : "library load done"); extern int krec_on; extern uint32_t kwin; krec_on = 1; kwin = TIMER_E + 450000; }
#else
    { void prof_mark(const char *); prof_mark(task ? "reload done" : "library load done"); }
#endif
    { void fat_state_log(const char *); fat_state_log("before the log write"); }
#endif
    if (task) { void fatdir_release(void); fatdir_release(); }   /* at boot: pre-build done or the first frame */
    if (task) { void rtxcbug_script(void); rtxcbug_script(); }   /* after a USB session: FLAC\rtxcbug.txt, if any */
    log_flush();
}

E_ENTRY void e_libload_boot(void *a0, void *a1, void *a2, void *a3) { libload(0, a0, a1, a2, a3); }
E_ENTRY void e_libload_task(void *a0, void *a1, void *a2, void *a3) { libload(1, a0, a1, a2, a3); }

/* Album artists. Apple's Artists menu groups by the artist string, so the sync writes the album
 * artist there (libsync make_track) and keeps the song's own artist, when it differs, as the
 * comment (string type 8, track record +0x4c), which nothing else shows for FLAC entries. Apple's per-song artist
 * text (Now Playing, song rows: 0x08298f3c) reads the artist with 0x0804a840 (record, string out);
 * that call comes here and takes the comment first (0x0804a8d8, same form). */
int track_is_flac(const uint8_t *t);           /* path.c */
E_ENTRY void e_song_artist(uint8_t *track, void *out)
{
    CRUMB();
    /* FLAC entries only: tracks from iTunes or Finder keep their real comments there */
    if (*(uint32_t *)(track + 0x4c) && track_is_flac(track)) ((void (*)(void *, void *))0x0804a8d8)(track, out);
    else ((void (*)(void *, void *))0x0804a840)(track, out);
}
