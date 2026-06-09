/*
 * Z-Wave management screen — Settings -> Z-Wave.
 *
 * Talks to the Toon's built-in Z-Wave controller (hdrv_zwave) over the
 * NATIVE BoxTalk bus via boxtalk.c — the same path qt-gui uses — not HTTP.
 *   GetDevices / NodeNeighborUpdate / IncludeDevice / ExcludeDevice /
 *   basicSet / SetDeviceName  on serviceId "specific1", destuuid
 *   qb-...:hdrv_zwave (see boxtalk_zwave_* + reference_toon_zwave_api memory).
 *
 * The device-list response (boxtalk_zwave_get_devices -> zwave_response_buf)
 * is XML: <u:GetDevicesResponse><devices> <device>... </device> ... </devices>.
 * We poll zwave_response_ready in the refresh timer and parse on the UI thread.
 * Fire-and-forget actions call boxtalk_zwave_* directly (send_msg is the same
 * lock-guarded path the thermostat +/- buttons already use from the UI thread).
 *
 * "Enable control" gates the write actions and is a pure toonui setting
 * (settings.enable_zwave) — we do NOT poke the stock supportControl flag or
 * restart the driver; reads work regardless and the API responds either way.
 */
#include "screens.h"
#include "display.h"
#include "settings.h"
#include "boxtalk.h"
#include "meteradapter.h"
#include <stdio.h>
#include <stdlib.h>
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

#include "screen_zwave.h"

/* Internal struct + cap now live in screen_zwave.h so the master↔slave
 * bridge can serialise/deserialise the controller device list. */
#define MAX_ZW_DEV ZWAVE_DEV_MAX
typedef zwave_dev_t zw_dev_t;

static lv_obj_t * scr_root      = NULL;
static lv_obj_t * lbl_ctrl      = NULL;
static lv_obj_t * lbl_status    = NULL;
static lv_obj_t * list_box      = NULL;
static lv_obj_t * sw_enable     = NULL;
static lv_obj_t * bar_actions   = NULL;
static lv_obj_t * btn_inc_lbl   = NULL;   /* "+ Add device" / "Stop" */
static lv_obj_t * btn_exc_lbl   = NULL;
static lv_timer_t * refresh_timer = NULL;

static zw_dev_t g_dev[MAX_ZW_DEV];
static int      g_dev_count = 0;
static int      g_inc_active = 0;
static int      g_exc_active = 0;
static char     g_status[192] = "";
static int      g_poll_ticks = 0;

/* ---- Meteradapter (Z-Wave smart meter) ------------------------------ */
static lv_obj_t * lbl_met_state = NULL;
static lv_obj_t * lbl_met_sub   = NULL;
static lv_obj_t * btn_pair_lbl  = NULL;
static int        g_met_found   = 0;     /* HAE_METER node present in Z-Wave list */
static int        g_pair_active = 0;     /* Z-Wave inclusion window open */
static lv_obj_t * g_met_confirm = NULL;  /* pair confirm dialog */

/* ===================================================================== */
/* XML helpers                                                           */
/* ===================================================================== */
/* Copy the text of <key>...</key> within [src,end) into out. Returns 1/0. */
static int xml_elem(const char * src, const char * end, const char * key,
                    char * out, size_t outsz) {
    char open[48];
    snprintf(open, sizeof open, "<%s>", key);
    const char * p = strstr(src, open);
    if (!p || p >= end) { if (out && outsz) out[0] = 0; return 0; }
    p += strlen(open);
    char close[48];
    snprintf(close, sizeof close, "</%s>", key);
    const char * q = strstr(p, close);
    if (!q || q > end) { if (out && outsz) out[0] = 0; return 0; }
    size_t n = (size_t)(q - p);
    if (n + 1 > outsz) n = outsz - 1;
    if (out) { memcpy(out, p, n); out[n] = 0; }
    return 1;
}

