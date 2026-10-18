/* src/player/airmeta.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_AIRMETA_H
#define AUDIOLAZ_AIRMETA_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void am_code(uint32_t c, char out[5]) {
    for (int i = 0; i < 4; i++) {
        const unsigned char b = (unsigned char)(c >> (24 - 8 * i));
        out[i] = b >= 0x20 && b < 0x7f ? (char)b : '?';
    }
    out[4] = 0;
}

static size_t am_clean(char *dst, size_t len, const unsigned char *s, size_t n) {
    size_t w = 0, i = 0;
    if (!len) return 0;
    while (i < n && s[i] == ' ') i++;
    for (; i < n && w + 1 < len; i++) {
        const unsigned char c = s[i];
        if (!c) break;
        dst[w++] = c < 0x20 || c == 0x7f || c == '"' || c == '\\' || c == '=' || c == '/' || c == '|' ? ' ' : (char)c;
    }
    if (w > 0 && i < n && s[i]) {                          /* cut: not inside a character */
        size_t j = w - 1;
        while (j > 0 && ((unsigned char)dst[j] & 0xC0) == 0x80) j--;
        const unsigned char c = (unsigned char)dst[j];
        if (c >= 0xC0 && w - j < (size_t)(c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2)) w = j;
    }
    while (w > 0 && dst[w - 1] == ' ') w--;
    dst[w] = 0;
    return w;
}

/* Text or not: printable ASCII and well-formed UTF-8 (a NUL at its very end allowed: some senders end strings so). */
static int am_is_text(const unsigned char *d, size_t n) {
    if (n && !d[n - 1]) n--;
    for (size_t i = 0; i < n; ) {
        const unsigned char c = d[i];
        if (c >= 0x20 && c < 0x7f) { i++; continue; }
        if (c == '\t' || c == '\n' || c == '\r') { i++; continue; }
        const size_t k = c >= 0xF0 && c < 0xF5 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 0;
        if (!k || i + k > n) return 0;
        for (size_t j = 1; j < k; j++) if ((d[i + j] & 0xC0) != 0x80) return 0;
        i += k;
    }
    return 1;
}

static void am_value(uint32_t code, const unsigned char *d, size_t n, size_t most, char *out, size_t outn) {
    if (!outn) return;
    out[0] = 0;
    if (!n) { snprintf(out, outn, "(empty)"); return; }
    if (code == 0x50494354u) {                             /* 'PICT': its size and kind only */
        const char *k = n > 3 && d[0] == 0xFF && d[1] == 0xD8 ? "JPEG" : n > 7 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G' ? "PNG" : "image";
        snprintf(out, outn, "%zu bytes (%s)", n, k);
        return;
    }
    if (n >= 8 && !memcmp(d, "bplist00", 8)) { snprintf(out, outn, "binary plist, %zu bytes", n); return; }
    if (am_is_text(d, n)) {
        char t[512];
        const size_t m = most < sizeof t - 1 ? most : sizeof t - 1;
        const size_t w = am_clean(t, m + 1 < sizeof t ? m + 1 : sizeof t, d, n);
        const int room = (int)(outn > 32 ? outn - 32 : 0);
        if (w < n && n > m) snprintf(out, outn, "\"%.*s…\" (%zu bytes)", room, t, n);
        else snprintf(out, outn, "\"%.*s\"", room, t);
        return;
    }
    char h[3 * 8 + 1] = "";
    size_t w = 0;
    for (size_t i = 0; i < n && i < 8; i++) w += (size_t)snprintf(h + w, sizeof h - w, "%s%02x", i ? " " : "", d[i]);
    if (n == 4) snprintf(out, outn, "%zu bytes: %s (%u)", n, h, (unsigned)((uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]));
    else if (n == 8) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v = v << 8 | d[i];
        snprintf(out, outn, "%zu bytes: %s (%llu)", n, h, (unsigned long long)v);
    } else snprintf(out, outn, "%zu bytes: %s%s", n, h, n > 8 ? " …" : "");
}

