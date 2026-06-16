/*
 * screen_dim_layout_editor.c — "Dim-indeling" editor.
 *
 * Full-screen modal that arranges the dim/ambient screen's peripheral blocks
 * (weather, waste, Life360, ventilation, thermostat readout, forecast strip) on
 * the same LAYOUT_COLS×LAYOUT_ROWS grid the home editor uses. Far simpler than
 * the home editor: a FIXED set of blocks (no insert/delete/presets/pages), drag
 * to snap a block to a cell (overlap allowed — it's the user's arrangement),
 * +/- to resize, eye to hide/show, Standaard to reset. Save writes
 * toonui_dim_layout.cfg and restarts the UI so screen_dim rebuilds from it.
 *
 * The clock + date stay centered and are NOT editable here (they're the fixed
 * centerpiece). screen_dim translates each block's widgets by the delta between
 * its placed cell and its default cell, so this WYSIWYG grid maps 1:1 to where
 * the blocks land on the real dim screen.
 */
#include "lvgl/lvgl.h"
#include "screens.h"
#include "display.h"   /* SF()/SX()/SY() */
#include "layout.h"
#include "i18n.h"
#include <stdio.h>
#include <stdint.h>

#ifdef TOON1
#  define SCR_W 800
#  define SCR_H 480
#else
#  define SCR_W 1024
#  define SCR_H 600
#endif
#define BAR_H    64
#define CANVAS_H (SCR_H - BAR_H)
#define CELL_W   (SCR_W / LAYOUT_COLS)
#define CELL_H   (CANVAS_H / LAYOUT_ROWS)
#define DIM_MIN_W 2
#define DIM_MIN_H 2

static lv_obj_t * modal;
static lv_obj_t * rects[DB_COUNT];
static dim_block_t edit_dim[DB_COUNT];
static int sel = -1;
static lv_obj_t * sel_lbl;

static const uint32_t DB_COL[DB_COUNT] = {
    [DB_THERMO]=0x335577, [DB_WEATHER]=0x4488aa, [DB_FORECAST]=0x66bbdd,
    [DB_WASTE]=0x88dd66,  [DB_FAMILY]=0xff8866,  [DB_VENT]=0x6666aa,
};

static void place_rect(int i);
static void update_sel_label(void);

static void select_block(int i) {
    sel = i;
    for (int k = 0; k < DB_COUNT; k++)
        if (rects[k]) lv_obj_set_style_border_width(rects[k], k == i ? 4 : 0, 0);
    update_sel_label();
}

static void update_sel_label(void) {
    if (!sel_lbl) return;
    if (sel < 0) { lv_label_set_text(sel_lbl, tr("Tik een blok om te selecteren", "Tap a block to select")); return; }
    dim_block_t * b = &edit_dim[sel];
    lv_label_set_text_fmt(sel_lbl, "%s  %dx%d  %s",
        dim_block_name(sel), b->w, b->h, b->visible ? "" : tr("(verborgen)", "(hidden)"));
}

/* Snap the dragged rect's pixel position to the nearest grid cell, clamped. */
static void snap_cell(int i, int * oc, int * orow) {
    dim_block_t * b = &edit_dim[i];
    int col = (lv_obj_get_x(rects[i]) + CELL_W / 2) / CELL_W;
    int row = (lv_obj_get_y(rects[i]) + CELL_H / 2) / CELL_H;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (col + b->w > LAYOUT_COLS) col = LAYOUT_COLS - b->w;
    if (row + b->h > LAYOUT_ROWS) row = LAYOUT_ROWS - b->h;
    *oc = col; *orow = row;
}

static int drag_moved, press_x0, press_y0;

static void rect_event(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * r = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(r);
    if (code == LV_EVENT_PRESSED) {
        select_block(i);
        drag_moved = 0;
        press_x0 = lv_obj_get_x(r); press_y0 = lv_obj_get_y(r);
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t * in = lv_indev_get_act();
        lv_point_t v; lv_indev_get_vect(in, &v);
        lv_obj_set_pos(r, lv_obj_get_x(r) + v.x, lv_obj_get_y(r) + v.y);
        int dx = lv_obj_get_x(r) - press_x0, dy = lv_obj_get_y(r) - press_y0;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > 10 || dy > 10) drag_moved = 1;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (drag_moved) {
            int col, row; snap_cell(i, &col, &row);
            edit_dim[i].col = col; edit_dim[i].row = row;
        }
        place_rect(i);            /* commit snap (or undo jitter on a plain tap) */
        select_block(i);
        drag_moved = 0;
    }
}

static void place_rect(int i) {
    dim_block_t * b = &edit_dim[i];
    lv_obj_t * r = rects[i];
    if (!r) return;
    lv_obj_set_pos(r, b->col * CELL_W, b->row * CELL_H);
    lv_obj_set_size(r, b->w * CELL_W - 4, b->h * CELL_H - 4);
    lv_obj_set_style_bg_opa(r, b->visible ? LV_OPA_COVER : 60, 0);
}

static void create_rect(int i) {
    lv_obj_t * r = lv_obj_create(modal);
    rects[i] = r;
    lv_obj_set_user_data(r, (void *)(intptr_t)i);
    lv_obj_set_style_bg_color(r, lv_color_hex(DB_COL[i]), 0);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_set_style_pad_all(r, 4, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, rect_event, LV_EVENT_ALL, NULL);
    lv_obj_t * l = lv_label_create(r);
    lv_label_set_text(l, dim_block_name(i));
    lv_obj_set_style_text_color(l, lv_color_hex(0x0a121e), 0);
    lv_obj_set_style_text_font(l, SF(14), 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 2, 2);
    place_rect(i);
}

