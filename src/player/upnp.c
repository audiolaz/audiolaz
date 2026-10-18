/* src/player/upnp.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <uuid/uuid.h>
#endif
#include "upnp.h"
#include "netout.h"
#include "version.h"

#define UPNP_PORT 49494
#define ANNOUNCE_S 120.0
#define MAX_AGE 1800
#define NSUB 32
#define SUB_MAX_S 1800

/* ── Settings ─────────────────────────────────────────────────────────────────────────────────────────────────── */
static int en_cmd = 0;                 /* --upnp on */
static atomic_int en_live = 1;         /* upnp= in the control (the app's switch) */
static int vol_digital_cmd = 0;        /* --upnp-volume digital */
static atomic_int vol_digital = 0;
static int http_port = UPNP_PORT;
static char ifname[64] = "";           /* --upnp-interface */
static in_addr_t if_only = 0;          /* its IPv4 (network order), 0: every interface */
static char name_[128] = "audiolaz";
static char name_opt[128] = "";        /* --upnp-name */
static char uuid_opt[56] = "";
static char udn[72] = "";
static char rooms_[1024] = ".";
static int started = 0;

int upnp_enabled(void) { return en_cmd; }
const char *upnp_udn(void) { return udn; }
void upnp_set_name(const char *n) { if (n && *n) snprintf(name_, sizeof name_, "%s", n); }
int upnp_option(const char *name, const char *value) {
    if (!strcmp(name, "--upnp")) en_cmd = !strcmp(value, "on") || !strcmp(value, "1");
    else if (!strcmp(name, "--upnp-port")) { http_port = atoi(value); if (http_port <= 0 || http_port > 65535) http_port = UPNP_PORT; }
    else if (!strcmp(name, "--upnp-interface")) snprintf(ifname, sizeof ifname, "%s", value);
    else if (!strcmp(name, "--upnp-name")) snprintf(name_opt, sizeof name_opt, "%s", value);
    else if (!strcmp(name, "--upnp-uuid")) snprintf(uuid_opt, sizeof uuid_opt, "%s", value);
    else if (!strcmp(name, "--upnp-volume")) vol_digital_cmd = !strcmp(value, "digital");
    else return 0;
    return 1;
}
static const char *fname(void) { return name_opt[0] ? name_opt : name_; }

