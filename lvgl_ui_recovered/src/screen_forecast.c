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
#include "theme.h"
#include "lvgl/src/extra/libs/gif/lv_gif.h"
#include "buienradar_rain.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
static int radar_loaded = 0;
static lv_timer_t * refresh_timer = NULL;

static int radar_zoom = 200;

static int fc_view_mode = 0; /* 0 = Radar, 1 = 3h, 2 = 24h */
static lv_obj_t * btn_view_radar = NULL;
static lv_obj_t * btn_view_3h = NULL;
static lv_obj_t * btn_view_24h = NULL;
static lv_obj_t * fc_rain_cont = NULL;
static lv_obj_t * fc_rain_chart = NULL;
static lv_chart_series_t * fc_rain_ser = NULL;
static lv_obj_t * fc_rain_lbl_status = NULL;
static lv_obj_t * fc_rain_lbl_empty = NULL;
static lv_obj_t * fc_thresh_lbl[3] = {0};
static lv_obj_t * fc_rain_lbl_times[5] = {0};

static void update_forecast_view_mode(void) {
    if (!scr_root) return;
    int light = settings.home_light_mode;

    /* Update button styles */
    lv_obj_t * btns[3] = { btn_view_radar, btn_view_3h, btn_view_24h };
    for (int i = 0; i < 3; i++) {
        if (!btns[i]) continue;
        int active = (fc_view_mode == i);
        lv_obj_set_style_border_width(btns[i], active ? 2 : 0, 0);
        lv_obj_set_style_border_color(btns[i], lv_color_hex(light ? 0x224488 : 0xffffff), 0);
        lv_obj_set_style_bg_color(btns[i], lv_color_hex(active ? (light ? 0x90c0f0 : 0x3a6090) : (light ? 0xccddee : 0x243242)), 0);
        lv_obj_t * l = lv_obj_get_child(btns[i], 0);
        if (l) lv_obj_set_style_text_color(l, lv_color_hex(active ? (light ? 0x112233 : 0xffffff) : (light ? 0x445566 : 0x88aabb)), 0);
    }

    if (fc_view_mode == 0) {
        if (radar_img) {
            lv_obj_clear_flag(radar_img, LV_OBJ_FLAG_HIDDEN);
            lv_gif_t * g = (lv_gif_t *)radar_img;
            if (g->timer) lv_timer_resume(g->timer);
        }
        if (fc_rain_cont) lv_obj_add_flag(fc_rain_cont, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (radar_img) {
            lv_obj_add_flag(radar_img, LV_OBJ_FLAG_HIDDEN);
            lv_gif_t * g = (lv_gif_t *)radar_img;
            if (g->timer) lv_timer_pause(g->timer);
        }
        if (fc_rain_cont) lv_obj_clear_flag(fc_rain_cont, LV_OBJ_FLAG_HIDDEN);

        buienradar_rain_snapshot_t snap;
        buienradar_rain_get_snapshot(&snap);

        int has_rain = (fc_view_mode == 1) ? snap.has_rain_3h : snap.has_rain_24h;

        if (fc_rain_lbl_empty) {
            if (snap.is_valid && !has_rain) {
                lv_label_set_text(fc_rain_lbl_empty, (fc_view_mode == 2) ?
                                  tr("Geen neerslag verwacht (24u)", "No rain expected (24h)") :
                                  tr("Geen neerslag verwacht", "No rain expected"));
                lv_obj_clear_flag(fc_rain_lbl_empty, LV_OBJ_FLAG_HIDDEN);
                if (fc_rain_lbl_status) lv_obj_add_flag(fc_rain_lbl_status, LV_OBJ_FLAG_HIDDEN);
                if (fc_rain_chart) lv_obj_add_flag(fc_rain_chart, LV_OBJ_FLAG_HIDDEN);
                for (int i = 0; i < 3; i++) {
                    if (fc_thresh_lbl[i]) lv_obj_add_flag(fc_thresh_lbl[i], LV_OBJ_FLAG_HIDDEN);
                }
                for (int i = 0; i < 5; i++) {
                    if (fc_rain_lbl_times[i]) lv_obj_add_flag(fc_rain_lbl_times[i], LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                lv_obj_add_flag(fc_rain_lbl_empty, LV_OBJ_FLAG_HIDDEN);
                if (fc_rain_lbl_status) lv_obj_clear_flag(fc_rain_lbl_status, LV_OBJ_FLAG_HIDDEN);
                if (fc_rain_chart) lv_obj_clear_flag(fc_rain_chart, LV_OBJ_FLAG_HIDDEN);
                for (int i = 0; i < 3; i++) {
                    if (fc_thresh_lbl[i]) lv_obj_clear_flag(fc_thresh_lbl[i], LV_OBJ_FLAG_HIDDEN);
                }
                for (int i = 0; i < 5; i++) {
                    if (fc_rain_lbl_times[i]) lv_obj_clear_flag(fc_rain_lbl_times[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        if (fc_view_mode == 1) {
            /* 3h view */
            if (fc_rain_lbl_status) {
                lv_label_set_text(fc_rain_lbl_status, snap.summary_3h[0] ? snap.summary_3h : tr("Laden...", "Loading..."));
            }
            if (fc_rain_chart && fc_rain_ser) {
                int n = snap.count_3h;
                if (n <= 0) n = 38;
                lv_chart_set_point_count(fc_rain_chart, n);
                for (int i = 0; i < n; i++) {
                    fc_rain_ser->y_points[i] = (i < snap.count_3h) ? snap.points_3h[i].value : 0;
                }
                lv_chart_refresh(fc_rain_chart);
            }
            if (snap.count_3h > 0) {
                int n = snap.count_3h;
                if (fc_rain_lbl_times[0]) lv_label_set_text(fc_rain_lbl_times[0], tr("Nu", "Now"));
                if (fc_rain_lbl_times[1]) lv_label_set_text(fc_rain_lbl_times[1], snap.points_3h[n / 4].time_str);
                if (fc_rain_lbl_times[2]) lv_label_set_text(fc_rain_lbl_times[2], snap.points_3h[n / 2].time_str);
                if (fc_rain_lbl_times[3]) lv_label_set_text(fc_rain_lbl_times[3], snap.points_3h[3 * n / 4].time_str);
                if (fc_rain_lbl_times[4]) {
                    lv_label_set_text(fc_rain_lbl_times[4], snap.points_3h[n - 1].time_str);
                    if (fc_rain_chart) lv_obj_align_to(fc_rain_lbl_times[4], fc_rain_chart, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 4);
                }
            }
        } else {
            /* 24h view */
            if (fc_rain_lbl_status) {
                lv_label_set_text(fc_rain_lbl_status, snap.summary_24h[0] ? snap.summary_24h : tr("Laden...", "Loading..."));
            }
            if (fc_rain_chart && fc_rain_ser) {
                int n = snap.count_24h;
                if (n <= 0) n = 24;
                lv_chart_set_point_count(fc_rain_chart, n);
                for (int i = 0; i < n; i++) {
                    fc_rain_ser->y_points[i] = (i < snap.count_24h) ? snap.points_24h[i].value : 0;
                }
                lv_chart_refresh(fc_rain_chart);
            }
            if (snap.count_24h > 0) {
                int n = snap.count_24h;
                int idxs[5] = { 0, n / 4, n / 2, (3 * n) / 4, n - 1 };
                for (int i = 0; i < 5; i++) {
                    if (!fc_rain_lbl_times[i]) continue;
                    int idx = idxs[i];
                    if (idx < 0) idx = 0;
                    if (idx >= n) idx = n - 1;
                    char hbuf[8] = "";
                    const char * ts = snap.points_24h[idx].time_str;
                    if (ts[0]) {
                        snprintf(hbuf, sizeof(hbuf), "%.2s", ts);
                    } else {
                        snprintf(hbuf, sizeof(hbuf), "%02d", i * 6);
                    }
                    lv_label_set_text(fc_rain_lbl_times[i], hbuf);
                }
                if (fc_rain_chart && fc_rain_lbl_times[4]) {
                    lv_obj_align_to(fc_rain_lbl_times[4], fc_rain_chart, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 4);
                }
            }
        }
    }
}

static void on_view_btn_click(lv_event_t * e) {
    int mode = (int)(intptr_t)lv_event_get_user_data(e);
    fc_view_mode = mode;
    update_forecast_view_mode();
}

static void on_day_card_clicked(lv_event_t * e) {
    int day_idx = (int)(intptr_t)lv_event_get_user_data(e);
    screen_weather_hourly_show(day_idx);
}

static void on_back(lv_event_t * e) {
    (void)e;
    if (radar_img) {
        lv_gif_t * g = (lv_gif_t *)radar_img;
        if (g->timer) lv_timer_pause(g->timer);
    }
    ui_pop();
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (!scr_root) return;
    if (lv_scr_act() != scr_root) {
        if (radar_img) {
            lv_gif_t * g = (lv_gif_t *)radar_img;
            if (g->timer) lv_timer_pause(g->timer);
        }
        return;
    }
    if (radar_img && fc_view_mode == 0) {
        lv_gif_t * g = (lv_gif_t *)radar_img;
        if (g->timer) lv_timer_resume(g->timer);
    }
    if (fc_view_mode != 0) {
        update_forecast_view_mode();
    }
    weather_state_t wx;
    weather_get_snapshot(&wx);
    lv_label_set_text(lbl_title, wx.weatherreport_title);
    lv_label_set_text(lbl_body,  wx.weatherreport_text);

    if (radar_img && !radar_loaded && access("/tmp/toonui_radar.gif", R_OK) == 0) {
        lv_gif_set_src(radar_img, "S:/tmp/toonui_radar.gif");
        lv_img_set_zoom(radar_img, radar_zoom);
        lv_obj_center(radar_img);
        radar_loaded = 1;
    }

    int show_hourly = settings.forecast_mode != FORECAST_DAILY
                      && wx.hour_count > 0;
    if (show_hourly) {
        for (int i = 0; i < wx.hour_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_hour_t * h = &wx.hours[i];
            lv_label_set_text(fc_day_lbl[i], h->label);
            lv_label_set_text_fmt(fc_temp_lbl[i], "%.0f\xc2\xb0",
                                  weather_hour_display_temp(h));
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
        for (int i = 0; i < wx.day_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_day_t * d = &wx.days[i];
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

static int scr_light_mode = -1;

lv_obj_t * screen_forecast_create(void) {
    if (scr_root && scr_light_mode == settings.home_light_mode) {
        if (radar_img) {
            lv_gif_t * g = (lv_gif_t *)radar_img;
            if (g->timer) lv_timer_resume(g->timer);
        }
        return scr_root;
    }
    if (scr_root) {
        lv_obj_del(scr_root);
        scr_root = NULL;
        radar_img = NULL;
        radar_loaded = 0;
        btn_view_radar = NULL;
        btn_view_3h = NULL;
        btn_view_24h = NULL;
        fc_rain_cont = NULL;
        fc_rain_chart = NULL;
        fc_rain_ser = NULL;
        fc_rain_lbl_status = NULL;
        fc_rain_lbl_empty = NULL;
        for (int i = 0; i < 3; i++) fc_thresh_lbl[i] = NULL;
        for (int i = 0; i < 5; i++) fc_rain_lbl_times[i] = NULL;
    }
    scr_light_mode = settings.home_light_mode;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(THEME_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Close button top-right */
    lv_obj_t * close_btn = lv_btn_create(scr_root);
    int cb_w = (DISP_HOR < 1000) ? 68 : 74;
    int cb_h = (DISP_VER < 600) ? 46 : 50;
    lv_obj_set_size(close_btn, cb_w, cb_h);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, SX(-12), SY(8));
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(close_btn, 10, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_set_ext_click_area(close_btn, 30);
    lv_obj_add_event_cb(close_btn, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * close_lbl = lv_label_create(close_btn);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(close_lbl, SF(22), 0);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);

    /* Radar image — buienradar serves a 500x512 PNG of the Netherlands
       precipitation map. Our weather thread saves it to disk every 5 min;
       we render via LVGL's stdio FS driver + PNG decoder. */
    int rf_sz = (DISP_VER < 600) ? 340 : 430;
    int rf_x = (DISP_VER < 600) ? 12 : 18;
    int rf_y = (DISP_VER < 600) ? 12 : 16;
    lv_obj_t * radar_frame = lv_obj_create(scr_root);
    lv_obj_set_size(radar_frame, rf_sz, rf_sz);
    lv_obj_set_pos(radar_frame, rf_x, rf_y);
    lv_obj_set_style_bg_color(radar_frame, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_radius(radar_frame, 12, 0);
    lv_obj_set_style_border_color(radar_frame, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(radar_frame, 1, 0);
    lv_obj_set_style_pad_all(radar_frame, 0, 0);
    lv_obj_set_style_clip_corner(radar_frame, true, 0);
    lv_obj_clear_flag(radar_frame, LV_OBJ_FLAG_SCROLLABLE);

    /* Buienradar radar GIF. With our local gifdec patch the no-GCT
       case is now accepted; the per-frame LCT populates the palette. */
    radar_img = lv_gif_create(radar_frame);
    if (access("/tmp/toonui_radar.gif", R_OK) == 0) {
        lv_gif_set_src(radar_img, "S:/tmp/toonui_radar.gif");
        radar_loaded = 1;
    } else {
        radar_loaded = 0;
    }
    radar_zoom = (DISP_VER < 600) ? 160 : 202;
    lv_img_set_zoom(radar_img, radar_zoom);
    lv_obj_center(radar_img);

    /* Expanded Rain Forecast container (3h / 24h charts) inside radar_frame */
    fc_rain_cont = lv_obj_create(radar_frame);
    lv_obj_set_size(fc_rain_cont, rf_sz, rf_sz);
    lv_obj_set_pos(fc_rain_cont, 0, 0);
    lv_obj_set_style_bg_color(fc_rain_cont, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_border_width(fc_rain_cont, 0, 0);
    lv_obj_set_style_pad_all(fc_rain_cont, 8, 0);
    lv_obj_clear_flag(fc_rain_cont, LV_OBJ_FLAG_SCROLLABLE);
    if (fc_view_mode == 0) lv_obj_add_flag(fc_rain_cont, LV_OBJ_FLAG_HIDDEN);

    fc_rain_lbl_status = lv_label_create(fc_rain_cont);
    lv_obj_set_style_text_color(fc_rain_lbl_status, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(fc_rain_lbl_status, SF(15), 0);
    lv_label_set_text(fc_rain_lbl_status, tr("Laden...", "Loading..."));
    lv_obj_align(fc_rain_lbl_status, LV_ALIGN_TOP_LEFT, 6, 42);

    fc_rain_chart = lv_chart_create(fc_rain_cont);
    int label_w = 42;
    int chart_w = rf_sz - 16 - label_w;
    int chart_h = rf_sz - 116;
    lv_obj_set_size(fc_rain_chart, chart_w, chart_h);
    lv_obj_align(fc_rain_chart, LV_ALIGN_BOTTOM_RIGHT, -4, -26);
    lv_obj_set_style_bg_color(fc_rain_chart, lv_color_hex(settings.home_light_mode ? 0xeef4fa : 0x141e2a), 0);
    lv_obj_set_style_border_color(fc_rain_chart, lv_color_hex(settings.home_light_mode ? 0xccdae8 : 0x1e2c3e), 0);
    lv_obj_set_style_border_width(fc_rain_chart, 1, 0);
    lv_obj_set_style_radius(fc_rain_chart, 8, 0);
    lv_obj_set_style_pad_ver(fc_rain_chart, 2, 0);
    lv_obj_set_style_pad_hor(fc_rain_chart, 0, 0);
    lv_obj_clear_flag(fc_rain_chart, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_chart_set_type(fc_rain_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_range(fc_rain_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(fc_rain_chart, 4, 4);
    lv_obj_set_style_line_color(fc_rain_chart, lv_color_hex(settings.home_light_mode ? 0xccddee : 0x223344), LV_PART_MAIN);
    lv_obj_set_style_line_opa(fc_rain_chart, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_column(fc_rain_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(fc_rain_chart, 2, LV_PART_ITEMS);

    fc_rain_ser = lv_chart_add_series(fc_rain_chart, lv_color_hex(0x5a9bd3), LV_CHART_AXIS_PRIMARY_Y);

    static const char * thresh_names[] = {"Zwaar", "Matig", "Licht"};
    for (int i = 0; i < 3; i++) {
        fc_thresh_lbl[i] = lv_label_create(fc_rain_cont);
        lv_obj_set_style_text_color(fc_thresh_lbl[i], lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(fc_thresh_lbl[i], SF(11), 0);
        lv_label_set_text(fc_thresh_lbl[i], thresh_names[i]);
        int y_off = (chart_h * i) / 3 - 6;
        lv_obj_align_to(fc_thresh_lbl[i], fc_rain_chart, LV_ALIGN_OUT_LEFT_TOP, -4, y_off);
    }

    for (int i = 0; i < 5; i++) {
        fc_rain_lbl_times[i] = lv_label_create(fc_rain_cont);
        lv_obj_set_style_text_color(fc_rain_lbl_times[i], lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(fc_rain_lbl_times[i], SF(12), 0);
        lv_label_set_text(fc_rain_lbl_times[i], (i == 4) ? "24" : (i == 0) ? "00" : "");
        if (i == 0) {
            lv_obj_align_to(fc_rain_lbl_times[i], fc_rain_chart, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
        } else if (i == 4) {
            lv_obj_align_to(fc_rain_lbl_times[i], fc_rain_chart, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 4);
        } else {
            lv_obj_align_to(fc_rain_lbl_times[i], fc_rain_chart, LV_ALIGN_OUT_BOTTOM_LEFT, (chart_w * i) / 4 - 8, 4);
        }
    }

    fc_rain_lbl_empty = lv_label_create(fc_rain_cont);
    lv_obj_set_style_text_color(fc_rain_lbl_empty, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(fc_rain_lbl_empty, SF(18), 0);
    lv_label_set_text(fc_rain_lbl_empty, tr("Geen neerslag verwacht", "No rain expected"));
    lv_obj_align(fc_rain_lbl_empty, LV_ALIGN_CENTER, 0, 15);
    lv_obj_add_flag(fc_rain_lbl_empty, LV_OBJ_FLAG_HIDDEN);

    /* Floating selector pill bar at top of radar_frame */
    lv_obj_t * pill_bar = lv_obj_create(radar_frame);
    lv_obj_set_size(pill_bar, rf_sz - 16, 34);
    lv_obj_align(pill_bar, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_color(pill_bar, lv_color_hex(0x101722), 0);
    lv_obj_set_style_bg_opa(pill_bar, LV_OPA_80, 0);
    lv_obj_set_style_border_color(pill_bar, lv_color_hex(0x223344), 0);
    lv_obj_set_style_border_width(pill_bar, 1, 0);
    lv_obj_set_style_radius(pill_bar, 10, 0);
    lv_obj_set_style_pad_all(pill_bar, 3, 0);
    lv_obj_clear_flag(pill_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    int btn_w = (rf_sz - 16 - 18) / 3;
    btn_view_radar = lv_btn_create(pill_bar);
    lv_obj_set_size(btn_view_radar, btn_w, 26);
    lv_obj_align(btn_view_radar, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_radius(btn_view_radar, 6, 0);
    lv_obj_set_ext_click_area(btn_view_radar, 6);
    lv_obj_add_event_cb(btn_view_radar, on_view_btn_click, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    lv_obj_t * lr = lv_label_create(btn_view_radar);
    lv_label_set_text(lr, tr("Radar", "Radar"));
    lv_obj_set_style_text_font(lr, SF(13), 0);
    lv_obj_center(lr);

    btn_view_3h = lv_btn_create(pill_bar);
    lv_obj_set_size(btn_view_3h, btn_w, 26);
    lv_obj_align(btn_view_3h, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(btn_view_3h, 6, 0);
    lv_obj_set_ext_click_area(btn_view_3h, 6);
    lv_obj_add_event_cb(btn_view_3h, on_view_btn_click, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t * l3 = lv_label_create(btn_view_3h);
    lv_label_set_text(l3, tr("3 uur", "3 hours"));
    lv_obj_set_style_text_font(l3, SF(13), 0);
    lv_obj_center(l3);

    btn_view_24h = lv_btn_create(pill_bar);
    lv_obj_set_size(btn_view_24h, btn_w, 26);
    lv_obj_align(btn_view_24h, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_radius(btn_view_24h, 6, 0);
    lv_obj_set_ext_click_area(btn_view_24h, 6);
    lv_obj_add_event_cb(btn_view_24h, on_view_btn_click, LV_EVENT_CLICKED, (void *)(intptr_t)2);
    lv_obj_t * l24 = lv_label_create(btn_view_24h);
    lv_label_set_text(l24, tr("24 uur", "24 hours"));
    lv_obj_set_style_text_font(l24, SF(13), 0);
    lv_obj_center(l24);

    update_forecast_view_mode();

    /* Right side: title + body text */
    int right_x = rf_x + rf_sz + (DISP_VER < 600 ? 12 : 20);
    int right_w = DISP_HOR - right_x - (DISP_VER < 600 ? 12 : 18);
    lbl_title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_title, (DISP_VER < 600 ? SF(22) : SF(28)), 0);
    lv_obj_set_width(lbl_title, right_w - 76);
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, right_x, rf_y);
    lv_label_set_text(lbl_title, tr("Weersverwachting", "Weather forecast"));

    /* Body text — vertically-scrollable container cleanly below header (title + close button) */
    int body_y = (DISP_VER < 600) ? 58 : 68;
    int body_h = (rf_y + rf_sz) - body_y;
    lv_obj_t * body_scroll = lv_obj_create(scr_root);
    lv_obj_set_size(body_scroll, right_w, body_h);
    lv_obj_align(body_scroll, LV_ALIGN_TOP_LEFT, right_x, body_y);
    lv_obj_set_style_bg_opa(body_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body_scroll, 0, 0);
    lv_obj_set_style_pad_all(body_scroll, 0, 0);
    lv_obj_set_scroll_dir(body_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body_scroll, LV_SCROLLBAR_MODE_AUTO);
    lbl_body = lv_label_create(body_scroll);
    lv_obj_set_style_text_color(lbl_body, lv_color_hex(settings.home_light_mode ? 0x334455 : 0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_body, SF(18), 0);
    lv_obj_set_width(lbl_body, right_w - 12);
    lv_label_set_long_mode(lbl_body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_body, tr("(laden...)", "(loading...)"));

    /* 5-day forecast strip — compact height to avoid whitespace, cleanly below radar & text */
    int col_w = (DISP_HOR - 20) / WEATHER_FORECAST_DAYS;
    int strip_y = (DISP_VER < 600) ? 384 : 476;
    int strip_h = (DISP_VER < 600) ? 86  : 112;
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        lv_obj_t * col = lv_obj_create(scr_root);
        lv_obj_set_size(col, col_w - 8, strip_h);
        lv_obj_set_pos(col, 10 + i * col_w, strip_y);
        lv_obj_set_style_bg_color(col, lv_color_hex(THEME_CARD_BG), 0);
        lv_obj_set_style_radius(col, 10, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_pad_all(col, 6, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(col, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(col, 4);
        lv_obj_set_style_bg_color(col, lv_color_hex(settings.home_light_mode ? 0xccdae8 : 0x223348), LV_STATE_PRESSED);
        lv_obj_add_event_cb(col, on_day_card_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* Centred big icon */
        fc_icon[i] = lv_img_create(col);
        lv_img_set_src(fc_icon[i], &icon_wx_cloud_lg);
        lv_obj_set_style_img_recolor(fc_icon[i], lv_color_hex(settings.home_light_mode ? 0x64748b : 0xddeeff), 0);
        lv_obj_set_style_img_recolor_opa(fc_icon[i], 255, 0);
        if (DISP_VER < 600) {
            lv_img_set_zoom(fc_icon[i], 120);
            lv_obj_align(fc_icon[i], LV_ALIGN_CENTER, 0, 5);
        } else {
            lv_img_set_zoom(fc_icon[i], 160);
            lv_obj_align(fc_icon[i], LV_ALIGN_CENTER, 0, 7);
        }

        /* Wind-direction arrow + Bft at the bottom of each column. */
        fc_wind_arrow[i] = lv_img_create(col);
        lv_img_set_src(fc_wind_arrow[i], &icon_wind_arrow);
        lv_img_set_pivot(fc_wind_arrow[i], 16, 16);
        lv_obj_align(fc_wind_arrow[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);

        fc_wind_lbl[i] = lv_label_create(col);
        lv_obj_set_style_text_color(fc_wind_lbl[i], lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(fc_wind_lbl[i], (DISP_VER < 600) ? SF(14) : SF(16), 0);
        lv_label_set_text(fc_wind_lbl[i], "");
        lv_obj_align(fc_wind_lbl[i], LV_ALIGN_BOTTOM_LEFT, (DISP_VER < 600) ? 20 : 26, -1);

        /* Day and temperature stacked vertically to avoid horizontal overlap */
        fc_day_lbl[i] = lv_label_create(col);
        lv_obj_set_style_text_color(fc_day_lbl[i], lv_color_hex(THEME_TEXT_HI), 0);
        lv_obj_set_style_text_font(fc_day_lbl[i], (DISP_VER < 600) ? SF(14) : SF(18), 0);
        lv_label_set_text(fc_day_lbl[i], "--");
        lv_obj_align(fc_day_lbl[i], LV_ALIGN_TOP_MID, 0, (DISP_VER < 600 ? 1 : 2));

        fc_temp_lbl[i] = lv_label_create(col);
        lv_obj_set_style_text_color(fc_temp_lbl[i], lv_color_hex(THEME_WARN_ORANGE), 0);
        lv_obj_set_style_text_font(fc_temp_lbl[i], (DISP_VER < 600) ? SF(14) : SF(18), 0);
        lv_label_set_text(fc_temp_lbl[i], "");
        lv_obj_align(fc_temp_lbl[i], LV_ALIGN_TOP_MID, 0, (DISP_VER < 600 ? 17 : 22));

        /* Verbose desc moved to the title body — match the home band. */
        fc_desc_lbl[i] = NULL;
    }

    lv_obj_move_foreground(close_btn);
    refresh_cb(NULL);
    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 5000, NULL);
    return scr_root;
}
