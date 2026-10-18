/* engine/alsa/airmeta.h's checks : any machine, no ALSA.
 *   cc -std=c99 -O2 -Wall -Wextra -o /tmp/test_airmeta engine/alsa/test_airmeta.c && /tmp/test_airmeta
 * A small bplist00 writer here builds what the senders send (a POST /command's now playing plist: a dict in a dict, an
 * array, ASCII and UTF-16 strings, an int, a real, a bool, a picture as data, a plist inside a data item); the walk
 * must give every leaf with its path and value; broken or hostile plists (cut, a reference out of range, an array that
 * names itself) must end, not crash. The journal's short values too. */
#include "airmeta.h"
#include <stdlib.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ── a bplist00 writer: objects appended, refs of one byte, offsets of two ── */
typedef struct { unsigned char b[1 << 16]; size_t n; unsigned off[256]; int nobj; } W;
static int obj(W *w) { w->off[w->nobj] = (unsigned)w->n; return w->nobj++; }
static void put(W *w, const void *p, size_t k) { memcpy(w->b + w->n, p, k); w->n += k; }
static void mark(W *w, unsigned hi, size_t cnt) {
    unsigned char m;
    if (cnt < 15) { m = (unsigned char)(hi << 4 | cnt); put(w, &m, 1); }
    else { m = (unsigned char)(hi << 4 | 15); put(w, &m, 1); unsigned char i[3] = { 0x11, (unsigned char)(cnt >> 8), (unsigned char)cnt }; put(w, i, 3); }
}
static int ascii(W *w, const char *s) { int k = obj(w); mark(w, 5, strlen(s)); put(w, s, strlen(s)); return k; }
static int utf16(W *w, const unsigned short *s, size_t n) {
    int k = obj(w); mark(w, 6, n);
    for (size_t i = 0; i < n; i++) { unsigned char c[2] = { (unsigned char)(s[i] >> 8), (unsigned char)s[i] }; put(w, c, 2); }
    return k;
}
static int integer(W *w, long v) { int k = obj(w); unsigned char m[5] = { 0x12, (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v }; put(w, m, 5); return k; }
static int real(W *w, double d) {
    int k = obj(w); uint64_t v; memcpy(&v, &d, 8);
    unsigned char m[9] = { 0x23 };
    for (int i = 0; i < 8; i++) m[1 + i] = (unsigned char)(v >> (56 - 8 * i));
    put(w, m, 9); return k;
}
static int boolean(W *w, int t) { int k = obj(w); unsigned char m = t ? 0x09 : 0x08; put(w, &m, 1); return k; }
static int data(W *w, const void *p, size_t n) { int k = obj(w); mark(w, 4, n); put(w, p, n); return k; }
static int array(W *w, const int *refs, size_t n) { int k = obj(w); mark(w, 0xA, n); for (size_t i = 0; i < n; i++) { unsigned char r = (unsigned char)refs[i]; put(w, &r, 1); } return k; }
/* a dict whose keys and values are already written */
static int dict(W *w, const int *keys, const int *vals, size_t n) {
    int k = obj(w); mark(w, 0xD, n);
    for (size_t i = 0; i < n; i++) { unsigned char r = (unsigned char)keys[i]; put(w, &r, 1); }
    for (size_t i = 0; i < n; i++) { unsigned char r = (unsigned char)vals[i]; put(w, &r, 1); }
    return k;
}
static size_t finish(W *w, int top) {
    const size_t table = w->n;
    for (int i = 0; i < w->nobj; i++) { unsigned char o[2] = { (unsigned char)(w->off[i] >> 8), (unsigned char)w->off[i] }; put(w, o, 2); }
    unsigned char t[32] = { 0 };
    t[6] = 2; t[7] = 1;
    for (int i = 0; i < 8; i++) { t[8 + i] = (unsigned char)((uint64_t)w->nobj >> (56 - 8 * i)); t[16 + i] = (unsigned char)((uint64_t)top >> (56 - 8 * i)); t[24 + i] = (unsigned char)((uint64_t)table >> (56 - 8 * i)); }
    put(w, t, 32);
    return w->n;
}
static void start(W *w) { w->n = 0; w->nobj = 0; put(w, "bplist00", 8); }

/* ── what the walk gives ── */
typedef struct { char path[64][160]; char val[64][200]; int kind[64]; int n; } Got;
static void leaf(void *u, const char *path, int kind, const unsigned char *s, size_t n, double num) {
    Got *g = u;
    if (g->n >= 64) return;
    snprintf(g->path[g->n], sizeof g->path[0], "%s", path);
    if (kind == AM_STR) snprintf(g->val[g->n], sizeof g->val[0], "%.*s", (int)n, (const char *)s);
    else if (kind == AM_DATA) snprintf(g->val[g->n], sizeof g->val[0], "data %zu", n);
    else snprintf(g->val[g->n], sizeof g->val[0], "%g", num);
    g->kind[g->n++] = kind;
}
static const char *val_of(const Got *g, const char *path) { for (int i = 0; i < g->n; i++) if (!strcmp(g->path[i], path)) return g->val[i]; return NULL; }

static void check_bplist(void) {
    /* the inner plist (as mrSupportedCommandsFromSender's items are): { "kCommandInfoCommandKey": 2 } */
    static W in;
    start(&in);
    int ik = ascii(&in, "kCommandInfoCommandKey"), iv = integer(&in, 2);
    int itop = dict(&in, &ik, &iv, 1);
    const size_t inn = finish(&in, itop);

    static W w;
    start(&w);
    int k1 = ascii(&w, "type"), v1 = ascii(&w, "updateMRNowPlayingInfo");
    int t1 = ascii(&w, "kMRMediaRemoteNowPlayingInfoTitle"), t1v = ascii(&w, "Le Bureau des légendes");
    const unsigned short elodie[] = { 0xC9, 'l', 'o', 'd', 'i', 'e' };
    int t2 = ascii(&w, "kMRMediaRemoteNowPlayingInfoArtist"), t2v = utf16(&w, elodie, 6);
    int t3 = ascii(&w, "kMRMediaRemoteNowPlayingInfoDuration"), t3v = real(&w, 3120.5);
    int t4 = ascii(&w, "kMRMediaRemoteNowPlayingInfoIsMusicApp"), t4v = boolean(&w, 0);
    unsigned char jpeg[300] = { 0xFF, 0xD8, 0xFF };
    int t5 = ascii(&w, "kMRMediaRemoteNowPlayingInfoArtworkData"), t5v = data(&w, jpeg, sizeof jpeg);
    int t6 = ascii(&w, "kMRMediaRemoteNowPlayingClientBundleIdentifier"), t6v = ascii(&w, "com.canal-plus.mycanal");
    int pk[6] = { t1, t2, t3, t4, t5, t6 }, pv[6] = { t1v, t2v, t3v, t4v, t5v, t6v };
    int pp = dict(&w, pk, pv, 6);
    int a0 = ascii(&w, "a"), a1 = data(&w, in.b, inn);
    int arr[2] = { a0, a1 };
    int ar = array(&w, arr, 2);
    int k2 = ascii(&w, "params"), k3 = ascii(&w, "list"), k4 = ascii(&w, "params");
    int mk[2] = { k4, k3 }, mv[2] = { pp, ar };
    int mid = dict(&w, mk, mv, 2);
    int ok[2] = { k1, k2 }, ov[2] = { v1, mid };
    int top = dict(&w, ok, ov, 2);
    const size_t n = finish(&w, top);

    static Got g;
    memset(&g, 0, sizeof g);
    CHECK(am_bplist_walk(w.b, n, leaf, &g) == 0, "the walk");
    CHECK(g.n == 9, "9 leaves, got %d", g.n);
    const char *v;
    CHECK((v = val_of(&g, "type")) && !strcmp(v, "updateMRNowPlayingInfo"), "type: %s", v ? v : "(none)");
    CHECK((v = val_of(&g, "params.params.kMRMediaRemoteNowPlayingInfoTitle")) && !strcmp(v, "Le Bureau des légendes"), "title: %s", v ? v : "(none)");
    CHECK((v = val_of(&g, "params.params.kMRMediaRemoteNowPlayingInfoArtist")) && !strcmp(v, "\xc3\x89lodie"), "UTF-16 artist: %s", v ? v : "(none)");
    CHECK((v = val_of(&g, "params.params.kMRMediaRemoteNowPlayingInfoDuration")) && !strcmp(v, "3120.5"), "duration: %s", v ? v : "(none)");
    CHECK((v = val_of(&g, "params.params.kMRMediaRemoteNowPlayingInfoIsMusicApp")) && !strcmp(v, "0"), "bool: %s", v ? v : "(none)");
    CHECK((v = val_of(&g, "params.params.kMRMediaRemoteNowPlayingInfoArtworkData")) && !strcmp(v, "data 300"), "artwork: %s", v ? v : "(none)");
    CHECK((v = val_of(&g, "params.list[0]")) && !strcmp(v, "a"), "array: %s", v ? v : "(none)");
    CHECK((v = val_of(&g, "params.list[1]>kCommandInfoCommandKey")) && !strcmp(v, "2"), "a plist in a plist: %s", v ? v : "(none)");
    CHECK(am_key_has(am_leaf_key("params.params.kMRMediaRemoteNowPlayingClientBundleIdentifier"), "bundleidentifier"), "key match");
    CHECK(!strcmp(am_leaf_key("params.list[1]>kCommandInfoCommandKey"), "kCommandInfoCommandKey"), "leaf key after >");

    /* broken: every cut of it ends (no crash), and none gives a leaf past the bytes */
    for (size_t cut = 0; cut < n; cut += 7) { memset(&g, 0, sizeof g); (void)am_bplist_walk(w.b, cut, leaf, &g); }
    /* a reference out of range */
    static W b;
    start(&b);
    int bk = ascii(&b, "x"); int bad = 77;
    int bt = dict(&b, &bk, &bad, 1);
    const size_t bn = finish(&b, bt);
    memset(&g, 0, sizeof g);
    CHECK(am_bplist_walk(b.b, bn, leaf, &g) == -1, "a reference out of range: refused");
    /* an array that names itself 200 times: the budget ends it quickly */
    static W c;
    start(&c);
    int self[200];
    for (int i = 0; i < 200; i++) self[i] = 0;               /* object 0 is the array itself */
    int ct = array(&c, self, 200);
    const size_t cn = finish(&c, ct);
    memset(&g, 0, sizeof g);
    CHECK(am_bplist_walk(c.b, cn, leaf, &g) == -1, "an array naming itself: ended");
    CHECK(am_bplist_walk((const unsigned char *)"not a plist at all, not at all, not at all", 44, leaf, &g) == -1, "not a bplist");
}

static void check_values(void) {
    char out[300];
    am_value(0x736e616du, (const unsigned char *)"CHAMBRE (3)", 11, 80, out, sizeof out);
    CHECK(!strcmp(out, "\"CHAMBRE (3)\""), "text: %s", out);
    unsigned char jpg[1000] = { 0xFF, 0xD8, 0xFF, 0xE0 };
    am_value(0x50494354u, jpg, sizeof jpg, 80, out, sizeof out);
    CHECK(!strcmp(out, "1000 bytes (JPEG)"), "picture: %s", out);
    char longs[300];
    memset(longs, 'a', sizeof longs);
    am_value(0x6d696e6du, (const unsigned char *)longs, sizeof longs, 40, out, sizeof out);
    CHECK(strstr(out, "…\" (300 bytes)") && strlen(out) < 60, "cut: %s", out);
    const unsigned char bin[5] = { 0x00, 0x01, 0xFE, 0x80, 0x7F };
    am_value(0x61736169u, bin, 5, 80, out, sizeof out);
    CHECK(!strcmp(out, "5 bytes: 00 01 fe 80 7f"), "binary: %s", out);
    const unsigned char id4[4] = { 0, 0, 1, 2 };
    am_value(0x61736169u, id4, 4, 80, out, sizeof out);
    CHECK(!strcmp(out, "4 bytes: 00 00 01 02 (258)"), "int32: %s", out);
    am_value(0x70626567u, NULL, 0, 80, out, sizeof out);
    CHECK(!strcmp(out, "(empty)"), "empty: %s", out);
    am_value(0x636f706cu, (const unsigned char *)"bplist00xxxxxxxx", 16, 80, out, sizeof out);
    CHECK(!strcmp(out, "binary plist, 16 bytes"), "plist: %s", out);
    char code[5];
    am_code(0x73736e63u, code);
    CHECK(!strcmp(code, "ssnc"), "code: %s", code);
    am_code(0x00ff4142u, code);
    CHECK(!strcmp(code, "??AB"), "code unprintable: %s", code);

    char cl[16];
    am_clean(cl, sizeof cl, (const unsigned char *)" a=b/c|d\"e\\f\n ", 14);
    CHECK(!strcmp(cl, "a b c d e f"), "clean: [%s]", cl);
    am_clean(cl, 8, (const unsigned char *)"Télé été", 11);
    CHECK(!strcmp(cl, "Télé \xc3\xa9") || !strcmp(cl, "Télé"), "UTF-8 cut: [%s]", cl);
    CHECK(am_is_text((const unsigned char *)"Study\0", 6) && !am_is_text((const unsigned char *)"\xff\xfe", 2), "text or not");
}

int main(void) {
    check_bplist();
    check_values();
    if (fails) { printf("airmeta: %d FAILED\n", fails); return 1; }
    printf("airmeta: bplist walk (dicts, arrays, ASCII and UTF-16, numbers, data, a plist inside, broken ones), short values, names OK\n");
    return 0;
}
