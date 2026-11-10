/* Shells out to /usr/bin/curl for HTTPS fetches. Keeps Toonui free of
   libcurl/libssl link dependencies and avoids cross-toolchain header
   hassle — Toon already has a recent curl with system CA trust.
   For plain http://, uses lightweight direct sockets to avoid expensive
   fork/popen overhead on Toon 1's 400MHz CPU. */
#define _GNU_SOURCE
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <errno.h>

static int socket_http_fetch(const char * url, char * out, size_t out_max, int timeout_s) {
    const char * p = url + 7;
    const char * slash = strchr(p, '/');
    char host_port[256];
    size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hlen == 0 || hlen >= sizeof(host_port)) return -1;
    memcpy(host_port, p, hlen);
    host_port[hlen] = '\0';

    const char * path = slash ? slash : "/";

    char host[256];
    char port_str[16] = "80";
    char * colon = strchr(host_port, ':');
    if (colon) {
        size_t host_len = (size_t)(colon - host_port);
        if (host_len >= sizeof(host)) return -1;
        memcpy(host, host_port, host_len);
        host[host_len] = '\0';
        snprintf(port_str, sizeof(port_str), "%s", colon + 1);
    } else {
        snprintf(host, sizeof(host), "%s", host_port);
    }

    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        return -2;
    }

    int s = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s < 0) continue;
        struct timeval tv = { .tv_sec = timeout_s > 0 ? timeout_s : 8, .tv_usec = 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    if (s < 0) return -2;

    char req[1024];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: toonui/1.0\r\n"
        "Connection: close\r\n\r\n",
        path, host);
    if (req_len <= 0 || req_len >= (int)sizeof(req) || send(s, req, req_len, 0) != req_len) {
        close(s);
        return -2;
    }

    /* Read header first into a scratch buffer */
    char hdr_buf[2048];
    size_t hdr_got = 0;
    char * body_start = NULL;

    while (hdr_got < sizeof(hdr_buf) - 1) {
        ssize_t n = recv(s, hdr_buf + hdr_got, sizeof(hdr_buf) - 1 - hdr_got, 0);
        if (n <= 0) break;
        hdr_got += (size_t)n;
        hdr_buf[hdr_got] = '\0';
        body_start = strstr(hdr_buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            break;
        }
        body_start = strstr(hdr_buf, "\n\n");
        if (body_start) {
            body_start += 2;
            break;
        }
    }

    if (!body_start) {
        close(s);
        return -2;
    }

    /* Check status code: HTTP/1.x 200 */
    int status = 0;
    if (sscanf(hdr_buf, "HTTP/%*d.%*d %d", &status) != 1 || status != 200) {
        close(s);
        /* If 3xx redirect, return -1 so curl -L can follow it */
        if (status >= 300 && status < 400) return -1;
        return -2;
    }

    size_t body_pre = hdr_got - (size_t)(body_start - hdr_buf);
    size_t to_copy = body_pre;
    if (to_copy >= out_max - 1) to_copy = out_max - 1;
    memcpy(out, body_start, to_copy);
    size_t total_body = to_copy;

    while (total_body < out_max - 1) {
        ssize_t n = recv(s, out + total_body, out_max - 1 - total_body, 0);
        if (n <= 0) break;
        total_body += (size_t)n;
    }
    out[total_body] = '\0';
    close(s);
    return 0;
}

