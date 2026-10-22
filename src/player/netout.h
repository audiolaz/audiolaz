/* src/player/netout.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_NETOUT_H
#define AUDIOLAZ_NETOUT_H
#include <stdint.h>
#include <stddef.h>

/* The HTTP stream server (http_port, 9001) and the SlimProto server (slim_port, 3484: never 3483, Roon's), the UPnP
 * search: threads of their own. 0 when they run (a port refused: −1, the network outputs unavailable). */
int netout_init(int http_port, int slim_port);

int netout_relay_serve(const char *name);
void netout_relay_use(const char *name);
/* An out= value: 1 for a network one ("squeeze:<name or MAC>", "upnp:<UDN>", "airplay:<MAC or name>"), 0 otherwise
 * ("alsa:…", ""). */
int netout_is_net(const char *spec);

int netout_is_airplay(const char *spec);
/* Before netout_init : the AirPlay bridge's config.xml (its devices' AirPlay names; default
 * /var/lib/audiolaz-airplay-out/config.xml), and the player's own name (its AirPlay input's: never an output). */
void netout_airplay_conf(const char *path);
void netout_set_self(const char *name);
/* the player's own UPnP renderer (its UDN, upnp.c): never one of its outputs (it would play into itself). */
void netout_set_self_upnp(const char *udn);
/* the AirPlay input's name now, when it follows the output (roomd.c airplay_tick): its own too, never an output;
 * and whether another AirPlay device the bridge knows already has a name (1: taken). */
void netout_set_self2(const char *name);
int netout_airplay_taken(const char *name);
/* Before netout_init (08/10, review): 1 when the AirPlay bridge is installed (--airplay-out on): only then is UDP
 * 127.0.0.1:3483, Roon's Squeezebox port, taken to answer the bridge's discovery. */
void netout_airplay_on(int on);
/* Before netout_init (10/10, the Mac player): the bridge's discovery answered on this IPv4 address (the Mac's own on its
 * network, the bridge started with -s <it>:<slim_port>) instead of 127.0.0.1, which Room for Mac's own relay may hold.
 * NULL or "": the loopback. */
void netout_disc_addr(const char *ip);
/* Before netout_init (09/10, Room for Mac): 1 for this machine's own receivers only (the AirPlay bridge beside it): its
 * ports on the loopback, no UPnP search. */
void netout_local_only(int on);
/* The AirPlay bridge's way here : 1 when the player answers its discovery on UDP 127.0.0.1:3483, 0 when that port
 * is held by another program (Roon Server here, alone on it), −1 not tried (no bridge, no SlimProto port). */
int netout_airplay_ready(void);
/* The next streams' start : 1 for an AirPlay video play (a Squeezebox player, the AirPlay bridge: told to start
 * on 8 KB and a tenth of a second, not 32 KB and half a second), 0 as before. */
void netout_low_latency(int on);
/* What the chosen receiver holds unplayed at the least : the AirPlay bridge's RAOP latency (its read_ahead + 250
 * ms), 0 for the others. ms. */
int netout_hold_floor_ms(void);
int netout_hold_floor_ms_for(const char *spec);   /* the same for an out= value */
/* The receiver the next netout_open plays to (an out= value; "" for none). */
void netout_select(const char *spec);
/* The highest rate the chosen receiver takes (its HELO's MaxSampleRate; 0: not said). */
unsigned netout_max_rate(void);
/* A new stream at this rate to the chosen receiver: it is told to play our URL and has connected to it: 0; −1 (not
 * found, refused, or it did not come within 10 s: tried again by the caller). A stream still finishing on the receiver
 * (the last one's tail) is let through first. */
int netout_open(unsigned rate);
/* Frames (32-bit stereo, as the DAC's) taken: whole ones; 0: the receiver's buffer is full (wait: netout_wait); −1:
 * the receiver lost (its connection gone and not back, it stopped answering or stalled for 10 s). */
long netout_write(const int32_t *lr, long frames);
void netout_wait(int ms);
/* This stream's frames the network took, and those not yet played by the receiver (its buffer: the Squeezebox player's
 * own elapsed time, the renderer's position; an estimate between its reports). */
