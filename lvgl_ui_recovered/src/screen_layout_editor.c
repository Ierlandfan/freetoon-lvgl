/*
 * screen_layout_editor.c — "Indeling" (home-tile layout) editor. Phase 2.
 *
 * Full-screen modal that edits a working copy of the page-0 grid tiles
 * (layout.c). Drag a tile to move it: instead of blocking an overlapping drop,
 * the other tiles are pushed out of the way and the grid re-packs upward
 * (layout_reflow_push) — the moved tile stays where you drop it, and the reflow
 * is previewed live while you drag (green = fits, red = no room). The +/- buttons
 * resize the selected tile (also reflowing), the eye button hides/shows it, and
 * "+ Tegel" opens a palette to insert a tile at a chosen size (Klein/Half/Groot/
 * Breed). Save writes the layout, enables custom_layout_enabled, and restarts the
 * UI to apply; Cancel discards. 1:1 on the real screen — WYSIWYG.
 */
#include "lvgl/lvgl.h"
#include "screens.h"
#include "display.h"   /* SF()/SX()/SY() scaling for Toon 1 */
#include "layout.h"
#include "settings.h"
#include "i18n.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#ifdef TOON1
#  define SCR_W 800
#  define SCR_H 480
#else
#  define SCR_W 1024
#  define SCR_H 600
#endif
#define BAR_H  64
/* The editor canvas is the area ABOVE the toolbar, so a cell's height is based
 * on (SCR_H - BAR_H): all LAYOUT_ROWS rows fit above the bar and nothing is
 * hidden behind it. Width is unaffected (the bar is full-width at the bottom).
 * Only the editor's preview scaling changes — the saved layout (grid units) and
 * the real home rendering (layout_cell_px, full-screen) are untouched. */
#define CANVAS_H (SCR_H - BAR_H)
#define CELL_W (SCR_W / LAYOUT_COLS)
#define CELL_H (CANVAS_H / LAYOUT_ROWS)

static lv_obj_t * modal;
static lv_obj_t * chooser;      /* the "+ Tegel" size/type palette, when open */
static lv_obj_t * rects[LAYOUT_MAX_TILES];
static layout_t   edit;
static int        sel = -1;
static int        edit_page = 0;   /* which page the editor is currently editing */
static lv_obj_t * sel_lbl;
static lv_obj_t * page_btn;        /* toolbar page-switch button (label shows current page) */
static void build_canvas_rects(void);   /* fwd */
static void on_copy_p2(lv_event_t * e);  /* fwd */

/* insert-palette size presets (grid units) */
static const struct { const char * name; int w, h; } SIZES[] = {
    { "Klein\n3x2",  3, 2 },
    { "Half\n6x2",   6, 2 },
    { "Groot\n6x4",  6, 4 },
    { "Breed\n12x2", 12, 2 },
};
#define N_SIZES ((int)(sizeof SIZES / sizeof SIZES[0]))
static int        pick_w = 6, pick_h = 2;   /* currently-selected insert size */
static lv_obj_t * size_btns[N_SIZES];
static lv_obj_t * type_grid;                 /* palette's type-button container */
static lv_obj_t * preset_mgr;                /* "Indelingen" preset-manager modal */
static lv_obj_t * preset_list;               /* scrollable list inside preset_mgr */
static lv_obj_t * name_modal;                /* keyboard modal for "Opslaan als…" */
static lv_obj_t * name_ta;                   /* its text area */

/* tile types offerable in the insert palette (LT_SLOT is page-1 only). */
static const int PALETTE[] = {
    LT_THERMOSTAT, LT_FORECAST, LT_NEWS_TICKER, LT_NEWS_SUMMARY, LT_CALENDAR,
    LT_ENERGY, LT_WATER, LT_VENT, LT_FAMILY, LT_WASTE, LT_LIGHTS,
};
#define N_PALETTE ((int)(sizeof PALETTE / sizeof PALETTE[0]))

static const uint32_t TYPE_COL[LT_COUNT] = {
    [LT_THERMOSTAT]=0x335577, [LT_FORECAST]=0x4488aa, [LT_NEWS_TICKER]=0x666688,
    [LT_NEWS_SUMMARY]=0x6666aa, [LT_CALENDAR]=0x4477cc, [LT_ENERGY]=0xaa77ff,
    [LT_WATER]=0x44aaff, [LT_VENT]=0x66bbdd, [LT_FAMILY]=0xff8866,
    [LT_WASTE]=0x88dd66, [LT_LIGHTS]=0xddaa44, [LT_SLOT]=0x778899,
};

static void place_rect(int i);
static void create_rect(int i);
static void update_sel_label(void);

/* True if grid rect (c,r,w,h) overlaps any OTHER visible page-0 tile.
 * Still used by the "find a free hole" / un-hide paths. */
static int would_overlap(int idx, int c, int r, int w, int h) {
    for (int j = 0; j < edit.count; j++) {
        if (j == idx) continue;
        layout_tile_t * t = &edit.tiles[j];
        if (t->page != edit_page || !t->visible) continue;
        if (c < t->col + t->w && t->col < c + w &&
            r < t->row + t->h && t->row < r + h) return 1;
    }
    return 0;
}

/* First free top-left cell that fits a w*h tile (excluding tile idx, -1=none). */
static int find_free_cell(int idx, int w, int h, int * oc, int * orow) {
    for (int r = 0; r <= LAYOUT_ROWS - h; r++)
        for (int c = 0; c <= LAYOUT_COLS - w; c++)
            if (!would_overlap(idx, c, r, w, h)) { *oc = c; *orow = r; return 1; }
    return 0;
}

/* Grid cell the rect's current pixel position rounds to, clamped on-screen. */
static void snap_cell(int i, int * out_col, int * out_row) {
    lv_obj_t * r = rects[i];
    int w = edit.tiles[i].w, h = edit.tiles[i].h;
    int col = (lv_obj_get_x(r) + CELL_W / 2) / CELL_W;
    int row = (lv_obj_get_y(r) + CELL_H / 2) / CELL_H;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (col + w > LAYOUT_COLS) col = LAYOUT_COLS - w;
    if (row + h > LAYOUT_ROWS) row = LAYOUT_ROWS - h;
    *out_col = col; *out_row = row;
}

