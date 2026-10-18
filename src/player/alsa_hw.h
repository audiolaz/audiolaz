/* src/player/alsa_hw.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef ALSA_HW_H
#define ALSA_HW_H
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
static inline int alsa_hw_line(const char *path, char *out, size_t len) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, (int)len, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
    return out[0] ? 0 : -1;
}

static inline void alsa_hw_clean(char *s) {
    size_t w = 0;
    for (size_t i = 0; s[i]; i++) if (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_' || s[i] == '.') s[w++] = s[i];
    s[w] = 0;
}
/* The machine by its hardware: the MAC of its first physical network interface (by name; one with a device in /sys),
 * 12 hex digits, "" none. */
static inline void alsa_hw_host(char *out, size_t len) {
    out[0] = 0;
    DIR *d = opendir("/sys/class/net");
    if (!d) return;
    struct dirent *e;
    char best[64] = "";
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo") || strlen(e->d_name) >= sizeof best) continue;
        char p[128], a[40];
        snprintf(p, sizeof p, "/sys/class/net/%.40s/device", e->d_name);
        if (access(p, F_OK)) continue;                          /* virtual (a bridge, a tunnel, docker) */
        snprintf(p, sizeof p, "/sys/class/net/%.40s/address", e->d_name);
        if (alsa_hw_line(p, a, sizeof a) || !strcmp(a, "00:00:00:00:00:00")) continue;
        if (best[0] && strcmp(e->d_name, best) > 0) continue;
        snprintf(best, sizeof best, "%.63s", e->d_name);
        size_t w = 0;
        for (size_t i = 0; a[i] && w + 1 < len; i++) if (isxdigit((unsigned char)a[i])) out[w++] = (char)tolower((unsigned char)a[i]);
        out[w] = 0;
    }
    closedir(d);
    if (strlen(out) != 12) out[0] = 0;
}
static inline void alsa_card_hw(int card, char *hw, size_t len) {
    char p[96], v[32], dev[PATH_MAX], s[64] = "";
    hw[0] = 0;
    if (len < 2 || card < 0) return;
    snprintf(p, sizeof p, "/proc/asound/card%d/usbid", card);
    if (alsa_hw_line(p, v, sizeof v)) return;                  /* not USB */

    for (char *c = v; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (strlen(v) != 9 || v[4] != ':') return;
    for (int i = 0; i < 9; i++) if (i != 4 && !isxdigit((unsigned char)v[i])) return;

    snprintf(p, sizeof p, "/sys/class/sound/card%d/device", card);
    if (!realpath(p, dev)) { snprintf(hw, len, "usb:%s", v); return; }
    char *sl = strrchr(dev, '/');
    if (sl && strchr(sl, ':')) *sl = 0;                        /* the interface → the device */
    char sp[PATH_MAX + 16];
    snprintf(sp, sizeof sp, "%s/serial", dev);
    if (!alsa_hw_line(sp, s, sizeof s)) alsa_hw_clean(s);
    if (s[0]) snprintf(hw, len, "usb:%s:%.40s", v, s);
    else {
        sl = strrchr(dev, '/');
        char path[64], mac[16];
        snprintf(path, sizeof path, "%.63s", sl ? sl + 1 : dev);
        alsa_hw_clean(path);
        alsa_hw_host(mac, sizeof mac);
        snprintf(hw, len, "usb:%s@%s%s%s", v, path, mac[0] ? "/" : "", mac);
    }
}
static inline int alsa_card_index(const char *id) {
    char p[64], v[64];
    if (!id || !id[0]) return -1;
    for (int n = 0; n < 32; n++) {
        snprintf(p, sizeof p, "/proc/asound/card%d/id", n);
        if (!alsa_hw_line(p, v, sizeof v) && !strcmp(v, id)) return n;
    }
    return -1;
}
static inline void alsa_dev_card(const char *dev, char *id, size_t len) {
    id[0] = 0;
    if (!dev || len < 2) return;
    const char *c = strstr(dev, "CARD=");
    if (c) {
        c += 5;
        size_t k = strcspn(c, ",:' \"");
        if (k >= len) k = len - 1;
        memcpy(id, c, k); id[k] = 0;
        return;
    }
    const char *h = !strncmp(dev, "plughw:", 7) ? dev + 7 : !strncmp(dev, "hw:", 3) ? dev + 3 : NULL;
    if (!h) return;
    if (isdigit((unsigned char)*h)) {
        char p[64];
        snprintf(p, sizeof p, "/proc/asound/card%d/id", atoi(h));
        if (alsa_hw_line(p, id, len)) id[0] = 0;
    } else { size_t k = strcspn(h, ",:"); if (k >= len) k = len - 1; memcpy(id, h, k); id[k] = 0; }
}
#else
static inline void alsa_card_hw(int card, char *hw, size_t len) { (void)card; if (len) hw[0] = 0; }
static inline int alsa_card_index(const char *id) { (void)id; return -1; }
static inline void alsa_dev_card(const char *dev, char *id, size_t len) { (void)dev; if (len) id[0] = 0; }
#endif
#endif
