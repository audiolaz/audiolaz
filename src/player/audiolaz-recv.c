/* src/player/audiolaz-recv.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "anet.h"
#include "alsa_hw.h"
#ifndef ANET_NULL_ONLY
#include "out.h"
#define OUT_AGAIN_ OUT_AGAIN
#else
#define OUT_AGAIN_ (-11)
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0                     /* (the tests on a Mac: SIGPIPE ignored anyway) */
#endif

static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double cputime(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void lg(const char *f, ...) {
    va_list a; va_start(a, f); fprintf(stderr, "audiolaz-recv: "); vfprintf(stderr, f, a); va_end(a); fputc('\n', stderr); fflush(stderr);
}

/* ── options ── */
static const char *o_dac = "auto", *o_dump = NULL;
static char o_name[96] = "", o_id[64] = "";
static char o_hw[96] = "";               /* the DAC's identity (alsa_hw.h), said in INFO and mDNS */
static int o_port = ANET_PORT, o_null = 0, o_mdns = 1, o_rt = -1, o_once = 0, o_period_us = 2500, o_periods = 4, o_ring_ms = ANET_RING_MS, o_cpu = -1, o_stats = 0;
static int o_ring_max_ms = ANET_RING_MAX_MS, o_adapt = 1, o_v2 = 0, o_mem_s = 300;
static int o_force = 0, o_lock_mb = 32;   /* o_rt / o_cpu −1: auto */
static double o_q = 0.999;
static unsigned o_max = 0;
static double o_ppm = 0;
static char dac_dev[128];

/* ── histograms (--stats): cheap counters in the path, printed after ── */
#define HB 40
typedef struct { unsigned long n[HB]; double sum, max; unsigned long cnt; } hist;   /* buckets of `step` µs */
static void h_add(hist *h, double us, double step) { int b = (int)(us / step); if (b < 0) b = 0; if (b >= HB) b = HB - 1; h->n[b]++; h->sum += us; h->cnt++; if (us > h->max) h->max = us; }
static double h_pct(const hist *h, double p, double step) {
    unsigned long want = (unsigned long)(p * h->cnt), c = 0;
    for (int b = 0; b < HB; b++) { c += h->n[b]; if (c > want) return (b + 0.5) * step; }
    return HB * step;
}
static hist H_pkt, H_wake, H_work;
static double step_pkt = 50, step_wake = 100, step_work = 5;

/* ── the device: the DAC (out_alsa.c) or the null clock ── */
static struct {
    int open; unsigned rate;
    unsigned long buffer, period;
#ifndef ANET_NULL_ONLY
    out_pcm *o;
#endif
    double t0; uint64_t eaten0, written;
    FILE *dump;
} D;
static int dev_open(unsigned rate) {
    unsigned long per = (unsigned long)((uint64_t)rate * (unsigned)o_period_us / 1000000);
    if (per < 16) per = 16;
    if (o_null) {
        D.rate = rate; D.period = per; D.buffer = per * (unsigned)o_periods; D.t0 = 0; D.eaten0 = 0; D.written = 0; D.open = 1;
        if (o_dump && !D.dump) D.dump = fopen(o_dump, "ab");
        return 0;
    }
#ifndef ANET_NULL_ONLY

    for (int i = 0; i < 50 && !(D.o = out_open(dac_dev, OUTF_S32, rate, per, (unsigned)o_periods, i < 49)); i++) usleep(50000);
    if (!D.o) return -1;
    out_params(D.o, &D.buffer, &D.period);
    D.rate = rate; D.open = 1;
    lg("%s open at %u Hz, S32_LE, period %lu, buffer %lu frames", dac_dev, rate, D.period, D.buffer);
    return 0;
#else
    return -1;
#endif
}
static void dev_close(void) {
    if (!D.open) return;
    D.open = 0;
    if (D.dump) fflush(D.dump);
#ifndef ANET_NULL_ONLY
    if (!o_null) { out_close(D.o); D.o = NULL; }   /* snd_pcm_drop, then close: the DAC given back at once */
#endif
}
static _Atomic unsigned long null_xruns;
static double test_xrun_s = 0;   /* AUDIOLAZ_RECV_TEST_XRUN=<s>: the null DAC's buffer run dry every s seconds */
static uint64_t null_eaten(double t) { return D.t0 == 0 ? D.eaten0 : D.eaten0 + (uint64_t)((t - D.t0) * D.rate * (1.0 + o_ppm * 1e-6)); }
static long dev_delay(void) {
    if (o_null) { const uint64_t e = null_eaten(mono()); return e >= D.written ? 0 : (long)(D.written - e); }
#ifndef ANET_NULL_ONLY
    long d = 0; if (out_delay(D.o, &d)) return -1; return d;
#else
    return 0;
#endif
}
static long dev_avail(void) {
    if (o_null) {
        const double t = mono();
        uint64_t e = null_eaten(t);
        if (D.t0 > 0 && e > D.written) { D.eaten0 = D.written; D.t0 = t; e = D.written; atomic_fetch_add(&null_xruns, 1); }   /* the clock not fed in time (our thread late): an underrun */
        return (long)D.buffer - (long)(D.written - e);
    }
#ifndef ANET_NULL_ONLY
    return out_avail(D.o);
#else
    return -1;
#endif
}
static long dev_write(const int32_t *p, unsigned long n, int data) {
    if (o_null) {
        if (test_xrun_s > 0 && data) {   /* the test: its buffer run dry now and then, nothing taken that pass */
            static double t_x;
            const double t = mono();
            if (t_x == 0) t_x = t;
            else if (t - t_x >= test_xrun_s) { t_x = t; return -EPIPE; }
        }
        if (D.t0 == 0) { D.t0 = mono(); D.eaten0 = 0; }
        if (D.dump && data) fwrite(p, 8, n, D.dump);
        D.written += n;
        return (long)n;
    }
#ifndef ANET_NULL_ONLY
    (void)data;
    return out_write(D.o, p, n);
#else
    return -1;
#endif
}
static void dev_wait(void) {   /* until a period's room */
    if (o_null) {
        const long a = dev_avail();
        if (a < (long)D.period) { const double s = (double)((long)D.period - a) / D.rate; usleep((useconds_t)(s * 1e6) + 1); }
        return;
    }
#ifndef ANET_NULL_ONLY
    out_wait(D.o, o_period_us / 250 + 20);
#endif
}

/* after a negative write or avail: the DAC ready again (0), or not (−1: the stream lost); the null clock never
 * fails. More than XR_MAX in XR_S seconds: not taken (a DAC really gone, never a loop of restarts). */
#define XR_MAX 5
#define XR_S 10.0
static int dev_recover(long e) {
    static double t0; static int n;
    const double t = mono();
    if (t - t0 > XR_S) { t0 = t; n = 0; }
    if (++n > XR_MAX) return -1;
    if (o_null) return 0;
#ifndef ANET_NULL_ONLY
    return out_recover(D.o, e);
#else
    (void)e; return -1;
#endif
}

/* ── the ring: frames by their stream index (index & mask) ── */
static int32_t *ring;
static uint64_t ring_mask;
static _Atomic uint64_t rw, rr;            /* frames come (the highest index + frames), frames read */
static _Atomic unsigned long lost_frames, late_pkts;
static _Atomic double t_last_pkt;          /* the last packet of the stream taken (the player's sign of life too) */

/* ── protocol 3: the jitter, measured by the packets' thread, the target from it ──────────────────────────
 * Each packet's one-way delay (its arrival less the time the player stamped it, µs modulo 2^32: an offset between the
 * two clocks that does not matter) less the least of the last JW seconds is its lateness. Each second's lateness, a
 * high quantile of it (o_q), is kept (JM seconds at most, o_mem_s); the target is the largest kept × 1.25 + 15 ms. */
#define JW 10
#define JM 3600
#define JBINS 1024                         /* 1 ms bins */
static struct {
    int on;                                /* this stream stamped (protocol 3) */
    uint32_t base;                         /* the first packet's delay: the others relative to it */
    int32_t win_min[JW]; int wi; double t_sec;
    int32_t cur_min;
    unsigned long bins[JBINS], n;
    float mem[JM]; double mem_t[JM]; int mn, mh;   /* the seconds' quantiles (ms), a ring of them */
    float late_ms;                         /* the last second's */
} J;
static char j_from[48];                    /* the player the memory is of: its id, else its address */
static _Atomic uint32_t j_late_us, j_peak_us, j_goal;   /* the last second's, the remembered peak (µs); the target (frames) */
static _Atomic unsigned long j_adjusts;
static _Atomic int j_live = 1;             /* the player's source live (its pings say it): the narrow margin */
static void j_reset_stream(void) {
    J.on = 0; J.base = 0; J.wi = 0; J.t_sec = 0; J.cur_min = INT32_MAX; J.n = 0;
    for (int i = 0; i < JW; i++) J.win_min[i] = INT32_MAX;
    memset(J.bins, 0, sizeof J.bins);
}
static double j_peak_ms(double now) {      /* the largest second's quantile still remembered */
    float p = 0;
    for (int i = 0; i < J.mn; i++) { const int k = (J.mh - 1 - i + JM) % JM; if (now - J.mem_t[k] > o_mem_s) break; if (J.mem[k] > p) p = J.mem[k]; }
    return p;
}
/* the target from the peak (frames): 1.25 × + 15 ms, between the start and the ceiling; up at once, down with hysteresis */
static void j_target(unsigned rate, double now) {
    const double pk = j_peak_ms(now);
    double want = atomic_load(&j_live) ? pk * 1.25 + 15.0 : pk * 2.0 + 30.0;   /* music read ahead: its latency costs nothing */
    if (pk <= 0) want = 0;
    if (want < o_ring_ms) want = o_ring_ms;
    if (want > o_ring_max_ms) want = o_ring_max_ms;
    const uint32_t cur = atomic_load(&j_goal), w = (uint32_t)((uint64_t)rate * ((uint64_t)(want / 5.0) + ((double)(uint64_t)(want / 5.0) * 5.0 < want)) * 5 / 1000);   /* up to 5 ms */
    const double cur_ms = cur * 1000.0 / rate;
    if (w > cur || (cur_ms - want > 10.0 && cur_ms - want > 0.1 * cur_ms)) { atomic_store(&j_goal, w); atomic_fetch_add(&j_adjusts, 1); }
    atomic_store(&j_peak_us, (uint32_t)(pk * 1000));
}
static void j_packet(uint32_t stamp, double now, unsigned rate) {   /* the packets' thread */
    const uint32_t raw = (uint32_t)(uint64_t)(now * 1e6 + 0.5) - stamp;   /* (no libm: the build lines take none) */
    if (!J.on) { J.on = 1; J.base = raw; J.t_sec = now; }
    const int32_t rel = (int32_t)(raw - J.base);
    if (rel < J.cur_min) J.cur_min = rel;
    int32_t mn = J.cur_min;
    for (int i = 0; i < JW; i++) if (J.win_min[i] < mn) mn = J.win_min[i];
    const double late = (rel - mn) / 1000.0;                          /* ms */
    int b = (int)late; if (b < 0) b = 0; if (b >= JBINS) b = JBINS - 1;
    J.bins[b]++; J.n++;
    if (now - J.t_sec >= 1.0) {                                       /* a second: its quantile kept, the target */
        unsigned long want = (unsigned long)(o_q * J.n), c = 0; int q = 0;
        for (; q < JBINS; q++) { c += J.bins[q]; if (c > want) break; }
        J.late_ms = (float)(q + 1);
        atomic_store(&j_late_us, (uint32_t)(J.late_ms * 1000));
        J.mem[J.mh] = J.late_ms; J.mem_t[J.mh] = now; J.mh = (J.mh + 1) % JM; if (J.mn < JM) J.mn++;
        J.win_min[J.wi] = J.cur_min; J.wi = (J.wi + 1) % JW; J.cur_min = INT32_MAX;
        memset(J.bins, 0, sizeof J.bins); J.n = 0; J.t_sec = now;
        j_target(rate, now);
    }
}

/* ── shared state, under lk ── */
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static struct {
    int want_park, parked;
    int started, ended, paused, drained, failed, live;   /* live: a stream's packets taken (its id) */
    unsigned id, rate, caps;               /* caps: protocol 3, what of it this stream has (ANET_CAP_*; 0 version 2) */
    uint64_t target;                       /* the ring's start (frames); the sender keeps it (version 2) or the target */
    uint64_t tw, zeros; long delay;
    unsigned long underruns, gaps;         /* periods of zeros in the music; and the gaps they made (heard as one each) */
    unsigned long xruns;                   /* the DAC's buffer run dry (its thread late), made ready again */
    int dry;
    uint64_t rmin, rmax;                   /* the ring this stream while playing (frames) */
    unsigned why, seq;                     /* the last stop's reason, the last ping's seq */
    double t_ping;                         /* the last ping */
    int cfd;                               /* the stream's TCP connection: reports */
    uint8_t pend[ANET_REP3]; size_t npend; /* a report half sent (the socket full): the rest first */
    struct in_addr from;                   /* its sender: the only packets taken */
} S = { .cfd = -1 };

static int send_all(int fd, const void *p, size_t n) {
    const uint8_t *c = p;
    while (n) { const ssize_t w = send(fd, c, n, MSG_NOSIGNAL); if (w < 0 && errno == EINTR) continue; if (w <= 0) return -1; c += w; n -= (size_t)w; }
    return 0;
}
static unsigned state_locked(void) {
    if (S.failed) return ANET_RS_DAC_FAILED;
    if (S.drained) return ANET_RS_DRAINED;
    if (!S.live) return S.cfd >= 0 ? ANET_RS_STOPPED : ANET_RS_IDLE;
    if (S.paused) return ANET_RS_PAUSED;
    if (S.ended) return ANET_RS_DRAINING;
    return S.started ? ANET_RS_PLAYING : ANET_RS_FILLING;
}
/* a report now (under lk): never blocking (the DAC's thread sends them); a report the socket took half of is finished
 * before the next one (else the player would read them askew) */
static void report_locked_f(uint32_t extra_flags) {
    if (S.cfd < 0) return;
    if (S.npend) {
        const ssize_t w = send(S.cfd, S.pend, S.npend, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (w > 0) { memmove(S.pend, S.pend + w, S.npend - (size_t)w); S.npend -= (size_t)w; }
        if (S.npend) return;
    }
    anet_rep r = { .id = S.id, .dac_rate = D.rate };
    const uint64_t got = atomic_load(&rw), read = atomic_load(&rr);
    const long d = D.open ? S.delay : 0;
    r.played = read > (uint64_t)d ? read - (uint64_t)d : 0;
    r.received = got;
    r.ring = (uint32_t)(got - read);
    if (S.started && !S.drained && D.open) r.flags |= ANET_R_PLAYING;
    if (S.paused) r.flags |= ANET_R_PAUSED;
    if (S.drained) r.flags |= ANET_R_DRAINED;
    if (S.failed) r.flags |= ANET_R_FAILED;
    r.flags |= extra_flags;
    r.reason = (uint32_t)atomic_load(&lost_frames);
    if (S.caps) {                          /* protocol 3 */
        r.v3 = 1;
        r.target = S.caps & ANET_CAP_ADAPT ? atomic_load(&j_goal) : (uint32_t)S.target;
        r.late_us = atomic_load(&j_late_us); r.peak_us = atomic_load(&j_peak_us);
        r.underruns = (uint32_t)S.underruns; r.adjusts = (uint32_t)atomic_load(&j_adjusts);
        r.ring_min = S.rmin == UINT64_MAX ? 0 : (uint32_t)S.rmin; r.ring_max = (uint32_t)S.rmax;
        r.state = state_locked(); r.why = S.why; r.seq = S.seq;
    }
    uint8_t b[ANET_REP3];
    const size_t n = anet_rep_pack(b, &r);
    const ssize_t w = send(S.cfd, b, n, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (w > 0 && (size_t)w < n) { memcpy(S.pend, b + w, n - (size_t)w); S.npend = n - (size_t)w; }
}
static void report_locked(void) { report_locked_f(0); }

static int P_prio = 0, P_nice = -10;      /* the DAC's thread's SCHED_FIFO level (0: SCHED_OTHER at P_nice) */
static int P_swap = 0;                    /* swap here: what is locked matters */
static _Atomic int a_tid, u_tid;          /* the two threads' kernel ids (their nice, their state, SIGXCPU) */
static _Atomic int rt_now;                /* the DAC's thread SCHED_FIFO now */
static _Atomic int xcpu_hit;              /* SIGXCPU: a real-time thread of ours ran 200 ms without blocking */
static const char *STAMP_DEF = "/tmp/audiolaz-recv.backoff";
#ifdef __linux__
static cpu_set_t P_house, P_rt;           /* the housekeeping cores (ours); the two threads' cores */
static int slurp(const char *path, char *b, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) { b[0] = 0; return -1; }
    const size_t k = fread(b, 1, n - 1, f); fclose(f); b[k] = 0;
    return 0;
}
/* "0-3,5" into cs (added), up to the line's end; isolcpus='s flags ("nohz,domain,1-3", "managed_irq,…") and
 * "(null)" skipped */
static void cpus_parse(const char *s, cpu_set_t *cs) {
    while (s && *s && *s != '\n') {
        if (*s == ',' || *s == ' ' || *s == '\t') { s++; continue; }
        if (*s < '0' || *s > '9') { while (*s && *s != ',' && *s != '\n') s++; continue; }   /* a flag: to its comma */
        char *e; const long a = strtol(s, &e, 10); long b = a;
        if (*e == '-') b = strtol(e + 1, &e, 10);
        for (long i = a; i <= b && i < CPU_SETSIZE; i++) if (i >= 0) CPU_SET((int)i, cs);
        s = e;
        while (*s && *s != ',' && *s != '\n') s++;   /* "0-7:2/4" (groups): the rest of it */
    }
}
static void cpus_text(const cpu_set_t *cs, char *o, size_t n) {
    size_t k = 0; o[0] = 0;
    for (int i = 0; i < CPU_SETSIZE && k + 12 < n; i++) {
        if (!CPU_ISSET(i, cs)) continue;
        int j = i; while (j + 1 < CPU_SETSIZE && CPU_ISSET(j + 1, cs)) j++;
        k += (size_t)snprintf(o + k, n - k, j > i ? "%s%d-%d" : "%s%d", k ? "," : "", i, j);
        i = j;
    }
    if (!o[0]) snprintf(o, n, "none");
}
/* the isolated cores: the kernel's own lists, isolcpus= on its line; AUDIOLAZ_ISOLATED (the tests) added */
static void cpus_isolated(cpu_set_t *iso) {
    char b[4096];
    CPU_ZERO(iso);
    if (!slurp("/sys/devices/system/cpu/isolated", b, sizeof b)) cpus_parse(b, iso);
    if (!slurp("/sys/devices/system/cpu/nohz_full", b, sizeof b)) cpus_parse(b, iso);
    if (!slurp("/proc/cmdline", b, sizeof b)) {
        for (char *p = b; (p = strstr(p, "isolcpus=")); p += 9)
            if (p == b || p[-1] == ' ') { char v[256]; snprintf(v, sizeof v, "%.*s", (int)strcspn(p + 9, " \n"), p + 9); cpus_parse(v, iso); }
    }
    const char *t = getenv("AUDIOLAZ_ISOLATED");
    if (t) cpus_parse(t, iso);
}
/* /proc/<pid>/task/<tid>/stat: its comm, state, flags (9), rt_priority (40), policy (41); −1 unreadable */
static int task_stat(const char *path, char *comm, size_t cl, char *state, unsigned long *flags, int *prio, int *policy) {
    char b[1024];
    if (slurp(path, b, sizeof b)) return -1;
    char *o = strchr(b, '('), *c = strrchr(b, ')');
    if (!o || !c || c < o) return -1;
    if (comm) snprintf(comm, cl, "%.*s", (int)(c - o - 1), o + 1);
    char *s = c + 2, *sv = NULL; int f = 3;
    *flags = 0; *prio = 0; *policy = 0; if (state) *state = *s;
    for (char *tok = strtok_r(s, " ", &sv); tok; tok = strtok_r(NULL, " ", &sv), f++) {
        if (f == 9) *flags = strtoul(tok, NULL, 10);
        else if (f == 40) *prio = atoi(tok);
        else if (f == 41) { *policy = atoi(tok); break; }
    }
    return f >= 41 ? 0 : -1;
}
static char task_state(int tid) {
    char p[64], st = '?'; unsigned long fl; int pr, po;
    snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    return task_stat(p, NULL, 0, &st, &fl, &pr, &po) ? '?' : st;
}
/* The real-time threads of other programs whose cores meet `on` (the lowest level, who), and the kernel's IRQ
 * threads (their lowest level; 0 none: hard IRQs). Ours left out. */
typedef struct { int n, lo, irq; char who[112]; } rt_seen;
static void rt_scan(const cpu_set_t *on, rt_seen *r) {
    memset(r, 0, sizeof *r); r->lo = 100;
    const int me = (int)getpid();
    DIR *d = opendir("/proc");
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        const int pid = atoi(e->d_name);
        if (pid <= 0 || pid == me) continue;
        char p[96], comm[64]; unsigned long fl; int pr, po;
        snprintf(p, sizeof p, "/proc/%d/stat", pid);
        if (task_stat(p, comm, sizeof comm, NULL, &fl, &pr, &po)) continue;
        if (fl & 0x00200000) {                                   /* PF_KTHREAD: only the IRQ threads matter */
            if ((po == SCHED_FIFO || po == SCHED_RR) && !strncmp(comm, "irq/", 4) && (!r->irq || pr < r->irq)) r->irq = pr;
            continue;
        }
        snprintf(p, sizeof p, "/proc/%d/task", pid);
        DIR *td = opendir(p);
        if (!td) continue;
        for (struct dirent *t; (t = readdir(td));) {
            const int tid = atoi(t->d_name);
            if (tid <= 0) continue;
            char tp[128], tc[64], st[4096];
            snprintf(tp, sizeof tp, "/proc/%d/task/%d/stat", pid, tid);
            if (task_stat(tp, tc, sizeof tc, NULL, &fl, &pr, &po)) continue;
            if (po != SCHED_FIFO && po != SCHED_RR && po != 6 /* SCHED_DEADLINE */) continue;
            snprintf(tp, sizeof tp, "/proc/%d/task/%d/status", pid, tid);
            if (slurp(tp, st, sizeof st)) continue;
            const char *ca = strstr(st, "Cpus_allowed_list:");
            cpu_set_t cs, x; CPU_ZERO(&cs);
            if (ca) cpus_parse(ca + 18, &cs); else CPU_OR(&cs, &cs, on);
            CPU_AND(&x, &cs, on);
            if (!CPU_COUNT(&x)) continue;
            if (po == 6) pr = 99;                                /* a deadline thread: above everything */
            r->n++;
            if (pr < r->lo) { r->lo = pr; snprintf(r->who, sizeof r->who, "« %s » %s %d", tc, po == SCHED_RR ? "RR" : po == 6 ? "DEADLINE" : "FIFO", pr); }
        }
        closedir(td);
    }
    closedir(d);
}
/* the level allowed under what is found (auto: 40 at most; explicit: as asked, unless above them without --force) */
static int rt_cap(int want, const rt_seen *r, int explicit_) {
    int cap = 99;
    if (r->n && r->lo - 1 < cap) cap = r->lo - 1;
    if (r->irq && r->irq - 1 < cap) cap = r->irq - 1;
    if (explicit_ && o_force) return want;
    return want < cap ? want : cap;
}
#endif
/* a thread of ours (t, its kernel id tid) on cs at prio (SCHED_FIFO; 0: SCHED_OTHER at nice) */
static int thr_apply(pthread_t t, int tid, int prio, int nice_v) {
    int e = 0;
#ifdef __linux__
    if (pthread_setaffinity_np(t, sizeof P_rt, &P_rt)) lg("cores refused (%s)", strerror(errno));
#endif
    if (prio > 0) {
        const struct sched_param sp = { .sched_priority = prio };
        if (!(e = pthread_setschedparam(t, SCHED_FIFO, &sp))) return 0;
    }
    const struct sched_param s0 = { .sched_priority = 0 };
    pthread_setschedparam(t, SCHED_OTHER, &s0);
#ifdef __linux__
    if (tid > 0) setpriority(PRIO_PROCESS, (id_t)tid, nice_v);   /* Linux: a thread's own nice (refused without the right: left at 0) */
#else
    (void)tid; (void)nice_v;
#endif
    return e;
}
static int my_tid(void) {
#ifdef __linux__
    return (int)syscall(SYS_gettid);
#else
    return 0;
#endif
}
/* the calling thread placed: the DAC's (udp 0) at P_prio, the packets' one below */
static void rt_self(int udp) {
    const int tid = my_tid();
#ifdef __linux__
    pthread_setname_np(pthread_self(), udp ? "az-net" : "az-dac");   /* ps -L, /proc/…/task/…/comm: which is which */
#endif
    atomic_store(udp ? &u_tid : &a_tid, tid);
    const int prio = udp ? (P_prio > 1 ? P_prio - 1 : 0) : P_prio;
    const int e = thr_apply(pthread_self(), tid, prio, P_nice);
    if (prio > 0 && e) { if (!udp) lg("no real-time priority (%s): SCHED_OTHER at nice %d", strerror(e), P_nice); }
    else if (prio > 0 && !udp) atomic_store(&rt_now, 1);
#if defined(__linux__)
    if (P_swap) {   /* this thread's stack locked (with swap only), within its 256 KB */
        pthread_attr_t a; void *sa; size_t sz;
        if (!pthread_getattr_np(pthread_self(), &a)) { if (!pthread_attr_getstack(&a, &sa, &sz) && sz <= (1u << 20)) mlock(sa, sz); pthread_attr_destroy(&a); }
    }
#endif
}
/* SIGXCPU (RLIMIT_RTTIME): both threads out of real time at once, from the handler (plain system calls) */
static void on_xcpu(int s) {
    (void)s;
#ifdef __linux__
    const struct sched_param p = { .sched_priority = 0 };
    const int a = atomic_load(&a_tid), u = atomic_load(&u_tid);
    if (a > 0) sched_setscheduler(a, SCHED_OTHER, &p);
    if (u > 0) sched_setscheduler(u, SCHED_OTHER, &p);
#endif
    atomic_store(&rt_now, 0); atomic_store(&xcpu_hit, 1);
}

static const char *stamp_path(void) { const char *p = getenv("AUDIOLAZ_RECV_STAMP"); return p && *p ? p : STAMP_DEF; }
static double stamp_age(char *why, size_t wl) {
    FILE *f = fopen(stamp_path(), "r");
    if (!f) return -1;
    char b[256] = "", *e;
    const int got = fgets(b, sizeof b, f) != NULL;
    fclose(f);
    const long t = got ? strtol(b, &e, 10) : 0;
    if (!got || e == b) return -1;
    (void)strtol(e, &e, 10);   /* the level */
    while (*e == ' ') e++;
    snprintf(why, wl, "%.*s", (int)strcspn(e, "\n"), e);
    const double age = difftime(time(NULL), (time_t)t);
    return age >= 0 ? age : -1;
}
static void stamp_write(int level, const char *why) {
    FILE *f = fopen(stamp_path(), "w");
    if (f) { fprintf(f, "%lld %d %s\n", (long long)time(NULL), level, why); fclose(f); }
}

/* At the start: the cores, the level, the limit, the memory; one line each said. */
static void place_init(void) {
    {   /* our limit: 200 ms without blocking at real time, SIGXCPU; 2 s, killed */
#ifdef RLIMIT_RTTIME
        struct rlimit rl;
        if (!getrlimit(RLIMIT_RTTIME, &rl)) {
            rlim_t soft = 200000, hard = 2000000;
            if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < hard) hard = rl.rlim_max;
            if (soft > hard) soft = hard;
            rl.rlim_cur = soft; rl.rlim_max = hard;
            if (setrlimit(RLIMIT_RTTIME, &rl)) lg("RLIMIT_RTTIME not set (%s)", strerror(errno));
        }
        signal(SIGXCPU, on_xcpu);
#endif
    }
    {   /* swap: anything locked only if pages can go out at all */
        char b[512]; FILE *f = fopen("/proc/swaps", "r"); int lines = 0;
        while (f && fgets(b, sizeof b, f)) lines++;
        if (f) fclose(f);
        P_swap = lines > 1;
    }
#ifdef __linux__
    cpu_set_t allow, iso;
    CPU_ZERO(&allow);
    if (sched_getaffinity(0, sizeof allow, &allow)) for (int i = 0; i < 64; i++) CPU_SET(i, &allow);
    cpus_isolated(&iso);
    CPU_ZERO(&P_house);
    for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &allow) && !CPU_ISSET(i, &iso)) CPU_SET(i, &P_house);
    const int all_iso = !CPU_COUNT(&P_house);
    if (all_iso) P_house = allow;                         /* started on isolated cores only: staying there, never real time */
    P_rt = P_house;
    int explicit_cpu = 0;
    if (o_cpu >= 0) {
        if (o_cpu >= CPU_SETSIZE || !CPU_ISSET(o_cpu, &allow)) { lg("--cpu %d: not a core this process may use: ours instead", o_cpu); o_cpu = -1; }
        else if (CPU_ISSET(o_cpu, &iso) && !o_force) {
            rt_seen r; cpu_set_t one; CPU_ZERO(&one); CPU_SET(o_cpu, &one); rt_scan(&one, &r);
            lg("--cpu %d refused: an isolated core (isolcpus / nohz_full: its own kernel work waits for real-time threads there)%s%s; --force to insist",
               o_cpu, r.n ? ", " : "", r.n ? r.who : "");
            o_cpu = -1;
        } else { CPU_ZERO(&P_rt); CPU_SET(o_cpu, &P_rt); explicit_cpu = 1; }
    }
    if (sched_setaffinity(0, sizeof P_house, &P_house)) lg("cores refused (%s)", strerror(errno));   /* the main thread: every thread after inherits */
    rt_seen r; rt_scan(&P_rt, &r);
    char why[400] = "";
    char sw[200]; const double sage = stamp_age(sw, sizeof sw);
    if (o_rt == 0) { P_prio = 0; snprintf(why, sizeof why, "--rt 0"); }
    else if (o_rt > 0) {
        P_prio = rt_cap(o_rt, &r, 1);
        if (P_prio < o_rt) snprintf(why, sizeof why, "--rt %d lowered below %s%s", o_rt, r.n && r.lo - 1 == P_prio ? r.who : "the kernel's IRQ threads", ": --force to insist");
        if (P_prio < 1) P_prio = 0;
    } else if (all_iso) { P_prio = 0; snprintf(why, sizeof why, "only isolated cores here"); }
    else if (CPU_COUNT(&P_house) < 2 && !explicit_cpu) { P_prio = 0; snprintf(why, sizeof why, "one core left to the system: shared with it"); }
    else if (sage >= 0 && sage < 6 * 3600) { P_prio = 0; snprintf(why, sizeof why, "backed off %.0f min ago (%s)", sage / 60, sw); }
    else {
        P_prio = rt_cap(40, &r, 0);
        if (P_prio < 40) snprintf(why, sizeof why, "below %s", r.n && r.lo - 1 == P_prio ? r.who : "the kernel's IRQ threads");
        if (P_prio < 5) { P_prio = 0; snprintf(why, sizeof why, "another program's real time there (%s)", r.who); }
    }
    char ta[64], ti[64], tr[64];
    cpus_text(&allow, ta, sizeof ta); cpus_text(&iso, ti, sizeof ti); cpus_text(&P_rt, tr, sizeof tr);
    rt_seen ri; int n_iso_rt = 0;
    if (CPU_COUNT(&iso)) { rt_scan(&iso, &ri); n_iso_rt = ri.n; }
    lg("cores: may use %s, isolated %s%s%s%s, ours %s; %s %d%s%s%s", ta, ti, n_iso_rt ? " (" : "", n_iso_rt ? ri.who : "", n_iso_rt ? " there)" : "", tr,
       P_prio ? "SCHED_FIFO" : "SCHED_OTHER, nice", P_prio ? P_prio : P_nice, why[0] ? " (" : "", why, why[0] ? ")" : "");
    {
        char b[64] = ""; long rtu = 0, rtp = 0;
        if (!slurp("/proc/sys/kernel/sched_rt_runtime_us", b, sizeof b)) rtu = atol(b);
        if (!slurp("/proc/sys/kernel/sched_rt_period_us", b, sizeof b)) rtp = atol(b);
        if (P_prio && rtu < 0) lg("the kernel's real-time throttling is off (sched_rt_runtime_us -1): ours instead (RLIMIT_RTTIME 200 ms, the guard)");
        else if (P_prio && rtp > 0) lg("the kernel's real-time throttling: %ld µs in %ld; ours besides (RLIMIT_RTTIME 200 ms, the guard)", rtu, rtp);
    }
