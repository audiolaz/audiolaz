/* src/player/roomd.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef _DEFAULT_SOURCE   /* build.sh passes -D_DEFAULT_SOURCE */
#define _DEFAULT_SOURCE
#endif
#ifdef __APPLE__
#define st_mtim st_mtimespec            /* macOS's name for the same field */
#endif
#define ADJUST_S 10.0                   /* seconds of DAC fill averaged per clock correction */

#define RING_S 0.8
#define RING_ULTRA_S 0.55
#define RING_DIRECT_S 0.7
#define RING_START_S 0.3

#define TRIM_FLOOR 256                  /* one step of 24 bits, in the output's 32-bit words */
#define LOOKAHEAD_MS 20

#define PRIME_MAX_RATE 96000

#define CAP_PERIODS 96

static int rt_priority = 0;
static int writer_rt = 94;              /* --writer-rt N: the DAC's writer thread in SCHED_FIFO at N (0: normal) */
static int writer_cpu = -1;             /* --writer-cpu N : the writer pinned to CPU N (Linux); -1 (default): not */
static int mem_lock = 1;                /* --mlock 0|1 : mlockall at start, no page fault on the audio path */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/mman.h>
#include <sys/syscall.h>
#endif
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/event.h>   /* --parent-pid */
#endif
#include "player.h"      /* the free player: the music carried as it came, nothing of the correction */
#include "squeeze.h"
#include "pcmclock.h"
#include "memlock.h"     /* the player kept in memory */
#include "rt_thread.h"    /* the audio's threads' priority, macOS and Linux */
#include "rt_watch.h"     /* who held the cores, said after a wake late by seconds (Linux) */
#include "page.h"
#include "version.h"
#include "netout.h"
#include "airmeta.h"
#include "out.h"
#include "src.h"
#include "bitperfect.h"
#include "channels.h"     /* the speakers' channels: left and right swapped, a speaker's polarity inverted */
#include "vol.h"

static unsigned st_bp_rate, st_bp_orate, st_bp_dev;
static int st_bp_obits;
static char st_bp_dac[160];

#ifdef __APPLE__
static const char *LOOP = "";                  /* macOS (src_pipe.c): the folder of the inputs' pipes, "" <rooms>/in */
#else
static const char *LOOP = "9";                 /* the Loopback card's index */
#endif
static int NO_LOOP = 0;                        /* --loop none: the Squeezebox input only */

#define LOOP_MAX_SUBS 8
#ifdef __APPLE__
static int LOOP_SUBS = 2;                      /* macOS: input 1 is the AirPlay receiver roomd runs (src_pipe.c) */
#else
static int LOOP_SUBS = 1;
#endif
static int cur_sub = 0;                        /* the subdevice playing, or to play */

static vol_ramp vr;
static int vol_on_now = 0, vol_mute_now = 0;
static double vol_db_now = VOL_ON_DB;
static _Atomic long vol_cdb_st = 0;
static _Atomic int vol_act_st = 0;
static _Atomic int air_vol_live = 0;           /* AirPlay's sender's volume to the stage (shairport-sync ignores it) */
static _Atomic int air_vol_fresh = 1;          /* … its first word of the session next ('conn') */
void roomd_vol_now(double *db, int *active) { *db = (double)atomic_load(&vol_cdb_st) / 100.0; *active = atomic_load(&vol_act_st); }

static int st_input = -1;
static const char *DAC = NULL;
static const char *ROOMS = "/var/lib/audiolaz";

static const char *VOIE = NULL;
static const char *VOIE_NAME = NULL;
static const char *VOIE_OUT = NULL;
static const char *VOIE_SELF = NULL;
static const char *VOIE_LOC = NULL;            /* --voie-loc: its renderer's description URL, asked directly while not found */           /* --voie-self: the player's own name (its AirPlay input: never an output) */
static int voie_unbound = 0;                   /* its room bound to no output now (the player stops it) */
static int voie_page_port = 0;                 /* its own small page (page.c page_start_voie): 127.0.0.1, 0 none */
static char status_dir_v[700];
static const char *status_dir(void) {          /* where status (and what this process alone keeps) goes */
    if (!VOIE) return ROOMS;
    if (!status_dir_v[0]) snprintf(status_dir_v, sizeof status_dir_v, "%s/voies/%s", ROOMS, VOIE);
    return status_dir_v;
}
enum { RC_LEAD_CARD, RC_LEAD_DIRECT, RC_LEAD_UPNP, RC_LEAD_SQUEEZE, RC_LEAD_AIRPLAY, RC_LEAD_AUDIOLAZ };

#include "../tap/levels.h"
static lvtap lv_tap;
int roomd_levels_client(int fd) { return levels_client(&lv_tap, fd); }
static const char *SQ_SERVER = NULL;           /* --squeeze: the Squeezebox input too */
static const char *SQ_NAME = "audiolaz Room";  /* --name: the player's name in Roon */
static int claim_port_arg = 3489;              /* --claim-port, --claim-to: the tests (netout_claims) */
static const char *claim_to_arg = NULL;
static int PAGE_PORT = 0;                      /* --page: the engine's own page (04/10, engine/alsa/page.c) */
static const char *OUT_LABEL = NULL;           /* --output: what the page calls the DAC */
static int DIRECT_OUT = 0;
static const char *DAC_ARG = NULL;       /* --dac as given (DAC: this play's, out=alsa: may name another, 08/10) */

enum { OUT_ALSA, OUT_DIRECT, OUT_NET };
static int OUT = OUT_ALSA;
static int NET_ON = 1, NET_PORT = 9001, SLIM_PORT = 3484;   /* --net off, --net-port, --slim-port */
/* Why a network receiver does not play this stream (08/10, review), for the status while busy: "rate" (the stream above
 * its MaxSampleRate, out_max), "" otherwise. Refused again every second, said in the journal once per receiver and rate. */
static const char *st_out_why = "";
static unsigned st_out_max = 0;

static char net_spec[160];
#define NET_UP_RESET() ((void)0)
static uint64_t dither_rng = 0x9E3779B97F4A7C15ull;   /* the output's TPDF dither (pcmclock.h s32_out_q) */
/* The speakers' channels (channels.h) for the output this process plays to, as the control file says them
 * (swap.<out>=1, invert_l.<out>=1, invert_r.<out>=1; <out> its out=, "default" the command line's; absent: off): CH_*
 * bits, the free player as Room's. Two stages: the swap is the music's, in the play thread as soon as it is
 * read, before everything that belongs to one speaker (each output channel keeps its own processing), never on what
 * POST /measure or /calibrate plays (chan_swap_now: what one speaker is sent stays on it); the polarity is the
 * writer's, per output channel (invert_l: the speaker on the output's left channel), on everything sent. A change while
 * it plays crossfaded in each. */
static _Atomic int chan_want = 0;
static int chan_swap_now(void) {
    return (atomic_load_explicit(&chan_want, memory_order_relaxed) & CH_SWAP) && !(SQ_SERVER && sq_measuring() == 1) ? CH_SWAP : 0;
}
static volatile sig_atomic_t quit = 0;
static void on_signal(int s) { (void)s; quit = 1; }
/* --parent-pid <pid> (10/10, the Mac player started by the app, src/player.js, as the Swift player was): the player
 * stops with the program that started it (the app quit, or killed: no player left holding the DAC and Roon's zone).
 * Watched by a thread of its own: macOS tells the process's end (kqueue, NOTE_EXIT); elsewhere it is asked every
 * second. Then as a SIGTERM: the play ends as it does when the service stops. */
static pid_t parent_pid = 0;
static void *parent_watch(void *arg) {
    (void)arg;
#ifdef __APPLE__
    int kq = kqueue();
    struct kevent ev;
    EV_SET(&ev, parent_pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    if (kq >= 0 && kevent(kq, &ev, 1, NULL, 0, NULL) == 0) {
        struct kevent got;
        while (kevent(kq, NULL, 0, &got, 1, NULL) < 0 && errno == EINTR) { }
    } else
#endif
    while (kill(parent_pid, 0) == 0 || errno == EPERM) sleep(1);
    quit = 1;
    return NULL;
}

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void logf_(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); fflush(stderr);
}

/* ── The control file ──────────────────────────────────────────────────────────────────────────── */
#define DYN_LO -38.0
#define DYN_HI -18.0                    /* dB: none of it at or above (a loud passage) */

#define CTX_MAX 24
#define CTX_KEY 112
typedef struct { char room[256]; int bypass; double gain; char dyn[64]; double dyn_lo, dyn_hi, dyn_band; char image[64];
                 char sounds_at[40];
                 char out[160];         /* out= : alsa:<device>, squeeze:<name or MAC>, upnp:<UDN>, or "" (the header) */
                 char base[700];

                 char place[80];
                 int noroom;
                 int air_mode;          /* airplay_mode= : AIR_AUTO (absent too), AIR_VIDEO, AIR_MUSIC */
                 int air_iv;            /* airplay_ignore_volume= : 1, the AirPlay sender's volume ignored; 0 applied; −1
                                         * absent: airplay.conf's line left as it is */
                 double air_dev_max;    /* airplay_device_volume_max= (11/10, dB −30…0): the device's ceiling; NAN absent:
                                         * its volume at the session's start */
                 int air_dev;

                 long air_iv_now;       /* airplay_ignore_volume_now= : when the person said yes to the cut
                                         * (s since 1970): applied at once, in an AirPlay session too; 0 none */
                 int relief, dialogue;

                 int out_lat;

                 int ctx_n;

                 char ctx_k[CTX_MAX][CTX_KEY];
                 int ctx_v[CTX_MAX];
                 double cal_ms;

                 int meta_log;          /* airplay_meta_log= : 1, the next AirPlay session's metadata in the journal;
                                         * 2, the same with shairport-sync's richer plist metadata; 0 (absent too) none */
                 char meta_log_v[32];   /* its value as written: one capture per value */
                 int air_buf_full;

               } control;
enum { AIR_AUTO, AIR_VIDEO, AIR_MUSIC };
/* The outputs' corrections : at most this many kept in the control file, each within these bounds (ms; page.c
 * keeps to the same). */
#define OUT_LAT_MAX 16
#define OUT_LAT_MIN (-1000)
#define OUT_LAT_TOP 5000
/* the contexts' settings kept in the control file, every output's together (page.c keeps to the same), and a
 * measured calibration's bounds (ms; page.c too) */
#define CTX_LINES 64

#define MEAS_RELOCK_MS 1000
#define CAL_MS_MIN (-200.0)
#define CAL_MS_MAX 3000.0
/* Which control file was read last: its inode (each switch renames a new file in place) and its time to the
 * nanosecond. Seconds alone missed a second switch within the same second (30/09: a quick A then B on the A/B page left
 * the engine on A while the page showed B). */
typedef struct { ino_t ino; long sec, nsec; } stamp;

static int measuring(void) {
    if (!SQ_SERVER || sq_measuring() != 1) return 0;
    const int k = sq_measure_corrected();
    return k == 2 ? 0 : k ? 2 : 1;
}
static int played_control(control *c, const control *file, int meas);

static void voie_out_of(const char *room, char *out, size_t on) {
    char p[600], line[600], pick[160] = "", first[160] = "", pv[160] = "";
    int have = 0, pick_ok = 0;
    const size_t rl = strlen(room);
    snprintf(p, sizeof p, "%s/control", ROOMS);
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "voie.", 5) && !strncmp(line + 5, room, rl) && line[5 + rl] == '=') snprintf(pv, sizeof pv, "%.159s", line + 6 + rl);
        else if (!strncmp(line, "outroom.", 8)) {
            char *eq = strrchr(line + 8, '=');
            if (!eq || strcmp(eq + 1, room) || eq - (line + 8) >= 160) continue;
            *eq = 0;
            if (!have) { snprintf(first, sizeof first, "%.159s", line + 8); have = 1; }
            if (pv[0] && !strcmp(pv, line + 8)) { snprintf(pick, sizeof pick, "%.159s", line + 8); pick_ok = 1; }
        }
    }
    if (f) fclose(f);
    if (pv[0] && !pick_ok) {                         /* the voie. line after the outroom lines: looked for again */
        f = fopen(p, "r");
        while (f && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strncmp(line, "outroom.", 8) ? NULL : strrchr(line + 8, '=');
            if (eq && !strcmp(eq + 1, room)) { *eq = 0; if (!strcmp(line + 8, pv)) { snprintf(pick, sizeof pick, "%s", pv); pick_ok = 1; } }
        }
        if (f) fclose(f);
    }
    voie_unbound = !have;
    const char *o = pick_ok ? pick : first;
    snprintf(out, on, "%s", !have || !strcmp(o, "default") ? "" : o);
}

static void voie_auto_gain(control *n) { (void)n; }   /* the free player: no level */
static int vl_seen = 0;
static atomic_int vl_gen;
static int read_control(control *c, stamp *seen) {
    static control file;                /* the control file's settings, as last read (c: as played) */
    static int have = 0, meas_was = 0;
    static double ctl_gain = 0;         /* the control's gain= as read (a voie may play its own: voie_auto_gain) */
    const int meas = measuring();
    if (VOIE) page_conf_follow(ROOMS);                 /* a voie: the page's settings as the player's page keeps them */
    const int vmoved = VOIE && have && atomic_load(&vl_gen) != vl_seen;   /* its own automatic level found again */
    char path[512];
    snprintf(path, sizeof path, "%s/control", ROOMS);
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    stamp now = { st.st_ino, (long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec };
    const int same = have && now.ino == seen->ino && now.sec == seen->sec && now.nsec == seen->nsec;
    if (same && meas == meas_was && !vmoved) return 0;

    if (meas != meas_was && SQ_SERVER)
        logf_("measurement state: %d → %d (squeeze: measuring %d, state %d, generation %u, corrected %d)", meas_was, meas,
              sq_measuring(), sq_state(), sq_generation(), sq_measure_corrected());
    meas_was = meas;
    if (same) {
        if (VOIE) { file.gain = ctl_gain; voie_auto_gain(&file); }   /* its own level found again, or by hand now */
        return played_control(c, &file, meas);
    }
    *seen = now;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    control n = have ? file : *c;
    n.dyn[0] = 0; n.dyn_lo = DYN_LO; n.dyn_hi = DYN_HI; n.dyn_band = 0;   /* the dynamic filter only while the file names it */
    n.image[0] = 0;                                                     /* the image too */
    n.out[0] = 0;                                                       /* the output too: the command line's when absent */
    n.air_mode = AIR_AUTO;                                              /* AirPlay's mode too: by its sender when absent */
    n.air_iv = -1; n.air_iv_now = 0; n.air_dev = 0; n.air_dev_max = NAN;                                   /* the sender's volume: the file's as it is when absent */
    n.relief = n.dialogue = 0;
    n.out_lat = 0;                                                      /* the outputs' corrections: read below */
    static char olat_k[OUT_LAT_MAX][160];                               /* out_lat_ms.<out>= lines, their output and value */
    static int olat_v[OUT_LAT_MAX];
    int olat_n = 0;
    /* the contexts' corrections and the outputs' calibrations, every output's: this output's kept below */
    static char cx_o[CTX_LINES][160], cx_k[CTX_LINES][CTX_KEY], cal_o[OUT_LAT_MAX][160];
    static int cx_v[CTX_LINES];
    static double cal_v[OUT_LAT_MAX];
    int cx_n = 0, cal_n = 0;
    /* the speakers' channels, every output's lines (swap., invert_l., invert_r.): this output's kept below */
    static char chan_o[OUT_LAT_MAX][160];
    static int chan_b[OUT_LAT_MAX];
    int chan_n = 0;
    n.ctx_n = 0; n.cal_ms = NAN; n.meta_log = 0; n.meta_log_v[0] = 0; n.air_buf_full = 1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "room=", 5)) snprintf(n.room, sizeof n.room, "%.255s", line + 5);
        else if (!strncmp(line, "bypass=", 7)) n.bypass = atoi(line + 7) != 0;
        else if (!strncmp(line, "gain=", 5)) n.gain = atof(line + 5);
        else if (!strncmp(line, "dynamic=", 8)) snprintf(n.dyn, sizeof n.dyn, "%.63s", line + 8);
        else if (!strncmp(line, "dyn_lo=", 7)) n.dyn_lo = atof(line + 7);
        else if (!strncmp(line, "dyn_hi=", 7)) n.dyn_hi = atof(line + 7);
        else if (!strncmp(line, "dyn_band=", 9)) n.dyn_band = atof(line + 9);
        else if (!strncmp(line, "image=", 6)) snprintf(n.image, sizeof n.image, "%.63s", line + 6);
        else if (!strncmp(line, "sounds_at=", 10)) snprintf(n.sounds_at, sizeof n.sounds_at, "%.39s", line + 10);
        else if (!strncmp(line, "out=", 4)) snprintf(n.out, sizeof n.out, "%.159s", line + 4);
        else if (!strncmp(line, "airplay_ignore_volume=", 22)) n.air_iv = atoi(line + 22) != 0;
        else if (!strncmp(line, "airplay_ignore_volume_now=", 26)) n.air_iv_now = atol(line + 26);
        else if (!strncmp(line, "airplay_device_volume=", 22)) n.air_dev = atoi(line + 22) != 0;
        else if (!strncmp(line, "airplay_device_volume_max=", 26) && line[26] && strcmp(line + 26, "start")) n.air_dev_max = fmin(0, fmax(-30, atof(line + 26)));
        else if (!strncmp(line, "airplay_mode=", 13)) n.air_mode = !strcmp(line + 13, "video") ? AIR_VIDEO : !strcmp(line + 13, "music") ? AIR_MUSIC : AIR_AUTO;
        else if (!strncmp(line, "out_lat_ms.", 11) && olat_n < OUT_LAT_MAX) {
            char *eq = strrchr(line + 11, '=');
            if (eq && eq > line + 11 && eq - (line + 11) < 160) {
                snprintf(olat_k[olat_n], sizeof olat_k[olat_n], "%.*s", (int)(eq - (line + 11)), line + 11);
                olat_v[olat_n++] = atoi(eq + 1);
            }
        }

        else if (!strncmp(line, "ctx_lat_ms.", 11) && cx_n < CTX_LINES) {
            char *eq = strrchr(line + 11, '=');
            if (eq) {
                *eq = 0;
                char *sl = strrchr(line + 11, '/');
                if (sl && sl > line + 11 && sl[1] && sl - (line + 11) < 160 && strlen(sl + 1) < CTX_KEY) {
                    snprintf(cx_o[cx_n], sizeof cx_o[cx_n], "%.*s", (int)(sl - (line + 11)), line + 11);
                    snprintf(cx_k[cx_n], sizeof cx_k[cx_n], "%s", sl + 1);
                    cx_v[cx_n++] = atoi(eq + 1);
                }
            }
        }
        else if (!strncmp(line, "airplay_cal_ms.", 15) && cal_n < OUT_LAT_MAX) {   /* <out>=<ms>, measured */
            char *eq = strrchr(line + 15, '=');
            if (eq && eq > line + 15 && eq - (line + 15) < 160 && eq[1]) {
                snprintf(cal_o[cal_n], sizeof cal_o[cal_n], "%.*s", (int)(eq - (line + 15)), line + 15);
                cal_v[cal_n++] = atof(eq + 1);
            }
        }
        else if ((!strncmp(line, "swap.", 5) || !strncmp(line, "invert_l.", 9) || !strncmp(line, "invert_r.", 9)) && chan_n < OUT_LAT_MAX) {
            const int sw = line[0] == 's', k = sw ? 5 : 9;
            char *eq = strrchr(line + k, '=');
            if (eq && eq > line + k && eq - (line + k) < 160 && atoi(eq + 1) == 1) {
                snprintf(chan_o[chan_n], sizeof chan_o[chan_n], "%.*s", (int)(eq - (line + k)), line + k);
                chan_b[chan_n++] = sw ? CH_SWAP : line[7] == 'l' ? CH_INV_L : CH_INV_R;
            }
        }
        else if (!strncmp(line, "airplay_buffered=", 17)) n.air_buf_full = strcmp(line + 17, "declared") != 0;   /* (10/10 evening; 11/10: full again, tvOS does not honour it in Buffered) */
        else if (!strncmp(line, "airplay_meta_log=", 17)) {             /* the capture: 1 or 2[.<stamp>] */
            n.meta_log = atoi(line + 17);
            if (n.meta_log < 0 || n.meta_log > 2) n.meta_log = 0;
            snprintf(n.meta_log_v, sizeof n.meta_log_v, "%.31s", line + 17);
        }
    }
    if (VOIE) {                                        /* a voie: its room's output (the player's choice), whatever out= says */

        n.air_mode = AIR_AUTO; n.relief = n.dialogue = 0; n.meta_log = 0; n.meta_log_v[0] = 0;
        voie_out_of(VOIE, n.out, sizeof n.out);
        if (VOIE_OUT) snprintf(n.out, sizeof n.out, "%.159s", strcmp(VOIE_OUT, "default") ? VOIE_OUT : "");
    }
    if (strchr(n.dyn, '/') || !strcmp(n.dyn, "..")) n.dyn[0] = 0;
    if (strchr(n.image, '/') || !strcmp(n.image, "..")) n.image[0] = 0;
    if (n.out[0] && strncmp(n.out, OUT_CARD_PREFIX, OUT_CARD_PREFIX_LEN) && !netout_is_net(n.out)) n.out[0] = 0;   /* an unknown kind: the command line's */

    n.noroom = !page_outroom(ROOMS, n.out, n.place, sizeof n.place, n.base, sizeof n.base, NULL, 0);
    for (int i = 0; i < olat_n; i++) if (!strcmp(olat_k[i], n.out[0] ? n.out : "default")) n.out_lat = olat_v[i];
    if (n.out_lat < OUT_LAT_MIN) n.out_lat = OUT_LAT_MIN; else if (n.out_lat > OUT_LAT_TOP) n.out_lat = OUT_LAT_TOP;
    for (int i = 0; i < cx_n && n.ctx_n < CTX_MAX; i++) if (!strcmp(cx_o[i], n.out[0] ? n.out : "default")) {
        int v = cx_v[i];
        if (v < OUT_LAT_MIN) v = OUT_LAT_MIN; else if (v > OUT_LAT_TOP) v = OUT_LAT_TOP;
        snprintf(n.ctx_k[n.ctx_n], CTX_KEY, "%.*s", CTX_KEY - 1, cx_k[i]); n.ctx_v[n.ctx_n++] = v;
    }
    {
        int ch = 0;
        for (int i = 0; i < chan_n; i++) if (!strcmp(chan_o[i], n.out[0] ? n.out : "default")) ch |= chan_b[i];
        if (ch != atomic_load(&chan_want)) logf_("output's channels: %s%s%s%s", ch & CH_SWAP ? "the music's left and right swapped, before the correction" : "", ch & CH_SWAP && ch & (CH_INV_L | CH_INV_R) ? ", " : "",
                                                 ch & (CH_INV_L | CH_INV_R) ? (ch & CH_INV_L && ch & CH_INV_R ? "both outputs' polarity inverted" : ch & CH_INV_L ? "the left output's polarity inverted" : "the right output's polarity inverted") : "",
                                                 ch ? "" : "as they come (nothing swapped nor inverted)");
        atomic_store(&chan_want, ch);
    }
    for (int i = 0; i < cal_n; i++) if (!strcmp(cal_o[i], n.out[0] ? n.out : "default") && cal_v[i] >= CAL_MS_MIN && cal_v[i] <= CAL_MS_MAX) n.cal_ms = cal_v[i];
    fclose(f);
    if (n.gain > 0) n.gain = 0;
    if (n.gain < -40) n.gain = -40;
    ctl_gain = n.gain;
    voie_auto_gain(&n);                  /* a voie: its own room's automatic level (the page's level automatic) */
    file = n; have = 1;
    netout_bound(n.out);                 /* the receiver it is set to, said to the other audiolaz players */
    return played_control(c, &file, meas);
}
/* What plays from the file's settings: those, or a measurement's (see measuring()). 1 when it changed. */
static int played_control(control *c, const control *file, int meas) {
    control n = *file;

    n.bypass = 1; n.gain = 0; n.room[0] = 0; n.dyn[0] = 0; n.image[0] = 0; n.noroom = 1;
    snprintf(n.place, sizeof n.place, "-");
    n.relief = n.dialogue = 0; n.out_lat = 0; n.ctx_n = 0; n.cal_ms = NAN; n.air_dev = 0; n.air_iv = 1; n.air_iv_now = 0;

    if (meas) {
    }
    int changed = strcmp(n.room, c->room) || n.bypass != c->bypass || n.gain != c->gain || strcmp(n.dyn, c->dyn) ||
                  n.dyn_lo != c->dyn_lo || n.dyn_hi != c->dyn_hi || n.dyn_band != c->dyn_band || strcmp(n.image, c->image) ||
                  strcmp(n.sounds_at, c->sounds_at) || strcmp(n.out, c->out) || n.air_mode != c->air_mode ||
                  strcmp(n.base, c->base) || n.noroom != c->noroom
                  ;
    *c = n;
    return changed;
}

#define UP_FREE() ((void)0)
#define UP_LAT(k) 0

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;

typedef struct { long warm, lat; int unit, two; void *alt, *xd; char name[64], image[64], key[340]; } filt;
static void request(const control *c, unsigned rate) { (void)c; (void)rate; }
static void *loader(void *arg) { (void)arg; return NULL; }

/* ── ALSA ──────────────────────────────────────────────────────────────────────────────────────── */
static int dac_quiet = 0;                        /* the DAC refusing try after try: its lines come from dac_refused() */

#define CAP_MAX_CH SRC_MAX_CH
static unsigned cap_ch = 2;                        /* the capture's channels this play (2: as before) */
static src_pcm *open_capture(unsigned rate, unsigned long iper, int want) {
    cap_ch = want > 2 && want <= CAP_MAX_CH ? (unsigned)want : 2;
    src_pcm *in = src_open(cur_sub, rate, cap_ch, iper, CAP_PERIODS);
    if (!in && cap_ch != 2) { cap_ch = 2; in = src_open(cur_sub, rate, 2, iper, CAP_PERIODS); }
    if (in && want > 2 && cap_ch != (unsigned)want) logf_("input %d: %d channels not taken: its first two only", cur_sub, want);
    return in;
}

static int slave_active_sub(int k) { return src_active(k); }
static int slave_active(void) { return src_active(cur_sub); }
static unsigned slave_rate(void) { return src_rate(cur_sub); }
static void shift_set(long v) { src_shift(cur_sub, v); }

static void input_stop(int k) {

    if (!VOIE && src_stop(k)) return;          /* macOS: the receiver roomd runs itself, started again (src.h) */
    char path[600], cmd[700];
    if (VOIE) snprintf(path, sizeof path, "%s/voies/%s/input-stop", ROOMS, VOIE);
    else snprintf(path, sizeof path, "%s/input-stop", ROOMS);
    if (access(path, X_OK)) return;
    snprintf(cmd, sizeof cmd, "'%s' %d </dev/null >/dev/null 2>&1 &", path, k);
    if (system(cmd) == -1) logf_("input-stop %d: not run", k);
}
/* The subdevice that started last among those playing (its trigger time, /proc), or -1: the source to play. */
static int newest_sub(void) {
    int best = -1;
    double t = -1;
    for (int k = 0; k < LOOP_SUBS; k++) {
        if (!slave_active_sub(k)) continue;
        const double tk = src_started(k);
        if (best < 0 || tk > t) { best = k; t = tk; }
    }
    return best;
}

#define AIRPLAY_CAL_MS 430
#define AIR_MOVE_S 0.05                 /* rewritten only beyond this (s) */
#define AIR_IDLE_S 10.0                 /* no AirPlay session for this long first (s) */
#define AIR_SPACING_S 30.0              /* restarts at least this far apart (s) */
static double airplay_cal_ms = AIRPLAY_CAL_MS;

#define AIR_VIDEO_RING_MS 250
#define AIR_CUT_QUIET_S 0.3             /* AirPlay's Loopback side stopped this long, no metadata: cut */
#define AIR_CUT_LATE_S 1.0              /* … metadata read but no pfls / pend said: cut (the sender gone without a word) */
#define AIR_CUT_LIVE_S 10.0             /* … its sender still connected: only that long (a receiver stuck) */
static double air_video_ring_ms = AIR_VIDEO_RING_MS;

#define AIR_CTX_MIN_MS 20.0
#define AIR_CTX_LATER_MAX_MS 1500.0
#define AIR_VIDEO_RING_FLOOR_MS 150.0
/* AirPlay video: the ring's excess over its target dropped at once beyond this (play(), vf_*) */
#define AIR_VIDEO_RING_TOL_MS 60.0
static double st_ctx_adj_ms = 0, st_live_left_ms = 0;   /* st_live_left_ms : a press's sooner beyond the ring's room */

#define AIR_MUSIC_RING_MS 500
static double air_music_ring_ms = AIR_MUSIC_RING_MS;
#define DRV_RING_MS 450
static double drv_ring_ms = DRV_RING_MS;

static double drv_ring_learned_s = 0;

/* a source at the DAC's pace to a network output: read at its rate while the ring is full, never later than
 * this after its last read (well within the card's AZC_STALE_MS), the ring at most this much above its level */
#define PACED_KEEP_S 0.05
#define PACED_KEEP_MAX_S 1.5
#define PACED_HOLD_MAX_S 8.0                    /* the card held for a UPnP renderer that starts this long at most */
#define DRV_DECAY_QUIET_S 30.0
#define DRV_DECAY_STEP_MS 2.0
#define DRV_DECAY_EVERY_S 1.0
static double drv_paced_hold_t = -1e9;         /* the last hold-up (now_s) */
static double drv_paced_learned(double t) {
    const double quiet = t - drv_paced_hold_t - DRV_DECAY_QUIET_S;
    if (drv_ring_learned_s <= 0 || quiet < 0) return drv_ring_learned_s;
    const double v = drv_ring_learned_s - (floor(quiet / DRV_DECAY_EVERY_S) + 1) * DRV_DECAY_STEP_MS / 1000.0;
    return v > 0 ? v : 0;
}
/* the app ended its IO (src_io_ended): the DAC let go this long after, once the ring played out */
#define DRV_END_S 0.5

#define AIR_STREAM_S 2.0
#define AIR_MARGIN_S 0.1
static double st_lat_ms = -1;           /* this play's latency (ms), for the status; −1: not measured yet */

static char st_heard[340] = "";

