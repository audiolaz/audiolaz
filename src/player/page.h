/* src/player/page.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_PAGE_H
#define AUDIOLAZ_PAGE_H
#include <stddef.h>
extern volatile int page_image_pct;     /* how much of the image's crossed paths plays: 0–300 %, 100 as designed */
extern volatile int page_limiter_on;
extern volatile double page_in_peak;    /* the Loopback input's peak since /state last read it , 0–1 */
extern volatile int page_slim_port;     /* where Squeezebox players connect to the network outputs : 0, none */

extern volatile int page_lat_ring, page_lat_rt, page_lat_link;
int roomd_latency_json(char *b, size_t len);
/* Serves http://<this machine>:port, the settings kept in <rooms>/page.conf; output: what the page calls the DAC. */

int page_outroom(const char *rooms, const char *out, char *id, size_t idn, char *base, size_t bn, char *name, size_t nn);
int page_start(int port, const char *rooms, const char *output);

void page_conf_follow(const char *rooms);

int page_level_manual(void);

void page_sound_label(const char *key, char *sound, size_t sl, char *room, size_t rl);
/* The acoustic calibration (10/10, roomd.c): clicks at at[0, n) ms of the app's next short track marked as it plays
 * (0, or −1: refused); its state and the clicks' times leaving the output as JSON (the bytes, −1: too long); the
 * player's clock (CLOCK_MONOTONIC, ns), the one those times are on. */

int page_vol_for(const char *out, const char *place, double *db, int *mute);
double page_vol_src(int src, double want, int mute, int fresh, int try_);
void roomd_vol_now(double *db, int *active);
/* upnp.c: the renderer's Volume and Mute told the level the player applies (its GetVolume, its events); db NAN:
 * the option off, its mute taken off (nothing left silent) */
void upnp_vol_follow(double db, int mute);
/* GET /levels (engine/tap/levels.h): the connection handed to the levels' stream, 0 (it closes it), or −1 (the
 * caller keeps it: too many clients) */
int roomd_levels_client(int fd);
int roomd_cal_arm(const double *at, int n);
void roomd_cal_cancel(void);
int roomd_cal_json(char *b, size_t len);
long long roomd_clock_ns(void);

static inline double page_auto_common(const double *top, int n) {
    double m = -120;
    for (int i = 0; i < n; i++) if (top[i] > m) m = top[i];
    if (m <= -100) return -3;
    const double a = -m;
    return a > -3 ? -3 : a < -15 ? -15 : a;
}
#endif
