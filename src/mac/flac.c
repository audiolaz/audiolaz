/* src/mac/flac.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#include <stdlib.h>
#include <string.h>
#include "FLAC/stream_decoder.h"

const char *const FLAC__StreamDecoderErrorStatusString[] = {
    "FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC", "FLAC__STREAM_DECODER_ERROR_STATUS_BAD_HEADER",
    "FLAC__STREAM_DECODER_ERROR_STATUS_FRAME_CRC_MISMATCH", "FLAC__STREAM_DECODER_ERROR_STATUS_UNPARSEABLE_STREAM",
    "FLAC__STREAM_DECODER_ERROR_STATUS_BAD_METADATA", "FLAC__STREAM_DECODER_ERROR_STATUS_OUT_OF_BOUNDS" };

enum { PAD = 16, CHUNK = 65536, MAXCH = 8 };

/* ROOM_FLAC_STATS (engine/mac/test_flac.c only): what the decoded streams used, counted (independent, left/side,
 * right/side, mid/side frames; CONSTANT, VERBATIM, FIXED 0–4, LPC 1–32 subframes; wasted bits; escaped partitions and
 * residuals by method). */
enum { S_CA = 0, S_CONST = 4, S_VERB, S_FIXED, S_LPC = S_FIXED + 5, S_WASTED = S_LPC + 32, S_ESC, S_RICE = S_ESC + 2, S_N = S_RICE + 2 };
#ifdef ROOM_FLAC_STATS
unsigned long long room_flac_stats[S_N];
#define ST(i) (room_flac_stats[i]++)
#else
#define ST(i) ((void)0)
#endif

struct FLAC__StreamDecoder {
    FLAC__StreamDecoderReadCallback rd;
    FLAC__StreamDecoderWriteCallback wr;
    FLAC__StreamDecoderMetadataCallback md;
    FLAC__StreamDecoderErrorCallback er;
    void *cd;
    int inited, started, eof, aborted, quiet;     /* quiet: no LOST_SYNC for the scan that follows a reported error */
    uint8_t *buf;
    size_t cap, len;                              /* bytes held, valid bytes (PAD more allocated past them) */
    size_t pos;                                   /* read position, in bits from buf */
    size_t keep;                                  /* the first byte still needed (the frame's start) */
    FLAC__StreamMetadata_StreamInfo si;
    int has_si;
    uint32_t blkcap;
    int64_t *s[MAXCH];
    int32_t *out[MAXCH], *res;
};
typedef FLAC__StreamDecoder D;

