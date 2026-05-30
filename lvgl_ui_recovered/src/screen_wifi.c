/*
 * WiFi screen — Settings -> WiFi.
 *
 * Native BoxTalk path via boxtalk.c to hcb_netcon (the Toon's network
 * manager, serviceId NetworkInformation, destuuid qb-...:hcb_netcon):
 *   GetWirelessNetworkInformation  -> current link (SSID/signal/IP)
 *   GetWirelessNetworks            -> scan results (briefly re-inits wlan0)
 *   SetWirelessNetworkInformation  -> connect (SSID + EncryptionKey)
 * Responses land in netcon_response_buf; we poll netcon_response_ready in the
 * refresh timer and parse on the UI thread. Internet-reachability is a
 * genuinely external check (http_fetch to 1.1.1.1) run on a worker thread.
 *
 * WPS: hcb_netcon does NOT expose WPS, and driving wpa_supplicant directly
 * behind hcb_netcon's back would fight its connection management — so WPS is
 * shown as unsupported rather than implemented unsafely.
 */
#include "screens.h"
#include "display.h"
#include "boxtalk.h"
#include "http.h"
#include <pthread.h>
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
#define COL_ACCENT   0x2a4060

#define MAX_AP 32

typedef struct {
    char ssid[48];
    int  quality;     /* 0..100 */
    int  signal_dbm;
    int  secured;     /* 0 = open, 1 = encrypted */
} ap_t;

static lv_obj_t * scr_root   = NULL;
static lv_obj_t * lbl_status = NULL;   /* connected SSID + signal + internet */
static lv_obj_t * lbl_hint   = NULL;
static lv_obj_t * list_box   = NULL;
static lv_obj_t * btn_scan_lbl = NULL;
static lv_timer_t * refresh_timer = NULL;

static ap_t g_ap[MAX_AP];
static int  g_ap_count = 0;
static char g_cur_ssid[48] = "";
static int  g_cur_quality = -1;
static char g_cur_ip[24] = "";
static volatile int g_internet = -1;   /* -1 unknown, 0 no, 1 yes */
static int  g_scanning = 0;
static int  g_status_ticks = 0;
static int  g_status_known = 0;   /* 0 until the first real status response lands;
                                     keeps "querying..." up instead of flashing a
                                     false "not connected" before the query returns */

/* ===================================================================== */
/* XML helper                                                            */
/* ===================================================================== */
static int xml_elem(const char * src, const char * end, const char * key,
                    char * out, size_t outsz) {
    char open[40]; snprintf(open, sizeof open, "<%s>", key);
    const char * p = strstr(src, open);
    if (!p || p >= end) { if (out && outsz) out[0] = 0; return 0; }
    p += strlen(open);
    char close[40]; snprintf(close, sizeof close, "</%s>", key);
    const char * q = strstr(p, close);
    if (!q || q > end) { if (out && outsz) out[0] = 0; return 0; }
    size_t n = (size_t)(q - p);
    if (n + 1 > outsz) n = outsz - 1;
    if (out) { memcpy(out, p, n); out[n] = 0; }
    return 1;
}

/* "78/100" -> 78 ; "-61 dBm" -> -61 */
static int parse_lead_int(const char * s) {
    while (*s == ' ' || *s == '\t') s++;
    return atoi(s);
}

/* ===================================================================== */
/* Response parsing                                                      */
/* ===================================================================== */
static void parse_status(const char * buf) {
    const char * end = buf + strlen(buf);
    char tmp[64];
    if (xml_elem(buf, end, "SSID", g_cur_ssid, sizeof g_cur_ssid) ||
        xml_elem(buf, end, "ESSID", g_cur_ssid, sizeof g_cur_ssid)) {}
    if (xml_elem(buf, end, "Link_Quality", tmp, sizeof tmp))
        g_cur_quality = parse_lead_int(tmp);
    xml_elem(buf, end, "ipAddress", g_cur_ip, sizeof g_cur_ip);
}

