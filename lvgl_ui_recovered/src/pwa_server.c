/* See pwa_server.h.
 *
 * One detached thread per accepted connection so a long-lived SSE stream
 * doesn't block the accept loop or other clients. Handlers are stateless
 * apart from reads of toon_state / ha_state.
 *
 * Routes:
 *   GET  /                  →  index.html
 *   GET  /manifest.json     →  PWA manifest
 *   GET  /sw.js             →  service-worker stub (install + offline shell)
 *   GET  /icon-192.png      →  PWA icon
 *   GET  /api/state         →  one-shot toon_state JSON (legacy / curl)
 *   GET  /api/state/stream  →  SSE: emits on state change + 10s heartbeat
 *   POST /api/setpoint      →  body {"value": "18.50"} → roomSetpoint write
 *   POST /api/program       →  body {"state": 0..3} (Comfort/Home/Sleep/Away)
 *   POST /api/curtain       →  body {"action": "open|close|stop"} via HA
 *
 * All static files live under PWA_ROOT (/mnt/data/pwa/). */
#define _GNU_SOURCE      /* strcasestr */
#include "pwa_server.h"
#include "boxtalk.h"
#include "homeassistant.h"
#include "schedule.h"
#include "settings.h"
#include "weather.h"
#include "notify.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#define PWA_PORT  10081
#define PWA_ROOT  "/mnt/data/pwa"

static int sock_send_all(int fd, const char * buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t k = send(fd, buf + off, n - off, MSG_NOSIGNAL);
        if (k <= 0) return -1;
        off += (size_t)k;
    }
    return 0;
}

static const char * mime_for(const char * path) {
    const char * dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".css"))  return "text/css";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    return "application/octet-stream";
}

static int send_status(int fd, int code, const char * status, const char * body) {
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
        code, status, strlen(body));
    if (sock_send_all(fd, hdr, n) < 0) return -1;
    return sock_send_all(fd, body, strlen(body));
}

/* Serve a static file from PWA_ROOT. Guards against `..` path-escape. */
static int serve_static(int fd, const char * path) {
    if (strstr(path, "..")) return send_status(fd, 400, "Bad Request", "..");
    char full[512];
    snprintf(full, sizeof(full), "%s%s",
             PWA_ROOT, strcmp(path, "/") == 0 ? "/index.html" : path);
    FILE * f = fopen(full, "rb");
    if (!f) return send_status(fd, 404, "Not Found", "no");
    struct stat st;
    if (stat(full, &st) != 0) { fclose(f); return -1; }
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n",
        mime_for(full), (long long)st.st_size);
    if (sock_send_all(fd, hdr, n) < 0) { fclose(f); return -1; }
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
        if (sock_send_all(fd, buf, r) < 0) { fclose(f); return -1; }
    fclose(f); return 0;
}

/* Pure-function snapshot of the live state into JSON. Reads volatile shared
 * structs once; safe to call from any thread (the writers update atomically
 * field-by-field — readers tolerate a briefly inconsistent frame). */
static int render_state_json(char * body, size_t sz) {
    return snprintf(body, sz,
        "{"
        "\"indoor_temp\":%.2f,"
        "\"setpoint\":%.2f,"
        "\"program_state\":%d,"
        "\"active_state\":%d,"
        "\"burner_on\":%d,"
        "\"dhw_on\":%d,"
        "\"modulation_level\":%.1f,"
        "\"boiler_in_temp\":%.2f,"
        "\"boiler_out_temp\":%.2f,"
        "\"ch_setpoint\":%.2f,"
        "\"water_pressure\":%.2f,"
        "\"humidity\":%.1f,"
        "\"eco2\":%d,"
        "\"tvoc\":%d,"
        "\"ot_comm_error\":%d,"
        "\"connected\":%d,"
        "\"ha_connected\":%d,"
        "\"curtain_state\":\"%s\","
        "\"curtain_pos\":%d,"
        "\"curtain_battery\":%d"
        "}",
        (double)toon_state.indoor_temp,
        (double)toon_state.setpoint,
        toon_state.program_state, toon_state.active_state,
        toon_state.burner_on, toon_state.dhw_on,
        (double)toon_state.modulation_level,
        (double)toon_state.boiler_in_temp,
        (double)toon_state.boiler_out_temp,
        (double)toon_state.ch_setpoint,
        (double)toon_state.water_pressure,
        (double)toon_state.humidity,
        toon_state.eco2, toon_state.tvoc,
        toon_state.ot_comm_error,
        toon_state.connected,
        ha_state.connected,
        ha_state.curtain_state,
        ha_state.curtain_pos,
        ha_state.curtain_battery
    );
}

static int handle_state(int fd) {
    char body[2048];
    int n = render_state_json(body, sizeof(body));
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n", n);
    if (sock_send_all(fd, hdr, hn) < 0) return -1;
    return sock_send_all(fd, body, n);
}

/* SSE stream — emits a `data: {…}` event whenever the JSON snapshot changes,
 * plus a `: keepalive\n\n` comment every ~10s so proxies don't drop the conn.
 * Loops until the client disconnects (sock_send_all returns -1). */
static int handle_state_stream(int fd) {
    const char * hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "X-Accel-Buffering: no\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n";
    if (sock_send_all(fd, hdr, strlen(hdr)) < 0) return -1;
    /* Cap one-second send timeout so a dead peer surfaces fast. */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char last[2048] = "";
    int idle_ticks = 0;
    /* Push the first frame immediately so the client doesn't sit on default values. */
    char body[2048], ev[2200];
    int n = render_state_json(body, sizeof(body));
    if (n > 0) {
        int en = snprintf(ev, sizeof(ev), "data: %s\n\n", body);
        if (sock_send_all(fd, ev, en) < 0) return -1;
        memcpy(last, body, n + 1);
    }
    while (1) {
        usleep(1000 * 1000);   /* 1 Hz poll of shared state */
        n = render_state_json(body, sizeof(body));
        if (n <= 0) continue;
        if (strcmp(body, last) != 0) {
            int en = snprintf(ev, sizeof(ev), "data: %s\n\n", body);
            if (sock_send_all(fd, ev, en) < 0) return -1;
            memcpy(last, body, n + 1);
            idle_ticks = 0;
        } else if (++idle_ticks >= 10) {
            /* No state change for 10s — emit SSE comment line as a heartbeat. */
            if (sock_send_all(fd, ": ka\n\n", 6) < 0) return -1;
            idle_ticks = 0;
        }
    }
}

/* Tiny extractor: find "key":<num> in flat JSON and return it as float.
 * Tolerates whitespace + optional quotes around the value. */
