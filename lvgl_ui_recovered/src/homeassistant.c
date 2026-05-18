/*
 * Minimal Home Assistant REST client. Used by the home-screen Curtains
 * tile to read cover state and issue open/close/stop commands against
 * cover.gordijnen_voorkamer (which is a Zigbee2MQTT group of two
 * curtain motors in the voorkamer).
 *
 * Auth: a single-line Long-Lived Access Token at /mnt/data/ha.cfg.
 */

#include "homeassistant.h"
#include "http.h"
#include "notify.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HA_HOST   "192.168.3.101:8123"
#define HA_POLL_S 10
#define HA_TOKEN_PATH "/mnt/data/ha.cfg"

#define CURTAIN_GROUP   "cover.gordijnen_voorkamer"
#define CURTAIN_LEFT    "sensor.curtain_3_7c3c_batterij"
#define CURTAIN_RIGHT   "sensor.curtain_3_7bc9_batterij"

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
    if (!g_token[0]) return -1;
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

/* POST /api/services/cover/<action>. Returns 0 on HTTP 2xx.
 * On failure, also fires a Toon-side notification so the user knows the
 * curtain button didn't actually do anything (cleared once HA recovers). */
static int ha_call_cover_service(const char * action) {
    if (!g_token[0]) return -1;
    char cmd[1024], out[256];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s --max-time 6 --connect-timeout 3 "
        "-X POST -H 'Authorization: Bearer %s' "
        "-H 'Content-Type: application/json' "
        "--data '{\"entity_id\":\"%s\"}' "
        "'http://%s/api/services/cover/%s' 2>/dev/null",
        g_token, CURTAIN_GROUP, HA_HOST, action);
    FILE * p = popen(cmd, "r");
    if (!p) {
        notify_show("system", "ha_offline", "HA niet bereikbaar — gordijn-actie niet uitgevoerd");
        return -1;
    }
    size_t n = fread(out, 1, sizeof(out) - 1, p);
    out[n] = 0;
    int rc = pclose(p);
    fprintf(stderr, "[ha] cover.%s rc=%d body=%.60s\n", action, rc, out);
    if (rc != 0 || n == 0)
        notify_show("system", "ha_offline", "HA niet bereikbaar — gordijn-actie niet uitgevoerd");
    else
        notify_clear("system", "ha_offline");
    return (rc == 0) ? 0 : -1;
}

/* POST /api/services/light/<action> on a specific entity_id.
 * On failure, fires a Toon-side notification (same logic as the cover
 * helper) so users pressing the lights tile while HA is dead get a
 * visible signal rather than silent no-op. */
static int ha_call_light_service(const char * action, const char * entity_id) {
    if (!g_token[0]) return -1;
    char cmd[1024], out[256];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s --max-time 6 --connect-timeout 3 "
        "-X POST -H 'Authorization: Bearer %s' "
        "-H 'Content-Type: application/json' "
        "--data '{\"entity_id\":\"%s\"}' "
        "'http://%s/api/services/light/%s' 2>/dev/null",
        g_token, entity_id, HA_HOST, action);
    FILE * p = popen(cmd, "r");
    if (!p) {
        notify_show("system", "ha_offline", "HA niet bereikbaar — licht-actie niet uitgevoerd");
        return -1;
    }
    size_t n = fread(out, 1, sizeof(out) - 1, p);
    out[n] = 0;
    int rc = pclose(p);
    fprintf(stderr, "[ha] light.%s %s rc=%d\n", action, entity_id, rc);
    if (rc != 0 || n == 0)
        notify_show("system", "ha_offline", "HA niet bereikbaar — licht-actie niet uitgevoerd");
    else
        notify_clear("system", "ha_offline");
    return (rc == 0) ? 0 : -1;
}

/* Curated list of room lights the home Lights screen exposes. Hardcoded
 * because the user's HA has 40+ light entities, most of which are camera
 * indicators / WLED sub-segments / offline placeholders. Pick the ones
 * worth showing per room; extend by editing this table + bumping
 * HA_LIGHT_COUNT in the header. */
