/*
 * Minimal Home Assistant REST client. Used by the home-screen Curtains
 * tile to read cover state and issue open/close/stop commands against the
 * cover entity configured in settings.curtain_entity, plus the room-lights
 * screen. Host = settings.ha_host; entities all come from settings/config
 * so nothing personal is baked into the binary.
 *
 * Auth: a single-line Long-Lived Access Token at /mnt/data/ha.cfg.
 */

#include "homeassistant.h"
#include "http.h"
#include "notify.h"
#include "settings.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* Host comes from settings (settings.ha_host, "ip:port" no scheme) so no
 * personal address ships in the binary. Empty host = HA disabled. */
#define HA_HOST   (settings.ha_host)
#define HA_POLL_S 10
#define HA_TOKEN_PATH "/mnt/data/ha.cfg"
/* Last good GPS fix per tracker, persisted so a tag drop-out (unknown/
 * unavailable) or a UI restart still shows the last known location instead
 * of blanking to "unknown". */
#define HA_TRACKERS_PATH "/mnt/data/ha_trackers.dat"

/* Curtain entity ids are user-specific too — sourced from settings.
 * Empty curtain entity = curtain tile inactive. */
#define CURTAIN_GROUP   (settings.curtain_entity)
#define CURTAIN_LEFT    (settings.curtain_bat_a)
#define CURTAIN_RIGHT   (settings.curtain_bat_b)

ha_state_t ha_state = {0};

static char g_token[256] = "";

static void load_token(void) {
    FILE * f = fopen(HA_TOKEN_PATH, "r");
    if (!f) return;
    if (fgets(g_token, sizeof(g_token), f)) {
        char * nl = strchr(g_token, '\n'); if (nl) *nl = 0;
        char * cr = strchr(g_token, '\r'); if (cr) *cr = 0;
    }
    fclose(f);
}

/* On-device token config (Settings -> Integrations -> Configure HA entities).
 * The Long-Lived Access Token lives one-per-line in /mnt/data/ha.cfg. */
void ha_get_token(char * out, size_t n) {
    if (!out || n == 0) return;
    if (!g_token[0]) load_token();
    snprintf(out, n, "%s", g_token);
}
int ha_set_token(const char * tok) {
    if (!tok) return -1;
    FILE * f = fopen(HA_TOKEN_PATH, "w");
    if (!f) return -1;
    fprintf(f, "%s\n", tok);
    fclose(f);
    snprintf(g_token, sizeof g_token, "%s", tok);   /* live, no restart needed */
    return 0;
}

/* An entity id / cover group goes straight into a popen'd curl command line,
 * so guard against shell-metachar injection: HA ids are only [A-Za-z0-9._-].
 * Reject anything else (and empty). Defence-in-depth alongside the input
 * sanitizer — values reach here from settings the user types. */
static int ha_id_safe(const char * s) {
    if (!s || !s[0]) return 0;
    for (const char * p = s; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* Lifted from ventilation.c — tiny flat-JSON helpers, no recursion. */
static int extract_int(const char * json, const char * key, int * out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char * p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p == '"') return 0;  /* string, not int */
    if (*p == 'f' && !strncmp(p, "false", 5)) { *out = 0; return 1; }
    if (*p == 't' && !strncmp(p, "true",  4)) { *out = 1; return 1; }
    *out = (int)strtol(p, NULL, 10);
    return 1;
}

static int extract_double(const char * json, const char * key, double * out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char * p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p == '"' || *p == 'n') return 0;   /* string / null */
    *out = strtod(p, NULL);
    return 1;
}

static int extract_str(const char * json, const char * key, char * out, size_t outsz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char * p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    const char * e = strchr(p, '"');
    if (!e) return 0;
    size_t n = e - p; if (n > outsz - 1) n = outsz - 1;
    memcpy(out, p, n); out[n] = 0;
    return 1;
}

/* GET /api/states/<entity_id> with Bearer auth. Shells out to curl —
 * http.c's http_fetch doesn't take headers, and adding a header parameter
 * everywhere is more churn than just inlining popen here. */
static int ha_get_state(const char * entity_id, char * out, size_t out_max) {
    if (!g_token[0] || !ha_id_safe(entity_id)) return -1;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s --max-time 6 --connect-timeout 3 "
        "-H 'Authorization: Bearer %s' "
        "'http://%s/api/states/%s' 2>/dev/null",
        g_token, HA_HOST, entity_id);
    FILE * p = popen(cmd, "r");
    if (!p) return -1;
    size_t n = fread(out, 1, out_max - 1, p);
    out[n] = 0;
    int rc = pclose(p);
    return (rc == 0 && n > 0) ? 0 : -1;
}

/* GET /api/calendars/<entity>?start=&end= with Bearer auth — fills `out` with
 * HA's JSON event array. Public so calendar.c can pull HA events without
 * duplicating the token plumbing. Lazy-loads the token if the HA thread hasn't
 * yet. Returns 0 on success. */
int ha_fetch_calendar(const char * entity, const char * start_iso,
                      const char * end_iso, char * out, size_t out_max) {
    if (!g_token[0]) load_token();
    if (!g_token[0] || !ha_id_safe(entity) || !HA_HOST[0]) return -1;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s --max-time 8 --connect-timeout 4 "
        "-H 'Authorization: Bearer %s' "
        "'http://%s/api/calendars/%s?start=%s&end=%s' 2>/dev/null",
        g_token, HA_HOST, entity, start_iso, end_iso);
    FILE * p = popen(cmd, "r");
    if (!p) return -1;
    size_t n = fread(out, 1, out_max - 1, p);
    out[n] = 0;
    int rc = pclose(p);
    return (rc == 0 && n > 0) ? 0 : -1;
}

/* POST /api/services/<domain>/<service> with entity_id and optional extra JSON.
 * Returns 0 on HTTP 2xx. extra_json is appended INSIDE the body after the
 * entity_id, e.g. extra_json = ",\"brightness_pct\":50". Pass NULL for
 * plain entity-id-only calls. */
static int ha_call_service(const char * domain, const char * service,
                           const char * entity_id, const char * extra_json) {
    if (!g_token[0] || !ha_id_safe(entity_id)) return -1;
    char cmd[1024], out[256];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s --max-time 6 --connect-timeout 3 "
        "-X POST -H 'Authorization: Bearer %s' "
        "-H 'Content-Type: application/json' "
        "--data '{\"entity_id\":\"%s\"%s}' "
        "'http://%s/api/services/%s/%s' 2>/dev/null",
        g_token, entity_id,
        extra_json ? extra_json : "",
        HA_HOST, domain, service);
    FILE * p = popen(cmd, "r");
    if (!p) {
        notify_show("system", "ha_offline", "HA niet bereikbaar — actie niet uitgevoerd");
        return -1;
    }
    size_t n = fread(out, 1, sizeof(out) - 1, p);
    out[n] = 0;
    int rc = pclose(p);
    if (rc != 0 || n == 0)
        notify_show("system", "ha_offline", "HA niet bereikbaar — actie niet uitgevoerd");
    else
        notify_clear("system", "ha_offline");
    return (rc == 0) ? 0 : -1;
}

