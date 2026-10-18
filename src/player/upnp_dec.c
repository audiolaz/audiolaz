/* src/player/upnp_dec.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <FLAC/stream_decoder.h>
#include "upnp.h"

#if defined(ROOM_PORTABLE) && !defined(ROOM_FLAC_BUILTIN)   /* as squeeze.c: libFLAC opened at run time */
#include <dlfcn.h>
static struct {
    FLAC__StreamDecoder *(*dnew)(void);
    FLAC__StreamDecoderInitStatus (*init)(FLAC__StreamDecoder *, FLAC__StreamDecoderReadCallback, FLAC__StreamDecoderSeekCallback,
                                          FLAC__StreamDecoderTellCallback, FLAC__StreamDecoderLengthCallback, FLAC__StreamDecoderEofCallback,
                                          FLAC__StreamDecoderWriteCallback, FLAC__StreamDecoderMetadataCallback,
                                          FLAC__StreamDecoderErrorCallback, void *);
    FLAC__bool (*run)(FLAC__StreamDecoder *), (*finish)(FLAC__StreamDecoder *);
    void (*del)(FLAC__StreamDecoder *);
} flac;
static int have_flac;
static void flac_load(void) {
    static const char *const LIBS[] = { "libFLAC.so.14", "libFLAC.so.12", "libFLAC.so.8" };
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS && !have_flac; i++) {
        void *h = dlopen(LIBS[i], RTLD_NOW | RTLD_LOCAL);
        if (!h) continue;
        *(void **)&flac.dnew = dlsym(h, "FLAC__stream_decoder_new");
        *(void **)&flac.init = dlsym(h, "FLAC__stream_decoder_init_stream");
        *(void **)&flac.run = dlsym(h, "FLAC__stream_decoder_process_until_end_of_stream");
        *(void **)&flac.finish = dlsym(h, "FLAC__stream_decoder_finish");
        *(void **)&flac.del = dlsym(h, "FLAC__stream_decoder_delete");
        have_flac = flac.dnew && flac.init && flac.run && flac.finish && flac.del;
        if (!have_flac) dlclose(h);
    }
    if (!have_flac) ulog("UPnP: no libFLAC (.so.14, .12 or .8): FLAC refused, WAV, AIFF and LPCM only");
}
#define FLAC__stream_decoder_new flac.dnew
#define FLAC__stream_decoder_init_stream flac.init
#define FLAC__stream_decoder_process_until_end_of_stream flac.run
#define FLAC__stream_decoder_finish flac.finish
#define FLAC__stream_decoder_delete flac.del
#else
static const int have_flac = 1;
static void flac_load(void) { }
#endif

/* ── HTTP ─────────────────────────────────────────────────────────────────────────────────────────────────────── */
#define H_NODATA_S 20.0                /* nothing from the server this long: the track lost */
typedef struct {
    int fd, status, chunked, eof;
    long long left;                    /* Content-Length's bytes still to come (−1: to the connection's end) */
    long long chunk;                   /* chunked: bytes left in this chunk (−1: its size line next) */
    long long clen;                    /* the whole resource's length when known (Content-Range, else Content-Length), −1 */
    long long from;                    /* where this response starts in the resource (206), 0 */
    char ctype[200];
    unsigned char buf[65536];
    size_t pos, len;
    unsigned gen;                      /* aborted as soon as the queue's generation moves on (0: never) */
} hconn;

