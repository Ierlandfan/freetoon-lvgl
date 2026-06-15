/*
 * Vent remote screen — a single vertical SPEED SLIDER (0-100%) that sets the
 * fan directly via the Itho speed= PWM API (vent_set_speed_async), plus a
 * live status row and an "Auto" button to hand control back to the unit's
 * own CO2/auto logic. Reachable by tapping the fan spinner on the Vent tile.
 *
 * Why a slider instead of the old Away/Low/Med/High/timer button grid: on
 * this CVE the vremote preset commands frequently don't actuate the fan,
 * while speed= reliably does — so a continuous slider is both cleaner and
 * the control path that actually works. The full i2c settings list is still
 * one tap away behind "Advanced".
 */
#include "screens.h"
#include "display.h"
#include "ventilation.h"
#include <stdio.h>

static lv_obj_t * scr_root = NULL;
static lv_obj_t * lbl_status;
static lv_obj_t * slider      = NULL;
static lv_obj_t * lbl_pct     = NULL;   /* big number (digits-only 96px font) */
static lv_obj_t * lbl_pct_sign = NULL;  /* "%" in a font that has the glyph */
static lv_timer_t * refresh_timer = NULL;
/* When the user last moved the slider — refresh_cb won't yank it back to the
 * live value for a few seconds, giving the Itho + MQTT time to catch up. */
static uint32_t last_user_ms = 0;

static void on_back(lv_event_t * e) { (void)e; ui_pop(); }

static void on_open_advanced(lv_event_t * e) {
    (void)e;
    ui_push(screen_vent_advanced_create());
}

static void on_auto(lv_event_t * e) {
    (void)e;
    /* Hand control back to the unit's own CO2/auto logic. */
    vent_send_vremote_async("auto");
    last_user_ms = lv_tick_get();
}

/* Live label while dragging (no HTTP). */
static void on_slider_changed(lv_event_t * e) {
    (void)e;
    int v = lv_slider_get_value(slider);
    lv_label_set_text_fmt(lbl_pct, "%d", v);   /* digits only (96px font) */
    last_user_ms = lv_tick_get();
}

/* Commit on release — one HTTP write, off the LVGL thread. */
static void on_slider_released(lv_event_t * e) {
    (void)e;
    int v = lv_slider_get_value(slider);       /* 0..100 */
    vent_set_speed_async(v * 255 / 100);       /* PWM 0..255 */
    last_user_ms = lv_tick_get();
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (vent_state.connected && vent_state.itho_online == 0) {
        lv_label_set_text(lbl_status, "Itho offline (MQTT LWT)");
    } else if (vent_state.connected) {
        lv_label_set_text_fmt(lbl_status,
            "Setpoint %d%%   Exh %d%%   Fan %d rpm   Mode %s",
            vent_state.speed_pct, vent_state.exh_fan_pct,
            vent_state.fan_rpm, vent_mode_label());
    } else {
        lv_label_set_text(lbl_status, "Vent: disconnected");
    }

    /* Reflect the live commanded level onto the slider when the user isn't
     * actively touching it and hasn't just set it (4 s grace). exh_fan_pct is
     * the Itho's "Ventilation level (%)" — what the fan is commanded to. */
    if (slider && lbl_pct) {
        int dragging = lv_obj_has_state(slider, LV_STATE_PRESSED);
        int grace    = (lv_tick_get() - last_user_ms) < 4000;
        if (!dragging && !grace && vent_state.connected
                                && vent_state.itho_online != 0) {
            int live = vent_state.exh_fan_pct;
            if (live < 0)   live = 0;
            if (live > 100) live = 100;
            if (lv_slider_get_value(slider) != live) {
                lv_slider_set_value(slider, live, LV_ANIM_OFF);
                lv_label_set_text_fmt(lbl_pct, "%d", live);
            }
        }
    }
}

