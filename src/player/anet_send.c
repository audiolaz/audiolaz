/* src/player/anet_send.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "anet.h"
#include "rt_thread.h"

#define AS_LOST_S 5.0                   /* version 2: no report this long (and none coming): the receiver lost */
/* protocol 3: the anchor moved by at most this much of a tick's frames: up +25 % (music read ahead), down 1 %
 * (slowly: 250 ms in 25 s); silence put in at twice the pace, zeros left out at +25 %; within a ms of the anchor, the
 * gentle correction alone (the DAC's clock against ours) */
#define AS_GROW 0.25
#define AS_SHRINK 0.01
#define AS_INSERT 1.0
#define AS_DEAD_MS 1.0
#define AS_LOOK_MS 5.0                  /* a silence: the frames going now, and this much after them, all exactly zero */
/* late: caught up when behind by up to AS_CATCH_S (beyond, the receiver has run dry long since: the rhythm
 * taken up again from now), AS_BURST ticks back to back at most before a tick's pause (catching up at about 7× real
 * time: the receiver's socket never flooded, 192 kHz included); music read ahead short at the receiver by more than
 * AS_DEFICIT_MS while it plays: up to AS_REFILL ticks more a tick; a delay over AS_LATE_S journaled */
#define AS_CATCH_S 1.0
#define AS_BURST 8
#define AS_DEFICIT_MS 20.0
#define AS_REFILL 3.0
#define AS_LATE_S 0.05
#define AS_TX_BUSY 0.25                 /* macOS: the timer thread's computation, a share of its period (rt_clock) */

#define AS_FIT_TAU 60.0                 /* s: the fit's memory */
#define AS_FIT_MIN 20.0                 /* reports (their weight) before the fit says anything */
#define AS_OUT_N 4.0                    /* set aside: further from the fit than this many times its spread … */
#define AS_OUT_MS 2.0                   /* … and than this (ms) */
#define AS_STEP_N 6                     /* this many set aside in a row: a step, the fit started again */
#define AS_KF_SLEW 20e-6                /* the clock moved at most this much a second */
#define AS_KF_LIMIT 1000e-6             /* never further from ours than this */
#define AS_PULL_MAX 40e-6               /* the ring's pull: at most this */
#define AS_PULL_FAR 300e-6              /* a source read at our pace, its ring far off its anchor (on average): up to this */
#define AS_PULL_SLEW 10e-6              /* the pull moved at most this much a second */
#define AS_PERR_S 2.0                   /* the ring's error averaged over about this long for the pull */
#ifdef MSG_NOSIGNAL
#define MSG_NOSIGNAL_ MSG_NOSIGNAL
#else
#define MSG_NOSIGNAL_ 0
#endif
#ifndef ANET_LOG
#define ANET_LOG nlog                   /* netout.c's journal */
#endif

static double as_mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static pthread_mutex_t as_lk = PTHREAD_MUTEX_INITIALIZER, as_sendlk = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int on, fd, ufd, lost, ended, end_sent, paused, full_once, playing, locked, stop_tx;
    unsigned id, rate, pkt_frames, pkt_us, ring_target;
    int32_t *q; uint64_t qcap, qw, qr;  /* the queue (frames): roomd writes qw, the timer thread reads qr */
    uint64_t rep_played; uint32_t rep_ring;
    double t_rep, p0, t0, k;            /* the DAC's clock: played ≈ p0 + (t − t0)·rate·k while playing */
    double close_by, t_open;
    pthread_t th, tx;
    char why[200];
    /* protocol 3: what the receiver follows (ANET_CAP_*, 0: version 2); the stream's frames sent (sw: the
     * queue's qr, plus the silence put in, less the zeros left out: ins); what its DAC read at its last report; the
     * anchor (frames: the nominal ring held) and the target it asks for; any music sent yet, the last frame sent zero */
    unsigned caps;
    int music, last_zero;
    uint64_t sw, rep_read; int64_t ins;
    double anchor; uint32_t goal, start;
    uint32_t late_us, peak_us, r_underruns, r_adjusts, r_min, r_max, rstate, rwhy;
    double goal_lo, goal_hi;            /* the target's range this stream (frames) */
    uint64_t put_in, left_out, sped, slowed;   /* frames: silence put in, zeros left out, sent ahead, held back */
    unsigned ping_seq; double t_ping;   /* the pings */
    unsigned stop_why; char stop_text[160];    /* anet_stop_reason: why the next close */
    unsigned meta_sent;                 /* the meta's serial told */
    int r_stopped;                      /* the receiver stopped the stream itself (ANET_R_STOPPED) */
    double after;                       /* heard after the receiver's DAC (its reports' after: frames at our rate) */
    double pull;                        /* the ring's pull on the pace now (ppm/1e6: + faster), the DAC's being k */
    uint64_t left_over, put_back;       /* live: zeros left out above the receiver's target, put in below the anchor */
    double n_avg, t_navg;               /* the nominal ring averaged over about 2 s (frames): what over and under weigh;
                                         * since when (it plays: weighed from 3 s after) */
    double k_sum, pull_sum; long kp_n;  /* the DAC's clock and the pull, summed each tick while playing (their means said) */
    /* the DAC's clock fitted (as_fit): its reports (t, played - rate·t) by least squares, weighted
     * exp(-age/AS_FIT_TAU); a report far off the fit set aside (rep_out); the clock k that both the pace and the
     * estimates follow, the fit's slope weighed against what was known before (kf0, its variance kf_p), moved
     * AS_KF_SLEW at most; the ring's error to its anchor averaged for the pull (perr) */
    double f_s0, f_sx, f_sy, f_sxx, f_sxy, f_t, f_sig, f_runsum; int f_run;
    double kf0, kf_p, kf_var, t_kf, perr; int perr_on;
    unsigned long rep_out, rep_steps;
    /* the timer thread's delays (AS_LATE_S and more): kept here by it, journaled by the control's thread */
    int rt_err, rt_said;                /* its priority refused (errno), said */
    unsigned long late_n[4]; double late_max[4];   /* by cause (AS_L_*): how many, the worst (s) */
    struct { int kind; double s, ring_ms; } late_q[8]; unsigned late_w, late_r;   /* to be journaled */
    char hp[160];                       /* the receiver: host:port */
} AS = { .fd = -1, .ufd = -1 };
enum { AS_L_WOKE, AS_L_LOCK, AS_L_SEND, AS_L_EMPTY };
static const char *const as_late_text[4] = {
    "the packets' thread woke late (not given a CPU in time: the machine busy)",
    "the packets' thread waited for its lock (another of roomd's threads held it, held up itself)",
    "a send held the packets' thread (the system's network stack)",
    "nothing to send: the music not made in time (roomd's convolution or writer, or its source, late)" };
static unsigned as_ids = 0;
static int as_rt_prio = 60;             /* anet_rt */
static int as_live_next = 0;            /* anet_live: the source's kind (read at each tick) */
static int as_paced_next = 0;           /* anet_paced: the source read at our pace (a card: Roon follows it) */
static unsigned as_caps_offer = ANET_CAP_ALL;   /* anet_caps: what anet_open offers */
/* who we are (anet_identity), the next open a take-over (anet_take), the last open refused: who plays there */
static char as_uid[40], as_name[96] = "audiolaz";
static int as_take_next = 0, as_busy = 0;
static char as_busy_by[128];
/* what plays (anet_meta) */
static pthread_mutex_t as_metalk = PTHREAD_MUTEX_INITIALIZER;
static char as_meta_lines[ANET_META_MAX]; static unsigned as_meta_serial;
/* what each receiver said last (by host:port): its target and start, its state, its jitter, the last stop */
#define AS_KNOWN 8
static struct {
    char hp[160]; unsigned rate, rstate, rwhy; double target_ms, start_ms, late_ms, peak_ms, lo_ms, hi_ms, t_seen;
    unsigned long underruns, adjusts; char last_stop[200]; double t_stop;
    double kf; int has_kf;              /* its DAC's clock as the last stream fitted it (the next one starts there) */
} as_known[AS_KNOWN];
/* the packets' rhythm as sent (the journal at each stream's end): gaps between two sends, 25 µs buckets */
#define AS_HB 200
static struct { unsigned long n[AS_HB], cnt; double sum, max; } AS_H;
static double as_hpct(double p) {
    unsigned long want = (unsigned long)(p * AS_H.cnt), c = 0;
    for (int b = 0; b < AS_HB; b++) { c += AS_H.n[b]; if (c > want) return b * 25.0 + 12.5; }
    return AS_HB * 25.0;
}

static void as_setwhy(const char *f, ...) { va_list a; va_start(a, f); vsnprintf(AS.why, sizeof AS.why, f, a); va_end(a); }
const char *anet_why(void) { return AS.why; }
/* the stream now lost within s seconds of its opening: 1 (the reason in anet_why; the receiver's own, if it
 * said one, in *rwhy: ANET_WHY_*, 0 none) */
