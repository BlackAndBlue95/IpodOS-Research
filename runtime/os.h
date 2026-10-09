/* Apple iPod Classic OS 2.0.4 (Firmware-35.9.0.4) entry points used by region E.
 * Addresses are DRAM run addresses. */
#ifndef E_OS_H
#define E_OS_H
#include <stdint.h>

#define E_BASE          0x08b33000u
#define E_RESERVE       0x00080000u     /* heap starts at E_BASE + E_RESERVE */

/* heap */
#define os_malloc       ((void *(*)(unsigned))0x0829fefc)
#define os_free         ((void (*)(void *))0x0829fe4c)

/* File class: ctor(mem 0x114, "DIR\\name", 1024, 1), then open() to read or create() to write */
typedef struct File File;
struct FileVT {
    void *(*dtor)(File *);
    void  (*del)(File *);                    /* closes and frees */
    int   (*open)(File *);                   /* 0 = ok */
    int   (*close)(File *);
    int   (*read)(File *, void *, int, int); /* (buf, len, 2) -> bytes */
    int   (*seek)(File *, int64_t, int);     /* 0 = ok */
    int   (*s6)(File *);                     /* size is max(s6, s7) */
    int   (*s7)(File *);
};
struct File { const struct FileVT *vt; };
#define os_file_ctor    ((File *(*)(void *, const char *, int, int))0x081ea554)
#define os_file_create  ((int (*)(File *))0x081ea28c)
#define os_file_write   ((int (*)(File *, const void *, uint32_t *))0x081ea508)

/* PathString {vtable, char *} */
struct os_str { uint32_t w[2]; };
#define os_str_init     ((void (*)(struct os_str *, const char *))0x0826e3ac)
#define os_str_free     ((void (*)(struct os_str *))0x0826e424)
#define os_str_c        ((const char *(*)(struct os_str *))0x0829a1d8)

/* volume: lock(lk, 0); vol = get(lk, 1); ... unlock(lk). lk is 16 bytes */
#define os_vol_lock     ((void (*)(void *, int))0x081fbf68)
#define os_vol_get      ((void **(*)(void *, int))0x0817f1e4)
#define os_vol_unlock   ((void (*)(void *))0x081fbf94)
#define os_vol_fatpath  ((void (*)(void *, struct os_str *))0x081b2ae0)   /* "<letter>:\" + path */
/* FAT driver vtable 0x0898cb60 slots */
#define VOL_DELETE      0x5c   /* (vol, PathString *) -> 0 ok */
#define VOL_RMDIR       0x60
#define VOL_RENAME      0x64   /* (vol, PathString *path, PathString *new_name) -> 0 ok */

/* FAT find: first(rec, fatpath "...\\*.*") != 0 found; next(rec) 1/0; close(rec) after a hit */
#define os_findfirst    ((int (*)(void *, const char *))0x082d73ac)
#define os_findnext     ((int (*)(void *))0x082d74d8)
#define os_findclose    ((void (*)(void *))0x082d6df8)
#define FIND_SIZE       0x300
#define FIND_NAME       0x0d   /* long name, C string, up to 256 bytes */
#define FIND_ATTR       0x11c
#define FIND_TIME       0x11e  /* FAT write time */
#define FIND_DATE       0x120  /* FAT write date */
#define FIND_FSIZE      0x124

/* library load, called at boot (0x0804d7c4) and by TMusicLoadingTask (0x081ae29c) */
#define os_libload      ((void (*)(void *, void *, void *, void *))0x0805d62c)

/* SysInfo copy, FireWire GUID at +0x38 */
#define os_sysinfo      ((uint8_t *(*)(void))0x0835ed14)
#define os_vol_mount    ((int (*)(int))0x080e45dc)   /* mount FAT volume idx (0 = the music volume); 0 ok, no-op if mounted */

#define TIMER_E         (*(volatile uint32_t *)0x3c7000b4)

#endif
