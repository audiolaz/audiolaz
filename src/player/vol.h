/* src/player/vol.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_VOL_H
#define AUDIOLAZ_VOL_H
#include <math.h>
#include <stdint.h>
#include "pcmclock.h"                   /* s32_in, s32_out_q: the free player's words (vol_ramp_run_s32) */

#define VOL_MIN_DB (-80.0)              /* the level's floor (the slider's): below it, mute */
#define VOL_ON_DB (-70.0)               /* where it starts when the option is turned on */
#define VOL_MAX_PROV (-30.0)            /* the ceiling until the listener has set a maximum */
#define VOL_FIND_S 900.0                /* the guided step's lease: the ceiling open to the app's slider this long at most */
#define VOL_JUMP_DB 6.0                 /* the most one command raises the level */
#define VOL_UP_DBS 10.0                 /* the level rises at most this fast (dB a second) */
#define VOL_DOWN_DBS 60.0               /* … and falls */
#define VOL_MUTE_S 0.05                 /* a mute's fade out */
#define VOL_UNMUTE_S 0.3                /* … and back */
#define VOL_FULL_DB (-0.05)
#define VOL_FRESH_S 3.0                 /* a source's words this soon after its start: its start's */
#define VOL_SPOTIFY_DB 60.0             /* Spotify's slider, 0 … 65535: librespot's own log scale, 60 dB (50 %: −30 dB) */
#define VOL_LN10_20 0.11512925464970228420   /* ln(10) / 20: a level in dB to its gain, exp(dB × this) */

enum { VS_APP, VS_SPOTIFY, VS_UPNP, VS_ROON, VS_DEVICE, VS_AIRPLAY, VS_N };
static inline const char *vol_src_name(int k) { static const char *const N[VS_N] = { "app", "spotify", "upnp", "roon", "device", "airplay" }; return k >= 0 && k < VS_N ? N[k] : ""; }

/* A level as kept: 0 dB at most, VOL_MIN_DB at least; NAN (none): VOL_ON_DB */
static inline double vol_clamp(double db) { return !(db == db) ? VOL_ON_DB : db > 0 ? 0.0 : db < VOL_MIN_DB ? VOL_MIN_DB : db; }
/* A ceiling as kept: 0 dB at most, VOL_MIN_DB at least; NAN (none set): VOL_MAX_PROV */
static inline double vol_max_clamp(double max) { return !(max == max) ? VOL_MAX_PROV : max > 0 ? 0.0 : max < VOL_MIN_DB ? VOL_MIN_DB : max; }
/* The level once a source asks want dB (NAN: unchanged, a mute alone) while it is cur, under the ceiling max (NAN: the
 * provisional one); fresh: the source's first word. The level never above the ceiling, a lower ceiling bringing it down. */
static inline double vol_rule(double cur, double want, int fresh, double max) {
    max = vol_max_clamp(max);
    cur = vol_clamp(cur);
    if (cur > max) cur = max;
    if (!(want == want)) return cur;
    if (fresh && want >= VOL_FULL_DB) return cur;              /* starting at 100 %: the level kept */
    if (want > cur + VOL_JUMP_DB) want = cur + VOL_JUMP_DB;    /* up: one step at most */
    want = vol_clamp(want);
    return want > max ? max : want;
}
/* The level when the option is turned on: VOL_ON_DB, or the one kept when lower; never above the ceiling */
static inline double vol_on_level(double kept, double max) {
    max = vol_max_clamp(max);
    const double v = kept == kept && kept < VOL_ON_DB ? vol_clamp(kept) : VOL_ON_DB;
    return v > max ? max : v;
}

/* Spotify's slider (librespot's volume, 0 … 65535) and UPnP's Volume (0 … 100, upnp.c's own scale: 40 × log10(v/100),
 * 50: −12 dB, 10: −40 dB) to a level and back */
static inline double vol_db_of_spotify(unsigned v) { return v >= 65535u ? 0.0 : v == 0 ? VOL_MIN_DB : VOL_SPOTIFY_DB * ((double)v / 65535.0 - 1.0); }
static inline unsigned vol_spotify_of_db(double db) {
    if (db >= 0) return 65535u;
    const long v = lround(65535.0 * (1.0 + db / VOL_SPOTIFY_DB));
    return v < 1 ? 1u : v > 65535 ? 65535u : (unsigned)v;      /* below −60 dB: its lowest step, never 0 (Spotify's mute) */
}
static inline double vol_db_of_upnp(int v) { return v >= 100 ? 0.0 : v <= 0 ? VOL_MIN_DB : vol_clamp(40.0 * log10(v / 100.0)); }
static inline int vol_upnp_of_db(double db) {
    if (db >= 0) return 100;
    const long v = lround(100.0 * pow(10.0, db / 40.0));
    return v < 0 ? 0 : v > 100 ? 100 : (int)v;
}