static int parse_url(const char *url, char *host, size_t hl, char *port, size_t pl, const char **path) {
    if (strncasecmp(url, "http://", 7)) return -1;
    const char *h = url + 7, *e;
    if (*h == '[') { e = strchr(h, ']'); if (!e) return -1; h++; }
    else { e = h; while (*e && *e != ':' && *e != '/' && *e != '?') e++; }
    if ((size_t)(e - h) >= hl || e == h) return -1;
    memcpy(host, h, (size_t)(e - h)); host[e - h] = 0;
    if (*e == ']') e++;
    snprintf(port, pl, "80");
    if (*e == ':') { const char *p = ++e; while (*e >= '0' && *e <= '9') e++; if (e == p || (size_t)(e - p) >= pl) return -1; memcpy(port, p, (size_t)(e - p)); port[e - p] = 0; }
    *path = *e == '/' ? e : *e == '?' ? e : "/";
    return 0;
}
static int tcp_connect(const char *host, const char *port, int ms) {
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) || !res) return -1;
    int s = -1;
    for (struct addrinfo *a = res; a && s < 0; a = a->ai_next) {
        s = socket(a->ai_family, SOCK_STREAM, 0);
        if (s < 0) continue;
        fcntl(s, F_SETFL, O_NONBLOCK);
        if (connect(s, a->ai_addr, a->ai_addrlen) && errno != EINPROGRESS) { close(s); s = -1; continue; }
        struct pollfd p = { s, POLLOUT, 0 };
        int err = 0;
        socklen_t el = sizeof err;
        if (poll(&p, 1, ms) != 1 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) || err) { close(s); s = -1; continue; }
        fcntl(s, F_SETFL, 0);
    }
    freeaddrinfo(res);
    if (s >= 0) {
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    }
    return s;
}
/* Raw bytes from the socket into buf (what is there); 0 at its end, −1 lost or aborted. */
static int h_fill(hconn *h) {
    if (h->pos < h->len) return 1;
    h->pos = h->len = 0;
    const double t0 = umono();
    for (;;) {
        if (h->gen && uq_gen() != h->gen) return -1;
        struct pollfd p = { h->fd, POLLIN, 0 };
        const int r = poll(&p, 1, 300);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -1;
        if (r == 0) { if (umono() - t0 > H_NODATA_S) return -1; continue; }
        const ssize_t k = recv(h->fd, h->buf, sizeof h->buf, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) return -1;
        if (k == 0) return 0;
        h->len = (size_t)k;
        return 1;
    }
}
/* A line of the head or of the chunked encoding (CRLF taken off). */
static int h_line(hconn *h, char *out, size_t len) {
    size_t w = 0;
    for (;;) {
        const int f = h_fill(h);
        if (f <= 0) return -1;
        const unsigned char c = h->buf[h->pos++];
        if (c == '\n') { while (w && out[w - 1] == '\r') w--; out[w] = 0; return 0; }
        if (w + 1 < len) out[w++] = (char)c;
    }
}
static void hc_close(hconn *h) { if (h->fd >= 0) close(h->fd); h->fd = -1; }
/* GET url from byte `from` (≥ 0: a Range). 0: the body follows (h->status 200 or 206), −1. */
static int hc_open(hconn *h, const char *url0, long long from, unsigned gen) {
    char url[2048];
    snprintf(url, sizeof url, "%s", url0);
    memset(h, 0, offsetof(hconn, buf));
    h->fd = -1; h->gen = gen;
    for (int hop = 0; hop < 6; hop++) {
        char host[256], port[16], req[3072];
        const char *path;
        if (parse_url(url, host, sizeof host, port, sizeof port, &path)) { ulog("UPnP: %s: not an http:// address", url); return -1; }
        h->fd = tcp_connect(host, port, 5000);
        if (h->fd < 0) { ulog("UPnP: %s:%s unreachable", host, port); return -1; }
        struct timeval tv = { 10, 0 };
        setsockopt(h->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        char range[64] = "";
        if (from > 0) snprintf(range, sizeof range, "Range: bytes=%lld-\r\n", from);
        const int hn = strchr(host, ':') ? snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: [%s]:%s\r\n", path, host, port)
                                         : snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%s\r\n", path, host, port);
        const int n = hn + snprintf(req + hn, sizeof req - (size_t)hn, "User-Agent: audiolaz-player UPnP/1.0 DLNADOC/1.50\r\nAccept: */*\r\n"
                                    "transferMode.dlna.org: Streaming\r\n%sConnection: close\r\n\r\n", range);
        if (send(h->fd, req, (size_t)n, 0) != n) { hc_close(h); return -1; }
        char line[2048], loc[2048] = "";
        h->pos = h->len = 0;
        if (h_line(h, line, sizeof line) || sscanf(line, "HTTP/%*s %d", &h->status) != 1) { ulog("UPnP: %s: no HTTP answer", url); hc_close(h); return -1; }
        h->left = -1; h->clen = -1; h->from = 0; h->chunked = 0; h->ctype[0] = 0;
        for (;;) {
            if (h_line(h, line, sizeof line)) { hc_close(h); return -1; }
            if (!line[0]) break;
            const char *v = strchr(line, ':');
            if (!v) continue;
            v++; while (*v == ' ' || *v == '\t') v++;
            if (!strncasecmp(line, "Content-Length:", 15)) h->left = atoll(v);
            else if (!strncasecmp(line, "Transfer-Encoding:", 18) && strcasestr(v, "chunked")) h->chunked = 1;
            else if (!strncasecmp(line, "Content-Type:", 13)) snprintf(h->ctype, sizeof h->ctype, "%s", v);
            else if (!strncasecmp(line, "Location:", 9)) snprintf(loc, sizeof loc, "%s", v);
            else if (!strncasecmp(line, "Content-Range:", 14)) {
                long long a = 0, b = 0, t = -1;
                if (sscanf(v, "bytes %lld-%lld/%lld", &a, &b, &t) >= 2) { h->from = a; if (t > 0) h->clen = t; }
            }
        }
        if (h->status >= 300 && h->status < 400 && loc[0]) {
            hc_close(h);
            if (loc[0] == '/') { char base[512]; const char *p2; char hh[256], pp[16];
                if (!parse_url(url, hh, sizeof hh, pp, sizeof pp, &p2)) { snprintf(base, sizeof base, "http://%s:%s", hh, pp); snprintf(url, sizeof url, "%s%s", base, loc); } }
            else snprintf(url, sizeof url, "%s", loc);
            continue;
        }
        if (h->status != 200 && h->status != 206) { ulog("UPnP: %s: HTTP %d", url, h->status); hc_close(h); return -1; }
        if (h->status == 200) { h->from = 0; if (h->left >= 0 && !h->chunked) h->clen = h->left; }
        if (h->chunked) { h->left = -1; h->chunk = -1; }
        return 0;
    }
    ulog("UPnP: %s: too many redirections", url0);
    return -1;
}
/* Up to n bytes of the body: how many, 0 at its end, −1 lost or aborted. */
static long hc_read(hconn *h, void *dst, size_t n) {
    if (h->eof || !n) return 0;
    if (h->chunked) {
        if (h->chunk == 0) { char l[64]; if (h_line(h, l, sizeof l)) return -1; h->chunk = -1; }   /* the CRLF after a chunk */
        if (h->chunk < 0) {
            char l[256];
            if (h_line(h, l, sizeof l)) return -1;
            h->chunk = strtoll(l, NULL, 16);
            if (h->chunk <= 0) { h->eof = 1; return 0; }
        }
        if ((long long)n > h->chunk) n = (size_t)h->chunk;
    } else if (h->left >= 0) {
        if (h->left == 0) { h->eof = 1; return 0; }
        if ((long long)n > h->left) n = (size_t)h->left;
    }
    const int f = h_fill(h);
    if (f < 0) return -1;
    if (f == 0) {                                        /* the server closed it: its end, even before the length it said
                                                          * (some control points announce a length they never reach) */
        h->eof = 1;
        if (h->left > 0 || h->chunked) ulog("UPnP: the stream closed %s: taken as the track's end", h->chunked ? "inside a chunk" : "before its announced length");
        return 0;
    }
    size_t k = h->len - h->pos;
    if (k > n) k = n;
    memcpy(dst, h->buf + h->pos, k);
    h->pos += k;
    if (h->chunked) h->chunk -= (long long)k;
    else if (h->left >= 0) h->left -= (long long)k;
    return (long)k;
}
/* Exactly n bytes (1), the end before (0), lost (−1). */
static int hc_full(hconn *h, void *dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        const long k = hc_read(h, (unsigned char *)dst + got, n - got);
        if (k < 0) return -1;
        if (k == 0) return 0;
        got += (size_t)k;
    }
    return 1;
}
static int hc_skip(hconn *h, long long n) {
    unsigned char b[4096];
    while (n > 0) { const int r = hc_full(h, b, n > (long long)sizeof b ? sizeof b : (size_t)n); if (r <= 0) return r; n -= n > (long long)sizeof b ? (long long)sizeof b : n; }
    return 1;
}

