/* src/player/anet.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_ANET_H
#define AUDIOLAZ_ANET_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define ANET_MAGIC 0x314E5A41u      /* "AZN1" */
#define ANET_RMAGIC 0x31525A41u     /* "AZR1" */
#define ANET_DMAGIC 0x32445A41u     /* "AZD2" */
#define ANET_IMAGIC 0x32495A41u     /* "AZI2" */
#define ANET_RMAGIC3 0x33525A41u    /* "AZR3": protocol 3's longer reports */
#define ANET_VERSION 2              /* the header's version: 2 for ever (protocol 3 is told by the capabilities) */
#define ANET_PROTO 3                /* the protocol's own number (mDNS TXT v=, INFO v=) */
#define ANET_PORT 4890
#define ANET_SERVICE "_audiolaz._tcp.local"
#define ANET_MAX_RATE 1536000u
#define ANET_HDR 32
#define ANET_REP 40
#define ANET_REP3 80
#define ANET_DHDR 24
#define ANET_INFO 192
#define ANET_PKT_FRAMES 176         /* 24 + 176 × 8 = 1432 bytes: one Ethernet frame (MTU 1500), never fragmented */
#define ANET_PKT_MAX_US 2000
#define ANET_REPORT_MS 50
#define ANET_RING_MS 50             /* the receiver's ring target by default (the DAC's 10 ms buffer besides) */
#define ANET_FIFO_MS 150            /* the sender's own queue, before its timer: what roomd may give ahead */
#define ANET_RING_MAX_MS 500        /* protocol 3: the receiver's ring at most, by default (--ring-max-ms) */
#define ANET_PING_MS 500            /* protocol 3: the player's sign of life */
#define ANET_ASK_MS 1000            /* no report this long: each ping is a question (the receiver answers it at once) */
#define ANET_LOST_MS 5000           /* questions unanswered this long: the receiver lost */
#define ANET_SILENT_MS 10000        /* the receiver: no ping nor packet this long (not paused): the player lost */
#define ANET_META_MAX 4096
#define ANET_KEEP_MS 5000           /* the DAC the last player's this long after its last sign of life */
#define ANET_HELLO_MAX 512          /* ANET_M_HELLO's payload at most */
#define ANET_BMAGIC 0x31425A41u     /* "AZB1": who plays, after a refusal ANET_E_BUSY (ANET_INFO bytes) */

enum { ANET_F_S32 = 1, ANET_F_S24_IN_S32 = 2, ANET_F_DOP = 3 };
enum { ANET_M_DATA = 1, ANET_M_END = 2, ANET_M_STOP = 3, ANET_M_PAUSE = 4, ANET_M_RESUME = 5, ANET_M_PING = 6, ANET_M_META = 7,
       ANET_M_HELLO = 8 };          /* who the player is (ANET_CAP_WHO), right after its header */
enum { ANET_R_ACCEPTED = 1, ANET_R_REFUSED = 2, ANET_R_PLAYING = 4, ANET_R_DRAINED = 8, ANET_R_PAUSED = 16, ANET_R_FAILED = 32,
       ANET_R_P3 = 64,              /* protocol 3: the receiver follows the sender's capabilities (ANET_REP3 next) */
       ANET_R_STOPPED = 128 };      /* the receiver ends the stream itself (the reason in the report) */
enum { ANET_CAP_STAMP = 1, ANET_CAP_ADAPT = 2, ANET_CAP_STATE = 4, ANET_CAP_P3 = 7,
       ANET_CAP_WHO = 8,            /* one DAC, one player: its HELLO follows the header */
       ANET_CAP_ALL = 15 };
/* the player's state (ANET_M_PING), the receiver's (its reports) */
enum { ANET_SS_STARTING = 1, ANET_SS_PLAYING = 2, ANET_SS_PAUSED = 3, ANET_SS_ENDING = 4, ANET_SS_LIVE = 0x100 };   /* LIVE: or'ed, a live source */
enum { ANET_RS_IDLE = 0, ANET_RS_FILLING = 1, ANET_RS_PLAYING = 2, ANET_RS_PAUSED = 3, ANET_RS_DRAINING = 4, ANET_RS_DRAINED = 5,
       ANET_RS_DAC_REFUSED = 6, ANET_RS_DAC_FAILED = 7, ANET_RS_STOPPED = 8 };
/* why a stream stops, either side */
enum { ANET_WHY_NONE = 0, ANET_WHY_END = 1, ANET_WHY_USER = 2, ANET_WHY_RATE = 3, ANET_WHY_SETTINGS = 4, ANET_WHY_SOURCE = 5,
       ANET_WHY_LOST = 6, ANET_WHY_TAKEN = 7, ANET_WHY_DAC = 8, ANET_WHY_QUIT = 9, ANET_WHY_PAUSE = 10, ANET_WHY_OUTPUT = 11,
       ANET_WHY_CLOSED = 12, ANET_WHY_AGAIN = 13, ANET_WHY_BACKOFF = 14, ANET_WHY_N = 15 };
