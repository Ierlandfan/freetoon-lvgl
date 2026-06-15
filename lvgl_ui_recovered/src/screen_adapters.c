/*
 * Adapters screen — Settings -> Adapters.
 *
 * Status + diagnostics for the Toon's two hardware adapters:
 *
 *   Meteradapter  — the built-in smart-meter reader. It is a Z-Wave node
 *                   (HAE_METER); happ_pwrusage aggregates it and publishes the
 *                   live power on the ElectricityFlowMeter BoxTalk service
 *                   (see meteradapter.c). "Found" = an HAE_METER node is in the
 *                   Z-Wave device list (native boxtalk_zwave_get_devices);
 *                   "Online" = flow notifies are fresh (meter_state.connected).
 *                   A "Pair via Z-Wave" button starts Z-Wave inclusion — gated
 *                   behind an explicit confirm so we never auto-fire a mutating
 *                   Z-Wave action (a probe once wiped the controller).
 *
 *   Keteladapter  — the boiler adapter. WIRED (OpenTherm over /dev/ttymxc0),
 *                   not Z-Wave, so there is nothing to pair. "Online" =
 *                   happ_thermstat reports otCommError==0 (toon_state).
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "boxtalk.h"
#include "meteradapter.h"
#include <stdio.h>
#include <string.h>

#define COL_BG       0x0e1a2a
#define COL_CARD     0x1a2940
#define COL_TEXT_HI  0xffffff
#define COL_TEXT_DIM 0x88aabb
#define COL_OK       0x2e6e3a
#define COL_WARN     0x6e3a3a
#define COL_OFF      0x3a4658
#define COL_GREEN    0x36c46b
#define COL_RED      0xd4574f
#define COL_AMBER    0xd9a23a

static lv_obj_t * scr_root      = NULL;
static lv_obj_t * lbl_met_state = NULL;
static lv_obj_t * lbl_met_sub   = NULL;
static lv_obj_t * lbl_ket_state = NULL;
static lv_obj_t * lbl_ket_sub   = NULL;
static lv_obj_t * btn_pair_lbl  = NULL;
static lv_timer_t * refresh_timer = NULL;

static int  g_met_found  = 0;     /* HAE_METER node present in Z-Wave list */
static int  g_pair_active = 0;    /* Z-Wave inclusion window open */
static int  g_query_ticks = 0;

/* ===================================================================== */
/* Meter Z-Wave "found" detection                                        */
/* ===================================================================== */
/* The GetDevices response (zwave_response_buf) lists every node; the meter
 * shows up as an HAE_METER* type. We only need presence, so a substring scan
 * is enough and robust against the XML/JSON shape differences between fw. */
static void scan_met_found(void) {
    if (!zwave_response_ready) return;
    g_met_found = (strstr(zwave_response_buf, "HAE_METER") != NULL);
    zwave_response_ready = 0;
}

/* ===================================================================== */
/* Status text                                                           */
/* ===================================================================== */
static void update_meter_labels(void) {
    int online = meter_state.connected;
    uint32_t col = online ? COL_GREEN : (g_met_found ? COL_AMBER : COL_RED);
    lv_obj_set_style_text_color(lbl_met_state, lv_color_hex(col), 0);

    if (online)
        lv_label_set_text_fmt(lbl_met_state, tr("Online  -  %.0f W", "Online  -  %.0f W"), (double)meter_state.power_w);
    else if (g_met_found)
        lv_label_set_text(lbl_met_state, tr("Gevonden - geen live data", "Found - no live data"));
    else
        lv_label_set_text(lbl_met_state, tr("Niet gevonden", "Not found"));

    if (g_pair_active)
        lv_label_set_text(lbl_met_sub,
            tr("Z-Wave koppelen actief - druk op de knop van de meter (stopt vanzelf na 60 s).",
               "Z-Wave inclusion active - press the button on the meter (auto-stops in 60 s)."));
    else if (online)
        lv_label_set_text(lbl_met_sub, tr("Z-Wave slimme meter - publiceert op ElectricityFlowMeter.", "Z-Wave smart meter - publishing on ElectricityFlowMeter."));
    else if (g_met_found)
        lv_label_set_text(lbl_met_sub, tr("Meter is gekoppeld maar verzendt niet. Controleer de P1-/meterkabel.", "Meter is paired but not sending. Check the P1/meter cable."));
    else
        lv_label_set_text(lbl_met_sub, tr("Geen HAE_METER node gekoppeld. Tik op \"Koppel via Z-Wave\" om toe te voegen.", "No HAE_METER node paired. Tap \"Pair via Z-Wave\" to add it."));
}

