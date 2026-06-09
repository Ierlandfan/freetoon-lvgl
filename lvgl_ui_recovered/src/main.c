/*
 * Toon LVGL UI — boot entry. Initializes LVGL, display, input,
 * starts the BoxTalk client, then hands off to the screen stack.
 */
#include "lvgl/lvgl.h"
#include "fbdev.h"   /* vendored copy: cutout + page-flip extras (not the submodule's) */
#include "lv_drivers/indev/evdev.h"
#include "bootpick.h"
#include "boxtalk.h"
#include "display.h"
#include "screens.h"
#include "settings.h"
#include "layout.h"
#include "tile_slots.h"
#include "update_check.h"
#include "backlight.h"
#include "homewizard.h"
#include "energy_hist.h"
#include "weather.h"
#include "otgw.h"
#include "wastecollection.h"
#include "ventilation.h"
#include "homeassistant.h"
#include "doorbell.h"
#include "video.h"
#include "ui_cmd.h"
#include "healthcheck.h"
#include "pwa_server.h"
#include "packages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <time.h>

/* Wrap evdev_read so any PR event marks activity for the idle timer. */
static void evdev_read_with_activity(lv_indev_drv_t * drv, lv_indev_data_t * data) {
    evdev_read(drv, data);
    if (data->state == LV_INDEV_STATE_PR) ui_mark_activity();
}

/* DISP_HOR / DISP_VER come from display.h (per-target geometry).
 *
 * FULL-SCREEN draw buffer. The earlier "third-screen" buffer test measured zero
 * difference *in speed* (scrolling is pixel-bound — the same pixels are
 * recomputed regardless of buffer size). But buffer size is not about speed
 * here, it's about TEARING: with a 100-line buffer LVGL renders+flushes a
 * scroll in horizontal bands straight to the live framebuffer, so the panel
 * (and any VNC capture) shows a half-drawn frame — "incomplete text" during
 * scrolling. The CPU is never the limit (it caps ~76 %, i.e. the renderer keeps
 * up at ~50 fps with idle to spare), so we trade RAM for a single full-screen
 * buffer: the whole dirty region is composed off-screen and flushed in ONE
 * memcpy → complete, tear-free* frames. (*one seam max until vsync sync.)
 * 768 KB at 16bpp 800x480 — negligible on a 128 MB device. */
#define DRAW_BUF_LINES DISP_VER

static lv_color_t buf1[DISP_HOR * DRAW_BUF_LINES];

/* Framebuffer + LVGL + touch bring-up. Skipped entirely in headless WASM-host
 * mode so the stock qt-gui can own the panel while we still run the data
 * daemons + pwa_server. The disp/indev driver structs are static so LVGL's
 * retained pointers stay valid after this returns. */
static void init_display_and_input(void) {
    lv_init();

    /* Load config up front: the display-buffer choice (page-flip vs partial),
     * the Toon 1 touch calibration below, and everything after all read the
     * saved settings — not the compiled-in defaults. (Was loaded further down,
     * which made the page-flip guard see video_overlay=0 regardless of cfg.) */
    settings_load();

    fbdev_init();

    /* Partial-mode double buffer in cached RAM, then memcpy to the fb in
     * flush. We tried hardware page-flip (render straight into the fb pages +
     * FBIOPAN_DISPLAY) but it was a net loss: the i.MX27 fb is uncached, so
     * LVGL's blend/AA read-modify-write rendering into it is slower than
     * rendering cached + one streaming memcpy. See git history if revisiting
     * (e.g. on a cached-fb platform). */
    static lv_disp_draw_buf_t draw_buf;
    /* Single full-screen buffer (buf2 = NULL): compose the whole frame, then one
     * flush. A second buffer wouldn't help — the flush is synchronous, so there
     * is no render/flush overlap to pipeline. */
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, DISP_HOR * DRAW_BUF_LINES);
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = fbdev_flush;
    disp_drv.hor_res  = DISP_HOR;
    disp_drv.ver_res  = DISP_VER;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
#ifdef TOON1
    /* Toon 1's TSC2007 resistive panel needs a different device path AND
     * linear scaling of raw ADC values to pixel coords — see toon1_touch.c. */
    extern int  toon1_touch_init(void);
    extern void toon1_touch_read(lv_indev_drv_t *, lv_indev_data_t *);
    toon1_touch_init();
    indev_drv.read_cb = toon1_touch_read;
#else
    evdev_init();
    indev_drv.read_cb = evdev_read_with_activity;
