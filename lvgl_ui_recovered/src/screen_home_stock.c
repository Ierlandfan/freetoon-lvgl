/*
 * screen_home_stock.c — stock-qt-gui-style light home (the "stock theme"),
 * selected when settings.home_theme == 1.
 *
 * Like the stock Toon, the carousel is a grid of customizable tile slots:
 * every slot is long-press-swappable from a tile picker (clock, humidity,
 * power, water, indoor temp, CO2, TVOC, weather, boiler temps, gas, vent,
 * agenda). Layout persists in settings.stock_tiles. The top bar (Freetoon
 * wordmark + apps-grid → Settings) and the thermostat side panel are fixed.
 *
 * Geometry + palette lifted from the extracted qt-gui resource bundle; fonts
 * are Open Sans (fonts_opensans.h).
 */
#include "screens.h"
#include "i18n.h"
#include "display.h"
#include "fonts_opensans.h"
#include "boxtalk.h"
#include "icons.h"
#include "settings.h"
#include "homeassistant.h"
#include "meteradapter.h"
#include "homewizard.h"
#include "weather.h"
#include "ventilation.h"
#include "calendar.h"
#include "domoticz.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

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

#define G_X0       41
#define G_COLW     293
#define G_GAPX     14
#define G_Y0       97
#define G_ROWH     167
#define G_GAPY     17
#define SP_X0      673
#define SP_W       316
#define SP_Y0      97
#define SP_H       464

#define N_PAGES 4
#define N_SLOTS (N_PAGES * 4)
#define N_ESEG  12

/* ---- tile types ---------------------------------------------------------- */
enum { LINK_NONE = 0, LINK_STATS, LINK_FORECAST, LINK_HEATER, LINK_VENT, LINK_TRACKERS };
typedef enum {
    TT_EMPTY = 0, TT_CLOCK, TT_HUMID, TT_POWER, TT_WATERP, TT_INDOOR,
    TT_CO2, TT_TVOC, TT_WEATHER, TT_BOILIN, TT_BOILOUT, TT_GAS, TT_VENT,
    TT_AGENDA, TT_TRACKERS, TT_COUNT
} ttype_t;
static const struct { const char * key; const char * nl; const char * en; int link; } TM[TT_COUNT] = {
    { "empty",   "Verwijderen",   "Remove",       LINK_NONE     },
    { "clock",   "Klok",          "Clock",        LINK_NONE     },
    { "humid",   "Luchtvochtigheid", "Humidity",  LINK_NONE     },
    { "power",   "Stroom nu",     "Power now",    LINK_STATS    },
    { "waterp",  "Waterdruk",     "Water pres.",  LINK_STATS    },
    { "indoor",  "Binnen",        "Indoor",       LINK_HEATER   },
    { "co2",     "CO2",           "CO2",          LINK_NONE     },
    { "tvoc",    "TVOC",          "TVOC",         LINK_NONE     },
    { "weather", "Weer",          "Weather",      LINK_FORECAST },
    { "boilin",  "Ketel aanvoer", "Boiler flow",  LINK_HEATER   },
    { "boilout", "Ketel retour",  "Boiler return",LINK_HEATER   },
    { "gas",     "Gas",           "Gas",          LINK_STATS    },
    { "vent",    "Ventilatie",    "Ventilation",  LINK_VENT     },
    { "agenda",  "Agenda",        "Calendar",     LINK_NONE     },
    { "trackers","Trackers",      "Trackers",     LINK_TRACKERS },
};

typedef struct {
    lv_obj_t * card, * title, * val, * sub, * icon, * banner;
    lv_obj_t * gauge[N_ESEG];
    int type, idx;
} slot_t;

/* ---- module state -------------------------------------------------------- */
static lv_obj_t * scr_root = NULL;
static lv_obj_t * tv = NULL;
static lv_obj_t * pages[N_PAGES];
static lv_obj_t * dots[N_PAGES];
static slot_t     slots[N_SLOTS];
static lv_obj_t * lbl_setpoint, * lbl_setpoint_lo, * lbl_prog;
static lv_obj_t * scene_btn[4], * scene_lbl[4];
static lv_timer_t * refresh_timer = NULL;
static lv_point_t   g_press_pt;
static int          g_edit_slot = -1;
static lv_obj_t *   g_picker = NULL;

extern toon_state_t  toon_state;
extern meter_state_t meter_state;
extern hw_state_t    hw_state;

