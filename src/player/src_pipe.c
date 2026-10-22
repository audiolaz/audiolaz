/* src/player/src_pipe.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <pthread/spawn.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/proc_info.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "src_plat.h"   /* src.h's calls as plat_src_*: src_upnp.c hands each input to its own */
#include "src.h"
#include "audiolaz_drv.h"

extern char **environ;

static void slog(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); fflush(stderr);
}
static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

#define IN_GAP_S 0.5                   /* no data that long: the source stopped (when no metadata said it at once) */
#define IN_CLOSED_S 1.0                /* … and its side closed (no AirPlay session) */
#define IN_IGNORE_S 0.15               /* after the sender's pause or stop: what still comes dropped that long */
#define IN_LEAD_S 2.0                  /* a play's first seconds: its timeline settling (take), no silence put in (src_main) */
#define IN_BURST_S 0.1                 /* what comes in its first 0.1 s (the lead-in, written at once) beyond … */
#define IN_LEAD_SLACK_S 0.03           /* … the frames' own lead (early) and this much: dropped (take) */

#define IN_STEP_S 0.012                /* the source's timeline stepping by more than this, for IN_RECENT_S: put right (take) */
#define IN_HIST 1024                   /* its reads' lateness kept (shairport-sync writes 125 packets a second) */
#define IN_RECENT_S 1.0                /* … the lowest over the last second against … */
#define IN_BASE_S 3.0                  /* … the lowest over the 2 s before */
#define IN_FILL_S 0.15                 /* nothing that long past its time: silence in its place (src_main), so that the ring
                                        * (250 ms for video at least) never runs dry for it; the music that comes after
                                        * all (late, not lost: shairport-sync puts silence itself where packets are lost)
                                        * then has as much of its start dropped at once (take: s->debt), the timeline as
                                        * before. 0.05 s put silence in on every burst a Buffered stream makes */
#define IN_DEBT_MAX_S 1.0              /* silence put in owed back at most this much */

#define AIR_D_S 0.3
#define RING_FRAMES (1u << 17)         /* what waits to be read: 3 s at 44.1 kHz (a play that stops reading: an overrun) */
#define AIR_K 1                        /* the AirPlay input's number (Linux: install.sh's sub_of airplay) */
#define AIR_CH 8                       /* its pipe's channels, always (above) */
#define LAY_TH (1 << 12)               /* a sample past this (-114 dBFS) on a channel beyond the stream's: music there */
#define LAY_CLAIM_S 2.0                /* a wider layout said, not heard: taken that long after */
#define LAY_DOWN_S 0.3                 /* a narrower one said: taken once the channels beyond it are silent that long */
#define LAY_START_S 1.0                /* … or at once in a play's first second (said as it starts: its own) */

/* ── Options (src_option) ──────────────────────────────────────────────────────────────────────────────────────── */
static int air_on = 1;                 /* --airplay-in on|off */
static char air_bin[PATH_MAX] = "";    /* --airplay-in-bin: where audiolaz-airplay2-in and audiolaz-ptp are */
static int air_port = 7110;            /* --airplay-in-port: its RTSP port (macOS's own receiver holds 5000 and 7000) */
static int air_udp = 0;                /* --airplay-in-udp: its UDP ports' base (100 of them); 0: 6301 for 7110, else port + 100 */
static char air_if[32] = "";           /* --airplay-in-interface: announced there only ("lo0": this Mac alone, the tests) */
static int air_ptp = 1;                /* --airplay-in-ptp auto|off: AirPlay 2's clock when its ports are free; off: classic */
static double fake_ppm = 0;            /* --in-fake-ppm N (the tests): the sources' clocks N ppm fast */
static double gap_s = IN_GAP_S;        /* --in-gap-ms */
#define DRV_ZERO_S 15.0                /* the driver's input: this much digital silence, the app paused */
static int drv_k = -1;
static char dump_path[PATH_MAX] = "";  /* --in-dump <file> (the tests): when each click came in */
static char *pipe_arg[SRC_MAX];        /* --in-pipe k=<path> */

int src_option(const char *name, const char *value) {
    if (!strcmp(name, "--airplay-in")) air_on = strcmp(value, "off") != 0;
    else if (!strcmp(name, "--airplay-in-bin")) snprintf(air_bin, sizeof air_bin, "%s", value);
    else if (!strcmp(name, "--airplay-in-port")) air_port = atoi(value);
    else if (!strcmp(name, "--airplay-in-udp")) air_udp = atoi(value);
    else if (!strcmp(name, "--airplay-in-interface")) snprintf(air_if, sizeof air_if, "%s", value);
    else if (!strcmp(name, "--airplay-in-ptp")) air_ptp = strcmp(value, "off") != 0;
    else if (!strcmp(name, "--in-fake-ppm")) fake_ppm = atof(value);
    else if (!strcmp(name, "--in-gap-ms")) { gap_s = atof(value) / 1000.0; if (!(gap_s >= 0.05)) gap_s = 0.05; }
    else if (!strcmp(name, "--in-driver")) drv_k = strcmp(value, "off") && atoi(value) >= 0 && atoi(value) < SRC_MAX ? atoi(value) : -1;
    else if (!strcmp(name, "--in-dump")) snprintf(dump_path, sizeof dump_path, "%s", value);
    else if (!strcmp(name, "--in-pipe")) {
        const int k = atoi(value);
        const char *eq = strchr(value, '=');
        if (!eq || k < 0 || k >= SRC_MAX) return 0;
        free(pipe_arg[k]); pipe_arg[k] = strdup(eq + 1);
    }
    else return 0;
    return 1;
}

/* ── The inputs ───────────────────────────────────────────────────────────────────────────────────────────────── */
typedef struct {
    int used, air;                     /* air: the AirPlay receiver's stdout; else a FIFO */
    int drv;
    az_shm *shm; uint64_t dr; uint32_t dgen;
    _Atomic int64_t doff;              /* the driver's sample time of ring frame i: i + doff */
    unsigned cyc;                      /* its IO cycle last logged */
    double zero_since;                 /* digital silence since (0: music) */
    long dop_run, dop_off; int dop_on, dop_said; uint32_t dop_last;   /* drv_dop */
    char path[PATH_MAX];               /* the FIFO */
    _Atomic int fd;                    /* what is read (−1 none) */
    unsigned rate; int bytes;          /* its format: 2 (S16LE) or 4 (S32LE) bytes a sample */
    int ch;                            /* its channels a frame (the ring's too): a FIFO's .fmt (2 by default), AIR_CH */
    unsigned char carry[64]; int ncarry;
    /* the stream's layout in those channels (above): now (src_channels), said (sdsc; 0 not said), when said,
     * when its channels 3-6 and 7-8 last had music */
    _Atomic int lay, claim;
    _Atomic double t_claim;
    double t_hi6, t_hi8;
    /* what the source does (written by src_main, read by roomd's threads) */
    _Atomic int active, open;
    _Atomic double started, t_data;
    _Atomic int stop_req;              /* the sender's pause or stop (src_air_item), counted */
    int stop_seen; double ignore_until;
    /* the music, as it came (src_main writes, the play's thread reads) */
    int32_t *ring;
    _Atomic uint64_t w, r;
    _Atomic int overrun;
    _Atomic double steer_ppm;          /* src_shift's */
    int wake[2];                       /* the play's poll: data to read */
    _Atomic int armed;
    double cover;                      /* until when what came lasts (take) */
    double early;                      /* how long before its time the source writes each frame (s): AIR_D_S, 0 */
    uint64_t lead_dropped;             /* the lead-in silence dropped (frames) */
    uint64_t filled;                   /* silence put where the source had nothing in time (frames, src_main) */
    uint64_t debt, repaid;             /* of it, not yet dropped from the music that came after all; dropped */
    double t0f; uint64_t nf;           /* the source's timeline: when its frame 0 was, the frames on it so far (take) */
    struct { double t, late; } hist[IN_HIST];
    unsigned nh;
    unsigned long fixes;               /* its steps put right this play */
    double t_click;                    /* the tests' dump: the last click */
    float lvl;
} input;
static input in_[SRC_MAX];
static int SUBS = 1;
static char ROOMS[PATH_MAX] = ".", DIR_[PATH_MAX] = "";
static char what[PATH_MAX + 64] = "no local input";
static int ev[2] = { -1, -1 };        /* src_event_fds: a source started or stopped */
static int ctl[2] = { -1, -1 };       /* src_main woken: a restart asked */
static pthread_t th;
static _Atomic int quit_, started_th;
static FILE *dumpf;

static void go_active(input *s, int on, double now);
static void notify(void) { if (ev[1] >= 0) { const char b = 1; (void)!write(ev[1], &b, 1); } }
static void set_fl(int fd, int fl) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | fl); fcntl(fd, F_SETFD, FD_CLOEXEC); }

/* ── The AirPlay receiver ─────────────────────────────────────────────────────────────────────────────────────── */
static struct {
    char conf[PATH_MAX], meta[PATH_MAX], log[PATH_MAX], ptplog[PATH_MAX], recv[PATH_MAX], ptp[PATH_MAX];
    char name0[160];                   /* the name to start with (src_air_name: roomd.c's rule) */
    pid_t pid, ptp_pid;                /* their watchdogs (each its process group) */
    int life[2];                       /* the lifeline: the watchdogs read [0], roomd alone holds [1] */
    int classic, failures;
    double next_try, t_start;
    _Atomic int restart_req, session;
    int restart_done;
    char state[16], why[200];
    pthread_mutex_t mu;
} A = { .pid = 0, .ptp_pid = 0, .life = { -1, -1 }, .state = "off", .mu = PTHREAD_MUTEX_INITIALIZER };

static void air_state(const char *s, const char *fmt, ...) {
    char w[200] = "";
    if (fmt) { va_list a; va_start(a, fmt); vsnprintf(w, sizeof w, fmt, a); va_end(a); }
    pthread_mutex_lock(&A.mu);
    const int changed = strcmp(A.state, s) || strcmp(A.why, w);
    snprintf(A.state, sizeof A.state, "%s", s); snprintf(A.why, sizeof A.why, "%s", w);
    pthread_mutex_unlock(&A.mu);
    if (changed && w[0]) slog("AirPlay input: %s", w);
}

/* The whole line of key in airplay.conf (its first one; kept_only: one marked the player's), indented by two spaces,
 * into out; 0: none. */