static int extract_float(const char * body, const char * key, float * out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char * p = strstr(body, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p == '"') p++;
    char * end;
    float v = strtof(p, &end);
    if (end == p) return 0;
    *out = v; return 1;
}
static int extract_str(const char * body, const char * key,
                       char * out, size_t outsz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char * p = strstr(body, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    const char * e = strchr(p, '"');
    if (!e) return 0;
    size_t L = e - p; if (L > outsz - 1) L = outsz - 1;
    memcpy(out, p, L); out[L] = 0;
    return 1;
}

static int handle_setpoint(int fd, const char * body) {
    float v;
    if (!extract_float(body, "value", &v)) {
        return send_status(fd, 400, "Bad Request",
            "{\"err\":\"need {\\\"value\\\":18.50}\"}");
    }
    boxtalk_set_setpoint(v);
    return send_status(fd, 200, "OK", "{\"ok\":1}");
}

/* -------- weekly schedule (Comfort/Home/Sleep/Away) -------------------- */
/* GET → array of {state, start_day, start_hour, start_min, end_day, end_hour,
 *                 end_min}; POST replaces the whole list atomically. The LVGL
 * schedule screen and this endpoint share schedule_entries[] — both keep the
 * canonical hcb_config copy in sync via schedule_load/schedule_save. */
static int handle_schedule_get(int fd) {
    if (schedule_load() != 0)
        return send_status(fd, 502, "Bad Gateway", "{\"err\":\"load\"}");
    char body[8192];
    int off = snprintf(body, sizeof(body), "{\"entries\":[");
    for (int i = 0; i < schedule_count; i++) {
        const schedule_entry_t * e = &schedule_entries[i];
        off += snprintf(body + off, sizeof(body) - off,
            "%s{\"state\":%d,\"start_day\":%d,\"start_hour\":%d,\"start_min\":%d,"
            "\"end_day\":%d,\"end_hour\":%d,\"end_min\":%d}",
            i ? "," : "",
            e->target_state, e->start_day, e->start_hour, e->start_min,
            e->end_day, e->end_hour, e->end_min);
    }
    off += snprintf(body + off, sizeof(body) - off, "]}");
    char hdr[128];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n", off);
    if (sock_send_all(fd, hdr, hn) < 0) return -1;
    return sock_send_all(fd, body, off);
}

/* Parse one entry from a JSON object substring starting at `*p`. Advances
 * `*p` past the matching '}'. Returns 1 on success, 0 at end-of-list. */
static int sched_parse_entry(const char ** p, schedule_entry_t * e) {
    const char * q = strchr(*p, '{');
    if (!q) return 0;
    memset(e, 0, sizeof(*e));
    /* Each int field is "key":NN — small JSON, lookup-by-needle is fine. */
    struct { const char * key; int * out; } fields[] = {
        {"\"state\":",      &e->target_state},
        {"\"start_day\":",  &e->start_day},
        {"\"start_hour\":", &e->start_hour},
        {"\"start_min\":",  &e->start_min},
        {"\"end_day\":",    &e->end_day},
        {"\"end_hour\":",   &e->end_hour},
        {"\"end_min\":",    &e->end_min},
    };
    const char * end = strchr(q, '}');
    if (!end) return 0;
    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); i++) {
        const char * k = strstr(q, fields[i].key);
        if (!k || k > end) return 0;
        *fields[i].out = atoi(k + strlen(fields[i].key));
    }
    *p = end + 1;
    return 1;
}

static int handle_schedule_post(int fd, const char * body) {
    const char * arr = strstr(body, "\"entries\"");
    if (!arr) return send_status(fd, 400, "Bad Request",
        "{\"err\":\"need {\\\"entries\\\":[...]}\"}");
    const char * p = strchr(arr, '[');
    if (!p) return send_status(fd, 400, "Bad Request", "no [");
    p++;
    schedule_count = 0;
    while (schedule_count < SCHEDULE_MAX) {
        const char * cb = strchr(p, '{');
        const char * eb = strchr(p, ']');
        if (!cb || (eb && cb > eb)) break;
        if (!sched_parse_entry(&p, &schedule_entries[schedule_count])) break;
        schedule_count++;
    }
    if (schedule_save() != 0)
        return send_status(fd, 502, "Bad Gateway", "{\"err\":\"save\"}");
    char ok[64];
    snprintf(ok, sizeof(ok), "{\"ok\":1,\"count\":%d}", schedule_count);
    return send_status(fd, 200, "OK", ok);
}

static int handle_program(int fd, const char * body) {
    float v;
    if (!extract_float(body, "state", &v)) {
        return send_status(fd, 400, "Bad Request",
            "{\"err\":\"need {\\\"state\\\":0-3 or -1}\"}");
    }
    int s = (int)v;
    if (s < 0) boxtalk_set_manual();
    else       boxtalk_set_program(s);
    return send_status(fd, 200, "OK", "{\"ok\":1}");
}

/* -------------------------------------------------------------------- */
/* Carrier-agnostic delivery tracker. State lives in /mnt/data/packages.json
 * (JSON array of {id,label,eta,url,place,status,actual_place,added_at,
 * received_at}). pwa_server is the sole writer; no length cap. PWA + HA
 * webhook both POST here. Concurrency: a single coarse mutex around the
 * file, fine for handful-per-day write rate. */
#define PACKAGES_PATH "/mnt/data/packages.json"
static pthread_mutex_t g_pkg_mtx = PTHREAD_MUTEX_INITIALIZER;

static int pkg_read_all(char * out, size_t outsz) {
    FILE * f = fopen(PACKAGES_PATH, "r");
    if (!f) { snprintf(out, outsz, "[]"); return 0; }
    size_t n = fread(out, 1, outsz - 1, f);
    out[n] = 0;
    fclose(f);
    if (n == 0) snprintf(out, outsz, "[]");
    return 0;
}

static int pkg_write_all(const char * data) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), PACKAGES_PATH ".new");
    FILE * f = fopen(tmp, "w");
    if (!f) return -1;
    size_t n = strlen(data);
    int ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    if (!ok) { unlink(tmp); return -1; }
    return rename(tmp, PACKAGES_PATH);
}

static int handle_packages_get(int fd) {
    char body[16384];
    pthread_mutex_lock(&g_pkg_mtx);
    pkg_read_all(body, sizeof(body));
    pthread_mutex_unlock(&g_pkg_mtx);
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n", strlen(body));
    if (sock_send_all(fd, hdr, hn) < 0) return -1;
    return sock_send_all(fd, body, strlen(body));
}

/* Append the POSTed JSON object to the list. Generates id + added_at +
 * defaults status="pending". Caller's body is a single JSON object. */
/* Notify on package state change — native Toon banner (happ_usermsg), no HA.
 * subType is keyed per package so a later DeleteNotification could clear it. */
static void notify_pkg_state_change(const char * merchant, const char * label,
                                    const char * old_status, const char * new_status,
                                    const char * eta) {
    char text[256];
    snprintf(text, sizeof text, "%s: %s — %s (verwacht %s)",
             merchant, new_status, label, eta);
    notify_show("package", merchant, text);
    fprintf(stderr, "[pkg] notify (Toon): %s '%s' %s->%s\n",
            merchant, label, old_status, new_status);
}

