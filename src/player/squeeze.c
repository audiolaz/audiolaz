/* src/player/squeeze.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef _DEFAULT_SOURCE   /* build.sh passes -D_DEFAULT_SOURCE */
#define _DEFAULT_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#ifdef __APPLE__
#include <uuid/uuid.h>
#endif
#include <FLAC/stream_decoder.h>
#include "squeeze.h"
#include "rt_thread.h"

#if defined(ROOM_PORTABLE) && !defined(ROOM_FLAC_BUILTIN)   /* ROOM_FLAC_BUILTIN: engine/mac/flac.c linked in (macOS) */
#include <dlfcn.h>
static struct {
    FLAC__StreamDecoder *(*dnew)(void);
    FLAC__StreamDecoderInitStatus (*init)(FLAC__StreamDecoder *, FLAC__StreamDecoderReadCallback, FLAC__StreamDecoderSeekCallback,
                                          FLAC__StreamDecoderTellCallback, FLAC__StreamDecoderLengthCallback, FLAC__StreamDecoderEofCallback,
                                          FLAC__StreamDecoderWriteCallback, FLAC__StreamDecoderMetadataCallback,
                                          FLAC__StreamDecoderErrorCallback, void *);
    FLAC__bool (*run)(FLAC__StreamDecoder *), (*finish)(FLAC__StreamDecoder *);
    void (*del)(FLAC__StreamDecoder *);
    const char *const *errs;
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
        flac.errs = (const char *const *)dlsym(h, "FLAC__StreamDecoderErrorStatusString");
        have_flac = flac.dnew && flac.init && flac.run && flac.finish && flac.del && flac.errs;
        if (!have_flac) dlclose(h);
    }
    if (!have_flac) fputs("squeeze: no libFLAC (.so.14, .12 or .8): PCM only. In Roon, turn FLAC off for this zone "
                          "(Settings → Audio → the zone → Device Setup)\n", stderr);
}
#define FLAC__stream_decoder_new flac.dnew
#define FLAC__stream_decoder_init_stream flac.init
#define FLAC__stream_decoder_process_until_end_of_stream flac.run
#define FLAC__stream_decoder_finish flac.finish
#define FLAC__stream_decoder_delete flac.del
#define FLAC__StreamDecoderErrorStatusString flac.errs
#else
enum { have_flac = 1 };
#endif

static void sq_log(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    fputs("squeeze: ", stderr); vfprintf(stderr, fmt, a); fputc('\n', stderr); fflush(stderr);
    va_end(a);
}
static double mono_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static char server_arg[256], player[64];
static uint8_t mac[6];
static int fd = -1;
static uint32_t server_ip;                     /* network order: where a stream with no address of its own comes from */
static pthread_mutex_t send_lk = PTHREAD_MUTEX_INITIALIZER;
static double t_start;

static _Atomic unsigned gen = 1;               /* + 1 at every stop or flush */
static _Atomic int state = SQ_STOPPED;
static _Atomic int cont_seen = 0;
static _Atomic int active = 0;                 /* streams of this generation still being read */
static _Atomic int stmu_sent = 1;
static _Atomic uint64_t bytes_in = 0;
static _Atomic long elapsed_ms = 0;
static volatile double volume = 1.0;
/* What the status and the page say : the server's 3483 connected, and the last stream's format. */
enum { FMT_NONE, FMT_PCM, FMT_FLAC, FMT_FLAC_REFUSED };
static _Atomic int connected = 0;
static _Atomic int last_fmt = FMT_NONE;

static _Atomic unsigned measure_gen = 0;
/* The server's commands, a measurement's start, stop and failure: one at a time (each reads gen, state and measure_gen,
 * then changes them). Taken before qlk and send_lk, never inside them. */
static pthread_mutex_t cmd_lk = PTHREAD_MUTEX_INITIALIZER;
static int measure_current(void) { const unsigned m = atomic_load(&measure_gen); return m && m == atomic_load(&gen); }
static int measuring_now(void) { return measure_current() && atomic_load(&state) != SQ_STOPPED; }

/* ── The queue: decoded frames, the decoders writing, the engine reading ────────────────────────────── */
#define QFRAMES (1u << 22)                     /* 4 Mi frames: 21 s at 192 kHz, 95 s at 44.1, 32 MB */
#define QKEEP (1u << 19)                       /* the frames just read kept (2.7 s at 192 kHz): a pause gives back what the
                                                * engine had read and not yet played */

#define AHEAD_S 8

#define START_S 4
/* The stream socket's receive buffer (Linux doubles it: 128 KB, half a second at 44.1 kHz in 24 bits): left to itself,
 * Linux grows it to megabytes during the first burst, seconds more read from Roon beyond AHEAD_S. Not for 3483. */
#define STREAM_RCVBUF (64 * 1024)
static int32_t *qbuf;                          /* frames × 2 */
static _Atomic uint64_t qw, qr;                /* frames written, read, since the start */
static _Atomic uint64_t ahead = AHEAD_S * 44100u;   /* the read-ahead, in frames at the stream's rate (told to Roon too) */
static _Atomic uint64_t ramp_at;               /* qr when the queue was last found empty: the read-ahead grows from there */
/* Where each track begins in the queue, its rate and generation; tn: its number, for STMs and STMd. */
typedef struct { uint64_t at; unsigned rate, gen, tn; int told; } seg;
#define NSEG 64
static seg segs[NSEG];
static int nseg;
typedef struct { unsigned gen; int complete, started, sent_d; } trk;
#define NTRK 64
static trk trks[NTRK];
static unsigned tn_next, tn_start;             /* tracks begun by the decoders; the next the engine will say started */
static pthread_mutex_t qlk = PTHREAD_MUTEX_INITIALIZER;

