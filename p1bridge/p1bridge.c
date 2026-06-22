/*
 * p1bridge — hybrid feeder replacing the dead meteradapter / happ_pwrusage.
 *
 * Two data legs published as BoxTalk notifies on the original meteradapter
 * source UUIDs, so every downstream Toon consumer (toonui, hcb_rrd, cloud
 * agent, lighttpd dashboards) keeps working.
 *
 *   Elec + gas:  HWE-P1   192.168.99.69   API v2 over WebSocket-Secure (live).
 *                We pipe TLS through a /usr/bin/openssl s_client subprocess
 *                on the Toon — no openssl linkage in our binary, sidestepping
 *                the missing dev headers in the Linaro sysroot.
 *   Water:       HWE-WTR  192.168.99.115  API v1 polling on /api/v1/data
 *                (HWE-WTR v2 is "In development" upstream; not shipped).
 *
 * Config:  /mnt/data/p1bridge.conf  (one line per device)
 *   192.168.99.69=<v2 token from hw_enroll.sh>
 *
 * Build (on the build host):
 *   /tmp/qt_rebuild/linaro/bin/arm-linux-gnueabihf-gcc -O2 -Wall \
 *     p1bridge.c -o p1bridge
 * Deploy: scp p1bridge root@192.168.3.212:/mnt/data/p1bridge
 * Persist (inittab):
 *   p1br:345:respawn:/mnt/data/p1bridge >> /var/volatile/tmp/p1bridge.log 2>&1
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---- config ---------------------------------------------------------- */
#define BXT_HOST        "127.0.0.1"
#define BXT_PORT        1337
#define HWE_P1_HOST     "192.168.99.69"
#define HWE_WTR_HOST    "192.168.99.115"
#define CONF_PATH       "/mnt/data/p1bridge.conf"
#define OPENSSL_BIN     "/usr/bin/openssl"
#define WATER_POLL_S    5
#define WS_PING_S       30

/* UUIDs that hcb_rrd ACTUALLY records under (verified via bxtsniff capture
   2026-05-15). The original meter publisher on this Toon was hdrv_zwave's
   HAE_METER_v3 sub-devices (Z-Wave smart-meter dongle, since dead). Publishing
   under those concrete UUIDs makes hcb_rrd resolve elec_flow/gas_flow/water_flow
   placeholders to us instead of the dead Z-Wave devices.

   bd1dfd97 = HAE_METER_v3_2 intAddr "3.2" (ElectricityFlowMeter)
   2177068a = HAE_METER_v3_1 intAddr "3.1" (GasFlowMeter)
   d431b440 = hdrv_p1 "water" outputDevice (WaterFlowMeter) — concrete in hcb_rrd cfg */
#define UUID_ELEC  "bd1dfd97-a03e-45da-ad66-d59b5990c702"
#define UUID_GAS   "2177068a-4416-444f-afc7-a0a86155670a"
#define UUID_WATER "d431b440-8c39-470b-ab12-f39b3d578bb3"
#define OUR_UUID   "qb-659918000101-2011A0LOHI:p1bridge"

static char g_elec_token[256] = "";

/* ===================================================================== */
/* generic helpers                                                       */
/* ===================================================================== */

static long now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
static void msleep(int ms) {
    struct timespec ts = { ms/1000, (ms%1000)*1000000L };
    nanosleep(&ts, NULL);
}
static void logmsg(const char *fmt, ...) {
    char ts[32]; time_t t = time(NULL); struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    fprintf(stderr, "[p1bridge %s] ", ts);
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
    fputc('\n', stderr);
}

/* base64 (no padding handling needed for output — RFC 6455 wants standard
   base64 with padding). */
static const char B64C[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_encode(const unsigned char *in, size_t n, char *out) {
    size_t i, o = 0;
    for (i = 0; i + 3 <= n; i += 3) {
        out[o++] = B64C[(in[i]   >> 2) & 0x3f];
        out[o++] = B64C[((in[i]  << 4) | (in[i+1] >> 4)) & 0x3f];
        out[o++] = B64C[((in[i+1]<< 2) | (in[i+2] >> 6)) & 0x3f];
        out[o++] = B64C[in[i+2] & 0x3f];
    }
    if (i < n) {
        out[o++] = B64C[(in[i] >> 2) & 0x3f];
        if (i + 1 == n) {
            out[o++] = B64C[(in[i] << 4) & 0x3f];
            out[o++] = '='; out[o++] = '=';
        } else {
            out[o++] = B64C[((in[i] << 4) | (in[i+1] >> 4)) & 0x3f];
            out[o++] = B64C[(in[i+1] << 2) & 0x3f];
            out[o++] = '=';
        }
    }
    out[o] = 0;
}

/* ===================================================================== */
/* config loader                                                         */
/* ===================================================================== */

static int load_config(void) {
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) { logmsg("config %s: %s", CONF_PATH, strerror(errno)); return -1; }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        /* strip CR/LF/spaces */
        char *p;
        for (p = val + strlen(val) - 1; p >= val && (*p=='\r'||*p=='\n'||*p==' '); p--) *p = 0;
        while (*key == ' ') key++;
        if (strcmp(key, HWE_P1_HOST) == 0) {
            snprintf(g_elec_token, sizeof(g_elec_token), "%s", val);
        }
    }
    fclose(f);
    if (!g_elec_token[0]) {
        logmsg("config: no token for %s — elec WS will not start", HWE_P1_HOST);
        return -1;
    }
    logmsg("config: elec token loaded (%zu chars)", strlen(g_elec_token));
    return 0;
}

