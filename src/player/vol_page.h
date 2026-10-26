/* src/player/vol_page.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_VOL_PAGE_H
#define AUDIOLAZ_VOL_PAGE_H
#include "vol.h"

static void save_conf(void);
static int out_ok(const char *v);
static int place_ok(const char *v);
static void out_room_of(const char *out, char *id, size_t n);
static void reply(int c, const char *type, const char *body, size_t len);
static double qnum(const char *q, const char *key, int *ok);
static void url_decode(const char *s, char *out, size_t len);
static void vol_dac_of(const char *out, char *k, size_t n);   /* page.c: the DAC an output reaches, "" unknown */
static char rooms_dir[512];
#define VOL_OUTS 16
typedef struct { int set, on, mute, maxset, aut; double db, max, find_until; unsigned seq; char out[160], place[80], dac[96], from[160], pair[24]; } vol_row;   /* pair: an A/B switch's pair ("" none) */   /* aut: made by the safe default, its question not answered; dac: the DAC it reaches ("" unknown); from: the output it was copied from (the same DAC), "" none */
static pthread_mutex_t vol_mu = PTHREAD_MUTEX_INITIALIZER;
static vol_row vol_rows[VOL_OUTS];
static unsigned vol_seq, vol_gen, vol_gen_saved, vol_gen_told;   /* vol_gen: + 1 at each change */
static int vol_by = -1;                                /* the source of the last change (VS_*) */
static char vol_out_now[160], vol_place_now[80];       /* the output playing (the play's thread's last page_vol_for) */
static int vol_out_known = 0;                          /* … asked once at least (before: the control's out=, vol_target) */
static int vol_spot = 0;
static int vol_default_safe = 0;                       /* page.conf volume_default=safe: row-less outputs safe */
static int vol_paths = 0;                              /* page.conf volume_paths=1: its rows per output already */
static int vol_last = -1;                              /* the row page.conf gave last (its volume_dac= line) */
static char vol_next_pair[24];                         /* (23/10, §2.4) the pair of the next row page.conf gives */
/* (23/10, §2.4) the pair in service on each output of an A/B switch (out "" the command line's) */
typedef struct { int set; char out[160], pair[24]; } vol_ab_row;
static vol_ab_row vol_abs[VOL_OUTS];
static const char *vol_pair_now(const char *out) {
    for (int i = 0; i < VOL_OUTS; i++) if (vol_abs[i].set && out_same(out, vol_abs[i].out)) return vol_abs[i].pair;
    return "";
}
static void vol_pair_set(const char *out, const char *pair) {   /* pair "" drops it; under vol_mu */
    int i = -1;
    for (int k = 0; k < VOL_OUTS; k++) if (vol_abs[k].set && out_same(out, vol_abs[k].out)) i = k;
    if (!pair || !pair[0]) { if (i >= 0) vol_abs[i].set = 0; return; }
    for (int k = 0; i < 0 && k < VOL_OUTS; k++) if (!vol_abs[k].set) i = k;
    if (i < 0) i = 0;
    vol_abs[i].set = 1;
    snprintf(vol_abs[i].out, sizeof vol_abs[i].out, "%.159s", out && strcmp(out, "default") ? out : "");
    snprintf(vol_abs[i].pair, sizeof vol_abs[i].pair, "%.23s", pair);
}
/* a pair's tag: letters and digits, 1 to 23 */
static int vol_pair_ok(const char *v) {
    size_t n = 0;
    for (; v && v[n]; n++) if (!((v[n] >= 'a' && v[n] <= 'z') || (v[n] >= '0' && v[n] <= '9'))) return 0;
    return n >= 1 && n <= 23;
}
static double vol_mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
/* out's own row, −1 none (under vol_mu): the answer is the output's (the path's), never its room's */
static int vol_own_p(const char *out, const char *pair) {
    for (int i = 0; i < VOL_OUTS; i++) if (vol_rows[i].set && out_same(out, vol_rows[i].out) && !strcmp(vol_rows[i].pair, pair ? pair : "")) return i;
    return -1;
}
/* (23/10, §2.4) an output of an A/B switch: the row of its pair in service */
static int vol_own(const char *out) { return vol_own_p(out, vol_pair_now(out)); }
/* one device: the same room (a real one) and the same DAC known for both. The free player has no rooms: the
 * same DAC known for both (a row-less output there plays as volume_default says, as before) */
static int vol_same_dev(const char *pa, const char *da, const char *pb, const char *db_) {
    (void)pa; (void)pb;
    return da && db_ && da[0] && !strcmp(da, db_);
}
/* a row of another output of the room place: 1 (its room has an answer of some output), 0 none */
static int vol_room_any(const char *place) {
    if (!place || !place_ok(place)) return 0;
    for (int i = 0; i < VOL_OUTS; i++) if (vol_rows[i].set && !strcmp(place, vol_rows[i].place)) return 1;
    return 0;
}
/* (23/10, §2.4) the output's row of another pair (the amplifier's answer), −1 none */
static int vol_other_pair(const char *out, const char *pair) {
    int best = -1;
    for (int i = 0; i < VOL_OUTS; i++)
        if (vol_rows[i].set && out_same(out, vol_rows[i].out) && strcmp(vol_rows[i].pair, pair) && (best < 0 || vol_rows[i].seq > vol_rows[best].seq)) best = i;
    return best;
}
/* out's own row, of its pair pair (a free one, else the oldest: off at −30 dB to begin with; under vol_mu) */
static vol_row *vol_put_p(const char *out, const char *pair) {
    int i = -1;
    for (int k = 0; k < VOL_OUTS; k++) if (vol_rows[k].set && out_same(out, vol_rows[k].out) && !strcmp(vol_rows[k].pair, pair ? pair : "")) i = k;
    for (int k = 0; i < 0 && k < VOL_OUTS; k++) if (!vol_rows[k].set) i = k;
    if (i < 0) { i = 0; for (int k = 1; k < VOL_OUTS; k++) if (vol_rows[k].seq < vol_rows[i].seq) i = k; }
    vol_row *r = &vol_rows[i];
    if (!(r->set && out_same(out, r->out) && !strcmp(r->pair, pair ? pair : ""))) {
        memset(r, 0, sizeof *r);
        snprintf(r->out, sizeof r->out, "%.159s", out && strcmp(out, "default") ? out : "");
        snprintf(r->pair, sizeof r->pair, "%.23s", pair ? pair : "");
        r->db = VOL_ON_DB; r->max = NAN;
    }
    r->set = 1; r->seq = ++vol_seq;
    return r;
}
static vol_row *vol_put(const char *out) { return vol_put_p(out, vol_pair_now(out)); }
/* the ceiling a source's words meet on row r: the listener's maximum (VOL_MAX_PROV until set); the app's
 * own slider during the guided step, 0 dB */
