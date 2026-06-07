/*
 * Boiler adapter screen — Settings -> Heating -> Boiler adapter.
 *
 * Status + diagnostics for the Toon's wired boiler adapter:
 *
 *   Keteladapter  — the boiler adapter. WIRED (OpenTherm over /dev/ttymxc0),
 *                   not Z-Wave, so there is nothing to pair. "Online" =
 *                   happ_thermstat reports otCommError==0 (toon_state).
 *
 * The Meteradapter (smart-meter Z-Wave node) has moved to the Z-Wave
 * devices screen (screen_zwave.c).
 */
#include "screens.h"
#include "display.h"
#include "settings.h"
#include "boxtalk.h"
#include <stdio.h>
#include <string.h>

#define COL_BG       0x0e1a2a
#define COL_CARD     0x1a2940
#define COL_TEXT_HI  0xffffff
#define COL_TEXT_DIM 0x88aabb
#define COL_OK       0x2e6e3a
#define COL_WARN     0x6e3a3a
#define COL_OFF      0x3a4658
#define COL_GREEN    0x36c46b
#define COL_RED      0xd4574f

static lv_obj_t * scr_root      = NULL;
static lv_obj_t * lbl_ket_state = NULL;
static lv_obj_t * lbl_ket_sub   = NULL;
static lv_timer_t * refresh_timer = NULL;

static int g_query_ticks = 0;

/* ===================================================================== */
/* Status text                                                           */
/* ===================================================================== */
static void update_ket_labels(void) {
    int online = (toon_state.ot_comm_error == 0);
    lv_obj_set_style_text_color(lbl_ket_state, lv_color_hex(online ? COL_GREEN : COL_RED), 0);
    if (online)
        lv_label_set_text_fmt(lbl_ket_state, "Online  -  boiler %.0f C, mod %.0f%%",
            (double)toon_state.boiler_out_temp, (double)toon_state.modulation_level);
    else
        lv_label_set_text(lbl_ket_state, "Offline - OpenTherm comm error");
    lv_label_set_text(lbl_ket_sub,
        "Wired OpenTherm adapter (/dev/ttymxc0) - no pairing needed.");
}

/* ===================================================================== */
/* Buttons                                                               */
/* ===================================================================== */
static void on_ket_test(lv_event_t * e) {
    (void)e;
    boxtalk_request_boiler_refresh();
    lv_label_set_text(lbl_ket_sub, "Re-querying boiler (happ_thermstat)...");
}

/* ===================================================================== */
/* Refresh loop                                                          */
/* ===================================================================== */
static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (g_query_ticks <= 0) {
        boxtalk_request_boiler_refresh();
        g_query_ticks = 5;
    } else {
        g_query_ticks--;
    }
    update_ket_labels();
}

/* ===================================================================== */
/* Screen build                                                          */
/* ===================================================================== */
static void back_async(void * u) { (void)u; ui_pop(); }
static void on_back(lv_event_t * e) {
    (void)e;
    lv_async_call(back_async, NULL);
}

static void on_scr_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) {
        if (refresh_timer) lv_timer_resume(refresh_timer);
        g_query_ticks = 0;
    } else if (c == LV_EVENT_SCREEN_UNLOADED) {
        if (refresh_timer) lv_timer_pause(refresh_timer);
    }
}

static lv_obj_t * mk_btn(lv_obj_t * parent, const char * txt, uint32_t col,
                         lv_event_cb_t cb) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, 220, 56);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, SF(20), 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

lv_obj_t * screen_adapters_create(void) {
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
    lv_obj_set_style_text_font(bl, SF(22), 0);
    lv_label_set_text(bl, "< Back");
    lv_obj_center(bl);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, "Boiler adapter");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 180, 24);

    /* Keteladapter card + button. */
    lv_obj_t * card = lv_obj_create(scr_root);
    lv_obj_set_size(card, DISP_HOR - 44, SY(210));
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 22, SY(86));
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * tt = lv_label_create(card);
    lv_obj_set_style_text_color(tt, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(tt, SF(22), 0);
    lv_label_set_text(tt, "Keteladapter");
    lv_obj_align(tt, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t * kd = lv_label_create(card);
    lv_obj_set_style_text_color(kd, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(kd, SF(14), 0);
    lv_label_set_text(kd, "Boiler - wired (OpenTherm)");
    lv_obj_align(kd, LV_ALIGN_TOP_LEFT, 2, SY(34));

    lbl_ket_state = lv_label_create(card);
    lv_obj_set_style_text_font(lbl_ket_state, SF(22), 0);
    lv_obj_set_style_text_color(lbl_ket_state, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(lbl_ket_state, "...");
    lv_obj_align(lbl_ket_state, LV_ALIGN_TOP_LEFT, 0, SY(64));

    lbl_ket_sub = lv_label_create(card);
    lv_obj_set_style_text_font(lbl_ket_sub, SF(14), 0);
    lv_obj_set_style_text_color(lbl_ket_sub, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_width(lbl_ket_sub, DISP_HOR - 280);
    lv_label_set_long_mode(lbl_ket_sub, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_ket_sub, "");
    lv_obj_align(lbl_ket_sub, LV_ALIGN_TOP_LEFT, 0, SY(104));

    mk_btn(card, "Test", 0x2a4060, on_ket_test);
    lv_obj_t * ket_test = lv_obj_get_child(card, lv_obj_get_child_cnt(card) - 1);
    lv_obj_align(ket_test, LV_ALIGN_TOP_RIGHT, 0, 0);

    update_ket_labels();

    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}
