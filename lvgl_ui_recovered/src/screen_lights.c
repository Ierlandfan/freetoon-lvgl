/*
 * Lights page — opened via swipe-right from home. Header has the
 * "All Lights" master controls; below, room cards each list the lights
 * in that area with a Toggle button. State is polled by ha_thread.
 */
#include "screens.h"
#include "display.h"
#include "homeassistant.h"
#include "icons.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>

#define COL_BG        0x0e1a2a
#define COL_CARD      0x1a2940
#define COL_TEXT_HI   0xffffff
#define COL_TEXT_DIM  0x88aabb
#define COL_ON        0xffcc44
#define COL_OFF       0x3a4658
#define COL_OFFLINE   0x6e6e6e

/* Per-light row widgets, kept so refresh_cb can update colours/text
 * without rebuilding the screen. Indexed parallel to ha_lights[]. */
typedef struct {
    lv_obj_t * row;        /* the row container */
    lv_obj_t * lbl_name;   /* "Bank lamp" */
    lv_obj_t * lbl_state;  /* "on" / "off" / "offline" */
    lv_obj_t * btn;        /* toggle button */
    lv_obj_t * btn_lbl;    /* button label "Toggle" */
} light_row_t;

static lv_obj_t   * scr_root = NULL;
static light_row_t  rows[HA_LIGHT_COUNT];
/* Full-page "HA offline" overlay shown when settings.enable_ha is 0.
 * Toggled by refresh_cb so live changes apply without rebuilding. */
static lv_obj_t   * ha_offline_overlay = NULL;
static lv_timer_t * refresh_timer = NULL;

/* ui_pop deferred via lv_async_call — calling it directly from inside an
 * LVGL event dispatch can cause issues when lv_scr_load fires another
 * event before the original is fully unwound. */
static void back_async(void * unused) { (void)unused; ui_pop(); }
static void on_back(lv_event_t * e) { (void)e; lv_async_call(back_async, NULL); }

/* Pause/resume the refresh ticker with the screen so cached-widget
 * pointers in rows[] never get touched while we're off-screen. */
static void on_scr_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) {
        if (refresh_timer) lv_timer_resume(refresh_timer);
    } else if (c == LV_EVENT_SCREEN_UNLOADED) {
        if (refresh_timer) lv_timer_pause(refresh_timer);
    }
}

static void on_all_on (lv_event_t * e) { (void)e; ha_lights_all_on_async();  }
static void on_all_off(lv_event_t * e) { (void)e; ha_lights_all_off_async(); }

