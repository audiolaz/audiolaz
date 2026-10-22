

#include <CoreAudio/AudioServerPlugIn.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../src/player/src_plat.h"
#include "../src/player/src.h"
#include "../src/player/audiolaz_drv.h"

#define K 2                            /* the input */
static int fails;
static void check(int ok, const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    printf("%s  ", ok ? "ok  " : "FAIL"); vprintf(fmt, a); printf("\n");
    va_end(a);
    if (!ok) fails++;
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void nap(double s) { struct timespec t = { (time_t)s, (long)((s - (time_t)s) * 1e9) }; nanosleep(&t, NULL); }

static AudioServerPlugInDriverRef drv;
/* what the driver told the apps (from its watcher thread too), and the host's storage (its volume) */
static _Atomic int n_name, n_lat, n_vol;
static CFPropertyListRef stored;
static OSStatus h_changed(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress *a) {
    (void)h;
    for (UInt32 i = 0; i < n; i++) {
        if (o == 2 && a[i].mSelector == kAudioObjectPropertyName) n_name++;
        if (o == 2 && a[i].mSelector == kAudioDevicePropertyLatency) n_lat++;
        if (o == 4 && a[i].mSelector == kAudioLevelControlPropertyScalarValue) n_vol++;
    }
    return 0;
}
static OSStatus h_copy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *d) { (void)h; *d = stored && CFEqual(k, CFSTR("vol_cdb")) ? CFRetain(stored) : NULL; return 0; }
static OSStatus h_write(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef d) { (void)h; if (CFEqual(k, CFSTR("vol_cdb"))) { if (stored) CFRelease(stored); stored = CFRetain(d); } return 0; }
static OSStatus h_delete(AudioServerPlugInHostRef h, CFStringRef k) { (void)h; (void)k; return 0; }
static OSStatus h_request(AudioServerPlugInHostRef h, AudioObjectID o, UInt64 a, void *i) { (void)h; return (*drv)->PerformDeviceConfigurationChange(drv, o, a, i); }
static AudioServerPlugInHostInterface host = { h_changed, h_copy, h_write, h_delete, h_request };

static UInt32 get(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope sc, void *out, UInt32 room) {
    AudioObjectPropertyAddress a = { s, sc, kAudioObjectPropertyElementMain };
    UInt32 n = 0;
    if ((*drv)->GetPropertyData(drv, o, 0, &a, 0, NULL, room, &n, out)) return 0;
    return n;
}
static int set_f32(AudioObjectID o, AudioObjectPropertySelector s, Float32 v) {
    AudioObjectPropertyAddress a = { s, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    return (*drv)->SetPropertyData(drv, o, 0, &a, 0, NULL, sizeof v, &v) == 0;
}
static Float32 get_f32(AudioObjectID o, AudioObjectPropertySelector s) { Float32 v = NAN; get(o, s, kAudioObjectPropertyScopeGlobal, &v, sizeof v); return v; }
static int str_is(AudioObjectPropertySelector s, const char *want) {
    CFStringRef c = NULL;
    get(2, s, kAudioObjectPropertyScopeGlobal, &c, sizeof c);
    char b[160] = "";
    if (c) { CFStringGetCString(c, b, sizeof b, kCFStringEncodingUTF8); CFRelease(c); }
    if (strcmp(b, want)) fprintf(stderr, "  (« %s », « %s » expected)\n", b, want);
    return !strcmp(b, want);
}
static int told(_Atomic int *n, int was, double s) { const double t = now_s(); while (now_s() - t < s && atomic_load(n) == was) nap(0.01); return atomic_load(n) != was; }
double plat_src_volume(int k);
void plat_src_dac(const char *name, const char *maker, const char *model, int local);
void plat_src_latency(int k, double ms);
static int set_rate(Float64 r) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    return (*drv)->SetPropertyData(drv, 2, 0, &a, 0, NULL, sizeof r, &r) == 0;
}

