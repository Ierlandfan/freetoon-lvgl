#ifndef TOON_HOMEASSISTANT_H
#define TOON_HOMEASSISTANT_H

/* Thin HTTP client for talking to Home Assistant via REST.
 * Auth is a Long-Lived Access Token read from /mnt/data/ha.cfg
 * (single line: just the token).
 *
 * Only the entities the LVGL UI actually shows are polled; everything
 * else is on-demand through the action helpers below. */

typedef struct {
    volatile int   connected;          /* 0 until first successful poll  */
    /* Curtain group `cover.gordijnen_voorkamer` (= "Gordijnen voorkamer"). */
    volatile int   curtain_pos;        /* 0..100 (current_position attr) */
    volatile int   curtain_is_closed;  /* 0/1 (is_closed attr)           */
    volatile int   curtain_battery;    /* % — min of the two child curtains */
    char           curtain_state[16];  /* "open" / "closed" / "opening" / "closing" / "unknown" */
    /* Life360 location for the two tracked people. "home" when at home,
     * else "<city/region> > <street> > <number>" formatted from the
     * device_tracker.life360_* address attribute. 128 chars to fit longer
     * Dutch city + street names without truncation. */
    char           loc_ronald[128];
    char           loc_caja[128];
} ha_state_t;

extern ha_state_t ha_state;

/* Per-light state. Hardcoded list of entities (see ha_lights[] in
 * homeassistant.c) — covers the rooms the user actually cares about.
 * State is polled in the same ha_thread; toggles are fire-and-forget. */
typedef struct {
    const char * entity_id;    /* "light.bank_lamp" */
    const char * name;         /* "Bank lamp" (display) */
    const char * area;         /* "Woonkamer" / "Keuken" / … (display group) */
    volatile int on;           /* 0/1 — 0 also covers unavailable */
    volatile int available;    /* 0 if HA says unavailable */
    volatile int brightness;   /* 0..255, -1 if not reported */
} ha_light_t;

#define HA_LIGHT_COUNT 14
extern ha_light_t ha_lights[HA_LIGHT_COUNT];

/* Start the poller (background thread, ~10s loop). Returns 0 on success. */
int ha_start(void);

/* Fire-and-forget actions on cover.gordijnen_voorkamer. Async — the HTTP
 * POST runs on a detached thread so LVGL stays responsive. */
void ha_curtain_open_async(void);
void ha_curtain_close_async(void);
void ha_curtain_stop_async(void);

/* Per-light + group actions. */
void ha_light_toggle_async(const char * entity_id);
void ha_lights_all_on_async(void);
void ha_lights_all_off_async(void);

#endif