int anet_lost_early(double s, unsigned *rwhy) {
    pthread_mutex_lock(&as_lk);
    const int q = AS.on && AS.lost && as_mono() - AS.t_open < s;
    if (rwhy) *rwhy = AS.r_stopped || AS.rstate == ANET_RS_DAC_FAILED ? AS.rwhy : 0;
    pthread_mutex_unlock(&as_lk);
    return q;
}
int anet_active(void) { pthread_mutex_lock(&as_lk); const int on = AS.on; pthread_mutex_unlock(&as_lk); return on; }
void anet_live(int on) { pthread_mutex_lock(&as_lk); as_live_next = !!on; pthread_mutex_unlock(&as_lk); }
void anet_paced(int on) { pthread_mutex_lock(&as_lk); as_paced_next = !!on; pthread_mutex_unlock(&as_lk); }

static volatile int as_fifo_ms = ANET_FIFO_MS;
void anet_fifo_ms(int ms) { as_fifo_ms = ms <= 0 ? ANET_FIFO_MS : ms < 10 ? 10 : ms > ANET_FIFO_MS ? ANET_FIFO_MS : ms; }
void anet_caps(unsigned caps) { pthread_mutex_lock(&as_lk); as_caps_offer = caps & (ANET_CAP_ALL | ANET_CAP_RATES); pthread_mutex_unlock(&as_lk); }
void anet_convert(int on) { pthread_mutex_lock(&as_lk); as_caps_offer = on ? as_caps_offer | ANET_CAP_RATES : as_caps_offer & ~(unsigned)ANET_CAP_RATES; pthread_mutex_unlock(&as_lk); }
static unsigned as_refused_e = 0;     /* the last anet_open's refusal (ANET_E_*) */
unsigned anet_refused(void) { pthread_mutex_lock(&as_lk); const unsigned e = as_refused_e; pthread_mutex_unlock(&as_lk); return e; }
void anet_rt(int prio) { pthread_mutex_lock(&as_lk); as_rt_prio = prio > 0 ? prio : 0; pthread_mutex_unlock(&as_lk); }
/* as_lk priority-inheriting (Linux) before anything takes it: the timer thread shares it with the writer, the
 * control's thread and the page's (anet_get_state) */
__attribute__((constructor)) static void as_init_locks(void) { rt_mutex_pi(&as_lk); }
/* under as_lk: a delay of the timer thread (s), its cause, what the receiver held then (ms; <0 unknown) */
static void as_late_note(int kind, double s, double ring_ms) {
    AS.late_n[kind]++;
    if (s > AS.late_max[kind]) AS.late_max[kind] = s;
    if (AS.late_w - AS.late_r < 8) { const unsigned i = AS.late_w++ & 7; AS.late_q[i].kind = kind; AS.late_q[i].s = s; AS.late_q[i].ring_ms = ring_ms; }
}
void anet_identity(const char *uid, const char *name) {
    pthread_mutex_lock(&as_lk);
    snprintf(as_uid, sizeof as_uid, "%s", uid ? uid : "");
    snprintf(as_name, sizeof as_name, "%s", name && *name ? name : "audiolaz");
    pthread_mutex_unlock(&as_lk);
}
void anet_take(int on) { pthread_mutex_lock(&as_lk); as_take_next = !!on; pthread_mutex_unlock(&as_lk); }
int anet_busy(char *by, size_t len) {
    pthread_mutex_lock(&as_lk);
    const int b = as_busy;
    if (by && len) snprintf(by, len, "%s", b ? as_busy_by : "");
    pthread_mutex_unlock(&as_lk);
    return b;
}
void anet_stop_reason(unsigned why, const char *text) {
    pthread_mutex_lock(&as_lk);
    AS.stop_why = why; snprintf(AS.stop_text, sizeof AS.stop_text, "%s", text ? text : "");
    pthread_mutex_unlock(&as_lk);
}
void anet_meta(const char *lines, unsigned serial) {
    pthread_mutex_lock(&as_metalk);
    snprintf(as_meta_lines, sizeof as_meta_lines, "%s", lines ? lines : "");
    as_meta_serial = serial;
    pthread_mutex_unlock(&as_metalk);
}

/* under as_lk: this receiver's row (made when absent: the oldest given up) */
static int as_known_at(const char *hp, int make) {
    int k = -1, old = 0;
    for (int i = 0; i < AS_KNOWN; i++) { if (as_known[i].hp[0] && !strcmp(as_known[i].hp, hp)) return i; if (as_known[i].t_seen < as_known[old].t_seen) old = i; }
    if (!make) return -1;
    k = old;
    memset(&as_known[k], 0, sizeof as_known[k]);
    snprintf(as_known[k].hp, sizeof as_known[k].hp, "%s", hp);
    as_known[k].t_stop = -1;
    return k;
}
static void as_known_note(void) {   /* under as_lk: the stream's figures into its receiver's row */
    const int k = as_known_at(AS.hp, 1);
    const double ms = 1000.0 / (AS.rate ? AS.rate : 1);
    as_known[k].rate = AS.rate; as_known[k].rstate = AS.rstate; as_known[k].rwhy = AS.rwhy;
    as_known[k].target_ms = AS.goal * ms; as_known[k].start_ms = AS.start * ms;
    as_known[k].late_ms = AS.late_us / 1000.0; as_known[k].peak_ms = AS.peak_us / 1000.0;
    as_known[k].lo_ms = AS.goal_lo * ms; as_known[k].hi_ms = AS.goal_hi * ms;
    as_known[k].underruns = AS.r_underruns; as_known[k].adjusts = AS.r_adjusts;
    as_known[k].t_seen = as_mono();
}

/* under as_lk: what the DAC has read by now (the stream's frames: its last report, and since at its clock) */
static double as_read_est(double t) {
    double r = (double)AS.rep_read;
    if (AS.playing && !AS.paused && AS.locked) r += (t - AS.t_rep) * AS.rate * AS.k;
    if (r > (double)AS.sw) r = (double)AS.sw;
    return r;
}
static double as_est_played(double t) {   /* under as_lk */
    double p = AS.p0;
    if (AS.playing && !AS.paused && AS.locked) p += (t - AS.t0) * AS.rate * AS.k;
    if (p < (double)AS.rep_played) p = (double)AS.rep_played;
    if (p > (double)AS.sw) p = (double)AS.sw;          /* never more than was sent (the stream's frames, 14/10) */
    return p;
}
/* under as_lk: the fit started again (a step in the reports, a pause, the DAC stopped): the clock kept, known
 * this well (its variance; no better than (10 ppm)², no worse than before) */
static void as_fit_restart(void) {
    AS.f_s0 = AS.f_sx = AS.f_sy = AS.f_sxx = AS.f_sxy = 0; AS.f_t = 0; AS.f_run = 0; AS.f_runsum = 0;
    AS.kf0 = AS.k;
    if (AS.kf_var > 0) { const double v = fmax(4.0 * AS.kf_var, 1e-10); if (v < AS.kf_p) AS.kf_p = v; }
    AS.kf_var = 0; AS.t_kf = 0;
}
/* under as_lk: a report into the fit (x: s since the stream opened, y: what it played less rate·x, frames);
 * 0 taken, 1 set aside (far off the fit), 2 a step (AS_STEP_N set aside in a row: the fit started again); the clock
 * AS.k moved toward what the fit says, weighed by how well each knows it */
