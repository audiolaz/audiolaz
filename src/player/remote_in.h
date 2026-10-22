/* src/player/remote_in.h: part of the audiolaz player (https://audiolaz.com). MIT licence: see LICENSE. */
#ifndef AUDIOLAZ_REMOTE_IN_H
#define AUDIOLAZ_REMOTE_IN_H
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RI_AIR_RUN "/run/audiolaz-airplay/remote"
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0                 /* macOS: none (roomd ignores SIGPIPE) */
#endif

/* The AirPlay receiver's pipe, opened for writing when a receiver reads it (else -1: ENXIO, none there) */
static int ri_air_open(const char *rooms) {
    char p[600];
    const char *paths[2] = { p, RI_AIR_RUN };
    snprintf(p, sizeof p, "%s/airplay-remote", rooms && *rooms ? rooms : "/var/lib/audiolaz");
    for (int i = 0; i < 2; i++) {
        struct stat st;
        if (stat(paths[i], &st) || !S_ISFIFO(st.st_mode)) continue;
        const int fd = open(paths[i], O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) return fd;
    }
    return -1;
}

/* librespot's control port, from the environment, 0: none */
static int ri_spotify_port(void) {
    const char *e = getenv("LIBRESPOT_CONTROL_PORT");
    const long p = e ? strtol(e, NULL, 10) : 0;
    return p > 0 && p < 65536 ? (int)p : 0;
}

/* (22/10, the Windows bench) Windows answers a connection to a closed port of 127.0.0.1 only after its SYN retries,
 * 2 s: the Windows player's /state took 2 s while librespot was not running (Spotify off, or no librespot.exe) and the
 * app's polls of the page gave up. The connection made without blocking, 200 ms at most (librespot on this machine
 * accepts at once); and /state asks that same port no more for 3 s after nobody answered there (POST /remote, a key
 * pressed, always asks). */
static int ri_down_port;
static double ri_down_at;
static double ri_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int ri_spotify_ask(const char *line, char *ans, size_t al, int quick) {
    const int port = ri_spotify_port();
    ans[0] = 0;
    if (!port) return -1;
    if (quick && port == ri_down_port && ri_now() - ri_down_at < 3.0) return -1;
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    fcntl(s, F_SETFD, FD_CLOEXEC);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ok = connect(s, (struct sockaddr *)&a, sizeof a) == 0;
    if (!ok && errno == EINPROGRESS) {
        struct pollfd pc = { .fd = s, .events = POLLOUT };
        int err = 0;
        socklen_t el = sizeof err;
        ok = poll(&pc, 1, 200) == 1 && !getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) && !err;
    }
    if (!ok) { ri_down_port = port; ri_down_at = ri_now(); close(s); return -1; }
    ri_down_port = 0;
    if (ok) {
        char l[64];
        const int n = snprintf(l, sizeof l, "%s\n", line);
        ok = n > 0 && send(s, l, (size_t)n, MSG_NOSIGNAL) == n;
    }
    size_t got = 0;
    while (ok && got + 1 < al) {
        struct pollfd pf = { .fd = s, .events = POLLIN };
        if (poll(&pf, 1, 400) <= 0) break;
        const ssize_t r = recv(s, ans + got, al - 1 - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
        ans[got] = 0;
        if (strchr(ans, '\n')) break;
    }
    close(s);
    ans[strcspn(ans, "\r\n")] = 0;
    return ans[0] ? 0 : -1;
}

/* /state's two keys, added before its closing brace */
static int remote_in_json(char *b, size_t bn, int n, const char *rooms) {
    if (n < 1 || b[n - 1] != '}') return n;
    const int fd = ri_air_open(rooms);
    if (fd >= 0) close(fd);
    char sp[32] = "", js[64] = "";
    if (!ri_spotify_ask("state", sp, sizeof sp, 1)) {
        static const char *const S[] = { "playing", "paused", "loading", "stopped", "inactive" };
        for (size_t i = 0; i < sizeof S / sizeof *S; i++) if (!strcmp(sp, S[i])) snprintf(js, sizeof js, ",\"remoteSpotify\":\"%s\"", S[i]);
    }
    const int m = snprintf(b + n - 1, bn - (size_t)n + 1, ",\"remoteAirplay\":%s%s}", fd >= 0 ? "true" : "false", js);
    if (m > 0 && (size_t)m < bn - (size_t)n + 1) return n - 1 + m;
    b[n - 1] = '}'; b[n] = 0;
    return n;
}

/* a query's value (src=, c=), letters only */
static void ri_arg(const char *q, const char *key, char *out, size_t ol) {
    out[0] = 0;
    const size_t kl = strlen(key);
    for (const char *p = q; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, kl) || p[kl] != '=') continue;
        size_t i = 0;
        for (const char *v = p + kl + 1; *v && *v != '&' && i + 1 < ol; v++) if ((*v >= 'a' && *v <= 'z') || (*v >= 'A' && *v <= 'Z')) out[i++] = *v;
        out[i] = 0;
        return;
    }
}

/* POST /remote: its HTTP code, its answer in out */
static int remote_in_post(const char *rooms, const char *q, char *out, size_t ol) {
    char src[16], cmd[16];
    ri_arg(q, "src", src, sizeof src);
    ri_arg(q, "c", cmd, sizeof cmd);
    static const char *const C[][3] = {   /* the app's word, AirPlay's (shairport-sync's pipe), Spotify's (librespot's) */
        { "play", "play", "play" }, { "pause", "pause", "pause" }, { "playpause", "playpause", "playpause" },
        { "stop", "stop", "pause" }, { "next", "nextitem", "next" }, { "previous", "previtem", "prev" } };
    const char *word = NULL;
    const int air = !strcmp(src, "airplay"), spot = !strcmp(src, "spotify");
    for (size_t i = 0; i < sizeof C / sizeof *C && (air || spot); i++) if (!strcmp(cmd, C[i][0])) word = C[i][air ? 1 : 2];
    if (!word) { snprintf(out, ol, "{\"ok\":false,\"why\":\"bad\"}"); return 400; }
    if (air) {
        const int fd = ri_air_open(rooms);
        if (fd < 0) { snprintf(out, ol, "{\"ok\":false,\"why\":\"none\"}"); return 409; }
        char l[32];
        const int n = snprintf(l, sizeof l, "%s\n", word);
        const int wr = write(fd, l, (size_t)n) == n;
        close(fd);
        if (!wr) { snprintf(out, ol, "{\"ok\":false,\"why\":\"none\"}"); return 409; }
        snprintf(out, ol, "{\"ok\":true}");
        return 200;
    }
    char ans[32];
    if (ri_spotify_ask(word, ans, sizeof ans, 0)) { snprintf(out, ol, "{\"ok\":false,\"why\":\"none\"}"); return 409; }
    if (!strcmp(ans, "inactive")) { snprintf(out, ol, "{\"ok\":false,\"why\":\"inactive\"}"); return 409; }
    if (strcmp(ans, "ok")) { snprintf(out, ol, "{\"ok\":false,\"why\":\"none\"}"); return 409; }
    snprintf(out, ol, "{\"ok\":true}");
    return 200;
}
#endif
