/* src/player/page.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef _DEFAULT_SOURCE   /* build.sh passes -D_DEFAULT_SOURCE */
#define _DEFAULT_SOURCE
#endif
#include <arpa/inet.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include "player.h"
#include "page.h"
#include "squeeze.h"
#include "version.h"
#include "netout.h"
#include "out.h"
#include "alsa_hw.h"
#include "remote_in.h"   /* play, pause, next, previous for AirPlay and Spotify Connect: POST /remote */
#define hypot(a, b) sqrt((a) * (a) + (b) * (b))   /* hypot is GLIBC_2.35 ; the phasor is near 1: nothing overflows */

volatile int page_image_pct = 100;
volatile int page_slim_port = 0;

#define IMAGE_MAX 300
volatile int page_limiter_on = 1;
volatile double page_in_peak = 0;
volatile int page_lat_ring = 0, page_lat_rt = 0, page_lat_link = 0;
static int lat_bound(long v, long lo, long hi) { return v <= 0 ? 0 : v < lo ? (int)lo : v > hi ? (int)hi : (int)v; }

static int out_same(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    return !strcmp(!strcmp(a, "default") ? "" : a, !strcmp(b, "default") ? "" : b);
}
#include "vol_page.h"
static char rooms_dir[512], out_label[128], level_mode[16] = "auto";
int page_level_manual(void) { return !strcmp(level_mode, "manual"); }
/* The app's address : http(s)://, a host, a port, a path, of safe characters only (nothing to escape in the JSON
 * nor in the page's link), 200 characters at most; "" until the app tells it. */
static char app_url[201] = "";
static int app_url_ok(const char *v) {
    if (strlen(v) > 200) return 0;
    const char *p = !strncmp(v, "https://", 8) ? v + 8 : !strncmp(v, "http://", 7) ? v + 7 : NULL;
    if (!p) return 0;
    const char *h = p;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-') p++;
    if (p == h || *h == '.' || *h == '-') return 0;
    if (*p == ':') {
        long port = 0;
        const char *d = ++p;
        while (*p >= '0' && *p <= '9' && p - d < 6) port = port * 10 + (*p++ - '0');
        if (p == d || p - d > 5 || port < 1 || port > 65535) return 0;
    }
    if (*p && *p != '/') return 0;
    for (; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("/._~-", *p))) return 0;
    return 1;
}
static volatile double auto_db = -13, max_gain_db = 13;
static volatile int auto_ready = 0;

#define NSOUNDS 5
static const char *SOUND_DIR[NSOUNDS] = { "neutral", "balanced", "warm", "analogue", "custom" };
#ifdef ROOM_EN
static const char *SOUND_NAME[NSOUNDS] = { "Neutral", "Balanced", "Warm", "Analogue", "Custom" };
#define NO_CORRECTION "No correction"
#define THE_DAC "the DAC"
#define IN_MAC "All the Mac’s apps"
#define ROOM_EARLIER "Room installed earlier"
#define NO_LICENCE "no licence here: its room's filter not played"
#else
static const char *SOUND_NAME[NSOUNDS] = { "Neutre", "Équilibré", "Chaleureux", "Analogique", "Personnalisé" };
#define NO_CORRECTION "Sans correction"
#define THE_DAC "le DAC"
#define IN_MAC "Toutes les apps du Mac"
#define ROOM_EARLIER "Pièce installée avant"
#define NO_LICENCE "pas de licence ici : le filtre de la pièce n’est pas joué"
#endif
static int control_get(const char *key, char *out, size_t len);

static int place_ok(const char *v) {
    if (!*v || strlen(v) > 64 || !strcmp(v, "-") || !strcmp(v, "_")) return 0;
    for (const char *p = v; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    return 1;
}
static int is_dir(const char *d) { struct stat st; return stat(d, &st) == 0 && S_ISDIR(st.st_mode); }
static int sounds_in(const char *base) {
    char d[1500];
    for (int k = 0; k < NSOUNDS; k++) { snprintf(d, sizeof d, "%s/%s", base, SOUND_DIR[k]); if (is_dir(d)) return 1; }
    return 0;
}
/* A room's name as the app gave it (places/<id>/name), quotes and controls made spaces; "" for none. */
static void place_name(const char *rooms, const char *id, char *name, size_t nn) {
    char p[1024], line[128] = "";
    if (!name || !nn) return;
    name[0] = 0;
    if (!strcmp(id, "_")) { snprintf(name, nn, "%s", ROOM_EARLIER); return; }
    snprintf(p, sizeof p, "%s/places/%s/name", rooms, id);
    FILE *f = fopen(p, "r");
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    line[strcspn(line, "\r\n")] = 0;
    for (char *q = line; *q; q++) if (*q == '"' || *q == '\\' || (unsigned char)*q < 0x20) *q = ' ';
    snprintf(name, nn, "%s", line[0] ? line : id);
}

void page_sound_label(const char *key, char *sound, size_t sl, char *room, size_t rl) {
    snprintf(sound, sl, "%s", NO_CORRECTION);
    if (room && rl) room[0] = 0;

    if (key && !strcmp(key, "-L")) { if (room && rl) snprintf(room, rl, "%s", NO_LICENCE); return; }
    const char *slash = key ? strrchr(key, '/') : NULL;
    if (!key || !strcmp(key, "-") || !slash) return;
    char id[128];
    snprintf(id, sizeof id, "%.*s", (int)(slash - key), key);
    for (int k = 0; k < NSOUNDS; k++) if (!strcmp(slash + 1, SOUND_DIR[k])) snprintf(sound, sl, "%s", SOUND_NAME[k]);
    if (room && rl && rooms_dir[0] && strcmp(id, "_")) place_name(rooms_dir, id, room, rl);
}
int page_outroom(const char *rooms, const char *out, char *id, size_t idn, char *base, size_t bn, char *name, size_t nn) {
    char p[1024], line[600], place[80] = "", v[80] = "";
    const char *key = out && *out ? out : "default";
    const size_t kl = strlen(key);
    int any = 0, have = 0;
    snprintf(p, sizeof p, "%s/control", rooms);
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "place=", 6)) snprintf(place, sizeof place, "%.64s", line + 6);
        else if (!strncmp(line, "outroom.", 8)) {
            char *eq = strrchr(line + 8, '=');
            if (!eq) continue;
            any = 1;
            if ((size_t)(eq - (line + 8)) == kl && !memcmp(line + 8, key, kl)) { snprintf(v, sizeof v, "%.64s", eq + 1); have = 1; }
        }
    }
    if (f) fclose(f);
    if (!place_ok(place)) place[0] = 0;
    if (!any) snprintf(v, sizeof v, "%s", place[0] ? place : "_");
    else if (!have || (strcmp(v, "_") && !place_ok(v))) snprintf(v, sizeof v, "-");
    char d[1024];
    if (!strcmp(v, "_") || (place[0] && !strcmp(v, place) && (snprintf(d, sizeof d, "%s/places/%s", rooms, v), !is_dir(d))))
        snprintf(d, sizeof d, "%s/rooms", rooms);
    else if (strcmp(v, "-")) { snprintf(d, sizeof d, "%s/places/%s", rooms, v); if (!is_dir(d)) snprintf(v, sizeof v, "-"); }
    if (id && idn) snprintf(id, idn, "%s", v);
    if (!strcmp(v, "-")) { if (base && bn) base[0] = 0; if (name && nn) name[0] = 0; return 0; }
    if (base && bn) snprintf(base, bn, "%s", d);
    place_name(rooms, v, name, nn);
    return 1;
}
/* the room output out plays in, a real one only (places/<id>), else "" */
static void out_room_of(const char *out, char *id, size_t n) {
    const char *o = out && strcmp(out, "default") ? out : "";
    if (!page_outroom(rooms_dir, o, id, n, NULL, 0, NULL, 0) || !place_ok(id)) id[0] = 0;
}

static void vol_dac_of(const char *out, char *k, size_t n) {
    char v[160] = "";
    const char *o = out && strcmp(out, "default") ? out : "";
    if (!o[0]) { if (strcmp(out_label, THE_DAC)) snprintf(v, sizeof v, "%s", out_label); }
    else if (!strncmp(o, "audiolaz:", 9)) {
        netout_out_dac(o, v, sizeof v);
        char nm[128] = "";
        if (!v[0]) { netout_out_name(o, nm, sizeof nm); if (nm[0]) snprintf(v, sizeof v, "link:%.120s", nm); }
    }
    else if (!strncmp(o, OUT_CARD_PREFIX, OUT_CARD_PREFIX_LEN) && o[OUT_CARD_PREFIX_LEN]) snprintf(v, sizeof v, "%s", o + OUT_CARD_PREFIX_LEN);
    const char *a = strstr(v, "\xe2\x86\x92"), *s = a ? a + 3 : v;
    while (*s == ' ') s++;
    size_t l = strcspn(s, "(");
    while (l && s[l - 1] == ' ') l--;
    snprintf(k, n, "%.*s", (int)l, s);
    for (char *p = k; *p; p++) *p = (unsigned char)*p < 0x20 ? ' ' : *p >= 'A' && *p <= 'Z' ? (char)(*p - 'A' + 'a') : *p;
}

static void rooms_json(char *o, size_t len) {
    char d[1024], place[80] = "", nm[100];
    size_t w = (size_t)snprintf(o, len, "[");
    control_get("place", place, sizeof place);
    snprintf(d, sizeof d, "%s/places/%s", rooms_dir, place);
    int named = place_ok(place) && is_dir(d);
    snprintf(d, sizeof d, "%s/rooms", rooms_dir);
    if (!named && sounds_in(d)) { place_name(rooms_dir, "_", nm, sizeof nm); w += (size_t)snprintf(o + w, w < len ? len - w : 0, "{\"id\":\"_\",\"name\":\"%s\"}", nm); }
    snprintf(d, sizeof d, "%s/places", rooms_dir);
    DIR *dir = opendir(d);
    struct dirent *e;
    int n = 0;
    while (dir && (e = readdir(dir)) && n < 32) {
        char pd[1400];
        snprintf(pd, sizeof pd, "%s/%s", d, e->d_name);
        if (!place_ok(e->d_name) || !is_dir(pd) || !sounds_in(pd)) continue;
        place_name(rooms_dir, e->d_name, nm, sizeof nm);
        if (w + 200 >= len) break;
        w += (size_t)snprintf(o + w, len - w, "%s{\"id\":\"%s\",\"name\":\"%s\"}", w > 1 ? "," : "", e->d_name, nm);
        n++;
    }
    if (dir) closedir(dir);
    if (w + 2 < len) snprintf(o + w, len - w, "]"); else snprintf(o, len, "[]");
}
/* The control's outroom.<out>=<id> lines, as JSON: {"<out, or default>":"<id or ->"}. */
static void outrooms_json(char *o, size_t len) {
    char p[1024], line[600];
    size_t w = (size_t)snprintf(o, len, "{");
    snprintf(p, sizeof p, "%s/control", rooms_dir);
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strncmp(line, "outroom.", 8) ? NULL : strrchr(line + 8, '=');
        if (!eq) continue;
        *eq = 0;
        for (char *q = line; *q; q++) if (*q == '"' || *q == '\\' || (unsigned char)*q < 0x20) *q = ' ';
        for (char *q = eq + 1; *q; q++) if (*q == '"' || *q == '\\' || (unsigned char)*q < 0x20) *q = ' ';
        if (w + strlen(line) + strlen(eq + 1) + 8 >= len) break;
        w += (size_t)snprintf(o + w, len - w, "%s\"%s\":\"%s\"", w > 1 ? "," : "", line + 8, eq + 1);
    }
    if (f) fclose(f);
    snprintf(o + w, len - w, "}");
}
static int have_sound_in(const char *base, int k) {
    char p[1200];
    if (!base[0]) return 0;
    snprintf(p, sizeof p, "%s/%s", base, SOUND_DIR[k]);
    return is_dir(p);
}

/* ── The control file: read a key, set some (a new file renamed in place, as the app does) ──────────── */
/* (11/10, the performance pass) /state reads some sixty keys of the control and the status: each read opened and read
 * its file whole again. While it is made, both are read once (snap_begin) and the keys taken from memory, the same
 * way (the last line of a key wins). Per thread: the page's other readers go on reading the files. */
