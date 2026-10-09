/* Lives over the dead PCM reader at 0x08277bb8 (1420 bytes, PCM reader plus its helper). Replaces the
 * File ctor call in the WAV reader's open(). For .flac paths it loads
 * flacdec.bin once from the iPod disk and hands over to it.
 * v8: the trace lives in a heap buffer and is written to /flaclogNN.bin by
 * the OS's own file code (IRAM does not survive a reboot). */
#include <stdint.h>
typedef struct File File;
struct FileVT {
    void *(*dtor)(File *); void (*del)(File *); int (*open)(File *); int (*close)(File *);
    int (*read)(File *, void *, int, int); int (*seek)(File *, int64_t, int);
};
struct File { const struct FileVT *vt; };
typedef File *(*ctor_t)(void *, const char *, int, int);
#define os_file_ctor ((ctor_t)0x081ea554)
#define os_file_create ((int (*)(File *))0x081ea28c)
#define os_file_write ((int (*)(File *, const void *, uint32_t *))0x081ea508)
#define os_malloc ((void *(*)(unsigned))0x0829fefc)
#define os_free ((void (*)(void *))0x0829fe4c)

static const char pfx[] = "FLAC\\";
#define TRMAGIC 0x52544c46
/* flacdec.bin entry: table of its functions, [0] File ctor, [1] art find, [2] art load */
static void *const *api __attribute__((section(".text.zdata")));
static volatile uint32_t *trp __attribute__((section(".trp")));
#define TR trp

/* called by flacdec too, its address is fixed by the link order */
void __attribute__((used, noinline)) flac_dump(void)
{
    /* written next to Music/, the same root the track path uses */
    static const char nm[] = "flaclog00.bin";
    char name[96];

    uint32_t n = 256, k, p, i;
    File *f;
    if (!trp || trp[63] >= 200) return;

    for (p = 0; pfx[p]; p++) name[p] = pfx[p];
    for (i = 0; i < sizeof(nm); i++) name[p + i] = nm[i];
    k = ++trp[63];
    name[p + 7] = 'a' + (k >> 4); name[p + 8] = 'a' + (k & 15);
    f = os_file_ctor(os_malloc(0x114), name, 1024, 1);
    if (!f) return;
    trp[60] = os_file_create(f);
    if (!trp[60]) trp[61] = os_file_write(f, (const void *)trp, &n);
    f->vt->del(f);
}

static void __attribute__((noinline)) tinit(void)
{
    uint32_t *t;
    int i;
    if (trp || !(t = os_malloc(256))) return;
    for (i = 0; i < 64; i++) t[i] = 0;
    t[0] = TRMAGIC; trp = t;
}

