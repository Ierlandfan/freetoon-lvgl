/*
 * Thermostat detail screen — full-screen room temperature view with
 * setpoint adjustment. Reachable from the home tile.
 */
#include "screens.h"
#include "display.h"
#include "boxtalk.h"
#include "homewizard.h"
#include "icons.h"
#include "settings.h"
#include "pin_modal.h"
#include <stdio.h>
#include <time.h>

static lv_obj_t * scr_root = NULL;
static lv_obj_t * lbl_temp;
static lv_obj_t * lbl_humidity;
static lv_obj_t * lbl_voc;
static lv_obj_t * lbl_water;
static lv_obj_t * lbl_setpoint;
static lv_obj_t * lbl_conn;
static lv_obj_t * lbl_burner;
static lv_obj_t * lbl_ch_hdr;
static lv_obj_t * lbl_flow;
static lv_obj_t * lbl_return;
static lv_obj_t * img_flame;
static lv_obj_t * img_faucet;
static lv_obj_t * img_drop;
/* Radiator+flame glyph next to the big indoor-temp, shown while CH is firing. */
static lv_obj_t * img_temp_flame;
static lv_obj_t * lbl_clock;
static lv_obj_t * lbl_date;
static lv_timer_t * refresh_timer = NULL;
/* Program preset buttons just below the indoor temp.
 *   [0] Scheduled  → resume_schedule (whatever the schedule says now)
 *   [1] Manual     → set_manual (permanent hold, activeState = -1).
 *                    This is the dedicated "leave the schedule
 *                    indefinitely" control; the home tile's +/- nudges
 *                    are temporary (auto-resume at the next switch).
 *   [2..5] Comfort/Home/Sleep/Away → set_program(0..3)
 * Sentinel -2 in prog_state[] means "resume schedule" — distinct from -1
 * (manual). The active mode (Scheduled or Manual) AND the active preset
 * both get a white outline; the two highlights live on different buttons
 * so they don't clash. */
static lv_obj_t * btn_prog[6] = {0};
static const int  prog_state[6] = {-2, -1, 0, 1, 2, 3};

static void on_open_advanced(lv_event_t * e) { (void)e; ui_push(screen_heater_advanced_create()); }
/* Gated via pin_modal: setpoint nudges, preset taps, and schedule/manual
 * mode toggle all run through pin_gate so the PIN prompts when enabled. */