/* ── Input ─────────────────────────────────────────────────────────────────────────────────────────── */
/* At least nbits from pos held, reading only as much as that needs; 0 at the stream's end (or an abort) before then. */
static int fill(D *d, size_t nbits) {
    size_t want = (d->pos + nbits + 7) >> 3;
    while (d->len < want) {
        if (d->eof || d->aborted) return 0;
        size_t k = d->keep < (d->pos >> 3) ? d->keep : d->pos >> 3;
        if (k) {
            memmove(d->buf, d->buf + k, d->len - k);
            d->len -= k; d->pos -= 8 * k; d->keep -= k; want -= k;
        }
        if (d->cap < d->len + CHUNK + PAD) {
            size_t nc = d->cap ? d->cap : CHUNK + PAD;
            while (nc < d->len + CHUNK + PAD) nc *= 2;
            uint8_t *nb = realloc(d->buf, nc);
            if (!nb) { d->aborted = 1; return 0; }
            d->buf = nb; d->cap = nc;
        }
        size_t got = CHUNK;
        FLAC__StreamDecoderReadStatus st = d->rd(d, d->buf + d->len, &got, d->cd);
        if (got > CHUNK) got = CHUNK;
        if (st == FLAC__STREAM_DECODER_READ_STATUS_ABORT) { d->aborted = 1; return 0; }
        d->len += got;
        memset(d->buf + d->len, 0, PAD);
        if (st == FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM) d->eof = 1;
    }
    return 1;
}
static inline size_t avail(const D *d) { return 8 * d->len - d->pos; }
/* The 64 bits from pos (those past len meaningless): len + PAD bytes are always allocated. */
static inline uint64_t peek64(const D *d) {
    const uint8_t *p = d->buf + (d->pos >> 3);
    uint64_t w;
    memcpy(&w, p, 8);
    w = __builtin_bswap64(w);
    unsigned sh = d->pos & 7;
    if (sh) w = w << sh | p[8] >> (8 - sh);
    return w;
}
static inline int ubits(D *d, unsigned n, uint32_t *v) {        /* n ≤ 32 */
    if (!n) { *v = 0; return 1; }
    if (avail(d) < n && !fill(d, n)) return 0;
    *v = (uint32_t)(peek64(d) >> (64 - n));
    d->pos += n;
    return 1;
}
static inline int sbits(D *d, unsigned n, int64_t *v) {         /* n ≤ 33, two's complement */
    if (!n) { *v = 0; return 1; }
    if (avail(d) < n && !fill(d, n)) return 0;
    *v = (int64_t)peek64(d) >> (64 - n);
    d->pos += n;
    return 1;
}
static int unary(D *d, uint32_t *q) {                          /* zeros before a one */
    uint64_t c = 0;
    for (;;) {
        size_t a = avail(d);
        if (!a) { if (!fill(d, 1)) return 0; a = avail(d); }
        uint64_t w = peek64(d);
        unsigned lim = a < 64 ? (unsigned)a : 64;
        if (lim < 64) w &= ~0ull << (64 - lim);
        if (w) {
            unsigned z = (unsigned)__builtin_clzll(w);
            c += z; d->pos += z + 1;
            if (c > 0xffffffffu) c = 0xffffffffu;
            *q = (uint32_t)c;
            return 1;
        }
        c += lim; d->pos += lim;
    }
}
/* Whole bytes skipped (pos byte aligned); 0 at the stream's end. */
static int skip_bytes(D *d, uint64_t n) {
    while (n) {
        d->keep = d->pos >> 3;
        size_t a = d->len - (d->pos >> 3);
        if (!a) { if (!fill(d, 8)) return 0; continue; }
        size_t k = a < n ? a : (size_t)n;
        d->pos += 8 * k; n -= k;
    }
    d->keep = d->pos >> 3;
    return 1;
}
static int byte_at(D *d, size_t i, uint8_t *b) {              /* the byte i from pos's byte (pos byte aligned) */
    size_t p = (d->pos >> 3) + i;
    if (p >= d->len && !fill(d, 8 * (i + 1))) return 0;
    *b = d->buf[(d->pos >> 3) + i];
    return 1;
}

/* ── Checks ────────────────────────────────────────────────────────────────────────────────────────── */
static uint8_t crc8_t[256];
static uint16_t crc16_t[8][256];
static void crc_init(void) {
    static int done;
    if (done) return;
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++) c = (c << 1 ^ (c & 0x80 ? 0x07 : 0)) & 0xff;
        crc8_t[i] = (uint8_t)c;
        unsigned w = i << 8;
        for (int k = 0; k < 8; k++) w = (w << 1 ^ (w & 0x8000 ? 0x8005 : 0)) & 0xffff;
        crc16_t[0][i] = (uint16_t)w;
    }
    for (unsigned i = 0; i < 256; i++)                 /* slicing by 8 */
        for (int t = 1; t < 8; t++) crc16_t[t][i] = (uint16_t)(crc16_t[t - 1][i] << 8 ^ crc16_t[0][crc16_t[t - 1][i] >> 8]);
    done = 1;
}
static uint8_t crc8(const uint8_t *p, size_t n) {
    uint8_t c = 0;
    while (n--) c = crc8_t[c ^ *p++];
    return c;
}
static uint16_t crc16(const uint8_t *p, size_t n) {
    unsigned c = 0;
    while (n >= 8) {
        c = crc16_t[7][(c >> 8 ^ p[0]) & 0xff] ^ crc16_t[6][(c ^ p[1]) & 0xff] ^ crc16_t[5][p[2]] ^ crc16_t[4][p[3]] ^
            crc16_t[3][p[4]] ^ crc16_t[2][p[5]] ^ crc16_t[1][p[6]] ^ crc16_t[0][p[7]];
        p += 8; n -= 8;
    }
    while (n--) c = (c << 8 ^ crc16_t[0][(c >> 8 ^ *p++) & 0xff]) & 0xffff;
    return (uint16_t)c;
}

