/*
 * ventilation.c — background poller for the NRG-Itho-Wifi bridge.
 * Pulls /api.html?get=ithostatus every VENT_POLL_S seconds, parses the few
 * fields we care about, and exposes them via vent_state. Commands are POSTed
 * (well, sent via GET — the API is all GET) by vent_send_vremote.
 *
 * Auth is by query string: ?username=…&password=… on every URL.
 *
 * Configurable via /mnt/data/vent.conf (one line):  user:pass
 * Falls back to compiled-in defaults if missing.
 */
#include "ventilation.h"
#include "http.h"
#include "settings.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>          /* atoi */
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <errno.h>
#include <time.h>

/* Itho host comes from settings (empty = vent disabled) so no personal IP
 * ships in the binary. All call sites use it as a "%s" snprintf arg. */
#define VENT_HOST       (settings.vent_host)
/* MQTT push gives near-instant updates; REST poll is now a safety
 * fallback. 60s is plenty to detect a dead Itho without hammering it. */
#define VENT_POLL_S     60
#define CONF_PATH       "/mnt/data/vent.conf"

vent_state_t vent_state = {0};

/* Forward decls — vent_wire_cmd / vent_user_preset are defined below the
 * fetch helpers but consumed inside them. */
static const char * vent_wire_cmd(const char * user_preset);
static const char * vent_user_preset(const char * wire_cmd);

static char g_user[32] = "";   /* filled from /mnt/data/vent.conf */
static char g_pass[32] = "";

static char g_settings_json[8192];

static void load_conf(void) {
    FILE * f = fopen(CONF_PATH, "r");
    if (!f) return;
    char line[128];
    if (fgets(line, sizeof(line), f)) {
        char * nl = strchr(line, '\n'); if (nl) *nl = 0;
        char * c  = strchr(line, ':');
        if (c) {
            *c = 0;
            snprintf(g_user, sizeof(g_user), "%s", line);
            snprintf(g_pass, sizeof(g_pass), "%s", c + 1);
        }
    }
    fclose(f);
}

/* Tiny JSON helpers — the response is flat key/value so no recursion. */
static int extract_int(const char * json, const char * key, int * out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char * p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p == '"') return 0;  /* "not available" */
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

/* Parse a full ithostatus JSON body and apply to vent_state. Used by both
 * the HTTP fallback path AND the MQTT push path. */
static void parse_status_json(const char * body) {
    vent_state.connected = 1;
    int v;
    /* speed_pct is the user-visible "running %". Prefer Itho's
     * "ExhFanSpeed (%)" (actual exhaust-fan output) when it's plausible;
     * fall back to "Ventilation setpoint (%)" otherwise.
     *
     * On this unit ExhFanSpeed sticks at 0 even while the fan is
     * spinning (Error=16 on the API — sensor fault). Without the
     * fallback the home + dim tiles would lie "0 %" while the fan
     * blasts at 100. If rpm is non-zero but ExhFanSpeed is 0, trust the
     * setpoint instead — it's what the controller is commanding the
     * fan to do, which is the next-best ground truth. */
    int exh_pct = -1, set_pct = -1, rpm = -1;
    /* Itho-Wifi firmware revisions disagree on the key names. The older
     * NRG firmware exposed "ExhFanSpeed (%)" + "Ventilation setpoint (%)";
     * the current firmware (.236, verified 2026-06-15) renamed them to
     * "Speed status" (real fan-output %, now a true non-zero measurement —
     * the old Error=16 sensor fault is gone) + "Ventilation level (%)"
     * (the commanded level). Accept whichever the unit actually sends,
     * else the home + dim vent tiles silently read 0 % / never respond. */
    if      (extract_int(body, "ExhFanSpeed (%)",          &v)) exh_pct = v;
    else if (extract_int(body, "Speed status",             &v)) exh_pct = v;
    if      (extract_int(body, "Ventilation setpoint (%)", &v)) set_pct = v;
    else if (extract_int(body, "Ventilation level (%)",    &v)) set_pct = v;
    if      (extract_int(body, "Fan speed (rpm)",          &v)) rpm     = v;
    if (set_pct >= 0) vent_state.exh_fan_pct = set_pct;
    if (rpm     >= 0) vent_state.fan_rpm     = rpm;
    /* speed_pct = honest "what's the fan actually doing".
     *   - Trust ExhFanSpeed if Itho reports >0 (real measurement).
     *   - Otherwise derive from rpm — the sensor on this unit is broken
     *     (Error=16) and stays at 0 in High mode, so the commanded
     *     setpoint can lie ("High 100 %" while rpm shows 696). 3500 rpm =
     *     full blast on Itho 1006 ≈ 100 %, scale linearly.
     *   - 0 rpm → 0 %. */
    if (exh_pct > 0)        vent_state.speed_pct = exh_pct;
    else if (rpm > 50)      vent_state.speed_pct = rpm * 100 / 3500;
    else                    vent_state.speed_pct = 0;
    if (extract_int(body, "Filter dirty",               &v)) vent_state.filter_dirty   = v;
    if (extract_int(body, "Internal fault",             &v)) vent_state.internal_fault = v;
    if (extract_int(body, "Error",                      &v)) vent_state.error_code     = v;
    if (extract_int(body, "Total operation (hours)",    &v)) vent_state.total_hours    = v;
    if (extract_int(body, "RemainingTime (min)",        &v)) vent_state.remaining_min  = v;
    /* FanInfo reports the wire-side label ("low" / "high" / "auto" / "timer"
     * / "medium"). Translate to the user-intent name so display code
     * doesn't need to know about the swap. */
    char wire_fi[16] = {0};
    if (extract_str(body, "FanInfo", wire_fi, sizeof(wire_fi))) {
        const char * shown = vent_user_preset(wire_fi);
        snprintf(vent_state.fan_info, sizeof(vent_state.fan_info),
                 "%s", shown ? shown : wire_fi);
    }
}

