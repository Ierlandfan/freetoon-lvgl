/*
 * Minimal BoxTalk client embedded in toonui.
 * Connects to 127.0.0.1:1337, handles handshake, subscribes to sensor services,
 * parses incoming notify messages and updates toon_state.
 *
 * Wire format: NUL-delimited XML messages. We do a simple read-loop that
 * accumulates bytes into a buffer; on each NUL, we have a full message.
 *
 * XML parsing: ad-hoc, no library. The message shapes are predictable; we
 * just hunt for attribute values and child-element text via strstr.
 */
#include "boxtalk.h"
#include "i18n.h"
#include "inbox.h"
#include "schedule.h"
#include "settings.h"
#include "tile_slots.h"
#include "meteradapter.h"
#include "client_link.h"
#include "http.h"
#include "canonical_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

toon_state_t toon_state = { .program_state = -1, .active_state = -1,
                            .boiler_type = 1 }; /* default 1 (On/Off) per CR6 */

static pthread_mutex_t g_bxt_state_mtx = PTHREAD_MUTEX_INITIALIZER;

void boxtalk_lock(void) {
    pthread_mutex_lock(&g_bxt_state_mtx);
}

void boxtalk_unlock(void) {
    pthread_mutex_unlock(&g_bxt_state_mtx);
}

void boxtalk_get_snapshot(toon_state_t * out) {
    if (!out) return;
    pthread_mutex_lock(&g_bxt_state_mtx);
    *out = toon_state;
    pthread_mutex_unlock(&g_bxt_state_mtx);
}

/* Temporary-override bookkeeping. Read by program_label() at the top of
 * the file as well as by the +/- nudge and tick paths further down, so
 * the declarations live up here. See note_temp_override() for semantics. */
static int temp_override_active = 0;
static int temp_override_origin = -1;      /* preset at moment of override */
static int temp_override_sched_entry = -1; /* schedule entry index at moment of override */

static void clear_temp_override(void) {
    temp_override_active = 0;
    temp_override_origin = -1;
    temp_override_sched_entry = -1;
}

/* happ_thermstat device uuid — destination for thermostat/boiler actions. */
#define THERMSTAT_UUID "b822de89-ecbd-4f6f-9fd2-5cac18fc06c4"
/* boilerDev device uuid (profile "BoilerInfo") — destination for boiler
   state-variable queries (flow/return temps). */
#define BOILER_UUID    "9846ac80-1ec4-4530-a3f8-c2e487029461"

const char * air_quality_label(int eco2, int tvoc) {
    int level = 0;
    if (eco2 > 0) {
        if      (eco2 >= 2000) level = 5;
        else if (eco2 >= 1500) level = 4;
        else if (eco2 >= 1000) level = 3;
        else if (eco2 >=  700) level = 2;
        else                   level = 1;
    }
    if (tvoc > 0) {
        int t = 1;
        if      (tvoc >= 1000) t = 5;
        else if (tvoc >=  500) t = 4;
        else if (tvoc >=  300) t = 3;
        else if (tvoc >=  100) t = 2;
        if (t > level) level = t;
    }
    switch (level) {
        case 1: return "Excellent";
        case 2: return "Good";
        case 3: return "Fair";
        case 4: return "Poor";
        case 5: return "Bad";
        default: return "";
    }
}

unsigned int air_quality_color(int eco2, int tvoc) {
    const char * l = air_quality_label(eco2, tvoc);
    if (!*l) return 0x888888;
    switch (l[0]) {
        case 'E': return 0x66cc88;   /* Excellent — green   */
        case 'G': return 0x88dd66;   /* Good      — lime    */
        case 'F': return 0xffcc44;   /* Fair      — yellow  */
        case 'P': return 0xff8844;   /* Poor      — orange  */
        case 'B': return 0xff3344;   /* Bad       — red     */
    }
    return 0x888888;
}

const char* program_label(void) {
    /* Dim/ambient label = the active preset name (stock getStateName(activeState)),
     * which the stock UI surfaces regardless of mode; "Manual" only when there is
     * no active preset (activeState == -1). If a temporary override is active,
     * reflect the active preset until the scheduled switch point rather than
     * displaying "Manual". */
    int st = toon_state.active_state;
    if (st < 0 && temp_override_active && temp_override_origin >= 0) {
        st = temp_override_origin;
    }
    switch (st) {
        case 0: return tr("Comfort", "Comfort");
        case 1: return tr("Thuis", "Home");
        case 2: return tr("Slapen", "Sleep");
        case 3: return tr("Weg", "Away");
        default: return tr("Handmatig", "Manual");
    }
}

static int sock_fd = -1;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;

/* Device UUIDs are derived from the local hostname at startup — on a Toon the
 * hostname IS the device serial (e.g. "qb-000000000000-0000000000"), and
 * targeted BoxTalk actions (Z-Wave, netcon, usermsg, rrd) must address the
 * *local* device or hcb_bxtproxy won't route them. Hardcoding one serial broke
 * every other Toon. The literals below are just the fallback if gethostname
 * fails or returns a non-"qb-" name. */
static char dev_base[64]      = "qb-000000000000-0000000000";
static char OUR_UUID[96]      = "qb-000000000000-0000000000:toonui";
static char ZWAVE_UUID[96]    = "qb-000000000000-0000000000:hdrv_zwave";
static char NETCON_UUID[96]   = "qb-000000000000-0000000000:hcb_netcon";
static char USERMSG_UUID[96]  = "qb-000000000000-0000000000:happ_usermsg";
static char RRD_UUID[96]      = "qb-000000000000-0000000000:hcb_rrd";
static char THERMD_UUID[96]   = "qb-000000000000-0000000000:happ_thermstat";
static char CONFIG_UUID[96]   = "qb-000000000000-0000000000:hcb_config";
static char s_therm_settings_uuid[96] = {0};

static void bt_seed_thermostat_settings_uuid(void) {
    FILE * f = fopen("/qmf/config/config_happ_thermstat.xml", "r");
    if (!f) return;
    char line[512];
    char cur_uuid[96] = {0};
    while (fgets(line, sizeof line, f)) {
        char * u1 = strstr(line, "<uuid>");
        if (u1) {
            char * u2 = strstr(u1 + 6, "</uuid>");
            if (u2 && (size_t)(u2 - (u1 + 6)) < sizeof(cur_uuid)) {
                size_t len = u2 - (u1 + 6);
                memcpy(cur_uuid, u1 + 6, len);
                cur_uuid[len] = '\0';
            }
        }
        if (strstr(line, "thermostatSettings") && cur_uuid[0]) {
            snprintf(s_therm_settings_uuid, sizeof s_therm_settings_uuid, "%s", cur_uuid);
            fprintf(stderr, "[bxt] seeded thermostatSettings uuid from config: %s\n", s_therm_settings_uuid);
            break;
        }
    }
    fclose(f);
}

static void bt_init_dev_uuids(void) {
    char hn[64] = {0};
    /* Accept both "qb-..." (Toon 2 / nxt) and "eneco-..." (Toon 1 / qb2)
     * hostnames as the BoxTalk UUID prefix -- without the eneco- case a
     * Toon 1 keeps the dummy serial and every subscription (incl. the
     * indoor-temperature dataset) silently fails. */
    if (gethostname(hn, sizeof hn - 1) == 0 &&
        (strncmp(hn, "qb-", 3) == 0 || strncmp(hn, "eneco-", 6) == 0))
        snprintf(dev_base, sizeof dev_base, "%s", hn);
    snprintf(OUR_UUID,     sizeof OUR_UUID,     "%s:toonui",      dev_base);
    snprintf(ZWAVE_UUID,   sizeof ZWAVE_UUID,   "%s:hdrv_zwave",  dev_base);
    snprintf(NETCON_UUID,  sizeof NETCON_UUID,  "%s:hcb_netcon",  dev_base);
    snprintf(USERMSG_UUID, sizeof USERMSG_UUID, "%s:happ_usermsg",dev_base);
    snprintf(RRD_UUID,     sizeof RRD_UUID,     "%s:hcb_rrd",     dev_base);
    snprintf(THERMD_UUID,  sizeof THERMD_UUID,  "%s:happ_thermstat", dev_base);
    snprintf(CONFIG_UUID,  sizeof CONFIG_UUID,  "%s:hcb_config",     dev_base);
}

/* Runtime device UUIDs for other modules (inbox/notify) — avoids hardcoding
 * the device serial anywhere. Valid after bt_init_dev_uuids() (called at
 * connect); before that they hold the generic placeholder. */
const char * boxtalk_our_uuid(void)     { return OUR_UUID; }
const char * boxtalk_usermsg_uuid(void) { return USERMSG_UUID; }

#define BUFCAP (64 * 1024)
static char rxbuf[BUFCAP];
static size_t rxlen = 0;

/* ---- low-level send ---- */
static int sock_send_all(const char* buf, size_t n) {
    while (n > 0) {
        ssize_t k = send(sock_fd, buf, n, 0);
        if (k <= 0) return -1;
        buf += k; n -= k;
    }
    return 0;
}

static int send_msg(const char* xml) {
    pthread_mutex_lock(&send_lock);
    int rc = sock_send_all(xml, strlen(xml));
    if (rc == 0) {
        char nul = 0;
        rc = sock_send_all(&nul, 1);
    }
    pthread_mutex_unlock(&send_lock);
    return rc;
}

