/*
 * domoticz.c — Domoticz client (lights + blinds).
 *
 * Live state comes over Domoticz's WebSocket at ws://host/json (subprotocol
 * "domoticz") — NO HTTP polling. We open the socket, request the device list
 * once, then re-request it whenever the server pushes a change notification.
 * That keeps us event-driven (a push wakes us) without depending on the exact
 * shape of Domoticz's push payload: any inbound notification simply triggers a
 * fresh getdevices over the same socket. Connection retry + ping keepalive are
 * connection management, not polling.
 *
 * Control (switch/dim/blind) is a fire-and-forget HTTP GET on a detached
 * thread — simple, and the resulting state change comes straight back over the
 * WebSocket as a push.
 */
#define _GNU_SOURCE
#include "domoticz.h"
#include "http.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

domoticz_state_t domoticz_state = {0};

/* Build "http://[user:pass@]host/<path>" for the control GETs. host may include
 * a scheme; we normalise to bare host:port. */
static void build_url(char * out, size_t osz, const char * path) {
    const char * host = settings.domoticz_host;
    if (strncmp(host, "http://", 7) == 0)  host += 7;
    else if (strncmp(host, "https://", 8) == 0) host += 8;
    if (settings.domoticz_user[0])
        snprintf(out, osz, "http://%s:%s@%s/%s",
                 settings.domoticz_user, settings.domoticz_pass, host, path);
    else
        snprintf(out, osz, "http://%s/%s", host, path);
}

/* Copy the string value of "key" : "value" from a JSON object slice. */
static int jstr(const char * p, const char * end, const char * key, char * out, size_t osz) {
    char needle[40];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char * k = strstr(p, needle);
    if (!k || k >= end) { if (osz) out[0] = 0; return 0; }
    k = strchr(k + strlen(needle), ':');
    if (!k || k >= end) { if (osz) out[0] = 0; return 0; }
    k++;
    while (*k == ' ' || *k == '\t') k++;
    if (*k == '"') {
        k++;
        const char * e = strchr(k, '"');
        if (!e || e > end) { if (osz) out[0] = 0; return 0; }
        size_t n = (size_t)(e - k); if (n >= osz) n = osz - 1;
        memcpy(out, k, n); out[n] = 0;
        return 1;
    }
    /* numeric / bool */
    size_t n = 0;
    while (k < end && *k != ',' && *k != '}' && *k != '\r' && *k != '\n' && n + 1 < osz)
        out[n++] = *k++;
    out[n] = 0;
    return n > 0;
}

static int parse_devices(const char * body) {
    int n = 0;
    const char * p = strstr(body, "\"result\"");
    if (!p) return -1;
    while (n < DOMOTICZ_MAX_DEV && (p = strstr(p, "{")) != NULL) {
        const char * end = strstr(p, "}");
        if (!end) break;
        char idx[16] = "", name[40] = "", stype[32] = "", status[40] = "", level[8] = "";
        jstr(p, end, "idx", idx, sizeof idx);
        jstr(p, end, "Name", name, sizeof name);
        jstr(p, end, "SwitchType", stype, sizeof stype);
        jstr(p, end, "Status", status, sizeof status);
        jstr(p, end, "Level", level, sizeof level);
        if (idx[0] && name[0]) {
            domoticz_dev_t * d = &domoticz_state.dev[n];
            d->idx = atoi(idx);
            snprintf(d->name, sizeof d->name, "%s", name);
            if (strstr(stype, "Blind"))      d->kind = DZ_BLIND;
            else if (strstr(stype, "Dimmer")) d->kind = DZ_DIMMER;
            else                              d->kind = DZ_SWITCH;
            d->on = !(strcmp(status, "Off") == 0 || strcmp(status, "Closed") == 0 ||
                      strcmp(status, "Stopped") == 0);
            d->level = (d->kind == DZ_SWITCH) ? -1 : atoi(level);
            n++;
        }
        p = end + 1;
    }
    domoticz_state.count = n;
    return 0;
}

/* ---------------------------------------------------------------- WebSocket */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64enc(const unsigned char * in, int n, char * out) {
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        int v = in[i] << 16;
        if (i + 1 < n) v |= in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? B64[v & 63] : '=';
    }
    out[o] = 0;
}

/* host[:port] from settings (default Domoticz port 8080). */
static void parse_host(char * host, size_t hsz, char * port, size_t psz) {
    const char * h = settings.domoticz_host;
    if (strncmp(h, "http://", 7) == 0) h += 7;
    else if (strncmp(h, "https://", 8) == 0) h += 8;
    snprintf(host, hsz, "%s", h);
    char * slash = strchr(host, '/'); if (slash) *slash = 0;
    char * colon = strrchr(host, ':');
    if (colon) { *colon = 0; snprintf(port, psz, "%s", colon + 1); }
    else snprintf(port, psz, "8080");
}

