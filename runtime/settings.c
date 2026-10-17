/* Settings menu rows (Theme, Accent Colour, Library, Hearing), their submenus, the settings file and
 * the sunset/sunrise switch. Menu resources come from host/gen_settings.py (settings_data.h).
 * Theme, accent and style changes apply live (switch_live); only Modern's font waits for a restart. */
#include "e.h"
#include "settings_data.h"
#include "sun.h"
#include "cities.h"
#include "flac/hearing.h"

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
/* The root view (0x0811340c) and its reload walk, slot 0xd8 (0x0814d394): what Apple's language
   change runs (0x081e1444) after switching the strings. Every visible view re-reads its resources
   and updates in place (0x082636e8 then each class's slot 0xd4), so the screens on the stack keep
   their place. After a theme apply it picks up the recoloured resources the same way. */
#define os_root_view     ((uint8_t *(*)(void))0x0811340c)
#define WDTCON (*(volatile uint32_t *)0x3c800000)              /* S5L8702 watchdog: 0x100000 resets the SoC */

int theme_apply(int mode, int accent, int rebuild_ui);
extern int theme_mode, theme_accent;
extern char sync_summary[];          /* ossync.c: the last sync's one-line result */

/* The choice the early hook falls back to when the settings file is not readable yet: the build's
   defaults (nothing writes into the boot image at run time). */
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

static const char *const fatigue_keys[4] = { "off", "gentle", "medium", "strong" };
static const char *const limit_keys[7] = { "off", "75", "80", "85", "90", "95", "100" };
static const char *const phones_keys[4] = { "earbuds", "inear", "overear", "highimp" };
void dose_load(void);
void dose_tick(void);
const char *dose_text(void);

static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int prefix(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }

/* ---- settings file ---- */
static int put_line(char *buf, int n, const char *key, const char *val)
{
    while (*key) buf[n++] = *key++;
    while (*val) buf[n++] = *val++;
    buf[n++] = '\n';
    return n;
}
static void settings_save(void)
{
    char buf[256];
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
    n = put_line(buf, n, "limit=", limit_keys[hear_limit]);
    n = put_line(buf, n, "loudness=", hear_loud ? "on" : "off");
    n = put_line(buf, n, "fatigue=", fatigue_keys[hear_fatigue & 3]);
    n = put_line(buf, n, "crossfeed=", hear_xfeed ? "on" : "off");
    n = put_line(buf, n, "phones=", phones_keys[hear_phones & 3]);
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
        } else if (prefix(buf + i, "loudness=")) hear_loud = prefix(buf + i + 9, "on");
        else if (prefix(buf + i, "crossfeed=")) hear_xfeed = prefix(buf + i + 10, "on");
        else if (prefix(buf + i, "fatigue=")) {
            for (k = 0; k < 4; k++) if (prefix(buf + i + 8, fatigue_keys[k])) hear_fatigue = k;
        } else if (prefix(buf + i, "limit=")) {
            for (k = 0; k < 7; k++) if (prefix(buf + i + 6, limit_keys[k])) hear_limit = k;
        } else if (prefix(buf + i, "phones=")) {
            for (k = 0; k < 4; k++) if (prefix(buf + i + 7, phones_keys[k])) hear_phones = k;
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
    hear_gen++;
    return 1;
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
    crumb_state(1);                       /* DIAG */
#ifdef PROF
#ifdef PROF_UI
    { void prof_mark(const char *); extern int krec_on; extern uint32_t kwin; prof_mark("ui init"); krec_on = 1; kwin = TIMER_E; }
#else
    { void prof_mark(const char *); void prof_stop(int); void fat_state_log(const char *); fat_state_log("at ui init"); prof_mark("ui init"); prof_stop(1); }
#endif
#endif
    if (file_loaded) log_flush();      /* the raw handle stays open (fatdir.c): closing it would power the drive down before this write */
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
    dose_load();
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
static void post_hearing(void)
{
    void *obj = os_settings_obj();
    int k;
    os_post(obj, FOURCC('*', '*', '*', '*'), KEY_HEARING_TEXT);
    for (k = 0; k < N_HEARING_OPTIONS; k++) os_post(obj, FOURCC('*', '*', '*', '*'), KEY_HEARING_CHECK + k);
}
/* Hearing rows' value text, drawn on the right as Clicker's On/Off is */
static const char *hearing_value(int k)
{
    static const char *const limit[7] = { "Off", "75 dB", "80 dB", "85 dB", "90 dB", "95 dB", "100 dB" };
    static const char *const fatigue[4] = { "Off", "Gentle", "Medium", "Strong" };
    static const char *const phones[4] = { "Earbuds", "In-ear", "Over-ear", "Hi-Z" };
    return k == 0 ? limit[hear_limit] : k == 1 ? (hear_loud ? "On" : "Off") : k == 2 ? fatigue[hear_fatigue & 3]
         : k == 3 ? (hear_xfeed ? "On" : "Off") : phones[hear_phones & 3];
}
void os_restart(void);
static void reboot(void) { os_restart(); }
void os_restart(void)
{
    log_s("   settings: restart\n");
    log_flush();
    vol_sync();                                         /* a menu action: the cached FAT changes reach the disk */
    __asm__ volatile("msr CPSR_c, #0xd3");                  /* supervisor mode, IRQ and FIQ masked */
    WDTCON = 0x100000;
    for (;;) ;
}

/* a theme, accent or style change from a menu action: re-colour the resources, then let every
   view on screen reload them as a language change does. Each step flushes the log so a crash
   names the step. */
static void switch_live(int mode)
{
    uint8_t *root;
    log_s("   switch: apply\n"); log_flush();
    theme_apply(mode, theme_accent, 0);
    applied = mode;
    boot_mode = settings_mode;
    log_s("   switch: reload walk\n"); log_flush();
    if ((root = os_root_view())) ((void (*)(void *))((void **)*(void **)root)[0xd8 / 4])(root);
    log_s("   switch: walk\n"); log_flush();
    theme_walk(mode, theme_accent);                     /* colours views copied when they were built */
    log_s("   switch: redraw\n"); log_flush();
    { void theme_redraw_all(void); theme_redraw_all(); }   /* the status bar and other windows repaint */
    log_s("   settings: live switch to "); log_s(mode ? "dark" : "light"); log_s(theme_style ? ", modern" : ", classic"); log_s(", screens kept\n");
    log_flush();
    { extern int theme_walk_pending; theme_walk_pending = 1; }   /* ... and Now Playing when it next appears */
}

E_ENTRY int e_settings_action(void *self, const char *action, uint32_t arg)
{
    CRUMB();
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
        if (g == 3) {                                        /* Hearing: the decoder picks the change up at its next frame */
            if (k == 0) hear_limit = (hear_limit + 1) % 7;
            else if (k == 1) hear_loud ^= 1;
            else if (k == 2) hear_fatigue = (hear_fatigue + 1) & 3;
            else if (k == 3) hear_xfeed ^= 1;
            else hear_phones = (hear_phones + 1) & 3;
            hear_gen++;
            log_s("   settings: "); log_s(action); log_s(" -> limit "); log_s(limit_keys[hear_limit]); log_s(", loudness "); log_d(hear_loud);
            log_s(", fatigue "); log_s(fatigue_keys[hear_fatigue]); log_s(", crossfeed "); log_d(hear_xfeed);
            log_s(", phones "); log_s(phones_keys[hear_phones]); log_c('\n');
            settings_save();
            post_hearing();
            return 1;
        }
        if (g == 0 && k == 3) { settings_style ^= 1; theme_style = settings_style; }   /* "Modern style" (its font follows at the next start) */
        else if (g == 0) settings_mode = k; else theme_accent = k;
        log_s("   switch: action "); log_s(action); log_c('\n'); log_flush();
        settings_save();
        log_s("   switch: saved\n"); log_flush();
        switch_live(g ? (applied < 0 ? effective_mode(settings_mode) : applied) : effective_mode(settings_mode));
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
    CRUMB();
    int g, k;
    if (key == KEY_THEME_TEXT) { *out = (void *)theme_text(); return 1; }
    if (key == KEY_ACCENT_TEXT) { *out = (void *)accent_labels[theme_accent]; return 1; }
    if (key == KEY_LIBRARY_TEXT) { *out = (void *)sync_summary; return 1; }
    if (key == KEY_HEARING_TEXT) { *out = (void *)dose_text(); return 1; }
    if (key == KEY_DEBUG_TEXT) { *out = (void *)"Apple's diagnostics"; return 1; }
    if (key >= KEY_HEARING_CHECK && key < KEY_HEARING_CHECK + N_HEARING_OPTIONS) {
        if (type != T_BMAP) *out = (void *)hearing_value(key - KEY_HEARING_CHECK);
        return 1;
    }
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
    { void power_trail(uint32_t); power_trail('tick'); }   /* DIAG */
    { void power_resume_check(void); power_resume_check(); }
    dose_tick();
    /* no file calls and no theme switch here: this hook can run inside a FAT driver call. Automatic
       mode changes at the next start. */
    if (settings_mode == 2) {
        int m = is_night();
        if (m != applied && m != noted) {
            noted = m;
            log_s("   settings: Automatic: "); log_s(m ? "sunset" : "sunrise"); log_s(", the theme follows at the next start\n");
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
