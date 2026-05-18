/*
 * Settings screen — a category landing page. Five tiles (Display, Weather,
 * Waste, Heating, About); tapping a tile opens a distinct modal with just
 * that category's controls.
 *
 * Modal infrastructure:
 *   modal_open(title, h)  builds a dimmed backdrop + centred panel and
 *                         returns the panel for the caller to fill.
 *   modal_close()         tears it down (async-deleted) and persists.
 * Only one category modal is open at a time, so the per-control widget
 * pointers below are simply reassigned each time a modal is built.
 *
 * The Heating modal's OpenTherm/On-Off switch does NOT write immediately —
 * it raises a confirm dialog first, because SetBoilerType reconfigures the
 * live boiler (see boxtalk.c).
 */
#include "screens.h"
#include "settings.h"
#include "backlight.h"
#include "boxtalk.h"
#include "icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdlib.h>

static lv_obj_t * scr_root = NULL;

/* ---- modal state ---- */
static lv_obj_t *   cur_modal     = NULL;   /* backdrop of the open category modal */
static lv_obj_t *   confirm_box   = NULL;   /* boiler-type confirm dialog (child of cur_modal) */
static lv_timer_t * modal_timer   = NULL;   /* live refresh for Heating/About modals */
static void       (*modal_tick_fn)(void) = NULL;

/* ---- per-control widget pointers (valid only while a modal is open) ---- */
static lv_obj_t * sw_enable;
static lv_obj_t * sl_timeout,  * lbl_timeout_val;
static lv_obj_t * sl_act,      * lbl_act_val;
static lv_obj_t * sl_dim,      * lbl_dim_val;
static lv_obj_t * sw_dim_wx;
static lv_obj_t * sl_forecast_mode, * lbl_forecast_mode;
static lv_obj_t * sw_dim_waste;
static lv_obj_t * sl_waste_lead, * lbl_waste_lead;
static lv_obj_t * sl_offset,   * lbl_offset_val;
static lv_obj_t * sw_boiler;
static lv_obj_t * lbl_boiler_mode;
static lv_obj_t * lbl_boiler_ot;
static lv_obj_t * lbl_about_status;
static int        pending_boiler_type = -1;

/* ============================ value callbacks ============================ */

static void on_enable_change(lv_event_t * e) {
    settings.auto_dim_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static void on_dim_wx_change(lv_event_t * e) {
    settings.show_dim_weather = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static const char * forecast_mode_label(int m) {
    if (m == FORECAST_HOURLY) return "always hourly";
    if (m == FORECAST_DAILY)  return "always daily";
    return "auto (hourly if available)";
}
static void on_forecast_mode_change(lv_event_t * e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    settings.forecast_mode = v;
    lv_label_set_text(lbl_forecast_mode, forecast_mode_label(v));
}

/* Weather modal: free-text city label + GeoNames id, both persisted via
 * the modal_close() save path. The id is what the API actually uses; the
 * label is purely cosmetic for the Weather screen header. */
static lv_obj_t * ta_wx_city = NULL;
static lv_obj_t * ta_wx_id   = NULL;
static lv_obj_t * lbl_wx_status = NULL;
static void on_weather_apply(lv_event_t * e) {
    (void)e;
    if (ta_wx_city) snprintf(settings.weather_location,
                             sizeof settings.weather_location,
                             "%s", lv_textarea_get_text(ta_wx_city));
    if (ta_wx_id) {
        int id = atoi(lv_textarea_get_text(ta_wx_id));
        if (id > 0) settings.weather_location_id = id;
    }
    settings_save();
    if (lbl_wx_status)
        lv_label_set_text_fmt(lbl_wx_status,
            "Saved. %s = %d", settings.weather_location,
            settings.weather_location_id);
}
static void on_dim_waste_change(lv_event_t * e) {
    settings.show_dim_waste = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static void on_waste_lead_change(lv_event_t * e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    settings.dim_waste_lead_days = v;
    if (v == 0)      lv_label_set_text(lbl_waste_lead, "uit");
    else if (v == 1) lv_label_set_text(lbl_waste_lead, "vanaf 1 dag vooraf");
    else             lv_label_set_text_fmt(lbl_waste_lead, "vanaf %d dagen vooraf", v);
}
static void on_timeout_change(lv_event_t * e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    settings.auto_dim_seconds = v;
    lv_label_set_text_fmt(lbl_timeout_val, "%d s", v);
}
static void on_act_change(lv_event_t * e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    settings.active_brightness = v;
    lv_label_set_text_fmt(lbl_act_val, "%d", v);
    backlight_set(v);                       /* live preview */
}
static void on_dim_change(lv_event_t * e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    settings.dim_brightness = v;
    lv_label_set_text_fmt(lbl_dim_val, "%d", v);
}
static void on_offset_change(lv_event_t * e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    settings.temp_offset_centi = v;
    lv_label_set_text_fmt(lbl_offset_val, "%+.1f C", v / 100.0f);
}

/* ============================ small builders ============================ */

/* One settings row inside a modal panel: title at left, optional value
   label at right, control area below. Returns the row container. */
static lv_obj_t * panel_row(lv_obj_t * parent, int y, const char * title,
                            lv_obj_t ** out_val_lbl) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, 800, 74);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 4, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1f3050), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_all(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);   /* swallow taps, don't close modal */

    lv_obj_t * lbl = lv_label_create(row);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl, title);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    if (out_val_lbl) {
        *out_val_lbl = lv_label_create(row);
        lv_obj_set_style_text_color(*out_val_lbl, lv_color_hex(0x88aabb), 0);
        lv_obj_set_style_text_font(*out_val_lbl, &lv_font_montserrat_22, 0);
        lv_obj_align(*out_val_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    return row;
}

/* A slider that fills the bottom of a row built by panel_row(). */
static lv_obj_t * row_slider(lv_obj_t * row, int lo, int hi, int val,
                             lv_event_cb_t cb) {
    lv_obj_t * s = lv_slider_create(row);
    lv_obj_set_size(s, 760, 14);
    lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_slider_set_range(s, lo, hi);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return s;
}

/* A switch pinned to the right of a panel_row(). */
static lv_obj_t * row_switch(lv_obj_t * row, int checked, lv_event_cb_t cb) {
    lv_obj_t * sw = lv_switch_create(row);
    lv_obj_set_size(sw, 64, 30);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -4, 0);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

/* ============================ modal infra ============================ */

static void modal_timer_cb(lv_timer_t * t) { (void)t; if (modal_tick_fn) modal_tick_fn(); }

static void modal_close(lv_event_t * e) {
    (void)e;
    if (confirm_box) { lv_obj_del_async(confirm_box); confirm_box = NULL; }
    if (modal_timer) { lv_timer_del(modal_timer); modal_timer = NULL; }
    modal_tick_fn = NULL;
    if (cur_modal) {
        lv_obj_t * m = cur_modal;
        cur_modal = NULL;
        lv_obj_del_async(m);          /* async: we're inside a descendant's event */
    }
    settings_save();                  /* persist whatever the modal changed */
}

/* Build a dimmed full-screen backdrop + centred panel. Returns the panel;
   caller positions its content below y≈64 (title + close button live there). */
static lv_obj_t * modal_open(const char * title, int panel_h) {
    cur_modal = lv_obj_create(scr_root);
    lv_obj_remove_style_all(cur_modal);
    lv_obj_set_size(cur_modal, 1024, 600);
    lv_obj_set_pos(cur_modal, 0, 0);
    lv_obj_set_style_bg_color(cur_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cur_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(cur_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cur_modal, LV_OBJ_FLAG_CLICKABLE);          /* tap-outside target */
    lv_obj_add_event_cb(cur_modal, modal_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t * panel = lv_obj_create(cur_modal);
    lv_obj_set_size(panel, 860, panel_h);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x16243a), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_pad_all(panel, 20, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);              /* stop taps reaching backdrop */

    lv_obj_t * t = lv_label_create(panel);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 4, 4);

    lv_obj_t * x = lv_btn_create(panel);
    lv_obj_set_size(x, 56, 56);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 4, -4);
    lv_obj_set_style_bg_color(x, lv_color_hex(0x33445a), 0);
    lv_obj_set_style_radius(x, 12, 0);
    lv_obj_add_event_cb(x, modal_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xl, lv_color_hex(0xffffff), 0);
    lv_obj_center(xl);

    return panel;
}

/* ====================== boiler-type confirm dialog ====================== */

static void confirm_dismiss(void) {
    if (confirm_box) { lv_obj_del_async(confirm_box); confirm_box = NULL; }
}