static int handle_packages_post(int fd, const char * body) {
    char label[128] = "", eta[16] = "", url[256] = "", place[64] = "",
         source[16] = "manual",
         tracking[64] = "", postal[16] = "",
         merchant[32] = "", order_id[32] = "",
         status[16] = "pending";
    extract_str(body, "label",       label,    sizeof(label));
    extract_str(body, "eta",         eta,      sizeof(eta));
    extract_str(body, "url",         url,      sizeof(url));
    extract_str(body, "place",       place,    sizeof(place));
    extract_str(body, "source",      source,   sizeof(source));
    extract_str(body, "tracking",    tracking, sizeof(tracking));
    extract_str(body, "postal_code", postal,   sizeof(postal));
    extract_str(body, "merchant",    merchant, sizeof(merchant));
    extract_str(body, "order_id",    order_id, sizeof(order_id));
    extract_str(body, "status",      status,   sizeof(status));
    if (!label[0]) return send_status(fd, 400, "Bad Request",
        "{\"err\":\"label required\"}");
    /* default eta = today */
    if (!eta[0]) {
        time_t now = time(NULL);
        struct tm tm; localtime_r(&now, &tm);
        strftime(eta, sizeof(eta), "%Y-%m-%d", &tm);
    }
    char ts[24]; time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));
    /* id needs sub-second resolution so rapid-fire adds don't collide
     * (the earlier seconds-only id duplicated when the email poller
     * dropped a batch). Use ms since epoch from gettimeofday. */
    struct timeval tv; gettimeofday(&tv, NULL);
    char id[32];
    snprintf(id, sizeof(id), "%lld%03d",
             (long long)tv.tv_sec, (int)(tv.tv_usec / 1000));

    /* If the caller supplied a (merchant + order_id) tuple, try upsert
     * first — finds an existing entry with the same pair and rebuilds it
     * (preserving id + added_at). On state advancement we fire HA notify.
     * If no match (or no merchant+order_id provided), fall through to
     * the append-as-new path below. */
    pthread_mutex_lock(&g_pkg_mtx);
    char cur[16384]; pkg_read_all(cur, sizeof(cur));

    if (merchant[0] && order_id[0]) {
        /* Scan objects for matching merchant+order_id. */
        const char * scan = cur;
        char target_m[64], target_o[64];
        snprintf(target_m, sizeof(target_m), "\"merchant\":\"%s\"", merchant);
        snprintf(target_o, sizeof(target_o), "\"order_id\":\"%s\"", order_id);
        while ((scan = strchr(scan, '{'))) {
            const char * obj_end = strchr(scan, '}');
            if (!obj_end) break;
            size_t L = (size_t)(obj_end - scan + 1);
            if (L < sizeof(cur) && memmem(scan, L, target_m, strlen(target_m))
                                && memmem(scan, L, target_o, strlen(target_o))) {
                /* HIT — extract the existing entry's preserved fields and
                 * its current status, then rebuild with new status/eta. */
                char obj[1024]; if (L >= sizeof(obj)) L = sizeof(obj) - 1;
                memcpy(obj, scan, L); obj[L] = 0;
                char xid[32]="", xadded[24]="", xplace[64]="", xactual[64]="",
                     xreceived[24]="", xstatus[16]="", xurl[256]="", xtrk[64]="",
                     xpostal[16]="";
                extract_str(obj, "id",         xid,      sizeof(xid));
                extract_str(obj, "added_at",   xadded,   sizeof(xadded));
                extract_str(obj, "place",      xplace,   sizeof(xplace));
                extract_str(obj, "actual_place", xactual,sizeof(xactual));
                extract_str(obj, "received_at",xreceived,sizeof(xreceived));
                extract_str(obj, "status",     xstatus,  sizeof(xstatus));
                extract_str(obj, "url",        xurl,     sizeof(xurl));
                extract_str(obj, "tracking",   xtrk,     sizeof(xtrk));
                extract_str(obj, "postal_code",xpostal,  sizeof(xpostal));
                /* Don't downgrade status (delivered > shipped > ordered > pending) */
                int rank_old = !strcmp(xstatus,"delivered")?4:
                               !strcmp(xstatus,"shipped")?3:
                               !strcmp(xstatus,"ordered")?2:
                               !strcmp(xstatus,"received")?5:1;
                int rank_new = !strcmp(status,"delivered")?4:
                               !strcmp(status,"shipped")?3:
                               !strcmp(status,"ordered")?2:
                               !strcmp(status,"received")?5:1;
                const char * eff_status = (rank_new > rank_old) ? status : xstatus;
                /* Keep first-seen added_at + existing place/url/etc if caller didn't override */
                if (!place[0])  strcpy(place,  xplace);
                if (!url[0])    strcpy(url,    xurl);
                if (!tracking[0]) strcpy(tracking, xtrk);
                if (!postal[0]) strcpy(postal, xpostal);
                if (!xadded[0]) strcpy(xadded, ts);

                char rebuilt[1024];
                snprintf(rebuilt, sizeof(rebuilt),
                    "{\"id\":\"%s\",\"label\":\"%s\",\"eta\":\"%s\",\"url\":\"%s\","
                    "\"place\":\"%s\",\"actual_place\":\"%s\",\"source\":\"%s\","
                    "\"tracking\":\"%s\",\"postal_code\":\"%s\","
                    "\"merchant\":\"%s\",\"order_id\":\"%s\","
                    "\"status\":\"%s\",\"added_at\":\"%s\",\"received_at\":\"%s\"}",
                    xid, label, eta, url, place, xactual, source,
                    tracking, postal, merchant, order_id,
                    eff_status, xadded, xreceived);
                char merged[17000];
                size_t pre = (size_t)(scan - cur);
                size_t post_len = strlen(obj_end + 1);
                memcpy(merged, cur, pre);
                memcpy(merged + pre, rebuilt, strlen(rebuilt));
                memcpy(merged + pre + strlen(rebuilt), obj_end + 1, post_len);
                merged[pre + strlen(rebuilt) + post_len] = 0;
                pkg_write_all(merged);
                pthread_mutex_unlock(&g_pkg_mtx);
                /* Fire notify only on actual state advancement */
                if (rank_new > rank_old && strcmp(xstatus, status) != 0) {
                    notify_pkg_state_change(merchant, label, xstatus, status, eta);
                }
                return send_status(fd, 200, "OK", "{\"ok\":1,\"upsert\":\"updated\"}");
            }
            scan = obj_end + 1;
        }
    }

    /* No match — append as new */
    char merged[17000];
    char entry[1024];
    snprintf(entry, sizeof(entry),
        "{\"id\":\"%s\",\"label\":\"%s\",\"eta\":\"%s\",\"url\":\"%s\","
        "\"place\":\"%s\",\"actual_place\":\"\",\"source\":\"%s\","
        "\"tracking\":\"%s\",\"postal_code\":\"%s\","
        "\"merchant\":\"%s\",\"order_id\":\"%s\","
        "\"status\":\"%s\",\"added_at\":\"%s\",\"received_at\":\"\"}",
        id, label, eta, url, place, source, tracking, postal,
        merchant, order_id, status, ts);
    size_t L = strlen(cur);
    while (L > 0 && (cur[L-1] == '\n' || cur[L-1] == ' ')) L--;
    if (L < 2 || cur[L-1] != ']')      snprintf(merged, sizeof(merged), "[%s]", entry);
    else if (L == 2)                   snprintf(merged, sizeof(merged), "[%s]", entry);
    else {
        cur[L-1] = 0;
        snprintf(merged, sizeof(merged), "%s,%s]", cur, entry);
    }
    pkg_write_all(merged);
    pthread_mutex_unlock(&g_pkg_mtx);
    if (merchant[0] && !strcmp(status, "ordered")) {
        /* First time we hear of an order — fire a notify too */
        notify_pkg_state_change(merchant, label, "(new)", status, eta);
    }
    return send_status(fd, 200, "OK", "{\"ok\":1,\"upsert\":\"created\"}");
}

