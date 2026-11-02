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
#include "display.h"
#include "settings.h"
#include "theme.h"
#include "i18n.h"
#include "domoticz.h"
#include "homeassistant.h"
#include "calendar.h"
#include "weather.h"
#include "backlight.h"
#include "boxtalk.h"
#include "icons.h"
#include "tile_slots.h"
#include "news.h"
#include "wastecollection.h"
#include "update_check.h"
#include "update_modal.h"
#include "anim_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdlib.h>

static lv_obj_t * scr_root = NULL;
static lv_obj_t * scr_tiles_cont = NULL;
static int        scr_light_mode = -1;

/* ---- modal state ---- */
static lv_obj_t *   cur_modal     = NULL;   /* backdrop of the topmost open modal */
static lv_obj_t *   parent_modal  = NULL;   /* backdrop underneath cur_modal when nested */
static lv_obj_t *   confirm_box   = NULL;   /* boiler-type confirm dialog (child of cur_modal) */
static lv_timer_t * modal_timer   = NULL;   /* live refresh for Heating/About modals */
static void       (*modal_tick_fn)(void) = NULL;

/* ---- per-control widget pointers (valid only while a modal is open) ---- */
static lv_obj_t * sw_enable;
static lv_obj_t * sl_timeout,  * lbl_timeout_val;
static lv_obj_t * sw_home_enable;
static lv_obj_t * sl_home_timeout, * lbl_home_timeout_val;
static lv_obj_t * sl_act,      * lbl_act_val;
static lv_obj_t * sl_dim,      * lbl_dim_val;
static lv_obj_t * sw_dim_wx;
static lv_obj_t * sw_forecast_mode, * sw_weather_feel;
static lv_obj_t * sw_dim_waste;
static lv_obj_t * sw_dim_bars, * sw_dim_bars_swap;
static lv_obj_t * sl_waste_lead, * lbl_waste_lead;
static lv_obj_t * sl_offset,   * lbl_offset_val;
static lv_obj_t * sw_boiler;
static lv_obj_t * lbl_boiler_mode;
static lv_obj_t * lbl_boiler_ot;
static int        pending_boiler_type = -1;
static int        s_ot_check_countdown = -1;
static int        s_ot_err_display_countdown = 0;

/* ============================ value callbacks ============================ */

