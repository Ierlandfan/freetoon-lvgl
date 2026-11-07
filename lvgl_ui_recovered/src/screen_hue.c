/*
 * Philips Hue screen — opened via the home left button / swipe-right from home
 * when Philips Hue is the active backend, or from Settings -> Philips Hue.
 * Lists lights from the local Philips Hue Bridge, provides on/off toggles,
 * brightness sliders, and "All off" / "All on" quick controls.
 */
#include "screens.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "theme.h"
#include "hue.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define COL_BG        THEME_BG
#define COL_CARD      THEME_CARD_BG
#define COL_TEXT_HI   THEME_TEXT_HI
#define COL_TEXT_DIM  THEME_TEXT_DIM
#define COL_ON        (settings.home_light_mode ? 0xd97706 : 0xffcc44)
#define COL_OFF       THEME_OFF
#define COL_OFFLINE   (settings.home_light_mode ? 0x94a3b8 : 0x6e6e6e)
#define COL_ACCENT    THEME_ACCENT_BLUE

typedef struct {
    char id[16];
    lv_obj_t * card;
    lv_obj_t * icon;
    lv_obj_t * lbl_state;
    lv_obj_t * btn_toggle;
    lv_obj_t * btn_lbl;
    lv_obj_t * slider;
    int        last_on;
    int        last_bri;
    int        last_reachable;
} hue_row_t;

static lv_obj_t   * scr_root      = NULL;
static lv_obj_t   * list_box      = NULL;
static lv_obj_t   * lbl_status    = NULL;
static lv_obj_t   * empty_hint    = NULL;
static lv_timer_t * refresh_timer = NULL;
static hue_row_t    rows[HUE_MAX_LIGHTS];
static int          row_count     = 0;
static int          g_built_count = -1;

static void back_async(void * unused) { (void)unused; ui_pop(); }
static void on_back_click(lv_event_t * e) {
    (void)e;
    lv_async_call(back_async, NULL);
}

static void on_toggle_click(lv_event_t * e) {
    const char * id = (const char *)lv_event_get_user_data(e);
    if (id) hue_light_toggle_async(id);
}

static void on_slider_change(lv_event_t * e) {
    const char * id = (const char *)lv_event_get_user_data(e);
    lv_obj_t * s = lv_event_get_target(e);
    if (id && s) {
        int v = lv_slider_get_value(s);
        hue_light_set_bri_async(id, v);
    }
}

static void on_all_off_click(lv_event_t * e) {
    (void)e;
    hue_all_off_async();
}

static void on_all_on_click(lv_event_t * e) {
    (void)e;
    hue_all_on_async();
}

static void update_row_display(hue_row_t * R, const hue_light_t * L) {
    R->last_on = L->on;
    R->last_bri = L->bri_pct;
    R->last_reachable = L->reachable;

    if (!L->reachable) {
        if (R->lbl_state) lv_label_set_text(R->lbl_state, tr("onbereikbaar", "unreachable"));
        if (R->icon) lv_obj_set_style_text_color(R->icon, lv_color_hex(COL_OFFLINE), 0);
        if (R->btn_toggle) {
            lv_obj_set_style_bg_color(R->btn_toggle, lv_color_hex(COL_OFFLINE), 0);
            if (R->btn_lbl) lv_label_set_text(R->btn_lbl, tr("OFFLINE", "OFFLINE"));
        }
    } else if (L->on) {
        char st[32];
        if (L->is_dimmable)
            snprintf(st, sizeof(st), "%s (%d%%)", tr("aan", "on"), L->bri_pct);
        else
            snprintf(st, sizeof(st), "%s", tr("aan", "on"));
        if (R->lbl_state) lv_label_set_text(R->lbl_state, st);
        if (R->icon) lv_obj_set_style_text_color(R->icon, lv_color_hex(COL_ON), 0);
        if (R->btn_toggle) {
            lv_obj_set_style_bg_color(R->btn_toggle, lv_color_hex(COL_ON), 0);
            if (R->btn_lbl) lv_label_set_text(R->btn_lbl, tr("AAN", "ON"));
        }
    } else {
        if (R->lbl_state) lv_label_set_text(R->lbl_state, tr("uit", "off"));
        if (R->icon) lv_obj_set_style_text_color(R->icon, lv_color_hex(COL_OFF), 0);
        if (R->btn_toggle) {
            lv_obj_set_style_bg_color(R->btn_toggle, lv_color_hex(COL_OFF), 0);
            if (R->btn_lbl) lv_label_set_text(R->btn_lbl, tr("UIT", "OFF"));
        }
    }

    if (R->slider && lv_slider_get_value(R->slider) != L->bri_pct) {
        lv_slider_set_value(R->slider, L->bri_pct, LV_ANIM_OFF);
    }
}