static void put32(uint8_t *p, size_t *n, uint32_t v) { p[(*n)++] = v >> 24; p[(*n)++] = v >> 16; p[(*n)++] = v >> 8; p[(*n)++] = v; }
static void put16(uint8_t *p, size_t *n, uint16_t v) { p[(*n)++] = v >> 8; p[(*n)++] = v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void send_msg(const char *op, const void *payload, size_t len) {
    if (measuring_now() && (!memcmp(op, "RESP", 4) || !memcmp(op, "DSCO", 4))) return;   /* a measurement's stream: not the server's */
    uint8_t h[8];
    size_t n = 0;
    memcpy(h, op, 4); n = 4;
    put32(h, &n, (uint32_t)len);
    pthread_mutex_lock(&send_lk);
    if (fd >= 0) { send(fd, h, 8, MSG_NOSIGNAL); if (len) send(fd, payload, len, MSG_NOSIGNAL); }
    pthread_mutex_unlock(&send_lk);
}
static void stat_(const char *ev, uint32_t server_ts) {
    if (measuring_now() && strcmp(ev, "STMt")) return;         /* a measurement: only the heartbeat's answer, the player stopped */
    uint8_t p[64];
    size_t n = 0;
    memcpy(p, ev, 4); n = 4;
    p[n++] = 0; p[n++] = 0; p[n++] = 0;                         /* CRLFs, MAS initialised, MAS mode */
    uint64_t w = atomic_load(&qw), r = atomic_load(&qr);
    uint64_t size = atomic_load(&ahead) * 8u, full = (w - r) * 8u;     /* the stream's buffer and its fullness: never more */
    if (full > size) size = full;                                       /* full than its size (the previous track's
                                                                         * frames at another rate, or given back) */
    put32(p, &n, (uint32_t)size); put32(p, &n, (uint32_t)full);
    uint64_t b = atomic_load(&bytes_in);
    put32(p, &n, (uint32_t)(b >> 32)); put32(p, &n, (uint32_t)b);
    put16(p, &n, 0xffff);                                       /* signal: wired */
    put32(p, &n, (uint32_t)((mono_s() - t_start) * 1000));      /* jiffies */
    put32(p, &n, 3528000); put32(p, &n, 0);                     /* the output's buffer: not told as full (Roon's bar) */
    long ms = atomic_load(&elapsed_ms);
    put32(p, &n, (uint32_t)(ms / 1000)); put16(p, &n, 0); put32(p, &n, (uint32_t)ms);
    put32(p, &n, server_ts); put16(p, &n, 0);
    send_msg("STAT", p, n);
    if (strcmp(ev, "STMt")) sq_log("→ Roon %s", ev);
}

/* Under qlk: the segment the frame at r belongs to (the last beginning at or before it), -1 for none. */
static int seg_at(uint64_t r) { int k = -1; for (int i = 0; i < nseg; i++) if (segs[i].at <= r) k = i; return k; }
/* Under qlk: the tracks behind the reader forgotten, another generation's music (stopped, flushed) skipped. */
static void purge(void) {
    unsigned g = atomic_load(&gen);
    for (;;) {
        uint64_t r = atomic_load(&qr), w = atomic_load(&qw);
        int k = seg_at(r);
        if (k > 0) { memmove(segs, segs + k, sizeof(seg) * (size_t)(nseg - k)); nseg -= k; k = 0; }
        if (k < 0) { if (r < w && (!nseg || segs[0].at > w)) atomic_store(&qr, nseg ? segs[0].at : w); if (nseg && segs[0].at <= atomic_load(&qr)) continue; return; }
        if (segs[0].gen == g) return;
        atomic_store(&qr, nseg > 1 ? segs[1].at : w);
        memmove(segs, segs + 1, sizeof(seg) * (size_t)(nseg - 1)); nseg--;
        if (!nseg) { atomic_store(&qr, atomic_load(&qw)); return; }
    }
}

unsigned sq_next_rate(void) {
    pthread_mutex_lock(&qlk);
    purge();
    uint64_t r = atomic_load(&qr), w = atomic_load(&qw);
    int k = seg_at(r);
    unsigned rate = k >= 0 && (r < w || segs[k].at == r) ? segs[k].rate : 0;
    pthread_mutex_unlock(&qlk);
    return rate;
}

long sq_read(int32_t *dst, long frames, unsigned rate, int *track) {
    *track = 0;
    pthread_mutex_lock(&qlk);
    purge();
    uint64_t r = atomic_load(&qr), w = atomic_load(&qw);
    int k = seg_at(r);
    if (k < 0 || segs[k].rate != rate || r >= w) { pthread_mutex_unlock(&qlk); return 0; }
    uint64_t lim = w;
    if (k + 1 < nseg && segs[k + 1].at < lim) lim = segs[k + 1].at;   /* the next track begins a block of its own */
    if (segs[k].at == r && !segs[k].told) { segs[k].told = 1; *track = 1; }
    uint64_t n = lim - r < (uint64_t)frames ? lim - r : (uint64_t)frames;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t j = (r + i) & (QFRAMES - 1);
        dst[2 * i] = qbuf[2 * j]; dst[2 * i + 1] = qbuf[2 * j + 1];
    }
    atomic_store(&qr, r + n);
    pthread_mutex_unlock(&qlk);
    return (long)n;
}

void sq_unread(long frames) {
    if (frames <= 0) return;
    pthread_mutex_lock(&qlk);
    purge();
    uint64_t r = atomic_load(&qr);
    int k = seg_at(r);
    uint64_t lo = k >= 0 ? segs[k].at : r, back = (uint64_t)frames;
    if (back > QKEEP - 1) back = QKEEP - 1;
    if (back > r - lo) back = r - lo;
    if (k >= 0 && r - lo < QKEEP) {
        const trk *t = &trks[segs[k].tn % NTRK];
        if (t->gen == segs[k].gen && !t->started) { back = r - lo; segs[k].told = 0; }
    }
    atomic_store(&qr, r - back);
    pthread_mutex_unlock(&qlk);
}