/* ===================================================================== */
/* BoxTalk client                                                        */
/* ===================================================================== */

static int bxt_fd = -1;

static int bxt_connect(void) {
    if (bxt_fd >= 0) { close(bxt_fd); bxt_fd = -1; }
    bxt_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (bxt_fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(BXT_PORT);
    inet_aton(BXT_HOST, &a.sin_addr);
    if (connect(bxt_fd, (struct sockaddr*)&a, sizeof(a)) != 0) {
        close(bxt_fd); bxt_fd = -1; return -2;
    }
    /* hcb_comm sends routing acks back; drain them to prevent the TCP
     * receive window from filling and blocking hcb_comm's delivery loop.
     * Use ioctl instead of fcntl to stay within GLIBC_2.4 (Toon glibc 2.21). */
    { int one = 1; ioctl(bxt_fd, FIONBIO, &one); }
    return 0;
}

/* Discard all pending hcb_comm replies on bxt_fd (non-blocking). */
static void bxt_drain(void) {
    if (bxt_fd < 0) return;
    char sink[256];
    while (recv(bxt_fd, sink, sizeof(sink), 0) > 0)
        ;
}

/* NUL-delimited frame. */
static int bxt_send(const char *xml) {
    if (bxt_fd < 0) return -1;
    size_t n = strlen(xml);
    if (send(bxt_fd, xml, n, MSG_NOSIGNAL) != (ssize_t)n) return -1;
    char nul = 0;
    if (send(bxt_fd, &nul, 1, MSG_NOSIGNAL) != 1) return -1;
    return 0;
}

/* Rebind an hcb_rrd series to our publisher uuid. Without this, elec_flow/
   gas_flow stay "placeholder" and our notifies are ignored. Discovered via
   bxt_action_SetRrdDeviceUuid strings in hcb_rrd binary, 2026-05-15. */
static int bxt_set_rrd_binding(const char *series_uuid, const char *pub_uuid) {
    char buf[640];
    snprintf(buf, sizeof(buf),
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" serviceid=\"rrdLogger\">\n"
        "    <u:SetRrdDeviceUuid xmlns:u=\"urn:hcb-hae-com:service:rrdLogger:1\">\n"
        "        <deviceUuid>%s</deviceUuid>\n"
        "    </u:SetRrdDeviceUuid>\n"
        "</action>",
        OUR_UUID, series_uuid, pub_uuid);
    return bxt_send(buf);
}

/* hcb_rrd subdevice UUIDs for the meter series — pulled from hcb_rrd's
   own discovery announcement / .dat file inspection. */
#define RRD_ELEC_FLOW_UUID    "49869aaf-cbf4-40a9-9e6d-8f7ae3f5d536"
#define RRD_GAS_FLOW_UUID     "5c743a5b-95e5-4a00-a72d-5fbddf2e4199"
/* Cumulative meter readings — bound to "placeholder" or to Eneco-internal
 * UUIDs out of the box, so happ_pwrmtr (which doesn't run on a stock
 * post-cloud Toon) was the only writer. Re-binding to our publisher
 * UUIDs lets p1bridge fill the *_quantity_* archives that feed the
 * stats page's week/month/year tabs.
 *
 * Multiple ElectricityQuantityMeter rows exist — one per RRA bucket
 * size and tariff. We rebind ALL the placeholder-bound ones so the
 * 5min-week, 5yrhours and 10yrdays archives all get fed from the same
 * notify. The chart sums nt+lt for display, so publishing the total
 * to just nt (with lt staying 0) renders correctly without needing a
 * separate tariff split. */
#define RRD_ELEC_QTY_5YR_UUID       "657ccec4-4c7b-42e3-ae6d-4d4d880045c7"
#define RRD_ELEC_QTY_5YR_ALT_UUID   "095f63f6-8877-40dd-ae93-66904e5f77ac"
#define RRD_ELEC_QTY_5YR_ALT2_UUID  "1507acc2-2dc0-4565-a0ee-38ebeba81054"
#define RRD_GAS_QTY_5YR_UUID        "07b90980-c1ba-4801-88b0-75a3f7deaa32"
#define RRD_GAS_QTY_WEEK_UUID       "0e884c48-f425-4be9-a290-e6ab671c55d1"
#define RRD_WATER_QTY_UUID          "d598ceb1-ad11-466b-a4c8-0594a877c1ea"

static int bxt_handshake(void) {
    long now = (long)time(NULL); int pid = (int)getpid();
    char buf[2048];
    /* Mirror hdrv_zwave's discovery: declare each sub-device UUID with its
       supported services. hcb_bxtproxy/hcb_rrd register these so subsequent
       notifies from those UUIDs are accepted. */
    snprintf(buf, sizeof(buf),
        "<discovery nts=\"ssdp:connect\" uuid=\"%s\" "
        "type=\"urn:schemas-hcb-hae-com:device:meteradapter\" version=\"v\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
        "sessionKey=\"%d-%ld\">\n"
        "\t<device uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:HAE_METER_v3_2\" "
        "intAddr=\"3.2\" parent=\"53ec79ed-bb7d-4914-a58e-dcdeff614663\" version=\"1\">\n"
        "\t<service type=\"ElectricityFlowMeter\" version=\"(null)\"/>\n"
        "\t<service type=\"ElectricityQuantityMeter\" version=\"(null)\"/>\n"
        "\t</device>\n"
        "\t<device uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:HAE_METER_v3_1\" "
        "intAddr=\"3.1\" parent=\"53ec79ed-bb7d-4914-a58e-dcdeff614663\" version=\"1\">\n"
        "\t<service type=\"GasFlowMeter\" version=\"(null)\"/>\n"
        "\t<service type=\"GasQuantityMeter\" version=\"(null)\"/>\n"
        "\t</device>\n"
        "\t<device uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:outputDevice\" "
        "intAddr=\"water\" version=\"1\">\n"
        "\t<service type=\"WaterFlowMeter\" version=\"(null)\"/>\n"
        "\t<service type=\"WaterQuantityMeter\" version=\"(null)\"/>\n"
        "\t</device>\n"
        "</discovery>",
        OUR_UUID, pid, now, UUID_ELEC, UUID_GAS, UUID_WATER);
    if (bxt_send(buf) != 0) return -1;
    snprintf(buf, sizeof(buf),
        "<discovery nts=\"ssdp:alive\" uuid=\"%s\" "
        "type=\"urn:schemas-hcb-hae-com:device:meteradapter\" version=\"v\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n"
        "\t<device uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:HAE_METER_v3_2\" "
        "intAddr=\"3.2\" parent=\"53ec79ed-bb7d-4914-a58e-dcdeff614663\" version=\"1\">\n"
        "\t<service type=\"ElectricityFlowMeter\" version=\"(null)\"/>\n"
        "\t<service type=\"ElectricityQuantityMeter\" version=\"(null)\"/>\n"
        "\t</device>\n"
        "\t<device uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:HAE_METER_v3_1\" "
        "intAddr=\"3.1\" parent=\"53ec79ed-bb7d-4914-a58e-dcdeff614663\" version=\"1\">\n"
        "\t<service type=\"GasFlowMeter\" version=\"(null)\"/>\n"
        "\t<service type=\"GasQuantityMeter\" version=\"(null)\"/>\n"
        "\t</device>\n"
        "\t<device uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:outputDevice\" "
        "intAddr=\"water\" version=\"1\">\n"
        "\t<service type=\"WaterFlowMeter\" version=\"(null)\"/>\n"
        "\t<service type=\"WaterQuantityMeter\" version=\"(null)\"/>\n"
        "\t</device>\n"
        "</discovery>",
        OUR_UUID, UUID_ELEC, UUID_GAS, UUID_WATER);
    return bxt_send(buf);
}

static int bxt_notify(const char *src_uuid, const char *service,
                      const char *field, double value, int as_int) {
    char buf[768], val[32];
    if (as_int) snprintf(val, sizeof(val), "%lld", (long long)value);
    else        snprintf(val, sizeof(val), "%.3f", value);
    /* Format mirrors what happ_thermstat emits (verified via bxtsniff capture).
       Critically: NO destuuid="" attr, NO version="1" attr — hcb_rrd's filter
       rejects frames carrying them. Whitespace before payload matches the
       working pattern. */
    snprintf(buf, sizeof(buf),
        "<notify uuid=\"%s\" serviceid=\"urn:hcb-hae-com:serviceId:%s\">\n"
        "    <%s>%s</%s>\n"
        "</notify>", src_uuid, service, field, val, field);
    return bxt_send(buf);
}

static int bxt_ensure(void) {
    if (bxt_fd >= 0) return 0;
    if (bxt_connect() != 0) { logmsg("BoxTalk connect: %s", strerror(errno)); return -1; }
    if (bxt_handshake() != 0) { logmsg("BoxTalk handshake failed"); close(bxt_fd); bxt_fd = -1; return -1; }
    /* Re-bind elec_flow + gas_flow series to our publisher UUIDs every time
       we (re)connect. water_flow is hardcoded in hcb_rrd config (d431b440)
       so no rebind needed there. Safe to call on every reconnect. */
    bxt_set_rrd_binding(RRD_ELEC_FLOW_UUID, UUID_ELEC);
    bxt_set_rrd_binding(RRD_GAS_FLOW_UUID,  UUID_GAS);
    /* Cumulative meter loggers — same trick. Feeds the stats page's
       week/month/year tabs which were stuck on "no data" because the
       Eneco daemons that originally wrote here aren't running on a
       post-cloud Toon. */
    bxt_set_rrd_binding(RRD_ELEC_QTY_5YR_UUID,      UUID_ELEC);
    bxt_set_rrd_binding(RRD_ELEC_QTY_5YR_ALT_UUID,  UUID_ELEC);
    bxt_set_rrd_binding(RRD_ELEC_QTY_5YR_ALT2_UUID, UUID_ELEC);
    bxt_set_rrd_binding(RRD_GAS_QTY_5YR_UUID,       UUID_GAS);
    bxt_set_rrd_binding(RRD_GAS_QTY_WEEK_UUID,      UUID_GAS);
    bxt_set_rrd_binding(RRD_WATER_QTY_UUID,         UUID_WATER);
    logmsg("BoxTalk connected + handshaken + bindings (flow + qty) refreshed");
    return 0;
}

/* ===================================================================== */
/* tiny JSON value extractors (HW JSON is flat-ish — no escaped quotes   */
/* in the numeric fields we read)                                        */
/* ===================================================================== */

static const char *find_key(const char *json, const char *key) {
    char needle[64]; snprintf(needle, sizeof(needle), "\"%s\":", key);
    return strstr(json, needle);
}
static double json_num(const char *json, const char *key, double dflt) {
    const char *p = find_key(json, key); if (!p) return dflt;
    p += strlen(key) + 3;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"' || *p == 'n') return dflt;
    return strtod(p, NULL);
}
/* extract gas (m3) value from "external" array. */
static double json_external_value(const char *json, const char *want_type, double dflt) {
    const char *p = find_key(json, "external"); if (!p) return dflt;
    /* scan the array body looking for "type":"<want_type>", then take the
       nearest following "value":N. */
    char tneedle[64]; snprintf(tneedle, sizeof(tneedle), "\"type\":\"%s\"", want_type);
    const char *tp = strstr(p, tneedle); if (!tp) return dflt;
    const char *vp = strstr(tp, "\"value\":"); if (!vp) return dflt;
    vp += 8;
    while (*vp == ' ' || *vp == '\t') vp++;
    return strtod(vp, NULL);
}

/* ===================================================================== */
/* water poller (v1)                                                     */
/* ===================================================================== */

static int http_get_v1(const char *host, const char *path, char *out, size_t outsz) {
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return -1;
    struct timeval tv = {3, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(80);
    if (inet_aton(host, &a.sin_addr) == 0) { close(s); return -2; }
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) { close(s); return -3; }
    char req[256];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    if (send(s, req, n, 0) != n) { close(s); return -4; }
    char buf[4096]; size_t total = 0;
    while (total < sizeof(buf) - 1) {
        ssize_t r = recv(s, buf + total, sizeof(buf) - 1 - total, 0);
        if (r <= 0) break; total += (size_t)r;
    }
    close(s); buf[total] = 0;
    char *body = strstr(buf, "\r\n\r\n"); if (!body) return -5;
    snprintf(out, outsz, "%s", body + 4);
    return 0;
}

static void water_tick(void) {
    char body[4096];
    if (http_get_v1(HWE_WTR_HOST, "/api/v1/data", body, sizeof(body)) != 0) return;
    if (!strstr(body, "active_liter_lpm")) return;
    double lpm = json_num(body, "active_liter_lpm", 0);
    double m3  = json_num(body, "total_liter_m3", -1);
    if (bxt_ensure() == 0) {
        bxt_notify(UUID_WATER, "WaterFlowMeter", "CurrentWaterFlow", lpm, 0);
        /* Cumulative water reading as litres for the integer-typed
         * CurrentWaterQuantity field. Skipped if the meter reading is
         * missing rather than publishing 0 (which would wreck the
         * monotonic-rising assumption of consumption charts). */
        if (m3 >= 0)
            bxt_notify(UUID_WATER, "WaterQuantityMeter",
                       "CurrentWaterQuantity", m3 * 1000.0, 1);
    }
}

/* ===================================================================== */
/* TLS-via-subprocess: fork /usr/bin/openssl s_client                    */
/* ===================================================================== */

static pid_t g_tls_pid = -1;

static int tls_spawn(const char *host) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(sv[0]); close(sv[1]); return -1; }
    if (pid == 0) {
        /* child: openssl s_client's stdio = our socketpair end */
        close(sv[0]);
        dup2(sv[1], 0); dup2(sv[1], 1);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 2); close(devnull); }
        close(sv[1]);
        char port[]   = "443";
        char hostport[64];
        snprintf(hostport, sizeof(hostport), "%s:%s", host, port);
        execl(OPENSSL_BIN, "openssl", "s_client",
              "-connect",   hostport,
              "-servername", host,
              "-quiet",
              "-verify",    "0",
              (char*)NULL);
        _exit(127);
    }
    close(sv[1]);
    g_tls_pid = pid;
    /* non-blocking on the parent side so select() drives us */
    { int one = 1; ioctl(sv[0], FIONBIO, &one); }
    return sv[0];
}

