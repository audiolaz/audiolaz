/* src/player/bitperfect.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_BITPERFECT_H
#define AUDIOLAZ_BITPERFECT_H
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BP_SPAN 4096                    /* frames a comparison */
#define BP_Q 256                        /* spans queued (≥ 23 s at 44.1 kHz: more than any ring) */
enum {                                  /* why it is not intact (bits of bp.why) */
    BP_CORRECTION = 1, BP_LEVEL = 2, BP_VOLUME = 4, BP_UPSAMPLING = 8, BP_DSD = 16, BP_CHANNELS = 32, BP_BITS = 64,
    BP_RATE = 128, BP_DROPPED = 256, BP_CHANGED = 512, BP_LIMITER = 1024, BP_OUTPUT = 2048,
    BP_NIGHT = 4096,
    BP_SWAP = 8192, BP_POLARITY = 16384 /* the speakers' channels (channels.h): exact, after the comparison */
};
#define BP_EXACT (BP_SWAP | BP_POLARITY) /* what changes no sample's value: lossless */
static const char *const BP_WHY_NAME[] = { "correction", "level", "volume", "upsampling", "dsd", "channels", "bits",
                                           "rate", "dropped", "changed", "limiter", "output", "night",
                                           "swap", "polarity" };
typedef struct {
    _Atomic int on;                     /* the writer compares (the offset below known) */
    _Atomic int checked;
    _Atomic int64_t base;               /* the ring's place of the music's frame 0 */
    _Atomic unsigned why;               /* BP_* said this play */
    /* the play's thread */
    int64_t in_m; uint64_t in_h; int32_t in_or;
    struct { int64_t end; uint64_t h; } q[BP_Q];
    _Atomic unsigned qw, qr;
    /* the writer */
    int64_t out_k; uint64_t out_h; int out_any, out_spoiled;
    _Atomic long ok, bad, skipped;      /* spans compared equal, different; spans not comparable (skipped) */
    _Atomic int bits_in;                /* the source's significant bits seen (32 − its words' common trailing zeros) */
} bitperf;
static bitperf bp;

static inline uint64_t bp_mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
static inline uint64_t bp_f(int64_t m, int32_t l, int32_t r) {
    return bp_mix((uint64_t)m * 0xD6E8FEB86659FD93ull ^ ((uint64_t)(uint32_t)l << 32 | (uint32_t)r));
}

static inline void bp_start(int checked) {
    atomic_store(&bp.on, 0);
    bp.in_m = 0; bp.in_h = 0; bp.in_or = 0;
    atomic_store(&bp.qw, 0); atomic_store(&bp.qr, 0);
    bp.out_k = -1; bp.out_h = 0; bp.out_any = 0; bp.out_spoiled = 0;
    atomic_store(&bp.ok, 0); atomic_store(&bp.bad, 0); atomic_store(&bp.skipped, 0);
    atomic_store(&bp.why, 0); atomic_store(&bp.bits_in, 0);
    atomic_store(&bp.checked, checked);
}
static inline void bp_say(unsigned why) { atomic_fetch_or(&bp.why, why); }