/* ── XML ──────────────────────────────────────────────────────────────────────────────────────────────────────── */
static void xesc(const char *s, char *out, size_t len) {
    size_t w = 0;
    for (; s && *s && w + 7 < len; s++) {
        const char *e = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : *s == '\'' ? "&apos;" : NULL;
        if (e) { const size_t l = strlen(e); memcpy(out + w, e, l); w += l; } else out[w++] = *s;
    }
    if (len) out[w] = 0;
}
static char *xesc_dup(const char *s) {
    const size_t n = s ? strlen(s) : 0;
    char *o = malloc(6 * n + 8);
    if (o) xesc(s ? s : "", o, 6 * n + 8);
    return o;
}
/* entities undone in place (named and numeric); CDATA kept as it is */
static void xunesc(char *s) {
    char *w = s;
    for (char *p = s; *p; ) {
        if (*p == '&') {
            static const struct { const char *e; char c; } ENT[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' } };
            size_t k = 0;
            for (; k < sizeof ENT / sizeof *ENT; k++) if (!strncmp(p, ENT[k].e, strlen(ENT[k].e))) { *w++ = ENT[k].c; p += strlen(ENT[k].e); break; }
            if (k < sizeof ENT / sizeof *ENT) continue;
            if (p[1] == '#') {
                char *e;
                unsigned long c = p[2] == 'x' || p[2] == 'X' ? strtoul(p + 3, &e, 16) : strtoul(p + 2, &e, 10);
                if (*e == ';' && c > 0 && c < 0x110000) {
                    if (c < 0x80) *w++ = (char)c;
                    else if (c < 0x800) { *w++ = (char)(0xC0 | c >> 6); *w++ = (char)(0x80 | (c & 0x3F)); }
                    else if (c < 0x10000) { *w++ = (char)(0xE0 | c >> 12); *w++ = (char)(0x80 | (c >> 6 & 0x3F)); *w++ = (char)(0x80 | (c & 0x3F)); }
                    else { *w++ = (char)(0xF0 | c >> 18); *w++ = (char)(0x80 | (c >> 12 & 0x3F)); *w++ = (char)(0x80 | (c >> 6 & 0x3F)); *w++ = (char)(0x80 | (c & 0x3F)); }
                    p = e + 1;
                    continue;
                }
            }
        }
        *w++ = *p++;
    }
    *w = 0;
}
/* The text of the first element named tag (any namespace prefix, any attributes), entities undone, malloc'd; NULL when
 * absent. attrs: its attributes' text (malloc'd) when not NULL. */
static char *xel(const char *x, const char *tag, char **attrs) {
    const size_t tl = strlen(tag);
    if (attrs) *attrs = NULL;
    for (const char *p = x; p && (p = strchr(p, '<')); p++) {
        const char *n = p + 1;
        if (*n == '/' || *n == '?' || *n == '!') continue;
        const char *c = strchr(n, ':'), *sp = n + strcspn(n, " \t\r\n/>");
        if (c && c < sp) n = c + 1;
        if (strncmp(n, tag, tl) || (n[tl] != '>' && n[tl] != ' ' && n[tl] != '/' && n[tl] != '\t' && n[tl] != '\r' && n[tl] != '\n')) continue;
        const char *gt = strchr(n, '>');
        if (!gt) return NULL;
        if (attrs) { const size_t al = (size_t)(gt - (n + tl)); *attrs = malloc(al + 1); if (*attrs) { memcpy(*attrs, n + tl, al); (*attrs)[al] = 0; } }
        if (gt[-1] == '/') { char *e = strdup(""); return e; }
        const char *v = gt + 1;
        /* its end: </prefix:tag> or </tag> */
        const char *e = v;
        for (;;) {
            e = strstr(e, "</");
            if (!e) return NULL;
            const char *m = e + 2, *cc = strchr(m, ':'), *g2 = strchr(m, '>');
            if (cc && g2 && cc < g2) m = cc + 1;
            if (!strncmp(m, tag, tl) && m[tl] == '>') break;
            e += 2;
        }
        size_t l = (size_t)(e - v);
        if (l >= 9 && !strncmp(v, "<![CDATA[", 9)) {
            const char *ce = strstr(v + 9, "]]>");
            if (ce && ce < e) { char *o = malloc((size_t)(ce - v - 9) + 1); if (o) { memcpy(o, v + 9, (size_t)(ce - v - 9)); o[ce - v - 9] = 0; } return o; }
        }
        char *o = malloc(l + 1);
        if (!o) return NULL;
        memcpy(o, v, l); o[l] = 0;
        xunesc(o);
        return o;
    }
    return NULL;
}
static void xel_to(const char *x, const char *tag, char *out, size_t len) {
    char *v = xel(x, tag, NULL);
    snprintf(out, len, "%s", v ? v : "");
    free(v);
    size_t a = 0, n = strlen(out);
    while (a < n && isspace((unsigned char)out[a])) a++;
    while (n > a && isspace((unsigned char)out[n - 1])) n--;
    memmove(out, out + a, n - a); out[n - a] = 0;
}
static void attr_of(const char *attrs, const char *name, char *out, size_t len) {
    out[0] = 0;
    if (!attrs) return;
    const size_t nl = strlen(name);
    for (const char *p = attrs; (p = strstr(p, name)); p += nl) {
        if (p > attrs && !isspace((unsigned char)p[-1])) continue;
        const char *q = p + nl;
        while (*q == ' ') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ') q++;
        const char qc = *q;
        if (qc != '"' && qc != '\'') continue;
        const char *e = strchr(q + 1, qc);
        if (!e) return;
        snprintf(out, len, "%.*s", (int)(e - q - 1), q + 1);
        xunesc(out);
        return;
    }
}

/* ── The transport ────────────────────────────────────────────────────────────────────────────────────────────── */
enum { ST_NOMEDIA, ST_STOPPED, ST_PLAYING, ST_PAUSED, ST_TRANSITIONING };
static const char *const ST_NAME[] = { "NO_MEDIA_PRESENT", "STOPPED", "PLAYING", "PAUSED_PLAYBACK", "TRANSITIONING" };
typedef struct {
    int set, seq;
    char uri[2048];
    char *meta;                        /* the DIDL-Lite as given (NULL: none) */
    char mime[160];
    char title[256], artist[256], album[256], art[1024];
    double dur;                        /* s, 0 not known */
} track;
static struct {
    pthread_mutex_t m;
    pthread_cond_t cv;
    int state;
    char status[24];
    track cur, next;
    int seq;                           /* the tracks' numbers */
    int queued;                        /* the last track handed to the decoder (its seq) */
    unsigned gen;                      /* the queue's generation of this play */
    double held_pos;                   /* the position said when not playing (s) */
    int vol, mute;
    int av_ev, rc_ev;                  /* a change to tell the subscribers */
    int np_seq;                        /* the track whose title went to nowplay.h */
} T = { .m = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .state = ST_NOMEDIA, .status = "OK", .vol = 100, .queued = -1, .np_seq = -1 };
static pthread_cond_t ev_cv = PTHREAD_COND_INITIALIZER;

static void track_free(track *t) { free(t->meta); memset(t, 0, sizeof *t); }
static double hms_s(const char *s) {
    if (!s || !*s) return -1;
    double h = 0, m = 0, sec = 0;
    if (sscanf(s, "%lf:%lf:%lf", &h, &m, &sec) == 3) return h * 3600 + m * 60 + sec;
    if (sscanf(s, "%lf:%lf", &m, &sec) == 2) return m * 60 + sec;
    return atof(s);
}
static void s_hms(double s, char *out, size_t len) {
    if (!(s >= 0)) s = 0;
    const long t = (long)s;
    snprintf(out, len, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
}
/* A track from SetAVTransportURI's (or the next's) URI and DIDL-Lite */
static void track_set(track *t, const char *uri, const char *meta) {
    track_free(t);
    t->set = 1;
    snprintf(t->uri, sizeof t->uri, "%s", uri ? uri : "");
    if (meta && *meta) t->meta = strdup(meta);
    if (!t->meta) return;
    xel_to(t->meta, "title", t->title, sizeof t->title);
    xel_to(t->meta, "artist", t->artist, sizeof t->artist);
    if (!t->artist[0]) xel_to(t->meta, "creator", t->artist, sizeof t->artist);
    xel_to(t->meta, "album", t->album, sizeof t->album);
    xel_to(t->meta, "albumArtURI", t->art, sizeof t->art);
    /* its res: the one whose URL is this URI, else the first; protocolInfo's MIME type, duration */
    char *at = NULL, *res = NULL;
    const char *p = t->meta;
    for (int i = 0; i < 16; i++) {
        char *a2 = NULL, *r2 = xel(p, "res", &a2);
        if (!r2) { free(a2); break; }
        char *trim = r2; while (isspace((unsigned char)*trim)) trim++;
        size_t tl = strlen(trim); while (tl && isspace((unsigned char)trim[tl - 1])) trim[--tl] = 0;
        const int hit = !strcmp(trim, t->uri);
        if (!res || hit) { free(res); free(at); res = r2; at = a2; } else { free(r2); free(a2); }
        if (hit) break;
        const char *nx = strstr(p, "res");
        if (!nx) break;
        const char *close = strstr(nx, "/res>");
        p = close ? close + 5 : nx + 3;
    }
    char pi[256], du[64];
    attr_of(at, "protocolInfo", pi, sizeof pi);
    attr_of(at, "duration", du, sizeof du);
    if (pi[0]) { const char *a = strchr(pi, ':'), *b = a ? strchr(a + 1, ':') : NULL, *c = b ? strchr(b + 1, ':') : NULL;
                 if (b && c) snprintf(t->mime, sizeof t->mime, "%.*s", (int)(c - b - 1), b + 1); }
    if (du[0]) t->dur = hms_s(du);
    free(at); free(res);
}
static void av_changed(void) { T.av_ev = 1; pthread_cond_broadcast(&ev_cv); }   /* under T.m */

/* What plays told to nowplay.h (the page, the receivers' screens), and its cover fetched */
typedef struct { char url[1024]; int seq; } art_job;
static void *art_main(void *arg) {
    art_job *a = arg;
    unsigned char *img = NULL;
    size_t n = 0;
    if (a->url[0] && !ud_fetch(a->url, &img, &n, 8u << 20)) {
        pthread_mutex_lock(&T.m);
        const int still = T.np_seq == a->seq;
        pthread_mutex_unlock(&T.m);
        if (still) netout_np_cover(NETOUT_NP_UPNP, img, n);
    }
    free(img); free(a);
    return NULL;
}
static void np_tell(const track *t) {   /* under T.m */
    if (!t->set || T.np_seq == t->seq) return;
    T.np_seq = t->seq;
    netout_np_text(NETOUT_NP_UPNP, t->title, t->artist, t->album);
    netout_np_cover(NETOUT_NP_UPNP, NULL, 0);
    if (t->art[0]) {
        art_job *a = calloc(1, sizeof *a);
        pthread_t th;
        if (a) { snprintf(a->url, sizeof a->url, "%s", t->art); a->seq = t->seq;
                 if (!pthread_create(&th, NULL, art_main, a)) pthread_detach(th); else free(a); }
    }
}

/* A play of the current track from s seconds (under T.m) */
static void play_from(double s) {
    T.gen = uq_flush();
    ud_job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->gen = T.gen; j->seq = T.cur.seq; j->seek_s = s;
    snprintf(j->uri, sizeof j->uri, "%s", T.cur.uri);
    snprintf(j->mime, sizeof j->mime, "%s", T.cur.mime);
    T.queued = T.cur.seq;
    T.held_pos = s;
    ud_start(j);
    free(j);
    uq_started();
    np_tell(&T.cur);
    pthread_cond_broadcast(&T.cv);
}
static void stop_now(void) {   /* under T.m */
    T.gen = uq_flush();
    T.held_pos = 0;
    if (T.state == ST_PLAYING || T.state == ST_PAUSED || T.state == ST_TRANSITIONING) T.state = T.cur.set ? ST_STOPPED : ST_NOMEDIA;
    pthread_cond_broadcast(&T.cv);
}
void upnp_input_stop(void) {
    pthread_mutex_lock(&T.m);
    if (T.state == ST_PLAYING || T.state == ST_PAUSED) { ulog("UPnP: another input took over: stopped"); stop_now(); av_changed(); }
    pthread_mutex_unlock(&T.m);
}

/* The decoder at a track's end (upnp.h) */
int upnp_next(unsigned gen, int seq, ud_job *j) {
    (void)seq;
    pthread_mutex_lock(&T.m);
    for (;;) {
        if (gen != uq_gen() || (T.state != ST_PLAYING && T.state != ST_PAUSED)) break;
        if (T.next.set && T.next.seq > T.queued && T.next.uri[0]) {
            memset(j, 0, sizeof *j);
            j->gen = gen; j->seq = T.next.seq;
            snprintf(j->uri, sizeof j->uri, "%s", T.next.uri);
            snprintf(j->mime, sizeof j->mime, "%s", T.next.mime);
            T.queued = T.next.seq;
            pthread_mutex_unlock(&T.m);
            ulog("UPnP: next track (%d) decoded on the same run (gapless)", j->seq);
            return 1;
        }
        uq_at a;
        uq_where(&a);
        if (a.drained) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&T.cv, &T.m, &ts);
    }
    pthread_mutex_unlock(&T.m);
    return 0;
}
void upnp_failed(unsigned gen, int seq, const char *why) {
    pthread_mutex_lock(&T.m);
    if (gen == T.gen) {
        if (T.next.set && seq == T.next.seq) { ulog("UPnP: the next track not played (%s): left out", why); track_free(&T.next); }
        else if (seq == T.cur.seq) {
            uq_at a;
            uq_where(&a);
            if (!a.any) {                                 /* nothing of it played: the control point told */
                stop_now();
                snprintf(T.status, sizeof T.status, "ERROR_OCCURRED");
            }
        }
        av_changed();
    }
    pthread_mutex_unlock(&T.m);
}
void upnp_duration(int seq, double s) {
    pthread_mutex_lock(&T.m);
    if (T.cur.set && T.cur.seq == seq && !(T.cur.dur > 0)) { T.cur.dur = s; av_changed(); }
    else if (T.next.set && T.next.seq == seq && !(T.next.dur > 0)) T.next.dur = s;
    pthread_mutex_unlock(&T.m);
}
double upnp_gain(void) {
    pthread_mutex_lock(&T.m);
    const int mute = T.mute, v = T.vol;
    pthread_mutex_unlock(&T.m);
    if (mute) return 0.0;
    if (!atomic_load(&vol_digital) || v >= 100) return 1.0;
    if (v <= 0) return 0.0;
    return pow(10.0, 40.0 * log10(v / 100.0) / 20.0);   /* 50: −12 dB, 10: −40 dB */
}
/* Where the current track is (s) */
/* (the music the player read, less what it holds: its latency, input to output, as its status says it, 1 s until it is
 * measured; never before the place the play started from) */
static double lat_s = 1.0;
static double pos_now(void) {   /* under T.m */
    if (T.state != ST_PLAYING && T.state != ST_PAUSED) return T.held_pos;
    uq_at a;
    uq_where(&a);
    if (a.seq == T.cur.seq && a.rate) { const double p = (double)a.frame / a.rate - lat_s, f = (double)a.first / a.rate; return p > f ? p : f; }
    return T.held_pos;
}
static void latency_read(void) {
    char p[1100], line[256];
    snprintf(p, sizeof p, "%s/status", rooms_);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "input_latency_ms=", 17)) { const double v = atof(line + 17); if (v > 0 && v < 30000) lat_s = v / 1000.0; }
    fclose(f);
}

/* ── Its state, once every 100 ms: the next track reached, the music's end, the switch ────────────────────────────── */
static void control_read(void) {
    static long long mt = -1;
    char p[1100], line[256];
    snprintf(p, sizeof p, "%s/control", rooms_);
    struct stat st;
    if (stat(p, &st)) return;
    const long long m = (long long)st.st_mtime * 1000000000LL + 0;
    if (m == mt) return;
    mt = m;
    FILE *f = fopen(p, "r");
    if (!f) return;
    int on = 1, dig = vol_digital_cmd;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "upnp=", 5)) on = strcmp(line + 5, "0") && strcmp(line + 5, "off");
        else if (!strncmp(line, "upnp_volume=", 12)) dig = !strcmp(line + 12, "digital");
    }
    fclose(f);
    if (atomic_load(&vol_digital) != dig) { atomic_store(&vol_digital, dig); ulog("UPnP: the control point's volume %s", dig ? "applied (digital)" : "not applied (fixed, bit for bit)"); }
    if (atomic_load(&en_live) != on) {
        atomic_store(&en_live, on);
        ulog("UPnP input: %s", on ? "on" : "off (switched off: silent on the network)");
        if (!on) { pthread_mutex_lock(&T.m); stop_now(); av_changed(); pthread_mutex_unlock(&T.m); }
    }
}
static void *tick_main(void *arg) {
    (void)arg;
    double t_ctl = 0;
    for (;;) {
        usleep(100000);
        if (umono() - t_ctl >= 1.0) { t_ctl = umono(); control_read(); pthread_mutex_lock(&T.m); latency_read(); pthread_mutex_unlock(&T.m); }
        pthread_mutex_lock(&T.m);
        if (T.state == ST_PLAYING || T.state == ST_PAUSED) {
            uq_at a;
            uq_where(&a);
            if (T.next.set && a.seq == T.next.seq && ((double)(a.frame - a.first) >= lat_s * a.rate || a.drained)) {   /* the next track heard:
                                                                                   * it is the current one now */
                ulog("UPnP: now track %d (\"%s\")", T.next.seq, T.next.title);
                track_free(&T.cur);
                T.cur = T.next;
                memset(&T.next, 0, sizeof T.next);
                np_tell(&T.cur);
                av_changed();
            } else if (a.drained && T.state == ST_PLAYING && !uq_reading()) {
                if (T.next.set && T.next.seq > T.queued && T.next.uri[0]) {   /* not gapless (too late, or another format): played next */
                    track_free(&T.cur);
                    T.cur = T.next;
                    memset(&T.next, 0, sizeof T.next);
                    ulog("UPnP: next track (%d)", T.cur.seq);
                    play_from(0);
                } else {
                    ulog("UPnP: the music's end");
                    stop_now();
                }
                av_changed();
            }
        }
        pthread_mutex_unlock(&T.m);
    }
    return NULL;
}

