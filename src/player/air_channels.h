/* src/player/air_channels.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AIR_CHANNELS_H
#define AIR_CHANNELS_H

/* The channels the source opened input k with (the Loopback: device 1's hw_params; macOS: the AirPlay receiver's
 * layout in its pipe's 8 channels; src.h), 2 when not known. */
static int loop_channels(int k) {
    const unsigned ch = src_channels(k);
    return ch >= 1 && ch <= 32 ? (int)ch : 2;
}

/* the built-in UPnP input: WAV's and FLAC's own order (FL FR FC LFE BL BR SL SR), FFmpeg's */

static int st_in_ch = 0;                 /* this play's source channels (0: not playing) */
static char st_in_layout[24] = "";       /* "stereo", "5.1", "7.1"… */
static int st_dlg_centre = 0;
static int air_ch_set = 0;               /* the control's airplay_channels: 1 multi, 0 stereo (absent too: 11/10) */
static int air_ch_file = -1;             /* airplay.conf's: 1 multichannel, 0 stereo, −1 no such file */

/* The settings, their section (0 general, 1 alsa) and values: [0] stereo, [1] multi. */
#define AIRCH_N 5
static const char *const AIRCH_KEY[AIRCH_N] = { "six_channel_mode", "eight_channel_mode", "mixdown", "output_channel_mapping", "output_channels" };
static const int AIRCH_SEC[AIRCH_N] = { 0, 0, 0, 0, 1 };
static const char *const AIRCH_VAL[2][AIRCH_N] = { { "\"off\"", "\"off\"", "\"stereo\"", "\"off\"", "2" },
                                                    { "\"on\"", "\"on\"", "\"off\"", "\"off\"", "\"auto\"" } };
static int airch_key(const char *s) {
    s += strspn(s, " \t");
    for (int i = 0; i < AIRCH_N; i++) {
        const size_t n = strlen(AIRCH_KEY[i]);
        if (!strncmp(s, AIRCH_KEY[i], n) && (s[n] == ' ' || s[n] == '\t' || s[n] == '=' || s[n] == ':')) return i;
    }
    return -1;
}
/* A setting's value as written (after = or :, up to ; or a comment), trimmed. */
static void airch_value(const char *s, char *v, size_t vl) {
    v[0] = 0;
    const char *eq = strpbrk(s, "=:");
    if (!eq) return;
    eq++;
    eq += strspn(eq, " \t");
    size_t n = strcspn(eq, ";/\r\n");
    while (n > 0 && (eq[n - 1] == ' ' || eq[n - 1] == '\t')) n--;
    snprintf(v, vl, "%.*s", (int)n, eq);
}
static void airch_line(char *o, size_t ol, int k, int multi) {
    snprintf(o, ol, "%s = %s; // audiolaz: AirPlay channels, the player's (%s)", AIRCH_KEY[k], AIRCH_VAL[multi][k],
             multi ? "multi: 5.1 and 7.1 received, mixed down by the player" : "stereo: the sender mixes down");
}
/* A value multichannel takes as its own: absent (shairport-sync 5's default), the one written, or what does the same
 * on the Loopback (on/yes; mixdown and the channel map off, no or auto; the output's channels auto). */
static int airch_multi_ok(int k, const char *v, int present) {
    if (!present) return 1;
    switch (k) {
    case 0: case 1: return !strcmp(v, "\"on\"") || !strcmp(v, "\"yes\"");
    case 2: case 3: return !strcmp(v, "\"off\"") || !strcmp(v, "\"no\"") || !strcmp(v, "\"auto\"");
    default: return !strcmp(v, "\"auto\"");
    }
}
/* 1: the file is as `multi` wants (each setting at most once; stereo: each written, as stereo wants). *fm: what it
 * says (six_channel_mode "off" or "no": stereo; anything else, or none: multichannel, shairport-sync 5's default). */