static void on_boiler_confirm_yes(lv_event_t * e) {
    (void)e;
    if (pending_boiler_type == 0 || pending_boiler_type == 1)
        boxtalk_set_boiler_type(pending_boiler_type);
    confirm_dismiss();
}

static void on_boiler_confirm_no(lv_event_t * e) {
    (void)e;
    /* revert the switch to whatever the boiler currently reports */
    if (sw_boiler) {
        if (toon_state.boiler_type == 1) lv_obj_add_state(sw_boiler, LV_STATE_CHECKED);
        else                             lv_obj_clear_state(sw_boiler, LV_STATE_CHECKED);
    }
    confirm_dismiss();
}

static void on_boiler_switch(lv_event_t * e) {
    int want_onoff = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    pending_boiler_type = want_onoff;     /* 1 = On/Off, 0 = OpenTherm */

    /* Confirm dialog, layered on top of the Heating modal. */
    confirm_box = lv_obj_create(cur_modal);
    lv_obj_remove_style_all(confirm_box);
    lv_obj_set_size(confirm_box, 1024, 600);
    lv_obj_set_pos(confirm_box, 0, 0);
    lv_obj_set_style_bg_color(confirm_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(confirm_box, LV_OPA_60, 0);
    lv_obj_clear_flag(confirm_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(confirm_box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * dlg = lv_obj_create(confirm_box);
    lv_obj_set_size(dlg, 720, 360);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x2a1c1c), 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0xcc5544), 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 16, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_clear_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dlg, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * dt = lv_label_create(dlg);
    lv_obj_set_style_text_font(dt, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(dt, lv_color_hex(0xffcc66), 0);
    lv_label_set_text(dt, "Change boiler control?");
    lv_obj_align(dt, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t * body = lv_label_create(dlg);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(0xddddee), 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, 672);
    lv_label_set_text_fmt(body,
        "Switch boiler control to %s?\n\n"
        "This reconfigures how Toon drives your boiler and is written to "
        "the device immediately. Only change it if you know your boiler "
        "wiring — the wrong setting can stop your heating from working.",
        want_onoff ? "On/Off" : "OpenTherm");
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 48);

    lv_obj_t * b_no = lv_btn_create(dlg);
    lv_obj_set_size(b_no, 200, 64);
    lv_obj_align(b_no, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(b_no, lv_color_hex(0x44556a), 0);
    lv_obj_set_style_radius(b_no, 12, 0);
    lv_obj_add_event_cb(b_no, on_boiler_confirm_no, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_no_l = lv_label_create(b_no);
    lv_label_set_text(b_no_l, "Cancel");
    lv_obj_set_style_text_font(b_no_l, &lv_font_montserrat_22, 0);
    lv_obj_center(b_no_l);

    lv_obj_t * b_yes = lv_btn_create(dlg);
    lv_obj_set_size(b_yes, 260, 64);
    lv_obj_align(b_yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(b_yes, lv_color_hex(0xcc5544), 0);
    lv_obj_set_style_radius(b_yes, 12, 0);
    lv_obj_add_event_cb(b_yes, on_boiler_confirm_yes, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_yes_l = lv_label_create(b_yes);
    lv_label_set_text_fmt(b_yes_l, "Set %s", want_onoff ? "On/Off" : "OpenTherm");
    lv_obj_set_style_text_font(b_yes_l, &lv_font_montserrat_22, 0);
    lv_obj_center(b_yes_l);
}

/* ============================ live ticks ============================ */

static void heating_tick(void) {
    if (lbl_boiler_mode) {
        const char * m = (toon_state.boiler_type == 1) ? "On/Off"
                       : (toon_state.boiler_type == 0) ? "OpenTherm"
                       : "detecting...";
        lv_label_set_text_fmt(lbl_boiler_mode, "Current mode: %s", m);
    }
    if (lbl_boiler_ot) {
        if (toon_state.boiler_type == 0) {
            lv_label_set_text_fmt(lbl_boiler_ot, "Modulation %d%%   -   OT link %s",
                                  toon_state.modulation_level,
                                  toon_state.ot_comm_error ? "ERROR" : "OK");
        } else {
            lv_label_set_text(lbl_boiler_ot, "");
        }
    }
}

static void about_tick(void) {
    if (!lbl_about_status) return;
    double up = 0;
    FILE * f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
    int uh = (int)(up / 3600), um = (int)((up - uh * 3600) / 60);
    lv_label_set_text_fmt(lbl_about_status,
        "BoxTalk: %s   (msg %d)\n"
        "Boiler: %s\n"
        "Indoor %.1f C   -   Setpoint %.1f C\n"
        "System uptime: %dh %dm",
        toon_state.connected ? "connected" : "connecting...",
        toon_state.msg_count,
        toon_state.boiler_type == 1 ? "On/Off" :
        toon_state.boiler_type == 0 ? "OpenTherm" : "unknown",
        toon_state.indoor_temp, toon_state.setpoint, uh, um);
}

/* ============================ category modals ============================ */

static void open_display_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open("Display", 460);
    int y = 70;
    lv_obj_t * r;

    r = panel_row(p, y, "Auto-dim when idle", NULL);
    sw_enable = row_switch(r, settings.auto_dim_enabled, on_enable_change);
    y += 82;

    r = panel_row(p, y, "Idle timeout", &lbl_timeout_val);
    lv_label_set_text_fmt(lbl_timeout_val, "%d s", settings.auto_dim_seconds);
    sl_timeout = row_slider(r, 5, 300, settings.auto_dim_seconds, on_timeout_change);
    y += 82;

    r = panel_row(p, y, "Active brightness", &lbl_act_val);
    lv_label_set_text_fmt(lbl_act_val, "%d", settings.active_brightness);
    sl_act = row_slider(r, 100, 1000, settings.active_brightness, on_act_change);
    y += 82;

    r = panel_row(p, y, "Dim brightness", &lbl_dim_val);
    lv_label_set_text_fmt(lbl_dim_val, "%d", settings.dim_brightness);
    sl_dim = row_slider(r, 0, 400, settings.dim_brightness, on_dim_change);
}

static void open_weather_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open("Weather", 560);
    int y = 70;

    lv_obj_t * r = panel_row(p, y, "Show weather on dim screen", NULL);
    sw_dim_wx = row_switch(r, settings.show_dim_weather, on_dim_wx_change);
    y += 82;

    /* 3-position slider: 0=auto, 1=hourly, 2=daily. Current label updates
       in-place via on_forecast_mode_change. */
    r = panel_row(p, y, "Forecast strip", &lbl_forecast_mode);
    lv_label_set_text(lbl_forecast_mode,
                      forecast_mode_label(settings.forecast_mode));
    sl_forecast_mode = row_slider(r, 0, 2, settings.forecast_mode,
                                  on_forecast_mode_change);
    y += 90;

    /* City name (cosmetic) + Buienradar GeoNames id (used by the API). */
    lv_obj_t * lbl_city = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_city, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_city, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_city, "City:");
    lv_obj_align(lbl_city, LV_ALIGN_TOP_LEFT, 4, y);
    ta_wx_city = lv_textarea_create(p);
    lv_obj_set_size(ta_wx_city, 380, 44);
    lv_obj_align(ta_wx_city, LV_ALIGN_TOP_LEFT, 240, y - 4);
    lv_textarea_set_one_line(ta_wx_city, true);
    lv_textarea_set_text(ta_wx_city, settings.weather_location);
    y += 60;

    lv_obj_t * lbl_id = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_id, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_id, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_id, "Buienradar id:");
    lv_obj_align(lbl_id, LV_ALIGN_TOP_LEFT, 4, y);
    ta_wx_id = lv_textarea_create(p);
    lv_obj_set_size(ta_wx_id, 380, 44);
    lv_obj_align(ta_wx_id, LV_ALIGN_TOP_LEFT, 240, y - 4);
    lv_textarea_set_one_line(ta_wx_id, true);
    lv_textarea_set_accepted_chars(ta_wx_id, "0123456789");
    char id_str[16]; snprintf(id_str, sizeof id_str, "%d",
                              settings.weather_location_id);
    lv_textarea_set_text(ta_wx_id, id_str);
    y += 50;

    /* Hint — where to find the id. */
    lv_obj_t * hint = lv_label_create(p);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(hint,
        "Find your city's id in the URL on buienradar.nl/weer/<city>/nl/<ID>\n"
        "(e.g. Medemblik = 2751073, De Bilt = 2757783). Plain KNMI codes\n"
        "won't work — these are GeoNames ids.");
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 4, y);
    y += 70;

    lv_obj_t * btn = lv_btn_create(p);
    lv_obj_set_size(btn, 200, 50);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 4, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a6090), 0);
    lv_obj_add_event_cb(btn, on_weather_apply, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(btn);
    lv_label_set_text(bl, "Apply");
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_center(bl);

    lbl_wx_status = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_wx_status, lv_color_hex(0x9fc4e6), 0);
    lv_obj_set_style_text_font(lbl_wx_status, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_wx_status, "");
    lv_obj_align(lbl_wx_status, LV_ALIGN_TOP_LEFT, 220, y + 12);
}