#define LAT_RATES 16
#define LAT_SAVE_MS 5.0
#define LAT_PROC_EST_MS 180.0
static double net_video_hold_ms;                       /* below (NET_VIDEO_HOLD_MS) */
static struct { char out[160]; unsigned rate; double video_ms, base_ms, proc_ms; } lat_at[LAT_RATES];
static int lat_next = 0;
static unsigned air_rate = 0;
static void lat_save(void) {
    char path[760], tmp[780];
    snprintf(path, sizeof path, "%s/airplay-latency", status_dir());
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# audiolaz-roomd: the latencies measured (ms), kept across restarts: rate video base processing output\n");
    for (int k = 0; k < LAT_RATES; k++)
        if (lat_at[k].rate) fprintf(f, "%u %.1f %.1f %.1f %s\n", lat_at[k].rate, lat_at[k].video_ms, lat_at[k].base_ms, lat_at[k].proc_ms, lat_at[k].out);
    if (fclose(f) == 0) rename(tmp, path); else unlink(tmp);
}
static void lat_load(void) {
    char path[760], line[400];
    snprintf(path, sizeof path, "%s/airplay-latency", status_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    int n = 0;
    while (n < LAT_RATES && fgets(line, sizeof line, f)) {
        unsigned r; double v, b, p; int at = 0;
        if (line[0] == '#' || sscanf(line, "%u %lf %lf %lf %n", &r, &v, &b, &p, &at) != 4 || !r || at <= 0) continue;
        line[strcspn(line, "\r\n")] = 0;
        snprintf(lat_at[n].out, sizeof lat_at[n].out, "%s", line + at);
        lat_at[n].rate = r; lat_at[n].video_ms = v > 0 ? v : 0; lat_at[n].base_ms = b > 0 ? b : 0; lat_at[n].proc_ms = p > 0 ? p : 0;
        n++;
    }
    fclose(f);
    lat_next = n % LAT_RATES;
    if (n) logf_("AirPlay: %d latencies kept from before (%s)", n, path);
}
static void lat_keep(const char *out, unsigned rate, double video_ms, double base_ms, double proc_ms) {
    int k = -1, moved = 0;
    for (int i = 0; i < LAT_RATES && k < 0; i++) if (lat_at[i].rate == rate && !strcmp(lat_at[i].out, out)) k = i;
    if (k < 0) {                                       /* a new one: a free place, else the oldest made */
        k = lat_next; lat_next = (lat_next + 1) % LAT_RATES;
        snprintf(lat_at[k].out, sizeof lat_at[k].out, "%s", out); lat_at[k].rate = rate; lat_at[k].video_ms = 0; lat_at[k].base_ms = 0; lat_at[k].proc_ms = 0;
        moved = 1;
    }
    if (video_ms > 0) { moved |= fabs(lat_at[k].video_ms - video_ms) >= LAT_SAVE_MS; lat_at[k].video_ms = video_ms; }
    if (base_ms > 0) { moved |= fabs(lat_at[k].base_ms - base_ms) >= LAT_SAVE_MS; lat_at[k].base_ms = base_ms; }
    if (proc_ms > 0) { moved |= fabs(lat_at[k].proc_ms - proc_ms) >= LAT_SAVE_MS; lat_at[k].proc_ms = proc_ms; }
    if (moved) lat_save();
}
/* The video latency the AirPlay input's offset follows (music needs no lip sync), to this output: a video play's own
 * measure at the rate AirPlay last played (44.1 kHz, shairport-sync's usual, before it ever played), else another play's
 * base at that rate plus the video ring, else the same at any rate; a network receiver: its estimate (above); −1:
 * nothing known yet. *est: 1 when estimated from the base, 2 a network receiver's estimate. */
static double lat_airplay(const char *out, int *est) {
    const unsigned r0 = air_rate ? air_rate : 44100;
    *est = 0;
    for (int any = 0; any < 2; any++) {
        for (int k = 0; k < LAT_RATES; k++) if (lat_at[k].rate && !strcmp(lat_at[k].out, out) && (any || lat_at[k].rate == r0) && lat_at[k].video_ms > 0) return lat_at[k].video_ms;
        for (int k = 0; k < LAT_RATES; k++)
            if (lat_at[k].rate && !strcmp(lat_at[k].out, out) && (any || lat_at[k].rate == r0) && lat_at[k].base_ms > 0) { *est = 1; return lat_at[k].base_ms + air_video_ring_ms; }
    }
    if (!netout_is_net(out)) return -1;
    double proc = 0;                                   /* the processing as measured at that rate (any output), else any */
    for (int any = 0; any < 2 && proc <= 0; any++)
        for (int k = 0; k < LAT_RATES && proc <= 0; k++) if (lat_at[k].rate && (any || lat_at[k].rate == r0) && lat_at[k].proc_ms > 0) proc = lat_at[k].proc_ms;
    if (proc <= 0) proc = LAT_PROC_EST_MS;
    *est = 2;
    return round(proc + air_video_ring_ms + net_video_hold_ms + netout_hold_floor_ms_for(out) + netout_link_extra_ms_for(out));   /* Link's ring */
}

typedef struct { int n; char key[5][CTX_KEY]; char label[5][100]; const char *lvl[5]; } air_ctx;   /* most precise first */
static void air_ctx_now(air_ctx *x);     /* the metadata thread's identity of the session going on, or of the last one */
static void air_ctx_for(air_ctx *x, const char *styp_as);   /* the same, as if its stream were of that kind */
static int air_sess_buffered(void);
static int air_sess_any(void);

static int air_rest_pending = 0;
static int air_sender_paused(double min_s);   /* the sender said pause or stop, that long ago, nothing since */
#define AIR_REST_IDLE_S 2.0
static void airplay_tick(const control *c);
static int air_styp_buffered(void);
/* A context's correction (ms) for the output c plays to: the most precise context with a setting kept, else the
 * output's own (out_lat_ms), else 0. *from: its index in x, −1 the output's, −2 none kept. */
static int corr_for(const control *c, const air_ctx *x, int *from) {
    for (int i = 0; i < x->n; i++)
        for (int k = 0; k < c->ctx_n; k++) if (!strcmp(c->ctx_k[k], x->key[i])) { *from = i; return c->ctx_v[k]; }
    *from = c->out_lat ? -1 : -2;
    return c->out_lat;
}

static int corr_marker(const control *c, const char *key, int was) {
    (void)was;
    if (key[0]) for (int k = 0; k < c->ctx_n; k++) if (!strcmp(c->ctx_k[k], key)) return c->ctx_v[k];
    return c->out_lat;
}
/* What the AirPlay input's settings were last written for (its marker line, airplay_tick): the context's key and the
 * correction (ms) in the offset and the declared latency now; air_prep_known 0: an older file, written for the output's
 * own correction (out_lat_ms) as before 10/10. */
static char air_prep_key[CTX_KEY] = "";
static int air_prep_corr = 0, air_prep_known = 0, air_prep_corr_b = 0, air_prep_known_b = 0;   /* _b: the Buffered offset's */
/* The capture of the senders' metadata (10/10, airplay_meta_log=; the metadata thread below): asked and not done yet (its
 * level, 1 or 2, set from the control by the main loop), shairport-sync running with its plist metadata (level 2:
 * airplay_tick restarts it so between sessions), a capture going on (its level), one just over (the main loop then
 * keeps its value as done: one capture per value). */
static _Atomic int cap_want = 0, cap_plist = 0, cap_on = 0, cap_over = 0;
static char cap_done_v[32] = "";
static int cap_plist_refused = 0;       /* airplay.conf has a diagnostics section of its own: level 2 as 1 */
static double air_file_s = NAN;         /* the offset in <rooms>/airplay.conf (the status), NAN: no such file */

static int air_iv_file = 0;             /* the file's ignore_volume_control: 1 "yes" (the status); 2 "yes" for the device's
                                         * volume (11/10, AIR_IV_DEV) */

#define AIR_IV_DEV "the sender's volume sent to the device"
static pthread_mutex_t air_mu = PTHREAD_MUTEX_INITIALIZER;   /* (11/10: here, airplay_tick takes it) */
static atomic_int dv_live = 0;          /* the file says so and the output has a volume: pvol forwarded */
static char dv_spec[160];               /* that output (under air_mu) */
static double air_pvol = NAN;           /* the sender's last volume, dB (under air_mu) */
static int air_session = 0;             /* an AirPlay session now: its subdevice open or a sender connected (the status) */
static int air_port_conf = 0;           /* the receiver's RTSP port as airplay.conf says it (0: its default) */

static char air_name_file[160] = "", air_name_next[160] = "";   /* the file's, the one it will take ("" the same) */
#define AIR_ABSENT_S 60.0               /* the output absent this long: the AirPlay input named after the player alone */

static int out_present(const control *c) {
    if (netout_is_net(c->out)) { char b[160]; netout_out_name(c->out, b, sizeof b); return b[0] != 0; }
    const char *d = !strncmp(c->out, OUT_CARD_PREFIX, OUT_CARD_PREFIX_LEN) && c->out[OUT_CARD_PREFIX_LEN] ? c->out + OUT_CARD_PREFIX_LEN : DAC_ARG;
    return out_dev_present(d);                         /* out.h : macOS's Core Audio device by its UID too */
}

#if defined(__APPLE__) || defined(__CYGWIN__)
/* macOS: a Core Audio device by its UID, named as the page lists it (out_list_json's "name"); "" when it is not
 * there (unplugged: the player's name alone, below), and for "null" and "default" (no device of its own to name).
 * Windows the same: a WASAPI endpoint by its id (engine/win/out_wasapi.c). */
static void card_label(const char *uid, char *b, size_t bl) {
    static char list[16384];
    char id[200];
    b[0] = 0;
    out_list_json(list, sizeof list);
    snprintf(id, sizeof id, "\"id\":\"%s%s\"", OUT_CARD_PREFIX, uid);
    const char *p = strstr(list, id), *e = p ? strchr(p, '}') : NULL, *n = p ? strstr(p, "\"name\":\"") : NULL;
    if (!n || (e && n > e)) return;
    n += 8;
    size_t w = 0;
    for (; *n && *n != '"' && w + 1 < bl; n++) { if (*n == '\\' && n[1]) n++; b[w++] = *n; }
    b[w] = 0;
}
#endif
static void out_short(const control *c, char *o, size_t ol) {
    char b[200] = "";
    if (netout_is_net(c->out)) netout_out_name(c->out, b, sizeof b);
#if defined(__APPLE__) || defined(__CYGWIN__)
    else if (!strncmp(c->out, OUT_CARD_PREFIX, OUT_CARD_PREFIX_LEN) && c->out[OUT_CARD_PREFIX_LEN]) card_label(c->out + OUT_CARD_PREFIX_LEN, b, sizeof b);
    else if (OUT_LABEL) snprintf(b, sizeof b, "%s", OUT_LABEL);
    else card_label(DAC_ARG ? DAC_ARG : "", b, sizeof b);
#else
    else {
        const char *d = !strncmp(c->out, OUT_CARD_PREFIX, OUT_CARD_PREFIX_LEN) && c->out[OUT_CARD_PREFIX_LEN] ? c->out + OUT_CARD_PREFIX_LEN : OUT_LABEL ? OUT_LABEL : DAC_ARG;
        const char *cd = strstr(d, "CARD=");
        if (cd) { snprintf(b, sizeof b, "%s", cd + 5); b[strcspn(b, ",")] = 0; }
        else snprintf(b, sizeof b, "%s", d);
    }
#endif
    char *par = strstr(b, " (");
    if (par && par > b) *par = 0;
    par = strstr(b, " \u00b7 ");
    if (par && par > b) *par = 0;
    size_t w = 0;
    for (const char *q = b; *q && w + 1 < ol; q++) if ((unsigned char)*q >= 0x20 && *q != '"' && *q != '\\' && *q != '%') o[w++] = *q;
    while (w > 0 && o[w - 1] == ' ') w--;
    o[w] = 0;
}

static void air_name_want(const control *c, char *nm, size_t len, char *alone, size_t alen) {
    char os[120], me[100];
    size_t w = 0;
    for (const char *q = SQ_NAME; *q && w + 1 < sizeof me; q++) if ((unsigned char)*q >= 0x20 && *q != '"' && *q != '\\' && *q != '%') me[w++] = *q;
    me[w] = 0;
    if (alone) snprintf(alone, alen, "%s", me);
    out_short(c, os, sizeof os);

    if (!strncmp(c->out, "audiolaz:", 9)) {
        char d[120], t[120];
        netout_out_dac(c->out, d, sizeof d);
        size_t w2 = 0;
        for (const char *q = d; *q && w2 + 1 < sizeof t; q++) if ((unsigned char)*q >= 0x20 && *q != '"' && *q != '\\' && *q != '%') t[w2++] = *q;
        while (w2 > 0 && t[w2 - 1] == ' ') w2--;
        t[w2] = 0;
        if (w2) snprintf(os, sizeof os, "%s", t);
    }
    if (os[0]) snprintf(nm, len, "%.90s \u00b7 %.60s", os, me);   /* 153 bytes at most: the name's 160 */
    else snprintf(nm, len, "%s", me);
}

static void dac_of(const control *c, char *b, size_t bl) {
    b[0] = 0;
    if (netout_is_net(c->out)) netout_out_dac(c->out, b, bl);
    if (!b[0]) out_short(c, b, bl);
    if (!strcmp(b, "null") || !strcmp(b, "default")) b[0] = 0;
}

static int dac_by_play = 0;                       /* play() named it by its card (macOS: name, maker, model) */
static void dac_tick(const control *c, int playing) {
    static double last = 0, absent_since = 0;
    if (!playing) dac_by_play = 0;
    else if (dac_by_play) return;                     /* the device's own name meanwhile, not the page's */
    static char was[sizeof c->out] = "";
    const double t = now_s();
    if (last && t - last < 2.0 && !strcmp(was, c->out)) return;
    last = t;
    if (strcmp(was, c->out)) { snprintf(was, sizeof was, "%s", c->out); absent_since = 0; }
    char b[120] = "";
    if (out_present(c)) { absent_since = 0; dac_of(c, b, sizeof b); }
    else {
        if (!absent_since) absent_since = t;
        if (t - absent_since < AIR_ABSENT_S) return;   /* not there yet: the name as it is */
    }
    src_dac(b, NULL, NULL, 0);
}

static void inputs_conf_path(char *path, size_t n) {
    if (VOIE) snprintf(path, n, "%s/voies/%s/inputs.conf", ROOMS, VOIE);
    else snprintf(path, n, "%s/inputs.conf", ROOMS);
}

static int airplay_sub(void) {
    if (VOIE) return -1;
    char path[600], line[128];
    inputs_conf_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int k = -1;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] >= '0' && line[0] <= '9' && eq && !strcmp(eq + 1, "airplay")) k = atoi(line);
    }
    fclose(f);
    return k >= 0 && k < LOOP_MAX_SUBS ? k : -1;
}
/* the input inputs.conf names on subdevice k ("airplay", "spotify"…), "" none. */
static void sub_input_name(int k, char *out, size_t len) {
    char path[600], line[128];
    out[0] = 0;
    inputs_conf_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] >= '0' && line[0] <= '9' && eq && atoi(line) == k) snprintf(out, len, "%s", eq + 1);
    }
    fclose(f);
}
/* <rooms>/airplay.conf read into buf: 1 with the player's offset line in it (its value in *off, port= in *port, 0 if
 * not set; its ALSA buffer in *bufs, 0.2 s if not set), 0 otherwise (none, or not the player's to keep). *decl: the
 * output latency declared to the senders (09/10, below), its line the player's too, −1 when the file has none. */
static int iv_line(const char *k) { return !strncmp(k, "ignore_volume_control", 21) && (k[21] == ' ' || k[21] == '\t' || k[21] == '='); }

static int ip_line(const char *k) { return !strncmp(k, "interpolation", 13) && (k[13] == ' ' || k[13] == '\t' || k[13] == '='); }
#define AIR_IP_LINE "interpolation = \"basic\"; // audiolaz: the sender's clock followed by a frame put in or left out (resampled packets came out as full-scale garbage)"

typedef struct { char prep_key[CTX_KEY]; int prep_corr, prep_known, plist, diag_other; char prep_key_b[CTX_KEY]; int prep_corr_b, prep_known_b, has_boff; double boff; char boff_why[160];
                 char prep_out[160]; int prep_out_known; } air_extra;   /* prep_out : the output they were written for */
#define AIR_PREP_MARK "// audiolaz: prepared for \""
#define AIR_PREP_MARK_B "; Buffered \""
#define AIR_PREP_MARK_O "; output \""
#define AIR_BOFF_LINE "buffered_latency_offset_in_seconds"
static int plist_line(const char *k) { return !strncmp(k, "diagnostics", 11) && strstr(k, "get_plist_metadata") && strstr(k, "audiolaz: kept by the player"); }
static int air_read(const char *path, char *buf, size_t len, double *off, int *port, double *bufs, double *decl, int *iv, char *nm, size_t nml, air_extra *x) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    const size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = 0;
    int found = 0;
    *port = 0;
    *bufs = 0.2;
    *decl = -1;
    *iv = 0;
    nm[0] = 0;
    memset(x, 0, sizeof *x);
    for (const char *l = buf; *l; ) {
        const char *e = strchr(l, '\n');
        const size_t ll = e ? (size_t)(e - l) : strlen(l);
        char line[600];
        snprintf(line, sizeof line, "%.*s", (int)(ll < sizeof line - 1 ? ll : sizeof line - 1), l);
        const char *k = line + strspn(line, " \t");
        if (!strncmp(k, "audio_backend_latency_offset_in_seconds", 39) && strstr(k, "audiolaz: kept by the player")) {
            const char *eq = strchr(k, '=');
            if (eq) { *off = strtod(eq + 1, NULL); found = 1; }
        } else if (!strncmp(k, "declared_output_latency_in_seconds", 34) && strstr(k, "audiolaz: kept by the player")) {
            const char *eq = strchr(k, '=');
            if (eq) *decl = strtod(eq + 1, NULL);
        } else if (!strncmp(k, "port", 4) && (k[4] == ' ' || k[4] == '\t' || k[4] == '=')) {
            const char *eq = strchr(k, '=');
            if (eq) *port = atoi(eq + 1);
        } else if (!strncmp(k, "name", 4) && (k[4] == ' ' || k[4] == '=') && strstr(k, "audiolaz: kept by the player")) {
            const char *q1 = strchr(k, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q1 && q2) snprintf(nm, nml, "%.*s", (int)(q2 - q1 - 1), q1 + 1);
        } else if (iv_line(k)) {
            const char *eq = strchr(k, '=');
            *iv = eq && strstr(eq, "\"yes\"") != NULL;
            if (*iv && strstr(eq, AIR_IV_DEV)) *iv = 2;            /* the player's own: the volume on the device */
        } else if (!strncmp(k, "audio_backend_buffer_desired_length_in_seconds", 47) && (k[47] == ' ' || k[47] == '\t' || k[47] == '=')) {
            const char *eq = strchr(k, '=');
            if (eq && strtod(eq + 1, NULL) > 0) *bufs = strtod(eq + 1, NULL);
        } else if (!strncmp(k, AIR_PREP_MARK, strlen(AIR_PREP_MARK))) {   /* for which context */
            const char *q1 = k + strlen(AIR_PREP_MARK), *q2 = strchr(q1, '"'), *cr = q2 ? strstr(q2, "correction ") : NULL;
            if (q2 && cr) {
                snprintf(x->prep_key, sizeof x->prep_key, "%.*s", (int)(q2 - q1 < CTX_KEY - 1 ? q2 - q1 : CTX_KEY - 1), q1);
                x->prep_corr = atoi(cr + 11); x->prep_known = 1;
            }
            const char *b1 = q2 ? strstr(q2, AIR_PREP_MARK_B) : NULL;   /* the Buffered offset's */
            const char *b2 = b1 ? strchr(b1 + strlen(AIR_PREP_MARK_B), '"') : NULL, *bc = b2 ? strstr(b2, "correction ") : NULL;
            if (b1 && b2 && bc) {
                b1 += strlen(AIR_PREP_MARK_B);
                snprintf(x->prep_key_b, sizeof x->prep_key_b, "%.*s", (int)(b2 - b1 < CTX_KEY - 1 ? b2 - b1 : CTX_KEY - 1), b1);
                x->prep_corr_b = atoi(bc + 11); x->prep_known_b = 1;
            }
            const char *o1 = q2 ? strstr(q2, AIR_PREP_MARK_O) : NULL;   /* the output's */
            const char *o2 = o1 ? strchr(o1 + strlen(AIR_PREP_MARK_O), '"') : NULL;
            if (o1 && o2) {
                o1 += strlen(AIR_PREP_MARK_O);
                snprintf(x->prep_out, sizeof x->prep_out, "%.*s", (int)(o2 - o1 < 159 ? o2 - o1 : 159), o1);
                x->prep_out_known = 1;
            }
        } else if (!strncmp(k, AIR_BOFF_LINE, strlen(AIR_BOFF_LINE)) && strstr(k, "audiolaz: kept by the player")) {
            const char *eq = strchr(k, '=');
            if (eq) { x->boff = strtod(eq + 1, NULL); x->has_boff = 1; }
        } else if (plist_line(k)) x->plist = 1;
        else if (!strncmp(k, "diagnostics", 11) && (k[11] == ' ' || k[11] == '\t' || k[11] == '=' || !k[11])) x->diag_other = 1;
        l += ll + (e ? 1 : 0);
    }
    return found;
}
/* The offset line (and the declared latency's, when the file has one) rewritten, the rest of the file as it was,
 * atomically. lat_ms: the latency in video the offset follows (−1: nothing measured), cal_ms: what follows the output
 * (measured, or AIRPLAY_CAL_MS), corr_ms: the correction by ear (the context's, else the output's), missing_ms: what the
 * offset could not take. w: the context it is written for (its marker line, after the offset's), and
 * shairport-sync's plist metadata for a capture (w->plist: 1 the player's diagnostics line there, 0 none). 0: done. */
static int air_write(const char *path, const char *buf, double off, double decl, double lat_ms, double cal_ms, double corr_ms, double missing_ms, int iv, const char *name, const air_extra *w) {
    char tmp[620];
    snprintf(tmp, sizeof tmp, "%s/.airplay.conf", ROOMS);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    char corr[48] = "";
    if (corr_ms) snprintf(corr, sizeof corr, " %+.0f ms (set by ear)", corr_ms);
    int plist_done = 0;
    char ivl[200];                                       /* the sender's volume line , after the offset's */
    if (iv == 2) snprintf(ivl, sizeof ivl, "ignore_volume_control = \"yes\"; // audiolaz: set by the player (%s)", AIR_IV_DEV);
    else snprintf(ivl, sizeof ivl, "ignore_volume_control = \"%s\"; // audiolaz: set on the player's page (%s)", iv ? "yes" : "no",
             iv ? "the sender's volume ignored: the volume is set on the amp or the streamer" : "the sender's volume applied");
    int iv_done = 0, ip_done = 0;
    for (const char *l = buf; *l; ) {
        const char *e = strchr(l, '\n');
        const size_t ll = e ? (size_t)(e - l) : strlen(l);
        const size_t ind = strspn(l, " \t");
        char line[600];
        snprintf(line, sizeof line, "%.*s", (int)(ll < sizeof line - 1 ? ll : sizeof line - 1), l);
        if (ind < ll && !strncmp(l + ind, "audio_backend_latency_offset_in_seconds", 39) && strstr(line, "audiolaz: kept by the player")) {
            if (lat_ms < 0)                          /* nothing measured yet: only brought back within a 2 s stream */
                fprintf(f, "%.*saudio_backend_latency_offset_in_seconds = %.3f; // audiolaz: kept by the player (held at what a 2 s stream accepts; not measured yet)%s",
                        (int)ind, l, off, e ? "\n" : "");
            else fprintf(f, "%.*saudio_backend_latency_offset_in_seconds = %.3f; // audiolaz: kept by the player (its latency in video %.0f ms + %.0f ms%s%s%s)%s",
                         (int)ind, l, off, lat_ms, cal_ms, cal_ms != airplay_cal_ms ? " measured" : "", corr, missing_ms > 0 ? ", held at what a 2 s stream accepts" : "", e ? "\n" : "");
            /* for which context: right after it, the one line of its kind (the Buffered offset's too when the
             * file has its line) */
            char mb[CTX_KEY + 260] = "";
            if (w->has_boff) snprintf(mb, sizeof mb, AIR_PREP_MARK_B "%s\" correction %d ms", w->prep_key_b, w->prep_corr_b);
            { const size_t ml = strlen(mb); snprintf(mb + ml, sizeof mb - ml, AIR_PREP_MARK_O "%.150s\"", w->prep_out); }   /* */
            fprintf(f, "%s%.*s" AIR_PREP_MARK "%s\" correction %d ms%s (the player's, for the next AirPlay session)\n", e ? "" : "\n", (int)ind, l,
                    w->prep_key, w->prep_corr, mb);
            if (!iv_done && iv >= 0) {                   /* none in the file yet: in the same section, after it (one
                                                          * further down: replaced there instead) */
                int later = 0;
                for (const char *m = e ? e + 1 : l + ll; *m && !later; ) {
                    const char *me = strchr(m, '\n');
                    later = iv_line(m + strspn(m, " \t"));
                    m = me ? me + 1 : m + strlen(m);
                }
                if (!later) { fprintf(f, "%s%.*s%s\n", e ? "" : "\n", (int)ind, l, ivl); iv_done = 1; }
            }
            if (!ip_done) {                              /* the same for the interpolation's line */
                int later = 0;
                for (const char *m = e ? e + 1 : l + ll; *m && !later; ) {
                    const char *me = strchr(m, '\n');
                    later = ip_line(m + strspn(m, " \t"));
                    m = me ? me + 1 : m + strlen(m);
                }
                if (!later) { fprintf(f, "%.*s" AIR_IP_LINE "\n", (int)ind, l); ip_done = 1; }
            }
        }
        else if (ind < ll && ip_line(l + ind)) {
            if (!ip_done) { fprintf(f, "%.*s" AIR_IP_LINE "%s", (int)ind, l, e ? "\n" : ""); ip_done = 1; }
        }
        else if (name && ind < ll && !strncmp(l + ind, "name", 4) && (l[ind + 4] == ' ' || l[ind + 4] == '=') && strstr(line, "audiolaz: kept by the player"))
            fprintf(f, "%.*sname = \"%s\"; // audiolaz: kept by the player (the output's name)%s", (int)ind, l, name, e ? "\n" : "");
        else if (ind < ll && iv_line(l + ind)) {
            if (iv >= 0 && !iv_done) { fprintf(f, "%.*s%s%s", (int)ind, l, ivl, e ? "\n" : ""); iv_done = 1; }
            else if (iv < 0) fwrite(l, 1, ll + (e ? 1 : 0), f);
            /* a second one: dropped (libconfig refuses a setting twice) */
        }
        else if (ind < ll && !strncmp(l + ind, "declared_output_latency_in_seconds", 34) && strstr(line, "audiolaz: kept by the player"))
            fprintf(f, "%.*sdeclared_output_latency_in_seconds = %.3f; // audiolaz: kept by the player (%s)%s", (int)ind, l, decl,
                    decl > 0 ? "what the offset cannot take: a sender honouring it holds its pictures back by that much" : "nothing beyond the offset", e ? "\n" : "");
        else if (ind < ll && !strncmp(l + ind, AIR_PREP_MARK, strlen(AIR_PREP_MARK))) { /* the marker: written above, again */ }
        else if (w->has_boff && ind < ll && !strncmp(l + ind, AIR_BOFF_LINE, strlen(AIR_BOFF_LINE)) && strstr(line, "audiolaz: kept by the player"))
            fprintf(f, "%.*s" AIR_BOFF_LINE " = %.3f; // audiolaz: kept by the player (Buffered streams: %s)%s", (int)ind, l, w->boff,
                    w->boff_why, e ? "\n" : "");             /* the Buffered streams' offset */
        else if (ind < ll && plist_line(l + ind)) { if (w->plist && !plist_done) { fwrite(l, 1, ll + (e ? 1 : 0), f); plist_done = 1; } }
        else fwrite(l, 1, ll + (e ? 1 : 0), f);
        l += ll + (e ? 1 : 0);
    }
    if (w->plist && !plist_done)                         /* a capture with the plist metadata: the section, at the end */
        fprintf(f, "%sdiagnostics = { get_plist_metadata = \"yes\"; }; // audiolaz: kept by the player (a capture of the AirPlay metadata, airplay_meta_log=2; taken off after it)\n",
                buf[0] && buf[strlen(buf) - 1] != '\n' ? "\n" : "");
    if (fchmod(fileno(f), 0644) || fclose(f)) { unlink(tmp); return -1; }
    return rename(tmp, path);
}

static double air_need_ms = NAN, air_missing_ms = 0;
static int air_declares = 0;
/* for the status: the calibration used (ms) and whether it was measured for this output; the correction by ear
 * used and where it comes from (the context's index in air_ctx, −1 the output's, −2 none) */
static double air_cal_used = AIRPLAY_CAL_MS;
static int air_cal_measured = 0, air_corr_used = 0, air_corr_from = -2;
/* for the status and play(): the file has the Buffered streams' offset line (the patched shairport-sync),
 * its value (s; NAN none), the kind the last session played most (1 Buffered, 0 Realtime, −1 none) */
static int air_has_boff = 0, air_bmain = -1;
static double air_boff_s = NAN;
/* Once a second at most, from the main loop and from play(): the AirPlay input's offset kept (above), for the output
 * the control names now (c->out), the correction by ear of the last AirPlay session's context (10/10; c->out_lat, the
 * output's own, when none is kept for it) added. */