#else
    P_prio = o_rt > 0 ? o_rt : o_rt < 0 ? 40 : 0;
    (void)stamp_age;
#endif
}
/* At each stream (the control thread): another program's real-time thread may have come on our cores since: the
 * level lowered under it (never raised again: a back-off is for the process's life) */
static pthread_t th_a, th_u;
static int place_recheck(void) {
#ifdef __linux__
    if (!atomic_load(&rt_now) || P_prio <= 0) return 0;
    rt_seen r; rt_scan(&P_rt, &r);
    const int np = rt_cap(P_prio, &r, o_rt > 0);
    if (np >= P_prio) return 0;
    const int was = P_prio;
    P_prio = np >= 5 ? np : 0;
    if (!P_prio) atomic_store(&rt_now, 0);
    thr_apply(th_a, atomic_load(&a_tid), P_prio, P_nice);
    thr_apply(th_u, atomic_load(&u_tid), P_prio > 1 ? P_prio - 1 : 0, P_nice);
    lg("real time %d → %s %d: %s on our cores now", was, P_prio ? "SCHED_FIFO" : "SCHED_OTHER, nice", P_prio ? P_prio : P_nice, r.n && r.lo - 1 <= np ? r.who : "an IRQ thread");
    return 1;
#else
    return 0;
#endif
}

