

#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../src/player/netout.h"
void anet_caps(unsigned caps);
void anet_rt(int prio);
void anet_meta(const char *lines, unsigned serial);

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
/* what the receiver says is heard after its DAC (netout_link_json afterMs), frames: in netout_held, never in the
 * pace (a player behind the receiver tells its latency once measured: held steps by that much, the pace does not) */
static double after_frames(const char *out, unsigned rate) {
    char lj[1400];
    netout_link_json(out, lj, sizeof lj);
    const char *a = strstr(lj, "\"afterMs\":");
    return a ? atof(a + 10) * rate / 1000.0 : 0;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: anet-loop <out=> <rate> <seconds> <sent file> [ahead|live] [caps]\n"); return 2; }
    const unsigned rate = (unsigned)atoi(argv[2]);
    const double secs = atof(argv[3]);
    const int live = argc > 5 && !strcmp(argv[5], "live");
    if (argc > 6) { anet_caps((unsigned)atoi(argv[6])); netout_link_convert(!!(atoi(argv[6]) & 16)); }   /* 16: ANET_CAP_RATES */
    if (getenv("ANET_RT")) anet_rt(atoi(getenv("ANET_RT")));   /* test/anet-load.sh: the packets' thread's priority */
    if (getenv("ANET_WRITER_RT")) {   /* and this thread's, the writer's (roomd's: SCHED_FIFO 94, a Mach time constraint) */
        struct sched_param sp = { .sched_priority = atoi(getenv("ANET_WRITER_RT")) };
        if (sp.sched_priority > 0 && pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp)) fprintf(stderr, "writer: no SCHED_FIFO\n");
    }
    FILE *sf = fopen(argv[4], "wb");
    netout_local_only(1);
    netout_airplay_conf("/nonexistent/audiolaz-test-config.xml");
    netout_claims(0, "");
    if (netout_init(19000 + (int)(getpid() % 500), 0)) { fprintf(stderr, "netout_init failed\n"); }
    netout_select(argv[1]);
    netout_live(live);
    const int lbits = getenv("ANET_LOOP_BITS") ? atoi(getenv("ANET_LOOP_BITS")) : 32;   /* */
    const double sine = getenv("ANET_LOOP_SINE") ? atof(getenv("ANET_LOOP_SINE")) : 0;   /* a sine instead */
    const double pace_from = getenv("ANET_LOOP_PACE_FROM") ? atof(getenv("ANET_LOOP_PACE_FROM")) : 2.0;
    if (getenv("ANET_LOOP_META")) {
        static char ml[1024];
        snprintf(ml, sizeof ml, "%s\n", getenv("ANET_LOOP_META"));
        for (char *c = ml; *c; c++) if (*c == ';') *c = '\n';
        anet_meta(ml, 1);
    }
    if (netout_open(rate)) { printf("OPEN refused %s\n", netout_why()); return 1; }
    const uint64_t total = (uint64_t)(secs * rate);
    static int32_t buf[2 * 4096];
    uint32_t x = 2463534242u;
    uint64_t done = 0, made = 0;
    long have = 0, off = 0;
    double t0 = 0, sum = 0, sum2 = 0, hmin = 1e18, hmax = 0; long ns = 0;
    double t_s = 0; uint64_t sent_at_s = 0;     /* the send rate measured from 2 s after the start */
    double t_last = now(), t_trace = 0;
    const double t_start = now();
    char lj[1400];
    /* test/anet-load.sh dry: this writer held up ANET_WRITER_STALL_MS every 3 s once playing (roomd late) */
    const double w_stall = getenv("ANET_WRITER_STALL_MS") ? atof(getenv("ANET_WRITER_STALL_MS")) / 1000.0 : 0;
    double t_wstall = 0;
    while (done < total) {
        if (off == have) {
            long want = (long)(total - made < 4096 ? total - made : 4096);
            if (live) {                                   /* the source's pace: no more than its clock has made */
                const uint64_t due = (uint64_t)((now() - t_start) * rate);
                if (due <= made) { usleep(2000); goto trace; }
                if ((uint64_t)want > due - made) want = (long)(due - made);
            }
            have = want; off = 0;
            for (long i = 0; i < have; i++) {
                const double ts = (double)(made + (uint64_t)i) / rate;
                const int quiet = live && (ts < 0.6 || fmod(ts, 5.0) >= 4.2);   /* its silences: exactly zero */
                if (sine > 0) {                           /* ANET_LOOP_SINE: a sine at −6 dBFS, both channels */
                    buf[2 * i] = buf[2 * i + 1] = quiet ? 0 : (int32_t)lrint(0.5 * sin(2 * M_PI * sine * ts) * 2147483647.0 * (lbits < 32 ? ldexp(1.0, lbits - 32) : 1.0));
                    continue;
                }
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                buf[2 * i] = quiet ? 0 : lbits < 32 ? (int32_t)x >> (32 - lbits) | 1 : (int32_t)(x | 1);   /* never zero in the music: its runs told apart */
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                buf[2 * i + 1] = quiet ? 0 : lbits < 32 ? (int32_t)x >> (32 - lbits) : (int32_t)x;
            }
            made += (uint64_t)have;
            fwrite(buf, 8, (size_t)have, sf);
        }
        if (w_stall > 0 && netout_started()) {
            const double tn = now();
            if (!t_wstall) t_wstall = tn + 1.0;
            else if (tn >= t_wstall) { usleep((useconds_t)(w_stall * 1e6)); t_wstall = now() + 3.0; }
        }
        {
            const long w = netout_write(buf + 2 * off, have - off);
            if (w < 0) { printf("LOST %s\n", netout_why()); return 1; }
            off += w; done += (uint64_t)w;
            if (w == 0) netout_wait(10);
        }
    trace:;
        const double t = now();
        if (netout_started()) {
            if (t0 == 0) t0 = t;
            if (t_s == 0 && t - t0 > pace_from) { t_s = t; sent_at_s = netout_sent() - (uint64_t)netout_held() + (uint64_t)after_frames(argv[1], rate); }
            if (t_s > 0 && t - t_last >= 0.05) {
                t_last = t;
                const double h = (double)netout_held() / rate * 1000;
                sum += h; sum2 += h * h; ns++; if (h < hmin) hmin = h; if (h > hmax) hmax = h;
            }
        }
        if (t - t_trace >= 0.5) {
            t_trace = t;
            netout_link_json(argv[1], lj, sizeof lj);
            char *r = strstr(lj, "\"reserveMs\":"), *g = strstr(lj, "\"targetMs\":"), *l = strstr(lj, "\"latencyMs\":"), *st = strstr(lj, "\"state\":\""), *si = strstr(lj, "\"silenceInMs\":");
            const char *af = strstr(lj, "\"afterMs\":");
            if (r && g && l && st && si) fprintf(stderr, "TRACE %.1f reserve %.0f target %.0f held %.0f silence %.0f after %.0f state %.*s\n", t - t_start, atof(r + 12), atof(g + 11), atof(l + 12), atof(si + 14), af ? atof(af + 10) : 0.0, (int)strcspn(st + 9, "\""), st + 9);
        }
    }
    const double t_e = now(); const uint64_t sent_e = netout_sent() - (uint64_t)netout_held() + (uint64_t)after_frames(argv[1], rate);
    fclose(sf);
    netout_end();
    for (double t = now(); netout_held() > 0 && now() - t < 10;) usleep(20000);
    netout_link_json(argv[1], lj, sizeof lj);
    netout_close(1);
    fprintf(stderr, "LINK %s\n", lj);
    const double ratio = t_e > t_s && t_s > 0 ? (double)(sent_e - sent_at_s) / (t_e - t_s) / rate : 0;
    const double mean = ns ? sum / ns : 0, sd = ns ? sqrt(fmax(0, sum2 / ns - mean * mean)) : 0;
    printf("RATE %u sent %llu frames; played rate / nominal %.6f (%+.0f ppm); held mean %.1f ms sd %.2f ms min %.1f max %.1f\n",
           rate, (unsigned long long)done, ratio, (ratio - 1) * 1e6, mean, sd, hmin, hmax);
    return 0;
}
