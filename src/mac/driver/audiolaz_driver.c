/* src/mac/driver/audiolaz_driver.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../player/audiolaz_drv.h"

#define ID_PLUGIN kAudioObjectPlugInObject
#define ID_DEVICE 2
#define ID_STREAM 3
#define ID_VOLUME 4
#define VOL_MIN_DB (-60.0f)                /* the control's bottom: silence (AZ_VOL_FLOOR_CDB) */
#define DEV_UID "audiolaz_UID"
#define FACTORY_UUID "FDC7DBA7-A0F4-4A29-B376-134F0B61310D"

static const Float64 RATES[] = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000 };
#define NRATES (sizeof RATES / sizeof RATES[0])

static AudioServerPlugInHostRef host;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static UInt32 refs = 0;
static Float64 rate = 44100, pending_rate = 0;
static UInt32 io_clients = 0;
static double host_hz;                 /* host ticks a second */
static double ns_per_tick;
static Float64 zts_sample; static UInt64 zts_host; static UInt64 zts_seed = 1;
static az_shm *shm;
static uint64_t w_last;
static int zts_pub;                    /* the zero time stamps published since StartIO */
static az_info *info;                  /* the device's identity, latency and volume (audiolaz_drv.h) */

/* ── the shared memory ─────────────────────────────────────────────────────────────────────────────────────────── */
static void shm_open_once(void) {
    if (shm) return;
    const int fd = shm_open(az_shm_name(), O_RDWR | O_CREAT, 0666);
    if (fd < 0) return;
    fchmod(fd, 0666);                  /* the player runs as the user, coreaudiod as _coreaudiod */
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size < (off_t)sizeof(az_shm) && ftruncate(fd, sizeof(az_shm)) != 0) { close(fd); return; }
    void *m = mmap(NULL, sizeof(az_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return;
    shm = m;
    shm->ch = AZ_CH; shm->ring = AZ_RING; shm->magic = AZ_MAGIC;
    atomic_store(&shm->rate, (uint32_t)rate);
}
/* the identity's memory, made as the ring's is; what it holds kept (the player's last DAC, the volume) */
static void info_open_once(void) {
    if (info) return;
    const int fd = shm_open(az_info_name(), O_RDWR | O_CREAT, 0666);
    if (fd < 0) return;
    fchmod(fd, 0666);
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size < (off_t)sizeof(az_info) && ftruncate(fd, sizeof(az_info)) != 0) { close(fd); return; }
    void *m = mmap(NULL, sizeof(az_info), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return;
    info = m;
    info->magic = AZ_INFO_MAGIC;
}
/* the DAC as the player wrote it, read under its seqlock ("" none) */
static void info_dac(char *name, char *maker, char *model, uint32_t *local) {
    name[0] = maker[0] = model[0] = 0; *local = 0;
    if (!info) return;
    for (int i = 0; i < 100; i++) {
        const uint32_t a = atomic_load_explicit(&info->seq, memory_order_acquire);
        if (a & 1) continue;
        memcpy(name, info->name, AZ_INFO_TEXT); memcpy(maker, info->maker, AZ_INFO_TEXT); memcpy(model, info->model, AZ_INFO_TEXT);
        *local = info->local;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&info->seq, memory_order_relaxed) == a) break;
    }
    name[AZ_INFO_TEXT - 1] = maker[AZ_INFO_TEXT - 1] = model[AZ_INFO_TEXT - 1] = 0;
}

