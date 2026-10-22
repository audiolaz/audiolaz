/* src/player/nowplay.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#endif
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Wcomma"
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_GIF
#define STBI_MAX_DIMENSIONS 3000
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma GCC diagnostic pop
#include <ctype.h>
#include "cover_font.h"

#define NP_S 500                                   /* the pictures: 500 × 500 (10/10: was 600) */
#define NP_FLASH_S 3.0
#define NP_FLASH_MAX_S 8.0                         /* … and once the bridge read it (it asks every ~6 s), else this long */
#define NP_IMG_MAX (8u << 20)                      /* a picture given: 8 MB at most */
#define NP_TXT 256
typedef struct {
    int has;                                       /* something said (a title at least, or a picture) */
    char title[NP_TXT], artist[NP_TXT], album[NP_TXT];
    unsigned char *img;                            /* the picture as given (JPEG or PNG), NULL none */
    size_t img_len;
} np_t;
static np_t NP[NETOUT_NP_N];
static const char *const NP_SRC[NETOUT_NP_N] = { "roon", "airplay", "spotify", "upnp" };
static int np_act = -1;                            /* the source playing (netout_np_source), −1 none */
static struct { int on, served; double t0; char title[NP_TXT], sub[NP_TXT]; } FL;

static char NPI_t[NP_TXT], NPI_s[NP_TXT];
static unsigned np_ser = 1;                        /* what is shown, numbered: the cover's URL (/cover/<n>.jpg) */
static pthread_mutex_t nplk = PTHREAD_MUTEX_INITIALIZER;   /* NP, np_act, FL, np_ser, CV; taken after clk, never before it */
static pthread_cond_t npcv = PTHREAD_COND_INITIALIZER;
static struct { unsigned ser; unsigned char *jpg; size_t len; } CV;   /* the picture made for np_ser (ser 0: none yet) */
static int np_cli = 9095;                          /* the CLI's port (netout_np_cli_port; 0: none) */
static int np_started = 0;

/* ── What is shown now ─────────────────────────────────────────────────────────────────────────────────────── */
typedef struct {
    unsigned ser;
    char title[NP_TXT], artist[NP_TXT], album[NP_TXT];
    int card;                                      /* 1: the audiolaz card (card_t, card_s), 0: the picture (img) */
    char card_t[NP_TXT], card_s[NP_TXT];
    unsigned char *img; size_t img_len;            /* a copy (np_view with pic: the caller frees it) */
} np_view_t;
static void np_cut(char *dst, size_t len, const char *s) {   /* copied, control characters as spaces, UTF-8 not cut inside a character */
    size_t w = 0;
    for (; s && *s && w + 1 < len; s++) dst[w++] = (unsigned char)*s < 0x20 ? ' ' : *s;
    if (s && *s && w > 0) { size_t j = w; while (j > 0 && ((unsigned char)dst[j - 1] & 0xC0) == 0x80) j--; if (j > 0 && (unsigned char)dst[j - 1] >= 0xC0) w = j - 1; }
    dst[w] = 0;
    while (w > 0 && dst[w - 1] == ' ') dst[--w] = 0;
}
static void np_tell_link(void);                     /* netout.c: what plays, to an audiolaz receiver (ANET_M_META) */
static void np_bump(void) { np_ser++; pthread_cond_broadcast(&npcv); np_tell_link(); }   /* under nplk */

static void np_flash_tick(void) {
    if (!FL.on) return;
    const double age = mono() - FL.t0;
    if ((age >= NP_FLASH_S && FL.served) || age >= NP_FLASH_MAX_S) { FL.on = 0; np_bump(); }
}

static void np_view(np_view_t *v, int flash, int pic) {
    char me[96];
    pthread_mutex_lock(&clk);
    snprintf(me, sizeof me, "%s", me_name);
    pthread_mutex_unlock(&clk);
    memset(v, 0, sizeof *v);
    pthread_mutex_lock(&nplk);
    np_flash_tick();
    v->ser = np_ser;
    const np_t *n = np_act >= 0 && NP[np_act].has ? &NP[np_act] : NULL;
    if (flash && FL.on) {
        snprintf(v->title, NP_TXT, "audiolaz \xc2\xb7 %.200s", FL.title);
        snprintf(v->artist, NP_TXT, "%s", FL.sub);
        v->card = 1; snprintf(v->card_t, NP_TXT, "%s", FL.title); snprintf(v->card_s, NP_TXT, "%s", FL.sub);
    } else if (n) {
        snprintf(v->title, NP_TXT, "%s", n->title[0] ? n->title : me);
        snprintf(v->artist, NP_TXT, "%s", n->artist); snprintf(v->album, NP_TXT, "%s", n->album);
        if (n->img && n->img_len) {
            if (pic && (v->img = malloc(n->img_len))) { memcpy(v->img, n->img, n->img_len); v->img_len = n->img_len; }
        } else { v->card = 1; snprintf(v->card_t, NP_TXT, "%s", v->title); snprintf(v->card_s, NP_TXT, "%s", n->artist); }
    } else if (NPI_t[0]) {
        snprintf(v->title, NP_TXT, "%s", NPI_t); snprintf(v->artist, NP_TXT, "%s", NPI_s);
        v->card = 1; snprintf(v->card_t, NP_TXT, "%s", NPI_t); snprintf(v->card_s, NP_TXT, "%s", NPI_s);
    } else {
        snprintf(v->title, NP_TXT, "%s", me);
        v->card = 1; snprintf(v->card_t, NP_TXT, "%s", me);
    }
    pthread_mutex_unlock(&nplk);
}

