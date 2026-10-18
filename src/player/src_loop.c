/* src/player/src_loop.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include "audiolaz_drv.h"
#include <math.h>
#include <sys/ioctl.h>
#include "../linux/kmod/azcard.h"
#include "src_plat.h"   /* src.h's calls as plat_src_*: src_upnp.c hands each input to its own */
#include "src.h"

static void slog(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); fflush(stderr);
}

static char LOOP[16] = "9";            /* the card's index */
static int SUBS = 1;
static char what[64];
static snd_ctl_t *ctl;
static snd_ctl_elem_value_t *shift_vals[SRC_MAX], *active_vals[SRC_MAX], *srate_vals[SRC_MAX];
static int shift_oks[SRC_MAX];
static struct pollfd ctl_fds[4];
static int ctl_nfds = 0;

typedef struct { int running; unsigned rate, channels; double trigger; } loop_state;
static loop_state read_loop_sub(int sub) {
    loop_state s = { 0, 0, 0, 0 };
    char path[256], line[256];
    snprintf(path, sizeof path, "/proc/asound/card%s/pcm1p/sub%d/status", LOOP, sub);
    FILE *f = fopen(path, "r");
    if (!f) return s;
    if (fgets(line, sizeof line, f)) s.running = strstr(line, "RUNNING") != NULL;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "trigger_time:", 13)) s.trigger = atof(line + 13);
    fclose(f);
    snprintf(path, sizeof path, "/proc/asound/card%s/pcm1p/sub%d/hw_params", LOOP, sub);
    if ((f = fopen(path, "r"))) {
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "rate:", 5)) s.rate = (unsigned)strtoul(line + 5, NULL, 10);
            else if (!strncmp(line, "channels:", 9)) s.channels = (unsigned)strtoul(line + 9, NULL, 10);   /* 5.1: 6 */
        }
        fclose(f);
    }
    return s;
}