static inline void bp_base(int64_t base) { atomic_store(&bp.base, base); atomic_store_explicit(&bp.on, 1, memory_order_release); }
static inline void bp_stop(void) { atomic_store(&bp.on, 0); atomic_store(&bp.checked, 0); }
/* The play's thread: n frames of the source as read (nch channels interleaved). */
static inline void bp_in(const int32_t *raw, long n, int nch) {
    if (!atomic_load_explicit(&bp.checked, memory_order_relaxed)) return;
    for (long i = 0; i < n; i++) {
        const int32_t l = raw[(size_t)i * (size_t)nch], r = raw[(size_t)i * (size_t)nch + (nch > 1)];
        bp.in_or |= l | r;
        bp.in_h += bp_f(bp.in_m, l, r);
        if (++bp.in_m % BP_SPAN == 0) {
            const unsigned w = atomic_load_explicit(&bp.qw, memory_order_relaxed);
            if (w - atomic_load_explicit(&bp.qr, memory_order_acquire) < BP_Q) {
                bp.q[w % BP_Q].end = bp.in_m; bp.q[w % BP_Q].h = bp.in_h;
                atomic_store_explicit(&bp.qw, w + 1, memory_order_release);
            }
            bp.in_h = 0;
        }
    }
    int b = 32;
    if (bp.in_or) { uint32_t v = (uint32_t)bp.in_or; b = 32; while (b > 1 && !(v & 1)) { v >>= 1; b--; } }
    else b = 0;
    atomic_store_explicit(&bp.bits_in, b, memory_order_relaxed);
}
/* The writer: the span ending at end compared with the source's, if it has it. */
static inline void bp_close(int64_t end, uint64_t h, int spoiled) {
    for (;;) {
        const unsigned r = atomic_load_explicit(&bp.qr, memory_order_relaxed);
        if (r == atomic_load_explicit(&bp.qw, memory_order_acquire)) return;   /* not read yet: nothing to compare */
        const int64_t e = bp.q[r % BP_Q].end;
        if (e > end) return;
        const uint64_t hin = bp.q[r % BP_Q].h;
        atomic_store_explicit(&bp.qr, r + 1, memory_order_release);
        if (e < end) { atomic_fetch_add(&bp.skipped, 1); continue; }           /* a span the output never had */
        if (spoiled) atomic_fetch_add(&bp.skipped, 1);
        else if (hin == h) atomic_fetch_add(&bp.ok, 1);
        else atomic_fetch_add(&bp.bad, 1);
        return;
    }
}
/* The writer: n frames taken from the ring at r0, as they go to the output. */
static inline void bp_out(uint64_t r0, const int32_t *pcm, unsigned long n) {
    if (!atomic_load_explicit(&bp.on, memory_order_acquire)) return;
    const int64_t base = atomic_load_explicit(&bp.base, memory_order_relaxed);
    for (unsigned long i = 0; i < n; i++) {
        const int64_t m = (int64_t)(r0 + i) - base;
        if (m < 0) continue;
        const int64_t k = m / BP_SPAN;
        if (k != bp.out_k) {
            if (bp.out_k >= 0 && bp.out_any) bp_close((bp.out_k + 1) * BP_SPAN, bp.out_h, bp.out_spoiled);
            bp.out_spoiled = m % BP_SPAN != 0;                                  /* entered past its start: not whole */
            bp.out_k = k; bp.out_h = 0; bp.out_any = 0;
        }
        bp.out_h += bp_f(m, pcm[2 * i], pcm[2 * i + 1]);
        bp.out_any = 1;
    }
}
/* The writer dropped frames of the ring (a backlog): the span going on cannot be compared. */
static inline void bp_skip(void) { bp.out_spoiled = 1; }
/* For the status: 1 intact (verified), 0 not, −1 not known; why: the reasons' names, comma separated. */
static inline int bp_state(char *why, size_t wl) {
    const unsigned w = atomic_load(&bp.why) | (atomic_load(&bp.bad) ? BP_CHANGED : 0);
    size_t o = 0;
    if (wl) why[0] = 0;
    for (unsigned i = 0; i < sizeof BP_WHY_NAME / sizeof *BP_WHY_NAME; i++)
        if (w & (1u << i) && o + strlen(BP_WHY_NAME[i]) + 2 < wl) o += (size_t)snprintf(why + o, wl - o, "%s%s", o ? "," : "", BP_WHY_NAME[i]);
    if (w) return 0;
    if (!atomic_load(&bp.checked) || !atomic_load(&bp.ok)) return -1;
    return 1;
}
/* 1: not bit-perfect only by the speakers' channels (swapped, a polarity inverted: exact), the spans compared
 * equal: lossless, verified; 0 otherwise. */
static inline int bp_lossless(void) {
    const unsigned w = atomic_load(&bp.why) | (atomic_load(&bp.bad) ? BP_CHANGED : 0);
    return w && !(w & ~(unsigned)BP_EXACT) && atomic_load(&bp.checked) && atomic_load(&bp.ok) > 0;
}
#endif
