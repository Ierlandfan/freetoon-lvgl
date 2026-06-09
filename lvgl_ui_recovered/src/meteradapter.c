/*
 * meteradapter.c — official energy source: the Toon's built-in smart-meter.
 * The meter is a Z-Wave HAE_METER node read by happ_pwrusage, which publishes
 * the live electricity flow over BoxTalk (ElectricityFlowMeter service). We
 * subscribe to that notify in boxtalk.c — meteradapter_on_flow() lands the
 * value here. No HTTP polling. A watchdog thread clears `connected` if the
 * notifies dry up (meter unplugged / not included in the Z-Wave network).
 */
#include "meteradapter.h"
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define MET_STALE_S 60   /* no flow notify for this long → meter offline */

meter_state_t meter_state = {0};

void meteradapter_on_flow(float watts) {
    meter_state.power_w   = watts;
    meter_state.last_flow_s = time(NULL);
    meter_state.connected = 1;
}

void meteradapter_on_gas_flow(float lph) {
    meter_state.gas_hour_m3   = lph / 1000.0f;   /* liters/hour -> m3/h */
    meter_state.last_gas_s    = time(NULL);
    meter_state.gas_connected = 1;
}

void meteradapter_on_gas_qty(float dm3) {
    meter_state.gas_m3        = dm3 / 1000.0f;    /* dm3 -> m3 */
    meter_state.last_gas_s    = time(NULL);
    meter_state.gas_connected = 1;
}

static void *watchdog_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(5);
        if (meter_state.last_flow_s == 0 ||
            time(NULL) - meter_state.last_flow_s > MET_STALE_S)
            meter_state.connected = 0;
        if (meter_state.last_gas_s == 0 ||
            time(NULL) - meter_state.last_gas_s > MET_STALE_S)
            meter_state.gas_connected = 0;
    }
    return NULL;
}

int meteradapter_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, watchdog_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
