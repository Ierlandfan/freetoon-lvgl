/*
 * canonical_state.h — FreeToon Canonical State Engine & Event Bus
 *
 * Single unified state representation and observer pub/sub engine for
 * FreeToon. Decouples UI screens, REST/SSE APIs, and hardware providers.
 */

#ifndef CANONICAL_STATE_H
#define CANONICAL_STATE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event bitmasks for change notifications */
#define CSTATE_EVT_SYSTEM      (1U << 0)
#define CSTATE_EVT_THERMOSTAT  (1U << 1)
#define CSTATE_EVT_BOILER      (1U << 2)
#define CSTATE_EVT_SENSORS     (1U << 3)
#define CSTATE_EVT_ENERGY      (1U << 4)
#define CSTATE_EVT_VENTILATION (1U << 5)
#define CSTATE_EVT_WEATHER     (1U << 6)
#define CSTATE_EVT_SCHEDULE    (1U << 7)
#define CSTATE_EVT_ALL         (0xFFFFU)

/* Canonical Thermostat Sub-State */
typedef struct {
    float room_temp;         /* Current room temperature (°C) */
    float setpoint;          /* Target room setpoint (°C) */
    float ch_setpoint;       /* Central heating control setpoint (°C) */
    int   active_state;      /* Live preset: 0=Comfort, 1=Home, 2=Sleep, 3=Away, -1=Manual */
    int   program_state;     /* 0=Manual, 1=Base schedule, 2=Temporary override */
    int   burner_on;         /* 1 if boiler is firing for heating */
    int   dhw_on;            /* 1 if boiler is firing for domestic hot water */
    int   connected;         /* 1 if thermostat hardware connection is healthy */
    int   ot_comm_error;     /* 1 if OpenTherm communication error is active */
} canonical_thermostat_t;

/* Canonical Boiler Sub-State */
typedef struct {
    int   protocol;          /* 0=OpenTherm, 1=On/Off */
    float modulation_pct;    /* Modulation percentage (0..100%) */
    float in_temp;           /* Boiler return (inlet) water temperature (°C) */
    float out_temp;          /* Boiler flow (outlet) water temperature (°C) */
    float water_pressure;    /* Water pressure in bar */
    int   burner_on;         /* 1 if firing for CH */
    int   dhw_on;            /* 1 if firing for DHW */
    int   comm_error;        /* Error code or 0 */
} canonical_boiler_t;

/* Canonical Environmental Sensors Sub-State */
typedef struct {
    float humidity;          /* Relative humidity (%) */
    int   eco2;              /* Equivalent CO2 in ppm */
    int   tvoc;              /* Total volatile organic compounds in ppb */
    const char * air_label;  /* Air quality rating: Excellent, Good, Fair, etc. */
} canonical_sensors_t;

/* Canonical Energy / Smart Meter Sub-State */
typedef struct {
    float power_w;           /* Live net electricity power (W) */
    float kwh_import_t1;     /* Total import tariff 1 (kWh) */
    float kwh_import_t2;     /* Total import tariff 2 (kWh) */
    float kwh_import_total;  /* Combined import (kWh) */
    float kwh_export_total;  /* Combined export (kWh) */
    float gas_m3;            /* Total gas meter reading (m³) */
    float gas_hour_m3;       /* Recent hourly gas usage (m³/h) */
    float voltage_l1;        /* Line voltage (V) */
    float current_l1;        /* Line current (A) */
    int   tariff;            /* Current active tariff (1 or 2) */
    int   connected;         /* 1 if P1 / meter data is streaming */
} canonical_energy_t;

/* Observer Callback Signature */
typedef void (*canonical_state_cb_t)(uint32_t mask, void * user_data);

/* Subscribe to state updates. Returns 0 on success, -1 if registry full. */
int canonical_state_subscribe(canonical_state_cb_t cb, void * user_data);

/* Unsubscribe an existing callback. */
int canonical_state_unsubscribe(canonical_state_cb_t cb);

/* Notify all subscribers that specific subsystems changed. */
void canonical_state_notify(uint32_t mask);

/* Snapshots */
void canonical_state_get_thermostat(canonical_thermostat_t * out);
void canonical_state_get_boiler(canonical_boiler_t * out);
void canonical_state_get_sensors(canonical_sensors_t * out);
void canonical_state_get_energy(canonical_energy_t * out);

/* Actions & Setters */
int canonical_state_set_setpoint(float temp_c);
int canonical_state_set_preset(int preset); /* 0: Comfort, 1: Home, 2: Sleep, 3: Away, -1: Manual */
int canonical_state_set_boiler_mode(int mode); /* 0: OpenTherm, 1: On/Off */

/* JSON Serialization for /api/v1/ endpoints */
int canonical_state_render_full_json(char * buf, size_t maxlen);
int canonical_state_render_thermostat_json(char * buf, size_t maxlen);
int canonical_state_render_boiler_json(char * buf, size_t maxlen);
int canonical_state_render_sensors_json(char * buf, size_t maxlen);
int canonical_state_render_energy_json(char * buf, size_t maxlen);

#ifdef __cplusplus
}
#endif

#endif /* CANONICAL_STATE_H */

