/*
 * Home screen — tile grid matching qt-gui's layout.
 * Big thermostat tile top-left (240*2+gap = 500 wide, 220*2+gap = 460 tall).
 * Smaller tiles laid out 4 per row to its right and below.
 *
 * Tap a tile to navigate to its detail screen.
 */
#include "screens.h"
#include "boxtalk.h"
#include "icons.h"
#include "homewizard.h"
#include "meteradapter.h"
#include "settings.h"
#include "weather.h"
#include "wastecollection.h"
#include "ventilation.h"
#include "homeassistant.h"
#include "client_link.h"
#include "news.h"
#include "packages.h"
#include "tile_slots.h"
#include "update_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define COL_BG          0x0f1a2a
#define COL_TILE_BG     0x1a2a44
#define COL_TILE_ACCENT 0x335577
#define COL_TEXT_HI     0xffffff
#define COL_TEXT_DIM    0x88aabb
#define COL_TEMP_YELLOW 0xffcc44
#define COL_BURNER_RED  0xff6644

static lv_obj_t * scr_root = NULL;

/* Big thermostat tile widgets — updated by refresh timer */
static lv_obj_t * lbl_t_clock;
static lv_obj_t * lbl_t_temp;
static lv_obj_t * lbl_t_setpoint;
static lv_obj_t * lbl_t_burner;
static lv_obj_t * lbl_t_pressure;
static lv_obj_t * lbl_t_humidity;
static lv_obj_t * lbl_t_ppm;
static lv_obj_t * lbl_t_tvoc;
static lv_obj_t * lbl_t_date;
static lv_obj_t * moon_phase_img;  /* small moon-phase widget next to date */
static lv_obj_t * lbl_t_aq;
static lv_obj_t * lbl_waste_date;
static lv_obj_t * lbl_waste_type;
static lv_obj_t * lbl_waste_date_2;
static lv_obj_t * lbl_waste_type_2;
static lv_obj_t * waste_icon_1;
static lv_obj_t * waste_icon_2;
/* Pressure-warning banner — covers the top of the Heater tile when the
 * boiler's CH water pressure falls into the "low" / "very low" zones.
 * Hidden when pressure is OK or unknown. */
static lv_obj_t * pressure_banner = NULL;
static lv_obj_t * pressure_banner_lbl = NULL;
static lv_obj_t * lbl_t_program;     /* small "Home (temp)" hint under buttons */
/* Mode toggle on the heater tile — two side-by-side buttons. Manual taps
 * to permanent hold; Program taps to resume schedule. Active mode gets a
 * white border so the user can read state at a glance. */
static lv_obj_t * tile_btn_mode_manual  = NULL;
static lv_obj_t * tile_btn_mode_program = NULL;
/* Four small one-tap preset buttons sitting just below the mode toggle —
 * Comfort / Home / Sleep / Away. Each fires boxtalk_set_program(N) so the
 * Toon jumps straight to that preset (and the schedule keeps progressing
 * from there). Active preset gets a white border. */
static lv_obj_t * tile_btn_preset[4] = {0};
static lv_obj_t * tile_img_flame;
static lv_obj_t * tile_img_faucet;
static lv_obj_t * tile_img_drop;
/* Live water-flow indicator — small drop + "X L/min" on the home tile,
 * visible whenever any tap is open (HomeWizard reports flow > 0.05). */
static lv_obj_t * tile_img_water = NULL;
static lv_obj_t * tile_lbl_water = NULL;
/* Bottom-row labels (Energy/Weather/Waste) — updated by refresh_cb */
static lv_obj_t * lbl_bot_energy;
static lv_obj_t * lbl_bot_weather;
static lv_obj_t * lbl_bot_waste;
static lv_obj_t * lbl_inbox_main;
static lv_obj_t * lbl_inbox_sub;
static lv_obj_t * water_spinner;
static lv_obj_t * forecast_box;
static lv_obj_t * lbl_outside_main;
static lv_obj_t * lbl_outside_sub;
static lv_obj_t * fc_day_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_temp_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_wind_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_icon[WEATHER_FORECAST_DAYS];
static lv_obj_t * envelope_btn;
static lv_obj_t * envelope_badge;
static lv_obj_t * envelope_badge_lbl;
static lv_obj_t * water_spinner;
static lv_obj_t * forecast_box;
static lv_obj_t * lbl_outside_main;
static lv_obj_t * lbl_outside_sub;
static lv_obj_t * fc_day_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_temp_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_wind_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_icon[WEATHER_FORECAST_DAYS];
/* Bicolor partly-cloudy overlay — yellow sun layered on top of the
 * white cloud for 'b'/'j' Buienradar codes. Hidden for all other codes;
 * set_forecast_icon() manages visibility + src + recolor. */
static lv_obj_t * fc_icon_sun[WEATHER_FORECAST_DAYS];

/* Buienradar uses a single-letter code for daytime weather and a
 * double-letter code ("aa", "bb", "rr", "nn") for the same condition
 * at night. We detect the double-letter as "night-time" and substitute
 * a moon-flavoured rendering: clear nights become a plain moon icon,
 * partly-cloudy nights become moon-behind-cloud (instead of sun-behind-
 * cloud). Other conditions keep their daytime appearance — at night,
 * rain and snow still LOOK like rain and snow. */
static int is_night_code(const char * letter) {
    if (!letter || !letter[0] || !letter[1]) return 0;
    return letter[0] == letter[1];   /* "aa","bb","rr","nn" → night variant */
}

