/*
 * Headless Toon simulator — renders the LVGL UI to an offscreen framebuffer
 * and dumps it as a PPM, with no real hardware, X, or SDL. Lets us eyeball the
 * layout (especially the Toon-1 800x480 target) and iterate on screens without
 * a device. Build via `make TARGET=sim` (1024x600) or `make TARGET=sim1`
 * (800x480, -DTOON1). Hardware-bound bits (meteradapter / Z-Wave / WiFi / live
 * BoxTalk) are not emulated — the global state structs are filled with mock
 * data so the screens have something representative to draw.
 *
 *   ./toonui-sim <screen> [out.ppm]   render one screen directly
 *   ./toonui-sim all [outdir]         render every screen to <outdir>/sim_<name>.ppm
 *   ./toonui-sim nav <out.ppm> X,Y..  start at home, inject taps, dump active screen
 */
#include "lvgl/lvgl.h"
#include "display.h"
#include "screens.h"
#include "settings.h"
#include "homeassistant.h"
#include "layout.h"
#include "boxtalk.h"
#include "weather.h"
#include "wastecollection.h"
#include "homewizard.h"
#include "meteradapter.h"
#include "ventilation.h"
#include "efanlamp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static lv_color_t fb[DISP_HOR * DISP_VER];

static void sim_flush(lv_disp_drv_t * drv, const lv_area_t * a, lv_color_t * px) {
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++)
            fb[y * DISP_HOR + x] = *px++;
    lv_disp_flush_ready(drv);
}

