/*
 * screen_energy_timeline.c — Energy → Device timeline
 *
 * Gantt-style canvas showing when each recognised appliance was on/off
 * over the last 1/2/4 hours.  Data comes from the nilm_log ring buffer
 * in meteradapter.c (every step-change event, named + unnamed).
 *
 * Layout:
 *
 *   [← Back]       Apparaten timeline          [1u] [2u] [4u]
 *   ──────────────────────────────────────────────────────────
 *   Fridge        |  ██████  |    █████  |              |
 *   CV boiler     |          |  ██       |              |
 *   TV / Decoder  |          |           |  ████████    |
 *   ~82 W         |  ██      |           |              |
 *   ──────────────────────────────────────────────────────────
 *   [time axis]  -4h        -2h         -1h           now
 *
 * A bar starts at every ON event and ends at the next OFF for the same
 * device (or extends to "now" if no OFF has been seen yet).
 * Standalone OFF events (power strip, device switched off externally)
 * are shown as a short downward tick.
 */

#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "meteradapter.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── layout constants (at 1024×600 design resolution) ─────────── */
#define LABEL_W  SX(190)   /* device-name column width */
#define ROW_H    SY(44)    /* height of each device row */
#define HDR_H    SY(32)    /* time-axis row at top of canvas */
#define MAX_DEVS 10        /* max rows shown */

/* Canvas buffer — covers the full timeline area */
#define TL_BUF_W  1024
#define TL_BUF_H  512
static lv_color_t tl_buf[TL_BUF_W * TL_BUF_H];

/* ── colours ───────────────────────────────────────────────────── */
#define COL_BG       0x0e1a2a
#define COL_CANVAS   0x0a1520
#define COL_TEXT_HI  0xffffff
#define COL_TEXT_DIM 0x88aabb
#define COL_GRID     0x1e3248
#define COL_NOW      0xffffff
#define COL_TICK_OFF 0x445566

/* 8-colour device palette */
static const uint32_t dev_palette[] = {
    0x3a9fd4, 0x36c46b, 0xd9a23a, 0xaa77ff,
    0xd4574f, 0x44cccc, 0xcc6688, 0x99bb44,
};
#define N_COLORS 8

/* ── screen state ─────────────────────────────────────────────── */
static lv_obj_t   * scr_root    = NULL;
static lv_obj_t   * canvas      = NULL;
static lv_obj_t   * label_cont  = NULL;   /* left column label objects */
static lv_timer_t * refresh_tmr = NULL;
static int          g_range_h   = 2;      /* selected hours: 1/2/4 */
static lv_obj_t   * btn_range[3]= {0};
static int          g_last_log  = -1;

/* ── device row tracking ──────────────────────────────────────── */
typedef struct {
    char    name[40];
    uint32_t color;
} dev_row_t;
static dev_row_t g_rows[MAX_DEVS];
static int       g_nrows = 0;

static uint32_t dev_color(const char *name) {
    unsigned h = 5381;
    for (const char *p = name; *p; p++) h = h * 33 ^ (unsigned char)*p;
    return dev_palette[h % N_COLORS];
}

/* ── helpers ──────────────────────────────────────────────────── */
static int ts_to_x(time_t ts, time_t t_start, long range_s, int canvas_w) {
    long off = (long)(ts - t_start);
    if (off < 0) off = 0;
    if (off > range_s) off = range_s;
    return (int)(off * canvas_w / range_s);
}

/* Find or insert a device row; returns row index, -1 if table full */
static int find_or_add_row(const char *name) {
    for (int i = 0; i < g_nrows; i++)
        if (strcmp(g_rows[i].name, name) == 0) return i;
    if (g_nrows >= MAX_DEVS) return -1;
    strncpy(g_rows[g_nrows].name, name, 39);
    g_rows[g_nrows].color = dev_color(name);
    return g_nrows++;
}