static _Thread_local char *snap_ctl, *snap_st;
static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 16384, n = 0;
    char *b = malloc(cap);
    while (b) {
        const size_t k = fread(b + n, 1, cap - n - 1, f);
        n += k;
        if (k == 0 || n + 1 < cap) break;
        if (cap >= (1 << 20)) break;                   /* 1 MB: more is not a control file */
        char *nb = realloc(b, cap * 2);
        if (!nb) { free(b); b = NULL; break; }
        b = nb; cap *= 2;
    }
    fclose(f);
    if (b) b[n] = 0;
    return b;
}
/* key=value from a file's text in memory, the last such line; 1 when found */
static int snap_get(const char *text, const char *key, char *out, size_t len) {
    const size_t k = strlen(key);
    int found = 0;
    for (const char *l = text; l && *l; ) {
        const char *e = l + strcspn(l, "\r\n");
        if ((size_t)(e - l) > k && !strncmp(l, key, k) && l[k] == '=') { snprintf(out, len, "%.*s", (int)(e - l - k - 1), l + k + 1); found = 1; }
        l = *e ? e + 1 : e;
    }
    return found;
}
static void snap_begin(void) {
    char p[1024];
    snprintf(p, sizeof p, "%s/control", rooms_dir); snap_ctl = slurp(p);
    snprintf(p, sizeof p, "%s/status", rooms_dir); snap_st = slurp(p);
}
static void snap_end(void) { free(snap_ctl); free(snap_st); snap_ctl = snap_st = NULL; }
static int control_get(const char *key, char *out, size_t len) {
    if (snap_ctl) return snap_get(snap_ctl, key, out, len);
    char p[1024], line[600];
    snprintf(p, sizeof p, "%s/control", rooms_dir);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    size_t k = strlen(key);
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, key, k) && line[k] == '=') { snprintf(out, len, "%s", line + k + 1); found = 1; }
    }
    fclose(f);
    return found;
}
/* The settings by context kept at most (roomd.c reads up to 64, every output's): a new one refused beyond it. */
#define CTX_KEEP 48

#define SYNC_APPLY_MIN_MS 15.0

static pthread_mutex_t control_mu = PTHREAD_MUTEX_INITIALIZER;
static void control_set_(const char *const *keys, const char *const *vals, int n);
static void control_set(const char *const *keys, const char *const *vals, int n) {
    pthread_mutex_lock(&control_mu);
    control_set_(keys, vals, n);
    pthread_mutex_unlock(&control_mu);
}
static void control_set_(const char *const *keys, const char *const *vals, int n) {
    char p[1024], real[PATH_MAX], tmp[PATH_MAX + 16], line[600];
    snprintf(p, sizeof p, "%s/control", rooms_dir);
    if (!realpath(p, real)) snprintf(real, sizeof real, "%s", p);   /* a link (an engine on trial): the file it names */
    snprintf(tmp, sizeof tmp, "%s.page", real);
    FILE *in = fopen(real, "r"), *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return; }
    int done[8] = { 0 };
    while (in && fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = 0;
        int set = 0;
        /* a NULL value: the key's line taken off */
        for (int i = 0; i < n; i++) { size_t k = strlen(keys[i]); if (!strncmp(line, keys[i], k) && line[k] == '=') { if (vals[i] && !done[i]) fprintf(out, "%s=%s\n", keys[i], vals[i]); done[i] = set = 1; } }
        if (!set && line[0]) fprintf(out, "%s\n", line);
    }
    for (int i = 0; i < n; i++) if (!done[i] && vals[i]) fprintf(out, "%s=%s\n", keys[i], vals[i]);
    if (in) fclose(in);
    fclose(out);
    rename(tmp, real);
}
static void status_get(const char *key, char *out, size_t len) {
    char p[1024], line[600];
    out[0] = 0;
    if (snap_st) { snap_get(snap_st, key, out, len); return; }
    snprintf(p, sizeof p, "%s/status", rooms_dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    size_t k = strlen(key);
    while (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; if (!strncmp(line, key, k) && line[k] == '=') snprintf(out, len, "%s", line + k + 1); }
    fclose(f);
}

static int link_json(char *b, size_t bn, int n, const char *out) {
    if (n < 1 || b[n - 1] != '}') return n;
    char lj[1200], add[1300];
    netout_link_json(out, lj, sizeof lj);
    /* outBusy: this output's audiolaz receiver refused us lately, another player playing there (its name:
     * outUsedBy, outUsedPlaying); POST /take plays here instead */
    const int busy = out && *out && netout_busy(out, NULL, 0);
    const int m = snprintf(add, sizeof add, ",\"outBusy\":%s,\"link\":%s}", busy ? "true" : "false", lj);
    if (m <= 0 || (size_t)m >= sizeof add || (size_t)(n - 1 + m) >= bn) return n;   /* no room: /state as it was */
    memcpy(b + n - 1, add, (size_t)m + 1);
    return n - 1 + m;
}

static int channels_json(char *b, size_t bn, int n) {
    if (n < 1 || b[n - 1] != '}') return n;
    char ch[8], lay[24], dc[8], set[16] = "", fil[16];
    status_get("input_channels", ch, sizeof ch); status_get("input_layout", lay, sizeof lay);
    status_get("dialogue_centre", dc, sizeof dc); status_get("airplay_channels_file", fil, sizeof fil);
    control_get("airplay_channels", set, sizeof set);
    if (strcmp(lay, "stereo") && strcmp(lay, "mono") && strcmp(lay, "5.1") && strcmp(lay, "7.1")) snprintf(lay, sizeof lay, "%s", atoi(ch) > 0 ? "other" : "");
    const int m = snprintf(b + n - 1, bn - (size_t)n + 1,
        ",\"inputChannels\":%d,\"inputLayout\":\"%s\",\"dialogueCentre\":%s,\"airplayChannels\":\"%s\",\"airplayChannelsFile\":%s}",
        atoi(ch), lay, atoi(dc) ? "true" : "false", strcmp(set, "multi") ? "stereo" : "multi",   /* stereo when absent */
        !strcmp(fil, "multi") ? "\"multi\"" : !strcmp(fil, "stereo") ? "\"stereo\"" : "null");
    if (m > 0 && (size_t)m < bn - (size_t)n + 1) return n - 1 + m;
    b[n - 1] = '}'; b[n] = 0;
    return n;
}
/* The speakers' channels (roomd.c, channels.h), added to /state: outChannels { swap (the music's left and right
 * swapped, before the correction: each speaker keeps its own), invertL, invertR (the polarity of the speaker on the
 * output's left channel, or right: per output channel, after everything), as the control keeps
 * them for out (swap.<out>=1, invert_l.<out>=1, invert_r.<out>=1; <out> "default" for the command line's; absent: off),
 * now (what the output plays with, the player's status: CH_* bits, swap 1, left 2, right 4) }. */
static void spk_keys(const char *out, char k[3][200]) {
    snprintf(k[0], sizeof k[0], "swap.%s", out[0] ? out : "default");
    snprintf(k[1], sizeof k[1], "invert_l.%s", out[0] ? out : "default");
    snprintf(k[2], sizeof k[2], "invert_r.%s", out[0] ? out : "default");
}
/* (23/10, the release) The polarity's meaning, kept for those who had set it: until 1.2.8 each inversion acted on the
 * music's channel before the swap (invert_l: the speaker that plays the music's left, wherever the swap sent it), since
 * 1.2.9 on the speaker of the output's left or right channel, after the swap moved to the music before the correction.
 * With swap.<out>=1 that is the other side: invert_l.<out> and invert_r.<out> exchanged once, so every speaker keeps
 * its polarity and an update changes nothing heard (swap off: the same side, nothing to do). Done once per folder,
 * before the player reads its control (roomd.c main), marked channels_order=2; every POST /channels writes the mark
 * too (a control made after the update never exchanged). → how many outputs were exchanged, −1 none read. */
#define CHAN_ORDER_KEY "channels_order"
int page_channels_migrate(const char *rooms) {
    if (!rooms_dir[0]) snprintf(rooms_dir, sizeof rooms_dir, "%s", rooms);
    char v[16] = "";
    if (control_get(CHAN_ORDER_KEY, v, sizeof v) && atoi(v) >= 2) return 0;
    char p[1024], line[600];
    snprintf(p, sizeof p, "%s/control", rooms_dir);
    static char outs[64][200];
    int n = 0;
    FILE *f = fopen(p, "r");
    if (!f) return -1;   /* no control yet: nothing set, marked by the first POST /channels */
    while (fgets(line, sizeof line, f) && n < 64) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "swap.", 5)) continue;
        char *eq = strrchr(line + 5, '=');
        if (!eq || eq == line + 5 || atoi(eq + 1) != 1 || eq - (line + 5) >= (long)sizeof outs[0]) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) if ((size_t)(eq - (line + 5)) == strlen(outs[i]) && !strncmp(outs[i], line + 5, (size_t)(eq - (line + 5)))) dup = 1;
        if (!dup) { snprintf(outs[n], sizeof outs[n], "%.*s", (int)(eq - (line + 5)), line + 5); n++; }
    }
    fclose(f);
    int moved = 0;
    for (int i = 0; i < n; i++) {
        char kl[220], kr[220], vl[8] = "", vr[8] = "";
        snprintf(kl, sizeof kl, "invert_l.%.199s", outs[i]);
        snprintf(kr, sizeof kr, "invert_r.%.199s", outs[i]);
        const int l = control_get(kl, vl, sizeof vl) && atoi(vl) == 1, r = control_get(kr, vr, sizeof vr) && atoi(vr) == 1;
        if (l == r) continue;   /* both or neither: the same either way */
        const char *ks[2] = { kl, kr }, *vs[2] = { r ? "1" : NULL, l ? "1" : NULL };
        control_set(ks, vs, 2);
        moved++;
    }
    const char *mk[1] = { CHAN_ORDER_KEY }, *mv[1] = { "2" };
    control_set(mk, mv, 1);
    return moved;
}
static int spk_get(const char *out) {
    char k[3][200], v[8];
    spk_keys(out, k);
    int b = 0;
    for (int i = 0; i < 3; i++) if (control_get(k[i], v, sizeof v) && atoi(v) == 1) b |= 1 << i;
    return b;
}
static int spk_obj(char *b, size_t bn, int ch, int now) {
    return snprintf(b, bn, "{\"swap\":%s,\"invertL\":%s,\"invertR\":%s,\"now\":%d}", ch & 1 ? "true" : "false", ch & 2 ? "true" : "false",
                    ch & 4 ? "true" : "false", now);
}
static int spk_json(char *b, size_t bn, int n) {
    if (n < 1 || b[n - 1] != '}') return n;
    char out[180] = "", nw[8] = "", o[120];
    control_get("out", out, sizeof out);
    status_get("channels_now", nw, sizeof nw);
    spk_obj(o, sizeof o, spk_get(out), atoi(nw) & 7);
    const int m = snprintf(b + n - 1, bn - (size_t)n + 1, ",\"outChannels\":%s}", o);
    if (m > 0 && (size_t)m < bn - (size_t)n + 1) return n - 1 + m;
    b[n - 1] = '}'; b[n] = 0;
    return n;
}