/* HTTP fallback. Calls the parser if it gets a valid body. */
static int fetch_status(void) {
    char url[256], body[4096];
    snprintf(url, sizeof(url),
        "http://%s/api.html?get=ithostatus&username=%s&password=%s",
        VENT_HOST, g_user, g_pass);
    int rc = http_fetch(url, body, sizeof(body));
    if (rc != 0 || strstr(body, "AUTHENTICATION FAILED")) {
        vent_state.connected = 0;
        return -1;
    }
    parse_status_json(body);
    return 0;
}

/* Low/High wire-label swap. Was needed 2026-05-18 because the unit's virtual
 * remote was MISCONFIGURED (wrong type "RFT CO2", never joined) so vremote
 * preset commands behaved backwards/inconsistently — "High" gave less airflow
 * than "Low".
 *
 * DISABLED 2026-06-15: the virtual remote was fixed — retyped to "RFT CVE"
 * (the unit's native class) and properly joined to the CVE. The box now
 * honours preset commands correctly, so the compensating swap is no longer
 * needed and was making the buttons reversed. Set back to 1 only if a unit
 * genuinely reports inverted airflow again. */
#define VENT_SWAP_LOW_HIGH 0

static const char * vent_wire_cmd(const char * user_preset) {
    if (!user_preset) return NULL;
#if VENT_SWAP_LOW_HIGH
    if (!strcmp(user_preset, "low"))  return "high";
    if (!strcmp(user_preset, "high")) return "low";
#endif
    return user_preset;
}

/* Inverse of vent_wire_cmd — given what we sent on the wire (or what we
 * read back from FanInfo / lastcmd), return the user-intent preset name
 * the UI should display. */
static const char * vent_user_preset(const char * wire_cmd) {
    if (!wire_cmd) return NULL;
#if VENT_SWAP_LOW_HIGH
    if (!strcmp(wire_cmd, "low"))  return "high";
    if (!strcmp(wire_cmd, "high")) return "low";
#endif
    return wire_cmd;
}

/* Clean display label for the current vent MODE. Source: fan_info if the
 * firmware still publishes FanInfo, else last_cmd. Normalises the raw Itho
 * lastcmd strings: "speed:N" / "speed:N,timer:0" → "Manual" (the slider set
 * a fixed speed), "timerN" → "Timer", else the capitalised word. Returns a
 * static buffer — call from the LVGL thread only. */
const char * vent_mode_label(void) {
    static char out[16];
    char src[24];
    snprintf(src, sizeof src, "%s",
             vent_state.fan_info[0] ? (const char *)vent_state.fan_info
                                    : (const char *)vent_state.last_cmd);
    if (!src[0]) return "?";
    if (!strncasecmp(src, "speed", 5)) return "Manual";
    if (!strncasecmp(src, "timer", 5)) return "Timer";
    snprintf(out, sizeof out, "%c%s",
             (src[0] >= 'a' && src[0] <= 'z') ? src[0] - 'a' + 'A' : src[0],
             src + 1);
    return out;
}