/* the HAL's side: its IO thread writing at the device's clock, as its zero time stamps say */
static volatile int w_run;
static double host_hz;
static az_shm *shm;                    /* the driver's memory, as the tests see it */
static unsigned cur_rate;
static volatile int dop_mode;          /* DSD as DoP: markers 0x05 / 0xFA in each sample's top byte */
static void *writer(void *arg) {
    (void)arg;
    double tpf = host_hz / cur_rate, pzs = -1; UInt64 pzh = 0;
    uint64_t ws = 0;
    static float buf[8192 * 2];
    const double zero_after = getenv("AZ_ZERO_AFTER") ? atof(getenv("AZ_ZERO_AFTER")) : 0;   /* --write: silence from then (s) */
    const int cyc = getenv("AZ_CYCLE") ? atoi(getenv("AZ_CYCLE")) : 128;   /* the HAL's IO cycle (frames) */
    if (getenv("AZ_RT")) {   /* in Mach real time, as the HAL's own IO thread (test/mac-link.sh: no stall of the stand-in) */
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        const double ms = 1e6 * tb.denom / tb.numer;
        thread_time_constraint_policy_data_t p = { (uint32_t)(1 * ms), (uint32_t)(0.3 * ms), (uint32_t)(1 * ms), 1 };
        thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    }
    while (w_run) {
        Float64 zs; UInt64 zh, seed;
        (*drv)->GetZeroTimeStamp(drv, 2, 0, &zs, &zh, &seed);
        if (pzs >= 0 && zs > pzs) tpf = (double)(zh - pzh) / (zs - pzs);
        if (zs != pzs) { pzs = zs; pzh = zh; }
        const double at = zs + (double)(mach_absolute_time() - zh) / tpf;
        while ((double)ws < at + 2.0 * cyc) {               /* as the HAL: each cycle written one cycle ahead of its time */
            for (int i = 0; i < cyc; i++) {
                const int32_t c = (int32_t)((ws + (uint64_t)i) & 0x7FFFFF);
                buf[2 * i] = (float)c / 8388608.0f; buf[2 * i + 1] = -(float)c / 8388608.0f;
                if (zero_after > 0 && (double)ws / cur_rate > zero_after) buf[2 * i] = buf[2 * i + 1] = 0;   /* an app paused, its IO held */
                if (dop_mode) {
                    int32_t v = (int32_t)((((ws + (uint64_t)i) & 1 ? 0xFAu : 0x05u) << 16) | ((ws + (uint64_t)i) & 0xFFFF));
                    if (v & 0x800000) v -= 0x1000000;
                    buf[2 * i] = buf[2 * i + 1] = (float)v / 8388608.0f;
                }
            }
            AudioServerPlugInIOCycleInfo ci = { 0 };
            ci.mOutputTime.mSampleTime = (Float64)ws;
            (*drv)->DoIOOperation(drv, 2, 3, 0, kAudioServerPlugInIOOperationWriteMix, (UInt32)cyc, &ci, buf, NULL);
            ws += (uint64_t)cyc;
        }
        nap(0.001);
    }
    return NULL;
}