/* ── Protocol info ────────────────────────────────────────────────────────────────────────────────────────────── */
static char sink_info[8192];
static void make_sink(void) {
    static const char *const M[] = { "audio/flac", "audio/x-flac", "audio/wav", "audio/wave", "audio/x-wav", "audio/vnd.wave",
                                     "audio/aiff", "audio/x-aiff", "audio/L16", "audio/L24" };
    size_t w = 0;
    for (size_t i = 0; i < sizeof M / sizeof *M; i++)
        w += (size_t)snprintf(sink_info + w, sizeof sink_info - w, "%shttp-get:*:%s:*", w ? "," : "", M[i]);
    static const unsigned R[] = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 };
    for (int b = 16; b <= 24; b += 8)
        for (size_t i = 0; i < sizeof R / sizeof *R; i++)
            for (int c = 1; c <= 2; c++)
                w += (size_t)snprintf(sink_info + w, sizeof sink_info - w, ",http-get:*:audio/L%d;rate=%u;channels=%d:%s", b, R[i], c,
                                      b == 16 && R[i] == 44100 && c == 2 ? "DLNA.ORG_PN=LPCM" : "*");
    w += (size_t)snprintf(sink_info + w, sizeof sink_info - w, ",http-get:*:audio/wav:DLNA.ORG_PN=WAV,http-get:*:audio/x-flac:DLNA.ORG_PN=FLAC");
}

/* ── Descriptions ─────────────────────────────────────────────────────────────────────────────────────────────── */
static const char *const SVC[3] = { "AVTransport", "RenderingControl", "ConnectionManager" };
#define ARG(n, d, v) "<argument><name>" n "</name><direction>" d "</direction><relatedStateVariable>" v "</relatedStateVariable></argument>"
#define IID ARG("InstanceID", "in", "A_ARG_TYPE_InstanceID")
#define ACT(n, args) "<action><name>" n "</name><argumentList>" args "</argumentList></action>"
#define ACT0(n) "<action><name>" n "</name></action>"
#define SV(n, t) "<stateVariable sendEvents=\"no\"><name>" n "</name><dataType>" t "</dataType></stateVariable>"
#define SVE(n, t) "<stateVariable sendEvents=\"yes\"><name>" n "</name><dataType>" t "</dataType></stateVariable>"
#define SVL(n, t, l) "<stateVariable sendEvents=\"no\"><name>" n "</name><dataType>" t "</dataType><allowedValueList>" l "</allowedValueList></stateVariable>"
#define AV(v) "<allowedValue>" v "</allowedValue>"
#define SCPD_HEAD "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion><actionList>"
static const char SCPD_AVT[] = SCPD_HEAD
    ACT("SetAVTransportURI", IID ARG("CurrentURI", "in", "AVTransportURI") ARG("CurrentURIMetaData", "in", "AVTransportURIMetaData"))
    ACT("SetNextAVTransportURI", IID ARG("NextURI", "in", "NextAVTransportURI") ARG("NextURIMetaData", "in", "NextAVTransportURIMetaData"))
    ACT("GetMediaInfo", IID ARG("NrTracks", "out", "NumberOfTracks") ARG("MediaDuration", "out", "CurrentMediaDuration") ARG("CurrentURI", "out", "AVTransportURI")
        ARG("CurrentURIMetaData", "out", "AVTransportURIMetaData") ARG("NextURI", "out", "NextAVTransportURI") ARG("NextURIMetaData", "out", "NextAVTransportURIMetaData")
        ARG("PlayMedium", "out", "PlaybackStorageMedium") ARG("RecordMedium", "out", "RecordStorageMedium") ARG("WriteStatus", "out", "RecordMediumWriteStatus"))
    ACT("GetTransportInfo", IID ARG("CurrentTransportState", "out", "TransportState") ARG("CurrentTransportStatus", "out", "TransportStatus") ARG("CurrentSpeed", "out", "TransportPlaySpeed"))
    ACT("GetPositionInfo", IID ARG("Track", "out", "CurrentTrack") ARG("TrackDuration", "out", "CurrentTrackDuration") ARG("TrackMetaData", "out", "CurrentTrackMetaData")
        ARG("TrackURI", "out", "CurrentTrackURI") ARG("RelTime", "out", "RelativeTimePosition") ARG("AbsTime", "out", "AbsoluteTimePosition")
        ARG("RelCount", "out", "RelativeCounterPosition") ARG("AbsCount", "out", "AbsoluteCounterPosition"))
    ACT("GetDeviceCapabilities", IID ARG("PlayMedia", "out", "PossiblePlaybackStorageMedia") ARG("RecMedia", "out", "PossibleRecordStorageMedia") ARG("RecQualityModes", "out", "PossibleRecordQualityModes"))
    ACT("GetTransportSettings", IID ARG("PlayMode", "out", "CurrentPlayMode") ARG("RecQualityMode", "out", "CurrentRecordQualityMode"))
    ACT("Stop", IID) ACT("Play", IID ARG("Speed", "in", "TransportPlaySpeed")) ACT("Pause", IID)
    ACT("Seek", IID ARG("Unit", "in", "A_ARG_TYPE_SeekMode") ARG("Target", "in", "A_ARG_TYPE_SeekTarget"))
    ACT("Next", IID) ACT("Previous", IID)
    ACT("SetPlayMode", IID ARG("NewPlayMode", "in", "CurrentPlayMode"))
    ACT("GetCurrentTransportActions", IID ARG("Actions", "out", "CurrentTransportActions"))
    "</actionList><serviceStateTable>"
    SVL("TransportState", "string", AV("STOPPED") AV("PLAYING") AV("PAUSED_PLAYBACK") AV("TRANSITIONING") AV("NO_MEDIA_PRESENT"))
    SVL("TransportStatus", "string", AV("OK") AV("ERROR_OCCURRED"))
    SVL("PlaybackStorageMedium", "string", AV("NONE") AV("NETWORK")) SVL("RecordStorageMedium", "string", AV("NOT_IMPLEMENTED"))
    SV("PossiblePlaybackStorageMedia", "string") SV("PossibleRecordStorageMedia", "string")
    SVL("CurrentPlayMode", "string", AV("NORMAL")) SVL("TransportPlaySpeed", "string", AV("1"))
    SVL("RecordMediumWriteStatus", "string", AV("NOT_IMPLEMENTED")) SV("PossibleRecordQualityModes", "string") SVL("CurrentRecordQualityMode", "string", AV("NOT_IMPLEMENTED"))
    SV("NumberOfTracks", "ui4") SV("CurrentTrack", "ui4") SV("CurrentTrackDuration", "string") SV("CurrentMediaDuration", "string")
    SV("CurrentTrackMetaData", "string") SV("CurrentTrackURI", "string") SV("AVTransportURI", "string") SV("AVTransportURIMetaData", "string")
    SV("NextAVTransportURI", "string") SV("NextAVTransportURIMetaData", "string") SV("RelativeTimePosition", "string") SV("AbsoluteTimePosition", "string")
    SV("RelativeCounterPosition", "i4") SV("AbsoluteCounterPosition", "i4") SV("CurrentTransportActions", "string")
    SVE("LastChange", "string")
    SVL("A_ARG_TYPE_SeekMode", "string", AV("REL_TIME") AV("ABS_TIME") AV("TRACK_NR")) SV("A_ARG_TYPE_SeekTarget", "string") SV("A_ARG_TYPE_InstanceID", "ui4")
    "</serviceStateTable></scpd>";
#define CH ARG("Channel", "in", "A_ARG_TYPE_Channel")
static const char SCPD_RCS[] = SCPD_HEAD
    ACT("ListPresets", IID ARG("CurrentPresetNameList", "out", "PresetNameList"))
    ACT("SelectPreset", IID ARG("PresetName", "in", "A_ARG_TYPE_PresetName"))
    ACT("GetMute", IID CH ARG("CurrentMute", "out", "Mute")) ACT("SetMute", IID CH ARG("DesiredMute", "in", "Mute"))
    ACT("GetVolume", IID CH ARG("CurrentVolume", "out", "Volume")) ACT("SetVolume", IID CH ARG("DesiredVolume", "in", "Volume"))
    ACT("GetVolumeDB", IID CH ARG("CurrentVolume", "out", "VolumeDB")) ACT("SetVolumeDB", IID CH ARG("DesiredVolume", "in", "VolumeDB"))
    ACT("GetVolumeDBRange", IID CH ARG("MinValue", "out", "VolumeDB") ARG("MaxValue", "out", "VolumeDB"))
    "</actionList><serviceStateTable>"
    SV("PresetNameList", "string") SVE("LastChange", "string") SV("Mute", "boolean")
    "<stateVariable sendEvents=\"no\"><name>Volume</name><dataType>ui2</dataType><allowedValueRange><minimum>0</minimum><maximum>100</maximum><step>1</step></allowedValueRange></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>VolumeDB</name><dataType>i2</dataType><allowedValueRange><minimum>-20480</minimum><maximum>0</maximum></allowedValueRange></stateVariable>"
    SVL("A_ARG_TYPE_Channel", "string", AV("Master")) SV("A_ARG_TYPE_InstanceID", "ui4") SVL("A_ARG_TYPE_PresetName", "string", AV("FactoryDefaults"))
    "</serviceStateTable></scpd>";