static void open_waste_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open("Waste", 300);
    int y = 70;
    lv_obj_t * r;

    r = panel_row(p, y, "Show waste on dim screen", NULL);
    sw_dim_waste = row_switch(r, settings.show_dim_waste, on_dim_waste_change);
    y += 82;

    r = panel_row(p, y, "Waste alert window", &lbl_waste_lead);
    if      (settings.dim_waste_lead_days == 0) lv_label_set_text(lbl_waste_lead, "uit");
    else if (settings.dim_waste_lead_days == 1) lv_label_set_text(lbl_waste_lead, "vanaf 1 dag vooraf");
    else lv_label_set_text_fmt(lbl_waste_lead, "vanaf %d dagen vooraf",
                               settings.dim_waste_lead_days);
    sl_waste_lead = row_slider(r, 0, 7, settings.dim_waste_lead_days, on_waste_lead_change);
}

static void open_heating_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open("Heating", 470);
    int y = 70;

    /* indoor temp calibration */
    lv_obj_t * r = panel_row(p, y, "Indoor temp offset", &lbl_offset_val);
    lv_label_set_text_fmt(lbl_offset_val, "%+.1f C", settings.temp_offset_centi / 100.0f);
    sl_offset = row_slider(r, -500, 500, settings.temp_offset_centi, on_offset_change);
    y += 90;

    /* boiler control type */
    lv_obj_t * box = lv_obj_create(p);
    lv_obj_set_size(box, 800, 200);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 4, y);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1f3050), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * hdr = lv_label_create(box);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_22, 0);
    lv_label_set_text(hdr, "Boiler control");
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);

    sw_boiler = lv_switch_create(box);
    lv_obj_set_size(sw_boiler, 64, 30);
    lv_obj_align(sw_boiler, LV_ALIGN_TOP_RIGHT, -4, 2);
    if (toon_state.boiler_type == 1) lv_obj_add_state(sw_boiler, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_boiler, on_boiler_switch, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * swlbl = lv_label_create(box);
    lv_obj_set_style_text_color(swlbl, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(swlbl, &lv_font_montserrat_18, 0);
    lv_label_set_text(swlbl, "Off = OpenTherm    On = On/Off");
    lv_obj_align(swlbl, LV_ALIGN_TOP_RIGHT, -4, 38);

    lbl_boiler_mode = lv_label_create(box);
    lv_obj_set_style_text_color(lbl_boiler_mode, lv_color_hex(0xffcc44), 0);
    lv_obj_set_style_text_font(lbl_boiler_mode, &lv_font_montserrat_22, 0);
    lv_obj_align(lbl_boiler_mode, LV_ALIGN_TOP_LEFT, 0, 36);

    lbl_boiler_ot = lv_label_create(box);
    lv_obj_set_style_text_color(lbl_boiler_ot, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_boiler_ot, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl_boiler_ot, LV_ALIGN_TOP_LEFT, 0, 70);

    lv_obj_t * warn = lv_label_create(box);
    lv_obj_set_style_text_color(warn, lv_color_hex(0xcc7766), 0);
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_18, 0);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(warn, 772);
    lv_label_set_text(warn,
        "Changing this writes to the live boiler. You'll be asked to confirm.");
    lv_obj_align(warn, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* refresh the boiler labels live + re-query the type */
    heating_tick();
    boxtalk_get_boiler_type();
    modal_tick_fn = heating_tick;
    modal_timer = lv_timer_create(modal_timer_cb, 1000, NULL);
}

static void open_about_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open("About", 320);

    lv_obj_t * ver = lv_label_create(p);
    lv_obj_set_style_text_color(ver, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_22, 0);
    lv_label_set_text(ver, "toonui - LVGL rebuild   (build " __DATE__ ")");
    lv_obj_align(ver, LV_ALIGN_TOP_LEFT, 4, 70);

    lbl_about_status = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_about_status, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_about_status, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl_about_status, LV_ALIGN_TOP_LEFT, 4, 112);

    about_tick();
    modal_tick_fn = about_tick;
    modal_timer = lv_timer_create(modal_timer_cb, 1000, NULL);
}

/* ============================ OT Bridge modal ============================ *
 *
 * Lets the user toggle between the two heating-control topologies:
 *
 *   Keteladapter (wired)   — happ_thermstat speaks Quby directly to the
 *                            keteladapter on /dev/ttymxc0; keteladapter is
 *                            wired into OTGW T-side; OTGW relays in GW=1.
 *                            Currently the ONLY proven-working topology.
 *
 *   OTGW (wireless)        — quby_bridge bind-mounts a PTY over /dev/ttymxc0,
 *                            intercepts happ_thermstat's Quby frames, and
 *                            forwards OT writes to OTGW via HTTP. Removes
 *                            the need for OT wires keteladapter↔OTGW.
 *                            Boot-handshake mismatch on Subscribe/Enable
 *                            opcodes — not yet usable.
 *
 * Apply rewrites /etc/inittab + kicks quby_bridge / happ_thermstat.
 * Reads/writes settings via the existing settings.[ch] machinery.
 *
 * Test + Check buttons call OTGW HTTP from a detached thread so LVGL
 * stays responsive while curl runs. */

#include <sys/wait.h>

/* Modal widget pointers (re-used per modal open). */
static lv_obj_t * ta_otgw_host;
static lv_obj_t * ta_otgw_user;
static lv_obj_t * ta_otgw_pass;
static lv_obj_t * lbl_otmode;
static lv_obj_t * lbl_test_result;
static lv_obj_t * lbl_check_result;
static lv_obj_t * lbl_ket_result;
static lv_obj_t * lbl_apply_warning;

/* Latest async-test results (set by background thread, read by ui timer). */
static char       g_test_result_buf[200]  = "";
static char       g_check_result_buf[200] = "";
static char       g_ket_result_buf[256]   = "";
static volatile int g_test_pending  = 0;
static volatile int g_check_pending = 0;
static volatile int g_ket_pending   = 0;

/* Spawn an HTTP request via popen+curl. Returns 0 on HTTP 2xx with body in
 * `out`, -1 otherwise. Mirrors homeassistant.c's helper to avoid a libcurl
 * link dep. */
static int otgw_http_call(const char * method, const char * path,
                          const char * body, char * out, size_t outsz) {
    char host[80], cmd[1024], auth[160] = "";
    snprintf(host, sizeof(host), "%s", settings.otgw_host);
    if (!host[0]) snprintf(host, sizeof(host), "192.168.99.21");
    if (settings.otgw_user[0]) {
        snprintf(auth, sizeof(auth), "-u %s:%s ",
                 settings.otgw_user, settings.otgw_pass);
    }
    if (body) {
        snprintf(cmd, sizeof(cmd),
            "/usr/bin/curl -s --max-time 5 --connect-timeout 3 %s"
            "-X %s -H 'Content-Type: application/json' "
            "--data '%s' 'http://%s%s' 2>&1",
            auth, method, body, host, path);
    } else {
        snprintf(cmd, sizeof(cmd),
            "/usr/bin/curl -s --max-time 5 --connect-timeout 3 %s"
            "-X %s 'http://%s%s' 2>&1",
            auth, method, host, path);
    }
    FILE * f = popen(cmd, "r");
    if (!f) { snprintf(out, outsz, "popen failed"); return -1; }
    size_t n = fread(out, 1, outsz - 1, f);
    out[n] = 0;
    int rc = pclose(f);
    return (rc == 0 && n > 0) ? 0 : -1;
}