/* Thin wrappers — keep the old signatures so existing callers don't change. */
static int ha_call_cover_service_only(const char * action) {
    if (!ha_id_safe(CURTAIN_GROUP)) return -1;
    return ha_call_service("cover", action, CURTAIN_GROUP, NULL);
}

static int ha_call_light_service_only(const char * action, const char * entity_id) {
    return ha_call_service("light", action, entity_id, NULL);
}

/* Room lights for the home Lights screen — loaded at runtime from
 * /mnt/data/ha_lights.conf (one "entity_id|Name|Area" per line, '#' or
 * blank lines ignored) so no personal entity ids ship in the binary.
 * Empty array until ha_lights_load() runs; ha_light_count = rows loaded. */
ha_light_t ha_lights[HA_LIGHT_COUNT];
int        ha_light_count = 0;

#define HA_LIGHTS_CONF "/mnt/data/ha_lights.conf"

static void ha_lights_load(void) {
    ha_light_count = 0;
    FILE * f = fopen(HA_LIGHTS_CONF, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f) && ha_light_count < HA_LIGHT_COUNT) {
        char * nl = strchr(line, '\n'); if (nl) *nl = 0;
        char * cr = strchr(line, '\r'); if (cr) *cr = 0;
        if (!line[0] || line[0] == '#') continue;
        /* entity_id | name | area */
        char * bar1 = strchr(line, '|');
        if (!bar1) continue;
        *bar1 = 0;
        char * name = bar1 + 1;
        char * bar2 = strchr(name, '|');
        const char * area = "";
        if (bar2) { *bar2 = 0; area = bar2 + 1; }
        ha_light_t * L = &ha_lights[ha_light_count];
        snprintf(L->entity_id, sizeof(L->entity_id), "%s", line);
        snprintf(L->name,      sizeof(L->name),      "%s", name);
        snprintf(L->area,      sizeof(L->area),      "%s", area);
        L->on = 0; L->available = 0; L->brightness = -1;
        ha_light_count++;
    }
    fclose(f);
    fprintf(stderr, "[ha] loaded %d lights from " HA_LIGHTS_CONF "\n", ha_light_count);
}

/* Refresh every light's on/availability/brightness. Cheap — one
 * /api/states/<id> call per light. ~14 small requests, runs in the same
 * thread as the curtain poll. */
/* Parse a light state object into one ha_light_t. */
static void apply_light(ha_light_t * L, const char * body) {
    char st[24] = {0};
    extract_str(body, "state", st, sizeof(st));
    if (!strcmp(st, "on"))        { L->available = 1; L->on = 1; }
    else if (!strcmp(st, "off"))  { L->available = 1; L->on = 0; }
    else                          { L->available = 0; L->on = 0; }
    int v;
    if (extract_int(body, "brightness", &v)) L->brightness = v;
}

static void poll_lights(void) {
    char body[512];
    for (int i = 0; i < ha_light_count; i++) {
        ha_light_t * L = &ha_lights[i];
        if (ha_get_state(L->entity_id, body, sizeof(body)) != 0) {
            L->available = 0;
            continue;
        }
        apply_light(L, body);
    }
}

/* Async helpers for the screen handlers. Each runs the actual REST POST
 * on a detached thread so LVGL stays responsive; the next poll picks up
 * the new state within a few seconds. */
static void * light_action_thread(void * arg) {
    char ** p = (char **)arg;       /* [action, entity_id] */
    ha_call_light_service_only(p[0], p[1]);
    poll_lights();
    free(p[0]); free(p[1]); free(p);
    return NULL;
}

static void fire_light_action(const char * action, const char * entity_id) {
    char ** p = malloc(2 * sizeof(char *));
    if (!p) return;
    p[0] = strdup(action);
    p[1] = strdup(entity_id);
    if (!p[0] || !p[1]) { free(p[0]); free(p[1]); free(p); return; }
    pthread_t t;
    if (pthread_create(&t, NULL, light_action_thread, p) != 0) {
        free(p[0]); free(p[1]); free(p);
        return;
    }
    pthread_detach(t);
}

void ha_light_toggle_async(const char * entity_id) {
    fire_light_action("toggle", entity_id);
}

void ha_lights_all_on_async(void)  { fire_light_action("turn_on",  "light.all_lights"); }
void ha_lights_all_off_async(void) { fire_light_action("turn_off", "light.all_lights"); }

/* ======================= Unified dynamic device list =======================
 * One user-managed list (lights, covers, switches, scripts, scenes) loaded
 * from /mnt/data/ha_devices.conf. Replaces the old fixed cover slots; the
 * Devices screen + home pinned-tiles render it. Control routes through the
 * generic ha_call_service(), state through ha_get_state(). */
#define HA_DEVICES_CONF "/mnt/data/ha_devices.conf"

ha_device_t ha_devices[HA_DEVICE_MAX];
int         ha_device_count = 0;

const char * hadev_type_str(int type) {
    switch (type) {
        case HADEV_COVER:  return "cover";
        case HADEV_SWITCH: return "switch";
        case HADEV_SCRIPT: return "script";
        case HADEV_SCENE:  return "scene";
        default:           return "light";
    }
}
int hadev_type_from_str(const char * s) {
    if (!s)                    return HADEV_LIGHT;
    if (!strcmp(s, "cover"))   return HADEV_COVER;
    if (!strcmp(s, "switch"))  return HADEV_SWITCH;
    if (!strcmp(s, "script"))  return HADEV_SCRIPT;
    if (!strcmp(s, "scene"))   return HADEV_SCENE;
    return HADEV_LIGHT;
}

void ha_devices_save(void) {
    FILE * f = fopen(HA_DEVICES_CONF, "w");
    if (!f) return;
    fprintf(f, "# type|entity_id|Name|pin   (type = light|cover|switch|script|scene)\n");
    for (int i = 0; i < ha_device_count; i++) {
        ha_device_t * D = &ha_devices[i];
        fprintf(f, "%s|%s|%s|%d\n", hadev_type_str(D->type),
                D->entity_id, D->name, D->pin_home ? 1 : 0);
    }
    fclose(f);
}

static void devices_add(int type, const char * entity, const char * name, int pin) {
    if (ha_device_count >= HA_DEVICE_MAX || !entity || !entity[0]) return;
    if (!ha_id_safe(entity)) return;
    ha_device_t * D = &ha_devices[ha_device_count++];
    memset((void *)D, 0, sizeof *D);
    D->type = type;
    snprintf(D->entity_id, sizeof D->entity_id, "%s", entity);
    snprintf(D->name, sizeof D->name, "%s", (name && name[0]) ? name : entity);
    D->pin_home   = pin ? 1 : 0;
    D->brightness = -1;
    D->position   = -1;
}

