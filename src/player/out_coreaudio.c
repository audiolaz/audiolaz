/* src/player/out_coreaudio.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <errno.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/semaphore.h>
#include <mach/thread_policy.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "out.h"

static void olog(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); fflush(stderr);
}
static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static uint64_t host_ns(uint64_t h) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return h * tb.numer / tb.denom;
}

/* ── Options (out_option) ───────────────────────────────────────────────────────────────────────── */
static int opt_exclusive = 1;          /* hog mode while open (the app ran the Swift player with --exclusive) */
static int opt_integer = 0;            /* --integer on: non-mixable 32-bit integers when held and offered */
static unsigned refused_test[16];      /* --test-refuse-rates 48000,96000: as if the device did not list them (tests) */
static int n_refused_test = 0;

/* ── Core Audio's properties ────────────────────────────────────────────────────────────────────── */
static AudioObjectPropertyAddress pa(AudioObjectPropertySelector s, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = { s, sc, kAudioObjectPropertyElementMain };
    return a;
}
static int pget(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope sc, void *v, UInt32 sz) {
    AudioObjectPropertyAddress a = pa(s, sc);
    UInt32 z = sz;
    return AudioObjectGetPropertyData(o, &a, 0, NULL, &z, v) == noErr ? 0 : -1;
}
static int pset(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope sc, const void *v, UInt32 sz) {
    AudioObjectPropertyAddress a = pa(s, sc);
    return AudioObjectSetPropertyData(o, &a, 0, NULL, sz, v) == noErr ? 0 : -1;
}
static void pstr(AudioObjectID o, AudioObjectPropertySelector s, char *buf, size_t len) {
    CFStringRef c = NULL;
    buf[0] = 0;
    if (pget(o, s, kAudioObjectPropertyScopeGlobal, &c, sizeof c) == 0 && c) {
        if (!CFStringGetCString(c, buf, (CFIndex)len, kCFStringEncodingUTF8)) buf[0] = 0;
        CFRelease(c);
    }
}
static int dev_ids(AudioObjectID *ids, int max) {
    AudioObjectPropertyAddress a = pa(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    UInt32 z = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &z) != noErr) return 0;
    if (z > (UInt32)max * sizeof *ids) z = (UInt32)max * sizeof *ids;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &z, ids) != noErr) return 0;
    return (int)(z / sizeof *ids);
}
/* Its output channels (0: an input only, or gone). */
static void pstr(AudioObjectID o, AudioObjectPropertySelector s, char *buf, size_t len);

static int dev_is_ours(AudioObjectID d) { char u[64]; pstr(d, kAudioDevicePropertyDeviceUID, u, sizeof u); return !strcmp(u, "audiolaz_UID"); }
static int dev_out_channels(AudioObjectID d) {
    if (dev_is_ours(d)) return 0;
    AudioObjectPropertyAddress a = pa(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput);
    UInt32 z = 0;
    if (AudioObjectGetPropertyDataSize(d, &a, 0, NULL, &z) != noErr || z < sizeof(AudioBufferList)) return 0;
    AudioBufferList *l = malloc(z);
    int n = 0;
    if (l && AudioObjectGetPropertyData(d, &a, 0, NULL, &z, l) == noErr)
        for (UInt32 i = 0; i < l->mNumberBuffers; i++) n += (int)l->mBuffers[i].mNumberChannels;
    free(l);
    return n;
}
static AudioObjectID dev_default(void) {
    AudioObjectID d = kAudioObjectUnknown;
    pget(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, &d, sizeof d);
    if (d != kAudioObjectUnknown && dev_is_ours(d)) {
        AudioObjectID ids[128];
        const int n = dev_ids(ids, 128);
        d = kAudioObjectUnknown;
        for (int i = 0; i < n && d == kAudioObjectUnknown; i++) if (dev_out_channels(ids[i]) > 0) d = ids[i];
    }
    return d;
}
static pid_t dev_hog(AudioObjectID d) {
    pid_t p = -1;
    return pget(d, kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal, &p, sizeof p) ? -1 : p;
}
static double dev_rate(AudioObjectID d) {
    Float64 r = 0;
    pget(d, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &r, sizeof r);
    return r;
}
static int test_refused(unsigned r) {
    for (int i = 0; i < n_refused_test; i++) if (refused_test[i] == r) return 1;
    return 0;
}
/* The rates it lists (kAudioDevicePropertyAvailableNominalSampleRates), as the Swift player read them. */
static int dev_lists(AudioObjectID d, unsigned r) {
    AudioObjectPropertyAddress a = pa(kAudioDevicePropertyAvailableNominalSampleRates, kAudioObjectPropertyScopeOutput);
    AudioValueRange v[64];
    UInt32 z = 0;
    if (AudioObjectGetPropertyDataSize(d, &a, 0, NULL, &z) != noErr || !z) return 0;
    if (z > sizeof v) z = sizeof v;
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &z, v) != noErr) return 0;
    for (UInt32 i = 0; i < z / sizeof *v; i++) if (v[i].mMinimum - 1 <= r && r <= v[i].mMaximum + 1) return 1;
    return 0;
}
/* Rates the device listed but would not go to (10/10, the Flex 8 DL: it stays at 88.2 kHz), kept for the engine's
 * life per device, forgotten when it is found at another rate than when refused (its own setting changed). */