static void update_ket_labels(void) {
    int online = (toon_state.ot_comm_error == 0);
    lv_obj_set_style_text_color(lbl_ket_state, lv_color_hex(online ? COL_GREEN : COL_RED), 0);
    if (online)
        lv_label_set_text_fmt(lbl_ket_state, tr("Online  -  ketel %.0f C, mod %.0f%%", "Online  -  boiler %.0f C, mod %.0f%%"),
            (double)toon_state.boiler_out_temp, (double)toon_state.modulation_level);
    else
        lv_label_set_text(lbl_ket_state, tr("Offline - OpenTherm communicatiefout", "Offline - OpenTherm comm error"));
    lv_label_set_text(lbl_ket_sub,
        tr("Bedrade OpenTherm-adapter (/dev/ttymxc0) - koppelen niet nodig.",
           "Wired OpenTherm adapter (/dev/ttymxc0) - no pairing needed."));
}

/* ===================================================================== */
/* Buttons                                                               */
/* ===================================================================== */
static void on_met_test(lv_event_t * e) {
    (void)e;
    boxtalk_zwave_get_devices();   /* refresh found-state; flow drives online */
    g_query_ticks = 2;
    lv_label_set_text(lbl_met_sub, tr("Z-Wave + live verbruik opvragen...", "Querying Z-Wave + live flow..."));
}

static void on_ket_test(lv_event_t * e) {
    (void)e;
    boxtalk_request_boiler_refresh();
    lv_label_set_text(lbl_ket_sub, tr("Ketel opnieuw opvragen (happ_thermstat)...", "Re-querying boiler (happ_thermstat)..."));
}

/* --- pair (Z-Wave inclusion) with confirm ---------------------------- */
static lv_obj_t * g_confirm = NULL;
static void confirm_close(void) { if (g_confirm) { lv_obj_del(g_confirm); g_confirm = NULL; } }
static void on_confirm_cancel(lv_event_t * e) { (void)e; confirm_close(); }
static void on_confirm_pair(lv_event_t * e) {
    (void)e;
    confirm_close();
    g_pair_active = 1;
    boxtalk_zwave_include(1);     /* begin add mode (auto-stops in 60 s) */
    g_query_ticks = 1;
}
static void on_pair_clicked(lv_event_t * e) {
    (void)e;
    if (g_pair_active) {          /* already running -> stop */
        boxtalk_zwave_include(0);
        g_pair_active = 0;
        return;
    }
    /* Confirm before starting a mutating Z-Wave action. */
    g_confirm = lv_obj_create(scr_root);
    lv_obj_set_size(g_confirm, DISP_HOR, DISP_VER);
    lv_obj_set_pos(g_confirm, 0, 0);
    lv_obj_set_style_bg_color(g_confirm, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_confirm, LV_OPA_70, 0);
    lv_obj_set_style_border_width(g_confirm, 0, 0);
    lv_obj_clear_flag(g_confirm, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * card = lv_obj_create(g_confirm);
    lv_obj_set_size(card, 640, 280);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * t = lv_label_create(card);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(t, SF(22), 0);
    lv_obj_set_width(t, 600);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_label_set_text(t,
        tr("Z-Wave koppelen starten?\n\n"
           "Dit opent een venster van 60 s om de slimme meter te koppelen. Start "
           "koppelen op de meter zelf. Bestaande apparaten blijven ongewijzigd.",
           "Start Z-Wave inclusion?\n\n"
           "This opens a 60 s window to pair the smart meter. Trigger inclusion on "
           "the meter itself. Existing devices are not affected."));
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 16, 14);

    lv_obj_t * go = lv_btn_create(card);
    lv_obj_set_size(go, 180, 52);
    lv_obj_align(go, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_set_style_bg_color(go, lv_color_hex(COL_OK), 0);
    lv_obj_add_event_cb(go, on_confirm_pair, LV_EVENT_CLICKED, NULL);
    lv_obj_t * gl = lv_label_create(go); lv_label_set_text(gl, tr("Start", "Start")); lv_obj_center(gl);

    lv_obj_t * ca = lv_btn_create(card);
    lv_obj_set_size(ca, 180, 52);
    lv_obj_align(ca, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    lv_obj_set_style_bg_color(ca, lv_color_hex(COL_OFF), 0);
    lv_obj_add_event_cb(ca, on_confirm_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(ca); lv_label_set_text(cl, tr("Annuleer", "Cancel")); lv_obj_center(cl);
}

/* ===================================================================== */
/* Refresh loop                                                          */
/* ===================================================================== */
static void refresh_cb(lv_timer_t * t) {
    (void)t;
    scan_met_found();

    /* Refresh the Z-Wave device list periodically (read-only) so "found"
     * stays current without continuous polling of the value. */
    if (g_query_ticks <= 0) {
        boxtalk_zwave_get_devices();
        g_query_ticks = 5;
    } else {
        g_query_ticks--;
    }

    if (btn_pair_lbl)
        lv_label_set_text(btn_pair_lbl, g_pair_active ? tr("Stop koppelen", "Stop pairing") : tr("Koppel via Z-Wave", "Pair via Z-Wave"));
    update_meter_labels();
    update_ket_labels();
}

/* ===================================================================== */
/* Screen build                                                          */
/* ===================================================================== */
static void back_async(void * u) { (void)u; ui_pop(); }
static void on_back(lv_event_t * e) {
    (void)e;
    if (g_pair_active) { boxtalk_zwave_include(0); g_pair_active = 0; }
    lv_async_call(back_async, NULL);
}

static void on_scr_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) {
        if (refresh_timer) lv_timer_resume(refresh_timer);
        g_query_ticks = 0;
    } else if (c == LV_EVENT_SCREEN_UNLOADED) {
        if (refresh_timer) lv_timer_pause(refresh_timer);
    }
}

static lv_obj_t * mk_btn(lv_obj_t * parent, const char * txt, uint32_t col,
                         lv_event_cb_t cb, lv_obj_t ** out_lbl) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, 220, 56);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, SF(20), 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    if (out_lbl) *out_lbl = l;
    return b;
}

