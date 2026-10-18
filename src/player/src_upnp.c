/* src/player/src_upnp.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "src_plat.h"
#include "src.h"                       /* plat_src_* */
#define SRC_PLAT_UNDO
#include "src_plat.h"
#undef AUDIOLAZ_SRC_H
#include "src.h"                       /* src_* */
#include "upnp.h"

void ulog(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); fflush(stderr);
}
double umono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* ── The queue ────────────────────────────────────────────────────────────────────────────────────────────────── */
#define QW (1u << 22)                  /* words (16 MB): 5.4 s of 384 kHz stereo */
#define SEGS 64
typedef struct { uint64_t pos; int seq; unsigned rate, ch; uint64_t f0; } seg_t;
static struct {
    pthread_mutex_t m;
    pthread_cond_t cv;                 /* room in the queue, a new generation */
    int32_t *buf;
    unsigned gen;
    uint64_t lo, w, r;                 /* the generation's first word, written, read (words) */
    seg_t seg[SEGS];
    int sh, sn;                        /* the segments from the reader's (sh) on: sn of them */
    int done, any, paused;
    double started;
    /* the side roomd reads (one capture at a time) */
    int open, cut;
    unsigned crate, cch;
    unsigned long ccap, cper;          /* its buffer and the period it reads (frames) */
    long released;                     /* frames the clock released and roomd has not read */
    double t_last, frac, ratio;
} Q = { .m = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .ratio = 1.0 };
static int evp[2] = { -1, -1 };
static atomic_uint qgen;

