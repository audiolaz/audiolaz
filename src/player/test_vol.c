/* src/player/test_vol.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vol.h"
#include "pcmclock.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double urand(uint64_t *s) { return (double)(dither_next(s) >> 11) * (1.0 / 9007199254740992.0) * 2.0 - 1.0; }

/* the gain of each frame (the stage on a buffer of ones) */
static void gains(vol_ramp *r, double *g, long n, double rate) {
    double *x = malloc(sizeof(double) * 2 * (size_t)n);
    for (long i = 0; i < 2 * n; i++) x[i] = 1.0;
    vol_ramp_run(r, x, n, rate);
    for (long i = 0; i < n; i++) g[i] = x[2 * i];
    free(x);
}

static void unity(void) {
    const long n = 48000;
    double *a = malloc(sizeof(double) * 2 * n), *b = malloc(sizeof(double) * 2 * n);
    uint64_t s = 12345;
    for (long i = 0; i < 2 * n; i++) a[i] = urand(&s);
    memcpy(b, a, sizeof(double) * 2 * n);
    vol_ramp r = { 0 };
    vol_ramp_run(&r, b, n, 48000);                             /* off */
    CHECK(!memcmp(a, b, sizeof(double) * 2 * n), "off: untouched");
    vol_ramp_take(&r, 1.0, 0.0, 0);                            /* on at 0 dB, from the source's full level */
    CHECK(vol_ramp_unity(&r), "on at 0 dB: unity");
    vol_ramp_run(&r, b, n, 48000);
    CHECK(!memcmp(a, b, sizeof(double) * 2 * n), "on at 0 dB: bit for bit the input");
    vol_ramp_to(&r, -30, 0);                                   /* down and back up: then untouched again */
    for (int k = 0; k < 10; k++) { memcpy(b, a, sizeof(double) * 2 * n); vol_ramp_run(&r, b, n, 48000); }
    CHECK(fabs(r.db + 30) < 1e-12 && memcmp(a, b, sizeof(double) * 2 * n), "at −30 dB: the music lower");
    vol_ramp_to(&r, 0, 0);
    for (int k = 0; k < 10; k++) { memcpy(b, a, sizeof(double) * 2 * n); vol_ramp_run(&r, b, n, 48000); }
    memcpy(b, a, sizeof(double) * 2 * n);
    vol_ramp_run(&r, b, n, 48000);
    CHECK(vol_ramp_unity(&r) && r.db == 0.0 && !memcmp(a, b, sizeof(double) * 2 * n), "back at 0 dB: bit for bit again (db %.17g)", r.db);
    /* taken over from nothing playing: at once where it goes, no ramp */
    vol_ramp q = { 0 };
    vol_ramp_take(&q, NAN, -25, 0);
    CHECK(q.db == -25 && q.mg == 1.0 && vol_ramp_settled(&q), "a play's start: at the level at once");
    vol_ramp_take(&q, 0.1, -25, 0);
    CHECK(fabs(q.db + 20) < 1e-9 && !vol_ramp_settled(&q), "taken from a source's −20 dB: from there (%.3f)", q.db);
    free(a); free(b);
}

static void blocks(void) {
    const long n = 20000;
    double *ref = malloc(sizeof(double) * 2 * n), *x = malloc(sizeof(double) * 2 * n);
    uint64_t s = 99;
    for (long i = 0; i < 2 * n; i++) ref[i] = urand(&s);
    const long sizes[] = { 1, 37, 4096, n };
    double *first = NULL;
    for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        memcpy(x, ref, sizeof(double) * 2 * n);
        vol_ramp r = { 0 };
        vol_ramp_take(&r, 1.0, -12, 0);
        for (long o = 0; o < n; o += sizes[k]) {
            const long m = o + sizes[k] > n ? n - o : sizes[k];
            vol_ramp_run(&r, x + 2 * o, m, 44100);
        }
        if (!first) { first = malloc(sizeof(double) * 2 * n); memcpy(first, x, sizeof(double) * 2 * n); }
        else CHECK(!memcmp(first, x, sizeof(double) * 2 * n), "blocks of %ld frames: the same frames", sizes[k]);
    }
    free(first); free(ref); free(x);
}