typedef struct { char uid[160]; unsigned rate; double at; } refusal;
static refusal refusals[32];
static int n_refusals = 0;
static pthread_mutex_t refusals_lk = PTHREAD_MUTEX_INITIALIZER;
static int dev_refused(const char *uid, unsigned r, double now_rate) {
    int hit = 0;
    pthread_mutex_lock(&refusals_lk);
    for (int i = 0; i < n_refusals; i++)
        if (refusals[i].rate == r && !strcmp(refusals[i].uid, uid)) {
            if (fabs(refusals[i].at - now_rate) < 1) hit = 1;
            else refusals[i] = refusals[--n_refusals];   /* found elsewhere since: asked again */
            break;
        }
    pthread_mutex_unlock(&refusals_lk);
    return hit;
}
static void dev_refuse(const char *uid, unsigned r, double now_rate) {
    pthread_mutex_lock(&refusals_lk);
    if (n_refusals < 32) { refusal *x = &refusals[n_refusals++]; snprintf(x->uid, sizeof x->uid, "%s", uid); x->rate = r; x->at = now_rate; }
    pthread_mutex_unlock(&refusals_lk);
}
/* Whether it plays r itself: listed, not refused before, not refused by the tests. */
static int dev_offers(AudioObjectID d, const char *uid, unsigned r) {
    return !test_refused(r) && dev_lists(d, r) && !dev_refused(uid, r, dev_rate(d));
}
static const unsigned STD_RATES[] = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 };
#define N_STD (sizeof STD_RATES / sizeof *STD_RATES)
/* The rates to play music at `want` at when the device does not take it, best first (squeeze.swift rateCandidates):
 * the same family above (44.1 → 88.2, 176.4; 48 → 96, 192), then any above, then the highest below. */
static int rate_candidates(AudioObjectID d, const char *uid, unsigned want, unsigned *out) {
    int n = 0;
    for (size_t i = 0; i < N_STD; i++) if (STD_RATES[i] > want && STD_RATES[i] % want == 0 && dev_offers(d, uid, STD_RATES[i])) out[n++] = STD_RATES[i];
    for (size_t i = 0; i < N_STD; i++) if (STD_RATES[i] > want && STD_RATES[i] % want && dev_offers(d, uid, STD_RATES[i])) out[n++] = STD_RATES[i];
    for (size_t i = N_STD; i-- > 0;) if (STD_RATES[i] < want && dev_offers(d, uid, STD_RATES[i])) out[n++] = STD_RATES[i];
    return n;
}
/* The device set to r and the rate applied (it takes a moment): squeeze.swift setDeviceRate, 1.5 s at most. */
static int dev_set_rate(AudioObjectID d, unsigned r) {
    if (fabs(dev_rate(d) - r) < 1) return 1;
    Float64 want = r;
    const int e = pset(d, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &want, sizeof want);
    for (int i = 0; i < (e ? 4 : 60); i++) { if (fabs(dev_rate(d) - r) < 1) return 1; usleep(25000); }
    return 0;
}
/* The device a name stands for: its UID exactly, "default", else the first output whose name contains it (any case). */
static AudioObjectID dev_find(const char *dev) {
    if (!dev || !*dev) return kAudioObjectUnknown;
    if (!strncmp(dev, OUT_CARD_PREFIX, OUT_CARD_PREFIX_LEN)) dev += OUT_CARD_PREFIX_LEN;
    if (!strcmp(dev, "default")) return dev_default();
    AudioObjectID ids[128];
    const int n = dev_ids(ids, 128);
    char s[256];
    for (int i = 0; i < n; i++) { pstr(ids[i], kAudioDevicePropertyDeviceUID, s, sizeof s); if (!strcmp(s, dev) && dev_out_channels(ids[i]) > 0) return ids[i]; }
    for (int i = 0; i < n; i++) {
        if (dev_out_channels(ids[i]) <= 0) continue;
        pstr(ids[i], kAudioObjectPropertyName, s, sizeof s);
        if (strcasestr(s, dev)) return ids[i];
    }
    return kAudioObjectUnknown;
}
static void proc_label(pid_t p, char *buf, size_t len) {
    char n[256] = "";
    if (proc_name(p, n, sizeof n) <= 0) snprintf(n, sizeof n, "?");
    snprintf(buf, len, "%s (process %d)", n, (int)p);
}
static AudioObjectID dev_stream(AudioObjectID d) {
    AudioObjectID s[16];
    AudioObjectPropertyAddress a = pa(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
    UInt32 z = sizeof s;
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &z, s) != noErr || z < sizeof *s) return kAudioObjectUnknown;
    return s[0];
}
/* --integer on (squeeze.swift integerFormat): the stream's non-mixable, packed 32-bit integer stereo format at r. */
static int integer_format(AudioObjectID st, unsigned r, AudioStreamBasicDescription *f) {
    AudioObjectPropertyAddress a = pa(kAudioStreamPropertyAvailablePhysicalFormats, kAudioObjectPropertyScopeGlobal);
    UInt32 z = 0;
    if (AudioObjectGetPropertyDataSize(st, &a, 0, NULL, &z) != noErr || !z) return 0;
    AudioStreamRangedDescription *all = malloc(z);
    int ok = 0;
    if (all && AudioObjectGetPropertyData(st, &a, 0, NULL, &z, all) == noErr)
        for (UInt32 i = 0; i < z / sizeof *all && !ok; i++) {
            const AudioStreamBasicDescription m = all[i].mFormat;
            const int fits = fabs(m.mSampleRate - r) < 1 || (m.mSampleRate == 0 && all[i].mSampleRateRange.mMinimum <= r && r <= all[i].mSampleRateRange.mMaximum);
            if ((m.mFormatFlags & kAudioFormatFlagIsSignedInteger) && (m.mFormatFlags & kAudioFormatFlagIsNonMixable) &&
                (m.mFormatFlags & kAudioFormatFlagIsPacked) && m.mBitsPerChannel == 32 && m.mChannelsPerFrame == 2 && m.mBytesPerFrame == 8 && fits) {
                *f = m; f->mSampleRate = r; ok = 1;
            }
        }
    free(all);
    return ok;
}

