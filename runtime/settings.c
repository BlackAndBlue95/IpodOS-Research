/* Settings menu rows (Theme, Accent Colour, Library), their submenus, the settings file and the
 * sunset/sunrise switch. Menu resources come from host/gen_settings.py (settings_data.h).
 * Light/Dark/Automatic changes apply at the next start; the accent is live. */
#include "e.h"
#include "settings_data.h"
#include "sun.h"
#include "cities.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define T_BMAP FOURCC('B', 'M', 'a', 'p')
#define T_STR  FOURCC('S', 't', 'r', ' ')

#define os_settings_obj  ((void *(*)(void))0x081e01ec)
#define os_handle_action ((int (*)(void *, const char *, uint32_t))0x0821c690)
#define os_provider      ((int (*)(void *, uint32_t, uint32_t, void **))0x081e03ec)
#define os_post          ((void (*)(void *, uint32_t, uint32_t))0x08267428)
#define os_get_datetime  ((int (*)(uint8_t *))0x08083b08)      /* sec, min, hour, mday, month, -, u16 year, wday */
#define os_tz_info       ((void (*)(uint8_t *))0x0804b64c)     /* +10 i16 tz minutes, +12 i16 DST minutes */
#define os_rsrc_mgr       ((void *(*)(void))0x08267464)
#define os_get_resource   ((void *(*)(void *, uint32_t, uint32_t))0x08267294)         /* (mgr, type, id) -> data */
#define CHECKMARK         0x0dad0bcd                            /* Settings_MainMenu_CheckmarkBlack_Image (themed) */
#define PROBE 0xdeadbeefu                                     /* action argument that only asks whether the action is handled */
/* Music app getter (0x0817d524; argument ignored) and the library-reload handler (0x0817cd30):
   the reload clears app+0x118 and signals the event at app+0xec, and the UI thread then
   rebuilds the home screen. Used to rebuild the UI after a theme change. */
#define os_music_app     ((void *(*)(int))0x0817d524)
#define os_app_reloaded  ((void (*)(void *, int))0x0817cd30)
#define os_pop_to_main   ((void (*)(void))0x08287e30)      /* PopToMainScreen: pops (destroys) every other screen */
#define WDTCON (*(volatile uint32_t *)0x3c800000)              /* S5L8702 watchdog: 0x100000 resets the SoC */

int theme_apply(int mode, int accent, int rebuild_ui);
extern int theme_mode, theme_accent;
extern char sync_summary[];          /* ossync.c: the last sync's one-line result */

/* The choice the early hook reads. It sits in region E, at file offset
   IMG_HDR + IMG_E_LOAD + (address - E_BASE) in the image file; embed_write() updates it.
   The file is /osos-v18.dec until the bootloader boots the firmware-partition copy. */
#define IMG_FILE   "osos-v18.dec"
#define IMG_HDR    0x800u
#define IMG_E_LOAD 0xb3ded8u                                     /* mkpatch18.py E_LOAD */
#define THEME_MAGIC 0x454d4854u                                  /* "THME" */
volatile uint32_t theme_boot[4] __attribute__((aligned(16))) = { THEME_MAGIC, 0, 0, 0 };   /* magic, mode, accent, city + 1 (initialised data inside E) */

static const char *const mode_names[3] = { "Light", "Dark", "Automatic" };
static const char *const accent_labels[8] = { "Blue", "Purple", "Pink", "Red", "Orange", "Yellow", "Green", "Graphite" };
static const char *const accent_keys[8] = { "blue", "purple", "pink", "red", "orange", "yellow", "green", "graphite" };
int settings_mode = 0;        /* 0 light, 1 dark, 2 auto: the saved choice */
int settings_city = -1;       /* index into cities; -1 = from the iPod's time zone */
static int boot_mode = -1;    /* the choice the UI was built with (-1: early hook did not run) */
static int settings_style;    /* 0 classic (Apple's gradients), 1 modern (flat) */
extern int theme_style;       /* theme.c reads it while patching */
static int applied = -1;      /* theme mode (0/1) the UI was built with */
static int last_rise = -1, last_set = -1, last_tz, last_night;

static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int prefix(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }

/* ---- settings file ---- */
static void settings_save(void)
{
    char buf[96];
    int n = 0;
    const char *p;
    for (p = "theme="; *p; p++) buf[n++] = *p;
    for (p = settings_mode == 1 ? "dark" : settings_mode == 2 ? "auto" : "light"; *p; p++) buf[n++] = *p;
    buf[n++] = '\n';
    for (p = "accent="; *p; p++) buf[n++] = *p;
    for (p = accent_keys[theme_accent]; *p; p++) buf[n++] = *p;
    buf[n++] = '\n';
    for (p = settings_style ? "style=modern\n" : "style=classic\n"; *p; p++) buf[n++] = *p;
    if (settings_city >= 0) {
        for (p = "city="; *p; p++) buf[n++] = *p;
        for (p = cities[settings_city].name; *p; p++) buf[n++] = *p;
        buf[n++] = '\n';
    }
    vol_delete("FLAC\\settings.txt");
    file_write("FLAC\\settings.txt", buf, n);
}

static int settings_load(void)
{
    char buf[512];
    int n, sz, i, k;
    n = file_read("FLAC\\settings.txt", buf, sizeof buf - 1, &sz);
    if (n <= 0) return 0;
    buf[n] = 0;
    for (i = 0; i < n; i++) {
        if (i && buf[i - 1] != '\n') continue;
        if (prefix(buf + i, "theme=dark")) settings_mode = 1;
        else if (prefix(buf + i, "theme=auto")) settings_mode = 2;
        else if (prefix(buf + i, "theme=light")) settings_mode = 0;
        else if (prefix(buf + i, "style=modern")) settings_style = 1;
        else if (prefix(buf + i, "style=classic")) settings_style = 0;
        else if (prefix(buf + i, "accent=")) {
            for (k = 0; k < 8; k++) if (prefix(buf + i + 7, accent_keys[k])) theme_accent = k;
        } else if (prefix(buf + i, "city=")) {
            /* optional override of the place used for sunrise/sunset (no menu row for it) */
            for (k = 0; k < NCITIES; k++) {
                const char *c = cities[k].name;
                int j = 0;
                while (c[j] && buf[i + 5 + j] == c[j]) j++;
                if (!c[j] && (buf[i + 5 + j] == '\n' || buf[i + 5 + j] == '\r' || !buf[i + 5 + j])) settings_city = k;
            }
        }
    }
    theme_style = settings_style;
    return 1;
}

/* ---- the copy inside the image file ---- */
static int embed_write(void)
{
    void *m = os_malloc(0x114);
    File *f;
    uint32_t off = IMG_HDR + IMG_E_LOAD + ((uint32_t)theme_boot - E_BASE), magic = 0, n = 12, vals[3];
    int rc;
    vals[0] = settings_mode; vals[1] = theme_accent; vals[2] = settings_city + 1;
    if (theme_boot[1] == vals[0] && theme_boot[2] == vals[1] && theme_boot[3] == vals[2]) return 0;
    static int failed;
    if (failed) return -6;                                   /* the OS opens the image read-only (rc -4): stop after the first failure */
    if (!m || !(f = os_file_ctor(m, IMG_FILE, 1024, 1))) return -1;
    rc = f->vt->open(f);
    if (!rc) {
        if (f->vt->seek(f, (int64_t)off, 0) || f->vt->read(f, &magic, 4, 2) != 4 || magic != THEME_MAGIC) rc = -2;
        else if (f->vt->seek(f, (int64_t)off + 4, 0)) rc = -3;
        else rc = os_file_write(f, vals, &n) ? -4 : 0;
        f->vt->close(f);
    } else rc = -5;
    f->vt->del(f);
    if (!rc) { theme_boot[1] = vals[0]; theme_boot[2] = vals[1]; theme_boot[3] = vals[2]; } else failed = 1;
    log_s("   settings: image copy "); log_s(rc ? "not updated, rc " : "updated"); if (rc) log_d(rc); log_c('\n');
    return rc;
}