static void set_forecast_icon(lv_obj_t * cloud, lv_obj_t * sun,
                              const char * letter) {
    if (!cloud) return;
    int is_night   = is_night_code(letter);
    int is_partly  = letter && (letter[0] == 'b' || letter[0] == 'j');
    int is_thunder = letter && (letter[0] == 'g' || letter[0] == 'm');
    int is_clear   = letter && letter[0] == 'a';

    /* Clear sky at night → moon icon, no overlay needed. */
    if (is_night && is_clear) {
        lv_img_set_src(cloud, moon_phase_icon(40));
        lv_obj_set_style_img_recolor(cloud, lv_color_hex(0xe8edf2), 0);   /* moon white */
        if (sun) lv_obj_add_flag(sun, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (is_partly) {
        /* Partly cloudy — white cloud + sun overlay (day) or moon overlay
         * (night). The night variant ("bb") swaps the yellow sun for a
         * pale moon so the slot doesn't render a sun glyph at midnight. */
        lv_img_set_src(cloud, weather_icon_for("d"));            /* plain cloud */
        lv_obj_set_style_img_recolor(cloud, lv_color_hex(0xf0f4f8), 0);
        if (sun) {
            if (is_night) {
                lv_img_set_src(sun, moon_phase_icon(40));
                lv_obj_set_style_img_recolor(sun, lv_color_hex(0xe8edf2), 0);
            } else {
                lv_img_set_src(sun, weather_icon_for("a"));      /* plain sun */
                lv_obj_set_style_img_recolor(sun, lv_color_hex(0xffd24a), 0);
            }
            lv_obj_set_style_img_recolor_opa(sun, 255, 0);
            lv_obj_clear_flag(sun, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (is_thunder) {
        /* Thunder — dark-gray storm cloud + yellow bolt overlay.
         * Single-recolor would have made the whole pictogram amber and
         * lost the "stormy" reading. Use the bolt-only icon (no cloud
         * portion) overlaid on a separately-recolored gray cloud. */
        lv_img_set_src(cloud, weather_icon_for("d"));            /* plain cloud */
        lv_obj_set_style_img_recolor(cloud, lv_color_hex(0x6a7888), 0);   /* stormy gray */
        if (sun) {
            lv_img_set_src(sun, &icon_wx_bolt);
            lv_obj_set_style_img_recolor(sun, lv_color_hex(0xffd24a), 0);
            lv_obj_set_style_img_recolor_opa(sun, 255, 0);
            lv_obj_clear_flag(sun, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_img_set_src(cloud, weather_icon_for(letter));
        lv_obj_set_style_img_recolor(cloud,
            lv_color_hex(weather_icon_color_for(letter)), 0);
        if (sun) lv_obj_add_flag(sun, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t * water_spinner;
static lv_obj_t * forecast_box;
static lv_obj_t * lbl_forecast_city = NULL;
static lv_obj_t * lbl_life360_ronald = NULL;
static lv_obj_t * lbl_life360_caja   = NULL;
static lv_obj_t * lbl_outside_main;
static lv_obj_t * lbl_outside_sub;
static lv_obj_t * fc_day_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_temp_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_desc_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_wind_arrow[WEATHER_FORECAST_DAYS];
static lv_obj_t * lbl_inbox_main;
static lv_obj_t * lbl_inbox_sub;
/* Bottom-row labels (Energy/Weather/Waste) — updated by refresh_cb */
static lv_obj_t * lbl_bot_energy;
static lv_obj_t * lbl_bot_weather;
static lv_obj_t * lbl_bot_waste;
static lv_obj_t * tile_img_flame;
static lv_obj_t * tile_img_faucet;
static lv_obj_t * tile_img_drop;

/* Whole-tile pointers so refresh_cb can flip their offline labels or
 * hide them entirely based on the matching enable_* flag +
 * settings.hide_offline_tiles. NULL until the screen is built. */
static lv_obj_t * tile_water    = NULL;
static lv_obj_t * tile_family   = NULL;
static lv_obj_t * tile_vent     = NULL;
static lv_obj_t * tile_energy   = NULL;
static lv_obj_t * tile_waste    = NULL;
static lv_obj_t * tile_curtains = NULL;

/* Swipeable tile pages. The five built-in tiles (Waste/Energy/Vent/Family/
 * Water) are "page 0". Swiping left in the tile zone reveals page 1 — an
 * overlay with extra configurable slots — and swiping right returns. We keep
 * page 0 absolutely positioned and just toggle visibility (no reparenting). */
static lv_obj_t * home_page1      = NULL;   /* page-1 overlay container */
static lv_obj_t * home_dot[2]     = {0};    /* pagination dots */
static int        home_tile_page  = 0;
/* Page-1 tiles render whatever marketplace integration is bound to slots
 * TILE_SLOT_P1_0..3 (or a "tap to assign" placeholder when empty). */
static lv_obj_t * p1_title[4] = {0};
static lv_obj_t * p1_main[4]  = {0};
static lv_obj_t * p1_sub[4]   = {0};

/* RSS news ticker (above the forecast). Tap → headline list + a QR per
 * article so it can be opened on a phone. */
static lv_obj_t * news_ticker = NULL;
static lv_obj_t * news_modal  = NULL;
static lv_obj_t * news_qr     = NULL;
/* Small banner at the top of the home screen — shown when the update
 * checker finds a newer freetoon-lvgl release. Tapping it opens a modal
 * with the release notes + the one-line install command. */
static lv_obj_t * update_banner     = NULL;
static lv_obj_t * update_banner_lbl = NULL;
static lv_obj_t * update_modal      = NULL;
static lv_obj_t * about_status_lbl  = NULL;   /* live status line inside the modal */
static char       skipped_version[UPDATE_VERSION_MAX] = "";  /* banner suppressed for this ver */
static int        update_minimized = 0;   /* dismissed → shown as an envelope icon */
static int        install_pinned   = 0;   /* freeze the modal status line during install/result */
static lv_obj_t * update_env_btn   = NULL; /* minimized-update envelope (top-right) */

/* Smaller tile widgets */
static lv_obj_t * lbl_humid_val;       /* removed widget — kept as NULL for old refs */
static lv_obj_t * lbl_energy_w;
static lv_obj_t * lbl_energy_gas;
static lv_obj_t * lbl_energy_today;
static lv_obj_t * lbl_boiler_state;
static lv_obj_t * lbl_boiler_pressure;
static lv_obj_t * vent_fan_img = NULL;
static int        vent_anim_period_ms = -1;

static lv_timer_t * refresh_timer = NULL;

/* ---- Energy source selector ----
 * settings.energy_source: 0 = meteradapter (Toon's own meter via happ_pwrusage),
 * 1 = HomeWizard P1. The Energy tile reads through these so the user can switch
 * source in Settings without touching the rest of the layout. */
static int energy_connected(void) {
    return settings.energy_source == 0 ? meter_state.connected
                                       : (settings.enable_p1_elec && hw_state.connected_p1);
}
static float energy_power_w(void) {
    return settings.energy_source == 0 ? meter_state.power_w : hw_state.power_w;
}
/* Cumulative gas (m³) is only available from the HomeWizard P1; happ_pwrusage's
 * HTTP path doesn't expose it. Returns <0 when unavailable. */
static float energy_gas_m3(void) {
    if (settings.energy_source == 1 && hw_state.connected_p1) return hw_state.gas_m3;
    return -1.0f;
}
static const char * energy_offline_label(void) {
    return settings.energy_source == 0 ? "meter offline" : "P1 offline";
}

/* ---------- tile builder helpers ---------- */
typedef struct {
    lv_obj_t * tile;
    lv_obj_t * title;
    lv_obj_t * value;  /* main big-ish value */
    lv_obj_t * sub;    /* sub-line under value */
} tile_t;

/* Generic tile: w x h pixels, given title, optional click handler */
static void make_tile(lv_obj_t * parent, int x, int y, int w, int h,
                      const char * title, uint32_t accent_color,
                      lv_event_cb_t click_cb, tile_t * out) {
    lv_obj_t * t = lv_obj_create(parent);
    lv_obj_set_size(t, w, h);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_style_bg_color(t, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_radius(t, 14, 0);
    lv_obj_set_style_pad_all(t, 10, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    if (click_cb) {
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, click_cb, LV_EVENT_CLICKED, NULL);
    }

    /* Thin accent bar on top */
    lv_obj_t * bar = lv_obj_create(t);
    lv_obj_set_size(bar, w - 20, 4);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(accent_color), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * lbl_title = lv_label_create(t);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_title, title);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 0, 14);

    out->tile = t;
    out->title = lbl_title;
    out->value = NULL;
    out->sub = NULL;
}

/* ---------- navigation handlers ---------- */
static void open_thermostat(lv_event_t * e) {
    (void)e;
    ui_push(screen_thermostat_create());
}
/* +/- on the tile: child clicks don't bubble to parent, so these
   don't navigate. They just adjust setpoint in place. */
static void on_tile_sp_up(lv_event_t * e)   { (void)e; boxtalk_setpoint_increase(); }
static void on_tile_sp_down(lv_event_t * e) { (void)e; boxtalk_setpoint_decrease(); }

/* ---------- program-picker modal ---------- */
typedef struct {
    int        state_value;   /* 0..3 = named scheme, -1 = Manual */
    lv_obj_t * modal;
} picker_entry_t;
static picker_entry_t picker_entries[5];

static void picker_close_cb(lv_event_t * e) {
    lv_obj_t * modal = lv_event_get_user_data(e);
    if (modal) lv_obj_del(modal);
}

static void picker_apply_cb(lv_event_t * e) {
    picker_entry_t * pe = lv_event_get_user_data(e);
    if (!pe) return;
    if (pe->state_value < 0) boxtalk_set_manual();
    else                     boxtalk_set_program(pe->state_value);
    if (pe->modal) lv_obj_del(pe->modal);
}

/* Mode-toggle taps. Each button always sends the corresponding action;
 * re-tapping the active mode is a no-op the user can't get wrong. */
static void on_mode_manual(lv_event_t * e)  { (void)e; boxtalk_set_manual();      }
static void on_mode_program(lv_event_t * e) { (void)e; boxtalk_resume_schedule(); }

/* Direct preset tap on one of the four pill-side buttons. user_data carries
 * the Toon program state (0=Comfort, 1=Home, 2=Sleep, 3=Away). */
static void on_tile_preset(lv_event_t * e) {
    int s = (int)(intptr_t)lv_event_get_user_data(e);
    boxtalk_set_program(s);
}

static void on_program_tap(lv_event_t * e) {
    (void)e;
    lv_obj_t * modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal, 1024, 600);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, 200, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(modal);
    lv_label_set_text(title, "Select program");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);

    const char* names[]  = {"Comfort", "Home", "Sleep", "Away", "Manual"};
    int         values[] = {0, 1, 2, 3, -1};
    uint32_t    colors[] = {0xff8866, 0x66cc88, 0x4466cc, 0xaa66ff, 0xffaa44};
    for (int i = 0; i < 5; i++) {
        picker_entries[i].state_value = values[i];
        picker_entries[i].modal       = modal;
        lv_obj_t * btn = lv_btn_create(modal);
        lv_obj_set_size(btn, 170, 220);
        lv_obj_set_pos(btn, 30 + i * 195, 160);
        lv_obj_set_style_bg_color(btn, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_radius(btn, 16, 0);
        lv_obj_add_event_cb(btn, picker_apply_cb, LV_EVENT_CLICKED, &picker_entries[i]);
        lv_obj_t * lbl = lv_label_create(btn);
        lv_label_set_text(lbl, names[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
        lv_obj_center(lbl);
    }

    lv_obj_t * cancel = lv_btn_create(modal);
    lv_obj_set_size(cancel, 220, 70);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(cancel, 14, 0);
    lv_obj_add_event_cb(cancel, picker_close_cb, LV_EVENT_CLICKED, modal);
    lv_obj_t * cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_22, 0);
    lv_obj_center(cancel_lbl);
}
/* Long-press on any of the four right-column tiles. We don't yet know
 * which tile triggered this — the modal lets the user pick the slot
 * anyway — so the handler just opens the shared picker. Cheaper UI and
 * no fragile tile-→-slot mapping. */
static void on_tile_longpress(lv_event_t * e) {
    (void)e;
    screen_settings_open_tile_slots_modal();
}

static void open_placeholder(lv_event_t * e) {
    (void)e;  /* TODO: per-tile detail screens */
}

/* Update banner click → modal with release notes + install command.
 * Built lazily; one instance reused across taps (recreated if a newer
 * version becomes available while it's already open and the user is
 * looking at stale notes, since refresh_cb sees that and rebuilds). */
static void on_update_modal_close(lv_event_t * e) {
    (void)e;
    about_status_lbl = NULL;
    install_pinned = 0;
    if (update_modal) { lv_obj_del_async(update_modal); update_modal = NULL; }
}

/* True when the running build already matches the latest release tag (base,
 * ignoring any `git describe` suffix) — i.e. there's nothing to install. */
static int update_already_installed(void) {
    const char * t = g_update_state.latest_version;
    if (!t[0]) return 0;
    size_t tn = strcspn(t, "-");
    size_t bn = strcspn(BUILD_VERSION, "-");
    return tn == bn && strncmp(t, BUILD_VERSION, tn) == 0;
}

/* Run the on-device self-installer detached, so it survives toonui being
 * killed mid-update (init respawns the new binary via ui_launcher). */
static void do_install_now(lv_event_t * e) {
    (void)e;
    install_pinned = 1;   /* stop refresh_cb from overwriting the status line */
    if (update_already_installed()) {
        if (about_status_lbl)
            lv_label_set_text_fmt(about_status_lbl,
                "Already on %s - nothing to install.", BUILD_VERSION);
        return;
    }
    if (about_status_lbl)
        lv_label_set_text(about_status_lbl, "Installing... the screen will restart shortly.");
    system("setsid sh -c 'sleep 1; curl -fsSL "
           "https://raw.githubusercontent.com/Ierlandfan/freetoon-lvgl/main/scripts/toon-selfinstall.sh "
           "| sh' >/var/volatile/tmp/selfinstall.log 2>&1 &");
}

/* Skip this version — suppress the banner until a newer release appears. */
static void do_skip_version(lv_event_t * e) {
    snprintf(skipped_version, sizeof skipped_version, "%s", g_update_state.latest_version);
    update_minimized = 0;
    on_update_modal_close(e);
}

/* Dismiss → minimize the update notice to an envelope icon (re-openable). */
static void do_dismiss_to_envelope(lv_event_t * e) {
    update_minimized = 1;
    on_update_modal_close(e);
}

static void * check_thread(void * arg) { (void)arg; update_check_now(); return NULL; }
static void do_check_updates(lv_event_t * e) {
    (void)e;
    if (about_status_lbl) lv_label_set_text(about_status_lbl, "Checking for updates...");
    pthread_t t;
    if (pthread_create(&t, NULL, check_thread, NULL) == 0) pthread_detach(t);
}

/* About / Update modal — opened by the logo (always) and the update banner.
 * Shows version, status, release notes, and Check / Install / Skip actions. */
static void open_about_modal(lv_event_t * e) {
    (void)e;
    if (update_modal) { lv_obj_del_async(update_modal); update_modal = NULL; }

    update_modal = lv_obj_create(scr_root);
    lv_obj_remove_style_all(update_modal);
    lv_obj_set_size(update_modal, 1024, 600);
    lv_obj_set_pos(update_modal, 0, 0);
    lv_obj_set_style_bg_color(update_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(update_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(update_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(update_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(update_modal, on_update_modal_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t * panel = lv_obj_create(update_modal);
    lv_obj_set_size(panel, 760, 470);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x16243a), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_pad_all(panel, 22, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);   /* swallow taps */

    /* Logo mark + title */
    lv_obj_t * mark = lv_obj_create(panel);
    lv_obj_set_size(mark, 46, 46);
    lv_obj_align(mark, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(mark, lv_color_hex(0x2e6e9e), 0);
    lv_obj_set_style_radius(mark, 12, 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * mk = lv_label_create(mark);
    lv_obj_set_style_text_color(mk, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(mk, &lv_font_montserrat_28, 0);
    lv_label_set_text(mk, "ft");
    lv_obj_center(mk);

    lv_obj_t * t = lv_label_create(panel);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), 0);
    lv_label_set_text(t, "freetoon");
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 60, 2);

    lv_obj_t * ver = lv_label_create(panel);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ver, lv_color_hex(0x88aabb), 0);
    lv_label_set_text_fmt(ver, "%s  -  beta", BUILD_VERSION);
    lv_obj_align(ver, LV_ALIGN_TOP_LEFT, 60, 34);

    /* Live status line (updated by refresh_cb while open). */
    about_status_lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(about_status_lbl, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(about_status_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_width(about_status_lbl, 716);
    lv_label_set_long_mode(about_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(about_status_lbl, LV_ALIGN_TOP_LEFT, 0, 70);

    /* Release notes (scrollable area). */
    lv_obj_t * notesbox = lv_obj_create(panel);
    lv_obj_set_size(notesbox, 716, 230);
    lv_obj_align(notesbox, LV_ALIGN_TOP_LEFT, 0, 110);
    lv_obj_set_style_bg_color(notesbox, lv_color_hex(0x0e1a2a), 0);
    lv_obj_set_style_border_width(notesbox, 0, 0);
    lv_obj_set_style_radius(notesbox, 10, 0);
    lv_obj_set_style_pad_all(notesbox, 12, 0);
    lv_obj_set_scroll_dir(notesbox, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(notesbox, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_t * notes = lv_label_create(notesbox);
    lv_obj_set_style_text_font(notes, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(notes, lv_color_hex(0xc8d4e0), 0);
    lv_label_set_long_mode(notes, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(notes, 690);
    lv_label_set_text(notes, g_update_state.release_notes[0]
                      ? g_update_state.release_notes
                      : "Release notes appear here after a check finds a new version.");

    /* Button row. */
    lv_obj_t * b_check = lv_btn_create(panel);
    lv_obj_set_size(b_check, 180, 56);
    lv_obj_align(b_check, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(b_check, lv_color_hex(0x2a4060), 0);
    lv_obj_add_event_cb(b_check, do_check_updates, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cl = lv_label_create(b_check);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_18, 0);
    lv_label_set_text(cl, "Check"); lv_obj_center(cl);

    if (g_update_state.available) {
        lv_obj_t * b_inst = lv_btn_create(panel);
        lv_obj_set_size(b_inst, 150, 56);
        lv_obj_align(b_inst, LV_ALIGN_BOTTOM_LEFT, 188, 0);
        lv_obj_set_style_bg_color(b_inst, lv_color_hex(0x2e6e3a), 0);
        lv_obj_add_event_cb(b_inst, do_install_now, LV_EVENT_CLICKED, NULL);
        lv_obj_t * il = lv_label_create(b_inst);
        lv_obj_set_style_text_font(il, &lv_font_montserrat_18, 0);
        lv_label_set_text(il, "Install now"); lv_obj_center(il);

        lv_obj_t * b_skip = lv_btn_create(panel);
        lv_obj_set_size(b_skip, 96, 56);
        lv_obj_align(b_skip, LV_ALIGN_BOTTOM_LEFT, 346, 0);
        lv_obj_set_style_bg_color(b_skip, lv_color_hex(0x6a5424), 0);
        lv_obj_add_event_cb(b_skip, do_skip_version, LV_EVENT_CLICKED, NULL);
        lv_obj_t * sl = lv_label_create(b_skip);
        lv_obj_set_style_text_font(sl, &lv_font_montserrat_18, 0);
        lv_label_set_text(sl, "Skip"); lv_obj_center(sl);

        lv_obj_t * b_dis = lv_btn_create(panel);
        lv_obj_set_size(b_dis, 130, 56);
        lv_obj_align(b_dis, LV_ALIGN_BOTTOM_LEFT, 450, 0);
        lv_obj_set_style_bg_color(b_dis, lv_color_hex(0x33445a), 0);
        lv_obj_add_event_cb(b_dis, do_dismiss_to_envelope, LV_EVENT_CLICKED, NULL);
        lv_obj_t * dl = lv_label_create(b_dis);
        lv_obj_set_style_text_font(dl, &lv_font_montserrat_18, 0);
        lv_label_set_text(dl, LV_SYMBOL_ENVELOPE " Dismiss"); lv_obj_center(dl);
    }

    lv_obj_t * x = lv_btn_create(panel);
    lv_obj_set_size(x, 120, 56);
    lv_obj_align(x, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(0x33445a), 0);
    lv_obj_add_event_cb(x, on_update_modal_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * xl = lv_label_create(x);
    lv_obj_set_style_text_font(xl, &lv_font_montserrat_22, 0);
    lv_label_set_text(xl, "Close"); lv_obj_center(xl);
}
/* Keep the old name as an alias for the banner's click handler. */
static void on_update_banner_click(lv_event_t * e) { open_about_modal(e); }

static void open_vent(lv_event_t * e) {
    (void)e;
    ui_push(screen_vent_remote_create());
}

/* +/- buttons send a direct PWM bump (~15/255 ≈ 6%-step). e->user_data is
 * the int delta cast to a pointer. */
static void on_vent_bump(lv_event_t * e) {
    intptr_t d = (intptr_t)lv_event_get_user_data(e);
    vent_bump_speed((int)d);
}

/* Forward-declared here because on_vent_mode (next) writes to it but the
 * full set of vent statics below also references it from the timer handler.
 * See the 90-s window logic in refresh_cb's src_tag branch. */
static uint32_t   vent_local_press_ms = 0;

/* The remaining corner buttons send virtual-remote commands directly.
 * user_data is a literal C string (low/high/auto). The async send keeps
 * the LVGL event loop snappy — the HTTP roundtrip used to block ~1 s. */
static void on_vent_mode(lv_event_t * e) {
    const char * cmd = (const char *)lv_event_get_user_data(e);
    if (cmd) {
        vent_local_press_ms = lv_tick_get();
        vent_send_vremote_async(cmd);
    }
}

/* Low/High are back as discrete corner buttons; the switch flip-flopped
 * on each refresh when sync'd to fan_info, and "Auto" / "Timer" don't map
 * onto a 2-position toggle anyway. The buttons reuse on_vent_mode with
 * cmd="low" / "high". */

/* --- Curtains tile (talks to HA via REST in homeassistant.c) ---
 * lbl_curtain_state is updated from refresh_cb; the 3 button handlers
 * fire off async open/close/stop calls.
 *
 * curt_spinner is an lv_spinner that's only visible while ha_state.
 * curtain_state ∈ {"opening","closing"}. curt_bar is a position bar
 * (0..100) so the user can see at a glance how open the curtain is. */
static lv_obj_t * lbl_curtain_state = NULL;
static lv_obj_t * curt_spinner      = NULL;
static lv_obj_t * curt_bar          = NULL;
static void on_curt_open (lv_event_t * e) { (void)e; if (settings.client_mode) client_link_curtain("open");  else ha_curtain_open_async();  }
static void on_curt_close(lv_event_t * e) { (void)e; if (settings.client_mode) client_link_curtain("close"); else ha_curtain_close_async(); }
static void on_curt_stop (lv_event_t * e) { (void)e; if (settings.client_mode) client_link_curtain("stop");  else ha_curtain_stop_async();  }

/* Timer button cycles 10 → 20 → 30 → off and updates its own label.
 * "off" maps to vremotecmd=auto (the most natural resting state). */
static lv_obj_t * vent_timer_lbl = NULL;
/* Vent preset buttons — refs kept so refresh_cb can highlight the one that
 * matches vent_state.fan_info (white outline = currently active preset). */
static lv_obj_t * vent_btn_low   = NULL;
static lv_obj_t * vent_btn_high  = NULL;
static lv_obj_t * vent_btn_auto  = NULL;
static lv_obj_t * vent_btn_timer = NULL;
static int        vent_timer_step = 0;
static void on_vent_timer(lv_event_t * e) {
    (void)e;
    vent_timer_step = (vent_timer_step + 1) & 3;
    const char * cmd; const char * label;
    switch (vent_timer_step) {
        case 1:  cmd = "timer1"; label = "10m"; break;
        case 2:  cmd = "timer2"; label = "20m"; break;
        case 3:  cmd = "timer3"; label = "30m"; break;
        default: cmd = "auto";   label = "Timer"; vent_timer_step = 0; break;
    }
    if (vent_timer_lbl) lv_label_set_text(vent_timer_lbl, label);
    vent_local_press_ms = lv_tick_get();
    vent_send_vremote_async(cmd);
}

/* Apply rotation animation directly to the fan image. TRUE_COLOR_ALPHA
 * format rotates fine via lv_img_set_angle. */
static void vent_fan_anim_cb(void * obj, int32_t v) {
    lv_img_set_angle((lv_obj_t *)obj, v);
}
static void vent_apply_fan_anim(int rpm) {
    if (!vent_fan_img) return;
    /* Drive the spin off rpm — the only signal that's actually physical on
       this unit. Itho's ExhFanSpeed (%) is broken (Error:16, stuck at 0 in
       High) AND its "Low" button can produce higher rpm than "High", so
       any percentage-derived rate gives the wrong visual.

       Park the icon below 50 rpm (fan off / coasting down). */
    if (rpm < 50) {
        if (vent_anim_period_ms == 0) return;
        vent_anim_period_ms = 0;
        lv_anim_del(vent_fan_img, NULL);
        return;
    }
    /* Linear rpm → ms/turn: faster fan = shorter period. Clamped to a
       visually-perceptible range. Empirical anchors on this Itho:
         ~700 rpm (FanInfo=high)  → ~2.8 s/turn — slow but visible
         ~2500 rpm (FanInfo=low)  → ~1.0 s/turn — clearly faster
         3500+ rpm (boost)        →  ~0.2 s/turn — blur */
    int period = 3500 - rpm;
    if (period < 200)  period = 200;
    if (period > 3500) period = 3500;
    /* Hysteresis: rpm jitters ±1 every 8 s poll. Without this every poll
       would lv_anim_del + restart the spin, snapping the icon back to 0°
       (visible glitch — that's what "spinner acting weird" actually was). */
    if (abs(period - vent_anim_period_ms) < 100) return;
    vent_anim_period_ms = period;
    lv_anim_del(vent_fan_img, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, vent_fan_img);
    lv_anim_set_exec_cb(&a, vent_fan_anim_cb);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_time(&a, period);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}
static void open_settings(lv_event_t * e) {
    (void)e;
    ui_push(screen_settings_create());
}
static void open_stats(lv_event_t * e) {
    (void)e;
    ui_push(screen_stats_create());
}
static void open_forecast(lv_event_t * e) {
    (void)e;
    fprintf(stderr, "[ui] forecast tap\n");
    ui_push(screen_forecast_create());
}
static void open_inbox(lv_event_t * e) {
    (void)e;
    screen_inbox_show();
}

/* ---------- refresh timer ---------- */
/* Per-tile offline rendering. Computes whether the tile's integration is
 * "live" right now (toggle on + remote reachable), and then either:
 *   - hides the entire tile (settings.hide_offline_tiles=1), or
 *   - lets the per-tile refresh below paint an "offline" placeholder
 *     (default — keeps the layout stable, matches Energy's existing
 *     "P1 offline" treatment).
 * Called once per refresh tick from refresh_cb. */
static void apply_offline_tile_visibility(void) {
    int hide = settings.hide_offline_tiles;
    int energy_live = energy_connected();
    int water_live  = settings.enable_p1_water && hw_state.connected_water;
    int vent_live   = settings.enable_vent     && vent_state.connected;
    int family_live = settings.enable_ha;   /* family tile only needs the
                                             * HA poller running; absent
                                             * Life360 data falls through
                                             * to "?" naturally. */
    struct { lv_obj_t * tile; int live; } v[] = {
        { tile_energy,   energy_live },
        { tile_water,    water_live  },
        { tile_vent,     vent_live   },
        { tile_family,   family_live },
        { tile_curtains, family_live },   /* curtains share the HA gate */
    };
    for (size_t i = 0; i < sizeof(v)/sizeof(v[0]); i++) {
        if (!v[i].tile) continue;
        if (hide && !v[i].live) lv_obj_add_flag(v[i].tile, LV_OBJ_FLAG_HIDDEN);
        else                    lv_obj_clear_flag(v[i].tile, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Render an integration's latest_value/subtitle into the given pair of
 * labels. Returns 1 if a binding existed (caller should skip the built-in
 * refresh path), 0 otherwise. */
static int render_tile_slot(int slot, lv_obj_t * lbl_main, lv_obj_t * lbl_sub) {
    const integration_meta_t * m = tile_slots_meta_for(slot);
    if (!m) return 0;
    if (lbl_main) {
        const char * v = (const char *)m->latest_value;
        if (v && v[0])
            lv_label_set_text_fmt(lbl_main, "%s%s%s", v,
                                  m->value_unit[0] ? " " : "",
                                  m->value_unit);
        else
            lv_label_set_text_fmt(lbl_main, "%s...", m->tile_title);
    }
    if (lbl_sub) {
        const char * s = (const char *)m->latest_subtitle;
        if (s && s[0])
            lv_label_set_text_fmt(lbl_sub, "%s%s%s", s,
                                  m->subtitle_unit[0] ? " " : "",
                                  m->subtitle_unit);
        else
            lv_label_set_text(lbl_sub, "");
    }
    return 1;
}

/* Render a specific integration's latest value/subtitle/title into a page-1
 * tile's labels (used by the auto-rotate slot, which shows arbitrary
 * integrations rather than a fixed binding). */
static void render_meta_into(const integration_meta_t * m,
                             lv_obj_t * title, lv_obj_t * main, lv_obj_t * sub) {
    if (!m) return;
    if (title) lv_label_set_text(title, m->tile_title[0] ? m->tile_title : m->name);
    if (main) {
        const char * v = (const char *)m->latest_value;
        if (v && v[0]) lv_label_set_text_fmt(main, "%s%s%s", v,
                              m->value_unit[0] ? " " : "", m->value_unit);
        else           lv_label_set_text_fmt(main, "%s...", m->tile_title);
    }
    if (sub) {
        const char * s = (const char *)m->latest_subtitle;
        if (s && s[0]) lv_label_set_text_fmt(sub, "%s%s%s", s,
                              m->subtitle_unit[0] ? " " : "", m->subtitle_unit);
        else           lv_label_set_text(sub, "");
    }
}

/* Return the n-th (wrapping) integration id from the comma-separated
 * settings.tile_rotate_members list. Returns 0 if the list is empty. */
static int rotate_member_at(int n, char * out, size_t sz) {
    const char * s = settings.tile_rotate_members;
    if (!s[0]) return 0;
    int count = 1;
    for (const char * p = s; *p; p++) if (*p == ',') count++;
    int want = ((n % count) + count) % count;
    int idx = 0;
    const char * start = s;
    for (const char * p = s; ; p++) {
        if (*p == ',' || *p == 0) {
            if (idx == want) {
                size_t len = (size_t)(p - start);
                if (len >= sz) len = sz - 1;
                memcpy(out, start, len); out[len] = 0;
                /* trim spaces */
                while (out[0] == ' ') memmove(out, out + 1, strlen(out));
                return out[0] ? 1 : 0;
            }
            idx++; start = p + 1;
            if (*p == 0) break;
        }
    }
    return 0;
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;

    /* Only manage page-0 tile visibility while page 0 is showing — otherwise
     * this would clear the HIDDEN flag on the page-0 tiles every tick and let
     * them (e.g. the Family/Life360 tile) bleed through behind the page-2
     * swipe slots. */
    if (home_tile_page == 0) apply_offline_tile_visibility();

    /* Marketplace tile overrides — if a slot is bound, render the
     * integration's data into the matching tile's labels and remember
     * to skip the built-in refresh path further down. The vent tile's
     * buttons/fan stay visible (no separate hide pass) — accepted UX
     * trade-off for keeping refresh logic local. */
    int slot_active[TILE_SLOT_COUNT] = {0};
    slot_active[TILE_SLOT_ENERGY] = render_tile_slot(TILE_SLOT_ENERGY,
                                                    lbl_energy_w, lbl_energy_gas);
    slot_active[TILE_SLOT_FAMILY] = render_tile_slot(TILE_SLOT_FAMILY,
                                                    lbl_life360_ronald, lbl_life360_caja);
    slot_active[TILE_SLOT_VENT]   = render_tile_slot(TILE_SLOT_VENT,
                                                    lbl_boiler_state, lbl_boiler_pressure);
    slot_active[TILE_SLOT_WATER]  = render_tile_slot(TILE_SLOT_WATER,
                                                    lbl_inbox_main, lbl_inbox_sub);

    /* Page-1 (swipe) slots — render the bound integration, or a placeholder. */
    for (int i = 0; i < 4; i++) {
        const integration_meta_t * m = tile_slots_meta_for(TILE_SLOT_P1_0 + i);
        if (m) {
            if (p1_title[i]) lv_label_set_text(p1_title[i],
                                 m->tile_title[0] ? m->tile_title : m->name);
            render_tile_slot(TILE_SLOT_P1_0 + i, p1_main[i], p1_sub[i]);
        } else {
            if (p1_title[i]) lv_label_set_text(p1_title[i], "");
            if (p1_main[i])  lv_label_set_text(p1_main[i], LV_SYMBOL_PLUS "  Tap to assign");
            if (p1_sub[i])   lv_label_set_text(p1_sub[i], "");
        }
    }

    /* News ticker text — rebuild only when the lead headline changes (every
     * ~15 min) so the scroll animation isn't reset each second. */
    if (news_ticker) {
        if (settings.news_enabled && news_count() > 0 && home_tile_page == 0) {
            lv_obj_clear_flag(news_ticker, LV_OBJ_FLAG_HIDDEN);
            static char last_first[NEWS_TITLE_MAX] = "";
            char t0[NEWS_TITLE_MAX], l0[NEWS_LINK_MAX];
            news_item(0, t0, sizeof t0, l0, sizeof l0);
            if (strcmp(t0, last_first) != 0) {
                snprintf(last_first, sizeof last_first, "%s", t0);
                char buf[1400]; buf[0] = 0;
                int nn = news_count();
                for (int i = 0; i < nn; i++) {
                    char ti[NEWS_TITLE_MAX], li[NEWS_LINK_MAX];
                    if (news_item(i, ti, sizeof ti, li, sizeof li) != 0) continue;
                    strncat(buf, ti, sizeof buf - strlen(buf) - 1);
                    if (i < nn - 1) strncat(buf, "    -    ", sizeof buf - strlen(buf) - 1);
                }
                lv_label_set_text(news_ticker, buf);
            }
        } else {
            lv_obj_add_flag(news_ticker, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Auto-rotate: page-1 slot 0 cycles through the chosen integrations every
     * tile_rotate_seconds. Driven off this 1 Hz refresh — no extra timer. */
    if (settings.tile_rotate_enabled && settings.tile_rotate_members[0]) {
        static int rot_ctr = 0, rot_idx = 0;
        int period = settings.tile_rotate_seconds < 3 ? 3 : settings.tile_rotate_seconds;
        if (++rot_ctr >= period) { rot_ctr = 0; rot_idx++; }
        char id[48];
        if (rotate_member_at(rot_idx, id, sizeof id)) {
            const integration_meta_t * m = tile_slots_integration_by_id(id);
            if (m) render_meta_into(m, p1_title[0], p1_main[0], p1_sub[0]);
        }
    }

    /* Update-available banner — toggled live so the user sees it within
     * a refresh tick of the background checker finding a new release. */
    if (update_banner && update_banner_lbl) {
        int skipped = (skipped_version[0] &&
                       strcmp(skipped_version, g_update_state.latest_version) == 0);
        int relevant = g_update_state.available && !skipped;
        if (!relevant) update_minimized = 0;   /* reset once it's gone */
        /* Banner when relevant + not minimized; envelope when minimized. */
        if (relevant && !update_minimized) {
            lv_label_set_text_fmt(update_banner_lbl, "%s available  -  tap for details",
                                  g_update_state.latest_version);
            lv_obj_clear_flag(update_banner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(update_banner, LV_OBJ_FLAG_HIDDEN);
        }
        if (update_env_btn) {
            if (relevant && update_minimized) lv_obj_clear_flag(update_env_btn, LV_OBJ_FLAG_HIDDEN);
            else                              lv_obj_add_flag(update_env_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* Live status line inside the About/Update modal (if open) — frozen while
     * an install/result message is pinned. */
    if (about_status_lbl && !install_pinned) {
        if (g_update_state.available)
            lv_label_set_text_fmt(about_status_lbl, "Update %s available",
                                  g_update_state.latest_version);
        else if (g_update_state.last_check_epoch)
            lv_label_set_text(about_status_lbl, g_update_state.last_check_ok
                              ? "You're on the latest release." : "Update check failed - check internet.");
        else
            lv_label_set_text(about_status_lbl, "Tap \"Check for updates\".");
    }

    /* Expire any pending +/- temporary override once the schedule advances. */
    boxtalk_tick();

    /* Clock on thermostat tile */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk[16];
    strftime(clk, sizeof(clk), "%H:%M", &tm);
    lv_label_set_text(lbl_t_clock, clk);
    if (lbl_t_date) {
        char dt[48];
        strftime(dt, sizeof(dt), "%a %d %b", &tm);
        lv_label_set_text(lbl_t_date, dt);
    }
    /* Refresh the moon-phase icon at most once per refresh tick — phase
     * changes daily, not by-the-second, but keep this cheap: a single
     * src-swap when the phase slot moves. */
    if (moon_phase_img) {
        const lv_img_dsc_t * ph = moon_phase_icon(40);
        if (lv_img_get_src(moon_phase_img) != ph)
            lv_img_set_src(moon_phase_img, ph);
    }

    if (toon_state.indoor_temp > 0)
        lv_label_set_text_fmt(lbl_t_temp, "%.1f°C", display_indoor_temp(toon_state.indoor_temp));
    /* Setpoint visible at all times; the "to" prefix only when the CH
     * burner is actively heating toward it (idle just shows the target
     * temperature, no arrow). */
    if (toon_state.setpoint > 0) {
        if (toon_state.burner_on)
            lv_label_set_text_fmt(lbl_t_setpoint, "to %.1f°C", toon_state.setpoint);
        else
            lv_label_set_text_fmt(lbl_t_setpoint, "%.1f°C", toon_state.setpoint);
    } else {
        lv_label_set_text(lbl_t_setpoint, "");
    }

    /* Mode toggle + preset highlighting in one pass.  on_schedule treats a
     * +/- temporary override as "still on the schedule" — the schedule
     * daemon is parked at active_state=-1 server-side but the override
     * tick will swing it back, so the UI shouldn't pretend we're in Manual. */
    int temp_origin = boxtalk_temp_override_origin();   /* -1 if none */
    int on_schedule = (toon_state.active_state >= 0) || (temp_origin >= 0);

    if (tile_btn_mode_manual)
        lv_obj_set_style_border_width(tile_btn_mode_manual,
                                      on_schedule ? 0 : 2, 0);
    if (tile_btn_mode_program)
        lv_obj_set_style_border_width(tile_btn_mode_program,
                                      on_schedule ? 2 : 0, 0);

    /* Program-button label: just "Program" normally, "Program*" while a
     * +/- temporary override is outstanding (the schedule will reassert
     * itself at the next switch). The active preset is identifiable from
     * the highlighted button on the preset row below — no need to also
     * carry it inside the mode-toggle label. */
    int preset;
    if (toon_state.active_state >= 0 &&
        toon_state.program_state >= 0 && toon_state.program_state <= 3) {
        preset = toon_state.program_state;
    } else {
        preset = temp_origin;
    }
    if (lbl_t_program) {
        lv_label_set_text(lbl_t_program,
            boxtalk_temp_override_active() ? "Program*" : "Program");
    }

    /* Direct-preset row: white border on whichever preset is currently in
     * effect. While a +/- temporary override is armed the schedule has
     * been parked at active_state=-1 server-side, so fall back to the
     * captured origin preset so the highlight survives the nudge. Manual
     * mode (no override either) drops all borders. */
    {
        int hi = on_schedule ? preset : -1;
        for (int i = 0; i < 4; i++) {
            if (!tile_btn_preset[i]) continue;
            lv_obj_set_style_border_width(tile_btn_preset[i],
                                          (i == hi) ? 2 : 0, 0);
        }
    }

    /* Pure-icon burner indicator next to the big indoor-temp number. No
     * "-> 90 C" text — the radiator+flame glyph carries the meaning, same
     * style as the dim and heater-detail screens. */
    lv_label_set_text(lbl_t_burner, "");
    if (toon_state.burner_on) {
        lv_obj_clear_flag(tile_img_flame,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tile_img_faucet, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tile_img_drop,   LV_OBJ_FLAG_HIDDEN);
    } else if (toon_state.dhw_on) {
        lv_obj_add_flag(tile_img_flame, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(tile_img_faucet, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(tile_img_drop,   LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(tile_img_flame,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tile_img_faucet, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tile_img_drop,   LV_OBJ_FLAG_HIDDEN);
    }

    if (lbl_t_pressure) {
        if (toon_state.water_pressure > 0.1f)
            lv_label_set_text_fmt(lbl_t_pressure, "%.1f bar",
                                  toon_state.water_pressure);
        else
            lv_label_set_text(lbl_t_pressure, "-- bar");
    }
    /* Low-pressure banner. Thresholds: <0.6 = critical (red), <0.8 =
       warning (amber). >=0.8 or unknown (0) → hidden. */
    if (pressure_banner && pressure_banner_lbl) {
        float p = toon_state.water_pressure;
        if (p > 0.1f && p < 0.6f) {
            lv_obj_set_style_bg_color(pressure_banner,
                                      lv_color_hex(0xff3344), 0);
            lv_obj_set_style_bg_opa(pressure_banner, LV_OPA_COVER, 0);
            lv_label_set_text_fmt(pressure_banner_lbl,
                                  "CH water pressure CRITICAL: %.1f bar", p);
            lv_obj_clear_flag(pressure_banner, LV_OBJ_FLAG_HIDDEN);
        } else if (p > 0.1f && p < 0.8f) {
            lv_obj_set_style_bg_color(pressure_banner,
                                      lv_color_hex(0xffcc44), 0);
            lv_obj_set_style_bg_opa(pressure_banner, LV_OPA_COVER, 0);
            lv_label_set_text_fmt(pressure_banner_lbl,
                                  "CH water pressure low: %.1f bar", p);
            lv_obj_clear_flag(pressure_banner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(pressure_banner, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (lbl_t_humidity && toon_state.humidity > 0)
        lv_label_set_text_fmt(lbl_t_humidity, "RH %.0f%%",
                              toon_state.humidity);
    if (lbl_t_ppm && toon_state.eco2)
        lv_label_set_text_fmt(lbl_t_ppm, "%d ppm", toon_state.eco2);
    if (lbl_t_tvoc && toon_state.tvoc)
        lv_label_set_text_fmt(lbl_t_tvoc, "TVOC %d", toon_state.tvoc);
    if (lbl_t_aq) {
        const char * aql = air_quality_label(toon_state.eco2, toon_state.tvoc);
        if (*aql) {
            lv_label_set_text_fmt(lbl_t_aq, "Air: %s", aql);
            lv_obj_set_style_text_color(lbl_t_aq,
                lv_color_hex(air_quality_color(toon_state.eco2, toon_state.tvoc)),
                0);
        } else {
            lv_label_set_text(lbl_t_aq, "");
        }
    }

    /* (Air quality tile removed — those readings now live on the
       Heater bottom strip.) */

    /* Waste tile (top + next-up rows). Each row gets the icon and accent
       colour for its type label. */
    if (lbl_waste_date && lbl_waste_type) {
        waste_pickup_t p1, p2;
        memset(&p1, 0, sizeof p1); memset(&p2, 0, sizeof p2);
        int n = waste_state.connected ? waste_next_2_pickups(&p1, &p2) : 0;
        if (n >= 1) {
            int mo = atoi(p1.date + 5), d = atoi(p1.date + 8);
            lv_label_set_text_fmt(lbl_waste_date, "%d-%d", d, mo);
            lv_label_set_text(lbl_waste_type, p1.labels);
            if (waste_icon_1) {
                lv_img_set_src(waste_icon_1, waste_icon_for_label(p1.labels));
                lv_obj_set_style_img_recolor(waste_icon_1,
                    lv_color_hex(waste_accent_for_label(p1.labels)), 0);
            }
        } else {
            lv_label_set_text(lbl_waste_date,
                              waste_state.connected ? "--" : "...");
            lv_label_set_text(lbl_waste_type,
                              waste_state.connected ? "geen" : "");
        }
        if (n >= 2 && lbl_waste_date_2 && lbl_waste_type_2 && waste_icon_2) {
            int mo = atoi(p2.date + 5), d = atoi(p2.date + 8);
            lv_label_set_text_fmt(lbl_waste_date_2, "%d-%d", d, mo);
            lv_label_set_text(lbl_waste_type_2, p2.labels);
            lv_img_set_src(waste_icon_2, waste_icon_for_label(p2.labels));
            lv_obj_set_style_img_recolor(waste_icon_2,
                lv_color_hex(waste_accent_for_label(p2.labels)), 0);
            lv_obj_clear_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
        } else if (waste_icon_2) {
            lv_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
            if (lbl_waste_date_2) lv_label_set_text(lbl_waste_date_2, "");
            if (lbl_waste_type_2) lv_label_set_text(lbl_waste_type_2, "");
        }
    }

    /* Humidity tile is gone — humidity now lives on the Heater bottom strip.
       Keep the guarded write for any legacy reference. */
    if (toon_state.humidity > 0 && lbl_humid_val)
        lv_label_set_text_fmt(lbl_humid_val, "%.0f %%", toon_state.humidity);

    /* Energy tile — live power + cumulative gas. "Today" stays blank until
       we track a midnight-baseline; intent is to show kWh-since-midnight
       once that book-keeping lands. Skip when a marketplace integration
       has taken over this slot. */
    if (!slot_active[TILE_SLOT_ENERGY]) {
        if (lbl_energy_w) {
            if (energy_connected())
                lv_label_set_text_fmt(lbl_energy_w, "%.0f W", energy_power_w());
            else
                lv_label_set_text(lbl_energy_w, energy_offline_label());
        }
        if (lbl_energy_gas) {
            float g = energy_gas_m3();
            if (g >= 0) lv_label_set_text_fmt(lbl_energy_gas, "%.0f m3 gas", g);
            else if (energy_connected()) lv_label_set_text(lbl_energy_gas, "via meter");
        }
    }

    /* Vent tile. Top-right line combines preset + remaining ("Auto", "High",
       "Timer 25m"); bottom line combines % + rpm + source ("100 % · 695 rpm
       · vremote"). All four physical signals (preset/pct/rpm/who) on two
       label widgets so the tile stays uncluttered. Skip the whole block
       when a marketplace integration has taken over the slot — the labels
       were already rewritten by render_tile_slot. */
    if (!slot_active[TILE_SLOT_VENT] && lbl_boiler_state) {
        if (!settings.enable_vent) {
            /* Integration turned off in Settings → keep the tile visible
             * (unless hide_offline_tiles is on, in which case the whole
             * tile is already hidden by apply_offline_tile_visibility) but
             * make it obvious why no data is flowing. */
            lv_label_set_text(lbl_boiler_state, "Itho offline");
            if (vent_btn_low)   lv_obj_set_style_border_width(vent_btn_low,   0, 0);
            if (vent_btn_high)  lv_obj_set_style_border_width(vent_btn_high,  0, 0);
            if (vent_btn_auto)  lv_obj_set_style_border_width(vent_btn_auto,  0, 0);
            if (vent_btn_timer) lv_obj_set_style_border_width(vent_btn_timer, 0, 0);
        } else if (vent_state.connected) {
            /* Same snapshot trick as last_source — fan_info is a non-
             * volatile char[] written by another thread; memcpy a local
             * copy each refresh to dodge stale-cache reads. */
            char fi_local[16];
            memcpy(fi_local, (const char *)vent_state.fan_info,
                   sizeof(fi_local));
            fi_local[sizeof(fi_local) - 1] = 0;
            const char * preset = fi_local[0] ? fi_local : "?";
            char preset_pretty[16] = {0};
            /* "low"/"high" → "Low"/"High"; preserve "auto"/"medium"/"timer" */
            snprintf(preset_pretty, sizeof(preset_pretty), "%c%s",
                     (preset[0] >= 'a' && preset[0] <= 'z')
                         ? preset[0] - 'a' + 'A' : preset[0],
                     preset + 1);
            if (vent_state.remaining_min > 0)
                lv_label_set_text_fmt(lbl_boiler_state, "%s %dm",
                                      preset_pretty, vent_state.remaining_min);
            else
                lv_label_set_text(lbl_boiler_state, preset_pretty);

            /* Highlight whichever preset button matches the current fan_info
             * (Timer wins when a countdown is running). White border = active,
             * 0 = idle. ventilation.c already translates wire→user-intent. */
            int act_low   = strcmp(fi_local, "low")    == 0;
            int act_high  = strcmp(fi_local, "high")   == 0;
            int act_auto  = strcmp(fi_local, "auto")   == 0
                         || strcmp(fi_local, "medium") == 0;
            int act_timer = strcmp(fi_local, "timer")  == 0
                         || vent_state.remaining_min > 0;
            if (act_timer) { act_low = act_high = act_auto = 0; }
            if (vent_btn_low)
                lv_obj_set_style_border_width(vent_btn_low,   act_low   ? 2 : 0, 0);
            if (vent_btn_high)
                lv_obj_set_style_border_width(vent_btn_high,  act_high  ? 2 : 0, 0);
            if (vent_btn_auto)
                lv_obj_set_style_border_width(vent_btn_auto,  act_auto  ? 2 : 0, 0);
            if (vent_btn_timer)
                lv_obj_set_style_border_width(vent_btn_timer, act_timer ? 2 : 0, 0);
        } else {
            lv_label_set_text(lbl_boiler_state, "off");
            if (vent_btn_low)   lv_obj_set_style_border_width(vent_btn_low,   0, 0);
            if (vent_btn_high)  lv_obj_set_style_border_width(vent_btn_high,  0, 0);
            if (vent_btn_auto)  lv_obj_set_style_border_width(vent_btn_auto,  0, 0);
            if (vent_btn_timer) lv_obj_set_style_border_width(vent_btn_timer, 0, 0);
        }
    }
    if (!slot_active[TILE_SLOT_VENT] && lbl_boiler_pressure) {
        if (vent_state.connected) {
            /* Classify source into a 3-char tag so the line stays compact:
               API = HTML/vremote (toonui or HA), RF  = physical radio remote,
               BTN = Itho front-panel button, ???  = anything unrecognised. */
            /* Snapshot vent_state.last_source into a local before strstr —
             * the buffer is written by vent_thread without a memory
             * barrier, so the compiler/CPU can hand refresh_cb a stale
             * (all-zero) view of the chars. memcpy forces a fresh read. */
            char src_local[64];
            memcpy(src_local, (const char *)vent_state.last_source,
                   sizeof(src_local));
            src_local[sizeof(src_local) - 1] = 0;
            const char * src_tag = "?";
            const char * s = src_local;
            int api_src = (strstr(s, "vremote") || strstr(s, "HTML")) ? 1 : 0;
            if      (api_src)                                   src_tag = "API";
            else if (strstr(s, "RFT")     || strstr(s, "RF"))   src_tag = "RF";
            else if (strstr(s, "button")  || strstr(s, "Itho")) src_tag = "BTN";
            else if (s[0])                                      src_tag = s;
            /* If the source looks like a vremote/HTML call AND we issued one
             * locally within the last 90 s, attribute it to TOON. Itho can't
             * distinguish callers — this is our only signal. */
            if (api_src && vent_local_press_ms != 0 &&
                (lv_tick_get() - vent_local_press_ms) < 90000) {
                src_tag = "TOON";
            }
            lv_label_set_text_fmt(lbl_boiler_pressure,
                                  "%d%%  %drpm\nvia %s",
                                  vent_state.speed_pct,
                                  vent_state.fan_rpm,
                                  src_tag);
        } else {
            lv_label_set_text(lbl_boiler_pressure, "-- %");
        }
    }
    /* spin speed tracks fan_rpm, not the %, because Itho's % is unreliable
       on this unit (Error:16 sticks ExhFanSpeed at 0 and the Low/High labels
       are backwards relative to actual airflow). */
    if (vent_state.connected)
        vent_apply_fan_anim(vent_state.fan_rpm);

    /* Curtains tile — state + position + battery from the HA poller.
       Spinner is shown only while actively moving; the position bar always
       reflects the live percentage. */
    if (lbl_curtain_state) {
        if (ha_state.connected) {
            const char * s = ha_state.curtain_state[0]
                             ? ha_state.curtain_state : "?";
            char pretty[32] = {0};
            snprintf(pretty, sizeof(pretty), "%c%s",
                     (s[0] >= 'a' && s[0] <= 'z') ? s[0] - 'a' + 'A' : s[0],
                     s + 1);
            lv_label_set_text_fmt(lbl_curtain_state,
                                  "Gordijnen %s %d%%  bat %d%%",
                                  pretty,
                                  ha_state.curtain_pos,
                                  ha_state.curtain_battery);
            if (curt_bar) lv_bar_set_value(curt_bar, ha_state.curtain_pos,
                                           LV_ANIM_ON);
            if (curt_spinner) {
                int moving = (!strcmp(s, "opening") || !strcmp(s, "closing"));
                if (moving) lv_obj_clear_flag(curt_spinner, LV_OBJ_FLAG_HIDDEN);
                else        lv_obj_add_flag (curt_spinner, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_label_set_text(lbl_curtain_state, "Gordijnen  (HA offline)");
            if (curt_spinner) lv_obj_add_flag(curt_spinner, LV_OBJ_FLAG_HIDDEN);
        }
    }


    /* Energy bottom tile: live power + cumulative gas. */
    if (lbl_bot_energy) {
        if (energy_connected()) {
            float g = energy_gas_m3();
            if (g >= 0)
                lv_label_set_text_fmt(lbl_bot_energy, "%.0f W\n%.0f m3 gas",
                                      energy_power_w(), g);
            else
                lv_label_set_text_fmt(lbl_bot_energy, "%.0f W", energy_power_w());
        } else {
            lv_label_set_text(lbl_bot_energy, energy_offline_label());
        }
    }
    if (lbl_bot_weather && lv_label_get_text(lbl_bot_weather)
        && strcmp(lv_label_get_text(lbl_bot_weather), "...") == 0)
        lv_label_set_text(lbl_bot_weather, "(soon)");
    if (lbl_bot_waste) {
        if (waste_state.connected) {
            char date[16], labels[40];
            waste_next_pickup(date, sizeof(date), labels, sizeof(labels));
            if (date[0]) {
                /* "YYYY-MM-DD" → "DD-M" short. Keep label tight. */
                int y = atoi(date), mo = atoi(date+5), d = atoi(date+8);
                (void)y;
                lv_label_set_text_fmt(lbl_bot_waste, "%d-%d  %s", d, mo, labels);
            } else {
                lv_label_set_text(lbl_bot_waste, "geen");
            }
        } else {
            lv_label_set_text(lbl_bot_waste, "...");
        }
    }

    /* Water tile: cumulative m³ on top, per-pour line below. Live L/min
     * while flowing; right after the tap closes the session total stays
     * visible ("+1.4 L just poured") for ~60 s so the user gets immediate
     * feedback that the pour registered. Skip when a marketplace
     * integration owns this slot. */
    if (!slot_active[TILE_SLOT_WATER]) {
        if (lbl_inbox_main) {
            if (!settings.enable_p1_water)
                lv_label_set_text(lbl_inbox_main, "offline");
            else if (hw_state.connected_water)
                lv_label_set_text_fmt(lbl_inbox_main, "%.3f m3", hw_state.water_total_m3);
            else
                lv_label_set_text(lbl_inbox_main, "WTR offline");
        }
        if (lbl_inbox_sub) {
            if (!settings.enable_p1_water || !hw_state.connected_water) {
                lv_label_set_text(lbl_inbox_sub, "");
            } else if (hw_state.water_lpm > 0.05f) {
                lv_label_set_text_fmt(lbl_inbox_sub, "%.1f L/min  +%.1f L",
                                      hw_state.water_lpm,
                                      hw_state.water_session_l);
            } else if (hw_state.water_session_l > 0) {
                lv_label_set_text_fmt(lbl_inbox_sub, "+%.1f L just poured",
                                      hw_state.water_session_l);
            } else {
                lv_label_set_text(lbl_inbox_sub, "0.0 L/min");
            }
        }
    }
    /* Drop + L/m indicator on the big heater tile — visible whenever a tap
     * is open so the user sees the flow at a glance without checking the
     * (smaller) Water tile. Stays up briefly after the tap closes to show
     * the just-poured total. */
    if (tile_img_water && tile_lbl_water) {
        if (hw_state.connected_water && hw_state.water_lpm > 0.05f) {
            lv_label_set_text_fmt(tile_lbl_water, "%.1f L/m",
                                  hw_state.water_lpm);
            lv_obj_clear_flag(tile_img_water, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(tile_lbl_water, LV_OBJ_FLAG_HIDDEN);
        } else if (hw_state.connected_water && hw_state.water_session_l > 0) {
            lv_label_set_text_fmt(tile_lbl_water, "+%.1f L",
                                  hw_state.water_session_l);
            lv_obj_clear_flag(tile_img_water, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(tile_lbl_water, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(tile_img_water, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(tile_lbl_water, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* (water_spinner visibility decided ONCE at the bottom of refresh_cb
     * with the proper connected_water + >0.05 LPM gate. This earlier copy
     * was a splat-recovery duplicate with a too-loose 0.01 threshold and
     * no connected check, so meter wobble or stale data could leave the
     * spinner running indefinitely. Dropped.) */

    /* Outside tile — current temp + short description. */
    if (lbl_outside_main) {
        if (weather_state.connected)
            lv_label_set_text_fmt(lbl_outside_main, "%.1f°C", weather_state.current_temp);
        else
            lv_label_set_text(lbl_outside_main, "-- C");
    }

    /* Forecast band: honour settings.forecast_mode (auto / forced hourly /
       forced daily). Forced-hourly still falls back to daily if the hourly
       fetch has nothing yet. */
    int show_hourly = settings.forecast_mode != FORECAST_DAILY
                      && weather_state.hour_count > 0;
    if (show_hourly) {
        /* The "Medemblik - X.X C now" header above the strip already shows
         * the current hour's value, so start at index 1 (the next 3-hour
         * slot) to avoid duplicating it in the first column. */
        for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
            int si = i + 1;     /* skip slot 0 — that's "now" */
            if (si < weather_state.hour_count) {
                const weather_hour_t * h = &weather_state.hours[si];
                lv_label_set_text(fc_day_lbl[i], h->label);
                lv_label_set_text_fmt(fc_temp_lbl[i], "%.0f\xc2\xb0",
                                      h->temperature);
                set_forecast_icon(fc_icon[i], fc_icon_sun[i], h->icon);
                if (h->wind_dir[0]) {
                    lv_label_set_text_fmt(fc_wind_lbl[i], "%s %d Bft",
                                          h->wind_dir, h->wind_bft);
                    int ang = wind_dir_angle(h->wind_dir);
                    if (fc_wind_arrow[i] && ang >= 0) {
                        lv_img_set_angle(fc_wind_arrow[i], ang);
                        lv_obj_clear_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                    } else if (fc_wind_arrow[i]) {
                        lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                    }
                } else {
                    lv_label_set_text(fc_wind_lbl[i], "");
                    if (fc_wind_arrow[i])
                        lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                /* Past the hourly horizon — fall back to the daily forecast
                 * so the last column(s) still carry signal. Index into days[]
                 * starting at 0 (tomorrow / first daily entry available). */
                int di = si - weather_state.hour_count;
                if (di >= weather_state.day_count) {
                    lv_label_set_text(fc_day_lbl[i], "");
                    lv_label_set_text(fc_temp_lbl[i], "");
                    lv_label_set_text(fc_wind_lbl[i], "");
                    if (fc_wind_arrow[i])
                        lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                    continue;
                }
                const weather_day_t * d = &weather_state.days[di];
                lv_label_set_text(fc_day_lbl[i], d->day);
                lv_label_set_text_fmt(fc_temp_lbl[i],
                                      "%.0f\xc2\xb0 (%.0f\xc2\xb0)",
                                      d->max_temp, d->min_temp);
                set_forecast_icon(fc_icon[i], fc_icon_sun[i], d->icon);
                if (d->wind_dir[0]) {
                    lv_label_set_text_fmt(fc_wind_lbl[i], "%s %d Bft",
                                          d->wind_dir, d->wind_bft);
                    int ang = wind_dir_angle(d->wind_dir);
                    if (fc_wind_arrow[i] && ang >= 0) {
                        lv_img_set_angle(fc_wind_arrow[i], ang);
                        lv_obj_clear_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                    } else if (fc_wind_arrow[i]) {
                        lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                    }
                } else {
                    lv_label_set_text(fc_wind_lbl[i], "");
                    if (fc_wind_arrow[i])
                        lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    } else {
        for (int i = 0; i < weather_state.day_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_day_t * d = &weather_state.days[i];
            lv_label_set_text(fc_day_lbl[i], d->day);
            lv_label_set_text_fmt(fc_temp_lbl[i],
                                  "%.0f\xc2\xb0 (%.0f\xc2\xb0)",
                                  d->max_temp, d->min_temp);
            set_forecast_icon(fc_icon[i], fc_icon_sun[i], d->icon);
            if (d->wind_dir[0]) {
                lv_label_set_text_fmt(fc_wind_lbl[i], "%s %d Bft",
                                      d->wind_dir, d->wind_bft);
                int ang = wind_dir_angle(d->wind_dir);
                if (fc_wind_arrow[i] && ang >= 0) {
                    lv_img_set_angle(fc_wind_arrow[i], ang);
                    lv_obj_clear_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                lv_label_set_text(fc_wind_lbl[i], "");
                if (fc_wind_arrow[i])
                    lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    /* Show the spinner only while water is actually flowing. */
    if (water_spinner) {
        if (hw_state.connected_water && hw_state.water_lpm > 0.05f)
            lv_obj_clear_flag(water_spinner, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(water_spinner, LV_OBJ_FLAG_HIDDEN);
    }

    /* Outside tile — current temp + short description on second line. */
    if (lbl_outside_main && weather_state.connected) {
        lv_label_set_text_fmt(lbl_outside_main, "%.1f°C",
                              weather_state.current_temp);
    }
    /* "Medemblik - 14.7 C now" header above the forecast strip. Life360
     * moved to its own tile so this stays purely weather. */
    if (lbl_forecast_city) {
        const char * city = settings.weather_location[0] ? settings.weather_location : "Forecast";
        if (weather_state.connected)
            lv_label_set_text_fmt(lbl_forecast_city, "%s  -  %.1f°C now",
                                  city, weather_state.current_temp);
        else
            lv_label_set_text(lbl_forecast_city, city);
    }

    /* Family tile (Life360). Name prefix + scrolling address. When HA
     * integration is off there's no poller to populate ha_state, so
     * surface that explicitly instead of leaving a pair of stale "?"s
     * that could be mistaken for "in transit" or "out of range".
     * Skip when a marketplace integration owns this slot. */
    if (!slot_active[TILE_SLOT_FAMILY]) {
        if (lbl_life360_ronald) {
            if (!settings.enable_ha)
                lv_label_set_text(lbl_life360_ronald, "HA offline");
            else
                lv_label_set_text_fmt(lbl_life360_ronald, "Ronald: %s",
                    ha_state.loc_ronald[0] ? ha_state.loc_ronald : "?");
        }
        if (lbl_life360_caja) {
            if (!settings.enable_ha)
                lv_label_set_text(lbl_life360_caja, "");
            else
                lv_label_set_text_fmt(lbl_life360_caja, "Caja: %s",
                    ha_state.loc_caja[0]   ? ha_state.loc_caja   : "?");
        }
    }

    /* Forecast band — splat-recovery left two more copies of this
       writer further down; gate them on the same hourly/daily decision so
       the data the first writer painted isn't immediately overwritten. */
    if (!show_hourly) {
        for (int i = 0; i < weather_state.day_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_day_t * d = &weather_state.days[i];
            if (fc_day_lbl[i])
                lv_label_set_text(fc_day_lbl[i], d->day);
            if (fc_temp_lbl[i])
                lv_label_set_text_fmt(fc_temp_lbl[i],
                                      "%.0f\xc2\xb0 (%.0f\xc2\xb0)",
                                      d->max_temp, d->min_temp);
            if (fc_desc_lbl[i])
                lv_label_set_text(fc_desc_lbl[i], d->desc);
        }
    }

    /* Energy bottom tile: live power + cumulative gas. */
    if (lbl_bot_energy) {
        if (energy_connected()) {
            float g = energy_gas_m3();
            if (g >= 0)
                lv_label_set_text_fmt(lbl_bot_energy, "%.0f W\n%.0f m3 gas",
                                      energy_power_w(), g);
            else
                lv_label_set_text_fmt(lbl_bot_energy, "%.0f W", energy_power_w());
        } else {
            lv_label_set_text(lbl_bot_energy, energy_offline_label());
        }
    }
    if (lbl_bot_weather && lv_label_get_text(lbl_bot_weather)
        && strcmp(lv_label_get_text(lbl_bot_weather), "...") == 0)
        lv_label_set_text(lbl_bot_weather, "(soon)");
    if (lbl_bot_waste) {
        if (waste_state.connected) {
            char date[16], labels[40];
            waste_next_pickup(date, sizeof(date), labels, sizeof(labels));
            if (date[0]) {
                /* "YYYY-MM-DD" → "DD-M" short. Keep label tight. */
                int y = atoi(date), mo = atoi(date+5), d = atoi(date+8);
                (void)y;
                lv_label_set_text_fmt(lbl_bot_waste, "%d-%d  %s", d, mo, labels);
            } else {
                lv_label_set_text(lbl_bot_waste, "geen");
            }
        } else {
            lv_label_set_text(lbl_bot_waste, "...");
        }
    }

    /* Water tile: cumulative m³ on top, per-pour line below. Live L/min
     * while flowing; right after the tap closes the session total stays
     * visible ("+1.4 L just poured") for ~60 s so the user gets immediate
     * feedback that the pour registered. Skip when a marketplace
     * integration owns this slot. */
    if (!slot_active[TILE_SLOT_WATER]) {
        if (lbl_inbox_main) {
            if (!settings.enable_p1_water)
                lv_label_set_text(lbl_inbox_main, "offline");
            else if (hw_state.connected_water)
                lv_label_set_text_fmt(lbl_inbox_main, "%.3f m3", hw_state.water_total_m3);
            else
                lv_label_set_text(lbl_inbox_main, "WTR offline");
        }
        if (lbl_inbox_sub) {
            if (!settings.enable_p1_water || !hw_state.connected_water) {
                lv_label_set_text(lbl_inbox_sub, "");
            } else if (hw_state.water_lpm > 0.05f) {
                lv_label_set_text_fmt(lbl_inbox_sub, "%.1f L/min  +%.1f L",
                                      hw_state.water_lpm,
                                      hw_state.water_session_l);
            } else if (hw_state.water_session_l > 0) {
                lv_label_set_text_fmt(lbl_inbox_sub, "+%.1f L just poured",
                                      hw_state.water_session_l);
            } else {
                lv_label_set_text(lbl_inbox_sub, "0.0 L/min");
            }
        }
    }
    /* Drop + L/m indicator on the big heater tile — visible whenever a tap
     * is open so the user sees the flow at a glance without checking the
     * (smaller) Water tile. Stays up briefly after the tap closes to show
     * the just-poured total. */
    if (tile_img_water && tile_lbl_water) {
        if (hw_state.connected_water && hw_state.water_lpm > 0.05f) {
            lv_label_set_text_fmt(tile_lbl_water, "%.1f L/m",
                                  hw_state.water_lpm);
            lv_obj_clear_flag(tile_img_water, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(tile_lbl_water, LV_OBJ_FLAG_HIDDEN);
        } else if (hw_state.connected_water && hw_state.water_session_l > 0) {
            lv_label_set_text_fmt(tile_lbl_water, "+%.1f L",
                                  hw_state.water_session_l);
            lv_obj_clear_flag(tile_img_water, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(tile_lbl_water, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(tile_img_water, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(tile_lbl_water, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* (water_spinner visibility decided ONCE at the bottom of refresh_cb
     * with the proper connected_water + >0.05 LPM gate. This earlier copy
     * was a splat-recovery duplicate with a too-loose 0.01 threshold and
     * no connected check, so meter wobble or stale data could leave the
     * spinner running indefinitely. Dropped.) */

    /* Outside tile — current temp + short description. */
    if (lbl_outside_main) {
        if (weather_state.connected)
            lv_label_set_text_fmt(lbl_outside_main, "%.1f°C", weather_state.current_temp);
        else
            lv_label_set_text(lbl_outside_main, "-- C");
    }

    /* Forecast band — third splat-recovery copy of the daily writer;
       same gating as the earlier two. */
    if (!show_hourly) {
        for (int i = 0; i < weather_state.day_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_day_t * d = &weather_state.days[i];
            lv_label_set_text(fc_day_lbl[i], d->day);
            lv_label_set_text_fmt(fc_temp_lbl[i],
                                  "%.0f\xc2\xb0 (%.0f\xc2\xb0)",
                                  d->max_temp, d->min_temp);
            set_forecast_icon(fc_icon[i], fc_icon_sun[i], d->icon);
            if (d->wind_dir[0])
                lv_label_set_text_fmt(fc_wind_lbl[i], "%s %d Bft",
                                      d->wind_dir, d->wind_bft);
            else
                lv_label_set_text(fc_wind_lbl[i], "");
        }
    }

    /* Energy bottom tile: live power + cumulative gas. */
    if (lbl_bot_energy) {
        if (energy_connected()) {
            float g = energy_gas_m3();
            if (g >= 0)
                lv_label_set_text_fmt(lbl_bot_energy, "%.0f W\n%.0f m3 gas",
                                      energy_power_w(), g);
            else
                lv_label_set_text_fmt(lbl_bot_energy, "%.0f W", energy_power_w());
        } else {
            lv_label_set_text(lbl_bot_energy, energy_offline_label());
        }
    }
    /* (Old splat-recovered "(soon)" override block removed — it was
       clobbering the real waste-pickup text written earlier in this
       refresh callback.) */

    /* Water tile (replaces Inbox placeholder) — total + live l/min.
     * Splat-recovered duplicate of the earlier guarded block; same
     * marketplace-slot guard applies here. */
    if (!slot_active[TILE_SLOT_WATER]) {
        if (lbl_inbox_main && hw_state.connected_water) {
            lv_label_set_text_fmt(lbl_inbox_main, "%.3f m3", hw_state.water_total_m3);
        }
        if (lbl_inbox_sub && hw_state.connected_water) {
            lv_label_set_text_fmt(lbl_inbox_sub, "%.1f L/min", hw_state.water_lpm);
        }
    }

    lv_obj_invalidate(scr_root);
}

/* ---------- screen builder ---------- */
/* Push the Lights page. Used by both the swipe-right gesture and the
 * lightbulb button in the top-right. */
/* Route to whichever lights backend is enabled: Domoticz if configured,
 * otherwise the Home Assistant lights page. */
static void open_lights_backend(void) {
    if (settings.enable_domoticz) ui_push(screen_domoticz_create());
    else                          ui_push(screen_lights_create());
}

/* ---- swipeable tile pages ---- */
static void home_set_dot(int active) {
    for (int i = 0; i < 2; i++)
        if (home_dot[i])
            lv_obj_set_style_bg_opa(home_dot[i],
                                    i == active ? LV_OPA_COVER : LV_OPA_30, 0);
}
static void home_show_page(int n) {
    home_tile_page = n;
    lv_obj_t * p0[5] = { tile_waste, tile_energy, tile_vent, tile_family, tile_water };
    if (n == 0) {
        for (int i = 0; i < 5; i++) if (p0[i]) lv_obj_clear_flag(p0[i], LV_OBJ_FLAG_HIDDEN);
        if (home_page1) lv_obj_add_flag(home_page1, LV_OBJ_FLAG_HIDDEN);
        /* ticker belongs to page 0 — show it again (if news is on) */
        if (news_ticker && settings.news_enabled) lv_obj_clear_flag(news_ticker, LV_OBJ_FLAG_HIDDEN);
        apply_offline_tile_visibility();   /* re-hide offline tiles after un-hiding */
    } else {
        for (int i = 0; i < 5; i++) if (p0[i]) lv_obj_add_flag(p0[i], LV_OBJ_FLAG_HIDDEN);
        if (home_page1) lv_obj_clear_flag(home_page1, LV_OBJ_FLAG_HIDDEN);
        /* hide the page-0 ticker so it doesn't draw over the page-2 slots */
        if (news_ticker) lv_obj_add_flag(news_ticker, LV_OBJ_FLAG_HIDDEN);
    }
    home_set_dot(n);
}
static void on_page1_slot(lv_event_t * e) {
    (void)e;
    screen_settings_open_tile_slots_modal();
}
void screen_home_reset_to_main(void) {
    if (home_tile_page != 0) home_show_page(0);
}
static void on_home_gesture_to_lights(lv_event_t * e) {
    (void)e;
    open_lights_backend();
}

/* Collapsible lights handle: idles as a slim half-circle tab peeking from the
 * left edge, expands into a horizontal "Lights" button while pressed, and
 * opens the lights backend on release. */
static lv_obj_t * lights_handle     = NULL;
static lv_obj_t * lights_handle_lbl = NULL;

static void lights_handle_set(bool open) {
    if (!lights_handle) return;
    lv_obj_set_size(lights_handle, open ? 132 : 22, open ? 56 : 64);
    lv_obj_align(lights_handle, LV_ALIGN_LEFT_MID, open ? 2 : -10, 0);
    lv_obj_set_style_radius(lights_handle, open ? 16 : LV_RADIUS_CIRCLE, 0);
    if (open) lv_obj_clear_flag(lights_handle_lbl, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(lights_handle_lbl, LV_OBJ_FLAG_HIDDEN);
}
static void on_lights_handle(lv_event_t * e) {
    switch (lv_event_get_code(e)) {
        case LV_EVENT_PRESSED:                            lights_handle_set(true);  break;
        case LV_EVENT_RELEASED: case LV_EVENT_PRESS_LOST: lights_handle_set(false); break;
        case LV_EVENT_CLICKED:  lights_handle_set(false); open_lights_backend();    break;
        default: break;
    }
}

static void on_home_gesture(lv_event_t * e) {
    (void)e;
    lv_indev_t * indev = lv_indev_get_act();
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    lv_point_t p; lv_indev_get_point(indev, &p);
    /* Right of x≈556 is the tile zone: swipe pages between tile sets.
     * Left of it (the thermostat) keeps the swipe-right-for-lights shortcut. */
    if (p.x >= 556) {
        if      (dir == LV_DIR_LEFT)  home_show_page(1);
        else if (dir == LV_DIR_RIGHT) home_show_page(0);
    } else if (dir == LV_DIR_RIGHT) {
        open_lights_backend();
    }
}

/* ---- news ticker tap → headline list + per-article QR ---- */
static void news_modal_close(lv_event_t * e) {
    (void)e;
    if (news_modal) { lv_obj_del(news_modal); news_modal = NULL; news_qr = NULL; }
}
static void on_news_item(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char title[NEWS_TITLE_MAX], link[NEWS_LINK_MAX];
    if (news_item(i, title, sizeof title, link, sizeof link) != 0) return;
    if (news_qr && link[0]) lv_qrcode_update(news_qr, link, strlen(link));
}
static void on_news_tap(lv_event_t * e) {
    (void)e;
    if (news_modal || news_count() == 0) return;
    news_modal = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(news_modal);
    lv_obj_set_size(news_modal, 1024, 600);
    lv_obj_set_style_bg_color(news_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(news_modal, LV_OPA_70, 0);
    lv_obj_add_flag(news_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(news_modal, news_modal_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t * card = lv_obj_create(news_modal);
    lv_obj_set_size(card, 900, 500);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x16243a), 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   /* swallow taps */

    lv_obj_t * t = lv_label_create(card);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), 0);
    lv_label_set_text(t, "Nieuws");
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 16, 12);

    lv_obj_t * list = lv_list_create(card);
    lv_obj_set_size(list, 540, 420);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 12, 56);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x0e1a2a), 0);
    int n = news_count();
    for (int i = 0; i < n; i++) {
        char ti[NEWS_TITLE_MAX], li[NEWS_LINK_MAX];
        if (news_item(i, ti, sizeof ti, li, sizeof li) != 0) continue;
        lv_obj_t * b = lv_list_add_btn(list, NULL, ti);
        lv_obj_set_style_text_font(b, &lv_font_montserrat_18, 0);
        lv_obj_add_event_cb(b, on_news_item, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    news_qr = lv_qrcode_create(card, 240, lv_color_hex(0x000000), lv_color_hex(0xffffff));
    lv_obj_align(news_qr, LV_ALIGN_TOP_RIGHT, -50, 90);
    lv_obj_t * ql = lv_label_create(card);
    lv_obj_set_style_text_color(ql, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(ql, &lv_font_montserrat_18, 0);
    lv_label_set_text(ql, "Scan om te openen");
    lv_obj_align(ql, LV_ALIGN_TOP_RIGHT, -90, 340);

    char t0[NEWS_TITLE_MAX], l0[NEWS_LINK_MAX];
    if (news_item(0, t0, sizeof t0, l0, sizeof l0) == 0 && l0[0])
        lv_qrcode_update(news_qr, l0, strlen(l0));

    lv_obj_t * x = lv_btn_create(card);
    lv_obj_set_size(x, 130, 48);
    lv_obj_align(x, LV_ALIGN_BOTTOM_RIGHT, -16, -12);
    lv_obj_add_event_cb(x, news_modal_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * xl = lv_label_create(x); lv_label_set_text(xl, "Sluiten"); lv_obj_center(xl);
}

lv_obj_t * screen_home_create(void) {
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_home_gesture, LV_EVENT_GESTURE, NULL);

    /* Top-of-screen package banner (hidden when queue is empty). Tap
     * dismisses the top entry. Receives from MQTT via packages.c. */
    packages_banner_attach(scr_root);

    /* Layout (1024x600):
         Big thermostat tile:  20,20  520x420   (col 0-1 spanned, row 0-1 spanned)
         Tile col 2 row 0:    560,20  220x200   Air quality
         Tile col 3 row 0:    790,20  220x200   Humidity
         Tile col 2 row 1:    560,230 220x200   Boiler / heating
         Tile col 3 row 1:    790,230 220x200   Inbox (placeholder)
         Bottom row (4 small tiles): 20,450 -> 1004,580
           Energy (P1), Buienradar, Waste, Settings
    */

    /* --- Big thermostat tile (height 360 — was 410, lost 50 px to make room
           for the new Curtains strip below it). --- */
    lv_obj_t * th = lv_obj_create(scr_root);
    lv_obj_set_size(th, 520, 360);   /* bottom flush with the curtains/right column */
    lv_obj_set_pos(th, 20, 20);
    lv_obj_set_style_bg_color(th, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_border_width(th, 0, 0);
    lv_obj_set_style_radius(th, 18, 0);
    lv_obj_set_style_pad_all(th, 20, 0);
    lv_obj_clear_flag(th, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(th, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(th, open_thermostat, LV_EVENT_CLICKED, NULL);

    lbl_t_clock = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_clock, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_t_clock, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_t_clock, "--:--");
    lv_obj_align(lbl_t_clock, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Date directly under the clock so the top-left corner is a full
       date+time block. */
    lbl_t_date = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_date, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_t_date, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_t_date, "");
    lv_obj_align(lbl_t_date, LV_ALIGN_TOP_LEFT, 0, 36);

    /* Moon phase — 40-px icon sitting below the date, just left-of-centre
     * inside the heater tile. ALWAYS shown so the user sees the current
     * lunar phase even during the day; the icon is separate from the
     * forecast band so it doesn't compete with the weather icon. A fixed
     * pixel position is used (rather than align-to-date-label) because
     * lv_obj_align_to runs before the label has measured its text width
     * → the icon ends up clipped behind the clock. */
    moon_phase_img = lv_img_create(th);
    lv_img_set_src(moon_phase_img, moon_phase_icon(40));
    lv_obj_set_style_img_recolor(moon_phase_img, lv_color_hex(0xe8edf2), 0);
    lv_obj_set_style_img_recolor_opa(moon_phase_img, 255, 0);
    /* Position: hovering to the right of "Tue 19 May", at clock baseline.
     * Using lv_obj_align (not lv_obj_set_pos) because the heater tile is
     * a styled container and set_pos has been observed to clip behind
     * other children. The 40-px icon spans (148+20, 24+20) → (188+20,
     * 64+20) inside the padded tile — well clear of the temp number. */
    lv_obj_align(moon_phase_img, LV_ALIGN_TOP_LEFT, 148, 24);

    /* Air-quality badge — top-right of the Heater tile, coloured per
       eCO2/TVOC bucket (green/lime/yellow/orange/red). */
    lbl_t_aq = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_aq, lv_color_hex(0x66cc88), 0);
    lv_obj_set_style_text_font(lbl_t_aq, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_t_aq, "");
    lv_obj_align(lbl_t_aq, LV_ALIGN_TOP_RIGHT, 0, 6);

    /* "Thermostat" title removed per request — the gear icon and tile
       content already make this tile's role obvious. */

    lbl_t_temp = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_temp, lv_color_hex(COL_TEMP_YELLOW), 0);
    lv_obj_set_style_text_font(lbl_t_temp, &lv_font_montserrat_48, 0);
    lv_label_set_text(lbl_t_temp, "-- C");
    lv_obj_align(lbl_t_temp, LV_ALIGN_CENTER, 0, -90);

    /* Setpoint row: [-] setpoint [+]. sp_row itself is NOT clickable so
     * taps on its centre (the setpoint label dead-zone between the two
     * buttons) pass through to the tile and open the detail page. The
     * +/- buttons keep their own click handlers. */
    lv_obj_t * sp_row = lv_obj_create(th);
    lv_obj_set_size(sp_row, 460, 84);
    lv_obj_align(sp_row, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_opa(sp_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp_row, 0, 0);
    lv_obj_set_style_pad_all(sp_row, 0, 0);
    lv_obj_clear_flag(sp_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(sp_row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * btn_dn = lv_btn_create(sp_row);
    lv_obj_set_size(btn_dn, 84, 76);
    lv_obj_align(btn_dn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_dn, lv_color_hex(COL_TILE_ACCENT), 0);
    lv_obj_set_style_radius(btn_dn, 12, 0);
    lv_obj_set_ext_click_area(btn_dn, 12);
    lv_obj_add_event_cb(btn_dn, on_tile_sp_down, LV_EVENT_CLICKED, NULL);
    lv_obj_t * btn_dn_lbl = lv_label_create(btn_dn);
    lv_label_set_text(btn_dn_lbl, "-");
    lv_obj_set_style_text_font(btn_dn_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(btn_dn_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_center(btn_dn_lbl);

    lbl_t_setpoint = lv_label_create(sp_row);
    lv_obj_set_style_text_color(lbl_t_setpoint, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_t_setpoint, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_t_setpoint, "to -- C");
    lv_obj_align(lbl_t_setpoint, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t * btn_up = lv_btn_create(sp_row);
    lv_obj_set_size(btn_up, 84, 76);
    lv_obj_align(btn_up, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_up, lv_color_hex(COL_TILE_ACCENT), 0);
    lv_obj_set_style_radius(btn_up, 12, 0);
    lv_obj_set_ext_click_area(btn_up, 12);
    lv_obj_add_event_cb(btn_up, on_tile_sp_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t * btn_up_lbl = lv_label_create(btn_up);
    lv_label_set_text(btn_up_lbl, "+");
    lv_obj_set_style_text_font(btn_up_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(btn_up_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_center(btn_up_lbl);

    /* Mode toggle row — Manual | Program, both tappable. Active mode gets
     * a white outline. Buttons are compact (110 + 140) — the active preset
     * is already visible from the highlighted preset button on the row
     * below, so the Program button doesn't need to carry it. An asterisk
     * appears on Program while a +/- temporary override is in flight. */
    {
        const int manual_w = 110, prog_w = 140, btn_h = 38, gap = 6;
        const int total   = manual_w + prog_w + gap;
        const int left_x  = -total / 2 + manual_w / 2;
        const int right_x = -total / 2 + manual_w + gap + prog_w / 2;

        tile_btn_mode_manual = lv_btn_create(th);
        lv_obj_set_size(tile_btn_mode_manual, manual_w, btn_h);
        lv_obj_align(tile_btn_mode_manual, LV_ALIGN_CENTER, left_x, 70);
        lv_obj_set_style_bg_color(tile_btn_mode_manual, lv_color_hex(0x6a5424), 0);
        lv_obj_set_style_radius(tile_btn_mode_manual, 19, 0);
        lv_obj_set_style_border_color(tile_btn_mode_manual, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_border_width(tile_btn_mode_manual, 0, 0);
        lv_obj_set_ext_click_area(tile_btn_mode_manual, 8);
        lv_obj_add_event_cb(tile_btn_mode_manual, on_mode_manual,
                            LV_EVENT_CLICKED, NULL);
        lv_obj_t * ml = lv_label_create(tile_btn_mode_manual);
        lv_label_set_text(ml, "Manual");
        lv_obj_set_style_text_color(ml, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(ml, &lv_font_montserrat_22, 0);
        lv_obj_center(ml);

        tile_btn_mode_program = lv_btn_create(th);
        lv_obj_set_size(tile_btn_mode_program, prog_w, btn_h);
        lv_obj_align(tile_btn_mode_program, LV_ALIGN_CENTER, right_x, 70);
        lv_obj_set_style_bg_color(tile_btn_mode_program, lv_color_hex(0x2f6b6b), 0);
        lv_obj_set_style_radius(tile_btn_mode_program, 19, 0);
        lv_obj_set_style_border_color(tile_btn_mode_program, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_border_width(tile_btn_mode_program, 0, 0);
        lv_obj_set_ext_click_area(tile_btn_mode_program, 8);
        lv_obj_add_event_cb(tile_btn_mode_program, on_mode_program,
                            LV_EVENT_CLICKED, NULL);
        lbl_t_program = lv_label_create(tile_btn_mode_program);
        lv_label_set_text(lbl_t_program, "Program");
        lv_obj_set_style_text_color(lbl_t_program, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(lbl_t_program, &lv_font_montserrat_22, 0);
        lv_obj_center(lbl_t_program);
    }

    /* Direct-preset row just below the pill — one tap to set Comfort /
     * Home / Sleep / Away. Colours mirror the schedule editor and heater
     * detail screen for instant recognition. The active preset gets a
     * white border, updated in refresh_cb. Width 78 so "Comfort" doesn't
     * clip at 18-pt; row centred at +118 so it sits between the pill
     * (bottom ~y=276) and the metrics strip (top ~y=334). */
    {
        const char * names[4] = {"Comfort", "Home", "Sleep", "Away"};
        uint32_t     cols[4]  = {0xcc7733, 0x3377cc, 0x553388, 0x557788};
        const int    bw = 78, bh = 34, gap = 4;
        int total = 4 * bw + 3 * gap;
        for (int i = 0; i < 4; i++) {
            lv_obj_t * b = lv_btn_create(th);
            lv_obj_set_size(b, bw, bh);
            lv_obj_align(b, LV_ALIGN_CENTER,
                         -total / 2 + i * (bw + gap) + bw / 2, 124);
            lv_obj_set_style_bg_color(b, lv_color_hex(cols[i]), 0);
            lv_obj_set_style_radius(b, 8, 0);
            lv_obj_set_style_border_color(b, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_pad_all(b, 0, 0);
            lv_obj_set_ext_click_area(b, 6);
            lv_obj_add_event_cb(b, on_tile_preset, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
            lv_obj_t * bl = lv_label_create(b);
            lv_label_set_text(bl, names[i]);
            lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_text_font(bl, &lv_font_montserrat_18, 0);
            lv_obj_center(bl);
            tile_btn_preset[i] = b;
        }
    }

    /* Burner status sits above the metrics strip. The icons follow it so
       the flame/faucet stays paired with the text. */
    lbl_t_burner = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_burner, lv_color_hex(COL_BURNER_RED), 0);
    lv_obj_set_style_text_font(lbl_t_burner, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_t_burner, "idle");
    lv_obj_align(lbl_t_burner, LV_ALIGN_BOTTOM_MID, 30, -40);

    /* Bottom strip: humidity | eCO2 | TVOC | water-pressure on one row.
       Font 18 keeps the 4 values from running into each other on a
       520-wide tile. y offset 0 pins them flush with the tile bottom
       so the mode-toggle and preset rows above have more breathing room. */
    lbl_t_humidity = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_humidity, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_t_humidity, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_t_humidity, "RH --%");
    lv_obj_align(lbl_t_humidity, LV_ALIGN_BOTTOM_LEFT, 12, 8);

    lbl_t_ppm = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_ppm, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_t_ppm, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_t_ppm, "-- ppm");
    lv_obj_align(lbl_t_ppm, LV_ALIGN_BOTTOM_LEFT, 140, 8);

    lbl_t_tvoc = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_tvoc, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_t_tvoc, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_t_tvoc, "TVOC --");
    lv_obj_align(lbl_t_tvoc, LV_ALIGN_BOTTOM_LEFT, 280, 8);

    lbl_t_pressure = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_pressure, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_t_pressure, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_t_pressure, "-- bar");
    lv_obj_align(lbl_t_pressure, LV_ALIGN_BOTTOM_RIGHT, -12, 8);

    /* CH-pressure warning banner. Sits on top of the Heater tile so the
       user can't miss it. EVENT_BUBBLE keeps the tile clickable through
       the banner. Hidden by default; shown only when pressure is low. */
    pressure_banner = lv_obj_create(th);
    lv_obj_set_size(pressure_banner, 520, 42);
    lv_obj_align(pressure_banner, LV_ALIGN_TOP_LEFT, -10, -10);
    lv_obj_set_style_radius(pressure_banner, 0, 0);
    lv_obj_set_style_border_width(pressure_banner, 0, 0);
    lv_obj_clear_flag(pressure_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pressure_banner, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(pressure_banner, LV_OBJ_FLAG_HIDDEN);

    pressure_banner_lbl = lv_label_create(pressure_banner);
    lv_obj_set_style_text_color(pressure_banner_lbl,
                                lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(pressure_banner_lbl,
                               &lv_font_montserrat_22, 0);
    lv_label_set_text(pressure_banner_lbl, "");
    lv_obj_center(pressure_banner_lbl);
    lv_obj_add_flag(pressure_banner_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Burner indicator icons — replace the old "heating"/"hot water" word.
     * Native 32×40 source; render at full size (zoom 256) so the symbol
     * reads at a glance. Flame sits left of the target-temp label, faucet
     * & drop pair on the left for DHW. */
    /* Icons sit to the right of the big indoor-temp label (lbl_t_temp at
     * CENTER (0, -90), 48-pt font ≈ 150 px wide). Same placement style
     * as dim and heater-detail screens. */
    tile_img_flame = lv_img_create(th);
    lv_img_set_src(tile_img_flame, &icon_radiator);
    lv_img_set_zoom(tile_img_flame, 256);
    lv_obj_set_style_img_recolor(tile_img_flame, lv_color_hex(COL_BURNER_RED), 0);
    lv_obj_set_style_img_recolor_opa(tile_img_flame, 255, 0);
    lv_obj_align(tile_img_flame, LV_ALIGN_CENTER, 110, -90);
    lv_obj_add_flag(tile_img_flame, LV_OBJ_FLAG_HIDDEN);

    /* DHW indicator is FULLY red (faucet + drop both tinted) so it's
     * unambiguously distinct from the blue cold-water-flow drop on the
     * left side. Silver faucet was too easy to mistake for "off". */
    tile_img_faucet = lv_img_create(th);
    lv_img_set_src(tile_img_faucet, &icon_faucet);
    lv_img_set_zoom(tile_img_faucet, 256);
    lv_obj_set_style_img_recolor(tile_img_faucet, lv_color_hex(0xff5544), 0);
    lv_obj_set_style_img_recolor_opa(tile_img_faucet, 255, 0);
    lv_obj_align(tile_img_faucet, LV_ALIGN_CENTER, 105, -100);
    lv_obj_add_flag(tile_img_faucet, LV_OBJ_FLAG_HIDDEN);

    /* DHW drop is RED so it's instantly distinguishable from the blue
     * cold-water-flow drop on the left. Silver faucet + red drop = "tap
     * is delivering hot water from the boiler"; lone blue drop on the
     * other side = "cold water flowing through the meter". */
    tile_img_drop = lv_img_create(th);
    lv_img_set_src(tile_img_drop, &icon_drop);
    lv_img_set_zoom(tile_img_drop, 256);
    lv_obj_set_style_img_recolor(tile_img_drop, lv_color_hex(0xff5544), 0);
    lv_obj_set_style_img_recolor_opa(tile_img_drop, 255, 0);
    lv_obj_align(tile_img_drop, LV_ALIGN_CENTER, 125, -80);
    lv_obj_add_flag(tile_img_drop, LV_OBJ_FLAG_HIDDEN);

    /* Live water-flow indicator on the left side of the indoor temp —
     * mirrors the radiator+flame on the right. The 48-pt temp spans
     * ~150 px, so its left edge sits around x = -75. Icon at -180 and
     * 14-pt label at -135 keep the indicator clear of the temp glyphs. */
    tile_img_water = lv_img_create(th);
    lv_img_set_src(tile_img_water, &icon_drop);
    lv_img_set_zoom(tile_img_water, 256);
    lv_obj_set_style_img_recolor(tile_img_water, lv_color_hex(0x66bbff), 0);
    lv_obj_set_style_img_recolor_opa(tile_img_water, 255, 0);
    lv_obj_align(tile_img_water, LV_ALIGN_CENTER, -180, -90);
    lv_obj_add_flag(tile_img_water, LV_OBJ_FLAG_HIDDEN);

    tile_lbl_water = lv_label_create(th);
    lv_obj_set_style_text_color(tile_lbl_water, lv_color_hex(0x66bbff), 0);
    lv_obj_set_style_text_font(tile_lbl_water, &lv_font_montserrat_14, 0);
    lv_label_set_text(tile_lbl_water, "");
    lv_obj_align(tile_lbl_water, LV_ALIGN_CENTER, -140, -90);
    lv_obj_add_flag(tile_lbl_water, LV_OBJ_FLAG_HIDDEN);

    /* --- Waste tile: two stacked pickup rows ---
       Each row gets its own type-specific icon (newspaper for Papier,
       milk carton for Plastic/PMD, leaf for GFT, trashcan fallback) so
       the user can read at a glance what's coming when. */
    tile_t waste_big;
    make_tile(scr_root, 560, 20, 220, 200, "Waste", 0x88dd66,
              open_placeholder, &waste_big);
    tile_waste = waste_big.tile;

    waste_icon_1 = lv_img_create(waste_big.tile);
    lv_img_set_src(waste_icon_1, &icon_trash);
    lv_obj_set_style_img_recolor_opa(waste_icon_1, 255, 0);
    /* Below the make_tile() title (font_18 at y=14 → ends at ~y=36).
     * Was 26, which overlapped the "Waste" header glyphs. */
    lv_obj_align(waste_icon_1, LV_ALIGN_TOP_LEFT, 4, 42);
    lv_obj_add_flag(waste_icon_1, LV_OBJ_FLAG_EVENT_BUBBLE);

    lbl_waste_date = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_date, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_waste_date, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_waste_date, "--");
    lv_obj_align(lbl_waste_date, LV_ALIGN_TOP_LEFT, 72, 44);

    lbl_waste_type = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_type, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_waste_type, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_waste_type, "");
    lv_obj_align(lbl_waste_type, LV_ALIGN_TOP_LEFT, 72, 78);

    /* Second-pickup icon renders at native size — lv_img_set_zoom on an
       alpha-8 source skips the recolor pass in LVGL 8.3 so the icon
       comes out invisible. Better to live with one consistent size. */
    waste_icon_2 = lv_img_create(waste_big.tile);
    lv_img_set_src(waste_icon_2, &icon_trash);
    lv_obj_set_style_img_recolor_opa(waste_icon_2, 255, 0);
    lv_obj_align(waste_icon_2, LV_ALIGN_TOP_LEFT, 4, 128);
    lv_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);

    lbl_waste_date_2 = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_date_2, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_waste_date_2, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_waste_date_2, "");
    lv_obj_align(lbl_waste_date_2, LV_ALIGN_TOP_LEFT, 72, 130);

    lbl_waste_type_2 = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_type_2, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_waste_type_2, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_waste_type_2, "");
    lv_obj_align(lbl_waste_type_2, LV_ALIGN_TOP_LEFT, 72, 164);

    /* Live Energy tile (replaces the old Humidity tile — humidity is now
       on the Heater bottom strip). Big live power on top, gas total below,
       today's kWh in the corner. */
    tile_t energy_t;
    make_tile(scr_root, 790, 20, 214, 130, "Energy", 0xaa77ff,
              open_stats, &energy_t);
    tile_energy = energy_t.tile;
    /* Compressed for the new 130-px tile: 28-pt W value (was 48), gas
     * total on the same baseline range below. */
    lbl_energy_w = lv_label_create(energy_t.tile);
    lv_obj_set_style_text_color(lbl_energy_w, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_energy_w, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_energy_w, "-- W");
    /* +6 sits the big number just below the tile centre — clears the
     * "Energy" header more cleanly than the prior -8. */
    lv_obj_align(lbl_energy_w, LV_ALIGN_CENTER, 0, 6);

    lbl_energy_gas = lv_label_create(energy_t.tile);
    lv_obj_set_style_text_color(lbl_energy_gas, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_energy_gas, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_energy_gas, "-- m3 gas");
    lv_obj_align(lbl_energy_gas, LV_ALIGN_BOTTOM_LEFT, 0, -4);

    lbl_energy_today = lv_label_create(energy_t.tile);
    lv_obj_set_style_text_color(lbl_energy_today, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_energy_today, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_energy_today, "");
    lv_obj_align(lbl_energy_today, LV_ALIGN_TOP_RIGHT, -4, 4);

    /* Vent tile — spinning fan in centre, manual +/- on the sides, live
       % above and rpm below the fan. Tap on the fan itself opens the remote
       (the buttons get their own click handlers so they don't bubble). */
    tile_t vent;
    make_tile(scr_root, 560, 230, 220, 200, "Vent", 0x66bbdd,
              (lv_event_cb_t)open_vent, &vent);
    tile_vent = vent.tile;

    /* Speed % shares the title row with the "Vent" label so the
       top-row buttons can sit cleanly below without overlap. */
    lbl_boiler_state = lv_label_create(vent.tile);
    lv_obj_set_style_text_color(lbl_boiler_state, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_boiler_state, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_boiler_state, "-- %");
    lv_obj_align(lbl_boiler_state, LV_ALIGN_TOP_RIGHT, -4, 14);

    /* Spinning fan: 80x80 TRUE_COLOR_ALPHA icon (color baked in). Rotated
       directly with lv_img_set_angle — works reliably on this format.
       Scaled to ~50px (zoom 160) and parked dead-centre so it has clear
       space below it for the 2-line % / rpm / source label without any
       overlap on either side. */
    vent_fan_img = lv_img_create(vent.tile);
    lv_img_set_src(vent_fan_img, &icon_fan);
    lv_img_set_pivot(vent_fan_img, 40, 40);
    lv_img_set_zoom(vent_fan_img, 160);            /* 160/256 ≈ 62 % */
    lv_obj_align(vent_fan_img, LV_ALIGN_CENTER, 0, -6);
    lv_obj_add_flag(vent_fan_img, LV_OBJ_FLAG_EVENT_BUBBLE);  /* tap → tile */

    /* Four corner buttons — Low / High at top, Auto / Timer at bottom.
       Each issues an Itho-Wifi virtual-remote command (vremotecmd=…).
       Timer cycles 10/20/30 min and resets to "auto". */
    /* Buttons send user-intent commands ("low"/"high"). ventilation.c
     * applies the wire-side swap (VENT_SWAP_LOW_HIGH) and translates
     * FanInfo back, so UI code deals only in intuitive labels. */
    struct { lv_align_t a; int x, y; uint32_t col; const char * cmd;
             const char * txt; lv_event_cb_t cb; } btn[] = {
        { LV_ALIGN_TOP_LEFT,     4,  46, 0x224d70, "low",  "Low",   on_vent_mode  },
        { LV_ALIGN_TOP_RIGHT,   -4,  46, 0x804030, "high", "High",  on_vent_mode  },
        { LV_ALIGN_BOTTOM_LEFT,  4,  -4, 0x2e6e3a, "auto", "Auto",  on_vent_mode  },
        { LV_ALIGN_BOTTOM_RIGHT,-4,  -4, 0x6a5424, NULL,   "Timer", on_vent_timer },
    };
    for (size_t i = 0; i < sizeof(btn)/sizeof(btn[0]); i++) {
        lv_obj_t * b = lv_btn_create(vent.tile);
        lv_obj_set_size(b, 60, 28);
        lv_obj_align(b, btn[i].a, btn[i].x, btn[i].y);
        lv_obj_set_style_bg_color(b, lv_color_hex(btn[i].col), 0);
        lv_obj_set_style_radius(b, 8, 0);
        /* Pre-style border to 0 so the refresh-side toggle is a single
         * width change (color always white when shown). */
        lv_obj_set_style_border_color(b, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_add_event_cb(b, btn[i].cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(uintptr_t)btn[i].cmd);
        lv_obj_t * bl = lv_label_create(b);
        lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
        lv_label_set_text(bl, btn[i].txt);
        lv_obj_center(bl);
        if (btn[i].cb == on_vent_timer) vent_timer_lbl = bl;
        /* Refs keyed by the visible label, not the cmd — the two are inverted
         * for low/high on this Itho. */
        if      (strcmp(btn[i].txt, "Low")   == 0) vent_btn_low   = b;
        else if (strcmp(btn[i].txt, "High")  == 0) vent_btn_high  = b;
        else if (strcmp(btn[i].txt, "Auto")  == 0) vent_btn_auto  = b;
        else if (btn[i].cb == on_vent_timer)       vent_btn_timer = b;
    }

    /* % + rpm + source on two lines just above the Auto/Timer buttons.
       Two lines so the per-line width stays narrow and the text doesn't
       have to overlap the fan icon. Brighter colour than the old DIM grey
       so it's actually readable against the navy tile. */
    lbl_boiler_pressure = lv_label_create(vent.tile);
    lv_obj_set_style_text_color(lbl_boiler_pressure, lv_color_hex(0xbbd6e8), 0);
    lv_obj_set_style_text_font(lbl_boiler_pressure, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(lbl_boiler_pressure, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_boiler_pressure, "--\n--");
    lv_obj_align(lbl_boiler_pressure, LV_ALIGN_BOTTOM_MID, 0, -38);

    /* Family tile — Life360 locations for Ronald + Caja. Sits between
     * the shrunken Energy and Water tiles in the right column. */
    tile_t family_t;
    make_tile(scr_root, 790, 160, 214, 130, "Family", 0xff8866,
              open_placeholder, &family_t);
    tile_family = family_t.tile;
    /* Two scrolling labels — the formatted address ("City > Street > Num")
     * almost always exceeds the 194-px tile width, so we use
     * SCROLL_CIRCULAR to slide the full string through the label area
     * with a short gap between repeats. Looks tidier than chopping with
     * an ellipsis and the user can read the entire address without
     * tapping into a detail view. */
    /* Person 1 — blue (matches dim screen's first row colour). The two
     * scrolling labels are coloured rather than name-prefixed; the colour
     * carries the identity so the scroll has full width for the address. */
    lbl_life360_ronald = lv_label_create(family_t.tile);
    lv_obj_set_style_text_color(lbl_life360_ronald, lv_color_hex(0x88aaff), 0);
    lv_obj_set_style_text_font(lbl_life360_ronald, &lv_font_montserrat_18, 0);
    lv_obj_set_width(lbl_life360_ronald, 194);
    lv_label_set_long_mode(lbl_life360_ronald, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(lbl_life360_ronald, "?");
    lv_obj_align(lbl_life360_ronald, LV_ALIGN_TOP_LEFT, 0, 44);

    /* Person 2 — pink (mirrors dim screen). */
    lbl_life360_caja = lv_label_create(family_t.tile);
    lv_obj_set_style_text_color(lbl_life360_caja, lv_color_hex(0xff88cc), 0);
    lv_obj_set_style_text_font(lbl_life360_caja, &lv_font_montserrat_18, 0);
    lv_obj_set_width(lbl_life360_caja, 194);
    lv_label_set_long_mode(lbl_life360_caja, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(lbl_life360_caja, "?");
    lv_obj_align(lbl_life360_caja, LV_ALIGN_TOP_LEFT, 0, 76);

    tile_t water_t;
    make_tile(scr_root, 790, 300, 214, 130, "Water", 0x44aaff, open_placeholder, &water_t);
    tile_water = water_t.tile;

    /* Long-press on any of the four right-column tiles → tile-slots picker.
     * Same modal as Settings → Tiles. Cheap to attach; the handler only
     * fires when LVGL's long-press timer expires (default 400 ms), so a
     * normal tap still routes through the existing CLICKED handler. */
    lv_obj_add_event_cb(tile_energy, on_tile_longpress, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(tile_family, on_tile_longpress, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(tile_vent,   on_tile_longpress, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(tile_water,  on_tile_longpress, LV_EVENT_LONG_PRESSED, NULL);
    lbl_inbox_main = lv_label_create(water_t.tile);
    lv_obj_set_style_text_color(lbl_inbox_main, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_inbox_main, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_inbox_main, "-- m3");
    /* +6 matches the Energy tile so the two big numbers line up vertically. */
    lv_obj_align(lbl_inbox_main, LV_ALIGN_CENTER, 0, 6);
    lbl_inbox_sub = lv_label_create(water_t.tile);
    lv_obj_set_style_text_color(lbl_inbox_sub, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_inbox_sub, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_inbox_sub, "-- L/min");
    lv_obj_align(lbl_inbox_sub, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* Spinner overlay: visible only while water is flowing. lv_spinner draws
       a continuously-rotating arc that reads as "something is moving". */
    water_spinner = lv_spinner_create(water_t.tile, 1200, 80);
    lv_obj_set_size(water_spinner, 48, 48);
    lv_obj_align(water_spinner, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_set_style_arc_color(water_spinner, lv_color_hex(0x223344), LV_PART_MAIN);
    lv_obj_set_style_arc_color(water_spinner, lv_color_hex(0x44aaff), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(water_spinner, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(water_spinner, 6, LV_PART_INDICATOR);
    lv_obj_add_flag(water_spinner, LV_OBJ_FLAG_HIDDEN);

    /* ---- Page-1 tile overlay (revealed by swiping the tile zone left) ----
     * Four configurable slots; tapping one opens the tile-slots picker so the
     * user can map any integration onto it. Sits on top of the tile zone and
     * is hidden until paged in. */
    home_page1 = lv_obj_create(scr_root);
    lv_obj_set_pos(home_page1, 556, 16);
    lv_obj_set_size(home_page1, 456, 418);
    lv_obj_set_style_bg_opa(home_page1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(home_page1, 0, 0);
    lv_obj_set_style_pad_all(home_page1, 0, 0);
    lv_obj_clear_flag(home_page1, LV_OBJ_FLAG_SCROLLABLE);
    {
        static const struct { int x, y; uint32_t c; } slots[4] = {
            {   4,   4, 0x88dd66 }, { 234,   4, 0xaa77ff },
            {   4, 214, 0x66bbdd }, { 234, 214, 0xff8866 },
        };
        for (int i = 0; i < 4; i++) {
            tile_t s;
            make_tile(home_page1, slots[i].x, slots[i].y, 214, 196,
                      "", slots[i].c, on_page1_slot, &s);
            p1_title[i] = s.title;   /* make_tile's title label, reused */
            p1_main[i] = lv_label_create(s.tile);
            lv_obj_set_style_text_color(p1_main[i], lv_color_hex(COL_TEXT_HI), 0);
            lv_obj_set_style_text_font(p1_main[i], &lv_font_montserrat_28, 0);
            lv_label_set_text(p1_main[i], LV_SYMBOL_PLUS "  Tap to assign");
            lv_obj_align(p1_main[i], LV_ALIGN_CENTER, 0, 0);
            p1_sub[i] = lv_label_create(s.tile);
            lv_obj_set_style_text_color(p1_sub[i], lv_color_hex(COL_TEXT_DIM), 0);
            lv_obj_set_style_text_font(p1_sub[i], &lv_font_montserrat_18, 0);
            lv_label_set_text(p1_sub[i], "");
            lv_obj_align(p1_sub[i], LV_ALIGN_BOTTOM_MID, 0, -4);
        }
    }

    /* Pagination dots, centred in the gap under the tile zone. */
    for (int i = 0; i < 2; i++) {
        home_dot[i] = lv_obj_create(scr_root);
        lv_obj_set_size(home_dot[i], 10, 10);
        lv_obj_set_pos(home_dot[i], 966 + i * 18, 461);   /* on the location row, right — off the ticker */
        lv_obj_set_style_radius(home_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(home_dot[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_border_width(home_dot[i], 0, 0);
        lv_obj_clear_flag(home_dot[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    home_show_page(0);

    /* --- Curtains tile — slim strip below the (shrunk) thermostat tile.
           Talks to Home Assistant via REST. Shows the group state +
           battery; provides Open / Stop / Close buttons. --- */
    lv_obj_t * curt_tile = lv_obj_create(scr_root);
    tile_curtains = curt_tile;
    lv_obj_set_size(curt_tile, 520, 44);
    lv_obj_set_pos(curt_tile, 20, 386);
    lv_obj_set_style_bg_color(curt_tile, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_border_width(curt_tile, 0, 0);
    lv_obj_set_style_radius(curt_tile, 12, 0);
    lv_obj_set_style_pad_all(curt_tile, 6, 0);
    lv_obj_clear_flag(curt_tile, LV_OBJ_FLAG_SCROLLABLE);

    /* Left strip: title + live state ("Gordijnen — open / 99%  bat 89%"). */
    lbl_curtain_state = lv_label_create(curt_tile);
    lv_obj_set_style_text_color(lbl_curtain_state, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_curtain_state, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_curtain_state, "Gordijnen  --");
    lv_obj_align(lbl_curtain_state, LV_ALIGN_TOP_LEFT, 8, 2);

    /* Position bar 0..100 sits underneath the label so the user can see
       at a glance how open the curtain is. Same teal as the existing
       curtain-related elements. */
    curt_bar = lv_bar_create(curt_tile);
    lv_obj_set_size(curt_bar, 240, 6);
    lv_obj_align(curt_bar, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_bar_set_range(curt_bar, 0, 100);
    lv_bar_set_value(curt_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(curt_bar, lv_color_hex(0x223344), LV_PART_MAIN);
    lv_obj_set_style_bg_color(curt_bar, lv_color_hex(0x44aaff), LV_PART_INDICATOR);

    /* Tiny spinner overlay — visible only while the curtain is mid-move.
       lv_spinner is the same widget the water tile uses; it's an arc
       that rotates continuously. */
    curt_spinner = lv_spinner_create(curt_tile, 1200, 80);
    lv_obj_set_size(curt_spinner, 28, 28);
    lv_obj_align(curt_spinner, LV_ALIGN_LEFT_MID, 254, 0);
    lv_obj_set_style_arc_color(curt_spinner, lv_color_hex(0x223344), LV_PART_MAIN);
    lv_obj_set_style_arc_color(curt_spinner, lv_color_hex(0x44aaff),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(curt_spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(curt_spinner, 4, LV_PART_INDICATOR);
    lv_obj_add_flag(curt_spinner, LV_OBJ_FLAG_HIDDEN);

    /* Three buttons on the right: Open / Stop / Close. Narrower than the
       default tile-button width so the label on the left has room for the
       full state + battery readout. */
    struct { const char * txt; uint32_t col; lv_event_cb_t cb; int x; } cbtn[] = {
        { "Open",  0x2e6e3a, on_curt_open,  -156 },
        { "Stop",  0x6a5424, on_curt_stop,   -82 },
        { "Close", 0x6e3a3a, on_curt_close,   -8 },
    };
    for (size_t i = 0; i < sizeof(cbtn)/sizeof(cbtn[0]); i++) {
        lv_obj_t * b = lv_btn_create(curt_tile);
        lv_obj_set_size(b, 70, 32);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, cbtn[i].x, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(cbtn[i].col), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_add_event_cb(b, cbtn[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t * bl = lv_label_create(b);
        lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
        lv_label_set_text(bl, cbtn[i].txt);
        lv_obj_center(bl);
    }

    /* City header above the forecast — labels the strip and shows the
     * current outside temperature so the user immediately sees where the
     * data is coming from. Refreshed from settings.weather_location and
     * weather_state.current_temp by refresh_cb. */
    lbl_forecast_city = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_forecast_city, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_forecast_city, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_forecast_city, settings.weather_location);
    /* Location sits BELOW the ticker, as the forecast's header line. */
    lv_obj_align(lbl_forecast_city, LV_ALIGN_TOP_LEFT, 22, 457);

    /* News ticker — full-width strip at the top of the bottom band, above the
     * location + forecast. Tap opens the headline list + QR. */
    news_ticker = lv_label_create(scr_root);
    lv_label_set_long_mode(news_ticker, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(news_ticker, 1000);
    lv_obj_align(news_ticker, LV_ALIGN_TOP_LEFT, 12, 432);
    lv_obj_set_style_text_color(news_ticker, lv_color_hex(0xcfe0f0), 0);
    lv_obj_set_style_text_font(news_ticker, &lv_font_montserrat_18, 0);
    lv_label_set_text(news_ticker, "");
    lv_obj_add_flag(news_ticker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(news_ticker, 12);
    lv_obj_add_event_cb(news_ticker, on_news_tap, LV_EVENT_CLICKED, NULL);
    if (!settings.news_enabled) lv_obj_add_flag(news_ticker, LV_OBJ_FLAG_HIDDEN);

    /* --- Forecast band — fills the area below the upper-row tiles.
           5 day columns; each shows day label + min/max temp + a big
           weather icon. Shifted down 14 px to make room for the city
           header above; height shrunk to match so the bottom edge stays
           flush with the screen. --- */
    forecast_box = lv_obj_create(scr_root);
    lv_obj_set_size(forecast_box, 1004, 116);
    lv_obj_set_pos(forecast_box, 10, 480);
    lv_obj_set_style_bg_color(forecast_box, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_radius(forecast_box, 12, 0);
    lv_obj_set_style_border_width(forecast_box, 0, 0);
    lv_obj_set_style_pad_all(forecast_box, 6, 0);
    lv_obj_clear_flag(forecast_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(forecast_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(forecast_box, open_forecast, LV_EVENT_CLICKED, NULL);
    {
        int col_w = 1004 / WEATHER_FORECAST_DAYS;
        for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
            lv_obj_t * col = lv_obj_create(forecast_box);
            lv_obj_set_size(col, col_w - 4, 104);   /* fit the shorter box (was 132 → wind clipped) */
            lv_obj_set_pos(col, i * col_w + 2, 0);
            lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(col, 0, 0);
            lv_obj_set_style_pad_all(col, 0, 0);
            lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);

            fc_day_lbl[i] = lv_label_create(col);
            lv_obj_set_style_text_color(fc_day_lbl[i], lv_color_hex(COL_TEXT_HI), 0);
            lv_obj_set_style_text_font(fc_day_lbl[i], &lv_font_montserrat_22, 0);
            lv_label_set_text(fc_day_lbl[i], "--");
            lv_obj_align(fc_day_lbl[i], LV_ALIGN_TOP_LEFT, 8, 4);

            fc_temp_lbl[i] = lv_label_create(col);
            lv_obj_set_style_text_color(fc_temp_lbl[i], lv_color_hex(COL_TEMP_YELLOW), 0);
            lv_obj_set_style_text_font(fc_temp_lbl[i], &lv_font_montserrat_22, 0);
            lv_label_set_text(fc_temp_lbl[i], "");
            lv_obj_align(fc_temp_lbl[i], LV_ALIGN_TOP_RIGHT, -8, 4);

            /* Big weather icon centered in the column, fills the room
               freed by the dropped description text. */
            fc_icon[i] = lv_img_create(col);
            lv_obj_set_style_img_recolor(fc_icon[i], lv_color_hex(0xc8d4e0), 0);
            lv_obj_set_style_img_recolor_opa(fc_icon[i], 255, 0);
            lv_img_set_src(fc_icon[i], weather_icon_for_lg("d"));   /* default cloud */
            lv_obj_align(fc_icon[i], LV_ALIGN_CENTER, 0, 8);
            lv_obj_add_flag(fc_icon[i], LV_OBJ_FLAG_EVENT_BUBBLE);

            /* Sun overlay for 'b'/'j' partly-cloudy codes — small,
             * top-left of the cloud so it reads as "sun peeking from
             * behind a cloud". Hidden by default; set_forecast_icon()
             * toggles visibility per refresh. */
            fc_icon_sun[i] = lv_img_create(col);
            lv_img_set_src(fc_icon_sun[i], weather_icon_for("a"));
            lv_img_set_zoom(fc_icon_sun[i], 180);   /* 70 % of cloud size */
            lv_obj_set_style_img_recolor(fc_icon_sun[i],
                                         lv_color_hex(0xffd24a), 0);
            lv_obj_set_style_img_recolor_opa(fc_icon_sun[i], 255, 0);
            lv_obj_align_to(fc_icon_sun[i], fc_icon[i], LV_ALIGN_TOP_LEFT,
                            -8, -8);
            lv_obj_add_flag(fc_icon_sun[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(fc_icon_sun[i], LV_OBJ_FLAG_EVENT_BUBBLE);

            fc_wind_lbl[i] = lv_label_create(col);
            lv_obj_set_style_text_color(fc_wind_lbl[i], lv_color_hex(COL_TEXT_DIM), 0);
            lv_obj_set_style_text_font(fc_wind_lbl[i], &lv_font_montserrat_18, 0);
            lv_label_set_text(fc_wind_lbl[i], "");
            lv_obj_align(fc_wind_lbl[i], LV_ALIGN_BOTTOM_MID, 12, -4);

            /* Wind-direction arrow next to the wind label. Rotated per
               forecast via lv_img_set_angle. Hidden until a valid
               direction code arrives. */
            fc_wind_arrow[i] = lv_img_create(col);
            lv_img_set_src(fc_wind_arrow[i], &icon_wind_arrow);
            lv_img_set_pivot(fc_wind_arrow[i], 16, 16);
            lv_obj_align(fc_wind_arrow[i], LV_ALIGN_BOTTOM_LEFT, 8, -4);
            lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);

            /* fc_desc_lbl is gone but the refresh-cb still touches it via
               its array slot; NULL so the writer no-ops. */
            fc_desc_lbl[i] = NULL;
        }
    }

    /* Bottom row removed entirely — the live W + cumulative gas now live
       on the top-right Energy tile. Null the refs so legacy refresh
       writers no-op cleanly. */
    lbl_bot_energy   = NULL;
    lbl_outside_main = NULL;
    lbl_outside_sub  = NULL;
    lbl_bot_waste    = NULL;
    lbl_bot_weather  = NULL;

    /* (Old narrow bottom-row Waste tile + its trash icon removed — the
       full-size Waste tile now lives in the right column.) */

    /* Settings + Inbox icons are placed at the absolute screen top-right
       (not inside the big tile) per the user's spec. They float in the
       gap above the Humidity tile's accent bar — overlapping just the
       accent line, never the humidity value. */
    /* Update-available banner — sits along the top edge, between the
     * left-side schedule controls and the right-side buttons. Hidden
     * by default; refresh_cb shows it when g_update_state.available
     * flips on. Tap opens a modal with release notes + install hint. */
    update_banner = lv_btn_create(scr_root);
    lv_obj_set_size(update_banner, 380, 36);
    lv_obj_align(update_banner, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_color(update_banner, lv_color_hex(0x2e6e3a), 0);
    lv_obj_set_style_radius(update_banner, 8, 0);
    lv_obj_set_ext_click_area(update_banner, 8);
    lv_obj_add_event_cb(update_banner, on_update_banner_click,
                        LV_EVENT_CLICKED, NULL);
    update_banner_lbl = lv_label_create(update_banner);
    lv_obj_set_style_text_color(update_banner_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(update_banner_lbl, &lv_font_montserrat_18, 0);
    lv_label_set_text(update_banner_lbl, "Update available");
    lv_obj_center(update_banner_lbl);
    lv_obj_add_flag(update_banner, LV_OBJ_FLAG_HIDDEN);

    /* Minimized-update envelope — shown (green) when the user dismisses the
     * update banner; tap re-opens the update modal. Sits left of the inbox
     * envelope. Hidden until dismissed. */
    update_env_btn = lv_btn_create(scr_root);
    lv_obj_set_size(update_env_btn, 44, 44);
    lv_obj_align(update_env_btn, LV_ALIGN_TOP_RIGHT, -118, 4);
    lv_obj_set_style_bg_color(update_env_btn, lv_color_hex(0x2e6e3a), 0);
    lv_obj_set_style_radius(update_env_btn, 22, 0);
    lv_obj_set_ext_click_area(update_env_btn, 14);
    lv_obj_add_event_cb(update_env_btn, open_about_modal, LV_EVENT_CLICKED, NULL);
    lv_obj_t * uenv = lv_label_create(update_env_btn);
    lv_label_set_text(uenv, LV_SYMBOL_ENVELOPE);
    lv_obj_set_style_text_color(uenv, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(uenv, &lv_font_montserrat_18, 0);
    lv_obj_center(uenv);
    lv_obj_add_flag(update_env_btn, LV_OBJ_FLAG_HIDDEN);

    envelope_btn = lv_btn_create(scr_root);
    lv_obj_set_size(envelope_btn, 44, 44);
    lv_obj_align(envelope_btn, LV_ALIGN_TOP_RIGHT, -66, 4);
    lv_obj_set_style_bg_color(envelope_btn, lv_color_hex(0x335577), 0);
    lv_obj_set_style_radius(envelope_btn, 22, 0);
    lv_obj_set_ext_click_area(envelope_btn, 14);
    lv_obj_add_event_cb(envelope_btn, open_inbox, LV_EVENT_CLICKED, NULL);
    lv_obj_t * env_lbl = lv_label_create(envelope_btn);
    lv_label_set_text(env_lbl, LV_SYMBOL_ENVELOPE);
    lv_obj_set_style_text_color(env_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(env_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(env_lbl);

    envelope_badge = lv_obj_create(scr_root);
    lv_obj_set_size(envelope_badge, 20, 20);
    lv_obj_align(envelope_badge, LV_ALIGN_TOP_RIGHT, -60, 2);
    lv_obj_set_style_bg_color(envelope_badge, lv_color_hex(0xff3344), 0);
    lv_obj_set_style_radius(envelope_badge, 10, 0);
    lv_obj_set_style_border_width(envelope_badge, 2, 0);
    lv_obj_set_style_border_color(envelope_badge, lv_color_hex(0x0f1a2a), 0);
    lv_obj_set_style_pad_all(envelope_badge, 0, 0);
    lv_obj_clear_flag(envelope_badge, LV_OBJ_FLAG_SCROLLABLE);
    envelope_badge_lbl = lv_label_create(envelope_badge);
    lv_obj_set_style_text_color(envelope_badge_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(envelope_badge_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(envelope_badge_lbl, "0");
    lv_obj_center(envelope_badge_lbl);
    lv_obj_add_flag(envelope_btn,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envelope_badge, LV_OBJ_FLAG_HIDDEN);

    /* (Old top-right lights button removed — superseded by the left-edge
     * collapsible lights handle and the swipe-right gesture.) */

    /* freetoon logo badge — top-left corner, above the clock/date. Tap →
     * About / version / updates modal. */
    {
        lv_obj_t * logo = lv_btn_create(scr_root);
        lv_obj_set_size(logo, 40, 40);
        lv_obj_align(logo, LV_ALIGN_TOP_LEFT, 8, 6);
        lv_obj_set_style_bg_color(logo, lv_color_hex(0x2e6e9e), 0);
        lv_obj_set_style_bg_color(logo, lv_color_hex(0x3a86bf), LV_STATE_PRESSED);
        lv_obj_set_style_radius(logo, 12, 0);
        lv_obj_set_ext_click_area(logo, 14);
        lv_obj_add_event_cb(logo, open_about_modal, LV_EVENT_CLICKED, NULL);
        lv_obj_t * logo_lbl = lv_label_create(logo);
        lv_label_set_text(logo_lbl, "ft");
        lv_obj_set_style_text_color(logo_lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(logo_lbl, &lv_font_montserrat_22, 0);
        lv_obj_center(logo_lbl);
    }

    /* Lights handle — a slim half-circle tab on the left edge that auto-hides.
     * Touch it and it expands into a horizontal "Lights" button; releasing
     * opens the lights backend (Domoticz if enabled, else Home Assistant).
     * Same target as the swipe-right gesture, but discoverable. */
    {
        lights_handle = lv_btn_create(scr_root);
        lv_obj_set_size(lights_handle, 22, 64);
        lv_obj_align(lights_handle, LV_ALIGN_LEFT_MID, -10, 0);
        lv_obj_set_style_bg_color(lights_handle, lv_color_hex(0x2a4060), 0);
        lv_obj_set_style_bg_color(lights_handle, lv_color_hex(0x3a5688), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(lights_handle, LV_OPA_70, 0);
        lv_obj_set_style_radius(lights_handle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(lights_handle, 0, 0);
        lv_obj_set_ext_click_area(lights_handle, 18);   /* easy to grab while slim */
        lv_obj_add_event_cb(lights_handle, on_lights_handle, LV_EVENT_PRESSED,    NULL);
        lv_obj_add_event_cb(lights_handle, on_lights_handle, LV_EVENT_RELEASED,   NULL);
        lv_obj_add_event_cb(lights_handle, on_lights_handle, LV_EVENT_PRESS_LOST, NULL);
        lv_obj_add_event_cb(lights_handle, on_lights_handle, LV_EVENT_CLICKED,    NULL);
        lights_handle_lbl = lv_label_create(lights_handle);
        lv_obj_set_style_text_color(lights_handle_lbl, lv_color_hex(0xffe08a), 0);
        lv_obj_set_style_text_font(lights_handle_lbl, &lv_font_montserrat_22, 0);
        lv_label_set_text(lights_handle_lbl, LV_SYMBOL_CHARGE " Lights");
        lv_obj_center(lights_handle_lbl);
        lv_obj_add_flag(lights_handle_lbl, LV_OBJ_FLAG_HIDDEN);   /* shown only while expanded */
    }

    /* Gear in the very top-right corner of the screen. */
    lv_obj_t * gear = lv_btn_create(scr_root);
    lv_obj_set_size(gear, 44, 44);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_bg_color(gear, lv_color_hex(0x223344), 0);
    lv_obj_set_style_radius(gear, 22, 0);
    lv_obj_set_ext_click_area(gear, 14);
    lv_obj_add_event_cb(gear, open_settings, LV_EVENT_CLICKED, NULL);
    lv_obj_t * gear_lbl = lv_label_create(gear);
    lv_label_set_text(gear_lbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gear_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(gear_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(gear_lbl);

    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 500, NULL);
    return scr_root;
}
