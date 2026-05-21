#ifndef TOON_TILE_SLOTS_H
#define TOON_TILE_SLOTS_H

/* Tile-reassignment registry. Phase-2 of the marketplace work.
 *
 * The four right-column home tiles (Energy / Family / Vent / Water)
 * each have a "slot id" that the user can bind to any installed
 * marketplace integration. When bound:
 *   - the tile's title + colour + icon come from the integration's
 *     manifest.json
 *   - the big number is whatever the integration last published in the
 *     manifest's `tile.value_field` over BoxTalk
 *   - the subtitle is the `tile.subtitle_field`
 *
 * Storage path: /mnt/data/toonui.cfg keys
 *   tile_slot_energy=<integration-id>
 *   tile_slot_family=<integration-id>
 *   tile_slot_vent=<integration-id>
 *   tile_slot_water=<integration-id>
 * Empty value → built-in behaviour for that slot (HomeWizard P1, Life360,
 * Itho, HWE-WTR respectively).
 *
 * BoxTalk dispatch — when toonui starts, tile_slots_init() scans
 * /mnt/data/integrations/<id>/manifest.json, registers each integration,
 * and subscribes to its serviceId. Notify frames update the runtime
 * value cache so refresh_cb can paint them. */

#include <stddef.h>

#define TILE_SLOT_ENERGY  0
#define TILE_SLOT_FAMILY  1
#define TILE_SLOT_VENT    2
#define TILE_SLOT_WATER   3
/* Page-2 (swipe) slots — independent assignable positions. */
#define TILE_SLOT_P1_0    4
#define TILE_SLOT_P1_1    5
#define TILE_SLOT_P1_2    6
#define TILE_SLOT_P1_3    7
#define TILE_SLOT_COUNT   8

#define INTEG_NAME_MAX        48
#define INTEG_ID_MAX          48
#define INTEG_SERVICE_MAX     48
#define INTEG_FIELD_MAX       32
#define INTEG_UNIT_MAX        16
#define INTEG_VALUE_MAX       64

typedef struct {
    char id[INTEG_ID_MAX];
    char name[INTEG_NAME_MAX];
    char service_id[INTEG_SERVICE_MAX];
    char tile_title[INTEG_NAME_MAX];
    char tile_color[12];               /* "0xRRGGBB" string from manifest */
    char tile_icon[16];                /* "sun" / "drop" / etc., maps via icons.h */
    char value_field[INTEG_FIELD_MAX];
    char value_unit[INTEG_UNIT_MAX];
    char subtitle_field[INTEG_FIELD_MAX];
    char subtitle_unit[INTEG_UNIT_MAX];

    /* Last value seen on BoxTalk for this integration's service. Updated
     * by tile_slots_on_notify() from a non-UI thread; refresh_cb reads
     * these straight (single-writer / single-reader on home-tile fields). */
    volatile char latest_value[INTEG_VALUE_MAX];
    volatile char latest_subtitle[INTEG_VALUE_MAX];
    volatile long  latest_epoch;
} integration_meta_t;

#define MAX_INSTALLED_INTEGRATIONS 16

/* Scan /mnt/data/integrations/<id>/manifest.json at boot; subscribe to each
 * service via BoxTalk. Idempotent — safe to call again to pick up new
 * installs without restarting. */
int  tile_slots_init(void);

/* How many integrations are installed right now. */
int  tile_slots_integration_count(void);

/* Read-only access to the i-th integration's metadata (0..count-1). */
const integration_meta_t * tile_slots_integration_at(int i);

/* Look up an integration by id. Returns NULL if not installed. */
const integration_meta_t * tile_slots_integration_by_id(const char * id);

/* Look up by serviceId (what BoxTalk notify frames carry). */
integration_meta_t * tile_slots_integration_by_service(const char * service_id);

/* Get the integration id currently bound to the given slot. NULL if
 * the slot is empty (built-in behaviour active). */
const char * tile_slots_binding(int slot);

/* Resolve a slot to its bound integration's metadata, or NULL. */
const integration_meta_t * tile_slots_meta_for(int slot);

/* Bind/unbind. Empty id (or NULL) unbinds. Persists via settings_save. */
void tile_slots_bind(int slot, const char * integration_id);

/* Called by boxtalk.c when a notify frame arrives for any service we
 * subscribed to. Looks up the matching integration and updates its
 * latest_value / latest_subtitle from the manifest's field names. */
void tile_slots_on_notify(const char * service_id, const char * xml);

/* Issue a BoxTalk subscribe for every registered integration's serviceId.
 * Called by boxtalk.c right after the handshake completes — also on every
 * reconnect, since subscriptions don't survive a broker socket drop. */
void tile_slots_subscribe_all(void);

/* Human-readable slot name for the Settings modal. */
const char * tile_slot_label(int slot);

#endif
