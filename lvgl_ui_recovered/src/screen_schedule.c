/*
 * Weekly schedule screen.
 *
 * Layout:
 *   Header:        back btn, "Schedule" title.
 *   Day chips:     [Mon][Tue][Wed][Thu][Fri][Sat][Sun] — selected highlighted.
 *   Timeline grid: 7 rows × 24h, colored blocks per entry, "now" marker.
 *   Day detail:    list of switch points for the selected day, each row has
 *                  time + state + [Edit] + [Delete]. Bottom: [+ Add point].
 *
 * Edit modal: time + state buttons, [Save] / [Cancel] / [Delete].
 *
 * On any change we call schedule_save() which writes back via hcb_config.
 */
#include "screens.h"
#include "i18n.h"
#include "display.h"
#include "schedule.h"
#include "settings.h"
#include "theme.h"
#include "boxtalk.h"
#include "pin_modal.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static lv_obj_t * scr_root = NULL;
static int selected_day = 0;      /* 0=Mon..6=Sun */
static lv_obj_t * day_chips[7];
static lv_obj_t * timeline_box;   /* parent of all schedule rectangles */
static lv_obj_t * timeline_now_line;
static lv_obj_t * hour_labels[5] = {NULL};
static lv_obj_t * day_list;       /* parent of switch-point rows */
static lv_timer_t * refresh_timer = NULL;

/* Layout constants — VERTICAL orientation: one column per weekday, time
 * runs top→bottom (00:00 at top, 24:00 at bottom). Reads like a planner.
 * Adapts to Toon 1 (800x480) and Toon 2 (1024x600). */
#define TL_BOX_X      ((DISP_HOR < 900) ? 46 : 60)
#define TL_BOX_Y      ((DISP_VER < 550) ? 98 : 118)
#define TL_COL_W      ((DISP_HOR < 900) ? 102 : 126)
#define TL_HOUR_PX    ((DISP_VER < 550) ? 7 : 10)
#define TL_H          (24 * TL_HOUR_PX)
#define TL_W          (7 * TL_COL_W)
#define TL_PAD_TOP    0
#define TL_PAD_BOTTOM 0
#define TL_PAD_LEFT   0
#define TL_PAD_RIGHT  0

/* Forward decls */
static void rebuild_day_list(void);
static void rebuild_timeline(void);
static void show_edit_modal(int idx);   /* idx<0 = add new */

static void on_back(lv_event_t * e) { (void)e; ui_pop(); }

/* ---------- day chips ---------- */
static int chip_day_of(lv_obj_t * c) {
    for (int i = 0; i < 7; i++) if (day_chips[i] == c) return i;
    return -1;
}
static void style_chip(lv_obj_t * c, int sel) {
    lv_obj_set_style_bg_color(c, lv_color_hex(sel ? (settings.home_light_mode ? 0x2563eb : 0x3388aa) : THEME_CARD_BG), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(sel ? (settings.home_light_mode ? 0x1d4ed8 : 0xffffff) : THEME_BORDER), 0);
    lv_obj_set_style_border_width(c, sel ? 2 : 1, 0);
    lv_obj_t * lbl = lv_obj_get_child(c, 0);
    if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(sel ? 0xffffff : THEME_TEXT_HI), 0);
}
static void on_chip_tap(lv_event_t * e) {
    int d = chip_day_of(lv_event_get_target(e));
    if (d < 0) return;
    selected_day = d;
    for (int i = 0; i < 7; i++) style_chip(day_chips[i], i == d);
    rebuild_day_list();
}