void uq_wake(void) { if (evp[1] >= 0) { const char c = 1; if (write(evp[1], &c, 1) < 0) { } } }
unsigned uq_gen(void) { return atomic_load(&qgen); }
static seg_t *seg_at(int i) { return &Q.seg[(Q.sh + i) % SEGS]; }
/* The segment the reader is in: the last one starting at or before r (one starting exactly there wins). */
static void seg_settle(void) {
    while (Q.sn > 1 && seg_at(1)->pos <= Q.r) {
        const seg_t *a = seg_at(0), *b = seg_at(1);
        if (Q.open && (a->rate != b->rate || a->ch != b->ch)) break;   /* not across a change of format while read */
        Q.sh = (Q.sh + 1) % SEGS; Q.sn--;
    }
}
/* Where the run the reader is in ends (words): the next segment of another format, else what is written. */
static uint64_t run_end(void) {
    if (!Q.sn) return Q.w;
    const seg_t *a = seg_at(0);
    for (int i = 1; i < Q.sn; i++) { const seg_t *b = seg_at(i); if (b->rate != a->rate || b->ch != a->ch) return b->pos; }
    return Q.w;
}
unsigned uq_flush(void) {
    pthread_mutex_lock(&Q.m);
    Q.gen++;
    atomic_store(&qgen, Q.gen);
    Q.lo = Q.r = Q.w;
    Q.sh = 0; Q.sn = 0; Q.done = 0; Q.any = 0; Q.paused = 0;
    if (Q.open) Q.cut = 1;
    Q.released = 0;
    pthread_cond_broadcast(&Q.cv);
    const unsigned g = Q.gen;
    pthread_mutex_unlock(&Q.m);
    uq_wake();
    return g;
}
int uq_segment(unsigned gen, int seq, unsigned rate, unsigned ch, uint64_t f0) {
    pthread_mutex_lock(&Q.m);
    if (gen != Q.gen) { pthread_mutex_unlock(&Q.m); return -1; }
    if (Q.sn == SEGS) { Q.sh = (Q.sh + 1) % SEGS; Q.sn--; }   /* never: 64 tracks ahead */
    *seg_at(Q.sn) = (seg_t){ Q.w, seq, rate, ch, f0 };
    Q.sn++;
    Q.done = 0;
    pthread_mutex_unlock(&Q.m);
    return 0;
}
int uq_push(unsigned gen, const int32_t *pcm, long frames) {
    pthread_mutex_lock(&Q.m);
    if (gen != Q.gen || !Q.sn || !Q.buf) { pthread_mutex_unlock(&Q.m); return -1; }
    const seg_t *last = seg_at(Q.sn - 1);
    const uint64_t words = (uint64_t)frames * last->ch;
    /* ahead of the reader: 2.5 s at most; behind it, 2 s kept for src_unread; both within the queue */
    uint64_t ahead = (uint64_t)last->rate * last->ch * 5 / 2, keep = (uint64_t)last->rate * last->ch * 2;
    if (ahead > QW / 2) ahead = QW / 2;
    if (keep > QW / 2 - words) keep = QW / 2 - words;
    for (;;) {
        if (gen != Q.gen) { pthread_mutex_unlock(&Q.m); return -1; }
        const uint64_t floor_ = Q.r - Q.lo > keep ? Q.r - keep : Q.lo;
        if (Q.w + words - Q.r <= ahead + words && Q.w + words - floor_ <= QW) break;
        pthread_cond_wait(&Q.cv, &Q.m);
    }
    for (uint64_t i = 0; i < words; i++) Q.buf[(Q.w + i) & (QW - 1)] = pcm[i];
    Q.w += words;
    const int first = !Q.any;
    Q.any = 1;
    pthread_mutex_unlock(&Q.m);
    if (first) uq_wake();
    return 0;
}
void uq_done(unsigned gen) {
    pthread_mutex_lock(&Q.m);
    if (gen == Q.gen) Q.done = 1;
    pthread_mutex_unlock(&Q.m);
    uq_wake();
}
void uq_where(uq_at *a) {
    pthread_mutex_lock(&Q.m);
    seg_settle();
    a->seq = -1; a->frame = 0; a->rate = 0; a->first = 0;
    if (Q.sn) {
        /* the reader's place: what roomd read (the frames released and not read are not played yet) */
        const seg_t *s = seg_at(0);
        a->seq = s->seq; a->rate = s->rate;
        a->frame = s->f0 + (Q.r > s->pos ? (Q.r - s->pos) / s->ch : 0);
        a->first = s->f0;
        /* at the very start of the next segment of another format, the reader still in the last one's end */
    }
    a->drained = Q.done && Q.r >= Q.w;
    a->any = Q.any;
    pthread_mutex_unlock(&Q.m);
}
void uq_pause(int on) {
    pthread_mutex_lock(&Q.m);
    Q.paused = on;
    Q.t_last = umono();
    pthread_mutex_unlock(&Q.m);
    uq_wake();
}
void uq_started(void) { pthread_mutex_lock(&Q.m); Q.started = umono(); pthread_mutex_unlock(&Q.m); uq_wake(); }
int uq_reading(void) { pthread_mutex_lock(&Q.m); const int o = Q.open; pthread_mutex_unlock(&Q.m); return o; }

/* The UPnP input's side, under Q.m */
static int up_active(void) {
    if (!Q.buf || !Q.any) return 0;
    if (Q.paused && !Q.open) return 0;
    seg_settle();
    if (!Q.sn) return 0;
    if (Q.open) {
        if (Q.r >= run_end() && (Q.r < Q.w || Q.done)) return 0;   /* the run's end: another format next, or the music's end */
        return 1;
    }
    return Q.r < Q.w;
}
static void up_clock(void) {                   /* the frames due since the last call, released as far as there is music */
    const double now = umono();
    if (!Q.open) { Q.t_last = now; return; }
    if (Q.paused) { Q.t_last = now; Q.frac = 0; return; }
    double due = (now - Q.t_last) * Q.crate * Q.ratio + Q.frac;
    Q.t_last = now;
    if (due < 0) due = 0;
    long add = (long)due;
    const uint64_t e = run_end();
    const long data = e > Q.r ? (long)((e - Q.r) / Q.cch) - Q.released : 0;
    const long room = (long)Q.ccap - Q.released;
    long lim = data < room ? data : room;
    if (lim < 0) lim = 0;
    if (add > lim) { add = lim; Q.frac = 0; }     /* the music late, or roomd's side full: the clock waits */
    else Q.frac = due - (double)add;
    Q.released += add;
}