/* ---- helpers ---- */
static const char* attr_value(const char* xml, const char* name, char* out, size_t outsz) {
    /* find name="..." */
    char needle[64];
    snprintf(needle, sizeof(needle), "%s=\"", name);
    const char* p = strstr(xml, needle);
    if (!p) { if (out && outsz) out[0] = 0; return NULL; }
    p += strlen(needle);
    const char* e = strchr(p, '"');
    if (!e) { if (out && outsz) out[0] = 0; return NULL; }
    size_t n = (size_t)(e - p);
    if (out && outsz) {
        if (n >= outsz) n = outsz - 1;
        memcpy(out, p, n);
        out[n] = 0;
    }
    return p;
}

static int elem_text_float(const char* xml, const char* elem, float* out) {
    char open[64];
    snprintf(open, sizeof(open), "<%s>", elem);
    const char* p = strstr(xml, open);
    if (!p) return 0;
    p += strlen(open);
    char close[64];
    snprintf(close, sizeof(close), "</%s>", elem);
    const char* e = strstr(p, close);
    if (!e) return 0;
    char buf[64];
    size_t n = (size_t)(e - p);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, p, n);
    buf[n] = 0;
    *out = (float)atof(buf);
    return 1;
}

static time_t s_burner_last_active_ts = 0;

static void update_burner_state(int bi) {
    time_t now = time(NULL);
    if (bi == 1) {
        s_burner_last_active_ts = now;
        toon_state.burner_on = 1;
        toon_state.dhw_on    = 0;
    } else if (bi == 2) {
        toon_state.burner_on = 0;
        toon_state.dhw_on    = 1;
    } else {
        toon_state.dhw_on = 0;
        /* Debounce burner off-state: hold active for 20s if still heating towards setpoint */
        if (s_burner_last_active_ts > 0 && (now - s_burner_last_active_ts < 20) &&
            (toon_state.setpoint > toon_state.indoor_temp)) {
            toon_state.burner_on = 1;
        } else {
            toon_state.burner_on = 0;
        }
    }
}

static inline void set_water_pressure(float v) {
    if (v <= 0.05f) return;
    if (v >= 50.0f) toon_state.water_pressure = v / 100.0f;       /* centibar: 180 -> 1.80 */
    else if (v >= 5.0f) toon_state.water_pressure = v / 10.0f;    /* decibar:  15  -> 1.50 */
    else toon_state.water_pressure = v;                           /* bar:      1.5 -> 1.50 */
}

/* ---- BoxTalk message handlers ---- */
/* Sensor uuids on this Toon. UUID_AMBIENT is the hardware sensor block on
 * the display (humidity + VOC + a *raw* temperature ~3°C above the actual
 * room temp). UUID_THERMSTAT is happ_thermstat itself, which publishes
 * the *calibrated* indoor temperature qt-gui shows — that's the one
 * users intuit as "the" room temperature, so it gets indoor_temp. */
#define UUID_AMBIENT    "87efe05a-14dc-40df-9769-1f2cf3ade4b7"
#define UUID_THERMSTAT  "b822de89-ecbd-4f6f-9fd2-5cac18fc06c4"
#define UUID_BOILER     "6ecc08b0-1e18-42d5-9725-46db009d1a00"

/* The happ_thermstat ThermostatInfo *publisher* uuid is DYNAMIC (regenerated each
 * happ start, e.g. e7b8a710-… / 4eb79be8-…) — NOT the hardcoded THERMSTAT_UUID we
 * use as a query destination. BoilerChPressure only arrives via a rare change-notify,
 * and the startup query to the stale hardcoded uuid never answers, so the pressure
 * tile sits at "-- bar" until a notify happens to fire. Learn the live uuid here and
 * re-query the pressure against it until it's seeded. */
static char g_therm_pub[64] = {0};
static int send_query(const char* destuuid, const char* service, const char* state_var);

static void handle_notify(const char* xml) {
    if (settings.client_mode) return;
    char sid[128];
    if (!attr_value(xml, "serviceid", sid, sizeof(sid))) return;
    char src_uuid[64] = {0};
    attr_value(xml, "uuid", src_uuid, sizeof(src_uuid));
    /* sid is full urn: extract the trailing part */
    const char* tail = strrchr(sid, ':');
    if (tail) tail++; else tail = sid;

    if (strstr(sid, "Flow") || strstr(sid, "Meter") || strstr(sid, "Usage") || strstr(sid, "usage") || strstr(sid, "Power") || strstr(sid, "power")) {
        fprintf(stderr, "[bxt-meter] notify sid='%s' tail='%s' uuid='%s': %.250s\n",
                sid, tail, src_uuid, xml);
    }

    if (strcmp(tail, "TemperatureSensor") == 0) {
        float v;
        if (elem_text_float(xml, "CurrentTemperature", &v) || elem_text_float(xml, "currentTemp", &v)) {
            /* Note: TemperatureSensor carries raw uncalibrated hardware sensor readings
             * (e.g. ambient display sensor running warm from internal SoC/screen heat).
             * Calibrated room temperature is published exclusively by happ_thermstat
             * via ThermostatInfo and the thermostatInfo dataset. Do NOT assign indoor_temp here. */
            if (strcmp(src_uuid, UUID_BOILER) == 0 || strstr(src_uuid, "boiler")) {
                toon_state.boiler_temp = v;
                toon_state.msg_count++;
            }
        }
    } else if (strcmp(tail, "HumiditySensor") == 0) {
        float v;
        if (elem_text_float(xml, "CurrentHumidity", &v)) {
            if (v > 0.0f && v <= 100.0f) {
                toon_state.humidity = v;
                toon_state.msg_count++;
            }
        }
    } else if (strcmp(tail, "vocSensor") == 0) {
        float v;
        if (elem_text_float(xml, "eco2", &v)) {
            toon_state.eco2 = (int)v;
            toon_state.msg_count++;
        }
        if (elem_text_float(xml, "tvoc", &v)) {
            toon_state.tvoc = (int)v;
            toon_state.msg_count++;
        }
    } else if (strcmp(tail, "ThermostatInfo") == 0) {
        float v;
        /* Learn the live (dynamic) thermostat publisher uuid, but do not let
         * a boiler device (e.g. otgw / HappBoiler) stomp on the thermostat. */
        if (src_uuid[0] && !strstr(src_uuid, "otgw") && strcmp(src_uuid, UUID_BOILER) != 0 && strncmp(src_uuid, "e7b8a710", 8) != 0) {
            snprintf(g_therm_pub, sizeof g_therm_pub, "%s", src_uuid);
            static int seed_tries = 0;
            if (toon_state.water_pressure <= 0.05f && seed_tries < 10) {
                seed_tries++;
                send_query(g_therm_pub, "ThermostatInfo", "BoilerChPressure");
            }
        }
        /* Indoor room temperature — happ_thermstat (UUID_THERMSTAT) pushes
         * this on every measurable change. Was being silently dropped on
         * this notify path; without it the dim/home screens fell back to
         * whatever indoor_temp got seeded with at boot and drifted out of
         * date. Accept both centi-°C (happ's internal: "1889") and °C
         * already-decimalised, since some firmwares use the larger value. */
        /* Accept currentTemp from any ThermostatInfo publisher — that service
         * is happ_thermstat's own calibrated room temp. (The uncalibrated
         * hardware sensor publishes TemperatureSensor, a different service, so
         * there's no risk of picking it up here.) The previous hardcoded
         * src_uuid filter matched only one Toon, leaving indoor_temp at 0 (and
         * the PWA/home tile showing "--") on every other device. */
        if (elem_text_float(xml, "currentTemp", &v) ||
            elem_text_float(xml, "CurrentTemperature", &v) ||
            elem_text_float(xml, "currentDisplayTemp", &v)) {
            toon_state.indoor_temp = (v > 80.0f) ? v / 100.0f : v;
            toon_state.msg_count++;
        }
        if (elem_text_float(xml, "currentSetpoint", &v)) {
            toon_state.setpoint = v / 100.0f;  /* setpoint is centidegrees */
            toon_state.msg_count++;
        }
        if (elem_text_float(xml, "burnerInfo", &v)) {
            update_burner_state((int)v);
            toon_state.msg_count++;
        }
        if (elem_text_float(xml, "BoilerChPressure", &v)) {
            /* Auto-detect bar, decibar, or centibar by magnitude. */
            if (v > 0.05f)
                set_water_pressure(v);
            toon_state.msg_count++;
        }
    } else if (strcasecmp(tail, "ElectricityFlowMeter") == 0 ||
               strcasecmp(tail, "PowerUsage") == 0) {
        /* Official energy source: happ_pwrusage publishes the live smart-meter
         * power here (W). Drives the Energy tile when energy_source=meteradapter
         * and the Adapters tile's online/offline state. */
        float v = 0.0f;
        int got = 0;
        if (elem_text_float(xml, "CurrentElectricityFlow", &v) ||
            elem_text_float(xml, "currentElectricityFlow", &v) ||
            elem_text_float(xml, "value", &v) ||
            elem_text_float(xml, "power", &v)) {
            got = 1;
        }
        if (got) {
            fprintf(stderr, "[bxt-meter] flow accepted: %.1f W (svc='%s' uuid='%s')\n", (double)v, tail, src_uuid);
            meteradapter_on_flow(v);
            toon_state.msg_count++;
        } else {
            fprintf(stderr, "[bxt-meter] warning: matched service '%s' but no flow tag found in xml: %.350s\n", tail, xml);
        }
    } else if (strcasecmp(tail, "ElectricityQuantityMeter") == 0) {
        /* Analog electricity meter (pulse counter / optical sensor) */
        float q = 0.0f;
        int got = 0;
        if (elem_text_float(xml, "CurrentElectricityQuantity", &q) ||
            elem_text_float(xml, "currentElectricityQuantity", &q) ||
            elem_text_float(xml, "value", &q)) {
            got = 1;
        }
        if (got) {
            fprintf(stderr, "[bxt-meter] analog pulse notify: quantity=%.3f kWh (svc='%s')\n", (double)q, tail);
            toon_state.msg_count++;
            extern void probe_happ_pwrusage(void);
            probe_happ_pwrusage();
        }
    } else if (tile_slots_integration_by_service(tail) != NULL) {
        /* Marketplace integration — dispatched via the manifest's value_field
         * / subtitle_field. tile_slots_on_notify updates the latest-value
         * cache the home-tile renderer reads on the next refresh.
         * Match on the stripped `tail` (bare serviceId), not the full URN
         * `sid`, like every other branch — the manifest's service_id is bare,
         * so using `sid` here never matched and the crypto tile stayed empty. */
        tile_slots_on_notify(tail, xml);
    } else if (strcmp(tail, "BoilerInfo") == 0) {
        /* Dump the *full* XML the first 3 times so we can confirm the exact
         * element names this Toon's happ_thermstat publishes. The previous
         * truncated %.400s log let us miss the body — the values sit past
         * a long enum prefix. */
        static int boiler_log_n = 0;
        if (boiler_log_n < 3) {
            boiler_log_n++;
            fprintf(stderr, "[bxt] BoilerInfo notify (full): %s\n", xml);
        }
        float v;
        int got = 0;
        if (elem_text_float(xml, "CurrentBoilerTemperature", &v) ||
            elem_text_float(xml, "currentBoilerTemperature", &v) ||
            elem_text_float(xml, "boilerTemp", &v)) {
            toon_state.boiler_out_temp = v;
            toon_state.msg_count++;
            got |= 1;
        }
        if (elem_text_float(xml, "CurrentBoilerReturnTemperature", &v) ||
            elem_text_float(xml, "currentBoilerReturnTemperature", &v) ||
            elem_text_float(xml, "boilerReturnTemp", &v)) {
            toon_state.boiler_in_temp = v;
            toon_state.msg_count++;
            got |= 2;
        }
        if (elem_text_float(xml, "ControlSetpoint", &v)) {
            toon_state.ch_setpoint = v;
            toon_state.msg_count++;
            got |= 4;
        }
        if (boiler_log_n <= 3) {
            fprintf(stderr, "[bxt] BoilerInfo parsed: got_mask=0x%x  out=%.2f in=%.2f chsp=%.2f\n",
                    got, (double)toon_state.boiler_out_temp,
                    (double)toon_state.boiler_in_temp,
                    (double)toon_state.ch_setpoint);
        }
    }
}

