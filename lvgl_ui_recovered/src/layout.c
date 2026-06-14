/*
 * layout.c — persisted, grid-based home-tile layout model (Phase 0).
 *
 * Pure data + persistence + grid→pixel math. screen_home.c renders from
 * g_layout (Phase 1); the "Indeling" editor mutates + saves it (Phase 2).
 */
#define _GNU_SOURCE
#include "layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#ifdef TOON1
#  define SCREEN_W 800
#  define SCREEN_H 480
#else
#  define SCREEN_W 1024
#  define SCREEN_H 600
#endif

/* Data dir holding the layout files. Overridable via $TOONUI_DATA_DIR so the
 * headless sim (and tests) can read/write layouts outside /mnt/data. */
static const char * layout_dir(void) {
    const char * d = getenv("TOONUI_DATA_DIR");
    return (d && *d) ? d : "/mnt/data";
}

#define LAYOUT_PREFIX "toonui_layout"   /* default: toonui_layout.cfg; named: …_<name>.cfg */

/* Copy `name` into `out` keeping only filename-safe chars (alnum/-/_), spaces
 * folded to '_'. Empty/oversized → empty (= the default preset). */
static void sanitize_name(const char * name, char * out, int outsz) {
    int o = 0;
    if (name)
        for (const char * p = name; *p && o < outsz - 1; p++) {
            char c = *p;
            if (c == ' ') c = '_';
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_')
                out[o++] = c;
        }
    out[o] = 0;
}

/* Build the file path for preset `name` ("" = default). Returns `buf`. */
static const char * layout_path(const char * name, char * buf, int bufsz) {
    char safe[LAYOUT_NAME_MAX];
    sanitize_name(name, safe, sizeof safe);
    if (safe[0])
        snprintf(buf, bufsz, "%s/%s_%s.cfg", layout_dir(), LAYOUT_PREFIX, safe);
    else
        snprintf(buf, bufsz, "%s/%s.cfg", layout_dir(), LAYOUT_PREFIX);
    return buf;
}

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
    /* Ticker overlays the thermostat's bottom row (row 5); the forecast is a
     * single-row bar across the bottom (row 6). The home-screen forecast
     * scaler shrinks its 5 day-columns to fit one row, so it no longer needs
     * the old 2-row height. */
    { LT_NEWS_TICKER,   0,   0,  5,  7, 1, 1, -1 },
    { LT_FORECAST,      0,   0,  6, 12, 1, 1, -1 },
    /* news-summary + calendar are NOT preplaced (the grid is full) — add them
     * on demand via the editor's "+ Tegel" button, which drops them in a free
     * cell. */
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

