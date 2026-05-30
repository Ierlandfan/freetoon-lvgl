/*
 * Vent Advanced screen — full editable Itho i2c-settings list.
 * Rows are sourced from vent_settings[] (warmed by the background poller
 * via vent_fetch_all_settings on startup). Tapping a row opens a modal
 * with [-] / [+] / Save / Cancel. Save goes out as ?setsetting=N&value=V.
 *
 * Note: the Itho-Wifi addon ships with the write API disabled by default.
 * vent_save_setting returns -2 in that case; the modal surfaces this so
 * the user knows to flip the toggle in the addon's web UI.
 */
#include "screens.h"
#include "display.h"
#include "ventilation.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t * scr_root = NULL;
static lv_obj_t * list_col = NULL;
static lv_obj_t * lbl_status = NULL;

/* Per-row references so the refresh timer can update value text in-place. */
static lv_obj_t * row_value_lbl[VENT_SETTING_COUNT] = {0};
static lv_obj_t * row_label_lbl[VENT_SETTING_COUNT] = {0};
static lv_timer_t * row_refresh_timer = NULL;

/* Modal state. Only one open at a time. */
static lv_obj_t * modal_backdrop = NULL;
static lv_obj_t * modal_value_lbl = NULL;
static lv_obj_t * modal_err_lbl = NULL;
static int        modal_idx = -1;
static int        modal_val = 0;

static void on_back(lv_event_t * e) {
    (void)e;
    if (row_refresh_timer) { lv_timer_del(row_refresh_timer); row_refresh_timer = NULL; }
    ui_pop();
}

static void modal_close(void) {
    if (modal_backdrop) {
        lv_obj_del(modal_backdrop);
        modal_backdrop = NULL;
        modal_value_lbl = NULL;
        modal_err_lbl = NULL;
        modal_idx = -1;
    }
}

static void update_modal_value(void) {
    if (!modal_value_lbl) return;
    lv_label_set_text_fmt(modal_value_lbl, "%d", modal_val);
}

static void on_modal_step(lv_event_t * e) {
    intptr_t d = (intptr_t)lv_event_get_user_data(e);
    if (modal_idx < 0) return;
    int v = modal_val + (int)d;
    int lo = vent_settings[modal_idx].minimum;
    int hi = vent_settings[modal_idx].maximum;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    modal_val = v;
    update_modal_value();
}

static void on_modal_cancel(lv_event_t * e) {
    (void)e;
    modal_close();
}

static void on_modal_save(lv_event_t * e) {
    (void)e;
    if (modal_idx < 0) return;
    int rc = vent_save_setting(modal_idx, modal_val);
    if (rc == 0) {
        modal_close();
        return;
    }
    if (modal_err_lbl) {
        const char * msg = (rc == -2)
            ? "Write rejected: settings API is disabled in Itho-Wifi addon"
            : "Save failed (HTTP / parse error)";
        lv_label_set_text(modal_err_lbl, msg);
        lv_obj_set_style_text_color(modal_err_lbl,
                                    lv_color_hex(0xff8866), 0);
    }
}