static char last_query_var[64] = {0};

static void handle_query_response(const char* xml) {
    if (settings.client_mode) return;
    /* QueryStateVariableResponse wraps the value as <VarName>VAL</VarName>.
       Identify the variable by element name in the response itself — much
       more robust than tracking last_query_var, since multiple queries can
       be in flight concurrently. */
    char sid[128];
    if (!attr_value(xml, "serviceid", sid, sizeof(sid))) return;
    const char* tail = strrchr(sid, ':');
    if (tail) tail++; else tail = sid;

    float v;
    if (strcmp(tail, "ThermostatInfo") == 0) {
        if (elem_text_float(xml, "CurrentSetpoint", &v)) {
            toon_state.setpoint = (v > 100.0f) ? v / 100.0f : v;
            toon_state.msg_count++;
            fprintf(stderr, "[bxt] query response CurrentSetpoint = %.2f\n", v);
            return;
        }
        /* happ_thermstat publishes the calibrated room temperature here.
         * Seeds indoor_temp until the first TemperatureSensor notify
         * arrives (and acts as a fallback for hardware-sensor outages). */
        if (elem_text_float(xml, "CurrentTemperature", &v) ||
            elem_text_float(xml, "currentTemp", &v) ||
            elem_text_float(xml, "currentDisplayTemp", &v)) {
            if (v > 0.0f) toon_state.indoor_temp = (v > 80.0f) ? v / 100.0f : v;
            toon_state.msg_count++;
            fprintf(stderr, "[bxt] query response CurrentTemperature = %.2f\n", toon_state.indoor_temp);
            return;
        }
        if (elem_text_float(xml, "BoilerChPressure", &v)) {
            if (v > 0.05f)
                set_water_pressure(v);
            toon_state.msg_count++;
            fprintf(stderr, "[bxt] query response BoilerChPressure = %.2f (stored %.2f)\n", v, toon_state.water_pressure);
            return;
        }
    }
    /* vocSensor responses to our startup seed-query — same element names
     * as the notify path. Keeps the home tile from showing "TVOC --" /
     * "-- ppm" during the multi-minute window before happ emits its
     * first change-notify. */
    if (strcmp(tail, "vocSensor") == 0) {
        if (elem_text_float(xml, "tvoc", &v)) {
            /* This sensor's seed-query returns tvoc=0 even when the live value
             * (via change-notify) is non-zero — don't let the 0 clobber it,
             * or the home tile and the Stats TVOC graph stay flat at zero. */
            if (v > 0) {
                toon_state.tvoc = (int)v;
                toon_state.msg_count++;
            }
            fprintf(stderr, "[bxt] query response tvoc = %d%s\n",
                    (int)v, v > 0 ? "" : " (ignored, keeping live value)");
            return;
        }
        if (elem_text_float(xml, "eco2", &v)) {
            toon_state.eco2 = (int)v;
            toon_state.msg_count++;
            fprintf(stderr, "[bxt] query response eco2 = %d\n", toon_state.eco2);
            return;
        }
    }
    /* Boiler flow/return temps. Element name carries the variable, so match
       on it regardless of serviceid tail. Try the names seen in the binary. */
    if (elem_text_float(xml, "boilerTemps", &v) ||
        elem_text_float(xml, "CurrentBoilerTemperature", &v) ||
        elem_text_float(xml, "boilerTemp", &v)) {
        toon_state.boiler_out_temp = v;
        toon_state.msg_count++;
        fprintf(stderr, "[bxt] query response boiler flow temp = %.2f\n", v);
        return;
    }
    if (elem_text_float(xml, "boilerRetTemps", &v) ||
        elem_text_float(xml, "CurrentBoilerReturnTemperature", &v) ||
        elem_text_float(xml, "boilerRetTemp", &v)) {
        toon_state.boiler_in_temp = v;
        toon_state.msg_count++;
        fprintf(stderr, "[bxt] query response boiler return temp = %.2f\n", v);
        return;
    }
    fprintf(stderr, "[bxt] query response (unhandled): %.300s\n", xml);
}

