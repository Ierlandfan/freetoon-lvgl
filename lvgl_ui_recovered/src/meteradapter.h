#ifndef TOON_METERADAPTER_H
#define TOON_METERADAPTER_H
#include <time.h>

/* Energy from the Toon's OWN built-in smart-meter — the official path.
 * The meter is a Z-Wave HAE_METER node; happ_pwrusage aggregates it and
 * publishes the live value on the BoxTalk ElectricityFlowMeter service
 * (CurrentElectricityFlow, W). We SUBSCRIBE to that notify rather than
 * polling — boxtalk.c calls meteradapter_on_flow() on each notify, and a
 * small watchdog thread marks the meter offline if notifies stop. */
typedef struct {
    volatile int   connected;    /* 1 while flow notifies are fresh (<MET_STALE_S) */
    volatile float power_w;      /* live electricity flow, W */
    volatile float avg_w;        /* average usage, W (if published) */
    volatile long  last_flow_s;  /* time() of the last flow notify, 0 = never */
    volatile float gas_today_m3; /* today's gas usage in m³, <0 = unavailable */
    volatile float gas_now_m3h;  /* current gas flow rate in m³/h (from gasUsage) */
    volatile float gas_m3;       /* cumulative total gas in m³ */
    /* NILM — step-change device detection */
    char   nilm_device[40];      /* matched device name + direction, e.g. "Fridge ON" */
    int    nilm_direction;       /* +1 = on, -1 = off */
    time_t nilm_event_ts;        /* epoch of last event, 0 = none yet */
} meter_state_t;

extern meter_state_t meter_state;

int  meteradapter_start(void);
/* Called from the BoxTalk notify handler when an ElectricityFlowMeter
 * CurrentElectricityFlow value arrives. */
void meteradapter_on_flow(float watts);
/* Query happ_pwrusage via local HTTP daemon (fallback / analog pulse meters) */
void probe_happ_pwrusage(void);

#endif