static int curl_fetch(const char * url, char * out, size_t out_max, int timeout_s, int verify_tls) {
    if (!url || !out || out_max < 16) return -1;
    if (timeout_s < 1) timeout_s = 8;

    /* Fast-path: try direct socket fetch for plain http:// endpoints to avoid
     * expensive fork/popen(/usr/bin/curl) on Toon 1's 400MHz CPU.
     * Falls back to curl ONLY for redirects (3xx). If host lookup or connection
     * failed (-2), curl will also fail — return -1 immediately without forking. */
    if (!verify_tls && strncasecmp(url, "http://", 7) == 0) {
        int s_rc = socket_http_fetch(url, out, out_max, timeout_s);
        if (s_rc == 0) return 0;
        if (s_rc == -2) return -1;
    }

    /* Sanity check the URL — only allow http/https and pre-escaped chars,
       so we can pass it via popen without further quoting. */
    for (const char * p = url; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '\'' || c == '"' || c == '\\' || c == '`'
            || c == ' ' || c == '\n' || c == '\r') return -1;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s -4 %s-L --compressed --max-time %d --connect-timeout 4 "
        "-A 'toonui/1.0' '%s'", verify_tls ? "" : "-k ", timeout_s, url);
    FILE * fp = popen(cmd, "r");
    if (!fp) return -1;
    size_t got = 0;
    while (got < out_max - 1) {
        size_t k = fread(out + got, 1, out_max - 1 - got, fp);
        if (k == 0) break;
        got += k;
    }
    out[got] = 0;
    int rc = pclose(fp);
    /* A response larger than the buffer fills it and we stop reading; curl then
     * hits SIGPIPE on the now-closed pipe and exits non-zero. The prefix we DID
     * read is still usable (e.g. the newest release's tag_name sits near the top
     * of a /releases listing), so treat a full buffer as success regardless of
     * curl's exit code — otherwise a big payload silently fails the whole fetch. */
    if (got >= out_max - 1) return 0;
    return (rc == 0 && got > 0) ? 0 : -1;
}

/* Data feeds (weather, news, waste schedules, ...): certificate checks off.
 * The Toon's CA store / OpenSSL can't be relied on to validate every feed's
 * chain, and these responses are only ever displayed, never executed. The URL
 * is still character-filtered above, so a response can't inject commands. */
int http_fetch_to(const char * url, char * out, size_t out_max, int timeout_s) {
    return curl_fetch(url, out, out_max, timeout_s, 0);
}

/* Anything that decides what code gets installed or run (marketplace catalog,
 * release metadata, downloaded plugin scripts) uses this: https only, TLS
 * verified (no -k), no plain-http fast path. */
int http_fetch_secure(const char * url, char * out, size_t out_max, int timeout_s) {
    if (!url || strncasecmp(url, "https://", 8) != 0) return -1;
    return curl_fetch(url, out, out_max, timeout_s, 1);
}

int http_fetch(const char * url, char * out, size_t out_max) {
    return http_fetch_to(url, out, out_max, 8);
}

/* Like http_fetch, but reads AND writes a curl cookie jar (-b/-c) so a prior
   login's session cookie (e.g. Domoticz's DMZSID) carries over and
   authenticates the request. `jar` is a writable file path we control. */
int http_fetch_cookies(const char * url, const char * jar, char * out, size_t out_max) {
    if (!url || !jar || !out || out_max < 16) return -1;

    /* Same conservative quoting check as http_fetch — applied to both the URL
       and the jar path since both go into the popen'd command line. */
    for (const char * p = url; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '\'' || c == '"' || c == '\\' || c == '`'
            || c == ' ' || c == '\n' || c == '\r') return -1;
    }
    for (const char * p = jar; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '\'' || c == '"' || c == '\\' || c == '`'
            || c == ' ' || c == '\n' || c == '\r') return -1;
    }

    char cmd[1400];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s -4 -k -L --compressed --max-time 8 --connect-timeout 4 "
        "-A 'toonui/1.0' -c '%s' -b '%s' '%s'", jar, jar, url);
    FILE * fp = popen(cmd, "r");
    if (!fp) return -1;
    size_t got = 0;
    while (got < out_max - 1) {
        size_t k = fread(out + got, 1, out_max - 1 - got, fp);
        if (k == 0) break;
        got += k;
    }
    out[got] = 0;
    int rc = pclose(fp);
    /* A response larger than the buffer fills it and we stop reading; curl then
     * hits SIGPIPE on the now-closed pipe and exits non-zero. The prefix we DID
     * read is still usable (e.g. the newest release's tag_name sits near the top
     * of a /releases listing), so treat a full buffer as success regardless of
     * curl's exit code — otherwise a big payload silently fails the whole fetch. */
    if (got >= out_max - 1) return 0;
    return (rc == 0 && got > 0) ? 0 : -1;
}