/* Surgical edit: find the {…} object whose "id":"<id>" matches, then
 * either delete it (DELETE) or rewrite its status to received (PATCH-ish).
 * We do this with string ops instead of a real JSON parser to avoid
 * pulling in a JSON dep on Toon. The objects we write are flat so this
 * is safe — no nested braces. */
static const char * find_obj_by_id(const char * json, const char * id) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"id\":\"%s\"", id);
    return strstr(json, needle);
}

static int handle_packages_delete(int fd, const char * id) {
    pthread_mutex_lock(&g_pkg_mtx);
    char cur[16384]; pkg_read_all(cur, sizeof(cur));
    const char * needle = find_obj_by_id(cur, id);
    if (!needle) {
        pthread_mutex_unlock(&g_pkg_mtx);
        return send_status(fd, 404, "Not Found", "{\"err\":\"id not found\"}");
    }
    /* Walk back to the opening '{' */
    const char * start = needle;
    while (start > cur && *start != '{') start--;
    /* Walk forward to the closing '}' */
    const char * end = needle;
    while (*end && *end != '}') end++;
    if (!*end) {
        pthread_mutex_unlock(&g_pkg_mtx);
        return send_status(fd, 500, "Server Error", "{\"err\":\"parse fail\"}");
    }
    end++;
    /* Eat a trailing or preceding comma so we don't leave [,] or [..,,..]. */
    if (*end == ',') end++;
    else if (start > cur && *(start - 1) == ',') start--;

    char out[16384];
    size_t pre = (size_t)(start - cur);
    size_t post_len = strlen(end);
    if (pre + post_len + 1 > sizeof(out)) {
        pthread_mutex_unlock(&g_pkg_mtx);
        return send_status(fd, 500, "Server Error", "{\"err\":\"too large\"}");
    }
    memcpy(out, cur, pre);
    memcpy(out + pre, end, post_len);
    out[pre + post_len] = 0;
    pkg_write_all(out);
    pthread_mutex_unlock(&g_pkg_mtx);
    return send_status(fd, 200, "OK", "{\"ok\":1}");
}

/* POST /api/packages/<id>/receive  body: {"actual_place":"keuken"} */
static int handle_packages_receive(int fd, const char * id, const char * body) {
    char place[64] = "";
    extract_str(body, "actual_place", place, sizeof(place));

    pthread_mutex_lock(&g_pkg_mtx);
    char cur[16384]; pkg_read_all(cur, sizeof(cur));
    char * needle = (char *)find_obj_by_id(cur, id);
    if (!needle) {
        pthread_mutex_unlock(&g_pkg_mtx);
        return send_status(fd, 404, "Not Found", "{\"err\":\"id not found\"}");
    }
    char * start = needle;
    while (start > cur && *start != '{') start--;
    char * end = needle;
    while (*end && *end != '}') end++;
    if (!*end) {
        pthread_mutex_unlock(&g_pkg_mtx);
        return send_status(fd, 500, "Server Error", "{\"err\":\"parse fail\"}");
    }
    /* Pull existing label/eta/url/place out of the matched object so we
     * can rebuild it with status=received + the new actual_place + a
     * received_at timestamp. Bounded by '{' .. '}'. */
    char obj[1024];
    size_t L = (size_t)(end - start + 1);
    if (L >= sizeof(obj)) L = sizeof(obj) - 1;
    memcpy(obj, start, L); obj[L] = 0;
    char xid[32] = "", xlabel[128] = "", xeta[16] = "", xurl[256] = "",
         xplace[64] = "", xsource[16] = "", xadded[24] = "",
         xtracking[64] = "", xpostal[16] = "";
    extract_str(obj, "id",          xid,      sizeof(xid));
    extract_str(obj, "label",       xlabel,   sizeof(xlabel));
    extract_str(obj, "eta",         xeta,     sizeof(xeta));
    extract_str(obj, "url",         xurl,     sizeof(xurl));
    extract_str(obj, "place",       xplace,   sizeof(xplace));
    extract_str(obj, "source",      xsource,  sizeof(xsource));
    extract_str(obj, "added_at",    xadded,   sizeof(xadded));
    extract_str(obj, "tracking",    xtracking,sizeof(xtracking));
    extract_str(obj, "postal_code", xpostal,  sizeof(xpostal));
    if (!place[0]) snprintf(place, sizeof(place), "%s", xplace);
    char ts[24]; time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));
    char rebuilt[1024];
    snprintf(rebuilt, sizeof(rebuilt),
        "{\"id\":\"%s\",\"label\":\"%s\",\"eta\":\"%s\",\"url\":\"%s\","
        "\"place\":\"%s\",\"actual_place\":\"%s\",\"source\":\"%s\","
        "\"tracking\":\"%s\",\"postal_code\":\"%s\","
        "\"status\":\"received\",\"added_at\":\"%s\",\"received_at\":\"%s\"}",
        xid, xlabel, xeta, xurl, xplace, place, xsource,
        xtracking, xpostal, xadded, ts);

    char merged[17000];
    size_t pre = (size_t)(start - cur);
    size_t post_len = strlen(end + 1);
    if (pre + strlen(rebuilt) + post_len + 1 > sizeof(merged)) {
        pthread_mutex_unlock(&g_pkg_mtx);
        return send_status(fd, 500, "Server Error", "{\"err\":\"too large\"}");
    }
    memcpy(merged, cur, pre);
    memcpy(merged + pre, rebuilt, strlen(rebuilt));
    memcpy(merged + pre + strlen(rebuilt), end + 1, post_len);
    merged[pre + strlen(rebuilt) + post_len] = 0;
    pkg_write_all(merged);
    pthread_mutex_unlock(&g_pkg_mtx);
    return send_status(fd, 200, "OK", "{\"ok\":1}");
}

/* POST /api/email — accepts {"from":"...", "subject":"...", "body":"..."}
 * (as fired by HA's imap_content event after Jinja substitution). Pipes
 * the JSON to /mnt/data/parse_email.py via popen; the script extracts
 * merchant + status + order_id and POSTs back to /api/packages here.
 * Synchronous — returns the script's stdout so the caller (HA) gets
 * a readable trace in the log. */
static int handle_email_post(int fd, const char * body) {
    if (!body || !body[0]) return send_status(fd, 400, "Bad Request",
        "{\"err\":\"empty body\"}");
    /* Write the body to a temp file so we don't have to escape it through
     * the shell. parse_email.py reads JSON from stdin. */
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "/tmp/email-%ld.json", (long)time(NULL));
    FILE * t = fopen(tmp, "w");
    if (!t) return send_status(fd, 500, "Server Error", "{\"err\":\"tmp\"}");
    fwrite(body, 1, strlen(body), t);
    fclose(t);
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "TARGET_URL=http://127.0.0.1:10081/api/packages "
        "python3 /mnt/data/parse_email.py < %s 2>&1; rm -f %s", tmp, tmp);
    FILE * p = popen(cmd, "r");
    if (!p) return send_status(fd, 500, "Server Error", "{\"err\":\"popen\"}");
    char out[2048]; size_t n = fread(out, 1, sizeof(out) - 1, p);
    out[n] = 0;
    pclose(p);
    /* Pass through whatever parse_email.py printed. */
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n", n);
    if (sock_send_all(fd, hdr, hn) < 0) return -1;
    return sock_send_all(fd, out, n);
}

