/*
 * meteradapter.c — official energy source: the Toon's built-in smart-meter.
 * The meter is a Z-Wave HAE_METER node read by happ_pwrusage, which publishes
 * the live electricity flow over BoxTalk (ElectricityFlowMeter service). We
 * subscribe to that notify in boxtalk.c — meteradapter_on_flow() lands the
 * value here. No HTTP polling. A watchdog thread clears `connected` if the
 * notifies dry up (meter unplugged / not included in the Z-Wave network).
 */
#include "meteradapter.h"
#include "settings.h"
#include "canonical_state.h"
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MET_STALE_S       60    /* no flow notify for this long → meter offline */
#define NILM_THRESHOLD_W  18.0f /* min |delta| W to trigger a NILM event */
#define NILM_DEBOUNCE_S   3     /* squelch repeated events within N seconds */

typedef struct { const char *name; float lo; float hi; } nilm_sig_t;
static const nilm_sig_t nilm_sigs[] = {
    /* no built-in signatures — every step goes to unknowns for user labelling */
};

meter_state_t    meter_state   = {0};

void meteradapter_on_flow(float watts) {
    static float  prev_w       = -1.0f;
    static time_t last_nilm_ts = 0;

    if (prev_w >= 0.0f) {
        float delta  = watts - prev_w;
        float adelta = delta < 0.0f ? -delta : delta;
        time_t now   = time(NULL);
        if (adelta >= NILM_THRESHOLD_W && (now - last_nilm_ts) > NILM_DEBOUNCE_S) {
            const char *dev = NULL;

            /* 1. Check built-in signatures */
            for (size_t i = 0; i < sizeof nilm_sigs / sizeof nilm_sigs[0]; i++) {
                if (adelta >= nilm_sigs[i].lo && adelta <= nilm_sigs[i].hi) {
                    dev = nilm_sigs[i].name;
                    break;
                }
            }

            /* 2. Check custom signatures from settings */
            if (!dev) {
                for (int i = 0; i < settings.nilm_sig_count && i < NILM_CUSTOM_MAX; i++) {
                    if (adelta >= settings.nilm_sig_lo[i] && adelta <= settings.nilm_sig_hi[i]) {
                        dev = settings.nilm_sig_name[i];
                        break;
                    }
                }
            }

            char fallback[24];
            if (!dev) {
                snprintf(fallback, sizeof fallback, "~%.0f W", adelta);
                dev = fallback;
            }

            meter_state.nilm_direction = (delta > 0.0f) ? +1 : -1;
            snprintf(meter_state.nilm_device, sizeof meter_state.nilm_device,
                     "%s %s", dev, delta > 0.0f ? "ON" : "OFF");
            meter_state.nilm_event_ts = now;
            last_nilm_ts = now;
        }
    }
    prev_w = watts;
    meter_state.power_w      = watts;
    meter_state.last_flow_s  = (long)time(NULL);
    if (!meter_state.connected) {
        fprintf(stderr, "[meteradapter] connected! live power = %.1f W\n", (double)watts);
    }
    meter_state.connected    = 1;
    canonical_state_notify(CSTATE_EVT_ENERGY);
}

/* Probe happ_pwrusage on 127.0.0.1:10080 for live meter wattage and diagnostics.
 * Supports both smart meters (instantaneous 'value') and analog pulse meters
 * (where 'value' is often null, but 'avgValue' holds the calculated wattage).
 * Also subtracts powerProduction if solar power is being generated. */
void probe_happ_pwrusage(void) {
    extern int http_fetch_to(const char *, char *, size_t, int);
    char buf[512] = {0};
    static int log_budget = 5;
    static int last_status = -1; /* 1 = got usage, 0 = null, -2 = fetch error */

    if (http_fetch_to("http://127.0.0.1:10080/happ_pwrusage?action=GetCurrentUsage", buf, sizeof(buf), 2) == 0) {
        float usage_w = 0.0f;
        int got_usage = 0;
        int is_avg = 0;
        const char * p_pu = strstr(buf, "\"powerUsage\"");
        if (p_pu) {
            const char * p_val = strstr(p_pu, "\"value\"");
            if (p_val) {
                p_val += 7;
                while (*p_val == ' ' || *p_val == ':' || *p_val == '\t') p_val++;
                if (*p_val != 'n' && *p_val != 'N') {
                    if (sscanf(p_val, "%f", &usage_w) == 1) {
                        got_usage = 1;
                    }
                }
            }
            if (!got_usage) {
                /* Analog pulse meter fallback: between pulses 'value' is null,
                 * but 'avgValue' carries the calculated average wattage */
                const char * p_avg = strstr(p_pu, "\"avgValue\"");
                if (p_avg) {
                    p_avg += 10;
                    while (*p_avg == ' ' || *p_avg == ':' || *p_avg == '\t') p_avg++;
                    if (*p_avg != 'n' && *p_avg != 'N') {
                        if (sscanf(p_avg, "%f", &usage_w) == 1) {
                            got_usage = 1;
                            is_avg = 1;
                        }
                    }
                }
            }
        }

        /* Check solar production to calculate net flow */
        float prod_w = 0.0f;
        const char * p_pp = strstr(buf, "\"powerProduction\"");
        if (p_pp) {
            const char * p_pval = strstr(p_pp, "\"value\"");
            if (p_pval) {
                p_pval += 7;
                while (*p_pval == ' ' || *p_pval == ':' || *p_pval == '\t') p_pval++;
                if (*p_pval != 'n' && *p_pval != 'N') {
                    float v = 0.0f;
                    if (sscanf(p_pval, "%f", &v) == 1) prod_w = v;
                }
            }
        }

        /* Check gas usage (supports both smart meters and analog pulse meters) */
        const char * p_gu = strstr(buf, "\"gasUsage\"");
        if (p_gu) {
            float g_val = 0.0f;
            int got_g = 0;
            const char * p_val = strstr(p_gu, "\"value\"");
            if (p_val) {
                p_val += 7;
                while (*p_val == ' ' || *p_val == ':' || *p_val == '\t') p_val++;
                if (*p_val != 'n' && *p_val != 'N') {
                    if (sscanf(p_val, "%f", &g_val) == 1) got_g = 1;
                }
            }
            if (!got_g) {
                const char * p_avg = strstr(p_gu, "\"avgValue\"");
                if (p_avg) {
                    p_avg += 10;
                    while (*p_avg == ' ' || *p_avg == ':' || *p_avg == '\t') p_avg++;
                    if (*p_avg != 'n' && *p_avg != 'N') {
                        if (sscanf(p_avg, "%f", &g_val) == 1) got_g = 1;
                    }
                }
            }
            if (got_g && g_val >= 0.0f) {
                meter_state.gas_now_m3h = g_val / 1000.0f; /* L/h to m³/h */
            }
        }

        if (got_usage) {
            float net_w = usage_w - prod_w;
            if (log_budget > 0 || last_status != 1) {
                if (log_budget > 0) log_budget--;
                fprintf(stderr, "[meteradapter] GetCurrentUsage: live power = %.1f W%s\n",
                        (double)net_w, is_avg ? " (analog pulse avgValue)" : "");
            }
            last_status = 1;
            meteradapter_on_flow(net_w);
        } else {
            if (log_budget > 0 || last_status != 0) {
                if (log_budget > 0) log_budget--;
                fprintf(stderr, "[meteradapter] GetCurrentUsage: powerUsage value/avgValue are null (waiting for pulses)\n");
            }
            last_status = 0;
        }
    } else {
        if (log_budget > 0 || last_status != -2) {
            if (log_budget > 0) log_budget--;
            fprintf(stderr, "[meteradapter] GetCurrentUsage: HTTP fetch to 127.0.0.1:10080 failed\n");
        }
        last_status = -2;
    }
}