/* ANET_WHY_BACKOFF: the receiver's guard stopped the stream to keep its machine up (audiolaz-recv.c: a thread
 * of its own looping, or stuck, or the cores starved); the reason at length in the receiver's journal */
static inline const char *anet_why_text(unsigned w) {
    static const char *const T[ANET_WHY_N] = { "no reason given", "the music's end", "stopped or flushed by the source",
        "another rate next", "the settings changed", "the source stopped or changed", "the other side silent",
        "taken by another sender", "the DAC failed or refused", "the program ends", "paused too long: let go",
        "another output chosen", "the connection closed without a word", "the same player again",
        "the receiver backed off: its machine first" };
    return w < ANET_WHY_N ? T[w] : "an unknown reason";
}
static inline const char *anet_rs_text(unsigned s) {
    static const char *const T[] = { "idle", "filling", "playing", "paused", "draining", "drained", "DAC refused", "DAC failed", "stopped" };
    return s < sizeof T / sizeof T[0] ? T[s] : "?";
}
enum { ANET_E_NONE = 0, ANET_E_HEADER = 1, ANET_E_RATE = 2, ANET_E_DAC = 3, ANET_E_TAKEN = 4,
       ANET_E_BUSY = 5 };           /* another player plays (its name after the report: ANET_BMAGIC) */

typedef struct { uint32_t id, rate, pkt_frames, pkt_us, caps; uint16_t ch, fmt; } anet_hdr;
typedef struct { uint32_t id, flags, dac_rate, ring, reason; uint64_t played, received;
                 uint32_t target, late_us, peak_us, underruns, adjusts, ring_min, ring_max, state, why, seq; int v3; } anet_rep;

static inline void anet_p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline uint32_t anet_g32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void anet_p64(uint8_t *p, uint64_t v) { anet_p32(p, (uint32_t)v); anet_p32(p + 4, (uint32_t)(v >> 32)); }
static inline uint64_t anet_g64(const uint8_t *p) { return anet_g32(p) | (uint64_t)anet_g32(p + 4) << 32; }

/* The packets' rhythm at a rate: frames a packet (nominal) and the interval, µs. */
static inline void anet_pkt_plan(uint32_t rate, uint32_t *frames, uint32_t *us) {
    uint32_t f = (uint32_t)((uint64_t)rate * ANET_PKT_MAX_US / 1000000);
    if (f > ANET_PKT_FRAMES) f = ANET_PKT_FRAMES;
    if (f < 1) f = 1;
    *frames = f;
    *us = (uint32_t)((uint64_t)f * 1000000 / (rate ? rate : 1));
}

static inline void anet_hdr_pack(uint8_t b[ANET_HDR], const anet_hdr *h) {
    memset(b, 0, ANET_HDR);
    anet_p32(b, ANET_MAGIC); anet_p32(b + 4, ANET_VERSION); anet_p32(b + 8, h->id); anet_p32(b + 12, h->rate);
    b[16] = (uint8_t)h->ch; b[17] = (uint8_t)(h->ch >> 8); b[18] = (uint8_t)h->fmt; b[19] = (uint8_t)(h->fmt >> 8);
    anet_p32(b + 20, h->caps); anet_p32(b + 24, h->pkt_frames); anet_p32(b + 28, h->pkt_us);
}
/* 0, or ANET_E_HEADER / ANET_E_RATE; rate 0 (INFO) is 0 too */
static inline int anet_hdr_parse(const uint8_t b[ANET_HDR], anet_hdr *h) {
    if (anet_g32(b) != ANET_MAGIC || anet_g32(b + 4) != ANET_VERSION) return ANET_E_HEADER;
    h->id = anet_g32(b + 8); h->rate = anet_g32(b + 12);
    h->ch = (uint16_t)(b[16] | b[17] << 8); h->fmt = (uint16_t)(b[18] | b[19] << 8); h->caps = anet_g32(b + 20);
    h->pkt_frames = anet_g32(b + 24); h->pkt_us = anet_g32(b + 28);
    if (h->rate == 0) return 0;
    if (h->ch != 2 || h->fmt < ANET_F_S32 || h->fmt > ANET_F_DOP) return ANET_E_HEADER;
    if (h->rate < 8000 || h->rate > ANET_MAX_RATE) return ANET_E_RATE;
    return 0;
}
static inline void anet_msg_pack(uint8_t b[8], uint32_t type, uint32_t len) { anet_p32(b, type); anet_p32(b + 4, len); }

