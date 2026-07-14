/*
 * Background poller for an ESPHome-based P1 reader — the Zuidwijk SlimmeLezer
 * (and SlimmeLezer+), which is a Wemos D1 / ESP8266 running ESPHome's `dsmr`
 * platform. Requested on the forum as an alternative to reading energy via
 * Home Assistant.
 *
 * Transport is ESPHome's `web_server` component (port 80, enabled in Zuidwijk's
 * stock slimmelezer.yaml), which serves one JSON object per sensor:
 *
 *     GET /sensor/power_consumed
 *     {"id":"sensor-power_consumed","value":0.417,"state":"0.417 kW"}
 *
 * We read `value` (the raw float; `state` is a formatted string with the unit).
 * The object id is the sensor's name slugified, so the ids below track the
 * names in Zuidwijk's own YAML.
 *
 * NOTE the DSMR power sensors are in kW, not W — hence the *1000.
 *
 * Polling is deliberately frugal. This is an ESP8266 with a handful of lwIP
 * PCBs, and every HTTP/1.0 "Connection: close" request leaves a socket in
 * TIME_WAIT on the device for a minute or two. Hammering a P1 meter this way is
 * exactly what used to make the HomeWizard reboot (see homewizard.c), and the
 * ESP8266 has less headroom, not more. So: only the live power value is read on
 * the 10 s tick; the totals and the gas counter — which move slowly and are
 * only used for the hourly figure — are read once a minute. One socket at a
 * time, never in parallel.
 */
#include "p1esphome.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

p1esp_state_t p1esp_state = {0};

#define P1_POWER_EVERY_S  10
#define P1_TOTALS_EVERY_S 60

/* GET <path> from the reader. Host may be an IP or a hostname (ESPHome devices
 * are commonly reached as "slimmelezer.local"), so resolve rather than
 * inet_pton. Returns 0 and the raw response (headers included) in out. */
static int http_get(const char * host, const char * path, char * out, size_t outsz) {
    char hostname[128], port[8] = "80";
    snprintf(hostname, sizeof hostname, "%s", host);
    char * colon = strrchr(hostname, ':');
    if (colon) { *colon = 0; snprintf(port, sizeof port, "%s", colon + 1); }

    struct addrinfo hints = {0}, * res = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostname, port, &hints, &res) != 0 || !res) return -1;

    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); return -1; }
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int rc = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) { close(s); return -1; }

    char req[256];
    int n = snprintf(req, sizeof req,
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, hostname);
    if (send(s, req, n, 0) != n) { close(s); return -1; }
    size_t got = 0;
    while (got < outsz - 1) {
        ssize_t k = recv(s, out + got, outsz - 1 - got, 0);
        if (k <= 0) break;
        got += (size_t)k;
    }
    out[got] = 0;
    close(s);
    return got ? 0 : -1;
}

/* Read one ESPHome sensor's `value`. Returns 0 on success. A sensor that is
 * present but has no reading yet serialises as "value":null → treated as a
 * miss so we don't latch a 0. */
static int read_sensor(const char * host, const char * object_id, float * out) {
    char path[96], body[512];
    snprintf(path, sizeof path, "/sensor/%s", object_id);
    if (http_get(host, path, body, sizeof body) != 0) return -1;
    const char * j = strstr(body, "\r\n\r\n");
    j = j ? j + 4 : body;
    const char * v = strstr(j, "\"value\":");
    if (!v) return -1;
    v += 8;
    while (*v == ' ' || *v == '\t') v++;
    if (*v == 'n') return -1;                  /* null — no reading yet */
    *out = (float)strtod(v, NULL);
    return 0;
}

/* Trailing-60-min gas, derived from the cumulative counter — same approach as
 * the HomeWizard poller (the meter only ever exposes a total). */
#define GAS_RING_N 64
static struct { long t; float m3; } gas_ring[GAS_RING_N];
static int  gas_ring_head = 0, gas_ring_count = 0;
static long gas_ring_last_t = 0;