static int handle_curtain(int fd, const char * body) {
    char action[16];
    if (!extract_str(body, "action", action, sizeof(action))) {
        return send_status(fd, 400, "Bad Request",
            "{\"err\":\"need {\\\"action\\\":\\\"open|close|stop\\\"}\"}");
    }
    if      (!strcmp(action, "open"))  ha_curtain_open_async();
    else if (!strcmp(action, "close")) ha_curtain_close_async();
    else if (!strcmp(action, "stop"))  ha_curtain_stop_async();
    else return send_status(fd, 400, "Bad Request",
        "{\"err\":\"action must be open/close/stop\"}");
    return send_status(fd, 200, "OK", "{\"ok\":1}");
}

/* -------- full settings page (web mirror of Settings screen) ---------- */
static int extract_int(const char * body, const char * key, int * out) {
    float v;
    if (!extract_float(body, key, &v)) return 0;
    *out = (int)v;
    return 1;
}

static int handle_settings_get(int fd) {
    char body[4096];
    int n = snprintf(body, sizeof body,
        "{"
        "\"auto_dim_enabled\":%d,\"auto_dim_seconds\":%d,"
        "\"active_brightness\":%d,\"dim_brightness\":%d,"
        "\"temp_offset_centi\":%d,\"show_dim_weather\":%d,"
        "\"show_dim_waste\":%d,\"dim_waste_lead_days\":%d,"
        "\"waste_postcode\":\"%s\",\"waste_housenr\":\"%s\","
        "\"waste_provider\":%d,\"waste_ics_url\":\"%s\","
        "\"weather_location\":\"%s\",\"weather_location_id\":%d,"
        "\"forecast_mode\":%d,\"ot_bridge_mode\":\"%s\",\"otgw_host\":\"%s\","
        "\"mqtt_enabled\":%d,\"mqtt_host\":\"%s\",\"mqtt_port\":%d,\"mqtt_user\":\"%s\","
        "\"enable_p1_elec\":%d,\"enable_p1_water\":%d,\"enable_vent\":%d,"
        "\"enable_ha\":%d,\"enable_zwave\":%d,\"vnc_enabled\":%d,"
        "\"enable_domoticz\":%d,\"domoticz_host\":\"%s\",\"domoticz_user\":\"%s\","
        "\"hide_offline_tiles\":%d,\"boot_picker_enabled\":%d,"
        "\"update_check_enabled\":%d,"
        "\"ha_host\":\"%s\",\"life360_a_entity\":\"%s\",\"life360_a_name\":\"%s\","
        "\"life360_b_entity\":\"%s\",\"life360_b_name\":\"%s\","
        "\"curtain_entity\":\"%s\",\"curtain_bat_a\":\"%s\",\"curtain_bat_b\":\"%s\","
        "\"doorbell_entity\":\"%s\",\"doorbell_camera\":\"%s\",\"doorbell_seconds\":%d,"
        "\"doorbell_stream_url\":\"%s\","
        "\"p1_elec_host\":\"%s\",\"p1_water_host\":\"%s\",\"vent_host\":\"%s\",\"opnsense_host\":\"%s\","
        "\"energy_source\":%d,\"auto_update_enabled\":%d,\"auto_update_hour\":%d,"
        "\"news_enabled\":%d,\"news_rss_url\":\"%s\","
        "\"tile_rotate_enabled\":%d,\"tile_rotate_seconds\":%d,\"tile_rotate_members\":\"%s\","
        "\"client_mode\":%d,\"master_host\":\"%s\""
        "}",
        settings.auto_dim_enabled, settings.auto_dim_seconds,
        settings.active_brightness, settings.dim_brightness,
        settings.temp_offset_centi, settings.show_dim_weather,
        settings.show_dim_waste, settings.dim_waste_lead_days,
        settings.waste_postcode, settings.waste_housenr,
        settings.waste_provider, settings.waste_ics_url,
        settings.weather_location, settings.weather_location_id,
        settings.forecast_mode, settings.ot_bridge_mode, settings.otgw_host,
        settings.mqtt_enabled, settings.mqtt_host, settings.mqtt_port, settings.mqtt_user,
        settings.enable_p1_elec, settings.enable_p1_water, settings.enable_vent,
        settings.enable_ha, settings.enable_zwave, settings.vnc_enabled,
        settings.enable_domoticz, settings.domoticz_host, settings.domoticz_user,
        settings.hide_offline_tiles, settings.boot_picker_enabled,
        settings.update_check_enabled,
        settings.ha_host, settings.life360_a_entity, settings.life360_a_name,
        settings.life360_b_entity, settings.life360_b_name,
        settings.curtain_entity, settings.curtain_bat_a, settings.curtain_bat_b,
        settings.doorbell_entity, settings.doorbell_camera, settings.doorbell_seconds,
        settings.doorbell_stream_url,
        settings.p1_elec_host, settings.p1_water_host, settings.vent_host, settings.opnsense_host,
        settings.energy_source, settings.auto_update_enabled, settings.auto_update_hour,
        settings.news_enabled, settings.news_rss_url,
        settings.tile_rotate_enabled, settings.tile_rotate_seconds, settings.tile_rotate_members,
        settings.client_mode, settings.master_host);
    char hdr[160];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\n\r\n", n);
    if (sock_send_all(fd, hdr, hn) < 0) return -1;
    return sock_send_all(fd, body, n);
}