/* Where the next frame to play stands in its stream, in seconds from the stream's first frame (the one its STMs
 * told); −1 with no stream, or one not told yet (its first frame next: sq_read tells it). Roon sends tracks of one
 * format one after the other in ONE stream and follows them by the elapsed time alone . */
double sq_position(void) {
    pthread_mutex_lock(&qlk);
    purge();
    uint64_t r = atomic_load(&qr);
    int k = seg_at(r);
    double s = k >= 0 && segs[k].told ? (double)(r - segs[k].at) / segs[k].rate : -1;
    pthread_mutex_unlock(&qlk);
    return s;
}

double sq_heard_position(void) {
    pthread_mutex_lock(&qlk);
    const int ok = tn_start > 0 && trks[(tn_start - 1) % NTRK].gen == atomic_load(&gen) && trks[(tn_start - 1) % NTRK].started;
    pthread_mutex_unlock(&qlk);
    return ok ? atomic_load(&elapsed_ms) / 1000.0 : -1.0;
}

int sq_state(void) { return atomic_load(&state); }
unsigned sq_generation(void) { return atomic_load(&gen); }
double sq_volume(void) { return volume; }
void sq_elapsed(double s) { atomic_store(&elapsed_ms, (long)(s * 1000)); }
int sq_ended(void) {
    if (atomic_load(&active) > 0) return 0;
    pthread_mutex_lock(&qlk);
    purge();
    int e = atomic_load(&qr) >= atomic_load(&qw);
    pthread_mutex_unlock(&qlk);
    return e;
}
/* STMd (Roon may then send the next track) once a track is both read to its end and playing, whichever comes
 * second: Roon never sends the queue ahead of what plays. */
void sq_started(void) {
    pthread_mutex_lock(&qlk);
    unsigned g = atomic_load(&gen);
    while (tn_start < tn_next && trks[tn_start % NTRK].gen != g) tn_start++;
    if (tn_start < tn_next) {
        trk *t = &trks[tn_start % NTRK];
        tn_start++;
        t->started = 1;
        atomic_store(&elapsed_ms, 0);
        stat_("STMs", 0);
        if (t->complete && !t->sent_d) { t->sent_d = 1; stat_("STMd", 0); }
    }
    pthread_mutex_unlock(&qlk);
}
void sq_finished(void) {
    if (atomic_exchange(&stmu_sent, 1)) return;
    stat_("STMu", 0);
    atomic_store(&state, SQ_STOPPED);
}

/* ── One track: its HTTP stream, decoded into the queue ──────────────────────────────────────────── */
typedef struct {
    char host[64]; int port;
    uint8_t header[2048]; size_t hlen;
    unsigned gen, tn, rate, bps;
    int contwait, sock, begun;
    uint8_t pre[65536]; size_t pre_len, pre_pos;   /* the music that came with the response's headers */
    int32_t *pcm; size_t pcm_cap;
    char fmt;                                      /* 'f' FLAC, 'p' PCM */
    uint64_t p_left; int p_sized;

    int measure, loop;                             /* the app's measuring track (sq_measure); played again and again
                                                    * (the live tuning's pink noise): fetched anew at its end, gapless */
    unsigned p_bits, p_rate, p_ch; int p_little;
} job;

static int tcp_connect(const char *host, int port, int rcvbuf, int conn_ms) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &ai) || !ai) return -1;
    int s = socket(ai->ai_family, ai->ai_socktype, 0);
    if (s >= 0 && rcvbuf > 0) setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);   /* before connect: the window */
    if (s >= 0 && conn_ms > 0) { struct timeval ct = { conn_ms / 1000, (conn_ms % 1000) * 1000 }; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &ct, sizeof ct); }   /* Linux: connect gives up then */
    if (s >= 0 && connect(s, ai->ai_addr, ai->ai_addrlen)) { close(s); s = -1; }
    freeaddrinfo(ai);
    if (s >= 0) { int on = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on); }
    return s;
}

/* The stream's next bytes: what came with the response's headers first, then the socket. > 0: read, 0: its end, −1: a
 * stop (another generation) or an error. */
static ssize_t take(job *j, uint8_t *buf, size_t want) {
    if (j->pre_pos < j->pre_len) {
        size_t k = j->pre_len - j->pre_pos < want ? j->pre_len - j->pre_pos : want;
        memcpy(buf, j->pre + j->pre_pos, k);
        j->pre_pos += k;
        return (ssize_t)k;
    }
    for (;;) {
        if (atomic_load(&gen) != j->gen) return -1;
        ssize_t r = recv(j->sock, buf, want, 0);
        if (r > 0) { atomic_fetch_add(&bytes_in, (uint64_t)r); return r; }
        if (r == 0) return 0;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
    }
}
static FLAC__StreamDecoderReadStatus read_cb(const FLAC__StreamDecoder *d, FLAC__byte buf[], size_t *bytes, void *ctx) {
    (void)d;
    ssize_t r = take(ctx, buf, *bytes);
    if (r > 0) { *bytes = (size_t)r; return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE; }
    *bytes = 0;
    return r == 0 ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM : FLAC__STREAM_DECODER_READ_STATUS_ABORT;
}
/* Frames into the queue, waiting for room (no more than AHEAD_S seconds of the stream's rate ahead of what plays, START_S
 * plus what played since the queue was last empty); 0 when this stream's generation has ended. */