/* ── The open device ────────────────────────────────────────────────────────────────────────────── */
struct out_pcm {
    int null;                          /* the null device */
    FILE *dump;                        /* null:<file>: what it is given (the tests) */
    unsigned rate;                     /* the stream's (roomd.c's frames) */
    unsigned long period, buffer;      /* in the stream's frames */
    double t0;                         /* null: when its frame 0 is heard */
    uint64_t written;                  /* null: frames written since the start */

    AudioObjectID dev, stream;
    char name[160], uid[160];
    char maker[160], model[160];       /* out_device_maker, out_device_model */
    AudioComponentInstance au;
    unsigned drate;                    /* the device's rate */
    int resample;                      /* drate != rate: the converter below */
    double ratio;                      /* drate / rate */
    int hogged;                        /* we hold its hog */
    int fmt_set;                       /* --integer: its physical format changed, fmt_was to put back */
    AudioStreamBasicDescription fmt_was;
    double lat_dev;                    /* its latency and its stream's (device frames) */
    UInt32 bufsz, safety;
    int bits;                          /* the bits of each 32-bit word the device keeps (out_bits) */
    /* The ring of device frames (32-bit words, two a frame: integers natively, floats resampled): out_write pushes,
     * the render callback pops. cap: what it may hold (the buffer asked, in device frames). */
    int32_t *ring;
    uint64_t mask, cap;
    _Atomic uint64_t w, r;
    _Atomic uint64_t seq;              /* odd while the callback updates r, t_host, t_n together */
    _Atomic uint64_t t_host;           /* the last buffer given: when its first frame goes out (host time), its frames */
    _Atomic uint32_t t_n;
    _Atomic uint64_t last_cb;          /* host time of the last callback */
    _Atomic int started, ever_full, waiting;
    _Atomic long underruns;
    semaphore_t sem;
    /* The converter (resampling): the stream's frames in as 64-bit floats, the device's out as 32-bit floats. */
    AudioConverterRef conv;
    double *stage;                     /* the frames handed to it this call */
    float *outb;
    unsigned long stage_cap;
    UInt32 stage_n, stage_pos;
    uint64_t fed, produced;            /* stream frames it took, device frames it gave, since the start */
    double t_check;                    /* the device's health, looked at every 250 ms (mono) */
    long dead;                         /* the error out_write says from now on (0: alive) */
};

static _Atomic(out_pcm *) open_dev;    /* the one open (roomd.c opens one at a time): its hog given back at exit */
static void release_at_exit(void) {
    out_pcm *o = atomic_exchange(&open_dev, NULL);
    if (!o || o->null) return;
    if (o->au) AudioOutputUnitStop(o->au);
    if (o->fmt_set && o->stream) pset(o->stream, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, &o->fmt_was, sizeof o->fmt_was);
    if (o->hogged) { pid_t p = -1; pset(o->dev, kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal, &p, sizeof p); }
}

static OSStatus render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io) {
    (void)flags; (void)bus;
    out_pcm *o = ref;
    int32_t *dst = io->mBuffers[0].mData;
    const UInt32 room = io->mBuffers[0].mDataByteSize / 8;
    if (n > room) n = room;
    const uint64_t r = atomic_load_explicit(&o->r, memory_order_relaxed), w = atomic_load_explicit(&o->w, memory_order_acquire);
    const uint64_t have = w - r, k = have < n ? have : n;
    for (uint64_t i = 0; i < k; i++) { const uint64_t j = (r + i) & o->mask; dst[2 * i] = o->ring[2 * j]; dst[2 * i + 1] = o->ring[2 * j + 1]; }
    if (k < n) {                                       /* ran short: silence (0 is 0.0f too), counted */
        memset(dst + 2 * k, 0, (size_t)(n - k) * 8);
        if (atomic_load_explicit(&o->ever_full, memory_order_relaxed)) atomic_fetch_add_explicit(&o->underruns, 1, memory_order_relaxed);
    }
    const uint64_t s = atomic_load_explicit(&o->seq, memory_order_relaxed);
    atomic_store_explicit(&o->seq, s + 1, memory_order_release);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&o->t_host, ts && (ts->mFlags & kAudioTimeStampHostTimeValid) ? ts->mHostTime : 0, memory_order_relaxed);
    atomic_store_explicit(&o->t_n, n, memory_order_relaxed);
    atomic_store_explicit(&o->r, r + k, memory_order_release);
    atomic_store_explicit(&o->seq, s + 2, memory_order_release);
    atomic_store_explicit(&o->last_cb, mach_absolute_time(), memory_order_relaxed);
    if (atomic_exchange_explicit(&o->waiting, 0, memory_order_acq_rel)) semaphore_signal(o->sem);
    return noErr;
}

static void ca_free(out_pcm *o) {
    if (!o) return;
    if (o->au) { AudioOutputUnitStop(o->au); AudioUnitUninitialize(o->au); AudioComponentInstanceDispose(o->au); }
    if (o->fmt_set && o->stream) pset(o->stream, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, &o->fmt_was, sizeof o->fmt_was);
    if (o->hogged) { pid_t p = -1; pset(o->dev, kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal, &p, sizeof p); }
    if (o->conv) AudioConverterDispose(o->conv);
    if (o->sem) semaphore_destroy(mach_task_self(), o->sem);
    free(o->ring); free(o->stage); free(o->outb);
    free(o);
}