static void airplay_tick(const control *c) {
    if (VOIE) return;                          /* (18/10, the box: a voie renamed and restarted the player's AirPlay input) the player's alone */
    static double t_last = 0, idle_since = 0, t_restart = -1e9, failed = NAN, held_said = NAN;
    static int iv_failed = -1;                         /* the sender's volume setting that could not be written */
    static long now_done = 0;
    static char buf[16384];
    const double t = now_s();
    if (t - t_last < 1.0 || NO_LOOP) return;
    t_last = t;
    /* the capture : one per value of airplay_meta_log=, the one just over noted as done */
    if (atomic_exchange(&cap_over, 0)) snprintf(cap_done_v, sizeof cap_done_v, "%s", c->meta_log_v);
    atomic_store(&cap_want, c->meta_log > 0 && strcmp(c->meta_log_v, cap_done_v) ? c->meta_log : 0);
    const int k = airplay_sub();
    char path[600];
    snprintf(path, sizeof path, "%s/airplay.conf", ROOMS);
    double cur = 0, bufs = 0.2, decl = -1;
    int port = 0, est = 0, ivf = 0;
    char nmf[160];
    air_extra xr;
    if (k < 0 || !air_read(path, buf, sizeof buf, &cur, &port, &bufs, &decl, &ivf, nmf, sizeof nmf, &xr)) {
        air_file_s = NAN; air_need_ms = NAN; air_missing_ms = 0; air_declares = 0; air_iv_file = 0; air_session = 0;
        air_name_file[0] = air_name_next[0] = 0; air_prep_known = 0; atomic_store(&cap_plist, 0); return;
    }
    snprintf(air_prep_key, sizeof air_prep_key, "%s", xr.prep_key);
    air_prep_corr = xr.prep_corr; air_prep_known = xr.prep_known; air_prep_corr_b = xr.prep_corr_b; air_prep_known_b = xr.prep_known_b && xr.has_boff;
    /* a capture with shairport-sync's plist metadata (airplay_meta_log=2): its line there while one is asked or
     * going on, taken off after it; never beside a diagnostics section of another's */
    int pl_want = atomic_load(&cap_want) == 2 || atomic_load(&cap_on) == 2;
    if (pl_want && xr.diag_other && !cap_plist_refused) {
        cap_plist_refused = 1;
        logf_("AirPlay capture: %s has a diagnostics section of its own: the plist metadata not asked, the capture as airplay_meta_log=1", path);
    }
    if (cap_plist_refused) pl_want = 0;
    atomic_store(&cap_plist, xr.plist || cap_plist_refused);
    const int plmove = pl_want != xr.plist;
    /* its name : the output's, the file's own the player's input (never an output) */
    snprintf(air_name_file, sizeof air_name_file, "%s", nmf);
    netout_set_self2(nmf);
    char nmw[160] = "", pn[100];
    if (nmf[0]) {
        static double absent_since = 0;
        static char absent_out[160] = "";
        if (strcmp(absent_out, c->out)) { snprintf(absent_out, sizeof absent_out, "%s", c->out); absent_since = 0; }
        const int here = out_present(c);
        if (here) absent_since = 0;
        else if (!absent_since) absent_since = t;
        char want[160];
        air_name_want(c, want, sizeof want, pn, sizeof pn);
        if (absent_since && t - absent_since >= AIR_ABSENT_S) snprintf(nmw, sizeof nmw, "%s", pn);
        else if (here) snprintf(nmw, sizeof nmw, "%s", want);
        /* not there yet, less than a minute: the name as it is */
    }
    const int nmmove = nmw[0] && strcmp(nmw, nmf);
    snprintf(air_name_next, sizeof air_name_next, "%s", nmmove ? nmw : "");
    air_file_s = cur;
    air_iv_file = ivf;
    air_session = !src_closed(k) || src_air_connected(port);   /* an AirPlay session: playing, or a sender connected */
    air_port_conf = port;
    air_declares = decl >= 0;
    /* the sender's volume : the file to follow the page's choice; at once (in a session too) when the person said
     * yes to the cut within the last two minutes, else between sessions as the offset */
    /* civ: what the file should say: 2 the device's volume (AIR_IV_DEV), else the page's choice; the player's
     * own "yes" taken back to "no" when nothing is asked */
    const int net_out = NET_ON && netout_is_net(c->out);
    const int dev_want = net_out && c->air_dev && netout_dev_volume_kind(c->out);
    netout_dev_volume_max(c->air_dev_max);              /* the ceiling (NAN: the session's start volume) */

    static int vol_air = 0, vol_air_was = 0;
    {
        double vd = 0;
        int vm = 0;
        const int vo = page_vol_for(c->out, c->noroom ? "" : c->place, &vd, &vm);
        if (vo >= 0) vol_air = vo && !dev_want;
    }
    if (vol_air) vol_air_was = 1;
    atomic_store(&air_vol_live, vol_air && ivf == 2);
    const int civ = dev_want || vol_air ? 2 : c->air_iv >= 0 ? c->air_iv : ivf == 2 ? 0 : -1;
    {
        const int live = dev_want && ivf == 2;
        double pv = NAN;
        pthread_mutex_lock(&air_mu);
        const int moved = live && (!atomic_load(&dv_live) || strcmp(dv_spec, c->out));
        snprintf(dv_spec, sizeof dv_spec, "%s", c->out);
        atomic_store(&dv_live, live);
        if (moved) pv = air_pvol;
        pthread_mutex_unlock(&air_mu);
        if (moved && !isnan(pv)) netout_dev_volume(c->out, pv);   /* the sender's last, now that it goes there */
    }
    const int ivmove = civ >= 0 && ivf != civ;
    const long tnow = (long)time(NULL);
    /* the output back to a card with the player's "yes" : at once, in a session too (full level to the amp) */
    const int force = ivmove && ((c->air_iv_now > 0 && c->air_iv_now != now_done && labs(tnow - c->air_iv_now) < 120) || (ivf == 2 && !net_out && !vol_air_was));
    /* the correction by ear of the last session's context (else the output's), the calibration measured for this
     * output (else AIRPLAY_CAL_MS). each kind of stream its own: the Realtime offset (and the declared
     * latency) from the context as Realtime, the Buffered offset from it as Buffered */
    air_ctx cx, cxr, cxb;
    air_ctx_now(&cx);
    air_ctx_for(&cxr, "Realtime");
    air_ctx_for(&cxb, "Buffered");
    int from_now, from, from_b;
    (void)corr_for(c, &cx, &from_now);
    double corr = corr_for(c, &cxr, &from), corr_b = corr_for(c, &cxb, &from_b);
    /* no AirPlay session seen since the player started (a restart, an update; its latencies kept from
     * before, lat_load): the contexts the file was written for (its marker) kept, not the output's own, so that a
     * restart rewrites nothing */
    const int keep_marks = !air_sess_any() && xr.prep_known;
    if (keep_marks) {
        corr = corr_marker(c, xr.prep_key, xr.prep_corr);
        corr_b = xr.prep_known_b ? corr_marker(c, xr.prep_key_b, xr.prep_corr_b) : corr;
    }
    const double cal = !isnan(c->cal_ms) ? c->cal_ms : airplay_cal_ms;
    air_cal_used = cal; air_cal_measured = !isnan(c->cal_ms); air_corr_used = (int)corr; air_corr_from = from;
    air_extra wx = { "", (int)corr, 1, pl_want, 0 };
    snprintf(wx.prep_out, sizeof wx.prep_out, "%s", c->out);   /* the output they follow: the control's, now */
    snprintf(wx.prep_key, sizeof wx.prep_key, "%s", keep_marks ? xr.prep_key : from >= 0 ? cxr.key[from] : "");
    snprintf(wx.prep_key_b, sizeof wx.prep_key_b, "%s", keep_marks ? (xr.prep_known_b ? xr.prep_key_b : xr.prep_key) : from_b >= 0 ? cxb.key[from_b] : "");
    wx.prep_corr_b = (int)corr_b; wx.has_boff = xr.has_boff; wx.boff = xr.boff;
    snprintf(wx.boff_why, sizeof wx.boff_why, "not measured yet");
    air_has_boff = xr.has_boff; air_boff_s = xr.has_boff ? xr.boff : NAN; air_bmain = air_sess_buffered();
    char csaid[200] = "";
    if (corr && keep_marks) snprintf(csaid, sizeof csaid, " %+.0f ms (%s%.120s%s)", corr, xr.prep_key[0] ? "kept for \"" : "the output's correction", xr.prep_key, xr.prep_key[0] ? "\"" : "");
    else if (corr) snprintf(csaid, sizeof csaid, " %+.0f ms (%s%s%s)", corr, from >= 0 ? "kept for \"" : "the output's correction", from >= 0 ? cxr.label[from] : "", from >= 0 ? "\"" : "");
    const double lat = lat_airplay(c->out, &est);
    /* never more than a Realtime stream accepts (AIR_STREAM_S above): beyond it shairport-sync would play with none */
    const double most = round((AIR_STREAM_S - bufs - AIR_MARGIN_S) * 1000.0) / 1000.0;
    double need = NAN;
    if (lat >= 0) { need = round(lat + cal + corr); if (need < 0) need = 0; }
    air_need_ms = need;
    air_missing_ms = !isnan(need) && most > 0 && need > most * 1000.0 + 0.5 ? need - most * 1000.0 : 0;

    double want_decl = air_declares ? (lat < 0 ? decl : round(air_missing_ms) / 1000.0) : -1;

    double want_b = NAN;
    if (xr.has_boff && lat >= 0) {
        const double need_b = round(lat + cal + corr_b) < 0 ? 0 : round(lat + cal + corr_b);
        const double d = !c->air_buf_full && air_declares && want_decl > 0 ? want_decl * 1000.0 : 0;
        want_b = -(need_b - d) / 1000.0;
        if (want_b < -10.0) want_b = -10.0; else if (want_b > 10.0) want_b = 10.0;
        want_b = round(want_b * 1000.0) / 1000.0;
        char cb[48] = "";
        if (corr_b) snprintf(cb, sizeof cb, " %+.0f ms set by ear", corr_b);
        snprintf(wx.boff_why, sizeof wx.boff_why, "its latency in video %.0f ms%s + %.0f ms%s, %s", lat, est == 2 ? " (estimated)" : "", cal, cb,
                 d > 0 ? "less the declared latency" : "all of it, no latency declared to it");
        wx.boff = want_b;
    }
    if (lat < 0 && !(most > 0 && -cur > most + 0.0005) && !ivmove && !nmmove && !plmove) return;   /* nothing measured since the player started */

    double want = lat < 0 ? cur : -need / 1000.0;
    const int held = most > 0 && -want > most;
    if (held && lat < 0) want = -most;
    else if (held) {
        if (isnan(held_said) || fabs(held_said + want) > AIR_MOVE_S) {   /* once per value, not every second */
            held_said = -want;
            logf_("AirPlay input: the player's latency in video %.0f ms%s + %.0f ms%s%s = %.3f s (to %s), more than a %.0f s Realtime stream accepts (its ALSA buffer %.2f s, a margin of %.1f s): the offset held at %.3f s, the sound still %.1f s behind the picture%s",
                  lat, est == 2 ? " (estimated: the network receiver's hold, the video ring, the processing)" : est ? " (estimated: another input's, its ring as video's)" : "", cal, air_cal_measured ? " (measured)" : "", csaid, -want,
                  c->out[0] ? c->out : "the default output", AIR_STREAM_S, bufs, AIR_MARGIN_S, -most,
                  air_missing_ms / 1000.0, air_declares ? " unless the sender honours the output latency declared to it" : " (no output latency can be declared to the sender: shairport-sync not built for it)");
        }
        want = -most;
    }
    const int offmove = fabs(want - cur) > AIR_MOVE_S || (air_declares && fabs(want_decl - decl) > AIR_MOVE_S)
                        || (!isnan(want_b) && fabs(want_b - xr.boff) > AIR_MOVE_S);
    if (!offmove && !ivmove && !nmmove && !plmove) { idle_since = 0; return; }
    if (!isnan(failed) && fabs(want - failed) <= AIR_MOVE_S && (!ivmove || iv_failed == civ) && !nmmove && !plmove) return;   /* never again for it */
    if (!offmove && !nmmove && !plmove && iv_failed == civ) return;

    const int outmove = offmove && lat >= 0 && (!xr.prep_out_known || strcmp(xr.prep_out, c->out));
    if (!force) {
        const int rest = air_rest_pending && offmove;   /* a press's rest: at the pause */
        if (!src_closed(k) || (air_session && ((!outmove && !rest) || !air_sender_paused(AIR_REST_IDLE_S)))) { idle_since = 0; return; }   /* an AirPlay session: never in it */
        if (!idle_since) { idle_since = t; return; }
        if (rest ? t - idle_since < AIR_REST_IDLE_S : (t - idle_since < AIR_IDLE_S || t - t_restart < AIR_SPACING_S)) return;
    }
    idle_since = 0;
    if (force && c->air_iv_now > 0) now_done = c->air_iv_now;
    if (air_rest_pending && offmove) logf_("AirPlay input: « Synchro image »'s rest taken at the sender's pause (no sound for %.0f s)", AIR_REST_IDLE_S);
    air_rest_pending = 0;
    /* the marker says the correction the offset written holds (the two always together, so that play() takes from the
     * session's own context only what they lack); nothing measured: the offset not made from it, the file's kept */
    if (lat < 0) {
        snprintf(wx.prep_key, sizeof wx.prep_key, "%s", xr.prep_known ? xr.prep_key : "");
        wx.prep_corr = xr.prep_known ? xr.prep_corr : c->out_lat;
        snprintf(wx.prep_key_b, sizeof wx.prep_key_b, "%s", xr.prep_known_b ? xr.prep_key_b : wx.prep_key);   /* */
        wx.prep_corr_b = xr.prep_known_b ? xr.prep_corr_b : wx.prep_corr;
    }
    if (air_write(path, buf, want, want_decl > 0 ? want_decl : 0, lat, cal, lat < 0 ? 0 : corr, lat < 0 ? 0 : air_missing_ms, civ, nmmove ? nmw : NULL, &wx)) {
        failed = want; if (ivmove) iv_failed = civ;
        logf_("AirPlay input: %s not written (%s): its latency offset stays %.3f s, the sender's volume %s", path, strerror(errno), cur, ivf ? "ignored" : "applied");
        return;
    }
    t_restart = t; air_file_s = want; if (civ >= 0) air_iv_file = civ; iv_failed = -1;
    if (civ != 2) vol_air_was = 0;                     /* the player's volume no longer the sender's */
    if (plmove) atomic_store(&cap_plist, pl_want);       /* the capture's next session: with it from now */
    if (nmmove) { snprintf(air_name_file, sizeof air_name_file, "%s", nmw); air_name_next[0] = 0; netout_set_self2(nmw); }
    char stop[600], dsaid[400] = "";
    snprintf(stop, sizeof stop, "%s/input-stop", ROOMS);
    const int can = src_owns(k) || !access(stop, X_OK);
    if (ivmove) logf_("AirPlay input: the sender's volume %s (ignore_volume_control \"%s\", %s)%s: %s", civ == 2 ? (vol_air ? "applied by the player's own volume (« Mon ampli n'a pas de volume »), the music at full level into it" : "set on the device, the music at full level") : civ ? "ignored: the volume is set on the amp or the streamer" : "applied again",
                      civ ? "yes" : "no", civ == 2 ? c->out : dev_want || ivf == 2 ? "the output has no volume of its own" : "chosen on the page",
                      force ? (ivf == 2 && !net_out ? ", at once (the output is a card now)" : ", at once (the cut said yes to, in the AirPlay session)") : ", no AirPlay session",
                      can ? "the input restarted" : "read at the input's next start (no input-stop)");
    if (nmmove) logf_("AirPlay input: named \"%s\" (was \"%s\"): the output's (%s), no AirPlay session: %s", nmw, nmf, c->out[0] ? c->out : "the default output",
                      can ? "the input restarted" : "read at the input's next start (no input-stop)");
    if (plmove) logf_("AirPlay capture: shairport-sync's plist metadata %s (diagnostics.get_plist_metadata), no AirPlay session: %s",
                      pl_want ? "asked for the capture" : "taken off again, the capture over", can ? "the input restarted" : "read at the input's next start (no input-stop)");
    if (outmove) logf_("AirPlay input: its settings were for %s, the output now %s%s", xr.prep_out_known ? (xr.prep_out[0] ? xr.prep_out : "the default output") : "an output not said",
                       c->out[0] ? c->out : "the default output", air_session ? " (a sender connected, no sound for 10 s: the input restarted in silence)" : "");
    if (air_declares) snprintf(dsaid, sizeof dsaid, ", output latency declared to the senders %.3f s → %.3f s", decl, want_decl > 0 ? want_decl : 0);
    { const size_t dl = strlen(dsaid); snprintf(dsaid + dl, sizeof dsaid - dl, ", for %.80s", c->out[0] ? c->out : "the default output"); }   /* */
    if (!isnan(want_b)) {                                /* the Buffered streams' offset */
        const size_t dl = strlen(dsaid);
        snprintf(dsaid + dl, sizeof dsaid - dl, ", Buffered streams' offset %.3f s → %.3f s", xr.boff, want_b);
        air_boff_s = want_b;
    }
    if (!offmove) { if (can) input_stop(k); return; }
    if (lat < 0) logf_("AirPlay input: latency offset %.3f s → %.3f s (nothing measured yet: held at what a %.0f s Realtime stream accepts, its ALSA buffer %.2f s, a margin of %.1f s)%s, no AirPlay session: %s",
                       cur, want, AIR_STREAM_S, bufs, AIR_MARGIN_S, dsaid, can ? "the input restarted" : "read at the input's next start (no input-stop)");
    else logf_("AirPlay input: latency offset %.3f s → %.3f s (the player's latency in video %.0f ms%s + %.0f ms%s%s%s)%s, no AirPlay session: %s",
               cur, want, lat, est == 2 ? ", estimated before any video play to it" : est ? ", estimated" : "", cal, air_cal_measured ? " measured for this output" : "", csaid, held ? ", held at what a 2 s stream accepts" : "",
               dsaid, can ? "the input restarted" : "read at the input's next start (no input-stop)");
    if (can) input_stop(k);
}

#include "air_channels.h"                      /* multichannel AirPlay: the source's channels, airplay_channels= */

static const char *AIR_META = "/run/audiolaz-airplay/metadata";   /* --airplay-meta */
static int air_meta_given = 0;
#define AIR_SENDERS 8

static char air_name[96], air_model[48];  /* this session's sender as said: snam, cmod ("" not said) */
/* the play's state as shairport-sync says it: when it last said pfls (the sender's flush: a pause, a seek, a
 * stop) or pend (the play's end), 0 never; and whether its metadata pipe is read now (air_meta_main). */
static double air_stop_said = 0, air_go_said = 0;   /* air_go_said : its last conn, pbeg or prsm */
static volatile int air_meta_open = 0;

static char air_styp[16], air_sdsc[48], air_app[96], air_app_label[64], air_media[48];

static unsigned air_sess_gen = 0, air_sess_seen = 0;
static double air_sess_rt = 0, air_sess_buf = 0;
/* a video play's time added to its session's, by its kind (1 Buffered, 0 Realtime) */
static void air_sess_add(int buffered, double secs) {
    pthread_mutex_lock(&air_mu);
    if (air_sess_seen != air_sess_gen) { air_sess_seen = air_sess_gen; air_sess_rt = air_sess_buf = 0; }
    if (buffered) air_sess_buf += secs; else air_sess_rt += secs;
    pthread_mutex_unlock(&air_mu);
}

static int air_sender_paused(double min_s) {
    pthread_mutex_lock(&air_mu);
    const int r = air_stop_said > 0 && air_stop_said > air_go_said && now_s() - air_stop_said >= min_s;
    pthread_mutex_unlock(&air_mu);
    return r;
}
/* an AirPlay session seen since the player started */
static int air_sess_any(void) {
    pthread_mutex_lock(&air_mu);
    const int r = air_sess_gen > 0;
    pthread_mutex_unlock(&air_mu);
    return r;
}
/* the kind the last session's video played most: 1 Buffered, 0 Realtime, −1 none known */
static int air_sess_buffered(void) {
    pthread_mutex_lock(&air_mu);
    const int r = air_sess_buf > air_sess_rt ? 1 : air_sess_rt > 0 ? 0 : -1;
    pthread_mutex_unlock(&air_mu);
    return r;
}
static int air_last = -1;                 /* the mode of the last sender whose model was known: 1 video, 0 music, −1 none */
static struct { char name[96]; int video; } air_seen[AIR_SENDERS];   /* the senders known by their model, by name */
static int air_seen_n = 0;
static int air_play_video = -1;           /* this play's: 1 video, 0 music; −1 not an AirPlay play (the status) */
static double st_ring_ms = 0;             /* this play's ring target (ms), for the status */

#ifndef SCHED_RESET_ON_FORK
#define SCHED_RESET_ON_FORK 0x40000000    /* <sched.h> names it with _GNU_SOURCE only (the portable build) */
#endif
#define LAT_YIELD_S 0.005               /* working this long without waiting: a nap */
#define LAT_NAP_S 0.0002                /* the nap: the core to the kernel's threads (ksoftirqd, ktimers…) */
#define LAT_UP_MIN_MS 150
#define LAT_PACED_MS 40                   /* an input at the DAC's pace: the ring at most this */
#define LAT_WAITED_S 0.0003               /* a wait at least this long: the thread did wait */
#define PACED_RT_PRIO 1                   /* a source at the DAC's pace at normal priority: this level (play()) */
static int lat_rt_now = 0, lat_rt_want = 0;   /* the level this thread runs at now, this play's (0: normal) */
static double lat_busy_t = 0;             /* since when this thread has not waited */
static long lat_drops = 0;                /* naps taken in a burst of work, this play */
static volatile double lat_low_ms = -1;   /* this play's lowest ring once full (ms; −1: not yet) */
static volatile double lat_min_low_ms = -1;   /* the last full minute's lowest */
static volatile int lat_ring_now = 0;
static void lat_rt_set(int prio) {
#ifdef __linux__
    if (prio == lat_rt_now) return;
    if (prio > 0 && !rt_allowed()) return;             /* within RT_OFF_S of a thread of ours dropped (SIGXCPU) */
    struct sched_param sp = { .sched_priority = prio > 0 ? prio : 0 };
    if (!sched_setscheduler((pid_t)syscall(SYS_gettid), prio > 0 ? (SCHED_FIFO | SCHED_RESET_ON_FORK) : SCHED_OTHER, &sp)) {
        lat_rt_now = prio > 0 ? prio : 0;
        if (prio > 0) rt_register(); else rt_unregister();   /* rt_thread.h: its SIGXCPU drops it */
    }
    else if (prio > 0) logf_("latency: no real-time priority for the convolution (%s): it plays at normal priority", strerror(errno));
#elif defined(__APPLE__)

    const int on = prio > 0;
    if (on == (lat_rt_now > 0)) return;
    if (!on) { rt_standard(); lat_rt_now = 0; return; }
    const int e = rt_clock(2 * LAT_YIELD_S, 0.5, 0);
    if (!e) lat_rt_now = prio;
    else logf_("latency: no Mach real time for the convolution (%s): it plays at its usual priority", strerror(e));
#else
    (void)prio;
#endif
}
#ifdef __linux__
/* (21/10, rt_watch.h) the player's watch: an ordinary thread waking every 250 ms; woken RT_WATCH_LATE_S late or more
 * (the cores held by real-time threads: on a PREEMPT_RT kernel an ordinary thread's timer waits for ktimers/<core>,
 * SCHED_FIFO 1, which any real-time thread of that core holds off), the real-time threads that ran most meanwhile
 * said (any program's: Roon's, ours, the kernel's), once a minute at most */
#define RT_WATCH_LATE_S 1.5
static void *rt_watch_main(void *arg) {
    (void)arg;
#ifdef _GNU_SOURCE
    pthread_setname_np(pthread_self(), "az-watch");
#endif
    static rtw_snap was;
    rtw_take(&was);
    double t_prev = rtw_now(), t_said = -1e9;
    for (;;) {
        const struct timespec d = { 0, 250000000 };
        nanosleep(&d, NULL);
        const double t = rtw_now(), late = t - t_prev - 0.25;
        t_prev = t;
        if (late >= RT_WATCH_LATE_S && t - t_said >= 60) {
            t_said = t;
            char who[600]; double span = 0;
            const int n = rtw_late(&was, who, sizeof who, &span);
            if (n) logf_("real time: this player's watch woke %.1f s late (the cores held); the real-time threads that ran most in the last %.1f s: %s", late, span, who);
            else logf_("real time: this player's watch woke %.1f s late (the cores held), no real-time thread seen running that long", late);
        }
        if (t - was.at >= RTW_EVERY_S) rtw_take(&was);
    }
    return NULL;
}
static void rt_watch_start(void) {
    pthread_t th;
    if (!pthread_create(&th, NULL, rt_watch_main, NULL)) pthread_detach(th);
}
#endif
/* (21/10, rt_thread.h) a real-time thread of ours dropped by its SIGXCPU (200 ms without blocking): said here, from an
 * ordinary place (the main loop, the play's loop) */
static void rt_xcpu_tick(void) {
    static int seen = 0;
    char nm[48];
    const int n = rt_xcpu_said(&seen, nm, sizeof nm);
    if (n) logf_("real time: « %s » ran %d ms without blocking (a loop of ours): dropped to normal priority at once by its SIGXCPU, no real time in this process for %.0f s (%d so far)",
                 nm, RT_LIMIT_US / 1000, RT_OFF_S, n);
}
/* after each wait of the play's loop (t0: before it, t1: after it) */
static void lat_rt_guard(double t0, double t1) {
    if (!lat_rt_want) return;
    if (t1 - t0 >= LAT_WAITED_S) { lat_busy_t = t1; return; }
    if (lat_rt_now && t1 - lat_busy_t > LAT_YIELD_S) {
        const struct timespec nap = { 0, (long)(LAT_NAP_S * 1e9) };
        nanosleep(&nap, NULL);
        lat_busy_t = now_s(); lat_drops++;
    }
}
int roomd_latency_json(char *b, size_t len) {
    const int n = snprintf(b, len, "{\"ring\":%d,\"rt\":%d,\"link\":%d,\"ringNowMs\":%d,\"ringTargetMs\":%.0f,\"ringLowMs\":%.1f,"
                           "\"ringMinuteLowMs\":%.1f,\"rtNow\":%d,\"bursts\":%ld,\"inputLatencyMs\":%.0f}",
                           page_lat_ring, page_lat_rt, page_lat_link, lat_ring_now, st_ring_ms, lat_low_ms, lat_min_low_ms,
                           lat_rt_now, lat_drops, st_lat_ms);
    return n > 0 && (size_t)n < len ? n : -1;
}
static int model_video(const char *m) { return !strncmp(m, "AppleTV", 7); }
/* A string of the pipe kept: printable (no control character, no quote, no backslash: the status and the JSON take it
 * as it is), UTF-8 cut at a character's start. */
static void air_keep(char *dst, size_t len, const unsigned char *s, size_t n) {
    size_t w = 0;
    for (size_t i = 0; i < n && w + 1 < len; i++) dst[w++] = s[i] < 0x20 || s[i] == 0x7f || s[i] == '"' || s[i] == '\\' ? ' ' : (char)s[i];
    if (w < n && w > 0) {                                               /* cut: not inside a character */
        size_t j = w - 1;
        while (j > 0 && ((unsigned char)dst[j] & 0xC0) == 0x80) j--;
        const unsigned char c = (unsigned char)dst[j];
        if (c >= 0xC0 && w - j < (size_t)(c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2)) w = j;
    }
    dst[w] = 0;
}

static struct { int in; char t[256], a[256], l[256]; double t_pict; char last_l[256]; } air_np;
static void air_np_text(char *dst, size_t len, const unsigned char *d, size_t n) {
    size_t w = 0;
    for (size_t i = 0; i < n && w + 1 < len; i++) dst[w++] = d[i] < 0x20 ? ' ' : (char)d[i];
    dst[w] = 0;
}
static void air_np_commit(void) {
    if (strcmp(air_np.l, air_np.last_l) && now_s() - air_np.t_pict > 1.0) netout_np_cover(NETOUT_NP_AIRPLAY, NULL, 0);
    snprintf(air_np.last_l, sizeof air_np.last_l, "%s", air_np.l);
    netout_np_text(NETOUT_NP_AIRPLAY, air_np.t, air_np.a, air_np.l);
}

#define CAP_PER_CODE 25
#define CAP_LINES 6000
#define CAP_MAX_S 10800.0
static struct { uint32_t type, code; long n; } cap_cnt[96];
static int cap_ncnt = 0;
static long cap_items = 0, cap_lines = 0;
static double cap_t0 = 0, cap_t_conn = -1e9;
static void cap_leaf(void *u, const char *path, int kind, const unsigned char *s, size_t n, double num) {
    (void)u;
    if (cap_lines >= CAP_LINES) return;
    char v[320];
    if (kind == AM_STR) am_value(0, s, n, 120, v, sizeof v);
    else if (kind == AM_DATA) snprintf(v, sizeof v, "%zu bytes of data%s", n, n > 3 && s[0] == 0xFF && s[1] == 0xD8 ? " (JPEG)" : n > 7 && s[0] == 0x89 && s[1] == 'P' ? " (PNG)" : "");
    else if (kind == AM_BOOL) snprintf(v, sizeof v, "%s", num ? "true" : "false");
    else if (kind == AM_NULL) snprintf(v, sizeof v, "null");
    else if (kind == AM_UID) snprintf(v, sizeof v, "uid (%zu bytes)", n);
    else if (kind == AM_DATE) snprintf(v, sizeof v, "date %.0f s after 2001-01-01", num);
    else snprintf(v, sizeof v, "%.10g", num);
    logf_("AirPlay capture:   %s = %s", path[0] ? path : "(top)", v);
    cap_lines++;
}
/* n_full: the item's length as shairport-sync said it (what is in d may be cut: 255 bytes at most but for PICT, copl) */
static void cap_item(uint32_t type, uint32_t code, const unsigned char *d, size_t n, size_t n_full) {
    const double t = now_s();
    const int ssnc = type == 0x73736e63u;
    int on = atomic_load(&cap_on);
    const int start = ssnc && (code == 0x636f6e6eu || (code == 0x636c6970u && t - cap_t_conn > 2.0));   /* 'conn', 'clip' */
    if (ssnc && code == 0x636f6e6eu) cap_t_conn = t;
    const int want = atomic_load(&cap_want);
    if (!on && start && want && (want == 1 || atomic_load(&cap_plist))) {
        on = want;
        atomic_store(&cap_on, on);
        cap_ncnt = 0; cap_items = 0; cap_lines = 0; cap_t0 = t;
        logf_("AirPlay capture: a new session: every metadata item of it written here (airplay_meta_log=%d%s)", on,
              on == 2 ? ", shairport-sync's plist metadata on" : "");
    }
    if (!on) return;
    cap_items++;
    int k = 0;
    while (k < cap_ncnt && (cap_cnt[k].type != type || cap_cnt[k].code != code)) k++;
    if (k == cap_ncnt && cap_ncnt < (int)(sizeof cap_cnt / sizeof *cap_cnt)) { cap_cnt[k].type = type; cap_cnt[k].code = code; cap_cnt[k].n = 0; cap_ncnt++; }
    const long seen = k < cap_ncnt ? ++cap_cnt[k].n : CAP_PER_CODE + 1;
    if (seen <= CAP_PER_CODE && cap_lines < CAP_LINES) {
        char ty[5], co[5], v[400];
        am_code(type, ty); am_code(code, co);
        if (ssnc && code == 0x636f706cu && n >= 8 && n == n_full && !memcmp(d, "bplist00", 8)) {   /* 'copl': leaf by leaf */
            logf_("AirPlay capture: %s %s, a binary plist of %zu bytes:", ty, co, n);
            cap_lines++;
            if (am_bplist_walk(d, n, cap_leaf, NULL)) { logf_("AirPlay capture:   (not read whole: broken, or too large)"); cap_lines++; }
        } else {
            am_value(code, d, n, 120, v, sizeof v);
            if (n < n_full) logf_("AirPlay capture: %s %s %s (cut: %zu bytes in all)", ty, co, v, n_full);
            else logf_("AirPlay capture: %s %s %s", ty, co, v);
            cap_lines++;
        }
    }
    const char *why = ssnc && code == 0x64697363u ? "the sender left (disc)" : ssnc && code == 0x61656e64u ? "the session over (aend)"
                    : cap_lines >= CAP_LINES ? "enough lines" : t - cap_t0 > CAP_MAX_S ? "3 hours" : NULL;
    if (why) {
        char sum[700] = "";
        size_t w = 0;
        for (int i = 0; i < cap_ncnt && w + 40 < sizeof sum; i++) if (cap_cnt[i].n > CAP_PER_CODE) {
            char ty[5], co[5];
            am_code(cap_cnt[i].type, ty); am_code(cap_cnt[i].code, co);
            w += (size_t)snprintf(sum + w, sizeof sum - w, "%s%s %s ×%ld", w ? ", " : "", ty, co, cap_cnt[i].n);
        }
        logf_("AirPlay capture: end (%s): %ld items in %.0f s%s%s", why, cap_items, t - cap_t0, sum[0] ? "; written only the first 25 times: " : "", sum);
        atomic_store(&cap_on, 0);
        atomic_store(&cap_over, 1);
    }
}