static int q_push(const int32_t *src, uint64_t n, unsigned g, unsigned rate) {
    uint64_t done = 0;
    const uint64_t fr = rate ? rate : 44100u, cap = (uint64_t)AHEAD_S * fr;
    atomic_store(&ahead, cap);
    while (done < n) {
        if (atomic_load(&gen) != g) return 0;
        uint64_t w = atomic_load(&qw), r = atomic_load(&qr), used = w - r, room = used + QKEEP >= QFRAMES ? 0 : QFRAMES - QKEEP - used;
        if (!used) atomic_store(&ramp_at, r);
        uint64_t ra = atomic_load(&ramp_at), lim = (uint64_t)START_S * fr + (r > ra ? r - ra : 0);
        if (lim > cap) lim = cap;
        if (room > (used < lim ? lim - used : 0)) room = used < lim ? lim - used : 0;
        /* (11/10, the performance pass) 10 ms, not 2: the queue is seconds ahead (AHEAD_S), a wait of 2 ms woke the
         * player some 400 times a second for the whole play (the PC, test/engine-prof.sh: 650 context switches a
         * second, 270 now) */
        if (!room) { usleep(10000); continue; }
        uint64_t k = n - done < room ? n - done : room;
        for (uint64_t i = 0; i < k; i++) {
            uint64_t x = (w + i) & (QFRAMES - 1);
            qbuf[2 * x] = src[2 * (done + i)]; qbuf[2 * x + 1] = src[2 * (done + i) + 1];
        }
        atomic_store(&qw, w + k);
        done += k;
    }
    return 1;
}
/* A track (or, never seen, a rate change inside one) begins here in the queue: 0 when this stream's generation ended. */
static int track_begins(job *j, unsigned rate, unsigned bps, unsigned ch) {
    pthread_mutex_lock(&qlk);
    if (atomic_load(&gen) != j->gen) { pthread_mutex_unlock(&qlk); return 0; }
    if (nseg == NSEG) { memmove(segs, segs + 1, sizeof(seg) * (NSEG - 1)); nseg--; }
    j->tn = tn_next++;
    trks[j->tn % NTRK] = (trk){ j->gen, 0, 0, 0 };
    segs[nseg++] = (seg){ atomic_load(&qw), rate, j->gen, j->tn, 0 };
    pthread_mutex_unlock(&qlk);
    if (!j->begun) {
        sq_log("%s %u Hz, %u bits, %u channel(s)", j->fmt == 'p' ? "PCM" : "FLAC", rate, bps, ch);
        atomic_store(&last_fmt, j->fmt == 'p' ? FMT_PCM : FMT_FLAC);
    }
    j->begun = 1; j->rate = rate;
    return 1;
}
static FLAC__StreamDecoderWriteStatus write_cb(const FLAC__StreamDecoder *d, const FLAC__Frame *f, const FLAC__int32 *const b[], void *ctx) {
    (void)d;
    job *j = ctx;
    unsigned n = f->header.blocksize, ch = f->header.channels, rate = f->header.sample_rate;
    unsigned bps = f->header.bits_per_sample ? f->header.bits_per_sample : j->bps;
    if (!bps || bps > 32 || !ch || !rate) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    if ((!j->begun || rate != j->rate) && !track_begins(j, rate, bps, ch)) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    if (j->pcm_cap < n) { free(j->pcm); j->pcm = malloc(sizeof(int32_t) * 2 * n); j->pcm_cap = j->pcm ? n : 0; if (!j->pcm) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; }
    const unsigned sh = 32 - bps;                  /* the sample at the top of 32 bits: exact */
    for (unsigned i = 0; i < n; i++) {
        j->pcm[2 * i] = (int32_t)((uint32_t)b[0][i] << sh);
        j->pcm[2 * i + 1] = (int32_t)((uint32_t)b[ch > 1 ? 1 : 0][i] << sh);   /* mono: on both channels */
    }
    return q_push(j->pcm, n, j->gen, rate) ? FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE : FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
}
static void meta_cb(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *ctx) {
    (void)d;
    job *j = ctx;
    if (m->type == FLAC__METADATA_TYPE_STREAMINFO) j->bps = m->data.stream_info.bits_per_sample;
}
static void error_cb(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus s, void *ctx) {
    (void)d; (void)ctx;
    sq_log("FLAC: %s", FLAC__StreamDecoderErrorStatusString[s]);
}

static unsigned le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned be16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static double ext80(const uint8_t *p) {           /* AIFF's rate: an 80-bit IEEE extended float */
    int e = ((p[0] & 0x7f) << 8 | p[1]) - 16383 - 63;
    uint64_t m = 0;
    for (int i = 0; i < 8; i++) m = m << 8 | p[2 + i];
    return ldexp((double)m, e);
}
/* The header in h[0 … n): its description into j and where the samples start; 0: no header there; −1: one, unreadable or
 * not plain PCM; −2: the samples' start not reached yet (read more). */
