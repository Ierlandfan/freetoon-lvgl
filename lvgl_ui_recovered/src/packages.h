#ifndef TOON_PACKAGES_H
#define TOON_PACKAGES_H

#include "lvgl/lvgl.h"

/* Poller thread + banner queue for the HA-side delivery tracker.
 *
 * - Background thread polls HA's sensor.pkg_state_map every 15s,
 *   diffs against the cached map, queues a banner per state advancement.
 * - packages_banner_attach(parent) creates a banner widget pinned to the
 *   top of `parent` (home screen + dim screen each get one). Widget is
 *   invisible when the queue is empty; tap dismisses the top entry. */

void packages_start(void);

/* Attach a banner overlay to a screen. Safe to call multiple times — each
 * call creates a fresh widget owned by `parent`. Use on screen-create. */
void packages_banner_attach(lv_obj_t * parent);

#define PACKAGES_BANNER_MAX 8

/* Per-banner queue entry. Exposed here so the master↔slave bridge can move
 * the whole queue as a fixed-layout struct array. */
typedef struct {
    char key[96];     /* merchant|order_id — dedup key */
    char title[64];
    char msg[160];
    char url[256];
} packages_banner_t;

/* Read-only queue accessors used by the bridge. */
int  packages_banner_count(void);
int  packages_banner_at(int i, packages_banner_t * out);

/* Slave/WASM bridge: replace the entire queue with `n` entries from `src`.
 * Idempotent; called from client_link_apply_state on each SSE frame so the
 * WASM client's banner widget shows what the master is queuing. */
void packages_set_banners_from_remote(int n, const packages_banner_t * src);

#endif
