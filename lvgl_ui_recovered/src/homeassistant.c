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
#include "domoticz.h"      /* unified Domoticz devices: DZ_* + control + state */
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

/* Parse a JSON "options":["A","B","C"] array into a pipe-delimited string.
 * Example output: "Open|Peek|Close". Truncates to outsz-1. */
static void extract_options(const char * json, char * out, size_t outsz) {
    out[0] = 0;
    const char * p = strstr(json, "\"options\":[");
    if (!p) return;
    p += 10;
    size_t olen = 0;
    while (*p && *p != ']' && olen < outsz - 1) {
        p = strchr(p, '"');
        if (!p) break;
        p++;
        const char * e = strchr(p, '"');
        if (!e) break;
        size_t n = e - p;
        if (n > outsz - olen - 2) n = outsz - olen - 2;
        if (olen > 0) out[olen++] = '|';
        memcpy(out + olen, p, n);
        olen += n;
        p = e + 1;
    }
    out[olen] = 0;
}

/* ---- HA read path: a WebSocket get_states snapshot ----
 * One ws_get_states() round-trip fills ha_snapshot with the full states array
 * (same entity objects REST /api/states returns). ha_get_state() then extracts
 * a single entity's object from it — no curl, no per-entity request. The
 * persistent ha_thread keeps the snapshot fresh from live state_changed events;
 * ha_snapshot_refresh() re-pulls the whole set at seed/periodic/after-action. */
static char ha_snapshot[768 * 1024];
static int  ha_snapshot_valid = 0;
static pthread_mutex_t ha_snap_lock = PTHREAD_MUTEX_INITIALIZER;
static void ha_snapshot_refresh(void);   /* defined after the WS helpers below */

/* Copy the states[] element whose entity_id == id into out. entity_id is the
 * first key of each element, so the '{' just before it opens that object; we
 * then brace-match (string-aware) to its close. 0 on success, -1 if absent. */
static int snap_extract_entity(const char * snap, const char * id,
                               char * out, size_t out_max) {
    char needle[96];
    int nl = snprintf(needle, sizeof needle, "\"entity_id\":\"%s\"", id);
    if (nl <= 0 || nl >= (int)sizeof needle) return -1;
    const char * p = strstr(snap, needle);
    if (!p) return -1;
    const char * start = p;
    while (start > snap && *start != '{') start--;
    if (*start != '{') return -1;
    int depth = 0, instr = 0, esc = 0;
    const char * q = start;
    for (; *q; q++) {
        char c = *q;
        if (instr) {
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') instr = 0;
        } else if (c == '"') instr = 1;
        else if (c == '{') depth++;
        else if (c == '}') { if (--depth == 0) { q++; break; } }
    }
    size_t len = (size_t)(q - start);
    if (len >= out_max) len = out_max - 1;
    memcpy(out, start, len);
    out[len] = 0;
    return 0;
}

/* Extract one entity's state object from the latest WS snapshot. Drop-in for
 * the old per-entity REST GET — same body shape, so every parser is unchanged. */
static int ha_get_state(const char * entity_id, char * out, size_t out_max) {
    if (!ha_id_safe(entity_id)) return -1;
    int rc;
    pthread_mutex_lock(&ha_snap_lock);
    rc = ha_snapshot_valid ? snap_extract_entity(ha_snapshot, entity_id, out, out_max) : -1;
    pthread_mutex_unlock(&ha_snap_lock);
    return rc;
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

/* Call an HA service over the WebSocket (one-shot connect+auth+call_service).
 * extra_json = ",\"brightness_pct\":50" (leading comma) or NULL. No curl. */
static int ws_call_service(const char * domain, const char * service,
                           const char * entity, const char * extra_json);
static int ha_call_service(const char * domain, const char * service,
                           const char * entity_id, const char * extra_json) {
    if (!ha_id_safe(entity_id)) return -1;
    int rc = ws_call_service(domain, service, entity_id, extra_json);
    if (rc != 0) notify_show("system", "ha_offline", "HA niet bereikbaar — actie niet uitgevoerd");
    else         notify_clear("system", "ha_offline");
    return rc;
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
    ha_snapshot_refresh(); poll_lights();   /* authoritative read; event backstops */
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
        case HADEV_SELECT: return "input_select";
        case HADEV_SCRIPT: return "script";
        case HADEV_SCENE:  return "scene";
        default:           return "light";
    }
}
int hadev_type_from_str(const char * s) {
    if (!s)                    return HADEV_LIGHT;
    if (!strcmp(s, "cover"))   return HADEV_COVER;
    if (!strcmp(s, "switch"))  return HADEV_SWITCH;
    if (!strcmp(s, "input_select")) return HADEV_SELECT;
    if (!strcmp(s, "script"))  return HADEV_SCRIPT;
    if (!strcmp(s, "scene"))   return HADEV_SCENE;
    return HADEV_LIGHT;
}

