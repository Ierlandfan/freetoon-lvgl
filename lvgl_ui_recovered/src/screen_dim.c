/*
 * Dim/ambient screen — pure black background, large white clock plus
 * indoor temp and setpoint. Tap anywhere to wake.
 * No colour, no icons; this is the screen we want visible while idle.
 */
#include "screens.h"
#include "display.h"
#include "boxtalk.h"
#include "settings.h"
#include "homewizard.h"
#include "domoticz.h"
#include "meteradapter.h"
#include "homeassistant.h"
#include "energy_hist.h"
#include "packages.h"
#include "weather.h"
#include "wastecollection.h"
#include "ventilation.h"
#include "icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(lv_font_montserrat_96_custom);

static lv_obj_t * scr_root = NULL;

/* ---- usage bars flanking the clock: energy now (W) + gas hourly (m³) ----
 * Restored (wired to settings.show_dim_bars; default on) after the symmetric
 * rework dropped them. Vertical bars at the outer edges, centred on the temp
 * row; the fill grows up. Fixed full-scale (DIM_E_FULL_W / DIM_G_FULL_M3H), so a
 * maxed bar is ~2x clock height. The waste (left) and weather (right) blocks sit
 * raised on the date row (see screen_dim_create), so the date / waste date /
 * "Buiten" line up and a full-height bar passes the edges without crowding. */
#define DIM_BAR_W     44      /* bar width, design px (SX-scaled) */
#define DIM_BAR_INSET 20      /* gap from the screen bezel (outer edges) */
#define DIM_BAR_Y     45      /* vertical centre = the indoor-temp row */
#define DIM_CLOCK_H   96      /* clock font px; envelope = 2x this */
#define DIM_E_FULL_W    5000.0f   /* power at full bar height (fixed scale) */
#define DIM_G_FULL_M3H  2.0f      /* gas (m³/h) at full bar height */
static lv_obj_t * bar_l_env, * bar_l_fill, * bar_l_cap;
static lv_obj_t * bar_r_env, * bar_r_fill, * bar_r_cap;
static int   dim_bar_h = 0;        /* envelope height (px, computed at create) */

static lv_obj_t * lbl_clock;
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
static lv_obj_t * dim_vent_fan  = NULL;   /* spinning fan icon */
static lv_obj_t * dim_vent_lbl  = NULL;   /* "57 %" — actual ExhFanSpeed */
static lv_obj_t * dim_img_water = NULL;   /* drop icon, visible while pouring */
static lv_obj_t * dim_lbl_water = NULL;   /* "1.4 L/m" / "+1.4 L" */
static int        dim_vent_period_ms = 0; /* current spin animation period */
static lv_timer_t * refresh_timer = NULL;

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
    /* Same linear curve as the home tile: period_ms = 3500 - rpm, clamped. */
    int period = 3500 - rpm;
    if (period < 200)  period = 200;
    if (period > 3500) period = 3500;
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

/* Build one bar slot (side: -1 left edge, +1 right edge). Envelope is an
 * invisible container at the screen edge, vertically centred on the temp row;
 * the coloured fill is a child anchored to the bottom that grows upward. The
 * caption is align_to'd to the envelope so it reads inward. Neither is
 * CLICKABLE, so a tap on the bar still wakes the screen. */
