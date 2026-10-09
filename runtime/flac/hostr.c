/* Host harness for the FLAC reader and decoder (flacblob.c). Drives them the way the OS player does.
 * hostr file.flac [seek_ms]   -> pcmhash of everything posted, stats
 * hostr file.flac seek_ms out.pcm dumps the posted PCM (16-bit LE interleaved). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef struct File File;
struct FileVT { void *(*dtor)(File*); void (*del)(File*); int (*open)(File*); int (*close)(File*);
  int (*read)(File*,void*,int,int); int (*seek)(File*,int64_t,int); int (*s6)(File*); int (*s7)(File*); int (*s8)(File*); };
struct File { const struct FileVT *vt; };
typedef struct { File f; unsigned char *d; long sz, pos; int open; char path[1024]; long reads, rbytes; } HF;
static void *h_dtor(File*f){return f;}
static void h_del(File*f){HF*h=(HF*)f; free(h->d); free(f);}
static int h_open(File*f){HF*h=(HF*)f; FILE*fp=fopen(h->path,"rb"); if(!fp) { perror(h->path); return 3; } static char vb[65536] __attribute__((aligned(4096))); setvbuf(fp,vb,_IOFBF,sizeof vb); fseek(fp,0,SEEK_END); h->sz=ftell(fp); fseek(fp,0,SEEK_SET); if(h->sz<=0){printf("ftell %ld\n",h->sz);return 3;} h->d=malloc(h->sz+64); if(!h->d||fread(h->d,1,h->sz,fp)!=(size_t)h->sz) { printf("fread fail\n"); return 3; } fclose(fp); h->pos=0; return 0;}
static int h_close(File*f){return 0;}
static int h_read(File*f,void*b,int n,int m){HF*h=(HF*)f; h->reads++; long r=h->sz-h->pos; if(r>n) r=n; if(r<0) r=0; memcpy(b,h->d+h->pos,r); { static int every=-1; if(every<0){char*e=getenv("CORRUPT"); every=e?atoi(e):0;} if(every && r>64 && h->reads%every==0){ ((unsigned char*)b)[(h->reads*7919)%r]^=0x10; } } h->pos+=r; h->rbytes+=r; return r;}
static int h_seek(File*f,int64_t o,int w){HF*h=(HF*)f; long p=w==0?o:w==1?h->pos+o:h->sz+o; if(p<0) return 3; h->pos=p; return 0;}
static int h_s6(File*f){return ((HF*)f)->sz;}
static int h_s7(File*f){return ((HF*)f)->pos;}
static int h_s8(File*f){return 0;}
static const struct FileVT hvt={h_dtor,h_del,h_open,h_close,h_read,h_seek,h_s6,h_s7,h_s8};
HF *last;
File *host_file_ctor(void *mem, const char *path, int a, int b){ HF*h=calloc(1,sizeof(HF)); free(mem); h->f.vt=&hvt; strcpy(h->path,path); last=h; return &h->f; }

/* the player stand-ins */
static uint8_t *pk_ptr[65536]; static int pk_len[65536]; static uint32_t pk_ms[65536]; static int pk_n, pk_i;
int host_src_get(void *trk, int *len, uint8_t **ptr, uint32_t *ms, uint32_t *fl)
{ (void)trk; if (pk_i >= pk_n) return 1; *len = pk_len[pk_i]; *ptr = pk_ptr[pk_i]; *ms = pk_ms[pk_i]; *fl = 0; pk_i++; return 0; }
static int released;
void host_src_rel(void *trk, int len, uint8_t *ptr, uint32_t ms) { (void)trk; (void)len; (void)ptr; (void)ms; released++; }
static uint8_t outbuf[2][16384]; static int outidx;
int host_out_get(void **buf, int keep) { (void)keep; *buf = outbuf[outidx]; outidx ^= 1; return 0; }
static uint64_t hh = 1469598103934665603ULL; static long posted_frames, posts, bpf = 4; static FILE *outf;
static uint32_t first_ms = 0xffffffff, last_ms, end_flags; static int end_posts;
void host_out_post(void *buf, int frames, uint32_t ms, int flags, void *trk)
{
    (void)trk; int n = frames * bpf, q;
    if (flags & 2) { end_posts++; end_flags = flags; return; }
    if (first_ms == 0xffffffff) first_ms = ms;
    last_ms = ms;
    for (q = 0; q < n; q++) { hh ^= ((uint8_t *)buf)[q]; hh *= 1099511628211ULL; }
    if (outf) fwrite(buf, 1, n, outf);
    posted_frames += frames; posts++;
}
static uint8_t track[0x1400]; void *host_cur_track(void) { return track; }

