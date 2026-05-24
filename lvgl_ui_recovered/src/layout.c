/*
 * layout.c — persisted, grid-based home-tile layout model (Phase 0).
 *
 * Pure data + persistence + grid→pixel math. screen_home.c renders from
 * g_layout (Phase 1); the "Indeling" editor mutates + saves it (Phase 2).
 */
#define _GNU_SOURCE
#include "layout.h"
#include <stdio.h>
#include <string.h>

#ifdef TOON1
#  define SCREEN_W 800
#  define SCREEN_H 480
#else
#  define SCREEN_W 1024
#  define SCREEN_H 600
#endif

#define LAYOUT_PATH "/mnt/data/toonui_layout.cfg"

layout_t g_layout = {0};

/* Built-in default — mirrors the current hardcoded home arrangement closely
 * enough to be a sane starting point; the editor lets the user refine it.
 * Page 0: thermostat (left), the four right-column tiles, news ticker +
 * forecast across the bottom. Page 1: assignable slots. */
static const layout_tile_t DEFAULTS[] = {
    /* type,           page, col, row, w, h, vis, slot
     * Phase 1a only consults the five built-in tiles below; they live in the
     * right region (col>=7) so they clear the still-hardcoded thermostat (left)
     * and ticker/forecast (bottom). The thermostat/ticker/forecast/slot rows
     * are kept for the later full migration but aren't placed from here yet. */
    { LT_WASTE,         0,   7,  0,  2, 3, 1, -1 },
    { LT_VENT,          0,   7,  3,  2, 3, 1, -1 },
    { LT_ENERGY,        0,   9,  0,  3, 2, 1, -1 },
    { LT_FAMILY,        0,   9,  2,  3, 2, 1, -1 },
    { LT_WATER,         0,   9,  4,  3, 2, 1, -1 },
    { LT_THERMOSTAT,    0,   0,  0,  7, 6, 1, -1 },
    { LT_NEWS_TICKER,   0,   0,  6, 12, 1, 1, -1 },
    { LT_FORECAST,      0,   0,  7, 12, 1, 1, -1 },
    { LT_SLOT,          1,   0,  0,  6, 4, 1,  4 },   /* TILE_SLOT_P1_0 */
    { LT_SLOT,          1,   6,  0,  6, 4, 1,  5 },
    { LT_SLOT,          1,   0,  4,  6, 4, 1,  6 },
    { LT_SLOT,          1,   6,  4,  6, 4, 1,  7 },
};

void layout_reset_default(void) {
    g_layout.count = (int)(sizeof DEFAULTS / sizeof DEFAULTS[0]);
    if (g_layout.count > LAYOUT_MAX_TILES) g_layout.count = LAYOUT_MAX_TILES;
    memcpy(g_layout.tiles, DEFAULTS, g_layout.count * sizeof(layout_tile_t));
}

void layout_load(void) {
    FILE * f = fopen(LAYOUT_PATH, "r");
    if (!f) { layout_reset_default(); return; }
    g_layout.count = 0;
    char line[160];
    while (fgets(line, sizeof line, f) && g_layout.count < LAYOUT_MAX_TILES) {
        if (strncmp(line, "tile=", 5) != 0) continue;
        layout_tile_t t = { .slot = -1 };
        int got = sscanf(line + 5, "%d,%d,%d,%d,%d,%d,%d,%d",
                         &t.type, &t.page, &t.col, &t.row, &t.w, &t.h, &t.visible, &t.slot);
        if (got >= 7 && t.type > LT_NONE && t.type < LT_COUNT)
            g_layout.tiles[g_layout.count++] = t;
    }
    fclose(f);
    if (g_layout.count == 0) layout_reset_default();   /* empty/garbled → defaults */
}

void layout_save(void) {
    FILE * f = fopen(LAYOUT_PATH, "w");
    if (!f) return;
    for (int i = 0; i < g_layout.count; i++) {
        const layout_tile_t * t = &g_layout.tiles[i];
        fprintf(f, "tile=%d,%d,%d,%d,%d,%d,%d,%d\n",
                t->type, t->page, t->col, t->row, t->w, t->h, t->visible, t->slot);
    }
    fclose(f);
}

void layout_cell_px(int col, int row, int w, int h,
                    int * x, int * y, int * pw, int * ph) {
    /* Round so the right/bottom edges land exactly on the screen border
     * instead of accumulating per-cell rounding gaps. */
    int x0 = col * SCREEN_W / LAYOUT_COLS;
    int y0 = row * SCREEN_H / LAYOUT_ROWS;
    int x1 = (col + w) * SCREEN_W / LAYOUT_COLS;
    int y1 = (row + h) * SCREEN_H / LAYOUT_ROWS;
    if (x)  *x  = x0;
    if (y)  *y  = y0;
    if (pw) *pw = x1 - x0;
    if (ph) *ph = y1 - y0;
}

const layout_tile_t * layout_find(int type) {
    for (int i = 0; i < g_layout.count; i++)
        if (g_layout.tiles[i].type == type) return &g_layout.tiles[i];
    return NULL;
}

const char * layout_type_name(int type) {
    switch (type) {
        case LT_THERMOSTAT:  return "Thermostaat";
        case LT_FORECAST:    return "Weer";
        case LT_NEWS_TICKER: return "Nieuws (ticker)";
        case LT_NEWS_SUMMARY:return "Nieuws (overzicht)";
        case LT_CALENDAR:    return "Agenda";
        case LT_ENERGY:      return "Energie";
        case LT_WATER:       return "Water";
        case LT_VENT:        return "Ventilatie";
        case LT_FAMILY:      return "Familie";
        case LT_WASTE:       return "Afval";
        case LT_LIGHTS:      return "Verlichting";
        case LT_SLOT:        return "Integratie";
        default:             return "?";
    }
}