static void on_light_toggle(lv_event_t * e) {
    const char * id = (const char *)lv_event_get_user_data(e);
    if (id) ha_light_toggle_async(id);
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    /* HA integration off → don't bother painting N "offline" rows; show
     * one big "HA offline" overlay instead. Toggled live so the user can
     * flip the integration in Settings and see the page change without
     * re-entering it. */
    if (ha_offline_overlay) {
        if (settings.enable_ha)
            lv_obj_add_flag(ha_offline_overlay, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(ha_offline_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < ha_light_count; i++) {
        if (!rows[i].lbl_state) continue;
        if (!ha_lights[i].available) {
            lv_label_set_text(rows[i].lbl_state, "offline");
            lv_obj_set_style_text_color(rows[i].lbl_state,
                                        lv_color_hex(COL_OFFLINE), 0);
            lv_obj_set_style_bg_color(rows[i].btn,
                                      lv_color_hex(COL_OFFLINE), 0);
        } else if (ha_lights[i].on) {
            int b = ha_lights[i].brightness;
            if (b > 0) lv_label_set_text_fmt(rows[i].lbl_state,
                                             "on  %d%%", b * 100 / 255);
            else       lv_label_set_text(rows[i].lbl_state, "on");
            lv_obj_set_style_text_color(rows[i].lbl_state,
                                        lv_color_hex(COL_ON), 0);
            lv_obj_set_style_bg_color(rows[i].btn,
                                      lv_color_hex(COL_ON), 0);
        } else {
            lv_label_set_text(rows[i].lbl_state, "off");
            lv_obj_set_style_text_color(rows[i].lbl_state,
                                        lv_color_hex(COL_TEXT_DIM), 0);
            lv_obj_set_style_bg_color(rows[i].btn,
                                      lv_color_hex(COL_OFF), 0);
        }
    }
}

/* Build one light row inside an area card. */
static void build_light_row(lv_obj_t * parent, int i, int y) {
    light_row_t * R = &rows[i];
    R->row = lv_obj_create(parent);
    /* Fill the card's inner width (card is narrower on Toon 1) so the
     * RIGHT_MID Toggle button stays inside the card. */
    lv_obj_set_size(R->row, lv_pct(100), 44);
    lv_obj_set_pos(R->row, 0, y);
    lv_obj_set_style_bg_opa(R->row, 0, 0);
    lv_obj_set_style_border_width(R->row, 0, 0);
    lv_obj_set_style_pad_all(R->row, 4, 0);
    lv_obj_clear_flag(R->row, LV_OBJ_FLAG_SCROLLABLE);

    R->lbl_name = lv_label_create(R->row);
    lv_obj_set_style_text_color(R->lbl_name, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(R->lbl_name, SF(18), 0);
    lv_label_set_text(R->lbl_name, ha_lights[i].name);
    lv_obj_align(R->lbl_name, LV_ALIGN_LEFT_MID, 0, -8);

    R->lbl_state = lv_label_create(R->row);
    lv_obj_set_style_text_color(R->lbl_state, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(R->lbl_state, SF(14), 0);
    lv_label_set_text(R->lbl_state, "--");
    lv_obj_align(R->lbl_state, LV_ALIGN_LEFT_MID, 0, 12);

    R->btn = lv_btn_create(R->row);
    lv_obj_set_size(R->btn, 90, 36);
    lv_obj_align(R->btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(R->btn, lv_color_hex(COL_OFF), 0);
    lv_obj_set_style_radius(R->btn, 8, 0);
    /* Soak up sloppy taps so finger-on-edge still hits the button. */
    lv_obj_set_ext_click_area(R->btn, 18);
    lv_obj_add_event_cb(R->btn, on_light_toggle, LV_EVENT_CLICKED,
                        (void *)ha_lights[i].entity_id);

    R->btn_lbl = lv_label_create(R->btn);
    lv_obj_set_style_text_color(R->btn_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(R->btn_lbl, SF(14), 0);
    lv_label_set_text(R->btn_lbl, "Toggle");
    lv_obj_center(R->btn_lbl);
}

/* Build a card per area; cards are arranged in 3 columns. */
static void build_area_card(int col, const char * area_name) {
    /* Three cards across: derive width from the panel so the row fits both
     * Toon 2 (1024 → 320px cards) and Toon 1 (800 → ~245px cards). */
    int card_w = (DISP_HOR - 16 * 2 - 12 * 2) / 3;
    int card_h = SY(380);
    int x = 16 + col * (card_w + 12);
    int y = SY(130);
    lv_obj_t * card = lv_obj_create(scr_root);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * hdr = lv_label_create(card);
    lv_obj_set_style_text_color(hdr, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(hdr, SF(22), 0);
    lv_label_set_text(hdr, area_name);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 4, 0);

    int y_row = 36;
    for (int i = 0; i < ha_light_count; i++) {
        if (strcmp(ha_lights[i].area, area_name) != 0) continue;
        build_light_row(card, i, y_row);
        y_row += 50;
    }
}

lv_obj_t * screen_lights_create(void) {
    /* Singleton — same lv_obj on repeat calls so re-entry doesn't leak
     * widgets or strand timers. (Matches screen_home_create.) */
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_LOADED,   NULL);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_UNLOADED, NULL);
    memset(rows, 0, sizeof(rows));

    /* Top bar: title + Back button (left), All On / All Off (right) */
    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, "Lights");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 16);

    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, 110, 46);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 160, 10);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x3a4658), 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_ext_click_area(back, 20);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_label_set_text(bl, "< Back");
    lv_obj_center(bl);

    /* All Lights master controls — header strip */
    lv_obj_t * master = lv_obj_create(scr_root);
    /* Cap the strip so it clears the title + Back button on the narrower
     * Toon 1 panel (720px would start at x=64 and collide with them). */
    lv_obj_set_size(master, (DISP_VER < 600 ? 500 : 720), 60);
    lv_obj_align(master, LV_ALIGN_TOP_RIGHT, -16, 12);
    lv_obj_set_style_bg_color(master, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(master, 0, 0);
    lv_obj_set_style_radius(master, 12, 0);
    lv_obj_set_style_pad_all(master, 8, 0);
    lv_obj_clear_flag(master, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * lbl_all = lv_label_create(master);
    lv_obj_set_style_text_color(lbl_all, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_all, SF(22), 0);
    lv_label_set_text(lbl_all, "All Lights");
    lv_obj_align(lbl_all, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t * b_on = lv_btn_create(master);
    lv_obj_set_size(b_on, 180, 44);
    lv_obj_align(b_on, LV_ALIGN_RIGHT_MID, -200, 0);
    lv_obj_set_style_bg_color(b_on, lv_color_hex(0x2e6e3a), 0);
    lv_obj_set_style_radius(b_on, 8, 0);
    lv_obj_set_ext_click_area(b_on, 12);
    lv_obj_add_event_cb(b_on, on_all_on, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl_on = lv_label_create(b_on);
    lv_obj_set_style_text_color(lbl_on, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_on, SF(22), 0);
    lv_label_set_text(lbl_on, "All ON");
    lv_obj_center(lbl_on);

    lv_obj_t * b_off = lv_btn_create(master);
    lv_obj_set_size(b_off, 180, 44);
    lv_obj_align(b_off, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(b_off, lv_color_hex(0x6e3a3a), 0);
    lv_obj_set_style_radius(b_off, 8, 0);
    lv_obj_set_ext_click_area(b_off, 12);
    lv_obj_add_event_cb(b_off, on_all_off, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl_off = lv_label_create(b_off);
    lv_obj_set_style_text_color(lbl_off, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_off, SF(22), 0);
    lv_label_set_text(lbl_off, "All OFF");
    lv_obj_center(lbl_off);

    /* Three area cards: Woonkamer | Keuken | Boven (slaapkamers) */
    build_area_card(0, "Woonkamer");
    build_area_card(1, "Keuken");
    build_area_card(2, "Boven");

    /* HA-offline overlay — sits ON TOP of the area cards + master strip
     * so they vanish behind it when HA integration is off. The area
     * cards stay built (cheap), the overlay just covers them. */
    ha_offline_overlay = lv_obj_create(scr_root);
    lv_obj_set_size(ha_offline_overlay, DISP_HOR, DISP_VER);
    lv_obj_set_pos(ha_offline_overlay, 0, 0);
    lv_obj_set_style_bg_color(ha_offline_overlay, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(ha_offline_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ha_offline_overlay, 0, 0);
    lv_obj_clear_flag(ha_offline_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ha_offline_overlay, LV_OBJ_FLAG_CLICKABLE);   /* swallow taps */
    {
        lv_obj_t * back = lv_btn_create(ha_offline_overlay);
        lv_obj_set_size(back, 140, 52);
        lv_obj_align(back, LV_ALIGN_TOP_LEFT, 24, 20);
        lv_obj_set_style_bg_color(back, lv_color_hex(0x3a4658), 0);
        lv_obj_set_style_radius(back, 8, 0);
        lv_obj_set_ext_click_area(back, 20);
        lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
        lv_obj_t * bl = lv_label_create(back);
        lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(bl, SF(22), 0);
        lv_label_set_text(bl, "< Back");
        lv_obj_center(bl);

        lv_obj_t * big = lv_label_create(ha_offline_overlay);
        lv_obj_set_style_text_color(big, lv_color_hex(COL_TEXT_HI), 0);
        lv_obj_set_style_text_font(big, SF(28), 0);
        lv_label_set_text(big, "HA offline");
        lv_obj_align(big, LV_ALIGN_CENTER, 0, -30);

        lv_obj_t * hint = lv_label_create(ha_offline_overlay);
        lv_obj_set_style_text_color(hint, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(hint, SF(22), 0);
        /* Leave a margin so the centered wrapped text never reaches the
         * panel edge (was a fixed 800px → clipped on Toon 1). */
        lv_obj_set_width(hint, DISP_HOR - 80);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(hint,
            "Home Assistant integration is disabled.\n"
            "Enable it in Settings  >  Integrations to control lights from here.");
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 40);
    }
    /* Hidden by default; refresh_cb flips it on if enable_ha=0. */
    lv_obj_add_flag(ha_offline_overlay, LV_OBJ_FLAG_HIDDEN);

    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    refresh_cb(refresh_timer);
    return scr_root;
}