/* Map a user-intent vremote command to the speed % we expect the fan to
 * settle at. Used for optimistic UI updates so the "57 %" pill reflects
 * the new state without waiting for the next 8-second poll. */
static int vent_expected_pct(const char * cmd) {
    if (!cmd) return -1;
    if (!strcmp(cmd, "away"))    return  0;
    if (!strcmp(cmd, "low"))     return 20;
    if (!strcmp(cmd, "medium"))  return 50;
    if (!strcmp(cmd, "high"))    return 100;
    if (!strcmp(cmd, "auto"))    return 30;
    if (!strncmp(cmd, "timer", 5)) return 100;
    return -1;
}

/* Fetch /api.html?get=lastcmd and populate vent_state.last_cmd /
 * last_source / last_cmd_ts. The lastcmd command is the only place the
 * bridge exposes WHO triggered the change — physical RF remote vs HTML
 * API vremote vs the Itho's own front buttons. */
/* Parse a lastcmd JSON body and apply to vent_state. */
static void parse_lastcmd_json(const char * body) {
    char wire_cmd[16] = {0};
    if (extract_str(body, "command", wire_cmd, sizeof(wire_cmd))) {
        const char * shown = vent_user_preset(wire_cmd);
        snprintf((char *)vent_state.last_cmd, sizeof(vent_state.last_cmd),
                 "%s", shown ? shown : wire_cmd);
    }
    extract_str(body, "source",
                (char *)vent_state.last_source, sizeof(vent_state.last_source));
    int ts;
    if (extract_int(body, "timestamp", &ts)) vent_state.last_cmd_ts = ts;
    fprintf(stderr, "[vent] lastcmd source=%.40s cmd=%.16s\n",
            vent_state.last_source, vent_state.last_cmd);
}

static void fetch_lastcmd(void) {
    char url[256], body[512];
    snprintf(url, sizeof(url),
        "http://%s/api.html?get=lastcmd&username=%s&password=%s",
        VENT_HOST, g_user, g_pass);
    int rc = http_fetch(url, body, sizeof(body));
    if (rc != 0) {
        fprintf(stderr, "[vent] fetch_lastcmd rc=%d\n", rc);
        return;
    }
    parse_lastcmd_json(body);
}

/* Thread entry — strdup'd cmd, freed here. */
static void * vremote_thread(void * arg) {
    char * cmd = (char *)arg;
    vent_send_vremote(cmd);
    free(cmd);
    return NULL;
}

void vent_send_vremote_async(const char * cmd) {
    int p = vent_expected_pct(cmd);
    if (p >= 0) {
        vent_state.speed_pct   = p;
        vent_state.exh_fan_pct = p;     /* approximate — actual %≈setpoint */
        /* NOTE: deliberately do NOT fake fan_rpm here. The spinner is driven
         * by real rpm, which physically lags the command by tens of seconds
         * (and may not rise at all if the command doesn't actuate). Faking a
         * high rpm made the spinner spike fast then snap back to the real low
         * rpm on the next MQTT push ("fast then drops to slow"). Let it track
         * the honest rpm instead. */
    }
#ifdef WASM_BUILD
    /* WASM client: bridge via the master Toon's POST /api/vent — the master
     * then runs the real http_fetch against the Itho on its own LAN. No
     * pthread or raw sockets needed (and neither would work here anyway). */
    extern void wasm_push_event(const char *, const char *);
    char body[64];
    snprintf(body, sizeof body, "{\"cmd\":\"%s\"}", cmd ? cmd : "");
    wasm_push_event("/api/vent", body);
    return;
#endif
    pthread_t t;
    char * dup = strdup(cmd ? cmd : "");
    if (!dup) return;
    if (pthread_create(&t, NULL, vremote_thread, dup) != 0) {
        free(dup);
        return;
    }
    pthread_detach(t);
}

