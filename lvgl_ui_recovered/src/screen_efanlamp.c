/*
 * screen_efanlamp.c — BLE ceiling fan control screen
 *
 * Layout:
 *   ← Ventilator
 *   status line (connected / disconnected / last source)
 *   ──────────────────────────────────────────────────
 *   [ Uit ][ 1 ][ 2 ][ 3 ][ 4 ][ 5 ][ 6 ]   [ Achteruit ]
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "efanlamp.h"
#include <stdio.h>
#include <string.h>

/* ── colours ──────────────────────────────────────────────────── */
#define COL_BG       0x0e1a2a
#define COL_CARD     0x1a2940
#define COL_TEXT_HI  0xffffff
#define COL_TEXT_DIM 0x88aabb
#define COL_ACCENT   0x4FC3F7
#define COL_BTN      0x1e3550

/* ── state ─────────────────────────────────────────────────────── */
static lv_obj_t   * scr_root      = NULL;
static lv_obj_t   * lbl_status    = NULL;
static lv_timer_t * refresh_timer = NULL;

#define FAN_BTN_COUNT  8   /* Uit + 1..6 + Achteruit */
static lv_obj_t   * fan_btns[FAN_BTN_COUNT] = {0};

/* ── helpers ─────────────────────────────────────────────────── */
static void on_back(lv_event_t * e) { (void)e; ui_pop(); }

static lv_obj_t * make_btn(lv_obj_t * parent, const char * label,
                            uint32_t col, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, SX(8), 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_t * l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(l, SF(20), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return b;
}

static void highlight_btn(lv_obj_t * btn, int active) {
    lv_obj_set_style_border_width(btn, active ? SX(3) : 0, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(COL_TEXT_HI), 0);
}

/* ── fan button handler ───────────────────────────────────────── */
static void on_fan_btn(lv_event_t * e) {
    intptr_t ud = (intptr_t)lv_event_get_user_data(e);
    if (ud == -1) {
        efanlamp_fan_set(0, 0);
    } else if (ud >= 1 && ud <= 6) {
        efanlamp_fan_set(1, (int)ud);
    } else if (ud == 7) {
        efanlamp_fan_set(1, efanlamp.fan_speed > 0 ? efanlamp.fan_speed : 3);
    }
}

/* ── refresh ──────────────────────────────────────────────────── */
static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (!scr_root) return;

    if (!efanlamp.connected) {
        lv_label_set_text(lbl_status, tr("Niet verbonden", "Disconnected"));
    } else {
        char src[32] = "";
        if (efanlamp.last_source[0])
            snprintf(src, sizeof src, "  ·  %s", (const char *)efanlamp.last_source);
        if (efanlamp.fan_on)
            lv_label_set_text_fmt(lbl_status,
                tr("Verbonden  ·  L%d%s", "Connected  ·  L%d%s"),
                efanlamp.fan_speed, src);
        else
            lv_label_set_text_fmt(lbl_status,
                tr("Verbonden  ·  uit%s", "Connected  ·  off%s"), src);
    }

    /* highlight active fan speed button */
    for (int i = 0; i < FAN_BTN_COUNT - 1; i++) {
        if (!fan_btns[i]) continue;
        intptr_t ud = (intptr_t)lv_obj_get_event_user_data(fan_btns[i], 0);
        int active = (ud == -1 && !efanlamp.fan_on) ||
                     (ud >= 1 && ud <= 6 && efanlamp.fan_on && efanlamp.fan_speed == (int)ud);
        highlight_btn(fan_btns[i], active);
    }
}

/* ── screen builder ───────────────────────────────────────────── */
lv_obj_t * screen_efanlamp_create(void) {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_size(scr_root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* ── header ─────────────────────────────────────────────── */
    lv_obj_t * btn_back = lv_btn_create(scr_root);
    lv_obj_set_size(btn_back, SX(120), SY(50));
    lv_obj_set_pos(btn_back, SX(10), SY(10));
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_radius(btn_back, SX(8), 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_t * lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, tr(LV_SYMBOL_LEFT " Terug", LV_SYMBOL_LEFT " Back"));
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_back, SF(20), 0);
    lv_obj_center(lbl_back);
    lv_obj_add_event_cb(btn_back, on_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl_title = lv_label_create(scr_root);
    lv_label_set_text(lbl_title, tr("Ventilator", "Fan"));
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_title, SF(26), 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, SY(18));

    /* ── status line ────────────────────────────────────────── */
    lbl_status = lv_label_create(scr_root);
    lv_label_set_text(lbl_status, "...");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_status, SF(18), 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, SY(56));

    /* ── divider ─────────────────────────────────────────────── */
    lv_obj_t * div1 = lv_obj_create(scr_root);
    lv_obj_set_size(div1, LV_HOR_RES - SX(20), 1);
    lv_obj_set_pos(div1, SX(10), SY(90));
    lv_obj_set_style_bg_color(div1, lv_color_hex(0x2a4060), 0);
    lv_obj_set_style_border_width(div1, 0, 0);

    /* ── fan speed row: [Uit][1][2][3][4][5][6]  [Achteruit] ── */
    const char * fan_labels[] = {
        tr("Uit","Off"), "1", "2", "3", "4", "5", "6"
    };
    const intptr_t fan_uds[] = { -1, 1, 2, 3, 4, 5, 6 };
    int btn_w = SX(96), btn_h = SY(90), btn_gap = SX(10);
    int row_y = SY(110);
    for (int i = 0; i < 7; i++) {
        fan_btns[i] = make_btn(scr_root, fan_labels[i], COL_BTN, on_fan_btn, fan_uds[i]);
        lv_obj_set_size(fan_btns[i], btn_w, btn_h);
        lv_obj_set_pos(fan_btns[i], SX(10) + i * (btn_w + btn_gap), row_y);
    }
    /* Reverse button — right side */
    fan_btns[7] = make_btn(scr_root, tr("Achteruit", "Reverse"), COL_BTN, on_fan_btn, 7);
    lv_obj_set_size(fan_btns[7], SX(130), btn_h);
    lv_obj_set_pos(fan_btns[7], LV_HOR_RES - SX(140), row_y);

    refresh_cb(NULL);
    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}