/* ── src.h ────────────────────────────────────────────────────────────────────────────────────────────────────── */
static int subs_asked = 1, plat_on = 0, plat_subs = 1;
static char what[256];
static int up_on(void) { return upnp_enabled(); }
static int is_up(int k) { return k == UPNP_K && up_on(); }

__attribute__((used)) static const char card_inputs_mark[] = "audiolaz-card-inputs: past-upnp";
static int plat_k(int k) { return plat_on && k >= 0 && (k < plat_subs || (k > UPNP_K && k < subs_asked)); }

int src_inputs(int subs) {
    subs_asked = subs;
    return up_on() && subs <= UPNP_K ? UPNP_K + 1 : subs;
}
int src_builtin(void) { return up_on(); }
void src_player_name(const char *name) { upnp_set_name(name); }
int src_option(const char *name, const char *value) { return upnp_option(name, value) || plat_src_option(name, value); }

static void inputs_conf(const char *rooms) {
    char path[1200], tmp[1200], line[256], keep[4096] = "";
    snprintf(path, sizeof path, "%s/inputs.conf", rooms);
    int had = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (atoi(line) == UPNP_K && line[0] >= '0' && line[0] <= '9' && strchr(line, '=')) { had = !strcmp(strchr(line, '=') + 1, "upnp\n"); if (had) { if (strlen(keep) + strlen(line) < sizeof keep) strcat(keep, line); } continue; }
            if (strlen(keep) + strlen(line) < sizeof keep) strcat(keep, line);
        }
        fclose(f);
    }
    if (had) return;
    snprintf(tmp, sizeof tmp, "%s/.inputs.conf.upnp", rooms);
    if (!(f = fopen(tmp, "w"))) return;
    if (!keep[0]) fputs("# audiolaz player: its inputs, for its page\n", f);
    fputs(keep, f);
    fprintf(f, "%d=upnp\n", UPNP_K);
    if (fclose(f) || rename(tmp, path)) unlink(tmp);
}

