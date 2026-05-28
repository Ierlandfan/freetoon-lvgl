/*
 * client_link — slave-mode bridge to a master Toon.
 *
 * Reads: a background thread holds open `curl -N` against the master's
 * Server-Sent-Events endpoint (GET /api/state/stream, PWA port 10081) and
 * copies each pushed JSON frame into the shared toon_state / ha_state structs
 * that the LVGL screens already read. No local HCB / integration traffic.
 *
 * Writes: the control helpers POST JSON to the master's /api/setpoint,
 * /api/program and /api/curtain endpoints — the same ones the PWA uses.
 *
 * The master host is taken from settings.master_host and validated to a safe
 * character set before being interpolated into the curl command line.
 */
#include "client_link.h"
#include "settings.h"
#include "boxtalk.h"
#include "homeassistant.h"
#include "homewizard.h"
#include "meteradapter.h"
#include "ventilation.h"
#include "weather.h"
#include "wastecollection.h"
#include "schedule.h"
#include "inbox.h"
#include "calendar.h"
#include "domoticz.h"
#include "news.h"
#include "tile_slots.h"
#include "packages.h"
#include "screen_zwave.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>

#define MASTER_PORT 10081

/* Host must be a bare IP/hostname (optionally :port) — reject anything that
 * could break out of the single-quoted curl argument. */
static int host_ok(const char * h) {
    if (!h || !*h) return 0;
    for (const char * p = h; *p; p++) {
        char c = *p;
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':'))
            return 0;
    }
    return 1;
}

/* ---- tiny JSON field extractors (flat object, known keys) ---- */
static int jnum(const char * j, const char * key, double * out) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char * p = strstr(j, pat);
    if (!p) return 0;
    *out = strtod(p + strlen(pat), NULL);
    return 1;
}
static int jint(const char * j, const char * key, int * out) {
    double d; if (!jnum(j, key, &d)) return 0; *out = (int)d; return 1;
}
static int jstr(const char * j, const char * key, char * out, size_t n) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char * p = strstr(j, pat);
    if (!p) return 0;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i < n - 1) out[i++] = *p++;
    out[i] = 0;
    return 1;
}

/* Find the array body for `"key":[ ... ]`. Returns a pointer one past the `[`
 * on success (so the caller can walk through `{` ... `}` objects), or NULL. */
static const char * find_array(const char * j, const char * key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":[", key);
    const char * p = strstr(j, pat);
    return p ? p + strlen(pat) : NULL;
}

/* Walk one `{...}` object starting at `*pp`; on return *pp points to the
 * char after the closing `}`. Copies the object body (without braces) into
 * `out` so the caller can use jnum/jint/jstr on a self-contained substring.
 * Returns 0 if there's no object at `*pp`. */
static int walk_object(const char ** pp, char * out, size_t outsz) {
    const char * p = *pp;
    while (*p && (*p == ',' || *p == ' ')) p++;
    if (*p != '{') return 0;
    const char * obj = p++;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
        p++;
    }
    size_t n = (size_t)(p - obj);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, obj, n);
    out[n] = 0;
    *pp = p;
    return 1;
}