static void open_edit_modal(int idx) {
    if (idx < 0 || idx >= VENT_SETTING_COUNT) return;
    if (vent_settings[idx].loaded != 1) return;
    modal_idx = idx;
    modal_val = vent_settings[idx].current;

    modal_backdrop = lv_obj_create(scr_root);
    lv_obj_set_size(modal_backdrop, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(modal_backdrop, 0, 0);
    lv_obj_set_style_bg_color(modal_backdrop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal_backdrop, 180, 0);
    lv_obj_set_style_border_width(modal_backdrop, 0, 0);
    lv_obj_clear_flag(modal_backdrop, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * box = lv_obj_create(modal_backdrop);
    lv_obj_set_size(box, SX(720), SY(380));
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1a2a44), 0);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x335577), 0);
    lv_obj_set_style_pad_all(box, 16, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * t1 = lv_label_create(box);
    lv_obj_set_style_text_font(t1, SF(22), 0);
    lv_obj_set_style_text_color(t1, lv_color_hex(0xffffff), 0);
    lv_label_set_text_fmt(t1, "#%d", idx);
    lv_obj_align(t1, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t * t2 = lv_label_create(box);
    lv_obj_set_style_text_font(t2, SF(18), 0);
    lv_obj_set_style_text_color(t2, lv_color_hex(0xaaccee), 0);
    lv_obj_set_width(t2, SX(660));
    lv_label_set_long_mode(t2, LV_LABEL_LONG_WRAP);
    lv_label_set_text(t2, vent_settings[idx].label);
    lv_obj_align(t2, LV_ALIGN_TOP_LEFT, SX(60), SY(4));

    lv_obj_t * range = lv_label_create(box);
    lv_obj_set_style_text_font(range, SF(14), 0);
    lv_obj_set_style_text_color(range, lv_color_hex(0x88aabb), 0);
    lv_label_set_text_fmt(range, "range %d .. %d",
                          vent_settings[idx].minimum,
                          vent_settings[idx].maximum);
    lv_obj_align(range, LV_ALIGN_CENTER, 0, SY(-40));

    modal_value_lbl = lv_label_create(box);
    lv_obj_set_style_text_font(modal_value_lbl, SF(48), 0);
    lv_obj_set_style_text_color(modal_value_lbl, lv_color_hex(0xffcc44), 0);
    lv_label_set_text_fmt(modal_value_lbl, "%d", modal_val);
    lv_obj_align(modal_value_lbl, LV_ALIGN_CENTER, 0, SY(4));

    /* Step buttons. -1 / +1 single-tap, -10 / +10 hold for repeat. */
    struct { lv_align_t a; int x, y; int d; const char * t; } step[] = {
        { LV_ALIGN_CENTER, -200,  4, -1,  "-"  },
        { LV_ALIGN_CENTER, +200,  4, +1,  "+"  },
        { LV_ALIGN_CENTER, -130,  4, -10, "--" },
        { LV_ALIGN_CENTER, +130,  4, +10, "++" },
    };
    for (size_t i = 0; i < sizeof(step)/sizeof(step[0]); i++) {
        lv_obj_t * b = lv_btn_create(box);
        lv_obj_set_size(b, SX(60), SY(56));
        lv_obj_align(b, step[i].a, SX(step[i].x), SY(step[i].y));
        lv_obj_set_style_bg_color(b, lv_color_hex(0x335577), 0);
        lv_obj_add_event_cb(b, on_modal_step, LV_EVENT_CLICKED,
                            (void *)(intptr_t)step[i].d);
        lv_obj_add_event_cb(b, on_modal_step, LV_EVENT_LONG_PRESSED_REPEAT,
                            (void *)(intptr_t)step[i].d);
        lv_obj_t * bl = lv_label_create(b);
        lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(bl, SF(28), 0);
        lv_label_set_text(bl, step[i].t);
        lv_obj_center(bl);
    }

    modal_err_lbl = lv_label_create(box);
    lv_obj_set_style_text_font(modal_err_lbl, SF(14), 0);
    lv_obj_set_width(modal_err_lbl, SX(660));
    lv_label_set_long_mode(modal_err_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(modal_err_lbl, "");
    lv_obj_align(modal_err_lbl, LV_ALIGN_BOTTOM_MID, 0, SY(-80));

    lv_obj_t * cancel = lv_btn_create(box);
    lv_obj_set_size(cancel, SX(160), SY(48));
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, SY(-4));
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x444444), 0);
    lv_obj_add_event_cb(cancel, on_modal_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(cancel);
    lv_obj_set_style_text_color(cl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cl, SF(18), 0);
    lv_label_set_text(cl, "Cancel");
    lv_obj_center(cl);

    lv_obj_t * save = lv_btn_create(box);
    lv_obj_set_size(save, SX(160), SY(48));
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, 0, SY(-4));
    lv_obj_set_style_bg_color(save, lv_color_hex(0x2e6e3a), 0);
    lv_obj_add_event_cb(save, on_modal_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sl = lv_label_create(save);
    lv_obj_set_style_text_color(sl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(sl, SF(18), 0);
    lv_label_set_text(sl, "Save");
    lv_obj_center(sl);
}

static void on_row_clicked(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    open_edit_modal(idx);
}

static void refresh_rows_cb(lv_timer_t * t) {
    (void)t;
    int loaded = 0;
    for (int i = 0; i < VENT_SETTING_COUNT; i++) {
        if (!row_value_lbl[i] || !row_label_lbl[i]) continue;
        if (vent_settings[i].loaded == 1) {
            loaded++;
            lv_label_set_text_fmt(row_value_lbl[i], "%d",
                                  vent_settings[i].current);
            if (vent_settings[i].label[0])
                lv_label_set_text(row_label_lbl[i],
                                  vent_settings[i].label);
        } else if (vent_settings[i].loaded == -1) {
            lv_label_set_text(row_value_lbl[i], "err");
        }
    }
    if (lbl_status) {
        if (loaded == VENT_SETTING_COUNT)
            lv_label_set_text(lbl_status, "all settings loaded - tap to edit");
        else
            lv_label_set_text_fmt(lbl_status, "loading %d/%d ...",
                                  loaded, VENT_SETTING_COUNT);
    }
}

static void build_rows(void) {
    for (int i = 0; i < VENT_SETTING_COUNT; i++) {
        lv_obj_t * row = lv_obj_create(list_col);
        lv_obj_set_size(row, SX(980), SY(32));
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_bg_color(row,
            lv_color_hex((i & 1) ? 0x161e2a : 0x111722), 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t * idx_lbl = lv_label_create(row);
        lv_obj_set_style_text_font(idx_lbl, SF(14), 0);
        lv_obj_set_style_text_color(idx_lbl, lv_color_hex(0x88aabb), 0);
        lv_obj_set_width(idx_lbl, SX(38));
        lv_label_set_text_fmt(idx_lbl, "#%d", i);
        lv_obj_align(idx_lbl, LV_ALIGN_LEFT_MID, SX(4), 0);

        row_label_lbl[i] = lv_label_create(row);
        lv_obj_set_style_text_font(row_label_lbl[i], SF(14), 0);
        lv_obj_set_style_text_color(row_label_lbl[i], lv_color_hex(0xeeeeee), 0);
        lv_obj_set_width(row_label_lbl[i], SX(780));
        lv_label_set_long_mode(row_label_lbl[i], LV_LABEL_LONG_CLIP);
        lv_label_set_text(row_label_lbl[i], "(loading)");
        lv_obj_align(row_label_lbl[i], LV_ALIGN_LEFT_MID, SX(50), 0);
        lv_obj_add_flag(row_label_lbl[i], LV_OBJ_FLAG_EVENT_BUBBLE);

        row_value_lbl[i] = lv_label_create(row);
        lv_obj_set_style_text_font(row_value_lbl[i], SF(18), 0);
        lv_obj_set_style_text_color(row_value_lbl[i], lv_color_hex(0xffcc44), 0);
        lv_label_set_text(row_value_lbl[i], "...");
        lv_obj_align(row_value_lbl[i], LV_ALIGN_RIGHT_MID, SX(-10), 0);
        lv_obj_add_flag(row_value_lbl[i], LV_OBJ_FLAG_EVENT_BUBBLE);
    }
}

lv_obj_t * screen_vent_advanced_create(void) {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x0f1a2a), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * hdr = lv_label_create(scr_root);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(hdr, SF(28), 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, SX(20), SY(14));
    lv_label_set_text(hdr, "Ventilation - Settings");

    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, SX(100), SY(44));
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, SX(-20), SY(14));
    lv_obj_set_style_bg_color(back, lv_color_hex(0x223344), 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(bl, SF(18), 0);
    lv_label_set_text(bl, "Back");
    lv_obj_center(bl);

    lbl_status = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_status, SF(14), 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x888888), 0);
    lv_label_set_text(lbl_status, "loading...");
    lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, SX(20), SY(56));

    list_col = lv_obj_create(scr_root);
    lv_obj_set_size(list_col, SX(1004), SY(510));
    lv_obj_align(list_col, LV_ALIGN_TOP_LEFT, SX(10), SY(82));
    lv_obj_set_style_bg_color(list_col, lv_color_hex(0x0c1320), 0);
    lv_obj_set_style_border_width(list_col, 0, 0);
    lv_obj_set_style_pad_all(list_col, 0, 0);
    lv_obj_set_flex_flow(list_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_col, 0, 0);

    memset(row_value_lbl, 0, sizeof row_value_lbl);
    memset(row_label_lbl, 0, sizeof row_label_lbl);
    build_rows();

    /* Reads vent_settings[] populated by the background thread. */
    row_refresh_timer = lv_timer_create(refresh_rows_cb, 1000, NULL);
    refresh_rows_cb(NULL);
    return scr_root;
}