static int conf_line(const char *buf, const char *key, int kept_only, char *out, size_t len) {
    const size_t kl = strlen(key);
    for (const char *l = buf; l && *l; ) {
        const char *e = strchr(l, '\n');
        const size_t ll = e ? (size_t)(e - l) : strlen(l);
        const char *k = l + strspn(l, " \t");
        const size_t kn = ll - (size_t)(k - l);
        if ((size_t)(k - l) < ll && !strncmp(k, key, kl) && (k[kl] == ' ' || k[kl] == '\t' || k[kl] == '=')) {
            char line[700];
            snprintf(line, sizeof line, "%.*s", (int)(kn < sizeof line - 1 ? kn : sizeof line - 1), k);
            if (!kept_only || strstr(line, "audiolaz: kept by the player")) { snprintf(out, len, "  %s\n", line); return 1; }
        }
        l = e ? e + 1 : NULL;
    }
    return 0;
}
/* A value of airplay.conf's line key (the first one, after "="): its text up to ';' (quotes taken off) into v. */
static int conf_get(const char *buf, const char *key, int kept_only, char *v, size_t vl) {
    const size_t kl = strlen(key);
    for (const char *l = buf; l && *l; ) {
        const char *e = strchr(l, '\n');
        const size_t ll = e ? (size_t)(e - l) : strlen(l);
        const char *k = l + strspn(l, " \t");
        if ((size_t)(k - l) < ll && !strncmp(k, key, kl) && (k[kl] == ' ' || k[kl] == '\t' || k[kl] == '=')) {
            char line[700];
            snprintf(line, sizeof line, "%.*s", (int)(ll < sizeof line - 1 ? ll : sizeof line - 1), l);
            if (!kept_only || strstr(line, "audiolaz: kept by the player") || strstr(line, "audiolaz: set on the player's page")) {
                const char *eq = strchr(line, '=');
                if (eq) {
                    eq++; eq += strspn(eq, " \t");
                    const char *q = eq;
                    if (*q == '"') { const char *q2 = strchr(q + 1, '"'); snprintf(v, vl, "%.*s", q2 ? (int)(q2 - q - 1) : 0, q + 1); }
                    else { size_t n = strcspn(q, ";"); while (n && (q[n - 1] == ' ' || q[n - 1] == '\t')) n--; snprintf(v, vl, "%.*s", (int)n, q); }
                    return 1;
                }
            }
        }
        l = e ? e + 1 : NULL;
    }
    return 0;
}

static int air_multi_setting(void) {
    char path[PATH_MAX + 16], line[256];
    snprintf(path, sizeof path, "%s/control", ROOMS);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int v = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "airplay_channels=", 17)) v = !strcmp(line + 17, "multi");
    }
    fclose(f);
    return v;
}

static int air_knows(const char *key, int *known) {   /* the receiver's program names this setting (read once) */
    if (*known >= 0) return *known;
    *known = 0;
    FILE *f = fopen(A.recv, "rb");
    if (!f) return *known;
    struct stat st;
    char *buf = !fstat(fileno(f), &st) && st.st_size > 0 && st.st_size < (64 << 20) ? malloc((size_t)st.st_size) : NULL;
    if (buf && fread(buf, 1, (size_t)st.st_size, f) == (size_t)st.st_size && memmem(buf, (size_t)st.st_size, key, strlen(key))) *known = 1;
    free(buf);
    fclose(f);
    return *known;
}
static int air_has_boff(void) { static int k = -1; return air_knows("buffered_latency_offset_in_seconds", &k); }
/* play, pause, next, previous from the app (engine/alsa/remote_in.h): a receiver built with
 * engine/linux/patches/shairport-sync-5.6.1-remote-pipe.patch reads them from <rooms>/airplay-remote */
static int air_has_remote(void) { static int k = -1; return air_knows("remote_control_pipe", &k); }
static int air_conf(void) {
    char old[16384] = "";
    FILE *f = fopen(A.conf, "r");
    if (f) { const size_t n = fread(old, 1, sizeof old - 1, f); old[n] = 0; fclose(f); }
    /* the player's lines as they are (roomd.c's air_write: their comments say how the value was made), else at first */
    char lname[760], loff[760], ldecl[760], liv[760];
    if (!conf_line(old, "name", 1, lname, sizeof lname)) {
        char nm[200];
        size_t w = 0;                                  /* nothing the line or shairport-sync's %-substitutions would break */
        for (const char *q = A.name0[0] ? A.name0 : "audiolaz"; *q && w + 1 < sizeof nm; q++)
            if ((unsigned char)*q >= 0x20 && *q != '"' && *q != '\\' && *q != '%') nm[w++] = *q;
        nm[w] = 0;
        snprintf(lname, sizeof lname, "  name = \"%s\"; // audiolaz: kept by the player (the output's name)\n", nm);
    }
    if (!conf_line(old, "audio_backend_latency_offset_in_seconds", 1, loff, sizeof loff))
        snprintf(loff, sizeof loff, "  audio_backend_latency_offset_in_seconds = 0.0; // audiolaz: kept by the player\n");
    if (!conf_line(old, "declared_output_latency_in_seconds", 1, ldecl, sizeof ldecl))
        snprintf(ldecl, sizeof ldecl, "  declared_output_latency_in_seconds = 0.0; // audiolaz: kept by the player\n");
    if (!conf_line(old, "ignore_volume_control", 0, liv, sizeof liv))
        snprintf(liv, sizeof liv, "  ignore_volume_control = \"no\"; // audiolaz: set on the player's page (yes: the sender's volume ignored)\n");

    char lboff[760] = "";
    if (air_has_boff() && !conf_line(old, "buffered_latency_offset_in_seconds", 1, lboff, sizeof lboff)) {
        const char *eq = strchr(loff, '=');
        snprintf(lboff, sizeof lboff, "  buffered_latency_offset_in_seconds = %.3f; // audiolaz: kept by the player\n", eq ? atof(eq + 1) : 0.0);
    }
    char lrem[760] = "";
    if (air_has_remote()) snprintf(lrem, sizeof lrem, "  remote_control_pipe = \"%s/airplay-remote\"; // the app's play, pause, next (remote_in.h)\n", ROOMS);
    char lprep[760] = "", lplist[760] = "";
    for (const char *l = old; *l; ) {
        const char *e = strchr(l, '\n');
        const size_t ll = e ? (size_t)(e - l) : strlen(l);
        const char *k = l + strspn(l, " \t");
        const size_t kn = ll - (size_t)(k - l), nk = kn < 700 ? kn : 700;
        if ((size_t)(k - l) < ll && !strncmp(k, "// audiolaz: prepared for \"", 27) && !lprep[0]) snprintf(lprep, sizeof lprep, "  %.*s\n", (int)nk, k);
        else if ((size_t)(k - l) < ll && !strncmp(k, "diagnostics", 11) && memmem(k, kn, "get_plist_metadata", 18) && memmem(k, kn, "audiolaz: kept by the player", 28) && !lplist[0])
            snprintf(lplist, sizeof lplist, "%.*s\n", (int)nk, k);
        l += ll + (e ? 1 : 0);
    }
    const int udp = air_udp > 0 ? air_udp : air_port == 7110 ? 6301 : air_port + 100;
    const int multi = air_multi_setting();
    const char *mw = multi ? "multi: 5.1 and 7.1 received, mixed down by the player" : "stereo: the sender mixes down";
    char ifl[96] = "";
    if (air_if[0]) snprintf(ifl, sizeof ifl, "  interface = \"%s\"; // announced there only (--airplay-in-interface)\n", air_if);
    char now[16384];
    snprintf(now, sizeof now,
        "// audiolaz player (roomd, macOS): its AirPlay input's settings (audiolaz-airplay2-in -c %s), as install.sh's on\n"
        "// Linux. The lines marked « kept by the player » are the player's (its latency, measured, plus a calibration; the\n"
        "// output's name; the page's choice): rewritten by it between sessions. The rest written at each start of the receiver.\n"
        "general =\n"
        "{\n"
        "%s"
        "  port = %d; // the receiver's RTSP port (roomd --airplay-in-port; macOS's own receiver holds 5000 and 7000)\n"
        "  udp_port_base = %d;\n"
        "  udp_port_range = 100;\n"
        "%s"
        "  output_backend = \"stdout\"; // read by the player (engine/alsa/src_pipe.c)\n"
        "  mdns_backend = \"dns-sd\";\n"
        "  audio_backend_buffer_desired_length_in_seconds = %.3f; // stdout: each frame written that long before its time (the player counts it)\n"

        "  six_channel_mode = \"%s\"; // audiolaz: AirPlay channels, the player's (%s)\n"
        "  eight_channel_mode = \"%s\"; // audiolaz: AirPlay channels, the player's (%s)\n"
        "  mixdown = \"%s\"; // audiolaz: AirPlay channels, the player's (%s)\n"
        "  output_channel_mapping = \"off\"; // audiolaz: AirPlay channels, the player's (FFmpeg's order, the stream's channels first)\n"
        "%s%s%s%s%s%s"
        "};\n"
        "sessioncontrol =\n"
        "{\n"
        "  session_timeout = 60;\n"
        "};\n"
        "stdout =\n"
        "{\n"
        "  output_rate = 44100; // 44.1 kHz end to end: AirPlay's own rate\n"
        "  output_format = \"S32_LE\";\n"
        "  output_channels = 8; // audiolaz: AirPlay channels, the player's (always 8: stereo on 1-2, 5.1 on 1-6, 7.1 on all, the rest silent)\n"
        "};\n"
        "metadata =\n"
        "{\n"
        "  enabled = \"yes\"; // the player reads who sends (an Apple TV: video, lip sync; the others: music), and what plays\n"
        "  include_cover_art = \"yes\";\n"
        "  pipe_name = \"%s\";\n"
        "};\n"
        "%s", A.conf, lname, air_port, udp, ifl, AIR_D_S, multi ? "on" : "off", mw, multi ? "on" : "off", mw,
        multi ? "off" : "stereo", multi ? "multi: none, the player mixes" : "stereo: anything wider folded by the receiver",
        loff, lprep, liv, ldecl, lboff, lrem, A.meta, lplist);
    if (!strcmp(old, now)) return 0;
    char tmp[PATH_MAX + 16];
    snprintf(tmp, sizeof tmp, "%s/.airplay.conf.in", ROOMS);
    if (!(f = fopen(tmp, "w"))) { slog("AirPlay input: %s not written (%s)", tmp, strerror(errno)); return -1; }
    fputs(now, f);
    if (fchmod(fileno(f), 0644) || fclose(f) || rename(tmp, A.conf)) { unlink(tmp); slog("AirPlay input: %s not written (%s)", A.conf, strerror(errno)); return -1; }
    return 0;
}

/* A port free (nobody bound to it): 1. Asked by binding it ourselves (no SO_REUSEADDR: a holder's bind refuses ours),
 * nothing sent. */