void ha_devices_load(void) {
    ha_device_count = 0;
    FILE * f = fopen(HA_DEVICES_CONF, "r");
    if (f) {
        char line[200];
        while (fgets(line, sizeof line, f) && ha_device_count < HA_DEVICE_MAX) {
            char * nl = strchr(line, '\n'); if (nl) *nl = 0;
            char * cr = strchr(line, '\r'); if (cr) *cr = 0;
            if (!line[0] || line[0] == '#') continue;
            /* type | entity_id | name | pin */
            char * p_ent = strchr(line, '|'); if (!p_ent) continue; *p_ent++ = 0;
            const char * name = ""; int pin = 0;
            char * p_name = strchr(p_ent, '|');
            if (p_name) {
                *p_name++ = 0;
                char * p_pin = strchr(p_name, '|');
                if (p_pin) { *p_pin++ = 0; pin = atoi(p_pin); }
                name = p_name;
            }
            devices_add(hadev_type_from_str(line), p_ent, name, pin);
        }
        fclose(f);
        fprintf(stderr, "[ha] loaded %d devices from " HA_DEVICES_CONF "\n", ha_device_count);
        return;
    }
    /* No ha_devices.conf yet — migrate from the legacy ha_lights.conf and the
     * old fixed curtain/blinds cover slots, then persist so future edits stick. */
    ha_lights_load();
    for (int i = 0; i < ha_light_count; i++)
        devices_add(HADEV_LIGHT, ha_lights[i].entity_id, ha_lights[i].name, 0);
    /* pin=0: devices are opt-in for the home quick-tiles (toggle "Home" per
     * device in Settings -> Devices). Keeps the default home uncluttered; the
     * full list is always one tap away on the Devices screen. */
    if (settings.curtain_entity[0])
        devices_add(HADEV_COVER, settings.curtain_entity, "Gordijnen",  0);
    if (settings.blinds_entity[0])
        devices_add(HADEV_COVER, settings.blinds_entity,  "Jaloezieen", 0);
    if (ha_device_count > 0) ha_devices_save();
    fprintf(stderr, "[ha] migrated %d devices to " HA_DEVICES_CONF "\n", ha_device_count);
}

/* Parse a state object into one device (by type). */
static void apply_device(ha_device_t * D, const char * body) {
    char st[24] = {0};
    extract_str(body, "state", st, sizeof st);
    if (D->type == HADEV_COVER) {
        D->available = (st[0] && strcmp(st, "unavailable") && strcmp(st, "unknown"));
        snprintf(D->state, sizeof D->state, "%s", st);
        int v;
        if (extract_int(body, "current_position", &v)) D->position = v;
    } else {                          /* light / switch */
        if (!strcmp(st, "on"))        { D->available = 1; D->on = 1; }
        else if (!strcmp(st, "off"))  { D->available = 1; D->on = 0; }
        else                          { D->available = 0; D->on = 0; }
        int v;
        if (extract_int(body, "brightness", &v)) D->brightness = v;
    }
}

/* Refresh every stateful device — one /api/states call each. Scripts/scenes
 * are stateless and skipped. */
static void poll_devices(void) {
    char body[768];
    for (int i = 0; i < ha_device_count; i++) {
        ha_device_t * D = &ha_devices[i];
        if (D->type == HADEV_SCRIPT || D->type == HADEV_SCENE) continue;
        if (ha_get_state(D->entity_id, body, sizeof body) != 0) { D->available = 0; continue; }
        apply_device(D, body);
    }
}

/* Generic fire-and-forget service call + a quick re-poll so the screen
 * reflects the result within a second instead of waiting for the next pass. */
struct ha_svc_arg { char domain[12]; char service[24]; char entity[64]; };
static void * ha_svc_thread(void * arg) {
    struct ha_svc_arg * a = (struct ha_svc_arg *)arg;
    ha_call_service(a->domain, a->service, a->entity, NULL);
    poll_devices();
    free(a);
    return NULL;
}
static void ha_service_async(const char * domain, const char * service, const char * entity) {
    struct ha_svc_arg * a = malloc(sizeof *a);
    if (!a) return;
    snprintf(a->domain,  sizeof a->domain,  "%s", domain);
    snprintf(a->service, sizeof a->service, "%s", service);
    snprintf(a->entity,  sizeof a->entity,  "%s", entity);
    pthread_t t;
    if (pthread_create(&t, NULL, ha_svc_thread, a) != 0) { free(a); return; }
    pthread_detach(t);
}

void ha_device_toggle_async(int type, const char * entity_id) {
    ha_service_async(type == HADEV_SWITCH ? "switch" : "light", "toggle", entity_id);
}
void ha_device_cover_async(const char * entity_id, const char * cmd) {
    const char * svc = !strcmp(cmd, "open")  ? "open_cover"
                     : !strcmp(cmd, "close") ? "close_cover"
                                             : "stop_cover";
    ha_service_async("cover", svc, entity_id);
}
void ha_device_run_async(int type, const char * entity_id) {
    ha_service_async(type == HADEV_SCENE ? "scene" : "script", "turn_on", entity_id);
}

/* ---- list editing (settings device manager) ---- */
int ha_device_add(int type, const char * entity_id, const char * name, int pin) {
    int before = ha_device_count;
    devices_add(type, entity_id, name, pin);
    if (ha_device_count == before) return -1;   /* rejected: full / unsafe id */
    ha_devices_save();
    return ha_device_count - 1;
}
void ha_device_remove(int idx) {
    if (idx < 0 || idx >= ha_device_count) return;
    for (int i = idx; i < ha_device_count - 1; i++) ha_devices[i] = ha_devices[i + 1];
    ha_device_count--;
    ha_devices_save();
}
void ha_device_set_pin(int idx, int pin) {
    if (idx < 0 || idx >= ha_device_count) return;
    ha_devices[idx].pin_home = pin ? 1 : 0;
    ha_devices_save();
}

/* Strip leading/trailing whitespace in place. Returns the input pointer
 * (possibly advanced) so callers can chain. */
static char * trim(char * s) {
    while (*s == ' ' || *s == '\t') s++;
    char * e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

/* Pull the house number out of a "<num> <street>" or "<street> <num>"
 * fragment. Writes the number into *num_out (may be empty if not found)
 * and the street name into *street_out, both NUL-terminated. */
static void split_street(const char * src, char * street_out, size_t street_sz,
                                              char * num_out,    size_t num_sz) {
    street_out[0] = 0;
    num_out[0]    = 0;
    char buf[160];
    snprintf(buf, sizeof(buf), "%s", src);
    char * s = trim(buf);

    /* Case 1: leading digits — "32 Graaf Florislaan". */
    if (s[0] >= '0' && s[0] <= '9') {
        char * sp = s;
        while (*sp && *sp != ' ') sp++;
        size_t nl = (size_t)(sp - s);
        if (nl >= num_sz) nl = num_sz - 1;
        memcpy(num_out, s, nl); num_out[nl] = 0;
        while (*sp == ' ') sp++;
        snprintf(street_out, street_sz, "%s", sp);
        return;
    }
    /* Case 2: trailing digits — "Graafland 32". Scan back from end. */
    size_t L = strlen(s);
    if (L == 0) return;
    char * end = s + L;
    char * p = end;
    while (p > s && p[-1] >= '0' && p[-1] <= '9') p--;
    if (p < end && p > s && p[-1] == ' ') {
        snprintf(num_out, num_sz, "%s", p);
        *--p = 0;
        snprintf(street_out, street_sz, "%s", trim(s));
        return;
    }
    /* No number recognised — treat the whole thing as street. */
    snprintf(street_out, street_sz, "%s", s);
}

/* Strip a leading postcode (e.g. "1693 AT Wervershoof" → "Wervershoof"). */
static const char * strip_postcode(const char * city) {
    while (*city == ' ') city++;
    const char * p = city;
    int saw_digit = 0;
    while ((*p >= '0' && *p <= '9') || *p == ' ') { if (*p >= '0') saw_digit = 1; p++; }
    if (saw_digit) {
        /* skip an optional letter block ("AT") + spaces */
        while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) p++;
        while (*p == ' ') p++;
        if (*p) return p;
    }
    return city;
}

