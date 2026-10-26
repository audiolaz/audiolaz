/* src/player/netout.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef _DEFAULT_SOURCE   /* build.sh passes -D_DEFAULT_SOURCE */
#define _DEFAULT_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "netout.h"
#include <sys/wait.h>
#include "anet.h"
#include "azsrc.h"    /* the corrector's own rate converter: a rate a receiver's DAC takes, never its machine's */

#define BPF 6                    /* bytes per frame: two 24-bit samples (S.bpf: 4 to the AirPlay bridge, 10/10) */
#define CHUNK 2048               /* frames packed per write */
#define AP_RATE 44100            /* the AirPlay device's rate (libraop's): what the bridge is sent, converted here */
#define STALL_S 10.0             /* nothing taken for this long while playing (not paused): the receiver is lost */
#define GONE_S 5.0               /* its stream connection closed: this long for it to come again (a renderer's retry) */
#define FETCH_S 10.0             /* after it is told to play: this long to fetch the stream */
#define TAIL_S 30.0              /* the last stream's tail: at most this long for the receiver to finish it */
#define SEARCH_S 30.0            /* SSDP: a search every 30 s */
#define FORGET_S 150.0           /* a renderer not seen for this long is forgotten */

static void nlog(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    fputs("net: ", stderr); vfprintf(stderr, fmt, a); fputc('\n', stderr); fflush(stderr);
    va_end(a);
}
static const char *strcasestr_(const char *h, const char *n) {   /* strcasestr, GNU's: not in the portable build */
    const size_t k = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, k)) return h;
    return NULL;
}
static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void put32be(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put16be(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32le(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16le(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static uint32_t get32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ── The receivers: Squeezebox players connected to us, renderers found ───────────────────────────────── */
enum { NK_NONE, NK_SQUEEZE, NK_UPNP, NK_AIRPLAY, NK_AUDIOLAZ };
#define SLIM(k) ((k) == NK_SQUEEZE || (k) == NK_AIRPLAY)   /* a SlimProto connection of ours: squeezelite, the bridge */
#define AP_MODEL "audiolaz-airplay"                         /* the bridge's -M: its players are AirPlay devices */
typedef struct {
    int used, kind;
    int self;                                     /* AirPlay: the player's own AirPlay input (never listed, never played to) */
    char id[96], name[96], model[64], addr[48];   /* id: the MAC (aa:bb:…) or the UDN (uuid:…) */
    char hw[96];                                  /* audiolaz: its DAC's identity as it says it (anet.h anet_peer) */
    char rates[96], err[24];                      /* audiolaz: the rates its DAC takes as it is, its error (INFO) */
    char bits[48];                                /* audiolaz: the bits its DAC keeps (INFO bits=, anet.h) */
    unsigned max_rate;                            /* its HELO's MaxSampleRate (0: not said) */
    double seen;
    int fd;                                       /* Squeezebox: its SlimProto connection */
    struct in_addr local;                         /* our address as it reaches us */
    unsigned n_stms, n_stmd, n_stmu, n_stmf, n_stmn, n_stmc;
    unsigned stmu_at_stms;                        /* n_stmu at its last STMs: an STMu after it is that stream's */
    unsigned stmf_at_stmc;                        /* n_stmf at its last STMc : an STMf before it answers our strm s */
    long elapsed_ms;                              /* its last STAT's: the playing stream's, from its first frame */
    double t_stat;
    double t_stms;                                /* when its last STMs came (10/10: the AirPlay bridge's elapsed time
                                                   * never counted beyond the time since, netout_held) */
    double t_stmu;                                /* when its last STMu came (10/10: the bridge idle since, netout_open) */
    char loc[256], av_url[320], av_type[96];      /* UPnP: its description, its AVTransport's control URL and type */
    char rc_url[320], rc_type[96];                /* its RenderingControl's: the sender's volume set there ("" none) */
    int lent;                                     /* lent to a voie (relay below): its connection's fd + 1, 0 none */
    char maker[64];                               /* UPnP: its description's manufacturer ("" none) */
} rcv;

#define NRCV 32
static rcv R[NRCV];
static pthread_mutex_t rlk = PTHREAD_MUTEX_INITIALIZER;      /* R; taken after slk, never before it */
static pthread_mutex_t sendlk = PTHREAD_MUTEX_INITIALIZER;   /* one SlimProto message at a time */
static int http_port_ = 9001;
/* The next stream's start hints (09/10, netout_low_latency): an AirPlay video play wants the receiver to start on little. */
static int low_lat = 0;
void netout_low_latency(int on) { low_lat = on; }

static int smooth_next = 0;
void netout_smooth_position(int on) { smooth_next = on; }

/* ── The stream playing (one per play: one rate) ────────────────────────────────────────────────────── */
static struct {
    int on, kind, ri;
    unsigned id, rate;
    int bpf;                                      /* bytes per frame: 6 (24 bits), 4 (16 bits, dithered: the AirPlay bridge) */
    int fd;                                       /* the receiver's HTTP connection, -1 before it comes (or after it went) */
    uint8_t pend[CHUNK * BPF];
    size_t plen, poff;                            /* packed, not yet sent: pend[poff … plen) */
    uint64_t bytes;                               /* audio bytes the network took */
    int full_once, paused, lost, ended;          /* ended: all sent, its end of file given (netout_end), still followed */
    double t_take, t_gone, t_block;               /* t_block: since when nothing was taken (0: it takes) */
    unsigned stms0, stmd0, stmu0, stmf0, stmn0, stmc0;   /* the Squeezebox player's counters when it was told this stream */
    int tstate, fails, seen_playing, rel_moved;   /* UPnP: its transport state (below), failed polls in a row */
    double rel, t_rel, t_playing;                 /* its position (s) and when read; when it was first seen playing */
    double t_pause0, paused_s;                    /* since when paused (0: not), the time paused before: not played */
    int rel_whole, low;                           /* its position in whole seconds (09/10, below); low: netout_low_latency */
    double t_poll;                                /* when its position was last read */
    double t_open;                                /* when it was opened: its age, said to the other players */
    int taken;                                    /* lost because another sender took the receiver: not told to stop */
    int uri_ours;                                 /* (10/10, UPnP) its TrackURI seen as our stream's */
    int gone;                                     /* lost because the receiver left (the bridge lost it, or stopped) */
    int smooth, n_corr;                           /* a renderer's position smoothed (netout_smooth_position) */
    double corr, corr0;                           /* its position less the time since it was seen playing (s), averaged;
                                                   * that difference when it settled (ten reads) */
    /* to the AirPlay bridge at another rate than the device's 44.1 kHz: converted here (azsrc.h), then made 16
     * bits once (see netout_open). rate is then what goes on the wire (44100), irate the play's own; every count
     * the play reads (netout_sent, netout_held) said in irate's frames. cv NULL: none (irate == rate). */
    unsigned irate;
    azsrc *cv;
    int32_t *cvb;                                 /* its output, CHUNK frames, before pack16 */
    uint64_t cin;                                 /* the play's frames the converter took (this stream) */
    long clips;                                   /* its samples beyond full scale (intersample peaks), clipped */
} S = { .fd = -1, .bpf = BPF };
enum { TS_UNKNOWN, TS_PLAYING, TS_PAUSED, TS_STOPPED, TS_TRANSITIONING, TS_NO_MEDIA };
static pthread_mutex_t slk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scv = PTHREAD_COND_INITIALIZER;
static unsigned stream_ids = 0;
static char spec_now[160];                        /* the out= chosen (netout_select), under slk */
static char why_[600];

static const double BK_STEPS[] = { 2, 5, 15, 30, 60 };
#define BK_STALE_S 35.0
static struct { char spec[160], name[96]; int fails; double until; } BK;   /* under slk */
static void bk_after(const char *spec, const char *name, int played, double floor_s) {   /* under slk */
    if (strcmp(BK.spec, spec)) { memset(&BK, 0, sizeof BK); snprintf(BK.spec, sizeof BK.spec, "%s", spec); }
    snprintf(BK.name, sizeof BK.name, "%s", name);
    if (played) BK.fails = 0;
    const int k = BK.fails < (int)(sizeof BK_STEPS / sizeof BK_STEPS[0]) ? BK.fails : (int)(sizeof BK_STEPS / sizeof BK_STEPS[0]) - 1;
    double w = played ? BK_STEPS[0] : BK_STEPS[k];
    if (!played) BK.fails++;
    if (w < floor_s) w = floor_s;
    BK.until = mono() + w;
}
static void bk_ok(const char *spec) {                 /* under slk: it plays */
    if (!strcmp(BK.spec, spec)) { BK.fails = 0; BK.until = 0; }
}
double netout_retry_in(char *name, size_t len) {
    pthread_mutex_lock(&slk);
    const double w = BK.spec[0] && !strcmp(BK.spec, spec_now) ? BK.until - mono() : 0;
    if (name && len) snprintf(name, len, "%s", w > 0 ? BK.name : "");
    pthread_mutex_unlock(&slk);
    return w > 0 ? w : 0;
}
/* The last stream, ended (an end of file) and still playing out on its receiver: a new one there waits for it. */
static struct { int on, kind, ri; unsigned stmd0, stmu0, stmf0; double t; } F;

static void set_why(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vsnprintf(why_, sizeof why_, fmt, a); va_end(a);
}
const char *netout_why(void) { return why_; }

static int spec_kind(const char *s, const char **key) {
    if (!s) return NK_NONE;
    if (!strncmp(s, "squeeze:", 8) && s[8]) { *key = s + 8; return NK_SQUEEZE; }
    if (!strncmp(s, "upnp:", 5) && s[5]) { *key = s + 5; return NK_UPNP; }
    if (!strncmp(s, "airplay:", 8) && s[8]) { *key = s + 8; return NK_AIRPLAY; }
    if (!strncmp(s, "audiolaz:", 9) && s[9]) { *key = s + 9; return NK_AUDIOLAZ; }
    return NK_NONE;
}
int netout_is_net(const char *spec) { const char *k; return spec_kind(spec, &k) != NK_NONE; }
int netout_is_airplay(const char *spec) { const char *k; return spec_kind(spec, &k) == NK_AIRPLAY; }

/* ── AirPlay devices' names : the bridge's config.xml, the player's own AirPlay input ─────────────── */
static char ap_conf[512] = "/var/lib/audiolaz-airplay-out/config.xml", self_name[96] = "";
static struct { char mac[24], name[96]; } AP[64];
static int n_ap;
static int ap_ahead_ms = 1000;                       /* its <read_ahead> : the RAOP latency it asks, ms (1000 when not said) */
static double t_ap;
static time_t ap_mtime;
static off_t ap_size;
static pthread_mutex_t aplk = PTHREAD_MUTEX_INITIALIZER;     /* AP; never taken under rlk */
void netout_airplay_conf(const char *path) { if (path) snprintf(ap_conf, sizeof ap_conf, "%s", path); }
void netout_set_self(const char *name) { snprintf(self_name, sizeof self_name, "%s", name ? name : ""); }
static char self_udn[96] = "";
void netout_set_self_upnp(const char *u) { snprintf(self_udn, sizeof self_udn, "%s", u ? u : ""); }
static char self_name2[160] = "";                 /* under aplk */
void netout_set_self2(const char *name) {
    pthread_mutex_lock(&aplk);
    snprintf(self_name2, sizeof self_name2, "%s", name ? name : "");
    pthread_mutex_unlock(&aplk);
}
static int xml_text(const char *x, const char *tag, char *out, size_t len);

static void ap_clean(char *n) {
    static const char *const TAIL[] = { ".local", "._raop._tcp", "._airplay._tcp" };
    for (int k = 0; k < 3; k++) {
        const size_t l = strlen(n), t = strlen(TAIL[k]);
        if (l > t && !strcasecmp(n + l - t, TAIL[k])) n[l - t] = 0;
    }
}

static void ap_load(void) {
    pthread_mutex_lock(&aplk);
    const double t = mono();
    if (t - t_ap < 0.25) { pthread_mutex_unlock(&aplk); return; }
    t_ap = t;
    struct stat st;
    if (stat(ap_conf, &st) || (st.st_mtime == ap_mtime && st.st_size == ap_size)) { pthread_mutex_unlock(&aplk); return; }
    FILE *f = st.st_size > 0 && st.st_size < 1 << 20 ? fopen(ap_conf, "r") : NULL;
    char *x = f ? malloc((size_t)st.st_size + 1) : NULL;
    const size_t got = x ? fread(x, 1, (size_t)st.st_size, f) : 0;
    if (f) fclose(f);
    if (x) {
        x[got] = 0;
        ap_mtime = st.st_mtime; ap_size = st.st_size; n_ap = 0;
        char ra[16] = "";
        const char *cm = strstr(x, "<common>");
        if (cm && xml_text(cm, "read_ahead", ra, sizeof ra) && atoi(ra) > 0) ap_ahead_ms = atoi(ra);
        for (char *p = x, *e; (p = strstr(p, "<device>")) && (e = strstr(p, "</device>")) && n_ap < 64; p = e + 9) {
            *e = 0;
            char udn[160] = "", mac[24] = "";
            if (xml_text(p, "udn", udn, sizeof udn) && xml_text(p, "mac", mac, sizeof mac) && strchr(udn, '@')) {
                snprintf(AP[n_ap].mac, sizeof AP[n_ap].mac, "%s", mac);
                snprintf(AP[n_ap].name, sizeof AP[n_ap].name, "%s", strchr(udn, '@') + 1);
                ap_clean(AP[n_ap].name);
                n_ap++;
            }
            *e = '<';
        }
        free(x);
    }
    pthread_mutex_unlock(&aplk);
}

static int ap_audiolaz_input(const char *name, char names[][96], int nn) {
    static const char MARK[] = " \u00b7 audiolaz";
    const char *m = strstr(name, MARK);
    if (m && (m[sizeof MARK - 1] == 0 || m[sizeof MARK - 1] == ' ')) return 1;
    for (int k = 0; k < nn; k++) if (names[k][0] && !strcasecmp(name, names[k])) return 1;
    return 0;
}
static void claim_names(char names[][96], int *nn, int max);
/* The AirPlay receivers named from the bridge's config, the player's own input marked (self), and any audiolaz
 * player's ; each one's name else what the bridge said (SETD). Called before rlk is taken. */
static void ap_names(void) {
    ap_load();
    char others[32][96];                           /* on the stack: ap_names runs from several threads */
    int no = 0;
    claim_names(others, &no, 32);
    pthread_mutex_lock(&aplk);
    pthread_mutex_lock(&rlk);
    for (int i = 0; i < NRCV; i++) {
        if (!R[i].used || R[i].kind != NK_AIRPLAY) continue;
        ap_clean(R[i].name);
        for (int k = 0; k < n_ap; k++)
            if (!strcasecmp(AP[k].mac, R[i].id)) {
                if (strcmp(R[i].name, AP[k].name)) snprintf(R[i].name, sizeof R[i].name, "%s", AP[k].name);
                break;
            }
        const int own = (self_name[0] && !strcasecmp(R[i].name, self_name)) || (self_name2[0] && !strcasecmp(R[i].name, self_name2));
        const int self = own || ap_audiolaz_input(R[i].name, others, no);
        if (self && !R[i].self) nlog("AirPlay device \"%s\" (%s): %s AirPlay input, never an output", R[i].name, R[i].id, own ? "the player's own" : "an audiolaz player's");
        R[i].self = self;
    }
    pthread_mutex_unlock(&rlk);
    pthread_mutex_unlock(&aplk);
}
void netout_select(const char *spec) {
    pthread_mutex_lock(&slk);
    snprintf(spec_now, sizeof spec_now, "%s", spec ? spec : "");
    pthread_mutex_unlock(&slk);
}
/* Under rlk: the receiver an out= names (its id or its name, any case), -1 for none there now. */
static int find_rcv(int kind, const char *key) {
    for (int i = 0; i < NRCV; i++)
        if (R[i].used && R[i].kind == kind && (!SLIM(kind) || R[i].fd >= 0) && !R[i].self && (!strcasecmp(R[i].id, key) || !strcasecmp(R[i].name, key) ||
            (kind == NK_AUDIOLAZ && (!strcasecmp(R[i].loc, key) || !strcasecmp(R[i].addr, key))))) return i;   /* audiolaz:<address>[:<port>] too */
    return -1;
}

/* ── Small network helpers ───────────────────────────────────────────────────────────────────────────── */
static int send_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) { ssize_t k = send(fd, b, n, MSG_NOSIGNAL); if (k <= 0) { if (k < 0 && errno == EINTR) continue; return -1; } b += k; n -= (size_t)k; }
    return 0;
}
static int recv_all(int fd, void *p, size_t n) {
    uint8_t *b = p;
    while (n) { ssize_t k = recv(fd, b, n, 0); if (k <= 0) { if (k < 0 && errno == EINTR) continue; return -1; } b += k; n -= (size_t)k; }
    return 0;
}
static void timeouts(int fd, int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
/* Only this machine's own receivers (09/10, Room for Mac: its AirPlay bridge runs beside it): the ports on the loopback,
 * no UPnP search (nothing on the network asked, no firewall question for ports nobody else uses). */
static int local_only = 0;
void netout_local_only(int on) { local_only = on; }
static int listen_on(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0), on = 1;
    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(local_only ? INADDR_LOOPBACK : INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 8)) { close(s); return -1; }
    return s;
}
/* A stream connection closed as an end of file: our side shut, then what the receiver may still send read and dropped
 * until it closes (closing with unread bytes would reset the connection, and the receiver lose what we sent last). */
static void *linger_main(void *arg) {
    int fd = (int)(intptr_t)arg;
    shutdown(fd, SHUT_WR);
    char b[512];
    const double t0 = mono();
    while (mono() - t0 < 60) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 1000) > 0 && recv(fd, b, sizeof b, 0) <= 0) break;
    }
    close(fd);
    return NULL;
}
static void close_gently(int fd) {
    pthread_t th;
    if (pthread_create(&th, NULL, linger_main, (void *)(intptr_t)fd)) { shutdown(fd, SHUT_WR); close(fd); return; }
    pthread_detach(th);
}

#define CLAIM_PORT 3489
#define NCLAIM 32
#define CLAIM_PLAY_EVERY 2.0
#define CLAIM_IDLE_EVERY 10.0
#define CLAIM_PLAY_KEEP 6.5
#define CLAIM_IDLE_KEEP 31.0
typedef struct {
    int used, state;                               /* state: 0 idle, 1 set to it, 2 playing to it */
    char id[24], name[96], kind[12], rid[96], rname[96], addr[48];
    double seen, age;                              /* age: how long it has played (s), when said */
} claim;
static claim CL[NCLAIM];
static pthread_mutex_t clk = PTHREAD_MUTEX_INITIALIZER;     /* CL, me_name, bound_spec, TK; never taken with slk or rlk held */
static char me_id[24], me_name[96] = "audiolaz", bound_spec[160];
static int claim_fd = -1, claim_port = CLAIM_PORT, claim_ignore = 0;
static char claim_to[256] = "";                    /* "host:port,…" (the tests); "" each interface's broadcast */
/* The last stream taken by another sender : its out=, its receiver's name, the taker's ("" not known). */
static struct { char spec[160], rname[96], by[96]; } TK;
static void json_escape(const char *s, char *out, size_t len);
static int kind_of(const char *k) { return !strcmp(k, "audiolaz") ? NK_AUDIOLAZ : !strcmp(k, "airplay") ? NK_AIRPLAY : !strcmp(k, "upnp") ? NK_UPNP : !strcmp(k, "squeeze") ? NK_SQUEEZE : NK_NONE; }
static const char *kind_name(int k) { return k == NK_AUDIOLAZ ? "audiolaz" : k == NK_AIRPLAY ? "airplay" : k == NK_UPNP ? "upnp" : k == NK_SQUEEZE ? "squeeze" : ""; }
static void one_line(char *s) { for (; *s; s++) if ((unsigned char)*s < 0x20) *s = ' '; }
void netout_set_name(const char *name) {
    pthread_mutex_lock(&clk);
    snprintf(me_name, sizeof me_name, "%s", name && *name ? name : "audiolaz");
    one_line(me_name);
    pthread_mutex_unlock(&clk);
}
void netout_claims(int port, const char *to) {
    claim_port = port;
    snprintf(claim_to, sizeof claim_to, "%s", to ? to : "");
}
void netout_claim_ignore(int on) { claim_ignore = on; }
/* A claim names this receiver (kind, its id and name as we know it). */
static int claim_is(const claim *c, int kind, const char *id, const char *name) {
    if (kind_of(c->kind) != kind) return 0;
    if (c->rid[0] && ((id[0] && !strcasecmp(c->rid, id)) || (name[0] && !strcasecmp(c->rid, name)))) return 1;
    if (c->rname[0] && ((name[0] && !strcasecmp(c->rname, name)) || (id[0] && !strcasecmp(c->rname, id)))) return 1;
    return 0;
}
/* An out= → its kind, the receiver's id and name (as found here now; else its key as both). */
static int spec_ids(const char *spec, char *id, size_t il, char *name, size_t nl) {
    const char *key;
    const int kind = spec_kind(spec, &key);
    id[0] = name[0] = 0;
    if (!kind) return NK_NONE;
    pthread_mutex_lock(&rlk);
    int i = -1;
    for (int k = 0; k < NRCV && i < 0; k++)
        if (R[k].used && R[k].kind == kind && !R[k].self && (!strcasecmp(R[k].id, key) || !strcasecmp(R[k].name, key))) i = k;
    if (i >= 0) { snprintf(id, il, "%s", R[i].id); snprintf(name, nl, "%s", R[i].name); }
    pthread_mutex_unlock(&rlk);
    if (i < 0) { snprintf(id, il, "%s", key); snprintf(name, nl, "%s", key); }
    return kind;
}
/* Our datagram (op ann or who) into b. */
static int claim_msg(const char *op, char *b, size_t len) {
    char spec[160], id[96] = "", name[96] = "", me[96];
    int state = 0, kind = 0;
    double age = 0;
    pthread_mutex_lock(&slk);
    const int on = S.on && !S.lost, ri = S.ri;
    if (on) { kind = S.kind; age = mono() - S.t_open; }
    pthread_mutex_unlock(&slk);
    if (on) {
        pthread_mutex_lock(&rlk);
        if (R[ri].used) { snprintf(id, sizeof id, "%s", R[ri].id); snprintf(name, sizeof name, "%s", R[ri].name); }
        pthread_mutex_unlock(&rlk);
        state = 2;
    }
    pthread_mutex_lock(&clk);
    snprintf(spec, sizeof spec, "%s", bound_spec);
    snprintf(me, sizeof me, "%s", me_name);
    pthread_mutex_unlock(&clk);
    if (!on && (kind = spec_ids(spec, id, sizeof id, name, sizeof name))) state = 1;
    one_line(id); one_line(name);
    return snprintf(b, len, "AUDIOLAZ-OUT 1\nop=%s\nid=%s\nname=%s\nstate=%s\nkind=%s\nrid=%s\nrname=%s\nage=%.1f\n",
                    op, me_id, me, state == 2 ? "play" : state == 1 ? "set" : "idle", kind_name(kind), id, name, age);
}
/* Where our datagrams go: claim_to's addresses, else each interface's broadcast address (255.255.255.255 when none). */

/* whether addr (dotted IPv4) is one of this machine's own, the list read again every 30 s */
static int own_ipv4(const char *addr) {
    static char own[16][INET_ADDRSTRLEN]; static int nown = 0; static double at = -1;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    const double now = ts.tv_sec + ts.tv_nsec / 1e9;
    if (at < 0 || now - at > 30) {
        nown = 0; at = now;
        struct ifaddrs *ifs = NULL;
        if (!getifaddrs(&ifs)) {
            for (struct ifaddrs *f = ifs; f && nown < 16; f = f->ifa_next)
                if (f->ifa_addr && f->ifa_addr->sa_family == AF_INET)
                    inet_ntop(AF_INET, &((struct sockaddr_in *)f->ifa_addr)->sin_addr, own[nown++], INET_ADDRSTRLEN);
            freeifaddrs(ifs);
        }
    }
    if (!strncmp(addr, "127.", 4)) return 0;   /* the loopback: a receiver run here on purpose (the tests) */
    for (int i = 0; i < nown; i++) if (!strcmp(own[i], addr)) return 1;
    return 0;
}