static int upnp_json(char *b, size_t bn, int n) {
    if (n < 1 || b[n - 1] != '}') return n;
    char on[8] = "", vol[16] = "", stt[32] = "", fmt[96] = "", port[16] = "";
    status_get("upnp", on, sizeof on);
    int m;
    if (!on[0]) m = snprintf(b + n - 1, bn - (size_t)n + 1, ",\"upnp\":null}");
    else {
        status_get("upnp_volume", vol, sizeof vol); status_get("upnp_state", stt, sizeof stt); status_get("upnp_format", fmt, sizeof fmt);
        status_get("upnp_port", port, sizeof port);
        for (char *p = stt; *p; p++) if (!((*p >= 'A' && *p <= 'Z') || *p == '_')) *p = '_';
        for (char *p = fmt; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
        m = snprintf(b + n - 1, bn - (size_t)n + 1, ",\"upnp\":{\"on\":%s,\"volume\":\"%s\",\"state\":\"%s\",\"format\":\"%s\",\"port\":%d}}",
                     strcmp(on, "on") ? "false" : "true", strcmp(vol, "digital") ? "fixed" : "digital", stt, fmt, atoi(port));
    }
    if (m > 0 && (size_t)m < bn - (size_t)n + 1) return n - 1 + m;
    b[n - 1] = '}'; b[n] = 0;
    return n;
}
static void json_clean(char *s) { for (char *p = s; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' '; }
static void load_conf(void);
static void clean_json(char *dst, size_t len, const char *src);
/* a voie (roomd.c --voie): no page of its own, the player's page.conf followed (its settings, its places per
 * output), read again whenever it changes */
void page_conf_follow(const char *rooms) {
    static struct timespec seen;
    static int have = 0;
    if (!rooms_dir[0]) snprintf(rooms_dir, sizeof rooms_dir, "%s", rooms);
    char p[700];
    snprintf(p, sizeof p, "%s/page.conf", rooms_dir);
    struct stat st;
    if (stat(p, &st)) return;
#ifdef __APPLE__
    const struct timespec t = st.st_mtimespec;
#else
    const struct timespec t = st.st_mtim;
#endif
    if (have && t.tv_sec == seen.tv_sec && t.tv_nsec == seen.tv_nsec) return;
    /* a file written this very moment (the player's page rewrites it whole, save_conf; its time the same to the
     * kernel's tick before and after): read once it has been left alone for 50 ms */
    struct timespec nw;
    clock_gettime(CLOCK_REALTIME, &nw);
    if ((double)(nw.tv_sec - t.tv_sec) + (nw.tv_nsec - t.tv_nsec) / 1e9 < 0.05) return;
    seen = t; have = 1;
    load_conf();
}

static void input_get(char *out, size_t len) {
    char v[64], p[1024], line[128];
    status_get("input", v, sizeof v);
    out[0] = 0;
    if (!strncmp(v, "loop", 4) && v[4] >= '0' && v[4] <= '9') {
        const int k = atoi(v + 4);
        snprintf(p, sizeof p, "%s/inputs.conf", rooms_dir);
        FILE *f = fopen(p, "r");
        if (f) {
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                char *eq = strchr(line, '=');
                if (line[0] >= '0' && line[0] <= '9' && eq && atoi(line) == k) snprintf(out, len, "%s", eq + 1);
            }
            fclose(f);
        }
        if (!out[0]) snprintf(out, len, "%s", v);
    } else if (!strcmp(v, "squeeze")) snprintf(out, len, "squeeze");
    for (char *q = out; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '-')) *q = '-';
}
/* An AirPlay input among the player's : inputs.conf names one "airplay". */
static int airplay_input(void) {
    char p[1024], line[128];
    snprintf(p, sizeof p, "%s/inputs.conf", rooms_dir);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int k = 0;
    while (!k && fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; const char *eq = strchr(line, '='); k = line[0] >= '0' && line[0] <= '9' && eq && !strcmp(eq + 1, "airplay"); }
    fclose(f);
    return k;
}
static void save_conf(void) {
    char p[1024];
    snprintf(p, sizeof p, "%s/page.conf", rooms_dir);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "mode=%s\nimage=%d\nlimiter=%d\n", level_mode, page_image_pct, page_limiter_on);
    if (app_url[0]) fprintf(f, "app=%s\n", app_url);
    if (page_lat_ring || page_lat_rt || page_lat_link) fprintf(f, "lat_ring=%d\nlat_rt=%d\nlat_link=%d\n", page_lat_ring, page_lat_rt, page_lat_link);
    vol_conf_save(f);
    fclose(f);
}
static int vol_migrated = -1;                          /* vol_migrate's at the last read */
static void load_conf(void) {
    char p[1024], line[1024];   /* 256 before: the bells' lines are longer */
    snprintf(p, sizeof p, "%s/page.conf", rooms_dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    vol_conf_begin();                                  /* volume_paths= as this file says it */
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "mode=", 5)) snprintf(level_mode, sizeof level_mode, "%s", strcmp(line + 5, "manual") ? "auto" : "manual");
        else if (!strncmp(line, "image=", 6)) { int v = atoi(line + 6); page_image_pct = v < 0 ? 0 : v > IMAGE_MAX ? IMAGE_MAX : v; }
        else if (!strncmp(line, "limiter=", 8)) page_limiter_on = atoi(line + 8) != 0;
        else if (!strncmp(line, "lat_ring=", 9)) page_lat_ring = lat_bound(atol(line + 9), 15, 800);
        else if (!strncmp(line, "lat_rt=", 7)) page_lat_rt = lat_bound(atol(line + 7), 1, 89);
        else if (!strncmp(line, "lat_link=", 9)) page_lat_link = lat_bound(atol(line + 9), 10, 150);
        else if (!strncmp(line, "app=", 4) && app_url_ok(line + 4)) snprintf(app_url, sizeof app_url, "%s", line + 4);
        else if (vol_conf_line(line)) { }
    }
    fclose(f);
    vol_migrated = vol_migrate();                          /* a page.conf of before: its rooms' answers per output */
}

static void sounds_check(void) { }
/* ── HTTP ─────────────────────────────────────────────────────────────────────────────────────────── */

#define CURVE_CSS \
".cv{background:var(--tile);border-radius:var(--r);padding:16px 15px 12px}\n" \
".cv .hd{display:flex;align-items:flex-end;justify-content:space-between;gap:16px}.cv .lab{color:var(--muted)}\n" \
".cv .big{font-size:clamp(28px,8.5vw,40px)}\n" \
"#cv{margin:12px 0 0;min-height:170px}#cv svg{display:block;width:100%;height:auto;overflow:visible}\n" \
"#cv .g{stroke:var(--grid);stroke-width:1}#cv .z{stroke:var(--ink);stroke-width:1;stroke-dasharray:4 4;opacity:.5}\n" \
"#cv text{fill:var(--muted);font:600 10.5px Archivo,system-ui,-apple-system,sans-serif}\n" \
"#cv .c{fill:none;stroke:var(--orange);stroke-width:2.5;stroke-linejoin:round;stroke-linecap:round}\n" \
".cvl{display:flex;flex-wrap:wrap;gap:4px 14px;font-size:12px;font-weight:600;color:var(--muted);margin-top:6px}.cvl span{display:inline-flex;align-items:center;gap:7px}\n" \
".cvl i{width:16px;border-top:2.5px solid var(--orange)}.cvl i.z{border-top:1.5px dashed var(--ink);opacity:.5}\n" \
".cv .ft{border-top:1px solid var(--grid);margin-top:10px;padding-top:10px;font-size:13px;font-weight:600;color:var(--muted)}\n"
#define CURVE_JS \
"let CV={k:'',t:0,b:0,d:null,T:null};\n" \
"function cvd(d,T){const box=$('cv');CV.d=d;CV.T=T;const W=Math.max(240,Math.round(box.clientWidth||320)),H=170,L=30,R=8,P=8,B=20,N=121,db=!d.none&&Array.isArray(d.db)?d.db:null;\n" \
"let lo=0,hi=0;if(db)for(const v of db){if(v>-60){lo=Math.min(lo,v);hi=Math.max(hi,v)}}lo=Math.floor((lo-2)/5)*5;hi=Math.ceil((hi+2)/5)*5;while(hi-lo<20){if(hi<-lo)hi+=5;else lo-=5}if(hi-lo>45)lo=hi-45;\n" \
"const x=f=>L+Math.log10(f/20)/3*(W-L-R),y=v=>P+(hi-Math.max(lo,Math.min(hi,v)))/(hi-lo)*(H-P-B),F=[20,50,100,200,500,1000,2000,5000,10000,20000],lb=W<460?[20,100,1000,10000]:F;let g='';\n" \
"for(const f of F){const p=x(f).toFixed(1);g+='<line class=\"g\" x1=\"'+p+'\" x2=\"'+p+'\" y1=\"'+P+'\" y2=\"'+(H-B)+'\"/>';if(lb.includes(f))g+='<text x=\"'+p+'\" y=\"'+(H-5)+'\" text-anchor=\"'+(f===20?'start':f===20000?'end':'middle')+'\">'+(f>=1000?f/1000+'kHz':f+'Hz')+'</text>'}\n" \
"const st=hi-lo>30?10:5;for(let v=Math.ceil(lo/st)*st;v<=hi;v+=st){const p=y(v).toFixed(1);g+='<line class=\"'+(v===0?'z':'g')+'\" x1=\"'+L+'\" x2=\"'+(W-R)+'\" y1=\"'+p+'\" y2=\"'+p+'\"/><text x=\"'+(L-6)+'\" y=\"'+p+'\" dy=\".33em\" text-anchor=\"end\">'+(v>0?'+':v<0?'−':'')+Math.abs(v)+'</text>'}\n" \
"const pts=db?db.map((v,i)=>x(20*Math.pow(1000,i/(N-1))).toFixed(1)+','+y(v).toFixed(1)).join(' '):x(20)+','+y(0)+' '+x(20000)+','+y(0);\n" \
"box.innerHTML='<svg viewBox=\"0 0 '+W+' '+H+'\" width=\"'+W+'\" height=\"'+H+'\">'+g+'<polyline class=\"c\" points=\"'+pts+'\"/></svg>';\n" \
"const e=s=>String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;');$('cvl').innerHTML=db?'<span><i></i>'+e(d.sound)+(d.roomName?T.room+e(d.roomName):'')+' · '+(d.rate/1000).toLocaleString(T.loc)+' kHz</span><span><i class=\"z\"></i>0 dB</span><span>'+T.how+'</span>':'<span><i></i>'+e(d.sound)+T.flat+'</span>'}\n" \
"function curve(i,k,T){const now=Date.now();k=i+'|'+k;if(CV.b&&now-CV.b<5000)return;if(k===CV.k&&now-CV.t<15000)return;CV.b=now;\n" \
"fetch('/curve'+(i>=0?'?s='+i:'')).then(r=>r.json()).then(d=>{CV.k=k;CV.t=Date.now();cvd(d,T)}).catch(()=>{}).finally(()=>{CV.b=0})}\n" \
"addEventListener('resize',()=>{if(CV.d)cvd(CV.d,CV.T)});\n"
#define PAGE_CSS \
":root{color-scheme:light;--paper:#eeede6;--ink:#18160f;--tile:#e3e2da;--tile2:#d8d7ce;--orange:#e9531a;--on:#f6f4ec;--muted:#7c7a70;--grid:#d3d2c9;--r:5px}\n" \
"@media (prefers-color-scheme:dark){:root{color-scheme:dark;--paper:#151410;--ink:#eeede6;--tile:#211f19;--tile2:#2b2922;--orange:#f2611f;--on:#151410;--muted:#89877c;--grid:#2e2c25}}\n" \
"*{box-sizing:border-box}[hidden]{display:none!important}\n" \
"body{margin:0;background:var(--paper);color:var(--ink);font:500 15px/1.45 Archivo,system-ui,-apple-system,'Segoe UI',Arial,sans-serif;-webkit-font-smoothing:antialiased;-webkit-text-size-adjust:100%}\n" \
"main{max-width:560px;margin:0 auto;padding:max(20px,env(safe-area-inset-top)) 16px calc(32px + env(safe-area-inset-bottom))}\n" \
".top{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:22px}\n" \
".logo{font-size:22px;font-weight:800;letter-spacing:-.03em;line-height:1;color:inherit;text-decoration:none}.logo .back{color:var(--muted);font-weight:700;margin-right:6px}\n" \
".status{font-size:13px;font-weight:600;color:var(--muted);display:inline-flex;align-items:center;gap:7px;white-space:nowrap}\n" \
".status i{width:7px;height:7px;border-radius:50%;background:var(--muted)}.status.on i{background:var(--orange)}\n" \
CURVE_CSS \
".lab,.label{font-size:11px;font-weight:700;letter-spacing:.14em;text-transform:uppercase}\n" \
".big{font-size:clamp(30px,9vw,46px);font-weight:800;letter-spacing:-.03em;line-height:.95;margin-top:8px;overflow-wrap:anywhere}\n" \
".kv{text-align:right;font-variant-numeric:tabular-nums;flex:0 0 auto}.kv b{display:block;font-size:22px;font-weight:800;line-height:1}\n" \
".kv small{display:block;font-size:11px;font-weight:700;letter-spacing:.12em;text-transform:uppercase;opacity:.6;margin-top:4px}\n" \
".label{color:var(--muted);margin:26px 0 10px}\n" \
".ab{display:grid;grid-template-columns:1fr 1fr;gap:10px}\n" \
".ab button{min-height:84px;padding:14px;border:0;border-radius:var(--r);background:var(--tile);color:var(--ink);font-family:inherit;font-size:17px;font-weight:800;letter-spacing:-.02em;line-height:1.05;text-align:left;display:flex;align-items:flex-end;cursor:pointer;transition:background .12s}\n" \
".ab button:hover{background:var(--tile2)}.ab button:active{transform:translateY(1px)}\n" \
".ab button.on,.ab button.on:hover{background:var(--orange);color:var(--on)}\n" \
".ab button.none{grid-column:1/-1;min-height:50px;align-items:center;justify-content:center;background:none;box-shadow:inset 0 0 0 2px var(--ink);font-size:13px;font-weight:700;letter-spacing:.04em;text-transform:uppercase}\n" \
".ab button.none.on{box-shadow:none}\n" \
".ab button.wait{animation:abw 1.1s ease-in-out infinite}@keyframes abw{50%{background:var(--tile2);box-shadow:inset 0 0 0 2px var(--orange)}}\n" \
"@media (prefers-reduced-motion:reduce){.ab button.wait{animation:none;box-shadow:inset 0 0 0 2px var(--orange)}}\n" \
".panel{background:var(--tile);border-radius:var(--r);padding:3px 15px}\n" \
".row{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:10px 0;min-height:46px;border-top:1px solid var(--grid);font-weight:600}\n" \
".panel>.row:first-child,.panel>[hidden]:first-child+.row,.row.note{border-top:0}\n" \
".row>span:last-child{text-align:right;font-weight:700;font-variant-numeric:tabular-nums;min-width:0}\n" \
".row.note{display:block;min-height:0;padding:0 0 10px;margin-top:-4px;color:var(--muted);font-size:13px}.row.note>span:last-child{text-align:left;font-weight:600}\n" \
"input[type=range]{-webkit-appearance:none;appearance:none;display:block;width:100%;background:transparent;height:28px;margin:0 0 8px}\n" \
"input[type=range]::-webkit-slider-runnable-track{height:2px;background:var(--ink);border-radius:1px}input[type=range]::-moz-range-track{height:2px;background:var(--ink)}\n" \
"input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:18px;height:18px;border-radius:50%;background:var(--orange);margin-top:-8px;border:0}\n" \
"input[type=range]::-moz-range-thumb{width:18px;height:18px;border-radius:50%;background:var(--orange);border:0}input[type=range]:disabled{opacity:.35}\n" \
"select{font-family:inherit;font-size:14px;font-weight:700;color:var(--ink);background-color:var(--paper);border:0;border-radius:var(--r);padding:9px 30px 9px 10px;min-height:38px;max-width:100%;-webkit-appearance:none;appearance:none;\n" \
"background-image:linear-gradient(45deg,transparent 50%,var(--muted) 50%),linear-gradient(135deg,var(--muted) 50%,transparent 50%);background-position:calc(100% - 16px) 50%,calc(100% - 11px) 50%;background-size:5px 5px;background-repeat:no-repeat}\n" \
"select:disabled{opacity:.4}\n" \
"label{display:inline-flex;align-items:center;gap:8px;cursor:pointer}\n" \
"input[type=checkbox]{-webkit-appearance:none;appearance:none;width:22px;height:22px;margin:0;border:2px solid var(--ink);border-radius:3px;background:transparent;cursor:pointer;display:inline-grid;place-content:center}\n" \
"input[type=checkbox]:checked{background:var(--orange);border-color:var(--orange)}\n" \
"input[type=checkbox]:checked::after{content:'';width:10px;height:6px;border:2.5px solid var(--on);border-top:0;border-right:0;transform:translateY(-1px) rotate(-45deg)}\n" \
"select:focus-visible,button:focus-visible,input:focus-visible{outline:2px solid var(--orange);outline-offset:2px}\n" \
".mrow{padding:10px 0 12px;border-top:1px solid var(--grid)}.mh{display:flex;justify-content:space-between;align-items:baseline;gap:12px;font-weight:600}\n" \
".mh>span:last-child{font-weight:700;font-variant-numeric:tabular-nums}.mh .k{font-size:11px;font-weight:700;letter-spacing:.14em;text-transform:uppercase;color:var(--muted)}\n" \
".meter{height:6px;background:var(--tile2);border-radius:3px;overflow:hidden;margin-top:9px}.meter>div{height:100%;width:0;background:var(--ink);transition:width .2s linear}.meter.hot>div{background:var(--orange)}\n" \
".cells{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:10px}.cell{background:var(--paper);border-radius:var(--r);padding:10px 11px;min-width:0}\n" \
".cell .k{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-size:10px;font-weight:700;letter-spacing:.12em;text-transform:uppercase;color:var(--muted)}.cell .v{font-size:18px;font-weight:800;letter-spacing:-.01em;margin-top:4px;font-variant-numeric:tabular-nums;white-space:nowrap}.cell .v small{font-size:11px;font-weight:700;color:var(--muted);margin-left:3px}\n" \
".acting{color:var(--orange)}\n" \
".sy{flex:1;min-height:40px;border:0;border-radius:var(--r);background:var(--tile);color:var(--ink);font-family:inherit;font-size:13px;font-weight:700;cursor:pointer}.sy:hover{background:var(--tile2)}.sy:active{transform:translateY(1px)}#syb{gap:8px}\n"

