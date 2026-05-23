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

/* Seed all watched state once over REST (events carry only future changes). */
static void seed_all(void) {
    poll_once();        /* curtain group + battery */
    poll_lights();
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
    if (CURTAIN_GROUP[0] && !strcmp(ent, CURTAIN_GROUP)) { apply_curtain(ns); return; }
    if (CURTAIN_LEFT[0]  && !strcmp(ent, CURTAIN_LEFT))  { apply_curtain_bat(ns, 0); return; }
    if (CURTAIN_RIGHT[0] && !strcmp(ent, CURTAIN_RIGHT)) { apply_curtain_bat(ns, 1); return; }
    if (settings.life360_a_entity[0] && !strcmp(ent, settings.life360_a_entity)) {
        apply_life360(ns, ha_state.loc_a, sizeof ha_state.loc_a, &ha_state.lat_a, &ha_state.lon_a, 0); return; }
    if (settings.life360_b_entity[0] && !strcmp(ent, settings.life360_b_entity)) {
        apply_life360(ns, ha_state.loc_b, sizeof ha_state.loc_b, &ha_state.lat_b, &ha_state.lon_b, 1); return; }
    if (settings.doorbell_entity[0] && !strcmp(ent, settings.doorbell_entity)) { apply_doorbell(ns); return; }
    for (int i = 0; i < ha_light_count; i++)
        if (!strcmp(ent, ha_lights[i].entity_id)) { apply_light(&ha_lights[i], ns); return; }
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
