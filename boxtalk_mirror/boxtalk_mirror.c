/*
 * boxtalk_mirror — dev-side injector: mirror a MASTER Toon's BoxTalk data onto
 * THIS (dev) Toon's local bus, so the dev's STOCK qt-gui renders the master's
 * data. No freetoon UI here; runs alongside stock qt-gui.
 *
 *   master bus ──(authenticated tunnel, boxtalk_proxy on the master)──► us
 *        │ we SUBSCRIBE (push, no polling)
 *        ▼ for each frame: rewrite the master's device-UUID base → the dev's
 *   dev bus 127.0.0.1:1337 ◄── we PUBLISH the rewritten notify (quby_bridge
 *        │ pattern: declare devices in an ssdp handshake, then <notify…>)
 *        ▼ stock qt-gui (subscriber) shows the master's values.
 *
 * The dev's own data daemons MUST be stopped (full-stop) so they don't fight
 * the mirror — see boxtalk_mirror_mute.sh.
 *
 * Usage: boxtalk_mirror <master_host> <proxy_port> <user> <pass> [filter]
 *   filter: "thermostat" (default, ThermostatInfo only) | "all" (everything)
 *
 * UUID bases are auto-detected from each bus's hcb_comm greeting
 * (uuid="qb-<serial>-<model>:hcb_comm").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BUS_HOST "127.0.0.1"
#define BUS_PORT 1337

static const char *g_master_host, *g_user, *g_pass, *g_filter = "thermostat";
static int g_proxy_port;
static char g_master_base[96] = "", g_dev_base[96] = "";

static int b64enc(const unsigned char *in, int n, char *out, int outsz) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        int b = in[i] << 16;
        if (i+1 < n) b |= in[i+1] << 8;
        if (i+2 < n) b |= in[i+2];
        if (o+5 > outsz) return -1;
        out[o++] = t[(b>>18)&63]; out[o++] = t[(b>>12)&63];
        out[o++] = (i+1<n)?t[(b>>6)&63]:'='; out[o++] = (i+2<n)?t[b&63]:'=';
    }
    out[o] = 0; return o;
}

static int tcp_connect(const char *host, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port);
    if (inet_aton(host, &a.sin_addr) == 0) { close(s); return -1; }
    if (connect(s, (struct sockaddr*)&a, sizeof a) != 0) { close(s); return -1; }
    return s;
}

/* Open the authenticated tunnel to the master's boxtalk_proxy. Returns a fd on
 * which native BoxTalk flows after the HTTP 200. */
static int proxy_open(void) {
    int s = tcp_connect(g_master_host, g_proxy_port);
    if (s < 0) return -1;
    char up[160], cred[256], req[512];
    snprintf(up, sizeof up, "%s:%s", g_user, g_pass);
    b64enc((unsigned char*)up, strlen(up), cred, sizeof cred);
    int n = snprintf(req, sizeof req,
        "GET /boxtalk HTTP/1.0\r\nAuthorization: Basic %s\r\n\r\n", cred);
    if (send(s, req, n, 0) != n) { close(s); return -1; }
    /* read just the HTTP status line + headers (up to the blank line) */
    char hdr[512]; int h = 0;
    while (h < (int)sizeof(hdr)-1) {
        int k = recv(s, hdr+h, 1, 0);   /* byte-at-a-time so we don't eat bus bytes */
        if (k <= 0) { close(s); return -1; }
        h += k; hdr[h]=0;
        if (h >= 4 && strcmp(hdr+h-4, "\r\n\r\n") == 0) break;
    }
    if (!strstr(hdr, " 200 ")) {
        fprintf(stderr, "[mirror] proxy auth failed: %.40s\n", hdr); close(s); return -1;
    }
    return s;
}

static int send_frame(int fd, const char *xml) {
    size_t n = strlen(xml); char z = 0;
    if (send(fd, xml, n, MSG_NOSIGNAL) != (ssize_t)n) return -1;
    if (send(fd, &z, 1, MSG_NOSIGNAL) != 1) return -1;
    return 0;
}

/* Read one NUL-delimited frame (without the NUL). Returns length, 0 on EOF. */
static int read_frame(int fd, char *buf, int cap) {
    int n = 0;
    for (;;) {
        char c; int k = recv(fd, &c, 1, 0);
        if (k <= 0) return 0;
        if (c == 0) { buf[n] = 0; return n; }
        if (n < cap-1) buf[n++] = c;
    }
}