static void tls_kill(int fd) {
    if (fd >= 0) close(fd);
    if (g_tls_pid > 0) {
        kill(g_tls_pid, SIGTERM);
        int st; waitpid(g_tls_pid, &st, 0);
        g_tls_pid = -1;
    }
}

/* blocking write helper through the TLS pipe. */
static int tls_write_all(int fd, const void *buf, size_t n) {
    const char *p = buf; size_t left = n;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { msleep(20); continue; }
            return -1;
        }
        p += w; left -= (size_t)w;
    }
    return 0;
}

/* ===================================================================== */
/* WebSocket — RFC 6455 client                                           */
/* ===================================================================== */

/* SHA1 — needed for Sec-WebSocket-Accept verification. Tiny portable impl. */
static void sha1_compress(uint32_t *h, const unsigned char *blk) {
    uint32_t w[80], a, b, c, d, e, f, k, tmp;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (blk[i*4]<<24)|(blk[i*4+1]<<16)|(blk[i*4+2]<<8)|blk[i*4+3];
    for (i = 16; i < 80; i++) {
        uint32_t v = w[i-3]^w[i-8]^w[i-14]^w[i-16];
        w[i] = (v<<1)|(v>>31);
    }
    a=h[0]; b=h[1]; c=h[2]; d=h[3]; e=h[4];
    for (i = 0; i < 80; i++) {
        if      (i<20) { f=(b&c)|((~b)&d); k=0x5A827999; }
        else if (i<40) { f=b^c^d;          k=0x6ED9EBA1; }
        else if (i<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
        else           { f=b^c^d;          k=0xCA62C1D6; }
        tmp = ((a<<5)|(a>>27)) + f + e + k + w[i];
        e=d; d=c; c=(b<<30)|(b>>2); b=a; a=tmp;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
}
static void sha1(const unsigned char *msg, size_t len, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    size_t i;
    for (i = 0; i + 64 <= len; i += 64) sha1_compress(h, msg + i);
    unsigned char tail[128]; size_t r = len - i;
    memcpy(tail, msg + i, r);
    tail[r] = 0x80;
    size_t pad = (r + 1 <= 56) ? (56 - r - 1) : (120 - r - 1);
    memset(tail + r + 1, 0, pad);
    uint64_t bits = (uint64_t)len * 8;
    size_t off = r + 1 + pad;
    int j;
    for (j = 0; j < 8; j++) tail[off + j] = (bits >> (56 - 8*j)) & 0xff;
    sha1_compress(h, tail);
    if (r + 1 + pad + 8 > 64) sha1_compress(h, tail + 64);
    for (j = 0; j < 5; j++) {
        out[j*4  ] = (h[j] >> 24) & 0xff;
        out[j*4+1] = (h[j] >> 16) & 0xff;
        out[j*4+2] = (h[j] >>  8) & 0xff;
        out[j*4+3] = (h[j]      ) & 0xff;
    }
}

/* Read until a delimiter or until n bytes — blocking-ish on a non-block fd. */
static ssize_t fd_read_until(int fd, char *buf, size_t cap, const char *delim, int timeout_ms) {
    size_t got = 0; long t0 = now_ms();
    while (got + 1 < cap) {
        ssize_t r = read(fd, buf + got, cap - 1 - got);
        if (r > 0) {
            got += (size_t)r; buf[got] = 0;
            if (delim && strstr(buf, delim)) return (ssize_t)got;
        } else if (r == 0) {
            return -1;
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
            if (now_ms() - t0 > timeout_ms) return -2;
            msleep(20);
        }
    }
    return (ssize_t)got;
}

static int ws_handshake(int fd, const char *host) {
    unsigned char key_raw[16];
    int rnd = open("/dev/urandom", O_RDONLY);
    if (rnd < 0) return -1;
    if (read(rnd, key_raw, 16) != 16) { close(rnd); return -1; }
    close(rnd);
    char key_b64[32]; b64_encode(key_raw, 16, key_b64);

    char req[512];
    int rn = snprintf(req, sizeof(req),
        "GET /api/ws HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n", host, key_b64);
    if (tls_write_all(fd, req, rn) != 0) return -1;

    char resp[2048];
    if (fd_read_until(fd, resp, sizeof(resp), "\r\n\r\n", 6000) < 0) {
        logmsg("ws: no upgrade response"); return -1;
    }
    if (!strstr(resp, " 101 ")) {
        logmsg("ws: bad upgrade status: %.*s", 40, resp); return -1;
    }
    /* verify accept hash */
    char concat[128];
    snprintf(concat, sizeof(concat),
             "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key_b64);
    unsigned char digest[20]; sha1((unsigned char*)concat, strlen(concat), digest);
    char want[40]; b64_encode(digest, 20, want);
    const char *got = strcasestr(resp, "sec-websocket-accept:");
    if (!got || !strstr(got, want)) {
        logmsg("ws: accept-hash mismatch"); return -1;
    }
    logmsg("ws: upgraded to /api/ws");
    return 0;
}

/* Send a TEXT frame, always masked. */
static int ws_send_text(int fd, const char *s) {
    size_t n = strlen(s);
    unsigned char hdr[14]; size_t hlen = 0;
    hdr[0] = 0x81;          /* FIN=1, opcode=text(1) */
    if (n < 126) {
        hdr[1] = 0x80 | (unsigned char)n; hlen = 2;
    } else if (n < 65536) {
        hdr[1] = 0x80 | 126; hdr[2] = (n>>8)&0xff; hdr[3] = n&0xff; hlen = 4;
    } else {
        hdr[1] = 0x80 | 127;
        int i;
        for (i = 0; i < 8; i++) hdr[2+i] = (n >> (56 - 8*i)) & 0xff;
        hlen = 10;
    }
    unsigned char mask[4];
    int rnd = open("/dev/urandom", O_RDONLY);
    if (rnd < 0) return -1;
    if (read(rnd, mask, 4) != 4) { close(rnd); return -1; }
    close(rnd);
    memcpy(hdr + hlen, mask, 4); hlen += 4;
    if (tls_write_all(fd, hdr, hlen) != 0) return -1;
    /* masked payload, chunked so we don't malloc */
    char buf[1024]; size_t i;
    for (i = 0; i < n; ) {
        size_t chunk = n - i; if (chunk > sizeof(buf)) chunk = sizeof(buf);
        size_t k;
        for (k = 0; k < chunk; k++) buf[k] = s[i+k] ^ mask[(i+k) & 3];
        if (tls_write_all(fd, buf, chunk) != 0) return -1;
        i += chunk;
    }
    return 0;
}

/* Read one complete WS frame's payload as a NUL-terminated string into out.
   Returns opcode (1=text, 8=close, 9=ping, 10=pong) or -1 on error / EOF.
   Control frames are auto-replied (ping->pong) and silently skipped, caller
   re-loops. */
static int ws_recv_into(int fd, char *out, size_t outsz, size_t *outlen) {
    static unsigned char rbuf[8192]; static size_t rlen = 0;
    /* keep reading until we have a full frame */
    for (;;) {
        /* try to parse a frame from what we have */
        if (rlen >= 2) {
            unsigned char b0 = rbuf[0], b1 = rbuf[1];
            int fin = b0 >> 7, op = b0 & 0x0f, masked = b1 >> 7;
            size_t plen = b1 & 0x7f, hdr = 2;
            if (plen == 126) {
                if (rlen < 4) goto need_more;
                plen = ((size_t)rbuf[2]<<8) | rbuf[3]; hdr = 4;
            } else if (plen == 127) {
                if (rlen < 10) goto need_more;
                plen = 0;
                int i; for (i = 0; i < 8; i++) plen = (plen<<8) | rbuf[2+i];
                hdr = 10;
            }
            if (masked) hdr += 4;          /* server should not mask, but be safe */
            if (rlen < hdr + plen) goto need_more;
            unsigned char *payload = rbuf + hdr;
            if (masked) {
                unsigned char *m = rbuf + hdr - 4; size_t i;
                for (i = 0; i < plen; i++) payload[i] ^= m[i & 3];
            }
            int ret_op = -1;
            if (op == 0x9) {              /* ping → pong */
                unsigned char ph[14]; size_t phlen = 0;
                ph[0] = 0x8A;             /* pong, FIN */
                if (plen < 126) { ph[1] = 0x80 | (unsigned char)plen; phlen = 2; }
                else            { ph[1] = 0x80 | 126; ph[2] = (plen>>8)&0xff; ph[3] = plen&0xff; phlen = 4; }
                unsigned char mask[4];
                int rnd = open("/dev/urandom", O_RDONLY); read(rnd, mask, 4); close(rnd);
                memcpy(ph + phlen, mask, 4); phlen += 4;
                tls_write_all(fd, ph, phlen);
                if (plen) {
                    /* masked echo of payload */
                    static char pb[125]; size_t i;
                    for (i = 0; i < plen && i < sizeof(pb); i++) pb[i] = payload[i] ^ mask[i & 3];
                    tls_write_all(fd, pb, plen);
                }
                ret_op = 9;
            } else if (op == 0x8) {        /* close */
                ret_op = 8;
            } else if (op == 0x1 || op == 0x0) { /* text / continuation */
                size_t copy = plen < outsz - 1 ? plen : outsz - 1;
                memcpy(out, payload, copy); out[copy] = 0;
                if (outlen) *outlen = copy;
                ret_op = 1;
                (void)fin;
            } else {
                ret_op = op;
            }
            /* slide remaining bytes */
            size_t consumed = hdr + plen;
            memmove(rbuf, rbuf + consumed, rlen - consumed);
            rlen -= consumed;
            if (ret_op == 9) continue;     /* ping handled, look for next */
            return ret_op;
        }
need_more:
        if (rlen == sizeof(rbuf)) { logmsg("ws: framing overflow"); return -1; }
        ssize_t r = read(fd, rbuf + rlen, sizeof(rbuf) - rlen);
        if (r > 0) rlen += (size_t)r;
        else if (r == 0) return -1;
        else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -2; /* would block */
            return -1;
        }
    }
}

/* tiny lookup: did this JSON frame have "type":"<v>"? */
static int json_type_is(const char *json, const char *v) {
    const char *p = strstr(json, "\"type\":");
    if (!p) return 0;
    p += 7;
    while (*p == ' ') p++;
    if (*p++ != '"') return 0;
    size_t n = strlen(v);
    return strncmp(p, v, n) == 0 && p[n] == '"';
}

/* ===================================================================== */
/* elec WS state machine                                                 */
/* ===================================================================== */

/* gas rate state — same as polling version. */
static double  g_prev_gas_m3 = -1; static time_t g_prev_gas_t = 0;

static void elec_publish(const char *json) {
    double pw = json_num(json, "power_w", -1e308);
    if (pw > -1e307 && bxt_ensure() == 0) {
        bxt_notify(UUID_ELEC, "ElectricityFlowMeter",
                   "CurrentElectricityFlow", pw, 1);
    }
    /* Cumulative elec meter — kWh from the WS frame, published as Wh so
     * the integer-typed hcb_rrd CurrentElectricityQuantity field gets
     * monotonic-rising values. Sum t1+t2 since we bind the placeholder
     * elec_quantity_nt and the chart's read code sums nt+lt anyway. */
    /* WS measurement frame uses shorter field names than the v1 REST API:
     *   "energy_import_t1_kwh", "energy_import_t2_kwh" (or total_kwh).
     * Fall back to the v1 names so the same code works against either. */
    double e_t1 = json_num(json, "energy_import_t1_kwh", -1);
    double e_t2 = json_num(json, "energy_import_t2_kwh", -1);
    if (e_t1 < 0) e_t1 = json_num(json, "total_power_import_t1_kwh", -1);
    if (e_t2 < 0) e_t2 = json_num(json, "total_power_import_t2_kwh", -1);
    if (e_t1 < 0) e_t1 = json_num(json, "energy_import_kwh", -1);
    if (e_t1 < 0) e_t1 = json_num(json, "total_power_import_kwh", -1);
    if (e_t2 < 0) e_t2 = 0;
    if (e_t1 >= 0 && bxt_ensure() == 0) {
        double wh = (e_t1 + (e_t2 > 0 ? e_t2 : 0)) * 1000.0;
        bxt_notify(UUID_ELEC, "ElectricityQuantityMeter",
                   "CurrentElectricityQuantity", wh, 1);
    }
    /* gas (external array) */
    double m3 = json_external_value(json, "gas_meter", -1);
    if (m3 >= 0) {
        time_t now = time(NULL);
        double rate_lph = 0;
        if (g_prev_gas_m3 >= 0 && now > g_prev_gas_t) {
            double dm3 = m3 - g_prev_gas_m3; if (dm3 < 0) dm3 = 0;
            rate_lph = dm3 * 1000.0 * 3600.0 / (double)(now - g_prev_gas_t);
        }
        g_prev_gas_m3 = m3; g_prev_gas_t = now;
        if (bxt_ensure() == 0) {
            bxt_notify(UUID_GAS, "GasFlowMeter", "CurrentGasFlow", rate_lph, 1);
            /* Cumulative gas reading as litres (m3 * 1000) for the
             * integer-typed CurrentGasQuantity field. */
            bxt_notify(UUID_GAS, "GasQuantityMeter",
                       "CurrentGasQuantity", m3 * 1000.0, 1);
        }
    }
}

static int elec_run_once(void) {
    int fd = tls_spawn(HWE_P1_HOST);
    if (fd < 0) { logmsg("tls spawn failed"); return -1; }
    if (ws_handshake(fd, HWE_P1_HOST) != 0) { tls_kill(fd); return -1; }

    char msg[16384]; size_t mlen;
    int saw_auth_req = 0, authorized = 0, subscribed = 0;
    long last_seen = now_ms();

    for (;;) {
        int op = ws_recv_into(fd, msg, sizeof(msg), &mlen);
        if (op == -1) { logmsg("ws: eof / read err"); tls_kill(fd); return -1; }
        if (op == -2) {
            if (now_ms() - last_seen > (WS_PING_S + 30) * 1000L) {
                logmsg("ws: stale, reconnecting"); tls_kill(fd); return -1;
            }
            /* idle — drive water-poll cadence from caller; here just brief sleep */
            msleep(100);
            continue;
        }
        last_seen = now_ms();
        if (op == 8) { logmsg("ws: server closed"); tls_kill(fd); return 0; }
        if (op != 1) continue;

        if (json_type_is(msg, "authorization_requested")) {
            char auth[512];
            snprintf(auth, sizeof(auth),
                "{\"type\":\"authorization\",\"data\":\"%s\"}", g_elec_token);
            if (ws_send_text(fd, auth) != 0) { tls_kill(fd); return -1; }
            saw_auth_req = 1;
        } else if (json_type_is(msg, "authorized")) {
            authorized = 1;
            if (ws_send_text(fd,
                "{\"type\":\"subscribe\",\"data\":\"measurement\"}") != 0)
                { tls_kill(fd); return -1; }
            subscribed = 1;
            logmsg("ws: authorized, subscribed to measurement");
        } else if (json_type_is(msg, "measurement")) {
            static int first = 1;
            if (first) {
                logmsg("ws: first measurement (%zu B): %.640s%s",
                       mlen, msg, mlen > 180 ? "..." : "");
                first = 0;
            }
            elec_publish(msg);
        } else if (json_type_is(msg, "error")) {
            logmsg("ws: device error: %.*s", 200, msg);
        }
        (void)saw_auth_req; (void)authorized; (void)subscribed;

        /* drain hcb_comm acks so TCP window never stalls the bus */
        bxt_drain();

        /* opportunistic water tick — keeps single-thread design */
        static long last_water_ms = 0;
        long t = now_ms();
        if (t - last_water_ms > WATER_POLL_S * 1000) {
            water_tick();
            last_water_ms = t;
        }
    }
}

/* ===================================================================== */
/* main                                                                  */
/* ===================================================================== */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    logmsg("starting, pid=%d", (int)getpid());

    int have_elec = (load_config() == 0);

    int backoff_ms = 1000;
    for (;;) {
        if (have_elec) {
            int rc = elec_run_once();
            if (rc != 0) {
                logmsg("elec WS down, backoff %dms", backoff_ms);
                msleep(backoff_ms);
                backoff_ms = backoff_ms < 30000 ? backoff_ms * 2 : 30000;
                /* keep water flowing while elec is down */
                water_tick();
                continue;
            }
            backoff_ms = 1000;
        } else {
            /* no elec token yet — just poll water on a slow loop */
            water_tick();
            msleep(WATER_POLL_S * 1000);
        }
    }
    return 0;
}
