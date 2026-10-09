/* libsync: keeps the iPod's iTunesDB in sync with the .flac files under /Music.
 * Incremental mode (the plugin): entries whose file is unchanged (same location, size and
 * mtime) are kept byte for byte; changed files keep their id, dbid, play counts and rating;
 * new files are added and missing ones dropped. Nothing is written if nothing changed.
 * Full mode rebuilds every FLAC entry as flacsync.py does, to compare byte for byte. */
#ifndef LIBSYNC_H
#define LIBSYNC_H
#include <stdint.h>
#include <stddef.h>

struct ls_dirent {
    const char *name;
    int is_dir;
    uint32_t size;
    uint32_t mtime;      /* seconds since 1970 as the file system reports it */
};

struct ls_opts {
    const char *root;    /* prefix for every path, "" on the iPod */
    const char *db_out;  /* NULL: safely replace the iPod's iTunesDB, else write here */
    const char *db_in;   /* NULL: <root>/iPod_Control/iTunes/iTunesDB */
    uint8_t fwid[8];     /* FireWire GUID, signs the database (hash58) */
    uint32_t now;        /* seconds since 1970 */
    uint8_t seed[8];     /* makes the new dbids unique */
    int full;
    int dry;             /* do everything but write */
    /* Called once per folder holding .flac files, in scan order, with the folder ("/Music/x"),
       its first .flac and its listing sorted by name. Pointers are valid only during the call. */
    void (*album_cb)(void *ctx, const char *dir, const char *first_flac, const struct ls_dirent *ents, int n);
    void *cb_ctx;
};

struct ls_result {
    int tracks;          /* tracks in the database afterwards */
    int flacs;           /* .flac files found under /Music */
    int kept, updated, added, removed, skipped;
    int renamed;         /* entries carried over to a moved or renamed file */
    int migrated;        /* entries of an older sync rebuilt (marker or string order) */
    int noalbum;         /* FLAC entries without an ALBUM tag (the OS lists those under no album) */
    int verified;        /* 1 if the new image passed ls_verify */
    int reordered;       /* 1 if the track list was put back in id order */
    int wrote;           /* 1 if a new database was written */
    int playcounts;      /* 1 if Play Counts were merged in */
    int playlists;       /* .m3u playlists written from /Playlists */
    int badsig;          /* the database isn't signed with this fwid, nothing written */
    char msg[96];        /* error, or empty */
};

/* mem: scratch memory, a few times the iTunesDB size plus room for the file list */
int ls_sync(const struct ls_opts *o, void *mem, size_t memsz, struct ls_result *r);

/* hash58 of a database image, also used to check the FireWire GUID */
void ls_hash58(const uint8_t fwid[8], const uint8_t *db, uint32_t len, uint8_t out[20]);

/* Checks an image against the rules the OS relies on. fwid NULL skips the signature; marker 1
   requires the entry marker on FLAC entries (legacy_ok entries may lack it); npl is the expected
   number of our m3u playlists per section, or -1. mem: 64 KB plus 48 bytes per track and album.
   Returns 0, or -1 with msg "verify: ...". */
int ls_verify(const uint8_t *db, uint32_t len, const uint8_t fwid[8], int marker, int legacy_ok, int npl,
              void *mem, size_t memsz, char *msg, size_t msgsz);

/* Port layer: implemented by host.c and by the plugin. */
int  ls_open_r(const char *path);
int  ls_open_w(const char *path);   /* create or truncate */
long ls_read(int fd, void *buf, long n);
long ls_write(int fd, const void *buf, long n);
long ls_seek(int fd, long off);     /* from the start */
long ls_fsize(int fd);
int  ls_close(int fd);
int  ls_remove(const char *path);
int  ls_rename(const char *from, const char *to);
void *ls_opendir(const char *path);
int  ls_readdir(void *dir, struct ls_dirent *e);   /* 1 entry, 0 end */
void ls_closedir(void *dir);
void ls_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void ls_status(const char *msg);   /* progress line for the screen */

#endif
