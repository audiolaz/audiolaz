/* src/player/azsrc.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_AZSRC_H
#define AUDIOLAZ_AZSRC_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define AZSRC_MAX_L 2560                   /* 44.1 ↔ 768 kHz: 2560/147 */
#define AZSRC_ATT_DB 150.0
typedef struct {
    unsigned in, out, L, M, T;             /* rates; the ratio reduced; taps per phase (even) */
    double *h;                             /* L phases of T taps: h[p * T + k] */
    double *x;                             /* the last T input frames, stereo, twice over (no wrap in the dot product) */
    unsigned xp;                           /* where the next input frame goes (0 … T−1) */
    uint64_t n_in, n_out;                  /* frames taken, made */
} azsrc;
static inline unsigned azsrc_gcd(unsigned a, unsigned b) { while (b) { const unsigned t = a % b; a = b; b = t; } return a; }
static inline double azsrc_i0(double x) { double s = 1, t = 1; for (int k = 1; k < 200; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < s * 1e-17) break; } return s; }
static inline void azsrc_free(azsrc *s) { if (!s) return; free(s->h); free(s->x); free(s); }
static inline azsrc *azsrc_new(unsigned in, unsigned out) {
    if (!in || !out || in == out) return NULL;
    const unsigned g = azsrc_gcd(in, out), L = out / g, M = in / g;
    if (L > AZSRC_MAX_L || M > AZSRC_MAX_L) return NULL;
    azsrc *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->in = in; s->out = out; s->L = L; s->M = M;
    /* the prototype at in·L: cutoff at the lower rate's Nyquist; its length for the attenuation over the transition */
    const double lo = in < out ? in : out, up = (double)in * L;
    const double fp = 0.4535 * lo, fs = lo - fp, dw = 2 * M_PI * (fs - fp) / up;
    const double beta = 0.1102 * (AZSRC_ATT_DB - 8.7);
    unsigned T = (unsigned)ceil((AZSRC_ATT_DB - 8.0) / (2.285 * dw) / L) + 1;
    T += T & 1;
    s->T = T;
    const unsigned N = L * T;
    s->h = malloc(sizeof(double) * N);
    s->x = calloc(4 * (size_t)T, sizeof(double));
    if (!s->h || !s->x) { azsrc_free(s); return NULL; }
    const double fc = 0.5 * lo / up, c = (N - 1) / 2.0, i0b = azsrc_i0(beta);
    for (unsigned m = 0; m < N; m++) {
        const double t = m - c, u = t / c;
        const double sinc = fabs(t) < 1e-12 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
        const double w = fabs(u) >= 1 ? 0 : azsrc_i0(beta * sqrt(1 - u * u)) / i0b;
        /* polyphase: phase p = m mod L, tap k = m / L; gain L (the zeros stuffed between the input samples) */
        s->h[(m % L) * T + m / L] = sinc * w * L;
    }
    /* each phase's sum exactly 1 (DC through unchanged, whichever phase) */
    for (unsigned p = 0; p < L; p++) { double sum = 0; for (unsigned k = 0; k < T; k++) sum += s->h[p * T + k]; if (sum != 0) for (unsigned k = 0; k < T; k++) s->h[p * T + k] /= sum; }
    return s;
}
/* the delay it adds, in input frames (half the prototype) */
static inline double azsrc_delay_in(const azsrc *s) { return s ? (s->L * (double)s->T - 1) / 2.0 / s->L : 0; }
/* frames in (S32 stereo) to frames out: as many out as cap allows; *used: the input frames taken */
static inline long azsrc_run(azsrc *s, const int32_t *in, long n_in, int32_t *out, long cap, long *used) {
    long o = 0, u = 0;
    const unsigned T = s->T, L = s->L;
    const double k_in = 1.0 / 2147483648.0;
    for (;;) {
        /* output n reads input frames up to floor(n·M / L): taken first, one at a time */
        const uint64_t need = (s->n_out * s->M) / L + 1;      /* input frames needed in all */
        while (s->n_in < need) {
            if (u >= n_in) goto done;
            const double l = in[2 * u] * k_in, r = in[2 * u + 1] * k_in;
            /* twice over: x[xp] and x[xp + T] (stereo: 2 doubles a frame), so that the T latest are always contiguous */
            s->x[2 * s->xp] = l; s->x[2 * s->xp + 1] = r;
            s->x[2 * (s->xp + T)] = l; s->x[2 * (s->xp + T) + 1] = r;
            s->xp = s->xp + 1 == T ? 0 : s->xp + 1;
            s->n_in++; u++;
        }
        if (o >= cap) break;
        const unsigned p = (unsigned)((s->n_out * s->M) % L);
        const double *h = s->h + (size_t)p * T;
        /* the newest frame is the one just before xp: taps k = 0 … T−1 from the newest back */
        const double *x = s->x + 2 * ((size_t)s->xp + T - 1);
        double a = 0, b = 0;
        for (unsigned k = 0; k < T; k++) { a += h[k] * x[-2 * (long)k]; b += h[k] * x[-2 * (long)k + 1]; }
        a *= 2147483648.0; b *= 2147483648.0;
        out[2 * o] = a >= 2147483647.0 ? INT32_MAX : a <= -2147483648.0 ? INT32_MIN : (int32_t)lrint(a);
        out[2 * o + 1] = b >= 2147483647.0 ? INT32_MAX : b <= -2147483648.0 ? INT32_MIN : (int32_t)lrint(b);
        o++; s->n_out++;
    }
done:
    if (used) *used = u;
    return o;
}
#endif