int vent_send_vremote(const char * cmd) {
    const char * wire = vent_wire_cmd(cmd);
    if (!wire) return -1;
    char url[256], body[256];
    /* vremoteindex defaults to 0 → first configured virtual remote. */
    snprintf(url, sizeof(url),
        "http://%s/api.html?vremotecmd=%s&vremoteindex=0&username=%s&password=%s",
        VENT_HOST, wire, g_user, g_pass);
    int rc = http_fetch(url, body, sizeof(body));
    fprintf(stderr, "[vent] user=%s wire=%s rc=%d body=%.40s\n",
            cmd, wire, rc, body);
    int ok = (rc == 0 && !strstr(body, "AUTHENTICATION FAILED"));
    /* MQTT-only status model: no HTTP re-poll after the write. The button's
     * optimistic update (vent_send_vremote_async writes the expected % up
     * front) covers the visible gap, and the Itho's own itho/ithostatus +
     * itho/lastcmd MQTT publishes deliver the confirmed state a moment later.
     * The vremotecmd write itself stays HTTP — that's a command, not status,
     * and the bridge exposes no MQTT command channel. */
    return ok ? 0 : -1;
}

int vent_set_speed(int pwm) {
    if (pwm < 0)   pwm = 0;
    if (pwm > 255) pwm = 255;
    char url[256], body[256];
    snprintf(url, sizeof(url),
        "http://%s/api.html?speed=%d&timer=0&username=%s&password=%s",
        VENT_HOST, pwm, g_user, g_pass);
    int rc = http_fetch(url, body, sizeof(body));
    fprintf(stderr, "[vent] speed=%d rc=%d body=%.40s\n", pwm, rc, body);
    return (rc == 0 && !strstr(body, "AUTHENTICATION FAILED")) ? 0 : -1;
}

int vent_bump_speed(int delta) {
    char url[256], body[64];
    snprintf(url, sizeof(url),
        "http://%s/api.html?get=currentspeed&username=%s&password=%s",
        VENT_HOST, g_user, g_pass);
    if (http_fetch(url, body, sizeof(body)) != 0
        || strstr(body, "AUTHENTICATION FAILED"))
        return -1;
    int cur = atoi(body);
    return vent_set_speed(cur + delta);
}

/* Detached-thread wrapper around vent_set_speed so the slider's release
 * handler returns instantly (the HTTP GET to the Itho takes ~0.5-1 s and
 * must never run on the LVGL thread). Writes an optimistic % into vent_state
 * up front; the next itho/ithostatus MQTT push confirms/corrects it.
 * On this CVE the speed= PWM API is the RELIABLE control path (the vremote
 * presets often don't actuate), so the slider is the primary control. */
static void * setspeed_thread(void * arg) {
    int pwm = *(int *)arg;
    free(arg);
    vent_set_speed(pwm);
    return NULL;
}

void vent_set_speed_async(int pwm) {
    if (pwm < 0)   pwm = 0;
    if (pwm > 255) pwm = 255;
    int pct = pwm * 100 / 255;
    vent_state.exh_fan_pct = pct;
    vent_state.speed_pct   = pct;   /* optimistic; MQTT corrects in ~seconds */
    /* No optimistic fan_rpm — spinner tracks real rpm (see vent_send_vremote_async). */
#ifdef WASM_BUILD
    /* Slave/WASM client: forward to the master Toon, which owns the Itho LAN
     * link. handle_vent_post understands {"speed":N}. */
    extern void wasm_push_event(const char *, const char *);
    char body[48];
    snprintf(body, sizeof body, "{\"speed\":%d}", pwm);
    wasm_push_event("/api/vent", body);
    return;
#endif
    int * dup = malloc(sizeof(int));
    if (!dup) return;
    *dup = pwm;
    pthread_t t;
    if (pthread_create(&t, NULL, setspeed_thread, dup) != 0) { free(dup); return; }
    pthread_detach(t);
}

int vent_refresh_settings(void) {
    char url[256];
    snprintf(url, sizeof(url),
        "http://%s/api.html?get=ithosettings&username=%s&password=%s",
        VENT_HOST, g_user, g_pass);
    int rc = http_fetch(url, g_settings_json, sizeof(g_settings_json));
    if (rc != 0 || strstr(g_settings_json, "AUTHENTICATION FAILED")) {
        snprintf(g_settings_json, sizeof(g_settings_json),
                 "(fetch failed: rc=%d)", rc);
        return -1;
    }
    return 0;
}

const char * vent_settings_json(void) {
    return g_settings_json;
}

/* --- Per-index settings (getsetting=N / setsetting=N&value=V) --- */
vent_setting_t vent_settings[VENT_SETTING_COUNT] = {0};