/* Parse zwave_response_buf (GetDevicesResponse XML) into g_dev[]. */
static void parse_devices(void) {
    g_dev_count = 0;
    const char * buf = zwave_response_buf;
    const char * ds = strstr(buf, "<devices>");
    const char * de = strstr(buf, "</devices>");
    if (!ds || !de) return;
    ds += strlen("<devices>");

    const char * scan = ds;
    while (g_dev_count < MAX_ZW_DEV) {
        const char * d0 = strstr(scan, "<device>");
        if (!d0 || d0 >= de) break;
        const char * d1 = strstr(d0, "</device>");
        if (!d1 || d1 > de) break;
        d0 += strlen("<device>");

        zw_dev_t * d = &g_dev[g_dev_count];
        memset(d, 0, sizeof *d);
        xml_elem(d0, d1, "uuid", d->uuid, sizeof d->uuid);
        xml_elem(d0, d1, "name", d->name, sizeof d->name);
        xml_elem(d0, d1, "type", d->type, sizeof d->type);

        char tmp[64];
        if (xml_elem(d0, d1, "nodeId", tmp, sizeof tmp) && tmp[0])
            d->node_id = atoi(tmp);
        else if (xml_elem(d0, d1, "internalAddress", tmp, sizeof tmp))
            d->node_id = atoi(tmp);

        /* Binary-switch capable = command class 0x25 in the CC list. */
        char cc[256] = "";
        if (xml_elem(d0, d1, "ccList", cc, sizeof cc) ||
            xml_elem(d0, d1, "supportedCC", cc, sizeof cc)) {
            d->is_switch = (strstr(cc, "25") != NULL);
        }
        if (xml_elem(d0, d1, "CurrentState", tmp, sizeof tmp) && tmp[0])
            d->state = atoi(tmp);
        else
            d->state = -1;

        if (d->uuid[0]) g_dev_count++;
        scan = d1 + strlen("</device>");
    }
}

/* ===================================================================== */
/* Meteradapter — status + actions                                       */
/* ===================================================================== */
static void scan_met_found(void) {
    if (!zwave_response_ready) return;
    g_met_found = (strstr(zwave_response_buf, "HAE_METER") != NULL);
}

static void update_meter_labels(void) {
    if (!lbl_met_state || !lbl_met_sub) return;
    int online = meter_state.connected;
    uint32_t col = online ? COL_GREEN : (g_met_found ? COL_AMBER : COL_RED);
    lv_obj_set_style_text_color(lbl_met_state, lv_color_hex(col), 0);

    if (online)
        lv_label_set_text_fmt(lbl_met_state, "Online  -  %.0f W", (double)meter_state.power_w);
    else if (g_met_found)
        lv_label_set_text(lbl_met_state, "Found - no live data");
    else
        lv_label_set_text(lbl_met_state, "Not found");

    if (g_pair_active)
        lv_label_set_text(lbl_met_sub,
            "Z-Wave inclusion active - press the button on the meter (auto-stops in 60 s).");
    else if (online)
        lv_label_set_text(lbl_met_sub, "Z-Wave smart meter - publishing on ElectricityFlowMeter.");
    else if (g_met_found)
        lv_label_set_text(lbl_met_sub, "Meter is paired but not sending. Check the P1/meter cable.");
    else
        lv_label_set_text(lbl_met_sub, "No HAE_METER node paired. Tap \"Pair via Z-Wave\" to add it.");
}

static void on_met_test(lv_event_t * e) {
    (void)e;
    boxtalk_zwave_get_devices();
    g_poll_ticks = 2;
    if (lbl_met_sub)
        lv_label_set_text(lbl_met_sub, "Querying Z-Wave + live flow...");
}