static __attribute__((target("arm"), noinline)) void icinv(void)
{ __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory"); }

static int load(const char *pre, int plen)
{
    char name[96];
    static const char fn[] = "flacdec.bin";
    File *f; uint32_t h[4]; uint8_t *m; unsigned i; int ok = 0, rc;
    for (i = 0; i < (unsigned)plen; i++) name[i] = pre[i];
    for (i = 0; i < sizeof(fn); i++) name[plen + i] = fn[i];
    f = os_file_ctor(os_malloc(0x114), name, 1024, 1);
    if (!f) return 0;
    rc = f->vt->open(f);

    if (rc == 0) {
        rc = f->vt->read(f, h, 16, 2);
        if (rc == 16 && h[0] == 0x31424c46 && h[1] < 0x40000) {
            m = os_malloc(h[1] + h[2] * 4);
            if (m && f->vt->read(f, m, h[1] + h[2] * 4, 2) == (int)(h[1] + h[2] * 4)) {
                uint32_t *r = (uint32_t *)(m + h[1]);
                for (i = 0; i < h[2]; i++) *(uint32_t *)(m + r[i]) += (uint32_t)m;
                ((void (*)(void))0x0802ce00)();
                icinv();
                api = (void *const *)(m + h[3]);

                ok = 1;
            } else if (m) os_free(m);
        }
        f->vt->close(f);
    }
    f->vt->del(f);
    return ok;
}

static int __attribute__((noinline)) isflac(const char *p, int n)
{
    return n > 5 && p[n - 5] == '.' && (p[n - 4] | 32) == 'f' && (p[n - 3] | 32) == 'l' &&
        (p[n - 2] | 32) == 'a' && (p[n - 1] | 32) == 'c';
}

/* load flacdec.bin once */
static void __attribute__((noinline)) ensure(void)
{
    tinit();
    if (trp && !(TR[3] & 0x80)) { TR[3] |= 0x80; load(pfx, sizeof(pfx) - 1); }
}

/* Album art for FLAC tracks, done in flacdec: 0x080457e4 FindImageForTrack(artlib, track)
 * and 0x080f32ec ArtworkThumb::Load(thumb, alloc). */
#define os_art_find ((void *(*)(void *, void *))0x080457e4)
#define os_art_load ((void (*)(void *, void *))0x080f32ec)
void *art_find_t(void *lib, void *track)
{
    ensure();
    return api ? ((void *(*)(void *, void *))api[1])(lib, track) : os_art_find(lib, track);
}
void art_load_t(void *th, void *alloc)
{
    ensure();
    if (api) ((void (*)(void *, void *))api[2])(th, alloc); else os_art_load(th, alloc);
}
__attribute__((target("arm"), naked)) void art_find(void)
{ __asm__ volatile("ldr ip, 1f\n bx ip\n 1: .word art_find_t\n"); }
__attribute__((target("arm"), naked)) void art_load(void)
{ __asm__ volatile("ldr ip, 1f\n bx ip\n 1: .word art_load_t\n"); }

File *flac_hook_t(void *mem, const char *path, int a2, int a3)
{
    int n = 0;
    File *r;
    while (path[n]) n++;
    if (isflac(path, n)) {
        ensure();
        if (api) r = ((ctor_t)api[0])(mem, path, a2, a3);
        else r = os_file_ctor(mem, path, a2, a3);
        flac_dump();
        return r;
    }
    return os_file_ctor(mem, path, a2, a3);
}

/* ARM entry: the patched bl at 0x0828a96c lands here in ARM state */
__attribute__((target("arm"), naked, section(".text.flac_hook"))) void flac_hook(void)
{
    __asm__ volatile("ldr ip, 1f\n bx ip\n 1: .word flac_hook_t\n");
}

/* Full length paths. Track records keep only 35 chars of the location after
 * "iPod_Control\Music\". Locations that don't fit get a heap copy, and the
 * record holds a marker byte 1 plus the copy's address.
 * 0x080dc410(track, location, len): database load, stores location[20:55]
 *   with ':' -> '\' at track+0xc0, flag at +0xe4.
 * 0x0804fde0(path, loc, flag): file layer, path -> loc (path+21, 35 chars).
 * 0x080605c8(loc, path): loc -> "iPod_Control\Music\" + loc. */
#define LPMAGIC 0x54504c46
#define os_loc_db ((int (*)(char *, const char *, int))0x080dc410)
#define os_loc_set ((int (*)(void *, char *, int))0x0804fde0)
#define os_loc_path ((int (*)(const char *, void *))0x080605c8)
static const char ipc[] = ":iPod_Control:Music:";

/* s/n: location, colon form (db) or backslash form at +1 (file layer) */
static int __attribute__((noinline)) mark(char *loc, const char *s, int n, int db)
{
    int i, k;
    uint32_t *m, a;
    char *d, c;
    for (i = 0; i < 20 && i < n && (db ? s[i] : s[i] == '\\' ? ':' : s[i]) == ipc[i + 1 - db]; i++);
    if (i >= 19 + db && n <= 54 + db) return 0;
    if (n > 255) n = 255;
    m = os_malloc(n + 5);
    if (!m) return 0;
    m[0] = LPMAGIC;
    d = (char *)(m + 1);
    for (i = (db && s[0] == ':'), k = 0; i < n; i++) {
        c = s[i];
        if (db) c = c == '\\' ? '_' : c == ':' ? '\\' : c;
        d[k++] = c;
    }
    d[k] = 0;
    a = (uint32_t)m;
    for (i = 0; i < 36; i++) loc[i] = 0;
    loc[0] = 1; loc[1] = a; loc[2] = a >> 8; loc[3] = a >> 16; loc[4] = a >> 24;
    return 1;
}
int loc_db_t(char *t, const char *s, int n)
{
    int rc = os_loc_db(t, s, n);
    if (!rc && s && t[0xe4] == 1) mark(t + 0xc0, s, n, 1);
    if (isflac(s, n)) t[0x1c] |= 0x40;   /* .flac: has artwork */
    return rc;
}
int loc_set_t(void *p, char *loc, int flag)
{
    const char *s = (const char *)p + 2;
    int n = 0;
    if (p && loc && flag) {
        while (s[n]) n++;
        if (mark(loc, s, n, 0)) { loc[36] = flag; return 0; }
    }
    return os_loc_set(p, loc, flag);
}
int loc_path_t(const char *loc, void *dst)
{
    uint32_t a; const char *s; char *d = (char *)dst + 2; int i;
    if (loc && dst && loc[36] && loc[0] == 1) {
        a = (uint8_t)loc[1] | (uint8_t)loc[2] << 8 | (uint8_t)loc[3] << 16 | (uint32_t)(uint8_t)loc[4] << 24;
        if (!(a & 3) && (a >> 26) == 2 && *(uint32_t *)a == LPMAGIC) {
            s = (const char *)a + 4;
            *(uint16_t *)dst = 0;
            for (i = 0; i < 255 && s[i]; i++) d[i] = s[i];
            d[i] = 0;
            return 0;
        }
    }
    return os_loc_path(loc, dst);
}
__attribute__((target("arm"), naked)) void loc_db(void)
{ __asm__ volatile("ldr ip, 1f\n bx ip\n 1: .word loc_db_t\n"); }
__attribute__((target("arm"), naked)) void loc_set(void)
{ __asm__ volatile("ldr ip, 1f\n bx ip\n 1: .word loc_set_t\n"); }
__attribute__((target("arm"), naked)) void loc_path(void)
{ __asm__ volatile("ldr ip, 1f\n bx ip\n 1: .word loc_path_t\n"); }