static int claim_dests(struct sockaddr_in *d, int max) {
    int n = 0;
    if (claim_to[0]) {
        char list[256];
        snprintf(list, sizeof list, "%s", claim_to);
        for (char *sv = NULL, *t = strtok_r(list, ",", &sv); t && n < max; t = strtok_r(NULL, ",", &sv)) {
            char *c = strrchr(t, ':');
            if (!c) continue;
            *c = 0;
            memset(&d[n], 0, sizeof d[n]);
            d[n].sin_family = AF_INET; d[n].sin_port = htons((uint16_t)atoi(c + 1));
            if (inet_pton(AF_INET, t, &d[n].sin_addr) == 1) n++;
        }
        return n;
    }
    struct ifaddrs *ifs = NULL;
    if (!getifaddrs(&ifs)) {
        for (struct ifaddrs *i = ifs; i && n < max; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)
                || !(i->ifa_flags & IFF_BROADCAST) || !i->ifa_broadaddr) continue;
            const struct in_addr b = ((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr;
            int dup = !b.s_addr;
            for (int k = 0; k < n; k++) if (d[k].sin_addr.s_addr == b.s_addr) dup = 1;
            if (dup) continue;
            memset(&d[n], 0, sizeof d[n]);
            d[n].sin_family = AF_INET; d[n].sin_port = htons((uint16_t)claim_port); d[n].sin_addr = b;
            n++;
        }
        freeifaddrs(ifs);
    }
    if (!n) { memset(&d[0], 0, sizeof d[0]); d[0].sin_family = AF_INET; d[0].sin_port = htons((uint16_t)claim_port); d[0].sin_addr.s_addr = htonl(INADDR_BROADCAST); n = 1; }
    return n;
}
static void claim_send(int fd, const char *op, const struct sockaddr_in *to) {
    char b[600];
    const int n = claim_msg(op, b, sizeof b);
    if (n <= 0 || fd < 0) return;
    if (to) { sendto(fd, b, (size_t)n, 0, (const struct sockaddr *)to, sizeof *to); return; }
    struct sockaddr_in d[8];
    const int nd = claim_dests(d, 8);
    for (int k = 0; k < nd; k++) sendto(fd, b, (size_t)n, 0, (struct sockaddr *)&d[k], sizeof d[k]);
}
static double claim_last = 0;
/* Our state said now (it changed: a stream opened, lost, closed). */
static void claim_announce(void) {
    if (claim_fd < 0) return;
    claim_send(claim_fd, "ann", NULL);
    claim_last = mono();
}
static void line_value(const char *b, const char *key, char *out, size_t len) {
    out[0] = 0;
    const size_t kl = strlen(key);
    for (const char *p = b; p && *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1, *e = strchr(v, '\n');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= len) n = len - 1;
            memcpy(out, v, n); out[n] = 0;
            return;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
}
/* Another player's state heard: kept; ours let go when it is newer on our receiver. */
static void claim_heard(const char *b, const struct sockaddr_in *from) {
    claim c;
    memset(&c, 0, sizeof c);
    char op[8], st[8], age[16];
    line_value(b, "op", op, sizeof op); line_value(b, "id", c.id, sizeof c.id);
    if (!c.id[0] || !strcmp(c.id, me_id)) return;
    if (!strcmp(op, "who")) { claim_send(claim_fd, "ann", from); return; }
    if (strcmp(op, "ann")) return;
    line_value(b, "name", c.name, sizeof c.name); line_value(b, "state", st, sizeof st); line_value(b, "kind", c.kind, sizeof c.kind);
    line_value(b, "rid", c.rid, sizeof c.rid); line_value(b, "rname", c.rname, sizeof c.rname); line_value(b, "age", age, sizeof age);
    c.state = !strcmp(st, "play") ? 2 : !strcmp(st, "set") ? 1 : 0;
    c.age = atof(age); c.seen = mono(); c.used = 1;
    inet_ntop(AF_INET, &from->sin_addr, c.addr, sizeof c.addr);
    pthread_mutex_lock(&clk);
    int k = -1;
    for (int i = 0; i < NCLAIM && k < 0; i++) if (CL[i].used && !strcmp(CL[i].id, c.id)) k = i;
    const int was = k >= 0 ? CL[k].state : -1;
    if (k < 0) for (int i = 0; i < NCLAIM && k < 0; i++) if (!CL[i].used) k = i;
    if (k < 0) { k = 0; for (int i = 1; i < NCLAIM; i++) if (CL[i].seen < CL[k].seen) k = i; }
    CL[k] = c;
    pthread_mutex_unlock(&clk);
    if (was != c.state && c.state == 2) nlog("the audiolaz player \"%s\" (%s) plays to %s \"%s\"", c.name, c.addr, c.kind, c.rname[0] ? c.rname : c.rid);
    if (c.state != 2 || claim_ignore) return;
    /* playing to ours, and newer than our stream (a tie: the larger id lets go): ours was taken */
    pthread_mutex_lock(&slk);
    int lose = 0;
    const int ri = S.ri, kind = S.kind;
    char spec[160];
    snprintf(spec, sizeof spec, "%s", spec_now);
    if (S.on && !S.lost) {
        const double mine = mono() - S.t_open;
        lose = c.age < mine - 0.3 || (fabs(c.age - mine) <= 0.3 && strcmp(me_id, c.id) > 0);
    }
    pthread_mutex_unlock(&slk);
    if (!lose) return;
    char id[96] = "", name[96] = "";
    pthread_mutex_lock(&rlk);
    if (R[ri].used && R[ri].kind == kind) { snprintf(id, sizeof id, "%s", R[ri].id); snprintf(name, sizeof name, "%s", R[ri].name); }
    pthread_mutex_unlock(&rlk);
    if (!claim_is(&c, kind, id, name)) return;
    pthread_mutex_lock(&slk);
    const int hit = S.on && !S.lost && S.ri == ri;
    if (hit) { S.lost = 1; S.taken = 1; set_why("\"%s\" taken by the audiolaz player \"%s\" (%s)", name, c.name, c.addr); }
    pthread_mutex_unlock(&slk);
    if (!hit) return;
    pthread_mutex_lock(&clk);
    snprintf(TK.spec, sizeof TK.spec, "%s", spec); snprintf(TK.rname, sizeof TK.rname, "%s", name); snprintf(TK.by, sizeof TK.by, "%s", c.name);
    pthread_mutex_unlock(&clk);
    nlog("%s \"%s\" taken by the audiolaz player \"%s\" (%s): let go", kind_name(kind), name, c.name, c.addr);
    claim_announce();
}
static void *claims_main(void *arg) {
    (void)arg;
    char b[1024], sig[600] = "";
    for (;;) {
        struct pollfd p = { claim_fd, POLLIN, 0 };
        if (poll(&p, 1, 250) > 0) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            const ssize_t n = recvfrom(claim_fd, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl);
            if (n > 0) { b[n] = 0; if (!strncmp(b, "AUDIOLAZ-OUT 1\n", 15)) claim_heard(b, &from); }
        }
        /* ours: every 2 s while playing, every 10 s otherwise, at once when it changed */
        char m[600];
        claim_msg("ann", m, sizeof m);
        char *age = strstr(m, "\nage=");
        if (age) age[1] = 0;                             /* the age changes each time: not a change */
        const int playing = strstr(m, "\nstate=play\n") != NULL;
        const double t = mono();
        if (strcmp(m, sig) || t - claim_last >= (playing ? CLAIM_PLAY_EVERY : CLAIM_IDLE_EVERY)) {
            snprintf(sig, sizeof sig, "%s", m);
            claim_announce();
        }
    }
    return NULL;
}
static double claim_t0 = 0;
static void claims_start(void) {
    if (claim_port <= 0 || (local_only && !claim_to[0])) return;   /* the tests: local_only and claim_to */
    unsigned seed = (unsigned)time(NULL) ^ ((unsigned)getpid() << 16) ^ (unsigned)(mono() * 1e6);
    snprintf(me_id, sizeof me_id, "%08x%08x", (unsigned)rand_r(&seed), (unsigned)rand_r(&seed) ^ (unsigned)getpid());
    const int s = socket(AF_INET, SOCK_DGRAM, 0), on = 1;
    if (s < 0) return;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
#endif
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)claim_port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a)) { nlog("UDP port %d (which receiver each audiolaz player uses) refused: %s", claim_port, strerror(errno)); close(s); return; }
    claim_fd = s;
    claim_t0 = mono();
    pthread_t th;
    if (pthread_create(&th, NULL, claims_main, NULL)) { close(s); claim_fd = -1; return; }
    pthread_detach(th);
    claim_send(claim_fd, "who", NULL);                  /* the others' answers at once, not in 10 s */
}
void netout_bound(const char *spec) {
    pthread_mutex_lock(&clk);
    snprintf(bound_spec, sizeof bound_spec, "%s", spec && netout_is_net(spec) ? spec : "");
    pthread_mutex_unlock(&clk);
}
/* The other audiolaz players' names heard (ap_names: their AirPlay inputs named after them alone). */
static void claim_names(char names[][96], int *nn, int max) {
    *nn = 0;
    const double t = mono();
    pthread_mutex_lock(&clk);
    for (int i = 0; i < NCLAIM && *nn < max; i++)
        if (CL[i].used && t - CL[i].seen <= CLAIM_IDLE_KEEP && CL[i].name[0]) snprintf(names[(*nn)++], 96, "%s", CL[i].name);
    pthread_mutex_unlock(&clk);
}

static const double AZB_STEPS[] = { 2, 5, 10, 15 };
#define AZB_KEEP_S 25.0
#define AZB_TAKE_S 30.0
static pthread_mutex_t azlk = PTHREAD_MUTEX_INITIALIZER;   /* AZB; taken alone (never with another of ours held) */
static struct { char spec[160], by[96]; int fails; double until, t_first, t_last, take_until; } AZB;
int netout_busy(const char *spec, char *by, size_t len) {
    char sp[160];
    if (spec) snprintf(sp, sizeof sp, "%s", spec);
    else { pthread_mutex_lock(&slk); snprintf(sp, sizeof sp, "%s", spec_now); pthread_mutex_unlock(&slk); }
    pthread_mutex_lock(&azlk);
    const int b = sp[0] && !strcmp(AZB.spec, sp) && AZB.fails > 0 && mono() - AZB.t_last <= AZB_KEEP_S;
    if (by && len) snprintf(by, len, "%s", b ? AZB.by : "");
    pthread_mutex_unlock(&azlk);
    return b;
}
void netout_take(void) {
    pthread_mutex_lock(&azlk);
    AZB.take_until = mono() + AZB_TAKE_S; AZB.until = 0;
    pthread_mutex_unlock(&azlk);
}
/* the receiver of this row the one that refused us (its key: its id, name, address or address:port) */
static int az_busy_row(const rcv *r, char *by, size_t len) {
    pthread_mutex_lock(&azlk);
    const char *key = !strncmp(AZB.spec, "audiolaz:", 9) ? AZB.spec + 9 : "";
    const int b = r->kind == NK_AUDIOLAZ && key[0] && AZB.fails > 0 && mono() - AZB.t_last <= AZB_KEEP_S
                  && (!strcasecmp(key, r->id) || !strcasecmp(key, r->name) || !strcmp(key, r->loc) || !strcmp(key, r->addr));
    if (b && by && len) snprintf(by, len, "%s", AZB.by);
    pthread_mutex_unlock(&azlk);
    return b;
}
/* Another player on this out=: 2 playing to it, 1 set to it only, 0 none; its name. An audiolaz receiver
 * that refused us: the player playing there, 2. */
int netout_used_by(const char *spec, char *name, size_t len) {
    char id[96], rn[96];
    if (name && len) name[0] = 0;
    if (spec && !strncmp(spec, "audiolaz:", 9) && netout_busy(spec, name, len)) return 2;
    const int kind = spec ? spec_ids(spec, id, sizeof id, rn, sizeof rn) : NK_NONE;
    if (!kind) return 0;
    int best = 0;
    const double t = mono();
    pthread_mutex_lock(&clk);
    for (int i = 0; i < NCLAIM; i++) {
        const claim *c = &CL[i];
        if (!c->used || c->state <= best || t - c->seen > (c->state == 2 ? CLAIM_PLAY_KEEP : CLAIM_IDLE_KEEP)) continue;
        if (!claim_is(c, kind, id, rn)) continue;
        best = c->state;
        if (name && len) snprintf(name, len, "%s", c->name);
    }
    pthread_mutex_unlock(&clk);
    return best;
}
/* The receiver this out= names, when its last stream was taken by another sender: its name, the taker's ("" unknown). */
int netout_taken(const char *spec, char *rname, size_t rl, char *by, size_t bl) {
    pthread_mutex_lock(&clk);
    const int hit = spec && *spec && TK.spec[0] && !strcmp(TK.spec, spec);
    if (hit) { snprintf(rname, rl, "%s", TK.rname); snprintf(by, bl, "%s", TK.by); }
    pthread_mutex_unlock(&clk);
    return hit;
}
static void taken_clear(const char *spec) {
    pthread_mutex_lock(&clk);
    if (!strcmp(TK.spec, spec)) TK.spec[0] = 0;
    pthread_mutex_unlock(&clk);
}
/* The other players heard, as JSON: [{name, addr, state, kind, id, rname}]. */
void netout_players_json(char *out, size_t len) {
    size_t w = (size_t)snprintf(out, len, "[");
    const double t = mono();
    pthread_mutex_lock(&clk);
    for (int i = 0; i < NCLAIM; i++) {
        const claim *c = &CL[i];
        if (!c->used || t - c->seen > (c->state == 2 ? CLAIM_PLAY_KEEP : CLAIM_IDLE_KEEP)) continue;
        char nm[200], id[200], rn[200], kd[30];
        json_escape(c->name, nm, sizeof nm); json_escape(c->rid, id, sizeof id); json_escape(c->rname, rn, sizeof rn); json_escape(c->kind, kd, sizeof kd);
        char item[800];
        const int n = snprintf(item, sizeof item, "%s{\"name\":\"%s\",\"addr\":\"%s\",\"state\":\"%s\",\"kind\":\"%s\",\"id\":\"%s\",\"rname\":\"%s\"}",
                               w > 1 ? "," : "", nm, c->addr, c->state == 2 ? "play" : c->state == 1 ? "set" : "idle", kd, id, rn);
        if (n > 0 && w + (size_t)n + 2 < len) { memcpy(out + w, item, (size_t)n); w += (size_t)n; }
    }
    pthread_mutex_unlock(&clk);
    snprintf(out + w, len - w, "]");
}

int netout_who(int ms, char *out, size_t len) {
    out[0] = 0;
    const int s = socket(AF_INET, SOCK_DGRAM, 0), on = 1;
    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a)) { close(s); return -1; }
    if (!me_id[0]) snprintf(me_id, sizeof me_id, "who%d", (int)getpid());
    claim_send(s, "who", NULL);
    in_addr_t mine[16];
    int nm = 0;
    struct ifaddrs *ifs = NULL;
    if (!getifaddrs(&ifs)) {
        for (struct ifaddrs *i = ifs; i && nm < 16; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET) mine[nm++] = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
        freeifaddrs(ifs);
    }
    size_t w = 0;
    char seen[2048] = "";
    const double t0 = mono();
    while (mono() - t0 < ms / 1000.0) {
        struct pollfd p = { s, POLLIN, 0 };
        if (poll(&p, 1, 50) <= 0) continue;
        char b[1024];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const ssize_t n = recvfrom(s, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        b[n] = 0;
        if (strncmp(b, "AUDIOLAZ-OUT 1\n", 15)) continue;
        int local = 0;
        for (int k = 0; k < nm; k++) if (mine[k] == from.sin_addr.s_addr) local = 1;
        char id[24], name[96], st[8], ip[INET_ADDRSTRLEN], key[40];
        line_value(b, "id", id, sizeof id); line_value(b, "name", name, sizeof name); line_value(b, "state", st, sizeof st);
        snprintf(key, sizeof key, "|%s|", id);
        if ((local && !claim_to[0]) || !name[0] || strstr(seen, key)) continue;
        if (strlen(seen) + strlen(key) < sizeof seen) strcat(seen, key);
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        const int k = snprintf(out + w, len - w, "%s\t%s\t%s\n", name, ip, st);
        if (k > 0 && w + (size_t)k < len) w += (size_t)k;
    }
    close(s);
    return 0;
}