int vent_fetch_one(int idx) {
    if (idx < 0 || idx >= VENT_SETTING_COUNT) return -1;
    char url[256], body[1024];
    snprintf(url, sizeof(url),
        "http://%s/api.html?getsetting=%d&username=%s&password=%s",
        VENT_HOST, idx, g_user, g_pass);
    int rc = http_fetch(url, body, sizeof(body));
    if (rc != 0 || strstr(body, "AUTHENTICATION FAILED") ||
        strstr(body, "\"status\":\"fail\"")) {
        vent_settings[idx].idx    = idx;
        vent_settings[idx].loaded = -1;
        return -1;
    }
    vent_settings[idx].idx = idx;
    extract_str(body, "label",
                vent_settings[idx].label, sizeof(vent_settings[idx].label));
    int v;
    if (extract_int(body, "current", &v)) vent_settings[idx].current = v;
    if (extract_int(body, "minimum", &v)) vent_settings[idx].minimum = v;
    if (extract_int(body, "maximum", &v)) vent_settings[idx].maximum = v;
    vent_settings[idx].loaded = 1;
    return 0;
}

int vent_fetch_all_settings(void) {
    int ok = 0;
    for (int i = 0; i < VENT_SETTING_COUNT; i++) {
        if (vent_fetch_one(i) == 0) ok++;
        usleep(20 * 1000);                 /* 20 ms pause between requests */
    }
    fprintf(stderr, "[vent] settings warm-load: %d/%d ok\n",
            ok, VENT_SETTING_COUNT);
    return ok;
}

int vent_save_setting(int idx, int value) {
    if (idx < 0 || idx >= VENT_SETTING_COUNT) return -1;
    char url[256], body[512];
    snprintf(url, sizeof(url),
        "http://%s/api.html?setsetting=%d&value=%d&username=%s&password=%s",
        VENT_HOST, idx, value, g_user, g_pass);
    int rc = http_fetch(url, body, sizeof(body));
    fprintf(stderr, "[vent] setsetting=%d value=%d rc=%d body=%.80s\n",
            idx, value, rc, body);
    if (rc != 0) return -1;
    if (strstr(body, "settings API is disabled")) return -2;
    if (strstr(body, "\"status\":\"success\"")) {
        vent_fetch_one(idx);               /* refresh cache after write */
        return 0;
    }
    return -1;
}

/* ============================================================
 * Minimal MQTT 3.1.1 subscriber for itho/ithostatus + itho/lastcmd.
 *
 * Toon doesn't ship libmosquitto / paho — implementing just enough of
 * the protocol inline. Subscribe-only client (we don't publish anything
 * MQTT-side; commands still go through the REST API).
 *
 * Config: /mnt/data/mqtt.cfg, single line: host:user:pass
 * (empty/missing = no MQTT push; the REST poll still works).
 *
 * Topics covered:
 *   itho/ithostatus  — full status JSON, every state change
 *   itho/lastcmd     — last command + source (HTML/RF/BTN)
 * ============================================================ */

#define MQTT_KEEPALIVE_S 60
#define MQTT_PORT        1883

static char g_mqtt_host[32] = "";   /* filled from config; empty = no MQTT push */
static char g_mqtt_user[32] = "";
static char g_mqtt_pass[64] = "";

static void load_mqtt_conf(void) {
    FILE * f = fopen("/mnt/data/mqtt.cfg", "r");
    if (!f) return;
    char line[256];
    if (fgets(line, sizeof(line), f)) {
        char * nl = strchr(line, '\n'); if (nl) *nl = 0;
        char * c1 = strchr(line, ':');
        if (c1) {
            *c1 = 0;
            char * c2 = strchr(c1 + 1, ':');
            if (c2) {
                *c2 = 0;
                snprintf(g_mqtt_host, sizeof(g_mqtt_host), "%s", line);
                snprintf(g_mqtt_user, sizeof(g_mqtt_user), "%s", c1 + 1);
                snprintf(g_mqtt_pass, sizeof(g_mqtt_pass), "%s", c2 + 1);
            }
        }
    }
    fclose(f);
}

/* MQTT var-int (remaining-length) encode. Returns # bytes written. */
static int mqtt_encode_remaining_length(unsigned int len, unsigned char * out) {
    int n = 0;
    do {
        unsigned char d = len & 0x7f;
        len >>= 7;
        if (len) d |= 0x80;
        out[n++] = d;
    } while (len);
    return n;
}

