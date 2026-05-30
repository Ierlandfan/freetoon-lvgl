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
static const metric_t metrics[6] = {
    {"Electricity", "W",     "kWh",  "elec_flow",  "elec_quantity_nt", "elec_quantity_lt", 0xaa77ff, 0},
    {"Gas",         "l/h",   "m3",   "gas_flow",   "gas_quantity",     NULL,                0xffaa44, 0},
    {"Water",       "l/min", "m3",   "water_flow", "water_quantity",   NULL,                0x44aaff, 0},
    {"CO2",         "ppm",   "ppm",  NULL,         NULL,               NULL,                0x44cc88, 1},
    {"TVOC",        "ppb",   "ppb",  NULL,         NULL,               NULL,                0xcc8844, 1},
    {"CV bar",      "bar",   "bar",  NULL,         NULL,               NULL,                0x5fb3ff, 2},  /* CH pressure, all periods */
};
#define N_METRIC ((int)(sizeof metrics / sizeof metrics[0]))

static int      selected_metric = 0;
static period_t selected_period = PERIOD_HOUR;
static stats_series_t  series;
static stats_series_t  series2;   /* for elec second tariff */

static void on_back(lv_event_t * e) { (void)e; ui_pop(); }

/* Final bar values + labels, produced by render_chart's bucketing step.
 * The chart shows these directly (one bar per bucket). */
static double g_bar_val[64];
static char   g_bar_label[64][8];
static int    g_bar_count = 0;

/* Manual X-axis labels under the chart (lv_chart's own tick labels never
 * rendered reliably for bars). Up to 12 evenly-spaced bar labels. */
#define XLBL_COUNT 12
static lv_obj_t * g_xlbl[XLBL_COUNT] = {0};
/* Chart plot geometry — chart is 700x300 at TOP_RIGHT -30,235; the Y tick
 * labels (draw_size 60) inset the plot on the left. Tuned to bar centers. */
#define PLOT_L SX(360)
#define PLOT_R SX(986)
#define XLBL_Y SY(540)