/* the guard's view of the two threads: each pass counted, the DAC's thread parked or not, its longest pass of
 * the last second (µs) */
static _Atomic unsigned long a_beat, u_beat;
static _Atomic int a_parked, a_want_park;
static _Atomic uint32_t a_gap_us;
static double test_spin_s = 0; static int test_spin_park = 0;   /* AUDIOLAZ_RECV_TEST_SPIN=<s>[:park] (the guard's test) */
static double t_stream0;

/* ── the DAC's thread: a period at a time ── */
static void *audio_main(void *arg) {
    (void)arg;
    rt_self(0);
    static int32_t zero[2 * 16384];
    unsigned per_report = 1, since = 0;
    double t_wake = 0, t_pass = 0;
    pthread_mutex_lock(&lk);
    for (;;) {
        atomic_fetch_add(&a_beat, 1);
        if (S.want_park || !D.open) {
            S.parked = 1; atomic_store(&a_parked, 1); pthread_cond_broadcast(&cv);
            while (S.want_park || !D.open) pthread_cond_wait(&cv, &lk);
            S.parked = 0; atomic_store(&a_parked, 0); t_wake = 0; t_pass = 0; since = 0;
            per_report = (unsigned)((uint64_t)D.rate * ANET_REPORT_MS / 1000 / (D.period ? D.period : 1)); if (!per_report) per_report = 1;
            continue;
        }
        {   /* the longest pass, for the guard */
            const double t = mono();
            if (t_pass > 0) { const uint32_t g = (uint32_t)((t - t_pass) * 1e6); if (g > atomic_load(&a_gap_us)) atomic_store(&a_gap_us, g); }
            t_pass = t;
        }
        if (test_spin_s > 0 && S.started && mono() - t_stream0 > test_spin_s) {   /* the guard's test: a thread gone mad */
            pthread_mutex_unlock(&lk);
            lg("test: the DAC's thread spins (AUDIOLAZ_RECV_TEST_SPIN)");
            test_spin_s = 0;
            while (!(test_spin_park && atomic_load(&a_want_park))) { }
            pthread_mutex_lock(&lk);
            continue;
        }
        const uint64_t have = atomic_load(&rw) - atomic_load(&rr);
        if (!S.started && (have >= S.target || (S.ended && have))) S.started = 1;
        if (S.ended && !have) {                            /* all played out: the DAC's buffer heard, then let go */
            const long d = dev_delay();
            S.delay = d > 0 ? d : 0;
            if (d <= 0 || S.tw == 0) { S.drained = 1; report_locked(); dev_close(); continue; }
            pthread_mutex_unlock(&lk); usleep(2000); pthread_mutex_lock(&lk);
            continue;
        }
        const int started = S.started, paused = S.paused, running = S.tw > 0;
        pthread_mutex_unlock(&lk);
        if (!started && !running) { usleep((useconds_t)o_period_us); pthread_mutex_lock(&lk); continue; }   /* filling, the DAC not started */
        long a = dev_avail();
        if (a >= 0 && a < (long)D.period) {
            dev_wait();
            const double t = mono();
            if (t_wake > 0) h_add(&H_wake, (t - t_wake) * 1e6, step_wake);
            t_wake = t;
            a = dev_avail();
        }
        const double c0 = cputime();
        long w = 0; int data = 0;
        if (a < 0) w = a;
        else if (a >= (long)D.period) {
            unsigned long n = (unsigned long)a - (unsigned long)a % D.period;   /* whole periods */
            const uint64_t r = atomic_load(&rr), h = atomic_load(&rw) - r;
            if (started && !paused && h) {
                if (n > h) n = (unsigned long)h;
                const uint64_t at = r & ring_mask;
                if (n > ring_mask + 1 - at) n = (unsigned long)(ring_mask + 1 - at);
                w = dev_write(ring + 2 * at, n, 1);
                data = 1;
                if (w > 0) atomic_fetch_add(&rr, (uint64_t)w);
            } else {                                       /* paused, filling again, or dry: a period of zeros, the DAC kept locked */
                w = dev_write(zero, D.period < 16384 ? D.period : 16384, 0);
            }
        }
        /* never a busy loop at real-time priority: a pass that wrote nothing (the DAC's buffer full though its
         * wait said otherwise, -EAGAIN, a device going away) sleeps a quarter of a period before the next */
        if (w == 0 || w == OUT_AGAIN_) usleep((useconds_t)(o_period_us / 4 + 1));
        pthread_mutex_lock(&lk);
        if (w == OUT_AGAIN_) continue;
        if (w < 0 && started && !paused && dev_recover(w) == 0) {   /* its buffer run dry: a gap, the music going on */
            S.xruns++; S.underruns++; if (!S.dry) S.gaps++; S.dry = 1;
            continue;
        }
        if (w < 0) { S.failed = 1; S.why = ANET_WHY_DAC; report_locked(); dev_close(); continue; }
        if (w > 0) { S.tw += (uint64_t)w; if (!data) { S.zeros += (uint64_t)w; if (started && !paused) { S.underruns++; if (!S.dry) S.gaps++; S.dry = 1; } } else S.dry = 0; }
        if (started && !paused && !S.ended) {             /* the ring this stream, while it plays */
            const uint64_t h2 = atomic_load(&rw) - atomic_load(&rr);
            if (h2 < S.rmin) S.rmin = h2;
            if (h2 > S.rmax) S.rmax = h2;
        }
        const long d = dev_delay();
        S.delay = d > 0 ? d : 0;
        if (++since >= per_report) { since = 0; report_locked(); }
        h_add(&H_work, (cputime() - c0) * 1e6, step_work);
    }
    return NULL;
}