static const char * MND[12] = {"januari","februari","maart","april","mei","juni",
    "juli","augustus","september","oktober","november","december"};

static float cur_power_w(void) { return settings.energy_source == 0 ? meter_state.power_w : hw_state.power_w; }
static void  comma(char * s) { for (; *s; s++) if (*s == '.') *s = ','; }

/* ---- builders ------------------------------------------------------------ */
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
static lv_obj_t * lcard(lv_obj_t * tile, int col, int row) {
    return card(tile, col * (G_COLW + G_GAPX), row * (G_ROWH + G_GAPY), G_COLW, G_ROWH);
}
static lv_obj_t * tile_title(lv_obj_t * t, const char * txt) {
    lv_obj_t * l = lv_label_create(t);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, OSR(16), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TITLE), 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, SY(14));
    return l;
}
static lv_obj_t * mklabel(lv_obj_t * par, const char * txt, const lv_font_t * font, uint32_t col) {
    lv_obj_t * l = lv_label_create(par);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    return l;
}

/* ---- native-drawn icons -------------------------------------------------- */
static void cv_teardrop(lv_obj_t * cv, int cx, int ty, int r, uint32_t color) {
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color); d.bg_opa = LV_OPA_COVER;
    lv_point_t p[5] = { {cx, ty}, {cx + r, ty + (r*7)/5}, {cx + (r*7)/10, ty + (r*11)/5},
                        {cx - (r*7)/10, ty + (r*11)/5}, {cx - r, ty + (r*7)/5} };
    lv_canvas_draw_polygon(cv, p, 5, &d);
}
static lv_obj_t * make_humidity_icon(lv_obj_t * par) {
    static uint8_t buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(72, 64)] __attribute__((aligned(4)));
    lv_obj_t * cv = lv_canvas_create(par);
    lv_canvas_set_buffer(cv, buf, 72, 64, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_canvas_fill_bg(cv, lv_color_hex(C_CARD), LV_OPA_TRANSP);
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(C_ACCENT); d.bg_opa = LV_OPA_COVER; d.radius = 2;
    lv_canvas_draw_rect(cv, 16, 27, 40, 33, &d);
    lv_point_t roof[3] = { {4, 30}, {36, 3}, {68, 30} };
    lv_canvas_draw_polygon(cv, roof, 3, &d);
    cv_teardrop(cv, 28, 30, 5, C_CARD); cv_teardrop(cv, 44, 30, 5, C_CARD); cv_teardrop(cv, 36, 42, 5, C_CARD);
    lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    return cv;
}
static lv_obj_t * make_sprout_icon(lv_obj_t * par) {
    static uint8_t buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(24, 24)] __attribute__((aligned(4)));
    lv_obj_t * cv = lv_canvas_create(par);
    lv_canvas_set_buffer(cv, buf, 24, 24, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_canvas_fill_bg(cv, lv_color_hex(C_CARD), LV_OPA_TRANSP);
    lv_draw_rect_dsc_t st; lv_draw_rect_dsc_init(&st);
    st.bg_color = lv_color_hex(0x2E7D32); st.bg_opa = LV_OPA_COVER;
    lv_canvas_draw_rect(cv, 11, 9, 2, 13, &st);
    lv_draw_rect_dsc_t lf; lv_draw_rect_dsc_init(&lf);
    lf.bg_color = lv_color_hex(0x689F38); lf.bg_opa = LV_OPA_COVER;
    lv_point_t left[4]  = { {12, 12}, {2, 9}, {5, 3}, {12, 8} };
    lv_point_t right[4] = { {12, 12}, {22, 9}, {19, 3}, {12, 8} };
    lv_canvas_draw_polygon(cv, left, 4, &lf);
    lv_canvas_draw_polygon(cv, right, 4, &lf);
    return cv;
}

/* ---- thermostat control callbacks ---------------------------------------- */
static void on_sp_up(lv_event_t * e)   { (void)e; boxtalk_setpoint_increase(); }
static void on_sp_down(lv_event_t * e) { (void)e; boxtalk_setpoint_decrease(); }
static void on_prog_toggle(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_target(e);
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) boxtalk_resume_schedule();
    else                                        boxtalk_set_manual();
}
static const int scene_state[4] = { 3, 1, 2, 0 };   /* Weg Thuis / Slapen Comfort */
static void on_scene(lv_event_t * e) { boxtalk_set_program(scene_state[(int)(intptr_t)lv_event_get_user_data(e)]); }
static void on_apps_grid(lv_event_t * e) { (void)e; ui_push(screen_settings_create()); }

