/*
 * freetoon-WASM entry point.
 *
 * SDL2's JNI/JS shim runs SDL_main once the canvas is available. We initialise
 * LVGL, wire a small inline SDL display/input adapter (instead of pulling in
 * lv_drivers/sdl, which uses an SDL_Thread that doesn't survive a non-pthread
 * Emscripten build), and hand the main loop to the browser via
 * emscripten_set_main_loop.
 *
 * Phase 1 scope: smoke-test the toolchain. Skip integrations; draw with
 * mock data. Phase 2 wires client_link via the JS bridge for real state.
 */

#include <SDL2/SDL.h>
#include <emscripten.h>
#include <emscripten/html5.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "display.h"
#include "screens.h"
#include "settings.h"
#include "layout.h"
#include "tile_slots.h"
#include "boxtalk.h"
#include "homewizard.h"
#include "meteradapter.h"
#include "weather.h"
#include "homeassistant.h"
#include "client_link.h"

extern void ui_init(void);
extern void ui_idle_tick(void);

/* ---- inline SDL display + input adapter -----------------------------------
 * One SDL_Window + Renderer + Texture; flush callback updates a sub-rect of
 * the texture and re-presents. Input: poll SDL_Event in the main loop and
 * publish mouse/touch state for LVGL's pointer driver to pick up. */
#define LV_HOR_RES 1024
#define LV_VER_RES 600

static SDL_Window   * g_win;
static SDL_Renderer * g_ren;
static SDL_Texture  * g_tex;

static struct { int x, y, pressed; } g_pt = { 0, 0, 0 };

static void wasm_flush(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * px) {
    SDL_Rect r = { area->x1, area->y1,
                   area->x2 - area->x1 + 1, area->y2 - area->y1 + 1 };
    /* lv_color_t == 4B (LV_COLOR_DEPTH=32) → pitch = w * 4 */
    SDL_UpdateTexture(g_tex, &r, px, r.w * sizeof(lv_color_t));
    SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
    lv_disp_flush_ready(drv);
}

static void wasm_pointer_read(lv_indev_drv_t * drv, lv_indev_data_t * data) {
    (void)drv;
    data->point.x = g_pt.x;
    data->point.y = g_pt.y;
    data->state   = g_pt.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void pump_sdl_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_MOUSEBUTTONDOWN: g_pt.pressed = 1; g_pt.x = e.button.x; g_pt.y = e.button.y; break;
        case SDL_MOUSEBUTTONUP:   g_pt.pressed = 0; break;
        case SDL_MOUSEMOTION:     g_pt.x = e.motion.x; g_pt.y = e.motion.y; break;
        case SDL_FINGERDOWN:      g_pt.pressed = 1;
                                  g_pt.x = (int)(e.tfinger.x * LV_HOR_RES);
                                  g_pt.y = (int)(e.tfinger.y * LV_VER_RES); break;
        case SDL_FINGERUP:        g_pt.pressed = 0; break;
        case SDL_FINGERMOTION:    g_pt.x = (int)(e.tfinger.x * LV_HOR_RES);
                                  g_pt.y = (int)(e.tfinger.y * LV_VER_RES); break;
        default: break;
        }
    }
}

/* ---- placeholder data so the UI renders without integrations -------------- */
static void seed_mock(void) {
    toon_state.indoor_temp   = 20.0f;
    toon_state.setpoint      = 19.0f;
    toon_state.program_state = 1;
    weather_state.connected  = 0;
    meter_state.connected    = 1;
    meter_state.power_w      = 0;
    hw_state.connected_p1    = 0;
}

/* ---- JS ↔ WASM slave bridge ----------------------------------------------
 * shell.html opens EventSource('/api/state/stream') and on each frame calls
 * Module.ccall('wasm_push_state', ...). We just hand the JSON to
 * client_link_apply_state(), the exact function the Toon's reader thread uses
 * — so the same toon_state/ha_state mapping covers both. */
EMSCRIPTEN_KEEPALIVE
void wasm_push_state(const char * json) {
    if (!json || *json != '{') return;
    client_link_apply_state(json);
}

/* Optional reverse direction: LVGL on-tap → wasm_push_event → window.ftPost
 * (defined in shell.html) → fetch POST to the master. Not wired into any
 * LVGL callback yet (that's Phase 2b). */
EMSCRIPTEN_KEEPALIVE
void wasm_push_event(const char * topic, const char * payload) {
    if (!topic || !payload) return;
    EM_ASM({
        if (window.ftPost) {
            window.ftPost(UTF8ToString($0), JSON.parse(UTF8ToString($1) || '{}'));
        }
    }, topic, payload);
}

static uint32_t last_idle_tick = 0;
static void main_loop_iter(void) {
    pump_sdl_events();
    lv_timer_handler();
    lv_tick_inc(16);
    uint32_t now = lv_tick_get();
    if (now - last_idle_tick > 200) {
        ui_idle_tick();
        last_idle_tick = now;
    }
}

int main(int argc, char * argv[]) {
    (void)argc; (void)argv;
    fprintf(stderr, "[wasm] freetoon starting\n");

    /* Phase 2 will mount IDBFS here for persistent settings; for now the
     * in-memory FS is fine. */
    setenv("TOONUI_DATA_DIR", "/", 1);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[wasm] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    g_win = SDL_CreateWindow("freetoon", SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED,
                             LV_HOR_RES, LV_VER_RES, SDL_WINDOW_SHOWN);
    g_ren = SDL_CreateRenderer(g_win, -1, 0);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING,
                              LV_HOR_RES, LV_VER_RES);
    if (!g_win || !g_ren || !g_tex) {
        fprintf(stderr, "[wasm] SDL window/renderer/texture failed: %s\n", SDL_GetError());
        return 1;
    }

    lv_init();

    static lv_color_t buf1[LV_HOR_RES * 60];
    static lv_color_t buf2[LV_HOR_RES * 60];
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LV_HOR_RES * 60);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = wasm_flush;
    disp_drv.hor_res  = LV_HOR_RES;
    disp_drv.ver_res  = LV_VER_RES;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = wasm_pointer_read;
    lv_indev_drv_register(&indev_drv);

    settings_load();
    /* Force the integration "gates" on for the slave — these are normally OFF
     * by default in fresh settings, which would make the home tiles render as
     * "HA offline" / "Initializing…" even though state IS arriving via the SSE
     * bridge. Treat the gates as enabled because the master signals "live"
     * through the SSE itself (ha_connected, p1_connected, etc). */
    settings.energy_source  = 1;   /* read hw_state.power_w (HomeWizard from master) */
    settings.enable_p1_elec = 1;
    settings.enable_ha      = 1;
    settings.enable_vent    = 1;   /* Itho — vent_state.connected comes via SSE */
    layout_load_named(settings.active_layout);
    tile_slots_init();
    seed_mock();
#if 0
    /* Minimal LVGL hello — isolates "SDL/LVGL plumbing works" from "any
     * specific freetoon screen crashes". Flip the #if to 0 once verified to
     * bring the real ui_init() back. */
    {
        lv_obj_t * scr = lv_scr_act();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a2540), 0);
        lv_obj_t * lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "freetoon WASM — hello");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
        lv_obj_center(lbl);
    }
#else
    ui_init();
#endif

    fprintf(stderr, "[wasm] UI up — handing main loop to browser\n");
    emscripten_set_main_loop(main_loop_iter, 0, 1);
    return 0;
}
