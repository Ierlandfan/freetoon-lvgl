/*
 * Schedule parser + writer for happ_thermstat's thermostatProgram config.
 * Data is stored by hcb_config in a quirky JSON shape:
 *   [{"entry": [{"type":"weekly_recurring"}, {"startMin":"0"}, ...]}, ...]
 * — each entry is an array of single-key objects. We hand-parse with strstr.
 */
#include "schedule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

schedule_entry_t schedule_entries[SCHEDULE_MAX];
int              schedule_count = 0;

unsigned schedule_state_color(int state) {
    switch (state) {
        case 0: return 0xff8866;   /* Comfort  — warm orange */
        case 1: return 0x66cc88;   /* Home     — green */
        case 2: return 0x4466cc;   /* Sleep    — blue */
        case 3: return 0xaa66ff;   /* Away     — purple */
        default:return 0x666666;
    }
}
const char * schedule_state_name(int state) {
    switch (state) {
        case 0: return "Comfort";
        case 1: return "Home";
        case 2: return "Sleep";
        case 3: return "Away";
        default:return "--";
    }
}
const char * schedule_day_short(int day) {
    static const char * names[] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
    if (day < 0 || day > 6) return "--";
    return names[day];
}

/* Which Comfort/Home/Sleep/Away program does the weekly schedule say is
 * "active right now" — i.e. the one with the most-recent start time that
 * is at or before this moment, wrapping around the week if every entry
 * is in the future. Returns -1 if there are no entries at all.
 *
 * Used by the home pill + heater-detail "Scheduled" button to switch the
 * Toon out of manual override and pick the right preset to resume from. */
int schedule_program_now(void) {
    if (schedule_count <= 0) return -1;
    time_t t = time(NULL);
    struct tm tm; localtime_r(&t, &tm);
    /* tm_wday: 0=Sun..6=Sat. Toon convention: 0=Mon..6=Sun. */
    int toon_day = (tm.tm_wday + 6) % 7;
    int now_mow  = toon_day * 1440 + tm.tm_hour * 60 + tm.tm_min;

    int best_state    = -1;
    int best_mow      = -1;     /* most-recent ≤ now */
    int latest_state  = -1;
    int latest_mow    = -1;     /* fallback: largest in week (wraps) */
    for (int i = 0; i < schedule_count; i++) {
        const schedule_entry_t * e = &schedule_entries[i];
        int mow = e->start_day * 1440 + e->start_hour * 60 + e->start_min;
        if (mow <= now_mow && mow > best_mow) {
            best_mow   = mow;
            best_state = e->target_state;
        }
        if (mow > latest_mow) {
            latest_mow   = mow;
            latest_state = e->target_state;
        }
    }
    return (best_state >= 0) ? best_state : latest_state;
}

/* ---- low-level HTTP for hcb_config + happ_thermstat ---- */
static int http_get_body(const char* host_path, const char* qs, char* out, size_t outsz) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(10080);
    a.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) { close(s); return -1; }
    char req[1024];
    int n = snprintf(req, sizeof(req),
        "GET /%s?%s HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        host_path, qs);
    if (send(s, req, n, 0) != n) { close(s); return -1; }
    char accum[16384]; size_t acclen = 0;
    while (acclen < sizeof(accum) - 1) {
        ssize_t k = recv(s, accum + acclen, sizeof(accum) - 1 - acclen, 0);
        if (k <= 0) break;
        acclen += (size_t)k;
    }
    accum[acclen] = 0;
    close(s);
    const char* body = strstr(accum, "\r\n\r\n");
    if (body) body += 4; else body = accum;
    size_t blen = strlen(body);
    if (blen >= outsz) blen = outsz - 1;
    memcpy(out, body, blen); out[blen] = 0;
    return 0;
}

/* Find "key":"NNN" or "key":"X" inside [start..end] and return atoi. */
static int find_int(const char * s, const char * e, const char * key, int dflt) {
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char * p = s;
    while (p < e) {
        const char * h = strstr(p, needle);
        if (!h || h >= e) return dflt;
        return atoi(h + strlen(needle));
    }
    return dflt;
}