static void * test_thread(void * arg) {
    (void)arg;
    char buf[256];
    int rc = otgw_http_call("POST", "/api/v0/otgw/command",
                            "{\"command\":\"PR=A\"}", buf, sizeof(buf));
    /* Trim to first 120 chars + strip newlines */
    for (char * p = buf; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    if (rc == 0)
        snprintf(g_test_result_buf, sizeof(g_test_result_buf),
                 "OK: %.140s", buf);
    else
        snprintf(g_test_result_buf, sizeof(g_test_result_buf),
                 "FAIL: %.140s", buf);
    g_test_pending = 1;
    return NULL;
}

static void * check_thread(void * arg) {
    (void)arg;
    char buf[2048];
    int rc = otgw_http_call("GET", "/api/v0/settings", NULL, buf, sizeof(buf));
    if (rc != 0) {
        snprintf(g_check_result_buf, sizeof(g_check_result_buf),
                 "FAIL: settings endpoint unreachable");
        g_check_pending = 1; return NULL;
    }
    /* Look for `"otgwcommands","value":"..."` substring */
    const char * needle = "\"otgwcommands\",\"value\":\"";
    const char * p = strstr(buf, needle);
    char gw_value[64] = "";
    if (p) {
        p += strlen(needle);
        const char * e = strchr(p, '"');
        if (e && e - p < (int)sizeof(gw_value) - 1) {
            size_t L = e - p;
            memcpy(gw_value, p, L); gw_value[L] = 0;
        }
    }
    if (!gw_value[0]) {
        snprintf(g_check_result_buf, sizeof(g_check_result_buf),
                 "WARN: otgwcommands setting not found in response");
    } else if (strstr(gw_value, "GW=1")) {
        snprintf(g_check_result_buf, sizeof(g_check_result_buf),
                 "OK: %s (gateway/relay — correct for wired mode)", gw_value);
    } else if (strstr(gw_value, "GW=2")) {
        snprintf(g_check_result_buf, sizeof(g_check_result_buf),
                 "INFO: %s (no-thermostat mode — needed for wireless mode)", gw_value);
    } else if (strstr(gw_value, "GW=0")) {
        snprintf(g_check_result_buf, sizeof(g_check_result_buf),
                 "WARN: %s (monitor-only — won't drive boiler)", gw_value);
    } else {
        snprintf(g_check_result_buf, sizeof(g_check_result_buf),
                 "INFO: otgwcommands=\"%s\"", gw_value);
    }
    g_check_pending = 1;
    return NULL;
}

static void on_test_click(lv_event_t * e) {
    (void)e;
    lv_label_set_text(lbl_test_result, "Testing…");
    pthread_t t;
    if (pthread_create(&t, NULL, test_thread, NULL) == 0) pthread_detach(t);
}

/* Keteladapter (wired path) test. Verifies the
 * happ_thermstat ↔ /dev/ttymxc0 ↔ keteladapter chain by:
 *   - GET localhost:10080/happ_thermstat?action=getThermostatInfo
 *   - Checking the freshness/sanity of values that only populate when the
 *     adapter is actively talking OT:
 *       * currentTemp in (5..40)°C        → ambient sensor read OK
 *       * currentInternalBoilerSetpoint   → derived from stooklijn + OT slave-info;
 *                                           0 or absurd = no OT data flowing
 *       * burnerInfo != -1                → -1 means "no slave-status ever received"
 *       * otCommError                     → patched to 0; if non-zero anyway → real problem
 *
 * If the read fails, happ_thermstat is dead or /dev/ttymxc0 is locked
 * elsewhere (e.g. quby_bridge bind-mount in wireless mode while it's
 * mid-handshake).
 *
 * This is the wired-mode counterpart of the OTGW Test button; together
 * they cover both topologies. */
static void * ket_thread(void * arg) {
    (void)arg;
    char buf[2048];
    FILE * f = popen(
        "/usr/bin/curl -s --max-time 4 --connect-timeout 2 "
        "'http://localhost:10080/happ_thermstat?action=getThermostatInfo' 2>&1",
        "r");
    if (!f) {
        snprintf(g_ket_result_buf, sizeof(g_ket_result_buf),
                 "FAIL: cannot exec curl");
        g_ket_pending = 1; return NULL;
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    int rc = pclose(f);
    if (rc != 0 || n == 0) {
        snprintf(g_ket_result_buf, sizeof(g_ket_result_buf),
                 "FAIL: happ_thermstat HTTP unreachable (rc=%d)", rc);
        g_ket_pending = 1; return NULL;
    }
    /* Pull the key fields out of the flat JSON. */
    int    burner_info = -99;
    int    ot_err      = -99;
    int    boil_sp     = -99;
    double room_c      = -99.0;
    const char * p;
    if ((p = strstr(buf, "\"currentTemp\":\""))) room_c = atoi(p + 15) / 100.0;
    if ((p = strstr(buf, "\"burnerInfo\":\""))) burner_info = atoi(p + 14);
    if ((p = strstr(buf, "\"otCommError\":\""))) ot_err = atoi(p + 15);
    if ((p = strstr(buf, "\"currentInternalBoilerSetpoint\":\"")))
        boil_sp = atoi(p + 33);

    /* Mode-aware analysis: in off/proxy mode (keteladapter is real OT
     * master) expect REAL OT data; in wireless mode the keteladapter
     * isn't in the loop so don't fail on absent burnerInfo. */
    int wired = strcmp(settings.ot_bridge_mode, "wireless") != 0;
    int ok = 1;
    char detail[200];
    snprintf(detail, sizeof(detail),
             "room=%.2fC burner=%d boilerSP=%d otErr=%d",
             room_c, burner_info, boil_sp, ot_err);
    if (room_c < 5.0 || room_c > 40.0) {
        ok = 0;
        snprintf(g_ket_result_buf, sizeof(g_ket_result_buf),
                 "FAIL: ambient sensor reads %.2fC (out of 5-40 range). %s",
                 room_c, detail);
    } else if (wired && burner_info == -1) {
        ok = 0;
        snprintf(g_ket_result_buf, sizeof(g_ket_result_buf),
                 "FAIL: burnerInfo=-1 (no OT slave-status received). "
                 "Check OT wires keteladapter<->OTGW polarity. %s", detail);
    } else if (ot_err > 0) {
        ok = 0;
        snprintf(g_ket_result_buf, sizeof(g_ket_result_buf),
                 "FAIL: otCommError=%d (active OT fault). %s",
                 ot_err, detail);
    }
    if (ok) {
        snprintf(g_ket_result_buf, sizeof(g_ket_result_buf),
                 "OK: keteladapter responding. %s", detail);
    }
    g_ket_pending = 1;
    return NULL;
}

static void on_ket_click(lv_event_t * e) {
    (void)e;
    lv_label_set_text(lbl_ket_result, "Testing keteladapter…");
    pthread_t t;
    if (pthread_create(&t, NULL, ket_thread, NULL) == 0) pthread_detach(t);
}

static void on_check_click(lv_event_t * e) {
    (void)e;
    lv_label_set_text(lbl_check_result, "Querying OTGW…");
    pthread_t t;
    if (pthread_create(&t, NULL, check_thread, NULL) == 0) pthread_detach(t);
}

/* Three mode-pick buttons; pointers cached so the click handler can update
 * their "checked" highlight state. The actual destructive mode-swap runs
 * on Apply, not on tap — taps just record the *intended* mode + repaint.
 * We also cache each button's label child because lv_btn doesn't propagate
 * style changes to its label (the label needs its own style update). */
static lv_obj_t * btn_off  = NULL;
static lv_obj_t * btn_proxy = NULL;
static lv_obj_t * btn_wireless = NULL;
static lv_obj_t * lbl_off  = NULL;
static lv_obj_t * lbl_proxy = NULL;
static lv_obj_t * lbl_wireless = NULL;

static const char * mode_label(const char * m) {
    /* Hyphens (not em-dashes): Montserrat at this size lacks the em-dash
     * glyph and would render it as a missing-glyph square. */
    if (strcmp(m, "off")      == 0) return "Off - keteladapter direct, no bridge";
    if (strcmp(m, "wireless") == 0) return "Wireless - bridge fakes Quby, OTGW drives boiler";
    return                             "Proxy - bridge sniffs Quby, native heat path preserved";
}

static void paint_mode_buttons(void) {
    const struct { lv_obj_t * b; lv_obj_t * lbl; const char * mode; } items[] = {
        { btn_off,      lbl_off,      "off" },
        { btn_proxy,    lbl_proxy,    "proxy" },
        { btn_wireless, lbl_wireless, "wireless" },
    };
    for (unsigned i = 0; i < sizeof items / sizeof items[0]; i++) {
        if (!items[i].b) continue;
        int active = strcmp(settings.ot_bridge_mode, items[i].mode) == 0;
        /* Force opacity so theme defaults don't fight us. */
        lv_obj_set_style_bg_opa(items[i].b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(items[i].b,
            lv_color_hex(active ? 0x3a6090 : 0x1a2a44), 0);
        /* Label text color lives on the LABEL, not the button — lv_btn
         * doesn't propagate text styles to its children. */
        if (items[i].lbl) {
            lv_obj_set_style_text_color(items[i].lbl,
                lv_color_hex(active ? 0xffffff : 0x88aabb), 0);
        }
        /* Belt-and-braces: mark dirty so LVGL repaints at next refresh. */
        lv_obj_invalidate(items[i].b);
    }
}

static void set_mode_and_repaint(const char * m) {
    snprintf(settings.ot_bridge_mode, sizeof(settings.ot_bridge_mode), "%s", m);
    lv_label_set_text(lbl_otmode, mode_label(m));
    paint_mode_buttons();
    lv_label_set_text(lbl_apply_warning,
        "Tap Apply to write inittab + restart bridge/thermstat.\n"
        "Heat will be off for ~10-15s during the switch.");
    /* OTGW host/user/pass only meaningful for proxy + wireless (both talk
     * to OTGW). Off mode doesn't need them but no harm keeping editable. */
    int needs_otgw = strcmp(m, "off") != 0;
    if (needs_otgw) {
        lv_obj_clear_state(ta_otgw_host, LV_STATE_DISABLED);
        lv_obj_clear_state(ta_otgw_user, LV_STATE_DISABLED);
        lv_obj_clear_state(ta_otgw_pass, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(ta_otgw_host, LV_STATE_DISABLED);
        lv_obj_add_state(ta_otgw_user, LV_STATE_DISABLED);
        lv_obj_add_state(ta_otgw_pass, LV_STATE_DISABLED);
    }
}

static void on_mode_off_click(lv_event_t * e)      { (void)e; set_mode_and_repaint("off"); }
static void on_mode_proxy_click(lv_event_t * e)    { (void)e; set_mode_and_repaint("proxy"); }
static void on_mode_wireless_click(lv_event_t * e) { (void)e; set_mode_and_repaint("wireless"); }

static void on_apply_click(lv_event_t * e) {
    (void)e;
    /* Persist text-area edits into settings before applying. */
    const char * h = lv_textarea_get_text(ta_otgw_host);
    const char * u = lv_textarea_get_text(ta_otgw_user);
    const char * p = lv_textarea_get_text(ta_otgw_pass);
    snprintf(settings.otgw_host, sizeof(settings.otgw_host), "%s", h ? h : "");
    snprintf(settings.otgw_user, sizeof(settings.otgw_user), "%s", u ? u : "");
    snprintf(settings.otgw_pass, sizeof(settings.otgw_pass), "%s", p ? p : "");
    settings_save();

    /* Apply mode switch via shell. We shell out because rewriting inittab
     * + pkill + umount is messy to do via syscalls. The script lives at
     * /mnt/data/ot_mode_switch.sh and accepts off|proxy|wireless. */
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "/mnt/data/ot_mode_switch.sh %s >> /tmp/ot_mode_switch.log 2>&1 &",
        settings.ot_bridge_mode);
    system(cmd);
    lv_label_set_text(lbl_apply_warning,
        "Applied — bridge/thermstat restarting. "
        "Re-open this modal in ~15s to verify.");
}

/* Per-tick: pick up async test/check results and update the modal labels. */
static void otbridge_tick(void) {
    if (g_test_pending) {
        lv_label_set_text(lbl_test_result, g_test_result_buf);
        g_test_pending = 0;
    }
    if (g_check_pending) {
        lv_label_set_text(lbl_check_result, g_check_result_buf);
        g_check_pending = 0;
    }
    if (g_ket_pending) {
        lv_label_set_text(lbl_ket_result, g_ket_result_buf);
        g_ket_pending = 0;
    }
}

static void open_otbridge_modal(lv_event_t * e) {
    (void)e;
    /* Sync settings.ot_bridge_mode to whatever is *actually* running before
     * we paint, so the highlighted button reflects reality (not whatever
     * was last saved to cfg). Reads /etc/inittab for the qbri line; if
     * absent we're in `off`, otherwise the `-m <mode>` arg wins. */
    {
        FILE * f = fopen("/etc/inittab", "r");
        const char * detected = "off";
        char line[256];
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "qbri:", 5) != 0) continue;
                if (strstr(line, "-m active"))      detected = "wireless";
                else if (strstr(line, "-m proxy"))  detected = "proxy";
                break;
            }
            fclose(f);
        }
        snprintf(settings.ot_bridge_mode, sizeof(settings.ot_bridge_mode),
                 "%s", detected);
    }

    lv_obj_t * p = modal_open("OT Bridge", 600);
    int y = 70;

    /* Mode picker — 3-way: off / proxy / wireless. */
    lv_obj_t * mode_lbl = lv_label_create(p);
    lv_obj_set_style_text_color(mode_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(mode_lbl, &lv_font_montserrat_22, 0);
    lv_label_set_text(mode_lbl, "Mode:");
    lv_obj_align(mode_lbl, LV_ALIGN_TOP_LEFT, 4, y + 12);

    const struct { lv_obj_t ** btn_slot; lv_obj_t ** lbl_slot;
                   const char * caption;
                   lv_event_cb_t cb; int x; } picks[] = {
        { &btn_off,      &lbl_off,      "Off",      on_mode_off_click,      120 },
        { &btn_proxy,    &lbl_proxy,    "Proxy",    on_mode_proxy_click,    270 },
        { &btn_wireless, &lbl_wireless, "Wireless", on_mode_wireless_click, 470 },
    };
    for (unsigned i = 0; i < sizeof picks / sizeof picks[0]; i++) {
        lv_obj_t * b = lv_btn_create(p);
        lv_obj_set_size(b, 140, 50);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, picks[i].x, y);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_add_event_cb(b, picks[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t * l = lv_label_create(b);
        lv_label_set_text(l, picks[i].caption);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_22, 0);
        lv_obj_center(l);
        *picks[i].btn_slot = b;
        *picks[i].lbl_slot = l;
    }
    y += 60;

    /* Mode description (one-liner that flips with the selection). */
    lbl_otmode = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_otmode, lv_color_hex(0xa8c4dc), 0);
    lv_obj_set_style_text_font(lbl_otmode, &lv_font_montserrat_18, 0);
    lv_obj_set_width(lbl_otmode, 560);
    lv_label_set_long_mode(lbl_otmode, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_otmode, mode_label(settings.ot_bridge_mode));
    lv_obj_align(lbl_otmode, LV_ALIGN_TOP_LEFT, 4, y);
    paint_mode_buttons();
    y += 50;

    /* OTGW host */
    lv_obj_t * lbl_host = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_host, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_host, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_host, "OTGW host:");
    lv_obj_align(lbl_host, LV_ALIGN_TOP_LEFT, 4, y);
    ta_otgw_host = lv_textarea_create(p);
    lv_obj_set_size(ta_otgw_host, 380, 44);
    lv_obj_align(ta_otgw_host, LV_ALIGN_TOP_LEFT, 240, y - 4);
    lv_textarea_set_one_line(ta_otgw_host, true);
    lv_textarea_set_text(ta_otgw_host, settings.otgw_host);
    y += 60;

    /* OTGW user/pass */
    lv_obj_t * lbl_user = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_user, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_user, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_user, "OTGW user (opt):");
    lv_obj_align(lbl_user, LV_ALIGN_TOP_LEFT, 4, y);
    ta_otgw_user = lv_textarea_create(p);
    lv_obj_set_size(ta_otgw_user, 380, 44);
    lv_obj_align(ta_otgw_user, LV_ALIGN_TOP_LEFT, 240, y - 4);
    lv_textarea_set_one_line(ta_otgw_user, true);
    lv_textarea_set_text(ta_otgw_user, settings.otgw_user);
    y += 60;

    lv_obj_t * lbl_pass = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_pass, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_pass, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_pass, "OTGW pass (opt):");
    lv_obj_align(lbl_pass, LV_ALIGN_TOP_LEFT, 4, y);
    ta_otgw_pass = lv_textarea_create(p);
    lv_obj_set_size(ta_otgw_pass, 380, 44);
    lv_obj_align(ta_otgw_pass, LV_ALIGN_TOP_LEFT, 240, y - 4);
    lv_textarea_set_one_line(ta_otgw_pass, true);
    lv_textarea_set_password_mode(ta_otgw_pass, true);
    lv_textarea_set_text(ta_otgw_pass, settings.otgw_pass);
    y += 60;

    /* Enable/disable host/user/pass per current mode */
    int wireless = strcmp(settings.ot_bridge_mode, "otgw") == 0;
    if (!wireless) {
        lv_obj_add_state(ta_otgw_host, LV_STATE_DISABLED);
        lv_obj_add_state(ta_otgw_user, LV_STATE_DISABLED);
        lv_obj_add_state(ta_otgw_pass, LV_STATE_DISABLED);
    }

    /* Buttons row — three OTGW-side buttons */
    lv_obj_t * b_test = lv_btn_create(p);
    lv_obj_set_size(b_test, 200, 50);
    lv_obj_align(b_test, LV_ALIGN_TOP_LEFT, 4, y);
    lv_obj_set_style_bg_color(b_test, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(b_test, on_test_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_test_lbl = lv_label_create(b_test);
    lv_label_set_text(b_test_lbl, "Test OTGW");
    lv_obj_center(b_test_lbl);

    lv_obj_t * b_check = lv_btn_create(p);
    lv_obj_set_size(b_check, 220, 50);
    lv_obj_align(b_check, LV_ALIGN_TOP_LEFT, 218, y);
    lv_obj_set_style_bg_color(b_check, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(b_check, on_check_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_check_lbl = lv_label_create(b_check);
    lv_label_set_text(b_check_lbl, "Check GW mode");
    lv_obj_center(b_check_lbl);

    lv_obj_t * b_ket = lv_btn_create(p);
    lv_obj_set_size(b_ket, 220, 50);
    lv_obj_align(b_ket, LV_ALIGN_TOP_LEFT, 452, y);
    lv_obj_set_style_bg_color(b_ket, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(b_ket, on_ket_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_ket_lbl = lv_label_create(b_ket);
    lv_label_set_text(b_ket_lbl, "Test keteladapter");
    lv_obj_center(b_ket_lbl);
    y += 65;

    /* Second button row — just Apply */
    lv_obj_t * b_apply = lv_btn_create(p);
    lv_obj_set_size(b_apply, 200, 50);
    lv_obj_align(b_apply, LV_ALIGN_TOP_LEFT, 4, y);
    lv_obj_set_style_bg_color(b_apply, lv_color_hex(0x884422), 0);
    lv_obj_add_event_cb(b_apply, on_apply_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_apply_lbl = lv_label_create(b_apply);
    lv_label_set_text(b_apply_lbl, "Apply mode");
    lv_obj_center(b_apply_lbl);
    y += 60;

    /* Result labels stacked below the button rows */
    lbl_test_result = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_test_result, lv_color_hex(0xa8c4dc), 0);
    lv_obj_set_style_text_font(lbl_test_result, &lv_font_montserrat_18, 0);
    lv_label_set_long_mode(lbl_test_result, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_test_result, 770);
    lv_obj_align(lbl_test_result, LV_ALIGN_TOP_LEFT, 4, y);
    lv_label_set_text(lbl_test_result, "OTGW test: (not run)");
    y += 35;

    lbl_check_result = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_check_result, lv_color_hex(0xa8c4dc), 0);
    lv_obj_set_style_text_font(lbl_check_result, &lv_font_montserrat_18, 0);
    lv_obj_set_width(lbl_check_result, 770);
    lv_label_set_long_mode(lbl_check_result, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_check_result, LV_ALIGN_TOP_LEFT, 4, y);
    lv_label_set_text(lbl_check_result, "GW mode check: (not run)");
    y += 35;

    lbl_ket_result = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_ket_result, lv_color_hex(0xa8c4dc), 0);
    lv_obj_set_style_text_font(lbl_ket_result, &lv_font_montserrat_18, 0);
    lv_obj_set_width(lbl_ket_result, 770);
    lv_label_set_long_mode(lbl_ket_result, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_ket_result, LV_ALIGN_TOP_LEFT, 4, y);
    lv_label_set_text(lbl_ket_result, "Keteladapter test: (not run)");
    y += 45;

    lbl_apply_warning = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_apply_warning, lv_color_hex(0xcc8866), 0);
    lv_obj_set_style_text_font(lbl_apply_warning, &lv_font_montserrat_18, 0);
    lv_label_set_long_mode(lbl_apply_warning, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_apply_warning, 770);
    lv_obj_align(lbl_apply_warning, LV_ALIGN_TOP_LEFT, 4, y);
    lv_label_set_text(lbl_apply_warning,
        "Apply rewrites /etc/inittab and restarts quby_bridge + happ_thermstat. "
        "Heat will pause for ~10s during the switch.");

    /* Reset async-result flags so this modal-open starts clean. */
    g_test_pending = 0;
    g_check_pending = 0;
    g_ket_pending = 0;

    modal_tick_fn = otbridge_tick;
    modal_timer = lv_timer_create(modal_timer_cb, 500, NULL);
}

/* ============================ landing page ============================ */

static void on_back(lv_event_t * e) { (void)e; ui_pop(); }

/* --------- Clean-screen overlay: 30 s timer that ignores all touch -------
 * Loads a full-black screen with a single centred countdown so the user can
 * wipe the glass without triggering buttons. The screen has zero event
 * handlers — taps reach the LVGL input layer and find nothing actionable.
 * Backlight stays at the current "active" level so the countdown stays
 * readable; the LCD is 99% black anyway, which reads as "off". On expiry
 * the screen pops itself back to settings. */
static lv_obj_t *  g_clean_scr  = NULL;
static lv_obj_t *  g_clean_lbl  = NULL;
static lv_timer_t * g_clean_tmr = NULL;
static int         g_clean_remaining = 0;

static void clean_finish(void) {
    if (g_clean_tmr) { lv_timer_del(g_clean_tmr); g_clean_tmr = NULL; }
    if (g_clean_scr) {
        lv_obj_t * dead = g_clean_scr;
        g_clean_scr = NULL; g_clean_lbl = NULL;
        ui_pop();
        lv_obj_del_async(dead);
    }
}

static void clean_tick(lv_timer_t * t) {
    (void)t;
    g_clean_remaining--;
    if (g_clean_remaining <= 0) { clean_finish(); return; }
    if (g_clean_lbl) lv_label_set_text_fmt(g_clean_lbl, "%d", g_clean_remaining);
}

static void open_clean_modal(lv_event_t * e) {
    (void)e;
    g_clean_remaining = 30;
    g_clean_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_clean_scr, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(g_clean_scr, LV_OBJ_FLAG_SCROLLABLE);
    /* No event_cb anywhere on this screen → touches do nothing. */
    g_clean_lbl = lv_label_create(g_clean_scr);
    lv_obj_set_style_text_color(g_clean_lbl, lv_color_hex(0xffffff), 0);
    LV_FONT_DECLARE(lv_font_montserrat_96_custom);
    lv_obj_set_style_text_font(g_clean_lbl, &lv_font_montserrat_96_custom, 0);
    lv_label_set_text_fmt(g_clean_lbl, "%d", g_clean_remaining);
    lv_obj_center(g_clean_lbl);
    ui_push(g_clean_scr);
    g_clean_tmr = lv_timer_create(clean_tick, 1000, NULL);
}

/* One category tile: icon (optional), big title, caption. */
static void make_tile(int x, int y, const lv_img_dsc_t * icon, const char * sym,
                      const char * title, const char * caption, lv_event_cb_t cb) {
    lv_obj_t * tile = lv_btn_create(scr_root);
    lv_obj_set_size(tile, 308, 188);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x1a2a44), 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x24385c), LV_STATE_PRESSED);
    lv_obj_set_style_radius(tile, 16, 0);
    lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, NULL);

    if (icon) {
        lv_obj_t * im = lv_img_create(tile);
        lv_img_set_src(im, icon);
        lv_obj_set_style_img_recolor(im, lv_color_hex(0x9fc4e6), 0);
        lv_obj_set_style_img_recolor_opa(im, 255, 0);
        lv_obj_align(im, LV_ALIGN_TOP_MID, 0, 18);
    } else if (sym) {
        lv_obj_t * s = lv_label_create(tile);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(s, lv_color_hex(0x9fc4e6), 0);
        lv_label_set_text(s, sym);
        lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 26);
    }

    lv_obj_t * t = lv_label_create(tile);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 14);

    lv_obj_t * c = lv_label_create(tile);
    lv_obj_set_style_text_font(c, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(c, lv_color_hex(0x7d97b5), 0);
    lv_label_set_text(c, caption);
    lv_obj_align(c, LV_ALIGN_BOTTOM_MID, 0, -14);
}

