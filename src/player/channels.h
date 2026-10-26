/* The speakers' channels: left and right swapped, each speaker's polarity inverted. Two stages, each where it belongs
 * (22/10: the swap had been the output's last stage, so with it on, the left speaker played the right speaker's
 * processing: each speaker's own, measured for it, must stay with it):
 *   - The swap acts on the MUSIC, as soon as it is read (roomd.c's play thread: chan_swap_d on its doubles, chan_run
 *     on the free player's words), before everything that belongs to one speaker; after it, each output channel (each
 *     speaker) keeps its own processing. What the player sends to one speaker itself (a measurement's sweeps, its test
 *     tones, a sync click) is never swapped: the play thread does not swap while one plays.
 *   - The polarity is the OUTPUT's, per output channel: invert_l the speaker on the output's left channel, invert_r
 *     the one on its right. The last stage (roomd.c's writer, every kind of output: a card, a network receiver, the
 *     direct output), on the 32-bit words it hands over, everything sent to the output (the music, a measurement,
 *     a test tone). The output is always one stereo pair (the ring's frames, two words each).
 *   - Exact: the swap moves the words (or the doubles) as they are; an inversion is the exact negation on the output's
 *     grid (its bits: the music already requantized to them), −x, the one value that has no opposite (the most
 *     negative) given the largest positive on that grid. Nothing resampled, nothing rounded: lossless, but no longer
 *     the source channel for channel (bitperfect.h's "swap", "polarity").
 *   - DSD (native, or DoP: 16 bits of DSD in bits 23–8, the marker byte above added after this): an inversion flips the
 *     DSD bits, never a DoP marker (DSD's polarity: every bit the other way). The swap never sees DSD: it is made from
 *     the music after the swap.
 *   - A change while it plays: in PCM a raised-cosine crossfade from the old setting to the new over CH_FADE_MS, so a
 *     speaker's polarity turning goes through a dip, not a step (no click), and the swap through a blend; in DSD at
 *     once, at a chunk's start.
 *   - Off: nothing read, nothing written (chan_run and chan_swap_d return at once): byte for byte as before.
 * One setting (chan_frame, chan_run) is: the swap first, then each output word's inversion. Header-only, its own test
 * (engine/alsa/test_channels.c). */
#ifndef AUDIOLAZ_CHANNELS_H
#define AUDIOLAZ_CHANNELS_H
#include <math.h>
#include <stdint.h>

enum { CH_SWAP = 1, CH_INV_L = 2, CH_INV_R = 4, CH_ALL = 7 };
enum { CH_PCM, CH_DOP, CH_DSD };        /* the words: PCM (on a grid of bits), DoP, native DSD */
#define CH_FADE_MS 5.0
#define CH_DOP_DATA 0x00FFFF00u         /* DoP's DSD bits (its marker in bits 31–24, added by the writer after this) */

typedef struct { int now, from; long k, len; } chan_stage;   /* the setting applied; during a change: from, k of len */

/* The largest positive on a grid of `bits` (24: 0x7FFFFF00), the most negative's opposite there. */
static inline int32_t chan_top(int bits) {
    if (bits <= 0 || bits >= 32) return INT32_MAX;
    return (int32_t)(0x7FFFFFFFu & ~((1u << (32 - bits)) - 1u));
}
static inline int32_t chan_neg(int32_t x, int32_t top) { return x == INT32_MIN ? top : -x; }
/* One frame through setting s (exact): the swap first, then the inversion of each output word (CH_INV_L the left
 * one, wherever it came from). */
static inline void chan_frame(int s, int kind, int32_t top, int32_t *l, int32_t *r) {
    int32_t a = *l, b = *r;
    if (s & CH_SWAP) { const int32_t t = a; a = b; b = t; }
    if (kind == CH_PCM) { if (s & CH_INV_L) a = chan_neg(a, top); if (s & CH_INV_R) b = chan_neg(b, top); }
    else {
        const uint32_t m = kind == CH_DOP ? CH_DOP_DATA : 0xFFFFFFFFu;
        if (s & CH_INV_L) a = (int32_t)((uint32_t)a ^ m);
        if (s & CH_INV_R) b = (int32_t)((uint32_t)b ^ m);
    }
    *l = a; *r = b;
}
/* A word of the crossfade, back on the grid (its step: 2^(32 − bits)), within the words' range. */
static inline int32_t chan_grid(double v, int bits) {
    const double step = bits <= 0 || bits >= 32 ? 1.0 : (double)(1u << (32 - bits));
    v = nearbyint(v / step) * step;
    const double top = (double)chan_top(bits);
    return v >= top ? (int32_t)top : v <= -2147483648.0 ? INT32_MIN : (int32_t)v;
}
/* The stage at the start of a play: setting s at once. */
static inline void chan_init(chan_stage *st, int s) { st->now = st->from = s & CH_ALL; st->k = st->len = 0; }
/* n frames (stereo words, interleaved) as they go to the output, setting `want` asked; bits: the PCM's grid; rate: the
 * output's frames a second (the crossfade's length). Returns 1 when it touched them. */