/* ── the packets' thread ── */
static int ufd = -1;
static void *udp_main(void *arg) {
    (void)arg;
    rt_self(1);
    static uint8_t b[2048];
    double t_last = 0;
    uint32_t last_id = 0;
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof from;
        const ssize_t n = recvfrom(ufd, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        atomic_fetch_add(&u_beat, 1);
        if (n <= 0) continue;
        uint32_t id, frames; uint64_t index;
        if (anet_dhdr_parse(b, (size_t)n, &id, &index, &frames)) continue;
        pthread_mutex_lock(&lk);
        const int ok = S.live && id == S.id && from.sin_addr.s_addr == S.from.s_addr;
        const int stamped = ok && (S.caps & ANET_CAP_STAMP);
        const unsigned rate = S.rate;
        pthread_mutex_unlock(&lk);
        if (!ok) continue;
        const double t = mono();
        atomic_store(&t_last_pkt, t);
        if (id != last_id) { last_id = id; t_last = 0; }   /* each stream's own rhythm */
        if (t_last > 0) h_add(&H_pkt, (t - t_last) * 1e6, step_pkt);
        t_last = t;
        if (stamped) j_packet(anet_dhdr_flags(b), t, rate);   /* its lateness */
        const uint64_t w = atomic_load(&rw), r = atomic_load(&rr);
        if (index < w) { atomic_fetch_add(&late_pkts, 1); continue; }   /* late or repeated */
        if (index + frames - r > ring_mask + 1) { atomic_fetch_add(&lost_frames, frames); continue; }   /* no room (the sender keeps the target: never) */
        for (uint64_t i = w; i < index; i++) { const uint64_t at = i & ring_mask; ring[2 * at] = ring[2 * at + 1] = 0; }   /* a gap: zeros */
        if (index > w) atomic_fetch_add(&lost_frames, (unsigned long)(index - w));
        const uint64_t at = index & ring_mask, first = ring_mask + 1 - at < frames ? ring_mask + 1 - at : frames;
        memcpy(ring + 2 * at, b + ANET_DHDR, (size_t)first * 8);
        if (first < frames) memcpy(ring, b + ANET_DHDR + first * 8, (size_t)(frames - first) * 8);
        atomic_store(&rw, index + frames);
    }
    return NULL;
}

static void park(void) { pthread_mutex_lock(&lk); S.want_park = 1; atomic_store(&a_want_park, 1); pthread_cond_broadcast(&cv); while (!S.parked) pthread_cond_wait(&cv, &lk); }
static void unpark(void) { S.want_park = 0; atomic_store(&a_want_park, 0); pthread_cond_broadcast(&cv); pthread_mutex_unlock(&lk); }
/* the same within s seconds (the guard's back-off: the DAC's thread may be stuck): 0 parked (lk held), −1 not */
static int park_timed(double s) {
    struct timespec ab; clock_gettime(CLOCK_REALTIME, &ab);
    ab.tv_sec += (time_t)s; ab.tv_nsec += (long)((s - (double)(time_t)s) * 1e9);
    if (ab.tv_nsec >= 1000000000L) { ab.tv_sec++; ab.tv_nsec -= 1000000000L; }
#ifdef __linux__
    if (pthread_mutex_timedlock(&lk, &ab)) return -1;
#else
    pthread_mutex_lock(&lk);
#endif
    S.want_park = 1; atomic_store(&a_want_park, 1); pthread_cond_broadcast(&cv);
    while (!S.parked)
        if (pthread_cond_timedwait(&cv, &lk, &ab) == ETIMEDOUT && !S.parked) { S.want_park = 0; atomic_store(&a_want_park, 0); pthread_mutex_unlock(&lk); return -1; }
    return 0;
}