static int port_free(int type, int port) {
    const int s = socket(AF_INET, type, 0), on = 1;
    if (s < 0) return 0;
    /* TCP: a port in TIME_WAIT (the last session's) is free for a listener (shairport-sync sets SO_REUSEADDR too);
     * another listener on it is not */
    if (type == SOCK_STREAM) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    const int ok = bind(s, (struct sockaddr *)&a, sizeof a) == 0;
    close(s);
    return ok;
}

/* The watchdog: runs "$@" in the background, stops it when fd 3 (the lifeline) reads an end of file (roomd gone) or on
 * SIGTERM, SIGKILL 2 s after; exits with it. The process group is the watchdog's (POSIX_SPAWN_SETPGROUP): roomd stops
 * them all at once (killpg). */
static const char *WATCH =
    "trap 'kill -TERM $C 2>/dev/null' TERM INT HUP; "
    "\"$@\" 3<&- & C=$!; "
    "( read _ <&3; kill -TERM $C 2>/dev/null; sleep 2; kill -KILL $C 2>/dev/null ) >/dev/null 2>&1 & W=$!; "
    "exec 3<&-; "
    "while kill -0 $C 2>/dev/null; do wait $C; done; "
    "kill $W 2>/dev/null; exit 0";
static pid_t spawn(char *const argv[], int out_fd, const char *log) {
    if (A.life[1] < 0) {
        if (pipe(A.life)) return -1;
        fcntl(A.life[0], F_SETFD, FD_CLOEXEC); fcntl(A.life[1], F_SETFD, FD_CLOEXEC);
    }
    const int lf = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644), nul = open("/dev/null", O_RDONLY | O_CLOEXEC);
    char *args[24];
    int n = 0;
    args[n++] = "/bin/sh"; args[n++] = "-c"; args[n++] = (char *)WATCH; args[n++] = "audiolaz-in";
    for (int i = 0; argv[i] && n < 23; i++) args[n++] = argv[i];
    args[n] = NULL;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    posix_spawn_file_actions_adddup2(&fa, nul >= 0 ? nul : 0, 0);
    posix_spawn_file_actions_adddup2(&fa, out_fd >= 0 ? out_fd : lf, 1);
    posix_spawn_file_actions_adddup2(&fa, lf >= 0 ? lf : 2, 2);
    posix_spawn_file_actions_adddup2(&fa, A.life[0], 3);
    sigset_t none, def;
    sigemptyset(&none); sigemptyset(&def);
    sigaddset(&def, SIGPIPE); sigaddset(&def, SIGTERM); sigaddset(&def, SIGINT); sigaddset(&def, SIGHUP);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setpgroup(&at, 0);
    /* the receiver's timing is the music's: with the Mac busy (load 20 on 8 cores, the tests), at normal priority
     * it wrote late and dropped what was late, 100 to 350 ms at a time */
    posix_spawnattr_set_qos_class_np(&at, QOS_CLASS_USER_INTERACTIVE);
    /* nothing of roomd's but these four (its page's socket, Roon's…: never held by a child once roomd is gone) */
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_CLOEXEC_DEFAULT);
    pid_t pid = -1;
    const int e = posix_spawn(&pid, "/bin/sh", &fa, &at, args, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (lf >= 0) close(lf);
    if (nul >= 0) close(nul);
    if (e) { slog("AirPlay input: %s not started (%s)", argv[0], strerror(e)); return -1; }
    return pid;
}
/* A watchdog's group stopped (SIGTERM, SIGKILL after 1 s: shairport-sync 5.5.1 often hangs in its own shutdown, 10/10),
 * waited for. */
static void stop_group(pid_t *pid) {
    if (*pid <= 0) return;
    killpg(*pid, SIGTERM);
    int st;
    for (int i = 0; i < 20; i++) { if (waitpid(*pid, &st, WNOHANG) == *pid) break; usleep(50000); if (i == 19) { killpg(*pid, SIGKILL); waitpid(*pid, &st, 0); } }
    killpg(*pid, SIGKILL);                             /* anything of its group left */
    *pid = 0;
}
/* Its journal's last error line (why it stopped). */
static void last_error(const char *log, char *out, size_t len) {
    snprintf(out, len, "no message");
    FILE *f = fopen(log, "r");
    if (!f) return;
    char line[400];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strstr(line, "error") || strstr(line, "fatal") || strstr(line, "die") || strstr(line, "could not") || strstr(line, "conflict"))
            snprintf(out, len, "%.160s", line);
    }
    fclose(f);
}

/* AirPlay 2's clock: audiolaz-ptp (nqptp) on UDP 319/320. Taken by another program: classic AirPlay only. */
static void start_ptp(void) {
    int st;
    if (A.ptp_pid > 0 && waitpid(A.ptp_pid, &st, WNOHANG) == 0) { A.classic = 0; return; }
    A.ptp_pid = 0;
    if (!air_ptp) { A.classic = 1; return; }
    if (access(A.ptp, X_OK)) { A.classic = 1; slog("AirPlay input: no AirPlay 2 clock (%s): classic AirPlay only", A.ptp); return; }
    if (!port_free(SOCK_DGRAM, 319) || !port_free(SOCK_DGRAM, 320)) {
        A.classic = 1; slog("AirPlay input: AirPlay 2's clock impossible (UDP 319/320 already taken by another program): classic AirPlay only");
        return;
    }
    char *argv[] = { A.ptp, NULL };
    A.ptp_pid = spawn(argv, -1, A.ptplog);
    for (int i = 0; i < 10 && A.ptp_pid > 0; i++) {
        usleep(50000);
        if (waitpid(A.ptp_pid, &st, WNOHANG) == A.ptp_pid) {
            A.ptp_pid = 0;
            char w[200]; last_error(A.ptplog, w, sizeof w);
            slog("AirPlay input: AirPlay 2's clock stopped at once (%s): classic AirPlay only", w);
        }
    }
    A.classic = A.ptp_pid <= 0;
}

static void air_start(void) {
    input *s = &in_[AIR_K];
    if (!port_free(SOCK_STREAM, air_port)) {
        A.failures++;
        const double wait = A.failures < 6 ? (double)(1 << A.failures) : 60;
        A.next_try = mono() + wait;
        air_state("failed", "receiver impossible: TCP port %d already taken, trying again in %.0f s", air_port, wait);
        return;
    }
    start_ptp();
    if (air_conf()) { A.next_try = mono() + 5; return; }
    struct stat st;
    if (stat(A.meta, &st) || !S_ISFIFO(st.st_mode)) { unlink(A.meta); mkfifo(A.meta, 0600); }
    /* Its stdout: a local socket pair rather than a pipe : a pipe holds 16 to 64 KB on macOS, 46 to 186 ms of
     * S32 stereo; this side held up longer than that (the Mac busy), shairport-sync's write blocked, its frames came
     * late and it dropped some (seen: 100 to 350 ms missing at a play's start). 2 MB was 6 s of stereo; its 8 channels
     * take 4 times as much: 6 MB asked (4.4 s), less when the system's limit refuses it (2 MB: 1.5 s). */
    int p[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, p)) { A.next_try = mono() + 5; return; }
    for (int big = 6 << 20; big >= 2 << 20; big -= 1 << 20)
        if (!setsockopt(p[1], SOL_SOCKET, SO_SNDBUF, &big, sizeof big)) break;
    for (int big = 6 << 20; big >= 2 << 20; big -= 1 << 20)
        if (!setsockopt(p[0], SOL_SOCKET, SO_RCVBUF, &big, sizeof big)) break;
    shutdown(p[0], SHUT_WR); shutdown(p[1], SHUT_RD);
    set_fl(p[0], O_NONBLOCK);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    char *argv[8];
    int na = 0;
    argv[na++] = A.recv; argv[na++] = "-c"; argv[na++] = A.conf;
    if (A.classic) argv[na++] = "--service-type=classic";
    if (getenv("AUDIOLAZ_AIRPLAY_IN_LOG")) argv[na++] = "-vv";   /* its journal in detail (<rooms>/in/airplay.log) */
    argv[na] = NULL;
    A.pid = spawn(argv, p[1], A.log);
    close(p[1]);
    if (A.pid <= 0) { close(p[0]); A.failures++; A.next_try = mono() + 5; air_state("failed", "receiver not started"); return; }
    const int old = atomic_exchange(&s->fd, p[0]);
    if (old >= 0) close(old);
    s->ncarry = 0;
    A.t_start = mono();
    char nm[200] = "", buf[16384] = "";
    FILE *f = fopen(A.conf, "r");
    if (f) { const size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
    conf_get(buf, "name", 0, nm, sizeof nm);
    char off[32] = "0"; conf_get(buf, "audio_backend_latency_offset_in_seconds", 0, off, sizeof off);
    air_state(A.classic ? "classic" : "ready", NULL);
    slog("AirPlay input: receiver \"%s\" started (%s, RTSP port %d%s%s), latency offset %s s, %s", nm, A.classic ? "classic AirPlay" : "AirPlay 2",
         air_port, air_if[0] ? ", announced on " : "", air_if, off, A.conf);
}
/* Once in a while from src_main: the receiver started again when it stopped (by itself: after 2, 4 … 60 s; asked:
 * at once). */
static void air_supervise(void) {
    input *s = &in_[AIR_K];
    const double now = mono();
    const int req = atomic_load(&A.restart_req);
    if (A.pid > 0) {
        int st;
        if (req != A.restart_done) {                   /* asked (input_stop, or airplay.conf changed between sessions) */
            A.restart_done = req;
            stop_group(&A.pid);
            const int old = atomic_exchange(&s->fd, -1);
            if (old >= 0) close(old);
            atomic_store(&A.session, 0);
            go_active(s, 0, now);                      /* its session over: at once, not IN_GAP_S later */
            A.next_try = 0;
        } else if (waitpid(A.pid, &st, WNOHANG) == A.pid) {   /* stopped by itself */
            killpg(A.pid, SIGKILL);
            A.pid = 0;
            const int old = atomic_exchange(&s->fd, -1);
            if (old >= 0) close(old);
            atomic_store(&A.session, 0);
            go_active(s, 0, now);
            if (now - A.t_start > 60) A.failures = 0;
            A.failures++;
            const double wait = A.failures < 6 ? (double)(1 << A.failures) : 60;
            A.next_try = now + wait;
            char w[200]; last_error(A.log, w, sizeof w);
            air_state(A.failures >= 4 ? "failed" : "starting", "the receiver stopped (%s): started again in %.0f s", w, wait);
        } else {
            if (now - A.t_start > 60) A.failures = 0;
            if (A.ptp_pid > 0 && waitpid(A.ptp_pid, &st, WNOHANG) == A.ptp_pid) {   /* its clock gone: again, at its next start */
                A.ptp_pid = 0; slog("AirPlay input: AirPlay 2's clock stopped: started again with the receiver's next start");
            }
            return;
        }
    } else A.restart_done = req;
    if (now >= A.next_try) air_start();
}

