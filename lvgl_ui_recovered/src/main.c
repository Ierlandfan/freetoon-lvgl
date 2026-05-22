/*
 * Toon LVGL UI — boot entry. Initializes LVGL, display, input,
 * starts the BoxTalk client, then hands off to the screen stack.
 */
#include "lvgl/lvgl.h"
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"
#include "bootpick.h"
#include "boxtalk.h"
#include "display.h"
#include "screens.h"
#include "settings.h"
#include "tile_slots.h"
#include "update_check.h"
#include "backlight.h"
#include "homewizard.h"
#include "weather.h"
#include "wastecollection.h"
#include "ventilation.h"
#include "homeassistant.h"
#include "doorbell.h"
#include "healthcheck.h"
#include "pwa_server.h"
#include "packages.h"
#include "weather.h"
#include "homewizard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Wrap evdev_read so any PR event marks activity for the idle timer. */
static void evdev_read_with_activity(lv_indev_drv_t * drv, lv_indev_data_t * data) {
    evdev_read(drv, data);
    if (data->state == LV_INDEV_STATE_PR) ui_mark_activity();
}

/* DISP_HOR / DISP_VER come from display.h (per-target geometry). */
#define DRAW_BUF_LINES 100

static lv_color_t buf1[DISP_HOR * DRAW_BUF_LINES];
static lv_color_t buf2[DISP_HOR * DRAW_BUF_LINES];

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Boot-picker mode: ui_launcher.sh runs us with --bootpick at boot.
     * We render only the picker screen and exit with rc 0 (freetoon)
     * or 99 (qt-gui) so the launcher can dispatch to the chosen binary.
     * No pollers, no full UI — keeps the picker snappy and avoids
     * touching the BoxTalk client which would race with the real toonui
     * we spawn right after. */
    if (argc > 1 && strcmp(argv[1], "--bootpick") == 0) {
        return bootpick_run();
    }

    fprintf(stderr, "[main] starting toonui\n");

    lv_init();
    fbdev_init();

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, DISP_HOR * DRAW_BUF_LINES);
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = fbdev_flush;
    disp_drv.hor_res  = DISP_HOR;
    disp_drv.ver_res  = DISP_VER;
    lv_disp_drv_register(&disp_drv);

    evdev_init();
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = evdev_read_with_activity;
    lv_indev_drv_register(&indev_drv);

    settings_load();

    /* Marketplace registry — load before boxtalk so the handshake's
     * tile_slots_subscribe_all() has something to subscribe to. */
    tile_slots_init();

    if (settings.client_mode) {
        /* Slave: mirror a master Toon over its PWA API; start NO local
         * integrations so we connect only to the master. */
        extern int client_link_start(void);
        if (client_link_start() != 0)
            fprintf(stderr, "[main] client_link_start failed\n");
    } else {
        if (boxtalk_start() != 0)
            fprintf(stderr, "[main] boxtalk_start failed\n");
        if (homewizard_start() != 0)
            fprintf(stderr, "[main] homewizard_start failed\n");
        extern int meteradapter_start(void);
        if (meteradapter_start() != 0)
            fprintf(stderr, "[main] meteradapter_start failed\n");
        if (weather_start() != 0)
            fprintf(stderr, "[main] weather_start failed\n");
        if (waste_start() != 0)
            fprintf(stderr, "[main] waste_start failed\n");
        if (vent_start() != 0)
            fprintf(stderr, "[main] vent_start failed\n");
        if (ha_start() != 0)
            fprintf(stderr, "[main] ha_start failed\n");
        extern int domoticz_start(void);
        if (domoticz_start() != 0)
            fprintf(stderr, "[main] domoticz_start failed\n");
        extern int news_start(void);
        news_start();
    }
    if (healthcheck_start() != 0)
        fprintf(stderr, "[main] healthcheck_start failed\n");
    if (pwa_start() != 0)
        fprintf(stderr, "[main] pwa_start failed\n");
    if (update_check_start() != 0)
        fprintf(stderr, "[main] update_check_start failed\n");
    if (!settings.client_mode)
        packages_start();

    extern void airhist_start(void);
    airhist_start();   /* record eCO2/TVOC history (RRD doesn't) for Stats graphs */
    backlight_als_start();  /* poll the ambient sensor off the UI thread (auto-brightness) */

    ui_init();

    /* Doorbell snapshot overlay — watches ha_state.doorbell_seq and shows the
     * camera snapshot fullscreen over any screen. No-op unless configured. */
    if (!settings.client_mode)
        doorbell_ui_init();

    fprintf(stderr, "[main] entering LVGL loop\n");
    uint32_t last_idle_check = 0;
    while (1) {
        lv_timer_handler();
        usleep(5000);
        lv_tick_inc(5);
        /* Cheap idle check ~5x/sec; ui_idle_tick is internally
           cheap and only acts when timeout elapsed. */
        uint32_t now = lv_tick_get();
        if (now - last_idle_check > 200) {
            ui_idle_tick();
            last_idle_check = now;
        }
    }
    return 0;
}
