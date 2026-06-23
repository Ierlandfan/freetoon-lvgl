/*
 * screen_vent_remote.c — Ventilation detail screen
 *
 * Layout:
 *   ← Back                  Ventilatie           Geavanceerd →
 *   status line (speed / rpm / mode)
 *   ─────────────────────────────────────────────────────────
 *   [ Weg ]  [ Laag ]  [ Middel ]  [ Hoog ]
 *           [ Auto ]            [ Timer ]
 *   ─────────────────────────────────────────────────────────
 *   Manual slider  0 ────────●──────── 100 %
 *
 * The active mode button is highlighted with a white border.
 * The slider commits on release via vent_set_speed_async (PWM path).
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "ventilation.h"
#include <stdio.h>
#include <string.h>

/* ── state ─────────────────────────────────────────────────── */
static lv_obj_t  * scr_root      = NULL;
static lv_obj_t  * lbl_status    = NULL;
static lv_obj_t  * slider        = NULL;
static lv_obj_t  * lbl_pct       = NULL;
static lv_obj_t  * lbl_pct_sign  = NULL;
static lv_timer_t* refresh_timer = NULL;
static uint32_t    last_user_ms  = 0;

/* Mode buttons — kept so refresh_cb can highlight the active one */
#define MODE_BTN_COUNT 6
static lv_obj_t  * mode_btns[MODE_BTN_COUNT];
static const char* mode_cmds[MODE_BTN_COUNT] = {
    "away", "low", "medium", "high", "auto", NULL   /* NULL = timer */
};

/* ── helpers ────────────────────────────────────────────────── */
static void on_back(lv_event_t * e)         { (void)e; ui_pop(); }
static void on_open_advanced(lv_event_t * e){ (void)e; ui_push(screen_vent_advanced_create()); }

static void on_mode_btn(lv_event_t * e) {
    const char * cmd = (const char *)lv_event_get_user_data(e);
    if (cmd) {
        vent_send_vremote_async(cmd);
        last_user_ms = lv_tick_get();
    }
}

/* Timer cycles: first tap → timer1, next → timer2, next → timer3, then auto */
static int timer_step = 0;
static void on_timer_btn(lv_event_t * e) {
    (void)e;
    const char * cmds[] = { "timer1", "timer2", "timer3" };
    vent_send_vremote_async(cmds[timer_step % 3]);
    timer_step++;
    last_user_ms = lv_tick_get();
}

static void on_slider_changed(lv_event_t * e) {
    (void)e;
    int v = lv_slider_get_value(slider);
    lv_label_set_text_fmt(lbl_pct, "%d", v);
    last_user_ms = lv_tick_get();
}

static void on_slider_released(lv_event_t * e) {
    (void)e;
    int v = lv_slider_get_value(slider);
    vent_set_speed_async(v * 255 / 100);
    last_user_ms = lv_tick_get();
}

/* ── highlight active mode button ──────────────────────────── */
static void update_btn_highlights(void) {
    /* Determine active mode from vent_state */
    const char * fi = vent_state.fan_info;   /* "low"/"high"/"auto"/"timer"/… */
    const char * lc = vent_state.last_cmd;

    /* active_cmd: prefer fan_info, fall back to last_cmd */
    const char * active = (fi && fi[0]) ? fi : lc;
    int is_timer  = (active && strncmp(active, "timer", 5) == 0);
    int is_manual = (active && strncmp(active, "speed", 5) == 0);

    for (int i = 0; i < MODE_BTN_COUNT; i++) {
        if (!mode_btns[i]) continue;
        int lit = 0;
        if (i < 5) {
            /* named preset */
            lit = active && !is_timer && !is_manual
                  && strcasecmp(active, mode_cmds[i]) == 0;
        } else {
            /* timer button */
            lit = is_timer;
        }
        lv_obj_set_style_border_width(mode_btns[i], lit ? SX(3) : 0, 0);
    }
}