static int parse_rrd_delta(const char * body, float * out_delta, float * out_latest) {
    if (!body || !out_delta) return 0;
    const char * p = strchr(body, '{');
    if (!p) p = body;
    float first = -1.0f, last = -1.0f;
    while (*p) {
        const char * k1 = strchr(p, '"');
        if (!k1) break;
        const char * k2 = strchr(k1 + 1, '"');
        if (!k2) break;
        const char * col = strchr(k2 + 1, ':');
        if (!col) break;
        const char * v = col + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (strncmp(v, "null", 4) != 0 && strncmp(v, "NaN", 3) != 0 && strncmp(v, "nan", 3) != 0) {
            char * endp = NULL;
            float val = strtof(v, &endp);
            if (endp != v && val >= 0.0f) {
                if (first < 0.0f) first = val;
                last = val;
            }
            p = (endp && endp > v) ? endp : v + 1;
        } else {
            p = v + 4;
        }
    }
    if (first >= 0.0f && last >= first) {
        *out_delta = (last - first) / 1000.0f;
        if (out_latest) *out_latest = last / 1000.0f;
        return 1;
    }
    return 0;
}

static void *watchdog_thread(void *arg) {
    (void)arg;
    int rrd_tick = 11; /* trigger RRD gas query on first loop after 5s */
    for (;;) {
        sleep(5);
        time_t now = time(NULL);

        /* On standalone/master with native meteradapter:
         * If BoxTalk flow notifies haven't arrived in the last 4 seconds,
         * poll happ_pwrusage (enables analog pulse meters and acts as failsafe). */
        if (!settings.client_mode && settings.energy_source == 0) {
            if (meter_state.last_flow_s == 0 || (now - meter_state.last_flow_s) >= 4) {
                probe_happ_pwrusage();
                now = time(NULL);
            }

            /* Query RRD gas_quantity hourly archive every ~60 seconds to compute today's gas */
            if (++rrd_tick >= 12) {
                rrd_tick = 0;
                struct tm ltm;
                localtime_r(&now, &ltm);
                ltm.tm_hour = 0; ltm.tm_min = 0; ltm.tm_sec = 0;
                time_t midnight = mktime(&ltm);
                char rrd_url[256];
                snprintf(rrd_url, sizeof rrd_url,
                         "http://127.0.0.1:10080/hcb_rrd?action=getRrdData&loggerName=gas_quantity&rra=5yrhours&from=%ld&to=%ld&readableTime=1&nullForNaN=1&samples=24",
                         (long)midnight, (long)now);
                char rrd_buf[2048];
                if (http_fetch_to(rrd_url, rrd_buf, sizeof rrd_buf, 2) == 0) {
                    float d = 0.0f, tot = 0.0f;
                    if (parse_rrd_delta(rrd_buf, &d, &tot)) {
                        meter_state.gas_today_m3 = d;
                        meter_state.gas_m3 = tot;
                    }
                }
            }
        }

        if (meter_state.last_flow_s == 0 ||
            now - meter_state.last_flow_s > MET_STALE_S) {
            if (meter_state.connected) {
                fprintf(stderr, "[meteradapter] watchdog: marked offline (no flow notify for %ld s)\n",
                        meter_state.last_flow_s ? (long)(now - meter_state.last_flow_s) : -1L);
            }
            meter_state.connected = 0;
        }
    }
    return NULL;
}

int meteradapter_start(void) {
    meter_state.gas_today_m3 = -1.0f;
    pthread_t t;
    if (pthread_create(&t, NULL, watchdog_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
