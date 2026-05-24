/*
 * screen_layout_editor.c — "Indeling" (home-tile layout) editor. Phase 2.
 *
 * Full-screen modal that edits a working copy of the page-0 tiles in the grid
 * layout (layout.c): drag a tile to move (snaps to the grid on release), the
 * +/- buttons resize the selected tile, and the eye button hides/shows it.
 * Save writes the layout, enables custom_layout_enabled, and rebuilds home;
 * Cancel discards. Edits 1:1 on the real screen so what you see is what renders.
 */
#include "lvgl/lvgl.h"
#include "screens.h"
#include "layout.h"
#include "settings.h"
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
#define CELL_W (SCR_W / LAYOUT_COLS)
#define CELL_H (SCR_H / LAYOUT_ROWS)
#define BAR_H  64

static lv_obj_t * modal;
static lv_obj_t * rects[LAYOUT_MAX_TILES];
static layout_t   edit;
static int        sel = -1;
static lv_obj_t * sel_lbl;

static const uint32_t TYPE_COL[LT_COUNT] = {
    [LT_THERMOSTAT]=0x335577, [LT_FORECAST]=0x4488aa, [LT_NEWS_TICKER]=0x666688,
    [LT_NEWS_SUMMARY]=0x6666aa, [LT_CALENDAR]=0x4477cc, [LT_ENERGY]=0xaa77ff,
    [LT_WATER]=0x44aaff, [LT_VENT]=0x66bbdd, [LT_FAMILY]=0xff8866,
    [LT_WASTE]=0x88dd66, [LT_LIGHTS]=0xddaa44, [LT_SLOT]=0x778899,
};

static void place_rect(int i);
static void update_sel_label(void);

static void select_tile(int i) {
    sel = i;
    for (int k = 0; k < edit.count; k++) {
        if (!rects[k]) continue;
        lv_obj_set_style_border_width(rects[k], k == i ? 4 : 0, 0);
        lv_obj_set_style_border_color(rects[k], lv_color_hex(0xffffff), 0);
    }
    update_sel_label();
}