/* The speakers' channels (POST /channels, /state outChannels), on every page: its three switches sent together,
 * shown as /state says them (not while one has the focus: the one just pressed). The swap acts on the music,
 * before the correction (each speaker keeps its own); the polarity is the output's, per channel. */
#define SPK_NOTE_EN "For this output. The swap acts on the music; the polarity on the speaker at each output. Every sample kept exactly: lossless."
#define SPK_NOTE_FR "Pour cette sortie. L’inversion gauche-droite agit sur la musique, la phase sur l’enceinte de chaque sortie. Chaque échantillon gardé à l’identique\u00a0: sans perte."
#define SPK_HTML_EN \
"<div id=\"spk\" hidden><div class=\"label\">Channels</div><div class=\"panel\">\n" \
"<div class=\"row\"><span>Swap left and right</span><span><input type=\"checkbox\" id=\"cws\" onchange=\"spk()\"></span></div>\n" \
"<div class=\"row\"><span>Invert polarity</span><span><label>left output <input type=\"checkbox\" id=\"cil\" onchange=\"spk()\"></label> <label>right output <input type=\"checkbox\" id=\"cir\" onchange=\"spk()\"></label></span></div>\n" \
"<div class=\"row note\"><span>" SPK_NOTE_EN "</span></div>\n" \
"</div></div>\n"
#define SPK_HTML_FR \
"<div id=\"spk\" hidden><div class=\"label\">Canaux</div><div class=\"panel\">\n" \
"<div class=\"row\"><span>Inverser gauche et droite</span><span><input type=\"checkbox\" id=\"cws\" onchange=\"spk()\"></span></div>\n" \
"<div class=\"row\"><span>Inverser la phase</span><span><label>sortie gauche <input type=\"checkbox\" id=\"cil\" onchange=\"spk()\"></label> <label>sortie droite <input type=\"checkbox\" id=\"cir\" onchange=\"spk()\"></label></span></div>\n" \
"<div class=\"row note\"><span>" SPK_NOTE_FR "</span></div>\n" \
"</div></div>\n"
#define SPK_JS \
"function spk(){post('/channels?swap='+($('cws').checked?1:0)+'&invert_l='+($('cil').checked?1:0)+'&invert_r='+($('cir').checked?1:0))}\n" \
"function spks(s){const c=s.outChannels;$('spk').hidden=!c;if(!c)return;for(const[i,k]of[['cws','swap'],['cil','invertL'],['cir','invertR']]){const x=$(i);if(document.activeElement!==x)x.checked=!!c[k]}}\n"

