

#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double t0 = -1, started, clockpos, first_open = 0;
static long opens, refused_writes, refused_opens;
static int fresh;                                    /* opened, not written to yet */
static unsigned rate = 44100;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double env(const char *k, double d) { const char *v = getenv(k); return v ? atof(v) : d; }
static double since(void) { if (t0 < 0) t0 = now(); return now() - t0; }
__attribute__((constructor)) static void init(void) { since(); }

int snd_pcm_open(snd_pcm_t **p, const char *name, snd_pcm_stream_t s, int mode) {
    static int (*real)(snd_pcm_t **, const char *, snd_pcm_stream_t, int);
    if (!real) real = dlsym(RTLD_NEXT, "snd_pcm_open");
    if (s == SND_PCM_STREAM_PLAYBACK) {
        double t = since();
        if (t >= env("BUSY_FROM", 1e9) && t < env("BUSY_TO", 0)) { refused_opens++; return -EBUSY; }
        opens++;
        started = now(); clockpos = 0; fresh = 1;
        if (first_open <= 0) first_open = started;
    }
    return real(p, name, s, mode);
}

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *p, const void *buf, snd_pcm_uframes_t n) {
    static snd_pcm_sframes_t (*real)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
    if (!real) real = dlsym(RTLD_NEXT, "snd_pcm_writei");
    if (snd_pcm_stream(p) != SND_PCM_STREAM_PLAYBACK) return real(p, buf, n);
    /* (10/10 evening, test/engine-inputs.sh part airbuf) STALL_AFTER/STALL_S: once, that long after the first playback
     * open, a write held STALL_S seconds (an output that stops taking for a moment: the ring fills meanwhile); PASS=1:
     * nothing else done here (the Loopback as the card, paced by itself) */
    static int stalled = 0;
    if (!stalled && env("STALL_S", 0) > 0 && first_open > 0 && now() - first_open >= env("STALL_AFTER", 1e9)) {
        const double h = env("STALL_S", 0);
        struct timespec s = { (time_t)h, (long)((h - (long)h) * 1e9) };
        stalled = 1;
        nanosleep(&s, NULL);
    }
    if (env("PASS", 0) > 0) return real(p, buf, n);
    double t = since();
    if (t >= env("FAIL_FROM", 1e9) && t < env("FAIL_TO", 0)) { refused_writes++; return -EPIPE; }
    if (fresh) {
        double h = env("HOLD_FIRST", 0);
        fresh = 0;
        if (h > 0) { struct timespec s = { (time_t)h, (long)((h - (long)h) * 1e9) }; nanosleep(&s, NULL); started = now(); clockpos = 0; }
    }
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    if (snd_pcm_hw_params_current(p, hw) == 0) snd_pcm_hw_params_get_rate(hw, &rate, NULL);
    clockpos += (double)n / rate;                    /* a card a quarter of a second deep, played in real time */
    double ahead = clockpos - (now() - started) - 0.25;
    if (ahead > 0) { struct timespec s = { (time_t)ahead, (long)((ahead - (long)ahead) * 1e9) }; nanosleep(&s, NULL); }
    return real(p, buf, n);
}

int snd_pcm_hw_params_set_format(snd_pcm_t *p, snd_pcm_hw_params_t *hw, snd_pcm_format_t f) {
    static int (*real)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_format_t);
    if (!real) real = dlsym(RTLD_NEXT, "snd_pcm_hw_params_set_format");
    if (env("REFUSE_DSD", 0) > 0 && (f == SND_PCM_FORMAT_DSD_U8 || f == SND_PCM_FORMAT_DSD_U16_LE || f == SND_PCM_FORMAT_DSD_U16_BE ||
                                     f == SND_PCM_FORMAT_DSD_U32_LE || f == SND_PCM_FORMAT_DSD_U32_BE)) return -EINVAL;
    return real(p, hw, f);
}

static int over_max(snd_pcm_t *p, unsigned r) {
    return snd_pcm_stream(p) == SND_PCM_STREAM_PLAYBACK && env("MAX_RATE", 0) > 0 && r > env("MAX_RATE", 0);
}
int snd_pcm_hw_params_set_rate(snd_pcm_t *p, snd_pcm_hw_params_t *hw, unsigned int r, int dir) {
    static int (*real)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int, int);
    if (!real) real = dlsym(RTLD_NEXT, "snd_pcm_hw_params_set_rate");
    return over_max(p, r) ? -EINVAL : real(p, hw, r, dir);
}
int snd_pcm_hw_params_test_rate(snd_pcm_t *p, snd_pcm_hw_params_t *hw, unsigned int r, int dir) {
    static int (*real)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int, int);
    if (!real) real = dlsym(RTLD_NEXT, "snd_pcm_hw_params_test_rate");
    return over_max(p, r) ? -EINVAL : real(p, hw, r, dir);
}

__attribute__((destructor)) static void report(void) {
    fprintf(stderr, "SHIM: playback opens %ld, refused writes %ld, refused opens %ld\n", opens, refused_writes, refused_opens);
}