static void rect_event(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * r = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(r);
    if (code == LV_EVENT_PRESSED) {
        select_tile(i);
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t * in = lv_indev_get_act();
        lv_point_t v; lv_indev_get_vect(in, &v);
        lv_obj_set_pos(r, lv_obj_get_x(r) + v.x, lv_obj_get_y(r) + v.y);
    } else if (code == LV_EVENT_RELEASED) {
        int col = (lv_obj_get_x(r) + CELL_W / 2) / CELL_W;
        int row = (lv_obj_get_y(r) + CELL_H / 2) / CELL_H;
        if (col < 0) col = 0;
        if (row < 0) row = 0;
        if (col + edit.tiles[i].w > LAYOUT_COLS) col = LAYOUT_COLS - edit.tiles[i].w;
        if (row + edit.tiles[i].h > LAYOUT_ROWS) row = LAYOUT_ROWS - edit.tiles[i].h;
        edit.tiles[i].col = col; edit.tiles[i].row = row;
        place_rect(i);
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

static void update_sel_label(void) {
    if (!sel_lbl) return;
    if (sel < 0) { lv_label_set_text(sel_lbl, "Tik een tegel om te selecteren"); return; }
    layout_tile_t * t = &edit.tiles[sel];
    lv_label_set_text_fmt(sel_lbl, "%s  %dx%d  %s",
        layout_type_name(t->type), t->w, t->h, t->visible ? "" : "(verborgen)");
}

/* toolbar actions ------------------------------------------------------- */
static void on_resize(lv_event_t * e) {
    if (sel < 0) return;
    int d = (int)(intptr_t)lv_event_get_user_data(e);   /* 0:w- 1:w+ 2:h- 3:h+ */
    layout_tile_t * t = &edit.tiles[sel];
    if (d == 0 && t->w > 1) t->w--;
    if (d == 1 && t->col + t->w < LAYOUT_COLS) t->w++;
    if (d == 2 && t->h > 1) t->h--;
    if (d == 3 && t->row + t->h < LAYOUT_ROWS) t->h++;
    place_rect(sel); update_sel_label();
}
static void on_toggle_vis(lv_event_t * e) {
    (void)e;
    if (sel < 0) return;
    edit.tiles[sel].visible = !edit.tiles[sel].visible;
    place_rect(sel); update_sel_label();
}
static void on_reset(lv_event_t * e) {
    (void)e;
    layout_reset_default();
    /* rebuild the working copy + rects */
    screen_layout_editor_show();   /* simplest: re-open from defaults */
}
static void on_cancel(lv_event_t * e) { (void)e; if (modal) { lv_obj_del(modal); modal = NULL; } sel = -1; }
static void on_save(lv_event_t * e) {
    (void)e;
    g_layout = edit;
    layout_save();
    settings.custom_layout_enabled = 1;
    settings_save();
    /* Apply by restarting the UI (ui_launcher.sh respawns us) — same clean
     * path the "Restart UI" tile uses; the new layout is read on boot. */
    fprintf(stderr, "[layout] saved — restarting UI to apply\n");
    _exit(0);
}

static lv_obj_t * tb_btn(lv_obj_t * bar, int x, int w, const char * txt,
                         lv_event_cb_t cb, void * ud, uint32_t col) {
    lv_obj_t * b = lv_btn_create(bar);
    lv_obj_set_size(b, w, BAR_H - 14);
    lv_obj_set_pos(b, x, 7);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t * l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
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
    sel = -1;
    /* Working copy; ensure we have something to edit. */
    if (g_layout.count == 0) layout_reset_default();
    edit = g_layout;

    modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal, SCR_W, SCR_H);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x0a121e), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    /* One draggable rect per page-0 tile. */
    for (int i = 0; i < edit.count; i++) {
        rects[i] = NULL;
        if (edit.tiles[i].page != 0) continue;
        layout_tile_t * t = &edit.tiles[i];
        lv_obj_t * r = lv_obj_create(modal);
        rects[i] = r;
        lv_obj_set_user_data(r, (void *)(intptr_t)i);
        lv_obj_set_style_bg_color(r, lv_color_hex(TYPE_COL[t->type % LT_COUNT]), 0);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_set_style_pad_all(r, 4, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, rect_event, LV_EVENT_ALL, NULL);
        lv_obj_t * l = lv_label_create(r);
        lv_label_set_text(l, layout_type_name(t->type));
        lv_obj_set_style_text_color(l, lv_color_hex(0x0a121e), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 2, 2);
        place_rect(i);
    }

    /* Bottom toolbar. */
    lv_obj_t * bar = lv_obj_create(modal);
    lv_obj_set_size(bar, SCR_W, BAR_H);
    lv_obj_set_pos(bar, 0, SCR_H - BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x16263a), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    int x = 8;
    tb_btn(bar, x, 70, "Sluit",    on_cancel,     NULL, 0x444444); x += 78;
    tb_btn(bar, x, 70, "Standaard",on_reset,      NULL, 0x665522); x += 78;
    tb_btn(bar, x, 46, "W-", on_resize, (void *)(intptr_t)0, 0x2a4060); x += 50;
    tb_btn(bar, x, 46, "W+", on_resize, (void *)(intptr_t)1, 0x2a4060); x += 50;
    tb_btn(bar, x, 46, "H-", on_resize, (void *)(intptr_t)2, 0x2a4060); x += 50;
    tb_btn(bar, x, 46, "H+", on_resize, (void *)(intptr_t)3, 0x2a4060); x += 54;
    tb_btn(bar, x, 110, "Verberg/Toon", on_toggle_vis, NULL, 0x553355); x += 118;

    sel_lbl = lv_label_create(bar);
    lv_obj_set_style_text_color(sel_lbl, lv_color_hex(0xccddee), 0);
    lv_obj_set_style_text_font(sel_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(sel_lbl, LV_ALIGN_LEFT_MID, x + 6, 0);

    lv_obj_t * save = tb_btn(bar, SCR_W - 96, 88, "Opslaan", on_save, NULL, 0x2e6e3a);
    (void)save;
    update_sel_label();
}