/* ---------- timeline (vertical layout) ---------- */
static void rebuild_timeline(void) {
    /* Clear all children of timeline_box except the now-line. */
    lv_obj_clean(timeline_box);
    timeline_now_line = NULL;

    /* Subtle vertical day-column divider lines connecting columns with day chips */
    for (int d = 1; d < 7; d++) {
        lv_obj_t * div = lv_obj_create(timeline_box);
        lv_obj_set_size(div, 1, TL_H);
        lv_obj_set_pos(div, d * TL_COL_W, 0);
        lv_obj_set_style_bg_color(div, lv_color_hex(THEME_BORDER), 0);
        lv_obj_set_style_bg_opa(div, LV_OPA_40, 0);
        lv_obj_set_style_border_width(div, 0, 0);
        lv_obj_set_style_radius(div, 0, 0);
        lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Horizontal hour gridlines every 6 hours, spanning all 7 day columns. */
    for (int idx = 0, hr = 0; hr <= 24; hr += 6, idx++) {
        int y = hr * TL_HOUR_PX;
        lv_obj_t * tick = lv_obj_create(timeline_box);
        lv_obj_set_size(tick, TL_W, 1);
        lv_obj_set_pos(tick, 0, y);
        lv_obj_set_style_bg_color(tick, lv_color_hex(THEME_BORDER), 0);
        lv_obj_set_style_border_width(tick, 0, 0);
        lv_obj_set_style_radius(tick, 0, 0);
        lv_obj_clear_flag(tick, LV_OBJ_FLAG_SCROLLABLE);

        if (!hour_labels[idx]) {
            hour_labels[idx] = lv_label_create(scr_root);
            lv_label_set_text_fmt(hour_labels[idx], "%02d", hr);
            lv_obj_set_style_text_color(hour_labels[idx], lv_color_hex(THEME_TEXT_DIM), 0);
            lv_obj_set_style_text_font(hour_labels[idx], SF(14), 0);
        }
        lv_obj_set_pos(hour_labels[idx], TL_BOX_X - 26, TL_BOX_Y + TL_PAD_TOP + 1 + y - 7);
    }

    /* Schedule blocks. Each is a vertical rectangle inside its day's
     * column, from start-time at the top to end-time below. Overnight
     * entries split into a "rest of source day" piece + "start of next
     * day" piece. */
    for (int i = 0; i < schedule_count; i++) {
        const schedule_entry_t * e = &schedule_entries[i];
        int s_min = e->start_hour * 60 + e->start_min;
        int e_min = e->end_hour   * 60 + e->end_min;
        int day = e->start_day;
        uint32_t col = schedule_state_color(e->target_state);
        int col_x = day * TL_COL_W + 3;
        int col_w = TL_COL_W - 6;
        if (e->end_day == e->start_day && e_min > s_min) {
            int y1 = (s_min * TL_HOUR_PX) / 60;
            int y2 = (e_min * TL_HOUR_PX) / 60;
            lv_obj_t * b = lv_obj_create(timeline_box);
            lv_obj_set_size(b, col_w, y2 - y1);
            lv_obj_set_pos(b, col_x, y1);
            lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_radius(b, 3, 0);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        } else {
            /* Overnight — bottom slice on start_day, top slice on end_day. */
            int y1 = (s_min * TL_HOUR_PX) / 60;
            int y_end = TL_H;
            lv_obj_t * b1 = lv_obj_create(timeline_box);
            lv_obj_set_size(b1, col_w, y_end - y1);
            lv_obj_set_pos(b1, col_x, y1);
            lv_obj_set_style_bg_color(b1, lv_color_hex(col), 0);
            lv_obj_set_style_border_width(b1, 0, 0);
            lv_obj_set_style_radius(b1, 3, 0);
            lv_obj_clear_flag(b1, LV_OBJ_FLAG_SCROLLABLE);

            int y2 = (e_min * TL_HOUR_PX) / 60;
            int d2 = e->end_day;
            int col_x2 = d2 * TL_COL_W + 3;
            lv_obj_t * b2 = lv_obj_create(timeline_box);
            lv_obj_set_size(b2, col_w, y2);
            lv_obj_set_pos(b2, col_x2, 0);
            lv_obj_set_style_bg_color(b2, lv_color_hex(col), 0);
            lv_obj_set_style_border_width(b2, 0, 0);
            lv_obj_set_style_radius(b2, 3, 0);
            lv_obj_clear_flag(b2, LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    /* "Now" horizontal stripe across the current day's column. */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int wd = tm.tm_wday;            /* 0=Sunday..6=Saturday in tm */
    int toon_day = (wd + 6) % 7;    /* convert to 0=Mon */
    int y_now = ((tm.tm_hour * 60 + tm.tm_min) * TL_HOUR_PX) / 60;
    timeline_now_line = lv_obj_create(timeline_box);
    lv_obj_set_size(timeline_now_line, TL_COL_W - 2, 3);
    lv_obj_set_pos(timeline_now_line, toon_day * TL_COL_W + 1, y_now - 1);
    lv_obj_set_style_bg_color(timeline_now_line, lv_color_hex(settings.home_light_mode ? 0xd97706 : 0xffffff), 0);
    lv_obj_set_style_border_width(timeline_now_line, 0, 0);
    lv_obj_set_style_radius(timeline_now_line, 1, 0);
    lv_obj_clear_flag(timeline_now_line, LV_OBJ_FLAG_SCROLLABLE);
}

/* ---------- per-day list ---------- */
typedef struct { int idx; } row_userdata_t;
static row_userdata_t row_data[SCHEDULE_MAX];

static void on_edit_row(lv_event_t * e) {
    row_userdata_t * rd = (row_userdata_t *)lv_event_get_user_data(e);
    if (rd) show_edit_modal(rd->idx);
}
static void on_add_row(lv_event_t * e) { (void)e; show_edit_modal(-1); }

static void rebuild_day_list(void) {
    lv_obj_clean(day_list);

    lv_obj_t * hdr = lv_label_create(day_list);
    lv_obj_set_style_text_color(hdr, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(hdr, SF(22), 0);
    lv_label_set_text_fmt(hdr, tr("%s - schakelpunten", "%s - switch points"), schedule_day_short(selected_day));
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);

    int row_y = 36;
    int row_count = 0;
    for (int i = 0; i < schedule_count; i++) {
        const schedule_entry_t * e = &schedule_entries[i];
        if (e->start_day != selected_day) continue;
        row_data[i].idx = i;

        lv_obj_t * row = lv_obj_create(day_list);
        lv_obj_set_size(row, lv_pct(100), 50);
        lv_obj_set_pos(row, 0, row_y);
        lv_obj_set_style_bg_color(row, lv_color_hex(THEME_CARD_BG), 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* color swatch */
        lv_obj_t * sw = lv_obj_create(row);
        lv_obj_set_size(sw, 14, 32);
        lv_obj_align(sw, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(schedule_state_color(e->target_state)), 0);
        lv_obj_set_style_radius(sw, 4, 0);
        lv_obj_set_style_border_width(sw, 0, 0);

        char buf[64];
        snprintf(buf, sizeof(buf), "%02d:%02d   %s",
                 e->start_hour, e->start_min, schedule_state_name(e->target_state));
        lv_obj_t * lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_set_style_text_font(lbl, SF(22), 0);
        lv_label_set_text(lbl, buf);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 28, 0);

        lv_obj_t * btn = lv_btn_create(row);
        lv_obj_set_size(btn, 110, 40);
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(THEME_BTN_ACCENT), 0);
        lv_obj_add_event_cb(btn, on_edit_row, LV_EVENT_CLICKED, &row_data[i]);
        lv_obj_t * btn_lbl = lv_label_create(btn);
        lv_label_set_text(btn_lbl, tr("Wijzig", "Edit"));
        lv_obj_set_style_text_color(btn_lbl, lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_center(btn_lbl);

        row_y += 56;
        row_count++;
    }

    if (row_count == 0) {
        lv_obj_t * empty = lv_label_create(day_list);
        lv_obj_set_style_text_color(empty, lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(empty, SF(22), 0);
        lv_label_set_text(empty, tr("Geen schakelpunten voor deze dag.", "No switch points for this day."));
        lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 0, 44);
        row_y = 100;
    }

    /* "Add" button at the end */
    lv_obj_t * add = lv_btn_create(day_list);
    lv_obj_set_size(add, 180, 50);
    lv_obj_set_pos(add, 0, row_y + 4);
    lv_obj_set_style_bg_color(add, lv_color_hex(settings.home_light_mode ? 0x2563eb : 0x3388aa), 0);
    lv_obj_set_style_radius(add, 10, 0);
    lv_obj_add_event_cb(add, on_add_row, LV_EVENT_CLICKED, NULL);
    lv_obj_t * add_lbl = lv_label_create(add);
    lv_label_set_text(add_lbl, tr("+ Schakelpunt", "+ Add switch point"));
    lv_obj_set_style_text_color(add_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(add_lbl, SF(18), 0);
    lv_obj_center(add_lbl);
}

/* ---------- edit modal ---------- */
typedef struct {
    int idx;            /* -1 = add new */
    int hour;
    int minute;
    int target_state;
} edit_state_t;
static edit_state_t  edit_state;
static lv_obj_t *    edit_modal;
static lv_obj_t *    edit_time_lbl;
static lv_obj_t *    edit_state_lbls[4];

static void edit_update_time(void) {
    char b[16];
    snprintf(b, sizeof(b), "%02d:%02d", edit_state.hour, edit_state.minute);
    lv_label_set_text(edit_time_lbl, b);
}
static void edit_update_state_buttons(void) {
    for (int i = 0; i < 4; i++) {
        int active = (i == edit_state.target_state);
        lv_obj_t * btn = lv_obj_get_parent(edit_state_lbls[i]);
        lv_obj_set_style_bg_color(btn,
            lv_color_hex(active ? schedule_state_color(i) : THEME_BTN_BG), 0);
        lv_obj_set_style_border_color(btn,
            lv_color_hex(active ? 0xffffff : THEME_BORDER), 0);
        lv_obj_set_style_border_width(btn, active ? 2 : 1, 0);
    }
}

static void on_hour_inc(lv_event_t * e)   { (void)e; edit_state.hour = (edit_state.hour + 1) % 24;  edit_update_time(); }
static void on_hour_dec(lv_event_t * e)   { (void)e; edit_state.hour = (edit_state.hour + 23) % 24; edit_update_time(); }
static void on_min_inc(lv_event_t * e)    { (void)e; edit_state.minute = (edit_state.minute + 15) % 60; edit_update_time(); }
static void on_min_dec(lv_event_t * e)    { (void)e; edit_state.minute = (edit_state.minute + 45) % 60; edit_update_time(); }

static void on_state_btn(lv_event_t * e) {
    int st = (int)(long)lv_event_get_user_data(e);
    edit_state.target_state = st;
    edit_update_state_buttons();
}

static void close_modal(void) {
    if (edit_modal) { lv_obj_del(edit_modal); edit_modal = NULL; }
}

/* Compute the end-time of an entry as "next switch point on the same day,
   or 23:59 if last." For simplicity new entries default to a 1-hour window. */
static void fill_end_for_new(schedule_entry_t * e) {
    e->end_min  = e->start_min;
    e->end_hour = (e->start_hour + 1) % 24;
    e->end_day  = (e->start_hour + 1 >= 24) ? (e->start_day + 1) % 7 : e->start_day;
}

/* Save + Delete are the only mutation points in this screen — every other
 * handler (chip taps, hour/minute spinners, state buttons) only mutates
 * the scratch `edit_state` struct. PIN-gate just the commits so the user
 * can still browse the schedule and the edit modal freely without being
 * prompted; the prompt fires when they're about to actually change the
 * stored schedule. edit_state is a static so it's still valid when the
 * gated action eventually fires (pin_modal defers asynchronously). */
static void save_apply(void * ctx) {
    (void)ctx;
    schedule_entry_t ent = {0};
    ent.target_state = edit_state.target_state;
    ent.start_min    = edit_state.minute;
    ent.start_hour   = edit_state.hour;
    ent.start_day    = selected_day;
    if (edit_state.idx >= 0) {
        /* keep existing end-time */
        ent.end_min  = schedule_entries[edit_state.idx].end_min;
        ent.end_hour = schedule_entries[edit_state.idx].end_hour;
        ent.end_day  = schedule_entries[edit_state.idx].end_day;
        schedule_replace(edit_state.idx, &ent);
    } else {
        fill_end_for_new(&ent);
        schedule_add(&ent);
    }
    if (schedule_save() == 0) {
        rebuild_timeline();
        rebuild_day_list();
    }
    close_modal();
}
static void on_save(lv_event_t * e) {
    (void)e;
    pin_gate(save_apply, NULL);
}
static void delete_apply(void * ctx) {
    (void)ctx;
    if (edit_state.idx >= 0) {
        schedule_remove(edit_state.idx);
        if (schedule_save() == 0) {
            rebuild_timeline();
            rebuild_day_list();
        }
    }
    close_modal();
}
static void on_delete(lv_event_t * e) {
    (void)e;
    pin_gate(delete_apply, NULL);
}
static void on_cancel(lv_event_t * e) { (void)e; close_modal(); }

static void show_edit_modal(int idx) {
    if (edit_modal) lv_obj_del(edit_modal);

    if (idx >= 0) {
        edit_state.idx          = idx;
        edit_state.hour         = schedule_entries[idx].start_hour;
        edit_state.minute       = schedule_entries[idx].start_min;
        edit_state.target_state = schedule_entries[idx].target_state;
    } else {
        edit_state.idx = -1;
        edit_state.hour = 12; edit_state.minute = 0; edit_state.target_state = 0;
    }

    int is_small = (DISP_VER < 550);

    edit_modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(edit_modal, DISP_HOR, DISP_VER);
    lv_obj_set_pos(edit_modal, 0, 0);
    lv_obj_set_style_bg_color(edit_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(edit_modal, 220, 0);
    lv_obj_set_style_border_width(edit_modal, 0, 0);
    lv_obj_set_style_radius(edit_modal, 0, 0);
    lv_obj_set_style_pad_all(edit_modal, 0, 0);
    lv_obj_clear_flag(edit_modal, LV_OBJ_FLAG_SCROLLABLE);

    int pw = is_small ? 660 : 820;
    int ph = is_small ? 360 : 470;
    int pad = is_small ? 14 : 24;

    lv_obj_t * panel = lv_obj_create(edit_modal);
    lv_obj_set_size(panel, pw, ph);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, pad, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(panel);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, is_small ? SF(20) : SF(26), 0);
    lv_label_set_text_fmt(title, tr("%s - schakelpunt", "%s - switch point"),
                          schedule_day_short(selected_day));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Time editor: big time label on top, buttons below in their own row. */
    edit_time_lbl = lv_label_create(panel);
    lv_obj_set_style_text_color(edit_time_lbl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(edit_time_lbl, is_small ? SF(32) : SF(46), 0);
    edit_update_time();
    lv_obj_align(edit_time_lbl, LV_ALIGN_TOP_MID, 0, is_small ? 16 : 38);

    /* Buttons row (u-, u+, m-, m+) */
    lv_obj_t * b;
    int row_y = is_small ? 62 : 115;
    int tb_w = is_small ? 56 : 72;
    int tb_h = is_small ? 38 : 52;
    int off_far = is_small ? 135 : 190;
    int off_near = is_small ? 46 : 75;

    b = lv_btn_create(panel);
    lv_obj_set_size(b, tb_w, tb_h); lv_obj_align(b, LV_ALIGN_TOP_MID, -off_far, row_y);
    lv_obj_set_style_bg_color(b, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_add_event_cb(b, on_hour_dec, LV_EVENT_CLICKED, NULL);
    lv_obj_t * l = lv_label_create(b); lv_label_set_text(l, tr("u-", "h-"));
    lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(l, is_small ? SF(16) : SF(22), 0); lv_obj_center(l);

    b = lv_btn_create(panel);
    lv_obj_set_size(b, tb_w, tb_h); lv_obj_align(b, LV_ALIGN_TOP_MID, -off_near, row_y);
    lv_obj_set_style_bg_color(b, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_add_event_cb(b, on_hour_inc, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(b); lv_label_set_text(l, tr("u+", "h+"));
    lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(l, is_small ? SF(16) : SF(22), 0); lv_obj_center(l);

    b = lv_btn_create(panel);
    lv_obj_set_size(b, tb_w, tb_h); lv_obj_align(b, LV_ALIGN_TOP_MID, off_near, row_y);
    lv_obj_set_style_bg_color(b, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_add_event_cb(b, on_min_dec, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(b); lv_label_set_text(l, tr("m-", "m-"));
    lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(l, is_small ? SF(16) : SF(22), 0); lv_obj_center(l);

    b = lv_btn_create(panel);
    lv_obj_set_size(b, tb_w, tb_h); lv_obj_align(b, LV_ALIGN_TOP_MID, off_far, row_y);
    lv_obj_set_style_bg_color(b, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_add_event_cb(b, on_min_inc, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(b); lv_label_set_text(l, tr("m+", "m+"));
    lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(l, is_small ? SF(16) : SF(22), 0); lv_obj_center(l);

    /* State picker row (4 buttons) */
    const char * names[] = {tr("Comfort", "Comfort"), tr("Thuis", "Home"),
                            tr("Slapen", "Sleep"), tr("Weg", "Away")};
    int inner_w = pw - 2 * pad;
    int gap = is_small ? 8 : 14;
    int sb_w = (inner_w - 3 * gap) / 4;
    int sb_h = is_small ? 44 : 68;
    int sb_y = is_small ? 116 : 205;

    for (int i = 0; i < 4; i++) {
        lv_obj_t * sb = lv_btn_create(panel);
        lv_obj_set_size(sb, sb_w, sb_h);
        lv_obj_set_pos(sb, i * (sb_w + gap), sb_y);
        lv_obj_set_style_radius(sb, 12, 0);
        lv_obj_add_event_cb(sb, on_state_btn, LV_EVENT_CLICKED, (void *)(long)i);
        lv_obj_t * lbl = lv_label_create(sb);
        lv_label_set_text(lbl, names[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(lbl, is_small ? SF(16) : SF(22), 0);
        lv_obj_center(lbl);
        edit_state_lbls[i] = lbl;
    }
    edit_update_state_buttons();

    /* Bottom buttons */
    int bb_h = is_small ? 40 : 56;
    int bb_w = is_small ? 145 : 190;

    lv_obj_t * save = lv_btn_create(panel);
    lv_obj_set_size(save, bb_w, bb_h);
    lv_obj_align(save, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(settings.home_light_mode ? 0x2563eb : 0x3388aa), 0);
    lv_obj_add_event_cb(save, on_save, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(save); lv_label_set_text(l, tr("Opslaan", "Save"));
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, is_small ? SF(18) : SF(22), 0);
    lv_obj_center(l);

    if (idx >= 0) {
        lv_obj_t * del = lv_btn_create(panel);
        lv_obj_set_size(del, bb_w, bb_h);
        lv_obj_align(del, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(del, lv_color_hex(0xaa3344), 0);
        lv_obj_add_event_cb(del, on_delete, LV_EVENT_CLICKED, NULL);
        l = lv_label_create(del); lv_label_set_text(l, tr("Verwijderen", "Delete"));
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(l, is_small ? SF(18) : SF(22), 0);
        lv_obj_center(l);
    }

    lv_obj_t * cancel = lv_btn_create(panel);
    lv_obj_set_size(cancel, bb_w, bb_h);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_add_event_cb(cancel, on_cancel, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(cancel); lv_label_set_text(l, tr("Annuleren", "Cancel"));
    lv_obj_set_style_text_color(l, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(l, is_small ? SF(18) : SF(22), 0);
    lv_obj_center(l);
}

/* Periodic timer just to refresh the "now" indicator. */
static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (!scr_root || lv_scr_act() != scr_root || !timeline_box) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int wd = tm.tm_wday;
    int toon_day = (wd + 6) % 7;
    int y_now = ((tm.tm_hour * 60 + tm.tm_min) * TL_HOUR_PX) / 60;
    if (!timeline_now_line) {
        timeline_now_line = lv_obj_create(timeline_box);
        lv_obj_set_size(timeline_now_line, TL_COL_W - 2, 3);
        lv_obj_set_style_bg_color(timeline_now_line, lv_color_hex(settings.home_light_mode ? 0xd97706 : 0xffffff), 0);
        lv_obj_set_style_border_width(timeline_now_line, 0, 0);
        lv_obj_set_style_radius(timeline_now_line, 1, 0);
        lv_obj_clear_flag(timeline_now_line, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_set_pos(timeline_now_line, toon_day * TL_COL_W + 1, y_now - 1);
}

static int scr_light_mode = -1;

lv_obj_t * screen_schedule_create(void) {
    if (scr_root && scr_light_mode == settings.home_light_mode) {
        /* Refresh data each time we enter (in case schedule changed elsewhere) */
        schedule_load();
        rebuild_timeline();
        rebuild_day_list();
        return scr_root;
    }
    if (scr_root) { lv_obj_del(scr_root); scr_root = NULL; }
    scr_light_mode = settings.home_light_mode;
    memset(hour_labels, 0, sizeof(hour_labels));
    timeline_now_line = NULL;

    schedule_load();

    /* Default selected day to "today" */
    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    selected_day = (tm.tm_wday + 6) % 7;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(THEME_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Close "X" button + title */
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
    lv_obj_set_style_text_font(title, (DISP_VER < 550) ? SF(24) : SF(28), 0);
    if (settings.client_mode && settings.master_host[0]) {
        lv_label_set_text(title, tr("Programma (Master)", "Schedule (Master)"));
    } else {
        lv_label_set_text(title, tr("Programma", "Schedule"));
    }
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(30), (DISP_VER < 550) ? 14 : SY(18));

    /* Day chips — precisely aligned over their corresponding timeline column */
    for (int i = 0; i < 7; i++) {
        lv_obj_t * c = lv_obj_create(scr_root);
        int chip_w = TL_COL_W - 4;
        int chip_h = (DISP_VER < 550) ? 32 : 40;
        int chip_x = TL_BOX_X + TL_PAD_LEFT + 1 + i * TL_COL_W + 2;
        int chip_y = (DISP_VER < 550) ? 56 : 68;

        lv_obj_set_size(c, chip_w, chip_h);
        lv_obj_set_pos(c, chip_x, chip_y);
        lv_obj_set_style_radius(c, 8, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, on_chip_tap, LV_EVENT_CLICKED, NULL);

        lv_obj_t * lbl = lv_label_create(c);
        lv_label_set_text(lbl, schedule_day_short(i));
        lv_obj_set_style_text_font(lbl, (DISP_VER < 550) ? SF(16) : SF(20), 0);
        lv_obj_center(lbl);

        style_chip(c, i == selected_day);
        day_chips[i] = c;
    }

    /* Timeline container — sized with balanced vertical & horizontal padding */
    int box_w = TL_W + TL_PAD_LEFT + TL_PAD_RIGHT + 2;
    int box_h = TL_H + TL_PAD_TOP + TL_PAD_BOTTOM + 2;
    timeline_box = lv_obj_create(scr_root);
    lv_obj_set_size(timeline_box, box_w, box_h);
    lv_obj_set_pos(timeline_box, TL_BOX_X, TL_BOX_Y);
    lv_obj_set_style_bg_color(timeline_box, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_radius(timeline_box, 10, 0);
    lv_obj_set_style_border_color(timeline_box, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(timeline_box, 1, 0);
    lv_obj_set_style_pad_top(timeline_box, TL_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(timeline_box, TL_PAD_BOTTOM, 0);
    lv_obj_set_style_pad_left(timeline_box, TL_PAD_LEFT, 0);
    lv_obj_set_style_pad_right(timeline_box, TL_PAD_RIGHT, 0);
    lv_obj_clear_flag(timeline_box, LV_OBJ_FLAG_SCROLLABLE);

    /* Day-list container below the timeline */
    int list_y = TL_BOX_Y + box_h + ((DISP_VER < 550) ? 8 : 14);
    int list_h = DISP_VER - list_y - ((DISP_VER < 550) ? 6 : 10);
    day_list = lv_obj_create(scr_root);
    lv_obj_set_size(day_list, box_w, list_h);
    lv_obj_set_pos(day_list, TL_BOX_X, list_y);
    lv_obj_set_style_bg_opa(day_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(day_list, 0, 0);
    lv_obj_set_style_pad_all(day_list, 0, 0);
    lv_obj_set_style_pad_right(day_list, 14, 0);
    lv_obj_add_flag(day_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(day_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(day_list, LV_SCROLLBAR_MODE_ON);
    lv_obj_set_style_bg_color(day_list, lv_color_hex(THEME_BORDER),
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(day_list, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(day_list, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(day_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(day_list, 3, LV_PART_SCROLLBAR);

    rebuild_timeline();
    rebuild_day_list();

    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 60000, NULL);
    return scr_root;
}