static void handle_msg(const char* xml) {
    const char* p = xml;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '<') return;
    p++;
    boxtalk_lock();
    if (strstr(xml, "thermostatSettings")) {
        const char * ts = strstr(xml, "thermostatSettings");
        const char * dev = ts;
        while (dev > xml && strncmp(dev, "<device", 7) != 0) dev--;
        if (strncmp(dev, "<device", 7) == 0) {
            char u[96];
            if (attr_value(dev, "uuid", u, sizeof u)) {
                snprintf(s_therm_settings_uuid, sizeof s_therm_settings_uuid, "%s", u);
                fprintf(stderr, "[bxt] learned thermostatSettings uuid = %s\n", s_therm_settings_uuid);
            }
        }
    }
    if (strncmp(p, "notify", 6) == 0) {
        handle_notify(xml);
    } else if (strncmp(p, "query", 5) == 0) {
        if (strstr(xml, "class=\"response\"")) {
            handle_query_response(xml);
        }
    } else if (strncmp(p, "action", 6) == 0) {
        /* happ_usermsg notifications dataset — log the payload so we can
           reverse-engineer the entry shape for the Inbox UI. */
        if (strstr(xml, "happ_usermsg") && strstr(xml, "UpdateDataSet")
            && strstr(xml, "notifications")) {
            inbox_parse_dataset(xml);
        }
        /* happ_thermstat thermostatInfo dataset — the native source for the
         * calibrated room temp + humidity (the same push qt-gui consumes).
         * currentTemp/currentSetpoint are centi-°C; currentHumidity is %. */
        if (!settings.client_mode && strstr(xml, "happ_thermstat") && strstr(xml, "UpdateDataSet")
            && strstr(xml, "thermostatInfo")) {
            float v;
            if ((elem_text_float(xml, "currentTemp", &v) ||
                 elem_text_float(xml, "currentDisplayTemp", &v) ||
                 elem_text_float(xml, "CurrentTemperature", &v)) && v > 0) {
                toon_state.indoor_temp = (v > 80.0f) ? v / 100.0f : v;
                toon_state.msg_count++;
            }
            if (elem_text_float(xml, "currentHumidity", &v) && v > 0)
                toon_state.humidity = v;
            if (elem_text_float(xml, "currentSetpoint", &v) && v > 0)
                toon_state.setpoint = v / 100.0f;
        }
        /* Could be a response to our SetSetpoint — extract setpoint from response */
        if (!settings.client_mode && (strstr(xml, "ManualSetpoint") || strstr(xml, "SetSetpoint") || strstr(xml, "Setpoint"))) {
            float v;
            if (elem_text_float(xml, "currentSetpoint", &v) || elem_text_float(xml, "CurrentSetpoint", &v)) {
                toon_state.setpoint = v / 100.0f;
                toon_state.msg_count++;
                fprintf(stderr, "[bxt] action response setpoint=%.2f\n", toon_state.setpoint);
            }
        }
        /* GetBoilerType / SetBoilerType response — both echo the boiler type
           back in an <ot> element (0=OpenTherm, 1=On/Off). Log raw the first
           few times so the verb/serviceid can be confirmed on-device. */
        if (!settings.client_mode && strstr(xml, "BoilerType")) {
            static int bt_log_n = 0;
            if (bt_log_n < 4) { bt_log_n++; fprintf(stderr, "[bxt] BoilerType msg: %.400s\n", xml); }
            float v;
            if (elem_text_float(xml, "ot", &v) || elem_text_float(xml, "boilerType", &v)) {
                toon_state.boiler_type = (int)v;
                fprintf(stderr, "[bxt] boiler_type = %d (%s)\n",
                        toon_state.boiler_type,
                        toon_state.boiler_type == 1 ? "On/Off" : "OpenTherm");
            }
        }
        /* GetRraData response — stash so the stats screen can parse it */
        if (strstr(xml, "GetRraDataResponse") && strstr(xml, "class=\"response\"")) {
            size_t n = strlen(xml);
            if (n >= sizeof(rrd_response_buf)) n = sizeof(rrd_response_buf) - 1;
            memcpy(rrd_response_buf, xml, n);
            rrd_response_buf[n] = 0;
            rrd_response_ready = 1;
            fprintf(stderr, "[bxt] GetRraData response %zu bytes\n", n);
        }
        /* hdrv_zwave GetDevices response — capture the device set for the
         * Z-Wave screen. Match on the device-list markers so we catch it
         * regardless of the exact response verb wrapper. */
        if ((strstr(xml, "GetDevices") || strstr(xml, "supportedCC") ||
             strstr(xml, "nodeIdInfo")) && strstr(xml, "class=\"response\"")) {
            size_t n = strlen(xml);
            if (n >= sizeof(zwave_response_buf)) n = sizeof(zwave_response_buf) - 1;
            memcpy(zwave_response_buf, xml, n);
            zwave_response_buf[n] = 0;
            zwave_response_ready = 1;
            fprintf(stderr, "[bxt] ZWAVE response %zu bytes\n", n);
        }
        /* hcb_netcon wireless responses — capture for the WiFi screen. */
        if ((strstr(xml, "WirelessNetwork") || strstr(xml, "GetWireless")) &&
            strstr(xml, "class=\"response\"")) {
            size_t n = strlen(xml);
            if (n >= sizeof(netcon_response_buf)) n = sizeof(netcon_response_buf) - 1;
            memcpy(netcon_response_buf, xml, n);
            netcon_response_buf[n] = 0;
            netcon_response_ready = 1;
        }
        /* ConfigProvider response — capture for schedule / config parser */
        if ((strstr(xml, "GetObjectConfig") || strstr(xml, "GetPackageConfig") ||
             strstr(xml, "SetObjectConfig") || strstr(xml, "ConfigProvider")) &&
            strstr(xml, "class=\"response\"")) {
            size_t n = strlen(xml);
            if (n >= sizeof(config_response_buf)) n = sizeof(config_response_buf) - 1;
            memcpy(config_response_buf, xml, n);
            config_response_buf[n] = 0;
            config_response_ready = 1;
            fprintf(stderr, "[bxt] ConfigProvider response %zu bytes\n", n);
        }
    }
    boxtalk_unlock();
}

/* ---- RRD GetRraData request/response plumbing ---- */
volatile int rrd_response_ready = 0;
char         rrd_response_buf[16384];

/* ---- hdrv_zwave (built-in Z-Wave controller) plumbing ---- */
volatile int zwave_response_ready = 0;
char         zwave_response_buf[16384];

/* Build + send a hdrv_zwave action on serviceId specific1. `verb` is the
 * U-element name; `inner` is the (already-escaped) child-element payload. */
static int zwave_action(const char * verb, const char * inner, const char * tag) {
    char xml[1024];
    snprintf(xml, sizeof xml,
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:specific1\" requestid=\"%d-%s\">"
        "<u:%s xmlns:u=\"urn:hcb-hae-com:service:specific1:1\">%s</u:%s></action>",
        OUR_UUID, ZWAVE_UUID, getpid(), tag, verb, inner ? inner : "", verb);
    return send_msg(xml);
}

/* Request the full Z-Wave device set. Response captured in zwave_response_buf
 * (poll zwave_response_ready, clear by writing 0). */
int boxtalk_zwave_get_devices(void) {
    zwave_response_ready = 0;
    return zwave_action("GetDevices", NULL, "zwd");
}

/* Network heal — routing/neighbour update for all nodes. */
int boxtalk_zwave_heal(void) {
    return zwave_action("NodeNeighborUpdate", "<timeout>300</timeout>", "zwh");
}

/* Inclusion (add). start!=0 begins add mode; start==0 stops it. */
int boxtalk_zwave_include(int start) {
    if (start)
        return zwave_action("IncludeDevice",
            "<event>add</event><lowPower>0</lowPower><timeout>60</timeout>", "zwi");
    return zwave_action("IncludeDevice", "<event>stop</event>", "zwi");
}

/* Exclusion (remove). start!=0 begins delete mode; start==0 stops it. */
int boxtalk_zwave_exclude(int start) {
    if (start)
        return zwave_action("ExcludeDevice",
            "<event>delete</event><timeout>30</timeout>", "zwe");
    return zwave_action("ExcludeDevice", "<event>stop</event>", "zwe");
}

/* ---- hcb_netcon (WiFi / network manager) plumbing ---- */
volatile int netcon_response_ready = 0;
char         netcon_response_buf[16384];

static int netcon_action(const char * verb, const char * inner, const char * tag) {
    char xml[1024];
    snprintf(xml, sizeof xml,
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:NetworkInformation\" requestid=\"%d-%s\">"
        "<u:%s xmlns:u=\"urn:hcb-hae-com:service:NetworkInformation:1\">%s</u:%s></action>",
        OUR_UUID, NETCON_UUID, getpid(), tag, verb, inner ? inner : "", verb);
    return send_msg(xml);
}

/* Current wireless connection info (read-only — no scan). */
int boxtalk_wifi_get_status(void) {
    netcon_response_ready = 0;
    return netcon_action("GetWirelessNetworkInformation", NULL, "wfi");
}

/* Trigger a scan + return the visible networks. NOTE: briefly disrupts the
 * wireless interface (hcb_netcon re-inits to scan). */
int boxtalk_wifi_scan(void) {
    netcon_response_ready = 0;
    return netcon_action("GetWirelessNetworks", NULL, "wfs");
}

/* Connect to an SSID. key may be empty for open networks. ssid/key must be
 * pre-sanitised (no XML metacharacters). */
int boxtalk_wifi_connect(const char * ssid, const char * key) {
    char inner[256];
    snprintf(inner, sizeof inner,
        "<SSID>%s</SSID><EncryptionKey>%s</EncryptionKey>", ssid, key ? key : "");
    return netcon_action("SetWirelessNetworkInformation", inner, "wfc");
}

/* Disconnect / clear the current wireless config (hcb_netcon ResetWirelessNetwork).
 * NOTE: on a WiFi-only Toon this drops it off the network until reconnected. */
int boxtalk_wifi_disconnect(void) {
    return netcon_action("ResetWirelessNetwork", NULL, "wfx");
}

/* Switch a binary device on (1) / off (0). Targets the device uuid. */
int boxtalk_zwave_basic_set(const char * uuid, int state) {
    char inner[160];
    snprintf(inner, sizeof inner, "<uuid>%s</uuid><state>%d</state>", uuid, state ? 1 : 0);
    return zwave_action("basicSet", inner, "zwb");
}

/* Rename a device. name must be pre-sanitised (no XML metacharacters). */
int boxtalk_zwave_set_name(const char * uuid, const char * name) {
    char inner[320];
    snprintf(inner, sizeof inner, "<uuid>%s</uuid><deviceName>%s</deviceName><name>%s</name>", uuid, name, name);
    return zwave_action("SetDeviceName", inner, "zwn");
}

int boxtalk_send_raw_xml(const char * xml) {
    if (!xml || sock_fd < 0) return -1;
    return send_msg(xml);
}

/* Subscribe to an arbitrary BoxTalk service. Used by tile_slots_init() so
 * each marketplace integration's serviceId reaches handle_notify(). Safe to
 * call before connect — early calls just no-op (sock_fd<0); tile_slots_init
 * is called again after connect to flush. */
int boxtalk_subscribe_service(const char * service_id) {
    if (!service_id || !service_id[0]) return -1;
    if (sock_fd < 0) return -1;
    char buf[512];
    snprintf(buf, sizeof(buf),
        "<subscribe uuid=\"%s\" destuuid=\"\"><target uuid=\"\" "
        "serviceid=\"%s\"></target></subscribe>",
        OUR_UUID, service_id);
    fprintf(stderr, "[bxt] subscribing to %s\n", service_id);
    return send_msg(buf);
}

/* ---- hcb_config (ConfigProvider) plumbing ---- */
volatile int config_response_ready = 0;
char         config_response_buf[65536];

int boxtalk_get_object_config(const char * internal_address) {
    if (!internal_address || !internal_address[0] || sock_fd < 0) return -1;
    config_response_ready = 0;
    config_response_buf[0] = 0;
    char req[512];
    snprintf(req, sizeof req,
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:ConfigProvider\" requestid=\"%d-goc\">"
        "<u:GetObjectConfig xmlns:u=\"urn:hcb-hae-com:service:ConfigProvider:1\">"
        "<package>happ_thermstat</package>"
        "<internalAddress>%s</internalAddress>"
        "</u:GetObjectConfig></action>",
        OUR_UUID, CONFIG_UUID, getpid(), internal_address);
    return send_msg(req);
}