int host_fr_open(void *obj, const char *path); int host_fr_getfmt(void *obj, uint8_t *info); int host_fr_seek(void *obj, uint32_t ms);
int host_fr_read(void *obj, uint8_t *buf, int size, int flag, volatile uint8_t *abortp); int host_fr_close(void *obj);
int host_fd_init(const uint8_t *desc); int host_fd_next(void); int host_fd_pending(void); uint32_t *host_trace(void);

int main(int argc, char **argv)
{
    uint8_t *obj = calloc(1, 0x440), info[0x40], desc[0x40], *buf = malloc(0x80000);
    uint32_t seekms = argc > 2 ? (uint32_t)atol(argv[2]) : 0, rate, ch, bits;
    volatile uint8_t abort = 0;
    int rc, reads = 0, pk_total = 0, k, drained = 0;
    if (argc > 3) outf = fopen(argv[3], "wb");
    memset(info, 0, sizeof info); memset(desc, 0, sizeof desc);
    rc = host_fr_open(obj, argv[1]);
    if (rc) { uint32_t *t = host_trace(); printf("open fail %d tr24 %x\n", rc, t[24]); return 1; }
    host_fr_getfmt(obj, info);
    rate = *(uint32_t *)(info + 0x30); ch = *(uint32_t *)(info + 0x2c); bits = *(uint32_t *)(info + 0x10);
    printf("fmt dur %u ms codec %d bitrate %u bits %u ch %u rate %u fpp %u marker %02x%02x\n", *(uint32_t *)(info + 4), info[9],
           *(uint32_t *)(info + 0xc), bits, ch, rate, *(uint32_t *)(info + 0x34), info[0x14], info[0x15]);
    bpf = ch * 2;
    /* the desc the selector builds from info */
    desc[0] = info[9]; *(uint32_t *)(desc + 4) = rate; *(uint32_t *)(desc + 8) = bits; *(uint32_t *)(desc + 0xc) = ch;
    desc[0x10] = info[0x14]; desc[0x11] = info[0x15]; *(uint32_t *)(desc + 0x14) = *(uint32_t *)(info + 0xc);
    *(uint32_t *)(desc + 0x18) = *(uint32_t *)(info + 4); *(uint32_t *)(desc + 0x24) = *(uint32_t *)(info + 4);
    *(uint32_t *)(desc + 0x28) = *(uint32_t *)(info + 0x34);
    *(void **)(track + 0x107c) = obj;
    if (argc > 2) { rc = host_fr_seek(obj, seekms); printf("seek %u -> %d\n", seekms, rc); }
    rc = host_fd_init(desc);
    if (rc) { printf("init fail %d\n", rc); return 1; }
    for (;;) {
        uint8_t *E; uint32_t *trail; int n;
        rc = host_fr_read(obj, buf, 0x80000, 0, &abort);
        reads++;
        if (rc == 2 || rc == 3) { printf("read rc %d\n", rc); break; }
        E = buf + 0x80000; trail = (uint32_t *)E;
        n = (int)trail[-2];
        if (reads <= 2 || rc == 1) printf("read %d: rc %d packets %d first ms %u span %u ms\n", reads, rc, n, trail[-3], trail[-4]);
        pk_n = 0; pk_i = 0;
        for (k = 0; k < n; k++) { uint32_t *e = (uint32_t *)(E - 28 - 12 * k); pk_ptr[pk_n] = buf + e[0]; pk_len[pk_n] = (int)e[1]; pk_ms[pk_n] = e[2]; pk_n++; }
        pk_total += n;
        if (getenv("DBG") && reads == 1) for (k = 0; k < 3 && k < n; k++) { uint32_t *e = (uint32_t *)(E - 28 - 12 * k); printf("entry %d: off %u len %u ms %u\n", k, e[0], e[1], e[2]); }
        while (pk_i < pk_n || host_fd_pending()) { int r = host_fd_next(); if (r) { printf("decode_next %d\n", r); drained = 1; break; } }
        if (drained || rc == 1) break;
    }
    /* the end: the source has nothing more */
    pk_n = pk_i = 0;
    rc = host_fd_next();
    {
        uint32_t *t = host_trace();
        printf("pcmhash %08x%08x\n", (unsigned)(hh >> 32), (unsigned)hh);
        printf("reads %d packets %d posts %ld frames %ld first_ms %u last_ms %u end_posts %d end_flags %u released %d\n",
               reads, pk_total, posts, posted_frames, first_ms, last_ms, end_posts, end_flags, released);
        printf("file reads %ld bytes %ld | decoded frames %u failed %u lasterr %x skipped %u falsesync %u carried %u seekpts %u trunc %u\n",
               last->reads, last->rbytes, t[44], t[45] & 0xffff, t[45] >> 16, t[38], t[39], t[55], t[40] & 0xffff, t[40] >> 16);
    }
    host_fr_close(obj);
    if (outf) fclose(outf);
    return 0;
}