int ud_fetch(const char *url, unsigned char **out, size_t *len, size_t max) {
    hconn *h = malloc(sizeof *h);
    if (!h) return -1;
    *out = NULL; *len = 0;
    if (hc_open(h, url, 0, 0)) { free(h); return -1; }
    size_t cap = 65536, n = 0;
    unsigned char *b = malloc(cap);
    const double t0 = umono();
    while (b && umono() - t0 < 10) {
        if (n == cap) { if (cap >= max) break; cap *= 2; unsigned char *nb = realloc(b, cap); if (!nb) break; b = nb; }
        const long k = hc_read(h, b + n, cap - n);
        if (k <= 0) break;
        n += (size_t)k;
    }
    hc_close(h); free(h);
    if (!b || !n || n > max) { free(b); return -1; }
    *out = b; *len = n;
    return 0;
}

/* ── What was opened last (a seek to the same track goes straight to its place) ──────────────────────────────────── */
enum { K_NONE, K_FLAC, K_WAV, K_AIFF, K_LPCM };
typedef struct {
    char uri[2048];
    int kind;
    unsigned rate, ch, bits, bpf;      /* bpf: bytes a frame (PCM kinds) */
    int be, flt, u8;                   /* big-endian samples, 32-bit float, unsigned 8-bit */
    long long data_off, data_len;      /* the PCM's place in the resource and its bytes (−1 unknown) */
    long long clen;                    /* the resource's bytes (−1) */
    uint64_t total;                    /* frames in the track (0 unknown) */
} tinfo;
static tinfo TI;
static pthread_mutex_t tim = PTHREAD_MUTEX_INITIALIZER;
static char fmt_now[96] = "";
static int dur_seq = -1;
static double dur_s = 0;
void ud_format(char *out, size_t len) { pthread_mutex_lock(&tim); snprintf(out, len, "%s", fmt_now); pthread_mutex_unlock(&tim); }
double ud_duration(int seq) { pthread_mutex_lock(&tim); const double d = seq == dur_seq ? dur_s : 0; pthread_mutex_unlock(&tim); return d; }
static void say_format(const ud_job *j, const char *kind, unsigned rate, unsigned bits, unsigned ch, uint64_t total) {
    pthread_mutex_lock(&tim);
    snprintf(fmt_now, sizeof fmt_now, "%s %.1f kHz %u bits%s", kind, rate / 1000.0, bits, ch == 1 ? " mono" : ch > 2 ? " multichannel" : "");
    if (total && rate) { dur_seq = j->seq; dur_s = (double)total / rate; }
    pthread_mutex_unlock(&tim);
    if (total && rate) upnp_duration(j->seq, (double)total / rate);
}