static int pcm_header(job *j, const uint8_t *h, size_t n, size_t *start) {
    if (n < 12) return -2;
    if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        size_t at = 12;
        int fmt_seen = 0;
        while (at + 8 <= n) {
            uint32_t sz = le32(h + at + 4);
            if (!memcmp(h + at, "fmt ", 4)) {
                if (at + 8 + 16 > n) return -2;
                unsigned tag = le16(h + at + 8);
                if (tag == 0xfffe && at + 8 + 26 <= n) tag = le16(h + at + 8 + 24);   /* WAVE_FORMAT_EXTENSIBLE: its sub-format */
                if (tag != 1) return -1;
                j->p_ch = le16(h + at + 10); j->p_rate = le32(h + at + 12); j->p_bits = le16(h + at + 22); j->p_little = 1;
                fmt_seen = 1;
            } else if (!memcmp(h + at, "data", 4)) {
                if (!fmt_seen) return -1;
                if (j->measure && sz && sz != 0xffffffffu) { j->p_left = sz; j->p_sized = 1; }
                *start = at + 8; return 1;
            }
            at += 8 + sz + (sz & 1);
        }
        return n > 8192 ? -1 : -2;
    }
    if (!memcmp(h, "FORM", 4) && (!memcmp(h + 8, "AIFF", 4) || !memcmp(h + 8, "AIFC", 4))) {
        size_t at = 12;
        int comm_seen = 0;
        while (at + 8 <= n) {
            uint32_t sz = get32(h + at + 4);
            if (!memcmp(h + at, "COMM", 4)) {
                if (at + 8 + 18 > n) return -2;
                int little = 0;                            /* AIFC: big-endian ("NONE", "twos") or little ("sowt") */
                if (!memcmp(h + 8, "AIFC", 4)) {
                    if (at + 8 + 22 > n) return -2;
                    if (!memcmp(h + at + 26, "sowt", 4)) little = 1;
                    else if (memcmp(h + at + 26, "NONE", 4) && memcmp(h + at + 26, "twos", 4)) return -1;
                }
                j->p_ch = be16(h + at + 8); j->p_bits = be16(h + at + 14); j->p_rate = (unsigned)lround(ext80(h + at + 16)); j->p_little = little;
                comm_seen = 1;
            } else if (!memcmp(h + at, "SSND", 4)) {
                if (!comm_seen || at + 16 > n) return comm_seen ? -2 : -1;
                *start = at + 16 + get32(h + at + 8);    /* its offset: samples start that much further */
                return *start <= n ? 1 : -2;
            }
            at += 8 + sz + (sz & 1);
        }
        return n > 8192 ? -1 : -2;
    }
    return 0;
}
static int pcm_play(job *j) {
    uint8_t buf[65536 + 16];
    size_t have = 0, start = 0;
    /* the header, if any: read until it is whole (or seen to be no header) */
    for (;;) {
        int h = pcm_header(j, buf, have, &start);
        if (h == 1) { memmove(buf, buf + start, have - start); have -= start; break; }
        if (h == 0) break;
        if (h == -1) { sq_log("PCM header not understood"); return 0; }
        if (have >= sizeof buf - 16) { sq_log("PCM header too long"); return 0; }
        ssize_t r = take(j, buf + have, sizeof buf - 16 - have);
        if (r <= 0) return 0;
        have += (size_t)r;
    }
    const unsigned bits = j->p_bits, ch = j->p_ch, rate = j->p_rate, bytes = bits / 8;
    if (!rate || !ch || ch > 8 || (bits != 16 && bits != 24 && bits != 32) || j->p_little < 0) {
        sq_log("PCM not described (%u bits, %u Hz, %u channel(s))", bits, rate, ch);
        return 0;
    }
    const size_t frame = (size_t)bytes * ch;
    if (j->p_sized && have > j->p_left) have = (size_t)j->p_left;
    enum { CHUNK = 4096 };
    int32_t out[2 * CHUNK];
    for (;;) {
        size_t frames = have / frame;
        if (frames && !j->begun && !track_begins(j, rate, bits, ch)) return 0;
        for (size_t done = 0; done < frames;) {
            size_t k = frames - done < CHUNK ? frames - done : CHUNK;
            for (size_t i = 0; i < k; i++) {
                const uint8_t *f = buf + (done + i) * frame;
                for (unsigned c = 0; c < 2; c++) {
                    const uint8_t *s = f + (size_t)(c < ch ? c : 0) * bytes;
                    uint32_t v = 0;
                    if (j->p_little) for (unsigned b = 0; b < bytes; b++) v |= (uint32_t)s[b] << (8 * (4 - bytes + b));
                    else for (unsigned b = 0; b < bytes; b++) v |= (uint32_t)s[b] << (8 * (3 - b));
                    out[2 * i + c] = (int32_t)v;          /* at the top of 32 bits: exact */
                }
            }
            if (!q_push(out, k, j->gen, rate)) return 0;
            done += k;
        }
        size_t used = frames * frame;
        memmove(buf, buf + used, have - used);
        have -= used;
        if (j->p_sized) { j->p_left -= used; if (j->p_left < frame) return j->begun; }   /* the data chunk's end */
        ssize_t r = take(j, buf + have, sizeof buf - 16 - have);
        if (r < 0) return 0;
        if (r == 0) return j->begun && !(j->p_sized && j->p_left >= frame);   /* its end (a measurement cut short: not) */
        have += (size_t)r;
        if (j->p_sized && have > j->p_left) have = (size_t)j->p_left;
    }
}