/* ── The pictures ──────────────────────────────────────────────────────────────────────────────────────────── */
typedef struct { unsigned char *p; int w, h; } np_img;          /* RGB */
static void np_blend(np_img *im, int x, int y, const unsigned char rgb[3], double a) {
    if (x < 0 || y < 0 || x >= im->w || y >= im->h || a <= 0) return;
    if (a > 1) a = 1;
    unsigned char *q = im->p + ((size_t)y * im->w + x) * 3;
    for (int k = 0; k < 3; k++) q[k] = (unsigned char)(q[k] + (rgb[k] - q[k]) * a + (rgb[k] >= q[k] ? 0.5 : -0.5));
}
/* The mark (site/brand/mark.svg, a 48-unit square): the room seen from above (a square, 6…42, its stroke 2.6) and the
 * place in it (a diamond, centre 24,25, half-width 6.4, half-height 8). In a cell of this side at (x0, y0); 4 × 4
 * samples a pixel. */
static int np_in_mark(double u, double v) {
    const double o = 1.3;
    const int sq = u >= 6 - o && u <= 42 + o && v >= 6 - o && v <= 42 + o && !(u > 6 + o && u < 42 - o && v > 6 + o && v < 42 - o);
    return sq || fabs(u - 24) / 6.4 + fabs(v - 25) / 8.0 <= 1.0;
}
static void np_mark(np_img *im, double x0, double y0, double side, const unsigned char rgb[3], double alpha) {
    const double k = 48.0 / side;
    for (int y = (int)floor(y0); y < (int)ceil(y0 + side); y++)
        for (int x = (int)floor(x0); x < (int)ceil(x0 + side); x++) {
            int in = 0;
            for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++)
                in += np_in_mark((x + (sx + 0.5) / 4 - x0) * k, (y + (sy + 0.5) / 4 - y0) * k);
            if (in) np_blend(im, x, y, rgb, alpha * in / 16.0);
        }
}
static void np_round_rect(np_img *im, double x0, double y0, double w, double h, double r, const unsigned char rgb[3], double alpha) {
    for (int y = (int)floor(y0); y < (int)ceil(y0 + h); y++)
        for (int x = (int)floor(x0); x < (int)ceil(x0 + w); x++) {
            int in = 0;
            for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
                const double px = x + (sx + 0.5) / 4, py = y + (sy + 0.5) / 4;
                if (px < x0 || py < y0 || px > x0 + w || py > y0 + h) continue;
                const double cx = px < x0 + r ? x0 + r : px > x0 + w - r ? x0 + w - r : px;
                const double cy = py < y0 + r ? y0 + r : py > y0 + h - r ? y0 + h - r : py;
                in += (px - cx) * (px - cx) + (py - cy) * (py - cy) <= r * r;
            }
            if (in) np_blend(im, x, y, rgb, alpha * in / 16.0);
        }
}
static const unsigned char NP_INK[3] = { 0x1c, 0x1c, 0x1a }, NP_PAPER[3] = { 0xee, 0xed, 0xe6 };
/* The badge on a cover: the mark, light, on a dark rounded square, bottom right (12 % of the side, 3.5 % from the edges). */
static void np_badge(np_img *im) {
    const double b = im->w * 0.12, m = im->w * 0.035, x0 = im->w - m - b, y0 = im->h - m - b;
    np_round_rect(im, x0, y0, b, b, b * 0.2, NP_INK, 0.62);
    np_mark(im, x0 + b * 0.12, y0 + b * 0.12, b * 0.76, NP_PAPER, 0.95);
}
/* A picture given, square: its centre cropped, scaled to NP_S (each target pixel the mean of the source pixels it
 * covers; bilinear for a smaller picture). 0, or −1 not a picture we read. */