/* one play at rate r for secs: the DAC (+dac_ppm) consuming, a PI regulator steering; what was read checked */
/* fixed (drift_check): the steer held (ppm, + slower), the device's pace measured against the host's */
static void play(unsigned r, double secs, double dac_ppm, int drift_check, double fixed) {
    check(set_rate(r), "%u Hz: the device's rate set", r);
    cur_rate = r;
    (*drv)->StartIO(drv, 2, 0);
    w_run = 1;
    pthread_t th; pthread_create(&th, NULL, writer, NULL);
    double t = now_s();
    while (now_s() - t < 1 && !(src_active(K) && src_rate(K) == r)) nap(0.002);
    check(src_active(K) && src_rate(K) == r, "%u Hz: the input active at its rate (%u) in %.0f ms", r, src_rate(K), (now_s() - t) * 1000);
    const unsigned long period = r / 200;
    src_pcm *p = src_open(K, r, 2, period, 8);
    check(p != NULL, "%u Hz: opened", r);
    if (!p) { w_run = 0; pthread_join(th, NULL); (*drv)->StopIO(drv, 2, 0); return; }
    src_start(p);
    const double target = 0.02 * r;            /* the level kept: 20 ms */
    long lvl = 0;
    while (src_delay(p, &lvl), lvl < target) nap(0.001);
    int32_t *pcm = malloc(sizeof(int32_t) * 2 * r);
    const double t0 = now_s();
    uint64_t consumed = 0, bad = 0, gaps = 0; int32_t last = -1;
    double integ = 0, tr = t0, g_n = 0, g_t = 0, g_l = 0, g_tt = 0, g_tl = 0;
    for (;;) {
        const double tn = now_s();
        if (tn - t0 >= secs) break;
        const uint64_t due = (uint64_t)((tn - t0) * r * (1 + dac_ppm * 1e-6));
        while (consumed < due) {
            unsigned long want = due - consumed; if (want > (unsigned long)r) want = r;
            const long got = src_read(p, pcm, want);
            if (got == SRC_AGAIN) break;
            if (got < 0) { src_recover(p, got); gaps++; break; }
            for (long i = 0; i < got; i++) {
                const int32_t L = pcm[2 * i], R = pcm[2 * i + 1];
                const int32_t c = L >> 8;
                if ((L & 0xFF) || R != -L || c < 0) bad++;
                if (last >= 0 && c != ((last + 1) & 0x7FFFFF)) gaps++;
                last = c;
            }
            consumed += (uint64_t)got;
        }
        if (tn - tr >= 0.05) {                 /* the regulator: the level's error steers the device's clock */
            const double dt = tn - tr; tr = tn;
            src_delay(p, &lvl);
            const double err = (lvl - target) / r;   /* s */
            integ += err * dt;
            double ppm = 30000 * err + 6000 * integ; /* + : the source slower */
            if (ppm > 2000) ppm = 2000; else if (ppm < -2000) ppm = -2000;
            if (drift_check) ppm = fixed;          /* the steer held: the device's pace measured against it */
            src_shift(K, 100000 + lrint(ppm / 11.0));
            if (getenv("AZ_DEBUG") && drift_check) fprintf(stderr, "t %.2f err %.2f ms ppm %.0f\n", tn - t0, err * 1000, ppm);
            /* what the device made (read + waiting) against the time, over the last half: its pace, the level's */
        }
        if (drift_check && tn - t0 > 1) {          /* the device's frames against the time (least squares): its pace */
            const double x = tn - t0, y = (double)atomic_load(&shm->w) / r - x;
            g_n++; g_t += x; g_l += y; g_tt += x * x; g_tl += x * y;
        }
        nap(0.002);
    }
    check(bad == 0 && gaps == 0 && consumed > 0.9 * secs * r, "%u Hz: %llu frames read bit for bit, in order (%llu wrong, %llu gaps)",
          r, (unsigned long long)consumed, (unsigned long long)bad, (unsigned long long)gaps);
    if (drift_check) {
        const double slope = (g_n * g_tl - g_t * g_l) / (g_n * g_tt - g_t * g_t), want = -fixed;
        check(fabs(slope * 1e6 - want) < 25,   /* 25 ppm: 0.2 ms in 9 s, the HAL stand-in writing 128 frames at a time */ "%u Hz, DAC %+.0f ppm, the device steered %+.0f ppm: its frames come %+.1f ppm fast (%+.0f expected)%s",
              r, dac_ppm, -fixed, slope * 1e6, want, fixed == -dac_ppm ? ": no drift" : "");
    }
    free(pcm);
    w_run = 0; pthread_join(th, NULL);
    (*drv)->StopIO(drv, 2, 0);
    t = now_s();
    while (now_s() - t < 1 && src_active(K)) nap(0.001);
    check(!src_active(K), "%u Hz: stopped with the IO, in %.0f ms", r, (now_s() - t) * 1000);
    src_close(p);
    src_shift(K, 100000);
}