/* Try moving tile i to (col,row) and re-pack the rest into `out`.
 * Returns 1 if the whole arrangement fits the grid. */
static int trial_move(int i, int col, int row, layout_t * out) {
    *out = edit;
    out->tiles[i].col = col;
    out->tiles[i].row = row;
    return layout_reflow_push(out, i);
}

/* Position every rect EXCEPT `except` from layout `src` (used for live preview
 * of where the other tiles re-pack to while dragging). */
static void preview_others(const layout_t * src, int except) {
    for (int k = 0; k < edit.count; k++) {
        if (k == except || !rects[k]) continue;
        const layout_tile_t * t = &src->tiles[k];
        lv_obj_set_pos(rects[k], t->col * CELL_W, t->row * CELL_H);
    }
}

static void select_tile(int i) {
    sel = i;
    for (int k = 0; k < edit.count; k++) {
        if (!rects[k]) continue;
        lv_obj_set_style_border_width(rects[k], k == i ? 4 : 0, 0);
        lv_obj_set_style_border_color(rects[k], lv_color_hex(0xffffff), 0);
    }
    update_sel_label();
}

static int drag_last_col = -1, drag_last_row = -1;
static layout_t drag_preview;
static int       drag_preview_ok;
static int       long_pressed;            /* held long enough on this press */
static int       drag_moved;              /* finger moved → it's a drag, never a delete */
static int       press_x0, press_y0;      /* rect pixel pos when the press started */
static void show_delete_confirm(int i);   /* fwd */

static void rect_event(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * r = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(r);
    if (code == LV_EVENT_PRESSED) {
        select_tile(i);
        drag_last_col = drag_last_row = -1;
        long_pressed = drag_moved = 0;
        press_x0 = lv_obj_get_x(r); press_y0 = lv_obj_get_y(r);
    } else if (code == LV_EVENT_LONG_PRESSED) {
        /* Just mark it. The delete is offered on RELEASE and only if the tile was
         * NOT dragged, so a long-press-then-drag stays a drag, never a delete. */
        long_pressed = 1;
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t * in = lv_indev_get_act();
        lv_point_t v; lv_indev_get_vect(in, &v);
        lv_obj_set_pos(r, lv_obj_get_x(r) + v.x, lv_obj_get_y(r) + v.y);
        int dx = lv_obj_get_x(r) - press_x0, dy = lv_obj_get_y(r) - press_y0;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > 10 || dy > 10) drag_moved = 1;   /* moved enough → it's a drag */
        if (!drag_moved) return;                  /* still a tap: don't preview/re-pack */
        /* Re-evaluate only when the snap target cell changes — cheap + smooth. */
        int col, row; snap_cell(i, &col, &row);
        if (col != drag_last_col || row != drag_last_row) {
            drag_last_col = col; drag_last_row = row;
            drag_preview_ok = trial_move(i, col, row, &drag_preview);
            lv_obj_set_style_border_width(r, 4, 0);
            lv_obj_set_style_border_color(r,
                lv_color_hex(drag_preview_ok ? 0x33dd66 : 0xdd4444), 0);
            /* Live re-pack the other tiles (or snap them back if it won't fit). */
            preview_others(drag_preview_ok ? &drag_preview : &edit, i);
        }
    } else if (code == LV_EVENT_RELEASED) {
        if (long_pressed && !drag_moved) {   /* held in place, no drag → confirm delete */
            for (int k = 0; k < edit.count; k++) if (rects[k]) place_rect(k);
            select_tile(i);
            show_delete_confirm(i);
            drag_last_col = drag_last_row = -1;
            return;
        }
        if (!drag_moved) {                   /* a plain tap → just select, no move */
            place_rect(i);                   /* undo any sub-threshold jitter */
            select_tile(i);                  /* selection persists for the W+/H+ buttons */
            drag_last_col = drag_last_row = -1;
            return;
        }
        int col, row; snap_cell(i, &col, &row);
        layout_t res;
        if (trial_move(i, col, row, &res)) edit = res;   /* commit re-packed layout */
        /* else: edit untouched → everything snaps back to where it was */
        for (int k = 0; k < edit.count; k++) if (rects[k]) place_rect(k);
        select_tile(i);
        drag_last_col = drag_last_row = -1;
    } else if (code == LV_EVENT_PRESS_LOST) {
        /* touch interrupted mid-press: restore tiles + keep the selection, so we
         * never leave a stuck green-border / half-dragged state. */
        for (int k = 0; k < edit.count; k++) if (rects[k]) place_rect(k);
        select_tile(i);
        drag_last_col = drag_last_row = -1;
    }
}

static void place_rect(int i) {
    layout_tile_t * t = &edit.tiles[i];
    lv_obj_t * r = rects[i];
    if (!r) return;
    lv_obj_set_pos(r, t->col * CELL_W, t->row * CELL_H);
    lv_obj_set_size(r, t->w * CELL_W - 4, t->h * CELL_H - 4);
    lv_obj_set_style_bg_opa(r, t->visible ? LV_OPA_COVER : 60, 0);
}

/* Build the draggable rect for tile i (page-0 only). */
static void create_rect(int i) {
    layout_tile_t * t = &edit.tiles[i];
    if (t->page != edit_page) { rects[i] = NULL; return; }
    lv_obj_t * r = lv_obj_create(modal);
    rects[i] = r;
    lv_obj_set_user_data(r, (void *)(intptr_t)i);
    lv_obj_set_style_bg_color(r, lv_color_hex(TYPE_COL[t->type % LT_COUNT]), 0);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_set_style_pad_all(r, 4, 0);
    /* SCROLL_CHAIN off too: without it, dragging a tile is forwarded as a scroll
     * gesture up to the scrollable home screen behind the modal, so the whole
     * screen scrolls along with the drag. */
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, rect_event, LV_EVENT_ALL, NULL);
    lv_obj_t * l = lv_label_create(r);
    lv_label_set_text(l, layout_type_name(t->type));
    lv_obj_set_style_text_color(l, lv_color_hex(0x0a121e), 0);
    lv_obj_set_style_text_font(l, SF(14), 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 2, 2);
    place_rect(i);
}