static void parse_scan(const char * buf) {
    g_ap_count = 0;
    const char * end = buf + strlen(buf);
    const char * scan = buf;
    while (g_ap_count < MAX_AP) {
        const char * a0 = strstr(scan, "<WirelessNetwork>");
        if (!a0 || a0 >= end) break;
        const char * a1 = strstr(a0, "</WirelessNetwork>");
        if (!a1) break;
        a0 += strlen("<WirelessNetwork>");

        ap_t * a = &g_ap[g_ap_count];
        memset(a, 0, sizeof *a);
        char tmp[64];
        if (!xml_elem(a0, a1, "ESSID", a->ssid, sizeof a->ssid) || !a->ssid[0]) {
            scan = a1 + strlen("</WirelessNetwork>");
            continue;
        }
        if (xml_elem(a0, a1, "Quality", tmp, sizeof tmp)) a->quality = parse_lead_int(tmp);
        if (xml_elem(a0, a1, "Signal", tmp, sizeof tmp))  a->signal_dbm = parse_lead_int(tmp);
        if (xml_elem(a0, a1, "Auth", tmp, sizeof tmp))
            a->secured = (strcmp(tmp, "OPEN") != 0 && tmp[0]);
        else
            a->secured = 1;
        g_ap_count++;
        scan = a1 + strlen("</WirelessNetwork>");
    }
}

/* ===================================================================== */
/* Internet reachability (genuinely external — worker thread)            */
/* ===================================================================== */
static void * internet_thread(void * arg) {
    (void)arg;
    char tmp[64];
    /* Captive-portal style check: returns "success" (200, small body, no
     * redirect) when the internet is reachable. */
    int rc = http_fetch("http://detectportal.firefox.com/success.txt", tmp, sizeof tmp);
    g_internet = (rc == 0 && strstr(tmp, "success")) ? 1 : 0;
    return NULL;
}
static void kick_internet_check(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, internet_thread, NULL) == 0) pthread_detach(t);
}

/* ===================================================================== */
/* Connect modal                                                         */
/* ===================================================================== */
static char       g_conn_ssid[48];
static lv_obj_t * g_conn_ta = NULL;
static lv_obj_t * g_conn_modal = NULL;

static void conn_close(void) {
    if (g_conn_modal) { lv_obj_del(g_conn_modal); g_conn_modal = NULL; }
    g_conn_ta = NULL;
}
static void on_conn_cancel(lv_event_t * e) { (void)e; conn_close(); }
static void on_conn_go(lv_event_t * e) {
    (void)e;
    char key[96] = "";
    if (g_conn_ta) {
        const char * src = lv_textarea_get_text(g_conn_ta);
        size_t o = 0;
        for (const char * p = src; *p && o + 1 < sizeof key; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '<' || c == '>' || c == '&' || c == '"' || c == '\'' || c < 0x20) continue;
            key[o++] = (char)c;
        }
        key[o] = 0;
    }
    boxtalk_wifi_connect(g_conn_ssid, key);
    lv_label_set_text_fmt(lbl_hint, "Connecting to %s ...", g_conn_ssid);
    g_status_ticks = 0;
    conn_close();
}