static double vol_ceiling(const vol_row *r, int src) {
    if (src == VS_APP && r->find_until > 0 && vol_mono() < r->find_until) return 0.0;
    return r->maxset ? vol_max_clamp(r->max) : VOL_MAX_PROV;
}
/* a level brought under row r's ceiling (the sources' one) */
static void vol_under(vol_row *r) { const double c = vol_ceiling(r, -1); if (r->db > c) { r->db = c; vol_gen++; } }
/* out's row made (it has none), playing the room place, reaching the DAC dk ("" unknown): a copy of the newest
 * row of the same device (vol_same_dev: its answer, level, mute and maximum); else, its room having an answer or the
 * default safe, the safe state (on at −70 dB, the provisional ceiling, auto: its question to ask); else off (fixed, as
 * before: nothing asked anywhere in its room, volume_default fixed), or none at all when play (its first play: as before,
 * no row; a request makes one). Under vol_mu; its index, −1 none. */
static int vol_pair_new(const char *out, const char *pair, const char *place, const char *dk);
static int vol_new(const char *out, const char *place, const char *dk, int play) {
    if (vol_pair_now(out)[0]) return vol_pair_new(out, vol_pair_now(out), place, dk);   /* (23/10, §2.4) */
    int src = -1;
    for (int k = 0; k < VOL_OUTS; k++)
        if (vol_rows[k].set && !vol_rows[k].pair[0] && !vol_pair_now(vol_rows[k].out)[0] && !out_same(out, vol_rows[k].out) && vol_same_dev(place, dk, vol_rows[k].place, vol_rows[k].dac) && (src < 0 || vol_rows[k].seq > vol_rows[src].seq)) src = k;
    const int room = vol_room_any(place);
    if (play && src < 0 && !room && !vol_default_safe) return -1;
    vol_row s;
    if (src >= 0) s = vol_rows[src];                   /* before vol_put: it may take that very row (the oldest) */
    vol_row *r = vol_put(out ? out : "");
    const char *nm = r->out[0] ? r->out : "the command line's output";
    if (src >= 0) {
        r->on = s.on; r->aut = s.aut; r->mute = s.mute; r->db = s.db; r->max = s.max; r->maxset = s.maxset; r->find_until = 0;
        snprintf(r->from, sizeof r->from, "%s", s.out[0] ? s.out : "default");
        fprintf(stderr, "page: the volume of %s: the answer of %s, the same DAC (%s): %s\n", nm, r->from, dk,
                s.aut ? "not answered yet, the safe state" : s.on ? "audiolaz sets it" : "fixed");
    } else if (room || vol_default_safe) {
        r->on = 1; r->mute = 0; r->db = VOL_ON_DB; r->max = NAN; r->maxset = 0; r->aut = 1; r->find_until = 0; r->from[0] = 0;
        fprintf(stderr, "page: the volume of %s: the safe state (on at %.0f dB, at most %.0f dB) until its question is answered in the app%s\n", nm, VOL_ON_DB, VOL_MAX_PROV,
                room ? " (its room's answer is another output's, not this one's device)" : "");
    }
    if (place && place_ok(place)) snprintf(r->place, sizeof r->place, "%s", place);
    if (dk && dk[0]) snprintf(r->dac, sizeof r->dac, "%.95s", dk);
    vol_gen++; vol_by = -1;
    return (int)(r - vol_rows);
}
/* (23/10, §2.4) out's row for the pair pair made (it has none): the amplifier's answer (the output's row of another
 * pair, else its plain row), its own level from the safe −70 dB and the provisional ceiling (another pair of speakers:
 * another sensitivity, never the other pair's level); no row of the output at all: as a new output (the safe state when
 * its room has an answer or the default is safe, else off). Under vol_mu; its index. */
static int vol_pair_new(const char *out, const char *pair, const char *place, const char *dk) {
    int src = vol_other_pair(out, pair);
    const int room = vol_room_any(place);
    vol_row s;
    memset(&s, 0, sizeof s);
    if (src >= 0) s = vol_rows[src];
    vol_row *r = vol_put_p(out ? out : "", pair);
    const char *nm = r->out[0] ? r->out : "the command line's output";
    if (src >= 0) {
        r->on = s.on; r->aut = s.aut; r->mute = 0; r->db = VOL_ON_DB; r->max = NAN; r->maxset = 0; r->find_until = 0; r->from[0] = 0;
        fprintf(stderr, "page: the volume of %s for the speakers %s: the amplifier's answer (%s), their own level%s\n", nm, pair,
                s.aut ? "not answered yet, the safe state" : s.on ? "audiolaz sets it" : "fixed", s.on ? " from -70 dB, at most -30 dB until their maximum is set" : "");
    } else {
        r->on = room || vol_default_safe; r->aut = r->on; r->mute = 0; r->db = VOL_ON_DB; r->max = NAN; r->maxset = 0; r->find_until = 0; r->from[0] = 0;
        if (r->on) fprintf(stderr, "page: the volume of %s for the speakers %s: the safe state (on at %.0f dB, at most %.0f dB) until its question is answered in the app\n", nm, pair, VOL_ON_DB, VOL_MAX_PROV);
    }
    if (place && place_ok(place)) snprintf(r->place, sizeof r->place, "%s", place);
    if (dk && dk[0]) snprintf(r->dac, sizeof r->dac, "%.95s", dk);
    vol_gen++; vol_by = -1;
    return (int)(r - vol_rows);
}
/* two rows that agree (answer, level, mute, maximum): the ones of one device move together while they do */
static int vol_in_step(const vol_row *a, const vol_row *b) {
    return a->on == b->on && a->aut == b->aut && a->mute == b->mute && fabs(a->db - b->db) < 0.005 && a->maxset == b->maxset
           && (!a->maxset || fabs(a->max - b->max) < 0.005);
}
/* row r just changed from was: the other rows of its device that agreed with was take it too (one amplifier:
 * its level never different by the way to it; a row that went its own way keeps it; never a jump: they were at the
 * level r was at, and follow r's rules). Under vol_mu. */
