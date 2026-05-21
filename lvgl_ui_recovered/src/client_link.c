/*
 * client_link — slave-mode bridge to a master Toon.
 *
 * Reads: a background thread holds open `curl -N` against the master's
 * Server-Sent-Events endpoint (GET /api/state/stream, PWA port 10081) and
 * copies each pushed JSON frame into the shared toon_state / ha_state structs
 * that the LVGL screens already read. No local HCB / integration traffic.
 *
 * Writes: the control helpers POST JSON to the master's /api/setpoint,
 * /api/program and /api/curtain endpoints — the same ones the PWA uses.
 *
 * The master host is taken from settings.master_host and validated to a safe
 * character set before being interpolated into the curl command line.
 */
#include "client_link.h"
#include "settings.h"
#include "boxtalk.h"
#include "homeassistant.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>

#define MASTER_PORT 10081

/* Host must be a bare IP/hostname (optionally :port) — reject anything that
 * could break out of the single-quoted curl argument. */
static int host_ok(const char * h) {
    if (!h || !*h) return 0;
    for (const char * p = h; *p; p++) {
        char c = *p;
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':'))
            return 0;
    }
    return 1;
}

/* ---- tiny JSON field extractors (flat object, known keys) ---- */
static int jnum(const char * j, const char * key, double * out) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char * p = strstr(j, pat);
    if (!p) return 0;
    *out = strtod(p + strlen(pat), NULL);
    return 1;
}
static int jint(const char * j, const char * key, int * out) {
    double d; if (!jnum(j, key, &d)) return 0; *out = (int)d; return 1;
}
static int jstr(const char * j, const char * key, char * out, size_t n) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char * p = strstr(j, pat);
    if (!p) return 0;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i < n - 1) out[i++] = *p++;
    out[i] = 0;
    return 1;
}

static void apply_state(const char * j) {
    double d; int v;
    if (jnum(j, "indoor_temp",      &d)) toon_state.indoor_temp     = (float)d;
    if (jnum(j, "setpoint",         &d)) toon_state.setpoint        = (float)d;
    if (jint(j, "program_state",    &v)) toon_state.program_state   = v;
    if (jint(j, "active_state",     &v)) toon_state.active_state    = v;
    if (jint(j, "burner_on",        &v)) toon_state.burner_on       = v;
    if (jint(j, "dhw_on",           &v)) toon_state.dhw_on          = v;
    if (jnum(j, "modulation_level", &d)) toon_state.modulation_level= (float)d;
    if (jnum(j, "boiler_in_temp",   &d)) toon_state.boiler_in_temp  = (float)d;
    if (jnum(j, "boiler_out_temp",  &d)) toon_state.boiler_out_temp = (float)d;
    if (jnum(j, "ch_setpoint",      &d)) toon_state.ch_setpoint     = (float)d;
    if (jnum(j, "water_pressure",   &d)) toon_state.water_pressure  = (float)d;
    if (jnum(j, "humidity",         &d)) toon_state.humidity        = (float)d;
    if (jint(j, "eco2",             &v)) toon_state.eco2            = v;
    if (jint(j, "tvoc",             &v)) toon_state.tvoc            = v;
    if (jint(j, "ot_comm_error",    &v)) toon_state.ot_comm_error   = v;
    if (jint(j, "boiler_type",      &v)) toon_state.boiler_type     = v;

    /* Curtains mirror into ha_state so the existing curtain card works. */
    if (jint(j, "ha_connected",     &v)) ha_state.connected         = v;
    if (jint(j, "curtain_pos",      &v)) ha_state.curtain_pos       = v;
    if (jint(j, "curtain_battery",  &v)) ha_state.curtain_battery   = v;
    jstr(j, "curtain_state", ha_state.curtain_state, sizeof ha_state.curtain_state);

    toon_state.connected = 1;
}

static void * reader_thread(void * arg) {
    (void)arg;
    char url[160], cmd[256], line[8192];
    for (;;) {
        if (!host_ok(settings.master_host)) {
            toon_state.connected = 0;
            sleep(3);
            continue;
        }
        snprintf(url, sizeof url, "http://%s:%d/api/state/stream",
                 settings.master_host, MASTER_PORT);
        /* -N: no buffering (stream lines as they arrive). --connect-timeout
         * bounds the initial dial; no --max-time so the stream stays open. */
        snprintf(cmd, sizeof cmd,
                 "curl -s -N --connect-timeout 8 -A freetoon-client '%s' 2>/dev/null",
                 url);
        FILE * f = popen(cmd, "r");
        if (!f) { toon_state.connected = 0; sleep(3); continue; }
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "data:", 5) == 0) {
                char * j = line + 5;
                while (*j == ' ') j++;
                if (*j == '{') apply_state(j);
            }
        }
        pclose(f);
        toon_state.connected = 0;   /* stream dropped — reconnect shortly */
        sleep(2);
    }
    return NULL;
}

static int post_json(const char * path, const char * json) {
    if (!host_ok(settings.master_host)) return -1;
    char cmd[512];
    snprintf(cmd, sizeof cmd,
        "curl -s -m 6 -X POST -H 'Content-Type: application/json' "
        "--data '%s' 'http://%s:%d%s' >/dev/null 2>&1",
        json, settings.master_host, MASTER_PORT, path);
    return system(cmd) == 0 ? 0 : -1;
}

int client_link_setpoint(float temp) {
    char b[48];
    snprintf(b, sizeof b, "{\"value\":\"%.2f\"}", (double)temp);
    return post_json("/api/setpoint", b);
}
int client_link_program(int state) {
    char b[32];
    snprintf(b, sizeof b, "{\"state\":%d}", state);
    return post_json("/api/program", b);
}
int client_link_curtain(const char * action) {
    char b[48];
    snprintf(b, sizeof b, "{\"action\":\"%s\"}", action);
    return post_json("/api/curtain", b);
}

int client_link_start(void) {
    if (!settings.client_mode) return 0;
    toon_state.connected = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, reader_thread, NULL) != 0) {
        fprintf(stderr, "[client_link] thread create failed\n");
        return -1;
    }
    pthread_detach(t);
    fprintf(stderr, "[client_link] slave mode → master %s\n", settings.master_host);
    return 0;
}
