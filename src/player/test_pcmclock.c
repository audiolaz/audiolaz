/* Checks pcmclock.h : the samples out of the player bit for bit as they came in, and the clock regulator against
 * a step in the ring (a gap of the source, a refill after a silence) as well as a steady gap between the clocks.
 *   gcc -std=gnu11 -O2 -Wall -Wextra -o test_pcmclock engine/alsa/test_pcmclock.c -lm && ./test_pcmclock */
#include <stdio.h>
#include <stdlib.h>
#include "pcmclock.h"

static int fails = 0;
static void check(int ok, const char *what) { printf("%-80s %s\n", what, ok ? "OK" : "WRONG"); if (!ok) fails++; }

/* The ring (seconds of audio) over `minutes`, the source's clock `drift` ppm faster than the DAC's, a step of `step` s
 * at minute 2; returns the integral at the end, its largest magnitude, and the minutes the ring stayed off by > 20 ms
 * after the step. law 1: as now; law 0: the old one (integral everywhere, ±500). */
static void run(int law, double drift, double step, int minutes, double *steady_end, double *steady_max, double *off_min, double *ring_dev_end) {
    const double target = 0.8, dt = 0.1, span = 10.0;
    double ring = target, steady = 0, ppm = 0, sum = 0, smax = 0, off = 0;
    int n = 0;
    for (long k = 0; k < (long)(minutes * 60 / dt); k++) {
        const double t = k * dt;
        if (step != 0 && t >= 120 && t < 120 + dt) ring += step;
        ring += (drift - ppm) * 1e-6 * dt;            /* ppm > 0 slows the source */
        if (ring < 0) ring = 0;
        sum += ring; n++;
        if (n == (int)(span / dt)) {
            const double e = sum / n - target;
            if (law) ppm = clock_regulate(e, span, &steady);
            else {
                steady += 30.0 * e * span;
                if (steady > 500) steady = 500; else if (steady < -500) steady = -500;
                ppm = 10000.0 * e + steady;
                if (ppm > 600) ppm = 600; else if (ppm < -600) ppm = -600;
            }
            sum = 0; n = 0;
        }
        if (fabs(steady) > smax) smax = fabs(steady);
        if (t > 120 && fabs(ring - target) > 0.02) off += dt / 60;
    }
    *steady_end = steady; *steady_max = smax; *off_min = off; *ring_dev_end = ring - target;
}