/* Fetch a device_tracker.life360_* and format as "City > Street > Number".
 *   home → "home"
 *   else → "<city/region> > <street> > <number>"
 * Falls back gracefully when fields are missing: drops the empty
 * segments instead of leaving stray "> >" markers. */
/* Life360's address attribute often lacks the city (just "street, province"),
 * so reverse-geocode the GPS via OSM Nominatim to get the real town/city.
 * Cached per person on a ~100m grid so we don't hammer the service. */
static char geo_key[3][40];
static char geo_city[3][64];
/* Reverse-geocode lat/lon for a tracker.
 *   want_street=0 → town/city name (zoom 14) — used to supplement a Life360
 *                   address that only carries the province.
 *   want_street=1 → street name (zoom 18, the road field, NO house number) —
 *                   used for GPS-only trackers (e.g. a Google Find My car) where
 *                   the street is more useful than the town. Falls back to the
 *                   town when no road is returned (e.g. on open road / water). */
static void reverse_city(double lat, double lon, int person, int want_street,
                         char * out, size_t osz) {
    out[0] = 0;
    if (person < 0 || person > 2) return;
    char key[40];
    snprintf(key, sizeof key, "%d:%.3f,%.3f", want_street, lat, lon);   /* ~100m bucket */
    if (strcmp(key, geo_key[person]) == 0) { snprintf(out, osz, "%s", geo_city[person]); return; }
    char cmd[400];
    snprintf(cmd, sizeof cmd,
        "/usr/bin/curl -fsSL -k --max-time 8 -A 'freetoon-lvgl/1.0' "
        "'https://nominatim.openstreetmap.org/reverse?lat=%.5f&lon=%.5f"
        "&format=json&zoom=%d&addressdetails=1'", lat, lon, want_street ? 18 : 14);
    FILE * fp = popen(cmd, "r");
    if (!fp) return;
    static char body[4096];
    size_t n = fread(body, 1, sizeof body - 1, fp);
    pclose(fp);
    body[n] = 0;
    /* Street first when asked (road, then pedestrian/footway-style ways). */
    if (want_street) {
        const char * skeys[] = { "road", "pedestrian", "footway", "cycleway", "path", NULL };
        for (int i = 0; skeys[i]; i++)
            if (extract_str(body, skeys[i], out, osz) && out[0]) break;
    }
    /* City/town — the want_street fallback, or the primary result otherwise. */
    if (!out[0]) {
        const char * keys[] = { "city", "town", "village", "municipality", "suburb", NULL };
        for (int i = 0; keys[i]; i++)
            if (extract_str(body, keys[i], out, osz) && out[0]) break;
    }
    snprintf(geo_key[person], sizeof geo_key[person], "%s", key);
    snprintf(geo_city[person], sizeof geo_city[person], "%s", out);
}

/* Persist / restore the last good fix for all three trackers, so a tag
 * drop-out or a UI restart keeps showing the last known location. */
static void ha_trackers_load(void) {
    FILE * f = fopen(HA_TRACKERS_PATH, "r");
    if (!f) return;
    float la, lo, lb, ob, lc, oc;
    if (fscanf(f, "%f %f %f %f %f %f", &la, &lo, &lb, &ob, &lc, &oc) == 6) {
        ha_state.lat_a = la; ha_state.lon_a = lo;
        ha_state.lat_b = lb; ha_state.lon_b = ob;
        ha_state.lat_c = lc; ha_state.lon_c = oc;
    }
    fclose(f);
}
static void ha_trackers_save(void) {
    FILE * f = fopen(HA_TRACKERS_PATH, "w");
    if (!f) return;
    fprintf(f, "%.6f %.6f\n%.6f %.6f\n%.6f %.6f\n",
            ha_state.lat_a, ha_state.lon_a,
            ha_state.lat_b, ha_state.lon_b,
            ha_state.lat_c, ha_state.lon_c);
    fclose(f);
}

/* Parse a life360 state object (REST /api/states body or a WS new_state) into
 * the location string + coords. */
static void apply_life360(const char * body, char * out, size_t outsz,
                          volatile float * lat, volatile float * lon, int person) {
    if (lat && lon) {
        double dlat, dlon;
        if (extract_double(body, "latitude", &dlat) &&
            extract_double(body, "longitude", &dlon)) {
            float nlat = (float)dlat, nlon = (float)dlon;
            /* Only rewrite the persist file on a real change (>~1 m) so periodic
             * polls re-reading the same fix don't churn the eMMC. */
            float ddlat = nlat - *lat, ddlon = nlon - *lon;
            if (ddlat < 0) ddlat = -ddlat;
            if (ddlon < 0) ddlon = -ddlon;
            int changed = (ddlat > 1e-5f || ddlon > 1e-5f);
            *lat = nlat; *lon = nlon;
            if (changed) ha_trackers_save();
        }
    }
    char state[24] = {0};
    extract_str(body, "state", state, sizeof(state));
    char addr[160] = {0};
    extract_str(body, "address", addr, sizeof(addr));

    if (!addr[0]) {
        /* GPS-only tracker (no Life360 address, e.g. a Google Find My car).
         * Its fix is noisy (±tens of metres), so HA's home zone flaps the
         * state between "home" and "not_home" while the device is parked in
         * one spot. Always reverse-geocode the street from the coordinates —
         * even when HA says "home" — so the label is a stable street name
         * instead of a flapping home/away. Falls back to "home"/raw state only
         * when there are no usable coords. */
        if (lat && lon && (*lat != 0.0f || *lon != 0.0f)) {
            char gstreet[64] = {0};
            reverse_city(*lat, *lon, person, 1, gstreet, sizeof gstreet);
            if (gstreet[0]) { snprintf(out, outsz, "%s", gstreet); return; }
        }
        if (strcasecmp(state, "home") == 0) { snprintf(out, outsz, "home"); return; }
        if (state[0]) snprintf(out, outsz, "%s", state);
        return;
    }
    /* Trackers that carry an address (Life360) keep honouring the home zone. */
    if (strcasecmp(state, "home") == 0) {
        snprintf(out, outsz, "home");
        return;
    }
    /* Split address on commas — Life360 emits e.g.
     *   "32 Graaf Florislaan, North Holland"
     *   "Graafland 32, 1693AT Wervershoof, Netherlands"
     * We treat parts[0] as the street, parts[1] (postcode stripped) as
     * the city/region; deeper parts (country) get ignored. */
    char work[160];
    snprintf(work, sizeof(work), "%s", addr);
    char * c1 = strchr(work, ',');
    if (c1) *c1 = 0;
    char * c2 = (c1 ? strchr(c1 + 1, ',') : NULL);
    if (c2) *c2 = 0;

    char street[96] = {0}, num[16] = {0};
    split_street(work, street, sizeof(street), num, sizeof(num));
    /* Prefer the reverse-geocoded city (Life360 only gives the province); fall
     * back to the address's region segment when geocoding has nothing yet. */
    char gcity[64] = {0};
    if (lat && lon && (*lat != 0.0f || *lon != 0.0f))
        reverse_city(*lat, *lon, person, 0, gcity, sizeof gcity);   /* city to supplement the address */
    const char * city = gcity[0] ? gcity : (c1 ? strip_postcode(trim(c1 + 1)) : "");

    /* Compose "city > street > number" with graceful elision. */
    char tmp[160];
    int n = 0;
    if (city && city[0]) n += snprintf(tmp + n, sizeof(tmp) - n, "%s", city);
    if (street[0]) n += snprintf(tmp + n, sizeof(tmp) - n,
                                 "%s%s", (n ? " > " : ""), street);
    if (num[0])    n += snprintf(tmp + n, sizeof(tmp) - n,
                                 "%s%s", (n ? " > " : ""), num);
    snprintf(out, outsz, "%s", n ? tmp : addr);
}