/* --- pair (Z-Wave inclusion for the meter) with confirm --------------- */
static void met_confirm_close(void) {
    if (g_met_confirm) { lv_obj_del(g_met_confirm); g_met_confirm = NULL; }
}
static void on_met_confirm_cancel(lv_event_t * e) { (void)e; met_confirm_close(); }
static void on_met_confirm_pair(lv_event_t * e) {
    (void)e;
    met_confirm_close();
    g_pair_active = 1;
    boxtalk_zwave_include(1);
    g_poll_ticks = 1;
}
static void on_pair_clicked(lv_event_t * e) {
    (void)e;
    if (g_pair_active) {
        boxtalk_zwave_include(0);
        g_pair_active = 0;
        return;
    }
    if (!scr_root) return;
    g_met_confirm = lv_obj_create(scr_root);
    lv_obj_set_size(g_met_confirm, DISP_HOR, DISP_VER);
    lv_obj_set_pos(g_met_confirm, 0, 0);
    lv_obj_set_style_bg_color(g_met_confirm, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_met_confirm, LV_OPA_70, 0);
    lv_obj_set_style_border_width(g_met_confirm, 0, 0);
    lv_obj_clear_flag(g_met_confirm, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * card = lv_obj_create(g_met_confirm);
    lv_obj_set_size(card, SX(640), SY(280));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * t = lv_label_create(card);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(t, SF(22), 0);
    lv_obj_set_width(t, SX(600));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_label_set_text(t,
        "Start Z-Wave inclusion?\n\n"
        "This opens a 60 s window to pair the smart meter. Trigger inclusion on "
        "the meter itself. Existing devices are not affected.");
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, SX(16), SY(14));

    lv_obj_t * go = lv_btn_create(card);
    lv_obj_set_size(go, SX(180), SY(52));
    lv_obj_align(go, LV_ALIGN_BOTTOM_RIGHT, SX(-12), SY(-12));
    lv_obj_set_style_bg_color(go, lv_color_hex(COL_OK), 0);
    lv_obj_set_style_radius(go, 10, 0);
    lv_obj_add_event_cb(go, on_met_confirm_pair, LV_EVENT_CLICKED, NULL);
    lv_obj_t * gl = lv_label_create(go); lv_label_set_text(gl, "Start"); lv_obj_center(gl);

    lv_obj_t * ca = lv_btn_create(card);
    lv_obj_set_size(ca, SX(180), SY(52));
    lv_obj_align(ca, LV_ALIGN_BOTTOM_LEFT, SX(12), SY(-12));
    lv_obj_set_style_bg_color(ca, lv_color_hex(COL_OFF), 0);
    lv_obj_set_style_radius(ca, 10, 0);
    lv_obj_add_event_cb(ca, on_met_confirm_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(ca); lv_label_set_text(cl, "Cancel"); lv_obj_center(cl);
}

/* ===================================================================== */
/* Event handlers (all UI thread; boxtalk_zwave_* are lock-guarded)       */
/* ===================================================================== */
static void on_switch_toggled(lv_event_t * e) {
    zw_dev_t * d = (zw_dev_t *)lv_event_get_user_data(e);
    lv_obj_t * sw = lv_event_get_target(e);
    int want = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    boxtalk_zwave_basic_set(d->uuid, want);
    snprintf(g_status, sizeof g_status, "%s -> %s", d->name, want ? "on" : "off");
    g_poll_ticks = 0;   /* refresh soon */
}

/* --- rename modal --------------------------------------------------- */
static char       g_rename_uuid[40];
static lv_obj_t * g_rename_ta = NULL;
static lv_obj_t * g_rename_modal = NULL;

static void rename_close(void) {
    if (g_rename_modal) { lv_obj_del(g_rename_modal); g_rename_modal = NULL; }
    g_rename_ta = NULL;
}
static void on_rename_cancel(lv_event_t * e) { (void)e; rename_close(); }
static void on_rename_save(lv_event_t * e) {
    (void)e;
    if (!g_rename_ta) return;
    /* Strip XML metacharacters so the name is safe inside the action XML. */
    const char * src = lv_textarea_get_text(g_rename_ta);
    char clean[64]; size_t o = 0;
    for (const char * p = src; *p && o + 1 < sizeof clean; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '<' || c == '>' || c == '&' || c == '"' || c == '\'' || c < 0x20)
            continue;
        clean[o++] = (char)c;
    }
    clean[o] = 0;
    boxtalk_zwave_set_name(g_rename_uuid, clean);
    snprintf(g_status, sizeof g_status, "Renamed to %s", clean);
    g_poll_ticks = 0;
    rename_close();
}
static void on_row_rename(lv_event_t * e) {
    zw_dev_t * d = (zw_dev_t *)lv_event_get_user_data(e);
    snprintf(g_rename_uuid, sizeof g_rename_uuid, "%s", d->uuid);

    g_rename_modal = lv_obj_create(scr_root);
    lv_obj_set_size(g_rename_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_rename_modal, 0, 0);
    lv_obj_set_style_bg_color(g_rename_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_rename_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(g_rename_modal, 0, 0);
    lv_obj_clear_flag(g_rename_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * card = lv_obj_create(g_rename_modal);
    lv_obj_set_size(card, SX(640), SY(340));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * t = lv_label_create(card);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(t, SF(22), 0);
    lv_label_set_text(t, "Rename device");
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, SX(8), SY(6));

    g_rename_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(g_rename_ta, true);
    lv_textarea_set_text(g_rename_ta, d->name);
    lv_obj_set_width(g_rename_ta, SX(600));
    lv_obj_align(g_rename_ta, LV_ALIGN_TOP_MID, 0, SY(44));

    lv_obj_t * kb = lv_keyboard_create(card);
    lv_obj_set_size(kb, SX(620), SY(190));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, SY(-50));
    lv_keyboard_set_textarea(kb, g_rename_ta);

    lv_obj_t * save = lv_btn_create(card);
    lv_obj_set_size(save, SX(130), SY(42));
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, SX(-8), SY(-4));
    lv_obj_set_style_bg_color(save, lv_color_hex(COL_OK), 0);
    lv_obj_add_event_cb(save, on_rename_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t * sl = lv_label_create(save); lv_label_set_text(sl, "Save");
    lv_obj_center(sl);

    lv_obj_t * cancel = lv_btn_create(card);
    lv_obj_set_size(cancel, SX(130), SY(42));
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, SX(8), SY(-4));
    lv_obj_set_style_bg_color(cancel, lv_color_hex(COL_OFF), 0);
    lv_obj_add_event_cb(cancel, on_rename_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(cancel); lv_label_set_text(cl, "Cancel");
    lv_obj_center(cl);
}