static void ramps(void) {
    const double rate = 44100;
    const long n = (long)(4 * rate);
    double *g = malloc(sizeof(double) * (size_t)n);
    vol_ramp r = { 0 };
    vol_ramp_take(&r, 1.0, -30, 0);                            /* turned on, from full: down to −30 dB */
    gains(&r, g, n, rate);
    double dmax = 0, prev = 1.0;
    int mono = 1;
    for (long i = 0; i < n; i++) { const double d = 20 * log10(g[i]) - 20 * log10(prev); if (fabs(d) > dmax) dmax = fabs(d); if (g[i] > prev) mono = 0; prev = g[i]; }
    CHECK(mono && dmax <= VOL_DOWN_DBS / rate + 1e-9 && fabs(20 * log10(g[n - 1]) + 30) < 1e-9, "0 → −30 dB: falling, %.6f dB a frame at most, there", dmax);
    long t = -1;
    vol_ramp_to(&r, -24, 0);                                   /* up 6 dB: 0.6 s at least */
    gains(&r, g, n, rate);
    prev = exp(-30 * VOL_LN10_20); dmax = 0; mono = 1;
    for (long i = 0; i < n; i++) { const double d = 20 * log10(g[i]) - 20 * log10(prev); if (fabs(d) > dmax) dmax = fabs(d); if (g[i] < prev) mono = 0; if (t < 0 && fabs(20 * log10(g[i]) + 24) < 1e-9) t = i; prev = g[i]; }
    CHECK(mono && dmax <= VOL_UP_DBS / rate + 1e-9 && VOL_UP_DBS == 10.0 && t >= (long)(0.6 * rate) - 2 && t <= (long)(0.6 * rate) + 2, "−30 → −24 dB: rising, %.6f dB a frame at most (10 dB a second), there after %.3f s", dmax, t / rate);
    vol_ramp_to(&r, -20, 0);
    gains(&r, g, n, rate);
    vol_ramp_to(&r, -20, 1);                                   /* mute: a fade, no step */
    gains(&r, g, n, rate);
    prev = exp(-20 * VOL_LN10_20); dmax = 0; mono = 1; t = -1;
    for (long i = 0; i < n; i++) { const double d = fabs(g[i] - prev); if (d > dmax) dmax = d; if (g[i] > prev) mono = 0; if (t < 0 && g[i] == 0) t = i; prev = g[i]; }
    CHECK(mono && dmax < 1e-3 && t > 0 && t <= (long)(VOL_MUTE_S * rate) + 2, "mute: faded out in %.1f ms, %.2e a frame at most", 1000.0 * t / rate, dmax);
    vol_ramp_to(&r, -20, 0);
    gains(&r, g, n, rate);
    prev = 0; dmax = 0; mono = 1;
    for (long i = 0; i < n; i++) { const double d = fabs(g[i] - prev); if (d > dmax) dmax = d; if (g[i] < prev) mono = 0; prev = g[i]; }
    CHECK(mono && dmax < 1e-3 && fabs(20 * log10(g[n - 1]) + 20) < 1e-9, "unmute: back to −20 dB, %.2e a frame at most", dmax);
    /* a level and a mute together, then a mute taken back half way: still no step */
    vol_ramp_to(&r, -40, 1);
    gains(&r, g, 1000, rate);
    vol_ramp_to(&r, -10, 0);
    double *h = malloc(sizeof(double) * (size_t)n);
    gains(&r, h, n, rate);
    dmax = fabs(h[0] - g[999]);
    for (long i = 1; i < n; i++) if (fabs(h[i] - h[i - 1]) > dmax) dmax = fabs(h[i] - h[i - 1]);
    CHECK(dmax < 1e-3 && fabs(20 * log10(h[n - 1]) + 10) < 1e-9, "turned around mid-way: %.2e a frame at most, there", dmax);
    free(h); free(g);
}

