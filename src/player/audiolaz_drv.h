/* src/player/audiolaz_drv.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_DRV_H
#define AUDIOLAZ_DRV_H
#include <stdatomic.h>
#include <stddef.h>                    /* offsetof */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>                   /* strcasecmp (az_shown) */

#define AZ_SHM_NAME "/audiolaz.out"    /* AUDIOLAZ_SHM in the environment: another one (the tests) */
#define AZ_MAGIC 0x617A6C32u           /* "azl2" (12/10: the device's clock published, its IO cycle) */
#define AZ_RING (1u << 18)             /* frames: 0.34 s at 768 kHz, 5.9 s at 44.1 kHz */
#define AZ_CH 2
#define AZ_BEAT_NS 1000000000ull       /* the player's beat older than this: the host clock */
#define AZ_BEAT_LX_NS 250000000ull     /* Linux: the player's beat older than this, the plugin's own host clock */
#define AZ_F32 0u
#define AZ_S32 1u
#define AZ_ZTS_PERIOD 16384u           /* the device's zero time stamp period (frames) */

typedef struct {
    uint32_t magic, ch, ring;
    _Atomic uint32_t rate;             /* the device's nominal rate now */
    _Atomic uint32_t running;          /* the apps' IO on */
    _Atomic uint32_t gen;              /* each StartIO and rate change */
    _Atomic uint64_t w;                /* sample time after the last frame written (frames) */
    _Atomic uint64_t beat;             /* player: mach_absolute_time of its last read, 0 none */
    _Atomic int64_t ppb;               /* player: its clock's rate against the host's (parts per billion, + slower) */
    /* driver: its last zero time stamp (sample zs at host time zh, tpf host ticks a frame), under seq (odd while
     * written: a seqlock, the IO thread alone writing), so that the player releases what came at the device's own pace
     * (a capture card's), not in the HAL's bursts of one IO cycle (cycle: its frames, the last WriteMix's) */
    _Atomic uint32_t seq, cycle;
    double zs, tpf;
    uint64_t zh;

    _Atomic uint64_t rd;
    _Atomic uint32_t fmt, lx;          /* lx: 1, the Linux plugin's ring; 2, an uplink's (below) */
    _Atomic uint64_t t0;

    _Atomic uint32_t lat_us, vol_cdb;
    _Atomic uint64_t hold_ns;
    uint8_t pad[128 - 4 * 3 - 4 * 3 - 8 * 3 - 4 * 2 - 8 * 3 - 8 * 3 - 4 * 2 - 8];
    float data[AZ_RING * AZ_CH];
} az_shm;
_Static_assert(offsetof(az_shm, data) == 128, "az_shm: its header 128 bytes, as every driver and plugin built before");

/* The device's sample time now, by its last zero time stamp (0: none yet) */
static inline double az_now(az_shm *m, uint64_t host) {
    for (int i = 0; i < 100; i++) {
        const uint32_t a = atomic_load_explicit(&m->seq, memory_order_acquire);
        if (a & 1) continue;
        const double zs = m->zs, tpf = m->tpf; const uint64_t zh = m->zh;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&m->seq, memory_order_relaxed) != a) continue;
        if (!(tpf > 0) || !zh) return 0;
        return zs + ((double)host - (double)zh) / tpf;
    }
    return 0;
}
static inline const char *az_shm_name(void) {
    const char *e = getenv("AUDIOLAZ_SHM");
    return e && *e == '/' ? e : AZ_SHM_NAME;
}
/* float sample (±1 full scale) → 32-bit integer, full scale ±2^31: a 24-bit value in float comes back exact */
static inline int32_t az_s32(float v) {
    const double d = (double)v * 2147483648.0;
    return d >= 2147483647.0 ? INT32_MAX : d <= -2147483648.0 ? INT32_MIN : (int32_t)d;
}
/* Ring frame i's sample c as a 32-bit integer (full scale ±2^31), whatever the ring holds */
static inline int32_t az_get(const az_shm *m, uint32_t fmt, uint64_t i, int c) {
    const float *f = m->data + AZ_CH * (i & (AZ_RING - 1)) + c;
    if (fmt == AZ_S32) { int32_t v; __builtin_memcpy(&v, f, 4); return v; }
    return az_s32(*f);
}
/* The device's host ticks a frame: nominal (the host's frequency over the rate), times 1 + ppb·1e-9 */
static inline double az_tpf(double host_hz, uint32_t rate, int64_t ppb) { return host_hz / rate * (1.0 + (double)ppb * 1e-9); }

