/* src/player/anet_mdns.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <arpa/inet.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "anet.h"

#define MDNS_GROUP "224.0.0.251"
#define MDNS_PORT 5353
enum { AM_T_A = 1, AM_T_PTR = 12, AM_T_TXT = 16, AM_T_SRV = 33, AM_T_ANY = 255 };

static int am_put_name(uint8_t *b, size_t len, size_t *o, const char *name) {
    const char *s = name;
    while (*s) {
        const char *d = strchr(s, '.');
        size_t l = d ? (size_t)(d - s) : strlen(s);
        if (l == 0 || l > 63 || *o + 1 + l >= len) return -1;
        b[(*o)++] = (uint8_t)l; memcpy(b + *o, s, l); *o += l;
        s += l; if (*s == '.') s++;
    }
    if (*o >= len) return -1;
    b[(*o)++] = 0;
    return 0;
}
/* The instance's first label as one label (its own dots: as is, a single label of up to 63 bytes) then the service. */
static int am_put_instance(uint8_t *b, size_t len, size_t *o, const char *id) {
    size_t l = strlen(id); if (l > 63) l = 63;
    if (*o + 1 + l >= len) return -1;
    b[(*o)++] = (uint8_t)l; memcpy(b + *o, id, l); *o += l;
    return am_put_name(b, len, o, ANET_SERVICE);
}
static int am_put16(uint8_t *b, size_t len, size_t *o, unsigned v) { if (*o + 2 > len) return -1; b[*o] = (uint8_t)(v >> 8); b[*o + 1] = (uint8_t)v; *o += 2; return 0; }
static int am_put32(uint8_t *b, size_t len, size_t *o, uint32_t v) { return am_put16(b, len, o, v >> 16) || am_put16(b, len, o, v & 0xFFFF); }
static unsigned am_g16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

/* A name at *o (compressed or not) into out, labels joined by '.'; *o after it. −1 malformed. */
static int am_get_name(const uint8_t *b, size_t n, size_t *o, char *out, size_t ol) {
    size_t p = *o, w = 0; int jumped = 0, hops = 0;
    out[0] = 0;
    for (;;) {
        if (p >= n) return -1;
        const uint8_t l = b[p];
        if (l == 0) { if (!jumped) *o = p + 1; break; }
        if ((l & 0xC0) == 0xC0) {
            if (p + 1 >= n || ++hops > 16) return -1;
            if (!jumped) *o = p + 2;
            jumped = 1; p = (size_t)(l & 0x3F) << 8 | b[p + 1];
            continue;
        }
        if (p + 1 + l > n) return -1;
        if (w && w + 1 < ol) out[w++] = '.';
        for (unsigned i = 0; i < l && w + 1 < ol; i++) out[w++] = (char)b[p + 1 + i];
        out[w] = 0;
        p += 1 + l;
    }
    return 0;
}
static void am_host_of(const char *id, char *out, size_t len) {
    size_t w = 0;
    for (const char *s = id; *s && w + 1 < len && w < 40; s++) out[w++] = (*s == '.' || *s == ' ') ? '-' : *s;
    out[w] = 0;
    snprintf(out + w, len - w, "-audiolaz.local");
}

size_t anet_mdns_query(uint8_t *b, size_t len, uint16_t qid) {
    size_t o = 0;
    if (am_put16(b, len, &o, qid) || am_put16(b, len, &o, 0) || am_put16(b, len, &o, 1) || am_put16(b, len, &o, 0) || am_put16(b, len, &o, 0) || am_put16(b, len, &o, 0)) return 0;
    if (am_put_name(b, len, &o, ANET_SERVICE) || am_put16(b, len, &o, AM_T_PTR) || am_put16(b, len, &o, 1)) return 0;
    return o;
}