static void rules(void) {
    const double P = VOL_MAX_PROV;                             /* the provisional ceiling, −30 dB */
    CHECK(VOL_ON_DB == -70.0 && VOL_JUMP_DB == 6.0 && P == -30.0, "the safety rules' numbers: on at −70 dB, +6 dB a word, −30 dB until a maximum is set");
    CHECK(vol_on_level(NAN, NAN) == -70 && vol_on_level(-10, NAN) == -70 && vol_on_level(-75, NAN) == -75 && vol_on_level(-200, NAN) == VOL_MIN_DB, "on: −70 dB, or lower kept");
    CHECK(vol_on_level(NAN, -72) == -72 && vol_on_level(NAN, 0) == -70, "on: never above the ceiling");
    CHECK(vol_rule(-30, 0, 1, P) == -30, "a first word at 100 %%: the level kept");
    CHECK(vol_rule(-30, -0.01, 1, P) == -30, "a first word at 99.9 %%: the level kept");
    CHECK(vol_rule(-30, -40, 1, P) == -40, "a first word below: taken");
    CHECK(vol_rule(-40, -36, 1, P) == -36 && vol_rule(-40, 0, 1, P) == -40, "a first word up: by its step");
    CHECK(vol_rule(-70, 0, 0, 0) == -64, "up 70 dB asked: 6 dB");
    CHECK(vol_rule(-64, 0, 0, 0) == -58 && vol_rule(-58, -50, 0, 0) == -52, "and again, 6 dB a word");
    CHECK(vol_rule(-34, 0, 0, P) == -30 && vol_rule(-30, 0, 0, P) == -30 && vol_rule(-30, -29, 0, P) == -30, "never above the provisional ceiling (−30 dB), whatever the source asks");
    CHECK(vol_rule(-30, 0, 0, NAN) == -30, "no maximum set: the provisional ceiling");
    CHECK(vol_rule(-20, 0, 0, -18) == -18 && vol_rule(-18, -10, 0, -18) == -18, "the listener's maximum, −18 dB: never above");
    CHECK(vol_rule(-12, NAN, 0, -18) == -18 && vol_rule(-12, -25, 0, -18) == -25, "a lower maximum brings the level down");
    CHECK(vol_rule(-6, 0, 0, 0) == 0 && vol_rule(-6, 0, 0, 3) == 0, "0 dB only with the maximum there");
    CHECK(vol_rule(-30, -50, 0, P) == -50 && vol_rule(-30, -300, 0, P) == VOL_MIN_DB && vol_rule(-30, -INFINITY, 0, P) == VOL_MIN_DB, "down: any, the floor at −80 dB");
    CHECK(vol_rule(-30, NAN, 0, P) == -30 && vol_rule(NAN, -75, 0, P) == -75 && vol_rule(5, 3, 0, 0) == 0, "unchanged; none kept (−70 dB); never above 0 dB");
    CHECK(vol_max_clamp(NAN) == P && vol_max_clamp(4) == 0 && vol_max_clamp(-200) == VOL_MIN_DB, "a ceiling as kept");
    CHECK(vol_db_of_spotify(65535) == 0 && fabs(vol_db_of_spotify(32768) + 29.999) < 0.01 && vol_db_of_spotify(0) == VOL_MIN_DB, "Spotify: 100 %% 0 dB, 50 %% −30 dB");
    CHECK(vol_spotify_of_db(0) == 65535 && vol_spotify_of_db(-30) == 32768 && vol_spotify_of_db(-70) == 1, "Spotify back: −30 dB 32768, below −60 dB its lowest step");
    int rt = 1;
    for (unsigned v = 1; v <= 65535; v += 97) if (vol_spotify_of_db(vol_db_of_spotify(v)) != v) rt = 0;
    CHECK(rt, "Spotify: there and back exact");
    CHECK(vol_db_of_upnp(100) == 0 && fabs(vol_db_of_upnp(50) + 12.0412) < 1e-3 && fabs(vol_db_of_upnp(10) + 40) < 1e-9 && vol_db_of_upnp(0) == VOL_MIN_DB, "UPnP: 50 −12 dB, 10 −40 dB (upnp.c's)");
    rt = 1;
    for (int v = 1; v <= 100; v++) if (vol_upnp_of_db(vol_db_of_upnp(v)) != v) rt = 0;
    CHECK(rt && vol_upnp_of_db(-80) == 1, "UPnP: there and back exact");
}

