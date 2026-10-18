/* src/player/bitperfect.h : its cost, and that it sees a change. One second of 192 kHz stereo through bp_in and
 * bp_out (the play's thread and the writer's work), 200 times: the share of a core it takes; then the same frames with
 * one sample changed by one LSB, one frame dropped, one moved: each seen (a span different), and intact frames all equal.
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
    const int ok = a == 0 && b >= 1 && c >= 1 && d >= 1 && per_s < 0.005;
    puts(ok ? "BITPERFECT OK" : "BITPERFECT FAIL");
    return ok ? 0 : 1;
}