void ha_devices_save(void) {
    FILE * f = fopen(HA_DEVICES_CONF, "w");
    if (!f) return;
    fprintf(f, "# type|entity_id|Name|pin|backend   "
               "(type=light|cover|switch|input_select|script|scene; backend 0=HA 1=Domoticz, entity 'dz:<idx>')\n");
    for (int i = 0; i < ha_device_count; i++) {
        ha_device_t * D = &ha_devices[i];
        fprintf(f, "%s|%s|%s|%d|%d\n", hadev_type_str(D->type),
                D->entity_id, D->name, D->pin_home ? 1 : 0, D->backend);
    }
    fclose(f);
}

/* backend = HADEV_BE_HA | HADEV_BE_DZ. For Domoticz, `entity` is "dz:<idx>"
 * and ha_id_safe() is skipped (the idx isn't an HA entity id). */
static void devices_add_be(int type, const char * entity, const char * name,
                           int pin, int backend) {
    if (ha_device_count >= HA_DEVICE_MAX || !entity || !entity[0]) return;
    if (backend == HADEV_BE_HA && !ha_id_safe(entity)) return;
    ha_device_t * D = &ha_devices[ha_device_count++];
    memset((void *)D, 0, sizeof *D);
    D->type = type;
    D->backend = backend;
    snprintf(D->entity_id, sizeof D->entity_id, "%s", entity);
    snprintf(D->name, sizeof D->name, "%s", (name && name[0]) ? name : entity);
    D->pin_home   = pin ? 1 : 0;
    D->brightness = -1;
    D->position   = -1;
    if (backend == HADEV_BE_DZ) {
        const char * c = strchr(entity, ':');
        D->dz_idx = c ? atoi(c + 1) : 0;
    }
}
static void devices_add(int type, const char * entity, const char * name, int pin) {
    devices_add_be(type, entity, name, pin, HADEV_BE_HA);
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
            /* type | entity_id | name | pin | backend(optional) */
            char * p_ent = strchr(line, '|'); if (!p_ent) continue; *p_ent++ = 0;
            const char * name = ""; int pin = 0, backend = HADEV_BE_HA;
            char * p_name = strchr(p_ent, '|');
            if (p_name) {
                *p_name++ = 0;
                char * p_pin = strchr(p_name, '|');
                if (p_pin) {
                    *p_pin++ = 0;
                    char * p_be = strchr(p_pin, '|');
                    if (p_be) { *p_be++ = 0; backend = atoi(p_be); }
                    pin = atoi(p_pin);
                }
                name = p_name;
            }
            devices_add_be(hadev_type_from_str(line), p_ent, name, pin, backend);
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
    } else if (D->type == HADEV_SELECT) {
        snprintf(D->state, sizeof D->state, "%s", st[0] ? st : "?");
        extract_options(body, D->options, sizeof D->options);
        D->available = 1;
    } else {                          /* light / switch */
        if (!strcmp(st, "on"))        { D->available = 1; D->on = 1; }
        else if (!strcmp(st, "off"))  { D->available = 1; D->on = 0; }
        else                          { D->available = 0; D->on = 0; }
        int v;
        if (extract_int(body, "brightness", &v)) D->brightness = v;
    }
}

/* Refresh every stateful device from the WS snapshot. Scripts/scenes are
 * stateless and skipped. */
