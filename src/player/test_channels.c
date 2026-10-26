/* Checks channels.h: left and right swapped, each output channel's polarity inverted, exactly, in each kind of word the
 * output takes (PCM on 16, 24 and 32 bits, DoP, native DSD), the most negative value given the grid's largest, off
 * touching nothing, and a change while it plays crossfaded in PCM (no step), at once in DSD. One setting is the
 * swap first, then each output word's inversion (invert_l: the output's left word, whatever it carries); the swap on
 * the music in doubles (chan_swap_d: the play thread's, before the correction), with its surround pair, exact, off
 * untouched, a change crossfaded; chan_moving while it changes; the free player's words (32 bits) exchanged exactly.
 *   gcc -std=gnu11 -O2 -Wall -Wextra -o test_channels engine/alsa/test_channels.c -lm && ./test_channels */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "channels.h"

static int fails = 0;
static void check(int ok, const char *what) { printf("%-80s %s\n", what, ok ? "OK" : "WRONG"); if (!ok) fails++; }

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
/* n frames of noise on the grid of `bits`, the extremes among them */
static void fill(int32_t *p, unsigned long n, int bits) {
    const uint32_t mask = bits >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - bits)) - 1u);
    for (unsigned long i = 0; i < 2 * n; i++) p[i] = (int32_t)(rnd() & mask);
    p[0] = INT32_MIN; p[3] = INT32_MIN; p[4] = chan_top(bits); p[5] = 0;
}

