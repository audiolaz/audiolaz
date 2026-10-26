/* src/player/pcmpack.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_PCMPACK_H
#define AUDIOLAZ_PCMPACK_H
#include <stddef.h>
#include <stdint.h>

static inline uint64_t pk_next(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
/* w on the grid of `keep` bits (8 … 31; 32: as it is): itself when on it already, else TPDF-dithered to it; *changed
 * counts the words that were not on it */
static inline int32_t pk_q(int32_t w, int keep, uint64_t *rng, unsigned long *changed) {
    if (keep >= 32 || keep < 8) return w;
    const int s = 32 - keep;
    const int64_t q = (int64_t)1 << s, m = q - 1;
    if (!((int64_t)w & m)) return w;
    const uint64_t r = pk_next(rng);
    const int64_t d = (int64_t)(r & (uint64_t)m) + (int64_t)((r >> 32) & (uint64_t)m) - m;   /* TPDF: −(q−1) … q−1 */
    const int64_t x = (int64_t)w + d + (q >> 1);
    int64_t y = x >= 0 ? x >> s : ~(~x >> s);                                              /* floor (x / q), shifts only */
    const int64_t top = ((int64_t)1 << (keep - 1)) - 1, bot = -((int64_t)1 << (keep - 1));
    if (y > top) y = top; else if (y < bot) y = bot;
    if (changed) (*changed)++;
    return (int32_t)(y * q);
}
/* n samples (words) of src into dst: `bytes` a sample (4, 3, 2), its width (32, 24, 16: the word's top bits it holds),
 * big-endian or not; keep: the bits the card keeps (≤ width); dither 0: the bits under keep dropped (DoP). Returns the
 * samples requantized (not on the grid). */
static inline unsigned long pk_pack(uint8_t *dst, const int32_t *src, size_t n, int bytes, int width, int be, int keep, int dither, uint64_t *rng) {
    unsigned long changed = 0;
    const int sh = 32 - width;
    const uint32_t low = keep < 32 ? ((uint32_t)1 << (32 - keep)) - 1 : 0;
    for (size_t i = 0; i < n; i++) {
        int32_t w = src[i];
        if (low && ((uint32_t)w & low)) w = dither ? pk_q(w, keep, rng, &changed) : (int32_t)((uint32_t)w & ~low);
        const uint32_t v = sh ? (uint32_t)(w < 0 ? ~(~(uint32_t)w >> sh) : (uint32_t)w >> sh) : (uint32_t)w;   /* w >> sh, sign kept */
        uint8_t *p = dst + i * (size_t)bytes;
        if (!be) for (int k = 0; k < bytes; k++) p[k] = (uint8_t)(v >> (8 * k));
        else for (int k = 0; k < bytes; k++) p[k] = (uint8_t)(v >> (8 * (bytes - 1 - k)));
    }
    return changed;
}
/* n frames (two words each) that are DoP: every frame's top byte its marker, the same in both channels, 0x05
 * and 0xFA in turn (DoP 1.1: what a DAC looks for), carried on from the last frame before them (*last: its marker,
 * 0 none: then 4 frames at least to say so: music never keeps that pattern so long, a writer never starts on fewer).
 * Updated: the last frame's marker if DoP, else 0. */
static inline int pk_dop(const int32_t *src, size_t n, uint8_t *last) {
    uint8_t m = *last;
    if (!n || (!m && n < 4)) { *last = 0; return 0; }
    for (size_t i = 0; i < n; i++) {
        const uint8_t a = (uint8_t)((uint32_t)src[2 * i] >> 24), b = (uint8_t)((uint32_t)src[2 * i + 1] >> 24);
        if (a != b || (a != 0x05 && a != 0xFA) || (m && a == m)) { *last = 0; return 0; }
        m = a;
    }
    *last = m;
    return 1;
}
/* (23/10, the review) What a writer does with DoP told by its markers: never at a rate below 176.4 kHz (DSD64's, the
 * least; music at 44.1 to 96 kHz is never DoP, whatever its top bytes: a tone at half its rate near −27 dBFS, the same
 * in both channels, has these markers), and silence on a card keeping fewer than 24 bits only once it has lasted
 * PK_DOP_MUTE_DIV-ths of a second in a row (20 ms: what no music keeps; a DoP DAC itself takes such PCM as DoP), never
 * on markers alone: before that, played as PCM (dithered). pk_dop_at: pk_dop at that rate; pk_dop_mute: m frames told
 * DoP or not, *run the frames of DoP in a row before them (the writer takes back what the card did not take): 1 when
 * they are to be silence. */
#define PK_DOP_RATE 176400u
#define PK_DOP_MUTE_DIV 50u
static inline int pk_dop_at(unsigned rate, const int32_t *src, size_t n, uint8_t *last) {
    if (rate < PK_DOP_RATE) { *last = 0; return 0; }
    return pk_dop(src, n, last);
}
static inline int pk_dop_mute(int dop, unsigned long m, unsigned rate, unsigned long *run) {
    if (!dop || rate < PK_DOP_RATE) { *run = 0; return 0; }
    const int mute = *run >= rate / PK_DOP_MUTE_DIV;
    *run += m;
    return mute;
}
/* The other way (test_pcm.c, the tests' captures): a sample of src in that format back to a 32-bit word, its bits at the top */
static inline int32_t pk_unpack(const uint8_t *p, int bytes, int width, int be) {
    uint32_t v = 0;
    if (!be) for (int k = 0; k < bytes; k++) v |= (uint32_t)p[k] << (8 * k);
    else for (int k = 0; k < bytes; k++) v |= (uint32_t)p[k] << (8 * (bytes - 1 - k));
    return (int32_t)(v << (32 - width));
}
#endif