static void place_x_labels(void) {
    for (int i = 0; i < XLBL_COUNT; i++)
        if (g_xlbl[i]) lv_obj_add_flag(g_xlbl[i], LV_OBJ_FLAG_HIDDEN);
    if (g_bar_count <= 0) return;
    int shown = g_bar_count < XLBL_COUNT ? g_bar_count : XLBL_COUNT;
    double plot_w = PLOT_R - PLOT_L;
    for (int k = 0; k < shown; k++) {
        int b = (g_bar_count <= XLBL_COUNT) ? k
              : (shown > 1 ? k * (g_bar_count - 1) / (shown - 1) : 0);
        double cx = PLOT_L + (b + 0.5) * plot_w / g_bar_count;
        if (!g_xlbl[k]) continue;
        lv_label_set_text(g_xlbl[k], g_bar_label[b]);
        lv_obj_set_width(g_xlbl[k], SX(58));
        lv_obj_set_style_text_align(g_xlbl[k], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(g_xlbl[k], (int)(cx - SX(29)), XLBL_Y);
        lv_obj_clear_flag(g_xlbl[k], LV_OBJ_FLAG_HIDDEN);
    }
}


/* Per-period: which logger + rra + time window + how many samples.
 *
 * Critical: each tab MUST pass an explicit `window_seconds` to
 * stats_fetch so hcb_rrd returns only the trailing slice of the RRA
 * archive. Without it, the archive's full multi-year span is
 * downsampled to N evenly-spaced points — that's the "Week tab
 * showed 21 days" bug. With from/to scoped, samples= just trims
 * within the window.
 *
 *   Tab     Logger        RRA       Window      Samples cap
 *   Hour    flow_logger   5min      1 h         12   (5-min res)
 *   Day     flow_logger   5min      24 h        288  (5-min res)
 *   Week    cum_logger    5yrhours  7 d         168  (hourly res)
 *   Month   cum_logger    10yrdays  31 d        31   (daily res)
 *   Year    cum_logger    10yrdays  365 d       365  (daily res)
 * Returns 0 if loaded. */
static int load_for_period(void) {
    const metric_t * m = &metrics[selected_metric];
    const long HOUR = 3600, DAY = 86400, WEEK = 7*DAY,
               MONTH = 31*DAY, YEAR = 365*DAY;
    if (m->local_hist) {                /* CO2/TVOC/pressure — freetoon's recorder */
        series2.n = 0;
        if (m->local_hist == 2) {       /* CH pressure — hourly ring, all periods */
            long win = (selected_period == PERIOD_HOUR)  ? HOUR
                     : (selected_period == PERIOD_DAY)   ? DAY
                     : (selected_period == PERIOD_WEEK)  ? WEEK
                     : (selected_period == PERIOD_MONTH) ? MONTH : YEAR;
            return airhist_pres_series(win, 365, &series);
        }
        long win = (selected_period == PERIOD_HOUR) ? HOUR
                 : (selected_period == PERIOD_DAY)  ? DAY : WEEK;  /* CO2/TVOC: 7d ring */
        return airhist_series(selected_metric - 3, win, 288, &series);
    }
    switch (selected_period) {
        case PERIOD_HOUR:
            return stats_fetch(m->flow_logger, "5min", HOUR, 12, &series);
        case PERIOD_DAY:
            return stats_fetch(m->flow_logger, "5min", DAY, 288, &series);
        case PERIOD_WEEK:
            /* Cumulative meter, hourly samples — diff'd to per-hour usage
             * in render_chart. 168 hours = 7 days exactly. */
            if (m->cum_logger_extra) {
                stats_fetch(m->cum_logger_extra, "5yrhours", WEEK, 168, &series2);
            } else {
                series2.n = 0;
            }
            return stats_fetch(m->cum_logger, "5yrhours", WEEK, 168, &series);
        case PERIOD_MONTH:
            if (m->cum_logger_extra) {
                stats_fetch(m->cum_logger_extra, "10yrdays", MONTH, 31, &series2);
            } else {
                series2.n = 0;
            }
            return stats_fetch(m->cum_logger, "10yrdays", MONTH, 31, &series);
        case PERIOD_YEAR: {
            /* One-year window at daily resolution. hcb_rrd's socket path
             * returns all-zero for multi-year windows (it spreads the samples
             * too thin over the archive span), so we cap at 365 d / 365 daily
             * samples — the proven-working query. render_chart buckets the
             * daily deltas into one bar per calendar month (labelled mon-yy),
             * so the Year view fills in month-by-month as history accrues.
             * NB: a logger only shows as far back as it actually has data. */
            if (m->cum_logger_extra) {
                stats_fetch(m->cum_logger_extra, "10yrdays", YEAR, 365, &series2);
            } else {
                series2.n = 0;
            }
            return stats_fetch(m->cum_logger, "10yrdays", YEAR, 365, &series);
        }
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

static void render_chart(void) {
    int n = series.n > STATS_MAX_SAMPLES ? STATS_MAX_SAMPLES : series.n;
    if (n < 0) n = 0;

    int is_cum = (selected_period == PERIOD_WEEK ||
                  selected_period == PERIOD_MONTH ||
                  selected_period == PERIOD_YEAR)
                 && !metrics[selected_metric].local_hist;  /* CO2/TVOC are instantaneous */

    /* Cumulative meters → per-sample usage deltas. The elec NT/LT split is
     * diffed separately then summed (the source sometimes returns NT-alone,
     * sometimes NT+LT — diffing before summing avoids a false spike). */
    if (is_cum) {
        double cap = (selected_period == PERIOD_WEEK) ? 10000.0 : 200000.0;
        diff_in_place(&series, n, cap);
        if (selected_metric == 0 && series2.n >= n) {
            diff_in_place(&series2, n, cap);
            for (int i = 0; i < n; i++) {
                if (isnan(series.samples[i]) && !isnan(series2.samples[i]))
                    series.samples[i] = series2.samples[i];
                else if (!isnan(series.samples[i]) && !isnan(series2.samples[i]))
                    series.samples[i] += series2.samples[i];
            }
        }
    }

    /* How many bars this period wants — mimics the original Toon: hourly
     * for Hour/Day, daily for Week/Month, monthly for Year. */
    int want;
    switch (selected_period) {
        case PERIOD_HOUR:  want = 12; break;
        case PERIOD_DAY:   want = 24; break;
        case PERIOD_WEEK:  want = 7;  break;
        case PERIOD_MONTH: want = 31; break;
        case PERIOD_YEAR:  want = 12; break;
        default:           want = 12; break;
    }

    g_bar_count = 0;
    static const char * mon[] = {"","jan","feb","mar","apr","may","jun",
                                 "jul","aug","sep","okt","nov","dec"};

    if (selected_period == PERIOD_YEAR) {
        /* Group consecutive samples by calendar month+year, summing usage →
         * one bar per month across all available history. Label "mon-yy"
         * (e.g. "may-26") so the displayed year is unambiguous. */
        int cur_mm = -1; char cur_yy[3] = "";
        for (int i = 0; i < n && g_bar_count < 64; i++) {
            if (isnan(series.samples[i])) continue;
            const char * lab = series.labels[i];
            int mm = (lab[3]-'0')*10 + (lab[4]-'0');
            const char * yy = series.year2[i];
            if (mm != cur_mm || strncmp(yy, cur_yy, 2) != 0) {
                cur_mm = mm;
                cur_yy[0] = yy[0]; cur_yy[1] = yy[1]; cur_yy[2] = 0;
                g_bar_val[g_bar_count] = 0;
                if (mm >= 1 && mm <= 12 && yy[0])
                    snprintf(g_bar_label[g_bar_count], sizeof g_bar_label[0], "%s-%s", mon[mm], yy);
                else if (mm >= 1 && mm <= 12)
                    snprintf(g_bar_label[g_bar_count], sizeof g_bar_label[0], "%s", mon[mm]);
                else
                    snprintf(g_bar_label[g_bar_count], sizeof g_bar_label[0], "?");
                g_bar_count++;
            }
            g_bar_val[g_bar_count-1] += series.samples[i];
        }
    } else {
        /* Compact real samples, then aggregate into `want` even buckets:
         * average for flow (Hour/Day), sum of deltas for cumulative. */
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
            if (selected_period == PERIOD_HOUR || selected_period == PERIOD_DAY)
                snprintf(g_bar_label[b], sizeof g_bar_label[0], "%.5s", lab + 6); /* HH:MM */
            else
                snprintf(g_bar_label[b], sizeof g_bar_label[0], "%.5s", lab);     /* DD-MM */
            g_bar_count++;
        }
    }

    /* Cumulative bars are summed deltas in Wh/mL — a month easily exceeds
     * 300 000, which overflows lv_coord_t (int16) in lv_chart. Scale to the
     * display unit (kWh/m3) so bar values + the Y range stay in int16 range. */
    if (is_cum)
        for (int i = 0; i < g_bar_count; i++) g_bar_val[i] /= 1000.0;

    /* Y range from 0 baseline (bars) up to ~110% of the tallest bar. */
    double hi = 0;
    for (int i = 0; i < g_bar_count; i++) if (g_bar_val[i] > hi) hi = g_bar_val[i];
    double top = hi * 1.1; if (top < 1) top = 1;

    /* lv_chart's bar draw faults with a single data point, so keep at least
     * two slots — extra slots beyond the real bars are LV_CHART_POINT_NONE
     * (not drawn). Happens for Year when only the current month has history. */
    int pc = g_bar_count < 2 ? 2 : g_bar_count;
    lv_chart_set_point_count(chart, pc);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, (lv_coord_t)top);
    for (int i = 0; i < pc; i++)
        cs->y_points[i] = (i < g_bar_count) ? (lv_coord_t)g_bar_val[i]
                                            : LV_CHART_POINT_NONE;

    lv_chart_refresh(chart);
    place_x_labels();

    /* Caption + headline value. */
    const metric_t * m = &metrics[selected_metric];
    if (!is_cum) {
        lv_label_set_text(lbl_unit, "Current");
        double cur = NAN;
        if (selected_metric == 0)      cur = hw_state.power_w;
        else if (selected_metric == 2) cur = hw_state.water_lpm;
        if (isnan(cur) || cur == 0) {
            for (int i = series.n - 1; i >= 0; i--)
                if (!isnan(series.samples[i])) { cur = series.samples[i]; break; }
        }
        if (isnan(cur)) lv_label_set_text(lbl_value, "--");
        else lv_label_set_text_fmt(lbl_value, "%.1f %s", cur, m->unit_flow);
    } else {
        lv_label_set_text(lbl_unit, "Period total");
        if (g_bar_count == 0) {
            lv_label_set_text(lbl_value, "no data");
        } else {
            double total = 0;   /* g_bar_val already in kWh/m3 for cumulative */
            for (int i = 0; i < g_bar_count; i++) total += g_bar_val[i];
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
    for (int i = 0; i < N_METRIC; i++) style_metric_tab(i, i == idx);
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
    lv_obj_set_style_text_font(bl, SF(22), 0);
    lv_obj_center(bl);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, "Statistics");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 180, 26);

    /* Metric tabs row */
    for (int i = 0; i < N_METRIC; i++) {
        lv_obj_t * t = lv_obj_create(scr_root);
        lv_obj_set_size(t, SX(150), SY(56));
        lv_obj_set_pos(t, SX(26 + i * 163), SY(100));
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