static const char SCPD_CM[] = SCPD_HEAD
    ACT("GetProtocolInfo", ARG("Source", "out", "SourceProtocolInfo") ARG("Sink", "out", "SinkProtocolInfo"))
    ACT("GetCurrentConnectionIDs", ARG("ConnectionIDs", "out", "CurrentConnectionIDs"))
    ACT("GetCurrentConnectionInfo", ARG("ConnectionID", "in", "A_ARG_TYPE_ConnectionID") ARG("RcsID", "out", "A_ARG_TYPE_RcsID") ARG("AVTransportID", "out", "A_ARG_TYPE_AVTransportID")
        ARG("ProtocolInfo", "out", "A_ARG_TYPE_ProtocolInfo") ARG("PeerConnectionManager", "out", "A_ARG_TYPE_ConnectionManager")
        ARG("PeerConnectionID", "out", "A_ARG_TYPE_ConnectionID") ARG("Direction", "out", "A_ARG_TYPE_Direction") ARG("Status", "out", "A_ARG_TYPE_ConnectionStatus"))
    "</actionList><serviceStateTable>"
    SVE("SourceProtocolInfo", "string") SVE("SinkProtocolInfo", "string") SVE("CurrentConnectionIDs", "string")
    SVL("A_ARG_TYPE_ConnectionStatus", "string", AV("OK") AV("ContentFormatMismatch") AV("InsufficientBandwidth") AV("UnreliableChannel") AV("Unknown"))
    SV("A_ARG_TYPE_ConnectionManager", "string") SVL("A_ARG_TYPE_Direction", "string", AV("Input") AV("Output")) SV("A_ARG_TYPE_ProtocolInfo", "string")
    SV("A_ARG_TYPE_ConnectionID", "i4") SV("A_ARG_TYPE_AVTransportID", "i4") SV("A_ARG_TYPE_RcsID", "i4")
    "</serviceStateTable></scpd>";
static void description(char *out, size_t len) {
    char nm[512];
    xesc(fname(), nm, sizeof nm);
    int n = snprintf(out, len, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">"
                     "<specVersion><major>1</major><minor>0</minor></specVersion><device>"
                     "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType><friendlyName>%s</friendlyName>"
                     "<manufacturer>audiolaz</manufacturer><manufacturerURL>https://audiolaz.com</manufacturerURL>"
                     "<modelDescription>audiolaz player: room correction</modelDescription><modelName>audiolaz player</modelName>"
                     "<modelNumber>%s</modelNumber><modelURL>https://audiolaz.com</modelURL><serialNumber>%.8s</serialNumber><UDN>%s</UDN>"
                     "<dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC><serviceList>", nm, ROOMD_VERSION, udn + 5, udn);
    for (int i = 0; i < 3; i++)
        n += snprintf(out + n, len - (size_t)n, "<service><serviceType>urn:schemas-upnp-org:service:%s:1</serviceType><serviceId>urn:upnp-org:serviceId:%s</serviceId>"
                      "<SCPDURL>/%s.xml</SCPDURL><controlURL>/ctl/%s</controlURL><eventSubURL>/evt/%s</eventSubURL></service>", SVC[i], SVC[i], SVC[i], SVC[i], SVC[i]);
    snprintf(out + n, len - (size_t)n, "</serviceList></device></root>\n");
}

/* ── Events (GENA) ────────────────────────────────────────────────────────────────────────────────────────────── */
typedef struct { int used, svc; char sid[48], cb[512]; unsigned seq; double until; int due; } sub_t;
static sub_t SUBS[NSUB];
static pthread_mutex_t sm = PTHREAD_MUTEX_INITIALIZER;
static const char *actions(void) {   /* under T.m */
    switch (T.state) {
    case ST_PLAYING: return T.next.set ? "Pause,Stop,Seek,Next,Previous" : "Pause,Stop,Seek,Previous";
    case ST_PAUSED: return "Play,Stop,Seek,Previous";
    case ST_STOPPED: return "Play,Seek";
    default: return "";
    }
}
/* A text value for an attribute (val="…") of LastChange: escaped once (the whole escaped again as the property's text) */
static size_t put(char *b, size_t n, size_t len, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t put(char *b, size_t n, size_t len, const char *fmt, ...) {
    if (n >= len) return n;
    va_list a; va_start(a, fmt);
    const int k = vsnprintf(b + n, len - n, fmt, a);
    va_end(a);
    return k < 0 ? n : n + (size_t)k >= len ? len - 1 : n + (size_t)k;
}
static char *avt_lastchange(void) {   /* under T.m; malloc'd, not escaped yet */
    char *cm = xesc_dup(T.cur.meta ? T.cur.meta : ""), *nm = xesc_dup(T.next.meta ? T.next.meta : "");
    char *cu = xesc_dup(T.cur.uri), *nu = xesc_dup(T.next.set ? T.next.uri : "");
    const size_t len = 4096 + (cm ? strlen(cm) * 2 : 0) + (nm ? strlen(nm) : 0) + (cu ? strlen(cu) * 3 : 0) + (nu ? strlen(nu) : 0);
    char *b = malloc(len);
    if (b) {
        char d[32];
        s_hms(T.cur.dur, d, sizeof d);
        size_t n = 0;
        n = put(b, n, len, "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\"><InstanceID val=\"0\">");
        n = put(b, n, len, "<TransportState val=\"%s\"/><TransportStatus val=\"%s\"/><PlaybackStorageMedium val=\"%s\"/>", ST_NAME[T.state], T.status, T.cur.set ? "NETWORK" : "NONE");
        n = put(b, n, len, "<PossiblePlaybackStorageMedia val=\"NETWORK\"/><PossibleRecordStorageMedia val=\"NOT_IMPLEMENTED\"/><CurrentPlayMode val=\"NORMAL\"/>");
        n = put(b, n, len, "<TransportPlaySpeed val=\"1\"/><RecordStorageMedium val=\"NOT_IMPLEMENTED\"/><RecordMediumWriteStatus val=\"NOT_IMPLEMENTED\"/>");
        n = put(b, n, len, "<NumberOfTracks val=\"%d\"/><CurrentTrack val=\"%d\"/><CurrentTrackDuration val=\"%s\"/><CurrentMediaDuration val=\"%s\"/>", T.cur.set, T.cur.set, d, d);
        n = put(b, n, len, "<CurrentTrackURI val=\"%s\"/><AVTransportURI val=\"%s\"/><CurrentTrackMetaData val=\"%s\"/><AVTransportURIMetaData val=\"%s\"/>", cu ? cu : "", cu ? cu : "", cm ? cm : "", cm ? cm : "");
        n = put(b, n, len, "<NextAVTransportURI val=\"%s\"/><NextAVTransportURIMetaData val=\"%s\"/><CurrentTransportActions val=\"%s\"/>", nu ? nu : "", nm ? nm : "", actions());
        put(b, n, len, "</InstanceID></Event>");
    }
    free(cm); free(nm); free(cu); free(nu);
    return b;
}
static char *rcs_lastchange(void) {   /* under T.m */
    char *b = malloc(1024);
    if (b) snprintf(b, 1024, "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\"><InstanceID val=\"0\"><Volume channel=\"Master\" val=\"%d\"/>"
                    "<Mute channel=\"Master\" val=\"%d\"/><VolumeDB channel=\"Master\" val=\"%d\"/><PresetNameList val=\"FactoryDefaults\"/></InstanceID></Event>",
                    T.vol, T.mute, T.vol <= 0 ? -20480 : (int)lround(256 * 40 * log10(T.vol / 100.0)));
    return b;
}
static char *event_body(int svc) {
    char *inner = NULL;
    if (svc == 2) {
        const size_t len = strlen(sink_info) + 512;
        char *b = malloc(len);
        if (b) snprintf(b, len, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\"><e:property><SourceProtocolInfo></SourceProtocolInfo></e:property>"
                        "<e:property><SinkProtocolInfo>%s</SinkProtocolInfo></e:property><e:property><CurrentConnectionIDs>0</CurrentConnectionIDs></e:property></e:propertyset>", sink_info);
        return b;
    }
    pthread_mutex_lock(&T.m);
    inner = svc == 0 ? avt_lastchange() : rcs_lastchange();
    pthread_mutex_unlock(&T.m);
    if (!inner) return NULL;
    char *esc = xesc_dup(inner);
    free(inner);
    if (!esc) return NULL;
    const size_t len = strlen(esc) + 512;
    char *b = malloc(len);
    if (b) snprintf(b, len, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\"><e:property><LastChange>%s</LastChange></e:property></e:propertyset>", esc);
    free(esc);
    return b;
}
static int send_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
#ifdef MSG_NOSIGNAL
        const ssize_t k = send(fd, c, n, MSG_NOSIGNAL);
#else
        const ssize_t k = send(fd, c, n, 0);
#endif
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        c += k; n -= (size_t)k;
    }
    return 0;
}
static int notify_one(const char *cb, const char *sid, unsigned seq, const char *body) {
    if (strncasecmp(cb, "http://", 7)) return -1;
    char host[256] = "", port[16] = "80";
    const char *h = cb + 7, *e = h;
    if (*h == '[') { e = strchr(h, ']'); if (!e) return -1; snprintf(host, sizeof host, "%.*s", (int)(e - h - 1), h + 1); e++; }
    else { while (*e && *e != ':' && *e != '/') e++; snprintf(host, sizeof host, "%.*s", (int)(e - h), h); }
    if (*e == ':') { const char *p = ++e; while (*e >= '0' && *e <= '9') e++; snprintf(port, sizeof port, "%.*s", (int)(e - p), p); }
    const char *path = *e == '/' ? e : "/";
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) || !res) return -1;
    int s = socket(res->ai_family, SOCK_STREAM, 0);
    if (s < 0) { freeaddrinfo(res); return -1; }