ha_light_t ha_lights[HA_LIGHT_COUNT] = {
    /* Woonkamer */
    { "light.dimmer_voorkamer_licht", "Voorkamer",   "Woonkamer", 0,0,-1 },
    { "light.bank_lamp",              "Bank lamp",   "Woonkamer", 0,0,-1 },
    { "light.moodlight_papa",         "Moodlight",   "Woonkamer", 0,0,-1 },
    { "light.spotje_rechts",          "Spotje R",    "Woonkamer", 0,0,-1 },
    { "light.main_light",             "Main",        "Woonkamer", 0,0,-1 },
    { "light.ledstrip_tv",            "TV strip",    "Woonkamer", 0,0,-1 },
    /* Keuken */
    { "light.wled_keukenkast",        "WLED kast",   "Keuken",    0,0,-1 },
    { "light.led_strip_aanrecht",     "Aanrecht",    "Keuken",    0,0,-1 },
    { "light.keuken_begin",           "Keuken begin","Keuken",    0,0,-1 },
    { "light.keuken_eind",            "Keuken eind", "Keuken",    0,0,-1 },
    { "light.spotjes_schouw",         "Schouw",      "Keuken",    0,0,-1 },
    /* Slaapkamers / overig */
    { "light.grote_slaapkamer_licht", "Slaapkamer",  "Boven",     0,0,-1 },
    { "light.studeerkamer_licht",     "Studeer",     "Boven",     0,0,-1 },
    { "light.lamp_van_kaya",          "Kaja",        "Boven",     0,0,-1 },
};

/* Refresh every light's on/availability/brightness. Cheap — one
 * /api/states/<id> call per light. ~14 small requests, runs in the same
 * thread as the curtain poll. */
static void poll_lights(void) {
    char body[512];
    for (int i = 0; i < HA_LIGHT_COUNT; i++) {
        ha_light_t * L = &ha_lights[i];
        if (ha_get_state(L->entity_id, body, sizeof(body)) != 0) {
            L->available = 0;
            continue;
        }
        char st[24] = {0};
        extract_str(body, "state", st, sizeof(st));
        if (!strcmp(st, "on"))        { L->available = 1; L->on = 1; }
        else if (!strcmp(st, "off"))  { L->available = 1; L->on = 0; }
        else                          { L->available = 0; L->on = 0; }
        int v;
        if (extract_int(body, "brightness", &v)) L->brightness = v;
    }
}

/* Async helpers for the screen handlers. Each runs the actual REST POST
 * on a detached thread so LVGL stays responsive; the next poll picks up
 * the new state within a few seconds. */
static void * light_action_thread(void * arg) {
    char ** p = (char **)arg;       /* [action, entity_id] */
    ha_call_light_service(p[0], p[1]);
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
static void poll_life360_one(const char * entity_id, char * out, size_t outsz) {
    char body[1536];
    if (ha_get_state(entity_id, body, sizeof(body)) != 0) return;
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
    const char * city = c1 ? strip_postcode(trim(c1 + 1)) : "";

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

static void poll_life360(void) {
    poll_life360_one("device_tracker.life360_ronald_brakeboer",
                     ha_state.loc_ronald, sizeof(ha_state.loc_ronald));
    poll_life360_one("device_tracker.life360_caja_brakeboer",
                     ha_state.loc_caja,   sizeof(ha_state.loc_caja));
}

static void poll_once(void) {
    static int miss = 0;
    char body[1024];
    if (ha_get_state(CURTAIN_GROUP, body, sizeof(body)) != 0) {
        /* Single failed fetch isn't enough to drop the offline flag — HA
         * can be slow during its own service-call processing. Tolerate up
         * to 2 consecutive misses before the UI shows "(HA offline)". */
        if (++miss >= 2) ha_state.connected = 0;
        return;
    }
    miss = 0;
    ha_state.connected = 1;
    extract_str(body, "state",
                ha_state.curtain_state, sizeof(ha_state.curtain_state));
    int v;
    if (extract_int(body, "current_position", &v)) ha_state.curtain_pos = v;
    if (extract_int(body, "is_closed",        &v)) ha_state.curtain_is_closed = v;

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
        if (id == CURTAIN_RIGHT) break;
    }
    ha_state.curtain_battery = bat_min;
}

static void * ha_thread(void * arg) {
    (void)arg;
    while (1) {
        poll_once();
        poll_lights();
        poll_life360();
        /* Speed up the poll while the curtain is actively moving so the
         * spinner / position bar feel live. Back off to the normal 10 s
         * cadence as soon as it parks. */
        int moving = ha_state.curtain_state[0] &&
                     (!strcmp(ha_state.curtain_state, "opening") ||
                      !strcmp(ha_state.curtain_state, "closing"));
        sleep(moving ? 2 : HA_POLL_S);
    }
    return NULL;
}

static void * cover_action_thread(void * arg) {
    char * action = (char *)arg;
    ha_call_cover_service(action);
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

int ha_start(void) {
    load_token();
    if (!g_token[0]) {
        fprintf(stderr, "[ha] no token at " HA_TOKEN_PATH " — HA tile will stay disconnected\n");
    }
    pthread_t t;
    pthread_create(&t, NULL, ha_thread, NULL);
    pthread_detach(t);
    fprintf(stderr, "[ha] poller started (host=%s every %ds)\n",
            HA_HOST, HA_POLL_S);
    return 0;
}