/* ── Subframes ─────────────────────────────────────────────────────────────────────────────────────── */
enum { OK = 1, END = 0, BAD = -1 };

/* The residual of n samples after a predictor of this order, into d->res[order…n). */
static int residual(D *d, unsigned n, unsigned order) {
    uint32_t v;
    if (!ubits(d, 6, &v)) return END;
    unsigned method = v >> 4, po = v & 15;
    if (method > 1) return BAD;
    const unsigned pbits = method ? 5 : 4, esc = method ? 31 : 15;
    ST(S_RICE + method);
    const unsigned parts = 1u << po, per = n >> po;
    if ((per << po) != n || per < order) return BAD;
    int32_t *r = d->res;
    unsigned i = order;
    for (unsigned p = 0; p < parts; p++) {
        const unsigned end = (p + 1) * per;
        uint32_t k;
        if (!ubits(d, pbits, &k)) return END;
        if (k == esc) {
            ST(S_ESC + method);
            uint32_t nb;
            if (!ubits(d, 5, &nb)) return END;
            for (; i < end; i++) { int64_t x; if (!sbits(d, nb, &x)) return END; r[i] = (int32_t)x; }
            continue;
        }
        for (; i < end; i++) {
            uint64_t u;
            if (avail(d) >= 64) {                     /* the common case: the whole code in the next 64 bits */
                uint64_t w = peek64(d);
                if (w) {
                    unsigned z = (unsigned)__builtin_clzll(w);
                    if (z + 1 + k <= 64) {
                        u = (uint64_t)z << k;
                        if (k) u |= (w << (z + 1)) >> (64 - k);
                        d->pos += z + 1 + k;
                        goto have;
                    }
                }
            }
            {
                uint32_t q, lo;
                if (!unary(d, &q) || !ubits(d, k, &lo)) return END;
                u = (uint64_t)q << k | lo;
            }
        have:
            if (u >> 32) return BAD;
            r[i] = (int32_t)((uint32_t)(u >> 1) ^ (0u - (uint32_t)(u & 1)));
        }
    }
    return OK;
}

static void fixed(int64_t *s, const int32_t *r, unsigned n, unsigned order) {
    unsigned i = order;
    switch (order) {
    case 0: for (; i < n; i++) s[i] = r[i]; break;
    case 1: for (; i < n; i++) s[i] = s[i - 1] + r[i]; break;
    case 2: for (; i < n; i++) s[i] = 2 * s[i - 1] - s[i - 2] + r[i]; break;
    case 3: for (; i < n; i++) s[i] = 3 * (s[i - 1] - s[i - 2]) + s[i - 3] + r[i]; break;
    case 4: for (; i < n; i++) s[i] = 4 * (s[i - 1] + s[i - 3]) - 6 * s[i - 2] - s[i - 4] + r[i]; break;
    }
}

static void lpc(int64_t *s, const int32_t *r, unsigned n, unsigned order, const int64_t *c, unsigned shift) {
    /* c[j] multiplies s[i-1-j] */
    if (order == 8) {
        for (unsigned i = 8; i < n; i++) {
            const int64_t *x = s + i;
            int64_t a = c[0] * x[-1] + c[1] * x[-2] + c[2] * x[-3] + c[3] * x[-4] + c[4] * x[-5] + c[5] * x[-6] + c[6] * x[-7] + c[7] * x[-8];
            s[i] = r[i] + (a >> shift);
        }
        return;
    }
    if (order == 12) {
        for (unsigned i = 12; i < n; i++) {
            const int64_t *x = s + i;
            int64_t a = c[0] * x[-1] + c[1] * x[-2] + c[2] * x[-3] + c[3] * x[-4] + c[4] * x[-5] + c[5] * x[-6] + c[6] * x[-7] + c[7] * x[-8]
                      + c[8] * x[-9] + c[9] * x[-10] + c[10] * x[-11] + c[11] * x[-12];
            s[i] = r[i] + (a >> shift);
        }
        return;
    }
    for (unsigned i = order; i < n; i++) {
        const int64_t *x = s + i;
        int64_t a = 0;
        for (unsigned j = 0; j < order; j++) a += c[j] * x[-1 - (int)j];
        s[i] = r[i] + (a >> shift);
    }
}

