/*
 * Schedule parser + writer for happ_thermstat's thermostatProgram config.
 * Communicates via HTTP (port 10080) with hcb_config, with direct file fallback
 * to /qmf/config/config_happ_thermstat.xml (or /mnt/data/qmf/config/).
 *
 * NOTE on Day of Week:
 *   Both hcb_config and this UI use the same convention:
 *   0 = Mon, 1 = Tue, 2 = Wed, 3 = Thu, 4 = Fri, 5 = Sat, 6 = Sun
 */
#include "schedule.h"
#include "boxtalk.h"
#include "i18n.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

schedule_entry_t schedule_entries[SCHEDULE_MAX];
int              schedule_count = 0;

static void parse_host_port(const char * src, int default_port, char * host_out, size_t host_sz, int * port_out) {
    if (!src || !src[0]) {
        snprintf(host_out, host_sz, "127.0.0.1");
        *port_out = default_port;
        return;
    }
    const char * colon = strrchr(src, ':');
    if (colon) {
        size_t hlen = colon - src;
        if (hlen >= host_sz) hlen = host_sz - 1;
        memcpy(host_out, src, hlen);
        host_out[hlen] = '\0';
        *port_out = atoi(colon + 1);
        if (*port_out <= 0) *port_out = default_port;
    } else {
        snprintf(host_out, host_sz, "%s", src);
        *port_out = default_port;
    }
}

unsigned schedule_state_color(int state) {
    switch (state) {
        case 0: return 0xff8866;   /* Comfort  — warm orange */
        case 1: return 0x66cc88;   /* Home     — green */
        case 2: return 0x4466cc;   /* Sleep    — blue */
        case 3: return 0xaa66ff;   /* Away     — purple */
        default:return 0x666666;
    }
}

const char * schedule_state_name(int state) {
    switch (state) {
        case 0: return tr("Comfort", "Comfort");
        case 1: return tr("Thuis", "Home");
        case 2: return tr("Slapen", "Sleep");
        case 3: return tr("Weg", "Away");
        default:return "--";
    }
}

const char * schedule_day_short(int day) {
    static const char * names_nl[] = {"Ma", "Di", "Wo", "Do", "Vr", "Za", "Zo"};
    static const char * names_en[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    if (day < 0 || day > 6) return "--";
    return settings.language == LANG_EN ? names_en[day] : names_nl[day];
}

/* Returns the index (0..schedule_count-1) of the entry active right now
 * (the one with the most-recent start time <= now, wrapping around the week).
 * Returns -1 if there are no entries at all. */
int schedule_current_entry_index(void) {
    if (schedule_count <= 0) return -1;
    time_t t = time(NULL);
    struct tm tm; localtime_r(&t, &tm);
    int toon_day = (tm.tm_wday + 6) % 7;
    int now_mow  = toon_day * 1440 + tm.tm_hour * 60 + tm.tm_min;

    int best_idx = -1, best_mow = -1;
    int latest_idx = -1, latest_mow = -1;
    for (int i = 0; i < schedule_count; i++) {
        const schedule_entry_t * e = &schedule_entries[i];
        int mow = e->start_day * 1440 + e->start_hour * 60 + e->start_min;
        if (mow <= now_mow && mow > best_mow) {
            best_mow = mow;
            best_idx = i;
        }
        if (mow > latest_mow) {
            latest_mow = mow;
            latest_idx = i;
        }
    }
    return (best_idx >= 0) ? best_idx : latest_idx;
}

/* Which Comfort/Home/Sleep/Away program does the weekly schedule say is
 * "active right now". Returns -1 if there are no entries at all. */
int schedule_program_now(void) {
    int idx = schedule_current_entry_index();
    return (idx >= 0) ? schedule_entries[idx].target_state : -1;
}

static int compare_entries(const void * a, const void * b) {
    const schedule_entry_t * x = (const schedule_entry_t *)a;
    const schedule_entry_t * y = (const schedule_entry_t *)b;
    if (x->start_day != y->start_day) return x->start_day - y->start_day;
    if (x->start_hour != y->start_hour) return x->start_hour - y->start_hour;
    return x->start_min - y->start_min;
}

int schedule_add(const schedule_entry_t * e) {
    if (schedule_count >= SCHEDULE_MAX) return -1;
    /* If an entry already exists at the exact same day, hour, min, overwrite it */
    for (int i = 0; i < schedule_count; i++) {
        if (schedule_entries[i].start_day == e->start_day &&
            schedule_entries[i].start_hour == e->start_hour &&
            schedule_entries[i].start_min == e->start_min) {
            schedule_entries[i] = *e;
            qsort(schedule_entries, schedule_count, sizeof(schedule_entry_t), compare_entries);
            return 0;
        }
    }
    schedule_entries[schedule_count++] = *e;
    qsort(schedule_entries, schedule_count, sizeof(schedule_entry_t), compare_entries);
    return 0;
}

int schedule_remove(int idx) {
    if (idx < 0 || idx >= schedule_count) return -1;
    memmove(&schedule_entries[idx], &schedule_entries[idx + 1],
            (schedule_count - idx - 1) * sizeof(schedule_entry_t));
    schedule_count--;
    return 0;
}

int schedule_replace(int idx, const schedule_entry_t * e) {
    if (idx < 0 || idx >= schedule_count) return -1;
    schedule_entries[idx] = *e;
    qsort(schedule_entries, schedule_count, sizeof(schedule_entry_t), compare_entries);
    return 0;
}

/* Parse an integer from either XML <key>NNN</key> or JSON "key": NNN */
static int get_field_int(const char * s, const char * e, const char * key, int dflt) {
    char xml_tag[48];
    snprintf(xml_tag, sizeof xml_tag, "<%s>", key);
    const char * p = strstr(s, xml_tag);
    if (p && p < e) {
        p += strlen(xml_tag);
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        return atoi(p);
    }
    char json_key[48];
    snprintf(json_key, sizeof json_key, "\"%s\"", key);
    p = strstr(s, json_key);
    if (p && p < e) {
        p += strlen(json_key);
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p < e && *p == ':') p++;
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p < e && *p == '"') p++;
        return atoi(p);
    }
    return dflt;
}