int boxtalk_set_object_config(const char * config_xml) {
    if (!config_xml || !config_xml[0] || sock_fd < 0) return -1;
    config_response_ready = 0;
    config_response_buf[0] = 0;
    size_t len = strlen(config_xml) + 512;
    char * req = malloc(len);
    if (!req) return -1;
    snprintf(req, len,
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:ConfigProvider\" requestid=\"%d-soc\">"
        "<u:SetObjectConfig xmlns:u=\"urn:hcb-hae-com:service:ConfigProvider:1\">"
        "<Config>%s</Config>"
        "</u:SetObjectConfig></action>",
        OUR_UUID, CONFIG_UUID, getpid(), config_xml);
    int rc = send_msg(req);
    free(req);
    return rc;
}

/* ---- Boiler control type (OpenTherm vs On/Off) ----
 * Reverse-engineered from /qmf/sbin/happ_thermstat (not stripped):
 *   - dispatch table verbs "GetBoilerType" / "SetBoilerType"
 *   - bxt_action_SetBoilerType reads an int action-arg "ot", stores it,
 *     and tail-calls updateBoilerType()
 *   - updateBoilerType: ot==1 takes the On/Off path (clears boiler error,
 *     seeds water pressure), ot==0 stays OpenTherm. It persists the package
 *     config and pokes the OpenTherm gateway — so SetBoilerType is a real
 *     write to the live boiler.
 * serviceid: the dispatch table groups GetBoilerType/SetBoilerType under the
 * "Thermostat" service (svc_index 7, action block at 0x14647c whose service
 * descriptor name string is "Thermostat") — NOT "ThermostatInfo" (queries)
 * and NOT "specific1" (that route returns "Not implemented"). */
int boxtalk_get_boiler_type(void) {
    if (!s_therm_settings_uuid[0]) bt_seed_thermostat_settings_uuid();
    const char * dest = s_therm_settings_uuid[0] ? s_therm_settings_uuid : (g_therm_pub[0] ? g_therm_pub : THERMD_UUID);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:Thermostat\" requestid=\"%d-gbt\">"
        "<u:GetBoilerType xmlns:u=\"urn:hcb-hae-com:service:Thermostat:1\">"
        "</u:GetBoilerType></action>",
        OUR_UUID, dest, getpid());
    return send_msg(buf);
}

int boxtalk_set_boiler_type(int type) {
    if (type != 0 && type != 1) return -1;
    if (!s_therm_settings_uuid[0]) bt_seed_thermostat_settings_uuid();
    const char * dest = s_therm_settings_uuid[0] ? s_therm_settings_uuid : (g_therm_pub[0] ? g_therm_pub : THERMD_UUID);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:Thermostat\" requestid=\"%d-sbt\">"
        "<u:SetBoilerType xmlns:u=\"urn:hcb-hae-com:service:Thermostat:1\">"
        "<ot>%d</ot></u:SetBoilerType></action>",
        OUR_UUID, dest, getpid(), type);
    int rc = send_msg(buf);
    fprintf(stderr, "[bxt] SetBoilerType ot=%d rc=%d (dest=%s)\n", type, rc, dest);
    /* Optimistic local update; GetBoilerType refresh confirms. */
    if (rc == 0) toon_state.boiler_type = type;
    boxtalk_get_boiler_type();
    return rc;
}

int boxtalk_get_rra_data(const char * uuid, const char * rra_name) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:specific1\" requestid=\"%d-rra\">"
        "<u:GetRraData xmlns:u=\"urn:hcb-hae-com:service:specific1:1\">"
        "<rraName>%s</rraName><uuid>%s</uuid>"
        "</u:GetRraData></action>",
        OUR_UUID, RRD_UUID, getpid(), rra_name, uuid);
    rrd_response_ready = 0;
    return send_msg(buf);
}

static int next_req = 1;

static int send_query(const char* destuuid, const char* service, const char* state_var) {
    snprintf(last_query_var, sizeof(last_query_var), "%s", state_var);
    char buf[1024];
    int req = __sync_fetch_and_add(&next_req, 1);
    snprintf(buf, sizeof(buf),
        "<query class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:%s\" requestid=\"%d-%d\">"
        "<u:QueryStateVariable xmlns:u=\"urn:hcb-hae-com:service:%s:1\">"
        "<varName>%s</varName>"
        "<requestId>%d-%d</requestId>"
        "<timeout>30</timeout>"
        "</u:QueryStateVariable></query>",
        OUR_UUID, destuuid, service, getpid(), req, service, state_var, getpid(), req);
    return send_msg(buf);
}

/* ---- protocol startup ---- */
static int connect_loop(void) {
    while (1) {
        sock_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd < 0) { sleep(2); continue; }
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_port = htons(1337);
        a.sin_addr.s_addr = htonl(0x7f000001);
        if (connect(sock_fd, (struct sockaddr*)&a, sizeof(a)) == 0) {
            fprintf(stderr, "[bxt] connected to 127.0.0.1:1337\n");
            return 0;
        }
        close(sock_fd); sock_fd = -1;
        fprintf(stderr, "[bxt] connect failed (errno=%d), retrying...\n", errno);
        sleep(2);
    }
}