static CFStringRef info_cf(const char *s, int shown, const char *fallback) {
    char b[AZ_SHOWN_MAX];
    if (shown) { az_shown(b, sizeof b, s); s = b; }
    CFStringRef r = s[0] ? CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8) : NULL;
    return r ? r : CFStringCreateWithCString(NULL, fallback, kCFStringEncodingUTF8);
}
/* the volume control: scalar (0 … 1, linear in dB: the Mac's slider even in loudness) and dB; the bottom is silence */
static Float32 vol_db(void) { const uint32_t c = info ? atomic_load(&info->vol_cdb) : 0; return c >= AZ_VOL_FLOOR_CDB ? VOL_MIN_DB : -(Float32)c / 100.0f; }
static Float32 db_scalar(Float32 db) { return db <= VOL_MIN_DB ? 0.0f : db >= 0 ? 1.0f : 1.0f - db / VOL_MIN_DB; }
static Float32 scalar_db(Float32 x) { return x <= 0 ? VOL_MIN_DB : x >= 1 ? 0.0f : VOL_MIN_DB * (1.0f - x); }
static void vol_set_db(Float32 db, int tell) {
    if (!info) return;
    if (!(db >= VOL_MIN_DB)) db = VOL_MIN_DB;         /* NaN too */
    if (db > 0) db = 0;
    const uint32_t c = (uint32_t)lroundf(-db * 100.0f);
    if (atomic_exchange(&info->vol_cdb, c) == c) return;
    atomic_fetch_add(&info->vseq, 1);
    if (!tell || !host) return;                       /* tell: set by an app (kept, the apps told); not: restored */
    const SInt32 v = (SInt32)c;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    if (n) { host->WriteToStorage(host, CFSTR("vol_cdb"), n); CFRelease(n); }
    const AudioObjectPropertyAddress a[2] = { { kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                                              { kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain } };
    host->PropertiesChanged(host, ID_VOLUME, 2, a);
}
/* the player's changes told to the apps: its DAC (name, maker, model), its latency; every 250 ms */
static void *watch_main(void *arg) {
    (void)arg;
    uint32_t seen_seq = 0, seen_lat = 0;
    int first = 1;
    if (info) { first = 0; seen_seq = atomic_load(&info->seq); seen_lat = atomic_load(&info->latency); }   /* as it is now */
    for (;;) {
        usleep(250000);
        if (!info) { info_open_once(); continue; }
        const uint32_t q = atomic_load(&info->seq), l = atomic_load(&info->latency);
        if (first) { first = 0; seen_seq = q; seen_lat = l; continue; }
        if (!host || (q & 1)) continue;
        if (q != seen_seq) {
            seen_seq = q;
            const AudioObjectPropertyAddress a[3] = { { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                                                      { kAudioObjectPropertyManufacturer, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                                                      { kAudioDevicePropertyModelUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain } };
            host->PropertiesChanged(host, ID_DEVICE, 3, a);
        }
        if (l != seen_lat) {
            seen_lat = l;
            const AudioObjectPropertyAddress a = { kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
            host->PropertiesChanged(host, ID_DEVICE, 1, &a);
        }
    }
    return NULL;
}

/* the device's host ticks a frame now: the player's clock while its beat is fresh, the host's otherwise */
static double tpf_now(UInt64 now) {
    int64_t ppb = 0;
    if (shm) {
        const uint64_t b = atomic_load(&shm->beat);
        if (b && now >= b && (double)(now - b) * ns_per_tick < (double)AZ_BEAT_NS) ppb = atomic_load(&shm->ppb);
    }
    if (ppb > 2000000) ppb = 2000000; else if (ppb < -2000000) ppb = -2000000;   /* ±2000 ppm at most */
    return az_tpf(host_hz, (uint32_t)rate, ppb);
}

/* ── IUnknown, the plug-in's lifetime ─────────────────────────────────────────────────────────────────────────── */
static AudioServerPlugInDriverInterface iface;
static AudioServerPlugInDriverInterface *iface_p = &iface;
static AudioServerPlugInDriverRef self_ref = &iface_p;

static HRESULT QueryInterface(void *d, REFIID iid, LPVOID *out) {
    if (d != self_ref || !out) return kAudioHardwareBadObjectError;
    CFUUIDRef u = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(u, IUnknownUUID) || CFEqual(u, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&mu); refs++; pthread_mutex_unlock(&mu);
        *out = self_ref; r = S_OK;
    }
    CFRelease(u);
    return r;
}
static ULONG AddRef(void *d) { (void)d; pthread_mutex_lock(&mu); const ULONG r = ++refs; pthread_mutex_unlock(&mu); return r; }
static ULONG Release(void *d) { (void)d; pthread_mutex_lock(&mu); const ULONG r = refs ? --refs : 0; pthread_mutex_unlock(&mu); return r; }

static OSStatus Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef h) {
    if (d != self_ref) return kAudioHardwareBadObjectError;
    host = h;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    ns_per_tick = (double)tb.numer / tb.denom;
    host_hz = 1e9 / ns_per_tick;
    shm_open_once();
    info_open_once();                                  /* */
    CFPropertyListRef v = NULL;                        /* the volume as last set (the host's storage) */
    if (host->CopyFromStorage(host, CFSTR("vol_cdb"), &v) == 0 && v) {
        SInt32 c = 0;
        if (CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, &c) && c >= 0) vol_set_db(-(Float32)c / 100.0f, 0);
        CFRelease(v);
    }
    static int watching;
    if (!watching) {
        pthread_t th; pthread_attr_t ta; pthread_attr_init(&ta); pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);
        watching = pthread_create(&th, &ta, watch_main, NULL) == 0;
        pthread_attr_destroy(&ta);
    }
    return 0;
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *c, AudioObjectID *o) { (void)d; (void)desc; (void)c; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o) { (void)d; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) { (void)d; (void)o; (void)c; return 0; }
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) { (void)d; (void)o; (void)c; return 0; }