/* "null", or "null:<file>" (10/10, the tests) */
static int is_null(const char *dev) { return dev && (!strcmp(dev, "null") || !strncmp(dev, "null:", 5)); }
out_pcm *out_open(const char *dev, int fmt, unsigned rate, unsigned long period, unsigned periods, int quiet) {
    if (is_null(dev)) {
        if (fmt != OUTF_S32 && fmt != OUTF_DSD_U32_LE) { if (!quiet) olog("null: format refused"); return NULL; }
        out_pcm *o = calloc(1, sizeof *o);
        if (!o) return NULL;
        o->null = 1; o->rate = rate; o->period = period; o->buffer = period * periods; o->t0 = 0;
        if (dev[4] == ':' && dev[5]) o->dump = fopen(dev + 5, "ab");
        return o;
    }
    if (fmt != OUTF_S32) { if (!quiet) olog("%s: Core Audio takes no native DSD (DoP does: dsdpack=dop)", dev ? dev : "(none)"); return NULL; }
    const AudioObjectID d = dev_find(dev);
    if (d == kAudioObjectUnknown) { if (!quiet) olog("%s: no such Core Audio output", dev ? dev : "(none)"); return NULL; }
    out_pcm *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->dev = d; o->rate = rate; o->period = period; o->buffer = period * periods;
    pstr(d, kAudioObjectPropertyName, o->name, sizeof o->name);
    pstr(d, kAudioObjectPropertyManufacturer, o->maker, sizeof o->maker);
    pstr(d, kAudioDevicePropertyModelUID, o->model, sizeof o->model);
    pstr(d, kAudioDevicePropertyDeviceUID, o->uid, sizeof o->uid);
    const pid_t me = getpid();
    pid_t h = dev_hog(d);
    char who[300];
    if (h > 0 && h != me) {                            /* held by another program: busy (roomd.c tries again) */
        if (!quiet) { proc_label(h, who, sizeof who); olog("%s: held exclusively by %s", o->name, who); }
        free(o); return NULL;
    }
    if (opt_exclusive && h != me) {
        pid_t p = me;
        pset(d, kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal, &p, sizeof p);
        h = dev_hog(d);
        if (h == me) o->hogged = 1;
        else if (h > 0) { if (!quiet) { proc_label(h, who, sizeof who); olog("%s: taken exclusively by %s meanwhile", o->name, who); } free(o); return NULL; }
        else if (!quiet) olog("%s: cannot be held exclusively: shared", o->name);
    }
    /* Its rate: the stream's when it plays it, else the best it takes (the header) */
    if (dev_offers(d, o->uid, rate)) {
        if (dev_set_rate(d, rate)) o->drate = rate;
        else {
            const double at = dev_rate(d);
            if (!quiet) olog("%s refuses %u Hz: it lists it, stays at %.0f Hz", o->name, rate, at);
            dev_refuse(o->uid, rate, at);
        }
    }
    if (!o->drate) {
        unsigned c[N_STD];
        const int nc = rate_candidates(d, o->uid, rate, c);
        for (int i = 0; i < nc && !o->drate; i++) {
            if (dev_set_rate(d, c[i])) o->drate = c[i];
            else {
                const double at = dev_rate(d);
                if (!quiet) olog("%s refuses %u Hz: it lists it, stays at %.0f Hz", o->name, c[i], at);
                dev_refuse(o->uid, c[i], at);
            }
        }
        if (!o->drate) {                               /* nothing it lists would do: where it stands, resampled */
            const double at = dev_rate(d);
            if (at >= 8000 && !test_refused((unsigned)at)) o->drate = (unsigned)lround(at);
        }
        if (!o->drate) { if (!quiet) olog("%s: no rate it would take for %u Hz", o->name, rate); ca_free(o); return NULL; }
    }
    o->resample = o->drate != rate;
    o->ratio = (double)o->drate / rate;
    o->stream = dev_stream(d);
    /* --integer on (held only, as squeeze.swift): its physical format to non-mixable 32-bit integers, put back after */
    if (opt_integer && o->hogged && o->stream && !o->resample) {
        AudioStreamBasicDescription want, was;
        if (integer_format(o->stream, o->drate, &want) && pget(o->stream, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, &was, sizeof was) == 0) {
            if (pset(o->stream, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, &want, sizeof want) == 0) {
                o->fmt_set = 1; o->fmt_was = was;
                AudioStreamBasicDescription now;
                for (int i = 0; i < 40; i++) {
                    if (pget(o->stream, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, &now, sizeof now) == 0 &&
                        now.mFormatFlags == want.mFormatFlags && now.mBitsPerChannel == 32 && fabs(now.mSampleRate - o->drate) < 1) break;
                    usleep(25000);
                }
            }
        } else if (!quiet) olog("%s: no 32-bit integer format at %u Hz: its own (32-bit floats)", o->name, o->drate);
    }
    /* The AUHAL on it, given the ring's frames at the device's rate (never its own converter's rate change) */
    AudioComponentDescription cd = { kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &cd);
    OSStatus e = comp ? AudioComponentInstanceNew(comp, &o->au) : -1;
    AudioStreamBasicDescription f = { (Float64)o->drate, kAudioFormatLinearPCM,
                                      (o->resample ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) | kAudioFormatFlagIsPacked,
                                      8, 1, 8, 2, 32, 0 };
    AURenderCallbackStruct cb = { render, o };
    if (!e) e = AudioUnitSetProperty(o->au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &o->dev, sizeof o->dev);
    if (!e) e = AudioUnitSetProperty(o->au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    if (!e) e = AudioUnitSetProperty(o->au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (!e) e = AudioUnitInitialize(o->au);
    if (e) { if (!quiet) olog("%s: its output unit refused (OSStatus %d)", o->name, (int)e); ca_free(o); return NULL; }
    /* The ring: the buffer asked, in device frames */
    o->cap = (uint64_t)ceil((double)o->buffer * o->ratio);
    uint64_t c = 1;
    while (c < o->cap + 8192) c <<= 1;
    o->mask = c - 1;
    o->ring = calloc(2 * c, sizeof(int32_t));
    if (!o->ring || semaphore_create(mach_task_self(), &o->sem, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) { o->sem = 0; ca_free(o); return NULL; }
    if (o->resample) {                                 /* Apple's converter at its best (squeeze.swift's Resampler) */
        AudioStreamBasicDescription in = { (Float64)rate, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 16, 1, 16, 2, 64, 0 };
        AudioStreamBasicDescription out = { (Float64)o->drate, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0 };
        UInt32 v;
        if (AudioConverterNew(&in, &out, &o->conv) != noErr) { o->conv = NULL; if (!quiet) olog("%s: no converter %u → %u Hz", o->name, rate, o->drate); ca_free(o); return NULL; }
        v = kAudioConverterSampleRateConverterComplexity_Mastering;
        AudioConverterSetProperty(o->conv, kAudioConverterSampleRateConverterComplexity, sizeof v, &v);
        v = kAudioConverterQuality_Max;
        AudioConverterSetProperty(o->conv, kAudioConverterSampleRateConverterQuality, sizeof v, &v);
        o->stage_cap = 4096;
        o->stage = malloc(sizeof(double) * 2 * o->stage_cap);
        o->outb = malloc(sizeof(float) * 2 * (size_t)(o->cap + 64));
        if (!o->stage || !o->outb) { ca_free(o); return NULL; }
    }
    /* what the device keeps of the 32-bit words (out_bits): its physical format's bits, at most the 24 its
     * float32 virtual format holds on the way (AUHAL's mixable path); all 32 in integer mode (non-mixable integers,
     * nothing in between); resampled, the converter's floats (its own rounding: nothing for roomd.c to dither) */
    o->bits = 32;
    if (o->stream && !o->resample) {
        AudioStreamBasicDescription ph;
        if (pget(o->stream, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, &ph, sizeof ph) == 0 && ph.mBitsPerChannel >= 8) {
            const int pb = (ph.mFormatFlags & kAudioFormatFlagIsFloat) ? 24 : (int)ph.mBitsPerChannel;
            o->bits = o->fmt_set ? pb : pb < 24 ? pb : 24;
        }
    }
    UInt32 lat = 0, slat = 0;
    pget(d, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, &lat, sizeof lat);
    if (o->stream) pget(o->stream, kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, &slat, sizeof slat);
    pget(d, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, &o->bufsz, sizeof o->bufsz);
    pget(d, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, &o->safety, sizeof o->safety);
    o->lat_dev = (double)lat + slat;
    o->t_check = mono();
    if (!quiet) olog("%s: open at %u Hz%s%s, 32-bit %s, buffer %lu frames, %d bits kept by the device", o->name, o->drate, o->hogged ? ", exclusive" : ", shared",
                     o->fmt_set ? ", integer mode" : "", o->resample ? "floats (resampled)" : "integers", o->buffer, o->bits);
    static int at_exit = 0;
    if (!at_exit++) atexit(release_at_exit);
    atomic_store(&open_dev, o);
    return o;
}

/* null: what it has played by now, frames */
static uint64_t null_played(out_pcm *o) {
    if (!o->t0) return 0;
    const double p = (mono() - o->t0) * o->rate;
    return p <= 0 ? 0 : (uint64_t)p > o->written ? o->written : (uint64_t)p;
}

/* The writer's thread in Mach real time : roomd.c asks SCHED_FIFO, which macOS keeps from user processes in
 * effect; Core Audio's own threads are time-constraint ones. Once per thread, at its first write: a period's time,
 * half of it computing at most (the resampler's share of a period at mastering quality is far under that). */
static _Thread_local int rt_done = 0;
static void writer_rt(out_pcm *o) {
    if (rt_done) return;
    rt_done = 1;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const double ns = 1e9 * (double)o->period / o->rate, per = ns * tb.denom / tb.numer;
    thread_time_constraint_policy_data_t p = { (uint32_t)per, (uint32_t)(per / 4), (uint32_t)per, 1 };
    if (thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT) != KERN_SUCCESS)
        olog("%s: the writer stays at normal priority (Mach time constraint refused)", o->name);
}

/* The device's health (every 250 ms from the writer): gone, taken, its rate changed behind us, or no longer calling. */
static long health(out_pcm *o) {
    if (o->dead) return o->dead;
    const double t = mono();
    if (t - o->t_check < 0.25) return 0;
    o->t_check = t;
    UInt32 alive = 0;
    char who[300];
    if (pget(o->dev, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, &alive, sizeof alive) || !alive) {
        olog("%s: gone", o->name); return o->dead = -ENODEV;
    }
    const pid_t h = dev_hog(o->dev);
    if (h > 0 && h != getpid()) { proc_label(h, who, sizeof who); olog("%s: taken exclusively by %s", o->name, who); return o->dead = -EBUSY; }
    if (o->hogged && h != getpid()) { olog("%s: no longer held exclusively", o->name); return o->dead = -EIO; }
    const double r = dev_rate(o->dev);
    if (fabs(r - o->drate) >= 1) { olog("%s: set to %.0f Hz behind us (it played at %u Hz)", o->name, r, o->drate); return o->dead = -EIO; }
    if (atomic_load(&o->started)) {
        const uint64_t last = atomic_load(&o->last_cb), now = mach_absolute_time();
        if (last && host_ns(now - last) > 1000000000ull) { olog("%s: stopped asking for its music", o->name); return o->dead = -EIO; }
    }
    return 0;
}
static uint64_t ring_fill(out_pcm *o) { return atomic_load_explicit(&o->w, memory_order_relaxed) - atomic_load_explicit(&o->r, memory_order_acquire); }
static void start_if_ready(out_pcm *o) {
    if (atomic_load(&o->started) || ring_fill(o) < o->cap / 2) return;
    atomic_store(&o->ever_full, 1);
    atomic_store(&o->last_cb, mach_absolute_time());
    const OSStatus e = AudioOutputUnitStart(o->au);
    if (e) { olog("%s: would not start (OSStatus %d)", o->name, (int)e); o->dead = -EIO; return; }
    atomic_store(&o->started, 1);
}
static void ring_push(out_pcm *o, const int32_t *src, uint64_t n) {
    const uint64_t w = atomic_load_explicit(&o->w, memory_order_relaxed);
    for (uint64_t i = 0; i < n; i++) { const uint64_t j = (w + i) & o->mask; o->ring[2 * j] = src[2 * i]; o->ring[2 * j + 1] = src[2 * i + 1]; }
    atomic_store_explicit(&o->w, w + n, memory_order_release);
}
/* The converter's input: this call's frames, once ('nodt' after them: the converter keeps its state for the next). */
#define NO_DATA ((OSStatus)0x6e6f6474)
static OSStatus conv_in(AudioConverterRef c, UInt32 *np, AudioBufferList *d, AudioStreamPacketDescription **pd, void *u) {
    (void)c; (void)pd;
    out_pcm *o = u;
    UInt32 k = o->stage_n - o->stage_pos;
    if (k > *np) k = *np;
    if (!k) { *np = 0; return NO_DATA; }
    d->mBuffers[0].mData = o->stage + 2 * o->stage_pos;
    d->mBuffers[0].mDataByteSize = k * 16;
    d->mBuffers[0].mNumberChannels = 2;
    *np = k; o->stage_pos += k; o->fed += k;
    return noErr;
}
/* What the converter holds: stream frames it took and has not given back (its output in time with its input). */
static double conv_held(out_pcm *o) { return o->resample ? (double)o->fed - (double)o->produced / o->ratio : 0; }
/* Room in the stream's frames. */
static long room_in(out_pcm *o) {
    const double room = (double)o->cap - (double)ring_fill(o);
    if (!o->resample) return (long)room;
    const long k = (long)floor(room / o->ratio - conv_held(o)) - 4;
    return k > 0 ? k : 0;
}

long out_write(out_pcm *o, const int32_t *pcm, unsigned long n) {
    if (o->null) {
        const uint64_t played = null_played(o);
        if (o->t0 && played >= o->written) {           /* ran dry: starts again from now, as a card after an underrun */
            /* said, with how late: the tests count these as a card's underruns (a gap heard) */
            olog("null: ran dry, %.1f ms late (a card's underrun)", ((mono() - o->t0) * o->rate - (double)o->written) * 1000.0 / o->rate);
            o->t0 = 0; o->written = 0;
        }
        const uint64_t held = o->written - null_played(o);
        if (held >= o->buffer) return OUT_AGAIN;
        const unsigned long room = (unsigned long)(o->buffer - held), k = n < room ? n : room;
        if (o->dump && k) fwrite(pcm, sizeof(int32_t) * 2, k, o->dump);
        o->written += k;
        if (!o->t0 && o->written >= o->buffer / 2) o->t0 = mono();   /* starts once half full, as a card started by roomd */
        return (long)k;
    }
    writer_rt(o);
    const long bad = health(o);
    if (bad) return bad;
    long room = room_in(o);
    if (room <= 0) { start_if_ready(o); return o->dead ? o->dead : OUT_AGAIN; }
    unsigned long k = n < (unsigned long)room ? n : (unsigned long)room;
    if (!o->resample) ring_push(o, pcm, k);
    else {
        if (k > o->stage_cap) k = o->stage_cap;
        for (unsigned long i = 0; i < 2 * k; i++) o->stage[i] = pcm[i] * (1.0 / 2147483648.0);   /* exact in 64 bits */
        o->stage_n = (UInt32)k; o->stage_pos = 0;
        UInt32 got = (UInt32)(o->cap - ring_fill(o));
        AudioBufferList bl = { 1, { { 2, got * 8, o->outb } } };
        const OSStatus e = AudioConverterFillComplexBuffer(o->conv, conv_in, o, &got, &bl, NULL);
        if (e && e != NO_DATA) { olog("%s: the converter failed (OSStatus %d)", o->name, (int)e); return o->dead = -EIO; }
        o->produced += got;
        ring_push(o, (const int32_t *)o->outb, got);   /* float bits, as the AU was told */
        k = o->stage_pos;                              /* what it took (all of it, but for a full output) */
    }
    start_if_ready(o);
    if (o->dead) return o->dead;
    return k ? (long)k : OUT_AGAIN;
}
void out_wait(out_pcm *o, int ms) {
    if (o->null) { const long us = (long)(1e6 * (double)o->period / o->rate); usleep((useconds_t)(us < ms * 1000L ? us : ms * 1000L)); return; }
    if (!atomic_load(&o->started)) { usleep(1000); return; }
    atomic_store(&o->waiting, 1);
    if (room_in(o) > 0) { atomic_store(&o->waiting, 0); return; }
    mach_timespec_t t = { (unsigned)(ms / 1000), (clock_res_t)((ms % 1000) * 1000000) };
    semaphore_timedwait(o->sem, t);
}
long out_avail(out_pcm *o) {
    if (o->null) return (long)(o->buffer - (o->written - null_played(o)));
    if (o->dead) return o->dead;
    return room_in(o);
}
int out_delay(out_pcm *o, long *frames) {
    if (o->null) { *frames = (long)(o->written - null_played(o)); return 0; }
    uint64_t s, r, th;
    uint32_t tn;
    do {                                               /* r and its buffer's time stamp together */
        s = atomic_load_explicit(&o->seq, memory_order_acquire);
        r = atomic_load_explicit(&o->r, memory_order_acquire);
        th = atomic_load_explicit(&o->t_host, memory_order_relaxed);
        tn = atomic_load_explicit(&o->t_n, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
    } while ((s & 1) || s != atomic_load_explicit(&o->seq, memory_order_acquire));
    const double fill = (double)(atomic_load(&o->w) - r);
    double taken;                                      /* given to the device, not out of it yet (device frames) */
    if (atomic_load(&o->started) && th) {
        const double out_end = (double)host_ns(th) / 1e9 + tn / (double)o->drate;
        taken = (out_end - (double)host_ns(mach_absolute_time()) / 1e9) * o->drate;
        if (taken < 0) taken = 0;
    } else taken = (double)o->bufsz + o->safety;      /* not started: what it will take first */
    *frames = lround((fill + taken + o->lat_dev) / o->ratio + conv_held(o));
    return 0;
}
void out_params(out_pcm *o, unsigned long *buffer, unsigned long *period) { *buffer = o->buffer; *period = o->period; }
void out_close(out_pcm *o) {
    if (!o) return;
    if (o->dump) { fclose(o->dump); o->dump = NULL; }
    if (!o->null) {
        out_pcm *x = o;
        atomic_compare_exchange_strong(&open_dev, &x, NULL);
        const long u = atomic_load(&o->underruns);
        if (u) olog("%s: ran short %ld time%s while it played (silence)", o->name, u, u > 1 ? "s" : "");
    }
    ca_free(o);
}
int out_takes(const char *dev, int fmt, unsigned rate) {
    if (is_null(dev)) return fmt == OUTF_S32 || fmt == OUTF_DSD_U32_LE;
    if (fmt != OUTF_S32) return 0;                     /* no native DSD through Core Audio */
    const AudioObjectID d = dev_find(dev);
    if (d == kAudioObjectUnknown) return 0;
    const pid_t h = dev_hog(d);
    if (h > 0 && h != getpid()) return -1;             /* held: cannot be asked now */
    char uid[160];
    pstr(d, kAudioDevicePropertyDeviceUID, uid, sizeof uid);
    if (dev_offers(d, uid, rate)) return 1;
    unsigned c[N_STD];
    return rate_candidates(d, uid, rate, c) > 0 || dev_rate(d) >= 8000 ? 2 : 0;
}
const char *out_strerror(long e) { return e == OUT_AGAIN ? "busy" : strerror((int)(e < 0 ? -e : e)); }
const char *out_fmt_name(int fmt) { return fmt == OUTF_DSD_U32_LE ? "DSD_U32_LE" : fmt == OUTF_DSD_U32_BE ? "DSD_U32_BE" : "S32_LE"; }
int out_recover(out_pcm *o, long e) { (void)o; (void)e; return -1; }   /* Core Audio: closed and opened again */
unsigned out_device_rate(out_pcm *o) { return o && !o->null && o->resample ? o->drate : 0; }
int out_bits(out_pcm *o) { return !o || o->null ? 32 : o->bits; }   /* null: all 32 (the tests compare bit for bit) */
const char *out_device_name(out_pcm *o) { return !o ? "" : o->null ? "null" : o->name; }
const char *out_device_maker(out_pcm *o) { return !o || o->null ? "" : o->maker; }
const char *out_device_model(out_pcm *o) { return !o || o->null ? "" : o->model; }
const char *out_default_dev(void) { return "default"; }
int out_dev_present(const char *dev) {
    if (!dev || !*dev || is_null(dev)) return 1;
    return dev_find(dev) != kAudioObjectUnknown;
}
/* A Core Audio UID: printable, nothing the control file or the page's JSON cannot hold. */
int out_card_ok(const char *v) {
    if (!v[0] || strlen(v) > 150) return 0;
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) if (*p < 0x20 || *p == '"' || *p == '\\' || *p == '%' || *p == 0x7f) return 0;
    return 1;
}

/* ── The lists: the page's (out_list_json), the app's (out_print_devices) ─────────────────────────── */
static size_t json_str(char *dst, size_t len, const char *s) {   /* a JSON string's inside, UTF-8 kept */
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && w + 7 < len; p++) {
        if (*p == '"' || *p == '\\') { dst[w++] = '\\'; dst[w++] = (char)*p; }
        else if (*p < 0x20) w += (size_t)snprintf(dst + w, len - w, "\\u%04x", *p);
        else dst[w++] = (char)*p;
    }
    dst[w] = 0;
    return w;
}
static const char *transport_name(UInt32 t) {
    switch (t) {
    case kAudioDeviceTransportTypeBuiltIn: return "Built-in";
    case kAudioDeviceTransportTypeUSB: return "USB";
    case kAudioDeviceTransportTypeHDMI: return "HDMI";
    case kAudioDeviceTransportTypeDisplayPort: return "DisplayPort";
    case kAudioDeviceTransportTypeBluetooth: case kAudioDeviceTransportTypeBluetoothLE: return "Bluetooth";
    case kAudioDeviceTransportTypeAirPlay: return "AirPlay";
    case kAudioDeviceTransportTypeThunderbolt: return "Thunderbolt";
    case kAudioDeviceTransportTypeFireWire: return "FireWire";
    case kAudioDeviceTransportTypeAggregate: return "Aggregate";
    case kAudioDeviceTransportTypeVirtual: return "Virtual";
    case kAudioDeviceTransportTypeAVB: return "AVB";
    case kAudioDeviceTransportTypePCI: return "PCI";
    default: return "Core Audio";
    }
}
/* The page asks twice a second: read again 5 s apart at most (squeeze.swift cachedOutputs). Every output device,
 * BlackHole and the like included (a loopback is a way to another program); the network outputs are netout.c's. */
void out_list_json(char *out, size_t len) {
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    static char cache[8192] = "[]";
    static double at = 0;
    pthread_mutex_lock(&lk);
    const double now = mono();
    if (!at || now - at >= 5) {
        at = now;
        size_t w = (size_t)snprintf(cache, sizeof cache, "[");
        AudioObjectID ids[128];
        const int n = dev_ids(ids, 128);
        for (int i = 0; i < n; i++) {
            if (dev_out_channels(ids[i]) <= 0) continue;
            char uid[256], name[256], man[256], ju[600], jn[600], jc[600], item[2000];
            pstr(ids[i], kAudioDevicePropertyDeviceUID, uid, sizeof uid);
            if (!out_card_ok(uid)) continue;           /* not one the page could set */
            pstr(ids[i], kAudioObjectPropertyName, name, sizeof name);
            pstr(ids[i], kAudioObjectPropertyManufacturer, man, sizeof man);
            if (!man[0]) { UInt32 t = 0; pget(ids[i], kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, &t, sizeof t); snprintf(man, sizeof man, "%s", transport_name(t)); }
            json_str(ju, sizeof ju, uid); json_str(jn, sizeof jn, name[0] ? name : uid); json_str(jc, sizeof jc, man);
            const int k = snprintf(item, sizeof item, "%s{\"id\":\"%s%s\",\"card\":\"%s\",\"name\":\"%s\"}", w > 1 ? "," : "", OUT_CARD_PREFIX, ju, jc, jn);
            if (k > 0 && w + (size_t)k + 2 < sizeof cache) { memcpy(cache + w, item, (size_t)k); w += (size_t)k; }
        }
        snprintf(cache + w, sizeof cache - w, "]");
    }
    snprintf(out, len, "%s", cache);
    pthread_mutex_unlock(&lk);
}
/* --list-devices: what the Swift player printed for the app (src/player.js devices()), field for field: every Core
 * Audio output, its name, UID, highest rate among Roon's (44.1 to 192 kHz; 48000 when it lists none of them) and
 * whether it is the default one. No network output (the page lists those, netout.c). */
void out_print_devices(void) {
    static const unsigned roon_rates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
    AudioObjectID ids[128];
    const int n = dev_ids(ids, 128);
    const AudioObjectID def = dev_default();
    int first = 1;
    fputc('[', stdout);
    for (int i = 0; i < n; i++) {
        if (dev_out_channels(ids[i]) <= 0) continue;
        char uid[256], name[256], ju[600], jn[600];
        pstr(ids[i], kAudioDevicePropertyDeviceUID, uid, sizeof uid);
        pstr(ids[i], kAudioObjectPropertyName, name, sizeof name);
        unsigned max = 0;
        for (size_t k = 0; k < sizeof roon_rates / sizeof *roon_rates; k++) if (dev_lists(ids[i], roon_rates[k])) max = roon_rates[k];
        json_str(ju, sizeof ju, uid); json_str(jn, sizeof jn, name[0] ? name : "?");
        printf("%s{\"name\":\"%s\",\"uid\":\"%s\",\"maxRate\":%u,\"default\":%s}", first ? "" : ",", jn, ju, max ? max : 48000, ids[i] == def ? "true" : "false");
        first = 0;
    }
    puts("]");
    fflush(stdout);
}
/* --exclusive on|off (the default on), --shared on (the same as --exclusive off), --integer on|off, and for the tests
 * --test-refuse-rates 48000,96000 (those rates as if the device did not list them: resampled, test/mac-roomd-out.py). */
int out_option(const char *name, const char *value) {
    const int on = value && (!strcmp(value, "on") || !strcmp(value, "1") || !strcmp(value, "yes"));
    if (!strcmp(name, "--exclusive")) { opt_exclusive = on; return 1; }
    if (!strcmp(name, "--shared")) { opt_exclusive = !on; return 1; }
    if (!strcmp(name, "--integer")) { opt_integer = on; return 1; }
    if (!strcmp(name, "--test-refuse-rates")) {
        n_refused_test = 0;
        for (const char *p = value ? value : ""; *p && n_refused_test < 16;) {
            const long r = strtol(p, (char **)&p, 10);
            if (r > 0) refused_test[n_refused_test++] = (unsigned)r;
            while (*p && (*p < '0' || *p > '9')) p++;
        }
        return 1;
    }
    return 0;
}
