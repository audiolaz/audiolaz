

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include "anet.h"

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)

int main(void) {
    uint8_t b[1500];
    anet_hdr h = { .id = 7, .rate = 768000, .ch = 2, .fmt = ANET_F_DOP, .pkt_frames = 176, .pkt_us = 229 }, g;
    anet_hdr_pack(b, &h);
    CHECK(anet_hdr_parse(b, &g) == 0 && g.id == 7 && g.rate == 768000 && g.fmt == ANET_F_DOP && g.pkt_frames == 176 && g.pkt_us == 229 && g.ch == 2, "header round trip");
    h.rate = 0; anet_hdr_pack(b, &h); CHECK(anet_hdr_parse(b, &g) == 0 && g.rate == 0, "INFO request (rate 0)");
    h.rate = 768000;
    {   /* the packets' rhythm: one Ethernet frame each (≤ 1472 bytes of UDP), 2 ms at most between two */
        static const unsigned R[] = { 44100, 48000, 96000, 192000, 384000, 768000 };
        int ok = 1;
        for (int i = 0; i < 6; i++) { uint32_t f, us; anet_pkt_plan(R[i], &f, &us); ok &= ANET_DHDR + f * 8 <= 1472 && us <= 2000 && us >= 200 && f >= 80; }
        CHECK(ok, "packets: under the MTU, ≤ 2 ms apart, 44.1–768 kHz");
        uint8_t d[ANET_DHDR + 8 * 3]; uint32_t id, fr; uint64_t ix;
        anet_dhdr_pack(d, 9, 0x100000007ULL, 3, 0xDEADBEEFu);
        CHECK(anet_dhdr_parse(d, sizeof d, &id, &ix, &fr) == 0 && id == 9 && ix == 0x100000007ULL && fr == 3 && anet_dhdr_flags(d) == 0xDEADBEEFu, "data packet header round trip (its stamp too)");
        CHECK(anet_dhdr_parse(d, sizeof d - 1, &id, &ix, &fr) != 0, "data packet of the wrong length refused");
    }
    CHECK(b[0] == 'A' && b[1] == 'Z' && b[2] == 'N' && b[3] == '1', "header magic AZN1, little-endian");
    h.rate = 1536000; anet_hdr_pack(b, &h); CHECK(anet_hdr_parse(b, &g) == 0, "1536 kHz taken");
    h.rate = 3072000; anet_hdr_pack(b, &h); CHECK(anet_hdr_parse(b, &g) == ANET_E_RATE, "3072 kHz refused");
    h.rate = 44100; h.ch = 6; anet_hdr_pack(b, &h); CHECK(anet_hdr_parse(b, &g) == ANET_E_HEADER, "6 channels refused");
    h.ch = 2; h.fmt = 9; anet_hdr_pack(b, &h); CHECK(anet_hdr_parse(b, &g) == ANET_E_HEADER, "unknown format refused");
    h.fmt = 1; anet_hdr_pack(b, &h); b[0] ^= 1; CHECK(anet_hdr_parse(b, &g) == ANET_E_HEADER, "bad magic refused");

    anet_rep r = { .id = 3, .flags = ANET_R_PLAYING | ANET_R_ACCEPTED, .dac_rate = 384000, .played = 0x123456789ULL, .received = 0x223456789ULL, .ring = 4242, .reason = 0 }, q;
    anet_rep_pack(b, &r);
    CHECK(anet_rep_parse(b, &q) == 0 && q.id == 3 && q.flags == r.flags && q.played == r.played && q.received == r.received && q.ring == 4242 && q.dac_rate == 384000, "report round trip (64-bit counts)");
    b[0] = 0; CHECK(anet_rep_parse(b, &q) != 0, "report bad magic refused");

    {   /* protocol 3: the capabilities in the header's reserved word, read by an older parse as nothing */
        anet_hdr h3 = { .id = 5, .rate = 44100, .ch = 2, .fmt = ANET_F_S32, .pkt_frames = 88, .pkt_us = 1995, .caps = ANET_CAP_ALL }, g3;
        anet_hdr_pack(b, &h3);
        CHECK(anet_hdr_parse(b, &g3) == 0 && g3.caps == ANET_CAP_ALL && anet_g32(b + 4) == 2, "protocol 3: caps carried, the header's version still 2 (a version-2 receiver takes it)");
        h3.caps = 0; anet_hdr_pack(b, &h3);
        CHECK(anet_g32(b + 20) == 0, "a version-2 header: its reserved word 0");
        anet_rep r3 = { .id = 8, .flags = ANET_R_PLAYING | ANET_R_STOPPED, .dac_rate = 48000, .played = 1, .received = 2, .ring = 3, .reason = 4,
                        .target = 14400, .late_us = 12345, .peak_us = 130000, .underruns = 2, .adjusts = 3, .ring_min = 100, .ring_max = 20000,
                        .state = ANET_RS_PLAYING, .why = ANET_WHY_LOST, .seq = 77, .v3 = 1 }, q3;
        const size_t n3 = anet_rep_pack(b, &r3);
        CHECK(n3 == ANET_REP3 && anet_rep_len(b) == ANET_REP3 && anet_rep_parse(b, &q3) == 0 && q3.v3 && q3.target == 14400 && q3.late_us == 12345 && q3.peak_us == 130000
              && q3.underruns == 2 && q3.adjusts == 3 && q3.ring_min == 100 && q3.ring_max == 20000 && q3.state == ANET_RS_PLAYING && q3.why == ANET_WHY_LOST && q3.seq == 77
              && q3.played == 1 && q3.received == 2 && q3.ring == 3 && q3.reason == 4, "protocol 3 report (AZR3, 80 bytes) round trip");
        r3.v3 = 0;
        CHECK(anet_rep_pack(b, &r3) == ANET_REP && anet_rep_len(b) == ANET_REP && anet_rep_parse(b, &q3) == 0 && !q3.v3, "version-2 report still 40 bytes (AZR1)");
        char v[64];
        anet_meta_get("title=Kind of Blue\nartist=Miles Davis\ncover=http://1.2.3.4:9001/cover/7.jpg\n", "artist", v, sizeof v);
        CHECK(!strcmp(v, "Miles Davis"), "meta: a value read");
        anet_meta_get("title=x\n", "album", v, sizeof v);
        CHECK(v[0] == 0, "meta: an absent key is empty");
        CHECK(!strcmp(anet_why_text(ANET_WHY_LOST), "the other side silent") && !strcmp(anet_why_text(999), "an unknown reason"), "stop reasons: texts");
    }
    {   /* one DAC, one player: the HELLO, whole, in multiples of 8, never a NUL (a version-2 receiver reads it 8
         * bytes at a time: none of its types, 1 to 7, ever), read back */
        uint8_t hb[8 + ANET_HELLO_MAX]; char v[128];
        const size_t hn = anet_hello_pack(hb, sizeof hb, "0123456789abcdef", "bureau; audiolaz\n", 1);
        int nul = 0, small = 0;
        for (size_t i = 8; i < hn; i++) nul |= hb[i] == 0;
        for (size_t i = 8; i + 4 <= hn; i += 8) { const uint32_t t = anet_g32(hb + i); small |= t >= 1 && t <= 8; }
        CHECK(hn > 8 && (hn - 8) % 8 == 0 && !nul && !small && anet_g32(hb) == ANET_M_HELLO && anet_g32(hb + 4) == hn - 8, "HELLO: a message of its own, padded to 8, no NUL, no type a version-2 receiver knows");
        hb[hn] = 0;
        anet_meta_get((char *)hb + 8, "player", v, sizeof v); CHECK(!strcmp(v, "0123456789abcdef"), "HELLO: its id read back");
        anet_meta_get((char *)hb + 8, "name", v, sizeof v); CHECK(!strcmp(v, "bureau  audiolaz "), "HELLO: its name read back, « ; » and control characters made spaces");
        anet_meta_get((char *)hb + 8, "take", v, sizeof v); CHECK(!strcmp(v, "1"), "HELLO: take-over said");
        CHECK(ANET_CAP_ALL == (ANET_CAP_P3 | ANET_CAP_WHO) && !strcmp(anet_why_text(ANET_WHY_AGAIN), "the same player again"), "HELLO: its capability, the reason « the same player again »");
    }
    uint8_t m[8]; anet_msg_pack(m, ANET_M_DATA, 4096 * 8); CHECK(anet_g32(m) == 1 && anet_g32(m + 4) == 32768, "message header");

    const size_t qn = anet_mdns_query(b, sizeof b, 0x1234);
    uint16_t qid = 0;
    CHECK(qn > 12 && anet_mdns_is_query_for_us(b, qn, "strm", &qid) && qid == 0x1234, "mDNS PTR query recognised");
    uint32_t ip; inet_pton(AF_INET, "192.0.2.31", &ip);
    const size_t an = anet_mdns_answer(b, sizeof b, 0x1234, "strm", "Swiss Electronics Ltd DAC-01", 4890, 384000, ip, 120);
    anet_peer p;
    CHECK(an > 0 && anet_mdns_parse_answer(b, an, &p) == 0, "mDNS answer parsed");
    CHECK(!strcmp(p.id, "strm") && !strcmp(p.dac, "Swiss Electronics Ltd DAC-01") && p.port == 4890 && p.max_rate == 384000 && !strcmp(p.addr, "192.0.2.31"), "mDNS answer: id, DAC, port, max rate, address");
    CHECK(!anet_mdns_is_query_for_us(b, an, "strm", &qid), "an answer is not a query");
    CHECK(!p.hw[0], "mDNS answer: no DAC identity said, none read");
    /* the DAC's identity (alsa_hw.h) in the TXT: read back; an older reader skips the key */
    anet_mdns_set_hw("usb:16d0:0b6c:QT1234");
    const size_t an2 = anet_mdns_answer(b, sizeof b, 0x1234, "strm-other", "DAC-01", 4890, 384000, ip, 120);
    anet_mdns_set_hw("");
    CHECK(an2 > 0 && anet_mdns_parse_answer(b, an2, &p) == 0 && !strcmp(p.hw, "usb:16d0:0b6c:QT1234") && !strcmp(p.id, "strm-other") && !strcmp(p.dac, "DAC-01"), "mDNS answer: the DAC's identity (hw=) read back");
    /* a compressed name (pointer) in a foreign answer: read without crashing, not ours */
    uint8_t x[64] = { 0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0xC0, 12 };
    CHECK(anet_mdns_parse_answer(x, 14, &p) != 0, "malformed packet refused (pointer loop)");
    printf(fails ? "ANET PROTO: %d FAILED\n" : "ANET PROTO OK\n", fails);
    return fails != 0;
}
