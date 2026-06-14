/*
 * Screen stack for navigation + idle controller for auto-dim.
 *
 * Screens cache themselves: each screen_*_create returns the same lv_obj
 * on repeat calls, so push/pop is cheap and state survives navigation.
 *
 * Idle behavior: ui_mark_activity() records lv_tick_get(); ui_idle_tick()
 * is polled from the main loop and, after settings.auto_dim_seconds of no
 * activity, pushes the dim screen and lowers the backlight.
 */
#include "screens.h"
#include "settings.h"
#include "backlight.h"
#include "boxtalk.h"
#include <stdio.h>
#include <unistd.h>

#define STACK_MAX 8
static lv_obj_t * stack[STACK_MAX];
static int sp = 0;

/* Request a clean UI restart. Drops a marker so ui_launcher.sh's crash-loop
 * guard doesn't mistake this intentional _exit(0) for a crash (3 fast exits
 * in 120 s otherwise force the qt-gui fallback — which bit the layout editor's
 * Save/preset/restart round-trips). Then flush + exit; init respawns the
 * launcher, which re-launches toonui with the freshly saved cfg/layout. */
#define UI_RESTART_MARKER "/var/volatile/tmp/toonui_restart"
void ui_request_restart(void) {
    FILE * f = fopen(UI_RESTART_MARKER, "w");
    if (f) fclose(f);
    fflush(NULL);
    _exit(0);
}

static uint32_t last_activity_ms = 0;
static int      is_dimmed = 0;

void ui_push(lv_obj_t * scr) {
    if (sp >= STACK_MAX || !scr) return;
    stack[sp++] = scr;
    lv_scr_load(scr);
}

void ui_pop(void) {
    if (sp <= 1) return;
    sp--;
    lv_scr_load(stack[sp - 1]);
}

static volatile int wake_pending = 0;

void ui_mark_activity(void) {
    last_activity_ms = lv_tick_get();
    /* Any touch while dimmed queues a wake. The actual pop+lv_scr_load
       runs from ui_idle_tick (main-loop context) so we never mutate the
       screen stack from inside an LVGL event dispatch. */
    if (is_dimmed) wake_pending = 1;
}

void ui_wake_now(void) {
    wake_pending = 1;
}

/* Set the active backlight — either the fixed value, or following the ambient
   light sensor (auto-brightness, Toon 2). Falls back to fixed if no sensor. */
static void apply_active_brightness(void) {
    if (settings.auto_brightness) {
        int lvl = backlight_auto_level(settings.dim_brightness, settings.active_brightness);
        if (lvl >= 0) { backlight_set(lvl); return; }
    }
    backlight_set(settings.active_brightness);
}

void ui_idle_tick(void) {
    /* Process any queued wake first so a touch immediately undims, even
       if the on-screen click handler couldn't run for some reason. */
    if (wake_pending && is_dimmed) {
        fprintf(stderr, "[ui] waking from dim\n");
        /* Wake event — single-shot BoxTalk query so the home screen shows
         * the latest indoor temperature instead of whatever the periodic
         * 15s background query last cached. Not polling — this is one
         * query per wake. */
        boxtalk_request_indoor_refresh();
        ui_pop();
        is_dimmed = 0;
        apply_active_brightness();
        last_activity_ms = lv_tick_get();
        wake_pending = 0;
        /* Tell the indev to ignore any lingering touch state so the wake
           tap does not bleed through onto a tile that's now visible. */
        lv_indev_t * id = lv_indev_get_next(NULL);
        while (id) {
            lv_indev_wait_release(id);
            id = lv_indev_get_next(id);
        }
        return;
    }
    wake_pending = 0;

    /* Auto-brightness: while active, retrack the ambient sensor every ~3 s. */
    if (!is_dimmed && settings.auto_brightness) {
        static uint32_t last_als_ms = 0;
        uint32_t nt = lv_tick_get();
        if (nt - last_als_ms > 3000) { apply_active_brightness(); last_als_ms = nt; }
    }

    uint32_t now = lv_tick_get();
    uint32_t elapsed_ms = now - last_activity_ms;

    /* Auto-return-to-home: after settings.auto_home_seconds of no touch, drop
     * any sub-screen the user navigated into so they come back to the home
     * screen. Runs independently of auto-dim and works whether or not we're
     * dimmed: when dimmed, the dim screen sits on top, so we collapse the
     * stack underneath it to just home (waking then lands on home rather than
     * the abandoned sub-screen). Doesn't reset last_activity, so auto-dim keeps
     * counting from the original idle moment. */
    if (settings.auto_home_enabled) {
        uint32_t home_threshold = (uint32_t)settings.auto_home_seconds * 1000u;
        if (elapsed_ms >= home_threshold) {
            int on_subscreen = is_dimmed ? (sp > 2) : (sp > 1);
            if (on_subscreen) {
                fprintf(stderr, "[ui] auto-home after %u ms idle\n", elapsed_ms);
                if (is_dimmed) {
                    /* Keep the dim screen on top; make home the only thing under it. */
                    stack[1] = stack[sp - 1];
                    sp = 2;
                } else {
                    sp = 1;
                    lv_scr_load(stack[0]);
                }
            }
            /* Also return the home screen's swipe page to the main page, so
             * "auto to main screen" lands on page 1 even if the user wandered
             * to page 2. No-op when already on page 1. */
            screen_home_reset_to_main();
        }
    }

    if (!settings.auto_dim_enabled) return;
    uint32_t threshold = (uint32_t)settings.auto_dim_seconds * 1000u;
    if (!is_dimmed && elapsed_ms >= threshold) {
        fprintf(stderr, "[ui] dimming after %u ms idle\n", elapsed_ms);
        backlight_set(settings.dim_brightness);
        ui_push(screen_dim_create());
        is_dimmed = 1;
    }
}

void ui_init(void) {
    lv_obj_t * home = screen_home_create();
    ui_push(home);
    ui_mark_activity();
    apply_active_brightness();
}