static int np_cover_from(const unsigned char *data, size_t len, np_img *out) {
    int w, h, ch;
    if (!stbi_info_from_memory(data, (int)len, &w, &h, &ch) || w < 16 || h < 16 || w > 3000 || h > 3000) return -1;
    unsigned char *src = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 3);
    if (!src) return -1;
    const int side = w < h ? w : h, ox = (w - side) / 2, oy = (h - side) / 2;
    out->w = out->h = NP_S;
    out->p = malloc((size_t)NP_S * NP_S * 3);
    if (!out->p) { stbi_image_free(src); return -1; }
    const double f = (double)side / NP_S;
    for (int y = 0; y < NP_S; y++)
        for (int x = 0; x < NP_S; x++) {
            double acc[3] = { 0, 0, 0 };
            if (f > 1) {                                                /* down: the mean of the area */
                const int x1 = ox + (int)(x * f), x2 = ox + (int)((x + 1) * f), y1 = oy + (int)(y * f), y2 = oy + (int)((y + 1) * f);
                int n = 0;
                for (int yy = y1; yy < (y2 > y1 ? y2 : y1 + 1); yy++)
                    for (int xx = x1; xx < (x2 > x1 ? x2 : x1 + 1); xx++) {
                        const unsigned char *s = src + ((size_t)yy * w + xx) * 3;
                        acc[0] += s[0]; acc[1] += s[1]; acc[2] += s[2]; n++;
                    }
                for (int k = 0; k < 3; k++) acc[k] /= n;
            } else {                                                    /* up: bilinear */
                double sx = (x + 0.5) * f - 0.5, sy = (y + 0.5) * f - 0.5;
                if (sx < 0) sx = 0;
                if (sy < 0) sy = 0;
                int ix = (int)sx, iy = (int)sy;
                if (ix > side - 2) ix = side - 2;
                if (iy > side - 2) iy = side - 2;
                const double fx = sx - ix, fy = sy - iy;
                for (int k = 0; k < 3; k++) {
                    const unsigned char *s = src + ((size_t)(oy + iy) * w + ox + ix) * 3 + k;
                    acc[k] = (s[0] * (1 - fx) + s[3] * fx) * (1 - fy) + (s[(size_t)w * 3] * (1 - fx) + s[(size_t)w * 3 + 3] * fx) * fy;
                }
            }
            unsigned char *d = out->p + ((size_t)y * NP_S + x) * 3;
            for (int k = 0; k < 3; k++) d[k] = (unsigned char)(acc[k] + 0.5);
        }
    stbi_image_free(src);
    return 0;
}
/* Text, with the brand's font: UTF-8 read as code points (none we cannot draw: skipped). */
static stbtt_fontinfo np_font[2];
static int np_font_ok = -1;
static int np_fonts(void) {
    if (np_font_ok < 0) np_font_ok = stbtt_InitFont(&np_font[0], cover_font_bold, 0) && stbtt_InitFont(&np_font[1], cover_font_semi, 0);
    return np_font_ok;
}
static int np_utf8(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p, n = c < 0x80 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (n < 0) { (*s)++; return 0xFFFD; }
    c = n == 0 ? c : n == 1 ? c & 0x1F : n == 2 ? c & 0x0F : c & 0x07;
    p++;
    for (int i = 0; i < n; i++, p++) { if ((*p & 0xC0) != 0x80) { *s = (const char *)p; return 0xFFFD; } c = (c << 6) | (*p & 0x3F); }
    *s = (const char *)p;
    return c;
}
/* The width (px) of s[0…len) at this size; up to its end when len < 0. */
static double np_width(int f, double px, const char *s, int len) {
    const stbtt_fontinfo *fi = &np_font[f];
    const double sc = stbtt_ScaleForPixelHeight(fi, (float)px);
    double w = 0;
    int prev = 0;
    const char *e = len < 0 ? s + strlen(s) : s + len;
    while (s < e && *s) {
        const int c = np_utf8(&s);
        if (!stbtt_FindGlyphIndex(fi, c)) continue;
        int adv, lsb;
        stbtt_GetCodepointHMetrics(fi, c, &adv, &lsb);
        if (prev) w += sc * stbtt_GetCodepointKernAdvance(fi, prev, c);
        w += sc * adv; prev = c;
    }
    return w;
}
static void np_text(np_img *im, int f, double px, double x, double baseline, const char *s, int len, const unsigned char rgb[3], double alpha) {
    const stbtt_fontinfo *fi = &np_font[f];
    const float sc = stbtt_ScaleForPixelHeight(fi, (float)px);
    int prev = 0;
    const char *e = len < 0 ? s + strlen(s) : s + len;
    while (s < e && *s) {
        const int c = np_utf8(&s);
        if (!stbtt_FindGlyphIndex(fi, c)) continue;
        if (prev) x += sc * stbtt_GetCodepointKernAdvance(fi, prev, c);
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(fi, c, &adv, &lsb);
        const float sx = (float)(x - floor(x));
        stbtt_GetCodepointBitmapBoxSubpixel(fi, c, sc, sc, sx, 0, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw < 400 && gh < 400) {
            unsigned char *g = malloc((size_t)gw * gh);
            if (g) {
                stbtt_MakeCodepointBitmapSubpixel(fi, g, gw, gh, gw, sc, sc, sx, 0, c);
                for (int yy = 0; yy < gh; yy++) for (int xx = 0; xx < gw; xx++)
                    if (g[yy * gw + xx]) np_blend(im, (int)floor(x) + x0 + xx, (int)baseline + y0 + yy, rgb, alpha * g[yy * gw + xx] / 255.0);
                free(g);
            }
        }
        x += sc * adv; prev = c;
    }
}