static void update_sel_label(void) {
    if (!sel_lbl) return;
    if (sel < 0) { lv_label_set_text(sel_lbl, tr("Tik een tegel om te selecteren", "Tap a tile to select")); return; }
    layout_tile_t * t = &edit.tiles[sel];
    lv_label_set_text_fmt(sel_lbl, "%s  %dx%d  %s",
        layout_type_name(t->type), t->w, t->h, t->visible ? "" : tr("(verborgen)", "(hidden)"));
}

/* ---- insert palette ("+ Tegel") --------------------------------------- */

/* Insert a tile of `type` at the picked size: drop it in the first free hole;
 * if the grid is full, place it top-left and push the rest down (re-pack). */
static void do_insert(int type, int w, int h) {
    if (edit.count >= LAYOUT_MAX_TILES) { lv_label_set_text(sel_lbl, tr("Maximum bereikt", "Maximum reached")); return; }
    if (w > LAYOUT_COLS) w = LAYOUT_COLS;
    if (h > LAYOUT_ROWS) h = LAYOUT_ROWS;
    int i = edit.count;
    int c, r;
    if (find_free_cell(-1, w, h, &c, &r)) {
        edit.tiles[i] = (layout_tile_t){ .type = type, .page = edit_page, .col = c, .row = r,
                                         .w = w, .h = h, .visible = 1, .slot = -1 };
        edit.count++;
    } else {
        layout_t s = edit;
        s.tiles[i] = (layout_tile_t){ .type = type, .page = edit_page, .col = 0, .row = 0,
                                      .w = w, .h = h, .visible = 1, .slot = -1 };
        s.count++;
        if (!layout_reflow_push(&s, i)) {
            lv_label_set_text(sel_lbl, tr("Geen ruimte - kies een kleinere tegel of verberg er een", "No room - pick a smaller tile or hide one"));
            return;
        }
        edit = s;
    }
    create_rect(i);
    for (int k = 0; k < edit.count; k++) if (rects[k]) place_rect(k);  /* others may have moved */
    select_tile(i);
}

static void close_chooser(void) { if (chooser) { lv_obj_del(chooser); chooser = NULL; type_grid = NULL; } }

static void on_type_pick(lv_event_t * e);   /* fwd */

/* (Re)fill the palette's type list with the types that aren't already placed
 * AND whose content fits the currently-picked size (pick_w x pick_h). */
static void populate_types(void) {
    if (!type_grid) return;
    lv_obj_clean(type_grid);
    int added = 0;
    for (int p = 0; p < N_PALETTE; p++) {
        int type = PALETTE[p];
        int present = 0;
        for (int k = 0; k < edit.count; k++)
            if (edit.tiles[k].type == type && edit.tiles[k].page == edit_page) { present = 1; break; }
        if (present) continue;
        int mw, mh; layout_type_min(type, &mw, &mh);
        if (pick_w < mw || pick_h < mh) continue;   /* too small for this content */
        lv_obj_t * b = lv_btn_create(type_grid);
        lv_obj_set_size(b, SCR_W * 72 / 100 / 3 - 18, 52);
        lv_obj_set_style_bg_color(b, lv_color_hex(TYPE_COL[type % LT_COUNT]), 0);
        lv_obj_add_event_cb(b, on_type_pick, LV_EVENT_CLICKED, (void *)(intptr_t)type);
        lv_obj_t * l = lv_label_create(b);
        lv_label_set_text(l, layout_type_name(type));
        lv_obj_set_style_text_color(l, lv_color_hex(0x0a121e), 0);
        lv_obj_center(l);
        added++;
    }
    if (!added) {
        lv_obj_t * l = lv_label_create(type_grid);
        lv_label_set_text(l, tr("Geen types passen in deze grootte - kies een grotere tegel", "No types fit this size - pick a larger tile"));
        lv_obj_set_style_text_color(l, lv_color_hex(0xccddee), 0);
    }
}

static void on_size_pick(lv_event_t * e) {
    int s = (int)(intptr_t)lv_event_get_user_data(e);
    pick_w = SIZES[s].w; pick_h = SIZES[s].h;
    for (int k = 0; k < N_SIZES; k++)
        lv_obj_set_style_border_width(size_btns[k], k == s ? 3 : 0, 0);
    populate_types();   /* the eligible type list depends on the picked size */
}
static void on_type_pick(lv_event_t * e) {
    int type = (int)(intptr_t)lv_event_get_user_data(e);
    close_chooser();
    do_insert(type, pick_w, pick_h);
}
static void on_chooser_close(lv_event_t * e) { (void)e; close_chooser(); }

static void on_add(lv_event_t * e) {
    (void)e;
    if (chooser) return;
    if (edit.count >= LAYOUT_MAX_TILES) { lv_label_set_text(sel_lbl, tr("Maximum bereikt", "Maximum reached")); return; }

    chooser = lv_obj_create(modal);
    lv_obj_set_size(chooser, SCR_W * 72 / 100, SCR_H * 84 / 100);
    lv_obj_center(chooser);
    lv_obj_set_style_bg_color(chooser, lv_color_hex(0x16263a), 0);
    lv_obj_set_style_border_color(chooser, lv_color_hex(0x33506e), 0);
    lv_obj_set_style_border_width(chooser, 2, 0);
    lv_obj_set_style_radius(chooser, 12, 0);
    lv_obj_set_style_pad_all(chooser, 12, 0);
    lv_obj_set_flex_flow(chooser, LV_FLEX_FLOW_COLUMN);

    lv_obj_t * title = lv_label_create(chooser);
    lv_label_set_text(title, tr("Tegel toevoegen  -  kies grootte, dan type", "Add tile  -  pick size, then type"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(title, SF(14), 0);

    /* size row */
    lv_obj_t * srow = lv_obj_create(chooser);
    lv_obj_set_width(srow, LV_PCT(100));
    lv_obj_set_height(srow, 70);
    lv_obj_set_style_bg_opa(srow, 0, 0);
    lv_obj_set_style_border_width(srow, 0, 0);
    lv_obj_set_style_pad_all(srow, 2, 0);
    lv_obj_clear_flag(srow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(srow, LV_FLEX_FLOW_ROW);
    for (int s = 0; s < N_SIZES; s++) {
        lv_obj_t * b = lv_btn_create(srow);
        size_btns[s] = b;
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, 58);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2a4060), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_border_width(b, (SIZES[s].w == pick_w && SIZES[s].h == pick_h) ? 3 : 0, 0);
        lv_obj_add_event_cb(b, on_size_pick, LV_EVENT_CLICKED, (void *)(intptr_t)s);
        lv_obj_t * l = lv_label_create(b);
        lv_label_set_text(l, SIZES[s].name);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
    }

    /* type grid (wraps; scrolls if it overflows) — filled by populate_types(),
     * which re-runs whenever the picked size changes. */
    type_grid = lv_obj_create(chooser);
    lv_obj_set_width(type_grid, LV_PCT(100));
    lv_obj_set_flex_grow(type_grid, 1);
    lv_obj_set_style_bg_opa(type_grid, 0, 0);
    lv_obj_set_style_border_width(type_grid, 0, 0);
    lv_obj_set_style_pad_all(type_grid, 2, 0);
    lv_obj_set_flex_flow(type_grid, LV_FLEX_FLOW_ROW_WRAP);
    populate_types();

    lv_obj_t * close = lv_btn_create(chooser);
    lv_obj_set_width(close, LV_PCT(100));
    lv_obj_set_height(close, 46);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x444444), 0);
    lv_obj_add_event_cb(close, on_chooser_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(close);
    lv_label_set_text(cl, tr("Annuleer", "Cancel"));
    lv_obj_center(cl);
}