typedef struct { char bundle[96], label[64], media[48], t[256], a[256], l[256]; const unsigned char *art; size_t art_n; } plist_found;
static void plist_leaf(void *u, const char *path, int kind, const unsigned char *s, size_t n, double num) {
    (void)num;
    plist_found *f = u;
    const char *key = am_leaf_key(path);
    if (kind == AM_DATA) { if (am_key_has(key, "ArtworkData") && n > 0) { f->art = s; f->art_n = n; } return; }
    if (kind != AM_STR || !n) return;
    if (!f->bundle[0] && (am_key_has(key, "BundleIdentifier") || am_key_has(key, "bundleID"))) am_clean(f->bundle, sizeof f->bundle, s, n);
    else if (!f->label[0] && (am_key_has(key, "DisplayName") || am_key_has(key, "ApplicationName"))) am_clean(f->label, sizeof f->label, s, n);
    else if (!f->media[0] && am_key_has(key, "MediaType")) am_clean(f->media, sizeof f->media, s, n);
    else if (!strcmp(key, "kMRMediaRemoteNowPlayingInfoTitle")) air_np_text(f->t, sizeof f->t, s, n);
    else if (!strcmp(key, "kMRMediaRemoteNowPlayingInfoArtist")) air_np_text(f->a, sizeof f->a, s, n);
    else if (!strcmp(key, "kMRMediaRemoteNowPlayingInfoAlbum")) air_np_text(f->l, sizeof f->l, s, n);
}
static void air_plist(const unsigned char *d, size_t n) {
    static plist_found f;
    memset(&f, 0, sizeof f);
    if (n < 8 || memcmp(d, "bplist00", 8)) return;
    (void)am_bplist_walk(d, n, plist_leaf, &f);
    if (f.t[0] || f.a[0] || f.l[0]) {                                   /* the visual (nowplay.h), as from DMAP */
        snprintf(air_np.t, sizeof air_np.t, "%s", f.t); snprintf(air_np.a, sizeof air_np.a, "%s", f.a); snprintf(air_np.l, sizeof air_np.l, "%s", f.l);
        air_np_commit();
    }
    if (f.art && f.art_n) { air_np.t_pict = now_s(); netout_np_cover(NETOUT_NP_AIRPLAY, f.art, f.art_n); }
    if (!f.bundle[0] && !f.label[0] && !f.media[0]) return;
    pthread_mutex_lock(&air_mu);
    const int news = strcmp(air_app, f.bundle[0] ? f.bundle : f.label);
    if (f.bundle[0] || f.label[0]) {
        snprintf(air_app, sizeof air_app, "%s", f.bundle[0] ? f.bundle : f.label);
        if (f.label[0]) snprintf(air_app_label, sizeof air_app_label, "%s", f.label);
        else {
            const char *dot = strrchr(f.bundle, '.');
            snprintf(air_app_label, sizeof air_app_label, "%s", dot && dot[1] ? dot + 1 : f.bundle);
        }
    }
    if (f.media[0]) snprintf(air_media, sizeof air_media, "%s", f.media);
    char app[96], lab[64], med[48];
    snprintf(app, sizeof app, "%s", air_app); snprintf(lab, sizeof lab, "%s", air_app_label); snprintf(med, sizeof med, "%s", air_media);
    pthread_mutex_unlock(&air_mu);
    if (news && app[0]) logf_("AirPlay: the sender's plist names the app \"%s\" (%s)%s%s", lab, app, med[0] ? ", media type " : "", med);
}
static void air_item(uint32_t type, uint32_t code, const unsigned char *d, size_t n, size_t n_full) {
    cap_item(type, code, d, n, n_full);                                 /* the capture, when one is asked */
    if (type == 0x636f7265u) {                                          /* 'core': the track's own (DAAP) */
        char *f = code == 0x6d696e6du ? air_np.t : code == 0x61736172u ? air_np.a : code == 0x6173616cu ? air_np.l : NULL;   /* minm asar asal */
        if (f) { air_np_text(f, sizeof air_np.t, d, n); if (!air_np.in) air_np_commit(); }
        return;
    }
    if (type != 0x73736e63u) return;                                    /* 'ssnc': shairport-sync's own */

    if (code == 0x70666c73u || code == 0x70656e64u || code == 0x70617573u) {
        pthread_mutex_lock(&air_mu); air_stop_said = now_s(); pthread_mutex_unlock(&air_mu);
    }
    if (code == 0x636f6e6eu || code == 0x70626567u || code == 0x7072736du) {   /* 'conn', 'pbeg', 'prsm' : it plays, or is about to */
        pthread_mutex_lock(&air_mu); air_go_said = now_s(); pthread_mutex_unlock(&air_mu);
    }
    /* macOS: the receiver roomd runs (src.h): its input stopped at once on the sender's pause, stop or end (the
     * stop said above first: play() then cuts at once, as on Linux), its session (conn … disc) */
    src_air_item(code);
    if (code == 0x6d647374u) { air_np.in = 1; air_np.t[0] = air_np.a[0] = air_np.l[0] = 0; return; }   /* 'mdst' */
    if (code == 0x6d64656eu) { if (air_np.in) air_np_commit(); air_np.in = 0; return; }                  /* 'mden' */
    if (code == 0x50494354u) { air_np.t_pict = now_s(); netout_np_cover(NETOUT_NP_AIRPLAY, d, n); return; }   /* 'PICT' */
    if (code == 0x636f706cu) { if (n == n_full) air_plist(d, n); return; }   /* 'copl' : a sender's plist */
    if (code == 0x636f6e6eu) { memset(&air_np, 0, sizeof air_np); netout_np_clear(NETOUT_NP_AIRPLAY); netout_dev_volume_new(); atomic_store(&air_vol_fresh, 1); }  /* 'conn' */
    if (code == 0x70766f6cu) {
        char pv[64];
        air_keep(pv, sizeof pv, d, n);
        char *e;
        const double db = strtod(pv, &e);
        if (e != pv && db <= 0 && db >= -200) {
            char spec[160];
            pthread_mutex_lock(&air_mu);
            const int was = isnan(air_pvol) || fabs(air_pvol - db) > 0.01;
            air_pvol = db;
            snprintf(spec, sizeof spec, "%s", dv_spec);
            pthread_mutex_unlock(&air_mu);
            const int live = atomic_load(&dv_live), vlive = atomic_load(&air_vol_live);
            if (live) netout_dev_volume(spec, db);
            double lv = NAN;
            if (vlive) lv = page_vol_src(VS_AIRPLAY, db <= -144 ? NAN : db, db <= -144, atomic_exchange(&air_vol_fresh, 0), 0);
            if (was) logf_("AirPlay: the sender's volume %.2f dB%s", db, live ? ", set on the device" : lv == lv ? ", the player's volume" : "");
        }
        return;
    }
    if (code == 0x70666c73u || code == 0x70656e64u || code == 0x70617573u) return;   /* 'pfls', 'pend', 'paus': said above */
    char v[96];
    air_keep(v, sizeof v, d, n);
    if (code == 0x73647363u) src_air_format(v);                        /* 'sdsc' : macOS, its pipe's layout (src.h) */
    pthread_mutex_lock(&air_mu);

    if (code == 0x636f6e6eu || code == 0x736e7561u) { air_app[0] = air_app_label[0] = air_media[0] = 0; air_sess_gen++; }
    if (code == 0x636f6e6eu) { air_name[0] = 0; air_model[0] = 0; }
    else if (code == 0x736e7561u) air_model[0] = 0;
    else if (code == 0x736e616du) snprintf(air_name, sizeof air_name, "%s", v);   /* 'snam' */
    else if (code == 0x73747970u) snprintf(air_styp, sizeof air_styp, "%.15s", v);   /* 'styp' */
    else if (code == 0x73647363u) {                                     /* 'sdsc' , said as the decoder starts */
        snprintf(air_sdsc, sizeof air_sdsc, "%.47s", v);
        /* the kind of stream from it, before styp says it: AirPlay 2's Realtime is ALAC 44.1 kHz 16 bits, its Buffered
         * AAC (5.1, 7.1 too) or ALAC 48 kHz 24 bits (shairport-sync 5.5.1, AIRPLAY2.md) */
        if (!strncmp(v, "AAC", 3) || strstr(v, "48000/S24")) snprintf(air_styp, sizeof air_styp, "Buffered");
        else if (strstr(v, "44100/S16")) snprintf(air_styp, sizeof air_styp, "Realtime");
    }
    else if (code == 0x636d6f64u && v[0]) {                             /* 'cmod' */
        snprintf(air_model, sizeof air_model, "%.47s", v);
        air_last = model_video(air_model);
        if (air_name[0]) {
            int k = 0;
            while (k < air_seen_n && strcmp(air_seen[k].name, air_name)) k++;
            if (k == air_seen_n) { if (air_seen_n < AIR_SENDERS) air_seen_n++; else k = AIR_SENDERS - 1; }
            snprintf(air_seen[k].name, sizeof air_seen[k].name, "%s", air_name); air_seen[k].video = air_last;
        }
        logf_("AirPlay: sender \"%s\", %s: %s", air_name, air_model, air_last ? "video (an Apple TV)" : "music");
    }
    pthread_mutex_unlock(&air_mu);
}
/* base64 (whitespace skipped), at most len bytes kept; the bytes decoded */
static size_t b64(const char *s, const char *e, unsigned char *out, size_t len) {
    unsigned acc = 0; int bits = 0; size_t w = 0;
    for (; s < e; s++) {
        const char c = *s;
        const int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
                    : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;                                            /* '=', newlines */
        acc = (acc << 6) | (unsigned)v; bits += 6;
        if (bits >= 8) { bits -= 8; if (w < len) out[w++] = (unsigned char)(acc >> bits); acc &= (1u << bits) - 1; }
    }
    return w;
}
/* The items in buf[0, *len): each whole one handled, what is left of an unfinished one kept at the start. */
static void air_parse(char *buf, size_t *len) {
    buf[*len] = 0;
    char *p = buf;
    for (;;) {
        char *s = strstr(p, "<item>");
        if (!s) {                                                       /* keep a tail that may be the start of "<item>" */
            const size_t keep = *len - (size_t)(p - buf) < 5 ? *len - (size_t)(p - buf) : 5;
            memmove(buf, buf + *len - keep, keep); *len = keep; return;
        }
        char *e = strstr(s, "</item>");
        if (!e) { *len -= (size_t)(s - buf); memmove(buf, s, *len); return; }
        *e = 0;
        unsigned type = 0, code = 0, n = 0;
        if (sscanf(s, "<item><type>%x</type><code>%x</code><length>%u</length>", &type, &code, &n) == 3) {
            /* a picture whole (PICT: up to the pipe's buffer), a sender's plist too (copl), the rest 255 bytes
             * at most (the capture says the length it had) */
            unsigned char small[256], *d = small;
            size_t cap = sizeof small, dn = 0;
            if ((code == 0x50494354u || code == 0x636f706cu) && n > cap && n <= (8u << 20) && (d = malloc(n))) cap = n;
            if (!d) { d = small; cap = sizeof small; }
            char *ds = strstr(s, "<data"), *de = ds ? strstr(ds, "</data>") : NULL;
            if (ds && de && (ds = strchr(ds, '>')) && ds < de) dn = b64(ds + 1, de, d, cap);
            if (code != 0x50494354u || dn == n) air_item(type, code, d, dn < n ? dn : n, n);   /* a picture cut: not given */
            if (d != small) free(d);
        }
        p = e + 7;
    }
}
static void *air_meta_main(void *arg) {
    (void)arg;
    {   /* normal priority, whatever the audio thread's (--rt): it was made after it */
        struct sched_param sp = { .sched_priority = 0 };
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    }
    /* room for a cover (PICT, base64: 4/3 of it), 6 MB */
    static char *buf;
    const size_t BUF = 6u << 20;
    if (!buf && !(buf = malloc(BUF + 1))) return NULL;
    size_t len = 0;
    int fd = -1, said = 0;
    struct stat fs = { 0 };
    while (!quit) {
        if (fd < 0) {
            struct stat st;
            if (stat(AIR_META, &st) || !S_ISFIFO(st.st_mode)) { sleep(2); continue; }
            fd = open(AIR_META, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) { if (!said++) logf_("AirPlay: %s not opened (%s): the senders' models unknown", AIR_META, strerror(errno)); sleep(2); continue; }
            fstat(fd, &fs); len = 0; said = 0; air_meta_open = 1;
            logf_("AirPlay: the senders read from %s (shairport-sync's metadata)", AIR_META);
        }
        struct pollfd p = { fd, POLLIN, 0 };
        const int r = poll(&p, 1, 2000);
        if (r <= 0) {                                                   /* quiet: the FIFO still the same one? */
            struct stat st;
            if (stat(AIR_META, &st) || st.st_ino != fs.st_ino || st.st_dev != fs.st_dev) { close(fd); fd = -1; air_meta_open = 0; }
            continue;
        }
        const ssize_t n = read(fd, buf + len, BUF - len);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) { close(fd); fd = -1; air_meta_open = 0; continue; }
        len += (size_t)n;
        air_parse(buf, &len);
        if (len >= BUF) len = 0;                                        /* one item larger than all of it: dropped */
    }
    if (fd >= 0) close(fd);
    return NULL;
}

static void air_ctx_for(air_ctx *x, const char *styp_as) {
    memset(x, 0, sizeof *x);
    char app[96], lab[64], name[96], model[48], styp[16], sdsc[48], raw[300], kind[40] = "";
    pthread_mutex_lock(&air_mu);
    snprintf(app, sizeof app, "%s", air_app); snprintf(lab, sizeof lab, "%s", air_app_label);
    snprintf(name, sizeof name, "%s", air_name); snprintf(model, sizeof model, "%s", air_model); snprintf(styp, sizeof styp, "%s", styp_as ? styp_as : air_styp);
    snprintf(sdsc, sizeof sdsc, "%s", air_sdsc);
    pthread_mutex_unlock(&air_mu);

    if (styp[0]) snprintf(kind, sizeof kind, "%s%s", styp, !styp_as && strstr(sdsc, "7.1") ? " 7.1" : !styp_as && strstr(sdsc, "5.1") ? " 5.1" : "");
    for (int pass = 0; pass < 2; pass++) {
        const int with = pass == 0;                    /* with the kind of stream, then without it */
        if (with && !styp[0]) continue;
        if (app[0]) {
            snprintf(raw, sizeof raw, "app:%s%s%s", app, with ? " · " : "", with ? styp : "");
            am_clean(x->key[x->n], CTX_KEY, (const unsigned char *)raw, strlen(raw));
            snprintf(raw, sizeof raw, "%s%s%s", lab[0] ? lab : app, with ? " · " : "", with ? kind : "");
            am_clean(x->label[x->n], sizeof x->label[0], (const unsigned char *)raw, strlen(raw));
            x->lvl[x->n++] = "app";
        }
        if (name[0] || model[0]) {
            snprintf(raw, sizeof raw, "sender:%s%s%s%s%s", model, model[0] && name[0] ? " " : "", name, with ? " · " : "", with ? styp : "");
            am_clean(x->key[x->n], CTX_KEY, (const unsigned char *)raw, strlen(raw));
            snprintf(raw, sizeof raw, "%s%s%s", name[0] ? name : model, with ? " · " : "", with ? kind : "");
            am_clean(x->label[x->n], sizeof x->label[0], (const unsigned char *)raw, strlen(raw));
            x->lvl[x->n++] = "sender";
        }
    }
    if (styp[0]) {
        snprintf(raw, sizeof raw, "stream:%s", styp);
        am_clean(x->key[x->n], CTX_KEY, (const unsigned char *)raw, strlen(raw));
        snprintf(raw, sizeof raw, "AirPlay %s", styp);
        am_clean(x->label[x->n], sizeof x->label[0], (const unsigned char *)raw, strlen(raw));
        x->lvl[x->n++] = "stream";
    }
}
static void air_ctx_now(air_ctx *x) { air_ctx_for(x, NULL); }
/* the session's kind of stream now: 1 Buffered, 0 Realtime, −1 not said */
static int air_styp_buffered(void) {
    pthread_mutex_lock(&air_mu);
    const int r = !strcmp(air_styp, "Buffered") ? 1 : !strcmp(air_styp, "Realtime") ? 0 : -1;
    pthread_mutex_unlock(&air_mu);
    return r;
}

static int air_video_for(int setting, char *why, size_t len) {
    if (setting != AIR_AUTO) { snprintf(why, len, "chosen on the page"); return setting == AIR_VIDEO; }
    pthread_mutex_lock(&air_mu);
    int v = -1;
    if (air_model[0]) { v = model_video(air_model); snprintf(why, len, "\"%s\", %s", air_name, air_model); }
    for (int k = 0; v < 0 && air_name[0] && k < air_seen_n; k++)
        if (!strcmp(air_seen[k].name, air_name)) { v = air_seen[k].video; snprintf(why, len, "\"%s\", known as %s", air_name, v ? "video" : "music"); }
    if (v < 0 && air_last >= 0) { v = air_last; snprintf(why, len, "\"%s\" not known: as the last sender", air_name); }
    if (v < 0) { v = 1; snprintf(why, len, "no sender known yet"); }
    pthread_mutex_unlock(&air_mu);
    return v;
}

/* ── Status ────────────────────────────────────────────────────────────────────────────────────── */

static void write_status(const char *state, unsigned rate, unsigned orate, const control *c, long clipped, long xruns, double ppm, double steady, long fill, long delay, double share, double level) {
    char path[760], tmp[760];
    snprintf(path, sizeof path, "%s/status", status_dir());
    snprintf(tmp, sizeof tmp, "%s/.status", status_dir());
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "state=%s\nrate=%u\nroom=%s\nbypass=%d\ngain=%.1f\nclipped=%ld\nxruns=%ld\nroon_slowed_ppm=%.1f\nsteady_ppm=%.1f\ndac_fill=%ld\ndac_delay=%ld\n"
               "dynamic=%s\nshare=%.0f\nlevel=%.1f\nimage=%s\nrate_out=%u\n",
            state, rate, c->room, c->bypass, c->gain, clipped, xruns, ppm, steady, fill, delay, c->dyn, share * 100, level, c->image,
            netout_conv_rate() ? netout_conv_rate() : orate);   /* a Link receiver's rate, converted here; the AirPlay bridge's 44.1 kHz */
    /* what the app reads : which engine, and whether Roon reaches it as a Squeezebox and in which format */
    fprintf(f, "variant=player\n");
    if (VOIE) {
        static time_t since = 0;
        if (!strcmp(state, "idle")) since = 0; else if (!since) since = time(NULL);
        fprintf(f, "voie=%s\nvoie_name=%s\nvoie_pid=%d\nvoie_unbound=%d\nsince=%ld\n", VOIE, VOIE_NAME ? VOIE_NAME : "", (int)getpid(), voie_unbound, (long)since);

        if (voie_page_port > 0) fprintf(f, "page_port=%d\n", voie_page_port);
    }
    fprintf(f, "version=%s\nlang=%s\nsqueeze=%d\nformat=%s\n", ROOMD_VERSION, ROOMD_LANG, SQ_SERVER ? sq_connected() : 0, SQ_SERVER ? sq_format() : "");
    /* the output : the control's out= (empty: the command line's), and its kind */
    fprintf(f, "place=%s\nplace_none=%d\n", c->place, c->noroom);
    fprintf(f, "heard=%s\n", strcmp(state, "idle") ? st_heard : "");
    fprintf(f, "out=%s\nout_kind=%s\n", c->out, !strcmp(state, "idle") ? (netout_is_net(c->out) && NET_ON ? "net" : !strncmp(c->out, OUT_CARD_PREFIX, OUT_CARD_PREFIX_LEN) ? "alsa" : DIRECT_OUT ? "direct" : "alsa")
                                               : OUT == OUT_NET ? "net" : OUT == OUT_DIRECT ? "direct" : "alsa");
    fprintf(f, "out_why=%s\nout_max=%u\n", strcmp(state, "busy") ? "" : st_out_why, strcmp(state, "busy") ? 0 : st_out_max);
    /* (22/10, point 9) the Link receiver's gaps this stream (its underruns: micro-cuts never silent), a voie's too */
    if (OUT == OUT_NET && !strcmp(state, "playing")) fprintf(f, "link_gaps=%ld\n", netout_link_gaps());
    {   /* the network output not answering: when it is asked again (netout.c's backoff), its name */
        char rn[96] = "";
        const double rw = OUT == OUT_NET || netout_is_net(c->out) ? netout_retry_in(rn, sizeof rn) : 0;
        fprintf(f, "out_retry_s=%.0f\nout_retry_name=%s\n", ceil(rw), rn);
    }
    /* the AirPlay bridge's way here (08/10, netout.c): ok, taken (Roon Server holds UDP 3483 alone: the bridge must not
     * start, install.sh and its unit read this), off (no bridge: --airplay-out on not given; no network outputs) */
    fprintf(f, "airplay_disc=%s\n", netout_airplay_ready() > 0 ? "ok" : netout_airplay_ready() == 0 ? "taken" : "off");
    /* the player's latency, input to output (08/10: this play's, once measured), and the AirPlay input's offset */
    if (!strcmp(state, "playing") && st_lat_ms >= 0) fprintf(f, "input_latency_ms=%.0f\n", st_lat_ms); else fprintf(f, "input_latency_ms=\n");
    if (!isnan(air_file_s)) fprintf(f, "airplay_offset_ms=%.0f\n", air_file_s * 1000); else fprintf(f, "airplay_offset_ms=\n");

    if (!isnan(air_need_ms)) fprintf(f, "airplay_need_ms=%.0f\n", air_need_ms); else fprintf(f, "airplay_need_ms=\n");
    fprintf(f, "airplay_missing_ms=%.0f\nairplay_declares=%d\nout_lat_ms=%d\n", air_missing_ms, air_declares, c->out_lat);
    if (!strcmp(state, "idle")) fprintf(f, "input=\n");
    else if (st_input < 0) fprintf(f, "input=squeeze\n");
    else fprintf(f, "input=loop%d\n", st_input);
    /* AirPlay : this play's mode while AirPlay plays, else the one its next play would get now; its last sender
     * (name, model: "" not said); the ring the clock regulator keeps this play (ms, 0 idle) */
    if (!NO_LOOP) {
        char why[160];
        const int v = air_play_video >= 0 && strcmp(state, "idle") ? air_play_video : air_video_for(c->air_mode, why, sizeof why);
        pthread_mutex_lock(&air_mu);
        fprintf(f, "airplay_mode=%s\nairplay_sender=%s\nairplay_model=%s\n", v ? "video" : "music", air_name, air_model);
        pthread_mutex_unlock(&air_mu);
    }
    fprintf(f, "ring_target_ms=%.0f\n", strcmp(state, "idle") ? st_ring_ms : 0.0);

    fprintf(f, "airplay_ignore_volume=%d\nairplay_session=%d\n", air_iv_file, air_session);
    {   /* the sender's volume on the device: where it is ("" not forwarded now) */
        char dvs[200] = "";
        if (atomic_load(&dv_live)) netout_dev_volume_said(dvs, sizeof dvs);
        fprintf(f, "airplay_device_volume=%s\n", dvs);
    }
    fprintf(f, "airplay_name=%s\nairplay_name_next=%s\n", air_name_file, air_name_next);   /* its name, the output's */
    if (!NO_LOOP) {   /* the picture's sync by context (the session's, else the last one's), the calibration, the capture */
        air_ctx cx;
        air_ctx_now(&cx);
        int from;
        const int corr = corr_for(c, &cx, &from);
        fprintf(f, "airplay_ctx_key=%s\nairplay_ctx_label=%s\nairplay_ctx_level=%s\n", cx.n ? cx.key[0] : "", cx.n ? cx.label[0] : "", cx.n ? cx.lvl[0] : "");
        fprintf(f, "airplay_ctx_from=%s\nairplay_ctx_from_label=%s\nairplay_ctx_ms=%d\nairplay_ctx_adj_ms=%.0f\n", from >= 0 ? cx.lvl[from] : from == -1 ? "output" : "none",
                from >= 0 ? cx.label[from] : "", corr, strcmp(state, "idle") && air_play_video == 1 ? st_ctx_adj_ms : 0.0);
        fprintf(f, "airplay_live_left_ms=%.0f\n", strcmp(state, "idle") && air_play_video == 1 ? st_live_left_ms : 0.0);   /* */
        {

            int si = -1;
            for (int i = 0; i < cx.n && si < 0; i++) if (!strcmp(cx.lvl[i], "sender")) si = i;
            fprintf(f, "airplay_ctx_sender_key=%s\nairplay_ctx_sender_label=%s\n", si >= 0 ? cx.key[si] : "", si >= 0 ? cx.label[si] : "");
        }
        fprintf(f, "airplay_cal_ms=%.0f\nairplay_cal_measured=%d\n", !isnan(c->cal_ms) ? c->cal_ms : airplay_cal_ms, !isnan(c->cal_ms));
        /* the kind of stream now (or last), the Buffered streams' offset (empty: shairport-sync without
         * its line), the mode, the kind the last session played most */
        const int sb = air_styp_buffered();
        fprintf(f, "airplay_stream=%s\n", sb == 1 ? "Buffered" : sb == 0 ? "Realtime" : "");
        if (air_has_boff && !isnan(air_boff_s)) fprintf(f, "airplay_buffered_offset_ms=%.0f\n", air_boff_s * 1000); else fprintf(f, "airplay_buffered_offset_ms=\n");
        fprintf(f, "airplay_buffered_mode=%s\nairplay_session_kind=%s\n", c->air_buf_full ? "full" : "declared",
                air_bmain == 1 ? "buffered" : air_bmain == 0 ? "realtime" : "");
        const int want = atomic_load(&cap_want), on = atomic_load(&cap_on);
        fprintf(f, "airplay_meta_log=%s\n", on ? "capturing" : want == 2 && !atomic_load(&cap_plist) ? "waiting" : want ? "armed" : c->meta_log > 0 ? "done" : "off");
    }
    { char sl[1600]; if (src_status(sl, sizeof sl)) fputs(sl, f); }   /* macOS: the AirPlay receiver roomd runs (src.h) */
    dmx_status(f, state);                /* the source's channels, AirPlay's choice (air_channels.h) */
    {
        char why[200];
        const int live = strcmp(state, "idle") != 0;
        const int bpv = live ? bp_state(why, sizeof why) : -1;
        fprintf(f, "bit_perfect=%d\nbit_perfect_why=%s\nbit_perfect_spans=%ld\nbit_perfect_bad=%ld\n", bpv, live ? why : "",
                live ? atomic_load(&bp.ok) : 0, live ? atomic_load(&bp.bad) : 0);
        /* not bit-perfect by the speakers' channels alone (swapped, inverted: exact): lossless, verified; and
         * the channels this output plays with now (channels.h CH_* bits) */
        fprintf(f, "bit_perfect_lossless=%d\nchannels_now=%d\n", live && bpv == 0 ? bp_lossless() : 0, atomic_load(&chan_want));
        fprintf(f, "path_in_rate=%u\npath_in_bits=%d\npath_out_rate=%u\npath_out_bits=%d\npath_dev_rate=%u\npath_dac=%s\n",
                live ? st_bp_rate : 0, live ? atomic_load(&bp.bits_in) : 0, live ? st_bp_orate : 0, live ? st_bp_obits : 0,
                live ? st_bp_dev : 0, live ? st_bp_dac : "");
    }
    fclose(f);
    rename(tmp, path);
}
/* The bridge's start reads airplay_disc=ok (install.sh: its unit's ExecStartPre): off once this player is gone, and
 * until this run answers (08/10, review: a status left by a stopped or crashed player said ok, and the bridge then
 * started found Roon Server on UDP 3483, its devices Roon's zones without the correction). */
static void status_disc_off(void) {
    char path[760], tmp[760], b[8192];
    snprintf(path, sizeof path, "%s/status", status_dir());
    snprintf(tmp, sizeof tmp, "%s/.status", status_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    const size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    if (!strstr(b, "airplay_disc=ok\n")) return;
    if (!(f = fopen(tmp, "w"))) return;
    for (char *l = b, *e; *l; l = e) {
        e = strchr(l, '\n'); e = e ? e + 1 : l + strlen(l);
        if (strncmp(l, "airplay_disc=", 13)) fwrite(l, 1, (size_t)(e - l), f);
    }
    fputs("airplay_disc=off\n", f);
    if (fclose(f) == 0) rename(tmp, path);
}

/* ── The DAC ───────────────────────────────────────────────────────────────────────────────────── */

static int out_fmt = OUTF_S32;                 /* this play's: PCM's 32-bit words (22/10: packed by out_alsa.c into the
                                                * best format the card takes, S16_LE…), or DSD's */
static unsigned dac_periods = 16;
static out_pcm *dac_open(unsigned rate, unsigned long period) {
    return out_open(DAC, out_fmt, rate, period, dac_periods, dac_quiet);   /* non-blocking (out.h) */
}

/* The DAC refusing to open, try after try (06/10: another player holds the card, such as its own Roon zone; the engine
 * tries again every second): one line when it starts, then one a minute instead of one a second, one when it opens. */
static double dac_busy_t0, dac_busy_line;
static long dac_busy_tries = 0;
static void dac_refused(unsigned rate) {
    const double t = now_s();
    if (!dac_busy_tries++) {
        dac_busy_t0 = dac_busy_line = t;
        dac_quiet = 1;

        if (!out_takes(DAC, out_fmt, rate)) logf_("the DAC refuses %u Hz (above): it takes none of the formats we send at this rate (%s); trying again every second, a line a minute", rate, out_fmt == OUTF_S32 ? "PCM: S32, S24, S24_3, S16" : out_fmt_name(out_fmt));
        else logf_("the DAC refuses %u Hz (above): another player may hold it, such as its own Roon zone; trying again every second, a line a minute", rate);
    } else if (t - dac_busy_line >= 60) {
        dac_busy_line = t;
        logf_("the DAC still refuses %u Hz after %.0f s (%ld tries)", rate, t - dac_busy_t0, dac_busy_tries);
    }
}
static int dac_opened(void) {                    /* 1: after refusals (the status said busy) */
    const int was = dac_busy_tries > 0;
    if (was) logf_("the DAC opened after %.0f s of refusals (%ld tries)", now_s() - dac_busy_t0, dac_busy_tries);
    dac_busy_tries = 0;
    dac_quiet = 0;
    return was;
}
static void silence(out_pcm *out, const int32_t *zero, unsigned long period, long frames) {
    for (long i = 0; i < frames; i += (long)period) if (out_write(out, zero, period) < 0) break;
}

static out_pcm *dac_restart(out_pcm *out, src_pcm *in, unsigned rate, unsigned long period, long target, const int32_t *zero) {
    out_close(out);
    if (!(out = dac_open(rate, period))) return NULL;
    long backlog = in ? src_avail(in) : 0;
    silence(out, zero, period, target - (backlog > 0 ? backlog : 0));
    return out;
}

static void output_release(out_pcm *out, int finish) {
    if (OUT == OUT_NET) { netout_close(finish); return; }   /* finish: the music ended (the receiver plays out its buffer) */
    out_close(out);
}

typedef struct {
    int32_t *buf;                     /* frames × 2 */
    uint64_t mask;                    /* capacity − 1, in frames (a power of two) */
    _Atomic uint64_t w, r;            /* frames written, read, since the start */
} ring;

static uint64_t ring_fill(ring *q) { return atomic_load_explicit(&q->w, memory_order_acquire) - atomic_load_explicit(&q->r, memory_order_acquire); }
static void ring_push(ring *q, const int32_t *src, uint64_t n) {
    uint64_t w = atomic_load_explicit(&q->w, memory_order_relaxed);
    for (uint64_t i = 0; i < n; i++) { uint64_t k = (w + i) & q->mask; q->buf[2 * k] = src[2 * i]; q->buf[2 * k + 1] = src[2 * i + 1]; }
    atomic_store_explicit(&q->w, w + n, memory_order_release);
}
static void ring_pop(ring *q, int32_t *dst, uint64_t n) {              /* the writer only */
    uint64_t r = atomic_load_explicit(&q->r, memory_order_relaxed);
    for (uint64_t i = 0; i < n; i++) { uint64_t k = (r + i) & q->mask; dst[2 * i] = q->buf[2 * k]; dst[2 * i + 1] = q->buf[2 * k + 1]; }
    atomic_store_explicit(&q->r, r + n, memory_order_release);
}

#define AIR_LIVE_FADE 256                /* frames: ~6 ms at 44.1 kHz */
#define AIR_LIVE_FLOOR_MS 100.0          /* the ring never brought below this by a press (stalls still covered) */
static void ring_fade(ring *q, uint64_t from, uint64_t n, int in) {
    for (uint64_t i = 0; i < n; i++) {
        const double g = in ? (double)(i + 1) / (double)n : 1.0 - (double)(i + 1) / (double)n;
        const uint64_t k = (from + i) & q->mask;
        q->buf[2 * k] = (int32_t)llround(q->buf[2 * k] * g); q->buf[2 * k + 1] = (int32_t)llround(q->buf[2 * k + 1] * g);
    }
}
/* n > 0: n frames of silence added; n < 0: up to −n frames taken back (what was, returned). *fin: where to fade in. */
static long ring_live(ring *q, long n, uint64_t cap, unsigned long period, uint64_t *fin) {
    const uint64_t w = atomic_load_explicit(&q->w, memory_order_relaxed), f = ring_fill(q);
    if (n > 0) {
        if (f + (uint64_t)n + AIR_LIVE_FADE > cap) n = f + AIR_LIVE_FADE < cap ? (long)(cap - f - AIR_LIVE_FADE) : 0;
        if (n <= 0) return 0;
        if (f > AIR_LIVE_FADE + 2 * period) ring_fade(q, w - AIR_LIVE_FADE, AIR_LIVE_FADE, 0);
        for (long i = 0; i < n; i++) { const uint64_t k = (w + (uint64_t)i) & q->mask; q->buf[2 * k] = q->buf[2 * k + 1] = 0; }
        atomic_store_explicit(&q->w, w + (uint64_t)n, memory_order_release);
        *fin = w + (uint64_t)n;
        return n;
    }
    const uint64_t keep = 2 * (uint64_t)period + 2 * AIR_LIVE_FADE;
    uint64_t m = (uint64_t)(-n);
    if (f <= keep) return 0;
    if (m > f - keep) m = f - keep;
    atomic_store_explicit(&q->w, w - m, memory_order_release);
    ring_fade(q, w - m - AIR_LIVE_FADE, AIR_LIVE_FADE, 0);
    *fin = w - m;
    return -(long)m;
}

/* The DAC stopping again and again : stops less than STOP_RUN_S apart count as one run; within a run, each new
 * try waits STOP_WAIT_MS, doubling up to STOP_WAIT_MAX_MS, and the log says it at most once in STOP_LOG_S. */
#define STOP_RUN_S 10.0
#define STOP_WAIT_MS 50L
#define STOP_WAIT_MAX_MS 2000L
#define STOP_LOG_S 10.0

typedef struct {
    pthread_t th;
    out_pcm *out;
    unsigned rate;
    unsigned long period, outbuf;
    long target;                      /* DAC fill kept with silence while the ring has nothing */
    _Atomic uint64_t prime;           /* frames the ring holds before the DAC takes from it (again); a network receiver
                                       * from a Loopback source: the level kept once anchored (08/10, below) */
    ring *q;
    _Atomic int stop, dead, pause;    /* pause (Squeezebox): silence written, the ring's music kept for after */
    _Atomic long underruns, silences, fill, skip;

    double dry_t0; _Atomic long dry_ms, dry_held_ms;
    int32_t idle;                     /* silence: 0, or DSD's 0x69 pattern */
    int dop;                          /* DoP: the writer marks each frame, 0x05 then 0xFA, in the order they go out */
    int net, sq;                      /* a network receiver ; fed by the Squeezebox input: no silence ever sent */
    int az;                           /* an audiolaz receiver (anet.h): DAC-clocked, private link: started at once,
                                       * a small ring, no music dropped, no silence while it still holds music */
    int vid;                          /* an AirPlay video play to a network receiver , or any Loopback source's music
                                       * to an AirPlay device : it holds little (hold_max) */
    double hold_ms;                   /* then: what it may hold unplayed beyond the bridge's own latency (ms) */
    double hold_max_s;                /* and the most it is let hold when it does not play on that (s) */
    int upnp;                         /* a UPnP renderer's music held so: filled as before if it never starts;
                                       * its AirPlay video too */
    double step_s;                    /* before it plays: its hold raised every step_s while it does not start */
    const char *what;
    int vgate;                        /* and a Squeezebox player or the AirPlay bridge (not UPnP): its start on top of the ring */
    int L;                            /* the DAC's frames per stream frame */
    int obits, chk;                   /* the output's bits (its PCM's grid) and its words' kind (channels.h CH_PCM,
                                       * CH_DOP, CH_DSD), for the speakers' channels */
    _Atomic int full;                 /* a network receiver from a Loopback source: the music started (it was full;
                                       * AirPlay video: the receiver heard playing) */
    _Atomic long hold;                /* AirPlay video to a network receiver: what it may hold unplayed (hold_max) */
    _Atomic int ending;               /* the source has ended (Roon's stream over, or at another rate next; the Loopback's
                                       * source stopped): the ring running out then is the music's end, not a gap */
    /* For the player's latency (08/10, below): the frames of the chunk in hand not written yet, and a card's own delay
     * (snd_pcm_delay: its buffer and what its driver adds) when last written, with its time (CLOCK_MONOTONIC, ns). */
    _Atomic long hand, delay;
    _Atomic int64_t delay_ns;
    double t0;
    uint64_t start;                   /* the level the play's first music starts on (≤ prime; 0: prime's) */

    _Atomic int say_first, say_fill;
    double first_ms, first_ring_ms, first_out_ms, fill_ms, fill_after_s, fill_low_ms;
} writer;

/* squeeze.c's QKEEP : the frames sq_unread can give back, in the stream's frames (2.7 s at 192 kHz). */
#define SQ_KEEP (1L << 19)

static long net_hold_max(unsigned orate, int L) {
    const long a = (long)orate * 3, b = SQ_KEEP / 2 * (long)L;
    return a < b ? a : b;
}

#define NET_VIDEO_HOLD_MS 350
#define NET_VIDEO_HOLD_MAX_S 4.0
static double net_video_hold_ms = NET_VIDEO_HOLD_MS;

#define NET_AIRPLAY_MUSIC_HOLD_MS 1000
static double net_airplay_hold_ms = NET_AIRPLAY_MUSIC_HOLD_MS;

#define NET_UPNP_MUSIC_HOLD_MS 1500
#define NET_UPNP_RING_MS 1000

#define NET_AZ_RING_MS 300
#define NET_AZ_QUIET_MS 150   /* the ring dry: nothing sent while it holds more than this (no silence in the music) */
#define NET_UPNP_HOLD_MAX_S 8.0
#define NET_UPNP_FILL_AFTER_S 20.0

#define NET_UPNP_START_STEP_S 3.0
static double net_upnp_hold_ms = NET_UPNP_MUSIC_HOLD_MS;
/* A receiver fed from a Loopback source that never stalls 0.2 s (08/10, review: one reading its period every 10 ms
 * once full never sets netout_was_full): the music starts anyway this long after the ring reached its level. */
#define NET_FULL_WAIT_S 10.0

/* What a network receiver played of this play's music, in the ring's frames : what it took less what it holds.
 * With the Squeezebox input only music is sent, so its stream and the ring count alike. */
static uint64_t net_heard(ring *q) {
    const uint64_t s = netout_sent(), h = (uint64_t)netout_held(), r = atomic_load(&q->r), p = s > h ? s - h : 0;
    return p < r ? p : r;
}
/* The music read and not heard yet, in the ring's frames: the ring's, and the DAC's buffer (a card's) or what the network
 * receiver holds and what is packed for it. */
static uint64_t unheard(ring *q, writer *W) {
    if (W->net) return ring_fill(q) + (atomic_load(&q->r) - net_heard(q));
    return ring_fill(q) + (uint64_t)atomic_load(&W->fill);
}
/* A network receiver missing (08/10: not connected, not found, not fetching): as the DAC refusing, a line when it
 * starts, then one a minute, one when it plays. */
static double net_busy_t0, net_busy_line;
static long net_busy_tries = 0;
static void net_refused(void) {
    const double t = now_s();
    /* an audiolaz receiver another player plays to: netout.c's own line, its questions spaced (2, 5, 10, 15 s) */
    if (netout_busy(NULL, NULL, 0)) { if (!net_busy_tries++) net_busy_t0 = net_busy_line = t; return; }
    if (!net_busy_tries++) { net_busy_t0 = net_busy_line = t; logf_("network output: %s; trying again every second, a line a minute", netout_why()); }
    else if (t - net_busy_line >= 60) { net_busy_line = t; logf_("network output still not there after %.0f s (%ld tries): %s", t - net_busy_t0, net_busy_tries, netout_why()); }
}
static void net_opened(void) {
    if (net_busy_tries) logf_("network output there after %.0f s (%ld tries)", now_s() - net_busy_t0, net_busy_tries);
    net_busy_tries = 0;
}

#define CAL_MAX 32
enum { CAL_IDLE, CAL_ARMED, CAL_MARKED };
static struct {
    _Atomic int state;
    int n;
    double at_ms[CAL_MAX];            /* the clicks' places in the track (ms from its first frame) */
    uint64_t idx[CAL_MAX];            /* their places in the ring (its frames, the DAC's) */
    _Atomic int64_t exit_ns[CAL_MAX]; /* when each leaves the output, by the player's account (CLOCK_MONOTONIC), 0: not yet */
    unsigned rate, orate;
    char out[160];
    double t_armed;
} cal;
static pthread_mutex_t cal_mu = PTHREAD_MUTEX_INITIALIZER;
static int64_t mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }
/* page.c (page.h): clicks at[0, n) ms asked; 0, or −1 (too many, none, out of order) */
int roomd_cal_arm(const double *at, int n) {
    if (n < 1 || n > CAL_MAX) return -1;
    for (int i = 0; i < n; i++) if (!(at[i] >= 0 && at[i] < 120000) || (i && at[i] <= at[i - 1])) return -1;
    pthread_mutex_lock(&cal_mu);
    atomic_store(&cal.state, CAL_IDLE);
    cal.n = n;
    for (int i = 0; i < n; i++) { cal.at_ms[i] = at[i]; cal.idx[i] = 0; atomic_store(&cal.exit_ns[i], 0); }
    cal.rate = cal.orate = 0; cal.out[0] = 0; cal.t_armed = now_s();
    atomic_store(&cal.state, CAL_ARMED);
    pthread_mutex_unlock(&cal_mu);
    logf_("calibration: %d clicks asked, the track next (the app's)", n);
    return 0;
}
void roomd_cal_cancel(void) { atomic_store(&cal.state, CAL_IDLE); }
long long roomd_clock_ns(void) { return (long long)mono_ns(); }
/* {"state":"idle|armed|playing|done","now":<ns>,"rate":…,"orateHz":…,"out":"…","marks":[{"i":0,"atMs":…,"ns":…},…]}
 * (ns 0: not left yet). done: every click left the output, or the track over. */
