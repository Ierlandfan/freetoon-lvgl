/*
 * domoticz.c — Domoticz JSON-API client (lights + blinds). See domoticz.h.
 * Uses http_fetch (curl). Basic auth, when configured, is embedded in the URL
 * (http://user:pass@host/...) since http_fetch has no header support.
 */
#include "domoticz.h"
#include "http.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

domoticz_state_t domoticz_state = {0};

/* Build "http://[user:pass@]host/<path>". host may already include a scheme;
 * we normalise to bare host:port. */
static void build_url(char * out, size_t osz, const char * path) {
    const char * host = settings.domoticz_host;
    if (strncmp(host, "http://", 7) == 0)  host += 7;
    else if (strncmp(host, "https://", 8) == 0) host += 8;
    if (settings.domoticz_user[0])
        snprintf(out, osz, "http://%s:%s@%s/%s",
                 settings.domoticz_user, settings.domoticz_pass, host, path);
    else
        snprintf(out, osz, "http://%s/%s", host, path);
}

/* Copy the string value of "key" : "value" from a JSON object slice. */
static int jstr(const char * p, const char * end, const char * key, char * out, size_t osz) {
    char needle[40];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char * k = strstr(p, needle);
    if (!k || k >= end) { if (osz) out[0] = 0; return 0; }
    k = strchr(k + strlen(needle), ':');
    if (!k || k >= end) { if (osz) out[0] = 0; return 0; }
    k++;
    while (*k == ' ' || *k == '\t') k++;
    if (*k == '"') {
        k++;
        const char * e = strchr(k, '"');
        if (!e || e > end) { if (osz) out[0] = 0; return 0; }
        size_t n = (size_t)(e - k); if (n >= osz) n = osz - 1;
        memcpy(out, k, n); out[n] = 0;
        return 1;
    }
    /* numeric / bool */
    size_t n = 0;
    while (k < end && *k != ',' && *k != '}' && *k != '\r' && *k != '\n' && n + 1 < osz)
        out[n++] = *k++;
    out[n] = 0;
    return n > 0;
}

static int parse_devices(const char * body) {
    int n = 0;
    const char * p = strstr(body, "\"result\"");
    if (!p) return -1;
    while (n < DOMOTICZ_MAX_DEV && (p = strstr(p, "{")) != NULL) {
        const char * end = strstr(p, "}");
        if (!end) break;
        char idx[16] = "", name[40] = "", stype[32] = "", status[40] = "", level[8] = "";
        jstr(p, end, "idx", idx, sizeof idx);
        jstr(p, end, "Name", name, sizeof name);
        jstr(p, end, "SwitchType", stype, sizeof stype);
        jstr(p, end, "Status", status, sizeof status);
        jstr(p, end, "Level", level, sizeof level);
        if (idx[0] && name[0]) {
            domoticz_dev_t * d = &domoticz_state.dev[n];
            d->idx = atoi(idx);
            snprintf(d->name, sizeof d->name, "%s", name);
            if (strstr(stype, "Blind"))      d->kind = DZ_BLIND;
            else if (strstr(stype, "Dimmer")) d->kind = DZ_DIMMER;
            else                              d->kind = DZ_SWITCH;
            /* on = anything that isn't Off/Closed/Stopped */
            d->on = !(strcmp(status, "Off") == 0 || strcmp(status, "Closed") == 0 ||
                      strcmp(status, "Stopped") == 0);
            d->level = (d->kind == DZ_SWITCH) ? -1 : atoi(level);
            n++;
        }
        p = end + 1;
    }
    domoticz_state.count = n;
    return 0;
}

static int poll_once(void) {
    if (!settings.domoticz_host[0]) return -1;
    char url[320];
    build_url(url, sizeof url, "json.htm?type=command&param=getdevices&filter=light&used=true&order=Name");
    static char body[64 * 1024];
    if (http_fetch(url, body, sizeof body) != 0) return -1;
    if (!strstr(body, "\"status\" : \"OK\"") && !strstr(body, "\"status\":\"OK\"")) return -1;
    return parse_devices(body);
}

static void * dz_thread(void * arg) {
    (void)arg;
    while (1) {
        if (settings.enable_domoticz && poll_once() == 0)
            domoticz_state.connected = 1;
        else
            domoticz_state.connected = 0;
        sleep(10);
    }
    return NULL;
}

int domoticz_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, dz_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}

/* ---- control (async) ---- */
typedef struct { int idx; char cmd[16]; int level; } dz_action_t;

static void * action_thread(void * arg) {
    dz_action_t * a = arg;
    char path[160], url[360];
    if (a->cmd[0])
        snprintf(path, sizeof path,
                 "json.htm?type=command&param=switchlight&idx=%d&switchcmd=%s", a->idx, a->cmd);
    else
        snprintf(path, sizeof path,
                 "json.htm?type=command&param=switchlight&idx=%d&switchcmd=Set%%20Level&level=%d",
                 a->idx, a->level);
    build_url(url, sizeof url, path);
    char body[1024];
    http_fetch(url, body, sizeof body);
    free(a);
    return NULL;
}

static void fire(int idx, const char * cmd, int level) {
    dz_action_t * a = calloc(1, sizeof *a);
    if (!a) return;
    a->idx = idx; a->level = level;
    if (cmd) snprintf(a->cmd, sizeof a->cmd, "%s", cmd);
    pthread_t t;
    if (pthread_create(&t, NULL, action_thread, a) == 0) pthread_detach(t);
    else free(a);
}

void domoticz_switch_async(int idx, const char * cmd) { fire(idx, cmd, 0); }
void domoticz_set_level_async(int idx, int level)     { fire(idx, NULL, level); }