/* ── The thread reading every input, and keeping the receiver ─────────────────────────────────────────────────── */
static void fifo_fmt(input *s) {
    char p[PATH_MAX + 8], line[128];
    snprintf(p, sizeof p, "%s.fmt", s->path);
    s->rate = 44100; s->bytes = 2;
    int ch = 2;
    FILE *f = fopen(p, "r");
    if (f) {
        if (fgets(line, sizeof line, f)) {
            const char *r = strstr(line, "rate="), *b = strstr(line, "bits="), *c = strstr(line, "channels=");
            if (r && atoi(r + 5) >= 8000) s->rate = (unsigned)atoi(r + 5);
            if (b) s->bytes = atoi(b + 5) == 32 ? 4 : 2;
            if (c && atoi(c + 9) >= 2 && atoi(c + 9) <= SRC_MAX_CH) ch = atoi(c + 9);   /* as a Loopback source opens it */
        }
        fclose(f);
    }
    if (ch != s->ch) {                                 /* the ring is the channels' (nothing in it yet: not opened before) */
        int32_t *r = calloc((size_t)ch * RING_FRAMES, sizeof(int32_t));
        if (r) { int32_t *o = s->ring; s->ring = r; s->ch = ch; free(o); }
    }
    atomic_store(&s->lay, s->ch);
}
/* a FIFO opened (both ends, so that a writer going away is no end of file) once it is there */
static void fifo_open(input *s) {
    struct stat st;
    if (atomic_load(&s->fd) >= 0 || stat(s->path, &st) || !S_ISFIFO(st.st_mode)) return;
    const int fd = open(s->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    fifo_fmt(s);
    atomic_store(&s->fd, fd);
    slog("input %d: %s (raw, %d channels, %d bits at %u Hz)", (int)(s - in_), s->path, s->ch, s->bytes * 8, s->rate);
}
static void go_active(input *s, int on, double now) {
    if (atomic_load(&s->active) == on) return;
    if (on) atomic_store(&s->started, now);
    atomic_store(&s->active, on);
    if (!on && s->early > 0 && (s->filled || s->fixes))   /* what its timing took, this play */
        slog("input %d: its source's timing kept: %.0f ms of silence where it had nothing in time (%.0f ms of the music that came after all dropped for it), %lu steps put right, %.0f ms of lead-in dropped",
             (int)(s - in_), 1000.0 * (double)s->filled / s->rate, 1000.0 * (double)s->repaid / s->rate, s->fixes, 1000.0 * (double)s->lead_dropped / s->rate);
    if (dumpf) { fprintf(dumpf, "%s %d %.6f\n", on ? "start" : "stop", (int)(s - in_), now); fflush(dumpf); }
    notify();
}
/* A sample as it came (S16LE or S32LE), as a 32-bit integer (full scale ±2^31). */
static inline int32_t smp(const unsigned char *p, int bytes) {
    return bytes == 4 ? (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) : (int32_t)((uint32_t)(p[0] | p[1] << 8) << 16);
}
static const char *lay_name(int ch) { return ch == 6 ? "5.1" : ch == 8 ? "7.1" : ch == 2 ? "stereo" : "other"; }
/* The AirPlay receiver's layout in its 8 channels (the header): n frames come at now, *starting: a play's
 * first. What its channels beyond the stream's carry (heard) against what shairport-sync said (claim). */
static void follow_layout(input *s, const unsigned char *b, size_t n, double now, int starting) {
    const int bpf = s->ch * s->bytes;
    int hi = 2;
    for (size_t i = 0; i < n && hi < 8; i++)
        for (int c = hi; c < s->ch; c++) {
            const int32_t v = smp(b + i * (size_t)bpf + (size_t)c * (size_t)s->bytes, s->bytes);
            if (v > LAY_TH || v < -LAY_TH) { hi = c >= 6 ? 8 : 6; if (hi == 8) break; c = 5; }
        }
    if (hi >= 6) s->t_hi6 = now;
    if (hi == 8) s->t_hi8 = now;
    const int claim = atomic_load(&s->claim), lay = atomic_load(&s->lay);
    int nl = lay;
    const char *why = "";
    if (starting) {                                    /* a play: as said (heard, when more) */
        nl = claim > hi ? claim : hi;
        why = claim ? (hi > claim ? "heard (more than said)" : "said by shairport-sync") : "heard (nothing said)";
    } else if (hi > lay) { nl = claim > hi ? claim : hi; why = "heard"; }
    else {
        /* a play's first second: what is said is its stream's own, said just after its first frames came (its
         * metadata on another pipe, racing them; shairport-sync's lead-in silence first): taken at once */
        const int early = now - atomic_load(&s->started) < LAY_START_S;
        const double quiet = now - (claim >= 6 ? s->t_hi8 : s->t_hi6);
        if (claim > lay && (early || now - atomic_load(&s->t_claim) >= LAY_CLAIM_S)) { nl = claim; why = early ? "said by shairport-sync" : "said, not heard yet"; }
        else if (claim && claim < lay && (quiet >= LAY_DOWN_S || (early && quiet > 1e8))) { nl = claim; why = early ? "said by shairport-sync" : "said, the other channels silent"; }
    }
    if (nl == lay && !starting) return;
    atomic_store(&s->lay, nl);
    if (nl != lay || starting)
        slog("input %d: AirPlay %s (%d channels: %s)%s", (int)(s - in_), lay_name(nl), nl, why, starting || nl == lay ? "" : ", the play starts again with them");
    if (dumpf) { fprintf(dumpf, "layout %d %d %.6f\n", (int)(s - in_), nl, now); fflush(dumpf); }
}
/* n frames read (in bytes b) at time now: into the ring when a play reads this input */
static void take(input *s, const unsigned char *b, size_t n, double now) {
    if (now < s->ignore_until) return;
    const int starting = !atomic_load(&s->active);
    if (s->air) {                                      /* its layout first: what a play opened now must take */
        if (starting) s->t_hi6 = s->t_hi8 = -1e9;
        follow_layout(s, b, n, now, starting);
    }
    if (starting) { go_active(s, 1, now); s->cover = now; s->t0f = now; s->nf = 0; s->nh = 0; s->fixes = 0; s->filled = 0; s->debt = 0; s->repaid = 0; s->lead_dropped = 0; }
    /* the silence src_main put in where this music was due, owed: as much of it dropped (counted on the timeline
     * already, with that silence) */
    if (s->debt && n) {
        const size_t k = s->debt < n ? (size_t)s->debt : n;
        b += k * (size_t)(s->ch * s->bytes); n -= k; s->debt -= k; s->repaid += k;
        if (dumpf) { fprintf(dumpf, "repay %d %.1f %.6f\n", (int)(s - in_), 1000.0 * (double)k / s->rate, now); fflush(dumpf); }
        if (!n) { atomic_store(&s->t_data, s->cover > now ? s->cover : now); return; }   /* it came: the source alive */
    }
    /* Until when what came lasts, at its pace from when it came (t_data: the source stopped once it is that long
     * past). shairport-sync's stdout backend writes a play's lead-in silence at once, up to its first frame's time
     * (0.7 s with pyatv's classic AirPlay, 10/10), then each frame AIR_D_S before its time: the lead-in beyond that
     * is dropped (what comes in the play's first IN_BURST_S only: that burst; 10/10, over its first 2 s, music that
     * came late then all at once was taken for it, 100 ms of it dropped), so that the ring starts with the music's own
     * timing, not 0.7 s late with a backlog the regulator would take minutes to bring back. */
    const double after = (s->cover > now ? s->cover : now) + (double)n / s->rate;
    if (s->early > 0 && now - atomic_load(&s->started) < IN_BURST_S && after - now > s->early + IN_LEAD_SLACK_S) {
        size_t k = (size_t)((after - now - s->early - IN_LEAD_SLACK_S) * s->rate);
        if (k > n) k = n;
        b += k * (size_t)(s->ch * s->bytes); n -= k; s->lead_dropped += k;
    }
    s->cover = after;
    atomic_store(&s->t_data, after);
    size_t zeros = 0;                                  /* silence put in before what came (below) */
    if (s->early > 0 && n > 0) {
        /* The source's own timeline kept : shairport-sync's stdout backend writes each frame at its time, but
         * where the sender's stream skips (pyatv's frames came 32 ms short now and then, 10/10, as the Swift player
         * saw) it writes nothing in their place, and where its timing jumps back it writes more at once: with ALSA (the
         * Loopback, Linux) it puts silence or stuffing there itself, keeping its output's buffer at its level; to a
         * pipe, the music just comes that much early or late from then on, the ring (and the lip sync) off by as much
         * for the minutes the clock regulator takes to bring it back. Each read's lateness against the timeline (the
         * end of what came against the frames so far at the rate): its lowest over the last IN_RECENT_S against its
         * lowest over the IN_BASE_S before, beyond IN_STEP_S: that much silence put in (late), or of what came dropped
         * (early), at once. The clocks' drift (600 ppm at most: 1.8 ms in 3 s) never reaches it; a read held up
         * (this thread late, or shairport-sync's writes late then caught up) neither: its last packet is on time. */
        /* the end of what came against the timeline: a packet released at its time ends one packet ahead of it,
         * whatever its size (a read that caught part of one is later still: never the lowest) */
        const double late = now - (s->t0f + (double)(s->nf + n) / s->rate);
        if (now - atomic_load(&s->started) < IN_LEAD_S) { s->t0f += late; s->nh = 0; }   /* the lead-in: not yet */
        else {
            s->hist[s->nh % IN_HIST].t = now; s->hist[s->nh % IN_HIST].late = late; s->nh++;
            if (dumpf) fprintf(dumpf, "late %.2f %zu %.6f\n", late * 1000, n, now);
            double base = 1e9, recent = 1e9, oldest = now;
            int nr = 0;
            for (unsigned i = 0; i < IN_HIST && i < s->nh; i++) {
                const double ht = s->hist[i].t, hl = s->hist[i].late;
                if (now - ht > IN_BASE_S) continue;
                if (ht < oldest) oldest = ht;
                if (now - ht <= IN_RECENT_S) { if (hl < recent) recent = hl; nr++; }
                else if (hl < base) base = hl;
            }
            const double d = recent - base;
            if (now - oldest >= IN_BASE_S - 0.1 && nr >= 10 && fabs(d) > IN_STEP_S) {
                double done;
                if (d > 0) { zeros = (size_t)lround(d * s->rate); s->nf += zeros; done = (double)zeros / s->rate; }
                else {
                    size_t k = (size_t)lround(-d * s->rate);
                    if (k > n) k = n;
                    b += k * (size_t)(s->ch * s->bytes); n -= k;
                    done = -(double)k / s->rate;
                }
                /* what came since the step, as it would have been: so that a step back later (late frames coming
                 * after all) is seen against the same level, and put right in its turn */
                for (unsigned i = 0; i < IN_HIST && i < s->nh; i++) if (now - s->hist[i].t <= IN_RECENT_S) s->hist[i].late -= done;
                s->fixes++;
                if (dumpf) { fprintf(dumpf, "fix %d %.1f %.6f\n", (int)(s - in_), d * 1000, now); fflush(dumpf); }
                if (s->fixes <= 20 || s->fixes % 100 == 0)
                    slog(d > 0 ? "input %d: %.0f ms missing from the source's stream (#%lu), as much silence in their place"
                               : "input %d: the source's stream %.0f ms early from now (#%lu), as much of it dropped",
                         (int)(s - in_), fabs(d) * 1000, s->fixes);
            }
        }
        s->nf += n;
    }
    if (dumpf) {                                       /* the tests: each click's own time (its place in what came) */
        /* on the source's smooth timeline: what was read last released now at its first frame's time (shairport-sync
         * writes each 352-frame packet then), what came before it that much earlier */
        const size_t blk = s->air && n > 352 ? 352 : n;
        for (size_t i = 0; i < n; i++) {
            const int32_t v = smp(b + i * (size_t)(s->ch * s->bytes), s->bytes);   /* its first channel */
            const float a = fabsf((float)v / 2147483648.0f);
            if (a > 0.003f && s->lvl <= 0.003f) {    /* −50 dBFS: a click at the sender's volume too */
                const double t = now + ((double)i - (double)(n - blk)) / s->rate;
                if (t - s->t_click > 0.3) { fprintf(dumpf, "click %d %.6f\n", (int)(s - in_), t); fflush(dumpf); }
                s->t_click = t;
            }
            s->lvl = a > 0.003f ? a : s->lvl * 0.9995f;
        }
    }
    if (!atomic_load(&s->open)) return;
    uint64_t w = atomic_load(&s->w);
    const uint64_t r = atomic_load(&s->r);
    if (w - r + zeros + n > RING_FRAMES) { atomic_store(&s->overrun, 1); return; }
    const int ch = s->ch;
    for (size_t i = 0; i < zeros; i++) memset(s->ring + (size_t)ch * ((w + i) & (RING_FRAMES - 1)), 0, sizeof(int32_t) * (size_t)ch);
    w += zeros;
    for (size_t i = 0; i < n; i++) {
        int32_t *d = s->ring + (size_t)ch * ((w + i) & (RING_FRAMES - 1));
        const unsigned char *f = b + i * (size_t)(ch * s->bytes);
        for (int c = 0; c < ch; c++) d[c] = smp(f + (size_t)c * (size_t)s->bytes, s->bytes);
    }
    atomic_store(&s->w, w + n);
    if (atomic_load(&s->armed) && w + n - r >= 64) { atomic_store(&s->armed, 0); const char c = 1; (void)!write(s->wake[1], &c, 1); }
}
/* its identity, latency and volume (audiolaz_drv.h, az_info), mapped once the driver made it (an older driver
 * has none: nothing shown, the volume full): the DAC and the latency roomd last gave (src_dac, src_latency) written
 * there at once, or as soon as it is mapped; its volume control read at each block (plat_src_volume) */
static _Atomic(az_info *) dinfo;
static pthread_mutex_t dinfo_lk = PTHREAD_MUTEX_INITIALIZER;
static struct { int set; char name[AZ_INFO_TEXT], maker[AZ_INFO_TEXT], model[AZ_INFO_TEXT]; uint32_t local; } dac_want;
static double lat_want_ms = -1;
static int lat_said_ms = -1;
static void dinfo_write_dac(az_info *i) {              /* under dinfo_lk: the seqlock, one writer */
    if (!dac_want.set) return;
    atomic_fetch_add_explicit(&i->seq, 1, memory_order_acq_rel);
    memcpy(i->name, dac_want.name, AZ_INFO_TEXT); memcpy(i->maker, dac_want.maker, AZ_INFO_TEXT); memcpy(i->model, dac_want.model, AZ_INFO_TEXT);
    i->local = dac_want.local;
    atomic_fetch_add_explicit(&i->seq, 1, memory_order_release);
}
static void dinfo_write_lat(az_info *i) {              /* under dinfo_lk */
    const unsigned r = drv_k >= 0 && drv_k < SRC_MAX ? in_[drv_k].rate : 0;
    if (lat_want_ms < 0 || r < 8000) return;
    atomic_store(&i->latency, (uint32_t)llround(lat_want_ms * r / 1000.0));
    if (abs((int)lrint(lat_want_ms) - lat_said_ms) >= 20) {
        lat_said_ms = (int)lrint(lat_want_ms);
        slog("« audiolaz »: its latency told to the apps, %d ms (%u frames at %u Hz)", lat_said_ms, atomic_load(&i->latency), r);
    }
}
static void dinfo_attach(void) {
    if (atomic_load(&dinfo)) return;
    const int fd = shm_open(az_info_name(), O_RDWR);
    if (fd < 0) return;
    struct stat st;
    void *m = fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(az_info) ? mmap(NULL, sizeof(az_info), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    if (m == MAP_FAILED) return;
    if (((az_info *)m)->magic != AZ_INFO_MAGIC) { munmap(m, sizeof(az_info)); return; }
    pthread_mutex_lock(&dinfo_lk);
    dinfo_write_dac(m); dinfo_write_lat(m);
    atomic_store(&dinfo, (az_info *)m);
    pthread_mutex_unlock(&dinfo_lk);
    const uint32_t c = atomic_load(&((az_info *)m)->vol_cdb);
    slog("« audiolaz »: its name, latency and volume shared (%s; its volume %s)", az_info_name(), c >= AZ_VOL_FLOOR_CDB ? "at the bottom, silent" : c ? "below full" : "full");
}
/* s into a field of AZ_INFO_TEXT bytes, cut at a character's start (the driver takes UTF-8 only) */
static void info_text(char *d, const char *s) {
    snprintf(d, AZ_INFO_TEXT, "%s", s ? s : "");
    size_t n = strlen(d);
    if (s && strlen(s) > n) while (n > 0 && ((unsigned char)d[n] & 0xC0) == 0x80) d[--n] = 0;   /* d[n] the cut's first byte */
}
void plat_src_dac(const char *name, const char *maker, const char *model, int local) {
    char n[AZ_INFO_TEXT], m[AZ_INFO_TEXT], u[AZ_INFO_TEXT];
    info_text(n, name); info_text(m, maker); info_text(u, model);
    pthread_mutex_lock(&dinfo_lk);
    /* maker and model not given (roomd's dac_tick: the name only): those of the same DAC kept */
    if (!maker && dac_want.set && !strcmp(n, dac_want.name)) memcpy(m, dac_want.maker, sizeof m);
    if (!model && dac_want.set && !strcmp(n, dac_want.name)) memcpy(u, dac_want.model, sizeof u);
    const int same = dac_want.set && !strcmp(n, dac_want.name) && !strcmp(m, dac_want.maker) && !strcmp(u, dac_want.model) && dac_want.local == (uint32_t)!!local;
    if (!same) {
        memcpy(dac_want.name, n, sizeof n); memcpy(dac_want.maker, m, sizeof m); memcpy(dac_want.model, u, sizeof u);
        dac_want.local = !!local; dac_want.set = 1;
        az_info *i = atomic_load(&dinfo);
        if (i) dinfo_write_dac(i);
        char sh[AZ_SHOWN_MAX];
        az_shown(sh, sizeof sh, n);                      /* the driver's rule, the Linux card's too */
        if (drv_k >= 0 && n[0]) slog("« audiolaz »: shown as « %s »%s%s, the DAC it plays to", sh, m[0] ? " by " : "", m);
        else if (drv_k >= 0) slog("« audiolaz »: shown as « audiolaz » (the DAC it plays to not known)");
    }
    pthread_mutex_unlock(&dinfo_lk);
}
void plat_src_hold(int k, int ms) { (void)k; (void)ms; }   /* the Linux card's alone */
void plat_src_latency(int k, double ms) {
    if (k != drv_k) return;
    pthread_mutex_lock(&dinfo_lk);
    lat_want_ms = ms;
    az_info *i = atomic_load(&dinfo);
    if (i) dinfo_write_lat(i);
    pthread_mutex_unlock(&dinfo_lk);
}

#define DOP_ON 64
#define DOP_OFF 4096
static void drv_dop(input *s, int32_t *pcm, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const uint32_t m = (uint32_t)pcm[2 * i] >> 24, m2 = (uint32_t)pcm[2 * i + 1] >> 24;
        const int mark = (m == 0x05 || m == 0xFA) && m2 == m && m != s->dop_last;
        s->dop_last = m;
        if (mark) { s->dop_run++; s->dop_off = 0; } else { s->dop_run = 0; s->dop_off++; }
        if (!s->dop_on && s->dop_run >= DOP_ON) {
            s->dop_on = 1;
            if (!s->dop_said) { s->dop_said = 1; slog("input %d: « audiolaz » receives DSD as DoP: silence (in Roon, the device's DSD playback: « Convert to PCM »)", (int)(s - in_)); }
        }
        if (s->dop_on && s->dop_off >= DOP_OFF) s->dop_on = 0;
    }
    if (s->dop_on) memset(pcm, 0, sizeof(int32_t) * 2 * n);
}

static void drv_attach(input *s) {
    dinfo_attach();                                    /* */
    if (s->shm) return;
    const int fd = shm_open(az_shm_name(), O_RDWR);
    if (fd < 0) return;
    struct stat st;
    void *m = fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(az_shm) ? mmap(NULL, sizeof(az_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    if (m == MAP_FAILED) return;
    if (((az_shm *)m)->magic != AZ_MAGIC) { munmap(m, sizeof(az_shm)); return; }
    s->shm = m;
    s->dgen = atomic_load(&s->shm->gen); s->dr = atomic_load(&s->shm->w);
    const unsigned r = atomic_load(&s->shm->rate);
    if (r >= 8000) s->rate = r;
    slog("input %d: the Mac's virtual output « audiolaz » (%s, float32 stereo at %u Hz)", (int)(s - in_), az_shm_name(), s->rate);
}
static void drv_read(input *s, double now, unsigned char *buf, size_t cap) {
    az_shm *m = s->shm;
    if (!m) return;
    const double ppm = atomic_load(&s->open) ? atomic_load(&s->steer_ppm) : 0;
    atomic_store(&m->ppb, (int64_t)llround(ppm * 1000.0));
    atomic_store(&m->beat, mach_absolute_time());
    const uint32_t gen = atomic_load(&m->gen), rate = atomic_load(&m->rate);
    if (gen != s->dgen || (rate >= 8000 && rate != s->rate)) {
        s->dgen = gen;
        if (rate >= 8000 && rate != s->rate) {
            if (atomic_load(&s->active)) go_active(s, 0, now);
            slog("input %d: « audiolaz » now at %u Hz", (int)(s - in_), rate);
            s->rate = rate;
            notify();
        }
    }
    const uint64_t w = atomic_load_explicit(&m->w, memory_order_acquire);
    if (w < s->dr) s->dr = 0;                          /* the IO started again (its sample time from 0) */
    if (w - s->dr > AZ_RING / 2) s->dr = w - AZ_RING / 4;   /* read too late: the oldest dropped */
    const size_t maxf = cap / (sizeof(int32_t) * AZ_CH);
    while (w > s->dr) {
        size_t n = (size_t)(w - s->dr);
        if (n > maxf) n = maxf;
        int32_t *o = (int32_t *)(void *)buf;
        const uint32_t fmt = atomic_load(&m->fmt);    /* the Mac's driver: float (AZ_F32) */
        for (size_t i = 0; i < n; i++) { o[2 * i] = az_get(m, fmt, s->dr + i, 0); o[2 * i + 1] = az_get(m, fmt, s->dr + i, 1); }
        atomic_store(&s->doff, (int64_t)s->dr - (int64_t)atomic_load(&s->w));
        s->dr += n;
        /* an app paused but holding its IO (Roon keeps the device, writing digital silence): stopped after
         * DRV_ZERO_S of it, so that roomd ends the play and releases the DAC as for any input; music again: a new play */
        int z = 1;
        for (size_t i = 0; i < 2 * n && z; i++) z = o[i] == 0;
        if (!z) s->zero_since = 0; else if (!s->zero_since) s->zero_since = now;
        if (z && now - s->zero_since >= DRV_ZERO_S) {
            if (atomic_load(&s->active)) { slog("input %d: « audiolaz »: %.0f s of digital silence (the app paused): stopped", (int)(s - in_), DRV_ZERO_S); go_active(s, 0, now); }
        } else { if (!z) drv_dop(s, o, n); take(s, buf, n, now); }
    }
    const unsigned cyc = atomic_load(&m->cycle);
    if (cyc && cyc != s->cyc) { s->cyc = cyc; slog("input %d: « audiolaz »: the apps' IO cycle %u frames (%.1f ms)", (int)(s - in_), cyc, 1000.0 * cyc / s->rate); }
    /* released at the device's pace (pump): the play woken as a capture card would, not once per IO cycle */
    if (atomic_load(&s->armed) && atomic_load(&s->open)) {
        const double dn = az_now(m, mach_absolute_time());
        const int64_t rel = (int64_t)dn - atomic_load(&s->doff);
        if (dn > 0 && rel - (int64_t)atomic_load(&s->r) >= 64) { atomic_store(&s->armed, 0); const char c = 1; (void)!write(s->wake[1], &c, 1); }
    }
    if (!atomic_load(&m->running) && atomic_load(&s->active)) go_active(s, 0, now);
}

static void *src_main(void *arg) {
    (void)arg;
    /* the music's way in, its timing measured here (take): as the audio's own threads (macOS's QoS classes) */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        const double ms = 1e6 * tb.denom / tb.numer;
        thread_time_constraint_policy_data_t p = { 0, (uint32_t)(2 * ms), (uint32_t)(10 * ms), 1 };
        if (thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p,
                              THREAD_TIME_CONSTRAINT_POLICY_COUNT) != KERN_SUCCESS)
            slog("inputs: their reader stays at its QoS (Mach time constraint refused)");
    }
    /* what waits is read whole (take's timeline: a read's last packet is the latest one written, on time): 64 KB was
     * 186 ms of stereo; the AirPlay pipe's 8 channels would have made it 46 ms, a read held up longer than that
     * cut in two, its first half taken for late music (silence put in, then as much dropped). 1 MB: 0.74 s of 8 channels */
    static unsigned char buf[(1 << 20) + 64];
    double t_sup = 0, t_fifo = 0;
    while (!atomic_load(&quit_)) {
        struct pollfd p[SRC_MAX + 1];
        int ks[SRC_MAX + 1], n = 0;
        p[n].fd = ctl[0]; p[n].events = POLLIN; ks[n++] = -1;
        for (int k = 0; k < SUBS; k++) {
            const int fd = atomic_load(&in_[k].fd);
            if (in_[k].used && fd >= 0) { p[n].fd = fd; p[n].events = POLLIN; ks[n++] = k; }
        }
        poll(p, (nfds_t)n, drv_k >= 0 ? 4 : 20);
        const double now = mono();
        if (drv_k >= 0 && drv_k < SUBS) drv_read(&in_[drv_k], now, buf, sizeof buf - 64);
        if (p[0].revents & POLLIN) { char c[64]; while (read(ctl[0], c, sizeof c) > 0) { } }
        for (int i = 1; i < n; i++) {
            if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            input *s = &in_[ks[i]];
            const int fd = p[i].fd;
            const int bpf = s->ch * s->bytes;
            memcpy(buf, s->carry, (size_t)s->ncarry);
            const ssize_t r = read(fd, buf + s->ncarry, sizeof buf - 64);
            if (r > 0) {
                const size_t all = (size_t)s->ncarry + (size_t)r, frames = all / (size_t)bpf;
                take(s, buf, frames, now);
                s->ncarry = (int)(all - frames * (size_t)bpf);
                memcpy(s->carry, buf + frames * (size_t)bpf, (size_t)s->ncarry);
            } else if (r == 0) {                       /* the receiver's stdout closed: it is stopping */
                if (atomic_compare_exchange_strong(&s->fd, &(int){ fd }, -1)) close(fd);
            }
        }
        for (int k = 0; k < SUBS; k++) {               /* stopped: the sender said so, or nothing came for a while */
            input *s = &in_[k];
            if (!s->used) continue;
            const int sr = atomic_load(&s->stop_req);
            if (sr != s->stop_seen) {
                s->stop_seen = sr;
                if (atomic_load(&s->active)) { s->ignore_until = now + IN_IGNORE_S; go_active(s, 0, now); }
            }
            if (atomic_load(&s->active) && now - atomic_load(&s->t_data) > gap_s) go_active(s, 0, now);
            /* Nothing from the receiver past its time (10/10, pyatv's stream from a busy Mac: 30 to 220 ms missing at
             * a time, now and then, never caught up): silence in its place as it goes, as shairport-sync gives its
             * ALSA output, so that the ring never runs dry for it (the ring refilled to its level after a gap, then
             * the music's own timing put back on top: 0.4 s of latency too much, 10/10). Should the music come after
             * all (late, not lost), as much of its start is dropped at once (take: s->debt, 11/10; before, a second
             * step had to be seen first: the ring off by the fill till then). */
            if (s->early > 0 && atomic_load(&s->active) && atomic_load(&s->open) && now - atomic_load(&s->started) >= IN_LEAD_S
                && now - s->cover > IN_FILL_S) {
                const size_t z = (size_t)((now - s->cover) * s->rate);
                const uint64_t w = atomic_load(&s->w), r = atomic_load(&s->r);
                if (w - r + z <= RING_FRAMES) {
                    for (size_t i = 0; i < z; i++) memset(s->ring + (size_t)s->ch * ((w + i) & (RING_FRAMES - 1)), 0, sizeof(int32_t) * (size_t)s->ch);
                    atomic_store(&s->w, w + z);
                }
                s->nf += z; s->cover += (double)z / s->rate; s->filled += z;
                s->debt += z;                          /* owed by the music that comes after all (take) */
                if (s->debt > (uint64_t)(IN_DEBT_MAX_S * s->rate)) s->debt = (uint64_t)(IN_DEBT_MAX_S * s->rate);
                if (dumpf) { fprintf(dumpf, "fill %d %.1f %.6f\n", k, 1000.0 * (double)z / s->rate, now); fflush(dumpf); }
            }
        }
        if (now - t_sup >= 0.1) { t_sup = now; if (in_[AIR_K].air && air_on) air_supervise(); }
        if (now - t_fifo >= 2.0) { t_fifo = now; for (int k = 0; k < SUBS; k++) if (in_[k].used && in_[k].drv) drv_attach(&in_[k]); else if (in_[k].used && !in_[k].air) fifo_open(&in_[k]); }
    }
    return NULL;
}

/* ── src.h ────────────────────────────────────────────────────────────────────────────────────────────────────── */
/* The receiver beside roomd's own executable (the app: both in Contents/Resources), unless --airplay-in-bin. */
static void bin_dir(void) {
    if (air_bin[0]) return;
    char p[PATH_MAX], r[PATH_MAX];
    uint32_t n = sizeof p;
    if (_NSGetExecutablePath(p, &n) || !realpath(p, r)) return;
    char *sl = strrchr(r, '/');
    if (sl) *sl = 0;
    snprintf(air_bin, sizeof air_bin, "%s", r);
}
/* <rooms>/inputs.conf: "1=airplay" (roomd.c's airplay_sub, the page), the other lines kept; none when off */
static void inputs_conf(int on) {
    char path[PATH_MAX + 16], tmp[PATH_MAX + 16], line[256], keep[4096] = "";
    snprintf(path, sizeof path, "%s/inputs.conf", ROOMS);
    int had = 0, hadm = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, "=airplay")) { had = 1; continue; }
            if (strstr(line, "=mac")) { hadm = 1; continue; }
            if (strlen(keep) + strlen(line) < sizeof keep) strcat(keep, line);
        }
        fclose(f);
    }
    if (had == on && hadm == (drv_k >= 0 && drv_k < SUBS)) return;
    snprintf(tmp, sizeof tmp, "%s/.inputs.conf", ROOMS);
    if (!(f = fopen(tmp, "w"))) return;
    if (!keep[0]) fputs("# audiolaz player (roomd, macOS): its inputs, for its page\n", f);
    fputs(keep, f);
    if (on) fprintf(f, "%d=airplay\n", AIR_K);
    if (drv_k >= 0 && drv_k < SUBS) fprintf(f, "%d=mac\n", drv_k);
    if (fclose(f) || rename(tmp, path)) unlink(tmp);
}