void client_link_apply_state(const char * j) {
    if (!j || *j != '{') return;

    /* ---- scalars: expand state_fields.def with macros that parse JSON ---- */
    #define STATE_NUM(g, f, jname, fmt)  do { double _d; (void)fmt; \
        if (jnum(j, #jname, &_d)) g.f = (float)_d; } while (0);
    #define STATE_INT(g, f, jname)       do { int _v; \
        if (jint(j, #jname, &_v)) g.f = _v; } while (0);
    #define STATE_STR(g, f, jname)       jstr(j, #jname, (char *)g.f, sizeof g.f);
    #include "state_fields.def"
    #undef STATE_NUM
    #undef STATE_INT
    #undef STATE_STR

    /* ---- weather hourly forecast array ---- */
    {
        const char * p = find_array(j, "wx_hours");
        int idx = 0;
        char obj[160];
        while (p && idx < WEATHER_FORECAST_HOURS && walk_object(&p, obj, sizeof obj)) {
            weather_hour_t * h = &weather_state.hours[idx];
            double d; int v;
            jstr(obj, "label", h->label, sizeof h->label);
            if (jnum(obj, "temp", &d)) h->temperature = (float)d;
            if (jint(obj, "bft", &v))  h->wind_bft    = v;
            jstr(obj, "dir",  h->wind_dir, sizeof h->wind_dir);
            jstr(obj, "icon", h->icon,     sizeof h->icon);
            idx++;
        }
        if (p) weather_state.hour_count = idx;
    }

    /* ---- weather daily forecast array ---- */
    {
        const char * p = find_array(j, "wx_days");
        int idx = 0;
        char obj[160];
        while (p && idx < WEATHER_FORECAST_DAYS && walk_object(&p, obj, sizeof obj)) {
            weather_day_t * dd = &weather_state.days[idx];
            double d; int v;
            jstr(obj, "day", dd->day, sizeof dd->day);
            if (jnum(obj, "min", &d))  dd->min_temp    = (float)d;
            if (jnum(obj, "max", &d))  dd->max_temp    = (float)d;
            if (jint(obj, "bft", &v))  dd->wind_bft    = v;
            if (jint(obj, "rain", &v)) dd->rain_chance = v;
            jstr(obj, "dir",  dd->wind_dir, sizeof dd->wind_dir);
            jstr(obj, "icon", dd->icon,     sizeof dd->icon);
            idx++;
        }
        if (p) weather_state.day_count = idx;
    }

    /* ---- next waste pickups array → waste_state.items[] ---- */
    {
        const char * p = find_array(j, "wt_next");
        int idx = 0;
        char obj[120];
        while (p && idx < WASTE_TYPES && walk_object(&p, obj, sizeof obj)) {
            jstr(obj, "labels", waste_state.items[idx].label, sizeof waste_state.items[idx].label);
            jstr(obj, "date",   waste_state.items[idx].date,  sizeof waste_state.items[idx].date);
            idx++;
        }
        if (p) {
            /* Clear remaining slots so a shrinking list doesn't leave stale entries. */
            for (int i = idx; i < WASTE_TYPES; i++) {
                waste_state.items[i].label[0] = 0;
                waste_state.items[i].date[0]  = 0;
            }
            waste_state.connected = 1;
        }
    }
    {
        int v;
        if (jint(j, "waste_connected", &v)) waste_state.connected = v;
    }

    /* ---- HA lights array ---- */
    {
        const char * p = find_array(j, "ha_lights");
        int idx = 0;
        char obj[300];
        while (p && idx < HA_LIGHT_COUNT && walk_object(&p, obj, sizeof obj)) {
            ha_light_t * L = &ha_lights[idx];
            int v;
            jstr(obj, "id",   L->entity_id, sizeof L->entity_id);
            jstr(obj, "name", L->name,      sizeof L->name);
            jstr(obj, "area", L->area,      sizeof L->area);
            if (jint(obj, "on", &v)) L->on         = v;
            if (jint(obj, "av", &v)) L->available  = v;
            if (jint(obj, "br", &v)) L->brightness = v;
            idx++;
        }
        if (p) ha_light_count = idx;
    }

    /* ---- schedule entries ---- */
    {
        const char * p = find_array(j, "schedule");
        int idx = 0;
        char obj[160];
        while (p && idx < SCHEDULE_MAX && walk_object(&p, obj, sizeof obj)) {
            schedule_entry_t * e = &schedule_entries[idx];
            int v;
            if (jint(obj, "s",  &v)) e->target_state = v;
            if (jint(obj, "sd", &v)) e->start_day    = v;
            if (jint(obj, "sh", &v)) e->start_hour   = v;
            if (jint(obj, "sm", &v)) e->start_min    = v;
            if (jint(obj, "ed", &v)) e->end_day      = v;
            if (jint(obj, "eh", &v)) e->end_hour     = v;
            if (jint(obj, "em", &v)) e->end_min      = v;
            idx++;
        }
        if (p) schedule_count = idx;
    }

    /* ---- inbox messages ---- */
    {
        const char * p = find_array(j, "inbox");
        int idx = 0, v;
        char obj[700];
        while (p && idx < INBOX_MAX && walk_object(&p, obj, sizeof obj)) {
            inbox_msg_t * m = &inbox_msgs[idx];
            jstr(obj, "uuid", m->uuid,     sizeof m->uuid);
            jstr(obj, "type", m->type,     sizeof m->type);
            jstr(obj, "sub",  m->sub_type, sizeof m->sub_type);
            jstr(obj, "text", m->text,     sizeof m->text);
            double d; if (jnum(obj, "ts",  &d)) m->creation_date = (long)d;
            if (jint(obj, "read", &v)) m->read = v;
            idx++;
        }
        if (p) inbox_count = idx;
        if (jint(j, "inbox_unread", &v)) inbox_unread = v;
    }

    /* ---- calendar events ---- */
    {
        const char * p = find_array(j, "cal");
        int idx = 0, v;
        char obj[200];
        while (p && idx < CAL_MAX && walk_object(&p, obj, sizeof obj)) {
            calendar_event_t * e = &calendar_state.ev[idx];
            jstr(obj, "date", e->date,    sizeof e->date);
            jstr(obj, "time", e->time,    sizeof e->time);
            jstr(obj, "sum",  e->summary, sizeof e->summary);
            idx++;
        }
        if (p) calendar_state.count = idx;
        if (jint(j, "cal_connected", &v)) calendar_state.connected = v;
    }

    /* ---- news ticker ---- */
    {
        const char * p = find_array(j, "news");
        int idx = 0;
        char obj[600];
        char title[NEWS_TITLE_MAX], link[NEWS_LINK_MAX];
        while (p && idx < NEWS_MAX_ITEMS && walk_object(&p, obj, sizeof obj)) {
            int feed = -1;
            jstr(obj, "t", title, sizeof title);
            jstr(obj, "u", link,  sizeof link);
            jint(obj, "f", &feed);
            news_set_item_data(idx, title, link, feed);
            idx++;
        }
        if (p) news_set_count(idx);
    }

    /* ---- marketplace integrations (manifest + latest_value/subtitle) ----
     * Mirrors the master's whole g_integ[]. Plug-and-play: install on the
     * master => next SSE frame carries the new entry => tile_slots picks it
     * up => existing renderer paints the new tile without any rebuild. */
    {
        const char * p = find_array(j, "integrations");
        integration_meta_t tmp[MAX_INSTALLED_INTEGRATIONS];
        memset(tmp, 0, sizeof tmp);
        int idx = 0;
        char obj[800];
        while (p && idx < MAX_INSTALLED_INTEGRATIONS && walk_object(&p, obj, sizeof obj)) {
            integration_meta_t * M = &tmp[idx];
            double d;
            jstr(obj, "id",     M->id,             sizeof M->id);
            jstr(obj, "name",   M->name,           sizeof M->name);
            jstr(obj, "svc",    M->service_id,     sizeof M->service_id);
            jstr(obj, "ttitle", M->tile_title,     sizeof M->tile_title);
            jstr(obj, "tcolor", M->tile_color,     sizeof M->tile_color);
            jstr(obj, "ticon",  M->tile_icon,      sizeof M->tile_icon);
            jstr(obj, "vfield", M->value_field,    sizeof M->value_field);
            jstr(obj, "vunit",  M->value_unit,     sizeof M->value_unit);
            jstr(obj, "sfield", M->subtitle_field, sizeof M->subtitle_field);
            jstr(obj, "sunit",  M->subtitle_unit,  sizeof M->subtitle_unit);
            jstr(obj, "afield", M->alert_field,    sizeof M->alert_field);
            jstr(obj, "lval",   (char *)M->latest_value,    sizeof M->latest_value);
            jstr(obj, "lsub",   (char *)M->latest_subtitle, sizeof M->latest_subtitle);
            jstr(obj, "lalrt",  (char *)M->latest_alert,    sizeof M->latest_alert);
            if (jnum(obj, "lts", &d)) M->latest_epoch = (long)d;
            idx++;
        }
        if (p) tile_slots_set_from_remote(idx, tmp);
    }

    /* ---- packages: delivery banner queue ---- */
    {
        const char * p = find_array(j, "pkg_banners");
        packages_banner_t tmp[PACKAGES_BANNER_MAX];
        memset(tmp, 0, sizeof tmp);
        int idx = 0;
        char obj[700];
        while (p && idx < PACKAGES_BANNER_MAX && walk_object(&p, obj, sizeof obj)) {
            jstr(obj, "key",   tmp[idx].key,   sizeof tmp[idx].key);
            jstr(obj, "title", tmp[idx].title, sizeof tmp[idx].title);
            jstr(obj, "msg",   tmp[idx].msg,   sizeof tmp[idx].msg);
            jstr(obj, "url",   tmp[idx].url,   sizeof tmp[idx].url);
            idx++;
        }
        if (p) packages_set_banners_from_remote(idx, tmp);
    }

    /* ---- Z-Wave controller device list ---- */
    {
        const char * p = find_array(j, "zwave");
        zwave_dev_t tmp[ZWAVE_DEV_MAX];
        memset(tmp, 0, sizeof tmp);
        int idx = 0;
        char obj[280];
        while (p && idx < ZWAVE_DEV_MAX && walk_object(&p, obj, sizeof obj)) {
            int vv;
            jstr(obj, "uuid", tmp[idx].uuid, sizeof tmp[idx].uuid);
            jstr(obj, "name", tmp[idx].name, sizeof tmp[idx].name);
            jstr(obj, "type", tmp[idx].type, sizeof tmp[idx].type);
            if (jint(obj, "node", &vv)) tmp[idx].node_id  = vv;
            if (jint(obj, "sw",   &vv)) tmp[idx].is_switch = vv;
            if (jint(obj, "st",   &vv)) tmp[idx].state    = vv;
            idx++;
        }
        if (p) zwave_set_devices_from_remote(idx, tmp);
    }

    /* ---- Domoticz devices ---- */
    {
        const char * p = find_array(j, "dz");
        int idx = 0, v;
        char obj[160];
        while (p && idx < DOMOTICZ_MAX_DEV && walk_object(&p, obj, sizeof obj)) {
            domoticz_dev_t * d = &domoticz_state.dev[idx];
            int vv;
            if (jint(obj, "idx",   &vv)) d->idx   = vv;
            if (jint(obj, "kind",  &vv)) d->kind  = vv;
            jstr(obj, "name", d->name, sizeof d->name);
            if (jint(obj, "on",    &vv)) d->on    = vv;
            if (jint(obj, "level", &vv)) d->level = vv;
            idx++;
        }
        if (p) domoticz_state.count = idx;
        if (jint(j, "dz_connected", &v)) domoticz_state.connected = v;
    }

    toon_state.connected = 1;
}

static void * reader_thread(void * arg) {
    (void)arg;
    char url[160], cmd[256], line[8192];
    for (;;) {
        if (!host_ok(settings.master_host)) {
            toon_state.connected = 0;
            sleep(3);
            continue;
        }
        snprintf(url, sizeof url, "http://%s:%d/api/state/stream",
                 settings.master_host, MASTER_PORT);
        /* -N: no buffering (stream lines as they arrive). --connect-timeout
         * bounds the initial dial; no --max-time so the stream stays open. */
        snprintf(cmd, sizeof cmd,
                 "curl -s -N --connect-timeout 8 -A freetoon-client '%s' 2>/dev/null",
                 url);
        FILE * f = popen(cmd, "r");
        if (!f) { toon_state.connected = 0; sleep(3); continue; }
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "data:", 5) == 0) {
                char * j = line + 5;
                while (*j == ' ') j++;
                if (*j == '{') client_link_apply_state(j);
            }
        }
        pclose(f);
        toon_state.connected = 0;   /* stream dropped — reconnect shortly */
        sleep(2);
    }
    return NULL;
}

static int post_json(const char * path, const char * json) {
    if (!host_ok(settings.master_host)) return -1;
    char cmd[512];
    snprintf(cmd, sizeof cmd,
        "curl -s -m 6 -X POST -H 'Content-Type: application/json' "
        "--data '%s' 'http://%s:%d%s' >/dev/null 2>&1",
        json, settings.master_host, MASTER_PORT, path);
    return system(cmd) == 0 ? 0 : -1;
}

int client_link_setpoint(float temp) {
    char b[48];
    snprintf(b, sizeof b, "{\"value\":\"%.2f\"}", (double)temp);
    return post_json("/api/setpoint", b);
}
int client_link_program(int state) {
    char b[32];
    snprintf(b, sizeof b, "{\"state\":%d}", state);
    return post_json("/api/program", b);
}
int client_link_curtain(const char * action) {
    char b[48];
    snprintf(b, sizeof b, "{\"action\":\"%s\"}", action);
    return post_json("/api/curtain", b);
}

int client_link_start(void) {
    if (!settings.client_mode) return 0;
    toon_state.connected = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, reader_thread, NULL) != 0) {
        fprintf(stderr, "[client_link] thread create failed\n");
        return -1;
    }
    pthread_detach(t);
    fprintf(stderr, "[client_link] slave mode → master %s\n", settings.master_host);
    return 0;
}
