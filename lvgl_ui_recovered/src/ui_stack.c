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
#include <stdlib.h>
#include <unistd.h>
#ifdef SIM
#include "layout.h"
#endif

#define STACK_MAX 8
static lv_obj_t * stack[STACK_MAX];
static int sp = 0;

/* Request a clean UI restart. Drops a marker so ui_launcher.sh's crash-loop
 * guard doesn't mistake this intentional _exit(0) for a crash (3 fast exits
 * in 120 s otherwise force the qt-gui fallback — which bit the layout editor's
 * Save/preset/restart round-trips). Then flush + exit; init respawns the
 * launcher, which re-launches toonui with the freshly saved cfg/layout. */
#define UI_RESTART_MARKER "/var/volatile/tmp/toonui_restart"
#ifdef SIM
void ui_request_restart(void) {
    fprintf(stderr, "[sim] UI restart requested — exiting with code 42 to respawn\n");
    fflush(stderr);
    _exit(42);
}
void ui_request_reboot(void) {
    fprintf(stderr, "[sim] device reboot requested — exiting\n");
    fflush(stderr);
    _exit(0);
}
#else
void ui_request_restart(void) {
    FILE * f = fopen(UI_RESTART_MARKER, "w");
    if (f) fclose(f);
    fflush(NULL);
    _exit(0);
}
void ui_request_reboot(void) {
    fprintf(stderr, "[system] device reboot requested — syncing and rebooting\n");
    fflush(NULL);
    sync();
    system("/sbin/reboot 2>/dev/null || reboot");
    _exit(0);
}
#endif

/* Switch the home screen between the freetoon dark dashboard and the stock
 * light theme live, no restart. Both screens cache themselves (singleton
 * create), so this just builds/fetches the wanted one, swaps it in as the
 * stack base, and — if home is the visible screen — loads it immediately.
 * The inactive home keeps its (cheap, in-memory) refresh timer; harmless. */
void ui_apply_home_theme(void) {
    lv_obj_t * target = settings.home_theme == 1 ? screen_home_stock_create()
                                                 : screen_home_create();
    if (sp == 0 || stack[0] == target) return;
    stack[0] = target;
    if (sp == 1) lv_scr_load(target);   /* only swap on-screen if home is showing */
}

/* Rebuild the freetoon home screen live when tile light-mode is toggled */
void ui_refresh_home(void) {
    if (settings.home_theme == 0) {
        screen_home_destroy();
        lv_obj_t * target = screen_home_create();
        if (sp > 0) {
            stack[0] = target;
            if (sp == 1) lv_scr_load(target);
        }
    }
}

void ui_refresh_theme(void) {
    ui_refresh_home();
}

static uint32_t last_activity_ms = 0;
static int      is_dimmed = 0;
static int      is_black_screen = 0;

static lv_obj_t * scr_black = NULL;

static void on_black_wake_tap(lv_event_t * e) {
    (void)e;
    ui_wake_now();
}

lv_obj_t * screen_black_create(void) {
    if (scr_black) return scr_black;
    scr_black = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_black, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr_black, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_black, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr_black, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_black, on_black_wake_tap, LV_EVENT_CLICKED, NULL);
    return scr_black;
}

static inline int is_sleep_or_away(void) {
    return (toon_state.active_state == 2 || toon_state.active_state == 3);
}

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

int ui_is_dimmed(void) {
    return is_dimmed;
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
        is_black_screen = 0;
        lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
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

    /* Track thermostat comfort preset changes (active_state) for auto-black / auto-wake */
    static int last_active_state = -999;
    if (last_active_state == -999) {
        last_active_state = toon_state.active_state;
    }
    if (toon_state.active_state != last_active_state) {
        int prev_as = last_active_state;
        last_active_state = toon_state.active_state;

        if (is_dimmed) {
            if (is_black_screen) {
                /* Screen is black because state was Slapen (2) or Weg (3).
                 * If state changed to something else (e.g. Thuis (1), Comfort (0), Manual (-1)),
                 * wake up automatically. */
                if (!is_sleep_or_away()) {
                    fprintf(stderr, "[ui] active_state changed from %d to %d -> waking from black screen\n",
                            prev_as, toon_state.active_state);
                    ui_wake_now();
                    return;
                }
            } else {
                /* Screen is dimmed on normal dim screen.
                 * If state changed to Slapen (2) or Weg (3), switch to black screen. */
                if (settings.dim_sleep_away_black && is_sleep_or_away()) {
                    fprintf(stderr, "[ui] active_state changed from %d to %d while dimmed -> switching to black screen\n",
                            prev_as, toon_state.active_state);
                    backlight_set(0);
                    if (sp > 1) {
                        stack[sp - 1] = screen_black_create();
                        lv_scr_load(stack[sp - 1]);
                    } else {
                        ui_push(screen_black_create());
                    }
                    is_black_screen = 1;
                }
            }
        }
    }

    /* Live settings change: if user disabled dim_sleep_away_black while dimmed black, switch back to normal dim */
    if (is_dimmed && is_black_screen && !settings.dim_sleep_away_black) {
        backlight_set(settings.dim_brightness);
        if (sp > 1) {
            stack[sp - 1] = screen_dim_create();
            lv_scr_load(stack[sp - 1]);
        }
        is_black_screen = 0;
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
            /* Also dismiss any top-layer modals (calendar, waste, news) and
             * return home's swipe page to main page (CR2). */
            screen_calendar_close();
            screen_waste_close();
            screen_home_dismiss_modals();
            screen_settings_close_modals();
            screen_home_reset_to_main();
        }
    }

    if (!settings.auto_dim_enabled) return;
    uint32_t threshold = (uint32_t)settings.auto_dim_seconds * 1000u;
    if (!is_dimmed && elapsed_ms >= threshold) {
        fprintf(stderr, "[ui] dimming after %u ms idle\n", elapsed_ms);
        screen_calendar_close();
        screen_waste_close();
        screen_home_dismiss_modals();
        if (settings.dim_sleep_away_black && is_sleep_or_away()) {
            backlight_set(0);
            ui_push(screen_black_create());
            is_black_screen = 1;
        } else {
            backlight_set(settings.dim_brightness);
            ui_push(screen_dim_create());
            is_black_screen = 0;
        }
        is_dimmed = 1;
        lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_init(void) {
    lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
    lv_obj_t * home = settings.home_theme == 1 ? screen_home_stock_create()
                                               : screen_home_create();
    ui_push(home);
    ui_mark_activity();
    apply_active_brightness();
}

/* ---- clock-colon pulse -------------------------------------------------------
 * Shared by the dim + home clocks: a gentle ~1.1s opacity pulse on a separate
 * ":" overlay label so the divider blinks like a real clock. The colon is its
 * own label (the HH/MM digits render with a space between them) so the digits
 * never jitter as the colon fades. */
static void clock_colon_anim_cb(void * obj, int32_t v) {
    lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}
void clock_colon_pulse(lv_obj_t * colon) {
    if (!colon) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, colon);
    lv_anim_set_exec_cb(&a, clock_colon_anim_cb);
    /* Pulse between full and ~45% so the tick is clearly visible but the colon
     * never vanishes (255->40 read as off/invisible, esp. on the small home
     * clock). ~1 s period, like a real clock. */
    lv_anim_set_values(&a, 255, 115);
    lv_anim_set_time(&a, 500);
    lv_anim_set_playback_time(&a, 500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}