/* One subframe of n samples, bps bits each (a side channel's one more), into s. */
static int subframe(D *d, int64_t *s, unsigned n, unsigned bps) {
    uint32_t v;
    if (!ubits(d, 8, &v)) return END;
    if (v & 0x80) return BAD;
    const unsigned type = v >> 1 & 63;
    unsigned wasted = 0;
    if (v & 1) {
        uint32_t q;
        if (!unary(d, &q)) return END;
        if (q >= bps - 1) return BAD;
        wasted = q + 1;
        ST(S_WASTED);
        bps -= wasted;
    }
    int rc;
    if (type == 0) {                                    /* CONSTANT */
        ST(S_CONST);
        int64_t x;
        if (!sbits(d, bps, &x)) return END;
        for (unsigned i = 0; i < n; i++) s[i] = x;
    } else if (type == 1) {                             /* VERBATIM */
        ST(S_VERB);
        for (unsigned i = 0; i < n; i++) if (!sbits(d, bps, &s[i])) return END;
    } else if (type >= 8 && type <= 12) {               /* FIXED */
        const unsigned order = type - 8;
        ST(S_FIXED + order);
        if (order > n) return BAD;
        for (unsigned i = 0; i < order; i++) if (!sbits(d, bps, &s[i])) return END;
        if ((rc = residual(d, n, order)) != OK) return rc;
        fixed(s, d->res, n, order);
    } else if (type >= 32) {                            /* LPC */
        const unsigned order = type - 31;
        ST(S_LPC + order - 1);
        if (order > n) return BAD;
        for (unsigned i = 0; i < order; i++) if (!sbits(d, bps, &s[i])) return END;
        uint32_t prec;
        int64_t shift, c[32];
        if (!ubits(d, 4, &prec)) return END;
        if (prec == 15) return BAD;
        if (!sbits(d, 5, &shift)) return END;
        if (shift < 0) return BAD;
        for (unsigned j = 0; j < order; j++) if (!sbits(d, prec + 1, &c[j])) return END;
        if ((rc = residual(d, n, order)) != OK) return rc;
        lpc(s, d->res, n, order, c, (unsigned)shift);
    } else return BAD;
    if (wasted) for (unsigned i = 0; i < n; i++) s[i] = (int64_t)((uint64_t)s[i] << wasted);
    return OK;
}

/* ── Stream ────────────────────────────────────────────────────────────────────────────────────────── */
static void err(D *d, FLAC__StreamDecoderErrorStatus st) { d->er(d, st, d->cd); }

static int grow(D *d, uint32_t n) {
    if (n <= d->blkcap) return 1;
    for (int c = 0; c < MAXCH; c++) {
        free(d->s[c]); free(d->out[c]);
        d->s[c] = malloc(sizeof(int64_t) * n);
        d->out[c] = malloc(sizeof(int32_t) * n);
    }
    free(d->res);
    d->res = malloc(sizeof(int32_t) * n);
    int ok = d->res != NULL;
    for (int c = 0; c < MAXCH; c++) ok = ok && d->s[c] && d->out[c];
    d->blkcap = ok ? n : 0;
    return ok;
}

