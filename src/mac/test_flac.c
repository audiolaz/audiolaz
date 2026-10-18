/* Tests of engine/mac/flac.c , run by test/mac-flac.sh: FLAC files decoded through libFLAC's API as squeeze.c
 * calls it (reads of random sizes, some of one byte), the decoded samples checked bit-exactly against the MD5 of the
 * original PCM the encoder put in STREAMINFO, or against another file's decoding.
 *   test_flac md5 F            decoded = the STREAMINFO MD5, as many samples as it says, no error
 *   test_flac cmp MODE A B     B against A: same (identical), prefix (B a truncated A: its first samples, fewer),
 *                              suffix (B begins inside A: A's last samples, fewer), crc (as many, one frame's worth
 *                              silenced at most, a CRC error said), gap (one frame's worth missing at most, an error said)
 *   test_flac abort F          a write callback's ABORT ends the decoding (false, no more frames), as a read's ABORT
 *   test_flac open F           a stream left open after its last frame: every sample written before more is asked for
 *   test_flac speed F [s]      decoding from memory, times real time
 *   test_flac raw F OUT        the samples, interleaved, at the top of 32 bits (as squeeze.c queues them), little endian
 * cc -O2 -DROOM_FLAC_BUILTIN -Iengine/mac engine/mac/test_flac.c engine/mac/flac.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define COMMON_DIGEST_FOR_OPENSSL
#include <CommonCrypto/CommonDigest.h>
#include "FLAC/stream_decoder.h"

typedef struct {
    const uint8_t *data; size_t len, at;
    uint32_t rng; int random_reads, open_end, abort_write_at, abort_read_at;
    unsigned ch, bps, rate, maxbs; uint64_t frames, n, reads;
    int32_t *pcm; size_t cap;                     /* interleaved */
    int errs[6];
    FLAC__StreamMetadata_StreamInfo si; int has_si;
    int open_ok;                                  /* open: what was written when more was asked past the end */
} run;

static uint8_t *slurp(const char *p, size_t *len) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); *len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len + 1);
    if (fread(b, 1, *len, f) != *len) { perror(p); exit(2); }
    fclose(f);
    return b;
}
static uint32_t rnd(run *r) { r->rng = r->rng * 1664525u + 1013904223u; return r->rng >> 8; }

