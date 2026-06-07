/*
 * screen_calendar.c — full-screen "Agenda" modal listing upcoming events from
 * calendar_state (HA + iCal, merged/sorted in calendar.c). Opened on demand
 * via screen_calendar_show() — from Settings now, and from the home tile later.
 */
#include "lvgl/lvgl.h"
#include "screens.h"
#include "display.h"   /* SF()/SX()/SY() scaling for Toon 1 */
#include "calendar.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static lv_obj_t * modal = NULL;

static void on_close(lv_event_t * e) {
    (void)e;
    if (modal) { lv_obj_del(modal); modal = NULL; }
}

/* "YYYY-MM-DD" → "wo 27 mei" (Dutch). Falls back to the raw date on error. */
static void pretty_date(const char * iso, char * out, size_t osz) {
    static const char * wd[] = { "zo","ma","di","wo","do","vr","za" };
    static const char * mo[] = { "jan","feb","mrt","apr","mei","jun",
                                 "jul","aug","sep","okt","nov","dec" };
    int y, m, d;
    if (sscanf(iso, "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12) {
        snprintf(out, osz, "%s", iso); return;
    }
    struct tm tm = {0};
    tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d; tm.tm_hour = 12;
    time_t t = mktime(&tm);
    struct tm lt; localtime_r(&t, &lt);
    snprintf(out, osz, "%s %d %s", wd[lt.tm_wday % 7], d, mo[m - 1]);
}

void screen_calendar_show(void) {
    if (modal) lv_obj_del(modal);

    /* Parent to the top layer, NOT lv_scr_act(): this is opened from the
     * Settings list, which is usually scrolled down by the time you reach the
     * Agenda button — a modal on the scrolled screen inherits that scroll
     * offset and its top ends up above the viewport. The top layer is a fixed
     * full-screen overlay that's never scrolled. */
    modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x0f1a2a), 0);
    lv_obj_set_style_bg_opa(modal, 240, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

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
    lv_label_set_text(title, "Agenda");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(180), SY(26));

    lv_obj_t * list = lv_obj_create(modal);
    lv_obj_set_size(list, SX(960), SY(480));
    lv_obj_set_pos(list, SX(30), SY(100));
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    if (!settings.calendar_enabled || (!settings.calendar_ha_entity[0] && !settings.calendar_ics_url[0])) {
        lv_obj_t * m = lv_label_create(list);
        lv_label_set_text(m, "Agenda niet ingesteld.\nZet een HA-agenda of een iCal-URL in Instellingen.");
        lv_obj_set_style_text_color(m, lv_color_hex(0x99aabb), 0);
        lv_obj_set_style_text_font(m, SF(22), 0);
        return;
    }
    if (calendar_state.count == 0) {
        lv_obj_t * m = lv_label_create(list);
        lv_label_set_text(m, calendar_state.connected
            ? "Geen aankomende afspraken."
            : "Agenda laden... (of geen verbinding)");
        lv_obj_set_style_text_color(m, lv_color_hex(0x99aabb), 0);
        lv_obj_set_style_text_font(m, SF(22), 0);
        return;
    }

    char last_date[12] = "";
    for (int i = 0; i < calendar_state.count; i++) {
        calendar_event_t * ev = &calendar_state.ev[i];
        /* Date header whenever the day changes. */
        if (strcmp(ev->date, last_date) != 0) {
            snprintf(last_date, sizeof last_date, "%s", ev->date);
            char pd[32]; pretty_date(ev->date, pd, sizeof pd);
            lv_obj_t * h = lv_label_create(list);
            lv_label_set_text(h, pd);
            lv_obj_set_style_text_color(h, lv_color_hex(0x66ccff), 0);
            lv_obj_set_style_text_font(h, SF(22), 0);
            lv_obj_set_style_pad_top(h, i ? 10 : 0, 0);
        }
        lv_obj_t * row = lv_label_create(list);
        lv_obj_set_width(row, SX(900));
        lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
        char line[120];
        snprintf(line, sizeof line, "   %s  %s",
                 ev->time[0] ? ev->time : "hele dag", ev->summary);
        lv_label_set_text(row, line);
        lv_obj_set_style_text_color(row, lv_color_hex(0xdddddd), 0);
        lv_obj_set_style_text_font(row, SF(18), 0);
    }
}