/* Parse entries from XML or JSON text.
 * NOTE: Parses into a temporary array first. Never wipes in-memory entries if
 * no entries were found in the buffer. */
static int parse_schedule_buffer(const char * buf) {
    if (!buf || !buf[0]) return -1;

    const char * sched_start = strstr(buf, "<schedule>");
    const char * sched_end   = sched_start ? strstr(sched_start, "</schedule>") : NULL;
    int is_xml = (sched_start && sched_end);

    if (!is_xml) {
        sched_start = strstr(buf, "\"schedule\"");
        if (!sched_start) sched_start = buf;
        sched_end = buf + strlen(buf);
    }

    schedule_entry_t tmp[SCHEDULE_MAX];
    int tmp_count = 0;
    const char * p = sched_start;
    while (p < sched_end && tmp_count < SCHEDULE_MAX) {
        const char * entry_start = NULL;
        const char * entry_end   = NULL;

        if (is_xml) {
            entry_start = strstr(p, "<entry");
            if (!entry_start || entry_start >= sched_end) break;
            entry_end = strstr(entry_start, "</entry>");
            if (!entry_end || entry_end > sched_end) break;
            p = entry_end + 8;
        } else {
            entry_start = strstr(p, "\"entry\"");
            if (!entry_start || entry_start >= sched_end) break;
            entry_start = strchr(entry_start, '[');
            if (!entry_start || entry_start >= sched_end) break;
            entry_end = strchr(entry_start, ']');
            if (!entry_end || entry_end > sched_end) break;
            p = entry_end + 1;
        }

        schedule_entry_t * e = &tmp[tmp_count++];
        int hcb_sday    = get_field_int(entry_start, entry_end, "startDayOfWeek", 0);
        int hcb_eday    = get_field_int(entry_start, entry_end, "endDayOfWeek",   0);

        /* Map hcb_config day (0=Sun..6=Sat) to UI (0=Mon..6=Sun) */
        e->start_day    = (hcb_sday + 6) % 7;
        e->end_day      = (hcb_eday + 6) % 7;
        e->start_min    = get_field_int(entry_start, entry_end, "startMin",    0);
        e->start_hour   = get_field_int(entry_start, entry_end, "startHour",   0);
        e->end_min      = get_field_int(entry_start, entry_end, "endMin",      0);
        e->end_hour     = get_field_int(entry_start, entry_end, "endHour",     0);
        e->target_state = get_field_int(entry_start, entry_end, "targetState", 0);
    }

    if (tmp_count > 0) {
        memcpy(schedule_entries, tmp, tmp_count * sizeof(schedule_entry_t));
        schedule_count = tmp_count;
        qsort(schedule_entries, schedule_count, sizeof(schedule_entry_t), compare_entries);
        fprintf(stderr, "[sched] parsed %d entries\n", schedule_count);
        return 0;
    }
    return -1;
}