/* ── canvas redraw ─────────────────────────────────────────────── */
static void redraw_canvas(void) {
    if (!canvas) return;
    int cw = lv_obj_get_width(canvas);
    int ch = lv_obj_get_height(canvas);

    lv_canvas_fill_bg(canvas, lv_color_hex(COL_CANVAS), LV_OPA_COVER);

    time_t t_end   = time(NULL);
    time_t t_start = t_end - (time_t)(g_range_h * 3600);
    long   range_s = (long)(t_end - t_start);

    /* ── discover device rows from log within window ── */
    g_nrows = 0;
    int total = nilm_log_count < NILM_LOG_MAX ? nilm_log_count : NILM_LOG_MAX;
    /* walk oldest-first so first-seen order = top row */
    int oldest = nilm_log_count > NILM_LOG_MAX ? nilm_log_count - NILM_LOG_MAX : 0;
    for (int i = oldest; i < nilm_log_count; i++) {
        nilm_event_t *ev = &nilm_log[i % NILM_LOG_MAX];
        if (ev->ts < t_start) continue;
        find_or_add_row(ev->device);
    }
    (void)total;

    /* ── row background bands ── */
    for (int r = 0; r < g_nrows; r++) {
        lv_draw_rect_dsc_t bd; lv_draw_rect_dsc_init(&bd);
        bd.bg_color = lv_color_hex(r & 1 ? 0x0f1e30 : COL_CANVAS);
        bd.bg_opa   = LV_OPA_COVER;
        bd.radius   = 0;
        bd.border_width = 0;
        lv_canvas_draw_rect(canvas, 0,
            HDR_H + r * ROW_H, cw, ROW_H, &bd);
    }

    /* ── time-axis grid lines + labels ── */
    int n_ticks = g_range_h <= 1 ? 6 : g_range_h <= 2 ? 8 : 8;
    for (int t = 0; t <= n_ticks; t++) {
        int x = t * cw / n_ticks;
        lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
        ld.color = lv_color_hex(COL_GRID);
        ld.width = 1;
        lv_point_t grid_pts[2] = {{x, HDR_H}, {x, ch}};
        lv_canvas_draw_line(canvas, grid_pts, 2, &ld);

        /* time label */
        time_t tick_ts = t_start + (time_t)((long)t * range_s / n_ticks);
        struct tm *tm = localtime(&tick_ts);
        char lbuf[8];
        strftime(lbuf, sizeof lbuf, "%H:%M", tm);
        lv_draw_label_dsc_t tld; lv_draw_label_dsc_init(&tld);
        tld.color = lv_color_hex(COL_TEXT_DIM);
        tld.font  = SF(12);
        lv_canvas_draw_text(canvas, x > 4 ? x - SX(22) : 1, 4, SX(44), &tld, lbuf);
    }

    /* ── bars for each device ── */
    lv_draw_rect_dsc_t br; lv_draw_rect_dsc_init(&br);
    br.bg_opa = LV_OPA_COVER;
    br.radius = 3;
    br.border_width = 0;

    lv_draw_rect_dsc_t tick_d; lv_draw_rect_dsc_init(&tick_d);
    tick_d.bg_color = lv_color_hex(COL_TICK_OFF);
    tick_d.bg_opa = LV_OPA_COVER;
    tick_d.radius = 2;
    tick_d.border_width = 0;

    for (int r = 0; r < g_nrows; r++) {
        int bar_y  = HDR_H + r * ROW_H + SY(6);
        int bar_h  = ROW_H - SY(12);
        br.bg_color = lv_color_hex(g_rows[r].color);

        /* Collect events for this device, oldest first, within window.
         * Walk and pair ON/OFF: ON opens a span, OFF closes it. */
        time_t span_start = 0;  /* non-zero = device currently on */

        for (int i = oldest; i < nilm_log_count; i++) {
            nilm_event_t *ev = &nilm_log[i % NILM_LOG_MAX];
            if (strcmp(ev->device, g_rows[r].name) != 0) continue;

            if (ev->direction == +1) {
                /* ON: close any unclosed span first (missed OFF), then open new */
                if (span_start && ev->ts > t_start) {
                    int x0 = ts_to_x(span_start, t_start, range_s, cw);
                    int x1 = ts_to_x(ev->ts,     t_start, range_s, cw);
                    if (x1 > x0)
                        lv_canvas_draw_rect(canvas, x0, bar_y, x1 - x0, bar_h, &br);
                }
                span_start = ev->ts < t_start ? t_start : ev->ts;
            } else {
                /* OFF */
                if (span_start) {
                    int x0 = ts_to_x(span_start, t_start, range_s, cw);
                    int x1 = ts_to_x(ev->ts,     t_start, range_s, cw);
                    if (x1 > x0)
                        lv_canvas_draw_rect(canvas, x0, bar_y, x1 - x0, bar_h, &br);
                    span_start = 0;
                } else if (ev->ts >= t_start) {
                    /* Standalone OFF tick */
                    int x = ts_to_x(ev->ts, t_start, range_s, cw);
                    lv_canvas_draw_rect(canvas, x > 2 ? x - 2 : 0,
                                        bar_y, 4, bar_h, &tick_d);
                }
            }
        }

        /* Unclosed span = device still on: extend to now */
        if (span_start) {
            int x0 = ts_to_x(span_start, t_start, range_s, cw);
            lv_draw_rect_dsc_t pulsing = br;
            pulsing.bg_opa = LV_OPA_80;  /* slightly dimmer "live" bar */
            lv_canvas_draw_rect(canvas, x0, bar_y, cw - x0, bar_h, &pulsing);
        }
    }

    /* ── "now" line ── */
    {
        lv_draw_line_dsc_t nl; lv_draw_line_dsc_init(&nl);
        nl.color = lv_color_hex(COL_NOW);
        nl.width = 2;
        lv_point_t now_pts[2] = {{cw - 1, HDR_H}, {cw - 1, ch}};
        lv_canvas_draw_line(canvas, now_pts, 2, &nl);
    }

    /* ── empty state ── */
    if (g_nrows == 0) {
        lv_draw_label_dsc_t eld; lv_draw_label_dsc_init(&eld);
        eld.color = lv_color_hex(COL_TEXT_DIM);
        eld.font  = SF(18);
        const char *msg = meter_state.connected
            ? tr("Geen stap-detectie events in dit tijdvenster.",
                 "No step-change events in this time window.")
            : tr("P1 meter offline.", "P1 meter offline.");
        lv_canvas_draw_text(canvas, SX(20), SY(40), cw - SX(40), &eld, msg);
    }

    /* ── rebuild label column ── */
    if (label_cont) lv_obj_clean(label_cont);
    for (int r = 0; r < g_nrows; r++) {
        int y = HDR_H + r * ROW_H + (ROW_H - SY(20)) / 2;
        lv_obj_t *dot = lv_obj_create(label_cont);
        lv_obj_set_size(dot, SX(10), SY(10));
        lv_obj_set_pos(dot, SX(6), y + SY(5));
        lv_obj_set_style_bg_color(dot, lv_color_hex(g_rows[r].color), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_radius(dot, 5, 0);

        lv_obj_t *lbl = lv_label_create(label_cont);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_HI), 0);
        lv_obj_set_style_text_font(lbl, SF(15), 0);
        lv_obj_set_width(lbl, LABEL_W - SX(24));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_label_set_text(lbl, g_rows[r].name);
        lv_obj_set_pos(lbl, SX(20), y);
    }
}

