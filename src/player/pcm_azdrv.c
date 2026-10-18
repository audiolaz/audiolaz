/* src/player/pcm_azdrv.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include "audiolaz_drv.h"

typedef struct {
    snd_pcm_ioplug_t io;
    az_shm *m;
    int tfd;
    char shm[64];
    uint64_t base;            /* the ring's sample time of app frame 0 (prepare) */
    uint64_t pos;             /* app frames consumed (the hardware pointer, unwrapped) */
    uint64_t clk;             /* host clock (ns) up to which pos was advanced while standing in */
    snd_pcm_uframes_t boundary;
    int run;
} azd;

static uint64_t ns(clockid_t c) { struct timespec t; clock_gettime(c, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

static int map(azd *a) {
    const mode_t um = umask(0);
    const int fd = shm_open(a->shm, O_RDWR | O_CREAT, 0666);
    umask(um);
    if (fd < 0) return -errno;
    struct stat st;
    if (fstat(fd, &st) || (st.st_size < (off_t)sizeof(az_shm) && ftruncate(fd, sizeof(az_shm)))) { close(fd); return -EIO; }
    void *p = mmap(NULL, sizeof(az_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return -errno;
    a->m = p;
    if (a->m->magic != AZ_MAGIC) { a->m->ch = AZ_CH; a->m->ring = AZ_RING; atomic_store(&a->m->lx, 1); a->m->magic = AZ_MAGIC; }
    return 0;
}

/* The hardware pointer: the player's rd while it beats, else the host clock; never past what the app wrote. */
static uint64_t advance(azd *a) {
    az_shm *m = a->m;
    const uint64_t now = ns(CLOCK_MONOTONIC), beat = atomic_load(&m->beat), w = atomic_load(&m->w);
    const uint64_t appl = w - a->base;
    if (a->run && beat && now - beat < AZ_BEAT_LX_NS && now >= beat) {
        const uint64_t rd = atomic_load(&m->rd);
        if (rd > a->base && rd - a->base > a->pos) a->pos = rd - a->base;
        a->clk = now;
    } else if (a->run) {
        const uint64_t f = (now - a->clk) * a->io.rate / 1000000000ull;
        a->pos += f; a->clk += f * 1000000000ull / a->io.rate;
        if (a->pos >= appl) { a->pos = appl; a->clk = now; }   /* the app late: no xrun, the sink waits */
        atomic_store(&m->rd, a->base + a->pos);
    }
    if (a->pos > appl) a->pos = appl;
    return a->pos;
}

static snd_pcm_sframes_t az_pointer(snd_pcm_ioplug_t *io) { azd *a = io->private_data; return (snd_pcm_sframes_t)(advance(a) % a->boundary); }

static snd_pcm_sframes_t az_transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *ar, snd_pcm_uframes_t off, snd_pcm_uframes_t n) {
    azd *a = io->private_data;
    az_shm *m = a->m;
    const uint64_t w = atomic_load(&m->w);
    for (snd_pcm_uframes_t i = 0; i < n; i++) {
        float *d = m->data + AZ_CH * ((w + i) & (AZ_RING - 1));
        for (int c = 0; c < AZ_CH; c++) {
            const unsigned char *s = (const unsigned char *)ar[c].addr + (ar[c].first + ar[c].step * (off + i)) / 8;
            int32_t v;
            switch (io->format) {
            case SND_PCM_FORMAT_S16_LE: v = (int32_t)((uint32_t)(s[0] | s[1] << 8) << 16); break;
            case SND_PCM_FORMAT_S24_LE: v = (int32_t)((uint32_t)(s[0] | s[1] << 8 | s[2] << 16) << 8); break;   /* low 3 bytes of 4 */
            case SND_PCM_FORMAT_S24_3LE: v = (int32_t)((uint32_t)(s[0] | s[1] << 8 | s[2] << 16) << 8); break;
            default: memcpy(&v, s, 4); break;                                                                  /* S32_LE, FLOAT_LE */
            }
            memcpy(d + c, &v, 4);
        }
    }
    atomic_store_explicit(&m->w, w + n, memory_order_release);
    return (snd_pcm_sframes_t)n;
}

static void timer(azd *a, int on) {
    struct itimerspec t = { { 0, 0 }, { 0, 0 } };
    if (on) {
        long p = (long)((double)a->io.period_size / a->io.rate * 1e9 / 4);
        if (p < 1000000) p = 1000000; else if (p > 10000000) p = 10000000;
        t.it_interval.tv_nsec = t.it_value.tv_nsec = p;
    }
    timerfd_settime(a->tfd, 0, &t, NULL);
}

static int az_prepare(snd_pcm_ioplug_t *io) {
    azd *a = io->private_data;
    az_shm *m = a->m;
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    if (snd_pcm_sw_params_current(io->pcm, sw) == 0) snd_pcm_sw_params_get_boundary(sw, &a->boundary);
    if (!a->boundary) a->boundary = io->buffer_size;
    atomic_store(&m->running, 0);
    a->run = 0; a->pos = 0;
    a->base = atomic_load(&m->w);
    atomic_store(&m->rd, a->base);
    atomic_store(&m->fmt, io->format == SND_PCM_FORMAT_FLOAT_LE ? AZ_F32 : AZ_S32);
    atomic_store(&m->rate, io->rate);
    atomic_fetch_add(&m->gen, 1);
    timer(a, 1);                     /* the app's prefill wakes too */
    return 0;
}
static int az_start(snd_pcm_ioplug_t *io) {
    azd *a = io->private_data;
    a->run = 1; a->clk = ns(CLOCK_MONOTONIC);
    atomic_store(&a->m->t0, ns(CLOCK_REALTIME));
    atomic_store(&a->m->running, 1);
    return 0;
}
static int az_stop(snd_pcm_ioplug_t *io) {
    azd *a = io->private_data;
    a->run = 0;
    atomic_store(&a->m->running, 0);
    timer(a, 0);
    return 0;
}
static int az_pause(snd_pcm_ioplug_t *io, int enable) {
    azd *a = io->private_data;
    if (enable) { advance(a); a->run = 0; atomic_store(&a->m->running, 0); }
    else { a->run = 1; a->clk = ns(CLOCK_MONOTONIC); atomic_store(&a->m->t0, ns(CLOCK_REALTIME)); atomic_store(&a->m->running, 1); }
    return 0;
}
static int az_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int nfds, unsigned short *revents) {
    azd *a = io->private_data;
    (void)nfds;
    uint64_t x;
    if (pfd[0].revents & POLLIN) (void)!read(a->tfd, &x, sizeof x);
    *revents = 0;
    if (io->state == SND_PCM_STATE_RUNNING || io->state == SND_PCM_STATE_PREPARED || io->state == SND_PCM_STATE_DRAINING) {
        const uint64_t hw = advance(a), appl = atomic_load(&a->m->w) - a->base;
        if (io->state == SND_PCM_STATE_DRAINING ? hw >= appl : io->buffer_size - (appl - hw) >= io->period_size) *revents = POLLOUT;
    }
    return 0;
}
static int az_close(snd_pcm_ioplug_t *io) {
    azd *a = io->private_data;
    if (a->m) { atomic_store(&a->m->running, 0); munmap(a->m, sizeof(az_shm)); }
    if (a->tfd >= 0) close(a->tfd);
    free(a);
    return 0;
}

static const snd_pcm_ioplug_callback_t cb = {
    .start = az_start, .stop = az_stop, .pointer = az_pointer, .transfer = az_transfer, .close = az_close,
    .prepare = az_prepare, .pause = az_pause, .poll_revents = az_revents,
};

SND_PCM_PLUGIN_DEFINE_FUNC(azdrv) {
    (void)root;
    const char *shm = az_shm_name();
    snd_config_iterator_t i, next;
    snd_config_for_each(i, next, conf) {
        snd_config_t *n = snd_config_iterator_entry(i);
        const char *id;
        if (snd_config_get_id(n, &id) < 0) continue;
        if (!strcmp(id, "comment") || !strcmp(id, "type") || !strcmp(id, "hint")) continue;
        if (!strcmp(id, "shm")) { if (snd_config_get_string(n, &shm) < 0 || shm[0] != '/') return -EINVAL; continue; }
        SNDERR("azdrv: unknown field %s", id);
        return -EINVAL;
    }
    if (stream != SND_PCM_STREAM_PLAYBACK) return -EINVAL;
    azd *a = calloc(1, sizeof *a);
    if (!a) return -ENOMEM;
    snprintf(a->shm, sizeof a->shm, "%s", shm);
    a->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    int e = a->tfd < 0 ? -errno : map(a);
    if (e < 0) { if (a->tfd >= 0) close(a->tfd); free(a); return e; }
    a->io.version = SND_PCM_IOPLUG_VERSION;
    a->io.name = "audiolaz (the player's input)";
    a->io.callback = &cb;
    a->io.private_data = a;
    a->io.poll_fd = a->tfd;
    a->io.poll_events = POLLIN;
    a->io.mmap_rw = 0;
    a->io.flags = SND_PCM_IOPLUG_FLAG_BOUNDARY_WA;
    if ((e = snd_pcm_ioplug_create(&a->io, name, stream, mode)) < 0) { munmap(a->m, sizeof(az_shm)); close(a->tfd); free(a); return e; }
    static const unsigned acc[] = { SND_PCM_ACCESS_RW_INTERLEAVED, SND_PCM_ACCESS_MMAP_INTERLEAVED };
    static const unsigned fmt[] = { SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S24_3LE, SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_FLOAT_LE };
    static const unsigned rates[] = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000 };
    if ((e = snd_pcm_ioplug_set_param_list(&a->io, SND_PCM_IOPLUG_HW_ACCESS, 2, acc)) < 0 ||
        (e = snd_pcm_ioplug_set_param_list(&a->io, SND_PCM_IOPLUG_HW_FORMAT, 5, fmt)) < 0 ||
        (e = snd_pcm_ioplug_set_param_minmax(&a->io, SND_PCM_IOPLUG_HW_CHANNELS, 2, 2)) < 0 ||
        (e = snd_pcm_ioplug_set_param_list(&a->io, SND_PCM_IOPLUG_HW_RATE, 10, rates)) < 0 ||
        (e = snd_pcm_ioplug_set_param_minmax(&a->io, SND_PCM_IOPLUG_HW_PERIODS, 2, 64)) < 0 ||
        (e = snd_pcm_ioplug_set_param_minmax(&a->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 64 * 4, AZ_RING)) < 0 ||
        (e = snd_pcm_ioplug_set_param_minmax(&a->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 256 * 4, AZ_RING / 2 * 4))) {   /* ≤ half the ring */
        snd_pcm_ioplug_delete(&a->io);
        return e;
    }
    *pcmp = a->io.pcm;
    return 0;
}
SND_PCM_PLUGIN_SYMBOL(azdrv);