static void open_connect_modal(ap_t * a) {
    snprintf(g_conn_ssid, sizeof g_conn_ssid, "%s", a->ssid);

    g_conn_modal = lv_obj_create(scr_root);
    lv_obj_set_size(g_conn_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_conn_modal, 0, 0);
    lv_obj_set_style_bg_color(g_conn_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_conn_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(g_conn_modal, 0, 0);
    lv_obj_clear_flag(g_conn_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * card = lv_obj_create(g_conn_modal);
    lv_obj_set_size(card, SX(660), SY(340));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * t = lv_label_create(card);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(t, SF(22), 0);
    lv_label_set_text_fmt(t, "Connect to %s", a->ssid);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, SX(8), SY(6));

    if (a->secured) {
        g_conn_ta = lv_textarea_create(card);
        lv_textarea_set_one_line(g_conn_ta, true);
        lv_textarea_set_password_mode(g_conn_ta, true);
        lv_textarea_set_placeholder_text(g_conn_ta, "password");
        lv_obj_set_width(g_conn_ta, SX(620));
        lv_obj_align(g_conn_ta, LV_ALIGN_TOP_MID, 0, SY(44));

        lv_obj_t * kb = lv_keyboard_create(card);
        lv_obj_set_size(kb, SX(640), SY(190));
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, SY(-50));
        lv_keyboard_set_textarea(kb, g_conn_ta);
    } else {
        lv_obj_t * o = lv_label_create(card);
        lv_obj_set_style_text_color(o, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(o, SF(18), 0);
        lv_label_set_text(o, "Open network - no password needed.");
        lv_obj_align(o, LV_ALIGN_CENTER, 0, 0);
    }

    lv_obj_t * go = lv_btn_create(card);
    lv_obj_set_size(go, SX(150), SY(44));
    lv_obj_align(go, LV_ALIGN_BOTTOM_RIGHT, SX(-8), SY(-4));
    lv_obj_set_style_bg_color(go, lv_color_hex(COL_OK), 0);
    lv_obj_add_event_cb(go, on_conn_go, LV_EVENT_CLICKED, NULL);
    lv_obj_t * gl = lv_label_create(go); lv_label_set_text(gl, "Connect");
    lv_obj_center(gl);

    lv_obj_t * cancel = lv_btn_create(card);
    lv_obj_set_size(cancel, SX(130), SY(44));
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, SX(8), SY(-4));
    lv_obj_set_style_bg_color(cancel, lv_color_hex(COL_OFF), 0);
    lv_obj_add_event_cb(cancel, on_conn_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(cancel); lv_label_set_text(cl, "Cancel");
    lv_obj_center(cl);
}

static void on_ap_clicked(lv_event_t * e) {
    open_connect_modal((ap_t *)lv_event_get_user_data(e));
}

/* ===================================================================== */
/* Scan button                                                           */
/* ===================================================================== */
static void on_scan(lv_event_t * e) {
    (void)e;
    g_scanning = 1;
    boxtalk_wifi_scan();
    lv_label_set_text(lbl_hint, "Scanning... (wifi may blip for a moment)");
}

/* ===================================================================== */
/* Disconnect (with confirm — it drops the Toon off the network)         */
/* ===================================================================== */
static lv_obj_t * g_disc_modal = NULL;
static void disc_close(void) {
    if (g_disc_modal) { lv_obj_del(g_disc_modal); g_disc_modal = NULL; }
}
static void on_disc_cancel(lv_event_t * e) { (void)e; disc_close(); }
static void on_disc_confirm(lv_event_t * e) {
    (void)e;
    boxtalk_wifi_disconnect();
    if (lbl_hint) lv_label_set_text(lbl_hint,
        "Disconnected. Tap Scan to reconnect.");
    g_cur_ssid[0] = 0; g_internet = -1; g_status_ticks = 0;
    g_status_known = 1;   /* deliberate disconnect: "not connected" is correct now */
    disc_close();
}
static void on_disconnect(lv_event_t * e) {
    (void)e;
    g_disc_modal = lv_obj_create(scr_root);
    lv_obj_set_size(g_disc_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_disc_modal, 0, 0);
    lv_obj_set_style_bg_color(g_disc_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_disc_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(g_disc_modal, 0, 0);
    lv_obj_clear_flag(g_disc_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * card = lv_obj_create(g_disc_modal);
    lv_obj_set_size(card, SX(640), SY(240));
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
    lv_label_set_text_fmt(t, "Disconnect from %s?\nThe Toon will leave the network until it reconnects.",
                          g_cur_ssid[0] ? g_cur_ssid : "WiFi");
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, SX(12), SY(12));

    lv_obj_t * yes = lv_btn_create(card);
    lv_obj_set_size(yes, SX(170), SY(48));
    lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, SX(-8), SY(-4));
    lv_obj_set_style_bg_color(yes, lv_color_hex(COL_WARN), 0);
    lv_obj_add_event_cb(yes, on_disc_confirm, LV_EVENT_CLICKED, NULL);
    lv_obj_t * yl = lv_label_create(yes); lv_label_set_text(yl, "Disconnect");
    lv_obj_center(yl);

    lv_obj_t * no = lv_btn_create(card);
    lv_obj_set_size(no, SX(130), SY(48));
    lv_obj_align(no, LV_ALIGN_BOTTOM_LEFT, SX(8), SY(-4));
    lv_obj_set_style_bg_color(no, lv_color_hex(COL_OFF), 0);
    lv_obj_add_event_cb(no, on_disc_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t * nl = lv_label_create(no); lv_label_set_text(nl, "Cancel");
    lv_obj_center(nl);
}

/* ===================================================================== */
/* List rendering                                                        */
/* ===================================================================== */
static const char * bars_for(int quality) {
    if (quality >= 75) return LV_SYMBOL_WIFI "  ";
    if (quality >= 50) return LV_SYMBOL_WIFI " ";
    if (quality >= 25) return LV_SYMBOL_WIFI;
    return LV_SYMBOL_WIFI;
}

static void build_rows(void) {
    lv_obj_clean(list_box);
    if (g_ap_count == 0) {
        lv_obj_t * empty = lv_label_create(list_box);
        lv_obj_set_style_text_color(empty, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(empty, SF(18), 0);
        lv_label_set_text(empty, "Tap Scan to list nearby networks.");
        return;
    }
    for (int i = 0; i < g_ap_count; i++) {
        ap_t * a = &g_ap[i];
        lv_obj_t * row = lv_btn_create(list_box);
        lv_obj_set_size(row, SX(940), SY(64));
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x24385c), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_add_event_cb(row, on_ap_clicked, LV_EVENT_CLICKED, a);

        int connected = (strcmp(a->ssid, g_cur_ssid) == 0);

        lv_obj_t * nm = lv_label_create(row);
        lv_obj_set_style_text_font(nm, SF(22), 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(connected ? COL_OK : COL_TEXT_HI), 0);
        lv_label_set_text_fmt(nm, "%s%s", a->ssid, connected ? "  (connected)" : "");
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, SX(4), 0);

        lv_obj_t * meta = lv_label_create(row);
        lv_obj_set_style_text_font(meta, SF(18), 0);
        lv_obj_set_style_text_color(meta, lv_color_hex(COL_TEXT_DIM), 0);
        lv_label_set_text_fmt(meta, "%s %d%%  %s", bars_for(a->quality), a->quality,
                              a->secured ? LV_SYMBOL_KEYBOARD : "");
        lv_obj_align(meta, LV_ALIGN_RIGHT_MID, SX(-8), 0);
    }
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    if (netcon_response_ready) {
        if (strstr(netcon_response_buf, "GetWirelessNetworksResponse")) {
            parse_scan(netcon_response_buf);
            g_scanning = 0;
            build_rows();
            lv_label_set_text_fmt(lbl_hint, "%d network%s found.",
                                  g_ap_count, g_ap_count == 1 ? "" : "s");
        } else if (strstr(netcon_response_buf, "GetWirelessNetworkInformationResponse")) {
            parse_status(netcon_response_buf);
            g_status_known = 1;   /* we now have a real answer to render */
        }
        netcon_response_ready = 0;
    }

    /* Periodic status refresh (~5 s) + internet recheck. */
    if (g_status_ticks <= 0) {
        boxtalk_wifi_get_status();
        kick_internet_check();
        g_status_ticks = 5;
    } else {
        g_status_ticks--;
    }

    if (lbl_status) {
        const char * net = g_internet == 1 ? "internet OK"
                         : g_internet == 0 ? "no internet" : "checking...";
        if (g_cur_ssid[0])
            lv_label_set_text_fmt(lbl_status, "%s   %d%%   %s   %s",
                g_cur_ssid, g_cur_quality < 0 ? 0 : g_cur_quality,
                g_cur_ip[0] ? g_cur_ip : "-", net);
        else if (g_status_known)
            /* Only declare "not connected" once a real status query has come
             * back empty — never in the gap between screen-load and the first
             * response, which would flash a false "not connected". */
            lv_label_set_text(lbl_status, "not connected");
        else
            lv_label_set_text(lbl_status, "querying...");
    }
    if (btn_scan_lbl) lv_label_set_text(btn_scan_lbl, g_scanning ? "Scanning..." : "Scan");
}

/* ===================================================================== */
/* Screen build                                                          */
/* ===================================================================== */
static void back_async(void * u) { (void)u; ui_pop(); }
static void on_back(lv_event_t * e) { (void)e; lv_async_call(back_async, NULL); }

static void on_scr_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) {
        if (refresh_timer) lv_timer_resume(refresh_timer);
        g_status_ticks = 0;
        g_status_known = 0;   /* show "querying..." until the fresh query returns */
    } else if (c == LV_EVENT_SCREEN_UNLOADED) {
        if (refresh_timer) lv_timer_pause(refresh_timer);
    }
}

