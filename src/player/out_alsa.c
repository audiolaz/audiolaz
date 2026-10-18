/* The player's card on Linux (10/10, out.h): an ALSA device, opened as roomd.c always opened it (non-blocking, read-write
 * interleaved, stereo, the format and rate asked exactly: no plug layer unless the device's name asks for one). */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <alsa/asoundlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "out.h"
#include "alsa_hw.h"

struct out_pcm { snd_pcm_t *p; int bits; unsigned dev_rate; };

static void olog(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); fflush(stderr);
}
static snd_pcm_format_t alsa_fmt(int fmt) {
    return fmt == OUTF_DSD_U32_LE ? SND_PCM_FORMAT_DSD_U32_LE : fmt == OUTF_DSD_U32_BE ? SND_PCM_FORMAT_DSD_U32_BE : SND_PCM_FORMAT_S32_LE;
}

out_pcm *out_open(const char *dev, int fmt, unsigned rate, unsigned long period, unsigned periods, int quiet) {
    snd_pcm_t *p;
    int e = snd_pcm_open(&p, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (e < 0) { if (!quiet) olog("%s: %s", dev, snd_strerror(e)); return NULL; }
    {
        snd_pcm_info_t *pi;
        snd_pcm_info_alloca(&pi);
        char hw[16];
        snd_ctl_t *ctl;
        snd_ctl_card_info_t *ci;
        snd_ctl_card_info_alloca(&ci);
        if (snd_pcm_info(p, pi) == 0 && snd_pcm_info_get_card(pi) >= 0) {
            snprintf(hw, sizeof hw, "hw:%d", snd_pcm_info_get_card(pi));
            if (snd_ctl_open(&ctl, hw, 0) == 0) {
                const int own = snd_ctl_card_info(ctl, ci) == 0 && !strcmp(snd_ctl_card_info_get_driver(ci), "audiolaz");
                snd_ctl_close(ctl);
                if (own) { if (!quiet) olog("%s: the « audiolaz » card is this player's input, never its output", dev); snd_pcm_close(p); return NULL; }
            }
        }
    }
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(p, hw);
    unsigned r = rate, n = periods;
    snd_pcm_uframes_t per = period;
    if ((e = snd_pcm_hw_params_set_access(p, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (e = snd_pcm_hw_params_set_format(p, hw, alsa_fmt(fmt))) < 0 ||
        (e = snd_pcm_hw_params_set_channels(p, hw, 2)) < 0 ||
        (e = snd_pcm_hw_params_set_rate(p, hw, r, 0)) < 0 ||
        (e = snd_pcm_hw_params_set_period_size_near(p, hw, &per, 0)) < 0 ||
        (e = snd_pcm_hw_params_set_periods_near(p, hw, &n, 0)) < 0 ||
        (e = snd_pcm_hw_params(p, hw)) < 0) {
        if (!quiet) olog("%s at %u Hz: %s", dev, rate, snd_strerror(e));
        snd_pcm_close(p);
        return NULL;
    }
    out_pcm *o = malloc(sizeof *o);
    if (!o) { snd_pcm_close(p); return NULL; }
    o->p = p;

    o->dev_rate = 0;
    {
        snd_output_t *so;
        if (snd_output_buffer_open(&so) == 0) {
            if (snd_pcm_dump(p, so) == 0) {
                char *txt = NULL;
                const size_t tl = snd_output_buffer_string(so, &txt);
                for (size_t i = 0; txt && i < tl; ) {
                    const char *l = txt + i, *e = memchr(l, '\n', tl - i);
                    const size_t ll = e ? (size_t)(e - l) : tl - i;
                    char line[128];
                    snprintf(line, sizeof line, "%.*s", (int)(ll < sizeof line - 1 ? ll : sizeof line - 1), l);

                    const char *q = line + strspn(line, " \t");
                    if (!strncmp(q, "rate", 4) && (q[4] == ' ' || q[4] == '\t' || q[4] == ':')) {
                        q += 4; q += strspn(q, " \t");
                        if (*q == ':') { const unsigned long v = strtoul(q + 1, NULL, 10); if (v && v != rate) o->dev_rate = (unsigned)v; }
                    }
                    i += ll + 1;
                }
            }
            snd_output_close(so);
        }
    }
    /* what the card keeps of the 32-bit words (out_bits): its significant bits (a USB DAC's S32_LE often 24) */
    const int sb = fmt == OUTF_S32 ? snd_pcm_hw_params_get_sbits(hw) : 0;
    o->bits = sb >= 8 && sb < 32 ? sb : 32;
    return o;
}
long out_write(out_pcm *o, const int32_t *pcm, unsigned long n) {
    const snd_pcm_sframes_t w = snd_pcm_writei(o->p, pcm, n);
    return w == -EAGAIN ? OUT_AGAIN : (long)w;
}
void out_wait(out_pcm *o, int ms) { snd_pcm_wait(o->p, ms); }
long out_avail(out_pcm *o) { return (long)snd_pcm_avail_update(o->p); }
int out_delay(out_pcm *o, long *frames) {
    snd_pcm_sframes_t d;
    if (snd_pcm_delay(o->p, &d)) return -1;
    *frames = (long)d;
    return 0;
}
void out_params(out_pcm *o, unsigned long *buffer, unsigned long *period) {
    snd_pcm_uframes_t b = 0, p = 0;
    snd_pcm_get_params(o->p, &b, &p);
    *buffer = (unsigned long)b; *period = (unsigned long)p;
}
int out_recover(out_pcm *o, long e) { return o && e < 0 && e != OUT_AGAIN && snd_pcm_recover(o->p, (int)e, 1) == 0 ? 0 : -1; }
void out_close(out_pcm *o) {
    if (!o) return;
    snd_pcm_drop(o->p);
    snd_pcm_close(o->p);
    free(o);
}
int out_takes(const char *dev, int fmt, unsigned rate) {
    snd_pcm_t *p;
    if (snd_pcm_open(&p, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) return -1;
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    const int ok = snd_pcm_hw_params_any(p, hw) >= 0 && snd_pcm_hw_params_set_access(p, hw, SND_PCM_ACCESS_RW_INTERLEAVED) >= 0 &&
                   snd_pcm_hw_params_set_format(p, hw, alsa_fmt(fmt)) >= 0 && snd_pcm_hw_params_set_channels(p, hw, 2) >= 0 &&
                   snd_pcm_hw_params_test_rate(p, hw, rate, 0) == 0;
    snd_pcm_close(p);
    return ok;
}
const char *out_strerror(long e) { return snd_strerror((int)e); }
const char *out_fmt_name(int fmt) { return snd_pcm_format_name(alsa_fmt(fmt)); }
unsigned out_device_rate(out_pcm *o) { return o ? o->dev_rate : 0; }
int out_bits(out_pcm *o) { return o ? o->bits : 32; }
const char *out_device_name(out_pcm *o) { (void)o; return ""; }
const char *out_device_maker(out_pcm *o) { (void)o; return ""; }
const char *out_device_model(out_pcm *o) { (void)o; return ""; }
const char *out_default_dev(void) { return "plughw:0,0"; }
int out_dev_present(const char *dev) {
    const char *cd = dev ? strstr(dev, "CARD=") : NULL;
    if (!cd) return 1;
    char id[64];
    snprintf(id, sizeof id, "%s", cd + 5);
    id[strcspn(id, ",")] = 0;
    return snd_card_get_index(id) >= 0;
}

int out_card_ok(const char *v) {
    for (const char *p = v; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr(":=,_.-", *p))) return 0;
    return v[0] != 0;
}

/* The machine's playback devices, as JSON: read again 10 s apart at most (the page asks twice a second). */
static void clean_json(char *dst, size_t len, const char *src) {
    snprintf(dst, len, "%s", src ? src : "");
    for (char *p = dst; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
}
void out_list_json(char *out, size_t len) {
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    static char cache[8192] = "[]";   /* 8 kB: each device says its hw too */
    static time_t at = 0;
    pthread_mutex_lock(&lk);
    const time_t now = time(NULL);
    if (!at || now - at >= 10 || now < at) {
        at = now;
        size_t w = (size_t)snprintf(cache, sizeof cache, "[");
        int card = -1;
        while (snd_card_next(&card) == 0 && card >= 0) {
            char hw[16], id[64], cname[96], dname[96], item[520];
            snprintf(hw, sizeof hw, "hw:%d", card);
            snd_ctl_t *ctl;
            if (snd_ctl_open(&ctl, hw, 0) < 0) continue;
            snd_ctl_card_info_t *ci;
            snd_ctl_card_info_alloca(&ci);

            if (snd_ctl_card_info(ctl, ci) == 0 && strcmp(snd_ctl_card_info_get_id(ci), "Loopback")
                && strcmp(snd_ctl_card_info_get_driver(ci), "Loopback") && strcmp(snd_ctl_card_info_get_name(ci), "Loopback")
                && strcmp(snd_ctl_card_info_get_driver(ci), "audiolaz")) {
                clean_json(id, sizeof id, snd_ctl_card_info_get_id(ci));
                clean_json(cname, sizeof cname, snd_ctl_card_info_get_name(ci));
                char chw[96];
                alsa_card_hw(card, chw, sizeof chw);
                int dev = -1;
                while (snd_ctl_pcm_next_device(ctl, &dev) == 0 && dev >= 0) {
                    snd_pcm_info_t *pi;
                    snd_pcm_info_alloca(&pi);
                    snd_pcm_info_set_device(pi, (unsigned)dev);
                    snd_pcm_info_set_subdevice(pi, 0);
                    snd_pcm_info_set_stream(pi, SND_PCM_STREAM_PLAYBACK);
                    if (snd_ctl_pcm_info(ctl, pi) < 0) continue;
                    clean_json(dname, sizeof dname, snd_pcm_info_get_name(pi));

                    const int n = snprintf(item, sizeof item, "%s{\"id\":\"alsa:hw:CARD=%s,DEV=%d\",\"card\":\"%s\",\"name\":\"%s\",\"hw\":\"%s\"}",
                                           w > 1 ? "," : "", id, dev, cname, dname, chw);
                    if (n > 0 && w + (size_t)n + 2 < sizeof cache) { memcpy(cache + w, item, (size_t)n); w += (size_t)n; }
                }
            }
            snd_ctl_close(ctl);
        }
        snprintf(cache + w, sizeof cache - w, "]");
    }
    snprintf(out, len, "%s", cache);
    pthread_mutex_unlock(&lk);
}
int out_option(const char *name, const char *value) { (void)name; (void)value; return 0; }
void out_print_devices(void) { char b[8192]; out_list_json(b, sizeof b); puts(b); }