/* ── Frames to the queue ──────────────────────────────────────────────────────────────────────────────────────── */
#define EMIT_MAX 4096
typedef struct {
    const ud_job *j;
    unsigned rate, ch;                 /* the source's channels (1: doubled) */
    uint64_t pos;                      /* the next frame's place in the track */
    uint64_t skip_to;                  /* a seek: frames before it dropped */
    double skip_s;                     /* … or its time, the rate not known yet */
    int seg;                           /* a segment begun at this rate and these channels */
    unsigned seg_rate, seg_ch;
    int dop_run, dop, dop_said, dop_last;
    int32_t out[EMIT_MAX * 8];
    const char *why;                   /* a refusal, for the control point */
} dctx;
/* n frames, ch interleaved words each (the source's bits at the top): 0, −1 (another generation now). */
static int emit(dctx *d, const int32_t *in, long n) {
    const unsigned oc = d->ch == 1 ? 2 : d->ch;
    if (d->skip_s > 0 && d->rate) { d->skip_to = (uint64_t)llround(d->skip_s * d->rate); d->skip_s = 0; }
    while (n > 0) {
        long k = n > EMIT_MAX ? EMIT_MAX : n;
        if (d->pos + (uint64_t)k <= d->skip_to) { d->pos += (uint64_t)k; in += (size_t)k * d->ch; n -= k; continue; }
        long drop = d->pos < d->skip_to ? (long)(d->skip_to - d->pos) : 0;
        if (!d->seg || d->seg_rate != d->rate || d->seg_ch != oc) {
            if (uq_segment(d->j->gen, d->j->seq, d->rate, oc, d->pos + (uint64_t)drop)) return -1;
            d->seg = 1; d->seg_rate = d->rate; d->seg_ch = oc;
        }
        long m = 0;
        for (long i = drop; i < k; i++, m++) {
            const int32_t *f = in + (size_t)i * d->ch;
            int32_t *o = d->out + (size_t)m * oc;
            if (d->ch == 1) { o[0] = f[0]; o[1] = f[0]; }
            else memcpy(o, f, sizeof(int32_t) * oc);
            /* DoP: both sides' top byte 0x05 / 0xFA, alternating */
            const unsigned t0 = ((uint32_t)o[0] >> 24) & 0xFF, t1 = ((uint32_t)o[1] >> 24) & 0xFF;
            const int mk = (t0 == 0x05 || t0 == 0xFA) && t0 == t1 && (int)t0 != d->dop_last;
            d->dop_last = (int)t0;
            if (mk) { if (++d->dop_run >= 64 && !d->dop) { d->dop = 1; if (!d->dop_said++) ulog("UPnP: DoP (DSD in PCM) in this track: not played (silence), DSD is refused here; let the control point convert it to PCM"); } }
            else { d->dop_run = 0; d->dop = 0; }
            if (d->dop) memset(o, 0, sizeof(int32_t) * oc);
        }
        d->pos += (uint64_t)k;
        if (m && uq_push(d->j->gen, d->out, m)) return -1;
        in += (size_t)k * d->ch;
        n -= k;
    }
    return 0;
}

