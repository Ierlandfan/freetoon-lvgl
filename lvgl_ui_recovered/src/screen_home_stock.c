/*
 * screen_home_stock.c — pixel-faithful clone of the stock qt-gui light home
 * (the grey tile-carousel "active" screen), rebuilt in LVGL.
 *
 * Geometry and palette are lifted verbatim from the extracted qt-gui resource
 * bundle (themes/Colors.qml, themes/Fonts.qml) and measured against a real
 * 1024x600 capture (/tmp/toon2_stock.png):
 *
 *   canvas      #DCDCDC      tile card   #FFFFFF      header strip #F0F0F0
 *   title text  #565656      body text   #000000      accent       #E64F0A
 *   alert red   #FF1744      secondary   #8C8C8C       pressed      #A8A8A8
 *
 *   page-1 layout (1024x600, measured):
 *     top bar            y   0..96   (apps-grid · TOON · status glyphs)
 *     tile clock         x  41..334  y  97..264
 *     tile humidity      x 348..641  y  97..264
 *     tile stroom-nu     x  41..334  y 281..448
 *     tile waterdruk     x 348..641  y 281..448
 *     thermostat panel   x 673..989  y  97..561
 *     carousel/dots      y 540..575
 *
 * This is the freetoon "stock theme" home. It reads the same live data as the
 * dark default home (toon_state / meter_state / hw_state) and drives the same
 * BoxTalk control verbs, so it is a faithful reskin, not a static mockup.
 *
 * Selected when settings.home_theme == HOME_THEME_STOCK.
 */
#include "screens.h"
#include "i18n.h"
#include "display.h"
#include "fonts_opensans.h"
#include "boxtalk.h"
#include "icons.h"
#include "settings.h"
#include "meteradapter.h"
#include "homewizard.h"
#include "weather.h"
#include "ventilation.h"
#include "calendar.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* ---- stock palette ------------------------------------------------------- */
#define C_CANVAS   0xDCDCDC
#define C_CARD     0xFFFFFF
#define C_HEADER   0xF0F0F0
#define C_TITLE    0x565656
#define C_TEXT     0x000000
#define C_ACCENT   0xE64F0A
#define C_ALERT    0xFF1744
#define C_SECOND   0x8C8C8C
#define C_PRESSED  0xA8A8A8

/* Design-space (1024x600) geometry, scaled to the panel via SX()/SY(). */
#define BAR_H      96
#define G_X0       41          /* left card left edge   */
#define G_COLW     293         /* card width            */
#define G_GAPX     14          /* gap between columns    */
#define G_Y0       97          /* top row top edge      */
#define G_ROWH     167         /* card height           */
#define G_GAPY     17
#define G_X1       (G_X0 + G_COLW + G_GAPX)   /* right column left = 348 */
#define G_Y1       (G_Y0 + G_ROWH + G_GAPY)   /* bottom row top    = 281 */
#define SP_X0      673
#define SP_W       316
#define SP_Y0      97
#define SP_H       464

static lv_obj_t * scr_root = NULL;
static lv_obj_t * lbl_clock, * lbl_date, * lbl_humid, * lbl_watt, * lbl_water;
static lv_obj_t * lbl_setpoint, * lbl_setpoint_lo, * lbl_prog, * water_banner;
static lv_obj_t * scene_btn[4], * scene_lbl[4];
#define N_ESEG 12
static lv_obj_t * eseg[N_ESEG];        /* segmented "stroom nu" gauge */
static lv_timer_t * refresh_timer = NULL;

/* carousel: the left tile region is an lv_tileview paging through tile-sets;
 * the top bar + thermostat panel stay fixed, exactly like stock qt-gui. */
#define N_PAGES 5
static lv_obj_t * tv = NULL;
static lv_obj_t * pages[N_PAGES];
static lv_obj_t * dots[N_PAGES];
static lv_obj_t * p2_lbl[4];           /* page-2 readouts (Binnen/aanvoer/retour/CV)  */
static lv_obj_t * p3_lbl[4];           /* page-3 weather: [0]=temp [1]=desc [2]=sub */
static lv_obj_t * p3_icon = NULL;      /* page-3 weather condition icon */
static lv_obj_t * p4_lbl[4];           /* page-4 air+vent (CO2/TVOC/vent/kwaliteit)   */
static lv_obj_t * p5_agenda = NULL;    /* page-5 agenda list                          */

extern toon_state_t toon_state;
extern meter_state_t meter_state;
extern hw_state_t    hw_state;

static float cur_power_w(void) {
    return settings.energy_source == 0 ? meter_state.power_w : hw_state.power_w;
}

/* ---- small builders ------------------------------------------------------ */