/* A plain HTTP request (a renderer's description, its SOAP control): the status (−1: unreachable), the body in out. */
static int parse_url(const char *url, char *host, size_t hl, int *port, const char **path) {
    if (strncasecmp(url, "http://", 7)) return -1;
    const char *h = url + 7, *e = h;
    while (*e && *e != ':' && *e != '/') e++;
    if ((size_t)(e - h) >= hl || e == h) return -1;
    memcpy(host, h, (size_t)(e - h)); host[e - h] = 0;
    *port = 80;
    if (*e == ':') { *port = atoi(e + 1); while (*e && *e != '/') e++; }
    *path = *e ? e : "/";
    return *port > 0 && *port < 65536 ? 0 : -1;
}
static int tcp_to(const char *host, int port, int ms) {
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return -1;    /* renderers give their address, not a name */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    fcntl(s, F_SETFL, O_NONBLOCK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) && errno != EINPROGRESS) { close(s); return -1; }
    struct pollfd p = { s, POLLOUT, 0 };
    int err = 0;
    socklen_t el = sizeof err;
    if (poll(&p, 1, ms) != 1 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) || err) { close(s); return -1; }
    fcntl(s, F_SETFL, 0);
    timeouts(s, ms);
    return s;
}
static int http_req(const char *url, const char *method, const char *extra, const char *body, char *out, size_t outlen) {
    char host[64], *req = NULL;
    const char *path;
    int port;
    if (out && outlen) out[0] = 0;
    if (parse_url(url, host, sizeof host, &port, &path)) return -1;
    int s = tcp_to(host, port, 2000);
    if (s < 0) return -1;
    const size_t bl = body ? strlen(body) : 0;
    const size_t rl = bl + strlen(path) + strlen(host) + (extra ? strlen(extra) : 0) + 256;
    if (!(req = malloc(rl))) { close(s); return -1; }
    int n = snprintf(req, rl, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\nUser-Agent: audiolaz-roomd\r\n%sContent-Length: %zu\r\n\r\n%s",
                     method, path, host, port, extra ? extra : "", bl, body ? body : "");
    const int sent = send_all(s, req, (size_t)n);
    free(req);
    if (sent) { close(s); return -1; }
    size_t got = 0, want = 0;                         /* want: its head and its Content-Length's body, once known */
    int head = 0;
    char *buf = malloc(65536);
    if (!buf) { close(s); return -1; }

    const double t_end = mono() + 3.0;
    for (;;) {
        const int left = (int)((t_end - mono()) * 1000);
        struct pollfd p = { s, POLLIN, 0 };
        if (left <= 0 || poll(&p, 1, left) <= 0) break;
        ssize_t k = recv(s, buf + got, 65535 - got, 0);
        if (k <= 0) break;
        got += (size_t)k; buf[got] = 0;
        if (!head) {
            const char *e = strstr(buf, "\r\n\r\n");
            if (e) {
                head = 1;
                for (const char *l = buf, *nl; l < e; l = nl + 2) {
                    if (!strncasecmp(l, "Content-Length:", 15)) { want = (size_t)(e + 4 - buf) + strtoul(l + 15, NULL, 10); break; }
                    if (!(nl = strstr(l, "\r\n"))) break;
                }
            }
        }
        if ((want && got >= want) || got >= 65535) break;
    }
    close(s);
    buf[got] = 0;
    int status = -1;
    if (sscanf(buf, "HTTP/%*s %d", &status) != 1) status = -1;
    const char *b = strstr(buf, "\r\n\r\n");
    if (out && outlen && b) snprintf(out, outlen, "%s", b + 4);
    free(buf);
    return status;
}
/* The first <tag>…</ text (a namespace prefix allowed), its entities undone; 0 when absent. */
static int xml_text(const char *x, const char *tag, char *out, size_t len) {
    const size_t tl = strlen(tag);
    for (const char *p = x; (p = strstr(p, tag)); p += tl) {
        if (p == x || (p[-1] != '<' && p[-1] != ':') || p[tl] != '>') continue;
        if (p[-1] == ':') { const char *q = p - 1; while (q > x && q[-1] != '<' && q[-1] != '/' && q[-1] != ' ' && q[-1] != '>') q--; if (q == x || q[-1] != '<') continue; }
        const char *v = p + tl + 1, *e = strchr(v, '<');
        if (!e) return 0;
        size_t w = 0;
        while (v < e && w + 1 < len) {
            if (*v == '&') {
                static const struct { const char *e; char c; } ENT[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' } };
                size_t k = 0;
                for (; k < sizeof ENT / sizeof *ENT; k++) if (!strncmp(v, ENT[k].e, strlen(ENT[k].e))) { out[w++] = ENT[k].c; v += strlen(ENT[k].e); break; }
                if (k < sizeof ENT / sizeof *ENT) continue;
            }
            out[w++] = *v++;
        }
        out[w] = 0;
        while (w && (out[w - 1] == ' ' || out[w - 1] == '\n' || out[w - 1] == '\r' || out[w - 1] == '\t')) out[--w] = 0;
        size_t s = 0;
        while (out[s] == ' ' || out[s] == '\n' || out[s] == '\r' || out[s] == '\t') s++;
        if (s) memmove(out, out + s, strlen(out + s) + 1);
        return 1;
    }
    return 0;
}
static void xml_escape(const char *s, char *out, size_t len) {
    size_t w = 0;
    for (; *s && w + 7 < len; s++) {
        const char *e = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : NULL;
        if (e) { memcpy(out + w, e, strlen(e)); w += strlen(e); } else out[w++] = *s;
    }
    out[w] = 0;
}
static void json_escape(const char *s, char *out, size_t len) {
    size_t w = 0;
    for (; *s && w + 3 < len; s++) {
        if (*s == '"' || *s == '\\') { out[w++] = '\\'; out[w++] = *s; }
        else if ((unsigned char)*s >= 0x20) out[w++] = *s;
    }
    out[w] = 0;
}

/* ── The HTTP stream server ───────────────────────────────────────────────────────────────────────────── */
static int http_fd = -1;
/* what plays, shown by the receiver: nowplay.h, included at the end */
static int np_http_cover(int c, const char *method, const char *path);
static int np_cli_start(void);
static int np_cli;
static void np_didl(char *out, size_t len, const char *ip);
static void wav_header(uint8_t *h, unsigned rate) {
    memcpy(h, "RIFF", 4); put32le(h + 4, 0xFFFFFFFFu); memcpy(h + 8, "WAVEfmt ", 8);
    put32le(h + 16, 16); put16le(h + 20, 1); put16le(h + 22, 2); put32le(h + 24, rate); put32le(h + 28, rate * BPF);
    put16le(h + 32, BPF); put16le(h + 34, 24); memcpy(h + 36, "data", 4); put32le(h + 40, 0xFFFFFFFFu);
}
static void http_reply(int c, int code, const char *text) {
    char h[160];
    const int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", code, text);
    send_all(c, h, (size_t)n);
}

/* A renderer asking for a part of the stream: Range: bytes=N- (any case). MPD's curl input takes any 2xx answer to a
 * Range as the bytes from N: its decoder (ffmpeg's WAV demuxer, which looks at the end then seeks to byte 44 while it
 * opens the stream) was given 200, a new WAV header and the live music, read as audio from byte 44: RIFF a click near
 * full scale, then every 24-bit frame 2 bytes off (44 is not a multiple of 6), noise near full scale. 0: no Range (or
 * from 0); 1: from *from; 2: past any end (a suffix bytes=-N, or N at RANGE_END or beyond: ffmpeg's look at the end
 * asks for 9223372036854775807-). */
#define RANGE_END (1ULL << 40)
static int http_range(const char *req, unsigned long long *from) {
    const char *e = strstr(req, "\r\n\r\n");
    for (const char *p = req; (p = strcasestr_(p, "\r\nrange:")) && (!e || p < e); p += 8) {
        const char *v = p + 8;
        while (*v == ' ' || *v == '\t') v++;
        if (strncasecmp(v, "bytes", 5)) return 0;
        v += 5;
        while (*v == ' ' || *v == '\t') v++;
        if (*v++ != '=') return 0;
        while (*v == ' ' || *v == '\t') v++;
        if (*v == '-') return 2;                      /* its last bytes: a stream with no end has none */
        if (*v < '0' || *v > '9') return 0;
        char *w;
        const unsigned long long n = strtoull(v, &w, 10);   /* past 2^64: ULLONG_MAX */
        v = w;
        while (*v == ' ' || *v == '\t') v++;
        if (*v != '-') return 0;
        *from = n;
        return !n ? 0 : n >= RANGE_END ? 2 : 1;
    }
    return 0;
}
static void *http_main(void *arg) {
    (void)arg;
    for (;;) {
        struct sockaddr_in pa;
        socklen_t pal = sizeof pa;
        int c = accept(http_fd, (struct sockaddr *)&pa, &pal);
        if (c < 0) { usleep(100000); continue; }
        timeouts(c, 2000);
        char req[4096];
        size_t got = 0;
        /* the request's head, 2 s in all (08/10, review: 2 s for each recv held this one thread for hours on a client
         * sending a byte every 1.9 s, the receiver's fetch waiting behind it) */
        const double t_end = mono() + 2.0;
        while (got < sizeof req - 1) {
            const int left = (int)((t_end - mono()) * 1000);
            struct pollfd p = { c, POLLIN, 0 };
            if (left <= 0 || poll(&p, 1, left) <= 0) break;
            ssize_t k = recv(c, req + got, sizeof req - 1 - got, 0);
            if (k <= 0) break;
            got += (size_t)k; req[got] = 0;
            if (strstr(req, "\r\n\r\n")) break;
        }
        req[got] = 0;
        char method[8] = "", path[256] = "";
        unsigned id = 0;
        if (sscanf(req, "%7s %255s", method, path) == 2 && (!strcmp(method, "GET") || !strcmp(method, "HEAD")) && np_http_cover(c, method, path)) { close(c); continue; }
        if (sscanf(req, "%7s %255s", method, path) != 2 || (strcmp(method, "GET") && strcmp(method, "HEAD")) ||
            sscanf(path, "/stream/%u.wav", &id) != 1) { http_reply(c, 404, "Not Found"); close(c); continue; }
        pthread_mutex_lock(&slk);
        /* an old stream (a renderer asking again for the last one), or this one after its end (08/10, review: a renderer
         * retrying on the early end of file was given a connection nothing was ever sent on, nor closed) */
        if (!S.on || S.ended || id != S.id) {
            pthread_mutex_unlock(&slk);
            http_reply(c, 404, "Not Found"); close(c);
            continue;
        }

        {
            char peer[INET_ADDRSTRLEN] = "";
            inet_ntop(AF_INET, &pa.sin_addr, peer, sizeof peer);
            pthread_mutex_lock(&rlk);
            const int theirs = R[S.ri].used && !strcmp(R[S.ri].addr, peer);
            pthread_mutex_unlock(&rlk);
            if (!theirs) {
                static double t_said = -1e9;
                if (mono() - t_said > 60) { t_said = mono(); nlog("stream %u asked for by %s, not the receiver: refused", id, peer); }
                pthread_mutex_unlock(&slk);
                http_reply(c, 403, "Forbidden"); close(c);
                continue;
            }
        }
        /* No Accept-Ranges header, not even "none": MPD 0.21 to 0.24 (behind upmpdcli's renderers; its
         * CurlInputPlugin, OnHeaders) takes the header being there as seekable, whatever it says, and its ffmpeg decoder
         * then seeks while it opens the stream (http_range above). That the stream cannot be sought is said by
         * DLNA.ORG_OP=00, transferMode Streaming and no Content-Length. The bench's MPD 0.24.4 without it: one fetch,
         * no seek, the music decoded as it was sent. */
        static const char HEAD_REST[] = "Content-Type: audio/wav\r\nServer: audiolaz-roomd\r\nCache-Control: no-cache\r\n"
            "Connection: close\r\ntransferMode.dlna.org: Streaming\r\n"
            "contentFeatures.dlna.org: DLNA.ORG_OP=00;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000\r\n\r\n";
        /* the WAV header to a renderer only (08/10, review): a Squeezebox player takes the format strm s gave it and does
         * not read a header then, it plays it as audio: squeezelite 2.0 played our 44 bytes, and every sample after them
         * came out 2 bytes off (noise), the bit-perfect test's capture */
        uint8_t wav[44];
        wav_header(wav, S.rate);
        const int hdr = S.kind == NK_UPNP;
        /* A renderer asking for a part anyway (a Range, http_range) is never given a stream out of its frames:
         * from byte N < 44, the WAV header's rest, then the music; from N ≥ 44, as many zero bytes as bring it to the
         * next frame of its grid (44 + a whole number of frames), then whole frames: what it decodes from N is the music,
         * as it believes. Past any end (ffmpeg's look at the end): 206 and nothing, the stream left where it plays (the
         * bench: MPD then seeks back to 44 and plays it right). Never 416: MPD then ends the song (the renderer STOPPED,
         * the stream lost). 206 Partial Content, its Content-Range open (no length: a live stream). Its WAV streams
         * only (a Squeezebox player, the AirPlay bridge: no header, never a Range). */
        unsigned long long from = 0;
        const int rg = hdr && !strcmp(method, "GET") ? http_range(req, &from) : 0;
        char head[sizeof HEAD_REST + 160];
        const int hl = rg == 2 ? snprintf(head, sizeof head, "HTTP/1.1 206 Partial Content\r\nContent-Type: audio/wav\r\nServer: audiolaz-roomd\r\n"
                                                                    "Content-Length: 0\r\nConnection: close\r\n\r\n")
                     : rg == 1 ? snprintf(head, sizeof head, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %llu-%llu/*\r\n%s", from, from + RANGE_END - 1, HEAD_REST)
                     : snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\n%s", HEAD_REST);
        static unsigned said_end, said_from;          /* each said once per stream (MPD asks twice from 44) */
        if (rg == 2 && said_end != S.id) { said_end = S.id; nlog("stream %u: the receiver looked for its end (Range): answered empty (206), the stream left where it plays", S.id); }
        if (rg == 1 && said_from != S.id) { said_from = S.id; nlog("stream %u fetched again from byte %llu (Range): answered from there, its frames where the receiver counts them", S.id, from); }
        uint8_t pre[44];                              /* rg 1: what goes before the music (the header's rest, or zeros) */
        size_t pl = 0;
        if (rg == 1 && from < sizeof wav) { pl = sizeof wav - (size_t)from; memcpy(pre, wav + from, pl); }
        else if (rg == 1) { pl = (size_t)((S.bpf - (from - sizeof wav) % (unsigned)S.bpf) % (unsigned)S.bpf); memset(pre, 0, pl); }
        if (send_all(c, head, (size_t)hl) || (hdr && !strcmp(method, "GET") && !rg && send_all(c, wav, sizeof wav)) || (pl && send_all(c, pre, pl))) { pthread_mutex_unlock(&slk); close(c); continue; }
        if (!strcmp(method, "HEAD") || rg == 2) { pthread_mutex_unlock(&slk); close(c); continue; }
        /* A small send buffer (Linux doubles it: 128 KB, half a second at 44.1 kHz): the push-back is the receiver's. */
        int sb = 64 * 1024, one = 1;
        setsockopt(c, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb);
        /* no Nagle: our writes are a few KB each; held for the receiver's delayed ACK, a small window crawled (08/10, the
         * fake squeezelite's 16 KB: 155 KB/s, half real time at 44.1 kHz) */
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        fcntl(c, F_SETFL, O_NONBLOCK);
        if (S.fd >= 0) {                             /* fetched again (a renderer probing first): the stream goes on there */
            nlog("stream %u fetched again by the receiver: going on in the new connection (%.1f s sent)", S.id, (double)S.bytes / (uint64_t)S.bpf / S.rate);
            close(S.fd);
            S.plen = S.poff = 0;                     /* a part of a frame would shift every sample after it */
        }
        S.fd = c;
        S.t_take = mono();
        pthread_cond_broadcast(&scv);
        pthread_mutex_unlock(&slk);
    }
    return NULL;
}

/* ── SlimProto: Squeezebox players connecting to us ─────────────────────────────────────────────────── */
static int slim_fd = -1;

#define RELAY_FDS 1024
static struct in_addr relay_local_v[RELAY_FDS], relay_peer_v[RELAY_FDS];
static char relay_has[RELAY_FDS];                     /* fd: a lent connection (its addresses above) */
static uint8_t helo_raw[NRCV][600];                   /* each receiver's last HELO, as it said it */
static uint32_t helo_len[NRCV];
static char relay_name[64];                           /* a voie: the player's socket's name ("" none) */
static int slim_send(int fd, const char *op, const void *p, size_t n) {
    uint8_t h[6];
    put16be(h, (uint16_t)(n + 4)); memcpy(h + 2, op, 4);
    pthread_mutex_lock(&sendlk);
    const int r = send_all(fd, h, 6) || (n && send_all(fd, p, n)) ? -1 : 0;
    pthread_mutex_unlock(&sendlk);
    return r;
}
static char rate_code(unsigned r) {
    static const unsigned RATES[] = { 11025, 22050, 32000, 44100, 48000, 8000, 12000, 16000, 24000, 96000, 88200, 176400, 192000, 352800, 384000 };
    for (unsigned i = 0; i < sizeof RATES / sizeof *RATES; i++) if (RATES[i] == r) return (char)('0' + i);
    return '?';                                       /* its WAV header says */
}
/* strm: s (play the URL in hdr; autostart 1), p, u, q, t (arg: the timestamp t's STAT echoes, else 0). */
static int strm_send(int fd, char cmd, uint32_t arg, unsigned rate, struct in_addr ip, const char *hdr, int bits16) {
    uint8_t p[24 + 256];
    memset(p, 0, 24);
    p[0] = (uint8_t)cmd; p[1] = '1'; p[2] = 'p'; p[3] = bits16 ? '1' : '2'; p[4] = (uint8_t)rate_code(rate); p[5] = '2'; p[6] = '1';
    /* KB read before it starts: a fifth of a second at 44.1 kHz; tenths of a second decoded before its output starts.
     * An AirPlay video play (09/10, netout_low_latency): 8 KB and a tenth, so that it starts on the little the player
     * lets it hold then (roomd.c: what it holds unplayed is held to a few tenths, its latency the picture waits for);
     * with 5 tenths it would never start. */
    p[7] = low_lat ? 8 : 32;
    p[8] = '0'; p[10] = '0';
    p[12] = low_lat ? 1 : 5;
    put32be(p + 14, arg);
    put16be(p + 18, (uint16_t)http_port_);
    memcpy(p + 20, &ip.s_addr, 4);
    size_t n = 24;
    if (hdr) { const size_t hl = strlen(hdr) < 256 ? strlen(hdr) : 255; memcpy(p + 24, hdr, hl); n += hl; }
    return slim_send(fd, "strm", p, n);
}
static void audg_unity(int fd) {                      /* their volume at 100 %: the samples untouched */
    uint8_t p[18];
    put32be(p, 0x10000); put32be(p + 4, 0x10000); p[8] = 1; p[9] = 255; put32be(p + 10, 0x10000); put32be(p + 14, 0x10000);
    slim_send(fd, "audg", p, sizeof p);
}

static void audg_bridge(int fd) {
    uint8_t p[18];
    put32be(p, 0x10000); put32be(p + 4, 0x10000); p[8] = 0; p[9] = 255; put32be(p + 10, 0x10000); put32be(p + 14, 0x10000);
    slim_send(fd, "audg", p, sizeof p);
}
/* Under rlk: the Squeezebox player's connection for a message sent after rlk is let go, dup'd (08/10, review: the
 * number copied was closed under us when the player left or came again, and soon another connection's, a stream's
 * among them: our strm in its PCM, every sample after it 2 bytes off); the caller closes it. −1: no player there. */
static int slim_dup(int ri, struct in_addr *l) {
    if (ri < 0 || !R[ri].used || !SLIM(R[ri].kind) || R[ri].fd < 0) return -1;
    if (l) *l = R[ri].local;
    return dup(R[ri].fd);
}
static uint32_t jiffies(void) { return (uint32_t)(mono() * 1000); }
static void receiver_gone(int ri, const char *what) {
    pthread_mutex_lock(&slk);
    if (S.on && S.ri == ri && !S.lost) { S.lost = 1; S.gone = 1; set_why("%s", what); }
    if (F.on && F.ri == ri) F.on = 0;
    pthread_mutex_unlock(&slk);
}
static void caps_value(const char *caps, const char *key, char *out, size_t len) {
    out[0] = 0;
    const size_t kl = strlen(key);
    for (const char *p = caps; p && *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1, *e = strchr(v, ',');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= len) n = len - 1;
            memcpy(out, v, n); out[n] = 0;
            return;
        }
        p = strchr(p, ',');
        if (p) p++;
    }
}
static void *slim_conn(void *arg) {
    const int fd = (int)(intptr_t)arg;
    timeouts(fd, 2000);
    struct timeval tv = { 30, 0 };                    /* it answers our strm t every 5 s at least */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int ri = -1;
    uint8_t h[8], *buf = malloc(65536);
    while (buf && !recv_all(fd, h, 8)) {
        const uint32_t n = get32be(h + 4);
        if (n > 65535 || (n && recv_all(fd, buf, n))) break;
        buf[n < 65535 ? n : 65535] = 0;
        {   /* lent to a voie: what it says goes there too (a new HELO: the loan ends, the voie asks again) */
            pthread_mutex_lock(&rlk);
            const int lf = ri >= 0 && R[ri].fd == fd ? R[ri].lent - 1 : -1;
            if (lf >= 0 && !memcmp(h, "HELO", 4)) { R[ri].lent = 0; shutdown(lf, SHUT_RDWR); }
            pthread_mutex_unlock(&rlk);
            if (lf >= 0 && memcmp(h, "HELO", 4) && (send_all(lf, h, 8) || (n && send_all(lf, buf, n)))) {
                pthread_mutex_lock(&rlk); if (R[ri].lent == lf + 1) { R[ri].lent = 0; shutdown(lf, SHUT_RDWR); } pthread_mutex_unlock(&rlk);
            }
        }
        if (!memcmp(h, "HELO", 4) && n >= 20) {
            char mac[24], caps[1024] = "", model[64], mname[64], maxr[16], nm[96];
            snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
            if (n > 36) snprintf(caps, sizeof caps, "%.*s", (int)(n - 36), (const char *)buf + 36);
            caps_value(caps, "Model", model, sizeof model);
            caps_value(caps, "ModelName", mname, sizeof mname);
            caps_value(caps, "MaxSampleRate", maxr, sizeof maxr);
            caps_value(caps, "Name", nm, sizeof nm);
            struct sockaddr_in me, peer;
            socklen_t ml = sizeof me, pl = sizeof peer;
            memset(&me, 0, sizeof me); memset(&peer, 0, sizeof peer);
            if (fd >= 0 && fd < RELAY_FDS && relay_has[fd]) { me.sin_addr = relay_local_v[fd]; peer.sin_addr = relay_peer_v[fd]; }   /* lent */
            else { getsockname(fd, (struct sockaddr *)&me, &ml); getpeername(fd, (struct sockaddr *)&peer, &pl); }
            pthread_mutex_lock(&rlk);
            /* another MAC on this connection: the slot it held goes (08/10, review: left holding our fd, closed at the
             * end and soon another connection's number, it was still listed and sent strm t every 5 s) */
            int dropped = -1;
            char was[96] = "";
            if (ri >= 0 && R[ri].used && R[ri].fd == fd && strcmp(R[ri].id, mac)) { snprintf(was, sizeof was, "%s", R[ri].id); R[ri].used = 0; R[ri].fd = -1; dropped = ri; ri = -1; }
            int k = -1;
            /* the bridge's players are AirPlay devices : their own kind, never sent audg */
            const int kind = !strcmp(mname, AP_MODEL) ? NK_AIRPLAY : NK_SQUEEZE;
            for (int i = 0; i < NRCV; i++) if (R[i].used && SLIM(R[i].kind) && !strcmp(R[i].id, mac)) k = i;
            if (k >= 0 && R[k].fd >= 0 && R[k].fd != fd) { shutdown(R[k].fd, SHUT_RDWR); }   /* it came again: the old one goes */
            if (k < 0) for (int i = 0; i < NRCV && k < 0; i++) if (!R[i].used) k = i;
            if (k >= 0) {
                char keep[96] = "";
                /* it came again: its counters kept (08/10, review: squeezelite reconnecting after a glitch plays on, and
                 * zeroed under the stream's snapshots they said nothing played, its end never heard, the next stream
                 * waiting TAIL_S); a new slot only is cleared */
                if (R[k].used) snprintf(keep, sizeof keep, "%s", R[k].name);
                else memset(&R[k], 0, sizeof R[k]);
                R[k].used = 1; R[k].kind = kind; R[k].fd = fd;
                snprintf(R[k].id, sizeof R[k].id, "%s", mac);
                snprintf(R[k].model, sizeof R[k].model, "%s", kind == NK_AIRPLAY ? "AirPlay" : model[0] ? model : mname);
                snprintf(R[k].name, sizeof R[k].name, "%s", nm[0] ? nm : keep[0] ? keep : mname[0] ? mname : model[0] ? model : "Squeezebox");
                R[k].max_rate = (unsigned)strtoul(maxr, NULL, 10);
                R[k].local = me.sin_addr;
                inet_ntop(AF_INET, &peer.sin_addr, R[k].addr, sizeof R[k].addr);
                R[k].seen = mono();
                helo_len[k] = n <= sizeof helo_raw[k] ? n : 0;   /* as it said it, for a voie it is lent to */
                if (helo_len[k]) memcpy(helo_raw[k], buf, n);
            }
            pthread_mutex_unlock(&rlk);
            if (dropped >= 0) { nlog("Squeezebox player %s: another one (%s) on its connection", was, mac); receiver_gone(dropped, "the Squeezebox player left"); }
            if (k < 0) { nlog("a %s (%s) refused: %d receivers already", kind == NK_AIRPLAY ? "AirPlay device" : "Squeezebox player", mac, NRCV); break; }
            ri = k;
            if (kind == NK_AIRPLAY) nlog("AirPlay device %s (the bridge, up to %s Hz) connected", mac, maxr[0] ? maxr : "?");
            else nlog("Squeezebox player %s (%s, %s, up to %s Hz) connected", mac, model[0] ? model : "?", R[k].addr, maxr[0] ? maxr : "?");
            const uint8_t q0 = 0;
            slim_send(fd, "setd", &q0, 1);           /* its name, please */
            const uint8_t on[2] = { 1, 1 };
            slim_send(fd, "aude", on, 2);
            if (kind == NK_SQUEEZE) audg_unity(fd);  /* never to AirPlay: 100 % there is the amplifier's own volume */
            else audg_bridge(fd);
            strm_send(fd, 't', jiffies(), 44100, me.sin_addr, NULL, 0);
            continue;
        }
        if (ri < 0) continue;
        if (!memcmp(h, "SETD", 4) && n >= 2 && buf[0] == 0) {
            pthread_mutex_lock(&rlk);
            snprintf(R[ri].name, sizeof R[ri].name, "%.*s", (int)(n - 1), (const char *)buf + 1);
            R[ri].name[strcspn(R[ri].name, "\r\n")] = 0;
            const int ap = R[ri].kind == NK_AIRPLAY;
            pthread_mutex_unlock(&rlk);
            if (ap) ap_names();                      /* its AirPlay name, when the bridge's config has it already */
            nlog("%s %s is \"%s\"", ap ? "AirPlay device" : "Squeezebox player", R[ri].id, R[ri].name);
        } else if (!memcmp(h, "STAT", 4) && n >= 4) {
            const char *ev = (const char *)buf;
            pthread_mutex_lock(&rlk);
            R[ri].seen = mono();
            /* its elapsed time; the AirPlay bridge's, until the device plays its first frame, wraps round (09/10, VM: its
             * frames played less its RAOP latency, unsigned: ~4.29e9 ms for a second or so; read as played, all that
             * was sent seemed played, nothing held, and the video play sent 20 s at once): none yet */
            if (n >= 47) { const uint32_t e = get32be(buf + 43); R[ri].elapsed_ms = e >= 0x80000000u ? 0 : (long)e; R[ri].t_stat = mono(); }
            const long el = R[ri].elapsed_ms;
            if (!memcmp(ev, "STMs", 4)) { R[ri].n_stms++; R[ri].stmu_at_stms = R[ri].n_stmu; R[ri].t_stms = mono(); }
            else if (!memcmp(ev, "STMc", 4)) { R[ri].n_stmc++; R[ri].stmf_at_stmc = R[ri].n_stmf; }
            else if (!memcmp(ev, "STMd", 4)) R[ri].n_stmd++;
            else if (!memcmp(ev, "STMu", 4)) { R[ri].n_stmu++; R[ri].t_stmu = mono(); }
            else if (!memcmp(ev, "STMf", 4)) R[ri].n_stmf++;
            else if (!memcmp(ev, "STMn", 4)) R[ri].n_stmn++;
            pthread_mutex_unlock(&rlk);
            if (memcmp(ev, "STMt", 4)) nlog("%s: %.4s, elapsed %.2f s%s", R[ri].name, ev, el / 1000.0, n >= 47 ? "" : " (a short STAT)");
        } else if (!memcmp(h, "DSCO", 4) && n >= 1 && buf[0]) nlog("%s: its stream connection ended (reason %d)", R[ri].name, buf[0]);
    }
    free(buf);
    if (ri >= 0) {
        pthread_mutex_lock(&rlk);
        const int mine = R[ri].fd == fd, ap = R[ri].kind == NK_AIRPLAY;
        char id[96], nm[96];
        snprintf(id, sizeof id, "%s", R[ri].id); snprintf(nm, sizeof nm, "%s", R[ri].name);
        if (mine) { if (R[ri].lent) shutdown(R[ri].lent - 1, SHUT_RDWR); R[ri].lent = 0; R[ri].used = 0; R[ri].fd = -1; }   /* its voie told */
        pthread_mutex_unlock(&rlk);
        if (mine) { nlog("%s %s (\"%s\") left", ap ? "AirPlay device" : "Squeezebox player", id, nm);
                    receiver_gone(ri, ap ? "the AirPlay device left (the bridge lost it, or stopped)" : "the Squeezebox player left"); }
    }
    if (fd >= 0 && fd < RELAY_FDS) relay_has[fd] = 0;
    close(fd);
    return NULL;
}

static int disc_ok = -1;                              /* −1 not tried (no bridge, --net off, no SlimProto port), 0 taken, 1 ours */
int netout_airplay_ready(void) { return disc_ok; }