static void dim_make_bar(int side, lv_obj_t ** env, lv_obj_t ** fill,
                         lv_obj_t ** cap) {
    int bw = SX(DIM_BAR_W);
    lv_align_t al = (side < 0) ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID;
    int xinset = (side < 0) ? SX(DIM_BAR_INSET) : -SX(DIM_BAR_INSET);

    *env = lv_obj_create(scr_root);
    lv_obj_remove_style_all(*env);
    lv_obj_set_size(*env, bw, dim_bar_h);
    lv_obj_set_style_bg_opa(*env, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(*env, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(*env, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(*env, al, xinset, SY(DIM_BAR_Y));

    *fill = lv_obj_create(*env);
    lv_obj_remove_style_all(*fill);
    lv_obj_set_width(*fill, bw);
    lv_obj_set_height(*fill, 0);
    lv_obj_set_style_bg_color(*fill, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(*fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(*fill, 4, 0);
    lv_obj_clear_flag(*fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(*fill, LV_ALIGN_BOTTOM_MID, 0, 0);

    *cap = lv_label_create(scr_root);
    lv_obj_set_style_text_color(*cap, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(*cap, SF(18), 0);
    lv_obj_set_style_text_align(*cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(*cap, "");
    lv_obj_align_to(*cap, *env,
                    (side < 0) ? LV_ALIGN_OUT_BOTTOM_LEFT : LV_ALIGN_OUT_BOTTOM_RIGHT,
                    0, SY(14));
}

/* Apply a value to a bar slot. ratio 0..1 of the envelope. Hidden when !show. */
static void dim_bar_set(lv_obj_t * env, lv_obj_t * fill, lv_obj_t * cap,
                        int side, int show, int text_only,
                        float ratio, uint32_t color, const char * txt) {
    if (!env) return;
    if (!show) {
        lv_obj_add_flag(env, LV_OBJ_FLAG_HIDDEN);
        if (cap) lv_obj_add_flag(cap, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (text_only) {
        lv_obj_add_flag(env, LV_OBJ_FLAG_HIDDEN);
        if (cap) {
            lv_label_set_text(cap, txt);
            lv_obj_set_style_text_color(cap, lv_color_hex(color), 0);
            lv_obj_set_style_text_font(cap, SF(22), 0);
            lv_obj_align_to(cap, env,
                (side < 0) ? LV_ALIGN_OUT_RIGHT_MID : LV_ALIGN_OUT_LEFT_MID,
                (side < 0) ? SX(6) : -SX(6), 0);
            lv_obj_clear_flag(cap, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    int h = (int)(ratio * dim_bar_h + 0.5f);
    if (ratio > 0 && h < 3) h = 3;            /* show a sliver when nonzero */
    lv_obj_set_height(fill, h);
    lv_obj_align(fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(color), 0);
    lv_obj_clear_flag(env, LV_OBJ_FLAG_HIDDEN);
    if (cap) {
        lv_label_set_text(cap, txt);
        lv_obj_set_style_text_color(cap, lv_color_hex(color), 0);
        lv_obj_set_style_text_font(cap, SF(18), 0);
        lv_obj_align_to(cap, env,
            (side < 0) ? LV_ALIGN_OUT_BOTTOM_LEFT : LV_ALIGN_OUT_BOTTOM_RIGHT,
            0, SY(14));
        lv_obj_clear_flag(cap, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_wake_tap(lv_event_t * e) {
    (void)e;
    ui_wake_now();
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    static int n = 0;
    if (++n % 5 == 0) fprintf(stderr, "[dim] tick t=%.2f sp=%.2f prog=%s\n",
                              toon_state.indoor_temp, toon_state.setpoint,
                              program_label());

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk[16];
    strftime(clk, sizeof(clk), "%H:%M", &tm);
    lv_label_set_text(lbl_clock, clk);
    char dt[64];
    strftime(dt, sizeof(dt), "%A %d %B", &tm);
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
    /* Setpoint visible at all times; "to" prefix only when the boiler is
     * actively heating toward it (see screen_home.c for the same idea). */
    if (toon_state.setpoint > 0) {
        if (toon_state.burner_on)
            lv_label_set_text_fmt(lbl_setpoint, "to %.1f°C", toon_state.setpoint);
        else
            lv_label_set_text_fmt(lbl_setpoint, "%.1f°C", toon_state.setpoint);
    } else {
        lv_label_set_text(lbl_setpoint, "");
    }

    lv_label_set_text(lbl_program, program_label());

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
                snprintf(buf, sizeof buf, "%s    %s    %s    Air: %s",
                         tvoc, co2, bar, aql);
            else
                snprintf(buf, sizeof buf, "%s    %s    %s", tvoc, co2, bar);
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

    /* Vent — fan icon spin tracks fan_rpm; label shows preset + pct +
       remaining ("High 100 %" or "Timer 25m 100 %"). Source intentionally
       omitted on the dim screen to keep it clean — full audit is on the
       home tile. */
    if (dim_vent_fan && dim_vent_lbl) {
        if (vent_state.connected) {
            /* memcpy snapshot — same defence as screen_home.c, see comment
             * there. fan_info is a non-volatile char[] written by another
             * thread; without the local copy refresh_cb can read a stale
             * (or partial) view of the chars. */
            char fi_local[16];
            memcpy(fi_local, (const char *)vent_state.fan_info,
                   sizeof(fi_local));
            fi_local[sizeof(fi_local) - 1] = 0;
            const char * preset = fi_local[0] ? fi_local : "?";
            char pretty[24] = {0};
            snprintf(pretty, sizeof(pretty), "%c%s",
                     (preset[0] >= 'a' && preset[0] <= 'z')
                         ? preset[0] - 'a' + 'A' : preset[0],
                     preset + 1);
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
                                ? settings.weather_location : "Forecast";
            lv_label_set_text_fmt(dim_lbl_city, "%s  -  %.1f°C now",
                                  city, weather_state.current_temp);
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

    int use_hourly = settings.show_dim_weather && weather_state.hour_count > 0;
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
                                          h->temperature, h->wind_dir, h->wind_bft);
                else
                    lv_label_set_text_fmt(dim_fc_temp[i], "%.0f°C",
                                          h->temperature);
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
            lv_label_set_text_fmt(lbl_outside_temp, "%.1f C", weather_state.current_temp);
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
            /* Show actual next, no 3-day lead window, so the icon stays. */
            if (waste_next_2_pickups(&wp, &dummy) >= 1) {
                const char * l_clean = wp.labels;
                if (strncmp(l_clean, "Afval ", 6) == 0) l_clean += 6;
                lv_img_set_src(waste_icon, &icon_trash_lg);
                lv_obj_set_style_img_recolor(waste_icon,
                    lv_color_hex(waste_accent_for_label(l_clean)), 0);
                lv_obj_set_style_img_recolor_opa(waste_icon, 255, 0);

                long days_until = waste_days_until(wp.date);
                const char * when = (days_until == 0) ? "Vandaag" : (days_until == 1) ? "Morgen" : NULL;
                if (when) lv_label_set_text_fmt(lbl_waste, "%s: %s", when, l_clean);
                else {
                    static const char * const nl_month[12] = {
                        "januari", "februari", "maart", "april", "mei", "juni",
                        "juli", "augustus", "september", "oktober", "november", "december"};
                    int mo = atoi(wp.date + 5), dy = atoi(wp.date + 8);
                    const char * mn = (mo >= 1 && mo <= 12) ? nl_month[mo - 1] : "";
                    lv_label_set_text_fmt(lbl_waste, "%d %s: %s", dy, mn, l_clean);
                }
            } else {
                lv_img_set_src(waste_icon, &icon_trash_lg);
                lv_obj_set_style_img_recolor_opa(waste_icon, 100, 0); /* dimmed */
                lv_label_set_text(lbl_waste, "Geen");
            }
            lv_obj_clear_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_waste,  LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_waste,  LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Usage bars: energy now (W) + gas trailing-hour (m³/h), gated on
     * show_dim_bars. Side assignment honours dim_bars_swap; default (swap off)
     * = gas LEFT, energy RIGHT. Each bar reads from its independently-
     * configured source (ENERGY_SRC_* in settings). */
    if (bar_l_env) {
        /* Electricity bar — per-source dispatch, with production awareness.
         * When the net is exporting (production > consumption), the bar turns
         * green and shows the export power. Otherwise white for consumption. */
        int   e_conn; float e, eprod = 0;
        uint32_t ecolor = 0xffffff;
        switch (settings.energy_elec_source) {
        case ENERGY_SRC_ZWAVE:    e_conn = meter_state.connected; e = meter_state.power_w; break;
        case ENERGY_SRC_HW_P1:    e_conn = hw_state.connected_p1; e = hw_state.power_w; break;
        case ENERGY_SRC_HA:       e_conn = ha_energy.connected;   e = ha_energy.power_w;
                                  eprod  = ha_energy.power_prod_w; break;
        case ENERGY_SRC_DOMOTICZ: e_conn = dz_energy.connected;   e = dz_energy.power_w;
                                  eprod  = dz_energy.power_prod_w; break;
        default:                  e_conn = 0; e = 0; break;
        }
        /* P1 reports signed power (negative = export). HA/Domoticz have
         * separate prod sensors. Normalise: net negative → green bar. */
        float net = e - eprod;
        if (net < 0) { net = -net; ecolor = 0x44dd66; }
        float er = net / DIM_E_FULL_W;
        char etxt[24];
        if (net >= 1000) snprintf(etxt, sizeof etxt, "%.1f kW", net / 1000.0f);
        else             snprintf(etxt, sizeof etxt, "%.0f W", net);

        /* Gas bar — per-source dispatch for hourly m³/h */
        int   g_conn;
        float g;
        switch (settings.energy_gas_source) {
        case ENERGY_SRC_ZWAVE:    g_conn = meter_state.gas_connected; g = meter_state.gas_hour_m3; break;
        case ENERGY_SRC_HW_P1:    g_conn = hw_state.connected_p1;     g = hw_state.gas_hour_m3; break;
        case ENERGY_SRC_HA:       g_conn = ha_energy.connected;       g = ha_energy.gas_hour_m3; break;
        case ENERGY_SRC_DOMOTICZ: g_conn = dz_energy.connected;       g = dz_energy.gas_hour_m3; break;
        default:                  g_conn = 0; g = 0; break;
        }
        if (g < 0) g = 0;
        float gr = g / DIM_G_FULL_M3H;
        char gtxt[24];
        snprintf(gtxt, sizeof gtxt, "%.2f m3/h", g);

        int show = settings.show_dim_bars;
        if (!settings.dim_bars_swap) {
            dim_bar_set(bar_l_env, bar_l_fill, bar_l_cap, -1, show && g_conn, 0, gr, 0xffaa33, gtxt);
            dim_bar_set(bar_r_env, bar_r_fill, bar_r_cap, +1, show && e_conn, 0, er, ecolor, etxt);
        } else {
            dim_bar_set(bar_l_env, bar_l_fill, bar_l_cap, -1, show && e_conn, 0, er, ecolor, etxt);
            dim_bar_set(bar_r_env, bar_r_fill, bar_r_cap, +1, show && g_conn, 0, gr, 0xffaa33, gtxt);
        }
    }

    lv_obj_invalidate(scr_root);
}

lv_obj_t * screen_dim_create(void) {
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

    /* Clock — custom 96pt Montserrat (digits + ':' + space only,
       generated via lv_font_conv into lv_font_montserrat_96_custom.c). */
    lbl_clock = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_96_custom, 0);
    lv_label_set_text(lbl_clock, "--:--");
    lv_obj_align(lbl_clock, LV_ALIGN_CENTER, 0, SY(-130));

    /* Usage bars at the outer edges, centred on the temp row. Created always;
     * shown/hidden per settings.show_dim_bars in refresh_cb. */
    dim_bar_h = SY(2 * DIM_CLOCK_H);
    dim_make_bar(-1, &bar_l_env, &bar_l_fill, &bar_l_cap);
    dim_make_bar(+1, &bar_r_env, &bar_r_fill, &bar_r_cap);

    /* All labels positioned against screen center with explicit Y offsets so
       different content widths can't drift them out of alignment. */
    lbl_date = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_date, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_date, "");
    lv_obj_align(lbl_date, LV_ALIGN_CENTER, 0, SY(-50));

    /* Moon phase — paired with the weather icon to form one tidy "sky" cluster
     * in the top-right, instead of floating orphaned in the gap between the
     * clock and the corner. Sits just left of the weather icon (TOP_RIGHT,
     * -60,50) on the same baseline. Keep the 80-px size identical at create
     * *and* refresh — mismatched sizes make LVGL redraw at a recomputed
     * position that ends up off-screen. */
    dim_moon_img = lv_img_create(scr_root);
    lv_img_set_src(dim_moon_img, moon_phase_icon(80));
    lv_obj_set_style_img_recolor(dim_moon_img, lv_color_hex(0xe8edf2), 0);
    lv_obj_set_style_img_recolor_opa(dim_moon_img, 255, 0);
    lv_obj_align(dim_moon_img, LV_ALIGN_TOP_RIGHT, SX(-50), SY(8));

    lbl_temp = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_temp, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_temp, &lv_font_montserrat_48, 0);
    lv_label_set_text(lbl_temp, "-- C");
    lv_obj_align(lbl_temp, LV_ALIGN_CENTER, 0, SY(45));

    lbl_setpoint = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_setpoint, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_setpoint, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_setpoint, "to -- C");
    lv_obj_align(lbl_setpoint, LV_ALIGN_CENTER, 0, SY(115));

    /* Active program — sits directly under the setpoint and above the
       air-quality / pressure metrics strip. Same vertical-ordering as the
       home thermostat tile so the eye-tracking matches. */
    lbl_program = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_program, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_program, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_program, "--");
    lv_obj_align(lbl_program, LV_ALIGN_CENTER, 0, SY(140));

    /* Air-quality + CH-pressure strip — moved below program so the TVOC /
       ppm / bar / AQ block doesn't shove the manual label off the layout.
       At +170 it ends around y=492, leaving room for dim_lbl_city at y=498
       and the forecast strip at y=518 without anything overlapping. */
    lbl_metrics = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_metrics, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_metrics, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_metrics, "");
    lv_obj_align(lbl_metrics, LV_ALIGN_CENTER, 0, SY(162));

    /* Burner state — sits to the right of lbl_program on the same baseline.
       CH-heating shows "-> 90 C" (red). DHW shows the faucet+drop pair
       slightly to the left of where the text would be. Idle hides both. */
    lbl_burner = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_burner, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_burner, "");
    lv_obj_align(lbl_burner, LV_ALIGN_CENTER, SX(80), SY(140));
    lv_obj_add_flag(lbl_burner, LV_OBJ_FLAG_HIDDEN);

    /* Toon-style radiator+flame glyph, parked to the right of the big indoor
     * temp. lbl_temp is at CENTER (0, 45) with the 96-pt font, so its right
     * edge sits ~95 px right of centre; the icon (32 wide) at +135 leaves
     * a clean gap. y matches the temp baseline. */
    dim_img_flame = lv_img_create(scr_root);
    lv_img_set_src(dim_img_flame, &icon_radiator);
    lv_img_set_zoom(dim_img_flame, 256);
    lv_obj_set_style_img_recolor(dim_img_flame, lv_color_hex(0xff8866), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_flame, 255, 0);
    lv_obj_align(dim_img_flame, LV_ALIGN_CENTER, SX(145), SY(45));
    lv_obj_add_flag(dim_img_flame, LV_OBJ_FLAG_HIDDEN);

    /* DHW faucet fully red — see screen_home.c for rationale. */
    dim_img_faucet = lv_img_create(scr_root);
    lv_img_set_src(dim_img_faucet, &icon_faucet);
    lv_img_set_zoom(dim_img_faucet, 256);
    lv_obj_set_style_img_recolor(dim_img_faucet, lv_color_hex(0xff5544), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_faucet, 255, 0);
    lv_obj_align(dim_img_faucet, LV_ALIGN_CENTER, SX(140), SY(35));
    lv_obj_add_flag(dim_img_faucet, LV_OBJ_FLAG_HIDDEN);

    /* DHW drop in RED to distinguish from the blue cold-water-flow drop —
     * see comment in screen_home.c next to tile_img_drop. */
    dim_img_drop = lv_img_create(scr_root);
    lv_img_set_src(dim_img_drop, &icon_drop);
    lv_img_set_zoom(dim_img_drop, 256);
    lv_obj_set_style_img_recolor(dim_img_drop, lv_color_hex(0xff5544), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_drop, 255, 0);
    lv_obj_align(dim_img_drop, LV_ALIGN_CENTER, SX(158), SY(55));
    lv_obj_add_flag(dim_img_drop, LV_OBJ_FLAG_HIDDEN);

    /* Live water flow — drop icon + "X.X L/m" right of the indoor temp,
     * sits below the radiator slot so both can be visible at once. */
    dim_img_water = lv_img_create(scr_root);
    lv_img_set_src(dim_img_water, &icon_drop);
    lv_img_set_zoom(dim_img_water, 256);
    lv_obj_set_style_img_recolor(dim_img_water, lv_color_hex(0x66bbff), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_water, 255, 0);
    lv_obj_align(dim_img_water, LV_ALIGN_CENTER, SX(130), SY(80));
    lv_obj_add_flag(dim_img_water, LV_OBJ_FLAG_HIDDEN);

    dim_lbl_water = lv_label_create(scr_root);
    lv_obj_set_style_text_color(dim_lbl_water, lv_color_hex(0x66bbff), 0);
    lv_obj_set_style_text_font(dim_lbl_water, &lv_font_montserrat_22, 0);
    lv_label_set_text(dim_lbl_water, "");
    lv_obj_align(dim_lbl_water, LV_ALIGN_CENTER, SX(175), SY(80));
    lv_obj_add_flag(dim_lbl_water, LV_OBJ_FLAG_HIDDEN);

    /* Vent — small fan icon top-RIGHT (mirrors the waste icon on top-LEFT)
       with the actual ExhFanSpeed % below. Spin animation tracks % so
       the user can read at-a-glance whether the unit is idling or
       blasting. Hidden when the Itho bridge isn't reachable. */
    dim_vent_fan = lv_img_create(scr_root);
    lv_img_set_src(dim_vent_fan, &icon_fan);
    /* icon_fan is large (~96px); scale down for the dim chrome row. */
    lv_img_set_zoom(dim_vent_fan, 128);
    lv_obj_set_style_img_recolor(dim_vent_fan, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_img_recolor_opa(dim_vent_fan, 255, 0);
    /* Set pivot to center so rotation looks natural — has to be set
       AFTER the source is bound. icon_fan is 80×80 native (not 128 like
       the original comment claimed — wrong pivot made the icon orbit a
       point 24px off-centre, which is what "spinner acting weird" was). */
    lv_img_set_pivot(dim_vent_fan, 40, 40);
    /* Mirror the radiator+flame at (+145, +45): vent indicator sits to the
     * LEFT of the big indoor temp at the same offset. Label tucks under it
     * so the preset/% stays glanceable without crowding the temp row. */
    lv_obj_align(dim_vent_fan, LV_ALIGN_CENTER, SX(-145), SY(45));
    lv_obj_add_flag(dim_vent_fan, LV_OBJ_FLAG_HIDDEN);

    dim_vent_lbl = lv_label_create(scr_root);
    lv_obj_set_style_text_color(dim_vent_lbl, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(dim_vent_lbl, &lv_font_montserrat_18, 0);
    lv_label_set_text(dim_vent_lbl, "-- %");
    lv_obj_set_style_text_align(dim_vent_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(dim_vent_lbl, SX(120));
    lv_obj_align(dim_vent_lbl, LV_ALIGN_CENTER, SX(-145), SY(85));
    lv_obj_add_flag(dim_vent_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Weather cluster (right column): fixed top/width coordinates instead of
       RIGHT_MID offsets, so the icon and labels stay as one visual block even
       when text widths change. */
    lbl_outside_temp = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_outside_temp, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_outside_temp, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_outside_temp, "");
    lv_obj_set_width(lbl_outside_temp, SX(180));
    lv_obj_set_style_text_align(lbl_outside_temp, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(lbl_outside_temp, LV_ALIGN_TOP_RIGHT, SX(-48), SY(125));

    wx_icon = lv_img_create(scr_root);
    lv_img_set_src(wx_icon, &icon_wx_cloud_lg);
    lv_obj_set_style_img_recolor(wx_icon, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_img_recolor_opa(wx_icon, 255, 0);
    lv_obj_align(wx_icon, LV_ALIGN_TOP_RIGHT, SX(-116), SY(127));

    lbl_outside = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_outside, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_outside, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_outside, "Buiten");
    lv_obj_set_width(lbl_outside, SX(210));
    lv_obj_set_style_text_align(lbl_outside, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_outside, LV_ALIGN_TOP_RIGHT, SX(-56), SY(237));

    /* Life360 — sits under the outside temp on the right edge, mirroring
     * the Family tile on the home screen. Right-aligned so longer street
     * names extend to the LEFT instead of clipping the bezel. */
    dim_lbl_life360_a = lv_label_create(scr_root);
    lv_obj_set_style_text_color(dim_lbl_life360_a, lv_color_hex(0x88aaff), 0);
    lv_obj_set_style_text_font(dim_lbl_life360_a, &lv_font_montserrat_18, 0);
    lv_obj_set_width(dim_lbl_life360_a, SX(340));
    lv_obj_set_style_text_align(dim_lbl_life360_a, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(dim_lbl_life360_a, LV_LABEL_LONG_DOT);
    lv_label_set_text(dim_lbl_life360_a, "");
    lv_obj_align(dim_lbl_life360_a, LV_ALIGN_TOP_RIGHT, SX(-24), SY(263));
    lv_obj_add_flag(dim_lbl_life360_a, LV_OBJ_FLAG_HIDDEN);

    dim_lbl_life360_b = lv_label_create(scr_root);
    lv_obj_set_style_text_color(dim_lbl_life360_b, lv_color_hex(0xff88cc), 0);
    lv_obj_set_style_text_font(dim_lbl_life360_b, &lv_font_montserrat_18, 0);
    lv_obj_set_width(dim_lbl_life360_b, SX(340));
    lv_obj_set_style_text_align(dim_lbl_life360_b, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(dim_lbl_life360_b, LV_LABEL_LONG_DOT);
    lv_label_set_text(dim_lbl_life360_b, "");
    lv_obj_align(dim_lbl_life360_b, LV_ALIGN_TOP_RIGHT, SX(-24), SY(287));
    lv_obj_add_flag(dim_lbl_life360_b, LV_OBJ_FLAG_HIDDEN);

    /* Waste cluster (left column): mirror the weather cluster. Use the native
       80 px trash glyph and recolor it by waste type; no lv_img zoom here
       because zoomed alpha-only icons can disappear on Toon 1. */
    waste_icon = lv_img_create(scr_root);
    lv_img_set_src(waste_icon, &icon_trash_lg);
    lv_obj_set_style_img_recolor(waste_icon, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_img_recolor_opa(waste_icon, 255, 0);
    lv_obj_align(waste_icon, LV_ALIGN_TOP_LEFT, SX(100), SY(127));

    lbl_waste = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_waste, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_waste, &lv_font_montserrat_22, 0);
    lv_obj_set_width(lbl_waste, SX(260));
    lv_obj_set_style_text_align(lbl_waste, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_waste, LV_LABEL_LONG_WRAP);
    /* Centre the label box on the 80px (native, un-scaled) trash icon so the
       centred text sits directly under the icon on both targets. */
    lv_obj_align(lbl_waste, LV_ALIGN_TOP_LEFT, SX(100) + 40 - SX(260) / 2, SY(237));

    /* (City header above forecast strip removed — location moved under wx icon) */
    dim_lbl_city = NULL;
    waste_box_ptr = NULL;  /* absolute layout doesn't use the box pointer */

    /* 5-day forecast strip across the bottom of dim. Black/white style:
       40×40 icon at top, day label below, temp range under that. */
    /* Strip sits below the city header (y=498..~516) with a tiny gap.
     * Total vertical budget: 600 − 518 = 82 px for icon(40) + 4 gap +
     * day(18) + 2 gap + temp(18) = 82, tight but fits cleanly. */
    /* SY() compresses the strip onto the shorter Toon 1 panel; the extra
       nudge clears the bottom edge for the temp row's 18-px font (unscaled). */
    int strip_y = SY(508) - (DISP_VER < 600 ? SY(34) : 0);
    int col_w   = DISP_HOR / WEATHER_FORECAST_DAYS;
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        int cx = i * col_w + col_w / 2;

        dim_fc_icon[i] = lv_img_create(scr_root);
        lv_img_set_src(dim_fc_icon[i], &icon_wx_cloud);
        lv_obj_set_style_img_recolor(dim_fc_icon[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_img_recolor_opa(dim_fc_icon[i], 255, 0);
        lv_obj_set_pos(dim_fc_icon[i], cx -SX(20), strip_y);
        lv_obj_add_flag(dim_fc_icon[i], LV_OBJ_FLAG_HIDDEN);

        dim_fc_day[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_color(dim_fc_day[i], lv_color_hex(0xbbbbbb), 0);
        lv_obj_set_style_text_font(dim_fc_day[i], &lv_font_montserrat_18, 0);
        lv_label_set_text(dim_fc_day[i], "");
        lv_obj_set_style_text_align(dim_fc_day[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(dim_fc_day[i], col_w);
        lv_obj_set_pos(dim_fc_day[i], i * col_w, strip_y + SY(44));
        lv_obj_add_flag(dim_fc_day[i], LV_OBJ_FLAG_HIDDEN);

        dim_fc_temp[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_color(dim_fc_temp[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(dim_fc_temp[i], &lv_font_montserrat_18, 0);
        lv_label_set_text(dim_fc_temp[i], "");
        lv_obj_set_style_text_align(dim_fc_temp[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(dim_fc_temp[i], col_w);
        lv_obj_set_pos(dim_fc_temp[i], i * col_w, strip_y + SY(64));
        lv_obj_add_flag(dim_fc_temp[i], LV_OBJ_FLAG_HIDDEN);
    }

    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}