/* HTTP request helper for hcb_config (GET or POST) with timeout and HTTP status verification */
static int http_request(const char * host, int port, const char * method, const char * path, const char * payload, char * out, size_t outsz) {
    if (!host || !host[0]) return -1;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        return -1;
    }

    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) {
        freeaddrinfo(res);
        return -1;
    }

    /* 2 second socket timeout for LAN reliability */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        close(s);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    size_t payload_len = payload ? strlen(payload) : 0;
    size_t req_cap = payload_len + 2048;
    char * req = (char *)malloc(req_cap);
    if (!req) {
        close(s);
        return -1;
    }

    int n = 0;
    if (strcmp(method, "POST") == 0) {
        n = snprintf(req, req_cap,
            "POST /%s HTTP/1.0\r\n"
            "Host: %s:%d\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n"
            "%s",
            path, host, port, payload_len, payload ? payload : "");
    } else {
        if (payload_len > 0) {
            n = snprintf(req, req_cap,
                "GET /%s?%s HTTP/1.0\r\n"
                "Host: %s:%d\r\n"
                "Connection: close\r\n\r\n",
                path, payload, host, port);
        } else {
            n = snprintf(req, req_cap,
                "GET /%s HTTP/1.0\r\n"
                "Host: %s:%d\r\n"
                "Connection: close\r\n\r\n",
                path, host, port);
        }
    }

    if (n <= 0 || (size_t)n >= req_cap) {
        free(req);
        close(s);
        return -1;
    }

    size_t sent = 0;
    while (sent < (size_t)n) {
        ssize_t k = send(s, req + sent, (size_t)n - sent, 0);
        if (k <= 0) {
            free(req);
            close(s);
            return -1;
        }
        sent += (size_t)k;
    }
    free(req);

    size_t accum_cap = 64 * 1024;
    char * accum = (char *)malloc(accum_cap);
    if (!accum) {
        close(s);
        return -1;
    }

    size_t acclen = 0;
    while (acclen < accum_cap - 1) {
        ssize_t k = recv(s, accum + acclen, accum_cap - 1 - acclen, 0);
        if (k <= 0) break;
        acclen += (size_t)k;
    }
    accum[acclen] = '\0';
    close(s);

    if (acclen == 0) {
        free(accum);
        return -1;
    }

    /* Verify HTTP status code (2xx) */
    if (strncmp(accum, "HTTP/1.", 7) == 0 && acclen > 12) {
        int status = atoi(accum + 9);
        if (status < 200 || status >= 300) {
            free(accum);
            return -1;
        }
    }

    const char * body = strstr(accum, "\r\n\r\n");
    if (body) body += 4;
    else body = accum;

    if (out && outsz > 0) {
        size_t blen = strlen(body);
        if (blen >= outsz) blen = outsz - 1;
        memcpy(out, body, blen);
        out[blen] = '\0';
    }

    free(accum);
    return 0;
}

/* Update schedule on target host (trying port 80 first, then port 10080; POST then GET) */
static int http_hcb_set_config(const char * host, int explicit_port, const char * qs) {
    int ports[2];
    int nports = 0;
    if (explicit_port > 0) {
        ports[nports++] = explicit_port;
    } else {
        ports[nports++] = 80;
        ports[nports++] = 10080;
    }

    for (int p = 0; p < nports; p++) {
        int port = ports[p];
        /* 1. Try POST with form data */
        if (http_request(host, port, "POST", "hcb_config", qs, NULL, 0) == 0) {
            fprintf(stderr, "[sched] setObjectConfig sent to %s:%d via POST\n", host, port);
            return 0;
        }
        /* 2. Try GET with query string */
        if (http_request(host, port, "GET", "hcb_config", qs, NULL, 0) == 0) {
            fprintf(stderr, "[sched] setObjectConfig sent to %s:%d via GET\n", host, port);
            return 0;
        }
    }
    return -1;
}

