/* src/tap/levels.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_LEVELS_H
#define AUDIOLAZ_LEVELS_H

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LV_BANDS 8
#define LV_FPS 30
#define LV_RING 256              /* frames: 8.5 s, more than any player's ring and output buffer */
#define LV_MAX_CLIENTS 4
#define LV_LEAD_MS 150           /* a frame sent this long before it is heard */
#define LV_FLOOR_DB -100.0
#ifdef MSG_NOSIGNAL
#define LV_NOSIG MSG_NOSIGNAL
#else
#define LV_NOSIG 0               /* (macOS before MSG_NOSIGNAL: roomd ignores SIGPIPE) */
#endif

typedef struct {
    long long t_ns;              /* when heard, on CLOCK_MONOTONIC */
    unsigned rate;
    float rms[2], pk[2], band[LV_BANDS], onset;
} lv_frame;

typedef struct { double b0, b1, b2, a1, a2, z1, z2; } lv_bq;

typedef struct {
    _Atomic int listeners;       /* clients connected: the audio thread analyses only while > 0 */
    _Atomic int gen;             /* a new first client: the analysis starts afresh */
    _Atomic long long delay_us;  /* from the ring's entry to the ear (levels_set_delay) */
    /* the audio thread's own */
    unsigned rate;
    int seen_gen, dec, dgot;
    long hop, got, bn;
    double dsum, ss[2], pk[2], bs[LV_BANDS], prev[LV_BANDS], since;
    lv_bq bq[LV_BANDS][2];         /* two sections each: 24 dB down two octaves away (one section: 12) */
    /* frames: written by the audio thread, read by the stream's */
    lv_frame ring[LV_RING];
    _Atomic unsigned long long w;
    /* the stream's thread's */
    int fds[LV_MAX_CLIENTS], nfd, started;
    unsigned long long r;
    long long idle_said;
} lvtap;

static pthread_mutex_t lv_mu = PTHREAD_MUTEX_INITIALIZER;

static const double LV_CENTRE[LV_BANDS] = { 62.5, 125, 250, 500, 1000, 2000, 4000, 8000 };
/* the attack's weights: the kick and the bass first, the cymbals' wash least */
static const double LV_W[LV_BANDS] = { 1.6, 1.5, 1.2, 1.0, 0.9, 0.7, 0.6, 0.5 };

static inline long long lv_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static inline double lv_db(double ms) {
    const double d = ms > 1e-20 ? 10.0 * log10(ms) : LV_FLOOR_DB;
    return d < LV_FLOOR_DB ? LV_FLOOR_DB : d;
}

