/* src/player/rt_thread.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_RT_THREAD_H
#define AUDIOLAZ_RT_THREAD_H
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread/qos.h>
#endif

#define RT_LIMIT_US 200000
#define RT_OFF_S 60.0
#define RT_TIDS 32
#ifdef __linux__
__attribute__((weak)) _Atomic int rt_tids[RT_TIDS];          /* the real-time threads of ours (kernel ids) */
__attribute__((weak)) _Atomic int rt_xcpu_n, rt_xcpu_tid, rt_drop_any;
__attribute__((weak)) _Atomic long long rt_off_until_ns;     /* no real time before this (CLOCK_MONOTONIC, ns) */
static inline long long rt_mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000000000LL + t.tv_nsec; }
static inline int rt_gettid(void) { return (int)syscall(SYS_gettid); }
/* the calling thread is real time from now (kept until it ends: a slot freed by rt_unregister) */
static inline void rt_register(void) {
    const int me = rt_gettid();
    for (int i = 0; i < RT_TIDS; i++) if (atomic_load(&rt_tids[i]) == me) return;
    for (int i = 0; i < RT_TIDS; i++) { int z = 0; if (atomic_compare_exchange_strong(&rt_tids[i], &z, me)) return; }
    for (int i = 0; i < RT_TIDS; i++) {                /* full: a thread that ended without its rt_unregister, its slot */
        char p[48]; int x = atomic_load(&rt_tids[i]);
        snprintf(p, sizeof p, "/proc/self/task/%d", x);
        if (access(p, F_OK) && atomic_compare_exchange_strong(&rt_tids[i], &x, me)) return;
    }
}
static inline void rt_unregister(void) {
    const int me = rt_gettid();
    for (int i = 0; i < RT_TIDS; i++) { int x = me; if (atomic_compare_exchange_strong(&rt_tids[i], &x, 0)) return; }
}
/* real time allowed now (not within RT_OFF_S of a SIGXCPU) */
static inline int rt_allowed(void) { return rt_mono_ns() >= atomic_load(&rt_off_until_ns); }
__attribute__((unused)) static void rt_on_xcpu(int s) {
    (void)s;
    const int me = rt_gettid();
    int ours = atomic_load(&rt_drop_any);
    for (int i = 0; i < RT_TIDS && !ours; i++) if (atomic_load(&rt_tids[i]) == me) ours = 1;
    if (!ours) return;                                 /* another code's thread (a library's): left as it is */
    const struct sched_param p = { .sched_priority = 0 };
    sched_setscheduler(me, SCHED_OTHER, &p);
    atomic_store(&rt_off_until_ns, rt_mono_ns() + (long long)(RT_OFF_S * 1e9));
    atomic_store(&rt_xcpu_tid, me);
    atomic_fetch_add(&rt_xcpu_n, 1);
}
/* at the process's start: 0, or errno (said by the caller). any: every real-time thread here is ours. */
static inline int rt_limit_install(int any) {
    atomic_store(&rt_drop_any, any);
    struct rlimit rl;
    if (getrlimit(RLIMIT_RTTIME, &rl)) return errno;
    rlim_t soft = RT_LIMIT_US;
    if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < soft) soft = rl.rlim_max;
    rl.rlim_cur = soft;
    if (setrlimit(RLIMIT_RTTIME, &rl)) return errno;
    struct sigaction sa;
    sa.sa_handler = rt_on_xcpu; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART;
    return sigaction(SIGXCPU, &sa, NULL) ? errno : 0;
}
/* an ordinary thread's look (the main loop's): a thread dropped since the last look, its name in `name` (comm);
 * returns how many in all (0: none new) */
static inline int rt_xcpu_said(int *seen, char *name, size_t nl) {
    const int n = atomic_load(&rt_xcpu_n);
    if (n == *seen) return 0;
    *seen = n;
    if (name && nl) {
        name[0] = 0;
        char p[64]; snprintf(p, sizeof p, "/proc/self/task/%d/comm", atomic_load(&rt_xcpu_tid));
        FILE *f = fopen(p, "r");
        if (f) { if (fgets(name, (int)nl, f)) name[strcspn(name, "\n")] = 0; fclose(f); }
        if (!name[0]) snprintf(name, nl, "thread %d", atomic_load(&rt_xcpu_tid));
    }
    return n;
}
#else
static inline void rt_register(void) { }
static inline void rt_unregister(void) { }
static inline int rt_allowed(void) { return 1; }
static inline int rt_limit_install(int any) { (void)any; return 0; }
static inline int rt_xcpu_said(int *seen, char *name, size_t nl) { (void)seen; (void)name; (void)nl; return 0; }
#endif
static inline double rt_clk_(clockid_t c) { struct timespec t; clock_gettime(c, &t); return t.tv_sec + t.tv_nsec / 1e9; }
/* the tests' own (test/rt-safety.sh): AUDIOLAZ_TEST_RT_SPIN=<which>:<after s>[:<for s>] — the thread named
 * <which> (writer, play, tx) spins that long without a wait, once, <after s> into its play: never in a real play */