#define AZ_INFO_NAME "/audiolaz.info"
#define AZ_INFO_MAGIC 0x617A6931u     /* "azi1" */
#define AZ_VOL_FLOOR_CDB 6000u         /* −60 dB: the control's bottom, silence */
#define AZ_INFO_TEXT 64
typedef struct {
    uint32_t magic;
    _Atomic uint32_t seq;              /* the player's seqlock over name, maker, model, local, latency */
    char name[AZ_INFO_TEXT], maker[AZ_INFO_TEXT], model[AZ_INFO_TEXT];
    uint32_t local;
    _Atomic uint32_t latency;          /* frames at the device's rate */
    _Atomic uint32_t vseq;             /* the driver's: + 1 at each volume change */
    _Atomic uint32_t vol_cdb;          /* the volume control: hundredths of a dB below full */
    uint32_t pad[11];
} az_info;
static inline const char *az_info_name(void) {
    const char *e = getenv("AUDIOLAZ_SHM_INFO");
    return e && *e == '/' ? e : AZ_INFO_NAME;
}

#define AZ_SHOWN_MAX 32
#define AZ_SHOWN_TAG " (audiolaz)"
#define AZ_SHOWN_ARROW "audiolaz \xe2\x86\x92 "
static inline void az_shown(char *d, size_t n, const char *dac) {
    char b[AZ_INFO_TEXT * 2];
    size_t w = 0;
    if (n > AZ_SHOWN_MAX) n = AZ_SHOWN_MAX;
    if (!n) return;
    if (dac && !strncmp(dac, AZ_SHOWN_ARROW, sizeof AZ_SHOWN_ARROW - 1)) {
        size_t k = 0;
        while (dac[k] && k + 1 < n) {
            const unsigned char c = (unsigned char)dac[k];
            const size_t cw = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            if (k + cw + 1 > n) break;
            k += cw;
        }
        if (dac[k] && dac[k] != ' ') {                 /* cut: at a word's end, after the arrow */
            size_t sp = k;
            while (sp > sizeof AZ_SHOWN_ARROW - 1 && dac[sp] != ' ') sp--;
            if (sp > sizeof AZ_SHOWN_ARROW - 1) k = sp;
        }
        while (k > 0 && dac[k - 1] == ' ') k--;
        memcpy(d, dac, k); d[k] = 0;
        for (size_t i = 0; i < k; i++) if ((unsigned char)d[i] < 0x20 || d[i] == '"' || d[i] == '\\' || d[i] == '%') d[i] = ' ';
        return;
    }
    for (const char *q = dac ? dac : ""; *q && w + 1 < sizeof b; q++) {
        const unsigned char c = (unsigned char)*q;
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\' || c == '%') continue;
        if (c == ' ' && (!w || b[w - 1] == ' ')) continue;
        b[w++] = (char)c;
    }
    while (w && b[w - 1] == ' ') w--;
    b[w] = 0;
    const size_t tl = sizeof AZ_SHOWN_TAG - 1;
    if (w >= tl && !strcasecmp(b + w - tl, AZ_SHOWN_TAG)) { w -= tl; b[w] = 0; }
    if (!strcasecmp(b, "audiolaz")) { w = 0; b[0] = 0; }
    if (w > 2 && w % 2 == 1 && b[w / 2] == ' ' && !strncmp(b, b + w / 2 + 1, w / 2)) { w /= 2; b[w] = 0; }
    if (!w) { snprintf(d, n, "audiolaz"); return; }
    if (n <= tl + 1) { snprintf(d, n, "audiolaz"); return; }
    size_t room = n - 1 - tl;
    if (w > room) {
        w = room;
        while (w > 0 && ((unsigned char)b[w] & 0xC0) == 0x80) w--;   /* b[w]: the cut's first byte, a character's start */
        if (b[w] != ' ') { size_t sp = w; while (sp > 0 && b[sp] != ' ') sp--; if (sp > 0) w = sp; }   /* a word whole */
        while (w > 0 && b[w - 1] == ' ') w--;
        b[w] = 0;
    }
    memcpy(d, b, w);                                   /* w + the tag + its end: n at most */
    memcpy(d + w, AZ_SHOWN_TAG, tl + 1);
}
#endif
