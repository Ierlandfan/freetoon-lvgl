/*
 * Forecast detail screen.
 *
 * Final layout (Phase 2 adds the radar image on the left):
 *   [ Radar image | weersverwachting title + body text ]
 *   [ 5-day forecast strip across the bottom ]
 *
 * This Phase-1 version uses a placeholder block where the radar image
 * goes, and shows the weatherreport text body on the right.
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "weather.h"
#include "icons.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t * scr_root = NULL;
static lv_obj_t * lbl_title;
static lv_obj_t * lbl_body;
static lv_obj_t * fc_day_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_temp_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_desc_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_icon[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_wind_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_wind_arrow[WEATHER_FORECAST_DAYS];
static lv_obj_t * radar_img = NULL;
static lv_timer_t * refresh_timer = NULL;

/* Radar zoom — LVGL convention: 256 = 100 %. Source GIF is 550x512, frame
 * is 380x380, so we clamp to a range that lets the user see detail without
 * overshooting the visible area too far. */
#define RADAR_ZOOM_MIN  128
#define RADAR_ZOOM_MAX  768
#define RADAR_ZOOM_STEP  64
static int radar_zoom = 195;             /* fits 550-wide source into 380 */

static void on_back(lv_event_t * e) { (void)e; ui_pop(); }
static void on_radar_zoom(lv_event_t * e) {
    intptr_t d = (intptr_t)lv_event_get_user_data(e);
    int z = radar_zoom + (int)d;
    if (z < RADAR_ZOOM_MIN) z = RADAR_ZOOM_MIN;
    if (z > RADAR_ZOOM_MAX) z = RADAR_ZOOM_MAX;
    radar_zoom = z;
    if (radar_img) lv_img_set_zoom(radar_img, radar_zoom);
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    lv_label_set_text(lbl_title, weather_state.weatherreport_title);
    lv_label_set_text(lbl_body,  weather_state.weatherreport_text);

    int show_hourly = settings.forecast_mode != FORECAST_DAILY
                      && weather_state.hour_count > 0;
    if (show_hourly) {
        for (int i = 0; i < weather_state.hour_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_hour_t * h = &weather_state.hours[i];
            lv_label_set_text(fc_day_lbl[i], h->label);
            lv_label_set_text_fmt(fc_temp_lbl[i], "%.0f\xc2\xb0",
                                  h->temperature);
            if (fc_icon[i]) {
                lv_img_set_src(fc_icon[i], weather_icon_for_lg(h->icon));
                lv_obj_set_style_img_recolor(fc_icon[i],
                    lv_color_hex(weather_icon_color_for(h->icon)), 0);
            }
            if (fc_wind_lbl[i]) {
                if (h->wind_dir[0])
                    lv_label_set_text_fmt(fc_wind_lbl[i], "%s %d Bft",
                                          h->wind_dir, h->wind_bft);
                else
                    lv_label_set_text(fc_wind_lbl[i], "");
            }
            if (fc_wind_arrow[i]) {
                int ang = wind_dir_angle(h->wind_dir);
                if (ang >= 0) {
                    lv_img_set_angle(fc_wind_arrow[i], ang);
                    lv_obj_clear_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    } else {
        for (int i = 0; i < weather_state.day_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_day_t * d = &weather_state.days[i];
            lv_label_set_text(fc_day_lbl[i], d->day);
            lv_label_set_text_fmt(fc_temp_lbl[i],
                                  "%.0f\xc2\xb0 (%.0f\xc2\xb0)",
                                  d->max_temp, d->min_temp);
            if (fc_icon[i]) {
                lv_img_set_src(fc_icon[i], weather_icon_for_lg(d->icon));
                lv_obj_set_style_img_recolor(fc_icon[i],
                    lv_color_hex(weather_icon_color_for(d->icon)), 0);
            }
            if (fc_wind_lbl[i]) {
                if (d->wind_dir[0])
                    lv_label_set_text_fmt(fc_wind_lbl[i], "%s %d Bft",
                                          d->wind_dir, d->wind_bft);
                else
                    lv_label_set_text(fc_wind_lbl[i], "");
            }
            if (fc_wind_arrow[i]) {
                int ang = wind_dir_angle(d->wind_dir);
                if (ang >= 0) {
                    lv_img_set_angle(fc_wind_arrow[i], ang);
                    lv_obj_clear_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }
}

lv_obj_t * screen_forecast_create(void) {
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x0f1a2a), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Back + title */
    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, 140, 70);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x223344), 0);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_set_ext_click_area(back, 20);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_label_set_text(bl, tr("< Terug", "< Back"));
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(bl, SF(22), 0);
    lv_obj_center(bl);

    /* Radar image — buienradar serves a 500x512 PNG of the Netherlands
       precipitation map. Our weather thread saves it to disk every 5 min;
       we render via LVGL's stdio FS driver + PNG decoder. */
    lv_obj_t * radar_frame = lv_obj_create(scr_root);
    lv_obj_set_size(radar_frame, SX(380), SY(380));
    lv_obj_set_pos(radar_frame, 30, SY(100));
    lv_obj_set_style_bg_color(radar_frame, lv_color_hex(0x1a2a44), 0);
    lv_obj_set_style_radius(radar_frame, 12, 0);
    lv_obj_set_style_border_color(radar_frame, lv_color_hex(0x335577), 0);
    lv_obj_set_style_border_width(radar_frame, 1, 0);
    lv_obj_set_style_pad_all(radar_frame, 0, 0);
    lv_obj_set_style_clip_corner(radar_frame, true, 0);
    lv_obj_clear_flag(radar_frame, LV_OBJ_FLAG_SCROLLABLE);

    /* Buienradar radar GIF. With our local gifdec patch the no-GCT
       case is now accepted; the per-frame LCT populates the palette. */
    radar_img = lv_gif_create(radar_frame);
    lv_gif_set_src(radar_img, "S:/tmp/toonui_radar.gif");
    lv_img_set_zoom(radar_img, radar_zoom);
    lv_obj_center(radar_img);

    /* Zoom + / - buttons stacked on top of the radar frame's top-left
       corner. Translucent so they don't fully hide the map underneath. */
    struct { lv_align_t a; int x, y; int d; const char * t; } z[] = {
        { LV_ALIGN_TOP_LEFT,     34,  SY(102), +RADAR_ZOOM_STEP, "+" },
        { LV_ALIGN_TOP_LEFT,     34,  SY(146), -RADAR_ZOOM_STEP, "-" },
    };
    for (size_t i = 0; i < sizeof(z)/sizeof(z[0]); i++) {
        lv_obj_t * b = lv_btn_create(scr_root);
        lv_obj_set_size(b, 40, 40);
        lv_obj_align(b, z[i].a, z[i].x, z[i].y);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x335577), 0);
        lv_obj_set_style_bg_opa(b, 180, 0);
        lv_obj_set_style_radius(b, 20, 0);
        lv_obj_add_event_cb(b, on_radar_zoom, LV_EVENT_CLICKED,
                            (void *)(intptr_t)z[i].d);
        lv_obj_t * bl = lv_label_create(b);
        lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(bl, SF(28), 0);
        lv_label_set_text(bl, z[i].t);
        lv_obj_center(bl);
    }

    /* Right side: title + body text */
    lbl_title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_title, SF(28), 0);
    lv_obj_set_width(lbl_title, SX(540));
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, SX(440), SY(100));
    lv_label_set_text(lbl_title, tr("Weersverwachting", "Weather forecast"));

    /* Body text — wrap inside a vertically-scrollable container so the
     * full weather report is readable. Previously the LONG_DOT mode
     * truncated everything past ~340 px tall with an ellipsis. */
    lv_obj_t * body_scroll = lv_obj_create(scr_root);
    lv_obj_set_size(body_scroll, SX(560), SY(340) - (DISP_VER < 600 ? 80 : 0));
    lv_obj_align(body_scroll, LV_ALIGN_TOP_LEFT, SX(440), SY(150));
    lv_obj_set_style_bg_opa(body_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body_scroll, 0, 0);
    lv_obj_set_style_pad_all(body_scroll, 0, 0);
    lv_obj_set_scroll_dir(body_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body_scroll, LV_SCROLLBAR_MODE_AUTO);
    lbl_body = lv_label_create(body_scroll);
    lv_obj_set_style_text_color(lbl_body, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_body, SF(18), 0);
    lv_obj_set_width(lbl_body, SX(540));
    lv_label_set_long_mode(lbl_body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_body, tr("(laden...)", "(loading...)"));

    /* 5-day forecast strip — same column anatomy as the home band: day
       label top-left, max°(min°) top-right, big icon centred, wind arrow
       + Bft along the bottom. The home strip switched away from the
       verbose desc text so we keep the layouts identical. */
    int col_w = (DISP_HOR - 20) / WEATHER_FORECAST_DAYS;
    /* On Toon 1 the radar/text block (ends ~y=384) leaves only a thin band
     * at the bottom, so sit the strip just under it and shorten it so its
     * bottom stays above y=480. On Toon 2 it keeps the roomy y=460/130. */
    int strip_y = (DISP_VER < 600) ? 390 : 460;
    int strip_h = (DISP_VER < 600) ? 84  : 130;
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        lv_obj_t * col = lv_obj_create(scr_root);
        lv_obj_set_size(col, col_w - 8, strip_h);
        lv_obj_set_pos(col, 10 + i * col_w, strip_y);
        lv_obj_set_style_bg_color(col, lv_color_hex(0x1a2a44), 0);
        lv_obj_set_style_radius(col, 10, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_pad_all(col, 8, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

        /* Centred big icon, larger than the home band since the detail
           screen gives us more vertical room per column. */
        fc_icon[i] = lv_img_create(col);
        lv_img_set_src(fc_icon[i], &icon_wx_cloud_lg);
        lv_obj_set_style_img_recolor(fc_icon[i], lv_color_hex(0xddeeff), 0);
        lv_obj_set_style_img_recolor_opa(fc_icon[i], 255, 0);
        /* Shrink the big icon on Toon 1's shorter strip so it fits the
         * 84px column without crowding the day/temp/wind text. */
        if (DISP_VER < 600) lv_img_set_zoom(fc_icon[i], 160);
        lv_obj_align(fc_icon[i], LV_ALIGN_CENTER, 0, 0);

        /* Wind-direction arrow + Bft at the bottom of each column. */
        fc_wind_arrow[i] = lv_img_create(col);
        lv_img_set_src(fc_wind_arrow[i], &icon_wind_arrow);
        lv_img_set_pivot(fc_wind_arrow[i], 16, 16);
        lv_obj_align(fc_wind_arrow[i], LV_ALIGN_BOTTOM_LEFT, 0, 2);
        lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);

        fc_wind_lbl[i] = lv_label_create(col);
        lv_obj_set_style_text_color(fc_wind_lbl[i], lv_color_hex(0x88aabb), 0);
        lv_obj_set_style_text_font(fc_wind_lbl[i], SF(18), 0);
        lv_label_set_text(fc_wind_lbl[i], "");
        lv_obj_align(fc_wind_lbl[i], LV_ALIGN_BOTTOM_LEFT, 30, -2);

        fc_day_lbl[i] = lv_label_create(col);
        lv_obj_set_style_text_color(fc_day_lbl[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(fc_day_lbl[i], SF(22), 0);
        lv_label_set_text(fc_day_lbl[i], "--");
        lv_obj_align(fc_day_lbl[i], LV_ALIGN_TOP_LEFT, 0, 0);

        fc_temp_lbl[i] = lv_label_create(col);
        lv_obj_set_style_text_color(fc_temp_lbl[i], lv_color_hex(0xffcc44), 0);
        lv_obj_set_style_text_font(fc_temp_lbl[i], SF(22), 0);
        lv_label_set_text(fc_temp_lbl[i], "");
        lv_obj_align(fc_temp_lbl[i], LV_ALIGN_TOP_RIGHT, 0, 0);

        /* Verbose desc moved to the title body — match the home band. */
        fc_desc_lbl[i] = NULL;
    }

    refresh_cb(NULL);
    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 5000, NULL);
    return scr_root;
}