/* ===================================================================== */
/* MQTT modal: broker creds + Test + Discover + topic-checklist + Apply  */
/* ===================================================================== */
#include "mqtt_client.h"

static lv_obj_t * ta_mqtt_host;
static lv_obj_t * ta_mqtt_port;
static lv_obj_t * ta_mqtt_user;
static lv_obj_t * ta_mqtt_pass;
static lv_obj_t * lbl_mqtt_result;

/* Up to 32 discovered topics; first 16 rendered as checkboxes in the modal.
 * Worker thread fills these; LVGL tick pulls them out and (re-)builds the
 * checkbox column on change. */
#define MQTT_DISC_CAP 32
static char     g_disc_topics[MQTT_DISC_CAP][96];
static int      g_disc_count = 0;
static volatile int g_disc_dirty = 0;
static volatile int g_disc_running = 0;
static volatile int g_test_done = 0;
static char     g_test_result[160] = "";
static pthread_mutex_t g_disc_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Checkboxes for the discovered topics — rebuilt by the tick when dirty. */
static lv_obj_t * cb_topics[16] = {NULL};
static lv_obj_t * lbl_topics[16] = {NULL};
static int        cb_count = 0;
static lv_obj_t * topics_container = NULL;

static int topic_is_subscribed(const char * t) {
    for (int i = 0; i < settings.mqtt_topic_count; i++)
        if (!strcmp(settings.mqtt_topics[i], t)) return 1;
    return 0;
}