static int metadata(D *d) {
    for (;;) {
        uint32_t h, len;
        if (!ubits(d, 32, &h)) return END;
        const int last = h >> 31, type = h >> 24 & 127;
        len = h & 0xffffff;
        if (type == 127) { err(d, FLAC__STREAM_DECODER_ERROR_STATUS_BAD_METADATA); return OK; }
        if (type == 0 && len >= 34) {
            FLAC__StreamMetadata m;
            memset(&m, 0, sizeof m);
            FLAC__StreamMetadata_StreamInfo *si = &m.data.stream_info;
            uint32_t a, b, c;
            if (!ubits(d, 16, &si->min_blocksize) || !ubits(d, 16, &si->max_blocksize) || !ubits(d, 24, &si->min_framesize) ||
                !ubits(d, 24, &si->max_framesize) || !ubits(d, 20, &si->sample_rate) || !ubits(d, 3, &a) || !ubits(d, 5, &b) ||
                !ubits(d, 4, &c) || !ubits(d, 32, &h)) return END;
            si->channels = a + 1; si->bits_per_sample = b + 1;
            si->total_samples = (uint64_t)c << 32 | h;
            for (int i = 0; i < 16; i++) { if (!ubits(d, 8, &a)) return END; si->md5sum[i] = (FLAC__byte)a; }
            m.type = FLAC__METADATA_TYPE_STREAMINFO; m.is_last = last; m.length = len;
            d->si = *si; d->has_si = 1;
            if (d->md) d->md(d, &m, d->cd);
            len -= 34;
        }
        if (!skip_bytes(d, len)) return END;
        if (last) return OK;
    }
}

static int sync(D *d) {
    int lost = 0;
    for (;;) {
        d->keep = d->pos >> 3;
        uint8_t b0, b1, x[10];
        if (!byte_at(d, 0, &b0)) return 0;
        if (b0 == 0xff) {
            if (!byte_at(d, 1, &b1)) return 0;
            if ((b1 & 0xfe) == 0xf8) { d->started = 1; d->quiet = 0; return 1; }
        } else if (!d->started && (b0 == 'I' || b0 == 'f')) {
            int n = b0 == 'I' ? 10 : 4, have = 1;
            for (int i = 0; i < n && have; i++) have = byte_at(d, (size_t)i, &x[i]);
            if (have && b0 == 'I' && x[1] == 'D' && x[2] == '3' && !((x[6] | x[7] | x[8] | x[9]) & 0x80)) {
                uint64_t sz = (uint64_t)x[6] << 21 | x[7] << 14 | x[8] << 7 | x[9];
                if (x[5] & 0x10) sz += 10;                   /* a footer */
                if (!skip_bytes(d, 10 + sz)) return 0;
                continue;
            }
            if (have && b0 == 'f' && !memcmp(x, "fLaC", 4)) {
                d->pos += 32;
                d->started = 1;
                int r = metadata(d);
                if (r == END) return 0;
                d->pos = (d->pos + 7) & ~(size_t)7;
                continue;
            }
        }
        if (!lost && !d->quiet) err(d, FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC);
        lost = 1;
        d->pos += 8;
    }
}

/* One frame from pos (on a frame sync). OK: written (or the client aborted: d->aborted); END: the stream ended
 * inside it; BAD: not a frame (the error reported), looked for again from the next byte. */
