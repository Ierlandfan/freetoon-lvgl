#ifndef TOON_LAYOUT_H
#define TOON_LAYOUT_H

/* Customizable home-screen tile layout.
 *
 * Tiles are placed on a grid (LAYOUT_COLS × LAYOUT_ROWS) in *grid units*, not
 * pixels, so one layout scales to both Toon 2 (1024×600) and Toon 1 (800×480).
 * The renderer (screen_home) maps grid cells → pixels for the active screen.
 *
 * Persisted to /mnt/data/toonui_layout.cfg, one tile per line:
 *   tile=<type>,<page>,<col>,<row>,<w>,<h>,<visible>,<slot>
 * A missing/empty file seeds the built-in default arrangement, so behaviour is
 * unchanged until the user edits the layout in the "Indeling" editor. */

#define LAYOUT_MAX_TILES 24
#define LAYOUT_COLS      12      /* grid columns across the screen */
#define LAYOUT_ROWS       8      /* grid rows down the screen      */
#define LAYOUT_NAME_MAX  24      /* max preset-name length (incl. NUL) */
#define LAYOUT_MAX_PRESETS 12

/* Tile kinds the home screen can render. Keep LT_* stable — they're persisted
 * as integers in the layout file. Append new kinds at the end. */
typedef enum {
    LT_NONE = 0,
    LT_THERMOSTAT,      /* main setpoint/temperature block */
    LT_FORECAST,        /* weather forecast strip */
    LT_NEWS_TICKER,     /* scrolling RSS ticker */
    LT_NEWS_SUMMARY,    /* static multi-line headline summary */
    LT_CALENDAR,        /* next agenda event(s) */
    LT_ENERGY,          /* P1 electricity */
    LT_WATER,           /* HWE water */
    LT_VENT,            /* Itho ventilation */
    LT_FAMILY,          /* Life360 */
    LT_WASTE,           /* waste pickup */
    LT_LIGHTS,          /* lights shortcut */
    LT_SLOT,            /* assignable marketplace-integration slot (uses .slot) */
    LT_COUNT
} layout_tile_type_t;

typedef struct {
    int type;           /* layout_tile_type_t */
    int page;           /* swipe page index (0..) */
    int col, row;       /* top-left grid cell */
    int w, h;           /* span in grid cells */
    int visible;        /* 0 hides the tile */
    int slot;           /* for LT_SLOT: tile_slots.h slot id; else -1 */
} layout_tile_t;

typedef struct {
    int           count;
    layout_tile_t tiles[LAYOUT_MAX_TILES];
} layout_t;

extern layout_t g_layout;

void layout_load(void);            /* load the default preset, or seed defaults */
void layout_save(void);            /* save g_layout to the default preset file */
void layout_reset_default(void);   /* overwrite g_layout with the built-in default */

/* Named presets — files at <data>/toonui_layout_<name>.cfg. An empty name maps
 * to the default toonui_layout.cfg (same file layout_load/layout_save use). */
void layout_load_named(const char * name);
void layout_save_named(const char * name);
int  layout_list_presets(char out[][LAYOUT_NAME_MAX], int max);  /* -> count */
void layout_delete_preset(const char * name);

/* Map a grid rect → pixel rect for the active screen. */
void layout_cell_px(int col, int row, int w, int h,
                    int * x, int * y, int * pw, int * ph);

const char * layout_type_name(int type);   /* human label for the editor palette */

/* Smallest grid span (in cells) a tile type's content renders sensibly at.
 * Multi-row data tiles (energy, agenda, …) report a min height > 2 so the editor
 * refuses to put them in a short "Half"/"Breed" tile; strip tiles (ticker,
 * forecast) report min height 1. Writes 0 to both for an unknown type. */
void layout_type_min(int type, int * min_w, int * min_h);

/* First placed tile of `type`, or NULL if none. */
const layout_tile_t * layout_find(int type);

/* "Push & re-pack" reflow. `moved` is already at its desired col/row/w/h; shove
 * the other visible page-0 tiles straight down to clear any collision with it,
 * then pull them back up to close the gaps (moved stays pinned where the user
 * dropped it). Mutates L in place. Returns 1 on success, 0 if a tile can no
 * longer fit on the grid — in which case the caller should discard L and revert. */
int layout_reflow_push(layout_t * L, int moved);

#endif
