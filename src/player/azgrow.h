/* src/player/azgrow.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_AZGROW_H
#define AUDIOLAZ_AZGROW_H
#include <stdio.h>

#define AZG_LOW 0.4
#define AZG_HIGH 0.5
#define AZG_UP 1.5
#define AZG_STEP_MS 20.0
#define AZG_GAP_S 0.25
#define AZG_CALM_S 600.0
#define AZG_DOWN_S 60.0

typedef struct {
    double start, size, max;               /* frames: where it starts, now, its ceiling */
    double rate;                           /* frames a second (for ms) */
    double t_sign, t_down;                 /* the last sign (s, the caller's clock), the last step down */
    double low_win;                        /* the lowest level since the last step down (or the last sign) */
    unsigned grows, shrinks, shortfalls;
    double low_ms;                         /* the lowest level this stream (ms) */
    double low_frac;                       /* its sign: under this share of its size (AZG_LOW) */
} azgrow;

static inline void azg_init(azgrow *g, double start, double max, double rate, double now) {
    g->start = start; g->size = start; g->max = max > start ? max : start; g->rate = rate > 0 ? rate : 1;
    g->t_sign = now; g->t_down = now; g->low_win = start; g->grows = g->shrinks = g->shortfalls = 0;
    g->low_ms = start * 1000.0 / g->rate;
    g->low_frac = AZG_LOW;
}
/* The lowest level seen since the last look (frames) and how many shortfalls (callbacks short, writes too late)
 * meanwhile. Returns +1 grown, −1 shrunk, 0 the same; why: the words for the journal when it changed. */
static inline int azg_look(azgrow *g, double low, unsigned shortfalls, double now, char *why, size_t len) {
    const double ms = 1000.0 / g->rate;
    if (low < 0) low = 0;
    if (low * ms < g->low_ms) g->low_ms = low * ms;
    if (low < g->low_win) g->low_win = low;
    g->shortfalls += shortfalls;
    if (shortfalls || low < g->low_frac * g->size) {
        const double was = g->t_sign;
        g->t_sign = now; g->t_down = now; g->low_win = g->size;
        if (now - was < AZG_GAP_S && g->grows) return 0;   /* grown a moment ago: it refills first */
        if (g->size >= g->max) return 0;
        double s = g->size * AZG_UP;
        if (s < g->size + AZG_STEP_MS / ms) s = g->size + AZG_STEP_MS / ms;
        if (s > g->max) s = g->max;
        if (why) {
            if (shortfalls) snprintf(why, len, "it ran short (%u time%s): from %.0f to %.0f ms", shortfalls, shortfalls > 1 ? "s" : "", g->size * ms, s * ms);
            else snprintf(why, len, "it fell to %.1f ms of its %.0f (its writer came late): from %.0f to %.0f ms, before it runs short", low * ms, g->size * ms, g->size * ms, s * ms);
        }
        g->size = s; g->grows++;
        return 1;
    }
    if (g->size > g->start && now - g->t_sign >= AZG_CALM_S && now - g->t_down >= AZG_DOWN_S) {
        const int calm = g->low_win >= AZG_HIGH * g->size;
        g->t_down = now; g->low_win = g->size;
        if (!calm) return 0;
        double s = g->size - (g->size - g->start) / 10.0;
        if (s - g->start < 1.0 / ms) s = g->start;          /* within a millisecond: there */
        if (why) snprintf(why, len, "calm for %.0f min: from %.0f to %.0f ms (back towards its start, %.0f)", (now - g->t_sign) / 60.0, g->size * ms, s * ms, g->start * ms);
        g->size = s; g->shrinks++;
        return -1;
    }
    return 0;
}
#endif