static int ap_on = 0;
void netout_airplay_on(int on) { ap_on = on; }
/* The address the discovery is answered on (10/10, the Mac player): 0, the loopback (as before). */
static in_addr_t disc_ip = 0;
void netout_disc_addr(const char *ip) { struct in_addr a; disc_ip = ip && inet_pton(AF_INET, ip, &a) == 1 ? a.s_addr : 0; }
static void *disc_main(void *arg) {
    const int in = ((int *)arg)[0], out = ((int *)arg)[1];
    /* and where its CLI is (CLIP: the bridge asks it what plays, nowplay.h) and our HTTP port (JSON) */
    char reply[64];
    int rl = snprintf(reply, sizeof reply, "EVERS\x05" "9.0.0");
    char v[8];
    int vl = snprintf(v, sizeof v, "%d", http_port_);
    rl += snprintf(reply + rl, sizeof reply - (size_t)rl, "JSON%c%s", vl, v);
    if (np_cli > 0) { vl = snprintf(v, sizeof v, "%d", np_cli); rl += snprintf(reply + rl, sizeof reply - (size_t)rl, "CLIP%c%s", vl, v); }
    for (;;) {
        char b[256];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const ssize_t n = recvfrom(in, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) { if (n < 0 && errno != EINTR) usleep(100000); continue; }
        if (b[0] == 'e') sendto(out, reply, (size_t)rl, 0, (struct sockaddr *)&from, fl);
    }
    return NULL;
}
static void disc_start(int slim_port) {
    static int fds[2];
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = disc_ip ? disc_ip : htonl(INADDR_LOOPBACK) };
    int on = 1;
    fds[0] = socket(AF_INET, SOCK_DGRAM, 0); fds[1] = socket(AF_INET, SOCK_DGRAM, 0);
    if (fds[0] >= 0) setsockopt(fds[0], SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    a.sin_port = htons(3483);
    const int in_ok = fds[0] >= 0 && !bind(fds[0], (struct sockaddr *)&a, sizeof a);
    a.sin_port = htons((uint16_t)slim_port);
    const int out_ok = fds[1] >= 0 && !bind(fds[1], (struct sockaddr *)&a, sizeof a);
    pthread_t th;
    if (in_ok && out_ok) np_cli_start();              /* before the replies say its port */
    disc_ok = in_ok && out_ok && !pthread_create(&th, NULL, disc_main, fds);
    if (disc_ok) { pthread_detach(th); return; }
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    nlog("UDP %s:%d not ours (%s): no AirPlay outputs (their bridge would find Roon itself)", ip, in_ok ? slim_port : 3483, strerror(errno));
    if (fds[0] >= 0) close(fds[0]);
    if (fds[1] >= 0) close(fds[1]);
}
static void *slim_main(void *arg) {
    (void)arg;
    for (;;) {
        int c = accept(slim_fd, NULL, NULL);
        if (c < 0) { usleep(100000); continue; }
        pthread_t th;
        if (pthread_create(&th, NULL, slim_conn, (void *)(intptr_t)c)) { close(c); continue; }
        pthread_detach(th);
    }
    return NULL;
}

/* ── the loan of a SlimProto receiver to a voie (above, at slim_fd) ── */
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0                                 /* macOS: no voies there (abstract sockets are Linux's) */
#endif
static int relay_addr(const char *name, struct sockaddr_un *a, socklen_t *al) {
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    const size_t n = strlen(name);
    if (!n || n + 1 >= sizeof a->sun_path) return -1;
    memcpy(a->sun_path + 1, name, n);                  /* abstract: nothing on the disk */
    *al = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
    return 0;
}
static void *relay_conn(void *arg) {                   /* the player: one voie's ask, then its loan */
    const int c = (int)(intptr_t)arg;
    char req[200];
    size_t k = 0;
    struct timeval tv = { 2, 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (k + 1 < sizeof req && recv(c, req + k, 1, 0) == 1 && req[k] != '\n') k++;
    req[k] = 0;
    const char *key;
    const int kind = spec_kind(req, &key);
    int ri = -1, pfd = -1;
    char why[120] = "", ok[120];
    struct in_addr l = { 0 }, pa = { 0 };
    pthread_mutex_lock(&slk);                          /* slk before rlk, never after */
    const int playing = S.on && !S.lost;
    const int sri = S.ri;
    pthread_mutex_unlock(&slk);
    pthread_mutex_lock(&rlk);
    if (!SLIM(kind)) snprintf(why, sizeof why, "not a Squeezebox player nor an AirPlay device");
    else if ((ri = find_rcv(kind, key)) < 0) snprintf(why, sizeof why, "not connected here");
    else if (R[ri].lent) snprintf(why, sizeof why, "lent to another voie already");
    else if (playing && sri == ri) snprintf(why, sizeof why, "the player plays to it");
    else if (!helo_len[ri] || (pfd = dup(R[ri].fd)) < 0) snprintf(why, sizeof why, "its HELO not known");
    else { R[ri].lent = c + 1; l = R[ri].local; inet_pton(AF_INET, R[ri].addr, &pa); }
    uint8_t helo[8 + 600];
    const uint32_t hn = ri >= 0 ? helo_len[ri] : 0;
    if (!why[0]) { memcpy(helo, "HELO", 4); put32be(helo + 4, hn); memcpy(helo + 8, helo_raw[ri], hn); }
    char nm[96] = "";
    if (ri >= 0) snprintf(nm, sizeof nm, "%s", R[ri].name);
    pthread_mutex_unlock(&rlk);
    if (why[0]) {
        snprintf(ok, sizeof ok, "NO %s\n", why);
        send_all(c, ok, strlen(ok));
        close(c);
        return NULL;
    }
    char ls[INET_ADDRSTRLEN], ps[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &l, ls, sizeof ls); inet_ntop(AF_INET, &pa, ps, sizeof ps);
    snprintf(ok, sizeof ok, "OK %s %s\n", ls, ps);
    nlog("%s lent to a voie of this player", nm);
    if (send_all(c, ok, strlen(ok)) == 0 && send_all(c, helo, 8 + hn) == 0) {
        struct timeval t0 = { 0, 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &t0, sizeof t0);
        uint8_t *m = malloc(2 + 65536);
        while (m) {                                    /* the voie's messages, whole, between the player's own */
            if (recv_all(c, m, 2)) break;
            const size_t L = (size_t)m[0] << 8 | m[1];
            if (L < 4 || recv_all(c, m + 2, L)) break;
            pthread_mutex_lock(&sendlk);
            const int bad = send_all(pfd, m, 2 + L);
            pthread_mutex_unlock(&sendlk);
            if (bad) break;
        }
        free(m);
    }
    pthread_mutex_lock(&rlk);
    if (ri >= 0 && R[ri].lent == c + 1) R[ri].lent = 0;
    pthread_mutex_unlock(&rlk);
    nlog("%s: the voie's loan over", nm);
    close(pfd);
    close(c);
    return NULL;
}
static void *relay_main(void *arg) {
    const int srv = (int)(intptr_t)arg;
    for (;;) {
        const int c = accept(srv, NULL, NULL);
        if (c < 0) { usleep(100000); continue; }
        pthread_t th;
        if (pthread_create(&th, NULL, relay_conn, (void *)(intptr_t)c)) { close(c); continue; }
        pthread_detach(th);
    }
    return NULL;
}
int netout_relay_serve(const char *name) {
    struct sockaddr_un a;
    socklen_t al;
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0 || relay_addr(name, &a, &al) || bind(s, (struct sockaddr *)&a, al) || listen(s, 8)) { if (s >= 0) close(s); return -1; }
    pthread_t th;
    if (pthread_create(&th, NULL, relay_main, (void *)(intptr_t)s)) { close(s); return -1; }
    pthread_detach(th);
    return 0;
}
void netout_relay_use(const char *name) { snprintf(relay_name, sizeof relay_name, "%s", name ? name : ""); }
/* a voie: the receiver spec names borrowed from the player (its connection then this netout's own: slim_conn); 0 once
 * it is listed here, −1 (set_why) */
static int relay_fetch(const char *spec) {
    struct sockaddr_un a;
    socklen_t al;
    const int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c < 0 || relay_addr(relay_name, &a, &al) || connect(c, (struct sockaddr *)&a, al)) { if (c >= 0) close(c); set_why("%s: the player that started this voie does not answer", spec); return -1; }
    char line[200];
    snprintf(line, sizeof line, "%s\n", spec);
    struct timeval tv = { 3, 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    size_t k = 0;
    if (send_all(c, line, strlen(line)) == 0) while (k + 1 < sizeof line && recv(c, line + k, 1, 0) == 1 && line[k] != '\n') k++;
    line[k] = 0;
    char ls[48] = "", ps[48] = "";
    if (strncmp(line, "OK ", 3) || sscanf(line + 3, "%47s %47s", ls, ps) != 2 || c >= RELAY_FDS) {
        set_why("%s: %s", spec, !strncmp(line, "NO ", 3) ? line + 3 : "not lent by the player");
        close(c);
        return -1;
    }
    struct timeval t0 = { 0, 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &t0, sizeof t0);
    inet_pton(AF_INET, ls, &relay_local_v[c]); inet_pton(AF_INET, ps, &relay_peer_v[c]);
    relay_has[c] = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, slim_conn, (void *)(intptr_t)c)) { relay_has[c] = 0; close(c); set_why("%s: no thread", spec); return -1; }
    pthread_detach(th);
    const char *key;
    const int kind = spec_kind(spec, &key);
    for (int i = 0; i < 100; i++) {                   /* its HELO taken (a second at most) */
        pthread_mutex_lock(&rlk);
        const int ri = find_rcv(kind, key);
        pthread_mutex_unlock(&rlk);
        if (ri >= 0) { nlog("%s: borrowed from the player that started this voie", spec); return 0; }
        usleep(10000);
    }
    set_why("%s: lent, its HELO not taken", spec);
    return -1;
}

/* ── UPnP: renderers found, their AVTransport driven ─────────────────────────────────────────────────── */
/* svc 0: its AVTransport; 1 : its RenderingControl */
static int soap_svc(int ri, int svc, const char *action, const char *args, char *out, size_t outlen) {
    char url[320], type[96];
    pthread_mutex_lock(&rlk);
    snprintf(url, sizeof url, "%s", svc ? R[ri].rc_url : R[ri].av_url); snprintf(type, sizeof type, "%s", svc ? R[ri].rc_type : R[ri].av_type);
    const int ok = R[ri].used && R[ri].kind == NK_UPNP && url[0];
    pthread_mutex_unlock(&rlk);
    if (!ok) return -1;
    const size_t bl = strlen(args) + 2 * strlen(action) + strlen(type) + 400;
    char *body = malloc(bl), extra[256];
    if (!body) return -1;
    snprintf(body, bl, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
             action, type, args, action);
    snprintf(extra, sizeof extra, "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPACTION: \"%s#%s\"\r\n", type, action);
    char resp[8192];
    const int st = http_req(url, "POST", extra, body, resp, sizeof resp);
    free(body);
    if (out && outlen) snprintf(out, outlen, "%s", resp);
    if (st != 200) {
        char code[32] = "", desc[96] = "";
        xml_text(resp, "errorCode", code, sizeof code); xml_text(resp, "errorDescription", desc, sizeof desc);
        if (strcmp(action, "GetTransportInfo") && strcmp(action, "GetPositionInfo") && strcmp(action, "GetVolume") && strcmp(action, "GetMute"))
            nlog("UPnP %s: %s%s%s%s%s%s", action, st < 0 ? "no answer" : "refused", code[0] ? " (" : "", code, desc[0] ? ", " : "", desc, code[0] ? ")" : "");
        return -1;
    }
    return 0;
}
static int soap(int ri, const char *action, const char *args, char *out, size_t outlen) { return soap_svc(ri, 0, action, args, out, outlen); }
static int transport_state(const char *s) {
    return !strcmp(s, "PLAYING") ? TS_PLAYING : !strcmp(s, "PAUSED_PLAYBACK") ? TS_PAUSED : !strcmp(s, "STOPPED") ? TS_STOPPED
         : !strcmp(s, "TRANSITIONING") ? TS_TRANSITIONING : !strcmp(s, "NO_MEDIA_PRESENT") ? TS_NO_MEDIA : TS_UNKNOWN;
}
static int upnp_state(int ri) {
    char r[8192], s[32];
    if (soap(ri, "GetTransportInfo", "<InstanceID>0</InstanceID>", r, sizeof r) || !xml_text(r, "CurrentTransportState", s, sizeof s)) return -1;
    return transport_state(s);
}
static double hms(const char *t) {                     /* H:MM:SS[.mmm] → seconds; −1: none */
    unsigned h, m;
    double s;
    return sscanf(t, "%u:%u:%lf", &h, &m, &s) == 3 ? h * 3600.0 + m * 60.0 + s : -1;
}
/* A description fetched: the renderer kept (its UDN, name, AVTransport's control URL), or refreshed. */

static unsigned upnp_sink_rate(const char *url, const char *type, int *dsd) {
    char body[600], extra[256], *resp = malloc(65536);
    if (!resp) return 0;
    snprintf(body, sizeof body, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:GetProtocolInfo xmlns:u=\"%s\"></u:GetProtocolInfo></s:Body></s:Envelope>\r\n", type);
    snprintf(extra, sizeof extra, "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPACTION: \"%s#GetProtocolInfo\"\r\n", type);
    unsigned m = 0;
    if (http_req(url, "POST", extra, body, resp, 65536) == 200) {
        char *sink = strstr(resp, "Sink>");
        if (sink && dsd) *dsd = strstr(sink, "audio/dsf") || strstr(sink, "audio/x-dsf") || strstr(sink, "audio/dff") || strstr(sink, "audio/x-dff") || strstr(sink, "audio/dsd") || strstr(sink, "audio/x-dsd");
        if (sink) for (const char *p = sink; (p = strstr(p, "rate=")); p += 5) {

            const char *st = p;
            while (st > sink && *st != ',' && *st != '>') st--;
            char ent[160];
            snprintf(ent, sizeof ent, "%.*s", (int)(p - st) < 150 ? (int)(p - st) : 150, st);
            if (!strstr(ent, "audio/wav") && !strstr(ent, "audio/x-wav") && !strstr(ent, "audio/wave")) continue;
            const unsigned r = (unsigned)strtoul(p + 5, NULL, 10);
            if (r >= 8000 && r <= 1536000 && r > m) m = r;
        }
    }
    free(resp);
    return m;
}
static void upnp_add(const char *loc, const char *ip) {
    char *d = malloc(65536);
    if (!d) return;
    if (http_req(loc, "GET", NULL, NULL, d, 65536) != 200) { free(d); return; }
    char udn[96] = "", name[96] = "", model[64] = "", base[256] = "", av[320] = "", avt[96] = "", maker[64] = "";
    xml_text(d, "UDN", udn, sizeof udn);
    xml_text(d, "manufacturer", maker, sizeof maker);
    xml_text(d, "friendlyName", name, sizeof name);
    xml_text(d, "modelName", model, sizeof model);
    xml_text(d, "URLBase", base, sizeof base);
    char rc[320] = "", rct[96] = "", cm[320] = "", cmt[96] = "";
    for (const char *p = d, *e; (p = strstr(p, "<service>")) && (e = strstr(p, "</service>")); p = e) {   /* each service */
        char t[96] = "", u[256] = "", blk[2048];
        size_t bl = (size_t)(e - p);
        if (bl >= sizeof blk) bl = sizeof blk - 1;
        memcpy(blk, p, bl); blk[bl] = 0;
        if (!xml_text(blk, "serviceType", t, sizeof t) || !xml_text(blk, "controlURL", u, sizeof u)) continue;
        const int is_av = strstr(t, ":service:AVTransport:") && !av[0], is_rc = strstr(t, ":service:RenderingControl:") && !rc[0];
        const int is_cm = strstr(t, ":service:ConnectionManager:") && !cm[0];   /* its formats (upnp_sink_rate) */
        if (!is_av && !is_rc && !is_cm) continue;      /* RenderingControl too: the sender's volume */
        char *out = is_av ? av : is_rc ? rc : cm;
        snprintf(is_av ? avt : is_rc ? rct : cmt, sizeof avt, "%s", t);
        if (!strncasecmp(u, "http://", 7)) snprintf(out, sizeof av, "%s", u);
        else {
            char host[64]; const char *path; int port;
            const char *b = base[0] ? base : loc;
            if (parse_url(b, host, sizeof host, &port, &path)) break;
            if (u[0] == '/') snprintf(out, sizeof av, "http://%s:%d%s", host, port, u);
            else {                                    /* relative to the base's folder */
                char dir[256];
                snprintf(dir, sizeof dir, "%s", path);
                char *sl = strrchr(dir, '/');
                if (sl) sl[1] = 0;
                snprintf(out, sizeof av, "http://%.63s:%d%.120s%.120s", host, port, dir, u);
            }
        }
    }
    free(d);
    if (!udn[0] || !av[0]) return;                    /* no AVTransport: not a renderer we can drive */
    if (self_udn[0] && !strcasecmp(udn, self_udn)) return;   /* the player's own UPnP input: never an output */
    pthread_mutex_lock(&rlk);
    int k = -1;
    for (int i = 0; i < NRCV; i++) if (R[i].used && R[i].kind == NK_UPNP && !strcmp(R[i].id, udn)) k = i;
    const int fresh = k < 0;
    if (k < 0) for (int i = 0; i < NRCV && k < 0; i++) if (!R[i].used) k = i;
    if (k >= 0) {
        if (fresh) { memset(&R[k], 0, sizeof R[k]); R[k].fd = -1; }
        snprintf(R[k].maker, sizeof R[k].maker, "%s", maker);   /* */
        R[k].used = 1; R[k].kind = NK_UPNP; R[k].seen = mono();
        snprintf(R[k].id, sizeof R[k].id, "%s", udn);
        snprintf(R[k].name, sizeof R[k].name, "%s", name[0] ? name : udn);
        snprintf(R[k].model, sizeof R[k].model, "%s", model);
        snprintf(R[k].addr, sizeof R[k].addr, "%.47s", ip);
        snprintf(R[k].loc, sizeof R[k].loc, "%.255s", loc);
        snprintf(R[k].av_url, sizeof R[k].av_url, "%s", av);
        snprintf(R[k].av_type, sizeof R[k].av_type, "%s", avt);
        snprintf(R[k].rc_url, sizeof R[k].rc_url, "%s", rc);
        snprintf(R[k].rc_type, sizeof R[k].rc_type, "%s", rct);
    }
    pthread_mutex_unlock(&rlk);
    if (fresh && k >= 0) nlog("UPnP renderer \"%s\" (%s, %s) found: %s", name, model[0] ? model : "?", udn, av);
    if (fresh && k >= 0 && cm[0]) {
        int dsd = 0;
        const unsigned m = upnp_sink_rate(cm, cmt, &dsd);
        pthread_mutex_lock(&rlk);
        if (R[k].used && !strcmp(R[k].id, udn)) R[k].max_rate = m;
        pthread_mutex_unlock(&rlk);
        if (m) nlog("UPnP renderer \"%s\": WAV up to %u Hz (its ProtocolInfo): upsampling up to that", name, m);
        else nlog("UPnP renderer \"%s\": no WAV rate in its ProtocolInfo (a rate refused: a level down)%s", name, dsd ? "; DSD files listed (DSF/DFF)" : "");
    }
}
static void ssdp_search(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    unsigned char ttl = 2;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(1900) };
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    static const char MS[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                             "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\nUSER-AGENT: Linux UPnP/1.0 audiolaz-roomd\r\n\r\n";
    /* (18/10, the box: the CXNv2 never seen again once on, the Yamahas were) its AVTransport asked for too: a renderer
     * answering only for its services is found that way */
    static const char MS2[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                              "ST: urn:schemas-upnp-org:service:AVTransport:1\r\nUSER-AGENT: Linux UPnP/1.0 audiolaz-roomd\r\n\r\n";
    for (int k = 0; k < 2; k++) { sendto(s, MS, sizeof MS - 1, 0, (struct sockaddr *)&to, sizeof to); sendto(s, MS2, sizeof MS2 - 1, 0, (struct sockaddr *)&to, sizeof to); }
    char locs[64][256], ips[64][48];   /* 64 (16 before: a house's TVs and boxes answering first kept the others out) */
    int nl = 0;
    const double t0 = mono();
    while (mono() - t0 < 3.0) {
        struct pollfd p = { s, POLLIN, 0 };
        if (poll(&p, 1, 200) <= 0) continue;
        char b[2048];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t r = recvfrom(s, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) continue;
        b[r] = 0;
        char loc[256] = "";
        for (char *line = b; line && *line; ) {
            char *e = strstr(line, "\r\n");
            if (e) *e = 0;
            if (!strncasecmp(line, "LOCATION:", 9)) { const char *v = line + 9; while (*v == ' ') v++; snprintf(loc, sizeof loc, "%s", v); }
            line = e ? e + 2 : NULL;
        }
        if (!loc[0]) continue;
        int dup = 0;
        for (int i = 0; i < nl; i++) if (!strcmp(locs[i], loc)) dup = 1;
        if (!dup && nl < 64) { snprintf(locs[nl], sizeof locs[nl], "%s", loc); inet_ntop(AF_INET, &from.sin_addr, ips[nl], sizeof ips[nl]); nl++; }
    }
    close(s);
    for (int i = 0; i < nl; i++) upnp_add(locs[i], ips[i]);
    pthread_mutex_lock(&slk);
    const int playing = S.on ? S.ri : -1;
    pthread_mutex_unlock(&slk);
    pthread_mutex_lock(&rlk);                          /* not seen for a while: forgotten (never the one playing) */
    for (int i = 0; i < NRCV; i++)
        if (R[i].used && R[i].kind == NK_UPNP && mono() - R[i].seen > FORGET_S && i != playing) { nlog("UPnP renderer \"%s\" no longer seen", R[i].name); R[i].used = 0; }
    pthread_mutex_unlock(&rlk);
}
/* a renderer by its description's URL, without a search: its announces (NOTIFY ssdp:alive, below), a voie's
 * output known from before (netout_upnp_probe). upnp_add reads the description again; at most once in 20 s a URL. */
static void upnp_probe_url(const char *loc) {
    static char seen[16][256];
    static double at[16];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    int k = -1, old = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < 16 && k < 0; i++) if (!strcmp(seen[i], loc)) k = i;
    if (k >= 0 && mono() - at[k] < 20.0) { pthread_mutex_unlock(&mu); return; }
    if (k < 0) { for (int i = 1; i < 16; i++) if (at[i] < at[old]) old = i; k = old; snprintf(seen[k], sizeof seen[k], "%s", loc); }
    at[k] = mono();
    pthread_mutex_unlock(&mu);
    char host[64]; const char *path; int port;
    if (parse_url(loc, host, sizeof host, &port, &path)) return;
    upnp_add(loc, host);
}
static void *upnp_probe_main(void *arg) { char *loc = arg; upnp_probe_url(loc); free(loc); return NULL; }
void netout_upnp_probe(const char *loc) {
    if (!loc || strncasecmp(loc, "http://", 7)) return;
    char *c = strdup(loc);
    pthread_t th;
    if (c && !pthread_create(&th, NULL, upnp_probe_main, c)) pthread_detach(th); else free(c);
}
void netout_upnp_loc(const char *spec, char *out, size_t len) {   /* a renderer's description's URL, "" none */
    out[0] = 0;
    const char *key;
    if (!spec || spec_kind(spec, &key) != NK_UPNP) return;
    pthread_mutex_lock(&rlk);
    const int i = find_rcv(NK_UPNP, key);
    if (i >= 0) snprintf(out, len, "%s", R[i].loc);
    pthread_mutex_unlock(&rlk);
}
/* the announces: a renderer switched on says so (NOTIFY ssdp:alive, its LOCATION) without waiting for our search */
static void *ssdp_notify_main(void *arg) {
    (void)arg;
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return NULL;
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
#endif
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(1900), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a)) { close(s); return NULL; }
    struct ifaddrs *ifs = NULL, *f;                  /* the group joined on every interface */
    if (!getifaddrs(&ifs)) {
        for (f = ifs; f; f = f->ifa_next) {
            if (!f->ifa_addr || f->ifa_addr->sa_family != AF_INET || (f->ifa_flags & IFF_LOOPBACK)) continue;
            struct ip_mreq m;
            inet_pton(AF_INET, "239.255.255.250", &m.imr_multiaddr);
            m.imr_interface = ((struct sockaddr_in *)f->ifa_addr)->sin_addr;
            setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);
        }
        freeifaddrs(ifs);
    }
    for (;;) {
        char b[2048];
        const ssize_t r = recv(s, b, sizeof b - 1, 0);
        if (r <= 0) { usleep(100000); continue; }
        b[r] = 0;
        if (strncmp(b, "NOTIFY", 6) || !strcasestr_(b, "ssdp:alive") || !(strcasestr_(b, "MediaRenderer") || strcasestr_(b, "AVTransport"))) continue;
        char loc[256] = "";
        for (char *line = b; line && *line; ) {
            char *e = strstr(line, "\r\n");
            if (e) *e = 0;
            if (!strncasecmp(line, "LOCATION:", 9)) { const char *v = line + 9; while (*v == ' ') v++; snprintf(loc, sizeof loc, "%s", v); }
            line = e ? e + 2 : NULL;
        }
        if (!loc[0]) continue;
        int known = 0;
        pthread_mutex_lock(&rlk);
        for (int i = 0; i < NRCV; i++) if (R[i].used && R[i].kind == NK_UPNP && !strcmp(R[i].loc, loc)) { known = 1; R[i].seen = mono(); }
        pthread_mutex_unlock(&rlk);
        if (!known) upnp_probe_url(loc);
    }
    return NULL;
}
static volatile int search_soon = 0;           /* netout_search_soon: the next search in a second */
void netout_search_soon(void) { search_soon = 1; }
static void *upnp_main(void *arg) {
    (void)arg;
    pthread_t th;                                  /* the renderers' announces too */
    if (!pthread_create(&th, NULL, ssdp_notify_main, NULL)) pthread_detach(th);
    for (;;) {
        ssdp_search();
        for (int i = 0; i < (int)SEARCH_S && !search_soon; i++) sleep(1);
        search_soon = 0;
    }
    return NULL;
}

