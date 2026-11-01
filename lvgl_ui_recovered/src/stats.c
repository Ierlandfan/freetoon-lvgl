/*
 * Fetches a single RRA archive over HTTP from hcb_rrd and parses the JSON
 * `{"DD-MM-YYYY HH:MM:SS": value, ...}` into a stats_series_t.
 */
#include "stats.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>

#ifdef WASM_BUILD
#include <emscripten.h>
/* Synchronous XHR (yes, deprecated but still works on main thread in Firefox
 * + Chromium) is the simplest way to keep stats_fetch synchronous in WASM.
 * emscripten_fetch's SYNCHRONOUS mode doesn't work on the main thread even
 * with ASYNCIFY unless we spin up a fetch worker, and that's a bigger config
 * change. For our few-hundred-KB RRD JSON fetched once per stats-open the
 * UI stall is <500 ms on LAN — acceptable. */
EM_JS(int, wasm_sync_xhr, (const char * url_p, char * out_p, int outsz), {
    var url = UTF8ToString(url_p);
    var xhr = new XMLHttpRequest();
    try {
        xhr.open('GET', url, false);            // false = synchronous
        xhr.send();
    } catch(e) {
        console.warn('[stats] xhr open/send failed:', e);
        return -1;
    }
    if (xhr.status !== 200) {
        console.warn('[stats] xhr status', xhr.status, 'for', url);
        return -1;
    }
    var body = xhr.responseText;
    var n = lengthBytesUTF8(body);
    if (n + 1 > outsz) n = outsz - 1;
    stringToUTF8(body, out_p, outsz);
    return n;
});

static int http_get_body(const char * path_qs, char * out, size_t outsz) {
    const char * qs = strchr(path_qs, '?');
    if (!qs) return -1;
    char url[768];
    snprintf(url, sizeof url, "/api/rrd?%s", qs + 1);
    /* Synthesise the HTTP framing the rest of stats.c expects so its
     * strstr("\r\n\r\n") body-split still finds the body start. */
    const char hdr[] = "HTTP/1.0 200 OK\r\n\r\n";
    size_t hlen = sizeof hdr - 1;
    if (hlen + 1 >= outsz) return -1;
    memcpy(out, hdr, hlen);
    int n = wasm_sync_xhr(url, out + hlen, (int)(outsz - hlen));
    if (n <= 0) { out[hlen] = 0; return -1; }
    return 0;
}
#else
static int http_get_body(const char * path_qs, char * out, size_t outsz) {
    const char * host = "127.0.0.1";
    char host_buf[64];
    char port_buf[16] = "80";
    int custom_port = 0;
    if (settings.client_mode && settings.master_host[0]) {
        snprintf(host_buf, sizeof(host_buf), "%s", settings.master_host);
        char * colon = strchr(host_buf, ':');
        if (colon) {
            *colon = '\0';
            snprintf(port_buf, sizeof(port_buf), "%s", colon + 1);
            custom_port = 1;
        } else {
            snprintf(port_buf, sizeof(port_buf), "80");
        }
        host = host_buf;
    }

    const char * ports[2];
    int nports = 1;
    if (custom_port) {
        ports[0] = port_buf;
        nports = 1;
    } else if (settings.client_mode) {
        ports[0] = "80";
        ports[1] = "10080";
        nports = 2;
    } else {
        ports[0] = "10080";
        ports[1] = "80";
        nports = 2;
    }

    int s = -1;
    for (int pi = 0; pi < nports && s < 0; pi++) {
        struct addrinfo hints, *res = NULL, *rp;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(host, ports[pi], &hints, &res) != 0 || !res) {
            continue;
        }

        for (rp = res; rp != NULL; rp = rp->ai_next) {
            s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (s < 0) continue;
            struct timeval tv = { .tv_sec = 4, .tv_usec = 0 };
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0) break;
            close(s);
            s = -1;
        }
        freeaddrinfo(res);
    }
    if (s < 0) return -1;

    char req[1024];
    int n = snprintf(req, sizeof(req),
        "GET /%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
        path_qs, host);
    if (send(s, req, n, 0) != n) { close(s); return -1; }
    /* Read everything we can; the hcb_rrd response is small JSON for 5min
       archives (<10 KB). */
    size_t got = 0;
    while (got < outsz - 1) {
        ssize_t k = recv(s, out + got, outsz - 1 - got, 0);
        if (k <= 0) break;
        got += (size_t)k;
    }
    out[got] = 0;
    close(s);
    return 0;
}
#endif