static void mqtt_test_thread_fn(void) { /* not used — kept for symmetry */ }

static void * mqtt_test_thread(void * arg) {
    (void)arg;
    char host[64], user[32], pass[64];
    snprintf(host, sizeof(host), "%s", lv_textarea_get_text(ta_mqtt_host));
    snprintf(user, sizeof(user), "%s", lv_textarea_get_text(ta_mqtt_user));
    snprintf(pass, sizeof(pass), "%s", lv_textarea_get_text(ta_mqtt_pass));
    int port = atoi(lv_textarea_get_text(ta_mqtt_port));
    char err[128] = "";
    int rc = mqtt_test_connection(host, port, user, pass, err, sizeof(err));
    pthread_mutex_lock(&g_disc_mtx);
    snprintf(g_test_result, sizeof(g_test_result), "%s: %s",
             rc == 0 ? "OK" : "FAIL", err);
    g_test_done = 1;
    pthread_mutex_unlock(&g_disc_mtx);
    return NULL;
}

static void on_mqtt_test_click(lv_event_t * e) {
    (void)e;
    lv_label_set_text(lbl_mqtt_result, "Testing connection…");
    pthread_t t;
    if (pthread_create(&t, NULL, mqtt_test_thread, NULL) == 0) pthread_detach(t);
}