static int handle_settings_post(int fd, const char * body) {
    int iv; char sv[256];
    if (extract_int(body, "auto_dim_enabled", &iv))   settings.auto_dim_enabled = !!iv;
    if (extract_int(body, "auto_dim_seconds", &iv))   settings.auto_dim_seconds = iv < 5 ? 5 : (iv > 300 ? 300 : iv);
    if (extract_int(body, "active_brightness", &iv))  settings.active_brightness = iv < 0 ? 0 : (iv > 1000 ? 1000 : iv);
    if (extract_int(body, "dim_brightness", &iv))     settings.dim_brightness = iv < 0 ? 0 : (iv > 1000 ? 1000 : iv);
    if (extract_int(body, "temp_offset_centi", &iv))  settings.temp_offset_centi = iv < -500 ? -500 : (iv > 500 ? 500 : iv);
    if (extract_int(body, "show_dim_weather", &iv))   settings.show_dim_weather = !!iv;
    if (extract_int(body, "show_dim_waste", &iv))     settings.show_dim_waste = !!iv;
    if (extract_int(body, "dim_waste_lead_days", &iv))settings.dim_waste_lead_days = iv < 0 ? 0 : (iv > 7 ? 7 : iv);
    if (extract_str(body, "waste_postcode", sv, sizeof sv))
        snprintf(settings.waste_postcode, sizeof settings.waste_postcode, "%s", sv);
    if (extract_str(body, "waste_housenr", sv, sizeof sv))
        snprintf(settings.waste_housenr, sizeof settings.waste_housenr, "%s", sv);
    if (extract_int(body, "waste_provider", &iv)) settings.waste_provider = !!iv;
    if (extract_str(body, "waste_ics_url", sv, sizeof sv))
        snprintf(settings.waste_ics_url, sizeof settings.waste_ics_url, "%s", sv);
    if (extract_int(body, "forecast_mode", &iv))      settings.forecast_mode = iv;
    if (extract_int(body, "mqtt_port", &iv))          settings.mqtt_port = iv;
    if (extract_int(body, "enable_p1_elec", &iv))     settings.enable_p1_elec = !!iv;
    if (extract_int(body, "enable_p1_water", &iv))    settings.enable_p1_water = !!iv;
    if (extract_int(body, "enable_vent", &iv))        settings.enable_vent = !!iv;
    if (extract_int(body, "enable_ha", &iv))          settings.enable_ha = !!iv;
    if (extract_int(body, "enable_domoticz", &iv))    settings.enable_domoticz = !!iv;
    if (extract_str(body, "domoticz_host", sv, sizeof sv))
        snprintf(settings.domoticz_host, sizeof settings.domoticz_host, "%s", sv);
    if (extract_str(body, "domoticz_user", sv, sizeof sv))
        snprintf(settings.domoticz_user, sizeof settings.domoticz_user, "%s", sv);
    if (extract_str(body, "domoticz_pass", sv, sizeof sv))
        snprintf(settings.domoticz_pass, sizeof settings.domoticz_pass, "%s", sv);
    if (extract_int(body, "enable_zwave", &iv))       settings.enable_zwave = !!iv;
    if (extract_int(body, "vnc_enabled", &iv))        settings.vnc_enabled = !!iv;
    if (extract_int(body, "hide_offline_tiles", &iv)) settings.hide_offline_tiles = !!iv;
    if (extract_int(body, "boot_picker_enabled", &iv))settings.boot_picker_enabled = !!iv;
    if (extract_int(body, "update_check_enabled", &iv))settings.update_check_enabled = !!iv;
    if (extract_int(body, "mqtt_enabled", &iv))       settings.mqtt_enabled = !!iv;
    /* City name is authoritative: when it changes, auto-resolve the Buienradar
     * location id (Open-Meteo geocoding). Only fall back to a manually-entered
     * id when the city is unchanged, so the id field still allows an override. */
    {
        int city_changed = 0;
        if (extract_str(body, "weather_location", sv, sizeof sv)) {
            city_changed = strcmp(sv, settings.weather_location) != 0;
            snprintf(settings.weather_location, sizeof settings.weather_location, "%s", sv);
        }
        if (city_changed && settings.weather_location[0]) {
            int gid = weather_geocode(settings.weather_location);
            if (gid > 0) settings.weather_location_id = gid;
        } else if (extract_int(body, "weather_location_id", &iv) && iv > 0) {
            settings.weather_location_id = iv;
        }
    }
    if (extract_str(body, "ot_bridge_mode", sv, sizeof sv))
        snprintf(settings.ot_bridge_mode, sizeof settings.ot_bridge_mode, "%s", sv);
    if (extract_str(body, "otgw_host", sv, sizeof sv))
        snprintf(settings.otgw_host, sizeof settings.otgw_host, "%s", sv);
    if (extract_str(body, "mqtt_host", sv, sizeof sv))
        snprintf(settings.mqtt_host, sizeof settings.mqtt_host, "%s", sv);
    if (extract_str(body, "mqtt_user", sv, sizeof sv))
        snprintf(settings.mqtt_user, sizeof settings.mqtt_user, "%s", sv);
    if (extract_int(body, "client_mode", &iv))        settings.client_mode = !!iv;
    if (extract_str(body, "master_host", sv, sizeof sv))
        snprintf(settings.master_host, sizeof settings.master_host, "%s", sv);
    /* Home Assistant host + Life360 + curtain entities */
    if (extract_str(body, "ha_host", sv, sizeof sv))
        snprintf(settings.ha_host, sizeof settings.ha_host, "%s", sv);
    if (extract_str(body, "life360_a_entity", sv, sizeof sv))
        snprintf(settings.life360_a_entity, sizeof settings.life360_a_entity, "%s", sv);
    if (extract_str(body, "life360_a_name", sv, sizeof sv))
        snprintf(settings.life360_a_name, sizeof settings.life360_a_name, "%s", sv);
    if (extract_str(body, "life360_b_entity", sv, sizeof sv))
        snprintf(settings.life360_b_entity, sizeof settings.life360_b_entity, "%s", sv);
    if (extract_str(body, "life360_b_name", sv, sizeof sv))
        snprintf(settings.life360_b_name, sizeof settings.life360_b_name, "%s", sv);
    if (extract_str(body, "curtain_entity", sv, sizeof sv))
        snprintf(settings.curtain_entity, sizeof settings.curtain_entity, "%s", sv);
    if (extract_str(body, "curtain_bat_a", sv, sizeof sv))
        snprintf(settings.curtain_bat_a, sizeof settings.curtain_bat_a, "%s", sv);
    if (extract_str(body, "curtain_bat_b", sv, sizeof sv))
        snprintf(settings.curtain_bat_b, sizeof settings.curtain_bat_b, "%s", sv);
    if (extract_str(body, "doorbell_entity", sv, sizeof sv))
        snprintf(settings.doorbell_entity, sizeof settings.doorbell_entity, "%s", sv);
    if (extract_str(body, "doorbell_camera", sv, sizeof sv))
        snprintf(settings.doorbell_camera, sizeof settings.doorbell_camera, "%s", sv);
    if (extract_int(body, "doorbell_seconds", &iv))
        settings.doorbell_seconds = (iv < 3 || iv > 300) ? 30 : iv;
    if (extract_str(body, "doorbell_stream_url", sv, sizeof sv))
        snprintf(settings.doorbell_stream_url, sizeof settings.doorbell_stream_url, "%s", sv);
    /* Integration LAN hosts */
    if (extract_str(body, "p1_elec_host", sv, sizeof sv))
        snprintf(settings.p1_elec_host, sizeof settings.p1_elec_host, "%s", sv);
    if (extract_str(body, "p1_water_host", sv, sizeof sv))
        snprintf(settings.p1_water_host, sizeof settings.p1_water_host, "%s", sv);
    if (extract_str(body, "vent_host", sv, sizeof sv))
        snprintf(settings.vent_host, sizeof settings.vent_host, "%s", sv);
    if (extract_str(body, "opnsense_host", sv, sizeof sv))
        snprintf(settings.opnsense_host, sizeof settings.opnsense_host, "%s", sv);
    if (extract_int(body, "energy_source", &iv))      settings.energy_source = !!iv;
    /* Auto-update */
    if (extract_int(body, "auto_update_enabled", &iv))settings.auto_update_enabled = !!iv;
    if (extract_int(body, "auto_update_hour", &iv))   settings.auto_update_hour = (iv < 0 || iv > 23) ? 2 : iv;
    /* Newsreader */
    if (extract_int(body, "news_enabled", &iv))       settings.news_enabled = !!iv;
    if (extract_str(body, "news_rss_url", sv, sizeof sv))
        snprintf(settings.news_rss_url, sizeof settings.news_rss_url, "%s", sv);
    /* Tile auto-rotate */
    if (extract_int(body, "tile_rotate_enabled", &iv))settings.tile_rotate_enabled = !!iv;
    if (extract_int(body, "tile_rotate_seconds", &iv))settings.tile_rotate_seconds = iv < 3 ? 3 : (iv > 120 ? 120 : iv);
    if (extract_str(body, "tile_rotate_members", sv, sizeof sv))
        snprintf(settings.tile_rotate_members, sizeof settings.tile_rotate_members, "%s", sv);
    settings_save();
    return send_status(fd, 200, "OK", "{\"ok\":1,\"note\":\"some changes apply after a toonui restart\"}");
}