/* A white rounded card at design-space rect. */
static lv_obj_t * card(lv_obj_t * par, int x, int y, int w, int h) {
    lv_obj_t * c = lv_obj_create(par);
    lv_obj_set_pos(c, SX(x), SY(y));
    lv_obj_set_size(c, SX(w), SY(h));
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, SX(6), 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_shadow_width(c, SX(6), 0);
    lv_obj_set_style_shadow_ofs_y(c, SY(2), 0);
    lv_obj_set_style_shadow_opa(c, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* Tile title (grey, top-centred), as on every stock card. */
static lv_obj_t * tile_title(lv_obj_t * t, const char * txt) {
    lv_obj_t * l = lv_label_create(t);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, OSR(16), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TITLE), 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, SY(14));
    return l;
}

/* font is an Open Sans face (OSL/OSR/OSS) or SF() for LV_SYMBOL glyphs. */
static lv_obj_t * mklabel(lv_obj_t * par, const char * txt, const lv_font_t * font, uint32_t col) {
    lv_obj_t * l = lv_label_create(par);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    return l;
}

/* ---- control callbacks --------------------------------------------------- */
static void on_sp_up(lv_event_t * e)   { (void)e; boxtalk_setpoint_increase(); }
static void on_sp_down(lv_event_t * e) { (void)e; boxtalk_setpoint_decrease(); }
static void on_prog_toggle(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_target(e);
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) boxtalk_resume_schedule();
    else                                        boxtalk_set_manual();
}
/* scene order on screen: Weg(3) Thuis(1) / Slapen(2) Comfort(0) */
static const int scene_state[4] = { 3, 1, 2, 0 };
static void on_scene(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    boxtalk_set_program(scene_state[idx]);
}
/* apps-grid → open Settings (the only nav off the stock home, so the user can
 * reach the layout editor / theme toggle / everything else). */
static void on_apps_grid(lv_event_t * e) { (void)e; ui_push(screen_settings_create()); }

/* Tap-through: a tile opens its detail/graph screen, like the stock qt-gui. */
enum { LINK_STATS = 1, LINK_FORECAST, LINK_HEATER, LINK_VENT };
static void on_tile_link(lv_event_t * e) {
    switch ((int)(intptr_t)lv_event_get_user_data(e)) {
        case LINK_STATS:    ui_push(screen_stats_create());           break;
        case LINK_FORECAST: ui_push(screen_forecast_create());        break;
        case LINK_HEATER:   ui_push(screen_heater_advanced_create()); break;
        case LINK_VENT:     ui_push(screen_vent_advanced_create());   break;
    }
}
/* Make a card tappable → detail screen. Inside the swipeable tileview a tap
 * (no drag) fires CLICKED while a drag still scrolls pages. */
static void tile_link(lv_obj_t * card, int code) {
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, on_tile_link, LV_EVENT_CLICKED, (void *)(intptr_t)code);
}

