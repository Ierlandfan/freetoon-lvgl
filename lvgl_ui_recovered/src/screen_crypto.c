/*
 * screen_crypto.c — per-coin price-history graphs for the Crypto integration.
 * Opened by tapping the crypto tile. Reads the provider's pre-fetched series
 * files (hist/<id>_<days>.tsv, ts<TAB>price) and plots them in an lv_chart,
 * with a coin selector (the configured coins) and timeframe buttons
 * (24h/7d/30d/1y/max). No network here — the provider does the fetching.
 *
 * Prices are scaled into the chart's lv_coord_t range (BTC ~62900 would
 * overflow int16), and the real min/max/last are shown as text.
 */
#include "screens.h"
#include "display.h"   /* SX()/SY() scaling for Toon 1 (800x480) vs Toon 2 (1024x600) */
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CRYPTO_CONF "/mnt/data/integrations/crypto/crypto.conf"
#define HIST_DIR    "/mnt/data/integrations/crypto/hist"
#define MAX_COINS   5
#define MAX_PTS     240

static lv_obj_t * scr_root;
static lv_obj_t * chart;
static lv_chart_series_t * cs;
static lv_obj_t * lbl_title;
static lv_obj_t * lbl_hi, * lbl_lo;
static lv_obj_t * coin_btns[MAX_COINS];
static lv_obj_t * tf_btns[5];

static char coin_id[MAX_COINS][64];
static char coin_sym[MAX_COINS][24];
static int  coin_n = 0;
static int  cur_coin = 0;
static int  cur_tf = 1;   /* default 7d */

static const char * const TF_DAYS[5]  = { "1", "7", "30", "365", "max" };
/* TF_LABEL is populated at runtime so the timeframe chips follow the language. */
static const char * TF_LABEL[5];
static void init_tf_labels(void) {
    TF_LABEL[0] = tr("24u", "24h");
    TF_LABEL[1] = tr("7d", "7d");
    TF_LABEL[2] = tr("30d", "30d");
    TF_LABEL[3] = tr("1j", "1y");
    TF_LABEL[4] = tr("max", "max");
}

/* compact price like the tile: 62.9k / 1.23M / 0.42 */
static void fmt_price(double v, char * out, size_t osz) {
    if (v >= 1e6)      snprintf(out, osz, "%.2fM", v / 1e6);
    else if (v >= 1e3) snprintf(out, osz, "%.1fk", v / 1e3);
    else if (v >= 1)   snprintf(out, osz, "%.0f", v);
    else               snprintf(out, osz, "%.4f", v);
}

static void load_coins(void) {
    coin_n = 0;
    FILE * f = fopen(CRYPTO_CONF, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && coin_n < MAX_COINS) {
        if (strncmp(line, "COIN=", 5) != 0) continue;
        char * v = line + 5; char * nl = strchr(v, '\n'); if (nl) *nl = 0;
        char * c1 = strchr(v, ','); if (c1) *c1 = 0;
        snprintf(coin_id[coin_n], sizeof coin_id[0], "%s", v);
        char sym[24] = "";
        if (c1) { char * c2 = strchr(c1 + 1, ','); if (c2) *c2 = 0; snprintf(sym, sizeof sym, "%s", c1 + 1); }
        snprintf(coin_sym[coin_n], sizeof coin_sym[0], "%s", sym[0] ? sym : coin_id[coin_n]);
        coin_n++;
    }
    fclose(f);
}