static void on_enable_change(lv_event_t * e) {
    settings.auto_dim_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static void on_dim_wx_change(lv_event_t * e) {
    settings.show_dim_weather = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static void on_dim_bars_change(lv_event_t * e) {
    settings.show_dim_bars = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static void on_dim_bars_swap_change(lv_event_t * e) {
    settings.dim_bars_swap = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static void on_dim_bars_vertical_change(lv_event_t * e) {
    settings.dim_bars_vertical = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
}
static void on_dim_sleep_away_black_change(lv_event_t * e) {
    settings.dim_sleep_away_black = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
}
static void on_forecast_mode_change(lv_event_t * e) {
    int daily = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings.forecast_mode = daily ? FORECAST_DAILY : FORECAST_HOURLY;
    settings_save();
}
static void on_weather_feel_change(lv_event_t * e) {
    settings.weather_use_feel = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
}

/* Weather modal: free-text city label + GeoNames id, both persisted via
 * the modal_close() save path. The id is what the API actually uses; the
 * label is purely cosmetic for the Weather screen header. */
static lv_obj_t * ta_wx_city = NULL;
static lv_obj_t * ta_wx_id   = NULL;
static lv_obj_t * ta_wx_lat  = NULL;
static lv_obj_t * ta_wx_lon  = NULL;
static lv_obj_t * lbl_wx_status = NULL;
static lv_obj_t * ta_master   = NULL;
static lv_obj_t * sw_client   = NULL;
/* Auto-rotate modal widgets */
static lv_obj_t * sw_rotate       = NULL;
static lv_obj_t * ta_rotate_secs  = NULL;
static lv_obj_t * rotate_cb[16]   = {0};
static char       rotate_cb_id[16][48];
static int        rotate_cb_count = 0;
static lv_obj_t * sw_news         = NULL;
static lv_obj_t * ta_news_url     = NULL;
static lv_obj_t * lbl_news_status = NULL;
static lv_obj_t * lbl_news_speed  = NULL;
static lv_obj_t * sl_news_speed   = NULL;
static lv_obj_t * news_feeds_list = NULL;   /* multi-feed list */
static void on_weather_apply(lv_event_t * e) {
    (void)e;
    int city_changed = 0;
    if (ta_wx_city) {
        const char * c = lv_textarea_get_text(ta_wx_city);
        city_changed = strcmp(c, settings.weather_location) != 0;
        snprintf(settings.weather_location,
                 sizeof settings.weather_location, "%s", c);
    }
    /* City is authoritative: a changed name auto-resolves the Buienradar id via
     * Open-Meteo geocoding. The id field is only honoured as a manual override
     * when the city is unchanged. */
    if (city_changed && settings.weather_location[0]) {
        int gid = weather_geocode(settings.weather_location);
        if (gid > 0) settings.weather_location_id = gid;
        if (ta_wx_id) {
            char idbuf[12]; snprintf(idbuf, sizeof idbuf, "%d", settings.weather_location_id);
            lv_textarea_set_text(ta_wx_id, idbuf);
        }
    } else if (ta_wx_id) {
        int id = atoi(lv_textarea_get_text(ta_wx_id));
        if (id > 0) settings.weather_location_id = id;
    }
    if (ta_wx_lat) {
        snprintf(settings.weather_lat, sizeof settings.weather_lat, "%s", lv_textarea_get_text(ta_wx_lat));
    }
    if (ta_wx_lon) {
        snprintf(settings.weather_lon, sizeof settings.weather_lon, "%s", lv_textarea_get_text(ta_wx_lon));
    }
    settings_save();
    weather_refresh_async();
    extern void buienradar_rain_refresh_async(void);
    buienradar_rain_refresh_async();
    if (lbl_wx_status)
        lv_label_set_text_fmt(lbl_wx_status,
            tr("Opgeslagen. %s = %d", "Saved. %s = %d"), settings.weather_location,
            settings.weather_location_id);
}
static void on_dim_waste_change(lv_event_t * e) {
    settings.show_dim_waste = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static void on_waste_lead_change(lv_event_t * e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    settings.dim_waste_lead_days = v;
    if (v == 0)      lv_label_set_text(lbl_waste_lead, tr("uit", "off"));
    else if (v == 1) lv_label_set_text(lbl_waste_lead, tr("vanaf 1 dag vooraf", "from 1 day ahead"));
    else             lv_label_set_text_fmt(lbl_waste_lead, tr("vanaf %d dagen vooraf", "from %d days ahead"), v);
}
static void on_timeout_change(lv_event_t * e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    settings.auto_dim_seconds = v;
    lv_label_set_text_fmt(lbl_timeout_val, "%d s", v);
}
static void on_home_enable_change(lv_event_t * e) {
    settings.auto_home_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static void on_home_timeout_change(lv_event_t * e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    settings.auto_home_seconds = v;
    lv_label_set_text_fmt(lbl_home_timeout_val, "%d s", v);
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
#ifndef TOON1
static void on_auto_brightness_change(lv_event_t * e) {
    int on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings.auto_brightness = on;
    if (on) {
        int lvl = backlight_auto_level(settings.dim_brightness, settings.active_brightness);
        backlight_set(lvl >= 0 ? lvl : settings.active_brightness);
    } else {
        backlight_set(settings.active_brightness);
    }
}
#endif
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
    lv_obj_set_size(row, SX(800), SY(74));
    int row_y = SY(y);
    int min_y = SY(64) + (DISP_VER < 600 ? 12 : 6);
    if (row_y < min_y) row_y = min_y;
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, SX(4), row_y);
    lv_obj_set_style_bg_color(row, lv_color_hex(THEME_ROW_BG), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_all(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);   /* swallow taps, don't close modal */

    lv_obj_t * lbl = lv_label_create(row);
    lv_obj_set_style_text_color(lbl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl, SF(22), 0);
    lv_label_set_text(lbl, title);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    if (out_val_lbl) {
        *out_val_lbl = lv_label_create(row);
        lv_obj_set_style_text_color(*out_val_lbl, lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(*out_val_lbl, SF(22), 0);
        lv_obj_align(*out_val_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    return row;
}

/* A slider that fills the bottom of a row built by panel_row(). */
static lv_obj_t * row_slider(lv_obj_t * row, int lo, int hi, int val,
                             lv_event_cb_t cb) {
    lv_obj_t * s = lv_slider_create(row);
    lv_obj_set_size(s, SX(760), SY(14));
    lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, SY(-2));
    lv_slider_set_range(s, lo, hi);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return s;
}

/* A switch pinned to the right of a panel_row(). */
static lv_obj_t * row_switch(lv_obj_t * row, int checked, lv_event_cb_t cb) {
    lv_obj_t * sw = lv_switch_create(row);
    lv_obj_set_size(sw, SX(64), SY(30));
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, SX(-4), 0);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* If row has a value label (child 1), align it cleanly to the left of the switch */
    if (lv_obj_get_child_cnt(row) >= 3) {
        lv_obj_t * val_lbl = lv_obj_get_child(row, 1);
        lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, SX(-80), 0);
    }
    return sw;
}

/* ===================== on-screen keyboard for modals =====================
 * The Toon registers only a touch indev, so no hardware/VNC keyboard ever
 * reaches LVGL. Every textarea in these modals therefore needs a tappable
 * keyboard. We keep one floating keyboard on the top layer that pops up when
 * a textarea is tapped and hides on OK/close/defocus. modal_open() schedules
 * settings_attach_kb_async() so it runs once the modal's textareas exist. */
static lv_obj_t * g_kb = NULL;

/* Numeric keypad with ':' and '.' (host:port / IP entry) — mirrors LVGL's
   default number map but swaps the unused '+/-' for ':'. */
static const char * const kb_num_map[] = {
    "1", "2", "3", LV_SYMBOL_KEYBOARD, "\n",
    "4", "5", "6", LV_SYMBOL_OK, "\n",
    "7", "8", "9", LV_SYMBOL_BACKSPACE, "\n",
    ":", "0", ".", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, ""
};
static const lv_btnmatrix_ctrl_t kb_num_ctrl[] = {
    1, 1, 1, LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
    1, 1, 1, LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
    1, 1, 1, 2,
    1, 1, 1, 1, 1
};

static lv_obj_t * s_last_scroll_parent = NULL;

static void kb_hide(void) {
    if (g_kb) lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
    if (s_last_scroll_parent && lv_obj_is_valid(s_last_scroll_parent)) {
        lv_obj_set_style_pad_bottom(s_last_scroll_parent, SUNI(40), 0);
        s_last_scroll_parent = NULL;
    }
}

static void kb_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL) kb_hide();
}
/* Tag a textarea as numeric/IP so its keyboard opens on the number pad (digits
 * + '.' on one screen — no bouncing to the letters screen for the dots). The
 * keyboard-icon key still switches to letters for the odd hostname. */
#define KB_NUMERIC ((void *)(intptr_t)0x4e554d)   /* 'NUM' */
static void ta_make_numeric(lv_obj_t * ta) { if (ta) lv_obj_set_user_data(ta, KB_NUMERIC); }

static void ta_kb_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    if (c == LV_EVENT_FOCUSED || c == LV_EVENT_CLICKED) {
        if (!g_kb) {
            g_kb = lv_keyboard_create(lv_layer_top());
            lv_obj_set_size(g_kb, LV_HOR_RES, SY(240));
            lv_obj_align(g_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_add_event_cb(g_kb, kb_event, LV_EVENT_ALL, NULL);
            lv_keyboard_set_map(g_kb, LV_KEYBOARD_MODE_NUMBER, (const char **)kb_num_map, kb_num_ctrl);
        }
        lv_keyboard_set_textarea(g_kb, ta);
        lv_keyboard_set_mode(g_kb, lv_obj_get_user_data(ta) == KB_NUMERIC
                             ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_clear_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_kb);

        /* Scroll the textarea up so it is never obscured by the keyboard */
        lv_obj_t * p = lv_obj_get_parent(ta);
        while (p && !lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) {
            p = lv_obj_get_parent(p);
        }
        if (p) {
            s_last_scroll_parent = p;
            lv_obj_set_style_pad_bottom(p, SY(260), 0);
            lv_obj_update_layout(p);
            lv_area_t ta_area;
            lv_obj_get_coords(ta, &ta_area);
            lv_coord_t kb_top = DISP_VER - SY(240);
            if (ta_area.y2 >= kb_top - SY(10)) {
                lv_coord_t shift = ta_area.y2 - (kb_top - SY(25));
                lv_obj_scroll_by_bounded(p, 0, -shift, LV_ANIM_ON);
            }
        }
    } else if (c == LV_EVENT_DEFOCUSED) {
        kb_hide();
    } else if (c == LV_EVENT_DELETE) {
        /* Textarea (and its modal) going away — drop the dangling binding. */
        if (g_kb && lv_keyboard_get_textarea(g_kb) == ta) {
            lv_keyboard_set_textarea(g_kb, NULL);
            kb_hide();
        }
    }
}
static void settings_attach_kb_tree(lv_obj_t * obj) {
    uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t * ch = lv_obj_get_child(obj, i);
        if (lv_obj_check_type(ch, &lv_textarea_class))
            lv_obj_add_event_cb(ch, ta_kb_event, LV_EVENT_ALL, NULL);
        settings_attach_kb_tree(ch);
    }
}
static void settings_attach_kb_async(void * panel) { settings_attach_kb_tree((lv_obj_t *)panel); }

/* ============================ modal infra ============================ */

static void modal_timer_cb(lv_timer_t * t) { (void)t; if (modal_tick_fn) modal_tick_fn(); }

static void modal_close(lv_event_t * e) {
    (void)e;
    kb_hide();
    if (confirm_box) { lv_obj_del_async(confirm_box); confirm_box = NULL; }
    if (modal_timer) { lv_timer_del(modal_timer); modal_timer = NULL; }
    modal_tick_fn = NULL;
    if (cur_modal) {
        lv_obj_t * m = cur_modal;
        cur_modal = parent_modal;     /* restore the modal underneath, if any */
        parent_modal = NULL;
        lv_obj_del_async(m);          /* async: we're inside a descendant's event */
    }
    if (e) lv_event_stop_bubbling(e); /* prevent the click from bubbling to the
                                         backdrop and firing modal_close twice */
    settings_save();                  /* persist whatever the modal changed */
}

static void clean_finish(void);

void screen_settings_close_modals(void) {
    clean_finish();
    while (cur_modal) {
        modal_close(NULL);
    }
}

/* Build a dimmed full-screen backdrop + centred panel. Returns the panel;
   caller positions its content below y≈64 (title + close button live there). */
static lv_obj_t * modal_open(const char * title, int panel_h) {
    /* Parent on the TOP LAYER, not lv_scr_act(): the Settings grid scrolls, and
     * a backdrop parented to the scrolled screen sits at content-(0,0) — i.e.
     * above the viewport once you've scrolled down to a low tile (e.g. Agenda),
     * so the centered panel's top runs off-screen ("scroll the whole screen up
     * to see it"). The top layer is a fixed full-screen overlay that's never
     * scrolled, so the panel always centers on the real screen — and it still
     * works when a modal is opened from the home screen too. */
    if (cur_modal) parent_modal = cur_modal;  /* nesting: save the modal underneath */
    cur_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(cur_modal);
    lv_obj_set_size(cur_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(cur_modal, 0, 0);
    lv_obj_set_style_bg_color(cur_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cur_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(cur_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cur_modal, LV_OBJ_FLAG_CLICKABLE);          /* tap-outside target */
    lv_obj_add_event_cb(cur_modal, modal_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t * panel = lv_obj_create(cur_modal);
    int h = SY(panel_h);
    if (h > DISP_VER - SUNI(20))
        h = DISP_VER - SUNI(20);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(panel, SX(860), h);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(settings.home_light_mode ? 0xffffff : 0x16243a), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_clip_corner(panel, true, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);              /* stop taps reaching backdrop */

    /* Scrollable body hosting caller controls.
     * Starts at (0,0) so existing callers' y-offsets (e.g. y=70) place content
     * cleanly below the fixed 64px header. */
    lv_obj_t * body = lv_obj_create(panel);
    lv_obj_set_size(body, SX(860), h);
    lv_obj_set_pos(body, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_pad_hor(body, 20, 0);
    lv_obj_set_style_pad_top(body, 0, 0);
    lv_obj_set_style_pad_bottom(body, SUNI(40), 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    /* Fixed top header bar on top of the scrolling body so title & "X" never scroll off. */
    lv_obj_t * hdr = lv_obj_create(panel);
    lv_obj_set_size(hdr, SX(860), SY(64));
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(settings.home_light_mode ? 0xffffff : 0x16243a), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 18, 0);
    lv_obj_set_style_pad_hor(hdr, 20, 0);
    lv_obj_set_style_pad_ver(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * t = lv_label_create(hdr);
    lv_obj_set_style_text_font(t, SF(28), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(THEME_TEXT_HI), 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, SX(4), 0);

    lv_obj_t * x = lv_btn_create(hdr);
    int x_w = (DISP_HOR < 1000) ? 68 : 74;
    int x_h = (DISP_VER < 600) ? 46 : 50;
    lv_obj_set_size(x, x_w, x_h);
    lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(x, 12, 0);
    lv_obj_set_ext_click_area(x, (DISP_VER < 600) ? 6 : 10);
    lv_obj_add_event_cb(x, modal_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(xl, SF(22), 0);
    lv_obj_set_style_text_color(xl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_center(xl);

    /* After the caller fills this panel, wire a pop-up keyboard to every
     * textarea it created (runs on the next LVGL tick). */
    lv_async_call(settings_attach_kb_async, panel);
    return body;
}

/* ====================== boiler-type confirm dialog ====================== */

static void confirm_dismiss(void) {
    if (confirm_box) { lv_obj_del_async(confirm_box); confirm_box = NULL; }
}

static void on_boiler_confirm_yes(lv_event_t * e) {
    (void)e;
    if (pending_boiler_type == 0 || pending_boiler_type == 1) {
        boxtalk_set_boiler_type(pending_boiler_type);
        if (pending_boiler_type == 0) {
            /* User requested OpenTherm: start a 35-second countdown to verify OT boiler comms.
             * happ_thermstat takes up to ~30s to exhaust retries if no boiler is connected. */
            s_ot_check_countdown = 35;
            s_ot_err_display_countdown = 0;
            toon_state.ot_comm_error = 0;
        } else {
            s_ot_check_countdown = -1;
            s_ot_err_display_countdown = 0;
        }
    }
    confirm_dismiss();
}

static void on_boiler_confirm_no(lv_event_t * e) {
    (void)e;
    /* revert the switch to whatever the boiler currently reports */
    if (sw_boiler) {
        if (toon_state.boiler_type == 0) lv_obj_clear_state(sw_boiler, LV_STATE_CHECKED);
        else                             lv_obj_add_state(sw_boiler, LV_STATE_CHECKED);
    }
    confirm_dismiss();
}

static void on_boiler_switch(lv_event_t * e) {
    int want_onoff = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    pending_boiler_type = want_onoff;     /* 1 = On/Off, 0 = OpenTherm */

    /* Confirm dialog, layered on top of the Heating modal. */
    confirm_box = lv_obj_create(cur_modal);
    lv_obj_remove_style_all(confirm_box);
    lv_obj_set_size(confirm_box, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(confirm_box, 0, 0);
    lv_obj_set_style_bg_color(confirm_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(confirm_box, LV_OPA_60, 0);
    lv_obj_clear_flag(confirm_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(confirm_box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * dlg = lv_obj_create(confirm_box);
    lv_obj_set_size(dlg, SX(720), SY(360));
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x2a1c1c), 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0xcc5544), 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 16, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_clear_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dlg, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * dt = lv_label_create(dlg);
    lv_obj_set_style_text_font(dt, SF(28), 0);
    lv_obj_set_style_text_color(dt, lv_color_hex(0xffcc66), 0);
    lv_label_set_text(dt, tr("Ketelregeling wijzigen?", "Change boiler control?"));
    lv_obj_align(dt, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t * body = lv_label_create(dlg);
    lv_obj_set_style_text_font(body, SF(18), 0);
    lv_obj_set_style_text_color(body, lv_color_hex(0xddddee), 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, SX(672));
    lv_label_set_text_fmt(body,
        tr("Ketelregeling omzetten naar %s?\n\n"
           "Dit verandert hoe Toon je ketel aanstuurt en wordt direct naar "
           "het apparaat geschreven. Wijzig dit alleen als je je ketel-"
           "bedrading kent - de verkeerde instelling kan je verwarming uitschakelen.",
           "Switch boiler control to %s?\n\n"
           "This reconfigures how Toon drives your boiler and is written to "
           "the device immediately. Only change it if you know your boiler "
           "wiring - the wrong setting can stop your heating from working."),
        want_onoff ? "On/Off" : "OpenTherm");
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, SY(48));

    lv_obj_t * b_no = lv_btn_create(dlg);
    lv_obj_set_size(b_no, SX(200), SY(64));
    lv_obj_align(b_no, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(b_no, lv_color_hex(0x44556a), 0);
    lv_obj_set_style_radius(b_no, 12, 0);
    lv_obj_add_event_cb(b_no, on_boiler_confirm_no, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_no_l = lv_label_create(b_no);
    lv_label_set_text(b_no_l, tr("Annuleren", "Cancel"));
    lv_obj_set_style_text_font(b_no_l, SF(22), 0);
    lv_obj_center(b_no_l);

    lv_obj_t * b_yes = lv_btn_create(dlg);
    lv_obj_set_size(b_yes, SX(260), SY(64));
    lv_obj_align(b_yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(b_yes, lv_color_hex(0xcc5544), 0);
    lv_obj_set_style_radius(b_yes, 12, 0);
    lv_obj_add_event_cb(b_yes, on_boiler_confirm_yes, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_yes_l = lv_label_create(b_yes);
    lv_label_set_text_fmt(b_yes_l, tr("Zet %s", "Set %s"), want_onoff ? "On/Off" : "OpenTherm");
    lv_obj_set_style_text_font(b_yes_l, SF(22), 0);
    lv_obj_center(b_yes_l);
}

/* ============================ live ticks ============================ */

static void heating_tick(void) {
    if (s_ot_err_display_countdown > 0) {
        s_ot_err_display_countdown--;
        if (lbl_boiler_mode) {
            lv_label_set_text(lbl_boiler_mode, tr("Huidige modus: On/Off (OT-fout)", "Current mode: On/Off (OT error)"));
        }
        if (lbl_boiler_ot) {
            lv_label_set_text(lbl_boiler_ot, tr("Geen ketelreactie: teruggezet naar On/Off", "No boiler response: reverted to On/Off"));
        }
        return;
    }

    /* If in OpenTherm mode and communication error is flagged (e.g. no boiler attached
     * to keteladapter), immediately revert back to On/Off (CR6). */
    if (toon_state.boiler_type == 0 && toon_state.ot_comm_error != 0) {
        s_ot_check_countdown = -1;
        s_ot_err_display_countdown = 6; /* Display error explanation for 6 seconds */
        boxtalk_set_boiler_type(1);
        toon_state.boiler_type = 1;
        if (sw_boiler) {
            lv_obj_add_state(sw_boiler, LV_STATE_CHECKED);
        }
        if (lbl_boiler_mode) {
            lv_label_set_text(lbl_boiler_mode, tr("Huidige modus: On/Off (OT-fout)", "Current mode: On/Off (OT error)"));
        }
        if (lbl_boiler_ot) {
            lv_label_set_text(lbl_boiler_ot, tr("Geen ketelreactie: teruggezet naar On/Off", "No boiler response: reverted to On/Off"));
        }
        return;
    }

    if (s_ot_check_countdown > 0) {
        s_ot_check_countdown--;
        if (lbl_boiler_mode) {
            /* Keep concise so it never collides with the switch label on the right */
            lv_label_set_text(lbl_boiler_mode, tr("Huidige modus: controleren...", "Current mode: checking..."));
        }
        if (lbl_boiler_ot) {
            /* Full-width line at SY(70) displays the remaining seconds */
            lv_label_set_text_fmt(lbl_boiler_ot,
                tr("OpenTherm ketel zoeken... (%d s)", "Searching for OpenTherm boiler... (%d s)"),
                s_ot_check_countdown + 1);
        }
        return;
    } else if (s_ot_check_countdown == 0) {
        s_ot_check_countdown = -1;
        /* Timeout reached: check if OpenTherm communications are active */
        if (toon_state.ot_comm_error != 0) {
            /* No OpenTherm comms found: move back to On/Off (CR6) */
            s_ot_err_display_countdown = 6;
            boxtalk_set_boiler_type(1);
            toon_state.boiler_type = 1;
            if (sw_boiler) {
                lv_obj_add_state(sw_boiler, LV_STATE_CHECKED);
            }
            if (lbl_boiler_mode) {
                lv_label_set_text(lbl_boiler_mode, tr("Huidige modus: On/Off (OT-fout)", "Current mode: On/Off (OT error)"));
            }
            if (lbl_boiler_ot) {
                lv_label_set_text(lbl_boiler_ot, tr("Geen ketelreactie: teruggezet naar On/Off", "No boiler response: reverted to On/Off"));
            }
            return;
        }
    }

    if (lbl_boiler_mode) {
        const char * m = (toon_state.boiler_type == 0) ? "OpenTherm" : "On/Off";
        lv_label_set_text_fmt(lbl_boiler_mode, tr("Huidige modus: %s", "Current mode: %s"), m);
    }
    if (lbl_boiler_ot) {
        if (toon_state.boiler_type == 0) {
            lv_label_set_text_fmt(lbl_boiler_ot, tr("Modulatie %d%%   -   OT-link %s", "Modulation %d%%   -   OT link %s"),
                                  (int)toon_state.modulation_level,
                                  toon_state.ot_comm_error ? tr("FOUT", "ERROR") : "OK");
        } else {
            lv_label_set_text(lbl_boiler_ot, "");
        }
    }
}

/* Language selector — OFF = Nederlands (LANG_NL), ON = English (LANG_EN).
   Switching takes effect on the next modal/screen build (tr() reads
   settings.language live); no restart needed. */
static lv_obj_t * lbl_lang_val __attribute__((unused)) = NULL;
static void on_language_change(lv_event_t * e) __attribute__((unused));
static void on_language_change(lv_event_t * e) {
    int on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings.language = on ? LANG_EN : LANG_NL;
    if (lbl_lang_val)
        lv_label_set_text(lbl_lang_val, on ? "English" : "Nederlands");
    settings_save();
}

/* Home theme — OFF = freetoon dark dashboard, ON = stock light tile-carousel.
   Applies on the next home build (return-to-home / restart); tr() & ui_init
   read settings.home_theme live. */
static void on_stock_big_indoor_change(lv_event_t * e) __attribute__((unused));
static void on_stock_big_indoor_change(lv_event_t * e) {
    settings.stock_big_indoor = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
}
static lv_obj_t * lbl_theme_val = NULL;
static lv_obj_t * lbl_light_mode_val = NULL;
static lv_obj_t * sw_light_mode = NULL;
static void settings_apply_theme(void);

static void on_home_theme_change(lv_event_t * e) {
    int on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings.home_theme = on ? 1 : 0;
    if (lbl_theme_val)
        lv_label_set_text(lbl_theme_val, on ? tr("Stock", "Stock")
                                            : tr("freetoon", "freetoon"));
    if (on) {
        settings.home_light_mode = 1;
        if (lbl_light_mode_val)
            lv_label_set_text(lbl_light_mode_val, tr("Aan", "On"));
        if (sw_light_mode)
            lv_obj_add_state(sw_light_mode, LV_STATE_CHECKED);
        settings_apply_theme();
        ui_refresh_theme();
        scr_light_mode = -1;
    }
    settings_save();
    ui_apply_home_theme();   /* live swap — visible next time home is shown */
}

static void settings_apply_theme(void) {
    if (scr_root) {
        lv_obj_set_style_bg_color(scr_root, lv_color_hex(THEME_BG), 0);
        uint32_t cnt = lv_obj_get_child_cnt(scr_root);
        for (uint32_t i = 0; i < cnt; i++) {
            lv_obj_t * ch = lv_obj_get_child(scr_root, i);
            if (lv_obj_check_type(ch, &lv_btn_class)) {
                lv_obj_set_style_bg_color(ch, lv_color_hex(THEME_BTN_BG), 0);
                if (lv_obj_get_child_cnt(ch) > 0)
                    lv_obj_set_style_text_color(lv_obj_get_child(ch, 0), lv_color_hex(THEME_TEXT_HI), 0);
            } else if (lv_obj_check_type(ch, &lv_label_class)) {
                lv_obj_set_style_text_color(ch, lv_color_hex(THEME_TEXT_HI), 0);
            }
        }
    }
    if (scr_tiles_cont) {
        uint32_t n = lv_obj_get_child_cnt(scr_tiles_cont);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t * t = lv_obj_get_child(scr_tiles_cont, i);
            if (!t) continue;
            lv_obj_set_style_bg_color(t, lv_color_hex(THEME_CARD_BG), 0);
            lv_obj_set_style_bg_color(t, lv_color_hex(THEME_BTN_BG), LV_STATE_PRESSED);
            uint32_t tc = lv_obj_get_child_cnt(t);
            for (uint32_t j = 0; j < tc; j++) {
                lv_obj_t * c = lv_obj_get_child(t, j);
                if (lv_obj_check_type(c, &lv_img_class)) {
                    lv_obj_set_style_img_recolor(c, lv_color_hex(settings.home_light_mode ? 0x2563eb : 0x9fc4e6), 0);
                } else if (lv_obj_check_type(c, &lv_label_class)) {
                    const lv_font_t * f = lv_obj_get_style_text_font(c, 0);
                    if (f == SF(14)) {
                        lv_obj_set_style_text_color(c, lv_color_hex(THEME_TEXT_DIM), 0);
                    } else if (f == SF(28) || f == SF(22)) {
                        if (lv_obj_get_y(c) < 20 && lv_obj_get_child_cnt(t) == 3 && j == 0) {
                            lv_obj_set_style_text_color(c, lv_color_hex(settings.home_light_mode ? 0x2563eb : 0x9fc4e6), 0);
                        } else {
                            lv_obj_set_style_text_color(c, lv_color_hex(THEME_TEXT_HI), 0);
                        }
                    }
                }
            }
        }
    }
    if (cur_modal) {
        lv_obj_t * panel = lv_obj_get_child(cur_modal, 0);
        if (panel) {
            lv_obj_set_style_bg_color(panel, lv_color_hex(settings.home_light_mode ? 0xffffff : 0x16243a), 0);
            uint32_t pc = lv_obj_get_child_cnt(panel);
            for (uint32_t pci = 0; pci < pc; pci++) {
                lv_obj_t * ch = lv_obj_get_child(panel, pci);
                if (pci == 0) {
                    /* Scrollable body */
                    uint32_t bc = lv_obj_get_child_cnt(ch);
                    for (uint32_t bi = 0; bi < bc; bi++) {
                        lv_obj_t * row = lv_obj_get_child(ch, bi);
                        if (row && lv_obj_get_width(row) > 400) {
                            lv_obj_set_style_bg_color(row, lv_color_hex(THEME_ROW_BG), 0);
                            uint32_t rc = lv_obj_get_child_cnt(row);
                            if (rc > 0) lv_obj_set_style_text_color(lv_obj_get_child(row, 0), lv_color_hex(THEME_TEXT_HI), 0);
                            if (rc > 1) {
                                lv_obj_t * r1 = lv_obj_get_child(row, 1);
                                if (lv_obj_check_type(r1, &lv_label_class))
                                    lv_obj_set_style_text_color(r1, lv_color_hex(THEME_TEXT_DIM), 0);
                            }
                        }
                    }
                } else if (pci == 1) {
                    /* Header bar */
                    lv_obj_set_style_bg_color(ch, lv_color_hex(settings.home_light_mode ? 0xffffff : 0x16243a), 0);
                    uint32_t hc = lv_obj_get_child_cnt(ch);
                    if (hc > 0) lv_obj_set_style_text_color(lv_obj_get_child(ch, 0), lv_color_hex(THEME_TEXT_HI), 0);
                    if (hc > 1) {
                        lv_obj_t * xbtn = lv_obj_get_child(ch, 1);
                        lv_obj_set_style_bg_color(xbtn, lv_color_hex(THEME_BTN_BG), 0);
                        if (lv_obj_get_child_cnt(xbtn) > 0)
                            lv_obj_set_style_text_color(lv_obj_get_child(xbtn, 0), lv_color_hex(THEME_TEXT_HI), 0);
                    }
                }
            }
        }
    }
}

static void on_home_light_mode_change(lv_event_t * e) {
    int on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings.home_light_mode = on ? 1 : 0;
    if (lbl_light_mode_val)
        lv_label_set_text(lbl_light_mode_val, on ? tr("Aan", "On")
                                                 : tr("Uit", "Off"));
    settings_save();
    settings_apply_theme();
    ui_refresh_theme();
    scr_light_mode = -1;
}

static void on_clock_moon_phase_change(lv_event_t * e) __attribute__((unused));
static void on_clock_moon_phase_change(lv_event_t * e) {
    settings.clock_moon_phase = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
}

static lv_obj_t * lbl_vnc_val = NULL;
static void on_vnc_change(lv_event_t * e) {
    settings.vnc_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    if (lbl_vnc_val)
        lv_label_set_text(lbl_vnc_val, settings.vnc_enabled ? tr("Aan", "On") : tr("Uit", "Off"));
    settings_save();
    vnc_apply_state(settings.vnc_enabled);
}

/* ============================ category modals ============================ */

static void open_display_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open(tr("Scherm", "Display"), 560);
    /* Six rows don't fit a fixed panel on a 600px screen — let it scroll. */
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    int y = 70;
    lv_obj_t * r;

    r = panel_row(p, y, tr("Automatisch dimmen bij inactiviteit", "Auto-dim when idle"), NULL);
    sw_enable = row_switch(r, settings.auto_dim_enabled, on_enable_change);
    y += 82;

    r = panel_row(p, y, tr("Wachttijd inactief", "Idle timeout"), &lbl_timeout_val);
    lv_label_set_text_fmt(lbl_timeout_val, "%d s", settings.auto_dim_seconds);
    sl_timeout = row_slider(r, 5, 300, settings.auto_dim_seconds, on_timeout_change);
    y += 82;

    r = panel_row(p, y, tr("Terug naar start bij inactiviteit", "Return to home when idle"), NULL);
    sw_home_enable = row_switch(r, settings.auto_home_enabled, on_home_enable_change);
    y += 82;

    r = panel_row(p, y, tr("Wachttijd terug naar start", "Return-home timeout"), &lbl_home_timeout_val);
    lv_label_set_text_fmt(lbl_home_timeout_val, "%d s", settings.auto_home_seconds);
    sl_home_timeout = row_slider(r, 5, 600, settings.auto_home_seconds, on_home_timeout_change);
    y += 82;

    r = panel_row(p, y, tr("Helderheid actief", "Active brightness"), &lbl_act_val);
    lv_label_set_text_fmt(lbl_act_val, "%d", settings.active_brightness);
    sl_act = row_slider(r, 100, 1000, settings.active_brightness, on_act_change);
    y += 82;

    r = panel_row(p, y, tr("Helderheid gedimd", "Dim brightness"), &lbl_dim_val);
    lv_label_set_text_fmt(lbl_dim_val, "%d", settings.dim_brightness);
    sl_dim = row_slider(r, 0, 400, settings.dim_brightness, on_dim_change);
    y += 82;

    /* Screen off on sleep / away presets (pure black + backlight 0) */
    r = panel_row(p, y, tr("Scherm uit bij Slapen en Weg", "Screen off on Sleep and Away"), NULL);
    row_switch(r, settings.dim_sleep_away_black, on_dim_sleep_away_black_change);
    y += 82;

#ifndef TOON1
    /* Auto-brightness — follow the LTR-303 ambient sensor (Toon 2). When on, the
       active backlight tracks the room between the dim and active values above. */
    r = panel_row(p, y, tr("Auto-helderheid (lichtsensor)", "Auto-brightness (light sensor)"), NULL);
    row_switch(r, settings.auto_brightness, on_auto_brightness_change);
    y += 82;
#endif

    /* Usage bars flanking the dim clock: energy now (W) + gas hourly (m³),
       auto-scaled. The swap toggle flips which metric is on which side. */
    r = panel_row(p, y, tr("Verbruiksbalken op dimscherm", "Usage bars on dim screen"), NULL);
    sw_dim_bars = row_switch(r, settings.show_dim_bars, on_dim_bars_change);
    y += 82;

    r = panel_row(p, y, tr("Balken: wisselen (energie links / gas rechts)", "Bars: swap (energy left / gas right)"), NULL);
    sw_dim_bars_swap = row_switch(r, settings.dim_bars_swap, on_dim_bars_swap_change);
    y += 82;

    /* Bars orientation: horizontal vs vertical segmented (classic Toon style) */
    r = panel_row(p, y, tr("Balken: verticaal (klassieke Toon-stijl)", "Bars: vertical (classic Toon style)"), NULL);
    row_switch(r, settings.dim_bars_vertical, on_dim_bars_vertical_change);
    y += 82;

    /* Home theme — classic Toon (stock) light tile-carousel vs freetoon dark
       dashboard. Takes effect on the next return-to-home / restart. */
    r = panel_row(p, y, tr("Thema startscherm (klassieke Toon-look)",
                           "Home screen theme (classic Toon look)"), &lbl_theme_val);
    lv_label_set_text(lbl_theme_val, settings.home_theme == 1
                      ? tr("Stock", "Stock") : tr("freetoon", "freetoon"));
    row_switch(r, settings.home_theme == 1, on_home_theme_change);
    y += 82;

    /* Light theme — applies to all screens */
    r = panel_row(p, y, tr("Licht thema (alle schermen)",
                           "Light theme (all screens)"), &lbl_light_mode_val);
    lv_label_set_text(lbl_light_mode_val, settings.home_light_mode == 1
                      ? tr("Aan", "On") : tr("Uit", "Off"));
    sw_light_mode = row_switch(r, settings.home_light_mode == 1, on_home_light_mode_change);
    y += 82;

    /* VNC remote control (x11vnc server on port 5900) */
    r = panel_row(p, y, tr("VNC schermovername (poort 5900)",
                           "VNC remote control (port 5900)"), &lbl_vnc_val);
    lv_label_set_text(lbl_vnc_val, settings.vnc_enabled ? tr("Aan", "On") : tr("Uit", "Off"));
    row_switch(r, settings.vnc_enabled, on_vnc_change);
    y += 82;
}

static void open_weather_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open(tr("Weer", "Weather"), 580);
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    int y = 70;

    lv_obj_t * r = panel_row(p, y, tr("Weer tonen op dimscherm", "Show weather on dim screen"), NULL);
    sw_dim_wx = row_switch(r, settings.show_dim_weather, on_dim_wx_change);
    y += 82;

    r = panel_row(p, y, tr("5-daagse verwachting (i.p.v. per 3 uur)", "5-day forecast (instead of 3-hourly)"), NULL);
    sw_forecast_mode = row_switch(r, settings.forecast_mode == FORECAST_DAILY, on_forecast_mode_change);
    y += 82;

    r = panel_row(p, y, tr("Gevoelstemperatuur tonen", "Show feel temperature"), NULL);
    sw_weather_feel = row_switch(r, settings.weather_use_feel, on_weather_feel_change);
    y += 82;

    /* City name (cosmetic) + Buienradar GeoNames id (used by the API). */
    lv_obj_t * lbl_city = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_city, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_city, SF(22), 0);
    lv_label_set_text(lbl_city, tr("Plaats:", "City:"));
    lv_obj_align(lbl_city, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    ta_wx_city = lv_textarea_create(p);
    lv_obj_set_size(ta_wx_city, SX(380), SY(44));
    lv_obj_align(ta_wx_city, LV_ALIGN_TOP_LEFT, SX(240), SY(y - 4));
    lv_textarea_set_one_line(ta_wx_city, true);
    lv_textarea_set_text(ta_wx_city, settings.weather_location);
    y += 60;

    lv_obj_t * lbl_id = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_id, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_id, SF(22), 0);
    lv_label_set_text(lbl_id, tr("Buienradar-id:", "Buienradar id:"));
    lv_obj_align(lbl_id, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    ta_wx_id = lv_textarea_create(p);
    lv_obj_set_size(ta_wx_id, SX(380), SY(44));
    lv_obj_align(ta_wx_id, LV_ALIGN_TOP_LEFT, SX(240), SY(y - 4));
    lv_textarea_set_one_line(ta_wx_id, true);
    lv_textarea_set_accepted_chars(ta_wx_id, "0123456789");
    char id_str[16]; snprintf(id_str, sizeof id_str, "%d",
                              settings.weather_location_id);
    lv_textarea_set_text(ta_wx_id, id_str);
    y += 50;

    lv_obj_t * lbl_lat = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_lat, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_lat, SF(22), 0);
    lv_label_set_text(lbl_lat, tr("Breedtegraad (Lat):", "Latitude:"));
    lv_obj_align(lbl_lat, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    ta_wx_lat = lv_textarea_create(p);
    lv_obj_set_size(ta_wx_lat, SX(380), SY(44));
    lv_obj_align(ta_wx_lat, LV_ALIGN_TOP_LEFT, SX(240), SY(y - 4));
    lv_textarea_set_one_line(ta_wx_lat, true);
    lv_textarea_set_accepted_chars(ta_wx_lat, "0123456789.-");
    lv_textarea_set_text(ta_wx_lat, settings.weather_lat);
    y += 50;

    lv_obj_t * lbl_lon = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_lon, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_lon, SF(22), 0);
    lv_label_set_text(lbl_lon, tr("Lengtegraad (Lon):", "Longitude:"));
    lv_obj_align(lbl_lon, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    ta_wx_lon = lv_textarea_create(p);
    lv_obj_set_size(ta_wx_lon, SX(380), SY(44));
    lv_obj_align(ta_wx_lon, LV_ALIGN_TOP_LEFT, SX(240), SY(y - 4));
    lv_textarea_set_one_line(ta_wx_lon, true);
    lv_textarea_set_accepted_chars(ta_wx_lon, "0123456789.-");
    lv_textarea_set_text(ta_wx_lon, settings.weather_lon);
    y += 50;

    /* Hint — where to find the id. */
    lv_obj_t * hint = lv_label_create(p);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(hint, SF(14), 0);
    lv_label_set_text(hint,
        tr("Vind de id van je plaats in de URL op buienradar.nl/weer/<plaats>/nl/<ID>\n"
           "(bijv. Medemblik = 2751073, De Bilt = 2757783). Gewone KNMI-codes\n"
           "werken niet - dit zijn GeoNames-id's.",
           "Find your city's id in the URL on buienradar.nl/weer/<city>/nl/<ID>\n"
           "(e.g. Medemblik = 2751073, De Bilt = 2757783). Plain KNMI codes\n"
           "won't work - these are GeoNames ids."));
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    y += 70;

    lv_obj_t * btn = lv_btn_create(p);
    lv_obj_set_size(btn, SX(200), SY(50));
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a6090), 0);
    lv_obj_add_event_cb(btn, on_weather_apply, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(btn);
    lv_label_set_text(bl, tr("Toepassen", "Apply"));
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_center(bl);

    lbl_wx_status = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_wx_status, lv_color_hex(0x9fc4e6), 0);
    lv_obj_set_style_text_font(lbl_wx_status, SF(18), 0);
    lv_label_set_text(lbl_wx_status, "");
    lv_obj_align(lbl_wx_status, LV_ALIGN_TOP_LEFT, SX(220), SY(y + 12));
}

static lv_obj_t * lbl_waste_status = NULL;
static lv_obj_t * ta_waste_ics = NULL, * dd_waste_prov = NULL;
static lv_obj_t * ta_waste_pc = NULL, * ta_waste_nr = NULL, * ta_waste_icsid = NULL,
                * ta_waste_street = NULL, * ta_waste_city = NULL;

/* Provider list from the ToonSoftwareCollective plugin_index.json (the same
   dropdown the stock app shows). Fetched on modal open. */
#define WASTE_MAX_PROV 80
static char g_prov_ids[WASTE_MAX_PROV][8];
static int  g_prov_count = 0;
static char g_prov_opts[6144];

static void waste_load_providers(void) {
    extern int http_fetch(const char *, char *, size_t);
    g_prov_count = 0; g_prov_opts[0] = 0;
    static char body[24576];
    if (http_fetch("https://raw.githubusercontent.com/ToonSoftwareCollective/"
                   "wastecollection_plugins/main/plugin_index.json", body, sizeof body) != 0)
        return;
    const char * p = body;
    while (g_prov_count < WASTE_MAX_PROV) {
        const char * fn = strstr(p, "\"friendlyName\"");
        if (!fn) break;
        const char * colon = strchr(fn, ':');
        const char * q1 = colon ? strchr(colon, '"') : NULL;   /* opening quote of value */
        const char * q2 = q1 ? strchr(q1 + 1, '"') : NULL;     /* closing quote of value */
        if (!q1 || !q2) break;
        char name[64]; int nl = (int)(q2 - q1 - 1); if (nl > 63) nl = 63;
        memcpy(name, q1 + 1, nl); name[nl] = 0;
        const char * pr = strstr(q2, "\"provider\"");
        const char * v1 = pr ? strchr(pr + 10, '"') : NULL;
        const char * v2 = v1 ? strchr(v1 + 1, '"') : NULL;
        if (!v1 || !v2) break;
        int il = (int)(v2 - v1 - 1); if (il > 7) il = 7;
        memcpy(g_prov_ids[g_prov_count], v1 + 1, il); g_prov_ids[g_prov_count][il] = 0;
        if (g_prov_count) strncat(g_prov_opts, "\n", sizeof g_prov_opts - strlen(g_prov_opts) - 1);
        strncat(g_prov_opts, name, sizeof g_prov_opts - strlen(g_prov_opts) - 1);
        g_prov_count++;
        p = v2 + 1;
    }
}

static void waste_set_field_upper(char * dst, size_t dsz, const char * src) {
    size_t o = 0;
    for (const char * q = src; *q && o + 1 < dsz; q++) {
        unsigned char c = (unsigned char)*q;
        if (c == ' ') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[o++] = (char)c;
    }
    dst[o] = 0;
}

static void on_waste_apply(lv_event_t * e) {
    (void)e;
    if (dd_waste_prov && g_prov_count) {
        int idx = lv_dropdown_get_selected(dd_waste_prov);
        if (idx >= 0 && idx < g_prov_count)
            snprintf(settings.waste_plugin, sizeof settings.waste_plugin, "%s", g_prov_ids[idx]);
    }
    if (ta_waste_pc)    waste_set_field_upper(settings.waste_postcode, sizeof settings.waste_postcode,
                                              lv_textarea_get_text(ta_waste_pc));
    if (ta_waste_nr)    snprintf(settings.waste_housenr, sizeof settings.waste_housenr, "%s", lv_textarea_get_text(ta_waste_nr));
    if (ta_waste_icsid) snprintf(settings.waste_icsid,   sizeof settings.waste_icsid,   "%s", lv_textarea_get_text(ta_waste_icsid));
    if (ta_waste_street)snprintf(settings.waste_street,  sizeof settings.waste_street,  "%s", lv_textarea_get_text(ta_waste_street));
    if (ta_waste_city)  snprintf(settings.waste_city,    sizeof settings.waste_city,    "%s", lv_textarea_get_text(ta_waste_city));
    if (ta_waste_ics)   snprintf(settings.waste_ics_url, sizeof settings.waste_ics_url, "%s", lv_textarea_get_text(ta_waste_ics));
    settings_save();
    waste_state.wake_fetch = 1;   /* wake the background fetcher so the
                                     "refreshes within a few seconds" status
                                     below is actually true (else up to 4h). */
    if (lbl_waste_status)
        lv_label_set_text(lbl_waste_status,
            tr("Opgeslagen - de kalender ververst binnen enkele seconden.",
               "Saved - the calendar refreshes within a few seconds."));
}

/* small labelled one-line text field; returns the textarea */
static lv_obj_t * waste_field(lv_obj_t * p, int x, int y, int w, const char * lbl, const char * val) {
    lv_obj_t * l = lv_label_create(p);
    lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(l, SF(18), 0);
    lv_label_set_text(l, lbl);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, SX(x), SY(y));
    lv_obj_t * ta = lv_textarea_create(p);
    lv_obj_set_size(ta, SX(w), SY(44));
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, SX(x), SY(y + 26));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, val ? val : "");
    return ta;
}

static void open_waste_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open(tr("Afval", "Waste"), 720);
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    int y = 70;
    lv_obj_t * r;

    r = panel_row(p, y, tr("Afval tonen op dimscherm", "Show waste on dim screen"), NULL);
    sw_dim_waste = row_switch(r, settings.show_dim_waste, on_dim_waste_change);
    y += 82;

    r = panel_row(p, y, tr("Afval-waarschuwingsvenster", "Waste alert window"), &lbl_waste_lead);
    if      (settings.dim_waste_lead_days == 0) lv_label_set_text(lbl_waste_lead, tr("uit", "off"));
    else if (settings.dim_waste_lead_days == 1) lv_label_set_text(lbl_waste_lead, tr("vanaf 1 dag vooraf", "from 1 day ahead"));
    else lv_label_set_text_fmt(lbl_waste_lead, tr("vanaf %d dagen vooraf", "from %d days ahead"), settings.dim_waste_lead_days);
    sl_waste_lead = row_slider(r, 0, 7, settings.dim_waste_lead_days, on_waste_lead_change);
    y += 90;

    /* Provider dropdown — the ToonSoftwareCollective plugin list (same as the
       stock Afvalkalender app). Fetched live; the wastefetch helper then runs
       the chosen provider's script. */
    lv_obj_t * lp = lv_label_create(p);
    lv_obj_set_style_text_color(lp, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lp, SF(22), 0);
    lv_label_set_text(lp, tr("Afvalverwerker (provider):", "Waste collector (provider):"));
    lv_obj_align(lp, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    waste_load_providers();
    dd_waste_prov = lv_dropdown_create(p);
    lv_obj_set_width(dd_waste_prov, SX(720));
    lv_obj_align(dd_waste_prov, LV_ALIGN_TOP_LEFT, SX(4), SY(y + 30));
    if (g_prov_count) {
        lv_dropdown_set_options(dd_waste_prov, g_prov_opts);
        for (int i = 0; i < g_prov_count; i++)
            if (strcmp(g_prov_ids[i], settings.waste_plugin) == 0) { lv_dropdown_set_selected(dd_waste_prov, i); break; }
    } else {
        lv_dropdown_set_options(dd_waste_prov, tr("(providerlijst niet beschikbaar - controleer internet)", "(provider list unavailable - check internet)"));
    }
    y += 96;

    /* Per-provider fields — fill the ones your provider needs (postcode+nr, or a
       calendar number/ICS-id, or street+city). Hint shown per provider's info. */
    ta_waste_pc    = waste_field(p, 4,   y, 220, tr("Postcode", "Postcode"),        settings.waste_postcode);
    ta_waste_nr    = waste_field(p, 240, y, 120, tr("Huisnr", "House no."),          settings.waste_housenr);
    ta_waste_icsid = waste_field(p, 380, y, 360, tr("Kalender-/ICS-id", "Calendar/ICS id"), settings.waste_icsid);
    ta_make_numeric(ta_waste_nr); ta_make_numeric(ta_waste_icsid);
    y += 84;
    ta_waste_street = waste_field(p, 4,   y, 360, tr("Straat (opt)", "Street (opt)"),   settings.waste_street);
    ta_waste_city   = waste_field(p, 380, y, 360, tr("Plaats (opt)", "City (opt)"),   settings.waste_city);
    y += 84;
    ta_waste_ics = waste_field(p, 4, y, 736, tr("of: eigen iCal / ICS URL", "or: own iCal / ICS URL"), settings.waste_ics_url);
    y += 92;

    lv_obj_t * apply = lv_btn_create(p);
    lv_obj_set_size(apply, SX(200), SY(50));
    lv_obj_align(apply, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    lv_obj_set_style_bg_color(apply, lv_color_hex(0x2e6e3a), 0);
    lv_obj_add_event_cb(apply, on_waste_apply, LV_EVENT_CLICKED, NULL);
    lv_obj_t * al = lv_label_create(apply); lv_label_set_text(al, tr("Opslaan", "Save")); lv_obj_center(al);

    lbl_waste_status = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_waste_status, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_waste_status, SF(14), 0);
    lv_obj_set_width(lbl_waste_status, SX(736));
    lv_label_set_long_mode(lbl_waste_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_waste_status, tr("Kies een provider en vul de velden die deze nodig heeft.", "Pick a provider and fill in the fields it needs."));
    lv_obj_align(lbl_waste_status, LV_ALIGN_TOP_LEFT, SX(220), SY(y + 8));
}

static void open_heating_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open(tr("Verwarming", "Heating"), 470);
    int y = 70;

    /* indoor temp calibration */
    lv_obj_t * r = panel_row(p, y, tr("Correctie binnentemperatuur", "Indoor temp offset"), &lbl_offset_val);
    if (settings.client_mode) {
        lv_label_set_text(lbl_offset_val, tr("Beheerd door Master", "Managed by Master"));
        sl_offset = row_slider(r, -500, 500, 0, on_offset_change);
        lv_obj_add_state(sl_offset, LV_STATE_DISABLED);
    } else {
        lv_label_set_text_fmt(lbl_offset_val, "%+.1f C", settings.temp_offset_centi / 100.0f);
        sl_offset = row_slider(r, -500, 500, settings.temp_offset_centi, on_offset_change);
    }
    y += 90;

    /* boiler control type */
    lv_obj_t * box = lv_obj_create(p);
    lv_obj_set_size(box, SX(800), SY(200));
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1f3050), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * hdr = lv_label_create(box);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(hdr, SF(22), 0);
    lv_label_set_text(hdr, tr("Ketelregeling", "Boiler control"));
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);

    s_ot_check_countdown = -1;
    s_ot_err_display_countdown = 0;
    sw_boiler = lv_switch_create(box);
    lv_obj_set_size(sw_boiler, SX(64), SY(30));
    lv_obj_align(sw_boiler, LV_ALIGN_TOP_RIGHT, SX(-4), SY(2));
    if (toon_state.boiler_type == 0) lv_obj_clear_state(sw_boiler, LV_STATE_CHECKED);
    else                             lv_obj_add_state(sw_boiler, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_boiler, on_boiler_switch, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * swlbl = lv_label_create(box);
    lv_obj_set_style_text_color(swlbl, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(swlbl, SF(18), 0);
    lv_label_set_text(swlbl, tr("Uit = OpenTherm    Aan = On/Off", "Off = OpenTherm    On = On/Off"));
    lv_obj_align(swlbl, LV_ALIGN_TOP_RIGHT, SX(-4), SY(38));

    lbl_boiler_mode = lv_label_create(box);
    lv_obj_set_style_text_color(lbl_boiler_mode, lv_color_hex(0xffcc44), 0);
    lv_obj_set_style_text_font(lbl_boiler_mode, SF(22), 0);
    lv_obj_align(lbl_boiler_mode, LV_ALIGN_TOP_LEFT, 0, SY(36));

    lbl_boiler_ot = lv_label_create(box);
    lv_obj_set_style_text_color(lbl_boiler_ot, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_boiler_ot, SF(18), 0);
    lv_obj_align(lbl_boiler_ot, LV_ALIGN_TOP_LEFT, 0, SY(70));

    lv_obj_t * warn = lv_label_create(box);
    lv_obj_set_style_text_color(warn, lv_color_hex(0xcc7766), 0);
    lv_obj_set_style_text_font(warn, SF(18), 0);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(warn, SX(772));
    lv_label_set_text(warn,
        tr("Wijzigen schrijft direct naar de ketel. Je wordt om bevestiging gevraagd.",
           "Changing this writes to the live boiler. You'll be asked to confirm."));
    lv_obj_align(warn, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* refresh the boiler labels live + re-query the type */
    heating_tick();
    boxtalk_get_boiler_type();
    modal_tick_fn = heating_tick;
    modal_timer = lv_timer_create(modal_timer_cb, 1000, NULL);
}

/* ---------- Preset temperatures ---------- */
/* Each scheme state (0=Comfort..3=Away) has a stored room-setpoint inside
 * happ_thermstat — that's the temperature the schedule daemon snaps to
 * when it transitions between presets, AND what the one-tap preset
 * buttons on the home/heater screens apply. Without an editor those
 * values can only be set via the stock Toon UI (which we replaced) or
 * by hand-poking the HTTP API. */
static lv_obj_t * lbl_preset_val[4];
static int        last_preset_centi[4] = {-1,-1,-1,-1};

static void on_preset_change(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int v = lv_slider_get_value(lv_event_get_target(e));
    /* Quantise to 0.5 °C steps so the displayed value matches the schedule
     * editor's resolution and we don't write a fresh HTTP for each pixel. */
    int snapped = ((v + 25) / 50) * 50;
    if (snapped < 500)  snapped = 500;
    if (snapped > 3000) snapped = 3000;
    if (lbl_preset_val[idx])
        lv_label_set_text_fmt(lbl_preset_val[idx], "%.1f C", snapped / 100.0f);
    if (snapped != last_preset_centi[idx]) {
        last_preset_centi[idx] = snapped;
        boxtalk_set_state_value(idx, snapped);
    }
}

static void open_presets_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open(tr("Voorkeurstemperaturen", "Preset temperatures"), 430);
    static const char * names[4];
    names[0] = tr("Comfort", "Comfort");
    names[1] = tr("Thuis",   "Home");
    names[2] = tr("Slapen",  "Sleep");
    names[3] = tr("Weg",     "Away");
    int y = 70;
    for (int i = 0; i < 4; i++) {
        int v = boxtalk_get_state_value(i);
        if (v < 500) v = 2000;   /* fallback if happ_thermstat unreachable */
        last_preset_centi[i] = v;
        lv_obj_t * r = panel_row(p, y, names[i], &lbl_preset_val[i]);
        lv_label_set_text_fmt(lbl_preset_val[i], "%.1f C", v / 100.0f);
        lv_obj_t * s = lv_slider_create(r);
        lv_obj_set_size(s, SX(760), SY(14));
        lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, SY(-2));
        lv_slider_set_range(s, 500, 3000);
        lv_slider_set_value(s, v, LV_ANIM_OFF);
        lv_obj_add_event_cb(s, on_preset_change, LV_EVENT_VALUE_CHANGED,
                            (void *)(intptr_t)i);
        y += 84;
    }
}

/* ====================== UI mode modal =======================
 * Lets the user flip between freetoon-lvgl and the stock Eneco qt-gui,
 * and toggle the 10-second boot picker. Switching to qt-gui requires a
 * confirmation tap because it triggers an exit-and-respawn — the user
 * sees their UI vanish immediately.
 *
 * The mode is persisted in /mnt/data/ui_choice (consumed by
 * ui_launcher.sh on the next boot); the picker toggle lives in
 * toonui.cfg via settings.boot_picker_enabled. */
#define UI_CHOICE_PATH "/mnt/data/ui_choice"

static lv_obj_t * lbl_uimode_current;
static lv_obj_t * sw_uimode_picker;
static lv_obj_t * confirm_uimode = NULL;

static int read_ui_choice(void) {
    FILE * f = fopen(UI_CHOICE_PATH, "r");
    if (!f) return 0;   /* default: freetoon */
    char buf[32] = {0};
    if (fgets(buf, sizeof(buf), f)) {
        char * nl = strchr(buf, '\n'); if (nl) *nl = 0;
    }
    fclose(f);
    if (strcmp(buf, "qt-gui") == 0 || strcmp(buf, "qtgui") == 0 || strcmp(buf, "stock") == 0)
        return 1;
    return 0;
}

static void write_ui_choice(int qtgui) {
    FILE * f = fopen(UI_CHOICE_PATH, "w");
    if (!f) return;
    fprintf(f, "%s\n", qtgui ? "qt-gui" : "freetoon");
    fclose(f);
}

static void on_uimode_picker(lv_event_t * e) {
    settings.boot_picker_enabled =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}

static void uimode_confirm_dismiss(void) {
    if (confirm_uimode) { lv_obj_del_async(confirm_uimode); confirm_uimode = NULL; }
}

/* qt-gui debug logging: a marker file on the data partition (so it survives
 * freetoon updates) whose contents are the log path ui_launcher.sh redirects
 * stock-UI output to. Present = on, absent = off. Set from the switch in the
 * "Switch to qt-gui" dialog and applied when the user confirms. */
#define QTLOG_PATH   "/mnt/data/qtlog"
#define QTLOG_TARGET "/var/log/qt"
static int  g_qtgui_debug = 0;   /* dialog switch state */
static int qtlog_enabled(void) { FILE * f = fopen(QTLOG_PATH, "r"); if (f) { fclose(f); return 1; } return 0; }
static void qtlog_apply(int on) {
    if (on) { FILE * f = fopen(QTLOG_PATH, "w"); if (f) { fprintf(f, "%s\n", QTLOG_TARGET); fclose(f); } }
    else    { remove(QTLOG_PATH); }
}
static void on_qtgui_debug_toggle(lv_event_t * e) {
    g_qtgui_debug = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}

/* Yes-tap: flip /mnt/data/ui_choice to qt-gui, save settings, then kill
 * ourselves. ui_launcher.sh respawns toonui --bootpick, the picker shows
 * qt-gui as the new default, 10 s later (or immediately if picker is
 * off) the launcher exec's /qmf/sbin/qt-gui. */
static void on_uimode_confirm_yes(lv_event_t * e) {
    (void)e;
    qtlog_apply(g_qtgui_debug);          /* enable/disable stock-UI logging for this boot */
    write_ui_choice(1);

    /* Disable quby_bridge in /etc/inittab and kill/unmount so stock qt-gui
     * has exclusive ownership of the hardware keteladapter and UART /dev/ttymxc0 */
    if (access("/mnt/data/ot_mode_switch.sh", X_OK) == 0) {
        system("/mnt/data/ot_mode_switch.sh off >> /tmp/ot_mode_switch.log 2>&1");
    } else {
        system("grep -v '^qbri:' /etc/inittab > /etc/inittab.new 2>/dev/null && mv -f /etc/inittab.new /etc/inittab && kill -HUP 1 2>/dev/null || true");
        system("pkill -9 -x quby_bridge 2>/dev/null || true");
        system("umount -l /dev/ttymxc0 2>/dev/null || true");
    }
    snprintf(settings.ot_bridge_mode, sizeof(settings.ot_bridge_mode), "off");

    settings_save();
    uimode_confirm_dismiss();
    /* Give the LVGL frame a beat to flush the dismissal animation, then
     * bail. Init respawns us via the launcher within ~1 s. */
    fprintf(stderr, "[uimode] switching to qt-gui (quby_bridge disabled in inittab) — exiting\n");
    fflush(NULL);
    _exit(0);
}

static void on_uimode_confirm_no(lv_event_t * e) {
    (void)e;
    uimode_confirm_dismiss();
}

static void on_uimode_to_qtgui(lv_event_t * e) {
    (void)e;
    /* Layered modal — same shape as the boiler-type confirm dialog so the
     * UX matches. Red border + warning tone since flipping out of
     * freetoon mid-session is disruptive. */
    confirm_uimode = lv_obj_create(cur_modal);
    lv_obj_remove_style_all(confirm_uimode);
    lv_obj_set_size(confirm_uimode, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(confirm_uimode, 0, 0);
    lv_obj_set_style_bg_color(confirm_uimode, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(confirm_uimode, LV_OPA_70, 0);
    lv_obj_clear_flag(confirm_uimode, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(confirm_uimode, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * dlg = lv_obj_create(confirm_uimode);
    lv_obj_set_size(dlg, SX(760), SY(380));
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x2a1c1c), 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0xcc5544), 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 16, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_clear_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dlg, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * dt = lv_label_create(dlg);
    lv_obj_set_style_text_font(dt, SF(28), 0);
    lv_obj_set_style_text_color(dt, lv_color_hex(0xffcc66), 0);
    lv_label_set_text(dt, tr("Overschakelen naar standaard qt-gui?", "Switch to stock qt-gui?"));
    lv_obj_align(dt, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t * body = lv_label_create(dlg);
    lv_obj_set_style_text_font(body, SF(18), 0);
    lv_obj_set_style_text_color(body, lv_color_hex(0xddddee), 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, SX(712));
    lv_label_set_text(body,
        tr("De freetoon-UI sluit af en de originele Eneco-UI neemt binnen "
           "ongeveer een seconde over. Onze hulpbruggen (p1bridge, "
           "quby_bridge) gaan inactief zodat de standaard meteradapter / "
           "keteladapter de leiding houden. Om terug te komen, tik op de "
           "freetoon-knop in de bootkiezer (of houd rechtsboven 10 s "
           "ingedrukt tijdens het opstarten van qt-gui).",
           "The freetoon UI will exit and the original Eneco UI takes "
           "over within about a second. Our companion bridges (p1bridge, "
           "quby_bridge) go idle so stock meteradapter / keteladapter "
           "remain in charge. To come back, tap the freetoon button in "
           "the boot picker (or hold the touch top-right for 10 s during "
           "the qt-gui boot)."));
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, SY(56));

    /* Developer aid: capture stock qt-gui output to /var/log/qt for this boot.
     * Persists via /mnt/data/qtlog (read by ui_launcher.sh), so it isn't wiped
     * by a freetoon update. Off by default. */
    g_qtgui_debug = qtlog_enabled();
    lv_obj_t * dbg_l = lv_label_create(dlg);
    lv_obj_set_style_text_font(dbg_l, SF(18), 0);
    lv_obj_set_style_text_color(dbg_l, lv_color_hex(0xffcc66), 0);
    lv_label_set_text(dbg_l, tr("qt-gui debug-logging (/var/log/qt)",
                                "qt-gui debug logging (/var/log/qt)"));
    lv_obj_align(dbg_l, LV_ALIGN_BOTTOM_LEFT, 0, SY(-84));
    lv_obj_t * dbg_sw = lv_switch_create(dlg);
    lv_obj_set_size(dbg_sw, SX(84), SY(42));
    lv_obj_align(dbg_sw, LV_ALIGN_BOTTOM_RIGHT, 0, SY(-80));
    if (g_qtgui_debug) lv_obj_add_state(dbg_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(dbg_sw, on_qtgui_debug_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * b_no = lv_btn_create(dlg);
    lv_obj_set_size(b_no, SX(220), SY(64));
    lv_obj_align(b_no, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(b_no, lv_color_hex(0x44556a), 0);
    lv_obj_set_style_radius(b_no, 12, 0);
    lv_obj_add_event_cb(b_no, on_uimode_confirm_no, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_no_l = lv_label_create(b_no);
    lv_label_set_text(b_no_l, tr("Annuleren", "Cancel"));
    lv_obj_set_style_text_font(b_no_l, SF(22), 0);
    lv_obj_center(b_no_l);

    lv_obj_t * b_yes = lv_btn_create(dlg);
    lv_obj_set_size(b_yes, SX(300), SY(64));
    lv_obj_align(b_yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(b_yes, lv_color_hex(0xcc5544), 0);
    lv_obj_set_style_radius(b_yes, 12, 0);
    lv_obj_add_event_cb(b_yes, on_uimode_confirm_yes, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_yes_l = lv_label_create(b_yes);
    lv_label_set_text(b_yes_l, tr("Naar qt-gui", "Switch to qt-gui"));
    lv_obj_set_style_text_font(b_yes_l, SF(22), 0);
    lv_obj_center(b_yes_l);
}

/* Marketplace tile-tap: push the marketplace browser screen onto the
 * navigation stack. The browser does its own HTTP fetch in a background
 * thread, so this returns immediately. */
static void open_marketplace(lv_event_t * e) {
    (void)e;
    ui_push(screen_marketplace_create());
}

/* Crypto tile-tap: live-search picker for the crypto integration's coins. */
static void open_crypto_picker(lv_event_t * e) {
    (void)e;
    ui_push(screen_crypto_picker_create());
}

static void open_templogger(lv_event_t * e) {
    (void)e;
    ui_push(screen_chart_24h_create(tile_slots_integration_by_id("temperaturelogger")));
}

/* Programma tile-tap: push the weekly thermostat schedule screen directly. */
static void open_program_screen(lv_event_t * e) {
    (void)e;
    ui_push(screen_schedule_create());
}

/* Z-Wave tile-tap: push the built-in Z-Wave controller management screen. */
static void open_zwave(lv_event_t * e) {
    (void)e;
    ui_push(screen_zwave_create());
}

/* Adapters tile-tap: push the meter/boiler adapter status + diagnostics screen. */
/* Restart the freetoon UI from within the UI. _exit(0) → ui_launcher.sh
 * respawns toonui (re-reading toonui.cfg), so this also applies any settings
 * edits without an SSH round-trip. Confirm-gated to avoid an accidental tap. */
static lv_obj_t * g_restart_modal = NULL;
static void restart_dismiss(void) { if (g_restart_modal) { lv_obj_del(g_restart_modal); g_restart_modal = NULL; } }
static void on_restart_no(lv_event_t * e)  { (void)e; restart_dismiss(); }
static void on_restart_yes(lv_event_t * e) {
    (void)e;
    settings_save();
    fprintf(stderr, "[settings] user requested UI restart — exiting\n");
    ui_request_restart();
}
static void open_restart_confirm(lv_event_t * e) {
    (void)e;
    /* Parent to the TOP LAYER, not scr_root: scr_root is vertically
     * scrollable, so a modal placed there at (0,0) renders at the top of the
     * scrolled content — off-screen when the user has scrolled down to (e.g.)
     * the news section, which looked like "Apply did nothing". The top layer
     * is viewport-fixed and always covers the screen. */
    g_restart_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_restart_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_restart_modal, 0, 0);
    lv_obj_set_style_bg_color(g_restart_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_restart_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(g_restart_modal, 0, 0);
    lv_obj_clear_flag(g_restart_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * card = lv_obj_create(g_restart_modal);
    lv_obj_set_size(card, SX(620), SY(300));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x16243a), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * t = lv_label_create(card);
    lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(t, SF(22), 0);
    lv_obj_set_width(t, SX(560));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_label_set_text(t, tr("freetoon-UI nu herstarten?\n\nDe instellingen worden opnieuw geladen uit toonui.cfg en komt binnen enkele seconden terug.", "Restart the freetoon UI now?\n\nIt reloads settings from toonui.cfg and comes back in a few seconds."));
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, SX(14), SY(14));

    lv_obj_t * yes = lv_btn_create(card);
    lv_obj_set_size(yes, SX(180), SY(52));
    lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, SX(-14), SY(-14));
    lv_obj_set_style_bg_color(yes, lv_color_hex(0x2e6e3a), 0);
    lv_obj_add_event_cb(yes, on_restart_yes, LV_EVENT_CLICKED, NULL);
    lv_obj_t * yl = lv_label_create(yes); lv_label_set_text(yl, tr("Herstart", "Restart")); lv_obj_center(yl);

    lv_obj_t * no = lv_btn_create(card);
    lv_obj_set_size(no, SX(180), SY(52));
    lv_obj_align(no, LV_ALIGN_BOTTOM_LEFT, SX(14), SY(-14));
    lv_obj_set_style_bg_color(no, lv_color_hex(0x3a4658), 0);
    lv_obj_add_event_cb(no, on_restart_no, LV_EVENT_CLICKED, NULL);
    lv_obj_t * nl = lv_label_create(no); lv_label_set_text(nl, tr("Annuleren", "Cancel")); lv_obj_center(nl);
}

static void open_adapters(lv_event_t * e) {
    (void)e;
    ui_push(screen_adapters_create());
}

/* On-device Domoticz config — host/user/enable can now be set on the Toon
 * itself (previously PWA-only). "View lights/blinds" opens the device list. */
static lv_obj_t * ta_domoticz_host = NULL;
static lv_obj_t * ta_domoticz_user = NULL;
static lv_obj_t * lbl_domoticz_result = NULL;

static void on_domoticz_enabled_change(lv_event_t * e) {
    settings.enable_domoticz =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
}
static void on_domoticz_view_click(lv_event_t * e) {
    (void)e;
    ui_push(screen_domoticz_create());
}
static void on_domoticz_apply_click(lv_event_t * e) {
    (void)e;
    const char * h = lv_textarea_get_text(ta_domoticz_host);
    const char * u = lv_textarea_get_text(ta_domoticz_user);
    snprintf(settings.domoticz_host, sizeof(settings.domoticz_host), "%s", h ? h : "");
    snprintf(settings.domoticz_user, sizeof(settings.domoticz_user), "%s", u ? u : "");
    if (settings.domoticz_host[0]) settings.enable_domoticz = 1;
    settings_save();
    extern int domoticz_start(void);
    domoticz_start();    /* (re)connect with the new host */
    if (!lbl_domoticz_result) return;
    if (!settings.domoticz_host[0]) { lv_label_set_text(lbl_domoticz_result, tr("Vul eerst de host in (ip:poort).", "Enter the host first (ip:port).")); return; }

    /* Real connection test via domoticz_probe(): it runs the exact auth ladder
     * the live client uses — DMZSID session login (modern / "Login Page" mode)
     * with an HTTP Basic fallback ("Basic Authentication" mode) — so the result
     * reflects what the tile will actually do, instead of a Basic-only GET that
     * falsely fails against a session-mode Domoticz. */
    lv_label_set_text(lbl_domoticz_result, tr("Bezig met testen...", "Testing..."));
    lv_refr_now(NULL);
    extern int domoticz_probe(void);
    int rc = domoticz_probe();
    if (rc >= 1) {
        lv_label_set_text_fmt(lbl_domoticz_result, tr("Verbonden - %d apparaten", "Connected - %d devices"), rc);
    } else if (rc == 0) {
        lv_label_set_text(lbl_domoticz_result, tr("Verbonden - geen licht/blind-apparaten gevonden.", "Connected - no light/blind devices found."));
    } else if (rc == DZ_PROBE_AUTH) {
        lv_label_set_text(lbl_domoticz_result, tr("Fout: inloggen vereist (gebruiker/wachtwoord).", "Error: login required (user/password)."));
    } else {
        lv_label_set_text(lbl_domoticz_result, tr("Fout: geen verbinding - controleer host/poort.", "Error: no connection - check host/port."));
    }
    lv_obj_scroll_to_view(lbl_domoticz_result, LV_ANIM_ON);
}

static void open_domoticz(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open("Domoticz", 540);
    int is_t1 = (DISP_VER < 600);
    int y = is_t1 ? 60 : 70;

    lv_obj_t * r_en = panel_row(p, y, tr("Domoticz inschakelen", "Domoticz enabled"), NULL);
    row_switch(r_en, settings.enable_domoticz, on_domoticz_enabled_change);
    y += is_t1 ? 72 : 88;

    lv_obj_t * lh = lv_label_create(p);
    lv_obj_set_style_text_color(lh, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lh, SF(22), 0);
    lv_label_set_text(lh, tr("Host (ip:poort):", "Host (ip:port):"));
    lv_obj_align(lh, LV_ALIGN_TOP_LEFT, SX(4), y + (is_t1 ? 2 : 4));
    ta_domoticz_host = lv_textarea_create(p); ta_make_numeric(ta_domoticz_host);
    lv_obj_set_size(ta_domoticz_host, SX(420), SY(44));
    lv_obj_align(ta_domoticz_host, LV_ALIGN_TOP_LEFT, SX(260), y);
    lv_textarea_set_one_line(ta_domoticz_host, true);
    lv_textarea_set_text(ta_domoticz_host, settings.domoticz_host);
    y += is_t1 ? 48 : 58;

    lv_obj_t * lu = lv_label_create(p);
    lv_obj_set_style_text_color(lu, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lu, SF(22), 0);
    lv_label_set_text(lu, tr("Gebruiker (opt):", "User (opt):"));
    lv_obj_align(lu, LV_ALIGN_TOP_LEFT, SX(4), y + (is_t1 ? 2 : 4));
    ta_domoticz_user = lv_textarea_create(p);
    lv_obj_set_size(ta_domoticz_user, SX(420), SY(44));
    lv_obj_align(ta_domoticz_user, LV_ALIGN_TOP_LEFT, SX(260), y);
    lv_textarea_set_one_line(ta_domoticz_user, true);
    lv_textarea_set_text(ta_domoticz_user, settings.domoticz_user);
    y += is_t1 ? 52 : 62;

    lv_obj_t * b_apply = lv_btn_create(p);
    lv_obj_set_size(b_apply, SX(220), SY(48));
    lv_obj_align(b_apply, LV_ALIGN_TOP_LEFT, SX(4), y);
    lv_obj_set_style_bg_color(b_apply, lv_color_hex(0xc06030), 0);
    lv_obj_add_event_cb(b_apply, on_domoticz_apply_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * la = lv_label_create(b_apply); lv_label_set_text(la, tr("Opslaan + verbinden", "Save + connect")); lv_obj_center(la);

    lv_obj_t * b_view = lv_btn_create(p);
    lv_obj_set_size(b_view, SX(240), SY(48));
    lv_obj_align(b_view, LV_ALIGN_TOP_LEFT, SX(244), y);
    lv_obj_set_style_bg_color(b_view, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(b_view, on_domoticz_view_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lv = lv_label_create(b_view); lv_label_set_text(lv, tr("Licht/zonwering", "View lights/blinds")); lv_obj_center(lv);
    y += is_t1 ? 52 : 60;

    lbl_domoticz_result = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_domoticz_result, lv_color_hex(0xa8c4dc), 0);
    lv_obj_set_style_text_font(lbl_domoticz_result, SF(18), 0);
    lv_obj_set_width(lbl_domoticz_result, SX(770));
    lv_label_set_long_mode(lbl_domoticz_result, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_domoticz_result, LV_ALIGN_TOP_LEFT, SX(4), y);
    lv_label_set_text(lbl_domoticz_result, tr("Stel de Domoticz-host in en kies Opslaan + verbinden.", "Set the Domoticz host, then Save + connect."));
}

/* Client mode: this Toon mirrors a master Toon over its PWA API. */
static void on_client_apply(lv_event_t * e) {
    (void)e;
    if (sw_client) settings.client_mode = lv_obj_has_state(sw_client, LV_STATE_CHECKED) ? 1 : 0;
    if (ta_master) {
        const char * h = lv_textarea_get_text(ta_master);
        snprintf(settings.master_host, sizeof settings.master_host, "%s", h ? h : "");
    }
    settings_save();
    open_restart_confirm(NULL);   /* takes effect after a UI restart */
}
static void open_client_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open(tr("Clientmodus", "Client mode"), 470);
    int y = 70;

    lv_obj_t * r = panel_row(p, y, tr("Clientmodus (een master-Toon spiegelen)", "Client mode (mirror a master Toon)"), NULL);
    sw_client = row_switch(r, settings.client_mode, NULL);
    y += 90;

    lv_obj_t * lbl = lv_label_create(p);
    lv_obj_set_style_text_color(lbl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl, SF(22), 0);
    lv_label_set_text(lbl, tr("Master-IP:", "Master IP:"));
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX(4), y);
    ta_master = lv_textarea_create(p); ta_make_numeric(ta_master);
    lv_obj_set_size(ta_master, SX(400), SY(44));
    lv_obj_align(ta_master, LV_ALIGN_TOP_LEFT, SX(200), y - 4);
    lv_textarea_set_one_line(ta_master, true);
    lv_textarea_set_text(ta_master, settings.master_host);
    y += 64;

    lv_obj_t * hint = lv_label_create(p);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(hint, SF(18), 0);
    lv_label_set_text(hint,
        tr("Deze Toon wordt een alleen-weergave-slave: hij toont de thermostaat/\n"
           "ketel/lucht/gordijnen van de master en stuurt bediening terug, zonder\n"
           "lokale sensoren of integraties. Voer het IP van de master-Toon in\n"
           "(een Android-tablet kan in plaats daarvan http://<master>:10081 openen).",
           "This Toon becomes a display-only slave: it shows the master's\n"
           "thermostat/boiler/air/curtains and sends control back to it, with\n"
           "no local sensors or integrations. Enter the master Toon's IP\n"
           "(an Android tablet can instead just open http://<master>:10081 )."));
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, SX(4), y);
    y += 110;

    lv_obj_t * btn = lv_btn_create(p);
    lv_obj_set_size(btn, (DISP_VER < 600 ? 230 : SX(240)), SY(50));
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, SX(4), y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a6090), 0);
    lv_obj_add_event_cb(btn, on_client_apply, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(btn);
    lv_label_set_text(bl, tr("Toepassen & herstarten", "Apply & restart"));
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_center(bl);
}

/* Auto-rotate: page-2 slot 1 cycles through the checked integrations. */
static int rotate_member_is_checked(const char * id) {
    char wrap[300];
    snprintf(wrap, sizeof wrap, ",%s,", settings.tile_rotate_members);
    char needle[52];
    snprintf(needle, sizeof needle, ",%s,", id);
    return strstr(wrap, needle) != NULL;
}
static void on_rotate_apply(lv_event_t * e) {
    (void)e;
    if (sw_rotate) settings.tile_rotate_enabled = lv_obj_has_state(sw_rotate, LV_STATE_CHECKED) ? 1 : 0;
    if (ta_rotate_secs) {
        int v = atoi(lv_textarea_get_text(ta_rotate_secs));
        settings.tile_rotate_seconds = v < 3 ? 3 : (v > 120 ? 120 : v);
    }
    char buf[256] = ""; int first = 1;
    for (int i = 0; i < rotate_cb_count; i++) {
        if (rotate_cb[i] && lv_obj_has_state(rotate_cb[i], LV_STATE_CHECKED)) {
            if (!first) strncat(buf, ",", sizeof buf - strlen(buf) - 1);
            strncat(buf, rotate_cb_id[i], sizeof buf - strlen(buf) - 1);
            first = 0;
        }
    }
    snprintf(settings.tile_rotate_members, sizeof settings.tile_rotate_members, "%s", buf);
    settings_save();
}
static lv_obj_t * marketplace_button(lv_obj_t * parent);   /* defined below */

/* Add one rotate-member checkbox (id + label) to the scroll container. */
static void rotate_add_cb(lv_obj_t * sc, const char * id, const char * label, int * cy) {
    if (rotate_cb_count >= 16) return;
    lv_obj_t * cb = lv_checkbox_create(sc);
    lv_checkbox_set_text(cb, label);
    lv_obj_set_style_text_font(cb, SF(22), 0);
    lv_obj_set_style_text_color(cb, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_align(cb, LV_ALIGN_TOP_LEFT, SX(6), *cy);
    if (rotate_member_is_checked(id)) lv_obj_add_state(cb, LV_STATE_CHECKED);
    rotate_cb[rotate_cb_count] = cb;
    snprintf(rotate_cb_id[rotate_cb_count], 48, "%s", id);
    rotate_cb_count++;
    *cy += (DISP_VER < 600) ? 44 : 50;
}

static void open_rotate_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open(tr("Tegel automatisch wisselen", "Auto-rotate tile"), 540);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_bottom(p, 0, 0);

    int is_t1 = (DISP_VER < 600);
    int y = is_t1 ? 60 : 70;

    lv_obj_t * r = panel_row(p, y, tr("Inhoud pagina-2 slot 1 wisselen", "Rotate page-2 slot 1 content"), NULL);
    sw_rotate = row_switch(r, settings.tile_rotate_enabled, NULL);
    y += is_t1 ? 70 : 86;

    lv_obj_t * lbl = lv_label_create(p);
    lv_obj_set_style_text_color(lbl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl, SF(22), 0);
    lv_label_set_text(lbl, tr("Interval (s):", "Interval (s):"));
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX(4), y);
    ta_rotate_secs = lv_textarea_create(p);
    ta_make_numeric(ta_rotate_secs);
    lv_obj_set_size(ta_rotate_secs, SX(160), SY(44));
    lv_obj_align(ta_rotate_secs, LV_ALIGN_TOP_LEFT, SX(200), y - 4);
    lv_textarea_set_one_line(ta_rotate_secs, true);
    lv_textarea_set_accepted_chars(ta_rotate_secs, "0123456789");
    char sbuf[8]; snprintf(sbuf, sizeof sbuf, "%d", settings.tile_rotate_seconds);
    lv_textarea_set_text(ta_rotate_secs, sbuf);
    y += is_t1 ? 48 : 58;

    rotate_cb_count = 0;

    lv_obj_t * pick = lv_label_create(p);
    lv_obj_set_style_text_color(pick, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(pick, SF(18), 0);
    lv_label_set_text(pick, tr("Wissel tussen:", "Cycle through:"));
    lv_obj_align(pick, LV_ALIGN_TOP_LEFT, SX(4), y);
    y += is_t1 ? 28 : 34;

    int h = SY(540);
    int sc_bottom = h - (is_t1 ? 78 : 98);
    int sc_h = sc_bottom - y;
    if (sc_h < 60) sc_h = 60;

    lv_obj_t * sc = lv_obj_create(p);
    lv_obj_set_size(sc, SX(820), sc_h);
    lv_obj_align(sc, LV_ALIGN_TOP_LEFT, SX(4), y);
    lv_obj_set_style_bg_opa(sc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sc, 0, 0);
    lv_obj_set_style_clip_corner(sc, true, 0);
    lv_obj_set_style_pad_all(sc, 0, 0);
    lv_obj_set_style_pad_left(sc, SX(4), 0);
    lv_obj_set_style_pad_top(sc, 4, 0);
    lv_obj_set_style_pad_bottom(sc, 4, 0);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_AUTO);
    int cy = 0;

    /* Built-in local integrations that are switched on. */
    int nl = tile_slots_local_count();
    for (int i = 0; i < nl; i++)
        if (tile_slots_local_enabled(i))
            rotate_add_cb(sc, tile_slots_local_id(i), tile_slots_local_label(i), &cy);

    /* Plus any installed marketplace integrations. */
    int ni = tile_slots_integration_count();
    for (int i = 0; i < ni; i++) {
        const integration_meta_t * im = tile_slots_integration_at(i);
        if (im) rotate_add_cb(sc, im->id, tile_slots_integration_title(im), &cy);
    }

    if (rotate_cb_count == 0) {
        lv_obj_t * m = lv_label_create(sc);
        lv_obj_set_style_text_color(m, lv_color_hex(0x88aabb), 0);
        lv_obj_set_style_text_font(m, SF(18), 0);
        lv_obj_set_width(m, SX(780));
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
        lv_label_set_text(m, tr("Geen integraties aan. Zet ze aan bij Integraties, of installeer er een uit de Marketplace.", "No integrations on. Enable them under Integrations, or install one from the Marketplace."));
    }

    /* Remark pointing at the marketplace for more, placed above bottom buttons */
    lv_obj_t * note = lv_label_create(p);
    lv_obj_set_style_text_color(note, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(note, SF(18), 0);
    lv_obj_set_width(note, SX(800));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(note, tr("Meer integraties vind je in de Marketplace.", "Find more integrations in the Marketplace."));
    lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, SX(16), SY(-66));

    lv_obj_t * btn = lv_btn_create(p);
    lv_obj_set_size(btn, SX(160), SY(50));
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, SX(16), SY(-8));
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a6090), 0);
    lv_obj_add_event_cb(btn, on_rotate_apply, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(btn); lv_label_set_text(bl, tr("Toepassen", "Apply"));
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0); lv_obj_center(bl);

    marketplace_button(p);   /* bottom-right → opens the Marketplace screen */
}

/* WiFi tile-tap: push the WiFi scan/connect/status screen. */
static void open_wifi(lv_event_t * e) {
    (void)e;
    ui_push(screen_wifi_create());
}

/* Newsreader: enable + RSS feed URL. */
static void news_feeds_rebuild(void);   /* defined below */
static void on_news_apply(lv_event_t * e) {
    (void)e;
    if (sw_news) settings.news_enabled = lv_obj_has_state(sw_news, LV_STATE_CHECKED) ? 1 : 0;
    if (ta_news_url) {
        const char * raw = lv_textarea_get_text(ta_news_url);
        if (raw && raw[0]) {
            while (*raw == ' ' || *raw == '\t' || *raw == '\r' || *raw == '\n') raw++;
            char u[300];
            size_t len = strlen(raw);
            while (len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\t' || raw[len - 1] == '\r' || raw[len - 1] == '\n'))
                len--;
            if (len >= sizeof(u)) len = sizeof(u) - 1;
            memcpy(u, raw, len);
            u[len] = 0;
            if (u[0]) {
                if (settings.news_rss_url[0])
                    strncat(settings.news_rss_url, "\n",
                            sizeof settings.news_rss_url - strlen(settings.news_rss_url) - 1);
                strncat(settings.news_rss_url, u,
                        sizeof settings.news_rss_url - strlen(settings.news_rss_url) - 1);
                lv_textarea_set_text(ta_news_url, "");
                news_feeds_rebuild();
            }
        }
    }
    settings_save();
    news_wake_fetch();
    if (lbl_news_status)
        lv_label_set_text(lbl_news_status, tr("Instellingen opgeslagen - feeds worden ververst", "Settings saved - refreshing feeds"));
}
static void on_news_test(lv_event_t * e) {
    (void)e;
    if (!lbl_news_status) return;
    const char * raw = ta_news_url ? lv_textarea_get_text(ta_news_url) : NULL;
    char target_url[300] = "";
    if (raw && raw[0]) {
        while (*raw == ' ' || *raw == '\t' || *raw == '\r' || *raw == '\n') raw++;
        size_t len = strlen(raw);
        while (len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\t' || raw[len - 1] == '\r' || raw[len - 1] == '\n'))
            len--;
        if (len >= sizeof(target_url)) len = sizeof(target_url) - 1;
        memcpy(target_url, raw, len);
        target_url[len] = 0;
    } else if (settings.news_rss_url[0]) {
        /* If input box is empty, test the first active feed from configured list */
        char tmp[sizeof settings.news_rss_url];
        snprintf(tmp, sizeof tmp, "%s", settings.news_rss_url);
        char * sv = NULL;
        for (char * l = strtok_r(tmp, "\n", &sv); l; l = strtok_r(NULL, "\n", &sv)) {
            while (*l == ' ' || *l == '\t' || *l == '\r') l++;
            if (!*l || *l == '#') continue;
            snprintf(target_url, sizeof target_url, "%s", l);
            break;
        }
    }
    if (!target_url[0]) {
        lv_label_set_text(lbl_news_status, tr("Voer eerst een RSS-feed URL in", "Enter an RSS feed URL first"));
        return;
    }
    lv_label_set_text(lbl_news_status, tr("Testen...", "Testing..."));
    char msg[160];
    news_test_feed(target_url, msg, sizeof msg);
    lv_label_set_text(lbl_news_status, msg);
}
static void on_news_open_click(lv_event_t * e) {
    modal_close(e);
    screen_home_open_news();
}
static void on_news_enabled_change(lv_event_t * e) {
    settings.news_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
    if (settings.news_enabled) news_wake_fetch();
}
static void on_news_feed_remove(lv_event_t * e);   /* fwd */
static void on_news_feed_toggle(lv_event_t * e);   /* fwd */
/* Rebuild the feed list from settings.news_rss_url (one URL per line). Each row:
   an eye icon (open = shown, closed = hidden) — tapping the row toggles the feed
   on/off (a '#'-prefixed line is hidden by the fetcher); plus a trash button on
   the right to delete the feed entirely. */
static void news_feeds_rebuild(void) {
    if (!news_feeds_list) return;
    lv_obj_clean(news_feeds_list);
    char tmp[sizeof settings.news_rss_url];
    snprintf(tmp, sizeof tmp, "%s", settings.news_rss_url);
    int i = 0; char * save = NULL;
    for (char * u = strtok_r(tmp, "\n", &save); u && i < 12; u = strtok_r(NULL, "\n", &save)) {
        while (*u == ' ' || *u == '\r') u++;
        if (!*u) continue;
        int disabled = (*u == '#');
        const char * shown = u + (disabled ? 1 : 0);
        while (*shown == ' ') shown++;
        lv_obj_t * b = lv_list_add_btn(news_feeds_list,
            disabled ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN, shown);
        lv_obj_set_style_text_font(b, SF(14), 0);
        if (disabled) lv_obj_set_style_text_color(b, lv_color_hex(0x66788a), 0);
        lv_obj_set_style_pad_right(b, SX(56), 0);          /* room for trash */
        lv_obj_add_event_cb(b, on_news_feed_toggle, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        /* trash button overlaid on the right edge (own click → remove) */
        lv_obj_t * tr = lv_btn_create(b);
        lv_obj_add_flag(tr, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_size(tr, SX(46), SY(34));
        lv_obj_align(tr, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(tr, lv_color_hex(0x5a2a2a), 0);
        lv_obj_set_style_radius(tr, 8, 0);
        lv_obj_add_event_cb(tr, on_news_feed_remove, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t * tl = lv_label_create(tr);
        lv_label_set_text(tl, LV_SYMBOL_TRASH);
        lv_obj_center(tl);
        i++;
    }
    if (i == 0) lv_list_add_text(news_feeds_list, tr("(nog geen feeds - voeg er een toe)", "(no feeds yet - add one)"));
}
/* Toggle the idx-th feed's '#'-disabled prefix. */
static void on_news_feed_toggle(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    char tmp[sizeof settings.news_rss_url], out[sizeof settings.news_rss_url] = "";
    snprintf(tmp, sizeof tmp, "%s", settings.news_rss_url);
    int i = 0; char * save = NULL;
    for (char * u = strtok_r(tmp, "\n", &save); u; u = strtok_r(NULL, "\n", &save)) {
        char line[300];
        if (i++ == idx) {
            char * s = u; while (*s == ' ') s++;
            if (*s == '#') { s++; while (*s == ' ') s++;
                             snprintf(line, sizeof line, "%s", s); }   /* enable */
            else             snprintf(line, sizeof line, "#%s", u);    /* disable */
        } else snprintf(line, sizeof line, "%s", u);
        if (out[0]) strncat(out, "\n", sizeof out - strlen(out) - 1);
        strncat(out, line, sizeof out - strlen(out) - 1);
    }
    snprintf(settings.news_rss_url, sizeof settings.news_rss_url, "%s", out);
    settings_save();
    news_wake_fetch();
    news_feeds_rebuild();
}
static void on_news_feed_remove(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    char tmp[sizeof settings.news_rss_url], out[sizeof settings.news_rss_url] = "";
    snprintf(tmp, sizeof tmp, "%s", settings.news_rss_url);
    int i = 0; char * save = NULL;
    for (char * u = strtok_r(tmp, "\n", &save); u; u = strtok_r(NULL, "\n", &save)) {
        if (i++ == idx) continue;
        if (out[0]) strncat(out, "\n", sizeof out - strlen(out) - 1);
        strncat(out, u, sizeof out - strlen(out) - 1);
    }
    snprintf(settings.news_rss_url, sizeof settings.news_rss_url, "%s", out);
    settings_save();
    news_wake_fetch();
    news_feeds_rebuild();
}
static void on_news_add_click(lv_event_t * e) {
    (void)e;
    const char * raw = ta_news_url ? lv_textarea_get_text(ta_news_url) : NULL;
    if (!raw || !raw[0]) return;
    while (*raw == ' ' || *raw == '\t' || *raw == '\r' || *raw == '\n') raw++;
    char u[300];
    size_t len = strlen(raw);
    while (len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\t' || raw[len - 1] == '\r' || raw[len - 1] == '\n'))
        len--;
    if (len >= sizeof(u)) len = sizeof(u) - 1;
    memcpy(u, raw, len);
    u[len] = 0;
    if (!u[0]) return;
    if (strncmp(u, "http://", 7) != 0 && strncmp(u, "https://", 8) != 0) {
        if (lbl_news_status)
            lv_label_set_text(lbl_news_status, tr("Ongeldige URL (moet beginnen met http:// of https://)", "Invalid URL (must start with http:// or https://)"));
        return;
    }
    if (settings.news_rss_url[0])
        strncat(settings.news_rss_url, "\n", sizeof settings.news_rss_url - strlen(settings.news_rss_url) - 1);
    strncat(settings.news_rss_url, u, sizeof settings.news_rss_url - strlen(settings.news_rss_url) - 1);
    settings_save();
    news_wake_fetch();
    if (ta_news_url) lv_textarea_set_text(ta_news_url, "");
    news_feeds_rebuild();
    if (lbl_news_status)
        lv_label_set_text(lbl_news_status, tr("Feed toegevoegd! Bezig met ophalen...", "Feed added! Fetching..."));
}
static void on_news_speed_change(lv_event_t * e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    settings.news_scroll_speed = v;
    if (lbl_news_speed) lv_label_set_text_fmt(lbl_news_speed, "%d", v);
    screen_home_set_ticker_speed(v);   /* live */
}

/* ---- Calendar (agenda): enable + HA entity + multi-iCal URLs + open-agenda. ---- */
static lv_obj_t * sw_cal = NULL;
static lv_obj_t * ta_cal_ha = NULL;
static lv_obj_t * ta_cal_ics = NULL;
static lv_obj_t * cal_feeds_list = NULL;
static lv_obj_t * lbl_cal_status = NULL;
static lv_obj_t * sw_cal_notify = NULL;
static lv_obj_t * ta_cal_lead = NULL;

static void cal_feeds_rebuild(void);

static void on_cal_enabled_change(lv_event_t * e) {
    settings.calendar_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static void on_cal_notify_change(lv_event_t * e) {
    settings.calendar_notify_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static void on_cal_open_click(lv_event_t * e) { (void)e; screen_calendar_show(); }
static void open_layout_editor(lv_event_t * e) { (void)e; screen_layout_editor_show(); }
static void open_dim_layout_editor(lv_event_t * e) { (void)e; screen_dim_layout_editor_show(); }

static void on_cal_save_click(lv_event_t * e) {
    (void)e;
    if (ta_cal_ha) { const char * v = lv_textarea_get_text(ta_cal_ha);
        snprintf(settings.calendar_ha_entity, sizeof settings.calendar_ha_entity, "%s", v ? v : ""); }
    if (ta_cal_lead) { const char * v = lv_textarea_get_text(ta_cal_lead);
        int m = v ? atoi(v) : 15; if (m < 1) m = 1; if (m > 1440) m = 1440;
        settings.calendar_notify_lead_min = m; }
    settings_save();
    calendar_refresh_async();          /* off-thread fetch so the UI stays responsive */
}

static void on_cal_feed_remove(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    char tmp[sizeof settings.calendar_ics_url], out[sizeof settings.calendar_ics_url] = "";
    snprintf(tmp, sizeof tmp, "%s", settings.calendar_ics_url);
    int i = 0; char * save = NULL;
    for (char * u = strtok_r(tmp, "\n\t\r", &save); u; u = strtok_r(NULL, "\n\t\r", &save)) {
        while (*u == ' ' || *u == '\t') u++;
        if (!*u) continue;
        if (i++ == idx) continue;
        if (out[0]) strncat(out, "\n", sizeof out - strlen(out) - 1);
        strncat(out, u, sizeof out - strlen(out) - 1);
    }
    snprintf(settings.calendar_ics_url, sizeof settings.calendar_ics_url, "%s", out);
    settings_save();
    calendar_refresh_async();
    cal_feeds_rebuild();
}

static void cal_feeds_rebuild(void) {
    if (!cal_feeds_list) return;
    lv_obj_clean(cal_feeds_list);
    char tmp[sizeof settings.calendar_ics_url];
    snprintf(tmp, sizeof tmp, "%s", settings.calendar_ics_url);
    int i = 0; char * save = NULL;
    for (char * u = strtok_r(tmp, "\n\t\r", &save); u; u = strtok_r(NULL, "\n\t\r", &save)) {
        while (*u == ' ' || *u == '\t') u++;
        if (!*u) continue;

        lv_obj_t * btn = lv_list_add_btn(cal_feeds_list, NULL, u);
        lv_obj_set_style_text_font(btn, SF(15), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(THEME_CARD_BG), 0);
        lv_obj_set_style_pad_left(btn, SX(12), 0);
        lv_obj_set_style_pad_right(btn, SX(56), 0);

        /* Delete button on the right */
        lv_obj_t * tr_btn = lv_btn_create(btn);
        lv_obj_add_flag(tr_btn, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_size(tr_btn, SX(46), SY(34));
        lv_obj_align(tr_btn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(tr_btn, lv_color_hex(0x882222), 0);
        lv_obj_set_style_radius(tr_btn, 8, 0);
        lv_obj_add_event_cb(tr_btn, on_cal_feed_remove, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t * tl = lv_label_create(tr_btn);
        lv_obj_set_style_text_color(tl, lv_color_hex(0xffffff), 0);
        lv_label_set_text(tl, LV_SYMBOL_TRASH);
        lv_obj_center(tl);

        /* Ensure child label text color */
        uint32_t cnt = lv_obj_get_child_cnt(btn);
        for (uint32_t c = 0; c < cnt; c++) {
            lv_obj_t * ch = lv_obj_get_child(btn, c);
            if (ch != tr_btn) {
                lv_obj_set_style_text_color(ch, lv_color_hex(THEME_TEXT_HI), 0);
                lv_obj_set_style_text_font(ch, SF(15), 0);
            }
        }
        i++;
    }
    if (i == 0) {
        lv_obj_t * txt = lv_list_add_text(cal_feeds_list, tr("(geen kalenders - voeg er een toe)", "(no calendars - add one)"));
        lv_obj_set_style_text_color(txt, lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(txt, SF(15), 0);
        lv_obj_set_style_bg_opa(txt, LV_OPA_TRANSP, 0);
    }
}

static void on_cal_add_click(lv_event_t * e) {
    (void)e;
    const char * raw = ta_cal_ics ? lv_textarea_get_text(ta_cal_ics) : NULL;
    if (!raw || !raw[0]) return;
    while (*raw == ' ' || *raw == '\t' || *raw == '\r' || *raw == '\n') raw++;
    char u[300];
    size_t len = strlen(raw);
    while (len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\t' || raw[len - 1] == '\r' || raw[len - 1] == '\n'))
        len--;
    if (len >= sizeof(u)) len = sizeof(u) - 1;
    memcpy(u, raw, len);
    u[len] = 0;
    if (!u[0]) return;
    if (strncmp(u, "http://", 7) != 0 && strncmp(u, "https://", 8) != 0) {
        if (lbl_cal_status)
            lv_label_set_text(lbl_cal_status, tr("Ongeldige URL (moet http:// of https:// zijn)", "Invalid URL (must start with http:// or https://)"));
        return;
    }
    if (settings.calendar_ics_url[0])
        strncat(settings.calendar_ics_url, "\n", sizeof settings.calendar_ics_url - strlen(settings.calendar_ics_url) - 1);
    strncat(settings.calendar_ics_url, u, sizeof settings.calendar_ics_url - strlen(settings.calendar_ics_url) - 1);
    settings_save();
    calendar_refresh_async();
    if (ta_cal_ics) lv_textarea_set_text(ta_cal_ics, "");
    cal_feeds_rebuild();
    if (lbl_cal_status)
        lv_label_set_text(lbl_cal_status, tr("Kalender toegevoegd! Bezig met laden...", "Calendar added! Fetching..."));
}

static void on_cal_import_click(lv_event_t * e) {
    (void)e;
    FILE * cf = fopen("/mnt/data/tsc/calendar.userSettings.json", "r");
    if (!cf) {
        if (lbl_cal_status) lv_label_set_text(lbl_cal_status, tr("Geen Toon-instellingen gevonden", "No Toon settings found"));
        return;
    }
    char cbuf[8192];
    size_t n = fread(cbuf, 1, sizeof(cbuf) - 1, cf);
    fclose(cf);
    cbuf[n] = 0;
    int added = 0;
    const char * cu = strstr(cbuf, "\"Calendar_URL\"");
    if (cu) {
        const char * p = strchr(cu, '[');
        if (p) {
            p++;
            while (*p && *p != ']') {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
                if (*p == '"') {
                    p++;
                    const char * q = strchr(p, '"');
                    if (!q) break;
                    size_t ulen = (size_t)(q - p);
                    char u[300];
                    if (ulen >= sizeof(u)) ulen = sizeof(u) - 1;
                    memcpy(u, p, ulen);
                    u[ulen] = 0;
                    if (strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0) {
                        if (!strstr(settings.calendar_ics_url, u)) {
                            if (settings.calendar_ics_url[0])
                                strncat(settings.calendar_ics_url, "\n", sizeof(settings.calendar_ics_url) - strlen(settings.calendar_ics_url) - 1);
                            strncat(settings.calendar_ics_url, u, sizeof(settings.calendar_ics_url) - strlen(settings.calendar_ics_url) - 1);
                            added++;
                        }
                    }
                    p = q + 1;
                } else {
                    p++;
                }
            }
        }
    }
    if (added > 0) {
        settings_save();
        calendar_refresh_async();
        cal_feeds_rebuild();
        if (lbl_cal_status) lv_label_set_text_fmt(lbl_cal_status, tr("%d kalenders geïmporteerd!", "%d calendars imported!"), added);
    } else {
        if (lbl_cal_status) lv_label_set_text(lbl_cal_status, tr("Alle kalenders al aanwezig", "All calendars already present"));
    }
}

static void open_calendar_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open(tr("Agenda", "Calendar"), 560);
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    int y = 70;

    lv_obj_t * r = panel_row(p, y, tr("Agenda inschakelen", "Enable calendar"), NULL);
    sw_cal = row_switch(r, settings.calendar_enabled, on_cal_enabled_change);
    y += 86;

    lv_obj_t * l1 = lv_label_create(p);
    lv_obj_set_style_text_color(l1, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(l1, SF(18), 0);
    lv_label_set_text(l1, tr("Home Assistant agenda-entiteit (optioneel):", "Home Assistant calendar entity (optional):"));
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, SX(4), SY(y)); y += 26;
    ta_cal_ha = lv_textarea_create(p);
    lv_obj_set_size(ta_cal_ha, SX(470), SY(44));
    lv_obj_align(ta_cal_ha, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    lv_textarea_set_one_line(ta_cal_ha, true);
    lv_textarea_set_placeholder_text(ta_cal_ha, "calendar.gezin");
    lv_textarea_set_text(ta_cal_ha, settings.calendar_ha_entity);
    y += 56;

    /* Multi-calendar ICS feeds list */
    lv_obj_t * l2 = lv_label_create(p);
    lv_obj_set_style_text_color(l2, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(l2, SF(18), 0);
    lv_label_set_text(l2, tr("iCal (.ics) kalenders:", "iCal (.ics) calendars:"));
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, SX(4), SY(y)); y += 26;

    cal_feeds_list = lv_list_create(p);
    lv_obj_set_size(cal_feeds_list, SX(740), SY(130));
    lv_obj_align(cal_feeds_list, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    lv_obj_set_style_bg_color(cal_feeds_list, lv_color_hex(THEME_ROW_BG), 0);
    lv_obj_set_style_border_color(cal_feeds_list, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(cal_feeds_list, 1, 0);
    lv_obj_set_style_radius(cal_feeds_list, 10, 0);
    cal_feeds_rebuild();
    y += 140;

    ta_cal_ics = lv_textarea_create(p);
    lv_obj_set_size(ta_cal_ics, SX(450), SY(44));
    lv_obj_align(ta_cal_ics, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    lv_textarea_set_one_line(ta_cal_ics, true);
    lv_textarea_set_placeholder_text(ta_cal_ics, "https://... .ics");

    lv_obj_t * addb = lv_btn_create(p);
    lv_obj_set_size(addb, SX(120), SY(44));
    lv_obj_align(addb, LV_ALIGN_TOP_LEFT, SX(464), SY(y));
    lv_obj_set_style_bg_color(addb, lv_color_hex(0x2e6e3a), 0);
    lv_obj_add_event_cb(addb, on_cal_add_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * al = lv_label_create(addb); lv_label_set_text(al, tr("Toevoegen", "Add")); lv_obj_center(al);

    lv_obj_t * impb = lv_btn_create(p);
    lv_obj_set_size(impb, SX(150), SY(44));
    lv_obj_align(impb, LV_ALIGN_TOP_LEFT, SX(594), SY(y));
    lv_obj_set_style_bg_color(impb, lv_color_hex(0x224466), 0);
    lv_obj_add_event_cb(impb, on_cal_import_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * il = lv_label_create(impb); lv_label_set_text(il, tr("Toon import", "Toon import")); lv_obj_center(il);
    y += 52;

    lbl_cal_status = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_cal_status, lv_color_hex(0x9fc4e6), 0);
    lv_obj_set_style_text_font(lbl_cal_status, SF(16), 0);
    lv_label_set_text(lbl_cal_status, "");
    lv_obj_align(lbl_cal_status, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    y += 28;

    /* Notifications */
    lv_obj_t * rn = panel_row(p, y, tr("Meldingen", "Notifications"), NULL);
    sw_cal_notify = row_switch(rn, settings.calendar_notify_enabled, on_cal_notify_change);
    y += 86;

    lv_obj_t * l3 = lv_label_create(p);
    lv_obj_set_style_text_color(l3, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(l3, SF(18), 0);
    lv_label_set_text(l3, tr("Minuten van tevoren:", "Minutes before:"));
    lv_obj_align(l3, LV_ALIGN_TOP_LEFT, SX(4), SY(y + 8));
    ta_cal_lead = lv_textarea_create(p);
    lv_obj_set_size(ta_cal_lead, SX(120), SY(44));
    lv_obj_align(ta_cal_lead, LV_ALIGN_TOP_LEFT, SX(300), SY(y));
    lv_textarea_set_one_line(ta_cal_lead, true);
    lv_textarea_set_accepted_chars(ta_cal_lead, "0123456789");
    lv_textarea_set_max_length(ta_cal_lead, 4);
    char leadbuf[8]; snprintf(leadbuf, sizeof leadbuf, "%d", settings.calendar_notify_lead_min);
    lv_textarea_set_text(ta_cal_lead, leadbuf);
    y += 60;

    lv_obj_t * saveb = lv_btn_create(p);
    lv_obj_set_size(saveb, SX(150), SY(46));
    lv_obj_align(saveb, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    lv_obj_set_style_bg_color(saveb, lv_color_hex(0x2e6e3a), 0);
    lv_obj_add_event_cb(saveb, on_cal_save_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sl = lv_label_create(saveb); lv_label_set_text(sl, tr("Opslaan", "Save")); lv_obj_center(sl);

    lv_obj_t * openb = lv_btn_create(p);
    lv_obj_set_size(openb, SX(180), SY(46));
    lv_obj_align(openb, LV_ALIGN_TOP_LEFT, SX(170), SY(y));
    lv_obj_set_style_bg_color(openb, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(openb, on_cal_open_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ol = lv_label_create(openb); lv_label_set_text(ol, tr("Agenda openen", "Open calendar")); lv_obj_center(ol);
}

/* ===================== Animation Overlay Settings ===================== */
static lv_obj_t * sw_anim_en = NULL;
static lv_obj_t * sw_anim_bday = NULL;
static lv_obj_t * sw_anim_seas = NULL;
static lv_obj_t * sw_anim_dim = NULL;
static lv_obj_t * lbl_anim_status = NULL;

static void on_anim_enabled_change(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_target(e);
    settings.anim_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (!settings.anim_enabled) anim_overlay_stop();
    settings_save();
}

static void on_anim_bday_change(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_target(e);
    settings.anim_birthday_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    settings_save();
}

static void on_anim_seas_change(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_target(e);
    settings.anim_seasonal_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    settings_save();
}

static void on_anim_dim_change(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_target(e);
    settings.anim_dim_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    settings_save();
}

static void on_anim_test_click(lv_event_t * e) {
    anim_effect_t eff = (anim_effect_t)(intptr_t)lv_event_get_user_data(e);
    if (eff == ANIM_EFFECT_NONE) {
        anim_overlay_stop();
        if (lbl_anim_status) lv_label_set_text(lbl_anim_status, tr("Animatie gestopt", "Animation stopped"));
    } else {
        anim_overlay_trigger(eff, 25);
        if (lbl_anim_status) {
            lv_label_set_text_fmt(lbl_anim_status, "%s: %s (25 s)",
                                  tr("Test actief", "Test active"), anim_effect_title(eff));
        }
    }
}

static void open_anim_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open(tr("Animaties & Effecten", "Animations & Effects"), 560);
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    int y = 70;

    lv_obj_t * r = panel_row(p, y, tr("Animaties inschakelen", "Enable animations"), NULL);
    sw_anim_en = row_switch(r, settings.anim_enabled, on_anim_enabled_change);
    y += 86;

    r = panel_row(p, y, tr("Verjaardagen vieren (Agenda)", "Celebrate birthdays (Calendar)"), NULL);
    sw_anim_bday = row_switch(r, settings.anim_birthday_enabled, on_anim_bday_change);
    y += 86;

    r = panel_row(p, y, tr("Seizoenseffecten (Sint, Vuurwerk, Sneeuw)", "Seasonal effects (Sint, Fireworks, Snow)"), NULL);
    sw_anim_seas = row_switch(r, settings.anim_seasonal_enabled, on_anim_seas_change);
    y += 86;

    r = panel_row(p, y, tr("Zichtbaar in dimstand", "Visible on dim screen"), NULL);
    sw_anim_dim = row_switch(r, settings.anim_dim_enabled, on_anim_dim_change);
    y += 86;

    /* Effect test row */
    lv_obj_t * l_test = lv_label_create(p);
    lv_obj_set_style_text_color(l_test, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(l_test, SF(18), 0);
    lv_label_set_text(l_test, tr("Effect testen:", "Test effect:"));
    lv_obj_align(l_test, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    y += 26;

    /* Grid of test buttons */
    struct { anim_effect_t eff; const char * label; uint32_t bg; } btns[] = {
        { ANIM_EFFECT_BALLOONS,  tr("Ballonnen", "Balloons"),  0xd32f2f },
        { ANIM_EFFECT_CONFETTI,  tr("Confetti", "Confetti"),   0x7b1fa2 },
        { ANIM_EFFECT_SNOW,      tr("Sneeuw", "Snow"),         0x1976d2 },
        { ANIM_EFFECT_FIREWORKS, tr("Vuurwerk", "Fireworks"),  0xf57c00 },
        { ANIM_EFFECT_LEAVES,    tr("Herfst", "Autumn"),       0x5d4037 },
        { ANIM_EFFECT_HEARTS,    tr("Hartjes", "Hearts"),      0xc2185b },
        { ANIM_EFFECT_NONE,      tr("Stop", "Stop"),           0x455a64 }
    };
    int n_btns = (int)(sizeof(btns) / sizeof(btns[0]));

    int btn_w = SX(112), btn_h = SY(42), gap = SX(8);
    for (int i = 0; i < n_btns; i++) {
        lv_obj_t * b = lv_btn_create(p);
        lv_obj_set_size(b, btn_w, btn_h);
        int bx = SX(4) + (i % 4) * (btn_w + gap);
        int by = SY(y) + (i / 4) * (btn_h + SY(8));
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, bx, by);
        lv_obj_set_style_bg_color(b, lv_color_hex(btns[i].bg), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_add_event_cb(b, on_anim_test_click, LV_EVENT_CLICKED, (void *)(intptr_t)btns[i].eff);
        lv_obj_t * bl = lv_label_create(b);
        lv_obj_set_style_text_font(bl, SF(15), 0);
        lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
        lv_label_set_text(bl, btns[i].label);
        lv_obj_center(bl);
    }
    y += 105;

    lbl_anim_status = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_anim_status, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_anim_status, SF(15), 0);
    lv_label_set_text(lbl_anim_status, "");
    lv_obj_align(lbl_anim_status, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
}

static void open_news_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open(tr("Nieuwslezer", "Newsreader"), 760);
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    int y = 70;

    lv_obj_t * r = panel_row(p, y, tr("Nieuwsticker op startscherm", "News ticker on home screen"), NULL);
    sw_news = row_switch(r, settings.news_enabled, on_news_enabled_change);
    y += 90;

    /* Open the reader directly — so the news is reachable even with the ticker
       turned off (the ticker tap is otherwise the only entry point). */
    lv_obj_t * onb = lv_btn_create(p);
    lv_obj_set_size(onb, SX(220), SY(46));
    lv_obj_align(onb, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    lv_obj_set_style_bg_color(onb, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(onb, on_news_open_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * onl = lv_label_create(onb); lv_label_set_text(onl, tr("Nieuws openen", "Open news")); lv_obj_center(onl);
    y += 64;

    /* RSS feeds — a list (tap a row to remove) + an Add box below it. */
    lv_obj_t * lbl = lv_label_create(p);
    lv_obj_set_style_text_color(lbl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl, SF(22), 0);
    lv_label_set_text(lbl, tr("RSS-feeds (tik om te verwijderen):", "RSS feeds (tap to remove):"));
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    y += 36;
    news_feeds_list = lv_list_create(p);
    lv_obj_set_size(news_feeds_list, SX(740), SY(150));
    lv_obj_align(news_feeds_list, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    lv_obj_set_style_bg_color(news_feeds_list, lv_color_hex(0x0e1a2a), 0);
    news_feeds_rebuild();
    y += 158;

    ta_news_url = lv_textarea_create(p);
    lv_obj_set_size(ta_news_url, SX(470), SY(44));
    lv_obj_align(ta_news_url, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    lv_textarea_set_one_line(ta_news_url, true);
    lv_textarea_set_placeholder_text(ta_news_url, tr("https://... RSS-feed", "https://... RSS feed"));
    lv_obj_t * addb = lv_btn_create(p);
    lv_obj_set_size(addb, SX(120), SY(44));
    lv_obj_align(addb, LV_ALIGN_TOP_LEFT, SX(484), SY(y));
    lv_obj_set_style_bg_color(addb, lv_color_hex(0x2e6e3a), 0);
    lv_obj_add_event_cb(addb, on_news_add_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * al = lv_label_create(addb); lv_label_set_text(al, tr("Toevoegen", "Add")); lv_obj_center(al);
    lv_obj_t * tbtn = lv_btn_create(p);
    lv_obj_set_size(tbtn, SX(120), SY(44));
    lv_obj_align(tbtn, LV_ALIGN_TOP_LEFT, SX(614), SY(y));
    lv_obj_set_style_bg_color(tbtn, lv_color_hex(0x33445a), 0);
    lv_obj_add_event_cb(tbtn, on_news_test, LV_EVENT_CLICKED, NULL);
    lv_obj_t * tl = lv_label_create(tbtn); lv_label_set_text(tl, tr("Test", "Test")); lv_obj_center(tl);
    lv_obj_set_style_text_color(tl, lv_color_hex(0xffffff), 0);
    y += 64;

    int spd = settings.news_scroll_speed >= 30 ? settings.news_scroll_speed : 30;
    lv_obj_t * rs = panel_row(p, y, tr("Tickersnelheid (px/s)", "Ticker speed (px/s)"), &lbl_news_speed);
    lv_label_set_text_fmt(lbl_news_speed, "%d", spd);
    sl_news_speed = row_slider(rs, 30, 150, spd, on_news_speed_change);
    y += 86;

    lbl_news_status = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_news_status, lv_color_hex(0x9fc4e6), 0);
    lv_obj_set_style_text_font(lbl_news_status, SF(18), 0);
    lv_obj_set_width(lbl_news_status, SX(800));
    lv_label_set_long_mode(lbl_news_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_news_status, "");
    lv_obj_align(lbl_news_status, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    y += 30;

    lv_obj_t * hint = lv_label_create(p);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(hint, SF(18), 0);
    lv_obj_set_width(hint, SX(800));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint,
        tr("Koppen scrollen boven de verwachting; tik erop voor een QR-code die "
           "het artikel op je telefoon opent. Elke RSS/Atom-feed werkt "
           "(standaard: NOS Nieuws).",
           "Headlines scroll above the forecast; tap them to get a QR code that "
           "opens the article on your phone. Any RSS/Atom feed works "
           "(default: NOS Nieuws)."));
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    y += 70;

    lv_obj_t * btn = lv_btn_create(p);
    lv_obj_set_size(btn, SX(240), SY(50));
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a6090), 0);
    lv_obj_add_event_cb(btn, on_news_apply, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(btn);
    lv_label_set_text(bl, tr("Toepassen & herstarten", "Apply & restart"));
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_center(bl);
    y += 60;
}

static void open_uimode_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open(tr("UI-modus", "UI mode"), 440);
    int y = 70;

    lv_obj_t * r;

    r = panel_row(p, y, tr("Huidige UI", "Current UI"), &lbl_uimode_current);
    lv_label_set_text(lbl_uimode_current,
        read_ui_choice() ? tr("standaard qt-gui", "stock qt-gui") : "freetoon-lvgl");
    y += 84;

    r = panel_row(p, y, tr("Bootkiezer (10 s bij opstarten)", "Boot picker (10 s at boot)"), NULL);
    sw_uimode_picker = row_switch(r, settings.boot_picker_enabled, on_uimode_picker);
    y += 84;

    /* Switch-to-qt-gui button. Lives below the rows because tapping it
     * fires a destructive action — keep it visually distinct from the
     * benign panel rows. */
    lv_obj_t * b_switch = lv_btn_create(p);
    lv_obj_set_size(b_switch, SX(800), SY(64));
    lv_obj_align(b_switch, LV_ALIGN_TOP_LEFT, SX(4), y);
    lv_obj_set_style_bg_color(b_switch, lv_color_hex(0x6a3a2a), 0);
    lv_obj_set_style_radius(b_switch, 12, 0);
    lv_obj_add_event_cb(b_switch, on_uimode_to_qtgui, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bsw_l = lv_label_create(b_switch);
    lv_label_set_text(bsw_l, tr("Naar standaard qt-gui...", "Switch to stock qt-gui..."));
    lv_obj_set_style_text_font(bsw_l, SF(22), 0);
    lv_obj_set_style_text_color(bsw_l, lv_color_hex(0xffffff), 0);
    lv_obj_center(bsw_l);
}

/* ==================== Integrations modal ====================
 * Runtime on/off switches for the optional integrations. Toggles persist
 * to /mnt/data/toonui.cfg via modal_close → settings_save, but the
 * pollers only re-check the flags on (re)start, so the modal warns the
 * user that flipping a switch needs a toonui restart to fully take
 * effect (killing the proc → inittab respawns it cleanly). */
static lv_obj_t * lbl_integ_hint;
static lv_obj_t * sw_int_p1_elec;
static lv_obj_t * sw_int_p1_water;
static lv_obj_t * sw_int_vent;
static lv_obj_t * sw_int_ha;
static lv_obj_t * sw_int_hide_offline;

static void integ_dirty_hint(void) {
    if (!lbl_integ_hint) return;
    lv_label_set_text(lbl_integ_hint,
        tr("Opgeslagen. Herstart toonui om toe te passen (Instellingen -> UI herstarten, "
           "of trek de stekker er even uit).",
           "Saved. Restart toonui to apply (Settings -> Restart UI, "
           "or just unplug+plug the screen)."));
    lv_obj_set_style_text_color(lbl_integ_hint, lv_color_hex(0xffcc66), 0);
}

static void on_int_p1_elec(lv_event_t * e) {
    settings.enable_p1_elec  = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    integ_dirty_hint();
}
static void on_int_p1_water(lv_event_t * e) {
    settings.enable_p1_water = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    integ_dirty_hint();
}
static void on_int_vent(lv_event_t * e) {
    settings.enable_vent     = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    integ_dirty_hint();
}
static void on_int_ha(lv_event_t * e) {
    settings.enable_ha       = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    integ_dirty_hint();
    settings_save();
}
/* Hide-offline-tiles toggle. Takes effect on the next refresh tick — no
 * restart needed because apply_offline_tile_visibility() reads the flag
 * fresh every time. Doesn't dirty the integ_hint (no restart required). */
static void on_int_hide_offline(lv_event_t * e) {
    settings.hide_offline_tiles =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
/* Energy source. Every poller runs regardless; the tiles just read the chosen
 * one through energy.c, so switching is live — but persist it. Changing to/from
 * the ESPHome reader needs a restart to start its poller (it only spawns when a
 * host is configured), hence the dirty hint. */
static lv_obj_t * dd_energy_src = NULL;
static void on_int_energy_src(lv_event_t * e) {
    settings.energy_source = (int)lv_dropdown_get_selected(lv_event_get_target(e));
    settings_save();
    integ_dirty_hint();
}
/* Unlike the HA modal, Integrations has no Save button to collect textareas on
 * the way out — it's otherwise all switches, which persist as they're flipped.
 * So commit on DEFOCUSED *and* on DELETE: closing the modal with the keyboard
 * still up tears the textarea down without ever defocusing it, and the value
 * would be lost. DELETE fires before the object's text is freed, and
 * modal_close()'s own settings_save() has already run by then (the delete is
 * async), so save again here. */
static void on_int_p1esp_host(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_DEFOCUSED && c != LV_EVENT_DELETE) return;
    const char * v = lv_textarea_get_text(lv_event_get_target(e));
    if (!v) return;
    if (strcmp(v, settings.p1esp_host) == 0) return;   /* unchanged — don't churn the flash */
    snprintf(settings.p1esp_host, sizeof settings.p1esp_host, "%s", v);
    settings_save();
    /* NOT on DELETE: the hint label is a sibling in the modal being torn down and
     * may already be freed. Only touch UI while the modal is still alive. */
    if (c == LV_EVENT_DEFOCUSED) integ_dirty_hint();
}

static void on_int_video(lv_event_t * e) {
    settings.video_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
}

static void open_ha_entities_modal(lv_event_t * e);

static void open_energy_stats(lv_event_t * e) {
    (void)e;
    modal_close(NULL);
    ui_push(screen_stats_create());
}

static void open_integrations_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open(tr("Integraties", "Integrations"), 710);
    int y = 70;

    lv_obj_t * r;
    r = panel_row(p, y, tr("Energiebron", "Energy source"), NULL);
    dd_energy_src = lv_dropdown_create(r);
    lv_dropdown_set_options(dd_energy_src,
        tr("Toon meteradapter\nHomeWizard P1\nZuidwijk / ESPHome P1\nHome Assistant",
           "Toon meteradapter\nHomeWizard P1\nZuidwijk / ESPHome P1\nHome Assistant"));
    lv_dropdown_set_selected(dd_energy_src, settings.energy_source);
    lv_obj_set_width(dd_energy_src, SX(300));
    lv_obj_align(dd_energy_src, LV_ALIGN_RIGHT_MID, SX(-12), 0);
    lv_obj_add_event_cb(dd_energy_src, on_int_energy_src, LV_EVENT_VALUE_CHANGED, NULL);
    y += 84;

    /* Zuidwijk SlimmeLezer (or any ESPHome `dsmr` reader) — needs ESPHome's
     * web_server enabled on port 80, which Zuidwijk's stock config ships with. */
    r = panel_row(p, y, tr("Zuidwijk/ESPHome P1 host", "Zuidwijk/ESPHome P1 host"), NULL);
    {
        lv_obj_t * ta = lv_textarea_create(r);
        lv_obj_set_size(ta, SX(300), SY(44));
        lv_obj_align(ta, LV_ALIGN_RIGHT_MID, SX(-12), 0);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_placeholder_text(ta, "192.168.1.20 / slimmelezer.local");
        lv_textarea_set_text(ta, settings.p1esp_host);
        lv_obj_add_event_cb(ta, on_int_p1esp_host, LV_EVENT_ALL, NULL);
    }
    y += 84;

    r = panel_row(p, y, tr("HomeWizard P1 (elektriciteit + gas)", "HomeWizard P1 (electricity + gas)"), NULL);
    sw_int_p1_elec = row_switch(r, settings.enable_p1_elec, on_int_p1_elec);
    y += 84;

    r = panel_row(p, y, tr("HomeWizard HWE-WTR (water)", "HomeWizard HWE-WTR (water)"), NULL);
    sw_int_p1_water = row_switch(r, settings.enable_p1_water, on_int_p1_water);
    y += 84;

    r = panel_row(p, y, tr("Itho-ventilatie", "Itho ventilation"), NULL);
    sw_int_vent = row_switch(r, settings.enable_vent, on_int_vent);
    y += 84;

    r = panel_row(p, y, tr("Home Assistant (gordijnen, Life360)", "Home Assistant (curtains, Life360)"), NULL);
    sw_int_ha = row_switch(r, settings.enable_ha, on_int_ha);
    y += 84;

    r = panel_row(p, y, tr("Videostream / Camera knop", "Video stream / Camera button"), NULL);
    row_switch(r, settings.video_enabled, on_int_video);
    y += 84;

    r = panel_row(p, y, tr("Offline tegels volledig verbergen", "Hide offline tiles entirely"), NULL);
    sw_int_hide_offline = row_switch(r, settings.hide_offline_tiles, on_int_hide_offline);
    y += 92;

    lbl_integ_hint = lv_label_create(p);
    lv_obj_set_style_text_font(lbl_integ_hint, SF(18), 0);
    lv_obj_set_style_text_color(lbl_integ_hint, lv_color_hex(0x88aabb), 0);
    lv_obj_set_width(lbl_integ_hint, SX(800));
    lv_label_set_long_mode(lbl_integ_hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_integ_hint,
        tr("Elke integratie heeft zijn bijbehorende config nodig "
           "(p1bridge.conf / vent.conf / ha.cfg) voordat de tegels oplichten. "
           "Zet een schakelaar om en herstart toonui.",
           "Each integration needs its companion config "
           "(p1bridge.conf / vent.conf / ha.cfg) before its tiles light up. "
           "Toggle a switch then restart toonui."));
    lv_obj_align(lbl_integ_hint, LV_ALIGN_TOP_LEFT, SX(4), y);

    /* Button to open the HA entity configuration modal. */
    lv_obj_t * b_ha = lv_btn_create(p);
    lv_obj_set_size(b_ha, SX(420), SY(50));
    lv_obj_align(b_ha, LV_ALIGN_TOP_LEFT, SX(4), y + 60);
    lv_obj_set_style_bg_color(b_ha, lv_color_hex(0x335577), 0);
    lv_obj_add_event_cb(b_ha, open_ha_entities_modal, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bhl = lv_label_create(b_ha);
    lv_obj_set_style_text_color(bhl, lv_color_hex(0xffffff), 0);
    lv_label_set_text(bhl, tr("HA-entiteiten instellen...", "Configure HA entities..."));
    lv_obj_center(bhl);
}

/* ============== HA entity configuration modal ============== */
static lv_obj_t * ta_ha_host            = NULL;
static lv_obj_t * ta_ha_token           = NULL;
static lv_obj_t * ta_curtain_entity     = NULL;
static lv_obj_t * ta_curtain_bat_a      = NULL;
static lv_obj_t * ta_curtain_bat_b      = NULL;
static lv_obj_t * ta_blinds_entity      = NULL;
static lv_obj_t * ta_blinds_bat_a       = NULL;
static lv_obj_t * ta_blinds_bat_b       = NULL;
static lv_obj_t * ta_doorbell_entity    = NULL;
static lv_obj_t * ta_doorbell_camera    = NULL;
static lv_obj_t * ta_doorbell_seconds   = NULL;
static lv_obj_t * ta_doorbell_stream    = NULL;
static lv_obj_t * ta_life360_a_entity   = NULL;
static lv_obj_t * ta_life360_a_name     = NULL;
static lv_obj_t * ta_life360_b_entity   = NULL;
static lv_obj_t * ta_life360_b_name     = NULL;
static lv_obj_t * ta_life360_c_entity   = NULL;
static lv_obj_t * ta_life360_c_name     = NULL;
static lv_obj_t * ta_calendar_ha_entity = NULL;
static lv_obj_t * ta_ha_power_entity    = NULL;
static lv_obj_t * ta_ha_gas_entity      = NULL;
static lv_obj_t * ta_ha_solar_entity    = NULL;

/* Browse-button context — heap-allocated, freed in the callback. */
struct ha_browse_ctx {
    lv_obj_t * ta;
    char       domain[32];
};

static void on_ha_browse(lv_event_t * e) {
    struct ha_browse_ctx * ctx = lv_event_get_user_data(e);
    screen_ha_picker_open(ctx->domain, ctx->ta);
    free(ctx);
}

/* Helper: add a labelled textarea row to the modal panel. Returns the
 * y position for the next row. If `domain` is non-NULL a small [🔍]
 * browse button is placed next to the textarea — tapping it opens the
 * HA entity picker filtered to that domain. */
static int ha_field_row(lv_obj_t * p, int y, const char * label,
                        const char * value, const char * placeholder,
                        int width, lv_obj_t ** ta_out,
                        const char * domain) {
    lv_obj_t * lb = lv_label_create(p);
    lv_obj_set_style_text_color(lb, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lb, SF(18), 0);
    lv_label_set_text(lb, label);
    lv_obj_align(lb, LV_ALIGN_TOP_LEFT, SX(4), y + 4);

    lv_obj_t * ta = lv_textarea_create(p);
    lv_obj_set_size(ta, width, SY(36));
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, SX(260), y);
    lv_textarea_set_one_line(ta, true);
    if (placeholder) lv_textarea_set_placeholder_text(ta, placeholder);
    if (value && value[0]) lv_textarea_set_text(ta, value);
    *ta_out = ta;

    /* Browse button for entity fields */
    if (domain) {
        struct ha_browse_ctx * ctx = malloc(sizeof(*ctx));
        if (ctx) {
            ctx->ta = ta;
            snprintf(ctx->domain, sizeof(ctx->domain), "%s", domain);
            lv_obj_t * b = lv_btn_create(p);
            lv_obj_set_size(b, SY(36), SY(36));
            lv_obj_align(b, LV_ALIGN_TOP_LEFT,
                         SX(260) + width + SX(6), y);
            lv_obj_set_style_radius(b, 6, 0);
            lv_obj_t * bl = lv_label_create(b);
            lv_label_set_text(bl, "...");
            lv_obj_center(bl);
            lv_obj_add_event_cb(b, on_ha_browse, LV_EVENT_CLICKED, ctx);
        }
    }
    return y + 44;
}

static void on_ha_entities_save(lv_event_t * e) {
    (void)e;
    if (ta_ha_host) {
        const char * v = lv_textarea_get_text(ta_ha_host);
        snprintf(settings.ha_host, sizeof settings.ha_host, "%s", v ? v : "");
    }
    if (ta_ha_token) {
        const char * tk = lv_textarea_get_text(ta_ha_token);
        if (tk && tk[0]) ha_set_token(tk);   /* write /mnt/data/ha.cfg */
    }
    ha_start();                              /* reconnect with the new host + token */
    #define SAVE_TA(ta, field) do { \
        if (ta) { const char * v = lv_textarea_get_text(ta); \
                  snprintf(field, sizeof field, "%s", v ? v : ""); } \
    } while(0)
    SAVE_TA(ta_curtain_entity,     settings.curtain_entity);
    SAVE_TA(ta_curtain_bat_a,      settings.curtain_bat_a);
    SAVE_TA(ta_curtain_bat_b,      settings.curtain_bat_b);
    SAVE_TA(ta_blinds_entity,      settings.blinds_entity);
    SAVE_TA(ta_blinds_bat_a,       settings.blinds_bat_a);
    SAVE_TA(ta_blinds_bat_b,       settings.blinds_bat_b);
    SAVE_TA(ta_doorbell_entity,    settings.doorbell_entity);
    SAVE_TA(ta_doorbell_camera,    settings.doorbell_camera);
    SAVE_TA(ta_doorbell_stream,    settings.doorbell_stream_url);
    SAVE_TA(ta_life360_a_entity,   settings.life360_a_entity);
    SAVE_TA(ta_life360_a_name,     settings.life360_a_name);
    SAVE_TA(ta_life360_b_entity,   settings.life360_b_entity);
    SAVE_TA(ta_life360_b_name,     settings.life360_b_name);
    SAVE_TA(ta_life360_c_entity,   settings.life360_c_entity);
    SAVE_TA(ta_life360_c_name,     settings.life360_c_name);
    SAVE_TA(ta_calendar_ha_entity, settings.calendar_ha_entity);
    SAVE_TA(ta_ha_power_entity,    settings.ha_power_entity);
    SAVE_TA(ta_ha_gas_entity,      settings.ha_gas_entity);
    SAVE_TA(ta_ha_solar_entity,    settings.ha_solar_entity);
    if (ta_doorbell_seconds) {
        const char * v = lv_textarea_get_text(ta_doorbell_seconds);
        int iv = v ? atoi(v) : 30;
        if (iv < 3) iv = 3;
        if (iv > 300) iv = 300;
        settings.doorbell_seconds = iv;
    }
    #undef SAVE_TA
    modal_close(e);                       /* persists settings, closes sub-modal, restores parent */
}

/* Push the dynamic device manager (add/remove/pin lights, covers, switches,
 * scripts, scenes). The HA-entities modal stays open underneath, so the user
 * returns to it when they pop back. */
static void on_manage_devices(lv_event_t * e) {
    (void)e;
    ui_push(screen_ha_devices_create());
}

static void open_ha_entities_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open("Home Assistant", 980);
    int y = (DISP_VER < 600) ? 86 : 70;
    const int tw = SX(480);

    /* Enable toggle — makes this a self-contained HA config (like the
       Domoticz tile), so the dedicated Settings tile is all you need. */
    lv_obj_t * r_en = panel_row(p, y, tr("Home Assistant inschakelen", "Home Assistant enabled"), NULL);
    row_switch(r_en, settings.enable_ha, on_int_ha);
    y += 82;

    /* HA host */
    lv_obj_t * lh = lv_label_create(p);
    lv_obj_set_style_text_color(lh, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lh, SF(22), 0);
    lv_label_set_text(lh, tr("HA-host:", "HA host:"));
    lv_obj_align(lh, LV_ALIGN_TOP_LEFT, SX(4), y);
    ta_ha_host = lv_textarea_create(p);
    lv_obj_set_size(ta_ha_host, tw, SY(44));
    lv_obj_align(ta_ha_host, LV_ALIGN_TOP_LEFT, SX(260), y - 4);
    lv_textarea_set_one_line(ta_ha_host, true);
    /* ip:port, or a full URL — https:// is accepted (see homeassistant.c). */
    lv_textarea_set_placeholder_text(ta_ha_host, "192.168.1.10:8123 of https://ha.thuis.nl");
    lv_textarea_set_text(ta_ha_host, settings.ha_host);
    y += 56;

    /* HA Long-Lived Access Token -> /mnt/data/ha.cfg. HA auth is a token (no
     * user/pass); create one in HA at Profile -> Long-Lived Access Tokens.
     * Long to type on the panel — easier to paste via the web UI on :10081 —
     * but settable here so the on-device config is complete. */
    lv_obj_t * ltk = lv_label_create(p);
    lv_obj_set_style_text_color(ltk, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(ltk, SF(22), 0);
    lv_label_set_text(ltk, tr("HA-token:", "HA token:"));
    lv_obj_align(ltk, LV_ALIGN_TOP_LEFT, SX(4), y);
    ta_ha_token = lv_textarea_create(p);
    lv_obj_set_size(ta_ha_token, tw, SY(44));
    lv_obj_align(ta_ha_token, LV_ALIGN_TOP_LEFT, SX(260), y - 4);
    lv_textarea_set_one_line(ta_ha_token, true);
    lv_textarea_set_password_mode(ta_ha_token, true);
    lv_textarea_set_placeholder_text(ta_ha_token, "Long-Lived Access Token");
    { char tok[256]; ha_get_token(tok, sizeof tok); if (tok[0]) lv_textarea_set_text(ta_ha_token, tok); }
    y += 56;

    /* ── Devices ── (dynamic list: lights, covers, switches, scripts, scenes) */
    lv_obj_t * lsec = lv_label_create(p);
    lv_obj_set_style_text_color(lsec, lv_color_hex(0x44aaff), 0);
    lv_obj_set_style_text_font(lsec, SF(22), 0);
    lv_label_set_text(lsec, tr("Apparaten", "Devices"));
    lv_obj_align(lsec, LV_ALIGN_TOP_LEFT, SX(4), y);
    y += 36;
    {
        lv_obj_t * hint = lv_label_create(p);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x88aabb), 0);
        lv_obj_set_style_text_font(hint, SF(18), 0);
        lv_label_set_text(hint, tr("Lampen, zonwering, schakelaars, scripts en scenes -\n"
                                    "te zien achter de Apparaten-knop op start.",
                                    "Lights, covers, switches, scripts and scenes -\n"
                                    "shown behind the home Devices button."));
        lv_obj_align(hint, LV_ALIGN_TOP_LEFT, SX(4), y);
        y += 52;
        lv_obj_t * bm = lv_btn_create(p);
        lv_obj_set_size(bm, SX(300), SY(48));
        lv_obj_align(bm, LV_ALIGN_TOP_LEFT, SX(4), y);
        lv_obj_set_style_bg_color(bm, lv_color_hex(0x2e5e8a), 0);
        lv_obj_set_style_radius(bm, 8, 0);
        lv_obj_add_event_cb(bm, on_manage_devices, LV_EVENT_CLICKED, NULL);
        lv_obj_t * bml = lv_label_create(bm);
        lv_obj_set_style_text_color(bml, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(bml, SF(20), 0);
        lv_label_set_text(bml, tr("Apparaten beheren...", "Manage devices..."));
        lv_obj_center(bml);
        y += 60;
    }

    /* ── Energy ── */
    lsec = lv_label_create(p);
    lv_obj_set_style_text_color(lsec, lv_color_hex(0x44aaff), 0);
    lv_obj_set_style_text_font(lsec, SF(22), 0);
    lv_label_set_text(lsec, tr("Energie", "Energy"));
    lv_obj_align(lsec, LV_ALIGN_TOP_LEFT, SX(4), y);
    y += 36;
    {
        lv_obj_t * hint = lv_label_create(p);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x88aabb), 0);
        lv_obj_set_style_text_font(hint, SF(18), 0);
        lv_label_set_text(hint, tr(
            "Stroom/gas worden alleen gebruikt als de energiebron op\n"
            "'Home Assistant' staat (Integraties). Zon werkt altijd.",
            "Power/gas are only used when the energy source is set to\n"
            "'Home Assistant' (Integrations). Solar always applies."));
        lv_obj_align(hint, LV_ALIGN_TOP_LEFT, SX(4), y);
        y += 52;
    }
    y = ha_field_row(p, y, tr("Stroom nu (W):", "Power now (W):"),
                     settings.ha_power_entity, "sensor.power_consumption", tw,
                     &ta_ha_power_entity, "sensor");
    y = ha_field_row(p, y, tr("Gasmeter (m3):", "Gas meter (m3):"),
                     settings.ha_gas_entity, "sensor.gas_consumption", tw,
                     &ta_ha_gas_entity, "sensor");
    y = ha_field_row(p, y, tr("Zonnepanelen (W):", "Solar production (W):"),
                     settings.ha_solar_entity, "sensor.solar_power", tw,
                     &ta_ha_solar_entity, "sensor");

    /* ── Doorbell ── */
    lsec = lv_label_create(p);
    lv_obj_set_style_text_color(lsec, lv_color_hex(0x44aaff), 0);
    lv_obj_set_style_text_font(lsec, SF(22), 0);
    lv_label_set_text(lsec, tr("Deurbel", "Doorbell"));
    lv_obj_align(lsec, LV_ALIGN_TOP_LEFT, SX(4), y);
    y += 36;
    y = ha_field_row(p, y, tr("Trigger-entiteit:", "Trigger entity:"), settings.doorbell_entity, "binary_sensor.deurbel", tw, &ta_doorbell_entity, "binary_sensor");
    y = ha_field_row(p, y, tr("Camera-entiteit:", "Camera entity:"), settings.doorbell_camera, "camera.voordeur", tw, &ta_doorbell_camera, "camera");
    y = ha_field_row(p, y, tr("Seconden tonen (3-300):", "Show seconds (3-300):"), NULL, "30", SX(120), &ta_doorbell_seconds, NULL);
    if (ta_doorbell_seconds) {
        char sec_str[8];
        snprintf(sec_str, sizeof(sec_str), "%d", settings.doorbell_seconds);
        lv_textarea_set_text(ta_doorbell_seconds, sec_str);
    }
    y = ha_field_row(p, y, tr("MJPEG-stream-URL:", "MJPEG stream URL:"), settings.doorbell_stream_url, "http://.../stream.mjpeg", tw, &ta_doorbell_stream, NULL);
    y += 8;

    /* ── Family / Trackers ── */
    lsec = lv_label_create(p);
    lv_obj_set_style_text_color(lsec, lv_color_hex(0x44aaff), 0);
    lv_obj_set_style_text_font(lsec, SF(22), 0);
    lv_label_set_text(lsec, tr("Trackers", "Trackers"));
    lv_obj_align(lsec, LV_ALIGN_TOP_LEFT, SX(4), y);
    y += 36;
    y = ha_field_row(p, y, tr("Tracker A entiteit:", "Tracker A entity:"), settings.life360_a_entity, "device_tracker.auto", tw, &ta_life360_a_entity, "device_tracker");
    y = ha_field_row(p, y, tr("Tracker A naam:", "Tracker A name:"), settings.life360_a_name, "Auto", tw, &ta_life360_a_name, NULL);
    y = ha_field_row(p, y, tr("Tracker B entiteit:", "Tracker B entity:"), settings.life360_b_entity, "device_tracker.google_pixel_6a", tw, &ta_life360_b_entity, "device_tracker");
    y = ha_field_row(p, y, tr("Tracker B naam:", "Tracker B name:"), settings.life360_b_name, "Telefoon", tw, &ta_life360_b_name, NULL);
    y = ha_field_row(p, y, tr("Tracker C entiteit:", "Tracker C entity:"), settings.life360_c_entity, "device_tracker.life360_member", tw, &ta_life360_c_entity, "device_tracker");
    y = ha_field_row(p, y, tr("Tracker C naam:", "Tracker C name:"), settings.life360_c_name, "Persoon", tw, &ta_life360_c_name, NULL);
    y += 8;

    /* ── Calendar ── */
    lsec = lv_label_create(p);
    lv_obj_set_style_text_color(lsec, lv_color_hex(0x44aaff), 0);
    lv_obj_set_style_text_font(lsec, SF(22), 0);
    lv_label_set_text(lsec, tr("Agenda", "Calendar"));
    lv_obj_align(lsec, LV_ALIGN_TOP_LEFT, SX(4), y);
    y += 36;
    y = ha_field_row(p, y, tr("HA-agenda-entiteit:", "HA calendar entity:"), settings.calendar_ha_entity, "calendar.gezin", tw, &ta_calendar_ha_entity, "calendar");
    y += 16;

    /* Save button */
    lv_obj_t * b_save = lv_btn_create(p);
    lv_obj_set_size(b_save, SX(240), SY(50));
    lv_obj_align(b_save, LV_ALIGN_TOP_LEFT, SX(4), y);
    lv_obj_set_style_bg_color(b_save, lv_color_hex(0x2e6e3a), 0);
    lv_obj_set_style_radius(b_save, 8, 0);
    lv_obj_add_event_cb(b_save, on_ha_entities_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bsl = lv_label_create(b_save);
    lv_obj_set_style_text_color(bsl, lv_color_hex(0xffffff), 0);
    lv_label_set_text(bsl, tr("Opslaan & sluiten", "Save & close"));
    lv_obj_center(bsl);
}

/* ==================== Tile slots modal ====================
 * Phase 2 of marketplace work. Four dropdowns — one per right-column
 * home tile (Energy / Family / Vent / Water) — let the user replace the
 * built-in content with any installed marketplace integration. Empty
 * dropdown = "Built-in" (existing behaviour). The screen also doubles as
 * the picker triggered by long-press on a home tile (see tile_slot_open_picker).
 */
static lv_obj_t * dd_tile_slot[TILE_SLOT_COUNT];

/* Build the option string for the rollers: "Built-in\n<name>\n<name>...".
 * Output buffer is owned by caller and must be large enough — 64 chars per
 * integration + 1 for separator. */
static void build_slot_options(int slot, char * out, size_t cap) {
    size_t n = 0;
    if (slot < 4) {
        const char * def_label;
        if (slot == 0) def_label = tr("Ingebouwd (Energie)", "Built-in (Energy)");
        else if (slot == 1) def_label = tr("Ingebouwd (Familie)", "Built-in (Family)");
        else if (slot == 2) def_label = tr("Ingebouwd (Ventilatie)", "Built-in (Ventilation)");
        else def_label = tr("Ingebouwd (Water)", "Built-in (Water)");

        n = snprintf(out, cap, "%s\n%s\n%s", def_label,
                     tr("Agenda", "Calendar"), tr("Afval", "Waste"));
    } else {
        n = snprintf(out, cap, "%s\n%s\n%s\n%s\n%s\n%s\n%s",
                     tr("Geen", "None"),
                     tr("Energie", "Energy"),
                     tr("Familie", "Family"),
                     tr("Ventilatie", "Ventilation"),
                     tr("Water", "Water"),
                     tr("Agenda", "Calendar"),
                     tr("Afval", "Waste"));
    }

    int total = tile_slots_integration_count();
    for (int i = 0; i < total && n + INTEG_NAME_MAX + 2 < cap; i++) {
        const integration_meta_t * m = tile_slots_integration_at(i);
        n += snprintf(out + n, cap - n, "\n%s", tile_slots_integration_title(m));
    }
}

/* Resolve dropdown index → binding id for the given slot. */
static const char * slot_dropdown_to_id(int slot, int idx) {
    if (idx <= 0) return "";
    if (slot < 4) {
        if (idx == 1) return "local:agenda";
        if (idx == 2) return "local:waste";
        const integration_meta_t * m = tile_slots_integration_at(idx - 3);
        return m ? m->id : "";
    } else {
        if (idx == 1) return "local:energy";
        if (idx == 2) return "local:family";
        if (idx == 3) return "local:vent";
        if (idx == 4) return "local:water";
        if (idx == 5) return "local:agenda";
        if (idx == 6) return "local:waste";
        const integration_meta_t * m = tile_slots_integration_at(idx - 7);
        return m ? m->id : "";
    }
}

static int slot_dropdown_from_id(int slot, const char * id) {
    if (!id || !id[0]) return 0;
    if (slot < 4) {
        if (strcmp(id, "local:agenda") == 0) return 1;
        if (strcmp(id, "local:waste") == 0) return 2;
        int total = tile_slots_integration_count();
        for (int i = 0; i < total; i++) {
            const integration_meta_t * m = tile_slots_integration_at(i);
            if (strcmp(m->id, id) == 0 || (strcmp(id, "local:sonos") == 0 && strcmp(m->id, "sonos") == 0)) return i + 3;
        }
    } else {
        if (strcmp(id, "local:energy") == 0) return 1;
        if (strcmp(id, "local:family") == 0) return 2;
        if (strcmp(id, "local:vent") == 0) return 3;
        if (strcmp(id, "local:water") == 0) return 4;
        if (strcmp(id, "local:agenda") == 0) return 5;
        if (strcmp(id, "local:waste") == 0) return 6;
        int total = tile_slots_integration_count();
        for (int i = 0; i < total; i++) {
            const integration_meta_t * m = tile_slots_integration_at(i);
            if (strcmp(m->id, id) == 0 || (strcmp(id, "local:sonos") == 0 && strcmp(m->id, "sonos") == 0)) return i + 7;
        }
    }
    return 0;
}

static void on_tile_slot_change(lv_event_t * e) {
    lv_obj_t * dd = lv_event_get_target(e);
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    int idx  = lv_dropdown_get_selected(dd);
    tile_slots_bind(slot, slot_dropdown_to_id(slot, idx));
}

static void on_modal_to_marketplace(lv_event_t * e) {
    modal_close(e);                       /* tears down this modal (async) */
    ui_push(screen_marketplace_create()); /* then open the marketplace */
}
static lv_obj_t * marketplace_button(lv_obj_t * parent) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, SX(250), SY(50));
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, SX(-4), SY(-8));
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2e6e3a), 0);
    lv_obj_add_event_cb(b, on_modal_to_marketplace, LV_EVENT_CLICKED, NULL);
    lv_obj_t * l = lv_label_create(b);
    lv_label_set_text_fmt(l, LV_SYMBOL_DOWNLOAD "  %s", tr("Marketplace openen", "Open Marketplace"));
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_center(l);
    return b;
}

static void open_tile_slots_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open(tr("Tegelslots", "Tile slots"), 500);

    /* 8 real physical slots hosted in a vertical scroller */
    lv_obj_t * sc = lv_obj_create(p);
    lv_obj_set_size(sc, SX(820), (DISP_VER < 600) ? 250 : SY(320));
    lv_obj_align(sc, LV_ALIGN_TOP_LEFT, 0, SY(64));
    lv_obj_set_style_bg_opa(sc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sc, 0, 0);
    lv_obj_set_style_pad_all(sc, 0, 0);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_AUTO);

    int y = 0;

    /* Section 1: Startscherm (4 vaste tegels) */
    lv_obj_t * sec1 = lv_label_create(sc);
    lv_obj_set_style_text_font(sec1, SF(20), 0);
    lv_obj_set_style_text_color(sec1, lv_color_hex(0xffcc44), 0);
    lv_label_set_text(sec1, tr("Startscherm (4 vaste tegels)", "Home screen (4 fixed tiles)"));
    lv_obj_align(sec1, LV_ALIGN_TOP_LEFT, SX(8), y + 6);
    y += 34;

    for (int s = 0; s < 4; s++) {
        lv_obj_t * row = lv_obj_create(sc);
        lv_obj_set_size(row, SX(790), SY(72));
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, SX(4), y);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x152033), 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, SF(22), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_label_set_text(lbl, tile_slot_label(s));
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, SX(8), 0);

        char slot_opts[1024];
        build_slot_options(s, slot_opts, sizeof slot_opts);

        lv_obj_t * dd = lv_dropdown_create(row);
        lv_obj_set_size(dd, SX(480), SY(56));
        lv_obj_align(dd, LV_ALIGN_RIGHT_MID, SX(-8), 0);
        lv_dropdown_set_options(dd, slot_opts);
        lv_dropdown_set_selected(dd, slot_dropdown_from_id(s, tile_slots_binding(s)));
        lv_obj_set_style_text_font(dd, SF(22), 0);
        lv_obj_add_event_cb(dd, on_tile_slot_change, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)s);
        dd_tile_slot[s] = dd;

        y += 82;
    }

    y += 10;
    /* Section 2: Pagina 2 (Veegscherm - 4 tegels) */
    lv_obj_t * sec2 = lv_label_create(sc);
    lv_obj_set_style_text_font(sec2, SF(20), 0);
    lv_obj_set_style_text_color(sec2, lv_color_hex(0xffcc44), 0);
    lv_label_set_text(sec2, tr("Pagina 2 (Veegscherm - 4 tegels)", "Page 2 (Swipe screen - 4 tiles)"));
    lv_obj_align(sec2, LV_ALIGN_TOP_LEFT, SX(8), y + 6);
    y += 34;

    for (int s = 4; s < 8; s++) {
        lv_obj_t * row = lv_obj_create(sc);
        lv_obj_set_size(row, SX(790), SY(72));
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, SX(4), y);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x152033), 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, SF(22), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_label_set_text(lbl, tile_slot_label(s));
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, SX(8), 0);

        char slot_opts[1024];
        build_slot_options(s, slot_opts, sizeof slot_opts);

        lv_obj_t * dd = lv_dropdown_create(row);
        lv_obj_set_size(dd, SX(480), SY(56));
        lv_obj_align(dd, LV_ALIGN_RIGHT_MID, SX(-8), 0);
        lv_dropdown_set_options(dd, slot_opts);
        lv_dropdown_set_selected(dd, slot_dropdown_from_id(s, tile_slots_binding(s)));
        lv_obj_set_style_text_font(dd, SF(22), 0);
        lv_obj_add_event_cb(dd, on_tile_slot_change, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)s);
        dd_tile_slot[s] = dd;

        y += 82;
    }

    lv_obj_t * hint = lv_label_create(p);
    lv_obj_set_style_text_font(hint, SF(18), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x88aabb), 0);
    lv_obj_set_width(hint, SX(540));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint,
        tr("Startscherm: 4 tegels rechts. Pagina 2: 4 tegels op het veegscherm. "
           "\"Ingebouwd\" of \"Geen\" herstelt de standaardinhoud.",
           "Home screen: 4 tiles right. Page 2: 4 tiles on the swipe screen. "
           "\"Built-in\" or \"None\" keeps default content."));
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, SX(4), SY(-8));
    marketplace_button(p);
}

/* Public entry point called by screen_home.c long-press handler. Just
 * opens the same modal — keeps the picker UI consistent. */
void screen_settings_open_tile_slots_modal(void) {
    open_tile_slots_modal(NULL);
}

/* Open single-slot assignment modal for a specific tile slot (0..7). */
void screen_settings_open_tile_slot_single_modal(int slot) {
    if (slot < 0 || slot >= TILE_SLOT_COUNT) return;
    char title[64];
    snprintf(title, sizeof title, "%s: %s", tr("Tegel toewijzen", "Assign tile"), tile_slot_label(slot));
    lv_obj_t * p = modal_open(title, 260);

    lv_obj_t * info = lv_label_create(p);
    lv_obj_set_style_text_font(info, SF(18), 0);
    lv_obj_set_style_text_color(info, lv_color_hex(0x88aabb), 0);
    lv_label_set_text(info, tr("Kies welke integratie of inhoud op deze tegel getoond wordt:",
                               "Choose which integration or content is displayed on this tile:"));
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, SX(20), SY(70));

    char slot_opts[1024];
    build_slot_options(slot, slot_opts, sizeof slot_opts);

    lv_obj_t * dd = lv_dropdown_create(p);
    lv_obj_set_size(dd, SX(540), SY(56));
    lv_obj_align(dd, LV_ALIGN_TOP_LEFT, SX(20), SY(104));
    lv_dropdown_set_options(dd, slot_opts);
    lv_dropdown_set_selected(dd, slot_dropdown_from_id(slot, tile_slots_binding(slot)));
    lv_obj_set_style_text_font(dd, SF(22), 0);
    lv_obj_add_event_cb(dd, on_tile_slot_change, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)slot);
    dd_tile_slot[slot] = dd;

    marketplace_button(p);
}

static void open_about_modal(lv_event_t * e) {
    (void)e;
    update_modal_open(NULL);
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
static char       g_ket_result_buf[512]   = "";  /* fits the longest FAIL msg + detail */
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
    if (!host[0]) return -1;   /* no OTGW host configured */
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
                 "OK: %s (gateway/relay - correct for wired mode)", gw_value);
    } else if (strstr(gw_value, "GW=2")) {
        snprintf(g_check_result_buf, sizeof(g_check_result_buf),
                 "INFO: %s (no-thermostat mode - needed for wireless mode)", gw_value);
    } else if (strstr(gw_value, "GW=0")) {
        snprintf(g_check_result_buf, sizeof(g_check_result_buf),
                 "WARN: %s (monitor-only - won't drive boiler)", gw_value);
    } else {
        snprintf(g_check_result_buf, sizeof(g_check_result_buf),
                 "INFO: otgwcommands=\"%s\"", gw_value);
    }
    g_check_pending = 1;
    return NULL;
}

static void on_test_click(lv_event_t * e) {
    (void)e;
    lv_label_set_text(lbl_test_result, tr("Testen...", "Testing..."));
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
    lv_label_set_text(lbl_ket_result, tr("Keteladapter testen...", "Testing keteladapter..."));
    pthread_t t;
    if (pthread_create(&t, NULL, ket_thread, NULL) == 0) pthread_detach(t);
}

static void on_check_click(lv_event_t * e) {
    (void)e;
    lv_label_set_text(lbl_check_result, tr("OTGW opvragen...", "Querying OTGW..."));
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
    if (strcmp(m, "off")      == 0) return tr("Direct - keteladapter is de OT-master (origineel qt-gui-gedrag, geen bridge-script)", "Direct - keteladapter is the OT master (original qt-gui behaviour, no bridge script)");
    if (strcmp(m, "wireless") == 0) return tr("Draadloos - bridge bootst Quby na, OTGW stuurt de ketel", "Wireless - bridge fakes Quby, OTGW drives boiler");
    return                             tr("Proxy - bridge luistert Quby af, eigen warmtepad behouden", "Proxy - bridge sniffs Quby, native heat path preserved");
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
        tr("Tik op Toepassen om inittab te schrijven + bridge/thermstat te herstarten.\n"
           "Verwarming is ~10-15s uit tijdens het omschakelen.",
           "Tap Apply to write inittab + restart bridge/thermstat.\n"
           "Heat will be off for ~10-15s during the switch."));
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
        tr("Toegepast - bridge/thermstat herstart. "
           "Heropen dit venster over ~15s om te controleren.",
           "Applied - bridge/thermstat restarting. "
           "Re-open this modal in ~15s to verify."));
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
    lv_obj_set_style_text_color(mode_lbl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(mode_lbl, SF(22), 0);
    lv_label_set_text(mode_lbl, tr("Modus:", "Mode:"));
    lv_obj_align(mode_lbl, LV_ALIGN_TOP_LEFT, SX(4), y + 12);

    const struct { lv_obj_t ** btn_slot; lv_obj_t ** lbl_slot;
                   const char * caption;
                   lv_event_cb_t cb; int x; } picks[] = {
        { &btn_off,      &lbl_off,      tr("Direct", "Direct"),     on_mode_off_click,      120 },
        { &btn_proxy,    &lbl_proxy,    tr("Proxy", "Proxy"),       on_mode_proxy_click,    270 },
        { &btn_wireless, &lbl_wireless, tr("Draadloos", "Wireless"), on_mode_wireless_click, 470 },
    };
    for (unsigned i = 0; i < sizeof picks / sizeof picks[0]; i++) {
        lv_obj_t * b = lv_btn_create(p);
        lv_obj_set_size(b, SX(140), SY(50));
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, SX(picks[i].x), SY(y));
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_add_event_cb(b, picks[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t * l = lv_label_create(b);
        lv_label_set_text(l, picks[i].caption);
        lv_obj_set_style_text_font(l, SF(22), 0);
        lv_obj_center(l);
        *picks[i].btn_slot = b;
        *picks[i].lbl_slot = l;
    }
    y += 60;

    /* Mode description (one-liner that flips with the selection). */
    lbl_otmode = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_otmode, lv_color_hex(0xa8c4dc), 0);
    lv_obj_set_style_text_font(lbl_otmode, SF(18), 0);
    lv_obj_set_width(lbl_otmode, SX(560));
    lv_label_set_long_mode(lbl_otmode, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_otmode, mode_label(settings.ot_bridge_mode));
    lv_obj_align(lbl_otmode, LV_ALIGN_TOP_LEFT, SX(4), y);
    paint_mode_buttons();
    y += 50;

    /* OTGW host */
    lv_obj_t * lbl_host = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_host, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_host, SF(22), 0);
    lv_label_set_text(lbl_host, tr("OTGW-host:", "OTGW host:"));
    lv_obj_align(lbl_host, LV_ALIGN_TOP_LEFT, SX(4), y);
    ta_otgw_host = lv_textarea_create(p); ta_make_numeric(ta_otgw_host);
    lv_obj_set_size(ta_otgw_host, SX(380), SY(44));
    lv_obj_align(ta_otgw_host, LV_ALIGN_TOP_LEFT, SX(260), y - 4);
    lv_textarea_set_one_line(ta_otgw_host, true);
    lv_textarea_set_text(ta_otgw_host, settings.otgw_host);
    y += 60;

    /* OTGW user/pass */
    lv_obj_t * lbl_user = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_user, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_user, SF(22), 0);
    lv_label_set_text(lbl_user, tr("Gebruiker (opt):", "User (opt):"));
    lv_obj_align(lbl_user, LV_ALIGN_TOP_LEFT, SX(4), y);
    ta_otgw_user = lv_textarea_create(p);
    lv_obj_set_size(ta_otgw_user, SX(380), SY(44));
    lv_obj_align(ta_otgw_user, LV_ALIGN_TOP_LEFT, SX(260), y - 4);
    lv_textarea_set_one_line(ta_otgw_user, true);
    lv_textarea_set_text(ta_otgw_user, settings.otgw_user);
    y += 60;

    lv_obj_t * lbl_pass = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_pass, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_pass, SF(22), 0);
    lv_label_set_text(lbl_pass, tr("Wachtwoord (opt):", "Password (opt):"));
    lv_obj_align(lbl_pass, LV_ALIGN_TOP_LEFT, SX(4), y);
    ta_otgw_pass = lv_textarea_create(p);
    lv_obj_set_size(ta_otgw_pass, SX(380), SY(44));
    lv_obj_align(ta_otgw_pass, LV_ALIGN_TOP_LEFT, SX(260), y - 4);
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
    lv_obj_set_size(b_test, SX(200), SY(50));
    lv_obj_align(b_test, LV_ALIGN_TOP_LEFT, SX(4), y);
    lv_obj_set_style_bg_color(b_test, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(b_test, on_test_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_test_lbl = lv_label_create(b_test);
    lv_label_set_text(b_test_lbl, tr("Test OTGW", "Test OTGW"));
    lv_obj_center(b_test_lbl);

    lv_obj_t * b_check = lv_btn_create(p);
    lv_obj_set_size(b_check, SX(220), SY(50));
    lv_obj_align(b_check, LV_ALIGN_TOP_LEFT, SX(218), y);
    lv_obj_set_style_bg_color(b_check, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(b_check, on_check_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_check_lbl = lv_label_create(b_check);
    lv_label_set_text(b_check_lbl, tr("GW-modus checken", "Check GW mode"));
    lv_obj_center(b_check_lbl);

    lv_obj_t * b_ket = lv_btn_create(p);
    lv_obj_set_size(b_ket, SX(220), SY(50));
    lv_obj_align(b_ket, LV_ALIGN_TOP_LEFT, SX(452), y);
    lv_obj_set_style_bg_color(b_ket, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(b_ket, on_ket_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_ket_lbl = lv_label_create(b_ket);
    lv_label_set_text(b_ket_lbl, tr("Test keteladapter", "Test keteladapter"));
    lv_obj_center(b_ket_lbl);
    y += 65;

    /* Second button row — just Apply */
    lv_obj_t * b_apply = lv_btn_create(p);
    lv_obj_set_size(b_apply, SX(200), SY(50));
    lv_obj_align(b_apply, LV_ALIGN_TOP_LEFT, SX(4), y);
    lv_obj_set_style_bg_color(b_apply, lv_color_hex(0x884422), 0);
    lv_obj_add_event_cb(b_apply, on_apply_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_apply_lbl = lv_label_create(b_apply);
    lv_label_set_text(b_apply_lbl, tr("Modus toepassen", "Apply mode"));
    lv_obj_center(b_apply_lbl);
    y += 60;

    /* Result labels stacked below the button rows */
    lbl_test_result = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_test_result, lv_color_hex(0xa8c4dc), 0);
    lv_obj_set_style_text_font(lbl_test_result, SF(18), 0);
    lv_label_set_long_mode(lbl_test_result, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_test_result, SX(770));
    lv_obj_align(lbl_test_result, LV_ALIGN_TOP_LEFT, SX(4), y);
    lv_label_set_text(lbl_test_result, tr("OTGW-test: (niet uitgevoerd)", "OTGW test: (not run)"));
    y += 35;

    lbl_check_result = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_check_result, lv_color_hex(0xa8c4dc), 0);
    lv_obj_set_style_text_font(lbl_check_result, SF(18), 0);
    lv_obj_set_width(lbl_check_result, SX(770));
    lv_label_set_long_mode(lbl_check_result, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_check_result, LV_ALIGN_TOP_LEFT, SX(4), y);
    lv_label_set_text(lbl_check_result, tr("GW-moduscheck: (niet uitgevoerd)", "GW mode check: (not run)"));
    y += 35;

    lbl_ket_result = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_ket_result, lv_color_hex(0xa8c4dc), 0);
    lv_obj_set_style_text_font(lbl_ket_result, SF(18), 0);
    lv_obj_set_width(lbl_ket_result, SX(770));
    lv_label_set_long_mode(lbl_ket_result, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_ket_result, LV_ALIGN_TOP_LEFT, SX(4), y);
    lv_label_set_text(lbl_ket_result, tr("Keteladapter-test: (niet uitgevoerd)", "Keteladapter test: (not run)"));
    y += 45;

    lbl_apply_warning = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_apply_warning, lv_color_hex(0xcc8866), 0);
    lv_obj_set_style_text_font(lbl_apply_warning, SF(18), 0);
    lv_label_set_long_mode(lbl_apply_warning, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_apply_warning, SX(770));
    lv_obj_align(lbl_apply_warning, LV_ALIGN_TOP_LEFT, SX(4), y);
    lv_label_set_text(lbl_apply_warning,
        tr("Toepassen herschrijft /etc/inittab en herstart quby_bridge + happ_thermstat. "
           "Verwarming pauzeert ~10s tijdens het omschakelen.",
           "Apply rewrites /etc/inittab and restarts quby_bridge + happ_thermstat. "
           "Heat will pause for ~10s during the switch."));

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
 * Places an opaque full-black overlay on lv_layer_sys() with a centered countdown
 * so the user can wipe the glass without triggering buttons.
 * Because it sits on lv_layer_sys(), it cleanly covers the entire display without
 * modifying the screen stack (ui_stack), preventing race conditions with auto-dim
 * or auto-home. Touches are absorbed by the clickable overlay.
 * On expiry, the overlay is deleted, input devices are reset to clear any lingering
 * touch release from wiping, and activity is marked so settings stays visible. */
static lv_obj_t *  g_clean_overlay   = NULL;
static lv_obj_t *  g_clean_lbl       = NULL;
static lv_timer_t * g_clean_tmr      = NULL;
static int         g_clean_remaining = 0;

static void clean_finish(void) {
    if (!g_clean_overlay && !g_clean_tmr) return;
    if (g_clean_tmr) {
        lv_timer_del(g_clean_tmr);
        g_clean_tmr = NULL;
    }
    if (g_clean_overlay) {
        lv_obj_del(g_clean_overlay);
        g_clean_overlay = NULL;
        g_clean_lbl = NULL;
    }
    /* Invalidate/wait-release on indev so any trailing wipe touch does not click settings */
    lv_indev_t * indev = lv_indev_get_next(NULL);
    while (indev) {
        lv_indev_wait_release(indev);
        indev = lv_indev_get_next(indev);
    }
    ui_mark_activity();
}

static void clean_tick(lv_timer_t * t) {
    (void)t;
    ui_mark_activity(); /* prevent auto-dim / auto-home from triggering during clean */
    g_clean_remaining--;
    if (g_clean_remaining <= 0) {
        clean_finish();
        return;
    }
    if (g_clean_lbl) lv_label_set_text_fmt(g_clean_lbl, "%d", g_clean_remaining);
}

static void open_clean_modal(lv_event_t * e) {
    (void)e;
    clean_finish(); /* safety cleanup */

    g_clean_remaining = 30;
    ui_mark_activity();

    g_clean_overlay = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(g_clean_overlay);
    lv_obj_set_size(g_clean_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_clean_overlay, 0, 0);
    lv_obj_set_style_bg_color(g_clean_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_clean_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_clean_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_clean_overlay, LV_OBJ_FLAG_CLICKABLE);

    g_clean_lbl = lv_label_create(g_clean_overlay);
    lv_obj_set_style_text_color(g_clean_lbl, lv_color_hex(0xffffff), 0);
    LV_FONT_DECLARE(lv_font_montserrat_96_custom);
    lv_obj_set_style_text_font(g_clean_lbl, SF(96), 0);
    lv_label_set_text_fmt(g_clean_lbl, "%d", g_clean_remaining);
    lv_obj_center(g_clean_lbl);

    g_clean_tmr = lv_timer_create(clean_tick, 1000, NULL);
}

/* One category tile: icon (optional), big title, caption. */
/* Compact category tile — 4 fit per row. Was 308x188 with 28pt fonts
 * (only 3-wide and overflowed the screen badly); now 232x104 with 20pt
 * title so the whole grid fits in ~4 rows with minimal scrolling. */
static const lv_img_dsc_t * get_scaled_icon(const lv_img_dsc_t * src) {
    if (!src || src->header.cf != LV_IMG_CF_ALPHA_8BIT) return src;
    int target_h = (DISP_VER < 600) ? 18 : 22;
    if (src->header.h <= target_h + 2) return src;

    static struct {
        const lv_img_dsc_t * orig;
        lv_img_dsc_t dsc;
        uint8_t buf[32 * 32];
    } cache[8];
    static int cache_cnt = 0;

    for (int i = 0; i < cache_cnt; i++) {
        if (cache[i].orig == src && cache[i].dsc.header.h == target_h) return &cache[i].dsc;
    }
    if (cache_cnt >= 8) return src;

    int idx = cache_cnt++;
    cache[idx].orig = src;
    int dst_h = target_h;
    int dst_w = (src->header.w * dst_h) / src->header.h;
    if (dst_w > 32) dst_w = 32;
    cache[idx].dsc.header.cf = LV_IMG_CF_ALPHA_8BIT;
    cache[idx].dsc.header.w = dst_w;
    cache[idx].dsc.header.h = dst_h;
    cache[idx].dsc.header.always_zero = 0;
    cache[idx].dsc.header.reserved = 0;
    cache[idx].dsc.data_size = dst_w * dst_h;
    cache[idx].dsc.data = cache[idx].buf;

    uint8_t * dst = cache[idx].buf;
    int sw = src->header.w;
    int sh = src->header.h;
    for (int dy = 0; dy < dst_h; dy++) {
        int sy16 = (dy * sh * 256) / dst_h;
        int y0 = sy16 >> 8;
        int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
        int fy = sy16 & 0xff;
        for (int dx = 0; dx < dst_w; dx++) {
            int sx16 = (dx * sw * 256) / dst_w;
            int x0 = sx16 >> 8;
            int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            int fx = sx16 & 0xff;

            int p00 = src->data[y0 * sw + x0];
            int p10 = src->data[y0 * sw + x1];
            int p01 = src->data[y1 * sw + x0];
            int p11 = src->data[y1 * sw + x1];

            int top = p00 + (((p10 - p00) * fx) >> 8);
            int bot = p01 + (((p11 - p01) * fx) >> 8);
            int val = top + (((bot - top) * fy) >> 8);
            dst[dy * dst_w + dx] = (uint8_t)(val < 0 ? 0 : (val > 255 ? 255 : val));
        }
    }
    return &cache[idx].dsc;
}

#define SETT_TILE_W 232
#define SETT_TILE_H 104
static void make_tile(int x, int y, const lv_img_dsc_t * icon, const char * sym,
                      const char * title, const char * caption, lv_event_cb_t cb) {
    lv_obj_t * parent = scr_tiles_cont ? scr_tiles_cont : scr_root;
    lv_obj_t * tile = lv_btn_create(parent);
    lv_obj_set_size(tile, SX(SETT_TILE_W), SY(SETT_TILE_H));
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_style_bg_color(tile, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(THEME_BTN_BG), LV_STATE_PRESSED);
    lv_obj_set_style_radius(tile, 12, 0);
    lv_obj_set_style_pad_all(tile, 6, 0);
    lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, NULL);

    if (icon) {
        icon = get_scaled_icon(icon);
        lv_obj_t * im = lv_img_create(tile);
        lv_img_set_src(im, icon);
        lv_obj_set_style_img_recolor(im, lv_color_hex(settings.home_light_mode ? 0x2563eb : 0x9fc4e6), 0);
        lv_obj_set_style_img_recolor_opa(im, 255, 0);
#ifdef TOON1
        /* Scaled 24px icon pinned near top with clear gap above title */
        lv_obj_align(im, LV_ALIGN_TOP_MID, 0, 4);
#else
        lv_obj_align(im, LV_ALIGN_TOP_MID, 0, SY(6));
#endif
    } else if (sym) {
        lv_obj_t * s = lv_label_create(tile);
        lv_obj_set_style_text_font(s, SF(22), 0);
        lv_obj_set_style_text_color(s, lv_color_hex(settings.home_light_mode ? 0x2563eb : 0x9fc4e6), 0);
        lv_label_set_text(s, sym);
        lv_obj_align(s, LV_ALIGN_TOP_MID, 0, SY(8));
    }

    lv_obj_t * t = lv_label_create(tile);
    lv_obj_set_style_text_font(t, SF(22), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(THEME_TEXT_HI), 0);
    lv_label_set_text(t, title);
#ifdef TOON1
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 3);   /* proper gap to caption on 480px panel */
#else
    lv_obj_align(t, LV_ALIGN_CENTER, 0, SY(8));
#endif

    lv_obj_t * c = lv_label_create(tile);
    lv_obj_set_style_text_font(c, SF(14), 0);
    lv_obj_set_style_text_color(c, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_width(c, SX(SETT_TILE_W - 16));
    lv_obj_set_height(c, SY(18));
    lv_label_set_long_mode(c, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(c, caption);
    lv_obj_align(c, LV_ALIGN_BOTTOM_MID, 0, SY(-6));
}

/* ===================================================================== */
/* ===================================================================== */
/* Web access modal: PWA login toggle + username + password + Apply       */
/* PIN modal: on-device PIN toggle + 4-6 digit code + Apply               */
/*                                                                        */
/* Both follow the MQTT/Domoticz pattern — toggle commits immediately on  */
/* state change; textarea values commit only on the Apply button (modal-  */
/* close-without-apply discards them). Apply also calls settings_save()   */
/* explicitly so partial commits stick even if the modal is reopened.    */
/* ===================================================================== */
static lv_obj_t * ta_pwa_user = NULL;
static lv_obj_t * ta_pwa_pass = NULL;
static lv_obj_t * ta_pin_code = NULL;

static void on_pwa_enabled_change(lv_event_t * e) {
    settings.pwa_login_enabled =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static void on_pwa_apply_click(lv_event_t * e) {
    (void)e;
    const char * u = lv_textarea_get_text(ta_pwa_user);
    const char * p = lv_textarea_get_text(ta_pwa_pass);
    /* Empty user → fall back to "admin" rather than locking the user out
     * (a blank username field would otherwise make login impossible). */
    snprintf(settings.pwa_login_user, sizeof settings.pwa_login_user,
             "%s", (u && *u) ? u : "admin");
    /* Password may legitimately be empty — that flips the PWA into the
     * /set-password forced-onboarding state, which is the right behaviour
     * after the user clears the field. */
    snprintf(settings.pwa_login_pass, sizeof settings.pwa_login_pass,
             "%s", p ? p : "");
    settings_save();
}
/* One-tap "I forgot the web password" recovery — clear pwa_login_pass and
 * persist, so the next browser visit hits /set-password and the user can
 * mint a new one. Also clears the textarea so they don't see stale dots
 * from the password-mode field. Username is left alone (you reset the
 * password you forgot, not the username).
 *
 * In WASM build the user is sitting in a logged-in browser session — the
 * cleared password only matters at the auth gate on the next visit, so
 * nothing visibly changes without further action. Kick the session by
 * navigating to /logout so the user immediately sees the set-password
 * form: visible confirmation that the reset took effect. */
static void on_pwa_reset_click(lv_event_t * e) {
    (void)e;
    settings.pwa_login_pass[0] = 0;
    if (ta_pwa_pass) lv_textarea_set_text(ta_pwa_pass, "");
    settings_save();
#ifdef WASM_BUILD
    /* Defer the navigation a tick so the POST in settings_save() has time
     * to actually flush over the wire before the page reloads and tears
     * down the fetch. 250 ms is well above the round-trip on a LAN. */
    extern void wasm_redirect_after(const char * url, int ms);
    wasm_redirect_after("/logout", 250);
#endif
}
/* Primary LAN IPv4 of this Toon — so the Web Access modal can spell out the
 * real browser URL instead of letting users guess.
 *
 * Deliberately avoids getifaddrs()/freeifaddrs()/inet_ntop(): those are
 * versioned GLIBC_2.4 symbols (getifaddrs is even a WEAK one) that the Toon 1's
 * ancient firmware glibc does NOT export — referencing them aborts the whole
 * binary at load time with a "version GLIBC_2.4 not defined" relocation error
 * (this regressed Toon 1 in v0.9.47, see commit baf9837). Instead use the
 * UDP-connect + getsockname() trick: connecting a datagram socket sends no
 * packets but makes the kernel pick the default-route source address (the
 * wlan0/eth0 IP we want), which we read back and format with inet_ntoa()
 * (GLIBC_2.0 — present everywhere). */
static void ft_lan_ip(char * out, size_t n) {
    out[0] = 0;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(53);
    /* Any off-link address works; no traffic is generated by a UDP connect.
     * 8.8.8.8 just forces selection of the default-route interface. */
    dst.sin_addr.s_addr = inet_addr("8.8.8.8");
    struct sockaddr_in src;
    socklen_t slen = sizeof src;
    if (connect(s, (struct sockaddr *)&dst, sizeof dst) == 0 &&
        getsockname(s, (struct sockaddr *)&src, &slen) == 0 &&
        src.sin_addr.s_addr != 0) {
        snprintf(out, n, "%s", inet_ntoa(src.sin_addr));
    }
    close(s);
}

static void open_web_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open(tr("Webtoegang", "Web Access"), 540);
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    int y = 70;

    /* Headline: the web UI is on :10081 — NOT :10080 (that's the stock Toon API
     * and shows a blank page). Spell out this Toon's address so nobody guesses. */
    char _ip[40]; ft_lan_ip(_ip, sizeof _ip);
    char _url[72];
    if (_ip[0]) snprintf(_url, sizeof _url, "http://%s:10081", _ip);
    else        snprintf(_url, sizeof _url, "http://<toon-ip>:10081");
    lv_obj_t * lcap = lv_label_create(p);
    lv_obj_set_style_text_color(lcap, lv_color_hex(0x99aabb), 0);
    lv_obj_set_style_text_font(lcap, SF(14), 0);
    lv_label_set_text(lcap, tr("Openen in een browser op (telefoon/tablet/pc):",
                               "Open in a browser at (phone/tablet/PC):"));
    lv_obj_align(lcap, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    lv_obj_t * lurl = lv_label_create(p);
    lv_obj_set_style_text_color(lurl, lv_color_hex(0x66ccff), 0);
    lv_obj_set_style_text_font(lurl, SF(22), 0);
    lv_label_set_text(lurl, _url);
    lv_obj_align(lurl, LV_ALIGN_TOP_LEFT, SX(4), SY(y + 26));
    y += 90;

    lv_obj_t * r_en = panel_row(p, y, tr("Login vereist (browser)", "Login required (browser)"), NULL);
    row_switch(r_en, settings.pwa_login_enabled, on_pwa_enabled_change);
    y += 82;

    lv_obj_t * lu = lv_label_create(p);
    lv_obj_set_style_text_color(lu, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lu, SF(22), 0);
    lv_label_set_text(lu, tr("Gebruikersnaam:", "Username:"));
    lv_obj_align(lu, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    ta_pwa_user = lv_textarea_create(p);
    lv_obj_set_size(ta_pwa_user, SX(420), SY(44));
    lv_obj_align(ta_pwa_user, LV_ALIGN_TOP_LEFT, SX(260), SY(y - 4));
    lv_textarea_set_one_line(ta_pwa_user, true);
    lv_textarea_set_text(ta_pwa_user, settings.pwa_login_user);
    y += 60;

    lv_obj_t * lp = lv_label_create(p);
    lv_obj_set_style_text_color(lp, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lp, SF(22), 0);
    lv_label_set_text(lp, tr("Wachtwoord:", "Password:"));
    lv_obj_align(lp, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    ta_pwa_pass = lv_textarea_create(p);
    lv_obj_set_size(ta_pwa_pass, SX(420), SY(44));
    lv_obj_align(ta_pwa_pass, LV_ALIGN_TOP_LEFT, SX(260), SY(y - 4));
    lv_textarea_set_one_line(ta_pwa_pass, true);
    lv_textarea_set_password_mode(ta_pwa_pass, true);
    lv_textarea_set_text(ta_pwa_pass, settings.pwa_login_pass);
    y += 64;

    lv_obj_t * b_apply = lv_btn_create(p);
    lv_obj_set_size(b_apply, SX(220), SY(50));
    lv_obj_align(b_apply, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    lv_obj_set_style_bg_color(b_apply, lv_color_hex(0xc06030), 0);
    lv_obj_add_event_cb(b_apply, on_pwa_apply_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * la = lv_label_create(b_apply);
    lv_label_set_text(la, tr("Opslaan", "Save")); lv_obj_center(la);

    /* "Forgot password" / reset button — alongside Save. Clearing
     * the field + Save does the same thing, but a dedicated button is
     * faster to find when the password is genuinely lost. */
    lv_obj_t * b_reset = lv_btn_create(p);
    lv_obj_set_size(b_reset, SX(200), SY(50));
    lv_obj_align(b_reset, LV_ALIGN_TOP_LEFT, SX(244), SY(y));
    lv_obj_set_style_bg_color(b_reset, lv_color_hex(0x444444), 0);
    lv_obj_add_event_cb(b_reset, on_pwa_reset_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lr = lv_label_create(b_reset);
    lv_label_set_text(lr, tr("Wachtwoord resetten", "Reset password")); lv_obj_center(lr);

    /* Note label — explains the empty-password fallback so users aren't
     * surprised when clearing the field doesn't fully disable login. */
    lv_obj_t * note = lv_label_create(p);
    lv_obj_set_style_text_color(note, lv_color_hex(0x99aabb), 0);
    lv_obj_set_style_text_font(note, SF(14), 0);
    lv_label_set_text(note,
        tr("Leeg wachtwoord = volgende browserbezoek vraagt er een in te stellen.\n"
           "Wachtwoord resetten = hetzelfde, in 1 tik (als je het vergeten bent).\n"
           "Zet de schakelaar uit om login volledig uit te schakelen.",
           "Empty password = next browser visit prompts to set one.\n"
           "Reset password = same, in one tap (when you forgot it).\n"
           "Turn the toggle off to disable login entirely."));
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, SX(4), SY(y + 60));
    y += 130;
}

static void on_pin_enabled_change(lv_event_t * e) {
    settings.pin_enabled =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static void on_pin_apply_click(lv_event_t * e) {
    (void)e;
    const char * v = lv_textarea_get_text(ta_pin_code);
    /* Empty code = effectively off (matches pin_is_armed()'s check) — no
     * point treating that as an error. Otherwise enforce digits-only +
     * 4-6 chars to keep the modal numpad consistent. */
    if (v && *v) {
        size_t L = strlen(v);
        int bad = 0;
        for (size_t i = 0; i < L; i++) if (v[i] < '0' || v[i] > '9') { bad = 1; break; }
        if (bad || L < 4 || L > 6) {
            /* Just ignore — UI shouldn't allow this anyway via ta_make_numeric */
            return;
        }
    }
    snprintf(settings.pin_code, sizeof settings.pin_code, "%s", v ? v : "");
    settings_save();
}
static void open_pin_settings_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open(tr("Pincode", "PIN code"), 480);
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    int y = 70;

    lv_obj_t * r_en = panel_row(p, y, tr("Pincode vereist voor wijzigingen", "PIN required for changes"), NULL);
    row_switch(r_en, settings.pin_enabled, on_pin_enabled_change);
    y += 82;

    lv_obj_t * lp = lv_label_create(p);
    lv_obj_set_style_text_color(lp, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lp, SF(22), 0);
    lv_label_set_text(lp, tr("Pincode (4-6 cijfers):", "PIN (4-6 digits):"));
    lv_obj_align(lp, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    ta_pin_code = lv_textarea_create(p); ta_make_numeric(ta_pin_code);
    lv_obj_set_size(ta_pin_code, SX(260), SY(44));
    lv_obj_align(ta_pin_code, LV_ALIGN_TOP_LEFT, SX(260), SY(y - 4));
    lv_textarea_set_one_line(ta_pin_code, true);
    lv_textarea_set_password_mode(ta_pin_code, true);
    lv_textarea_set_max_length(ta_pin_code, sizeof settings.pin_code - 1);
    lv_textarea_set_text(ta_pin_code, settings.pin_code);
    y += 64;

    lv_obj_t * b_apply = lv_btn_create(p);
    lv_obj_set_size(b_apply, SX(220), SY(50));
    lv_obj_align(b_apply, LV_ALIGN_TOP_LEFT, SX(4), SY(y));
    lv_obj_set_style_bg_color(b_apply, lv_color_hex(0xc06030), 0);
    lv_obj_add_event_cb(b_apply, on_pin_apply_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * la = lv_label_create(b_apply);
    lv_label_set_text(la, tr("Opslaan", "Save")); lv_obj_center(la);

    lv_obj_t * note = lv_label_create(p);
    lv_obj_set_style_text_color(note, lv_color_hex(0x99aabb), 0);
    lv_obj_set_style_text_font(note, SF(14), 0);
    lv_label_set_text(note,
        tr("Indien aan, beschermt de pincode: Instellingen openen,\n"
           "thermostaatvoorkeuren (Comfort/Thuis/Slapen/Weg),\n"
           "+/- streefwaarde-aanpassingen en schema/handmatig-schakelaar.\n"
           "Lege pincode = beveiliging blijft open, ook al staat hij aan.",
           "When enabled, the PIN gates: opening Settings,\n"
           "thermostat presets (Comfort/Home/Sleep/Away),\n"
           "+/- setpoint nudges, and schedule/manual toggle.\n"
           "Empty PIN = gate stays open even if toggle is on."));
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, SX(4), SY(y + 60));
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

typedef struct {
    char host[64];
    char user[32];
    char pass[64];
    int port;
} mqtt_thread_params_t;

static void * mqtt_test_thread(void * arg) {
    mqtt_thread_params_t * p = (mqtt_thread_params_t *)arg;
    char err[128] = "";
    int rc = mqtt_test_connection(p->host, p->port, p->user, p->pass, err, sizeof(err));
    free(p);
    pthread_mutex_lock(&g_disc_mtx);
    snprintf(g_test_result, sizeof(g_test_result), "%s: %s",
             rc == 0 ? "OK" : "FAIL", err);
    g_test_done = 1;
    pthread_mutex_unlock(&g_disc_mtx);
    return NULL;
}

static void on_mqtt_test_click(lv_event_t * e) {
    (void)e;
    lv_label_set_text(lbl_mqtt_result, tr("Verbinding testen...", "Testing connection..."));
    mqtt_thread_params_t * p = malloc(sizeof(*p));
    if (!p) return;
    snprintf(p->host, sizeof(p->host), "%s", lv_textarea_get_text(ta_mqtt_host));
    snprintf(p->user, sizeof(p->user), "%s", lv_textarea_get_text(ta_mqtt_user));
    snprintf(p->pass, sizeof(p->pass), "%s", lv_textarea_get_text(ta_mqtt_pass));
    p->port = atoi(lv_textarea_get_text(ta_mqtt_port));
    pthread_t t;
    if (pthread_create(&t, NULL, mqtt_test_thread, p) == 0) {
        pthread_detach(t);
    } else {
        free(p);
    }
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
    mqtt_thread_params_t * p = (mqtt_thread_params_t *)arg;
    pthread_mutex_lock(&g_disc_mtx);
    g_disc_count = 0; g_disc_dirty = 1;
    pthread_mutex_unlock(&g_disc_mtx);
    /* 5 second scan of "#" is enough to catch retained + a couple of
     * ticks of live traffic on a normal home broker. */
    int n = mqtt_discover_topics(p->host, p->port, p->user, p->pass, "#", 5000,
                                 on_disc_topic, NULL);
    free(p);
    pthread_mutex_lock(&g_disc_mtx);
    snprintf(g_test_result, sizeof(g_test_result),
             n < 0 ? tr("Ontdekken mislukt (controleer inlog/host)", "Discovery failed (check creds/host)")
                   : tr("Ontdekken klaar: %d topic%s gezien", "Discovery done: %d topic%s seen"),
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
    lv_label_set_text(lbl_mqtt_result, tr("5s op # abonneren...", "Subscribing to # for 5s..."));
    mqtt_thread_params_t * p = malloc(sizeof(*p));
    if (!p) {
        pthread_mutex_lock(&g_disc_mtx);
        g_disc_running = 0;
        pthread_mutex_unlock(&g_disc_mtx);
        return;
    }
    snprintf(p->host, sizeof(p->host), "%s", lv_textarea_get_text(ta_mqtt_host));
    snprintf(p->user, sizeof(p->user), "%s", lv_textarea_get_text(ta_mqtt_user));
    snprintf(p->pass, sizeof(p->pass), "%s", lv_textarea_get_text(ta_mqtt_pass));
    p->port = atoi(lv_textarea_get_text(ta_mqtt_port));
    pthread_t t;
    if (pthread_create(&t, NULL, mqtt_discover_thread, p) == 0) {
        pthread_detach(t);
    } else {
        free(p);
        pthread_mutex_lock(&g_disc_mtx);
        g_disc_running = 0;
        pthread_mutex_unlock(&g_disc_mtx);
    }
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
        snprintf(snap[i], sizeof(snap[0]), "%.*s", (int)sizeof(snap[0]) - 1, g_disc_topics[i]);
    pthread_mutex_unlock(&g_disc_mtx);

    int y = 0;
    for (int i = 0; i < n; i++) {
        lv_obj_t * row = lv_obj_create(topics_container);
        lv_obj_set_size(row, SX(760), SY(36));
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
        lv_obj_set_style_text_color(lbl_topics[i], lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_set_style_text_font(lbl_topics[i], SF(18), 0);
        lv_label_set_text(lbl_topics[i], snap[i]);
        lv_obj_align(lbl_topics[i], LV_ALIGN_LEFT_MID, SX(36), 0);

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
        tr("Toegepast - abonnee verbindt opnieuw met de nieuwe instellingen.",
           "Applied - subscriber reconnecting with new settings."));
}

static void on_mqtt_enabled_change(lv_event_t * e) {
    settings.mqtt_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
}

static void open_mqtt_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open("MQTT", 760);
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    int y = 70;

    /* Enabled toggle — lets MQTT be turned off entirely (the broker subscriber
     * otherwise always runs whenever a host is configured). */
    lv_obj_t * r_en = panel_row(p, y, tr("MQTT inschakelen", "MQTT enabled"), NULL);
    row_switch(r_en, settings.mqtt_enabled, on_mqtt_enabled_change);
    y += 82;

    /* Host */
    lv_obj_t * lbl_h = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_h, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_h, SF(22), 0);
    lv_label_set_text(lbl_h, tr("Broker-host:", "Broker host:"));
    lv_obj_align(lbl_h, LV_ALIGN_TOP_LEFT, SX(4), y);
    ta_mqtt_host = lv_textarea_create(p); ta_make_numeric(ta_mqtt_host);
    lv_obj_set_size(ta_mqtt_host, SX(380), SY(44));
    lv_obj_align(ta_mqtt_host, LV_ALIGN_TOP_LEFT, SX(240), y - 4);
    lv_textarea_set_one_line(ta_mqtt_host, true);
    lv_textarea_set_text(ta_mqtt_host, settings.mqtt_host);
    /* Port — separate text area on the right */
    lv_obj_t * lbl_p = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_p, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_p, SF(22), 0);
    lv_label_set_text(lbl_p, tr("Poort:", "Port:"));
    lv_obj_align(lbl_p, LV_ALIGN_TOP_LEFT, SX(640), y);
    ta_mqtt_port = lv_textarea_create(p); ta_make_numeric(ta_mqtt_port);
    lv_obj_set_size(ta_mqtt_port, SX(100), SY(44));
    lv_obj_align(ta_mqtt_port, LV_ALIGN_TOP_LEFT, SX(720), y - 4);
    lv_textarea_set_one_line(ta_mqtt_port, true);
    char portbuf[8]; snprintf(portbuf, sizeof(portbuf), "%d",
                              settings.mqtt_port ? settings.mqtt_port : 1883);
    lv_textarea_set_text(ta_mqtt_port, portbuf);
    y += 60;

    /* User */
    lv_obj_t * lbl_u = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_u, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_u, SF(22), 0);
    lv_label_set_text(lbl_u, tr("Gebruiker (opt):", "User (opt):"));
    lv_obj_align(lbl_u, LV_ALIGN_TOP_LEFT, SX(4), y);
    ta_mqtt_user = lv_textarea_create(p);
    lv_obj_set_size(ta_mqtt_user, SX(380), SY(44));
    lv_obj_align(ta_mqtt_user, LV_ALIGN_TOP_LEFT, SX(240), y - 4);
    lv_textarea_set_one_line(ta_mqtt_user, true);
    lv_textarea_set_text(ta_mqtt_user, settings.mqtt_user);
    y += 60;

    /* Pass */
    lv_obj_t * lbl_pw = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_pw, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_pw, SF(22), 0);
    lv_label_set_text(lbl_pw, tr("Wachtwoord (opt):", "Pass (opt):"));
    lv_obj_align(lbl_pw, LV_ALIGN_TOP_LEFT, SX(4), y);
    ta_mqtt_pass = lv_textarea_create(p);
    lv_obj_set_size(ta_mqtt_pass, SX(380), SY(44));
    lv_obj_align(ta_mqtt_pass, LV_ALIGN_TOP_LEFT, SX(240), y - 4);
    lv_textarea_set_one_line(ta_mqtt_pass, true);
    lv_textarea_set_password_mode(ta_mqtt_pass, true);
    lv_textarea_set_text(ta_mqtt_pass, settings.mqtt_pass);
    y += 60;

    /* Test / Discover / Apply buttons */
    lv_obj_t * b_test = lv_btn_create(p);
    lv_obj_set_size(b_test, SX(220), SY(50));
    lv_obj_align(b_test, LV_ALIGN_TOP_LEFT, SX(4), y);
    lv_obj_set_style_bg_color(b_test, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(b_test, on_mqtt_test_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl1 = lv_label_create(b_test);
    lv_label_set_text(bl1, tr("Verbinding testen", "Test connection")); lv_obj_center(bl1);

    lv_obj_t * b_disc = lv_btn_create(p);
    lv_obj_set_size(b_disc, SX(250), SY(50));
    lv_obj_align(b_disc, LV_ALIGN_TOP_LEFT, SX(240), y);
    lv_obj_set_style_bg_color(b_disc, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(b_disc, on_mqtt_discover_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl2 = lv_label_create(b_disc);
    lv_label_set_text(bl2, tr("Topics ontdekken (5s)", "Discover topics (5s)")); lv_obj_center(bl2);

    lv_obj_t * b_apply = lv_btn_create(p);
    lv_obj_set_size(b_apply, SX(200), SY(50));
    lv_obj_align(b_apply, LV_ALIGN_TOP_LEFT, SX(500), y);
    lv_obj_set_style_bg_color(b_apply, lv_color_hex(0xc06030), 0);
    lv_obj_add_event_cb(b_apply, on_mqtt_apply_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl3 = lv_label_create(b_apply);
    lv_label_set_text(bl3, tr("Toepassen + herstart", "Apply + restart")); lv_obj_center(bl3);
    y += 60;

    /* Result line */
    lbl_mqtt_result = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_mqtt_result, lv_color_hex(0xa8c4dc), 0);
    lv_obj_set_style_text_font(lbl_mqtt_result, SF(18), 0);
    lv_obj_set_width(lbl_mqtt_result, SX(770));
    lv_label_set_long_mode(lbl_mqtt_result, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_mqtt_result, LV_ALIGN_TOP_LEFT, SX(4), y);
    lv_label_set_text(lbl_mqtt_result, tr("Klaar. Test de broker of kies Ontdekken.", "Ready. Test the broker or hit Discover."));
    y += 40;

    /* Discovered-topic checklist container — scrollable column */
    topics_container = lv_obj_create(p);
    lv_obj_set_size(topics_container, SX(770), SY(280));
    lv_obj_align(topics_container, LV_ALIGN_TOP_LEFT, SX(4), y);
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
    if (scr_root && scr_light_mode == settings.home_light_mode) return scr_root;
    if (scr_root) {
        lv_obj_del(scr_root);
        scr_root = NULL;
    }
    scr_light_mode = settings.home_light_mode;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(THEME_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Header — permanently fixed at top so close button never scrolls away */
    int x_w = (DISP_VER < 600) ? 60 : 70;
    int x_h = (DISP_VER < 600) ? 46 : 50;
    lv_obj_t * btn_close = lv_btn_create(scr_root);
    lv_obj_set_size(btn_close, x_w, x_h);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, SX(-12), SY(12));
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(btn_close, 12, 0);
    lv_obj_set_ext_click_area(btn_close, 25);
    lv_obj_add_event_cb(btn_close, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * xl = lv_label_create(btn_close);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(xl, SF(22), 0);
    lv_obj_center(xl);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, tr("Instellingen", "Settings"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(24), SY(24));

    /* Scrollable container for category tiles below the header */
    scr_tiles_cont = lv_obj_create(scr_root);
    lv_obj_set_pos(scr_tiles_cont, 0, SY(104));
    lv_obj_set_size(scr_tiles_cont, LV_HOR_RES, LV_VER_RES - SY(104));
    lv_obj_set_style_bg_opa(scr_tiles_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scr_tiles_cont, 0, 0);
    lv_obj_set_style_pad_all(scr_tiles_cont, 0, 0);
    lv_obj_set_style_pad_bottom(scr_tiles_cont, SY(24), 0);
    lv_obj_add_flag(scr_tiles_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scr_tiles_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scr_tiles_cont, LV_SCROLLBAR_MODE_AUTO);

    /* Category tiles in a compact 4-column grid, sorted alphabetically (CR9) */
    const int COLS = 4, MX = 20, TOP = 8, GAPX = 14, GAPY = 12;
    #define GX(i) (SX(MX + ((i) % COLS) * (SETT_TILE_W + GAPX)))
    #define GY(i) (SY(TOP + ((i) / COLS) * (SETT_TILE_H + GAPY)))
    int n = 0;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_CHARGE, tr("Adapters", "Adapters"),
              tr("meter & ketel", "meter & boiler"), open_adapters); n++;
    make_tile(GX(n), GY(n), &icon_trash, NULL, tr("Afval", "Waste"),
              tr("ophaalmeldingen", "pickup alerts"), open_waste_modal); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_LIST, tr("Agenda", "Calendar"),
              tr("HA + iCal-afspraken", "HA + iCal events"), open_calendar_modal); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_IMAGE, tr("Animaties", "Animations"),
              tr("effecten & feest", "effects & party"), open_anim_modal); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_LOOP, tr("Auto-wisselen", "Auto-rotate"),
              tr("tegel wisselen", "cycle tile"), open_rotate_modal); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_COPY, tr("Clientmodus", "Client mode"),
              tr("een master-Toon spiegelen", "mirror a master Toon"), open_client_modal); n++;
    if (tile_slots_integration_by_id("crypto") != NULL) {
        make_tile(GX(n), GY(n), NULL, LV_SYMBOL_LIST, "Crypto",
                  tr("kies munten", "pick coins"), open_crypto_picker); n++;
    }
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_EDIT, tr("Dim indeling", "Dim layout"),
              tr("dim-blokken (opt-in)", "dim blocks (opt-in)"), open_dim_layout_editor); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_HOME, "Domoticz",
              tr("licht & zonwering", "lights & blinds"), open_domoticz); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_CHARGE, tr("Energie", "Energy"),
              tr("grafieken & verbruik", "charts & usage"), open_energy_stats); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_HOME, "Home Assistant",
              tr("host, token, entiteiten", "host, token, entities"), open_ha_entities_modal); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_EDIT, tr("Indeling", "Layout"),
              tr("tegelindeling bewerken", "tile layout editor"), open_layout_editor); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_PLUS, tr("Integraties", "Integrations"),
              tr("P1 / water / vent / HA", "P1 / water / vent / HA"), open_integrations_modal); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_DOWNLOAD, "Marketplace",
              tr("integraties installeren", "install integrations"), open_marketplace); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_WIFI, "MQTT",
              tr("broker + topics", "broker + topics"), open_mqtt_modal); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_LIST, tr("Nieuwslezer", "Newsreader"),
              tr("RSS-koppenticker", "RSS headlines ticker"), open_news_modal); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_GPS, "OT Bridge",
              tr("uit / proxy / draadloos", "off / proxy / wireless"), open_otbridge_modal); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_OK, tr("Pincode", "PIN code"),
              tr("wijzigingen vergrendelen", "lock changes"), open_pin_settings_modal); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_LIST, tr("Programma", "Schedule"),
              tr("weekschema thermostaat", "weekly thermostat schedule"), open_program_screen); n++;
    make_tile(GX(n), GY(n), &icon_wx_sun, NULL, tr("Scherm", "Display"),
              tr("scherminstellingen", "display settings"), open_display_modal); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_EYE_CLOSE, tr("Schoonmaken", "Clean"),
              tr("30 s schermvergrendeling", "30 s screen lock"), open_clean_modal); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_HOME, tr("Tegels", "Tiles"),
              tr("starttegels toewijzen", "assign home tiles"), open_tile_slots_modal); n++;
    make_tile(GX(n), GY(n), &icon_chart, NULL, tr("Temperatuurlog", "Temperature log"),
              tr("24-uurs grafiek", "24-hour chart"), open_templogger); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_REFRESH, tr("UI herstarten", "Restart UI"),
              tr("instellingen herladen", "reload settings"), open_restart_confirm); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_POWER, tr("UI-modus", "UI mode"),
              tr("freetoon vs qt-gui", "freetoon vs qt-gui"), open_uimode_modal); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_DOWNLOAD, tr("Update", "Update"),
              tr("versie & bijwerken", "version & update"), open_about_modal); n++;
    make_tile(GX(n), GY(n), &icon_flame, NULL, tr("Verwarming", "Heating"),
              tr("correctie, keteltype", "offset, boiler type"), open_heating_modal); n++;
    make_tile(GX(n), GY(n), &icon_radiator, NULL, tr("Voorkeuren", "Presets"),
              tr("schema-streefwaarden", "scheme setpoints"), open_presets_modal); n++;
    make_tile(GX(n), GY(n), &icon_wx_cloud, NULL, tr("Weer", "Weather"),
              tr("weer op dimscherm", "weather on dim"), open_weather_modal); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_WIFI, "WiFi",
              tr("zoeken & verbinden", "scan & connect"), open_wifi); n++;
    make_tile(GX(n), GY(n), NULL, LV_SYMBOL_SETTINGS, "Z-Wave",
              tr("ingebouwde apparaten", "built-in devices"), open_zwave); n++;
    #undef GX
    #undef GY

    return scr_root;
}
