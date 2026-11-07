#define _GNU_SOURCE
#include "hue.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <errno.h>

hue_state_t hue_state = {0};

static pthread_mutex_t g_hue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_hue_thread;
static volatile int    g_hue_running = 0;
static volatile int    g_trigger_poll = 0;

/* Clean host string and extract port (default 80) */
static void parse_hue_host(const char * in_host, char * host, size_t hsz, char * port, size_t psz) {
    if (!in_host) { host[0] = 0; snprintf(port, psz, "80"); return; }
    if (strncmp(in_host, "http://", 7) == 0) in_host += 7;
    else if (strncmp(in_host, "https://", 8) == 0) in_host += 8;
    snprintf(host, hsz, "%s", in_host);
    char * slash = strchr(host, '/'); if (slash) *slash = 0;
    char * colon = strrchr(host, ':');
    if (colon) {
        *colon = 0;
        snprintf(port, psz, "%s", colon + 1);
    } else {
        snprintf(port, psz, "80");
    }
}

/* Direct socket HTTP request helper */
static int hue_http_req(const char * method, const char * raw_host, const char * path,
                        const char * body, char * out, size_t out_max, int timeout_s) {
    char host[128];
    char port_str[16];
    parse_hue_host(raw_host, host, sizeof(host), port_str, sizeof(port_str));
    if (!host[0]) return -2;

    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return -2;

    int s = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s < 0) continue;
        struct timeval tv = { .tv_sec = timeout_s > 0 ? timeout_s : 4, .tv_usec = 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    if (s < 0) return -2;

    int body_len = body ? (int)strlen(body) : 0;
    char req[1024];
    int req_len;
    if (body_len > 0) {
        req_len = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: toonui-hue/1.0\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n"
            "%s",
            method, path, host, body_len, body);
    } else {
        req_len = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: toonui-hue/1.0\r\n"
            "Connection: close\r\n\r\n",
            method, path, host);
    }

    if (req_len <= 0 || req_len >= (int)sizeof(req) || send(s, req, req_len, 0) != req_len) {
        close(s);
        return -2;
    }

    /* Read response */
    char hdr_buf[2048];
    size_t hdr_got = 0;
    char * body_start = NULL;

    while (hdr_got < sizeof(hdr_buf) - 1) {
        ssize_t n = recv(s, hdr_buf + hdr_got, sizeof(hdr_buf) - 1 - hdr_got, 0);
        if (n <= 0) break;
        hdr_got += (size_t)n;
        hdr_buf[hdr_got] = '\0';
        body_start = strstr(hdr_buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            break;
        }
        body_start = strstr(hdr_buf, "\n\n");
        if (body_start) {
            body_start += 2;
            break;
        }
    }

    if (!body_start) {
        close(s);
        return -2;
    }

    int status = 0;
    if (sscanf(hdr_buf, "HTTP/%*d.%*d %d", &status) != 1 || status != 200) {
        close(s);
        return -2;
    }

    size_t body_pre = hdr_got - (size_t)(body_start - hdr_buf);
    size_t to_copy = body_pre;
    if (out_max > 0) {
        if (to_copy >= out_max - 1) to_copy = out_max - 1;
        memcpy(out, body_start, to_copy);
        size_t total_body = to_copy;

        while (total_body < out_max - 1) {
            ssize_t n = recv(s, out + total_body, out_max - 1 - total_body, 0);
            if (n <= 0) break;
            total_body += (size_t)n;
        }
        out[total_body] = '\0';
    }

    close(s);
    return 0;
}

/* Helper to extract a JSON string field value between quotes */
static int json_extract_str(const char * src, const char * end, const char * key, char * out, size_t osz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char * k = strstr(src, needle);
    if (!k || (end && k >= end)) { if (osz) out[0] = 0; return 0; }
    k = strchr(k + strlen(needle), ':');
    if (!k || (end && k >= end)) { if (osz) out[0] = 0; return 0; }
    k++;
    while (*k == ' ' || *k == '\t') k++;
    if (*k == '"') {
        k++;
        const char * q = strchr(k, '"');
        if (!q || (end && q > end)) { if (osz) out[0] = 0; return 0; }
        size_t len = (size_t)(q - k);
        if (len >= osz) len = osz - 1;
        memcpy(out, k, len);
        out[len] = 0;
        return 1;
    }
    return 0;
}

