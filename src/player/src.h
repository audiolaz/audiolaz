/* src/player/src.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_SRC_H
#define AUDIOLAZ_SRC_H
#include <poll.h>
#include <stddef.h>
#include <stdint.h>

#define SRC_MAX 8                      /* inputs at most (LOOP_MAX_SUBS) */
#define SRC_AGAIN (-11)                /* src_read: nothing whole to read now */

#define SRC_ENDED (-1001)
#define SRC_LOST (-1002)
#define SRC_CHANGED (-1003)

/* The inputs' device (--loop: the Loopback's index or id on Linux; on macOS the folder of the FIFOs, "" for
 * <rooms>/in), subs inputs, rooms the player's folder. 0: ready; −1: none (their sources stay silent, said). */
int src_init(const char *card, int subs, const char *rooms);   /* lines to stderr, as roomd.c logs */

const char *src_what(void);
/* Input k: its source playing (1), its rate (0 unknown), when it started (s, comparable between inputs: the one that
 * started last plays), its source's side closed altogether (nobody holding it: shairport-sync stopped, not paused). */
int src_active(int k);
unsigned src_rate(int k);

unsigned src_channels(int k);
double src_started(int k);
int src_closed(int k);

int src_paced(int k);

int src_io_ended(int k);

int src_keeps(int k);
/* The events that wake the main loop when a source starts or stops (the Loopback's control), into fds (max of them):
 * how many; src_drain: read them (after poll). */
int src_event_fds(struct pollfd *fds, int max);
void src_drain(void);
/* The clock steer of input k: 100000 nominal, above it the source's music played slower (a step ≈ 11 ppm). */
void src_shift(int k, long v);
/* An AirPlay sender connected (holding its session, playing or not) to the receiver's port (0: AirPlay's 7000/5000). */
int src_air_connected(int port);

/* The sources this layer runs itself (macOS: the AirPlay receiver, no systemd there). src_owns: input k's
 * source is this layer's (1), so stopping it (its session ended for the sender) is src_stop's, not <rooms>/input-stop's
 * (Linux: install.sh's script restarting the input's service). src_stop: done (1), 0 not this layer's. */
int src_owns(int k);
int src_stop(int k);
/* The AirPlay receiver's metadata FIFO when this layer runs it: asked, roomd's --airplay-meta when given (called before
 * src_init then), NULL otherwise; the path it writes to, NULL when not this layer's (Linux: install.sh's). */
const char *src_air_meta(const char *asked);
/* Its metadata as roomd.c reads it (air_item): each ssnc item's code (pfls, pend, disc: the sender's pause, stop or end,
 * the input stopped at once; conn … disc: a session). */
void src_air_item(uint32_t code);

void src_air_format(const char *sdsc);
/* The name the AirPlay receiver starts with when its settings have none yet (roomd.c's rule, airplay_tick's). */
void src_air_name(const char *name);
/* Lines for roomd's status file ("key=value\n"…) into out: 1 when there are any (macOS: the receiver's state). */
int src_status(char *out, size_t len);

/* Options of this side (roomd's command line, by pairs): 1 when the name is one of its own, 0 otherwise. Called before
 * src_init. src_tick: called about once a second from roomd's main loop (an input's own process to keep running…). */
int src_option(const char *name, const char *value);
void src_tick(void);
/* Stopped at roomd's end (the processes the inputs started). */
void src_shutdown(void);

/* Reading input k: opened at its rate with ch channels (2 … SRC_MAX_CH: as its source opened it, the Loopback making
 * both ends share them), period frames per read, periods of them in its buffer. NULL: refused (that count too). */
#define SRC_MAX_CH 8
typedef struct src_pcm src_pcm;
src_pcm *src_open(int k, unsigned rate, unsigned ch, unsigned long period, unsigned periods);
void src_start(src_pcm *s);
/* Frames waiting (≥ 0), or negative: an overrun (read too late: src_recover), SRC_ENDED, SRC_LOST, SRC_CHANGED. */
long src_avail(src_pcm *s);
/* Up to n frames (the channels it was opened with, interleaved, 32-bit integers, full scale ±2^31) into pcm: how many,
 * SRC_AGAIN, or negative (overrun). */
long src_read(src_pcm *s, int32_t *pcm, unsigned long n);
/* What waits in it (frames) for the latency: 0 with *frames, −1. Negative when its source hands each frame
 * over early by contract (macOS: shairport-sync's stdout backend, its desired buffer before each frame's time), less
 * that much: the latency is then what the AirPlay offset must take, as with the Loopback. */
int src_delay(src_pcm *s, long *frames);
/* After an overrun: started again, what it held dropped. */
void src_recover(src_pcm *s, long err);
/* 1 when the input's channels are no longer those it was opened with (macOS: the AirPlay receiver's stream went
 * from stereo to 5.1 or back within a session, its pipe's 8 channels the same): the play starts again with
 * src_channels, as with a Loopback its source opened again in another count. Linux: 0 (the Loopback's source closes
 * its side first). */
int src_relayout(src_pcm *s);
/* Its descriptors for poll (data to read), into fds: how many. */
int src_poll_fds(src_pcm *s, struct pollfd *fds, int max);
void src_close(src_pcm *s);
const char *src_strerror(long e);

#define UPNP_K 3
int src_inputs(int subs);
int src_builtin(void);
void src_player_name(const char *name);
int src_cut(int k);
int src_paused(int k);
void src_unread(int k, long frames);
double src_volume(int k);
void src_dac(const char *name, const char *maker, const char *model, int local);
void src_latency(int k, double ms);
void src_hold(int k, int ms);

#endif