static int np_lines(np_img *im, int f, double px, double y, double step, double maxw, int nmax, const char *s, const unsigned char rgb[3], double alpha) {
    int n = 0;
    while (*s == ' ') s++;
    while (*s && n < nmax) {
        const char *best = NULL, *p = s;
        while (*p) {                                                    /* the longest run of whole words that fits */
            const char *q = p;
            while (*q == ' ') q++;
            while (*q && *q != ' ') q++;
            if (np_width(f, px, s, (int)(q - s)) > maxw) break;
            best = q; p = q;
        }
        if (!best) {                                                    /* one word too long: as many characters as fit */
            best = s;
            for (const char *q = s; *q; ) { const char *r = q; np_utf8(&r); if (np_width(f, px, s, (int)(r - s)) > maxw) break; best = q = r; }
            if (best == s) { np_utf8(&best); }
        }
        const char *rest = best;
        while (*rest == ' ') rest++;
        char line[NP_TXT + 8];
        int len = (int)(best - s);
        if (len > NP_TXT) len = NP_TXT;
        memcpy(line, s, (size_t)len); line[len] = 0;
        if (n == nmax - 1 && *rest) {
            while (len > 0 && np_width(f, px, line, -1) + np_width(f, px, "\xe2\x80\xa6", -1) > maxw) {
                do len--; while (len > 0 && ((unsigned char)line[len] & 0xC0) == 0x80);
                line[len] = 0;
            }
            while (len > 0 && line[len - 1] == ' ') line[--len] = 0;
            memcpy(line + len, "\xe2\x80\xa6", 4);
        }
        np_text(im, f, px, (im->w - np_width(f, px, line, -1)) / 2, y + n * step, line, -1, rgb, alpha);
        n++;
        s = rest;
    }
    return n;
}

static void np_card(np_img *im, const char *title, const char *sub) {
    im->w = im->h = NP_S;
    im->p = malloc((size_t)NP_S * NP_S * 3);
    if (!im->p) return;
    for (size_t i = 0; i < (size_t)NP_S * NP_S; i++) memcpy(im->p + i * 3, NP_INK, 3);
    const double side = NP_S * 0.24;
    np_mark(im, (NP_S - side) / 2, NP_S * 0.15, side, NP_PAPER, 1.0);
    if (!np_fonts()) return;
    const double tp = NP_S * 0.075, sp = NP_S * 0.045, maxw = NP_S * 0.84;
    const int n = np_lines(im, 0, tp, NP_S * 0.58, tp * 1.18, maxw, 2, title, NP_PAPER, 1.0);
    if (sub && *sub) np_lines(im, 1, sp, NP_S * 0.58 + (n > 0 ? n - 1 : 0) * tp * 1.18 + sp * 1.9, sp * 1.2, maxw, 2, sub, NP_PAPER, 0.62);
    if (strcmp(title, "audiolaz") && strncmp(title, "audiolaz ", 9)) {
        const double fp = NP_S * 0.034;
        np_text(im, 0, fp, (NP_S - np_width(0, fp, "audiolaz", -1)) / 2, NP_S * 0.93, "audiolaz", -1, NP_PAPER, 0.38);
    }
}
typedef struct { unsigned char *p; size_t len, cap; } np_buf;
static void np_put(void *ctx, void *data, int size) {
    np_buf *b = ctx;
    if (!b->p || size <= 0) return;
    if (b->len + (size_t)size > b->cap) {
        size_t c = b->cap * 2;
        while (c < b->len + (size_t)size) c *= 2;
        unsigned char *q = realloc(b->p, c);
        if (!q) { free(b->p); b->p = NULL; return; }
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->len, data, (size_t)size); b->len += (size_t)size;
}
/* JPEG, baseline (stb_image_write's only kind), 150 KB at most (quality 88, else lower). */
static unsigned char *np_jpeg(const np_img *im, size_t *len) {
    static const int Q[] = { 88, 80, 70, 60 };
    for (unsigned i = 0; i < sizeof Q / sizeof *Q; i++) {
        np_buf b = { malloc(65536), 0, 65536 };
        if (!b.p) return NULL;
        if (!stbi_write_jpg_to_func(np_put, &b, im->w, im->h, 3, im->p, Q[i]) || !b.p) { free(b.p); return NULL; }
        if (b.len <= 150000 || i == sizeof Q / sizeof *Q - 1) { *len = b.len; return b.p; }
        free(b.p);
    }
    return NULL;
}
/* What is shown, as a picture: the cover with the mark, or the card (a picture we cannot read: the card). */
static unsigned char *np_compose(const np_view_t *v, size_t *len) {
    np_img im = { 0 };
    if (!v->card && v->img && !np_cover_from(v->img, v->img_len, &im)) np_badge(&im);
    else { free(im.p); im.p = NULL; np_card(&im, v->card ? v->card_t : v->title, v->card ? v->card_s : v->artist); }
    if (!im.p) return NULL;
    unsigned char *j = np_jpeg(&im, len);
    free(im.p);
    return j;
}
/* Makes the picture of each new np_ser, at once (the bridge fetches it right after it read the text). */
static void *np_maker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&nplk);
        while (CV.ser == np_ser) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&npcv, &nplk, &ts);
            np_flash_tick();
        }
        pthread_mutex_unlock(&nplk);
        np_view_t v;
        np_view(&v, 1, 1);
        size_t len = 0;
        const double t0 = mono();
        unsigned char *j = np_compose(&v, &len);
        free(v.img);
        pthread_mutex_lock(&nplk);
        free(CV.jpg);
        CV.jpg = j; CV.len = j ? len : 0; CV.ser = v.ser;
        pthread_cond_broadcast(&npcv);
        pthread_mutex_unlock(&nplk);
        nlog("now playing %u: \"%s\" / \"%s\" / \"%s\", %s, %zu bytes (%.0f ms)", v.ser, v.title, v.artist, v.album,
             v.card ? "the audiolaz card" : "its cover", len, (mono() - t0) * 1000);
    }
    return NULL;
}
static void np_start(void) {
    pthread_mutex_lock(&nplk);
    const int go = !np_started;
    np_started = 1;
    pthread_mutex_unlock(&nplk);
    pthread_t th;
    if (go && !pthread_create(&th, NULL, np_maker, NULL)) pthread_detach(th);
}