static void stats_print(void) {
    if (!o_stats) return;
    pthread_mutex_lock(&lk);
    hist p = H_pkt, wk = H_wake, wo = H_work;
    memset(&H_pkt, 0, sizeof H_pkt); memset(&H_wake, 0, sizeof H_wake); memset(&H_work, 0, sizeof H_work);
    const unsigned long u = S.underruns, gp = S.gaps, xr = S.xruns;
    const double ms = 1000.0 / (S.rate ? S.rate : 1);
    const double rmin = S.rmin == UINT64_MAX ? 0 : S.rmin * ms, rmax = S.rmax * ms, rnow = (double)(atomic_load(&rw) - atomic_load(&rr)) * ms;
    const unsigned caps = S.caps;
    pthread_mutex_unlock(&lk);
    if (p.cnt) lg("stats: packets %lu, gap µs mean %.0f p1 %.0f p50 %.0f p99 %.0f max %.0f", p.cnt, p.sum / p.cnt, h_pct(&p, .01, step_pkt), h_pct(&p, .5, step_pkt), h_pct(&p, .99, step_pkt), p.max);
    if (wk.cnt) lg("stats: DAC wakeups %lu, interval µs mean %.0f p1 %.0f p50 %.0f p99 %.0f max %.0f", wk.cnt, wk.sum / wk.cnt, h_pct(&wk, .01, step_wake), h_pct(&wk, .5, step_wake), h_pct(&wk, .99, step_wake), wk.max);
    if (wo.cnt) lg("stats: work a period, CPU µs mean %.1f p99 %.0f max %.0f; underruns %lu (in %lu gaps; the DAC's buffer run dry %lu), frames lost %lu, late packets %lu", wo.sum / wo.cnt, h_pct(&wo, .99, step_work), wo.max, u, gp, xr, atomic_load(&lost_frames), atomic_load(&late_pkts));
    lg("stats: ring min %.0f ms, max %.0f ms, at the end %.0f ms; %s", rmin, rmax, rnow,
       caps & ANET_CAP_ADAPT ? "" : "fixed (no protocol 3 adapting)");
    if (caps & ANET_CAP_ADAPT) {
        const uint32_t g = atomic_load(&j_goal);
        lg("stats: target %.0f ms now (start %d, ceiling %d), %lu adjustments; lateness %.1f ms the last second, peak %.1f ms remembered",
           g * ms, o_ring_ms, o_ring_max_ms, atomic_load(&j_adjusts), atomic_load(&j_late_us) / 1000.0, atomic_load(&j_peak_us) / 1000.0);
    }
    if (o_null) { const unsigned long x = atomic_exchange(&null_xruns, 0); if (x) lg("stats: null clock not fed in time %lu times (this thread late)", x); }
}

static int recv_all(int fd, void *p, size_t n, int ms) {
    uint8_t *c = p;
    const double end = mono() + ms / 1000.0;
    while (n) {
        struct pollfd q = { fd, POLLIN, 0 };
        const double left = end - mono();
        if (left <= 0 || poll(&q, 1, (int)(left * 1000) + 1) <= 0) return -1;
        const ssize_t r = recv(fd, c, n, 0);
        if (r <= 0) return -1;
        c += r; n -= (size_t)r;
    }
    return 0;
}
static void refuse(int fd, uint32_t id, uint32_t why) {
    anet_rep r = { .id = id, .flags = ANET_R_REFUSED, .reason = why };
    uint8_t b[ANET_REP3]; const size_t n = anet_rep_pack(b, &r); send_all(fd, b, n);
}

/* A new connection as read: its header (e: ANET_E_HEADER / ANET_E_RATE, 0 good), and who sends it (its
 * ANET_M_HELLO: who, its id and name, take; none from a player before 14/10), its address. */
typedef struct { anet_hdr h; int e, who, take; char uid[40], name[96]; struct in_addr addr; } conn_t;
static void one_name(char *s) { for (; *s; s++) if ((unsigned char)*s < 0x20 || *s == ';' || *s == 0x7f) *s = ' '; }
/* 1: INFO answered (closed by the caller); −1: nothing read; 0: a stream asked (k->e said) */
static int conn_read(int fd, conn_t *k) {
    uint8_t hb[ANET_HDR];
    memset(k, 0, sizeof *k);
    if (recv_all(fd, hb, sizeof hb, 3000)) return -1;
    k->e = anet_hdr_parse(hb, &k->h);
    if (!k->e && k->h.rate == 0) {                     /* INFO */
        uint8_t ib[ANET_INFO] = { 0 };
        anet_p32(ib, ANET_IMAGIC);
        const int n = snprintf((char *)ib + 4, ANET_INFO - 4, "id=%s;dac=%s;max=%u;v=%d", o_id, o_name, o_max, o_v2 ? 2 : ANET_PROTO);
        /* the DAC's identity, whole or not at all (ANET_INFO is fixed: an older player reads that many bytes) */
        if (o_hw[0] && n > 0 && (size_t)n + 4 + strlen(o_hw) + 1 < ANET_INFO - 4) snprintf((char *)ib + 4 + n, ANET_INFO - 4 - (size_t)n, ";hw=%s", o_hw);
        send_all(fd, ib, sizeof ib);
        return 1;
    }
    struct sockaddr_in peer; socklen_t pl = sizeof peer;
    memset(&peer, 0, sizeof peer);
    getpeername(fd, (struct sockaddr *)&peer, &pl);
    k->addr = peer.sin_addr;
    /* its HELLO, sent with its header (a --v2 receiver leaves it to the messages' loop: skipped there, as before) */
    if (!k->e && !o_v2 && (k->h.caps & ANET_CAP_WHO)) {
        uint8_t mh[8]; char pay[ANET_HELLO_MAX + 1], tk[8];
        uint32_t len = 0;
        if (recv_all(fd, mh, 8, 2000) || anet_g32(mh) != ANET_M_HELLO || (len = anet_g32(mh + 4)) > ANET_HELLO_MAX || recv_all(fd, pay, len, 2000)) { k->e = ANET_E_HEADER; return 0; }
        pay[len] = 0;
        anet_meta_get(pay, "player", k->uid, sizeof k->uid); anet_meta_get(pay, "name", k->name, sizeof k->name); anet_meta_get(pay, "take", tk, sizeof tk);
        one_name(k->uid); one_name(k->name);
        k->who = 1; k->take = tk[0] == '1';
    }
    if (!k->e && k->h.rate > o_max) k->e = ANET_E_RATE;
    return 0;
}

/* ── one DAC, one player: the one playing, or the last one (ANET_KEEP_MS after its last sign of life) ──
 * The control thread's own. t_alive: its connection's last message (pings: every ANET_PING_MS) or its stream's stop;
 * its packets and pings besides (hold_alive). */
static struct { int set; char uid[40], name[96]; struct in_addr addr; double t_alive, t_start; } HOLD;
static struct { char uid[40]; struct in_addr addr; double t_line; unsigned long n; } REF;   /* the refusals journaled */
static void hold_name(char *out, size_t len) {
    if (HOLD.name[0]) snprintf(out, len, "%s", HOLD.name);
    else snprintf(out, len, "another player (%s)", inet_ntoa(HOLD.addr));   /* a player before 14/10: no name said */
}
static double hold_alive(int fd) {
    double t = HOLD.t_alive;
    pthread_mutex_lock(&lk);
    const int live = fd >= 0 && S.live, paused = S.paused, state = !!(S.caps & ANET_CAP_STATE);
    const double tp = S.t_ping;
    pthread_mutex_unlock(&lk);
    if (live) {
        const double tk = atomic_load(&t_last_pkt);
        if (tk > t) t = tk;
        if (tp > t) t = tp;
        if (paused && !state) t = mono();   /* a version-2 player paused: no ping, no packet, still there */
    }
    return t;
}
/* the newcomer k: 0 it plays (why: what the stream there, if any, is told), 1 refused (the DAC another player's) */
static int busy_for(const conn_t *k, int fd, unsigned *why) {
    *why = ANET_WHY_TAKEN;
    if (o_v2 || !HOLD.set) return 0;                   /* (--v2: the old way, the newest wins) */
    /* the same player: its id; two players before 14/10 (no id): their address, as before (behind a NAT: one) */
    if (k->uid[0] ? !strcmp(k->uid, HOLD.uid) : !HOLD.uid[0] && k->addr.s_addr == HOLD.addr.s_addr) { *why = ANET_WHY_AGAIN; return 0; }
    if (k->take) return 0;
    if (mono() - hold_alive(fd) > ANET_KEEP_MS / 1000.0) { *why = ANET_WHY_LOST; return 0; }
    return 1;
}
static void refuse_busy(int fd, const conn_t *k) {
    char by[128];
    hold_name(by, sizeof by);
    const double held = mono() - HOLD.t_start;
    if (!k->who) refuse(fd, k->h.id, ANET_E_DAC);
    else {
        refuse(fd, k->h.id, ANET_E_BUSY);
        uint8_t ib[ANET_INFO] = { 0 };
        anet_p32(ib, ANET_BMAGIC);
        snprintf((char *)ib + 4, ANET_INFO - 4, "by=%.100s;player=%s;for=%.0f", by, HOLD.uid, held);
        send_all(fd, ib, sizeof ib);
    }
    /* a line for each player refused, then a line a minute while it keeps asking */
    const double t = mono();
    const int same = !strcmp(REF.uid, k->uid) && (k->uid[0] || REF.addr.s_addr == k->addr.s_addr);
    if (same && t - REF.t_line < 60) { REF.n++; return; }
    char who[128];
    if (k->name[0]) snprintf(who, sizeof who, "« %s »", k->name); else snprintf(who, sizeof who, "a player before 14/10 (%s)", inet_ntoa(k->addr));
    if (same && REF.n) lg("stream %u from %s refused again (%lu times this minute): busy, playing from « %s » for %.0f s", k->h.id, who, REF.n + 1, by, held);
    else lg("stream %u from %s refused: busy, playing from « %s » for %.0f s (the first player keeps the DAC)", k->h.id, who, by, held);
    snprintf(REF.uid, sizeof REF.uid, "%s", k->uid); REF.addr = k->addr; REF.t_line = t; REF.n = 0;
}
static void hold_set(const conn_t *k) {
    HOLD.set = 1;
    snprintf(HOLD.uid, sizeof HOLD.uid, "%s", k->uid); snprintf(HOLD.name, sizeof HOLD.name, "%s", k->name);
    HOLD.addr = k->addr; HOLD.t_alive = HOLD.t_start = mono();
    if (!strcmp(REF.uid, k->uid)) REF.t_line = 0;
}

