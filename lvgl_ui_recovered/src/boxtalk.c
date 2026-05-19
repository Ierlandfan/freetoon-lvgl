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
#include "inbox.h"
#include "schedule.h"
#include "settings.h"
#include "tile_slots.h"

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
                            .boiler_type = -1 };

/* Temporary-override bookkeeping. Read by program_label() at the top of
 * the file as well as by the +/- nudge and tick paths further down, so
 * the declarations live up here. See note_temp_override() for semantics. */
static int temp_override_active = 0;
static int temp_override_origin = -1;   /* program at moment of override */

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
    /* activeState == -1 → manual override. Anything else → schedule is in
     * the driver's seat and program_state holds the currently-active preset.
     * UI surfaces it as "Scheduled: Home" so the *mode* (manual vs. on the
     * schedule) is one tap away from being obvious. Returns a pointer into
     * a static buffer; safe because LVGL copies labels on set_text. */
    /* Short label suitable for ambient/dim use: just the active preset
     * name, or "Manual" when off the schedule. The home tile renders
     * the override-aware "(temp)" hint via its own logic next to the
     * mode-toggle button instead of bolting it onto this label. */
    int origin = -1;
    if (temp_override_active && temp_override_origin >= 0 &&
                                temp_override_origin <= 3)
        origin = temp_override_origin;
    int preset_idx = (toon_state.active_state >= 0)
                         ? toon_state.program_state : origin;
    if (preset_idx < 0) return "Manual";
    switch (preset_idx) {
        case 0: return "Comfort";
        case 1: return "Home";
        case 2: return "Sleep";
        case 3: return "Away";
        default: return "Scheduled";
    }
}

static int sock_fd = -1;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static const char* OUR_UUID = "qb-659918000101-2011A0LOHI:toonui";

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
    fprintf(stderr, "[bxt] sent %zu bytes\n", strlen(xml));
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

/* ---- BoxTalk message handlers ---- */
/* Sensor uuids on this Toon. UUID_AMBIENT is the hardware sensor block on
 * the display (humidity + VOC + a *raw* temperature ~3°C above the actual
 * room temp). UUID_THERMSTAT is happ_thermstat itself, which publishes
 * the *calibrated* indoor temperature qt-gui shows — that's the one
 * users intuit as "the" room temperature, so it gets indoor_temp. */
#define UUID_AMBIENT    "87efe05a-14dc-40df-9769-1f2cf3ade4b7"
#define UUID_THERMSTAT  "b822de89-ecbd-4f6f-9fd2-5cac18fc06c4"
#define UUID_BOILER     "6ecc08b0-1e18-42d5-9725-46db009d1a00"