/* ── What the player is told ───────────────────────────────────────────────────────────────────────────────── */
void netout_np_text(int src, const char *title, const char *artist, const char *album) {
    if (src < 0 || src >= NETOUT_NP_N) return;
    np_start();
    pthread_mutex_lock(&nplk);
    np_t *n = &NP[src];
    char t[NP_TXT], a[NP_TXT], l[NP_TXT];
    np_cut(t, sizeof t, title ? title : n->title); np_cut(a, sizeof a, artist ? artist : n->artist); np_cut(l, sizeof l, album ? album : n->album);
    const int changed = !n->has || strcmp(t, n->title) || strcmp(a, n->artist) || strcmp(l, n->album);
    memcpy(n->title, t, sizeof t); memcpy(n->artist, a, sizeof a); memcpy(n->album, l, sizeof l);
    n->has = t[0] || a[0] || l[0] || n->img;
    if (changed && src == np_act) np_bump();
    pthread_mutex_unlock(&nplk);
}
void netout_np_cover(int src, const void *img, size_t len) {
    if (src < 0 || src >= NETOUT_NP_N || len > NP_IMG_MAX) return;
    np_start();
    unsigned char *c = len && img ? malloc(len) : NULL;
    if (c) memcpy(c, img, len);
    pthread_mutex_lock(&nplk);
    np_t *n = &NP[src];
    const int changed = (n->img_len != (c ? len : 0)) || (c && memcmp(n->img, c, len));
    free(n->img); n->img = c; n->img_len = c ? len : 0;
    n->has = n->title[0] || n->artist[0] || n->album[0] || n->img;
    if (changed && src == np_act) np_bump();
    pthread_mutex_unlock(&nplk);
}
void netout_np_clear(int src) {
    if (src < 0 || src >= NETOUT_NP_N) return;
    pthread_mutex_lock(&nplk);
    np_t *n = &NP[src];
    const int had = n->has;
    free(n->img);
    memset(n, 0, sizeof *n);
    if (had && src == np_act) np_bump();
    pthread_mutex_unlock(&nplk);
}
void netout_np_source(int src) {
    if (src < -1 || src >= NETOUT_NP_N) src = -1;
    np_start();
    pthread_mutex_lock(&nplk);
    if (src != np_act) { np_act = src; np_bump(); }
    pthread_mutex_unlock(&nplk);
}
void netout_np_flash(const char *title, const char *sub) {
    np_start();
    pthread_mutex_lock(&nplk);
    np_cut(FL.title, sizeof FL.title, title ? title : "");
    np_cut(FL.sub, sizeof FL.sub, sub ? sub : "");
    FL.on = 1; FL.served = 0; FL.t0 = mono();
    np_bump();
    pthread_mutex_unlock(&nplk);
}
void netout_np_cli_port(int port) { np_cli = port; }

void netout_np_idle(const char *title, const char *sub) {
    np_start();
    pthread_mutex_lock(&nplk);
    char t[NP_TXT], u[NP_TXT];
    np_cut(t, sizeof t, title ? title : ""); np_cut(u, sizeof u, sub ? sub : "");
    if (strcmp(t, NPI_t) || strcmp(u, NPI_s)) { snprintf(NPI_t, sizeof NPI_t, "%s", t); snprintf(NPI_s, sizeof NPI_s, "%s", u); np_bump(); }
    pthread_mutex_unlock(&nplk);
}
void netout_np_tick(void) { pthread_mutex_lock(&nplk); np_flash_tick(); pthread_mutex_unlock(&nplk); }

/* ── The cover, served by the stream server: GET /cover/<n>.jpg (the bridge, a renderer) ─────────────────── */
/* 1 when it was a cover's request (answered). The picture of what is shown now (a renderer's older number: the latest),
 * made within 3 s; none: 404. */
