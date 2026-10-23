/* dither_test shaped|tpdf RATE in.q8 out.s16 (int32 stereo, 256 per output LSB): flac/nsdither.h on
 * the host, driven by dither_test.py. Build: clang -O2 -Iruntime/flac -o dither_test dither_test.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nsdither.h"

int main(int argc, char **argv)
{
    static NSQ q;
    FILE *fi, *fo;
    int32_t v[2];
    int16_t o[2];
    if (argc < 5 || !(fi = fopen(argv[3], "rb")) || !(fo = fopen(argv[4], "wb"))) {
        fprintf(stderr, "usage: dither_test shaped|tpdf RATE in.q8 out.s16\n");
        return 1;
    }
    nsq_reset(&q);
    nsq_rate(&q, atoi(argv[2]), !strcmp(argv[1], "shaped"));
    while (fread(v, 4, 2, fi) == 2) {
        o[0] = nsq_run(&q, 0, v[0]);
        o[1] = nsq_run(&q, 1, v[1]);
        fwrite(o, 2, 2, fo);
    }
    fclose(fo);
    return 0;
}
