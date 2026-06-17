/* Shells out to /usr/bin/curl for HTTPS fetches. Keeps Toonui free of
   libcurl/libssl link dependencies and avoids cross-toolchain header
   hassle — Toon already has a recent curl with system CA trust. */
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int http_fetch_to(const char * url, char * out, size_t out_max, int timeout_s) {
    if (!url || !out || out_max < 16) return -1;
    if (timeout_s < 1) timeout_s = 8;

    /* Sanity check the URL — only allow http/https and pre-escaped chars,
       so we can pass it via popen without further quoting. */
    for (const char * p = url; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '\'' || c == '"' || c == '\\' || c == '`'
            || c == ' ' || c == '\n' || c == '\r') return -1;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s -k -L --max-time %d --connect-timeout 4 "
        "-A 'toonui/1.0' '%s'", timeout_s, url);
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
        "/usr/bin/curl -s -k -L --max-time 8 --connect-timeout 4 "
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