/* ---- collapsible lights handle (mirrors LVGL home screen) ---------------- */
static lv_obj_t * s_lights_handle     = NULL;
static lv_obj_t * s_lights_handle_lbl = NULL;

static void open_lights_backend(void) {
    if (settings.enable_domoticz) ui_push(screen_domoticz_create());
    else                          ui_push(screen_lights_create());
}
static void s_lights_handle_set(bool open) {
    if (!s_lights_handle) return;
    lv_obj_set_size(s_lights_handle, open ? SX(160) : SX(56), open ? SY(56) : SY(80));
    lv_obj_align(s_lights_handle, LV_ALIGN_LEFT_MID, open ? SX(4) : -SX(4), 0);
    lv_obj_set_style_radius(s_lights_handle, open ? SX(16) : LV_RADIUS_CIRCLE, 0);
    if (s_lights_handle_lbl) {
        lv_label_set_text(s_lights_handle_lbl,
                          open ? tr("Apparaten", "Devices") : LV_SYMBOL_CHARGE);
        lv_obj_set_style_text_font(s_lights_handle_lbl, open ? SF(22) : SF(28), 0);
    }
}
static void on_lights_handle(lv_event_t * e) {
    switch (lv_event_get_code(e)) {
        case LV_EVENT_PRESSED:                             s_lights_handle_set(true);  break;
        case LV_EVENT_RELEASED: case LV_EVENT_PRESS_LOST: s_lights_handle_set(false); break;
        case LV_EVENT_CLICKED: s_lights_handle_set(false); open_lights_backend();     break;
        default: break;
    }
}