static pthread_mutex_t feed_lk = PTHREAD_MUTEX_INITIALIZER;   /* streams decoded one after the other, in order */
static void *stream_main(void *arg) {
    job *j = arg;
    rt_interactive();
    pthread_mutex_lock(&feed_lk);
    int ok = 0;
again:
    if (atomic_load(&gen) != j->gen) goto done;
    j->sock = tcp_connect(j->host, j->port, STREAM_RCVBUF, j->measure ? 3000 : 0);   /* an app gone: never Roon's tracks held up */
    if (j->sock < 0) { sq_log("stream unreachable (%s:%d)", j->host, j->port); goto done; }
    struct timeval tv = { 0, 500000 };             /* a stop is seen within half a second while waiting for data */
    setsockopt(j->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    send(j->sock, j->header, j->hlen, MSG_NOSIGNAL);
    /* The response's headers, told to the server (RESP), then the music. */
    char head[16384];
    size_t hl = 0;
    for (;;) {
        if (atomic_load(&gen) != j->gen) goto closed;
        ssize_t r = recv(j->sock, head + hl, sizeof head - 1 - hl, 0);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (r <= 0) { sq_log("stream closed before the music"); goto closed; }
        hl += (size_t)r;
        head[hl] = 0;
        char *e = strstr(head, "\r\n\r\n");
        if (e) {
            size_t hend = (size_t)(e - head) + 4;
            j->pre_len = hl - hend < sizeof j->pre ? hl - hend : sizeof j->pre;
            memcpy(j->pre, head + hend, j->pre_len);
            atomic_fetch_add(&bytes_in, (uint64_t)(hl - hend));
            if (j->measure && (hl < 12 || head[9] != '2')) { sq_log("measurement: the app answered \"%.40s\"", head); goto closed; }   /* a 302, a 404: no track */
            send_msg("RESP", head, hend);
            break;
        }
        if (hl >= sizeof head - 1) { sq_log("stream headers too long"); goto closed; }
    }
    if (j->contwait) for (int k = 0; k < 500 && !atomic_load(&cont_seen); k++) usleep(10000);
    if (j->fmt == 'p') ok = pcm_play(j);
    else {
        FLAC__StreamDecoder *dec = FLAC__stream_decoder_new();
        if (dec && FLAC__stream_decoder_init_stream(dec, read_cb, NULL, NULL, NULL, NULL, write_cb, meta_cb, error_cb, j) == FLAC__STREAM_DECODER_INIT_STATUS_OK) {
            ok = FLAC__stream_decoder_process_until_end_of_stream(dec) && j->begun;
            FLAC__stream_decoder_finish(dec);
        }
        if (dec) FLAC__stream_decoder_delete(dec);
    }
closed:
    close(j->sock);
    if (atomic_load(&gen) == j->gen) {
        uint8_t reason = 0;
        send_msg("DSCO", &reason, 1);
        if (ok) {
            pthread_mutex_lock(&qlk);
            trk *t = &trks[j->tn % NTRK];
            if (t->gen == j->gen) { t->complete = 1; if (t->started && !t->sent_d) { t->sent_d = 1; stat_("STMd", 0); } }
            pthread_mutex_unlock(&qlk);
        } else if (!j->begun) stat_("STMn", 0);    /* nothing to play: Roon moves on */
        if (ok && j->measure && j->loop) {         /* once more, queued behind itself: no gap */
            j->pre_len = j->pre_pos = 0; j->begun = 0; j->p_bits = j->p_rate = j->p_ch = 0; j->p_little = -1; j->p_sized = 0; ok = 0;
            goto again;
        }
    }
done:
    pthread_mutex_lock(&cmd_lk);
    if (j->measure && (!j->begun || !ok) && atomic_load(&gen) == j->gen) {
        atomic_store(&state, SQ_STOPPED); atomic_store(&measure_gen, 0);
        sq_log("measurement: the app's track could not be read to its end (%s:%d)", j->host, j->port);
    }
    pthread_mutex_unlock(&cmd_lk);
    if (atomic_load(&gen) == j->gen && atomic_load(&active) > 0) atomic_fetch_sub(&active, 1);
    pthread_mutex_unlock(&feed_lk);
    free(j->pcm); free(j);
    return NULL;
}

/* ── The server's commands ─────────────────────────────────────────────────────────────────────────── */
static void stop_all(void) {
    atomic_fetch_add(&gen, 1);
    atomic_store(&state, SQ_STOPPED);
    atomic_store(&active, 0);
    atomic_store(&stmu_sent, 1);
    atomic_store(&elapsed_ms, 0);
    pthread_mutex_lock(&qlk);
    tn_start = tn_next;
    pthread_mutex_unlock(&qlk);
}
static void strm_(const uint8_t *d, size_t n);
static void strm(const uint8_t *d, size_t n) {
    pthread_mutex_lock(&cmd_lk);
    strm_(d, n);
    pthread_mutex_unlock(&cmd_lk);
}
static void strm_(const uint8_t *d, size_t n) {
    if (n < 24) return;
    char cmd = (char)d[0];
    if (cmd != 't') sq_log("← Roon strm %c%s", cmd, cmd == 's' ? (d[1] == '1' || d[1] == '3' ? " (starts at once)" : " (waits)") : "");
    switch (cmd) {
    case 't': stat_("STMt", get32(d + 14)); break;
    /* a measurement is the player's own: the server's stop or pause (it plays nothing there) leaves it be; its own
     * track (s) ends it */
    case 'q': case 'f': if (!measuring_now()) stop_all(); stat_("STMf", 0); break;
    case 'p': if (!measuring_now() && atomic_load(&state) == SQ_PLAYING) atomic_store(&state, SQ_PAUSED); stat_("STMp", 0); break;
    case 'u': atomic_store(&state, SQ_PLAYING); stat_("STMr", 0); break;
    case 's': {
        if (measure_current()) stop_all();             /* the server's track after (or over) a measurement: its own generation */
        if (d[2] != 'f' && d[2] != 'p') { sq_log("format \"%c\" not handled (FLAC and PCM only)", d[2]); stat_("STMn", 0); break; }
        if (d[2] == 'f' && !have_flac) { sq_log("FLAC stream refused (no libFLAC): turn FLAC off for this zone in Roon"); atomic_store(&last_fmt, FMT_FLAC_REFUSED); stat_("STMn", 0); break; }
        job *j = calloc(1, sizeof *j);
        if (!j) break;
        j->fmt = (char)d[2];
        if (j->fmt == 'p') {
            static const unsigned RATES[] = { 11025, 22050, 32000, 44100, 48000, 8000, 12000, 16000, 24000, 96000, 88200,
                                              176400, 192000, 352800, 384000 };
            int sz = d[3] - '0', rt = d[4] - '0';
            j->p_bits = sz >= 0 && sz <= 3 ? 8u * (unsigned)(sz + 1) : 0;   /* 8, 16, 24, 32 */
            j->p_rate = rt >= 0 && rt < (int)(sizeof RATES / sizeof *RATES) ? RATES[rt] : 0;
            j->p_ch = d[5] == '1' ? 1 : d[5] == '2' ? 2 : 0;
            j->p_little = d[6] == '1' ? 1 : d[6] == '0' ? 0 : -1;
            sq_log("PCM asked: size %c, rate %c, channels %c, order %c", d[3], d[4], d[5], d[6]);
        }
        uint32_t ip = get32(d + 20);
        struct in_addr a = { .s_addr = ip ? htonl(ip) : server_ip };
        inet_ntop(AF_INET, &a, j->host, sizeof j->host);
        j->port = d[18] << 8 | d[19];
        j->hlen = n - 24 < sizeof j->header ? n - 24 : sizeof j->header;
        memcpy(j->header, d + 24, j->hlen);
        int autostart = d[1] - '0';
        j->contwait = autostart >= 2;
        j->gen = atomic_load(&gen);
        atomic_store(&cont_seen, 0);
        /* the first track after a stop starts at once, or waits for 'u'; one more in a row changes nothing */
        if (atomic_load(&state) == SQ_STOPPED) atomic_store(&state, autostart % 2 ? SQ_PLAYING : SQ_PAUSED);
        atomic_fetch_add(&active, 1);
        atomic_store(&stmu_sent, 0);
        stat_("STMc", 0);
        pthread_t th;
        if (pthread_create(&th, NULL, stream_main, j)) { free(j); atomic_fetch_sub(&active, 1); break; }
        pthread_detach(th);
        break;
    }
    default: break;
    }
}
static void handle(const char *op, const uint8_t *d, size_t n) {
    if (!memcmp(op, "strm", 4)) strm(d, n);
    else if (!memcmp(op, "setd", 4)) {
        if (n >= 1 && d[0] == 0) {
            if (n == 1) { uint8_t p[80]; size_t k = strlen(player); p[0] = 0; memcpy(p + 1, player, k); p[k + 1] = 0; send_msg("SETD", p, k + 2); }
            else sq_log("Roon names the player \"%.*s\"", (int)(n - 1), (const char *)d + 1);
        }
    } else if (!memcmp(op, "audg", 4)) {
        /* old gains, digital volume, preamp, new gains (16.16 fixed point, 65536 = unity) */
        if (n >= 18) { double l = get32(d + 10), r = get32(d + 14), g = (l > r ? l : r) / 65536.0; volume = g > 1 ? 1 : g; }
    } else if (!memcmp(op, "cont", 4)) atomic_store(&cont_seen, 1);
}

static int read_all(uint8_t *p, size_t n) {
    size_t got = 0;
    while (got < n) { ssize_t r = recv(fd, p + got, n - got, 0); if (r <= 0) return 0; got += (size_t)r; }
    return 1;
}
static void helo(void) {
    uint8_t p[512];
    size_t n = 0;
    p[n++] = 12; p[n++] = 0;                       /* device 12 (Squeezelite's), revision */
    memcpy(p + n, mac, 6); n += 6;
    memset(p + n, 0, 16); n += 16;                 /* UUID */
    put16(p, &n, 0);                               /* WLAN channels */
    put32(p, &n, 0); put32(p, &n, 0);              /* bytes received */
    p[n++] = 'e'; p[n++] = 'n';
    n += (size_t)snprintf((char *)p + n, sizeof p - n, "Model=squeezelite,ModelName=%s,AccuratePlayPoints=1,HasDigitalOut=1,"
                          "Firmware=v1.9.9-audiolaz,MaxSampleRate=192000,%spcm,aif", player, have_flac ? "flc," : "");
    send_msg("HELO", p, n);
}
/* "auto": the first server to answer a broadcast on the Squeezebox port. */
static int discover(char *out, size_t len) {
    int s = socket(AF_INET, SOCK_DGRAM, 0), on = 1;
    if (s < 0) return 0;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    struct timeval tv = { 2, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(3483), .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
    for (int k = 0; k < 3; k++) {
        sendto(s, "e", 1, 0, (struct sockaddr *)&to, sizeof to);
        char buf[512];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t r = recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (r > 0 && buf[0] == 'E') { inet_ntop(AF_INET, &from.sin_addr, out, len); close(s); return 1; }
    }
    close(s);
    return 0;
}
static void *slim_main(void *arg) {
    (void)arg;
    for (;;) {
        char host[256];
        snprintf(host, sizeof host, "%s", server_arg);
        if (!strcmp(server_arg, "auto") && !discover(host, sizeof host)) { atomic_store(&connected, 0); sleep(5); continue; }

        int port = 3483;
        char *colon = strchr(host, ':');
        if (colon && !strchr(colon + 1, ':') && atoi(colon + 1) > 0) { port = atoi(colon + 1); *colon = 0; }
        int s = tcp_connect(host, port, 0, 0);
        if (s < 0) { atomic_store(&connected, 0); sq_log("server %s:%d unreachable, again in 5 s", host, port); sleep(5); continue; }
        struct sockaddr_in pa;
        socklen_t pl = sizeof pa;
        if (!getpeername(s, (struct sockaddr *)&pa, &pl)) server_ip = pa.sin_addr.s_addr;
        pthread_mutex_lock(&send_lk); fd = s; pthread_mutex_unlock(&send_lk);
        helo();
        sq_log("connected to %s: \"%s\" is in Roon → Settings → Audio (Squeezebox)", host, player);
        atomic_store(&connected, 1);
        uint8_t h[2], *buf = malloc(65536);
        while (buf && read_all(h, 2)) {
            size_t n = (size_t)h[0] << 8 | h[1];
            if (n < 4 || !read_all(buf, n)) break;
            handle((const char *)buf, buf + 4, n - 4);
        }
        free(buf);
        atomic_store(&connected, 0);
        sq_log("the server closed the connection: again");
        pthread_mutex_lock(&send_lk); close(fd); fd = -1; pthread_mutex_unlock(&send_lk);
        pthread_mutex_lock(&cmd_lk);
        if (!measuring_now()) stop_all();          /* a measurement goes on without the server */
        pthread_mutex_unlock(&cmd_lk);
        sleep(2);
    }
    return NULL;
}

/* The measurement: from http://host:port/path (the app's track, WAV), at once (another measurement replaced). 0:
 * started; −1: no Squeezebox input; −2: music plays (Roon's or LMS's: to stop there first, the server never told). */
static _Atomic int measure_corrected = 0;

int sq_measure_corrected(void) { return atomic_load(&measure_corrected); }
int sq_measure(const char *host, int port, const char *path, int loop, int corrected) {
    if (!qbuf) return -1;
    job *j = calloc(1, sizeof *j);
    if (!j) return -1;
    pthread_mutex_lock(&cmd_lk);
    if (atomic_load(&state) != SQ_STOPPED && !measuring_now()) { pthread_mutex_unlock(&cmd_lk); free(j); return -2; }   /* music plays: stopped first */
    stop_all();
    j->fmt = 'p'; j->p_little = -1; j->measure = 1; j->loop = loop != 0;   /* its header says what it is */
    snprintf(j->host, sizeof j->host, "%s", host);
    j->port = port;
    j->hlen = (size_t)snprintf((char *)j->header, sizeof j->header, "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", path, host, port);
    if (j->hlen >= sizeof j->header) { pthread_mutex_unlock(&cmd_lk); free(j); return -1; }
    j->gen = atomic_load(&gen);
    atomic_store(&measure_gen, j->gen);
    atomic_store(&measure_corrected, corrected == 2 ? 2 : corrected != 0);
    atomic_store(&cont_seen, 1);
    atomic_store(&state, SQ_PLAYING);
    atomic_fetch_add(&active, 1);
    atomic_store(&stmu_sent, 0);
    pthread_t th;
    if (pthread_create(&th, NULL, stream_main, j)) { free(j); atomic_fetch_sub(&active, 1); stop_all(); pthread_mutex_unlock(&cmd_lk); return -1; }
    pthread_detach(th);
    pthread_mutex_unlock(&cmd_lk);
    sq_log("measurement: %s:%d%s%s%s", host, port, path, loop ? " (in a loop)" : "", corrected == 2 ? " (through the sound as chosen: the app's)" : corrected ? " (through the room's sound: a verification)" : "");
    return 0;
}
/* Another of the player's inputs started (08/10, roomd.c --loop-subs): the server's music paused, and the server told
 * (STMp: Roon shows the zone paused, and its play takes it up again); never a measurement. */
void sq_pause_other(void) {
    pthread_mutex_lock(&cmd_lk);
    if (atomic_load(&state) == SQ_PLAYING && !measure_current()) { atomic_store(&state, SQ_PAUSED); stat_("STMp", 0); }
    pthread_mutex_unlock(&cmd_lk);
}
void sq_measure_stop(void) {
    pthread_mutex_lock(&cmd_lk);
    if (measuring_now()) { stop_all(); sq_log("measurement stopped"); }
    pthread_mutex_unlock(&cmd_lk);
}
/* 0: no measurement (none, stopped, or its track unreadable); 1: playing (or about to); 2: played to its end (until
 * the next stream or stop). */
int sq_measuring(void) {
    if (!measure_current()) return 0;
    return atomic_load(&state) == SQ_STOPPED ? 2 : 1;
}

int sq_connected(void) { return atomic_load(&connected); }
const char *sq_format(void) {
    static const char *const NAMES[] = { "", "pcm", "flac", "flac-refused" };
    const int f = atomic_load(&last_fmt);
    return f >= 0 && f < (int)(sizeof NAMES / sizeof *NAMES) ? NAMES[f] : "";
}
const char *sq_name(void) { return player; }

static char sq_mac_seed_v[128] = "audiolaz Room";
static int sq_mac_seeded = 0;          /* Linux: --mac-seed given (a voie's: its room), else the player's name */
void sq_mac_seed(const char *seed) { snprintf(sq_mac_seed_v, sizeof sq_mac_seed_v, "%s", seed && *seed ? seed : "audiolaz Room"); sq_mac_seeded = seed && *seed; }
int sq_start(const char *server, const char *name) {
    snprintf(server_arg, sizeof server_arg, "%s", server && *server ? server : "auto");
    snprintf(player, sizeof player, "%s", name && *name ? name : "audiolaz Room");
    qbuf = calloc(2 * (size_t)QFRAMES, sizeof(int32_t));
    if (!qbuf) return -1;
#if defined(ROOM_PORTABLE) && !defined(ROOM_FLAC_BUILTIN)
    flac_load();
#endif
    t_start = mono_s();
    /* A stable, locally administered MAC from the machine's id: Roon knows the player by it. */
    uint32_t hsh = 2166136261u;
#ifdef __APPLE__

    uuid_t hu;
    struct timespec wait = { 1, 0 };
    if (gethostuuid(hu, &wait)) memset(hu, 0, sizeof hu);
    for (int i = 0; i < 16; i++) { hsh ^= hu[i]; hsh *= 16777619u; }
    for (const char *p = sq_mac_seed_v; *p; p++) { hsh ^= (uint8_t)*p; hsh *= 16777619u; }
    mac[0] = 0x02; mac[1] = 0x41;
#else
    FILE *f = fopen("/etc/machine-id", "r");
    if (f) { int c; while ((c = fgetc(f)) != EOF) { hsh ^= (uint32_t)c; hsh *= 16777619u; } fclose(f); }

    for (const char *p = sq_mac_seeded ? sq_mac_seed_v : player; *p; p++) { hsh ^= (uint8_t)*p; hsh *= 16777619u; }
    mac[0] = 0x02; mac[1] = 0x4c;
#endif
    mac[2] = hsh >> 24; mac[3] = hsh >> 16; mac[4] = hsh >> 8; mac[5] = hsh;
    pthread_t th;
    if (pthread_create(&th, NULL, slim_main, NULL)) return -1;
    pthread_detach(th);
    return 0;
}