static void poll_life360_one(const char * entity_id, char * out, size_t outsz,
                             volatile float * lat, volatile float * lon, int person) {
    char body[1536];
    if (ha_get_state(entity_id, body, sizeof(body)) == 0)
        apply_life360(body, out, outsz, lat, lon, person);
}

static void poll_life360(void) {
    if (settings.life360_a_entity[0])
        poll_life360_one(settings.life360_a_entity,
                         ha_state.loc_a, sizeof(ha_state.loc_a),
                         &ha_state.lat_a, &ha_state.lon_a, 0);
    if (settings.life360_b_entity[0])
        poll_life360_one(settings.life360_b_entity,
                         ha_state.loc_b,   sizeof(ha_state.loc_b),
                         &ha_state.lat_b, &ha_state.lon_b, 1);
    if (settings.life360_c_entity[0])
        poll_life360_one(settings.life360_c_entity,
                         ha_state.loc_c,   sizeof(ha_state.loc_c),
                         &ha_state.lat_c, &ha_state.lon_c, 2);
}

/* Fetch a fresh snapshot of the configured doorbell camera into
 * DOORBELL_SNAP_PATH (JPEG). Returns 0 on success. */
static int fetch_doorbell_snapshot(void) {
    if (!g_token[0] || !settings.doorbell_camera[0]) return -1;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s --max-time 8 --connect-timeout 3 "
        "-H 'Authorization: Bearer %s' "
        "-o '%s' 'http://%s/api/camera_proxy/%s' 2>/dev/null",
        g_token, DOORBELL_SNAP_PATH, HA_HOST, settings.doorbell_camera);
    int rc = system(cmd);
    return rc == 0 ? 0 : -1;
}

/* Stream an MJPEG (multipart/x-mixed-replace) feed while the overlay is up,
 * writing each JPEG frame to DOORBELL_SNAP_PATH and bumping doorbell_frame so
 * the UI redraws it. Server-transcoded MJPEG (e.g. a Frigate/go2rtc resized
 * stream) keeps the Toon's job to cheap JPEG decode. Runs in the poll thread;
 * returns when doorbell_live clears or the stream drops. */
static void stream_doorbell_mjpeg(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s --max-time 120 '%s' 2>/dev/null",
        settings.doorbell_stream_url);
    FILE * p = popen(cmd, "r");
    if (!p) return;

    /* Accumulate bytes, split on JPEG SOI(FFD8FF)/EOI(FFD9). We don't parse the
     * MIME boundary — scanning for JPEG markers is simpler and boundary-agnostic. */
    static unsigned char buf[256 * 1024];
    size_t len = 0;
    int have_soi = 0;
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", DOORBELL_SNAP_PATH);

    while (ha_state.doorbell_live) {
        size_t n = fread(buf + len, 1, sizeof(buf) - len, p);
        if (n == 0) break;                 /* stream ended / timed out */
        len += n;

        for (;;) {
            if (!have_soi) {
                size_t i;
                for (i = 0; i + 2 < len; i++)
                    if (buf[i] == 0xFF && buf[i+1] == 0xD8 && buf[i+2] == 0xFF) break;
                if (i + 2 >= len) {         /* no SOI yet — keep last 2 bytes */
                    if (len > 2) { memmove(buf, buf + len - 2, 2); len = 2; }
                    break;
                }
                memmove(buf, buf + i, len - i);   /* drop pre-SOI junk */
                len -= i;
                have_soi = 1;
            }
            /* find EOI (FFD9) after the SOI */
            size_t j;
            for (j = 2; j + 1 < len; j++)
                if (buf[j] == 0xFF && buf[j+1] == 0xD9) break;
            if (j + 1 >= len) {             /* incomplete frame */
                if (len >= sizeof(buf)) { len = 0; have_soi = 0; }  /* oversized — resync */
                break;
            }
            size_t framelen = j + 2;
            FILE * fo = fopen(tmp, "wb");
            if (fo) {
                fwrite(buf, 1, framelen, fo);
                fclose(fo);
                rename(tmp, DOORBELL_SNAP_PATH);
                ha_state.doorbell_frame++;
            }
            memmove(buf, buf + framelen, len - framelen);
            len -= framelen;
            have_soi = 0;
        }
    }
    pclose(p);
}

/* Watch the doorbell trigger entity. On an off->on transition, fetch a camera
 * snapshot and bump ha_state.doorbell_seq so the UI shows it fullscreen. */
/* Edge-detect the doorbell trigger from a state object. On off->on, fetch a
 * snapshot and bump doorbell_seq so the UI shows it fullscreen. */
static void apply_doorbell(const char * body) {
    static int armed = 0;     /* 1 once we've seen the entity "off" — avoids
                                 firing if it's already on at startup */
    char st[24] = {0};
    extract_str(body, "state", st, sizeof(st));
    int on = (!strcmp(st, "on") || !strcmp(st, "true") ||
              !strcmp(st, "pressed") || !strcmp(st, "ringing"));
    if (!on) { armed = 1; return; }
    if (!armed) return;           /* was already on — ignore */
    armed = 0;                    /* re-arm only after it returns to off */
    if (fetch_doorbell_snapshot() == 0)
        ha_state.doorbell_seq++;  /* signal the UI */
}

static void poll_doorbell(void) {
    if (!settings.doorbell_entity[0]) return;
    char body[1024];
    if (ha_get_state(settings.doorbell_entity, body, sizeof(body)) == 0)
        apply_doorbell(body);
}

/* Parse a curtain (cover) state object into ha_state. */
static void apply_curtain(const char * body) {
    ha_state.connected = 1;
    extract_str(body, "state",
                ha_state.curtain_state, sizeof(ha_state.curtain_state));
    int v;
    if (extract_int(body, "current_position", &v)) ha_state.curtain_pos = v;
    if (extract_int(body, "is_closed",        &v)) ha_state.curtain_is_closed = v;
}

