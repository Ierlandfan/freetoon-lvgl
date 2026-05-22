/*
 * Boot picker — 10-second countdown screen the user sees right after
 * power-on. Lets them choose between freetoon-lvgl (default) and the
 * stock Eneco qt-gui without needing to drop to a shell.
 *
 * Drawn in LVGL so we can reuse the existing fbdev + evdev setup; the
 * picker process exits as soon as a button is tapped or the timer fires,
 * and ui_launcher.sh dispatches to the chosen binary based on rc.
 */
#include "bootpick.h"
#include "settings.h"
#include "lvgl/lvgl.h"
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BP_HOR 1024
#define BP_VER 600
#define BP_DRAW_LINES 100
#define BP_COUNTDOWN_S 10

#define CHOICE_FREETOON 0
#define CHOICE_QTGUI    99

#define CHOICE_FILE "/mnt/data/ui_choice"

/* Above this system uptime (seconds) we treat a bootpick invocation as a WARM
 * restart of toonui (update install, settings restart, crash respawn) rather
 * than a cold power-on. Warm = no chooser, just a "Restarting…" splash. */
#define BP_WARM_RESTART_S 120.0

static double system_uptime(void) {
    FILE * f = fopen("/proc/uptime", "r");
    if (!f) return 1e9;                 /* unknown → treat as warm (no picker) */
    double up = 1e9;
    if (fscanf(f, "%lf", &up) != 1) up = 1e9;
    fclose(f);
    return up;
}

static int  bp_default_choice    = CHOICE_FREETOON;
static int  bp_final_choice      = CHOICE_FREETOON;
static int  bp_user_picked       = 0;       /* 1 once a button is tapped */
static int  bp_remaining_seconds = BP_COUNTDOWN_S;
static lv_obj_t * bp_lbl_countdown = NULL;
static lv_obj_t * bp_btn_freetoon  = NULL;
static lv_obj_t * bp_btn_qtgui     = NULL;
static int  bp_done = 0;                    /* set to break out of loop */