int src_init(const char *card, int subs, const char *rooms) {
    SUBS = subs < 1 ? 1 : subs > SRC_MAX ? SRC_MAX : subs;
    snprintf(ROOMS, sizeof ROOMS, "%s", rooms);
    snprintf(DIR_, sizeof DIR_, "%s", card && *card ? card : "");
    if (!DIR_[0]) snprintf(DIR_, sizeof DIR_, "%s/in", rooms);
    mkdir(DIR_, 0755);
    if (pipe(ev) || pipe(ctl)) return -1;
    set_fl(ev[0], O_NONBLOCK); set_fl(ev[1], O_NONBLOCK); set_fl(ctl[0], O_NONBLOCK); set_fl(ctl[1], O_NONBLOCK);
    if (dump_path[0]) dumpf = fopen(dump_path, "a");
    bin_dir();
    snprintf(A.recv, sizeof A.recv, "%s/audiolaz-airplay2-in", air_bin);
    snprintf(A.ptp, sizeof A.ptp, "%s/audiolaz-ptp", air_bin);
    const int air = air_on && SUBS > AIR_K && !access(A.recv, X_OK);
    if (air_on && SUBS <= AIR_K) slog("AirPlay input: off (--loop-subs %d: none for it)", SUBS);
    else if (air_on && !air) slog("AirPlay input: no receiver (%s): none", A.recv);
    air_on = air;
    for (int k = 0; k < SUBS; k++) {
        input *s = &in_[k];
        s->used = 1; s->fd = -1; s->rate = 44100; s->bytes = 2;
        s->ch = k == AIR_K && air ? AIR_CH : 2;       /* a FIFO's: its .fmt's (fifo_fmt) */
        s->lay = 2;
        s->ring = calloc((size_t)s->ch * RING_FRAMES, sizeof(int32_t));
        if (pipe(s->wake)) return -1;
        set_fl(s->wake[0], O_NONBLOCK); set_fl(s->wake[1], O_NONBLOCK);
        if (k == AIR_K && air) { s->air = 1; s->rate = 44100; s->bytes = 4; s->early = AIR_D_S; continue; }
        if (k == drv_k) { s->drv = 1; s->bytes = 4; s->armed = 1; drv_attach(s); continue; }
        if (pipe_arg[k]) snprintf(s->path, sizeof s->path, "%s", pipe_arg[k]);
        else snprintf(s->path, sizeof s->path, "%s/%d.pcm", DIR_, k);
        fifo_open(s);
    }
    inputs_conf(air);
    if (air) {
        snprintf(A.conf, sizeof A.conf, "%s/airplay.conf", ROOMS);
        if (!A.meta[0]) snprintf(A.meta, sizeof A.meta, "%s/airplay-meta", DIR_);
        snprintf(A.log, sizeof A.log, "%s/airplay.log", DIR_);
        snprintf(A.ptplog, sizeof A.ptplog, "%s/airplay-ptp.log", DIR_);
        air_conf();                                    /* there before roomd.c's airplay_tick first reads it */
        air_state("starting", NULL);
    }
    if (drv_k >= SUBS) slog("« audiolaz » input: off (--loop-subs %d: no input %d)", SUBS, drv_k);
    snprintf(what, sizeof what, "pipes in %s%s%s", DIR_, air ? ", the AirPlay input on input 1" : "", drv_k >= 0 && drv_k < SUBS ? ", the Mac's « audiolaz » output" : "");
    if (pthread_create(&th, NULL, src_main, NULL)) return -1;
    atomic_store(&started_th, 1);
    return 0;
}
const char *src_what(void) { return what; }
int src_active(int k) { return k >= 0 && k < SUBS && atomic_load(&in_[k].active); }
unsigned src_rate(int k) { return k >= 0 && k < SUBS ? in_[k].rate : 0; }
unsigned src_channels(int k) { return k >= 0 && k < SUBS ? (unsigned)atomic_load(&in_[k].lay) : 0; }   /* its layout now (src.h) */
double src_started(int k) { return k >= 0 && k < SUBS ? atomic_load(&in_[k].started) : 0; }
int src_paced(int k) { (void)k; return 0; }   /* src.h: the Mac's driver input is steered instead (src_shift) */
/* src.h: the driver's IO stopped by the HAL (StopIO: running 0), not a stall nor zeros */
int src_io_ended(int k) { return k >= 0 && k < SUBS && in_[k].used && in_[k].drv && in_[k].shm && !atomic_load(&in_[k].shm->running); }
int src_keeps(int k) { (void)k; return 0; }   /* src.h: none here (an uplink's ring is the box's) */
int src_closed(int k) {
    if (k < 0 || k >= SUBS || !in_[k].used) return 1;
    if (in_[k].air && atomic_load(&A.session)) return 0;
    return !atomic_load(&in_[k].active) && mono() - atomic_load(&in_[k].t_data) > IN_CLOSED_S;
}
int src_event_fds(struct pollfd *fds, int max) {
    if (ev[0] < 0 || max < 1) return 0;
    fds[0].fd = ev[0]; fds[0].events = POLLIN; fds[0].revents = 0;
    return 1;
}
void src_drain(void) { char b[64]; if (ev[0] >= 0) while (read(ev[0], b, sizeof b) > 0) { } }

