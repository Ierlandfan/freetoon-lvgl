/*
 * Fetches a single RRA archive over HTTP from hcb_rrd and parses the JSON
 * `{"DD-MM-YYYY HH:MM:SS": value, ...}` into a stats_series_t.
 */
#include "stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int http_get_body(const char * path_qs, char * out, size_t outsz) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(10080);
    a.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) { close(s); return -1; }
    char req[1024];
    int n = snprintf(req, sizeof(req),
        "GET /%s HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        path_qs);
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

int stats_fetch(const char * logger_name, const char * rra,
                long window_seconds, int max_samples,
                stats_series_t * series) {
    series->n = 0;
    series->min = +1e30; series->max = -1e30;
    if (max_samples <= 0 || max_samples > STATS_MAX_SAMPLES)
        max_samples = STATS_MAX_SAMPLES;
    char qs[320];
    /* hcb_rrd accepts `from=<unix>&to=<unix>` to scope the query to a
     * specific time window. Without that, `samples=N` downsamples
     * across the entire archive span (5yrhours = 5 years, 10yrdays =
     * 10 years), so e.g. `samples=168` from 5yrhours returns 168
     * points spread across 5 years — exactly the "Week tab showed 21
     * days" bug we hit. With `from=now-7d&to=now` the response is
     * scoped to the actual trailing 7 days; `samples=N` then trims
     * within that window. */
    if (window_seconds > 0) {
        long now = (long)time(NULL);
        snprintf(qs, sizeof(qs),
            "hcb_rrd?action=getRrdData&loggerName=%s&rra=%s&from=%ld&to=%ld&readableTime=1&nullForNaN=1&samples=%d",
            logger_name, rra, now - window_seconds, now, max_samples);
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

int stats_elec_flow_5min(stats_series_t * out)  { return stats_fetch("elec_flow",  "5min", 0, STATS_MAX_SAMPLES, out); }
int stats_gas_flow_5min(stats_series_t * out)   { return stats_fetch("gas_flow",   "5min", 0, STATS_MAX_SAMPLES, out); }
int stats_water_flow_5min(stats_series_t * out) { return stats_fetch("water_flow", "5min", 0, STATS_MAX_SAMPLES, out); }