/* ── The watch: Squeezebox players kept awake and asked how far they played; the renderer's state ─────── */
static void *watch_main(void *arg) {
    (void)arg;
    double t_keep = 0, t_upnp = 0;
    for (;;) {
        usleep(250000);
        const double t = mono();
        pthread_mutex_lock(&slk);
        const int on = S.on, kind = S.kind, ri = S.ri, paused = S.paused, seen = S.seen_playing;
        pthread_mutex_unlock(&slk);
        /* the one playing: its STAT (elapsed) each round; every one of them every 5 s: a server that says
         * nothing for 35 s is dead to it. Their connections dup'd (slim_dup): ours until sent. */
        const int keep = t - t_keep >= 5.0;
        if (keep) { t_keep = t; ap_names(); }
        int fds[NRCV], nf = 0;
        struct in_addr loc[NRCV];
        pthread_mutex_lock(&rlk);
        for (int i = 0; i < NRCV; i++)
            if ((keep || (on && SLIM(kind) && ri == i)) && (fds[nf] = slim_dup(i, &loc[nf])) >= 0) nf++;
        pthread_mutex_unlock(&rlk);
        for (int i = 0; i < nf; i++) { strm_send(fds[i], 't', jiffies(), 0, loc[i], NULL, 0); close(fds[i]); }
        /* once a second; four times a second until it is seen playing (08/10, review: its start is when Roon is told the
         * track started, up to a whole second late otherwise: 16.4 or 17.3 s for the same 16 s in the fake's runs) */
        /* four times a second throughout for an AirPlay video play: its position's second seen turning within
         * a quarter of a second (what it holds unplayed is what the player lets it hold, a few tenths) */
        pthread_mutex_lock(&slk);
        const int low = S.low;
        pthread_mutex_unlock(&slk);
        if (on && kind == NK_UPNP && t - t_upnp >= (seen && !low ? 1.0 : 0.25)) {
            t_upnp = t;
            const int ts = upnp_state(ri);
            char r[8192], rel[32] = "";
            const int pos = !soap(ri, "GetPositionInfo", "<InstanceID>0</InstanceID>", r, sizeof r) && xml_text(r, "RelTime", rel, sizeof rel);
            char turi[400] = "";
            if (pos) xml_text(r, "TrackURI", turi, sizeof turi);
            int taken_now = 0;
            char tspec[160] = "";
            pthread_mutex_lock(&slk);
            if (S.on && S.ri == ri) {
                if (ts < 0) {
                    /* about 3 s of failed polls either way (polled faster before it plays) */
                    if (++S.fails >= (S.seen_playing ? 3 : 10) && !S.lost) { S.lost = 1; set_why("the UPnP renderer does not answer"); }
                } else {
                    S.fails = 0; S.tstate = ts;
                    if (ts == TS_PLAYING && !S.seen_playing) { S.seen_playing = 1; S.t_playing = mono(); }
                    const double h = pos ? hms(rel) : -1;

                    const double tn = mono();
                    if (h >= 0) {
                        if (h > 0 && h != S.rel) S.rel_moved = 1;
                        if (h != floor(h)) { S.rel = h; S.t_rel = tn; S.rel_whole = 0; }
                        else if (h != S.rel || !S.rel_whole) {
                            S.t_rel = S.rel_whole && h == S.rel + 1 && S.t_poll > 0 ? (S.t_poll + tn) / 2 : tn;
                            S.rel = h; S.rel_whole = 1;
                        }
                        S.t_poll = tn;
                        /* smoothed (netout_smooth_position): its position against the time since it plays */
                        /* from its second second on (gmediarender's first positions, read as it starts, are not yet
                         * its pace: the average went the wrong way, then took 20 s back at 50 ms a read) */
                        if (S.smooth && S.seen_playing && h >= 2 && !paused && ts == TS_PLAYING) {
                            const double obs = h + (h == floor(h) ? 0.5 : 0.0), base = tn - S.t_playing - S.paused_s;
                            double d = (obs - (base + S.corr)) * (S.n_corr < 4 ? 0.5 : 0.1);
                            if (S.n_corr >= 4) d = d > 0.05 ? 0.05 : d < -0.05 ? -0.05 : d;
                            S.corr += d; S.n_corr++;
                            if (S.n_corr == 10) S.corr0 = S.corr;
                            if (S.n_corr == 10 || S.n_corr % 60 == 0)  /* a line now and then: its offset, how far it moved */
                                nlog("UPnP position: %.1f s by its own, the time since it plays %.2f s: %+.0f ms apart (%+.0f ms since it settled)",
                                     h, base, S.corr * 1000, (S.corr - S.corr0) * 1000);
                        }
                    }
                    /* stopped under us (its own remote, or it gave up on the stream): lost, the play ends; after our end of
                     * file, it played it all */
                    /* its TrackURI ours, then another's: another sender took it (lost, never told to stop) */
                    if (turi[0]) {
                        char mine[48];
                        snprintf(mine, sizeof mine, "/stream/%u.wav", S.id);
                        if (strstr(turi, mine)) S.uri_ours = 1;
                        else if (S.uri_ours && !S.lost && !S.ended) {
                            S.lost = 1; S.taken = 1; taken_now = 1; snprintf(tspec, sizeof tspec, "%s", spec_now);
                            set_why("the UPnP renderer plays another sender's stream now");
                        }
                    }
                    if (S.seen_playing && !paused && !S.ended && (ts == TS_STOPPED || ts == TS_NO_MEDIA) && !S.lost) { S.lost = 1; set_why("the UPnP renderer stopped"); }
                }
            }
            pthread_mutex_unlock(&slk);
            if (taken_now) {
                char by[96] = "", rn[96] = "";
                if (netout_used_by(tspec, by, sizeof by) != 2) by[0] = 0;
                pthread_mutex_lock(&rlk);
                snprintf(rn, sizeof rn, "%s", R[ri].name);
                pthread_mutex_unlock(&rlk);
                pthread_mutex_lock(&clk);
                snprintf(TK.spec, sizeof TK.spec, "%s", tspec); snprintf(TK.rname, sizeof TK.rname, "%s", rn); snprintf(TK.by, sizeof TK.by, "%s", by);
                pthread_mutex_unlock(&clk);
                nlog("UPnP \"%s\" taken by another sender%s%s%s: let go", rn, by[0] ? " (the audiolaz player \"" : "", by, by[0] ? "\")" : "");
                claim_announce();
            }
        }
    }
    return NULL;
}

/* ── The pseudo-DAC ────────────────────────────────────────────────────────────────────────────────── */
/* What the chosen receiver holds unplayed at the least, whatever it is let hold (09/10, roomd.c's AirPlay video): the
 * AirPlay bridge's RAOP latency, its read_ahead (config.xml) plus the 11025 frames libraop adds (250 ms at 44.1 kHz:
 * raopcl_latency), the elapsed time it reports being what the device played; 0 for the others. ms. */
int netout_hold_floor_ms(void) {
    pthread_mutex_lock(&slk);
    char spec[160];
    snprintf(spec, sizeof spec, "%s", spec_now);
    pthread_mutex_unlock(&slk);
    return netout_hold_floor_ms_for(spec);
}

int netout_hold_floor_ms_for(const char *spec) {
    const char *key;
    if (!spec || spec_kind(spec, &key) != NK_AIRPLAY) return 0;
    ap_load();
    pthread_mutex_lock(&aplk);
    const int ms = (ap_ahead_ms > 250 ? ap_ahead_ms : 250) + 250;
    pthread_mutex_unlock(&aplk);
    return ms;
}
unsigned netout_max_rate(void) {
    const char *key;
    ap_names();
    pthread_mutex_lock(&slk);
    char spec[160];
    snprintf(spec, sizeof spec, "%s", spec_now);
    pthread_mutex_unlock(&slk);
    const int kind = spec_kind(spec, &key);
    unsigned m = 0;
    pthread_mutex_lock(&rlk);
    const int i = kind ? find_rcv(kind, key) : -1;
    if (i >= 0) m = R[i].max_rate;
    pthread_mutex_unlock(&rlk);
    return m;
}
/* The last stream's tail on the receiver now asked for another: let it finish (the Squeezebox player's STMd: it read it
 * all; the renderer: no longer playing), TAIL_S at most. */
static void wait_tail(int ri) {
    if (!F.on || F.ri != ri) { F.on = 0; return; }
    const double t0 = mono();
    int done = 0;
    while (!done && mono() - t0 < TAIL_S) {
        if (SLIM(F.kind)) {
            pthread_mutex_lock(&rlk);
            done = !R[ri].used || R[ri].fd < 0 || R[ri].n_stmd > F.stmd0 || R[ri].n_stmu > F.stmu0 || R[ri].n_stmf > F.stmf0;
            pthread_mutex_unlock(&rlk);
            if (!done) usleep(20000);
        } else {
            const int ts = upnp_state(ri);
            done = ts != TS_PLAYING && ts != TS_TRANSITIONING;
            if (!done) usleep(300000);
        }
    }
    nlog("the last stream %s on the receiver after %.1f s", done ? "finished" : "still not finished", mono() - t0);
    F.on = 0;
}
static int local_addr_to(const char *ip, struct in_addr *out) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(1900) }, me;
    socklen_t ml = sizeof me;
    if (s < 0 || inet_pton(AF_INET, ip, &a.sin_addr) != 1 || connect(s, (struct sockaddr *)&a, sizeof a) || getsockname(s, (struct sockaddr *)&me, &ml)) { if (s >= 0) close(s); return -1; }
    close(s);
    *out = me.sin_addr;
    return 0;
}
static void tell_stop(int kind, int ri) {
    if (SLIM(kind)) {
        struct in_addr l;
        pthread_mutex_lock(&rlk);
        const int fd = slim_dup(ri, &l);
        pthread_mutex_unlock(&rlk);
        if (fd >= 0) { strm_send(fd, 'q', 0, 0, l, NULL, 0); close(fd); }
    } else if (kind == NK_UPNP) soap(ri, "Stop", "<InstanceID>0</InstanceID>", NULL, 0);
}
/* the last netout_open that failed asked the receiver (it refused the stream or never fetched it), not one
 * turned away before (its backoff, not found, used by another player): only that says something of the rate */
static int last_asked = 0;
int netout_open_asked(void) { return last_asked; }

static int AZ = 0;                                     /* the stream playing is an audiolaz one */

static char az_known_path[700] = "";
void netout_state_dir(const char *dir) { if (dir && *dir) snprintf(az_known_path, sizeof az_known_path, "%s/link-known", dir); }
static int az_known_get(const char *id, char *loc, size_t ll) {
    if (!az_known_path[0] || !id || !*id) return -1;
    FILE *f = fopen(az_known_path, "r");
    char line[300]; int hit = -1;
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        if (!strcasecmp(line, id)) { snprintf(loc, ll, "%s", sp + 1); hit = 0; }
    }
    if (f) fclose(f);
    return hit;
}
static void az_known_put(const char *id, const char *loc) {
    char was[200] = "";
    if (!az_known_path[0] || !id[0] || !loc[0] || strchr(id, ' ') || (!az_known_get(id, was, sizeof was) && !strcmp(was, loc))) return;
    char tmp[720], line[300];
    snprintf(tmp, sizeof tmp, "%s.%d", az_known_path, (int)getpid());
    FILE *in = fopen(az_known_path, "r"), *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return; }
    int n = 0;
    while (in && fgets(line, sizeof line, in) && n < 63) {
        char k[100]; if (sscanf(line, "%99s", k) != 1 || !strcasecmp(k, id)) continue;
        fputs(line, out); n++;
    }
    if (in) fclose(in);
    fprintf(out, "%s %s\n", id, loc);
    if (fclose(out) || rename(tmp, az_known_path)) unlink(tmp);
}
#define AZ_FORGET_S 60.0
static void *az_browse_main(void *arg) {
    (void)arg;
    for (;;) {
        anet_peer P[16];

        char ask[160] = "";
        pthread_mutex_lock(&slk);
        if (!strncmp(spec_now, "audiolaz:", 9)) snprintf(ask, sizeof ask, "%s", spec_now + 9);
        const int playing = AZ;
        pthread_mutex_unlock(&slk);
        if (!ask[0]) { pthread_mutex_lock(&clk); if (!strncmp(bound_spec, "audiolaz:", 9)) snprintf(ask, sizeof ask, "%s", bound_spec + 9); pthread_mutex_unlock(&clk); }

        static char last_ask[160];
        if (!last_ask[0]) { const char *e = getenv("AUDIOLAZ_RECEIVER"); if (e) snprintf(last_ask, sizeof last_ask, "%s", e); }
        if (ask[0] && strchr(ask, '.')) snprintf(last_ask, sizeof last_ask, "%s", ask);   /* an address, never a bare id */
        if (!ask[0] || !strchr(ask, '.')) snprintf(ask, sizeof ask, "%s", last_ask);
        const char *pc = strchr(ask, ':');
        const int aport = pc ? atoi(pc + 1) : ANET_PORT;
        ask[strcspn(ask, ":")] = 0;
        struct in_addr ia;
        const int is_ip = inet_pton(AF_INET, ask, &ia) == 1;
        int n = anet_mdns_browse2(P, 15, 1500, is_ip ? ask : NULL);

        { int m = 0; for (int k = 0; k < n; k++) if (!own_ipv4(P[k].addr)) { if (m != k) P[m] = P[k]; m++; } n = m; }

        int known = 0;
        for (int k = 0; k < n; k++) known |= !strcmp(P[k].addr, ask);
        if (is_ip && !known && !playing && anet_info(ask, aport > 0 ? aport : ANET_PORT, &P[n], 1500) == 0) n++;
        const double t = mono();
        pthread_mutex_lock(&rlk);
        for (int k = 0; k < n; k++) {
            int i = -1, fr = -1;
            for (int j = 0; j < NRCV; j++) { if (R[j].used && R[j].kind == NK_AUDIOLAZ && !strcmp(R[j].id, P[k].id)) i = j; if (!R[j].used && fr < 0) fr = j; }
            if (i < 0) {
                if (fr < 0) continue;
                i = fr; memset(&R[i], 0, sizeof R[i]); R[i].fd = -1; R[i].used = 1; R[i].kind = NK_AUDIOLAZ;
                nlog("audiolaz receiver \"%s\" (%s) at %s:%d, up to %u Hz", P[k].id, P[k].dac, P[k].addr, P[k].port, P[k].max_rate);
            }
            snprintf(R[i].id, sizeof R[i].id, "%.63s", P[k].id);
            snprintf(R[i].name, sizeof R[i].name, "audiolaz Link \xe2\x86\x92 %.76s", P[k].dac[0] ? P[k].dac : P[k].id);
            snprintf(R[i].model, sizeof R[i].model, "%.63s", P[k].dac);
            snprintf(R[i].addr, sizeof R[i].addr, "%.47s", P[k].addr);
            snprintf(R[i].loc, sizeof R[i].loc, "%s:%d", P[k].addr, P[k].port);
            if (P[k].hw[0] || !R[i].hw[0]) snprintf(R[i].hw, sizeof R[i].hw, "%.95s", P[k].hw);   /* (an mDNS answer and INFO agree) */
            if (P[k].rates[0]) snprintf(R[i].rates, sizeof R[i].rates, "%s", P[k].rates);   /* INFO's only */
            if (P[k].rates[0]) snprintf(R[i].bits, sizeof R[i].bits, "%s", P[k].bits);      /* INFO's only, with its rates */
            if (P[k].rates[0] || P[k].err[0]) snprintf(R[i].err, sizeof R[i].err, "%s", P[k].err);
            R[i].max_rate = P[k].max_rate; R[i].seen = t;
            az_known_put(R[i].id, R[i].loc);              /* its address kept by its id */
        }
        for (int j = 0; j < NRCV; j++)
            if (R[j].used && R[j].kind == NK_AUDIOLAZ && t - R[j].seen > AZ_FORGET_S && !(AZ && S.ri == j)) { nlog("audiolaz receiver \"%s\" no longer seen", R[j].id); R[j].used = 0; }
        pthread_mutex_unlock(&rlk);
        sleep(n ? 15 : 5);
    }
    return NULL;
}

#define AZ_UP_HOOK "/usr/local/lib/audiolaz/audiolaz-receiver-up"
static void az_up(const char *host, int port) {
    static double last = 0;
    if (access(AZ_UP_HOOK, X_OK) || mono() - last < 15.0) return;
    last = mono();
    char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    const pid_t p = fork();
    if (p == 0) {                                      /* a grandchild: never a zombie of ours */
        if (fork() == 0) { setsid(); execl(AZ_UP_HOOK, AZ_UP_HOOK, host, ps, (char *)NULL); _exit(127); }
        _exit(0);
    }
    if (p > 0) { int st; waitpid(p, &st, 0); nlog("audiolaz receiver %s:%d not answering: %s started", host, port, AZ_UP_HOOK); }
}

int netout_az_unknown(const char *spec, char *heard, size_t hl) {
    const char *key;
    if (heard && hl) heard[0] = 0;
    if (!spec || spec_kind(spec, &key) != NK_AUDIOLAZ) return 0;
    pthread_mutex_lock(&rlk);
    const int ri = find_rcv(NK_AUDIOLAZ, key);
    if (heard && hl) for (int i = 0; i < NRCV; i++) if (R[i].used && R[i].kind == NK_AUDIOLAZ && !R[i].self) {
        const size_t l = strlen(heard);
        snprintf(heard + l, hl - l, "%s%s%s%s%s at %s", l ? ", " : "", R[i].id, R[i].model[0] ? " (" : "", R[i].model, R[i].model[0] ? ")" : "", R[i].loc);
    }
    pthread_mutex_unlock(&rlk);
    return ri < 0 && !strchr(key, '.') && !strchr(key, ':');
}

#define AZQ_S 3.0
static const double AZQ_STEPS[] = { 1, 2, 5, 10, 15 };
static struct { char spec[160]; int n; double until; } AZQ;
static char AZ_STUCK[160];                             /* the receiver whose DAC refuses to start ("" none) */
int netout_dac_start(const char *spec) { return spec && AZ_STUCK[0] && !strcmp(spec, AZ_STUCK); }
static void az_quick_note(const char *spec, int finish) {
    unsigned rw = 0;
    const int quick = !finish && anet_lost_early(AZQ_S, &rw);
    if (strcmp(AZQ.spec, spec)) { memset(&AZQ, 0, sizeof AZQ); snprintf(AZQ.spec, sizeof AZQ.spec, "%s", spec); }
    if (!quick) { AZQ.n = 0; return; }                 /* played (or lost later): forgotten */
    const int k = AZQ.n < (int)(sizeof AZQ_STEPS / sizeof AZQ_STEPS[0]) ? AZQ.n : (int)(sizeof AZQ_STEPS / sizeof AZQ_STEPS[0]) - 1;
    AZQ.n++;
    AZQ.until = mono() + AZQ_STEPS[k];
    if (rw == ANET_WHY_DAC_START) snprintf(AZ_STUCK, sizeof AZ_STUCK, "%s", spec);
    nlog("%s: its stream lost within %.0f s of its start (%s%s%s): the next one asked in %.0f s", spec, AZQ_S, anet_why(),
         rw ? ": " : "", rw ? anet_why_text(rw) : "", AZQ_STEPS[k]);
}
/* (21/10, point 7 of that evening: the Flex 8 DL took 48 kHz only, the stream went at 44.1 kHz, the Mac's Core Audio
 * converted) A receiver that says the rates its DAC takes as it is (INFO rates=, asked before each stream): a stream at
 * a rate it does not take goes at one it does, converted here by the corrector's own converter (azsrc.h: −156 dB of
 * error in band), never at its machine; ANET_CAP_RATES offered, so that a receiver that learns a refusal only by trying
 * (a Mac's device that lists a rate and stays at another) refuses it rather than resampling: its rates asked again,
 * the stream opened again at one of them. The rate chosen: the same family above (44.1 → 88.2, 176.4), then any above,
 * then the highest below (out_coreaudio.c's rule). The play upstream knows nothing of it (its frames, its latency, its
 * pace: all said in its own rate here). */
static struct { azsrc *s; unsigned rate, drate; int32_t *p; long pn, po, pcap; uint64_t taken; } AZC;
static void az_conv_free(void) { azsrc_free(AZC.s); free(AZC.p); memset(&AZC, 0, sizeof AZC); }
static unsigned az_pick(const char *rates, unsigned want) {
    static const unsigned R_[] = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000 };
    const int n = (int)(sizeof R_ / sizeof R_[0]);
    for (int i = 0; i < n; i++) if (R_[i] > want && R_[i] % want == 0 && anet_rate_in(rates, R_[i]) == 1) return R_[i];
    for (int i = 0; i < n; i++) if (R_[i] > want && anet_rate_in(rates, R_[i]) == 1) return R_[i];
    for (int i = n - 1; i >= 0; i--) if (R_[i] < want && anet_rate_in(rates, R_[i]) == 1) return R_[i];
    return 0;
}
unsigned netout_link_rate(void) { return AZ && AZC.s ? AZC.drate : 0; }
/* the bits the receiver's DAC keeps at the rate it is sent (INFO bits=, anet.h): this stream's, or (for a plan
 * before it opens, roomd.c's DoP) the last INFO's of the receiver spec names; 0 not said */