static void vol_sync(const vol_row *was, const vol_row *r) {
    if (vol_in_step(was, r) && !(r->find_until > 0) == !(was->find_until > 0)) return;
    if (r->pair[0]) return;                            /* (23/10, §2.4) a pair's level is its own */
    for (int k = 0; k < VOL_OUTS; k++) {
        vol_row *w = &vol_rows[k];
        if (w == r || !w->set || w->pair[0] || !vol_same_dev(r->place, r->dac, w->place, w->dac) || !vol_in_step(w, was)) continue;
        w->on = r->on; w->aut = r->aut; w->mute = r->mute; w->db = r->db; w->max = r->max; w->maxset = r->maxset; w->find_until = r->find_until;
        vol_gen++;
    }
}
int page_vol_for(const char *out, const char *place, double *db, int *mute) {
    if (pthread_mutex_trylock(&vol_mu)) return -1;
    snprintf(vol_out_now, sizeof vol_out_now, "%s", out ? out : "");
    snprintf(vol_place_now, sizeof vol_place_now, "%s", place ? place : "");
    vol_out_known = 1;
    int i = vol_own(out);
    int look = 0;                                      /* no rooms: a row of a known DAC, its answer for the same */
    for (int k = 0; k < VOL_OUTS && !look; k++) look = vol_rows[k].set && vol_rows[k].dac[0];
    look |= vol_default_safe;
    if (i < 0 && look) {
        /* a new output, nothing answered for it: its DAC, looked up once an output (vol_dk_out: none made for
         * it, the next periods take the one found), the lock left meanwhile (a receiver's: netout's lock); then the
         * same device's answer, else the safe state (vol_new) */
        static char vol_dk_out[160] = "\x01", vol_dk[96];
        char dk[96] = "";
        if (strcmp(vol_dk_out, out ? out : "")) {
            pthread_mutex_unlock(&vol_mu);
            vol_dac_of(out, dk, sizeof dk);
            if (pthread_mutex_trylock(&vol_mu)) return -1;
            snprintf(vol_dk_out, sizeof vol_dk_out, "%s", out ? out : "");
            snprintf(vol_dk, sizeof vol_dk, "%s", dk);
        } else snprintf(dk, sizeof dk, "%s", vol_dk);
        i = vol_own(out);
        if (i < 0) i = vol_new(out, place, dk, 1);
    }
    const int on = i >= 0 && vol_rows[i].on;
    if (on) { *db = vol_rows[i].db; *mute = vol_rows[i].mute; }
    if (i >= 0 && place && place_ok(place)) snprintf(vol_rows[i].place, sizeof vol_rows[i].place, "%s", place);
    pthread_mutex_unlock(&vol_mu);
    return on;
}
/* the room each row's output plays in, read again (its bindings: page_outroom), as night_place_update does */
static void vol_place_update(void) {
    char o[VOL_OUTS][160], id[VOL_OUTS][80], dk[VOL_OUTS][96];
    int set[VOL_OUTS];
    pthread_mutex_lock(&vol_mu);
    for (int i = 0; i < VOL_OUTS; i++) { set[i] = vol_rows[i].set; snprintf(o[i], sizeof o[i], "%s", vol_rows[i].out); }
    pthread_mutex_unlock(&vol_mu);
    for (int i = 0; i < VOL_OUTS; i++) { id[i][0] = dk[i][0] = 0; if (set[i]) { out_room_of(o[i], id[i], sizeof id[i]); vol_dac_of(o[i], dk[i], sizeof dk[i]); } }
    pthread_mutex_lock(&vol_mu);
    for (int i = 0; i < VOL_OUTS; i++) {
        if (!vol_rows[i].set || strcmp(vol_rows[i].out, o[i])) continue;
        if (id[i][0]) snprintf(vol_rows[i].place, sizeof vol_rows[i].place, "%.79s", id[i]);
        if (dk[i][0]) snprintf(vol_rows[i].dac, sizeof vol_rows[i].dac, "%.95s", dk[i]);   /* not heard: the one kept */
    }
    pthread_mutex_unlock(&vol_mu);
}
/* (under vol_mu) nothing played yet since the start: the output the control names, and the room it plays in */
static void vol_target(void) {
    if (vol_out_known) return;
    char o[200] = "", id[80] = "", p[1100], line[600];
    snprintf(p, sizeof p, "%s/control", rooms_dir);   /* read here (any thread): never the page's snapshot */
    FILE *f = fopen(p, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; if (!strncmp(line, "out=", 4)) snprintf(o, sizeof o, "%.199s", line + 4); }
        fclose(f);
    }
    out_room_of(o, id, sizeof id);
    snprintf(vol_out_now, sizeof vol_out_now, "%s", o);
    snprintf(vol_place_now, sizeof vol_place_now, "%s", id);
}
double page_vol_src(int src, double want, int mute, int fresh, int try_) {
    if (try_ ? pthread_mutex_trylock(&vol_mu) : pthread_mutex_lock(&vol_mu)) return -INFINITY;
    if (!try_) vol_target();
    const int i = vol_own(vol_out_now);
    double r = NAN;
    if (i >= 0 && vol_rows[i].on) {
        vol_row *w = &vol_rows[i];
        const vol_row was = *w;
        const double nd = vol_rule(w->db, want, fresh, vol_ceiling(w, src));
        const int nm = mute < 0 ? w->mute : mute != 0;
        if (nd != w->db || nm != w->mute) { w->db = nd; w->mute = nm; vol_gen++; vol_by = src >= 0 && src < VS_N ? src : -1; vol_sync(&was, w); }
        r = w->db;
    }
    pthread_mutex_unlock(&vol_mu);
    return r;
}
/* /state's "volume": { can, out (the output playing), on, db, mute (its row's), nowDb, active (the stage now: roomd.c),
 * by (the source of the last change), spotify (librespot says its slider here), max (the ceiling now),
 * maxSet (the listener's own), finding (the guided step), default (safe | fixed), startDb, provDb, rows [{ out
 * ("default" the command line's), place, on, db, mute, max, maxSet, auto (made by the safe default: not answered) }] }.
 * perOut: true (the answer is each output's: ask it per path, POST /volume?out=…&on=); answer: the output
 * playing's, "ask" (the safe state, its question not answered: ask it for THIS output), "audiolaz" (on, answered),
 * "fixed" (off, answered: préampli, DAC, maximum) or "none" (no row: its room has no answer and the default is fixed;
 * plays as before); each row also: answer, dac (the DAC it reaches, "" unknown), with (the other outputs of that DAC in
 * its room: one answer, moved together), from (the output its answer was copied from, the same DAC; "" none). */