int main(void) {
    enum { N = 4096 };
    int32_t *x = malloc(sizeof(int32_t) * 2 * N), *y = malloc(sizeof(int32_t) * 2 * N);
    chan_stage st;

    /* 1. off: nothing touched, nothing read */
    fill(x, N, 24); memcpy(y, x, sizeof(int32_t) * 2 * N);
    chan_init(&st, 0);
    check(chan_run(&st, 0, y, N, CH_PCM, 24, 44100) == 0 && !memcmp(x, y, sizeof(int32_t) * 2 * N), "off: the words as they came, byte for byte");

    /* 2. each setting, each grid, exact */
    const int grids[] = { 16, 24, 32 };
    for (int g = 0; g < 3; g++) {
        const int bits = grids[g];
        const int32_t top = chan_top(bits);
        for (int s = 1; s <= CH_ALL; s++) {
            fill(x, N, bits); memcpy(y, x, sizeof(int32_t) * 2 * N);
            chan_init(&st, s);
            chan_run(&st, s, y, N, CH_PCM, bits, 44100);
            int ok = 1;
            for (unsigned long i = 0; i < N && ok; i++) {   /* the output's left: the swapped word, then its inversion */
                int32_t el = s & CH_SWAP ? x[2 * i + 1] : x[2 * i], er = s & CH_SWAP ? x[2 * i] : x[2 * i + 1];
                if (s & CH_INV_L) el = el == INT32_MIN ? top : -el;
                if (s & CH_INV_R) er = er == INT32_MIN ? top : -er;
                ok = y[2 * i] == el && y[2 * i + 1] == er;
            }
            /* every word still on the grid */
            const uint32_t low = bits >= 32 ? 0 : (1u << (32 - bits)) - 1u;
            for (unsigned long i = 0; i < 2 * N && ok; i++) ok = ((uint32_t)y[i] & low) == 0;
            char what[120];
            snprintf(what, sizeof what, "PCM %d bits, %s%s%s: exact, on its grid", bits, s & CH_SWAP ? "swap " : "", s & CH_INV_L ? "invert left " : "", s & CH_INV_R ? "invert right" : "");
            check(ok, what);
        }
    }
    /* the most negative: the grid's largest; inverted twice, each other value comes back */
    check(chan_top(24) == (int32_t)0x7FFFFF00 && chan_top(16) == (int32_t)0x7FFF0000 && chan_top(32) == INT32_MAX, "the grid's largest: 24 bits 0x7FFFFF00, 16 bits 0x7FFF0000, 32 bits 0x7FFFFFFF");
    {
        int32_t l = INT32_MIN, r = INT32_MIN;
        chan_frame(CH_INV_L | CH_INV_R, CH_PCM, chan_top(24), &l, &r);
        check(l == (int32_t)0x7FFFFF00 && r == (int32_t)0x7FFFFF00, "24 bits: −full scale inverted is +full scale (0x7FFFFF00), no wrap");
        fill(x, N, 24); memcpy(y, x, sizeof(int32_t) * 2 * N);
        chan_init(&st, CH_INV_L | CH_INV_R);
        chan_run(&st, CH_INV_L | CH_INV_R, y, N, CH_PCM, 24, 44100); chan_run(&st, CH_INV_L | CH_INV_R, y, N, CH_PCM, 24, 44100);
        int ok = 1;
        for (unsigned long i = 0; i < 2 * N; i++) if (x[i] != INT32_MIN && y[i] != x[i]) ok = 0;
        check(ok, "inverted twice: every value back but the most negative (the grid's largest's opposite)");
    }

    /* 3. DSD: native, every bit; DoP, its DSD bits only, never the marker byte nor the bits under */
    for (int kind = CH_DOP; kind <= CH_DSD; kind++)
        for (int s = 1; s <= CH_ALL; s++) {
            for (unsigned long i = 0; i < 2 * N; i++) x[i] = (int32_t)rnd();
            memcpy(y, x, sizeof(int32_t) * 2 * N);
            chan_init(&st, 0);
            chan_run(&st, s, y, N, kind, 32, 352800);   /* asked while playing: at once, no crossfade in DSD */
            const uint32_t m = kind == CH_DOP ? 0x00FFFF00u : 0xFFFFFFFFu;
            int ok = 1;
            for (unsigned long i = 0; i < N && ok; i++) {
                uint32_t l = (uint32_t)(s & CH_SWAP ? x[2 * i + 1] : x[2 * i]), r = (uint32_t)(s & CH_SWAP ? x[2 * i] : x[2 * i + 1]);
                if (s & CH_INV_L) l ^= m;
                if (s & CH_INV_R) r ^= m;
                ok = (uint32_t)y[2 * i] == l && (uint32_t)y[2 * i + 1] == r;
            }
            char what[120];
            snprintf(what, sizeof what, "%s, %s%s%s: at once, exact", kind == CH_DOP ? "DoP (bits 23-8 only)" : "native DSD (every bit)", s & CH_SWAP ? "swap " : "", s & CH_INV_L ? "invert left " : "", s & CH_INV_R ? "invert right" : "");
            check(ok, what);
        }
    {   /* DSD's silence (0x69) inverted is 0x96: still DSD's idle pattern, the same density */
        int32_t l = (int32_t)0x69696969, r = (int32_t)0x00696900;
        chan_frame(CH_INV_L, CH_DSD, 0, &l, &r);
        int32_t a = (int32_t)0x05696900, b = (int32_t)0xFA696900;
        chan_frame(CH_INV_L | CH_INV_R, CH_DOP, 0, &a, &b);
        check((uint32_t)l == 0x96969696u && (uint32_t)a == 0x05969600u && (uint32_t)b == 0xFA969600u, "DSD silence 0x69 → 0x96; DoP's markers 0x05 / 0xFA kept");
    }

    /* 4. a change while it plays (PCM): a crossfade of CH_FADE_MS, no step, then exact */
    {
        const unsigned rate = 48000;
        const long F = (long)(rate * CH_FADE_MS / 1000.0);
        /* a sine on both channels in phase, 1 kHz at −6 dBFS, 24 bits */
        for (unsigned long i = 0; i < N; i++) { const int32_t v = chan_grid(0.5 * 2147483647.0 * sin(2 * M_PI * 1000.0 * i / rate), 24); x[2 * i] = v; x[2 * i + 1] = v; }
        memcpy(y, x, sizeof(int32_t) * 2 * N);
        chan_init(&st, 0);
        chan_run(&st, 0, y, 512, CH_PCM, 24, rate);                 /* off: as it came */
        chan_run(&st, CH_INV_L, y + 2 * 512, 1000, CH_PCM, 24, rate);   /* the left speaker inverted, while it plays */
        chan_run(&st, CH_INV_L, y + 2 * 1512, N - 1512, CH_PCM, 24, rate);
        double jump = 0, base = 0;
        for (unsigned long i = 1; i < N; i++) {
            const double d = fabs((double)y[2 * i] - (double)y[2 * i - 2]), d0 = fabs((double)x[2 * i] - (double)x[2 * i - 2]);
            if (d > jump) jump = d;
            if (d0 > base) base = d0;
        }
        int exact = 1, right = 1, before = 1;
        for (unsigned long i = 0; i < 512; i++) before &= y[2 * i] == x[2 * i];
        for (unsigned long i = 512 + (unsigned long)F; i < N; i++) exact &= y[2 * i] == (x[2 * i] == INT32_MIN ? chan_top(24) : -x[2 * i]);
        for (unsigned long i = 0; i < N; i++) right &= y[2 * i + 1] == x[2 * i + 1];
        char what[160];
        snprintf(what, sizeof what, "a change while it plays: %ld frames of crossfade, its largest step %.2f × the music's own", F, jump / base);
        check(before && exact && right && jump <= base * 1.25, what);
        /* the swap the same way: the two channels' sum kept through it (a crossfade between L and R) */
        for (unsigned long i = 0; i < N; i++) { x[2 * i] = chan_grid(0.4 * 2147483647.0 * sin(2 * M_PI * 440.0 * i / rate), 24); x[2 * i + 1] = chan_grid(0.3 * 2147483647.0 * sin(2 * M_PI * 660.0 * i / rate), 24); }
        memcpy(y, x, sizeof(int32_t) * 2 * N);
        chan_init(&st, 0);
        chan_run(&st, CH_SWAP, y, N, CH_PCM, 24, rate);
        int sum = 1, sw = 1;
        for (unsigned long i = 0; i < N; i++) {
            if (llabs((long long)y[2 * i] + y[2 * i + 1] - x[2 * i] - x[2 * i + 1]) > 512) sum = 0;
            if (i >= (unsigned long)F && (y[2 * i] != x[2 * i + 1] || y[2 * i + 1] != x[2 * i])) sw = 0;
        }
        check(sum && sw, "the swap while it plays: crossfaded (the sum of both kept), then the words exchanged exactly");
    }
    {   /* one frame, both: the output's left word inverted is the music's right (swapped), never its left */
        int32_t l = 1000 << 8, r = -(7 << 8);
        chan_frame(CH_SWAP | CH_INV_L, CH_PCM, chan_top(24), &l, &r);
        check(l == 7 << 8 && r == 1000 << 8, "swap and invert left: the output's left is the music's right, negated; its right the music's left as it is");
    }

    /* 5. the swap on the music in doubles (roomd.c's play thread, before the correction) */
    {
        const unsigned rate = 48000;
        const long F = (long)(rate * CH_FADE_MS / 1000.0);
        double *a = malloc(sizeof(double) * 2 * N), *b = malloc(sizeof(double) * 2 * N), *c = malloc(sizeof(double) * 2 * N);
        double *a0 = malloc(sizeof(double) * 2 * N), *b0 = malloc(sizeof(double) * 2 * N), *c0 = malloc(sizeof(double) * 2 * N);
        for (unsigned long i = 0; i < 2 * N; i++) { a0[i] = (int32_t)rnd() / 2147483648.0; b0[i] = (int32_t)rnd() / 4294967296.0; c0[i] = (int32_t)rnd() / 8589934592.0; }
        memcpy(a, a0, sizeof(double) * 2 * N); memcpy(b, b0, sizeof(double) * 2 * N); memcpy(c, c0, sizeof(double) * 2 * N);
        /* off: nothing read, nothing written, byte for byte */
        chan_init(&st, 0);
        const int touched = chan_swap_d(&st, 0, a, b, c, N, rate) | chan_swap_d(&st, CH_INV_L | CH_INV_R, a, b, c, N, rate);
        check(!touched && !memcmp(a, a0, sizeof(double) * 2 * N) && !memcmp(b, b0, sizeof(double) * 2 * N) && !memcmp(c, c0, sizeof(double) * 2 * N),
              "doubles, off (the polarity asked: not this stage's): untouched, byte for byte");
        /* on from the start: exchanged exactly, the surround pair and its widened copy with it */
        chan_init(&st, CH_SWAP);
        chan_swap_d(&st, CH_SWAP, a, b, NULL, 1000, rate); chan_swap_d(&st, CH_SWAP | CH_INV_R, a + 2000, b + 2000, NULL, N - 1000, rate);
        chan_swap_d(&st, CH_SWAP, c, NULL, NULL, N, rate);
        int ex = 1;
        for (unsigned long i = 0; i < N; i++)
            ex &= a[2 * i] == a0[2 * i + 1] && a[2 * i + 1] == a0[2 * i] && b[2 * i] == b0[2 * i + 1] && b[2 * i + 1] == b0[2 * i] && c[2 * i] == c0[2 * i + 1] && c[2 * i + 1] == c0[2 * i];
        check(ex, "doubles, swap on from the play's start: the music, its surround pair and its widened copy exchanged exactly");
        /* turned on while it plays: a crossfade (the sum of both sides kept, no step), then exact; chan_moving meanwhile */
        for (unsigned long i = 0; i < N; i++) { a0[2 * i] = 0.4 * sin(2 * M_PI * 440.0 * i / rate); a0[2 * i + 1] = 0.3 * sin(2 * M_PI * 660.0 * i / rate); }
        memcpy(a, a0, sizeof(double) * 2 * N); memcpy(b, a0, sizeof(double) * 2 * N);
        chan_init(&st, 0);
        int mv = !chan_moving(&st, 0) && chan_moving(&st, CH_SWAP);
        chan_swap_d(&st, 0, a, b, NULL, 512, rate);
        unsigned long at = 512, moving = 0;
        while (at < N) {                               /* in blocks of 100, as the play thread's */
            const unsigned long k = N - at < 100 ? N - at : 100;
            if (chan_moving(&st, CH_SWAP)) moving += k;
            chan_swap_d(&st, CH_SWAP, a + 2 * at, b + 2 * at, NULL, k, rate);
            at += k;
        }
        mv &= !chan_moving(&st, CH_SWAP) && chan_moving(&st, 0) && moving == (unsigned long)((F + 99) / 100 * 100);
        double jump = 0, base = 0;
        int sum = 1, after = 1, before = 1;
        for (unsigned long i = 1; i < N; i++)
            for (int ch = 0; ch < 2; ch++) {
                const double d = fabs(a[2 * i + ch] - a[2 * i - 2 + ch]), d0 = fabs(a0[2 * i + ch] - a0[2 * i - 2 + ch]), d1 = fabs(a0[2 * i + 1 - ch] - a0[2 * i - 1 - ch]);
                if (d > jump) jump = d;
                if (d0 > base) base = d0;
                if (d1 > base) base = d1;
            }
        for (unsigned long i = 0; i < N; i++) {
            if (fabs(a[2 * i] + a[2 * i + 1] - a0[2 * i] - a0[2 * i + 1]) > 1e-12) sum = 0;
            if (i < 512) before &= a[2 * i] == a0[2 * i] && a[2 * i + 1] == a0[2 * i + 1];
            if (i >= 512 + (unsigned long)F) after &= a[2 * i] == a0[2 * i + 1] && a[2 * i + 1] == a0[2 * i];
            if (a[2 * i] != b[2 * i] || a[2 * i + 1] != b[2 * i + 1]) sum = 0;   /* the surround pair: the same weights */
        }
        char what[200];
        snprintf(what, sizeof what, "doubles, swap turned on while it plays: %ld frames of crossfade (the sum kept), its largest step %.2f × the music's own, then exact", F, jump / base);
        check(before && sum && after && jump <= base * 1.25, what);
        check(mv, "chan_moving: true exactly while the swap changes (asked, then its crossfade), false once settled");
        /* off again while it plays: back to the music's own channels exactly */
        memcpy(a, a0, sizeof(double) * 2 * N);
        chan_init(&st, CH_SWAP);
        chan_swap_d(&st, CH_SWAP, a, NULL, NULL, 512, rate);
        chan_swap_d(&st, 0, a + 1024, NULL, NULL, N - 512, rate);
        int back = 1;
        for (unsigned long i = 512 + (unsigned long)F; i < N; i++) back &= a[2 * i] == a0[2 * i] && a[2 * i + 1] == a0[2 * i + 1];
        check(back && chan_swap_d(&st, 0, a, NULL, NULL, 1, rate) == 0, "doubles, swap turned off while it plays: the music's own channels again, exactly; then nothing touched");
        free(a); free(b); free(c); free(a0); free(b0); free(c0);
    }
    /* 6. the free player's words (its swap, at the music's: chan_run on 32 bits, the swap alone): only moved */
    {
        for (unsigned long i = 0; i < 2 * N; i++) x[i] = (int32_t)rnd();
        x[0] = INT32_MIN; x[3] = INT32_MAX;
        memcpy(y, x, sizeof(int32_t) * 2 * N);
        chan_init(&st, CH_SWAP);
        chan_run(&st, CH_SWAP, y, N, CH_PCM, 32, 44100);
        int ok = 1;
        for (unsigned long i = 0; i < N; i++) ok &= y[2 * i] == x[2 * i + 1] && y[2 * i + 1] == x[2 * i];
        check(ok, "the free player's swap (32-bit words): every word moved, none changed");
    }
    free(x); free(y);
    printf("%s\n", fails ? "CHANNELS WRONG" : "CHANNELS OK");
    return fails ? 1 : 0;
}