static int frame(D *d) {
    d->keep = d->pos >> 3;                            /* the frame's first byte (fill moves it: never kept apart) */
    uint32_t v, a, b;
    FLAC__Frame f;
    memset(&f, 0, sizeof f);
    FLAC__FrameHeader *h = &f.header;
    if (!ubits(d, 16, &v) || !ubits(d, 8, &a) || !ubits(d, 8, &b)) return END;
    const int variable = v & 1;
    const unsigned bs = a >> 4, sr = a & 15, ca = b >> 4, ss = b >> 1 & 7;
    if (!bs || sr == 15 || ca > 10 || ss == 3 || (b & 1)) goto bad_header;
    /* the frame or sample number, UTF-8 style */
    uint32_t c0;
    if (!ubits(d, 8, &c0)) return END;
    uint64_t num;
    int more;
    if (!(c0 & 0x80)) { num = c0; more = 0; }
    else if ((c0 & 0xe0) == 0xc0) { num = c0 & 0x1f; more = 1; }
    else if ((c0 & 0xf0) == 0xe0) { num = c0 & 0x0f; more = 2; }
    else if ((c0 & 0xf8) == 0xf0) { num = c0 & 0x07; more = 3; }
    else if ((c0 & 0xfc) == 0xf8) { num = c0 & 0x03; more = 4; }
    else if ((c0 & 0xfe) == 0xfc) { num = c0 & 0x01; more = 5; }
    else if (c0 == 0xfe) { num = 0; more = 6; }
    else goto bad_header;
    if (!variable && more > 5) goto bad_header;
    for (int i = 0; i < more; i++) {
        uint32_t cb;
        if (!ubits(d, 8, &cb)) return END;
        if ((cb & 0xc0) != 0x80) goto bad_header;
        num = num << 6 | (cb & 0x3f);
    }
    uint32_t n;
    if (bs == 1) n = 192;
    else if (bs <= 5) n = 144u << bs;                 /* 576, 1152, 2304, 4608 */
    else if (bs == 6) { if (!ubits(d, 8, &n)) return END; n++; }
    else if (bs == 7) { if (!ubits(d, 16, &n)) return END; n++; }
    else n = 1u << bs;                                /* 256 … 32768 */
    static const uint32_t RATES[12] = { 0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000 };
    uint32_t rate;
    if (sr == 0) { if (!d->has_si) goto bad_header; rate = d->si.sample_rate; }
    else if (sr < 12) rate = RATES[sr];
    else if (sr == 12) { if (!ubits(d, 8, &rate)) return END; rate *= 1000; }
    else if (sr == 13) { if (!ubits(d, 16, &rate)) return END; }
    else { if (!ubits(d, 16, &rate)) return END; rate *= 10; }
    static const unsigned SIZES[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };
    unsigned bps;
    if (ss == 0) { if (!d->has_si) goto bad_header; bps = d->si.bits_per_sample; }
    else bps = SIZES[ss];
    uint32_t crc;
    if (!ubits(d, 8, &crc)) return END;
    if (crc8(d->buf + d->keep, (d->pos >> 3) - 1 - d->keep) != crc) goto bad_header;
    if (n > 65535 || bps < 4 || bps > 32 || !rate) goto bad_header;
    const unsigned nch = ca < 8 ? ca + 1 : 2;
    h->blocksize = n; h->sample_rate = rate; h->channels = nch; h->bits_per_sample = bps; h->crc = (uint8_t)crc;
    h->channel_assignment = ca < 8 ? 0 : ca - 7;      /* independent, left/side, right/side, mid/side */
    h->number_type = 1;                               /* a sample number, as libFLAC gives it */
    if (variable) h->number.sample_number = num;
    else h->number.sample_number = num * (d->has_si && d->si.min_blocksize == d->si.max_blocksize && d->si.max_blocksize ? d->si.max_blocksize : n);
    if (!grow(d, n)) { d->aborted = 1; return OK; }
    /* the subframes */
    for (unsigned c = 0; c < nch; c++) {
        unsigned sb = bps + ((ca == 8 && c == 1) || (ca == 9 && c == 0) || (ca == 10 && c == 1));
        int r = subframe(d, d->s[c], n, sb);
        if (r == END) return END;
        if (r == BAD) { err(d, FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC); goto again; }
    }
    d->pos = (d->pos + 7) & ~(size_t)7;
    if (!ubits(d, 16, &crc)) return END;
    const int good = crc16(d->buf + d->keep, (d->pos >> 3) - 2 - d->keep) == crc;
    d->keep = d->pos >> 3;
    if (!good) {
        err(d, FLAC__STREAM_DECODER_ERROR_STATUS_FRAME_CRC_MISMATCH);
        for (unsigned c = 0; c < nch; c++) memset(d->out[c], 0, sizeof(int32_t) * n);
    } else {
        const int64_t *s0 = d->s[0], *s1 = d->s[1];
        int32_t *o0 = d->out[0], *o1 = d->out[1];
        ST(ca < 8 ? 0 : ca - 7);
        switch (ca) {
        case 8:  for (uint32_t i = 0; i < n; i++) { o0[i] = (int32_t)s0[i]; o1[i] = (int32_t)(s0[i] - s1[i]); } break;
        case 9:  for (uint32_t i = 0; i < n; i++) { o0[i] = (int32_t)(s0[i] + s1[i]); o1[i] = (int32_t)s1[i]; } break;
        case 10: for (uint32_t i = 0; i < n; i++) {
                     int64_t m = (int64_t)((uint64_t)s0[i] << 1) | (s1[i] & 1);
                     o0[i] = (int32_t)((m + s1[i]) >> 1); o1[i] = (int32_t)((m - s1[i]) >> 1);
                 } break;
        default: for (unsigned c = 0; c < nch; c++) for (uint32_t i = 0; i < n; i++) d->out[c][i] = (int32_t)d->s[c][i];
        }
    }
    if (d->wr(d, &f, (const FLAC__int32 *const *)d->out, d->cd) != FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE) d->aborted = 1;
    return OK;
bad_header:
    err(d, FLAC__STREAM_DECODER_ERROR_STATUS_BAD_HEADER);
again:
    d->quiet = 1;
    d->pos = 8 * (d->keep + 1);
    d->keep++;
    return BAD;
}