static const char *vol_answer(const vol_row *r) { return !r ? "none" : r->on && r->aut ? "ask" : r->on ? "audiolaz" : "fixed"; }
static void vol_jesc(char *s) { for (char *p = s; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' '; }
static int vol_json(char *b, size_t bn, int n) {
    if (n < 1 || b[n - 1] != '}') return n;
    char rows[VOL_OUTS * 900 + 4], ro[160], on_out[160], dj[96], fj[160], wj[VOL_OUTS * 162 + 4];
    size_t o = (size_t)snprintf(rows, sizeof rows, "[");
    double ndb = 0;
    int act = 0;
    roomd_vol_now(&ndb, &act);
    pthread_mutex_lock(&vol_mu);
    const int i = vol_own(vol_out_now);
    const int on = i >= 0 && vol_rows[i].on, mu = i >= 0 && vol_rows[i].mute;
    const double lv = i >= 0 ? vol_rows[i].db : VOL_ON_DB;
    const int by = vol_by;
    const int mset = i >= 0 && vol_rows[i].maxset, finding = i >= 0 && vol_rows[i].find_until > 0 && vol_mono() < vol_rows[i].find_until;
    const double mx = i >= 0 ? vol_ceiling(&vol_rows[i], -1) : VOL_MAX_PROV;
    const char *ans = vol_answer(i >= 0 ? &vol_rows[i] : NULL);
    char pn[24];                                       /* (23/10, §2.4) the pair in service on the output playing */
    snprintf(pn, sizeof pn, "%s", vol_pair_now(vol_out_now));
    snprintf(on_out, sizeof on_out, "%s", vol_out_now[0] ? vol_out_now : "default");
    for (int k = 0; k < VOL_OUTS; k++) {
        if (!vol_rows[k].set) continue;
        snprintf(ro, sizeof ro, "%s", vol_rows[k].out[0] ? vol_rows[k].out : "default");
        vol_jesc(ro);
        snprintf(dj, sizeof dj, "%s", vol_rows[k].dac); vol_jesc(dj);
        snprintf(fj, sizeof fj, "%s", vol_rows[k].from); vol_jesc(fj);
        size_t wo = 0;                                 /* the other outputs of its DAC in its room */
        wj[0] = 0;
        for (int j = 0; j < VOL_OUTS; j++) {
            if (j == k || !vol_rows[j].set || vol_rows[j].pair[0] || vol_rows[k].pair[0] || !vol_same_dev(vol_rows[k].place, vol_rows[k].dac, vol_rows[j].place, vol_rows[j].dac)) continue;
            char oj[160];
            snprintf(oj, sizeof oj, "%s", vol_rows[j].out[0] ? vol_rows[j].out : "default");
            vol_jesc(oj);
            const int x = snprintf(wj + wo, sizeof wj - wo, "%s\"%s\"", wo ? "," : "", oj);
            if (x < 0 || (size_t)x >= sizeof wj - wo) { wj[wo] = 0; break; }
            wo += (size_t)x;
        }
        const int w = snprintf(rows + o, sizeof rows - o, "%s{\"out\":\"%s\",\"place\":\"%s\",\"on\":%s,\"db\":%.2f,\"mute\":%s,\"max\":%.2f,\"maxSet\":%s,\"auto\":%s,"
                               "\"answer\":\"%s\",\"dac\":\"%s\",\"with\":[%s],\"from\":\"%s\",\"pair\":\"%s\",\"now\":%s}", o > 1 ? "," : "", ro,
                               vol_rows[k].place, vol_rows[k].on ? "true" : "false", vol_rows[k].db, vol_rows[k].mute ? "true" : "false",
                               vol_ceiling(&vol_rows[k], -1), vol_rows[k].maxset ? "true" : "false", vol_rows[k].aut ? "true" : "false",
                               vol_answer(&vol_rows[k]), dj, wj, fj, vol_rows[k].pair, !strcmp(vol_rows[k].pair, vol_pair_now(vol_rows[k].out)) ? "true" : "false");
        if (w < 0 || (size_t)w >= sizeof rows - o - 2) break;
        o += (size_t)w;
    }
    pthread_mutex_unlock(&vol_mu);
    snprintf(rows + o, sizeof rows - o, "]");
    for (char *p = on_out; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
    const int m = snprintf(b + n - 1, bn - (size_t)n + 1,
        ",\"volume\":{\"can\":true,\"out\":\"%s\",\"on\":%s,\"db\":%.2f,\"mute\":%s,\"nowDb\":%.2f,\"active\":%s,\"by\":\"%s\",\"spotify\":%s,"
        "\"max\":%.2f,\"maxSet\":%s,\"finding\":%s,\"default\":\"%s\",\"startDb\":%.0f,\"provDb\":%.0f,\"perOut\":true,\"answer\":\"%s\",\"pairs\":true,\"pair\":\"%s\",\"rows\":%s}}",
        on_out, on ? "true" : "false", lv, mu ? "true" : "false", act ? ndb : 0.0, act ? "true" : "false", vol_src_name(by),
        vol_spot ? "true" : "false", mx, mset ? "true" : "false", finding ? "true" : "false", vol_default_safe ? "safe" : "fixed", VOL_ON_DB, VOL_MAX_PROV, ans, pn, rows);
    if (m > 0 && (size_t)m < bn - (size_t)n + 1) return n - 1 + m;
    b[n - 1] = '}'; b[n] = 0;
    return n;
}

static void vol_spotify(int on, double t) {
    static unsigned long sess = 0;
    static long seen = -1;                              /* its slider as last read or set (−1 none) */
    static double since = 0;
    char ans[64];
    if (ri_spotify_ask("volume", ans, sizeof ans, 1)) { vol_spot = 0; return; }
    unsigned v = 0;
    unsigned long s = 0;
    if (sscanf(ans, "volume %u %lu", &v, &s) != 2 || v > 65535) { vol_spot = 0; return; }
    vol_spot = 1;
    if (!s) { sess = 0; seen = -1; return; }
    if (s != sess) { sess = s; since = t; seen = -1; }
    if (!on) { seen = (long)v; return; }
    if ((long)v != seen) {
        const int fresh = t - since < VOL_FRESH_S;
        const double lv = page_vol_src(VS_SPOTIFY, v ? vol_db_of_spotify(v) : NAN, v == 0, fresh, 0);
        if (lv == lv) fprintf(stderr, "page: Spotify's volume %u (%.1f dB)%s: the level %.1f dB\n", v, v ? vol_db_of_spotify(v) : -INFINITY,
                              fresh ? ", at its start" : "", lv);
        seen = (long)v;
    }
    pthread_mutex_lock(&vol_mu);
    const int i = vol_own(vol_out_now);
    const int lon = i >= 0 && vol_rows[i].on, mu = i >= 0 && vol_rows[i].mute;
    const double lv = i >= 0 ? vol_rows[i].db : 0;
    pthread_mutex_unlock(&vol_mu);
    if (!lon || mu || !seen) return;                    /* muted: its slider left where it is */
    const unsigned want = vol_spotify_of_db(lv);
    if (seen >= 0 && fabs(vol_db_of_spotify((unsigned)seen) - lv) < 0.3) return;
    char l[32], a2[32];
    snprintf(l, sizeof l, "volume %u", want);
    if (!ri_spotify_ask(l, a2, sizeof a2, 0) && !strcmp(a2, "ok")) seen = (long)want;
}
/* The page's thread, a few times a second: a change kept (page.conf, a second after it), told (UPnP's renderer),
 * Spotify's slider read */
static void vol_tick(void) {
    static double t_last = 0, t_change = 0;
    static int was_on = 0;
    const double t = vol_mono();
    if (t - t_last < 0.25) return;
    t_last = t;
    pthread_mutex_lock(&vol_mu);
    int any = 0;
    for (int k = 0; k < VOL_OUTS; k++) any |= vol_rows[k].set && vol_rows[k].on;
    pthread_mutex_unlock(&vol_mu);
    if (any) vol_spotify(1, t); else vol_spot = 0;     /* off everywhere: librespot never asked */
    pthread_mutex_lock(&vol_mu);
    for (int k = 0; k < VOL_OUTS; k++)                 /* a guided step over (its lease): under the ceiling */
        if (vol_rows[k].set && vol_rows[k].find_until > 0 && t >= vol_rows[k].find_until) { vol_rows[k].find_until = 0; vol_under(&vol_rows[k]); }
    const unsigned gen = vol_gen;
    const int i = vol_own(vol_out_now);
    const int on = i >= 0 && vol_rows[i].on, mu = i >= 0 && vol_rows[i].mute, by = vol_by;
    const double lv = i >= 0 ? vol_rows[i].db : 0;
    pthread_mutex_unlock(&vol_mu);
    if (gen != vol_gen_told || on != was_on) {
        if (on) upnp_vol_follow(lv, mu);
        else if (was_on) upnp_vol_follow(NAN, 0);
        if (gen != vol_gen_told && on) fprintf(stderr, "page: the volume %.1f dB%s (%s)\n", lv, mu ? ", muted" : "", by >= 0 ? vol_src_name(by) : "the page");
        vol_gen_told = gen; was_on = on;
        t_change = t;
    }
    if (gen != vol_gen_saved && t - t_change >= 1.0) { vol_gen_saved = gen; save_conf(); }
}
/* POST /volume (the comment at the top), its answer sent on c */
static void vol_post(int c, const char *q) {
    const char *o = q ? strstr(q, "out=") : NULL, *e = q ? strstr(q, "on=") : NULL, *mq = q ? strstr(q, "mute=") : NULL;
    const char *fq = q ? strstr(q, "find=") : NULL, *xq = q ? strstr(q, "maxset=") : NULL, *dq = q ? strstr(q, "default=") : NULL;
    /* (23/10, §2.4) pair=<tag>|- (the row of that pair; - drops the A/B), use=1 (that pair in service), was=<tag> */
    const char *pq = q ? strstr(q, "pair=") : NULL, *uq = q ? strstr(q, "use=") : NULL, *wq = q ? strstr(q, "was=") : NULL;
    if (pq && !(pq == q || pq[-1] == '&')) pq = NULL;
    if (uq && !(uq == q || uq[-1] == '&')) uq = NULL;
    if (wq && !(wq == q || wq[-1] == '&')) wq = NULL;
    char pv[32] = "", wv[32] = "";
    if (pq) { size_t k = 0; for (const char *x = pq + 5; *x && *x != '&' && k < sizeof pv - 1; x++) pv[k++] = *x; pv[k] = 0; }
    if (wq) { size_t k = 0; for (const char *x = wq + 4; *x && *x != '&' && k < sizeof wv - 1; x++) wv[k++] = *x; wv[k] = 0; }
    const int uv = uq ? (uq[4] == '1' && (!uq[5] || uq[5] == '&') ? 1 : -1) : 0;
    const int drop = pq && !strcmp(pv, "-");
    const int pgood = (!pq || drop || vol_pair_ok(pv)) && (!wq || (pq && !drop && vol_pair_ok(wv))) && uv >= 0 && (!uq || (pq && !drop));
    if (fq && !(fq == q || fq[-1] == '&')) fq = NULL;
    if (xq && !(xq == q || xq[-1] == '&')) xq = NULL;
    if (dq && !(dq == q || dq[-1] == '&')) dq = NULL;
    const int fv = fq ? (fq[5] == '1' ? 1 : fq[5] == '0' ? 0 : -1) : -2, xv = xq ? (xq[7] == '1' ? 1 : xq[7] == '0' ? 0 : -1) : -2;
    const int dv2 = dq ? (!strncmp(dq + 8, "safe", 4) && (!dq[12] || dq[12] == '&') ? 1 : !strncmp(dq + 8, "fixed", 5) && (!dq[13] || dq[13] == '&') ? 0 : -1) : -2;
    char to[200] = "";
    if (o && (o == q || o[-1] == '&')) url_decode(o + 4, to, sizeof to);
    if (!strcmp(to, "default")) to[0] = 0;
    if (e && !(e == q || e[-1] == '&')) e = NULL;
    if (mq && !(mq == q || mq[-1] == '&')) mq = NULL;
    int kd = 0, ks = 0, km = 0;
    const double dv = qnum(q, "db", &kd), sv = qnum(q, "step", &ks), mv = qnum(q, "max", &km);
    const int good = out_ok(to) && (!e || ((e[3] == '0' || e[3] == '1') && (!e[4] || e[4] == '&'))) && (!mq || ((mq[5] == '0' || mq[5] == '1') && (!mq[6] || mq[6] == '&')))
                     && (!kd || (dv == dv && dv >= -200 && dv <= 20)) && (!ks || (sv == sv && fabs(sv) <= 100)) && (!km || (mv == mv && mv >= VOL_MIN_DB && mv <= 0))
                     && fv != -1 && xv != -1 && dv2 != -1 && (xv == -2 || km || xv == 0) && pgood && (e || mq || kd || ks || km || fv >= 0 || xv >= 0 || dv2 >= 0 || pq);
    const char *why = good ? NULL : "bad";
    char rid[80] = "", dk[96] = "";
    if (good) vol_place_update();
    if (good) { out_room_of(to, rid, sizeof rid); vol_dac_of(to, dk, sizeof dk); }   /* a row to make: its room, its DAC */
    if (good && dv2 >= 0) {                            /* the default of outputs with no row of their own */
        pthread_mutex_lock(&vol_mu);
        if (vol_default_safe != dv2) { vol_default_safe = dv2; vol_gen++; vol_by = VS_APP; }
        pthread_mutex_unlock(&vol_mu);
        fprintf(stderr, "page: the volume of outputs not answered yet: %s\n", dv2 ? "safe (on at -70 dB, at most -30 dB)" : "fixed (as before)");
    }
    if (good && pq) {                                  /* (23/10, §2.4) the A/B switch declared, its pair chosen, or dropped */
        pthread_mutex_lock(&vol_mu);
        const char *now = vol_pair_now(to);
        if (drop) {
            if (now[0]) {                              /* the pair in service's row: the output's plain row again */
                const int keep = vol_own_p(to, now);
                for (int k = 0; k < VOL_OUTS; k++) if (k != keep && vol_rows[k].set && out_same(to, vol_rows[k].out)) vol_rows[k].set = 0;
                if (keep >= 0) vol_rows[keep].pair[0] = 0;
                vol_pair_set(to, "");
                vol_gen++; vol_by = VS_APP;
                fprintf(stderr, "page: the volume of %s: one pair of speakers again (the A/B switch dropped), the level of %s kept\n", to[0] ? to : "the command line's output", now);
            }
        } else {
            if (!now[0]) {                             /* its first declaration: its plain row is the pair in service's */
                const int plain = vol_own_p(to, "");   /* (was=, else the pair this request names): nothing heard changes */
                const char *first = wq ? wv : pv;
                if (plain >= 0) snprintf(vol_rows[plain].pair, sizeof vol_rows[plain].pair, "%s", first);
                vol_pair_set(to, first);
                vol_gen++;
                fprintf(stderr, "page: the volume of %s: an A/B switch, each pair of speakers its own level (%s in service)\n", to[0] ? to : "the command line's output", first);
            }
            if (vol_own_p(to, pv) < 0) vol_pair_new(to, pv, rid, dk);
            if (uv && strcmp(vol_pair_now(to), pv)) {
                vol_pair_set(to, pv);
                vol_gen++; vol_by = VS_APP;
                fprintf(stderr, "page: the volume of %s: the speakers %s in service (their own level)\n", to[0] ? to : "the command line's output", pv);
            }
        }
        pthread_mutex_unlock(&vol_mu);
        save_conf();
    }
    if (good && (e || mq || kd || ks || km || fv >= 0 || xv >= 0)) {
        pthread_mutex_lock(&vol_mu);
        /* out's own row; none: made as its first play would (vol_new: the same device's answer, else the safe
         * state, else off), never the room's row of another device. (§2.4) pair=: that pair's row */
        int own = pq && !drop ? vol_own_p(to, pv) : vol_own(to);
        if (own < 0) own = pq && !drop ? vol_pair_new(to, pv, rid, dk) : vol_new(to, rid, dk, 0);
        vol_row *r = &vol_rows[own];
        const vol_row was0 = *r;
        if (e) {
            const int on = e[3] == '1';
            if (on && !r->on) { r->db = vol_on_level(r->db, vol_ceiling(r, -1)); r->mute = 0; }   /* turned on: never at full */
            if (on != r->on || r->aut) { r->on = on; r->aut = 0; vol_gen++; vol_by = VS_APP; }   /* answered */
            if (!on) r->find_until = 0;
            /* (23/10, §2.4) the amplifier's answer: every pair of the output takes it (turned on: never at full) */
            for (int k = 0; k < VOL_OUTS; k++) {
                vol_row *w = &vol_rows[k];
                if (w == r || !w->set || !r->pair[0] || !out_same(r->out, w->out)) continue;
                if (on && !w->on) { w->db = vol_on_level(w->db, vol_ceiling(w, -1)); w->mute = 0; }
                if (w->on != on || w->aut) { w->on = on; w->aut = 0; vol_gen++; }
                if (!on) w->find_until = 0;
            }
        }
        if ((kd || ks || mq || fv == 1) && !r->on) why = "off";
        else {
            const double was = r->db, wmx = r->max;
            const int wm = r->mute, wset = r->maxset;
            /* the listener's maximum: set (maxset=1), or back to the provisional one; the level under it */
            if (km || xv == 0) { r->maxset = xv == 1 || (xv == -2 && km); r->max = r->maxset ? vol_max_clamp(mv) : NAN; }
            if (fv == 1) r->find_until = vol_mono() + VOL_FIND_S;
            if (fv == 0) r->find_until = 0;
            if (kd) r->db = vol_rule(r->db, dv, 0, vol_ceiling(r, VS_APP));
            if (ks) r->db = vol_rule(r->db, r->db + sv, 0, vol_ceiling(r, VS_APP));
            if (mq) r->mute = mq[5] == '1';
            if (!(r->find_until > 0)) vol_under(r);
            if (r->db != was || r->mute != wm || r->maxset != wset || !(r->max == wmx || (r->max != r->max && wmx != wmx))) { vol_gen++; vol_by = VS_APP; }
        }
        vol_sync(&was0, r);                            /* the other outputs of its DAC that agreed with it */
        pthread_mutex_unlock(&vol_mu);
        if (!why) save_conf();
    }
    if (why) { char bad[48]; const int k = snprintf(bad, sizeof bad, "{\"ok\":false,\"why\":\"%s\"}", why); reply(c, "application/json", bad, (size_t)k); return; }
    char b[VOL_OUTS * 900 + 600] = "{\"ok\":true}";
    int n2 = vol_json(b, sizeof b, (int)strlen(b));
    reply(c, "application/json", b, (size_t)n2);
}
/* page.conf: its rows (save_conf, under nothing), a line of it (load_conf) */
static void vol_conf_save(FILE *f) {
    pthread_mutex_lock(&vol_mu);
    if (vol_default_safe) fprintf(f, "volume_default=safe\n");   /* */
    fprintf(f, "volume_paths=1\n");                    /* the rows per output (a page.conf of before: vol_migrate) */
    for (int i = 0; i < VOL_OUTS; i++) if (vol_abs[i].set) fprintf(f, "volume_ab=%s|%s\n", vol_abs[i].pair, vol_abs[i].out);   /* (23/10, §2.4) */
    /* (§2.4) the rows of pairs not in service first: a player of before keeps the last row of an output, the one in service */
    for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < VOL_OUTS; i++) {               /* <on: 2 the safe default's>,<mute>,<dB>,<max: nan until set>,<maxset>,<out> */
        if (!vol_rows[i].set) continue;
        if ((pass == 0) != (vol_rows[i].pair[0] && strcmp(vol_rows[i].pair, vol_pair_now(vol_rows[i].out)))) continue;
        char mx[16];
        if (vol_rows[i].maxset) snprintf(mx, sizeof mx, "%.2f", vol_max_clamp(vol_rows[i].max)); else snprintf(mx, sizeof mx, "nan");
        if (vol_rows[i].pair[0]) fprintf(f, "volume_pair=%s\n", vol_rows[i].pair);   /* (23/10, §2.4) the next row's pair */
        fprintf(f, "volume=%d,%d,%.2f,%s,%d,%s\n", vol_rows[i].on ? (vol_rows[i].aut ? 2 : 1) : 0, vol_rows[i].mute, vol_rows[i].db, mx, vol_rows[i].maxset, vol_rows[i].out);
        if (vol_rows[i].dac[0]) fprintf(f, "volume_dac=%s\n", vol_rows[i].dac);   /* its DAC */
    }
    vol_gen_saved = vol_gen;
    pthread_mutex_unlock(&vol_mu);
}
static int vol_conf_line(const char *line) {
    if (!strncmp(line, "volume_default=", 15)) {       /* safe | fixed */
        pthread_mutex_lock(&vol_mu);
        vol_default_safe = !strncmp(line + 15, "safe", 4);
        pthread_mutex_unlock(&vol_mu);
        return 1;
    }
    if (!strncmp(line, "volume_paths=", 13)) { pthread_mutex_lock(&vol_mu); vol_paths = atoi(line + 13) >= 1; pthread_mutex_unlock(&vol_mu); return 1; }   /* */
    if (!strncmp(line, "volume_pair=", 12)) {          /* (23/10, §2.4) the pair of the next row */
        pthread_mutex_lock(&vol_mu);
        snprintf(vol_next_pair, sizeof vol_next_pair, "%.23s", vol_pair_ok(line + 12) ? line + 12 : "");
        pthread_mutex_unlock(&vol_mu);
        return 1;
    }
    if (!strncmp(line, "volume_ab=", 10)) {            /* (23/10, §2.4) <pair>|<out>: the pair in service */
        const char *bar = strchr(line + 10, '|');
        char pr[32] = "";
        if (bar && bar - (line + 10) < (long)sizeof pr) { memcpy(pr, line + 10, (size_t)(bar - (line + 10))); pr[bar - (line + 10)] = 0; }
        if (bar && vol_pair_ok(pr) && out_ok(bar + 1)) { pthread_mutex_lock(&vol_mu); vol_pair_set(bar + 1, pr); pthread_mutex_unlock(&vol_mu); }
        return 1;
    }
    if (!strncmp(line, "volume_dac=", 11)) {           /* the DAC of the row just read */
        pthread_mutex_lock(&vol_mu);
        if (vol_last >= 0 && vol_rows[vol_last].set) {
            snprintf(vol_rows[vol_last].dac, sizeof vol_rows[vol_last].dac, "%.95s", line + 11);
            for (char *p = vol_rows[vol_last].dac; *p; p++) if ((unsigned char)*p < 0x20) *p = ' ';
        }
        pthread_mutex_unlock(&vol_mu);
        return 1;
    }
    if (strncmp(line, "volume=", 7)) return 0;
    char np[24];
    pthread_mutex_lock(&vol_mu);
    snprintf(np, sizeof np, "%s", vol_next_pair);
    vol_next_pair[0] = 0;
    pthread_mutex_unlock(&vol_mu);
    int on = 0, mu = 0, at = 0, ms = 0, at2 = 0;       /* <on>,<mute>,<dB>,<max>,<maxset>,<out>; before: <on>,<mute>,<dB>,<out> */
    double v = NAN, mx = NAN;
    char mt[16] = "";
    const int n6 = sscanf(line + 7, "%d,%d,%lf,%15[^,],%d,%n", &on, &mu, &v, mt, &ms, &at2);
    if (n6 == 5 && at2 > 0 && (!strcmp(mt, "nan") || sscanf(mt, "%lf", &mx) == 1)) at = at2;
    else if (sscanf(line + 7, "%d,%d,%lf,%n", &on, &mu, &v, &at) != 3) at = 0;
    if (at > 0 && out_ok(line + 7 + at)) {
        pthread_mutex_lock(&vol_mu);
        vol_row *r = vol_put_p(line + 7 + at, np);     /* (§2.4) its pair, if any, said just before */
        r->on = on == 1 || on == 2; r->aut = on == 2; r->mute = mu == 1; r->db = vol_clamp(v);
        r->maxset = at == at2 && ms == 1 && mx == mx; r->max = r->maxset ? vol_max_clamp(mx) : NAN;
        if (r->db > vol_ceiling(r, -1)) r->db = vol_ceiling(r, -1);
        vol_last = (int)(r - vol_rows);
        vol_gen_saved = vol_gen_told = vol_gen;
        pthread_mutex_unlock(&vol_mu);
    }
    return 1;
}
/* page.conf read again: its marker as the file says it (load_conf, before its lines) */
static void vol_conf_begin(void) { pthread_mutex_lock(&vol_mu); vol_paths = 0; vol_last = -1; memset(vol_abs, 0, sizeof vol_abs); vol_next_pair[0] = 0; pthread_mutex_unlock(&vol_mu); }
/* a page.conf of before (no volume_paths=): its answers were its rooms'. Each output bound to a room (the
 * control's outroom.<out>= lines, and its out=) with no row of its own, its room having one, gets a copy of the row it
 * played by (the newest of its room's: vol_find's of before), so nothing that played changes; then volume_paths=1, and an
 * output bound afterwards starts at the safe state (vol_new). After the rows' rooms are read (vol_place_update). The
 * number of rows made, −1 nothing to migrate (the main player saves page.conf from 0 on, its marker with it: page_start). */
static int vol_migrate(void) {
    pthread_mutex_lock(&vol_mu);
    int any = 0;
    for (int i = 0; i < VOL_OUTS; i++) any |= vol_rows[i].set;
    const int done = vol_paths;
    vol_paths = 1;
    pthread_mutex_unlock(&vol_mu);
    if (done || !any) return -1;
    enum { MK = 40 };
    static char keys[MK][160];
    int nk = 0;
    char p[1100], line[600];
    snprintf(p, sizeof p, "%s/control", rooms_dir);
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f) && nk < MK) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "outroom.", 8)) {
            char *eq = strrchr(line + 8, '=');
            if (eq) { *eq = 0; snprintf(keys[nk++], sizeof keys[0], "%.159s", line + 8); }
        } else if (!strncmp(line, "out=", 4)) snprintf(keys[nk++], sizeof keys[0], "%.159s", line + 4);
    }
    if (f) fclose(f);
    int made = 0;
    for (int k = 0; k < nk; k++) {
        if (!out_ok(keys[k])) continue;
        char id[80] = "";
        out_room_of(keys[k], id, sizeof id);
        if (!id[0]) continue;
        pthread_mutex_lock(&vol_mu);
        int best = -1;
        if (vol_own(keys[k]) < 0)
            for (int i = 0; i < VOL_OUTS; i++)
                if (vol_rows[i].set && !strcmp(id, vol_rows[i].place) && (best < 0 || vol_rows[i].seq > vol_rows[best].seq)) best = i;
        if (best >= 0) {
            const vol_row s = vol_rows[best];
            vol_row *r = vol_put(keys[k]);
            r->on = s.on; r->aut = s.aut; r->mute = s.mute; r->db = s.db; r->max = s.max; r->maxset = s.maxset;
            snprintf(r->place, sizeof r->place, "%s", id);
            snprintf(r->from, sizeof r->from, "%s", s.out[0] ? s.out : "default");
            made++;
            fprintf(stderr, "page: the volume of %s: its room's answer kept as its own (%s's, before the answers per output)\n", r->out[0] ? r->out : "the command line's output", r->from);
        }
        pthread_mutex_unlock(&vol_mu);
    }
    if (made) vol_place_update();                      /* their DACs */
    return made;
}
#endif