static void on_disc_topic(const char * topic, void * arg) {
    (void)arg;
    pthread_mutex_lock(&g_disc_mtx);
    for (int i = 0; i < g_disc_count; i++)
        if (!strcmp(g_disc_topics[i], topic)) { pthread_mutex_unlock(&g_disc_mtx); return; }
    if (g_disc_count < MQTT_DISC_CAP) {
        snprintf(g_disc_topics[g_disc_count], sizeof(g_disc_topics[0]), "%s", topic);
        g_disc_count++;
        g_disc_dirty = 1;
    }
    pthread_mutex_unlock(&g_disc_mtx);
}

static void * mqtt_discover_thread(void * arg) {
    (void)arg;
    char host[64], user[32], pass[64];
    snprintf(host, sizeof(host), "%s", lv_textarea_get_text(ta_mqtt_host));
    snprintf(user, sizeof(user), "%s", lv_textarea_get_text(ta_mqtt_user));
    snprintf(pass, sizeof(pass), "%s", lv_textarea_get_text(ta_mqtt_pass));
    int port = atoi(lv_textarea_get_text(ta_mqtt_port));
    pthread_mutex_lock(&g_disc_mtx);
    g_disc_count = 0; g_disc_dirty = 1;
    pthread_mutex_unlock(&g_disc_mtx);
    /* 5 second scan of "#" is enough to catch retained + a couple of
     * ticks of live traffic on a normal home broker. */
    int n = mqtt_discover_topics(host, port, user, pass, "#", 5000,
                                 on_disc_topic, NULL);
    pthread_mutex_lock(&g_disc_mtx);
    snprintf(g_test_result, sizeof(g_test_result),
             n < 0 ? "Discovery failed (check creds/host)"
                   : "Discovery done: %d topic%s seen",
             n, n == 1 ? "" : "s");
    g_test_done = 1;
    g_disc_running = 0;
    pthread_mutex_unlock(&g_disc_mtx);
    return NULL;
}

static void on_mqtt_discover_click(lv_event_t * e) {
    (void)e;
    pthread_mutex_lock(&g_disc_mtx);
    if (g_disc_running) { pthread_mutex_unlock(&g_disc_mtx); return; }
    g_disc_running = 1;
    pthread_mutex_unlock(&g_disc_mtx);
    lv_label_set_text(lbl_mqtt_result, "Subscribing to # for 5s…");
    pthread_t t;
    if (pthread_create(&t, NULL, mqtt_discover_thread, NULL) == 0) pthread_detach(t);
}