static void seed_default_schedule(void) {
    schedule_count = 0;
    /* Standard weekly program: 07:00 Home (1), 23:00 Sleep (2) */
    for (int d = 0; d < 7; d++) {
        schedule_entry_t * e1 = &schedule_entries[schedule_count++];
        e1->start_day = d;
        e1->start_hour = 7;
        e1->start_min = 0;
        e1->target_state = 1;

        schedule_entry_t * e2 = &schedule_entries[schedule_count++];
        e2->start_day = d;
        e2->start_hour = 23;
        e2->start_min = 0;
        e2->target_state = 2;
    }
    for (int i = 0; i < schedule_count; i++) {
        int nxt = (i + 1) % schedule_count;
        schedule_entries[i].end_day  = schedule_entries[nxt].start_day;
        schedule_entries[i].end_hour = schedule_entries[nxt].start_hour;
        schedule_entries[i].end_min  = schedule_entries[nxt].start_min;
    }
    fprintf(stderr, "[sched] seeded default weekly schedule (%d entries)\n", schedule_count);
}

/* Fallback: read /qmf/config/config_happ_thermstat.xml (or /HCBv2/config/) directly */
static int read_file_config(void) {
    const char * paths[] = {
        "/qmf/config/config_happ_thermstat.xml",
        "/mnt/data/qmf/config/config_happ_thermstat.xml",
        "/HCBv2/config/config_happ_thermstat.xml",
        "/mnt/data/qmf/config/toonsim_hcb_config.xml",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        FILE * f = fopen(paths[i], "r");
        if (!f) continue;
        static char file_buf[64 * 1024];
        size_t n = fread(file_buf, 1, sizeof(file_buf) - 1, f);
        fclose(f);
        if (n > 0) {
            file_buf[n] = '\0';
            const char * tp = strstr(file_buf, "thermostatProgram");
            if (tp) {
                const char * dev_start = tp;
                while (dev_start > file_buf && strncmp(dev_start, "<device", 7) != 0) dev_start--;
                if (strncmp(dev_start, "<device", 7) == 0) {
                    if (parse_schedule_buffer(dev_start) == 0) {
                        fprintf(stderr, "[sched] loaded %d entries from %s\n", schedule_count, paths[i]);
                        return 0;
                    }
                }
            }
        }
    }
    return -1;
}

int schedule_load(void) {
    /* 0. If in slave/client mode, fetch directly from Master Toon (prefer port 80, fallback to 10080) */
    if (settings.client_mode && settings.master_host[0]) {
        char host[128];
        int explicit_port = 0;
        parse_host_port(settings.master_host, 0, host, sizeof(host), &explicit_port);

        static char body[64 * 1024];
        const char * qs = "action=getObjectConfig&package=happ_thermstat&internalAddress=thermostatProgram";

        int rc = -1;
        if (explicit_port > 0) {
            rc = http_request(host, explicit_port, "GET", "hcb_config", qs, body, sizeof(body));
        } else {
            /* Try port 80 first (via lighttpd proxy on master) */
            rc = http_request(host, 80, "GET", "hcb_config", qs, body, sizeof(body));
            if (rc != 0) {
                /* Fallback to port 10080 */
                rc = http_request(host, 10080, "GET", "hcb_config", qs, body, sizeof(body));
            }
        }

        if (rc == 0 && parse_schedule_buffer(body) == 0) {
            fprintf(stderr, "[sched] loaded %d entries from Master Toon (%s)\n", schedule_count, host);
            return 0;
        }

        fprintf(stderr, "[sched] failed to load schedule from Master Toon (%s)\n", host);
        if (schedule_count > 0) {
            /* Retain current in-memory schedule if master is temporarily unreachable */
            return 0;
        }
        seed_default_schedule();
        return 0;
    }

    /* 1. Try BoxTalk response buffer if ready */
    if (config_response_ready && config_response_buf[0]) {
        if (parse_schedule_buffer(config_response_buf) == 0) {
            fprintf(stderr, "[sched] loaded %d entries via BoxTalk buffer\n", schedule_count);
            return 0;
        }
    }

    /* 2. Try HTTP getObjectConfig from local hcb_config (port 10080 or 80) */
    static char body[64 * 1024];
    const char * qs = "action=getObjectConfig&package=happ_thermstat&internalAddress=thermostatProgram";
    if (http_request("127.0.0.1", 10080, "GET", "hcb_config", qs, body, sizeof(body)) == 0 ||
        http_request("127.0.0.1", 80,    "GET", "hcb_config", qs, body, sizeof(body)) == 0) {
        if (parse_schedule_buffer(body) == 0) {
            fprintf(stderr, "[sched] loaded %d entries via HTTP hcb_config\n", schedule_count);
            return 0;
        }
    }

    /* 3. Fallback to local config file */
    if (read_file_config() == 0) return 0;

    /* 4. If no config loaded but entries already exist in memory, preserve them */
    if (schedule_count > 0) {
        return 0;
    }

    /* 5. First boot with no config found: seed clean defaults */
    seed_default_schedule();
    return 0;
}