static void dump_ppm(const char * path) {
    FILE * f = fopen(path, "wb");
    if (!f) { perror("ppm"); return; }
    fprintf(f, "P6\n%d %d\n255\n", DISP_HOR, DISP_VER);
    for (int i = 0; i < DISP_HOR * DISP_VER; i++) {
#if LV_COLOR_DEPTH == 16
        /* RGB565: channels are 5/6/5-bit. Scale up to full 8-bit (replicate
           the high bits into the low ones) so the PPM isn't a dark cast of
           the real RGB565 device output. */
        unsigned r5 = fb[i].ch.red, g6 = fb[i].ch.green, b5 = fb[i].ch.blue;
        unsigned char rgb[3] = {
            (unsigned char)((r5 << 3) | (r5 >> 2)),
            (unsigned char)((g6 << 2) | (g6 >> 4)),
            (unsigned char)((b5 << 3) | (b5 >> 2)),
        };
#else
        unsigned char rgb[3] = { fb[i].ch.red, fb[i].ch.green, fb[i].ch.blue };
#endif
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* ---- scripted pointer input ---- */
static lv_coord_t tap_x = 0, tap_y = 0;
static int        tap_pressed = 0;
static void sim_indev_read(lv_indev_drv_t * drv, lv_indev_data_t * data) {
    data->point.x = tap_x;
    data->point.y = tap_y;
    data->state = tap_pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}
static void pump(int frames) {
    for (int i = 0; i < frames; i++) { lv_timer_handler(); lv_tick_inc(16); usleep(1000); }
}
static void sim_tap(int x, int y) {
    tap_x = x; tap_y = y; tap_pressed = 1; pump(25);
    tap_pressed = 0;                          pump(25);
}

/* Representative data so the screens render like a live Toon. */
static void mock_state(void) {
    toon_state.indoor_temp   = 20.1f;
    toon_state.setpoint      = 18.5f;
    toon_state.ch_setpoint   = 60.0f;
    toon_state.humidity      = 53.0f;
    toon_state.boiler_in_temp  = 58.0f;
    toon_state.boiler_out_temp = 47.0f;
    toon_state.modulation_level = 42.0f;
    toon_state.boiler_type   = 0;
    toon_state.burner_on     = 1;
    toon_state.dhw_on        = 0;
    toon_state.program_state = 2;   /* scheme mode = TEMPOVERRIDE */
    toon_state.active_state  = 1;   /* live comfort preset = Home */
    /* Sim-only: SIM_MANUAL=1 emulates manual hold — happ reports programState=0
     * (PROG_MANUAL) yet activeState still names the value-matching preset, so we
     * can verify no preset/program highlight leaks through in manual mode. */
    if (getenv("SIM_MANUAL")) { toon_state.program_state = 0; toon_state.active_state = 0; }
    /* Render an EXACT live device state (verify against real happ data):
     * SIM_PS=<programState> SIM_AS=<activeState>. */
    if (getenv("SIM_PS")) toon_state.program_state = atoi(getenv("SIM_PS"));
    if (getenv("SIM_AS")) toon_state.active_state  = atoi(getenv("SIM_AS"));

    weather_state.connected   = 1;
    weather_state.current_temp = 22.6f;
    weather_state.feel_temp    = 22.0f;
    strcpy(weather_state.current_desc, "Zonnig");
    strcpy(weather_state.current_icon, "a");
    static const char * hlbl[] = { "Nu", "16:00", "19:00", "22:00", "01:00", "04:00" };
    static const char * hic[]  = { "a",  "a",     "a",     "aa",    "aa",    "aa"   };
    static const float  htmp[] = { 22.6f,26,24,19,16,15 };
    weather_state.hour_count = WEATHER_FORECAST_HOURS;
    for (int i = 0; i < WEATHER_FORECAST_HOURS; i++) {
        snprintf(weather_state.hours[i].label, 8, "%s", hlbl[i]);
        snprintf(weather_state.hours[i].icon, 8, "%s", hic[i]);
        weather_state.hours[i].temperature = htmp[i];
        weather_state.hours[i].wind_bft = 2;
        snprintf(weather_state.hours[i].wind_dir, 6, "ZO");
    }
    static const char * dday[] = { "vr 22-5","za 23-5","zo 24-5","ma 25-5","di 26-5" };
    static const char * dic[]  = { "a","b","f","q","a" };
    weather_state.day_count = WEATHER_FORECAST_DAYS;
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        snprintf(weather_state.days[i].day, 16, "%s", dday[i]);
        snprintf(weather_state.days[i].icon, 8, "%s", dic[i]);
        weather_state.days[i].min_temp = 12 + i;
        weather_state.days[i].max_temp = 22 + i;
        weather_state.days[i].wind_bft = 3;
        snprintf(weather_state.days[i].wind_dir, 6, "ZW");
    }

    /* Energy/gas so the dim-screen usage bars render. Both auto-scale, so a
     * single frame pins them full (first reading == running max). */
    meter_state.connected = 1;
    meter_state.power_w   = 384.0f;
    hw_state.connected_p1 = 1;
    hw_state.power_w      = 384.0f;
    hw_state.gas_hour_m3  = 0.0f;   /* boiler idle → empty gas channel (visible track) */

    /* Itho ventilation so the dim_stock fan spinner renders ("Low 31%"). */
    vent_state.connected     = 1;
    vent_state.itho_online   = 1;
    vent_state.speed_pct     = 31;
    vent_state.fan_rpm       = 900;
    vent_state.remaining_min = 0;
    snprintf(vent_state.fan_info, sizeof vent_state.fan_info, "%s", "low");

    /* ESPHome BLE fan+lamp so the dim_stock BLE spinner renders. */
    efanlamp.connected = 1;
    efanlamp.fan_on    = 1;
    efanlamp.fan_speed = 3;
    efanlamp.light_on  = 1;
    efanlamp.light_brightness = 80;

    /* Waste: plastic today, paper + rest later (windowed logic picks). */
    waste_state.connected = 1;
    struct { const char * lbl; const char * date; } w[] = {
        { "GFT",       "2026-05-27" },
        { "Plastic",   "2026-05-22" },
        { "Papier",    "2026-05-28" },
        { "Restafval", "2026-05-30" },
    };
    for (int i = 0; i < WASTE_TYPES; i++) {
        snprintf(waste_state.items[i].label, sizeof waste_state.items[i].label, "%s", w[i].lbl);
        snprintf(waste_state.items[i].date,  sizeof waste_state.items[i].date,  "%s", w[i].date);
    }

    /* HA devices so the Devices screen renders populated (one of each type). */
    settings.enable_ha = 1;
    /* Sim-only: SIM_BACKEND=domoticz flips the active lights backend so the
     * backend-aware home handle/Devices routing can be verified both ways. */
    if (getenv("SIM_BACKEND") && !strcmp(getenv("SIM_BACKEND"), "domoticz")) {
        settings.enable_domoticz = 1;
        settings.enable_ha = 0;
    }
    ha_state.connected = 1;
    /* Sim-only: SIM_TRACKERS=1 seeds the stock-home Trackers tile (one tile,
     * 3 rows) so the new tile can be rendered/verified headless. */
    if (getenv("SIM_TRACKERS")) {
        snprintf(settings.life360_a_entity, sizeof settings.life360_a_entity, "device_tracker.auto");
        snprintf(settings.life360_a_name,   sizeof settings.life360_a_name,   "Auto");
        snprintf(settings.life360_b_entity, sizeof settings.life360_b_entity, "device_tracker.google_pixel_6a");
        snprintf(settings.life360_b_name,   sizeof settings.life360_b_name,   "Telefoon");
        snprintf(settings.life360_c_entity, sizeof settings.life360_c_entity, "device_tracker.life360_member");
        snprintf(settings.life360_c_name,   sizeof settings.life360_c_name,   "Caja");
        snprintf(ha_state.loc_a, sizeof ha_state.loc_a, "Niet thuis");
        snprintf(ha_state.loc_b, sizeof ha_state.loc_b, "Thuis");
        snprintf(ha_state.loc_c, sizeof ha_state.loc_c, "Breezand");
        snprintf(settings.stock_tiles, sizeof settings.stock_tiles,
                 "clock,trackers,power,waterp,indoor,weather,vent,agenda,empty,empty,empty,empty,empty,empty,empty,empty");
    }
    struct { int type; const char * ent; const char * name; int on; int bri; int pos; int pin; } d[] = {
        { HADEV_LIGHT,  "light.woonkamer",  "Woonkamer",   1, 200, -1, 0 },
        { HADEV_LIGHT,  "light.keuken",      "Keuken",      0,  -1, -1, 0 },
        { HADEV_COVER,  "cover.gordijnen",   "Gordijnen",   0,  -1, 60, 1 },
        { HADEV_SWITCH, "switch.tuinpomp",   "Tuinpomp",    1,  -1, -1, 0 },
        { HADEV_SCRIPT, "script.avondmodus", "Avondmodus",  0,  -1, -1, 0 },
        { HADEV_SCENE,  "scene.film",        "Filmavond",   0,  -1, -1, 0 },
    };
    ha_device_count = (int)(sizeof(d)/sizeof(d[0]));
    for (int i = 0; i < ha_device_count; i++) {
        ha_device_t * D = &ha_devices[i];
        memset((void *)D, 0, sizeof *D);
        D->type = d[i].type;
        snprintf(D->entity_id, sizeof D->entity_id, "%s", d[i].ent);
        snprintf(D->name, sizeof D->name, "%s", d[i].name);
        D->available = 1; D->on = d[i].on; D->brightness = d[i].bri;
        D->position = d[i].pos; D->pin_home = d[i].pin;
        if (d[i].type == HADEV_COVER) snprintf(D->state, sizeof D->state, "%s", d[i].pos > 0 ? "open" : "closed");
    }
}

typedef lv_obj_t * (*create_fn)(void);
static const struct { const char * name; create_fn fn; } SCREENS[] = {
    { "home",        screen_home_create },
    { "home_stock",  screen_home_stock_create },
    { "dim",         screen_dim_create },
    { "dim_stock",   screen_dim_stock_create },
    { "thermostat",  screen_thermostat_create },
    { "settings",    screen_settings_create },
    { "schedule",    screen_schedule_create },
    { "stats",       screen_stats_create },
    { "forecast",    screen_forecast_create },
    { "heater",      screen_heater_advanced_create },
    { "vent_remote", screen_vent_remote_create },
    { "vent_adv",    screen_vent_advanced_create },
    { "lights",      screen_lights_create },
    { "marketplace", screen_marketplace_create },
    { "zwave",       screen_zwave_create },
    { "wifi",        screen_wifi_create },
    { "adapters",    screen_adapters_create },
    { "appliances",  screen_appliances_create },
    { "domoticz",    screen_domoticz_create },
    { "ha_devices",  screen_ha_devices_create },
    { "layout",      screen_layout_editor_create },
    { "dim_layout",  screen_dim_layout_editor_create },
    { "dim_grid",    screen_dim_grid_create },
    { "bootpick",    screen_bootpick_create },
    { "crypto_pick", screen_crypto_picker_create },
    { "crypto",      screen_crypto_create },
};
#define N_SCREENS ((int)(sizeof SCREENS / sizeof SCREENS[0]))

static void render_one(create_fn fn, const char * out) {
    lv_obj_t * scr = fn();
    lv_scr_load(scr);
    pump(150);
    dump_ppm(out);
}

int main(int argc, char ** argv) {
    lv_init();
    static lv_disp_draw_buf_t db;
    static lv_color_t line_buf[DISP_HOR * 60];
    lv_disp_draw_buf_init(&db, line_buf, NULL, DISP_HOR * 60);
    static lv_disp_drv_t dd;
    lv_disp_drv_init(&dd);
    dd.draw_buf = &db;
    dd.flush_cb = sim_flush;
    dd.hor_res  = DISP_HOR;
    dd.ver_res  = DISP_VER;
    lv_disp_drv_register(&dd);

    static lv_indev_drv_t id;
    lv_indev_drv_init(&id);
    id.type = LV_INDEV_TYPE_POINTER;
    id.read_cb = sim_indev_read;
    lv_indev_drv_register(&id);

    settings_load();   /* defaults when /mnt/data/toonui.cfg is absent */
    mock_state();
    dim_layout_load();  /* dim block layout (seeds defaults if no file) */
    /* Sim-only: exercise a custom layout. $TOONUI_SIM_CUSTOM_LAYOUT turns it on
     * and loads $TOONUI_DATA_DIR/toonui_layout.cfg so screens render from it. */
    if (getenv("TOONUI_SIM_CUSTOM_LAYOUT")) {
        settings.custom_layout_enabled = 1;
        layout_load_named("");
    }

    const char * mode = (argc > 1) ? argv[1] : "home";

    if (!strcmp(mode, "all")) {
        const char * dir = (argc > 2) ? argv[2] : "/tmp";
        char path[256];
        for (int i = 0; i < N_SCREENS; i++) {
            snprintf(path, sizeof path, "%s/sim_%s.ppm", dir, SCREENS[i].name);
            render_one(SCREENS[i].fn, path);
            fprintf(stderr, "[sim] %-12s -> %s  (%dx%d)\n", SCREENS[i].name, path, DISP_HOR, DISP_VER);
        }
        return 0;
    }

    if (!strcmp(mode, "nav")) {
        const char * out = (argc > 2) ? argv[2] : "/tmp/sim_nav.ppm";
        ui_init();                 /* home + ui stack so ui_push navigation works */
        pump(100);
        if (getenv("TOONUI_SIM_PAGE2")) { screen_home_force_page(1); pump(40); }  /* page 2 is gesture-only */
        for (int a = 3; a < argc; a++) {
            int x, y;
            if (sscanf(argv[a], "%d,%d", &x, &y) == 2) sim_tap(x, y);
        }
        pump(60);
        dump_ppm(out);
        fprintf(stderr, "[sim] nav (%d taps) -> %s  (%dx%d)\n", argc - 3, out, DISP_HOR, DISP_VER);
        return 0;
    }

    /* single named screen, optionally followed by "x,y" taps to drive it */
    const char * out = (argc > 2) ? argv[2] : "/tmp/sim.ppm";
    create_fn fn = screen_home_create;
    for (int i = 0; i < N_SCREENS; i++)
        if (!strcmp(mode, SCREENS[i].name)) { fn = SCREENS[i].fn; break; }
    lv_obj_t * scr = fn();
    lv_scr_load(scr);
    pump(150);
    int taps = 0;
    for (int a = 3; a < argc; a++) {
        int x, y;
        if (sscanf(argv[a], "%d,%d", &x, &y) == 2) { sim_tap(x, y); taps++; }
    }
    pump(60);
    dump_ppm(out);
    fprintf(stderr, "[sim] %s (%d taps) -> %s  (%dx%d)\n", mode, taps, out, DISP_HOR, DISP_VER);
    return 0;
}