static int read_choice_file(void) {
    FILE * f = fopen(CHOICE_FILE, "r");
    if (!f) return CHOICE_FREETOON;
    char buf[32] = {0};
    fgets(buf, sizeof(buf), f);
    fclose(f);
    /* strip trailing newline / whitespace */
    for (char * p = buf; *p; p++) {
        if (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') { *p = 0; break; }
    }
    if (strcmp(buf, "qt-gui") == 0 || strcmp(buf, "qtgui") == 0 || strcmp(buf, "stock") == 0)
        return CHOICE_QTGUI;
    return CHOICE_FREETOON;
}

static void write_choice_file(int choice) {
    FILE * f = fopen(CHOICE_FILE, "w");
    if (!f) return;
    fprintf(f, "%s\n", (choice == CHOICE_QTGUI) ? "qt-gui" : "freetoon");
    fclose(f);
}

static void apply_button_highlight(void) {
    /* The "default" button gets a brighter border so the user can see at
     * a glance what's about to happen when the countdown hits zero. */
    if (bp_btn_freetoon) {
        lv_obj_set_style_border_width(bp_btn_freetoon,
            bp_default_choice == CHOICE_FREETOON ? 4 : 0, 0);
        lv_obj_set_style_border_color(bp_btn_freetoon, lv_color_hex(0x88ccff), 0);
    }
    if (bp_btn_qtgui) {
        lv_obj_set_style_border_width(bp_btn_qtgui,
            bp_default_choice == CHOICE_QTGUI ? 4 : 0, 0);
        lv_obj_set_style_border_color(bp_btn_qtgui, lv_color_hex(0x88ccff), 0);
    }
}

static void on_pick_freetoon(lv_event_t * e) {
    (void)e;
    bp_final_choice = CHOICE_FREETOON;
    bp_user_picked  = 1;
    bp_done         = 1;
}

static void on_pick_qtgui(lv_event_t * e) {
    (void)e;
    bp_final_choice = CHOICE_QTGUI;
    bp_user_picked  = 1;
    bp_done         = 1;
}

static void tick_countdown(lv_timer_t * t) {
    (void)t;
    bp_remaining_seconds--;
    if (bp_remaining_seconds <= 0) {
        bp_final_choice = bp_default_choice;
        bp_done = 1;
        return;
    }
    if (bp_lbl_countdown) {
        const char * which = (bp_default_choice == CHOICE_FREETOON)
                                 ? "freetoon"
                                 : "stock qt-gui";
        lv_label_set_text_fmt(bp_lbl_countdown,
            "Booting %s in %d s — tap to choose",
            which, bp_remaining_seconds);
    }
}

/* Single-shot LVGL/fb/evdev init + render + tap-loop. Returns the rc to
 * pass back to ui_launcher.sh. */
int bootpick_run(void) {
    settings_load();

    bp_default_choice = read_choice_file();
    bp_final_choice   = bp_default_choice;

    /* User said "no picker, just boot": skip the screen entirely and let
     * the launcher dispatch on the persisted choice. settings.h marks
     * boot_picker_enabled as default-1, so this only short-circuits when
     * a user explicitly toggled it off in Settings → UI mode. */
    if (!settings.boot_picker_enabled) {
        fprintf(stderr, "[bootpick] disabled — returning rc=%d\n", bp_default_choice);
        return bp_default_choice;
    }

    fprintf(stderr, "[bootpick] starting (default=%s, %d s timer)\n",
        bp_default_choice == CHOICE_QTGUI ? "qt-gui" : "freetoon",
        BP_COUNTDOWN_S);

    lv_init();
    fbdev_init();

    static lv_color_t buf1[BP_HOR * BP_DRAW_LINES];
    static lv_color_t buf2[BP_HOR * BP_DRAW_LINES];
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, BP_HOR * BP_DRAW_LINES);
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = fbdev_flush;
    disp_drv.hor_res  = BP_HOR;
    disp_drv.ver_res  = BP_VER;
    lv_disp_drv_register(&disp_drv);

    /* WARM restart (box already up a while): toonui is respawning, not a cold
     * boot — skip the chooser, show a brief "Restarting…" splash, then dispatch
     * straight to the persisted UI. The picker only makes sense at power-on. */
    if (system_uptime() > BP_WARM_RESTART_S) {
        fprintf(stderr, "[bootpick] warm restart (uptime>%.0fs) — splash only, rc=%d\n",
                BP_WARM_RESTART_S, bp_default_choice);
        lv_obj_t * scr = lv_scr_act();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f1a2a), 0);
        lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t * msg = lv_label_create(scr);
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(msg, lv_color_hex(0xffffff), 0);
        lv_label_set_text(msg, LV_SYMBOL_REFRESH "  Restarting...");
        lv_obj_center(msg);
        for (int i = 0; i < 300; i++) {     /* ~1.5 s so it actually shows */
            lv_timer_handler();
            usleep(5000);
            lv_tick_inc(5);
        }
        return bp_default_choice;
    }

    evdev_init();
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = evdev_read;
    lv_indev_drv_register(&indev_drv);

    lv_obj_t * scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f1a2a), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_label_set_text(title, "Choose UI");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);

    bp_lbl_countdown = lv_label_create(scr);
    lv_obj_set_style_text_font(bp_lbl_countdown, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(bp_lbl_countdown, lv_color_hex(0x88aabb), 0);
    lv_label_set_text(bp_lbl_countdown, "");
    lv_obj_align(bp_lbl_countdown, LV_ALIGN_TOP_MID, 0, 140);
    /* prime the label so the first second of the countdown isn't blank */
    {
        const char * which = (bp_default_choice == CHOICE_FREETOON)
                                 ? "freetoon"
                                 : "stock qt-gui";
        lv_label_set_text_fmt(bp_lbl_countdown,
            "Booting %s in %d s — tap to choose",
            which, bp_remaining_seconds);
    }

    /* Two big tap-friendly buttons, side by side and centred. */
    bp_btn_freetoon = lv_btn_create(scr);
    lv_obj_set_size(bp_btn_freetoon, 380, 240);
    lv_obj_align(bp_btn_freetoon, LV_ALIGN_CENTER, -210, 60);
    lv_obj_set_style_bg_color(bp_btn_freetoon, lv_color_hex(0x2a4060), 0);
    lv_obj_set_style_radius(bp_btn_freetoon, 24, 0);
    lv_obj_add_event_cb(bp_btn_freetoon, on_pick_freetoon, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t * l = lv_label_create(bp_btn_freetoon);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_label_set_text(l, "freetoon-lvgl");
        lv_obj_align(l, LV_ALIGN_CENTER, 0, -24);
        lv_obj_t * s = lv_label_create(bp_btn_freetoon);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(s, lv_color_hex(0x9ec4e6), 0);
        lv_label_set_text(s, "Custom LVGL UI");
        lv_obj_align(s, LV_ALIGN_CENTER, 0, 22);
    }

    bp_btn_qtgui = lv_btn_create(scr);
    lv_obj_set_size(bp_btn_qtgui, 380, 240);
    lv_obj_align(bp_btn_qtgui, LV_ALIGN_CENTER, 210, 60);
    lv_obj_set_style_bg_color(bp_btn_qtgui, lv_color_hex(0x483a2a), 0);
    lv_obj_set_style_radius(bp_btn_qtgui, 24, 0);
    lv_obj_add_event_cb(bp_btn_qtgui, on_pick_qtgui, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t * l = lv_label_create(bp_btn_qtgui);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_label_set_text(l, "Stock qt-gui");
        lv_obj_align(l, LV_ALIGN_CENTER, 0, -24);
        lv_obj_t * s = lv_label_create(bp_btn_qtgui);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(s, lv_color_hex(0xddc296), 0);
        lv_label_set_text(s, "Original Eneco UI");
        lv_obj_align(s, LV_ALIGN_CENTER, 0, 22);
    }
    apply_button_highlight();

    /* Force one paint so the screen actually appears before we drop into
     * the 1 Hz timer — without this the framebuffer can stay black for
     * a frame and the user sees nothing until the first tick. */
    lv_timer_handler();

    lv_timer_t * cd = lv_timer_create(tick_countdown, 1000, NULL);
    (void)cd;

    /* Run the LVGL loop until a button fires or the timer expires. */
    while (!bp_done) {
        lv_timer_handler();
        usleep(5000);
        lv_tick_inc(5);
    }

    fprintf(stderr, "[bootpick] picked=%s (%s)\n",
        bp_final_choice == CHOICE_QTGUI ? "qt-gui" : "freetoon",
        bp_user_picked ? "user-tap" : "timeout");

    /* Persist the choice so the launcher's fallback path agrees with
     * what the user just selected (even on timeout — that confirms the
     * default). */
    write_choice_file(bp_final_choice);
    return bp_final_choice;
}