/* ── The stage (the play's thread alone) ──────────────────────────────────────────────────────────────────────────── */
typedef struct {
    int active;                         /* in the chain: the option on, or its way back to the source's own not over */
    double db;                          /* the level now (dB) */
    double mg;
    double tdb;                         /* where the level goes */
    int tmute;                          /* … and the mute */
} vol_ramp;

/* the gain now: the level's, times the mute's fade (1 exactly at 0 dB unmuted) */
static inline double vol_ramp_gain(const vol_ramp *r) {
    const double m = r->mg >= 1.0 ? 1.0 : r->mg <= 0.0 ? 0.0 : 0.5 - 0.5 * cos(M_PI * r->mg);
    return r->db == 0.0 ? m : exp(r->db * VOL_LN10_20) * m;
}
static inline int vol_ramp_settled(const vol_ramp *r) { return r->db == r->tdb && r->mg == (r->tmute ? 0.0 : 1.0); }
/* nothing done to the music: off, or at 0 dB unmuted and not moving */
static inline int vol_ramp_unity(const vol_ramp *r) { return !r->active || (vol_ramp_settled(r) && r->db == 0.0 && r->mg == 1.0); }
/* where it goes (the level kept by vol_clamp) */
static inline void vol_ramp_to(vol_ramp *r, double tdb, int tmute) { r->tdb = vol_clamp(tdb); r->tmute = tmute != 0; }
/* Into the chain, from the gain playing now (g_now: the source's own volume as the chain applied it, 1 none, 0 silent),
 * so that nothing jumps; g_now NAN: nothing plays yet, at once where it goes (a play's first frames). */
static inline void vol_ramp_take(vol_ramp *r, double g_now, double tdb, int tmute) {
    r->active = 1;
    vol_ramp_to(r, tdb, tmute);
    if (!(g_now == g_now)) { r->db = r->tdb; r->mg = r->tmute ? 0.0 : 1.0; }
    else if (g_now > 0) { r->db = vol_clamp(20.0 * log10(g_now)); r->mg = 1.0; }
    else { r->db = r->tdb; r->mg = 0.0; }
}
/* frames stereo frames x (interleaved doubles), in place, at rate frames a second: the level and the mute moved toward
 * their targets frame by frame (up VOL_UP_DBS, down VOL_DOWN_DBS; the mute over VOL_MUTE_S, back over VOL_UNMUTE_S),
 * each frame times its gain. At 0 dB, settled: untouched. */
static inline void vol_ramp_run(vol_ramp *r, double *x, long frames, double rate) {
    if (!r->active || frames <= 0 || !(rate > 0)) return;
    if (vol_ramp_settled(r)) {
        if (r->db == 0.0 && r->mg == 1.0) return;              /* 0 dB: not a multiplication, bit for bit */
        const double g = vol_ramp_gain(r);
        for (long i = 0; i < 2 * frames; i++) x[i] *= g;
        return;
    }
    const double up = VOL_UP_DBS / rate, down = VOL_DOWN_DBS / rate;
    const double mt = r->tmute ? 0.0 : 1.0, ms = 1.0 / ((r->tmute ? VOL_MUTE_S : VOL_UNMUTE_S) * rate);
    for (long i = 0; i < frames; i++) {
        if (r->db != r->tdb) { const double d = r->tdb - r->db; r->db = d > up ? r->db + up : d < -down ? r->db - down : r->tdb; }
        if (r->mg != mt) { const double d = mt - r->mg; r->mg = d > ms ? r->mg + ms : d < -ms ? r->mg - ms : mt; }
        const double g = vol_ramp_gain(r);
        x[2 * i] *= g;
        x[2 * i + 1] *= g;
    }
}

static inline int vol_ramp_run_s32(vol_ramp *r, int32_t *pcm, double *x, long frames, double rate, int obits, uint64_t *rng) {
    if (vol_ramp_unity(r) || frames <= 0) return 0;
    for (long i = 0; i < 2 * frames; i++) x[i] = s32_in(pcm[i]);
    vol_ramp_run(r, x, frames, rate);
    for (long i = 0; i < 2 * frames; i++) pcm[i] = s32_out_q(x[i], obits, rng);
    return 1;
}
#endif
