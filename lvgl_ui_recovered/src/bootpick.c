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
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "lvgl/lvgl.h"
#include "fbdev.h"
#include "lv_drivers/indev/evdev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Drive LVGL + the draw buffer from the REAL panel resolution. Hardcoding
 * 1024x600 told LVGL the Toon 1 panel (800x480) was wider/taller than it is, so
 * fbdev_flush wrote every row at the wrong stride → the garbled "text up top,
 * noise below" boot splash the tester saw. DISP_HOR/VER are 800x480 under TOON1,
 * 1024x600 otherwise (display.h). */
#define BP_HOR DISP_HOR
#define BP_VER DISP_VER
#define BP_DRAW_LINES 100
#define BP_COUNTDOWN_S 10

/* Launcher-visible return codes (ui_launcher.sh dispatches on these). The
 * stock-LVGL-theme pick is still freetoon as far as the launcher is concerned —
 * it boots the same toonui binary, only with settings.home_theme=1 — so it maps
 * back to CHOICE_FREETOON on exit. */
#define CHOICE_FREETOON 0
#define CHOICE_QTGUI    99

/* Picker-internal choices (NOT launcher rc): the stock-theme variant exists only
 * inside the picker so we can write home_theme before returning CHOICE_FREETOON. */
#define PICK_FREETOON       0   /* freetoon dark dashboard  (home_theme=0) */
#define PICK_FREETOON_STOCK 1   /* freetoon stock LVGL home (home_theme=1) */
#define PICK_QTGUI          99  /* original Eneco qt-gui */

#define CHOICE_FILE "/mnt/data/ui_choice"

/* Above this system uptime (seconds) we treat a bootpick invocation as a WARM
 * restart of toonui (update install, settings restart, crash respawn) rather
 * than a cold power-on. Warm = no chooser, just a "Restarting…" splash.
 * Toon 1 boots considerably slower (~140-180s to reach inittab) due to slow
 * NAND flash and single-core ARM9 CPU, so allow 300s there. */
#ifdef TOON1
#define BP_WARM_RESTART_S 300.0
#else
#define BP_WARM_RESTART_S 120.0
#endif

static double system_uptime(void) {
    FILE * f = fopen("/proc/uptime", "r");
    if (!f) return 1e9;                 /* unknown → treat as warm (no picker) */
    double up = 1e9;
    if (fscanf(f, "%lf", &up) != 1) up = 1e9;
    fclose(f);
    return up;
}

static int  bp_default_choice    = PICK_FREETOON;
static int  bp_final_choice      = PICK_FREETOON;   /* a PICK_* value */
static int  bp_user_picked       = 0;       /* 1 once a button is tapped */
static int  bp_remaining_seconds = BP_COUNTDOWN_S;
static lv_obj_t * bp_lbl_countdown      = NULL;
static lv_obj_t * bp_btn_freetoon       = NULL;
static lv_obj_t * bp_btn_freetoon_stock = NULL;
static lv_obj_t * bp_btn_qtgui          = NULL;
static int  bp_done = 0;                    /* set to break out of loop */

/* Map a picker-internal PICK_* to the launcher return code. Both freetoon
 * variants boot the same toonui binary, so they share CHOICE_FREETOON; the
 * dark-vs-stock distinction is carried by settings.home_theme, not the rc. */
static int pick_to_rc(int pick) {
    return (pick == PICK_QTGUI) ? CHOICE_QTGUI : CHOICE_FREETOON;
}

/* Returns a PICK_* value. settings_load() must already have run so we can read
 * settings.home_theme to tell the two freetoon variants apart. */