void src_shift(int k, long v) { if (k >= 0 && k < SUBS) atomic_store(&in_[k].steer_ppm, (double)(v - 100000) * 11.0); }

/* An AirPlay sender connected to the receiver's RTSP port (port, 0: ours): a TCP connection of the receiver's process
 * group established on it (libproc: its sockets, no command run). */
int src_air_connected(int port) {
    if (A.pid <= 0) return 0;
    if (port <= 0) port = air_port;
    pid_t pids[16];
    const int np = proc_listpgrppids(A.pid, pids, sizeof pids);
    for (int i = 0; i < np && i < 16; i++) {
        struct proc_fdinfo fds[256];
        const int nb = proc_pidinfo(pids[i], PROC_PIDLISTFDS, 0, fds, sizeof fds);
        for (int j = 0; j < nb / (int)sizeof(struct proc_fdinfo); j++) {
            if (fds[j].proc_fdtype != PROX_FDTYPE_SOCKET) continue;
            struct socket_fdinfo si;
            if (proc_pidfdinfo(pids[i], fds[j].proc_fd, PROC_PIDFDSOCKETINFO, &si, sizeof si) < (int)sizeof si) continue;
            if (si.psi.soi_kind != SOCKINFO_TCP) continue;
            if (si.psi.soi_proto.pri_tcp.tcpsi_state == TSI_S_ESTABLISHED && ntohs((uint16_t)si.psi.soi_proto.pri_tcp.tcpsi_ini.insi_lport) == port) return 1;
        }
    }
    return 0;
}

