/* Host driver for runtime/fatdir.c: lists folders of a FAT32 image (a volume image, or a whole
 * disk image with an MBR).   fatdir_host IMAGE SECTORSIZE PATH...   -> name\tD|F\tsize\tmtime */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
int fatdir_host_open(const char *image, uint32_t ss);
int fatdir_begin(void);
void fatdir_end(void);
int fatdir_list(const char *path, int (*cb)(void *, const char *, int, uint32_t, uint16_t, uint16_t), void *ctx);
extern int fatdir_reads;
/* Copy of dos_mktime from runtime/osfile.c: FAT local date and time as if UTC, in seconds since 1970. */
static uint32_t dos_mktime(uint16_t date, uint16_t time)
{
    static const uint16_t cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    uint32_t y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31, days;
    if (m < 1) m = 1;
    if (m > 12) m = 12;
    days = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400 + cum[m - 1] + (d ? d - 1 : 0);
    if (m > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
    return days * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
}
static int cb(void *ctx, const char *name, int is_dir, uint32_t size, uint16_t date, uint16_t time)
{
    (void)ctx;
    printf("%s\t%c\t%u\t%u\n", name, is_dir ? 'D' : 'F', size, dos_mktime(date, time));
    return 0;
}
int main(int argc, char **argv)
{
    int i, rc = 0;
    if (argc < 4 || fatdir_host_open(argv[1], (uint32_t)strtoul(argv[2], 0, 0))) { fprintf(stderr, "usage: fatdir_host IMAGE SECTORSIZE PATH...\n"); return 2; }
    if (fatdir_begin()) { fprintf(stderr, "fatdir_begin failed\n"); return 1; }
    for (i = 3; i < argc; i++) {
        int n = fatdir_list(argv[i], cb, 0);
        printf("== %s: %d entries, reads so far %d\n", argv[i], n, fatdir_reads);
        if (n < 0) rc = 1;
    }
    fatdir_end();
    return rc;
}