static void send_initial_handshake(void) {
    char buf[2048];
    long now = (long)time(NULL);
    int pid = (int)getpid();

    /* discovery ssdp:connect */
    snprintf(buf, sizeof(buf),
        "<discovery nts=\"ssdp:connect\" uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:toonui\" version=\"v\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" sessionKey=\"%d-%ld\"></discovery>",
        OUR_UUID, pid, now);
    send_msg(buf);

    /* discovery ssdp:alive */
    snprintf(buf, sizeof(buf),
        "<discovery nts=\"ssdp:alive\" uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:toonui\" version=\"v\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" sessionKey=\"%d-%ld\"></discovery>",
        OUR_UUID, pid, now);
    send_msg(buf);

    /* subscribe to TemperatureSensor */
    snprintf(buf, sizeof(buf),
        "<subscribe uuid=\"%s\" destuuid=\"\"><target uuid=\"\" serviceid=\"urn:hcb-hae-com:serviceId:TemperatureSensor\"></target></subscribe>",
        OUR_UUID);
    send_msg(buf);

    /* subscribe to HumiditySensor */
    snprintf(buf, sizeof(buf),
        "<subscribe uuid=\"%s\" destuuid=\"\"><target uuid=\"\" serviceid=\"urn:hcb-hae-com:serviceId:HumiditySensor\"></target></subscribe>",
        OUR_UUID);
    send_msg(buf);

    /* subscribe to vocSensor */
    snprintf(buf, sizeof(buf),
        "<subscribe uuid=\"%s\" destuuid=\"\"><target uuid=\"\" serviceid=\"urn:hcb-hae-com:serviceId:vocSensor\"></target></subscribe>",
        OUR_UUID);
    send_msg(buf);

    /* subscribe to ThermostatInfo */
    snprintf(buf, sizeof(buf),
        "<subscribe uuid=\"%s\" destuuid=\"\"><target uuid=\"\" serviceid=\"urn:hcb-hae-com:serviceId:ThermostatInfo\"></target></subscribe>",
        OUR_UUID);
    send_msg(buf);

    /* subscribe to BoilerInfo so quby_bridge's ControlSetpoint notify (DID 1
     * → "what happ_thermstat is asking the boiler to heat to") reaches us.
     * Without this the burner badge can only show "Heating" with no target. */
    snprintf(buf, sizeof(buf),
        "<subscribe uuid=\"%s\" destuuid=\"\"><target uuid=\"\" serviceid=\"urn:hcb-hae-com:serviceId:BoilerInfo\"></target></subscribe>",
        OUR_UUID);
    send_msg(buf);

    /* subscribe to ElectricityFlowMeter — happ_pwrusage publishes the live
     * smart-meter power (the official "meteradapter" energy source). */
    snprintf(buf, sizeof(buf),
        "<subscribe uuid=\"%s\" destuuid=\"\"><target uuid=\"\" serviceid=\"urn:hcb-hae-com:serviceId:ElectricityFlowMeter\"></target></subscribe>",
        OUR_UUID);
    send_msg(buf);

    /* Also subscribe to PowerUsage in case happ_pwrusage publishes under PowerUsage */
    snprintf(buf, sizeof(buf),
        "<subscribe uuid=\"%s\" destuuid=\"\"><target uuid=\"\" serviceid=\"urn:hcb-hae-com:serviceId:PowerUsage\"></target></subscribe>",
        OUR_UUID);
    send_msg(buf);

    /* Also subscribe to ElectricityQuantityMeter for analog optical pulse meters */
    snprintf(buf, sizeof(buf),
        "<subscribe uuid=\"%s\" destuuid=\"\"><target uuid=\"\" serviceid=\"urn:hcb-hae-com:serviceId:ElectricityQuantityMeter\"></target></subscribe>",
        OUR_UUID);
    send_msg(buf);

    /* Subscribe to happ_thermstat's thermostatInfo dataset. This is how the
     * stock qt-gui gets the calibrated room temperature + humidity (verified
     * by sniffing qt-gui's BoxTalk traffic): happ_thermstat pushes an
     * UpdateDataSet <thermostatInfo><currentTemp>NNNN</currentTemp>
     * <currentHumidity>NN</currentHumidity>... on this subscription. The
     * TemperatureSensor service only carries the uncalibrated ambient sensor
     * (~3°C high), and there's no notify/queryable CurrentTemperature on the
     * thermostat service — so this dataset is the real native source. */
    snprintf(buf, sizeof(buf),
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:specific1\">"
        "<u:UpdateDataSetSubscription xmlns:u=\"urn:hcb-hae-com:service:specific1:1\">"
        "<updateAction>add</updateAction><dataSet>thermostatInfo</dataSet>"
        "<autoExpire>0</autoExpire><supportsNaN>1</supportsNaN><supportsPartial>0</supportsPartial>"
        "<requestFullSet>1</requestFullSet></u:UpdateDataSetSubscription></action>",
        OUR_UUID, THERMD_UUID);
    send_msg(buf);

    /* Subscribe to happ_usermsg notifications dataset (Inbox). */
    snprintf(buf, sizeof(buf),
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:specific1\">"
        "<u:UpdateDataSetSubscription xmlns:u=\"urn:hcb-hae-com:service:specific1:1\">"
        "<updateAction>add</updateAction><dataSet>notifications</dataSet>"
        "<autoExpire>0</autoExpire><supportsNaN>1</supportsNaN><supportsPartial>0</supportsPartial>"
        "<requestFullSet>1</requestFullSet></u:UpdateDataSetSubscription></action>",
        OUR_UUID, USERMSG_UUID);
    send_msg(buf);

    /* Active queries to force immediate values (instead of waiting for sensor change) */
    /* reSendAllNotifies — qt-gui sends this at startup to force all daemons to republish */
    snprintf(buf, sizeof(buf),
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:specific1\">"
        "<u:reSendAllNotifies xmlns:u=\"urn:hcb-hae-com:service:specific1:1\"></u:reSendAllNotifies></action>",
        OUR_UUID);
    send_msg(buf);

    /* Query current setpoint (not emitted as notify) */
    send_query("b822de89-ecbd-4f6f-9fd2-5cac18fc06c4", "ThermostatInfo", "CurrentSetpoint");
    /* Seed tvoc + eco2 from the vocSensor. happ_thermstat only emits
     * vocSensor notifies on CHANGE — and TVOC is typically stable enough
     * that the first notify can take 5+ minutes to arrive. Without a
     * startup query the home tile shows "TVOC --" for that whole window.
     * The query response goes through the same parser path as the notify
     * (handle_query_response → vocSensor branch). */
    send_query("efbb5c4f-7b5e-4aa8-8ab5-6b5ab204ceaa", "vocSensor", "tvoc");
    send_query("efbb5c4f-7b5e-4aa8-8ab5-6b5ab204ceaa", "vocSensor", "eco2");
    /* Query calibrated room temperature — happ_thermstat owns this value;
     * the raw hardware-sensor reading is ~3°C high so we don't use it. */
    send_query(THERMD_UUID, "ThermostatInfo", "CurrentTemperature");
    send_query("b822de89-ecbd-4f6f-9fd2-5cac18fc06c4", "ThermostatInfo", "CurrentTemperature");
    /* Query boiler CH water pressure (notifies are rare; this seeds the value
       and the http poll keeps it fresh). */
    send_query("b822de89-ecbd-4f6f-9fd2-5cac18fc06c4", "ThermostatInfo", "BoilerChPressure");
    /* Query boiler CH water pressure (notifies are rare; this seeds the value
       and the http poll keeps it fresh). */
    send_query("b822de89-ecbd-4f6f-9fd2-5cac18fc06c4", "ThermostatInfo", "BoilerChPressure");

    /* Seed the boiler control type (OpenTherm vs On/Off) for the Settings UI. */
    boxtalk_get_boiler_type();
    /* Boiler flow/return temps — BoilerInfo is query-only (no notifies). */
    boxtalk_request_boiler_refresh();

    /* Marketplace integrations — broker drops subscribes across reconnect,
     * so this runs every handshake. tile_slots_init() must already have
     * populated the registry from main.c. */
    tile_slots_subscribe_all();

    /* Pro-actively query WiFi status so hcb_netcon is stimulated on boot */
    boxtalk_wifi_get_status();

    fprintf(stderr, "[bxt] handshake + subscriptions sent\n");
}

void boxtalk_request_setpoint_refresh(void) {
    send_query("b822de89-ecbd-4f6f-9fd2-5cac18fc06c4", "ThermostatInfo", "CurrentSetpoint");
}

/* Pull a fresh indoor temperature read directly from happ_thermstat.
   The TemperatureSensor notify subscription only fires on a state CHANGE
   (and happ_thermstat throttles internally), so the dim screen could
   show a value many minutes old when room temp drifts slowly. The
   periodic poll loop + wake-screen handler call this to force a fresh
   value into toon_state.indoor_temp. */
void boxtalk_request_indoor_refresh(void) {
    if (g_therm_pub[0])
        send_query(g_therm_pub, "ThermostatInfo", "CurrentTemperature");
    send_query(THERMD_UUID, "ThermostatInfo", "CurrentTemperature");
    send_query(THERMSTAT_UUID, "ThermostatInfo", "CurrentTemperature");
}

void boxtalk_request_boiler_refresh(void) {
    /* The boilerDev/BoilerInfo path rejects CurrentBoiler* names. The boiler
       flow/return temps are exposed on happ_thermstat's ThermostatInfo
       service under the plural names seen in the binary. */
    send_query(THERMSTAT_UUID, "ThermostatInfo", "boilerTemps");
    send_query(THERMSTAT_UUID, "ThermostatInfo", "boilerRetTemps");
}

/* happ_thermstat's BoxTalk Set... and Manual... setpoint verbs return
   "Not implemented". The live setpoint path is the HTTP API on
   localhost:10080. We open a raw TCP socket and write a minimal
   HTTP/1.0 GET. No libcurl dependency.
   If out_body != NULL, writes up to out_max-1 bytes of the HTTP body
   (after the blank line) and NUL-terminates. */
static int http_get_thermstat_full(const char* path_with_query, char* out_body, size_t out_max) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(10080);
    a.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) { close(s); return -1; }
    char req[512];
    int n = snprintf(req, sizeof(req),
        "GET /happ_thermstat?%s HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        path_with_query);
    if (send(s, req, n, 0) != n) { close(s); return -1; }

    /* Drain full response. If out_body requested, accumulate up to out_max-1 bytes
       and then locate the body (after \r\n\r\n). */
    char accum[4096];
    size_t acclen = 0;
    while (1) {
        if (acclen >= sizeof(accum) - 1) break;
        ssize_t k = recv(s, accum + acclen, sizeof(accum) - 1 - acclen, 0);
        if (k <= 0) break;
        acclen += (size_t)k;
    }
    accum[acclen] = 0;
    close(s);

    if (out_body && out_max > 0) {
        const char* body = strstr(accum, "\r\n\r\n");
        if (body) body += 4; else body = accum;
        size_t blen = strlen(body);
        if (blen >= out_max) blen = out_max - 1;
        memcpy(out_body, body, blen);
        out_body[blen] = 0;
    }
    return 0;
}

static int http_get_thermstat(const char* path_with_query) {
    return http_get_thermstat_full(path_with_query, NULL, 0);
}

/* Tiny JSON value parser: finds "key": NNN or "key":"NNN" and returns atoi. */
static int parse_json_int(const char* json, const char* key, int dflt) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return dflt;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\"') p++;
    return atoi(p);
}