static FLAC__StreamDecoderReadStatus rd(const FLAC__StreamDecoder *d, FLAC__byte *buf, size_t *bytes, void *cd) {
    (void)d;
    run *r = cd;
    r->reads++;
    if (r->abort_read_at && r->reads >= (uint64_t)r->abort_read_at) { *bytes = 0; return FLAC__STREAM_DECODER_READ_STATUS_ABORT; }
    if (r->at >= r->len) {
        *bytes = 0;
        if (r->open_end) {                        /* the stream stays open: nothing more ever comes */
            r->open_ok = r->has_si && r->n == r->si.total_samples;
            return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        }
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    size_t want = *bytes;
    if (r->random_reads) {
        uint32_t x = rnd(r) % 100;
        size_t k = x < 10 ? 1 : x < 40 ? 1 + rnd(r) % 64 : 1 + rnd(r) % 20000;
        if (k < want) want = k;
    }
    if (want > r->len - r->at) want = r->len - r->at;
    memcpy(buf, r->data + r->at, want);
    r->at += want;
    *bytes = want;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}
static FLAC__StreamDecoderWriteStatus wr(const FLAC__StreamDecoder *d, const FLAC__Frame *f, const FLAC__int32 *const b[], void *cd) {
    (void)d;
    run *r = cd;
    const unsigned n = f->header.blocksize, ch = f->header.channels;
    if (r->abort_write_at && r->frames >= (uint64_t)r->abort_write_at) { r->frames++; return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; }
    if (!r->ch) { r->ch = ch; r->bps = f->header.bits_per_sample; r->rate = f->header.sample_rate; }
    if (ch != r->ch || f->header.bits_per_sample != r->bps || f->header.sample_rate != r->rate) {
        fprintf(stderr, "format change in frame %llu\n", (unsigned long long)r->frames); exit(1);
    }
    if (n > r->maxbs) r->maxbs = n;
    if ((r->n + n) * ch > r->cap) { r->cap = 2 * (r->n + n) * ch; r->pcm = realloc(r->pcm, sizeof(int32_t) * r->cap); }
    for (unsigned i = 0; i < n; i++) for (unsigned c = 0; c < ch; c++) r->pcm[(r->n + i) * ch + c] = b[c][i];
    r->n += n; r->frames++;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void md(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *cd) {
    (void)d;
    run *r = cd;
    if (m->type == FLAC__METADATA_TYPE_STREAMINFO) { r->si = m->data.stream_info; r->has_si = 1; }
}
static void er(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus s, void *cd) {
    (void)d;
    run *r = cd;
    r->errs[s < 6 ? s : 5]++;
}
static int decode(run *r) {
    FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
    if (!d || FLAC__stream_decoder_init_stream(d, rd, NULL, NULL, NULL, NULL, wr, md, er, r) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        fprintf(stderr, "init failed\n"); exit(2);
    }
    int ok = FLAC__stream_decoder_process_until_end_of_stream(d);
    FLAC__stream_decoder_finish(d);
    FLAC__stream_decoder_delete(d);
    return ok;
}
static void load(run *r, const char *p, int random_reads) {
    memset(r, 0, sizeof *r);
    r->data = slurp(p, &r->len);
    r->random_reads = random_reads;
    r->rng = 12345;
    for (const char *s = p; *s; s++) r->rng = r->rng * 31 + (uint8_t)*s;
}
static int nerr(const run *r) { int t = 0; for (int i = 0; i < 6; i++) t += r->errs[i]; return t; }
static void summary(const char *tag, const char *p, const run *r) {
    printf("%s %s: %u ch, %u bits, %u Hz, %llu samples, %llu frames (max %u), errors lost %d header %d crc %d other %d\n", tag, p,
           r->ch, r->bps, r->rate, (unsigned long long)r->n, (unsigned long long)r->frames, r->maxbs, r->errs[0], r->errs[1],
           r->errs[2], r->errs[3] + r->errs[4] + r->errs[5]);
}

static int md5_check(const char *p) {
    run r;
    load(&r, p, 1);
    int ok = decode(&r);
    summary(ok ? "decoded" : "FAILED", p, &r);
    if (!ok || !r.has_si) { printf("FAIL %s: %s\n", p, ok ? "no STREAMINFO" : "decoder returned false"); return 1; }
    if (nerr(&r)) { printf("FAIL %s: errors reported\n", p); return 1; }
    if (r.si.total_samples && r.n != r.si.total_samples) { printf("FAIL %s: %llu samples, STREAMINFO says %llu\n", p, (unsigned long long)r.n, (unsigned long long)r.si.total_samples); return 1; }
    if (r.si.bits_per_sample != r.bps || r.si.channels != r.ch || r.si.sample_rate != r.rate) { printf("FAIL %s: frames and STREAMINFO differ\n", p); return 1; }
    static const uint8_t zero[16];
    if (!memcmp(r.si.md5sum, zero, 16)) { printf("FAIL %s: no MD5 in STREAMINFO\n", p); return 1; }
    const unsigned bytes = (r.bps + 7) / 8;
    uint8_t *pcm = malloc(r.n * r.ch * bytes + 1);
    size_t k = 0;
    for (uint64_t i = 0; i < r.n * r.ch; i++) { uint32_t v = (uint32_t)r.pcm[i]; for (unsigned b = 0; b < bytes; b++) pcm[k++] = (uint8_t)(v >> (8 * b)); }
    uint8_t dig[16];
    CC_MD5(pcm, (CC_LONG)k, dig);
    if (memcmp(dig, r.si.md5sum, 16)) { printf("FAIL %s: MD5 differs\n", p); return 1; }
    printf("ok   %s: bit-exact (MD5)\n", p);
    return 0;
}

static int cmp(const char *mode, const char *pa, const char *pb) {
    run a, b;
    load(&a, pa, 0); load(&b, pb, 1);
    int oka = decode(&a), okb = decode(&b);
    summary("base", pa, &a); summary("test", pb, &b);
    if (!oka || nerr(&a)) { printf("FAIL %s: the base does not decode cleanly\n", pa); return 1; }
    if (!okb) { printf("FAIL %s: decoder returned false\n", pb); return 1; }
    const unsigned ch = a.ch;
    if (b.ch != ch || b.bps != a.bps) { printf("FAIL %s: format differs\n", pb); return 1; }
    const size_t fa = a.n, fb = b.n, sz = sizeof(int32_t) * ch;
    const int32_t *A = a.pcm, *B = b.pcm;
    int fail = 0;
    if (!strcmp(mode, "same")) fail = fa != fb || memcmp(A, B, fa * sz) || nerr(&b);
    else if (!strcmp(mode, "prefix")) fail = fb >= fa || !fb || memcmp(A, B, fb * sz) || nerr(&b);
    else if (!strcmp(mode, "suffix")) fail = fb >= fa || !fb || memcmp(A + (fa - fb) * ch, B, fb * sz);
    else if (!strcmp(mode, "crc") || !strcmp(mode, "gap")) {
        size_t lo = 0, hi = 0;                    /* the common start, the common end */
        const size_t m = fa < fb ? fa : fb;
        while (lo < m && !memcmp(A + lo * ch, B + lo * ch, sz)) lo++;
        while (hi < m - lo && !memcmp(A + (fa - 1 - hi) * ch, B + (fb - 1 - hi) * ch, sz)) hi++;
        const size_t da = fa - lo - hi, db = fb - lo - hi;
        printf("     differs: %zu samples of the base, %zu of the test, from %zu\n", da, db, lo);
        if (!strcmp(mode, "crc")) {
            fail = fa != fb || !b.errs[FLAC__STREAM_DECODER_ERROR_STATUS_FRAME_CRC_MISMATCH] || da > a.maxbs;
            for (size_t i = lo * ch; !fail && i < (lo + db) * ch; i++) fail = B[i] != 0;   /* silence there */
        } else fail = fb >= fa || db || da > a.maxbs || !nerr(&b);
    } else { fprintf(stderr, "unknown mode %s\n", mode); return 2; }
    printf("%s %s %s\n", fail ? "FAIL" : "ok  ", mode, pb);
    return fail;
}

static int aborts(const char *p) {
    run r;
    load(&r, p, 1);
    r.abort_write_at = 3;
    int ok = decode(&r);
    int fail = ok || r.frames != 4;
    printf("%s write ABORT: returned %d after %llu calls\n", fail ? "FAIL" : "ok  ", ok, (unsigned long long)r.frames);
    load(&r, p, 1);
    r.abort_read_at = 5;
    ok = decode(&r);
    printf("%s read ABORT: returned %d after %llu reads\n", ok ? "FAIL" : "ok  ", ok, (unsigned long long)r.reads);
    return fail || ok;
}

static int open_end(const char *p) {
    run r;
    load(&r, p, 1);
    r.open_end = 1;
    decode(&r);
    printf("%s open stream: %llu of %llu samples written before more was asked past the end\n", r.open_ok ? "ok  " : "FAIL",
           (unsigned long long)r.n, (unsigned long long)r.si.total_samples);
    return !r.open_ok;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int speed(const char *p, double min_x) {
    run r;
    load(&r, p, 0);
    decode(&r);
    const uint64_t n = r.n;
    const unsigned rate = r.rate;
    double best = 1e9;
    for (int k = 0; k < 5; k++) {
        r.n = 0; r.frames = 0; r.at = 0;
        double t0 = now();
        decode(&r);
        double t = now() - t0;
        if (t < best) best = t;
    }
    double x = (double)n / rate / best;
    printf("%s speed %s: %.1f s of %u Hz %u-bit %u ch decoded in %.3f s: %.0f× real time\n", x >= min_x ? "ok  " : "FAIL", p,
           (double)n / rate, rate, r.bps, r.ch, best, x);
    return x < min_x;
}

static int raw(const char *p, const char *o) {
    run r;
    load(&r, p, 1);
    if (!decode(&r)) { printf("FAIL %s: decoder returned false\n", p); return 1; }
    FILE *f = fopen(o, "wb");
    if (!f) { perror(o); return 2; }
    for (uint64_t i = 0; i < r.n * r.ch; i++) { uint32_t v = (uint32_t)r.pcm[i] << (32 - r.bps); fwrite(&v, 4, 1, f); }
    fclose(f);
    return nerr(&r) != 0;
}

#ifdef ROOM_FLAC_STATS
/* What the md5 files used, all together: every kind of frame, subframe and residual must have been met. */
extern unsigned long long room_flac_stats[];
static int coverage(void) {
    static const char *const CA[4] = { "independent", "left/side", "right/side", "mid/side" };
    const unsigned long long *s = room_flac_stats;
    int miss = 0, lpc = 0;
    printf("     frames:");
    for (int i = 0; i < 4; i++) { printf(" %s %llu", CA[i], s[i]); miss |= !s[i]; }
    printf("\n     subframes: CONSTANT %llu, VERBATIM %llu, FIXED", s[4], s[5]);
    miss |= !s[4] || !s[5];
    for (int o = 0; o < 5; o++) { printf(" %d:%llu", o, s[6 + o]); miss |= !s[6 + o]; }
    printf(", LPC");
    for (int o = 0; o < 32; o++) { lpc += s[11 + o] > 0; printf(" %d:%llu", o + 1, s[11 + o]); }
    miss |= lpc != 32;
    printf("\n     wasted bits %llu, escaped partitions %llu (Rice) %llu (Rice2), residuals Rice %llu, Rice2 %llu\n",
           s[43], s[44], s[45], s[46], s[47]);
    for (int i = 43; i < 48; i++) miss |= !s[i];
    printf("%s coverage: all frame, subframe and residual kinds met (LPC orders: %d of 32)\n", miss ? "FAIL" : "ok  ", lpc);
    return miss;
}
#else
static int coverage(void) { puts("(coverage: build with -DROOM_FLAC_STATS)"); return 0; }
#endif

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "md5")) { int f = 0; for (int i = 2; i < argc; i++) f |= md5_check(argv[i]); return f; }
    if (argc >= 3 && !strcmp(argv[1], "cover")) { int f = 0; for (int i = 2; i < argc; i++) f |= md5_check(argv[i]); return f | coverage(); }
    if (argc == 5 && !strcmp(argv[1], "cmp")) return cmp(argv[2], argv[3], argv[4]);
    if (argc == 3 && !strcmp(argv[1], "abort")) return aborts(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "open")) return open_end(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "raw")) return raw(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "speed")) return speed(argv[2], argc > 3 ? atof(argv[3]) : 50);
    fprintf(stderr, "usage: see the head of engine/mac/test_flac.c\n");
    return 2;
}