/* ---- sun: the place is the city nearest the iPod's Date & Time zone ---- */
static int tz_minutes(void)            /* local offset now, DST included */
{
    uint8_t info[64];
    int i;
    for (i = 0; i < 64; i++) info[i] = 0;
    os_tz_info(info);
    return (int16_t)(info[10] | info[11] << 8) + (int16_t)(info[12] | info[13] << 8);
}
static int tz_standard(void)           /* the zone's standard offset: what the city table lists */
{
    uint8_t info[64];
    int i;
    for (i = 0; i < 64; i++) info[i] = 0;
    os_tz_info(info);
    return (int16_t)(info[10] | info[11] << 8);
}
static int city_default(void)
{
    int tz = tz_standard(), k, best = 0, bd = 100000;
    for (k = 0; k < NCITIES; k++) {
        int d = cities[k].tz - tz;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}
static int city_now(void) { return settings_city >= 0 && settings_city < NCITIES ? settings_city : city_default(); }
/* 1 after sunset or before sunrise, at the current time */
static int is_night(void)
{
    uint8_t dt[16];
    int city = city_now(), rise, set, now, tz, y, m, d;
    os_get_datetime(dt);
    y = dt[6] | dt[7] << 8; m = dt[4]; d = dt[3];
    now = dt[2] * 60 + dt[1];
    tz = tz_minutes();
    if (sun_times(y, m, d, cities[city].lat / 10000.0, cities[city].lon / 10000.0, tz, &rise, &set)) {
        rise = 7 * 60; set = 18 * 60;                /* polar day or night: a fixed schedule */
    }
    last_rise = rise; last_set = set; last_tz = tz;
    return last_night = (now < rise || now >= set);
}
static int effective_mode(int choice) { return choice == 2 ? is_night() : choice; }

static void log_state(const char *what, int choice, int mode)
{
    log_s("   settings: "); log_s(what); log_s(" theme "); log_s(mode_names[choice]); log_s(" -> "); log_s(mode ? "dark" : "light");
    log_s(", accent "); log_s(accent_keys[theme_accent]);
    log_s(", city "); log_s(cities[city_now()].name);
    if (choice == 2) { log_s(", sunrise "); log_d(last_rise / 60); log_c(':'); log_d(last_rise % 60); log_s(" sunset "); log_d(last_set / 60); log_c(':'); log_d(last_set % 60); log_s(" tz "); log_d(last_tz); }
    log_c('\n');
}

/* ---- before the first view (root UI init hook, theme.c), since views keep the colours they are
   built with. The settings file is normally read by then; the image's copy is the fallback. ---- */
static int file_loaded;
void settings_early(void)
{
    int mode;
    if (!file_loaded) {
        if (theme_boot[0] != THEME_MAGIC) return;
        settings_mode = theme_boot[1] <= 2 ? (int)theme_boot[1] : 0;
        theme_accent = theme_boot[2] < 8 ? (int)theme_boot[2] : 0;
        settings_city = theme_boot[3] ? (int)theme_boot[3] - 1 : -1;
    }
    boot_mode = settings_mode;
    mode = effective_mode(boot_mode);
    theme_apply(mode, theme_accent, 0);   /* second pass on purpose: resources are reloaded after the library load, and without this the theme is mixed */
    applied = mode;
    { extern int theme_walk_pending; theme_walk_pending = 1; }   /* Now Playing: remap its stored colours when it first appears */
    log_state(file_loaded ? "ui init, from the file:" : "ui init, from the image:", boot_mode, mode);
    log_t(7);
    log_s("   t: ui init at "); log_d((int)(TIMER_E / 1000)); log_s(" ms\n");
#ifdef PROF
#ifdef PROF_UI
    { void prof_mark(const char *); extern int krec_on; extern uint32_t kwin; prof_mark("ui init"); krec_on = 1; kwin = TIMER_E; }
#else
    { void prof_mark(const char *); void prof_stop(int); void fat_state_log(const char *); fat_state_log("at ui init"); prof_mark("ui init"); prof_stop(1); }
#endif
#endif
    fatdir_release();                  /* the library load's raw reads are done */
    if (file_loaded) log_flush();
}

/* ---- library load (file system up): the settings file is authoritative for the next start ---- */
void theme_walk(int mode, int accent);
/* called from the Music app's init hook first (final = 0: only if the settings file can be read
   yet), then from the library load (final = 1) */
void settings_boot(int final)
{
    static int done;
    if (done) return;
    file_loaded = settings_load();
    if (!file_loaded && !final) return;
    done = 1;
    if (boot_mode < 0) {
        /* the UI is not built yet: set everything up for it */
        int mode = effective_mode(settings_mode);
        theme_apply(mode, theme_accent, 0);
        applied = mode;
        { extern int theme_walk_pending; theme_walk_pending = 1; }
        log_state(final ? "library load:" : "app init:", settings_mode, mode);
    } else {
        /* the UI already exists (built from the image's copy): the accent is live, the rest at the next start */
        theme_apply(applied, theme_accent, 0);
        if (settings_mode != boot_mode) log_s("   settings: theme choice changed, takes effect at the next start\n");
    }
    /* no embed_write here: the OS opens the image read-only (rc -4), so the write would fail
       on every boot. A settings action still tries it. */
}

/* ---- the two vtable slots ---- */
static void post_keys(void)
{
    void *obj = os_settings_obj();
    int k;
    os_post(obj, FOURCC('*', '*', '*', '*'), KEY_THEME_TEXT);
    os_post(obj, FOURCC('*', '*', '*', '*'), KEY_ACCENT_TEXT);
    os_post(obj, FOURCC('*', '*', '*', '*'), KEY_LIBRARY_TEXT);
    for (k = 0; k < N_THEME_OPTIONS; k++) os_post(obj, FOURCC('*', '*', '*', '*'), KEY_THEME_CHECK + k);
    for (k = 0; k < N_ACCENT_OPTIONS; k++) os_post(obj, FOURCC('*', '*', '*', '*'), KEY_ACCENT_CHECK + k);
}
void os_restart(void);
static void reboot(void) { os_restart(); }
void os_restart(void)
{
    log_s("   settings: restart\n");
    log_flush();
    __asm__ volatile("msr CPSR_c, #0xd3");                  /* supervisor mode, IRQ and FIQ masked */
    WDTCON = 0x100000;
    for (;;) ;
}

/* a Light/Dark switch while the UI is up: re-colour, then rebuild the UI as after a library reload */
static void switch_live(int mode)
{
    log_s("   switch: apply\n"); log_flush();
    theme_apply(mode, theme_accent, 0);
    applied = mode;
    boot_mode = settings_mode;
    log_s("   settings: live switch to "); log_s(mode ? "dark" : "light"); log_s(": screens popped, home rebuilt\n");
    /* screens keep the colours they were built with, so pop them all and rebuild the home
       screen. Each step flushes the log so a crash names the step. */
    log_s("   switch: pop\n"); log_flush();
    os_pop_to_main();
    log_s("   switch: reloaded\n"); log_flush();
    os_app_reloaded(os_music_app(0), 1);
    log_s("   switch: walk\n"); log_flush();
    theme_walk(mode, theme_accent);                     /* the root canvas and other long-lived views */
    /* not theme_refresh_panes(): re-rendering the main menu's right pane (slot 0x210) from
       here crashed the OS */
    log_s("   switch: redraw\n"); log_flush();
    { void theme_redraw_all(void); theme_redraw_all(); }   /* the status bar and other windows repaint */
    log_s("   switch: done\n"); log_flush();
    { extern int theme_walk_pending; theme_walk_pending = 1; }   /* ... and Now Playing when it next appears */
}

E_ENTRY int e_settings_action(void *self, const char *action, uint32_t arg)
{
    unsigned i;
    for (i = 0; i < sizeof settings_actions / sizeof *settings_actions; i++) {
        int g = settings_actions[i].group, k = settings_actions[i].option;
        if (!streq(action, settings_actions[i].action)) continue;
        if (arg == PROBE) return 1;
        if (g == 0 && k == 4) { reboot(); return 1; }        /* "Restart to apply" */
        if (g == 2 && k == 0) {                              /* "Sync library now": the boot syncs, verifies and rebuilds art */
            log_s("   settings: sync library now -> restart\n");
            reboot();
            return 1;
        }
        if (g == 2 && k == 1) { void power_state_log(const char *); power_state_log("asked"); log_flush(); return 1; }
        if (g == 0 && k == 3) { settings_style ^= 1; theme_style = settings_style; }   /* "Modern style" toggle */
        else if (g == 0) settings_mode = k; else theme_accent = k;
        log_s("   switch: action "); log_s(action); log_c('\n'); log_flush();
        settings_save();
        log_s("   switch: saved\n"); log_flush();
        embed_write();
        log_s("   switch: image copy tried\n"); log_flush();
        if (g) theme_apply(applied < 0 ? effective_mode(settings_mode) : applied, theme_accent, 0);   /* accent: applied at once */
        else switch_live(effective_mode(settings_mode));
        post_keys();
        log_flush();
        return 1;
    }
    return os_handle_action(self, action, arg);
}

/* row value text, e.g. "Dark" or "Automatic, sunset 19:42" */
static const char *theme_text(void)
{
    static char buf[48];
    int n = 0, t;
    const char *p;
    if (settings_mode == 2) {
        is_night();
        t = last_night ? last_rise : last_set;
        for (p = last_night ? "Automatic, sunrise " : "Automatic, sunset "; *p; p++) buf[n++] = *p;
        buf[n++] = '0' + t / 600; buf[n++] = '0' + t / 60 % 10; buf[n++] = ':';
        buf[n++] = '0' + t % 60 / 10; buf[n++] = '0' + t % 10;
    } else {
        for (p = mode_names[settings_mode]; *p; p++) buf[n++] = *p;
    }
    buf[n] = 0;
    return buf;
}

E_ENTRY int e_settings_provider(void *self, uint32_t type, uint32_t key, void **out)
{
    int g, k;
    if (key == KEY_THEME_TEXT) { *out = (void *)theme_text(); return 1; }
    if (key == KEY_ACCENT_TEXT) { *out = (void *)accent_labels[theme_accent]; return 1; }
    if (key == KEY_LIBRARY_TEXT) { *out = (void *)sync_summary; return 1; }
    if (key >= KEY_LIBRARY_CHECK && key < KEY_LIBRARY_CHECK + N_LIBRARY_OPTIONS) return 1;   /* no checkmark */
    if (key >= KEY_THEME_CHECK && key < KEY_THEME_CHECK + N_THEME_OPTIONS) { g = 0; k = key - KEY_THEME_CHECK; }
    else if (key >= KEY_ACCENT_CHECK && key < KEY_ACCENT_CHECK + N_ACCENT_OPTIONS) { g = 1; k = key - KEY_ACCENT_CHECK; }
    else return os_provider(self, type, key, out);
    /* the submenu rows' checkmark (SORC role 0xc, asked for as a BMap): only the current option
       has one; *out untouched means no image, as Apple's list provider does (0x081e2444) */
    if (type == T_BMAP && (g ? k == theme_accent : k < 3 ? k == settings_mode : k == 3 && settings_style))
        *out = os_get_resource(os_rsrc_mgr(), T_BMAP, CHECKMARK);
    return 1;
}

/* ---- clock minute tick (entry hook at 0x0817ce28): in Automatic mode, switches the theme at
   sunrise and sunset ---- */
static int __attribute__((used)) tick_c(uint32_t *r)
{
    static int noted = -1;
    (void)r;
    { void power_resume_check(void); power_resume_check(); }
    if (settings_mode == 2) {
        int m = is_night();
        if (m != applied && m != noted) {
            noted = m;
            log_s("   settings: Automatic: "); log_s(m ? "sunset" : "sunrise"); log_c('\n');
            switch_live(m);
            os_post(os_settings_obj(), FOURCC('*', '*', '*', '*'), KEY_THEME_TEXT);
        }
    }
    return 0;                                   /* always run the original */
}
__attribute__((naked, section(".text.entry"), used)) void hk_tick(void)
{
    __asm__ volatile(
        "push {r0-r5, ip, lr}\n mov r0, sp\n ldr ip, 1f\n mov lr, pc\n bx ip\n"
        "pop {r0-r5, ip, lr}\n"
        ".word 0xe92d4010\n ldr pc, 2f\n"      /* the displaced push {r4, lr}, then entry+4 */
        "1: .word tick_c\n 2: .word 0x0817ce2c\n");
}