/* Load the selected coin/timeframe series into the chart (scaled). */
static void rebuild_chart(void) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s_%s.tsv", HIST_DIR, coin_id[cur_coin], TF_DAYS[cur_tf]);
    double price[MAX_PTS]; int n = 0;
    double mn = 1e18, mx = -1e18, last = 0;
    FILE * f = fopen(path, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f) && n < MAX_PTS) {
            char * tab = strchr(line, '\t'); if (!tab) continue;
            double p = strtod(tab + 1, NULL);
            price[n++] = p; last = p;
            if (p < mn) mn = p; if (p > mx) mx = p;
        }
        fclose(f);
    }

    char ttl[96], hi[32], lo[32], lc[32];
    if (n == 0) {
        lv_label_set_text_fmt(lbl_title, tr("%s — geen data (nog aan het ophalen…)",
                                            "%s — no data (still fetching…)"), coin_sym[cur_coin]);
        lv_label_set_text(lbl_hi, ""); lv_label_set_text(lbl_lo, "");
        lv_chart_set_point_count(chart, 1);
        cs->y_points[0] = 0; lv_chart_refresh(chart);
        return;
    }
    fmt_price(last, lc, sizeof lc);
    snprintf(ttl, sizeof ttl, "%s   EUR %s   (%s)", coin_sym[cur_coin], lc, TF_LABEL[cur_tf]);
    lv_label_set_text(lbl_title, ttl);
    fmt_price(mx, hi, sizeof hi); fmt_price(mn, lo, sizeof lo);
    lv_label_set_text_fmt(lbl_hi, tr("hoog %s", "high %s"), hi);
    lv_label_set_text_fmt(lbl_lo, tr("laag %s", "low %s"), lo);

    /* Scale into 0..1000 of the (min..max) band so any price magnitude fits. */
    double span = (mx - mn); if (span < 1e-9) span = 1;
    lv_chart_set_point_count(chart, n);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    for (int i = 0; i < n; i++)
        cs->y_points[i] = (lv_coord_t)((price[i] - mn) / span * 1000.0);
    lv_chart_refresh(chart);
}

static void style_selected(lv_obj_t ** arr, int n, int sel) {
    for (int i = 0; i < n; i++) {
        if (!arr[i]) continue;
        lv_obj_set_style_bg_color(arr[i], lv_color_hex(i == sel ? 0x2e6e9e : 0x33415c), 0);
    }
}

static void on_coin(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    cur_coin = i; style_selected(coin_btns, coin_n, cur_coin); rebuild_chart();
}
static void on_tf(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    cur_tf = i; style_selected(tf_btns, 5, cur_tf); rebuild_chart();
}
static void on_back(lv_event_t * e) { (void)e; ui_pop(); }

static lv_obj_t * chip(lv_obj_t * parent, const char * txt, lv_event_cb_t cb, int idx, int x, int y) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, SX(96), SY(40));
    lv_obj_set_pos(b, SX(x), SY(y));
    lv_obj_set_style_bg_color(b, lv_color_hex(0x33415c), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_t * l = lv_label_create(b); lv_label_set_text(l, txt); lv_obj_center(l);
    return b;
}

lv_obj_t * screen_crypto_create(void) {
    init_tf_labels();
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x101418), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, SX(8), SY(8));
    lv_obj_t * bl = lv_label_create(back); lv_label_set_text(bl, tr(LV_SYMBOL_LEFT " Terug", LV_SYMBOL_LEFT " Back"));
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_title, SF(22), 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xffffff), 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, SY(16));

    load_coins();

    /* coin selector row */
    for (int i = 0; i < coin_n; i++)
        coin_btns[i] = chip(scr_root, coin_sym[i], on_coin, i, 8 + i * 102, 58);
    style_selected(coin_btns, coin_n, cur_coin);

    /* timeframe row */
    for (int i = 0; i < 5; i++)
        tf_btns[i] = chip(scr_root, TF_LABEL[i], on_tf, i, 8 + i * 102, 104);
    style_selected(tf_btns, 5, cur_tf);

    chart = lv_chart_create(scr_root);
    lv_obj_set_size(chart, LV_PCT(92), SY(300));
    lv_obj_align(chart, LV_ALIGN_TOP_MID, 0, SY(154));
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart, 5, 0);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x1a2a44), 0);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);   /* no point dots */
    cs = lv_chart_add_series(chart, lv_color_hex(0xf7931a), LV_CHART_AXIS_PRIMARY_Y);

    lbl_hi = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_hi, lv_color_hex(0x88dd66), 0);
    lv_label_set_text(lbl_hi, "");
    lv_obj_align_to(lbl_hi, chart, LV_ALIGN_OUT_TOP_RIGHT, 0, -2);
    lbl_lo = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_lo, lv_color_hex(0xff8866), 0);
    lv_label_set_text(lbl_lo, "");
    lv_obj_align_to(lbl_lo, chart, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);

    if (coin_n == 0) lv_label_set_text(lbl_title, tr("Geen munten gekozen — Instellingen → Crypto",
                                                     "No coins selected — Settings → Crypto"));
    else rebuild_chart();
    return scr_root;
}