/* Read var-int from socket. Returns length or -1 on error. */
static int mqtt_read_remaining_length(int fd) {
    unsigned int v = 0, mult = 1;
    for (int i = 0; i < 4; i++) {
        unsigned char d;
        if (read(fd, &d, 1) != 1) return -1;
        v += (d & 0x7f) * mult;
        if (!(d & 0x80)) return (int)v;
        mult <<= 7;
    }
    return -1;
}

/* Write a UTF-8 MQTT string (2-byte BE length + bytes) into buf. */
static int mqtt_put_string(unsigned char * buf, const char * s) {
    int n = (int)strlen(s);
    buf[0] = (n >> 8) & 0xff;
    buf[1] = n & 0xff;
    memcpy(buf + 2, s, n);
    return 2 + n;
}

#define MQTT_LWT_TOPIC "toonui/lwt"
#define MQTT_LWT_OFFLINE "offline"
#define MQTT_LWT_ONLINE  "online"

/* Build + send CONNECT packet with LWT (will=offline, retain). Returns 0 on
 * success. Other subscribers (HA dashboards, status pages) see the will
 * fire if we drop the TCP without a clean DISCONNECT. */
static int mqtt_send_connect(int fd, const char * client_id,
                             const char * user, const char * pass) {
    unsigned char vh[16], payload[512];
    int vhn = 0;
    /* Variable header: protocol name + level + flags + keepalive */
    vhn += mqtt_put_string(vh + vhn, "MQTT");
    vh[vhn++] = 0x04;                           /* MQTT 3.1.1 */
    /* Flags: 0x02 = clean session, 0x04 = will-flag, 0x00 = will-QoS-0,
     * 0x20 = will-retain, plus user/pass bits below. */
    unsigned char flags = 0x02 | 0x04 | 0x20;
    if (user && user[0]) flags |= 0x80;
    if (pass && pass[0]) flags |= 0x40;
    vh[vhn++] = flags;
    vh[vhn++] = (MQTT_KEEPALIVE_S >> 8) & 0xff;
    vh[vhn++] = MQTT_KEEPALIVE_S & 0xff;

    /* Payload order (per MQTT 3.1.1 §3.1.3): client_id, will_topic,
     * will_msg, username, password. */
    int pln = 0;
    pln += mqtt_put_string(payload + pln, client_id);
    pln += mqtt_put_string(payload + pln, MQTT_LWT_TOPIC);
    pln += mqtt_put_string(payload + pln, MQTT_LWT_OFFLINE);
    if (user && user[0]) pln += mqtt_put_string(payload + pln, user);
    if (pass && pass[0]) pln += mqtt_put_string(payload + pln, pass);

    unsigned char fh[8];
    fh[0] = 0x10;                                /* CONNECT */
    int rln = mqtt_encode_remaining_length(vhn + pln, fh + 1);
    int fhn = 1 + rln;

    if (write(fd, fh, fhn)      != fhn)  return -1;
    if (write(fd, vh, vhn)      != vhn)  return -1;
    if (write(fd, payload, pln) != pln)  return -1;

    /* Read CONNACK */
    unsigned char ca[4];
    if (read(fd, ca, 4) != 4) return -1;
    if (ca[0] != 0x20 || ca[3] != 0) {
        fprintf(stderr, "[mqtt] CONNACK rejected: type=0x%02x rc=%d\n",
                ca[0], ca[3]);
        return -1;
    }
    return 0;
}

/* Publish a retained "online" announcement at QoS 0 (the will already
 * covers the dropping case; this just clears the retained "offline" for
 * any subscriber that joined while we were down). */
static int mqtt_publish_retained(int fd, const char * topic, const char * payload) {
    unsigned char buf[256];
    int n = 0;
    n += mqtt_put_string(buf + n, topic);
    int plen = (int)strlen(payload);
    memcpy(buf + n, payload, plen); n += plen;

    unsigned char fh[8];
    fh[0] = 0x30 | 0x01;                         /* PUBLISH + RETAIN */
    int rln = mqtt_encode_remaining_length(n, fh + 1);
    if (write(fd, fh, 1 + rln) != 1 + rln) return -1;
    if (write(fd, buf, n)      != n)       return -1;
    return 0;
}

