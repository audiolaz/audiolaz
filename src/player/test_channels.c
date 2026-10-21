/* Checks channels.h: left and right swapped, each speaker's polarity inverted, exactly, in each kind of word the
 * output takes (PCM on 16, 24 and 32 bits, DoP, native DSD), the most negative value given the grid's largest, off
 * touching nothing, and a change while it plays crossfaded in PCM (no step), at once in DSD.
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
            for (unsigned long i = 0; i < N && ok; i++) {
                int32_t l = x[2 * i], r = x[2 * i + 1];
                if (s & CH_INV_L) l = l == INT32_MIN ? top : -l;
                if (s & CH_INV_R) r = r == INT32_MIN ? top : -r;
                const int32_t el = s & CH_SWAP ? r : l, er = s & CH_SWAP ? l : r;
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
                uint32_t l = (uint32_t)x[2 * i], r = (uint32_t)x[2 * i + 1];
                if (s & CH_INV_L) l ^= m;
                if (s & CH_INV_R) r ^= m;
                ok = (uint32_t)y[2 * i] == (s & CH_SWAP ? r : l) && (uint32_t)y[2 * i + 1] == (s & CH_SWAP ? l : r);
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
    free(x); free(y);
    printf("%s\n", fails ? "CHANNELS WRONG" : "CHANNELS OK");
    return fails ? 1 : 0;
}