static void build_rows(void) {
    if (!list_box) return;
    lv_obj_clean(list_box);
    row_count = 0;

    int n = hue_state.count;
    g_built_count = n;

    if (n == 0) {
        if (empty_hint) lv_obj_clear_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (empty_hint) lv_obj_add_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < n && i < HUE_MAX_LIGHTS; i++) {
        hue_light_t * L = &hue_state.lights[i];
        hue_row_t * R = &rows[row_count++];
        memset(R, 0, sizeof(*R));
        snprintf(R->id, sizeof(R->id), "%s", L->id);

        int h = L->is_dimmable ? SY(92) : SY(72);
        lv_obj_t * card = lv_obj_create(list_box);
        R->card = card;
        lv_obj_set_size(card, SX(940), h);
        lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(THEME_BORDER), 0);
        lv_obj_set_style_border_width(card, settings.home_light_mode ? 1 : 0, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        /* Bulb icon */
        lv_obj_t * ic = lv_label_create(card);
        R->icon = ic;
        lv_label_set_text(ic, LV_SYMBOL_CHARGE);
        lv_obj_set_style_text_font(ic, SF(24), 0);
        lv_obj_align(ic, LV_ALIGN_TOP_LEFT, SX(8), SY(10));

        /* Name */
        lv_obj_t * nm = lv_label_create(card);
        lv_obj_set_style_text_font(nm, SF(20), 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(COL_TEXT_HI), 0);
        lv_label_set_text(nm, L->name);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, SX(46), SY(6));

        /* State text */
        lv_obj_t * st = lv_label_create(card);
        R->lbl_state = st;
        lv_obj_set_style_text_font(st, SF(15), 0);
        lv_obj_set_style_text_color(st, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_align(st, LV_ALIGN_TOP_LEFT, SX(46), SY(30));

        /* Toggle button */
        lv_obj_t * btn = lv_btn_create(card);
        R->btn_toggle = btn;
        lv_obj_set_size(btn, SX(130), SY(46));
        lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, SX(-8), SY(6));
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_add_event_cb(btn, on_toggle_click, LV_EVENT_CLICKED, (void *)R->id);

        lv_obj_t * bl = lv_label_create(btn);
        R->btn_lbl = bl;
        lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(bl, SF(18), 0);
        lv_obj_center(bl);

        /* Slider for dimmable lights */
        if (L->is_dimmable) {
            lv_obj_t * s = lv_slider_create(card);
            R->slider = s;
            lv_obj_set_size(s, SX(900), SY(14));
            lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, SY(-4));
            lv_slider_set_range(s, 0, 100);
            lv_slider_set_value(s, L->bri_pct, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(s, lv_color_hex(0x223344), LV_PART_MAIN);
            lv_obj_set_style_bg_color(s, lv_color_hex(COL_ON), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(s, lv_color_hex(0xffffff), LV_PART_KNOB);
            lv_obj_add_event_cb(s, on_slider_change, LV_EVENT_RELEASED, (void *)R->id);
        }

        update_row_display(R, L);
    }
}

static void refresh_cb(lv_timer_t * tmr) {
    (void)tmr;
    if (!scr_root || !list_box) return;

    /* Update header subtitle */
    if (lbl_status) {
        if (!settings.hue_enabled) {
            lv_label_set_text(lbl_status, tr("Philips Hue is uitgeschakeld in Instellingen.",
                                             "Philips Hue is disabled in Settings."));
        } else if (hue_state.connected) {
            lv_label_set_text_fmt(lbl_status, tr("Verbonden met Hue Bridge (%d lampen)",
                                                 "Connected to Hue Bridge (%d lights)"),
                                  hue_state.count);
        } else {
            lv_label_set_text(lbl_status, tr("Niet verbonden met Hue Bridge — controleer host/token",
                                             "Not connected to Hue Bridge — check host/token"));
        }
    }

    /* Rebuild rows if device count changed */
    if (hue_state.count != g_built_count) {
        build_rows();
        return;
    }

    /* Update existing rows */
    for (int i = 0; i < row_count; i++) {
        hue_row_t * R = &rows[i];
        for (int j = 0; j < hue_state.count; j++) {
            if (strcmp(hue_state.lights[j].id, R->id) == 0) {
                const hue_light_t * L = &hue_state.lights[j];
                if (L->on != R->last_on || L->bri_pct != R->last_bri ||
                    L->reachable != R->last_reachable) {
                    update_row_display(R, L);
                }
                break;
            }
        }
    }
}

