

#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/player/src_plat.h"
#include "../src/player/src.h"

static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static uint32_t hash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
/* the 32-bit value the reader must see for raw x in that format */
static int32_t want(const char *f, uint32_t x) {
    if (!strcmp(f, "S16")) return (int32_t)(x & 0xFFFF0000u);
    if (!strcmp(f, "S32")) return (int32_t)x;
    return (int32_t)(x & 0xFFFFFF00u);                                   /* S24, S24_3, FLOAT */
}
static uint32_t ch0(uint64_t i) { return (uint32_t)(i << 16); }          /* 16 bits of the frame number: every format */

static int writer(const char *dev, const char *f, unsigned rate, double secs) {
    snd_pcm_format_t fm = !strcmp(f, "S16") ? SND_PCM_FORMAT_S16_LE : !strcmp(f, "S24") ? SND_PCM_FORMAT_S24_LE :
                          !strcmp(f, "S24_3") ? SND_PCM_FORMAT_S24_3LE : !strcmp(f, "FLOAT") ? SND_PCM_FORMAT_FLOAT_LE : SND_PCM_FORMAT_S32_LE;
    snd_pcm_t *p;
    int e = snd_pcm_open(&p, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (e < 0) { fprintf(stderr, "open %s: %s\n", dev, snd_strerror(e)); return 1; }
    if ((e = snd_pcm_set_params(p, fm, SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 0, 100000)) < 0) { fprintf(stderr, "params: %s\n", snd_strerror(e)); return 1; }
    const int bps = snd_pcm_format_physical_width(fm) / 8;
    enum { N = 1024 };
    unsigned char *b = malloc((size_t)N * 2 * 4);
    uint64_t i = 0, total = (uint64_t)(secs * rate);
    double t0 = 0, tw = 0; uint64_t i_t0 = 0, i_tw = 0;
    while (i < total) {
        for (int k = 0; k < N; k++) {
            const uint32_t v[2] = { ch0(i + k), hash((uint32_t)(i + k)) };
            for (int c = 0; c < 2; c++) {
                unsigned char *d = b + ((size_t)k * 2 + c) * bps;
                const uint32_t x = (uint32_t)want(f, v[c]);
                if (fm == SND_PCM_FORMAT_FLOAT_LE) { float fl = (float)((int32_t)x / 2147483648.0); memcpy(d, &fl, 4); }
                else if (bps == 2) { const uint16_t s = (uint16_t)(x >> 16); memcpy(d, &s, 2); }
                else if (bps == 3) { d[0] = (unsigned char)(x >> 8); d[1] = (unsigned char)(x >> 16); d[2] = (unsigned char)(x >> 24); }
                else if (fm == SND_PCM_FORMAT_S24_LE) { const int32_t s = (int32_t)x >> 8; memcpy(d, &s, 4); }
                else memcpy(d, &x, 4);
            }
        }
        snd_pcm_sframes_t n = snd_pcm_writei(p, b, N);
        if (n < 0) { fprintf(stderr, "write: %s\n", snd_strerror((int)n)); n = snd_pcm_recover(p, (int)n, 0); if (n < 0) return 1; continue; }
        i += (uint64_t)n;
        snd_pcm_sframes_t dl = 0;                                      /* consumed: written less what waits */
        if (snd_pcm_delay(p, &dl) < 0) dl = 0;
        if (!t0 && i > (uint64_t)rate) { t0 = mono(); i_t0 = i - (uint64_t)dl; }   /* after its first second (the prefill) */
        tw = mono(); i_tw = i - (uint64_t)dl;
    }
    const double pace = (double)(i_tw - i_t0) / (tw - t0);
    snd_pcm_drain(p);
    snd_pcm_close(p);
    printf("writer %s %u: %llu frames, pace %+.1f ppm against the host clock\n", f, rate, (unsigned long long)i, (pace / rate - 1) * 1e6);
    return 0;
}

static int reader(unsigned rate, double secs, double ppm, const char *f) {
    src_option("--in-driver", "0");
    src_init("none-here", 1, "/tmp");
    double t = mono() + 10;
    while (!(src_active(0) && src_rate(0) == rate)) { if (mono() > t) { fprintf(stderr, "reader: no « audiolaz » at %u\n", rate); return 1; } struct timespec z = { 0, 2000000 }; nanosleep(&z, NULL); }
    src_pcm *s = src_open(0, rate, 2, rate / 500, 4);
    if (!s) return 1;
    const unsigned long per = rate / 500;                              /* 2 ms */
    int32_t *b = malloc(per * 2 * 4 * 8);
    uint64_t got = 0, frame = 0, bad = 0, again = 0, zero = 0; int sync = 0;
    const double t0 = mono(), r_dac = rate * (1 + ppm * 1e-6);
    double due = 0;                                                    /* frames the fake DAC took so far */
    while (mono() - t0 < secs) {
        due = (mono() - t0) * r_dac + per * 4;                         /* a 4-period buffer kept full */
        while (got < due) {
            long n = src_read(s, b, per);
            if (n == SRC_AGAIN) { again++; break; }
            if (n < 0) { fprintf(stderr, "reader: %s\n", src_strerror(n)); src_recover(s, n); break; }
            for (long k = 0; k < n; k++) {
                if (!sync) { frame = (uint32_t)b[2 * k] >> 16; sync = 1; }
                if (!b[2 * k] && !b[2 * k + 1] && (frame & 0xFFFF)) { zero++; frame++; continue; }   /* aplay's last period padded */
                if (b[2 * k] != (int32_t)ch0(frame) || b[2 * k + 1] != want(f, hash((uint32_t)frame))) { if (bad++ < 3) fprintf(stderr, "frame %llu: %08x %08x\n", (unsigned long long)frame, b[2*k], b[2*k+1]); }
                frame++;
            }
            got += (uint64_t)n;
        }
        if (!src_active(0) && got) break;
        struct timespec z = { 0, 1000000 }; nanosleep(&z, NULL);
    }
    src_close(s);
    printf("reader %s %u (DAC %+.0f ppm): %llu frames checked, %llu wrong, %llu silent, %llu reads short\n", f, rate, ppm, (unsigned long long)got, (unsigned long long)bad, (unsigned long long)zero, (unsigned long long)again);
    return bad || got < rate ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc >= 6 && !strcmp(argv[1], "write")) return writer(argv[2], argv[3], (unsigned)atoi(argv[4]), atof(argv[5]));
    if (argc >= 6 && !strcmp(argv[1], "read")) return reader((unsigned)atoi(argv[2]), atof(argv[3]), atof(argv[4]), argv[5]);
    fprintf(stderr, "usage: linux-azdrv write <dev> <fmt> <rate> <s> | read <rate> <s> <ppm> <fmt>\n");
    return 2;
}
