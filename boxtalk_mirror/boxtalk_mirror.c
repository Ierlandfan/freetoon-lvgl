/*
 * boxtalk_mirror — make THIS (dev) Toon a full two-way remote screen of a MASTER
 * Toon: the dev's STOCK qt-gui shows the master's data AND its controls drive the
 * master. No freetoon UI here; runs alongside stock qt-gui.
 *
 * Bidirectional UUID-rewriting BoxTalk bridge (push both ways, no polling):
 *   DOWN  master notify/data ─tunnel→ rewrite master→dev → publish on dev bus
 *         → stock qt-gui shows it (we lazily register the impersonated devices).
 *   UP    qt-gui control invoke (SetSetpoint/ChangeSchemeState aimed at our
 *         impersonated qb-<dev>:happ_thermstat) → rewrite dev→master → forward
 *         to the master's REAL happ_thermstat, which executes it; the new state
 *         + the response flow back DOWN. So you can set/change everything.
 *
 * The dev's own data daemons MUST be stopped (full-stop) so they don't fight
 * the mirror — see boxtalk_mirror_mute.sh.
 *
 * Usage: boxtalk_mirror [-d] [--devguid <uuid>] <master_host> <proxy_port> <user> <pass> [filter]
 *   -d        : self-daemonize (detach from the controlling tty/SSH session so a
 *               dropped tunnel can't kill it; logs to /var/volatile/tmp/boxtalk_mirror.log)
 *   --devguid : this dev Toon's OWN thermostat instance GUID. ThermostatInfo is
 *               published under a per-Toon GUID (not the qb-base); the stock
 *               qt-gui binds its tile to the LOCAL GUID, so we rewrite the master's
 *               GUID (auto-learned) to this one. Capture it from the dev's own
 *               ThermostatInfo notify before muting happ_thermstat.
 *   filter    : "thermostat" (default, ThermostatInfo only) | "all" (everything)
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
#include <sys/select.h>
#include <fcntl.h>
#include <sys/stat.h>

#define BUS_HOST "127.0.0.1"
#define BUS_PORT 1337
#define DAEMON_LOG "/var/volatile/tmp/boxtalk_mirror.log"

static const char *g_master_host, *g_user, *g_pass, *g_filter = "thermostat";
static int g_proxy_port;
static char g_master_base[96] = "", g_dev_base[96] = "";
/* Which stock daemon we impersonate on the dev bus so qt-gui's point-to-point
 * dataset/queries route to us. happ_thermstat feeds the thermostat tile + water
 * pressure; run extra instances with --daemon happ_pwrusage (power) /
 * hdrv_sensory (humidity) to mirror those tiles too. */
static const char *g_daemon = "happ_thermstat";

/* ThermostatInfo (and similar happ_thermstat services) publish under a stable
 * per-Toon INSTANCE GUID (e.g. "b822de89-...."), NOT the qb-<base> daemon uuid.
 * The stock qt-gui binds its thermostat tile to its OWN local GUID, so injecting
 * the master's GUID is ignored. We auto-learn the master's GUID from the first
 * ThermostatInfo notify and rewrite it to the dev's GUID (passed via --devguid),
 * so qt-gui sees its expected device carrying the master's data. */
static char g_master_therm_guid[64] = "";   /* auto-learned from master notifies */
static char g_dev_therm_guid[64]    = "";    /* the dev Toon's own GUID (--devguid) */
static char g_up_uuid[64]   = "";            /* our up-connection connect uuid (mirror-<pid>) */
static char g_sub_client[96] = "";           /* the client that subscribed to the relayed dataset */

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

/* DIAG: async-signal-safe logger to learn which signal kills the daemon. */
static void diag_sig(int s) {
    char m[48]; const char *d = "0123456789";
    int n = 0; m[n++]='['; m[n++]='m'; m[n++]='i'; m[n++]='r'; m[n++]=']';
    m[n++]=' '; m[n++]='s'; m[n++]='i'; m[n++]='g'; m[n++]=' ';
    if (s>=10) m[n++]=d[s/10];
    m[n++]=d[s%10]; m[n++]='\n';
    (void)!write(2, m, n);
    _exit(128+s);
}