static int AZ_BITS = 0;
int netout_link_bits(void) { return AZ ? AZ_BITS : 0; }
int netout_link_bits_for(const char *spec, unsigned rate) {
    const char *key;
    if (!spec || spec_kind(spec, &key) != NK_AUDIOLAZ) return 0;
    pthread_mutex_lock(&rlk);
    const int i = find_rcv(NK_AUDIOLAZ, key);
    const int b = i >= 0 ? anet_bits_at(R[i].rates, R[i].bits, rate) : 0;
    pthread_mutex_unlock(&rlk);
    return b;
}
unsigned netout_conv_rate(void) {                     /* the rate it goes at once converted here (0: as it is) */
    if (AZ) return AZC.s ? AZC.drate : 0;
    pthread_mutex_lock(&slk);
    const unsigned r = S.on && S.cv ? S.rate : 0;
    pthread_mutex_unlock(&slk);
    return r;
}
static int AZ_CONV = 1;                                /* netout_link_convert (the tests: a sender before 21/10) */
void netout_link_convert(int on) { AZ_CONV = on; }
/* (22/10, the review) the next stream's words DoP (roomd.c): said to the receiver (ANET_F_DOP), never converted (its
 * markers and one-bit samples would be noise), refused by us when its DAC does not take that rate as it is or keeps
 * fewer than 24 bits there (its INFO), and by it when it cannot play them whole (ANET_E_FORMAT): netout_dop_refused 1,
 * once, the player playing PCM to it from then on */
static int AZ_DOP = 0, AZ_DOP_NO = 0;
void netout_link_dop(int on) { AZ_DOP = on; }
int netout_dop_refused(void) { const int r = AZ_DOP_NO; AZ_DOP_NO = 0; return r; }
static int az_open(const char *spec, const char *key, unsigned rate) {
    char host[256] = ""; int port = ANET_PORT;
    az_conv_free();
    if (!strcmp(AZQ.spec, spec) && mono() < AZQ.until) {
        set_why("its last stream lost at its start (%d in a row): asked again in %.0f s", AZQ.n, ceil(AZQ.until - mono()));
        last_asked = 0;
        return -1;
    }
    pthread_mutex_lock(&rlk);
    const int ri = find_rcv(NK_AUDIOLAZ, key);
    unsigned maxr = 0;
    if (ri >= 0) { snprintf(host, sizeof host, "%s", R[ri].loc); maxr = R[ri].max_rate; }
    pthread_mutex_unlock(&rlk);
    if (ri < 0 && (strchr(key, '.') || az_known_get(key, host, sizeof host))) snprintf(host, sizeof host, "%s", key);
    else if (ri < 0) {                                 /* link-known: said once an address */
        static char said[300];
        char now_[300]; snprintf(now_, sizeof now_, "%s %s", spec, host);
        if (strcmp(said, now_)) { snprintf(said, sizeof said, "%s", now_); nlog("%s: not heard lately: asked at its last address, %s", spec, host); }
    }
    char *c = strrchr(host, ':');
    if (c) { port = atoi(c + 1); *c = 0; }
    if (maxr && rate > maxr) { set_why("%s takes up to %u Hz, not %u", spec, maxr, rate); return -1; }
    /* another player plays there: asked again once its wait is over, or at once to take it over */
    const double t = mono();
    char by[128];
    pthread_mutex_lock(&azlk);
    if (strcmp(AZB.spec, spec)) { const double tk = AZB.take_until; memset(&AZB, 0, sizeof AZB); AZB.take_until = tk; snprintf(AZB.spec, sizeof AZB.spec, "%s", spec); }
    const int take = AZB.take_until > t;
    const double wait = !take && AZB.fails ? AZB.until - t : 0;
    snprintf(by, sizeof by, "%s", AZB.by);
    pthread_mutex_unlock(&azlk);
    last_asked = 0;
    if (wait > 0) { set_why("busy: playing from \"%s\" (asked again in %.0f s)", by, ceil(wait)); return -1; }
    {   /* who we are: this machine and our name (the same id from one run to the next: the receiver lets us back in) */
        char me[96], h[128] = "", uid[40];
        pthread_mutex_lock(&clk); snprintf(me, sizeof me, "%s", me_name); pthread_mutex_unlock(&clk);
        gethostname(h, sizeof h - 1);
        uint64_t x = 1469598103934665603ULL;
        for (const char *c = h; *c; c++) { x ^= (unsigned char)*c; x *= 1099511628211ULL; }
        x ^= '|'; x *= 1099511628211ULL;
        for (const char *c = me; *c; c++) { x ^= (unsigned char)*c; x *= 1099511628211ULL; }
        snprintf(uid, sizeof uid, "%016llx", (unsigned long long)x);
        anet_identity(uid, me);
    }
    anet_take(take);
    char why[600];
    /* its rates (and whether its DAC refuses to start), asked now: the rate it takes */
    anet_peer ip;
    char rates[96] = "", bits[48] = "";
    if (!anet_info(host, port, &ip, 1000)) {
        snprintf(rates, sizeof rates, "%s", ip.rates);
        snprintf(bits, sizeof bits, "%s", ip.bits);
        pthread_mutex_lock(&rlk);
        if (ri >= 0 && R[ri].used) { snprintf(R[ri].rates, sizeof R[ri].rates, "%s", ip.rates); snprintf(R[ri].err, sizeof R[ri].err, "%s", ip.err); snprintf(R[ri].bits, sizeof R[ri].bits, "%s", ip.bits); }
        pthread_mutex_unlock(&rlk);
        if (!strcmp(ip.err, "dac-start")) {
            snprintf(AZ_STUCK, sizeof AZ_STUCK, "%s", spec);
            set_why("%s: its DAC (%s) refuses to start: unplug it and plug it in again", spec, ip.dac);
            last_asked = 0;
            return -1;
        }
    }
    unsigned drate = rate;
    AZ_DOP_NO = 0;
    if (AZ_DOP) {                                      /* DoP: as it is, or not at all */
        const int b = anet_bits_at(rates, bits, rate);
        if (anet_rate_in(rates, rate) == 0 || (b > 0 && b < 24)) {
            AZ_DOP_NO = 1;
            set_why("%s: DoP at %u Hz not sent: its DAC %s (its markers and one-bit samples would be noise): PCM to it", spec, rate,
                    b > 0 && b < 24 ? "keeps fewer than 24 bits there" : "does not take that rate as it is, and DoP is never converted");
            last_asked = 1;
            return -1;
        }
    }
    if (!AZ_DOP && AZ_CONV && anet_rate_in(rates, rate) == 0) drate = az_pick(rates, rate);
    if (!drate) drate = rate;
    anet_convert(AZ_CONV && !AZ_DOP);
    int e = anet_open(host, port, drate, AZ_DOP ? ANET_F_DOP : ANET_F_S32, 0, why, sizeof why);
    if (e && AZ_DOP && anet_refused() == ANET_E_FORMAT) AZ_DOP_NO = 1;   /* its DAC cannot play it whole: PCM */
    if (e && !AZ_DOP && AZ_CONV && anet_refused() == ANET_E_RATE && drate == rate) {   /* refused by trying (a Mac's device): its rates again */
        if (!anet_info(host, port, &ip, 1000) && ip.rates[0]) {
            snprintf(rates, sizeof rates, "%s", ip.rates);
            snprintf(bits, sizeof bits, "%s", ip.bits);
            const unsigned d2 = az_pick(rates, rate);
            if (d2 && d2 != rate) { drate = d2; e = anet_open(host, port, drate, ANET_F_S32, 0, why, sizeof why); }
        }
    }
    if (e && anet_refused() == ANET_E_DAC_START) snprintf(AZ_STUCK, sizeof AZ_STUCK, "%s", spec);
    if (!e) AZ_STUCK[0] = 0;
    if (!e && drate != rate) {
        AZC.s = azsrc_new(rate, drate);
        AZC.pcap = 8192; AZC.p = malloc(sizeof(int32_t) * 2 * (size_t)AZC.pcap);
        if (!AZC.s || !AZC.p) { az_conv_free(); anet_close(0); set_why("%s: no converter %u → %u Hz", spec, rate, drate); return -1; }
        AZC.rate = rate; AZC.drate = drate;
        nlog("%s: its DAC takes %s Hz, not %u Hz as it is: the stream at %u Hz, converted here (the corrector's converter, %u taps a phase, %.1f ms; never the receiver's machine)",
             spec, rates, rate, drate, AZC.s->T, 1000.0 * azsrc_delay_in(AZC.s) / rate);
    }
    if (e) {
        if (anet_busy(by, sizeof by)) {                    /* another player there: never a rate refused, nothing to start */
            pthread_mutex_lock(&azlk);
            const int first = !AZB.fails;
            const int k = AZB.fails < (int)(sizeof AZB_STEPS / sizeof AZB_STEPS[0]) ? AZB.fails : (int)(sizeof AZB_STEPS / sizeof AZB_STEPS[0]) - 1;
            if (first) AZB.t_first = t;
            AZB.fails++; AZB.t_last = mono(); AZB.until = AZB.t_last + AZB_STEPS[k]; AZB.take_until = 0;
            snprintf(AZB.by, sizeof AZB.by, "%.95s", by);
            pthread_mutex_unlock(&azlk);
            if (first) nlog("%s: busy, playing from \"%s\": asked again in 2, 5, 10 s, then every 15 s (nothing sent meanwhile)", spec, by);
            set_why("busy: playing from \"%s\"", by);
            return -1;
        }
        {   /* a name no receiver answers to: which ones there are */
            char heard[400];
            if (strstr(why, "unknown address") && netout_az_unknown(spec, heard, sizeof heard)) {
                const size_t l = strlen(why);
                snprintf(why + l, sizeof why - l, " (no receiver of that id or name heard%s%s; out=audiolaz:<its id> or audiolaz:<its address>)", heard[0] ? ": " : "", heard);
            }
        }
        last_asked = strstr(why, "refused ") != NULL;
        if (!last_asked && (strstr(why, "onnection refused") || strstr(why, "timed out") || strstr(why, "o route") || strstr(why, "no answer"))) az_up(host, port);
        set_why("%s", why);
        return -1;
    }
    pthread_mutex_lock(&slk);
    AZ = 1; S.ri = ri;
    AZ_BITS = anet_bits_at(rates, bits, drate);        /* its DAC's bits at the rate sent (0 not said) */
    pthread_mutex_unlock(&slk);
    {   /* the receiver ours: the wait forgotten, said */
        pthread_mutex_lock(&azlk);
        const int was = AZB.fails;
        const double since = mono() - AZB.t_first;
        AZB.fails = 0; AZB.until = 0; AZB.take_until = 0; AZB.by[0] = 0;
        pthread_mutex_unlock(&azlk);
        if (was) nlog("%s: ours now%s, after %.0f s busy (%d questions)", spec, take ? " (taken over: the listener asked)" : "", since, was);
        else if (take) nlog("%s: taken over (the listener asked)", spec);
    }
    if (AZC.s) nlog("stream to %s at %u Hz, converted from %u Hz (audiolaz net: the DAC's clock)", spec, AZC.drate, rate);
    else if (AZ_BITS > 0 && AZ_BITS < 32) nlog("stream to %s at %u Hz (audiolaz net: its DAC keeps %d bits, the music requantized to them here, the DAC's clock)", spec, rate, AZ_BITS);
    else nlog("stream to %s at %u Hz (audiolaz net: 32 bits as they are, the DAC's clock)", spec, rate);
    return 0;
}
/* through the converter: what it made and anet has not taken first (all of it, or 0: full); then the frames
 * given, converted, as far as anet takes; the frames given taken in all (their conversion kept for the next call) */
static long az_conv_write(const int32_t *lr, long frames) {
    while (AZC.po < AZC.pn) {
        const long k = anet_write(AZC.p + 2 * AZC.po, AZC.pn - AZC.po);
        if (k < 0) return -1;
        if (k == 0) return 0;
        AZC.po += k;
    }
    AZC.pn = AZC.po = 0;
    /* at most what fits the buffer: frames · drate/rate + 2 out of this call's input */
    long n = (long)((double)(AZC.pcap - 4) * AZC.rate / AZC.drate);
    if (n > frames) n = frames;
    long used = 0;
    AZC.pn = azsrc_run(AZC.s, lr, n, AZC.p, AZC.pcap, &used);
    AZC.taken += (uint64_t)used;
    while (AZC.po < AZC.pn) {
        const long k = anet_write(AZC.p + 2 * AZC.po, AZC.pn - AZC.po);
        if (k < 0) return -1;
        if (k == 0) break;
        AZC.po += k;
    }
    return used;
}
/* its frames (at the receiver's rate) in the play's own */
static long az_conv_in(double frames) { return (long)(frames * AZC.rate / AZC.drate); }
int netout_open(unsigned rate) {
    last_asked = 0;
    {
        char sp[160]; const char *k;
        pthread_mutex_lock(&slk); snprintf(sp, sizeof sp, "%s", spec_now); pthread_mutex_unlock(&slk);
        if (AZ) { anet_close(0); AZ = 0; }
        if (spec_kind(sp, &k) == NK_AUDIOLAZ) return az_open(sp, k, rate);
    }
    char spec[160];
    pthread_mutex_lock(&slk);
    snprintf(spec, sizeof spec, "%s", spec_now);
    pthread_mutex_unlock(&slk);
    const char *key;
    const int kind = spec_kind(spec, &key);
    if (!kind) { set_why("no network output chosen"); return -1; }
    if (kind == NK_AIRPLAY) ap_names();
    if (http_fd < 0) { set_why("the stream's port %d could not be opened", http_port_); return -1; }
    if (SLIM(kind) && relay_name[0]) {                   /* a voie: the player's receiver, borrowed */
        pthread_mutex_lock(&rlk);
        const int have = find_rcv(kind, key) >= 0;
        pthread_mutex_unlock(&rlk);
        if (!have && relay_fetch(spec)) return -1;
    }
    pthread_mutex_lock(&rlk);
    const int ri = find_rcv(kind, key);
    const int lent = ri >= 0 && R[ri].lent;
    struct in_addr local = { 0 };
    char addr[48] = "", name[96] = "";
    unsigned maxr = 0;
    if (ri >= 0) { maxr = R[ri].max_rate; snprintf(addr, sizeof addr, "%s", R[ri].addr); snprintf(name, sizeof name, "%s", R[ri].name); }
    pthread_mutex_unlock(&rlk);
    if (lent) { set_why("%s: played by a voie of this player", spec); return -1; }   /* */
    if (ri < 0) { set_why("%s: %s", spec, kind == NK_SQUEEZE ? "not connected (squeezelite -s <this machine>:3484)" : kind == NK_AIRPLAY ? "not found by the AirPlay bridge (audiolaz-airplay-out), or the player's own AirPlay input" : "not found on the network"); return -1; }
    if (maxr && rate > maxr && kind != NK_UPNP) { set_why("%s takes up to %u Hz, not %u", name, maxr, rate); return -1; }
    {
        pthread_mutex_lock(&slk);
        const double w = !strcmp(BK.spec, spec) ? BK.until - mono() : 0;
        const int nf = BK.fails;
        pthread_mutex_unlock(&slk);
        if (w > 0) { set_why("%s does not answer (%d failed): tried again in %.0f s", name, nf, ceil(w)); return -1; }
    }

    if (!claim_ignore && claim_fd >= 0) {
        const double young = 1.0 - (mono() - claim_t0);
        if (young > 0) usleep((useconds_t)(young * 1e6));
        char by[96];
        if (netout_used_by(spec, by, sizeof by) == 2) { set_why("\"%s\" used by the audiolaz player \"%s\"", name, by); return -1; }
    }
    if (kind == NK_UPNP && local_addr_to(addr, &local)) { set_why("%s (%s): no route to it", name, addr); return -1; }
    wait_tail(ri);

    if (kind == NK_AIRPLAY) {
        struct in_addr l;
        const double behind = netout_hold_floor_ms() / 1000.0 - 0.25;
        pthread_mutex_lock(&rlk);
        /* idle: since the STMu that followed its last STMs (none yet: still on its last stream, as when wait_tail saw
         * its STMd only; never a stream: -1) */
        const double idle = !R[ri].n_stms ? -1 : R[ri].n_stmu > R[ri].stmu_at_stms ? mono() - R[ri].t_stmu : 0;
        const int qfd = (idle < 0 || idle > behind) && find_rcv(kind, key) == ri ? slim_dup(ri, &l) : -1;
        pthread_mutex_unlock(&rlk);
        if (qfd >= 0) { strm_send(qfd, 'q', 0, 0, l, NULL, 0); close(qfd); }
        if (idle >= 0) nlog("%s: idle %.1f s since its last stream%s", name, idle, qfd >= 0 ? ": its RAOP clock started again (strm q)" : ": goes on from its clock");
    }
    pthread_mutex_lock(&slk);
    if (S.fd >= 0) { close(S.fd); S.fd = -1; }
    /* the receiver looked up again after the tail (08/10, review: up to TAIL_S, the player gone or come again by then,
     * its connection closed and its number soon another's); the Squeezebox player's connection dup'd, ours until told */
    pthread_mutex_lock(&rlk);
    int fd = -1;
    const int again = find_rcv(kind, key) == ri;
    if (again && SLIM(kind)) fd = slim_dup(ri, &local);
    if (again) snprintf(addr, sizeof addr, "%s", R[ri].addr);
    pthread_mutex_unlock(&rlk);
    if (!again || (SLIM(kind) && fd < 0)) {
        pthread_mutex_unlock(&slk);
        set_why("%s: %s", spec, kind == NK_SQUEEZE ? "its connection went (squeezelite -s <this machine>:3484)" : kind == NK_AIRPLAY ? "the AirPlay bridge let it go" : "no longer found on the network");
        return -1;
    }
    const unsigned id = ++stream_ids;

    azsrc_free(S.cv); free(S.cvb); S.cv = NULL; S.cvb = NULL;
    S.irate = rate; S.cin = 0; S.clips = 0;
    if (kind == NK_AIRPLAY && rate != AP_RATE) {
        S.cv = azsrc_new(rate, AP_RATE);
        S.cvb = S.cv ? malloc(sizeof(int32_t) * 2 * CHUNK) : NULL;
        if (!S.cv || !S.cvb) { azsrc_free(S.cv); S.cv = NULL; free(S.cvb); S.cvb = NULL; nlog("%s: no converter %u → %u Hz: sent at %u Hz (the bridge resamples it)", name, rate, AP_RATE, rate); }
    }
    S.on = 1; S.kind = kind; S.ri = ri; S.id = id; S.rate = S.cv ? AP_RATE : rate;
    S.bpf = kind == NK_AIRPLAY ? 4 : BPF;
    S.plen = S.poff = 0; S.bytes = 0; S.full_once = 0; S.paused = 0; S.lost = 0; S.ended = 0; S.t_take = S.t_gone = mono(); S.t_block = 0;
    S.tstate = TS_UNKNOWN; S.fails = 0; S.seen_playing = 0; S.rel_moved = 0; S.rel = 0; S.t_rel = 0; S.t_playing = 0;
    S.t_pause0 = 0; S.paused_s = 0; S.rel_whole = 0; S.t_poll = 0; S.low = low_lat;
    S.t_open = mono(); S.taken = 0; S.uri_ours = 0; S.gone = 0;
    S.smooth = smooth_next && kind == NK_UPNP; S.corr = 0; S.corr0 = 0; S.n_corr = 0;
    pthread_mutex_lock(&rlk);
    S.stms0 = R[ri].n_stms; S.stmd0 = R[ri].n_stmd; S.stmu0 = R[ri].n_stmu; S.stmf0 = R[ri].n_stmf; S.stmn0 = R[ri].n_stmn; S.stmc0 = R[ri].n_stmc;
    pthread_mutex_unlock(&rlk);
    pthread_mutex_unlock(&slk);
    claim_announce();                                 /* ours now, said before it is told */
    char ip[INET_ADDRSTRLEN], url[128];
    inet_ntop(AF_INET, &local, ip, sizeof ip);
    snprintf(url, sizeof url, "http://%s:%d/stream/%u.wav", ip, http_port_, id);
    int told = 0;
    if (SLIM(kind)) {
        char hdr[160];
        snprintf(hdr, sizeof hdr, "GET /stream/%u.wav HTTP/1.0\r\nHost: %s:%d\r\n\r\n", id, ip, http_port_);
        if (kind == NK_SQUEEZE) audg_unity(fd);
        else audg_bridge(fd);                        /* AirPlay: its volume left as it is */
        told = !strm_send(fd, 's', 0, S.cv ? AP_RATE : rate, local, hdr, kind == NK_AIRPLAY);   /* the wire's rate */
        close(fd);
    } else {
        /* what plays as it starts: title, artist, album, the cover (nowplay.h) */
        char didl[6144], esc_didl[12288], args[12800], meta[4096];
        np_didl(meta, sizeof meta, ip);
        snprintf(didl, sizeof didl, "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                 "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\"><item id=\"audiolaz-%u\" parentID=\"0\" restricted=\"1\">%s"
                 "<upnp:class>object.item.audioItem.musicTrack</upnp:class><res protocolInfo=\"http-get:*:audio/wav:DLNA.ORG_OP=00;DLNA.ORG_CI=0;"
                 "DLNA.ORG_FLAGS=01700000000000000000000000000000\" sampleFrequency=\"%u\" bitsPerSample=\"24\" nrAudioChannels=\"2\">%s</res></item></DIDL-Lite>",
                 id, meta, rate, url);
        xml_escape(didl, esc_didl, sizeof esc_didl);
        soap(ri, "Stop", "<InstanceID>0</InstanceID>", NULL, 0);   /* whatever it played: ours now */
        snprintf(args, sizeof args, "<InstanceID>0</InstanceID><CurrentURI>%s</CurrentURI><CurrentURIMetaData>%s</CurrentURIMetaData>", url, esc_didl);
        told = !soap(ri, "SetAVTransportURI", args, NULL, 0) && !soap(ri, "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>", NULL, 0);
    }
    pthread_mutex_lock(&slk);
    if (told) {                                       /* its fetch, FETCH_S at most */
        const double t0 = mono();
        while (S.fd < 0 && !S.lost && mono() - t0 < FETCH_S) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&scv, &slk, &ts);
        }
    }
    const int ok = told && S.fd >= 0 && !S.lost;
    if (!ok) {
        last_asked = !S.taken;
        if (!S.lost) set_why("%s (%s) %s", name, addr, told ? "did not fetch the stream within 10 s" : "refused to play it");
        S.on = 0;
        if (S.fd >= 0) { close(S.fd); S.fd = -1; }
        if (!S.taken) bk_after(spec, name, 0, S.gone && kind == NK_AIRPLAY ? BK_STALE_S : 0);
    }
    pthread_mutex_unlock(&slk);
    if (!ok) { tell_stop(kind, ri); claim_announce(); return -1; }
    taken_clear(spec);
    char conv[200] = "";
    if (S.cv) snprintf(conv, sizeof conv, ", converted here to %u Hz (the corrector's converter, %u taps a phase, %.1f ms: the bridge does not resample)",
                       AP_RATE, S.cv->T, 1000.0 * azsrc_delay_in(S.cv) / rate);
    nlog("playing to \"%s\" (%s, %s) at %u Hz%s, %s: %s", name, kind == NK_SQUEEZE ? "Squeezebox" : kind == NK_AIRPLAY ? "AirPlay, through the bridge: 44.1 kHz 16 bits there" : "UPnP", addr, rate,
         conv, kind == NK_AIRPLAY ? "16 bits (TPDF dither, made here once)" : "24 bits", url);
    return 0;
}
/* Under slk: the pending bytes sent as far as the receiver takes them. 0 all sent; 1 some left (its buffer full); −1 the
 * connection closed. */
static int flush_pend(void) {
    while (S.poff < S.plen) {
        ssize_t k = send(S.fd, S.pend + S.poff, S.plen - S.poff, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (k > 0) { S.poff += (size_t)k; S.bytes += (uint64_t)k; S.t_take = mono(); S.t_block = 0; continue; }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* full: its buffer, once it took nothing for 0.2 s (a moment's push-back is only the sockets' own, the
             * receiver reading in bursts: 08/10) */
            const double t = mono();
            if (!S.t_block) S.t_block = t;
            else if (t - S.t_block >= 0.2) S.full_once = 1;
            return 1;
        }
        return -1;
    }
    return 0;
}

