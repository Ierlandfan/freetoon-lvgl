/*
 * freetoon ESP32 client — "Cheap Yellow Display" (ESP32-2432S028R).
 *
 * A small wall display that mirrors + controls the Toon over its PWA API.
 * No BoxTalk: it just polls GET /api/state and POSTs /api/setpoint and
 * /api/program over the LAN.
 *
 * Display: ILI9341 320x240 via TFT_eSPI (configured in platformio.ini).
 * Touch:   XPT2046 on a SEPARATE SPI bus (the CYD gotcha) — pins below.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include "config.h"

/* ---- CYD touch pins (separate VSPI bus from the display's HSPI) ---- */
#define TOUCH_CLK  25
#define TOUCH_MISO 39
#define TOUCH_MOSI 32
#define TOUCH_CS   33
#define TOUCH_IRQ  36

static const uint16_t SCR_W = 320, SCR_H = 240;

static TFT_eSPI tft;
static SPIClass touchSPI(VSPI);
static XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

/* LVGL draw buffer (10 lines tall). */
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[SCR_W * 10];

/* ---- shared state pulled from /api/state ---- */
static float   g_indoor = NAN, g_setpoint = NAN, g_pressure = NAN, g_humid = NAN;
static int     g_program = -1, g_burner = 0, g_connected = 0, g_eco2 = 0, g_tvoc = 0;
static volatile float g_pending_sp = NAN;   /* setpoint the user just nudged */

/* ---- UI widgets ---- */
static lv_obj_t *lbl_indoor, *lbl_setpoint, *lbl_mode, *lbl_status, *lbl_conn;

static const char *program_name(int p) {
    switch (p) { case 0: return "Comfort"; case 1: return "Home";
                 case 2: return "Sleep";  case 3: return "Away"; default: return "Manual"; }
}

/* ===================== LVGL display + touch glue ===================== */
static void disp_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(a->x1, a->y1, w, h);
    tft.pushColors((uint16_t *)px, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(d);
}

/* XPT2046 raw → screen pixels. These calibration bounds are typical for the
 * CYD; tweak if taps land off. */
#define TS_MINX 200
#define TS_MAXX 3700
#define TS_MINY 240
#define TS_MAXY 3800
static void touch_read(lv_indev_drv_t *d, lv_indev_data_t *data) {
    if (!ts.touched()) { data->state = LV_INDEV_STATE_REL; return; }
    TS_Point p = ts.getPoint();
    int x = map(p.x, TS_MINX, TS_MAXX, 0, SCR_W - 1);
    int y = map(p.y, TS_MINY, TS_MAXY, 0, SCR_H - 1);
    if (x < 0) x = 0; if (x >= SCR_W) x = SCR_W - 1;
    if (y < 0) y = 0; if (y >= SCR_H) y = SCR_H - 1;
    data->point.x = x; data->point.y = y;
    data->state = LV_INDEV_STATE_PR;
}

/* ===================== HTTP to the Toon ===================== */
static bool http_get_state(String &out) {
    HTTPClient http;
    String url = String("http://") + TOON_HOST + ":" + TOON_PORT + "/api/state";
    http.setConnectTimeout(4000);
    http.setTimeout(5000);
    if (!http.begin(url)) return false;
    int code = http.GET();
    bool ok = (code == 200);
    if (ok) out = http.getString();
    http.end();
    return ok;
}

static void http_post(const char *path, const String &body) {
    HTTPClient http;
    String url = String("http://") + TOON_HOST + ":" + TOON_PORT + path;
    if (!http.begin(url)) return;
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);
    http.POST(body);
    http.end();
}

static void post_setpoint(float v) {
    char body[48];
    snprintf(body, sizeof body, "{\"value\":\"%.2f\"}", v);
    http_post("/api/setpoint", body);
}
static void post_program(int state) {
    char body[32];
    snprintf(body, sizeof body, "{\"state\":%d}", state);
    http_post("/api/program", body);
}

static void parse_state(const String &json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) { g_connected = 0; return; }
    g_indoor   = doc["indoor_temp"]   | NAN;
    g_setpoint = doc["setpoint"]      | NAN;
    g_pressure = doc["water_pressure"]| NAN;
    g_humid    = doc["humidity"]      | NAN;
    g_program  = doc["program_state"] | -1;
    g_burner   = doc["burner_on"]     | 0;
    g_eco2     = doc["eco2"]          | 0;
    g_tvoc     = doc["tvoc"]          | 0;
    g_connected = doc["connected"]    | 0;
}

/* ===================== UI ===================== */
static void on_sp_down(lv_event_t *e) {
    (void)e;
    float base = !isnan(g_pending_sp) ? g_pending_sp : g_setpoint;
    if (isnan(base)) return;
    g_pending_sp = base - SP_STEP;
    post_setpoint(g_pending_sp);
}
static void on_sp_up(lv_event_t *e) {
    (void)e;
    float base = !isnan(g_pending_sp) ? g_pending_sp : g_setpoint;
    if (isnan(base)) return;
    g_pending_sp = base + SP_STEP;
    post_setpoint(g_pending_sp);
}
static void on_mode(lv_event_t *e) {
    /* cycle Comfort→Home→Sleep→Away */
    int next = (g_program + 1) & 3;
    post_program(next);
}