static inline int chan_run(chan_stage *st, int want, int32_t *pcm, unsigned long n, int kind, int bits, unsigned rate) {
    want &= CH_ALL;
    if (!want && !st->now && st->k >= st->len) return 0;
    if (want != st->now) {
        if (kind == CH_PCM && rate) { st->from = st->now; st->k = 0; st->len = (long)(rate * CH_FADE_MS / 1000.0); if (st->len < 1) st->len = 1; }
        else st->k = st->len = 0;
        st->now = want;
    }
    const int32_t top = chan_top(bits);
    unsigned long i = 0;
    for (; i < n && st->k < st->len; i++, st->k++) {     /* the change: from the old setting to the new, raised cosine */
        const double w = 0.5 - 0.5 * cos(M_PI * (double)(st->k + 1) / (double)(st->len + 1));
        int32_t a0 = pcm[2 * i], b0 = pcm[2 * i + 1], a1 = a0, b1 = b0;
        chan_frame(st->from, kind, top, &a0, &b0);
        chan_frame(st->now, kind, top, &a1, &b1);
        pcm[2 * i] = chan_grid((1 - w) * a0 + w * a1, bits);
        pcm[2 * i + 1] = chan_grid((1 - w) * b0 + w * b1, bits);
    }
    if (st->now) for (; i < n; i++) chan_frame(st->now, kind, top, &pcm[2 * i], &pcm[2 * i + 1]);
    return 1;
}
/* 1 while the swap of `st` changes (asked other than it plays, or its crossfade going on): the frames of then are a
 * blend of both sides, neither the music's channels nor their exchange. */
static inline int chan_moving(const chan_stage *st, int want) {
    return (want & CH_SWAP) != (st->now & CH_SWAP) || st->k < st->len;
}
/* chan_swap_d's own: frame i of p blended (s of the other side), and frames i… of p exchanged. */
static inline void chan_swap_d1(double *p, unsigned long i, double s) {
    const double l = p[2 * i], r = p[2 * i + 1];
    p[2 * i] = (1.0 - s) * l + s * r;
    p[2 * i + 1] = (1.0 - s) * r + s * l;
}
static inline void chan_swap_x(double *p, unsigned long i, unsigned long n) {
    for (; i < n; i++) { const double t = p[2 * i]; p[2 * i] = p[2 * i + 1]; p[2 * i + 1] = t; }
}
/* The swap on the music in doubles (roomd.c's play thread, as soon as it is read): n stereo frames of x, and of y and z
 * when not NULL (a source's surround pair and its widened copy, the same exchange), `want` asked (its CH_SWAP only);
 * rate: the music's frames a second (the crossfade's length). Off and settled: nothing read, nothing written (0).
 * A change: a raised-cosine blend over CH_FADE_MS (each side from its own channel to the other's: L' = (1 − s)·L +
 * s·R, R' = (1 − s)·R + s·L), then the two exchanged exactly. Returns 1 when it touched them. */
static inline int chan_swap_d(chan_stage *st, int want, double *x, double *y, double *z, unsigned long n, unsigned rate) {
    want &= CH_SWAP;
    if (!want && !(st->now & CH_SWAP) && st->k >= st->len) return 0;
    if (want != (st->now & CH_SWAP)) {
        st->from = st->now & CH_SWAP; st->k = 0;
        st->len = (long)(rate * CH_FADE_MS / 1000.0); if (st->len < 1) st->len = 1;
        st->now = want;
    }
    unsigned long i = 0;
    for (; i < n && st->k < st->len; i++, st->k++) {
        const double w = 0.5 - 0.5 * cos(M_PI * (double)(st->k + 1) / (double)(st->len + 1));
        const double s = (1.0 - w) * (st->from ? 1.0 : 0.0) + w * (st->now ? 1.0 : 0.0);
        chan_swap_d1(x, i, s);
        if (y) chan_swap_d1(y, i, s);
        if (z) chan_swap_d1(z, i, s);
    }
    if (st->now) { chan_swap_x(x, i, n); if (y) chan_swap_x(y, i, n); if (z) chan_swap_x(z, i, n); }
    return 1;
}
#endif
