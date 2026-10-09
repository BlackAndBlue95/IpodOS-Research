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

static void libload(int task, void *a0, void *a1, void *a2, void *a3)
{
    uint32_t t, res0;
    int sz, rc;
    if (!loads) e_debug = file_read("FLAC\\debug.txt", 0, 0, &sz) >= 0;
    loads++;
    log_t(1);
    log_s(task ? "== library reload after disk mode #" : "== library load at boot #");
    log_d(loads); log_s(", region E "); log_x((uint32_t)__e_start, 8); log_c('-'); log_x((uint32_t)__e_end, 8);
    log_s(" at "); log_d((int)(TIMER_E / 1000)); log_s(" ms");
    log_s(e_debug ? ", debug\n" : "\n");
    path_new_library();
    art_new_library();
    if (!task) {
        /* The OS reads the headset state from the Mikey chip (I2C 0x39) only on a plug event, so
           re-initialise the chip here (0x080e9ce4: reset, config, read back). The two plug
           events are posted at the end of this load. */
        ((void (*)(void))0x080e9ce4)();
    }
    /* After a USB session the OS leaves this volume unmounted. Its own loader mounts it 12
       instructions into LIBLOAD (0x080e45dc through 0x08058584); until then every file open
       returns 3. Mounting here is idempotent: at boot it does nothing. */
    t = TIMER_E;
    rc = os_vol_mount(0);
    log_s("   mount: volume 0 rc "); log_d(rc); log_s(" ("); log_d((int)(TIMER_E - t)); log_s(" us)\n");
    log_t(2);
    if (loads == 1) settings_boot(1);  /* settings file and theme, unless the app-init hook already did it */
    if (loads == 1) { void update_check(void); update_check(); }   /* /osos-update.dec -> firmware partition, restart */
    sync_library(task);                /* iTunesDB follows the .flac files before the OS reads it */
    if (sync_no_db) log_s("   sync: database not openable after the mount  <-- CHECK\n");
    log_t(3);
    res0 = power_residency(0);
    t = TIMER_E;
    path_calls_set = path_calls_path = 0; names_rebuilt = 0;
    boost_hold(10000);                 /* Apple's loader at full speed, however long the sync took */
    os_libload(a0, a1, a2, a3);
    log_s("   t: os_libload "); log_d((int)((TIMER_E - t) / 1000)); log_s(" ms, L0 residency +"); log_d((int)(power_residency(0) - res0));
    log_s("; during it: "); log_d(path_calls_path); log_s(" record->path, "); log_d(path_calls_set); log_s(" path->record, "); log_d(names_rebuilt); log_s(" long names rebuilt\n");
    log_t(4);
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
    artdb_update();                    /* FLAC\covers.db follows the album folders */
    log_t(5);
    if (!task) {
        ((void (*)(int))0x0802cf78)(0x3f);                      /* plug change (kernel event, code 3) */
        { void *tm = ((void *(*)(void))0x0802d040)(); ((void (*)(void *, int, int, int))0x0802d048)(tm, 2000, 0, 0x40); }   /* attention, in 2 s */
        log_s("   accessory: Mikey re-init at entry, plug change posted, attention in 2 s\n");
    }
    log_s("   "); log_d(path_fixed); log_s(" non-ASCII locations given their full length\n");
    path_fixed = 0;
    power_log();
    log_t(6);
    log_timeline();
    log_flush();
}

E_ENTRY void e_libload_boot(void *a0, void *a1, void *a2, void *a3) { libload(0, a0, a1, a2, a3); }
E_ENTRY void e_libload_task(void *a0, void *a1, void *a2, void *a3) { libload(1, a0, a1, a2, a3); }