/* a rate asked: through the host (the IO stopped meanwhile), taken in PerformDeviceConfigurationChange */
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 action, void *info) {
    (void)d; (void)action; (void)info;
    if (o != ID_DEVICE) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&mu);
    if (pending_rate > 0) { rate = pending_rate; pending_rate = 0; }
    if (shm) { atomic_store(&shm->rate, (uint32_t)rate); atomic_fetch_add(&shm->gen, 1); }
    zts_seed++;
    pthread_mutex_unlock(&mu);
    return 0;
}
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) { (void)d; (void)o; (void)a; (void)i; pthread_mutex_lock(&mu); pending_rate = 0; pthread_mutex_unlock(&mu); return 0; }

/* ── properties ───────────────────────────────────────────────────────────────────────────────────────────────── */
static AudioStreamBasicDescription asbd(Float64 r) {
    AudioStreamBasicDescription f = { 0 };
    f.mSampleRate = r; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = f.mBytesPerFrame = 4 * AZ_CH; f.mFramesPerPacket = 1; f.mChannelsPerFrame = AZ_CH; f.mBitsPerChannel = 32;
    return f;
}
static int known_rate(Float64 r) { for (size_t i = 0; i < NRATES; i++) if (RATES[i] == r) return 1; return 0; }

/* what a property is: its size (out NULL) or its data, into out (room bytes) */
#define PUT(T, v) do { if (out) { if (room < sizeof(T)) return kAudioHardwareBadPropertySizeError; *(T *)out = (v); } *size = sizeof(T); return 0; } while (0)
#define PUTCF(s) PUT(CFStringRef, (out ? CFRetain(CFSTR(s)) : NULL))
/* a list of objects: as many as fit */
#define PUTIDS(...) do { const AudioObjectID ids_[] = { __VA_ARGS__ }; UInt32 n_ = (UInt32)sizeof ids_; \
    if (out) { if (room < n_) n_ = room / (UInt32)sizeof(AudioObjectID) * (UInt32)sizeof(AudioObjectID); memcpy(out, ids_, n_); } \
    *size = n_; return 0; } while (0)
/* the DAC's identity: a string made here (the caller releases it), the player's or ours */
#define PUTDAC(which, shown, fallback) do { if (out) { char n_[AZ_INFO_TEXT], m_[AZ_INFO_TEXT], u_[AZ_INFO_TEXT]; uint32_t l_; \
    info_dac(n_, m_, u_, &l_); (void)l_; PUT(CFStringRef, info_cf(which == 0 ? n_ : which == 1 ? m_ : u_, (shown), fallback)); } \
    PUT(CFStringRef, NULL); } while (0)
