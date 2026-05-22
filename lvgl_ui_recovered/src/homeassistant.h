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
    /* Curtain cover group (settings.curtain_entity). */
    volatile int   curtain_pos;        /* 0..100 (current_position attr) */
    volatile int   curtain_is_closed;  /* 0/1 (is_closed attr)           */
    volatile int   curtain_battery;    /* % — min of the two child curtains */
    char           curtain_state[16];  /* "open" / "closed" / "opening" / "closing" / "unknown" */
    /* Life360 location for the two tracked people. "home" when at home,
     * else "<city/region> > <street> > <number>" formatted from the
     * device_tracker.life360_* address attribute. 128 chars to fit longer
     * Dutch city + street names without truncation. */
    char           loc_a[128];
    char           loc_b[128];
    /* Doorbell — doorbell_seq is bumped once when the trigger goes off->on
     * (opens the overlay). While the overlay is up the UI sets doorbell_live=1,
     * which makes the poll thread re-fetch the camera snapshot ~1x/s and bump
     * doorbell_frame; the UI redraws the canvas on each new frame for near-live
     * footage. The UI clears doorbell_live when the overlay closes. */
    volatile int   doorbell_seq;
    volatile int   doorbell_frame;
    volatile int   doorbell_live;
} ha_state_t;

/* Where poll_doorbell() writes the fetched JPEG (LVGL stdio drive 'S'). */
#define DOORBELL_SNAP_PATH "/tmp/toonui_doorbell.jpg"
#define DOORBELL_SNAP_LV   "S:" DOORBELL_SNAP_PATH

extern ha_state_t ha_state;

/* Per-light state. The list is loaded at runtime from
 * /mnt/data/ha_lights.conf (one "entity_id|Name|Area" per line) so no
 * personal entity ids ship in the binary. ha_light_count holds how many
 * rows were loaded (0 = none configured → the Lights screen is empty).
 * State is polled in the same ha_thread; toggles are fire-and-forget. */
typedef struct {
    char         entity_id[48];  /* "light.bank_lamp" */
    char         name[24];       /* "Bank lamp" (display) */
    char         area[20];       /* "Woonkamer" / "Keuken" / … (display group) */
    volatile int on;             /* 0/1 — 0 also covers unavailable */
    volatile int available;      /* 0 if HA says unavailable */
    volatile int brightness;     /* 0..255, -1 if not reported */
} ha_light_t;

#define HA_LIGHT_COUNT 32        /* array capacity (max rows from the conf) */
extern ha_light_t ha_lights[HA_LIGHT_COUNT];
extern int        ha_light_count;

/* Start the poller (background thread, ~10s loop). Returns 0 on success. */
int ha_start(void);

/* Fire-and-forget actions on the configured curtain cover. Async — the HTTP
 * POST runs on a detached thread so LVGL stays responsive. */
void ha_curtain_open_async(void);
void ha_curtain_close_async(void);
void ha_curtain_stop_async(void);

/* Per-light + group actions. */
void ha_light_toggle_async(const char * entity_id);
void ha_lights_all_on_async(void);
void ha_lights_all_off_async(void);

#endif