lv_obj_t * screen_wifi_create(void) {
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
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
    lv_label_set_text(title, "WiFi");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, SX(180), SY(24));

    lbl_status = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_status, SF(18), 0);
    lv_label_set_text(lbl_status, "querying...");
    lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, SX(280), SY(30));

    /* Scan button (top-right) */
    lv_obj_t * scan = lv_btn_create(scr_root);
    lv_obj_set_size(scan, SX(150), SY(52));
    lv_obj_align(scan, LV_ALIGN_TOP_RIGHT, SX(-16), SY(14));
    lv_obj_set_style_bg_color(scan, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_radius(scan, 10, 0);
    lv_obj_set_ext_click_area(scan, 12);
    lv_obj_add_event_cb(scan, on_scan, LV_EVENT_CLICKED, NULL);
    btn_scan_lbl = lv_label_create(scan);
    lv_obj_set_style_text_color(btn_scan_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(btn_scan_lbl, SF(22), 0);
    lv_label_set_text(btn_scan_lbl, "Scan");
    lv_obj_center(btn_scan_lbl);

    /* Disconnect button (left of Scan) */
    lv_obj_t * disc = lv_btn_create(scr_root);
    lv_obj_set_size(disc, SX(170), SY(52));
    lv_obj_align(disc, LV_ALIGN_TOP_RIGHT, SX(-176), SY(14));
    lv_obj_set_style_bg_color(disc, lv_color_hex(COL_WARN), 0);
    lv_obj_set_style_radius(disc, 10, 0);
    lv_obj_set_ext_click_area(disc, 8);
    lv_obj_add_event_cb(disc, on_disconnect, LV_EVENT_CLICKED, NULL);
    lv_obj_t * dl = lv_label_create(disc);
    lv_obj_set_style_text_color(dl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(dl, SF(22), 0);
    lv_label_set_text(dl, "Disconnect");
    lv_obj_center(dl);

    lbl_hint = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_hint, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_hint, SF(14), 0);
    lv_obj_set_width(lbl_hint, SX(980));
    lv_label_set_long_mode(lbl_hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_hint, "Tap Scan to list nearby networks.  WPS is not supported by the Toon's network manager.");
    lv_obj_align(lbl_hint, LV_ALIGN_TOP_LEFT, SX(22), SY(76));

    list_box = lv_obj_create(scr_root);
    lv_obj_set_size(list_box, SX(980), SY(470));
    lv_obj_align(list_box, LV_ALIGN_TOP_LEFT, SX(22), SY(110));
    lv_obj_set_style_bg_opa(list_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_box, 0, 0);
    lv_obj_set_style_pad_all(list_box, 4, 0);
    lv_obj_set_style_pad_row(list_box, 8, 0);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    build_rows();

    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}
