/*
 * Devices page (HA) — opened via the home left button / swipe-right from home
 * when Home Assistant is the active backend. Shows the user's unified device
 * list (ha_devices[]): lights, covers, switches, scripts and scenes, grouped
 * by type, each with type-appropriate controls. State is polled by ha_thread;
 * actions are fire-and-forget. (Domoticz has its own screen_domoticz.c, which
 * the home button opens instead when Domoticz is the active backend.)
 *
 * The row list is rebuilt on every SCREEN_LOADED so devices added/removed in
 * Settings → Devices show up on re-entry without a restart.
 */
#include "screens.h"
#include "display.h"
#include "homeassistant.h"
#include "icons.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define COL_BG        0x0e1a2a
#define COL_CARD      0x1a2940
#define COL_TEXT_HI   0xffffff
#define COL_TEXT_DIM  0x88aabb
#define COL_ON        0xffcc44
#define COL_OFF       0x3a4658
#define COL_OFFLINE   0x6e6e6e
#define COL_ACCENT    0x44aaff

/* Per-row widgets, kept so refresh_cb can repaint state without rebuilding.
 * dev_idx maps the row back to ha_devices[]. */
typedef struct {
    int        dev_idx;
    lv_obj_t * lbl_state;  /* "on 80%" / "open 50%" / … */
    lv_obj_t * btn;        /* primary toggle/run button (light/switch/script/scene) */
    lv_obj_t * btn_lbl;
    lv_obj_t * slider;     /* brightness / cover position (light/cover) or NULL */
    lv_obj_t * chips;      /* input_select: flex-wrap of option buttons, or NULL */
} dev_row_t;

static lv_obj_t   * scr_root  = NULL;
static lv_obj_t   * list      = NULL;   /* scrollable column of cards */
static dev_row_t    rows[HA_DEVICE_MAX];
static int          row_count = 0;
static lv_obj_t   * empty_hint = NULL;  /* shown when no devices / HA off */
static lv_timer_t * refresh_timer = NULL;

/* ui_pop deferred via lv_async_call — see the original lights screen. */
static void back_async(void * unused) { (void)unused; ui_pop(); }
static void on_back(lv_event_t * e) { (void)e; lv_async_call(back_async, NULL); }

/* ---- action handlers (user_data = device index) ---- */
static int ev_idx(lv_event_t * e) { return (int)(intptr_t)lv_event_get_user_data(e); }

static void on_toggle(lv_event_t * e)  { ha_dev_toggle(ev_idx(e)); }
static void on_run(lv_event_t * e)     { ha_dev_run(ev_idx(e)); }
static void on_bright(lv_event_t * e) {
    ha_dev_brightness(ev_idx(e), lv_slider_get_value(lv_event_get_target(e)));
}
static void on_cover_pos(lv_event_t * e) {
    ha_dev_position(ev_idx(e), lv_slider_get_value(lv_event_get_target(e)));
}
static void on_cover_open (lv_event_t * e) { ha_dev_cover(ev_idx(e), "open");  }
static void on_cover_stop (lv_event_t * e) { ha_dev_cover(ev_idx(e), "stop");  }
static void on_cover_close(lv_event_t * e) { ha_dev_cover(ev_idx(e), "close"); }

/* input_select: tap an option chip to select it. The chip's user_data packs
 * the device index in the high bits and the option index in the low 8 bits;
 * we read the chip's own label for the option name. */
static void on_select_chip(lv_event_t * e) {
    lv_obj_t * chip = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t * lbl = lv_obj_get_child(chip, 0);
    if (lbl) ha_dev_select_set(idx, lv_label_get_text(lbl));
}

/* ---- row builders ---- */
static lv_obj_t * make_card(int h) {
    lv_obj_t * c = lv_obj_create(list);
    lv_obj_set_size(c, lv_pct(100), h);
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}
static lv_obj_t * card_name(lv_obj_t * c, const char * txt, lv_align_t a, int x, int y) {
    lv_obj_t * l = lv_label_create(c);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(l, SF(18), 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, a, x, y);
    return l;
}
static lv_obj_t * mk_btn(lv_obj_t * c, const char * txt, uint32_t col, int w, int h,
                         lv_align_t a, int x, int y, lv_event_cb_t cb, int idx) {
    lv_obj_t * b = lv_btn_create(c);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, a, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_t * l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, SF(14), 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}