uint64_t netout_sent(void);
long netout_held(void);

int netout_lost(void);
/* 1 once the receiver has pushed back this stream (its buffer full: from then on it paces us). */
int netout_was_full(void);
/* 1 once the receiver plays this stream (a renderer seen PLAYING; a Squeezebox player's STMs for it). */
int netout_started(void);
/* 1 when the last netout_open that failed had asked the receiver (refused, never fetched): a rate it may not take */
int netout_open_asked(void);
/* The next stream to a UPnP renderer: what it played, the time since it plays set right by its position bit by
 * bit (netout.c), not its position as read (whole seconds, read late: jumps). For music held (roomd.c). */
void netout_smooth_position(int on);
/* 1 once what it played is known well enough to keep the ring's level on (smoothed: ten positions read, or
 * 30 s of playing for one whose position does not move). */
int netout_settled(void);
/* The music paused (1) or going on (0): the receiver told (strm p / u; Pause / Play). */
void netout_pause(int on);
/* All the music sent: its end of file given now (the receiver knows the end), the play still following what it plays. */
void netout_end(void);
/* The stream ends. finish: the music ended there (the receiver plays out what it holds, its connection closed as an
 * end of file); 0: stopped or flushed (the receiver told to stop at once). */
void netout_close(int finish);
/* Why the last open or write failed, for the journal. */
const char *netout_why(void);
/* The chosen receiver failed and is not asked again yet: the seconds left (0: it may be), its name. A receiver
 * that fails before playing anything is asked again after 2, 5, 15, 30, then 60 s; an AirPlay device that left during
 * a stream (the bridge lost it, crashed or restarted) not before 35 s (the session it may still hold). */
double netout_retry_in(char *name, size_t len);
/* For the page: {"receivers":[…]} items (kind, id, name, model, addr, maxRate), and the chosen one's name ("" when it is
 * not there now). */
void netout_receivers_json(char *out, size_t len);
void netout_out_name(const char *spec, char *out, size_t len);   /* spec: the out= read */

void netout_out_dac(const char *spec, char *out, size_t len);
/* 1: an out=audiolaz:<key> naming no receiver heard and no address; heard: those heard ("" none) */
int netout_az_unknown(const char *spec, char *heard, size_t hl);
void netout_out_hw(const char *spec, char *out, size_t len);
void netout_out_longname(const char *spec, char *out, size_t len);
/* the UPnP renderers searched again now (a voie waiting for one that is off: every 10 s, not 30) */
void netout_search_soon(void);
/* a renderer by its description's URL (its last known one: no search needed), and that URL ("" none) */
void netout_upnp_probe(const char *loc);
void netout_upnp_loc(const char *spec, char *out, size_t len);

void netout_set_name(const char *name);
void netout_claims(int port, const char *to);
void netout_bound(const char *spec);
int netout_used_by(const char *spec, char *name, size_t len);
int netout_taken(const char *spec, char *rname, size_t rl, char *by, size_t bl);
void netout_players_json(char *out, size_t len);
int netout_who(int ms, char *out, size_t len);
void netout_claim_ignore(int on);

enum { NETOUT_NP_ROON, NETOUT_NP_AIRPLAY, NETOUT_NP_SPOTIFY, NETOUT_NP_UPNP, NETOUT_NP_N };   /* UPnP : the player's own input's DIDL */
void netout_np_text(int src, const char *title, const char *artist, const char *album);
void netout_np_cover(int src, const void *img, size_t len);
void netout_np_clear(int src);
void netout_np_source(int src);
void netout_np_flash(const char *title, const char *sub);

void netout_np_idle(const char *title, const char *sub);
void netout_np_tick(void);
/* an audiolaz receiver whose DAC does not take the stream's rate as it is: the rate it is sent at, converted
 * here (netout.c, azsrc.h); 0 none (the stream as it is, or another kind of output) */