static int as_fit(double x, double y, double t) {
    const double rate = AS.rate;
    if (AS.f_t > 0 && t > AS.f_t) {
        const double d = exp(-(t - AS.f_t) / AS_FIT_TAU);
        AS.f_s0 *= d; AS.f_sx *= d; AS.f_sy *= d; AS.f_sxx *= d; AS.f_sxy *= d;
    }
    AS.f_t = t;
    if (AS.f_sig <= 0) AS.f_sig = 0.0005 * rate;
    int out = 0;
    if (AS.f_s0 >= AS_FIT_MIN) {
        const double mx = AS.f_sx / AS.f_s0, my = AS.f_sy / AS.f_s0, vx = AS.f_sxx / AS.f_s0 - mx * mx;
        if (vx > 1e-6) {
            const double b = (AS.f_sxy / AS.f_s0 - mx * my) / vx;
            const double res = y - (my + b * (x - mx)), lim = fmax(AS_OUT_N * AS.f_sig, AS_OUT_MS / 1000.0 * rate);
            if (fabs(res) > lim) {
                out = 1; AS.rep_out++;
                AS.f_sig += (lim / AS_OUT_N * 1.5 - AS.f_sig) * 0.02;   /* the spread grown slowly when they keep coming */
                AS.f_runsum += res;
                if (++AS.f_run >= AS_STEP_N) { AS.rep_steps++; as_fit_restart(); return 2; }
            } else { AS.f_run = 0; AS.f_runsum = 0; AS.f_sig += (fabs(res) * 1.25 - AS.f_sig) * 0.02; }
        }
    }
    if (!out) { AS.f_s0 += 1; AS.f_sx += x; AS.f_sy += y; AS.f_sxx += x * x; AS.f_sxy += x * y; }
    double want = AS.kf0;
    if (AS.f_s0 >= AS_FIT_MIN) {
        const double mx = AS.f_sx / AS.f_s0, my = AS.f_sy / AS.f_s0, vx = AS.f_sxx / AS.f_s0 - mx * mx;
        if (vx > 1e-6) {
            const double b = (AS.f_sxy / AS.f_s0 - mx * my) / vx;
            const double sd = AS.f_sig / 1.25 / rate, v = sd * sd / (AS.f_s0 * vx), w = AS.kf_p / (AS.kf_p + v);
            want = AS.kf0 + (1.0 + b / rate - AS.kf0) * w;
            AS.kf_var = AS.kf_p * v / (AS.kf_p + v);
        }
    }
    if (want > 1.0 + AS_KF_LIMIT) want = 1.0 + AS_KF_LIMIT;
    if (want < 1.0 - AS_KF_LIMIT) want = 1.0 - AS_KF_LIMIT;
    if (AS.t_kf > 0) {
        const double m = AS_KF_SLEW * (t - AS.t_kf);
        AS.k += fmax(-m, fmin(m, want - AS.k));
    }
    AS.t_kf = t;
    return out;
}
static void as_on_report(const anet_rep *r, double t) {   /* under as_lk */
    if (r->id != AS.id) return;
    AS.t_rep = t;
    if (r->v3) {                        /* protocol 3: what the receiver asks for, does and measures */
        if (AS.caps & ANET_CAP_ADAPT) {
            AS.goal = r->target;
            if (r->target < AS.goal_lo || AS.goal_lo == 0) AS.goal_lo = r->target;
            if (r->target > AS.goal_hi) AS.goal_hi = r->target;
        }
        AS.late_us = r->late_us; AS.peak_us = r->peak_us; AS.r_underruns = r->underruns; AS.r_adjusts = r->adjusts;
        AS.r_min = r->ring_min; AS.r_max = r->ring_max; AS.rstate = r->state; AS.rwhy = r->why;
        AS.after = (double)r->after_us * AS.rate / 1e6;   /* another player behind it: its latency */
        as_known_note();
    }
    if (r->flags & ANET_R_STOPPED) {    /* it ended the stream itself: its reason */
        if (!AS.lost) { AS.lost = 1; AS.r_stopped = 1; as_setwhy("the receiver stopped the stream: %s", anet_why_text(r->why)); }
        return;
    }
    if (r->flags & ANET_R_FAILED) {
        if (!AS.lost) { AS.lost = 1; if (r->v3 && r->why) { AS.rwhy = r->why; as_setwhy("the receiver's DAC failed: %s", anet_why_text(r->why)); } else as_setwhy("the receiver's DAC failed"); }
        return;
    }
    const int playing = !!(r->flags & ANET_R_PLAYING);
    AS.rep_played = r->played; AS.rep_ring = r->ring;
    AS.rep_read = r->received > r->ring ? r->received - r->ring : 0;
    if (!playing || !AS.locked) { AS.p0 = (double)r->played; AS.t0 = t; AS.locked = playing; AS.playing = playing; as_fit_restart(); return; }
    const double pred = AS.p0 + (t - AS.t0) * AS.rate * (AS.paused ? 0 : AS.k), e = (double)r->played - pred;
    /* the clock from the fit (as_fit), never from this report alone; the position as before, a report set
     * aside left out of it too (the position as predicted at the clock) */
    const int f = AS.paused ? 0 : as_fit(t - AS.t_open, (double)r->played - AS.rate * (t - AS.t_open), t);
    if (f == 2 || e > 0.1 * AS.rate || e < -0.1 * AS.rate) { AS.p0 = (double)r->played; AS.t0 = t; if (f != 2) as_fit_restart(); return; }
    if (f == 1) return;
    AS.p0 = pred + 0.25 * e; AS.t0 = t;
}

static int as_send_all(int fd, const void *p, size_t n) {
    const uint8_t *c = p;
    while (n) {
        const ssize_t w = send(fd, c, n, MSG_NOSIGNAL_);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        c += w; n -= (size_t)w;
    }
    return 0;
}
/* a message (len bytes of payload after its 8): never one with a payload to a version-2 receiver (the callers) */
static int as_send_msg2(uint32_t type, const void *pay, uint32_t len) {
    pthread_mutex_lock(&as_sendlk);
    uint8_t h[8];
    anet_msg_pack(h, type, len);
    const int r = AS.fd < 0 || as_send_all(AS.fd, h, 8) || (len && as_send_all(AS.fd, pay, len)) ? -1 : 0;
    pthread_mutex_unlock(&as_sendlk);
    return r;
}
static int as_send_msg(uint32_t type) { return as_send_msg2(type, NULL, 0); }
static int as_send_why(uint32_t type, unsigned why) {   /* END / STOP: with its reason to a protocol-3 receiver */
    pthread_mutex_lock(&as_lk);
    const int st = !!(AS.caps & ANET_CAP_STATE);
    pthread_mutex_unlock(&as_lk);
    if (!st) return as_send_msg(type);
    uint8_t w[4]; anet_p32(w, why);
    return as_send_msg2(type, w, 4);
}

/* What plays (anet_meta) sent now, its serial noted: by the control's thread when it changes, and once as the
 * stream opens, before its first packet (the Mac's uplink says its volume there: the room's corrector applies it from
 * the stream's first frame, never a first moment at full volume) */
static void as_meta_now(int fd) {
    char m[ANET_META_MAX]; unsigned ser;
    pthread_mutex_lock(&as_metalk); memcpy(m, as_meta_lines, sizeof m); ser = as_meta_serial; pthread_mutex_unlock(&as_metalk);
    char *self = strstr(m, "{self}");          /* our address as the receiver reaches it (this connection's own) */
    if (self) {
        struct sockaddr_in me; socklen_t ml = sizeof me; char ip[INET_ADDRSTRLEN] = "", rest[ANET_META_MAX];
        if (!getsockname(fd, (struct sockaddr *)&me, &ml)) inet_ntop(AF_INET, &me.sin_addr, ip, sizeof ip);
        snprintf(rest, sizeof rest, "%s", self + 6);
        snprintf(self, sizeof m - (size_t)(self - m), "%s%s", ip, rest);
    }
    if (!as_send_msg2(ANET_M_META, m, (uint32_t)strlen(m))) { pthread_mutex_lock(&as_lk); AS.meta_sent = ser; pthread_mutex_unlock(&as_lk); }
}