static void on_scr_event(lv_event_t * e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_SCREEN_LOADED) {
        build_rows();
        if (!refresh_timer)
            refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    } else if (c == LV_EVENT_SCREEN_UNLOADED) {
        if (refresh_timer) {
            lv_timer_del(refresh_timer);
            refresh_timer = NULL;
        }
    }
}

lv_obj_t * screen_hue_create(void) {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button */
    lv_obj_t * btn_back = lv_btn_create(scr_root);
    lv_obj_set_size(btn_back, SX(88), SY(56));
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, SX(12), SY(10));
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(THEME_BTN_BG), 0);
    lv_obj_set_style_radius(btn_back, 10, 0);
    lv_obj_add_event_cb(btn_back, on_back_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t * b_lbl = lv_label_create(btn_back);
    lv_label_set_text(b_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(b_lbl, SF(22), 0);
    lv_obj_set_style_text_color(b_lbl, lv_color_hex(THEME_TEXT_HI), 0);
    lv_obj_center(b_lbl);

    /* Title */
    lv_obj_t * lbl_title = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_title, SF(24), 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(COL_TEXT_HI), 0);
    lv_label_set_text(lbl_title, "Philips Hue");
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, SX(112), SY(12));

    /* Status subtitle */
    lbl_status = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_status, SF(15), 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(lbl_status, tr("Verbinden met Hue Bridge...", "Connecting to Hue Bridge..."));
    lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, SX(114), SY(40));

    /* Quick action buttons: Alles aan / Alles uit */
    lv_obj_t * btn_all_on = lv_btn_create(scr_root);
    lv_obj_set_size(btn_all_on, SX(140), SY(48));
    lv_obj_align(btn_all_on, LV_ALIGN_TOP_RIGHT, SX(-160), SY(14));
    lv_obj_set_style_bg_color(btn_all_on, lv_color_hex(COL_ON), 0);
    lv_obj_set_style_radius(btn_all_on, 10, 0);
    lv_obj_add_event_cb(btn_all_on, on_all_on_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl_on = lv_label_create(btn_all_on);
    lv_label_set_text(lbl_on, tr("Alles aan", "All on"));
    lv_obj_set_style_text_font(lbl_on, SF(16), 0);
    lv_obj_set_style_text_color(lbl_on, lv_color_hex(0xffffff), 0);
    lv_obj_center(lbl_on);

    lv_obj_t * btn_all_off = lv_btn_create(scr_root);
    lv_obj_set_size(btn_all_off, SX(140), SY(48));
    lv_obj_align(btn_all_off, LV_ALIGN_TOP_RIGHT, SX(-12), SY(14));
    lv_obj_set_style_bg_color(btn_all_off, lv_color_hex(COL_OFF), 0);
    lv_obj_set_style_radius(btn_all_off, 10, 0);
    lv_obj_add_event_cb(btn_all_off, on_all_off_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl_off = lv_label_create(btn_all_off);
    lv_label_set_text(lbl_off, tr("Alles uit", "All off"));
    lv_obj_set_style_text_font(lbl_off, SF(16), 0);
    lv_obj_set_style_text_color(lbl_off, lv_color_hex(0xffffff), 0);
    lv_obj_center(lbl_off);

    /* Scrollable device list */
    list_box = lv_obj_create(scr_root);
    lv_obj_set_size(list_box, SX(980), SY(DISP_VER - 80));
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, SY(74));
    lv_obj_set_style_bg_opa(list_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_box, 0, 0);
    lv_obj_set_style_pad_all(list_box, 8, 0);
    lv_obj_set_style_pad_row(list_box, SY(10), 0);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Empty hint */
    empty_hint = lv_label_create(scr_root);
    lv_obj_set_style_text_font(empty_hint, SF(18), 0);
    lv_obj_set_style_text_color(empty_hint, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_align(empty_hint, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(empty_hint,
        settings.hue_enabled
            ? tr("Geen Hue lampen gevonden.\nControleer of de Hue Bridge verbonden is met het netwerk\nen gekoppeld is in Instellingen > Philips Hue.",
                 "No Hue lights found.\nCheck if Hue Bridge is connected to the network\nand paired in Settings > Philips Hue.")
            : tr("Philips Hue is uitgeschakeld.\nSchakel het in bij Instellingen > Philips Hue.",
                 "Philips Hue is disabled.\nEnable it in Settings > Philips Hue."));

    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_ALL, NULL);
    return scr_root;
}