/* RBJ band-pass, 0 dB at its centre, an octave wide (Q √2) */
static void lv_bq_band(lv_bq *f, double fc, double fs) {
    const double w = 2 * M_PI * fc / fs, al = sin(w) / (2 * M_SQRT2), a0 = 1 + al;
    f->b0 = al / a0; f->b1 = 0; f->b2 = -al / a0;
    f->a1 = -2 * cos(w) / a0; f->a2 = (1 - al) / a0;
    f->z1 = f->z2 = 0;
}
static inline double lv_bq_step(lv_bq *f, double x) {   /* transposed direct form II */
    const double y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

static void lv_setup(lvtap *L, unsigned rate) {
    L->rate = rate;
    L->dec = rate > 48000 ? (int)(rate / 44100) : 1;  /* 88.2/96 → 2, 176.4/192 → 4, 352.8/384 → 8, 705.6/768 → 16 */
    if (L->dec < 1) L->dec = 1;
    const double fs = (double)rate / L->dec;
    for (int b = 0; b < LV_BANDS; b++) {
        lv_bq_band(&L->bq[b][0], LV_CENTRE[b], fs); lv_bq_band(&L->bq[b][1], LV_CENTRE[b], fs);
        L->prev[b] = LV_FLOOR_DB; L->bs[b] = 0;
    }
    L->hop = (long)((rate + LV_FPS / 2) / LV_FPS);
    L->got = L->bn = 0; L->dgot = 0; L->dsum = 0;
    L->ss[0] = L->ss[1] = L->pk[0] = L->pk[1] = 0;
    L->since = 1;
}

/* One frame closed: its levels, its attack, into the ring. */
static void lv_close(lvtap *L, long long t_ns) {
    lv_frame *f = &L->ring[atomic_load_explicit(&L->w, memory_order_relaxed) % LV_RING];
    f->t_ns = t_ns; f->rate = L->rate;
    for (int c = 0; c < 2; c++) {
        f->rms[c] = (float)lv_db(L->ss[c] / (double)L->hop);
        f->pk[c] = (float)lv_db(L->pk[c] * L->pk[c]);
        L->ss[c] = 0; L->pk[c] = 0;
    }
    /* the attack: the rise of each band since the frame before (the spectral flux, in dB), weighted, averaged */
    const double dt = (double)L->hop / L->rate;
    double flux = 0, wsum = 0, top = LV_FLOOR_DB;
    for (int b = 0; b < LV_BANDS; b++) {
        const double d = lv_db(L->bn ? L->bs[b] / (double)L->bn : 0);
        f->band[b] = (float)d;
        if (d > L->prev[b]) flux += (d - L->prev[b]) * LV_W[b];
        wsum += LV_W[b];
        if (d > top) top = d;
        L->prev[b] = d;
        L->bs[b] = 0;
    }
    L->bn = 0;
    flux /= wsum;
    L->since += dt;
    f->onset = 0;
    if (flux > 3 && top > -60 && L->since >= 0.099) {   /* 3 dB up across the bands at least, 100 ms after the last */
        const double o = (flux - 3) / 12;
        f->onset = (float)(o < 0.1 ? 0.1 : o > 1 ? 1 : o);
        L->since = 0;
    }
    atomic_fetch_add_explicit(&L->w, 1, memory_order_release);
}

static void lv_feed_(lvtap *L, const double *x, long n, unsigned rate, long long now_ns) {
    const int g = atomic_load_explicit(&L->gen, memory_order_acquire);
    if (rate != L->rate || g != L->seen_gen) { lv_setup(L, rate); L->seen_gen = g; }
    const long long delay_ns = atomic_load_explicit(&L->delay_us, memory_order_relaxed) * 1000;
    const double ns_per = 1e9 / rate;
    for (long i = 0; i < n; i++) {
        const double l = x[2 * i], r = x[2 * i + 1], al = fabs(l), ar = fabs(r);
        L->ss[0] += l * l; L->ss[1] += r * r;
        if (al > L->pk[0]) L->pk[0] = al;
        if (ar > L->pk[1]) L->pk[1] = ar;
        L->dsum += 0.5 * (l + r);
        if (++L->dgot == L->dec) {
            const double m = L->dsum / L->dec;
            for (int b = 0; b < LV_BANDS; b++) { const double y = lv_bq_step(&L->bq[b][1], lv_bq_step(&L->bq[b][0], m)); L->bs[b] += y * y; }
            L->bn++;
            L->dgot = 0; L->dsum = 0;
        }
        /* the frame's last sample enters the ring with the block's last, (n − 1 − i) samples after it in the music */
        if (++L->got == L->hop) { lv_close(L, now_ns + delay_ns - (long long)((double)(n - 1 - i) * ns_per)); L->got = 0; }
    }
}

/* The audio thread, once per block: x interleaved stereo (L, R), n frames at rate; now_ns when the block enters the
 * ring (CLOCK_MONOTONIC). Only reads x. */
static inline void levels_feed(lvtap *L, const double *x, long n, unsigned rate, long long now_ns) {
    if (!atomic_load_explicit(&L->listeners, memory_order_relaxed) || !rate || n <= 0) return;
    lv_feed_(L, x, n, rate, now_ns);
}
static inline int levels_wanted(lvtap *L) { return atomic_load_explicit(&L->listeners, memory_order_relaxed) > 0; }
static inline void levels_set_delay(lvtap *L, double seconds) {
    atomic_store_explicit(&L->delay_us, (long long)(seconds > 0 ? seconds * 1e6 : 0), memory_order_relaxed);
}

/* A frame as the stream sends it, d from now_ns; the bytes (≤ len − 1), 0 if it does not fit. */
static int lv_frame_json(const lv_frame *f, long long now_ns, char *b, size_t len) {
    const double d = (double)(f->t_ns - now_ns) / 1e6;
    int n = snprintf(b, len, "data: {\"p\":1,\"d\":%.0f,\"t\":%lld,\"r\":%u,\"rms\":[%.1f,%.1f],\"pk\":[%.1f,%.1f],\"b\":[",
                     d, f->t_ns, f->rate, f->rms[0], f->rms[1], f->pk[0], f->pk[1]);
    for (int i = 0; i < LV_BANDS && n > 0 && (size_t)n < len; i++)
        n += snprintf(b + n, len - (size_t)n, "%s%.1f", i ? "," : "", f->band[i]);
    if (n > 0 && (size_t)n < len) n += snprintf(b + n, len - (size_t)n, "],\"on\":%.2f}\n\n", f->onset);
    return n > 0 && (size_t)n < len ? n : 0;
}

/* Sends to every client; one that refuses (gone, or so far behind that its socket is full) is closed. Under lv_mu. */
static void lv_send_all(lvtap *L, const char *b, size_t n) {
    for (int i = 0; i < L->nfd; ) {
        const ssize_t s = send(L->fds[i], b, n, LV_NOSIG);
        if (s == (ssize_t)n || (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) { i++; continue; }   /* full: this frame skipped */
        close(L->fds[i]);
        L->fds[i] = L->fds[--L->nfd];
        atomic_fetch_sub(&L->listeners, 1);
    }
}

/* One pass of the stream's thread at now_ns: the frames due within LV_LEAD_MS sent, a frame late by more than a second
 * dropped, {"p":0} twice a second when nothing comes. Returns the frames sent. Under lv_mu. */
static int lv_pump(lvtap *L, long long now_ns) {
    const unsigned long long w = atomic_load_explicit(&L->w, memory_order_acquire);
    if (w - L->r > LV_RING) L->r = w - LV_RING;           /* overrun (nobody read for 8 s): the oldest gone */
    int sent = 0;
    char b[512];
    while (L->r < w) {
        const lv_frame *f = &L->ring[L->r % LV_RING];
        if (f->t_ns - now_ns > (long long)LV_LEAD_MS * 1000000) break;   /* not yet */
        L->r++;
        if (now_ns - f->t_ns > 1000000000LL) continue;   /* stale */
        const int n = lv_frame_json(f, now_ns, b, sizeof b);
        if (n) { lv_send_all(L, b, (size_t)n); sent++; L->idle_said = now_ns; }
    }
    const long long last_t = w ? L->ring[(w - 1) % LV_RING].t_ns : 0;
    if (!sent && L->r == w && now_ns - last_t > 300000000LL && now_ns - L->idle_said >= 500000000LL) {
        static const char IDLE[] = "data: {\"p\":0}\n\n";
        lv_send_all(L, IDLE, sizeof IDLE - 1);
        L->idle_said = now_ns;
    }
    return sent;
}

static void *lv_main(void *arg) {
    lvtap *L = arg;
    for (;;) {
        pthread_mutex_lock(&lv_mu);
        const int any = L->nfd > 0;
        if (any) lv_pump(L, lv_now_ns());
        pthread_mutex_unlock(&lv_mu);
        usleep(any ? 5000 : 200000);
    }
    return NULL;
}

/* GET /levels (page.c): the connection fd taken (0: kept, closed by the stream when the client goes), or −1: too many
 * clients or no thread (the caller still owns fd). */
static int levels_client(lvtap *L, int fd) {
    pthread_mutex_lock(&lv_mu);
    if (L->nfd >= LV_MAX_CLIENTS) { pthread_mutex_unlock(&lv_mu); return -1; }
    if (!L->started) {
        pthread_t th;
        if (pthread_create(&th, NULL, lv_main, L)) { pthread_mutex_unlock(&lv_mu); return -1; }
        pthread_detach(th);
        L->started = 1;
    }
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    static const char H[] = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
                            "Access-Control-Allow-Origin: *\r\nConnection: keep-alive\r\n\r\nretry: 2000\n\n";
    if (send(fd, H, sizeof H - 1, LV_NOSIG) != (ssize_t)(sizeof H - 1)) { pthread_mutex_unlock(&lv_mu); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (!L->nfd) {                                        /* the first: the analysis afresh, no old frame sent */
        atomic_fetch_add_explicit(&L->gen, 1, memory_order_release);
        L->r = atomic_load(&L->w);
    }
    L->fds[L->nfd++] = fd;
    L->idle_said = 0;
    atomic_fetch_add(&L->listeners, 1);
    pthread_mutex_unlock(&lv_mu);
    return 0;
}

#endif