enum { AM_NULL, AM_BOOL, AM_INT, AM_REAL, AM_DATE, AM_DATA, AM_STR, AM_UID };
typedef void (*am_leaf_fn)(void *u, const char *path, int kind, const unsigned char *s, size_t n, double num);
#define AM_DEPTH 12
#define AM_LEAVES 4000
#define AM_NODES 20000                  /* objects visited at most: a plist whose arrays name themselves stays cheap */
typedef struct { long leaves, nodes; } am_budget;     /* shared by a plist and the plists inside it */
typedef struct { const unsigned char *b; size_t n; uint64_t nobj, table; unsigned osz, rsz; am_leaf_fn cb; void *u; am_budget *bu; int nest; } am_bp;
static uint64_t am_be(const unsigned char *p, unsigned k) { uint64_t v = 0; for (unsigned i = 0; i < k; i++) v = v << 8 | p[i]; return v; }
static int am_off(const am_bp *P, uint64_t ref, size_t *at) {
    if (ref >= P->nobj) return -1;
    const uint64_t t = P->table + ref * P->osz;
    if (t + P->osz > P->n - 32) return -1;
    const uint64_t o = am_be(P->b + t, P->osz);
    if (o < 8 || o >= P->table) return -1;
    *at = (size_t)o;
    return 0;
}
/* the count of an object at `at` (its marker's low nibble, or the int after it); *body: where its contents start */
static int am_count(const am_bp *P, size_t at, uint64_t *cnt, size_t *body) {
    const unsigned lo = P->b[at] & 0x0F;
    if (lo != 0x0F) { *cnt = lo; *body = at + 1; return 0; }
    if (at + 2 > P->table) return -1;
    const unsigned char m = P->b[at + 1];
    if ((m & 0xF0) != 0x10) return -1;
    const unsigned k = 1u << (m & 0x0F);
    if (k > 8 || at + 2 + k > P->table) return -1;
    *cnt = am_be(P->b + at + 2, k);
    *body = at + 2 + k;
    return 0;
}
static int am_walk_bytes(const unsigned char *b, size_t n, am_leaf_fn cb, void *u, const char *prefix, int nest, am_budget *bu);
static int am_obj(am_bp *P, uint64_t ref, char *path, size_t plen, size_t pcap, int depth) {
    size_t at;
    if (depth > AM_DEPTH || P->bu->leaves >= AM_LEAVES || ++P->bu->nodes > AM_NODES || am_off(P, ref, &at)) return -1;
    const unsigned char m = P->b[at];
    const unsigned hi = m >> 4, lo = m & 0x0F;
    path[plen] = 0;
    switch (hi) {
    case 0x0:
        P->bu->leaves++;
        if (lo == 0x08 || lo == 0x09) P->cb(P->u, path, AM_BOOL, NULL, 0, lo == 0x09);
        else P->cb(P->u, path, AM_NULL, NULL, 0, 0);
        return 0;
    case 0x1: {
        const unsigned k = 1u << lo;
        if (k > 16 || at + 1 + k > P->table) return -1;
        const uint64_t v = am_be(P->b + at + 1 + (k == 16 ? 8 : 0), k == 16 ? 8 : k);
        P->bu->leaves++;
        P->cb(P->u, path, AM_INT, NULL, 0, k >= 8 ? (double)(int64_t)v : (double)v);
        return 0;
    }
    case 0x2: case 0x3: {
        const unsigned k = hi == 0x3 ? 8 : 1u << lo;
        if ((k != 4 && k != 8) || at + 1 + k > P->table) return -1;
        double d;
        if (k == 4) { const uint32_t v = (uint32_t)am_be(P->b + at + 1, 4); float f; memcpy(&f, &v, 4); d = f; }
        else { const uint64_t v = am_be(P->b + at + 1, 8); memcpy(&d, &v, 8); }
        P->bu->leaves++;
        P->cb(P->u, path, hi == 0x3 ? AM_DATE : AM_REAL, NULL, 0, d);
        return 0;
    }
    case 0x4: case 0x5: case 0x6: case 0x8: {
        uint64_t cnt; size_t body;
        if (hi == 0x8) { cnt = lo + 1; body = at + 1; }
        else if (am_count(P, at, &cnt, &body)) return -1;
        const uint64_t bytes = hi == 0x6 ? cnt * 2 : cnt;
        if (bytes > P->table || body + bytes > P->table) return -1;
        P->bu->leaves++;
        if (hi == 0x4 && P->nest < 3 && bytes >= 40 && !memcmp(P->b + body, "bplist00", 8)) {   /* a plist in a plist */
            char sub[512];
            snprintf(sub, sizeof sub, "%s>", path);
            if (!am_walk_bytes(P->b + body, (size_t)bytes, P->cb, P->u, sub, P->nest + 1, P->bu)) return 0;
            if (P->bu->leaves >= AM_LEAVES || P->bu->nodes > AM_NODES) return -1;
            /* a broken one: given as the data it is */
        }
        if (hi == 0x6) {                                   /* UTF-16BE → UTF-8 (the BMP; a surrogate pair as one) */
            unsigned char t[512];
            size_t w = 0;
            for (uint64_t i = 0; i < cnt && w + 4 < sizeof t; i++) {
                uint32_t c = (uint32_t)am_be(P->b + body + 2 * i, 2);
                if (c >= 0xD800 && c < 0xDC00 && i + 1 < cnt) {
                    const uint32_t c2 = (uint32_t)am_be(P->b + body + 2 * (i + 1), 2);
                    if (c2 >= 0xDC00 && c2 < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00); i++; }
                }
                if (c < 0x80) t[w++] = (unsigned char)c;
                else if (c < 0x800) { t[w++] = (unsigned char)(0xC0 | c >> 6); t[w++] = (unsigned char)(0x80 | (c & 0x3F)); }
                else if (c < 0x10000) { t[w++] = (unsigned char)(0xE0 | c >> 12); t[w++] = (unsigned char)(0x80 | (c >> 6 & 0x3F)); t[w++] = (unsigned char)(0x80 | (c & 0x3F)); }
                else { t[w++] = (unsigned char)(0xF0 | c >> 18); t[w++] = (unsigned char)(0x80 | (c >> 12 & 0x3F)); t[w++] = (unsigned char)(0x80 | (c >> 6 & 0x3F)); t[w++] = (unsigned char)(0x80 | (c & 0x3F)); }
            }
            P->cb(P->u, path, AM_STR, t, w, (double)cnt);
            return 0;
        }
        P->cb(P->u, path, hi == 0x4 ? AM_DATA : hi == 0x8 ? AM_UID : AM_STR, P->b + body, (size_t)bytes, (double)bytes);
        return 0;
    }
    case 0xA: case 0xC: case 0xD: {
        uint64_t cnt; size_t body;
        if (am_count(P, at, &cnt, &body)) return -1;
        const uint64_t refs = (hi == 0xD ? 2 : 1) * cnt;
        if (cnt > P->nobj || body + refs * P->rsz > P->table) return -1;
        for (uint64_t i = 0; i < cnt; i++) {
            size_t pl = plen;
            if (hi == 0xD) {                               /* the key: a string object, its text in the path */
                size_t ka; uint64_t kc; size_t kb;
                if (am_off(P, am_be(P->b + body + i * P->rsz, P->rsz), &ka) || am_count(P, ka, &kc, &kb)) return -1;
                const unsigned kh = P->b[ka] >> 4;
                if ((kh != 0x5 && kh != 0x6) || kb + (kh == 0x6 ? 2 * kc : kc) > P->table) return -1;
                if (pl && path[pl - 1] != '>' && pl + 1 < pcap) path[pl++] = '.';
                for (uint64_t j = 0; j < kc && pl + 1 < pcap; j++) {
                    const unsigned c = kh == 0x6 ? (unsigned)am_be(P->b + kb + 2 * j, 2) : P->b[kb + j];
                    path[pl++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
                }
                path[pl] = 0;
                if (am_obj(P, am_be(P->b + body + (cnt + i) * P->rsz, P->rsz), path, pl, pcap, depth + 1)) return -1;
            } else {
                pl += (size_t)snprintf(path + pl, pcap - pl, "[%llu]", (unsigned long long)i);
                if (pl >= pcap) pl = pcap - 1;
                if (am_obj(P, am_be(P->b + body + i * P->rsz, P->rsz), path, pl, pcap, depth + 1)) return -1;
            }
            path[plen] = 0;
        }
        return 0;
    }
    default:
        return -1;
    }
}
static int am_walk_bytes(const unsigned char *b, size_t n, am_leaf_fn cb, void *u, const char *prefix, int nest, am_budget *bu) {
    if (!b || n < 8 + 32 || memcmp(b, "bplist00", 8)) return -1;
    const unsigned char *t = b + n - 32;
    am_bp P = { b, n, am_be(t + 8, 8), am_be(t + 24, 8), t[6], t[7], cb, u, bu, nest };
    const uint64_t top = am_be(t + 16, 8);
    if (!P.osz || P.osz > 8 || !P.rsz || P.rsz > 8 || P.nobj == 0 || P.table < 8 || P.table > n - 32 ||
        P.nobj > (n - 32 - P.table) / P.osz) return -1;
    char path[512];
    snprintf(path, sizeof path, "%s", prefix ? prefix : "");
    return am_obj(&P, top, path, strlen(path), sizeof path, 0);
}
static int am_bplist_walk(const unsigned char *b, size_t n, am_leaf_fn cb, void *u) { am_budget bu = { 0, 0 }; return am_walk_bytes(b, n, cb, u, "", 0, &bu); }

static const char *am_leaf_key(const char *path) {
    const char *k = path;
    for (const char *p = path; *p; p++) if (*p == '.' || *p == '>') k = p + 1;
    return k;
}
/* A key that contains `want`, ignoring case (ASCII). */
static int am_key_has(const char *key, const char *want) {
    const size_t wl = strlen(want);
    for (const char *p = key; *p; p++) {
        size_t i = 0;
        while (i < wl && p[i] && (p[i] | 0x20) == (want[i] | 0x20)) i++;
        if (i == wl) return 1;
    }
    return 0;
}
#endif