/* ---- toolbar actions -------------------------------------------------- */
static void on_resize(lv_event_t * e) {
    if (sel < 0) return;
    int d = (int)(intptr_t)lv_event_get_user_data(e);   /* 0:w- 1:w+ 2:h- 3:h+ */
    layout_tile_t * t = &edit.tiles[sel];
    int w = t->w, h = t->h, col = t->col, row = t->row;
    int mw, mh; layout_type_min(t->type, &mw, &mh);
    if (mw < 1) mw = 1;
    if (mh < 1) mh = 1;
    if (d == 0 && w > mw) w--;                       /* W-: shrink from the right */
    if (d == 1) {                                    /* W+: grow right, or left if at the edge */
        if (col + w < LAYOUT_COLS) w++;
        else if (col > 0) { col--; w++; }            /* right edge → extend leftward instead */
    }
    if (d == 2 && h > mh) h--;                        /* H- */
    if (d == 3) {                                    /* H+: grow down, or up if at the edge */
        if (row + h < LAYOUT_ROWS) h++;
        else if (row > 0) { row--; h++; }
    }
    if (w == t->w && h == t->h && col == t->col && row == t->row) {
        lv_label_set_text(sel_lbl, tr("Kan niet verder - min. grootte of schermrand bereikt", "Can't go further - min. size or screen edge reached"));
        return;
    }
    /* Re-pack the rest around the new size; revert if it can't be made to fit. */
    layout_t s = edit;
    s.tiles[sel].w = w; s.tiles[sel].h = h; s.tiles[sel].col = col; s.tiles[sel].row = row;
    if (layout_reflow_push(&s, sel)) {
        edit = s;
        for (int k = 0; k < edit.count; k++) if (rects[k]) place_rect(k);
        select_tile(sel);   /* keep the selection visible on the resized tile */
    }
    update_sel_label();
}
static void on_toggle_vis(lv_event_t * e) {
    (void)e;
    if (sel < 0) return;
    layout_tile_t * t = &edit.tiles[sel];
    if (!t->visible) {
        /* Turning a tile back ON must not stack it onto whatever took its cell
         * while it was hidden. If the old spot is occupied, relocate to a free
         * cell; if there's none, keep it hidden. */
        if (would_overlap(sel, t->col, t->row, t->w, t->h)) {
            int c, r;
            if (find_free_cell(sel, t->w, t->h, &c, &r)) { t->col = c; t->row = r; }
            else { lv_label_set_text(sel_lbl, tr("Geen ruimte om te tonen - verberg eerst iets", "No room to show - hide something first")); return; }
        }
    }
    t->visible = !t->visible;
    place_rect(sel); update_sel_label();
}
/* Remove tile `i` entirely (not just hide it): frees its grid cell AND its type,
 * so a smaller tile of any type can be added in its place. Keeps the parallel
 * edit.tiles[]/rects[] arrays in sync by compacting both. */
static void delete_tile(int i) {
    if (i < 0 || i >= edit.count) return;
    if (rects[i]) { lv_obj_del(rects[i]); rects[i] = NULL; }
    for (int k = i; k < edit.count - 1; k++) {
        edit.tiles[k] = edit.tiles[k + 1];
        rects[k] = rects[k + 1];
        if (rects[k]) lv_obj_set_user_data(rects[k], (void *)(intptr_t)k);  /* fix stored index */
    }
    edit.count--;
    rects[edit.count] = NULL;
    sel = -1;
    update_sel_label();
}

/* Confirm dialog (also opened by a long-press on a tile). */
static int        confirm_idx = -1;
static lv_obj_t * confirm_modal;
static void confirm_close(void) { if (confirm_modal) { lv_obj_del(confirm_modal); confirm_modal = NULL; } confirm_idx = -1; }
static void on_confirm_cancel(lv_event_t * e) { (void)e; confirm_close(); }
static void on_confirm_ok(lv_event_t * e) { (void)e; int i = confirm_idx; confirm_close(); delete_tile(i); }