/* Send a PUBACK for an incoming QoS-1 PUBLISH. */
static int mqtt_send_puback(int fd, unsigned short packet_id) {
    unsigned char pkt[4] = { 0x40, 0x02,
                             (unsigned char)(packet_id >> 8),
                             (unsigned char)(packet_id & 0xff) };
    return (write(fd, pkt, 4) == 4) ? 0 : -1;
}

/* Subscribe to a single topic at QoS 1. Broker will retransmit any
 * messages we miss between connect cycles. */
static int mqtt_subscribe(int fd, unsigned short packet_id, const char * topic) {
    unsigned char vh_pl[256];
    int n = 0;
    vh_pl[n++] = (packet_id >> 8) & 0xff;
    vh_pl[n++] = packet_id & 0xff;
    n += mqtt_put_string(vh_pl + n, topic);
    vh_pl[n++] = 0x01;                          /* QoS 1 */

    unsigned char fh[8];
    fh[0] = 0x82;                                /* SUBSCRIBE | flags=0010 */
    int rln = mqtt_encode_remaining_length(n, fh + 1);
    if (write(fd, fh, 1 + rln) != 1 + rln) return -1;
    if (write(fd, vh_pl, n)    != n)       return -1;
    /* SUBACK is just consumed by the reader loop. */
    return 0;
}

