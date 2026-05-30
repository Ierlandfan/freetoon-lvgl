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
#include "homeassistant.h"
#include "meteradapter.h"
#include "packages.h"
#include "weather.h"
#include "wastecollection.h"
#include "ventilation.h"
#include "icons.h"
#include <stdio.h>
#include <stdlib.h>     /* abs() — emscripten/musl doesn't transitively expose it */
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(lv_font_montserrat_96_custom);
LV_FONT_DECLARE(lv_font_montserrat_64_custom);   /* big indoor-temp readout */

static lv_obj_t * scr_root = NULL;
static lv_obj_t * lbl_clock;
static lv_obj_t * lbl_date;
static lv_obj_t * dim_moon_img;   /* moon-phase widget — always shown */
static lv_obj_t * lbl_temp;
static lv_obj_t * lbl_setpoint;
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

/* ---- usage bars flanking the clock: energy now (W) + gas hourly (m³) ----
 * Vertical bars, fill grows up from the clock baseline, envelope height =
 * 2x clock height. Both auto-scale to the running max seen, so no fixed
 * full-scale to calibrate. Side + visibility come from settings in refresh_cb. */
#define DIM_BAR_W     44      /* bar width, design px (SX-scaled) */
#define DIM_BAR_INSET 20      /* gap from the screen bezel (outer edges) */
#define DIM_BAR_Y     45      /* vertical centre = the indoor-temp row */
#define DIM_CLOCK_H   96      /* clock font px; envelope = 2x this */
static lv_obj_t * bar_l_env, * bar_l_fill, * bar_l_cap;
static lv_obj_t * bar_r_env, * bar_r_fill, * bar_r_cap;
static int   dim_bar_h = 0;        /* envelope height (px, computed at create) */
#define DIM_E_FULL_W    5000.0f     /* power at full bar height (fixed scale) */
#define DIM_G_FULL_M3H  2.0f        /* gas (m³/h) at full bar height (fixed scale) */

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

/* Build one bar slot (side: -1 left edge, +1 right edge). Envelope is a dark
 * track at the screen edge, vertically centred on the indoor-temp row; the fill
 * is a child anchored to the bottom that grows upward. Caption is align_to'd to
 * the envelope so it stays centred under the bar regardless of width. Neither
 * is CLICKABLE, so a tap on the bar still falls through to the screen wake. */
static void dim_make_bar(int side, lv_obj_t ** env, lv_obj_t ** fill,
                         lv_obj_t ** cap) {
    int bw = SX(DIM_BAR_W);
    lv_align_t al = (side < 0) ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID;
    int xinset = (side < 0) ? SX(DIM_BAR_INSET) : -SX(DIM_BAR_INSET);

    *env = lv_obj_create(scr_root);
    lv_obj_remove_style_all(*env);
    lv_obj_set_size(*env, bw, dim_bar_h);
    /* Invisible container — only the coloured fill is drawn (no track box). */
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
    /* Anchor to the bar's edge so the value reads inward and never clips the
     * bezel (a centred caption under an edge bar runs off-screen). */
    lv_obj_align_to(*cap, *env,
                    (side < 0) ? LV_ALIGN_OUT_BOTTOM_LEFT : LV_ALIGN_OUT_BOTTOM_RIGHT,
                    0, SY(14));
}