/* Build one adapter card. Returns the card; fills *out_state / *out_sub with
 * the status + subtitle labels for the refresh loop to update. */
static lv_obj_t * mk_card(int y, const char * title, const char * kind,
                          lv_obj_t ** out_state, lv_obj_t ** out_sub) {
    lv_obj_t * card = lv_obj_create(scr_root);
    lv_obj_set_size(card, DISP_HOR - 44, SY(210));
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 22, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * tt = lv_label_create(card);
    lv_obj_set_style_text_color(tt, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(tt, SF(22), 0);
    lv_label_set_text(tt, title);
    lv_obj_align(tt, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t * kd = lv_label_create(card);
    lv_obj_set_style_text_color(kd, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(kd, SF(14), 0);
    lv_label_set_text(kd, kind);
    lv_obj_align(kd, LV_ALIGN_TOP_LEFT, 2, SY(34));

    lv_obj_t * st = lv_label_create(card);
    lv_obj_set_style_text_font(st, SF(22), 0);
    lv_obj_set_style_text_color(st, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(st, "...");
    lv_obj_align(st, LV_ALIGN_TOP_LEFT, 0, SY(64));
    *out_state = st;

    lv_obj_t * sub = lv_label_create(card);
    lv_obj_set_style_text_font(sub, SF(14), 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_width(sub, DISP_HOR - 280);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
    lv_label_set_text(sub, "");
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 0, SY(104));
    *out_sub = sub;

    return card;
}

lv_obj_t * screen_adapters_create(void) {
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_LOADED,   NULL);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_UNLOADED, NULL);

    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, 140, 52);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 20, 14);
    lv_obj_set_style_bg_color(back, lv_color_hex(COL_OFF), 0);
    lv_obj_set_style_radius(back, 10, 0);
    lv_obj_set_ext_click_area(back, 20);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(bl, SF(22), 0);
    lv_label_set_text(bl, tr("< Terug", "< Back"));
    lv_obj_center(bl);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, tr("Adapters", "Adapters"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 180, 24);

    /* Meteradapter card + buttons. */
    lv_obj_t * mcard = mk_card(SY(86), tr("Meteradapter", "Meteradapter"), tr("Slimme meter - Z-Wave", "Smart meter - Z-Wave"),
                               &lbl_met_state, &lbl_met_sub);
    mk_btn(mcard, tr("Test", "Test"), 0x2a4060, on_met_test, NULL);
    lv_obj_t * met_test = lv_obj_get_child(mcard, lv_obj_get_child_cnt(mcard) - 1);
    lv_obj_align(met_test, LV_ALIGN_TOP_RIGHT, 0, 0);
    mk_btn(mcard, tr("Koppel via Z-Wave", "Pair via Z-Wave"), COL_OK, on_pair_clicked, &btn_pair_lbl);
    lv_obj_t * met_pair = lv_obj_get_child(mcard, lv_obj_get_child_cnt(mcard) - 1);
    lv_obj_align(met_pair, LV_ALIGN_TOP_RIGHT, 0, SY(64));

    /* Keteladapter card + button. */
    lv_obj_t * kcard = mk_card(SY(312), tr("Keteladapter", "Keteladapter"), tr("Ketel - bedraad (OpenTherm)", "Boiler - wired (OpenTherm)"),
                               &lbl_ket_state, &lbl_ket_sub);
    mk_btn(kcard, tr("Test", "Test"), 0x2a4060, on_ket_test, NULL);
    lv_obj_t * ket_test = lv_obj_get_child(kcard, lv_obj_get_child_cnt(kcard) - 1);
    lv_obj_align(ket_test, LV_ALIGN_TOP_RIGHT, 0, 0);

    update_meter_labels();
    update_ket_labels();

    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}