/* A stream asked (k: read, good, let in): the DAC ready (0) or refused (−1, told). */
static unsigned cur_rate = 0;
static int stream_begin(int fd, const conn_t *k) {
    const anet_hdr h = k->h;
    /* the same rate, the last stream playing out: its tail heard first (the DAC kept open, fed), then this one */
    const int follow = D.open && cur_rate == h.rate && S.ended && !S.failed;
    place_recheck();   /* another program's real time on our cores since: below it */
    t_stream0 = mono();
    if (follow) for (int i = 0; i < 400 && atomic_load(&rr) < atomic_load(&rw); i++) usleep(5000);
    park();
    S.live = 0;
    if (!(follow && D.open)) {
        dev_close();
        S.tw = S.zeros = 0;
        if (dev_open(h.rate)) { S.why = ANET_WHY_DAC; unpark(); refuse(fd, h.id, ANET_E_DAC); lg("stream %u: the DAC %s did not open at %u Hz", h.id, dac_dev, h.rate); return -1; }
        cur_rate = h.rate;
    }
    atomic_store(&rw, 0); atomic_store(&rr, 0);
    S.delay = 0;
    S.id = h.id; S.rate = h.rate; S.started = 0; S.ended = 0; S.paused = 0; S.drained = 0; S.failed = 0; S.underruns = 0; S.gaps = 0; S.xruns = 0; S.dry = 0;
    atomic_store(&lost_frames, 0); atomic_store(&late_pkts, 0);
    S.target = (uint64_t)h.rate * (unsigned)o_ring_ms / 1000;
    /* protocol 3: what this player offers that we follow; the jitter's memory kept for the same player, its
     * target there already (the stream still starts on --ring-ms: at once) */
    S.caps = o_v2 ? 0 : h.caps & (ANET_CAP_STAMP | ANET_CAP_STATE | ANET_CAP_WHO | (o_adapt ? ANET_CAP_ADAPT : 0));
    if (S.caps & ANET_CAP_ADAPT) S.caps |= ANET_CAP_STAMP;   /* adapting needs the stamps */
    S.caps &= h.caps;
    if (!(S.caps & ANET_CAP_P3)) S.caps = 0;               /* who it is alone is not protocol 3: version 2 */
    char jkey[48];   /* the player by its id (behind a NAT, its address is every player's) */
    if (k->uid[0]) snprintf(jkey, sizeof jkey, "id:%s", k->uid); else snprintf(jkey, sizeof jkey, "ip:%s", inet_ntoa(k->addr));
    if (strcmp(jkey, j_from) || !(S.caps & ANET_CAP_ADAPT)) { J.mn = 0; J.mh = 0; atomic_store(&j_peak_us, 0); }
    snprintf(j_from, sizeof j_from, "%s", jkey);
    j_reset_stream();
    atomic_store(&j_adjusts, 0);
    atomic_store(&j_goal, (uint32_t)S.target);
    atomic_store(&j_live, 1);                              /* until its first ping says */
    if (S.caps & ANET_CAP_ADAPT) j_target(h.rate, mono());
    S.rmin = UINT64_MAX; S.rmax = 0; S.why = ANET_WHY_NONE; S.seq = 0; S.t_ping = mono(); S.npend = 0;
    atomic_store(&t_last_pkt, mono());
    S.from = k->addr;
    S.cfd = fd;
    S.live = 1;
    /* accepted: the start in `reason`; protocol 3 told (ANET_R_P3) with what of it in `received` */
    anet_rep r = { .id = h.id, .flags = ANET_R_ACCEPTED | (S.caps ? ANET_R_P3 : 0), .reason = (uint32_t)S.target, .dac_rate = h.rate, .received = S.caps };
    uint8_t b[ANET_REP3]; const size_t n = anet_rep_pack(b, &r);
    const unsigned caps = S.caps;
    const double tg = atomic_load(&j_goal) * 1000.0 / h.rate;
    unpark();
    send_all(fd, b, n);
    char from[140];
    if (k->name[0]) snprintf(from, sizeof from, " from « %s »%s", k->name, k->take ? " (taking it over)" : ""); else snprintf(from, sizeof from, " from %s", inet_ntoa(k->addr));
    lg("stream %u%s: %u Hz, %s, packets of %u frames every %u µs%s; %s", h.id, from, h.rate, h.fmt == ANET_F_DOP ? "DoP" : h.fmt == ANET_F_S24_IN_S32 ? "24 bits in 32" : "32 bits", h.pkt_frames, h.pkt_us, follow ? ", after the last one" : "",
       !caps ? "version 2 (the ring fixed)" : caps & ANET_CAP_ADAPT ? "protocol 3, the ring sized by the network" : "protocol 3, the ring fixed");
    if (caps & ANET_CAP_ADAPT && tg > o_ring_ms) lg("stream %u: starts on %d ms, the ring asked at %.0f ms at once (the jitter remembered from this player)", h.id, o_ring_ms, tg);
    return 0;
}

/* the stream ended here: the player told why (a last report, protocol 3), one line in the journal.
 * within `timeout` s (> 0: the guard's back-off, the DAC's thread perhaps stuck): −1 not done */
static int stop_here_t(unsigned why, unsigned id, double timeout) {
    if (timeout > 0) { if (park_timed(timeout)) return -1; }
    else park();
    S.why = why;
    if (S.caps & ANET_CAP_STATE) report_locked_f(ANET_R_STOPPED);
    S.live = 0; S.cfd = -1; dev_close(); S.ended = 1;
    unpark();
    if (why != ANET_WHY_LOST) HOLD.t_alive = mono();      /* its player's last sign: the stop (silent: its own) */
    lg("stream %u stopped by the receiver: %s", id, anet_why_text(why));
    return 0;
}
static void stop_here(unsigned why, unsigned id) { stop_here_t(why, id, 0); }

static volatile sig_atomic_t quit_sig = 0;
static int quit_pipe[2] = { -1, -1 };

/* ── the guard: a plain thread on our cores, every 250 ms ──────────────────────────────────────────────────
 * Backs off one step (the real time dropped: both threads SCHED_OTHER at nice −10 on the housekeeping cores; the
 * stream goes on) when: SIGXCPU (a real-time thread of ours ran 200 ms without blocking); either thread above 25 % of a
 * core 2 s running at real time (they take a few % at 768 kHz); the guard itself woken 1.5 s late while ours are
 * real-time (the cores starved); the DAC's thread runnable and not run 2 s, or the packets' with packets waiting (a
 * thread above it on its core); the DAC's thread's passes late (over 300 ms: certain gaps) 3 seconds in 10 on a core
 * asked for (--cpu). Two steps (the stream stopped, its player told ANET_WHY_BACKOFF, the DAC given back, every stream
 * refused 60 s, then 2, 4… 15 min; the threads stay out of real time for the process's life) when: either thread above
 * 80 % of a core 5 s without real time (a loop of ours), or the DAC's thread stuck 5 s not runnable (in the kernel: the
 * USB, the DAC). The control thread not done within 5 s: the process ends (75), the kernel gives the DAC back. Each
 * back-off one line in the journal, and stamped (stamp_write): this program started again within 6 h starts without
 * real time. Packets late (≥ 200 ms) while our threads keep up: said (at most once a minute), nothing done: the
 * player's machine or the network, not this one. */