/* Read exactly `n` bytes from fd. Returns 0 on success, -1 on error. */
static int mqtt_read_full(int fd, unsigned char * buf, int n) {
    int got = 0;
    while (got < n) {
        int r = read(fd, buf + got, n - got);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

/* Send PINGREQ. */
static int mqtt_send_ping(int fd) {
    unsigned char pkt[2] = { 0xc0, 0x00 };
    return (write(fd, pkt, 2) == 2) ? 0 : -1;
}

/* Connect → subscribe → read loop. Returns when the connection dies; the
 * caller is expected to retry. */
static void mqtt_run_once(void) {
    /* Resolve + connect */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(MQTT_PORT);
    if (inet_pton(AF_INET, g_mqtt_host, &addr.sin_addr) != 1) {
        struct hostent * he = gethostbyname(g_mqtt_host);
        if (!he) { fprintf(stderr, "[mqtt] resolve %s failed\n", g_mqtt_host); return; }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    /* Read timeout slightly longer than ping interval so missing PINGRESP
     * → socket close → reconnect. */
    struct timeval tv = { .tv_sec = MQTT_KEEPALIVE_S + 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[mqtt] connect %s:%d failed: %s\n",
                g_mqtt_host, MQTT_PORT, strerror(errno));
        close(fd);
        return;
    }
    char cid[32];
    snprintf(cid, sizeof(cid), "toonui-%d", (int)getpid());
    if (mqtt_send_connect(fd, cid, g_mqtt_user, g_mqtt_pass) != 0) {
        fprintf(stderr, "[mqtt] CONNECT failed\n");
        close(fd); return;
    }
    fprintf(stderr, "[mqtt] connected to %s as %s, subscribing\n",
            g_mqtt_host, g_mqtt_user);
    /* Replace the retained "offline" from the will with a "online" marker
     * so anyone subscribed to toonui/lwt sees we came back. */
    mqtt_publish_retained(fd, MQTT_LWT_TOPIC, MQTT_LWT_ONLINE);
    mqtt_subscribe(fd, 1, "itho/ithostatus");
    mqtt_subscribe(fd, 2, "itho/lastcmd");
    mqtt_subscribe(fd, 3, "itho/lwt");   /* Itho bridge online/offline (retained) */

    /* Read loop. */
    time_t next_ping = time(NULL) + (MQTT_KEEPALIVE_S - 5);
    while (1) {
        /* Time to ping? */
        time_t now = time(NULL);
        if (now >= next_ping) {
            if (mqtt_send_ping(fd) != 0) {
                fprintf(stderr, "[mqtt] ping write failed\n");
                break;
            }
            next_ping = now + (MQTT_KEEPALIVE_S - 5);
        }
        unsigned char fhdr;
        int r = read(fd, &fhdr, 1);
        if (r < 0 && errno == EAGAIN) continue;
        if (r <= 0) { fprintf(stderr, "[mqtt] read header failed\n"); break; }
        int rlen = mqtt_read_remaining_length(fd);
        if (rlen < 0) { fprintf(stderr, "[mqtt] bad remaining-len\n"); break; }
        unsigned char * pkt = malloc(rlen + 1);
        if (!pkt) break;
        if (rlen > 0 && mqtt_read_full(fd, pkt, rlen) < 0) {
            free(pkt); break;
        }
        pkt[rlen] = 0;
        unsigned char type = fhdr & 0xf0;
        if (type == 0x30) {                       /* PUBLISH */
            int qos = (fhdr & 0x06) >> 1;
            int tlen = (pkt[0] << 8) | pkt[1];
            char topic[64] = {0};
            int tn = tlen < (int)sizeof(topic) - 1 ? tlen : (int)sizeof(topic) - 1;
            memcpy(topic, pkt + 2, tn); topic[tn] = 0;
            int hdr_off = 2 + tlen;
            unsigned short pkt_id = 0;
            if (qos > 0) {                        /* QoS 1+ → 2-byte packet ID */
                pkt_id = (pkt[hdr_off] << 8) | pkt[hdr_off + 1];
                hdr_off += 2;
            }
            int plen = rlen - hdr_off;
            char * payload = (char *)(pkt + hdr_off);
            payload[plen] = 0;
            if (!strcmp(topic, "itho/ithostatus")) {
                parse_status_json(payload);
            } else if (!strcmp(topic, "itho/lastcmd")) {
                parse_lastcmd_json(payload);
            } else if (!strcmp(topic, "itho/lwt")) {
                /* Itho-Wifi Last-Will: "online" while the bridge holds the
                 * broker session, "offline" published by the broker when it
                 * drops. Drives the vent tile's online/offline indicator. */
                int online = !strncasecmp(payload, "online", 6);
                vent_state.itho_online = online;
                fprintf(stderr, "[vent] itho/lwt = %s\n",
                        online ? "online" : "offline");
            }
            /* QoS 1: ack so the broker drops it from its in-flight queue.
             * Without this the broker keeps retrying every reconnect. */
            if (qos == 1) mqtt_send_puback(fd, pkt_id);
        }
        /* SUBACK (0x90), PINGRESP (0xd0), PUBACK (0x40) — silently consumed. */
        free(pkt);
    }
    /* MQTT-only model has no HTTP fallback, so a dropped link means the data
     * we hold is now stale. Mark disconnected so the vent tile shows "—"
     * instead of a frozen reading until the subscriber reconnects and the
     * next itho/ithostatus repopulates it. */
    vent_state.connected = 0;
    close(fd);
}

static void * mqtt_thread(void * arg) {
    (void)arg;
    load_mqtt_conf();
    if (!g_mqtt_user[0]) {
        fprintf(stderr, "[mqtt] no /mnt/data/mqtt.cfg — push updates disabled\n");
        return NULL;
    }
    while (1) {
        mqtt_run_once();
        fprintf(stderr, "[mqtt] disconnected, retrying in 5s\n");
        sleep(5);
    }
    return NULL;
}

/* ============================================================ end MQTT */

static void * vent_thread(void * arg) {
    (void)arg;
    /* MQTT-only status model (no HTTP status polling, no fallback): live
       ithostatus/lastcmd arrive exclusively via the MQTT subscriber thread.
       This thread does ONE non-status job: warm-load the i2c settings table
       (HTTP getsetting=N) so the advanced page renders instantly when opened.
       Settings are not published over MQTT, so there's no push equivalent.
       After the warm-load completes the thread exits — no poll loop. */
    vent_fetch_all_settings();
    return NULL;
}

int vent_start(void) {
    vent_state.itho_online = -1;   /* unknown until the retained itho/lwt arrives */
    if (!settings.enable_vent) {
        fprintf(stderr, "[vent] integration disabled — not starting poller/MQTT\n");
        return 0;
    }
    if (!settings.vent_host[0]) {
        fprintf(stderr, "[vent] no host configured — not starting poller/MQTT\n");
        return 0;
    }
    load_conf();
    if (!g_pass[0]) {
        fprintf(stderr, "[vent] no /mnt/data/vent.conf — vent will fail auth\n");
    }
    pthread_t t;
    pthread_create(&t, NULL, vent_thread, NULL);
    pthread_detach(t);
    fprintf(stderr, "[vent] settings warm-load thread started (host=%s)\n",
            VENT_HOST);
    pthread_t mt;
    pthread_create(&mt, NULL, mqtt_thread, NULL);
    pthread_detach(mt);
    fprintf(stderr, "[vent] status = MQTT-only (itho/ithostatus + itho/lastcmd); "
            "no HTTP polling, no fallback\n");
    return 0;
}
