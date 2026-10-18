/* src/player/player.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_PLAYER_H
#define AUDIOLAZ_PLAYER_H
typedef struct limiter limiter;
static inline long lim_latency(const limiter *l) { (void)l; return 0; }
static inline void lim_track_reset(void) { }
static inline void lim_free(limiter *l) { (void)l; }

typedef struct { volatile long frames, limited, events; volatile double min_gain, peak; } lim_figures;
__attribute__((unused)) static lim_figures lim_now = { 0, 0, 0, 1, 0 }, lim_track = { 0, 0, 0, 1, 0 };

static inline void voies_start(int argc, char **argv) { (void)argc; (void)argv; }
static inline void voies_end(void) { }
#endif