static void sp_up_apply(void * c)   { (void)c; boxtalk_setpoint_increase(); }
static void sp_down_apply(void * c) { (void)c; boxtalk_setpoint_decrease(); }
static void on_setpoint_up(lv_event_t * e)   { (void)e; pin_gate(sp_up_apply,   NULL); }
static void on_setpoint_down(lv_event_t * e) { (void)e; pin_gate(sp_down_apply, NULL); }
static void on_back(lv_event_t * e) { ui_pop(); }
static void on_open_schedule(lv_event_t * e) { (void)e; ui_push(screen_schedule_create()); }
static void program_tap_apply(void * ctx) {
    int s = (int)(intptr_t)ctx;
    if      (s == -2) boxtalk_resume_schedule();
    else if (s == -1) boxtalk_set_manual();
    else              boxtalk_set_program(s);
}
static void on_program_tap(lv_event_t * e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int s = prog_state[idx];
    pin_gate(program_tap_apply, (void *)(intptr_t)s);
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk[16];
    strftime(clk, sizeof(clk), "%H:%M", &tm);
    lv_label_set_text(lbl_clock, clk);
    char dt[48];
    strftime(dt, sizeof(dt), "%A %d %B %Y", &tm);
    lv_label_set_text(lbl_date, dt);

    if (toon_state.connected) {
        lv_label_set_text_fmt(lbl_conn, "BoxTalk OK  msg=%d", toon_state.msg_count);
        lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0x66cc88), 0);
    } else {
        lv_label_set_text(lbl_conn, "BoxTalk: connecting...");
        lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0xff8866), 0);
    }

    if (toon_state.indoor_temp > 0)
        lv_label_set_text_fmt(lbl_temp, "%.1f°C", display_indoor_temp(toon_state.indoor_temp));

    /* Highlight logic: the mode button (Scheduled[0] or Off[1]) gets a white
     * border based on whether we're on the schedule; the preset button
     * (Comfort[2] / Home[3] / Sleep[4] / Away[5]) gets one based on the LIVE
     * comfort preset, which is active_state (happ's "activeState"), NOT
     * program_state (that's the scheme mode). A +/- temporary override counts
     * as "still on the schedule" so the Scheduled mode + origin preset stay
     * highlighted while it's in flight. */
    int temp_origin = boxtalk_temp_override_origin();   /* -1 if none */
    int on_schedule = (toon_state.active_state >= 0) || (temp_origin >= 0);
    int mode_idx    = on_schedule ? 0 : 1;
    int preset;
    if (toon_state.active_state >= 0 && toon_state.active_state <= 3) {
        preset = toon_state.active_state;
    } else {
        preset = temp_origin;
    }
    int preset_idx = (preset >= 0) ? preset + 2 : -1;
    for (int i = 0; i < 6; i++) {
        if (!btn_prog[i]) continue;
        int active = (i == mode_idx) || (i == preset_idx);
        lv_obj_set_style_border_width(btn_prog[i], active ? 2 : 0, 0);
    }
    if (toon_state.humidity > 0)
        lv_label_set_text_fmt(lbl_humidity, "RH %.0f%%", toon_state.humidity);
    if (toon_state.eco2 || toon_state.tvoc)
        lv_label_set_text_fmt(lbl_voc, "eCO2 %d ppm", toon_state.eco2);

    if (hw_state.connected_water) {
        /* 3 decimals — every litre poured ticks the displayed total. */
        if (hw_state.water_lpm > 0.05f)
            lv_label_set_text_fmt(lbl_water, "%.3f m3  /  %.1f L/min",
                                  hw_state.water_total_m3, hw_state.water_lpm);
        else
            lv_label_set_text_fmt(lbl_water, "%.3f m3",
                                  hw_state.water_total_m3);
    } else {
        lv_label_set_text(lbl_water, "Water -- m3");
    }
    if (toon_state.setpoint > 0)
        lv_label_set_text_fmt(lbl_setpoint, "Setpoint: %.1f°C", toon_state.setpoint);

    /* For the "flow" reading prefer boiler_out_temp — that's the freshest
     * OTGW CurrentBoilerTemperature coming through the bridge. The
     * TemperatureSensor-based boiler_temp from happ_thermstat lags by tens
     * of seconds while ramping, which made the label disagree with the
     * "CH water Flow" panel on the right. */
    float flow_t = (toon_state.boiler_out_temp > 0)
                       ? toon_state.boiler_out_temp
                       : toon_state.boiler_temp;
    /* Status line — short word only ("Heating" / "Hot water" / "Boiler idle").
     * The radiator+flame glyph next to the indoor temp carries the visual,
     * and the right-column "CH water Flow … Return …" panel has the temps.
     * The old left-side flame/faucet/drop icons are now redundant — kept as
     * widgets but always hidden. */
    if (toon_state.burner_on) {
        lv_label_set_text(lbl_burner, "Heating");
        lv_obj_set_style_text_color(lbl_burner, lv_color_hex(0xff6644), 0);
    } else if (toon_state.dhw_on) {
        lv_label_set_text(lbl_burner, "Hot water");
        lv_obj_set_style_text_color(lbl_burner, lv_color_hex(0x44aaff), 0);
    } else {
        lv_label_set_text(lbl_burner, "Boiler idle");
        lv_obj_set_style_text_color(lbl_burner, lv_color_hex(0x88aabb), 0);
    }
    lv_obj_add_flag(img_flame,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(img_faucet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(img_drop,   LV_OBJ_FLAG_HIDDEN);

    /* CH water inlet/outlet temps. Flow falls back to the boiler
       TemperatureSensor (always present) if the boilerTemps query hasn't
       answered; return comes only from the boilerRetTemps query. */
    float flow = toon_state.boiler_out_temp > 0 ? toon_state.boiler_out_temp
                                                : toon_state.boiler_temp;
    if (flow > 0) lv_label_set_text_fmt(lbl_flow, "Flow  %.1f°C", flow);
    else          lv_label_set_text(lbl_flow, "Flow  -- C");
    if (toon_state.boiler_in_temp > 0)
        lv_label_set_text_fmt(lbl_return, "Return  %.1f°C", toon_state.boiler_in_temp);
    else
        /* Many boilers leave OT DID 28 (CH return temp) unimplemented; OTGW
         * reports returnwatertemperature=0 in that case and happ_thermstat
         * forwards the zero unchanged. Mark it n/a so this row doesn't look
         * like a UI bug. */
        lv_label_set_text(lbl_return, "Return  n/a");

    /* Radiator+flame next to the big indoor-temp — visible while CH fires. */
    if (img_temp_flame) {
        if (toon_state.burner_on)
            lv_obj_clear_flag(img_temp_flame, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(img_temp_flame, LV_OBJ_FLAG_HIDDEN);
    }

    /* Force a full screen invalidate to defeat any partial-flush quirk. */
    lv_obj_invalidate(scr_root);
}

lv_obj_t * screen_thermostat_create(void) {
    if (scr_root) return scr_root;  /* cached */

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x0f1a2a), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button — generous hit area (140x96) plus extended ext_click_area
       so even sloppy taps near the corner register. */
    lv_obj_t * btn_back = lv_btn_create(scr_root);
    lv_obj_set_size(btn_back, 140, 96);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x223344), 0);
    lv_obj_set_style_radius(btn_back, 14, 0);
    lv_obj_set_ext_click_area(btn_back, 20);
    lv_obj_add_event_cb(btn_back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * btn_back_lbl = lv_label_create(btn_back);
    lv_label_set_text(btn_back_lbl, "< Back");
    lv_obj_set_style_text_color(btn_back_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(btn_back_lbl, SF(22), 0);
    lv_obj_center(btn_back_lbl);

    /* Clock + date */
    lbl_clock = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_clock, SF(28), 0);
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_LEFT, 180, 25);
    lv_label_set_text(lbl_clock, "--:--");

    lbl_date = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_date, SF(18), 0);
    lv_obj_align_to(lbl_date, lbl_clock, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 0);

    /* Schedule button (top-right) */
    lv_obj_t * btn_sched = lv_btn_create(scr_root);
    lv_obj_set_size(btn_sched, 180, 70);
    lv_obj_align(btn_sched, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_set_style_bg_color(btn_sched, lv_color_hex(0x335577), 0);
    lv_obj_set_style_radius(btn_sched, 12, 0);
    lv_obj_set_ext_click_area(btn_sched, 16);
    lv_obj_add_event_cb(btn_sched, on_open_schedule, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sched_lbl = lv_label_create(btn_sched);
    lv_label_set_text(sched_lbl, "Schedule");
    lv_obj_set_style_text_color(sched_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(sched_lbl, SF(22), 0);
    lv_obj_center(sched_lbl);

    /* Connection indicator just under the Schedule button */
    lbl_conn = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0xff8866), 0);
    lv_obj_set_style_text_font(lbl_conn, SF(18), 0);
    lv_obj_align(lbl_conn, LV_ALIGN_TOP_RIGHT, -20, 95);
    lv_label_set_text(lbl_conn, "BoxTalk: connecting...");

    /* Big indoor temp */
    lbl_temp = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_temp, lv_color_hex(0xffcc44), 0);
    lv_obj_set_style_text_font(lbl_temp, SF(48), 0);
    lv_obj_align(lbl_temp, LV_ALIGN_CENTER, 0, SY(-80));
    lv_label_set_text(lbl_temp, "-- C");

    /* Flame icon at left of the big temp — visible only when the burner
     * is firing CH. Same source/colour as the home-tile flame for
     * consistency. */
    img_temp_flame = lv_img_create(scr_root);
    lv_img_set_src(img_temp_flame, &icon_radiator);
    lv_img_set_zoom(img_temp_flame, 256);
    lv_obj_set_style_img_recolor(img_temp_flame, lv_color_hex(0xff6644), 0);
    lv_obj_set_style_img_recolor_opa(img_temp_flame, 255, 0);
    lv_obj_align(img_temp_flame, LV_ALIGN_CENTER, SX(145), SY(-75));
    lv_obj_add_flag(img_temp_flame, LV_OBJ_FLAG_HIDDEN);

    /* Program preset row — six small buttons centered horizontally just
     * below the indoor temp.  Scheduled / Manual are the mode toggle;
     * Comfort/Home/Sleep/Away pick a specific preset. Colours mirror the
     * schedule editor pills for instant recognition; Scheduled uses a
     * neutral teal so it doesn't fight any of the preset colours. */
    {
        const char * names[6] = {"Scheduled", "Manual",
                                 "Comfort", "Home", "Sleep", "Away"};
        uint32_t     cols[6]  = {0x2f6b6b, 0x6a5424,
                                 0xcc7733, 0x3377cc, 0x553388, 0x557788};
        /* On Toon 1 the 130px buttons + gaps total 810px and run off the
         * 800px panel; SX() shrinks them to fit (identity on Toon 2). */
        const int    bw = SX(130), bh = 44, gap = SX(6);
        int total = 6 * bw + 5 * gap;
        for (int i = 0; i < 6; i++) {
            lv_obj_t * b = lv_btn_create(scr_root);
            lv_obj_set_size(b, bw, bh);
            lv_obj_align(b, LV_ALIGN_CENTER,
                         -total / 2 + i * (bw + gap) + bw / 2, SY(-20));
            lv_obj_set_style_bg_color(b, lv_color_hex(cols[i]), 0);
            lv_obj_set_style_radius(b, 10, 0);
            lv_obj_set_style_border_color(b, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_add_event_cb(b, on_program_tap, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
            lv_obj_t * bl = lv_label_create(b);
            lv_label_set_text(bl, names[i]);
            lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_text_font(bl, SF(18), 0);
            lv_obj_center(bl);
            btn_prog[i] = b;
        }
    }

    /* Environment readings live in the left column just above the
       setpoint row, where "Boiler idle" used to sit. Boiler state has
       moved further up. */
    lbl_humidity = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_humidity, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_humidity, SF(22), 0);
    lv_obj_align(lbl_humidity, LV_ALIGN_BOTTOM_LEFT, 30,
                 SY(-260) - (DISP_VER < 600 ? 8 : 0));
    lv_label_set_text(lbl_humidity, "RH --%");

    lbl_voc = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_voc, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_voc, SF(22), 0);
    lv_obj_align(lbl_voc, LV_ALIGN_BOTTOM_LEFT, 30,
                 SY(-225) - (DISP_VER < 600 ? 8 : 0));
    lv_label_set_text(lbl_voc, "eCO2 -- ppm");

    lbl_water = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_water, lv_color_hex(0x66aaff), 0);
    lv_obj_set_style_text_font(lbl_water, SF(22), 0);
    lv_obj_align(lbl_water, LV_ALIGN_BOTTOM_LEFT, 30,
                 SY(-190) - (DISP_VER < 600 ? 8 : 0));
    lv_label_set_text(lbl_water, "Water -- m3");

    /* Setpoint row with +/- buttons. Wider than before so "Setpoint: NN.N C"
       has breathing room between the two buttons. */
    lv_obj_t * sp_row = lv_obj_create(scr_root);
    lv_obj_set_size(sp_row, SX(760), SY(130));
    /* Lift a little extra on Toon 1 so its bottom clears the Advanced btn. */
    lv_obj_align(sp_row, LV_ALIGN_BOTTOM_MID, 0,
                 SY(-40) - (DISP_VER < 600 ? 8 : 0));
    lv_obj_set_style_bg_color(sp_row, lv_color_hex(0x1a2a44), 0);
    lv_obj_set_style_border_width(sp_row, 0, 0);
    lv_obj_set_style_radius(sp_row, 14, 0);
    lv_obj_clear_flag(sp_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * btn_dn = lv_btn_create(sp_row);
    lv_obj_set_size(btn_dn, SX(110), SY(100));
    lv_obj_align(btn_dn, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_bg_color(btn_dn, lv_color_hex(0x335577), 0);
    lv_obj_add_event_cb(btn_dn, on_setpoint_down, LV_EVENT_CLICKED, NULL);
    lv_obj_t * btn_dn_lbl = lv_label_create(btn_dn);
    lv_label_set_text(btn_dn_lbl, "-");
    lv_obj_set_style_text_font(btn_dn_lbl,
                               (DISP_VER < 600 ? SF(28)
                                               : SF(48)), 0);
    lv_obj_center(btn_dn_lbl);

    lbl_setpoint = lv_label_create(sp_row);
    lv_obj_set_style_text_color(lbl_setpoint, lv_color_hex(0xffffff), 0);
    /* font_48 is too wide for the narrowed row on Toon 1 — drop to 34 there. */
    lv_obj_set_style_text_font(lbl_setpoint,
                               (DISP_VER < 600 ? SF(28)
                                               : SF(48)), 0);
    lv_obj_align(lbl_setpoint, LV_ALIGN_CENTER, 0, -8);
    lv_label_set_text(lbl_setpoint, "Setpoint: --");

    lv_obj_t * btn_up = lv_btn_create(sp_row);
    lv_obj_set_size(btn_up, SX(110), SY(100));
    lv_obj_align(btn_up, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_bg_color(btn_up, lv_color_hex(0x335577), 0);
    lv_obj_add_event_cb(btn_up, on_setpoint_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t * btn_up_lbl = lv_label_create(btn_up);
    lv_label_set_text(btn_up_lbl, "+");
    lv_obj_set_style_text_font(btn_up_lbl,
                               (DISP_VER < 600 ? SF(28)
                                               : SF(48)), 0);
    lv_obj_center(btn_up_lbl);

    /* Boiler state — icons + text. Icons live at left; text label sits
       to their right and is the only thing that ever fills its area. */
    img_flame = lv_img_create(scr_root);
    lv_img_set_src(img_flame, &icon_flame);
    lv_obj_set_style_img_recolor(img_flame, lv_color_hex(0xff6644), 0);
    lv_obj_set_style_img_recolor_opa(img_flame, 255, 0);
    lv_obj_align(img_flame, LV_ALIGN_BOTTOM_LEFT, 30, -340);
    lv_obj_add_flag(img_flame, LV_OBJ_FLAG_HIDDEN);

    img_faucet = lv_img_create(scr_root);
    lv_img_set_src(img_faucet, &icon_faucet);
    lv_obj_set_style_img_recolor(img_faucet, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_img_recolor_opa(img_faucet, 255, 0);
    lv_obj_align(img_faucet, LV_ALIGN_BOTTOM_LEFT, 26, -344);
    lv_obj_add_flag(img_faucet, LV_OBJ_FLAG_HIDDEN);

    /* Red drop overlapping the faucet spout */
    img_drop = lv_img_create(scr_root);
    lv_img_set_src(img_drop, &icon_drop);
    lv_obj_set_style_img_recolor(img_drop, lv_color_hex(0xff3333), 0);
    lv_obj_set_style_img_recolor_opa(img_drop, 255, 0);
    lv_obj_align(img_drop, LV_ALIGN_BOTTOM_LEFT, 48, -320);
    lv_obj_add_flag(img_drop, LV_OBJ_FLAG_HIDDEN);

    /* "Boiler idle / Heating / Hot water" status label — was at
     * BOTTOM_LEFT(90, -325) which collided horizontally with the new
     * program-buttons row (the Manual button sat exactly behind the
     * text). Pinned to the upper-left under the back button so it has
     * its own clean strip. */
    lbl_burner = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_burner, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_burner, SF(22), 0);
    lv_obj_align(lbl_burner, LV_ALIGN_TOP_LEFT, 180, SY(130));
    lv_label_set_text(lbl_burner, "Boiler idle");

    /* CH water inlet/outlet block — right side, above the setpoint row.
       Outlet = flow temp (CurrentBoilerTemperature),
       inlet  = return temp (CurrentBoilerReturnTemperature). */
    lbl_ch_hdr = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_ch_hdr, lv_color_hex(0x6688aa), 0);
    lv_obj_set_style_text_font(lbl_ch_hdr, SF(18), 0);
    lv_obj_align(lbl_ch_hdr, LV_ALIGN_BOTTOM_RIGHT, -40,
                 SY(-250) - (DISP_VER < 600 ? 8 : 0));
    lv_label_set_text(lbl_ch_hdr, "CH water");

    lbl_flow = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_flow, lv_color_hex(0xff8866), 0);
    lv_obj_set_style_text_font(lbl_flow, SF(22), 0);
    lv_obj_align(lbl_flow, LV_ALIGN_BOTTOM_RIGHT, -40,
                 SY(-220) - (DISP_VER < 600 ? 8 : 0));
    lv_label_set_text(lbl_flow, "Flow  -- C");

    lbl_return = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_return, lv_color_hex(0x66aaff), 0);
    lv_obj_set_style_text_font(lbl_return, SF(22), 0);
    lv_obj_align(lbl_return, LV_ALIGN_BOTTOM_RIGHT, -40,
                 SY(-192) - (DISP_VER < 600 ? 8 : 0));
    lv_label_set_text(lbl_return, "Return  -- C");

    /* "Advanced" button — pushes OTGW raw-DID list. */
    lv_obj_t * adv = lv_btn_create(scr_root);
    lv_obj_set_size(adv, 140, 44);
    lv_obj_align(adv, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_set_style_bg_color(adv, lv_color_hex(0x444444), 0);
    lv_obj_add_event_cb(adv, on_open_advanced, LV_EVENT_CLICKED, NULL);
    lv_obj_t * advl = lv_label_create(adv);
    lv_obj_set_style_text_color(advl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(advl, SF(18), 0);
    lv_label_set_text(advl, "Advanced");
    lv_obj_center(advl);

    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 500, NULL);
    return scr_root;
}
