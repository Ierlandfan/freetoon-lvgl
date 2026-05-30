/*
 * Inbox modal — fullscreen black overlay with a scrollable list of
 * notifications. Each row has subject (type+subType chip), text body,
 * unread indicator, and Delete button. Tapping a row marks it read.
 *
 * The modal is created on demand via screen_inbox_show() so it's always
 * fresh wrt the current inbox_msgs[]. lv_obj_del on close fully removes
 * it (no caching) — the list is small and rebuild is cheap.
 */
#include "screens.h"
#include "display.h"   /* SF()/SX()/SY() scaling for Toon 1 */
#include "inbox.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static lv_obj_t * modal = NULL;
static lv_obj_t * list_panel;

static void on_close(lv_event_t * e) {
    (void)e;
    if (modal) { lv_obj_del(modal); modal = NULL; }
}

typedef struct { int idx; lv_obj_t * row; } row_ud_t;
static row_ud_t row_ud[INBOX_MAX];

static void rebuild_list(void);  /* fwd */

static void on_row_tap(lv_event_t * e) {
    row_ud_t * ud = (row_ud_t *)lv_event_get_user_data(e);
    if (!ud || ud->idx < 0 || ud->idx >= inbox_count) return;
    if (!inbox_msgs[ud->idx].read) {
        inbox_mark_read(inbox_msgs[ud->idx].uuid);
        rebuild_list();
    }
}
static void on_row_delete(lv_event_t * e) {
    row_ud_t * ud = (row_ud_t *)lv_event_get_user_data(e);
    if (!ud || ud->idx < 0 || ud->idx >= inbox_count) return;
    inbox_delete(inbox_msgs[ud->idx].uuid);
    rebuild_list();
}

static void rebuild_list(void) {
    lv_obj_clean(list_panel);

    if (inbox_count == 0) {
        lv_obj_t * empty = lv_label_create(list_panel);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x88aabb), 0);
        lv_obj_set_style_text_font(empty, SF(22), 0);
        lv_label_set_text(empty, "Geen berichten.");
        lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 0, 12);
        return;
    }

    for (int i = 0; i < inbox_count; i++) {
        inbox_msg_t * m = &inbox_msgs[i];
        row_ud[i].idx = i;

        lv_obj_t * row = lv_obj_create(list_panel);
        lv_obj_set_size(row, SX(920), SY(90));
        lv_obj_set_pos(row, 0, SY(i * 100));
        lv_obj_set_style_bg_color(row, lv_color_hex(m->read ? 0x1a2a44 : 0x223e6a), 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 12, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_row_tap, LV_EVENT_CLICKED, &row_ud[i]);

        /* unread dot */
        if (!m->read) {
            lv_obj_t * dot = lv_obj_create(row);
            lv_obj_set_size(dot, SX(12), SY(12));
            lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(0xffaa44), 0);
            lv_obj_set_style_radius(dot, 6, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
        }

        /* subject chip — type / subType */
        char subj[80];
        snprintf(subj, sizeof(subj), "%s / %s",
                 m->type[0] ? m->type : "?",
                 m->sub_type[0] ? m->sub_type : "?");
        lv_obj_t * lbl_subj = lv_label_create(row);
        lv_obj_set_style_text_color(lbl_subj, lv_color_hex(0x88aabb), 0);
        lv_obj_set_style_text_font(lbl_subj, SF(18), 0);
        lv_label_set_text(lbl_subj, subj);
        lv_obj_align(lbl_subj, LV_ALIGN_TOP_LEFT, SX(24), 0);

        /* text body */
        lv_obj_t * lbl_text = lv_label_create(row);
        lv_obj_set_style_text_color(lbl_text, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(lbl_text, SF(22), 0);
        lv_obj_set_width(lbl_text, SX(720));
        lv_label_set_long_mode(lbl_text, LV_LABEL_LONG_DOT);
        lv_label_set_text(lbl_text, m->text);
        lv_obj_align(lbl_text, LV_ALIGN_TOP_LEFT, SX(24), SY(26));

        /* delete button */
        lv_obj_t * btn = lv_btn_create(row);
        lv_obj_set_size(btn, SX(110), SY(50));
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xaa3344), 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_add_event_cb(btn, on_row_delete, LV_EVENT_CLICKED, &row_ud[i]);
        lv_obj_t * btn_lbl = lv_label_create(btn);
        lv_label_set_text(btn_lbl, "Delete");
        lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(btn_lbl, SF(18), 0);
        lv_obj_center(btn_lbl);
    }
}

void screen_inbox_show(void) {
    if (modal) lv_obj_del(modal);

    modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x0f1a2a), 0);
    lv_obj_set_style_bg_opa(modal, 240, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Header */
    lv_obj_t * close = lv_btn_create(modal);
    lv_obj_set_size(close, SX(140), SY(70));
    lv_obj_align(close, LV_ALIGN_TOP_LEFT, SX(12), SY(12));
    lv_obj_set_style_bg_color(close, lv_color_hex(0x223344), 0);
    lv_obj_set_style_radius(close, 12, 0);
    lv_obj_set_ext_click_area(close, 20);
    lv_obj_add_event_cb(close, on_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(close);
    lv_label_set_text(cl, "Sluit");
    lv_obj_set_style_text_color(cl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cl, SF(22), 0);
    lv_obj_center(cl);

    lv_obj_t * title = lv_label_create(modal);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, "Inbox");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(180), SY(26));

    /* Scrollable list area */
    list_panel = lv_obj_create(modal);
    lv_obj_set_size(list_panel, SX(960), SY(480));
    lv_obj_set_pos(list_panel, SX(30), SY(100));
    lv_obj_set_style_bg_opa(list_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_panel, 0, 0);
    lv_obj_set_style_pad_all(list_panel, 0, 0);
    /* Vertical scrolling enabled by default. */

    rebuild_list();
}