static OSStatus prop(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 room, UInt32 *size, void *out) {
    const AudioObjectPropertySelector s = a->mSelector;
    if (o == ID_PLUGIN) {
        switch (s) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: PUTCF("audiolaz");
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: PUT(AudioObjectID, ID_DEVICE);
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            AudioObjectID id = kAudioObjectUnknown;
            if (qs == sizeof(CFStringRef) && q && *(CFStringRef const *)q && CFStringCompare(*(CFStringRef const *)q, CFSTR(DEV_UID), 0) == kCFCompareEqualTo) id = ID_DEVICE;
            PUT(AudioObjectID, id);
        }
        case kAudioPlugInPropertyResourceBundle: PUTCF("");
        }
        return kAudioHardwareUnknownPropertyError;
    }
    if (o == ID_DEVICE) {
        switch (s) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, ID_PLUGIN);
        case kAudioObjectPropertyName: PUTDAC(0, 1, "audiolaz");              /* the DAC behind it */
        case kAudioObjectPropertyManufacturer: PUTDAC(1, 0, "audiolaz");
        case kAudioDevicePropertyDeviceUID: PUTCF(DEV_UID);                  /* never changes: the apps' settings follow it */
        case kAudioDevicePropertyModelUID: PUTDAC(2, 0, "audiolaz_Model_UID");
        case kAudioDevicePropertyTransportType: PUT(UInt32, kAudioDeviceTransportTypeVirtual);
        case kAudioDevicePropertyRelatedDevices: PUT(AudioObjectID, ID_DEVICE);
        case kAudioDevicePropertyClockDomain: PUT(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: PUT(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: PUT(UInt32, io_clients > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: PUT(UInt32, a->mScope != kAudioObjectPropertyScopeInput);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, 0);
        case kAudioDevicePropertyLatency:                                    /* what the player adds after it */
            PUT(UInt32, a->mScope == kAudioObjectPropertyScopeInput || !info ? 0 : atomic_load(&info->latency));
        case kAudioDevicePropertySafetyOffset: PUT(UInt32, 0);
        case kAudioDevicePropertyIsHidden: PUT(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, AZ_ZTS_PERIOD);
        case kAudioDevicePropertyNominalSampleRate: PUT(Float64, rate);
        case kAudioObjectPropertyOwnedObjects:
            if (a->mScope == kAudioObjectPropertyScopeInput) { *size = 0; return 0; }
            PUTIDS(ID_STREAM, ID_VOLUME);
        case kAudioDevicePropertyStreams:
            if (a->mScope == kAudioObjectPropertyScopeInput) { *size = 0; return 0; }
            PUT(AudioObjectID, ID_STREAM);
        case kAudioObjectPropertyControlList: PUTIDS(ID_VOLUME);
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            const UInt32 n = (UInt32)(NRATES * sizeof(AudioValueRange));
            if (out) {
                UInt32 k = room / sizeof(AudioValueRange);
                if (k > NRATES) k = NRATES;
                for (UInt32 i = 0; i < k; i++) ((AudioValueRange *)out)[i] = (AudioValueRange){ RATES[i], RATES[i] };
                *size = k * (UInt32)sizeof(AudioValueRange);
            } else *size = n;
            return 0;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo:
            if (out) { if (room < 8) return kAudioHardwareBadPropertySizeError; ((UInt32 *)out)[0] = 1; ((UInt32 *)out)[1] = 2; }
            *size = 8; return 0;
        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 n = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + AZ_CH * sizeof(AudioChannelDescription));
            if (out) {
                if (room < n) return kAudioHardwareBadPropertySizeError;
                AudioChannelLayout *l = out; memset(l, 0, n);
                l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions; l->mNumberChannelDescriptions = AZ_CH;
                l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
                l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            }
            *size = n; return 0;
        }
        }
        return kAudioHardwareUnknownPropertyError;
    }
    if (o == ID_STREAM) {
        switch (s) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, ID_DEVICE);
        case kAudioObjectPropertyName: PUTCF("audiolaz");
        case kAudioObjectPropertyOwnedObjects: *size = 0; return 0;
        case kAudioStreamPropertyIsActive: PUT(UInt32, 1);
        case kAudioStreamPropertyDirection: PUT(UInt32, 0);
        case kAudioStreamPropertyTerminalType: PUT(UInt32, kAudioStreamTerminalTypeSpeaker);
        case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1);
        case kAudioStreamPropertyLatency: PUT(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: PUT(AudioStreamBasicDescription, asbd(rate));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            if (out) {
                UInt32 k = room / sizeof(AudioStreamRangedDescription);
                if (k > NRATES) k = NRATES;
                for (UInt32 i = 0; i < k; i++) ((AudioStreamRangedDescription *)out)[i] = (AudioStreamRangedDescription){ asbd(RATES[i]), { RATES[i], RATES[i] } };
                *size = k * (UInt32)sizeof(AudioStreamRangedDescription);
            } else *size = (UInt32)(NRATES * sizeof(AudioStreamRangedDescription));
            return 0;
        }
        }
        return kAudioHardwareUnknownPropertyError;
    }
    if (o == ID_VOLUME) {                              /* the device's volume (the player applies it) */
        switch (s) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, ID_DEVICE);
        case kAudioObjectPropertyOwnedObjects: *size = 0; return 0;
        case kAudioControlPropertyScope: PUT(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: PUT(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioLevelControlPropertyScalarValue: PUT(Float32, db_scalar(vol_db()));
        case kAudioLevelControlPropertyDecibelValue: PUT(Float32, vol_db());
        case kAudioLevelControlPropertyDecibelRange: PUT(AudioValueRange, ((AudioValueRange){ VOL_MIN_DB, 0 }));
        case kAudioLevelControlPropertyConvertScalarToDecibels:   /* in and out through the same Float32 */
            if (out && room >= sizeof(Float32)) *(Float32 *)out = scalar_db(*(Float32 *)out);
            else if (out) return kAudioHardwareBadPropertySizeError;
            *size = sizeof(Float32); return 0;
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            if (out && room >= sizeof(Float32)) *(Float32 *)out = db_scalar(*(Float32 *)out);
            else if (out) return kAudioHardwareBadPropertySizeError;
            *size = sizeof(Float32); return 0;
        }
        return kAudioHardwareUnknownPropertyError;
    }
    return kAudioHardwareBadObjectError;
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a) {
    (void)d; (void)pid;
    UInt32 n = 0;
    if (a->mSelector == kAudioPlugInPropertyTranslateUIDToDevice) return o == ID_PLUGIN;
    return prop(o, a, 0, NULL, 0, &n, NULL) == 0;
}
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    if (!HasProperty(d, o, pid, a)) return kAudioHardwareUnknownPropertyError;
    const AudioObjectPropertySelector s = a->mSelector;
    *out = (o == ID_DEVICE && s == kAudioDevicePropertyNominalSampleRate)
        || (o == ID_STREAM && (s == kAudioStreamPropertyVirtualFormat || s == kAudioStreamPropertyPhysicalFormat))
        || (o == ID_VOLUME && (s == kAudioLevelControlPropertyScalarValue || s == kAudioLevelControlPropertyDecibelValue));
    return 0;
}
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *size) {
    (void)d; (void)pid;
    return prop(o, a, qs, q, 0, size, NULL);
}
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 room, UInt32 *size, void *out) {
    (void)d; (void)pid;
    if (!out) return kAudioHardwareIllegalOperationError;
    return prop(o, a, qs, q, room, size, out);
}
static OSStatus ask_rate(Float64 r) {
    if (!known_rate(r)) return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&mu);
    const int same = r == rate;
    if (!same) pending_rate = r;
    pthread_mutex_unlock(&mu);
    if (!same && host) host->RequestDeviceConfigurationChange(host, ID_DEVICE, 0, NULL);
    return 0;
}
static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 size, const void *in) {
    (void)d; (void)pid; (void)qs; (void)q;
    if (o == ID_DEVICE && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (size < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return ask_rate(*(const Float64 *)in);
    }
    if (o == ID_VOLUME && (a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue)) {
        if (size < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
        if (!info) return kAudioHardwareNotRunningError;   /* no memory to give the player: nothing would change */
        const Float32 v = *(const Float32 *)in;
        vol_set_db(a->mSelector == kAudioLevelControlPropertyScalarValue ? scalar_db(v) : v, 1);
        return 0;
    }
    if (o == ID_STREAM && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        if (size < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription *f = in;
        const AudioStreamBasicDescription want = asbd(f->mSampleRate);
        if (f->mFormatID != want.mFormatID || f->mChannelsPerFrame != AZ_CH || f->mBitsPerChannel != 32 || !(f->mFormatFlags & kAudioFormatFlagIsFloat))
            return kAudioDeviceUnsupportedFormatError;
        return ask_rate(f->mSampleRate);
    }
    return kAudioHardwareUnknownPropertyError;
}

/* ── IO ───────────────────────────────────────────────────────────────────────────────────────────────────────── */
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o != ID_DEVICE) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&mu);
    if (io_clients++ == 0) {
        shm_open_once();
        zts_sample = 0; zts_host = mach_absolute_time(); zts_seed++; zts_pub = 0;
        w_last = 0;
        if (shm) {
            atomic_store(&shm->w, 0);
            atomic_store(&shm->rate, (uint32_t)rate);
            atomic_fetch_add(&shm->gen, 1);
            atomic_store(&shm->running, 1);
        }
    }
    pthread_mutex_unlock(&mu);
    return 0;
}
static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o != ID_DEVICE) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&mu);
    if (io_clients && --io_clients == 0 && shm) atomic_store(&shm->running, 0);
    pthread_mutex_unlock(&mu);
    return 0;
}
/* the period's zero time stamps: each AZ_ZTS_PERIOD frames, as many host ticks as the player's clock makes them */
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, Float64 *sample, UInt64 *ht, UInt64 *seed) {
    (void)d; (void)client;
    if (o != ID_DEVICE) return kAudioHardwareBadObjectError;
    /* no lock: the IO thread alone calls it, its state set with the IO stopped (StartIO, a rate change) */
    const UInt64 now = mach_absolute_time();
    const double tpf = tpf_now(now);
    int moved = !zts_pub;
    for (int i = 0; i < 64; i++) {
        const UInt64 next = zts_host + (UInt64)(AZ_ZTS_PERIOD * tpf);
        if (now < next) break;
        zts_host = next; zts_sample += AZ_ZTS_PERIOD; moved = 1;
    }
    if (moved && shm) {                                /* published for the player (audiolaz_drv.h: az_now) */
        atomic_fetch_add_explicit(&shm->seq, 1, memory_order_acq_rel);
        shm->zs = zts_sample; shm->zh = zts_host; shm->tpf = tpf;
        atomic_fetch_add_explicit(&shm->seq, 1, memory_order_release);
        zts_pub = 1;
    }
    *sample = zts_sample; *ht = zts_host; *seed = zts_seed;
    return 0;
}
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, Boolean *will, Boolean *inplace) {
    (void)d; (void)o; (void)client;
    *will = op == kAudioServerPlugInIOOperationWriteMix; *inplace = true;
    return 0;
}
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *i) { (void)d; (void)o; (void)c; (void)op; (void)n; (void)i; return 0; }
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *i) { (void)d; (void)o; (void)c; (void)op; (void)n; (void)i; return 0; }