static void on_include(lv_event_t * e) {
    (void)e;
    if (g_exc_active) { boxtalk_zwave_exclude(0); g_exc_active = 0; }
    g_inc_active = !g_inc_active;
    boxtalk_zwave_include(g_inc_active);
    snprintf(g_status, sizeof g_status, g_inc_active
        ? "Inclusion active - trigger pairing on the device (auto-stops in 60 s)."
        : "Inclusion stopped.");
    g_poll_ticks = 0;
}
static void on_exclude(lv_event_t * e) {
    (void)e;
    if (g_inc_active) { boxtalk_zwave_include(0); g_inc_active = 0; }
    g_exc_active = !g_exc_active;
    boxtalk_zwave_exclude(g_exc_active);
    snprintf(g_status, sizeof g_status, g_exc_active
        ? "Removal active - trigger exclusion on the device (auto-stops in 30 s)."
        : "Removal stopped.");
    g_poll_ticks = 0;
}
static void on_heal(lv_event_t * e) {
    (void)e;
    boxtalk_zwave_heal();
    snprintf(g_status, sizeof g_status, "Heal requested - routing update runs in the background.");
}

static void on_enable_toggled(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_target(e);
    settings.enable_zwave = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    settings_save();
    if (!settings.enable_zwave && (g_inc_active || g_exc_active)) {
        if (g_inc_active) boxtalk_zwave_include(0);
        if (g_exc_active) boxtalk_zwave_exclude(0);
        g_inc_active = g_exc_active = 0;
    }
    snprintf(g_status, sizeof g_status, settings.enable_zwave
        ? "Control enabled." : "Enable control to manage devices.");
}