static int np_http_cover(int c, const char *method, const char *path) {
    unsigned ser = 0;
    char ext[8] = "";
    if (sscanf(path, "/cover/%u.%7s", &ser, ext) != 2 || strcmp(ext, "jpg")) return 0;
    np_start();
    pthread_mutex_lock(&nplk);
    const double t0 = mono();
    while ((CV.ser < ser || CV.ser < np_ser) && mono() - t0 < 3.0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&npcv, &nplk, &ts);
    }
    unsigned char *j = CV.len ? malloc(CV.len) : NULL;
    const size_t len = j ? CV.len : 0;
    if (j) memcpy(j, CV.jpg, len);
    pthread_mutex_unlock(&nplk);
    if (!j) { http_reply(c, 404, "Not Found"); return 1; }
    char h[256];
    const int n = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\nServer: audiolaz-roomd\r\n"
                           "Cache-Control: no-cache\r\nConnection: close\r\n\r\n", len);
    if (!send_all(c, h, (size_t)n) && strcmp(method, "HEAD")) send_all(c, j, len);
    free(j);
    return 1;
}

/* ── The bridge's questions: an LMS CLI, the few commands squeeze2raop asks ──────────────────────────────────── */
static void np_pct(char *out, size_t len, const char *s, size_t max) {   /* every byte as %XX: no tag can be read inside a value */
    static const char H[] = "0123456789ABCDEF";
    size_t w = 0, n = 0;
    while (*s) {                                     /* a whole character at a time: never cut inside one */
        const unsigned char c = (unsigned char)*s;
        size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1, k = 0;
        while (k < need && s[k]) k++;
        if (n + k > max || w + 3 * k + 1 > len) break;
        for (size_t i = 0; i < k; i++) { out[w++] = '%'; out[w++] = H[(unsigned char)s[i] >> 4]; out[w++] = H[(unsigned char)s[i] & 15]; }
        s += k; n += k;
    }
    out[w] = 0;
}
static void *np_cli_conn(void *arg) {
    const int fd = (int)(intptr_t)arg;
    timeouts(fd, 2000);
    struct timeval tv = { 1200, 0 };                  /* it keeps its connection 15 min after its last question */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char in[2048];
    size_t got = 0;
    for (;;) {
        ssize_t k = recv(fd, in + got, sizeof in - 1 - got, 0);
        if (k <= 0) break;
        got += (size_t)k; in[got] = 0;
        char *nl;
        while ((nl = memchr(in, '\n', got))) {
            const size_t L = (size_t)(nl - in);
            char line[2048];
            memcpy(line, in, L); line[L] = 0;
            line[strcspn(line, "\r")] = 0;
            memmove(in, nl + 1, got - L - 1); got -= L + 1; in[got] = 0;

            char cmd[32] = "";
            const char *sp = strchr(line, ' ');
            if (sp) sscanf(sp + 1, "%31s", cmd);
            const size_t ll = strlen(line);
            const int ask = ll >= 2 && !strcmp(line + ll - 2, " ?");
            static char out[8192];
            if (ask) {
                line[ll - 2] = 0;
                snprintf(out, sizeof out, "%s %s\n", line, !strcmp(cmd, "mode") ? "play" : !strcmp(cmd, "time") ? "0" : !strcmp(cmd, "power") ? "1" : "");
            } else if (!strcmp(cmd, "status")) {
                np_view_t v;
                np_view(&v, 1, 0);
                pthread_mutex_lock(&nplk);
                if (FL.on) FL.served = 1;
                pthread_mutex_unlock(&nplk);
                struct sockaddr_in me;
                socklen_t ml = sizeof me;
                char ip[INET_ADDRSTRLEN] = "127.0.0.1", url[160], name[96];
                if (!getsockname(fd, (struct sockaddr *)&me, &ml)) inet_ntop(AF_INET, &me.sin_addr, ip, sizeof ip);
                snprintf(url, sizeof url, "http://%s:%d/cover/%u.jpg", ip, http_port_, v.ser);
                pthread_mutex_lock(&clk);
                snprintf(name, sizeof name, "%s", me_name);
                pthread_mutex_unlock(&clk);
                char et[3 * 180 + 4], ea[3 * 180 + 4], el[3 * 180 + 4], eu[3 * 160 + 4], en[3 * 96 + 4];
                np_pct(et, sizeof et, v.title, 180); np_pct(ea, sizeof ea, v.artist, 180); np_pct(el, sizeof el, v.album, 180);
                np_pct(eu, sizeof eu, url, 160); np_pct(en, sizeof en, name, 96);
                /* remote 1 without a duration: asked again every 5 s (the header) */
                snprintf(out, sizeof out, "%s player_name%%3A%s player_connected%%3A1 power%%3A1 mode%%3Aplay time%%3A0 "
                         "playlist_cur_index%%3A0 playlist_tracks%%3A1 playlist%%20index%%3A0 id%%3A%u title%%3A%s artist%%3A%s "
                         "album%%3A%s remote%%3A1 artwork_url%%3A%s\n", line, en, v.ser, et, ea, el, eu);
            } else snprintf(out, sizeof out, "%s\n", line);
            if (send_all(fd, out, strlen(out))) { close(fd); return NULL; }
        }
        if (got >= sizeof in - 1) got = 0;                          /* a line longer than all of it: dropped */
    }
    close(fd);
    return NULL;
}
static void *np_cli_main(void *arg) {
    const int s = (int)(intptr_t)arg;
    for (;;) {
        const int c = accept(s, NULL, NULL);
        if (c < 0) { usleep(100000); continue; }
        pthread_t th;
        if (pthread_create(&th, NULL, np_cli_conn, (void *)(intptr_t)c)) { close(c); continue; }
        pthread_detach(th);
    }
    return NULL;
}
/* With the bridge's discovery (disc_start): on its address only (the loopback; the Mac's own). 0 when it listens. */
static int np_cli_start(void) {
    if (np_cli <= 0) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0), on = 1;
    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)np_cli), .sin_addr.s_addr = disc_ip ? disc_ip : htonl(INADDR_LOOPBACK) };
    pthread_t th;
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4) || pthread_create(&th, NULL, np_cli_main, (void *)(intptr_t)s)) {
        nlog("now playing: the CLI's port %d not ours (%s): the AirPlay devices shown no title nor cover", np_cli, strerror(errno));
        close(s); np_cli = 0;
        return -1;
    }
    pthread_detach(th);
    np_start();
    return 0;
}

