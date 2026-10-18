/* src/player/out.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_OUT_H
#define AUDIOLAZ_OUT_H
#include <stddef.h>
#include <stdint.h>

/* The sample formats roomd.c sends a card. */
enum { OUTF_S32 = 0,            /* 32-bit signed integers, native endianness (ALSA's S32_LE on every machine we build on) */
       OUTF_DSD_U32_LE,         /* native DSD, 32 one-bit samples a word (ALSA's DSD_U32_LE) */
       OUTF_DSD_U32_BE };       /* the same, big-endian words */

#define OUT_AGAIN (-11)

typedef struct out_pcm out_pcm;

out_pcm *out_open(const char *dev, int fmt, unsigned rate, unsigned long period, unsigned periods, int quiet);
/* n frames (2 × n words) from pcm: how many it took (0 … n), OUT_AGAIN when it has no room, or another negative value. */
long out_write(out_pcm *o, const int32_t *pcm, unsigned long n);
/* Wait up to ms for room (after OUT_AGAIN). */
void out_wait(out_pcm *o, int ms);
/* The room in its buffer now (frames), or a negative value. */
long out_avail(out_pcm *o);
/* What is written and not heard yet (frames, the device's own latency in it where it says it): 0 with *frames, −1. */
int out_delay(out_pcm *o, long *frames);
/* Its buffer and period as taken (frames). */
void out_params(out_pcm *o, unsigned long *buffer, unsigned long *period);
/* After a negative out_write or out_avail: the device made ready for the next writes, its stream going on (ALSA:
 * an underrun, its buffer run dry, or a suspend: snd_pcm_recover) — 0; −1 when it cannot be (unplugged, taken; macOS
 * always): closed and opened again then. The receiver's (audiolaz-recv.c): a DAC late by its thread, a gap heard, never
 * the stream lost for it. */
int out_recover(out_pcm *o, long e);
/* Stopped at once (what it holds dropped) and closed. NULL: nothing. */
void out_close(out_pcm *o);
/* Whether dev takes this format at this rate: 1 (natively), 0, or −1 when it cannot be asked now (busy); 2 (macOS, 10/10)
 * only resampled: a PCM rate the device does not play, played at another one through out_coreaudio.c's converter.
 * Never 2 for DSD, and DoP (S32 at the DoP rate) needs 1: resampled, its markers and one-bit words would be noise
 * (roomd.c dsd_plan). Cheap enough to call before each play; roomd.c keeps the answers. */
int out_takes(const char *dev, int fmt, unsigned rate);
/* Words for the log. */
const char *out_strerror(long e);
const char *out_fmt_name(int fmt);
/* (macOS) what this play really does, for the log and the status: the device's rate when it is not the stream's (the
 * rate refused, resampled here), 0 otherwise; and the device's own name. Linux: 0 and "". */
unsigned out_device_rate(out_pcm *o);
/* The bits of each 32-bit sample word the device keeps (24: a 24-bit DAC, or Core Audio's float32 on the way;
 * 32: all of them, or not known): roomd.c requantizes to it once, with TPDF dither (pcmclock.h s32_out_q). */
int out_bits(out_pcm *o);
const char *out_device_name(out_pcm *o);

const char *out_device_maker(out_pcm *o);
const char *out_device_model(out_pcm *o);

void out_list_json(char *out, size_t len);
/* out= of a card (the control file, the page's POST /output): OUT_CARD_PREFIX then the device, as out_list_json names it.
 * out_card_ok: a device name the page may set (no character the control file or the JSON cannot hold). */
#ifdef __APPLE__
#define OUT_CARD_PREFIX "ca:"
#else
#define OUT_CARD_PREFIX "alsa:"
#endif
#define OUT_CARD_PREFIX_LEN (sizeof OUT_CARD_PREFIX - 1)
int out_card_ok(const char *dev);
/* Options of this side (roomd's command line, by pairs: --exclusive on, --shared on…): 1 when the name is one of its
 * own (value taken), 0 otherwise. Called before the play starts. */
int out_option(const char *name, const char *value);
/* --list-devices (anywhere on the command line): this machine's outputs for the app (src/player.js), JSON on stdout:
 * [{"name","uid","maxRate","default"}]; then roomd quits. */
void out_print_devices(void);
/* The system's default output when no --dac is given ("plughw:0,0" on Linux, "default" on macOS: the default device). */
const char *out_default_dev(void);
/* The device dev names there now: 1, or 0 (unplugged). Linux: a card named by its id (CARD=<id>) found, any
 * other name (hw:0,0, plughw:…) taken as there; macOS: a device of that UID (or name), "default" and "null" there.
 * roomd.c's AirPlay input is named after the output, after the player alone once the output has been absent a minute. */
int out_dev_present(const char *dev);

#endif
