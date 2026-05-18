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
#include "stats.h"
#include "homewizard.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static lv_obj_t * scr_root = NULL;
static lv_obj_t * chart;
static lv_chart_series_t * cs;
static lv_obj_t * lbl_metric_name;
static lv_obj_t * lbl_unit;
static lv_obj_t * lbl_value;
static lv_obj_t * tab_metric_btns[3];
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
} metric_t;
static const metric_t metrics[3] = {
    {"Electricity", "W",     "kWh",  "elec_flow",  "elec_quantity_nt", "elec_quantity_lt", 0xaa77ff},
    {"Gas",         "l/h",   "m3",   "gas_flow",   "gas_quantity",     NULL,                0xffaa44},
    {"Water",       "l/min", "m3",   "water_flow", "water_quantity",   NULL,                0x44aaff},
};

static int      selected_metric = 0;
static period_t selected_period = PERIOD_HOUR;
static stats_series_t  series;
static stats_series_t  series2;   /* for elec second tariff */

static void on_back(lv_event_t * e) { (void)e; ui_pop(); }

/* Per-period: which logger + rra to use, and whether to diff-aggregate.
   Returns 0 if loaded. */
static int load_for_period(void) {
    const metric_t * m = &metrics[selected_metric];
    switch (selected_period) {
        case PERIOD_HOUR:
        case PERIOD_DAY:
            return stats_fetch(m->flow_logger, "5min", &series);
        case PERIOD_WEEK:
            /* Cumulative meter, hourly samples — chart shows raw cumulative.
               TODO: diff adjacent samples for "per-hour usage". */
            if (m->cum_logger_extra) {
                stats_fetch(m->cum_logger_extra, "5yrhours", &series2);
            } else {
                series2.n = 0;
            }
            return stats_fetch(m->cum_logger, "5yrhours", &series);
        case PERIOD_MONTH:
        case PERIOD_YEAR:
            if (m->cum_logger_extra) {
                stats_fetch(m->cum_logger_extra, "10yrdays", &series2);
            } else {
                series2.n = 0;
            }
            return stats_fetch(m->cum_logger, "10yrdays", &series);
    }
    return -1;
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

static void render_chart(void) {
    int n = series.n > STATS_MAX_SAMPLES ? STATS_MAX_SAMPLES : series.n;
    if (n < 2) n = 2;

    /* For elec week/month/year, merge nt + lt by adding into a working
       copy of `series`. */
    if (selected_metric == 0 &&
        (selected_period == PERIOD_WEEK || selected_period == PERIOD_MONTH ||
         selected_period == PERIOD_YEAR) && series2.n == series.n) {
        for (int i = 0; i < series.n; i++) {
            if (!isnan(series.samples[i]) && !isnan(series2.samples[i]))
                series.samples[i] += series2.samples[i];
        }
    }

    /* Convert cumulative meter samples to deltas (per-bin usage). The raw
       value is a monotonically increasing cumulative reading. */
    if (selected_period == PERIOD_WEEK || selected_period == PERIOD_MONTH ||
        selected_period == PERIOD_YEAR) {
        double scale = (selected_metric == 0) ? 0.001 : 0.001;  /* kWh or m3 */
        for (int i = n - 1; i > 0; i--) {
            if (!isnan(series.samples[i]) && !isnan(series.samples[i-1]) &&
                series.samples[i] > series.samples[i-1]) {
                series.samples[i] = (series.samples[i] - series.samples[i-1]) * scale;
            } else {
                series.samples[i] = NAN;
            }
        }
        series.samples[0] = NAN;
    }

    /* Recompute min/max after potential diff. */
    double lo = +1e30, hi = -1e30;
    for (int i = 0; i < n; i++) {
        double v = series.samples[i];
        if (!isnan(v)) {
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    if (lo > hi) { lo = 0; hi = 1; }
    if (hi - lo < 0.5) hi = lo + 1.0;
    double pad = (hi - lo) * 0.1;

    /* Compact non-NaN samples so the chart's X axis maps to "real data
     * only". Without this, RRD's leading NaN gap (no data older than ~3 h
     * for hour view) ate ~90 % of the chart width and the visible trace
     * collapsed into the right edge. */
    static double comp[STATS_MAX_SAMPLES];
    int cn = 0;
    for (int i = 0; i < n; i++) {
        double v = series.samples[i];
        if (!isnan(v)) comp[cn++] = v;
    }
    if (cn < 2) cn = 2;

    lv_chart_set_point_count(chart, cn);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y,
                       (lv_coord_t)(lo - pad), (lv_coord_t)(hi + pad));
    for (int i = 0; i < cn; i++) {
        cs->y_points[i] = (lv_coord_t)comp[i];
    }
    lv_chart_refresh(chart);

    /* Caption above the value: "Current" for live periods, "Period total"
     * for cumulative ones. Replaces the duplicated unit string. */
    const metric_t * m = &metrics[selected_metric];
    if (selected_period == PERIOD_HOUR || selected_period == PERIOD_DAY) {
        lv_label_set_text(lbl_unit, "Current");
        double cur = NAN;
        if (selected_metric == 0)      cur = hw_state.power_w;
        else if (selected_metric == 1) cur = hw_state.water_lpm * 0;  /* gas flow not from HW */
        else if (selected_metric == 2) cur = hw_state.water_lpm;
        if (isnan(cur) || cur == 0) {
            for (int i = series.n - 1; i >= 0; i--)
                if (!isnan(series.samples[i])) { cur = series.samples[i]; break; }
        }
        if (isnan(cur)) lv_label_set_text(lbl_value, "--");
        else lv_label_set_text_fmt(lbl_value, "%.1f %s", cur, m->unit_flow);
    } else {
        lv_label_set_text(lbl_unit, "Period total");
        /* If every sample is NaN, the *_quantity_* loggers are wired but
         * empty (only *_flow* is being written by p1bridge). Show that
         * explicitly instead of "0.0 kWh". */
        if (cn < 2) {
            lv_label_set_text(lbl_value, "no data");
        } else {
            double total = 0;
            for (int i = 0; i < n; i++)
                if (!isnan(series.samples[i])) total += series.samples[i];
            lv_label_set_text_fmt(lbl_value, "%.1f %s", total, m->unit_cum);
        }
    }
}

static void reload_and_render(void) {
    lv_chart_set_series_color(chart, cs, lv_color_hex(metrics[selected_metric].color));
    lv_label_set_text(lbl_metric_name, metrics[selected_metric].label);
    load_for_period();
    render_chart();
}

static void on_metric_tap(lv_event_t * e) {
    int idx = (int)(long)lv_event_get_user_data(e);
    selected_metric = idx;
    for (int i = 0; i < 3; i++) style_metric_tab(i, i == idx);
    reload_and_render();
}
static void on_period_tap(lv_event_t * e) {
    int idx = (int)(long)lv_event_get_user_data(e);
    selected_period = (period_t)idx;
    for (int i = 0; i < 5; i++) style_period_tab(i, i == idx);
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
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_22, 0);
    lv_obj_center(bl);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_label_set_text(title, "Statistics");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 180, 26);

    /* Metric tabs row */
    for (int i = 0; i < 3; i++) {
        lv_obj_t * t = lv_obj_create(scr_root);
        lv_obj_set_size(t, 200, 56);
        lv_obj_set_pos(t, 30 + i * 220, 100);
        lv_obj_set_style_radius(t, 12, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, on_metric_tap, LV_EVENT_CLICKED, (void *)(long)i);
        lv_obj_t * lbl = lv_label_create(t);
        lv_label_set_text(lbl, metrics[i].label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
        lv_obj_center(lbl);
        tab_metric_btns[i] = t;
        style_metric_tab(i, i == selected_metric);
    }

    /* Period tabs row */
    const char * periods[] = {"Hour", "Day", "Week", "Month", "Year"};
    for (int i = 0; i < 5; i++) {
        lv_obj_t * t = lv_obj_create(scr_root);
        lv_obj_set_size(t, 184, 46);
        lv_obj_set_pos(t, 30 + i * 196, 170);
        lv_obj_set_style_radius(t, 10, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, on_period_tap, LV_EVENT_CLICKED, (void *)(long)i);
        lv_obj_t * lbl = lv_label_create(t);
        lv_label_set_text(lbl, periods[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        lv_obj_center(lbl);
        tab_period_btns[i] = t;
        style_period_tab(i, i == (int)selected_period);
    }

    /* Headline */
    lbl_metric_name = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_metric_name, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_metric_name, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_metric_name, "Electricity");
    lv_obj_align(lbl_metric_name, LV_ALIGN_TOP_LEFT, 30, 235);

    /* Repurposed as the "Current" / "Period total" caption above the
     * value. Was rendering a duplicated unit ("W" under "369.0 W"). */
    lbl_unit = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_unit, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_unit, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_unit, "Current");
    lv_obj_align(lbl_unit, LV_ALIGN_TOP_LEFT, 30, 265);

    lbl_value = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_value, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_value, &lv_font_montserrat_48, 0);
    lv_label_set_text(lbl_value, "--");
    lv_obj_align(lbl_value, LV_ALIGN_TOP_LEFT, 30, 290);

    /* Chart */
    chart = lv_chart_create(scr_root);
    lv_obj_set_size(chart, 700, 300);
    lv_obj_align(chart, LV_ALIGN_TOP_RIGHT, -30, 235);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart, 5, 8);
    /* 512 samples across 700 px → ~1.4 px/sample. Default 3-px dots overlap
     * into vertical bars that look like a scatter mess. Drop the per-point
     * marker, thicken the connecting line so it reads as a graph. */
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    /* Y axis tick labels — 5 major divisions, 2 minor in between, draw_size
     * 60 reserves the space LVGL needs for the rendered numbers. */
    lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_Y, 8, 4, 5, 2, true, 60);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x1a2a44), 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x335577), 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_radius(chart, 12, 0);
    cs = lv_chart_add_series(chart, lv_color_hex(metrics[0].color),
                             LV_CHART_AXIS_PRIMARY_Y);

    reload_and_render();
    return scr_root;
}
