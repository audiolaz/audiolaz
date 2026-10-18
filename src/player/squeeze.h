/* audiolaz-roomd's Squeezebox input : the engine as a player Roon streams to (engine/alsa/squeeze.c). */
#ifndef AUDIOLAZ_SQUEEZE_H
#define AUDIOLAZ_SQUEEZE_H
#include <stdint.h>

enum { SQ_STOPPED, SQ_PLAYING, SQ_PAUSED };

/* Connects to the server (an address, or "auto": the first to answer on the network) as a player of this name, and
 * stays connected (a thread of its own, reconnecting). 0 when the thread runs. */
int sq_start(const char *server, const char *name);

void sq_mac_seed(const char *seed);
/* The rate of the next frame to play (0: nothing waiting). */
unsigned sq_next_rate(void);
/* Up to `frames` frames (32-bit, stereo) of music at `rate`; 0 when none is there yet or the next is at another rate.
 * *track: 1 when the block begins a track (its first frame is the block's first). */
long sq_read(int32_t *dst, long frames, unsigned rate, int *track);
int sq_state(void);
/* The frames read and not played given back (a pause letting the DAC go): played on the restart. */
void sq_unread(long frames);
/* The next frame's place in its stream, in seconds from the stream's start (−1: no stream, or one not told yet, its
 * first frame next): a play that starts again inside a stream goes on telling Roon the elapsed time from there . */
double sq_position(void);
/* The elapsed time heard in the track now playing, −1 until it has started (its STMs): the measurement's position . */
double sq_heard_position(void);
/* Changes at every stop or flush: what was decoded or played before is to be dropped. */
unsigned sq_generation(void);
/* Nothing more will come: every stream read to its end, its music all read here. */
int sq_ended(void);
/* The engine tells: a track's first frame went to the DAC (STMs); the playing track's elapsed time; everything played
 * (STMu). */
void sq_started(void);
void sq_elapsed(double seconds);
void sq_finished(void);
/* Roon's volume (1 when it is fixed). */
double sq_volume(void);
/* For the status and the page : 1 while the connection to the server's port 3483 is up; the last stream's
 * format since the start ("pcm", "flac", "flac-refused": FLAC with no libFLAC; "" before any); the player's name. */
int sq_connected(void);
const char *sq_format(void);
const char *sq_name(void);

int sq_measure(const char *host, int port, const char *path, int loop, int corrected);

int sq_measure_corrected(void);
void sq_measure_stop(void);
/* Another input of the player started : the server's music paused there, the server told. */
void sq_pause_other(void);
int sq_measuring(void);
#endif