/* ---- live refresh -------------------------------------------------------- */
static void refresh_page2(void);
static void refresh_page3(void);
static void refresh_page4(void);
static void refresh_page5(void);
static void refresh_cb(lv_timer_t * t) {
    (void)t;
    time_t now = time(NULL);
    struct tm tmv; localtime_r(&now, &tmv);
    char buf[64];

    snprintf(buf, sizeof buf, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    lv_label_set_text(lbl_clock, buf);

    static const char * mnd[12] = {"januari","februari","maart","april","mei","juni",
        "juli","augustus","september","oktober","november","december"};
    snprintf(buf, sizeof buf, "%d %s %d", tmv.tm_mday, mnd[tmv.tm_mon], tmv.tm_year + 1900);
    lv_label_set_text(lbl_date, buf);

    if (toon_state.humidity > 0) { snprintf(buf, sizeof buf, "%.0f%%", toon_state.humidity);
        lv_label_set_text(lbl_humid, buf); } else lv_label_set_text(lbl_humid, "--%");

    snprintf(buf, sizeof buf, "%.0f Watt", cur_power_w());
    lv_label_set_text(lbl_watt, buf);

    float wp = toon_state.water_pressure;
    snprintf(buf, sizeof buf, "%.1f bar", wp);
    /* qt-gui shows comma decimals */
    for (char * p = buf; *p; p++) if (*p == '.') *p = ',';
    lv_label_set_text(lbl_water, buf);
    int low = (wp >= 0 && wp < 1.0f);
    lv_obj_set_style_text_color(lbl_water, lv_color_hex(low ? C_ALERT : C_TITLE), 0);
    if (water_banner) (low ? lv_obj_clear_flag : lv_obj_add_flag)(water_banner, LV_OBJ_FLAG_HIDDEN);

    /* setpoint big + control hint */
    snprintf(buf, sizeof buf, "%.1f°", toon_state.setpoint);
    for (char * p = buf; *p; p++) if (*p == '.') *p = ',';
    lv_label_set_text(lbl_setpoint, buf);
    snprintf(buf, sizeof buf, "%.1f°", toon_state.indoor_temp);
    for (char * p = buf; *p; p++) if (*p == '.') *p = ',';
    lv_label_set_text(lbl_setpoint_lo, buf);

    /* program pill text + active scene highlight (orange) */
    int manual = (toon_state.active_state < 0);
    lv_label_set_text(lbl_prog, manual ? tr("Programma uit", "Program off")
                                       : tr("Programma aan", "Program on"));
    for (int i = 0; i < 4; i++) {
        int active = (!manual && toon_state.active_state == scene_state[i]);
        lv_obj_set_style_text_color(scene_lbl[i],
            lv_color_hex(active ? C_ACCENT : C_TITLE), 0);
        lv_obj_set_style_bg_color(scene_btn[i],
            lv_color_hex(active ? C_HEADER : C_CARD), 0);
    }

    /* segmented gauge: light up the bottom k of N, green low → amber high,
     * exactly like qt-gui's stroom-nu column. */
    {
        float w = cur_power_w(); if (w < 0) w = 0;
        int lit = (int)(w / 2500.0f * N_ESEG + 0.5f);
        if (lit > N_ESEG) lit = N_ESEG;
        for (int i = 0; i < N_ESEG; i++) {
            int from_bottom = N_ESEG - 1 - i;       /* 0 = bottom segment */
            int on = (from_bottom < lit);
            uint32_t col = !on ? 0xE0E0E0
                         : (from_bottom < 7 ? 0x689F38 : 0xCCCC33);  /* green→amber */
            lv_obj_set_style_bg_color(eseg[i], lv_color_hex(col), 0);
        }
    }

    refresh_page2();
    refresh_page3();
    refresh_page4();
    refresh_page5();
}

/* ---- carousel paging ----------------------------------------------------- */
static int active_page(void) {
    lv_obj_t * act = lv_tileview_get_tile_act(tv);
    for (int i = 0; i < N_PAGES; i++) if (pages[i] == act) return i;
    return 0;
}
static void update_dots(void) {
    int idx = active_page();
    for (int i = 0; i < N_PAGES; i++)
        lv_obj_set_style_bg_color(dots[i], lv_color_hex(i == idx ? C_ACCENT : C_PRESSED), 0);
}
static void on_tv_change(lv_event_t * e) { (void)e; update_dots(); }
static void on_arrow(lv_event_t * e) {
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    int idx = active_page() + dir;
    if (idx < 0) idx = 0;
    if (idx >= N_PAGES) idx = N_PAGES - 1;
    lv_obj_set_tile(tv, pages[idx], LV_ANIM_ON);
    update_dots();
}

/* page-2 readout refresh (boiler/indoor temps from toon_state). */
static void refresh_page2(void) {
    char b[32];
    const float v[4] = { toon_state.indoor_temp, toon_state.boiler_in_temp,
                         toon_state.boiler_out_temp, toon_state.water_pressure };
    for (int i = 0; i < 4; i++) {
        if (!p2_lbl[i]) continue;
        if (i == 3) snprintf(b, sizeof b, "%.1f bar", v[i]);
        else        snprintf(b, sizeof b, "%.1f°", v[i]);
        for (char * p = b; *p; p++) if (*p == '.') *p = ',';
        lv_label_set_text(p2_lbl[i], b);
    }
}

/* comma-decimal helper */
static void comma(char * s) { for (; *s; s++) if (*s == '.') *s = ','; }

static void refresh_page3(void) {   /* weather — single tile */
    if (!p3_lbl[0]) return;
    char b[80];
    snprintf(b, sizeof b, "%.1f°", weather_state.current_temp); comma(b);
    lv_label_set_text(p3_lbl[0], b);
    lv_label_set_text(p3_lbl[1], weather_state.current_desc[0] ? weather_state.current_desc
                                                               : tr("Buiten", "Outside"));
    if (weather_state.day_count > 0) {
        const weather_day_t * d = &weather_state.days[0];
        snprintf(b, sizeof b, tr("min %.0f°   max %.0f°    %d Bft %s    %d%% neerslag",
                                 "min %.0f°   max %.0f°    %d Bft %s    %d%% rain"),
                 d->min_temp, d->max_temp, d->wind_bft, d->wind_dir, d->rain_chance);
        lv_label_set_text(p3_lbl[2], b);
    }
    if (p3_icon && weather_state.current_icon[0]) {
        lv_img_set_src(p3_icon, weather_icon_for_lg(weather_state.current_icon));
        lv_obj_set_style_img_recolor(p3_icon,
            lv_color_hex(weather_icon_color_for(weather_state.current_icon)), 0);
        lv_obj_set_style_img_recolor_opa(p3_icon, LV_OPA_COVER, 0);
    }
}
static void refresh_page4(void) {   /* air quality + ventilation */
    if (!p4_lbl[0]) return;
    char b[40];
    snprintf(b, sizeof b, "%d ppm", toon_state.eco2);  lv_label_set_text(p4_lbl[0], b);
    snprintf(b, sizeof b, "%d ppb", toon_state.tvoc);  lv_label_set_text(p4_lbl[1], b);
    if (vent_state.connected) snprintf(b, sizeof b, "%d%%", vent_state.speed_pct);
    else                      snprintf(b, sizeof b, "--");
    lv_label_set_text(p4_lbl[2], b);
    lv_label_set_text(p4_lbl[3], air_quality_label(toon_state.eco2, toon_state.tvoc));
    lv_obj_set_style_text_color(p4_lbl[3],
        lv_color_hex(air_quality_color(toon_state.eco2, toon_state.tvoc)), 0);
}
static void refresh_page5(void) {   /* agenda list */
    if (!p5_agenda) return;
    char buf[512]; buf[0] = 0; int n = calendar_state.count; if (n > 5) n = 5;
    if (n == 0) {
        snprintf(buf, sizeof buf, "%s", tr("Geen afspraken", "No events"));
    } else for (int i = 0; i < n; i++) {
        const calendar_event_t * e = &calendar_state.ev[i];
        char line[128];
        snprintf(line, sizeof line, "%s%s  %s\n",
                 e->date + 5, e->time[0] ? "" : "", e->summary);  /* MM-DD  summary */
        strncat(buf, line, sizeof buf - strlen(buf) - 1);
    }
    lv_label_set_text(p5_agenda, buf);
}

/* A white card at tile-local (col,row) in the 2x2 left grid. */
static lv_obj_t * lcard(lv_obj_t * tile, int col, int row) {
    return card(tile, col * (G_COLW + G_GAPX), row * (G_ROWH + G_GAPY), G_COLW, G_ROWH);
}

/* ---- native-drawn stock icons (no image assets) -------------------------- */
/* A teardrop: pointed apex at (cx,ty), rounded belly of radius r. */
static void cv_teardrop(lv_obj_t * cv, int cx, int ty, int r, uint32_t color) {
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color); d.bg_opa = LV_OPA_COVER;
    lv_point_t p[5] = {
        { cx,             ty },
        { cx + r,         ty + (r * 7) / 5 },
        { cx + (r*7)/10,  ty + (r * 11) / 5 },
        { cx - (r*7)/10,  ty + (r * 11) / 5 },
        { cx - r,         ty + (r * 7) / 5 },
    };
    lv_canvas_draw_polygon(cv, p, 5, &d);
}