static char am_HW[96];
void anet_mdns_set_hw(const char *hw) { snprintf(am_HW, sizeof am_HW, "%s", hw ? hw : ""); }
size_t anet_mdns_answer(uint8_t *b, size_t len, uint16_t qid, const char *id, const char *dac, int port, unsigned max_rate, uint32_t ip, uint32_t ttl) {
    size_t o = 0, l0;
    char host[96];
    am_host_of(id, host, sizeof host);
    const int q = qid != 0;                                  /* a legacy query: its question repeated */
    if (am_put16(b, len, &o, qid) || am_put16(b, len, &o, 0x8400) || am_put16(b, len, &o, q) || am_put16(b, len, &o, 1) || am_put16(b, len, &o, 0) || am_put16(b, len, &o, 3)) return 0;
    if (q && (am_put_name(b, len, &o, ANET_SERVICE) || am_put16(b, len, &o, AM_T_PTR) || am_put16(b, len, &o, 1))) return 0;
    /* PTR service → instance */
    if (am_put_name(b, len, &o, ANET_SERVICE) || am_put16(b, len, &o, AM_T_PTR) || am_put16(b, len, &o, 1) || am_put32(b, len, &o, ttl)) return 0;
    l0 = o; if (am_put16(b, len, &o, 0) || am_put_instance(b, len, &o, id)) return 0;
    b[l0] = (uint8_t)((o - l0 - 2) >> 8); b[l0 + 1] = (uint8_t)(o - l0 - 2);
    /* SRV instance → host:port (cache-flush bit on the unique records) */
    if (am_put_instance(b, len, &o, id) || am_put16(b, len, &o, AM_T_SRV) || am_put16(b, len, &o, 0x8001) || am_put32(b, len, &o, ttl)) return 0;
    l0 = o; if (am_put16(b, len, &o, 0) || am_put16(b, len, &o, 0) || am_put16(b, len, &o, 0) || am_put16(b, len, &o, (unsigned)port) || am_put_name(b, len, &o, host)) return 0;
    b[l0] = (uint8_t)((o - l0 - 2) >> 8); b[l0 + 1] = (uint8_t)(o - l0 - 2);
    /* TXT */
    if (am_put_instance(b, len, &o, id) || am_put16(b, len, &o, AM_T_TXT) || am_put16(b, len, &o, 0x8001) || am_put32(b, len, &o, ttl)) return 0;
    l0 = o; if (am_put16(b, len, &o, 0)) return 0;
    char kv[5][128];
    snprintf(kv[0], sizeof kv[0], "v=%d", ANET_PROTO);   /* 3: nobody reads it to refuse */
    snprintf(kv[1], sizeof kv[1], "id=%.100s", id);
    snprintf(kv[2], sizeof kv[2], "dac=%.100s", dac);
    snprintf(kv[3], sizeof kv[3], "max=%u", max_rate);
    snprintf(kv[4], sizeof kv[4], "hw=%.95s", am_HW);   /* the DAC's identity (anet.h anet_peer) */
    for (int i = 0; i < (am_HW[0] ? 5 : 4); i++) {
        const size_t k = strlen(kv[i]);
        if (o + 1 + k > len) return 0;
        b[o++] = (uint8_t)k; memcpy(b + o, kv[i], k); o += k;
    }
    b[l0] = (uint8_t)((o - l0 - 2) >> 8); b[l0 + 1] = (uint8_t)(o - l0 - 2);
    /* A */
    if (am_put_name(b, len, &o, host) || am_put16(b, len, &o, AM_T_A) || am_put16(b, len, &o, 0x8001) || am_put32(b, len, &o, ttl) || am_put16(b, len, &o, 4)) return 0;
    if (o + 4 > len) return 0;
    memcpy(b + o, &ip, 4); o += 4;
    return o;
}

int anet_mdns_is_query_for_us(const uint8_t *b, size_t n, const char *id, uint16_t *qid) {
    if (n < 12 || (b[2] & 0x80)) return 0;                     /* a response */
    *qid = (uint16_t)am_g16(b);
    const unsigned qd = am_g16(b + 4);
    char host[96], inst[200], nm[300];
    am_host_of(id, host, sizeof host);
    snprintf(inst, sizeof inst, "%.63s.%s", id, ANET_SERVICE);
    size_t o = 12;
    for (unsigned i = 0; i < qd; i++) {
        if (am_get_name(b, n, &o, nm, sizeof nm) || o + 4 > n) return 0;
        const unsigned t = am_g16(b + o); o += 4;
        if ((t == AM_T_PTR || t == AM_T_ANY) && !strcasecmp(nm, ANET_SERVICE)) return 1;
        if ((t == AM_T_SRV || t == AM_T_TXT || t == AM_T_ANY) && !strcasecmp(nm, inst)) return 1;
        if ((t == AM_T_A || t == AM_T_ANY) && !strcasecmp(nm, host)) return 1;
    }
    return 0;
}