static void poll_once(void) {
    static int miss = 0;
    char body[1024];
    if (!CURTAIN_GROUP[0]) return;   /* no curtain entity configured */
    if (ha_get_state(CURTAIN_GROUP, body, sizeof(body)) != 0) {
        /* Single failed fetch isn't enough to drop the offline flag — HA
         * can be slow during its own service-call processing. Tolerate up
         * to 2 consecutive misses before the UI shows "(HA offline)". */
        if (++miss >= 2) ha_state.connected = 0;
        return;
    }
    miss = 0;
    apply_curtain(body);

    /* Battery — take the min of the two child curtain sensors so the UI
     * shows the one that needs charging first. */
    int bat_min = 100;
    for (const char * id = CURTAIN_LEFT; ; id = CURTAIN_RIGHT) {
        char b[512];
        if (ha_get_state(id, b, sizeof(b)) == 0) {
            char st[16];
            if (extract_str(b, "state", st, sizeof(st))) {
                int p = atoi(st);
                if (p > 0 && p < bat_min) bat_min = p;
            }
        }
        if (strcmp(id, CURTAIN_RIGHT) == 0) break;
    }
    ha_state.curtain_battery = bat_min;
}

/* ======================= Home Assistant WebSocket ======================= */
/* Push instead of polling: connect ws://host/api/websocket, authenticate with
 * the long-lived token, then subscribe_events(state_changed) and update state
 * as events arrive. Current state is seeded once over REST on each (re)connect,
 * since the event stream only carries future changes. */

static const char WS_B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void ws_b64(const unsigned char * in, int n, char * out) {
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        int v = in[i] << 16;
        if (i + 1 < n) v |= in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = WS_B64[(v >> 18) & 63];
        out[o++] = WS_B64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? WS_B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? WS_B64[v & 63] : '=';
    }
    out[o] = 0;
}
static int ws_write_n(int fd, const void * b, size_t n) {
    const char * p = b; size_t left = n;
    while (left) { ssize_t w = send(fd, p, left, MSG_NOSIGNAL);
        if (w <= 0) { if (errno == EINTR) continue; return -1; } p += w; left -= (size_t)w; }
    return 0;
}
static int ws_read_n(int fd, void * b, size_t n) {
    char * p = b; size_t left = n;
    while (left) { ssize_t r = recv(fd, p, left, 0);
        if (r == 0) return -1; if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; left -= (size_t)r; }
    return 0;
}
/* Send one masked client frame (opcode 1=text, 9=ping). */
static int ws_send(int fd, int opcode, const unsigned char * data, size_t len) {
    unsigned char h[8]; int n = 0;
    h[n++] = (unsigned char)(0x80 | opcode);
    if (len < 126) h[n++] = (unsigned char)(0x80 | len);
    else if (len < 65536) { h[n++] = 0x80 | 126; h[n++] = (len >> 8) & 0xff; h[n++] = len & 0xff; }
    else return -1;
    unsigned char mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (unsigned char)(rand() & 0xff);
    if (ws_write_n(fd, h, n) < 0 || ws_write_n(fd, mask, 4) < 0) return -1;
    unsigned char tmp[1024];
    for (size_t i = 0; i < len; ) {
        size_t c = len - i; if (c > sizeof tmp) c = sizeof tmp;
        for (size_t j = 0; j < c; j++) tmp[j] = data[i + j] ^ mask[(i + j) & 3];
        if (ws_write_n(fd, tmp, c) < 0) return -1;
        i += c;
    }
    return 0;
}
/* Receive one (possibly fragmented) message; replies to pings. */
static int ws_recv_msg(int fd, char * buf, size_t bufsz) {
    size_t total = 0;
    for (;;) {
        unsigned char h2[2];
        if (ws_read_n(fd, h2, 2) < 0) return -1;
        int fin = h2[0] & 0x80, opcode = h2[0] & 0x0f;
        unsigned long len = h2[1] & 0x7f;
        if (len == 126) { unsigned char e[2]; if (ws_read_n(fd, e, 2) < 0) return -1; len = (e[0] << 8) | e[1]; }
        else if (len == 127) { unsigned char e[8]; if (ws_read_n(fd, e, 8) < 0) return -1;
            len = 0; for (int i = 4; i < 8; i++) len = (len << 8) | e[i]; }
        size_t room = (total < bufsz - 1) ? bufsz - 1 - total : 0;
        size_t take = (len < room) ? (size_t)len : room;
        if (take && ws_read_n(fd, buf + total, take) < 0) return -1;
        size_t drop = (size_t)len - take;
        while (drop) { char j[512]; size_t d = drop > sizeof j ? sizeof j : drop;
            if (ws_read_n(fd, j, d) < 0) return -1; drop -= d; }
        if (opcode == 0x8) return -1;                         /* close */
        if (opcode == 0x9) { ws_send(fd, 0xA, NULL, 0); continue; }  /* ping→pong */
        if (opcode == 0xA) continue;                          /* pong */
        total += take;
        if (fin) { buf[total] = 0; return (int)total; }
    }
}
static int ws_connect(void) {
    char host[80], port[8];
    snprintf(host, sizeof host, "%s", HA_HOST);
    char * colon = strrchr(host, ':');
    if (colon) { *colon = 0; snprintf(port, sizeof port, "%s", colon + 1); }
    else snprintf(port, sizeof port, "8123");
    struct addrinfo hints = {0}, * res = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0) { close(fd); return -1; }
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    unsigned char rnd[16];
    FILE * u = fopen("/dev/urandom", "rb");
    if (u) { if (fread(rnd, 1, 16, u) != 16) {} fclose(u); }
    for (int i = 0; i < 16; i++) rnd[i] ^= (unsigned char)(rand() & 0xff);
    char key[28]; ws_b64(rnd, 16, key);
    char req[400];
    int rl = snprintf(req, sizeof req,
        "GET /api/websocket HTTP/1.1\r\nHost: %s:%s\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
        host, port, key);
    if (ws_write_n(fd, req, (size_t)rl) < 0) { close(fd); return -1; }
    char resp[1024]; size_t got = 0;
    while (got < sizeof resp - 1) {
        ssize_t r = recv(fd, resp + got, 1, 0);
        if (r <= 0) { close(fd); return -1; }
        got += (size_t)r; resp[got] = 0;
        if (got >= 4 && !strcmp(resp + got - 4, "\r\n\r\n")) break;
    }
    if (!strstr(resp, " 101")) { close(fd); return -1; }
    return fd;
}

/* Curtain battery (one of the two child sensors) — track both, show the min. */
static int s_bat_a = 100, s_bat_b = 100;
static void apply_curtain_bat(const char * ns, int side) {
    char st[16] = {0};
    if (!extract_str(ns, "state", st, sizeof st)) return;
    int p = atoi(st);
    if (p <= 0) return;
    if (side == 0) s_bat_a = p; else s_bat_b = p;
    ha_state.curtain_battery = s_bat_a < s_bat_b ? s_bat_a : s_bat_b;
}

/* Blinds cover state — mirrors apply_curtain but writes ha_state.blinds_*. */
static void apply_blinds(const char * body) {
    ha_state.connected = 1;
    extract_str(body, "state", ha_state.blinds_state, sizeof(ha_state.blinds_state));
    int v;
    if (extract_int(body, "current_position", &v)) ha_state.blinds_pos = v;
    if (extract_int(body, "is_closed", &v))        ha_state.blinds_is_closed = v;
}

