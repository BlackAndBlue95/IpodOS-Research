/* Host build of libsync for testing on the host: the same sync code as the
 * plugin, with POSIX file access.
 *   libsync-host ROOT [--full] [--dry] [--in DB] [--out DB] [--now N] [--seed HEX] [--fwid HEX] [-v] [--albums]
 *   libsync-host --verify DB [--fwid HEX] [--nomarker] [--npl N]
 *   LS_FAULT=1..6 corrupts the built image (location order, duplicate item id, broken sort index,
 *   signature, album id, record length) so the tests can see ls_verify refuse it.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "libsync.h"

static int verbose, show_albums;
extern int ls_fault;
/* the art source, picked as runtime/ossync.c album_cb picks it: a cover file by name, else the
   first .flac */
static void album_cb(void *ctx, const char *dir, const char *first, const struct ls_dirent *e, int n)
{
    static const char *const names[5] = { "cover.jpg", "folder.jpg", "front.jpg", "cover.jpeg", "albumart.jpg" };
    int i, k, kind = 0, pick = -1;
    const char *fn = first + strlen(dir) + 1;
    (void)ctx;
    if (!show_albums) return;
    for (k = 0; k < 5 && !kind; k++)
        for (i = 0; i < n; i++) if (!e[i].is_dir && !strcasecmp(e[i].name, names[k])) { kind = k + 1; pick = i; break; }
    if (!kind) for (i = 0; i < n; i++) if (!e[i].is_dir && !strcmp(e[i].name, fn)) { kind = 6; pick = i; break; }
    printf("album %s | %s | %d | %d | %s | %u | %u\n", dir, first, n, kind, pick >= 0 ? e[pick].name : "-", pick >= 0 ? e[pick].size : 0, pick >= 0 ? e[pick].mtime : 0);
}

int ls_open_r(const char *p) { return open(p, O_RDONLY); }
int ls_open_w(const char *p) { return open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644); }
long ls_read(int fd, void *b, long n)
{
    long t = 0;
    while (t < n) {
        ssize_t k = read(fd, (char *)b + t, n - t);
        if (k <= 0) break;
        t += k;
    }
    return t;
}
long ls_write(int fd, const void *b, long n) { return write(fd, b, n); }
long ls_seek(int fd, long off) { return lseek(fd, off, SEEK_SET); }
long ls_fsize(int fd) { struct stat st; return fstat(fd, &st) ? -1 : st.st_size; }
int ls_close(int fd) { return close(fd); }
int ls_remove(const char *p) { return unlink(p); }
int ls_rename(const char *a, const char *b)
{
    struct stat st;
    if (!stat(b, &st)) return -1;   /* Rockbox's rename refuses an existing target too */
    return rename(a, b);
}

struct hdir { DIR *d; char path[1024]; };
void *ls_opendir(const char *p)
{
    DIR *d = opendir(p);
    struct hdir *h;
    if (!d) return NULL;
    h = malloc(sizeof *h);
    h->d = d;
    snprintf(h->path, sizeof h->path, "%s", p);
    return h;
}
int ls_readdir(void *v, struct ls_dirent *e)
{
    struct hdir *h = v;
    struct dirent *de = readdir(h->d);
    char full[2048];
    struct stat st;
    if (!de) return 0;
    snprintf(full, sizeof full, "%s/%s", h->path, de->d_name);
    if (lstat(full, &st)) memset(&st, 0, sizeof st);
    e->name = de->d_name;
    e->is_dir = S_ISDIR(st.st_mode);
    e->size = (uint32_t)st.st_size;
    e->mtime = (uint32_t)st.st_mtime;
    return 1;
}
void ls_closedir(void *v) { closedir(((struct hdir *)v)->d); free(v); }
void ls_qsort(void *b, size_t n, size_t s, int (*c)(const void *, const void *)) { qsort(b, n, s, c); }
void ls_status(const char *m) { if (verbose) fprintf(stderr, "  %s\n", m); }

static int hex8(const char *s, uint8_t out[8])
{
    if (strlen(s) != 16) return -1;
    for (int i = 0; i < 8; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct ls_opts o;
    struct ls_result r;
    size_t memsz = 256u << 20;
    void *mem = malloc(memsz);
    uint64_t t = (uint64_t)time(NULL);
    int rc;

    memset(&o, 0, sizeof o);
    o.root = NULL;
    o.now = (uint32_t)t;
    hex8("000A27002108AFDE", o.fwid);
    for (int i = 0; i < 8; i++) o.seed[i] = (uint8_t)(t >> (8 * i)) ^ (uint8_t)getpid();
    if (argc >= 3 && !strcmp(argv[1], "--verify")) {
        FILE *f = fopen(argv[2], "rb");
        long n;
        uint8_t *db;
        int marker = 1, npl = -1;
        char msg[96];
        if (!f) { perror(argv[2]); return 2; }
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        db = malloc(n);
        if (fread(db, 1, n, f) != (size_t)n) return 2;
        fclose(f);
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--nomarker")) marker = 0;
            else if (!strcmp(argv[i], "--npl") && i + 1 < argc) npl = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--fwid") && i + 1 < argc) { if (hex8(argv[++i], o.fwid)) return 2; }
        }
        msg[0] = 0;
        rc = ls_verify(db, (uint32_t)n, o.fwid, marker, 0, npl, mem, memsz, msg, sizeof msg);
        printf("%s %s (%ld bytes)\n", rc ? "BAD" : "ok", msg, n);
        return rc != 0;
    }
    if (getenv("LS_FAULT")) ls_fault = atoi(getenv("LS_FAULT"));
    o.album_cb = album_cb;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--full")) o.full = 1;
        else if (!strcmp(argv[i], "--albums")) show_albums = 1;
        else if (!strcmp(argv[i], "--dry")) o.dry = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--in") && i + 1 < argc) o.db_in = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) o.db_out = argv[++i];
        else if (!strcmp(argv[i], "--now") && i + 1 < argc) o.now = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) { if (hex8(argv[++i], o.seed)) return 2; }
        else if (!strcmp(argv[i], "--fwid") && i + 1 < argc) { if (hex8(argv[++i], o.fwid)) return 2; }
        else if (argv[i][0] != '-' && !o.root) o.root = argv[i];
        else { fprintf(stderr, "bad argument %s\n", argv[i]); return 2; }
    }
    if (!o.root || !mem) {
        fprintf(stderr, "usage: %s ROOT [--full] [--dry] [--in DB] [--out DB] [--now N] [--seed HEX] [--fwid HEX] [-v]\n", argv[0]);
        return 2;
    }
    rc = ls_sync(&o, mem, memsz, &r);
    printf("rc=%d tracks=%d flacs=%d kept=%d updated=%d added=%d removed=%d skipped=%d renamed=%d migrated=%d noalbum=%d verified=%d reordered=%d wrote=%d playcounts=%d playlists=%d%s%s\n",
           rc, r.tracks, r.flacs, r.kept, r.updated, r.added, r.removed, r.skipped, r.renamed, r.migrated, r.noalbum, r.verified, r.reordered,
           r.wrote, r.playcounts, r.playlists, r.msg[0] ? " msg=" : "", r.msg);
    return rc < 0;
}
