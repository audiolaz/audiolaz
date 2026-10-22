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
static char rooms_dir[512];
#define VOL_OUTS 16
typedef struct { int set, on, mute, maxset, aut; double db, max, find_until; unsigned seq; char out[160], place[80]; } vol_row;   /* aut: made by the safe default, its question not answered */
static pthread_mutex_t vol_mu = PTHREAD_MUTEX_INITIALIZER;
static vol_row vol_rows[VOL_OUTS];
static unsigned vol_seq, vol_gen, vol_gen_saved, vol_gen_told;   /* vol_gen: + 1 at each change */
static int vol_by = -1;                                /* the source of the last change (VS_*) */
static char vol_out_now[160], vol_place_now[80];       /* the output playing (the play's thread's last page_vol_for) */
static int vol_out_known = 0;                          /* … asked once at least (before: the control's out=, vol_target) */
static int vol_spot = 0;
static int vol_default_safe = 0;                       /* page.conf volume_default=safe: row-less outputs safe */
static double vol_mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
/* out's row, else the latest of an output playing the same room; −1 none (under vol_mu) */
static int vol_find(const char *out, const char *place) {
    for (int i = 0; i < VOL_OUTS; i++) if (vol_rows[i].set && out_same(out, vol_rows[i].out)) return i;
    int best = -1;
    if (place && place_ok(place))
        for (int i = 0; i < VOL_OUTS; i++)
            if (vol_rows[i].set && vol_rows[i].place[0] && !strcmp(place, vol_rows[i].place) && (best < 0 || vol_rows[i].seq > vol_rows[best].seq)) best = i;
    return best;
}
/* out's own row (a free one, else the oldest: off at −30 dB to begin with; under vol_mu) */
static vol_row *vol_put(const char *out) {
    int i = -1;
    for (int k = 0; k < VOL_OUTS; k++) if (vol_rows[k].set && out_same(out, vol_rows[k].out)) i = k;
    for (int k = 0; i < 0 && k < VOL_OUTS; k++) if (!vol_rows[k].set) i = k;
    if (i < 0) { i = 0; for (int k = 1; k < VOL_OUTS; k++) if (vol_rows[k].seq < vol_rows[i].seq) i = k; }
    vol_row *r = &vol_rows[i];
    if (!(r->set && out_same(out, r->out))) {
        memset(r, 0, sizeof *r);
        snprintf(r->out, sizeof r->out, "%.159s", out && strcmp(out, "default") ? out : "");
        r->db = VOL_ON_DB; r->max = NAN;
    }
    r->set = 1; r->seq = ++vol_seq;
    return r;
}
/* the ceiling a source's words meet on row r: the listener's maximum (VOL_MAX_PROV until set); the app's
 * own slider during the guided step, 0 dB */
static double vol_ceiling(const vol_row *r, int src) {
    if (src == VS_APP && r->find_until > 0 && vol_mono() < r->find_until) return 0.0;
    return r->maxset ? vol_max_clamp(r->max) : VOL_MAX_PROV;
}
/* a level brought under row r's ceiling (the sources' one) */
static void vol_under(vol_row *r) { const double c = vol_ceiling(r, -1); if (r->db > c) { r->db = c; vol_gen++; } }
int page_vol_for(const char *out, const char *place, double *db, int *mute) {
    if (pthread_mutex_trylock(&vol_mu)) return -1;
    snprintf(vol_out_now, sizeof vol_out_now, "%s", out ? out : "");
    snprintf(vol_place_now, sizeof vol_place_now, "%s", place ? place : "");
    vol_out_known = 1;
    int i = vol_find(out, place);
    if (i < 0 && vol_default_safe) {                   /* a new output, nothing answered yet: safe */
        vol_row *r = vol_put(out ? out : "");
        r->on = 1; r->mute = 0; r->db = VOL_ON_DB; r->max = NAN; r->maxset = 0; r->aut = 1;
        if (place && place_ok(place)) snprintf(r->place, sizeof r->place, "%s", place);
        vol_gen++; vol_by = -1;
        i = (int)(r - vol_rows);
        fprintf(stderr, "page: the volume of %s: the safe state (on at %.0f dB, at most %.0f dB) until its question is answered in the app\n", r->out[0] ? r->out : "the command line's output", VOL_ON_DB, VOL_MAX_PROV);
    }
    const int on = i >= 0 && vol_rows[i].on;
    if (on) {
        *db = vol_rows[i].db; *mute = vol_rows[i].mute;
        if (out_same(out, vol_rows[i].out) && place && place_ok(place)) snprintf(vol_rows[i].place, sizeof vol_rows[i].place, "%s", place);
    }
    pthread_mutex_unlock(&vol_mu);
    return on;
}
/* the room each row's output plays in, read again (its bindings: page_outroom), as night_place_update does */
static void vol_place_update(void) {
    char o[VOL_OUTS][160], id[VOL_OUTS][80];
    int set[VOL_OUTS];
    pthread_mutex_lock(&vol_mu);
    for (int i = 0; i < VOL_OUTS; i++) { set[i] = vol_rows[i].set; snprintf(o[i], sizeof o[i], "%s", vol_rows[i].out); }
    pthread_mutex_unlock(&vol_mu);
    for (int i = 0; i < VOL_OUTS; i++) { id[i][0] = 0; if (set[i]) out_room_of(o[i], id[i], sizeof id[i]); }
    pthread_mutex_lock(&vol_mu);
    for (int i = 0; i < VOL_OUTS; i++)
        if (vol_rows[i].set && !strcmp(vol_rows[i].out, o[i]) && id[i][0]) snprintf(vol_rows[i].place, sizeof vol_rows[i].place, "%.79s", id[i]);
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
    const int i = vol_find(vol_out_now, vol_place_now);
    double r = NAN;
    if (i >= 0 && vol_rows[i].on) {
        vol_row *w = &vol_rows[i];
        const double nd = vol_rule(w->db, want, fresh, vol_ceiling(w, src));
        const int nm = mute < 0 ? w->mute : mute != 0;
        if (nd != w->db || nm != w->mute) { w->db = nd; w->mute = nm; vol_gen++; vol_by = src >= 0 && src < VS_N ? src : -1; }
        r = w->db;
    }
    pthread_mutex_unlock(&vol_mu);
    return r;
}
/* /state's "volume": { can, out (the output playing), on, db, mute (its row's), nowDb, active (the stage now: roomd.c),
 * by (the source of the last change), spotify (librespot says its slider here), max (the ceiling now),
 * maxSet (the listener's own), finding (the guided step), default (safe | fixed), startDb, provDb, rows [{ out
 * ("default" the command line's), place, on, db, mute, max, maxSet, auto (made by the safe default: not answered) }] } */
