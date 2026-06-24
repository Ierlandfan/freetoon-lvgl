/*
 * screen_dim_stock.c — stock-qt-gui-style standby (dim) screen, shown when
 * settings.home_theme == 1 (stock theme). Mirrors the stock Toon dim layout:
 *
 *   Freetoon (top-left) · big clock (left) · vertical energy bar (left-centre)
 *   Waterdruk + value + low-pressure alert (centre) · big setpoint + eco +
 *   program status (right).
 *
 * Black background, Open Sans, live data. Selected from screen_dim_create();
 * the freetoon dark dim (screen_dim.c) is still used for the dark theme.
 */
#include "screens.h"
#include "i18n.h"
#include "display.h"
#include "fonts_opensans.h"
#include "boxtalk.h"
#include "settings.h"
#include "meteradapter.h"
#include "homewizard.h"
#include "weather.h"
#include "icons.h"
#include "ventilation.h"
#include "efanlamp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define D_BG     0x000000
#define D_WHITE  0xFFFFFF
#define D_GREY   0x8C8C8C
#define D_RED    0xFF1744
#define D_GREEN  0x689F38

static lv_obj_t * scr_root = NULL;
static lv_obj_t * d_clock, * d_date, * d_water, * d_water_ts, * d_setpoint, * d_eco, * d_prog;
static lv_obj_t * d_water_banner, * d_watts;
static lv_obj_t * d_wx_icon, * d_wx_temp;   /* outside weather, top-right */
static lv_obj_t * d_vent_fan = NULL;         /* Itho spinning fan icon */
static lv_obj_t * d_vent_lbl = NULL;         /* Itho mode + % label */
static int        d_vent_period_ms = 0;
static lv_obj_t * d_efan_fan = NULL;         /* BLE fan spinning icon */
static lv_obj_t * d_efan_lbl = NULL;         /* BLE fan speed label */
static lv_obj_t * d_efan_light_lbl = NULL;   /* BLE lamp brightness label */
static lv_obj_t * d_efan_src_lbl = NULL;     /* BLE last source label */
static int        d_efan_period_ms = 0;
#define D_NSEG 12
static lv_obj_t * d_eseg[D_NSEG];
static lv_timer_t * d_timer = NULL;

extern toon_state_t  toon_state;
extern meter_state_t meter_state;
extern hw_state_t    hw_state;
const char * program_label(void);

static float d_power_w(void) {
    return settings.energy_source == 0 ? meter_state.power_w : hw_state.power_w;
}
static void d_comma(char * s) { for (; *s; s++) if (*s == '.') *s = ','; }

static lv_obj_t * d_lbl(lv_obj_t * par, const char * txt, const lv_font_t * font, uint32_t col) {
    lv_obj_t * l = lv_label_create(par);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    return l;
}

/* green two-leaf sprout (same as the stock home eco mark) */
static lv_obj_t * d_make_sprout(lv_obj_t * par) {
    static uint8_t buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(24, 24)] __attribute__((aligned(4)));
    lv_obj_t * cv = lv_canvas_create(par);
    lv_canvas_set_buffer(cv, buf, 24, 24, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_canvas_fill_bg(cv, lv_color_hex(D_BG), LV_OPA_TRANSP);
    lv_draw_rect_dsc_t st; lv_draw_rect_dsc_init(&st);
    st.bg_color = lv_color_hex(0x2E7D32); st.bg_opa = LV_OPA_COVER;
    lv_canvas_draw_rect(cv, 11, 9, 2, 13, &st);
    lv_draw_rect_dsc_t lf; lv_draw_rect_dsc_init(&lf);
    lf.bg_color = lv_color_hex(D_GREEN); lf.bg_opa = LV_OPA_COVER;
    lv_point_t left[4]  = { {12, 12}, {2, 9}, {5, 3}, {12, 8} };
    lv_point_t right[4] = { {12, 12}, {22, 9}, {19, 3}, {12, 8} };
    lv_canvas_draw_polygon(cv, left, 4, &lf);
    lv_canvas_draw_polygon(cv, right, 4, &lf);
    return cv;
}

static void d_vent_anim_cb(void * obj, int32_t v) { lv_img_set_angle((lv_obj_t *)obj, v); }
static void d_efan_anim_cb(void * obj, int32_t v) { lv_img_set_angle((lv_obj_t *)obj, v); }