static int DK = -1;
static az_shm *DM;
static uint64_t dns(clockid_t c) { struct timespec t; clock_gettime(c, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static az_shm *drv(void) {
    if (DM) return DM;
    const int fd = shm_open(az_shm_name(), O_RDWR, 0);
    if (fd < 0) return NULL;
    struct stat st;
    void *m = fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(az_shm) ? mmap(NULL, sizeof(az_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    if (m == MAP_FAILED) return NULL;
    if (((az_shm *)m)->magic != AZ_MAGIC) { munmap(m, sizeof(az_shm)); return NULL; }
    slog("input %d: the ALSA output « audiolaz » (%s), at the DAC's clock", DK, az_shm_name());
    return DM = m;
}
static int is_drv(int k) { return k >= 0 && k == DK; }

#define CS_MAX 8
static int CKS[SRC_MAX];                  /* input k → the card's stream (−1: not the card's) */
static int cks_set = 0;
static int CFD[CS_MAX];
static struct azc_info CI[CS_MAX];        /* each stream's state as last read */
static uint32_t c_gen_seen[CS_MAX];       /* the generation c_started was taken at */
static double c_started[CS_MAX];          /* when its stream (that generation) was first seen playing */
static double c_tried[CS_MAX];
static char CPATH[CS_MAX][48];
static int card_arrow = 0;
static double cnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void cks_init(void) { if (!cks_set) { for (int k = 0; k < SRC_MAX; k++) CKS[k] = -1; for (int i = 0; i < CS_MAX; i++) CFD[i] = -1; cks_set = 1; } }
static int is_card(int k) { cks_init(); return k >= 0 && k < SRC_MAX && CKS[k] >= 0; }
static int any_card(void) { cks_init(); for (int k = 0; k < SRC_MAX; k++) if (CKS[k] >= 0) return 1; return 0; }
static void card_name_apply(void);
static struct azc_info *card_s(int cs) {
    if (cs < 0 || cs >= CS_MAX) return NULL;
    if (CFD[cs] < 0) {
        if (cnow() - c_tried[cs] < 1.0) return NULL;        /* the module not loaded: asked again once a second */
        c_tried[cs] = cnow();
        char dev[32];
        if (CPATH[cs][0]) snprintf(dev, sizeof dev, "%s", CPATH[cs]);
        else if (cs) snprintf(dev, sizeof dev, "%s%d", AZC_DEV, cs); else snprintf(dev, sizeof dev, "%s", AZC_DEV);
        CFD[cs] = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (CFD[cs] < 0) return NULL;
        int k = 0; while (k < SRC_MAX && CKS[k] != cs) k++;
        slog("input %d: the « audiolaz » sound card's stream %d (%s), at the DAC's clock", k, cs, dev);
        card_name_apply();                                  /* its name, if the player gave one already */
    }
    if (ioctl(CFD[cs], AZC_IOC_INFO, &CI[cs])) { close(CFD[cs]); CFD[cs] = -1; return NULL; }
    if (CI[cs].running && CI[cs].gen != c_gen_seen[cs]) { c_gen_seen[cs] = CI[cs].gen; c_started[cs] = cnow(); }
    return &CI[cs];
}
static struct azc_info *card(int k) { return is_card(k) ? card_s(CKS[k]) : NULL; }

int src_paced(int k) { return is_drv(k) || is_card(k); }
/* src.h: the plugin's app stopped, paused or closed it (running 0), not a stall nor zeros */
int src_io_ended(int k) {
    if (is_card(k)) { const struct azc_info *c = card(k); return c && !c->running; }
    az_shm *m = is_drv(k) ? drv() : NULL; return m && !atomic_load(&m->running);
}
/* the gain input k's source asks: the card's volume control (1 for the others) */
double plat_src_volume(int k) {
    if (!is_card(k)) return 1.0;
    const struct azc_info *c = card(k);
    if (!c) return 1.0;
    return c->mute ? 0.0 : c->volume_cdb >= 0 ? 1.0 : pow(10.0, c->volume_cdb / 2000.0);
}

static char card_name_want[AZ_SHOWN_MAX], card_name_done[AZ_SHOWN_MAX];
static int card_name_said;
static void card_name_apply(void) {
    if (!card_name_want[0] || !strcmp(card_name_want, card_name_done)) return;
    for (int cs = 0; cs < CS_MAX; cs++) {
        if (CFD[cs] < 0) continue;
        struct azc_name nm;
        memset(&nm, 0, sizeof nm);
        snprintf(nm.name, sizeof nm.name, "%s", card_name_want);
        if (ioctl(CFD[cs], AZC_IOC_SET_NAME, &nm) == 0) {
            snprintf(card_name_done, sizeof card_name_done, "%s", card_name_want);
            slog("the « audiolaz » card: shown as « %s », the DAC it plays to", card_name_done);
        } else if (!card_name_said++) slog("the « audiolaz » card: its name not changed (%s: a module before 18/10), its options' kept", strerror(errno));
        return;
    }
}
void plat_src_dac(const char *name, const char *maker, const char *model, int local) {
    (void)maker; (void)model; (void)local;
    cks_init();
    if (!any_card()) return;
    if (card_arrow) {
        if (!name || !*name) return;
        char t[160];
        snprintf(t, sizeof t, "audiolaz \u2192 %s", name);
        size_t k = 0;                                    /* the card's 31 bytes, cut after a whole UTF-8 character */
        while (t[k]) { const unsigned char c = (unsigned char)t[k]; const size_t w = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2; if (k + w + 1 > sizeof card_name_want) break; k += w; }
        memcpy(card_name_want, t, k); card_name_want[k] = 0;
    } else az_shown(card_name_want, sizeof card_name_want, name);
    card_name_apply();
}
void plat_src_latency(int k, double ms) { (void)k; (void)ms; }
/* src.h src_hold: AZC_IOC_HOLD (a module before 18/10: none, as before) */
void plat_src_hold(int k, int ms) {
    const struct azc_info *c = card(k);
    if (!c || CFD[CKS[k]] < 0) return;
    __u32 v = ms > 0 ? (__u32)ms : 0;
    static uint32_t said_gen = UINT32_MAX;
    if (ioctl(CFD[CKS[k]], AZC_IOC_HOLD, &v) == 0 && c->gen != said_gen) {   /* said once a stream */
        said_gen = c->gen;
        slog("input %d: the « audiolaz » card holds its music (up to %d ms at a time) while the output opens", k, ms);
    }
}

int src_init(const char *card, int subs, const char *rooms) {
    (void)rooms;
    SUBS = subs < 1 ? 1 : subs > SRC_MAX ? SRC_MAX : subs;
    snprintf(LOOP, sizeof LOOP, "%s", card);
    if (!strcmp(card, "-")) {
        snprintf(what, sizeof what, "the « audiolaz » card");
        return 0;
    }
    /* --loop by the card's id (08/10, install.sh: --loop Loopback): its index, which /proc/asound/card<n> needs */
    if (LOOP[0] < '0' || LOOP[0] > '9') {
        const int k = snd_card_get_index(card);
        if (k >= 0) { snprintf(LOOP, sizeof LOOP, "%d", k); slog("Loopback card \"%s\": index %d", card, k); }
        else slog("no card \"%s\" (snd-aloop not loaded?): its inputs stay silent", card);
    }
    snprintf(what, sizeof what, "Loopback card %s", LOOP);
    char name[32];
    snprintf(name, sizeof name, "hw:%s", LOOP);
    if (snd_ctl_open(&ctl, name, 0) < 0) return -1;
    for (int k = 0; k < SUBS; k++) {                /* the side we read (device 0, subdevice k): it sets that cable's pace */
        snd_ctl_elem_value_malloc(&shift_vals[k]);
        snd_ctl_elem_value_set_interface(shift_vals[k], SND_CTL_ELEM_IFACE_PCM);
        snd_ctl_elem_value_set_name(shift_vals[k], "PCM Rate Shift 100000");
        snd_ctl_elem_value_set_device(shift_vals[k], 0);
        snd_ctl_elem_value_set_subdevice(shift_vals[k], (unsigned)k);
        shift_oks[k] = snd_ctl_elem_read(ctl, shift_vals[k]) >= 0;
    }
    if (!shift_oks[0]) slog("no rate shift control on the Loopback: long plays may drift");
    for (int k = 0; k < SUBS; k++) {
        snd_ctl_elem_value_malloc(&active_vals[k]);
        snd_ctl_elem_value_set_interface(active_vals[k], SND_CTL_ELEM_IFACE_PCM);
        snd_ctl_elem_value_set_name(active_vals[k], "PCM Slave Active");
        snd_ctl_elem_value_set_subdevice(active_vals[k], (unsigned)k);
        snd_ctl_elem_value_malloc(&srate_vals[k]);
        snd_ctl_elem_value_set_interface(srate_vals[k], SND_CTL_ELEM_IFACE_PCM);
        snd_ctl_elem_value_set_name(srate_vals[k], "PCM Slave Rate");
        snd_ctl_elem_value_set_subdevice(srate_vals[k], (unsigned)k);
    }
    snd_ctl_nonblock(ctl, 1);
    snd_ctl_subscribe_events(ctl, 1);
    ctl_nfds = snd_ctl_poll_descriptors(ctl, ctl_fds, 4);
    return 0;
}
const char *src_what(void) { return what; }
int src_active(int k) {
    if (is_card(k)) { const struct azc_info *c = card(k); return c && c->running && c->rate >= 8000 && c->frame_bytes; }
    if (is_drv(k)) {                     /* playing, and writing (an app killed holding it: w still for 2 s, let go) */
        static uint64_t lw; static uint64_t lt;
        az_shm *m = drv();
        if (!m) return 0;
        const uint64_t w = atomic_load(&m->w), now = dns(CLOCK_MONOTONIC);
        if (!lt) { lw = w; lt = now - 3000000000ull; }   /* first seen: playing once it writes (not a killed app's) */
        else if (w != lw) { lw = w; lt = now; }
        return atomic_load(&m->running) && atomic_load(&m->rate) >= 8000 && now - lt < 2000000000ull;
    }
    if (!ctl || !active_vals[k] || snd_ctl_elem_read(ctl, active_vals[k]) < 0) return read_loop_sub(k).running;
    return snd_ctl_elem_value_get_boolean(active_vals[k], 0);
}
unsigned src_rate(int k) {
    if (is_card(k)) { const struct azc_info *c = card(k); return c ? c->rate : 0; }
    if (is_drv(k)) { az_shm *m = drv(); return m ? atomic_load(&m->rate) : 0; }
    if (!ctl || !srate_vals[k] || snd_ctl_elem_read(ctl, srate_vals[k]) < 0) return read_loop_sub(k).rate;
    return (unsigned)snd_ctl_elem_value_get_integer(srate_vals[k], 0);
}
double src_started(int k) { if (is_card(k)) { const struct azc_info *c = card(k); return c && c->running ? c_started[CKS[k]] : 0; } if (is_drv(k)) { az_shm *m = drv(); return m ? atomic_load(&m->t0) / 1e9 : 0; } return read_loop_sub(k).trigger; }
unsigned src_channels(int k) { if (is_card(k)) { const struct azc_info *c = card(k); return c ? c->channels : 0; } if (is_drv(k)) return 2; return read_loop_sub(k).channels; }
/* Its source's side (device 1, subdevice k) closed: shairport-sync not playing into it, nor holding it. */
int src_closed(int k) {
    if (is_drv(k) || is_card(k)) return !src_active(k);
    char path[256], line[128];
    snprintf(path, sizeof path, "/proc/asound/card%s/pcm1p/sub%d/status", LOOP, k);
    FILE *f = fopen(path, "r");
    if (!f) return 1;
    const int closed = fgets(line, sizeof line, f) && !strncmp(line, "closed", 6);
    fclose(f);
    return closed;
}
int src_event_fds(struct pollfd *fds, int max) {
    int n = 0;
    for (int k = 0; k < ctl_nfds && n < max; k++) fds[n++] = ctl_fds[k];
    for (int cs = 0; any_card() && cs < CS_MAX && n < max; cs++) {   /* a card's stream started or stopped */
        int used = 0; for (int k = 0; k < SRC_MAX; k++) if (CKS[k] == cs) used = 1;
        if (used && card_s(cs)) { fds[n].fd = CFD[cs]; fds[n].events = POLLPRI; fds[n].revents = 0; n++; }
    }
    return n;
}
void src_drain(void) {
    for (int k = 0; any_card() && k < SRC_MAX; k++) if (CKS[k] >= 0) (void)card(k);   /* generations seen: POLLPRI quiet */
    if (!ctl_nfds) return;
    snd_ctl_event_t *ev;
    snd_ctl_event_alloca(&ev);
    while (snd_ctl_read(ctl, ev) > 0) { }
}
void src_shift(int k, long v) {
    if (k < 0 || k >= SRC_MAX || !shift_oks[k] || is_drv(k) || is_card(k)) return;
    snd_ctl_elem_value_set_integer(shift_vals[k], 0, v);
    snd_ctl_elem_write(ctl, shift_vals[k]);
}
/* Somebody connected to shairport-sync (an AirPlay sender holding its session, playing or not): an established TCP
 * connection to its port (the settings' port=, else AirPlay 2's 7000 and AirPlay 1's 5000), IPv4 or IPv6. */
int src_air_connected(int port) {
    static const char *const files[2] = { "/proc/net/tcp", "/proc/net/tcp6" };
    char line[512];
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(files[i], "r");
        if (!f) continue;
        int hit = 0;
        while (!hit && fgets(line, sizeof line, f)) {
            unsigned lp = 0, st = 0;
            if (sscanf(line, " %*d: %*[0-9A-Fa-f]:%x %*[0-9A-Fa-f]:%*x %x", &lp, &st) != 2 || st != 1) continue;   /* 01: established */
            if (port > 0 ? lp == (unsigned)port : lp == 7000 || lp == 5000) hit = 1;
        }
        fclose(f);
        if (hit) return 1;
    }
    return 0;
}

struct src_pcm { snd_pcm_t *p; az_shm *m; uint64_t r; uint32_t gen; int tfd; int card, cs; uint32_t cfmt, cfb; unsigned char *cbuf; size_t cbn;
                 long dop_run, dop_off; int dop_on, dop_said; uint32_t dop_last;
                 uint32_t csi; unsigned crate; };   /* the card's stand_in as last seen (SRC_LOST), its rate */
static void beat(src_pcm *s) { atomic_store(&s->m->beat, dns(CLOCK_MONOTONIC)); }
static src_pcm *drv_open(unsigned rate, unsigned ch) {
    az_shm *m = drv();
    if (!m || ch != 2 || atomic_load(&m->rate) != rate) { slog("« audiolaz » at %u Hz, %u channels: not what it plays", rate, ch); return NULL; }
    src_pcm *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->m = m; s->gen = atomic_load(&m->gen); s->r = atomic_load(&m->rd);
    s->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);   /* a wake-up every 2 ms, not a clock */
    struct itimerspec t = { { 0, 2000000 }, { 0, 2000000 } };
    if (s->tfd >= 0) timerfd_settime(s->tfd, 0, &t, NULL);
    beat(s);
    return s;
}
static long drv_avail(src_pcm *s) {
    uint64_t x;
    if (s->tfd >= 0) (void)!read(s->tfd, &x, sizeof x);
    beat(s);
    if (atomic_load(&s->m->gen) != s->gen) return SRC_ENDED;       /* the app prepared again: another stream (18/10: no loss) */
    const uint64_t w = atomic_load_explicit(&s->m->w, memory_order_acquire);
    if (w < s->r || w - s->r > AZ_RING - 4096) return -EPIPE;
    return (long)(w - s->r);
}
/* the card: its stream at this rate, stereo, read as it plays (its frames as they came, to 32-bit words) */
static src_pcm *card_open(int k, unsigned rate, unsigned ch) {
    const struct azc_info *c = card(k);
    if (!c || !c->running || c->rate != rate || ch != c->channels || c->channels != 2 || !c->frame_bytes) {
        slog("the « audiolaz » card at %u Hz, %u channels: not what it plays", rate, ch); return NULL;
    }
    src_pcm *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->card = 1; s->cs = CKS[k]; s->gen = c->gen; s->cfmt = c->format; s->cfb = c->frame_bytes;
    s->csi = c->stand_in; s->crate = c->rate;                       /* what the card let go before this player came: not its */
    s->cbn = 8192; s->cbuf = malloc(s->cbn * s->cfb);
    if (!s->cbuf) { free(s); return NULL; }
    /* a wake-up every 2 ms, as the driver's input: the frames are there ahead (the application's buffer), the player
     * takes them as its ring has room (paced): polling the card itself would wake it at once, again and again */
    s->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    struct itimerspec t = { { 0, 2000000 }, { 0, 2000000 } };
    if (s->tfd >= 0) timerfd_settime(s->tfd, 0, &t, NULL);
    return s;
}

static char card_lost_why[96];
static long card_avail(src_pcm *s) {
    uint64_t x;
    if (s->tfd >= 0) (void)!read(s->tfd, &x, sizeof x);
    const struct azc_info *c = card_s(s->cs);
    if (!c) return -EPIPE;                                       /* the card gone (its module unloaded) */
    if (c->gen != s->gen || !c->running) return SRC_ENDED;
    if (c->stand_in > s->csi) {
        const uint32_t n = c->stand_in - s->csi;
        s->csi = c->stand_in;
        snprintf(card_lost_why, sizeof card_lost_why, "%.0f ms of it let go by the card's own clock, unread", 1000.0 * n / (s->crate ? s->crate : 48000));
        return SRC_LOST;
    }
    return c->written > c->read ? (long)(c->written - c->read) : 0;
}
static inline int32_t card_s32(const unsigned char *b, uint32_t fmt) {   /* full scale ±2^31, bit for bit */
    switch (fmt) {
    case AZC_FMT_S16: return (int32_t)((uint32_t)(b[0] | b[1] << 8) << 16);
    case AZC_FMT_S24: return (int32_t)((uint32_t)(b[0] | b[1] << 8 | (uint32_t)b[2] << 16) << 8);
    case AZC_FMT_S24_3: return (int32_t)((uint32_t)(b[0] | b[1] << 8 | (uint32_t)b[2] << 16) << 8);
    default: return (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
    }
}

#define DOP_ON 64
#define DOP_OFF 4096
static void card_dop(src_pcm *s, int32_t *pcm, unsigned long got) {
    if (s->cfmt == AZC_FMT_S16) return;
    for (unsigned long i = 0; i < got; i++) {
        const uint32_t m = (uint32_t)pcm[2 * i] >> 24, m2 = (uint32_t)pcm[2 * i + 1] >> 24;
        const int mark = (m == 0x05 || m == 0xFA) && m2 == m && m != s->dop_last;
        s->dop_last = m;
        if (mark) { s->dop_run++; s->dop_off = 0; } else { s->dop_run = 0; s->dop_off++; }
        if (!s->dop_on && s->dop_run >= DOP_ON) {
            s->dop_on = 1;
            if (!s->dop_said) { s->dop_said = 1; slog("the « audiolaz » card's stream %d receives DSD as DoP: silence (in Roon, the device's DSD playback: « Convert to PCM »)", s->cs); }
        }
        if (s->dop_on && s->dop_off >= DOP_OFF) s->dop_on = 0;
    }
    if (s->dop_on) memset(pcm, 0, sizeof(int32_t) * 2 * got);
}
static long card_read(src_pcm *s, int32_t *pcm, unsigned long n) {
    if (n > s->cbn) n = s->cbn;
    if (CFD[s->cs] < 0) return -EPIPE;
    const ssize_t r = read(CFD[s->cs], s->cbuf, n * s->cfb);
    if (r < 0) return errno == EAGAIN ? SRC_AGAIN : -EPIPE;
    if (r == 0) { const long a = card_avail(s); return a < 0 ? a : SRC_AGAIN; }   /* the stream stopped or changed under the read */
    const unsigned long got = (unsigned long)r / s->cfb;
    const unsigned sb = s->cfb / 2;                                 /* a sample's bytes */
    for (unsigned long i = 0; i < 2 * got; i++) pcm[i] = card_s32(s->cbuf + i * sb, s->cfmt);
    card_dop(s, pcm, got);
    return (long)got;
}

src_pcm *src_open(int k, unsigned rate, unsigned ch, unsigned long period, unsigned periods) {
    if (is_card(k)) return card_open(k, rate, ch);
    if (is_drv(k)) return drv_open(rate, ch);
    char name[64];
    snprintf(name, sizeof name, "plughw:%s,0,%d", LOOP, k);
    snd_pcm_t *p;
    int e = snd_pcm_open(&p, name, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (e < 0) { slog("%s: %s", name, snd_strerror(e)); return NULL; }
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(p, hw);
    unsigned r = rate, n = periods;
    snd_pcm_uframes_t per = period;
    if ((e = snd_pcm_hw_params_set_access(p, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (e = snd_pcm_hw_params_set_format(p, hw, SND_PCM_FORMAT_S32_LE)) < 0 ||
        (e = snd_pcm_hw_params_set_channels(p, hw, ch < 2 || ch > SRC_MAX_CH ? 2 : ch)) < 0 ||
        (e = snd_pcm_hw_params_set_rate(p, hw, r, 0)) < 0 ||
        (e = snd_pcm_hw_params_set_period_size_near(p, hw, &per, 0)) < 0 ||
        (e = snd_pcm_hw_params_set_periods_near(p, hw, &n, 0)) < 0 ||
        (e = snd_pcm_hw_params(p, hw)) < 0) {
        slog("%s at %u Hz: %s", name, rate, snd_strerror(e));
        snd_pcm_close(p);
        return NULL;
    }
    src_pcm *s = malloc(sizeof *s);
    if (!s) { snd_pcm_close(p); return NULL; }
    memset(s, 0, sizeof *s);
    s->p = p;
    return s;
}
void src_start(src_pcm *s) { if (s->p) snd_pcm_start(s->p); }
long src_avail(src_pcm *s) { if (s->card) return card_avail(s); if (s->m) return drv_avail(s); return (long)snd_pcm_avail_update(s->p); }
long src_read(src_pcm *s, int32_t *pcm, unsigned long n) {
    if (s->card) return card_read(s, pcm, n);
    if (s->m) {                                      /* bit for bit; rd: the plugin's pointer, at this read's pace */
        const long av = drv_avail(s);
        if (av <= 0) return av < 0 ? av : SRC_AGAIN;
        if ((unsigned long)av < n) n = (unsigned long)av;
        const uint32_t fmt = atomic_load(&s->m->fmt);
        for (unsigned long i = 0; i < n; i++) { pcm[2 * i] = az_get(s->m, fmt, s->r + i, 0); pcm[2 * i + 1] = az_get(s->m, fmt, s->r + i, 1); }
        s->r += n;
        atomic_store(&s->m->rd, s->r);
        return (long)n;
    }
    const snd_pcm_sframes_t r = snd_pcm_readi(s->p, pcm, n);
    return r == -EAGAIN ? SRC_AGAIN : (long)r;
}
int src_delay(src_pcm *s, long *frames) {
    if (s->card) { const long a = card_avail(s); *frames = a > 0 ? a : 0; return 0; }
    if (s->m) { const long a = drv_avail(s); *frames = a > 0 ? a : 0; return 0; }
    snd_pcm_sframes_t d;
    if (snd_pcm_delay(s->p, &d)) return -1;
    *frames = (long)d;
    return 0;
}
void src_recover(src_pcm *s, long err) { if (s->card) { const struct azc_info *c = card_s(s->cs); if (c) { if (c->gen != s->gen) s->csi = c->stand_in; s->gen = c->gen; s->crate = c->rate; } return; } if (s->m) { s->gen = atomic_load(&s->m->gen); s->r = atomic_load(&s->m->w); atomic_store(&s->m->rd, s->r); return; } snd_pcm_recover(s->p, (int)err, 1); snd_pcm_start(s->p); }
int src_poll_fds(src_pcm *s, struct pollfd *fds, int max) { if (s->card) { if (s->tfd < 0 || max < 1) return 0; fds[0].fd = s->tfd; fds[0].events = POLLIN; fds[0].revents = 0; return 1; } if (s->m) { if (s->tfd < 0 || max < 1) return 0; fds[0].fd = s->tfd; fds[0].events = POLLIN; return 1; } return snd_pcm_poll_descriptors(s->p, fds, (unsigned)max); }
void src_close(src_pcm *s) {
    if (!s) return;
    if (s->card) { free(s->cbuf); if (s->tfd >= 0) close(s->tfd); }   /* the device stays open: the card's next stream */
    else if (s->m) { atomic_store(&s->m->beat, 0); if (s->tfd >= 0) close(s->tfd); }   /* the plugin's host clock at once */
    else snd_pcm_close(s->p);
    free(s);
}
const char *src_strerror(long e) {
    if (e == SRC_ENDED) return "the stream ended or another began";
    if (e == SRC_LOST) return card_lost_why[0] ? card_lost_why : "music let go unread";
    return snd_strerror((int)e);
}
/* The sources are the system's services here (install.sh): none run by this layer, <rooms>/input-stop stops them. */
int src_owns(int k) { (void)k; return 0; }
int src_stop(int k) { (void)k; return 0; }
const char *src_air_meta(const char *asked) { (void)asked; return NULL; }
void src_air_item(uint32_t code) { (void)code; }
void src_air_format(const char *sdsc) { (void)sdsc; }
int src_relayout(src_pcm *s) { (void)s; return 0; }
void src_air_name(const char *name) { (void)name; }
int src_status(char *out, size_t len) { (void)out; (void)len; return 0; }
int src_option(const char *name, const char *value) {
    if (!strcmp(name, "--in-driver")) { DK = strcmp(value, "off") && atoi(value) >= 0 && atoi(value) < SRC_MAX ? atoi(value) : -1; return 1; }
    if (!strcmp(name, "--in-card-path")) {          /* s=<device>: the card's stream s read there (a voie's card) */
        char *e; const long cs = strtol(value, &e, 10);
        if (e != value && *e == '=' && cs >= 0 && cs < CS_MAX) snprintf(CPATH[cs], sizeof CPATH[cs], "%s", e + 1);
        return 1;
    }
    if (!strcmp(name, "--in-card-name")) { card_arrow = !strcmp(value, "arrow"); return 1; }
    if (!strcmp(name, "--in-card")) {               /* k[=s],…: input k reads the card's stream s (0 by default) */
        cks_init();
        for (int k = 0; k < SRC_MAX; k++) CKS[k] = -1;
        if (strcmp(value, "off")) for (const char *p = value; *p; ) {
            char *e; const long k = strtol(p, &e, 10); long cs = 0;
            if (e == p) break;
            if (*e == '=') { const char *q = e + 1; cs = strtol(q, &e, 10); if (e == q) break; }
            if (k >= 0 && k < SRC_MAX && cs >= 0 && cs < CS_MAX) CKS[k] = (int)cs;
            p = *e == ',' ? e + 1 : e;
            if (*e && *e != ',') break;
        }
        return 1;
    }
    return 0;
}
void src_tick(void) { }
void src_shutdown(void) { }