/* ── refresh timer ─────────────────────────────────────────────── */
static void refresh_cb(lv_timer_t *t) {
    (void)t;
    if (nilm_log_count != g_last_log) {
        g_last_log = nilm_log_count;
        redraw_canvas();
    }
}

/* ── range buttons ─────────────────────────────────────────────── */
static void style_range_btn(int idx, int sel) {
    lv_obj_set_style_bg_color(btn_range[idx],
        lv_color_hex(sel ? 0x3a6ea8 : 0x1e3050), 0);
    lv_obj_set_style_border_width(btn_range[idx], sel ? 2 : 0, 0);
    lv_obj_set_style_border_color(btn_range[idx], lv_color_hex(0x5090d0), 0);
}

static void on_range(lv_event_t *e) {
    int h = (int)(intptr_t)lv_event_get_user_data(e);
    g_range_h = h;
    for (int i = 0; i < 3; i++)
        style_range_btn(i, (int[]){1,2,4}[i] == h);
    g_last_log = -1;
    redraw_canvas();
}

/* ── screen event ──────────────────────────────────────────────── */
static void on_scr_event(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) {
        g_last_log = -1;
        redraw_canvas();
        if (refresh_tmr) lv_timer_resume(refresh_tmr);
    } else if (c == LV_EVENT_SCREEN_UNLOADED) {
        if (refresh_tmr) lv_timer_pause(refresh_tmr);
    }
}