#ifdef SO_NOSIGPIPE
    { int one = 1; setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
    fcntl(s, F_SETFL, O_NONBLOCK);
    int ok = !connect(s, res->ai_addr, res->ai_addrlen) || errno == EINPROGRESS;
    freeaddrinfo(res);
    if (ok) { struct pollfd p = { s, POLLOUT, 0 }; int err = 0; socklen_t el = sizeof err;
              ok = poll(&p, 1, 1500) == 1 && !getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) && !err; }
    if (!ok) { close(s); return -1; }
    fcntl(s, F_SETFL, 0);
    struct timeval tv = { 2, 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char hd[1024];
    const int hl = snprintf(hd, sizeof hd, "NOTIFY %s HTTP/1.1\r\nHOST: %s:%s\r\nCONTENT-TYPE: text/xml; charset=\"utf-8\"\r\nNT: upnp:event\r\nNTS: upnp:propchange\r\n"
                            "SID: %s\r\nSEQ: %u\r\nCONTENT-LENGTH: %zu\r\nConnection: close\r\n\r\n", path, host, port, sid, seq, strlen(body));
    ok = !send_all(s, hd, (size_t)hl) && !send_all(s, body, strlen(body));
    if (ok) { char r[256]; if (recv(s, r, sizeof r, 0) <= 0) { } }
    close(s);
    return ok ? 0 : -1;
}
static void *events_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&T.m);
        while (!T.av_ev && !T.rc_ev) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
            pthread_cond_timedwait(&ev_cv, &T.m, &ts);
            break;
        }
        const int av = T.av_ev, rc = T.rc_ev;
        T.av_ev = T.rc_ev = 0;
        pthread_mutex_unlock(&T.m);
        pthread_mutex_lock(&sm);
        for (int i = 0; i < NSUB; i++) if (SUBS[i].used && ((av && SUBS[i].svc == 0) || (rc && SUBS[i].svc == 1))) SUBS[i].due = 1;
        pthread_mutex_unlock(&sm);
        for (int i = 0; i < NSUB; i++) {
            pthread_mutex_lock(&sm);
            sub_t s = SUBS[i];
            const int go = s.used && s.due && umono() < s.until;
            if (s.used && umono() >= s.until) { SUBS[i].used = 0; }
            if (go) { SUBS[i].due = 0; SUBS[i].seq = s.seq + 1 == 0 ? 1 : s.seq + 1; }   /* sent with s.seq: 0 first */
            pthread_mutex_unlock(&sm);
            if (!go) continue;
            char *body = event_body(s.svc);
            if (body && notify_one(s.cb, s.sid, s.seq, body)) {
                static double said = 0;
                if (umono() - said > 60) { said = umono(); ulog("UPnP: event to %s not delivered", s.cb); }
            }
            free(body);
        }
        usleep(150000);   /* events moderated: at most ~6 a second */
    }
    return NULL;
}

