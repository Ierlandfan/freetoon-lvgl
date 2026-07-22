/*
 * freetoon -> Home Assistant push over MQTT auto-discovery.  See ha_mqtt.h.
 *
 * Self-contained MQTT 3.1.1 client (wire code modelled on ventilation.c's
 * proven publisher) so it can't disturb the working Itho path. Broker/creds
 * come from /mnt/data/mqtt.cfg ("host:user:pass") -- the same file the Itho
 * bridge reads -- because settings.mqtt_* is the marketplace *subscriber*
 * config and is empty on stock installs.
 */
#include "ha_mqtt.h"
#include "boxtalk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#define HA_KEEPALIVE_S   45
#define HA_PUBLISH_S     20          /* republish state at least this often */
#define MQTT_CFG         "/mnt/data/mqtt.cfg"

#define T_AVAIL     "freetoon/toon/availability"
#define T_STATE     "freetoon/toon/state"
#define T_CMD_SETP  "freetoon/toon/cmd/setpoint"
#define T_CMD_PRES  "freetoon/toon/cmd/preset"

/* Shared device block (HA groups all entities under one "Toon" device). */
#define HA_DEV "\"dev\":{\"ids\":[\"freetoon_toon\"],\"name\":\"Toon\"," \
               "\"mf\":\"Quby / freetoon\",\"mdl\":\"Toon\"}"

static char g_host[64] = "";
static char g_user[64] = "";
static char g_pass[64] = "";
static int  g_port     = 1883;

static const char * g_presets[4] = { "Comfort", "Home", "Sleep", "Away" };

/* ---- config ------------------------------------------------------------ */

static int load_cfg(void) {
    FILE * f = fopen(MQTT_CFG, "r");
    if (!f) return -1;
    char line[256];
    int ok = 0;
    if (fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        char * c1 = strchr(line, ':');
        if (c1) {
            *c1 = 0;
            char * c2 = strchr(c1 + 1, ':');
            if (c2) {
                *c2 = 0;
                /* host may carry ":port" -- but the cfg uses host:user:pass,
                 * so host is field 1 verbatim. */
                snprintf(g_host, sizeof(g_host), "%s", line);
                snprintf(g_user, sizeof(g_user), "%s", c1 + 1);
                snprintf(g_pass, sizeof(g_pass), "%s", c2 + 1);
                ok = 1;
            }
        }
    }
    fclose(f);
    return ok ? 0 : -1;
}

/* ---- MQTT 3.1.1 wire ---------------------------------------------------- */

