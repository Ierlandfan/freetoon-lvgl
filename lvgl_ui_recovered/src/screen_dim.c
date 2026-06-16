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
#include "homeassistant.h"
#include "packages.h"
#include "weather.h"
#include "wastecollection.h"
#include "ventilation.h"
#include "meteradapter.h"   /* meter_state — for the restored dim energy bar */
#include "layout.h"         /* customizable dim block layout (g_dim_blocks) */
#include "icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(lv_font_montserrat_96_custom);

static lv_obj_t * scr_root = NULL;
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
 * two labelled horizontal mini-bars; gated by the existing show_dim_bars toggle. */
static lv_obj_t * en_hdr;
static lv_obj_t * en_pwr_track, * en_pwr_fill, * en_pwr_lbl;
static lv_obj_t * en_gas_track, * en_gas_fill, * en_gas_lbl;
static int   en_track_w = 0;        /* mini-bar track width (px, set at create) */
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
    i18n_date_long(dt, sizeof(dt), &tm);   /* localised — strftime is C-locale (always English) */
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
            lv_label_set_text_fmt(lbl_setpoint, tr("naar %.1f°C", "to %.1f°C"), toon_state.setpoint);
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
                snprintf(buf, sizeof buf, tr("%s    %s    %s    Lucht: %s",
                                              "%s    %s    %s    Air: %s"),
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
                const char * l_clean = wp.labels;
                if (strncmp(l_clean, "Afval ", 6) == 0) l_clean += 6;
                /* Same day, two types ("Papier+Plastic") → one colour-coded
                   bin per type. Split on the first '+'. */
                char t1[40], t2[40] = "";
                snprintf(t1, sizeof t1, "%s", l_clean);
                char * plus = strchr(t1, '+');
                if (plus) {
                    *plus = 0;
                    const char * p2 = plus + 1; while (*p2 == ' ') p2++;
                    snprintf(t2, sizeof t2, "%s", p2);
                    size_t n = strlen(t1); while (n && t1[n-1] == ' ') t1[--n] = 0;
                }
                lv_img_set_src(waste_icon, &icon_trash_lg);
                lv_obj_set_style_img_recolor(waste_icon,
                    lv_color_hex(waste_accent_for_label(t1)), 0);
                lv_obj_set_style_img_recolor_opa(waste_icon, 255, 0);
                if (waste_icon_2) {
                    if (t2[0]) {
                        lv_obj_set_style_img_recolor(waste_icon_2,
                            lv_color_hex(waste_accent_for_label(t2)), 0);
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

    /* Energy block: power now (W, white) + gas trailing-hour (m³/h, amber),
     * each a value + fixed-full-scale mini-bar. Gas needs the P1; energy follows
     * settings.energy_source (Toon vs HWE). The whole block can be toggled off
     * via the existing show_dim_bars setting (or hidden in the dim editor). */
    if (en_pwr_lbl) {
        int   e_conn = (settings.energy_source == 0)
                         ? meter_state.connected
                         : (settings.enable_p1_elec && hw_state.connected_p1);
        float e = (settings.energy_source == 0) ? meter_state.power_w
                                                : hw_state.power_w;
        if (e < 0) e = 0;                          /* export → empty */
        char etxt[24];
        if (e >= 1000) snprintf(etxt, sizeof etxt, "%.1f kW", e / 1000.0f);
        else           snprintf(etxt, sizeof etxt, "%.0f W", e);

        int   g_conn = hw_state.connected_p1;
        float g = hw_state.gas_hour_m3; if (g < 0) g = 0;
        char gtxt[24];
        snprintf(gtxt, sizeof gtxt, "%.2f m3/h", g);

        en_set_bar(en_pwr_fill, en_pwr_lbl, en_track_w, e / DIM_E_FULL_W,
                   e_conn ? etxt : tr("-- W", "-- W"));
        en_set_bar(en_gas_fill, en_gas_lbl, en_track_w, g / DIM_G_FULL_M3H,
                   g_conn ? gtxt : tr("-- m3/h", "-- m3/h"));

        /* show_dim_bars off → hide the block's contents (block visibility in the
         * dim layout is handled separately by hiding the whole box at create). */
        int show = settings.show_dim_bars;
        lv_obj_t * en_parts[] = { en_hdr, en_pwr_track, en_gas_track, en_pwr_lbl, en_gas_lbl };
        for (unsigned i = 0; i < sizeof en_parts / sizeof en_parts[0]; i++)
            if (en_parts[i]) {
                if (show) lv_obj_clear_flag(en_parts[i], LV_OBJ_FLAG_HIDDEN);
                else      lv_obj_add_flag(en_parts[i], LV_OBJ_FLAG_HIDDEN);
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

    /* All labels positioned against screen center with explicit Y offsets so
       different content widths can't drift them out of alignment. */
    lbl_date = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_date, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_date, "");
    lv_obj_align(lbl_date, LV_ALIGN_CENTER, 0, SY(-50));

    /* ===== peripheral blocks — each rendered INSIDE its grid cell so the
     * "Dim indeling" editor is true WYSIWYG (content lands where the cell is).
     * Within a box, offsets are raw px (the box is already cell-sized). ===== */
    const int t1 = (DISP_VER < 600);

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

    dim_moon_img = lv_img_create(bx_w);
    lv_img_set_src(dim_moon_img, moon_phase_icon(80));
    lv_obj_set_style_img_recolor(dim_moon_img, lv_color_hex(0xe8edf2), 0);
    lv_obj_set_style_img_recolor_opa(dim_moon_img, 255, 0);
    lv_obj_align(dim_moon_img, LV_ALIGN_BOTTOM_LEFT, 6, -6);

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
    lv_obj_set_style_text_font(lbl_waste, &lv_font_montserrat_22, 0);
    lv_obj_set_width(lbl_waste, sbw - 8);
    lv_obj_set_style_text_align(lbl_waste, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_waste, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_waste, LV_ALIGN_TOP_MID, 0, 92);

    /* ---- THERMO: indoor temp + setpoint + program + metrics, centre-stacked ---- */
    int tbw = 0, tbh = 0; lv_obj_t * bx_t = dim_box(DB_THERMO, &tbw, &tbh); (void)tbh;
    lbl_temp = lv_label_create(bx_t);
    lv_obj_set_style_text_color(lbl_temp, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_temp, t1 ? &lv_font_montserrat_28 : &lv_font_montserrat_48, 0);
    lv_label_set_text(lbl_temp, "-- C");
    lv_obj_align(lbl_temp, LV_ALIGN_TOP_MID, 0, 2);

    lbl_setpoint = lv_label_create(bx_t);
    lv_obj_set_style_text_color(lbl_setpoint, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_setpoint, t1 ? &lv_font_montserrat_22 : &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_setpoint, tr("naar -- C", "to -- C"));
    lv_obj_align(lbl_setpoint, LV_ALIGN_TOP_MID, 0, t1 ? 42 : 58);

    lbl_program = lv_label_create(bx_t);
    lv_obj_set_style_text_color(lbl_program, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_program, t1 ? &lv_font_montserrat_18 : &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_program, "--");
    lv_obj_align(lbl_program, LV_ALIGN_TOP_MID, 0, t1 ? 70 : 92);

    lbl_metrics = lv_label_create(bx_t);
    lv_obj_set_style_text_color(lbl_metrics, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_metrics, &lv_font_montserrat_18, 0);
    lv_obj_set_width(lbl_metrics, tbw - 8);
    lv_obj_set_style_text_align(lbl_metrics, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_metrics, LV_LABEL_LONG_CLIP);
    lv_label_set_text(lbl_metrics, "");
    lv_obj_align(lbl_metrics, LV_ALIGN_TOP_MID, 0, t1 ? 94 : 122);

    lbl_burner = lv_label_create(bx_t);
    lv_obj_set_style_text_font(lbl_burner, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_burner, "");
    lv_obj_align(lbl_burner, LV_ALIGN_TOP_MID, 130, t1 ? 70 : 92);
    lv_obj_add_flag(lbl_burner, LV_OBJ_FLAG_HIDDEN);

    dim_img_flame = lv_img_create(bx_t);
    lv_img_set_src(dim_img_flame, &icon_radiator);
    lv_img_set_zoom(dim_img_flame, 256);
    lv_obj_set_style_img_recolor(dim_img_flame, lv_color_hex(0xff8866), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_flame, 255, 0);
    lv_obj_align(dim_img_flame, LV_ALIGN_TOP_MID, 120, 6);
    lv_obj_add_flag(dim_img_flame, LV_OBJ_FLAG_HIDDEN);

    dim_img_faucet = lv_img_create(bx_t);
    lv_img_set_src(dim_img_faucet, &icon_faucet);
    lv_img_set_zoom(dim_img_faucet, 256);
    lv_obj_set_style_img_recolor(dim_img_faucet, lv_color_hex(0xff5544), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_faucet, 255, 0);
    lv_obj_align(dim_img_faucet, LV_ALIGN_TOP_MID, 120, 2);
    lv_obj_add_flag(dim_img_faucet, LV_OBJ_FLAG_HIDDEN);

    dim_img_drop = lv_img_create(bx_t);
    lv_img_set_src(dim_img_drop, &icon_drop);
    lv_img_set_zoom(dim_img_drop, 256);
    lv_obj_set_style_img_recolor(dim_img_drop, lv_color_hex(0xff5544), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_drop, 255, 0);
    lv_obj_align(dim_img_drop, LV_ALIGN_TOP_MID, 150, 18);
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
    lv_obj_align(dim_vent_lbl, LV_ALIGN_TOP_MID, 0, 52);
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
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        int cx = i * col_w + col_w / 2;

        dim_fc_icon[i] = lv_img_create(bx_c);
        lv_img_set_src(dim_fc_icon[i], &icon_wx_cloud);
        lv_obj_set_style_img_recolor(dim_fc_icon[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_img_recolor_opa(dim_fc_icon[i], 255, 0);
        lv_obj_set_pos(dim_fc_icon[i], cx - 20, 4);
        lv_obj_add_flag(dim_fc_icon[i], LV_OBJ_FLAG_HIDDEN);

        dim_fc_day[i] = lv_label_create(bx_c);
        lv_obj_set_style_text_color(dim_fc_day[i], lv_color_hex(0xbbbbbb), 0);
        lv_obj_set_style_text_font(dim_fc_day[i], &lv_font_montserrat_18, 0);
        lv_label_set_text(dim_fc_day[i], "");
        lv_obj_set_style_text_align(dim_fc_day[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(dim_fc_day[i], col_w);
        lv_obj_set_pos(dim_fc_day[i], i * col_w, 46);
        lv_obj_add_flag(dim_fc_day[i], LV_OBJ_FLAG_HIDDEN);

        dim_fc_temp[i] = lv_label_create(bx_c);
        lv_obj_set_style_text_color(dim_fc_temp[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(dim_fc_temp[i], &lv_font_montserrat_18, 0);
        lv_label_set_text(dim_fc_temp[i], "");
        lv_obj_set_style_text_align(dim_fc_temp[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(dim_fc_temp[i], col_w);
        lv_obj_set_pos(dim_fc_temp[i], i * col_w, 66);
        lv_obj_add_flag(dim_fc_temp[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- ENERGY: power + gas mini-bars in their own grid cell ---- */
    int ebw = 0, ebh = 0; lv_obj_t * bx_e = dim_box(DB_ENERGY, &ebw, &ebh); (void)ebh;
    en_track_w = ebw - 8;
    en_hdr = lv_label_create(bx_e);
    lv_obj_set_style_text_color(en_hdr, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(en_hdr, &lv_font_montserrat_18, 0);
    lv_label_set_text(en_hdr, tr("Energie", "Energy"));
    lv_obj_align(en_hdr, LV_ALIGN_TOP_LEFT, 4, 2);
    en_make_bar(bx_e, en_track_w, 28, 0xffffff, &en_pwr_track, &en_pwr_fill, &en_pwr_lbl);
    en_make_bar(bx_e, en_track_w, 66, 0xffaa33, &en_gas_track, &en_gas_fill, &en_gas_lbl);

    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}