static _Atomic int bk_level, bk_req;      /* the steps taken; 2: the stream to stop (the control thread) */
static char bk_why[200];
static double bk_t_req, bk_until, bk_cool = 60;
/* the process ends (the DAC's thread unreachable): the player told why first if the lock comes within 0.5 s */
static void backoff_exit(const char *what) {
    lg("backing off: %s: the process ends (the kernel gives the DAC back)", what);
    struct timespec ab; clock_gettime(CLOCK_REALTIME, &ab); ab.tv_nsec += 500000000L; if (ab.tv_nsec >= 1000000000L) { ab.tv_sec++; ab.tv_nsec -= 1000000000L; }
#ifdef __linux__
    if (!pthread_mutex_timedlock(&lk, &ab)) {
        S.why = ANET_WHY_BACKOFF;
        if (S.cfd >= 0 && (S.caps & ANET_CAP_STATE)) report_locked_f(ANET_R_STOPPED);
        pthread_mutex_unlock(&lk);
    }
#endif
    _exit(75);
}
static void backoff(int level, const char *fmt, ...) {
    char w[200];
    va_list a; va_start(a, fmt); vsnprintf(w, sizeof w, fmt, a); va_end(a);
    if (atomic_load(&bk_level) < 1) {
        atomic_store(&bk_level, 1);
        atomic_store(&rt_now, 0);
        P_prio = 0;
#ifdef __linux__
        P_rt = P_house;
        char th[64]; cpus_text(&P_house, th, sizeof th);
#else
        const char *th = "any";
#endif
        thr_apply(th_a, atomic_load(&a_tid), 0, P_nice);
        thr_apply(th_u, atomic_load(&u_tid), 0, P_nice);
        lg("backing off: %s: the real time dropped (SCHED_OTHER, nice %d, cores %s)%s", w, P_nice, th, level < 2 ? "; the stream goes on" : "");
        stamp_write(1, w);
    }
    if (level < 2 || atomic_load(&bk_req)) return;
    snprintf(bk_why, sizeof bk_why, "%s", w);
    bk_t_req = mono();
    atomic_store(&bk_level, 2);
    stamp_write(2, w);
    lg("backing off: %s: the stream to stop, the DAC to give back", w);
    atomic_store(&bk_req, 2);
    if (quit_pipe[1] >= 0) { const char c = 2; (void)!write(quit_pipe[1], &c, 1); }
}
#ifdef __linux__
static double clk(clockid_t c) { struct timespec t; clock_gettime(c, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void *guard_main(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "az-guard");
    clockid_t ca, cu;
    const int have_clk = !pthread_getcpuclockid(th_a, &ca) && !pthread_getcpuclockid(th_u, &cu);
    double t_prev = mono(), t_win = t_prev, ca0 = have_clk ? clk(ca) : 0, cu0 = have_clk ? clk(cu) : 0, t_late_said = -1e9;
    unsigned long ab0 = atomic_load(&a_beat), ub0 = atomic_load(&u_beat);
    double t_ab = t_prev, t_ub = t_prev, u_stall = 0;
    int hot = 0, hot2 = 0; unsigned lates = 0;   /* lates: the last 10 seconds, a bit each (the DAC's thread late) */
    for (;;) {
        const struct timespec d = { 0, 250000000 };
        nanosleep(&d, NULL);
        const double t = mono(), late = t - t_prev - 0.25;
        t_prev = t;
        if (atomic_exchange(&xcpu_hit, 0)) backoff(1, "a real-time thread of ours ran 200 ms without blocking (SIGXCPU)");
        if (late > 1.5 && atomic_load(&rt_now)) backoff(1, "the guard woke %.1f s late: the cores starved", late);
        const unsigned long ab = atomic_load(&a_beat), ub = atomic_load(&u_beat);
        if (ab != ab0 || atomic_load(&a_parked)) { ab0 = ab; t_ab = t; }
        else {
            const double still = t - t_ab;
            const char st = task_state(atomic_load(&a_tid));
            if (still > 2 && st == 'R') { if (atomic_load(&bk_level) < 1) backoff(1, "the DAC's thread runnable and not run for %.1f s (a thread above it on its core)", still); }
            else if (still > 5 && st != 'R' && !atomic_load(&bk_req)) backoff(2, "the DAC's thread stuck %.0f s (state %c: in the kernel, the USB or the DAC)", still, st);
        }
        int q = 0;
        if (ub != ub0 || ioctl(ufd, FIONREAD, &q) || q <= 0) { ub0 = ub; t_ub = t; }
        else {
            const double still = t - t_ub;
            if (still > u_stall) u_stall = still;
            if (still > 2 && task_state(atomic_load(&u_tid)) == 'R' && atomic_load(&bk_level) < 1) backoff(1, "the packets' thread runnable and not run for %.1f s, packets waiting", still);
        }
        if (atomic_load(&bk_req) && t - bk_t_req > 5) backoff_exit("the control thread did not stop the stream within 5 s");
        if (t - t_win < 1.0) continue;
        /* a second: the shares, the DAC's thread's longest pass, the packets' lateness */
        const double dt = t - t_win;
        double sa = 0, su = 0;
        if (have_clk) { const double ca1 = clk(ca), cu1 = clk(cu); sa = (ca1 - ca0) / dt; su = (cu1 - cu0) / dt; ca0 = ca1; cu0 = cu1; }
        t_win = t;
        const double sh = sa > su ? sa : su;
        const char *which = sa >= su ? "DAC's" : "packets'";
        if (atomic_load(&rt_now)) {
            hot = sh > 0.25 ? hot + 1 : 0;
            if (hot >= 2) { backoff(1, "the %s thread used %.0f %% of a core for 2 s at real time", which, sh * 100); hot = 0; }
        } else {
            hot2 = sh > 0.80 ? hot2 + 1 : 0;
            if (hot2 >= 5 && !atomic_load(&bk_req)) { backoff(2, "the %s thread used %.0f %% of a core for 5 s without real time (a loop of ours)", which, sh * 100); hot2 = 0; }
        }
        const uint32_t gap = atomic_exchange(&a_gap_us, 0);
        lates = ((lates << 1) | (!atomic_load(&a_parked) && gap > 300000)) & 0x3ff;
        if (o_cpu >= 0 && __builtin_popcount(lates) >= 3 && atomic_load(&bk_level) < 1) { backoff(1, "the DAC's thread late 3 times in 10 s (up to %.0f ms: its core busy)", gap / 1000.0); lates = 0; }
        const uint32_t jl = atomic_load(&j_late_us);
        if (jl >= 200000 && u_stall < 0.1 && gap < 100000 && t - t_late_said > 60) {
            t_late_said = t;
            lg("packets up to %.0f ms late, this receiver's threads on time (longest pass %.0f ms): the player's machine or the network", jl / 1000.0, gap / 1000.0);
        }
        u_stall = 0;
    }
    return NULL;
}
#endif

static void on_quit(int s) { quit_sig = s; if (quit_pipe[1] >= 0) { const char c = 1; (void)!write(quit_pipe[1], &c, 1); } }

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--null")) o_null = 1;
        else if (!strcmp(a, "--no-mdns")) o_mdns = 0;
        else if (!strcmp(a, "--once")) o_once = 1;
        else if (!strcmp(a, "--stats")) o_stats = 1;
        else if (!strcmp(a, "--no-adapt")) o_adapt = 0;
        else if (!strcmp(a, "--v2")) o_v2 = 1;
        else if (!strcmp(a, "--dac")) { o_dac = v; i++; }
        else if (!strcmp(a, "--port")) { o_port = atoi(v); i++; }
        else if (!strcmp(a, "--name")) { snprintf(o_name, sizeof o_name, "%s", v); i++; }
        else if (!strcmp(a, "--id")) { snprintf(o_id, sizeof o_id, "%s", v); i++; }
        else if (!strcmp(a, "--max-rate")) { o_max = (unsigned)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--period-us")) { o_period_us = atoi(v); i++; }
        else if (!strcmp(a, "--periods")) { o_periods = atoi(v); i++; }
        else if (!strcmp(a, "--ring-ms")) { o_ring_ms = atoi(v); i++; }
        else if (!strcmp(a, "--ring-max-ms")) { o_ring_max_ms = atoi(v); i++; }
        else if (!strcmp(a, "--jitter-q")) { o_q = atof(v); i++; }
        else if (!strcmp(a, "--jitter-memory-s")) { o_mem_s = atoi(v); i++; }
        else if (!strcmp(a, "--rt")) { o_rt = !strcmp(v, "auto") ? -1 : atoi(v); i++; }
        else if (!strcmp(a, "--cpu")) { o_cpu = !strcmp(v, "auto") ? -1 : atoi(v); i++; }
        else if (!strcmp(a, "--force")) o_force = 1;
        else if (!strcmp(a, "--lock-mb")) { o_lock_mb = atoi(v); i++; }
        else if (!strcmp(a, "--null-ppm")) { o_ppm = atof(v); i++; }
        else if (!strcmp(a, "--dump")) { o_dump = v; i++; }
        else { fprintf(stderr, "usage: audiolaz-recv [--dac hw:…|auto] [--null] [--port N] [--name S] [--id S] [--max-rate Hz] [--period-us N] [--periods N] [--ring-ms N] [--ring-max-ms N] [--no-adapt] [--jitter-q Q] [--jitter-memory-s S] [--no-mdns] [--rt auto|N] [--cpu auto|N] [--force] [--lock-mb N] [--null-ppm N] [--dump F] [--stats] [--once] [--v2]\n"); return 2; }
    }
    if (o_period_us < 500) o_period_us = 500;
    if (o_periods < 2) o_periods = 2;
    if (o_ring_ms < 10) o_ring_ms = 10;
    if (o_ring_max_ms < o_ring_ms) o_ring_max_ms = o_ring_ms;
    if (o_ring_max_ms > 2000) o_ring_max_ms = 2000;
    if (o_q < 0.5 || o_q > 1.0) o_q = 0.999;
    if (o_mem_s < 1) o_mem_s = 1;
    if (o_mem_s > JM) o_mem_s = JM;
    if (o_rt > 99) o_rt = 99;
    if (o_lock_mb < 0) o_lock_mb = 0;
    {   /* the guard's test: the DAC's thread spins <s> after a stream starts (":park": until asked to park) */
        const char *ts = getenv("AUDIOLAZ_RECV_TEST_SPIN");
        if (ts && *ts) { test_spin_s = atof(ts); test_spin_park = strstr(ts, ":park") != NULL; }
        const char *tx = getenv("AUDIOLAZ_RECV_TEST_XRUN");
        if (tx && *tx) test_xrun_s = atof(tx);
    }
    signal(SIGPIPE, SIG_IGN);
    if (pipe(quit_pipe) == 0) { signal(SIGTERM, on_quit); signal(SIGINT, on_quit); }
    if (!o_id[0]) { gethostname(o_id, sizeof o_id); o_id[sizeof o_id - 1] = 0; char *d = strchr(o_id, '.'); if (d) *d = 0; }
#ifdef ANET_NULL_ONLY
    o_null = 1;
#endif
    if (o_null) { snprintf(dac_dev, sizeof dac_dev, "null"); if (!o_name[0]) snprintf(o_name, sizeof o_name, "null (test)"); if (!o_max) o_max = ANET_MAX_RATE; }
#ifndef ANET_NULL_ONLY
    else {

        char line[256], id[64] = "", nm[96] = "";
        FILE *f = fopen("/proc/asound/cards", "r");
        while (f && fgets(line, sizeof line, f)) {
            char *lb = strchr(line, '['), *rb = lb ? strchr(lb, ']') : NULL, *dash = rb ? strstr(rb, " - ") : NULL;
            if (!lb || !rb || !dash) continue;
            char cid[64]; snprintf(cid, sizeof cid, "%.*s", (int)(rb - lb - 1), lb + 1);
            for (char *e = cid + strlen(cid); e > cid && e[-1] == ' '; ) *--e = 0;
            const int want = strcmp(o_dac, "auto") ? strstr(o_dac, cid) != NULL : strstr(rb, "USB-Audio") != NULL;
            if (want && !id[0]) { snprintf(id, sizeof id, "%s", cid); snprintf(nm, sizeof nm, "%.95s", dash + 3); nm[strcspn(nm, "\r\n")] = 0; }
        }
        if (f) fclose(f);
        if (!strcmp(o_dac, "auto")) {
            if (!id[0]) { lg("no USB DAC found (/proc/asound/cards): --dac hw:CARD=<id>,DEV=0"); return 1; }
            snprintf(dac_dev, sizeof dac_dev, "hw:CARD=%s,DEV=0", id);
        } else snprintf(dac_dev, sizeof dac_dev, "%s", o_dac);
        if (!strncmp(dac_dev, "plug", 4) || !strncmp(dac_dev, "default", 7)) { lg("%s: a plug layer would resample: hw: only", dac_dev); return 1; }
        if (!o_name[0]) snprintf(o_name, sizeof o_name, "%.95s", nm[0] ? nm : id[0] ? id : dac_dev);
        {

            char cid[64];
            alsa_dev_card(dac_dev, cid, sizeof cid);
            alsa_card_hw(alsa_card_index(cid), o_hw, sizeof o_hw);
            if (o_hw[0]) lg("DAC %s: %s", cid, o_hw);
        }
        if (!o_max) {   /* the highest PCM rate the DAC takes as it is; busy now (another program on it): 384 kHz said */
            static const unsigned R[] = { 1536000, 768000, 705600, 384000, 352800, 192000, 176400, 96000, 88200, 48000, 44100 };
            for (size_t i = 0; i < sizeof R / sizeof R[0] && !o_max; i++) { const int t = out_takes(dac_dev, OUTF_S32, R[i]); if (t < 0) { o_max = 384000; lg("%s busy now: up to 384000 Hz said", dac_dev); } else if (t == 1) o_max = R[i]; }
            if (!o_max) o_max = 192000;
        }
    }
#endif
    /* the ring: half a second at the highest rate at least, and the ceiling's ring with room to spare */
    uint64_t cap = 1;
    const uint64_t need = (uint64_t)o_max / 2 > (uint64_t)o_max * (uint64_t)(o_ring_max_ms + 250) / 1000 ? (uint64_t)o_max / 2 : (uint64_t)o_max * (uint64_t)(o_ring_max_ms + 250) / 1000;
    while (cap < need) cap <<= 1;
    ring = calloc((size_t)cap, 8);
    ring_mask = cap - 1;
    if (!ring) { lg("no memory for the ring"); return 1; }
    place_init();   /* the cores, the level, our limit (above) */
    {   /* memory: the ring locked only if pages can go out (swap), within --lock-mb; never mlockall */
        const size_t rb = (size_t)cap * 8;
        if (!P_swap) lg("memory: no swap here, nothing locked (nothing can be paged out)");
        else if (rb > (size_t)o_lock_mb << 20) lg("memory: the ring (%zu MB) above --lock-mb %d: not locked", rb >> 20, o_lock_mb);
        else { memset(ring, 0, rb); if (mlock(ring, rb)) lg("memory: the ring not locked (%s)", strerror(errno)); else lg("memory: the ring locked (%.1f MB), the threads' stacks (256 KB each)", rb / 1048576.0); }
    }
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in la = { .sin_family = AF_INET, .sin_port = htons((uint16_t)o_port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(ls, (struct sockaddr *)&la, sizeof la) || listen(ls, 4)) { lg("port %d: %s", o_port, strerror(errno)); return 1; }
    ufd = socket(AF_INET, SOCK_DGRAM, 0);
    {   /* 300 ms of music at the highest rate, taken as root whatever net.core.rmem_max says (SO_RCVBUFFORCE);
         * the kernel doubles what it is asked (each packet's own cost, about as much as its music) */
        const unsigned top = o_max ? o_max : ANET_MAX_RATE;
        double want = top * 8.0 * 0.3;
        if (want < 1 << 20) want = 1 << 20;
        if (want > 32 << 20) want = 32 << 20;
        int rb = (int)want, got = 0; socklen_t gl = sizeof got;
#ifdef SO_RCVBUFFORCE
        if (setsockopt(ufd, SOL_SOCKET, SO_RCVBUFFORCE, &rb, sizeof rb))
#endif
            setsockopt(ufd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
        if (!getsockopt(ufd, SOL_SOCKET, SO_RCVBUF, &got, &gl)) {
            const double ms = got / (top * 8.0 * 2) * 1000.0;
            lg("network buffer: %.1f MB, about %.0f ms of packets at %u Hz%s", got / 1048576.0, ms, top,
               ms < 150 ? " (short: packets dropped if this machine stalls longer; root, or net.core.rmem_max raised)" : "");
        }
    }
    if (bind(ufd, (struct sockaddr *)&la, sizeof la)) { lg("UDP port %d: %s", o_port, strerror(errno)); return 1; }
    anet_mdns_set_hw(o_hw);
    if (o_mdns && anet_mdns_publish(o_id, o_name, o_port, o_max)) lg("mDNS not published (port 5353 refused): the player may name it audiolaz:<address>:%d", o_port);
    lg("ready: « audiolaz Link → %s » (%s), port %d, up to %u Hz, ring %d ms%s, periods %d × %d µs, %s %d%s", o_name, dac_dev, o_port, o_max, o_ring_ms,
       o_v2 ? " (version 2 only)" : o_adapt ? "" : " (fixed)", o_periods, o_period_us, P_prio ? "real time" : "no real time, nice", P_prio ? P_prio : P_nice, o_mdns ? "" : ", no mDNS");
    if (!o_v2 && o_adapt) lg("protocol 3: the ring from %d ms up to %d ms as the network needs it (jitter quantile %.4g, peaks remembered %d s)", o_ring_ms, o_ring_max_ms, o_q, o_mem_s);
    S.ended = 1;
    {   /* small stacks (256 KB: what is locked, with swap, stays small) */
        pthread_attr_t ta; pthread_attr_init(&ta); pthread_attr_setstacksize(&ta, 256 << 10);
        if (pthread_create(&th_a, &ta, audio_main, NULL) || pthread_create(&th_u, &ta, udp_main, NULL)) { lg("threads not started"); return 1; }
#ifdef __linux__
        pthread_t tg;
        for (int i = 0; i < 100 && (!atomic_load(&a_tid) || !atomic_load(&u_tid)); i++) usleep(10000);   /* their ids, for the guard */
        if (pthread_create(&tg, &ta, guard_main, NULL)) lg("the guard not started");
#endif
        pthread_attr_destroy(&ta);
    }

    /* the control thread (this one): connections and their messages; never in the music's path */
    int fd = -1; unsigned id = 0;
    static uint8_t pay[ANET_META_MAX + 1];
    for (;;) {
        struct pollfd p[3] = { { ls, POLLIN, 0 }, { quit_pipe[0], POLLIN, 0 }, { fd, POLLIN, 0 } };
        poll(p, fd >= 0 ? 3 : 2, 1000);
        if (!quit_sig && (p[1].revents & POLLIN)) { char c[16]; (void)!read(quit_pipe[0], c, sizeof c); }   /* the guard's call */
        if (atomic_load(&bk_req) == 2) {                  /* the guard: the stream stopped, the DAC given back, a while refused */
            int ok;
            if (fd >= 0) { ok = !stop_here_t(ANET_WHY_BACKOFF, id, 3.0); if (ok) { stats_print(); shutdown(fd, SHUT_WR); close(fd); fd = -1; } }
            else if ((ok = !park_timed(3.0))) { dev_close(); unpark(); }
            if (!ok) backoff_exit("the DAC's thread does not answer");
            bk_until = mono() + bk_cool;
            lg("backing off (%s): every stream refused %.0f s, then without real time", bk_why, bk_cool);
            bk_cool = bk_cool * 2 > 900 ? 900 : bk_cool * 2;
            atomic_store(&bk_req, 0);
            continue;
        }
        if (quit_sig) {                                   /* SIGTERM / SIGINT: the stream told, the DAC given back */
            if (fd >= 0) { stop_here(ANET_WHY_QUIT, id); stats_print(); shutdown(fd, SHUT_WR); close(fd); }
            else { park(); dev_close(); unpark(); }
            lg("signal %d: the DAC given back, done", (int)quit_sig);
            if (D.dump) fclose(D.dump);
            return 0;
        }
        /* protocol 3: the player silent (no ping, no packet) ANET_SILENT_MS while not paused: the stream ends here */
        if (fd >= 0) {
            pthread_mutex_lock(&lk);
            const int watch = S.live && (S.caps & ANET_CAP_STATE) && !S.paused;
            double quiet = mono() - S.t_ping;
            const double qp = mono() - atomic_load(&t_last_pkt);
            if (qp < quiet) quiet = qp;
            pthread_mutex_unlock(&lk);
            if (watch && quiet > ANET_SILENT_MS / 1000.0) {
                stop_here(ANET_WHY_LOST, id); stats_print();
                shutdown(fd, SHUT_WR); close(fd); fd = -1;
                continue;
            }
        }
        if (p[0].revents & POLLIN) {
            const int c = accept(ls, NULL, NULL);
            if (c >= 0) {
                setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                /* read whole first (an INFO request answered, a stream's header and who sends it), the stream
                 * playing untouched; then one DAC, one player: the newcomer refused, or it plays and the stream there ends */
                conn_t k;
                unsigned why = ANET_WHY_TAKEN;
                const int r = conn_read(c, &k);
                if (r) close(c);
                else if (k.e == ANET_E_HEADER) { refuse(c, k.h.id, ANET_E_HEADER); lg("stream refused (header)"); close(c); }
                else if (mono() < bk_until) {
                    static double t_said = -1e9;
                    refuse(c, k.h.id, ANET_E_DAC);
                    if (mono() - t_said > 60) { t_said = mono(); lg("stream %u refused: backing off (%s), %.0f s more", k.h.id, bk_why, bk_until - mono()); }
                    close(c);
                }
                else if (busy_for(&k, fd, &why)) { refuse_busy(c, &k); close(c); }
                else if (k.e) { refuse(c, k.h.id, (uint32_t)k.e); lg("stream refused (rate, %u Hz)", k.h.rate); close(c); }
                else {
                    if (fd >= 0) {
                        if (why == ANET_WHY_TAKEN && k.take) { char by[128]; hold_name(by, sizeof by); lg("stream %u: « %s » takes the DAC over from « %s » (its listener asked)", k.h.id, k.name, by); }
                        else if (why == ANET_WHY_LOST) lg("stream %u: the player there silent %.0f s: the DAC free", k.h.id, mono() - hold_alive(fd));
                        stop_here(why, id); shutdown(fd, SHUT_WR); close(fd); fd = -1; stats_print();
                    }
                    if (stream_begin(c, &k)) close(c);
                    else { fd = c; hold_set(&k); pthread_mutex_lock(&lk); id = S.id; pthread_mutex_unlock(&lk); }
                }
            }
        }
        if (fd < 0 || !(p[2].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        uint8_t mh[8];
        if (!recv_all(fd, mh, 8, 2000)) {
            const uint32_t t = anet_g32(mh), len = anet_g32(mh + 4);
            HOLD.t_alive = mono();                         /* its player there */
            /* a payload (protocol 3 only; read whole, a too long one skipped) */
            uint32_t got = 0;
            if (len) {
                int bad = 0;
                static uint8_t skip[ANET_META_MAX];
                for (uint32_t left = len; left && !bad;) {
                    const uint32_t k = left > ANET_META_MAX ? ANET_META_MAX : left;
                    if (recv_all(fd, got ? skip : pay, k, 2000)) bad = 1;
                    else { if (!got) got = k; left -= k; }
                }
                if (bad) goto gone;
                pay[got] = 0;
            }
            const unsigned why = len >= 4 && (t == ANET_M_STOP || t == ANET_M_END) ? anet_g32(pay) : ANET_WHY_NONE;
            if (t == ANET_M_PING) {                       /* answered at once: alive, whatever the DAC's thread does */
                pthread_mutex_lock(&lk);
                S.t_ping = mono(); if (len >= 4) S.seq = anet_g32(pay);
                if (len >= 8) atomic_store(&j_live, !!(anet_g32(pay + 4) & ANET_SS_LIVE));
                report_locked();
                pthread_mutex_unlock(&lk);
                continue;
            }
            if (t == ANET_M_META) {
                char ti[160], ar[160];
                anet_meta_get((char *)pay, "title", ti, sizeof ti); anet_meta_get((char *)pay, "artist", ar, sizeof ar);
                lg("stream %u: now playing « %s »%s%s", id, ti, ar[0] ? " by " : "", ar);
                continue;
            }
            park();
            if (t == ANET_M_END) { S.ended = 1; if (why) S.why = why; }
            else if (t == ANET_M_STOP) { S.why = why ? why : ANET_WHY_USER; S.live = 0; dev_close(); S.ended = 1; }
            else if (t == ANET_M_PAUSE) S.paused = 1;
            else if (t == ANET_M_RESUME) { S.paused = 0; S.t_ping = mono(); atomic_store(&t_last_pkt, mono()); }
            unpark();
            if (t == ANET_M_STOP) { lg("stream %u stopped by the player: %s", id, why ? anet_why_text(why) : "no reason given (a version-2 player)"); stats_print(); }
            if (t == ANET_M_END) lg("stream %u: its end (%s), played out", id, why ? anet_why_text(why) : "the music's end");
            continue;
        }
    gone:
        HOLD.t_alive = mono();                             /* its player's last sign: its connection gone */
        park();
        const int clean = S.ended;
        S.cfd = -1;
        if (!clean) { S.live = 0; dev_close(); S.ended = 1; S.why = ANET_WHY_CLOSED; }
        unpark();
        if (clean) shutdown(fd, SHUT_WR);
        close(fd); fd = -1;
        if (!clean) lg("stream %u: the connection went without its end: stopped (%s)", id, anet_why_text(ANET_WHY_CLOSED));
        for (int i = 0; i < 300; i++) { pthread_mutex_lock(&lk); const int done = !D.open; pthread_mutex_unlock(&lk); if (done) break; usleep(10000); }
        stats_print();
        if (o_once) {   /* the tests: quit once played out */
            if (D.dump) fclose(D.dump);
            pthread_mutex_lock(&lk);
            lg("done: %llu frames, %lu underruns (in %lu gaps), %lu frames lost", (unsigned long long)(S.tw - S.zeros), S.underruns, S.gaps, atomic_load(&lost_frames));
            pthread_mutex_unlock(&lk);
            return 0;
        }
    }
}