static lv_obj_t * mk_slider(lv_obj_t * c, uint32_t ind_col, lv_event_cb_t cb, int idx) {
    lv_obj_t * s = lv_slider_create(c);
    lv_obj_set_size(s, lv_pct(94), 14);
    lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_slider_set_range(s, 0, 100);
    lv_slider_set_value(s, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x223344), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(ind_col), LV_PART_INDICATOR);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, (void *)(intptr_t)idx);
    return s;
}
static lv_obj_t * mk_state(lv_obj_t * c, lv_align_t a, int x, int y) {
    lv_obj_t * l = lv_label_create(c);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(l, SF(14), 0);
    lv_label_set_text(l, "--");
    lv_obj_align(l, a, x, y);
    return l;
}

/* Build the card for ha_devices[i] and record its row widgets. */
static void build_dev_row(int i) {
    if (row_count >= HA_DEVICE_MAX) return;
    ha_device_t * D = &ha_devices[i];
    dev_row_t * R = &rows[row_count];
    memset(R, 0, sizeof *R);
    R->dev_idx = i;

    switch (D->type) {
    case HADEV_COVER: {
        lv_obj_t * c = make_card(86);
        card_name(c, D->name, LV_ALIGN_TOP_LEFT, 4, 2);
        R->lbl_state = mk_state(c, LV_ALIGN_TOP_LEFT, 4, 26);
        mk_btn(c, "Open",  0x2e6e3a, 58, 30, LV_ALIGN_TOP_RIGHT, -126, 2, on_cover_open,  i);
        mk_btn(c, "Stop",  0x6a5424, 58, 30, LV_ALIGN_TOP_RIGHT,  -64, 2, on_cover_stop,  i);
        mk_btn(c, "Close", 0x6e3a3a, 58, 30, LV_ALIGN_TOP_RIGHT,   -2, 2, on_cover_close, i);
        R->slider = mk_slider(c, COL_ACCENT, on_cover_pos, i);
        break;
    }
    case HADEV_LIGHT: {
        lv_obj_t * c = make_card(84);
        card_name(c, D->name, LV_ALIGN_TOP_LEFT, 4, 2);
        R->lbl_state = mk_state(c, LV_ALIGN_TOP_LEFT, 4, 26);
        R->btn = mk_btn(c, "Off", COL_OFF, 64, 32, LV_ALIGN_TOP_RIGHT, -2, 2, on_toggle, i);
        R->btn_lbl = lv_obj_get_child(R->btn, 0);
        R->slider = mk_slider(c, COL_ON, on_bright, i);
        break;
    }
    case HADEV_SWITCH: {
        lv_obj_t * c = make_card(56);
        card_name(c, D->name, LV_ALIGN_LEFT_MID, 4, -8);
        R->lbl_state = mk_state(c, LV_ALIGN_LEFT_MID, 4, 12);
        R->btn = mk_btn(c, "Off", COL_OFF, 64, 36, LV_ALIGN_RIGHT_MID, -2, 0, on_toggle, i);
        R->btn_lbl = lv_obj_get_child(R->btn, 0);
        break;
    }
    case HADEV_SELECT: {
        /* Flex-column card (name + a flex-wrap of all option chips). The card
         * sizes to its content, so every option is reachable — flowing onto a
         * 2nd (or further) row when they don't fit on one. */
        lv_obj_t * c = lv_obj_create(list);
        lv_obj_set_width(c, lv_pct(100));
        lv_obj_set_height(c, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_radius(c, 12, 0);
        lv_obj_set_style_pad_all(c, 8, 0);
        lv_obj_set_style_pad_row(c, 6, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        card_name(c, D->name, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t * box = lv_obj_create(c);
        lv_obj_remove_style_all(box);
        lv_obj_set_width(box, lv_pct(100));
        lv_obj_set_height(box, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(box, 6, 0);
        lv_obj_set_style_pad_column(box, 6, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        R->chips = box;

        if (D->options[0]) {
            char opts[256]; snprintf(opts, sizeof opts, "%s", D->options);
            char * save = NULL;
            for (char * tok = strtok_r(opts, "|", &save); tok; tok = strtok_r(NULL, "|", &save)) {
                lv_obj_t * chip = lv_btn_create(box);
                lv_obj_set_height(chip, 36);
                lv_obj_set_width(chip, LV_SIZE_CONTENT);
                lv_obj_set_style_radius(chip, 8, 0);
                lv_obj_set_style_pad_hor(chip, 12, 0);
                lv_obj_set_style_bg_color(chip, lv_color_hex(COL_OFF), 0);
                lv_obj_add_event_cb(chip, on_select_chip, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                lv_obj_t * cl = lv_label_create(chip);
                lv_obj_set_style_text_color(cl, lv_color_hex(0xffffff), 0);
                lv_obj_set_style_text_font(cl, SF(15), 0);
                lv_label_set_text(cl, tok);
                lv_obj_center(cl);
            }
        } else {
            lv_obj_t * ph = lv_label_create(box);
            lv_obj_set_style_text_color(ph, lv_color_hex(COL_TEXT_DIM), 0);
            lv_obj_set_style_text_font(ph, SF(15), 0);
            lv_label_set_text(ph, "(no options reported yet)");
        }
        break;
    }
    default: {   /* HADEV_SCRIPT / HADEV_SCENE — stateless Run button */
        lv_obj_t * c = make_card(56);
        card_name(c, D->name, LV_ALIGN_LEFT_MID, 4, 0);
        R->btn = mk_btn(c, D->type == HADEV_SCENE ? "Activate" : "Run",
                        0x2e5e8a, 96, 38, LV_ALIGN_RIGHT_MID, -2, 0, on_run, i);
        break;
    }
    }
    row_count++;
}

static void add_header(const char * txt) {
    lv_obj_t * h = lv_label_create(list);
    lv_obj_set_style_text_color(h, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(h, SF(16), 0);
    lv_label_set_text(h, txt);
}

/* Clear + rebuild the device list from ha_devices[], grouped by type. */
static void rebuild_rows(void) {
    if (!list) return;
    lv_obj_clean(list);
    row_count = 0;

    ha_devices_sync_dz();
    int any_backend = settings.enable_ha || settings.enable_domoticz;
    int show = any_backend && ha_device_count > 0;
    if (empty_hint) {
        if (show) lv_obj_add_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_clear_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);
    }
    if (!show) {
        lv_label_set_text(empty_hint,
            any_backend
                ? "No devices yet.\nAdd Home Assistant or Domoticz devices in\nSettings  >  Devices."
                : "No backend enabled.\nEnable Home Assistant and/or Domoticz in\nSettings  >  Integrations.");
        return;
    }

    const struct { int type; const char * hdr; } groups[] = {
        { HADEV_LIGHT,  "Lights"  }, { HADEV_COVER,  "Covers"  },
        { HADEV_SWITCH, "Switches"}, { HADEV_SELECT, "Selects" },
        { HADEV_SCRIPT, "Scripts" }, { HADEV_SCENE,  "Scenes"  },
    };
    for (size_t g = 0; g < sizeof(groups)/sizeof(groups[0]); g++) {
        int any = 0;
        for (int i = 0; i < ha_device_count; i++) {
            if (ha_devices[i].type != groups[g].type) continue;
            if (!any) { add_header(groups[g].hdr); any = 1; }
            build_dev_row(i);
        }
    }
}

static void on_scr_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) {
        rebuild_rows();                       /* pick up Settings → Devices edits */
        if (refresh_timer) lv_timer_resume(refresh_timer);
    } else if (c == LV_EVENT_SCREEN_UNLOADED) {
        if (refresh_timer) lv_timer_pause(refresh_timer);
    }
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    ha_devices_sync_dz();            /* pull live Domoticz state into ha_devices[] */
    for (int r = 0; r < row_count; r++) {
        dev_row_t * R = &rows[r];
        ha_device_t * D = &ha_devices[R->dev_idx];
        if (D->type == HADEV_COVER) {
            if (R->lbl_state) {
                if (!D->available) lv_label_set_text(R->lbl_state, "offline");
                else if (D->position >= 0)
                    lv_label_set_text_fmt(R->lbl_state, "%s  %d%%",
                                          D->state[0] ? D->state : "cover", D->position);
                else
                    lv_label_set_text(R->lbl_state, D->state[0] ? D->state : "--");
            }
            if (R->slider && D->position >= 0)
                lv_slider_set_value(R->slider, D->position, LV_ANIM_OFF);
        } else if (D->type == HADEV_LIGHT || D->type == HADEV_SWITCH) {
            const char * txt; uint32_t bcol; const char * blbl;
            if (!D->available)   { txt = "offline"; bcol = COL_OFFLINE; blbl = "Offline"; }
            else if (D->on)      { bcol = COL_ON;  blbl = "On";  txt = NULL; }
            else                 { txt = "off"; bcol = COL_OFF; blbl = "Off"; }
            if (R->lbl_state) {
                if (D->available && D->on) {
                    if (D->type == HADEV_LIGHT && D->brightness > 0)
                        lv_label_set_text_fmt(R->lbl_state, "on  %d%%", D->brightness * 100 / 255);
                    else lv_label_set_text(R->lbl_state, "on");
                    lv_obj_set_style_text_color(R->lbl_state, lv_color_hex(COL_ON), 0);
                } else if (txt) {
                    lv_label_set_text(R->lbl_state, txt);
                    lv_obj_set_style_text_color(R->lbl_state,
                        lv_color_hex(D->available ? COL_TEXT_DIM : COL_OFFLINE), 0);
                }
            }
            if (R->btn)     lv_obj_set_style_bg_color(R->btn, lv_color_hex(bcol), 0);
            if (R->btn_lbl) lv_label_set_text(R->btn_lbl, blbl);
            if (R->slider) {
                if (!D->available) lv_obj_add_state(R->slider, LV_STATE_DISABLED);
                else {
                    lv_obj_clear_state(R->slider, LV_STATE_DISABLED);
                    if (D->on && D->brightness > 0)
                        lv_slider_set_value(R->slider, D->brightness * 100 / 255, LV_ANIM_OFF);
                    else if (!D->on)
                        lv_slider_set_value(R->slider, 0, LV_ANIM_OFF);
                }
            }
        } else if (D->type == HADEV_SELECT) {
            if (R->chips) {                       /* highlight the active option chip */
                uint32_t nc = lv_obj_get_child_cnt(R->chips);
                for (uint32_t k = 0; k < nc; k++) {
                    lv_obj_t * chip = lv_obj_get_child(R->chips, k);
                    lv_obj_t * cl = lv_obj_get_child(chip, 0);
                    if (!cl) continue;
                    int sel = (strcmp(lv_label_get_text(cl), D->state) == 0);
                    lv_obj_set_style_bg_color(chip, lv_color_hex(sel ? 0x2e6e3a : COL_OFF), 0);
                }
            }
        }
    }
}

lv_obj_t * screen_lights_create(void) {
    /* Singleton scaffold; rows are (re)built on SCREEN_LOADED. */
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_LOADED,   NULL);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_UNLOADED, NULL);
    memset(rows, 0, sizeof(rows));

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, "Devices");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 16);

    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, 110, 46);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -16, 12);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x3a4658), 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_ext_click_area(back, 20);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_label_set_text(bl, "< Back");
    lv_obj_center(bl);

    /* Scrollable card column. */
    list = lv_obj_create(scr_root);
    lv_obj_set_size(list, DISP_HOR - 32, DISP_VER - SY(78));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, SY(70));
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);

    /* Centered hint for the empty / HA-off state (sibling of the list). */
    empty_hint = lv_label_create(scr_root);
    lv_obj_set_style_text_color(empty_hint, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(empty_hint, SF(22), 0);
    lv_obj_set_width(empty_hint, DISP_HOR - 120);
    lv_label_set_long_mode(empty_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(empty_hint, "");
    lv_obj_align(empty_hint, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);

    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    rebuild_rows();
    refresh_cb(refresh_timer);
    return scr_root;
}