/* Helper to extract an integer or boolean field value */
static int json_extract_int(const char * src, const char * end, const char * key, int def_val) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char * k = strstr(src, needle);
    if (!k || (end && k >= end)) return def_val;
    k = strchr(k + strlen(needle), ':');
    if (!k || (end && k >= end)) return def_val;
    k++;
    while (*k == ' ' || *k == '\t') k++;
    if (strncmp(k, "true", 4) == 0) return 1;
    if (strncmp(k, "false", 5) == 0) return 0;
    return atoi(k);
}

/* Parse GET /api/<user>/lights response body */
static int parse_hue_lights(const char * body) {
    if (!body || !body[0]) return -1;
    /* Check for error array: [{"error":{"type":1,...}}] */
    if (strstr(body, "\"error\"") && strstr(body, "\"type\":1")) return -1;

    hue_light_t parsed[HUE_MAX_LIGHTS];
    int count = 0;

    const char * p = body;
    while (*p && count < HUE_MAX_LIGHTS) {
        /* Find light id key: e.g. "1": { or "12": { */
        const char * id_quote = strchr(p, '"');
        if (!id_quote) break;
        const char * id_end = strchr(id_quote + 1, '"');
        if (!id_end) break;
        const char * colon = strchr(id_end, ':');
        if (!colon) break;
        const char * brace = strchr(colon, '{');
        if (!brace) break;

        /* Check if id is numeric string */
        char id_buf[16];
        size_t id_len = (size_t)(id_end - (id_quote + 1));
        if (id_len >= sizeof(id_buf)) id_len = sizeof(id_buf) - 1;
        memcpy(id_buf, id_quote + 1, id_len);
        id_buf[id_len] = 0;

        int is_num = 1;
        for (size_t i = 0; i < id_len; i++) {
            if (!isdigit((unsigned char)id_buf[i])) { is_num = 0; break; }
        }

        if (!is_num || id_len == 0) {
            p = id_end + 1;
            continue;
        }

        /* Find matching closing brace for this light's object */
        int depth = 1;
        const char * cur = brace + 1;
        while (*cur && depth > 0) {
            if (*cur == '{') depth++;
            else if (*cur == '}') depth--;
            cur++;
        }
        const char * obj_end = cur;

        hue_light_t * L = &parsed[count];
        memset(L, 0, sizeof(*L));
        snprintf(L->id, sizeof(L->id), "%s", id_buf);

        /* Parse name and type */
        json_extract_str(brace, obj_end, "name", L->name, sizeof(L->name));
        json_extract_str(brace, obj_end, "type", L->type, sizeof(L->type));

        /* Parse state sub-object */
        const char * state_pos = strstr(brace, "\"state\"");
        if (state_pos && state_pos < obj_end) {
            const char * state_brace = strchr(state_pos, '{');
            if (state_brace && state_brace < obj_end) {
                const char * state_end = strchr(state_brace, '}');
                if (!state_end || state_end > obj_end) state_end = obj_end;

                L->on = json_extract_int(state_brace, state_end, "on", 0);
                L->reachable = json_extract_int(state_brace, state_end, "reachable", 1);

                /* Check for bri */
                char bri_needle[] = "\"bri\"";
                const char * has_bri = strstr(state_brace, bri_needle);
                if (has_bri && has_bri < state_end) {
                    L->is_dimmable = 1;
                    L->bri = json_extract_int(state_brace, state_end, "bri", 254);
                    if (L->bri < 1) L->bri = 1;
                    if (L->bri > 254) L->bri = 254;
                    L->bri_pct = (L->bri * 100 + 127) / 254;
                } else {
                    L->is_dimmable = 0;
                    L->bri = L->on ? 254 : 0;
                    L->bri_pct = L->on ? 100 : 0;
                }
            }
        }

        if (L->name[0]) {
            count++;
        }
        p = obj_end;
    }

    pthread_mutex_lock(&g_hue_lock);
    hue_state.count = count;
    hue_state.connected = (count >= 0);
    for (int i = 0; i < count; i++) {
        hue_state.lights[i] = parsed[i];
    }
    pthread_mutex_unlock(&g_hue_lock);

    return count;
}