static int write_n(int fd, const void * buf, size_t n) {
    const char * p = buf; size_t left = n;
    while (left) {
        ssize_t w = send(fd, p, left, MSG_NOSIGNAL);
        if (w <= 0) { if (errno == EINTR) continue; return -1; }
        p += w; left -= (size_t)w;
    }
    return 0;
}
static int read_n(int fd, void * buf, size_t n) {
    char * p = buf; size_t left = n;
    while (left) {
        ssize_t r = recv(fd, p, left, 0);
        if (r == 0) return -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; left -= (size_t)r;
    }
    return 0;
}

/* Send one masked WebSocket frame (opcode 1=text, 9=ping, 10=pong). */
static int ws_send(int fd, int opcode, const unsigned char * data, size_t len) {
    unsigned char hdr[8]; int h = 0;
    hdr[h++] = (unsigned char)(0x80 | opcode);     /* FIN + opcode */
    if (len < 126) hdr[h++] = (unsigned char)(0x80 | len);
    else if (len < 65536) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (unsigned char)((len >> 8) & 0xff);
        hdr[h++] = (unsigned char)(len & 0xff);
    } else return -1;                              /* our frames are tiny */
    unsigned char mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (unsigned char)(rand() & 0xff);
    if (write_n(fd, hdr, h) < 0) return -1;
    if (write_n(fd, mask, 4) < 0) return -1;
    unsigned char tmp[512];
    for (size_t i = 0; i < len; ) {
        size_t chunk = len - i; if (chunk > sizeof tmp) chunk = sizeof tmp;
        for (size_t j = 0; j < chunk; j++) tmp[j] = data[i + j] ^ mask[(i + j) & 3];
        if (write_n(fd, tmp, chunk) < 0) return -1;
        i += chunk;
    }
    return 0;
}

/* Receive one (possibly fragmented) message into buf. Returns payload length,
 * or -1 on error. Replies to pings, ignores pongs, breaks on close. */
static int ws_recv_msg(int fd, char * buf, size_t bufsz) {
    size_t total = 0;
    for (;;) {
        unsigned char h2[2];
        if (read_n(fd, h2, 2) < 0) return -1;
        int fin = h2[0] & 0x80;
        int opcode = h2[0] & 0x0f;
        unsigned long len = h2[1] & 0x7f;          /* server frames are unmasked */
        if (len == 126) {
            unsigned char e[2]; if (read_n(fd, e, 2) < 0) return -1;
            len = ((unsigned long)e[0] << 8) | e[1];
        } else if (len == 127) {
            unsigned char e[8]; if (read_n(fd, e, 8) < 0) return -1;
            len = 0; for (int i = 4; i < 8; i++) len = (len << 8) | e[i];  /* low 32 bits */
        }
        /* read payload into buf (truncating to capacity, draining the rest) */
        size_t room = (total < bufsz - 1) ? bufsz - 1 - total : 0;
        size_t take = (len < room) ? (size_t)len : room;
        if (take && read_n(fd, buf + total, take) < 0) return -1;
        size_t drop = (size_t)len - take;
        while (drop) { char junk[512]; size_t d = drop > sizeof junk ? sizeof junk : drop;
                       if (read_n(fd, junk, d) < 0) return -1; drop -= d; }
        if (opcode == 0x8) return -1;              /* close */
        if (opcode == 0x9) { ws_send(fd, 0xA, (unsigned char *)buf + total, take); continue; }
        if (opcode == 0xA) continue;               /* pong */
        /* text(1) / binary(2) / continuation(0) */
        total += take;
        if (fin) { buf[total] = 0; return (int)total; }
    }
}

