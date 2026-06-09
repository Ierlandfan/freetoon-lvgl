#ifndef TOON_METERADAPTER_H
#define TOON_METERADAPTER_H

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
    /* Gas — same happ_pwrusage publisher, GasFlowMeter service (the Toon's own
     * smart-meter gas; no HomeWizard P1 needed). */
    volatile float gas_hour_m3;   /* live gas flow, m3/h (CurrentGasFlow lph / 1000) */
    volatile float gas_m3;        /* cumulative gas, m3 (CurrentGasQuantity dm3 / 1000) */
    volatile int   gas_connected; /* 1 while gas notifies are fresh (<MET_STALE_S) */
    volatile long  last_gas_s;    /* time() of the last gas notify, 0 = never */
} meter_state_t;

extern meter_state_t meter_state;

int  meteradapter_start(void);
/* Called from the BoxTalk notify handler when an ElectricityFlowMeter
 * CurrentElectricityFlow value arrives. */
void meteradapter_on_flow(float watts);
/* GasFlowMeter notifies: CurrentGasFlow (liters/hour) + CurrentGasQuantity (dm3). */
void meteradapter_on_gas_flow(float lph);
void meteradapter_on_gas_qty(float dm3);

#endif
