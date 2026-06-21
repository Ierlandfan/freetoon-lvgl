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
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>

#define MET_STALE_S       60    /* no flow notify for this long → meter offline */
#define NILM_THRESHOLD_W  18.0f /* min |delta| W to trigger a NILM event */
#define NILM_DEBOUNCE_S   3     /* squelch repeated events within N seconds */

typedef struct { const char *name; float lo; float hi; } nilm_sig_t;
static const nilm_sig_t nilm_sigs[] = {
    { "Bathroom light",  13.0f,  28.0f  },
    { "Fridge",          28.0f,  43.0f  },
    { "CV boiler",       43.0f,  57.0f  },
    { "TV / Decoder",    50.0f,  75.0f  },
    { "Itho fan",       115.0f, 170.0f  },
};

meter_state_t    meter_state   = {0};
nilm_unknown_t   nilm_unknowns [NILM_UNKNOWN_MAX] = {{0}};
volatile int     nilm_unknown_count = 0;

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
                if (adelta >= nilm_sigs[i].lo && adelta < nilm_sigs[i].hi) {
                    dev = nilm_sigs[i].name;
                    break;
                }
            }

            /* 2. Check custom signatures from settings */
            if (!dev) {
                for (int i = 0; i < settings.nilm_sig_count && i < NILM_CUSTOM_MAX; i++) {
                    if (adelta >= settings.nilm_sig_lo[i] && adelta < settings.nilm_sig_hi[i]) {
                        dev = settings.nilm_sig_name[i];
                        break;
                    }
                }
            }

            /* 3. Unknown — store in ring buffer for the Appliances screen */
            char fallback[24];
            if (!dev) {
                int slot = nilm_unknown_count % NILM_UNKNOWN_MAX;
                nilm_unknowns[slot].delta_w   = adelta;
                nilm_unknowns[slot].direction = (delta > 0.0f) ? +1 : -1;
                nilm_unknowns[slot].ts        = now;
                nilm_unknown_count++;
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
    meter_state.connected    = 1;
}

static void *watchdog_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(5);
        if (meter_state.last_flow_s == 0 ||
            time(NULL) - meter_state.last_flow_s > MET_STALE_S)
            meter_state.connected = 0;
    }
    return NULL;
}

int meteradapter_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, watchdog_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
