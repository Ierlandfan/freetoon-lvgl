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
#include "http.h"
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
#include <stdarg.h>

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

            if (strstr(m->text, "Service Center") || strstr(m->text, "Service center") ||
                strstr(m->text, "service center") || strstr(m->text, "ServiceCenter") ||
                strstr(m->type, "ServiceCenter") || strstr(m->sub_type, "ServiceCenter") ||
                (strstr(m->text, "vertrouwde") && strstr(m->text, "certificaten")) ||
                strstr(m->text, "webserver certificaten") ||
                strstr(m->text, "webcertificaten") ||
                (strcmp(m->type, "tsc") == 0 && strstr(m->text, "certificaten"))) {
                memset(m, 0, sizeof(*m));
                continue;
            }

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

    /* ---- news ticker (title + link + body + feed) ---- */
    {
        const char * p = find_array(j, "news");
        int idx = 0;
        char obj[NEWS_BODY_MAX + 400];   /* room for body + the other fields */
        char title[NEWS_TITLE_MAX], link[NEWS_LINK_MAX], body[NEWS_BODY_MAX];
        while (p && idx < NEWS_MAX_ITEMS && walk_object(&p, obj, sizeof obj)) {
            int feed = -1;
            jstr(obj, "t", title, sizeof title);
            jstr(obj, "u", link,  sizeof link);
            jstr(obj, "b", body,  sizeof body);
            jint(obj, "f", &feed);
            news_set_item_data(idx, title, link, body, feed);
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

static int g_is_stock_master = 0;

static int parse_json_val(const char * j, const char * key, int def) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char * p = strstr(j, pat);
    if (!p) return def;
    p += strlen(pat);
    while (*p == ' ' || *p == '\"') p++;
    return atoi(p);
}

static int parse_json_float(const char * j, const char * key, float * out) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char * p = strstr(j, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\"') p++;
    if (strncmp(p, "null", 4) == 0 || strncmp(p, "NaN", 3) == 0 || strncmp(p, "nan", 3) == 0) return 0;
    *out = (float)atof(p);
    return 1;
}

static int parse_rrd_val(const char * body, float * out) {
    if (!body) return 0;
    /* Skip key in quotes if present to avoid colons inside timestamps */
    const char * col = NULL;
    const char * k1 = strchr(body, '"');
    if (k1) {
        const char * k2 = strchr(k1 + 1, '"');
        if (k2) col = strchr(k2 + 1, ':');
    }
    if (!col) col = strchr(body, ':');
    if (!col) return 0;
    col++;
    while (*col == ' ' || *col == '\t') col++;
    if (strncmp(col, "NaN", 3) == 0 || strncmp(col, "nan", 3) == 0 || strncmp(col, "null", 4) == 0) return 0;
    *out = (float)atof(col);
    return 1;
}

static int parse_rrd_delta(const char * body, float * out_delta, float * out_latest) {
    if (!body || !out_delta) return 0;
    const char * p = strchr(body, '{');
    if (!p) p = body;
    float first = -1.0f, last = -1.0f;
    while (*p) {
        const char * k1 = strchr(p, '"');
        if (!k1) break;
        const char * k2 = strchr(k1 + 1, '"');
        if (!k2) break;
        const char * col = strchr(k2 + 1, ':');
        if (!col) break;
        const char * v = col + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (strncmp(v, "null", 4) != 0 && strncmp(v, "NaN", 3) != 0 && strncmp(v, "nan", 3) != 0) {
            char * endp = NULL;
            float val = strtof(v, &endp);
            if (endp != v && val >= 0.0f) {
                if (first < 0.0f) first = val;
                last = val;
            }
            p = (endp && endp > v) ? endp : v + 1;
        } else {
            p = v + 4;
        }
    }
    if (first >= 0.0f && last >= first) {
        *out_delta = (last - first) / 1000.0f;
        if (out_latest) *out_latest = last / 1000.0f;
        return 1;
    }
    return 0;
}

static time_t s_client_burner_last_on = 0;
static void client_update_burner(int bi) {
    time_t now = time(NULL);
    if (bi == 1) {
        s_client_burner_last_on = now;
        toon_state.burner_on = 1;
        toon_state.dhw_on    = 0;
    } else if (bi == 2) {
        toon_state.burner_on = 0;
        toon_state.dhw_on    = 1;
    } else {
        toon_state.dhw_on = 0;
        if (s_client_burner_last_on > 0 && (now - s_client_burner_last_on < 20) &&
            (toon_state.setpoint > toon_state.indoor_temp)) {
            toon_state.burner_on = 1;
        } else {
            toon_state.burner_on = 0;
        }
    }
}

static inline void client_set_water_pressure(float v) {
    if (v <= 0.05f) return;
    if (v >= 50.0f) toon_state.water_pressure = v / 100.0f;
    else if (v >= 5.0f) toon_state.water_pressure = v / 10.0f;
    else toon_state.water_pressure = v;
}

static void get_stock_url(char * out, size_t outsz, const char * fmt, ...) {
    char path_buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(path_buf, sizeof(path_buf), fmt, args);
    va_end(args);

    char host_buf[64];
    snprintf(host_buf, sizeof(host_buf), "%s", settings.master_host);
    char * colon = strchr(host_buf, ':');
    const char * slash = (path_buf[0] == '/') ? "" : "/";
    if (colon) {
        snprintf(out, outsz, "http://%s%s%s", host_buf, slash, path_buf);
    } else {
        snprintf(out, outsz, "http://%s:80%s%s", host_buf, slash, path_buf);
    }
}

static void poll_stock_master(void) {
    char url[512];
    char buf[4096];
    int fails = 0;
    int power_counter = 4; /* fetch immediately on first tick */
    int rrd_counter = 20;  /* fetch immediately on first tick */

    fprintf(stderr, "[client_link] running in Stock Toon master mode for %s\n", settings.master_host);

    while (settings.client_mode) {
        if (!host_ok(settings.master_host)) {
            toon_state.connected = 0;
            sleep(3);
            continue;
        }

        /* 1. Thermostat Info from happ_thermstat on port 80 */
        get_stock_url(url, sizeof url, "happ_thermstat?action=getThermostatInfo");
        if (http_fetch_to(url, buf, sizeof buf, 2) == 0 && strstr(buf, "\"currentTemp\"")) {
            fails = 0;
            int ct = parse_json_val(buf, "currentTemp", 0);
            if (ct > 0) toon_state.indoor_temp = ct / 100.0f;

            int sp = parse_json_val(buf, "currentSetpoint", 0);
            if (sp > 0) toon_state.setpoint = sp / 100.0f;

            toon_state.program_state = parse_json_val(buf, "programState", toon_state.program_state);
            toon_state.active_state  = parse_json_val(buf, "activeState",  toon_state.active_state);

            int bi = parse_json_val(buf, "burnerInfo", -1);
            if (bi >= 0) {
                client_update_burner(bi);
            }
            toon_state.modulation_level = parse_json_val(buf, "currentModulationLevel", toon_state.modulation_level);
            toon_state.ot_comm_error    = parse_json_val(buf, "otCommError", toon_state.ot_comm_error);

            int chsp = parse_json_val(buf, "currentInternalBoilerSetpoint", -1);
            if (chsp >= 0) toon_state.ch_setpoint = (float)chsp;

            float fval = 0.0f;
            if (parse_json_float(buf, "boilerPressure", &fval) || parse_json_float(buf, "chPressure", &fval)) {
                if (fval > 0.05f) client_set_water_pressure(fval);
            }
            if (parse_json_float(buf, "boilerOutTemp", &fval) || parse_json_float(buf, "currentBoilerTemperature", &fval)) {
                if (fval > 0.0f) toon_state.boiler_out_temp = (fval > 150.0f) ? fval / 100.0f : fval;
            }
            if (parse_json_float(buf, "boilerInTemp", &fval) || parse_json_float(buf, "currentBoilerReturnTemperature", &fval)) {
                if (fval > 0.0f) toon_state.boiler_in_temp = (fval > 150.0f) ? fval / 100.0f : fval;
            }

            toon_state.connected = 1;
        } else {
            fails++;
            if (fails >= 3) toon_state.connected = 0;
            if (fails >= 5) {
                fprintf(stderr, "[client_link] stock master %s unreachable (%d fails) — returning to probe\n",
                        settings.master_host, fails);
                break;
            }
        }

        /* If thermostat communication failed, master is down or network is not ready — wait and retry gently */
        if (!toon_state.connected) {
            sleep(10);
            continue;
        }

        /* 2. Electricity consumption from happ_pwrusage every ~12 seconds */
        power_counter++;
        if (power_counter >= 4) {
            power_counter = 0;
            /* Live real-time power from happ_pwrusage GetCurrentUsage on stock master */
            get_stock_url(url, sizeof url, "happ_pwrusage?action=GetCurrentUsage");
            if (http_fetch_to(url, buf, sizeof buf, 2) == 0 && strstr(buf, "\"powerUsage\"")) {
                double pu = 0.0, pp = 0.0;
                const char * p_pu = strstr(buf, "\"powerUsage\"");
                if (p_pu && jnum(p_pu, "value", &pu)) {
                    const char * p_pp = strstr(buf, "\"powerProduction\"");
                    if (p_pp) jnum(p_pp, "value", &pp);
                    float pw = (float)(pu - pp);
                    meter_state.power_w = pw;
                    meter_state.connected = 1;
                    hw_state.power_w = pw;
                    hw_state.connected_p1 = 1;
                }
            } else {
                /* Fallback to hcb_rrd elec_flow */
                get_stock_url(url, sizeof url, "hcb_rrd?action=getRrdData&loggerName=elec_flow&rra=5min&samples=1");
                if (http_fetch_to(url, buf, sizeof buf, 2) == 0) {
                    float pw = 0.0f;
                    if (parse_rrd_val(buf, &pw)) {
                        meter_state.power_w = pw;
                        meter_state.connected = 1;
                        hw_state.power_w = pw;
                        hw_state.connected_p1 = 1;
                    }
                }
            }
        }

        /* 3. Gas, Pressure & Boiler Temps from hcb_rrd every ~60 seconds */
        rrd_counter++;
        if (rrd_counter >= 20) {
            rrd_counter = 0;
            time_t now_ts = time(NULL);
            struct tm ltm;
            localtime_r(&now_ts, &ltm);
            ltm.tm_hour = 0; ltm.tm_min = 0; ltm.tm_sec = 0;
            time_t midnight = mktime(&ltm);
            get_stock_url(url, sizeof url,
                     "hcb_rrd?action=getRrdData&loggerName=gas_quantity&rra=5yrhours&from=%ld&to=%ld&readableTime=1&nullForNaN=1&samples=24",
                     (long)midnight, (long)now_ts);
            if (http_fetch_to(url, buf, sizeof buf, 2) == 0) {
                float d = 0.0f, tot = 0.0f;
                if (parse_rrd_delta(buf, &d, &tot)) {
                    meter_state.gas_today_m3 = d;
                    meter_state.gas_m3 = tot;
                    hw_state.gas_m3 = tot;
                    hw_state.gas_hour_m3 = d;
                    hw_state.connected_p1 = 1;
                }
            }

            /* Water pressure from hcb_rrd */
            static const struct {
                const char * name;
                const char * rra;
            } press_sources[] = {
                { "thermstat_boilerChPressure", "30days" },
                { "ch_pressure",                "5min" },
                { "boiler_pressure",            "5min" },
                { "water_pressure",             "5min" },
                { "pressure",                   "5min" }
            };
            static int press_idx = 0;
            int n_press = (int)(sizeof(press_sources) / sizeof(press_sources[0]));
            get_stock_url(url, sizeof url, "hcb_rrd?action=getRrdData&loggerName=%s&rra=%s&samples=1",
                     press_sources[press_idx].name, press_sources[press_idx].rra);
            int prc = http_fetch_to(url, buf, sizeof buf, 2);
            if (prc != 0) {
                get_stock_url(url, sizeof url, "hcb_rrd?action=getRrdData&loggerName=%s&rra=%s&samples=1",
                         press_sources[press_idx].name, press_sources[press_idx].rra);
                prc = http_fetch_to(url, buf, sizeof buf, 2);
            }
            if (prc == 0) {
                float bp = 0.0f;
                if (parse_rrd_val(buf, &bp) && bp > 0.05f) {
                    client_set_water_pressure(bp);
                } else {
                    press_idx = (press_idx + 1) % n_press;
                }
            }

            /* Boiler outgoing (flow) temperature from hcb_rrd */
            static const char * flow_loggers[] = { "ch_temp", "boiler_temp", "ch_flow_temp", "boiler_out_temp" };
            static int flow_idx = 0;
            get_stock_url(url, sizeof url, "hcb_rrd?action=getRrdData&loggerName=%s&rra=5min&samples=1",
                     flow_loggers[flow_idx]);
            if (http_fetch_to(url, buf, sizeof buf, 2) == 0) {
                float tf = 0.0f;
                if (parse_rrd_val(buf, &tf) && tf > 0.0f) {
                    toon_state.boiler_out_temp = (tf > 150.0f) ? tf / 100.0f : tf;
                } else {
                    flow_idx = (flow_idx + 1) % 4;
                }
            }

            /* Boiler incoming (return) temperature from hcb_rrd */
            static const char * ret_loggers[] = { "ch_return_temp", "boiler_return_temp", "boiler_in_temp" };
            static int ret_idx = 0;
            get_stock_url(url, sizeof url, "hcb_rrd?action=getRrdData&loggerName=%s&rra=5min&samples=1",
                     ret_loggers[ret_idx]);
            if (http_fetch_to(url, buf, sizeof buf, 2) == 0) {
                float tr = 0.0f;
                if (parse_rrd_val(buf, &tr) && tr > 0.0f) {
                    toon_state.boiler_in_temp = (tr > 150.0f) ? tr / 100.0f : tr;
                } else {
                    ret_idx = (ret_idx + 1) % 3;
                }
            }
        }

        sleep(3);
    }
}

static void * reader_thread(void * arg) {
    (void)arg;
    char url[160], cmd[256], line[8192], probe_buf[2048];
    sleep(8);                            /* initial settling delay for WiFi at boot */
    for (;;) {
        if (!host_ok(settings.master_host)) {
            toon_state.connected = 0;
            sleep(10);
            continue;
        }

        /* Probe master: check if FreeToon PWA port 10081 is open */
        snprintf(url, sizeof url, "http://%s:%d/api/state", settings.master_host, MASTER_PORT);
        if (http_fetch_to(url, probe_buf, sizeof probe_buf, 2) == 0 && strstr(probe_buf, "indoor_temp")) {
            g_is_stock_master = 0;
            fprintf(stderr, "[client_link] connected to FreeToon master %s\n", settings.master_host);
            snprintf(url, sizeof url, "http://%s:%d/api/state/stream",
                     settings.master_host, MASTER_PORT);
            snprintf(cmd, sizeof cmd,
                     "curl -s -N --connect-timeout 8 -A freetoon-client '%s' 2>/dev/null",
                     url);
            FILE * f = popen(cmd, "r");
            if (!f) { toon_state.connected = 0; sleep(5); continue; }
            while (fgets(line, sizeof line, f)) {
                if (strncmp(line, "data:", 5) == 0) {
                    char * j = line + 5;
                    while (*j == ' ') j++;
                    if (*j == '{') client_link_apply_state(j);
                }
            }
            pclose(f);
            toon_state.connected = 0;
            sleep(5);
            continue;
        }

        /* Check if master is a stock Toon with happ_thermstat on port 80 */
        get_stock_url(url, sizeof url, "happ_thermstat?action=getThermostatInfo");
        if (http_fetch_to(url, probe_buf, sizeof probe_buf, 2) == 0 && strstr(probe_buf, "currentTemp")) {
            g_is_stock_master = 1;
            poll_stock_master();
            sleep(5);
            continue;
        }

        toon_state.connected = 0;
        sleep(10);
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
    if (g_is_stock_master) {
        int centi = (int)(temp * 100.0f + 0.5f);
        char url[512], buf[256];
        get_stock_url(url, sizeof url, "happ_thermstat?action=roomSetpoint&Setpoint=%d", centi);
        int rc = http_fetch_to(url, buf, sizeof buf, 3);
        if (rc == 0) toon_state.setpoint = temp;
        return rc;
    }
    char b[48];
    snprintf(b, sizeof b, "{\"value\":\"%.2f\"}", (double)temp);
    return post_json("/api/setpoint", b);
}

int client_link_program(int state) {
    if (g_is_stock_master) {
        char url[512], buf[256];
        int rc;
        if (state < 0) {
            /* Manual hold */
            int ts = (toon_state.active_state >= 0 && toon_state.active_state <= 3) ? toon_state.active_state : 1;
            get_stock_url(url, sizeof url, "happ_thermstat?action=changeSchemeState&state=0&temperatureState=%d", ts);
            rc = http_fetch_to(url, buf, sizeof buf, 3);
            if (rc == 0) {
                toon_state.program_state = 0;
                toon_state.active_state  = -1;
            }
            return rc;
        }
        /* Presets 0..3: 0=Comfort 1=Home 2=Sleep 3=Away */
        int mode;
        if (toon_state.program_state == 8) mode = 2;       /* LOCKEDBASE -> TEMPOVERRIDE */
        else if (toon_state.program_state == 0) mode = 0;  /* MANUAL -> MANUAL */
        else if (toon_state.active_state == state) mode = 8;/* tap same -> LOCKEDBASE */
        else mode = 2;                                     /* TEMPOVERRIDE */

        get_stock_url(url, sizeof url, "happ_thermstat?action=changeSchemeState&state=%d&temperatureState=%d", mode, state);
        rc = http_fetch_to(url, buf, sizeof buf, 3);
        if (rc == 0) {
            toon_state.active_state  = state;
            toon_state.program_state = mode;
        }
        return rc;
    }
    char b[32];
    snprintf(b, sizeof b, "{\"state\":%d}", state);
    return post_json("/api/program", b);
}

int client_link_resume_schedule(void) {
    if (g_is_stock_master) {
        char url[512], buf[256];
        int now_state = toon_state.active_state >= 0 ? toon_state.active_state : 1;
        get_stock_url(url, sizeof url, "happ_thermstat?action=changeSchemeState&state=1&temperatureState=%d", now_state);
        int rc = http_fetch_to(url, buf, sizeof buf, 3);
        if (rc == 0) toon_state.program_state = 1;
        return rc;
    }
    char b[32];
    snprintf(b, sizeof b, "{\"state\":-2}");
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