unsigned netout_link_rate(void);
/* the tests': 0, never converted, never offered (a sender before 21/10); 1 the default */
void netout_link_convert(int on);
/* the audiolaz receiver spec names: its DAC refuses to start (its INFO said it, it refused a stream for it, or
 * it stopped one for it), until a stream to it opens again: 1 */
int netout_dac_start(const char *spec);
/* where the receivers' last addresses are kept by their ids (<dir>/link-known: roomd's rooms folder) */
void netout_state_dir(const char *dir);
void netout_np_cli_port(int port);
int netout_np_http(int c, const char *req, size_t n, const char *method, const char *path, const char *q);
/* The sender's volume on the device : the AirPlay input to a network output with a volume of its own.
 *   netout_dev_volume_kind: 1 an AirPlay device (the bridge, volume_mode 2), 2 a UPnP renderer with a RenderingControl,
 *     0 none (a Squeezebox player, a renderer without one or not seen, a card).
 *   netout_dev_volume: the volume asked (dB −30…0, ≤ −100 muted) for that out=; set by a thread of its own, never up at
 *     once (8 dB/s), from the renderer's own volume read (an AirPlay device's: what this player last set, else −30 dB).
 *   netout_dev_volume_new: a new AirPlay session (the renderer's volume read again).
 *   netout_dev_volume_said: where it is, for /state. */
int netout_dev_volume_kind(const char *spec);
void netout_dev_volume(const char *spec, double db);
void netout_dev_volume_new(void);
/* the ceiling: never above this (dB); NAN: the device's volume at the session's start (UPnP: read; AirPlay: what
 * this player last set on it, else never raised at all). */
void netout_dev_volume_max(double db);
void netout_dev_volume_said(char *out, size_t len);
/* audiolaz Link, protocol 3 (anet.h):
 *   netout_live: the next streams' source (and this one's from now): 1 live (AirPlay, Spotify, the driver, the UPnP
 *     input, any Loopback source: no music ahead of the player's ring, so the receiver's ring grows only with silence,
 *     at the start or in a silence), 0 read ahead (Roon's Squeezebox stream: it grows by sending faster).
 *   netout_stop_reason: why the next netout_close (ANET_WHY_*, a text for the journals; told to the receiver).
 *   netout_link_extra_ms(_for): what the receiver's ring holds now beyond its start (ms; 0 for another kind): the
 *     latency the network asked for, for the lip sync's estimates and the writer's hold in video.
 *   netout_link_json: the receiver of that out=, for the page's /state (null when nothing is known of it). */
enum { NETOUT_WHY_END = 1, NETOUT_WHY_USER = 2, NETOUT_WHY_RATE = 3, NETOUT_WHY_SETTINGS = 4, NETOUT_WHY_SOURCE = 5,
       NETOUT_WHY_LOST = 6, NETOUT_WHY_QUIT = 9, NETOUT_WHY_PAUSE = 10, NETOUT_WHY_OUTPUT = 11 };   /* anet.h's ANET_WHY_* */
void netout_live(int on);
/* the source read at the output's pace (src_paced: a card, its app on the DAC's clock): an audiolaz receiver's
 * ring pulled harder (anet_paced). netout_link_pace: the stream to an audiolaz receiver now, its pace said (the voie's
 * clock line): its DAC's clock against ours and the ring's pull (ppm), the ring, its target and anchor (ms); 0, or −1
 * none. */
void netout_paced(int on);
int netout_link_pace(double *dac_ppm, double *pull_ppm, double *ring_ms, double *target_ms, double *anchor_ms);
/* the audiolaz receiver's underruns this stream (what it heard missing), 0 none or no such stream */
long netout_link_gaps(void);
void netout_fifo_ms(int ms);
/* the audiolaz Link packets' thread's priority (anet_rt): Linux SCHED_FIFO level, macOS any > 0 a Mach time
 * constraint; 0 normal. Default 60. */
void netout_rt(int prio);
void netout_stop_reason(int why, const char *text);
double netout_link_extra_ms(void);
double netout_link_extra_ms_for(const char *spec);
int netout_link_json(const char *spec, char *out, size_t len);

int netout_busy(const char *spec, char *by, size_t len);
void netout_take(void);
#endif