#define PLAYER_JS \
"const $=id=>document.getElementById(id);function post(u){return fetch(u,{method:'POST'})}\n" \
"const e=x=>String(x).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/\"/g,'&quot;');\n" \
"function outs(s){const o=$('outs');if(!Array.isArray(s.receivers)){$('out').textContent=s.output;o.hidden=true;return}\n" \
"const L=[['',(s.outDefault||s.output)]].concat((s.cards||[]).map(c=>[c.id,c.card+(c.name&&c.name!==c.card?' · '+c.name:'')]),s.receivers.map(r=>[r.kind+':'+r.id,(r.kind==='upnp'?'UPnP':r.kind==='airplay'?'AirPlay':r.kind==='audiolaz'?'audiolaz Link':'Squeezebox')+' · '+r.name]));\n" \
"if(s.out&&!L.some(x=>x[0]===s.out))L.push([s.out,s.outName||s.out]);const k=JSON.stringify(L);if(o.dataset.k!==k){o.dataset.k=k;o.innerHTML=L.map(x=>'<option value=\"'+e(x[0])+'\">'+e(x[1])+'</option>').join('')}\n" \
"if(document.activeElement!==o)o.value=s.out||'';o.hidden=false;$('out').hidden=true}\n" \
"function outp(v){post('/output?to='+encodeURIComponent(v)).then(r=>r.json()).then(j=>{if(j&&j.why==='used')alert(j.by)}).catch(()=>{})}\n" \
"async function tick(){try{const s=await(await fetch('/state')).json();document.title=s.name||document.title;$('nm').textContent=s.name||'audiolaz';\n" \
"const on=s.state==='playing';$('st').classList.toggle('on',on);$('stt').textContent=on?T.playing:s.state==='busy'?T.busy:T.idle;\n" \
"$('rt').textContent=on&&s.rate>0?(s.rate/1000).toFixed(s.rate%1000?1:0):'–';$('in').textContent=s.input==='squeeze'?'Roon':s.input||'–';\n" \
"$('ch').textContent=s.inputChannels>2?T.first2(s.inputChannels):'';$('ch').hidden=!(s.inputChannels>2);outs(s);spks(s);\n" \
"const P=s.soundPath,LL=s.bitPerfect===false&&s.bitPerfectLossless===true,V=s.volume,VO=!!(V&&V.on),Z=VO&&s.bitPerfect===true&&!V.mute&&Math.abs(V.db)<0.005;\n" \
"const VB=VO&&!Z&&(s.bitPerfect!==false||(s.bitPerfectWhy||[]).every(w=>['volume','ampvolume','changed','bits'].includes(w)));\n" \
"$('bp').textContent=VB?T.vbp:s.bitPerfect===true?T.bp:s.bitPerfect===false?(LL?T.lbp:T.nbp)+(s.bitPerfectWhy||[]).map(w=>T.why[w]||w).join(', '):T.ubp;$('bp').classList.toggle('acting',!VB&&(s.bitPerfect===true||LL));\n" \
"$('path').textContent=P?(P.inRate/1000)+' kHz'+(P.inBits?' · '+P.inBits+' bits':'')+' → '+((P.deviceRate||P.outRate)/1000)+' kHz · '+P.outBits+' bits'+(P.dac?' · '+P.dac:''):'';\n" \
"$('volr').hidden=$('voln').hidden=!VO;if(VO)$('vol').textContent=V.mute?T.muted:Math.abs(V.db)<0.05?'0 dB':(V.db<0?'\\u2212':'')+Math.abs(V.db).toFixed(Math.abs(Math.round(V.db)-V.db)>=0.05?1:0).replace('.',T.dec)+' dB';\n" \
"const a=$('logo');if(s.appUrl){a.href=s.appUrl}else a.removeAttribute('href');$('roomb').hidden=!s.appUrl;if(s.appUrl)$('room').href=s.appUrl}catch(x){}setTimeout(tick,1000)}tick();\n"
static const char PAGE[] =
#ifdef ROOM_EN
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>audiolaz player</title>\n<style>\n"
#else
"<!doctype html><html lang=\"fr\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>Lecteur audiolaz</title>\n<style>\n"
#endif
PAGE_CSS
"</style></head><body><main>\n"
"<header class=\"top\"><a class=\"logo\" id=\"logo\">audiolaz</a><span class=\"status\" id=\"st\"><i></i><span id=\"stt\">…</span></span></header>\n"
"<div class=\"cv\"><div class=\"hd\"><div><div class=\"lab\" id=\"nm\">audiolaz</div><div class=\"big\">" NO_CORRECTION "</div></div><div class=\"kv\"><b id=\"rt\">–</b><small>kHz</small></div></div></div>\n"
#ifdef ROOM_EN
"<div class=\"label\">Playing</div><div class=\"panel\">\n"
"<div class=\"row\"><span>From</span><span id=\"in\">–</span></div>\n"
"<div class=\"row\"><span id=\"bp\">…</span><span id=\"path\"></span></div>\n"
"<div class=\"row note\" id=\"ch\" hidden></div>\n"
"<div class=\"row\"><span>To</span><span><select id=\"outs\" onchange=\"outp(this.value)\" hidden></select><span id=\"out\">…</span></span></div>\n"
"<div class=\"row\" id=\"volr\" hidden><span>Volume</span><span id=\"vol\">…</span></div>\n"
"<div class=\"row note\" id=\"voln\" hidden><span>64-bit, after everything else: bit-perfect at 0 dB.</span></div>\n"
"<div class=\"row note\"><span>Your DAC’s sound, intact: bit-perfect, verified, at the pace of its own clock. Controlled from your phone.</span></div>\n"
"</div><div id=\"roomb\" hidden><div class=\"label\">Your room</div><div class=\"panel\"><a class=\"row\" id=\"room\" style=\"color:inherit;text-decoration:none\"><span>What your room does to the sound, and what its correction would give back</span><span>›</span></a></div></div>\n"
SPK_HTML_EN
"<script>const T={playing:'playing',busy:'output busy',idle:'waiting',first2:n=>'A source of '+n+' channels: its first two (needed for this output)',muted:'Muted',dec:'.',\n"
"bp:'Bit-perfect, verified',vbp:'Adjustable volume: bit-perfect at 0 dB',nbp:'Not intact: ',lbp:'Lossless, verified: ',ubp:'Bit-perfect: not known yet',why:{volume:'volume set here (bit-perfect at 0 dB)',ampvolume:'volume (power amplifier)',swap:'left and right swapped',polarity:'polarity inverted',channels:'channels (needed for this output)',bits:'word length (needed for this output)',rate:'rate (needed for this device)',output:'converted by the AirPlay device',dropped:'frames dropped',changed:'samples changed'}};\n"
#else
"<div class=\"label\">Lecture</div><div class=\"panel\">\n"
"<div class=\"row\"><span>Depuis</span><span id=\"in\">–</span></div>\n"
"<div class=\"row\"><span id=\"bp\">…</span><span id=\"path\"></span></div>\n"
"<div class=\"row note\" id=\"ch\" hidden></div>\n"
"<div class=\"row\"><span>Vers</span><span><select id=\"outs\" onchange=\"outp(this.value)\" hidden></select><span id=\"out\">…</span></span></div>\n"
"<div class=\"row\" id=\"volr\" hidden><span>Volume</span><span id=\"vol\">…</span></div>\n"
"<div class=\"row note\" id=\"voln\" hidden><span>En 64 bits, après tout le reste\u00a0: bit-perfect à 0\u00a0dB.</span></div>\n"
"<div class=\"row note\"><span>Le son de votre DAC, intact\u00a0: bit-perfect vérifié, au rythme de son horloge. Piloté depuis votre téléphone.</span></div>\n"
"</div><div id=\"roomb\" hidden><div class=\"label\">Votre pièce</div><div class=\"panel\"><a class=\"row\" id=\"room\" style=\"color:inherit;text-decoration:none\"><span>Ce que votre pièce fait au son, et ce que la correction rendrait</span><span>›</span></a></div></div>\n"
SPK_HTML_FR
"<script>const T={playing:'en lecture',busy:'sortie occupée',idle:'en attente',first2:n=>'Une source de '+n+' voies : ses deux premières (conversion nécessaire pour cette sortie)',muted:'Muet',dec:',',\n"
"bp:'Bit-perfect, vérifié',vbp:'Volume réglable : bit-perfect à 0 dB',nbp:'Pas intact : ',lbp:'Sans perte, vérifié : ',ubp:'Bit-perfect : pas encore vérifié',why:{volume:'volume réglé ici (bit-perfect à 0 dB)',ampvolume:'volume (ampli de puissance)',swap:'gauche et droite inversées',polarity:'phase inversée',channels:'voies (conversion nécessaire pour cette sortie)',bits:'résolution (conversion nécessaire pour cette sortie)',rate:'fréquence (conversion nécessaire pour cet appareil)',output:'converti par l’appareil AirPlay',dropped:'échantillons perdus',changed:'échantillons modifiés'}};\n"
#endif
PLAYER_JS
SPK_JS
"</script></main></body></html>\n";

static void reply_code(int c, const char *code, const char *type, const char *body, size_t len) {
    char h[256];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", code, type, len);
    send(c, h, (size_t)n, MSG_NOSIGNAL);
    send(c, body, len, MSG_NOSIGNAL);
}
static void reply(int c, const char *type, const char *body, size_t len) { reply_code(c, "200 OK", type, body, len); }
static double qnum(const char *q, const char *key, int *ok) {
    char k[32];
    snprintf(k, sizeof k, "%s=", key);
    const char *p = q ? strstr(q, k) : NULL;
    *ok = p != NULL;
    return p ? atof(p + strlen(k)) : 0;
}
static double db(double v) { return v > 0 ? 20 * log10(v) : -120; }
static void curve_json(int c, const char *q) {
    (void)q;
    static const char NONE[] = "{\"none\":true,\"room\":\"-\",\"roomName\":\"\",\"sound\":\"" NO_CORRECTION "\",\"rate\":0}";
    reply(c, "application/json", NONE, sizeof NONE - 1);
}
static void url_decode(const char *s, char *out, size_t len) {
    size_t w = 0;
    while (*s && *s != '&' && w + 1 < len) {
        if (*s == '+') { out[w++] = ' '; s++; }
        else if (*s == '%' && s[1] && s[2]) { char h[3] = { s[1], s[2], 0 }; out[w++] = (char)strtol(h, NULL, 16); s += 3; }
        else out[w++] = *s++;
    }
    out[w] = 0;
}
/* An out= the page may set : "", alsa:<an ALSA device name>, squeeze:<a name or MAC>, upnp:<a UDN>,
 * airplay:<a MAC or name>; nothing that could break the control file's line or the JSON. */
static int out_ok(const char *v) {
    for (const char *p = v; *p; p++) if ((unsigned char)*p < 0x20 || *p == '"' || *p == '\\' || *p == 0x7f) return 0;
    if (!*v) return 1;
    if (strlen(v) > 150) return 0;
    if (!strncmp(v, OUT_CARD_PREFIX, OUT_CARD_PREFIX_LEN)) return out_card_ok(v + OUT_CARD_PREFIX_LEN);   /* a card (out.h) */
    return netout_is_net(v);
}
/* The machine's playback devices, as JSON (see the top): read again 10 s apart at most, the page asking twice a second. */
static void clean_json(char *dst, size_t len, const char *src) {
    snprintf(dst, len, "%s", src ? src : "");
    for (char *p = dst; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
}
static void cards_json(char *out, size_t len) { out_list_json(out, len); }   /* out.h: ALSA's cards, Core Audio's devices */

static void hosted_json(char *out, size_t len) {
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    static char cache[2048] = "[]";
    static time_t at = 0;
    pthread_mutex_lock(&lk);
#ifdef __linux__
    const time_t now = time(NULL);
    if (!at || now - at >= 10 || now < at) {
        at = now;
        size_t w = (size_t)snprintf(cache, sizeof cache, "[");
        DIR *d = opendir("/proc");
        struct dirent *e;
        int found = 0;
        while (d && (e = readdir(d)) && found < 8) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            char p[64], a[4096];
            snprintf(p, sizeof p, "/proc/%.20s/cmdline", e->d_name);
            FILE *f = fopen(p, "r");
            if (!f) continue;
            const size_t n = fread(a, 1, sizeof a - 1, f);
            fclose(f);
            if (!n) continue;
            a[n] = 0;
            const char *b0 = strrchr(a, '/');
            if (strncmp(b0 ? b0 + 1 : a, "squeezelite", 11)) continue;
            char nm[96] = "", mac[32] = "", dev[128] = "";

            const char *av[128];
            int ac = 0;
            for (size_t i = 0; i < n && ac < 128; i += strlen(a + i) + 1) av[ac++] = a + i;
            for (int j = 1; j < ac; j++) {
                const char *x = av[j];
                if (x[0] != '-' || !x[1] || !strchr("nmo", x[1])) continue;
                const char *v = x[2] ? x + 2 : j + 1 < ac ? av[++j] : NULL;
                if (!v) continue;
                if (x[1] == 'n') clean_json(nm, sizeof nm, v);
                else if (x[1] == 'm') clean_json(mac, sizeof mac, v);
                else clean_json(dev, sizeof dev, v);
            }
            char cid[64];
            alsa_dev_card(dev, cid, sizeof cid);
            char cidj[64];
            clean_json(cidj, sizeof cidj, cid);
            char item[400];
            const int k = snprintf(item, sizeof item, "%s{\"kind\":\"squeeze\",\"name\":\"%s\",\"mac\":\"%s\",\"out\":\"%s\",\"card\":\"%s\"}", w > 1 ? "," : "", nm, mac, dev, cidj);
            if (k > 0 && w + (size_t)k + 2 < sizeof cache) { memcpy(cache + w, item, (size_t)k); w += (size_t)k; found++; }
        }
        if (d) closedir(d);
        snprintf(cache + w, sizeof cache - w, "]");
    }
#else
    (void)at;
#endif
    snprintf(out, len, "%s", cache);
    pthread_mutex_unlock(&lk);
}

static int bitperfect_json(char *b, size_t bn, int n) {
    if (n < 1 || b[n - 1] != '}') return n;
    char v[8] = "", why[200] = "", sp[24] = "", ir[16], ib[8], orr[16], ob[8], dr[16], dac[160], inp[64], ch[8], ll[8] = "";
    status_get("bit_perfect", v, sizeof v); status_get("bit_perfect_why", why, sizeof why); status_get("bit_perfect_spans", sp, sizeof sp);
    status_get("bit_perfect_lossless", ll, sizeof ll);
    status_get("path_in_rate", ir, sizeof ir); status_get("path_in_bits", ib, sizeof ib); status_get("path_out_rate", orr, sizeof orr);
    status_get("path_out_bits", ob, sizeof ob); status_get("path_dev_rate", dr, sizeof dr); status_get("path_dac", dac, sizeof dac);
    status_get("input_channels", ch, sizeof ch);
    input_get(inp, sizeof inp);
    json_clean(dac);
    char wj[300] = "[";
    size_t w = 1;
    for (char *t = strtok(why, ","); t && w + strlen(t) + 4 < sizeof wj; t = strtok(NULL, ",")) {
        for (char *p = t; *p; p++) if (!(*p >= 'a' && *p <= 'z')) *p = '-';
        w += (size_t)snprintf(wj + w, sizeof wj - w, "%s\"%s\"", w > 1 ? "," : "", t);
    }
    snprintf(wj + w, sizeof wj - w, "]");
    const int bv = atoi(v), playing = atoi(ir) > 0;
    char path[600] = "null";
    if (playing) snprintf(path, sizeof path, "{\"input\":\"%s\",\"inRate\":%d,\"inBits\":%d,\"channels\":%d,\"outRate\":%d,\"outBits\":%d,\"deviceRate\":%s,\"dac\":\"%s\"}",
                          inp, atoi(ir), atoi(ib), atoi(ch) > 0 ? atoi(ch) : 2, atoi(orr), atoi(ob), atoi(dr) > 0 ? dr : "null", dac);

    const int m = snprintf(b + n - 1, bn - (size_t)n + 1, ",\"bitPerfect\":%s,\"bitPerfectWhy\":%s,\"bitPerfectSpans\":%ld,\"bitPerfectLossless\":%s,\"soundPath\":%s}",
                           !playing || !v[0] || bv < 0 ? "null" : bv ? "true" : "false", wj, atol(sp),
                           !playing || !v[0] || bv < 0 || bv ? "null" : atoi(ll) == 1 ? "true" : "false", path);
    if (m > 0 && (size_t)m < bn - (size_t)n + 1) return n - 1 + m;
    b[n - 1] = '}'; b[n] = 0;
    return n;
}