static void build_ui(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0e1a2a), 0);

    lbl_conn = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_conn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0x88aabb), 0);
    lv_label_set_text(lbl_conn, "...");
    lv_obj_align(lbl_conn, LV_ALIGN_TOP_RIGHT, -6, 6);

    lbl_indoor = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_indoor, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_indoor, lv_color_hex(0xffcc44), 0);
    lv_label_set_text(lbl_indoor, "--.-");
    lv_obj_align(lbl_indoor, LV_ALIGN_TOP_MID, 0, 18);

    /* Setpoint row: [-]  setpoint  [+] */
    lv_obj_t *bdn = lv_btn_create(scr);
    lv_obj_set_size(bdn, 64, 64);
    lv_obj_align(bdn, LV_ALIGN_LEFT_MID, 8, 10);
    lv_obj_set_style_bg_color(bdn, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(bdn, on_sp_down, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ldn = lv_label_create(bdn);
    lv_obj_set_style_text_font(ldn, &lv_font_montserrat_28, 0);
    lv_label_set_text(ldn, "-"); lv_obj_center(ldn);

    lbl_setpoint = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_setpoint, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_setpoint, lv_color_hex(0xffffff), 0);
    lv_label_set_text(lbl_setpoint, "--.-");
    lv_obj_align(lbl_setpoint, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t *bup = lv_btn_create(scr);
    lv_obj_set_size(bup, 64, 64);
    lv_obj_align(bup, LV_ALIGN_RIGHT_MID, -8, 10);
    lv_obj_set_style_bg_color(bup, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(bup, on_sp_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lup = lv_label_create(bup);
    lv_obj_set_style_text_font(lup, &lv_font_montserrat_28, 0);
    lv_label_set_text(lup, "+"); lv_obj_center(lup);

    /* Mode button (tap to cycle preset). */
    lv_obj_t *bmode = lv_btn_create(scr);
    lv_obj_set_size(bmode, 150, 40);
    lv_obj_align(bmode, LV_ALIGN_BOTTOM_MID, 0, -44);
    lv_obj_set_style_bg_color(bmode, lv_color_hex(0x3a4658), 0);
    lv_obj_add_event_cb(bmode, on_mode, LV_EVENT_CLICKED, NULL);
    lbl_mode = lv_label_create(bmode);
    lv_obj_set_style_text_font(lbl_mode, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_mode, "Manual"); lv_obj_center(lbl_mode);

    lbl_status = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x88aabb), 0);
    lv_label_set_text(lbl_status, "");
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void refresh_ui(void) {
    if (!isnan(g_indoor)) lv_label_set_text_fmt(lbl_indoor, "%.1f\xC2\xB0", g_indoor);
    float sp = !isnan(g_pending_sp) ? g_pending_sp : g_setpoint;
    if (!isnan(sp)) lv_label_set_text_fmt(lbl_setpoint, "%.1f\xC2\xB0", sp);
    lv_label_set_text(lbl_mode, program_name(g_program));
    lv_label_set_text_fmt(lbl_status, "%.1f bar   RH %.0f%%   CO2 %d   %s",
                          isnan(g_pressure) ? 0.0f : g_pressure,
                          isnan(g_humid) ? 0.0f : g_humid, g_eco2,
                          g_burner ? "burner on" : "idle");
    lv_label_set_text(lbl_conn, g_connected ? "online" : "offline");
    lv_obj_set_style_text_color(lbl_conn,
        lv_color_hex(g_connected ? 0x66cc88 : 0xcc6666), 0);
}

/* ===================== setup / loop ===================== */
static uint32_t last_poll = 0;

void setup() {
    Serial.begin(115200);

    tft.begin();
    tft.setRotation(1);              /* landscape 320x240 */
    tft.fillScreen(TFT_BLACK);
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    ts.begin(touchSPI);
    ts.setRotation(1);

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, SCR_W * 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCR_W;
    disp_drv.ver_res = SCR_H;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read;
    lv_indev_drv_register(&indev_drv);

    build_ui();
    lv_label_set_text(lbl_status, "connecting WiFi...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void loop() {
    lv_timer_handler();
    delay(5);

    uint32_t now = millis();
    if (now - last_poll >= POLL_MS) {
        last_poll = now;
        if (WiFi.status() == WL_CONNECTED) {
            String json;
            if (http_get_state(json)) {
                parse_state(json);
                /* The Toon confirmed our setpoint — drop the optimistic pending. */
                if (!isnan(g_pending_sp) && !isnan(g_setpoint) &&
                    fabsf(g_pending_sp - g_setpoint) < 0.05f)
                    g_pending_sp = NAN;
            } else {
                g_connected = 0;
            }
            refresh_ui();
        } else {
            lv_label_set_text(lbl_conn, "no wifi");
            lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0xcc6666), 0);
        }
    }
}
