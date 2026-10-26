/* src/player/bitperfect.h : its cost, and that it sees a change. One second of 192 kHz stereo through bp_in and
 * bp_out (the play's thread and the writer's work), 200 times: the share of a core it takes; then the same frames with
 * one sample changed by one LSB, one frame dropped, one moved: each seen (a span different), and intact frames all equal.
 * The swap on the music (bp_in_as): a source fingerprinted swapped matches an output swapped, never one as it
 * came; the spans of a change (its crossfade) skipped, never different, the ones after compared again.
 *   cc -O2 -o /tmp/bp test/bitperfect.c && /tmp/bp */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../src/player/bitperfect.h"
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int run(int32_t *x, long n, int what) {   /* 0 intact, 1 a sample changed, 2 a frame dropped, 3 two frames swapped */
    bp_start(1); bp_base(0);
    static int32_t y[2 * 400000];
    memcpy(y, x, sizeof(int32_t) * 2 * (size_t)n);
    if (what == 1) y[2 * 50000] ^= 1;
    if (what == 3) { int32_t t0 = y[2 * 70000], t1 = y[2 * 70000 + 1]; y[2 * 70000] = y[2 * 70001]; y[2 * 70000 + 1] = y[2 * 70001 + 1]; y[2 * 70001] = t0; y[2 * 70001 + 1] = t1; }
    for (long i = 0; i < n; i += 1000) bp_in(x + 2 * i, n - i < 1000 ? n - i : 1000, 2);
    long m = n;
    if (what == 2) { memmove(y + 2 * 60000, y + 2 * 60001, sizeof(int32_t) * 2 * (size_t)(n - 60001)); m = n - 1; }
    for (long i = 0; i < m; i += 689) bp_out((uint64_t)i, y + 2 * i, (unsigned long)(m - i < 689 ? m - i : 689));
    return (int)atomic_load(&bp.bad);
}
int main(void) {
    const long R = 192000;
    int32_t *x = malloc(sizeof(int32_t) * 2 * 400000);
    srand(1);
    for (long i = 0; i < 2 * 400000; i++) x[i] = (int32_t)((uint32_t)rand() << 8);
    const double t0 = now();
    for (int k = 0; k < 200; k++) { bp_start(1); bp_base(0); bp_in(x, R, 2); for (long i = 0; i < R; i += 689) bp_out((uint64_t)i, x + 2 * i, (unsigned long)(R - i < 689 ? R - i : 689)); }
    const double per_s = (now() - t0) / 200;
    printf("bit-perfect check: %.3f ms per second of 192 kHz stereo (%.3f %% of a core)\n", per_s * 1000, per_s * 100);
    const int a = run(x, 400000, 0), b = run(x, 400000, 1), c = run(x, 400000, 2), d = run(x, 400000, 3);
    printf("intact: %d spans different (of %ld equal); a sample ±1 LSB: %d; a frame dropped: %d; two frames swapped: %d\n", a, atomic_load(&bp.ok), b, c, d);
    /* the speakers' channels (channels.h, after the comparison): not bit-perfect, lossless; another reason: not */
    char why[200];
    run(x, 400000, 0);
    const int st0 = bp_state(why, sizeof why), l0 = bp_lossless();
    bp_say(BP_SWAP | BP_POLARITY);
    const int st1 = bp_state(why, sizeof why), l1 = bp_lossless();
    const int named = !strcmp(why, "swap,polarity");
    bp_say(BP_VOLUME);
    const int l2 = bp_lossless();
    run(x, 400000, 1); bp_say(BP_SWAP);
    const int l3 = bp_lossless();
    printf("the speakers' channels: intact %d (lossless %d), swapped and inverted %d (lossless %d, \"%s\"), with a volume: lossless %d, a sample changed: lossless %d\n",
           st0, l0, st1, l1, named ? "swap,polarity" : why, l2, l3);
    /* the swap before the comparison: the source fingerprinted as the output carries it */
    static int32_t sw[2 * 400000];
    for (long i = 0; i < 400000; i++) { sw[2 * i] = x[2 * i + 1]; sw[2 * i + 1] = x[2 * i]; }
    bp_start(1); bp_base(0);
    for (long i = 0; i < 400000; i += 1000) bp_in_as(x + 2 * i, 1000, 2, BP_IN_SWAP);
    for (long i = 0; i < 400000; i += 689) bp_out((uint64_t)i, sw + 2 * i, (unsigned long)(400000 - i < 689 ? 400000 - i : 689));
    const long s_ok = atomic_load(&bp.ok), s_bad = atomic_load(&bp.bad);
    bp_start(1); bp_base(0);
    for (long i = 0; i < 400000; i += 1000) bp_in_as(x + 2 * i, 1000, 2, BP_IN_SWAP);
    for (long i = 0; i < 400000; i += 689) bp_out((uint64_t)i, x + 2 * i, (unsigned long)(400000 - i < 689 ? 400000 - i : 689));
    const long u_bad = atomic_load(&bp.bad);
    /* turned on while it plays: as it came up to frame 100000, a crossfade (blocks 100000 to 101999: skipped), then
     * swapped; the output's crossfade frames neither (a blend) */
    static int32_t mix[2 * 400000];
    bp_start(1); bp_base(0);
    for (long i = 0; i < 400000; i += 1000) {
        const int how = i < 100000 ? BP_IN_AS : i < 102000 ? BP_IN_SKIP : BP_IN_SWAP;
        bp_in_as(x + 2 * i, 1000, 2, how);
        for (long k = i; k < i + 1000; k++) {
            if (how == BP_IN_AS) { mix[2 * k] = x[2 * k]; mix[2 * k + 1] = x[2 * k + 1]; }
            else if (how == BP_IN_SWAP || k >= 100240) { mix[2 * k] = x[2 * k + 1]; mix[2 * k + 1] = x[2 * k]; }
            else { mix[2 * k] = x[2 * k] / 2 + x[2 * k + 1] / 2; mix[2 * k + 1] = mix[2 * k]; }
        }
    }
    for (long i = 0; i < 400000; i += 689) bp_out((uint64_t)i, mix + 2 * i, (unsigned long)(400000 - i < 689 ? 400000 - i : 689));
    const long m_ok = atomic_load(&bp.ok), m_bad = atomic_load(&bp.bad), m_skip = atomic_load(&bp.skipped);
    printf("the swap before the comparison: swapped both sides %ld equal, %ld different; the output not swapped: %ld different; turned on while it plays: %ld equal, %ld skipped, %ld different\n",
           s_ok, s_bad, u_bad, m_ok, m_skip, m_bad);
    const int swap_ok = s_ok >= 90 && s_bad == 0 && u_bad >= 90 && m_bad == 0 && m_skip == 1 && m_ok == s_ok - 1;   /* frames 100000…101999: one span (98304…102399) */
    const int ok = a == 0 && b >= 1 && c >= 1 && d >= 1 && per_s < 0.005 && st0 == 1 && !l0 && st1 == 0 && l1 && named && !l2 && !l3 && swap_ok;
    puts(ok ? "BITPERFECT OK" : "BITPERFECT FAIL");
    return ok ? 0 : 1;
}