/* The control's own thread: the receiver's reports; (protocol 3) the pings, the questions, what plays */
static void *as_rx_main(void *arg) {
    const int fd = (int)(intptr_t)arg;
    uint8_t b[ANET_REP3];
    size_t have = 0;
    unsigned asks = 0;
    rt_interactive();                   /* the reports and the pings on time (macOS: the interactive band) */
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        pthread_mutex_lock(&as_lk);
        const int p3 = !!(AS.caps & ANET_CAP_STATE);
        pthread_mutex_unlock(&as_lk);
        const int pr = poll(&p, 1, p3 ? 100 : 500);
        const double t = as_mono();
        int ping = 0, meta = 0; uint8_t pb[8];
        int said_n = 0, rt_err = 0, said_kind[8]; double said_s[8], said_ring[8];
        pthread_mutex_lock(&as_lk);
        const int mine = AS.fd == fd && !(AS.close_by > 0 && t > AS.close_by);
        if (mine) {                     /* the timer thread's delays, journaled here (outside the lock, below) */
            for (; AS.late_r != AS.late_w && said_n < 8; AS.late_r++, said_n++) {
                const unsigned i = AS.late_r & 7;
                said_kind[said_n] = AS.late_q[i].kind; said_s[said_n] = AS.late_q[i].s; said_ring[said_n] = AS.late_q[i].ring_ms;
            }
            if (AS.rt_err && !AS.rt_said) { AS.rt_said = 1; rt_err = AS.rt_err; }
        }
        if (mine && AS.on && !AS.lost) {
            if (!p3) {                  /* version 2, as before: nothing heard for AS_LOST_S, nothing coming */
                if (pr == 0 && AS.t_rep > 0 && t - AS.t_rep > AS_LOST_S) { AS.lost = 1; as_setwhy("no report from the receiver for %.0f s", AS_LOST_S); }
            } else {
                /* a ping every ANET_PING_MS, each a question once no report came for ANET_ASK_MS (the receiver
                 * answers it at once, from its control thread); lost only after ANET_LOST_MS of questions unanswered */
                const double quiet = t - AS.t_rep;
                if (t - AS.t_ping >= ANET_PING_MS / 1000.0) {
                    AS.t_ping = t; ping = 1;
                    const unsigned ss = AS.ended ? ANET_SS_ENDING : AS.paused ? ANET_SS_PAUSED : AS.playing ? ANET_SS_PLAYING : ANET_SS_STARTING;
                    anet_p32(pb, ++AS.ping_seq); anet_p32(pb + 4, ss | (as_live_next ? ANET_SS_LIVE : 0));
                    if (quiet > ANET_ASK_MS / 1000.0) asks++; else asks = 0;
                }
                /* never while its answers wait to be read (this thread itself held up past ANET_LOST_MS) */
                if (quiet > ANET_LOST_MS / 1000.0 && pr == 0) { AS.lost = 1; as_setwhy("no answer from the receiver for %.1f s (%u state requests)", quiet, asks); ping = 0; }
                pthread_mutex_lock(&as_metalk);
                meta = as_meta_serial != AS.meta_sent;
                pthread_mutex_unlock(&as_metalk);
            }
        }
        pthread_mutex_unlock(&as_lk);
        if (!mine) break;
        if (rt_err) ANET_LOG("audiolaz: the packets' thread at normal priority (%s refused: %s)",
#ifdef __APPLE__
                             "a Mach time constraint",
#else
                             "SCHED_FIFO",
#endif
                             strerror(rt_err));
        for (int i = 0; i < said_n; i++) {
            char ring[64] = "";
            if (said_ring[i] >= 0) snprintf(ring, sizeof ring, "; the receiver held %.0f ms then", said_ring[i]);
            ANET_LOG("audiolaz: the packets %.0f ms late: %s%s", said_s[i] * 1000, as_late_text[said_kind[i]], ring);
        }
        if (ping) as_send_msg2(ANET_M_PING, pb, 8);
        if (meta) as_meta_now(fd);
        if (pr <= 0) continue;
        const ssize_t n = recv(fd, b + have, sizeof b - have, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            pthread_mutex_lock(&as_lk);
            if (AS.fd == fd && !AS.lost) { AS.lost = 1; as_setwhy("the receiver closed the connection%s", n < 0 ? " (an error)" : ""); }
            pthread_mutex_unlock(&as_lk);
            break;
        }
        have += (size_t)n;
        /* AZR1 (40 bytes) or protocol 3's AZR3 (80): told by the magic; anything else skipped a byte at a time */
        while (have >= 4) {
            const size_t need = anet_rep_len(b);
            if (!need) { memmove(b, b + 1, --have); continue; }
            if (have < need) break;
            anet_rep r;
            const int bad = anet_rep_parse(b, &r);
            memmove(b, b + need, have - need); have -= need;
            if (bad) continue;
            pthread_mutex_lock(&as_lk);
            as_on_report(&r, as_mono());
            pthread_mutex_unlock(&as_lk);
        }
    }
    return NULL;
}

/* ── The timer thread: a packet every pkt_us, steady ── */
static void as_sleep_until(double t) {
    const double d = t - as_mono();
    if (d <= 0) return;
    struct timespec s = { (time_t)d, (long)((d - (double)(time_t)d) * 1e9) };
#ifdef __linux__
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    struct timespec at = { now.tv_sec + s.tv_sec, now.tv_nsec + s.tv_nsec };
    if (at.tv_nsec >= 1000000000L) { at.tv_sec++; at.tv_nsec -= 1000000000L; }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, NULL) == EINTR) {}
#else
    nanosleep(&s, NULL);