/* Orange house silhouette with three white drops — qt-gui's humidity tile icon. */
static lv_obj_t * make_humidity_icon(lv_obj_t * par) {
    static uint8_t buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(72, 64)] __attribute__((aligned(4)));
    lv_obj_t * cv = lv_canvas_create(par);
    lv_canvas_set_buffer(cv, buf, 72, 64, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_canvas_fill_bg(cv, lv_color_hex(C_CARD), LV_OPA_TRANSP);
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(C_ACCENT); d.bg_opa = LV_OPA_COVER; d.radius = 2;
    lv_canvas_draw_rect(cv, 16, 27, 40, 33, &d);                 /* body  */
    lv_point_t roof[3] = { {4, 30}, {36, 3}, {68, 30} };
    lv_canvas_draw_polygon(cv, roof, 3, &d);                     /* roof  */
    cv_teardrop(cv, 28, 30, 5, C_CARD);                          /* drops */
    cv_teardrop(cv, 44, 30, 5, C_CARD);
    cv_teardrop(cv, 36, 42, 5, C_CARD);
    return cv;
}

/* Two-leaf green sprout — qt-gui's eco indicator next to the setpoint. */
static lv_obj_t * make_sprout_icon(lv_obj_t * par) {
    static uint8_t buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(24, 24)] __attribute__((aligned(4)));
    lv_obj_t * cv = lv_canvas_create(par);
    lv_canvas_set_buffer(cv, buf, 24, 24, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_canvas_fill_bg(cv, lv_color_hex(C_CARD), LV_OPA_TRANSP);
    lv_draw_rect_dsc_t st; lv_draw_rect_dsc_init(&st);
    st.bg_color = lv_color_hex(0x2E7D32); st.bg_opa = LV_OPA_COVER;
    lv_canvas_draw_rect(cv, 11, 9, 2, 13, &st);                  /* stem  */
    /* two leaves: angled teardrops sprouting left and right */
    lv_draw_rect_dsc_t lf; lv_draw_rect_dsc_init(&lf);
    lf.bg_color = lv_color_hex(0x689F38); lf.bg_opa = LV_OPA_COVER;
    lv_point_t left[4]  = { {12, 12}, {2, 9}, {5, 3}, {12, 8} };
    lv_point_t right[4] = { {12, 12}, {22, 9}, {19, 3}, {12, 8} };
    lv_canvas_draw_polygon(cv, left, 4, &lf);
    lv_canvas_draw_polygon(cv, right, 4, &lf);
    return cv;
}

