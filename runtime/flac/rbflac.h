/* rbflac: freestanding single-frame FLAC decoder.
 * Ported from Rockbox libffmpegFLAC (decoder.c, golomb.h, ffmpeg_get_bits.h,
 * arm.S).  Original code (c) 2003 Alex Beregszaszi, Michael Niedermayer
 * (LGPL 2.1+), ARM LPC asm (c) 2006 Thom Johansen (GPL 2+).
 *
 * No libc, no writable static data, no allocation.  Output is per-channel
 * int32 at the frame's native bit depth (no Rockbox output-depth shift).
 */
#ifndef RBFLAC_H
#define RBFLAC_H
#include <stdint.h>

#define RBFLAC_MAXCH  8     /* FLAC channel limit; all are decoded */
#define RBFLAC_PAD    16    /* bytes past buf+len that must be readable
                               (contents irrelevant, never used for output) */

/* error codes (all < 0) */
#define RBFLAC_ESYNC   (-1) /* no sync / bad header / header CRC8 / header does
                               not match stream params: not a frame here */
#define RBFLAC_ESHORT  (-2) /* header incomplete: need more data, or EOF */
#define RBFLAC_EDATA   (-3) /* header ok but subframe data invalid */
#define RBFLAC_EOVER   (-4) /* frame runs past buf+len (truncated/corrupt) */
#define RBFLAC_ECRC    (-5) /* CRC16 mismatch (only with RBFLAC_F_CRC16) */
#define RBFLAC_EPARAM  (-6) /* bad rbflac_stream / buffers */

#define RBFLAC_F_CRC16 1    /* verify the frame footer CRC16 */

typedef struct {
    int sr;          /* STREAMINFO sample rate                           */
    int ch;          /* STREAMINFO channels 1..8                         */
    int bps;         /* STREAMINFO bits per sample, 4..24 supported      */
    int maxbs;       /* STREAMINFO max block size, and capacity (samples)
                        of every out[] buffer; frames with bs > maxbs are
                        rejected (ESYNC), 16..65535                      */
    int flags;       /* RBFLAC_F_*                                       */
} rbflac_stream;

typedef struct {
    int blocksize;   /* samples per channel in this frame                */
    int chmode;      /* raw channel assignment code 0..10                */
    int nch;         /* channels decoded (== stream ch)                  */
    int bps;         /* sample depth (== stream bps)                     */
    int hdrlen;      /* frame header length in bytes incl. CRC8          */
    uint64_t sample; /* first sample number (frame# * maxbs for fixed-bs) */
} rbflac_frame;

/* Parse and validate only the frame header at buf (no subframe decode).
 * Returns hdrlen > 0, or RBFLAC_ESYNC / RBFLAC_ESHORT.  Same acceptance rule
 * as the old parse_fh() (sync, reserved bits, utf8, CRC8, nch/bps/sr equal to
 * the stream's, blocksize <= maxbs) except that it needs only the header
 * bytes (old: >= 16 bytes); ESHORT if they are not all there. Reads <= 16. */
int rbflac_parse_header(const rbflac_stream *st, const uint8_t *buf, int len,
                        rbflac_frame *fr);

/* Decode one frame starting at buf[0] (must be the sync code).
 * out[c] for c < st->ch: int32 buffers of st->maxbs samples each; stereo
 * decorrelation is applied, wasted bits restored.  For ch > 2 the extra
 * channels are decoded too (independent coding only, so out[2..ch-1] may all
 * alias one scratch buffer if the caller drops them).
 * buf must have RBFLAC_PAD readable bytes after buf+len.
 * Returns bytes consumed (> 0, header..CRC16 inclusive) or an error < 0.
 * On any error the caller should advance by 1 byte (or rescan for sync);
 * out[] contents are then undefined. */
int rbflac_peek_header(const uint8_t *p, int len, int *sr, int *nch, int *bps, int *bs, int *fixed);

int rbflac_decode_frame(const rbflac_stream *st, const uint8_t *buf, int len,
                        int32_t *const *out, rbflac_frame *fr);

#endif
