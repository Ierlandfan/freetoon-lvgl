#ifndef TOON_BOXTALK_H
#define TOON_BOXTALK_H

#include <stdint.h>

/* Shared state — written by BoxTalk thread, read by LVGL main thread.
   Use volatile + atomic semantics for primitive types; the BoxTalk thread
   only ever assigns to these (no read-modify-write). LVGL widget updates
   happen in the main thread on a periodic timer. */
typedef struct {
    volatile int    connected;       /* 0/1 */
    volatile float  indoor_temp;     /* °C, ambient sensor */
    volatile float  boiler_temp;     /* °C, boiler return temp */
    volatile float  boiler_in_temp;  /* °C, BoilerInfo.flowwatertemperature */
    volatile float  boiler_out_temp; /* °C, BoilerInfo.returnwatertemperature */
    volatile float  water_pressure;  /* bar, central heating water pressure */
    volatile float  modulation_level;/* %, boiler modulation level (DID 17 / otCurrentModulationLevel) */
    volatile int    ot_comm_error;   /* 0/1, OT communication error flag */
    volatile int    boiler_type;     /* 0=OpenTherm, 1=On/Off; -1=unknown */
    volatile float  humidity;        /* %  */
    volatile int    eco2;            /* ppm */
    volatile int    tvoc;            /* ppb */
    volatile float  setpoint;        /* °C — current thermostat (room) setpoint */
    volatile float  ch_setpoint;     /* °C — control setpoint: target CH water
                                        temperature happ_thermstat is commanding
                                        the boiler to reach. Comes from
                                        BoilerInfo.ControlSetpoint notifies
                                        (published by quby_bridge after DID 1
                                        writes). Distinct from the room
                                        setpoint above. */
    volatile int    burner_on;       /* 0/1 boiler firing for CH */
    volatile int    dhw_on;          /* 0/1 boiler firing for hot water */
    volatile int    msg_count;       /* total notify messages received */
    volatile int    program_state;   /* 0=Comfort 1=Home 2=Sleep 3=Away, -1=unknown */
    volatile int    active_state;    /* -1=manual override (no schedule), else == program_state */
} toon_state_t;

/* Human-readable label for the effective current mode (handles manual override). */
const char* program_label(void);

/* Classify air quality from CO2 (ppm) + TVOC (ppb). Returns an empty
 * string if neither input is valid. Buckets:
 *   eCO2 (ppm):  <700 Excellent, <1000 Good, <1500 Fair, <2000 Poor, else Bad
 *   TVOC (ppb):  <100 Excellent, <300 Good, <500 Fair, <1000 Poor, else Bad
 *   final = worse of the two */
const char *  air_quality_label(int eco2, int tvoc);
unsigned int  air_quality_color(int eco2, int tvoc);

extern toon_state_t toon_state;

/* Start BoxTalk client in a background thread. Returns 0 on success. */
int boxtalk_start(void);

/* Send a setpoint change via SetThermostatSetpoint action.
   Called from main thread; thread-safe via internal lock. */
int boxtalk_set_setpoint(float temp);

/* Manual increase / decrease — these call ManualSetpointIncrease/Decrease
   actions on happ_thermstat (the same verbs qt-gui uses for +/- buttons). */
int boxtalk_setpoint_increase(void);
int boxtalk_setpoint_decrease(void);

void boxtalk_request_setpoint_refresh(void);

/* Force a fresh CurrentTemperature pull from happ_thermstat so the dim
   screen doesn't show a stale value when the TemperatureSensor notify
   subscription throttles. Called on screen wake. */
void boxtalk_request_indoor_refresh(void);

/* Query boiler flow/return temps (BoilerInfo is query-only, no notifies). */
void boxtalk_request_boiler_refresh(void);

/* Program/scheme switching. state in 0..3 (Comfort/Home/Sleep/Away).
   Pass -1 (or call boxtalk_set_manual) to force manual override. */
int boxtalk_set_program(int state);
int boxtalk_set_manual(void);
/* Resume schedule = pick the preset the schedule says is active right
 * now and call set_program(N). Used by the on-screen "Scheduled" toggle. */
int boxtalk_resume_schedule(void);
/* Drive the temporary-override expiry. Call from a 1 s UI tick. When a
 * +/- nudge is outstanding and the schedule's "current preset" rolls
 * over (next switch point passed), this auto-calls resume_schedule. */
void boxtalk_tick(void);
int  boxtalk_temp_override_active(void);
/* Returns the preset (0..3) the schedule was on when the override armed,
 * or -1 when no override is in flight. Used by the UI to keep the right
 * preset button highlighted while active_state is parked at -1. */
int  boxtalk_temp_override_origin(void);

/* Per-preset room-setpoint stored inside happ_thermstat. state in 0..3
 * (Comfort/Home/Sleep/Away). Get returns centi-°C (1850 = 18.5 °C) or -1
 * on error. Set takes centi-°C, clamps to [500, 3000]; returns 0 on
 * success. These are the temperatures the schedule daemon snaps to when
 * it transitions between presets. */
int boxtalk_get_state_value(int state);
int boxtalk_set_state_value(int state, int centi);

/* Request an RRD archive from hcb_rrd. Response arrives asynchronously
   and is stored in rrd_response_buf (NUL-terminated XML). Caller polls
   rrd_response_ready (0/1). After consuming, clear ready by writing 0. */
extern volatile int rrd_response_ready;
extern char         rrd_response_buf[16384];
int boxtalk_get_rra_data(const char * uuid, const char * rra_name);

/* Low-level send wrapper for callers that need to issue ad-hoc BoxTalk
   action XML (e.g. inbox.c's DeleteNotification). */
int boxtalk_send_raw_xml(const char * xml);

/* Boiler control type (OpenTherm vs On/Off). GetBoilerType is read-only;
   the response updates toon_state.boiler_type asynchronously.
   SetBoilerType WRITES to the live boiler: it persists config and pokes
   the OpenTherm gateway. type: 0=OpenTherm, 1=On/Off. */
int boxtalk_get_boiler_type(void);
int boxtalk_set_boiler_type(int type);

/* Low-level send wrapper for callers that need to issue ad-hoc BoxTalk
   action XML (e.g. inbox.c's DeleteNotification). */
int boxtalk_send_raw_xml(const char * xml);

/* Request an RRD archive from hcb_rrd. Response arrives asynchronously
   and is stored in rrd_response_buf (NUL-terminated XML). Caller polls
   rrd_response_ready (0/1). After consuming, clear ready by writing 0. */
extern volatile int rrd_response_ready;
extern char         rrd_response_buf[16384];
int boxtalk_get_rra_data(const char * uuid, const char * rra_name);

/* Program/scheme switching. state in 0..3 (Comfort/Home/Sleep/Away).
   Pass -1 (or call boxtalk_set_manual) to force manual override. */
int boxtalk_set_program(int state);
int boxtalk_set_manual(void);

/* Subscribe to an arbitrary serviceid. Used by tile_slots_init() so
 * marketplace integrations can deliver notifies through the same parser
 * path. Returns 0 on send success, <0 if the broker socket is down. */
int boxtalk_subscribe_service(const char * service_id);

#endif
