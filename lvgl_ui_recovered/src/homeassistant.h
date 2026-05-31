#ifndef TOON_HOMEASSISTANT_H
#define TOON_HOMEASSISTANT_H

#include <stddef.h>   /* size_t (ha_fetch_calendar) */

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
    /* Raw GPS for the map view (0 = unknown). From the device_tracker's
     * latitude/longitude attributes. */
    volatile float lat_a, lon_a;
    volatile float lat_b, lon_b;
    /* Doorbell — doorbell_seq is bumped once when the trigger goes off->on
     * (opens the overlay). While the overlay is up the UI sets doorbell_live=1,
     * which makes the poll thread re-fetch the camera snapshot ~1x/s and bump
     * doorbell_frame; the UI redraws the canvas on each new frame for near-live
     * footage. The UI clears doorbell_live when the overlay closes. */
    volatile int   doorbell_seq;
    volatile int   doorbell_frame;
    volatile int   doorbell_live;
    /* Blinds — second cover entity (settings.blinds_entity). Same shape as
     * the curtain fields above. */
    volatile int   blinds_pos;        /* 0..100 */
    volatile int   blinds_is_closed;  /* 0/1 */
    volatile int   blinds_battery;    /* % — min of the two child sensors */
    char           blinds_state[16];  /* "open" / "closed" / "opening" / "closing" / "unknown" */
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

/* ---- Unified dynamic device list ---------------------------------------
 * A single user-managed list of HA entities of mixed kinds, loaded from
 * /mnt/data/ha_devices.conf ("type|entity_id|Name|pin" per line). Replaces
 * the old fixed cover slots: covers, lights, switches, scripts and scenes
 * all live here. State for light/cover/switch is polled in ha_thread;
 * script/scene are stateless (a Run button). The Devices screen renders the
 * list; pin==1 devices also appear as quick-tiles on the home screen. */
enum { HADEV_LIGHT = 0, HADEV_COVER, HADEV_SWITCH, HADEV_SCRIPT, HADEV_SCENE };

typedef struct {
    int          type;           /* HADEV_* */
    char         entity_id[64];  /* "light.bank_lamp" / "cover.blind" / … */
    char         name[32];       /* display name */
    int          pin_home;       /* 0/1 — also show as a home quick-tile */
    /* live state (light/switch use on+brightness; cover uses position+state) */
    volatile int available;      /* 0 if HA says unavailable / unreachable */
    volatile int on;             /* 0/1 (light/switch) */
    volatile int brightness;     /* 0..255, -1 if not reported (light) */
    volatile int position;       /* 0..100 (cover) */
    char         state[16];      /* raw HA state ("open"/"closing"/…) (cover) */
} ha_device_t;

#define HA_DEVICE_MAX 32
extern ha_device_t ha_devices[HA_DEVICE_MAX];
extern int         ha_device_count;

/* "light"/"cover"/"switch"/"script"/"scene" <-> HADEV_*. */
const char * hadev_type_str(int type);
int          hadev_type_from_str(const char * s);

/* (Re)load the device list from ha_devices.conf (migrating the legacy
 * ha_lights.conf + curtain/blinds slots on first run). Safe to call from the
 * settings UI after an edit; the poller picks up the new list on its next
 * pass. */
void ha_devices_load(void);
/* Persist the current ha_devices[] back to ha_devices.conf. */
void ha_devices_save(void);

/* Device control (async, fire-and-forget). `type` selects the HA domain. */
void ha_device_toggle_async(int type, const char * entity_id);      /* light/switch */
void ha_device_cover_async(const char * entity_id, const char * cmd); /* "open"/"stop"/"close" */
void ha_device_run_async(int type, const char * entity_id);          /* script/scene */

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
void ha_light_set_brightness_async(const char * entity_id, int brightness_pct);

/* Cover position setter (generic — works for curtains and blinds). */
void ha_cover_set_position_async(const char * entity_id, int position_pct);
void ha_cover_stop_async(const char * entity_id);

/* GET /api/calendars/<entity>?start=&end= (Bearer auth) → HA event-array JSON
 * into `out`. Returns 0 on success. Used by calendar.c. */
int ha_fetch_calendar(const char * entity, const char * start_iso,
                      const char * end_iso, char * out, size_t out_max);

/* Entity discovery — GET /api/states and return all entities whose entity_id
 * starts with `domain_prefix` (e.g. "cover", "light", "sensor"). Fills out[]
 * up to max entries. *count is set to the number written. Returns 0 on success,
 * -1 on failure (HA unreachable, no token, etc.). */
#define HA_DISCOVERED_MAX 128
typedef struct {
    char entity_id[64];
    char friendly_name[64];
} ha_discovered_t;

int ha_discover_entities(const char * domain_prefix,
                          ha_discovered_t * out, int * count, int max);

#endif
