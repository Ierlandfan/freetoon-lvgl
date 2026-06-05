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
#include "layout.h"
#include "boxtalk.h"
#include "weather.h"
#include "wastecollection.h"
#include "homewizard.h"
#include "meteradapter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#ifdef USE_SDL
#include "lv_drivers/sdl/sdl.h"
#endif

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
        unsigned char rgb[3] = { fb[i].ch.red, fb[i].ch.green, fb[i].ch.blue };
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
}

typedef lv_obj_t * (*create_fn)(void);
static const struct { const char * name; create_fn fn; } SCREENS[] = {
    { "home",        screen_home_create },
    { "dim",         screen_dim_create },
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
    { "domoticz",    screen_domoticz_create },
    { "layout",      screen_layout_editor_create },
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
#ifdef USE_SDL
    sdl_init();
#endif
    static lv_disp_draw_buf_t db;
    static lv_color_t line_buf[DISP_HOR * 60];
    lv_disp_draw_buf_init(&db, line_buf, NULL, DISP_HOR * 60);
    static lv_disp_drv_t dd;
    lv_disp_drv_init(&dd);
    dd.draw_buf = &db;
#ifdef USE_SDL
    dd.flush_cb = sdl_display_flush;
#else
    dd.flush_cb = sim_flush;
#endif
    dd.hor_res  = DISP_HOR;
    dd.ver_res  = DISP_VER;
    lv_disp_drv_register(&dd);

    static lv_indev_drv_t id;
    lv_indev_drv_init(&id);
    id.type = LV_INDEV_TYPE_POINTER;
#ifdef USE_SDL
    id.read_cb  = sdl_mouse_read;
#else
    id.read_cb = sim_indev_read;
#endif
    lv_indev_drv_register(&id);

    settings_load();   /* defaults when /mnt/data/toonui.cfg is absent */
    mock_state();
    /* Sim-only: exercise a custom layout. $TOONUI_SIM_CUSTOM_LAYOUT turns it on
     * and loads $TOONUI_DATA_DIR/toonui_layout.cfg so screens render from it. */
    if (getenv("TOONUI_SIM_CUSTOM_LAYOUT")) {
        settings.custom_layout_enabled = 1;
        layout_load_named("");
    }

    const char * mode = (argc > 1) ? argv[1] : "home";

#ifdef USE_SDL
    ui_init(); 
    
    uint32_t last_idle_check = 0;
    while(1) {
        lv_timer_handler(); 
        usleep(5000);
        lv_tick_inc(5);

        uint32_t now = lv_tick_get();
        if (now - last_idle_check > 200) {
            ui_idle_tick();
            last_idle_check = now;
        }
    }
    return 0;
#endif
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
