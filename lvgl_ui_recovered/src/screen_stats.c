/*
 * Statistics screen V2.
 * - Three metric tabs: Electricity / Gas / Water
 * - Five period tabs:  Hour / Day / Week / Month / Year
 * - lv_chart of values from the matching hcb_rrd archive
 *
 * Live (flow) metrics use the 5-min archive for short windows. Cumulative
 * meters use 5yrhours for week/month and 10yrdays for year — values are
 * diffed between adjacent samples to show usage-per-period rather than
 * the raw cumulative.
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "stats.h"
#include "airhist.h"
#include "homewizard.h"
#include "energy.h"
#include "settings.h"
#include "theme.h"
#include "boxtalk.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static lv_obj_t * scr_root = NULL;
static lv_obj_t * chart;
static lv_chart_series_t * cs;
static lv_obj_t * lbl_metric_name;
static lv_obj_t * lbl_unit;
static lv_obj_t * lbl_value;
static lv_obj_t * tab_metric_btns[6];
#define N_PERIOD 6
static lv_obj_t * tab_period_btns[N_PERIOD];

/* Time navigation widgets */
static lv_obj_t * btn_nav_prev = NULL;
static lv_obj_t * btn_nav_next = NULL;
static lv_obj_t * lbl_nav_range = NULL;
static int        g_time_offset = 0;     /* 0 = present, -1 = 1 window back, etc. */
static long       g_current_start_t = 0;
static long       g_current_end_t = 0;

typedef enum {
    PERIOD_NOW,     /* 12 x 5-minute intervals (past 1 hour) */
    PERIOD_HOURS,   /* 24 hours (24 hourly bars) */
    PERIOD_DAYS,    /* 31 days (31 daily bars) */
    PERIOD_WEEKS,   /* 12 weeks (12 weekly bars) */
    PERIOD_MONTHS,  /* 12 months (12 monthly bars) */
    PERIOD_YEARS    /* 12 years (12 yearly bars) */
} period_t;

typedef struct {
    const char * label;
    const char * unit_flow;   /* W / l/h / l/min */
    const char * unit_cum;    /* kWh / m3 */
    /* Logger names for flow and (where applicable) cumulative */
    const char * flow_logger;
    const char * cum_logger;
    /* For Electricity we sum two tariffs (nt + lt). */
    const char * cum_logger_extra;
    uint32_t color;
    /* local_hist: not in hcb_rrd — read from freetoon's airhist recorder
       (instantaneous ppm/ppb, never diffed). which = metric index - 3. */
    int local_hist;
} metric_t;
static const metric_t metrics[5] = {
    {"Electricity", "W",     "kWh",  "elec_flow",                  "elec_quantity_nt",      "elec_quantity_lt",      0xaa77ff, 0},
    {"Solar",       "W",     "kWh",  "elec_flow_prod",             "elec_quantity_nt_prod", "elec_quantity_lt_prod", 0xeeaa22, 0},
    {"Gas",         "l/h",   "m3",   "gas_flow",                   "gas_quantity",          NULL,                    0xffaa44, 0},
    {"Water",       "l/min", "m3",   "water_flow",                 "water_quantity",        NULL,                    0x44aaff, 0},
    {"CV bar",      "bar",   "bar",  "thermstat_boilerChPressure", "thermstat_boilerChPressure", NULL,          0x5fb3ff, 0},
};
#define N_METRIC ((int)(sizeof metrics / sizeof metrics[0]))

static const char * metric_title(int i) {
    switch (i) {
        case 0: return tr("Elektriciteit", "Electricity");
        case 1: return tr("Zon", "Solar");
        case 2: return tr("Gas", "Gas");
        case 3: return tr("Water", "Water");
        case 4: return tr("CV bar", "CH bar");
        default: return "";
    }
}

static int      selected_metric = 0;
static period_t selected_period = PERIOD_NOW;
static stats_series_t  series;
static stats_series_t  series2;   /* for elec second tariff */

static void on_back(lv_event_t * e) { (void)e; ui_pop(); }

static void reload_and_render(void);

/* Final bar values + labels, produced by render_chart's bucketing step.
 * The chart shows these directly (one bar per bucket). */
static double g_bar_val[64];
static char   g_bar_label[64][8];
static int    g_bar_count = 0;
static int    g_y_scale = 1;

/* Manual X-axis labels under the chart. Up to 12 evenly-spaced bar labels. */
#define XLBL_COUNT 24
static lv_obj_t * g_xlbl[XLBL_COUNT] = {0};
#define XLBL_Y SY(535)