#endif
}
/* under as_lk: the queue's n frames from its frame `from`, all exactly zero */
static int as_q_zero(uint64_t from, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) { const uint64_t at = (from + i) & AS.qcap; if (AS.q[2 * at] | AS.q[2 * at + 1]) return 0; }
    return 1;
}
static void *as_tx_main(void *arg) {
    (void)arg;
#if defined(__linux__) && defined(_GNU_SOURCE)
    pthread_setname_np(pthread_self(), "az-link-tx");  /* named in the journal's real-time lines, ps -L */
#endif
    static uint8_t pkt[ANET_DHDR + ANET_PKT_FRAMES * 8];
    static const int32_t zero2[2] = { 0, 0 };
    pthread_mutex_lock(&as_lk);
    const double T = AS.pkt_us / 1e6;
    const unsigned F = AS.pkt_frames, rate = AS.rate, id = AS.id;
    const int ufd = AS.ufd, prio = as_rt_prio;
    pthread_mutex_unlock(&as_lk);

    if (prio > 0) { const int e = rt_clock(T, AS_TX_BUSY, prio); if (e) { pthread_mutex_lock(&as_lk); AS.rt_err = e; pthread_mutex_unlock(&as_lk); } }
    const double tick = rate * T, dead = rate * AS_DEAD_MS / 1000.0, deficit = rate * AS_DEFICIT_MS / 1000.0;
    const uint64_t look = (uint64_t)(rate * AS_LOOK_MS / 1000.0);
    double next = as_mono(), acc = 0, t_sent = 0, prev_lag = 0, t_short = 0, dry_s = 0;
    int burst = 0;
    rt_guard tg = { 0 };                                /* (21/10, rt_thread.h) never a core held at real time */
    double tspin_for = 0, tspin_at = rt_test_spin("tx", &tspin_for);   /* the tests' (test/rt-safety.sh) */
    const double t_tspin0 = as_mono();
    memset(&AS_H, 0, sizeof AS_H);
    for (;;) {
        if (prio > 0) rt_guard_step(&tg, 0.005, 0.0002);
        if (tspin_at > 0 && as_mono() - t_tspin0 >= tspin_at) { tspin_at = 0; rt_test_spin_now(tspin_for); next = as_mono(); }
        next += T;
        /* behind (woken late): the ticks due run back to back, a tick's pause every AS_BURST of them */
        if (as_mono() < next) { burst = 0; as_sleep_until(next); }
        else if (++burst >= AS_BURST) { burst = 0; as_sleep_until(as_mono() + T); }
        const double t = as_mono();
        double lag = t - next;
        /* how late this wake was, beyond the lag already there (s): a delay of its own when over AS_LATE_S */
        const double woke = lag - (prev_lag > 0 ? prev_lag : 0);
        if (lag > AS_CATCH_S) { next = t; lag = 0; }   /* far behind: the receiver ran dry long since, the rhythm taken up again */
        prev_lag = lag;
        const int catching = lag > T;                  /* catching up: the ticks' own frames refill the receiver */
        pthread_mutex_lock(&as_lk);
        const double t_locked = as_mono();
        if (AS.stop_tx) { pthread_mutex_unlock(&as_lk); break; }
        if (AS.paused || AS.lost) { acc = 0; t_short = 0; pthread_mutex_unlock(&as_lk); continue; }
        const int adapt = !!(AS.caps & ANET_CAP_ADAPT), live = as_live_next;
        /* the DAC's rate (k), and the receiver's ring pulled gently to its target: 10 ms off → 25 ppm, ±300 at most;
         * protocol 3: its nominal ring (in flight counted as come) to the anchor */
        double ratio = AS.k, N = 0;           /* the fitted clock (as_fit): the last stream's to start with */
        if (adapt) {
            N = (double)AS.sw - as_read_est(t);
            if (AS.playing) {
                /* (22/10, the FOCALE voie: its card at −267 ppm from Roon's clock, the receiver's ring ~450 ms above
                 * where it was kept: this pull at its ±300 ppm limit for half an hour) a source read at our pace (a
                 * card: the app that plays to it follows its clock, nothing resampled) pulled up to ±1000 ppm; the rest
                 * taken out, or put back, in silences (below) */
                /* its error averaged (AS_PERR_S: the DAC's periods and the reports' jitter move N by
                 * milliseconds from one tick to the next), the pull ±AS_PULL_MAX, moved slowly: the pace never more
                 * than that from the DAC's clock; a card's app following us, its ring far off its anchor on
                 * average: up to ±AS_PULL_FAR, reached as slowly */
                const double err = AS.anchor - N;
                if (!AS.perr_on) { AS.perr = err; AS.perr_on = 1; }
                else AS.perr += (err - AS.perr) * fmin(1.0, T / AS_PERR_S);
                const double far_ = fmax(2.0 * deficit, 0.2 * (double)AS.goal);
                const double cmax = as_paced_next && fabs(AS.perr) > far_ ? AS_PULL_FAR : AS_PULL_MAX;
                double c = AS.perr / rate / 400.0;
                if (c > cmax) c = cmax;
                if (c < -cmax) c = -cmax;
                const double cs = AS_PULL_SLEW * T;
                if (c > AS.pull + cs) c = AS.pull + cs;
                if (c < AS.pull - cs) c = AS.pull - cs;
                ratio += c;
                AS.pull = c;
                if (AS.locked && t - AS.t_open > 10.0 && AS.kf_var > 0 && AS.kf_var < 4e-10) { AS.k_sum += AS.k; AS.pull_sum += c; AS.kp_n++; }
            } else { AS.pull = 0; AS.perr_on = 0; }
        } else if (AS.playing && AS.ring_target) {
            double c = -((double)AS.rep_ring - (double)AS.ring_target) / rate / 400.0;
            if (c > AS_PULL_MAX) c = AS_PULL_MAX;            /* as protocol 3's */
            if (c < -AS_PULL_MAX) c = -AS_PULL_MAX;
            ratio += c;
        }
        /* its delays noted (journaled by the control's thread): with what the receiver held then */
        const double ring_ms = AS.playing ? (adapt ? N : (double)AS.rep_ring) * 1000.0 / rate : -1;
        if (woke > AS_LATE_S) as_late_note(AS_L_WOKE, woke, ring_ms);
        if (t_locked - t > AS_LATE_S) as_late_note(AS_L_LOCK, t_locked - t, ring_ms);
        acc += tick * ratio;
        const uint64_t have = AS.qw - AS.qr;
        long n = (long)acc;                               /* the DAC's share of this tick, from the queue */
        if (n > (long)(2 * F)) n = (long)(2 * F);
        /* the queue short of the DAC's share while the receiver plays the music: since when; over, a delay */
        const int short_ = AS.playing && AS.music && !AS.ended && (uint64_t)n > have;
        if (short_ && !t_short) t_short = t;
        else if (!short_ && t_short) { dry_s = t - t_short; if (dry_s > AS_LATE_S) as_late_note(AS_L_EMPTY, dry_s, ring_ms); t_short = 0; }
        if (n > (long)have) n = (long)have;              /* roomd late: what there is; the rest next time (acc kept) */
        if (acc > 4.0 * F) acc = 4.0 * F;
        const long n_base = n;
        long zeros_in = 0, skip = 0;
        double owed_s = 0;                                /* music read ahead short at the receiver: how late (s) */
        if (adapt && !AS.ended) {
            const double G = AS.goal;
            if (!live) {                                  /* music read ahead: the anchor to the target, the queue sent faster or slower */
                if (G > AS.anchor) { AS.anchor += AS_GROW * tick; if (AS.anchor > G) AS.anchor = G; }
                else if (G < AS.anchor) { AS.anchor -= AS_SHRINK * tick; if (AS.anchor < G) AS.anchor = G; }
                const double d = AS.anchor - N;
                long x = 0;
                /* how late the refill is: the receiver's deficit, or the queue's dry spell when longer (the
                 * deficit stops at the anchor once the receiver ran dry: 50 ms said for 150, the bench, a second gap) */
                if (AS.playing && d > deficit) owed_s = fmax(d / rate, dry_s); else dry_s = 0;
                /* short of the anchor by more than AS_DEFICIT_MS while it plays (the queue was dry: roomd
                 * late): refilled at up to AS_REFILL ticks more a tick (+25 % before: 300 ms short took 1.2 s to
                 * refill, another delay meanwhile a gap); nothing more while catching up a late wake (its ticks do) */
                if (d > dead && !catching) { x = (long)fmin(d, (AS.playing && d > deficit ? AS_REFILL : AS_GROW) * tick); if (x > (long)(have - (uint64_t)n)) x = (long)(have - (uint64_t)n); AS.sped += (uint64_t)x; }
                else if (d < -dead) { x = -(long)fmin(-d, AS_SHRINK * tick + 1); if (x < -n) x = -n; AS.slowed += (uint64_t)-x; }
                n += x;
            } else {                                      /* live: silence put in or left out, only in silence (or before the music) */
                /* a gap at the receiver (its ring ran dry: the music behind it heard that much later) grew the ring as
                 * silence put in would have: taken as such up to the target, not pulled back at the clock's pace */
                if (N > AS.anchor + dead && AS.anchor < G) { AS.anchor += fmin(N - AS.anchor, G - AS.anchor); }
                const double want = G - AS.anchor;
                /* the ring above what the receiver asks (its gaps, a DAC's buffer grown and given back: never
                 * drained but by the pull before, ±300 ppm), or short of its anchor (the source late: a hole, the pull
                 * alone to fill it), by more than AS_DEFICIT_MS either way: zeros left out, or put in, in silences too */
                if (!AS.playing) { AS.n_avg = 0; AS.t_navg = 0; }
                else if (AS.t_navg == 0) { AS.n_avg = N; AS.t_navg = t; }   /* from its start playing, not from nothing sent */
                else AS.n_avg += (N - AS.n_avg) * fmin(1.0, T / 2.0);   /* a burst or a late report: not a level */
                const int weighed = AS.t_navg > 0 && t - AS.t_navg >= 3.0;
                /* far from it only (40 ms, a fifth of the target): a loaded machine's own swings left alone */
                const double far = fmax(2.0 * deficit, 0.2 * (double)G);
                const double over = weighed ? AS.n_avg - (G > AS.anchor ? (double)G : AS.anchor) : 0, under = weighed ? AS.anchor - AS.n_avg : 0;
                if (want > dead || (want < -dead && AS.ins > 0) || (AS.playing && (over > far || under > far))) {
                    const uint64_t span = (uint64_t)n + look < have ? (uint64_t)n + look : have;
                    const int silent = !AS.music || (AS.last_zero && as_q_zero(AS.qr, span));
                    /* n_avg moved at once by what is put in or left out (a step its average would take 2 s to
                     * follow: put back again meanwhile, then left out, the bench's ring swinging 0 to 990 ms) */
                    if (silent && want > dead) { zeros_in = (long)fmin(want, AS_INSERT * tick); AS.anchor += zeros_in; AS.ins += zeros_in; AS.put_in += (uint64_t)zeros_in; AS.n_avg += zeros_in; }
                    else if (silent && want < -dead && AS.music && AS.ins > 0) {
                        skip = (long)fmin(fmin(-want, (double)AS.ins), fmin((double)n, AS_GROW * tick));
                        if (skip > 0) { AS.anchor -= skip; AS.ins -= skip; AS.left_out += (uint64_t)skip; AS.n_avg -= skip; } else skip = 0;
                    }
                    /* (sw = qr + ins always: what is held unheard, anet_held, counts them) */
                    else if (silent && AS.playing && AS.music && under > far) { zeros_in = (long)fmin(under, AS_INSERT * tick); AS.ins += zeros_in; AS.put_in += (uint64_t)zeros_in; AS.put_back += (uint64_t)zeros_in; AS.n_avg += zeros_in; }
                    else if (silent && AS.playing && AS.music && over > far) {
                        skip = (long)fmin(over, fmin((double)n, AS_GROW * tick));
                        if (skip > 0) { AS.ins -= skip; AS.left_out += (uint64_t)skip; AS.left_over += (uint64_t)skip; AS.n_avg -= skip; } else skip = 0;
                    }
                }
            }
        }
        const int send_end = AS.ended && have == 0 && !AS.end_sent;
        if (send_end) AS.end_sent = 1;
        const uint64_t index = AS.qr, sidx = AS.sw;
        /* stamped at its time (next): a tick caught up late says how late; and music refilled after the queue ran
         * dry (roomd late) at the time it was due, its deficit before now: the receiver sees that delay too and sizes its
         * ring for it (before, stamped on time: the receiver never learnt it, a gap at each of roomd's delays, the bench) */
        const double due = fmin(next, t - owed_s);
        const uint32_t stamp = (AS.caps & ANET_CAP_STAMP) ? (uint32_t)(uint64_t)llround(due * 1e6) : 0;
        pthread_mutex_unlock(&as_lk);
        /* what goes: zeros_in zeros, then the queue's frames index + skip … index + n (skip: zeros left out) */
        const long total = zeros_in + (n - skip);
        int music = 0, last_zero = -1;
        const double t_send = as_mono();
        for (long done = 0; done < total;) {             /* packets of ANET_PKT_FRAMES at most */
            long m = total - done; if (m > ANET_PKT_FRAMES) m = ANET_PKT_FRAMES;
            anet_dhdr_pack(pkt, id, sidx + (uint64_t)done, (uint32_t)m, stamp);
            for (long i = 0; i < m; i++) {
                const long j = done + i;
                const int32_t *f = j < zeros_in ? zero2 : AS.q + 2 * ((index + (uint64_t)skip + (uint64_t)(j - zeros_in)) & AS.qcap);
                memcpy(pkt + ANET_DHDR + (size_t)i * 8, f, 8);   /* s32le: as the engine made them */
                if (f[0] | f[1]) { music = 1; last_zero = 0; } else last_zero = 1;
            }
            send(ufd, pkt, ANET_DHDR + (size_t)m * 8, 0);
            done += m;
            const double ts = as_mono();
            if (t_sent > 0) { const double us = (ts - t_sent) * 1e6; int b = (int)(us / 25); if (b >= AS_HB) b = AS_HB - 1; AS_H.n[b]++; AS_H.cnt++; AS_H.sum += us; if (us > AS_H.max) AS_H.max = us; }
            t_sent = ts;
        }
        const double sent_s = as_mono() - t_send;
        if (n > 0 || total > 0 || sent_s > AS_LATE_S) {
            pthread_mutex_lock(&as_lk);
            AS.qr += (uint64_t)n; AS.sw += (uint64_t)total; acc -= (double)n_base;
            if (music) AS.music = 1;
            if (last_zero >= 0) AS.last_zero = last_zero;
            if (sent_s > AS_LATE_S) as_late_note(AS_L_SEND, sent_s, ring_ms);
            pthread_mutex_unlock(&as_lk);
        }
        if (send_end) as_send_why(ANET_M_END, ANET_WHY_END);
    }
    rt_unregister();
    return NULL;
}

