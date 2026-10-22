/* src/player/rt_watch.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_RT_WATCH_H
#define AUDIOLAZ_RT_WATCH_H
#ifdef __linux__
#include <dirent.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define RTW_MAX 96
#define RTW_EVERY_S 10.0
typedef struct { int pid, tid, prio, pol; char comm[24]; unsigned long long ns; } rtw_thread;
typedef struct { rtw_thread t[RTW_MAX]; int n; double at; } rtw_snap;
static inline double rtw_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
/* a task's comm (its name, maybe with spaces), its policy (field 41) and priority (40); 0 or −1 */
static inline int rtw_stat(const char *path, char *comm, size_t cl, int *prio, int *pol) {
    char b[1024];
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    const size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    char *l = strchr(b, '('), *r = strrchr(b, ')');
    if (!l || !r || r < l) return -1;
    snprintf(comm, cl, "%.*s", (int)(r - l - 1), l + 1);
    int field = 2;
    for (char *p = r + 1; *p; p++) {
        if (*p != ' ') continue;
        field++;
        if (field == 40) *prio = atoi(p + 1);
        else if (field == 41) { *pol = atoi(p + 1); return 0; }
    }
    return -1;
}
static inline unsigned long long rtw_ns(int pid, int tid) {
    char p[96]; unsigned long long ns = 0;
    snprintf(p, sizeof p, "/proc/%d/task/%d/schedstat", pid, tid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char b[64];                                /* (no fscanf: glibc 2.38 versions it, the receivers keep to 2.34) */
    if (fgets(b, sizeof b, f)) ns = strtoull(b, NULL, 10);
    fclose(f);
    return ns;
}
/* every real-time thread of the machine (kernel threads too: an IRQ thread may hold a core as well) */
static inline void rtw_take(rtw_snap *s) {
    s->n = 0; s->at = rtw_now();
    DIR *d = opendir("/proc");
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)) && s->n < RTW_MAX;) {
        const int pid = atoi(e->d_name);
        if (pid <= 0) continue;
        char p[64];
        snprintf(p, sizeof p, "/proc/%d/task", pid);
        DIR *td = opendir(p);
        if (!td) continue;
        for (struct dirent *t; (t = readdir(td)) && s->n < RTW_MAX;) {
            const int tid = atoi(t->d_name);
            if (tid <= 0) continue;
            char tp[96], comm[64]; int prio = 0, pol = 0;
            snprintf(tp, sizeof tp, "/proc/%d/task/%d/stat", pid, tid);
            if (rtw_stat(tp, comm, sizeof comm, &prio, &pol) || (pol != SCHED_FIFO && pol != SCHED_RR && pol != 6)) continue;
            rtw_thread *x = &s->t[s->n++];
            x->pid = pid; x->tid = tid; x->prio = prio; x->pol = pol;
            snprintf(x->comm, sizeof x->comm, "%.23s", comm);
            x->ns = rtw_ns(pid, tid);
        }
        closedir(td);
    }
    closedir(d);
}

static inline int rtw_late(const rtw_snap *was, char *out, size_t len, double *span) {
    rtw_snap now_s;
    rtw_take(&now_s);
    *span = now_s.at - was->at;
    double ran[RTW_MAX];
    int idx[RTW_MAX], n = 0;
    for (int i = 0; i < now_s.n; i++) {
        /* in the last snapshot: what it ran since; not (started since, or real time since): all it ran, the window's
         * length at most */
        double s = (double)now_s.t[i].ns / 1e9;
        if (s > *span) s = *span;
        for (int j = 0; j < was->n; j++)
            if (now_s.t[i].tid == was->t[j].tid) { s = now_s.t[i].ns >= was->t[j].ns ? (double)(now_s.t[i].ns - was->t[j].ns) / 1e9 : 0; break; }
        if (s >= 0.05) { ran[n] = s; idx[n] = i; n++; }
    }
    for (int i = 1; i < n; i++)                        /* the most first */
        for (int j = i; j > 0 && ran[j] > ran[j - 1]; j--) { double r = ran[j]; ran[j] = ran[j - 1]; ran[j - 1] = r; int x = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = x; }
    size_t w = 0;
    out[0] = 0;
    for (int k = 0; k < n && k < 4 && w + 80 < len; k++) {
        const rtw_thread *x = &now_s.t[idx[k]];
        w += (size_t)snprintf(out + w, len - w, "%s« %s » (process %d, %s %d) %.1f s", k ? ", " : "", x->comm, x->pid,
                              x->pol == SCHED_RR ? "RR" : x->pol == 6 ? "DEADLINE" : "FIFO", x->prio, ran[k]);
    }
    return n < 4 ? n : 4;
}
#endif
#endif