static void place_x_labels(void) {
    for (int i = 0; i < XLBL_COUNT; i++)
        if (g_xlbl[i]) lv_obj_add_flag(g_xlbl[i], LV_OBJ_FLAG_HIDDEN);
    if (g_bar_count <= 0 || !chart || !cs) return;

    lv_obj_update_layout(chart);
    lv_coord_t chart_x = lv_obj_get_x(chart);

    int shown = (g_bar_count <= 16) ? g_bar_count : 7;
    if (shown > XLBL_COUNT) shown = XLBL_COUNT;

    for (int k = 0; k < shown; k++) {
        int b = (g_bar_count <= 16) ? k
              : (shown > 1 ? k * (g_bar_count - 1) / (shown - 1) : 0);
        lv_point_t pt;
        lv_chart_get_point_pos_by_id(chart, cs, (uint16_t)b, &pt);
        int cx = chart_x + pt.x;

        if (!g_xlbl[k]) continue;
        lv_label_set_text(g_xlbl[k], g_bar_label[b]);
        lv_obj_set_width(g_xlbl[k], SX(58));
        lv_obj_set_style_text_align(g_xlbl[k], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(g_xlbl[k], cx - SX(29), XLBL_Y);
        lv_obj_clear_flag(g_xlbl[k], LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_nav_range_label(long start_t, long end_t) {
    if (!lbl_nav_range) return;
    char buf[96] = "";
    struct tm tm_start, tm_end;
    time_t ts = (time_t)start_t, te = (time_t)end_t;
    localtime_r(&ts, &tm_start);
    localtime_r(&te, &tm_end);

    switch (selected_period) {
        case PERIOD_NOW:
            if (g_time_offset == 0) {
                snprintf(buf, sizeof(buf), "%s", tr("Afgelopen uur (5-min)", "Past hour (5-min)"));
            } else {
                snprintf(buf, sizeof(buf), "%d %s %02d:%02d - %02d:%02d",
                         tm_end.tm_mday, i18n_month_short(tm_end.tm_mon),
                         tm_start.tm_hour, tm_start.tm_min,
                         tm_end.tm_hour, tm_end.tm_min);
            }
            break;
        case PERIOD_HOURS:
            if (g_time_offset == 0) {
                snprintf(buf, sizeof(buf), "%s", tr("Afgelopen 24 uur", "Past 24 hours"));
            } else if (g_time_offset == -1) {
                snprintf(buf, sizeof(buf), "%s (%d %s)",
                         tr("Gisteren", "Yesterday"),
                         tm_start.tm_mday, i18n_month_short(tm_start.tm_mon));
            } else {
                snprintf(buf, sizeof(buf), "%d %s %04d",
                         tm_start.tm_mday, i18n_month_short(tm_start.tm_mon), tm_start.tm_year + 1900);
            }
            break;
        case PERIOD_DAYS:
            snprintf(buf, sizeof(buf), "%d %s - %d %s %04d",
                     tm_start.tm_mday, i18n_month_short(tm_start.tm_mon),
                     tm_end.tm_mday, i18n_month_short(tm_end.tm_mon), tm_end.tm_year + 1900);
            break;
        case PERIOD_WEEKS:
            snprintf(buf, sizeof(buf), "%s (%d %s - %d %s %04d)",
                     tr("12 weken", "12 weeks"),
                     tm_start.tm_mday, i18n_month_short(tm_start.tm_mon),
                     tm_end.tm_mday, i18n_month_short(tm_end.tm_mon), tm_end.tm_year + 1900);
            break;
        case PERIOD_MONTHS:
            snprintf(buf, sizeof(buf), "%s %04d - %s %04d",
                     i18n_month_short(tm_start.tm_mon), tm_start.tm_year + 1900,
                     i18n_month_short(tm_end.tm_mon), tm_end.tm_year + 1900);
            break;
        case PERIOD_YEARS:
            snprintf(buf, sizeof(buf), "%s", tr("Jaaroverzicht", "Yearly overview"));
            break;
    }
    lv_label_set_text(lbl_nav_range, buf);
}

static void on_nav_prev(lv_event_t * e) {
    (void)e;
    g_time_offset--;
    reload_and_render();
}

static void on_nav_next(lv_event_t * e) {
    (void)e;
    if (g_time_offset < 0) {
        g_time_offset++;
        reload_and_render();
    }
}

/* Per-period data fetching. Supports stepping backward/forward in time via g_time_offset. */
static int load_for_period(void) {
    const metric_t * m = &metrics[selected_metric];
    long now = (long)time(NULL);
    long from = 0, to = 0;

    const long HOUR = 3600;
    const long DAY = 86400;
    const long WEEK_SPAN = 12 * 7 * DAY;     /* 12 weeks = 84 days */
    const long DAYS_SPAN = 31 * DAY;         /* 31 days */

    switch (selected_period) {
        case PERIOD_NOW:
            to = now + (long)g_time_offset * HOUR;
            from = to - HOUR;
            break;
        case PERIOD_HOURS:
            to = now + (long)g_time_offset * DAY;
            from = to - DAY;
            break;
        case PERIOD_DAYS:
            to = now + (long)g_time_offset * DAYS_SPAN;
            from = to - DAYS_SPAN;
            break;
        case PERIOD_WEEKS:
            to = now + (long)g_time_offset * WEEK_SPAN;
            from = to - WEEK_SPAN;
            break;
        case PERIOD_MONTHS: {
            /* Exactly 12 calendar months: from 1st of (now - 11 months) to end of month / now */
            struct tm tm_now;
            time_t t_now = (time_t)now;
            localtime_r(&t_now, &tm_now);

            int target_year = tm_now.tm_year + g_time_offset;
            int target_mon  = tm_now.tm_mon;

            struct tm tm_to = {0};
            tm_to.tm_year = target_year;
            tm_to.tm_mon  = target_mon + 1;
            tm_to.tm_mday = 1;
            tm_to.tm_isdst = -1;
            to = (long)mktime(&tm_to);
            if (g_time_offset == 0 && to > now) to = now;

            struct tm tm_from = {0};
            tm_from.tm_year = target_year - 1;
            tm_from.tm_mon  = target_mon + 1;
            tm_from.tm_mday = 1;
            tm_from.tm_isdst = -1;
            from = (long)mktime(&tm_from);
            break;
        }
        case PERIOD_YEARS:
            to = now;
            from = 0;
            break;
    }

    g_current_start_t = from;
    g_current_end_t = to;

    if (strcmp(m->label, "CV bar") == 0) {
        series2.n = 0;
        int samples;
        switch (selected_period) {
            case PERIOD_NOW:    samples = 12; break; /* 12 x 5-minute intervals */
            case PERIOD_HOURS:  samples = 24; break; /* 24 hourly intervals */
            case PERIOD_DAYS:   samples = 31; break; /* 31 daily intervals */
            case PERIOD_WEEKS:  samples = 28; break; /* 4 weeks within 30-day retention */
            case PERIOD_MONTHS: samples = 30; break; /* up to 30 days */
            case PERIOD_YEARS:  samples = 30; break;
            default:            samples = 24; break;
        }
        int rc = stats_fetch_range("thermstat_boilerChPressure", "30days", from, to, samples, &series);
        if (rc != 0 || series.n == 0) {
            /* Fallback if local/master hcb_rrd doesn't have thermstat_boilerChPressure */
            rc = airhist_pres_series_range(from, to, 365, &series);
        }
        if (series.n > 0) {
            for (int i = 0; i < series.n; i++) {
                if (!isnan(series.samples[i]) && series.samples[i] > 0.05) {
                    double v = series.samples[i];
                    if (v >= 50.0)      series.samples[i] = v / 100.0; /* centibar -> bar */
                    else if (v >= 5.0)  series.samples[i] = v / 10.0;  /* decibar  -> bar */
                }
            }
        }
        return rc;
    }

    if (m->local_hist) {                /* freetoon's recorder hourly ring */
        series2.n = 0;
        return airhist_pres_series_range(from, to, 365, &series);
    }

    int is_solar = (selected_metric == 1);

    switch (selected_period) {
        case PERIOD_NOW: {
            /* Live 5-minute flow: 12 samples over 1 hour */
            int rc = stats_fetch_range(m->flow_logger, "5min", from, to, 12, &series);
            if ((rc != 0 || series.n == 0) && is_solar) {
                rc = stats_fetch_range("solar_flow", "5min", from, to, 12, &series);
            }
            return rc;
        }

        case PERIOD_HOURS:
            /* Cumulative meter, hourly samples — diff'd to per-hour usage.
             * 24 hours -> 26 samples with from - 3600 so hour 0 has prev sample. */
            if (m->cum_logger_extra) {
                stats_fetch_range(m->cum_logger_extra, "5yrhours", from - 3600, to, 26, &series2);
            } else {
                series2.n = 0;
            }
            {
                int rc = stats_fetch_range(m->cum_logger, "5yrhours", from - 3600, to, 26, &series);
                if ((rc != 0 || series.n == 0) && is_solar) {
                    series2.n = 0;
                    rc = stats_fetch_range("solar_quantity", "5yrhours", from - 3600, to, 26, &series);
                }
                return rc;
            }

        case PERIOD_DAYS:
            /* 31 days of daily cumulative meter samples -> diff'd to 31 daily bars */
            if (m->cum_logger_extra) {
                stats_fetch_range(m->cum_logger_extra, "10yrdays", from - DAY, to, 34, &series2);
            } else {
                series2.n = 0;
            }
            {
                int rc = stats_fetch_range(m->cum_logger, "10yrdays", from - DAY, to, 34, &series);
                if ((rc != 0 || series.n == 0) && is_solar) {
                    series2.n = 0;
                    rc = stats_fetch_range("solar_quantity", "10yrdays", from - DAY, to, 34, &series);
                }
                return rc;
            }

        case PERIOD_WEEKS:
            /* 12 weeks = 84 days of daily cumulative meter samples -> diff'd to 84 daily deltas,
             * bucketed into 12 weekly bars */
            if (m->cum_logger_extra) {
                stats_fetch_range(m->cum_logger_extra, "10yrdays", from - DAY, to, 88, &series2);
            } else {
                series2.n = 0;
            }
            {
                int rc = stats_fetch_range(m->cum_logger, "10yrdays", from - DAY, to, 88, &series);
                if ((rc != 0 || series.n == 0) && is_solar) {
                    series2.n = 0;
                    rc = stats_fetch_range("solar_quantity", "10yrdays", from - DAY, to, 88, &series);
                }
                return rc;
            }

        case PERIOD_MONTHS:
            /* 12 months = 365 days of daily cumulative meter samples -> diff'd to daily deltas,
             * bucketed by calendar month */
            if (m->cum_logger_extra) {
                stats_fetch_range(m->cum_logger_extra, "10yrdays", from - DAY, to, 370, &series2);
            } else {
                series2.n = 0;
            }
            {
                int rc = stats_fetch_range(m->cum_logger, "10yrdays", from - DAY, to, 370, &series);
                if ((rc != 0 || series.n == 0) && is_solar) {
                    series2.n = 0;
                    rc = stats_fetch_range("solar_quantity", "10yrdays", from - DAY, to, 370, &series);
                }
                return rc;
            }

        case PERIOD_YEARS:
            /* Full 10-year archive at downsampled monthly resolution (~120 samples) -> grouped by year */
            if (m->cum_logger_extra) {
                stats_fetch_range(m->cum_logger_extra, "10yrdays", 0, 0, 120, &series2);
            } else {
                series2.n = 0;
            }
            {
                int rc = stats_fetch_range(m->cum_logger, "10yrdays", 0, 0, 120, &series);
                if ((rc != 0 || series.n == 0) && is_solar) {
                    series2.n = 0;
                    rc = stats_fetch_range("solar_quantity", "10yrdays", 0, 0, 120, &series);
                }
                return rc;
            }
    }
    return -1;
}

static void style_metric_tab(int i, int sel) {
    if (i < 0 || i >= N_METRIC || !tab_metric_btns[i]) return;
    lv_obj_set_style_bg_color(tab_metric_btns[i],
        lv_color_hex(sel ? metrics[i].color : THEME_CARD_BG), 0);
    lv_obj_set_style_border_color(tab_metric_btns[i],
        lv_color_hex(sel ? (settings.home_light_mode ? 0x1d4ed8 : 0xffffff) : THEME_BORDER), 0);
    lv_obj_set_style_border_width(tab_metric_btns[i], sel ? 2 : 1, 0);
    lv_obj_t * lbl = lv_obj_get_child(tab_metric_btns[i], 0);
    if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(sel ? 0xffffff : THEME_TEXT_HI), 0);
}
static void style_period_tab(int i, int sel) {
    if (i < 0 || i >= N_PERIOD || !tab_period_btns[i]) return;
    lv_obj_set_style_bg_color(tab_period_btns[i],
        lv_color_hex(sel ? (settings.home_light_mode ? 0x2563eb : 0x3388aa) : THEME_CARD_BG), 0);
    lv_obj_set_style_border_color(tab_period_btns[i],
        lv_color_hex(sel ? (settings.home_light_mode ? 0x1d4ed8 : 0xffffff) : THEME_BORDER), 0);
    lv_obj_set_style_border_width(tab_period_btns[i], sel ? 2 : 1, 0);
    lv_obj_t * lbl = lv_obj_get_child(tab_period_btns[i], 0);
    if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(sel ? 0xffffff : THEME_TEXT_HI), 0);
}

/* Diff a cumulative series in place to per-sample usage deltas (Wh / mL).
 * Diffs against the previous *real* (non-NaN) sample, skipping NaN gaps —
 * crucial when a wide window returns the real readings scattered among NaN
 * slots (otherwise every reading sits next to a NaN and all deltas vanish).
 * Drops the cross-tariff artefact jumps above `cap`. */
static void diff_in_place(stats_series_t * s, int n, double cap) {
    double prev = NAN;   /* previous real cumulative reading */
    for (int i = 0; i < n; i++) {
        double v = s->samples[i];
        if (isnan(v)) continue;            /* leave NaN slots, keep prev */
        if (isnan(prev)) { s->samples[i] = NAN; prev = v; continue; }
        double d = (v >= prev) ? (v - prev) : NAN;
        if (!isnan(d) && d > cap) d = NAN;
        s->samples[i] = d;
        prev = v;
    }
}

static void chart_draw_event_cb(lv_event_t * e) {
    lv_obj_draw_part_dsc_t * dsc = lv_event_get_draw_part_dsc(e);
    if (!dsc) return;
    if (dsc->part == LV_PART_TICKS && dsc->type == LV_CHART_DRAW_PART_TICK_LABEL && dsc->id == LV_CHART_AXIS_PRIMARY_Y) {
        if (dsc->text) {
            if (dsc->value == 0) {
                lv_snprintf(dsc->text, dsc->text_length, "0");
            } else if (g_y_scale == 100) {
                float v = (float)dsc->value / 100.0f;
                if ((dsc->value % 100) == 0)
                    lv_snprintf(dsc->text, dsc->text_length, "%d", (int)v);
                else if ((dsc->value % 10) == 0)
                    lv_snprintf(dsc->text, dsc->text_length, "%.1f", v);
                else
                    lv_snprintf(dsc->text, dsc->text_length, "%.2f", v);
            } else if (g_y_scale == 10) {
                float v = (float)dsc->value / 10.0f;
                if ((dsc->value % 10) == 0)
                    lv_snprintf(dsc->text, dsc->text_length, "%d", (int)v);
                else
                    lv_snprintf(dsc->text, dsc->text_length, "%.1f", v);
            } else {
                lv_snprintf(dsc->text, dsc->text_length, "%d", dsc->value);
            }
        }
    }
}

/* Calculate a clean, human-friendly Y-axis ceiling for 4 intervals (5 ticks) */
static double calculate_nice_top(double hi) {
    if (hi <= 0.001) return 4.0; /* sensible fallback */

    /* Headroom: target 105% of tallest bar so it doesn't touch the top ceiling */
    double target_top = hi * 1.05;
    double raw_step = target_top / 4.0;

    double exponent = floor(log10(raw_step));
    double mag = pow(10.0, exponent);
    double frac = raw_step / mag; /* 1.0 <= frac <= 10.0 */

    double nice_frac;
    if (frac <= 1.0)       nice_frac = 1.0;
    else if (frac <= 1.25) nice_frac = 1.25;
    else if (frac <= 1.5)  nice_frac = 1.5;
    else if (frac <= 2.0)  nice_frac = 2.0;
    else if (frac <= 2.5)  nice_frac = 2.5;
    else if (frac <= 3.0)  nice_frac = 3.0;
    else if (frac <= 4.0)  nice_frac = 4.0;
    else if (frac <= 5.0)  nice_frac = 5.0;
    else if (frac <= 6.0)  nice_frac = 6.0;
    else if (frac <= 7.5)  nice_frac = 7.5;
    else if (frac <= 8.0)  nice_frac = 8.0;
    else                   nice_frac = 10.0;

    double step = nice_frac * mag;
    return step * 4.0;
}

static void render_chart(void) {
    const metric_t * m = &metrics[selected_metric];
    int n = series.n > STATS_MAX_SAMPLES ? STATS_MAX_SAMPLES : series.n;
    if (n < 0) n = 0;

    int is_cum = (selected_period != PERIOD_NOW) && (strcmp(m->label, "CV bar") != 0) && !m->local_hist;

    /* Cumulative meters → per-sample usage deltas. The elec / solar NT/LT split is
     * diffed separately then summed. */
    if (is_cum) {
        double cap;
        switch (selected_period) {
            case PERIOD_HOURS:  cap = 30000.0; break;
            case PERIOD_DAYS:   cap = 200000.0; break;
            case PERIOD_WEEKS:  cap = 1000000.0; break;
            case PERIOD_MONTHS: cap = 4000000.0; break;
            case PERIOD_YEARS:  cap = 50000000.0; break;
            default:            cap = 200000.0; break;
        }
        diff_in_place(&series, n, cap);
        if (m->cum_logger_extra && series2.n >= n) {
            diff_in_place(&series2, n, cap);
            for (int i = 0; i < n; i++) {
                if (isnan(series.samples[i]) && !isnan(series2.samples[i]))
                    series.samples[i] = series2.samples[i];
                else if (!isnan(series.samples[i]) && !isnan(series2.samples[i]))
                    series.samples[i] += series2.samples[i];
            }
        }
    }

    g_bar_count = 0;

    if (selected_period == PERIOD_YEARS) {
        /* Group consecutive monthly samples by calendar year -> exactly 1 bar per year */
        char cur_yy[3] = "";
        int bar_cnt[64] = {0};
        for (int i = 0; i < n && g_bar_count < 64; i++) {
            if (isnan(series.samples[i])) continue;
            const char * yy = series.year2[i];
            if (!yy[0]) continue;
            if (strncmp(yy, cur_yy, 2) != 0) {
                cur_yy[0] = yy[0]; cur_yy[1] = yy[1]; cur_yy[2] = 0;
                g_bar_val[g_bar_count] = 0;
                bar_cnt[g_bar_count] = 0;
                snprintf(g_bar_label[g_bar_count], sizeof g_bar_label[0], "20%s", yy);
                g_bar_count++;
            }
            g_bar_val[g_bar_count - 1] += series.samples[i];
            bar_cnt[g_bar_count - 1]++;
        }
        if (!is_cum) {
            for (int k = 0; k < g_bar_count; k++) {
                if (bar_cnt[k] > 0) g_bar_val[k] /= bar_cnt[k];
            }
        }
    } else if (selected_period == PERIOD_MONTHS) {
        /* Group consecutive daily samples by calendar month+year */
        int cur_mm = -1; char cur_yy[3] = "";
        int bar_cnt[64] = {0};
        for (int i = 0; i < n && g_bar_count < 64; i++) {
            if (isnan(series.samples[i])) continue;
            const char * lab = series.labels[i];
            int mm = (lab[3]-'0')*10 + (lab[4]-'0');
            const char * yy = series.year2[i];
            if (mm != cur_mm || strncmp(yy, cur_yy, 2) != 0) {
                cur_mm = mm;
                cur_yy[0] = yy[0]; cur_yy[1] = yy[1]; cur_yy[2] = 0;
                g_bar_val[g_bar_count] = 0;
                bar_cnt[g_bar_count] = 0;
                if (mm >= 1 && mm <= 12)
                    snprintf(g_bar_label[g_bar_count], sizeof g_bar_label[0], "%s", i18n_month_short(mm - 1));
                else
                    snprintf(g_bar_label[g_bar_count], sizeof g_bar_label[0], "?");
                g_bar_count++;
            }
            g_bar_val[g_bar_count-1] += series.samples[i];
            bar_cnt[g_bar_count-1]++;
        }
        if (!is_cum) {
            for (int k = 0; k < g_bar_count; k++) {
                if (bar_cnt[k] > 0) g_bar_val[k] /= bar_cnt[k];
            }
        }
    } else {
        int want;
        switch (selected_period) {
            case PERIOD_NOW:   want = 12; break;
            case PERIOD_HOURS: want = 24; break;
            case PERIOD_DAYS:  want = 31; break;
            case PERIOD_WEEKS: want = 12; break;
            default:           want = 12; break;
        }

        static double comp[STATS_MAX_SAMPLES];
        static int    compi[STATS_MAX_SAMPLES];
        int cn = 0;
        for (int i = 0; i < n; i++)
            if (!isnan(series.samples[i])) { comp[cn] = series.samples[i]; compi[cn] = i; cn++; }
        int bars = (want < cn) ? want : cn;
        if (bars > 64) bars = 64;
        for (int b = 0; b < bars; b++) {
            int s = b * cn / bars, e = (b + 1) * cn / bars;
            if (e <= s) e = s + 1;
            if (e > cn) e = cn;
            double acc = 0; int cnt = 0;
            for (int k = s; k < e; k++) { acc += comp[k]; cnt++; }
            g_bar_val[b] = is_cum ? acc : (cnt ? acc / cnt : 0);
            const char * lab = series.labels[compi[s]];
            if (selected_period == PERIOD_NOW || selected_period == PERIOD_HOURS)
                snprintf(g_bar_label[b], sizeof g_bar_label[0], "%.5s", lab + 6); /* HH:MM */
            else
                snprintf(g_bar_label[b], sizeof g_bar_label[0], "%.5s", lab);     /* DD-MM */
            g_bar_count++;
        }
    }

    /* Cumulative bars are summed deltas in Wh/L — convert to kWh/m3 */
    if (is_cum)
        for (int i = 0; i < g_bar_count; i++) g_bar_val[i] /= 1000.0;

    /* Extra safeguard: ensure CV bar values are in bar (e.g. 1.3 bar, not 13 or 130) */
    if (strcmp(m->label, "CV bar") == 0) {
        for (int i = 0; i < g_bar_count; i++) {
            if (g_bar_val[i] >= 50.0)      g_bar_val[i] /= 100.0;
            else if (g_bar_val[i] >= 5.0)  g_bar_val[i] /= 10.0;
        }
    }

    /* Y range from 0 baseline up to clean quantized nice-step top */
    double hi = 0;
    for (int i = 0; i < g_bar_count; i++) if (g_bar_val[i] > hi) hi = g_bar_val[i];
    double top;
    if (strcmp(m->label, "CV bar") == 0) {
        if (hi <= 1.7) top = 2.0;       /* Standard 2.0 bar scale (ticks: 0, 0.5, 1, 1.5, 2) */
        else if (hi <= 2.2) top = 2.5;
        else if (hi <= 2.6) top = 3.0;
        else top = calculate_nice_top(hi);
    } else {
        top = calculate_nice_top(hi);
    }

    double step = top / 4.0;
    /* Determine scale factor so that each of the 4 tick values is an exact integer in chart coords */
    if (fabs(step - floor(step + 0.0001)) < 0.001) {
        g_y_scale = 1;
    } else if (fabs(step * 10.0 - floor(step * 10.0 + 0.0001)) < 0.001) {
        g_y_scale = 10;
    } else {
        g_y_scale = 100;
    }

    int pc = g_bar_count < 2 ? 2 : g_bar_count;
    lv_chart_set_point_count(chart, pc);
    lv_coord_t top_scaled = (lv_coord_t)(top * g_y_scale + 0.5);
    if (top_scaled < 1) top_scaled = 1;
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, top_scaled);
    for (int i = 0; i < pc; i++) {
        if (i < g_bar_count) {
            cs->y_points[i] = (lv_coord_t)(g_bar_val[i] * g_y_scale + 0.5);
        } else {
            cs->y_points[i] = LV_CHART_POINT_NONE;
        }
    }

    lv_chart_refresh(chart);
    place_x_labels();
    update_nav_range_label(g_current_start_t, g_current_end_t);

    /* Update navigation buttons */
    if (btn_nav_prev && btn_nav_next) {
        if (selected_period == PERIOD_YEARS) {
            lv_obj_add_flag(btn_nav_prev, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(btn_nav_next, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(btn_nav_prev, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(btn_nav_next, LV_OBJ_FLAG_HIDDEN);
            if (g_time_offset >= 0) {
                lv_obj_set_style_opa(btn_nav_next, LV_OPA_30, 0);
                lv_obj_clear_flag(btn_nav_next, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_set_style_opa(btn_nav_next, LV_OPA_COVER, 0);
                lv_obj_add_flag(btn_nav_next, LV_OBJ_FLAG_CLICKABLE);
            }
        }
    }

    /* Caption + headline value. */
    if (!is_cum) {
        if (g_time_offset == 0)
            lv_label_set_text(lbl_unit, tr("Huidig", "Current"));
        else
            lv_label_set_text(lbl_unit, tr("Gemiddelde", "Average"));

        double cur = NAN;
        if (g_time_offset == 0) {
            if (selected_metric == 0)                                            cur = hw_state.power_w;
            else if (selected_metric == 1 && energy_have_solar())                cur = energy_solar_w();
            else if (selected_metric == 3)                                       cur = hw_state.water_lpm;
            else if (strcmp(m->label, "CV bar") == 0 && toon_state.water_pressure > 0.05f) cur = toon_state.water_pressure;
        }
        if (isnan(cur) || cur == 0) {
            for (int i = series.n - 1; i >= 0; i--)
                if (!isnan(series.samples[i])) { cur = series.samples[i]; break; }
        }
        if (isnan(cur)) {
            lv_label_set_text(lbl_value, "--");
        } else if (strcmp(m->unit_flow, "W") == 0) {
            lv_label_set_text_fmt(lbl_value, "%.0f %s", cur, m->unit_flow);
        } else {
            lv_label_set_text_fmt(lbl_value, "%.1f %s", cur, m->unit_flow);
        }
    } else {
        lv_label_set_text(lbl_unit, tr("Periodetotaal", "Period total"));
        if (g_bar_count == 0) {
            lv_label_set_text(lbl_value, tr("geen data", "no data"));
        } else {
            double total = 0;   /* g_bar_val already in kWh/m3 for cumulative */
            for (int i = 0; i < g_bar_count; i++) total += g_bar_val[i];
            if (total < 10.0 && total > 0.0)
                lv_label_set_text_fmt(lbl_value, "%.2f %s", total, m->unit_cum);
            else
                lv_label_set_text_fmt(lbl_value, "%.1f %s", total, m->unit_cum);
        }
    }
}

static void reload_and_render(void) {
    lv_chart_set_series_color(chart, cs, lv_color_hex(metrics[selected_metric].color));
    lv_label_set_text(lbl_metric_name, metric_title(selected_metric));
    load_for_period();
    render_chart();
}

static void on_metric_tap(lv_event_t * e) {
    int idx = (int)(long)lv_event_get_user_data(e);
    if (idx < 0 || idx >= N_METRIC) return;
    selected_metric = idx;
    g_time_offset = 0;
    for (int i = 0; i < N_METRIC; i++) style_metric_tab(i, i == idx);
    reload_and_render();
}
static void on_period_tap(lv_event_t * e) {
    int idx = (int)(long)lv_event_get_user_data(e);
    if (idx < 0 || idx >= N_PERIOD) return;
    selected_period = (period_t)idx;
    g_time_offset = 0;
    for (int i = 0; i < N_PERIOD; i++) style_period_tab(i, i == idx);
    reload_and_render();
}

static int scr_light_mode = -1;

lv_obj_t * screen_stats_create(void) {
    if (scr_root && scr_light_mode == settings.home_light_mode) {
        reload_and_render();
        return scr_root;
    }
    if (scr_root) { lv_obj_del(scr_root); scr_root = NULL; }
    scr_light_mode = settings.home_light_mode;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(THEME_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Header */
    int x_w = (DISP_HOR < 1000) ? 68 : 74;
    int x_h = (DISP_VER < 600) ? 46 : 50;

    lv_obj_t * x = lv_btn_create(scr_root);
    lv_obj_set_size(x, x_w, x_h);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, SX(-12), SY(12));
    lv_obj_set_style_bg_color(x, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(x, 12, 0);
    lv_obj_set_ext_click_area(x, 25);
    lv_obj_add_event_cb(x, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(xl, SF(22), 0);
    lv_obj_center(xl);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, tr("Statistieken", "Statistics"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(24), SY(18));

    /* Metric tabs row (5 metrics: Electricity, Solar, Gas, Water, CV bar) */
    for (int i = 0; i < N_METRIC; i++) {
        lv_obj_t * t = lv_obj_create(scr_root);
        lv_obj_set_size(t, SX(180), SY(54));
        lv_obj_set_pos(t, SX(26 + i * 198), SY(100));
        lv_obj_set_style_radius(t, 12, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, on_metric_tap, LV_EVENT_CLICKED, (void *)(long)i);
        lv_obj_t * lbl = lv_label_create(t);
        lv_label_set_text(lbl, metric_title(i));
        lv_obj_set_style_text_font(lbl, SF(20), 0);
        lv_obj_center(lbl);
        tab_metric_btns[i] = t;
        style_metric_tab(i, i == selected_metric);
    }

    /* Period tabs row (6 tabs: Nu, Uren, Dagen, Weken, Maanden, Jaren) */
    const char * periods[] = {
        tr("Nu", "Now"),
        tr("Uren", "Hours"),
        tr("Dagen", "Days"),
        tr("Weken", "Weeks"),
        tr("Maanden", "Months"),
        tr("Jaren", "Years")
    };
    for (int i = 0; i < N_PERIOD; i++) {
        lv_obj_t * t = lv_obj_create(scr_root);
        lv_obj_set_size(t, SX(150), SY(44));
        lv_obj_set_pos(t, SX(26 + i * 163), SY(164));
        lv_obj_set_style_radius(t, 10, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, on_period_tap, LV_EVENT_CLICKED, (void *)(long)i);
        lv_obj_t * lbl = lv_label_create(t);
        lv_label_set_text(lbl, periods[i]);
        lv_obj_set_style_text_font(lbl, SF(17), 0);
        lv_obj_center(lbl);
        tab_period_btns[i] = t;
        style_period_tab(i, i == (int)selected_period);
    }

    /* Time navigation row above chart */
    btn_nav_prev = lv_btn_create(scr_root);
    lv_obj_set_size(btn_nav_prev, SX(46), SY(32));
    lv_obj_set_pos(btn_nav_prev, SX(360), SY(214));
    lv_obj_set_style_bg_color(btn_nav_prev, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(btn_nav_prev, 6, 0);
    lv_obj_set_style_border_color(btn_nav_prev, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(btn_nav_prev, 1, 0);
    lv_obj_set_ext_click_area(btn_nav_prev, 10);
    lv_obj_add_event_cb(btn_nav_prev, on_nav_prev, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl_prev = lv_label_create(btn_nav_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(lbl_prev, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_prev, SF(16), 0);
    lv_obj_center(lbl_prev);

    lbl_nav_range = lv_label_create(scr_root);
    lv_obj_set_size(lbl_nav_range, SX(550), SY(30));
    lv_obj_set_pos(lbl_nav_range, SX(390), SY(218));
    lv_obj_set_style_text_color(lbl_nav_range, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_nav_range, SF(18), 0);
    lv_obj_set_style_text_align(lbl_nav_range, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_nav_range, "");

    btn_nav_next = lv_btn_create(scr_root);
    lv_obj_set_size(btn_nav_next, SX(46), SY(32));
    lv_obj_set_pos(btn_nav_next, SX(944), SY(214));
    lv_obj_set_style_bg_color(btn_nav_next, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(btn_nav_next, 6, 0);
    lv_obj_set_style_border_color(btn_nav_next, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(btn_nav_next, 1, 0);
    lv_obj_set_ext_click_area(btn_nav_next, 10);
    lv_obj_add_event_cb(btn_nav_next, on_nav_next, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl_next = lv_label_create(btn_nav_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(lbl_next, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_next, SF(16), 0);
    lv_obj_center(lbl_next);

    /* Headline */
    lbl_metric_name = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_metric_name, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_metric_name, SF(22), 0);
    lv_label_set_text(lbl_metric_name, metric_title(0));
    lv_obj_align(lbl_metric_name, LV_ALIGN_TOP_LEFT, SX(30), SY(245));

    /* Repurposed as the "Current" / "Period total" caption above the
     * value. Was rendering a duplicated unit ("W" under "369.0 W"). */
    lbl_unit = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_unit, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_unit, SF(18), 0);
    lv_label_set_text(lbl_unit, tr("Huidig", "Current"));
    lv_obj_align(lbl_unit, LV_ALIGN_TOP_LEFT, SX(30), SY(280));

    lbl_value = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_value, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_value, SF(36), 0);
    lv_label_set_text(lbl_value, "--");
    lv_obj_align(lbl_value, LV_ALIGN_TOP_LEFT, SX(30), SY(310));

    /* Chart */
    chart = lv_chart_create(scr_root);
    lv_obj_set_size(chart, SX(660), SY(280));
    lv_obj_align(chart, LV_ALIGN_TOP_RIGHT, SX(-30), SY(250));
    /* Bars, like the original Toon energy view. Rounded top corners + a
     * little inter-bar gap so they read as discrete usage blocks. */
    lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
    lv_chart_set_div_line_count(chart, 5, 0);
    lv_obj_set_style_radius(chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_column(chart, 3, LV_PART_MAIN);
    /* Y axis tick labels — 5 major divisions, draw_size 60 reserves space. */
    lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_Y, 8, 4, 5, 2, true, 60);
    /* X axis: short tick marks only, no built-in labels — we draw our own
     * labels (place_x_labels) under the chart since lv_chart's bar tick
     * labels never rendered reliably. */
    lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_X, 6, 0, 2, 0, false, 14);
    lv_obj_set_style_bg_color(chart, lv_color_hex(THEME_CARD_BG), 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_radius(chart, 12, 0);
    cs = lv_chart_add_series(chart, lv_color_hex(metrics[0].color),
                             LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_add_event_cb(chart, chart_draw_event_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    /* Manual X-axis label widgets under the chart. */
    for (int i = 0; i < XLBL_COUNT; i++) {
        g_xlbl[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_color(g_xlbl[i], lv_color_hex(THEME_TEXT_DIM), 0);
        lv_obj_set_style_text_font(g_xlbl[i], SF(14), 0);
        lv_label_set_text(g_xlbl[i], "");
        lv_obj_add_flag(g_xlbl[i], LV_OBJ_FLAG_HIDDEN);
    }

    reload_and_render();
    return scr_root;
}