/* ---- slot tile rendering + refresh --------------------------------------- */
static void render_slot(slot_t * s) {
    lv_obj_clean(s->card);
    s->title = s->val = s->sub = s->icon = s->banner = NULL;
    for (int i = 0; i < N_ESEG; i++) s->gauge[i] = NULL;
    int t = s->type;
    if (t == TT_EMPTY) {   /* faint "+" — long-press (or tap) to add a tile */
        lv_obj_t * plus = mklabel(s->card, "+", OSL(50), 0xCCCCCC);
        lv_obj_center(plus);
        return;
    }
    if (t != TT_CLOCK) s->title = tile_title(s->card, tr(TM[t].nl, TM[t].en));
    switch (t) {
    case TT_CLOCK:
        s->val = mklabel(s->card, "--:--", OSL(50), C_TITLE); lv_obj_align(s->val, LV_ALIGN_CENTER, 0, SY(-12));
        s->sub = mklabel(s->card, "", OSR(15), C_TITLE);      lv_obj_align(s->sub, LV_ALIGN_CENTER, 0, SY(34));
        break;
    case TT_HUMID:
        s->icon = make_humidity_icon(s->card); lv_obj_align(s->icon, LV_ALIGN_CENTER, 0, SY(-6));
        s->val = mklabel(s->card, "--%", OSL(30), C_TITLE);   lv_obj_align(s->val, LV_ALIGN_BOTTOM_MID, 0, SY(-14));
        break;
    case TT_POWER: {
        int sw = 26, sh = 5, gp = 2, th = N_ESEG * sh + (N_ESEG - 1) * gp;
        for (int i = 0; i < N_ESEG; i++) {
            s->gauge[i] = lv_obj_create(s->card);
            lv_obj_set_size(s->gauge[i], SX(sw), SY(sh));
            lv_obj_align(s->gauge[i], LV_ALIGN_CENTER, 0, SY(-4 - th / 2 + i * (sh + gp) + sh / 2));
            lv_obj_set_style_radius(s->gauge[i], SX(1), 0);
            lv_obj_set_style_border_width(s->gauge[i], 0, 0);
            lv_obj_set_style_bg_color(s->gauge[i], lv_color_hex(0xE0E0E0), 0);
            lv_obj_set_style_pad_all(s->gauge[i], 0, 0);
            lv_obj_clear_flag(s->gauge[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(s->gauge[i], LV_OBJ_FLAG_CLICKABLE);
        }
        s->val = mklabel(s->card, "-- Watt", OSL(30), C_TITLE); lv_obj_align(s->val, LV_ALIGN_BOTTOM_MID, 0, SY(-14));
        break; }
    case TT_WATERP: {
        s->val = mklabel(s->card, "-- bar", OSL(40), C_TITLE); lv_obj_align(s->val, LV_ALIGN_CENTER, 0, SY(-2));
        s->banner = lv_obj_create(s->card);
        lv_obj_set_size(s->banner, SX(G_COLW - 24), SY(46));
        lv_obj_align(s->banner, LV_ALIGN_CENTER, 0, SY(18));
        lv_obj_set_style_bg_color(s->banner, lv_color_hex(C_ALERT), 0);
        lv_obj_set_style_radius(s->banner, SX(4), 0);
        lv_obj_set_style_border_width(s->banner, 0, 0);
        lv_obj_clear_flag(s->banner, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(s->banner, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t * wb = mklabel(s->banner, tr("Waterdruk te laag. Ketel bijvullen", "Water pressure too low. Refill boiler"), OSR(13), C_CARD);
        lv_label_set_long_mode(wb, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(wb, SX(G_COLW - 40));
        lv_obj_set_style_text_align(wb, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(wb);
        lv_obj_add_flag(s->banner, LV_OBJ_FLAG_HIDDEN);
        break; }
    case TT_WEATHER:
        s->icon = lv_img_create(s->card);
        lv_img_set_src(s->icon, &icon_wx_cloud_lg);
        lv_obj_align(s->icon, LV_ALIGN_CENTER, 0, SY(-26));
        lv_obj_clear_flag(s->icon, LV_OBJ_FLAG_CLICKABLE);
        s->val = mklabel(s->card, "--°", OSL(30), C_TITLE);   lv_obj_align(s->val, LV_ALIGN_BOTTOM_MID, 0, SY(-34));
        s->sub = mklabel(s->card, "", OSR(15), C_SECOND);     lv_obj_align(s->sub, LV_ALIGN_BOTTOM_MID, 0, SY(-12));
        break;
    case TT_AGENDA:
        s->val = mklabel(s->card, "", OSR(16), C_TITLE);
        lv_label_set_long_mode(s->val, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s->val, SX(G_COLW - 28));
        lv_obj_set_style_text_align(s->val, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s->val, LV_ALIGN_CENTER, 0, SY(8));
        break;
    case TT_TRACKERS:
        /* One tile, up to three "Name: location" rows (configured trackers only).
         * Left-aligned multi-line label below the title, mirrors the dim stack. */
        s->val = mklabel(s->card, "", OSR(16), C_TITLE);
        lv_label_set_long_mode(s->val, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s->val, SX(G_COLW - 28));
        lv_obj_set_style_text_align(s->val, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(s->val, LV_ALIGN_TOP_LEFT, SX(14), SY(48));
        break;
    default: {
        /* big Open Sans Light is glyph-subset (digits/°/bar/Watt); values with
         * arbitrary letters (ppm/ppb/m3) need the full-Latin Regular face. */
        const lv_font_t * vf = (t == TT_CO2 || t == TT_TVOC || t == TT_GAS) ? OSR(28) : OSL(40);
        s->val = mklabel(s->card, "--", vf, C_TITLE);
        lv_obj_align(s->val, LV_ALIGN_CENTER, 0, SY(6));
        break; }
    }
}

static void refresh_slot(slot_t * s) {
    char b[96];
    switch (s->type) {
    case TT_EMPTY: return;
    case TT_CLOCK: {
        time_t n = time(NULL); struct tm tm; localtime_r(&n, &tm);
        snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min); lv_label_set_text(s->val, b);
        snprintf(b, sizeof b, "%d %s %d", tm.tm_mday, MND[tm.tm_mon], tm.tm_year + 1900); lv_label_set_text(s->sub, b);
        break; }
    case TT_HUMID:
        if (toon_state.humidity > 0) { snprintf(b, sizeof b, "%.0f%%", toon_state.humidity); lv_label_set_text(s->val, b); }
        else lv_label_set_text(s->val, "--%");
        break;
    case TT_POWER: {
        float w = cur_power_w(); if (w < 0) w = 0;
        snprintf(b, sizeof b, "%.0f Watt", cur_power_w()); lv_label_set_text(s->val, b);
        int lit = (int)(w / 2500.0f * N_ESEG + 0.5f); if (lit > N_ESEG) lit = N_ESEG;
        for (int i = 0; i < N_ESEG; i++) {
            if (!s->gauge[i]) continue;
            int fb = N_ESEG - 1 - i;
            uint32_t c = (fb < lit) ? (fb < 7 ? 0x689F38 : 0xCCCC33) : 0xE0E0E0;
            lv_obj_set_style_bg_color(s->gauge[i], lv_color_hex(c), 0);
        }
        break; }
    case TT_WATERP: {
        /* freetoon swallows zero updates (boxtalk.c), so wp<=0 means "no reading"
         * — show -- without the false "te laag" alarm, not 0,0. */
        float wp = toon_state.water_pressure;
        int have = (wp > 0.05f);
        if (have) { snprintf(b, sizeof b, "%.1f bar", wp); comma(b); } else strcpy(b, "-- bar");
        lv_label_set_text(s->val, b);
        int low = (have && wp < 1.0f);
        lv_obj_set_style_text_color(s->val, lv_color_hex(low ? C_ALERT : C_TITLE), 0);
        if (s->banner) (low ? lv_obj_clear_flag : lv_obj_add_flag)(s->banner, LV_OBJ_FLAG_HIDDEN);
        break; }
    case TT_INDOOR:  snprintf(b, sizeof b, "%.1f°", toon_state.indoor_temp);     comma(b); lv_label_set_text(s->val, b); break;
    case TT_CO2:     snprintf(b, sizeof b, "%d ppm", toon_state.eco2);                     lv_label_set_text(s->val, b); break;
    case TT_TVOC:    snprintf(b, sizeof b, "%d ppb", toon_state.tvoc);                     lv_label_set_text(s->val, b); break;
    case TT_BOILIN:  snprintf(b, sizeof b, "%.1f°", toon_state.boiler_in_temp);  comma(b); lv_label_set_text(s->val, b); break;
    case TT_BOILOUT: snprintf(b, sizeof b, "%.1f°", toon_state.boiler_out_temp); comma(b); lv_label_set_text(s->val, b); break;
    case TT_GAS:     snprintf(b, sizeof b, "%.2f m3", hw_state.gas_hour_m3);     comma(b); lv_label_set_text(s->val, b); break;
    case TT_VENT:
        if (vent_state.connected) { snprintf(b, sizeof b, "%d%%", vent_state.speed_pct); lv_label_set_text(s->val, b); }
        else lv_label_set_text(s->val, "--");
        break;
    case TT_AGENDA:
        if (calendar_state.count > 0) {
            const calendar_event_t * e = &calendar_state.ev[0];
            snprintf(b, sizeof b, "%s%s%s\n%s", e->date + 5,
                     e->time[0] ? "  " : "", e->time[0] ? e->time : "", e->summary);
            lv_label_set_text(s->val, b);
        } else lv_label_set_text(s->val, tr("Geen afspraken", "No events"));
        break;
    case TT_TRACKERS: {
        /* Build up to three rows from the configured trackers (A/B/C). Skip any
         * slot with no entity set; show "--" until HA delivers a location. */
        char tb[256]; size_t n = 0; int any = 0;
        const struct { const char * name; const char * entity; const char * loc; } trk[3] = {
            { settings.life360_a_name, settings.life360_a_entity, ha_state.loc_a },
            { settings.life360_b_name, settings.life360_b_entity, ha_state.loc_b },
            { settings.life360_c_name, settings.life360_c_entity, ha_state.loc_c },
        };
        tb[0] = 0;
        for (int i = 0; i < 3; i++) {
            if (!trk[i].entity[0]) continue;
            const char * nm = trk[i].name[0] ? trk[i].name : trk[i].entity;
            const char * lc = trk[i].loc[0]  ? trk[i].loc  : "--";
            n += snprintf(tb + n, sizeof tb - n, "%s%s: %s", any ? "\n" : "", nm, lc);
            any = 1;
            if (n >= sizeof tb) break;
        }
        if (!any) snprintf(tb, sizeof tb, "%s", tr("Geen trackers", "No trackers"));
        lv_label_set_text(s->val, tb);
        break; }
    case TT_WEATHER: {
        snprintf(b, sizeof b, "%.1f°", weather_state.current_temp); comma(b); lv_label_set_text(s->val, b);
        lv_label_set_text(s->sub, weather_state.current_desc[0] ? weather_state.current_desc : tr("Buiten", "Outside"));
        if (s->icon && weather_state.current_icon[0]) {
            lv_img_set_src(s->icon, weather_icon_for_lg(weather_state.current_icon));
            lv_obj_set_style_img_recolor(s->icon, lv_color_hex(weather_icon_color_for(weather_state.current_icon)), 0);
            lv_obj_set_style_img_recolor_opa(s->icon, LV_OPA_COVER, 0);
        }
        break; }
    }
}

/* ---- tile tap (detail) + long-press (picker) ----------------------------- */
static void on_tile_press(lv_event_t * e) {
    (void)e; lv_indev_t * in = lv_indev_get_act();
    if (in) lv_indev_get_point(in, &g_press_pt);
}
static void open_picker(int slot);
static void on_slot_click(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int link = TM[slots[idx].type].link;
    if (link == LINK_NONE && slots[idx].type != TT_EMPTY) return;
    lv_indev_t * in = lv_indev_get_act();      /* a flick that snapped pages is not a tap */
    if (in) { lv_point_t p; lv_indev_get_point(in, &p);
        long dx = p.x - g_press_pt.x, dy = p.y - g_press_pt.y, lim = SX(26);
        if (dx * dx + dy * dy > lim * lim) return; }
    if (slots[idx].type == TT_EMPTY) { open_picker(idx); return; }  /* empty → add a tile */
    switch (link) {
        case LINK_STATS:    ui_push(screen_stats_create());           break;
        case LINK_FORECAST: ui_push(screen_forecast_create());        break;
        case LINK_HEATER:   ui_push(screen_heater_advanced_create()); break;
        case LINK_VENT:     ui_push(screen_vent_remote_create());     break;
        case LINK_TRACKERS: life360_map_open();                       break;
    }
}

/* ---- settings persistence ------------------------------------------------ */
static int key_to_type(const char * k) {
    for (int t = 0; t < TT_COUNT; t++) if (!strcmp(k, TM[t].key)) return t;
    return TT_EMPTY;
}
static void load_layout(int out[N_SLOTS]) {
    static const int def[N_SLOTS] = {
        TT_CLOCK, TT_HUMID, TT_POWER, TT_WATERP,
        TT_INDOOR, TT_BOILIN, TT_BOILOUT, TT_WATERP,
        TT_WEATHER, TT_CO2, TT_TVOC, TT_VENT,
        TT_GAS, TT_AGENDA, TT_EMPTY, TT_EMPTY };
    for (int i = 0; i < N_SLOTS; i++) out[i] = def[i];
    if (settings.stock_tiles[0]) {
        char buf[256]; snprintf(buf, sizeof buf, "%s", settings.stock_tiles);
        char * sv = NULL, * tok = strtok_r(buf, ",", &sv); int i = 0;
        while (tok && i < N_SLOTS) { out[i++] = key_to_type(tok); tok = strtok_r(NULL, ",", &sv); }
    }
}
static void save_layout(void) {
    char buf[256] = "";
    for (int i = 0; i < N_SLOTS; i++) {
        strncat(buf, TM[slots[i].type].key, sizeof buf - strlen(buf) - 1);
        if (i < N_SLOTS - 1) strncat(buf, ",", sizeof buf - strlen(buf) - 1);
    }
    snprintf(settings.stock_tiles, sizeof settings.stock_tiles, "%s", buf);
    settings_save();
}

/* ---- tile picker --------------------------------------------------------- */
/* Keep the UI marked active while the picker is open so auto-dim doesn't push
 * the dim screen in behind it while the user is deciding. */
static lv_timer_t * g_picker_keepalive = NULL;
static void picker_keepalive(lv_timer_t * t) { (void)t; ui_mark_activity(); }
static void picker_close(void) {
    if (g_picker_keepalive) { lv_timer_del(g_picker_keepalive); g_picker_keepalive = NULL; }
    if (g_picker) { lv_obj_del(g_picker); g_picker = NULL; }
    g_edit_slot = -1;
}
static void on_picker_bg(lv_event_t * e) { (void)e; picker_close(); }
static void on_picker_pick(lv_event_t * e) {
    int type = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_edit_slot >= 0 && g_edit_slot < N_SLOTS) {
        slots[g_edit_slot].type = type;
        render_slot(&slots[g_edit_slot]);
        refresh_slot(&slots[g_edit_slot]);
        save_layout();
    }
    picker_close();
}
static void open_picker(int slot) {
    if (g_picker) picker_close();
    g_edit_slot = slot;
    ui_mark_activity();   /* and keep it awake while open */
    g_picker_keepalive = lv_timer_create(picker_keepalive, 2000, NULL);
    g_picker = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_picker, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_picker, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_picker, LV_OPA_50, 0);
    lv_obj_set_style_border_width(g_picker, 0, 0);
    lv_obj_set_style_pad_all(g_picker, 0, 0);
    lv_obj_add_flag(g_picker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_picker, on_picker_bg, LV_EVENT_CLICKED, NULL);

    lv_obj_t * p = lv_obj_create(g_picker);
    lv_obj_set_size(p, SX(620), SY(470));
    lv_obj_center(p);
    lv_obj_set_style_bg_color(p, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(p, SX(8), 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, SX(16), 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);   /* swallow bg clicks */
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * ttl = mklabel(p, tr("Kies tegel", "Choose tile"), OSR(20), C_TITLE);
    lv_obj_set_width(ttl, LV_PCT(100));
    lv_obj_set_style_text_align(ttl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(ttl, SY(8), 0);

    for (int t = 0; t < TT_COUNT; t++) {
        lv_obj_t * b = lv_btn_create(p);
        lv_obj_set_size(b, SX(180), SY(54));
        lv_obj_set_style_bg_color(b, lv_color_hex(C_HEADER), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_radius(b, SX(4), 0);
        lv_obj_add_event_cb(b, on_picker_pick, LV_EVENT_CLICKED, (void *)(intptr_t)t);
        lv_obj_t * l = mklabel(b, tr(TM[t].nl, TM[t].en), OSR(16), C_TITLE);
        lv_obj_center(l);
    }
}
static void on_slot_longpress(lv_event_t * e) { open_picker((int)(intptr_t)lv_event_get_user_data(e)); }

/* ---- carousel ------------------------------------------------------------ */
static int active_page(void) {
    lv_obj_t * a = lv_tileview_get_tile_act(tv);
    for (int i = 0; i < N_PAGES; i++) if (pages[i] == a) return i;
    return 0;
}
static void update_dots(void) {
    int idx = active_page();
    for (int i = 0; i < N_PAGES; i++)
        lv_obj_set_style_bg_color(dots[i], lv_color_hex(i == idx ? C_ACCENT : C_PRESSED), 0);
}
static void on_tv_change(lv_event_t * e) { (void)e; update_dots(); }
static void on_arrow(lv_event_t * e) {
    int idx = active_page() + (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0) idx = 0; if (idx >= N_PAGES) idx = N_PAGES - 1;
    lv_obj_set_tile(tv, pages[idx], LV_ANIM_ON);
    update_dots();
}

/* ---- periodic refresh ---------------------------------------------------- */
static void refresh_cb(lv_timer_t * t) {
    (void)t;
    for (int i = 0; i < N_SLOTS; i++) refresh_slot(&slots[i]);

    /* Big readout = indoor temp (default) or setpoint, per settings.stock_big_indoor;
     * the other shows small underneath. */
    char big[32], lo[32];
    float big_v = settings.stock_big_indoor ? toon_state.indoor_temp : toon_state.setpoint;
    float lo_v  = settings.stock_big_indoor ? toon_state.setpoint     : toon_state.indoor_temp;
    snprintf(big, sizeof big, "%.1f°", big_v); comma(big); lv_label_set_text(lbl_setpoint, big);
    snprintf(lo,  sizeof lo,  "%.1f°", lo_v);  comma(lo);  lv_label_set_text(lbl_setpoint_lo, lo);
    int manual = (toon_state.active_state < 0);
    lv_label_set_text(lbl_prog, manual ? tr("Programma uit", "Program off") : tr("Programma aan", "Program on"));
    for (int i = 0; i < 4; i++) {
        int active = (!manual && toon_state.active_state == scene_state[i]);
        lv_obj_set_style_text_color(scene_lbl[i], lv_color_hex(active ? C_ACCENT : C_TITLE), 0);
        lv_obj_set_style_bg_color(scene_btn[i], lv_color_hex(active ? C_HEADER : C_CARD), 0);
    }
}

/* ---- build --------------------------------------------------------------- */
lv_obj_t * screen_home_stock_create(void) {
    if (scr_root) return scr_root;
    scr_root = lv_obj_create(NULL);
    lv_obj_set_size(scr_root, lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL));
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(C_CANVAS), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr_root, 0, 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* top bar: apps-grid → Settings + Freetoon wordmark */
    lv_obj_t * grid = lv_obj_create(scr_root);
    lv_obj_set_pos(grid, SX(34), SY(28));
    lv_obj_set_size(grid, SX(34), SY(34));
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(grid, SX(18));
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
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t * brand = mklabel(scr_root, "Freetoon", OSS(28), C_TITLE);
    lv_obj_set_style_text_letter_space(brand, SX(2), 0);
    lv_obj_set_pos(brand, SX(120), SY(28));

    /* carousel of customizable slot pages */
    tv = lv_tileview_create(scr_root);
    lv_obj_set_pos(tv, SX(G_X0), SY(G_Y0));
    lv_obj_set_size(tv, SX(2 * G_COLW + G_GAPX), SY(2 * G_ROWH + G_GAPY + 4));
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tv, 0, 0);
    lv_obj_set_style_pad_all(tv, 0, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(tv, on_tv_change, LV_EVENT_VALUE_CHANGED, NULL);

    int types[N_SLOTS]; load_layout(types);
    for (int p = 0; p < N_PAGES; p++) {
        lv_obj_t * page = lv_tileview_add_tile(tv, p, 0, LV_DIR_HOR);
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
        pages[p] = page;
        for (int q = 0; q < 4; q++) {
            int i = p * 4 + q;
            slot_t * s = &slots[i];
            s->idx = i; s->type = types[i];
            s->card = lcard(page, q % 2, q / 2);
            lv_obj_add_flag(s->card, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s->card, on_tile_press, LV_EVENT_PRESSED, NULL);
            /* SHORT_CLICKED (not CLICKED) so a long-press doesn't also navigate */
            lv_obj_add_event_cb(s->card, on_slot_click, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
            lv_obj_add_event_cb(s->card, on_slot_longpress, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
            render_slot(s);
        }
    }

    /* thermostat side panel (fixed) */
    lv_obj_t * panel = card(scr_root, SP_X0, SP_Y0, SP_W, SP_H);
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

    lbl_setpoint = mklabel(panel, "--,-°", OSL(50), C_TITLE);
    lv_obj_set_pos(lbl_setpoint, SX(20), SY(78));
    lv_obj_t * tri = mklabel(panel, LV_SYMBOL_DOWN, SF(13), C_SECOND);
    lv_obj_set_pos(tri, SX(22), SY(140));
    lbl_setpoint_lo = mklabel(panel, "--,-°", OSR(15), C_SECOND);
    lv_obj_set_pos(lbl_setpoint_lo, SX(42), SY(138));
    lv_obj_t * sprout = make_sprout_icon(panel);
    lv_obj_set_pos(sprout, SX(96), SY(134));

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

    lbl_prog = mklabel(panel, tr("Programma uit", "Program off"), OSR(15), C_TITLE);
    lv_obj_set_pos(lbl_prog, SX(20), SY(214));
    lv_obj_t * sw = lv_switch_create(panel);
    lv_obj_set_size(sw, SX(56), SY(28));
    lv_obj_set_pos(sw, SX(SP_W - 76), SY(210));
    lv_obj_set_style_bg_color(sw, lv_color_hex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, on_prog_toggle, LV_EVENT_VALUE_CHANGED, NULL);

    const char * names[4] = { tr("Weg","Away"), tr("Thuis","Home"), tr("Slapen","Sleep"), tr("Comfort","Comfort") };
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

    /* arrows + dots */
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

    int cx = G_X0 + (2 * G_COLW + G_GAPX) / 2;
    int dot_gap = 24, dx0 = cx - ((N_PAGES - 1) * dot_gap) / 2 - 5;
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

    /* Lights / devices handle — same collapsible left-edge tab as the LVGL
     * home screen. Idles as a slim pill peeking from the left edge; press
     * expands to "Apparaten" / "Devices"; release + click opens the backend. */
    s_lights_handle = lv_btn_create(scr_root);
    lv_obj_set_size(s_lights_handle, SX(56), SY(80));
    lv_obj_align(s_lights_handle, LV_ALIGN_LEFT_MID, -SX(4), 0);
    lv_obj_set_style_bg_color(s_lights_handle, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(s_lights_handle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_lights_handle, 0, 0);
    lv_obj_set_style_shadow_width(s_lights_handle, SX(8), 0);
    lv_obj_set_style_shadow_opa(s_lights_handle, LV_OPA_30, 0);
    lv_obj_add_event_cb(s_lights_handle, on_lights_handle, LV_EVENT_ALL, NULL);
    s_lights_handle_lbl = lv_label_create(s_lights_handle);
    lv_label_set_text(s_lights_handle_lbl, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(s_lights_handle_lbl, SF(28), 0);
    lv_obj_set_style_text_color(s_lights_handle_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_center(s_lights_handle_lbl);

    refresh_cb(NULL);
    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}