static void on_resize(lv_event_t * e) {
    if (sel < 0) return;
    int d = (int)(intptr_t)lv_event_get_user_data(e);   /* 0:w- 1:w+ 2:h- 3:h+ */
    dim_block_t * b = &edit_dim[sel];
    if (d == 0 && b->w > DIM_MIN_W) b->w--;
    if (d == 1) { if (b->col + b->w < LAYOUT_COLS) b->w++; else if (b->col > 0) { b->col--; b->w++; } }
    if (d == 2 && b->h > DIM_MIN_H) b->h--;
    if (d == 3) { if (b->row + b->h < LAYOUT_ROWS) b->h++; else if (b->row > 0) { b->row--; b->h++; } }
    place_rect(sel);
    update_sel_label();
}

static void on_toggle_vis(lv_event_t * e) {
    (void)e;
    if (sel < 0) return;
    edit_dim[sel].visible = !edit_dim[sel].visible;
    place_rect(sel);
    update_sel_label();
}

static void on_reset(lv_event_t * e) {
    (void)e;
    dim_layout_reset_default();
    for (int i = 0; i < DB_COUNT; i++) edit_dim[i] = g_dim_blocks[i];
    for (int i = 0; i < DB_COUNT; i++) place_rect(i);
    sel = -1; update_sel_label();
}

static void on_cancel(lv_event_t * e) {
    (void)e;
    if (modal) { lv_obj_del(modal); modal = NULL; }
    for (int i = 0; i < DB_COUNT; i++) rects[i] = NULL;
    sel = -1;
}

static void on_save(lv_event_t * e) {
    (void)e;
    for (int i = 0; i < DB_COUNT; i++) g_dim_blocks[i] = edit_dim[i];
    dim_layout_save();
    fprintf(stderr, "[dim-layout] saved — restarting UI to apply\n");
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
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
#endif
    lv_obj_center(l);
    return b;
}

/* Create-style wrapper so the headless sim can render the editor. */
lv_obj_t * screen_dim_layout_editor_create(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_scr_load(scr);
    screen_dim_layout_editor_show();
    return scr;
}

void screen_dim_layout_editor_show(void) {
    if (modal) lv_obj_del(modal);
    modal = NULL; sel = -1;
    for (int i = 0; i < DB_COUNT; i++) { rects[i] = NULL; edit_dim[i] = g_dim_blocks[i]; }

    modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, SCR_W, SCR_H);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x0a121e), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);

    /* faint grid backdrop */
    for (int c = 1; c < LAYOUT_COLS; c++) {
        lv_obj_t * ln = lv_obj_create(modal);
        lv_obj_remove_style_all(ln);
        lv_obj_set_size(ln, 1, CANVAS_H);
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

    /* clock/date are fixed-centre and not editable — show a ghost hint. */
    lv_obj_t * ghost = lv_label_create(modal);
    lv_obj_set_style_text_color(ghost, lv_color_hex(0x33506e), 0);
    lv_obj_set_style_text_font(ghost, SF(18), 0);
    lv_label_set_text(ghost, tr("(klok blijft in het midden)", "(clock stays centered)"));
    lv_obj_align(ghost, LV_ALIGN_CENTER, 0, -SY(40));

    for (int i = 0; i < DB_COUNT; i++) create_rect(i);

    /* bottom toolbar */
    lv_obj_t * bar = lv_obj_create(modal);
    lv_obj_set_size(bar, SCR_W, BAR_H);
    lv_obj_set_pos(bar, 0, SCR_H - BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x16263a), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    int x = SX(8);
    tb_btn(bar, x, SX(62), tr("Sluit", "Close"), on_cancel, NULL, 0x444444); x += SX(68);
    tb_btn(bar, x, SX(40), "W-", on_resize, (void *)(intptr_t)0, 0x2a4060); x += SX(44);
    tb_btn(bar, x, SX(40), "W+", on_resize, (void *)(intptr_t)1, 0x2a4060); x += SX(44);
    tb_btn(bar, x, SX(40), "H-", on_resize, (void *)(intptr_t)2, 0x2a4060); x += SX(44);
    tb_btn(bar, x, SX(40), "H+", on_resize, (void *)(intptr_t)3, 0x2a4060); x += SX(46);
    tb_btn(bar, x, SX(82), tr("Verberg", "Hide"), on_toggle_vis, NULL, 0x553355); x += SX(88);
    tb_btn(bar, x, SX(92), tr("Standaard", "Default"), on_reset, NULL, 0x665522); x += SX(98);

    sel_lbl = lv_label_create(bar);
    lv_obj_set_style_text_color(sel_lbl, lv_color_hex(0xccddee), 0);
    lv_obj_set_style_text_font(sel_lbl, SF(14), 0);
    lv_obj_align(sel_lbl, LV_ALIGN_LEFT_MID, x + SX(6), 0);

    tb_btn(bar, SCR_W - SX(96), SX(88), tr("Opslaan", "Save"), on_save, NULL, 0x2e6e3a);
    update_sel_label();
}
