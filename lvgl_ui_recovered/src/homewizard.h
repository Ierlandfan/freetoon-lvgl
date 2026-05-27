#ifndef TOON_HOMEWIZARD_H
#define TOON_HOMEWIZARD_H

/* Live state from the two HomeWizard devices on VLAN99.
   Updated by homewizard_thread (poll every 5s). */
typedef struct {
    volatile int    connected_p1;
    /* 1 once a poll has been attempted (success or fail) — lets the UI show
     * "Initializing" before the first poll vs "offline" after one fails. */
    volatile int    polled_p1;
    volatile int    polled_water;
    volatile float  power_w;          /* active_power_w (signed; negative = export) */
    volatile float  kwh_import_t1;    /* total_power_import_t1_kwh */
    volatile float  kwh_import_t2;    /* total_power_import_t2_kwh */
    volatile float  kwh_import_total; /* total_power_import_kwh */
    volatile float  kwh_export_total; /* total_power_export_kwh */
    volatile int    tariff;           /* active_tariff: 1 or 2 */
    volatile float  gas_m3;           /* total_gas_m3 (cumulative) */
    volatile float  gas_hour_m3;      /* consumption over the trailing ~60 min,
                                         derived from the cumulative counter */
    volatile float  voltage_l1_v;
    volatile float  current_l1_a;

    volatile int    connected_water;
    volatile float  water_total_m3;   /* total_liter_m3 (cumulative m³) */
    volatile float  water_lpm;        /* active_liter_lpm */
    /* Per-pour session — set by the poller when L/min rises from 0 to >0,
     * finalised when it returns to 0 and stays there for ~20 s. Lets the
     * UI show "+12.3 L" so the user gets immediate feedback after using
     * water, without waiting for the next billing-cycle aggregate. */
    volatile float  water_session_l;       /* litres in the current/last pour */
    volatile int    water_session_active;  /* 1 while pouring, 0 between */
    volatile int    water_session_age_s;   /* seconds since session ended (for fade) */
} hw_state_t;

extern hw_state_t hw_state;

int homewizard_start(void);

#endif
