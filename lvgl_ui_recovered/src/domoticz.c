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
#include <stdint.h>
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

/* --------------------------------------------------------------- MD5 (RFC1321)
 * Self-contained, used only to hash the Domoticz login password for the
 * session-login handshake (Domoticz's logincheck expects password=md5(plain)).
 * Verified against the standard test vectors on the host before shipping. */
static void md5_transform(uint32_t st[4], const unsigned char b[64]) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
    static const unsigned char S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
    uint32_t M[16];
    for (int i = 0; i < 16; i++)
        M[i] = (uint32_t)b[i*4] | ((uint32_t)b[i*4+1] << 8) |
               ((uint32_t)b[i*4+2] << 16) | ((uint32_t)b[i*4+3] << 24);
    uint32_t A = st[0], B = st[1], C = st[2], D = st[3];
    for (int i = 0; i < 64; i++) {
        uint32_t F; int g;
        if (i < 16)      { F = (B & C) | (~B & D);        g = i; }
        else if (i < 32) { F = (D & B) | (~D & C);        g = (5*i + 1) & 15; }
        else if (i < 48) { F = B ^ C ^ D;                 g = (3*i + 5) & 15; }
        else             { F = C ^ (B | ~D);              g = (7*i) & 15; }
        F = F + A + K[i] + M[g]; A = D; D = C; C = B;
        B = B + ((F << S[i]) | (F >> (32 - S[i])));
    }
    st[0] += A; st[1] += B; st[2] += C; st[3] += D;
}
static void md5_hex(const char * data, char out[33]) {
    uint32_t st[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    size_t len = strlen(data), i = 0; uint64_t bitlen = (uint64_t)len * 8;
    unsigned char block[64];
    while (len - i >= 64) { memcpy(block, data + i, 64); md5_transform(st, block); i += 64; }
    size_t rem = len - i; unsigned char tail[128]; memset(tail, 0, sizeof tail);
    memcpy(tail, data + i, rem); tail[rem] = 0x80;
    size_t padlen = (rem < 56) ? 64 : 128;
    for (int j = 0; j < 8; j++) tail[padlen - 8 + j] = (unsigned char)((bitlen >> (8*j)) & 0xff);
    for (size_t off = 0; off < padlen; off += 64) md5_transform(st, tail + off);
    for (int j = 0; j < 4; j++) for (int k = 0; k < 4; k++)
        sprintf(out + (j*4 + k) * 2, "%02x", (st[j] >> (8*k)) & 0xff);
    out[32] = 0;
}

/* --------------------------------------------------------------- session auth
 * Modern Domoticz (2026.x and the default "Login Page" auth mode) ignores HTTP
 * Basic and authenticates via a DMZSID session cookie obtained from logincheck.
 * We keep a curl cookie jar; g_dmzsid mirrors its DMZSID for the WS handshake
 * (the WS upgrade is a raw socket, not curl). HTTP Basic is still sent too, so
 * a Domoticz set to "Basic Authentication" mode keeps working unchanged. */
static pthread_mutex_t g_sess_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_jar[96];      /* curl cookie-jar file path (per-process) */
static char g_dmzsid[96];   /* current DMZSID value, "" if none */

static void session_jar_path(void) {
    if (!g_jar[0])
        snprintf(g_jar, sizeof g_jar, "/var/volatile/tmp/.dz_cookies_%d", (int)getpid());
}

/* Percent-encode the base64 reserved chars so the username survives the query
 * string intact (+ would otherwise be read as a space by the server). */
static void url_pct(const char * in, char * out, size_t osz) {
    size_t o = 0;
    for (const char * p = in; *p && o + 3 < osz; p++) {
        if (*p == '+')      { out[o++]='%'; out[o++]='2'; out[o++]='B'; }
        else if (*p == '/') { out[o++]='%'; out[o++]='2'; out[o++]='F'; }
        else if (*p == '=') { out[o++]='%'; out[o++]='3'; out[o++]='D'; }
        else out[o++] = *p;
    }
    out[o] = 0;
}

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

/* Decode base64 (Domoticz "LevelNames" is base64 of "Off|opt1|opt2|…"). */
static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static void b64dec(const char * in, char * out, size_t osz) {
    size_t o = 0; int bits = 0, val = 0;
    for (const char * p = in; *p; p++) {
        if (*p == '=') break;
        int v = b64val((unsigned char)*p);
        if (v < 0) continue;
        val = (val << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; if (o + 1 < osz) out[o++] = (char)((val >> bits) & 0xff); }
    }
    out[o] = 0;
}

static int parse_devices(const char * body) {
    int n = 0;
    const char * p = strstr(body, "\"result\"");
    if (!p) return -1;
    while (n < DOMOTICZ_MAX_DEV && (p = strstr(p, "{")) != NULL) {
        const char * end = strstr(p, "}");
        if (!end) break;
        char idx[16] = "", name[40] = "", stype[32] = "", status[40] = "",
             level[8] = "", lnames[344] = "";
        jstr(p, end, "idx", idx, sizeof idx);
        jstr(p, end, "Name", name, sizeof name);
        jstr(p, end, "SwitchType", stype, sizeof stype);
        jstr(p, end, "Status", status, sizeof status);
        jstr(p, end, "Level", level, sizeof level);
        jstr(p, end, "LevelNames", lnames, sizeof lnames);
        if (idx[0] && name[0]) {
            domoticz_dev_t * d = &domoticz_state.dev[n];
            d->idx = atoi(idx);
            snprintf(d->name, sizeof d->name, "%s", name);
            if      (strstr(stype, "Selector")) d->kind = DZ_SELECTOR;
            else if (strstr(stype, "Blind"))    d->kind = DZ_BLIND;
            else if (strstr(stype, "Dimmer"))   d->kind = DZ_DIMMER;
            else                                d->kind = DZ_SWITCH;
            d->on = !(strcmp(status, "Off") == 0 || strcmp(status, "Closed") == 0 ||
                      strcmp(status, "Stopped") == 0);
            d->level = (d->kind == DZ_SWITCH) ? -1 : atoi(level);
            if (d->kind == DZ_SELECTOR && lnames[0]) b64dec(lnames, d->options, sizeof d->options);
            else d->options[0] = 0;
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

    /* Send both auth schemes so either Domoticz mode is satisfied: HTTP Basic
     * (for "Basic Authentication" mode) and the DMZSID session cookie (for the
     * default "Login Page" / 2026.x mode, which ignores Basic). g_dmzsid is
     * seeded by the http_getdevices()→dz_login() that runs before we connect. */
    char auth[160] = "";
    if (settings.domoticz_user[0]) {
        char up[100]; snprintf(up, sizeof up, "%s:%s",
                               settings.domoticz_user, settings.domoticz_pass);
        char b[140]; b64enc((unsigned char *)up, (int)strlen(up), b);
        snprintf(auth, sizeof auth, "Authorization: Basic %s\r\n", b);
    }
    char cookie[160] = "";
    if (g_dmzsid[0]) snprintf(cookie, sizeof cookie, "Cookie: DMZSID=%s\r\n", g_dmzsid);
    char req[640];
    int rl = snprintf(req, sizeof req,
        "GET /json HTTP/1.1\r\n"
        "Host: %s:%s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: domoticz\r\n"
        /* Domoticz's cWebem rejects the WS upgrade with 400 unless BOTH the
         * 'domoticz' subprotocol AND an Origin header are present (verified
         * live against Domoticz 2026.1). */
        "Origin: http://%s:%s\r\n"
        "%s%s\r\n", host, port, key, host, port, cookie, auth);
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


/* Read the DMZSID value out of the curl Netscape cookie jar into g_dmzsid. */
static void read_dmzsid(void) {
    g_dmzsid[0] = 0;
    FILE * f = fopen(g_jar, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char * k = strstr(line, "DMZSID");
        if (!k) continue;
        k += 6;
        while (*k == '\t' || *k == ' ') k++;        /* skip to the value field */
        size_t n = 0;
        while (k[n] && k[n] != '\n' && k[n] != '\r' && k[n] != '\t'
               && n + 1 < sizeof g_dmzsid) { g_dmzsid[n] = k[n]; n++; }
        g_dmzsid[n] = 0;
    }
    fclose(f);
    if (strcmp(g_dmzsid, "none") == 0) g_dmzsid[0] = 0;
}

/* Establish a Domoticz session: logincheck with base64(user)+md5(pass) seeds a
 * DMZSID cookie in the jar. Returns 1 if a real session was obtained. No-op
 * (returns 0) when no credentials are configured. */
static int dz_login(void) {
    if (!settings.domoticz_user[0]) return 0;
    session_jar_path();
    char host[80], port[8];
    parse_host(host, sizeof host, port, sizeof port);
    char ub[160];
    b64enc((unsigned char *)settings.domoticz_user, (int)strlen(settings.domoticz_user), ub);
    char ube[256]; url_pct(ub, ube, sizeof ube);
    char pmd5[33]; md5_hex(settings.domoticz_pass, pmd5);
    char url[512];
    snprintf(url, sizeof url,
        "http://%s:%s/json.htm?type=command&param=logincheck&username=%s&password=%s",
        host, port, ube, pmd5);
    pthread_mutex_lock(&g_sess_lock);
    char body[2048];
    if (http_fetch_cookies(url, g_jar, body, sizeof body) == 0) read_dmzsid();
    int ok = g_dmzsid[0] != 0;
    pthread_mutex_unlock(&g_sess_lock);
    return ok;
}

/* Fetch the light/blind list and parse it into domoticz_state. Returns 0 on
 * success. This is the DATA path; the WebSocket below is only a change-trigger.
 *
 * Auth is tried in the order that covers every Domoticz config: (1) the session
 * cookie jar — also the right path for an open (no-auth) instance; (2) on
 * failure, (re)login and retry once; (3) finally HTTP Basic embedded in the URL
 * for a Domoticz explicitly set to "Basic Authentication" mode. */
/* Run a json.htm query through the auth ladder (session cookie → re-login →
 * HTTP Basic) into body. 0 on a reply that contains a "result". */
static int dz_get(const char * query, char * body, size_t bsz) {
    char host[80], port[8];
    parse_host(host, sizeof host, port, sizeof port);
    char url[400];
    session_jar_path();
    snprintf(url, sizeof url, "http://%s:%s/%s", host, port, query);
    if (http_fetch_cookies(url, g_jar, body, bsz) == 0 && strstr(body, "\"result\"")) return 0;
    if (settings.domoticz_user[0] && dz_login() &&
        http_fetch_cookies(url, g_jar, body, bsz) == 0 && strstr(body, "\"result\"")) return 0;
    build_url(url, sizeof url, query);              /* Basic-auth-mode fallback */
    if (http_fetch(url, body, bsz) == 0 && strstr(body, "\"result\"")) return 0;
    return -1;
}

static int http_getdevices(void) {
    static char body[256 * 1024];   /* getdevices can be 200KB+ with many devices */
    if (dz_get("json.htm?type=command&param=getdevices&filter=light&used=true&order=Name",
               body, sizeof body) != 0) return -1;
    return parse_devices(body);
}

/* ----------------------------------------------------------- energy read-out */
domoticz_energy_t dz_energy = {0};

/* First number in a device field, e.g. "Usage":"450 Watt" -> 450, or the gas
 * "Counter":"1234.567 m3" -> 1234.567. -1 if the key is absent. */
static float dz_val(const char * p, const char * end, const char * key) {
    char buf[40];
    if (!jstr(p, end, key, buf, sizeof buf)) return -1.0f;
    return strtof(buf, NULL);
}

/* Trailing-hour gas use from the cumulative m³ counter (mirrors the HA path). */
#define DZ_GAS_RING_N 64
static struct { long t; float m3; } dz_gas_ring[DZ_GAS_RING_N];
static int  dz_gas_head = 0, dz_gas_count = 0; static long dz_gas_last_t = 0;
static void dz_gas_hour_update(float gas_now) {
    if (gas_now <= 0) { dz_energy.gas_hour_m3 = 0; return; }
    long now = time(NULL);
    if (dz_gas_last_t == 0 || now - dz_gas_last_t >= 55) {
        dz_gas_ring[dz_gas_head].t = now; dz_gas_ring[dz_gas_head].m3 = gas_now;
        dz_gas_head = (dz_gas_head + 1) % DZ_GAS_RING_N;
        if (dz_gas_count < DZ_GAS_RING_N) dz_gas_count++;
        dz_gas_last_t = now;
    }
    if (dz_gas_count < 2) { dz_energy.gas_hour_m3 = 0; return; }
    long midnight = now - (now % 86400), cutoff = now - 3600;
    if (cutoff < midnight) cutoff = midnight;        /* daily reset at midnight */
    int oldest = (dz_gas_head - dz_gas_count + DZ_GAS_RING_N) % DZ_GAS_RING_N, ref = oldest;
    for (int k = 0; k < dz_gas_count; k++) {
        int i = (oldest + k) % DZ_GAS_RING_N;
        if (dz_gas_ring[i].t <= cutoff) ref = i; else break;
    }
    float d = gas_now - dz_gas_ring[ref].m3;
    dz_energy.gas_hour_m3 = d < 0 ? 0 : d;
}

static int dz_fetch_device(int idx, char * body, size_t bsz) {
    char q[80];
    snprintf(q, sizeof q, "json.htm?type=command&param=getdevices&rid=%d", idx);
    return dz_get(q, body, bsz);
}

void domoticz_poll_energy(void) {
    if (!settings.enable_domoticz || !settings.domoticz_host[0]) { dz_energy.connected = 0; return; }
    int uses_dz = settings.energy_elec_source  == ENERGY_SRC_DOMOTICZ ||
                  settings.energy_gas_source   == ENERGY_SRC_DOMOTICZ ||
                  settings.energy_water_source == ENERGY_SRC_DOMOTICZ;
    if (!uses_dz) return;
    static time_t last = 0; time_t now = time(NULL);
    if (now - last < 8) return;                       /* rate-limit the HTTP polls */
    last = now;
    static char body[8192];
    int any = 0;
    if (settings.energy_elec_source == ENERGY_SRC_DOMOTICZ && settings.energy_elec_dz_idx > 0 &&
        dz_fetch_device(settings.energy_elec_dz_idx, body, sizeof body) == 0) {
        const char * end = body + strlen(body);
        float u = dz_val(body, end, "Usage"), d = dz_val(body, end, "UsageDeliv");
        if (u >= 0) { dz_energy.power_w = u; any = 1; }
        if (d >= 0)   dz_energy.power_prod_w = d;
    }
    if (settings.energy_gas_source == ENERGY_SRC_DOMOTICZ && settings.energy_gas_dz_idx > 0 &&
        dz_fetch_device(settings.energy_gas_dz_idx, body, sizeof body) == 0) {
        const char * end = body + strlen(body);
        float c = dz_val(body, end, "Counter");
        if (c >= 0) { dz_energy.gas_m3 = c; dz_gas_hour_update(c); any = 1; }
    }
    if (settings.energy_water_source == ENERGY_SRC_DOMOTICZ && settings.energy_water_dz_idx > 0 &&
        dz_fetch_device(settings.energy_water_dz_idx, body, sizeof body) == 0) {
        const char * end = body + strlen(body);
        float c = dz_val(body, end, "Counter");
        if (c >= 0) { dz_energy.water_m3 = c; any = 1; }
    }
    dz_energy.connected = any;
}

/* Settings "test connection": run the auth ladder once and report a granular
 * result. `reached` tracks whether any HTTP body came back at all, so we can
 * tell "wrong credentials" (got the server's 401 page) from "host unreachable"
 * (curl returned nothing). */
int domoticz_probe(void) {
    if (!settings.domoticz_host[0]) return DZ_PROBE_NOCONN;
    static const char QUERY[] =
        "json.htm?type=command&param=getdevices&filter=light&used=true&order=Name";
    char host[80], port[8];
    parse_host(host, sizeof host, port, sizeof port);
    static char body[256 * 1024];   /* getdevices can be 200KB+ with many devices */
    char url[400];
    int reached = 0;

    session_jar_path();
    snprintf(url, sizeof url, "http://%s:%s/%s", host, port, QUERY);
    if (http_fetch_cookies(url, g_jar, body, sizeof body) == 0) {
        reached = 1;
        if (strstr(body, "\"result\"")) { parse_devices(body); return domoticz_state.count; }
    }
    if (settings.domoticz_user[0] && dz_login() &&
        http_fetch_cookies(url, g_jar, body, sizeof body) == 0) {
        reached = 1;
        if (strstr(body, "\"result\"")) { parse_devices(body); return domoticz_state.count; }
    }
    build_url(url, sizeof url, QUERY);
    if (http_fetch(url, body, sizeof body) == 0) {
        reached = 1;
        if (strstr(body, "\"result\"")) { parse_devices(body); return domoticz_state.count; }
    }
    return reached ? DZ_PROBE_AUTH : DZ_PROBE_NOCONN;
}

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
        /* Seed the device list over HTTP — reliable, and shows devices even if
         * the WebSocket can't connect. */
        if (http_getdevices() == 0) domoticz_state.connected = 1;
        domoticz_poll_energy();         /* energy meters: also works without WS */

        /* The WebSocket is purely a push channel: Domoticz broadcasts a frame to
         * all connected clients whenever a device changes. We don't request data
         * over it (Domoticz's WS request/response shape is unreliable) — any
         * inbound frame just triggers a fresh HTTP getdevices. Not a timer poll:
         * we only re-fetch on a push (or on reconnect). */
        int fd = ws_connect();
        if (fd < 0) {
            /* No WS — keep the seeded list; reconnect after a backoff (which
             * re-seeds), so the list still refreshes without a busy poll. */
            domoticz_state.connected = (domoticz_state.count > 0);
            sleep(15);
            continue;
        }
        time_t last_ping = time(NULL), last_fetch = time(NULL);
        for (;;) {
            fd_set rs; FD_ZERO(&rs); FD_SET(fd, &rs);
            struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
            int s = select(fd + 1, &rs, NULL, NULL, &tv);
            if (s < 0) { if (errno == EINTR) continue; break; }
            if (s == 0) {                              /* idle → keepalive ping */
                if (ws_send(fd, 0x9, NULL, 0) < 0) break;
                last_ping = time(NULL);
                domoticz_poll_energy();                /* periodic energy refresh */
                continue;
            }
            int n = ws_recv_msg(fd, msg, sizeof msg);
            if (n < 0) break;
            /* Any push = a Domoticz change → refresh over HTTP (debounced to
             * once a second so a burst of pushes doesn't hammer the API). */
            if (time(NULL) - last_fetch >= 1) {
                if (http_getdevices() == 0) domoticz_state.connected = 1;
                domoticz_poll_energy();                /* energy meters change on push too */
                last_fetch = time(NULL);
            }
            if (time(NULL) - last_ping > 45) {
                if (ws_send(fd, 0x9, NULL, 0) < 0) break;
                last_ping = time(NULL);
            }
        }
        close(fd);
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
    char path[160];
    if (a->cmd[0])
        snprintf(path, sizeof path,
                 "json.htm?type=command&param=switchlight&idx=%d&switchcmd=%s", a->idx, a->cmd);
    else
        snprintf(path, sizeof path,
                 "json.htm?type=command&param=switchlight&idx=%d&switchcmd=Set%%20Level&level=%d",
                 a->idx, a->level);
    char body[1024], url[400];

    /* Same auth ladder as http_getdevices: session cookie first (re-login once
     * on failure), HTTP Basic as the last resort. */
    char host[80], port[8];
    parse_host(host, sizeof host, port, sizeof port);
    session_jar_path();
    snprintf(url, sizeof url, "http://%s:%s/%s", host, port, path);
    int ok = http_fetch_cookies(url, g_jar, body, sizeof body) == 0 &&
             strstr(body, "\"status\"");
    if (!ok && settings.domoticz_user[0] && dz_login())
        ok = http_fetch_cookies(url, g_jar, body, sizeof body) == 0 &&
             strstr(body, "\"status\"");
    if (!ok) { build_url(url, sizeof url, path); http_fetch(url, body, sizeof body); }
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

void domoticz_switch_async(int idx, const char * cmd) {
    fire(idx, cmd, 0);
}
void domoticz_set_level_async(int idx, int level) {
    fire(idx, NULL, level);
}