static void rebuild_topic_checkboxes(void) {
    if (!topics_container) return;
    /* Wipe existing children. */
    lv_obj_clean(topics_container);
    cb_count = 0;
    int n;
    pthread_mutex_lock(&g_disc_mtx);
    n = g_disc_count;
    if (n > 16) n = 16;
    /* Snapshot to a local array so we hold the mutex briefly. */
    char snap[16][96];
    for (int i = 0; i < n; i++)
        snprintf(snap[i], sizeof(snap[0]), "%s", g_disc_topics[i]);
    pthread_mutex_unlock(&g_disc_mtx);

    int y = 0;
    for (int i = 0; i < n; i++) {
        lv_obj_t * row = lv_obj_create(topics_container);
        lv_obj_set_size(row, 760, 36);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, y);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        cb_topics[i] = lv_checkbox_create(row);
        lv_checkbox_set_text(cb_topics[i], "");
        lv_obj_align(cb_topics[i], LV_ALIGN_LEFT_MID, 0, 0);
        if (topic_is_subscribed(snap[i]))
            lv_obj_add_state(cb_topics[i], LV_STATE_CHECKED);

        lbl_topics[i] = lv_label_create(row);
        lv_obj_set_style_text_color(lbl_topics[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(lbl_topics[i], &lv_font_montserrat_18, 0);
        lv_label_set_text(lbl_topics[i], snap[i]);
        lv_obj_align(lbl_topics[i], LV_ALIGN_LEFT_MID, 36, 0);

        y += 38;
    }
    cb_count = n;
}

static void mqtt_tick(void) {
    if (g_disc_dirty) {
        g_disc_dirty = 0;
        rebuild_topic_checkboxes();
    }
    if (g_test_done) {
        g_test_done = 0;
        char buf[200];
        pthread_mutex_lock(&g_disc_mtx);
        snprintf(buf, sizeof(buf), "%s", g_test_result);
        pthread_mutex_unlock(&g_disc_mtx);
        if (lbl_mqtt_result) lv_label_set_text(lbl_mqtt_result, buf);
    }
}

static void on_mqtt_apply_click(lv_event_t * e) {
    (void)e;
    snprintf(settings.mqtt_host, sizeof(settings.mqtt_host),
             "%s", lv_textarea_get_text(ta_mqtt_host));
    settings.mqtt_port = atoi(lv_textarea_get_text(ta_mqtt_port));
    if (settings.mqtt_port == 0) settings.mqtt_port = 1883;
    snprintf(settings.mqtt_user, sizeof(settings.mqtt_user),
             "%s", lv_textarea_get_text(ta_mqtt_user));
    snprintf(settings.mqtt_pass, sizeof(settings.mqtt_pass),
             "%s", lv_textarea_get_text(ta_mqtt_pass));
    /* Topic list = the currently-checked checkboxes from the discovered
     * list. If discovery wasn't run, preserve the existing topics so the
     * user doesn't accidentally wipe their config by clicking Apply. */
    if (cb_count > 0) {
        settings.mqtt_topic_count = 0;
        for (int i = 0; i < cb_count && settings.mqtt_topic_count < 8; i++) {
            if (lv_obj_has_state(cb_topics[i], LV_STATE_CHECKED)) {
                snprintf(settings.mqtt_topics[settings.mqtt_topic_count],
                         sizeof(settings.mqtt_topics[0]),
                         "%s", lv_label_get_text(lbl_topics[i]));
                settings.mqtt_topic_count++;
            }
        }
    }
    settings_save();
    /* Also mirror to /mnt/data/mqtt.cfg so external scripts that read it
     * (none today, future-proofing) stay in sync. */
    FILE * f = fopen("/mnt/data/mqtt.cfg", "w");
    if (f) {
        fprintf(f, "%s:%s:%s\n", settings.mqtt_host, settings.mqtt_user,
                settings.mqtt_pass);
        fclose(f); chmod("/mnt/data/mqtt.cfg", 0600);
    }
    mqtt_client_restart();
    lv_label_set_text(lbl_mqtt_result,
        "Applied — subscriber reconnecting with new settings.");
}

static void open_mqtt_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open("MQTT", 760);
    int y = 70;

    /* Host */
    lv_obj_t * lbl_h = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_h, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_h, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_h, "Broker host:");
    lv_obj_align(lbl_h, LV_ALIGN_TOP_LEFT, 4, y);
    ta_mqtt_host = lv_textarea_create(p);
    lv_obj_set_size(ta_mqtt_host, 380, 44);
    lv_obj_align(ta_mqtt_host, LV_ALIGN_TOP_LEFT, 240, y - 4);
    lv_textarea_set_one_line(ta_mqtt_host, true);
    lv_textarea_set_text(ta_mqtt_host, settings.mqtt_host);
    /* Port — separate text area on the right */
    lv_obj_t * lbl_p = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_p, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_p, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_p, "Port:");
    lv_obj_align(lbl_p, LV_ALIGN_TOP_LEFT, 640, y);
    ta_mqtt_port = lv_textarea_create(p);
    lv_obj_set_size(ta_mqtt_port, 100, 44);
    lv_obj_align(ta_mqtt_port, LV_ALIGN_TOP_LEFT, 720, y - 4);
    lv_textarea_set_one_line(ta_mqtt_port, true);
    char portbuf[8]; snprintf(portbuf, sizeof(portbuf), "%d",
                              settings.mqtt_port ? settings.mqtt_port : 1883);
    lv_textarea_set_text(ta_mqtt_port, portbuf);
    y += 60;

    /* User */
    lv_obj_t * lbl_u = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_u, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_u, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_u, "User (opt):");
    lv_obj_align(lbl_u, LV_ALIGN_TOP_LEFT, 4, y);
    ta_mqtt_user = lv_textarea_create(p);
    lv_obj_set_size(ta_mqtt_user, 380, 44);
    lv_obj_align(ta_mqtt_user, LV_ALIGN_TOP_LEFT, 240, y - 4);
    lv_textarea_set_one_line(ta_mqtt_user, true);
    lv_textarea_set_text(ta_mqtt_user, settings.mqtt_user);
    y += 60;

    /* Pass */
    lv_obj_t * lbl_pw = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_pw, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_pw, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_pw, "Pass (opt):");
    lv_obj_align(lbl_pw, LV_ALIGN_TOP_LEFT, 4, y);
    ta_mqtt_pass = lv_textarea_create(p);
    lv_obj_set_size(ta_mqtt_pass, 380, 44);
    lv_obj_align(ta_mqtt_pass, LV_ALIGN_TOP_LEFT, 240, y - 4);
    lv_textarea_set_one_line(ta_mqtt_pass, true);
    lv_textarea_set_password_mode(ta_mqtt_pass, true);
    lv_textarea_set_text(ta_mqtt_pass, settings.mqtt_pass);
    y += 60;

    /* Test / Discover / Apply buttons */
    lv_obj_t * b_test = lv_btn_create(p);
    lv_obj_set_size(b_test, 220, 50);
    lv_obj_align(b_test, LV_ALIGN_TOP_LEFT, 4, y);
    lv_obj_set_style_bg_color(b_test, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(b_test, on_mqtt_test_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl1 = lv_label_create(b_test);
    lv_label_set_text(bl1, "Test connection"); lv_obj_center(bl1);

    lv_obj_t * b_disc = lv_btn_create(p);
    lv_obj_set_size(b_disc, 250, 50);
    lv_obj_align(b_disc, LV_ALIGN_TOP_LEFT, 240, y);
    lv_obj_set_style_bg_color(b_disc, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(b_disc, on_mqtt_discover_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl2 = lv_label_create(b_disc);
    lv_label_set_text(bl2, "Discover topics (5s)"); lv_obj_center(bl2);

    lv_obj_t * b_apply = lv_btn_create(p);
    lv_obj_set_size(b_apply, 200, 50);
    lv_obj_align(b_apply, LV_ALIGN_TOP_LEFT, 500, y);
    lv_obj_set_style_bg_color(b_apply, lv_color_hex(0xc06030), 0);
    lv_obj_add_event_cb(b_apply, on_mqtt_apply_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl3 = lv_label_create(b_apply);
    lv_label_set_text(bl3, "Apply + restart"); lv_obj_center(bl3);
    y += 60;

    /* Result line */
    lbl_mqtt_result = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_mqtt_result, lv_color_hex(0xa8c4dc), 0);
    lv_obj_set_style_text_font(lbl_mqtt_result, &lv_font_montserrat_18, 0);
    lv_obj_set_width(lbl_mqtt_result, 770);
    lv_label_set_long_mode(lbl_mqtt_result, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_mqtt_result, LV_ALIGN_TOP_LEFT, 4, y);
    lv_label_set_text(lbl_mqtt_result, "Ready. Test the broker or hit Discover.");
    y += 40;

    /* Discovered-topic checklist container — scrollable column */
    topics_container = lv_obj_create(p);
    lv_obj_set_size(topics_container, 770, 280);
    lv_obj_align(topics_container, LV_ALIGN_TOP_LEFT, 4, y);
    lv_obj_set_style_bg_opa(topics_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(topics_container, 0, 0);
    lv_obj_set_style_pad_all(topics_container, 0, 0);
    cb_count = 0;
    /* Pre-populate the checklist with the currently-subscribed topics so
     * the user sees them before clicking Discover. */
    pthread_mutex_lock(&g_disc_mtx);
    g_disc_count = 0;
    for (int i = 0; i < settings.mqtt_topic_count && i < MQTT_DISC_CAP; i++) {
        if (!settings.mqtt_topics[i][0]) continue;
        snprintf(g_disc_topics[g_disc_count++],
                 sizeof(g_disc_topics[0]), "%s", settings.mqtt_topics[i]);
    }
    g_disc_dirty = 1;
    pthread_mutex_unlock(&g_disc_mtx);

    /* Hook the 1s modal timer so we can pick up async worker results. */
    modal_tick_fn = mqtt_tick;
    modal_timer = lv_timer_create(modal_timer_cb, 500, NULL);
}

lv_obj_t * screen_settings_create(void) {
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x0f1a2a), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* header */
    lv_obj_t * btn_back = lv_btn_create(scr_root);
    lv_obj_set_size(btn_back, 140, 80);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x223344), 0);
    lv_obj_set_style_radius(btn_back, 14, 0);
    lv_obj_set_ext_click_area(btn_back, 20);
    lv_obj_add_event_cb(btn_back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_lbl = lv_label_create(btn_back);
    lv_label_set_text(back_lbl, "< Back");
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_22, 0);
    lv_obj_center(back_lbl);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_label_set_text(title, "Settings");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 180, 30);

    /* 7 category tiles. About moves to row3 left to keep its info on screen
     * without overlapping rows 1-2 (each tile is 188px tall, row2 ends
     * at y=514, so row3 at y=520 ends at y=708 — partially off-screen,
     * but only the bottom 108px of About is clipped which is acceptable
     * since it's a label-only diagnostic page; users tap title area). */
    int x0 = 25, gap = 22, row1 = 120, row2 = 326, row3 = 520;
    make_tile(x0 + 0*(308+gap), row1, &icon_wx_cloud, NULL, "Display",
              "dim, timeout, brightness", open_display_modal);
    make_tile(x0 + 1*(308+gap), row1, &icon_wx_cloud, NULL, "Weather",
              "weather on dim screen", open_weather_modal);
    make_tile(x0 + 2*(308+gap), row1, &icon_trash, NULL, "Waste",
              "pickup alerts on dim", open_waste_modal);
    make_tile(x0 + 0*(308+gap), row2, &icon_flame, NULL, "Heating",
              "temp offset, boiler type", open_heating_modal);
    make_tile(x0 + 1*(308+gap), row2, NULL, LV_SYMBOL_GPS, "OT Bridge",
              "off / proxy / wireless", open_otbridge_modal);
    make_tile(x0 + 2*(308+gap), row2, NULL, LV_SYMBOL_WIFI, "MQTT",
              "broker + topics", open_mqtt_modal);
    make_tile(x0 + 0*(308+gap), row3, NULL, LV_SYMBOL_LIST, "About",
              "status & diagnostics", open_about_modal);
    make_tile(x0 + 1*(308+gap), row3, NULL, LV_SYMBOL_EYE_CLOSE, "Clean",
              "30 s screen lock to wipe", open_clean_modal);

    return scr_root;
}