/* ── refresh callback (1 Hz) ────────────────────────────────── */
static void refresh_cb(lv_timer_t * t) {
    (void)t;

    /* Status line */
    if (!vent_state.connected) {
        lv_label_set_text(lbl_status,
            tr("Ventilatie: niet verbonden", "Ventilation: disconnected"));
    } else if (vent_state.itho_online == 0) {
        lv_label_set_text(lbl_status,
            tr("Itho offline (MQTT LWT)", "Itho offline (MQTT LWT)"));
    } else {
        lv_label_set_text_fmt(lbl_status,
            tr("Uitblaas %d%%  |  Doel %d%%  |  %d rpm  |  %s",
               "Exh %d%%  |  Set %d%%  |  %d rpm  |  %s"),
            vent_state.speed_pct, vent_state.exh_fan_pct,
            vent_state.fan_rpm, vent_mode_label());
    }

    update_btn_highlights();

    /* Sync slider to live level when user isn't touching it */
    if (slider && lbl_pct) {
        int dragging = lv_obj_has_state(slider, LV_STATE_PRESSED);
        int grace    = (lv_tick_get() - last_user_ms) < 4000;
        if (!dragging && !grace && vent_state.connected
                                && vent_state.itho_online != 0) {
            int live = vent_state.exh_fan_pct;
            if (live < 0)   live = 0;
            if (live > 100) live = 100;
            if (lv_slider_get_value(slider) != live) {
                lv_slider_set_value(slider, live, LV_ANIM_OFF);
                lv_label_set_text_fmt(lbl_pct, "%d", live);
            }
        }
    }
}

static void on_scr_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED)   { if (refresh_timer) lv_timer_resume(refresh_timer); }
    else if (c == LV_EVENT_SCREEN_UNLOADED) { if (refresh_timer) lv_timer_pause(refresh_timer); }
}