/* ── SOAP ─────────────────────────────────────────────────────────────────────────────────────────────────────── */
typedef struct { char *b; size_t n, cap; } sbuf;
static void sb_add(sbuf *s, const char *t) {
    const size_t l = strlen(t);
    if (s->n + l + 1 > s->cap) { size_t c = s->cap ? s->cap : 4096; while (c < s->n + l + 1) c *= 2; char *nb = realloc(s->b, c); if (!nb) return; s->b = nb; s->cap = c; }
    memcpy(s->b + s->n, t, l + 1); s->n += l;
}
static void sb_arg(sbuf *s, const char *name, const char *val) {   /* <name>escaped val</name> */
    sb_add(s, "<"); sb_add(s, name); sb_add(s, ">");
    char *e = xesc_dup(val ? val : "");
    if (e) sb_add(s, e);
    free(e);
    sb_add(s, "</"); sb_add(s, name); sb_add(s, ">");
}
static void sb_argi(sbuf *s, const char *name, long v) { char b[32]; snprintf(b, sizeof b, "%ld", v); sb_arg(s, name, b); }
static void http_send(int c, int code, const char *ctype, const char *body, size_t bl, const char *extra) {
    char h[1024];
    const char *txt = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" : code == 412 ? "Precondition Failed" : code == 500 ? "Internal Server Error" : "Error";
    const int hl = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\n%s%s%sContent-Length: %zu\r\nServer: %s/1.0 UPnP/1.0 audiolaz-player/%s\r\nConnection: close\r\n%s\r\n",
                            code, txt, ctype ? "Content-Type: " : "", ctype ? ctype : "", ctype ? "\r\n" : "", bl,
#ifdef __APPLE__
                            "macOS",
#else
                            "Linux",
#endif
                            ROOMD_VERSION, extra ? extra : "");
    send_all(c, h, (size_t)hl);
    if (bl) send_all(c, body, bl);
}
static void soap_reply(int c, int svc, const char *action, const char *args) {
    sbuf s = { 0 };
    sb_add(&s, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:");
    sb_add(&s, action); sb_add(&s, "Response xmlns:u=\"urn:schemas-upnp-org:service:"); sb_add(&s, SVC[svc]); sb_add(&s, ":1\">");
    sb_add(&s, args ? args : "");
    sb_add(&s, "</u:"); sb_add(&s, action); sb_add(&s, "Response></s:Body></s:Envelope>");
    http_send(c, 200, "text/xml; charset=\"utf-8\"", s.b ? s.b : "", s.n, "EXT:\r\n");
    free(s.b);
}
static void soap_error(int c, int code, const char *desc) {
    char b[1024];
    const int n = snprintf(b, sizeof b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                           "<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
                           "<errorCode>%d</errorCode><errorDescription>%s</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>", code, desc);
    http_send(c, 500, "text/xml; charset=\"utf-8\"", b, (size_t)n, "EXT:\r\n");
}
static void arg_to(const char *body, const char *name, char *out, size_t len) {
    char *v = xel(body, name, NULL);
    snprintf(out, len, "%s", v ? v : "");
    free(v);
}
static void avt(int c, const char *a, const char *body) {
    sbuf s = { 0 };
    if (!strcmp(a, "SetAVTransportURI") || !strcmp(a, "SetNextAVTransportURI")) {
        const int nx = a[3] == 'N';
        char *uri = xel(body, nx ? "NextURI" : "CurrentURI", NULL), *meta = xel(body, nx ? "NextURIMetaData" : "CurrentURIMetaData", NULL);
        char *u = uri; if (u) { while (isspace((unsigned char)*u)) u++; size_t l = strlen(u); while (l && isspace((unsigned char)u[l - 1])) u[--l] = 0; }
        if (u && *u && strncasecmp(u, "http://", 7)) { free(uri); free(meta); soap_error(c, 714, "Illegal MIME-type"); return; }
        pthread_mutex_lock(&T.m);
        if (nx) {
            if (u && *u) { track_set(&T.next, u, meta); T.next.seq = ++T.seq; ulog("UPnP: next track (%d): \"%s\" %s", T.next.seq, T.next.title, T.next.uri); }
            else track_free(&T.next);
            pthread_cond_broadcast(&T.cv);
        } else {
            const int was = T.state;
            track_free(&T.next);
            if (u && *u) {
                track_set(&T.cur, u, meta); T.cur.seq = ++T.seq;
                snprintf(T.status, sizeof T.status, "OK");
                ulog("UPnP: track (%d): \"%s\" — %s — %s, %s", T.cur.seq, T.cur.title, T.cur.artist, T.cur.album, T.cur.uri);
                if (was == ST_PLAYING) play_from(0);   /* playing: the new one at once */
                else { stop_now(); T.state = ST_STOPPED; }
            } else { stop_now(); track_free(&T.cur); T.state = ST_NOMEDIA; }
        }
        av_changed();
        pthread_mutex_unlock(&T.m);
        free(uri); free(meta);
    } else if (!strcmp(a, "Play")) {
        pthread_mutex_lock(&T.m);
        if (!atomic_load(&en_live) || !T.cur.set) { pthread_mutex_unlock(&T.m); soap_error(c, 701, "Transition not available"); return; }
        if (T.state == ST_PAUSED) { uq_pause(0); uq_started(); T.state = ST_PLAYING; }
        else if (T.state != ST_PLAYING) { const double h = T.held_pos; T.state = ST_PLAYING; snprintf(T.status, sizeof T.status, "OK"); play_from(h); }
        else uq_started();
        ulog("UPnP: Play");
        av_changed();
        pthread_mutex_unlock(&T.m);
    } else if (!strcmp(a, "Pause")) {
        pthread_mutex_lock(&T.m);
        if (T.state == ST_PLAYING) { uq_pause(1); T.state = ST_PAUSED; T.held_pos = pos_now(); ulog("UPnP: Pause"); av_changed(); }
        pthread_mutex_unlock(&T.m);
    } else if (!strcmp(a, "Stop")) {
        pthread_mutex_lock(&T.m);
        if (T.state != ST_STOPPED && T.state != ST_NOMEDIA) ulog("UPnP: Stop");
        stop_now();
        av_changed();
        pthread_mutex_unlock(&T.m);
    } else if (!strcmp(a, "Seek")) {
        char unit[32], target[64];
        arg_to(body, "Unit", unit, sizeof unit); arg_to(body, "Target", target, sizeof target);
        double t = -1;
        if (!strcmp(unit, "REL_TIME") || !strcmp(unit, "ABS_TIME")) t = hms_s(target);
        else if (!strcmp(unit, "TRACK_NR")) t = 0;
        if (!(t >= 0)) { soap_error(c, 710, "Seek mode not supported"); return; }
        pthread_mutex_lock(&T.m);
        if (!T.cur.set) { pthread_mutex_unlock(&T.m); soap_error(c, 701, "Transition not available"); return; }
        ulog("UPnP: Seek %s", target);
        if (T.state == ST_PLAYING) play_from(t);
        else if (T.state == ST_PAUSED) { play_from(t); uq_pause(1); }
        else T.held_pos = t;
        av_changed();
        pthread_mutex_unlock(&T.m);
    } else if (!strcmp(a, "Next")) {
        pthread_mutex_lock(&T.m);
        if (T.next.set) {
            track_free(&T.cur); T.cur = T.next; memset(&T.next, 0, sizeof T.next);
            if (T.state == ST_PLAYING || T.state == ST_PAUSED) { T.state = ST_PLAYING; play_from(0); } else T.held_pos = 0;
            av_changed();
        }
        pthread_mutex_unlock(&T.m);
    } else if (!strcmp(a, "Previous")) {
        pthread_mutex_lock(&T.m);
        if (T.state == ST_PLAYING) play_from(0); else T.held_pos = 0;
        av_changed();
        pthread_mutex_unlock(&T.m);
    } else if (!strcmp(a, "GetTransportInfo")) {
        pthread_mutex_lock(&T.m);
        sb_arg(&s, "CurrentTransportState", ST_NAME[T.state]); sb_arg(&s, "CurrentTransportStatus", T.status); sb_arg(&s, "CurrentSpeed", "1");
        pthread_mutex_unlock(&T.m);
    } else if (!strcmp(a, "GetPositionInfo")) {
        char d[32], p[32];
        pthread_mutex_lock(&T.m);
        if (!(T.cur.dur > 0)) { const double dd = ud_duration(T.cur.seq); if (dd > 0) T.cur.dur = dd; }
        s_hms(T.cur.dur, d, sizeof d); s_hms(pos_now(), p, sizeof p);
        sb_argi(&s, "Track", T.cur.set); sb_arg(&s, "TrackDuration", d); sb_arg(&s, "TrackMetaData", T.cur.meta ? T.cur.meta : "");
        sb_arg(&s, "TrackURI", T.cur.uri); sb_arg(&s, "RelTime", p); sb_arg(&s, "AbsTime", p);
        sb_argi(&s, "RelCount", 2147483647); sb_argi(&s, "AbsCount", 2147483647);
        pthread_mutex_unlock(&T.m);
    } else if (!strcmp(a, "GetMediaInfo")) {
        char d[32];
        pthread_mutex_lock(&T.m);
        s_hms(T.cur.dur, d, sizeof d);
        sb_argi(&s, "NrTracks", T.cur.set); sb_arg(&s, "MediaDuration", d); sb_arg(&s, "CurrentURI", T.cur.uri);
        sb_arg(&s, "CurrentURIMetaData", T.cur.meta ? T.cur.meta : ""); sb_arg(&s, "NextURI", T.next.set ? T.next.uri : "");
        sb_arg(&s, "NextURIMetaData", T.next.meta ? T.next.meta : ""); sb_arg(&s, "PlayMedium", T.cur.set ? "NETWORK" : "NONE");
        sb_arg(&s, "RecordMedium", "NOT_IMPLEMENTED"); sb_arg(&s, "WriteStatus", "NOT_IMPLEMENTED");
        pthread_mutex_unlock(&T.m);
    } else if (!strcmp(a, "GetDeviceCapabilities")) {
        sb_arg(&s, "PlayMedia", "NETWORK"); sb_arg(&s, "RecMedia", "NOT_IMPLEMENTED"); sb_arg(&s, "RecQualityModes", "NOT_IMPLEMENTED");
    } else if (!strcmp(a, "GetTransportSettings")) {
        sb_arg(&s, "PlayMode", "NORMAL"); sb_arg(&s, "RecQualityMode", "NOT_IMPLEMENTED");
    } else if (!strcmp(a, "GetCurrentTransportActions")) {
        pthread_mutex_lock(&T.m);
        sb_arg(&s, "Actions", actions());
        pthread_mutex_unlock(&T.m);
    } else if (!strcmp(a, "SetPlayMode")) {
        char m[32];
        arg_to(body, "NewPlayMode", m, sizeof m);
        if (strcmp(m, "NORMAL")) { soap_error(c, 712, "Play mode not supported"); return; }
    } else { soap_error(c, 401, "Invalid Action"); return; }
    soap_reply(c, 0, a, s.b);
    free(s.b);
}
static void rcs(int c, const char *a, const char *body) {
    sbuf s = { 0 };
    char v[64];
    if (!strcmp(a, "GetVolume")) { pthread_mutex_lock(&T.m); sb_argi(&s, "CurrentVolume", T.vol); pthread_mutex_unlock(&T.m); }
    else if (!strcmp(a, "GetMute")) { pthread_mutex_lock(&T.m); sb_argi(&s, "CurrentMute", T.mute); pthread_mutex_unlock(&T.m); }
    else if (!strcmp(a, "SetVolume")) {
        arg_to(body, "DesiredVolume", v, sizeof v);
        int x = atoi(v);
        if (!v[0] || x < 0 || x > 100) { soap_error(c, 402, "Invalid Args"); return; }
        pthread_mutex_lock(&T.m); T.vol = x; T.rc_ev = 1; pthread_cond_broadcast(&ev_cv); pthread_mutex_unlock(&T.m);
    } else if (!strcmp(a, "SetMute")) {
        arg_to(body, "DesiredMute", v, sizeof v);
        const int m = !strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes");
        pthread_mutex_lock(&T.m); if (T.mute != m) ulog("UPnP: %s", m ? "muted" : "unmuted"); T.mute = m; T.rc_ev = 1; pthread_cond_broadcast(&ev_cv); pthread_mutex_unlock(&T.m);
    } else if (!strcmp(a, "GetVolumeDB")) {
        pthread_mutex_lock(&T.m); sb_argi(&s, "CurrentVolume", T.vol <= 0 ? -20480 : lround(256 * 40 * log10(T.vol / 100.0))); pthread_mutex_unlock(&T.m);
    } else if (!strcmp(a, "SetVolumeDB")) {
        arg_to(body, "DesiredVolume", v, sizeof v);
        const double db = atof(v) / 256.0;
        int x = db <= -80 ? 0 : (int)lround(100 * pow(10, db / 40));
        if (x > 100) x = 100;
        if (x < 0) x = 0;
        pthread_mutex_lock(&T.m); T.vol = x; T.rc_ev = 1; pthread_cond_broadcast(&ev_cv); pthread_mutex_unlock(&T.m);
    } else if (!strcmp(a, "GetVolumeDBRange")) { sb_argi(&s, "MinValue", -20480); sb_argi(&s, "MaxValue", 0); }
    else if (!strcmp(a, "ListPresets")) sb_arg(&s, "CurrentPresetNameList", "FactoryDefaults");
    else if (!strcmp(a, "SelectPreset")) { }
    else { soap_error(c, 401, "Invalid Action"); return; }
    soap_reply(c, 1, a, s.b);
    free(s.b);
}
static void cms(int c, const char *a, const char *body) {
    sbuf s = { 0 };
    if (!strcmp(a, "GetProtocolInfo")) { sb_arg(&s, "Source", ""); sb_arg(&s, "Sink", sink_info); }
    else if (!strcmp(a, "GetCurrentConnectionIDs")) sb_arg(&s, "ConnectionIDs", "0");
    else if (!strcmp(a, "GetCurrentConnectionInfo")) {
        char id[16];
        arg_to(body, "ConnectionID", id, sizeof id);
        if (strcmp(id, "0")) { soap_error(c, 706, "Invalid connection reference"); return; }
        sb_arg(&s, "RcsID", "0"); sb_arg(&s, "AVTransportID", "0"); sb_arg(&s, "ProtocolInfo", ""); sb_arg(&s, "PeerConnectionManager", "");
        sb_arg(&s, "PeerConnectionID", "-1"); sb_arg(&s, "Direction", "Input"); sb_arg(&s, "Status", "OK");
    } else { soap_error(c, 401, "Invalid Action"); return; }
    soap_reply(c, 2, a, s.b);
    free(s.b);
}

/* ── The HTTP server ──────────────────────────────────────────────────────────────────────────────────────────── */
static int svc_of(const char *p, const char *prefix) {
    const size_t l = strlen(prefix);
    if (strncmp(p, prefix, l)) return -1;
    for (int i = 0; i < 3; i++) if (!strcmp(p + l, SVC[i])) return i;
    return -1;
}
static void hdr(const char *req, const char *name, char *out, size_t len) {
    out[0] = 0;
    const size_t nl = strlen(name);
    for (const char *l = strstr(req, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        const char *k = l + 2;
        if (!strncasecmp(k, name, nl) && k[nl] == ':') {
            const char *v = k + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            const char *e = strstr(v, "\r\n");
            const size_t vl = e ? (size_t)(e - v) : strlen(v);
            snprintf(out, len, "%.*s", (int)vl, v);
            return;
        }
        if (k[0] == '\r') return;
    }
}
static void *conn_main(void *arg) {
    const int c = (int)(intptr_t)arg;
    struct timeval tv = { 5, 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    { int one = 1; setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
    size_t cap = 16384, n = 0;
    char *b = malloc(cap + 1);
    char *body = NULL;
    while (b) {
        const ssize_t k = recv(c, b + n, cap - n, 0);
        if (k <= 0) break;
        n += (size_t)k; b[n] = 0;
        char *e = strstr(b, "\r\n\r\n");
        if (e) {
            char cl[32];
            hdr(b, "Content-Length", cl, sizeof cl);
            const size_t want = (size_t)(e + 4 - b) + (size_t)atol(cl);
            if (want > 512 * 1024) break;
            if (want > cap) { char *nb = realloc(b, want + 1); if (!nb) break; b = nb; cap = want; }
            while (n < want) { const ssize_t k2 = recv(c, b + n, want - n, 0); if (k2 <= 0) break; n += (size_t)k2; }
            b[n] = 0;
            body = b + (e + 4 - b);
            break;
        }
        if (n == cap) break;
    }
    if (!body) { free(b); close(c); return NULL; }
    char method[16] = "", path[512] = "";
    sscanf(b, "%15s %511s", method, path);
    char *q = strchr(path, '?');
    if (q) *q = 0;
    int s;
    if (!strcmp(method, "GET") || !strcmp(method, "HEAD")) {
        const int head = method[0] == 'H';
        char *x = NULL;
        size_t xl = 0;
        if (!strcmp(path, "/description.xml") || !strcmp(path, "/")) { x = malloc(8192); if (x) { description(x, 8192); xl = strlen(x); } }
        else if (!strcmp(path, "/AVTransport.xml")) { x = strdup(SCPD_AVT); xl = sizeof SCPD_AVT - 1; }
        else if (!strcmp(path, "/RenderingControl.xml")) { x = strdup(SCPD_RCS); xl = sizeof SCPD_RCS - 1; }
        else if (!strcmp(path, "/ConnectionManager.xml")) { x = strdup(SCPD_CM); xl = sizeof SCPD_CM - 1; }
        if (x) { if (head) { http_send(c, 200, "text/xml; charset=\"utf-8\"", "", 0, NULL); } else http_send(c, 200, "text/xml; charset=\"utf-8\"", x, xl, NULL); }
        else http_send(c, 404, NULL, "", 0, NULL);
        free(x);
    } else if (!strcmp(method, "POST") && (s = svc_of(path, "/ctl/")) >= 0) {
        char sa[256];
        hdr(b, "SOAPACTION", sa, sizeof sa);
        const char *h = strchr(sa, '#');
        char action[64] = "";
        if (h) { snprintf(action, sizeof action, "%s", h + 1); action[strcspn(action, "\" ")] = 0; }
        if (!atomic_load(&en_live)) soap_error(c, 501, "Action Failed");
        else if (!action[0]) soap_error(c, 401, "Invalid Action");
        else if (s == 0) avt(c, action, body);
        else if (s == 1) rcs(c, action, body);
        else cms(c, action, body);
    } else if (!strcmp(method, "SUBSCRIBE") && (s = svc_of(path, "/evt/")) >= 0) {
        char cb[600], sid[64], to[64], nt[64];
        hdr(b, "CALLBACK", cb, sizeof cb); hdr(b, "SID", sid, sizeof sid); hdr(b, "TIMEOUT", to, sizeof to); hdr(b, "NT", nt, sizeof nt);
        int secs = SUB_MAX_S;
        if (!strncasecmp(to, "Second-", 7) && strcasecmp(to + 7, "infinite")) { secs = atoi(to + 7); if (secs <= 0 || secs > SUB_MAX_S) secs = SUB_MAX_S; }
        char extra[160];
        if (sid[0]) {                                   /* a renewal */
            int k = -1;
            pthread_mutex_lock(&sm);
            for (int i = 0; i < NSUB; i++) if (SUBS[i].used && !strcmp(SUBS[i].sid, sid)) { k = i; SUBS[i].until = umono() + secs; }
            pthread_mutex_unlock(&sm);
            if (k < 0 || cb[0]) http_send(c, 412, NULL, "", 0, NULL);
            else { snprintf(extra, sizeof extra, "SID: %s\r\nTIMEOUT: Second-%d\r\n", sid, secs); http_send(c, 200, NULL, "", 0, extra); }
        } else {
            char *l = strchr(cb, '<'), *r = l ? strchr(l, '>') : NULL;
            if (!l || !r || strcmp(nt, "upnp:event")) http_send(c, 412, NULL, "", 0, NULL);
            else {
                *r = 0;
                static unsigned nsid = 0;
                char id[48];
                snprintf(id, sizeof id, "uuid:%08x-%04x-4%03x-a%03x-%012lx", (unsigned)rand(), (unsigned)(nsid++ & 0xFFFF), (unsigned)rand() & 0xFFF, (unsigned)rand() & 0xFFF, (unsigned long)time(NULL));
                int k = -1;
                pthread_mutex_lock(&sm);
                for (int i = 0; i < NSUB && k < 0; i++) if (!SUBS[i].used || umono() >= SUBS[i].until) k = i;
                if (k >= 0) { SUBS[k] = (sub_t){ 1, s, "", "", 0, umono() + secs, 1 }; snprintf(SUBS[k].sid, sizeof SUBS[k].sid, "%s", id); snprintf(SUBS[k].cb, sizeof SUBS[k].cb, "%s", l + 1);
                            }   /* the first event: SEQ 0 */
                pthread_mutex_unlock(&sm);
                if (k < 0) http_send(c, 500, NULL, "", 0, NULL);
                else {
                    snprintf(extra, sizeof extra, "SID: %s\r\nTIMEOUT: Second-%d\r\n", id, secs);
                    http_send(c, 200, NULL, "", 0, extra);
                    pthread_mutex_lock(&T.m); if (s == 0) T.av_ev = 1; else T.rc_ev = 1; pthread_cond_broadcast(&ev_cv); pthread_mutex_unlock(&T.m);
                }
            }
        }
    } else if (!strcmp(method, "UNSUBSCRIBE") && svc_of(path, "/evt/") >= 0) {
        char sid[64];
        hdr(b, "SID", sid, sizeof sid);
        int k = -1;
        pthread_mutex_lock(&sm);
        for (int i = 0; i < NSUB; i++) if (SUBS[i].used && !strcmp(SUBS[i].sid, sid)) { SUBS[i].used = 0; k = i; }
        pthread_mutex_unlock(&sm);
        http_send(c, k < 0 ? 412 : 200, NULL, "", 0, NULL);
    } else http_send(c, 404, NULL, "", 0, NULL);
    free(b);
    close(c);
    return NULL;
}
static int http_fd = -1;
static void *http_main(void *arg) {
    (void)arg;
    for (;;) {
        const int c = accept(http_fd, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; usleep(100000); continue; }
        pthread_t th;
        if (pthread_create(&th, NULL, conn_main, (void *)(intptr_t)c)) close(c); else pthread_detach(th);
    }
    return NULL;
}

/* ── SSDP ─────────────────────────────────────────────────────────────────────────────────────────────────────── */
static int ssdp_fd = -1;
static const char *const NTS_[] = { "upnp:rootdevice", NULL /* uuid */, "urn:schemas-upnp-org:device:MediaRenderer:1",
                                    "urn:schemas-upnp-org:service:AVTransport:1", "urn:schemas-upnp-org:service:RenderingControl:1",
                                    "urn:schemas-upnp-org:service:ConnectionManager:1" };
#define NNT 6
static const char *nt_of(int i) { return i == 1 ? udn : NTS_[i]; }
static void usn_of(int i, char *out, size_t len) { if (i == 1) snprintf(out, len, "%s", udn); else snprintf(out, len, "%s::%s", udn, NTS_[i]); }
static const char *server_s(void) {
#ifdef __APPLE__
    return "macOS/1.0 UPnP/1.0 audiolaz-player/" ROOMD_VERSION;
#else
    return "Linux/1.0 UPnP/1.0 audiolaz-player/" ROOMD_VERSION;
#endif
}
typedef struct { struct in_addr a; char name[IFNAMSIZ]; } iface;
static int ifaces(iface *out, int max) {
    struct ifaddrs *ia = NULL;
    if (getifaddrs(&ia)) return 0;
    int n = 0;
    for (struct ifaddrs *i = ia; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_MULTICAST)) continue;
        const struct in_addr a = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
        if (if_only) { if (a.s_addr != if_only) continue; }
        else if (i->ifa_flags & IFF_LOOPBACK) continue;
        int dup = 0;
        for (int k = 0; k < n; k++) if (out[k].a.s_addr == a.s_addr) dup = 1;
        if (dup) continue;
        out[n].a = a; snprintf(out[n].name, sizeof out[n].name, "%s", i->ifa_name); n++;
    }
    freeifaddrs(ia);
    return n;
}
static void if_resolve(void) {
    if (!ifname[0]) return;
    struct in_addr a;
    if (inet_pton(AF_INET, ifname, &a) == 1) { if_only = a.s_addr; return; }
    struct ifaddrs *ia = NULL;
    if (getifaddrs(&ia)) return;
    for (struct ifaddrs *i = ia; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !strcmp(i->ifa_name, ifname)) { if_only = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr; break; }
    freeifaddrs(ia);
    if (!if_only) ulog("UPnP: no interface \"%s\": every interface", ifname);
}
static void join_all(void) {
    iface f[16];
    const int n = ifaces(f, 16);
    for (int i = 0; i < n; i++) {
        struct ip_mreq m;
        inet_pton(AF_INET, "239.255.255.250", &m.imr_multiaddr);
        m.imr_interface = f[i].a;
        setsockopt(ssdp_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);   /* EADDRINUSE when joined already */
    }
}
static void notify_all(int alive) {
    iface f[16];
    const int n = ifaces(f, 16);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    unsigned char ttl = 4, loop = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(1900) };
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    for (int i = 0; i < n; i++) {
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &f[i].a, sizeof f[i].a);
        char ip[32];
        inet_ntop(AF_INET, &f[i].a, ip, sizeof ip);
        for (int k = 0; k < NNT; k++) {
            char m[1024], usn[256];
            usn_of(k, usn, sizeof usn);
            const int l = alive ? snprintf(m, sizeof m, "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=%d\r\nLOCATION: http://%s:%d/description.xml\r\n"
                                           "NT: %s\r\nNTS: ssdp:alive\r\nSERVER: %s\r\nUSN: %s\r\n\r\n", MAX_AGE, ip, http_port, nt_of(k), server_s(), usn)
                                : snprintf(m, sizeof m, "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nNT: %s\r\nNTS: ssdp:byebye\r\nUSN: %s\r\n\r\n", nt_of(k), usn);
            sendto(s, m, (size_t)l, 0, (struct sockaddr *)&to, sizeof to);
        }
    }
    close(s);
}
/* The address of this machine that reaches ip (its route), for the LOCATION */
static int local_for(const struct sockaddr_in *peer, struct in_addr *out) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in p = *peer;
    if (!p.sin_port) p.sin_port = htons(1900);
    struct sockaddr_in me;
    socklen_t ml = sizeof me;
    const int r = connect(s, (struct sockaddr *)&p, sizeof p) || getsockname(s, (struct sockaddr *)&me, &ml) ? -1 : 0;
    close(s);
    if (!r) *out = me.sin_addr;
    return r;
}
static void msearch(const char *b, const struct sockaddr_in *from) {
    char st[256] = "", man[64] = "";
    for (const char *l = b; l && *l; ) {
        const char *e = strstr(l, "\r\n");
        const size_t ll = e ? (size_t)(e - l) : strlen(l);
        if (ll > 3 && !strncasecmp(l, "ST:", 3)) { const char *v = l + 3; while (*v == ' ') v++; snprintf(st, sizeof st, "%.*s", (int)(ll - (size_t)(v - l)), v); }
        else if (ll > 4 && !strncasecmp(l, "MAN:", 4)) { const char *v = l + 4; while (*v == ' ') v++; snprintf(man, sizeof man, "%.*s", (int)(ll - (size_t)(v - l)), v); }
        l = e ? e + 2 : NULL;
    }
    if (!strstr(man, "ssdp:discover")) return;
    size_t sl = strlen(st);
    while (sl && st[sl - 1] == ' ') st[--sl] = 0;
    struct in_addr me;
    if (local_for(from, &me)) return;
    if (if_only && me.s_addr != if_only) return;
    char ip[32];
    inet_ntop(AF_INET, &me, ip, sizeof ip);
    char date[64];
    const time_t now = time(NULL);
    struct tm g;
    gmtime_r(&now, &g);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &g);
    for (int k = 0; k < NNT; k++) {
        const int all = !strcmp(st, "ssdp:all");
        if (!all && strcmp(st, nt_of(k))) continue;
        char m[1024], usn[256];
        usn_of(k, usn, sizeof usn);
        const int l = snprintf(m, sizeof m, "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=%d\r\nDATE: %s\r\nEXT:\r\nLOCATION: http://%s:%d/description.xml\r\n"
                               "SERVER: %s\r\nST: %s\r\nUSN: %s\r\nContent-Length: 0\r\n\r\n", MAX_AGE, date, ip, http_port, server_s(), nt_of(k), usn);
        sendto(ssdp_fd, m, (size_t)l, 0, (const struct sockaddr *)from, sizeof *from);
    }
}
static atomic_int quit_ = 0;
static void *ssdp_main(void *arg) {
    (void)arg;
    double t_ann = 0, t_join = 0;
    int burst = 3, was_on = 0;
    while (!atomic_load(&quit_)) {
        const int on = atomic_load(&en_live);
        if (!on && was_on) { notify_all(0); ulog("UPnP: ssdp:byebye (switched off)"); }
        if (on && !was_on) { burst = 3; t_ann = 0; }
        was_on = on;
        const double t = umono();
        if (t - t_join > 60) { t_join = t; join_all(); }
        if (on && (t - t_ann >= (burst > 0 ? 1.0 : ANNOUNCE_S))) { t_ann = t; if (burst > 0) burst--; notify_all(1); }
        struct pollfd p = { ssdp_fd, POLLIN, 0 };
        if (poll(&p, 1, 500) <= 0) continue;
        char b[2048];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const ssize_t r = recvfrom(ssdp_fd, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) continue;
        b[r] = 0;
        if (on && !strncmp(b, "M-SEARCH * HTTP/1.1", 19)) msearch(b, &from);
    }
    return NULL;
}

