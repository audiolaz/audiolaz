/* src/player/out_alsa.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
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
#include "azgrow.h"
#include "pcmpack.h"

struct out_pcm {
    snd_pcm_t *p; int bits; unsigned dev_rate;
    /* --grow-ms: the buffer (hwbuf) and the level it is filled to (lvl, azgrow); the lowest level seen since
     * the last look; a line for the journal (out_said, never written by the writer) */
    int grow; unsigned rate; snd_pcm_uframes_t hwbuf, per, lvl, lvl0;
    long low; double t_look; azgrow g;
    char say[240]; int say_n;
    /* PCM in a format of its own (pcmpack.h): its PCMF entry, its bits kept; pk: the words packed (not the card's
     * S32_LE as they are), in pkbuf (pkcap frames, made at the open: nothing allocated by the writer) */
    int fi, pk; unsigned long pkcap; uint8_t *pkbuf; uint64_t rng;
    /* (22/10, the review) DoP told by its markers (pk_dop): its last frame's marker; said once (never played on a card
     * keeping fewer than 24 bits) */
    uint8_t dop_last; int dop_said;
    unsigned long dop_run; unsigned dop_rate;   /* its frames of DoP in a row, the rate (pcmpack.h pk_dop_mute) */
    snd_pcm_format_t f;                  /* the format it plays in (out_format) */
};
/* The PCM formats a card may take for the player's 32-bit words, best first (the first the card takes at the
 * rate wins): bytes a sample, its width (the word's top bits), big-endian */
static const struct { snd_pcm_format_t f; int bytes, width, be; } PCMF[] = {
    { SND_PCM_FORMAT_S32_LE, 4, 32, 0 }, { SND_PCM_FORMAT_S24_LE, 4, 24, 0 }, { SND_PCM_FORMAT_S24_3LE, 3, 24, 0 }, { SND_PCM_FORMAT_S16_LE, 2, 16, 0 },
    { SND_PCM_FORMAT_S32_BE, 4, 32, 1 }, { SND_PCM_FORMAT_S24_BE, 4, 24, 1 }, { SND_PCM_FORMAT_S24_3BE, 3, 24, 1 }, { SND_PCM_FORMAT_S16_BE, 2, 16, 1 } };
#define NPCMF ((int)(sizeof PCMF / sizeof PCMF[0]))
/* The first of PCMF this configuration (access and channels set) takes at the rate: its index, *bits its bits kept (the
 * width, or the card's msbits when fewer); −1 none. hw left as it was. */
static int pcm_pick(snd_pcm_t *p, const snd_pcm_hw_params_t *hw, unsigned rate, int *bits) {
    snd_pcm_hw_params_t *t;
    snd_pcm_hw_params_alloca(&t);
    for (int i = 0; i < NPCMF; i++) {
        snd_pcm_hw_params_copy(t, hw);
        if (snd_pcm_hw_params_set_format(p, t, PCMF[i].f) < 0 || snd_pcm_hw_params_set_rate(p, t, rate, 0) < 0) continue;
        const int sb = snd_pcm_hw_params_get_sbits(t);
        if (bits) *bits = sb >= 8 && sb < PCMF[i].width ? sb : PCMF[i].width;
        return i;
    }
    return -1;
}
static int opt_grow_ms = 0;
static double mono_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
/* the level to wake at: a period under the level (avail_min = the buffer less the level, plus a period) */
static void lvl_apply(out_pcm *o) {
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    if (snd_pcm_sw_params_current(o->p, sw) < 0) return;
    snd_pcm_uframes_t am = o->hwbuf - o->lvl + o->per;
    if (am > o->hwbuf) am = o->hwbuf;
    snd_pcm_sw_params_set_avail_min(o->p, sw, am);
    snd_pcm_sw_params(o->p, sw);
}

static void olog(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); fflush(stderr);
}
static snd_pcm_format_t alsa_fmt(int fmt) {
    return fmt == OUTF_DSD_U32_LE ? SND_PCM_FORMAT_DSD_U32_LE : fmt == OUTF_DSD_U32_BE ? SND_PCM_FORMAT_DSD_U32_BE : SND_PCM_FORMAT_S32_LE;
}