int stats_fetch_range(const char * logger_name, const char * rra,
                      long from_ts, long to_ts, int max_samples,
                      stats_series_t * series) {
    series->n = 0;
    series->min = +1e30; series->max = -1e30;
    if (max_samples <= 0 || max_samples > STATS_MAX_SAMPLES)
        max_samples = STATS_MAX_SAMPLES;
    char qs[320];
    if (from_ts > 0 && to_ts > from_ts) {
        snprintf(qs, sizeof(qs),
            "hcb_rrd?action=getRrdData&loggerName=%s&rra=%s&from=%ld&to=%ld&readableTime=1&nullForNaN=1&samples=%d",
            logger_name, rra, from_ts, to_ts, max_samples);
    } else {
        snprintf(qs, sizeof(qs),
            "hcb_rrd?action=getRrdData&loggerName=%s&rra=%s&readableTime=1&nullForNaN=1&samples=%d",
            logger_name, rra, max_samples);
    }
    static char body[256 * 1024];
    if (http_get_body(qs, body, sizeof(body)) != 0) return -1;

    /* Locate body after \r\n\r\n */
    char * p = strstr(body, "\r\n\r\n");
    p = p ? p + 4 : body;

    /* If error response, bail */
    if (strstr(p, "\"error\"")) {
        fprintf(stderr, "[stats] %s/%s error: %.200s\n", logger_name, rra, p);
        return -2;
    }

    /* Walk the JSON: find each "DD-MM-YYYY HH:MM:SS": <value-or-null> pair. */
    while (series->n < max_samples) {
        char * key_start = strchr(p, '"');
        if (!key_start) break;
        char * key_end = strchr(key_start + 1, '"');
        if (!key_end) break;
        /* Skip closing brace */
        if (key_end - key_start > 25) { p = key_end + 1; continue; }

        /* Key contents */
        char dt[32];
        size_t klen = (size_t)(key_end - key_start - 1);
        if (klen >= sizeof(dt)) klen = sizeof(dt) - 1;
        memcpy(dt, key_start + 1, klen);
        dt[klen] = 0;

        /* After key comes ": <value>" */
        char * colon = strchr(key_end + 1, ':');
        if (!colon) break;
        char * v = colon + 1;
        while (*v == ' ') v++;

        double val;
        if (strncmp(v, "null", 4) == 0 || strncmp(v, "NaN", 3) == 0) {
            val = NAN;
            p = v + (v[0] == 'n' ? 4 : 3);
        } else {
            char * end;
            val = strtod(v, &end);
            p = end;
        }

        /* Filter clearly bogus samples — water_flow once returned
         * -2,029,354 which dragged the chart's Y range so wide that
         * the real trace vanished. The cap has to stay above plausible
         * cumulative meter readings though: elec_quantity_nt is in Wh
         * and easily passes 1e6 (3.4 GWh meter = 3,452,227 Wh in the
         * current capture). Use ±1e10 — covers any house meter, still
         * catches the outlier values that wrecked the chart range. */
        if (!isnan(val) && (val < -1e10 || val > 1e10)) val = NAN;
        series->samples[series->n] = val;
        /* Short label: "DD-MM HH:MM" (positions 0-4 = "DD-MM", 11-15 = "HH:MM") */
        if (klen >= 16) {
            snprintf(series->labels[series->n], sizeof(series->labels[0]),
                     "%c%c-%c%c %c%c:%c%c",
                     dt[0], dt[1], dt[3], dt[4],
                     dt[11], dt[12], dt[14], dt[15]);
        } else {
            strncpy(series->labels[series->n], dt, sizeof(series->labels[0]) - 1);
            series->labels[series->n][sizeof(series->labels[0]) - 1] = 0;
        }
        /* 2-digit year from "DD-MM-YYYY..." (positions 8-9). Used by the Year
         * view to bucket/label across multiple calendar years. */
        if (klen >= 10) {
            series->year2[series->n][0] = dt[8];
            series->year2[series->n][1] = dt[9];
            series->year2[series->n][2] = 0;
        } else {
            series->year2[series->n][0] = 0;
        }
        if (!isnan(val)) {
            if (val < series->min) series->min = val;
            if (val > series->max) series->max = val;
        }
        series->n++;
    }
    if (series->min > series->max) { series->min = 0; series->max = 0; }
    fprintf(stderr, "[stats] %s/%s: %d samples min=%.2f max=%.2f\n",
            logger_name, rra, series->n, series->min, series->max);
    return 0;
}

int stats_fetch(const char * logger_name, const char * rra,
                long window_seconds, int max_samples,
                stats_series_t * series) {
    if (window_seconds > 0) {
        long now = (long)time(NULL);
        return stats_fetch_range(logger_name, rra, now - window_seconds, now, max_samples, series);
    }
    return stats_fetch_range(logger_name, rra, 0, 0, max_samples, series);
}

int stats_elec_flow_5min(stats_series_t * out)  { return stats_fetch("elec_flow",  "5min", 0, STATS_MAX_SAMPLES, out); }
int stats_gas_flow_5min(stats_series_t * out)   { return stats_fetch("gas_flow",   "5min", 0, STATS_MAX_SAMPLES, out); }
int stats_water_flow_5min(stats_series_t * out) { return stats_fetch("water_flow", "5min", 0, STATS_MAX_SAMPLES, out); }