static void handle_notify(const char* xml) {
    char sid[128];
    if (!attr_value(xml, "serviceid", sid, sizeof(sid))) return;
    char src_uuid[64] = {0};
    attr_value(xml, "uuid", src_uuid, sizeof(src_uuid));
    /* (quiet — was: fprintf notify log) */

    /* sid is full urn: extract the trailing part */
    const char* tail = strrchr(sid, ':');
    if (tail) tail++; else tail = sid;

    if (strcmp(tail, "TemperatureSensor") == 0) {
        float v;
        if (elem_text_float(xml, "CurrentTemperature", &v)) {
            /* happ_thermstat publishes the calibrated room temperature here.
             * The hardware-sensor UUID_AMBIENT is a separate (uncalibrated)
             * source — ignored for indoor_temp because it reads ~3°C high. */
            if (strcmp(src_uuid, UUID_THERMSTAT) == 0) {
                toon_state.indoor_temp = v;
                toon_state.msg_count++;
            } else if (strcmp(src_uuid, UUID_BOILER) == 0) {
                toon_state.boiler_temp = v;
            }
        }
    } else if (strcmp(tail, "HumiditySensor") == 0) {
        float v;
        if (elem_text_float(xml, "CurrentHumidity", &v)) {
            if (strcmp(src_uuid, UUID_AMBIENT) == 0) {
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
        /* Indoor room temperature — happ_thermstat (UUID_THERMSTAT) pushes
         * this on every measurable change. Was being silently dropped on
         * this notify path; without it the dim/home screens fell back to
         * whatever indoor_temp got seeded with at boot and drifted out of
         * date. Accept both centi-°C (happ's internal: "1889") and °C
         * already-decimalised, since some firmwares use the larger value. */
        if (strcmp(src_uuid, UUID_THERMSTAT) == 0 &&
            elem_text_float(xml, "currentTemp", &v)) {
            toon_state.indoor_temp = (v > 80.0f) ? v / 100.0f : v;
            toon_state.msg_count++;
        }
        if (elem_text_float(xml, "currentSetpoint", &v)) {
            toon_state.setpoint = v / 100.0f;  /* setpoint is centidegrees */
            toon_state.msg_count++;
        }
        if (elem_text_float(xml, "burnerInfo", &v)) {
            /* 0=off, 1=heating, 2=hot water, 3=preheat. Accept whoever
             * publishes — in wired mode that's happ_thermstat directly;
             * in wireless mode the quby_bridge republishes the same
             * field off OTGW. Either is canonical for "is the burner
             * firing right now". */
            toon_state.burner_on = (v == 1.0f);
            toon_state.dhw_on    = (v == 2.0f);
            toon_state.msg_count++;
        }
        if (elem_text_float(xml, "BoilerChPressure", &v)) {
            /* Some firmwares emit centibar (e.g. 180 = 1.80), others bar
               directly. Auto-detect by magnitude. happ_thermstat keeps
               pushing 0 even when DID 18 arrives over the Quby serial,
               so swallow zero updates — keep the OTGW-bridge value. */
            if (v > 0.05f)
                toon_state.water_pressure = (v > 10.0f) ? v / 100.0f : v;
            toon_state.msg_count++;
        }
    } else if (tile_slots_integration_by_service(sid) != NULL) {
        /* Marketplace integration — dispatched via the manifest's value_field
         * / subtitle_field. tile_slots_on_notify updates the latest-value
         * cache the home-tile renderer reads on the next refresh. */
        tile_slots_on_notify(sid, xml);
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
        if (elem_text_float(xml, "CurrentTemperature", &v)) {
            if (v > 0.0f) toon_state.indoor_temp = v;
            toon_state.msg_count++;
            fprintf(stderr, "[bxt] query response CurrentTemperature = %.2f\n", v);
            return;
        }
        if (elem_text_float(xml, "BoilerChPressure", &v)) {
            /* happ_thermstat keeps answering 0.00 on this query path even
               though DID 18 over Quby gives 1.10 bar. Drop the zero so the
               OTGW-bridge ThermostatInfo notify wins instead. */
            if (v > 0.05f)
                toon_state.water_pressure = (v > 10.0f) ? v / 100.0f : v;
            toon_state.msg_count++;
            fprintf(stderr, "[bxt] query response BoilerChPressure = %.2f\n", v);
            return;
        }
    }
    /* vocSensor responses to our startup seed-query — same element names
     * as the notify path. Keeps the home tile from showing "TVOC --" /
     * "-- ppm" during the multi-minute window before happ emits its
     * first change-notify. */
    if (strcmp(tail, "vocSensor") == 0) {
        if (elem_text_float(xml, "tvoc", &v)) {
            toon_state.tvoc = (int)v;
            toon_state.msg_count++;
            fprintf(stderr, "[bxt] query response tvoc = %d\n", toon_state.tvoc);
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
    /* Log non-notify messages so we can see what's happening */
    if (strncmp(p+1, "notify", 6) != 0) {
        fprintf(stderr, "[bxt] msg: %.500s\n---\n", xml);
    }
    p++;
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
        /* Could be a response to our SetSetpoint — extract setpoint from response */
        if (strstr(xml, "ManualSetpoint") || strstr(xml, "SetSetpoint") || strstr(xml, "Setpoint")) {
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
        if (strstr(xml, "BoilerType")) {
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
    }
}

/* ---- RRD GetRraData request/response plumbing ---- */
volatile int rrd_response_ready = 0;
char         rrd_response_buf[16384];

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
    char buf[512];
    snprintf(buf, sizeof(buf),
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:Thermostat\" requestid=\"%d-gbt\">"
        "<u:GetBoilerType xmlns:u=\"urn:hcb-hae-com:service:Thermostat:1\">"
        "</u:GetBoilerType></action>",
        OUR_UUID, THERMSTAT_UUID, getpid());
    return send_msg(buf);
}

int boxtalk_set_boiler_type(int type) {
    if (type != 0 && type != 1) return -1;
    char buf[512];
    snprintf(buf, sizeof(buf),
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:Thermostat\" requestid=\"%d-sbt\">"
        "<u:SetBoilerType xmlns:u=\"urn:hcb-hae-com:service:Thermostat:1\">"
        "<ot>%d</ot></u:SetBoilerType></action>",
        OUR_UUID, THERMSTAT_UUID, getpid(), type);
    int rc = send_msg(buf);
    fprintf(stderr, "[bxt] SetBoilerType ot=%d rc=%d\n", type, rc);
    /* Optimistic local update; GetBoilerType refresh confirms. */
    if (rc == 0) toon_state.boiler_type = type;
    boxtalk_get_boiler_type();
    return rc;
}

int boxtalk_get_rra_data(const char * uuid, const char * rra_name) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"qb-659918000101-2011A0LOHI:hcb_rrd\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:specific1\" requestid=\"%d-rra\">"
        "<u:GetRraData xmlns:u=\"urn:hcb-hae-com:service:specific1:1\">"
        "<rraName>%s</rraName><uuid>%s</uuid>"
        "</u:GetRraData></action>",
        OUR_UUID, getpid(), rra_name, uuid);
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

    /* Subscribe to happ_usermsg notifications dataset (Inbox). */
    snprintf(buf, sizeof(buf),
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"qb-659918000101-2011A0LOHI:happ_usermsg\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:specific1\">"
        "<u:UpdateDataSetSubscription xmlns:u=\"urn:hcb-hae-com:service:specific1:1\">"
        "<updateAction>add</updateAction><dataSet>notifications</dataSet>"
        "<autoExpire>0</autoExpire><supportsNaN>1</supportsNaN><supportsPartial>0</supportsPartial>"
        "<requestFullSet>1</requestFullSet></u:UpdateDataSetSubscription></action>",
        OUR_UUID);
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

/* Tiny JSON value parser: finds "key":"NNN" and returns atoi. */
static int parse_json_int(const char* json, const char* key, int dflt) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char* p = strstr(json, needle);
    if (!p) return dflt;
    p += strlen(needle);
    return atoi(p);
}


static void* http_poll_thread(void* arg) {
    (void)arg;
    char body[2048];
    while (1) {
        if (http_get_thermstat_full("action=getThermostatInfo", body, sizeof(body)) == 0) {
            toon_state.program_state = parse_json_int(body, "programState", toon_state.program_state);
            toon_state.active_state  = parse_json_int(body, "activeState",  toon_state.active_state);
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
                toon_state.burner_on = (bi == 1);
                toon_state.dhw_on    = (bi == 2);
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
        boxtalk_request_indoor_refresh();
        /* 3 s — used to be 15 s, but that was the source of "the flame icon
         * lights up half a minute after the boiler kicks in" complaints.
         * burner_on / dhw_on / active_state / setpoint all ride this poll;
         * happ_thermstat is the local loopback so the cost is negligible. */
        sleep(3);
    }
    return NULL;
}

int boxtalk_set_setpoint(float temp) {
    int centi = (int)(temp * 100.0f + 0.5f);
    if (centi < 500)  centi = 500;   /* 5°C floor */
    if (centi > 3000) centi = 3000;  /* 30°C cap  */
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
    char body[256];
    char q[64];
    snprintf(q, sizeof(q), "action=getStateValue&stateId=%d", state);
    if (http_get_thermstat_full(q, body, sizeof(body)) != 0) return -1;
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
    char q[80];
    snprintf(q, sizeof(q),
             "action=updateStateValue&stateId=%d&newValue=%d", state, centi);
    int rc = http_get_thermstat(q);
    fprintf(stderr, "[bxt] updateStateValue state=%d val=%d rc=%d\n",
            state, centi, rc);
    return rc;
}

int boxtalk_set_program(int state) {
    if (state < 0 || state > 3) return -1;
    char q[64];
    snprintf(q, sizeof(q), "action=changeSchemeState&state=%d", state);
    int rc = http_get_thermstat(q);
    fprintf(stderr, "[bxt] HTTP changeSchemeState=%d rc=%d\n", state, rc);
    /* Optimistic: we'll see authoritative state on next poll. */
    if (rc == 0) {
        toon_state.program_state = state;
        toon_state.active_state  = state;
    }
    return rc;
}

int boxtalk_set_manual(void) {
    /* "Manual" in the UI: leave the schedule and hold the setpoint
     * indefinitely. happ_thermstat only flips activeState to -1 when
     * roomSetpoint writes a *different* value than the current one — a
     * same-value write returns "ok" and silently no-ops, so tapping
     * Manual right after Program (which had already pinned the setpoint
     * to the preset's stored value) used to leave the device on the
     * schedule. Nudge by 1 centi-°C (0.01 °C, below display resolution)
     * to guarantee the write engages manual. */
    temp_override_active = 0;
    float sp = toon_state.setpoint > 0 ? toon_state.setpoint : 18.0f;
    int centi = (int)(sp * 100.0f + 0.5f);
    centi += (centi < 3000) ? 1 : -1;     /* +0.01 °C unless we're at the cap */
    int rc = boxtalk_set_setpoint(centi / 100.0f);
    if (rc == 0) toon_state.active_state = -1;
    return rc;
}

int boxtalk_resume_schedule(void) {
    /* "Follow the schedule again." happ_thermstat doesn't have a verb
     * for "resume schedule" — instead we ask for the preset the schedule
     * says is currently active, which puts activeState back ≥0 and lets
     * the schedule daemon progress normally from there. */
    temp_override_active = 0;
    int now_state = schedule_program_now();
    if (now_state < 0) now_state = 1;     /* fall back to Home if no schedule */
    return boxtalk_set_program(now_state);
}

/* +/- on the home tile = temporary override that auto-reverts to the
 * schedule at the next switch point. We can't ask happ_thermstat to do
 * this — roomSetpoint by itself flips to active_state=-1 (indefinite
 * manual). Workaround: remember the program the schedule was on when
 * the user first nudged the setpoint, then in boxtalk_tick() (called
 * from the UI refresh) watch for schedule_program_now() to change and
 * auto-call resume_schedule().
 *
 * The "Off" button (boxtalk_set_manual above) clears this flag — that's
 * the explicit "no, I really want a permanent hold" path. */
static void note_temp_override(void) {
    if (!temp_override_active) {
        /* First nudge — capture the preset that was active. If we're
         * already in manual (active_state < 0), fall back to whatever
         * the schedule says is current now; we still want the
         * auto-resume behaviour. */
        int origin = (toon_state.active_state >= 0)
                         ? toon_state.program_state
                         : schedule_program_now();
        if (origin < 0) return;   /* no schedule loaded — leave it manual */
        temp_override_origin = origin;
        temp_override_active = 1;
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
    /* Drive the temporary-override expiry from the UI's 1 s refresh.
     * When the weekly schedule advances past the program that was
     * active at the moment of the override, snap back to whatever the
     * schedule wants now and clear the flag. */
    if (!temp_override_active) return;
    int now_prog = schedule_program_now();
    if (now_prog < 0 || now_prog == temp_override_origin) return;
    temp_override_active = 0;
    boxtalk_resume_schedule();
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
        toon_state.connected = 1;
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
        toon_state.connected = 0;
        sleep(1);
    }
    return NULL;
}

int boxtalk_start(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, bxt_thread, NULL) != 0) return -1;
    pthread_detach(th);
    pthread_t poll;
    if (pthread_create(&poll, NULL, http_poll_thread, NULL) == 0)
        pthread_detach(poll);
    return 0;
}