/* --grow-ms: the buffer as long as that (at least the periods asked) */
static int grow_buffer(snd_pcm_t *p, snd_pcm_hw_params_t *hw, unsigned rate, snd_pcm_uframes_t per, unsigned n) {
    snd_pcm_uframes_t b = (snd_pcm_uframes_t)rate * (unsigned)opt_grow_ms / 1000;
    if (b < per * n) b = per * n;
    int e = snd_pcm_hw_params_set_buffer_size_near(p, hw, &b);
    if (e < 0) { unsigned m = n; e = snd_pcm_hw_params_set_periods_near(p, hw, &m, 0); }
    return e;
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
    int fi = -1, fbits = 32, nofmt = 0;
    if ((e = snd_pcm_hw_params_set_access(p, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (e = snd_pcm_hw_params_set_channels(p, hw, 2)) < 0 ||
        /* PCM (DoP too): the best format it takes at this rate */
        (fmt == OUTF_S32 && (fi = pcm_pick(p, hw, rate, &fbits)) < 0 && (nofmt = 1) && (e = -EINVAL) < 0) ||
        (e = snd_pcm_hw_params_set_format(p, hw, fi >= 0 ? PCMF[fi].f : alsa_fmt(fmt))) < 0 ||
        (e = snd_pcm_hw_params_set_rate(p, hw, r, 0)) < 0 ||
        (e = snd_pcm_hw_params_set_period_size_near(p, hw, &per, 0)) < 0 ||
        (e = opt_grow_ms > 0 ? 0 : snd_pcm_hw_params_set_periods_near(p, hw, &n, 0)) < 0 ||
        (opt_grow_ms > 0 && (e = grow_buffer(p, hw, rate, per, n)) < 0) ||
        (e = snd_pcm_hw_params(p, hw)) < 0) {
        /* (22/10, the review) the formats named only when they were the refusal (not the access, the channels…) */
        if (!quiet) olog("%s at %u Hz: %s%s", dev, rate, snd_strerror(e), nofmt ? " (none of S32, S24, S24_3, S16, little- or big-endian, at this rate)" : "");
        snd_pcm_close(p);
        return NULL;
    }
    out_pcm *o = calloc(1, sizeof *o);
    if (!o) { snd_pcm_close(p); return NULL; }
    o->p = p;
    o->fi = fi;
    o->dop_rate = rate;
    o->f = fi >= 0 ? PCMF[fi].f : alsa_fmt(fmt);
    if (opt_grow_ms > 0) {                             /* the level, from what was asked */
        snd_pcm_uframes_t b = 0, pp = 0;
        snd_pcm_get_params(p, &b, &pp);
        o->grow = b > 0 && pp > 0; o->rate = rate; o->hwbuf = b; o->per = pp;
        o->lvl0 = (snd_pcm_uframes_t)period * periods;
        if (o->lvl0 < 2 * pp) o->lvl0 = 2 * pp;
        if (o->lvl0 > b) o->lvl0 = b;
        o->lvl = o->lvl0; o->low = -1;
        azg_init(&o->g, (double)o->lvl0, (double)b, rate, mono_s());
        if (o->grow) lvl_apply(o);
    }

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
    /* what the card keeps of the 32-bit words (out_bits): its significant bits (a USB DAC's S32_LE often 24);
     * in its own format: that format's width at most (S16_LE: 16) */
    o->bits = 32;
    if (fi >= 0) {
        const int sb = snd_pcm_hw_params_get_sbits(hw);
        o->bits = sb >= 8 && sb < PCMF[fi].width ? sb : PCMF[fi].width;
        if (fbits < o->bits) o->bits = fbits;     /* as pcm_pick found it (out_takes_bits says the same) */
        o->pk = fi != 0 || o->bits < 32;          /* packed unless S32_LE kept whole (the writes as before) */
        o->rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)o ^ ((uint64_t)rate << 32);
        if (o->pk) {   /* its pages touched here, never first by the writer (real time) */
            o->pkcap = 2048;
            if (!(o->pkbuf = malloc(o->pkcap * 2 * (size_t)PCMF[fi].bytes))) { snd_pcm_close(p); free(o); return NULL; }
            memset(o->pkbuf, 0, o->pkcap * 2 * (size_t)PCMF[fi].bytes);
        }
        if (fi != 0) {   /* said once per device, format and rate (a play opens it again at each track) */
            static char said[200];
            char now_[200];
            snprintf(now_, sizeof now_, "%s %d %u", dev, fi, rate);
            if (strcmp(said, now_)) {
                snprintf(said, sizeof said, "%s", now_);
                olog("%s at %u Hz: it takes no S32_LE: %s, %d bits kept (a word with more dithered once, TPDF; one with as many "
                     "or fewer bit for bit)", dev, rate, snd_pcm_format_name(PCMF[fi].f), o->bits);
            }
        }
    }
    return o;
}
long out_write(out_pcm *o, const int32_t *pcm, unsigned long n) {
    if (o->pk && o->fi == 0 && o->bits >= 24) {   /* S32_LE whose msbits are fewer: as it is when every word is on
                                                    * that grid (fewer than 24: packed, below, where DoP is looked for) */
        const uint32_t low = ((uint32_t)1 << (32 - o->bits)) - 1;
        unsigned long i = 0;
        while (i < 2 * n && !((uint32_t)pcm[i] & low)) i++;
        if (i == 2 * n) { const snd_pcm_sframes_t w = snd_pcm_writei(o->p, pcm, n); return w == -EAGAIN ? OUT_AGAIN : (long)w; }
    }
    if (!o->pk) {
        const snd_pcm_sframes_t w = snd_pcm_writei(o->p, pcm, n);
        return w == -EAGAIN ? OUT_AGAIN : (long)w;
    }
    /* packed into its format, pkcap frames at a time: what it took (frames packed and not taken are packed again
     * at the next write, dithered anew: nothing played twice). (22/10, the review) DoP never dithered, whatever the
     * player decided before: told by its markers (pk_dop), its words packed with dither 0 (on the grid of 24 bits: as
     * they are); a card keeping fewer than 24 bits plays silence instead (its one-bit samples cut would be noise), said
     * once (roomd.c and the receiver refuse DoP there before: this is the last guard) */
    unsigned long done = 0;
    while (done < n) {
        const unsigned long m = n - done < o->pkcap ? n - done : o->pkcap;
        const uint8_t was = o->dop_last;
        const unsigned long run0 = o->dop_run;
        /* at 176.4 kHz or more only, silence once 20 ms of it in a row (pk_dop_mute): music never muted on
         * its top bytes alone */
        const int dop = pk_dop_at(o->dop_rate, pcm + 2 * done, m, &o->dop_last);
        const int mute = pk_dop_mute(dop, m, o->dop_rate, &o->dop_run);
        if (mute && o->bits < 24) {
            memset(o->pkbuf, 0, m * 2 * (size_t)PCMF[o->fi].bytes);
            if (!o->dop_said++) olog("DoP to a card that keeps %d bits (%s): silence instead (its one-bit samples would be noise; DoP needs 24)", o->bits, snd_pcm_format_name(PCMF[o->fi].f));
        } else pk_pack(o->pkbuf, pcm + 2 * done, 2 * m, PCMF[o->fi].bytes, PCMF[o->fi].width, PCMF[o->fi].be, o->bits, !dop, &o->rng);
        const snd_pcm_sframes_t w = snd_pcm_writei(o->p, o->pkbuf, m);
        if (w < 0) { o->dop_last = was; o->dop_run = run0; return done ? (long)done : w == -EAGAIN ? OUT_AGAIN : (long)w; }
        done += (unsigned long)w;
        if ((unsigned long)w < m) {   /* the rest written again next time: the marker of the last frame it took */
            o->dop_last = dop && w > 0 ? (uint8_t)((uint32_t)pcm[2 * (done - 1)] >> 24) : was;
            if (dop) o->dop_run = run0 + (unsigned long)w;
            break;
        }
    }
    return (long)done;
}
void out_wait(out_pcm *o, int ms) { snd_pcm_wait(o->p, ms); }
/* --grow-ms: the room below the level; how low it came (the writer comes after its wake: the lowest), looked
 * at every 20 ms at most (azgrow.h) */
static long grow_avail(out_pcm *o, long hw) {
    const long fill = (long)o->hwbuf - hw;
    if (snd_pcm_state(o->p) == SND_PCM_STATE_RUNNING && (o->low < 0 || fill < o->low)) o->low = fill;
    const double t = mono_s();
    if (o->low >= 0 && t - o->t_look >= 0.02) {
        o->t_look = t;
        char why[200];
        const int c = azg_look(&o->g, (double)o->low, 0, t, why, sizeof why);
        o->low = -1;
        if (c) {
            o->lvl = (snd_pcm_uframes_t)o->g.size;
            if (o->lvl > o->hwbuf) o->lvl = o->hwbuf;
            lvl_apply(o);
            if (!o->say_n) { snprintf(o->say, sizeof o->say, "the DAC's buffer %s", why); o->say_n = 1; }
        }
    }
    const long room = (long)o->lvl - fill;
    return room > 0 ? room : 0;
}
long out_avail(out_pcm *o) {
    const long a = (long)snd_pcm_avail_update(o->p);
    return o->grow && a >= 0 ? grow_avail(o, a) : a;
}
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
void out_drain(out_pcm *o) { (void)o; }   /* out.h */
long out_gaps(out_pcm *o) { (void)o; return 0; }
double out_grown_ms(out_pcm *o) { return o && o->grow && o->rate ? ((double)o->lvl - (double)o->lvl0) * 1000.0 / o->rate : 0; }
int out_said(out_pcm *o, char *buf, size_t len) {
    if (!o || !o->say_n) return 0;
    snprintf(buf, len, "%s", o->say); o->say_n = 0;
    return 1;
}
int out_recover(out_pcm *o, long e) { return o && e < 0 && e != OUT_AGAIN && snd_pcm_recover(o->p, (int)e, 1) == 0 ? 0 : -1; }
void out_close(out_pcm *o) {
    if (!o) return;
    snd_pcm_drop(o->p);
    snd_pcm_close(o->p);
    free(o->pkbuf);
    free(o);
}
/* PCM (DoP too): any format of PCMF at this rate; *bits: the bits kept then */
static int takes(const char *dev, int fmt, unsigned rate, int *bits) {
    snd_pcm_t *p;
    if (snd_pcm_open(&p, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) return -1;
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    int ok = snd_pcm_hw_params_any(p, hw) >= 0 && snd_pcm_hw_params_set_access(p, hw, SND_PCM_ACCESS_RW_INTERLEAVED) >= 0 &&
             snd_pcm_hw_params_set_channels(p, hw, 2) >= 0;
    if (ok && fmt == OUTF_S32) ok = pcm_pick(p, hw, rate, bits) >= 0;
    else if (ok) ok = snd_pcm_hw_params_set_format(p, hw, alsa_fmt(fmt)) >= 0 && snd_pcm_hw_params_test_rate(p, hw, rate, 0) == 0;
    snd_pcm_close(p);
    return ok;
}
int out_takes(const char *dev, int fmt, unsigned rate) { return takes(dev, fmt, rate, NULL); }
int out_takes_bits(const char *dev, unsigned rate) { int b = 0; const int t = takes(dev, OUTF_S32, rate, &b); return t == 1 ? b : t < 0 ? -1 : 0; }

const char *out_format(out_pcm *o) { return o ? snd_pcm_format_name(o->f) : ""; }
const char *out_strerror(long e) { return snd_strerror((int)e); }
const char *out_fmt_name(int fmt) { return snd_pcm_format_name(alsa_fmt(fmt)); }
unsigned out_device_rate(out_pcm *o) { return o ? o->dev_rate : 0; }
int out_bits(out_pcm *o) { return o ? o->bits : 32; }
const char *out_device_name(out_pcm *o) { (void)o; return ""; }
const char *out_device_maker(out_pcm *o) { (void)o; return ""; }
const char *out_device_model(out_pcm *o) { (void)o; return ""; }
const char *out_default_dev(void) { return "plughw:0,0"; }
/* out.h: hw: plays a rate as it is or not at all (native only by itself); why an open failed is said by the
 * caller from its own error; the card there or not; the rates it takes, asked one by one (its device opened: never
 * while another program holds it, −1 then) */
void out_native_only(int on) { (void)on; }
int out_last_refusal(void) { return OUT_REF_NONE; }
unsigned long out_dev_serial(const char *dev) { return out_dev_present(dev) ? 1 : 0; }
int out_rates_text(const char *dev, char *out, size_t len) {
    static const unsigned R[] = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000, 1411200, 1536000 };
    if (len) out[0] = 0;
    size_t w = 0;
    for (size_t i = 0; i < sizeof R / sizeof R[0] && w + 12 < len; i++) {
        const int t = out_takes(dev, OUTF_S32, R[i]);
        if (t < 0) return -1;
        if (t == 1) w += (size_t)snprintf(out + w, len - w, "%s%u", w ? "," : "", R[i]);
    }
    return (int)w;
}
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
int out_option(const char *name, const char *value) {
    if (!strcmp(name, "--grow-ms")) { opt_grow_ms = atoi(value); if (opt_grow_ms < 0) opt_grow_ms = 0; if (opt_grow_ms > 2000) opt_grow_ms = 2000; return 1; }   /* */
    return 0;
}
void out_print_devices(void) { char b[8192]; out_list_json(b, sizeof b); puts(b); }