/* ── The API ───────────────────────────────────────────────────────────────────────────────────────── */
FLAC__StreamDecoder *FLAC__stream_decoder_new(void) {
    crc_init();
    return calloc(1, sizeof(D));
}
FLAC__StreamDecoderInitStatus FLAC__stream_decoder_init_stream(FLAC__StreamDecoder *d, FLAC__StreamDecoderReadCallback rd,
                                                               FLAC__StreamDecoderSeekCallback seek, FLAC__StreamDecoderTellCallback tell,
                                                               FLAC__StreamDecoderLengthCallback length, FLAC__StreamDecoderEofCallback eof,
                                                               FLAC__StreamDecoderWriteCallback wr, FLAC__StreamDecoderMetadataCallback md,
                                                               FLAC__StreamDecoderErrorCallback er, void *cd) {
    if (d->inited) return FLAC__STREAM_DECODER_INIT_STATUS_ALREADY_INITIALIZED;
    if (!rd || !wr || !er || ((seek || tell || length || eof) && !(seek && tell && length && eof)))
        return FLAC__STREAM_DECODER_INIT_STATUS_INVALID_CALLBACKS;
    d->rd = rd; d->wr = wr; d->md = md; d->er = er; d->cd = cd;
    d->started = d->eof = d->aborted = d->quiet = d->has_si = 0;
    d->len = d->pos = d->keep = 0;
    if (!d->buf) {
        d->buf = malloc(CHUNK + PAD);
        if (!d->buf) return FLAC__STREAM_DECODER_INIT_STATUS_MEMORY_ALLOCATION_ERROR;
        d->cap = CHUNK + PAD;
    }
    memset(d->buf, 0, PAD);
    d->inited = 1;
    return FLAC__STREAM_DECODER_INIT_STATUS_OK;
}
/* true at the stream's end (a frame cut short there dropped, as libFLAC); false when the read or write callback aborted. */
FLAC__bool FLAC__stream_decoder_process_until_end_of_stream(FLAC__StreamDecoder *d) {
    if (!d->inited) return 0;
    while (!d->aborted) {
        if (!sync(d)) break;
        if (frame(d) == END) break;
    }
    return !d->aborted;
}
FLAC__bool FLAC__stream_decoder_finish(FLAC__StreamDecoder *d) {
    d->inited = 0;
    d->len = d->pos = d->keep = 0;
    return 1;
}
void FLAC__stream_decoder_delete(FLAC__StreamDecoder *d) {
    if (!d) return;
    free(d->buf);
    for (int c = 0; c < MAXCH; c++) { free(d->s[c]); free(d->out[c]); }
    free(d->res);
    free(d);
}