/* ---- build --------------------------------------------------------------- */
lv_obj_t * screen_home_stock_create(void) {
    if (scr_root) return scr_root;   /* cache like the other screens */
    scr_root = lv_obj_create(NULL);
    lv_obj_set_size(scr_root, lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL));
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(C_CANVAS), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr_root, 0, 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- top bar: apps-grid · TOON · status glyphs ---- */
    /* apps-grid (3x3 dots) */
    lv_obj_t * grid = lv_obj_create(scr_root);
    lv_obj_set_pos(grid, SX(34), SY(28));
    lv_obj_set_size(grid, SX(34), SY(34));
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(grid, SX(18));   /* generous finger target */
    lv_obj_add_event_cb(grid, on_apps_grid, LV_EVENT_CLICKED, NULL);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
        lv_obj_t * d = lv_obj_create(grid);
        lv_obj_set_size(d, SX(6), SY(6));
        lv_obj_set_pos(d, SX(c * 13), SY(r * 13));
        lv_obj_set_style_radius(d, SX(3), 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(C_TITLE), 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_pad_all(d, 0, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);  /* taps reach the apps-grid */
    }
    /* Freetoon wordmark (stock UI shows "TOON" here; this is the freetoon build) */
    lv_obj_t * brand = mklabel(scr_root, "Freetoon", OSS(28), C_TITLE);
    lv_obj_set_style_text_letter_space(brand, SX(2), 0);
    lv_obj_set_pos(brand, SX(120), SY(28));

    /* ===== left carousel: lv_tileview holding the paged tile-sets ===== */
    tv = lv_tileview_create(scr_root);
    lv_obj_set_pos(tv, SX(G_X0), SY(G_Y0));
    lv_obj_set_size(tv, SX(2 * G_COLW + G_GAPX), SY(2 * G_ROWH + G_GAPY + 4));
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tv, 0, 0);
    lv_obj_set_style_pad_all(tv, 0, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(tv, on_tv_change, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * page1 = lv_tileview_add_tile(tv, 0, 0, LV_DIR_HOR);
    lv_obj_set_style_pad_all(page1, 0, 0);
    lv_obj_set_scrollbar_mode(page1, LV_SCROLLBAR_MODE_OFF);
    pages[0] = page1;

    /* ===== tile 1: clock ===== */
    lv_obj_t * t_clock = lcard(page1, 0, 0);
    lbl_clock = mklabel(t_clock, "--:--", OSL(50), C_TITLE);
    lv_obj_align(lbl_clock, LV_ALIGN_CENTER, 0, SY(-12));
    lbl_date = mklabel(t_clock, "", OSR(15), C_TITLE);
    lv_obj_align(lbl_date, LV_ALIGN_CENTER, 0, SY(34));

    /* ===== tile 2: humidity (Luchtvochtigheid) ===== */
    lv_obj_t * t_hum = lcard(page1, 1, 0);
    tile_title(t_hum, tr("Luchtvochtigheid", "Humidity"));
    lv_obj_t * hum_icon = make_humidity_icon(t_hum);
    lv_obj_align(hum_icon, LV_ALIGN_CENTER, 0, SY(-6));
    lbl_humid = mklabel(t_hum, "--%", OSL(30), C_TITLE);
    lv_obj_align(lbl_humid, LV_ALIGN_BOTTOM_MID, 0, SY(-14));

    /* ===== tile 3: stroom nu ===== */
    lv_obj_t * t_pow = lcard(page1, 0, 1);
    tile_link(t_pow, LINK_STATS);   /* → energy/gas/water graphs */
    tile_title(t_pow, tr("Stroom nu", "Power now"));
    {   /* segmented vertical gauge, 12 stacked cells */
        int seg_w = 26, seg_h = 5, seg_gap = 2;
        int total_h = N_ESEG * seg_h + (N_ESEG - 1) * seg_gap;
        for (int i = 0; i < N_ESEG; i++) {
            eseg[i] = lv_obj_create(t_pow);
            lv_obj_set_size(eseg[i], SX(seg_w), SY(seg_h));
            lv_obj_align(eseg[i], LV_ALIGN_CENTER,
                         0, SY(-4 - total_h / 2 + i * (seg_h + seg_gap) + seg_h / 2));
            lv_obj_set_style_radius(eseg[i], SX(1), 0);
            lv_obj_set_style_border_width(eseg[i], 0, 0);
            lv_obj_set_style_bg_color(eseg[i], lv_color_hex(0xE0E0E0), 0);
            lv_obj_set_style_pad_all(eseg[i], 0, 0);
            lv_obj_clear_flag(eseg[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(eseg[i], LV_OBJ_FLAG_CLICKABLE);  /* let taps reach the tile */
        }
    }
    lbl_watt = mklabel(t_pow, "-- Watt", OSL(30), C_TITLE);
    lv_obj_align(lbl_watt, LV_ALIGN_BOTTOM_MID, 0, SY(-14));

    /* ===== tile 4: waterdruk ===== */
    lv_obj_t * t_wat = lcard(page1, 1, 1);
    tile_link(t_wat, LINK_STATS);   /* → graphs (incl. water) */
    tile_title(t_wat, tr("Waterdruk", "Water pressure"));
    lbl_water = mklabel(t_wat, "-- bar", OSL(40), C_TITLE);
    lv_obj_align(lbl_water, LV_ALIGN_CENTER, 0, SY(-2));
    water_banner = lv_obj_create(t_wat);
    lv_obj_set_size(water_banner, SX(G_COLW - 24), SY(46));
    lv_obj_align(water_banner, LV_ALIGN_CENTER, 0, SY(18));
    lv_obj_set_style_bg_color(water_banner, lv_color_hex(C_ALERT), 0);
    lv_obj_set_style_radius(water_banner, SX(4), 0);
    lv_obj_set_style_border_width(water_banner, 0, 0);
    lv_obj_clear_flag(water_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(water_banner, LV_OBJ_FLAG_CLICKABLE);  /* tap passes to the tile */
    lv_obj_t * wb = mklabel(water_banner, tr("Waterdruk te laag. Ketel bijvullen", "Water pressure too low. Refill boiler"), OSR(13), C_CARD);
    lv_label_set_long_mode(wb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(wb, SX(G_COLW - 40));
    lv_obj_set_style_text_align(wb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(wb);
    lv_obj_add_flag(water_banner, LV_OBJ_FLAG_HIDDEN);

    /* ===== page 2: indoor + boiler temps (freetoon data, stock styling) ===== */
    lv_obj_t * page2 = lv_tileview_add_tile(tv, 1, 0, LV_DIR_HOR);
    lv_obj_set_style_pad_all(page2, 0, 0);
    lv_obj_set_scrollbar_mode(page2, LV_SCROLLBAR_MODE_OFF);
    pages[1] = page2;
    {
        const char * p2t[4] = { tr("Binnen", "Indoor"), tr("Ketel aanvoer", "Boiler flow"),
                                tr("Ketel retour", "Boiler return"), tr("CV-druk", "CH pressure") };
        const int    col[4] = { 0, 1, 0, 1 }, row[4] = { 0, 0, 1, 1 };
        for (int i = 0; i < 4; i++) {
            lv_obj_t * c = lcard(page2, col[i], row[i]);
            tile_link(c, LINK_HEATER);   /* → boiler/heating detail */
            tile_title(c, p2t[i]);
            p2_lbl[i] = mklabel(c, "--,-°", OSL(40), C_TITLE);
            lv_obj_align(p2_lbl[i], LV_ALIGN_CENTER, 0, SY(6));
        }
    }

    /* ===== page 3: weather ===== */
    lv_obj_t * page3 = lv_tileview_add_tile(tv, 2, 0, LV_DIR_HOR);
    lv_obj_set_style_pad_all(page3, 0, 0);
    lv_obj_set_scrollbar_mode(page3, LV_SCROLLBAR_MODE_OFF);
    pages[2] = page3;
    {   /* one weather tile (like the stock Toon): icon + current temp + a
         * min/max·wind·rain sub-line; tap → full forecast. */
        lv_obj_t * c = card(page3, 0, 0, 2 * G_COLW + G_GAPX, 2 * G_ROWH + G_GAPY);
        tile_link(c, LINK_FORECAST);
        tile_title(c, tr("Weer", "Weather"));
        p3_icon = lv_img_create(c);
        lv_img_set_src(p3_icon, &icon_wx_cloud_lg);
        lv_obj_align(p3_icon, LV_ALIGN_LEFT_MID, SX(48), SY(-6));
        lv_obj_clear_flag(p3_icon, LV_OBJ_FLAG_CLICKABLE);
        p3_lbl[0] = mklabel(c, "--°", OSL(50), C_TITLE);      /* current temp */
        lv_obj_align(p3_lbl[0], LV_ALIGN_LEFT_MID, SX(210), SY(-24));
        p3_lbl[1] = mklabel(c, tr("Buiten", "Outside"), OSR(20), C_SECOND);
        lv_obj_align(p3_lbl[1], LV_ALIGN_LEFT_MID, SX(212), SY(26));
        p3_lbl[2] = mklabel(c, "", OSR(18), C_TITLE);          /* min/max · wind · rain */
        lv_obj_align(p3_lbl[2], LV_ALIGN_BOTTOM_MID, 0, SY(-28));
        p3_lbl[3] = NULL;
    }

    /* ===== page 4: air quality + ventilation ===== */
    lv_obj_t * page4 = lv_tileview_add_tile(tv, 3, 0, LV_DIR_HOR);
    lv_obj_set_style_pad_all(page4, 0, 0);
    lv_obj_set_scrollbar_mode(page4, LV_SCROLLBAR_MODE_OFF);
    pages[3] = page4;
    {
        const char * t[4] = { "CO2", "TVOC", tr("Ventilatie", "Ventilation"),
                              tr("Luchtkwaliteit", "Air quality") };
        const int col[4] = {0,1,0,1}, row[4] = {0,0,1,1};
        for (int i = 0; i < 4; i++) {
            lv_obj_t * c = lcard(page4, col[i], row[i]);
            if (i == 2) tile_link(c, LINK_VENT);   /* Ventilatie → vent detail */
            tile_title(c, t[i]);
            p4_lbl[i] = mklabel(c, "--", OSR(24), C_TITLE);
            lv_obj_align(p4_lbl[i], LV_ALIGN_CENTER, 0, SY(6));
        }
    }

    /* ===== page 5: agenda (one full card listing next events) ===== */
    lv_obj_t * page5 = lv_tileview_add_tile(tv, 4, 0, LV_DIR_HOR);
    lv_obj_set_style_pad_all(page5, 0, 0);
    lv_obj_set_scrollbar_mode(page5, LV_SCROLLBAR_MODE_OFF);
    pages[4] = page5;
    {
        lv_obj_t * c = card(page5, 0, 0, 2 * G_COLW + G_GAPX, 2 * G_ROWH + G_GAPY);
        tile_title(c, tr("Agenda", "Calendar"));
        p5_agenda = lv_label_create(c);
        lv_label_set_long_mode(p5_agenda, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(p5_agenda, SX(2 * G_COLW + G_GAPX - 48));
        lv_obj_set_style_text_font(p5_agenda, OSR(18), 0);
        lv_obj_set_style_text_color(p5_agenda, lv_color_hex(C_TITLE), 0);
        lv_obj_set_style_text_line_space(p5_agenda, SY(10), 0);
        lv_obj_align(p5_agenda, LV_ALIGN_TOP_LEFT, SX(24), SY(52));
        lv_label_set_text(p5_agenda, "");
    }

    /* ===== thermostat side panel ===== */
    lv_obj_t * panel = card(scr_root, SP_X0, SP_Y0, SP_W, SP_H);
    /* header strip */
    lv_obj_t * hdr = lv_obj_create(panel);
    lv_obj_set_size(hdr, SX(SP_W), SY(58));
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(C_HEADER), 0);
    lv_obj_set_style_radius(hdr, SX(6), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * rad = lv_img_create(hdr);
    lv_img_set_src(rad, &icon_radiator);
    lv_obj_set_style_img_recolor(rad, lv_color_hex(C_SECOND), 0);
    lv_obj_set_style_img_recolor_opa(rad, LV_OPA_COVER, 0);
    lv_obj_center(rad);

    /* setpoint readout (left) + sub-temp */
    lbl_setpoint = mklabel(panel, "--,-°", OSL(50), C_TITLE);
    lv_obj_set_pos(lbl_setpoint, SX(20), SY(78));
    lv_obj_t * tri = mklabel(panel, LV_SYMBOL_DOWN, SF(13), C_SECOND);
    lv_obj_set_pos(tri, SX(22), SY(140));
    lbl_setpoint_lo = mklabel(panel, "--,-°", OSR(15), C_SECOND);
    lv_obj_set_pos(lbl_setpoint_lo, SX(42), SY(138));
    lv_obj_t * sprout = make_sprout_icon(panel);
    lv_obj_set_pos(sprout, SX(96), SY(134));

    /* +/- stacked buttons (right) */
    lv_obj_t * bplus = lv_btn_create(panel);
    lv_obj_set_size(bplus, SX(86), SY(54));
    lv_obj_set_pos(bplus, SX(SP_W - 100), SY(72));
    lv_obj_set_style_bg_color(bplus, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_shadow_width(bplus, 0, 0);
    lv_obj_set_style_border_color(bplus, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_border_width(bplus, SX(1), 0);
    lv_obj_add_event_cb(bplus, on_sp_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t * pl = mklabel(bplus, "+", OSL(40), C_TITLE); lv_obj_center(pl);
    lv_obj_t * bminus = lv_btn_create(panel);
    lv_obj_set_size(bminus, SX(86), SY(54));
    lv_obj_set_pos(bminus, SX(SP_W - 100), SY(132));
    lv_obj_set_style_bg_color(bminus, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_shadow_width(bminus, 0, 0);
    lv_obj_set_style_border_color(bminus, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_border_width(bminus, SX(1), 0);
    lv_obj_add_event_cb(bminus, on_sp_down, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ml = mklabel(bminus, "-", OSL(40), C_TITLE); lv_obj_center(ml);

    /* "Programma uit" + toggle */
    lbl_prog = mklabel(panel, tr("Programma uit", "Program off"), OSR(15), C_TITLE);
    lv_obj_set_pos(lbl_prog, SX(20), SY(214));
    lv_obj_t * sw = lv_switch_create(panel);
    lv_obj_set_size(sw, SX(56), SY(28));
    lv_obj_set_pos(sw, SX(SP_W - 76), SY(210));
    lv_obj_set_style_bg_color(sw, lv_color_hex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, on_prog_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    /* scene 2x2: Weg Thuis / Slapen Comfort */
    const char * names[4] = { tr("Weg","Away"), tr("Thuis","Home"),
                              tr("Slapen","Sleep"), tr("Comfort","Comfort") };
    int sw_w = (SP_W - 30) / 2, sh = 74;
    for (int i = 0; i < 4; i++) {
        int col = i % 2, row = i / 2;
        scene_btn[i] = lv_btn_create(panel);
        lv_obj_set_size(scene_btn[i], SX(sw_w), SY(sh));
        lv_obj_set_pos(scene_btn[i], SX(10 + col * (sw_w + 5)), SY(256 + row * (sh + 5)));
        lv_obj_set_style_bg_color(scene_btn[i], lv_color_hex(C_CARD), 0);
        lv_obj_set_style_shadow_width(scene_btn[i], 0, 0);
        lv_obj_set_style_border_color(scene_btn[i], lv_color_hex(0xE0E0E0), 0);
        lv_obj_set_style_border_width(scene_btn[i], SX(1), 0);
        lv_obj_set_style_radius(scene_btn[i], SX(3), 0);
        lv_obj_add_event_cb(scene_btn[i], on_scene, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        scene_lbl[i] = mklabel(scene_btn[i], names[i], OSR(20), C_TITLE);
        lv_obj_center(scene_lbl[i]);
    }

    /* ===== carousel arrows + page dots (control the tileview) ===== */
    lv_obj_t * la = mklabel(scr_root, LV_SYMBOL_LEFT, SF(28), C_TITLE);
    lv_obj_set_pos(la, SX(56), SY(540));
    lv_obj_add_flag(la, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(la, SX(24));
    lv_obj_add_event_cb(la, on_arrow, LV_EVENT_CLICKED, (void*)(intptr_t)-1);
    lv_obj_t * ra = mklabel(scr_root, LV_SYMBOL_RIGHT, SF(28), C_TITLE);
    lv_obj_set_pos(ra, SX(596), SY(540));
    lv_obj_add_flag(ra, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(ra, SX(24));
    lv_obj_add_event_cb(ra, on_arrow, LV_EVENT_CLICKED, (void*)(intptr_t)1);

    /* N_PAGES dots, centred under the left tile region */
    int cx = G_X0 + (2 * G_COLW + G_GAPX) / 2;   /* left-region centre x */
    int dot_gap = 24;
    int dx0 = cx - ((N_PAGES - 1) * dot_gap) / 2 - 5;
    for (int i = 0; i < N_PAGES; i++) {
        dots[i] = lv_obj_create(scr_root);
        lv_obj_set_size(dots[i], SX(10), SY(10));
        lv_obj_set_pos(dots[i], SX(dx0 + i * dot_gap), SY(550));
        lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dots[i], lv_color_hex(i == 0 ? C_ACCENT : C_PRESSED), 0);
        lv_obj_set_style_border_width(dots[i], 0, 0);
        lv_obj_set_style_pad_all(dots[i], 0, 0);
        lv_obj_clear_flag(dots[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    update_dots();

    refresh_cb(NULL);
    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}