/* ── FLAC ─────────────────────────────────────────────────────────────────────────────────────────────────────── */
typedef struct { dctx *d; hconn *h; int bad; uint64_t total; unsigned bps; int32_t *il; size_t il_cap; long long got; int first; const unsigned char *pre; size_t pn; } fctx;
static FLAC__StreamDecoderReadStatus f_read(const FLAC__StreamDecoder *dec, FLAC__byte buf[], size_t *bytes, void *cd) {
    (void)dec;
    fctx *f = cd;
    if (f->pn && *bytes) {                               /* the bytes the format was told by, first */
        const size_t k = f->pn < *bytes ? f->pn : *bytes;
        memcpy(buf, f->pre, k); f->pre += k; f->pn -= k; *bytes = k;
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }
    const long k = hc_read(f->h, buf, *bytes);
    if (k > 0) { *bytes = (size_t)k; f->got += k; return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE; }
    *bytes = 0;
    if (k < 0) { f->bad = 1; return FLAC__STREAM_DECODER_READ_STATUS_ABORT; }
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
}
static FLAC__StreamDecoderWriteStatus f_write(const FLAC__StreamDecoder *dec, const FLAC__Frame *fr, const FLAC__int32 *const b[], void *cd) {
    (void)dec;
    fctx *f = cd;
    dctx *d = f->d;
    const unsigned n = fr->header.blocksize, ch = fr->header.channels, bps = fr->header.bits_per_sample ? fr->header.bits_per_sample : f->bps;
    if (!n || !ch || ch > 8 || !bps || bps > 32 || !fr->header.sample_rate) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    if (fr->header.number_type == 1) d->pos = fr->header.number.sample_number;   /* the frame's own place (a seek lands anywhere) */
    if (f->first) {
        f->first = 0;
        say_format(d->j, "FLAC", fr->header.sample_rate, bps, ch, f->total);
        pthread_mutex_lock(&tim);                         /* known at once: a seek in it goes straight to its place */
        if (strcmp(TI.uri, d->j->uri)) { memset(&TI, 0, sizeof TI); snprintf(TI.uri, sizeof TI.uri, "%s", d->j->uri); TI.clen = f->h->clen; TI.data_off = -1; }
        TI.kind = K_FLAC; TI.rate = fr->header.sample_rate; TI.ch = ch; if (f->total) TI.total = f->total;
        pthread_mutex_unlock(&tim);
        ulog("UPnP: FLAC %u Hz, %u bits, %u channel(s)%s", fr->header.sample_rate, bps, ch, d->skip_to ? ", from a seek" : "");
    }
    d->rate = fr->header.sample_rate; d->ch = ch;
    if (f->il_cap < (size_t)n * ch) { free(f->il); f->il_cap = (size_t)n * ch; f->il = malloc(sizeof(int32_t) * f->il_cap); if (!f->il) { f->il_cap = 0; return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; } }
    const int sh = 32 - (int)bps;
    for (unsigned i = 0; i < n; i++) for (unsigned c = 0; c < ch; c++) f->il[(size_t)i * ch + c] = (int32_t)((uint32_t)b[c][i] << sh);
    return emit(d, f->il, (long)n) ? FLAC__STREAM_DECODER_WRITE_STATUS_ABORT : FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void f_meta(const FLAC__StreamDecoder *dec, const FLAC__StreamMetadata *m, void *cd) {
    (void)dec;
    fctx *f = cd;
    if (m->type == FLAC__METADATA_TYPE_STREAMINFO) { f->total = m->data.stream_info.total_samples; f->bps = m->data.stream_info.bits_per_sample; }
}
static void f_error(const FLAC__StreamDecoder *dec, FLAC__StreamDecoderErrorStatus s, void *cd) { (void)dec; (void)s; (void)cd; }
/* 1 played to its end, 0 aborted (another generation), −1 failed. */
static int dec_flac(dctx *d, hconn *h, const unsigned char *pre, size_t pn) {
    if (!have_flac) { d->why = "FLAC: no decoder (libFLAC) on this machine"; return -1; }
    fctx f = { d, h, 0, 0, 0, NULL, 0, 0, 1, pre, pn };
    FLAC__StreamDecoder *dec = FLAC__stream_decoder_new();
    int ok = 0;
    if (dec && FLAC__stream_decoder_init_stream(dec, f_read, NULL, NULL, NULL, NULL, f_write, f_meta, f_error, &f) == FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        ok = FLAC__stream_decoder_process_until_end_of_stream(dec);
        FLAC__stream_decoder_finish(dec);
    }
    if (dec) FLAC__stream_decoder_delete(dec);
    free(f.il);
    if (uq_gen() != d->j->gen) return 0;
    if (f.first) { d->why = "FLAC: nothing decoded"; return -1; }
    pthread_mutex_lock(&tim);
    if (strcmp(TI.uri, d->j->uri)) { memset(&TI, 0, sizeof TI); snprintf(TI.uri, sizeof TI.uri, "%s", d->j->uri); TI.clen = h->clen; TI.data_off = -1; }
    TI.kind = K_FLAC; TI.rate = d->rate; TI.ch = d->ch; TI.total = f.total;
    pthread_mutex_unlock(&tim);
    return ok || !f.bad ? 1 : -1;
}

/* ── PCM: WAV, AIFF, LPCM ─────────────────────────────────────────────────────────────────────────────────────── */
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const unsigned char *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static double ext80(const unsigned char *p) {            /* AIFF's 80-bit extended sample rate */
    const int e = ((p[0] & 0x7F) << 8 | p[1]) - 16383;
    uint64_t m = 0;
    for (int i = 0; i < 8; i++) m = m << 8 | p[2 + i];
    return ldexp((double)m, e - 63);
}
/* The PCM from where h is: frames until data_len bytes (−1: to the end). 1, 0, −1. */
static int pcm_run(dctx *d, hconn *h, const tinfo *t, long long left) {
    const unsigned bpf = t->bpf, bps = bpf / t->ch;
    unsigned char raw[EMIT_MAX * 32];
    int32_t pcm[EMIT_MAX * 8];
    const size_t maxf = sizeof raw / bpf < EMIT_MAX ? sizeof raw / bpf : EMIT_MAX;
    size_t have = 0;
    for (;;) {
        size_t want = maxf * bpf;
        if (left >= 0 && (long long)want > left + (long long)have) want = (size_t)(left + (long long)have);
        if (want <= have) break;
        const long k = hc_read(h, raw + have, want - have);
        if (k < 0) return uq_gen() != d->j->gen ? 0 : -1;
        if (k == 0) break;
        if (left >= 0) left -= k;
        have += (size_t)k;
        const size_t nf = have / bpf;
        if (!nf) continue;
        for (size_t i = 0; i < nf * t->ch; i++) {
            const unsigned char *s = raw + i * bps;
            int32_t v;
            if (t->flt) { float x; uint32_t u = t->be ? be32(s) : le32(s); memcpy(&x, &u, 4);
                          const double y = (double)x * 2147483648.0; v = y >= 2147483647.0 ? INT32_MAX : y <= -2147483648.0 ? INT32_MIN : (int32_t)lrint(y); }
            else if (bps == 1) v = (int32_t)((uint32_t)(t->u8 ? (uint8_t)(s[0] ^ 0x80) : s[0]) << 24);
            else if (bps == 2) v = (int32_t)((uint32_t)(t->be ? be16(s) : le16(s)) << 16);
            else if (bps == 3) v = (int32_t)(t->be ? ((uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8) : ((uint32_t)s[2] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[0] << 8));
            else v = (int32_t)(t->be ? be32(s) : le32(s));
            pcm[i] = v;
        }
        if (emit(d, pcm, (long)nf)) return 0;
        memmove(raw, raw + nf * bpf, have - nf * bpf);
        have -= nf * bpf;
    }
    return uq_gen() != d->j->gen ? 0 : 1;
}
static void remember(const ud_job *j, const tinfo *t) {
    pthread_mutex_lock(&tim);
    TI = *t; snprintf(TI.uri, sizeof TI.uri, "%s", j->uri);
    pthread_mutex_unlock(&tim);
}
static int pcm_start(dctx *d, hconn *h, tinfo *t, const char *kind, long long data_len, long long at) {
    if (!t->ch || t->ch > 8 || !t->rate || t->rate > 768000 || !t->bpf || t->bpf % t->ch) { d->why = "PCM: a format not handled"; return -1; }
    const unsigned bits = t->bits ? t->bits : 8 * (t->bpf / t->ch);
    t->total = data_len > 0 ? (uint64_t)(data_len / t->bpf) : 0;
    t->data_len = data_len;
    remember(d->j, t);
    say_format(d->j, kind, t->rate, t->flt ? 32 : bits, t->ch, t->total);
    ulog("UPnP: %s %u Hz, %u bits%s, %u channel(s)%s", kind, t->rate, bits, t->flt ? " float" : "", t->ch, d->skip_to ? ", from a seek" : "");
    d->rate = t->rate; d->ch = t->ch;
    if (d->skip_s > 0) { d->skip_to = (uint64_t)llround(d->skip_s * t->rate); d->skip_s = 0; }
    /* where in the music we are: the seek's place when the server took the Range (at), the start otherwise */
    d->pos = at;
    if (d->skip_to > d->pos && h->status == 200) {      /* no Range: the bytes before it skipped here */
        const long long sk = (long long)(d->skip_to - d->pos) * t->bpf;
        if (hc_skip(h, sk) <= 0) return uq_gen() != d->j->gen ? 0 : -1;
        d->pos = d->skip_to;
        if (data_len > 0) data_len -= sk;
    }
    return pcm_run(d, h, t, data_len > 0 ? data_len : -1);
}
static int dec_wav(dctx *d, hconn *h, unsigned char *hd) {
    tinfo t = { .kind = K_WAV, .clen = h->clen };
    long long off = 12;
    const int rf64 = !memcmp(hd, "RF64", 4);
    long long ds64_data = -1;
    for (;;) {
        unsigned char ck[8];
        if (hc_full(h, ck, 8) <= 0) { d->why = "WAV: no data chunk"; return uq_gen() != d->j->gen ? 0 : -1; }
        off += 8;
        long long sz = le32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char f[64] = { 0 };
            const size_t rd = sz < (long long)sizeof f ? (size_t)sz : sizeof f;
            if (hc_full(h, f, rd) <= 0 || (sz > (long long)rd && hc_skip(h, sz - (long long)rd) <= 0)) return -1;
            unsigned tag = le16(f);
            t.ch = le16(f + 2); t.rate = le32(f + 4); t.bpf = le16(f + 12); t.bits = le16(f + 14);
            if (tag == 0xFFFE && rd >= 26) tag = le16(f + 24);   /* WAVE_FORMAT_EXTENSIBLE: its sub-format */
            if (tag == 3 && t.bits == 32) t.flt = 1;
            else if (tag != 1) { d->why = "WAV: compressed (only PCM and 32-bit float)"; return -1; }
            if (t.bits <= 8) t.u8 = 1;
            /* the container (block align) wins: 24 valid bits in 32-bit words (EXTENSIBLE) are left-justified, read whole */
            if (!(t.ch && t.bpf % t.ch == 0 && t.bpf / t.ch >= 1 && t.bpf / t.ch <= 4) && t.bits && t.bits <= 32) t.bpf = t.ch * ((t.bits + 7) / 8);
            if (t.flt && t.bpf != t.ch * 4) { d->why = "WAV: float other than 32 bits"; return -1; }
            off += sz + (sz & 1);
            if (sz & 1) hc_skip(h, 1);
        } else if (!memcmp(ck, "ds64", 4) && rf64) {
            unsigned char f[28];
            const size_t rd = sz < 28 ? (size_t)sz : 28;
            if (hc_full(h, f, rd) <= 0) return -1;
            if (rd >= 20) ds64_data = (long long)le32(f + 8) | (long long)le32(f + 12) << 32;
            if (sz > (long long)rd) hc_skip(h, sz - (long long)rd);
            off += sz;
        } else if (!memcmp(ck, "data", 4)) {
            if (!t.bpf) { d->why = "WAV: no fmt chunk"; return -1; }
            if (rf64 && ds64_data >= 0) sz = ds64_data;
            if (sz == 0 || sz == 0xFFFFFFFFLL) sz = -1;          /* a stream: to its end */
            t.data_off = off;
            return pcm_start(d, h, &t, "WAV", sz, 0);
        } else {
            if (hc_skip(h, sz + (sz & 1)) <= 0) { d->why = "WAV: cut short"; return uq_gen() != d->j->gen ? 0 : -1; }
            off += sz + (sz & 1);
        }
    }
}
static int dec_aiff(dctx *d, hconn *h, unsigned char *hd) {
    tinfo t = { .kind = K_AIFF, .clen = h->clen, .be = 1 };
    const int aifc = !memcmp(hd + 8, "AIFC", 4);
    long long off = 12;
    for (;;) {
        unsigned char ck[8];
        if (hc_full(h, ck, 8) <= 0) { d->why = "AIFF: no sound"; return uq_gen() != d->j->gen ? 0 : -1; }
        off += 8;
        long long sz = be32(ck + 4);
        if (!memcmp(ck, "COMM", 4)) {
            unsigned char f[64] = { 0 };
            const size_t rd = sz < (long long)sizeof f ? (size_t)sz : sizeof f;
            if (hc_full(h, f, rd) <= 0 || (sz > (long long)rd && hc_skip(h, sz - (long long)rd) <= 0)) return -1;
            t.ch = be16(f); t.bits = be16(f + 6); t.rate = (unsigned)lround(ext80(f + 8));
            t.bpf = t.ch * ((t.bits + 7) / 8);
            if (aifc && rd >= 22) {
                if (!memcmp(f + 18, "sowt", 4)) t.be = 0;
                else if (!memcmp(f + 18, "fl32", 4) || !memcmp(f + 18, "FL32", 4)) { t.flt = 1; t.bits = 32; t.bpf = t.ch * 4; }
                else if (memcmp(f + 18, "NONE", 4) && memcmp(f + 18, "twos", 4)) { d->why = "AIFC: compressed"; return -1; }
            }
            off += sz + (sz & 1);
            if (sz & 1) hc_skip(h, 1);
        } else if (!memcmp(ck, "SSND", 4)) {
            unsigned char f[8];
            if (!t.bpf || hc_full(h, f, 8) <= 0) { d->why = "AIFF: no COMM chunk"; return -1; }
            const long long o = be32(f);
            if (o && hc_skip(h, o) <= 0) return -1;
            t.data_off = off + 8 + o;
            return pcm_start(d, h, &t, aifc ? "AIFC" : "AIFF", sz - 8 - o > 0 ? sz - 8 - o : -1, 0);
        } else {
            if (hc_skip(h, sz + (sz & 1)) <= 0) { d->why = "AIFF: cut short"; return uq_gen() != d->j->gen ? 0 : -1; }
            off += sz + (sz & 1);
        }
    }
}
/* audio/L16;rate=48000;channels=2 → its rate and channels (DLNA's defaults when not said) */
static int lpcm_type(const char *m, tinfo *t) {
    const char *l = strcasestr(m, "audio/L");
    if (!l) return 0;
    const int bits = atoi(l + 7);
    if (bits != 16 && bits != 24 && bits != 8 && bits != 32) return 0;
    t->kind = K_LPCM; t->be = 1; t->bits = (unsigned)bits; t->rate = 44100; t->ch = 2;
    const char *r = strcasestr(l, "rate="), *c = strcasestr(l, "channels=");
    if (r) t->rate = (unsigned)atoi(r + 5);
    if (c) t->ch = (unsigned)atoi(c + 9);
    t->bpf = t->ch * (unsigned)(bits / 8);
    return 1;
}

/* ── One track ────────────────────────────────────────────────────────────────────────────────────────────────── */
static int play_job(const ud_job *j, char *why, size_t wl) {
    dctx *d = calloc(1, sizeof *d);
    hconn *h = malloc(sizeof *h);
    if (!d || !h) { free(d); free(h); snprintf(why, wl, "no memory"); return -1; }
    d->j = j;
    int r = -1;
    tinfo t;
    pthread_mutex_lock(&tim);
    t = TI;
    pthread_mutex_unlock(&tim);
    const int same = !strcmp(t.uri, j->uri) && t.kind != K_NONE;
    uint64_t target = 0;
    long long from = 0;
    if (j->seek_s > 0) {
        if (same && t.rate) target = (uint64_t)llround(j->seek_s * t.rate);
        if (same && (t.kind == K_WAV || t.kind == K_AIFF || t.kind == K_LPCM) && t.data_off >= 0) {
            if (t.total && target >= t.total) target = t.total ? t.total - 1 : 0;
            from = t.data_off + (long long)target * t.bpf;
        } else if (same && t.kind == K_FLAC && t.total && t.clen > 0) {
            /* the byte where that time should be, a little before it (the frames say where they are: the rest dropped) */
            const double fr = (double)target / (double)t.total;
            from = (long long)(fr * (double)t.clen) - 262144;
            if (from < 0) from = 0;
        }
        d->skip_to = target;
        if (!target) d->skip_s = j->seek_s;
    }
    if (hc_open(h, j->uri, from, j->gen)) { snprintf(why, wl, "the track could not be fetched"); goto out; }
    if (from > 0 && h->status == 206 && same && t.kind != K_FLAC) {   /* the PCM at once, its place known */
        d->rate = t.rate; d->ch = t.ch;
        tinfo t2 = t;
        long long left = t.data_len > 0 ? t.data_len - (from - t.data_off) : -1;
        say_format(j, t.kind == K_WAV ? "WAV" : t.kind == K_AIFF ? "AIFF" : "LPCM", t.rate, t.flt ? 32 : t.bits, t.ch, t.total);
        ulog("UPnP: seek to %.1f s (byte %lld)", j->seek_s, from);
        d->pos = target;
        r = pcm_run(d, h, &t2, left);
        goto out;
    }
    if (d->skip_to && !same) ulog("UPnP: seek to %.1f s: the track read from its start, the music before it dropped", j->seek_s);
    unsigned char hd[12];
    /* the first bytes say what it is (an ID3v2 tag skipped: FLAC may have one) */
    if (from > 0 && h->status == 206 && t.kind == K_FLAC) { ulog("UPnP: seek to %.1f s (FLAC, from byte %lld)", j->seek_s, from); r = dec_flac(d, h, NULL, 0); goto out; }
    tinfo lp = { 0 };
    if (lpcm_type(h->ctype, &lp) || lpcm_type(j->mime, &lp)) {
        lp.clen = h->clen; lp.data_off = 0;
        r = pcm_start(d, h, &lp, lp.bits == 24 ? "LPCM L24" : "LPCM L16", h->clen > 0 ? h->clen : -1, 0);
        goto out;
    }
    const int g = hc_full(h, hd, 4);
    if (g <= 0) { snprintf(why, wl, "an empty track"); r = g < 0 && uq_gen() != j->gen ? 0 : -1; goto out; }
    if (!memcmp(hd, "ID3", 3)) {                         /* ID3v2: its size, synchsafe */
        unsigned char x[6];
        if (hc_full(h, x, 6) <= 0) goto out;
        const long long sz = (long long)(x[2] & 0x7F) << 21 | (x[3] & 0x7F) << 14 | (x[4] & 0x7F) << 7 | (x[5] & 0x7F);
        if (hc_skip(h, sz + ((x[1] & 0x10) ? 10 : 0)) <= 0 || hc_full(h, hd, 4) <= 0) goto out;
        if (!memcmp(hd, "fLaC", 4)) { r = dec_flac(d, h, hd, 4); goto out; }
        snprintf(why, wl, "MP3 is not played here (FLAC, WAV, AIFF, LPCM)");
        goto out;
    }
    if (!memcmp(hd, "fLaC", 4)) { r = dec_flac(d, h, hd, 4); goto out; }
    if (hc_full(h, hd + 4, 8) <= 0) { snprintf(why, wl, "a track too short"); goto out; }
    if ((!memcmp(hd, "RIFF", 4) || !memcmp(hd, "RF64", 4)) && !memcmp(hd + 8, "WAVE", 4)) r = dec_wav(d, h, hd);
    else if (!memcmp(hd, "FORM", 4) && (!memcmp(hd + 8, "AIFF", 4) || !memcmp(hd + 8, "AIFC", 4))) r = dec_aiff(d, h, hd);
    else {
        const char *what = !memcmp(hd + 4, "ftyp", 4) ? "AAC / ALAC (MP4)" : !memcmp(hd, "OggS", 4) ? "Ogg" : !memcmp(hd, "DSD ", 4) ? "DSF (native DSD)"
                         : !memcmp(hd, "FRM8", 4) ? "DFF (native DSD)" : (hd[0] == 0xFF && (hd[1] & 0xE0) == 0xE0) ? "MP3 / AAC (ADTS)" : "an unknown format";
        snprintf(why, wl, "%s is not played here (FLAC, WAV, AIFF, LPCM)", what);
    }
out:
    if (r < 0 && !why[0]) snprintf(why, wl, "%s", d->why ? d->why : "the track could not be read to its end");
    if (r < 0 && uq_gen() != j->gen) r = 0;
    hc_close(h);
    free(h); free(d);
    return r;
}

/* ── The decoder's thread ─────────────────────────────────────────────────────────────────────────────────────── */
static pthread_mutex_t dm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dcv = PTHREAD_COND_INITIALIZER;
static ud_job pend;
static int has_pend = 0;
static void *dec_main(void *arg) {
    (void)arg;
    ud_job *j = malloc(sizeof *j);
    if (!j) return NULL;
    for (;;) {
        pthread_mutex_lock(&dm);
        while (!has_pend) pthread_cond_wait(&dcv, &dm);
        *j = pend; has_pend = 0;
        pthread_mutex_unlock(&dm);
        for (;;) {
            char why[200] = "";
            const int r = play_job(j, why, sizeof why);
            if (r == 1) {
                uq_done(j->gen);
                pthread_mutex_lock(&dm);
                const int newer = has_pend;
                pthread_mutex_unlock(&dm);
                if (!newer && upnp_next(j->gen, j->seq, j)) continue;   /* gapless: the next track on the same run */
            } else if (r < 0) {
                ulog("UPnP: track %d: %s", j->seq, why);
                uq_done(j->gen);
                upnp_failed(j->gen, j->seq, why);
            }
            break;
        }
    }
    return NULL;
}
void ud_init(void) {
    static int done = 0;
    if (done++) return;
    flac_load();
    pthread_t th;
    if (!pthread_create(&th, NULL, dec_main, NULL)) pthread_detach(th);
}
void ud_start(const ud_job *j) {
    pthread_mutex_lock(&dm);
    pend = *j; has_pend = 1;
    pthread_cond_signal(&dcv);
    pthread_mutex_unlock(&dm);
}