lv_obj_t * screen_vent_remote_create(void) {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x0f1a2a), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* back */
    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, 110, 60);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x223344), 0);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(bl, SF(22), 0);
    lv_label_set_text(bl, "< Back");
    lv_obj_center(bl);

    /* title */
    lv_obj_t * hdr = lv_label_create(scr_root);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(hdr, SF(28), 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 150, 22);
    lv_label_set_text(hdr, "Ventilation");

    /* Advanced */
    lv_obj_t * adv = lv_btn_create(scr_root);
    lv_obj_set_size(adv, 150, 60);
    lv_obj_align(adv, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_set_style_bg_color(adv, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(adv, 12, 0);
    lv_obj_add_event_cb(adv, on_open_advanced, LV_EVENT_CLICKED, NULL);
    lv_obj_t * advl = lv_label_create(adv);
    lv_obj_set_style_text_color(advl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(advl, SF(22), 0);
    lv_label_set_text(advl, "Advanced");
    lv_obj_center(advl);

    /* status line */
    lbl_status = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_status, SF(18), 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, 20, 92);
    lv_label_set_text(lbl_status, "(loading...)");

    /* ---- vertical speed slider (left-centre) ---- */
    /* Start at the live commanded level so the screen opens correct. */
    int init_pct = vent_state.connected ? vent_state.exh_fan_pct : 0;
    if (init_pct < 0)   init_pct = 0;
    if (init_pct > 100) init_pct = 100;

    slider = lv_slider_create(scr_root);
    lv_obj_set_size(slider, SX(110), SY(360));
    lv_obj_align(slider, LV_ALIGN_LEFT_MID, SX(150), SY(35));
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, init_pct, LV_ANIM_OFF);
    /* Fat track + knob so it's easy to grab with a finger. */
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x1d2c40), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3a9bdc), LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, SX(16), LV_PART_MAIN);
    lv_obj_set_style_radius(slider, SX(16), LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(slider, SX(10), LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, on_slider_changed,  LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, on_slider_released, LV_EVENT_RELEASED,      NULL);

    /* big live % readout (right of the slider). The 96px font is digits-only
     * (no '%' glyph), so the number and the '%' sign are separate labels. */
    lbl_pct = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_pct, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_pct, SF(96), 0);
    lv_obj_align(lbl_pct, LV_ALIGN_CENTER, SX(120), SY(-30));
    lv_label_set_text_fmt(lbl_pct, "%d", init_pct);

    lbl_pct_sign = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_pct_sign, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_pct_sign, SF(40), 0);
    lv_obj_align_to(lbl_pct_sign, lbl_pct, LV_ALIGN_OUT_RIGHT_BOTTOM, SX(6), -SY(18));
    lv_label_set_text(lbl_pct_sign, "%");

    lv_obj_t * cap = lv_label_create(scr_root);
    lv_obj_set_style_text_color(cap, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(cap, SF(22), 0);
    lv_obj_align_to(cap, lbl_pct, LV_ALIGN_OUT_BOTTOM_MID, SX(12), SY(4));
    lv_label_set_text(cap, "fan speed");

    /* Auto button (return to CO2/auto control) */
    lv_obj_t * autob = lv_btn_create(scr_root);
    lv_obj_set_size(autob, SX(220), SY(80));
    lv_obj_align(autob, LV_ALIGN_CENTER, SX(140), SY(110));
    lv_obj_set_style_bg_color(autob, lv_color_hex(0x6699cc), 0);
    lv_obj_set_style_radius(autob, 14, 0);
    lv_obj_add_event_cb(autob, on_auto, LV_EVENT_CLICKED, NULL);
    lv_obj_t * al = lv_label_create(autob);
    lv_obj_set_style_text_color(al, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(al, SF(28), 0);
    lv_label_set_text(al, "Auto");
    lv_obj_center(al);

    last_user_ms = lv_tick_get();    /* don't snap the slider on first frame */
    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}