static inline void anet_meta_get(const char *m, const char *key, char *out, size_t len) {
    const size_t k = strlen(key);
    out[0] = 0;
    for (const char *l = m; l && *l; ) {
        const char *e = strchr(l, '\n'); const size_t n = e ? (size_t)(e - l) : strlen(l);
        if (n > k && !strncmp(l, key, k) && l[k] == '=') { size_t v = n - k - 1; if (v >= len) v = len - 1; memcpy(out, l + k + 1, v); out[v] = 0; return; }
        l = e ? e + 1 : NULL;
    }
}

static inline size_t anet_hello_pack(uint8_t *b, size_t len, const char *uid, const char *name, int take) {
    if (len < 8 + 64) return 0;
    char *p = (char *)b + 8;
    const size_t room = len - 8 < ANET_HELLO_MAX ? len - 8 : ANET_HELLO_MAX;
    size_t n = 0;
    const char *const K[2] = { "player=", "name=" }, *const V[2] = { uid ? uid : "", name ? name : "" };
    for (int k = 0; k < 2; k++) {
        for (const char *c = K[k]; *c && n + 9 < room; c++) p[n++] = *c;
        size_t m = 0;   /* an id up to 39 bytes, a name up to 95 (the receiver's) */
        for (const unsigned char *c = (const unsigned char *)V[k]; *c && n + 9 < room && m < (k ? 95u : 39u); c++, m++) p[n++] = *c < 0x20 || *c == ';' || *c == 0x7f ? ' ' : (char)*c;
        p[n++] = '\n';
    }
    memcpy(p + n, take ? "take=1\n" : "take=0\n", 7); n += 7;
    while (n % 8 && n < room) p[n++] = ' ';
    anet_msg_pack(b, ANET_M_HELLO, (uint32_t)n);
    return 8 + n;
}
/* flags: 0, or (protocol 3, ANET_CAP_STAMP) the time the packet was due to leave, µs modulo 2^32 */
static inline void anet_dhdr_pack(uint8_t b[ANET_DHDR], uint32_t id, uint64_t index, uint32_t frames, uint32_t flags) {
    anet_p32(b, ANET_DMAGIC); anet_p32(b + 4, id); anet_p64(b + 8, index); anet_p32(b + 16, frames); anet_p32(b + 20, flags);
}
static inline int anet_dhdr_parse(const uint8_t *b, size_t n, uint32_t *id, uint64_t *index, uint32_t *frames) {
    if (n < ANET_DHDR || anet_g32(b) != ANET_DMAGIC) return -1;
    *id = anet_g32(b + 4); *index = anet_g64(b + 8); *frames = anet_g32(b + 16);
    return (size_t)*frames * 8 + ANET_DHDR == n ? 0 : -1;
}
static inline uint32_t anet_dhdr_flags(const uint8_t *b) { return anet_g32(b + 20); }
/* r->v3: the longer report (ANET_REP3 bytes, AZR3); else ANET_REP (AZR1). Returns the bytes packed. */
static inline size_t anet_rep_pack(uint8_t *b, const anet_rep *r) {
    anet_p32(b, r->v3 ? ANET_RMAGIC3 : ANET_RMAGIC); anet_p32(b + 4, r->id); anet_p32(b + 8, r->flags); anet_p32(b + 12, r->dac_rate);
    anet_p64(b + 16, r->played); anet_p64(b + 24, r->received); anet_p32(b + 32, r->ring); anet_p32(b + 36, r->reason);
    if (!r->v3) return ANET_REP;
    anet_p32(b + 40, r->target); anet_p32(b + 44, r->late_us); anet_p32(b + 48, r->peak_us); anet_p32(b + 52, r->underruns);
    anet_p32(b + 56, r->adjusts); anet_p32(b + 60, r->ring_min); anet_p32(b + 64, r->ring_max);
    anet_p32(b + 68, (r->state & 0xFFFFu) | (r->why & 0xFFFFu) << 16); anet_p32(b + 72, r->seq); anet_p32(b + 76, 0);
    return ANET_REP3;
}
/* The report's length from its magic (its first 4 bytes): ANET_REP, ANET_REP3, or 0 (not a report) */
static inline size_t anet_rep_len(const uint8_t *b) { const uint32_t m = anet_g32(b); return m == ANET_RMAGIC ? ANET_REP : m == ANET_RMAGIC3 ? ANET_REP3 : 0; }
static inline int anet_rep_parse(const uint8_t *b, anet_rep *r) {
    const size_t n = anet_rep_len(b);
    if (!n) return -1;
    memset(r, 0, sizeof *r);
    r->id = anet_g32(b + 4); r->flags = anet_g32(b + 8); r->dac_rate = anet_g32(b + 12);
    r->played = anet_g64(b + 16); r->received = anet_g64(b + 24); r->ring = anet_g32(b + 32); r->reason = anet_g32(b + 36);
    if (n == ANET_REP3) {
        r->v3 = 1; r->target = anet_g32(b + 40); r->late_us = anet_g32(b + 44); r->peak_us = anet_g32(b + 48); r->underruns = anet_g32(b + 52);
        r->adjusts = anet_g32(b + 56); r->ring_min = anet_g32(b + 60); r->ring_max = anet_g32(b + 64);
        const uint32_t sw = anet_g32(b + 68); r->state = sw & 0xFFFFu; r->why = sw >> 16; r->seq = anet_g32(b + 72);
    }
    return 0;
}