/* Build the JSON payload for hcb_config setObjectConfig endpoint */
static size_t build_json(char * out, size_t outsz) {
    size_t off = 0;
    int n = snprintf(out + off, outsz - off, "{\"device\":[{\"schedule\":[");
    if (n < 0) return 0;
    off += (size_t)n;
    for (int i = 0; i < schedule_count; i++) {
        const schedule_entry_t * e = &schedule_entries[i];
        int hcb_sday = (e->start_day + 1) % 7;
        int hcb_eday = (e->end_day + 1) % 7;
        n = snprintf(out + off, outsz - off,
            "%s{\"entry\":["
            "{\"type\":\"weekly_recurring\"},"
            "{\"startMin\":\"%d\"},{\"startHour\":\"%d\"},{\"startDayOfWeek\":\"%d\"},"
            "{\"targetState\":\"%d\"},"
            "{\"endMin\":\"%d\"},{\"endHour\":\"%d\"},{\"endDayOfWeek\":\"%d\"}"
            "]}",
            i ? "," : "",
            e->start_min, e->start_hour, hcb_sday,
            e->target_state,
            e->end_min, e->end_hour, hcb_eday);
        if (n < 0) return 0;
        off += (size_t)n;
        if (off >= outsz - 64) break;
    }
    n = snprintf(out + off, outsz - off, "]}]}");
    if (n < 0) return 0;
    off += (size_t)n;
    return off;
}

/* URL-encode a buffer into another. Out must be at least 3*in_len+1. */
static void url_encode(const char * in, size_t in_len, char * out) {
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            out[j++] = '%'; out[j++] = hex[c >> 4]; out[j++] = hex[c & 0xf];
        }
    }
    out[j] = 0;
}

/* Build XML payload for file persistence */
static size_t build_schedule_xml(char * out, size_t outsz) {
    size_t off = 0;
    int n = snprintf(out + off, outsz - off,
        "<device>"
        "<package>happ_thermstat</package>"
        "<internalAddress>thermostatProgram</internalAddress>"
        "<type>program</type>"
        "<visibility>1</visibility>"
        "<schedule>");
    if (n < 0) return 0;
    off += (size_t)n;

    for (int i = 0; i < schedule_count; i++) {
        const schedule_entry_t * e = &schedule_entries[i];
        int hcb_sday = (e->start_day + 1) % 7;
        int hcb_eday = (e->end_day + 1) % 7;
        n = snprintf(out + off, outsz - off,
            "<entry type=\"weekly_recurring\">"
            "<type>weekly_recurring</type>"
            "<startMin>%d</startMin>"
            "<startHour>%d</startHour>"
            "<startDayOfWeek>%d</startDayOfWeek>"
            "<targetState>%d</targetState>"
            "<endMin>%d</endMin>"
            "<endHour>%d</endHour>"
            "<endDayOfWeek>%d</endDayOfWeek>"
            "</entry>",
            e->start_min, e->start_hour, hcb_sday,
            e->target_state,
            e->end_min, e->end_hour, hcb_eday);
        if (n < 0) return 0;
        off += (size_t)n;
        if (off >= outsz - 128) break;
    }

    n = snprintf(out + off, outsz - off, "</schedule></device>");
    if (n < 0) return 0;
    off += (size_t)n;
    return off;
}

