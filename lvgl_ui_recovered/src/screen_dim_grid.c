/*
 * Dim/ambient screen — pure black background, large white clock plus
 * indoor temp and setpoint. Tap anywhere to wake.
 * No colour, no icons; this is the screen we want visible while idle.
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "boxtalk.h"
#include "settings.h"
#include "homewizard.h"
#include "energy.h"
#include "homeassistant.h"
#include "packages.h"
#include "weather.h"
#include "wastecollection.h"
#include "ventilation.h"
#include "calendar.h"
#include "meteradapter.h"   /* meter_state — for the restored dim energy bar */
#include "layout.h"         /* customizable dim block layout (g_dim_blocks) */
#include "icons.h"
#include "buienradar_rain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(lv_font_montserrat_96_custom);

static lv_obj_t * scr_root = NULL;
static lv_obj_t * lbl_clock_h = NULL;
static lv_obj_t * lbl_clock_m = NULL;
static lv_obj_t * lbl_clock_colon = NULL;   /* pulsing ":" overlay */
static lv_obj_t * lbl_date;
static lv_obj_t * dim_moon_img;   /* moon-phase widget — always shown */
static lv_obj_t * lbl_temp;
static lv_obj_t * lbl_setpoint;
static lv_obj_t * lbl_outside_temp = NULL;
static lv_obj_t * lbl_program;
static lv_obj_t * lbl_metrics;     /* TVOC / eCO2 / CH-water-pressure row */
static lv_obj_t * lbl_burner;      /* "90 C" when CH, hidden otherwise */
static lv_obj_t * dim_img_flame;   /* CH flame — paired with lbl_burner */
static lv_obj_t * dim_img_faucet;  /* DHW faucet — visible only on dhw_on */
static lv_obj_t * dim_img_drop;    /* paired water-drop next to the faucet */
static lv_obj_t * wx_icon = NULL;
static lv_obj_t * lbl_outside = NULL;
static lv_obj_t * waste_icon = NULL;
static lv_obj_t * waste_icon_2 = NULL;   /* 2nd bin shown when same day has 2 types */
static lv_obj_t * lbl_waste = NULL;
static lv_obj_t * waste_box_ptr = NULL;
static lv_obj_t * dim_fc_icon[WEATHER_FORECAST_DAYS];
static lv_obj_t * dim_fc_day[WEATHER_FORECAST_DAYS];
static lv_obj_t * dim_fc_temp[WEATHER_FORECAST_DAYS];
/* City header above the forecast strip — mirrors the home tile. */
static lv_obj_t * dim_lbl_city = NULL;
/* Life360 — stacked TOP_RIGHT under lbl_outside, opposite the waste block. */
static lv_obj_t * dim_lbl_life360_a = NULL;
static lv_obj_t * dim_lbl_life360_b   = NULL;
#define MAX_DIM_AGENDA_ROWS 12
static lv_obj_t * dim_lbl_agenda_time[MAX_DIM_AGENDA_ROWS] = {0};
static lv_obj_t * dim_lbl_agenda_title[MAX_DIM_AGENDA_ROWS] = {0};
static int        dim_agenda_row_count = 0;

/* Buienradar rain forecast block (DB_RAINFALL) */
static lv_obj_t * dim_rain_box = NULL;
static lv_obj_t * dim_rain_hdr = NULL;
static lv_obj_t * dim_rain_lbl_title = NULL;
static lv_obj_t * dim_rain_lbl_status = NULL;
static lv_obj_t * dim_rain_lbl_empty = NULL;
static lv_obj_t * dim_thresh_lbl[3] = {0};
static lv_obj_t * dim_rain_chart = NULL;
static lv_chart_series_t * dim_rain_ser = NULL;
static lv_obj_t * dim_rain_lbl_times[4] = {0};
static lv_obj_t * dim_vent_fan  = NULL;   /* spinning fan icon */
static lv_obj_t * dim_vent_lbl  = NULL;   /* "57 %" — actual ExhFanSpeed */
static lv_obj_t * dim_img_water = NULL;   /* drop icon, visible while pouring */
static lv_obj_t * dim_lbl_water = NULL;   /* "1.4 L/m" / "+1.4 L" */
static int        dim_vent_period_ms = 0; /* current spin animation period */
static lv_timer_t * refresh_timer = NULL;

/* ---- customizable dim layout ------------------------------------------------
 * Each peripheral block (weather/waste/family/vent/thermo/forecast) renders its
 * widgets INSIDE a transparent container placed at its grid cell, and its content
 * is laid out relative to that container. So what the "Dim indeling" editor shows
 * (a labelled rectangle per cell) is exactly where the block's content lands —
 * true WYSIWYG. The clock + date stay centered and are NOT part of this. */
static lv_obj_t * dim_box(int id, int * ow, int * oh) {
    int x, y, w, h;
    layout_cell_px(g_dim_blocks[id].col, g_dim_blocks[id].row,
                   g_dim_blocks[id].w, g_dim_blocks[id].h, &x, &y, &w, &h);
    lv_obj_t * c = lv_obj_create(scr_root);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    if (!g_dim_blocks[id].visible) lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    if (ow) *ow = w;
    if (oh) *oh = h;
    return c;
}

/* ---- gridded ENERGY block: power (W) + gas (m³/h) as value + thin bar ----
 * Replaces the old screen-edge VERTICAL bars, which were not part of the grid
 * and so overlapped whatever blocks landed at the edges (vent on the left, the
 * forecast strip at the bottom). Now it's a normal dim block (DB_ENERGY) with
 * two labelled horizontal mini-bars; gated by the existing show_dim_bars toggle.
 * When settings.dim_bars_vertical is on, renders classic Toon segmented vertical bars. */
static lv_obj_t * en_pwr_track, * en_pwr_fill, * en_pwr_lbl;
static lv_obj_t * en_gas_track, * en_gas_fill, * en_gas_lbl;
static lv_obj_t * en_sol_track, * en_sol_fill, * en_sol_lbl;
static int   en_pwr_w = 0, en_gas_w = 0, en_sol_w = 0;   /* mini-bar track widths (px, set at create) */
#define DIM_GRID_NSEG 12
static lv_obj_t * en_pwr_segs[DIM_GRID_NSEG];
static lv_obj_t * en_pwr_vlbl, * en_pwr_vunit;
static lv_obj_t * en_gas_segs[DIM_GRID_NSEG];
static lv_obj_t * en_gas_vlbl, * en_gas_vunit;
static lv_obj_t * en_sol_segs[DIM_GRID_NSEG];
static lv_obj_t * en_sol_vlbl, * en_sol_vunit;
#define DIM_E_FULL_W    5000.0f     /* power at a full bar (fixed scale) */
#define DIM_G_FULL_M3H  2.0f        /* gas (m³/h) at a full bar (fixed scale) */

