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
#include "stats.h"
#include "airhist.h"
#include "energy_hist.h"
#include "homewizard.h"
#include "settings.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <time.h>

static lv_obj_t * scr_root = NULL;
static lv_obj_t * chart;
static lv_chart_series_t * cs;
static lv_obj_t * lbl_metric_name;
static lv_obj_t * lbl_unit;
static lv_obj_t * lbl_value;
static lv_obj_t * tab_metric_btns[6];
static lv_obj_t * tab_period_btns[5];

typedef enum { PERIOD_HOUR, PERIOD_DAY, PERIOD_WEEK, PERIOD_MONTH, PERIOD_YEAR } period_t;

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
static const metric_t metrics[] = {
    {"Electricity", "W",     "kWh",  "elec_flow",  "elec_quantity_nt", "elec_quantity_lt", 0xaa77ff, 0},
    {"Gas",         "l/h",   "m3",   "gas_flow",   "gas_quantity",     NULL,                0xffaa44, 0},
    {"Water",       "l/min", "m3",   "water_flow", "water_quantity",   NULL,                0x44aaff, 0},
#ifndef TOON1
    {"CO2",         "ppm",   "ppm",  NULL,         NULL,               NULL,                0x44cc88, 1},
    {"TVOC",        "ppb",   "ppb",  NULL,         NULL,               NULL,                0xcc8844, 1},
#endif
    {"CV bar",      "bar",   "bar",  NULL,         NULL,               NULL,                0x5fb3ff, 2},  /* CH pressure, all periods */
};
#define N_METRIC ((int)(sizeof metrics / sizeof metrics[0]))

static int      selected_metric = 0;
static period_t selected_period = PERIOD_HOUR;
static stats_series_t  series;
static stats_series_t  series2;   /* for elec second tariff */
static lv_obj_t * nav_l;          /* ← older  */
static lv_obj_t * nav_r;          /* → newer  */
static lv_obj_t * lbl_nav;        /* "now" / "-8d" hint between the arrows */

static void on_back(lv_event_t * e) { (void)e; ui_pop(); }

/* Final bar values + labels, produced by render_chart's bucketing step.
 * The chart shows these directly (one bar per bucket). */
static double g_bar_val[64];
static char   g_bar_label[64][12];
static int    g_bar_count = 0;
/* lv_chart points are int16, so small fractional energy (gas/water, and hourly
 * kWh) would truncate to 0/1. Scale the points up by g_chart_scale and divide
 * back when drawing the Y-axis labels (with g_chart_dec decimals). */
static double g_chart_scale = 1;
static int    g_chart_dec   = 0;
static double g_period_total = 0;   /* sum of the visible buckets (signed) */

/* Manual X-axis labels under the chart (lv_chart's own tick labels never
 * rendered reliably for bars). Up to 12 evenly-spaced bar labels. */
#define XLBL_COUNT 12
static lv_obj_t * g_xlbl[XLBL_COUNT] = {0};
#define XLBL_Y SY(540)

/* Place up to XLBL_COUNT date labels exactly under their bars. We ask lv_chart
 * for each bar's real centre (lv_chart_get_point_pos_by_id) instead of guessing
 * the plot rectangle — the old hand-tuned PLOT_L/PLOT_R never matched the
 * chart's actual bar layout, so the labels drifted off the bars. */