/* ── mDNS (anet_mdns.c) ── */

typedef struct { char id[64], dac[96], addr[48], hw[96]; int port; unsigned max_rate; double seen; } anet_peer;
/* Receiver: answer for this service until the process ends (a thread). */
int anet_mdns_publish(const char *id, const char *dac, int port, unsigned max_rate);

void anet_mdns_set_hw(const char *hw);
/* Player: one query, answers collected for ms; up to max peers. Returns how many. */
int anet_mdns_browse(anet_peer *out, int max, int ms);
/* The same, and this IPv4 address asked straight (its port 5353) too (NULL: none). */
int anet_mdns_browse2(anet_peer *out, int max, int ms, const char *ask_ip);
/* Building and reading the packets (unit tests). */
size_t anet_mdns_query(uint8_t *b, size_t len, uint16_t qid);
size_t anet_mdns_answer(uint8_t *b, size_t len, uint16_t qid, const char *id, const char *dac, int port, unsigned max_rate, uint32_t ipv4_be, uint32_t ttl);
int anet_mdns_is_query_for_us(const uint8_t *b, size_t n, const char *id, uint16_t *qid);
int anet_mdns_parse_answer(const uint8_t *b, size_t n, anet_peer *p);

/* ── The sender (anet_send.c), driven by netout.c ── */
int anet_open(const char *host, int port, unsigned rate, int fmt, unsigned hold_ms, char *why, size_t wl);

void anet_identity(const char *uid, const char *name);
void anet_take(int on);
int anet_busy(char *by, size_t len);
/* protocol 3: the source of the next streams (and of this one, from now): 1 live (no music ahead of the
 * player's ring: the receiver's ring grows only with silence), 0 read ahead (it grows by sending faster). Default 0.
 * anet_caps: what the next anet_open offers (ANET_CAP_*; the tests: 0 is a version-2 sender). */
void anet_live(int on);
void anet_fifo_ms(int ms);              /* the queue (ms; 0: ANET_FIFO_MS) */
void anet_caps(unsigned caps);
/* the packets' thread's priority from the next anet_open: Linux, SCHED_FIFO at prio (0: normal); macOS, any
 * prio > 0 a Mach time constraint on the packets' period (rt_thread.h), 0 normal. Default 60. */
void anet_rt(int prio);
/* The stream now (protocol 3 or not): 0 when none. ms: the receiver's ring (nominal: what was sent less what its DAC
 * read), the target it asks for, the anchor held, all held unheard (queue, network, ring, DAC: the output's latency);
 * the jitter it measures (last second, peak remembered); the silence put in / left out (ms, live sources). */
typedef struct {
    int on, p3, adapt, live;            /* a stream; protocol 3 there; the ring adapting; a live source */
    unsigned rate, rstate, rwhy;        /* the stream's rate; the receiver's state (ANET_RS_*), its last stop's reason */
    double ring_ms, target_ms, start_ms, anchor_ms, held_ms, late_ms, peak_ms, ins_ms, lo_ms, hi_ms;
    unsigned long underruns, adjusts;
    unsigned long tx_late; double tx_late_ms;   /* this stream: the sender's own delays over 50 ms, the worst (ms) */
    char last_stop[200]; double last_stop_age;   /* the last stream's stop to this receiver, said (who, why), its age (s; <0 none) */
} anet_state;
/* hostport: the receiver (as anet_open had it, "host:port"); NULL: the stream now. 0, or −1 when nothing known. */
int anet_get_state(const char *hostport, anet_state *s);
/* protocol 3: the reason of the next anet_close (ANET_WHY_*, and a text for the journal: "" none), and what
 * plays (meta lines, anet.h; sent to the receiver when they change: the serial says it) */
void anet_stop_reason(unsigned why, const char *text);
void anet_meta(const char *lines, unsigned serial);
long anet_write(const int32_t *lr, long frames);   /* frames taken; 0 full (wait); −1 lost */
void anet_wait(int ms);
uint64_t anet_sent(void);
long anet_held(void);
int anet_started(void);
int anet_was_full(void);
int anet_active(void);
void anet_pause(int on);
void anet_end(void);
void anet_close(int finish);
const char *anet_why(void);
/* INFO (no stream): the receiver at host:port, its id, DAC name and highest rate: 0, or −1 (not there, ms at most). */
int anet_info(const char *host, int port, anet_peer *p, int ms);
#endif