/* Detach into our own session so an SSH logout / dropped tunnel can't take us
 * down (classic double-fork). Logs go to DAEMON_LOG. Started manually — this is
 * a backgrounded user daemon, NOT an init/system-config persistence change. */
static void daemonize(void) {
    if (fork() > 0) _exit(0);          /* parent leaves */
    setsid();                          /* new session, no controlling tty */
    if (fork() > 0) _exit(0);          /* can't reacquire a tty */
    signal(SIGHUP, SIG_IGN);
    int fd = open(DAEMON_LOG, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
    int nullfd = open("/dev/null", O_RDONLY);
    if (nullfd >= 0) { dup2(nullfd, 0); if (nullfd > 0) close(nullfd); }
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
    /* Force every daemon to republish its notify-cached values (room temp,
     * humidity, VOC, sensors). The stock qt-gui sends this at startup; we send it
     * on each (re)subscribe so a freshly-muted dev qt-gui gets the full CURRENT
     * state, not just future changes. (Broadcast — no destuuid/base needed.) */
    snprintf(b, sizeof b,
        "<action class=\"invoke\" uuid=\"mirror-%d\" destuuid=\"\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:specific1\">"
        "<u:reSendAllNotifies xmlns:u=\"urn:hcb-hae-com:service:specific1:1\">"
        "</u:reSendAllNotifies></action>",
        pid);
    send_frame(fd, b);
}

/* Query the non-notify thermostat state vars (setpoint, program state) from the
 * master; the responses are re-emitted downstream as notifies (see handle_frame)
 * so qt-gui populates them on startup instead of waiting for a change. Needs the
 * master thermostat GUID, auto-learned from the first notify. */
static void query_thermostat_state(int fd) {
    if (!g_master_therm_guid[0]) return;
    char b[768]; int pid = (int)getpid();
    static const char *vars[] = { "CurrentSetpoint", "ProgramState",
                                  "BoilerChPressure", "CurrentTemperature" };
    for (unsigned i = 0; i < sizeof(vars)/sizeof(vars[0]); i++) {
        snprintf(b, sizeof b,
            "<query class=\"invoke\" uuid=\"mirror-%d\" destuuid=\"%s\" "
            "serviceid=\"urn:hcb-hae-com:serviceId:ThermostatInfo\" requestid=\"%d-9\">"
            "<u:QueryStateVariable xmlns:u=\"urn:hcb-hae-com:service:ThermostatInfo:1\">"
            "<varName>%s</varName><requestId>%d-9</requestId><timeout>30</timeout>"
            "</u:QueryStateVariable></query>",
            pid, g_master_therm_guid, pid, vars[i], pid);
        send_frame(fd, b);
    }
}

/* Pull the single value element out of a QueryStateVariableResponse, e.g.
 * "...<requestId>..</requestId><CurrentSetpoint>19.50</CurrentSetpoint>..." →
 * "<CurrentSetpoint>19.50</CurrentSetpoint>". Returns -1 on error / queryError. */
static int extract_state_value(const char *frame, char *out, int outsz) {
    const char *p = strstr(frame, "</requestId>");
    if (!p) return -1;
    p += strlen("</requestId>");
    while (*p==' '||*p=='\n'||*p=='\t'||*p=='\r') p++;
    if (*p != '<') return -1;
    if (strncmp(p, "<queryError", 11) == 0) return -1;
    const char *tagstart = p + 1;
    const char *tagend = strchr(tagstart, '>');
    if (!tagend) return -1;
    int taglen = (int)(tagend - tagstart);
    if (taglen <= 0 || taglen > 48) return -1;
    char close[56];
    snprintf(close, sizeof close, "</%.*s>", taglen, tagstart);
    const char *e = strstr(p, close);
    if (!e) return -1;
    e += strlen(close);
    int len = (int)(e - p);
    if (len <= 0 || len >= outsz) return -1;
    memcpy(out, p, len); out[len] = 0;
    return 0;
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

/* Downstream: CLAIM THE happ_thermstat DAEMON IDENTITY. The stock qt-gui tile
 * gets its data from the "thermostatInfo" dataset (specific1 / UpdateDataSet),
 * subscribed point-to-point to the happ_thermstat daemon (verified by dissecting
 * qt-gui's BxtDatasetHandler). So we must BE that daemon on the dev bus, or
 * qt-gui's dataset subscription dead-ends. Falls back to a plain publisher node
 * if we don't yet know our base. */
static void downstream_connect(int fd) {
    char b[512]; long now=(long)time(NULL); int pid=(int)getpid();
    if (g_dev_base[0]) {
        snprintf(b, sizeof b,
            "<discovery nts=\"ssdp:connect\" uuid=\"qb-%s:%s\" "
            "type=\"urn:schemas-hcb-hae-com:device:%s\" version=\"v\" "
            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" sessionKey=\"%d-%ld\"></discovery>",
            g_dev_base, g_daemon, g_daemon, pid, now);
    } else {
        snprintf(b, sizeof b,
            "<discovery nts=\"ssdp:connect\" uuid=\"boxtalk_mirror-%d\" "
            "type=\"urn:schemas-hcb-hae-com:device:keteladapter\" version=\"v\" "
            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" sessionKey=\"%d-%ld\"></discovery>",
            pid, pid, now);
    }
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
    /* The declaration MUST match exactly how the stock daemon announced this
     * device, or qt-gui won't re-bind its tile after the local daemon was muted.
     * ThermostatInfo lives on a "thermostatSettings" subdevice published by
     * happ_thermstat — so impersonate happ_thermstat with that device type/intAddr
     * (verified against a stock Toon's ssdp:alive). Other services fall back to a
     * generic boiler-style declaration. */
    if (strcmp(svc, "ThermostatInfo") == 0) {
        snprintf(b, sizeof b,
            "<discovery nts=\"ssdp:alive\" uuid=\"qb-%s:happ_thermstat\" "
            "type=\"urn:schemas-hcb-hae-com:device:happ_thermstat\" version=\"v\" "
            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
            "<device uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:subdevice\" "
            "intAddr=\"thermostatSettings\" version=\"1\">"
            "<service type=\"ThermostatInfo\" version=\"(null)\"/></device></discovery>",
            g_dev_base, devuuid);
    } else {
        snprintf(b, sizeof b,
            "<discovery nts=\"ssdp:alive\" uuid=\"qb-%s:boxtalk_mirror\" "
            "type=\"urn:schemas-hcb-hae-com:device:keteladapter\" version=\"v\" "
            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
            "<device uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:HappBoiler\" version=\"1\">"
            "<service type=\"%s\" version=\"(null)\"/></device></discovery>",
            g_dev_base, devuuid, svc);
    }
    send_frame(fd, b);
    fprintf(stderr, "[mirror] registered %s (%s)\n", devuuid, svc);
}

/* Declare a CLIENT node (e.g. the client's qt-gui) as alive on the MASTER bus, so
 * the master's hcb_comm routes that uuid's dataset/responses back to OUR
 * connection (not to the master's own identically-named node). Without this, a
 * relayed UpdateDataSetSubscription's UpdateDataSet is dropped (uuid unknown on
 * the master). Tracked so we announce each client uuid once per connection. */
static char g_cli_reg[MAXREG][96];
static int  g_ncli_reg = 0;
static void register_client_on_master(int fd, const char *cli_uuid) {
    for (int i = 0; i < g_ncli_reg; i++) if (strcmp(g_cli_reg[i], cli_uuid) == 0) return;
    if (g_ncli_reg < MAXREG) snprintf(g_cli_reg[g_ncli_reg++], 96, "%s", cli_uuid);
    char b[512];
    snprintf(b, sizeof b,
        "<discovery nts=\"ssdp:alive\" uuid=\"%s\" "
        "type=\"urn:schemas-hcb-hae-com:device:toonui\" version=\"v\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"></discovery>",
        cli_uuid);
    send_frame(fd, b);
    fprintf(stderr, "[mirror] announced client %s on master\n", cli_uuid);
}

/* Forward a data frame? thermostat-first keeps only ThermostatInfo frames. */
static int wanted(const char *frame) {
    if (strstr(frame, "ssdp:") || strstr(frame, "<subscribe")) return 0; /* control */
    if (strcmp(g_filter, "all") == 0)
        return strstr(frame, "<notify") || strstr(frame, "UpdateDataSet");
    /* thermostat filter: the ThermostatInfo notify service AND the lowercase
     * "thermostatInfo" dataset (UpdateDataSet via specific1) — the latter is what
     * the stock qt-gui tile actually binds to. */
    return strstr(frame, "ThermostatInfo") != NULL
        || strstr(frame, "thermostatInfo") != NULL;
}

#define DIR_DOWN 0   /* master -> dev: data + responses */
#define DIR_UP   1   /* dev (qt-gui) -> master: control invokes */
static char g_out[16384];
static char g_tmp[16384];

/* Two-stage rewrite: (1) qb-<base> daemon uuid, (2) thermostat instance GUID.
 * dir picks the direction of the GUID swap. Result lands in g_out. */
static void rewrite_both(const char *frame, const char *from, const char *to, int dir) {
    rewrite(frame, from, to, g_tmp, sizeof g_tmp);          /* stage 1: qb base */
    if (g_master_therm_guid[0] && g_dev_therm_guid[0]) {    /* stage 2: GUID */
        if (dir == DIR_DOWN)
            rewrite(g_tmp, g_master_therm_guid, g_dev_therm_guid, g_out, sizeof g_out);
        else
            rewrite(g_tmp, g_dev_therm_guid, g_master_therm_guid, g_out, sizeof g_out);
    } else {
        memcpy(g_out, g_tmp, strlen(g_tmp) + 1);
    }
}

/* Process one complete frame. from/to = UUID bases to rewrite for this
 * direction; wfd = where the rewritten frame goes. */
static void handle_frame(const char *frame, int wfd,
                         const char *from, const char *to, int dir) {
    if (dir == DIR_DOWN) {
        /* The master's UpdateDataSet reply to a dataset subscription we relayed AS
         * OURSELVES (dest = g_up_uuid). Map our uuid back to the real client + swap
         * the master daemon/GUID to the dev ones, and push it onto the client bus so
         * qt-gui's dataset tile (incl. the right-side setpoint/program controls)
         * binds. This is the path that was missing → gray control panel. */
        if (g_up_uuid[0] && g_sub_client[0] && strstr(frame, "UpdateDataSet")
            && !strstr(frame, "Subscription") && strstr(frame, g_up_uuid)) {
            char a[128], b[128];
            rewrite(frame, g_up_uuid, g_sub_client, g_out, sizeof g_out);   /* dest -> real client */
            snprintf(a, sizeof a, "qb-%s:%s", g_master_base, g_daemon);
            snprintf(b, sizeof b, "qb-%s:%s", g_dev_base, g_daemon);
            rewrite(g_out, a, b, g_tmp, sizeof g_tmp);                       /* sender daemon -> dev */
            if (g_master_therm_guid[0] && g_dev_therm_guid[0])
                rewrite(g_tmp, g_master_therm_guid, g_dev_therm_guid, g_out, sizeof g_out);
            else
                memcpy(g_out, g_tmp, strlen(g_tmp) + 1);
            send_frame(wfd, g_out);
            fprintf(stderr, "[mirror] dataset down -> %s: %.90s\n", g_sub_client, g_out);
            return;
        }
        /* A QueryStateVariableResponse for a thermostat var (setpoint / program
         * state — these are never pushed as notifies) → re-emit as a notify under
         * the dev GUID so qt-gui populates them on startup, not on next change. */
        if (g_dev_therm_guid[0] && strstr(frame, "QueryStateVariableResponse")
            && strstr(frame, "ThermostatInfo")) {
            char val[160];
            if (extract_state_value(frame, val, sizeof val) == 0) {
                ensure_registered(wfd, g_dev_therm_guid, "ThermostatInfo");
                snprintf(g_out, sizeof g_out,
                    "<notify uuid=\"%s\" serviceid=\"urn:hcb-hae-com:serviceId:ThermostatInfo\">%s</notify>",
                    g_dev_therm_guid, val);
                send_frame(wfd, g_out);
            }
            return;
        }
        int data = strstr(frame, "<notify") || strstr(frame, "UpdateDataSet");
        int resp = strstr(frame, "class=\"response\"") != NULL;
        if (!data && !resp) return;                 /* skip greeting/discovery/handshake */
        if (data && !wanted(frame)) return;         /* thermostat-first / all filter */
        /* Non-thermostat instances (power/humidity) forward ONLY their own
         * daemon's dataset (uuid="qb-<master>:<g_daemon>") — otherwise a filter=all
         * instance also relays ThermostatInfo etc. and cross-contaminates the
         * other tiles. The thermostat instance keeps its device-GUID logic. */
        if (!g_dev_therm_guid[0]) {
            char mysrc[160];
            snprintf(mysrc, sizeof mysrc, "uuid=\"qb-%s:%s\"", g_master_base, g_daemon);
            if (!strstr(frame, mysrc)) return;
        }
        /* Auto-learn the master's thermostat instance GUID from the original
         * (pre-rewrite) frame: the bare-GUID uuid of a ThermostatInfo notify. */
        if (data && !g_master_therm_guid[0] && strstr(frame, "ThermostatInfo")) {
            char gu[64];
            if (attr(frame, "uuid", gu, sizeof gu) == 0 && strncmp(gu, "qb-", 3) != 0) {
                snprintf(g_master_therm_guid, sizeof g_master_therm_guid, "%s", gu);
                fprintf(stderr, "[mirror] learned master thermostat guid %s%s\n",
                        g_master_therm_guid,
                        g_dev_therm_guid[0] ? "" : " (no --devguid: qt-gui may ignore data)");
            }
        }
        rewrite_both(frame, from, to, DIR_DOWN);
        /* Thermostat-only: lazily declare the impersonated ThermostatInfo device
         * (legacy notify path). Other daemons (power/humidity) feed their tile via
         * the dataset over our claimed daemon identity — no device registration. */
        if (data && g_dev_therm_guid[0]) {
            char du[96], sid[96], svc[64];
            if (attr(g_out, "uuid", du, sizeof du) == 0) {
                const char *c = (attr(g_out, "serviceid", sid, sizeof sid) == 0)
                                ? strrchr(sid, ':') : NULL;
                snprintf(svc, sizeof svc, "%s", c ? c + 1 : "ThermostatInfo");
                ensure_registered(wfd, du, svc);
            }
        }
        send_frame(wfd, g_out);
    } else {                                        /* DIR_UP */
        /* Forward ONLY invokes addressed to the daemon WE impersonate
         * (destuuid="qb-<dev>:<g_daemon>") — or, for the thermostat, to its
         * device GUID. Matching merely on the dev base made every instance relay
         * ALL cross-daemon traffic, flooding the master and breaking the relay. */
        if (!strstr(frame, "class=\"invoke\"")) return;
        char mydest[160];
        snprintf(mydest, sizeof mydest, "destuuid=\"qb-%s:%s\"", g_dev_base, g_daemon);
        int targets = strstr(frame, mydest) != NULL ||
                      (g_dev_therm_guid[0] && strstr(frame, g_dev_therm_guid) != NULL);
        if (!targets) return;
        /* DATASET SUBSCRIPTION fix: the client (qt-gui) subscribes with
         * uuid="qb-<dev>:qt-gui". The blunt qb-dev->qb-master rewrite turns that
         * into qb-<master>:qt-gui — which COLLIDES with the master's OWN qt-gui, so
         * the master delivers the UpdateDataSet to its own qt-gui, never back to us
         * (→ client's dataset tile / right-panel controls stay empty). Fix: forward
         * the sub KEEPING the client's sender uuid (so the response routes back over
         * THIS connection / to that uuid which only we relay), swapping ONLY the
         * daemon dest (qb-dev:<daemon> -> qb-master:<daemon>) + the device GUID. */
        if (strstr(frame, "UpdateDataSetSubscription")) {
            /* Subscribe to the master AS OURSELVES (g_up_uuid = our up-connection's
             * connect uuid), so the master routes the UpdateDataSet back to THIS
             * connection (uuid-based routing — an ssdp:alive announce is NOT enough).
             * Remember the real client so we can map the response back to it. */
            char cli[96];
            if (attr(frame, "uuid", cli, sizeof cli) == 0)
                snprintf(g_sub_client, sizeof g_sub_client, "%s", cli);
            char a[128], b[128];
            rewrite(frame, g_sub_client, g_up_uuid, g_out, sizeof g_out);   /* sender -> us */
            snprintf(a, sizeof a, "qb-%s:%s", g_dev_base, g_daemon);
            snprintf(b, sizeof b, "qb-%s:%s", g_master_base, g_daemon);
            rewrite(g_out, a, b, g_tmp, sizeof g_tmp);                      /* dest -> master daemon */
            if (g_dev_therm_guid[0] && g_master_therm_guid[0])
                rewrite(g_tmp, g_dev_therm_guid, g_master_therm_guid, g_out, sizeof g_out);
            else
                memcpy(g_out, g_tmp, strlen(g_tmp) + 1);
            send_frame(wfd, g_out);
            fprintf(stderr, "[mirror] dataset-sub up (as %s for %s): %.90s\n", g_up_uuid, g_sub_client, g_out);
            return;
        }
        rewrite_both(frame, from, to, DIR_UP);
        send_frame(wfd, g_out);
        fprintf(stderr, "[mirror] control up: %.90s\n", g_out);
    }
}

/* recv once on rfd, dispatch every complete NUL-framed message via handle_frame.
 * Returns -1 on EOF/error. */
typedef struct { char b[16384]; int n; } fbuf_t;
static int pump(int rfd, fbuf_t *fb, int wfd, const char *from, const char *to, int dir) {
    int k = recv(rfd, fb->b + fb->n, (int)sizeof(fb->b) - fb->n - 1, 0);
    if (k <= 0) return -1;
    fb->n += k;
    int start = 0;
    for (int i = 0; i < fb->n; i++) {
        if (fb->b[i] != 0) continue;
        fb->b[i] = 0;
        handle_frame(fb->b + start, wfd, from, to, dir);
        start = i + 1;
    }
    if (start > 0) { memmove(fb->b, fb->b + start, fb->n - start); fb->n -= start; }
    if (fb->n >= (int)sizeof(fb->b) - 1) fb->n = 0;     /* overflow guard: drop partial */
    return 0;
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
    setvbuf(stderr, NULL, _IONBF, 0);   /* unbuffered so logs flush even to a file */
    int ai = 1, daemon = 0;
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != 0) {
        if (strcmp(argv[ai], "-d") == 0) { daemon = 1; ai++; }
        else if (strcmp(argv[ai], "--devguid") == 0 && ai + 1 < argc) {
            snprintf(g_dev_therm_guid, sizeof g_dev_therm_guid, "%s", argv[ai+1]); ai += 2;
        } else if (strcmp(argv[ai], "--daemon") == 0 && ai + 1 < argc) {
            g_daemon = argv[ai+1]; ai += 2;
        } else break;
    }
    if (argc - ai < 4) {
        fprintf(stderr, "usage: %s [-d] [--devguid <uuid>] <master_host> <proxy_port> <user> <pass> [thermostat|all]\n", argv[0]);
        return 2;
    }
    g_master_host = argv[ai]; g_proxy_port = atoi(argv[ai+1]);
    g_user = argv[ai+2]; g_pass = argv[ai+3];
    if (argc - ai > 4) g_filter = argv[ai+4];
    /* Learn our own base from the hostname (qb-<serial>) up front so the first
     * downstream_connect can already claim the happ_thermstat daemon identity.
     * The bus greeting confirms/sets it during the handshake anyway. */
    { char hn[128];
      if (gethostname(hn, sizeof hn) == 0 && strncmp(hn, "qb-", 3) == 0)
          snprintf(g_dev_base, sizeof g_dev_base, "%s", hn + 3); }
    signal(SIGPIPE, SIG_IGN);
    if (daemon) daemonize();   /* detach AFTER arg parse, BEFORE the reconnect loop */
    snprintf(g_up_uuid, sizeof g_up_uuid, "mirror-%d", (int)getpid());  /* must match upstream_subscribe */
    /* DIAG: log which signal kills us (async-signal-safe write). */
    { int sigs[] = {SIGTERM,SIGHUP,SIGINT,SIGQUIT,SIGSEGV,SIGBUS,SIGABRT,SIGALRM,SIGUSR1,SIGUSR2};
      for (unsigned i=0;i<sizeof(sigs)/sizeof(sigs[0]);i++) signal(sigs[i], diag_sig);
      (void)0; }
    signal(SIGPIPE, SIG_IGN);   /* keep PIPE ignored, not diag-trapped */

    for (;;) {                                   /* reconnect loop */
        fprintf(stderr, "[mirror] connecting to master proxy %s:%d...\n", g_master_host, g_proxy_port);
        int up = proxy_open();
        if (up < 0) { fprintf(stderr, "[mirror] proxy connect/auth failed\n"); sleep(5); continue; }
        fprintf(stderr, "[mirror] master proxy tunnel OPEN\n");
        int dn = tcp_connect(BUS_HOST, BUS_PORT);
        if (dn < 0) { fprintf(stderr, "[mirror] local bus connect failed\n"); close(up); sleep(5); continue; }
        fprintf(stderr, "[mirror] local dev bus connected\n");

        static char fr[8192];
        /* Each bus sends only a banner on connect, then waits for our handshake
         * before emitting the ssdp:alive frames that carry the UUID. So we MUST
         * handshake first, then read to learn the base. */
        upstream_subscribe(up);
        int tries = 0;
        while (g_master_base[0] == 0 && tries++ < 12) {
            int n = read_frame(up, fr, sizeof fr);
            if (n <= 0) break;
            detect_base(fr, g_master_base, sizeof g_master_base);
        }
        downstream_connect(dn);
        tries = 0;
        while (g_dev_base[0] == 0 && tries++ < 12) {
            int n = read_frame(dn, fr, sizeof fr);
            if (n <= 0) break;
            detect_base(fr, g_dev_base, sizeof g_dev_base);
        }
        if (!g_master_base[0] || !g_dev_base[0]) {
            fprintf(stderr, "[mirror] could not learn UUID bases (master='%s' dev='%s')\n",
                    g_master_base, g_dev_base);
            close(up); close(dn); sleep(5); continue;
        }
        fprintf(stderr, "[mirror] master=qb-%s  dev=qb-%s  filter=%s  devguid=%s\n",
                g_master_base, g_dev_base, g_filter,
                g_dev_therm_guid[0] ? g_dev_therm_guid : "(none)");
        g_nreg = 0;                              /* fresh registrations per connection */

        char mfrom[112], dto[112];
        snprintf(mfrom, sizeof mfrom, "qb-%s", g_master_base);
        snprintf(dto,   sizeof dto,   "qb-%s", g_dev_base);

        /* Re-subscribe NOW so the master re-pushes the FULL current dataset into
         * the relay loop — the initial push was consumed during base detection,
         * so without this the setpoint / program-state (Away/Home) stay blank
         * until something changes. */
        upstream_subscribe(up);

        /* Bidirectional relay: master DATA flows DOWN to the dev's qt-gui;
         * qt-gui's CONTROL invokes flow UP to the master's real happ_thermstat
         * (and its responses come back down). We also re-subscribe every
         * RESUB_SECS so qt-gui's tile is always fully populated, even when the
         * thermostat is idle (no field changes to push). */
        fbuf_t ub = {0}, db = {0};
        int mx = (up > dn ? up : dn) + 1;
        const int RESUB_SECS = 20;
        /* first refresh ~4s in (after reSendAllNotifies has let us learn the
         * thermostat GUID), then every RESUB_SECS. */
        time_t last_resub = time(NULL) - (RESUB_SECS - 4);
        for (;;) {
            fd_set rf; FD_ZERO(&rf); FD_SET(up, &rf); FD_SET(dn, &rf);
            struct timeval tv = { 4, 0 };
            int sr = select(mx, &rf, NULL, NULL, &tv);
            if (sr < 0) { if (errno == EINTR) continue; break; }
            time_t now = time(NULL);
            if (now - last_resub >= RESUB_SECS) {
                upstream_subscribe(up);       /* reSendAllNotifies + re-subscribe */
                query_thermostat_state(up);   /* setpoint + program state (non-notify) */
                last_resub = now;
            }
            if (sr == 0) continue;
            if (FD_ISSET(up, &rf) && pump(up, &ub, dn, mfrom, dto, DIR_DOWN) < 0) {
                fprintf(stderr, "[mirror] master link closed, reconnecting\n"); break;
            }
            if (FD_ISSET(dn, &rf) && pump(dn, &db, up, dto, mfrom, DIR_UP) < 0) {
                fprintf(stderr, "[mirror] dev bus closed, reconnecting\n"); break;
            }
        }
        close(up); close(dn); sleep(3);
    }
    return 0;
}