int roomd_cal_json(char *b, size_t len) {
    pthread_mutex_lock(&cal_mu);
    int st = atomic_load(&cal.state);
    if (st == CAL_ARMED && now_s() - cal.t_armed > 60.0) { atomic_store(&cal.state, CAL_IDLE); st = CAL_IDLE; }   /* never came */
    int all = cal.n > 0;
    for (int i = 0; i < cal.n; i++) if (!atomic_load(&cal.exit_ns[i])) all = 0;
    const char *s = st == CAL_IDLE ? "idle" : st == CAL_ARMED ? "armed" : all || sq_measuring() != 1 ? "done" : "playing";
    char o[200];
    size_t w = 0;
    for (const char *p = cal.out; *p && w + 1 < sizeof o; p++) o[w++] = *p == '"' || *p == '\\' || (unsigned char)*p < 0x20 ? ' ' : *p;
    o[w] = 0;
    int n = snprintf(b, len, "{\"state\":\"%s\",\"now\":%lld,\"rate\":%u,\"orateHz\":%u,\"out\":\"%s\",\"marks\":[", s, (long long)mono_ns(), cal.rate, cal.orate, o);
    for (int i = 0; st == CAL_MARKED && i < cal.n && n > 0 && (size_t)n < len; i++)
        n += snprintf(b + n, len - (size_t)n, "%s{\"i\":%d,\"atMs\":%.3f,\"ns\":%lld}", i ? "," : "", i, cal.at_ms[i], (long long)atomic_load(&cal.exit_ns[i]));
    if (n > 0 && (size_t)n < len) n += snprintf(b + n, len - (size_t)n, "]}");
    pthread_mutex_unlock(&cal_mu);
    return n > 0 && (size_t)n < len ? n : -1;
}

/* The writer, after handing the output frames up to the ring's r_end, q of them in the output then (not played yet,
 * by the player's account): the clicks among them dated (each frame q − (r_end − idx) frames from leaving). */
static void cal_note(uint64_t r_end, double q, unsigned orate) {
    if (atomic_load_explicit(&cal.state, memory_order_acquire) != CAL_MARKED) return;
    int64_t now = 0;
    for (int i = 0; i < cal.n; i++) {
        if (cal.idx[i] >= r_end || atomic_load(&cal.exit_ns[i])) continue;
        if (!now) now = mono_ns();
        const double ahead = q - (double)(r_end - cal.idx[i]);
        int64_t t = now + (int64_t)llround(ahead * 1e9 / orate);
        if (!t) t = 1;
        atomic_store(&cal.exit_ns[i], t);
    }
}