static void place_x_labels(void) {
    for (int i = 0; i < XLBL_COUNT; i++)
        if (g_xlbl[i]) lv_obj_add_flag(g_xlbl[i], LV_OBJ_FLAG_HIDDEN);
    if (g_bar_count <= 0) return;
    lv_obj_update_layout(chart);            /* so get_x + point positions are valid */
    int chart_x = lv_obj_get_x(chart);
    int shown = g_bar_count < XLBL_COUNT ? g_bar_count : XLBL_COUNT;
    for (int k = 0; k < shown; k++) {
        int b = (g_bar_count <= XLBL_COUNT) ? k
              : (shown > 1 ? k * (g_bar_count - 1) / (shown - 1) : 0);
        if (!g_xlbl[k]) continue;
        lv_point_t p;
        lv_chart_get_point_pos_by_id(chart, cs, (uint16_t)b, &p);  /* x rel. to chart */
        int cx = chart_x + p.x;
        lv_label_set_text(g_xlbl[k], g_bar_label[b]);
        lv_obj_set_width(g_xlbl[k], SX(58));
        lv_obj_set_style_text_align(g_xlbl[k], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(g_xlbl[k], cx - SX(29), XLBL_Y);
        lv_obj_clear_flag(g_xlbl[k], LV_OBJ_FLAG_HIDDEN);
    }
}


/* ---- Period model -------------------------------------------------------
 * Each tab is a fixed set of calendar buckets:
 *   Hour  = last 12 clock-hours      (label = hour)
 *   Day   = 8 days                   (label = weekday Ma/Di… or Mon/Tue…)
 *   Week  = 8 ISO weeks              (label = week number)
 *   Month = 12 months                (label = month name)
 *   Year  = 8 years                  (label = year)
 * Day/Week can be paged further back with nav_offset (windows back in time).
 * Buckets are filled from the local energy history (configured source) first,
 * then any still-empty bucket is back-filled from the Toon's built-in RRD. */
static int nav_offset = 0;          /* windows back; Day/Week only */
static int g_bar_prod[64];          /* 1 = bar is net export → draw green */

static const char * WD_EN[7] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
static const char * WD_NL[7] = {"Ma","Di","Wo","Do","Vr","Za","Zo"};
static const char * MO_EN[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
static const char * MO_NL[12] = {"jan","feb","mrt","apr","mei","jun",
                                 "jul","aug","sep","okt","nov","dec"};

static void diff_in_place(stats_series_t * s, int n, double cap);   /* fwd */

static int period_bars(period_t p) {
    switch (p) { case PERIOD_HOUR: return 12; case PERIOD_DAY: return 8;
                 case PERIOD_WEEK: return 8;  case PERIOD_MONTH: return 12;
                 case PERIOD_YEAR: return 12; }
    return 8;
}
static int period_pageable(period_t p) { return p != PERIOD_YEAR; }

typedef struct { long t0, t1; double val; int cnt; int has; } sbucket_t;
static sbucket_t bk[64];
static int       nbk;

/* Build nbk buckets for the current period (+nav_offset); fill t0/t1 + label. */
static void build_buckets(void) {
    int nl = (settings.lang == 1);
    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    nbk = period_bars(selected_period);
    int off = period_pageable(selected_period) ? nav_offset : 0;
    for (int i = 0; i < nbk; i++) { bk[i].val = 0; bk[i].cnt = 0; bk[i].has = 0; }

    if (selected_period == PERIOD_HOUR) {
        struct tm h = tm; h.tm_min = 0; h.tm_sec = 0;
        long h0 = (long)mktime(&h) - (long)off * 12 * 3600;   /* page back 12 h */
        for (int i = 0; i < nbk; i++) {
            long t0 = h0 - (long)(nbk - 1 - i) * 3600;
            bk[i].t0 = t0; bk[i].t1 = t0 + 3600;
            struct tm lt; time_t tt = t0; localtime_r(&tt, &lt);
            snprintf(g_bar_label[i], sizeof g_bar_label[0], "%d", lt.tm_hour);
        }
    } else if (selected_period == PERIOD_DAY) {
        struct tm d = tm; d.tm_hour = 12; d.tm_min = 0; d.tm_sec = 0;
        long end = (long)mktime(&d) - (long)off * 7 * 86400;   /* page back 1 week */
        for (int i = 0; i < nbk; i++) {
            long noon = end - (long)(nbk - 1 - i) * 86400;
            struct tm lt; time_t tt = noon; localtime_r(&tt, &lt);
            struct tm m0 = lt; m0.tm_hour = 0; m0.tm_min = 0; m0.tm_sec = 0;
            long t0 = (long)mktime(&m0);
            bk[i].t0 = t0; bk[i].t1 = t0 + 86400;
            snprintf(g_bar_label[i], sizeof g_bar_label[0], "%s",
                     nl ? WD_NL[(lt.tm_wday + 6) % 7] : WD_EN[(lt.tm_wday + 6) % 7]);
        }
    } else if (selected_period == PERIOD_WEEK) {
        struct tm d = tm; d.tm_hour = 12; d.tm_min = 0; d.tm_sec = 0;
        int dow = (d.tm_wday + 6) % 7;                 /* 0 = Monday */
        long monNoon = (long)mktime(&d) - (long)dow * 86400;
        long end = monNoon - (long)off * nbk * 7 * 86400;
        for (int i = 0; i < nbk; i++) {
            long noon = end - (long)(nbk - 1 - i) * 7 * 86400;
            struct tm lt; time_t tt = noon; localtime_r(&tt, &lt);
            struct tm m0 = lt; m0.tm_hour = 0; m0.tm_min = 0; m0.tm_sec = 0;
            long t0 = (long)mktime(&m0);
            bk[i].t0 = t0; bk[i].t1 = t0 + 7 * 86400;
            char w[8]; strftime(w, sizeof w, "%V", &lt);
            snprintf(g_bar_label[i], sizeof g_bar_label[0], "wk%s", w);
        }
    } else if (selected_period == PERIOD_MONTH) {
        for (int i = 0; i < nbk; i++) {
            struct tm m0 = tm; m0.tm_mday = 1; m0.tm_hour = 0; m0.tm_min = 0;
            m0.tm_sec = 0; m0.tm_isdst = -1;   /* let mktime pick DST for that month */
            m0.tm_mon -= (nbk - 1 - i) + off * 12;   /* page back 12 months */
            long t0 = (long)mktime(&m0);
            struct tm lt; time_t tt = t0; localtime_r(&tt, &lt);
            struct tm m1 = lt; m1.tm_mon += 1; m1.tm_isdst = -1;
            long t1 = (long)mktime(&m1);
            bk[i].t0 = t0; bk[i].t1 = t1;
            snprintf(g_bar_label[i], sizeof g_bar_label[0], "%s",
                     nl ? MO_NL[lt.tm_mon] : MO_EN[lt.tm_mon]);
        }
    } else { /* PERIOD_YEAR */
        for (int i = 0; i < nbk; i++) {
            struct tm y0 = tm; y0.tm_mon = 0; y0.tm_mday = 1;
            y0.tm_hour = 0; y0.tm_min = 0; y0.tm_sec = 0; y0.tm_isdst = -1;
            y0.tm_year -= (nbk - 1 - i);
            long t0 = (long)mktime(&y0);
            struct tm y1 = y0; y1.tm_year += 1; long t1 = (long)mktime(&y1);
            bk[i].t0 = t0; bk[i].t1 = t1;
            snprintf(g_bar_label[i], sizeof g_bar_label[0], "'%02d",
                     (y0.tm_year + 1900) % 100);   /* 12 bars → compact 2-digit year */
        }
    }
}

/* Add a timestamped series into the buckets. only_empty=1 skips buckets that
 * already hold local data (used for the RRD back-fill). */
static void bucket_series(const stats_series_t * s, int only_empty) {
    for (int i = 0; i < s->n; i++) {
        double v = s->samples[i];
        if (isnan(v)) continue;
        long t = s->ts[i];
        if (t == 0) continue;
        for (int b = 0; b < nbk; b++) {
            if (t < bk[b].t0 || t >= bk[b].t1) continue;
            if (only_empty && bk[b].has) break;
            bk[b].val += v; bk[b].cnt++;
            if (!only_empty) bk[b].has = 1;
            break;
        }
    }
}

/* Build buckets + fill them → g_bar_val / g_bar_prod / g_bar_count. Returns 0
 * if any bucket got data. */
static int build_and_fill(void) {
    const metric_t * m = &metrics[selected_metric];
    build_buckets();
    long win_from = bk[0].t0;
    long now = (long)time(NULL);
    int is_air = (m->local_hist != 0);    /* CO2/TVOC/pressure → instantaneous avg */

    series.n = 0; series2.n = 0;
    if (is_air) {
        if (m->local_hist == 2)
            airhist_pres_series(now - win_from, STATS_MAX_SAMPLES, &series);
        else
            airhist_series(selected_metric - 3, now - win_from, STATS_MAX_SAMPLES, &series);
        bucket_series(&series, 0);
        for (int b = 0; b < nbk; b++) if (bk[b].cnt) bk[b].val /= bk[b].cnt;  /* average */
    } else {
        if (selected_period == PERIOD_HOUR)
            energy_hist_hour_series(selected_metric, &series);
        else
            energy_hist_daily_series(selected_metric, win_from, bk[nbk - 1].t1, &series);
        bucket_series(&series, 0);

        /* Back-fill any bucket with no local data from the built-in RRD. */
        int empty = 0;
        for (int b = 0; b < nbk; b++) if (!bk[b].has) { empty = 1; break; }
        if (empty && m->cum_logger) {
            long wsec = now - win_from; if (wsec < 3600) wsec = 3600;
            const char * rra = (selected_period == PERIOD_HOUR ||
                                selected_period == PERIOD_DAY) ? "5yrhours" : "10yrdays";
            if (stats_fetch(m->cum_logger, rra, wsec, STATS_MAX_SAMPLES, &series) == 0 &&
                series.n > 0) {
                diff_in_place(&series, series.n, 1e9);
                if (m->cum_logger_extra &&
                    stats_fetch(m->cum_logger_extra, rra, wsec, STATS_MAX_SAMPLES, &series2) == 0)
                    diff_in_place(&series2, series2.n, 1e9);
                for (int i = 0; i < series.n; i++) {           /* Wh/mL → kWh/m3 */
                    double v = series.samples[i];
                    if (m->cum_logger_extra && i < series2.n && !isnan(series2.samples[i]))
                        v = (isnan(v) ? 0 : v) + series2.samples[i];
                    series.samples[i] = isnan(v) ? NAN : v / 1000.0;
                }
                bucket_series(&series, 1);
            }
        }
    }

    g_bar_count = nbk;
    int any = 0; g_period_total = 0;
    for (int b = 0; b < nbk; b++) {
        double v = bk[b].val;
        g_period_total += v;
        g_bar_prod[b] = (selected_metric == 0 && v < 0) ? 1 : 0;   /* net export */
        g_bar_val[b]  = (v < 0) ? -v : v;
        if (bk[b].cnt) any = 1;
    }
    return any ? 0 : -1;
}

static void style_metric_tab(int i, int sel) {
    lv_obj_set_style_bg_color(tab_metric_btns[i],
        lv_color_hex(sel ? metrics[i].color : 0x1a2a44), 0);
    lv_obj_set_style_border_color(tab_metric_btns[i],
        lv_color_hex(sel ? 0xffffff : 0x335577), 0);
    lv_obj_set_style_border_width(tab_metric_btns[i], sel ? 2 : 1, 0);
}
static void style_period_tab(int i, int sel) {
    lv_obj_set_style_bg_color(tab_period_btns[i],
        lv_color_hex(sel ? 0x3388aa : 0x1a2a44), 0);
    lv_obj_set_style_border_color(tab_period_btns[i],
        lv_color_hex(sel ? 0xffffff : 0x335577), 0);
    lv_obj_set_style_border_width(tab_period_btns[i], sel ? 2 : 1, 0);
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

/* Per-bar colour: green where the bar is net production (g_bar_prod[id]),
 * otherwise the metric's colour. lv_chart paints a whole series one colour, so
 * we override each bar's fill in the item draw event. */
static void chart_draw_cb(lv_event_t * e) {
    lv_obj_draw_part_dsc_t * d = lv_event_get_draw_part_dsc(e);
    if (!d) return;
    /* Y-axis tick labels: divide the scaled value back + show decimals. */
    if (d->part == LV_PART_TICKS && d->id == LV_CHART_AXIS_PRIMARY_Y && d->text) {
        lv_snprintf(d->text, d->text_length, "%.*f",
                    g_chart_dec, (double)d->value / g_chart_scale);
        return;
    }
    if (d->part != LV_PART_ITEMS || !d->rect_dsc) return;
    int id = (int)d->id;
    if (id >= 0 && id < g_bar_count && g_bar_prod[id])
        d->rect_dsc->bg_color = lv_color_hex(0x33cc66);            /* export → green */
    else
        d->rect_dsc->bg_color = lv_color_hex(metrics[selected_metric].color);
}

/* Show/enable the ← → paging arrows only on the pageable Day/Week tabs, and
 * show how far back we are. */
static void update_nav_ui(void) {
    if (!nav_l) return;
    int pageable = period_pageable(selected_period);
    if (!pageable) {
        lv_obj_add_flag(nav_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(nav_r, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_nav, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(nav_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(nav_r, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_nav, LV_OBJ_FLAG_HIDDEN);
    /* → disabled (dimmed) at the present. */
    lv_obj_set_style_bg_opa(nav_r, nav_offset > 0 ? LV_OPA_COVER : LV_OPA_50, 0);
    if (nav_offset == 0) { lv_label_set_text(lbl_nav, "now"); return; }
    switch (selected_period) {
    case PERIOD_HOUR:  lv_label_set_text_fmt(lbl_nav, "-%d hrs", nav_offset * 12); break;
    case PERIOD_DAY:   lv_label_set_text_fmt(lbl_nav, "-%d wk",  nav_offset);      break;
    case PERIOD_WEEK:  lv_label_set_text_fmt(lbl_nav, "-%d wk",  nav_offset * 8);  break;
    case PERIOD_MONTH: lv_label_set_text_fmt(lbl_nav, "-%d mo",  nav_offset * 12); break;
    default:           lv_label_set_text(lbl_nav, "now");                          break;
    }
}

static void render_chart(void) {
    /* g_bar_val[] (magnitudes), g_bar_prod[] (export flag) and g_bar_count are
     * produced by build_and_fill(); here we just push them to the chart. */
    double hi = 0;
    for (int i = 0; i < g_bar_count; i++) if (g_bar_val[i] > hi) hi = g_bar_val[i];
    double top = hi * 1.1; if (top < 0.001) top = 1;
    /* Scale + decimals so small fractional bars aren't truncated to 0/1. */
    if      (top < 2)  { g_chart_scale = 100; g_chart_dec = 2; }
    else if (top < 20) { g_chart_scale = 10;  g_chart_dec = 1; }
    else               { g_chart_scale = 1;   g_chart_dec = 0; }
    lv_coord_t top_i = (lv_coord_t)(top * g_chart_scale + 0.5);
    if (top_i < 1) top_i = 1;

    int pc = g_bar_count < 2 ? 2 : g_bar_count;
    lv_chart_set_point_count(chart, pc);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, top_i);
    for (int i = 0; i < pc; i++) {
        if (i >= g_bar_count) { cs->y_points[i] = LV_CHART_POINT_NONE; continue; }
        double yv = g_bar_val[i] * g_chart_scale + 0.5;
        if (yv > 32767) yv = 32767;
        cs->y_points[i] = (lv_coord_t)yv;
    }
    lv_chart_refresh(chart);
    place_x_labels();

    /* Headline. Electricity → live NET watts (negative + green while exporting
     * solar). Gas/Water → the period's total USAGE (not the meter's running
     * total). Air → the latest sample. */
    const metric_t * m = &metrics[selected_metric];
    uint32_t vcol = 0xffffff;
    if (selected_metric == 0) {
        lv_label_set_text(lbl_unit, "Now");
        float net = energy_hist_now_net_w();
        if (net < 0) vcol = 0x33cc66;
        lv_label_set_text_fmt(lbl_value, "%.0f W", net);
    } else if (selected_metric == 1 || selected_metric == 2) {
        lv_label_set_text(lbl_unit, "Total");
        lv_label_set_text_fmt(lbl_value, "%.2f %s", g_period_total, m->unit_cum);
    } else {
        lv_label_set_text(lbl_unit, "Now");
        double cur = NAN;
        for (int i = series.n - 1; i >= 0; i--)
            if (!isnan(series.samples[i])) { cur = series.samples[i]; break; }
        if (isnan(cur)) lv_label_set_text(lbl_value, "--");
        else lv_label_set_text_fmt(lbl_value, "%.1f %s", cur, m->unit_flow);
    }
    lv_obj_set_style_text_color(lbl_value, lv_color_hex(vcol), 0);

    update_nav_ui();
}

static void reload_and_render(void) {
    lv_chart_set_series_color(chart, cs, lv_color_hex(metrics[selected_metric].color));
    lv_label_set_text(lbl_metric_name, metrics[selected_metric].label);
    build_and_fill();
    render_chart();
}

static void on_metric_tap(lv_event_t * e) {
    int idx = (int)(long)lv_event_get_user_data(e);
    selected_metric = idx;
    for (int i = 0; i < N_METRIC; i++) style_metric_tab(i, i == idx);
    reload_and_render();
}
static void on_period_tap(lv_event_t * e) {
    int idx = (int)(long)lv_event_get_user_data(e);
    selected_period = (period_t)idx;
    nav_offset = 0;                       /* a new period starts at the present */
    for (int i = 0; i < 5; i++) style_period_tab(i, i == idx);
    reload_and_render();
}
static void on_nav_tap(lv_event_t * e) {
    int dir = (int)(long)lv_event_get_user_data(e);   /* +1 = older, -1 = newer */
    nav_offset += dir;
    if (nav_offset < 0) nav_offset = 0;
    reload_and_render();
}

lv_obj_t * screen_stats_create(void) {
    if (scr_root) {
        reload_and_render();
        return scr_root;
    }
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x0f1a2a), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Header */
    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, 140, 70);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x223344), 0);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_set_ext_click_area(back, 20);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_label_set_text(bl, "< Back");
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(bl, SF(22), 0);
    lv_obj_center(bl);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, "Statistics");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 180, 26);

    /* Metric tabs row — fewer tabs (no CO2/TVOC) on Toon 1, wider spacing. */
    for (int i = 0; i < N_METRIC; i++) {
        lv_obj_t * t = lv_obj_create(scr_root);
        int tw = SX(N_METRIC <= 4 ? 225 : 150);
        int tx = SX(N_METRIC <= 4 ? 26 + i * 240 : 26 + i * 163);
        lv_obj_set_size(t, tw, SY(56));
        lv_obj_set_pos(t, tx, SY(100));
        lv_obj_set_style_radius(t, 12, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, on_metric_tap, LV_EVENT_CLICKED, (void *)(long)i);
        lv_obj_t * lbl = lv_label_create(t);
        lv_label_set_text(lbl, metrics[i].label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(lbl, SF(18), 0);  /* 6 tabs → smaller */
        lv_obj_center(lbl);
        tab_metric_btns[i] = t;
        style_metric_tab(i, i == selected_metric);
    }

    /* Period tabs row */
    const char * periods[] = {"Hour", "Day", "Week", "Month", "Year"};
    for (int i = 0; i < 5; i++) {
        lv_obj_t * t = lv_obj_create(scr_root);
        lv_obj_set_size(t, SX(184), SY(46));
        lv_obj_set_pos(t, SX(30 + i * 196), SY(170));
        lv_obj_set_style_radius(t, 10, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, on_period_tap, LV_EVENT_CLICKED, (void *)(long)i);
        lv_obj_t * lbl = lv_label_create(t);
        lv_label_set_text(lbl, periods[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(lbl, SF(18), 0);
        lv_obj_center(lbl);
        tab_period_btns[i] = t;
        style_period_tab(i, i == (int)selected_period);
    }

    /* Headline */
    lbl_metric_name = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_metric_name, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_metric_name, SF(22), 0);
    lv_label_set_text(lbl_metric_name, "Electricity");
    lv_obj_align(lbl_metric_name, LV_ALIGN_TOP_LEFT, 30, SY(235));

    /* Repurposed as the "Current" / "Period total" caption above the
     * value. Was rendering a duplicated unit ("W" under "369.0 W"). */
    lbl_unit = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_unit, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_unit, SF(18), 0);
    lv_label_set_text(lbl_unit, "Current");
    lv_obj_align(lbl_unit, LV_ALIGN_TOP_LEFT, 30, SY(265));

    lbl_value = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_value, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_value, SF(48), 0);
    lv_label_set_text(lbl_value, "--");
    lv_obj_align(lbl_value, LV_ALIGN_TOP_LEFT, 30, SY(290));

    /* Chart */
    chart = lv_chart_create(scr_root);
    lv_obj_set_size(chart, SX(700), SY(300));
    lv_obj_align(chart, LV_ALIGN_TOP_RIGHT, SX(-30), SY(235));
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
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x1a2a44), 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x335577), 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_radius(chart, 12, 0);
    cs = lv_chart_add_series(chart, lv_color_hex(metrics[0].color),
                             LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_add_event_cb(chart, chart_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    /* ← / → paging arrows (Day & Week only), with a "now / -Nd" hint between. */
    nav_l = lv_btn_create(scr_root);
    lv_obj_set_size(nav_l, SX(64), SY(54));
    lv_obj_set_pos(nav_l, 30, SY(366));
    lv_obj_set_style_bg_color(nav_l, lv_color_hex(0x2a4060), 0);
    lv_obj_set_ext_click_area(nav_l, 12);
    lv_obj_add_event_cb(nav_l, on_nav_tap, LV_EVENT_CLICKED, (void *)(long)1);
    lv_obj_t * ll = lv_label_create(nav_l);
    lv_label_set_text(ll, LV_SYMBOL_LEFT);
    lv_obj_center(ll);

    lbl_nav = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_nav, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_nav, SF(16), 0);
    lv_obj_set_width(lbl_nav, SX(70));
    lv_obj_set_style_text_align(lbl_nav, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_nav, "now");
    lv_obj_set_pos(lbl_nav, 30 + SX(70), SY(382));

    nav_r = lv_btn_create(scr_root);
    lv_obj_set_size(nav_r, SX(64), SY(54));
    lv_obj_set_pos(nav_r, 30 + SX(146), SY(366));
    lv_obj_set_style_bg_color(nav_r, lv_color_hex(0x2a4060), 0);
    lv_obj_set_ext_click_area(nav_r, 12);
    lv_obj_add_event_cb(nav_r, on_nav_tap, LV_EVENT_CLICKED, (void *)(long)-1);
    lv_obj_t * rl = lv_label_create(nav_r);
    lv_label_set_text(rl, LV_SYMBOL_RIGHT);
    lv_obj_center(rl);

    /* Manual X-axis label widgets under the chart. */
    for (int i = 0; i < XLBL_COUNT; i++) {
        g_xlbl[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_color(g_xlbl[i], lv_color_hex(0x88aabb), 0);
        lv_obj_set_style_text_font(g_xlbl[i], SF(14), 0);
        lv_label_set_text(g_xlbl[i], "");
        lv_obj_add_flag(g_xlbl[i], LV_OBJ_FLAG_HIDDEN);
    }

    reload_and_render();
    return scr_root;
}
