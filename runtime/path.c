/* Track locations of any length. The OS keeps 35 characters of a location after
 * "iPod_Control\Music\" in a track record (+0xc0, 36 bytes). A longer location is copied to the
 * heap; the record then holds marker byte 1 and the copy's address ('FLPT' format, also read
 * by art.c track_path). Paths are UTF-8 bytes, passed through unchanged. */
#include "e.h"

#define LPMAGIC 0x54504c46   /* 'FLPT' */
#define PATH_MAX_OS 255      /* FileRef {u16; char path[255]} (0x0804f9dc) */
#define os_loc_db   ((int (*)(char *, const char *, int))0x080dc410)
#define os_loc_set  ((int (*)(void *, char *, int))0x0804fde0)
#define os_loc_path ((int (*)(const char *, void *))0x080605c8)

struct lp { struct lp *next; uint32_t magic; char s[]; };
static struct lp *gen_cur, *gen_prev;
int path_fixed;                     /* locations whose length loc_len() corrected, for the log */
/* Library health: FLAC tracks the OS loaded, and those with no artist (record +0x2c = 0) or no
   album (+0x34 = 0). Entries carry the location last, so the other strings are loaded by then. */
int lib_tracks, lib_noartist, lib_noalbum;
int path_calls_set, path_calls_path;      /* e_loc_set / e_loc_path calls during a load, for the log */

/* Frees the copies made two loads ago: the OS has torn those records down by now. */
void path_new_library(void)
{
    struct lp *p = gen_prev, *n;
    for (; p; p = n) { n = p->next; os_free(p); }
    gen_prev = gen_cur;
    gen_cur = 0;
    lib_tracks = lib_noartist = lib_noalbum = 0;
}

static int isflac(const char *p, int n)
{
    return n > 5 && p[n - 5] == '.' && (p[n - 4] | 32) == 'f' && (p[n - 3] | 32) == 'l' &&
        (p[n - 2] | 32) == 'a' && (p[n - 1] | 32) == 'c';
}

static const char ipc[] = ":iPod_Control:Music:";

/* s, n: the location in colon form when db is set, backslash form otherwise */
static int mark(char *loc, const char *s, int n, int db)
{
    int i, k;
    struct lp *m;
    uint32_t a;
    char c, *d;
    for (i = 0; i < 20 && i < n && (db ? s[i] : s[i] == '\\' ? ':' : s[i]) == ipc[i + 1 - db]; i++);
    if (i >= 19 + db && n <= 54 + db) return 0;        /* fits the record as the OS keeps it */
    if (n > PATH_MAX_OS) n = PATH_MAX_OS;
    if (!(m = os_malloc(sizeof *m + n + 1))) return 0;
    m->magic = LPMAGIC;
    d = m->s;
    for (i = (db && s[0] == ':'), k = 0; i < n; i++) {
        c = s[i];
        if (db) c = c == '\\' ? '_' : c == ':' ? '\\' : c;
        d[k++] = c;
    }
    d[k] = 0;
    if (db) { m->next = gen_cur; gen_cur = m; } else m->next = 0;
    a = (uint32_t)&m->magic;
    for (i = 0; i < 36; i++) loc[i] = 0;
    loc[0] = 1; loc[1] = a; loc[2] = a >> 8; loc[3] = a >> 16; loc[4] = a >> 24;
    return 1;
}

/* The loader converts a UTF-16 location to NUL-terminated UTF-8 (0x0803be14) but passes the
 * UTF-16 unit count as the length (0x080da08c), which cuts one byte per non-ASCII character.
 * If the string up to its NUL holds exactly n UTF-16 units, its real length is used. Raw
 * locations are not NUL-terminated and keep n. */
static int loc_len(const char *s, int n)
{
    int m = 0, units = 0;
    if (n <= 0) return n;
    while (m < 3 * n + 3 && m < 1024 && s[m]) {
        unsigned char c = s[m++];
        if ((c & 0xc0) != 0x80) units += c >= 0xf0 ? 2 : 1;
    }
    return m > n && !s[m] && units == n ? m : n;
}

/* 0x080da098: database load stores a track's location */
E_ENTRY int e_loc_db(char *t, const char *s, int n)
{
    int rc, m;
    if (s && (m = loc_len(s, n)) != n) { n = m; path_fixed++; }
    rc = os_loc_db(t, s, n);
    if (!rc && s && t[0xe4] == 1) mark(t + 0xc0, s, n, 1);
    if (s && isflac(s, n)) {
        t[0x1c] |= 0x40;                                   /* .flac: has artwork */
        lib_tracks++;
        if (!*(uint32_t *)(t + 0x2c)) lib_noartist++;
        if (!*(uint32_t *)(t + 0x34)) lib_noalbum++;
    }
    return rc;
}

/* 0x0805f738, 0x080d448c: file layer path {u16; chars} into a record */
E_ENTRY int e_loc_set(void *p, char *loc, int flag)
{
    const char *s = (const char *)p + 2;
    int n = 0;
    path_calls_set++;
    if (p && loc && flag) {
        while (s[n]) n++;
        if (mark(loc, s, n, 0)) { loc[36] = flag; return 0; }
    }
    return os_loc_set(p, loc, flag);
}

/* 0x08048288 and 4 more: record back to a path, written as {u16 0; chars} into a FileRef */
E_ENTRY int e_loc_path(const char *loc, void *dst)
{
    uint32_t a;
    const char *s;
    char *d = (char *)dst + 2;
    int i;
    path_calls_path++;
    if (loc && dst && loc[36] && loc[0] == 1) {
        a = (uint8_t)loc[1] | (uint8_t)loc[2] << 8 | (uint8_t)loc[3] << 16 | (uint32_t)(uint8_t)loc[4] << 24;
        if (!(a & 3) && (a >> 26) == 2 && *(uint32_t *)a == LPMAGIC) {
            s = (const char *)a + 4;
            *(uint16_t *)dst = 0;
            for (i = 0; i < PATH_MAX_OS && s[i]; i++) d[i] = s[i];
            d[i] = 0;
            return 0;
        }
    }
    return os_loc_path(loc, dst);
}