static uint32_t dith = 0x2545F491u;
static inline uint32_t dith_next(void) { dith ^= dith << 13; dith ^= dith >> 17; dith ^= dith << 5; return dith; }
static void pack16(const int32_t *lr, long n, uint8_t *p) {
    int whole = 1;
    for (long i = 0; i < 2 * n && whole; i++) whole = (lr[i] & 0xFFFF) == 0;
    for (long i = 0; i < 2 * n; i++) {
        int64_t y;
        if (whole) y = lr[i] >> 16;
        else {
            const int64_t d = (int64_t)(dith_next() & 0xFFFF) + (int64_t)(dith_next() & 0xFFFF) - 65535;   /* −1 … +1 LSB */
            y = ((int64_t)lr[i] + d + 32768) >> 16;
            if (y > 32767) y = 32767; else if (y < -32768) y = -32768;
        }
        p[0] = (uint8_t)y; p[1] = (uint8_t)(y >> 8); p += 2;
    }
}
/* Under slk: the play's frames through the converter (S.cv), as many as make CHUNK frames at most at the
 * wire's rate, into S.cvb; how many of the play's it took (*used), how many it made. Its samples pinned at full scale
 * counted (an intersample peak above 0 dBFS: clipped, said at the stream's end). */
static long cv_run(const int32_t *lr, long frames, long *used) {
    long n = (long)((double)(CHUNK - 4) * S.irate / S.rate);
    if (n > frames) n = frames;
    const long o = azsrc_run(S.cv, lr, n, S.cvb, CHUNK, used);
    for (long i = 0; i < 2 * o; i++) if (S.cvb[i] == INT32_MAX || S.cvb[i] == INT32_MIN) S.clips++;
    S.cin += (uint64_t)*used;
    return o;
}
/* Under slk, fd blocking: the converter's tail (its delay: the last frames given still inside it) sent before
 * the end of file, zeros pushed after them; exactly as many frames at the wire's rate as the play's frames make there
 * (the play's n frames: ceil(n · 44100 / rate)), none more. */
static void cv_tail(int fd) {
    if (!S.cv) return;
    static const int32_t z[2 * 256] = { 0 };
    const uint64_t want = (uint64_t)ceil((double)S.cin * S.rate / S.irate);
    for (int guard = 0; S.cv->n_out < want && guard < 4096; guard++) {
        long used = 0;
        const long cap = (long)(want - S.cv->n_out) < CHUNK ? (long)(want - S.cv->n_out) : CHUNK;
        const long o = azsrc_run(S.cv, z, 256, S.cvb, cap, &used);
        if (o <= 0) { if (!used) break; continue; }
        pack16(S.cvb, o, S.pend);
        if (send_all(fd, S.pend, (size_t)o * 4)) break;
        S.bytes += (uint64_t)o * 4;
    }
}
static void cv_free(void) {                           /* under slk: the stream over (its clips said) */
    if (S.cv && S.clips) nlog("stream %u: %ld samples beyond full scale once converted to %u Hz (intersample peaks): clipped", S.id, S.clips, S.rate);
    azsrc_free(S.cv); free(S.cvb); S.cv = NULL; S.cvb = NULL;
}
long netout_write(const int32_t *lr, long frames) {
    if (AZ) { const long r = AZC.s ? az_conv_write(lr, frames) : anet_write(lr, frames); if (r < 0) set_why("%s", anet_why()); return r; }
    pthread_mutex_lock(&slk);
    const double t = mono();
    long r = 0;
    if (!S.on || S.lost) { r = -1; goto out; }
    if (S.ended) { r = frames; goto out; }            /* nothing more goes after its end (none should come) */
    if (S.fd < 0) {                                   /* its connection gone: back within GONE_S, or lost */
        if (t - S.t_gone > GONE_S) { S.lost = 1; set_why("its stream connection closed and did not come back"); r = -1; }
        goto out;
    }
    int f = flush_pend();
    if (f == 0 && frames > 0 && S.cv) {               /* converted to the wire's rate, then 16 bits once */
        long used = 0;
        const long o = cv_run(lr, frames, &used);
        pack16(S.cvb, o, S.pend);
        S.plen = (size_t)o * 4; S.poff = 0;
        r = used;
        f = flush_pend();
    } else if (f == 0 && frames > 0) {                /* the next frames packed: 24 bits, rounded from the engine's 32 */
        const long n = frames < CHUNK ? frames : CHUNK;
        uint8_t *p = S.pend;
        if (S.bpf == 4) pack16(lr, n, p);
        else for (long i = 0; i < 2 * n; i++) {
            int64_t y = ((int64_t)lr[i] + 128) >> 8;
            if (y > 8388607) y = 8388607;
            p[0] = (uint8_t)y; p[1] = (uint8_t)(y >> 8); p[2] = (uint8_t)(y >> 16); p += 3;
        }
        S.plen = (size_t)n * (size_t)S.bpf; S.poff = 0;
        r = n;
        f = flush_pend();
    }
    if (f < 0) {
        nlog("stream %u: the receiver closed its connection (%.1f s sent)", S.id, (double)S.bytes / (uint64_t)S.bpf / S.rate);
        close(S.fd); S.fd = -1; S.t_gone = t; S.plen = S.poff = 0;
    } else if (f > 0 && !S.paused && t - S.t_take > STALL_S) {
        S.lost = 1; set_why("the receiver took nothing for %.0f s", STALL_S); r = -1;
    }
out:
    pthread_mutex_unlock(&slk);
    return r;
}
void netout_wait(int ms) {
    if (AZ) { anet_wait(ms); return; }
    pthread_mutex_lock(&slk);
    const int fd = S.fd;
    pthread_mutex_unlock(&slk);
    if (fd < 0) { usleep((useconds_t)ms * 1000); return; }
    struct pollfd p = { fd, POLLOUT, 0 };
    poll(&p, 1, ms);
}
uint64_t netout_sent(void) {
    if (AZ) return AZC.s ? AZC.taken : anet_sent();   /* the play's frames taken, converted or not */
    pthread_mutex_lock(&slk);
    /* converted for the AirPlay bridge: the play's frames the converter took (what is still in it, and what it
     * made and the network has not taken, held: netout_held) */
    const uint64_t n = S.cv ? S.cin : S.bytes / (uint64_t)S.bpf;
    pthread_mutex_unlock(&slk);
    return n;
}
int netout_lost(void) {
    if (AZ) return 0;                                 /* an audiolaz receiver: anet_write says it */
    pthread_mutex_lock(&slk);
    const int r = S.on && S.lost;
    pthread_mutex_unlock(&slk);
    return r;
}
long netout_held(void) {
    if (AZ) return AZC.s ? az_conv_in((double)anet_held() + (double)(AZC.pn - AZC.po)) + (long)azsrc_delay_in(AZC.s) : anet_held();
    pthread_mutex_lock(&slk);
    if (!S.on) { pthread_mutex_unlock(&slk); return 0; }
    const double sent = (double)(S.bytes / (uint64_t)S.bpf), t = mono(), srate = S.rate;
    /* converted for the AirPlay bridge: what follows counts frames at the wire's rate (44.1 kHz: the bridge's
     * elapsed time, the bytes); turned back into the play's own at the end, with what the converter holds (taken, not
     * made yet: its delay) and what it made that the network has not taken yet */
    const double ratio = S.cv ? (double)S.irate / S.rate : 1;
    const double inconv = S.cv ? (double)S.cin - (double)S.cv->n_out * ratio : 0;
    const double unsent = S.cv ? (double)S.cv->n_out - sent : 0;
    double played = 0;
    if (SLIM(S.kind)) {
        pthread_mutex_lock(&rlk);
        const rcv *r = &R[S.ri];
        if (r->used && S.ended && r->n_stms > S.stms0 && r->n_stmu > r->stmu_at_stms) played = sent;   /* after our end, played out */
        else if (r->used && r->n_stms > S.stms0 && r->t_stat > 0) {   /* it began this stream: its elapsed time, and since */
            played = (double)r->elapsed_ms * S.rate / 1000.0;
            if (!S.paused) played += (t - r->t_stat) * S.rate;

            if (S.kind == NK_AIRPLAY && r->t_stms > 0) { const double most = (t - r->t_stms) * S.rate; if (played > most) played = most; }
        }
        pthread_mutex_unlock(&rlk);
    } else if (S.seen_playing && S.ended && (S.tstate == TS_STOPPED || S.tstate == TS_NO_MEDIA)) played = sent;   /* played out */
    else if (S.seen_playing && S.smooth)               /* the time since it plays, set right by its position */
        played = ((S.paused && S.t_pause0 > 0 ? S.t_pause0 : t) - S.t_playing - S.paused_s + (S.n_corr >= 10 ? S.corr - S.corr0 : 0)) * S.rate;
    else if (S.seen_playing) {
        if (S.rel_moved) {
            double e = S.tstate == TS_PLAYING && !S.paused ? t - S.t_rel : 0;
            if (S.rel_whole && e > 1.0) e = 1.0;      /* its next second not seen turning yet: no further */
            played = S.rel * S.rate + e * S.rate;
        }
        /* a renderer that says no position: since it plays, its pauses left out (08/10, review: counted as played, its
         * end was heard early, the next stream sent before it had played what it held) */
        else played = ((S.paused && S.t_pause0 > 0 ? S.t_pause0 : t) - S.t_playing - S.paused_s) * S.rate;
    }
    pthread_mutex_unlock(&slk);
    /* more played than ever sent, beyond the guess between its reports : not a figure to trust, nothing played */
    if (played > sent + 0.5 * srate) played = 0;
    if (played > sent) played = sent;
    if (played < 0) played = 0;
    if (ratio != 1) { const double h = (sent - played + (unsent > 0 ? unsent : 0)) * ratio + (inconv > 0 ? inconv : 0); return (long)h; }
    return (long)(sent - played);
}
int netout_started(void) {
    if (AZ) return anet_started();
    pthread_mutex_lock(&slk);
    int on = S.on && !S.lost, k = S.kind, ri = S.ri, seen = S.seen_playing;
    const unsigned s0 = S.stms0;
    pthread_mutex_unlock(&slk);
    if (on && SLIM(k)) { pthread_mutex_lock(&rlk); seen = R[ri].used && R[ri].n_stms > s0; pthread_mutex_unlock(&rlk); }
    return on && seen;
}
int netout_settled(void) {
    if (AZ) return 1;
    pthread_mutex_lock(&slk);
    const int r = !S.smooth || S.n_corr >= 10 || (S.seen_playing && mono() - S.t_playing > 30.0);   /* no position: 30 s */
    pthread_mutex_unlock(&slk);
    return r;
}
int netout_was_full(void) {
    if (AZ) return anet_was_full();
    pthread_mutex_lock(&slk);
    const int f = S.full_once;
    pthread_mutex_unlock(&slk);
    return f;
}
void netout_pause(int on) {
    if (AZ) { anet_pause(on); return; }
    pthread_mutex_lock(&slk);
    if (!S.on || S.paused == on) { pthread_mutex_unlock(&slk); return; }
    S.paused = on;
    S.t_take = mono();                                /* the stall counted from the music going on */
    if (on) S.t_pause0 = S.t_take;                    /* the time paused, for netout_held */
    else if (S.t_pause0 > 0) { if (S.seen_playing) S.paused_s += S.t_take - (S.t_pause0 > S.t_playing ? S.t_pause0 : S.t_playing); S.t_pause0 = 0; }
    const int kind = S.kind, ri = S.ri;
    pthread_mutex_unlock(&slk);
    if (SLIM(kind)) {
        struct in_addr l;
        pthread_mutex_lock(&rlk);
        const int fd = slim_dup(ri, &l);
        pthread_mutex_unlock(&rlk);
        if (fd >= 0) { strm_send(fd, on ? 'p' : 'u', 0, 0, l, NULL, 0); close(fd); }
    } else soap(ri, on ? "Pause" : "Play", on ? "<InstanceID>0</InstanceID>" : "<InstanceID>0</InstanceID><Speed>1</Speed>", NULL, 0);
    nlog("%s", on ? "paused: the receiver told" : "going on: the receiver told");
}
/* All the music sent (the Squeezebox input's last track read to its end): the end of file now, so that the receiver
 * knows it (squeezelite's STMd then STMu, the renderer's STOPPED) while the play goes on following what it plays. */
void netout_end(void) {
    if (AZ) {
        if (AZC.s) {   /* what the converter still holds (its delay) out, then the end: a second at most */
            static const int32_t z[2 * 256] = { 0 };
            long left = (long)azsrc_delay_in(AZC.s) + 2;
            const double t0 = mono();
            while ((left > 0 || AZC.po < AZC.pn) && mono() - t0 < 1.0) {
                const long k = az_conv_write(z, left > 256 ? 256 : left);
                if (k < 0) break;
                if (k == 0 && AZC.po < AZC.pn) { anet_wait(5); continue; }
                left -= k;
            }
        }
        anet_end(); return;
    }
    pthread_mutex_lock(&slk);
    if (!S.on || S.ended || S.lost || S.fd < 0) { pthread_mutex_unlock(&slk); return; }
    const int fd = S.fd;
    fcntl(fd, F_SETFL, 0);
    timeouts(fd, 3000);
    if (S.poff < S.plen) { const size_t k = S.plen - S.poff; if (!send_all(fd, S.pend + S.poff, k)) S.bytes += k; }
    cv_tail(fd);                                      /* the converter's last frames */
    S.plen = S.poff = 0;
    S.fd = -1; S.ended = 1;
    const double secs = (double)(S.bytes / (uint64_t)S.bpf) / S.rate;
    pthread_mutex_unlock(&slk);
    close_gently(fd);
    nlog("all sent (%.1f s): the stream's end given to the receiver", secs);
}
/* (24/10, the release: test/engine-netout.sh fakeapsrc) Roon paused, to a Squeezebox player or the AirPlay bridge
 * (roomd.c give_late): what it played beyond the player's reckoning while it was being stopped, in the play's frames.
 * netout_stop_mark() just before netout_close (the reckoning: its frames played now, its STMf count, the stream's
 * rates); netout_stop_extra(ms) after it: its STMf awaited ms at most, its elapsed time then against the reckoning;
 * 0 when there is nothing to say (another kind of receiver, no STMf, a figure out of reason). */
static struct { int on, ri; unsigned stmf0, rate; double played, ratio; } SM;
void netout_stop_mark(void) {
    SM.on = 0;
    if (AZ) return;
    pthread_mutex_lock(&slk);
    if (!S.on || !SLIM(S.kind)) { pthread_mutex_unlock(&slk); return; }
    const int ri = S.ri;
    const unsigned rate = S.rate;
    const double ratio = S.cv ? (double)S.irate / S.rate : 1;
    pthread_mutex_unlock(&slk);
    const double played = (double)netout_sent() - (double)netout_held();
    pthread_mutex_lock(&rlk);
    const unsigned f0 = R[ri].n_stmf;
    pthread_mutex_unlock(&rlk);
    SM.ri = ri; SM.rate = rate; SM.ratio = ratio; SM.played = played; SM.stmf0 = f0; SM.on = 1;
}
long netout_stop_extra(int ms) {
    if (!SM.on) return 0;
    SM.on = 0;
    const double t0 = mono();
    long e = -1;
    for (;;) {
        pthread_mutex_lock(&rlk);
        if (R[SM.ri].used && R[SM.ri].n_stmf > SM.stmf0) e = R[SM.ri].elapsed_ms;
        pthread_mutex_unlock(&rlk);
        if (e >= 0 || mono() - t0 >= ms / 1000.0) break;
        struct timespec ts = { 0, 5000000 };
        nanosleep(&ts, NULL);
    }
    if (e < 0) return 0;
    const double extra = (double)e * SM.rate / 1000.0 * SM.ratio - SM.played;
    return extra > 0 && extra < SM.rate * SM.ratio ? (long)(extra + 0.5) : 0;   /* more than a second: not believed */
}
void netout_close(int finish) {
    if (AZ) {
        { char sp[160]; pthread_mutex_lock(&slk); snprintf(sp, sizeof sp, "%s", spec_now); pthread_mutex_unlock(&slk); az_quick_note(sp, finish); }   /* */
        anet_close(finish); AZ = 0; az_conv_free(); nlog("audiolaz stream %s", finish ? "ended: the receiver plays out what it holds" : "stopped"); return;
    }
    pthread_mutex_lock(&slk);
    if (!S.on) { pthread_mutex_unlock(&slk); return; }
    const int fd = S.fd, kind = S.kind, ri = S.ri, lost = S.lost, ended = S.ended, taken = S.taken, gone = S.gone;
    const double secs = (double)(S.bytes / (uint64_t)S.bpf) / S.rate;
    S.fd = -1; S.on = 0;
    /* played (its STMs for this stream; a renderer seen PLAYING), or not: the next try's step */
    pthread_mutex_lock(&rlk);
    const int played = SLIM(kind) ? R[ri].n_stms > S.stms0 : S.seen_playing;
    char rname[96];
    snprintf(rname, sizeof rname, "%s", R[ri].name);
    pthread_mutex_unlock(&rlk);
    if (lost && !taken) {
        bk_after(spec_now, rname, played, gone && kind == NK_AIRPLAY ? BK_STALE_S : 0);
        nlog("%s %s: not asked again for %.0f s%s", rname, played ? "lost after playing" : "lost before playing anything",
             BK.until - mono(), gone && kind == NK_AIRPLAY ? " (it left: its device may hold a session nobody tore down)" : "");
    } else if (played) bk_ok(spec_now);
    if (ended && finish) {                            /* its end given already (netout_end): it plays out the rest */
        /* the Squeezebox player's counters from this stream's start, not now (08/10, review: its STMd comes once it has
         * decoded it all, seconds after our end of file; not waited for, the next stream's strm s cut the old one's
         * last seconds, or played them at the new rate); an STMd or STMu of this stream already in: no wait */
        const unsigned d0 = S.stmd0, u0 = S.stmu0, f0 = S.stmf0, s0 = S.stms0, c0 = S.stmc0;
        cv_free();
        pthread_mutex_unlock(&slk);
        if (fd >= 0) close(fd);                       /* none should be (a fetch after the end is refused): never kept */
        pthread_mutex_lock(&rlk);
        F.on = 1; F.kind = kind; F.ri = ri; F.t = mono();
        if (SLIM(kind)) {                             /* an STMu counted from its STMs, as netout_held does */
            /* an STMf counted from its STMc (08/10, VM: the AirPlay bridge answers each strm s with STMf then STMc; taken
             * for the stream's end, the next strm s went at once and cut its last 1.8 s, still undecoded) */
            F.stmd0 = d0;
            F.stmf0 = R[ri].n_stmc > c0 && R[ri].stmf_at_stmc > f0 ? R[ri].stmf_at_stmc : f0;
            F.stmu0 = R[ri].n_stms > s0 && R[ri].stmu_at_stms > u0 ? R[ri].stmu_at_stms : u0;
        }
        else { F.stmd0 = R[ri].n_stmd; F.stmu0 = R[ri].n_stmu; F.stmf0 = R[ri].n_stmf; }
        pthread_mutex_unlock(&rlk);
        return;
    }
    if (finish && fd >= 0 && !lost) {                 /* the last packed frames, then an end of file */
        fcntl(fd, F_SETFL, 0);
        timeouts(fd, 3000);
        if (S.poff < S.plen) { const size_t k = S.plen - S.poff; if (!send_all(fd, S.pend + S.poff, k)) S.bytes += k; }
        cv_tail(fd);                                  /* the converter's last frames */
    }
    S.plen = S.poff = 0;
    cv_free();
    pthread_mutex_unlock(&slk);
    if (finish && fd >= 0 && !lost) {
        close_gently(fd);
        pthread_mutex_lock(&rlk);
        F.on = 1; F.kind = kind; F.ri = ri; F.stmd0 = R[ri].n_stmd; F.stmu0 = R[ri].n_stmu; F.stmf0 = R[ri].n_stmf; F.t = mono();
        pthread_mutex_unlock(&rlk);
        nlog("stream ended after %.1f s: the receiver plays out what it holds", secs);
        return;
    }
    if (fd >= 0) close(fd);
    F.on = 0;
    /* taken by another sender : a renderer is the other's now (never told to stop); the AirPlay bridge is told,
     * so that it does not take the device back */
    if (taken ? SLIM(kind) : !lost || kind == NK_UPNP) tell_stop(kind, ri);
    nlog("stream stopped after %.1f s%s", secs, taken ? " (the receiver taken by another sender)" : lost ? " (the receiver lost)" : "");
    claim_announce();
}

/* ── For the page ──────────────────────────────────────────────────────────────────────────────────── */
void netout_receivers_json(char *out, size_t len) {
    ap_names();
    /* each one's other audiolaz player (usedBy: its name; usedPlaying: it plays to it, else it is set to it):
     * copied under rlk, matched under clk (never both held) */
    static rcv L[NRCV];
    static pthread_mutex_t llk = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&llk);
    int nl = 0;
    pthread_mutex_lock(&rlk);
    for (int i = 0; i < NRCV; i++)
        if (R[i].used && !(SLIM(R[i].kind) && R[i].fd < 0) && !R[i].self) L[nl++] = R[i];
    pthread_mutex_unlock(&rlk);
    size_t w = (size_t)snprintf(out, len, "[");
    const double t = mono();
    for (int i = 0; i < nl; i++) {
        char by[96] = "";
        int used = 0;
        pthread_mutex_lock(&clk);
        for (int k = 0; k < NCLAIM; k++) {
            const claim *c = &CL[k];
            if (!c->used || c->state <= used || t - c->seen > (c->state == 2 ? CLAIM_PLAY_KEEP : CLAIM_IDLE_KEEP) || !claim_is(c, L[i].kind, L[i].id, L[i].name)) continue;
            used = c->state; snprintf(by, sizeof by, "%s", c->name);
        }
        pthread_mutex_unlock(&clk);
        if (L[i].kind == NK_AUDIOLAZ && az_busy_row(&L[i], by, sizeof by)) used = 2;   /* its receiver said so */
        char id[200], nm[200], md[140], byj[200], hwj[200];
        json_escape(L[i].id, id, sizeof id); json_escape(L[i].name, nm, sizeof nm); json_escape(L[i].model, md, sizeof md); json_escape(by, byj, sizeof byj);
        json_escape(L[i].hw, hwj, sizeof hwj);
        /* hw: an audiolaz receiver's DAC by what it is (the app's rooms: src/rooms.js), "" not said */
        char item[1300];
        const int n = snprintf(item, sizeof item, "%s{\"kind\":\"%s\",\"id\":\"%s\",\"name\":\"%s\",\"model\":\"%s\",\"addr\":\"%s\",\"maxRate\":%u,\"usedBy\":\"%s\",\"usedPlaying\":%s,\"hw\":\"%s\"}",
                               w > 1 ? "," : "", kind_name(L[i].kind), id, nm, md, L[i].addr, L[i].max_rate, byj, used == 2 ? "true" : "false", hwj);
        if (n > 0 && w + (size_t)n + 2 < len) { memcpy(out + w, item, (size_t)n); w += (size_t)n; }
    }
    pthread_mutex_unlock(&llk);
    snprintf(out + w, len - w, "]");
}
/* The out= the page has just read, not spec_now: spec_now changes only when a play opens it, so the page named the
 * previous receiver until then . */
int netout_airplay_taken(const char *name) {
    if (!name || !name[0]) return 0;
    ap_names();
    pthread_mutex_lock(&rlk);
    int hit = 0;
    for (int i = 0; i < NRCV && !hit; i++) hit = R[i].used && R[i].kind == NK_AIRPLAY && !R[i].self && !strcasecmp(R[i].name, name);
    pthread_mutex_unlock(&rlk);
    return hit;
}
void netout_out_dac(const char *spec, char *out, size_t len) {
    out[0] = 0;
    const char *key;
    if (!spec || spec_kind(spec, &key) != NK_AUDIOLAZ) return;
    pthread_mutex_lock(&rlk);
    const int i = find_rcv(NK_AUDIOLAZ, key);
    if (i >= 0) snprintf(out, len, "%s", R[i].model);
    pthread_mutex_unlock(&rlk);
}

