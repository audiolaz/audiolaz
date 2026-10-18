/* src/player/upnp.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_UPNP_H
#define AUDIOLAZ_UPNP_H
#include <stddef.h>
#include <stdint.h>

void ulog(const char *fmt, ...);
double umono(void);

/* ── The queue (src_upnp.c) ─────────────────────────────────────────────────────────────────────────────────────── */
/* A new generation: everything queued or held dropped (Stop, Seek, another track), the play cut at once when it reads
 * the input (src_cut). Returns it; the decoder's pushes of an older one are refused. */
unsigned uq_flush(void);
unsigned uq_gen(void);
/* A run of music starts: track seq, its format, its first frame's place in the track (a seek). 0, or −1 (another gen). */
int uq_segment(unsigned gen, int seq, unsigned rate, unsigned ch, uint64_t first_frame);
/* frames × ch words of the last segment's; blocks while the queue is full; 0, or −1 (another generation now). */
int uq_push(unsigned gen, const int32_t *pcm, long frames);
/* The decoder has nothing more for now (the track's end, no next one given yet). */
void uq_done(unsigned gen);
typedef struct {
    int seq;                 /* the track the reader is in (−1: none) */
    uint64_t frame;          /* its next frame's place in that track */
    unsigned rate;
    int drained;             /* done and all of it read */
    int any;                 /* anything queued since the flush */
    uint64_t first;          /* where that segment starts in its track (a seek's place) */
} uq_at;
void uq_where(uq_at *a);
void uq_pause(int on);
void uq_started(void);       /* a Play: the input started now (the one that started last plays) */
void uq_wake(void);          /* roomd's main loop woken (src_event_fds) */
int uq_reading(void);        /* the player reads the input now (its capture open) */

/* ── The decoder (upnp_dec.c) ───────────────────────────────────────────────────────────────────────────────────── */
typedef struct {
    unsigned gen;
    int seq;
    char uri[2048];
    char mime[160];          /* the DIDL's protocolInfo's, or "" */
    double seek_s;           /* where to start (s) */
} ud_job;
void ud_init(void);
void ud_start(const ud_job *j);
/* The format last opened, for the status and GetMediaInfo ("FLAC 96 kHz 24 bits"), and the track's length (s, 0 not
 * known). */
void ud_format(char *out, size_t len);
double ud_duration(int seq);
/* A whole small file by HTTP (the album art): 0 with *out (malloc'd), −1. */
int ud_fetch(const char *url, unsigned char **out, size_t *len, size_t max);

/* ── The renderer (upnp.c) ──────────────────────────────────────────────────────────────────────────────────────── */
/* At the decoder's end of a track (gen, seq): the next track to decode on, for gapless (1, *j set), or 0 (none: the
 * decoder waits for a job). Waits while the transport plays and nothing is set next, as long as the queue still holds
 * music: a SetNextAVTransportURI that comes late is still gapless. */
int upnp_next(unsigned gen, int seq, ud_job *j);
/* The decoder could not play track seq (a format refused, unreachable): said to the control point. */
void upnp_failed(unsigned gen, int seq, const char *why);
/* The track's length as the stream says it (s). */
void upnp_duration(int seq, double s);
int upnp_start(const char *rooms);
void upnp_shutdown(void);
int upnp_option(const char *name, const char *value);
void upnp_set_name(const char *name);
int upnp_enabled(void);       /* --upnp on */
void upnp_input_stop(void);   /* another input took over: stopped */
double upnp_gain(void);
int upnp_status(char *out, size_t len);
const char *upnp_udn(void);
#endif