int main(void) {
    /* 1. bit for bit: every 24-bit sample (as a 32-bit word), and 32-bit ones */
    long bad = 0, bad_old = 0;
    for (long v = -8388608; v <= 8388607; v++) {
        const int32_t w = (int32_t)((uint32_t)v << 8);
        if (s32_out(s32_in(w)) != w) bad++;
        const double o = s32_in(w) * 2147483647.0;   /* the old way out */
        const int32_t wo = o >= 2147483647.0 ? 2147483647 : (int32_t)lrint(o);
        if ((wo >> 8) != (w >> 8)) bad_old++;          /* read back by a 24-bit DAC */
    }
    srand(9);
    for (long i = 0; i < 20000000; i++) {
        const int32_t w = (int32_t)(((uint32_t)rand() << 16) ^ (uint32_t)rand() ^ ((uint32_t)rand() << 31));
        if (s32_out(s32_in(w)) != w) bad++;
    }
    if (s32_out(s32_in(INT32_MIN)) != INT32_MIN || s32_out(s32_in(INT32_MAX)) != INT32_MAX) bad++;
    printf("  (the old conversion: %ld of the 16.8 million 24-bit values one step low through a 24-bit DAC)\n", bad_old);
    check(bad == 0, "every 24-bit value and 20 million 32-bit ones out bit for bit as they came in");
    check(s32_out(1.5) == INT32_MAX && s32_out(-1.5) == INT32_MIN && s32_out(1.0) == INT32_MAX, "beyond full scale: clamped");

    /* 1b. to the DAC's 24 bits with TPDF dither (s32_out_q): what is on the grid untouched, the rest on it, the
     * error the same whatever the music (mean 0, variance 1/4 LSB², TPDF's 1/6 plus rounding's 1/12) */
    {
        uint64_t rng = 0x9E3779B97F4A7C15ull;
        long moved = 0, off_grid = 0;
        for (long v = -8388608; v <= 8388607; v++) {
            const int32_t w = (int32_t)((uint32_t)v << 8);
            if (s32_out_q(s32_in(w), 24, &rng) != w) moved++;
        }
        check(moved == 0 && s32_out_q(0.0, 24, &rng) == 0, "24 bits: every 24-bit value and digital silence untouched (bit for bit)");
        int same = 1, wide = 0;
        for (int k = 0; k < 9; k++) {                  /* fixed fractions of an LSB, as music between the steps */
            const double frac = k / 8.0 + 1e-9, base = 1000.0 + frac;
            double s = 0, s2 = 0;
            const long N = 400000;
            for (long i = 0; i < N; i++) {
                const int32_t o = s32_out_q(base * 256.0 / 2147483648.0, 24, &rng);
                if (o & 0xFF) off_grid++;
                const double e = o / 256.0 - base;
                if (fabs(e) > 1.5) wide++;
                s += e; s2 += e * e;
            }
            const double mean = s / N, var = s2 / N - mean * mean;
            if (fabs(mean) > 0.005 || fabs(var - 0.25) > 0.01) { same = 0; printf("  (fraction %.3f: mean %+.4f, variance %.4f)\n", frac, mean, var); }
        }
        check(off_grid == 0 && wide == 0, "24 bits: every output on the 24-bit grid, never more than 1.5 LSB away");
        check(same, "24 bits: the error's mean 0 and variance 1/4 LSB² whatever the sample (TPDF: independent of the music)");
        check(s32_out_q(1.5, 24, &rng) == (int32_t)(0x7FFFFF00u) && s32_out_q(-1.5, 24, &rng) == INT32_MIN &&
              s32_out_q(0.999999999, 24, &rng) <= (int32_t)0x7FFFFF00, "24 bits: full scale clamped on the grid");
        check(s32_out_q(0.1234567891, 32, &rng) == s32_out(0.1234567891), "32 bits: s32_out (nothing cut downstream)");
    }

    /* 2. the regulator */
    double se, sm, off, dev;
    run(1, 80, 0, 30, &se, &sm, &off, &dev);
    check(fabs(se - 80) < 5 && fabs(dev) < 0.002, "a steady 80 ppm between the clocks: learned (integral 80 ± 5 ppm), the ring at its target");
    printf("  (integral %.1f ppm, ring %+.1f ms)\n", se, dev * 1000);
    run(1, -150, 0, 40, &se, &sm, &off, &dev);
    check(fabs(se + 150) < 5 && fabs(dev) < 0.002, "a steady −150 ppm: learned too");
    const double steps[] = { -0.3, -0.6, 0.3 };
    for (int i = 0; i < 3; i++) {
        double se0, sm0, off0, dev0;
        run(1, 60, steps[i], 40, &se, &sm, &off, &dev);
        run(0, 60, steps[i], 40, &se0, &sm0, &off0, &dev0);
        char what[160];
        snprintf(what, sizeof what, "a step of %+.0f ms (60 ppm drift): integral within 60 ± 90 ppm (max %.0f; old law %.0f), back within 20 ms", steps[i] * 1000, sm, sm0);
        check(sm < 150 && fabs(se - 60) < 10 && fabs(dev) < 0.005, what);
        printf("  (off target %.1f min, old law %.1f min; ring at the end %+.1f ms, old law %+.1f ms)\n", off, off0, dev * 1000, dev0 * 1000);
    }

    {
        double at[2] = { -1, -1 }, over[2] = { 0, 0 }, r120[2] = { 0, 0 };
        for (int law = 0; law < 2; law++) {
            double ring = 0.685, steady = 0, ppm = 0, sum = 0;
            int n = 0;
            for (long k = 0; k < 6000; k++) {
                const double t = k * 0.1;
                ring += (0 - ppm) * 1e-6 * 0.1; sum += ring; n++;
                if (n == 100) {
                    const double e = sum / n - 0.8;
                    if (law) ppm = clock_regulate(e, 10.0, &steady);
                    else { steady += 30.0 * e * 10.0; if (steady > 500) steady = 500; else if (steady < -500) steady = -500;
                           ppm = 10000.0 * e + steady; if (ppm > 600) ppm = 600; else if (ppm < -600) ppm = -600; }
                    sum = 0; n = 0;
                }
                if (at[law] < 0 && fabs(ring - 0.8) < 0.01) at[law] = t;
                if (ring - 0.8 > over[law]) over[law] = ring - 0.8;
                if (fabs(t - 120) < 0.05) r120[law] = ring;
            }
        }
        char what[200];
        snprintf(what, sizeof what, "115 ms short at the start: within 10 ms after %.0f s (old law %.0f s), overshoot %.0f ms (old %.0f)", at[1], at[0], over[1] * 1000, over[0] * 1000);
        check(at[1] > 0 && at[1] <= at[0] + 10 && over[1] < 0.005 && r120[1] >= r120[0] - 0.001, what);
        printf("  (ring at 2 min %.0f ms, old law %.0f ms)\n", r120[1] * 1000, r120[0] * 1000);
    }

    for (int law = 0; law < 2; law++) {
        int pinned = 0, dry = 0, full = 0;
        double ppm_max = 0, dev_max = 0, steady_end = 0;
        for (int ph = 0; ph < 3; ph++) {
            const double dt = 0.01, target = 3.0, cap = 524288.0 / 88200.0, hi = cap - 1.5;
            double ring = target, rbuf = 20.0 - 0.25 * ph, steady = 0, ppm = 0, sum = 0, anchor = -1, t_win = 0;
            long n = 0;
            for (long k = 0; k < (long)(3600 / dt); k++) {
                const double t = k * dt;
                ring += (1.0 + (60.0 - ppm) * 1e-6) * dt;          /* the source writes, slowed by ppm > 0 */
                rbuf -= dt;                                        /* the receiver plays */
                if (rbuf < 19.7) { double take = 20.0 - rbuf; if (take > 0.7) take = 0.7; if (take > ring) take = ring; ring -= take; rbuf += take; }
                if (ring <= 0) dry++;
                if (ring >= cap) full++;
                sum += ring; n++;
                if (t - t_win >= 10.0) {
                    const double avg = sum / n;
                    if (anchor < 0) anchor = law ? (avg < target / 4 ? target / 4 : avg > hi ? hi : avg) : (avg > target ? target : avg);
                    const double e = avg - anchor;
                    ppm = law ? clock_regulate_dead(e, t - t_win, &steady, CLOCK_NET_DEAD_S) : clock_regulate(e, t - t_win, &steady);
                    if (fabs(ppm) >= CLOCK_PPM_MAX - 1e-9) pinned++;
                    if (fabs(ppm) > ppm_max) ppm_max = fabs(ppm);
                    if (t > 60 && fabs(avg - anchor) > dev_max) dev_max = fabs(avg - anchor);
                    sum = 0; n = 0; t_win = t;
                }
            }
            steady_end += steady / 3;
        }
        char what[240];
        snprintf(what, sizeof what, "%s: a receiver in 0.7 s bursts, 60 min ×3: %d windows at ±600 ppm, largest %.0f ppm; the ring %.0f ms off its level at most",
                 law ? "the player" : "(the old way)", pinned, ppm_max, dev_max * 1000);
        if (law) check(pinned == 0 && ppm_max < 300 && dry == 0 && full == 0 && dev_max < 0.15 && fabs(steady_end - 60) < 30, what);
        else printf("  %s\n", what);
        if (law) printf("  (never dry, never full; the integral learned %.0f ppm of the 60)\n", steady_end);
    }
    run(1, 1000, 0, 30, &se, &sm, &off, &dev);
    check(fabs(se) <= CLOCK_STEADY_MAX + 1e-9, "an impossible 1000 ppm: the integral held at its limit");
    printf(fails ? "PCMCLOCK: %d WRONG\n" : "PCMCLOCK OK\n", fails);
    return fails != 0;
}