/* speed_level 1-6 → rotation period in ms (faster at higher level) */
static void d_efan_apply_anim(int speed_level, int on) {
    if (!d_efan_fan) return;
    if (!on || speed_level < 1) {
        if (d_efan_period_ms == 0) return;
        d_efan_period_ms = 0;
        lv_anim_del(d_efan_fan, NULL);
        return;
    }
    /* level 1 = 2200 ms/rev, level 6 = 400 ms/rev, linear */
    int period = 2200 - (speed_level - 1) * 300;
    if (period < 400)  period = 400;
    if (period > 2200) period = 2200;
    if (abs(period - d_efan_period_ms) < 100) return;
    d_efan_period_ms = period;
    lv_anim_del(d_efan_fan, NULL);
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, d_efan_fan);
    lv_anim_set_exec_cb(&a, d_efan_anim_cb);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_time(&a, period);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void d_vent_apply_anim(int rpm) {
    if (!d_vent_fan) return;
    if (rpm < 50) {
        if (d_vent_period_ms == 0) return;
        d_vent_period_ms = 0;
        lv_anim_del(d_vent_fan, NULL);
        return;
    }
    int period = 2600 - (rpm - 1000) * 13 / 10;
    if (period < 280)  period = 280;
    if (period > 2600) period = 2600;
    if (abs(period - d_vent_period_ms) < 100) return;
    d_vent_period_ms = period;
    lv_anim_del(d_vent_fan, NULL);
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, d_vent_fan);
    lv_anim_set_exec_cb(&a, d_vent_anim_cb);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_time(&a, period);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void on_wake(lv_event_t * e) { (void)e; ui_wake_now(); }
static void efanlamp_fan_toggle_cb(lv_event_t * e) { (void)e; efanlamp_fan_toggle(); }

