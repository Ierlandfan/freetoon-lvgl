/*
 * Domoticz screen - Settings -> Domoticz. Lists the lights + blinds from a
 * Domoticz server (via domoticz.c's JSON-API poller) and controls them:
 * switches/dimmers toggle On/Off, blinds get Open/Stop/Close. Configured on the settings page.
 * configures host + (optional) basic-auth user/pass and the enable toggle.
 * For users who run Domoticz instead of Home Assistant.
 */
#include "screens.h"
#include "settings.h"
#include "domoticz.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define COL_BG       0x0e1a2a
#define COL_CARD     0x1a2940
#define COL_TEXT_HI  0xffffff
#define COL_TEXT_DIM 0x88aabb
#define COL_ON       0x2e6e3a
#define COL_OFF      0x3a4658

static lv_obj_t * scr_root   = NULL;
static lv_obj_t * list_box   = NULL;
static lv_obj_t * lbl_status = NULL;
static lv_timer_t * refresh_timer = NULL;
static int g_built_count = -1;   /* rebuild rows only when the device set changes */

/* ---- control callbacks (idx packed in user_data) ---- */
static void on_toggle(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    domoticz_switch_async(idx, "Toggle");
}
static void on_blind_open (lv_event_t * e) { domoticz_switch_async((int)(intptr_t)lv_event_get_user_data(e), "Open");  }
static void on_blind_stop (lv_event_t * e) { domoticz_switch_async((int)(intptr_t)lv_event_get_user_data(e), "Stop");  }
static void on_blind_close(lv_event_t * e) { domoticz_switch_async((int)(intptr_t)lv_event_get_user_data(e), "Close"); }

static lv_obj_t * mk_btn(lv_obj_t * parent, const char * txt, uint32_t col,
                         lv_event_cb_t cb, int idx, int w) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, w, 48);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_t * l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

static void build_rows(void) {
    lv_obj_clean(list_box);
    int n = domoticz_state.count;
    if (n == 0) {
        lv_obj_t * empty = lv_label_create(list_box);
        lv_obj_set_style_text_color(empty, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_18, 0);
        lv_label_set_text(empty, settings.domoticz_host[0]
            ? "No lights/blinds returned. Check host / credentials / 'used' devices."
            : "Set the Domoticz host on the settings page (PWA: Domoticz section).");
        g_built_count = n;
        return;
    }
    for (int i = 0; i < n; i++) {
        domoticz_dev_t * d = &domoticz_state.dev[i];
        lv_obj_t * row = lv_obj_create(list_box);
        lv_obj_set_size(row, 940, 76);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * nm = lv_label_create(row);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(COL_TEXT_HI), 0);
        lv_label_set_text(nm, d->name);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 4, 0);

        if (d->kind == DZ_BLIND) {
            mk_btn(row, "Close", 0x6e3a3a, on_blind_close, d->idx, 120); /* rightmost */
            lv_obj_align(lv_obj_get_child(row, lv_obj_get_child_cnt(row)-1), LV_ALIGN_RIGHT_MID, -4, 0);
            mk_btn(row, "Stop", 0x2a4060, on_blind_stop, d->idx, 110);
            lv_obj_align(lv_obj_get_child(row, lv_obj_get_child_cnt(row)-1), LV_ALIGN_RIGHT_MID, -132, 0);
            mk_btn(row, "Open", COL_ON, on_blind_open, d->idx, 120);
            lv_obj_align(lv_obj_get_child(row, lv_obj_get_child_cnt(row)-1), LV_ALIGN_RIGHT_MID, -250, 0);
        } else {
            char st[24];
            if (d->kind == DZ_DIMMER && d->on && d->level >= 0)
                snprintf(st, sizeof st, "%d%%", d->level);
            else
                snprintf(st, sizeof st, "%s", d->on ? "ON" : "OFF");
            lv_obj_t * b = mk_btn(row, st, d->on ? COL_ON : COL_OFF, on_toggle, d->idx, 150);
            lv_obj_align(b, LV_ALIGN_RIGHT_MID, -4, 0);
        }
    }
    g_built_count = n;
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (lbl_status) {
        if (!settings.enable_domoticz)
            lv_label_set_text(lbl_status, "Domoticz disabled - enable it on the settings page.");
        else if (domoticz_state.connected)
            lv_label_set_text_fmt(lbl_status, "Connected - %d device%s",
                                  domoticz_state.count, domoticz_state.count == 1 ? "" : "s");
        else
            lv_label_set_text(lbl_status, "Not connected - check host / credentials.");
    }
    /* Rebuild rows when the device count changes; otherwise repaint state. */
    if (domoticz_state.count != g_built_count) {
        build_rows();
    } else {
        for (int i = 0; i < domoticz_state.count && i < lv_obj_get_child_cnt(list_box); i++) {
            domoticz_dev_t * d = &domoticz_state.dev[i];
            if (d->kind == DZ_BLIND) continue;
            lv_obj_t * row = lv_obj_get_child(list_box, i);
            lv_obj_t * b = lv_obj_get_child(row, lv_obj_get_child_cnt(row) - 1);
            lv_obj_set_style_bg_color(b, lv_color_hex(d->on ? COL_ON : COL_OFF), 0);
            lv_obj_t * bl = lv_obj_get_child(b, 0);
            char st[24];
            if (d->kind == DZ_DIMMER && d->on && d->level >= 0) snprintf(st, sizeof st, "%d%%", d->level);
            else snprintf(st, sizeof st, "%s", d->on ? "ON" : "OFF");
            lv_label_set_text(bl, st);
        }
    }
}

/* Host + credentials + enable are configured on the settings page
 * (PWA /settings, Domoticz section), not from a gear here. */

/* ---- screen ---- */
static void back_async(void * u) { (void)u; ui_pop(); }
static void on_back(lv_event_t * e) { (void)e; lv_async_call(back_async, NULL); }
static void on_scr_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) { if (refresh_timer) lv_timer_resume(refresh_timer); g_built_count = -1; }
    else if (c == LV_EVENT_SCREEN_UNLOADED) { if (refresh_timer) lv_timer_pause(refresh_timer); }
}

lv_obj_t * screen_domoticz_create(void) {
    if (scr_root) return scr_root;
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_LOADED,   NULL);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_UNLOADED, NULL);

    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, 140, 52);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 20, 14);
    lv_obj_set_style_bg_color(back, lv_color_hex(COL_OFF), 0);
    lv_obj_set_style_radius(back, 10, 0);
    lv_obj_set_ext_click_area(back, 20);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_22, 0);
    lv_label_set_text(bl, "< Back"); lv_obj_center(bl);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_label_set_text(title, "Domoticz");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 180, 24);

    lbl_status = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_status, "...");
    lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, 22, 74);

    list_box = lv_obj_create(scr_root);
    lv_obj_set_size(list_box, 980, 470);
    lv_obj_align(list_box, LV_ALIGN_TOP_LEFT, 22, 110);
    lv_obj_set_style_bg_opa(list_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_box, 0, 0);
    lv_obj_set_style_pad_all(list_box, 4, 0);
    lv_obj_set_style_pad_row(list_box, 10, 0);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    build_rows();

    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}