/* ── Start, stop, status ──────────────────────────────────────────────────────────────────────────────────────── */
static void make_udn(void) {
    if (uuid_opt[0]) { snprintf(udn, sizeof udn, "%s%s", strncmp(uuid_opt, "uuid:", 5) ? "uuid:" : "", uuid_opt); return; }
    char seed[512] = "";
#ifdef __APPLE__
    {   /* the Mac's hardware UUID */
        uuid_t u; struct timespec w = { 1, 0 };
        if (!gethostuuid(u, &w)) for (int i = 0; i < 16; i++) snprintf(seed + 2 * i, 3, "%02x", u[i]);
    }
#else
    FILE *f = fopen("/etc/machine-id", "r");
    if (f) { if (!fgets(seed, 64, f)) seed[0] = 0; fclose(f); }
#endif
    if (!seed[0]) gethostname(seed, 64);
    char full[1200];
    snprintf(full, sizeof full, "%s|audiolaz upnp|%s|%d", seed, rooms_, http_port);
    uint64_t h1 = 1469598103934665603ULL, h2 = 0x84222325cbf29ce4ULL;
    for (const unsigned char *p = (const unsigned char *)full; *p; p++) { h1 = (h1 ^ *p) * 1099511628211ULL; h2 = (h2 ^ *p) * 0x100000001b3ULL + 0x9E3779B97F4A7C15ULL; }
    snprintf(udn, sizeof udn, "uuid:%08x-%04x-4%03x-%04x-%012llx", (unsigned)(h1 >> 32), (unsigned)(h1 >> 16) & 0xFFFF, (unsigned)h1 & 0xFFF,
             (unsigned)(0x8000 | ((h2 >> 48) & 0x3FFF)), (unsigned long long)(h2 & 0xFFFFFFFFFFFFULL));
}
int upnp_start(const char *rooms) {
    if (started) return 0;
    snprintf(rooms_, sizeof rooms_, "%s", rooms ? rooms : ".");
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    if_resolve();
    make_udn();
    make_sink();
    atomic_store(&vol_digital, vol_digital_cmd);
    control_read();
    netout_set_self_upnp(udn);                          /* never one of the player's own outputs */
    /* its HTTP server */
    http_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(http_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)http_port) };
    a.sin_addr.s_addr = if_only ? if_only : htonl(INADDR_ANY);
    if (http_fd < 0 || bind(http_fd, (struct sockaddr *)&a, sizeof a) || listen(http_fd, 32)) {
        ulog("UPnP input: port %d refused (%s): off", http_port, strerror(errno));
        if (http_fd >= 0) close(http_fd);
        en_cmd = 0;
        return -1;
    }
    /* SSDP: port 1900, shared with the other programs that listen there */
    ssdp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(ssdp_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(ssdp_fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    struct sockaddr_in s = { .sin_family = AF_INET, .sin_port = htons(1900), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (ssdp_fd < 0 || bind(ssdp_fd, (struct sockaddr *)&s, sizeof s)) {
        ulog("UPnP input: SSDP port 1900 refused (%s): the control points will not find it by themselves", strerror(errno));
    } else {
        unsigned char loop = 1;
        setsockopt(ssdp_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
        join_all();
    }
    ud_init();
    pthread_t th;
    if (pthread_create(&th, NULL, http_main, NULL)) return -1;
    pthread_detach(th);
    if (!pthread_create(&th, NULL, tick_main, NULL)) pthread_detach(th);
    if (!pthread_create(&th, NULL, events_main, NULL)) pthread_detach(th);
    if (ssdp_fd >= 0 && !pthread_create(&th, NULL, ssdp_main, NULL)) pthread_detach(th);
    started = 1;
    ulog("UPnP input: \"%s\" (%s), http://%s:%d/description.xml%s%s", fname(), udn, if_only ? ifname : "<this machine>", http_port,
         atomic_load(&en_live) ? "" : ", switched off (upnp=0)", atomic_load(&vol_digital) ? ", its volume applied" : "");
    return 0;
}
void upnp_shutdown(void) {
    if (!started) return;
    atomic_store(&quit_, 1);
    pthread_mutex_lock(&T.m);
    stop_now();
    pthread_mutex_unlock(&T.m);
    if (atomic_load(&en_live)) notify_all(0);
}
int upnp_status(char *out, size_t len) {
    char fmt[96];
    ud_format(fmt, sizeof fmt);
    pthread_mutex_lock(&T.m);
    const int st = T.state;
    char title[160];
    snprintf(title, sizeof title, "%.159s", T.cur.title);
    for (char *p = title; *p; p++) if ((unsigned char)*p < 0x20) *p = ' ';
    const int vol = T.vol, mute = T.mute;
    pthread_mutex_unlock(&T.m);
    const int n = snprintf(out, len, "upnp=%s\nupnp_state=%s\nupnp_volume=%s\nupnp_vol=%d\nupnp_mute=%d\nupnp_format=%s\nupnp_udn=%s\nupnp_port=%d\n",
                           atomic_load(&en_live) ? "on" : "off", ST_NAME[st], atomic_load(&vol_digital) ? "digital" : "fixed", vol, mute,
                           st == ST_PLAYING || st == ST_PAUSED ? fmt : "", udn, http_port);
    return n > 0;
}
