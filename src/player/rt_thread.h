/* src/player/rt_thread.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_RT_THREAD_H
#define AUDIOLAZ_RT_THREAD_H
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread/qos.h>
#endif

/* period_s: the thread's rhythm (a packet's time); busy: the share of it its work may take (0 < busy ≤ 1); prio: the
 * SCHED_FIFO level on Linux (0: left as it is). macOS: the computation at least 50 µs (the scheduler's quantum below
 * that is refused), the constraint the whole period, preemptible (a burst longer than the computation goes on later). */
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
    struct sched_param sp = { .sched_priority = prio };
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
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