static void dim_vent_fan_anim_cb(void * obj, int32_t v) {
    lv_img_set_angle((lv_obj_t *)obj, v);
}
static void dim_vent_apply_anim(int rpm) {
    if (!dim_vent_fan) return;
    /* Park below 50 rpm. See screen_home.c vent_apply_fan_anim — driving
       off rpm because Itho's ExhFanSpeed is unreliable and its Low/High
       labels are backwards on this unit. */
    if (rpm < 50) {
        if (dim_vent_period_ms == 0) return;
        dim_vent_period_ms = 0;
        lv_anim_del(dim_vent_fan, NULL);
        return;
    }
    /* Same steeper curve as the home tile, mapped onto the CVE's real rpm
       band (~1150 low → ~2700 high) so High visibly spins up. ~1.3 ms/rpm. */
    int period = 2600 - (rpm - 1000) * 13 / 10;
    if (period < 280)  period = 280;
    if (period > 2600) period = 2600;
    /* Hysteresis: every poll the rpm jitters ±1 which would re-spin the
       anim from 0° if we treated each tiny period delta as a change. Only
       restart when the period actually moves > 100 ms. */
    if (abs(period - dim_vent_period_ms) < 100) return;
    dim_vent_period_ms = period;
    lv_anim_del(dim_vent_fan, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, dim_vent_fan);
    lv_anim_set_exec_cb(&a, dim_vent_fan_anim_cb);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_time(&a, period);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

/* One labelled horizontal mini-bar inside the energy block: a dark track with a
 * coloured fill that grows left→right, and a value label beneath it. */
static void en_make_bar(lv_obj_t * parent, int w, int y, uint32_t color,
                        lv_obj_t ** track, lv_obj_t ** fill, lv_obj_t ** lbl) {
    *track = lv_obj_create(parent);
    lv_obj_remove_style_all(*track);
    lv_obj_set_size(*track, w, 10);
    lv_obj_set_style_bg_color(*track, lv_color_hex(0x223040), 0);
    lv_obj_set_style_bg_opa(*track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(*track, 5, 0);
    lv_obj_clear_flag(*track, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(*track, LV_ALIGN_TOP_LEFT, 4, y);

    *fill = lv_obj_create(*track);
    lv_obj_remove_style_all(*fill);
    lv_obj_set_size(*fill, 0, 10);
    lv_obj_set_style_bg_color(*fill, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(*fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(*fill, 5, 0);
    lv_obj_clear_flag(*fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(*fill, LV_ALIGN_LEFT_MID, 0, 0);

    *lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(*lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(*lbl, &lv_font_montserrat_18, 0);
    lv_label_set_text(*lbl, "");
    lv_obj_align(*lbl, LV_ALIGN_TOP_LEFT, 4, y + 13);
}
static void en_set_bar(lv_obj_t * fill, lv_obj_t * lbl,
                       int tw, float ratio, const char * txt) {
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    int w = (int)(ratio * tw + 0.5f);
    if (ratio > 0 && w < 3) w = 3;
    lv_obj_set_width(fill, w);
    lv_label_set_text(lbl, txt);
}

/* One vertical segmented bar (classic Toon style) inside the energy/gas block:
 * 12 stacked segment rectangles lighting up bottom->top (green -> yellow),
 * with a value label positioned underneath it. */
static void en_make_vbar(lv_obj_t * parent, int bw, int bh,
                         lv_obj_t * segs[DIM_GRID_NSEG], lv_obj_t ** lbl, lv_obj_t ** unit_lbl) {
    if (!parent) return;
    int is_1col = (DISP_HOR < 1000) ? (bw < 70) : (bw < 90);
    int sw = (DISP_HOR < 1000) ? (is_1col ? 22 : 26) : (is_1col ? 28 : 34);

    int val_h, unit_h, unit_gap, lbl_gap, lbl_total_h;
    if (is_1col) {
        val_h = (DISP_VER < 600) ? 16 : 18;
        unit_h = (DISP_VER < 600) ? 12 : 14;
        unit_gap = 1;
        lbl_gap = (DISP_VER < 600) ? 3 : 4;
        lbl_total_h = val_h + unit_gap + unit_h;
    } else {
        val_h = (DISP_VER < 600) ? 18 : 22;
        unit_h = 0;
        unit_gap = 0;
        lbl_gap = (DISP_VER < 600) ? 2 : 4;
        lbl_total_h = val_h;
    }

    int avail_th = bh - lbl_total_h - lbl_gap;

    int sh = 2, gp = 1;
    if (avail_th >= 80) {
        sh = (DISP_VER < 600) ? (is_1col ? 4 : 5) : (is_1col ? 5 : 6);
        gp = 2;
    } else if (avail_th >= 50) {
        sh = (DISP_VER < 600) ? 3 : (is_1col ? 3 : 4);
        gp = (DISP_VER < 600) ? 1 : (is_1col ? 1 : 2);
    } else {
        sh = 2;
        gp = 1;
    }

    int th = DIM_GRID_NSEG * sh + (DIM_GRID_NSEG - 1) * gp;
    int total_h = th + lbl_gap + lbl_total_h;
    int y0 = (bh > total_h) ? (bh - total_h) / 2 : 1;
    int x0 = (bw > sw) ? (bw - sw) / 2 : 4;

    for (int i = 0; i < DIM_GRID_NSEG; i++) {
        segs[i] = lv_obj_create(parent);
        lv_obj_remove_style_all(segs[i]);
        lv_obj_set_size(segs[i], sw, sh);
        lv_obj_set_pos(segs[i], x0, y0 + i * (sh + gp));
        lv_obj_set_style_radius(segs[i], 1, 0);
        lv_obj_set_style_border_width(segs[i], 0, 0);
        lv_obj_set_style_bg_color(segs[i], lv_color_hex(0x2A3540), 0);
        lv_obj_set_style_bg_opa(segs[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(segs[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    *lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(*lbl, lv_color_hex(0xffffff), 0);
    const lv_font_t * vfont = is_1col ? ((DISP_VER < 600) ? &lv_font_montserrat_14 : &lv_font_montserrat_18)
                                      : ((DISP_VER < 600) ? &lv_font_montserrat_18 : &lv_font_montserrat_22);
    lv_obj_set_style_text_font(*lbl, vfont, 0);
    lv_obj_set_style_text_align(*lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(*lbl, bw > 0 ? bw : LV_SIZE_CONTENT);
    lv_label_set_text(*lbl, "");
    lv_obj_set_pos(*lbl, 0, y0 + th + lbl_gap);

    if (unit_lbl) {
        if (is_1col) {
            *unit_lbl = lv_label_create(parent);
            lv_obj_set_style_text_color(*unit_lbl, lv_color_hex(0xbbbbbb), 0);
            const lv_font_t * ufont = (DISP_VER < 600) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
            lv_obj_set_style_text_font(*unit_lbl, ufont, 0);
            lv_obj_set_style_text_align(*unit_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(*unit_lbl, bw > 0 ? bw : LV_SIZE_CONTENT);
            lv_label_set_text(*unit_lbl, "");
            lv_obj_set_pos(*unit_lbl, 0, y0 + th + lbl_gap + val_h + unit_gap);
        } else {
            *unit_lbl = NULL;
        }
    }
}

static void en_set_vbar_color(lv_obj_t * segs[DIM_GRID_NSEG], lv_obj_t * lbl, lv_obj_t * unit_lbl,
                              float ratio, const char * val, const char * unit,
                              uint32_t col_low, uint32_t col_high) {
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    int lit = (int)(ratio * DIM_GRID_NSEG + 0.5f);
    if (lit > DIM_GRID_NSEG) lit = DIM_GRID_NSEG;

    for (int i = 0; i < DIM_GRID_NSEG; i++) {
        if (!segs[i]) continue;
        int fb = DIM_GRID_NSEG - 1 - i;   /* 0 = bottom, 11 = top */
        uint32_t c = (fb < lit) ? (fb < 7 ? col_low : col_high) : 0x2A3540;
        lv_obj_set_style_bg_color(segs[i], lv_color_hex(c), 0);
    }
    if (unit_lbl) {
        if (lbl) lv_label_set_text(lbl, val);
        lv_label_set_text(unit_lbl, unit);
    } else if (lbl) {
        char comb[32];
        snprintf(comb, sizeof comb, "%s %s", val, unit);
        lv_label_set_text(lbl, comb);
    }
}

static void en_set_vbar(lv_obj_t * segs[DIM_GRID_NSEG], lv_obj_t * lbl, lv_obj_t * unit_lbl,
                        float ratio, const char * val, const char * unit) {
    en_set_vbar_color(segs, lbl, unit_lbl, ratio, val, unit, 0x689F38, 0xCCCC33);
}

static void on_wake_tap(lv_event_t * e) {
    (void)e;
    ui_wake_now();
}

static int s_date_box_w = 0, s_date_box_h = 0;

static const lv_font_t * pick_date_font(const char * txt, lv_coord_t max_w, lv_coord_t max_h) {
    static const lv_font_t * const fonts[] = {
        &lv_font_montserrat_48,
        &lv_font_montserrat_36,
        &lv_font_montserrat_28,
        &lv_font_montserrat_24,
        &lv_font_montserrat_22,
        &lv_font_montserrat_20,
        &lv_font_montserrat_18,
        &lv_font_montserrat_14,
    };
    if (!txt || !txt[0]) return &lv_font_montserrat_22;
    for (size_t i = 0; i < sizeof(fonts)/sizeof(fonts[0]); i++) {
        const lv_font_t * f = fonts[i];
        if (!f) continue;
        lv_point_t sz;
        lv_txt_get_size(&sz, txt, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= max_w && (max_h <= 0 || sz.y <= max_h)) {
            return f;
        }
    }
    return &lv_font_montserrat_14;
}

static void format_agenda_date_time(const calendar_event_t * ev, char * out, size_t outsz) {
    if (!ev || !ev->date[0]) {
        if (outsz) out[0] = 0;
        return;
    }
    int yr = 0, mo = 0, da = 0;
    if (sscanf(ev->date, "%4d-%2d-%2d", &yr, &mo, &da) == 3) {
        if (ev->time[0])
            snprintf(out, outsz, "%02d/%02d %s", da, mo, ev->time);
        else
            snprintf(out, outsz, "%02d/%02d", da, mo);
    } else {
        snprintf(out, outsz, "%s", ev->date);
    }
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (lv_scr_act() != scr_root) return;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk_h[8], clk_m[8];
    strftime(clk_h, sizeof(clk_h), "%H", &tm);
    strftime(clk_m, sizeof(clk_m), "%M", &tm);
    lv_label_set_text(lbl_clock_h, clk_h);
    lv_label_set_text(lbl_clock_m, clk_m);
    lv_obj_update_layout(lbl_clock_h);
    lv_obj_update_layout(lbl_clock_m);
    lv_obj_align_to(lbl_clock_h, lbl_clock_colon, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    lv_obj_align_to(lbl_clock_m, lbl_clock_colon, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    char dt[64];
    snprintf(dt, sizeof(dt), "%s\n%d %s",
             i18n_day_long(tm.tm_wday),
             tm.tm_mday,
             i18n_month_long(tm.tm_mon));
    lv_coord_t max_w = s_date_box_w > 0 ? s_date_box_w - 12 : (DISP_HOR * 2 / 5);
    lv_coord_t max_h = s_date_box_h > 0 ? s_date_box_h - 8 : 60;
    const lv_font_t * df = pick_date_font(dt, max_w, max_h);
    lv_obj_set_style_text_font(lbl_date, df, 0);
    lv_label_set_text(lbl_date, dt);

    /* Moon (top-right, beside the current-weather icon): white at night,
       hidden during the day. Day/night from a real sunrise/sunset calc. */
    if (dim_moon_img) {
        if (is_daytime_now()) {
            lv_obj_add_flag(dim_moon_img, LV_OBJ_FLAG_HIDDEN);
        } else {
            const lv_img_dsc_t * ph = moon_phase_icon(80);
            if (lv_img_get_src(dim_moon_img) != ph)
                lv_img_set_src(dim_moon_img, ph);
            lv_obj_clear_flag(dim_moon_img, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Always paint values: if data not yet present, fall back to a
       "wait..." marker instead of leaving the stale "-- C" default. */
    if (toon_state.indoor_temp > 0)
        lv_label_set_text_fmt(lbl_temp, "%.1f°C", display_indoor_temp(toon_state.indoor_temp));
    else
        lv_label_set_text(lbl_temp, "...");
    lv_obj_update_layout(lbl_temp);

    /* Setpoint & program state under room temp */
    if (lbl_setpoint) {
        const char * prog = program_label();
        if (toon_state.setpoint > 0) {
            if (toon_state.burner_on)
                lv_label_set_text_fmt(lbl_setpoint, "%s - naar %.1f°C", prog, toon_state.setpoint);
            else
                lv_label_set_text_fmt(lbl_setpoint, "%s - %.1f°C", prog, toon_state.setpoint);
        } else {
            lv_label_set_text(lbl_setpoint, prog ? prog : "");
        }
        lv_obj_update_layout(lbl_setpoint);
        if (dim_img_flame) lv_obj_align_to(dim_img_flame, lbl_setpoint, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
        if (dim_img_faucet) lv_obj_align_to(dim_img_faucet, lbl_setpoint, LV_ALIGN_OUT_BOTTOM_MID, -10, 4);
        if (dim_img_drop) lv_obj_align_to(dim_img_drop, dim_img_faucet, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    } else {
        if (dim_img_flame) lv_obj_align_to(dim_img_flame, lbl_temp, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
        if (dim_img_faucet) lv_obj_align_to(dim_img_faucet, lbl_temp, LV_ALIGN_OUT_BOTTOM_MID, -10, 4);
        if (dim_img_drop) lv_obj_align_to(dim_img_drop, dim_img_faucet, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    }

    if (lbl_metrics) {
        if (!settings.show_dim_metrics) {
            lv_obj_add_flag(lbl_metrics, LV_OBJ_FLAG_HIDDEN);
        } else {
            /* TVOC / eCO2 ppm / CH water pressure / air-quality badge on one
               greyed row. Missing inputs collapse to "--" so the strip layout
               stays stable. AQ label is appended only when we actually have
               air-quality data to classify. */
            char buf[200];
            char bar[24]  = "CV --";
            if (toon_state.water_pressure > 0.1f)
                snprintf(bar,  sizeof bar,  "CV %.1f bar", toon_state.water_pressure);
#ifndef TOON1
            /* TVOC / eCO2 / air-quality come from the eCO2/TVOC air sensor
               that only Toon 2 has -- on Toon 1 the row is just CH pressure. */
            char tvoc[24] = "TVOC --";
            char co2[24]  = "CO2 --";
            if (toon_state.tvoc)
                snprintf(tvoc, sizeof tvoc, "TVOC %d ppb", toon_state.tvoc);
            if (toon_state.eco2)
                snprintf(co2,  sizeof co2,  "CO2 %d ppm", toon_state.eco2);
            const char * aql = air_quality_label(toon_state.eco2, toon_state.tvoc);
            if (*aql)
                snprintf(buf, sizeof buf, tr("%s  %s  %s  Lucht: %s",
                                              "%s  %s  %s  Air: %s"),
                         tvoc, co2, bar, aql);
            else
                snprintf(buf, sizeof buf, "%s  %s  %s", tvoc, co2, bar);
#else
            snprintf(buf, sizeof buf, "%s", bar);
#endif
            lv_label_set_text(lbl_metrics, buf);
            lv_obj_clear_flag(lbl_metrics, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Burner state — symbol-first now. CH-heating shows just the target
       degrees ("-> 90 C", red); DHW shows a faucet + water-drop icon pair
       (no text — the icons say it). Idle hides everything so the dim
       screen stays clean. */
    /* Live water-flow indicator on dim, right side below the radiator slot
     * so it can co-exist with the CH flame. Same visibility rules as the
     * home-tile version: drop+L/m while pouring, "+X.X L" briefly after. */
    if (dim_img_water && dim_lbl_water) {
        if (hw_state.connected_water && hw_state.water_lpm > 0.05f) {
            lv_label_set_text_fmt(dim_lbl_water, "%.1f L/m",
                                  hw_state.water_lpm);
            lv_obj_clear_flag(dim_img_water, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(dim_lbl_water, LV_OBJ_FLAG_HIDDEN);
        } else if (hw_state.connected_water && hw_state.water_session_l > 0) {
            lv_label_set_text_fmt(dim_lbl_water, "+%.1f L",
                                  hw_state.water_session_l);
            lv_obj_clear_flag(dim_img_water, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(dim_lbl_water, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(dim_img_water, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(dim_lbl_water, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Show the radiator-with-flame glyph next to the indoor temp when the
     * boiler is firing CH — original-Toon style. No "90 C" target text:
     * the glyph itself is the signal. */
    if (dim_img_flame) {
        if (toon_state.burner_on)
            lv_obj_clear_flag(dim_img_flame, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(dim_img_flame, LV_OBJ_FLAG_HIDDEN);
    }
    if (lbl_burner) lv_obj_add_flag(lbl_burner, LV_OBJ_FLAG_HIDDEN);
    if (dim_img_faucet && dim_img_drop) {
        if (toon_state.dhw_on) {
            lv_obj_clear_flag(dim_img_faucet, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(dim_img_drop,   LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(dim_img_faucet, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(dim_img_drop,   LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Vent — fan icon spin tracks fan_rpm; label shows the ventilation MODE
       (Low/High/Auto/Medium/Timer) + pct + remaining ("Auto 3 %" or
       "Timer 25m 100 %"). The mode word comes from fan_info if the firmware
       still publishes FanInfo, else from last_cmd (the itho/lastcmd MQTT
       topic) — current firmware dropped FanInfo so last_cmd is the source.
       Both are already swap-corrected to the user-intent label. Online/offline
       (itho/lwt) is shown on the HOME tile, not here. */
    if (dim_vent_fan && dim_vent_lbl) {
        if (vent_state.connected && vent_state.itho_online == 0) {
            /* Itho reported itself offline via its MQTT LWT — show that
               (overrides the mode) and park the spinner. Normal case below
               shows what it's doing (mode + %). */
            lv_label_set_text(dim_vent_lbl, tr("offline", "offline"));
            dim_vent_apply_anim(0);
            lv_obj_clear_flag(dim_vent_fan, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(dim_vent_lbl, LV_OBJ_FLAG_HIDDEN);
        } else if (vent_state.connected) {
            const char * pretty = vent_mode_label();
            if (vent_state.remaining_min > 0)
                lv_label_set_text_fmt(dim_vent_lbl, "%s %dm %d %%",
                                      pretty, vent_state.remaining_min,
                                      vent_state.speed_pct);
            else
                lv_label_set_text_fmt(dim_vent_lbl, "%s %d %%",
                                      pretty, vent_state.speed_pct);
            dim_vent_apply_anim(vent_state.fan_rpm);
            lv_obj_clear_flag(dim_vent_fan, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(dim_vent_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(dim_vent_fan, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(dim_vent_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (wx_icon) {
        if (settings.show_dim_weather && weather_state.day_count > 0) {
            const char * ic = weather_state.days[0].icon;
            lv_img_set_src(wx_icon, weather_icon_for_lg(ic));
            lv_obj_set_style_img_recolor(wx_icon,
                lv_color_hex(weather_icon_color_for(ic)), 0);
            lv_obj_clear_flag(wx_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(wx_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* Forecast strip — 3-hourly to match home screen. Falls back to daily
     * if the hourly feed hasn't populated yet (first 30 s after boot). */
    /* City header — weather only. */
    if (dim_lbl_city) {
        if (settings.show_dim_weather && weather_state.connected) {
            const char * city = settings.weather_location[0]
                                ? settings.weather_location : tr("Verwachting", "Forecast");
            lv_label_set_text_fmt(dim_lbl_city, tr("%s  -  %.1f°C nu", "%s  -  %.1f°C now"),
                                  city, weather_display_temp());
            lv_obj_clear_flag(dim_lbl_city, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(dim_lbl_city, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Life360 — top-right stack under the outside temp. Name prefix +
     * address; colour still identifies who's who. Hidden until data lands. */
    if (dim_lbl_life360_a) {
        if (ha_state.loc_a[0]) {
            lv_label_set_text_fmt(dim_lbl_life360_a, "%s: %s",
                                  settings.life360_a_name[0] ? settings.life360_a_name : "A",
                                  ha_state.loc_a);
            lv_obj_clear_flag(dim_lbl_life360_a, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(dim_lbl_life360_a, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (dim_lbl_life360_b) {
        if (ha_state.loc_b[0]) {
            lv_label_set_text_fmt(dim_lbl_life360_b, "%s: %s",
                                  settings.life360_b_name[0] ? settings.life360_b_name : "B",
                                  ha_state.loc_b);
            lv_obj_clear_flag(dim_lbl_life360_b, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(dim_lbl_life360_b, LV_OBJ_FLAG_HIDDEN);
        }
    }

    int use_hourly = settings.show_dim_weather && settings.forecast_mode != FORECAST_DAILY && weather_state.hour_count > 0;
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        if (!dim_fc_icon[i]) continue;
        if (!settings.show_dim_weather) {
            lv_obj_add_flag(dim_fc_icon[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(dim_fc_day[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(dim_fc_temp[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        /* For the hourly view, skip slot 0 — it's "now" and already lives
         * in the city header above. Daily view shows all 5 days from 0.
         * If the hourly horizon runs short (late evening) fall back to the
         * daily forecast so all 5 columns still carry data. */
        int painted = 0;
        if (use_hourly) {
            int si = i + 1;
            if (si < weather_state.hour_count) {
                const weather_hour_t * h = &weather_state.hours[si];
                lv_img_set_src(dim_fc_icon[i], weather_icon_for(h->icon));
                lv_obj_set_style_img_recolor(dim_fc_icon[i],
                    lv_color_hex(weather_icon_color_for(h->icon)), 0);
                lv_label_set_text(dim_fc_day[i], h->label);
                if (h->wind_dir[0])
                    lv_label_set_text_fmt(dim_fc_temp[i], "%.0f°C  %s%d",
                                          weather_hour_display_temp(h), h->wind_dir, h->wind_bft);
                else
                    lv_label_set_text_fmt(dim_fc_temp[i], "%.0f°C",
                                          weather_hour_display_temp(h));
                painted = 1;
            } else {
                /* Fall back to daily, starting at days[0] (= tomorrow). */
                int di = si - weather_state.hour_count;
                if (di < weather_state.day_count) {
                    const weather_day_t * d = &weather_state.days[di];
                    lv_img_set_src(dim_fc_icon[i], weather_icon_for(d->icon));
                    lv_obj_set_style_img_recolor(dim_fc_icon[i],
                        lv_color_hex(weather_icon_color_for(d->icon)), 0);
                    lv_label_set_text(dim_fc_day[i], d->day);
                    if (d->wind_dir[0])
                        lv_label_set_text_fmt(dim_fc_temp[i], "%.0f/%.0f  %s%d",
                                              d->min_temp, d->max_temp,
                                              d->wind_dir, d->wind_bft);
                    else
                        lv_label_set_text_fmt(dim_fc_temp[i], "%.0f/%.0f°C",
                                              d->min_temp, d->max_temp);
                    painted = 1;
                }
            }
        } else if (i < weather_state.day_count) {
            const weather_day_t * d = &weather_state.days[i];
            lv_img_set_src(dim_fc_icon[i], weather_icon_for(d->icon));
            lv_obj_set_style_img_recolor(dim_fc_icon[i],
                lv_color_hex(weather_icon_color_for(d->icon)), 0);
            lv_label_set_text(dim_fc_day[i], d->day);
            if (d->wind_dir[0])
                lv_label_set_text_fmt(dim_fc_temp[i], "%.0f/%.0f  %s%d",
                                      d->min_temp, d->max_temp,
                                      d->wind_dir, d->wind_bft);
            else
                lv_label_set_text_fmt(dim_fc_temp[i], "%.0f/%.0f°C",
                                      d->min_temp, d->max_temp);
            painted = 1;
        }
        if (painted) {
            lv_obj_clear_flag(dim_fc_icon[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(dim_fc_day[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(dim_fc_temp[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(dim_fc_icon[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(dim_fc_day[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(dim_fc_temp[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (lbl_outside_temp) {
        if (settings.show_dim_weather && weather_state.connected) {
            lv_label_set_text_fmt(lbl_outside_temp, "%.1f C", weather_display_temp());
            lv_obj_clear_flag(lbl_outside_temp, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_outside_temp, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (wx_icon) {
        if (settings.show_dim_weather && weather_state.connected) {
            lv_img_set_src(wx_icon, weather_icon_for_lg(weather_state.current_icon));
            lv_obj_clear_flag(wx_icon, LV_OBJ_FLAG_HIDDEN);
            if (lbl_outside) lv_obj_clear_flag(lbl_outside, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(wx_icon, LV_OBJ_FLAG_HIDDEN);
            if (lbl_outside) lv_obj_add_flag(lbl_outside, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Waste: next upcoming pickup — simplified and persistent for symmetry. */
    if (waste_icon && lbl_waste) {
        if (settings.show_dim_waste && waste_state.connected) {
            waste_pickup_t wp, dummy;
            /* Honour the dim_waste_lead_days setting: only surface the next
               pickup when it falls within the configured lead window (e.g.
               "1 day before"). 0 = filter off → always show. Beyond the
               window the whole cluster hides so the dim screen stays clean
               (the tester set lead=1 but the icon was showing 10 days early
               because this used to call waste_next_2_pickups unconditionally). */
            int have_wp = waste_next_2_pickups(&wp, &dummy) >= 1;
            int within  = have_wp && (settings.dim_waste_lead_days <= 0
                          || waste_days_until(wp.date) <= settings.dim_waste_lead_days);
            if (within) {
                char l_short[48];
                waste_shorten_label(wp.labels, l_short, sizeof l_short);
                const char * l_clean = l_short;
                /* Same day, two types ("Papier+Plastic") → one icon per type.
                   Split on the first '+'. */
                char t1[48], t2[48] = "";
                snprintf(t1, sizeof t1, "%s", l_clean);
                char * plus = strchr(t1, '+');
                if (plus) {
                    *plus = 0;
                    const char * p2 = plus + 1; while (*p2 == ' ') p2++;
                    snprintf(t2, sizeof t2, "%s", p2);
                    size_t n = strlen(t1); while (n && t1[n-1] == ' ') t1[--n] = 0;
                }
                lv_img_set_src(waste_icon, waste_icon_for_label_lg(t1));
                lv_obj_set_style_img_recolor(waste_icon, lv_color_hex(0xffffff), 0);
                lv_obj_set_style_img_recolor_opa(waste_icon, 255, 0);
                if (waste_icon_2) {
                    if (t2[0]) {
                        lv_img_set_src(waste_icon_2, waste_icon_for_label_lg(t2));
                        lv_obj_set_style_img_recolor(waste_icon_2, lv_color_hex(0xffffff), 0);
                        lv_obj_set_style_img_recolor_opa(waste_icon_2, 255, 0);
                        lv_obj_clear_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
                    } else lv_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
                }

                long days_until = waste_days_until(wp.date);
                const char * when = (days_until == 0) ? tr("Vandaag", "Today") : (days_until == 1) ? tr("Morgen", "Tomorrow") : NULL;
                if (when) lv_label_set_text_fmt(lbl_waste, "%s: %s", when, l_clean);
                else {
                    int mo = atoi(wp.date + 5), dy = atoi(wp.date + 8);
                    lv_label_set_text_fmt(lbl_waste, "%d-%d: %s", dy, mo, l_clean);
                }
                lv_obj_clear_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(lbl_waste,  LV_OBJ_FLAG_HIDDEN);
            } else if (settings.dim_waste_lead_days <= 0) {
                /* Filter off and genuinely no upcoming pickup → dimmed placeholder. */
                lv_img_set_src(waste_icon, &icon_trash_lg);
                lv_obj_set_style_img_recolor(waste_icon, lv_color_hex(0xffffff), 0);
                lv_obj_set_style_img_recolor_opa(waste_icon, 100, 0); /* dimmed */
                lv_label_set_text(lbl_waste, tr("Geen", "None"));
                if (waste_icon_2) lv_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(lbl_waste,  LV_OBJ_FLAG_HIDDEN);
            } else {
                /* Lead filter on but the next pickup is beyond the window → hide. */
                lv_obj_add_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(lbl_waste,  LV_OBJ_FLAG_HIDDEN);
                if (waste_icon_2) lv_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_waste,  LV_OBJ_FLAG_HIDDEN);
            if (waste_icon_2) lv_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Energy block: power now (W, white) + gas trailing-hour (m³/h, amber) + solar (W, gold),
     * each a value + fixed-full-scale mini-bar. Gas needs the P1; energy follows
     * settings.energy_source (Toon vs HWE); solar follows energy_solar_w().
     * The whole block can be toggled off via the existing show_dim_bars setting. */
    if (en_pwr_lbl || en_gas_lbl || en_sol_lbl) {
        int   e_conn = energy_connected();
        float e = energy_power_w();
        if (e < 0) e = 0;                          /* export → empty */

        float g_today = energy_gas_today_m3();
        float gh = (g_today >= 0) ? g_today : energy_gas_hour_m3();
        int   g_conn = (gh >= 0);
        float g = gh; if (g < 0) g = 0;

        int   s_conn = energy_have_solar();
        float s = energy_solar_w();
        if (s < 0) s = 0;

        char epwr_val[16], epwr_unit[16];
        char gas_val[16],  gas_unit[16];
        char sol_val[16],  sol_unit[16];

        if (e_conn) {
            if (e >= 1000) {
                snprintf(epwr_val, sizeof epwr_val, "%.1f", e / 1000.0f);
                snprintf(epwr_unit, sizeof epwr_unit, "kW");
            } else {
                snprintf(epwr_val, sizeof epwr_val, "%.0f", e);
                snprintf(epwr_unit, sizeof epwr_unit, "W");
            }
        } else {
            snprintf(epwr_val, sizeof epwr_val, "--");
            snprintf(epwr_unit, sizeof epwr_unit, "W");
        }

        if (g_conn) {
            snprintf(gas_val, sizeof gas_val, "%.2f", g);
            snprintf(gas_unit, sizeof gas_unit, (g_today >= 0) ? "m3" : "m3/h");
        } else {
            snprintf(gas_val, sizeof gas_val, "--");
            snprintf(gas_unit, sizeof gas_unit, "m3");
        }

        if (s_conn) {
            if (s >= 1000) {
                snprintf(sol_val, sizeof sol_val, "%.1f", s / 1000.0f);
                snprintf(sol_unit, sizeof sol_unit, "kW");
            } else {
                snprintf(sol_val, sizeof sol_val, "%.0f", s);
                snprintf(sol_unit, sizeof sol_unit, "W");
            }
        } else {
            snprintf(sol_val, sizeof sol_val, "--");
            snprintf(sol_unit, sizeof sol_unit, "W");
        }

        float er = e / DIM_E_FULL_W;
        float gr = (g_today >= 0) ? (g / 5.0f) : (g / DIM_G_FULL_M3H);
        float sr = s / DIM_E_FULL_W;

        char edisp[32], gdisp[32], sdisp[32];
        snprintf(edisp, sizeof edisp, "%s %s", epwr_val, epwr_unit);
        snprintf(gdisp, sizeof gdisp, "%s %s", gas_val,  gas_unit);
        snprintf(sdisp, sizeof sdisp, "%s %s", sol_val,  sol_unit);

        en_set_bar(en_pwr_fill, en_pwr_lbl, en_pwr_w, er, edisp);
        en_set_bar(en_gas_fill, en_gas_lbl, en_gas_w, gr, gdisp);
        en_set_bar(en_sol_fill, en_sol_lbl, en_sol_w, sr, sdisp);

        en_set_vbar(en_pwr_segs, en_pwr_vlbl, en_pwr_vunit, er, epwr_val, epwr_unit);
        en_set_vbar(en_gas_segs, en_gas_vlbl, en_gas_vunit, gr, gas_val,  gas_unit);
        en_set_vbar_color(en_sol_segs, en_sol_vlbl, en_sol_vunit, sr, sol_val, sol_unit, 0xE5A93B, 0xFFD54F);

        /* show_dim_bars off → hide blocks' contents.
         * dim_bars_vertical chooses horizontal mini-bars vs classic vertical segmented bars. */
        int show = settings.show_dim_bars;
        int vert = settings.dim_bars_vertical;

        lv_obj_t * en_parts[] = { en_pwr_track, en_gas_track, en_sol_track,
                                  en_pwr_lbl,   en_gas_lbl,   en_sol_lbl };
        for (unsigned i = 0; i < sizeof en_parts / sizeof en_parts[0]; i++)
            if (en_parts[i]) {
                if (show && !vert) lv_obj_clear_flag(en_parts[i], LV_OBJ_FLAG_HIDDEN);
                else               lv_obj_add_flag(en_parts[i], LV_OBJ_FLAG_HIDDEN);
            }
        for (int i = 0; i < DIM_GRID_NSEG; i++) {
            if (en_pwr_segs[i]) {
                if (show && vert) lv_obj_clear_flag(en_pwr_segs[i], LV_OBJ_FLAG_HIDDEN);
                else              lv_obj_add_flag(en_pwr_segs[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (en_gas_segs[i]) {
                if (show && vert) lv_obj_clear_flag(en_gas_segs[i], LV_OBJ_FLAG_HIDDEN);
                else              lv_obj_add_flag(en_gas_segs[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (en_sol_segs[i]) {
                if (show && vert) lv_obj_clear_flag(en_sol_segs[i], LV_OBJ_FLAG_HIDDEN);
                else              lv_obj_add_flag(en_sol_segs[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (en_pwr_vlbl) {
            if (show && vert) lv_obj_clear_flag(en_pwr_vlbl, LV_OBJ_FLAG_HIDDEN);
            else              lv_obj_add_flag(en_pwr_vlbl, LV_OBJ_FLAG_HIDDEN);
        }
        if (en_gas_vlbl) {
            if (show && vert) lv_obj_clear_flag(en_gas_vlbl, LV_OBJ_FLAG_HIDDEN);
            else              lv_obj_add_flag(en_gas_vlbl, LV_OBJ_FLAG_HIDDEN);
        }
        if (en_sol_vlbl) {
            if (show && vert) lv_obj_clear_flag(en_sol_vlbl, LV_OBJ_FLAG_HIDDEN);
            else              lv_obj_add_flag(en_sol_vlbl, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_t * en_vunits[] = { en_pwr_vunit, en_gas_vunit, en_sol_vunit };
        for (unsigned i = 0; i < sizeof en_vunits / sizeof en_vunits[0]; i++) {
            if (en_vunits[i]) {
                if (show && vert) lv_obj_clear_flag(en_vunits[i], LV_OBJ_FLAG_HIDDEN);
                else              lv_obj_add_flag(en_vunits[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    /* Agenda / Calendar block — 2-column (dd/mm hh:mm + title), height-adaptive, white text */
    if (dim_agenda_row_count > 0) {
        if (calendar_state.count > 0) {
            for (int r = 0; r < dim_agenda_row_count; r++) {
                if (!dim_lbl_agenda_time[r] || !dim_lbl_agenda_title[r]) continue;
                if (r < calendar_state.count) {
                    char dt[32];
                    format_agenda_date_time(&calendar_state.ev[r], dt, sizeof(dt));
                    lv_label_set_text(dim_lbl_agenda_time[r], dt);
                    lv_label_set_text(dim_lbl_agenda_title[r], calendar_state.ev[r].summary);
                } else {
                    lv_label_set_text(dim_lbl_agenda_time[r], "");
                    lv_label_set_text(dim_lbl_agenda_title[r], "");
                }
            }
        } else {
            if (dim_lbl_agenda_time[0]) lv_label_set_text(dim_lbl_agenda_time[0], "");
            if (dim_lbl_agenda_title[0]) lv_label_set_text(dim_lbl_agenda_title[0], tr("Geen afspraken", "No appointments"));
            for (int r = 1; r < dim_agenda_row_count; r++) {
                if (dim_lbl_agenda_time[r]) lv_label_set_text(dim_lbl_agenda_time[r], "");
                if (dim_lbl_agenda_title[r]) lv_label_set_text(dim_lbl_agenda_title[r], "");
            }
        }
    }

    if (dim_rain_chart && dim_rain_ser) {
        buienradar_rain_snapshot_t rsnap;
        buienradar_rain_get_snapshot(&rsnap);
        if (dim_rain_lbl_status) {
            lv_label_set_text(dim_rain_lbl_status, rsnap.summary_3h[0] ? rsnap.summary_3h : "");
        }
        int has_rain = rsnap.has_rain_3h;
        if (rsnap.is_valid && !has_rain) {
            if (dim_rain_lbl_empty) lv_obj_clear_flag(dim_rain_lbl_empty, LV_OBJ_FLAG_HIDDEN);
            if (dim_rain_hdr) lv_obj_add_flag(dim_rain_hdr, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(dim_rain_chart, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < 3; i++) {
                if (dim_thresh_lbl[i]) lv_obj_add_flag(dim_thresh_lbl[i], LV_OBJ_FLAG_HIDDEN);
            }
            for (int i = 0; i < 4; i++) {
                if (dim_rain_lbl_times[i]) lv_obj_add_flag(dim_rain_lbl_times[i], LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            if (dim_rain_lbl_empty) lv_obj_add_flag(dim_rain_lbl_empty, LV_OBJ_FLAG_HIDDEN);
            if (dim_rain_hdr) lv_obj_clear_flag(dim_rain_hdr, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(dim_rain_chart, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < 3; i++) {
                if (dim_thresh_lbl[i]) lv_obj_clear_flag(dim_thresh_lbl[i], LV_OBJ_FLAG_HIDDEN);
            }
            for (int i = 0; i < 4; i++) {
                if (dim_rain_lbl_times[i]) lv_obj_clear_flag(dim_rain_lbl_times[i], LV_OBJ_FLAG_HIDDEN);
            }
            int n = rsnap.count_3h;
            if (n <= 0) n = 38;
            lv_chart_set_point_count(dim_rain_chart, n);
            for (int i = 0; i < n; i++) {
                dim_rain_ser->y_points[i] = (i < rsnap.count_3h) ? rsnap.points_3h[i].value : 0;
            }
            lv_chart_refresh(dim_rain_chart);
            if (rsnap.count_3h > 0) {
                if (dim_rain_lbl_times[0]) lv_label_set_text(dim_rain_lbl_times[0], tr("Nu", "Now"));
                if (dim_rain_lbl_times[1]) lv_label_set_text(dim_rain_lbl_times[1], rsnap.points_3h[n / 3].time_str);
                if (dim_rain_lbl_times[2]) lv_label_set_text(dim_rain_lbl_times[2], rsnap.points_3h[2 * n / 3].time_str);
                if (dim_rain_lbl_times[3]) {
                    lv_label_set_text(dim_rain_lbl_times[3], rsnap.points_3h[n - 1].time_str);
                    if (dim_rain_chart) lv_obj_align_to(dim_rain_lbl_times[3], dim_rain_chart, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);
                }
            }
        }
    }
}

/* Grid (customizable) dim — invoked by screen_dim_create() only when
 * settings.dim_custom_enabled is set. The default dim lives in screen_dim.c. */
lv_obj_t * screen_dim_grid_create(void) {
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Package banner overlay (hidden when queue empty). Attached BEFORE
     * the wake-tap event handler so its CLICKABLE flag wins over the
     * screen-wide wake — tapping the banner dismisses it without also
     * waking the home screen. */
    packages_banner_attach(scr_root);

    /* Whole screen is a wake target. */
    lv_obj_add_flag(scr_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_root, on_wake_tap, LV_EVENT_CLICKED, NULL);

    /* Clock — custom 96pt Montserrat inside its customizable dim box (DB_CLOCK) */
    lv_obj_t * bx_clock = dim_box(DB_CLOCK, NULL, NULL);
    lbl_clock_colon = lv_label_create(bx_clock);
    lv_obj_set_style_text_color(lbl_clock_colon, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_clock_colon, SF(96), 0);
    lv_label_set_text(lbl_clock_colon, ":");
    lv_obj_align(lbl_clock_colon, LV_ALIGN_CENTER, 0, 0);

    lbl_clock_h = lv_label_create(bx_clock);
    lv_obj_set_style_text_color(lbl_clock_h, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_clock_h, SF(96), 0);
    lv_label_set_text(lbl_clock_h, "--");

    lbl_clock_m = lv_label_create(bx_clock);
    lv_obj_set_style_text_color(lbl_clock_m, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_clock_m, SF(96), 0);
    lv_label_set_text(lbl_clock_m, "--");

    lv_obj_update_layout(lbl_clock_colon);
    lv_obj_update_layout(lbl_clock_h);
    lv_obj_update_layout(lbl_clock_m);
    lv_obj_align_to(lbl_clock_h, lbl_clock_colon, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    lv_obj_align_to(lbl_clock_m, lbl_clock_colon, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    /* Date — inside its customizable dim box (DB_DATE) */
    int dw = 0, dh = 0;
    lv_obj_t * bx_date = dim_box(DB_DATE, &dw, &dh);
    s_date_box_w = dw;
    s_date_box_h = dh;
    lbl_date = lv_label_create(bx_date);
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_date, SF(28), 0);
    lv_obj_set_style_text_align(lbl_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_date, "");
    lv_obj_align(lbl_date, LV_ALIGN_CENTER, 0, 0);

    /* ===== peripheral blocks — each rendered INSIDE its grid cell so the
     * "Dim indeling" editor is true WYSIWYG (content lands where the cell is).
     * Within a box, offsets are raw px (the box is already cell-sized). ===== */
    const int t1 = (DISP_VER < 600); (void)t1;

    /* ---- WEATHER: [icon + temp] as ONE centred row, "Buiten" under it,
     * moon bottom-left. Keeping icon+temp in a flex row means the sun always
     * sits right next to the temperature and the pair stays centred in the
     * block — no matter how wide the block is — instead of the icon flying to
     * the left edge and the temp to the right edge. ---- */
    lv_obj_t * bx_w = dim_box(DB_WEATHER, NULL, NULL);
    /* Outside-temp font grows when the weather block is made WIDER in the editor
     * (wider tile = room for a bigger, clearer temp — the tester's request). */
    int w_grow = g_dim_blocks[DB_WEATHER].w - dim_block_default(DB_WEATHER).w;
    const lv_font_t * wtemp_font = (w_grow >= 1) ? &lv_font_montserrat_48 : &lv_font_montserrat_28;

    lv_obj_t * wrow = lv_obj_create(bx_w);
    lv_obj_remove_style_all(wrow);
    lv_obj_set_size(wrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wrow, 8, 0);
    lv_obj_clear_flag(wrow, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(wrow, LV_ALIGN_TOP_MID, 0, 10);

    wx_icon = lv_img_create(wrow);
    lv_img_set_src(wx_icon, &icon_wx_cloud_lg);
    lv_obj_set_style_img_recolor(wx_icon, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_img_recolor_opa(wx_icon, 255, 0);

    /* temp over "Buiten" in a column, so "Buiten" sits directly UNDER the
     * temperature (not centred under the whole icon+temp pair). */
    lv_obj_t * wcol = lv_obj_create(wrow);
    lv_obj_remove_style_all(wcol);
    lv_obj_set_size(wcol, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wcol, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(wcol, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lbl_outside_temp = lv_label_create(wcol);
    lv_obj_set_style_text_color(lbl_outside_temp, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_outside_temp, wtemp_font, 0);
    lv_label_set_long_mode(lbl_outside_temp, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl_outside_temp, LV_SIZE_CONTENT);
    lv_label_set_text(lbl_outside_temp, "--");

    lbl_outside = lv_label_create(wcol);
    lv_obj_set_style_text_color(lbl_outside, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_outside, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_outside, tr("Buiten", "Outside"));

    /* Moon phase icon removed per CR11 */
    dim_moon_img = NULL;

    /* ---- WASTE: bin icon(s) centred top, label centred below ---- */
    int sbw = 0, sbh = 0; lv_obj_t * bx_s = dim_box(DB_WASTE, &sbw, &sbh); (void)sbh;
    waste_icon = lv_img_create(bx_s);
    lv_img_set_src(waste_icon, &icon_trash_lg);
    lv_obj_set_style_img_recolor(waste_icon, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_img_recolor_opa(waste_icon, 255, 0);
    lv_obj_align(waste_icon, LV_ALIGN_TOP_MID, -6, 8);

    waste_icon_2 = lv_img_create(bx_s);
    lv_img_set_src(waste_icon_2, &icon_trash_lg);
    lv_obj_set_style_img_recolor_opa(waste_icon_2, 255, 0);
    lv_obj_align(waste_icon_2, LV_ALIGN_TOP_MID, 46, 8);
    lv_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);

    lbl_waste = lv_label_create(bx_s);
    lv_obj_set_style_text_color(lbl_waste, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_waste, (DISP_VER < 600) ? &lv_font_montserrat_14 : &lv_font_montserrat_18, 0);
    lv_obj_set_width(lbl_waste, sbw - 8);
    lv_obj_set_style_text_align(lbl_waste, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_waste, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_waste, LV_ALIGN_TOP_MID, 0, (DISP_VER < 600) ? 90 : 92);

    /* ---- THERMO: indoor temp + setpoint/program state below ---- */
    int tbw = 0, tbh = 0; lv_obj_t * bx_t = dim_box(DB_THERMO, &tbw, &tbh); (void)tbh;
    lbl_temp = lv_label_create(bx_t);
    lv_obj_set_style_text_color(lbl_temp, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_temp, SF(96), 0);
    lv_label_set_text(lbl_temp, "--.-°C");
    lv_obj_align(lbl_temp, LV_ALIGN_CENTER, 0, -14);

    lbl_setpoint = lv_label_create(bx_t);
    lv_obj_set_style_text_color(lbl_setpoint, lv_color_hex(0xdddddd), 0);
    lv_obj_set_style_text_font(lbl_setpoint, SF(20), 0);
    lv_obj_set_width(lbl_setpoint, (tbw > 0) ? tbw : LV_PCT(100));
    lv_obj_set_style_text_align(lbl_setpoint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_setpoint, "");
    lv_obj_align(lbl_setpoint, LV_ALIGN_CENTER, 0, (DISP_VER < 600 ? 28 : 34));

    lbl_program  = NULL;
    lbl_metrics  = NULL;
    lbl_burner   = NULL;

    dim_img_flame = lv_img_create(bx_t);
    lv_img_set_src(dim_img_flame, &icon_radiator);
    lv_img_set_zoom(dim_img_flame, 256);
    lv_obj_set_style_img_recolor(dim_img_flame, lv_color_hex(0xff8866), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_flame, 255, 0);
    lv_obj_align_to(dim_img_flame, lbl_temp, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    lv_obj_add_flag(dim_img_flame, LV_OBJ_FLAG_HIDDEN);

    dim_img_faucet = lv_img_create(bx_t);
    lv_img_set_src(dim_img_faucet, &icon_faucet);
    lv_img_set_zoom(dim_img_faucet, 256);
    lv_obj_set_style_img_recolor(dim_img_faucet, lv_color_hex(0xff5544), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_faucet, 255, 0);
    lv_obj_align_to(dim_img_faucet, lbl_temp, LV_ALIGN_OUT_BOTTOM_MID, -10, 4);
    lv_obj_add_flag(dim_img_faucet, LV_OBJ_FLAG_HIDDEN);

    dim_img_drop = lv_img_create(bx_t);
    lv_img_set_src(dim_img_drop, &icon_drop);
    lv_img_set_zoom(dim_img_drop, 256);
    lv_obj_set_style_img_recolor(dim_img_drop, lv_color_hex(0xff5544), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_drop, 255, 0);
    lv_obj_align_to(dim_img_drop, dim_img_faucet, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    lv_obj_add_flag(dim_img_drop, LV_OBJ_FLAG_HIDDEN);

    dim_img_water = lv_img_create(bx_t);
    lv_img_set_src(dim_img_water, &icon_drop);
    lv_img_set_zoom(dim_img_water, 256);
    lv_obj_set_style_img_recolor(dim_img_water, lv_color_hex(0x66bbff), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_water, 255, 0);
    lv_obj_align(dim_img_water, LV_ALIGN_TOP_MID, -150, 18);
    lv_obj_add_flag(dim_img_water, LV_OBJ_FLAG_HIDDEN);

    dim_lbl_water = lv_label_create(bx_t);
    lv_obj_set_style_text_color(dim_lbl_water, lv_color_hex(0x66bbff), 0);
    lv_obj_set_style_text_font(dim_lbl_water, &lv_font_montserrat_22, 0);
    lv_label_set_text(dim_lbl_water, "");
    lv_obj_align(dim_lbl_water, LV_ALIGN_TOP_MID, -110, 18);
    lv_obj_add_flag(dim_lbl_water, LV_OBJ_FLAG_HIDDEN);

    /* ---- VENT: fan icon centred, mode/% below ---- */
    int vbw = 0, vbh = 0; lv_obj_t * bx_v = dim_box(DB_VENT, &vbw, &vbh); (void)vbh;
    dim_vent_fan = lv_img_create(bx_v);
    lv_img_set_src(dim_vent_fan, &icon_fan);
    lv_img_set_zoom(dim_vent_fan, 128);
    lv_obj_set_style_img_recolor(dim_vent_fan, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_img_recolor_opa(dim_vent_fan, 255, 0);
    lv_img_set_pivot(dim_vent_fan, 40, 40);
    lv_obj_align(dim_vent_fan, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_add_flag(dim_vent_fan, LV_OBJ_FLAG_HIDDEN);

    dim_vent_lbl = lv_label_create(bx_v);
    lv_obj_set_style_text_color(dim_vent_lbl, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(dim_vent_lbl, &lv_font_montserrat_18, 0);
    lv_label_set_text(dim_vent_lbl, "-- %");
    lv_obj_set_style_text_align(dim_vent_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(dim_vent_lbl, vbw - 4);
    /* The fan IMG object is 80px tall (zoom only shrinks the drawn pixels, not
     * the layout box), so the label must clear y=88, not y=52, or it lands on
     * the fan — same offset the waste block uses for its 80px bin glyph. */
    lv_obj_align(dim_vent_lbl, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_add_flag(dim_vent_lbl, LV_OBJ_FLAG_HIDDEN);

    /* ---- FAMILY: Life360 lines stacked, right-aligned ---- */
    int fbw = 0, fbh = 0; lv_obj_t * bx_f = dim_box(DB_FAMILY, &fbw, &fbh); (void)fbh;
    dim_lbl_life360_a = lv_label_create(bx_f);
    lv_obj_set_style_text_color(dim_lbl_life360_a, lv_color_hex(0x88aaff), 0);
    lv_obj_set_style_text_font(dim_lbl_life360_a, &lv_font_montserrat_18, 0);
    lv_obj_set_width(dim_lbl_life360_a, fbw - 8);
    lv_obj_set_style_text_align(dim_lbl_life360_a, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(dim_lbl_life360_a, LV_LABEL_LONG_DOT);
    lv_label_set_text(dim_lbl_life360_a, "");
    lv_obj_align(dim_lbl_life360_a, LV_ALIGN_TOP_RIGHT, -4, 8);
    lv_obj_add_flag(dim_lbl_life360_a, LV_OBJ_FLAG_HIDDEN);

    dim_lbl_life360_b = lv_label_create(bx_f);
    lv_obj_set_style_text_color(dim_lbl_life360_b, lv_color_hex(0xff88cc), 0);
    lv_obj_set_style_text_font(dim_lbl_life360_b, &lv_font_montserrat_18, 0);
    lv_obj_set_width(dim_lbl_life360_b, fbw - 8);
    lv_obj_set_style_text_align(dim_lbl_life360_b, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(dim_lbl_life360_b, LV_LABEL_LONG_DOT);
    lv_label_set_text(dim_lbl_life360_b, "");
    lv_obj_align(dim_lbl_life360_b, LV_ALIGN_TOP_RIGHT, -4, 34);
    lv_obj_add_flag(dim_lbl_life360_b, LV_OBJ_FLAG_HIDDEN);

    dim_lbl_city  = NULL;
    waste_box_ptr = NULL;

    /* ---- FORECAST: 5 columns across the block ---- */
    int fcw = 0, fch = 0; lv_obj_t * bx_c = dim_box(DB_FORECAST, &fcw, &fch); (void)fch;
    int col_w = fcw / WEATHER_FORECAST_DAYS;
    const lv_font_t * fc_font = (DISP_VER < 600) ? &lv_font_montserrat_14 : &lv_font_montserrat_18;
    int day_dy  = (DISP_VER < 600) ? 42 : 46;
    int temp_dy = (DISP_VER < 600) ? 60 : 66;
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        int cx = i * col_w + col_w / 2;

        dim_fc_icon[i] = lv_img_create(bx_c);
        lv_img_set_src(dim_fc_icon[i], &icon_wx_cloud);
        lv_obj_set_style_img_recolor(dim_fc_icon[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_img_recolor_opa(dim_fc_icon[i], 255, 0);
        lv_obj_set_pos(dim_fc_icon[i], cx - 20, (DISP_VER < 600) ? 2 : 4);
        lv_obj_add_flag(dim_fc_icon[i], LV_OBJ_FLAG_HIDDEN);

        dim_fc_day[i] = lv_label_create(bx_c);
        lv_obj_set_style_text_color(dim_fc_day[i], lv_color_hex(0xbbbbbb), 0);
        lv_obj_set_style_text_font(dim_fc_day[i], fc_font, 0);
        lv_label_set_text(dim_fc_day[i], "");
        lv_obj_set_style_text_align(dim_fc_day[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(dim_fc_day[i], col_w);
        lv_obj_set_pos(dim_fc_day[i], i * col_w, day_dy);
        lv_obj_add_flag(dim_fc_day[i], LV_OBJ_FLAG_HIDDEN);

        dim_fc_temp[i] = lv_label_create(bx_c);
        lv_obj_set_style_text_color(dim_fc_temp[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(dim_fc_temp[i], fc_font, 0);
        lv_label_set_text(dim_fc_temp[i], "");
        lv_obj_set_style_text_align(dim_fc_temp[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(dim_fc_temp[i], col_w);
        lv_obj_set_pos(dim_fc_temp[i], i * col_w, temp_dy);
        lv_obj_add_flag(dim_fc_temp[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- STROOM (power) + GAS + ZON: separate single-metric blocks, each a value +
     * mini-bar. Independent so they can be placed/sized on their own. ---- */
    int ebw = 0, ebh = 0; lv_obj_t * bx_e = dim_box(DB_ENERGY, &ebw, &ebh);
    en_pwr_w = ebw - 8;
    en_make_bar(bx_e, en_pwr_w, 6, 0xffffff, &en_pwr_track, &en_pwr_fill, &en_pwr_lbl);
    en_make_vbar(bx_e, ebw, ebh, en_pwr_segs, &en_pwr_vlbl, &en_pwr_vunit);

    int gbw = 0, gbh = 0; lv_obj_t * bx_g = dim_box(DB_GAS, &gbw, &gbh);
    en_gas_w = gbw - 8;
    en_make_bar(bx_g, en_gas_w, 6, 0xffffff, &en_gas_track, &en_gas_fill, &en_gas_lbl);
    en_make_vbar(bx_g, gbw, gbh, en_gas_segs, &en_gas_vlbl, &en_gas_vunit);

    int sbw_sol = 0, sbh_sol = 0; lv_obj_t * bx_sol = dim_box(DB_SOLAR, &sbw_sol, &sbh_sol);
    en_sol_w = sbw_sol - 8;
    en_make_bar(bx_sol, en_sol_w, 6, 0xffcc00, &en_sol_track, &en_sol_fill, &en_sol_lbl);
    en_make_vbar(bx_sol, sbw_sol, sbh_sol, en_sol_segs, &en_sol_vlbl, &en_sol_vunit);

    /* ---- AGENDA: next calendar event(s) — 2-column, height-adaptive, all white text ---- */
    int abw = 0, abh = 0; lv_obj_t * bx_a = dim_box(DB_AGENDA, &abw, &abh);
    for (int r = 0; r < MAX_DIM_AGENDA_ROWS; r++) {
        dim_lbl_agenda_time[r] = NULL;
        dim_lbl_agenda_title[r] = NULL;
    }
    dim_agenda_row_count = 0;

    if (bx_a && abh > 20) {
        int row_h = (DISP_VER < 600) ? 20 : 22;
        int max_fit = (abh - 6) / row_h;
        if (max_fit < 1) max_fit = 1;
        if (max_fit > MAX_DIM_AGENDA_ROWS) max_fit = MAX_DIM_AGENDA_ROWS;
        dim_agenda_row_count = max_fit;

        int time_col_w = (DISP_HOR < 1000) ? 92 : 102;
        int title_x = time_col_w + 6;
        int title_w = abw - title_x - 4;
        if (title_w < 50) title_w = 50;

        for (int r = 0; r < dim_agenda_row_count; r++) {
            /* Col 1: Date & Time (dd/mm hh:mm) */
            lv_obj_t * lt = lv_label_create(bx_a);
            lv_obj_set_style_text_color(lt, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_text_font(lt, SF(15), 0);
            lv_obj_set_width(lt, time_col_w);
            lv_label_set_text(lt, "");
            lv_obj_align(lt, LV_ALIGN_TOP_LEFT, 4, 3 + r * row_h);
            dim_lbl_agenda_time[r] = lt;

            /* Col 2: Event title / summary */
            lv_obj_t * ls = lv_label_create(bx_a);
            lv_obj_set_style_text_color(ls, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_text_font(ls, SF(15), 0);
            lv_obj_set_width(ls, title_w);
            lv_label_set_long_mode(ls, LV_LABEL_LONG_DOT);
            lv_label_set_text(ls, "");
            lv_obj_align(ls, LV_ALIGN_TOP_LEFT, title_x, 3 + r * row_h);
            dim_lbl_agenda_title[r] = ls;
        }
    }

    /* ---- NEERSLAG (Buienradar 3h rain graph) ---- */
    int rbw = 0, rbh = 0;
    lv_obj_t * bx_r = dim_box(DB_RAINFALL, &rbw, &rbh);
    dim_rain_box = bx_r;
    if (bx_r && rbh > 30) {
        lv_obj_t * rhdr = lv_obj_create(bx_r);
        dim_rain_hdr = rhdr;
        lv_obj_set_size(rhdr, LV_PCT(100), 28);
        lv_obj_align(rhdr, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_opa(rhdr, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(rhdr, 0, 0);
        lv_obj_set_style_pad_all(rhdr, 0, 0);
        lv_obj_clear_flag(rhdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t * r_icon = lv_img_create(rhdr);
        lv_img_set_src(r_icon, &icon_drop);
        lv_img_set_zoom(r_icon, 160);
        lv_obj_set_style_img_recolor(r_icon, lv_color_hex(0x5a9bd3), 0);
        lv_obj_set_style_img_recolor_opa(r_icon, 255, 0);
        lv_obj_align(r_icon, LV_ALIGN_LEFT_MID, 2, 0);

        dim_rain_lbl_title = lv_label_create(rhdr);
        lv_obj_set_style_text_color(dim_rain_lbl_title, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(dim_rain_lbl_title, SF(16), 0);
        lv_label_set_text(dim_rain_lbl_title, tr("Neerslag", "Rain"));
        lv_obj_align_to(dim_rain_lbl_title, r_icon, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

        dim_rain_lbl_status = lv_label_create(rhdr);
        lv_obj_set_style_text_color(dim_rain_lbl_status, lv_color_hex(0x88aabb), 0);
        lv_obj_set_style_text_font(dim_rain_lbl_status, SF(13), 0);
        if (rbw > 200) lv_obj_set_width(dim_rain_lbl_status, rbw - 140);
        lv_label_set_long_mode(dim_rain_lbl_status, LV_LABEL_LONG_DOT);
        lv_label_set_text(dim_rain_lbl_status, "");
        lv_obj_align(dim_rain_lbl_status, LV_ALIGN_RIGHT_MID, -2, 0);

        dim_rain_chart = lv_chart_create(bx_r);
        int label_w = (rbw > 180) ? 44 : 0;
        int chart_w = rbw - 8 - label_w;
        int chart_h = rbh - 54;
        if (chart_h > 65) chart_h = 65;
        if (chart_h < 38) chart_h = 38;
        lv_obj_set_size(dim_rain_chart, chart_w, chart_h);
        lv_obj_align(dim_rain_chart, LV_ALIGN_BOTTOM_RIGHT, -4, -20);
        lv_obj_set_style_bg_color(dim_rain_chart, lv_color_hex(0x141e2a), 0);
        lv_obj_set_style_border_color(dim_rain_chart, lv_color_hex(0x1e2c3e), 0);
        lv_obj_set_style_border_width(dim_rain_chart, 1, 0);
        lv_obj_set_style_radius(dim_rain_chart, 8, 0);
        lv_obj_set_style_pad_ver(dim_rain_chart, 2, 0);
        lv_obj_set_style_pad_hor(dim_rain_chart, 0, 0);
        lv_obj_clear_flag(dim_rain_chart, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_chart_set_type(dim_rain_chart, LV_CHART_TYPE_BAR);
        lv_chart_set_range(dim_rain_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
        lv_chart_set_div_line_count(dim_rain_chart, 4, 4);
        lv_obj_set_style_line_color(dim_rain_chart, lv_color_hex(0x223344), LV_PART_MAIN);
        lv_obj_set_style_line_opa(dim_rain_chart, LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_pad_column(dim_rain_chart, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(dim_rain_chart, 2, LV_PART_ITEMS);

        dim_rain_ser = lv_chart_add_series(dim_rain_chart, lv_color_hex(0x5a9bd3), LV_CHART_AXIS_PRIMARY_Y);

        static const char * thresh_names[] = {"Zwaar", "Matig", "Licht"};
        for (int i = 0; i < 3; i++) {
            if (label_w > 0) {
                dim_thresh_lbl[i] = lv_label_create(bx_r);
                lv_obj_set_style_text_color(dim_thresh_lbl[i], lv_color_hex(0x88aabb), 0);
                lv_obj_set_style_text_font(dim_thresh_lbl[i], SF(11), 0);
                lv_label_set_text(dim_thresh_lbl[i], thresh_names[i]);
                int y_off = (chart_h * i) / 3 - 6;
                lv_obj_align_to(dim_thresh_lbl[i], dim_rain_chart, LV_ALIGN_OUT_LEFT_TOP, -4, y_off);
            } else {
                dim_thresh_lbl[i] = NULL;
            }
        }

        for (int i = 0; i < 4; i++) {
            dim_rain_lbl_times[i] = lv_label_create(bx_r);
            lv_obj_set_style_text_color(dim_rain_lbl_times[i], lv_color_hex(0x88aabb), 0);
            lv_obj_set_style_text_font(dim_rain_lbl_times[i], SF(12), 0);
            lv_label_set_text(dim_rain_lbl_times[i], "");
            if (i == 0) {
                lv_obj_align_to(dim_rain_lbl_times[i], dim_rain_chart, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);
            } else if (i == 3) {
                lv_obj_align_to(dim_rain_lbl_times[i], dim_rain_chart, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);
            } else {
                lv_obj_align_to(dim_rain_lbl_times[i], dim_rain_chart, LV_ALIGN_OUT_BOTTOM_LEFT, (chart_w * i) / 3 - 6, 2);
            }
        }

        dim_rain_lbl_empty = lv_label_create(bx_r);
        lv_obj_set_style_text_color(dim_rain_lbl_empty, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(dim_rain_lbl_empty, SF(15), 0);
        lv_label_set_text(dim_rain_lbl_empty, tr("Geen neerslag verwacht", "No rain expected"));
        lv_obj_align(dim_rain_lbl_empty, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(dim_rain_lbl_empty, LV_OBJ_FLAG_HIDDEN);
    }

    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}