int schedule_load(void) {
    static char body[12 * 1024];
    if (http_get_body("hcb_config",
        "action=getObjectConfig&package=happ_thermstat&internalAddress=thermostatProgram",
        body, sizeof(body)) != 0) return -1;

    /* Each entry is between {"entry":[ and the matching ]}. We walk through
       all occurrences of '"entry":[' and parse the bracket-balanced segment. */
    schedule_count = 0;
    const char * p = body;
    while ((p = strstr(p, "\"entry\":[")) != NULL && schedule_count < SCHEDULE_MAX) {
        p += strlen("\"entry\":[");
        const char * end = strchr(p, ']');
        if (!end) break;
        schedule_entry_t * e = &schedule_entries[schedule_count++];
        e->start_min   = find_int(p, end, "startMin",       0);
        e->start_hour  = find_int(p, end, "startHour",      0);
        e->start_day   = find_int(p, end, "startDayOfWeek", 0);
        e->end_min     = find_int(p, end, "endMin",         0);
        e->end_hour    = find_int(p, end, "endHour",        0);
        e->end_day     = find_int(p, end, "endDayOfWeek",   0);
        e->target_state= find_int(p, end, "targetState",    0);
        p = end + 1;
    }
    fprintf(stderr, "[sched] loaded %d entries\n", schedule_count);
    return 0;
}

/* Build the JSON payload from schedule_entries, then POST/PUT it via the
   hcb_config setObjectConfig endpoint. Toon takes a query-string `config`
   parameter (URL-encoded JSON). For simplicity we build a minimal JSON. */
static size_t build_json(char * out, size_t outsz) {
    size_t off = 0;
    int n = snprintf(out + off, outsz - off, "{\"device\":[{\"schedule\":[");
    if (n < 0) return 0; off += (size_t)n;
    for (int i = 0; i < schedule_count; i++) {
        const schedule_entry_t * e = &schedule_entries[i];
        n = snprintf(out + off, outsz - off,
            "%s{\"entry\":["
            "{\"type\":\"weekly_recurring\"},"
            "{\"startMin\":\"%d\"},{\"startHour\":\"%d\"},{\"startDayOfWeek\":\"%d\"},"
            "{\"targetState\":\"%d\"},"
            "{\"endMin\":\"%d\"},{\"endHour\":\"%d\"},{\"endDayOfWeek\":\"%d\"}"
            "]}",
            i ? "," : "",
            e->start_min, e->start_hour, e->start_day,
            e->target_state,
            e->end_min, e->end_hour, e->end_day);
        if (n < 0) return 0; off += (size_t)n;
        if (off >= outsz - 64) break;
    }
    n = snprintf(out + off, outsz - off, "]}]}");
    if (n < 0) return 0; off += (size_t)n;
    return off;
}

/* URL-encode a buffer into another. Out must be at least 3*in_len+1. */
static void url_encode(const char * in, size_t in_len, char * out) {
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            out[j++] = '%'; out[j++] = hex[c >> 4]; out[j++] = hex[c & 0xf];
        }
    }
    out[j] = 0;
}

int schedule_save(void) {
    static char json[12 * 1024];
    size_t jn = build_json(json, sizeof(json));
    if (!jn) return -1;

    static char encoded[36 * 1024];
    url_encode(json, jn, encoded);

    static char qs[40 * 1024];
    snprintf(qs, sizeof(qs),
        "action=setObjectConfig&package=happ_thermstat&internalAddress=thermostatProgram&config=%s",
        encoded);

    static char body[1024];
    int rc = http_get_body("hcb_config", qs, body, sizeof(body));
    fprintf(stderr, "[sched] save rc=%d body=%.200s\n", rc, body);
    return rc;
}

static int compare_entries(const void * a, const void * b) {
    const schedule_entry_t * x = (const schedule_entry_t *)a;
    const schedule_entry_t * y = (const schedule_entry_t *)b;
    if (x->start_day != y->start_day) return x->start_day - y->start_day;
    if (x->start_hour != y->start_hour) return x->start_hour - y->start_hour;
    return x->start_min - y->start_min;
}

int schedule_add(const schedule_entry_t * e) {
    if (schedule_count >= SCHEDULE_MAX) return -1;
    schedule_entries[schedule_count++] = *e;
    qsort(schedule_entries, schedule_count, sizeof(schedule_entry_t), compare_entries);
    return 0;
}
int schedule_remove(int idx) {
    if (idx < 0 || idx >= schedule_count) return -1;
    memmove(&schedule_entries[idx], &schedule_entries[idx + 1],
            (schedule_count - idx - 1) * sizeof(schedule_entry_t));
    schedule_count--;
    return 0;
}
int schedule_replace(int idx, const schedule_entry_t * e) {
    if (idx < 0 || idx >= schedule_count) return -1;
    schedule_entries[idx] = *e;
    qsort(schedule_entries, schedule_count, sizeof(schedule_entry_t), compare_entries);
    return 0;
}