int src_init(const char *card, int subs, const char *rooms) {
    (void)subs;
    if (pipe(evp) == 0) { fcntl(evp[0], F_SETFL, O_NONBLOCK); fcntl(evp[1], F_SETFL, O_NONBLOCK); }
    int r = 0;
    plat_subs = subs_asked;
    if (up_on() && plat_subs > UPNP_K) plat_subs = UPNP_K;   /* the Loopback's subdevice UPNP_K is never read then */
    if (!card || strcmp(card, "none")) { plat_on = 1; r = plat_src_init(card, plat_subs, rooms); }
    if (up_on()) {
        Q.buf = calloc(QW, sizeof(int32_t));
        atomic_store(&qgen, Q.gen);
        if (!Q.buf || upnp_start(rooms)) ulog("UPnP input: could not start");
        else inputs_conf(rooms);
    }
    snprintf(what, sizeof what, "%s%s%s", plat_on ? plat_src_what() : "", plat_on && up_on() ? ", " : "", up_on() ? "the UPnP input on input 3" : "");
    return plat_on ? r : 0;
}
const char *src_what(void) { return what; }
int src_active(int k) {
    if (is_up(k)) { pthread_mutex_lock(&Q.m); const int a = up_active(); pthread_mutex_unlock(&Q.m); return a; }
    return plat_k(k) ? plat_src_active(k) : 0;
}
unsigned src_rate(int k) {
    if (is_up(k)) {
        pthread_mutex_lock(&Q.m);
        seg_settle();
        unsigned r = 0;
        if (Q.sn) {
            r = seg_at(0)->rate;
            if (!Q.open && Q.r >= run_end()) for (int i = 1; i < Q.sn; i++) if (seg_at(i)->pos <= Q.r) r = seg_at(i)->rate;
        }
        pthread_mutex_unlock(&Q.m);
        return r;
    }
    return plat_k(k) ? plat_src_rate(k) : 0;
}
unsigned src_channels(int k) {
    if (is_up(k)) { pthread_mutex_lock(&Q.m); seg_settle(); const unsigned c = Q.sn ? seg_at(0)->ch : 2; pthread_mutex_unlock(&Q.m); return c; }
    return plat_k(k) ? plat_src_channels(k) : 0;
}
double src_started(int k) {
    if (is_up(k)) { pthread_mutex_lock(&Q.m); const double s = Q.started; pthread_mutex_unlock(&Q.m); return s; }
    return plat_k(k) ? plat_src_started(k) : 0;
}
int src_closed(int k) { return is_up(k) ? !src_active(k) : plat_k(k) ? plat_src_closed(k) : 1; }
int src_paced(int k) { return !is_up(k) && plat_k(k) && plat_src_paced(k); }
int src_io_ended(int k) { return !is_up(k) && plat_k(k) && plat_src_io_ended(k); }   /* */
int src_event_fds(struct pollfd *fds, int max) {
    int n = plat_on ? plat_src_event_fds(fds, max) : 0;
    if (up_on() && evp[0] >= 0 && n < max) { fds[n].fd = evp[0]; fds[n].events = POLLIN; fds[n].revents = 0; n++; }
    return n;
}
void src_drain(void) {
    if (plat_on) plat_src_drain();
    char b[64];
    if (evp[0] >= 0) while (read(evp[0], b, sizeof b) > 0) { }
}
void src_shift(int k, long v) {
    if (is_up(k)) { pthread_mutex_lock(&Q.m); up_clock(); Q.ratio = 1.0 - (double)(v - 100000) * 11e-6; pthread_mutex_unlock(&Q.m); return; }
    if (plat_k(k)) plat_src_shift(k, v);
}
int src_air_connected(int port) { return plat_on ? plat_src_air_connected(port) : 0; }
int src_owns(int k) { return is_up(k) ? 1 : plat_k(k) ? plat_src_owns(k) : 0; }
int src_stop(int k) {
    if (is_up(k)) { upnp_input_stop(); return 1; }
    return plat_k(k) ? plat_src_stop(k) : 0;
}
const char *src_air_meta(const char *asked) { return plat_src_air_meta(asked); }
void src_air_item(uint32_t code) { if (plat_on) plat_src_air_item(code); }
void src_air_name(const char *name) { plat_src_air_name(name); }
int src_status(char *out, size_t len) {
    int n = plat_on ? plat_src_status(out, len) : 0;
    if (!n && len) out[0] = 0;
    if (up_on()) { const size_t l = strlen(out); if (l < len) n |= upnp_status(out + l, len - l); }
    return n;
}
void src_tick(void) { if (plat_on) plat_src_tick(); }
void src_shutdown(void) { if (up_on()) upnp_shutdown(); if (plat_on) plat_src_shutdown(); }
int src_cut(int k) {
    if (!is_up(k)) return 0;
    pthread_mutex_lock(&Q.m);
    const int c = Q.cut;
    Q.cut = 0;
    pthread_mutex_unlock(&Q.m);
    return c;
}
int src_paused(int k) {
    if (!is_up(k)) return 0;
    pthread_mutex_lock(&Q.m);
    const int p = Q.paused;
    pthread_mutex_unlock(&Q.m);
    return p;
}
void src_unread(int k, long frames) {
    if (!is_up(k) || frames <= 0) return;
    pthread_mutex_lock(&Q.m);
    if (Q.sn) {
        const seg_t *s = seg_at(0);
        const uint64_t floor_ = Q.w > QW ? Q.w - QW + (uint64_t)s->ch * 4096 : 0;   /* what the decoder may have written over */
        uint64_t back = (uint64_t)frames * s->ch, lo = s->pos > Q.lo ? s->pos : Q.lo;
        if (lo < floor_) lo = floor_;
        if (Q.r - lo < back) back = Q.r - lo;
        Q.r -= back;
        ulog("UPnP input: %.2f s read and not heard given back (played at the resume)", (double)(back / s->ch) / s->rate);
    }
    pthread_mutex_unlock(&Q.m);
}
double plat_src_volume(int k);
double src_volume(int k) { return is_up(k) ? upnp_gain() : plat_k(k) ? plat_src_volume(k) : 1.0; }
void plat_src_dac(const char *name, const char *maker, const char *model, int local);   /* */
void plat_src_latency(int k, double ms);
void src_dac(const char *name, const char *maker, const char *model, int local) { plat_src_dac(name, maker, model, local); }
void src_latency(int k, double ms) { if (plat_k(k) && !is_up(k)) plat_src_latency(k, ms); }
void plat_src_hold(int k, int ms);
void src_hold(int k, int ms) { if (plat_k(k) && !is_up(k)) plat_src_hold(k, ms); }   /* */