static void dop_check(void) {
    const unsigned r = 176400;
    check(set_rate(r), "DoP: the device at %u Hz", r);
    cur_rate = r; dop_mode = 1;
    (*drv)->StartIO(drv, 2, 0);
    w_run = 1;
    pthread_t th; pthread_create(&th, NULL, writer, NULL);
    double t = now_s();
    while (now_s() - t < 1 && !(src_active(K) && src_rate(K) == r)) nap(0.002);
    src_pcm *p = src_open(K, r, 2, r / 200, 8);
    uint64_t got_n = 0, loud = 0;
    if (p) {
        src_start(p);
        int32_t *pcm = malloc(sizeof(int32_t) * 2 * r);
        t = now_s();
        while (now_s() - t < 0.6) {
            const long g = src_read(p, pcm, r / 10);
            if (g > 0) { for (long i = 0; i < g; i++) if (got_n + (uint64_t)i >= 256 && (pcm[2 * i] || pcm[2 * i + 1])) loud++; got_n += (uint64_t)g; }
            else if (g < 0 && g != SRC_AGAIN) src_recover(p, g);
            nap(0.002);
        }
        free(pcm);
    }
    check(p && got_n > r / 4 && loud == 0, "DoP: %llu frames read, silence past the first markers (%llu not)", (unsigned long long)got_n, (unsigned long long)loud);
    w_run = 0; pthread_join(th, NULL);
    (*drv)->StopIO(drv, 2, 0);
    t = now_s();
    while (now_s() - t < 1 && src_active(K)) nap(0.001);
    if (p) src_close(p);
    dop_mode = 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <audiolaz.driver>\n", argv[0]); return 2; }
    const int wmode = argc >= 4 && !strcmp(argv[2], "--write");   /* --write <s>: only the HAL stand-in (for a roomd) */
    char name[64]; snprintf(name, sizeof name, "/azt.%d", (int)getpid());
    if (wmode && getenv("AUDIOLAZ_SHM")) snprintf(name, sizeof name, "%s", getenv("AUDIOLAZ_SHM"));
    setenv("AUDIOLAZ_SHM", name, 1);
    char iname[64]; snprintf(iname, sizeof iname, "/azi.%d", (int)getpid());   /* its identity's memory */
    if (wmode && getenv("AUDIOLAZ_SHM_INFO")) snprintf(iname, sizeof iname, "%s", getenv("AUDIOLAZ_SHM_INFO"));
    setenv("AUDIOLAZ_SHM_INFO", iname, 1);
    char bin[1024]; snprintf(bin, sizeof bin, "%s/Contents/MacOS/audiolaz", argv[1]);
    void *h = dlopen(bin, RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    void *(*create)(CFAllocatorRef, CFUUIDRef) = (void *(*)(CFAllocatorRef, CFUUIDRef))dlsym(h, "audiolaz_Create");
    check(create != NULL, "the factory audiolaz_Create");
    if (!create) return 1;
    drv = create(NULL, kAudioServerPlugInTypeUUID);
    check(drv && (*drv)->Initialize(drv, &host) == 0, "created, initialized");
    const int sfd = shm_open(name, O_RDONLY);
    shm = sfd >= 0 ? mmap(NULL, sizeof *shm, PROT_READ, MAP_SHARED, sfd, 0) : MAP_FAILED;
    check(shm != MAP_FAILED && shm->magic == AZ_MAGIC, "its shared memory (%s)", name);
    if (shm == MAP_FAILED) return 1;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb); host_hz = 1e9 * tb.denom / tb.numer;
    if (wmode) {

        cur_rate = getenv("AZ_RATE") ? (unsigned)atoi(getenv("AZ_RATE")) : 44100;
        if (cur_rate != 44100) set_rate(cur_rate);
        if (getenv("AZ_VOL_DB")) set_f32(4, kAudioLevelControlPropertyDecibelValue, (Float32)atof(getenv("AZ_VOL_DB")));
        (*drv)->StartIO(drv, 2, 0); w_run = 1;
        pthread_t th; pthread_create(&th, NULL, writer, NULL);
        const double secs = atof(argv[3]), t_w0 = now_s();
        long long ppb_sum = 0, ppb_n = 0;
        double g_n = 0, g_t = 0, g_l = 0, g_tt = 0, g_tl = 0;   /* the device's frames against the time, its second half */
        while (now_s() - t_w0 < secs) {
            nap(0.002);                                /* many samples: the device writes a whole IO cycle at a time */
            const double x = now_s() - t_w0;
            if (x > secs / 2) {
                ppb_sum += atomic_load(&shm->ppb); ppb_n++;
                const double y = (double)atomic_load(&shm->w) / cur_rate - x;
                g_n++; g_t += x; g_l += y; g_tt += x * x; g_tl += x * y;
            }
        }
        const double pace = g_n > 2 ? (g_n * g_tl - g_t * g_l) / (g_n * g_tt - g_t * g_t) * 1e6 : 0;
        w_run = 0; pthread_join(th, NULL);
        CFStringRef nm = NULL; char b[160] = "";
        get(2, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &nm, sizeof nm);
        if (nm) { CFStringGetCString(nm, b, sizeof b, kCFStringEncodingUTF8); CFRelease(nm); }
        UInt32 lat = 0;
        get(2, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, &lat, sizeof lat);
        printf("WRITE name=%s latency=%u ppb=%lld pace=%+.1f frames=%llu\n", b, (unsigned)lat, ppb_n ? ppb_sum / ppb_n : 0LL, pace, (unsigned long long)atomic_load(&shm->w));
        fflush(stdout);
        (*drv)->StopIO(drv, 2, 0);
        /* AZ_SECOND=<gap s>:<rate>:<s>: a second play after a pause (the apps' IO stopped that long), at that
         * rate: the uplink keeps its stream through a pause shorter than its linger, a new one at another rate */
        if (getenv("AZ_SECOND")) {
            double gap = 0, s2 = 0; unsigned r2 = 0;
            if (sscanf(getenv("AZ_SECOND"), "%lf:%u:%lf", &gap, &r2, &s2) == 3 && r2 >= 8000) {
                nap(gap);
                if (r2 != cur_rate) { cur_rate = r2; set_rate(r2); }
                (*drv)->StartIO(drv, 2, 0); w_run = 1;
                pthread_t t2; pthread_create(&t2, NULL, writer, NULL);
                nap(s2); w_run = 0; pthread_join(t2, NULL);
                printf("SECOND rate=%u frames=%llu\n", r2, (unsigned long long)atomic_load(&shm->w));
                fflush(stdout);
                (*drv)->StopIO(drv, 2, 0);
            }
        }
        if (getenv("AZ_AFTER_S")) nap(atof(getenv("AZ_AFTER_S")));   /* the uplink's linger, the driver still there */
        return 0;
    }

    CFStringRef s = NULL;
    get(2, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &s, sizeof s);
    check(s && CFStringCompare(s, CFSTR("audiolaz"), 0) == kCFCompareEqualTo, "the device's name « audiolaz »");
    if (s) CFRelease(s);
    AudioObjectID dev = 0; CFStringRef uid = CFSTR("audiolaz_UID");
    AudioObjectPropertyAddress a = { kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 n = 0;
    (*drv)->GetPropertyData(drv, kAudioObjectPlugInObject, 0, &a, sizeof uid, &uid, sizeof dev, &n, &dev);
    check(dev == 2, "its UID found");
    AudioValueRange rr[16];
    n = get(2, kAudioDevicePropertyAvailableNominalSampleRates, kAudioObjectPropertyScopeGlobal, rr, sizeof rr);
    check(n == 10 * sizeof(AudioValueRange) && rr[0].mMinimum == 44100 && rr[9].mMinimum == 768000, "ten rates, 44.1 to 768 kHz");
    AudioStreamBasicDescription f;
    get(3, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, &f, sizeof f);
    check(f.mChannelsPerFrame == 2 && f.mBitsPerChannel == 32 && (f.mFormatFlags & kAudioFormatFlagIsFloat), "float32 stereo");
    AudioObjectID ids[4];
    check(get(2, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, ids, sizeof ids) == 4 && ids[0] == 3
          && get(2, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, ids, sizeof ids) == 0, "one output stream, no input");

    char rooms[] = "/tmp/azdrv.XXXXXX";
    if (!mkdtemp(rooms)) return 2;
    src_option("--airplay-in", "off");
    src_option("--in-driver", "2");
    check(src_init("", 3, rooms) == 0, "the inputs (%s)", src_what());

    /* its volume control */
    AudioObjectID cl[4] = { 0 };
    check(get(2, kAudioObjectPropertyControlList, kAudioObjectPropertyScopeGlobal, cl, sizeof cl) == sizeof(AudioObjectID) && cl[0] == 4, "its controls: one, the volume (4)");
    AudioClassID cls = 0;
    get(4, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal, &cls, sizeof cls);
    AudioValueRange vr = { 0, 0 };
    get(4, kAudioLevelControlPropertyDecibelRange, kAudioObjectPropertyScopeGlobal, &vr, sizeof vr);
    check(cls == kAudioVolumeControlClassID && vr.mMinimum == -60 && vr.mMaximum == 0, "a volume control, −60 … 0 dB");
    check(get_f32(4, kAudioLevelControlPropertyDecibelValue) == 0 && plat_src_volume(K) == 1.0, "full at first (0 dB): the player's gain 1");
    int nv = atomic_load(&n_vol);
    check(set_f32(4, kAudioLevelControlPropertyScalarValue, 0.5f), "the volume set (scalar 0.5)");
    const double g30 = plat_src_volume(K);
    check(fabsf(get_f32(4, kAudioLevelControlPropertyDecibelValue) + 30) < 0.01f && fabs(g30 - pow(10, -30 / 20.0)) < 1e-6 && atomic_load(&n_vol) > nv,
          "scalar 0.5: −30 dB, the player's gain %.5f, the apps told", g30);
    check(set_f32(4, kAudioLevelControlPropertyDecibelValue, -60) && plat_src_volume(K) == 0, "−60 dB, its bottom: silence");
    check(set_f32(4, kAudioLevelControlPropertyDecibelValue, -12.5f) && fabs(plat_src_volume(K) - pow(10, -12.5 / 20)) < 1e-6
          && fabsf(get_f32(4, kAudioLevelControlPropertyScalarValue) - (1 - 12.5f / 60)) < 1e-4, "−12.5 dB: its gain, its scalar");
    {   /* kept: a memory made anew, the driver started again: the volume from the host's storage */
        const int ifd = shm_open(iname, O_RDWR);
        az_info *im = ifd >= 0 ? mmap(NULL, sizeof *im, PROT_READ | PROT_WRITE, MAP_SHARED, ifd, 0) : MAP_FAILED;
        if (ifd >= 0) close(ifd);
        check(im != MAP_FAILED && im->magic == AZ_INFO_MAGIC, "its identity's memory (%s)", iname);
        if (im != MAP_FAILED) { atomic_store(&im->vol_cdb, 0); munmap(im, sizeof *im); }
        (*drv)->Initialize(drv, &host);
        check(fabsf(get_f32(4, kAudioLevelControlPropertyDecibelValue) + 12.5f) < 0.01f, "kept: −12.5 dB again after a restart (the host's storage)");
    }
    Float32 x = 0.25f; UInt32 xn = 0;
    AudioObjectPropertyAddress ca = { kAudioLevelControlPropertyConvertScalarToDecibels, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    (*drv)->GetPropertyData(drv, 4, 0, &ca, sizeof x, &x, sizeof x, &xn, &x);
    check(fabsf(x + 45) < 0.01f, "scalar 0.25 is −45 dB");
    set_f32(4, kAudioLevelControlPropertyDecibelValue, 0);
    /* the DAC behind it, its latency */
    check(str_is(kAudioObjectPropertyName, "audiolaz"), "no DAC known yet: « audiolaz »");
    int nn = atomic_load(&n_name);
    plat_src_dac("DAC-01", "Swiss Electronics Ltd", "DAC-01:245F:815", 0);
    check(told(&n_name, nn, 1.0), "the DAC given: the apps told");
    check(str_is(kAudioObjectPropertyName, "DAC-01 (audiolaz)") && str_is(kAudioObjectPropertyManufacturer, "Swiss Electronics Ltd")
          && str_is(kAudioDevicePropertyModelUID, "DAC-01:245F:815"), "shown as the DAC behind it: « DAC-01 (audiolaz) », its maker, its model");

    plat_src_dac("DAC-01", NULL, NULL, 1);
    check(str_is(kAudioObjectPropertyName, "DAC-01 (audiolaz)") && str_is(kAudioObjectPropertyManufacturer, "Swiss Electronics Ltd"),
          "the same DAC named again (roomd's tick, no maker known): « DAC-01 (audiolaz) », its maker kept");
    nn = atomic_load(&n_name);
    plat_src_dac("CXNv2", NULL, NULL, 0);
    check(told(&n_name, nn, 1.0) && str_is(kAudioObjectPropertyName, "CXNv2 (audiolaz)") && str_is(kAudioObjectPropertyManufacturer, "audiolaz"),
          "another DAC (a UPnP renderer): « CXNv2 (audiolaz) », no maker of its own");
    plat_src_dac("DAC-01 DAC-01 (audiolaz)", NULL, NULL, 0);
    check(str_is(kAudioObjectPropertyName, "DAC-01 (audiolaz)"), "a name given twice, « (audiolaz) » already there: « DAC-01 (audiolaz) »");
    plat_src_dac("Swiss Electronics Ltd DAC-01 DAC à deux", NULL, NULL, 0);
    check(str_is(kAudioObjectPropertyName, "Swiss Electronics (audiolaz)"), "a long name cut to 31 bytes at a word's end: « Swiss Electronics (audiolaz) »");

    plat_src_dac("audiolaz \xe2\x86\x92 FOCALE", NULL, NULL, 0);
    check(str_is(kAudioObjectPropertyName, "audiolaz \xe2\x86\x92 FOCALE"), "the uplink's room: « audiolaz → FOCALE », as it is");
    plat_src_dac("audiolaz \xe2\x86\x92 Salle de musique du premier étage", NULL, NULL, 0);
    check(str_is(kAudioObjectPropertyName, "audiolaz \xe2\x86\x92 Salle de musique"), "a long room cut to 31 bytes at a word's end");
    plat_src_dac("", NULL, NULL, 0);
    check(str_is(kAudioObjectPropertyName, "audiolaz"), "no DAC known: « audiolaz »");
    plat_src_dac("DAC-01", "Swiss Electronics Ltd", "DAC-01:245F:815", 1);
    check(str_is(kAudioObjectPropertyName, "DAC-01 (audiolaz)"), "the DAC-01 again: « DAC-01 (audiolaz) »");
    check(str_is(kAudioDevicePropertyDeviceUID, "audiolaz_UID"), "its UID unchanged (the apps' settings follow it)");
    int nl = atomic_load(&n_lat);
    plat_src_latency(K, 120);
    UInt32 lat = 0;
    get(2, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, &lat, sizeof lat);
    const unsigned r0 = src_rate(K) ? src_rate(K) : 44100;
    check(lat == (UInt32)llround(120.0 * r0 / 1000) && told(&n_lat, nl, 1.0), "its latency the player's: %u frames (120 ms at %u Hz), the apps told", lat, r0);
    const unsigned rates[] = { 44100, 96000, 192000, 384000, 768000 };
    for (unsigned i = 0; i < 5; i++) play(rates[i], 1.5, 300, 0, 0);
    play(96000, 10, 300, 1, 0);            /* not steered: the DAC's 300 ppm drains it */
    play(96000, 10, 300, 1, -300);         /* steered to the DAC's clock (what roomd's regulator settles at): none */
    play(44100, 10, -150, 1, 150);
    dop_check();
    src_shutdown();
    shm_unlink(name);
    shm_unlink(iname);
    char cmd[128]; snprintf(cmd, sizeof cmd, "rm -rf %s", rooms); (void)!system(cmd);
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