static void gas_hour_update(float gas_now) {
    if (gas_now <= 0) { p1esp_state.gas_hour_m3 = 0; return; }
    long now = time(NULL);
    if (gas_ring_last_t == 0 || now - gas_ring_last_t >= 55) {
        gas_ring[gas_ring_head].t  = now;
        gas_ring[gas_ring_head].m3 = gas_now;
        gas_ring_head = (gas_ring_head + 1) % GAS_RING_N;
        if (gas_ring_count < GAS_RING_N) gas_ring_count++;
        gas_ring_last_t = now;
    }
    if (gas_ring_count < 2) { p1esp_state.gas_hour_m3 = 0; return; }
    long cutoff = now - 3600;
    int oldest = (gas_ring_head - gas_ring_count + GAS_RING_N) % GAS_RING_N;
    int ref = oldest;
    for (int k = 0; k < gas_ring_count; k++) {
        int i = (oldest + k) % GAS_RING_N;
        if (gas_ring[i].t <= cutoff) ref = i; else break;
    }
    float delta = gas_now - gas_ring[ref].m3;
    if (delta < 0) delta = 0;          /* counter reset / rollover guard */
    p1esp_state.gas_hour_m3 = delta;
}

/* Live power. DSMR reports consumption and production as two separate positive
 * sensors; the rest of the UI wants one signed watt figure (negative = export),
 * matching what the HomeWizard's active_power_w gives us. */
static void poll_power(const char * host) {
    float consumed_kw = 0, produced_kw = 0;
    int have_c = (read_sensor(host, "power_consumed", &consumed_kw) == 0);
    int have_p = (read_sensor(host, "power_produced", &produced_kw) == 0);
    p1esp_state.polled = 1;
    if (!have_c && !have_p) { p1esp_state.connected = 0; return; }
    p1esp_state.consumed_w = consumed_kw * 1000.0f;
    p1esp_state.produced_w = produced_kw * 1000.0f;
    p1esp_state.power_w    = p1esp_state.consumed_w - p1esp_state.produced_w;
    p1esp_state.connected  = 1;
}

static void poll_totals(const char * host) {
    float v;
    if (read_sensor(host, "energy_consumed_tariff_1", &v) == 0) p1esp_state.kwh_import_t1 = v;
    if (read_sensor(host, "energy_consumed_tariff_2", &v) == 0) p1esp_state.kwh_import_t2 = v;
    if (read_sensor(host, "energy_produced_tariff_1", &v) == 0) p1esp_state.kwh_export_t1 = v;
    if (read_sensor(host, "energy_produced_tariff_2", &v) == 0) p1esp_state.kwh_export_t2 = v;
    if (read_sensor(host, "gas_consumed", &v) == 0) {
        p1esp_state.gas_m3 = v;
        gas_hour_update(v);
    }
}

static void * p1_thread(void * arg) {
    (void)arg;
    time_t last_power = 0, last_totals = 0;
    int logged_up = -1;
    while (1) {
        const char * host = settings.p1esp_host;
        if (!host[0]) { p1esp_state.connected = 0; sleep(5); continue; }
        time_t now = time(NULL);
        if (now - last_power >= P1_POWER_EVERY_S) {
            poll_power(host);
            last_power = now;
            if (p1esp_state.connected != logged_up) {
                logged_up = p1esp_state.connected;
                if (logged_up)
                    fprintf(stderr, "[p1esp] %s: %.0f W (in %.0f / out %.0f)\n", host,
                            p1esp_state.power_w, p1esp_state.consumed_w, p1esp_state.produced_w);
                else
                    fprintf(stderr, "[p1esp] %s unreachable — is ESPHome's web_server "
                                    "enabled on port 80?\n", host);
            }
        }
        if (p1esp_state.connected && now - last_totals >= P1_TOTALS_EVERY_S) {
            poll_totals(host);
            last_totals = now;
        }
        sleep(2);
    }
    return NULL;
}

int p1esphome_start(void) {
    if (!settings.p1esp_host[0]) {
        fprintf(stderr, "[p1esp] no host configured — not starting poller\n");
        return 0;
    }
    pthread_t th;
    if (pthread_create(&th, NULL, p1_thread, NULL) != 0) return -1;
    pthread_detach(th);
    fprintf(stderr, "[p1esp] poller started (host=%s)\n", settings.p1esp_host);
    return 0;
}