static void d_refresh(lv_timer_t * t) {
    (void)t;
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char b[64];

    snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(d_clock, b);
    static const char * mnd[12] = {"januari","februari","maart","april","mei","juni",
        "juli","augustus","september","oktober","november","december"};
    snprintf(b, sizeof b, "%d %s", tm.tm_mday, mnd[tm.tm_mon]);
    lv_label_set_text(d_date, b);

    /* water pressure + low alert — wp<=0 means no reading (boxtalk swallows
     * zeros), so show -- without the false "te laag" alarm. */
    float wp = toon_state.water_pressure;
    int have = (wp > 0.05f);
    if (have) { snprintf(b, sizeof b, "%.1f bar", wp); d_comma(b); } else strcpy(b, "-- bar");
    lv_label_set_text(d_water, b);
    int low = (have && wp < 1.0f);
    lv_obj_set_style_text_color(d_water, lv_color_hex(low ? D_RED : D_WHITE), 0);
    (low ? lv_obj_clear_flag : lv_obj_add_flag)(d_water_banner, LV_OBJ_FLAG_HIDDEN);
    snprintf(b, sizeof b, "%02d-%02d-%04d %02d:%02d:00",
             tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
    lv_label_set_text(d_water_ts, b);

    /* big readout = indoor temp (default) or setpoint, per stock_big_indoor */
    float big_v = settings.stock_big_indoor ? toon_state.indoor_temp : toon_state.setpoint;
    float lo_v  = settings.stock_big_indoor ? toon_state.setpoint     : toon_state.indoor_temp;
    snprintf(b, sizeof b, "%.1f", big_v); d_comma(b);
    lv_label_set_text(d_setpoint, b);
    if (lo_v > 0) { snprintf(b, sizeof b, LV_SYMBOL_RIGHT "  %.1f°", lo_v); d_comma(b); }
    else strcpy(b, LV_SYMBOL_RIGHT "  --");
    lv_label_set_text(d_eco, b);
    /* "Continue on X,X" — stock Toon wording for the held setpoint. */
    { char sp[16]; snprintf(sp, sizeof sp, "%.1f", toon_state.setpoint); d_comma(sp);
      snprintf(b, sizeof b, tr("Verder op %s", "Continue on %s"), sp);
      lv_label_set_text(d_prog, b); }

    /* current outside weather (top-right): white icon + temp, hidden until a
     * forecast lands or if the dim-weather toggle is off. */
    if (d_wx_icon && d_wx_temp) {
        if (settings.show_dim_weather && weather_state.connected) {
            lv_img_set_src(d_wx_icon, weather_icon_for_lg(weather_state.current_icon));
            snprintf(b, sizeof b, "%.0f°C", weather_state.current_temp);
            lv_label_set_text(d_wx_temp, b);
            lv_obj_clear_flag(d_wx_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(d_wx_icon, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(d_wx_temp, "");
        }
    }

    /* fan spinner */
    if (d_vent_fan && d_vent_lbl) {
        if (vent_state.connected) {
            const char * pretty = vent_mode_label();
            if (vent_state.remaining_min > 0)
                lv_label_set_text_fmt(d_vent_lbl, "%s %dm %d%%",
                                      pretty, vent_state.remaining_min, vent_state.speed_pct);
            else
                lv_label_set_text_fmt(d_vent_lbl, "%s %d%%",
                                      pretty, vent_state.speed_pct);
            d_vent_apply_anim(vent_state.fan_rpm);
            lv_obj_clear_flag(d_vent_fan, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(d_vent_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(d_vent_fan, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(d_vent_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* BLE fan spinner */
    if (d_efan_fan && d_efan_lbl) {
        if (efanlamp.connected) {
            d_efan_apply_anim(efanlamp.fan_speed, efanlamp.fan_on);
            if (efanlamp.fan_on)
                lv_label_set_text_fmt(d_efan_lbl, tr("L%d", "L%d"), efanlamp.fan_speed);
            else
                lv_label_set_text(d_efan_lbl, tr("off", "off"));
            lv_obj_clear_flag(d_efan_fan, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(d_efan_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            d_efan_apply_anim(0, 0);
            lv_obj_add_flag(d_efan_fan, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(d_efan_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* BLE lamp brightness */
    if (d_efan_light_lbl) {
        if (efanlamp.connected) {
            if (efanlamp.light_on)
                lv_label_set_text_fmt(d_efan_light_lbl, tr("lamp %d%%", "lamp %d%%"), efanlamp.light_brightness);
            else
                lv_label_set_text(d_efan_light_lbl, tr("lamp uit", "lamp off"));
            lv_obj_clear_flag(d_efan_light_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(d_efan_light_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* BLE last source */
    if (d_efan_src_lbl) {
        if (efanlamp.connected && efanlamp.last_source[0]) {
            lv_label_set_text(d_efan_src_lbl, (const char *)efanlamp.last_source);
            lv_obj_clear_flag(d_efan_src_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(d_efan_src_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* energy bar + live watts number */
    float pw = d_power_w();
    snprintf(b, sizeof b, "%.0f W", pw); lv_label_set_text(d_watts, b);
    float w = pw; if (w < 0) w = 0;
    int lit = (int)(w / 2500.0f * D_NSEG + 0.5f); if (lit > D_NSEG) lit = D_NSEG;
    for (int i = 0; i < D_NSEG; i++) {
        int fb = D_NSEG - 1 - i;
        uint32_t c = (fb < lit) ? (fb < 7 ? D_GREEN : 0xCCCC33) : 0x3A3A3A;
        lv_obj_set_style_bg_color(d_eseg[i], lv_color_hex(c), 0);
    }
}

lv_obj_t * screen_dim_stock_create(void) {
    if (scr_root) return scr_root;
    scr_root = lv_obj_create(NULL);
    lv_obj_set_size(scr_root, lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL));
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(D_BG), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr_root, 0, 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_root, on_wake, LV_EVENT_CLICKED, NULL);

    /* Freetoon wordmark */
    lv_obj_t * brand = d_lbl(scr_root, "Freetoon", OSS(28), D_WHITE);
    lv_obj_set_style_text_letter_space(brand, SX(2), 0);
    lv_obj_set_pos(brand, SX(24), SY(20));

    /* clock (left) + date */
    d_clock = d_lbl(scr_root, "--:--", OSL(90), D_WHITE);
    lv_obj_set_pos(d_clock, SX(86), SY(148));
    d_date = d_lbl(scr_root, "", OSR(20), D_GREY);
    lv_obj_set_pos(d_date, SX(92), SY(262));

    /* vertical energy bar (left-centre) */
    {
        int seg_w = 30, seg_h = 6, gap = 3, x = 172, y0 = 356;
        for (int i = 0; i < D_NSEG; i++) {
            d_eseg[i] = lv_obj_create(scr_root);
            lv_obj_set_size(d_eseg[i], SX(seg_w), SY(seg_h));
            lv_obj_set_pos(d_eseg[i], SX(x), SY(y0 + i * (seg_h + gap)));
            lv_obj_set_style_radius(d_eseg[i], SX(1), 0);
            lv_obj_set_style_border_width(d_eseg[i], 0, 0);
            lv_obj_set_style_bg_color(d_eseg[i], lv_color_hex(0x3A3A3A), 0);
            lv_obj_set_style_pad_all(d_eseg[i], 0, 0);
            lv_obj_clear_flag(d_eseg[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(d_eseg[i], LV_OBJ_FLAG_CLICKABLE);
        }
    }

    /* live power (W) centred under the energy bar (bar centre x≈187) */
    d_watts = d_lbl(scr_root, "", OSR(20), D_GREY);
    lv_obj_set_width(d_watts, SX(120));
    lv_obj_set_style_text_align(d_watts, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(d_watts, SX(127), SY(470));

    /* Waterdruk (centre) */
    lv_obj_t * wlab = d_lbl(scr_root, tr("Waterdruk", "Water pressure"), OSR(24), D_WHITE);
    lv_obj_set_pos(wlab, SX(360), SY(322));
    d_water = d_lbl(scr_root, "-- bar", OSL(50), D_WHITE);
    lv_obj_set_pos(d_water, SX(360), SY(366));
    d_water_ts = d_lbl(scr_root, "", OSR(13), D_GREY);
    lv_obj_set_pos(d_water_ts, SX(360), SY(484));
    /* low-pressure alert banner (over the value) */
    d_water_banner = lv_obj_create(scr_root);
    lv_obj_set_size(d_water_banner, SX(282), SY(56));
    lv_obj_set_pos(d_water_banner, SX(356), SY(414));
    lv_obj_set_style_bg_color(d_water_banner, lv_color_hex(D_RED), 0);
    lv_obj_set_style_radius(d_water_banner, SX(4), 0);
    lv_obj_set_style_border_width(d_water_banner, 0, 0);
    lv_obj_clear_flag(d_water_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(d_water_banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t * wb = d_lbl(d_water_banner,
        tr("Waterdruk te laag. Ketel bijvullen", "Water pressure too low. Refill boiler"),
        OSR(15), D_WHITE);
    lv_label_set_long_mode(wb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(wb, SX(258));
    lv_obj_set_style_text_align(wb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(wb);
    lv_obj_add_flag(d_water_banner, LV_OBJ_FLAG_HIDDEN);

    /* setpoint (right) + eco + program */
    d_setpoint = d_lbl(scr_root, "--,-", OSL(90), D_WHITE);
    lv_obj_set_pos(d_setpoint, SX(690), SY(252));
    d_eco = d_lbl(scr_root, LV_SYMBOL_RIGHT "  --", OSR(24), D_WHITE);
    lv_obj_set_style_text_font(d_eco, SF(22), 0);   /* LV_SYMBOL lives in Montserrat */
    lv_obj_set_pos(d_eco, SX(690), SY(388));
    lv_obj_t * sprout = d_make_sprout(scr_root);
    lv_obj_set_pos(sprout, SX(782), SY(386));
    d_prog = d_lbl(scr_root, "", OSR(24), D_WHITE);
    lv_obj_set_pos(d_prog, SX(690), SY(424));

    /* Outside weather, top-right corner — temp left of an all-white 80x80 icon,
     * mirroring the "Freetoon" brand at top-left. Recolor is set once here so
     * the icon is always white regardless of the weather type. */
    /* OSR (regular) font — the OSL light faces are a digits-only subset and lack
     * the ° and C glyphs (they'd render as missing-glyph boxes). */
    d_wx_temp = d_lbl(scr_root, "", OSR(30), D_WHITE);
    lv_obj_set_width(d_wx_temp, SX(150));
    lv_obj_set_style_text_align(d_wx_temp, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(d_wx_temp, SX(762), SY(38));
    d_wx_icon = lv_img_create(scr_root);
    lv_obj_set_pos(d_wx_icon, SX(924), SY(12));
    lv_obj_set_style_img_recolor(d_wx_icon, lv_color_hex(0xffffff), 0);   /* all white */
    lv_obj_set_style_img_recolor_opa(d_wx_icon, LV_OPA_COVER, 0);
    lv_obj_add_flag(d_wx_icon, LV_OBJ_FLAG_HIDDEN);

    /* Fan spinner — to the RIGHT of the big temperature, vertically centred on it.
     * OSL(90) digits span y=252..317 (cap→baseline, box_h≈65) → body centre ≈ y=285.
     * With pivot (40,40) the icon's rendered centre = pos_y + 40 (pivot stays fixed
     * under zoom), so pos_y = 285-40 = 245 to centre it on the numerals. */
    d_vent_fan = lv_img_create(scr_root);
    lv_img_set_src(d_vent_fan, &icon_fan);
    lv_img_set_zoom(d_vent_fan, 192);   /* 192/256 × 80 = 60 px */
    lv_obj_set_style_img_recolor(d_vent_fan, lv_color_hex(D_WHITE), 0);
    lv_obj_set_style_img_recolor_opa(d_vent_fan, LV_OPA_COVER, 0);
    lv_img_set_pivot(d_vent_fan, 40, 40);
    lv_obj_set_pos(d_vent_fan, SX(912), SY(245));   /* centre ≈ (952, 285) */
    lv_obj_add_flag(d_vent_fan, LV_OBJ_FLAG_HIDDEN);

    d_vent_lbl = d_lbl(scr_root, "", OSR(20), D_GREY);
    lv_obj_set_width(d_vent_lbl, SX(160));
    lv_obj_set_style_text_align(d_vent_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(d_vent_lbl, SX(882), SY(322));   /* below the 60px icon */
    lv_obj_add_flag(d_vent_lbl, LV_OBJ_FLAG_HIDDEN);

    /* BLE fan spinner — below Itho vent, same column */
    d_efan_fan = lv_img_create(scr_root);
    lv_img_set_src(d_efan_fan, &icon_fan);
    lv_img_set_zoom(d_efan_fan, 192);   /* 60 px */
    lv_obj_set_style_img_recolor(d_efan_fan, lv_color_hex(0x4FC3F7), 0);  /* light-blue tint */
    lv_obj_set_style_img_recolor_opa(d_efan_fan, LV_OPA_COVER, 0);
    lv_img_set_pivot(d_efan_fan, 40, 40);
    lv_obj_set_pos(d_efan_fan, SX(912), SY(370));
    lv_obj_add_flag(d_efan_fan, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(d_efan_fan, (lv_event_cb_t)efanlamp_fan_toggle_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(d_efan_fan, LV_OBJ_FLAG_CLICKABLE);

    d_efan_lbl = d_lbl(scr_root, "", OSR(20), D_GREY);
    lv_obj_set_width(d_efan_lbl, SX(160));
    lv_obj_set_style_text_align(d_efan_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(d_efan_lbl, SX(882), SY(438));
    lv_obj_add_flag(d_efan_lbl, LV_OBJ_FLAG_HIDDEN);

    /* BLE lamp label — amber, below fan label */
    d_efan_light_lbl = d_lbl(scr_root, "", OSR(20), 0xE6C200);
    lv_obj_set_width(d_efan_light_lbl, SX(160));
    lv_obj_set_style_text_align(d_efan_light_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(d_efan_light_lbl, SX(882), SY(468));
    lv_obj_add_flag(d_efan_light_lbl, LV_OBJ_FLAG_HIDDEN);

    /* BLE source label — dim grey, below light label */
    d_efan_src_lbl = d_lbl(scr_root, "", OSR(16), 0x667788);
    lv_obj_set_width(d_efan_src_lbl, SX(160));
    lv_obj_set_style_text_align(d_efan_src_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(d_efan_src_lbl, SX(882), SY(494));
    lv_obj_add_flag(d_efan_src_lbl, LV_OBJ_FLAG_HIDDEN);

    d_refresh(NULL);
    d_timer = lv_timer_create(d_refresh, 1000, NULL);
    return scr_root;
}