static int enc_rl(unsigned int len, unsigned char * out) {
    int n = 0;
    do { unsigned char d = len & 0x7f; len >>= 7; if (len) d |= 0x80; out[n++] = d; } while (len);
    return n;
}
static int read_rl(int fd) {
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
static int put_str(unsigned char * b, const char * s) {
    int n = (int)strlen(s);
    b[0] = (n >> 8) & 0xff; b[1] = n & 0xff;
    memcpy(b + 2, s, n);
    return 2 + n;
}
static int read_full(int fd, unsigned char * b, int n) {
    int got = 0;
    while (got < n) { int r = read(fd, b + got, n - got); if (r <= 0) return -1; got += r; }
    return 0;
}

/* CONNECT with LWT = availability/offline (retained). */
static int mq_connect(int fd, const char * cid) {
    unsigned char vh[16], pl[512];
    int vhn = 0;
    vhn += put_str(vh + vhn, "MQTT");
    vh[vhn++] = 0x04;                                   /* level 3.1.1 */
    unsigned char flags = 0x02 | 0x04 | 0x20;           /* clean, will, will-retain */
    if (g_user[0]) flags |= 0x80;
    if (g_pass[0]) flags |= 0x40;
    vh[vhn++] = flags;
    vh[vhn++] = (HA_KEEPALIVE_S >> 8) & 0xff;
    vh[vhn++] = HA_KEEPALIVE_S & 0xff;

    int pln = 0;
    pln += put_str(pl + pln, cid);
    pln += put_str(pl + pln, T_AVAIL);
    pln += put_str(pl + pln, "offline");
    if (g_user[0]) pln += put_str(pl + pln, g_user);
    if (g_pass[0]) pln += put_str(pl + pln, g_pass);

    unsigned char fh[8];
    fh[0] = 0x10;
    int rln = enc_rl(vhn + pln, fh + 1);
    if (write(fd, fh, 1 + rln) != 1 + rln) return -1;
    if (write(fd, vh, vhn)     != vhn)     return -1;
    if (write(fd, pl, pln)     != pln)     return -1;

    unsigned char ca[4];
    if (read(fd, ca, 4) != 4) return -1;
    if (ca[0] != 0x20 || ca[3] != 0) {
        fprintf(stderr, "[ha_mqtt] CONNACK rejected type=0x%02x rc=%d\n", ca[0], ca[3]);
        return -1;
    }
    return 0;
}

/* PUBLISH (QoS 0), written in pieces so payload size is unbounded. */
static int mq_pub(int fd, const char * topic, const char * payload, int retain) {
    int tlen = (int)strlen(topic), plen = (int)strlen(payload);
    unsigned char fh[8], tb[2];
    fh[0] = 0x30 | (retain ? 0x01 : 0x00);
    int rln = enc_rl(2 + tlen + plen, fh + 1);
    tb[0] = (tlen >> 8) & 0xff; tb[1] = tlen & 0xff;
    if (write(fd, fh, 1 + rln) != 1 + rln) return -1;
    if (write(fd, tb, 2)       != 2)       return -1;
    if (write(fd, topic, tlen) != tlen)    return -1;
    if (plen && write(fd, payload, plen) != plen) return -1;
    return 0;
}

static int mq_sub(int fd, unsigned short pid, const char * topic) {
    unsigned char b[160]; int n = 0;
    b[n++] = (pid >> 8) & 0xff; b[n++] = pid & 0xff;
    n += put_str(b + n, topic);
    b[n++] = 0x00;                                      /* QoS 0 */
    unsigned char fh[8];
    fh[0] = 0x82;
    int rln = enc_rl(n, fh + 1);
    if (write(fd, fh, 1 + rln) != 1 + rln) return -1;
    if (write(fd, b, n)        != n)       return -1;
    return 0;
}
static int mq_ping(int fd) { unsigned char p[2] = { 0xc0, 0x00 }; return write(fd, p, 2) == 2 ? 0 : -1; }

/* ---- discovery ---------------------------------------------------------- */

static void pub_disc(int fd, const char * comp, const char * obj, const char * cfg) {
    char topic[96];
    snprintf(topic, sizeof(topic), "homeassistant/%s/%s/config", comp, obj);
    mq_pub(fd, topic, cfg, 1);
}

static void publish_discovery(int fd) {
    char b[1024];

    /* climate */
    snprintf(b, sizeof(b),
        "{\"name\":null,\"uniq_id\":\"freetoon_thermostat\","
        "\"avty_t\":\"" T_AVAIL "\",\"temp_unit\":\"C\","
        "\"min_temp\":6,\"max_temp\":30,\"temp_step\":0.5,\"modes\":[\"heat\"],"
        "\"mode_stat_t\":\"" T_STATE "\",\"mode_stat_tpl\":\"heat\","
        "\"curr_temp_t\":\"" T_STATE "\",\"curr_temp_tpl\":\"{{ value_json.indoor_temp }}\","
        "\"temp_stat_t\":\"" T_STATE "\",\"temp_stat_tpl\":\"{{ value_json.setpoint }}\","
        "\"temp_cmd_t\":\"" T_CMD_SETP "\","
        "\"act_t\":\"" T_STATE "\",\"act_tpl\":\"{{ value_json.action }}\","
        "\"pr_mode_stat_t\":\"" T_STATE "\",\"pr_mode_val_tpl\":\"{{ value_json.preset }}\","
        "\"pr_mode_cmd_t\":\"" T_CMD_PRES "\","
        "\"pr_modes\":[\"Comfort\",\"Home\",\"Sleep\",\"Away\"]," HA_DEV "}");
    pub_disc(fd, "climate", "freetoon_thermostat", b);

    /* analog sensors: obj, name, json-field, unit, device_class ("" = none) */
    static const char * S[][5] = {
        { "freetoon_boiler_flow",   "Ketel aanvoer",     "boiler_flow",   "°C",  "temperature" },
        { "freetoon_boiler_return", "Ketel retour",      "boiler_return", "°C",  "temperature" },
        { "freetoon_ch_pressure",   "CV waterdruk",      "pressure",      "bar", "pressure" },
        { "freetoon_ch_setpoint",   "CV doeltemperatuur","ch_setpoint",   "°C",  "temperature" },
        { "freetoon_modulation",    "Brander modulatie", "modulation",    "%",   "" },
        { "freetoon_humidity",      "Luchtvochtigheid",  "humidity",      "%",   "humidity" },
        { "freetoon_co2",           "CO2",               "co2",           "ppm", "carbon_dioxide" },
        { "freetoon_tvoc",          "TVOC",              "tvoc",          "ppb", "volatile_organic_compounds_parts" },
    };
    for (unsigned i = 0; i < sizeof(S) / sizeof(S[0]); i++) {
        char dc[64] = "";
        if (S[i][4][0]) snprintf(dc, sizeof(dc), "\"dev_cla\":\"%s\",", S[i][4]);
        snprintf(b, sizeof(b),
            "{\"name\":\"%s\",\"uniq_id\":\"%s\",\"stat_t\":\"" T_STATE "\","
            "\"val_tpl\":\"{{ value_json.%s }}\",\"unit_of_meas\":\"%s\",%s"
            "\"stat_cla\":\"measurement\",\"avty_t\":\"" T_AVAIL "\"," HA_DEV "}",
            S[i][1], S[i][0], S[i][2], S[i][3], dc);
        pub_disc(fd, "sensor", S[i][0], b);
    }

    /* binary sensors: obj, name, json-field, device_class */
    static const char * B[][4] = {
        { "freetoon_burner",   "Brander (CV)",   "burner",   "heat" },
        { "freetoon_dhw",      "Warm water",     "dhw",      "running" },
        { "freetoon_ot_error", "OpenTherm fout", "ot_error", "problem" },
    };
    for (unsigned i = 0; i < sizeof(B) / sizeof(B[0]); i++) {
        snprintf(b, sizeof(b),
            "{\"name\":\"%s\",\"uniq_id\":\"%s\",\"stat_t\":\"" T_STATE "\","
            "\"val_tpl\":\"{{ value_json.%s }}\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
            "\"dev_cla\":\"%s\",\"avty_t\":\"" T_AVAIL "\"," HA_DEV "}",
            B[i][1], B[i][0], B[i][2], B[i][3]);
        pub_disc(fd, "binary_sensor", B[i][0], b);
    }
}

/* ---- state -------------------------------------------------------------- */

static void publish_state(int fd) {
    const toon_state_t * t = &toon_state;
    int as = t->active_state;
    const char * preset = (as >= 0 && as <= 3) ? g_presets[as] : "None";
    const char * action = !t->connected ? "off" : (t->burner_on ? "heating" : "idle");
    char b[512];
    snprintf(b, sizeof(b),
        "{\"indoor_temp\":%.1f,\"setpoint\":%.1f,\"ch_setpoint\":%.1f,"
        "\"boiler_flow\":%.1f,\"boiler_return\":%.1f,\"pressure\":%.2f,"
        "\"modulation\":%.0f,\"humidity\":%.0f,\"co2\":%d,\"tvoc\":%d,"
        "\"burner\":\"%s\",\"dhw\":\"%s\",\"ot_error\":\"%s\","
        "\"preset\":\"%s\",\"action\":\"%s\"}",
        t->indoor_temp, t->setpoint, t->ch_setpoint,
        t->boiler_in_temp, t->boiler_out_temp, t->water_pressure,
        t->modulation_level, t->humidity, t->eco2, t->tvoc,
        t->burner_on ? "ON" : "OFF", t->dhw_on ? "ON" : "OFF",
        t->ot_comm_error ? "ON" : "OFF", preset, action);
    mq_pub(fd, T_STATE, b, 1);
}

/* ---- inbound commands --------------------------------------------------- */

static void handle_cmd(const char * topic, const char * payload, int plen) {
    char v[32];
    int n = plen < (int)sizeof(v) - 1 ? plen : (int)sizeof(v) - 1;
    memcpy(v, payload, n); v[n] = 0;
    if (strcmp(topic, T_CMD_SETP) == 0) {
        float tt = (float)atof(v);
        if (tt >= 4.0f && tt <= 35.0f) {
            boxtalk_set_setpoint(tt);
            fprintf(stderr, "[ha_mqtt] HA -> setpoint %.1f\n", tt);
        }
    } else if (strcmp(topic, T_CMD_PRES) == 0) {
        for (int i = 0; i < 4; i++) {
            if (strcasecmp(v, g_presets[i]) == 0) {
                boxtalk_set_program(i);
                fprintf(stderr, "[ha_mqtt] HA -> preset %s\n", g_presets[i]);
                return;
            }
        }
        if (strcasecmp(v, "none") == 0 || strcasecmp(v, "manual") == 0)
            boxtalk_set_manual();
    }
}

/* ---- connection loop ---------------------------------------------------- */

static void run_once(void) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(g_port);
    if (inet_pton(AF_INET, g_host, &addr.sin_addr) != 1) {
        struct hostent * he = gethostbyname(g_host);
        if (!he) { fprintf(stderr, "[ha_mqtt] resolve %s failed\n", g_host); return; }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };   /* short: drives timers */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[ha_mqtt] connect %s:%d: %s\n", g_host, g_port, strerror(errno));
        close(fd); return;
    }
    char cid[40];
    snprintf(cid, sizeof(cid), "freetoon-ha-%d", (int)getpid());
    if (mq_connect(fd, cid) != 0) { close(fd); return; }

    fprintf(stderr, "[ha_mqtt] connected %s as %s -> publishing discovery\n", g_host, g_user);
    mq_pub(fd, T_AVAIL, "online", 1);
    publish_discovery(fd);
    mq_sub(fd, 1, T_CMD_SETP);
    mq_sub(fd, 2, T_CMD_PRES);
    publish_state(fd);

    time_t next_pub  = time(NULL) + HA_PUBLISH_S;
    time_t next_ping = time(NULL) + (HA_KEEPALIVE_S - 5);

    while (1) {
        time_t now = time(NULL);
        if (now >= next_pub)  { publish_state(fd); next_pub  = now + HA_PUBLISH_S; }
        if (now >= next_ping) { if (mq_ping(fd) != 0) break; next_ping = now + (HA_KEEPALIVE_S - 5); }

        unsigned char fhdr;
        int r = read(fd, &fhdr, 1);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;   /* timeout -> loop */
        if (r <= 0) { fprintf(stderr, "[ha_mqtt] read hdr failed\n"); break; }
        int rlen = read_rl(fd);
        if (rlen < 0) break;
        unsigned char * pkt = malloc(rlen + 1);
        if (!pkt) break;
        if (rlen > 0 && read_full(fd, pkt, rlen) < 0) { free(pkt); break; }
        pkt[rlen] = 0;

        if ((fhdr & 0xf0) == 0x30) {                        /* PUBLISH */
            int qos = (fhdr & 0x06) >> 1;
            int tlen = (pkt[0] << 8) | pkt[1];
            if (tlen + 2 <= rlen) {
                char topic[96];
                int tn = tlen < (int)sizeof(topic) - 1 ? tlen : (int)sizeof(topic) - 1;
                memcpy(topic, pkt + 2, tn); topic[tn] = 0;
                int off = 2 + tlen + (qos > 0 ? 2 : 0);
                if (off <= rlen) handle_cmd(topic, (const char *)pkt + off, rlen - off);
            }
        }
        free(pkt);
    }
    /* clean-ish exit: mark offline (the retained LWT also covers hard drops) */
    mq_pub(fd, T_AVAIL, "offline", 1);
    close(fd);
}

static void * ha_mqtt_thread(void * arg) {
    (void)arg;
    for (;;) {
        if (load_cfg() == 0 && g_host[0]) run_once();
        sleep(5);                                          /* reconnect / retry */
    }
    return NULL;
}

int ha_mqtt_start(void) {
    if (load_cfg() != 0 || !g_host[0]) {
        fprintf(stderr, "[ha_mqtt] no %s -> HA push disabled\n", MQTT_CFG);
        return 0;                                          /* not an error */
    }
    pthread_t t;
    if (pthread_create(&t, NULL, ha_mqtt_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