static void *writer_main(void *arg) {
    writer *W = arg;
#if defined(__linux__) && defined(_GNU_SOURCE)
    pthread_setname_np(pthread_self(), "az-writer");   /* named in the journal's real-time lines, ps -L */
#endif
#ifdef __APPLE__
    /* macOS refuses SCHED_FIFO 94 to a user process (EINVAL above 47: this line said so at each play, and the
     * writer stayed at the default priority). A Core Audio card's writer turns Mach real time at its first write
     * (out_coreaudio.c, writer_rt, its device's period); a network receiver's (audiolaz Link: the queue of the packets'
     * thread kept full, every packet's time, ANET_PKT_MAX_US) here, in a Mach time constraint on 2 ms, a quarter of
     * it computing (a period copied: a few µs) */
    if (writer_rt > 0 && W->net) { const int e = rt_clock(0.002, 0.25, 0); if (e) logf_("the network writer stays at normal priority (Mach time constraint refused)"); }
#else
    if (writer_rt > 0) {
        const int e = rt_clock(0, 0, writer_rt);
        if (e) logf_("the DAC's writer stays at normal priority (%s)", strerror(e));
    }
#endif
#if defined(__linux__) && defined(_GNU_SOURCE)   /* the portable build: not pinned */
    if (writer_cpu >= 0) {
        cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(writer_cpu, &cs);
        int e = pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
        if (e) logf_("the DAC's writer not pinned to CPU %d (%s)", writer_cpu, strerror(e));
    }
#endif
    const unsigned long P = W->period;
    int32_t *pcm = malloc(sizeof(int32_t) * 2 * P), *zero = calloc(2 * P, sizeof(int32_t)), *zrest = zero;
    for (unsigned long i = 0; i < 2 * P; i++) zero[i] = W->idle;
    uint64_t dopn = 0;                               /* DoP: frames marked so far */
    /* the speakers' polarity (channels.h), as asked when the play starts (no crossfade into it then); * the swap is the play thread's, on the music */
    chan_stage chs;
    chan_init(&chs, atomic_load(&chan_want) & (CH_INV_L | CH_INV_R));
    if (W->dop) {                                    /* what the DAC is given again after a stop: marked already */
        zrest = malloc(sizeof(int32_t) * 2 * P);
        for (unsigned long i = 0; i < P; i++) { const int32_t m = (int32_t)((i & 1 ? 0xFAu : 0x05u) << 24); zrest[2 * i] = W->idle | m; zrest[2 * i + 1] = W->idle | m; }
    }
    const double ms = 1000.0 / W->rate;
    rt_guard wg = { 0 };                             /* (21/10, rt_thread.h) never a core held: 5 ms of work, a nap */
    double spin_for = 0, spin_at = rt_test_spin("writer", &spin_for);   /* the tests' (test/rt-safety.sh) */
    const double t_spin0 = now_s();
    unsigned long off = 0, len = 0;              /* the chunk being written: pcm[off … len) */
    uint64_t chunk_r0 = UINT64_MAX;                  /* its first frame's place in the ring (UINT64_MAX: silence) */
    int primed = 0, sound_said = 0, ever = 0;   /* ever: primed once this play */
    int fill_said = 0;                           /* the ring's first time at its level, said */
    double t_music = 0;
    uint64_t low = 0;
    int net_full = 0;                                /* a Loopback source to a receiver: primed once this play */
    double t_ready = 0;                              /* since when the ring is at its level, the receiver not full */
    long hold_max = W->net && W->sq ? net_hold_max(W->rate, W->L) : 0;   /* a Loopback source's: set once full, below */
    long start_sil = -1;                             /* the AirPlay bridge: the silence it starts on (below), -1 none */
    if (W->vid) {                                    /* AirPlay video: from the start , on top of what it cannot do without */
        const int floor_ms = netout_hold_floor_ms();
        hold_max = (long)(W->rate * (W->hold_ms + floor_ms) / 1000.0);

        if (W->vgate && floor_ms > 0) start_sil = (long)(W->rate * W->hold_ms / 1000.0) + (long)atomic_load(&W->prime);
        logf_("network receiver, %s: it may hold %.0f ms unplayed%s", W->what, W->hold_ms + floor_ms, floor_ms ? " (the AirPlay bridge's own latency in it)" : "");
        atomic_store(&W->hold, hold_max);
    }
    const double t_w0 = now_s();                     /* a UPnP renderer that never starts on what it may hold */
    long pre_drop = 0;                               /* and the music dropped while it starts (above the ring's level) */
    double gate_t = 0;                               /* video: since when held back by hold_max, and what it had played */
    uint64_t gate_p = 0;
    double v_started = 0;                            /* video: when it was first heard playing (0: not yet) */
    long v_drop = 0, v_dropped = 0;                  /* video: what its start left too late, still to drop; dropped */
    int stops = 0;                                   /* the DAC's stops in a row, each within STOP_RUN_S of the last */
    double last_stop = -1e9, last_line = -1e9;
    long unlogged = 0;                               /* stops since the last line in the log */
    while (!atomic_load(&W->stop)) {
        if (writer_rt > 0) rt_guard_step(&wg, 0.005, 0.0002);
        if (spin_at > 0 && now_s() - t_spin0 >= spin_at) { spin_at = 0; logf_("test: the writer spins %.1f s without a wait", spin_for); rt_test_spin_now(spin_for); }
        atomic_store(&W->hand, (long)(len - off));   /* the chunk in hand: on its way, not in the ring, not in the DAC */
        /* an audiolaz receiver's ring grown to what the network needs (anet.h, protocol 3): video may hold that more */
        if (W->az && W->vid && hold_max) { const long hx = (long)(W->rate * (W->hold_ms + netout_link_extra_ms()) / 1000.0); if (hx > hold_max) { hold_max = hx; atomic_store(&W->hold, hold_max); } }

        if (len == off) {
            const long skip = atomic_exchange(&W->skip, 0);
            if (skip > 0) { const uint64_t f = ring_fill(W->q), n = (uint64_t)skip < f ? (uint64_t)skip : f;
                atomic_store_explicit(&W->q->r, atomic_load(&W->q->r) + n, memory_order_release); if (n) bp_skip(); }
        }
        const int fresh = off == len;                /* a new chunk below (DoP marks it) */
        if (off == len && atomic_load(&W->pause)) {  /* paused: silence, the music waits in the ring */
            if (W->net) { usleep(5000); continue; }  /* a network receiver paused itself (netout_pause): nothing sent */
            memcpy(pcm, zero, sizeof(int32_t) * 2 * P); off = 0; len = P; chunk_r0 = UINT64_MAX;
        } else if (off == len) {                     /* next chunk: a period of music, or of silence to keep the DAC fed */
            uint64_t have = ring_fill(W->q);
            /* the level to start (again) at: the anchored one once the main loop kept it (08/10, review: a re-prime
             * back at the 3 s target, 2 s above the level kept, was 3 s of silence, then an hour at 600 ppm) */
            /* (13/10, bug 10) the play's first start on a little of it (W->start), the ring filling up behind: once
             * started, a ring run dry starts again on all of it */
            const uint64_t pr = !ever && W->start && W->start < atomic_load(&W->prime) ? W->start : atomic_load(&W->prime);
            /* A network receiver fed from a Loopback source : silence until its buffer is full (it pushed back
             * once), so that from then on it paces us and the ring is the margin, as with a card. Once started, a
             * receiver that ran dry starts again on the ring's level alone. One that never pushes back 0.2 s (it reads
             * steadily once full): the music NET_FULL_WAIT_S after the ring was ready, not silence for the whole play. */
            int go = !W->net || W->sq || W->vid || W->az || net_full || netout_was_full();   /* audiolaz: at once */
            if (!primed && !go && have >= pr) {
                if (!t_ready) t_ready = now_s();
                else if (now_s() - t_ready >= NET_FULL_WAIT_S) {
                    go = 1;
                    logf_("network receiver: never pushed back, the music started after %.0f s of silence", NET_FULL_WAIT_S);
                }
            }

            if (W->upnp && hold_max && !primed && have > pr + W->rate / 2) {
                const uint64_t d = have - pr;
                atomic_store_explicit(&W->q->r, atomic_load(&W->q->r) + d, memory_order_release);
                pre_drop += (long)d; have = pr;
            }
            if (!primed && have >= pr && go && (!W->upnp || !hold_max || netout_started())) {
                primed = 1; t_ready = 0; ever = 1;
                if (W->net && !W->sq && !net_full && W->vid) net_full = 1;   /* video: not filled (full once it plays, below) */
                else if (W->net && !W->sq && !net_full) {
                    net_full = 1; atomic_store(&W->full, 1);
                    /* (08/10, bench: the real gmediarender, GStreamer pushes back once, then reads on as fast as it is
                     * given: it held 30 to 38 s, the ring ran dry, silence sent, two gaps a play) from now on no more
                     * held unheard than when it was full, a second at least: past it, its playing paces us, as a
                     * card's clock; a receiver with a buffer of its own (squeezelite) never reaches it */
                    const long h = netout_held();
                    hold_max = h > (long)W->rate ? h : (long)W->rate;
                }
                /* the receiver full at last : what the source played meanwhile beyond the ring's level dropped,
                 * so that the ring starts at its level (a backlog left there was latency the clock correction then
                 * took ten minutes to drain at its 600 ppm: the real squeezelite's 9 s buffer, 08/10) */
                /* video : what the receiver will be given at its start (hold_max) kept on top of the ring's level, below */
                /* a UPnP renderer holds all it may already, in silence: the ring's level only */
                const uint64_t keep = pr + (W->vid && !W->upnp ? (uint64_t)hold_max : 0);
                if (W->net && !W->sq && !W->az && have > keep) {   /* never to an audiolaz receiver */
                    atomic_store_explicit(&W->q->r, atomic_load(&W->q->r) + (have - keep), memory_order_release);
                    if (W->upnp && hold_max) logf_("network receiver playing on its silence: %.0f ms of the source's music dropped, the ring at its level, the music next", (double)(have - keep + (uint64_t)pre_drop) * ms);
                    else logf_("network receiver full: %.0f ms of the source's music dropped, the ring at its level", (double)(have - keep) * ms);
                    have = keep;
                }
            }

            /* The AirPlay bridge (10/10, start_sil above): until it plays, the music as it comes beyond the ring's level,
             * what it may hold the bound (below); its own estimate of what it holds counts all it took then (its elapsed
             * time 0 for its RAOP latency), so this never gives it more than the latency kept. */
            const int v_wait = W->vid && W->vgate && !v_started && primed &&
                               (start_sil >= 0 ? (long)have < (long)pr + (long)P : (long)have + netout_held() < (long)pr + hold_max);
            if (primed && have >= P && !v_wait) {
                if (W->dry_t0 > 0) {                     /* the music back: that dry spell said (over 100 ms) */
                    const double d = now_s() - W->dry_t0;
                    W->dry_t0 = 0;
                    if (d >= 0.1) atomic_store(&W->dry_ms, (long)(d * 1000));
                }
                chunk_r0 = atomic_load(&W->q->r); ring_pop(W->q, pcm, P); off = 0; len = P;
                if (W->L == 1) bp_out(chunk_r0, pcm, P);   /* bitperfect.h: compared as it goes out */

                {
                    const int cw = atomic_load_explicit(&chan_want, memory_order_relaxed) & (CH_INV_L | CH_INV_R);
                    if (chan_run(&chs, cw, pcm, P, W->chk, W->obits, W->rate) && cw && !(atomic_load_explicit(&bp.why, memory_order_relaxed) & BP_POLARITY))
                        bp_say(BP_POLARITY);
                }
                /* (13/10, bug 10) started on a little of it: when the ring reached its level, and its lowest until then */
                if (W->start && !fill_said) {
                    if (!t_music) { t_music = now_s(); low = have; }
                    if (have < low) low = have;
                    if (have >= atomic_load(&W->prime)) {
                        fill_said = 1;
                        W->fill_ms = (double)have * ms; W->fill_after_s = now_s() - t_music; W->fill_low_ms = (double)low * ms;
                        atomic_store_explicit(&W->say_fill, 1, memory_order_release);
                    }
                }

                if (!sound_said) for (unsigned long i = 0; i < 2 * P; i++) if (pcm[i] != W->idle) {
                    sound_said = 1;
                    W->first_ms = (now_s() - W->t0) * 1000; W->first_ring_ms = (double)(have - P) * ms; W->first_out_ms = (double)atomic_load(&W->fill) * ms;
                    atomic_store_explicit(&W->say_first, 1, memory_order_release);
                    break;
                }
            }
            else if (W->vid && !v_started && primed) {   /* AirPlay video, the receiver not playing yet : it got the
                                                          * ring's level at once; the music as it comes, no silence before it */
                usleep(2000); continue;
            }
            /* an audiolaz receiver still holding music: the source late a moment is no gap there (it plays on from
             * what it holds); silence only once it holds less than NET_AZ_QUIET_MS */
            else if (W->az && primed && !atomic_load(&W->ending) && netout_held() > (long)(W->rate * (NET_AZ_QUIET_MS / 1000.0))) {
                if (W->dry_t0 == 0) { W->dry_t0 = now_s(); atomic_store(&W->dry_held_ms, (long)(netout_held() * ms)); }   /* */
                usleep(2000); continue;
            }
            else if (W->net && W->sq) {              /* Roon's music late, or its end: nothing sent , the receiver
                                                      * plays from its buffer; silence would shift Roon's elapsed time */
                usleep(2000); continue;
            }
            /* a UPnP renderer's music, playing: the ring short a moment (the source late, or the renderer given a
             * little more at once: its position read late) is no gap, it holds what it may (NET_UPNP_MUSIC_HOLD_MS):
             * nothing sent, the music as it comes; silence only once it holds less than a quarter of a second (before,
             * silence at once, and then until the ring was full again: a second of silence in the middle of the music) */
            else if (W->upnp && hold_max && primed && v_started && !atomic_load(&W->ending) && netout_held() > (long)(W->rate / 4)) {
                usleep(2000); continue;
            } else {

                if (primed) { primed = 0; if (!atomic_load(&W->ending)) atomic_fetch_add(&W->silences, 1); }
                /* the AirPlay bridge's start (start_sil above): no more silence than that before the music */
                if (start_sil >= 0 && !net_full && (long)netout_sent() >= start_sil) { usleep(2000); continue; }
                memcpy(pcm, zero, sizeof(int32_t) * 2 * P); off = 0; len = P; chunk_r0 = UINT64_MAX;
            }
        }
        if (fresh && W->dop)                         /* DoP's markers, alternating frame after frame */
            for (unsigned long i = 0; i < len; i++, dopn++) {
                const uint32_t m = (dopn & 1 ? 0xFAu : 0x05u) << 24;
                pcm[2 * i] = (int32_t)(((uint32_t)pcm[2 * i] & 0x00FFFFFFu) | m);
                pcm[2 * i + 1] = (int32_t)(((uint32_t)pcm[2 * i + 1] & 0x00FFFFFFu) | m);
            }
        if (W->net) {                                /* a network receiver (08/10, netout.c): its TCP window as the buffer */
            /* the Squeezebox input: no more held unheard than sq_unread gives back (hold_max), once the receiver
             * plays (before, it may want its whole buffer to start: never held back then); a Loopback source: no
             * more than it held when full (hold_max set above) */
            if (W->vid && !v_started && netout_sent() > (uint64_t)netout_held() + W->rate / 2) {
                /* video : playing at last (half a second played: what it says, not our guess between its
                 * reports). What the source played while it set itself up (the AirPlay bridge: a second or two, its
                 * RAOP session and latency) is late for the whole play, never drained (the receiver takes no faster
                 * than it plays): beyond the ring's level and beyond what the receiver may hold, dropped, from the
                 * ring as it comes (what the receiver holds too much it plays meanwhile, the ring filling as much) */
                v_started = now_s();
                if (!W->upnp) atomic_store(&W->full, 1);   /* the regulator's level kept from now (the main loop) */
                const long h = netout_held();
                const uint64_t have = ring_fill(W->q), pr = atomic_load(&W->prime);
                /* beyond the ring's level and what it may hold, both together: what it holds less than hold_max it
                 * takes from the ring as it plays (09/10: counted apart, that much more dropped, the ring left short) */
                v_drop = (long)have + h - (long)pr - hold_max;
                if (v_drop < 0 || W->upnp) v_drop = 0;   /* a UPnP renderer's: done as its music starts (primed, above) */
                v_dropped = 0;
            }
            /* a UPnP renderer's music: the level kept once what it played is known (netout_settled: its
             * position's lead settled), not before (kept at 10 s, it was the level of a guess still moving) */
            if (W->upnp && v_started && !atomic_load(&W->full) && netout_settled()) atomic_store(&W->full, 1);
            if (v_drop > 0) {
                const uint64_t have = ring_fill(W->q), pr = atomic_load(&W->prime);
                if (have > pr) {
                    const long d = (long)(have - pr) < v_drop ? (long)(have - pr) : v_drop;
                    atomic_store_explicit(&W->q->r, atomic_load(&W->q->r) + (uint64_t)d, memory_order_release);
                    v_drop -= d; v_dropped += d;
                }
                if (v_drop <= 0 || now_s() - v_started > 10.0) {
                    logf_("network receiver playing: %.0f ms of the source's music dropped (what came while it started), the ring at its level", (double)v_dropped * ms);
                    v_drop = 0;
                }
            }
            /* a UPnP renderer still not playing on all it may hold: filled as before (its whole buffer, silence
             * then the music: what it wants to start), no longer held */
            if (W->upnp && hold_max && !v_started && now_s() - t_w0 >= NET_UPNP_FILL_AFTER_S) {
                logf_("network receiver: not started after %.0f s on the %.0f ms it may hold: filled as it takes it (its whole buffer)",
                      NET_UPNP_FILL_AFTER_S, (double)hold_max * 1000.0 / W->rate);
                hold_max = 0; atomic_store(&W->hold, 0);
            }
            if (hold_max) {

                if (netout_lost()) { atomic_fetch_add(&W->underruns, 1); atomic_store(&W->dead, 1); logf_("network output lost at %u Hz: %s", W->rate, netout_why()); break; }
                const long h = netout_held();
                const uint64_t sn = netout_sent();
                /* video : held back from the start too (before it plays, all it took is held) */
                if (h > hold_max && (W->vid || sn > (uint64_t)h)) {
                    if (W->vid) {                    /* not playing what it holds: let it hold more (the header) */
                        const uint64_t pl = sn > (uint64_t)h ? sn - (uint64_t)h : 0;
                        const double tg = now_s();
                        if (!gate_t) { gate_t = tg; gate_p = pl; }
                        else if (tg - gate_t >= (W->upnp && !v_started ? W->step_s : 1.5)) {

                            if (pl - gate_p < (uint64_t)(W->rate / 2) && hold_max < (long)(W->rate * W->hold_max_s) && !(W->upnp && v_started)) {
                                hold_max = hold_max * 3 / 2;
                                if (hold_max > (long)(W->rate * W->hold_max_s)) hold_max = (long)(W->rate * W->hold_max_s);
                                logf_("network receiver: %s: it may hold %.0f ms now (held short: %s)", pl ? "it played little of what it holds" : "not started on what it holds",
                                      (double)hold_max * 1000.0 / W->rate, W->upnp ? "UPnP" : "AirPlay");
                                atomic_store(&W->hold, hold_max);
                            }
                            gate_t = tg; gate_p = pl;
                        }
                        usleep(5000); continue;
                    }
                    usleep(10000); continue;
                }
                gate_t = 0;
            }
            long nw = netout_write(pcm + 2 * off, (long)(len - off));
            if (nw == 0) { netout_wait(5); continue; }   /* its buffer full: its clock paces us */
            if (nw < 0) { atomic_fetch_add(&W->underruns, 1); atomic_store(&W->dead, 1); logf_("network output lost at %u Hz: %s", W->rate, netout_why()); break; }
            off += (unsigned long)nw;
            atomic_store(&W->fill, netout_held());
            if (chunk_r0 != UINT64_MAX) cal_note(chunk_r0 + off, (double)netout_held(), W->rate);   /* */
            continue;
        }
        long w = out_write(W->out, pcm + 2 * off, len - off);
        if (w == OUT_AGAIN) { out_wait(W->out, 50); continue; }   /* the DAC's buffer is full: as RAAT, wait for room */
        if (w < 0) {
            atomic_fetch_add(&W->underruns, 1);
            double t0 = now_s();
            /* A lone stop: opened again at once, the music resumes. Stops in a row : a card that keeps refusing
             * the music had this real-time thread spin round close, open, fill, refused, with no pause and a log line
             * each turn, the pattern of the kernel freeze of 16/04. Now each new try waits longer (STOP_WAIT_MS,
             * doubling to STOP_WAIT_MAX_MS) and the log says it once in STOP_LOG_S, with the count. */
            stops = t0 - last_stop < STOP_RUN_S ? stops + 1 : 1;
            last_stop = t0;
            long wait = stops > 1 ? STOP_WAIT_MS << (stops - 2 < 8 ? stops - 2 : 8) : 0;
            if (wait > STOP_WAIT_MAX_MS) wait = STOP_WAIT_MAX_MS;
            for (long waited = 0; waited < wait && !atomic_load(&W->stop); waited += 10) usleep(10000);
            if (atomic_load(&W->stop)) break;
            out_pcm *o = dac_restart(W->out, NULL, W->rate, P, W->target, zrest);
            if (!o) { W->out = NULL; atomic_store(&W->dead, 1); logf_("DAC stopped at %u Hz (%s) and cannot be opened again", W->rate, out_strerror(w)); break; }
            W->out = o;
            const double t1 = now_s();
            if (stops > 1 && t1 - last_line < STOP_LOG_S) unlogged++;
            else {
                if (stops > 1) logf_("DAC stopped at %u Hz (%s) %ld times since the last line, %d in a row: opened again each time, now %ld ms apart",
                                     W->rate, out_strerror(w), unlogged + 1, stops, wait);
                else logf_("DAC stopped at %u Hz (%s), opened again in %.0f ms; the ring holds %.0f ms, the music resumes where it stopped",
                           W->rate, out_strerror(w), (t1 - t0) * 1000, (double)ring_fill(W->q) * ms);
                last_line = t1;
                unlogged = 0;
            }
            continue;                                /* the chunk being written is written again */
        }
        off += (unsigned long)w;
        long av = out_avail(W->out);
        if (av >= 0) atomic_store(&W->fill, (long)W->outbuf - (long)av);
        long dl;                        /* the card's delay, as its driver says it (the latency, 08/10) */
        if (out_delay(W->out, &dl) == 0) {
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            atomic_store(&W->delay, (long)dl);
            atomic_store(&W->delay_ns, (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec);
            if (chunk_r0 != UINT64_MAX) cal_note(chunk_r0 + off, (double)dl, W->rate);   /* the clicks dated */
        } else if (chunk_r0 != UINT64_MAX && av >= 0) cal_note(chunk_r0 + off, (double)((long)W->outbuf - (long)av), W->rate);
    }
    if (zrest != zero) free(zrest);
    free(pcm); free(zero);
    rt_unregister();                                 /* its slot free (rt_thread.h) */
    return NULL;
}

/* ── One play: the DAC open at one rate, from Roon starting on the Loopback to it stopping or changing rate ─── */

static int play_again_now = 0;
static void play(unsigned rate, control *c, stamp *seen, int sq) {
    const double t_enter = now_s();
    /* An AirPlay play : video or music, decided now and kept to its end (the header) */
    const int air_play = !sq && !NO_LOOP && cur_sub == airplay_sub();
    char air_why[160] = "";
    const int video = air_play && air_video_for(c->air_mode, air_why, sizeof air_why);
    const int air_asked = c->air_mode;
    air_play_video = air_play ? video : -1;
    if (air_play) air_rate = rate;

    int drv_play = 0;
    if (!sq && !NO_LOOP) { char nm[32]; sub_input_name(cur_sub, nm, sizeof nm); drv_play = !strcmp(nm, "mac"); }
    dac_periods = drv_play ? 4 : 16;

    const int paced = !sq && !NO_LOOP && src_paced(cur_sub);
    if (paced) drv_play = 1;
    st_input = sq ? -1 : cur_sub;
    double play_in_peak = 0, play_out_peak = 0;          /* this play's peaks, input and output */
    /* whose title and cover the receivers show (nowplay.h): Roon (the Squeezebox input, or Roon / Roon Bridge
     * into the Loopback's subdevice 0), AirPlay, Spotify Connect; another input: none (the player's own card) */
    {
        char inm[32];
        sub_input_name(cur_sub, inm, sizeof inm);

        netout_np_source(sq || (!NO_LOOP && ((cur_sub == 0 && !(VOIE && inm[0])) || !strcmp(inm, "roonbridge") || !strcmp(inm, "roon") || (paced && !inm[0] && !air_play))) ? NETOUT_NP_ROON

                         : air_play ? NETOUT_NP_AIRPLAY : !NO_LOOP && !VOIE && !strcmp(inm, "spotify") ? NETOUT_NP_SPOTIFY
                         : !NO_LOOP && !strcmp(inm, "upnp") ? NETOUT_NP_UPNP : -1);   /* the UPnP input's DIDL */
    }

    static char dac_buf[160];
    char out_asked[sizeof c->out];
    snprintf(out_asked, sizeof out_asked, "%s", c->out);
    OUT = DIRECT_OUT ? OUT_DIRECT : OUT_ALSA;
    DAC = DAC_ARG;
    if (netout_is_net(c->out)) {
        if (NET_ON) {
            OUT = OUT_NET; netout_select(c->out);
            if (strcmp(net_spec, c->out)) { snprintf(net_spec, sizeof net_spec, "%s", c->out); NET_UP_RESET(); }
        }
        else { static int said = 0; if (!said++) logf_("out=%s: the network outputs are off (--net off): the command line's output", c->out); }
    } else if (!strncmp(c->out, OUT_CARD_PREFIX, OUT_CARD_PREFIX_LEN) && c->out[OUT_CARD_PREFIX_LEN]) { snprintf(dac_buf, sizeof dac_buf, "%s", c->out + OUT_CARD_PREFIX_LEN); DAC = dac_buf; OUT = OUT_ALSA; }
    st_out_why = ""; st_out_max = 0;
    int net_cut = 0;                                    /* the play ends with the music cut: the receiver stopped at once */
    int give_late = 0;                                  /* Roon paused to a network receiver: given back at the end */
    int up_ultra = 0;
    int obits = 32;                                     /* the bits the output keeps: TPDF dither to them (pcmclock.h) */
    double air_q0 = 0, air_blip_said = 0;               /* since when AirPlay's side of the Loopback is stopped */
    double air_live_t = 0;                              /* its sender last looked for (connected), and found */
    int air_live = 0;
    long air_blips = 0;
    int net_paused = 0, net_ended = 0;
    int in_changed = 0, in_swapped = 0;                 /* its input playing another stream in another shape;
                                                         * seen so, its stop never seen (said at the play's end) */
    unsigned orate = rate;                              /* the DAC at the stream's own rate (or DSD's words, below) */
    int L = 1;
    out_fmt = OUTF_S32;

    int lat_ring = !video && !air_play && (!drv_play || paced) && !sq_measuring() && (OUT != OUT_NET || !strncmp(out_asked, "audiolaz:", 9)) ? page_lat_ring : 0;
#ifndef __linux__

    if (drv_play && lat_ring > 0) {
        if (lat_ring > LAT_PACED_MS) lat_ring = LAT_PACED_MS;
        if (lat_ring < drv_ring_learned_s * 1000.0) lat_ring = (int)ceil(drv_ring_learned_s * 1000.0);
    }
#endif

    if (paced && lat_ring > LAT_PACED_MS) lat_ring = LAT_PACED_MS;
    lat_ring_now = lat_ring; lat_low_ms = -1; lat_min_low_ms = -1; lat_drops = 0;
    unsigned long period = (orate / 4 + 8) / 16;    /* as Roon: 689 frames at 44.1 kHz … 3000 at 192 kHz */
    out_pcm *out = NULL;
    unsigned long outbuf = 0, outper = 0;

    /* (18/10 evening, the box: the output switched to a receiver not found while Roon played; each try, once a second,
     * held the card again: Roon's stream stood still 10 s, RAATServer spun at SCHED_FIFO 50, a hard reboot) held at the
     * first try only: an output that does not open lets the card's own clock go on (the music unheard, the source's
     * clock never stopped), and the card itself never holds a stream already played (azcard.h AZC_IOC_HOLD) */
    static int open_fails = 0;                         /* the output's tries that did not open it, in a row */
    if (paced && !open_fails && !net_busy_tries && !dac_busy_tries) src_hold(cur_sub, OUT == OUT_NET ? 4000 : 1500);
#define OPEN_FAILED() do { open_fails++; if (paced) src_hold(cur_sub, 0); } while (0)   /* the card's clock at once */
    const double t_hold0 = now_s();                    /* the hold renewed while a UPnP renderer starts (below) */
    double t_hold_last = 0;                            /* renewed at once when the ring is full (the first read ended it) */
    if (OUT == OUT_NET) {                              /* a network receiver : a new stream at this rate, fetched */
        const unsigned m = netout_max_rate();
        if (m && orate > m && strncmp(net_spec, "upnp:", 5)) {                            /* the stream's own rate above it (08/10, review): said once, and
                                                        * in the status (out_why=rate), not as a receiver missing */
            static char said[200];
            char now_[200];
            snprintf(now_, sizeof now_, "%s %u", net_spec, orate);
            if (strcmp(said, now_)) { snprintf(said, sizeof said, "%s", now_); logf_("network output: %s takes up to %u Hz, this stream is %u Hz: not played", net_spec, m, orate); }
            st_out_why = "rate"; st_out_max = m;
            OPEN_FAILED();
            write_status("busy", rate, orate, c, 0, 0, 0, 0, 0, 0, 0, -120);
            UP_FREE(); sleep(1); return;
        }

        netout_smooth_position(!sq && !strncmp(c->out, "upnp:", 5) && (video || net_upnp_hold_ms > 0));
        netout_live(!sq || lat_ring > 0);
        netout_paced(paced);                           /* a card's app follows its clock: the ring pulled harder */
        netout_low_latency(video || (!sq && netout_is_airplay(c->out)));   /* AirPlay video , a Loopback source's
                                                       * music to an AirPlay device : it starts on little, the
                                                       * writer's hold_max */
        if (netout_open(orate)) {
            net_refused();
            OPEN_FAILED();
            if (netout_busy(NULL, NULL, 0)) st_out_why = "used";   /* another player plays to it (out_why=used) */
            else if (netout_dac_start(c->out)) st_out_why = "dac-start";   /* its DAC refuses to start: unplug, plug in */
            write_status("busy", rate, orate, c, 0, 0, 0, 0, 0, 0, 0, -120);
            UP_FREE(); sleep(1); return;
        }
        net_opened();
        outbuf = 0;

        obits = !strncmp(out_asked, "audiolaz:", 9) || netout_is_airplay(out_asked) ? 32 : 24;
        if (!strncmp(out_asked, "audiolaz:", 9) && !netout_link_rate() && netout_link_bits() >= 8 && netout_link_bits() < 32) obits = netout_link_bits();
    } else
    {
        out = dac_open(orate, period);
        if (!out) {                                        /* the DAC is busy (its own Roon zone plays): try again */
            dac_refused(orate);
            OPEN_FAILED();
            write_status("busy", rate, orate, c, 0, 0, 0, 0, 0, 0, 0, -120);   /* a play wanted, the DAC refusing */
            UP_FREE(); sleep(1); return;
        }
        if (dac_opened()) write_status("playing", rate, orate, c, 0, 0, 0, 0, 0, 0, 0, -120);   /* busy no more, at once */
        out_params(out, &outbuf, &outper);
        if (out_device_rate(out)) logf_("the DAC (%s) refuses %u Hz: it plays at %u Hz, resampled here", out_device_name(out), orate, out_device_rate(out));
        obits = out_bits(out);                             /* a 24-bit DAC: 24 */
        period = outper;
    }
    open_fails = 0;                                    /* the output open */

    long meas_pre = 0;
    const int meas_play = sq && measuring();           /* a measurement's play: its output peak said at its end */
    {
        static char last_out[sizeof c->out];
        static unsigned last_orate = 0;
        static int last_dsd = 0;
        int dsd_now = 0;
        if (sq && measuring() && last_orate && !strcmp(last_out, out_asked) && (last_dsd != dsd_now || last_orate != orate)) {
            meas_pre = (long)((uint64_t)rate * MEAS_RELOCK_MS / 1000);
            logf_("measurement: in PCM at %u Hz (the last play %s at %u Hz): %d ms of silence first, the DAC relocking", rate,
                  last_dsd ? "in DSD" : "upsampled or at another rate", last_orate, MEAS_RELOCK_MS);
        }
        snprintf(last_out, sizeof last_out, "%s", out_asked);
        last_orate = orate; last_dsd = dsd_now;
    }
#undef OPEN_FAILED

    unsigned long iper = L > 1 ? period / (unsigned long)L : period;
    if (iper < 64) iper = 64;
    src_pcm *in = NULL;                                /* the Loopback, or a pipe (src.h; none with the Squeezebox input) */

    int nch = 2;
    if (!sq) {
        in = open_capture(rate, iper, loop_channels(cur_sub));
        if (!in) { output_release(out, 0); UP_FREE(); return; }
        nch = (int)cap_ch;
        src_start(in);
    }

    st_in_ch = nch; snprintf(st_in_layout, sizeof st_in_layout, "%s", nch == 2 ? "stereo" : nch == 6 ? "5.1" : nch == 8 ? "7.1" : nch == 1 ? "mono" : "multi");
    st_dlg_centre = 0;
    if (nch != 2) logf_("input: %d channels: %s, as they came (a conversion needed for this output)", nch, nch == 1 ? "the one on both" : "its first two played");

    const int net_loop = OUT == OUT_NET && !sq;
    const double ms = 1000.0 / orate;

    double ctx_adj_ms = 0;
    int live_prep = 0;                                   /* what the input was set for: the live changes' base */
    /* the play's kind of stream (sdsc says it before styp: 1 Buffered, 0 Realtime, −1 not said): a
     * Buffered play's setting compared with what the Buffered offset was written for, when the input has that offset */
    const int play_buf = air_play ? air_styp_buffered() : -1;
    if (air_play && video) {
        air_ctx cx;
        air_ctx_now(&cx);
        int from;
        const int corr = corr_for(c, &cx, &from);
        const int prep = play_buf == 1 && air_prep_known_b ? air_prep_corr_b : air_prep_known ? air_prep_corr : c->out_lat;
        live_prep = prep;
        /* each kind its own settings, set ahead: said, nothing to do */
        if (air_play) logf_("AirPlay: a %s stream%s%s", play_buf == 1 ? "Buffered" : play_buf == 0 ? "Realtime" : "not yet said",
                            play_buf == 1 && air_has_boff ? ", its own offset " : play_buf == 1 ? ", the Realtime offset (shairport-sync without the Buffered offset's patch)" : "",
                            play_buf == 1 && air_has_boff ? "(buffered_latency_offset_in_seconds)" : play_buf == 0 && air_missing_ms > 0 && air_declares ? ", the rest of its latency declared to the sender" : "");
        const double d = (double)(corr - prep);
        if (fabs(d) >= AIR_CTX_MIN_MS) {
            const double room = air_video_ring_ms > AIR_VIDEO_RING_FLOOR_MS ? air_video_ring_ms - AIR_VIDEO_RING_FLOOR_MS : 0;
            ctx_adj_ms = d < 0 ? fmin(-d, AIR_CTX_LATER_MAX_MS) : -fmin(d, room);
            logf_("AirPlay: this session's setting by ear %+d ms (%s%s%s), the input set for %+d ms%s%s: the sound %s by %.0f ms this play (its ring)%s",
                  corr, from >= 0 ? "kept for \"" : from == -1 ? "the output's" : "none kept", from >= 0 ? cx.label[from] : "", from >= 0 ? "\"" : "",
                  prep, air_prep_key[0] ? " for " : "", air_prep_key, ctx_adj_ms > 0 ? "later" : "sooner", fabs(ctx_adj_ms),
                  d > 0 && -ctx_adj_ms < d ? ", the rest at the next session (the ring's room taken)" : "");
        }
    }
    st_ctx_adj_ms = ctx_adj_ms; st_live_left_ms = 0;
    const long ctx_adj = (long)llround(orate * ctx_adj_ms / 1000.0);
    uint64_t live_fin = 0;                               /* a fade-in due from there (0: none) */
    double t_live = 0;
    uint64_t cap_frames = 1;
    while (cap_frames < (net_loop ? 4 : 1) * (uint64_t)orate + (uint64_t)(ctx_adj > 0 ? 2 * ctx_adj : 0)
                        + (air_play && video ? (uint64_t)(orate * AIR_CTX_LATER_MAX_MS / 1000.0) : 0)) cap_frames <<= 1;   /* room for a press later */
    ring q = { calloc(2 * cap_frames, sizeof(int32_t)), cap_frames - 1, 0, 0 };
    const int ap_short = net_loop && !video && netout_is_airplay(c->out);   /* music to an AirPlay device */
    /* music to a UPnP renderer (11/10, NET_UPNP_MUSIC_HOLD_MS): held, its ring NET_UPNP_RING_MS */
    const int upnp_short = net_loop && !video && !strncmp(out_asked, "upnp:", 5) && net_upnp_hold_ms > 0;

    const int lead_kind = OUT == OUT_NET ? (!strncmp(out_asked, "upnp:", 5) ? RC_LEAD_UPNP : !strncmp(out_asked, "audiolaz:", 9) ? RC_LEAD_AUDIOLAZ
                                            : netout_is_airplay(out_asked) ? RC_LEAD_AIRPLAY : RC_LEAD_SQUEEZE) : OUT == OUT_DIRECT ? RC_LEAD_DIRECT : RC_LEAD_CARD;
    (void)lead_kind;
    const int az_out = OUT == OUT_NET && !strncmp(out_asked, "audiolaz:", 9);   /* an audiolaz receiver */
    const double drv_learn_s = paced ? drv_paced_learned(now_s()) : drv_ring_learned_s;   /* */
    long ring_target = ctx_adj + (long)(orate * (video ? air_video_ring_ms / 1000.0 : upnp_short ? NET_UPNP_RING_MS / 1000.0 :
                                             az_out && net_loop ? NET_AZ_RING_MS / 1000.0 : net_loop && !ap_short ? 3.0 : air_play ? air_music_ring_ms / 1000.0 :
                                             drv_play ? fmax(drv_ring_ms / 1000.0, drv_learn_s) : !sq ? RING_S : up_ultra ? RING_ULTRA_S : OUT == OUT_DIRECT ? RING_DIRECT_S : RING_S));   /* a receiver read in bursts
                                                       * (squeezelite, UPnP) keeps its 3 s whatever the source */

    if (lat_ring > 0) ring_target = ctx_adj + (long)(orate * lat_ring / 1000.0);
    /* a source at the DAC's pace: the ring it comes back down to after a hold-up (drv_paced_learned), the one
     * this play would have started from with none */
    long ring_base = ring_target;
    if (paced && lat_ring <= 0 && drv_learn_s > drv_ring_ms / 1000.0 && ring_target == ctx_adj + (long)(orate * drv_learn_s))
        ring_base = ctx_adj + (long)(orate * drv_ring_ms / 1000.0);
    if (ring_target > ring_base)
        logf_("« audiolaz » input: its ring at %.0f ms (%.0f ms without the player's last hold-up, %.0f s ago)", ring_target * ms, ring_base * ms, now_s() - drv_paced_hold_t);
    st_ring_ms = ring_target * ms;
    writer W = { .out = out, .rate = orate, .period = period, .outbuf = outbuf, .target = (long)outbuf - 2 * (long)period,
                 .prime = (uint64_t)ring_target, .q = &q, .net = OUT == OUT_NET, .sq = sq, .az = az_out, .L = L, .vid = net_loop && (video || ap_short || upnp_short),
                 .vgate = net_loop && (video || ap_short) && strncmp(out_asked, "upnp:", 5),
                 .hold_ms = video ? net_video_hold_ms : upnp_short ? net_upnp_hold_ms : net_airplay_hold_ms,
                 .hold_max_s = upnp_short ? NET_UPNP_HOLD_MAX_S : NET_VIDEO_HOLD_MAX_S,
                 .upnp = upnp_short || (net_loop && video && !strncmp(out_asked, "upnp:", 5)),   /* video too */
                 .step_s = upnp_short ? NET_UPNP_START_STEP_S : 1.5,
                 .what = video ? "AirPlay video" : upnp_short ? "a UPnP renderer's music" : "an AirPlay device", .t0 = t_enter };
    W.obits = obits;                                    /* the PCM's grid, for an inverted speaker (channels.h) */

    if (sq && OUT != OUT_NET) W.start = (uint64_t)(orate * RING_START_S);
    if (lat_ring > 0 && W.start > (uint64_t)ring_target) W.start = (uint64_t)ring_target;   /* all of it at once */
    if (az_out) netout_fifo_ms(lat_ring > 0 ? page_lat_link : 0);                          /* Link's queue */
    if (!q.buf || pthread_create(&W.th, NULL, writer_main, &W)) { free(q.buf); output_release(out, 0); if (in) src_close(in); UP_FREE(); return; }

    lat_rt_want = rt_priority <= 0 && lat_ring > 0 ? page_lat_rt : 0;
#ifdef __linux__

    if (!lat_rt_want && paced && rt_priority <= 0) lat_rt_want = PACED_RT_PRIO;
#endif
    lat_busy_t = now_s();
    if (lat_rt_want) lat_rt_set(lat_rt_want);
    if (lat_rt_want && !lat_ring) {
        static int paced_rt_said = -1;
        if (paced_rt_said != lat_rt_now) {
            paced_rt_said = lat_rt_now;
            if (lat_rt_now) logf_("input %d at the DAC's pace: read at SCHED_FIFO %d while it plays (its waits woken on time; a nap of %.1f ms every %.0f ms of work)",
                                  cur_sub, lat_rt_now, LAT_NAP_S * 1000, LAT_YIELD_S * 1000);
        }
    }
    if (lat_ring > 0) {
        char rts[80], lks[48] = "";
        if (lat_rt_now) snprintf(rts, sizeof rts, "at SCHED_FIFO %d (a nap of %.1f ms every %.0f ms of work)", lat_rt_now, LAT_NAP_S * 1000, LAT_YIELD_S * 1000);
        else snprintf(rts, sizeof rts, "at normal priority");
        if (az_out) snprintf(lks, sizeof lks, ", audiolaz Link's queue %s", page_lat_link > 0 ? "set" : "as before");
        logf_("latency: the ring at %d ms (%s), the convolution %s%s", lat_ring, sq ? "Roon's music read ahead" : "a source at its own pace", rts, lks);
    }

    filt *cur = NULL, *next = NULL, *pend = NULL;         /* the free player: no filter, ever (build) */
    long warm = 0;
    (void)pend;
    st_heard[0] = 0;
    int32_t *raw = malloc(sizeof(int32_t) * CAP_MAX_CH * iper);
    double *a = malloc(sizeof(double) * 2 * iper);    /* the levels' copy (levels.h: they read, nothing goes back) */
    const long la = 0;                                 /* no look-ahead: nothing ahead to hear */
    int32_t *pcm = calloc(2 * iper * (size_t)L, sizeof(int32_t));
    long clipped = 0, xruns_in = 0, dropped = 0;
    double t_rd = 0;                                   /* when the source at the DAC's pace was last read (PACED_KEEP_S) */
    long keep_reads = 0;                               /* reads made by it, this play (the journal) */
    long silences_logged = 0;                          /* silences already written to the journal (01/10: their time) */
    double dry_hold = 0;                               /* this loop's longest turn since the writer's last dry spell said */
    static double steady = 0;                          /* the clocks' steady gap (ppm), kept from one play to the next */
    double ppm = steady, t_adjust = now_s(), t_status = now_s(), t_ctl = now_s(), t_log = now_s(), t_in_log = 0, held = 0, t_fill = 0;
    /* The ring's fill, ten times a second: averaged for the clock (fill_) and for the journal (log_). */
    double fill_sum = 0, log_sum = 0;
    long fill_n = 0, log_n = 0, log_low = LONG_MAX, log_high = 0;
    /* The player's latency (08/10, above airplay_tick), sampled with the ring's fill: its sum over the 10 s going on. */
    double lat_sum = 0, t_lat = now_s(), lat_part[5] = { 0 };   /* parts: capture side, processing, ring, chunk, output */
    long lat_n = 0;
    int lat_said = 0;                                   /* its parts in the journal, once a play */
    const double t_play0 = now_s();
    double up_pause_t = 0;                              /* a built-in input paused since then (src_paused) */
    st_lat_ms = -1;
    double t_loop = now_s(), wait_max = 0;              /* the longest time between two turns of this loop, per minute */
    double drv_hold_t = 0, drv_hold = 0;
    double t_ring_down = 0;                             /* its ring last brought down a step (paced) */
    int ring_down_said = 0;
    {   /* this play's buffers all made: kept in memory (memlock.h), so that swap never holds the loop up */
        static size_t kept_said;
        int refused = 0;
        const double t0k = now_s();
        const size_t kept = getenv("AUDIOLAZ_NOMLOCK") ? 0 : mem_keep(&refused);   /* AUDIOLAZ_NOMLOCK=1: not (A/B) */
        if (kept != kept_said || refused) {
            const size_t lim = mem_keep_limit();
            logf_("memory: %.0f MB of the player kept resident in %.0f ms%s (lock limit %s)", kept / 1048576.0, (now_s() - t0k) * 1000,
                  refused ? ", some refused" : "", lim == SIZE_MAX ? "unlimited" : lim ? "set" : "none");
            kept_said = kept;
        }
    }
    if (!sq) shift_set(100000 + lround(ppm / 11.0));
    /* the Loopback's sources playing as this play starts: one that starts after it takes over (above) */
    int loop_was[LOOP_MAX_SUBS] = { 0 };
    if (!NO_LOOP) for (int k = 0; k < LOOP_SUBS; k++) loop_was[k] = slave_active_sub(k);
    /* The Squeezebox input: its generation (a stop or a flush ends the play), where the next track's first frame reaches
     * the DAC (STMs), the playing track's start there (its elapsed time), Roon's volume as applied. */
    const unsigned sq_gen = sq ? sq_generation() : 0;
    uint64_t sq_start_at = 0, sq_track0 = 0;
    int sq_due = 0, sq_track = 0, sq_done = 0;
    long loop_tail = -1;                               /* the same for a Loopback or card source that stopped */
    int in_rest = 0;                                   /* the source stopped: its last frames, less than a period, read */
    long sq_tail = -1;

    double sq_t_el = 0, sq_t_end = 0, sq_t_pause = 0, vol = 1.0;
    int vr_first = 1;                                  /* the play's first frames: the stage at its level at once */
    {   /* the option as the page says it before the first frame (a new player starts with it off here: never a
         * first block at full because the page was busy that moment); the free player's too */
        double vdb = 0;
        int vmu = 0, von = -1;
        for (int i = 0; i < 400 && (von = page_vol_for(c->out, c->noroom ? "" : c->place, &vdb, &vmu)) < 0; i++) usleep(250);
        if (von >= 0) { vol_on_now = von; if (von) { vol_db_now = vdb; vol_mute_now = vmu; } }
    }
    double dev_last = NAN;                             /* the input's device volume as last told (NAN: not yet this play) */
    (void)vol;                                         /* the source's own volume never applied: the music as it came */
    double sq_el0 = 0;                                 /* the elapsed time at sq_track0 (0 at a track's first frame) */
    int sq_given = 0;                                  /* what was read and not heard already given back (sq_unread) */
    /* why the play ends (NETOUT_WHY_*), said in the journal and to an audiolaz receiver; 0: from how it ended */
    int end_why = 0; char end_txt[200] = "";
#define PLAY_END(code, ...) (end_why = (code), snprintf(end_txt, sizeof end_txt, __VA_ARGS__))
    if (sq) logf_("input: Squeezebox, \"%s\"", SQ_NAME);
    if (OUT == OUT_NET) logf_("output: %s (network, 24 bits at %u Hz)", c->out, orate);

    if (OUT == OUT_ALSA && out && out_device_name(out)[0] && strcmp(out_device_name(out), "null")) { src_dac(out_device_name(out), out_device_maker(out), out_device_model(out), 1); dac_by_play = 1; }
    else { char dn[120]; dac_of(c, dn, sizeof dn); src_dac(dn, NULL, NULL, 0); dac_by_play = 0; }

    limiter *lim = NULL;

    if (sq) {
        const double pos = sq_position();
        if (pos >= 0) {
            sq_track = 1; sq_el0 = pos;
            sq_track0 = (uint64_t)(((cur ? cur->lat : 0) + (lim ? lim_latency(lim) : 0)) * L);   /* where that frame lands */
            sq_elapsed(pos);
            logf_("squeeze: playing on in the stream at %.2f s", pos);
        }
    }

    logf_("playing: %u Hz, %s%s%s%s%s, room %s", rate, c->bypass ? "bypass" : c->room,
          cur && cur->alt ? " + dynamic " : "", cur && cur->alt ? cur->name : "", cur && cur->xd ? " + image " : "", cur && cur->xd ? cur->image : "",
          c->noroom ? "none for this output" : c->place);
    if (obits < 32) logf_("output: %d bits kept by it, the music requantized to them here with TPDF dither (once; silence and untouched samples as they are)", obits);
    if (air_play) logf_("AirPlay: as it came, the sender's volume %s (the free player)", vol_on_now ? "to the player's own volume" : "ignored");
    if (air_play) logf_("AirPlay: %s (%s), the ring kept at %.0f ms%s", video ? "video" : "music", air_why, st_ring_ms,
                        video ? ", no upsampling, no DSD, until this play ends" : ", until this play ends");

    long trim = sq && (OUT != OUT_NET || az_out) && !sq_measuring() && atomic_load(&cal.state) == CAL_IDLE ? (long)(la + (cur ? cur->lat : 0) + (lim ? lim_latency(lim) : 0) + UP_LAT(0)) * L : 0, trimmed = 0;

    {
        const int dsd_on = 0;
        bp_start(L == 1 && !dsd_on);
        if (dsd_on) bp_say(BP_DSD); else if (L > 1) bp_say(BP_UPSAMPLING);
        if (nch != 2) bp_say(BP_CHANNELS);
        if (OUT == OUT_NET && netout_is_airplay(out_asked)) bp_say(BP_OUTPUT);   /* the bridge: 16 bits, 44.1 kHz */
        st_bp_rate = rate; st_bp_orate = orate; st_bp_obits = obits; st_bp_dev = 0;
        if (OUT == OUT_ALSA && out && out_device_rate(out)) { st_bp_dev = out_device_rate(out); bp_say(BP_RATE); }
        /* a Link receiver whose DAC does not take this rate: converted by netout.c, said (never bit for bit) */
        if (OUT == OUT_NET && netout_link_rate()) { st_bp_dev = netout_link_rate(); bp_say(BP_RATE); logf_("output: the Link receiver's DAC takes %u Hz, not %u Hz: converted here (the corrector's converter)", netout_link_rate(), orate); }
        /* and keeps fewer bits (its INFO bits=): the converted words requantized by the receiver, once; said */
        if (OUT == OUT_NET && netout_link_rate() && netout_link_bits() >= 8 && netout_link_bits() < 32) st_bp_obits = netout_link_bits();
        if (OUT == OUT_ALSA && out && out_device_name(out)[0]) snprintf(st_bp_dac, sizeof st_bp_dac, "%s", out_device_name(out));
        else dac_of(c, st_bp_dac, sizeof st_bp_dac);

        if (!trim) bp_base((int64_t)(la + (cur ? cur->lat : 0) + (lim ? lim_latency(lim) : 0)) * L);
    }
    struct pollfd fds[16];
    int more = 0;
    int net_anchor = 0;                                 /* a network receiver from a Loopback source: 1 full, 2 level kept */
    double t_anchor = 0;                                /* when its level was first kept (its settling: 60 s) */
    double ctl_target = (double)ring_target;            /* the ring's level the clock correction keeps */

    double vf_sum = 0, t_vf = t_play0, t_vf_said = 0, t_vf_off = -1e9;   /* t_vf_off: the ring last seen above it */
    long vf_n = 0;
    int vf_high = 0;
    rt_guard pg = { 0 };                                /* (21/10, rt_thread.h) this thread at real time: bounded work */
    double pspin_for = 0, pspin_at = rt_test_spin("play", &pspin_for);   /* the tests' (test/rt-safety.sh) */
    const double t_pspin0 = now_s();
    chan_stage chi;                                     /* the swap, on the music (channels.h): at once here */
    chan_init(&chi, chan_swap_now());
    while (!quit && !atomic_load(&W.dead)) {
        rt_xcpu_tick();                                 /* a thread of ours dropped by its SIGXCPU: said */
        if (pspin_at > 0 && now_s() - t_pspin0 >= pspin_at) { pspin_at = 0; logf_("test: the play's thread spins %.1f s without a wait (SCHED_FIFO %d now)", pspin_for, lat_rt_now); rt_test_spin_now(pspin_for); }
        /* the writer's news, said here (writer: say_first, say_fill) */
        if (atomic_load_explicit(&W.say_first, memory_order_acquire) == 1) {
            atomic_store(&W.say_first, 2);
            logf_("first sound: %.0f ms after the play's start (the ring %.0f ms), %.0f ms of the output's buffer before it", W.first_ms, W.first_ring_ms, W.first_out_ms);
        }
        if (atomic_load_explicit(&W.say_fill, memory_order_acquire) == 1) {
            atomic_store(&W.say_fill, 2);
            logf_("the ring at its level (%.0f ms) %.2f s after the music started on %.0f ms, %.0f ms at its lowest", W.fill_ms, W.fill_after_s, (double)W.start * ms, W.fill_low_ms);
        }
        {
            double tn = now_s();
            const double hold = tn - t_loop;
            if (hold > wait_max) wait_max = hold;
            if (hold > dry_hold) dry_hold = hold;      /* the longest turn since a dry spell was said */
            t_loop = tn;

            if (drv_play && hold > 0.15) { drv_hold_t = tn; if (hold > drv_hold) drv_hold = hold; }
            else if (drv_play && drv_hold_t > 0 && tn - drv_hold_t >= 2.0) {   /* the backlog read since: the level it left */
                const long cap = (long)orate, fill = (long)ring_fill(&q), want = (long)((drv_hold + 0.1) * orate);
                long nt = fill > want ? fill : want;
                if (nt > cap) nt = cap;
                if (paced) {                               /* for a while (drv_paced_learned) */
                    drv_ring_learned_s = fmax(drv_paced_learned(tn), (double)(nt > ring_target ? nt : ring_target) / orate);
                    drv_paced_hold_t = tn;                 /* the wait before coming back down starts again */
                    t_ring_down = tn;
                    ring_down_said = 0;
                }
                if (nt > ring_target) {
                    if (paced)
                        logf_("« audiolaz » input: the player held up %.0f ms: its ring at %.0f ms from now (was %.0f), back down to %.0f ms by %.0f ms every %.0f s once nothing has held it up for %.0f s",
                              drv_hold * 1000, nt * ms, ring_target * ms, ring_base * ms, DRV_DECAY_STEP_MS, DRV_DECAY_EVERY_S, DRV_DECAY_QUIET_S);
                    else
                        logf_("« audiolaz » input: the player held up %.0f ms: its ring kept at %.0f ms from now (was %.0f), the next plays too", drv_hold * 1000, nt * ms, ring_target * ms);
                    ring_target = nt; st_ring_ms = ring_target * ms;
                    if (!paced && (double)nt / orate > drv_ring_learned_s) drv_ring_learned_s = (double)nt / orate;   /* */
                    atomic_store(&W.prime, (uint64_t)ring_target);
                }
                ctl_target = fill < ring_target ? (double)ring_target : (double)(fill > cap ? cap : fill);
                drv_hold_t = 0; drv_hold = 0;
            }
            /* a source at the DAC's pace, nothing holding the player up for DRV_DECAY_QUIET_S: its ring back down
             * DRV_DECAY_STEP_MS every DRV_DECAY_EVERY_S (as drv_paced_learned), never below ring_base, never while a
             * hold-up is being weighed (drv_hold_t) */
            else if (paced && drv_hold_t == 0 && ring_target > ring_base && tn - drv_paced_hold_t >= DRV_DECAY_QUIET_S
                     && tn - t_ring_down >= DRV_DECAY_EVERY_S) {
                t_ring_down = tn;
                if (!ring_down_said) {
                    ring_down_said = 1;
                    logf_("« audiolaz » input: nothing held the player up for %.0f s: its ring coming back down from %.0f ms to %.0f, %.0f ms every %.0f s",
                          tn - drv_paced_hold_t, ring_target * ms, ring_base * ms, DRV_DECAY_STEP_MS, DRV_DECAY_EVERY_S);
                }
                const long step = (long)(orate * DRV_DECAY_STEP_MS / 1000.0);
                ring_target = ring_target - step > ring_base ? ring_target - step : ring_base;
                st_ring_ms = ring_target * ms; ctl_target = (double)ring_target;
                atomic_store(&W.prime, (uint64_t)ring_target);
                if (ring_target == ring_base) logf_("« audiolaz » input: its ring back at %.0f ms, %.0f s after the player's last hold-up", ring_target * ms, tn - drv_paced_hold_t);
            }
        }
        int nf = 0;
        nf += src_event_fds(fds, 4);
        if (in) nf += src_poll_fds(in, fds + nf, 16 - nf);

        const int lat_poll = lat_ring > 0 ? (lat_ring / 8 < 2 ? 2 : lat_ring / 8 > 10 ? 10 : lat_ring / 8) : 10;
        const double lat_p0 = lat_rt_want ? now_s() : 0;
        poll(fds, (nfds_t)nf, more ? 0 : sq ? lat_poll : 20);   /* more: the last turn stopped reading in the middle */
        if (lat_rt_want) lat_rt_guard(lat_p0, now_s());
        more = 0;
        src_drain();

        if (sq) {
            if (sq_generation() != sq_gen) { net_cut = 1; PLAY_END(NETOUT_WHY_USER, "Roon stopped or flushed its stream"); break; }   /* stopped or flushed: silent at once */
            const int paused = sq_state() == SQ_PAUSED;
            atomic_store(&W.pause, paused);

            const double let_go = OUT == OUT_NET && netout_is_airplay(net_spec) ? 0.0 : 1.5;
            if (OUT == OUT_NET && paused != net_paused && let_go > 0) { net_paused = paused; netout_pause(paused); }   /* the receiver too */
            if (!paused) sq_t_pause = 0;
            else if (!sq_t_pause) sq_t_pause = now_s();
            else if (now_s() - sq_t_pause > let_go) {      /* the DAC let go; what was read and not heard, given back */

                if (W.net) give_late = 1;
                else sq_unread((long)(ring_fill(&q) / (uint64_t)L) + (cur ? cur->lat : 0));
                sq_given = 1; net_cut = 1;
                PLAY_END(NETOUT_WHY_PAUSE, "Roon paused %.1f s: the output let go", let_go);
                break;
            }

            const unsigned nr = sq_next_rate();
            const int drained = sq_tail == 0 && ring_fill(&q) < (uint64_t)period;
            /* a network receiver : the next stream queued behind this one's tail (gapless on a Squeezebox player),
             * but all played only once it played what it holds */
            const int heard = drained && (OUT != OUT_NET || netout_held() < (long)(orate / 5));
            if (nr && nr != rate && drained) { PLAY_END(NETOUT_WHY_RATE, "the next track at %u Hz", nr); break; }   /* the next track at another rate */
            /* a network receiver : all sent, its end of file given at once (it knows the end: squeezelite's STMd,
             * the renderer's STOPPED); more music after it (Roon's next stream) is another stream, read in the next play */
            if (W.net && !net_ended && !nr && sq_ended() && drained) { netout_end(); net_ended = 1; }
            if (net_ended && (nr || !sq_ended())) { PLAY_END(NETOUT_WHY_END, "all sent, Roon's next stream after it"); break; }
            if (!nr && sq_ended() && heard) {                                  /* all played: STMu; the DAC kept 2 s */
                if (!sq_done) { sq_done = 1; sq_t_end = now_s(); sq_finished(); }
                else if (now_s() - sq_t_end > 2.0) { PLAY_END(NETOUT_WHY_END, "all played, Roon's stream over"); break; }
            } else sq_done = 0;
            /* where the music is heard: the DAC's writes, or what the network receiver played */
            const uint64_t rr = W.net ? net_heard(&q) : atomic_load(&q.r);
            if (sq_due && rr >= sq_start_at) { sq_due = 0; sq_track0 = sq_start_at; sq_el0 = 0; sq_track = 1; sq_started(); lim_track_reset(); }
            if (sq_track && now_s() - sq_t_el > 0.25) {
                sq_t_el = now_s();
                sq_elapsed(sq_el0 + (rr > sq_track0 ? (double)(rr - sq_track0) / orate : 0));
            }
        } else {
            /* macOS: the AirPlay receiver's stream in another layout within its session (stereo to 5.1, or back:
             * its pipe's 8 channels the same, src_pipe.c): started again with its count, as a Loopback reopened */
            if (in && src_relayout(in)) {
                logf_("input %d: its source's channels now %u (they were %d): the play starts again with them", cur_sub, src_channels(cur_sub), nch);
                break;
            }

            /* the UPnP input (src_upnp.c): its Stop, Seek or another track while playing cut at once (what the player
             * holds dropped); its Pause as Roon's (silence written, the music kept in the ring), a pause of more than 2 s
             * letting the output go, what was read and not heard given back to the input (played at the resume) */
            if (in && src_cut(cur_sub)) {
                logf_("input %d: stopped by its source (Stop, Seek, another track): the sound cut at once, %.0f ms held by the player dropped",
                      cur_sub, (double)unheard(&q, &W) * 1000.0 / orate);
                src_close(in); in = NULL; net_cut = 1;
                break;
            }
            if (in) {
                const int pz = src_paused(cur_sub);
                if (pz != atomic_load(&W.pause)) atomic_store(&W.pause, pz);
                if (!pz) up_pause_t = 0;
                else if (!up_pause_t) up_pause_t = now_s();
                else if (now_s() - up_pause_t > 2.0) {
                    src_unread(cur_sub, (long)((unheard(&q, &W) + (uint64_t)atomic_load(&W.hand)) / (uint64_t)L) + (cur ? cur->lat : 0)
                                        + (lim ? lim_latency(lim) : 0) + UP_LAT(0) + la);
                    logf_("input %d: paused 2 s: the output let go", cur_sub);
                    src_close(in); in = NULL; net_cut = 1;
                    break;
                }
            }

            int swapped = 0;
            if (in && paced) {
                const unsigned sr = slave_active() ? slave_rate() : 0;
                swapped = in_changed || (sr >= 8000 && (sr != rate || loop_channels(cur_sub) != nch));
            }
            const int gone = in && (swapped || !slave_active());   /* read once: both cases below on the same answer */
            if (swapped) in_swapped = 1;
            if (air_play) {
                if (!gone) air_q0 = 0;
                else {
                    const double tn = now_s();
                    const int fresh = !air_q0;                   /* a new moment (counted once) */
                    if (fresh) air_q0 = tn;
                    pthread_mutex_lock(&air_mu);
                    const double said = air_stop_said;
                    pthread_mutex_unlock(&air_mu);
                    const int told = said > 0 && said >= air_q0 - 1.0, meta = air_meta_open;

                    if (meta && !told && (fresh || tn - air_live_t >= 0.5)) { air_live_t = tn; air_live = src_air_connected(air_port_conf); }
                    const double wait_s = !meta ? AIR_CUT_QUIET_S : air_live ? AIR_CUT_LIVE_S : AIR_CUT_LATE_S;
                    if (told || tn - air_q0 >= wait_s) {
                        logf_("AirPlay: the source stopped (%s): the sound cut at once, %.0f ms held by the player dropped",
                              told ? "the sender's pause, stop or end, said by shairport-sync"
                                   : !meta ? "stopped 0.3 s" : air_live ? "stopped 10 s, its sender still connected, nothing said" : "stopped 1 s, nothing said, its sender gone",
                              (double)unheard(&q, &W) * 1000.0 / orate);
                        src_close(in); in = NULL;               /* let go of the Loopback now : its next stream may come
                                                                 * in another channel count (5.1 → stereo): both sides free */
                        net_cut = 1;
                        break;
                    }
                    if (fresh && (!air_blips++ || tn - air_blip_said > 10)) {
                        air_blip_said = tn;
                        logf_("AirPlay: the source's side of the Loopback stopped for a moment (an underrun: its packets late): the play goes on (%ld so far)", air_blips);
                    }
                }
            }
            /* Roon stopped: let go of the Loopback now; the writer keeps the DAC fed with silence for up to 2 s */

#define IN_LET_GO() do { in_rest = 0; src_close(in); in = NULL; held = now_s(); atomic_store(&W.ending, 1); \
                         loop_tail = (cur ? cur->lat : 0) + (lim ? lim_latency(lim) : 0) + UP_LAT(1) + (long)(rate / 20) + (long)iper + 1; } while (0)
            if (gone && !air_play && in && !in_rest) { if (src_avail(in) > 0) in_rest = 1; else IN_LET_GO(); }
            if (!in) {
                if (slave_active()) {
                    loop_tail = -1;
                    const int unseen = in_swapped;
                    in_changed = 0; in_swapped = 0;
                    const unsigned sr = slave_rate();
                    if (sr != rate || loop_channels(cur_sub) != nch) {   /* another rate (or channel count, 10/10): start again */

                        if (paced) { src_hold(cur_sub, OUT == OUT_NET ? 4000 : 1500); play_again_now = 1; }
                        const char *how = unseen ? " (begun before its stop was seen)" : "";
                        if (loop_channels(cur_sub) != nch) PLAY_END(NETOUT_WHY_RATE, "the source's next stream at %u Hz, %d channels%s", sr, loop_channels(cur_sub), how);
                        else PLAY_END(NETOUT_WHY_RATE, "the source's next stream at %u Hz%s", sr, how);
                        break;
                    }
                    in = open_capture(rate, iper, nch); in_rest = 0;
                    if (in && (int)cap_ch != nch) { src_close(in); in = NULL; }   /* not that count after all: start again */
                    if (!in) break;
                    src_start(in);
                    atomic_store(&W.ending, 0);
                    continue;
                }

                const double since = now_s() - held;

                static double t_dry = 0;
                if (ring_fill(&q) != 0) t_dry = 0; else if (!t_dry) t_dry = now_s();
                const double dac_s = OUT == OUT_ALSA && orate ? (double)outbuf / orate + 0.02 : 0;
                if (drv_play && since > DRV_END_S && ring_fill(&q) == 0 && now_s() - t_dry >= dac_s && src_io_ended(cur_sub)) {
                    logf_("input %d: its app ended its IO: the DAC let go %.0f ms after the music stopped", cur_sub, since * 1000);
                    break;
                }
                /* (18/10, test/engine-multi.sh: a UPnP renderer's last second never sent) to a network receiver, held a
                 * second or more ahead of what it played: the ring sent first (15 s at most) */
                if (since > 2.0 && (OUT != OUT_NET || ring_fill(&q) < (uint64_t)period || since > 15.0)) break;   /* nothing: release the DAC */
                if (loop_tail <= 0) continue;                    /* else its tail first, below */
            }

        }

        const double t_read = now_s();
        for (;;) {                                         /* every whole period Roon has written; with the Squeezebox
                                                            * input, as long as the ring wants music (the DAC's pace) */
            if (lat_rt_now) rt_guard_step(&pg, LAT_YIELD_S, LAT_NAP_S);   /* a long read at real time: its naps */
            long n;
            int sq_first = 0;
            if (sq) {
                if ((long)ring_fill(&q) >= ring_target || atomic_load(&W.pause) || net_ended) break;
                /* 20 ms of reading at most per turn : a network receiver takes its whole buffer at once (seconds
                 * of music), and this loop, filling the ring as fast as it empties, kept the turn's checks (Roon's
                 * pause, STMs, the elapsed time) waiting for all of it: 10 s late on a slow machine. The next turn
                 * starts at once. */
                if (now_s() - t_read > 0.02) { more = 1; break; }
                if (meas_pre > 0) {                        /* a measurement's pre-roll: the DAC relocking */
                    n = meas_pre < (long)iper ? meas_pre : (long)iper;
                    memset(raw, 0, sizeof(int32_t) * 2 * (size_t)n);
                    meas_pre -= n;
                } else
                n = sq_read(raw, (long)iper, rate, &sq_first);
                if (n > 0) { if (sq_tail >= 0) atomic_store(&W.ending, 0); sq_tail = -1; }
                else {                                     /* nothing to read: the music late (wait), or its end */
                    const unsigned nr2 = sq_next_rate();
                    if (!((nr2 && nr2 != rate) || (!nr2 && sq_ended()))) break;
                    if (sq_tail < 0) { sq_tail = (cur ? cur->lat : 0) + (lim ? lim_latency(lim) : 0) + UP_LAT(1) + (long)(rate / 20) + (long)iper + 1; atomic_store(&W.ending, 1); }
                    if (sq_tail == 0) break;
                    n = sq_tail < (long)iper ? sq_tail : (long)iper;
                    memset(raw, 0, sizeof(int32_t) * 2 * (size_t)n);
                    sq_tail -= n;
                }
            } else if (!in) {
                if (loop_tail <= 0 || (long)ring_fill(&q) >= ring_target) break;
                n = loop_tail < (long)iper ? loop_tail : (long)iper;
                memset(raw, 0, sizeof(int32_t) * (size_t)nch * (size_t)n);
                loop_tail -= n;
            } else {

                /* a source at the DAC's pace to a UPnP renderer waits for it (src_hold, below): nothing read ahead
                 * that its start would drop (the writer's pre_drop) */
                const long vid_more = W.vid && !atomic_load(&W.full) && !(paced && W.upnp) ? atomic_load(&W.hold) + (long)period : 0;
                long av = src_avail(in);
                long keep = 0;

                if (paced && (long)ring_fill(&q) >= ring_target + vid_more && av >= 0) {
                    const double tk = now_s();
                    /* (18/10, test/engine-multi.sh: a renderer's first 1.7 s of music never heard, read and dropped while
                     * it started: the writer's pre_drop, made for a source that cannot wait) a source at the DAC's pace
                     * waits for the renderer instead: the card holds it (src_hold, renewed) until it plays, PACED_HOLD_MAX_S
                     * at most; then as before */
                    if (OUT == OUT_NET && (!atomic_load(&W.full) || (W.upnp && !netout_started())) && tk - t_hold0 < PACED_HOLD_MAX_S) {   /* any receiver filling */
                        if (tk - t_hold_last >= 0.1) { src_hold(cur_sub, 1000); t_hold_last = tk; }   /* well within AZC_STALE_MS */
                        break;
                    }
                    /* an uplink's ring keeps its music (src_keeps): never read ahead at its rate, its pace the output's */
                    if (OUT != OUT_NET || av <= 0 || t_rd <= 0 || tk - t_rd < PACED_KEEP_S || src_keeps(cur_sub)) break;
                    keep = (long)((tk - t_rd) * rate);
                    if (keep > av) keep = av;
                    if (keep > (long)iper) keep = (long)iper;
                    const long up = (long)((keep + 1) * ((double)orate / rate) + 1);
                    if (keep < 1 || (long)ring_fill(&q) + up > ring_target + vid_more + (long)(orate * PACED_KEEP_MAX_S)
                        || (long)ring_fill(&q) + up + 2 * (long)period > (long)q.mask) break;
                    keep_reads++;
                }

                const int part = paced || in_rest;          /* a stopped source's last frames: a part of a period */
                if (av >= 0 && av < (part ? 1L : (long)iper)) break;
                n = av < 0 ? av : src_read(in, raw, keep > 0 ? (unsigned long)keep : part && av < (long)iper ? (unsigned long)av : iper);
                if (n == SRC_AGAIN) break;
                if (n > 0) t_rd = now_s();

                if (n == SRC_CHANGED) { in_changed = 1; more = 1; break; }
                if (n == SRC_ENDED) { src_recover(in, n); break; }
                if (n < 0) {                               /* the Loopback's buffer overflowed: read too late */

                    static double t_lost = -1e9;
                    const double tl = now_s();
                    if (!paced || n != SRC_LOST || tl - t_lost > 0.25) xruns_in++;
                    if (paced && n == SRC_LOST) t_lost = tl;
                    if (now_s() - t_in_log > 5) {
                        t_in_log = now_s();
                        if (paced)                         /* the card's own clock let it go, or the driver's ring overflowed */
                            logf_("input %d at the DAC's pace: music lost at %u Hz (%s): the player read it too late; the ring holds %.0f ms",
                                  cur_sub, rate, src_strerror(n), (double)ring_fill(&q) * ms);
                        else
                            logf_("Loopback overrun at %u Hz (%s): the convolution read it too late; the ring holds %.0f ms", rate, src_strerror(n), (double)ring_fill(&q) * ms);
                    }
                    src_recover(in, n);
                    break;
                }

                int32_t ip = 0;
                for (long i = 0; i < nch * (long)n; i++) { const int32_t v = raw[i] == INT32_MIN ? INT32_MAX : abs(raw[i]); if (v > ip) ip = v; }
                const double ipd = ip / 2147483648.0;
                if (ipd > page_in_peak) page_in_peak = ipd;
                if (ipd > play_in_peak) play_in_peak = ipd;
            }
            /* the swap (channels.h), the music's, decided once a block for both builds: never what POST /measure
             * or /calibrate plays (chan_swap_now). The source fingerprinted as the output carries it (bitperfect.h). */
            const int cin = chan_swap_now();
            bp_in_as(raw, n, nch, chan_moving(&chi, cin) ? BP_IN_SKIP : cin ? BP_IN_SWAP : BP_IN_AS);   /* the source, as the output carries it */
            if (cin && nch > 1 && !(atomic_load_explicit(&bp.why, memory_order_relaxed) & BP_SWAP)) bp_say(BP_SWAP);
            /* The free player: the source's frames as they came, bit for bit; only what the output needs (its
             * two channels: a source of more, its first two, a mono one on both; its bits: a word longer than the DAC
             * takes, requantized once with TPDF dither, a sample already on its grid untouched, pcmclock.h). */
            long m = n;
            if (nch == 2) memcpy(pcm, raw, sizeof(int32_t) * 2 * (size_t)n);
            else for (long i = 0; i < n; i++) { pcm[2 * i] = raw[(size_t)i * (size_t)nch]; pcm[2 * i + 1] = raw[(size_t)i * (size_t)nch + (nch > 1)]; }
            chan_run(&chi, cin, pcm, (unsigned long)n, CH_PCM, 32, rate);   /* the swap, as Room's: the words moved */
            {

                const double vt = sq ? sq_volume() : src_volume(cur_sub);
                double vdb = 0;
                int vmu = 0, vdone = 0;
                const int von = page_vol_for(c->out, c->noroom ? "" : c->place, &vdb, &vmu);
                if (von >= 0) { vol_on_now = von; if (von) { vol_db_now = vdb; vol_mute_now = vmu; } }
                if (vol_on_now && !sq && cur_sub != UPNP_K && (dev_last != dev_last || fabs(vt - dev_last) > 1e-6)) {
                    const double r = page_vol_src(VS_DEVICE, vt > 0 ? 20.0 * log10(vt) : NAN, vt <= 0, dev_last != dev_last, 1);
                    if (r != -INFINITY) dev_last = vt;            /* the page busy: told at the next period */
                }
                if (vol_on_now && !vr.active) vol_ramp_take(&vr, vr_first ? NAN : 1.0, vol_db_now, vol_mute_now);
                if (vr.active) {
                    if (vol_on_now) vol_ramp_to(&vr, vol_db_now, vol_mute_now);
                    else vol_ramp_to(&vr, 0.0, 0);                /* off: back to full, the music as it came */
                    if (vr_first && vol_on_now) { vr.db = vr.tdb; vr.mg = vr.tmute ? 0.0 : 1.0; }   /* nothing played before it: at the level at once */
                    vdone = vol_ramp_run_s32(&vr, pcm, a, m, (double)rate * L, obits, &dither_rng);
                    if (vdone) bp_say(sq && measuring() ? BP_AMP_VOLUME : BP_VOLUME);   /* a measurement's: the power amplifier's, as Room's */
                    if (!vol_on_now && vol_ramp_settled(&vr)) vr.active = 0;   /* handed back, at 0 dB */
                }
                vr_first = 0;
                atomic_store_explicit(&vol_cdb_st, vr.active ? lround(vr.db * 100.0) : 0, memory_order_relaxed);
                atomic_store_explicit(&vol_act_st, vr.active, memory_order_relaxed);
                if (!vdone && obits < 32) for (long i = 0; i < 2 * m; i++) pcm[i] = s32_out_q(s32_in(pcm[i]), obits, &dither_rng);
            }
            for (long i = 0; i < 2 * m; i++) { const double x = fabs(s32_in(pcm[i])); if (x > play_out_peak) play_out_peak = x; }
            if (levels_wanted(&lv_tap)) {
                for (long i = 0; i < 2 * m; i++) a[i] = s32_in(pcm[i]);
                levels_feed(&lv_tap, a, n, rate, (long long)mono_ns());
            }
            if (sq_first) { sq_start_at = atomic_load(&q.w) + (uint64_t)(((cur ? cur->lat : 0) + (lim ? lim_latency(lim) : 0)) * L); sq_due = 1; }   /* the track's first frame, there */

            if (sq_first && atomic_load(&cal.state) == CAL_ARMED && sq_measuring() == 1 && sq_measure_corrected() == 2) {
                double pr = (double)la + (cur ? cur->lat : 0) + (lim ? lim_latency(lim) : 0) + UP_LAT(0);
                const uint64_t base = atomic_load(&q.w) + (uint64_t)llround(pr * L);
                pthread_mutex_lock(&cal_mu);
                for (int i = 0; i < cal.n; i++) cal.idx[i] = base + (uint64_t)llround(cal.at_ms[i] * rate / 1000.0) * (uint64_t)L;
                cal.rate = rate; cal.orate = orate;
                snprintf(cal.out, sizeof cal.out, "%s", out_asked);
                atomic_store_explicit(&cal.state, CAL_MARKED, memory_order_release);
                pthread_mutex_unlock(&cal_mu);
                logf_("calibration: the track started (%u Hz, to %s), %d clicks marked, %.1f ms of processing before them", rate,
                      out_asked[0] ? out_asked : "the default output", cal.n, 1000.0 * pr / rate);
            }
            if (trim > 0) {
                long k = 0;

                const int32_t tf = obits < 32 ? ((int32_t)1 << (32 - (obits < 8 ? 8 : obits))) + 1 : TRIM_FLOOR;
                while (k < m && k < trim && pcm[2 * k] > -tf && pcm[2 * k] < tf && pcm[2 * k + 1] > -tf && pcm[2 * k + 1] < tf) k++;
                trim = k < m && k < trim ? 0 : trim - k;   /* a step of music: nothing more dropped, ever, this play */
                if (k) {
                    const uint64_t w0 = atomic_load(&q.w);  /* what was to land after them lands k frames sooner */
                    if (sq_due) sq_start_at = sq_start_at > w0 + (uint64_t)k ? sq_start_at - (uint64_t)k : w0;
                    else if (sq_track && sq_track0 > w0) sq_track0 = sq_track0 > w0 + (uint64_t)k ? sq_track0 - (uint64_t)k : w0;
                    memmove(pcm, pcm + 2 * k, sizeof(int32_t) * 2 * (size_t)(m - k));
                    m -= k; trimmed += k;
                }
                if (!trim) logf_("the filters' delay at the start: %.0f ms of silence dropped", (double)trimmed * ms);
                if (!trim) bp_base((int64_t)(la + (cur ? cur->lat : 0) + (lim ? lim_latency(lim) : 0)) * L - (int64_t)trimmed);   /* */
                if (!m) continue;
            }
            if (ring_fill(&q) + (uint64_t)m > cap_frames) { dropped += m; bp_say(BP_DROPPED); bp_stop(); continue; }   /* the ring full: the DAC is stuck */
            ring_push(&q, pcm, (uint64_t)m);
            if (live_fin && atomic_load(&q.w) >= live_fin + AIR_LIVE_FADE) { ring_fade(&q, live_fin, AIR_LIVE_FADE, 1); live_fin = 0; }   /* */
        }
        if (in_rest) { if (in) IN_LET_GO(); else in_rest = 0; }   /* a stopped source's last frames read: let go now */

        double t = now_s();
        if (t - t_fill >= 0.1) {                           /* the ring's fill, exact: no question to any driver */
            t_fill = t;
            long f = (long)ring_fill(&q);
            /* AirPlay video to a network receiver : the ring as it is once the receiver holds all it may (it takes
             * that from the ring whenever it reads: a burst), so that the ring's level is there after the burst too */
            long eff = f;                                  /* the level as the clock correction sees it */
            if (W.vid && net_anchor) { const long room_left = atomic_load(&W.hold) - netout_held(); eff = f - (room_left > 0 ? room_left : 0); }
            fill_sum += (double)eff;
            fill_n++;
            log_sum += (double)f; log_n++;
            if (f < log_low) log_low = f;

            if (lat_ring > 0 && atomic_load(&W.say_first) && t - t_play0 > 3.0 && (lat_low_ms < 0 || f * ms < lat_low_ms)) lat_low_ms = f * ms;
            if (f > log_high) log_high = f;
            /* A backlog left by the card being opened again (the ring kept the music meanwhile): beyond 400 ms more
             * than the target, drop it rather than play half a second late for a quarter of an hour. * Its level as the clock correction sees it (a network receiver's room is music it will take, not late). */
            /* not to a network receiver's music: dropped there, what it played would be counted wrong
             * (unheard: the ring's read moved, not what it was sent), and it never was (the skip lost, writer above) */
            /* a UPnP renderer: not before its level is kept (its hold settled: what came while it started is
             * dropped as its music starts, the writer's) */
            if (eff > ring_target + (long)(orate * 0.4) && (!W.net || video) && !(W.upnp && !net_anchor)) { atomic_store(&W.skip, eff - ring_target); t_vf_off = t; }
            /* AirPlay video: held at its target (above, vf_*), once the output plays (a network
             * receiver: its level anchored) */
            if (air_play && video && (!W.net || net_anchor) && t - t_play0 >= 2.0) {
                vf_sum += (double)eff; vf_n++;
                if (t - t_vf >= 1.0) {
                    const double avg = vf_n ? vf_sum / vf_n : 0, tol = orate * AIR_VIDEO_RING_TOL_MS / 1000.0;
                    vf_high = avg > ctl_target + tol ? vf_high + 1 : 0;
                    if (vf_high) t_vf_off = t;
                    if (vf_high >= 2) {
                        const long ex = (long)(avg - ctl_target);
                        atomic_store(&W.skip, ex);
                        vf_high = 0;
                        fill_sum = 0; fill_n = 0; t_adjust = t;   /* the clock correction's average starts again */
                        if (t - t_vf_said >= 5.0) {
                            t_vf_said = t;
                            logf_("AirPlay video: the ring %.0f ms on average, its target %.0f ms: %.0f ms dropped at once, the picture's sync kept",
                                  avg * ms, ctl_target * ms, ex * ms);
                        }
                    }
                    vf_sum = 0; vf_n = 0; t_vf = t;
                }
            }

            const int settled = t - t_play0 >= 15.0 && (sq || in) && !atomic_load(&W.pause) && (!W.net || sq || net_anchor == 2);

            double oq;
            if (W.net) oq = (double)netout_held();
            else if (OUT == OUT_DIRECT) oq = (double)atomic_load(&W.fill);
            else {                                          /* the card's delay when last written, less what played since */
                const int64_t ns = atomic_load(&W.delay_ns);
                struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                const int64_t now_ns = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
                oq = ns ? (double)atomic_load(&W.delay) - (double)(now_ns - ns) / 1e9 * orate : (double)atomic_load(&W.fill);
                if (oq < 0) oq = 0;
            }
            const filt *pf = next && warm <= 0 ? next : cur;
            double proc_late = (pf ? pf->lat : 0) + (lim ? lim_latency(lim) : 0) + UP_LAT(0);
            if (settled) {
                double cap = 0;
                long d;
                /* negative from an input whose source writes early by contract (src.h: macOS's AirPlay) */
                if (in && src_delay(in, &d) == 0 && d > -(long)rate) cap = (double)d;
                const double proc = (double)la + proc_late;
                const double hand = (double)atomic_load(&W.hand);
                lat_sum += (cap + proc) / rate + ((double)f + hand + oq) / orate;
                lat_part[0] += cap / rate; lat_part[1] += proc / rate; lat_part[2] += (double)f / orate;
                lat_part[3] += hand / orate; lat_part[4] += oq / orate;
                lat_n++;
            }
            if (!settled) {                             /* the 10 s windows start once settled */
                t_lat = t; lat_sum = 0; lat_n = 0;
                for (int i = 0; i < 5; i++) lat_part[i] = 0;
            } else if (t - t_lat >= 10.0) {
                if (lat_n >= 50) {
                    st_lat_ms = 1000.0 * lat_sum / lat_n;
                    if (drv_play) src_latency(cur_sub, 1000.0 * (lat_sum - lat_part[0]) / lat_n);   /* all but the device's own side */
                    if (!lat_said++)
                        logf_("latency, input to output: %.0f ms (the Loopback's capture side %.0f, look-ahead, filter, limiter and upsampler %.0f, ring %.0f, chunk in hand %.0f, the output's buffer %.0f)",
                              st_lat_ms, 1000 * lat_part[0] / lat_n, 1000 * lat_part[1] / lat_n, 1000 * lat_part[2] / lat_n, 1000 * lat_part[3] / lat_n, 1000 * lat_part[4] / lat_n);

                    int same = orate == rate;
                    const int plain = !W.net && same;

                    const double ring_ms = 1000 * lat_part[2] / lat_n;
                    const int ring_ok = t - t_vf_off > 12.0;
                    lat_keep(out_asked, rate, video && ring_ok ? st_lat_ms - ctx_adj_ms : 0, plain && !video ? st_lat_ms - ring_ms : 0,
                             same ? 1000 * (lat_part[0] + lat_part[1] + lat_part[3]) / lat_n : 0);
                }
                lat_sum = 0; lat_n = 0; t_lat = t;
                for (int i = 0; i < 5; i++) lat_part[i] = 0;
            }
            if (levels_wanted(&lv_tap)) {                  /* the levels' frames: from the ring's entry to the ear */
                double oq;                                 /* the output's buffer, as the latency above counts it */
                if (W.net) oq = (double)netout_held();
                else if (OUT == OUT_DIRECT) oq = (double)atomic_load(&W.fill);
                else {
                    const int64_t ns = atomic_load(&W.delay_ns);
                    oq = ns ? (double)atomic_load(&W.delay) - (double)(mono_ns() - ns) / 1e9 * orate : (double)atomic_load(&W.fill);
                    if (oq < 0) oq = 0;
                }

                const double rest_ms = !isnan(c->cal_ms) ? c->cal_ms : OUT == OUT_DIRECT ? airplay_cal_ms : 0;
                levels_set_delay(&lv_tap, ((double)f + (double)atomic_load(&W.hand) + oq) / orate + (double)UP_LAT(0) / rate + rest_ms / 1000);
            }
        }
        if (t - t_ctl > 0.05) {                            /* the control file, 20 times a second (03/10: 4 before) */
            t_ctl = t;
            if (OUT == OUT_NET) netout_np_tick();
            /* Another source starting (08/10, --loop-subs): the one that started last plays. Played from the Loopback:
             * Roon's Squeezebox stream starting, or a newer subdevice, ends this play (the main loop takes it up). Played
             * from the Squeezebox input: a subdevice starting pauses Roon there (Roon shows it paused), and this play ends
             * as a pause does. */
            if (!NO_LOOP && LOOP_SUBS > 1) {
                if (!sq) {
                    if (SQ_SERVER && sq_state() == SQ_PLAYING) { logf_("input: Roon (Squeezebox) started: it takes over"); input_stop(cur_sub); net_cut = 1; break; }
                    const int k = newest_sub();
                    if (k >= 0 && k != cur_sub) { logf_("input: Loopback subdevice %d started: it takes over", k); input_stop(cur_sub); net_cut = 1; break; }
                } else {
                    for (int k = 0; k < LOOP_SUBS; k++) {
                        const int a = slave_active_sub(k);
                        if (a && !loop_was[k]) { logf_("input: Loopback subdevice %d started: Roon paused", k); sq_pause_other(); }
                        loop_was[k] = a;
                    }
                }
            }

            if (air_play && video && t - t_live >= 0.25) {
                t_live = t;
                air_ctx lx;
                air_ctx_now(&lx);
                int lfrom;
                const double d = (double)(corr_for(c, &lx, &lfrom) - live_prep);
                const double base_ms = (double)ring_target * ms - ctx_adj_ms;      /* the ring without any adjustment */
                const double room = base_ms > AIR_LIVE_FLOOR_MS ? base_ms - AIR_LIVE_FLOOR_MS : 0;
                const double want = d < 0 ? fmin(-d, AIR_CTX_LATER_MAX_MS) : -fmin(d, room);
                st_live_left_ms = d > room ? d - room : 0;
                static double left_done = 0;                   /* the rest at once: the input rewritten, restarted */
                if (st_live_left_ms <= 0) left_done = 0;
                else if (fabs(st_live_left_ms - left_done) >= 5.0) { left_done = st_live_left_ms; air_rest_pending = 1; }
                if (fabs(want - ctx_adj_ms) >= 5.0) {
                    const long got = ring_live(&q, (long)llround(orate * (want - ctx_adj_ms) / 1000.0), cap_frames, period, &live_fin);
                    if (got) {
                        const double was = ctx_adj_ms;
                        ctx_adj_ms += (double)got * ms;
                        ring_target += got; ctl_target += (double)got;
                        if (ctl_target < ring_target / 4.0) ctl_target = ring_target / 4.0;
                        st_ctx_adj_ms = ctx_adj_ms; st_ring_ms = ring_target * ms;
                        fill_sum = 0; fill_n = 0; t_adjust = t; vf_sum = 0; vf_n = 0; vf_high = 0; t_vf_off = t;
                        logf_("AirPlay: « Synchro image » %+.0f ms during the session: the sound %s by %.0f ms at once (its ring %.0f ms)%s",
                              d, got > 0 ? "later" : "sooner", fabs(ctx_adj_ms - was), ring_target * ms,
                              st_live_left_ms > 0 ? ", the rest at the sender's next pause" : "");
                    }
                }
            }
            if (read_control(c, seen)) {
                int again = 0;                             /* the DAC opened again: another rate, or PCM ↔ DSD */
                if (strcmp(c->out, out_asked)) again = 1;  /* another output */
                /* AirPlay's mode chosen on the page : the play opened again when it changes this one's; the
                 * sender's own changes never do (the header) */
                if (air_play && c->air_mode != air_asked) {
                    char w2[160];
                    if (air_video_for(c->air_mode, w2, sizeof w2) != video) { again = 1; logf_("AirPlay: %s chosen on the page: the play opened again", video ? "music" : "video"); }
                }
                if (again) {

                    if (strcmp(c->out, out_asked)) PLAY_END(NETOUT_WHY_OUTPUT, "the output changed (%.80s → %.80s)", out_asked, c->out);
                    else PLAY_END(NETOUT_WHY_SETTINGS, "the settings changed (DSD, or AirPlay's mode)");
                    /* the Squeezebox's music not heard yet given back : it plays on from where it was heard */
                    if (sq) sq_unread((long)(unheard(&q, &W) / (uint64_t)L) + (cur ? cur->lat : 0)
                                      + (lim ? lim_latency(lim) : 0) + UP_LAT(0));
                    sq_given = 1; net_cut = 1;
                    break;
                }
                request(c, rate);
            }
        }

        if (!sq && W.net && !net_anchor) {
            if (atomic_load(&W.full)) net_anchor = 1;   /* the music started (full, or never pushing back) */
            fill_sum = 0; fill_n = 0; t_adjust = t;
        }
        /* an uplink's ring to a network output: its play settled once the receiver took its music (nothing to
         * steer, no level to keep), so that its latency is measured (src_latency: the Mac's device tells its apps) */
        if (paced && net_anchor == 1 && src_keeps(cur_sub)) net_anchor = 2;
        if (paced) { fill_sum = 0; fill_n = 0; t_adjust = t; ppm = 0; }   /* nothing to steer */
        if (!sq && fill_n && t - t_adjust >= ADJUST_S) {
            /* Clocks, as CamillaDSP does it: the ring's fill averaged over 10 s, and a PI regulator on its distance to
             * the target, in seconds of audio: 10 ms too much → Roon 200 ppm slower at once (CLOCK_KP); the integral learns the
             * steady gap between the two clocks. ppm > 0 slows Roon (the control above 100000). */
            if (net_anchor == 1) {
                const double avg = fill_sum / fill_n;
                net_anchor = 2;

                if (W.vid && !W.upnp)                      /* AirPlay video : it holds no more than hold_max, never
                                                            * more out of the ring later: the target itself, its level kept
                                                            * through its bursts (above) */
                    logf_("network receiver: the ring kept at its target, %.0f ms once the receiver holds its %.0f ms (now %.0f ms on average)",
                          ring_target * ms, (double)atomic_load(&W.hold) * ms, avg * ms);
                else {

                    double hi = az_out ? (double)ring_target : (double)cap_frames - 1.5 * orate;   /* an audiolaz receiver: as before */
                    if (hi < ring_target) hi = ring_target;
                    ctl_target = avg < ring_target / 4.0 ? ring_target / 4.0 : avg > hi ? hi : avg;
                    t_anchor = t;
                    atomic_store(&W.prime, (uint64_t)ctl_target);   /* a re-prime starts at the level kept */
                    logf_("network receiver: the ring kept at %.0f ms (its target %.0f ms%s)", ctl_target * ms, ring_target * ms,
                          ctl_target > ring_target ? ": the receiver reads in bursts, the level it leaves kept" : "");
                }
            }
            double e = (fill_sum / fill_n - ctl_target) / (double)orate;

            if (W.net && !az_out && !W.vid && net_anchor == 2 && t_anchor > 0 && t - t_anchor < 60.0 && fabs(e) > CLOCK_NET_DEAD_S) {
                const double avg = fill_sum / fill_n;
                double hi = (double)cap_frames - 1.5 * orate;
                if (hi < ring_target) hi = ring_target;
                ctl_target = avg < ring_target / 4.0 ? ring_target / 4.0 : avg > hi ? hi : avg;
                atomic_store(&W.prime, (uint64_t)ctl_target);
                logf_("network receiver: its level still settling (%.0f s after it was kept): the ring kept at %.0f ms now", t - t_anchor, ctl_target * ms);
                e = (avg - ctl_target) / (double)orate;
            }
            double span = t - t_adjust;
            fill_sum = 0; fill_n = 0; t_adjust = t;
            ppm = clock_regulate_dead(e, span, &steady, W.net && !az_out ? CLOCK_NET_DEAD_S : 0.0);   /* the integral near the target only (pcmclock.h, 09/10); a Squeezebox or UPnP receiver's bursts in a band (an audiolaz receiver reads at its DAC's pace: as before) */
            shift_set(100000 + lround(ppm / 11.0));
        }
        if (log_n && t - t_log >= 60.0) {
            char lat_said[400] = "";
            if (st_lat_ms >= 0) snprintf(lat_said, sizeof lat_said, "; input to output %.0f ms", st_lat_ms);
            /* (22/10, the FOCALE voie at −267 ppm from Roon, unsaid) an audiolaz receiver's pace: its DAC's clock against
             * this machine's (its reports), the ring's pull on it (the two together: the pace a card's app follows) */

            {
                double dp, da, ds, pp, rg, tg, an;
                if (OUT == OUT_NET && !netout_link_pace(&dp, &da, &ds, &pp, &rg, &tg, &an)) {
                    const size_t ll = strlen(lat_said);
                    char dac_said[96];
                    if (ds > 0) snprintf(dac_said, sizeof dac_said, "%+.0f ppm against this machine's clock (its average over %.0f s; %+.0f now)", da, ds, dp);
                    else snprintf(dac_said, sizeof dac_said, "%+.0f ppm against this machine's clock (its estimate now)", dp);
                    snprintf(lat_said + ll, sizeof lat_said - ll, "; Link: its DAC %s, its ring %.0f ms (kept at %.0f, asked %.0f) pulling %+.0f ppm: sent at %+.0f ppm",
                             dac_said, rg, an, tg, pp, dp + pp);
                }
            }
            logf_("clock: ring %.0f ms on average (%.0f–%.0f, target %.0f), DAC buffer %.0f ms, Roon slowed %.1f ppm (steady %.1f), %ld DAC stops, %ld silences, %ld Loopback overruns; longest wait %.0f ms%s",
                  log_sum / log_n * ms, log_low * ms, log_high * ms, ring_target * ms, (double)atomic_load(&W.fill) * ms, ppm, steady,
                  atomic_load(&W.underruns), atomic_load(&W.silences), xruns_in, wait_max * 1000, lat_said);
            if (lat_ring > 0) {
                lat_min_low_ms = log_low * ms;
                logf_("latency: ring %d ms, its lowest %.1f ms this minute (%.1f ms this play), the convolution %s, %ld naps in bursts of work",
                      lat_ring, log_low * ms, lat_low_ms, lat_rt_now ? "in real time" : "at normal priority", lat_drops);
            }
            wait_max = 0;
            t_log = t; log_sum = 0; log_n = 0; log_low = LONG_MAX; log_high = 0;
        }
        airplay_tick(c);                                   /* the AirPlay input's offset : never in its session */
        dac_tick(c, 1);
        air_channels_tick();                               /* its channels, airplay_channels= (air_channels.h) */
        if (t - t_status > 1.0) {
            t_status = t;
            if (st_bp_obits < 32 && atomic_load(&bp.bits_in) > st_bp_obits) bp_say(BP_BITS);   /* requantized for the output */
            write_status("playing", rate, orate, c, clipped, atomic_load(&W.underruns) + xruns_in, ppm, steady, atomic_load(&W.fill), (long)ring_fill(&q),
                         0, -120);
        }

        long silences_now = atomic_load(&W.silences);
        if (silences_now != silences_logged) {
            silences_logged = silences_now;
            logf_("silence: the ring ran dry (%ld this play): the source did not deliver in time; ring now %.0f ms (target %.0f), DAC buffer %.0f ms, Roon slowed %.1f ppm (steady %.1f), %ld Loopback overruns",
                  silences_now, (double)ring_fill(&q) * ms, ring_target * ms, (double)atomic_load(&W.fill) * ms, ppm, steady, xruns_in);
        }

        {
            const long dm = atomic_exchange(&W.dry_ms, 0);
            if (dm > 0) {
                if (dry_hold * 1000 >= 0.5 * dm)
                    logf_("network: nothing to send for %ld ms (the receiver played on from the %ld ms it held): this player held up, its longest turn %.0f ms (its convolution, or a call that waited)", dm, atomic_load(&W.dry_held_ms), dry_hold * 1000);
                else
                    logf_("network: nothing to send for %ld ms (the receiver played on from the %ld ms it held): the source gave nothing in time (%s late), this player's longest turn %.0f ms", dm, atomic_load(&W.dry_held_ms),
                          paced ? "its app, writing to the card," : "the source", dry_hold * 1000);
                dry_hold = 0;
            }
        }
    }

    if (sq && !sq_given && !sq_done && atomic_load(&W.dead) && sq_generation() == sq_gen)
        sq_unread((long)(unheard(&q, &W) / (uint64_t)L) + (cur ? cur->lat : 0)
                  + (lim ? lim_latency(lim) : 0) + UP_LAT(0));
    atomic_store(&W.stop, 1);
    pthread_join(W.th, NULL);
    /* Roon paused, to a network receiver: what it has not played given back now, the writer stopped, just
     * before output_release tells it to stop (its position as close as can be to where it stops) */
    long give_back = 0;
    if (give_late) { give_back = (long)(unheard(&q, &W) / (uint64_t)L) + (cur ? cur->lat : 0); netout_stop_mark(); }
    /* the music ended there (played out, another rate, the source stopped): a network receiver plays out its buffer;
     * stopped, flushed, paused, another source, another output, the receiver lost: stopped at once */
    {   /* the play's end in one line, its reason told to an audiolaz receiver */
        if (!end_why) {
            if (atomic_load(&W.dead)) PLAY_END(NETOUT_WHY_LOST, "the output lost");
            else if (quit) PLAY_END(NETOUT_WHY_QUIT, "the player stops");
            else if (sq && sq_generation() != sq_gen) PLAY_END(NETOUT_WHY_USER, "Roon stopped or flushed its stream");
            else if (net_cut) PLAY_END(NETOUT_WHY_SOURCE, "the source stopped or changed");
            else PLAY_END(NETOUT_WHY_END, "the music's end");
        }
        logf_("the play ends: %s", end_txt);
        if (OUT == OUT_NET) netout_stop_reason(end_why, end_txt);
    }
#undef PLAY_END
    output_release(W.out, !net_cut && !atomic_load(&W.dead) && !quit && !(sq && sq_generation() != sq_gen));
    /* what the receiver played beyond that while it was being stopped (its STMf's elapsed time, 300 ms at most):
     * not given back, never heard twice */
    if (give_late) {
        const long extra = netout_stop_extra(300) / (long)L;
        if (extra > 0) logf_("paused: the receiver played %.0f ms more while it was stopped: not given back", extra * 1000.0 / rate);
        sq_unread(give_back > extra ? give_back - extra : 0);
    }
    if (in) src_close(in);
    free(raw); free(a); free(pcm); free(q.buf);
    lim_free(lim);                                     /* the free player: none (player.h) */
    if (lat_rt_now) lat_rt_set(0);                     /* normal priority between plays */
    lat_rt_want = 0; lat_ring_now = 0;
    if (az_out) netout_fifo_ms(0);
    st_lat_ms = -1; st_heard[0] = 0;
    st_in_ch = 0; st_in_layout[0] = 0; st_dlg_centre = 0; cap_ch = 2;   /* */
    if (keep_reads) logf_("the « audiolaz » input read on at its rate %ld times while the network receiver held the music (its bursts)", keep_reads);
    logf_("stopped (%ld DAC stops, %ld silences, %ld Loopback overruns, %ld frames dropped; %ld clipped samples)",
          atomic_load(&W.underruns), atomic_load(&W.silences), xruns_in, dropped, clipped);
    /* the peaks of this play : the source's as it came (a Loopback input: AirPlay, Spotify…), the output's */
    if (meas_play) logf_("measurement: its play's output peak %.1f dBFS", play_out_peak > 0 ? 20 * log10(play_out_peak) : -150.0);   /* test/engine-measure.sh */
    if (!sq) logf_("peaks this play: input %.1f dBFS (the source as it came%s), output %.1f dBFS", play_in_peak > 0 ? 20 * log10(play_in_peak) : -150.0,
                   air_play ? ", AirPlay: the sender's volume and the source's loudness" : "", play_out_peak > 0 ? 20 * log10(play_out_peak) : -150.0);
    /* its time counted for its session's kind of stream (the input set between sessions for the kind
     * played most): as said now, else as at its start */
    if (air_play && video) {
        const int k = air_styp_buffered();
        air_sess_add((k >= 0 ? k : play_buf) == 1, now_s() - t_play0);
    }
}