/* Apply a value to a bar slot. ratio 0..1 of the envelope. Hidden when !show.
 * When text_only, the bar is dropped and just the value is shown, centred on
 * the bar's location (at the screen edge, temp-row height). */
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
        /* Bar gone: show just the value at the edge, on the temp row, reading
         * inward (to the right of a left bar / left of a right bar). */
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
        /* TVOC / eCO2 ppm / CH water pressure / air-quality badge on one
           greyed row. Missing inputs collapse to "--" so the strip layout
           stays stable. AQ label is appended only when we actually have
           air-quality data to classify. */
        char buf[200];
        char tvoc[24] = "TVOC --";
        char co2[24]  = "CO2 --";
        char bar[24]  = "CV --";
        if (toon_state.tvoc)
            snprintf(tvoc, sizeof tvoc, "TVOC %d ppb", toon_state.tvoc);
        if (toon_state.eco2)
            snprintf(co2,  sizeof co2,  "CO2 %d ppm", toon_state.eco2);
        if (toon_state.water_pressure > 0.1f)
            snprintf(bar,  sizeof bar,  "CV %.1f bar", toon_state.water_pressure);
        const char * aql = air_quality_label(toon_state.eco2, toon_state.tvoc);
        if (*aql)
            snprintf(buf, sizeof buf, "%s    %s    %s    Air: %s",
                     tvoc, co2, bar, aql);
        else
            snprintf(buf, sizeof buf, "%s    %s    %s", tvoc, co2, bar);
        lv_label_set_text(lbl_metrics, buf);
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
    if (lbl_outside) {
        if (settings.show_dim_weather && weather_state.connected) {
            lv_label_set_text_fmt(lbl_outside, "%.1f°C", weather_state.current_temp);
            lv_obj_clear_flag(lbl_outside, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_outside, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Waste: next upcoming pickup within the lead window — shares the home
       tile's windowed + per-type-cutoff logic (Plastic until 21:00, the rest
       until 16:00), so both screens agree on what's "next". */
    int show_waste = 0;
    char wtype[40] = "", waste_text[60] = "";
    if (settings.show_dim_waste && settings.dim_waste_lead_days > 0 &&
        waste_state.connected) {
        waste_pickup_t wp;
        if (waste_next_n_windowed(settings.dim_waste_lead_days, &wp, 1) >= 1) {
            snprintf(wtype, sizeof(wtype), "%s", wp.labels);
            long days_until = waste_days_until(wp.date);
            show_waste = 1;
            const char * when =
                  (days_until == 0) ? "Vandaag"
                : (days_until == 1) ? "Morgen"
                : NULL;
            if (when) snprintf(waste_text, sizeof(waste_text), "%s: %s", when, wtype);
            else      snprintf(waste_text, sizeof(waste_text), "Over %ld d: %s",
                               days_until, wtype);
        }
    }
    if (waste_icon) {
        if (show_waste) {
            /* Pick the per-type icon (newspaper for Papier, milk carton
             * for Plastic, leaf for GFT) instead of always rendering the
             * generic trashcan. Same helper home.c uses. */
            lv_img_set_src(waste_icon, waste_icon_for_label(wtype));
            lv_obj_set_style_img_recolor(waste_icon,
                lv_color_hex(waste_accent_for_label(wtype)), 0);
            lv_obj_set_style_img_recolor_opa(waste_icon, 255, 0);
            lv_obj_clear_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (lbl_waste) {
        if (show_waste) {
            lv_label_set_text(lbl_waste, waste_text);
            lv_obj_clear_flag(lbl_waste, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_waste, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Usage bars: energy now (W, white) + gas trailing-hour (m³, amber), each
     * auto-scaled to the running max. Side assignment honours dim_bars_swap;
     * default (swap off) = energy LEFT, gas RIGHT. Gas needs the P1; energy
     * follows settings.energy_source (Toon meter vs HomeWizard). */
    if (bar_l_env) {
        int   e_conn = (settings.energy_source == 0)
                         ? meter_state.connected
                         : (settings.enable_p1_elec && hw_state.connected_p1);
        float e = (settings.energy_source == 0) ? meter_state.power_w
                                                : hw_state.power_w;
        if (e < 0) e = 0;                          /* export → empty */
        float er = e / DIM_E_FULL_W;               /* fixed 5 kW full-scale */
        char etxt[24];
        if (e >= 1000) snprintf(etxt, sizeof etxt, "%.1f kW", e / 1000.0f);
        else           snprintf(etxt, sizeof etxt, "%.0f W", e);

        int   g_conn = hw_state.connected_p1;
        float g = hw_state.gas_hour_m3; if (g < 0) g = 0;
        float gr = g / DIM_G_FULL_M3H;             /* fixed 2 m³/h full-scale */
        char gtxt[24];
        snprintf(gtxt, sizeof gtxt, "%.2f m3/h", g);

        int show = settings.show_dim_bars;
        if (!settings.dim_bars_swap) {             /* default: gas LEFT, energy RIGHT */
            dim_bar_set(bar_l_env, bar_l_fill, bar_l_cap, -1, show && g_conn, 0, gr, 0xffaa33, gtxt);
            dim_bar_set(bar_r_env, bar_r_fill, bar_r_cap, +1, show && e_conn, 0, er, 0xffffff, etxt);
        } else {                                   /* swapped: energy LEFT, gas RIGHT */
            dim_bar_set(bar_l_env, bar_l_fill, bar_l_cap, -1, show && e_conn, 0, er, 0xffffff, etxt);
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

    /* freetoon "ft" badge — same top-left corner + styling as the home screen,
     * for visual continuity. Non-clickable on purpose: a tap anywhere on the
     * dim screen should wake it, so the badge must let the press fall through
     * to the screen-wide wake handler (no CLICKABLE flag). */
    {
        lv_obj_t * logo = lv_obj_create(scr_root);
        lv_obj_remove_style_all(logo);
        lv_obj_set_size(logo, 40, 40);
        lv_obj_align(logo, LV_ALIGN_TOP_LEFT, 8, SY(6));
        lv_obj_set_style_bg_color(logo, lv_color_hex(0x2e6e9e), 0);
        lv_obj_set_style_bg_opa(logo, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(logo, 12, 0);
        lv_obj_clear_flag(logo, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t * logo_lbl = lv_label_create(logo);
        lv_label_set_text(logo_lbl, "ft");
        lv_obj_set_style_text_color(logo_lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(logo_lbl, SF(22), 0);
        lv_obj_center(logo_lbl);
    }

    /* Clock — custom 96pt Montserrat (digits + ':' + space only,
       generated via lv_font_conv into lv_font_montserrat_96_custom.c). */
    lbl_clock = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_clock, SF(96), 0);
    lv_label_set_text(lbl_clock, "--:--");
    /* Nudged down from -130 so there's more headroom between the clock and the
     * "ft" logo / top chrome. */
    lv_obj_align(lbl_clock, LV_ALIGN_CENTER, 0, SY(-116));

    /* Usage bars at the outer edges, centred on the indoor-temp row (gas left,
     * energy right by default). Values + side handled in refresh_cb. */
    dim_bar_h = SY(2 * DIM_CLOCK_H);
    dim_make_bar(-1, &bar_l_env, &bar_l_fill, &bar_l_cap);
    dim_make_bar(+1, &bar_r_env, &bar_r_fill, &bar_r_cap);

    /* All labels positioned against screen center with explicit Y offsets so
       different content widths can't drift them out of alignment. */
    lbl_date = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_date, SF(22), 0);
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
    lv_obj_align(dim_moon_img, LV_ALIGN_TOP_RIGHT, -150, SY(52));

    lbl_temp = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_temp, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_temp, SF(64), 0);
    lv_label_set_text(lbl_temp, "-- C");
    lv_obj_align(lbl_temp, LV_ALIGN_CENTER, 0, SY(45));

    lbl_setpoint = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_setpoint, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_setpoint, SF(28), 0);
    lv_label_set_text(lbl_setpoint, "to -- C");
    lv_obj_align(lbl_setpoint, LV_ALIGN_CENTER, 0, SY(115));

    /* Active program — sits directly under the setpoint and above the
       air-quality / pressure metrics strip. Same vertical-ordering as the
       home thermostat tile so the eye-tracking matches. */
    lbl_program = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_program, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_program, SF(22), 0);
    lv_label_set_text(lbl_program, "--");
    lv_obj_align(lbl_program, LV_ALIGN_CENTER, 0, SY(140));

    /* Air-quality + CH-pressure strip — moved below program so the TVOC /
       ppm / bar / AQ block doesn't shove the manual label off the layout.
       At +170 it ends around y=492, leaving room for dim_lbl_city at y=498
       and the forecast strip at y=518 without anything overlapping. */
    lbl_metrics = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_metrics, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_metrics, SF(22), 0);
    lv_label_set_text(lbl_metrics, "");
    lv_obj_align(lbl_metrics, LV_ALIGN_CENTER, 0, SY(162));

    /* Burner state — sits to the right of lbl_program on the same baseline.
       CH-heating shows "-> 90 C" (red). DHW shows the faucet+drop pair
       slightly to the left of where the text would be. Idle hides both. */
    lbl_burner = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_burner, SF(22), 0);
    lv_label_set_text(lbl_burner, "");
    lv_obj_align(lbl_burner, LV_ALIGN_CENTER, 80, SY(140));
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
    lv_obj_align(dim_img_flame, LV_ALIGN_CENTER, 145, SY(45));
    lv_obj_add_flag(dim_img_flame, LV_OBJ_FLAG_HIDDEN);

    /* DHW faucet fully red — see screen_home.c for rationale. */
    dim_img_faucet = lv_img_create(scr_root);
    lv_img_set_src(dim_img_faucet, &icon_faucet);
    lv_img_set_zoom(dim_img_faucet, 256);
    lv_obj_set_style_img_recolor(dim_img_faucet, lv_color_hex(0xff5544), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_faucet, 255, 0);
    lv_obj_align(dim_img_faucet, LV_ALIGN_CENTER, 140, SY(35));
    lv_obj_add_flag(dim_img_faucet, LV_OBJ_FLAG_HIDDEN);

    /* DHW drop in RED to distinguish from the blue cold-water-flow drop —
     * see comment in screen_home.c next to tile_img_drop. */
    dim_img_drop = lv_img_create(scr_root);
    lv_img_set_src(dim_img_drop, &icon_drop);
    lv_img_set_zoom(dim_img_drop, 256);
    lv_obj_set_style_img_recolor(dim_img_drop, lv_color_hex(0xff5544), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_drop, 255, 0);
    lv_obj_align(dim_img_drop, LV_ALIGN_CENTER, 158, SY(55));
    lv_obj_add_flag(dim_img_drop, LV_OBJ_FLAG_HIDDEN);

    /* Live water flow — drop icon + "X.X L/m" right of the indoor temp,
     * sits below the radiator slot so both can be visible at once. */
    dim_img_water = lv_img_create(scr_root);
    lv_img_set_src(dim_img_water, &icon_drop);
    lv_img_set_zoom(dim_img_water, 256);
    lv_obj_set_style_img_recolor(dim_img_water, lv_color_hex(0x66bbff), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_water, 255, 0);
    lv_obj_align(dim_img_water, LV_ALIGN_CENTER, 130, SY(80));
    lv_obj_add_flag(dim_img_water, LV_OBJ_FLAG_HIDDEN);

    dim_lbl_water = lv_label_create(scr_root);
    lv_obj_set_style_text_color(dim_lbl_water, lv_color_hex(0x66bbff), 0);
    lv_obj_set_style_text_font(dim_lbl_water, SF(22), 0);
    lv_label_set_text(dim_lbl_water, "");
    lv_obj_align(dim_lbl_water, LV_ALIGN_CENTER, 175, SY(80));
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
    lv_obj_align(dim_vent_fan, LV_ALIGN_CENTER, -145, SY(45));
    lv_obj_add_flag(dim_vent_fan, LV_OBJ_FLAG_HIDDEN);

    dim_vent_lbl = lv_label_create(scr_root);
    lv_obj_set_style_text_color(dim_vent_lbl, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(dim_vent_lbl, SF(18), 0);
    lv_label_set_text(dim_vent_lbl, "-- %");
    lv_obj_set_style_text_align(dim_vent_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(dim_vent_lbl, 120);
    lv_obj_align(dim_vent_lbl, LV_ALIGN_CENTER, -145, SY(85));
    lv_obj_add_flag(dim_vent_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Weather icon (large, top-right) + outside temp underneath.
       Visibility is gated by settings.show_dim_weather in refresh_cb. */
    wx_icon = lv_img_create(scr_root);
    lv_img_set_src(wx_icon, &icon_wx_cloud_lg);
    lv_obj_set_style_img_recolor(wx_icon, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_img_recolor_opa(wx_icon, 255, 0);
    /* Native 80×80 — bigger source bitmap, no transform. */
    lv_obj_align(wx_icon, LV_ALIGN_TOP_RIGHT, -60, SY(50));

    lbl_outside = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_outside, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_outside, SF(22), 0);
    lv_label_set_text(lbl_outside, "-- C");
    lv_obj_align(lbl_outside, LV_ALIGN_TOP_RIGHT, -30, SY(140));

    /* Life360 — sits under the outside temp on the right edge, mirroring
     * the Family tile on the home screen. Right-aligned so longer street
     * names extend to the LEFT instead of clipping the bezel. */
    dim_lbl_life360_a = lv_label_create(scr_root);
    lv_obj_set_style_text_color(dim_lbl_life360_a, lv_color_hex(0x88aaff), 0);
    lv_obj_set_style_text_font(dim_lbl_life360_a, SF(18), 0);
    lv_obj_set_width(dim_lbl_life360_a, 340);
    lv_obj_set_style_text_align(dim_lbl_life360_a, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(dim_lbl_life360_a, LV_LABEL_LONG_DOT);
    lv_label_set_text(dim_lbl_life360_a, "");
    lv_obj_align(dim_lbl_life360_a, LV_ALIGN_TOP_RIGHT, -30, SY(174));
    lv_obj_add_flag(dim_lbl_life360_a, LV_OBJ_FLAG_HIDDEN);

    dim_lbl_life360_b = lv_label_create(scr_root);
    lv_obj_set_style_text_color(dim_lbl_life360_b, lv_color_hex(0xff88cc), 0);
    lv_obj_set_style_text_font(dim_lbl_life360_b, SF(18), 0);
    lv_obj_set_width(dim_lbl_life360_b, 340);
    lv_obj_set_style_text_align(dim_lbl_life360_b, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(dim_lbl_life360_b, LV_LABEL_LONG_DOT);
    lv_label_set_text(dim_lbl_life360_b, "");
    lv_obj_align(dim_lbl_life360_b, LV_ALIGN_TOP_RIGHT, -30, SY(198));
    lv_obj_add_flag(dim_lbl_life360_b, LV_OBJ_FLAG_HIDDEN);

    /* Waste — 80×80 trash icon top-LEFT (mirroring the weather block).
       Visibility + label gated by settings + lead-days window. */
    waste_icon = lv_img_create(scr_root);
    lv_img_set_src(waste_icon, &icon_trash_lg);
    lv_obj_set_style_img_recolor(waste_icon, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_img_recolor_opa(waste_icon, 255, 0);
    lv_obj_align(waste_icon, LV_ALIGN_TOP_LEFT, 60, SY(50));
    lv_obj_add_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);

    lbl_waste = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_waste, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_waste, SF(22), 0);
    /* Cap the width and word-wrap so a long pickup string ("vrijdag: GFT en
     * etensresten") stacks onto a 2nd line in the left column instead of
     * running rightward into the centered clock. */
    lv_obj_set_width(lbl_waste, 300);
    lv_label_set_long_mode(lbl_waste, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl_waste, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_waste, "");
    lv_obj_align(lbl_waste, LV_ALIGN_TOP_LEFT, 20, SY(140));
    lv_obj_add_flag(lbl_waste, LV_OBJ_FLAG_HIDDEN);

    /* City header above the forecast strip — same content as the home
     * tile's "Medemblik - 14.5 C now" line. Placed above the strip in
     * the small gap left between the metrics row and the icons. */
    dim_lbl_city = lv_label_create(scr_root);
    lv_obj_set_style_text_color(dim_lbl_city, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(dim_lbl_city, SF(18), 0);
    lv_obj_set_width(dim_lbl_city, DISP_HOR);
    lv_obj_set_style_text_align(dim_lbl_city, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(dim_lbl_city, "");
    /* y=498 so it sits below the metrics row (CENTER + 170 ≈ 470..492)
     * with a small gap, and still above the forecast strip at y=518. */
    lv_obj_set_pos(dim_lbl_city, 0, SY(490));

    /* 5-day forecast strip across the bottom of dim. Black/white style:
       40×40 icon at top, day label below, temp range under that. */
    /* Strip sits below the city header (y=498..~516) with a tiny gap.
     * Total vertical budget: 600 − 518 = 82 px for icon(40) + 4 gap +
     * day(18) + 2 gap + temp(18) = 82, tight but fits cleanly. */
    /* SY() compresses the strip onto the shorter Toon 1 panel; the extra
       nudge clears the bottom edge for the temp row's 18-px font (unscaled). */
    int strip_y = SY(508) - (DISP_VER < 600 ? 12 : 0);
    int col_w   = DISP_HOR / WEATHER_FORECAST_DAYS;
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        int cx = i * col_w + col_w / 2;

        dim_fc_icon[i] = lv_img_create(scr_root);
        lv_img_set_src(dim_fc_icon[i], &icon_wx_cloud);
        lv_obj_set_style_img_recolor(dim_fc_icon[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_img_recolor_opa(dim_fc_icon[i], 255, 0);
        lv_obj_set_pos(dim_fc_icon[i], cx - 20, strip_y);
        lv_obj_add_flag(dim_fc_icon[i], LV_OBJ_FLAG_HIDDEN);

        dim_fc_day[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_color(dim_fc_day[i], lv_color_hex(0xbbbbbb), 0);
        lv_obj_set_style_text_font(dim_fc_day[i], SF(18), 0);
        lv_label_set_text(dim_fc_day[i], "");
        lv_obj_set_style_text_align(dim_fc_day[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(dim_fc_day[i], col_w);
        lv_obj_set_pos(dim_fc_day[i], i * col_w, strip_y + SY(44));
        lv_obj_add_flag(dim_fc_day[i], LV_OBJ_FLAG_HIDDEN);

        dim_fc_temp[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_color(dim_fc_temp[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(dim_fc_temp[i], SF(18), 0);
        lv_label_set_text(dim_fc_temp[i], "");
        lv_obj_set_style_text_align(dim_fc_temp[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(dim_fc_temp[i], col_w);
        lv_obj_set_pos(dim_fc_temp[i], i * col_w, strip_y + SY(64));
        lv_obj_add_flag(dim_fc_temp[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* "tap to wake" hint at top-mid. Pushed down a bit (was SY(6)) so the
       WASM client's "master: connected" status div (HTML overlay at top:4px)
       sits cleanly above it instead of overlapping. */
    lv_obj_t * hint = lv_label_create(scr_root);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(hint, SF(18), 0);
    lv_label_set_text(hint, "tap to wake");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, SY(24));

    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}