int anet_mdns_parse_answer(const uint8_t *b, size_t n, anet_peer *p) {
    if (n < 12 || !(b[2] & 0x80)) return -1;
    const unsigned qd = am_g16(b + 4), rr = am_g16(b + 6) + am_g16(b + 8) + am_g16(b + 10);
    size_t o = 12;
    char nm[300], inst[300] = "";
    memset(p, 0, sizeof *p);
    for (unsigned i = 0; i < qd; i++) { if (am_get_name(b, n, &o, nm, sizeof nm) || o + 4 > n) return -1; o += 4; }
    int ours = 0;
    for (unsigned i = 0; i < rr; i++) {
        if (am_get_name(b, n, &o, nm, sizeof nm) || o + 10 > n) return -1;
        const unsigned t = am_g16(b + o), rl = am_g16(b + o + 8);
        size_t d = o + 10;
        if (d + rl > n) return -1;
        if (t == AM_T_PTR && !strcasecmp(nm, ANET_SERVICE)) { size_t x = d; if (am_get_name(b, n, &x, inst, sizeof inst)) return -1; ours = 1; }
        else if (t == AM_T_SRV && rl >= 6) { const size_t sl = strlen(ANET_SERVICE), l = strlen(nm); if (l > sl && !strcasecmp(nm + l - sl, ANET_SERVICE)) p->port = (int)am_g16(b + d + 4); }
        else if (t == AM_T_TXT) {
            size_t x = d;
            while (x < d + rl) {
                const unsigned k = b[x++];
                if (x + k > d + rl) break;
                char kv[256]; memcpy(kv, b + x, k); kv[k] = 0; x += k;
                if (!strncmp(kv, "id=", 3)) snprintf(p->id, sizeof p->id, "%.63s", kv + 3);
                else if (!strncmp(kv, "dac=", 4)) snprintf(p->dac, sizeof p->dac, "%.95s", kv + 4);
                else if (!strncmp(kv, "max=", 4)) p->max_rate = (unsigned)strtoul(kv + 4, NULL, 10);
                else if (!strncmp(kv, "hw=", 3)) snprintf(p->hw, sizeof p->hw, "%.95s", kv + 3);
            }
        } else if (t == AM_T_A && rl == 4) inet_ntop(AF_INET, b + d, p->addr, sizeof p->addr);
        o = d + rl;
    }
    if (!ours || !p->port) return -1;
    if (!p->id[0]) { const char *dot = strchr(inst, '.'); snprintf(p->id, sizeof p->id, "%.*s", dot ? (int)(dot - inst) : (int)strlen(inst), inst); }
    return 0;
}

/* ── The receiver's responder ── */
static struct { char id[64], dac[96]; int port; unsigned max_rate; int fd; } am_PUB;
static uint32_t am_local_ip_to(struct in_addr to) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(9), .sin_addr = to };
    uint32_t ip = 0;
    socklen_t l = sizeof a;
    if (!connect(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &l)) ip = a.sin_addr.s_addr;
    close(s);
    return ip;
}
static void am_pub_send(const struct sockaddr_in *to, uint16_t qid, uint32_t ttl) {
    uint8_t b[1500];
    struct in_addr via = to->sin_addr;
    if (ntohs(to->sin_port) == MDNS_PORT) inet_pton(AF_INET, "8.8.8.8", &via);   /* the default route's address */
    uint32_t ip = am_local_ip_to(via);
    if (!ip) ip = am_local_ip_to(to->sin_addr);
    const size_t n = anet_mdns_answer(b, sizeof b, qid, am_PUB.id, am_PUB.dac, am_PUB.port, am_PUB.max_rate, ip, ttl);
    if (n) sendto(am_PUB.fd, b, n, 0, (const struct sockaddr *)to, sizeof *to);
}
static void *am_pub_main(void *arg) {
    (void)arg;
    struct sockaddr_in grp = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT) };
    inet_pton(AF_INET, MDNS_GROUP, &grp.sin_addr);
    am_pub_send(&grp, 0, 120);
    sleep(1);
    am_pub_send(&grp, 0, 120);
    uint8_t b[1500];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof from;
        const ssize_t n = recvfrom(am_PUB.fd, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        uint16_t qid;
        if (!anet_mdns_is_query_for_us(b, (size_t)n, am_PUB.id, &qid)) continue;
        if (ntohs(from.sin_port) != MDNS_PORT) am_pub_send(&from, qid, 10);   /* legacy: to the asker, its id */
        else am_pub_send(&grp, 0, 120);
    }
    return NULL;
}
int anet_mdns_publish(const char *id, const char *dac, int port, unsigned max_rate) {
    snprintf(am_PUB.id, sizeof am_PUB.id, "%s", id); snprintf(am_PUB.dac, sizeof am_PUB.dac, "%s", dac);
    am_PUB.port = port; am_PUB.max_rate = max_rate;
    int s = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a)) { close(s); return -1; }
    struct ip_mreq m; inet_pton(AF_INET, MDNS_GROUP, &m.imr_multiaddr); m.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);
    unsigned char ttl = 255; setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    am_PUB.fd = s;
    pthread_t th;
    if (pthread_create(&th, NULL, am_pub_main, NULL)) { close(s); return -1; }
    pthread_detach(th);
    return 0;
}