static int read_choice_file(void) {
    FILE * f = fopen(CHOICE_FILE, "r");
    char buf[32] = {0};
    if (f) { fgets(buf, sizeof(buf), f); fclose(f); }
    /* strip trailing newline / whitespace */
    for (char * p = buf; *p; p++) {
        if (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') { *p = 0; break; }
    }
    if (strcmp(buf, "qt-gui") == 0 || strcmp(buf, "qtgui") == 0 || strcmp(buf, "stock") == 0)
        return PICK_QTGUI;
    /* freetoon (or no file): remember whether the stock LVGL home was last in
     * use, so the default-highlight + timeout boot match the user's last state. */
    return (settings.home_theme == 1) ? PICK_FREETOON_STOCK : PICK_FREETOON;
}

static void write_choice_file(int choice) {
    FILE * f = fopen(CHOICE_FILE, "w");
    if (!f) return;
    fprintf(f, "%s\n", (choice == CHOICE_QTGUI) ? "qt-gui" : "freetoon");
    fclose(f);
}

/* Persist the home_theme implied by an explicit freetoon-variant pick. Only
 * called on a real user tap — a timeout leaves the user's saved theme alone. */
static void apply_pick_home_theme(int pick) {
    if (pick == PICK_FREETOON_STOCK && settings.home_theme != 1) {
        settings.home_theme = 1; settings_save();
    } else if (pick == PICK_FREETOON && settings.home_theme != 0) {
        settings.home_theme = 0; settings_save();
    }
}

static void hl(lv_obj_t * b, int on) {
    if (!b) return;
    lv_obj_set_style_border_width(b, on ? 4 : 0, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x88ccff), 0);
}

static void apply_button_highlight(void) {
    /* The "default" button gets a brighter border so the user can see at
     * a glance what's about to happen when the countdown hits zero. */
    hl(bp_btn_freetoon,       bp_default_choice == PICK_FREETOON);
    hl(bp_btn_freetoon_stock, bp_default_choice == PICK_FREETOON_STOCK);
    hl(bp_btn_qtgui,          bp_default_choice == PICK_QTGUI);
}

static void on_pick_freetoon(lv_event_t * e) {
    (void)e;
    bp_final_choice = PICK_FREETOON;
    bp_user_picked  = 1;
    bp_done         = 1;
}

static void on_pick_freetoon_stock(lv_event_t * e) {
    (void)e;
    bp_final_choice = PICK_FREETOON_STOCK;
    bp_user_picked  = 1;
    bp_done         = 1;
}

static void on_pick_qtgui(lv_event_t * e) {
    (void)e;
    bp_final_choice = PICK_QTGUI;
    bp_user_picked  = 1;
    bp_done         = 1;
}

/* Display name of the current default pick, for the countdown line. */
static const char * bp_default_name(void) {
    switch (bp_default_choice) {
        case PICK_QTGUI:          return tr("stock qt-gui", "stock qt-gui");
        case PICK_FREETOON_STOCK: return tr("freetoon (stock thema)", "freetoon (stock theme)");
        default:                  return "freetoon";
    }
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
        lv_label_set_text_fmt(bp_lbl_countdown,
            tr("%s start over %d s - tik om te kiezen",
               "Booting %s in %d s - tap to choose"),
            bp_default_name(), bp_remaining_seconds);
    }
}

/* Build the picker UI (title + countdown + two big buttons) onto `scr`. Shared
 * by bootpick_run() and the headless-sim wrapper so the exact layout can be
 * verified at 800x480 without the device. Coords are SX/SY-scaled so the picker
 * fits the Toon 1 panel, not just the 1024x600 design size. */