static inline double rt_test_spin(const char *which, double *for_s) {
    const char *e = getenv("AUDIOLAZ_TEST_RT_SPIN");
    const size_t n = strlen(which);
    if (!e || strncmp(e, which, n) || e[n] != ':') return 0;
    char *x;
    const double after = strtod(e + n + 1, &x);
    *for_s = *x == ':' ? atof(x + 1) : 2.0;
    return after > 0 ? after : 0;
}
static inline void rt_test_spin_now(double for_s) {   /* no system call: what a loop of ours gone mad does */
    const double t0 = rt_clk_(CLOCK_MONOTONIC);
    volatile unsigned long k = 0;
    while (rt_clk_(CLOCK_MONOTONIC) - t0 < for_s) k++;
}
typedef struct { double cpu0, wall0; long vcs0; unsigned long naps; } rt_guard;
/* each turn of a real-time loop: budget_s of CPU since it last waited, then a nap of nap_s */
static inline void rt_guard_step(rt_guard *g, double budget_s, double nap_s) {
    const double c = rt_clk_(CLOCK_THREAD_CPUTIME_ID);
#if defined(__linux__) && defined(RUSAGE_THREAD)
    struct rusage ru;
    const long vcs = getrusage(RUSAGE_THREAD, &ru) ? -1 : ru.ru_nvcsw;
    const int waited = !g->wall0 || vcs < 0 || vcs != g->vcs0;
#else
    const double w = rt_clk_(CLOCK_MONOTONIC);
    const long vcs = 0;
    const int waited = !g->wall0 || (w - g->wall0) - (c - g->cpu0) >= nap_s;   /* off the CPU that long since */
#endif
    if (!waited && c - g->cpu0 < budget_s) return;
    if (!waited) {
        const struct timespec z = { 0, (long)(nap_s * 1e9) };
        nanosleep(&z, NULL);
        g->naps++;
    }
    g->cpu0 = rt_clk_(CLOCK_THREAD_CPUTIME_ID); g->wall0 = rt_clk_(CLOCK_MONOTONIC);
#if defined(__linux__) && defined(RUSAGE_THREAD)
    struct rusage r2;
    g->vcs0 = getrusage(RUSAGE_THREAD, &r2) ? -1 : r2.ru_nvcsw;
#else
    g->vcs0 = vcs;
#endif
}

/* period_s: the thread's rhythm (a packet's time); busy: the share of it its work may take (0 < busy ≤ 1); prio: the
 * SCHED_FIFO level on Linux (0: left as it is). macOS: the computation at least 50 µs (the scheduler's quantum below
 * that is refused), the constraint the whole period, preemptible (a burst longer than the computation goes on later).
 * Linux: registered (rt_register: its SIGXCPU drops it), refused within RT_OFF_S of a drop (EAGAIN). */
static inline int rt_clock(double period_s, double busy, int prio) {
#ifdef __APPLE__
    (void)prio;
    mach_timebase_info_data_t tb;
    if (mach_timebase_info(&tb) != KERN_SUCCESS || !tb.numer) return EINVAL;
    const double per_ns = period_s * 1e9, to = (double)tb.denom / tb.numer;   /* ns → Mach ticks */
    double comp_ns = per_ns * (busy > 0 && busy <= 1 ? busy : 0.25);
    if (comp_ns < 50e3) comp_ns = 50e3;
    if (comp_ns > per_ns) comp_ns = per_ns;
    thread_time_constraint_policy_data_t p = { (uint32_t)(per_ns * to), (uint32_t)(comp_ns * to), (uint32_t)(per_ns * to), 1 };
    return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p,
                             THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS ? 0 : EPERM;
#else
    (void)period_s; (void)busy;
    if (prio <= 0) return 0;
    if (!rt_allowed()) return EAGAIN;
    struct sched_param sp = { .sched_priority = prio };
    const int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (!e) rt_register();
    return e;
#endif
}

/* back from rt_clock: macOS, the standard (timeshare) policy, the thread's QoS kept; Linux, SCHED_OTHER */
static inline int rt_standard(void) {
#ifdef __APPLE__
    thread_standard_policy_data_t p = { 0 };
    return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_STANDARD_POLICY, (thread_policy_t)&p,
                             THREAD_STANDARD_POLICY_COUNT) == KERN_SUCCESS ? 0 : EPERM;
#else
    struct sched_param sp = { .sched_priority = 0 };
    rt_unregister();
    return pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
#endif
}
static inline int rt_interactive(void) {
#ifdef __APPLE__
    return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#else
    return 0;
#endif
}

/* before the lock is first used (a constructor, or the start); 0 or why not (the lock then left as it was) */
static inline int rt_mutex_pi(pthread_mutex_t *m) {
#if defined(_POSIX_THREAD_PRIO_INHERIT) && _POSIX_THREAD_PRIO_INHERIT > 0 && !defined(__APPLE__)
    pthread_mutexattr_t a;
    if (pthread_mutexattr_init(&a)) return EINVAL;
    int e = pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    if (!e) { pthread_mutex_destroy(m); e = pthread_mutex_init(m, &a); if (e) pthread_mutex_init(m, NULL); }
    pthread_mutexattr_destroy(&a);
    return e;
#else
    (void)m;
    return 0;
#endif
}
#endif