static void show_delete_confirm(int i) {
    if (confirm_modal || i < 0 || i >= edit.count) return;
    confirm_idx = i;
    confirm_modal = lv_obj_create(modal);          /* full-screen input-capturing backdrop */
    lv_obj_set_size(confirm_modal, SCR_W, SCR_H);
    lv_obj_set_pos(confirm_modal, 0, 0);
    lv_obj_set_style_bg_color(confirm_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(confirm_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(confirm_modal, 0, 0);
    lv_obj_clear_flag(confirm_modal, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t * card = lv_obj_create(confirm_modal);
    lv_obj_set_size(card, SCR_W * 52 / 100, 180);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x16263a), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x33506e), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * t = lv_label_create(card);
    lv_obj_set_style_text_color(t, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(t, SF(18), 0);
    lv_label_set_text_fmt(t, tr("%s verwijderen?", "Delete %s?"), layout_type_name(edit.tiles[i].type));
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t * del = lv_btn_create(card);
    lv_obj_set_size(del, SX(150), SY(50));
    lv_obj_align(del, LV_ALIGN_BOTTOM_RIGHT, SX(-10), SY(-14));
    lv_obj_set_style_bg_color(del, lv_color_hex(0x6e2e2e), 0);
    lv_obj_add_event_cb(del, on_confirm_ok, LV_EVENT_CLICKED, NULL);
    lv_obj_t * dl = lv_label_create(del); lv_label_set_text(dl, tr("Verwijder", "Delete")); lv_obj_center(dl);

    lv_obj_t * ca = lv_btn_create(card);
    lv_obj_set_size(ca, SX(150), SY(50));
    lv_obj_align(ca, LV_ALIGN_BOTTOM_LEFT, SX(10), SY(-14));
    lv_obj_set_style_bg_color(ca, lv_color_hex(0x444444), 0);
    lv_obj_add_event_cb(ca, on_confirm_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cal = lv_label_create(ca); lv_label_set_text(cal, tr("Annuleer", "Cancel")); lv_obj_center(cal);
}

/* Toolbar "Verwijder" — routes through the same confirm dialog as the long-press. */
static void on_delete_tile(lv_event_t * e) {
    (void)e;
    if (sel < 0) { lv_label_set_text(sel_lbl, tr("Tik eerst een tegel", "Tap a tile first")); return; }
    show_delete_confirm(sel);
}
static void on_reset(lv_event_t * e) {
    (void)e;
    /* Recover a messy layout straight from the GUI: restore the built-in default
     * AND apply it. Just resetting the working copy (the old behaviour) left a
     * bad SAVED layout active on home unless the user also remembered to Opslaan,
     * so a wedged layout looked unfixable (BUG 6). Persist defaults to the active
     * preset, switch home back to the stock (non-custom) arrangement, and restart
     * so home boots clean — the same apply path on_save uses. */
    layout_reset_default();                       /* g_layout = built-in default */
    layout_save_named(settings.active_layout);    /* persist defaults to active preset */
    settings.custom_layout_enabled = 0;           /* home → built-in hardcoded layout */
    settings_save();
    fprintf(stderr, "[layout] reset to default — restarting UI\n");
    ui_request_restart();                          /* ui_launcher.sh respawns; home is clean */
}

/* ---- preset manager ("Indelingen") ------------------------------------ */
static char g_preset_names[LAYOUT_MAX_PRESETS][LAYOUT_NAME_MAX];  /* row idx → name */
static int  g_preset_count;
static void preset_mgr_refresh(void);

/* Mirror layout.c's filename sanitiser so active_layout matches on-disk names
 * (alnum/-/_ kept, spaces→'_'); used when storing the active preset name. */
static void name_sanitize(const char * in, char * out, int outsz) {
    int o = 0;
    if (in)
        for (const char * p = in; *p && o < outsz - 1; p++) {
            char c = *p; if (c == ' ') c = '_';
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_') out[o++] = c;
        }
    out[o] = 0;
}

static void name_close(void) { if (name_modal) { lv_obj_del(name_modal); name_modal = NULL; name_ta = NULL; } }
static void on_name_cancel(lv_event_t * e) { (void)e; name_close(); }
static void on_name_ok(lv_event_t * e) {
    (void)e;
    char nm[LAYOUT_NAME_MAX] = "";
    if (name_ta) name_sanitize(lv_textarea_get_text(name_ta), nm, sizeof nm);
    if (!nm[0]) return;                 /* need a usable name */
    g_layout = edit;
    layout_save_named(nm);
    snprintf(settings.active_layout, sizeof settings.active_layout, "%s", nm);
    settings.custom_layout_enabled = 1;
    settings_save();
    name_close();
    preset_mgr_refresh();
}
static void open_name_modal(lv_event_t * e) {
    (void)e;
    if (name_modal) return;
    name_modal = lv_obj_create(modal);
    lv_obj_set_size(name_modal, SCR_W, SCR_H);
    lv_obj_set_pos(name_modal, 0, 0);
    lv_obj_set_style_bg_color(name_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(name_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(name_modal, 0, 0);
    lv_obj_clear_flag(name_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * card = lv_obj_create(name_modal);
    lv_obj_set_size(card, SCR_W * 70 / 100, SCR_H - 60);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x16263a), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * t = lv_label_create(card);
    lv_obj_set_style_text_color(t, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(t, SF(18), 0);
    lv_label_set_text(t, tr("Naam van de indeling", "Layout name"));
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 6, 6);

    name_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(name_ta, true);
    lv_textarea_set_placeholder_text(name_ta, tr("bv. Dag, Nacht, Gasten", "e.g. Day, Night, Guests"));
    lv_obj_set_width(name_ta, SCR_W * 70 / 100 - 40);
    lv_obj_align(name_ta, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t * ok = lv_btn_create(card);
    lv_obj_set_size(ok, SX(130), SY(42));
    lv_obj_align(ok, LV_ALIGN_TOP_RIGHT, SX(-6), SY(90));
    lv_obj_set_style_bg_color(ok, lv_color_hex(0x2e6e3a), 0);
    lv_obj_add_event_cb(ok, on_name_ok, LV_EVENT_CLICKED, NULL);
    lv_obj_t * okl = lv_label_create(ok); lv_label_set_text(okl, tr("Opslaan", "Save")); lv_obj_center(okl);

    lv_obj_t * ca = lv_btn_create(card);
    lv_obj_set_size(ca, SX(130), SY(42));
    lv_obj_align(ca, LV_ALIGN_TOP_LEFT, SX(6), SY(90));
    lv_obj_set_style_bg_color(ca, lv_color_hex(0x444444), 0);
    lv_obj_add_event_cb(ca, on_name_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cal = lv_label_create(ca); lv_label_set_text(cal, tr("Annuleer", "Cancel")); lv_obj_center(cal);

    lv_obj_t * kb = lv_keyboard_create(card);
    lv_obj_set_size(kb, SCR_W * 70 / 100 - 20, SCR_H / 2 - 30);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_keyboard_set_textarea(kb, name_ta);
}

static void preset_mgr_close(void) {
    if (preset_mgr) { lv_obj_del(preset_mgr); preset_mgr = NULL; preset_list = NULL; }
}
static void on_preset_close(lv_event_t * e) { (void)e; preset_mgr_close(); }
static void on_save_as(lv_event_t * e) { open_name_modal(e); }

/* "Bewerk" — load the preset into the editor's working copy to change it.
 * Sets it as the active (Opslaan target) but does NOT apply to home yet. */
static void on_preset_load(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);     /* -1 = Standaard */
    const char * name = (idx < 0) ? "" : g_preset_names[idx];
    snprintf(settings.active_layout, sizeof settings.active_layout, "%s", name);
    layout_load_named(name);          /* → g_layout */
    preset_mgr_close();
    screen_layout_editor_show();      /* re-open the editor on the loaded layout */
}
/* "Startscherm" — make this preset the one shown on home, right now: persist it
 * as the active layout, enable custom layout, and restart so home boots from it.
 * No edit/save round-trip. */
static void on_preset_apply(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);     /* -1 = Standaard */
    const char * name = (idx < 0) ? "" : g_preset_names[idx];
    snprintf(settings.active_layout, sizeof settings.active_layout, "%s", name);
    settings.custom_layout_enabled = 1;
    settings_save();
    fprintf(stderr, "[layout] preset '%s' set as home — restarting UI\n",
            name[0] ? name : "(standaard)");
    ui_request_restart();              /* ui_launcher.sh respawns; home loads this preset */
}
static void on_preset_delete(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_preset_count) return;
    layout_delete_preset(g_preset_names[idx]);
    if (strcmp(settings.active_layout, g_preset_names[idx]) == 0)
        settings.active_layout[0] = 0;   /* deleted the active one → back to default */
    preset_mgr_refresh();
}

static void row_btn(lv_obj_t * parent, const char * txt, uint32_t col,
                    lv_event_cb_t cb, int idx, int w) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, w, 40);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_t * l = lv_label_create(b); lv_label_set_text(l, txt); lv_obj_center(l);
}

/* One preset row: name (+ ✓ if it's the active/home one) and the three actions —
 * Startscherm (show on home now), Bewerk (edit), Verwijder (delete, named only). */
static void preset_row(const char * name, int idx, int deletable) {
    lv_obj_t * row = lv_obj_create(preset_list);
    lv_obj_set_size(row, LV_PCT(100), 52);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    int active = (strcmp(settings.active_layout, idx < 0 ? "" : name) == 0);
    lv_obj_t * l = lv_label_create(row);
    lv_obj_set_style_text_color(l, lv_color_hex(active ? 0x66dd88 : 0xccddee), 0);
    lv_obj_set_style_text_font(l, SF(18), 0);
    lv_label_set_text_fmt(l, "%s%s", active ? LV_SYMBOL_OK " " : "",
                          (idx < 0) ? tr("Standaard", "Default") : name);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);

    /* right-aligned button cluster (flex so widths/gaps just work) */
    lv_obj_t * btns = lv_obj_create(row);
    lv_obj_set_size(btns, LV_SIZE_CONTENT, 44);
    lv_obj_align(btns, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(btns, 0, 0);
    lv_obj_set_style_border_width(btns, 0, 0);
    lv_obj_set_style_pad_all(btns, 0, 0);
    lv_obj_set_style_pad_column(btns, 6, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);

    row_btn(btns, tr("Startscherm", "Home"), 0x2e6e3a, on_preset_apply, idx, 130);  /* show on home now */
    row_btn(btns, tr("Bewerk", "Edit"),      0x2a4060, on_preset_load,  idx, 88);   /* edit in editor   */
    if (deletable)
        row_btn(btns, tr("Verwijder", "Delete"), 0x6e2e2e, on_preset_delete, idx, 96);
}

static void preset_mgr_refresh(void) {
    if (!preset_list) return;
    lv_obj_clean(preset_list);
    preset_row("", -1, 0);            /* the built-in default file */
    g_preset_count = layout_list_presets(g_preset_names, LAYOUT_MAX_PRESETS);
    for (int i = 0; i < g_preset_count; i++) preset_row(g_preset_names[i], i, 1);
}

static void open_preset_mgr(lv_event_t * e) {
    (void)e;
    if (preset_mgr) return;
    preset_mgr = lv_obj_create(modal);
    lv_obj_set_size(preset_mgr, SCR_W * 76 / 100, SCR_H * 84 / 100);
    lv_obj_center(preset_mgr);
    lv_obj_set_style_bg_color(preset_mgr, lv_color_hex(0x16263a), 0);
    lv_obj_set_style_border_color(preset_mgr, lv_color_hex(0x33506e), 0);
    lv_obj_set_style_border_width(preset_mgr, 2, 0);
    lv_obj_set_style_radius(preset_mgr, 12, 0);
    lv_obj_set_style_pad_all(preset_mgr, 12, 0);
    lv_obj_set_flex_flow(preset_mgr, LV_FLEX_FLOW_COLUMN);

    lv_obj_t * title = lv_label_create(preset_mgr);
    lv_obj_set_style_text_color(title, lv_color_hex(0xeaf2ff), 0);
    lv_obj_set_style_text_font(title, SF(18), 0);
    lv_label_set_text(title, tr("Indelingen  -  Startscherm = tonen op home, Bewerk = aanpassen", "Layouts  -  Home = show on home, Edit = change"));

    preset_list = lv_obj_create(preset_mgr);
    lv_obj_set_width(preset_list, LV_PCT(100));
    lv_obj_set_flex_grow(preset_list, 1);
    lv_obj_set_style_bg_opa(preset_list, 0, 0);
    lv_obj_set_style_border_width(preset_list, 0, 0);
    lv_obj_set_style_pad_all(preset_list, 2, 0);
    lv_obj_set_flex_flow(preset_list, LV_FLEX_FLOW_COLUMN);
    preset_mgr_refresh();

    /* Layout actions: copy page-0 shape onto page 2, and reset to default. */
    lv_obj_t * actions = lv_obj_create(preset_mgr);
    lv_obj_set_width(actions, LV_PCT(100));
    lv_obj_set_height(actions, 50);
    lv_obj_set_style_bg_opa(actions, 0, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 2, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actions, 6, 0);

    lv_obj_t * cp = lv_btn_create(actions);
    lv_obj_set_flex_grow(cp, 1); lv_obj_set_height(cp, 44);
    lv_obj_set_style_bg_color(cp, lv_color_hex(0x2e4e6e), 0);
    lv_obj_add_event_cb(cp, on_copy_p2, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cpl = lv_label_create(cp); lv_label_set_text(cpl, tr("Kopieer pagina 1 -> 2", "Copy page 1 -> 2")); lv_obj_center(cpl);

    lv_obj_t * rs = lv_btn_create(actions);
    lv_obj_set_width(rs, 150); lv_obj_set_height(rs, 44);
    lv_obj_set_style_bg_color(rs, lv_color_hex(0x665522), 0);
    lv_obj_add_event_cb(rs, on_reset, LV_EVENT_CLICKED, NULL);
    lv_obj_t * rsl = lv_label_create(rs); lv_label_set_text(rsl, tr("Standaard", "Default")); lv_obj_center(rsl);

    lv_obj_t * footer = lv_obj_create(preset_mgr);
    lv_obj_set_width(footer, LV_PCT(100));
    lv_obj_set_height(footer, 50);
    lv_obj_set_style_bg_opa(footer, 0, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_pad_all(footer, 2, 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);

    lv_obj_t * saveas = lv_btn_create(footer);
    lv_obj_set_flex_grow(saveas, 1); lv_obj_set_height(saveas, 44);
    lv_obj_set_style_bg_color(saveas, lv_color_hex(0x2e6e3a), 0);
    lv_obj_add_event_cb(saveas, on_save_as, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sl = lv_label_create(saveas); lv_label_set_text(sl, tr("Opslaan als nieuwe...", "Save as new...")); lv_obj_center(sl);

    lv_obj_t * cl = lv_btn_create(footer);
    lv_obj_set_width(cl, 120); lv_obj_set_height(cl, 44);
    lv_obj_set_style_bg_color(cl, lv_color_hex(0x444444), 0);
    lv_obj_add_event_cb(cl, on_preset_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cll = lv_label_create(cl); lv_label_set_text(cll, tr("Sluit", "Close")); lv_obj_center(cll);
}
static void on_cancel(lv_event_t * e) {
    (void)e;
    /* Close the editor and return to the screen underneath (Settings). Deleting
     * `modal` also frees any open sub-modals (they're its children); NULL their
     * static handles too so a stale non-NULL pointer can't block a later re-open
     * or get dereferenced after free. */
    close_chooser();
    if (modal) { lv_obj_del(modal); modal = NULL; }
    chooser = type_grid = NULL;
    preset_mgr = preset_list = NULL;
    name_modal = name_ta = NULL;
    confirm_modal = NULL; confirm_idx = -1;
    sel = -1;
}
static void on_save(lv_event_t * e) {
    (void)e;
    g_layout = edit;
    layout_save_named(settings.active_layout);   /* save to the ACTIVE preset */
    settings.custom_layout_enabled = 1;
    settings_save();
    /* Apply by restarting the UI (ui_launcher.sh respawns us) — same clean path
     * the "Restart UI" tile uses; the new layout is read on boot. */
    fprintf(stderr, "[layout] saved preset '%s' — restarting UI to apply\n",
            settings.active_layout[0] ? settings.active_layout : "(standaard)");
    ui_request_restart();
}

static lv_obj_t * tb_btn(lv_obj_t * bar, int x, int w, const char * txt,
                         lv_event_cb_t cb, void * ud, uint32_t col) {
    lv_obj_t * b = lv_btn_create(bar);
    lv_obj_set_size(b, w, SY(BAR_H - 14));
    lv_obj_set_pos(b, x, SY(7));
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t * l = lv_label_create(b);
    lv_label_set_text(l, txt);
#ifdef TOON1
    /* Toolbar buttons are width-tightened for 800px; use a smaller label so
     * long words ("Indelingen", "Verwijder") fit without clipping. */
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
#endif
    lv_obj_center(l);
    return b;
}

/* (Re)build the draggable rects for the tiles on the current edit_page. */
static void build_canvas_rects(void) {
    for (int i = 0; i < LAYOUT_MAX_TILES; i++)
        if (rects[i]) { lv_obj_del(rects[i]); rects[i] = NULL; }
    for (int i = 0; i < edit.count; i++) { rects[i] = NULL; create_rect(i); }
}

/* Toolbar "Thema" — flip the home between freetoon dark + stock light, live.
 * The layout editor only arranges the dark home's tiles, but the theme switch
 * lives here too so the whole home look is reachable from one place. */
static lv_obj_t * theme_btn = NULL;
static void theme_btn_label(void) {
    if (theme_btn)
        lv_label_set_text(lv_obj_get_child(theme_btn, 0),
            settings.home_theme == 1 ? tr("Thema: Stock", "Theme: Stock")
                                     : tr("Thema: Std",   "Theme: Std"));
}
static void on_theme_toggle(lv_event_t * e) {
    (void)e;
    settings.home_theme = settings.home_theme == 1 ? 0 : 1;
    settings_save();
    ui_apply_home_theme();   /* live swap — no restart */
    theme_btn_label();
}

/* Toolbar "Pagina" — flip between editing page 1 and page 2 (0/1 internally). */
static void on_page_toggle(lv_event_t * e) {
    (void)e;
    edit_page ^= 1;
    sel = -1;
    if (page_btn) lv_label_set_text(lv_obj_get_child(page_btn, 0),
                                    edit_page == 0 ? tr("Pagina 1", "Page 1") : tr("Pagina 2", "Page 2"));
    build_canvas_rects();
    lv_label_set_text_fmt(sel_lbl, tr("Pagina %d - tik een tegel om te selecteren", "Page %d - tap a tile to select"), edit_page + 1);
}

/* Copy the SHAPE of page-0's visible tiles onto page 1 as generic slots (no
 * integrations) — replaces any existing page-1 tiles. The user then assigns an
 * integration per slot. */
static void copy_page0_to_page1(void) {
    int w = 0;                                   /* drop existing page-1 tiles */
    for (int i = 0; i < edit.count; i++)
        if (edit.tiles[i].page != 1) edit.tiles[w++] = edit.tiles[i];
    edit.count = w;
    int base = edit.count;
    for (int i = 0; i < base && edit.count < LAYOUT_MAX_TILES; i++) {
        layout_tile_t * s = &edit.tiles[i];
        if (s->page != 0 || !s->visible) continue;
        layout_tile_t t = *s;
        t.page = 1; t.type = LT_SLOT; t.slot = -1; t.visible = 1;
        edit.tiles[edit.count++] = t;
    }
}

/* "Kopieer pagina 1 -> 2": seed page 2 from page 1's shape, then switch to it. */
static void on_copy_p2(lv_event_t * e) {
    (void)e;
    copy_page0_to_page1();
    preset_mgr_close();
    edit_page = 1;
    if (page_btn) lv_label_set_text(lv_obj_get_child(page_btn, 0), tr("Pagina 2", "Page 2"));
    build_canvas_rects();
    if (sel_lbl) lv_label_set_text(sel_lbl, tr("Pagina 2 - gekopieerd; sleep + wijs integraties toe", "Page 2 - copied; drag + assign integrations"));
}

/* Create-style wrapper so the headless sim (render_one expects a screen) can
 * render the editor. Unused on device. */
lv_obj_t * screen_layout_editor_create(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_scr_load(scr);
    screen_layout_editor_show();
    return scr;
}

void screen_layout_editor_show(void) {
    if (modal) lv_obj_del(modal);
    modal = NULL; chooser = NULL; type_grid = NULL; sel = -1;
    preset_mgr = NULL; preset_list = NULL; name_modal = NULL; name_ta = NULL;
    confirm_modal = NULL; confirm_idx = -1; long_pressed = drag_moved = 0;
    edit_page = 0; page_btn = NULL; theme_btn = NULL;
    drag_last_col = drag_last_row = -1;
    /* Working copy; ensure we have something to edit. */
    if (g_layout.count == 0) layout_reset_default();
    edit = g_layout;

    /* Build on the display's top layer, NOT lv_scr_act(): the editor is opened
     * from the Settings page, which is scrollable — adding the modal to a
     * scrolled screen placed it at the scrolled content origin, pushing the
     * whole editor (incl. the bottom toolbar) up into the middle of the screen.
     * The top layer is a fixed full-screen overlay unaffected by any scroll. */
    modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, SCR_W, SCR_H);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x0a121e), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);

    /* Faint grid backdrop so the snap cells are visible while dragging. Created
     * before the tiles (lower z) and non-interactive so it never eats drags. */
    for (int c = 1; c < LAYOUT_COLS; c++) {
        lv_obj_t * ln = lv_obj_create(modal);
        lv_obj_remove_style_all(ln);
        lv_obj_set_size(ln, 1, SCR_H - BAR_H);
        lv_obj_set_pos(ln, c * CELL_W, 0);
        lv_obj_set_style_bg_color(ln, lv_color_hex(0x2a3a4c), 0);
        lv_obj_set_style_bg_opa(ln, LV_OPA_50, 0);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
    }
    for (int r = 1; r < LAYOUT_ROWS; r++) {
        lv_obj_t * ln = lv_obj_create(modal);
        lv_obj_remove_style_all(ln);
        lv_obj_set_size(ln, SCR_W, 1);
        lv_obj_set_pos(ln, 0, r * CELL_H);
        lv_obj_set_style_bg_color(ln, lv_color_hex(0x2a3a4c), 0);
        lv_obj_set_style_bg_opa(ln, LV_OPA_50, 0);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
    }

    /* One draggable rect per tile on the current edit page. */
    build_canvas_rects();

    /* Bottom toolbar. */
    lv_obj_t * bar = lv_obj_create(modal);
    lv_obj_set_size(bar, SCR_W, BAR_H);
    lv_obj_set_pos(bar, 0, SCR_H - BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x16263a), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Widths tightened so the new "Verwijder" button fits on both 1024 and 800. */
    int x = SX(8);
    tb_btn(bar, x, SX(62), tr("Sluit", "Close"),      on_cancel,       NULL, 0x444444); x += SX(68);
    page_btn = tb_btn(bar, x, SX(78), tr("Pagina 1", "Page 1"), on_page_toggle, NULL, 0x4a3a6a); x += SX(84);
    tb_btn(bar, x, SX(40), "W-", on_resize, (void *)(intptr_t)0, 0x2a4060); x += SX(44);
    tb_btn(bar, x, SX(40), "W+", on_resize, (void *)(intptr_t)1, 0x2a4060); x += SX(44);
    tb_btn(bar, x, SX(40), "H-", on_resize, (void *)(intptr_t)2, 0x2a4060); x += SX(44);
    tb_btn(bar, x, SX(40), "H+", on_resize, (void *)(intptr_t)3, 0x2a4060); x += SX(46);
    tb_btn(bar, x, SX(82), tr("Verberg", "Hide"),    on_toggle_vis,   NULL, 0x553355); x += SX(88);
    tb_btn(bar, x, SX(84), tr("Verwijder", "Delete"),  on_delete_tile,  NULL, 0x6e2e2e); x += SX(90);
    tb_btn(bar, x, SX(70), tr("+ Tegel", "+ Tile"),    on_add,          NULL, 0x2e5e6e); x += SX(76);
    tb_btn(bar, x, SX(92), tr("Indelingen", "Layouts"), open_preset_mgr, NULL, 0x2e4e6e); x += SX(98);
    theme_btn = tb_btn(bar, x, SX(112), "", on_theme_toggle, NULL, 0x2e6e4e); x += SX(118);
    theme_btn_label();

    sel_lbl = lv_label_create(bar);
    lv_obj_set_style_text_color(sel_lbl, lv_color_hex(0xccddee), 0);
    lv_obj_set_style_text_font(sel_lbl, SF(14), 0);
    lv_obj_align(sel_lbl, LV_ALIGN_LEFT_MID, x + SX(6), 0);

    lv_obj_t * save = tb_btn(bar, SCR_W - SX(96), SX(88), tr("Opslaan", "Save"), on_save, NULL, 0x2e6e3a);
    (void)save;
    update_sel_label();
}