static void back_async(void *u) { (void)u; ui_pop(); }
static void on_back(lv_event_t *e) { (void)e; lv_async_call(back_async, NULL); }

/* ── screen builder ────────────────────────────────────────────── */
lv_obj_t * screen_energy_timeline_create(void) {
    if (scr_root) {
        g_last_log = -1;
        redraw_canvas();
        return scr_root;
    }

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_ALL, NULL);

    int hdr_y = SY(8);
    int hdr_h = SY(50);

    /* ── Back button ── */
    lv_obj_t *back = lv_btn_create(scr_root);
    lv_obj_set_size(back, SX(130), SY(44));
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, SX(8), hdr_y + SY(3));
    lv_obj_set_style_bg_color(back, lv_color_hex(0x1e3050), 0);
    lv_obj_set_style_radius(back, 10, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(bl, SF(18), 0);
    lv_label_set_text_fmt(bl, LV_SYMBOL_LEFT " %s", tr("Terug", "Back"));
    lv_obj_center(bl);

    /* ── Title ── */
    lv_obj_t *title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, SF(22), 0);
    lv_label_set_text(title, tr("Apparaten tijdlijn", "Device timeline"));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, hdr_y + SY(12));

    /* ── Range buttons ── */
    const int range_h[] = {1, 2, 4};
    const char *range_lbl[] = {"1u", "2u", "4u"};
    for (int i = 0; i < 3; i++) {
        btn_range[i] = lv_btn_create(scr_root);
        lv_obj_set_size(btn_range[i], SX(64), SY(36));
        lv_obj_align(btn_range[i], LV_ALIGN_TOP_RIGHT,
                     -(SX(74) * (2 - i)) - SX(8), hdr_y + SY(7));
        lv_obj_set_style_radius(btn_range[i], 8, 0);
        lv_obj_set_style_border_width(btn_range[i], 0, 0);
        lv_obj_add_event_cb(btn_range[i], on_range, LV_EVENT_CLICKED,
                            (void *)(intptr_t)range_h[i]);
        lv_obj_t *l = lv_label_create(btn_range[i]);
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(l, SF(16), 0);
        lv_label_set_text(l, range_lbl[i]);
        lv_obj_center(l);
        style_range_btn(i, range_h[i] == g_range_h);
    }

    /* ── Layout: label column + canvas side by side ── */
    int content_y = hdr_y + hdr_h;
    int content_h = DISP_VER - content_y - SY(4);
    int canvas_w  = DISP_HOR - LABEL_W - SX(8);
    int canvas_h  = content_h;
    /* cap to buffer */
    if (canvas_w > TL_BUF_W) canvas_w = TL_BUF_W;
    if (canvas_h > TL_BUF_H) canvas_h = TL_BUF_H;

    /* Label column — receives child labels built by redraw_canvas */
    label_cont = lv_obj_create(scr_root);
    lv_obj_set_size(label_cont, LABEL_W, canvas_h);
    lv_obj_set_pos(label_cont, 0, content_y);
    lv_obj_set_style_bg_color(label_cont, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(label_cont, 0, 0);
    lv_obj_set_style_pad_all(label_cont, 0, 0);
    lv_obj_clear_flag(label_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* Canvas */
    canvas = lv_canvas_create(scr_root);
    lv_canvas_set_buffer(canvas, tl_buf, canvas_w, canvas_h,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(canvas, LABEL_W, content_y);

    /* ── Refresh timer (30 s; also fires on log change) ── */
    if (!refresh_tmr)
        refresh_tmr = lv_timer_create(refresh_cb, 30000, NULL);
    lv_timer_pause(refresh_tmr);

    return scr_root;
}
