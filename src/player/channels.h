/* The speakers' channels: left and right swapped, each speaker's polarity inverted, the last stage before the
 * output (roomd.c's writer, every kind of output: a card, a network receiver, the direct output), on the 32-bit words
 * it hands over. The output is always one stereo pair (the ring's frames, two words each): these act on it alone.
 *   - What "left" means: the speaker that plays the music's left channel. Each speaker's inversion is applied to its
 *     channel, then the swap sends that channel to the other side of the output: inverting "the left speaker" inverts
 *     the music's left wherever it goes. So everything before this stage (and what was measured through it) keeps its
 *     left and right: a sweep for the left speaker still reaches the speaker that plays the left channel.
 *   - Exact: the swap moves the words as they are; an inversion is the exact negation on the output's grid (its bits:
 *     the music already requantized to them), −x, the one value that has no opposite (the most negative) given the
 *     largest positive on that grid. Nothing resampled, nothing rounded: lossless, but no longer the source channel for
 *     channel (bitperfect.h's "swap", "polarity").
 *   - DSD (native, or DoP: 16 bits of DSD in bits 23–8, the marker byte above added after this): the swap moves the
 *     words; an inversion flips the DSD bits, never a DoP marker (DSD's polarity: every bit the other way).
 *   - A change while it plays: in PCM a raised-cosine crossfade from the old setting to the new over CH_FADE_MS, so a
 *     speaker's polarity turning goes through a dip, not a step (no click); in DSD at once, at a chunk's start.
 *   - Off: nothing read, nothing written (chan_run returns at once): the output byte for byte as before.
 * Header-only, its own test (engine/alsa/test_channels.c). */
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
/* One frame through setting s (exact). */
static inline void chan_frame(int s, int kind, int32_t top, int32_t *l, int32_t *r) {
    int32_t a = *l, b = *r;
    if (kind == CH_PCM) { if (s & CH_INV_L) a = chan_neg(a, top); if (s & CH_INV_R) b = chan_neg(b, top); }
    else {
        const uint32_t m = kind == CH_DOP ? CH_DOP_DATA : 0xFFFFFFFFu;
        if (s & CH_INV_L) a = (int32_t)((uint32_t)a ^ m);
        if (s & CH_INV_R) b = (int32_t)((uint32_t)b ^ m);
    }
    if (s & CH_SWAP) { *l = b; *r = a; } else { *l = a; *r = b; }
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
#endif
