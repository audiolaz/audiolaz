/* src/player/pcmclock.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef PCMCLOCK_H
#define PCMCLOCK_H
#include <math.h>
#include <stdint.h>

static inline double s32_in(int32_t v) { return v * (1.0 / 2147483648.0); }
static inline int32_t s32_out(double x) {
    const double v = x * 2147483648.0;
    return v >= 2147483647.0 ? 2147483647 : v <= -2147483648.0 ? INT32_MIN : (int32_t)lrint(v);
}

static inline uint64_t dither_next(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static inline int32_t s32_out_q(double x, int bits, uint64_t *rng) {
    if (bits >= 32 || bits < 8) return s32_out(x);
    const double q = (double)(1u << (32 - bits));            /* one LSB at `bits`, in 32-bit steps */
    const double u = x * (2147483648.0 / q);                  /* in LSBs */
    const double top = 2147483648.0 / q - 1.0, bot = -2147483648.0 / q;
    double y;
    if (u == floor(u)) y = u;                                 /* on the grid already: untouched (clamped beyond it) */
    else {
        const uint64_t r = dither_next(rng);
        const double d = ((double)(r >> 40) + (double)(r & 0xFFFFFF)) * (1.0 / 16777216.0) - 1.0;   /* TPDF, −1 … +1 LSB */
        y = floor(u + d + 0.5);
    }
    if (y > top) y = top; else if (y < bot) y = bot;
    return (int32_t)(y * q);
}

#define CLOCK_PPM_MAX 600.0
#define CLOCK_STEADY_MAX 300.0
#define CLOCK_I_BAND_S 0.05
#define CLOCK_KP 20000.0
#define CLOCK_KI 40.0
static inline double clock_regulate(double e, double span, double *steady) {
    if (fabs(e) < CLOCK_I_BAND_S) *steady += CLOCK_KI * e * span;
    if (*steady > CLOCK_STEADY_MAX) *steady = CLOCK_STEADY_MAX; else if (*steady < -CLOCK_STEADY_MAX) *steady = -CLOCK_STEADY_MAX;
    double ppm = CLOCK_KP * e + *steady;
    if (ppm > CLOCK_PPM_MAX) ppm = CLOCK_PPM_MAX; else if (ppm < -CLOCK_PPM_MAX) ppm = -CLOCK_PPM_MAX;
    return ppm;
}
#endif