/* Directly update /qmf/config/config_happ_thermstat.xml on disk as backup */
static void update_file_config(const char * new_dev_xml) {
    const char * paths[] = {
        "/qmf/config/config_happ_thermstat.xml",
        "/mnt/data/qmf/config/config_happ_thermstat.xml",
        "/HCBv2/config/config_happ_thermstat.xml",
        "/mnt/data/qmf/config/toonsim_hcb_config.xml",
        NULL
    };
    const char * path = NULL;
    FILE * f = NULL;
    for (int i = 0; paths[i]; i++) {
        f = fopen(paths[i], "r");
        if (f) { path = paths[i]; break; }
    }
    if (!f || !path) return;
    static char fbuf[128 * 1024];
    size_t n = fread(fbuf, 1, sizeof(fbuf) - 1, f);
    fclose(f);
    if (n == 0) return;
    fbuf[n] = '\0';

    /* Locate thermostatProgram device block */
    const char * tp = strstr(fbuf, "thermostatProgram");
    if (!tp) return;
    const char * dev_start = tp;
    while (dev_start > fbuf && strncmp(dev_start, "<device", 7) != 0) dev_start--;
    if (strncmp(dev_start, "<device", 7) != 0) return;
    const char * dev_end = strstr(tp, "</device>");
    if (!dev_end) return;
    dev_end += 9; /* include </device> */

    char tmp_path[128];
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path);
    FILE * out = fopen(tmp_path, "w");
    if (!out) {
        out = fopen(path, "w");
        if (!out) return;
        fwrite(fbuf, 1, (size_t)(dev_start - fbuf), out);
        fputs(new_dev_xml, out);
        fputs(dev_end, out);
        fclose(out);
        fprintf(stderr, "[sched] updated %s directly\n", path);
        return;
    }

    /* Write up to dev_start */
    fwrite(fbuf, 1, (size_t)(dev_start - fbuf), out);
    /* Write new device XML */
    fputs(new_dev_xml, out);
    /* Write remainder of file */
    fputs(dev_end, out);
    fclose(out);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
    } else {
        fprintf(stderr, "[sched] updated %s via rename\n", path);
    }
}

int schedule_save(void) {
    if (schedule_count <= 0) return -1;

    /* Ensure chronological order */
    qsort(schedule_entries, schedule_count, sizeof(schedule_entry_t), compare_entries);

    /* Recalculate end times to match the next switch point */
    for (int i = 0; i < schedule_count; i++) {
        int nxt = (i + 1) % schedule_count;
        schedule_entries[i].end_day  = schedule_entries[nxt].start_day;
        schedule_entries[i].end_hour = schedule_entries[nxt].start_hour;
        schedule_entries[i].end_min  = schedule_entries[nxt].start_min;
    }

    /* Build JSON payload for hcb_config setObjectConfig endpoint */
    static char json[16 * 1024];
    size_t jn = build_json(json, sizeof(json));
    if (jn == 0) return -1;

    static char encoded[48 * 1024];
    url_encode(json, jn, encoded);

    static char qs[52 * 1024];
    snprintf(qs, sizeof(qs),
        "action=setObjectConfig&package=happ_thermstat&internalAddress=thermostatProgram&config=%s",
        encoded);

    /* If in slave/client mode: update Master Toon only, do NOT alter slave's local files or BoxTalk */
    if (settings.client_mode && settings.master_host[0]) {
        char host[128];
        int explicit_port = 0;
        parse_host_port(settings.master_host, 0, host, sizeof(host), &explicit_port);

        int rc = http_hcb_set_config(host, explicit_port, qs);
        if (rc == 0) {
            fprintf(stderr, "[sched] successfully updated schedule on Master Toon (%s)\n", host);
            return 0;
        } else {
            fprintf(stderr, "[sched] failed to update schedule on Master Toon (%s)\n", host);
            return -1;
        }
    }

    /* Master / standalone mode: */
    /* Build XML payload */
    static char xml[32 * 1024];
    size_t xn = build_schedule_xml(xml, sizeof(xml));
    if (xn > 0) {
        /* 1. Send via BoxTalk to hcb_config (ConfigProvider:SetObjectConfig) - Primary Toon QML method */
        boxtalk_set_object_config(xml);

        /* 2. Send via HTTP to hcb_config */
        http_hcb_set_config("127.0.0.1", 10080, qs);

        /* 3. Also update config file on disk for fallback persistence */
        update_file_config(xml);
    }

    return 0;
}