static int vol_json(char *b, size_t bn, int n) {
    if (n < 1 || b[n - 1] != '}') return n;
    char rows[VOL_OUTS * 300 + 4], ro[160], on_out[160];
    size_t o = (size_t)snprintf(rows, sizeof rows, "[");
    double ndb = 0;
    int act = 0;
    roomd_vol_now(&ndb, &act);
    pthread_mutex_lock(&vol_mu);
    const int i = vol_find(vol_out_now, vol_place_now);
    const int on = i >= 0 && vol_rows[i].on, mu = i >= 0 && vol_rows[i].mute;
    const double lv = i >= 0 ? vol_rows[i].db : VOL_ON_DB;
    const int by = vol_by;
    const int mset = i >= 0 && vol_rows[i].maxset, finding = i >= 0 && vol_rows[i].find_until > 0 && vol_mono() < vol_rows[i].find_until;
    const double mx = i >= 0 ? vol_ceiling(&vol_rows[i], -1) : VOL_MAX_PROV;
    snprintf(on_out, sizeof on_out, "%s", vol_out_now[0] ? vol_out_now : "default");
    for (int k = 0; k < VOL_OUTS; k++) {
        if (!vol_rows[k].set) continue;
        snprintf(ro, sizeof ro, "%s", vol_rows[k].out[0] ? vol_rows[k].out : "default");
        for (char *p = ro; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
        const int w = snprintf(rows + o, sizeof rows - o, "%s{\"out\":\"%s\",\"place\":\"%s\",\"on\":%s,\"db\":%.2f,\"mute\":%s,\"max\":%.2f,\"maxSet\":%s,\"auto\":%s}", o > 1 ? "," : "", ro,
                               vol_rows[k].place, vol_rows[k].on ? "true" : "false", vol_rows[k].db, vol_rows[k].mute ? "true" : "false",
                               vol_ceiling(&vol_rows[k], -1), vol_rows[k].maxset ? "true" : "false", vol_rows[k].aut ? "true" : "false");
        if (w < 0 || (size_t)w >= sizeof rows - o - 2) break;
        o += (size_t)w;
    }
    pthread_mutex_unlock(&vol_mu);
    snprintf(rows + o, sizeof rows - o, "]");
    for (char *p = on_out; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
    const int m = snprintf(b + n - 1, bn - (size_t)n + 1,
        ",\"volume\":{\"can\":true,\"out\":\"%s\",\"on\":%s,\"db\":%.2f,\"mute\":%s,\"nowDb\":%.2f,\"active\":%s,\"by\":\"%s\",\"spotify\":%s,"
        "\"max\":%.2f,\"maxSet\":%s,\"finding\":%s,\"default\":\"%s\",\"startDb\":%.0f,\"provDb\":%.0f,\"rows\":%s}}",
        on_out, on ? "true" : "false", lv, mu ? "true" : "false", act ? ndb : 0.0, act ? "true" : "false", vol_src_name(by),
        vol_spot ? "true" : "false", mx, mset ? "true" : "false", finding ? "true" : "false", vol_default_safe ? "safe" : "fixed", VOL_ON_DB, VOL_MAX_PROV, rows);
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
    const int i = vol_find(vol_out_now, vol_place_now);
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
    const int i = vol_find(vol_out_now, vol_place_now);
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
                     && fv != -1 && xv != -1 && dv2 != -1 && (xv == -2 || km || xv == 0) && (e || mq || kd || ks || km || fv >= 0 || xv >= 0 || dv2 >= 0);
    const char *why = good ? NULL : "bad";
    char rid[80] = "";
    if (good) vol_place_update();
    if (good && !e) out_room_of(to, rid, sizeof rid);   /* a level with no row of its own: the one of its room */
    if (good && dv2 >= 0) {                            /* the default of outputs with no row of their own */
        pthread_mutex_lock(&vol_mu);
        if (vol_default_safe != dv2) { vol_default_safe = dv2; vol_gen++; vol_by = VS_APP; }
        pthread_mutex_unlock(&vol_mu);
        fprintf(stderr, "page: the volume of outputs not answered yet: %s\n", dv2 ? "safe (on at -70 dB, at most -30 dB)" : "fixed (as before)");
    }
    if (good && (e || mq || kd || ks || km || fv >= 0 || xv >= 0)) {
        pthread_mutex_lock(&vol_mu);
        int own = -1;
        for (int k = 0; k < VOL_OUTS; k++) if (vol_rows[k].set && out_same(to, vol_rows[k].out)) own = k;
        const int via = own < 0 && !e ? vol_find(to, rid) : -1;
        vol_row *r = via >= 0 ? &vol_rows[via] : vol_put(to);
        if (e) {
            const int on = e[3] == '1';
            if (on && !r->on) { r->db = vol_on_level(r->db, vol_ceiling(r, -1)); r->mute = 0; }   /* turned on: never at full */
            if (on != r->on || r->aut) { r->on = on; r->aut = 0; vol_gen++; vol_by = VS_APP; }   /* answered */
            if (!on) r->find_until = 0;
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
        pthread_mutex_unlock(&vol_mu);
        if (!why) save_conf();
    }
    if (why) { char bad[48]; const int k = snprintf(bad, sizeof bad, "{\"ok\":false,\"why\":\"%s\"}", why); reply(c, "application/json", bad, (size_t)k); return; }
    char b[VOL_OUTS * 300 + 400] = "{\"ok\":true}";
    int n2 = vol_json(b, sizeof b, (int)strlen(b));
    reply(c, "application/json", b, (size_t)n2);
}
/* page.conf: its rows (save_conf, under nothing), a line of it (load_conf) */
static void vol_conf_save(FILE *f) {
    pthread_mutex_lock(&vol_mu);
    if (vol_default_safe) fprintf(f, "volume_default=safe\n");   /* */
    for (int i = 0; i < VOL_OUTS; i++) {               /* <on: 2 the safe default's>,<mute>,<dB>,<max: nan until set>,<maxset>,<out> */
        if (!vol_rows[i].set) continue;
        char mx[16];
        if (vol_rows[i].maxset) snprintf(mx, sizeof mx, "%.2f", vol_max_clamp(vol_rows[i].max)); else snprintf(mx, sizeof mx, "nan");
        fprintf(f, "volume=%d,%d,%.2f,%s,%d,%s\n", vol_rows[i].on ? (vol_rows[i].aut ? 2 : 1) : 0, vol_rows[i].mute, vol_rows[i].db, mx, vol_rows[i].maxset, vol_rows[i].out);
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
    if (strncmp(line, "volume=", 7)) return 0;
    int on = 0, mu = 0, at = 0, ms = 0, at2 = 0;       /* <on>,<mute>,<dB>,<max>,<maxset>,<out>; before: <on>,<mute>,<dB>,<out> */
    double v = NAN, mx = NAN;
    char mt[16] = "";
    const int n6 = sscanf(line + 7, "%d,%d,%lf,%15[^,],%d,%n", &on, &mu, &v, mt, &ms, &at2);
    if (n6 == 5 && at2 > 0 && (!strcmp(mt, "nan") || sscanf(mt, "%lf", &mx) == 1)) at = at2;
    else if (sscanf(line + 7, "%d,%d,%lf,%n", &on, &mu, &v, &at) != 3) at = 0;
    if (at > 0 && out_ok(line + 7 + at)) {
        pthread_mutex_lock(&vol_mu);
        vol_row *r = vol_put(line + 7 + at);
        r->on = on == 1 || on == 2; r->aut = on == 2; r->mute = mu == 1; r->db = vol_clamp(v);
        r->maxset = at == at2 && ms == 1 && mx == mx; r->max = r->maxset ? vol_max_clamp(mx) : NAN;
        if (r->db > vol_ceiling(r, -1)) r->db = vol_ceiling(r, -1);
        vol_gen_saved = vol_gen_told = vol_gen;
        pthread_mutex_unlock(&vol_mu);
    }
    return 1;
}
#endif
