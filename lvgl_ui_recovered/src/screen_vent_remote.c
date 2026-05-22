/*
 * Vent remote screen — virtual-remote buttons (Away / Low / Med / High /
 * Auto / Timers) plus a live status row at the top. Reachable from the
 * Vent tile on the home screen. The "Advanced" button opens the full
 * settings list.
 */
#include "screens.h"
#include "display.h"
#include "ventilation.h"
#include <stdio.h>

static lv_obj_t * scr_root = NULL;
static lv_obj_t * lbl_status;
static lv_timer_t * refresh_timer = NULL;

static void on_back(lv_event_t * e) { (void)e; ui_pop(); }

static void on_cmd_btn(lv_event_t * e) {
    const char * cmd = (const char *)lv_event_get_user_data(e);
    if (cmd) vent_send_vremote(cmd);
}

static void on_open_advanced(lv_event_t * e) {
    (void)e;
    ui_push(screen_vent_advanced_create());
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (vent_state.connected) {
        lv_label_set_text_fmt(lbl_status,
            "Setpoint %d%%   Exh %d%%   Fan %d rpm   Mode %s",
            vent_state.speed_pct, vent_state.exh_fan_pct,
            vent_state.fan_rpm,
            vent_state.fan_info[0] ? vent_state.fan_info : "?");
    } else {
        lv_label_set_text(lbl_status, "Vent: disconnected");
    }
}

/* helper to build one rectangular command button */
static void mk_btn(lv_obj_t * parent, int x, int y, int w, int h,
                   const char * txt, const char * cmd, uint32_t bg) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_add_event_cb(b, on_cmd_btn, LV_EVENT_CLICKED, (void *)cmd);
    lv_obj_t * l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
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
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_22, 0);
    lv_label_set_text(bl, "< Back");
    lv_obj_center(bl);

    /* title */
    lv_obj_t * hdr = lv_label_create(scr_root);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_28, 0);
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
    lv_obj_set_style_text_font(advl, &lv_font_montserrat_22, 0);
    lv_label_set_text(advl, "Advanced");
    lv_obj_center(advl);

    /* status line */
    lbl_status = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, 20, 92);
    lv_label_set_text(lbl_status, "(loading...)");

    /* 8 command buttons in a 4x2 grid. The full 4-wide row spans 968px at
     * design size and runs off Toon 1's 800px panel, so SX() shrinks the
     * column pitch + width to fit (identity on Toon 2). */
    const int X0 = SX(40), Y0 = SY(150), BW = SX(220), BH = SY(110), GAP = SX(16);
    mk_btn(scr_root, X0 + 0*(BW+GAP), Y0 + 0*(BH+GAP), BW, BH, "Away",   "away",   0x335577);
    mk_btn(scr_root, X0 + 1*(BW+GAP), Y0 + 0*(BH+GAP), BW, BH, "Low",    "low",    0x44aa66);
    mk_btn(scr_root, X0 + 2*(BW+GAP), Y0 + 0*(BH+GAP), BW, BH, "Medium", "medium", 0xddbb22);
    mk_btn(scr_root, X0 + 3*(BW+GAP), Y0 + 0*(BH+GAP), BW, BH, "High",   "high",   0xff6644);
    mk_btn(scr_root, X0 + 0*(BW+GAP), Y0 + 1*(BH+GAP), BW, BH, "Auto",   "auto",   0x6699cc);
    mk_btn(scr_root, X0 + 1*(BW+GAP), Y0 + 1*(BH+GAP), BW, BH, "10 min", "timer1", 0x886644);
    mk_btn(scr_root, X0 + 2*(BW+GAP), Y0 + 1*(BH+GAP), BW, BH, "20 min", "timer2", 0x886644);
    mk_btn(scr_root, X0 + 3*(BW+GAP), Y0 + 1*(BH+GAP), BW, BH, "30 min", "timer3", 0x886644);

    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}