static int air_ch_state(const char *buf, int multi, int *fm) {
    int cnt[AIRCH_N] = { 0 }, match[AIRCH_N] = { 0 };
    char val[AIRCH_N][64] = { { 0 } };
    *fm = 1;
    for (const char *l = buf; *l; ) {
        const char *e = strchr(l, '\n');
        const size_t ll = e ? (size_t)(e - l) : strlen(l);
        char line[600], v[64];
        snprintf(line, sizeof line, "%.*s", (int)(ll < sizeof line - 1 ? ll : sizeof line - 1), l);
        const int k = airch_key(line);
        if (k >= 0) {
            airch_value(line, v, sizeof v);
            if (++cnt[k] == 1) { match[k] = !strcmp(v, AIRCH_VAL[multi][k]); snprintf(val[k], sizeof val[k], "%s", v); }
            if (k == 0 && cnt[k] == 1) *fm = strcmp(v, "\"off\"") && strcmp(v, "\"no\"");
        }
        l += ll + (e ? 1 : 0);
    }
    for (int k = 0; k < AIRCH_N; k++) {
        if (cnt[k] > 1) return 0;
        if (multi ? !airch_multi_ok(k, val[k], cnt[k]) : !(cnt[k] == 1 && match[k])) return 0;
    }
    return 1;
}
/* The file rewritten (atomically) with the settings `multi` wants: each one where it was (its first, the others
 * dropped), the missing ones at the top of their section (an alsa section added when there is none). 0: done. */
static int air_ch_write(const char *path, const char *buf, int multi) {
    int seen[AIRCH_N] = { 0 }, done[AIRCH_N] = { 0 };
    for (const char *l = buf; *l; ) {
        const char *e = strchr(l, '\n');
        const size_t ll = e ? (size_t)(e - l) : strlen(l);
        char line[600];
        snprintf(line, sizeof line, "%.*s", (int)(ll < sizeof line - 1 ? ll : sizeof line - 1), l);
        const int k = airch_key(line);
        if (k >= 0) seen[k] = 1;
        l += ll + (e ? 1 : 0);
    }
    char tmp[620], ln[300];
    snprintf(tmp, sizeof tmp, "%s/.airplay.conf", ROOMS);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    int pend = -1, have_sec[2] = { 0, 0 };
    for (const char *l = buf; *l; ) {
        const char *e = strchr(l, '\n');
        const size_t ll = e ? (size_t)(e - l) : strlen(l);
        const size_t ind = strspn(l, " \t");
        char line[600];
        snprintf(line, sizeof line, "%.*s", (int)(ll < sizeof line - 1 ? ll : sizeof line - 1), l);
        const int k = airch_key(line);
        if (k >= 0) {                                    /* a setting of ours: its first place, rewritten; the others gone */
            if (!done[k]) { airch_line(ln, sizeof ln, k, multi); fprintf(f, "%.*s%s\n", (int)(ind < 40 ? ind : 40), l, ln); done[k] = 1; }
            l += ll + (e ? 1 : 0);
            continue;
        }
        const char *s = line + ind;
        for (int g = 0; g < 2; g++) {
            const char *nm = g ? "alsa" : "general";
            const size_t n = strlen(nm);
            if (!strncmp(s, nm, n) && (s[n] == ' ' || s[n] == '\t' || s[n] == '=' || s[n] == ':' || s[n] == '{')) { pend = g; have_sec[g] = 1; }
        }
        const char *br = pend >= 0 ? memchr(l, '{', ll) : NULL;
        if (br) {                                        /* the missing ones right after its brace */
            fwrite(l, 1, (size_t)(br - l) + 1, f);
            fputc('\n', f);
            for (int j = 0; j < AIRCH_N; j++)
                if (!seen[j] && !done[j] && AIRCH_SEC[j] == pend) { airch_line(ln, sizeof ln, j, multi); fprintf(f, "  %s\n", ln); done[j] = 1; }
            const char *rest = br + 1;
            const size_t rl = ll - (size_t)(rest - l);
            if (strspn(rest, " \t") < rl) fprintf(f, "%.*s\n", (int)rl, rest);
            pend = -1;
            l += ll + (e ? 1 : 0);
            continue;
        }
        fwrite(l, 1, ll, f);
        fputc('\n', f);
        l += ll + (e ? 1 : 0);
    }
    for (int g = 0; g < 2; g++) {                       /* a section missing: added at the end */
        int any = 0;
        for (int j = 0; j < AIRCH_N; j++) if (!done[j] && AIRCH_SEC[j] == g) any = 1;
        if (!any || have_sec[g]) continue;
        fprintf(f, "%s =\n{\n", g ? "alsa" : "general");
        for (int j = 0; j < AIRCH_N; j++) if (!done[j] && AIRCH_SEC[j] == g) { airch_line(ln, sizeof ln, j, multi); fprintf(f, "  %s\n", ln); done[j] = 1; }
        fprintf(f, "};\n");
    }
    if (fchmod(fileno(f), 0644) || fclose(f)) { unlink(tmp); return -1; }
    return rename(tmp, path);
}