/* ── The player's browser ── */
static double am_mono_(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int anet_mdns_browse(anet_peer *out, int max, int ms) { return anet_mdns_browse2(out, max, ms, NULL); }
int anet_mdns_browse2(anet_peer *out, int max, int ms, const char *ask_ip) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    unsigned char ttl = 255, loop = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    struct sockaddr_in grp = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT) };
    inet_pton(AF_INET, MDNS_GROUP, &grp.sin_addr);
    uint8_t b[1500];
    const uint16_t qid = (uint16_t)(1 + (getpid() ^ (unsigned)(am_mono_() * 1000)) % 65000);
    const size_t qn = anet_mdns_query(b, sizeof b, qid);
    sendto(s, b, qn, 0, (struct sockaddr *)&grp, sizeof grp);

    if (ask_ip) {
        struct sockaddr_in u = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT) };
        if (inet_pton(AF_INET, ask_ip, &u.sin_addr) == 1) sendto(s, b, qn, 0, (struct sockaddr *)&u, sizeof u);
    }
    struct ifaddrs *ifs = NULL;
    if (!getifaddrs(&ifs)) {
        for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            const struct in_addr a = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
            if (i->ifa_flags & IFF_MULTICAST) {
                setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof a);
                sendto(s, b, qn, 0, (struct sockaddr *)&grp, sizeof grp);
            }
            if (!i->ifa_netmask) continue;
            const uint32_t me = ntohl(a.s_addr), m = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
            uint32_t peer = 0;
            if (m == 0xFFFFFFFCu) { const uint32_t net = me & m; peer = me == net + 1 ? net + 2 : me == net + 2 ? net + 1 : 0; }
            else if (m == 0xFFFFFFFEu) peer = me ^ 1;
            if (!peer) continue;
            struct sockaddr_in u = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT) };
            u.sin_addr.s_addr = htonl(peer);
            sendto(s, b, qn, 0, (struct sockaddr *)&u, sizeof u);
        }
        freeifaddrs(ifs);
    }
    int n = 0;
    const double end = am_mono_() + ms / 1000.0;
    for (double t; (t = am_mono_()) < end;) {
        struct pollfd p = { s, POLLIN, 0 };
        if (poll(&p, 1, (int)((end - t) * 1000) + 1) <= 0) continue;
        struct sockaddr_in from; socklen_t fl = sizeof from;
        const ssize_t r = recvfrom(s, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        anet_peer p1;
        if (r <= 0 || anet_mdns_parse_answer(b, (size_t)r, &p1)) continue;
        if (!p1.addr[0] || !strncmp(p1.addr, "127.", 4)) inet_ntop(AF_INET, &from.sin_addr, p1.addr, sizeof p1.addr);
        p1.seen = am_mono_();
        int k = 0;
        while (k < n && strcmp(out[k].id, p1.id)) k++;
        if (k < max) { out[k] = p1; if (k == n) n++; }
    }
    close(s);
    return n;
}