/* The receiver is this layer's to stop (its session ended for the sender: started again). */
int src_owns(int k) { return k == AIR_K && in_[AIR_K].air && air_on; }
int src_stop(int k) {
    if (!src_owns(k)) return 0;
    atomic_fetch_add(&A.restart_req, 1);
    const char b = 1; (void)!write(ctl[1], &b, 1);
    return 1;
}
/* The metadata FIFO the receiver writes to (roomd.c reads it: air_meta_main): --airplay-meta's when given. */
const char *src_air_meta(const char *asked) {
    if (asked) snprintf(A.meta, sizeof A.meta, "%s", asked);
    return A.meta[0] && in_[AIR_K].air ? A.meta : NULL;
}
/* What the receiver's metadata says (ssnc codes, roomd.c's air_item): the sender's pause, stop or end cut at once */
void src_air_item(uint32_t code) {
    if (!in_[AIR_K].air) return;

    if (code == 0x70666c73u || code == 0x70656e64u || code == 0x64697363u || code == 0x70617573u) atomic_fetch_add(&in_[AIR_K].stop_req, 1);
    if (code == 0x636f6e6eu) { atomic_store(&A.session, 1); atomic_store(&in_[AIR_K].claim, 0); }                         /* conn: its format not said yet */
    if (code == 0x64697363u) { atomic_store(&A.session, 0); atomic_store(&in_[AIR_K].claim, 0); }                        /* disc: the next one says its own */
}

void src_air_format(const char *sdsc) {
    if (!in_[AIR_K].air || !sdsc) return;
    const char *sl = strrchr(sdsc, '/');
    const int c = !sl ? 0 : !strcmp(sl, "/7.1") ? 8 : !strcmp(sl, "/5.1") ? 6 : !strcmp(sl, "/2") || !strcmp(sl, "/1") ? 2 : 0;
    if (!c) { slog("AirPlay input: its stream's format « %s »: channels not known, followed by what they carry", sdsc); return; }
    if (atomic_exchange(&in_[AIR_K].claim, c) != c) atomic_store(&in_[AIR_K].t_claim, mono());
}
void src_air_name(const char *name) { snprintf(A.name0, sizeof A.name0, "%s", name ? name : ""); }
int src_status(char *out, size_t len) {
    if (!in_[AIR_K].air) return 0;
    pthread_mutex_lock(&A.mu);
    const int n = snprintf(out, len, "airplay_in=%s\nairplay_in_classic=%d\nairplay_in_why=%s\nairplay_in_port=%d\n",
                           atomic_load(&in_[AIR_K].active) ? "playing" : A.state, A.classic, A.why, air_port);
    pthread_mutex_unlock(&A.mu);
    return n > 0;
}
void src_tick(void) { }
void src_shutdown(void) {
    if (!atomic_load(&started_th)) return;
    atomic_store(&quit_, 1);
    pthread_join(th, NULL);
    stop_group(&A.pid);
    stop_group(&A.ptp_pid);
    if (dumpf) fclose(dumpf);
}

/* ── Reading one input: its frames resampled by the clock's ratio ─────────────────────────────────────────────────
 * A windowed sinc (Kaiser, β 9: ~90 dB), 2 × RS_H taps, RS_P phases interpolated linearly, in double precision: the
 * ratio within ±600 ppm of 1 (the regulator's limit), its passband to 20.5 kHz at 44.1 kHz, its delay RS_H frames
 * (2.9 ms, in src_delay). The ratio glides to the one asked (a 45 ms time constant): no step in the music's pitch.
 * Measured (test/mac-src-resample.c, 10/10): at ±605 ppm, a 997 Hz sine's error 123 dB down, 19 kHz's 110 dB (64 taps
 * a side: 114 and 109 dB; AirPlay's 16 bits are 98 dB); at the nominal ratio, 190 dB (the music as it came). */
