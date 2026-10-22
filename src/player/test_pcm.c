/* src/player/test_pcm.c: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#include "squeeze.c"

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
    printf(fails ? "PCM FAILED\n" : "PCM OK\n");
    return fails != 0;
}