static const char SETTINGS_HTML[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>freetoon settings</title><style>"
"body{font-family:system-ui,sans-serif;background:#0e1a2a;color:#dfe9f3;margin:0;padding:16px}"
"h1{font-size:20px}h2{font-size:15px;color:#88aabb;margin:18px 0 6px;border-bottom:1px solid #24385c;padding-bottom:4px}"
".r{display:flex;align-items:center;justify-content:space-between;padding:8px 0;gap:12px}"
".r label{flex:1}input,select{background:#1a2940;color:#fff;border:1px solid #2a4060;border-radius:8px;padding:8px;font-size:15px}"
"input[type=text],input[type=number]{width:170px}input[type=checkbox]{width:24px;height:24px}"
"button{background:#2e6e3a;color:#fff;border:0;border-radius:10px;padding:14px 22px;font-size:17px;margin-top:18px}"
"#msg{color:#ffcc44;margin-left:12px}"
"</style></head><body><h1>freetoon settings</h1>"
"<p style='margin:-6px 0 10px'><a href=/about style='color:#6fb3ff'>About / License</a></p>"
"<div id=f>loading...</div>"
"<button onclick=save()>Save</button><span id=msg></span>"
"<script>"
"var S={};"
"var SCHEMA=["
"['Display','h'],"
"['auto_dim_enabled','Auto-dim','b'],['auto_dim_seconds','Dim after (s)','n'],"
"['active_brightness','Active brightness (0-1000)','n'],['dim_brightness','Dim brightness (0-1000)','n'],"
"['temp_offset_centi','Temp offset (centi-C)','n'],"
"['show_dim_weather','Weather on dim','b'],['show_dim_waste','Waste on dim','b'],"
"['dim_waste_lead_days','Waste lead days (0-7)','n'],"
"['waste_postcode','Waste postcode (e.g. 1671AD)','t'],['waste_housenr','Waste house nr','t'],"
"['waste_provider','Waste: use ICS URL (0 HVC / 1 ICS)','n'],['waste_ics_url','Waste ICS calendar URL','t'],"
"['Weather','h'],"
"['weather_location','City (auto-resolves id)','t'],['weather_location_id','Buienradar id (auto)','n'],"
"['forecast_mode','Forecast (0 auto/1 hourly/2 daily)','n'],"
"['Heating','h'],"
"['ot_bridge_mode','OT bridge (off/proxy/wireless)','t'],['otgw_host','OTGW host (ip)','t'],"
"['MQTT','h'],"
"['mqtt_enabled','MQTT enabled','b'],"
"['mqtt_host','Broker host','t'],['mqtt_port','Port','n'],['mqtt_user','User','t'],"
"['Integrations','h'],"
"['enable_p1_elec','P1 electricity','b'],['p1_elec_host','P1 elec host (ip)','t'],"
"['enable_p1_water','P1 water','b'],['p1_water_host','P1 water host (ip)','t'],"
"['energy_source','Energy src (0 meteradapter / 1 P1)','n'],"
"['enable_vent','Ventilation','b'],['vent_host','Itho vent host (ip)','t'],"
"['enable_zwave','Z-Wave control','b'],"
"['opnsense_host','Router host (healthcheck, ip)','t'],"
"['Home Assistant','h'],"
"['enable_ha','Home Assistant enabled','b'],['ha_host','HA host (ip:port)','t'],"
"['curtain_entity','Curtain cover entity','t'],"
"['curtain_bat_a','Curtain battery sensor A','t'],['curtain_bat_b','Curtain battery sensor B','t'],"
"['doorbell_entity','Doorbell trigger entity (on=ring)','t'],"
"['doorbell_camera','Doorbell camera entity','t'],['doorbell_seconds','Snapshot shown (s)','n'],"
"['doorbell_stream_url','Doorbell MJPEG stream URL (live; blank=still)','t'],"
"['life360_a_entity','Person A device_tracker','t'],['life360_a_name','Person A name','t'],"
"['life360_b_entity','Person B device_tracker','t'],['life360_b_name','Person B name','t'],"
"['Domoticz','h'],"
"['enable_domoticz','Domoticz enabled','b'],['domoticz_host','Domoticz host (ip:port)','t'],"
"['domoticz_user','Domoticz user (opt)','t'],"
"['Newsreader','h'],"
"['news_enabled','News ticker','b'],['news_rss_url','RSS feed URL','t'],"
"['Tile auto-rotate','h'],"
"['tile_rotate_enabled','Rotate a tile','b'],['tile_rotate_seconds','Rotate every (s)','n'],"
"['tile_rotate_members','Rotate members (id1,id2,..)','t'],"
"['Updates','h'],"
"['update_check_enabled','Update check','b'],"
"['auto_update_enabled','Auto-update nightly','b'],['auto_update_hour','Auto-update hour (0-23)','n'],"
"['Client mode (slave Toon / tablet)','h'],"
"['client_mode','Client mode (mirror a master Toon)','b'],['master_host','Master Toon IP/host','t'],"
"['Display options','h'],"
"['vnc_enabled','VNC server','b'],['hide_offline_tiles','Hide offline tiles','b'],"
"['boot_picker_enabled','Boot picker','b']"
"];"
"function build(){var h='';for(var i=0;i<SCHEMA.length;i++){var s=SCHEMA[i];"
"if(s[1]=='h'){h+='<h2>'+s[0]+'</h2>';continue;}var k=s[0],lbl=s[1],t=s[2],v=S[k];"
"var inp;if(t=='b')inp='<input type=checkbox id=\"'+k+'\"'+(v?' checked':'')+'>';"
"else if(t=='n')inp='<input type=number id=\"'+k+'\" value=\"'+v+'\">';"
"else inp='<input type=text id=\"'+k+'\" value=\"'+(v==null?'':String(v).replace(/\"/g,'&quot;'))+'\">';"
"h+='<div class=r><label>'+lbl+'</label>'+inp+'</div>';}"
"document.getElementById('f').innerHTML=h;}"
"function load(){fetch('/api/settings').then(r=>r.json()).then(j=>{S=j;build();});}"
"function save(){var o={};for(var i=0;i<SCHEMA.length;i++){var s=SCHEMA[i];if(s[1]=='h')continue;"
"var k=s[0],t=s[2],e=document.getElementById(k);if(!e)continue;"
"o[k]=t=='b'?(e.checked?1:0):(t=='n'?parseInt(e.value||'0'):e.value);}"
"fetch('/api/settings',{method:'POST',body:JSON.stringify(o)}).then(r=>r.json()).then(j=>{"
"document.getElementById('msg').textContent=j.ok?'Saved. '+(j.note||''):'Error';});}"
"load();"
"</script></body></html>";

static int handle_settings_page(int fd) {
    size_t n = sizeof(SETTINGS_HTML) - 1;
    char hdr[160];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n\r\n", n);
    if (sock_send_all(fd, hdr, hn) < 0) return -1;
    return sock_send_all(fd, SETTINGS_HTML, n);
}

/* About / license — the web mirror of the LVGL logo→About modal. */
#ifndef BUILD_VERSION
#define BUILD_VERSION "dev"
#endif
static int handle_about_page(int fd) {
    char html[2048];
    int n = snprintf(html, sizeof html,
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>freetoon - about</title><style>"
        "body{font-family:system-ui,sans-serif;background:#0e1a2a;color:#dfe9f3;margin:0;padding:18px;line-height:1.5}"
        "h1{font-size:22px;margin:0 0 2px}.v{color:#88aabb;font-size:14px;margin-bottom:14px}"
        "h2{font-size:15px;color:#88aabb;border-bottom:1px solid #24385c;padding-bottom:4px;margin:18px 0 6px}"
        "code{background:#16243a;padding:2px 6px;border-radius:6px}a{color:#6fb3ff}"
        "ul{padding-left:20px}li{margin:3px 0}.mark{display:inline-block;background:#2e6e9e;color:#fff;"
        "border-radius:10px;padding:6px 10px;font-weight:bold;margin-right:8px}"
        "</style></head><body>"
        "<span class=mark>ft</span><h1>freetoon</h1>"
        "<div class=v>%s &nbsp;-&nbsp; beta &nbsp;-&nbsp; alternative UI by Ierlandfan &nbsp;-&nbsp; MIT License</div>"
        "<p>An independent LVGL UI for the Eneco Toon. Released under the "
        "<a href=https://github.com/Ierlandfan/freetoon-lvgl/blob/main/LICENSE>MIT License</a>.</p>"
        "<h2>Thanks</h2><p>To <b>Quby / Eneco</b> for the underlying Toon platform and the "
        "BoxTalk / Quby protocol structure this UI builds on. The stock Toon binaries and "
        "keteladapter firmware remain &copy; Eneco / Quby and are not redistributed or "
        "modified by this project.</p>"
        "<h2>Built with</h2><ul>"
        "<li>LVGL - embedded UI library (MIT) &copy; LVGL Kft</li>"
        "<li>QR-Code-generator (MIT) &copy; Project Nayuki</li>"
        "<li>LodePNG (zlib) &copy; Lode Vandevenne</li>"
        "<li>TJpgDec - JPEG decoder (BSD-3) &copy; ChaN</li>"
        "<li>OTGW HTTP firmware &copy; Robert van den Breemen</li>"
        "<li>Itho-WiFi REST add-on &copy; Arjen Hiemstra</li>"
        "<li>HomeWizard P1, Buienradar, NOS feeds - public APIs</li>"
        "</ul>"
        "<p><a href=/settings>&larr; Settings</a> &nbsp;&middot;&nbsp; "
        "<a href=https://github.com/Ierlandfan/freetoon-lvgl>Project on GitHub</a></p>"
        "</body></html>",
        BUILD_VERSION);
    char hdr[160];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n\r\n", n);
    if (sock_send_all(fd, hdr, hn) < 0) return -1;
    return sock_send_all(fd, html, n);
}

static int dispatch(int fd, char * req) {
    char method[8] = "", path[256] = "";
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        return send_status(fd, 400, "Bad Request", "bad request line");
    }
    char * body = strstr(req, "\r\n\r\n");
    if (body) body += 4; else body = "";

    if (!strcmp(method, "GET")) {
        if (!strcmp(path, "/api/state"))         return handle_state(fd);
        if (!strcmp(path, "/api/state/stream"))  return handle_state_stream(fd);
        if (!strcmp(path, "/api/packages"))      return handle_packages_get(fd);
        if (!strcmp(path, "/api/schedule"))      return handle_schedule_get(fd);
        if (!strcmp(path, "/api/settings"))      return handle_settings_get(fd);
        if (!strcmp(path, "/about") || !strcmp(path, "/about.html"))
            return handle_about_page(fd);
        if (!strcmp(path, "/settings") || !strcmp(path, "/settings.html"))
            return handle_settings_page(fd);
        return serve_static(fd, path);
    }
    if (!strcmp(method, "POST")) {
        if (!strcmp(path, "/api/setpoint")) return handle_setpoint(fd, body);
        if (!strcmp(path, "/api/program"))  return handle_program(fd, body);
        if (!strcmp(path, "/api/schedule")) return handle_schedule_post(fd, body);
        if (!strcmp(path, "/api/curtain"))  return handle_curtain(fd, body);
        if (!strcmp(path, "/api/settings")) return handle_settings_post(fd, body);
        if (!strcmp(path, "/api/packages")) return handle_packages_post(fd, body);
        if (!strcmp(path, "/api/email"))    return handle_email_post(fd, body);
        /* POST /api/packages/<id>/receive */
        if (!strncmp(path, "/api/packages/", 14)) {
            const char * tail = path + 14;
            const char * slash = strchr(tail, '/');
            if (slash && !strcmp(slash, "/receive")) {
                char id[32]; size_t L = (size_t)(slash - tail);
                if (L >= sizeof(id)) L = sizeof(id) - 1;
                memcpy(id, tail, L); id[L] = 0;
                return handle_packages_receive(fd, id, body);
            }
        }
        return send_status(fd, 404, "Not Found", "no");
    }
    if (!strcmp(method, "DELETE")) {
        /* DELETE /api/packages/<id> */
        if (!strncmp(path, "/api/packages/", 14))
            return handle_packages_delete(fd, path + 14);
        return send_status(fd, 404, "Not Found", "no");
    }
    if (!strcmp(method, "OPTIONS")) {
        const char * hdr =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n\r\n";
        return sock_send_all(fd, hdr, strlen(hdr));
    }
    return send_status(fd, 405, "Method Not Allowed", "no");
}