static _Thread_local int as_errno;   /* the last connection's errno (anet_errno: EHOSTUNREACH, EPERM…) */
int anet_errno(void) { return as_errno; }
static int as_connect(const char *host, int port, struct sockaddr_in *sa, char *why, size_t wl) {
    as_errno = 0;
    char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    if (getaddrinfo(host, ps, &hints, &ai) || !ai) { snprintf(why, wl, "audiolaz receiver %s: unknown address", host); return -1; }
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (fd < 0) { freeaddrinfo(ai); snprintf(why, wl, "socket"); return -1; }
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = { 3, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    memcpy(sa, ai->ai_addr, sizeof *sa);
    const int c = connect(fd, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);
    if (c) { const int e = errno; close(fd); as_errno = e; snprintf(why, wl, "audiolaz receiver %s:%d: %s", host, port, strerror(e)); return -1; }
    return fd;
}

int anet_info(const char *host, int port, anet_peer *p, int ms) { return anet_info_as(host, port, p, ms, 0); }
/* as: what this asker says of itself, in the INFO request's reserved word (ANET_IQ_*: the Mac's uplink, its
 * state); 0 nothing (every receiver before reads nothing there) */
int anet_info_as(const char *host, int port, anet_peer *p, int ms, unsigned as) {
    char why[200];
    struct sockaddr_in sa;
    const int fd = as_connect(host, port, &sa, why, sizeof why);
    if (fd < 0) return -1;
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    anet_hdr h = { .id = 0, .rate = 0, .ch = 2, .fmt = ANET_F_S32, .caps = as };
    uint8_t b[ANET_INFO + 1];
    anet_hdr_pack(b, &h);
    size_t have = 0;
    if (!as_send_all(fd, b, ANET_HDR))
        while (have < ANET_INFO) { const ssize_t n = recv(fd, b + have, ANET_INFO - have, 0); if (n <= 0) break; have += (size_t)n; }
    close(fd);
    if (have < ANET_INFO || anet_g32(b) != ANET_IMAGIC) return -1;
    b[ANET_INFO] = 0;
    memset(p, 0, sizeof *p);
    for (char *s = (char *)b + 4, *e; *s; s = e + 1) {
        e = strchr(s, ';'); if (!e) e = s + strlen(s);
        const char c = *e; *e = 0;
        if (!strncmp(s, "id=", 3)) snprintf(p->id, sizeof p->id, "%.63s", s + 3);
        else if (!strncmp(s, "dac=", 4)) snprintf(p->dac, sizeof p->dac, "%.95s", s + 4);
        else if (!strncmp(s, "max=", 4)) p->max_rate = (unsigned)strtoul(s + 4, NULL, 10);
        else if (!strncmp(s, "hw=", 3)) snprintf(p->hw, sizeof p->hw, "%.95s", s + 3);
        else if (!strncmp(s, "rates=", 6)) snprintf(p->rates, sizeof p->rates, "%.95s", s + 6);   /* */
        else if (!strncmp(s, "bits=", 5)) snprintf(p->bits, sizeof p->bits, "%.47s", s + 5);      /* */
        else if (!strncmp(s, "err=", 4)) snprintf(p->err, sizeof p->err, "%.23s", s + 4);
        else if (!strncmp(s, "room=", 5)) snprintf(p->room, sizeof p->room, "%.79s", s + 5);   /* an uplink's receiver */
        if (!c) break;
    }
    snprintf(p->addr, sizeof p->addr, "%.47s", inet_ntoa(sa.sin_addr));
    p->port = port;
    return p->id[0] ? 0 : -1;
}

/* our id when none was given: this machine and this player's name (the same from one run to the next) */
static void as_default_uid(void) {   /* under as_lk */
    if (as_uid[0]) return;
    char h[128] = "";
    gethostname(h, sizeof h - 1);
    uint64_t x = 1469598103934665603ULL;   /* FNV-1a */
    for (const char *const *p = (const char *const[]){ h, "|", as_name, NULL }; *p; p++)
        for (const unsigned char *c = (const unsigned char *)*p; *c; c++) { x ^= *c; x *= 1099511628211ULL; }
    snprintf(as_uid, sizeof as_uid, "%016llx", (unsigned long long)x);
}
int anet_open(const char *host, int port, unsigned rate, int fmt, unsigned hold_ms, char *why, size_t wl) {
    (void)hold_ms;
    anet_close(0);
    pthread_mutex_lock(&as_lk);
    as_busy = 0; as_busy_by[0] = 0; as_refused_e = 0;
    pthread_mutex_unlock(&as_lk);
    struct sockaddr_in sa;
    const int fd = as_connect(host, port, &sa, why, wl);
    if (fd < 0) return -1;
    pthread_mutex_lock(&as_lk);
    const unsigned offer = as_caps_offer;
    const int take = as_take_next;
    char uid[40], name[96];
    as_default_uid();
    snprintf(uid, sizeof uid, "%s", as_uid); snprintf(name, sizeof name, "%s", as_name);
    /* the streams' ids from a number of this run's own: two players behind one NAT never one id at once */
    if (!as_ids) as_ids = ((unsigned)getpid() * 2654435761u ^ (unsigned)(as_mono() * 1e6)) & 0x7FFF0000u;
    pthread_mutex_unlock(&as_lk);
    anet_hdr h = { .id = ++as_ids, .rate = rate, .ch = 2, .fmt = (uint16_t)fmt, .caps = offer };
    anet_pkt_plan(rate, &h.pkt_frames, &h.pkt_us);
    uint8_t hb[ANET_HDR + 8 + ANET_HELLO_MAX], rb[ANET_REP3];
    anet_hdr_pack(hb, &h);
    /* who we are, in the same write as the header (ANET_CAP_WHO) */
    const size_t hn = ANET_HDR + (offer & ANET_CAP_WHO ? anet_hello_pack(hb + ANET_HDR, sizeof hb - ANET_HDR, uid, name, take) : 0);
    anet_rep r;
    if (as_send_all(fd, hb, hn)) { close(fd); snprintf(why, wl, "audiolaz receiver: header not sent"); return -1; }
    size_t have = 0;                                     /* its first report: accepted or refused (the DAC opened) */
    const double end = as_mono() + 10.0;
    while (have < ANET_REP && as_mono() < end) {
        const ssize_t n = recv(fd, rb + have, ANET_REP - have, 0);
        if (n <= 0 && !(n < 0 && (errno == EAGAIN || errno == EINTR))) break;
        if (n > 0) have += (size_t)n;
    }
    if (have < ANET_REP || anet_rep_parse(rb, &r) || r.id != h.id) { close(fd); snprintf(why, wl, "audiolaz receiver: no answer"); return -1; }
    if (r.flags & ANET_R_REFUSED) {
        if (r.reason == ANET_E_BUSY) {
            uint8_t ib[ANET_INFO + 1];
            size_t got = 0;
            const double until = as_mono() + 2.0;
            while (got < ANET_INFO && as_mono() < until) {
                const ssize_t n = recv(fd, ib + got, ANET_INFO - got, 0);
                if (n <= 0 && !(n < 0 && (errno == EAGAIN || errno == EINTR))) break;
                if (n > 0) got += (size_t)n;
            }
            char by[128] = "";
            if (got == ANET_INFO && anet_g32(ib) == ANET_BMAGIC) {
                ib[ANET_INFO] = 0;
                const char *b = strstr((char *)ib + 4, "by=");
                if (b) snprintf(by, sizeof by, "%.*s", (int)strcspn(b + 3, ";"), b + 3);
            }
            close(fd);
            pthread_mutex_lock(&as_lk);
            as_busy = 1; snprintf(as_busy_by, sizeof as_busy_by, "%s", by[0] ? by : "another player");
            snprintf(why, wl, "busy: playing from \"%s\"", as_busy_by);
            pthread_mutex_unlock(&as_lk);
            return -1;
        }
        close(fd);
        pthread_mutex_lock(&as_lk); as_refused_e = r.reason; pthread_mutex_unlock(&as_lk);
        snprintf(why, wl, "audiolaz receiver refused %u Hz (%s)", rate,
                 r.reason == ANET_E_RATE ? ((offer & ANET_CAP_RATES) ? "not a rate its DAC takes as it is" : "above its DAC's rates")
                 : r.reason == ANET_E_DAC ? "its DAC did not open: busy, or the rate refused"
                 : r.reason == ANET_E_DAC_START ? "its DAC refuses to start: unplug it and plug it in again"
                 : r.reason == ANET_E_FORMAT ? "DoP its DAC cannot play whole: fewer than 24 bits there, or resampled" : "header");
        return -1;
    }
    pthread_mutex_lock(&as_lk);
    as_take_next = 0;                                     /* taken: once */
    pthread_mutex_unlock(&as_lk);

    const unsigned caps = r.flags & ANET_R_P3 ? (unsigned)r.received & offer : 0;
    const int ufd = socket(AF_INET, SOCK_DGRAM, 0);
    sa.sin_port = htons((uint16_t)port);
    if (ufd < 0 || connect(ufd, (struct sockaddr *)&sa, sizeof sa)) { if (ufd >= 0) close(ufd); close(fd); snprintf(why, wl, "audiolaz receiver: no UDP way"); return -1; }
    { int sb = 1 << 20; setsockopt(ufd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb); }
#ifdef IPTOS_LOWDELAY
    { int tos = 0xB8; setsockopt(ufd, IPPROTO_IP, IP_TOS, &tos, sizeof tos); }   /* EF: the music first on the wire */
#endif
    uint64_t cap = 1;
    while (cap < (uint64_t)rate) cap <<= 1;              /* a second at least */
    int32_t *q = malloc((size_t)cap * 8);
    if (!q) { close(ufd); close(fd); snprintf(why, wl, "no memory"); return -1; }
    pthread_mutex_lock(&as_lk);
    free(AS.q);
    AS.q = q; AS.qcap = cap - 1; AS.qw = AS.qr = 0;
    AS.on = 1; AS.fd = fd; AS.ufd = ufd; AS.lost = AS.ended = AS.end_sent = AS.paused = AS.full_once = AS.playing = AS.locked = AS.stop_tx = 0;
    AS.id = h.id; AS.rate = rate; AS.pkt_frames = h.pkt_frames; AS.pkt_us = h.pkt_us; AS.ring_target = r.reason;
    AS.rep_played = 0; AS.rep_ring = 0; AS.p0 = 0; AS.t0 = AS.t_rep = AS.t_open = as_mono(); AS.k = 1.0; AS.why[0] = 0; AS.close_by = 0;
    AS.caps = caps; AS.music = 0; AS.last_zero = 1; AS.sw = 0; AS.rep_read = 0; AS.ins = 0;
    AS.start = AS.goal = r.reason; AS.anchor = r.reason; AS.goal_lo = AS.goal_hi = r.reason;
    AS.late_us = AS.peak_us = AS.r_underruns = AS.r_adjusts = AS.r_min = AS.r_max = 0; AS.rstate = ANET_RS_FILLING; AS.rwhy = 0;
    AS.pull = 0; AS.left_over = AS.put_back = 0; AS.n_avg = 0; AS.t_navg = 0; AS.k_sum = AS.pull_sum = 0; AS.kp_n = 0;   /* */
    {

        char hp[sizeof AS.hp];
        snprintf(hp, sizeof hp, "%s:%d", host, port);
        const int kk = as_known_at(hp, 0), known = kk >= 0 && as_known[kk].has_kf;
        AS.k = known ? as_known[kk].kf : 1.0; AS.kf_var = 0;
        AS.f_sig = 0; AS.perr = 0; AS.perr_on = 0; AS.rep_out = AS.rep_steps = 0;
        as_fit_restart();
        AS.kf_p = known ? 9e-10 : 2.25e-8;
    }
    AS.put_in = AS.left_out = AS.sped = AS.slowed = 0; AS.ping_seq = 0; AS.t_ping = 0; AS.r_stopped = 0;
    AS.stop_why = ANET_WHY_NONE; AS.stop_text[0] = 0; AS.meta_sent = 0; AS.after = 0;
    AS.rt_err = AS.rt_said = 0; memset(AS.late_n, 0, sizeof AS.late_n); memset(AS.late_max, 0, sizeof AS.late_max); AS.late_w = AS.late_r = 0;
    snprintf(AS.hp, sizeof AS.hp, "%s:%d", host, port);
    as_known_note();
    pthread_mutex_unlock(&as_lk);
    ANET_LOG("audiolaz: stream %u to %s:%d at %u Hz, %s%s", h.id, host, port, rate,
             !caps ? "a version-2 receiver (its ring fixed, no state exchanged)" : (caps & ANET_CAP_P3) == ANET_CAP_P3 ? "protocol 3 (its ring sized by the network, the state exchanged)" : "protocol 3 in part",
             !caps ? "" : caps & ANET_CAP_WHO ? "; one player at a time on its DAC" : "; a receiver before 14/10: the newest player takes its DAC");
    /* what plays, before the first packet (as_meta_now) */
    if (caps & ANET_CAP_STATE) {
        pthread_mutex_lock(&as_metalk);
        const unsigned ser = as_meta_serial;
        pthread_mutex_unlock(&as_metalk);
        if (ser) as_meta_now(fd);
    }
    if (pthread_create(&AS.th, NULL, as_rx_main, (void *)(intptr_t)fd)) { close(ufd); close(fd); AS.on = 0; AS.fd = AS.ufd = -1; snprintf(why, wl, "thread"); return -1; }
    if (pthread_create(&AS.tx, NULL, as_tx_main, NULL)) { anet_close(0); snprintf(why, wl, "thread"); return -1; }
    return 0;
}

long anet_write(const int32_t *lr, long frames) {
    pthread_mutex_lock(&as_lk);
    if (!AS.on || AS.lost) { pthread_mutex_unlock(&as_lk); return -1; }
    if (AS.ended) { pthread_mutex_unlock(&as_lk); return frames; }
    const uint64_t limit = (uint64_t)AS.rate * (uint64_t)as_fifo_ms / 1000, have = AS.qw - AS.qr;
    if (have >= limit) { AS.full_once = 1; pthread_mutex_unlock(&as_lk); return 0; }
    long n = frames;
    if ((uint64_t)n > limit - have) n = (long)(limit - have);
    const uint64_t w = AS.qw;
    pthread_mutex_unlock(&as_lk);
    for (long i = 0; i < n;) {                           /* into the queue, as is (the timer thread reads behind qw only) */
        const uint64_t at = (w + (uint64_t)i) & AS.qcap;
        long m = (long)(AS.qcap + 1 - at); if (m > n - i) m = n - i;
        memcpy(AS.q + 2 * at, lr + 2 * i, (size_t)m * 8);
        i += m;
    }
    pthread_mutex_lock(&as_lk); AS.qw += (uint64_t)n; pthread_mutex_unlock(&as_lk);
    return n;
}
void anet_wait(int ms) {
    pthread_mutex_lock(&as_lk);
    double s = ms / 1000.0;
    if (AS.on && AS.rate) { const double need = (double)AS.pkt_us / 1e6; if (need < s) s = need; }   /* a packet's time: room */
    pthread_mutex_unlock(&as_lk);
    if (s < 0.0005) s = 0.0005;
    usleep((useconds_t)(s * 1e6));
}
uint64_t anet_sent(void) { pthread_mutex_lock(&as_lk); const uint64_t s = AS.qw; pthread_mutex_unlock(&as_lk); return s; }
/* all not heard yet: the queue, and what was sent less what was played (the silence put in counted: it is heard
 * before the music behind it, latency as any frame); and what the receiver says is heard after its DAC (another
 * player behind it: 0 for a receiver beside its DAC) */
long anet_held(void) {
    pthread_mutex_lock(&as_lk);
    const long h = AS.on ? (long)((double)AS.qw + (double)AS.ins - as_est_played(as_mono()) + AS.after) : 0;
    pthread_mutex_unlock(&as_lk);
    return h < 0 ? 0 : h;
}
long anet_queued(void) { pthread_mutex_lock(&as_lk); const long q = AS.on ? (long)(AS.qw - AS.qr) : 0; pthread_mutex_unlock(&as_lk); return q; }
int anet_started(void) { pthread_mutex_lock(&as_lk); const int s = AS.on && !AS.lost && AS.playing; pthread_mutex_unlock(&as_lk); return s; }
int anet_was_full(void) { pthread_mutex_lock(&as_lk); const int s = AS.full_once; pthread_mutex_unlock(&as_lk); return s; }
void anet_pause(int on) {
    pthread_mutex_lock(&as_lk);
    if (!AS.on || AS.paused == on) { pthread_mutex_unlock(&as_lk); return; }
    if (on) AS.p0 = as_est_played(as_mono());
    AS.t0 = as_mono();
    AS.paused = on;
    pthread_mutex_unlock(&as_lk);
    as_send_msg(on ? ANET_M_PAUSE : ANET_M_RESUME);
}
void anet_end(void) {   /* the timer thread sends END once the queue is out */
    pthread_mutex_lock(&as_lk);
    if (AS.on && !AS.lost) AS.ended = 1;
    pthread_mutex_unlock(&as_lk);
}

int anet_get_state(const char *hostport, anet_state *s) {
    memset(s, 0, sizeof *s);
    s->last_stop_age = -1;
    pthread_mutex_lock(&as_lk);
    const char *hp = hostport ? hostport : AS.on ? AS.hp : "";
    const int k = hp[0] ? as_known_at(hp, 0) : -1;
    if (k < 0 && !(AS.on && !strcmp(hp, AS.hp))) { pthread_mutex_unlock(&as_lk); return -1; }
    if (k >= 0) {
        s->rate = as_known[k].rate; s->rstate = as_known[k].rstate; s->rwhy = as_known[k].rwhy;
        s->target_ms = as_known[k].target_ms; s->start_ms = as_known[k].start_ms; s->late_ms = as_known[k].late_ms; s->peak_ms = as_known[k].peak_ms;
        s->lo_ms = as_known[k].lo_ms; s->hi_ms = as_known[k].hi_ms; s->underruns = as_known[k].underruns; s->adjusts = as_known[k].adjusts;
        snprintf(s->last_stop, sizeof s->last_stop, "%s", as_known[k].last_stop);
        if (as_known[k].t_stop >= 0 && as_known[k].last_stop[0]) s->last_stop_age = as_mono() - as_known[k].t_stop;
    }
    if (AS.on && !strcmp(hp, AS.hp)) {
        const double t = as_mono(), ms = 1000.0 / (AS.rate ? AS.rate : 1);
        s->on = 1; s->p3 = !!(AS.caps & ANET_CAP_STATE); s->adapt = !!(AS.caps & ANET_CAP_ADAPT); s->live = as_live_next;
        s->rate = AS.rate;
        s->ring_ms = s->adapt ? ((double)AS.sw - as_read_est(t)) * ms : AS.rep_ring * ms;
        s->anchor_ms = AS.anchor * ms; s->ins_ms = (double)AS.ins * ms;
        s->held_ms = ((double)AS.qw + (double)AS.ins - as_est_played(t) + AS.after) * ms;
        s->after_ms = AS.after * ms;
        s->dac_ppm = (AS.k - 1.0) * 1e6; s->pull_ppm = AS.pull * 1e6;   /* ; the fitted clock the pace follows */
        s->over_ms = (double)AS.left_over * ms; s->back_ms = (double)AS.put_back * ms;
        for (int i = 0; i < 4; i++) { s->tx_late += AS.late_n[i]; if (AS.late_max[i] * 1000 > s->tx_late_ms) s->tx_late_ms = AS.late_max[i] * 1000; }
        if (!s->adapt) { s->target_ms = AS.ring_target * ms; s->start_ms = s->target_ms; }
    }
    pthread_mutex_unlock(&as_lk);
    return 0;
}

void anet_close(int finish) {
    pthread_mutex_lock(&as_lk);
    if (!AS.on) { pthread_mutex_unlock(&as_lk); return; }
    if (finish && !AS.lost) {                            /* the queue sent out first (its END with it), 3 s at most */
        AS.ended = 1;
        const double until = as_mono() + 3.0;
        while (!AS.end_sent && !AS.lost && as_mono() < until) { pthread_mutex_unlock(&as_lk); usleep(2000); pthread_mutex_lock(&as_lk); }
    }
    const int fd = AS.fd, ufd = AS.ufd, lost = AS.lost, end_sent = AS.end_sent, r_stopped = AS.r_stopped;
    const unsigned why = AS.stop_why ? AS.stop_why : finish ? ANET_WHY_END : ANET_WHY_NONE;
    char text[200];
    snprintf(text, sizeof text, "%s", AS.stop_text[0] ? AS.stop_text : anet_why_text(why));
    AS.stop_tx = 1;
    pthread_mutex_unlock(&as_lk);
    pthread_join(AS.tx, NULL);
    if (AS_H.cnt) ANET_LOG("audiolaz: %lu packets every %u µs planned: gap µs mean %.0f p1 %.0f p50 %.0f p99 %.0f max %.0f",
                           AS_H.cnt, AS.pkt_us, AS_H.sum / AS_H.cnt, as_hpct(.01), as_hpct(.5), as_hpct(.99), AS_H.max);
    if (!lost) { if (!finish) as_send_why(ANET_M_STOP, why); else if (!end_sent) as_send_why(ANET_M_END, ANET_WHY_END); }
    /* the stop in one line: who, why, the stream's figures */
    pthread_mutex_lock(&as_lk);
    {
        const double ms = 1000.0 / (AS.rate ? AS.rate : 1);
        char said[480], fig[800] = "";
        if (lost) snprintf(said, sizeof said, "%s by the receiver: %s", r_stopped ? "stopped" : "lost", AS.why);
        else snprintf(said, sizeof said, "%s by the player: %s", finish ? "ended" : "stopped", text);
        if (AS.caps & ANET_CAP_ADAPT)
            snprintf(fig, sizeof fig, "; its ring %.0f ms at the start, target %.0f–%.0f ms (%.0f at the end), %u changes; jitter %.1f ms, peak %.1f; silence put in %.0f ms, left out %.0f; sent ahead %.0f ms, held back %.0f; its underruns %u",
                     AS.start * ms, AS.goal_lo * ms, AS.goal_hi * ms, AS.goal * ms, AS.r_adjusts, AS.late_us / 1000.0, AS.peak_us / 1000.0,
                     AS.put_in * ms, AS.left_out * ms, AS.sped * ms, AS.slowed * ms, AS.r_underruns);
        /* the pace: the DAC's clock as its reports said it, the ring's pull at the end; live, what was left out
         * above its target and put back below its anchor */
        if (AS.caps & ANET_CAP_ADAPT) {
            const size_t fl = strlen(fig);
            if (AS.kp_n) snprintf(fig + fl, sizeof fig - fl, "; its DAC's clock %+.0f ppm against ours on average, the ring's pull %+.0f ppm on average", (AS.k_sum / AS.kp_n - 1) * 1e6, AS.pull_sum / AS.kp_n * 1e6);
            if (AS.rep_out) { const size_t f3 = strlen(fig); snprintf(fig + f3, sizeof fig - f3, "; its reports: %lu set aside (off its clock by more than %.1f ms), %lu steps", AS.rep_out, fmax(AS_OUT_N * AS.f_sig, AS_OUT_MS / 1000.0 * AS.rate) * ms, AS.rep_steps); }
            if (AS.kf_var > 0 && AS.kf_var < 4e-10) { const int kk = as_known_at(AS.hp, 1); as_known[kk].kf = AS.k; as_known[kk].has_kf = 1; }   /* the next stream starts there */
            if (AS.left_over || AS.put_back) { const size_t f2 = strlen(fig); snprintf(fig + f2, sizeof fig - f2, " (silence left out above its target %.0f ms, put back below %.0f)", AS.left_over * ms, AS.put_back * ms); }
        }
        /* the sender's own delays this stream, by cause */
        const unsigned long ln = AS.late_n[0] + AS.late_n[1] + AS.late_n[2] + AS.late_n[3];
        if (ln) {
            const size_t fl = strlen(fig);
            snprintf(fig + fl, sizeof fig - fl, "; the sender late %lu time%s (woken late %lu, worst %.0f ms; its lock %lu, %.0f; a send %lu, %.0f; nothing to send %lu, %.0f)",
                     ln, ln > 1 ? "s" : "", AS.late_n[AS_L_WOKE], AS.late_max[AS_L_WOKE] * 1000, AS.late_n[AS_L_LOCK], AS.late_max[AS_L_LOCK] * 1000,
                     AS.late_n[AS_L_SEND], AS.late_max[AS_L_SEND] * 1000, AS.late_n[AS_L_EMPTY], AS.late_max[AS_L_EMPTY] * 1000);
        }
        ANET_LOG("audiolaz: stream %u %s after %.1f s%s", AS.id, said, as_mono() - AS.t_open, fig);
        as_known_note();
        const int k = as_known_at(AS.hp, 1);
        snprintf(as_known[k].last_stop, sizeof as_known[k].last_stop, "%.199s", said);
        as_known[k].t_stop = as_mono();
    }
    pthread_mutex_unlock(&as_lk);
    /* our side closed; the receiver closes its own once it has read everything (no reset) */
    shutdown(fd, SHUT_WR);
    pthread_mutex_lock(&as_lk);
    AS.on = 0; AS.close_by = as_mono() + 2.0;
    pthread_mutex_unlock(&as_lk);
    pthread_join(AS.th, NULL);
    pthread_mutex_lock(&as_lk);
    AS.fd = -1; AS.ufd = -1; AS.close_by = 0;
    pthread_mutex_unlock(&as_lk);
    close(fd); close(ufd);
}