/* ===================================================================== */
/* List rendering                                                        */
/* ===================================================================== */
static void build_rows(void) {
    lv_obj_clean(list_box);

    if (g_dev_count == 0) {
        lv_obj_t * empty = lv_label_create(list_box);
        lv_obj_set_style_text_color(empty, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(empty, SF(18), 0);
        lv_label_set_text(empty,
            "No Z-Wave devices paired.\n"
            "Tap \"Add device\" and trigger inclusion on the device.");
        return;
    }

    for (int i = 0; i < g_dev_count; i++) {
        zw_dev_t * d = &g_dev[i];
        lv_obj_t * row = lv_obj_create(list_box);
        lv_obj_set_size(row, SX(940), SY(84));
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * nm = lv_label_create(row);
        lv_obj_set_style_text_font(nm, SF(22), 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(COL_TEXT_HI), 0);
        lv_label_set_text(nm, d->name[0] ? d->name : d->type);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, SX(4), 0);

        lv_obj_t * sub = lv_label_create(row);
        lv_obj_set_style_text_font(sub, SF(14), 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(COL_TEXT_DIM), 0);
        lv_label_set_text_fmt(sub, "%s   node %d", d->type, d->node_id);
        lv_obj_align(sub, LV_ALIGN_TOP_LEFT, SX(4), SY(34));

        int x = -4;
        if (d->is_switch && settings.enable_zwave) {
            lv_obj_t * sw = lv_switch_create(row);
            lv_obj_align(sw, LV_ALIGN_RIGHT_MID, SX(x), 0);
            if (d->state == 1) lv_obj_add_state(sw, LV_STATE_CHECKED);
            lv_obj_add_event_cb(sw, on_switch_toggled, LV_EVENT_VALUE_CHANGED, d);
            x -= 80;
        }
        if (settings.enable_zwave) {
            lv_obj_t * ren = lv_btn_create(row);
            lv_obj_set_size(ren, SX(110), SY(48));
            lv_obj_align(ren, LV_ALIGN_RIGHT_MID, SX(x), 0);
            lv_obj_set_style_bg_color(ren, lv_color_hex(0x2a4060), 0);
            lv_obj_add_event_cb(ren, on_row_rename, LV_EVENT_CLICKED, d);
            lv_obj_t * rl = lv_label_create(ren); lv_label_set_text(rl, "Rename");
            lv_obj_center(rl);
        }
    }
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    /* Meteradapter: scan for HAE_METER in the Z-Wave device list. */
    scan_met_found();

    /* Poll for a pending GetDevices response. */
    if (zwave_response_ready) {
        parse_devices();
        zwave_response_ready = 0;
        build_rows();
        if (lbl_ctrl)
            lv_label_set_text_fmt(lbl_ctrl, "%d device%s",
                g_dev_count, g_dev_count == 1 ? "" : "s");
    }

    /* Periodically ask for the device list (every ~3 s, faster right after an
     * action sets g_poll_ticks = 0). */
    if (g_poll_ticks <= 0) {
        boxtalk_zwave_get_devices();
        g_poll_ticks = 3;
    } else {
        g_poll_ticks--;
    }

    /* Meteradapter labels + pair button. */
    if (btn_pair_lbl)
        lv_label_set_text(btn_pair_lbl, g_pair_active ? "Stop pairing" : "Pair via Z-Wave");
    update_meter_labels();

    /* Status + control-gated widgets. */
    if (lbl_status)
        lv_label_set_text(lbl_status, g_status[0] ? g_status :
            (settings.enable_zwave ? "Control enabled. Add, remove or toggle devices."
                                   : "Enable control to manage devices."));
    if (sw_enable) {
        if (settings.enable_zwave) lv_obj_add_state(sw_enable, LV_STATE_CHECKED);
        else                       lv_obj_clear_state(sw_enable, LV_STATE_CHECKED);
    }
    if (bar_actions) {
        if (settings.enable_zwave) lv_obj_clear_flag(bar_actions, LV_OBJ_FLAG_HIDDEN);
        else                       lv_obj_add_flag(bar_actions, LV_OBJ_FLAG_HIDDEN);
    }
    if (btn_inc_lbl) lv_label_set_text(btn_inc_lbl, g_inc_active ? "Stop add" : "+ Add device");
    if (btn_exc_lbl) lv_label_set_text(btn_exc_lbl, g_exc_active ? "Stop remove" : "- Remove");
}

/* ===================================================================== */
/* Screen build                                                          */
/* ===================================================================== */
static void back_async(void * u) { (void)u; ui_pop(); }
static void on_back(lv_event_t * e) {
    (void)e;
    if (g_inc_active) { boxtalk_zwave_include(0); g_inc_active = 0; }
    if (g_exc_active) { boxtalk_zwave_exclude(0); g_exc_active = 0; }
    if (g_pair_active) { boxtalk_zwave_include(0); g_pair_active = 0; }
    lv_async_call(back_async, NULL);
}