int main(int argc, char **argv) {

    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--voie")) { for (int fd = 3; fd < 1024; fd++) close(fd); break; }
    /* --version, anywhere, before anything else : which engine this is, for people (the app reads engine.version) */
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--version")) { puts(ROOMD_ID); return 0; }
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--list-devices")) { out_print_devices(); return 0; }   /* out.h */

    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--who")) {
        for (int k = 1; k + 1 < argc; k++) if (!strcmp(argv[k], "--claim-to")) netout_claims(3489, argv[k + 1]);
        static char who[8192];
        if (netout_who(1500, who, sizeof who)) return 1;
        fputs(who, stdout);
        return 0;
    }

    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc || !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { fprintf(stderr, "usage: audiolaz-roomd [--loop <index|id>|none] [--loop-subs N] [--dac hw:0,0] [--rooms /var/lib/audiolaz] [--rt 0] [--writer-rt 94] [--writer-cpu N] [--mlock 1] [--squeeze <server|auto> --name <name>] [--page 8765 --output <label>] [--net off | --net-port 9001 --slim-port 3484 --airplay-out on --airplay-conf <file>] [--airplay-cal-ms 430] [--airplay-meta /run/audiolaz-airplay/metadata] [--airplay-video-ring-ms 250] [--net-video-hold-ms 350] [--upnp on --upnp-port 49494 --upnp-volume fixed|digital --upnp-interface <if>] | --version\n"); return 2; }
        if (!strcmp(argv[i], "--loop")) { LOOP = argv[i + 1]; NO_LOOP = !strcmp(LOOP, "none"); }
        else if (!strcmp(argv[i], "--loop-subs")) { LOOP_SUBS = atoi(argv[i + 1]); if (LOOP_SUBS < 1) LOOP_SUBS = 1; if (LOOP_SUBS > LOOP_MAX_SUBS) LOOP_SUBS = LOOP_MAX_SUBS; }
        else if (!strcmp(argv[i], "--dac")) DAC = argv[i + 1];
        else if (!strcmp(argv[i], "--rooms")) ROOMS = argv[i + 1];
        else if (!strcmp(argv[i], "--rt")) rt_priority = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--writer-rt")) writer_rt = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--writer-cpu")) writer_cpu = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--mlock")) mem_lock = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--squeeze")) SQ_SERVER = argv[i + 1];
        else if (!strcmp(argv[i], "--name")) SQ_NAME = argv[i + 1];
        else if (!strcmp(argv[i], "--mac-seed")) sq_mac_seed(argv[i + 1]);

        else if (!strcmp(argv[i], "--voie")) VOIE = argv[i + 1];
        else if (!strcmp(argv[i], "--voie-name")) VOIE_NAME = argv[i + 1];
        else if (!strcmp(argv[i], "--voie-out")) VOIE_OUT = argv[i + 1];
        else if (!strcmp(argv[i], "--voie-self")) VOIE_SELF = argv[i + 1];
        else if (!strcmp(argv[i], "--voie-loc")) VOIE_LOC = argv[i + 1];
        else if (!strcmp(argv[i], "--parent-pid")) parent_pid = (pid_t)atol(argv[i + 1]);   /* the app that started it */
        else if (!strcmp(argv[i], "--net-bind")) netout_disc_addr(argv[i + 1]);   /* the AirPlay bridge's discovery there */
        else if (!strcmp(argv[i], "--page")) PAGE_PORT = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--output")) OUT_LABEL = argv[i + 1];
        else if (!strcmp(argv[i], "--net")) NET_ON = strcmp(argv[i + 1], "off") != 0;
        else if (!strcmp(argv[i], "--net-port")) NET_PORT = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--slim-port")) SLIM_PORT = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--airplay-conf")) netout_airplay_conf(argv[i + 1]);   /* the AirPlay bridge's */
        else if (!strcmp(argv[i], "--claim-port")) claim_port_arg = atoi(argv[i + 1]);   /* the tests: another UDP port, 0 none */
        else if (!strcmp(argv[i], "--claim-to")) claim_to_arg = argv[i + 1];
        else if (!strcmp(argv[i], "--airplay-out")) netout_airplay_on(!strcmp(argv[i + 1], "on"));   /* its bridge installed */
        else if (!strcmp(argv[i], "--airplay-cal-ms")) airplay_cal_ms = atof(argv[i + 1]);   /* AIRPLAY_CAL_MS's stand-in */
        else if (!strcmp(argv[i], "--net-video-hold-ms")) {               /* NET_VIDEO_HOLD_MS's stand-in : 100 to 4000 */
            net_video_hold_ms = atof(argv[i + 1]);
            if (!(net_video_hold_ms >= 100)) net_video_hold_ms = 100; else if (net_video_hold_ms > 4000) net_video_hold_ms = 4000;
        }
        else if (!strcmp(argv[i], "--net-airplay-hold-ms")) {             /* NET_AIRPLAY_MUSIC_HOLD_MS's stand-in : 100 to 4000 */
            net_airplay_hold_ms = atof(argv[i + 1]);
            if (!(net_airplay_hold_ms >= 100)) net_airplay_hold_ms = 100; else if (net_airplay_hold_ms > 4000) net_airplay_hold_ms = 4000;
        }
        else if (!strcmp(argv[i], "--tails")) { }                         /* read before (main's start, 11/10) */
        else if (!strcmp(argv[i], "--net-upnp-hold-ms")) {                /* NET_UPNP_MUSIC_HOLD_MS's stand-in : 0 (filled
                                                                          * whole, as before), 300 to 8000 */
            net_upnp_hold_ms = atof(argv[i + 1]);
            if (!(net_upnp_hold_ms > 0)) net_upnp_hold_ms = 0; else if (net_upnp_hold_ms < 300) net_upnp_hold_ms = 300; else if (net_upnp_hold_ms > 8000) net_upnp_hold_ms = 8000;
        }
        else if (!strcmp(argv[i], "--driver-ring-ms")) { drv_ring_ms = atof(argv[i + 1]); if (!(drv_ring_ms >= 20 && drv_ring_ms <= 800)) drv_ring_ms = DRV_RING_MS; }
        else if (!strcmp(argv[i], "--airplay-music-ring-ms")) {            /* AIR_MUSIC_RING_MS's stand-in : 50 to 800 */
            air_music_ring_ms = atof(argv[i + 1]);
            if (!(air_music_ring_ms >= 50)) air_music_ring_ms = 50; else if (air_music_ring_ms > 800) air_music_ring_ms = 800;
        }
        else if (!strcmp(argv[i], "--airplay-meta")) { AIR_META = argv[i + 1]; air_meta_given = 1; }   /* shairport-sync's metadata pipe */
        else if (!strcmp(argv[i], "--airplay-video-ring-ms")) {            /* AIR_VIDEO_RING_MS's stand-in : 50 to 800 */
            air_video_ring_ms = atof(argv[i + 1]);
            if (!(air_video_ring_ms >= 50)) air_video_ring_ms = 50; else if (air_video_ring_ms > 800) air_video_ring_ms = 800;
        }
        else if (out_option(argv[i], argv[i + 1]) || src_option(argv[i], argv[i + 1])) { }   /* the card's, the inputs' own (out.h, src.h) */
        else { fprintf(stderr, "usage: audiolaz-roomd [--loop <index|id>|none] [--loop-subs N] [--dac hw:0,0] [--rooms /var/lib/audiolaz] [--rt 0] [--writer-rt 94] [--writer-cpu N] [--mlock 1] [--squeeze <server|auto> --name <name>] [--page 8765 --output <label>] [--net off | --net-port 9001 --slim-port 3484 --airplay-out on --airplay-conf <file>] [--airplay-cal-ms 430] [--airplay-meta /run/audiolaz-airplay/metadata] [--airplay-video-ring-ms 250] [--net-video-hold-ms 350] [--upnp on --upnp-port 49494 --upnp-volume fixed|digital --upnp-interface <if>] | --version\n"); return 2; }
    }
    /* the built-in inputs (src.h: the UPnP renderer, --upnp on): the inputs gone through, its name; read even with
     * --loop none (no Loopback card then: those inputs alone, beside the Squeezebox one) */
    LOOP_SUBS = src_inputs(LOOP_SUBS);
    src_player_name(SQ_NAME);
    if (NO_LOOP && src_builtin()) { NO_LOOP = 0; LOOP = "none"; }
    if (!DAC) DAC = out_default_dev();               /* plughw:0,0 on Linux, the default device on macOS */
    DAC_ARG = DAC;
    if (NET_ON && (SLIM_PORT == 3483 || NET_PORT == SLIM_PORT || NET_PORT <= 0 || NET_PORT > 65535 || SLIM_PORT < 0 || SLIM_PORT > 65535)) {
        fprintf(stderr, "audiolaz-roomd: --net-port and --slim-port: two ports, never 3483 (Roon's)\n"); return 2;
    }
    signal(SIGPIPE, SIG_IGN);                           /* a network receiver gone mid-write : an error, not the end */
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    if (parent_pid > 0) {                              /* its app gone already: not started at all */
        if (kill(parent_pid, 0) && errno != EPERM) { fprintf(stderr, "audiolaz-roomd: --parent-pid %d: no such process\n", (int)parent_pid); return 1; }
        pthread_t pw;
        if (!pthread_create(&pw, NULL, parent_watch, NULL)) pthread_detach(pw);
    }
    if (VOIE) {
        PAGE_PORT = 0;
#if defined(__linux__) && defined(__GLIBC__)

        { extern int pthread_setattr_default_np(const pthread_attr_t *); pthread_attr_t ta;
          if (!pthread_attr_init(&ta)) { pthread_attr_setstacksize(&ta, 1u << 20); pthread_setattr_default_np(&ta); pthread_attr_destroy(&ta); } }
#endif
        char d[700];
        snprintf(d, sizeof d, "%s/voies", ROOMS); mkdir(d, 0755);
        mkdir(status_dir(), 0755);
        page_conf_follow(ROOMS);                       /* the page's settings (its place's per output) as the player's page keeps them */
        logf_("voie %s: « %s »", VOIE, VOIE_NAME ? VOIE_NAME : "");
    }
    if (!VOIE) {   /* before the control is read: the polarity set under 1.2.8 kept as it sounded (page.c) */
        const int mv = page_channels_migrate(ROOMS);
        if (mv > 0) logf_("channels: the polarity of %d output%s with left and right swapped kept as before (invert_l and invert_r exchanged: since 1.2.9 they name the output's side)", mv, mv > 1 ? "s" : "");
    }
    control c = { .gain = -13, .dyn_lo = DYN_LO, .dyn_hi = DYN_HI };
    c.gain = 0; c.bypass = 1; c.noroom = 1; snprintf(c.place, sizeof c.place, "-");   /* no control file yet: as it came too */
    stamp seen = { 0, 0, 0 };
    read_control(&c, &seen);