static void* http_poll_thread(void* arg) {
    (void)arg;
    char body[2048];
    while (1) {
        if (http_get_thermstat_full("action=getThermostatInfo", body, sizeof(body)) == 0) {
            toon_state.program_state = parse_json_int(body, "programState", toon_state.program_state);
            toon_state.active_state  = parse_json_int(body, "activeState",  toon_state.active_state);
            /* Also refresh indoor temp from canonical HTTP view — prevents initial
             * "--.-°C" or "..." delay after boot when waiting for BoxTalk change-notifies. */
            int ct = parse_json_int(body, "currentTemp", 0);
            if (ct <= 0) ct = parse_json_int(body, "currentDisplayTemp", 0);
            if (ct > 0) {
                toon_state.indoor_temp = (ct > 80) ? ct / 100.0f : (float)ct;
                toon_state.msg_count++;
            }
            int hum = parse_json_int(body, "currentHumidity", 0);
            if (hum > 0) toon_state.humidity = (float)hum;
            /* Also refresh setpoint from the canonical HTTP view in case a
               concurrent client changed it (cloud, web). Value is centi-°C. */
            int sp = parse_json_int(body, "currentSetpoint", 0);
            if (sp > 0) toon_state.setpoint = sp / 100.0f;
            /* OpenTherm health for the Heating settings modal. */
            toon_state.modulation_level = parse_json_int(body, "currentModulationLevel", toon_state.modulation_level);
            toon_state.ot_comm_error    = parse_json_int(body, "otCommError", toon_state.ot_comm_error);
            /* Burner state: needed for home/dim flame + faucet icons. The
             * BoxTalk path doesn't deliver this reliably — quby_bridge
             * publishes burnerInfo on ThermostatInfo from a HappBoiler-
             * registered UUID and HCB silently drops cross-service
             * notifies; happ_thermstat doesn't publish it as a notify
             * either (only on QueryStateVariable response). HTTP poll is
             * the canonical reliable path on this Toon. */
            int bi = parse_json_int(body, "burnerInfo", -2);
            if (bi >= 0) {
                update_burner_state(bi);
            }
            /* Stooklijn target — same reasoning as burnerInfo. */
            int chsp = parse_json_int(body, "currentInternalBoilerSetpoint", -1);
            if (chsp >= 0) toon_state.ch_setpoint = (float)chsp;
        }
        /* Same story for indoor temperature: happ_thermstat doesn't publish
         * a TemperatureSensor or ThermostatInfo notify on CurrentTemperature
         * change (verified by tailing /tmp/quby_bridge-shaped notifies — only
         * BoilerInfo arrives). Use the BoxTalk query mechanism, which is the
         * protocol's intended path for query-only state-vars. A query is one
         * small XML round-trip; not the same as HTTP polling. */
        /* Periodically fetch central heating water pressure from hcb_rrd (logger thermstat_boilerChPressure) */
        static int press_poll_cnt = 0;
        if (++press_poll_cnt >= 5 || toon_state.water_pressure <= 0.05f) {
            press_poll_cnt = 0;
            char rrd_buf[256];
            int rrc = http_fetch_to("http://127.0.0.1:10080/hcb_rrd?action=getRrdData&loggerName=thermstat_boilerChPressure&rra=30days&samples=1",
                                    rrd_buf, sizeof rrd_buf, 2);
            if (rrc != 0) {
                rrc = http_fetch_to("http://127.0.0.1/hcb_rrd?action=getRrdData&loggerName=thermstat_boilerChPressure&rra=30days&samples=1",
                                    rrd_buf, sizeof rrd_buf, 2);
            }
            if (rrc == 0 && !strstr(rrd_buf, "\"error\"")) {
                const char * col = NULL;
                const char * k1 = strchr(rrd_buf, '"');
                if (k1) {
                    const char * k2 = strchr(k1 + 1, '"');
                    if (k2) col = strchr(k2 + 1, ':');
                }
                if (!col) col = strchr(rrd_buf, ':');
                if (col) {
                    col++;
                    while (*col == ' ' || *col == '\t') col++;
                    if (strncmp(col, "NaN", 3) != 0 && strncmp(col, "nan", 3) != 0 && strncmp(col, "null", 4) != 0) {
                        float bp = (float)atof(col);
                        if (bp > 0.05f) {
                            set_water_pressure(bp);
                        }
                    }
                }
            }
        }

        /* 3 s — used to be 15 s, but that was the source of "the flame icon
         * lights up half a minute after the boiler kicks in" complaints.
         * burner_on / dhw_on / active_state / setpoint all ride this poll;
         * happ_thermstat is the local loopback so the cost is negligible. */
        canonical_state_notify(CSTATE_EVT_THERMOSTAT | CSTATE_EVT_BOILER);
        sleep(3);
    }
    return NULL;
}

#ifdef SIM
static int sim_preset_temps[4] = { 2050, 1950, 1500, 1700 };
#endif

int boxtalk_set_setpoint(float temp) {
    int centi = (int)(temp * 100.0f + 0.5f);
    if (centi < 500)  centi = 500;   /* 5°C floor */
    if (centi > 3000) centi = 3000;  /* 30°C cap  */
#ifdef SIM
    toon_state.setpoint = centi / 100.0f;
    toon_state.msg_count++;
    extern void canonical_state_notify(uint32_t);
    canonical_state_notify(CSTATE_EVT_THERMOSTAT);
    return 0;
#endif
#ifdef WASM_BUILD
    /* WASM client: same /api/setpoint endpoint the slave bridge already
     * has, just routed through the JS fetch shim (wasm_push_event) instead
     * of popen("curl"). Optimistically update local toon_state so the UI
     * reacts instantly; the next SSE frame will overwrite with the truth. */
    extern void wasm_push_event(const char *, const char *);
    char body[48];
    snprintf(body, sizeof body, "{\"value\":\"%.2f\"}", (double)(centi/100.0f));
    wasm_push_event("/api/setpoint", body);
    toon_state.setpoint = centi / 100.0f;
    return 0;
#endif
    if (settings.client_mode) {      /* slave: hand the write to the master */
        int rc = client_link_setpoint(centi / 100.0f);
        if (rc == 0) toon_state.setpoint = centi / 100.0f;
        return rc;
    }
    char q[64];
    snprintf(q, sizeof(q), "action=roomSetpoint&Setpoint=%d", centi);
    int rc = http_get_thermstat(q);
    fprintf(stderr, "[bxt] HTTP roomSetpoint=%d rc=%d\n", centi, rc);
    /* optimistic local update so UI feels instant; BoxTalk query will confirm */
    if (rc == 0) toon_state.setpoint = centi / 100.0f;
    boxtalk_request_setpoint_refresh();
    return rc;
}

/* Per-preset temperatures. Each scheme state (0=Comfort, 1=Home, 2=Sleep,
 * 3=Away) has a stored room-setpoint in happ_thermstat — that's what the
 * schedule daemon snaps to when it transitions. Returns centi-°C
 * (1850 = 18.50 °C) or -1 on error / unknown response. */
int boxtalk_get_state_value(int state) {
    if (state < 0 || state > 3) return -1;
#ifdef SIM
    return sim_preset_temps[state];
#endif
    char body[256];
    char q[64];
    snprintf(q, sizeof(q), "action=getStateValue&stateId=%d", state);
    if (settings.client_mode) {
        char url[256];
        snprintf(url, sizeof url, "http://%s:10080/happ_thermstat?%s", settings.master_host, q);
        extern int http_fetch_to(const char *, char *, size_t, int);
        if (http_fetch_to(url, body, sizeof body, 3) != 0) return -1;
    } else {
        if (http_get_thermstat_full(q, body, sizeof(body)) != 0) return -1;
    }
    const char * p = strstr(body, "stateValue");
    if (!p) return -1;
    /* skip "stateValue": then optional space + : (handles both ":1850" and ": 1850") */
    p += strlen("stateValue");
    while (*p && (*p == '"' || *p == ':' || *p == ' ')) p++;
    return atoi(p);
}

int boxtalk_set_state_value(int state, int centi) {
    if (state < 0 || state > 3) return -1;
    if (centi < 500)  centi = 500;
    if (centi > 3000) centi = 3000;
#ifdef SIM
    sim_preset_temps[state] = centi;
    return 0;
#endif
    char q[80];
    snprintf(q, sizeof(q),
             "action=updateStateValue&stateId=%d&newValue=%d", state, centi);
    int rc;
    if (settings.client_mode) {
        char url[256], body[256];
        snprintf(url, sizeof url, "http://%s:10080/happ_thermstat?%s", settings.master_host, q);
        extern int http_fetch_to(const char *, char *, size_t, int);
        rc = http_fetch_to(url, body, sizeof body, 3);
    } else {
        rc = http_get_thermstat(q);
    }
    fprintf(stderr, "[bxt] updateStateValue state=%d val=%d rc=%d\n",
            state, centi, rc);
    return rc;
}

/* happ_thermstat scheme MODES — the `state=` parameter of changeSchemeState.
 * Reverse-engineered from Eneco's own stock UI (/qmf/www/mobile/javascript/
 * mobile.js, setProgramState/changeSchemeState). This is the scheme MODE, NOT
 * the comfort preset: changeSchemeState takes BOTH `state=<mode>` AND
 * `temperatureState=<preset 0..3>`. Verified live on a Toon 2: only with both
 * params does the preset's setpoint actually apply (Comfort 1950 / Home 1850 /
 * Sleep 1750 / Away 1600). The old code sent only `state=<presetIndex>`, which
 * jammed the preset into the mode field — so presets 2/3 silently no-op'd and
 * the setpoint never followed. */
#define PROG_MANUAL        0   /* manual hold, no schedule */
#define PROG_BASE          1   /* follow the weekly program */
#define PROG_TEMPOVERRIDE  2   /* override to a preset until the next switch */
#define PROG_LOCKEDBASE    8   /* override held indefinitely (2nd tap of preset) */

int boxtalk_set_program(int preset) {
    if (preset < 0 || preset > 3) return -1;   /* preset = comfort level 0..3 */
    int mode;
#ifdef SIM
    if      (toon_state.program_state == PROG_LOCKEDBASE) mode = PROG_TEMPOVERRIDE;
    else if (toon_state.program_state == PROG_MANUAL)     mode = PROG_TEMPOVERRIDE;
    else if (toon_state.active_state  == preset)          mode = PROG_LOCKEDBASE;
    else                                                  mode = PROG_TEMPOVERRIDE;
    toon_state.active_state  = preset;
    toon_state.program_state = mode;
    toon_state.setpoint      = sim_preset_temps[preset] / 100.0f;
    toon_state.msg_count++;
    extern void canonical_state_notify(uint32_t);
    canonical_state_notify(CSTATE_EVT_THERMOSTAT | CSTATE_EVT_BOILER);
    return 0;
#endif
#ifdef WASM_BUILD
    /* WASM client: POST the preset to the master, which runs the mode logic. */
    extern void wasm_push_event(const char *, const char *);
    char body[24];
    snprintf(body, sizeof body, "{\"state\":%d}", preset);
    wasm_push_event("/api/program", body);
    toon_state.active_state = preset;   /* activeState = the live comfort preset */
    return 0;
#endif
    if (settings.client_mode) {      /* slave: hand the preset to the master */
        int rc = client_link_program(preset);
        if (rc == 0) toon_state.active_state = preset;
        return rc;
    }
    /* Tapping a preset from MANUAL resumes Program mode (TEMPOVERRIDE):
     *   if  programState == LOCKEDBASE -> TEMPOVERRIDE   (a new choice unlocks)
     *   elif programState == MANUAL    -> TEMPOVERRIDE   (activate schedule override)
     *   elif activeState  == preset    -> LOCKEDBASE     (2nd tap of same = lock)
     *   else                           -> TEMPOVERRIDE   (override until next switch) */
    if      (toon_state.program_state == PROG_LOCKEDBASE) mode = PROG_TEMPOVERRIDE;
    else if (toon_state.program_state == PROG_MANUAL)     mode = PROG_TEMPOVERRIDE;
    else if (toon_state.active_state  == preset)          mode = PROG_LOCKEDBASE;
    else                                                  mode = PROG_TEMPOVERRIDE;

    char q[80];
    snprintf(q, sizeof(q),
             "action=changeSchemeState&state=%d&temperatureState=%d", mode, preset);
    int rc = http_get_thermstat(q);
    fprintf(stderr, "[bxt] changeSchemeState mode=%d temperatureState=%d rc=%d\n",
            mode, preset, rc);
    /* Optimistic: reflect both the preset AND the new mode at once so the
     * highlight/program-toggle update immediately, before the next poll. */
    if (rc == 0) {
        toon_state.active_state  = preset;
        toon_state.program_state = mode;
    }
    clear_temp_override();
    return rc;
}