/* ── screen builder ─────────────────────────────────────────── */
lv_obj_t * screen_vent_remote_create(void) {
    if (scr_root) {
        last_user_ms = lv_tick_get();
        return scr_root;
    }

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x0f1a2a), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_ALL, NULL);

    /* ── header ── */
    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, SX(110), SY(48));
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, SX(8), SY(8));
    lv_obj_set_style_bg_color(back, lv_color_hex(0x223344), 0);
    lv_obj_set_style_radius(back, 10, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(bl, SF(18), 0);
    lv_label_set_text(bl, tr(LV_SYMBOL_LEFT " Terug", LV_SYMBOL_LEFT " Back"));
    lv_obj_center(bl);

    lv_obj_t * hdr = lv_label_create(scr_root);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(hdr, SF(26), 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, SY(16));
    lv_label_set_text(hdr, tr("Ventilatie", "Ventilation"));

    lv_obj_t * adv = lv_btn_create(scr_root);
    lv_obj_set_size(adv, SX(150), SY(48));
    lv_obj_align(adv, LV_ALIGN_TOP_RIGHT, -SX(8), SY(8));
    lv_obj_set_style_bg_color(adv, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(adv, 10, 0);
    lv_obj_set_style_border_width(adv, 0, 0);
    lv_obj_add_event_cb(adv, on_open_advanced, LV_EVENT_CLICKED, NULL);
    lv_obj_t * advl = lv_label_create(adv);
    lv_obj_set_style_text_color(advl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(advl, SF(18), 0);
    lv_label_set_text(advl, tr("Geavanceerd", "Advanced"));
    lv_obj_center(advl);

    /* ── status line ── */
    lbl_status = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_status, SF(16), 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, SY(66));
    lv_label_set_text(lbl_status, tr("(laden...)", "(loading...)"));

    /* ── mode buttons ── ─────────────────────────────────────
     *  Row 1:  Weg  |  Laag  |  Middel  |  Hoog
     *  Row 2:  Auto (wide)         Timer (wide)
     * ──────────────────────────────────────────────────────── */
    struct {
        const char * label_nl;
        const char * label_en;
        uint32_t     col;
        lv_event_cb_t cb;
        const char * cmd;    /* NULL for timer */
        int row, col_idx;    /* grid position */
    } btns[] = {
        { "Weg",    "Away",   0x3a4a5a, on_mode_btn, "away",   0, 0 },
        { "Laag",   "Low",    0x224d70, on_mode_btn, "low",    0, 1 },
        { "Middel", "Medium", 0x5a4a20, on_mode_btn, "medium", 0, 2 },
        { "Hoog",   "High",   0x804030, on_mode_btn, "high",   0, 3 },
        { "Auto",   "Auto",   0x2e6e3a, on_mode_btn, "auto",   1, 0 },
        { "Timer",  "Timer",  0x6a5424, on_timer_btn, NULL,    1, 1 },
    };

    int btn_y0   = SY(100);
    int btn_h    = SY(72);
    int btn_gap  = SY(10);
    int row0_w   = (DISP_HOR - SX(32) - SX(12)*3) / 4;   /* 4 cols */
    int row1_w   = (DISP_HOR - SX(32) - SX(12)) / 2;     /* 2 cols */

    for (int i = 0; i < MODE_BTN_COUNT; i++) {
        int row    = btns[i].row;
        int ci     = btns[i].col_idx;
        int y      = btn_y0 + row * (btn_h + btn_gap);
        int w, x;

        if (row == 0) {
            w = row0_w;
            x = SX(16) + ci * (w + SX(12));
        } else {
            w = row1_w;
            x = SX(16) + ci * (w + SX(12));
        }

        lv_obj_t * b = lv_btn_create(scr_root);
        lv_obj_set_size(b, w, btn_h);
        lv_obj_set_pos(b, x, y);
        lv_obj_set_style_bg_color(b, lv_color_hex(btns[i].col), 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_add_event_cb(b, btns[i].cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)btns[i].cmd);
        lv_obj_t * l = lv_label_create(b);
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(l, SF(20), 0);
        lv_label_set_text(l, tr(btns[i].label_nl, btns[i].label_en));
        lv_obj_center(l);
        mode_btns[i] = b;
    }

    /* ── manual slider ── */
    int slider_y = btn_y0 + 2 * (btn_h + btn_gap) + SY(16);

    lv_obj_t * man_lbl = lv_label_create(scr_root);
    lv_obj_set_style_text_color(man_lbl, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(man_lbl, SF(16), 0);
    lv_obj_set_pos(man_lbl, SX(16), slider_y);
    lv_label_set_text(man_lbl, tr("Handmatig", "Manual"));

    int init_pct = (vent_state.connected && vent_state.exh_fan_pct >= 0)
                   ? vent_state.exh_fan_pct : 0;
    if (init_pct > 100) init_pct = 100;

    slider = lv_slider_create(scr_root);
    lv_obj_set_size(slider, DISP_HOR - SX(32) - SX(120), SY(28));
    lv_obj_set_pos(slider, SX(16), slider_y + SY(30));
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, init_pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x1d2c40), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3a9bdc), LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, SX(8), LV_PART_MAIN);
    lv_obj_set_style_radius(slider, SX(8), LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(slider, SX(8), LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, on_slider_changed,  LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, on_slider_released, LV_EVENT_RELEASED,      NULL);

    /* % readout right of slider */
    lbl_pct = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_pct, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_pct, SF(40), 0);
    lv_obj_align_to(lbl_pct, slider, LV_ALIGN_OUT_RIGHT_MID, SX(12), 0);
    lv_label_set_text_fmt(lbl_pct, "%d", init_pct);

    lbl_pct_sign = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_pct_sign, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_pct_sign, SF(20), 0);
    lv_obj_align_to(lbl_pct_sign, lbl_pct, LV_ALIGN_OUT_RIGHT_BOTTOM, SX(2), -SY(4));
    lv_label_set_text(lbl_pct_sign, "%");

    last_user_ms = lv_tick_get();
    if (!refresh_timer)
        refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    lv_timer_pause(refresh_timer);

    return scr_root;
}