/* the HAL's mix (float32 stereo) at its sample time into the ring; a gap since the last write (frames skipped) as silence */
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, AudioObjectID st, UInt32 client, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *info, void *main, void *second) {
    (void)d; (void)st; (void)client; (void)second;
    if (o != ID_DEVICE) return kAudioHardwareBadObjectError;
    if (op != kAudioServerPlugInIOOperationWriteMix || !shm || !main) return 0;
    const double t = info->mOutputTime.mSampleTime;
    if (t < 0) return 0;
    const uint64_t at = (uint64_t)t;
    const float *f = main;
    if (at > w_last) {
        uint64_t gap = at - w_last;
        if (gap > AZ_RING) gap = AZ_RING;
        for (uint64_t i = at - gap; i < at; i++) memset(shm->data + AZ_CH * (i & (AZ_RING - 1)), 0, sizeof(float) * AZ_CH);
    }
    for (UInt32 i = 0; i < n; i++) memcpy(shm->data + AZ_CH * ((at + i) & (AZ_RING - 1)), f + AZ_CH * i, sizeof(float) * AZ_CH);
    atomic_store_explicit(&shm->cycle, n, memory_order_relaxed);
    if (at + n > w_last) { w_last = at + n; atomic_store_explicit(&shm->w, w_last, memory_order_release); }
    return 0;
}

static AudioServerPlugInDriverInterface iface = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformDeviceConfigurationChange, AbortDeviceConfigurationChange, HasProperty, IsPropertySettable, GetPropertyDataSize,
    GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation,
    EndIOOperation
};

/* the factory (Info.plist's CFPlugInFactories) */
__attribute__((visibility("default"))) void *audiolaz_Create(CFAllocatorRef alloc, CFUUIDRef type) {
    (void)alloc;
    if (!CFEqual(type, kAudioServerPlugInTypeUUID)) return NULL;
    pthread_mutex_lock(&mu); refs++; pthread_mutex_unlock(&mu);
    return self_ref;
}