static void on_scr_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) {
        if (refresh_timer) lv_timer_resume(refresh_timer);
        g_poll_ticks = 0;
    } else if (c == LV_EVENT_SCREEN_UNLOADED) {
        if (refresh_timer) lv_timer_pause(refresh_timer);
    }
}

static lv_obj_t * mk_action_btn(lv_obj_t * parent, const char * txt,
                                uint32_t col, lv_event_cb_t cb, int x,
                                lv_obj_t ** out_lbl) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, SX(210), SY(56));
    lv_obj_align(b, LV_ALIGN_LEFT_MID, SX(x), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, SF(22), 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    if (out_lbl) *out_lbl = l;
    return b;
}

lv_obj_t * screen_zwave_create(void) {
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_scroll_dir(scr_root, LV_DIR_VER);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_LOADED,   NULL);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_UNLOADED, NULL);

    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, SX(140), SY(52));
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, SX(20), SY(14));
    lv_obj_set_style_bg_color(back, lv_color_hex(0x3a4658), 0);
    lv_obj_set_style_radius(back, 10, 0);
    lv_obj_set_ext_click_area(back, 20);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(bl, SF(22), 0);
    lv_label_set_text(bl, "< Back");
    lv_obj_center(bl);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, "Z-Wave");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(180), SY(24));

    lbl_ctrl = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_ctrl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_ctrl, SF(14), 0);
    lv_label_set_text(lbl_ctrl, "querying...");
    lv_obj_align(lbl_ctrl, LV_ALIGN_TOP_LEFT, SX(300), SY(34));

    /* ---- Meteradapter card ------------------------------------------- */
    {
        lv_obj_t * card = lv_obj_create(scr_root);
        lv_obj_set_size(card, SX(980), SY(190));
        lv_obj_align(card, LV_ALIGN_TOP_LEFT, SX(22), SY(86));
        lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_pad_all(card, SX(16), 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * tt = lv_label_create(card);
        lv_obj_set_style_text_color(tt, lv_color_hex(COL_TEXT_HI), 0);
        lv_obj_set_style_text_font(tt, SF(22), 0);
        lv_label_set_text(tt, "Meteradapter");
        lv_obj_align(tt, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t * kd = lv_label_create(card);
        lv_obj_set_style_text_color(kd, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(kd, SF(14), 0);
        lv_label_set_text(kd, "Smart meter - Z-Wave");
        lv_obj_align(kd, LV_ALIGN_TOP_LEFT, SX(2), SY(34));

        lbl_met_state = lv_label_create(card);
        lv_obj_set_style_text_font(lbl_met_state, SF(22), 0);
        lv_obj_set_style_text_color(lbl_met_state, lv_color_hex(COL_TEXT_DIM), 0);
        lv_label_set_text(lbl_met_state, "...");
        lv_obj_align(lbl_met_state, LV_ALIGN_TOP_LEFT, 0, SY(64));

        lbl_met_sub = lv_label_create(card);
        lv_obj_set_style_text_font(lbl_met_sub, SF(14), 0);
        lv_obj_set_style_text_color(lbl_met_sub, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_width(lbl_met_sub, SX(700));
        lv_label_set_long_mode(lbl_met_sub, LV_LABEL_LONG_WRAP);
        lv_label_set_text(lbl_met_sub, "");
        lv_obj_align(lbl_met_sub, LV_ALIGN_TOP_LEFT, 0, SY(104));

        /* Buttons: Test + Pair via Z-Wave */
        lv_obj_t * tbtn = lv_btn_create(card);
        lv_obj_set_size(tbtn, SX(200), SY(48));
        lv_obj_align(tbtn, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_set_style_bg_color(tbtn, lv_color_hex(0x2a4060), 0);
        lv_obj_set_style_radius(tbtn, 10, 0);
        lv_obj_set_ext_click_area(tbtn, 8);
        lv_obj_add_event_cb(tbtn, on_met_test, LV_EVENT_CLICKED, NULL);
        lv_obj_t * tl = lv_label_create(tbtn);
        lv_obj_set_style_text_color(tl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(tl, SF(20), 0);
        lv_label_set_text(tl, "Test");
        lv_obj_center(tl);

        lv_obj_t * pbtn = lv_btn_create(card);
        lv_obj_set_size(pbtn, SX(220), SY(48));
        lv_obj_align(pbtn, LV_ALIGN_TOP_RIGHT, 0, SY(56));
        lv_obj_set_style_bg_color(pbtn, lv_color_hex(COL_OK), 0);
        lv_obj_set_style_radius(pbtn, 10, 0);
        lv_obj_set_ext_click_area(pbtn, 8);
        lv_obj_add_event_cb(pbtn, on_pair_clicked, LV_EVENT_CLICKED, NULL);
        btn_pair_lbl = lv_label_create(pbtn);
        lv_obj_set_style_text_color(btn_pair_lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(btn_pair_lbl, SF(20), 0);
        lv_label_set_text(btn_pair_lbl, "Pair via Z-Wave");
        lv_obj_center(btn_pair_lbl);
    }

    /* ---- Z-Wave device controls -------------------------------------- */
    lv_obj_t * en_lbl = lv_label_create(scr_root);
    lv_obj_set_style_text_color(en_lbl, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(en_lbl, SF(18), 0);
    lv_label_set_text(en_lbl, "Enable control");
    lv_obj_align(en_lbl, LV_ALIGN_TOP_RIGHT, SX(-90), SY(24));

    sw_enable = lv_switch_create(scr_root);
    lv_obj_align(sw_enable, LV_ALIGN_TOP_RIGHT, SX(-16), SY(18));
    if (settings.enable_zwave) lv_obj_add_state(sw_enable, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_enable, on_enable_toggled, LV_EVENT_VALUE_CHANGED, NULL);

    bar_actions = lv_obj_create(scr_root);
    lv_obj_set_size(bar_actions, SX(980), SY(76));
    lv_obj_align(bar_actions, LV_ALIGN_TOP_LEFT, SX(22), SY(294));
    lv_obj_set_style_bg_opa(bar_actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar_actions, 0, 0);
    lv_obj_set_style_pad_all(bar_actions, 0, 0);
    lv_obj_clear_flag(bar_actions, LV_OBJ_FLAG_SCROLLABLE);
    mk_action_btn(bar_actions, "+ Add device", COL_OK,   on_include, 0,   &btn_inc_lbl);
    mk_action_btn(bar_actions, "- Remove",     COL_WARN, on_exclude, 224, &btn_exc_lbl);
    mk_action_btn(bar_actions, "Heal network", 0x2a4060, on_heal,    448, NULL);
    if (!settings.enable_zwave) lv_obj_add_flag(bar_actions, LV_OBJ_FLAG_HIDDEN);

    lbl_status = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_status, SF(18), 0);
    lv_obj_set_width(lbl_status, SX(980));
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_status,
        settings.enable_zwave ? "Control enabled." : "Enable control to manage devices.");
    lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, SX(22), SY(376));

    list_box = lv_obj_create(scr_root);
    lv_obj_set_size(list_box, SX(980), SY(240));
    lv_obj_align(list_box, LV_ALIGN_TOP_LEFT, SX(22), SY(412));
    lv_obj_set_style_bg_opa(list_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_box, 0, 0);
    lv_obj_set_style_pad_all(list_box, 4, 0);
    lv_obj_set_style_pad_row(list_box, 10, 0);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    build_rows();

    update_meter_labels();

    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}

/* ---- master↔slave bridge -------------------------------------------------
 * The Toon's controller talks Z-Wave over BoxTalk on the master. On the WASM
 * slave we don't have a controller at all; the master serialises its current
 * g_dev[] into the SSE frame and the slave applies it here so the admin
 * screen's list view (build_rows() → g_dev) shows the same nodes. */
int zwave_dev_count(void) { return g_dev_count; }
int zwave_dev_at(int i, zwave_dev_t * out) {
    if (!out || i < 0 || i >= g_dev_count) return 0;
    *out = g_dev[i];
    return 1;
}
void zwave_set_devices_from_remote(int n, const zwave_dev_t * src) {
    if (n < 0) n = 0;
    if (n > MAX_ZW_DEV) n = MAX_ZW_DEV;
    for (int i = 0; i < n; i++) g_dev[i] = src[i];
    g_dev_count = n;
}