/* Open the WS, perform the HTTP upgrade. Returns fd or -1. */
static int ws_connect(void) {
    char host[80], port[8];
    parse_host(host, sizeof host, port, sizeof port);

    struct addrinfo hints = {0}, * res = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    /* connect timeout via non-blocking + select */
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0) { close(fd); return -1; }
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    unsigned char rnd[16];
    FILE * u = fopen("/dev/urandom", "rb");
    if (u) { if (fread(rnd, 1, sizeof rnd, u) != sizeof rnd) { /* fall back */ } fclose(u); }
    for (int i = 0; i < 16; i++) rnd[i] ^= (unsigned char)(rand() & 0xff);
    char key[28]; b64enc(rnd, 16, key);

    char auth[160] = "";
    if (settings.domoticz_user[0]) {
        char up[100]; snprintf(up, sizeof up, "%s:%s",
                               settings.domoticz_user, settings.domoticz_pass);
        char b[140]; b64enc((unsigned char *)up, (int)strlen(up), b);
        snprintf(auth, sizeof auth, "Authorization: Basic %s\r\n", b);
    }
    char req[512];
    int rl = snprintf(req, sizeof req,
        "GET /json HTTP/1.1\r\n"
        "Host: %s:%s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: domoticz\r\n"
        "%s\r\n", host, port, key, auth);
    if (write_n(fd, req, (size_t)rl) < 0) { close(fd); return -1; }

    /* Read response headers up to the blank line. */
    char resp[1024]; size_t got = 0;
    while (got < sizeof resp - 1) {
        ssize_t r = recv(fd, resp + got, 1, 0);
        if (r <= 0) { close(fd); return -1; }
        got += (size_t)r; resp[got] = 0;
        if (got >= 4 && strcmp(resp + got - 4, "\r\n\r\n") == 0) break;
    }
    if (!strstr(resp, " 101")) { close(fd); return -1; }
    return fd;
}

#define DZ_GETDEVICES \
    "{\"event\":\"request\",\"requestid\":1,\"query\":" \
    "\"type=command&param=getdevices&filter=light&used=true&order=Name\"}"

static void * dz_thread(void * arg) {
    (void)arg;
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    static char msg[64 * 1024];
    for (;;) {
        if (!settings.enable_domoticz || !settings.domoticz_host[0]) {
            domoticz_state.connected = 0;
            sleep(5);
            continue;
        }
        int fd = ws_connect();
        if (fd < 0) { domoticz_state.connected = 0; sleep(10); continue; }

        /* Initial device list. */
        if (ws_send(fd, 0x1, (unsigned char *)DZ_GETDEVICES, strlen(DZ_GETDEVICES)) < 0) {
            close(fd); domoticz_state.connected = 0; sleep(10); continue;
        }
        time_t last_ping = time(NULL);
        for (;;) {
            fd_set rs; FD_ZERO(&rs); FD_SET(fd, &rs);
            struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
            int s = select(fd + 1, &rs, NULL, NULL, &tv);
            if (s < 0) { if (errno == EINTR) continue; break; }
            if (s == 0) {                              /* idle → keepalive ping */
                if (ws_send(fd, 0x9, NULL, 0) < 0) break;
                last_ping = time(NULL);
                continue;
            }
            int n = ws_recv_msg(fd, msg, sizeof msg);
            if (n < 0) break;
            if (strstr(msg, "\"result\"")) {           /* a getdevices response */
                parse_devices(msg);
                domoticz_state.connected = 1;
            } else {                                   /* a change push → refresh */
                if (ws_send(fd, 0x1, (unsigned char *)DZ_GETDEVICES,
                            strlen(DZ_GETDEVICES)) < 0) break;
            }
            if (time(NULL) - last_ping > 45) {
                if (ws_send(fd, 0x9, NULL, 0) < 0) break;
                last_ping = time(NULL);
            }
        }
        close(fd);
        domoticz_state.connected = 0;
        sleep(3);                                      /* brief backoff, then reconnect */
    }
    return NULL;
}

int domoticz_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, dz_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}

/* ---- control (async, HTTP) ---- */
typedef struct { int idx; char cmd[16]; int level; } dz_action_t;

static void * action_thread(void * arg) {
    dz_action_t * a = arg;
    char path[160], url[360];
    if (a->cmd[0])
        snprintf(path, sizeof path,
                 "json.htm?type=command&param=switchlight&idx=%d&switchcmd=%s", a->idx, a->cmd);
    else
        snprintf(path, sizeof path,
                 "json.htm?type=command&param=switchlight&idx=%d&switchcmd=Set%%20Level&level=%d",
                 a->idx, a->level);
    build_url(url, sizeof url, path);
    char body[1024];
    http_fetch(url, body, sizeof body);
    free(a);
    return NULL;
}

static void fire(int idx, const char * cmd, int level) {
    dz_action_t * a = calloc(1, sizeof *a);
    if (!a) return;
    a->idx = idx; a->level = level;
    if (cmd) snprintf(a->cmd, sizeof a->cmd, "%s", cmd);
    pthread_t t;
    if (pthread_create(&t, NULL, action_thread, a) == 0) pthread_detach(t);
    else free(a);
}

void domoticz_switch_async(int idx, const char * cmd) { fire(idx, cmd, 0); }
void domoticz_set_level_async(int idx, int level)     { fire(idx, NULL, level); }