/* Worker thread for a single accepted connection. Owns the fd. */
static void * conn_thread(void * arg) {
    int fd = (int)(intptr_t)arg;
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char req[4096];
    size_t r = 0;
    while (r < sizeof(req) - 1) {
        ssize_t k = recv(fd, req + r, sizeof(req) - 1 - r, 0);
        if (k <= 0) break;
        r += (size_t)k;
        req[r] = 0;
        char * eoh = strstr(req, "\r\n\r\n");
        if (!eoh) continue;
        const char * cl = strcasestr(req, "Content-Length:");
        if (!cl) break;
        size_t need = (size_t)atoi(cl + 15);
        size_t have = r - (eoh + 4 - req);
        if (have >= need) break;
    }
    if (r > 0) dispatch(fd, req);
    close(fd);
    return NULL;
}

static void * pwa_thread(void * arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("[pwa] socket"); return NULL; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_port   = htons(PWA_PORT),
                             .sin_addr   = { htonl(INADDR_ANY) } };
    if (bind(srv, (struct sockaddr *)&a, sizeof(a)) != 0) {
        perror("[pwa] bind"); close(srv); return NULL;
    }
    listen(srv, 8);
    fprintf(stderr, "[pwa] listening on :%d  root=%s\n", PWA_PORT, PWA_ROOT);

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    /* 256 KB stack is plenty for our tiny handlers and keeps fork cost low. */
    pthread_attr_setstacksize(&at, 256 * 1024);

    while (1) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) continue;
        pthread_t t;
        if (pthread_create(&t, &at, conn_thread, (void *)(intptr_t)c) != 0) {
            close(c);
        }
    }
    pthread_attr_destroy(&at);
    return NULL;
}

int pwa_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, pwa_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