/* Pull "qb-<serial>-<model>" out of a frame carrying uuid="qb-...:hcb_comm". */
static int detect_base(const char *frame, char *out, int outsz) {
    const char *p = strstr(frame, "uuid=\"qb-");
    if (!p) return -1;
    p += 9;                              /* skip past 'uuid="qb-' to the serial */
    const char *e = strstr(p, ":hcb_comm");
    if (!e) return -1;
    int len = (int)(e - p);
    if (len <= 0 || len >= outsz) return -1;
    memcpy(out, p, len); out[len] = 0;
    return 0;
}

/* Replace every occurrence of `from` with `to` in `in` → `out`. */
static void rewrite(const char *in, const char *from, const char *to,
                    char *out, int outsz) {
    int fl = strlen(from), tl = strlen(to), o = 0;
    for (const char *p = in; *p && o < outsz-1; ) {
        if (fl && strncmp(p, from, fl) == 0 && o + tl < outsz-1) {
            memcpy(out+o, to, tl); o += tl; p += fl;
        } else out[o++] = *p++;
    }
    out[o] = 0;
}

/* What qt-gui consumes. thermostat-first = the first entry only. */
static const char *SERVICES[] = {
    "ThermostatInfo", "BoilerInfo", "TemperatureSensor",
    "HumiditySensor", "vocSensor",  "PowerUsage",
};
#define NSVC ((int)(sizeof(SERVICES)/sizeof(SERVICES[0])))

static int svc_count(void) { return strcmp(g_filter,"all")==0 ? NSVC : 1; }

/* Upstream: declare ourselves as a plain subscriber, then subscribe to the
 * master's services so it pushes notifies. */
static void upstream_subscribe(int fd) {
    char b[1024]; long now=(long)time(NULL); int pid=(int)getpid();
    snprintf(b, sizeof b,
        "<discovery nts=\"ssdp:connect\" uuid=\"mirror-%d\" "
        "type=\"urn:schemas-hcb-hae-com:device:toonui\" version=\"v\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" sessionKey=\"%d-%ld\"></discovery>",
        pid, pid, now);
    send_frame(fd, b);
    for (int i = 0; i < svc_count(); i++) {
        snprintf(b, sizeof b,
            "<subscribe uuid=\"mirror-%d\" destuuid=\"\"><target uuid=\"\" "
            "serviceid=\"urn:hcb-hae-com:serviceId:%s\"></target></subscribe>",
            pid, SERVICES[i]);
        send_frame(fd, b);
    }
}

/* Pull an attr value (uuid="..." / serviceid="...") out of a frame. */
static int attr(const char *frame, const char *name, char *out, int outsz) {
    char key[32]; snprintf(key, sizeof key, "%s=\"", name);
    const char *p = strstr(frame, key);
    if (!p) return -1;
    p += strlen(key);
    const char *e = strchr(p, '"');
    if (!e) return -1;
    int n = (int)(e - p);
    if (n <= 0 || n >= outsz) return -1;
    memcpy(out, p, n); out[n] = 0;
    return 0;
}

/* Downstream: announce ourselves as a publisher node. The impersonated devices
 * are declared LAZILY (ensure_registered) so their UUIDs match the actual
 * rewritten frames exactly — no daemon-name guessing. */
static void downstream_connect(int fd) {
    char b[512]; long now=(long)time(NULL); int pid=(int)getpid();
    snprintf(b, sizeof b,
        "<discovery nts=\"ssdp:connect\" uuid=\"qb-%s:boxtalk_mirror\" "
        "type=\"urn:schemas-hcb-hae-com:device:keteladapter\" version=\"v\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" sessionKey=\"%d-%ld\"></discovery>",
        g_dev_base, pid, now);
    send_frame(fd, b);
}

/* Declare a device (dev uuid = qb-<dev>:<daemon>, taken from the rewritten
 * frame) the first time we forward data for it, so the bus accepts our notifies
 * for that uuid/service. quby_bridge register-then-notify pattern. */
#define MAXREG 32
static char g_reg[MAXREG][96];
static int  g_nreg = 0;
static void ensure_registered(int fd, const char *devuuid, const char *svc) {
    for (int i = 0; i < g_nreg; i++) if (strcmp(g_reg[i], devuuid) == 0) return;
    if (g_nreg < MAXREG) snprintf(g_reg[g_nreg++], 96, "%s", devuuid);
    char b[768];
    snprintf(b, sizeof b,
        "<discovery nts=\"ssdp:alive\" uuid=\"qb-%s:boxtalk_mirror\" "
        "type=\"urn:schemas-hcb-hae-com:device:keteladapter\" version=\"v\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
        "<device uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:HappBoiler\" version=\"1\">"
        "<service type=\"%s\" version=\"(null)\"/></device></discovery>",
        g_dev_base, devuuid, svc);
    send_frame(fd, b);
    fprintf(stderr, "[mirror] registered %s (%s)\n", devuuid, svc);
}