static void poll_devices(void) {
    char body[768];
    for (int i = 0; i < ha_device_count; i++) {
        ha_device_t * D = &ha_devices[i];
        if (D->backend == HADEV_BE_DZ) continue;   /* Domoticz: synced separately */
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
    ha_snapshot_refresh(); poll_devices();
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

/* input_select.select_option — extra JSON carries the option value.
 * Re-polls devices so the UI reflects the new selection quickly. */
struct select_opt_arg { char entity[64]; char option[64]; };
static void * select_option_thread(void * arg) {
    struct select_opt_arg * a = (struct select_opt_arg *)arg;
    char extra[96];
    snprintf(extra, sizeof(extra), ",\"option\":\"%s\"", a->option);
    ha_call_service("input_select", "select_option", a->entity, extra);
    ha_snapshot_refresh(); poll_devices();
    free(a);
    return NULL;
}
void ha_device_select_option_async(const char * entity_id, const char * option) {
    struct select_opt_arg * a = malloc(sizeof *a);
    if (!a) return;
    snprintf(a->entity, sizeof a->entity, "%s", entity_id);
    snprintf(a->option, sizeof a->option, "%s", option);
    pthread_t t;
    if (pthread_create(&t, NULL, select_option_thread, a) != 0) { free(a); return; }
    pthread_detach(t);
}

/* Copy the n-th '|'-delimited token of s into out. */
static void pipe_token(const char * s, int n, char * out, size_t osz) {
    out[0] = 0;
    int k = 0; const char * tok = s;
    for (const char * p = s; ; p++) {
        if (*p == '|' || *p == 0) {
            if (k == n) {
                size_t len = (size_t)(p - tok);
                if (len >= osz) len = osz - 1;
                memcpy(out, tok, len); out[len] = 0; return;
            }
            if (*p == 0) return;
            k++; tok = p + 1;
        }
    }
}
/* Index of the token equal to `name`, or -1. */
static int pipe_index(const char * s, const char * name) {
    size_t nl = strlen(name);
    int k = 0; const char * tok = s;
    for (const char * p = s; ; p++) {
        if (*p == '|' || *p == 0) {
            if ((size_t)(p - tok) == nl && strncmp(tok, name, nl) == 0) return k;
            if (*p == 0) return -1;
            k++; tok = p + 1;
        }
    }
}

/* ---- backend-aware control by device index (HA WebSocket or Domoticz idx) ---- */
void ha_dev_toggle(int idx) {
    if (idx < 0 || idx >= ha_device_count) return;
    ha_device_t * D = &ha_devices[idx];
    if (D->backend == HADEV_BE_DZ) domoticz_switch_async(D->dz_idx, "Toggle");
    else                           ha_device_toggle_async(D->type, D->entity_id);
}
void ha_dev_cover(int idx, const char * cmd) {
    if (idx < 0 || idx >= ha_device_count) return;
    ha_device_t * D = &ha_devices[idx];
    if (D->backend == HADEV_BE_DZ) {
        const char * dz = !strcmp(cmd, "open") ? "Open"
                        : !strcmp(cmd, "close") ? "Close" : "Stop";
        domoticz_switch_async(D->dz_idx, dz);
    } else ha_device_cover_async(D->entity_id, cmd);
}
void ha_dev_position(int idx, int pct) {
    if (idx < 0 || idx >= ha_device_count) return;
    ha_device_t * D = &ha_devices[idx];
    if (D->backend == HADEV_BE_DZ) domoticz_set_level_async(D->dz_idx, pct);
    else                           ha_cover_set_position_async(D->entity_id, pct);
}
void ha_dev_brightness(int idx, int pct) {
    if (idx < 0 || idx >= ha_device_count) return;
    ha_device_t * D = &ha_devices[idx];
    if (D->backend == HADEV_BE_DZ) domoticz_set_level_async(D->dz_idx, pct);
    else                           ha_light_set_brightness_async(D->entity_id, pct);
}
void ha_dev_run(int idx) {
    if (idx < 0 || idx >= ha_device_count) return;
    ha_device_t * D = &ha_devices[idx];
    if (D->backend == HADEV_BE_DZ) domoticz_switch_async(D->dz_idx, "On");  /* Domoticz has no scripts */
    else                           ha_device_run_async(D->type, D->entity_id);
}
void ha_dev_select_set(int idx, const char * option) {
    if (idx < 0 || idx >= ha_device_count || !option) return;
    ha_device_t * D = &ha_devices[idx];
    if (D->backend == HADEV_BE_DZ) {
        int oi = pipe_index(D->options, option);   /* Domoticz selector level = idx*10 */
        if (oi >= 0) domoticz_set_level_async(D->dz_idx, oi * 10);
    } else {
        ha_device_select_option_async(D->entity_id, option);
    }
}

/* Mirror live Domoticz state into the backend==DZ entries of ha_devices[]. */
void ha_devices_sync_dz(void) {
    for (int i = 0; i < ha_device_count; i++) {
        ha_device_t * D = &ha_devices[i];
        if (D->backend != HADEV_BE_DZ) continue;
        D->available = 0;
        for (int j = 0; j < domoticz_state.count; j++) {
            domoticz_dev_t * z = &domoticz_state.dev[j];
            if (z->idx != D->dz_idx) continue;
            D->available = 1;
            D->on = z->on;
            if (D->type == HADEV_COVER) {
                D->position = (z->level >= 0) ? z->level : (z->on ? 100 : 0);
                snprintf(D->state, sizeof D->state, "%s", z->on ? "open" : "closed");
            } else if (D->type == HADEV_LIGHT) {
                D->brightness = (z->level >= 0) ? z->level * 255 / 100 : (z->on ? 255 : 0);
            } else if (D->type == HADEV_SELECT) {
                snprintf(D->options, sizeof D->options, "%s", z->options);
                pipe_token(z->options, (z->level > 0) ? z->level / 10 : 0,
                           D->state, sizeof D->state);
            }
            break;
        }
    }
}

/* ---- list editing (settings device manager) ---- */
int ha_device_add(int type, const char * entity_id, const char * name, int pin) {
    int before = ha_device_count;
    devices_add(type, entity_id, name, pin);
    if (ha_device_count == before) return -1;   /* rejected: full / unsafe id */
    ha_devices_save();
    /* Poll the new device immediately so it shows state right away instead
     * of waiting for the next WS event or reconnect. */
    char body[768];
    int idx = ha_device_count - 1;
    ha_snapshot_refresh();
    if (ha_get_state(entity_id, body, sizeof body) == 0)
        apply_device(&ha_devices[idx], body);
    return idx;
}
/* Add a Domoticz device to the managed list. kind = DZ_SWITCH/DZ_DIMMER/DZ_BLIND. */
int ha_device_add_dz(int dz_kind, int dz_idx, const char * name, int pin) {
    int type = (dz_kind == DZ_BLIND)    ? HADEV_COVER
             : (dz_kind == DZ_DIMMER)   ? HADEV_LIGHT
             : (dz_kind == DZ_SELECTOR) ? HADEV_SELECT : HADEV_SWITCH;
    char entity[64];
    snprintf(entity, sizeof entity, "dz:%d", dz_idx);
    int before = ha_device_count;
    devices_add_be(type, entity, name, pin, HADEV_BE_DZ);
    if (ha_device_count == before) return -1;        /* list full / duplicate */
    ha_devices_save();
    ha_devices_sync_dz();                            /* show its state right away */
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
static char geo_key[2][32];
static char geo_city[2][48];
static void reverse_city(double lat, double lon, int person, char * out, size_t osz) {
    out[0] = 0;
    if (person < 0 || person > 1) return;
    char key[32];
    snprintf(key, sizeof key, "%.3f,%.3f", lat, lon);   /* ~100m bucket */
    if (strcmp(key, geo_key[person]) == 0) { snprintf(out, osz, "%s", geo_city[person]); return; }
    char cmd[400];
    snprintf(cmd, sizeof cmd,
        "/usr/bin/curl -fsSL -k --max-time 8 -A 'freetoon-lvgl/1.0' "
        "'https://nominatim.openstreetmap.org/reverse?lat=%.5f&lon=%.5f"
        "&format=json&zoom=14&addressdetails=1'", lat, lon);
    FILE * fp = popen(cmd, "r");
    if (!fp) return;
    static char body[4096];
    size_t n = fread(body, 1, sizeof body - 1, fp);
    pclose(fp);
    body[n] = 0;
    /* Nominatim address sub-object: prefer city, then town/village/municipality. */
    const char * keys[] = { "city", "town", "village", "municipality", "suburb", NULL };
    for (int i = 0; keys[i]; i++)
        if (extract_str(body, keys[i], out, osz) && out[0]) break;
    snprintf(geo_key[person], sizeof geo_key[person], "%s", key);
    snprintf(geo_city[person], sizeof geo_city[person], "%s", out);
}

/* Parse a life360 state object (REST /api/states body or a WS new_state) into
 * the location string + coords. */
static void apply_life360(const char * body, char * out, size_t outsz,
                          volatile float * lat, volatile float * lon, int person) {
    if (lat && lon) {
        double dlat, dlon;
        if (extract_double(body, "latitude", &dlat) &&
            extract_double(body, "longitude", &dlon)) {
            *lat = (float)dlat; *lon = (float)dlon;
        }
    }
    char state[24] = {0};
    extract_str(body, "state", state, sizeof(state));
    if (strcmp(state, "home") == 0) {
        snprintf(out, outsz, "home");
        return;
    }
    char addr[160] = {0};
    extract_str(body, "address", addr, sizeof(addr));
    if (!addr[0]) {
        if (state[0]) snprintf(out, outsz, "%s", state);
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
    char gcity[48] = {0};
    if (lat && lon && (*lat != 0.0f || *lon != 0.0f))
        reverse_city(*lat, *lon, person, gcity, sizeof gcity);
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

/* ---- WebSocket request helpers (one-shot: connect+auth, do op, close) ----
 * The persistent ha_thread WS carries live state_changed events; these
 * short-lived connections do the request/response ops (get_states for the
 * picker, call_service for control) so HA needs no curl and no MQTT. */
static int ws_open_authed(void) {
    if (!g_token[0]) return -1;
    int fd = ws_connect();
    if (fd < 0) return -1;
    char m[2048];
    if (ws_recv_msg(fd, m, sizeof m) < 0) { close(fd); return -1; }   /* auth_required */
    char auth[320];
    snprintf(auth, sizeof auth, "{\"type\":\"auth\",\"access_token\":\"%s\"}", g_token);
    if (ws_send(fd, 0x1, (const unsigned char *)auth, strlen(auth)) < 0) { close(fd); return -1; }
    if (ws_recv_msg(fd, m, sizeof m) < 0 || !strstr(m, "auth_ok")) { close(fd); return -1; }
    return fd;
}
/* One-shot get_states -> raw JSON result into buf (same entity objects REST
 * returns, wrapped in {"result":[...]}). 0 on success. */
static int ws_get_states(char * buf, size_t bufsz) {
    int fd = ws_open_authed();
    if (fd < 0) return -1;
    const char * req = "{\"id\":1,\"type\":\"get_states\"}";
    int ok = -1;
    if (ws_send(fd, 0x1, (const unsigned char *)req, strlen(req)) == 0) {
        for (int t = 0; t < 5; t++) {
            int n = ws_recv_msg(fd, buf, bufsz);
            if (n < 0) break;
            if (strstr(buf, "\"type\":\"result\"")) { ok = 0; break; }
        }
    }
    ws_send(fd, 0x8, NULL, 0);
    close(fd);
    return ok;
}
/* One-shot call_service. extra_json is ",\"k\":v" (leading comma) or NULL. */
static int ws_call_service(const char * domain, const char * service,
                           const char * entity, const char * extra_json) {
    int fd = ws_open_authed();
    if (fd < 0) return -1;
    char req[640];
    if (extra_json && extra_json[0])
        snprintf(req, sizeof req,
            "{\"id\":1,\"type\":\"call_service\",\"domain\":\"%s\",\"service\":\"%s\","
            "\"service_data\":{%s},\"target\":{\"entity_id\":\"%s\"}}",
            domain, service, extra_json + 1, entity);
    else
        snprintf(req, sizeof req,
            "{\"id\":1,\"type\":\"call_service\",\"domain\":\"%s\",\"service\":\"%s\","
            "\"target\":{\"entity_id\":\"%s\"}}",
            domain, service, entity);
    int rc = ws_send(fd, 0x1, (const unsigned char *)req, strlen(req));
    char m[1024]; ws_recv_msg(fd, m, sizeof m);    /* best-effort result ack */
    ws_send(fd, 0x8, NULL, 0);
    close(fd);
    return rc;
}
/* Re-pull the whole states set into ha_snapshot (one WS round-trip). refresh_lock
 * serializes refreshers (so the static tmp is single-writer); the brief memcpy
 * under ha_snap_lock is all that blocks ha_get_state() readers. */
static void ha_snapshot_refresh(void) {
    static char tmp[768 * 1024];
    static pthread_mutex_t refresh_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&refresh_lock);
    if (ws_get_states(tmp, sizeof tmp) == 0) {
        pthread_mutex_lock(&ha_snap_lock);
        memcpy(ha_snapshot, tmp, sizeof ha_snapshot);
        ha_snapshot_valid = 1;
        pthread_mutex_unlock(&ha_snap_lock);
    }
    pthread_mutex_unlock(&refresh_lock);
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

/* ---- HA energy sensor polling -------------------------------------------
 * Periodically curls each configured energy sensor entity and parses its
 * numeric state. Rate-limited internally to HA_POLL_S seconds; called from
 * the WebSocket event loop so it piggybacks on the existing connection rhythm
 * without needing its own timer thread. */
ha_energy_state_t ha_energy = {0};

#define HA_GAS_RING_N 64
static struct { long t; float m3; } ha_gas_ring[HA_GAS_RING_N];
static int  ha_gas_ring_head = 0, ha_gas_ring_count = 0;
static long ha_gas_ring_last_t = 0;

static void ha_gas_hour_update(float gas_now) {
    if (gas_now <= 0) { ha_energy.gas_hour_m3 = 0; return; }
    long now = time(NULL);
    if (ha_gas_ring_last_t == 0 || now - ha_gas_ring_last_t >= 55) {
        ha_gas_ring[ha_gas_ring_head].t  = now;
        ha_gas_ring[ha_gas_ring_head].m3 = gas_now;
        ha_gas_ring_head = (ha_gas_ring_head + 1) % HA_GAS_RING_N;
        if (ha_gas_ring_count < HA_GAS_RING_N) ha_gas_ring_count++;
        ha_gas_ring_last_t = now;
    }
    if (ha_gas_ring_count < 2) { ha_energy.gas_hour_m3 = 0; return; }
    long midnight = now - (now % 86400);
    long cutoff = now - 3600;
    if (cutoff < midnight) cutoff = midnight;  /* daily reset — don't cross midnight */
    int oldest = (ha_gas_ring_head - ha_gas_ring_count + HA_GAS_RING_N) % HA_GAS_RING_N;
    int ref = oldest;
    for (int k = 0; k < ha_gas_ring_count; k++) {
        int i = (oldest + k) % HA_GAS_RING_N;
        if (ha_gas_ring[i].t <= cutoff) ref = i; else break;
    }
    float delta = gas_now - ha_gas_ring[ref].m3;
    if (delta < 0) delta = 0;
    ha_energy.gas_hour_m3 = delta;
}

static float ha_parse_state_float(const char * json) {
    const char * p = strstr(json, "\"state\"");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        if (!strncmp(p, "unavailable", 11)) return -1;
        if (!strncmp(p, "unknown", 7)) return -1;
    }
    return strtof(p, NULL);
}

static void poll_ha_energy(void) {
    static time_t last_s = 0;
    time_t now = time(NULL);
    if (now - last_s < HA_POLL_S) return;
    last_s = now;

    int any = 0;
    char body[1024];

    if (settings.energy_elec_source == ENERGY_SRC_HA && settings.energy_elec_ha_entity[0]) {
        if (ha_get_state(settings.energy_elec_ha_entity, body, sizeof body) == 0) {
            float v = ha_parse_state_float(body);
            if (v >= 0) { ha_energy.power_w = v; any = 1; }
        }
    }
    if (settings.energy_elec_source == ENERGY_SRC_HA && settings.energy_elec_prod_ha_entity[0]) {
        if (ha_get_state(settings.energy_elec_prod_ha_entity, body, sizeof body) == 0) {
            float v = ha_parse_state_float(body);
            if (v >= 0) { ha_energy.power_prod_w = v; any = 1; }
        }
    }
    if (settings.energy_gas_source == ENERGY_SRC_HA && settings.energy_gas_ha_entity[0]) {
        if (ha_get_state(settings.energy_gas_ha_entity, body, sizeof body) == 0) {
            float v = ha_parse_state_float(body);
            if (v >= 0) { ha_energy.gas_m3 = v; ha_gas_hour_update(v); any = 1; }
        }
    }
    if (settings.energy_water_source == ENERGY_SRC_HA && settings.energy_water_ha_entity[0]) {
        if (ha_get_state(settings.energy_water_ha_entity, body, sizeof body) == 0) {
            float v = ha_parse_state_float(body);
            if (v >= 0) { ha_energy.water_m3 = v; any = 1; }
        }
    }

    ha_energy.connected = any ? 1 : 0;
}

/* Seed all watched state once over REST (events carry only future changes). */
static void seed_all(void) {
    ha_snapshot_refresh();   /* one WS get_states; the pollers extract from it */
    poll_once();        /* curtain group + battery */
    poll_blinds();      /* blinds cover + battery */
    poll_lights();
    poll_devices();     /* unified device list */
    poll_life360();
    poll_doorbell();    /* arms the trigger edge-detector */
    poll_ha_energy();   /* seed the energy sensors from the snapshot */
}

/* Route an energy sensor's state object (a get_states element or a WS
 * new_state) into ha_energy, so energy tracks live state_changed events
 * instead of being polled. ent is the entity id, json its state object. */
static void route_energy(const char * ent, const char * json) {
    int matched = 0;
    if (settings.energy_elec_source == ENERGY_SRC_HA && settings.energy_elec_ha_entity[0]
        && !strcmp(ent, settings.energy_elec_ha_entity)) {
        float v = ha_parse_state_float(json); if (v >= 0) ha_energy.power_w = v; matched = 1;
    } else if (settings.energy_elec_source == ENERGY_SRC_HA && settings.energy_elec_prod_ha_entity[0]
        && !strcmp(ent, settings.energy_elec_prod_ha_entity)) {
        float v = ha_parse_state_float(json); if (v >= 0) ha_energy.power_prod_w = v; matched = 1;
    } else if (settings.energy_gas_source == ENERGY_SRC_HA && settings.energy_gas_ha_entity[0]
        && !strcmp(ent, settings.energy_gas_ha_entity)) {
        float v = ha_parse_state_float(json); if (v >= 0) { ha_energy.gas_m3 = v; ha_gas_hour_update(v); } matched = 1;
    } else if (settings.energy_water_source == ENERGY_SRC_HA && settings.energy_water_ha_entity[0]
        && !strcmp(ent, settings.energy_water_ha_entity)) {
        float v = ha_parse_state_float(json); if (v >= 0) ha_energy.water_m3 = v; matched = 1;
    }
    if (matched) ha_energy.connected = 1;
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
    else if (settings.doorbell_entity[0] && !strcmp(ent, settings.doorbell_entity))
        apply_doorbell(ns);
    /* Unified device list (light/cover/switch). */
    for (int i = 0; i < ha_device_count; i++)
        if (!strcmp(ent, ha_devices[i].entity_id)) { apply_device(&ha_devices[i], ns); break; }
    /* Legacy ha_lights array (old Lights screen — kept until migrated). */
    for (int i = 0; i < ha_light_count; i++)
        if (!strcmp(ent, ha_lights[i].entity_id)) { apply_light(&ha_lights[i], ns); break; }
    /* Energy sensors (P1 power/gas/water) — live, no separate poll. */
    route_energy(ent, ns);
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
        /* No token → nothing the WS can do; wait for the user to set one. */
        if (!g_token[0]) { sleep(15); continue; }
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
        time_t last_ping = time(NULL), last_reseed = time(NULL);
        while (1) {
            fd_set rs; FD_ZERO(&rs); FD_SET(fd, &rs);
            struct timeval tv = { .tv_sec = 20, .tv_usec = 0 };
            int s = select(fd + 1, &rs, NULL, NULL, &tv);
            if (s < 0) { if (errno == EINTR) continue; break; }
            if (s > 0) {
                int n = ws_recv_msg(fd, msg, sizeof msg);
                if (n < 0) break;
                if (strstr(msg, "state_changed")) dispatch_event(msg);
            }
            time_t now = time(NULL);
            if (now - last_ping > 40) { if (ws_send(fd, 0x9, NULL, 0) < 0) break; last_ping = now; }
            /* Belt-and-suspenders: a full re-pull every 60s recovers anything a
             * missed event left stale (the snapshot drives all the pollers). */
            if (now - last_reseed > 60) { seed_all(); last_reseed = now; }
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
    ha_snapshot_refresh(); poll_once();
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
    ha_snapshot_refresh(); poll_lights();
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
    ha_snapshot_refresh(); poll_once();
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
    ha_snapshot_refresh(); poll_once();
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
    if (!domain_prefix || !domain_prefix[0]) return -1;

    /* HA-over-WebSocket: get_states returns the same entity objects as REST
     * /api/states (wrapped in {"result":[...]}), so the parser below works
     * unchanged — one WS round-trip, no curl, no token on a command line.
     * 2 MB so a large install's full state isn't truncated before the tail
     * entities (e.g. a freshly-added P1 integration) are parsed. */
    static char body[2 * 1024 * 1024];
    if (ws_get_states(body, sizeof body) != 0) {
        fprintf(stderr, "[ha] ws discover: get_states failed (token set? HA reachable?)\n");
        return -1;
    }
    fprintf(stderr, "[ha] ws discover: get_states %zu bytes, domain '%s'\n",
            strlen(body), domain_prefix);

    /* … search loop as before … */
    char id_key[32];
    snprintf(id_key, sizeof id_key, "\"entity_id\"");
    size_t id_key_len = strlen(id_key);

    int found = 0;
    const char * pos = body;
    while (found < max) {
        const char * hit = strstr(pos, id_key);
        if (!hit) break;
        pos = hit + id_key_len;

        /* Skip colon + optional whitespace + opening quote */
        const char * p2 = pos;
        while (*p2 == ' ' || *p2 == '\t' || *p2 == '\n' || *p2 == '\r') p2++;
        if (*p2 != ':') continue;
        p2++;
        while (*p2 == ' ' || *p2 == '\t' || *p2 == '\n' || *p2 == '\r') p2++;
        if (*p2 != '"') continue;
        p2++;

        /* Check domain prefix + dot, e.g. "sensor." */
        size_t dlen = strlen(domain_prefix);
        if (strncmp(p2, domain_prefix, dlen) != 0 || p2[dlen] != '.') continue;
        p2 += dlen + 1;   /* skip "domain." -> at entity name start */

        /* Extract entity name (up to closing quote) */
        char ent[64];
        snprintf(ent, sizeof(ent), "%s.", domain_prefix);
        size_t elen = strlen(ent);
        const char * ep = p2;
        while (*ep && *ep != '"' && elen < sizeof(ent) - 1)
            ent[elen++] = *ep++;
        ent[elen] = 0;

        /* Walk forward to find "friendly_name" (tolerating spaces after colon) */
        char fn[64] = "";
        const char * fnp = strstr(pos, "\"friendly_name\"");
        if (fnp) {
            fnp += 15;   /* skip "friendly_name" */
            while (*fnp == ' ' || *fnp == '\t' || *fnp == '\n' || *fnp == '\r') fnp++;
            if (*fnp == ':') fnp++;
            while (*fnp == ' ' || *fnp == '\t' || *fnp == '\n' || *fnp == '\r') fnp++;
            if (*fnp == '"') {
                fnp++;
                size_t fni = 0;
                while (*fnp && *fnp != '"' && fni < sizeof(fn) - 1)
                    fn[fni++] = *fnp++;
                fn[fni] = 0;
            }
        }

        if (ent[0] && ha_id_safe(ent)) {
            if (found < 5)
                fprintf(stderr, "[ha] discover:   #%d entity='%s' friendly='%s'\n",
                        found, ent, fn[0] ? fn : "(none)");
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
    fprintf(stderr, "[ha] discover: domain='%s' returned %d entities (max=%d)\n",
            domain_prefix, found, max);
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