int boxtalk_set_manual(void) {
#ifdef SIM
    clear_temp_override();
    toon_state.program_state = PROG_MANUAL;
    toon_state.active_state  = -1;
    toon_state.msg_count++;
    extern void canonical_state_notify(uint32_t);
    canonical_state_notify(CSTATE_EVT_THERMOSTAT);
    return 0;
#endif
    if (settings.client_mode) {
        int rc = client_link_program(-1);
        if (rc == 0) {
            clear_temp_override();
            toon_state.program_state = PROG_MANUAL;
            toon_state.active_state  = -1;
        }
        return rc;
    }
    /* "Manual": leave the schedule and hold indefinitely (programState=0).
     * Verified on a Toon 2: a bare roomSetpoint write makes a TEMP override
     * (programState=2, reverts at the next schedule switch), NOT a manual hold.
     * The canonical manual is changeSchemeState&state=0. temperatureState holds
     * the preset whose stored temp the user is already on (≈ current setpoint);
     * happ jumps the setpoint to it. */
    clear_temp_override();
    int ts = (toon_state.active_state >= 0 && toon_state.active_state <= 3)
                 ? toon_state.active_state : 1;
    char q[80];
    snprintf(q, sizeof(q),
             "action=changeSchemeState&state=%d&temperatureState=%d", PROG_MANUAL, ts);
    int rc = http_get_thermstat(q);
    fprintf(stderr, "[bxt] set_manual changeSchemeState state=0 ts=%d rc=%d\n", ts, rc);
    if (rc == 0) {
        toon_state.program_state = PROG_MANUAL;   /* 0, optimistic */
        toon_state.active_state  = ts;            /* keep current preset active */
    }
    return rc;
}

int boxtalk_resume_schedule(void) {
    clear_temp_override();
    int now_state = schedule_program_now();
#ifdef SIM
    toon_state.program_state = PROG_BASE;
    if (now_state < 0 || now_state > 3) now_state = 1;
    toon_state.active_state  = now_state;
    toon_state.setpoint      = sim_preset_temps[now_state] / 100.0f;
    toon_state.msg_count++;
    extern void canonical_state_notify(uint32_t);
    canonical_state_notify(CSTATE_EVT_THERMOSTAT | CSTATE_EVT_BOILER);
    return 0;
#endif
    if (settings.client_mode) {
        extern int client_link_resume_schedule(void);
        return client_link_resume_schedule();
    }
    /* "Program/Auto": resume the weekly schedule (programState=1, PROG_BASE).
     * Verified on a Toon 2: state=1 repopulates nextProgram/nextState/nextTime
     * and activeState from the schedule. set_program would instead pick an
     * override mode (2/8), so send PROG_BASE directly. */
    if (now_state < 0) now_state = 1;     /* fall back to Home if the schedule is unknown */
    char q[80];
    snprintf(q, sizeof(q),
             "action=changeSchemeState&state=%d&temperatureState=%d", PROG_BASE, now_state);
    int rc = http_get_thermstat(q);
    fprintf(stderr, "[bxt] resume_schedule changeSchemeState state=1 ts=%d rc=%d\n", now_state, rc);
    if (rc == 0) toon_state.program_state = PROG_BASE;   /* 1, optimistic */
    return rc;
}

/* +/- on the home tile = temporary override that auto-reverts to the
 * schedule at the next switch point.
 *
 * If in manual mode (program_state == PROG_MANUAL), setpoint adjustments remain
 * manual. When in program mode (scheduled or preset override), adjusting the
 * setpoint keeps the active comfort preset highlighted and retains program
 * mode with the new setpoint until the weekly schedule reaches the next switch point. */
static void note_temp_override(void) {
    if (toon_state.program_state == PROG_MANUAL) return;

    if (!temp_override_active) {
        /* Capture the preset that was active (0=Comfort, 1=Home, 2=Sleep, 3=Away).
         * If active_state is already parked (-1), fall back to whatever the schedule says is current now. */
        int origin = (toon_state.active_state >= 0 && toon_state.active_state <= 3)
                         ? toon_state.active_state
                         : schedule_program_now();
        if (origin < 0) origin = 1;   /* fallback to Home if no schedule loaded */
        temp_override_origin = origin;
        temp_override_sched_entry = schedule_current_entry_index();
        temp_override_active = 1;
        fprintf(stderr, "[bxt] note_temp_override: origin=%d sched_entry=%d\n",
                temp_override_origin, temp_override_sched_entry);
    }
}

int boxtalk_setpoint_increase(void) {
    note_temp_override();
    float cur = toon_state.setpoint;
    if (cur < 5.0f) cur = 17.0f;  /* fallback if not yet known */
    return boxtalk_set_setpoint(cur + 0.5f);
}

int boxtalk_setpoint_decrease(void) {
    note_temp_override();
    float cur = toon_state.setpoint;
    if (cur < 5.0f) cur = 17.0f;
    return boxtalk_set_setpoint(cur - 0.5f);
}

void boxtalk_tick(void) {
    static int wifi_poll_ticks = 0;
    if (++wifi_poll_ticks >= 30) {
        wifi_poll_ticks = 0;
        boxtalk_wifi_get_status();
    }
    if (!temp_override_active) return;

    /* If manual mode, clear temporary override */
    if (toon_state.program_state == PROG_MANUAL) {
        clear_temp_override();
        return;
    }

    /* If server naturally returned to scheduled base state with valid preset,
     * the override has finished on the server side */
    if (toon_state.program_state == PROG_BASE && toon_state.active_state >= 0) {
        clear_temp_override();
        return;
    }

    /* When the weekly schedule advances past the switch point that was active
     * at the moment of override, expire the override and resume the schedule. */
    int cur_entry = schedule_current_entry_index();
    if (temp_override_sched_entry >= 0 && cur_entry >= 0 && cur_entry != temp_override_sched_entry) {
        fprintf(stderr, "[bxt] temp override expired at switch point (entry %d -> %d), resuming schedule\n",
                temp_override_sched_entry, cur_entry);
        clear_temp_override();
        boxtalk_resume_schedule();
    }
}

int boxtalk_temp_override_active(void) { return temp_override_active; }

int boxtalk_temp_override_origin(void) {
    return temp_override_active ? temp_override_origin : -1;
}

/* ---- background thread ---- */
static void* bxt_thread(void* arg) {
    (void)arg;
    while (1) {
        if (connect_loop() != 0) { sleep(2); continue; }
        if (!settings.client_mode) toon_state.connected = 1;
        rxlen = 0;
        send_initial_handshake();

        while (1) {
            if (rxlen >= sizeof(rxbuf) - 1) {
                fprintf(stderr, "[bxt] rxbuf full — flushing\n");
                rxlen = 0;
            }
            ssize_t n = recv(sock_fd, rxbuf + rxlen, sizeof(rxbuf) - rxlen - 1, 0);
            if (n <= 0) {
                fprintf(stderr, "[bxt] recv ret=%zd errno=%d — reconnecting\n", n, errno);
                break;
            }
            rxlen += (size_t)n;
            rxbuf[rxlen] = 0;

            /* process NUL-delimited messages */
            size_t scan = 0;
            while (scan < rxlen) {
                char* nul = (char*)memchr(rxbuf + scan, 0, rxlen - scan);
                if (!nul) break;  /* no full message yet */
                *nul = 0;
                if (rxbuf + scan != nul) {  /* non-empty */
                    handle_msg(rxbuf + scan);
                }
                scan = (size_t)(nul - rxbuf) + 1;
            }
            /* shift remainder */
            if (scan > 0) {
                memmove(rxbuf, rxbuf + scan, rxlen - scan);
                rxlen -= scan;
            }
        }
        close(sock_fd); sock_fd = -1;
        if (!settings.client_mode) toon_state.connected = 0;
        sleep(1);
    }
    return NULL;
}

int boxtalk_start(void) {
    bt_init_dev_uuids();
    pthread_t th;
    if (pthread_create(&th, NULL, bxt_thread, NULL) != 0) return -1;
    pthread_detach(th);
    if (!settings.client_mode) {
        pthread_t poll;
        if (pthread_create(&poll, NULL, http_poll_thread, NULL) == 0)
            pthread_detach(poll);
    }
    return 0;
}
