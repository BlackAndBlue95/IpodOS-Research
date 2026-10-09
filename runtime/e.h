/* Region E: code linked into the OS image at E_BASE. */
#ifndef E_H
#define E_H
#include <stdint.h>
#include "os.h"

/* functions the OS patch branches to: kept by the linker, names looked up by mkpatch18.py */
#define E_ENTRY __attribute__((section(".text.entry"), used, noinline))

/* log.c */
void log_c(char c);
void log_s(const char *s);
void log_x(uint32_t v, int digits);
void log_d(int v);
void log_name(const char *s);
int log_flush(void);

/* osfile.c */
int file_write(const char *path, const void *data, uint32_t n);
int file_read(const char *path, void *buf, int max, int *size);
int vol_delete(const char *path);
int vol_rename(const char *path, const char *new_name);
int dir_list(const char *dir, int (*cb)(void *ctx, const char *name, int is_dir, uint32_t size,
                                        uint16_t date, uint16_t time), void *ctx);
uint32_t dos_mktime(uint16_t date, uint16_t time);
File *file_open(const char *path);          /* open for reading, NULL if not */
int file_size(File *f);

/* ossync.c */
struct sync_album {                                  /* an album folder as the sync scanned it */
    char *dir, *first;                               /* "Music\\Album", "Music\\Album\\01.flac" */
    uint8_t kind;                                    /* art source: 1..5 cover file (ossync.c cover_names), 6 the first FLAC, 0 none */
    char src[96];                                    /* its name in the folder */
    uint32_t srcsize, srcmtime;
};
const struct sync_album *sync_albums(int *n);        /* the last scan's album folders, n = -1 if it did not complete */

/* fatdir.c: folder listings read straight from the FAT32 volume */
int fatdir_begin(void);                              /* opens the block device, reads the BPB; 0 ok */
void fatdir_end(void);                               /* the device stays open */
void fatdir_release(void);                           /* closes the device: after the boot's last raw read */
int fatdir_list(const char *path, int (*cb)(void *ctx, const char *name, int is_dir, uint32_t size, uint16_t date, uint16_t time), void *ctx);
extern int fatdir_reads;
uint32_t fatdir_free_clusters(uint32_t *nclus);      /* FSInfo free clusters, 0 if unusable; *nclus = clusters on the volume */
extern int fatdir_free_why;
uint32_t fatdir_next_free(uint32_t start, uint32_t end, int (*confirm)(void *, uint32_t), void *ctx, int *reads);
uint32_t fatdir_nclus(void);

/* log.c: boot timeline */
void log_t(int slot);                                /* TIMER_E now into slot 0..11 */
void log_timeline(void);
/* power.c */
uint32_t power_residency(int level);

/* artdb.c: FLAC\covers.db, one block of the four thumbnail formats per album folder */
#define ARTDB_FILE  "FLAC\\covers.db"
#define ARTDB_BLOCK (128 * 256 + 320 * 640 + 55 * 112 + 128 * 256)
uint64_t artdb_key(const char *dir);                 /* FNV-1a 64 of the folder path under Music, '/' separated */
int artdb_lookup(uint64_t key, uint32_t *pixoff, uint32_t *srcoff, uint32_t *srclen, char *src, int srccap);
                                                     /* 1 = pixels in covers.db at pixoff; src = the jpeg (file, or
                                                        FLAC with srclen > 0); 0 = unknown folder; -1 = known, no art */
int art_decode_set(const char *src, uint32_t srcoff, uint32_t srclen, uint8_t *out);   /* art.c: the 4 formats */

/* power.c: full speed through the OS's own governor */
void boost_hold(uint32_t ms);
int boost_begin(void);
void boost_end(int boosted);
void power_log(void);

/* freestanding helpers (flac/flacblob.c) */
#include <stddef.h>
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

#endif