/* Probe connection to Hue Bridge */
int hue_probe(const char * host, const char * user) {
    if (!host || !host[0] || !user || !user[0]) return -2;
    char path[128];
    snprintf(path, sizeof(path), "/api/%s/lights", user);
    char * body = malloc(65536);
    if (!body) return -2;
    int rc = hue_http_req("GET", host, path, NULL, body, 65536, 4);
    if (rc != 0) { free(body); return -2; }
    if (strstr(body, "\"error\"") && strstr(body, "\"type\":1")) {
        free(body);
        return -1;
    }
    int count = parse_hue_lights(body);
    free(body);
    return count >= 0 ? count : -1;
}

/* Pair with Hue Bridge (press link button within 30s) */
int hue_pair(const char * host, char * out_user, size_t out_sz, char * err_msg, size_t err_sz) {
    if (!host || !host[0]) {
        if (err_msg && err_sz) snprintf(err_msg, err_sz, "Geen host ingevuld");
        return -1;
    }
    char resp[4096];
    int rc = hue_http_req("POST", host, "/api", "{\"devicetype\":\"FreeToon#Toon\"}", resp, sizeof(resp), 5);
    if (rc != 0) {
        if (err_msg && err_sz) snprintf(err_msg, err_sz, "Geen verbinding met bridge");
        return -1;
    }

    /* Check for username */
    const char * u = strstr(resp, "\"username\"");
    if (u) {
        if (json_extract_str(resp, NULL, "username", out_user, out_sz)) {
            if (err_msg && err_sz) snprintf(err_msg, err_sz, "Gekoppeld!");
            return 0;
        }
    }

    /* Check for button not pressed error: error type 101 */
    if (strstr(resp, "\"type\":101") || strstr(resp, "link button not pressed")) {
        if (err_msg && err_sz) snprintf(err_msg, err_sz, "Druk eerst op de bridge-knop!");
        return 1;
    }

    if (err_msg && err_sz) snprintf(err_msg, err_sz, "Fout bij koppelen: %s", resp);
    return -1;
}

/* Polling thread */
static void * hue_poll_worker(void * arg) {
    (void)arg;
    char * body = malloc(65536);
    if (!body) return NULL;

    while (g_hue_running) {
        if (settings.hue_enabled && settings.hue_host[0] && settings.hue_user[0]) {
            char path[128];
            snprintf(path, sizeof(path), "/api/%s/lights", settings.hue_user);
            int rc = hue_http_req("GET", settings.hue_host, path, NULL, body, 65536, 4);
            if (rc == 0) {
                parse_hue_lights(body);
            } else {
                pthread_mutex_lock(&g_hue_lock);
                hue_state.connected = 0;
                pthread_mutex_unlock(&g_hue_lock);
            }
        } else {
            pthread_mutex_lock(&g_hue_lock);
            hue_state.connected = 0;
            pthread_mutex_unlock(&g_hue_lock);
        }

        /* Sleep up to 6 seconds with fast wakeup if triggered */
        for (int i = 0; i < 30 && g_hue_running; i++) {
            if (g_trigger_poll) {
                g_trigger_poll = 0;
                break;
            }
            usleep(200000); /* 200 ms */
        }
    }

    free(body);
    return NULL;
}

int hue_start(void) {
    if (g_hue_running) return 0;
    g_hue_running = 1;
    if (pthread_create(&g_hue_thread, NULL, hue_poll_worker, NULL) != 0) {
        g_hue_running = 0;
        return -1;
    }
    pthread_detach(g_hue_thread);
    return 0;
}

void hue_stop(void) {
    g_hue_running = 0;
    g_trigger_poll = 1;
}

/* Asynchronous action structure */
typedef struct {
    char id[16];
    int  action_type; /* 0 = toggle, 1 = set_bri, 2 = all_off, 3 = all_on */
    int  val;
} hue_act_t;