#endif
    lv_indev_drv_register(&indev_drv);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Nudge our scheduling priority up. Rendering is bursty and shares the one
     * 400 MHz core with the background pollers + happ stack; perf traces showed
     * ~20% of each heavy scroll frame's wall time lost to preemption. A modest
     * nice (-5, not realtime) lets the renderer win the core during those short
     * bursts without starving the light, event-driven thermostat daemon. Needs
     * root (the Toon UI runs as root); harmless no-op otherwise. */
    setpriority(PRIO_PROCESS, 0, -5);

    /* Boot-picker mode: ui_launcher.sh runs us with --bootpick at boot.
     * We render only the picker screen and exit with rc 0 (freetoon)
     * or 99 (qt-gui) so the launcher can dispatch to the chosen binary.
     * No pollers, no full UI — keeps the picker snappy and avoids
     * touching the BoxTalk client which would race with the real toonui
     * we spawn right after. */
    if (argc > 1 && strcmp(argv[1], "--bootpick") == 0) {
        return bootpick_run();
    }

    /* Headless WASM-host mode (the toon_wasm_host.sh gate runs us as
     * `toonui --headless`): bring up the data daemons + pwa_server but NOT the
     * framebuffer/LVGL/touch, so the stock qt-gui owns the panel while this
     * Toon stays reachable on :10081 — as a master (serves its own state) or,
     * with client_mode set, as a slave re-serving another master. */
    int headless = (argc > 1 && strcmp(argv[1], "--headless") == 0);

    fprintf(stderr, "[main] starting toonui%s\n",
            headless ? " (headless WASM-host)" : "");

    if (!headless)
        init_display_and_input();

    layout_load_named(settings.active_layout);   /* home-tile layout: active preset */

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
        if (otgw_start() != 0)
            fprintf(stderr, "[main] otgw_start failed\n");
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
        extern int calendar_start(void);
        calendar_start();
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
    energy_hist_start();  /* record energy/gas/water for Stats + daily persistence */
    if (headless) {
        /* No display: the daemons + pwa_server own their own threads, so just
         * stay alive and let init NOT respawn us. We deliberately skip the
         * ambient-light/backlight control (qt-gui owns the panel + brightness),
         * ui_init, the doorbell overlay, and the LVGL loop. healthcheck's daily
         * _exit restart is caught by the toon_wasm_host.sh respawn gate. */
        fprintf(stderr, "[main] headless WASM-host up — qt-gui owns the panel, "
                        "pwa_server serving :10081\n");
        for (;;) pause();
    }

    backlight_als_start();  /* poll the ambient sensor off the UI thread (auto-brightness) */

    ui_init();

    /* Live video pipeline (Toon 1): warm-start vpu_stream so the Video tile
     * opens in well under a second, and the UNIX-socket command channel for
     * HA-style /show /hide triggers (doorbell_daemon -> /tmp/toonui.cmd).
     * No-ops on non-TOON1 builds and when video_enabled=0. */
    video_init();
    ui_cmd_start();

    /* Doorbell snapshot overlay — watches ha_state.doorbell_seq and shows the
     * camera snapshot fullscreen over any screen. No-op unless configured. */
    if (!settings.client_mode)
        doorbell_ui_init();

    fprintf(stderr, "[main] entering LVGL loop\n");
    uint32_t last_idle_check = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t last_tick_ms = (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
    /* Perf probe (set env TOONUI_PERF=1): times each lv_timer_handler() call
     * (= render + fb flush for that refresh) and prints, every ~2 s, the worst
     * and average busy-frame cost. fbdev_flush separately reports the fb write
     * bandwidth, so render_ms ≈ frame_ms − flush_ms. Lets us confirm whether
     * the bottleneck is rendering (CPU) or the framebuffer flush. */
    int perf = getenv("TOONUI_PERF") != NULL;
    struct timespec pt0, pt1; uint32_t perf_last = 0;
    double perf_sum = 0, perf_max = 0; int perf_n = 0;
    while (1) {
        if (perf) clock_gettime(CLOCK_MONOTONIC, &pt0);
        uint32_t delay_ms = lv_timer_handler();
        if (perf) {
            clock_gettime(CLOCK_MONOTONIC, &pt1);
            double ms = (pt1.tv_sec - pt0.tv_sec) * 1000.0 +
                        (pt1.tv_nsec - pt0.tv_nsec) / 1000000.0;
            if (ms > 2.0) { perf_sum += ms; perf_n++; if (ms > perf_max) perf_max = ms; }
            uint32_t nowp = (uint32_t)(pt1.tv_sec * 1000u + pt1.tv_nsec / 1000000u);
            if (nowp - perf_last > 2000) {
                if (perf_n) fprintf(stderr, "[perf] frame(render+flush): max=%.0fms avg=%.0fms over %d busy frames\n",
                                    perf_max, perf_sum / perf_n, perf_n);
                perf_last = nowp; perf_sum = 0; perf_max = 0; perf_n = 0;
            }
        }
        /* lv_timer_handler returns ms until the next timer is due.  Sleep
         * exactly that long so animation frames (news ticker scroll, fan
         * spin, etc.) fire at a regular cadence.  Cap at 10ms to keep the
         * idle check granular and input responsive; floor at 1ms to yield
         * the CPU.  The old fixed usleep(5000) made frame intervals vary
         * with render cost — a slow frame was followed by a 5ms sleep, a
         * fast frame by a 5ms sleep — so the animation jittered. */
        if (delay_ms < 1)  delay_ms = 1;
        if (delay_ms > 10) delay_ms = 10;
        usleep(delay_ms * 1000u);
        /* Advance LVGL's clock by REAL elapsed wall time, not a fixed
         * increment (see commit log for why). */
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint32_t now_ms = (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
        lv_tick_inc(now_ms - last_tick_ms);
        last_tick_ms = now_ms;
        /* Cheap idle check ~5x/sec; ui_idle_tick only acts on timeout. */
        uint32_t now = lv_tick_get();
        if (now - last_idle_check > 200) {
            ui_idle_tick();
            last_idle_check = now;
        }
    }
    return 0;
}