/* Reading */
struct src_pcm { int up; plat_src_pcm *p; };
src_pcm *src_open(int k, unsigned rate, unsigned ch, unsigned long period, unsigned periods) {
    src_pcm *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (is_up(k)) {
        pthread_mutex_lock(&Q.m);
        seg_settle();
        const int ok = Q.sn && seg_at(0)->rate == rate && seg_at(0)->ch == ch && !Q.open;
        if (ok) {
            Q.open = 1; Q.cut = 0; Q.crate = rate; Q.cch = ch; Q.ccap = period * periods; Q.cper = period; Q.released = 0; Q.t_last = umono(); Q.frac = 0;
            s->up = 1;
        }
        pthread_mutex_unlock(&Q.m);
        if (!ok) { ulog("UPnP input: opened at %u Hz, %u channels: not what is queued", rate, ch); free(s); return NULL; }
        pthread_cond_broadcast(&Q.cv);
        return s;
    }
    if (!plat_k(k) || !(s->p = plat_src_open(k, rate, ch, period, periods))) { free(s); return NULL; }
    return s;
}
/* the platform's input changing its channel count within a play (macOS: the 8-channel pipe); UPnP: never */
int src_relayout(src_pcm *s) { return s && !s->up && s->p ? plat_src_relayout(s->p) : 0; }
void src_start(src_pcm *s) { if (s->up) { pthread_mutex_lock(&Q.m); Q.t_last = umono(); pthread_mutex_unlock(&Q.m); } else plat_src_start(s->p); }
long src_avail(src_pcm *s) {
    if (!s->up) return plat_src_avail(s->p);
    pthread_mutex_lock(&Q.m);
    up_clock();
    long a = Q.released;
    /* the run's last frames, less than a period (roomd reads whole periods): said to be a period, read as they are */
    if (a > 0 && a < (long)Q.cper) {
        const uint64_t e = run_end();
        if ((e < Q.w || Q.done) && Q.r + (uint64_t)a * Q.cch >= e) a = (long)Q.cper;
    }
    pthread_mutex_unlock(&Q.m);
    return a;
}
long src_read(src_pcm *s, int32_t *pcm, unsigned long n) {
    if (!s->up) return plat_src_read(s->p, pcm, n);
    pthread_mutex_lock(&Q.m);
    up_clock();
    long m = Q.released < (long)n ? Q.released : (long)n;
    if (m <= 0) { pthread_mutex_unlock(&Q.m); return SRC_AGAIN; }
    const uint64_t words = (uint64_t)m * Q.cch;
    for (uint64_t i = 0; i < words; i++) pcm[i] = Q.buf[(Q.r + i) & (QW - 1)];
    Q.r += words;
    Q.released -= m;
    seg_settle();
    pthread_mutex_unlock(&Q.m);
    pthread_cond_broadcast(&Q.cv);
    return m;
}
int src_delay(src_pcm *s, long *frames) {
    if (!s->up) return plat_src_delay(s->p, frames);
    pthread_mutex_lock(&Q.m);
    *frames = Q.released;
    pthread_mutex_unlock(&Q.m);
    return 0;
}
void src_recover(src_pcm *s, long err) { if (!s->up) plat_src_recover(s->p, err); }
int src_poll_fds(src_pcm *s, struct pollfd *fds, int max) { return s->up ? 0 : plat_src_poll_fds(s->p, fds, max); }
void src_close(src_pcm *s) {
    if (!s) return;
    if (s->up) {
        pthread_mutex_lock(&Q.m);
        Q.open = 0; Q.released = 0;            /* what was released and not read stays queued: read at the next open */
        seg_settle();
        pthread_mutex_unlock(&Q.m);
        uq_wake();
    } else plat_src_close(s->p);
    free(s);
}
const char *src_strerror(long e) { return plat_src_strerror(e); }
