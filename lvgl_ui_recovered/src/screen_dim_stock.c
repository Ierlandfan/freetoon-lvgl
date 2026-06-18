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
#include <stdio.h>
#include <string.h>
#include <time.h>

#define D_BG     0x000000
#define D_WHITE  0xFFFFFF
#define D_GREY   0x8C8C8C
#define D_RED    0xFF1744
#define D_GREEN  0x689F38

static lv_obj_t * scr_root = NULL;
static lv_obj_t * d_clock, * d_date, * d_water, * d_water_ts, * d_setpoint, * d_eco, * d_prog;
static lv_obj_t * d_water_banner;
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

static void on_wake(lv_event_t * e) { (void)e; ui_wake_now(); }

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

    /* water pressure + low alert */
    float wp = toon_state.water_pressure;
    snprintf(b, sizeof b, "%.1f bar", wp); d_comma(b);
    lv_label_set_text(d_water, b);
    int low = (wp >= 0 && wp < 1.0f);
    lv_obj_set_style_text_color(d_water, lv_color_hex(low ? D_RED : D_WHITE), 0);
    (low ? lv_obj_clear_flag : lv_obj_add_flag)(d_water_banner, LV_OBJ_FLAG_HIDDEN);
    snprintf(b, sizeof b, "%02d-%02d-%04d %02d:%02d:00",
             tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
    lv_label_set_text(d_water_ts, b);

    /* setpoint + eco + program */
    snprintf(b, sizeof b, "%.1f", toon_state.setpoint); d_comma(b);
    lv_label_set_text(d_setpoint, b);
    if (toon_state.indoor_temp > 0) { snprintf(b, sizeof b, LV_SYMBOL_RIGHT "  %.1f°", toon_state.indoor_temp); d_comma(b); }
    else strcpy(b, LV_SYMBOL_RIGHT "  --");
    lv_label_set_text(d_eco, b);
    lv_label_set_text(d_prog, program_label());

    /* energy bar */
    float w = d_power_w(); if (w < 0) w = 0;
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

    d_refresh(NULL);
    d_timer = lv_timer_create(d_refresh, 1000, NULL);
    return scr_root;
}