static void bootpick_build_ui(lv_obj_t * scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f1a2a), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_label_set_text(title, tr("Kies UI", "Choose UI"));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, SY(50));

    bp_lbl_countdown = lv_label_create(scr);
    lv_obj_set_style_text_font(bp_lbl_countdown, SF(22), 0);
    lv_obj_set_style_text_color(bp_lbl_countdown, lv_color_hex(0x88aabb), 0);
    lv_label_set_text(bp_lbl_countdown, "");
    lv_obj_align(bp_lbl_countdown, LV_ALIGN_TOP_MID, 0, SY(140));
    /* prime the label so the first second of the countdown isn't blank */
    lv_label_set_text_fmt(bp_lbl_countdown,
        tr("%s start over %d s - tik om te kiezen",
           "Booting %s in %d s - tap to choose"),
        bp_default_name(), bp_remaining_seconds);

    /* Three big tap-friendly buttons, side by side and centred:
     *   freetoon (dark)  |  freetoon (stock LVGL home)  |  original qt-gui
     * Narrower than the old two-button layout so all three fit 800x480 too. */
    const lv_coord_t BW = SX(300), BH = SY(240), DX = SX(335), DY = SY(60);

    bp_btn_freetoon = lv_btn_create(scr);
    lv_obj_set_size(bp_btn_freetoon, BW, BH);
    lv_obj_align(bp_btn_freetoon, LV_ALIGN_CENTER, -DX, DY);
    lv_obj_set_style_bg_color(bp_btn_freetoon, lv_color_hex(0x2a4060), 0);
    lv_obj_set_style_radius(bp_btn_freetoon, SX(24), 0);
    lv_obj_add_event_cb(bp_btn_freetoon, on_pick_freetoon, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t * l = lv_label_create(bp_btn_freetoon);
        lv_obj_set_style_text_font(l, SF(24), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_label_set_text(l, "freetoon-lvgl");
        lv_obj_align(l, LV_ALIGN_CENTER, 0, SY(-24));
        lv_obj_t * s = lv_label_create(bp_btn_freetoon);
        lv_obj_set_style_text_font(s, SF(16), 0);
        lv_obj_set_style_text_color(s, lv_color_hex(0x9ec4e6), 0);
        lv_label_set_text(s, tr("Donker dashboard", "Dark dashboard"));
        lv_obj_align(s, LV_ALIGN_CENTER, 0, SY(22));
    }

    bp_btn_freetoon_stock = lv_btn_create(scr);
    lv_obj_set_size(bp_btn_freetoon_stock, BW, BH);
    lv_obj_align(bp_btn_freetoon_stock, LV_ALIGN_CENTER, 0, DY);
    lv_obj_set_style_bg_color(bp_btn_freetoon_stock, lv_color_hex(0x35506e), 0);
    lv_obj_set_style_radius(bp_btn_freetoon_stock, SX(24), 0);
    lv_obj_add_event_cb(bp_btn_freetoon_stock, on_pick_freetoon_stock, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t * l = lv_label_create(bp_btn_freetoon_stock);
        lv_obj_set_style_text_font(l, SF(24), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_label_set_text(l, tr("Stock thema", "Stock theme"));
        lv_obj_align(l, LV_ALIGN_CENTER, 0, SY(-24));
        lv_obj_t * s = lv_label_create(bp_btn_freetoon_stock);
        lv_obj_set_style_text_font(s, SF(16), 0);
        lv_obj_set_style_text_color(s, lv_color_hex(0xb8d4ee), 0);
        lv_label_set_text(s, tr("freetoon, qt-gui look", "freetoon, qt-gui look"));
        lv_obj_align(s, LV_ALIGN_CENTER, 0, SY(22));
    }

    bp_btn_qtgui = lv_btn_create(scr);
    lv_obj_set_size(bp_btn_qtgui, BW, BH);
    lv_obj_align(bp_btn_qtgui, LV_ALIGN_CENTER, DX, DY);
    lv_obj_set_style_bg_color(bp_btn_qtgui, lv_color_hex(0x483a2a), 0);
    lv_obj_set_style_radius(bp_btn_qtgui, SX(24), 0);
    lv_obj_add_event_cb(bp_btn_qtgui, on_pick_qtgui, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t * l = lv_label_create(bp_btn_qtgui);
        lv_obj_set_style_text_font(l, SF(24), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_label_set_text(l, tr("Stock qt-gui", "Stock qt-gui"));
        lv_obj_align(l, LV_ALIGN_CENTER, 0, SY(-24));
        lv_obj_t * s = lv_label_create(bp_btn_qtgui);
        lv_obj_set_style_text_font(s, SF(16), 0);
        lv_obj_set_style_text_color(s, lv_color_hex(0xddc296), 0);
        lv_label_set_text(s, tr("Originele Eneco UI", "Original Eneco UI"));
        lv_obj_align(s, LV_ALIGN_CENTER, 0, SY(22));
    }
    apply_button_highlight();
}

/* Headless-sim wrapper: render the picker layout at the build's panel size
 * (sim1 = 800x480) so the Toon 1 layout can be checked without the device. */
lv_obj_t * screen_bootpick_create(void) {
    bp_default_choice    = PICK_FREETOON;
    bp_remaining_seconds = BP_COUNTDOWN_S;
    lv_obj_t * scr = lv_obj_create(NULL);
    bootpick_build_ui(scr);
    return scr;
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
        fprintf(stderr, "[bootpick] disabled — returning rc=%d\n", pick_to_rc(bp_default_choice));
        return pick_to_rc(bp_default_choice);
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

    /* WARM restart (box already up a while or restart marker set): toonui is
     * respawning, not a cold boot — skip the chooser, show a brief "Restarting…"
     * splash, then dispatch straight to the persisted UI. The picker only makes
     * sense at power-on. */
    int is_warm = (getenv("TOONUI_WARM_RESTART") != NULL) ||
                  (access("/var/volatile/tmp/toonui_restart", F_OK) == 0) ||
                  (system_uptime() > BP_WARM_RESTART_S);
    if (is_warm) {
        fprintf(stderr, "[bootpick] warm restart — splash only, rc=%d\n",
                bp_default_choice);
        lv_obj_t * scr = lv_scr_act();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f1a2a), 0);
        lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t * msg = lv_label_create(scr);
        lv_obj_set_style_text_font(msg, SF(28), 0);
        lv_obj_set_style_text_color(msg, lv_color_hex(0xffffff), 0);
        lv_label_set_text(msg, tr(LV_SYMBOL_REFRESH "  UI herstart", LV_SYMBOL_REFRESH "  UI restarting"));
        lv_obj_center(msg);
        for (int i = 0; i < 300; i++) {     /* ~1.5 s so it actually shows */
            lv_timer_handler();
            usleep(5000);
            lv_tick_inc(5);
        }
        fbdev_exit();
        return pick_to_rc(bp_default_choice);
    }

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
#ifdef TOON1
    /* Same Toon-1-specific touch path the main UI uses (TSC2007 at
     * /dev/input/event0, scaled). LVGL's stock evdev_init/read look at
     * event1 + pass raw ADC values, so the picker was untappable too. */
    extern int  toon1_touch_init(void);
    extern void toon1_touch_read(lv_indev_drv_t *, lv_indev_data_t *);
    toon1_touch_init();
    indev_drv.read_cb = toon1_touch_read;
#else
    evdev_init();
    indev_drv.read_cb = evdev_read;
#endif
    lv_indev_drv_register(&indev_drv);

    lv_obj_t * scr = lv_scr_act();
    bootpick_build_ui(scr);

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
        bp_final_choice == PICK_QTGUI          ? "qt-gui" :
        bp_final_choice == PICK_FREETOON_STOCK ? "freetoon-stock" : "freetoon",
        bp_user_picked ? "user-tap" : "timeout");

    /* On an explicit tap, persist the home_theme implied by the freetoon
     * variant the user chose (a timeout leaves their saved theme untouched). */
    if (bp_user_picked) apply_pick_home_theme(bp_final_choice);

    /* Persist the launcher-level choice so the launcher's fallback path agrees
     * with what the user just selected (even on timeout — that confirms the
     * default). */
    int rc = pick_to_rc(bp_final_choice);
    write_choice_file(rc);
    fbdev_exit();
    return rc;
}
