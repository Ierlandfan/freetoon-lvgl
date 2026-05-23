/*
 * otgw.c — OpenTherm Gateway telemetry poller.
 *
 * In "off" OT-bridge mode happ_thermstat talks directly to the keteladapter
 * and publishes CH pressure + setpoint over BoxTalk, but NOT the boiler
 * flow/return temps (those came from the quby_bridge proxy's frame-sniffing,
 * which is fragile and breaks happ's serial timing). The OTGW reads the same
 * boiler over OpenTherm and exposes everything reliably over HTTP, so we fill
 * the remaining boiler-card fields straight from it — read-only, robust, and
 * without the proxy. Water pressure is only a fallback (happ's value wins).
 */
#define _GNU_SOURCE
#include "boxtalk.h"          /* toon_state */
#include "settings.h"
#include "http.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* otmonitor is an array of {"name":..,"value":..,..} objects. Find the object
 * for `name` and copy its value. Tolerant of "k": v and "k" : v spacing. */
static int otmon_val(const char * body, const char * name, char * out, size_t osz) {
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", name);
    const char * p = strstr(body, needle);
    if (!p) return 0;
    p = strstr(p, "\"value\"");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '"') p++;
    size_t n = 0;
    while (*p && *p != '"' && *p != ',' && *p != '}' && n + 1 < osz) out[n++] = *p++;
    out[n] = 0;
    return n > 0;
}

static void poll_otgw(void) {
    if (!settings.otgw_host[0]) return;
    char url[160];
    snprintf(url, sizeof url, "http://%s/api/v1/otgw/otmonitor", settings.otgw_host);
    static char body[16384];
    if (http_fetch(url, body, sizeof body) != 0) return;

    char v[32];
    /* Boiler flow (supply) water temp — OT ID 25. happ doesn't surface this in
     * off mode; OTGW does. (boxtalk.h maps boiler_in_temp = flowwatertemperature) */
    if (otmon_val(body, "boilertemperature", v, sizeof v)) {
        float f = (float)atof(v);
        if (f > 0) toon_state.boiler_in_temp = f;
    }
    /* Return water temp — OT ID 28 (often 0: many boilers don't report it). */
    if (otmon_val(body, "returnwatertemperature", v, sizeof v)) {
        float f = (float)atof(v);
        if (f > 0) toon_state.boiler_out_temp = f;
    }
    /* Modulation + flame come from the boiler too. */
    if (otmon_val(body, "relmodlevel", v, sizeof v)) {
        float f = (float)atof(v);
        if (f >= 0) toon_state.modulation_level = f;
    }
    if (otmon_val(body, "flamestatus", v, sizeof v))
        toon_state.burner_on = (strcmp(v, "On") == 0);
    /* Pressure: fallback only — happ_thermstat's BoxTalk value wins when it has
     * one, so we don't fight it. Fills the gap if happ ever reports 0. */
    if (toon_state.water_pressure <= 0.05f &&
        otmon_val(body, "chwaterpressure", v, sizeof v)) {
        float f = (float)atof(v);
        if (f > 0) toon_state.water_pressure = f;
    }
}

static void * otgw_thread(void * arg) {
    (void)arg;
    for (;;) {
        poll_otgw();
        sleep(20);   /* boiler telemetry changes slowly; OTGW is local + cheap */
    }
    return NULL;
}

int otgw_start(void) {
    if (!settings.otgw_host[0]) {
        fprintf(stderr, "[otgw] no host configured — telemetry poller off\n");
        return 0;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, otgw_thread, NULL) != 0) return -1;
    pthread_detach(t);
    fprintf(stderr, "[otgw] telemetry poller started (host=%s)\n", settings.otgw_host);
    return 0;
}