/* Forward a data frame? thermostat-first keeps only ThermostatInfo frames. */
static int wanted(const char *frame) {
    if (strstr(frame, "ssdp:") || strstr(frame, "<subscribe")) return 0; /* control */
    if (strcmp(g_filter, "all") == 0)
        return strstr(frame, "<notify") || strstr(frame, "UpdateDataSet");
    return strstr(frame, "ThermostatInfo") != NULL;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) {
        const char *greet =
            "<discovery nts=\"ssdp:alive\" uuid=\"qb-659918000101-2011A0LOHI:hcb_comm\" "
            "type=\"urn:schemas-hcb-hae-com:device:hcb_comm\"></discovery>";
        char base[96]; detect_base(greet, base, sizeof base);
        printf("detect_base -> '%s'  (expect 659918000101-2011A0LOHI)\n", base);
        const char *frame =
            "<notify uuid=\"qb-659918000101-2011A0LOHI:happ_thermstat\" "
            "serviceid=\"urn:hcb-hae-com:serviceId:ThermostatInfo\">"
            "<currentTemp>2034</currentTemp></notify>";
        char out[512];
        rewrite(frame, "qb-659918000101-2011A0LOHI", "qb-DEADBEEF00-DEVMODEL01", out, sizeof out);
        printf("rewrite     -> %s\n", out);
        printf("RESULT: %s\n",
            (strcmp(base,"659918000101-2011A0LOHI")==0 &&
             strstr(out,"qb-DEADBEEF00-DEVMODEL01:happ_thermstat") &&
             !strstr(out,"659918000101")) ? "OK" : "FAIL");
        return 0;
    }
    if (argc < 5) {
        fprintf(stderr, "usage: %s <master_host> <proxy_port> <user> <pass> [thermostat|all]\n", argv[0]);
        return 2;
    }
    g_master_host = argv[1]; g_proxy_port = atoi(argv[2]);
    g_user = argv[3]; g_pass = argv[4];
    if (argc > 5) g_filter = argv[5];
    signal(SIGPIPE, SIG_IGN);

    for (;;) {                                   /* reconnect loop */
        int up = proxy_open();
        if (up < 0) { sleep(5); continue; }
        int dn = tcp_connect(BUS_HOST, BUS_PORT);
        if (dn < 0) { close(up); sleep(5); continue; }

        static char fr[8192], out[8192];
        /* learn the master base from its greeting (first frames) */
        int tries = 0;
        while (g_master_base[0] == 0 && tries++ < 8) {
            int n = read_frame(up, fr, sizeof fr);
            if (n <= 0) break;
            detect_base(fr, g_master_base, sizeof g_master_base);
        }
        /* learn the dev base from the local bus greeting */
        tries = 0;
        while (g_dev_base[0] == 0 && tries++ < 8) {
            int n = read_frame(dn, fr, sizeof fr);
            if (n <= 0) break;
            detect_base(fr, g_dev_base, sizeof g_dev_base);
        }
        if (!g_master_base[0] || !g_dev_base[0]) {
            fprintf(stderr, "[mirror] could not learn UUID bases (master='%s' dev='%s')\n",
                    g_master_base, g_dev_base);
            close(up); close(dn); sleep(5); continue;
        }
        fprintf(stderr, "[mirror] master=qb-%s  dev=qb-%s  filter=%s\n",
                g_master_base, g_dev_base, g_filter);

        upstream_subscribe(up);
        downstream_connect(dn);
        g_nreg = 0;                              /* fresh registrations per connection */

        char mfrom[112], dto[112];
        snprintf(mfrom, sizeof mfrom, "qb-%s", g_master_base);
        snprintf(dto,   sizeof dto,   "qb-%s", g_dev_base);

        for (;;) {                               /* relay */
            int n = read_frame(up, fr, sizeof fr);
            if (n <= 0) { fprintf(stderr, "[mirror] upstream closed, reconnecting\n"); break; }
            if (!wanted(fr)) continue;
            rewrite(fr, mfrom, dto, out, sizeof out);
            /* lazily declare the (rewritten) device before publishing its data */
            char du[96], sid[96], svc[64];
            if (attr(out, "uuid", du, sizeof du) == 0) {
                const char *c = (attr(out, "serviceid", sid, sizeof sid) == 0)
                                ? strrchr(sid, ':') : NULL;
                snprintf(svc, sizeof svc, "%s", c ? c+1 : "ThermostatInfo");
                ensure_registered(dn, du, svc);
            }
            if (send_frame(dn, out) != 0) { fprintf(stderr, "[mirror] dev bus closed\n"); break; }
        }
        close(up); close(dn); sleep(3);
    }
    return 0;
}