#ifdef __linux__
    /* all the engine's memory kept in RAM, now and later: no page fault (a disk read) in the writer's period.
     * Root, or LimitMEMLOCK=infinity in the unit; refused: said once, it plays as before. */
    if (mem_lock && mlockall(MCL_CURRENT | MCL_FUTURE)) logf_("memory not locked (%s)", strerror(errno));
#endif
    pthread_t th;
    pthread_create(&th, NULL, loader, NULL);            /* before the audio thread turns real-time: it stays normal */

    if (rt_priority > 0) {
        struct sched_param sp = { .sched_priority = rt_priority };
        int rt = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rt) logf_("no real-time priority for the audio (%s)", strerror(rt));
    }

    if (rt_priority <= 0) rt_interactive();
    netout_rt(writer_rt > 0 ? 60 : 0);
    if (NO_LOOP) {                                     /* the Squeezebox input only: no card to watch */
        if (!SQ_SERVER) { fprintf(stderr, "audiolaz-roomd: --loop none needs --squeeze\n"); return 2; }
        logf_("audiolaz-roomd: no Loopback (the Squeezebox input only), DAC %s, rooms %s", DAC, ROOMS);
    } else {
        /* macOS: the AirPlay receiver roomd runs, named by airplay_tick's rule when its settings have no name
         * yet, writing its metadata where air_meta_main reads it */
        { char nm[160]; air_name_want(&c, nm, sizeof nm, NULL, 0); src_air_name(nm); }
        if (air_meta_given) src_air_meta(AIR_META);
        src_init(LOOP, LOOP_SUBS, ROOMS);               /* the Loopback (Linux), the pipes (macOS): src.h */
        { const char *m = src_air_meta(NULL); if (m) AIR_META = m; }
        logf_("audiolaz-roomd: %s (the sources play into it), DAC %s, rooms %s", src_what(), DAC, ROOMS);
        pthread_t ath;                                 /* the AirPlay senders : video or music; the player's alone */
        if (!VOIE && !pthread_create(&ath, NULL, air_meta_main, NULL)) pthread_detach(ath);
    }
    status_disc_off();                                 /* an older run's airplay_disc=ok never outlives it (08/10, review) */
    netout_set_self(SQ_NAME);                           /* its own AirPlay input, never an output */
    netout_set_name(SQ_NAME);                           /* said with the receiver it plays to */
    if (claim_port_arg != 3489 || claim_to_arg) netout_claims(claim_port_arg, claim_to_arg);

    char relay_nm[48];
    snprintf(relay_nm, sizeof relay_nm, "audiolaz-slim-%d", VOIE ? (int)parent_pid : (int)getpid());
    if (VOIE && NET_ON) { netout_relay_use(relay_nm); if (VOIE_SELF) netout_set_self(VOIE_SELF); }
    netout_state_dir(ROOMS);                            /* the Link receivers' last addresses, by their ids */
    if (NET_ON && !netout_init(NET_PORT, SLIM_PORT)) page_slim_port = SLIM_PORT;   /* the network outputs */
    if (!VOIE && NET_ON && SLIM_PORT > 0 && netout_relay_serve(relay_nm)) logf_("voies: the Squeezebox players and AirPlay devices not lent (%s)", strerror(errno));
    if (SQ_SERVER && sq_start(SQ_SERVER, SQ_NAME)) logf_("the Squeezebox input could not start");
    else if (SQ_SERVER) logf_("Squeezebox input: \"%s\", server %s", SQ_NAME, SQ_SERVER);
    if (PAGE_PORT && page_start(PAGE_PORT, ROOMS, OUT_LABEL ? OUT_LABEL : DAC)) logf_("page: port %d refused", PAGE_PORT);
    else if (PAGE_PORT) logf_("page: http://<this machine>:%d", PAGE_PORT);
    lat_load();                                        /* the latencies known before this start */
    {

        const int e = rt_limit_install(VOIE || !DIRECT_OUT);
        if (e) logf_("real time: RLIMIT_RTTIME not set (%s): no limit of our own", strerror(e));
#ifdef __linux__
        else if (!VOIE) {
            char b[32] = ""; FILE *f = fopen("/proc/sys/kernel/sched_rt_runtime_us", "r");
            if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); }
            logf_("real time: each thread of ours at most %d ms without blocking (RLIMIT_RTTIME: dropped to normal priority beyond)%s",
                  RT_LIMIT_US / 1000, atol(b) < 0 ? "; the kernel's own throttling is off here (sched_rt_runtime_us -1)" : "");
        }
        if (!VOIE) rt_watch_start();                   /* who held the cores, said after a wake late by seconds */
#endif
    }
    if (!VOIE) voies_start(argc, argv);
    while (!quit) {
        rt_xcpu_tick();                                /* */
        read_control(&c, &seen);
        static double sq_wait = 0;                     /* since when Roon's stream is awaited */
        if (SQ_SERVER && sq_state() == SQ_PLAYING) {
            unsigned r = sq_next_rate();
            if (r) { sq_wait = 0; play(r, &c, &seen, 1); continue; }
            /* Roon's stream asked for and not here yet: waited for, 3 s at most (08/10: falling back to a Loopback source
             * meanwhile opened the DAC three times over as Roon took over) */
            if (!NO_LOOP && LOOP_SUBS > 1) {
                if (sq_wait == 0) sq_wait = now_s();
                if (now_s() - sq_wait < 3.0) { usleep(10000); continue; }
            }
        } else sq_wait = 0;
        if (NO_LOOP) usleep(20000);
        else {                                         /* woken by the Loopback's event: the source that started last */
            struct pollfd efd[8];                      /* the Loopback's, the card's four streams, the UPnP input's */
            const int en = src_event_fds(efd, 8);
            /* after a play ended on its source's next stream at another rate (play_again_now): no wait, it plays */
            const int at_once = play_again_now;
            play_again_now = 0;
            if (en > 0) { poll(efd, (nfds_t)en, at_once ? 0 : SQ_SERVER ? 20 : 100); src_drain(); } else if (!at_once) usleep(SQ_SERVER ? 20000 : 100000);
            const int k = LOOP_SUBS > 1 ? newest_sub() : (slave_active_sub(0) ? 0 : -1);
            if (k >= 0) {
                if (k != cur_sub) logf_("input: Loopback subdevice %d (the source that started last)", k);
                cur_sub = k;
                unsigned r = slave_rate();
                if (r) { play(r, &c, &seen, 0); continue; }
            }
        }
        if (!VOIE) airplay_tick(&c);                   /* the AirPlay input's offset, between its sessions */
        else {                                         /* a voie whose room is no longer bound: done */
            static double unb = 0;
            if (!voie_unbound) unb = 0; else if (!unb) unb = now_s(); else if (now_s() - unb > 10) { logf_("voie %s: its room no longer bound to an output: stopping", VOIE); quit = 1; }
            static double t_probe = -1e9;              /* its renderer not found yet: its description asked directly */
            if (VOIE_LOC && now_s() - t_probe >= 5.0 && !out_present(&c)) { t_probe = now_s(); netout_upnp_probe(VOIE_LOC); }
        }
        dac_tick(&c, 0);
        air_channels_tick();                           /* its channels, airplay_channels= (air_channels.h) */
        { static double t_tick = 0; if (now_s() - t_tick >= 1.0) { t_tick = now_s(); src_tick(); } }   /* src.h */
        { static int k = 0; if (k++ % (SQ_SERVER ? 50 : 10) == 0) write_status("idle", 0, 0, &c, 0, 0, 0, 0, 0, 0, 0, -120); }
    }
    voies_end();                                       /* the voies stopped, their cards taken away */
    src_shutdown();
    status_disc_off();
    pthread_mutex_lock(&lk); pthread_cond_signal(&wake); pthread_mutex_unlock(&lk);
    pthread_join(th, NULL);
    return 0;
}