static int variant_json(char *b, size_t bn, int n) {
    if (n < 1 || b[n - 1] != '}') return n;
    const int m = snprintf(b + n - 1, bn - (size_t)n + 1, ",\"variant\":\"player\",\"room\":false}");
    if (m > 0 && (size_t)m < bn - (size_t)n + 1) n = n - 1 + m;
    else { b[n - 1] = '}'; b[n] = 0; }
    return bitperfect_json(b, bn, n);
}
static void state_json(int c) {
    char st[32], rate[32], room[256], bypass[8], gain[32], xruns[32], image[256], input[64], cards[8192], b[65536];
    cards_json(cards, sizeof cards);
    char hosted[2048];
    hosted_json(hosted, sizeof hosted);
    input_get(input, sizeof input);
    /* the output : out=, its kind as the status says it, the receiver's name, the receivers there */
    char out[160] = "", outj[340], kind[16], oname[100], onamej[220], label[220], rcvs[16384];   /* 08/10: 32 receivers */
    control_get("out", out, sizeof out);
    char outc0[160];
    snprintf(outc0, sizeof outc0, "%s", out);
    for (char *p = out; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
    snprintf(outj, sizeof outj, "%s", out);
    status_get("out_kind", kind, sizeof kind);
    if (strcmp(kind, "net") && strcmp(kind, "direct")) snprintf(kind, sizeof kind, "%s", netout_is_net(out) ? "net" : "alsa");
    /* the name of the receiver out= names now, not of the last one played to : out= changes at once, the stream later */
    netout_out_name(out, oname, sizeof oname);
    size_t w = 0;
    for (const char *p = oname; *p && w + 2 < sizeof onamej; p++) { if (*p == '"' || *p == '\\') onamej[w++] = '\\'; if ((unsigned char)*p >= 0x20) onamej[w++] = *p; }
    onamej[w] = 0;
    /* what the page calls the output: the receiver's name, the card out= names, or the command line's label */
    snprintf(label, sizeof label, "%s", netout_is_net(out) && onamej[0] ? onamej : !strncmp(out, OUT_CARD_PREFIX, OUT_CARD_PREFIX_LEN) && out[OUT_CARD_PREFIX_LEN] ? out + OUT_CARD_PREFIX_LEN : out_label);
    netout_receivers_json(rcvs, sizeof rcvs);
    status_get("state", st, sizeof st); status_get("rate", rate, sizeof rate); status_get("xruns", xruns, sizeof xruns);
    if (!control_get("room", room, sizeof room)) room[0] = 0;
    if (!control_get("bypass", bypass, sizeof bypass)) snprintf(bypass, sizeof bypass, "0");
    if (!control_get("gain", gain, sizeof gain)) snprintf(gain, sizeof gain, "-13");
    if (!control_get("image", image, sizeof image)) image[0] = 0;

    char rid[80], rbase[1024], rname[100];
    const int rhas = 0;
    snprintf(rid, sizeof rid, "-"); rbase[0] = 0; rname[0] = 0;
    snprintf(bypass, sizeof bypass, "1"); snprintf(gain, sizeof gain, "0"); image[0] = 0;
    char sounds[512] = "\"" NO_CORRECTION "\"";
    int listening = atoi(bypass) || !rhas ? 0 : -1, idx = 1;
    for (int k = 0; k < NSOUNDS; k++) {
        if (!have_sound_in(rbase, k)) continue;
        strcat(sounds, ",\""); strcat(sounds, SOUND_NAME[k]); strcat(sounds, "\"");
        if (listening < 0 && !strcmp(room, SOUND_DIR[k])) listening = idx;
        idx++;
    }

    char hv[340];
    int heard = -1;
    status_get("heard", hv, sizeof hv);
    if (!strcmp(hv, "-") || !strcmp(hv, "-L")) heard = 0;
    else if (rhas && hv[0]) {
        char *sl = strrchr(hv, '/');
        if (sl) {
            *sl = 0;
            for (int k = 0, i = 1; k < NSOUNDS; k++) {
                if (!have_sound_in(rbase, k)) continue;
                if (!strcmp(hv, rid) && !strcmp(sl + 1, SOUND_DIR[k])) { heard = i; break; }
                i++;
            }
        }
    }
    const double r = atof(rate) > 0 ? atof(rate) : 48000;
    const long nf = lim_now.frames > 0 ? lim_now.frames : 1, tf = lim_track.frames > 0 ? lim_track.frames : 1;
    /* the engine as the app sees it : the name escaped as the output's label is (quotes, backslashes: spaces) */
    const char *state = !strcmp(st, "playing") || !strcmp(st, "busy") ? st : "idle";
    char name[80];
    snprintf(name, sizeof name, "%s", sq_name());
    for (char *p = name; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
    /* the app's address : app_url_ok lets nothing through that JSON escapes; escaped anyway, as the names are */
    char appj[420];
    w = 0;
    for (const char *p = app_url; *p && w + 3 < sizeof appj; p++) { if (*p == '"' || *p == '\\') appj[w++] = '\\'; appj[w++] = *p; }
    appj[w] = 0;
    /* the player's latency and the AirPlay input's offset : numbers, or null */
    char latv[32], airv[32], latj[32], airj[32];
    status_get("input_latency_ms", latv, sizeof latv); status_get("airplay_offset_ms", airv, sizeof airv);
    snprintf(latj, sizeof latj, "%s", latv[0] && strspn(latv, "-0123456789") == strlen(latv) ? latv : "null");
    snprintf(airj, sizeof airj, "%s", airv[0] && strspn(airv, "-0123456789") == strlen(airv) ? airv : "null");
    /* AirPlay, video or music : the status's (the player's), the page's choice (the control's) */
    char amode[16], asend[128], amodel[64], aset[16] = "auto";
    status_get("airplay_mode", amode, sizeof amode); status_get("airplay_sender", asend, sizeof asend); status_get("airplay_model", amodel, sizeof amodel);
    control_get("airplay_mode", aset, sizeof aset);
    if (strcmp(amode, "video") && strcmp(amode, "music")) amode[0] = 0;
    if (strcmp(aset, "video") && strcmp(aset, "music")) snprintf(aset, sizeof aset, "auto");
    for (char *p = asend; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
    for (char *p = amodel; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';

    char needv[32], needj[32], missv[32], declv[8], olk[200], olv[32] = "0", outc[180] = "";
    /* the sender's volume: the page's choice (the control's), as the AirPlay input has it now (the status's:
     * airplay.conf), an AirPlay session now. airplayLevel0 always false: AirPlay at the control's level */
    char ivc[8] = "", ivs[8] = "", ass[8] = "";
    control_get("airplay_ignore_volume", ivc, sizeof ivc);
    status_get("airplay_ignore_volume", ivs, sizeof ivs); status_get("airplay_session", ass, sizeof ass);
    /* "2": the player's own "yes", the sender's volume on the device (roomd.c AIR_IV_DEV): not the page's choice */
    if (!ivc[0]) snprintf(ivc, sizeof ivc, "%d", atoi(ivs) == 1);   /* never chosen here: as the AirPlay input has it */
    char dvc[8] = "", dvs[96] = "", dvm[16] = "";       /* airplay_device_volume= (0 absent), where the device is,
                                                         * its ceiling (airplay_device_volume_max=: dB, "" the start's) */
    control_get("airplay_device_volume", dvc, sizeof dvc); status_get("airplay_device_volume", dvs, sizeof dvs);
    control_get("airplay_device_volume_max", dvm, sizeof dvm);
    if (strcmp(dvm, "start") && dvm[0]) snprintf(dvm, sizeof dvm, "%.0f", fmin(0, fmax(-30, atof(dvm)))); else snprintf(dvm, sizeof dvm, "null");
    json_clean(dvs);
    /* the Loopback input's peak since the last read (dBFS; null: no Loopback input playing) */
    /* the AirPlay input's name, the output's, and the one it takes at the session's end ("" the same) */
    char apn[160] = "", apnx[160] = "";
    status_get("airplay_name", apn, sizeof apn); status_get("airplay_name_next", apnx, sizeof apnx);
    for (char *p = apn; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
    for (char *p = apnx; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) *p = ' ';
    char inpk[24] = "null";
    if (!strcmp(st, "playing") && strncmp(input, "squeeze", 7) && input[0]) snprintf(inpk, sizeof inpk, "%.2f", page_in_peak > 0 ? 20 * log10(page_in_peak) : -150.0);
    status_get("airplay_need_ms", needv, sizeof needv); status_get("airplay_missing_ms", missv, sizeof missv); status_get("airplay_declares", declv, sizeof declv);
    snprintf(needj, sizeof needj, "%s", needv[0] && strspn(needv, "-0123456789") == strlen(needv) ? needv : "null");
    control_get("out", outc, sizeof outc);
    snprintf(olk, sizeof olk, "out_lat_ms.%s", outc[0] ? outc : "default");
    control_get(olk, olv, sizeof olv);
    {   /* the setting that applies: the AirPlay session's context's when one is known (roomd.c corr_for) */
        char cms[16] = "", ckey[16] = "";
        status_get("airplay_ctx_ms", cms, sizeof cms); status_get("airplay_ctx_key", ckey, sizeof ckey);
        if (ckey[0] && cms[0] && strspn(cms, "-0123456789") == strlen(cms)) snprintf(olv, sizeof olv, "%s", cms);
    }

    char orooms[4096], roomsj[8192], prid[80], prname[100] = "", pnone[8] = "";
    outrooms_json(orooms, sizeof orooms);
    rooms_json(roomsj, sizeof roomsj);
    status_get("place", prid, sizeof prid); status_get("place_none", pnone, sizeof pnone);
    if (atoi(pnone)) snprintf(prid, sizeof prid, "-");
    else if (!prid[0] || (strcmp(prid, "_") && !place_ok(prid))) snprintf(prid, sizeof prid, "%s", rid);
    if (strcmp(prid, "-")) place_name(rooms_dir, prid, prname, sizeof prname);
    /* another audiolaz player on this output: its name (outUsedBy), playing to it (outUsedPlaying) or set to it;
     * outTaken, outTakenBy: the output's last stream taken by another sender (its name, the taker's: "" unknown), until
     * it plays again; players: the other players heard [{name, addr, state, kind, id, rname}] */
    char uby[96] = "", ubyj[200], tkr[96] = "", tkb[96] = "", tkrj[200] = "", tkbj[200] = "", players[8192];
    const int used = netout_used_by(outc0, uby, sizeof uby);
    clean_json(ubyj, sizeof ubyj, uby);
    if (netout_taken(outc0, tkr, sizeof tkr, tkb, sizeof tkb)) { clean_json(tkrj, sizeof tkrj, tkr); clean_json(tkbj, sizeof tkbj, tkb); }
    netout_players_json(players, sizeof players);
    /* the output not answering (netout.c's backoff): its name, the seconds before it is asked again (0: none) */
    char rtryn[96] = "", rtryj[200];
    const double rtry = netout_is_net(outc0) ? netout_retry_in(rtryn, sizeof rtryn) : 0;
    clean_json(rtryj, sizeof rtryj, rtryn);

    int n = snprintf(b, sizeof b,
        "{\"playing\":%s,\"rate\":%s,\"output\":\"%s\",\"sounds\":[%s],\"listening\":%d,\"levelDb\":%s,\"levelMode\":\"%s\","
        "\"autoLevelDb\":%.2f,\"maxGainDb\":%.2f,\"autoReady\":%s,\"imagePct\":%d,\"hasImage\":%s,\"limiter\":%s,"
        "\"limitedPercent\":%.4f,\"deepestDb\":%.2f,\"peakDb\":%.2f,\"trackSeconds\":%.2f,\"trackEvents\":%ld,"
        "\"trackLimitedPercent\":%.4f,\"trackDeepestDb\":%.2f,\"trackPeakDb\":%.2f,\"volumeDb\":%.2f,\"underruns\":%s,"
        "\"state\":\"%s\",\"version\":\"%s\",\"lang\":\"%s\",\"name\":\"%s\",\"squeeze\":%s,\"format\":\"%s\","
        "\"measure\":%d,\"measurePos\":%.2f,\"squeezePos\":%.2f,\"input\":\"%s\",\"out\":\"%s\",\"outKind\":\"%s\",\"outName\":\"%s\",\"receivers\":%s,\"squeezePort\":%d,\"outDefault\":\"%s\",\"cards\":%s,\"hosted\":%s,\"appUrl\":\"%s\",\"airplayReady\":%s,\"inputLatencyMs\":%s,\"airplayOffsetMs\":%s"
        ",\"airplayInput\":%s,\"airplayMode\":\"%s\",\"airplaySetting\":\"%s\",\"airplaySender\":\"%s\",\"airplayModel\":\"%s\""
        ",\"airplayNeedMs\":%s,\"airplayMissingMs\":%d,\"airplayDeclares\":%s,\"outLatMs\":%d"
        ",\"airplayLevel0\":%s,\"airplayIgnoreVolume\":%s,\"airplayIgnoreVolumeNow\":%s,\"airplaySession\":%s,\"airplayDeviceVolume\":%s,\"airplayDeviceVolumeNow\":%s,\"deviceVolume\":\"%s\",\"deviceVolumeMax\":%s,\"inputPeakDb\":%s,\"airplayName\":\"%s\",\"airplayNameNext\":\"%s\""
        ",\"outRoom\":\"%s\",\"outRoomName\":\"%s\",\"outRooms\":%s,\"rooms\":%s,\"playRoom\":\"%s\",\"playRoomName\":\"%s\",\"heard\":%d,\"shortSound\":true"
        ",\"outUsedBy\":\"%s\",\"outUsedPlaying\":%s,\"outTaken\":\"%s\",\"outTakenBy\":\"%s\",\"players\":%s,\"outRetryS\":%.0f,\"outRetryName\":\"%s\""
        "}",
        strcmp(st, "playing") ? "false" : "true", atof(rate) > 0 ? rate : "0", label, sounds, listening, gain, level_mode,
        auto_db, max_gain_db, auto_ready ? "true" : "false", page_image_pct, image[0] ? "true" : "false", page_limiter_on ? "true" : "false",
        100.0 * lim_now.limited / nf, db(lim_now.min_gain < 1 ? lim_now.min_gain : 1), db(lim_now.peak), lim_track.frames / r, lim_track.events,
        100.0 * lim_track.limited / tf, db(lim_track.min_gain < 1 ? lim_track.min_gain : 1), db(lim_track.peak), db(sq_volume()), xruns[0] ? xruns : "0",
        state, ROOMD_VERSION, ROOMD_LANG, name, sq_connected() ? "true" : "false", sq_format(),
        sq_measuring(), sq_measuring() == 1 ? sq_heard_position() : -1.0, sq_connected() ? sq_heard_position() : -1.0, input, outj, kind, onamej, rcvs, page_slim_port, out_label, cards, hosted, appj, netout_airplay_ready() > 0 ? "true" : "false", latj, airj
        , airplay_input() ? "true" : "false", amode, aset, asend, amodel
        , needj, atoi(missv), atoi(declv) ? "true" : "false", atoi(olv)
        , "false", atoi(ivc) ? "true" : "false", atoi(ivs) == 1 ? "true" : "false", atoi(ass) ? "true" : "false",
        atoi(dvc) ? "true" : "false", atoi(ivs) == 2 ? "true" : "false", dvs, dvm, inpk, apn, apnx
        , rid, rname, orooms, roomsj, prid, prname, heard
        , ubyj, used == 2 ? "true" : "false", tkrj, tkbj, players, ceil(rtry), rtryj
        );
    lim_now.frames = 0; lim_now.limited = 0; lim_now.min_gain = 1; lim_now.peak = 0; page_in_peak = 0;
    if (n > 0 && (size_t)n < sizeof b) n = vol_json(b, sizeof b, n);
    if (n > 0 && (size_t)n < sizeof b) n = channels_json(b, sizeof b, n);   /* */
    if (n > 0 && (size_t)n < sizeof b) n = spk_json(b, sizeof b, n);        /* the speakers' channels */
    if (n > 0 && (size_t)n < sizeof b) n = upnp_json(b, sizeof b, n);   /* */
    if (n > 0 && (size_t)n < sizeof b) n = remote_in_json(b, sizeof b, n, rooms_dir);   /* remote_in.h */
    if (n > 0 && (size_t)n < sizeof b) n = link_json(b, sizeof b, n, outc0);   /* */
    if (n > 0 && (size_t)n < sizeof b) n = variant_json(b, sizeof b, n);       /* the free player or Room's */
    reply(c, "application/json", b, (size_t)n);
}
static void handle(int c) {
    char req[4096];
    ssize_t n = recv(c, req, sizeof req - 1, 0);
    if (n <= 0) return;
    req[n] = 0;
    char method[8] = "", path[512] = "";
    sscanf(req, "%7s %511s", method, path);
    char *q = strchr(path, '?');
    if (q) *q++ = 0;
    int ok;
    /* the player's clock, first and fast: the app aligns its own on it by round trips (the calibration) */
    if (!strcmp(path, "/clock")) {
        char b[64];
        const int n2 = snprintf(b, sizeof b, "{\"ns\":%lld}", roomd_clock_ns());
        reply(c, "application/json", b, (size_t)n2);
        return;
    }

    if (!strcmp(method, "POST")) {
        static const char *const FREE[] = { "/measure", "/measure/stop", "/airplay-buffered", "/airplay-meta-log", "/app", "/output",
                                            "/take", "/upnp", "/airplay", "/latency", "/nowplaying", "/nowplaying/cover", "/channels", "/remote",
                                            "/volume" };
        int free_ = !strcmp(path, "/listen") && q && !strcmp(q, "to=0");
        for (size_t i = 0; !free_ && i < sizeof FREE / sizeof *FREE; i++) free_ = !strcmp(path, FREE[i]);
        if (!free_) {
            static const char ROOM[] = "{\"ok\":false,\"why\":\"room\"}";
            reply_code(c, "403 Forbidden", "application/json", ROOM, sizeof ROOM - 1);
            return;
        }
    }

    if (!strcmp(path, "/levels")) {
        const int d = dup(c);
        if (d < 0 || roomd_levels_client(d)) {
            if (d >= 0) close(d);
            static const char BUSY[] = "{\"ok\":false,\"why\":\"busy\"}";
            reply_code(c, "503 Service Unavailable", "application/json", BUSY, sizeof BUSY - 1);
        }
        return;
    }
    if (!strcmp(path, "/state")) { snap_begin(); state_json(c); snap_end(); return; }   /* the files read once */

    if (!strcmp(method, "POST") && !strcmp(path, "/volume")) { vol_post(c, q); return; }
    /* play, pause, next, previous for AirPlay's and Spotify's senders (remote_in.h): POST /remote?src=&c= */
    if (!strcmp(method, "POST") && !strcmp(path, "/remote")) {
        char b[96];
        const int code = remote_in_post(rooms_dir, q ? q : "", b, sizeof b);
        reply_code(c, code == 200 ? "200 OK" : code == 409 ? "409 Conflict" : "400 Bad Request", "application/json", b, strlen(b));
        return;
    }
    if (!strcmp(path, "/latency") && !strcmp(method, "GET")) {
        char b[1024];
        const int m = roomd_latency_json(b, sizeof b);
        if (m > 0) reply_code(c, "200 OK", "application/json", b, (size_t)m);
        else reply_code(c, "500 Internal Server Error", "application/json", "{}", 2);
        return;
    }
    /* what plays, for the receivers' screens (netout.c, nowplay.h): POST /nowplaying, /nowplaying/cover; GET */
    if (netout_np_http(c, req, (size_t)n, method, path, q ? q : "")) return;
    if (!strcmp(path, "/curve")) { curve_json(c, q); return; }

    if (!strcmp(method, "POST") && !strcmp(path, "/measure")) {
        const double port = qnum(q, "port", &ok);
        const char *sg = q ? strstr(q, "signal=") : NULL;
        char signal[33] = "";
        if (sg) { snprintf(signal, sizeof signal, "%.32s", sg + 7); signal[strcspn(signal, "&")] = 0; }
        int good = ok && port >= 1 && port <= 65535 && signal[0];
        for (const char *p = signal; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) good = 0;
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        char host[INET_ADDRSTRLEN] = "";
        if (good && (getpeername(c, (struct sockaddr *)&peer, &pl) || !inet_ntop(AF_INET, &peer.sin_addr, host, sizeof host))) good = 0;
        const char *why = "bad";
        if (good) {
            char tp[128];
            snprintf(tp, sizeof tp, "/download/measurement-track.wav?signal=%s", signal);
            int lk;
            const double lp = qnum(q, "loop", &lk);
            int ck;
            const double cv = qnum(q, "corrected", &ck);
            int sk;
            const double sv = qnum(q, "sound", &sk);
            const int r = sq_measure(host, (int)port, tp, lk && lp != 0, sk && sv != 0 ? 2 : ck && cv != 0);
            why = r == 0 ? NULL : r == -2 ? "playing" : "no-input";
        }
        char b[96];
        const int n2 = why ? snprintf(b, sizeof b, "{\"ok\":false,\"why\":\"%s\"}", why) : snprintf(b, sizeof b, "{\"ok\":true}");
        reply(c, "application/json", b, (size_t)n2);
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/measure/stop")) { sq_measure_stop(); reply(c, "application/json", "{\"ok\":true}", 11); return; }
    /* the Buffered AirPlay streams' offset (roomd.c airplay_tick): POST /airplay-buffered?m=full|declared:
     * the control's airplay_buffered= (full: all of their need by the receiver; declared: as a Realtime stream's) */
    if (!strcmp(method, "POST") && !strcmp(path, "/airplay-buffered")) {
        const char *m = q ? strstr(q, "m=") : NULL;
        if (m && (m == q || m[-1] == '&') && (!strncmp(m + 2, "full", 4) || !strncmp(m + 2, "declared", 8))) {
            const char *k[] = { "airplay_buffered" }, *vv[] = { !strncmp(m + 2, "full", 4) ? "full" : "declared" };
            control_set(k, vv, 1);
            reply(c, "application/json", "{\"ok\":true}", 11);
        } else { static const char BAD[] = "{\"ok\":false,\"why\":\"bad\"}"; reply(c, "application/json", BAD, sizeof BAD - 1); }
        return;
    }
    /* The capture of the AirPlay senders' metadata (10/10, roomd.c cap_item): POST /airplay-meta-log?m=0|1|2: the next
     * AirPlay session written to the journal (2: with shairport-sync's plist metadata, restarted between sessions for
     * it); each call a new capture (its time in the value); 0: none. */
    if (!strcmp(method, "POST") && !strcmp(path, "/airplay-meta-log")) {
        const char *m = q ? strstr(q, "m=") : NULL;
        const int lv = m && (m == q || m[-1] == '&') && m[2] >= '0' && m[2] <= '2' ? m[2] - '0' : -1;
        if (lv >= 0) {
            char val[32];
            if (lv) snprintf(val, sizeof val, "%d.%ld", lv, (long)time(NULL)); else snprintf(val, sizeof val, "0");
            const char *k[] = { "airplay_meta_log" }, *vv[] = { val };
            control_set(k, vv, 1);
            reply(c, "application/json", "{\"ok\":true}", 11);
        } else { static const char BAD[] = "{\"ok\":false,\"why\":\"bad\"}"; reply(c, "application/json", BAD, sizeof BAD - 1); }
        return;
    }
    /* The output : POST /output?to=<out=, URL-encoded> ("" the command line's): {"ok":true}, or
     * {"ok":false,"why":"bad"} (not a card's name, nor squeeze:…, nor upnp:…). Read by the engine at the next play. */
    /* The app's address for the way back (08/10, the app's EnginePlayer, when it changed): POST /app?url=<URL-encoded>,
     * {"ok":true}, kept in page.conf; anything not http(s)://host[:port][/path] of safe characters: 400. */
    if (!strcmp(method, "POST") && !strcmp(path, "/app")) {
        const char *v = q ? strstr(q, "url=") : NULL;
        char u[256] = "";
        if (v && (v == q || v[-1] == '&')) url_decode(v + 4, u, sizeof u);
        if (v && strlen(u) < sizeof u - 1 && app_url_ok(u)) {
            if (strcmp(u, app_url)) { snprintf(app_url, sizeof app_url, "%s", u); save_conf(); }
            reply(c, "application/json", "{\"ok\":true}", 11);
        } else { static const char BAD[] = "{\"ok\":false,\"why\":\"bad\"}"; reply_code(c, "400 Bad Request", "application/json", BAD, sizeof BAD - 1); }
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/output")) {
        const char *v = q ? strstr(q, "to=") : NULL;
        char to[200] = "";
        if (v && (v == q || v[-1] == '&')) url_decode(v + 3, to, sizeof to);
        const int good = v && out_ok(to);

        char by[96] = "", byj[200];
        if (good && netout_is_net(to) && netout_used_by(to, by, sizeof by) == 2) {
            char cur[160] = "";
            control_get("out", cur, sizeof cur);
            if (strcmp(cur, to)) {
                clean_json(byj, sizeof byj, by);
                char b[300];
                const int n2 = snprintf(b, sizeof b, "{\"ok\":false,\"why\":\"used\",\"by\":\"%s\"}", byj);
                reply(c, "application/json", b, (size_t)n2);
                return;
            }
        }

        if (good && netout_is_net(to)) {
            char heard[400], hj[900];
            const char *f = q ? strstr(q, "force=1") : NULL;
            if (!(f && (f == q || f[-1] == '&')) && netout_az_unknown(to, heard, sizeof heard)) {
                clean_json(hj, sizeof hj, heard);
                char b[1000];
                const int n2 = snprintf(b, sizeof b, "{\"ok\":false,\"why\":\"unknown\",\"heard\":\"%s\"}", hj);
                reply(c, "application/json", b, (size_t)n2);
                return;
            }
        }
        if (good) { const char *k[] = { "out" }, *vv[] = { to }; control_set(k, vv, 1); }
        if (good) reply(c, "application/json", "{\"ok\":true}", 11);
        else { static const char BAD[] = "{\"ok\":false,\"why\":\"bad\"}"; reply(c, "application/json", BAD, sizeof BAD - 1); }
        return;
    }
    /* The speakers' channels (roomd.c, channels.h; the free player too: nothing resampled nor rounded, every
     * sample exact, moved or negated; the swap on the music, before the correction, the polarity per output
     * channel): POST /channels?[swap=0|1][&invert_l=0|1][&invert_r=0|1][&out=<the output, URL-encoded;
     * "default" the command line's; absent: the control's out=, the one playing>]: each one given set for that output
     * (1 its line, 0 its line taken off), the others kept; the player follows at once (a crossfade of a few ms).
     * {"ok":true,"outChannels":{swap, invertL, invertR, now}} as /state says it, or {"ok":false,"why":"bad"}. */
    if (!strcmp(method, "POST") && !strcmp(path, "/channels")) {
        static const char *const K[] = { "swap=", "invert_l=", "invert_r=" };
        const char *o = q ? strstr(q, "out=") : NULL;
        char to[200] = "", k[3][200];
        if (o && (o == q || o[-1] == '&')) url_decode(o + 4, to, sizeof to);
        else control_get("out", to, sizeof to);
        if (!strcmp(to, "default")) to[0] = 0;
        const char *kk[4], *vv[4];
        int nk = 0, good = out_ok(to);
        spk_keys(to, k);
        for (int i = 0; i < 3 && q && good; i++) {
            const char *v = strstr(q, K[i]);
            while (v && v != q && v[-1] != '&') v = strstr(v + 1, K[i]);
            if (!v) continue;
            const char *x = v + strlen(K[i]);
            if ((x[0] != '0' && x[0] != '1') || (x[1] && x[1] != '&')) { good = 0; break; }
            kk[nk] = k[i]; vv[nk++] = x[0] == '1' ? "1" : NULL;
        }
        if (!good || !nk) { static const char BAD[] = "{\"ok\":false,\"why\":\"bad\"}"; reply(c, "application/json", BAD, sizeof BAD - 1); return; }
        kk[nk] = CHAN_ORDER_KEY; vv[nk++] = "2";   /* set with the meaning of 1.2.9: never exchanged later */
        control_set(kk, vv, nk);
        char nw[8] = "", ob[120], b[200];
        status_get("channels_now", nw, sizeof nw);
        spk_obj(ob, sizeof ob, spk_get(to), atoi(nw) & 7);
        const int n2 = snprintf(b, sizeof b, "{\"ok\":true,\"outChannels\":%s}", ob);
        reply(c, "application/json", b, (size_t)n2);
        return;
    }

    if (!strcmp(method, "POST") && !strcmp(path, "/take")) {
        netout_take();
        reply(c, "application/json", "{\"ok\":true}", 11);
        return;
    }

    if (!strcmp(method, "POST") && !strcmp(path, "/voies")) {
        const char *v = q ? strstr(q, "on=") : NULL;
        if (v && (v == q || v[-1] == '&') && (v[3] == '0' || v[3] == '1')) {
            const char *k[1] = { "voies" }, *vv[1] = { v[3] == '1' ? "1" : "0" };
            control_set(k, vv, 1);
            reply(c, "application/json", "{\"ok\":true}", 11);
        } else { static const char BAD[] = "{\"ok\":false,\"why\":\"bad\"}"; reply(c, "application/json", BAD, sizeof BAD - 1); }
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/outroom")) {
        const char *v = q ? strstr(q, "out=") : NULL, *r = q ? strstr(q, "room=") : NULL;
        char to[200] = "", rm[100] = "", d[1100];
        if (v && (v == q || v[-1] == '&')) url_decode(v + 4, to, sizeof to);
        if (r && (r == q || r[-1] == '&')) url_decode(r + 5, rm, sizeof rm);
        snprintf(d, sizeof d, "%s/places/%s", rooms_dir, rm);
        int good = v && r && out_ok(to) && (!strcmp(rm, "-") || !strcmp(rm, "_") || (place_ok(rm) && is_dir(d)));
        if (good) {
            char key[220], k0[220], k1[220], r0[80], cur[180] = "", line[600], p[1024];
            int any = 0;
            snprintf(p, sizeof p, "%s/control", rooms_dir);
            FILE *f = fopen(p, "r");
            while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "outroom.", 8)) any = 1;
            if (f) fclose(f);
            control_get("out", cur, sizeof cur);
            snprintf(key, sizeof key, "outroom.%s", to[0] ? to : "default");
            const char *k[3], *vv[3];
            int n = 0;
            if (!any) {                                    /* the first binding: the others keep the room of before */
                page_outroom(rooms_dir, "", r0, sizeof r0, NULL, 0, NULL, 0);
                snprintf(k0, sizeof k0, "outroom.default");
                if (to[0] && strcmp(r0, "-")) { k[n] = k0; vv[n++] = r0; }
                snprintf(k1, sizeof k1, "outroom.%s", cur);
                if (cur[0] && strcmp(cur, to) && strcmp(r0, "-")) { k[n] = k1; vv[n++] = r0; }
            }
            k[n] = key; vv[n++] = rm;
            control_set(k, vv, n);

            if (place_ok(rm) || !strcmp(rm, "_")) {
                char vk[120]; const char *vks[1] = { vk }, *none[1] = { NULL }, *val[1] = { to[0] ? to : "default" };
                snprintf(vk, sizeof vk, "voie.%s", rm);
                control_set(vks, none, 1);
                control_set(vks, val, 1);
            }
            reply(c, "application/json", "{\"ok\":true}", 11);
        } else { static const char BAD[] = "{\"ok\":false,\"why\":\"bad\"}"; reply(c, "application/json", BAD, sizeof BAD - 1); }
        return;
    }
    if (!strcmp(method, "POST")) {
        if (!strcmp(path, "/listen")) {

        } else if (!strcmp(path, "/upnp")) {       /* on=0|1: the UPnP input switched; vol=fixed|digital: its volume
                                                    * applied or not (upnp.c reads both from the control within a second) */
            const char *o2 = q ? strstr(q, "on=") : NULL, *v2 = q ? strstr(q, "vol=") : NULL;
            if (o2 && (o2 == q || o2[-1] == '&') && (o2[3] == '0' || o2[3] == '1')) { const char *k[] = { "upnp" }, *vv[] = { o2[3] == '1' ? "1" : "0" }; control_set(k, vv, 1); }
            (void)v2;                                     /* the free player: the UPnP input's volume never applied */
        } else if (!strcmp(path, "/airplay")) {    /* m=auto|video|music : AirPlay's mode; a play going on opened again when it changes its own */
            const char *m = q ? strstr(q, "m=") : NULL;
            char want[8] = "";
            if (m && (m == q || m[-1] == '&')) { snprintf(want, sizeof want, "%.7s", m + 2); want[strcspn(want, "&")] = 0; }
            if (!strcmp(want, "auto") || !strcmp(want, "video") || !strcmp(want, "music")) { const char *k[] = { "airplay_mode" }, *vv[] = { want }; control_set(k, vv, 1); }
        } else if (!strcmp(path, "/latency")) {      /* ring=, rt=, link= (page.h); the next play takes them */
            static const char *const K[] = { "ring=", "rt=", "link=" };
            for (int k = 0; k < 3 && q; k++) {
                const char *v = strstr(q, K[k]);
                while (v && v != q && v[-1] != '&') v = strstr(v + 1, K[k]);
                if (!v) continue;
                const long x = atol(v + strlen(K[k]));
                if (k == 0) page_lat_ring = lat_bound(x, 15, 800);
                else if (k == 1) page_lat_rt = lat_bound(x, 1, 89);
                else page_lat_link = lat_bound(x, 10, 150);
            }
            save_conf();
        }
        reply(c, "application/json", "{\"ok\":true}", 11);
        return;
    }
    /* <rooms>/page.html, when there, instead of the page built in : the page retouched without restarting the
     * engine */
    char pf[1024];
    snprintf(pf, sizeof pf, "%s/page.html", rooms_dir);
    FILE *f = fopen(pf, "r");
    if (f) {
        static char buf[262144];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        if (n > 0 && n < sizeof buf) { reply(c, "text/html; charset=utf-8", buf, n); return; }
    }
    reply(c, "text/html; charset=utf-8", PAGE, sizeof PAGE - 1);
}
static int listen_fd = -1;
static void *page_main(void *arg) {
    (void)arg;
    for (;;) {
        struct pollfd pf = { listen_fd, POLLIN, 0 };
        const int r = poll(&pf, 1, 250);                 /* four times a second: the volume's sources (vol_tick) */
        {
            static double t_sounds = 0;
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const double tn = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
            if (r > 0 || tn - t_sounds >= 1.0) { t_sounds = tn; sounds_check(); }
        }
        vol_tick();
        if (r <= 0) continue;
        int c = accept(listen_fd, NULL, NULL);
        if (c < 0) { usleep(100000); continue; }
        struct timeval tv = { 2, 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        handle(c);
        close(c);
    }
    return NULL;
}

int page_start(int port, const char *rooms, const char *output) {
    snprintf(rooms_dir, sizeof rooms_dir, "%s", rooms);
    snprintf(out_label, sizeof out_label, "%s", output && *output ? output : THE_DAC);
    for (char *p = out_label; *p; p++) if (*p == '"' || *p == '\\') *p = ' ';
    load_conf();
    if (vol_migrated >= 0) save_conf();                /* migrated (vol_migrate): kept, with volume_paths=1 */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&a, sizeof a) || listen(listen_fd, 8)) { if (listen_fd >= 0) close(listen_fd); return -1; }
    pthread_t th;
    if (pthread_create(&th, NULL, page_main, NULL)) return -1;
    pthread_detach(th);
    return 0;
}
