/* src/player/test_pcm.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#include "squeeze.c"
#include "pcmpack.h"

double page_vol_src(int src, double want, int mute, int fresh, int try_) { (void)src; (void)want; (void)mute; (void)fresh; (void)try_; return NAN; }
static int feed(job *j, const uint8_t *data, size_t n) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return -1;
    j->sock = sv[0];
    size_t pre = n < 100 ? n : 100;             /* a little with the headers, the rest through the socket */
    memcpy(j->pre, data, pre); j->pre_len = pre; j->pre_pos = 0;
    if (n > pre) { if (write(sv[1], data + pre, n - pre) != (ssize_t)(n - pre)) return -1; }
    close(sv[1]);
    int ok = pcm_play(j);
    close(sv[0]);
    return ok;
}
static void put_le(uint8_t *p, uint32_t v, int b) { for (int i = 0; i < b; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_be(uint8_t *p, uint32_t v, int b) { for (int i = 0; i < b; i++) p[i] = (uint8_t)(v >> (8 * (b - 1 - i))); }
static int check(const char *what, int bits, int frames, int expect_rate) {
    uint64_t r = atomic_load(&qr), w = atomic_load(&qw);
    int bad = 0;
    for (int i = 0; i < frames; i++) {
        int32_t L = qbuf[2 * ((r + i) & (QFRAMES - 1))], R = qbuf[2 * ((r + i) & (QFRAMES - 1)) + 1];
        int32_t eL = (int32_t)((uint32_t)(i * 1000 - 50000) << (32 - bits)), eR = (int32_t)((uint32_t)(-(i * 777) + 1234) << (32 - bits));
        if (L != eL || R != eR) { if (bad++ < 3) printf("  frame %d: %d %d, expected %d %d\n", i, L, R, eL, eR); }
    }
    printf("%-34s %s (%llu frames queued, rate %u)\n", what, !bad && (int)(w - r) == frames && segs[nseg - 1].rate == (unsigned)expect_rate ? "OK" : "WRONG",
           (unsigned long long)(w - r), segs[nseg - 1].rate);
    atomic_store(&qr, w);
    return bad;
}
int main(void) {
    qbuf = calloc(2 * (size_t)QFRAMES, sizeof(int32_t));
    const int N = 5000;
    static uint8_t buf[1 << 20];
    int fails = 0;
    /* 1. raw, described by the strm: 24-bit little-endian stereo at 96 kHz */
    { job j = { 0 }; j.gen = atomic_load(&gen); j.fmt = 'p'; j.p_bits = 24; j.p_rate = 96000; j.p_ch = 2; j.p_little = 1;
      size_t n = 0; for (int i = 0; i < N; i++) { put_le(buf + n, (uint32_t)(i * 1000 - 50000), 3); n += 3; put_le(buf + n, (uint32_t)(-(i * 777) + 1234), 3); n += 3; }
      feed(&j, buf, n); fails += check("raw 24-bit LE 96 kHz", 24, N, 96000); }

    { job j = { 0 }; j.gen = atomic_load(&gen); j.fmt = 'p'; j.p_little = -1;
      size_t n = 0; memcpy(buf, "RIFF\xff\xff\xff\xffWAVEfmt ", 16); n = 16; put_le(buf + n, 16, 4); n += 4; put_le(buf + n, 1, 2); n += 2; put_le(buf + n, 2, 2); n += 2;
      put_le(buf + n, 44100, 4); n += 4; put_le(buf + n, 44100 * 4, 4); n += 4; put_le(buf + n, 4, 2); n += 2; put_le(buf + n, 16, 2); n += 2;
      memcpy(buf + n, "LIST", 4); n += 4; put_le(buf + n, 5, 4); n += 4; memcpy(buf + n, "abcde\0", 6); n += 6;   /* an odd chunk, padded */
      memcpy(buf + n, "data", 4); n += 4; put_le(buf + n, 0xffffffff, 4); n += 4;
      for (int i = 0; i < N; i++) { put_le(buf + n, (uint32_t)(i * 1000 - 50000), 2); n += 2; put_le(buf + n, (uint32_t)(-(i * 777) + 1234), 2); n += 2; }
      feed(&j, buf, n); fails += check("WAV 16-bit 44.1 kHz (strm « ? »)", 16, N, 44100); }
    /* 3. an AIFF header: 24-bit big-endian stereo 192 kHz */
    { job j = { 0 }; j.gen = atomic_load(&gen); j.fmt = 'p'; j.p_little = -1;
      size_t n = 0; memcpy(buf, "FORM\0\0\0\0AIFFCOMM", 16); n = 16; put_be(buf + n, 18, 4); n += 4; put_be(buf + n, 2, 2); n += 2; put_be(buf + n, N, 4); n += 4; put_be(buf + n, 24, 2); n += 2;
      const uint8_t r192[10] = { 0x40, 0x10, 0xBB, 0x80, 0, 0, 0, 0, 0, 0 }; memcpy(buf + n, r192, 10); n += 10;   /* 192000 */
      memcpy(buf + n, "SSND", 4); n += 4; put_be(buf + n, 8 + N * 6, 4); n += 4; put_be(buf + n, 0, 4); n += 4; put_be(buf + n, 0, 4); n += 4;
      for (int i = 0; i < N; i++) { put_be(buf + n, (uint32_t)(i * 1000 - 50000), 3); n += 3; put_be(buf + n, (uint32_t)(-(i * 777) + 1234), 3); n += 3; }
      feed(&j, buf, n); fails += check("AIFF 24-bit BE 192 kHz", 24, N, 192000); }
    /* 4. raw 16-bit mono: the one channel on both */
    { job j = { 0 }; j.gen = atomic_load(&gen); j.fmt = 'p'; j.p_bits = 16; j.p_rate = 48000; j.p_ch = 1; j.p_little = 1;
      size_t n = 0; for (int i = 0; i < N; i++) { put_le(buf + n, (uint32_t)(i * 1000 - 50000), 2); n += 2; }
      feed(&j, buf, n);
      uint64_t r = atomic_load(&qr); int bad = 0;
      for (int i = 0; i < N; i++) { int32_t e = (int32_t)((uint32_t)(i * 1000 - 50000) << 16); if (qbuf[2 * ((r + i) & (QFRAMES - 1))] != e || qbuf[2 * ((r + i) & (QFRAMES - 1)) + 1] != e) bad++; }
      printf("%-34s %s\n", "raw 16-bit mono 48 kHz", bad ? "WRONG" : "OK"); fails += bad; atomic_store(&qr, atomic_load(&qw)); }
    /* 5. the words packed for a card's format (pcmpack.h) */
    {
        static const struct { const char *name; int bytes, width, be; } F[] = {
            { "S32_LE", 4, 32, 0 }, { "S24_LE", 4, 24, 0 }, { "S24_3LE", 3, 24, 0 }, { "S16_LE", 2, 16, 0 },
            { "S32_BE", 4, 32, 1 }, { "S24_BE", 4, 24, 1 }, { "S24_3BE", 3, 24, 1 }, { "S16_BE", 2, 16, 1 } };
        /* its bytes: 0x12345678 on each grid, and a negative one (S24 in four bytes: sign-extended) */
        static const uint8_t WANT[8][2][4] = {
            { { 0x78, 0x56, 0x34, 0x12 }, { 0x00, 0x00, 0x00, 0x80 } }, { { 0x56, 0x34, 0x12, 0x00 }, { 0x00, 0x00, 0x80, 0xff } },
            { { 0x56, 0x34, 0x12, 0 }, { 0x00, 0x00, 0x80, 0 } },       { { 0x34, 0x12, 0, 0 }, { 0x00, 0x80, 0, 0 } },
            { { 0x12, 0x34, 0x56, 0x78 }, { 0x80, 0x00, 0x00, 0x00 } }, { { 0x00, 0x12, 0x34, 0x56 }, { 0xff, 0x80, 0x00, 0x00 } },
            { { 0x12, 0x34, 0x56, 0 }, { 0x80, 0x00, 0x00, 0 } },       { { 0x12, 0x34, 0, 0 }, { 0x80, 0x00, 0, 0 } } };
        uint64_t rng = 0x9E3779B97F4A7C15ull;
        int bad = 0;
        for (int f = 0; f < 8; f++) {
            const uint32_t low = F[f].width < 32 ? ((uint32_t)1 << (32 - F[f].width)) - 1 : 0;
            const int32_t w[2] = { (int32_t)(0x12345678u & ~low), INT32_MIN };
            uint8_t b[8] = { 0 };
            pk_pack(b, w, 2, F[f].bytes, F[f].width, F[f].be, F[f].width, 1, &rng);
            if (memcmp(b, WANT[f][0], (size_t)F[f].bytes) || memcmp(b + F[f].bytes, WANT[f][1], (size_t)F[f].bytes)) {
                bad++; printf("  %s: bytes %02x %02x %02x %02x, %02x %02x %02x %02x\n", F[f].name, b[0], b[1], b[2], b[3], b[F[f].bytes], b[F[f].bytes + 1], b[F[f].bytes + 2], F[f].bytes > 3 ? b[F[f].bytes + 3] : 0);
            }
            /* words on its grid: back as they were, every one, none counted as requantized */
            enum { NW = 20000 };
            static int32_t in[NW]; static uint8_t pk[NW * 4];
            uint32_t x = 2463534242u;
            for (int i = 0; i < NW; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; in[i] = i < 100 ? 0 : (int32_t)(x & ~low); }
            in[100] = INT32_MIN; in[101] = (int32_t)(0x7FFFFFFFu & ~low);
            const unsigned long ch = pk_pack(pk, in, NW, F[f].bytes, F[f].width, F[f].be, F[f].width, 1, &rng);
            int diff = 0;
            for (int i = 0; i < NW; i++) diff += pk_unpack(pk + (size_t)i * (size_t)F[f].bytes, F[f].bytes, F[f].width, F[f].be) != in[i];
            if (diff || ch) { bad++; printf("  %s: %d words on its grid not back as they were, %lu requantized\n", F[f].name, diff, ch); }
        }
        printf("%-34s %s\n", "packed: 8 formats, bit for bit", bad ? "WRONG" : "OK"); fails += bad;
        /* words of 32 bits to a card of 16 (S16_LE) and of 24 (S24_3LE, a USB DAC's S32_LE of 24): dithered once */
        static const int KEEP[2] = { 16, 24 };
        for (int k = 0; k < 2; k++) {
            enum { NW = 400000 };
            static int32_t in[NW]; static uint8_t pk[NW * 4];
            uint32_t x = 88172645u;
            int on = 0;
            const uint32_t low = ((uint32_t)1 << (32 - KEEP[k])) - 1;
            for (int i = 0; i < NW; i++) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                in[i] = i % 1000 < 50 ? 0 : (int32_t)(x >> 2) - (int32_t)(1u << 29);   /* about −6 dBFS, 5 % digital silence */
            }
            in[0] = INT32_MAX; in[1] = INT32_MIN; in[2] = INT32_MAX - 1;
            for (int i = 0; i < NW; i++) on += !((uint32_t)in[i] & low);
            const int bytes = KEEP[k] == 16 ? 2 : 3;
            const unsigned long ch = pk_pack(pk, in, NW, bytes, KEEP[k], 0, KEEP[k], 1, &rng);
            double s1 = 0, s2 = 0, peak = 0; int quiet = 1, grid = 1; long ne = 0;
            const double lsb = (double)(1u << (32 - KEEP[k]));
            for (int i = 3; i < NW; i++) {                 /* the error of the words that were not on the grid */
                const int32_t y = pk_unpack(pk + (size_t)i * (size_t)bytes, bytes, KEEP[k], 0);
                if ((uint32_t)y & low) grid = 0;
                if (!((uint32_t)in[i] & low)) { if (y != in[i]) quiet = 0; continue; }
                const double e = ((double)y - (double)in[i]) / lsb;
                s1 += e; s2 += e * e; ne++; if (fabs(e) > peak) peak = fabs(e);
            }
            const double mean = s1 / (double)ne, rms = sqrt(s2 / (double)ne);
            const int32_t top = pk_unpack(pk, bytes, KEEP[k], 0), topm1 = pk_unpack(pk + 2 * bytes, bytes, KEEP[k], 0), bot = pk_unpack(pk + bytes, bytes, KEEP[k], 0);
            const int good = grid && quiet && peak < 1.5 && fabs(mean) < 0.01 && rms > 0.48 && rms < 0.52 && (long)ch == NW - on
                             && top == (int32_t)(0x7FFFFFFFu & ~low) && topm1 == top && bot == INT32_MIN;
            char what[64]; snprintf(what, sizeof what, "32 bits to %d: dithered once", KEEP[k]);
            printf("%-34s %s (error %.3f LSB at most, mean %+.4f, RMS %.3f; %lu of %d requantized; full scale %s)\n", what, good ? "OK" : "WRONG",
                   peak, mean, rms, ch, NW, top == (int32_t)(0x7FFFFFFFu & ~low) && bot == INT32_MIN ? "clamped" : "NOT clamped");
            fails += !good;
        }
        /* DoP (dither 0): its words as they are at 24 bits, what lies under them dropped, never dithered */
        {
            const int32_t w[4] = { (int32_t)0x05AA5500u, (int32_t)0xFA55AA00u, (int32_t)0x05AA55FFu, 0 };
            uint8_t b[12];
            const unsigned long ch = pk_pack(b, w, 4, 3, 24, 0, 24, 0, &rng);
            int good = !ch;
            for (int i = 0; i < 4; i++) good &= pk_unpack(b + 3 * i, 3, 24, 0) == (int32_t)((uint32_t)w[i] & 0xFFFFFF00u);
            printf("%-34s %s\n", "DoP to S24_3LE: never dithered", good ? "OK" : "WRONG"); fails += !good;
        }
        /* (22/10, the review) pk_q's floor a shift: the same words as the 64-bit division it replaced, every keep, the
         * same dither drawn (one pk_next a word off the grid), negatives and full scale included */
        {
            long diff = 0, n = 0;
            uint32_t x = 3141592653u;
            for (int keep = 8; keep <= 31; keep++) {
                uint64_t r1 = 0x0123456789ABCDEFull ^ (uint64_t)keep, r2 = r1;
                unsigned long c1 = 0, c2 = 0;
                for (int i = 0; i < 200000; i++) {
                    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                    int32_t w = (int32_t)x;
                    if (i < 8) { static const int32_t E[8] = { INT32_MIN, INT32_MAX, INT32_MIN + 1, INT32_MAX - 1, -1, 1, 0, -2 }; w = E[i]; }
                    const int32_t a = pk_q(w, keep, &r1, &c1);
                    /* the old one, as it was */
                    int32_t b = w;
                    if (keep < 32 && keep >= 8) {
                        const int sh = 32 - keep;
                        const int64_t q = (int64_t)1 << sh, m = q - 1;
                        if ((int64_t)w & m) {
                            const uint64_t r = pk_next(&r2);
                            const int64_t d = (int64_t)(r & (uint64_t)m) + (int64_t)((r >> 32) & (uint64_t)m) - m;
                            const int64_t xx = (int64_t)w + d + (q >> 1);
                            int64_t y = xx >= 0 ? xx / q : -((-xx + q - 1) / q);
                            const int64_t top = ((int64_t)1 << (keep - 1)) - 1, bot = -((int64_t)1 << (keep - 1));
                            if (y > top) y = top; else if (y < bot) y = bot;
                            c2++;
                            b = (int32_t)(y * q);
                        }
                    }
                    diff += a != b; n++;
                }
                diff += c1 != c2 || r1 != r2;
            }
            printf("%-34s %s (%ld words, keep 8 to 31)\n", "pk_q shift = division", diff ? "WRONG" : "OK", n); fails += diff != 0;
        }
        /* (22/10, the review) DoP told by its markers (pk_dop): whole DoP chunks yes (carried from one to the next, a short
         * one after DoP too), PCM no, a broken alternation no, a lone short chunk no; never dithered (pk_pack: !dop) */
        {
            enum { NF = 512 };
            static int32_t d[2 * NF], pcm[2 * NF];
            uint32_t x = 2718281828u;
            for (int i = 0; i < NF; i++) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                const uint32_t m = (i & 1 ? 0xFAu : 0x05u) << 24, bits = (x & 0xFFFF) << 8;
                d[2 * i] = (int32_t)(m | bits); d[2 * i + 1] = (int32_t)(m | ((x >> 16) << 8));
                pcm[2 * i] = (int32_t)x; pcm[2 * i + 1] = (int32_t)(x * 2654435761u);
            }
            uint8_t last = 0;
            int good = pk_dop(d, NF, &last) && last == 0xFA;          /* ends on an odd frame: 0xFA */
            good &= pk_dop(d, 4, &last);                               /* 0x05 next: carried on, short as it is */
            good &= last == 0xFA;
            uint8_t l2 = 0xFA; good &= !pk_dop(d + 2, 8, &l2) && l2 == 0;   /* 0xFA after 0xFA: not DoP */
            uint8_t l3 = 0; good &= !pk_dop(d, 3, &l3);                   /* 3 frames, nothing before: not said */
            uint8_t l3b = 0; good &= pk_dop(d, 4, &l3b) && l3b == 0xFA;   /* 4: said */
            uint8_t l4 = 0; good &= !pk_dop(pcm, NF, &l4);                /* music */
            d[2 * 300 + 1] ^= 0x01000000;                                  /* one channel's marker wrong */
            uint8_t l5 = 0; good &= !pk_dop(d, NF, &l5);
            d[2 * 300 + 1] ^= 0x01000000;
            /* a DoP word with something under its 24 bits, to a card of 24: those bits dropped, never dithered */
            d[2 * 7] |= 0x7F;
            static uint8_t b[NF * 2 * 3];
            uint64_t rng2 = 1;
            uint8_t l6 = 0;
            const int dop = pk_dop(d, NF, &l6);
            const unsigned long ch = pk_pack(b, d, 2 * NF, 3, 24, 0, 24, !dop, &rng2);
            int same = dop && !ch;
            for (int i = 0; i < 2 * NF; i++) same &= pk_unpack(b + 3 * i, 3, 24, 0) == (int32_t)((uint32_t)d[i] & 0xFFFFFF00u);
            good &= same;
            printf("%-34s %s\n", "DoP told by its markers", good ? "OK" : "WRONG"); fails += !good;
        }
        /* (23/10, the review) music with DoP's markers never muted: a tone at half the rate near −27 dBFS, the same in
         * both channels (every top byte 0x05 / 0xFA in turn), written in chunks of 2048 frames as out_alsa.c does: at 44.1
         * to 96 kHz never DoP; at 176.4 kHz played (dithered) for its first 20 ms, silence only after; a frame of music
         * between starts the count again */
        {
            enum { CH = 2048 };
            static int32_t t[2 * CH];
            for (int i = 0; i < CH; i++) t[2 * i] = t[2 * i + 1] = (i & 1) ? -0x05800000 : 0x05800000;   /* ±0.043 */
            int good = (uint8_t)((uint32_t)t[0] >> 24) == 0x05 && (uint8_t)((uint32_t)t[2] >> 24) == 0xFA;
            static const unsigned LO[4] = { 44100, 48000, 88200, 96000 };
            for (int r = 0; r < 4; r++) {
                uint8_t last = 0; unsigned long run = 0; int muted = 0;
                for (int c = 0; c < 200; c++) { const int dop = pk_dop_at(LO[r], t, CH, &last); muted |= dop | pk_dop_mute(dop, CH, LO[r], &run); }
                good &= !muted;
            }
            uint8_t last = 0; unsigned long run = 0; unsigned long before = 0; int first = -1;
            for (int c = 0; c < 10; c++) {
                const int dop = pk_dop_at(176400, t, CH, &last);
                const int mute = pk_dop_mute(dop, CH, 176400, &run);
                if (mute && first < 0) first = c;
                if (!mute) before += CH;
            }
            good &= first > 0 && before >= 176400 / 50 && before < 176400 / 50 + CH;   /* 20 ms played, then silence */
            t[2 * 100] = 0;                                                         /* a frame of music: counted again */
            good &= !pk_dop_mute(pk_dop_at(176400, t, CH, &last), CH, 176400, &run) && run == 0;
            t[2 * 100] = 0x05800000;
            good &= !pk_dop_mute(pk_dop_at(176400, t, CH, &last), CH, 176400, &run);   /* back: 20 ms again */
            printf("%-34s %s\n", "music with DoP's markers: no mute", good ? "OK" : "WRONG"); fails += !good;
        }
    }
    printf(fails ? "PCM FAILED\n" : "PCM OK\n");
    return fails != 0;
}