/* Once a second at most, from the main loop and from play(): the choice followed in airplay.conf, between AirPlay
 * sessions (none for AIR_IDLE_S), at most once in AIR_SPACING_S, never again for a choice it could not write. */
static void air_channels_tick(void) {
    if (VOIE) return;                          /* the AirPlay input is the player's, never a voie's */
    static double t_last = 0, idle_since = 0, t_done = -1e9;
    static int failed = -1;
    static char buf[16384];
    const double t = now_s();
    if (t - t_last < 1.0 || NO_LOOP) return;
    t_last = t;
    air_ch_set = 0;
    const int k = airplay_sub();
    char path[600], nm[160];
    snprintf(path, sizeof path, "%s/airplay.conf", ROOMS);
    double off = 0, bufs = 0.2, decl = -1;
    int port = 0, iv = 0, fm = 1;
    air_extra xr;
    if (k < 0 || !air_read(path, buf, sizeof buf, &off, &port, &bufs, &decl, &iv, nm, sizeof nm, &xr)) { air_ch_file = -1; idle_since = 0; return; }
    const int owned = src_owns(k);                       /* (2b) macOS: the layer writes the file, at its receiver's start */
    const int as_wanted = owned ? (air_ch_state(buf, air_ch_set, &fm), fm == air_ch_set) : air_ch_state(buf, air_ch_set, &fm);
    air_ch_file = fm;
    if (as_wanted) { idle_since = 0; failed = -1; return; }
    if (failed == air_ch_set) return;
    if (!src_closed(k) || src_air_connected(port)) { idle_since = 0; return; }   /* an AirPlay session: never in it */
    if (!idle_since) { idle_since = t; return; }
    if (t - idle_since < AIR_IDLE_S || t - t_done < AIR_SPACING_S) return;
    idle_since = 0; t_done = t;
    if (owned) {                                         /* (2b) its receiver started again: its settings written anew */
        logf_("AirPlay input: %s, no AirPlay session: the receiver started again with it",
              air_ch_set ? "multichannel, 5.1 and 7.1 received and mixed down here" : "stereo, the sender mixes down");
        input_stop(k);
        return;
    }
    if (air_ch_write(path, buf, air_ch_set)) {
        failed = air_ch_set;
        logf_("AirPlay input: %s not written (%s): its channels stay %s", path, strerror(errno), fm ? "multichannel" : "stereo");
        return;
    }
    air_ch_file = air_ch_set;
    char stop[600];
    snprintf(stop, sizeof stop, "%s/input-stop", ROOMS);
    const int can = !access(stop, X_OK);
    logf_("AirPlay input: %s (six_channel_mode and eight_channel_mode %s, mixdown %s, output_channels %s), no AirPlay session: %s",
          air_ch_set ? "multichannel, 5.1 and 7.1 received and mixed down here" : "stereo, the sender mixes down",
          AIRCH_VAL[air_ch_set][0], AIRCH_VAL[air_ch_set][2], AIRCH_VAL[air_ch_set][4],
          can ? "the input restarted" : "read at the input's next start (no input-stop)");
    if (can) input_stop(k);
}

/* The status's lines (write_status). */
static void dmx_status(FILE *f, const char *state) {
    const int live = strcmp(state, "idle") != 0;
    fprintf(f, "input_channels=%d\ninput_layout=%s\ndialogue_centre=%d\n", live ? st_in_ch : 0, live ? st_in_layout : "", live && st_dlg_centre);
    fprintf(f, "airplay_channels=%s\nairplay_channels_file=%s\n", air_ch_set ? "multi" : "stereo",
            air_ch_file < 0 ? "" : air_ch_file ? "multi" : "stereo");
}

#endif