void netout_out_longname(const char *spec, char *out, size_t len) {
    out[0] = 0;
    const char *key;
    const int kind = spec ? spec_kind(spec, &key) : NK_NONE;
    if (kind != NK_UPNP && kind != NK_SQUEEZE) return;
    pthread_mutex_lock(&rlk);
    const int i = find_rcv(kind, key);
    if (i >= 0) {
        const char *m = R[i].model, *mk = kind == NK_UPNP ? R[i].maker : "";
        const int generic = !m[0] || !strcasecmp(m, "squeezelite") || !strcasecmp(m, "SqueezeLite") || !strncasecmp(m, "MediaRenderer", 13) || !strcasecmp(m, "audiolaz player");
        if (!generic) snprintf(out, len, "%s%s%s", mk[0] && strncasecmp(m, mk, strlen(mk)) ? mk : "", mk[0] && strncasecmp(m, mk, strlen(mk)) ? " " : "", m);
    }
    pthread_mutex_unlock(&rlk);
}

void netout_out_hw(const char *spec, char *out, size_t len) {
    out[0] = 0;
    const char *key;
    if (!spec || spec_kind(spec, &key) != NK_AUDIOLAZ) return;
    pthread_mutex_lock(&rlk);
    const int i = find_rcv(NK_AUDIOLAZ, key);
    if (i >= 0) snprintf(out, len, "%s", R[i].hw);
    pthread_mutex_unlock(&rlk);
}
void netout_out_name(const char *spec, char *out, size_t len) {
    out[0] = 0;
    if (!spec) return;
    const char *key;
    const int kind = spec_kind(spec, &key);
    if (!kind) return;
    if (kind == NK_AIRPLAY) ap_names();
    pthread_mutex_lock(&rlk);
    const int i = find_rcv(kind, key);
    if (i >= 0) snprintf(out, len, "%s", R[i].name);
    pthread_mutex_unlock(&rlk);
}

#define DV_UP_DB 2.0
#define DV_STEP_S 0.25
static const uint8_t LMS_VOL[129] = {   /* squeeze2raop's LMSVolumeMap: LMS index → volume 0…100 */
    0, 3, 6, 7, 8, 10, 12, 13, 14, 16, 17, 18, 19, 20, 21, 22, 24, 25, 26, 27, 28, 28, 29, 30, 31, 32, 33,
    34, 35, 36, 37, 37, 38, 39, 40, 41, 41, 42, 43, 44, 45, 45, 46, 47, 48, 48, 49, 50, 51, 51, 52, 53, 53,
    54, 55, 55, 56, 57, 57, 58, 59, 60, 60, 61, 61, 62, 63, 63, 64, 65, 65, 66, 67, 67, 68, 69, 69, 70, 70,
    71, 72, 72, 73, 73, 74, 75, 75, 76, 76, 77, 78, 78, 79, 79, 80, 80, 81, 82, 82, 83, 83, 84, 84, 85, 86,
    86, 87, 87, 88, 88, 89, 89, 90, 91, 91, 92, 92, 93, 93, 94, 94, 95, 95, 96, 96, 97, 98, 99, 100 };
static pthread_mutex_t dvlk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dvcv = PTHREAD_COND_INITIALIZER;
static struct {
    int has;                     /* a volume asked */
    double want;                 /* dB, −30…0; ≤ −100 muted */
    char spec[160];              /* the out= to set it on */
    unsigned gen;                /* bumped by netout_dev_volume_new: the device's own volume read again */
    double cur;                  /* what the device is at as far as we know */
    int muted;
    char cur_spec[160];
    unsigned cur_gen;
    unsigned stms;               /* the bridge's n_stms at the last set: a new RAOP connection is told again */
    int started;
    char said[200];              /* for /state */
    double max;                  /* the person's ceiling, dB (NAN: the start's) */
    double start;                /* the device's volume at the session's start (NAN: not known: never raised) */
} DV = { .cur = NAN, .max = NAN, .start = NAN };
static struct { char spec[160]; double db; } DV_MEM[8];   /* an AirPlay device's last volume set (it cannot be read) */
static double dv_mem_get(const char *spec) {
    for (int i = 0; i < 8; i++) if (DV_MEM[i].spec[0] && !strcmp(DV_MEM[i].spec, spec)) return DV_MEM[i].db;
    return NAN;
}
static void dv_mem_set(const char *spec, double db) {
    int k = 0;
    for (int i = 0; i < 8; i++) if (!strcmp(DV_MEM[i].spec, spec) || !DV_MEM[i].spec[0]) { k = i; break; }
    snprintf(DV_MEM[k].spec, sizeof DV_MEM[k].spec, "%s", spec); DV_MEM[k].db = db;
}
static double dv_pct_of(double db) {     /* LMS's curve: −30 → 1, −15 → 50, 0 → 100 */
    if (db <= -30) return 1;
    if (db >= 0) return 100;
    return db < -15 ? 1 + (db + 30) * 49 / 15 : 50 + (db + 15) * 50 / 15;
}
static double dv_db_of(double pct) {
    if (pct <= 1) return -30;
    if (pct >= 100) return 0;
    return pct < 50 ? -30 + (pct - 1) * 15 / 49 : -15 + (pct - 50) * 15 / 50;
}
static int dv_lms_index(double db) {
    if (db <= -100) return 0;
    const double v = 1 + (db < -30 ? 0 : db > 0 ? 30 : db + 30) * 99 / 30;
    int best = 1;
    for (int k = 1; k < 129; k++) if (fabs(LMS_VOL[k] - v) < fabs(LMS_VOL[best] - v)) best = k;
    return best;
}
int netout_dev_volume_kind(const char *spec) {
    const char *key;
    const int k = spec_kind(spec, &key);
    if (k == NK_AIRPLAY) return 1;
    if (k != NK_UPNP) return 0;
    pthread_mutex_lock(&rlk);
    const int ri = find_rcv(NK_UPNP, key), rc = ri >= 0 && R[ri].rc_url[0];
    pthread_mutex_unlock(&rlk);
    return rc ? 2 : 0;
}
void netout_dev_volume(const char *spec, double db) {
    pthread_mutex_lock(&dvlk);
    DV.has = 1; DV.want = db; snprintf(DV.spec, sizeof DV.spec, "%s", spec ? spec : "");
    pthread_cond_signal(&dvcv);
    pthread_mutex_unlock(&dvlk);
}
void netout_dev_volume_new(void) {
    pthread_mutex_lock(&dvlk); DV.gen++; pthread_cond_signal(&dvcv); pthread_mutex_unlock(&dvlk);
}
void netout_dev_volume_max(double db) {
    pthread_mutex_lock(&dvlk);
    if (!(isnan(db) && isnan(DV.max)) && db != DV.max) { DV.max = db; pthread_cond_signal(&dvcv); }
    pthread_mutex_unlock(&dvlk);
}
void netout_dev_volume_said(char *out, size_t len) { pthread_mutex_lock(&dvlk); snprintf(out, len, "%s", DV.said); pthread_mutex_unlock(&dvlk); }
/* One volume set on the device: 0 done. */
static int dv_apply(int kind, const char *key, double db, int mute, int *muted) {
    if (kind == NK_AIRPLAY) {
        pthread_mutex_lock(&rlk);
        const int ri = find_rcv(NK_AIRPLAY, key), fd = slim_dup(ri, NULL);
        pthread_mutex_unlock(&rlk);
        if (fd < 0) return -1;
        const int k = mute ? 0 : dv_lms_index(db);
        uint8_t p[18];
        put32be(p, (uint32_t)k); put32be(p + 4, (uint32_t)k); p[8] = 1; p[9] = 255; put32be(p + 10, 0x10000); put32be(p + 14, 0x10000);
        const int r = slim_send(fd, "audg", p, sizeof p);
        close(fd);
        *muted = mute;
        return r < 0 ? -1 : 0;
    }
    pthread_mutex_lock(&rlk);
    const int ri = find_rcv(NK_UPNP, key);
    pthread_mutex_unlock(&rlk);
    if (ri < 0) return -1;
    char a[160];
    if (mute != *muted) {
        snprintf(a, sizeof a, "<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredMute>%d</DesiredMute>", mute);
        if (soap_svc(ri, 1, "SetMute", a, NULL, 0)) return -1;
        *muted = mute;
    }
    if (mute) return 0;
    snprintf(a, sizeof a, "<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredVolume>%.0f</DesiredVolume>", round(dv_pct_of(db)));
    return soap_svc(ri, 1, "SetVolume", a, NULL, 0);
}
static void *dv_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&dvlk);
    for (;;) {
        const int climbing = !isnan(DV.cur) && DV.has && DV.want > DV.cur + 0.05 && DV.want > -100 && !strcmp(DV.spec, DV.cur_spec);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        const long ns = (long)((climbing ? DV_STEP_S : 1.0) * 1e9);
        ts.tv_nsec += ns; ts.tv_sec += ts.tv_nsec / 1000000000L; ts.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&dvcv, &dvlk, &ts);
        if (!DV.has) continue;
        char spec[160]; snprintf(spec, sizeof spec, "%s", DV.spec);
        double want = DV.want;
        const int fresh = strcmp(spec, DV.cur_spec) || DV.gen != DV.cur_gen;
        const double umax = DV.max, start0 = DV.start;
        const unsigned gen = DV.gen, stms0 = DV.stms;
        double cur = fresh ? NAN : DV.cur;
        int muted = fresh ? 0 : DV.muted;
        pthread_mutex_unlock(&dvlk);
        const char *key;
        const int kind = spec_kind(spec, &key);
        if (kind != NK_AIRPLAY && kind != NK_UPNP) { pthread_mutex_lock(&dvlk); DV.has = 0; continue; }
        unsigned stms = 0;
        if (kind == NK_AIRPLAY) {
            pthread_mutex_lock(&rlk);
            const int ri = find_rcv(NK_AIRPLAY, key);
            stms = ri >= 0 ? R[ri].n_stms : 0;
            pthread_mutex_unlock(&rlk);
        }
        if (fresh && kind == NK_UPNP) {                 /* where it is: its own volume, read */
            pthread_mutex_lock(&rlk);
            const int ri = find_rcv(NK_UPNP, key);
            pthread_mutex_unlock(&rlk);
            char r[4096], v[16];
            if (ri >= 0 && !soap_svc(ri, 1, "GetVolume", "<InstanceID>0</InstanceID><Channel>Master</Channel>", r, sizeof r) && xml_text(r, "CurrentVolume", v, sizeof v))
                cur = dv_db_of(atof(v));
            if (ri >= 0 && !soap_svc(ri, 1, "GetMute", "<InstanceID>0</InstanceID><Channel>Master</Channel>", r, sizeof r) && xml_text(r, "CurrentMute", v, sizeof v))
                muted = atoi(v) != 0 || !strcasecmp(v, "true");
            pthread_mutex_lock(&dvlk); DV.start = isnan(cur) ? NAN : cur; pthread_mutex_unlock(&dvlk);
            if (isnan(cur)) cur = -31;                  /* not read: from the bottom */
            nlog("device volume: \"%s\" at %.1f dB (%.0f %%)%s, the sender asks %.1f dB", key, cur, dv_pct_of(cur), muted ? ", muted" : "", want);
        } else if (fresh) {
            cur = dv_mem_get(spec);
            pthread_mutex_lock(&dvlk); DV.start = cur; pthread_mutex_unlock(&dvlk);
            nlog("device volume: \"%s\" (AirPlay), %s, the sender asks %.1f dB", key, isnan(cur) ? "its volume not known: from the bottom (-30 dB) up" : "at what this player last set", want);
            if (isnan(cur)) cur = -31;                  /* below the bottom: −30 set first */
        }
        const double start = fresh ? (kind == NK_UPNP && cur >= -30 ? cur : kind == NK_AIRPLAY ? dv_mem_get(spec) : NAN) : start0;
        const double cap = !isnan(umax) ? umax : start;   /* the ceiling */
        if (isnan(cap)) {                               /* its volume not known, no ceiling set: never touched */
            if (fresh) nlog("device volume: \"%s\" not touched: its volume not known and no maximum set", key);
            pthread_mutex_lock(&dvlk);
            DV.cur = cur; DV.muted = muted; snprintf(DV.cur_spec, sizeof DV.cur_spec, "%s", spec); DV.cur_gen = gen; DV.stms = stms;
            snprintf(DV.said, sizeof DV.said, "not touched (no maximum)");
            continue;
        }
        if (want > -100 && want > cap) want = cap;
        const int mute = want <= -100;
        const int again = kind == NK_AIRPLAY && !fresh && stms != stms0;   /* a new RAOP connection: told again */
        double to = want;
        if (!mute && want > cur + 0.05) to = fmin(want, fmax(cur, -30 - DV_UP_DB) + DV_UP_DB);   /* up: one step */
        if (to < -30 && !mute) to = -30;
        int moved = 0;
        if (mute != muted || fabs(to - cur) > 0.05 || again) {
            if (!dv_apply(kind, key, to, mute, &muted)) {
                moved = 1;
                if (!mute) cur = to;
                if (kind == NK_AIRPLAY && !mute) dv_mem_set(spec, cur);
            } else nlog("device volume: \"%s\" not set (%.1f dB)", key, to);
        }
        pthread_mutex_lock(&dvlk);
        DV.cur = cur; DV.muted = muted; snprintf(DV.cur_spec, sizeof DV.cur_spec, "%s", spec); DV.cur_gen = gen; DV.stms = stms;
        if (moved) snprintf(DV.said, sizeof DV.said, "%s %.1f dB%s", mute ? "muted," : "at", mute ? want : cur, !mute && want > cur + 0.05 ? " (going up)" : "");
        if (moved && (mute || fabs(cur - want) <= 0.05)) nlog("device volume: \"%s\" %s %.1f dB (the sender's)", key, mute ? "muted," : "at", mute ? want : cur);
    }
    return NULL;
}
static void dv_start(void) {
    pthread_t th;
    if (!DV.started && !pthread_create(&th, NULL, dv_main, NULL)) { pthread_detach(th); DV.started = 1; }
}

int netout_init(int http_port, int slim_port) {
    http_port_ = http_port;
    for (int i = 0; i < NRCV; i++) R[i].fd = -1;
    pthread_t th;
    int bad = 0;
    if ((http_fd = listen_on(http_port)) < 0) { nlog("port %d (the streams) refused: no network output", http_port); bad = 1; }
    else if (!pthread_create(&th, NULL, http_main, NULL)) pthread_detach(th);
    if (slim_port > 0) {
        if ((slim_fd = listen_on(slim_port)) < 0) { nlog("port %d (Squeezebox players) refused", slim_port); bad = 1; }
        else if (!pthread_create(&th, NULL, slim_main, NULL)) { pthread_detach(th); if (ap_on) disc_start(slim_port); }
    }
    if (!local_only && !pthread_create(&th, NULL, upnp_main, NULL)) pthread_detach(th);
    if (!pthread_create(&th, NULL, watch_main, NULL)) pthread_detach(th);
    if (!pthread_create(&th, NULL, az_browse_main, NULL)) pthread_detach(th);   /* audiolaz receivers (mDNS) */
    dv_start();                                      /* the sender's volume on the device */
    claims_start();                                   /* which receiver each audiolaz player plays to */
    if (!bad) nlog("network outputs: streams on port %d, Squeezebox players (and the AirPlay bridge's devices) on port %d, UPnP renderers searched every %.0f s", http_port, slim_port, SEARCH_S);
    return bad ? -1 : 0;
}

#include "nowplay.h"

#include "anet_send.c"
#include "anet_mdns.c"

/* ── audiolaz Link, protocol 3 (anet.h): the source's kind, the stop's reason, its state for the page and the
 * lip sync, what plays ───────────────────────────────────────────────────────────────────────────────────────── */
_Static_assert((int)NETOUT_WHY_END == (int)ANET_WHY_END && (int)NETOUT_WHY_USER == (int)ANET_WHY_USER && (int)NETOUT_WHY_RATE == (int)ANET_WHY_RATE
               && (int)NETOUT_WHY_SETTINGS == (int)ANET_WHY_SETTINGS && (int)NETOUT_WHY_SOURCE == (int)ANET_WHY_SOURCE && (int)NETOUT_WHY_LOST == (int)ANET_WHY_LOST
               && (int)NETOUT_WHY_QUIT == (int)ANET_WHY_QUIT && (int)NETOUT_WHY_PAUSE == (int)ANET_WHY_PAUSE && (int)NETOUT_WHY_OUTPUT == (int)ANET_WHY_OUTPUT, "netout.h's reasons are anet.h's");
void netout_live(int on) { anet_live(on); }
void netout_paced(int on) { anet_paced(on); }   /* */
long netout_link_gaps(void) {
    anet_state s;
    if (!AZ || anet_get_state(NULL, &s) || !s.on) return 0;
    return (long)s.underruns;
}

static double as_dac_avg(double *ppm) {
    pthread_mutex_lock(&as_lk);
    const double secs = AS.on && AS.kp_n ? (double)AS.kp_n * (AS.pkt_us / 1e6) : 0;
    if (secs > 0) *ppm = (AS.k_sum / (double)AS.kp_n - 1.0) * 1e6;
    pthread_mutex_unlock(&as_lk);
    return secs;
}
int netout_link_pace(double *dac_ppm, double *dac_avg_ppm, double *avg_s, double *pull_ppm, double *ring_ms, double *target_ms, double *anchor_ms) {
    anet_state s;
    if (!AZ || anet_get_state(NULL, &s) || !s.on || !s.adapt) return -1;
    *dac_ppm = s.dac_ppm; *pull_ppm = s.pull_ppm; *ring_ms = s.ring_ms; *target_ms = s.target_ms; *anchor_ms = s.anchor_ms;
    *dac_avg_ppm = 0; *avg_s = as_dac_avg(dac_avg_ppm);   /* */
    return 0;
}
void netout_fifo_ms(int ms) { anet_fifo_ms(ms); }
void netout_rt(int prio) { anet_rt(prio); }   /* */
void netout_stop_reason(int why, const char *text) { anet_stop_reason((unsigned)why, text); }
/* host:port of an audiolaz out= (its receiver as listed, else the address it names); "" for another kind */
static void az_hostport(const char *spec, char *hp, size_t len) {
    const char *key;
    hp[0] = 0;
    if (!spec || spec_kind(spec, &key) != NK_AUDIOLAZ) return;
    pthread_mutex_lock(&rlk);
    const int ri = find_rcv(NK_AUDIOLAZ, key);
    if (ri >= 0) snprintf(hp, len, "%s", R[ri].loc);
    pthread_mutex_unlock(&rlk);
    if (!hp[0]) snprintf(hp, len, "%s%s", key, strchr(key, ':') ? "" : ":4890");
}
double netout_link_extra_ms_for(const char *spec) {
    char hp[200];
    az_hostport(spec, hp, sizeof hp);
    anet_state s;
    if (!hp[0] || anet_get_state(hp, &s)) return 0;
    const double top = s.on && s.anchor_ms > s.target_ms ? s.anchor_ms : s.target_ms;
    return top > s.start_ms && s.start_ms > 0 ? top - s.start_ms : 0;
}
double netout_link_extra_ms(void) {
    char sp[160];
    pthread_mutex_lock(&slk); snprintf(sp, sizeof sp, "%s", spec_now); pthread_mutex_unlock(&slk);
    return netout_link_extra_ms_for(sp);
}
int netout_link_json(const char *spec, char *out, size_t len) {
    char hp[200], dac[96] = "", rid[64] = "", rates[96] = "";
    az_hostport(spec, hp, sizeof hp);
    anet_state s;
    if (!hp[0] || anet_get_state(hp, &s)) return snprintf(out, len, "null");
    double dac_shown = s.dac_ppm;                     /* dacPpm: its average this stream once known (as_dac_avg) */
    if (s.on) (void)as_dac_avg(&dac_shown);
    {   /* the DAC's name and the receiver's id as it announces them */
        const char *key;
        if (spec_kind(spec, &key) == NK_AUDIOLAZ) {
            pthread_mutex_lock(&rlk);
            const int ri = find_rcv(NK_AUDIOLAZ, key);
            if (ri >= 0) { snprintf(dac, sizeof dac, "%s", R[ri].model); snprintf(rid, sizeof rid, "%s", R[ri].id); snprintf(rates, sizeof rates, "%s", R[ri].rates); }
            pthread_mutex_unlock(&rlk);
        }
        for (char *p = dac; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
        for (char *p = rid; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
        for (char *p = rates; *p; p++) if (!(*p >= '0' && *p <= '9') && *p != ',') *p = ' ';
        for (char *p = s.last_stop; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
    }
    return snprintf(out, len,
        "{\"id\":\"%s\",\"dac\":\"%s\",\"playing\":%s,\"protocol\":%d,\"adaptive\":%s,\"live\":%s,\"rate\":%u,\"state\":\"%s\","
        "\"reserveMs\":%.0f,\"targetMs\":%.0f,\"startMs\":%.0f,\"targetLowMs\":%.0f,\"targetHighMs\":%.0f,\"latencyMs\":%.0f,\"silenceInMs\":%.0f,"
        "\"jitterMs\":%.1f,\"jitterPeakMs\":%.1f,\"underruns\":%lu,\"adjustments\":%lu,\"senderLate\":%lu,\"senderLateMs\":%.0f,\"lastStop\":\"%s\",\"lastStopAgoS\":%.0f,\"afterMs\":%.0f,"
        "\"rates\":\"%s\",\"convertedFrom\":%u,\"dacStart\":%s,\"dacPpm\":%.1f,\"pullPpm\":%.1f,\"leftOverMs\":%.0f,\"putBackMs\":%.0f}",
        rid, dac, s.on ? "true" : "false", s.on ? (s.p3 ? 3 : 2) : 0, s.adapt ? "true" : "false", s.live ? "true" : "false", s.rate,
        anet_rs_text(s.rstate), s.on ? s.ring_ms : 0, s.target_ms, s.start_ms, s.lo_ms, s.hi_ms, s.on ? s.held_ms : 0, s.on ? s.ins_ms : 0,
        s.late_ms, s.peak_ms, s.underruns, s.adjusts, s.tx_late, s.tx_late_ms, s.last_stop, s.last_stop_age, s.on ? s.after_ms : 0,   /* afterMs: in latencyMs too */
        rates, AZ && AZC.s ? AZC.rate : 0u, netout_dac_start(spec) ? "true" : "false", s.on ? dac_shown : 0, s.on ? s.pull_ppm : 0, s.on ? s.over_ms : 0, s.on ? s.back_ms : 0);   /* its DAC's rates, the stream's own rate when converted, its DAC refusing to start */
}
/* under nplk (np_bump): what the source playing says, to the audiolaz receiver (its cover by our stream server's URL,
 * {self}: the address the receiver reaches us at, filled in by anet_send.c) */

static void np_tell_link(void) {
    char m[ANET_META_MAX] = "";
    if (FL.on) snprintf(m, sizeof m, "title=audiolaz \xc2\xb7 %s\nartist=%s\n", FL.title, FL.sub);
    else if (np_act >= 0 && NP[np_act].has) {
        const np_t *n = &NP[np_act];
        int k = snprintf(m, sizeof m, "source=%s\ntitle=%s\nartist=%s\nalbum=%s\n", NP_SRC[np_act], n->title, n->artist, n->album);
        if (n->img && k > 0 && (size_t)k < sizeof m) snprintf(m + k, sizeof m - (size_t)k, "cover=http://{self}:%d/cover/%u.jpg\n", http_port_, np_ser);
    } else if (NPI_t[0]) snprintf(m, sizeof m, "title=%s\nartist=%s\n", NPI_t, NPI_s);
    anet_meta(m, np_ser);
}