static void * hue_action_worker(void * arg) {
    hue_act_t * a = (hue_act_t *)arg;
    if (!a) return NULL;

    if (settings.hue_enabled && settings.hue_host[0] && settings.hue_user[0]) {
        char path[128];
        char body[64];
        char resp[512];

        if (a->action_type == 0) { /* toggle */
            snprintf(path, sizeof(path), "/api/%s/lights/%s/state", settings.hue_user, a->id);
            snprintf(body, sizeof(body), "{\"on\":%s}", a->val ? "true" : "false");
            hue_http_req("PUT", settings.hue_host, path, body, resp, sizeof(resp), 3);
        } else if (a->action_type == 1) { /* set bri */
            snprintf(path, sizeof(path), "/api/%s/lights/%s/state", settings.hue_user, a->id);
            if (a->val <= 0) {
                snprintf(body, sizeof(body), "{\"on\":false}");
            } else {
                int bri = (a->val * 254 + 50) / 100;
                if (bri < 1) bri = 1; if (bri > 254) bri = 254;
                snprintf(body, sizeof(body), "{\"on\":true,\"bri\":%d}", bri);
            }
            hue_http_req("PUT", settings.hue_host, path, body, resp, sizeof(resp), 3);
        } else if (a->action_type == 2) { /* all off */
            snprintf(path, sizeof(path), "/api/%s/groups/0/action", settings.hue_user);
            snprintf(body, sizeof(body), "{\"on\":false}");
            hue_http_req("PUT", settings.hue_host, path, body, resp, sizeof(resp), 3);
        } else if (a->action_type == 3) { /* all on */
            snprintf(path, sizeof(path), "/api/%s/groups/0/action", settings.hue_user);
            snprintf(body, sizeof(body), "{\"on\":true}");
            hue_http_req("PUT", settings.hue_host, path, body, resp, sizeof(resp), 3);
        }
        g_trigger_poll = 1;
    }

    free(a);
    return NULL;
}

static void queue_hue_act(const char * id, int type, int val) {
    hue_act_t * a = malloc(sizeof(hue_act_t));
    if (!a) return;
    memset(a, 0, sizeof(*a));
    if (id) snprintf(a->id, sizeof(a->id), "%s", id);
    a->action_type = type;
    a->val = val;

    pthread_t t;
    if (pthread_create(&t, NULL, hue_action_worker, a) == 0) {
        pthread_detach(t);
    } else {
        free(a);
    }
}

void hue_light_toggle_async(const char * id) {
    if (!id) return;
    int new_state = 1;
    pthread_mutex_lock(&g_hue_lock);
    for (int i = 0; i < hue_state.count; i++) {
        if (strcmp(hue_state.lights[i].id, id) == 0) {
            hue_state.lights[i].on = !hue_state.lights[i].on;
            new_state = hue_state.lights[i].on;
            break;
        }
    }
    pthread_mutex_unlock(&g_hue_lock);
    queue_hue_act(id, 0, new_state);
}

void hue_light_set_bri_async(const char * id, int pct) {
    if (!id) return;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    pthread_mutex_lock(&g_hue_lock);
    for (int i = 0; i < hue_state.count; i++) {
        if (strcmp(hue_state.lights[i].id, id) == 0) {
            hue_state.lights[i].bri_pct = pct;
            hue_state.lights[i].on = (pct > 0);
            break;
        }
    }
    pthread_mutex_unlock(&g_hue_lock);
    queue_hue_act(id, 1, pct);
}

void hue_all_off_async(void) {
    pthread_mutex_lock(&g_hue_lock);
    for (int i = 0; i < hue_state.count; i++) {
        hue_state.lights[i].on = 0;
    }
    pthread_mutex_unlock(&g_hue_lock);
    queue_hue_act(NULL, 2, 0);
}

void hue_all_on_async(void) {
    pthread_mutex_lock(&g_hue_lock);
    for (int i = 0; i < hue_state.count; i++) {
        hue_state.lights[i].on = 1;
    }
    pthread_mutex_unlock(&g_hue_lock);
    queue_hue_act(NULL, 3, 0);
}