void layout_load_named(const char * name) {
    char path[256];
    FILE * f = fopen(layout_path(name, path, sizeof path), "r");
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

void layout_save_named(const char * name) {
    char path[256];
    FILE * f = fopen(layout_path(name, path, sizeof path), "w");
    if (!f) return;
    for (int i = 0; i < g_layout.count; i++) {
        const layout_tile_t * t = &g_layout.tiles[i];
        fprintf(f, "tile=%d,%d,%d,%d,%d,%d,%d,%d\n",
                t->type, t->page, t->col, t->row, t->w, t->h, t->visible, t->slot);
    }
    fclose(f);
}

void layout_load(void) { layout_load_named(""); }
void layout_save(void) { layout_save_named(""); }

int layout_list_presets(char out[][LAYOUT_NAME_MAX], int max) {
    DIR * d = opendir(layout_dir());
    if (!d) return 0;
    const char * pre = LAYOUT_PREFIX "_";          /* "toonui_layout_" */
    size_t prelen = strlen(pre);
    int n = 0;
    struct dirent * e;
    while ((e = readdir(d)) && n < max) {
        const char * fn = e->d_name;
        size_t len = strlen(fn);
        if (strncmp(fn, pre, prelen) != 0) continue;          /* must be a NAMED preset */
        if (len < prelen + 4 || strcmp(fn + len - 4, ".cfg") != 0) continue;
        int nlen = (int)(len - prelen - 4);                   /* strip prefix + ".cfg" */
        if (nlen <= 0 || nlen >= LAYOUT_NAME_MAX) continue;
        memcpy(out[n], fn + prelen, nlen);
        out[n][nlen] = 0;
        n++;
    }
    closedir(d);
    return n;
}

void layout_delete_preset(const char * name) {
    char safe[LAYOUT_NAME_MAX];
    sanitize_name(name, safe, sizeof safe);
    if (!safe[0]) return;                  /* never delete the default file this way */
    char path[256];
    remove(layout_path(name, path, sizeof path));
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

/* Grid-rect intersection (half-open cells). */
static int rect_hits(int c, int r, int w, int h, const layout_tile_t * t) {
    return c < t->col + t->w && t->col < c + w &&
           r < t->row + t->h && t->row < r + h;
}

/* Strategy 1 — straight push-down: shove colliding tiles down their own column,
 * then gravity-compact up. Tidy + minimal disruption, but can't move tiles
 * sideways, so it FAILS when the moved tile needs space that's only free in
 * another column (e.g. dragging a big block across a packed grid). */
static int reflow_pushdown(layout_t * L, int moved) {
    /* Order the OTHER visible page-0 tiles top-to-bottom (then left-to-right):
     * we settle them in that order so higher tiles keep priority and lower ones
     * are the ones that get shoved down. */
    int pg = L->tiles[moved].page;     /* reflow only within the moved tile's page */
    int idx[LAYOUT_MAX_TILES], n = 0;
    for (int i = 0; i < L->count; i++) {
        if (i == moved) continue;
        const layout_tile_t * t = &L->tiles[i];
        if (t->page != pg || !t->visible) continue;
        idx[n++] = i;
    }
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++) {
            const layout_tile_t * ta = &L->tiles[idx[a]];
            const layout_tile_t * tb = &L->tiles[idx[b]];
            if (tb->row < ta->row || (tb->row == ta->row && tb->col < ta->col)) {
                int tmp = idx[a]; idx[a] = idx[b]; idx[b] = tmp;
            }
        }

    /* Push phase: place `moved` first (pinned), then drop each remaining tile in
     * its own column, sliding it down past anything already placed. */
    int placed[LAYOUT_MAX_TILES], pn = 0;
    placed[pn++] = moved;
    for (int k = 0; k < n; k++) {
        layout_tile_t * t = &L->tiles[idx[k]];
        int r = t->row;
        for (;;) {
            int hit = 0;
            for (int p = 0; p < pn && !hit; p++)
                hit = rect_hits(t->col, r, t->w, t->h, &L->tiles[placed[p]]);
            if (!hit) break;
            if (++r + t->h > LAYOUT_ROWS) return 0;   /* ran off the grid */
        }
        t->row = r;
        placed[pn++] = idx[k];
    }

    /* Gravity phase: pull each tile up as far as it can go (moved stays pinned —
     * it isn't in idx). Top-down order means the tile above is already settled. */
    for (int k = 0; k < n; k++) {
        layout_tile_t * t = &L->tiles[idx[k]];
        while (t->row > 0) {
            int rr = t->row - 1, hit = 0;
            for (int j = 0; j < L->count && !hit; j++) {
                if (j == idx[k]) continue;
                const layout_tile_t * o = &L->tiles[j];
                if (o->page != pg || !o->visible) continue;
                hit = rect_hits(t->col, rr, t->w, t->h, o);
            }
            if (hit) break;
            t->row = rr;
        }
    }
    return 1;
}

/* Strategy 2 — fill-any-hole re-pack: pin `moved`, keep every other tile that
 * doesn't clash with it where it is, and drop the displaced ones into the first
 * free cell anywhere (reading order). This relocates tiles sideways into the
 * space the moved tile vacated, so dragging a big block across a packed grid
 * rearranges instead of being rejected. Used only when push-down can't cope. */
static int reflow_firstfit(layout_t * L, int moved) {
    char occ[LAYOUT_ROWS][LAYOUT_COLS];
    memset(occ, 0, sizeof occ);
    layout_tile_t * m = &L->tiles[moved];
    int pg = m->page;                  /* re-pack only within the moved tile's page */
    if (m->col < 0 || m->row < 0 ||
        m->col + m->w > LAYOUT_COLS || m->row + m->h > LAYOUT_ROWS) return 0;
    for (int r = m->row; r < m->row + m->h; r++)
        for (int c = m->col; c < m->col + m->w; c++) occ[r][c] = 1;

    /* keep non-clashing tiles in place; collect the rest as displaced */
    int disp[LAYOUT_MAX_TILES], dn = 0;
    for (int i = 0; i < L->count; i++) {
        if (i == moved) continue;
        layout_tile_t * t = &L->tiles[i];
        if (t->page != pg || !t->visible) continue;
        int hit = 0;
        for (int r = t->row; r < t->row + t->h && !hit; r++)
            for (int c = t->col; c < t->col + t->w; c++) if (occ[r][c]) { hit = 1; break; }
        if (hit) disp[dn++] = i;
        else for (int r = t->row; r < t->row + t->h; r++)
                 for (int c = t->col; c < t->col + t->w; c++) occ[r][c] = 1;
    }
    /* settle displaced in reading order for a stable result */
    for (int a = 0; a < dn; a++)
        for (int b = a + 1; b < dn; b++) {
            const layout_tile_t * ta = &L->tiles[disp[a]];
            const layout_tile_t * tb = &L->tiles[disp[b]];
            if (tb->row < ta->row || (tb->row == ta->row && tb->col < ta->col)) {
                int tmp = disp[a]; disp[a] = disp[b]; disp[b] = tmp;
            }
        }
    for (int k = 0; k < dn; k++) {
        layout_tile_t * t = &L->tiles[disp[k]];
        int placed = 0;
        for (int r = 0; r <= LAYOUT_ROWS - t->h && !placed; r++)
            for (int c = 0; c <= LAYOUT_COLS - t->w && !placed; c++) {
                int free = 1;
                for (int rr = r; rr < r + t->h && free; rr++)
                    for (int cc = c; cc < c + t->w; cc++) if (occ[rr][cc]) { free = 0; break; }
                if (free) {
                    t->col = c; t->row = r;
                    for (int rr = r; rr < r + t->h; rr++)
                        for (int cc = c; cc < c + t->w; cc++) occ[rr][cc] = 1;
                    placed = 1;
                }
            }
        if (!placed) return 0;          /* genuinely no room left */
    }
    return 1;
}

int layout_reflow_push(layout_t * L, int moved) {
    /* Prefer the tidy push-down; fall back to fill-any-hole when it can't fit
     * (big block / packed grid). Each works on a copy so a failed attempt never
     * leaves L half-rearranged. */
    layout_t a = *L;
    if (reflow_pushdown(&a, moved)) { *L = a; return 1; }
    layout_t b = *L;
    if (reflow_firstfit(&b, moved)) { *L = b; return 1; }
    return 0;
}

void layout_type_min(int type, int * min_w, int * min_h) {
    /* {min_w, min_h} in grid cells. Height is the meaningful constraint: data
     * tiles that stack several rows of info need >=3 rows, so they can't be
     * dropped into a 2-row "Half"/"Breed" tile; ticker is a single-line strip;
     * the forecast renders fine as a single-row bar (the home-screen forecast
     * scaler shrinks its text to fit), so min_h=1. Tweak freely — purely a UI
     * guard, not persisted. */
    int w = 2, h = 2;
    switch (type) {
        case LT_THERMOSTAT:   w = 5; h = 4; break;
        case LT_FORECAST:     w = 4; h = 1; break;
        case LT_NEWS_TICKER:  w = 4; h = 1; break;
        case LT_NEWS_SUMMARY: w = 3; h = 3; break;
        case LT_CALENDAR:     w = 3; h = 3; break;
        case LT_ENERGY:       w = 3; h = 3; break;
        case LT_WATER:        w = 3; h = 2; break;
        case LT_VENT:         w = 2; h = 3; break;
        case LT_FAMILY:       w = 3; h = 2; break;
        case LT_WASTE:        w = 2; h = 3; break;
        case LT_LIGHTS:       w = 2; h = 2; break;
        case LT_SLOT:         w = 3; h = 3; break;
        default:              w = 0; h = 0; break;
    }
    if (min_w) *min_w = w;
    if (min_h) *min_h = h;
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