/* ── UPnP: what plays, in the DIDL-Lite of SetAVTransportURI (the stream's start) ─────────────────────────────── */
static void np_didl(char *out, size_t len, const char *ip) {
    np_view_t v;
    np_view(&v, 0, 0);
    char t[NP_TXT * 6], a[NP_TXT * 6], l[NP_TXT * 6];
    xml_escape(v.title, t, sizeof t); xml_escape(v.artist, a, sizeof a); xml_escape(v.album, l, sizeof l);
    int n = snprintf(out, len, "<dc:title>%s</dc:title>", t);
    if (a[0] && n >= 0 && (size_t)n < len) n += snprintf(out + n, len - (size_t)n, "<upnp:artist>%s</upnp:artist><dc:creator>%s</dc:creator>", a, a);
    if (l[0] && n >= 0 && (size_t)n < len) n += snprintf(out + n, len - (size_t)n, "<upnp:album>%s</upnp:album>", l);
    if (n >= 0 && (size_t)n < len) snprintf(out + n, len - (size_t)n, "<upnp:albumArtURI>http://%s:%d/cover/%u.jpg</upnp:albumArtURI>", ip, http_port_, v.ser);
}

/* ── The page's POST /nowplaying (page.c hands it over) ────────────────────────────────────────────────────── */
static void np_qval(const char *q, const char *key, char *out, size_t len, int *found) {
    out[0] = 0; if (found) *found = 0;
    const size_t kl = strlen(key);
    for (const char *p = q; p && *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t w = 0;
            for (; *v && *v != '&' && w + 1 < len; v++) {
                if (*v == '%' && isxdigit((unsigned char)v[1]) && isxdigit((unsigned char)v[2])) { char h[3] = { v[1], v[2], 0 }; out[w++] = (char)strtol(h, NULL, 16); v += 2; }
                else out[w++] = *v == '+' ? ' ' : *v;
            }
            out[w] = 0; if (found) *found = 1;
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}
/* POST /nowplaying?src=roon|airplay|spotify[&title=…&artist=…&album=…][&clear=1] (text; a field not given: kept);
 * POST /nowplaying/cover?src=…[&title=…] with the picture as the body (JPEG or PNG, 8 MB at most; an empty body:
 * none), the text with it when given;
 * GET /nowplaying: {"source":…,"title":…,"artist":…,"album":…,"cover":"/cover/<n>.jpg" (the stream server's port),
 * "coverPort":…,"flash":…}; GET /nowplaying/cover.jpg: that picture. 1 when it was one of these (answered). */
int netout_np_http(int c, const char *req, size_t n, const char *method, const char *path, const char *q) {
    if (strcmp(path, "/nowplaying") && strcmp(path, "/nowplaying/cover") && strcmp(path, "/nowplaying/cover.jpg")) return 0;
    static const char OK[] = "{\"ok\":true}", BAD[] = "{\"ok\":false,\"why\":\"bad\"}";
    /* the query read again from the request line itself: the page keeps 511 characters of the path, and three fields
     * of 255 bytes URL-encoded take up to 2.3 KB */
    char qb[4096];
    {
        const char *a = req ? strchr(req, ' ') : NULL, *e = a ? strpbrk(a + 1, " \r\n") : NULL, *m = a ? memchr(a + 1, '?', e ? (size_t)(e - a - 1) : 0) : NULL;
        if (m && e && (size_t)(e - m) < sizeof qb) { memcpy(qb, m + 1, (size_t)(e - m - 1)); qb[e - m - 1] = 0; q = qb; }
    }
    char src[16];
    np_qval(q, "src", src, sizeof src, NULL);
    int s = -1;
    for (int i = 0; i < NETOUT_NP_N; i++) if (!strcmp(src, NP_SRC[i])) s = i;
    char h[512];
    if (!strcmp(method, "GET") && !strcmp(path, "/nowplaying/cover.jpg")) return np_http_cover(c, method, "/cover/0.jpg"), 1;
    if (!strcmp(method, "GET") && !strcmp(path, "/nowplaying")) {
        np_view_t v;
        np_view(&v, 1, 0);
        if (s >= 0) {                                   /* ?src=: what that source said (playing or not) */
            pthread_mutex_lock(&nplk);
            snprintf(v.title, NP_TXT, "%s", NP[s].title); snprintf(v.artist, NP_TXT, "%s", NP[s].artist); snprintf(v.album, NP_TXT, "%s", NP[s].album);
            v.card = !NP[s].img;
            pthread_mutex_unlock(&nplk);
        }
        char t[NP_TXT * 2], a[NP_TXT * 2], l[NP_TXT * 2], b[2048];
        json_escape(v.title, t, sizeof t); json_escape(v.artist, a, sizeof a); json_escape(v.album, l, sizeof l);
        pthread_mutex_lock(&nplk);
        const int act = np_act, fl = FL.on;
        pthread_mutex_unlock(&nplk);
        const int bl = snprintf(b, sizeof b, "{\"source\":\"%s\",\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\",\"card\":%s,\"cover\":\"/cover/%u.jpg\",\"coverPort\":%d,\"flash\":%s,\"cliPort\":%d}",
                                act >= 0 ? NP_SRC[act] : "", t, a, l, v.card ? "true" : "false", v.ser, http_port_, fl ? "true" : "false", np_cli);
        const int hl = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", bl);
        send_all(c, h, (size_t)hl); send_all(c, b, (size_t)bl);
        return 1;
    }
    const int good = !strcmp(method, "POST") && s >= 0;
    if (good && !strcmp(path, "/nowplaying")) {
        char cl[8];
        np_qval(q, "clear", cl, sizeof cl, NULL);
        if (cl[0] == '1') netout_np_clear(s);
        else {
            char t[NP_TXT * 2], a[NP_TXT * 2], l[NP_TXT * 2];
            int ft, fa, fl;
            np_qval(q, "title", t, sizeof t, &ft); np_qval(q, "artist", a, sizeof a, &fa); np_qval(q, "album", l, sizeof l, &fl);
            netout_np_text(s, ft ? t : NULL, fa ? a : NULL, fl ? l : NULL);
        }
    } else if (good && !strcmp(path, "/nowplaying/cover")) {
        /* the body: what came with the head, then the rest (Content-Length; 10 s at most) */
        const char *hend = strstr(req, "\r\n\r\n");
        const char *clh = NULL;
        for (const char *p = req; !clh && (p = strstr(p, "\r\n")); p += 2) if (!strncasecmp(p, "\r\nContent-Length:", 17)) clh = p;
        const long want = clh && (!hend || clh < hend) ? strtol(clh + 17, NULL, 10) : -1;
        if (!hend || want < 0 || want > (long)NP_IMG_MAX) {
            const int hl = snprintf(h, sizeof h, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", sizeof BAD - 1);
            send_all(c, h, (size_t)hl); send_all(c, BAD, sizeof BAD - 1);
            return 1;
        }
        unsigned char *body = malloc((size_t)want + 1);
        size_t have = n - (size_t)(hend + 4 - req);
        if (have > (size_t)want) have = (size_t)want;
        if (body) memcpy(body, hend + 4, have);
        const double t0 = mono();
        while (body && have < (size_t)want && mono() - t0 < 10) {
            struct pollfd p = { c, POLLIN, 0 };
            if (poll(&p, 1, 1000) <= 0) continue;
            const ssize_t k = recv(c, body + have, (size_t)want - have, 0);
            if (k <= 0) break;
            have += (size_t)k;
        }
        if (body && have == (size_t)want) {
            netout_np_cover(s, body, have);
            char t[NP_TXT * 2], a[NP_TXT * 2], l[NP_TXT * 2];   /* the text with it, when given: one change */
            int ft, fa, fl;
            np_qval(q, "title", t, sizeof t, &ft); np_qval(q, "artist", a, sizeof a, &fa); np_qval(q, "album", l, sizeof l, &fl);
            if (ft || fa || fl) netout_np_text(s, ft ? t : NULL, fa ? a : NULL, fl ? l : NULL);
        }
        const int ok = body && have == (size_t)want;
        free(body);
        if (!ok) {
            const int hl = snprintf(h, sizeof h, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", sizeof BAD - 1);
            send_all(c, h, (size_t)hl); send_all(c, BAD, sizeof BAD - 1);
            return 1;
        }
    } else {
        const int hl = snprintf(h, sizeof h, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", sizeof BAD - 1);
        send_all(c, h, (size_t)hl); send_all(c, BAD, sizeof BAD - 1);
        return 1;
    }
    const int hl = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", sizeof OK - 1);
    send_all(c, h, (size_t)hl); send_all(c, OK, sizeof OK - 1);
    return 1;
}