/* Blinds battery (one of the two child sensors). */
static int s_blinds_bat_a = 100, s_blinds_bat_b = 100;
static void apply_blinds_bat(const char * ns, int side) {
    char st[16] = {0};
    if (!extract_str(ns, "state", st, sizeof st)) return;
    int p = atoi(st);
    if (p <= 0) return;
    if (side == 0) s_blinds_bat_a = p; else s_blinds_bat_b = p;
    ha_state.blinds_battery = s_blinds_bat_a < s_blinds_bat_b ? s_blinds_bat_a : s_blinds_bat_b;
}

/* Poll blinds cover state + battery — mirrors poll_once(). */
static void poll_blinds(void) {
    if (!settings.blinds_entity[0]) return;
    char body[1024];
    if (ha_get_state(settings.blinds_entity, body, sizeof(body)) == 0)
        apply_blinds(body);
    int bat_min = 100;
    for (const char * id = settings.blinds_bat_a; ; id = settings.blinds_bat_b) {
        char b[512];
        if (ha_get_state(id, b, sizeof(b)) == 0) {
            char st[16];
            if (extract_str(b, "state", st, sizeof(st))) {
                int p = atoi(st);
                if (p > 0 && p < bat_min) bat_min = p;
            }
        }
        if (strcmp(id, settings.blinds_bat_b) == 0) break;
    }
    ha_state.blinds_battery = bat_min;
}

/* Seed all watched state once over REST (events carry only future changes). */
static void seed_all(void) {
    poll_once();        /* curtain group + battery */
    poll_blinds();      /* blinds cover + battery */
    poll_lights();
    poll_devices();     /* unified device list */
    poll_life360();
    poll_doorbell();    /* arms the trigger edge-detector */
}

/* Route a state_changed event to the right apply_*. Scopes parsing to the
 * event's new_state object. */
static void dispatch_event(const char * msg) {
    char ent[96] = {0};
    extract_str(msg, "entity_id", ent, sizeof ent);   /* data.entity_id (first) */
    if (!ent[0]) return;
    const char * ns = strstr(msg, "\"new_state\"");
    if (!ns) return;
    /* Legacy fixed slots (curtain/blinds state+battery, life360, doorbell).
     * else-if: these are mutually exclusive by entity. No early return — the
     * device loops below still run so a curtain that's also a pinned cover
     * device updates both its tile and its Devices-screen row. */
    if      (CURTAIN_GROUP[0] && !strcmp(ent, CURTAIN_GROUP)) apply_curtain(ns);
    else if (CURTAIN_LEFT[0]  && !strcmp(ent, CURTAIN_LEFT))  apply_curtain_bat(ns, 0);
    else if (CURTAIN_RIGHT[0] && !strcmp(ent, CURTAIN_RIGHT)) apply_curtain_bat(ns, 1);
    else if (settings.blinds_entity[0] && !strcmp(ent, settings.blinds_entity)) apply_blinds(ns);
    else if (settings.blinds_bat_a[0]  && !strcmp(ent, settings.blinds_bat_a))  apply_blinds_bat(ns, 0);
    else if (settings.blinds_bat_b[0]  && !strcmp(ent, settings.blinds_bat_b))  apply_blinds_bat(ns, 1);
    else if (settings.life360_a_entity[0] && !strcmp(ent, settings.life360_a_entity))
        apply_life360(ns, ha_state.loc_a, sizeof ha_state.loc_a, &ha_state.lat_a, &ha_state.lon_a, 0);
    else if (settings.life360_b_entity[0] && !strcmp(ent, settings.life360_b_entity))
        apply_life360(ns, ha_state.loc_b, sizeof ha_state.loc_b, &ha_state.lat_b, &ha_state.lon_b, 1);
    else if (settings.life360_c_entity[0] && !strcmp(ent, settings.life360_c_entity))
        apply_life360(ns, ha_state.loc_c, sizeof ha_state.loc_c, &ha_state.lat_c, &ha_state.lon_c, 2);
    else if (settings.doorbell_entity[0] && !strcmp(ent, settings.doorbell_entity))
        apply_doorbell(ns);
    /* Unified device list (light/cover/switch). */
    for (int i = 0; i < ha_device_count; i++)
        if (!strcmp(ent, ha_devices[i].entity_id)) { apply_device(&ha_devices[i], ns); break; }
    /* Legacy ha_lights array (old Lights screen — kept until migrated). */
    for (int i = 0; i < ha_light_count; i++)
        if (!strcmp(ent, ha_lights[i].entity_id)) { apply_light(&ha_lights[i], ns); break; }
}

/* Doorbell live footage runs on its own thread so it never blocks the event
 * stream: when the overlay is up, stream MJPEG or re-fetch the still ~1x/s. */
static void * doorbell_thread(void * arg) {
    (void)arg;
    while (1) {
        if (ha_state.doorbell_live) {
            if (settings.doorbell_stream_url[0]) stream_doorbell_mjpeg();
            else { if (fetch_doorbell_snapshot() == 0) ha_state.doorbell_frame++; sleep(1); }
        } else {
            sleep(1);
        }
    }
    return NULL;
}

static void * ha_thread(void * arg) {
    (void)arg;
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    static char msg[64 * 1024];
    while (1) {
        int fd = ws_connect();
        if (fd < 0) { ha_state.connected = 0; sleep(8); continue; }
        /* auth_required → auth → auth_ok */
        if (ws_recv_msg(fd, msg, sizeof msg) < 0) { close(fd); sleep(5); continue; }
        char auth[320];
        snprintf(auth, sizeof auth, "{\"type\":\"auth\",\"access_token\":\"%s\"}", g_token);
        if (ws_send(fd, 0x1, (unsigned char *)auth, strlen(auth)) < 0) { close(fd); sleep(5); continue; }
        if (ws_recv_msg(fd, msg, sizeof msg) < 0 || !strstr(msg, "auth_ok")) {
            fprintf(stderr, "[ha] WS auth failed\n"); close(fd); sleep(15); continue;
        }
        seed_all();
        const char * sub = "{\"id\":1,\"type\":\"subscribe_events\",\"event_type\":\"state_changed\"}";
        if (ws_send(fd, 0x1, (unsigned char *)sub, strlen(sub)) < 0) { close(fd); continue; }
        fprintf(stderr, "[ha] WebSocket connected + subscribed to state_changed\n");
        time_t last_ping = time(NULL);
        while (1) {
            fd_set rs; FD_ZERO(&rs); FD_SET(fd, &rs);
            struct timeval tv = { .tv_sec = 20, .tv_usec = 0 };
            int s = select(fd + 1, &rs, NULL, NULL, &tv);
            if (s < 0) { if (errno == EINTR) continue; break; }
            if (s == 0) { if (ws_send(fd, 0x9, NULL, 0) < 0) break; last_ping = time(NULL); continue; }
            int n = ws_recv_msg(fd, msg, sizeof msg);
            if (n < 0) break;
            if (strstr(msg, "state_changed")) dispatch_event(msg);
            if (time(NULL) - last_ping > 40) { if (ws_send(fd, 0x9, NULL, 0) < 0) break; last_ping = time(NULL); }
        }
        close(fd);
        ha_state.connected = 0;
        sleep(3);
    }
    return NULL;
}

static void * cover_action_thread(void * arg) {
    char * action = (char *)arg;
    ha_call_cover_service_only(action);
    /* Speed up the next poll so the tile reflects the new state quickly
     * instead of waiting up to HA_POLL_S seconds. */
    poll_once();
    free(action);
    return NULL;
}