#define RS_H 128
#define RS_P 512
static double *rs_tab;                 /* (RS_P + 1) rows of 2·RS_H */
static pthread_once_t rs_once = PTHREAD_ONCE_INIT;
static double bessel_i0(double x) { double s = 1, t = 1; for (int k = 1; k < 50; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; }
static void rs_init(void) {
    const double fc = 0.4775, beta = 9.0, i0b = bessel_i0(beta);
    rs_tab = malloc(sizeof(double) * (RS_P + 1) * 2 * RS_H);
    for (int p = 0; p <= RS_P; p++) {
        const double f = (double)p / RS_P;
        double *row = rs_tab + (size_t)p * 2 * RS_H, sum = 0;
        for (int j = 0; j < 2 * RS_H; j++) {
            const double t = (double)(j - (RS_H - 1)) - f, u = t / RS_H;
            const double sinc = fabs(t) < 1e-12 ? 1.0 : sin(M_PI * 2 * fc * t) / (M_PI * 2 * fc * t);
            const double w = fabs(u) >= 1 ? 0 : bessel_i0(beta * sqrt(1 - u * u)) / i0b;
            row[j] = 2 * fc * sinc * w; sum += row[j];
        }
        for (int j = 0; j < 2 * RS_H; j++) row[j] /= sum;   /* unity at DC, each phase */
    }
}

struct src_pcm {
    input *s;
    int ch;                            /* its channels (the input's first ch: its layout when opened, 10/10) */
    double *x; size_t nx, xcap;        /* the music in (ch channels, double), its first frame RS_H − 1 behind the next output */
    double pos;                        /* where the next output frame is, in x */
    double step, coef[2 * RS_H];
    int32_t *out; size_t no, ocap, oh; /* what is ready to read */
    unsigned long period;
};

src_pcm *src_open(int k, unsigned rate, unsigned ch, unsigned long period, unsigned periods) {
    /* 2 to its channels: the first ch of them (its layout's, src_channels) */
    if (k < 0 || k >= SUBS || !in_[k].used || rate != in_[k].rate || ch < 2 || (int)ch > in_[k].ch) return NULL;
    pthread_once(&rs_once, rs_init);
    src_pcm *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->s = &in_[k];
    p->ch = (int)ch;
    p->period = period;
    p->xcap = 16384 + 2 * RS_H;
    p->ocap = period * periods > 8192 ? period * periods : 8192;
    if (p->ocap > RING_FRAMES) p->ocap = RING_FRAMES;
    p->x = calloc((size_t)p->ch * p->xcap, sizeof(double));
    p->out = malloc(sizeof(int32_t) * (size_t)p->ch * p->ocap);
    if (!p->x || !p->out) { free(p->x); free(p->out); free(p); return NULL; }
    p->nx = RS_H - 1; p->pos = RS_H - 1;
    p->step = (1 + atomic_load(&p->s->steer_ppm) * 1e-6) / (1 + fake_ppm * 1e-6);
    return p;
}
/* What came before the play opened it dropped, as a Loopback's capture started now. */
void src_start(src_pcm *p) {
    input *s = p->s;
    atomic_store(&s->overrun, 0);
    atomic_store(&s->r, atomic_load(&s->w));
    atomic_store(&s->armed, 1);
    atomic_store(&s->open, 1);
}

static void pump(src_pcm *p) {
    input *s = p->s;
    const uint64_t w = atomic_load(&s->w);
    uint64_t r = atomic_load(&s->r);
    const int ch = p->ch;
    if (s->drv) {                                      /* its clock is the player's (the driver's): as it came, bit for bit */
        /* what the HAL wrote ahead (one IO cycle at a time, up to its buffer early) handed over at the device's own
         * pace: up to its sample time now, as a capture card's frames; the burst stays here (src_delay counts it), so
         * the player's ring, its regulator's measure, moves smoothly */
        uint64_t lim = w;
        if (s->shm && !getenv("AUDIOLAZ_NOPACE")) {   /* AUDIOLAZ_NOPACE=1: as before 12/10 (A/B) */
            const double dn = az_now(s->shm, mach_absolute_time());
            const int64_t rel = (int64_t)dn - atomic_load(&s->doff);
            if (dn > 0 && rel >= 0 && (uint64_t)rel < lim) lim = (uint64_t)rel;
        }
        while (r < lim && p->no < p->ocap) {
            memcpy(p->out + (size_t)ch * ((p->oh + p->no) % p->ocap), s->ring + (size_t)s->ch * (r & (RING_FRAMES - 1)), sizeof(int32_t) * (size_t)ch);
            p->no++; r++;
        }
        atomic_store(&s->r, r);
        if (p->no < p->period) atomic_store(&s->armed, 1);   /* src_read alone too: woken again by drv_read as frames are due, not at the next IO cycle */
        return;
    }
    size_t room = p->xcap - p->nx;
    while (r < w && room > 0) {
        const int32_t *d = s->ring + (size_t)s->ch * (r & (RING_FRAMES - 1));
        for (int c = 0; c < ch; c++) p->x[(size_t)ch * p->nx + (size_t)c] = d[c] * (1.0 / 2147483648.0);
        p->nx++; r++; room--;
    }
    atomic_store(&s->r, r);
    const double target = (1 + atomic_load(&s->steer_ppm) * 1e-6) / (1 + fake_ppm * 1e-6);
    while (p->no < p->ocap) {
        const double fl = floor(p->pos);
        const size_t i = (size_t)fl;
        if (i + RS_H >= p->nx) break;
        const double ph = (p->pos - fl) * RS_P;
        const int pi = (int)ph;
        const double fr = ph - pi;
        const double *a = rs_tab + (size_t)pi * 2 * RS_H, *b = a + 2 * RS_H;
        const double *xx = p->x + (size_t)ch * (i - (RS_H - 1));
        int32_t *o = p->out + (size_t)ch * ((p->oh + p->no) % p->ocap);
        if (ch == 2) {                                 /* stereo: as it always was, the same arithmetic */
            double l = 0, rr = 0;
            for (int j = 0; j < 2 * RS_H; j++) {
                const double c = a[j] + fr * (b[j] - a[j]);
                l += c * xx[2 * j]; rr += c * xx[2 * j + 1];
            }
            const double vl = l * 2147483648.0, vr = rr * 2147483648.0;
            o[0] = vl >= 2147483647.0 ? INT32_MAX : vl <= -2147483648.0 ? INT32_MIN : (int32_t)lrint(vl);
            o[1] = vr >= 2147483647.0 ? INT32_MAX : vr <= -2147483648.0 ? INT32_MIN : (int32_t)lrint(vr);
        } else {                                       /* 5.1, 7.1: each channel the same filter */
            double acc[SRC_MAX_CH] = { 0 };
            for (int j = 0; j < 2 * RS_H; j++) {
                const double c = a[j] + fr * (b[j] - a[j]);
                const double *xj = xx + (size_t)ch * (size_t)j;
                for (int k = 0; k < ch; k++) acc[k] += c * xj[k];
            }
            for (int k = 0; k < ch; k++) {
                const double v = acc[k] * 2147483648.0;
                o[k] = v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : (int32_t)lrint(v);
            }
        }
        p->no++;
        p->step += (target - p->step) * (1.0 / 2000);
        p->pos += p->step;
    }
    const size_t keep_from = (size_t)floor(p->pos) - (RS_H - 1);   /* what the next output still needs */
    if (keep_from > 0 && keep_from <= p->nx) {
        memmove(p->x, p->x + (size_t)ch * keep_from, sizeof(double) * (size_t)ch * (p->nx - keep_from));
        p->nx -= keep_from; p->pos -= (double)keep_from;
    }
}
long src_avail(src_pcm *p) {
    char b[64];
    while (read(p->s->wake[0], b, sizeof b) > 0) { }
    if (atomic_load(&p->s->overrun)) return -EPIPE;
    pump(p);
    if (p->no < p->period) atomic_store(&p->s->armed, 1);
    if (dumpf) {                                       /* the tests: what waits here, twice a second */
        static double t_last;
        const double t = mono();
        if (t - t_last >= 0.5) {
            t_last = t;
            fprintf(dumpf, "buf %.0f %.0f %llu %.3f %.6f\n", (double)p->no, (double)p->nx - p->pos,
                    (unsigned long long)(atomic_load(&p->s->w) - atomic_load(&p->s->r)), (p->step - 1) * 1e6, t);
            fflush(dumpf);
        }
    }
    return (long)p->no;
}
long src_read(src_pcm *p, int32_t *pcm, unsigned long n) {
    if (atomic_load(&p->s->overrun)) return -EPIPE;
    pump(p);
    if (!p->no) return SRC_AGAIN;
    const size_t k = n < p->no ? n : p->no;
    const int ch = p->ch;
    for (size_t i = 0; i < k; i++) { const int32_t *o = p->out + (size_t)ch * ((p->oh + i) % p->ocap); memcpy(pcm + (size_t)ch * i, o, sizeof(int32_t) * (size_t)ch); }
    p->oh = (p->oh + k) % p->ocap; p->no -= k;
    return (long)k;
}

int src_delay(src_pcm *p, long *frames) {
    input *s = p->s;
    int q = 0;
    const int fd = atomic_load(&s->fd);
    if (fd >= 0) ioctl(fd, FIONREAD, &q);
    if (s->drv && s->shm) { const uint64_t w = atomic_load(&s->shm->w); q = w > s->dr ? (int)((w - s->dr) * (uint64_t)(s->ch * s->bytes)) : 0; }
    *frames = (long)p->no + (long)((double)p->nx - p->pos) + (long)(atomic_load(&s->w) - atomic_load(&s->r)) + q / (s->ch * s->bytes)
              - lround(s->early * s->rate);
    return 0;
}
void src_recover(src_pcm *p, long err) {
    (void)err;
    p->no = 0; p->oh = 0; p->nx = RS_H - 1; p->pos = RS_H - 1;
    memset(p->x, 0, sizeof(double) * (size_t)p->ch * p->nx);
    src_start(p);
}
/* the input's layout no longer the one it was opened with (the AirPlay receiver's: the header) */
int src_relayout(src_pcm *p) { return p && p->s->air && atomic_load(&p->s->lay) != p->ch; }
int src_poll_fds(src_pcm *p, struct pollfd *fds, int max) {
    if (max < 1) return 0;
    fds[0].fd = p->s->wake[0]; fds[0].events = POLLIN; fds[0].revents = 0;
    return 1;
}
void src_close(src_pcm *p) {
    if (!p) return;
    atomic_store(&p->s->open, 0);
    free(p->x); free(p->out); free(p);
}
const char *src_strerror(long e) { return e == -EPIPE ? "overrun: read too late" : e == SRC_AGAIN ? "nothing to read" : strerror((int)(e < 0 ? -e : e)); }

double plat_src_volume(int k) {
    az_info *i = k == drv_k ? atomic_load(&dinfo) : NULL;
    if (!i) return 1.0;
    const uint32_t c = atomic_load(&i->vol_cdb);
    return c == 0 ? 1.0 : c >= AZ_VOL_FLOOR_CDB ? 0.0 : pow(10.0, -(double)c / 2000.0);
}