/* a DFT bin's power (Goertzel, long double) */
static long double bin(const long double *e, long n, double k) {
    const long double w = 2.0L * (long double)M_PI * (long double)k / (long double)n, c = 2.0L * cosl(w);
    long double s1 = 0, s2 = 0;
    for (long i = 0; i < n; i++) { const long double s0 = e[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}
/* the strongest of the sine's first harmonics against the error's mean bin power (dB) */
static double harmonics_db(const long double *e, long n, long cyc) {
    long double tot = 0, top = 0;
    for (long i = 0; i < n; i++) tot += e[i] * e[i];
    const long double mean = tot;                              /* Parseval: |X_k|² averages Σe² */
    for (int h = 1; h <= 9; h++) { const long double p = bin(e, n, (double)(h * cyc)); if (p > top) top = p; }
    return (double)(10.0L * log10l(top / mean));
}

static void precision(void) {
    const double rate = 48000;
    const long n = 1L << 18, cyc = 4001;                       /* a whole number of cycles: no leakage (733 Hz) */
    const double amp = 8388607.0 / 8388608.0;                  /* 24-bit full scale */
    double *x = malloc(sizeof(double) * 2 * (size_t)n), *one = malloc(sizeof(double) * 2 * (size_t)n);
    long double *ref = malloc(sizeof(long double) * (size_t)n);
    for (long i = 0; i < n; i++) {
        const double v = floor(amp * sin(2 * M_PI * (double)cyc * (double)i / (double)n) * 8388608.0 + 0.5) / 8388608.0;   /* on the 24-bit grid */
        x[2 * i] = x[2 * i + 1] = v;
        one[2 * i] = one[2 * i + 1] = 1.0;
    }
    /* −0.01 dB every 50 ms from 0 dB (the gain's own run on ones, the same steps: the exact product's gain) */
    vol_ramp a = { 0 }, b = { 0 };
    vol_ramp_take(&a, 1.0, 0, 0); vol_ramp_take(&b, 1.0, 0, 0);
    const long step = (long)(0.05 * rate);
    double tdb = 0;
    for (long o = 0; o < n; o += step) {
        const long m = o + step > n ? n - o : step;
        tdb -= 0.01;
        vol_ramp_to(&a, tdb, 0); vol_ramp_to(&b, tdb, 0);
        vol_ramp_run(&a, x + 2 * o, m, rate);
        vol_ramp_run(&b, one + 2 * o, m, rate);
    }
    /* the stage against the exact product: 64-bit throughout */
    long double worst = 0;
    for (long i = 0; i < n; i++) {
        const long double g = (long double)one[2 * i];
        const long double t = (long double)(floor(amp * sin(2 * M_PI * (double)cyc * (double)i / (double)n) * 8388608.0 + 0.5) / 8388608.0) * g;
        ref[i] = t;
        const long double d = fabsl((long double)x[2 * i] - t);
        if (d > worst) worst = d;
    }
    CHECK(worst <= 4e-16L, "64 bits: the stage within %.2Le of the exact product (a float32 step: ~6e-8)", worst);
    float fw = 0;
    for (long i = 0; i < n; i++) { const float f = (float)x[2 * i]; const float d = (float)fabsl((long double)f - ref[i]); if (d > fw) fw = d; }
    CHECK(fw > 1e-9f, "the check's control: a float32 step is seen (%.2e)", (double)fw);
    /* to 24 bits once, the engine's TPDF dither: its noise alone; the controls: the same through a float32 step, and
     * plain truncation (no dither) */
    uint64_t rng = 0x9E3779B97F4A7C15ull, rng2 = 0x9E3779B97F4A7C15ull;
    long double *e = malloc(sizeof(long double) * (size_t)n);
    long double s2 = 0, s1 = 0, emax = 0, cor = 0, sx = 0, f2 = 0, t1 = 0;
    for (long i = 0; i < n; i++) {
        const int32_t q = s32_out_q(x[2 * i], 24, &rng);
        const long double lsb = ((long double)q / 2147483648.0L - ref[i]) * 8388608.0L;   /* in 24-bit LSBs */
        e[i] = lsb; s2 += lsb * lsb; s1 += lsb; if (fabsl(lsb) > emax) emax = fabsl(lsb);
        cor += lsb * ref[i]; sx += ref[i] * ref[i];
        const int32_t qf = s32_out_q((double)(float)x[2 * i], 24, &rng2);
        const long double lf = ((long double)qf / 2147483648.0L - ref[i]) * 8388608.0L;
        f2 += lf * lf;
        t1 += floorl(ref[i] * 8388608.0L) - ref[i] * 8388608.0L;
    }
    const double rms = (double)sqrtl(s2 / n), mean = (double)(s1 / n), hd = harmonics_db(e, n, cyc);
    const double corr = (double)(cor / sqrtl(s2 * sx)), frms = (double)sqrtl(f2 / n), tmean = (double)(t1 / n);
    CHECK(rms > 0.497 && rms < 0.503 && fabs(mean) < 0.005, "24 bits, TPDF: the error %.4f LSB rms, mean %+.4f (the dither's alone: 0.5, 0)", rms, mean);
    CHECK(emax <= 1.5L, "24 bits, TPDF: %.3Lf LSB at most", emax);
    CHECK(hd < 10.0 && fabs(corr) < 0.01, "24 bits, TPDF: no harmonic above the floor (the strongest %+.1f dB over a bin's mean, correlation %.4f)", hd, corr);
    CHECK(fabs(frms - 0.5) > 0.02, "the check's control: a float32 step is seen (%.4f LSB rms: half its samples land on the 24-bit grid, undithered)", frms);
    CHECK(tmean < -0.45, "the check's control: truncation is seen (its error's mean %+.3f LSB)", tmean);
    printf("vol: 64 bits %.2Le off the exact product; 24 bits TPDF %.4f LSB rms, mean %+.4f, max %.3Lf, harmonics %+.1f dB; float32 would be %.4f\n", worst, rms, mean, emax, hd, frms);
    free(x); free(one); free(ref); free(e);
}

/* the free player's words (vol_ramp_run_s32) against audiolaz Room's doubles (vol_ramp_run then
 * s32_out_q): the same stage */
static void free_words(void) {
    const long n = 48000;
    int32_t *src = malloc(sizeof(int32_t) * 2 * n), *w = malloc(sizeof(int32_t) * 2 * n), *want = malloc(sizeof(int32_t) * 2 * n);
    double *x = malloc(sizeof(double) * 2 * n), *d = malloc(sizeof(double) * 2 * n);
    uint64_t s = 4242;
    for (long i = 0; i < 2 * n; i++) src[i] = (int32_t)(urand(&s) * 0.25 * 8388608.0) * 256;   /* 24-bit words, as a DAC's */
    /* off: untouched, nothing done */
    vol_ramp r = { 0 };
    memcpy(w, src, sizeof(int32_t) * 2 * n);
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    int done = vol_ramp_run_s32(&r, w, x, n, 48000, 32, &rng);
    CHECK(!done && !memcmp(w, src, sizeof(int32_t) * 2 * n), "free: off, the words untouched");
    /* on at 0 dB: untouched */
    vol_ramp_take(&r, 1.0, 0.0, 0);
    done = vol_ramp_run_s32(&r, w, x, n, 48000, 24, &rng);
    CHECK(!done && !memcmp(w, src, sizeof(int32_t) * 2 * n) && rng == 0x9E3779B97F4A7C15ull, "free: on at 0 dB, the words untouched, no dither drawn");
    /* down to −20 dB, blocks of 37 frames: each word audiolaz Room's (its doubles, the same requantization) */
    vol_ramp a = r, b = r;
    vol_ramp_to(&a, -20, 0); vol_ramp_to(&b, -20, 0);
    uint64_t ra = 1234567, rb = 1234567;
    memcpy(w, src, sizeof(int32_t) * 2 * n);
    for (long i = 0; i < 2 * n; i++) d[i] = s32_in(src[i]);
    vol_ramp_run(&b, d, n, 48000);
    for (long i = 0; i < 2 * n; i++) want[i] = s32_out_q(d[i], 24, &rb);
    int all = 1;
    for (long o = 0; o < n; o += 37) { const long m = o + 37 > n ? n - o : 37; all &= vol_ramp_run_s32(&a, w + 2 * o, x, m, 48000, 24, &ra); }
    long diff = 0;
    for (long i = 0; i < 2 * n; i++) diff += w[i] != want[i];
    CHECK(all && diff == 0 && ra == rb, "free: down to −20 dB, every word audiolaz Room's (%ld differ)", diff);
    CHECK(fabs(a.db + 20) < 1e-12, "free: at −20 dB (%.6f)", a.db);
    /* settled at −20 dB: still the stage's (a level), then back to 0 dB: untouched again */
    memcpy(w, src, sizeof(int32_t) * 2 * n);
    CHECK(vol_ramp_run_s32(&a, w, x, n, 48000, 24, &ra) && memcmp(w, src, sizeof(int32_t) * 2 * n), "free: at −20 dB, the music lower");
    vol_ramp_to(&a, 0, 0);
    for (int k = 0; k < 4; k++) { memcpy(w, src, sizeof(int32_t) * 2 * n); vol_ramp_run_s32(&a, w, x, n, 48000, 24, &ra); }
    const uint64_t r0 = ra;
    memcpy(w, src, sizeof(int32_t) * 2 * n);
    done = vol_ramp_run_s32(&a, w, x, n, 48000, 24, &ra);
    CHECK(vol_ramp_unity(&a) && !done && !memcmp(w, src, sizeof(int32_t) * 2 * n) && ra == r0, "free: back at 0 dB, the words untouched again");
    /* a ramp landing on 0 dB within a block: the words after it as they came (on the grid: never dithered) */
    vol_ramp c = { 0 };
    vol_ramp_take(&c, 1.0, -0.5, 0);
    c.db = -0.5; c.tdb = 0;                            /* 0.05 s from 0 dB at 10 dB/s */
    memcpy(w, src, sizeof(int32_t) * 2 * n);
    vol_ramp_run_s32(&c, w, x, n, 48000, 24, &ra);
    long after = 0;
    for (long i = 2 * 2400 + 2; i < 2 * n; i++) after += w[i] != src[i];
    CHECK(after == 0, "free: from the frame the ramp lands at 0 dB, the words as they came (%ld differ)", after);
    printf("vol: free player's words: off, 0 dB and back untouched; at −20 dB each word audiolaz Room's\n");
    free(src); free(w); free(want); free(x); free(d);
}

int main(void) {
    unity();
    blocks();
    ramps();
    rules();
    precision();
    free_words();
    if (fails) { printf("vol: %d failed\n", fails); return 1; }
    printf("vol: ok\n");
    return 0;
}