static void fire_cover_action(const char * action) {
    pthread_t t;
    char * dup = strdup(action);
    if (!dup) return;
    if (pthread_create(&t, NULL, cover_action_thread, dup) != 0) {
        free(dup);
        return;
    }
    pthread_detach(t);
}

void ha_curtain_open_async(void)  { fire_cover_action("open_cover");  }
void ha_curtain_close_async(void) { fire_cover_action("close_cover"); }
void ha_curtain_stop_async(void)  { fire_cover_action("stop_cover");  }

/* --- brightness setter --------------------------------------------------- */
struct brightness_arg { char entity_id[48]; int pct; };
static void * brightness_action_thread(void * arg) {
    struct brightness_arg * a = (struct brightness_arg *)arg;
    if (a->pct <= 0)
        ha_call_service("light", "turn_off", a->entity_id, NULL);
    else {
        char extra[32];
        snprintf(extra, sizeof(extra), ",\"brightness_pct\":%d", a->pct);
        ha_call_service("light", "turn_on", a->entity_id, extra);
    }
    poll_lights();
    free(a);
    return NULL;
}

void ha_light_set_brightness_async(const char * entity_id, int brightness_pct) {
    struct brightness_arg * a = malloc(sizeof *a);
    if (!a) return;
    snprintf(a->entity_id, sizeof a->entity_id, "%s", entity_id);
    a->pct = brightness_pct;
    pthread_t t;
    if (pthread_create(&t, NULL, brightness_action_thread, a) != 0) { free(a); return; }
    pthread_detach(t);
}

/* --- cover position setter ----------------------------------------------- */
struct cover_pos_arg { char entity_id[64]; int pos; };
static void * cover_position_thread(void * arg) {
    struct cover_pos_arg * a = (struct cover_pos_arg *)arg;
    char extra[32];
    snprintf(extra, sizeof(extra), ",\"position\":%d", a->pos);
    ha_call_service("cover", "set_cover_position", a->entity_id, extra);
    poll_once();
    free(a);
    return NULL;
}

void ha_cover_set_position_async(const char * entity_id, int position_pct) {
    struct cover_pos_arg * a = malloc(sizeof *a);
    if (!a) return;
    snprintf(a->entity_id, sizeof a->entity_id, "%s", entity_id);
    a->pos = position_pct;
    pthread_t t;
    if (pthread_create(&t, NULL, cover_position_thread, a) != 0) { free(a); return; }
    pthread_detach(t);
}

/* Generic cover stop — same fire-and-forget pattern as the curtain stop
 * but takes an entity_id so blinds can use it too. */
struct cover_stop_arg { char entity_id[64]; };
static void * cover_stop_thread(void * arg) {
    struct cover_stop_arg * a = (struct cover_stop_arg *)arg;
    ha_call_service("cover", "stop_cover", a->entity_id, NULL);
    poll_once();
    free(a);
    return NULL;
}
void ha_cover_stop_async(const char * entity_id) {
    struct cover_stop_arg * a = malloc(sizeof *a);
    if (!a) return;
    snprintf(a->entity_id, sizeof a->entity_id, "%s", entity_id);
    pthread_t t;
    if (pthread_create(&t, NULL, cover_stop_thread, a) != 0) { free(a); return; }
    pthread_detach(t);
}

/* --- entity discovery --------------------------------------------------- */
int ha_discover_entities(const char * domain_prefix,
                          ha_discovered_t * out, int * count, int max) {
    *count = 0;
    if (!g_token[0] || !domain_prefix || !domain_prefix[0]) return -1;
    if (!HA_HOST[0]) return -1;

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s --max-time 10 --connect-timeout 4 "
        "-H 'Authorization: Bearer %s' "
        "'http://%s/api/states' 2>/dev/null",
        g_token, HA_HOST);
    FILE * p = popen(cmd, "r");
    if (!p) return -1;

    /* Read the full JSON array. HA instances can have hundreds of entities so
     * we need a large buffer — but still cap it to keep the stack safe. */
    static char body[131072];    /* 128 KB — enough for ~500 entities */
    size_t n = fread(body, 1, sizeof(body) - 1, p);
    pclose(p);
    if (n == 0) return -1;
    body[n] = 0;

    /* Build a match needle: "entity_id":"<prefix>." — the dot guarantees we
     * match e.g. "cover.living" but NOT "cover_long_entity". */
    char needle[80];
    int nlen = snprintf(needle, sizeof(needle), "\"entity_id\":\"%s.", domain_prefix);
    size_t ndlen = strlen(needle);

    int found = 0;
    const char * pos = body;
    while (found < max) {
        const char * hit = strstr(pos, needle);
        if (!hit) break;
        hit += ndlen;   /* skip to start of entity_id value (after the dot) */
        pos = hit;

        /* Extract entity_id — it's the full "domain_prefix.entity_name" */
        char ent[64];
        snprintf(ent, sizeof(ent), "%s.", domain_prefix);
        size_t elen = strlen(ent);
        const char * ep = hit;
        while (*ep && *ep != '"' && elen < sizeof(ent) - 1)
            ent[elen++] = *ep++;
        ent[elen] = 0;

        /* Walk forward to find "friendly_name":" */
        char fn[64] = "";
        const char * fnp = strstr(pos, "\"friendly_name\":\"");
        if (fnp) {
            fnp += 17;   /* skip "friendly_name":" */
            size_t fni = 0;
            while (*fnp && *fnp != '"' && fni < sizeof(fn) - 1)
                fn[fni++] = *fnp++;
            fn[fni] = 0;
        }

        if (ent[0] && ha_id_safe(ent)) {
            snprintf(out[found].entity_id, sizeof(out[found].entity_id), "%s", ent);
            if (fn[0])
                snprintf(out[found].friendly_name, sizeof(out[found].friendly_name),
                         "%s", fn);
            else
                snprintf(out[found].friendly_name, sizeof(out[found].friendly_name),
                         "%s", ent);
            found++;
        }
    }
    *count = found;
    return (found > 0) ? 0 : -1;
}

int ha_start(void) {
    if (!settings.enable_ha) {
        fprintf(stderr, "[ha] integration disabled — not starting poller\n");
        return 0;
    }
    if (!settings.ha_host[0]) {
        fprintf(stderr, "[ha] no host configured — not starting poller\n");
        return 0;
    }
    load_token();
    ha_lights_load();
    ha_devices_load();
    ha_trackers_load();   /* seed last-known fixes before the first poll/WS update */
    if (!g_token[0]) {
        fprintf(stderr, "[ha] no token at " HA_TOKEN_PATH " — HA tile will stay disconnected\n");
    }
    pthread_t t;
    pthread_create(&t, NULL, ha_thread, NULL);
    pthread_detach(t);
    /* Doorbell live-footage runs separately so it never blocks the WS stream. */
    if (settings.doorbell_entity[0] || settings.doorbell_camera[0]) {
        pthread_t d;
        if (pthread_create(&d, NULL, doorbell_thread, NULL) == 0) pthread_detach(d);
    }
    fprintf(stderr, "[ha] WebSocket client started (host=%s)\n", HA_HOST);
    return 0;
}
