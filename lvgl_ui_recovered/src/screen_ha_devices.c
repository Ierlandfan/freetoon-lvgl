/*
 * screen_ha_devices.c — Settings → Devices manager (Home Assistant).
 *
 * Manage the dynamic device list (ha_devices[]): add a light/cover/switch/
 * script/scene (via the HA entity picker), toggle whether each shows as a
 * home quick-tile, or remove it. Edits persist to ha_devices.conf immediately.
 *
 * The list is rebuilt on every SCREEN_LOADED, so when the picker pops back
 * after adding a device the new row appears without any extra plumbing.
 */
#include "screens.h"
#include "display.h"
#include "homeassistant.h"
#include "domoticz.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define COL_BG       0x0e1a2a
#define COL_CARD     0x1a2940
#define COL_TEXT_HI  0xffffff
#define COL_TEXT_DIM 0x88aabb
#define COL_PIN_ON   0x2e6e3a
#define COL_PIN_OFF  0x3a4658
#define COL_REMOVE   0x6e3a3a

static lv_obj_t * scr_root = NULL;
static lv_obj_t * list     = NULL;
static lv_obj_t * empty_hint = NULL;
static lv_obj_t * addrow   = NULL;
/* Which add-buttons to offer: 0 = Home Assistant types, 1 = Domoticz only. The
 * device LIST is the same (all backends) either way — only the +add options
 * differ, so HA settings and Domoticz settings both open this one manager. */
static int        g_add_mode = 0;
void screen_ha_devices_set_add_mode(int dz) { g_add_mode = dz ? 1 : 0; }

static void back_async(void * u) { (void)u; ui_pop(); }
static void on_back(lv_event_t * e) { (void)e; lv_async_call(back_async, NULL); }

static void rebuild(void);

/* "+ <type>" add buttons → open the picker in add-mode for that domain. */
static void on_add(lv_event_t * e) {
    int type = (int)(intptr_t)lv_event_get_user_data(e);
    screen_ha_picker_open_add(hadev_type_str(type), type);
}
static void on_pin(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < ha_device_count) {
        ha_device_set_pin(i, !ha_devices[i].pin_home);
        rebuild();
    }
}
static void on_remove(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < ha_device_count) { ha_device_remove(i); rebuild(); }
}

static lv_obj_t * small_btn(lv_obj_t * parent, const char * txt, uint32_t col,
                            int w, int h, lv_event_cb_t cb, int data) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)data);
    lv_obj_t * l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, SF(14), 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

/* ---- Domoticz device chooser (adds a Domoticz device to the managed list,
 * the same way the HA picker adds an HA entity). Filter chips by kind keep a
 * big install (100s of devices) navigable; render is capped. ---- */
static lv_obj_t * dz_modal = NULL;
static lv_obj_t * dz_list  = NULL;
static int        dz_filter = -1;      /* -1 = all, else DZ_* */
#define DZ_CHOOSER_MAX 100

static void dz_modal_close(void) {
    if (dz_modal) { lv_obj_del(dz_modal); dz_modal = NULL; dz_list = NULL; }
}
static void on_dz_close(lv_event_t * e) { (void)e; dz_modal_close(); }
static void on_dz_pick(lv_event_t * e) {
    int j = (int)(intptr_t)lv_event_get_user_data(e);
    if (j >= 0 && j < domoticz_state.count) {
        domoticz_dev_t * z = &domoticz_state.dev[j];
        ha_device_add_dz(z->kind, z->idx, z->name, 0);
    }
    dz_modal_close();
    rebuild();
}
static const char * dz_kind_name(int kind) {
    return kind == DZ_BLIND ? "blind" : kind == DZ_DIMMER ? "dimmer"
         : kind == DZ_SELECTOR ? "selector" : "switch";
}
static void populate_dz_list(void) {
    if (!dz_list) return;
    lv_obj_clean(dz_list);
    if (domoticz_state.count == 0) {
        lv_list_add_text(dz_list, settings.enable_domoticz
            ? "No Domoticz devices yet. Set the host in Settings -> Domoticz."
            : "Enable Domoticz first (Settings -> Domoticz).");
        return;
    }
    int shown = 0, matched = 0;
    for (int j = 0; j < domoticz_state.count; j++) {
        domoticz_dev_t * z = &domoticz_state.dev[j];
        if (dz_filter >= 0 && z->kind != dz_filter) continue;
        matched++;
        if (shown >= DZ_CHOOSER_MAX) continue;
        char label[96];
        snprintf(label, sizeof label, "%s   (%s #%d)", z->name, dz_kind_name(z->kind), z->idx);
        lv_obj_t * b = lv_list_add_btn(dz_list, NULL, label);
        lv_obj_add_event_cb(b, on_dz_pick, LV_EVENT_CLICKED, (void *)(intptr_t)j);
        shown++;
    }
    if (matched == 0)        lv_list_add_text(dz_list, "No devices of this type.");
    else if (matched > shown) {
        char m[64]; snprintf(m, sizeof m, "+%d more — narrow with a type filter", matched - shown);
        lv_list_add_text(dz_list, m);
    }
}
static void on_dz_chip(lv_event_t * e) {
    dz_filter = (int)(intptr_t)lv_event_get_user_data(e);
    populate_dz_list();
}
static void open_dz_chooser(lv_event_t * e) {
    (void)e;
    ha_devices_sync_dz();
    dz_filter = -1;
    dz_modal_close();
    dz_modal = lv_obj_create(scr_root);
    lv_obj_set_size(dz_modal, DISP_HOR, DISP_VER);
    lv_obj_align(dz_modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(dz_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dz_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(dz_modal, 0, 0);
    lv_obj_clear_flag(dz_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * panel = lv_obj_create(dz_modal);
    lv_obj_set_size(panel, SX(700), SY(450));
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x14233a), 0);
    lv_obj_set_style_radius(panel, 14, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);   /* explicit, so the TOP-anchored rows below line up */
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * t = lv_label_create(panel);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(t, SF(24), 0);
    lv_label_set_text(t, "Add Domoticz device");
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_t * x = small_btn(panel, LV_SYMBOL_CLOSE, 0x3a4658, 60, 40, on_dz_close, 0);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 0, 0);

    /* Kind filter chips. */
    static const struct { int f; const char * t; } chips[] = {
        { -1, "All" }, { DZ_SWITCH, "Switch" }, { DZ_DIMMER, "Dimmer" },
        { DZ_SELECTOR, "Selector" }, { DZ_BLIND, "Blind" },
    };
    int cx = 4;
    for (unsigned i = 0; i < sizeof(chips)/sizeof(chips[0]); i++) {
        lv_obj_t * c = small_btn(panel, chips[i].t, 0x2a4060, SX(124), SY(38), on_dz_chip, chips[i].f);
        lv_obj_align(c, LV_ALIGN_TOP_LEFT, SX(cx), SY(52));
        cx += 130;
    }

    /* List sits below the chip row (top-anchored so it never rides over them). */
    dz_list = lv_list_create(panel);
    lv_obj_set_size(dz_list, SX(676), SY(330));
    lv_obj_align(dz_list, LV_ALIGN_TOP_LEFT, 0, SY(98));
    lv_obj_set_style_bg_opa(dz_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dz_list, 0, 0);
    lv_obj_set_style_pad_all(dz_list, 0, 0);
    populate_dz_list();
}

static void rebuild(void) {
    if (!list) return;
    lv_obj_clean(list);

    int empty = (ha_device_count == 0);
    if (empty_hint) {
        if (empty) lv_obj_clear_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < ha_device_count; i++) {
        ha_device_t * D = &ha_devices[i];
        lv_obj_t * c = lv_obj_create(list);
        lv_obj_set_size(c, lv_pct(100), 56);
        lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_radius(c, 12, 0);
        lv_obj_set_style_pad_all(c, 8, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * nm = lv_label_create(c);
        lv_obj_set_style_text_color(nm, lv_color_hex(COL_TEXT_HI), 0);
        lv_obj_set_style_text_font(nm, SF(18), 0);
        lv_label_set_text(nm, D->name);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 4, -8);

        lv_obj_t * tp = lv_label_create(c);
        lv_obj_set_style_text_color(tp, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(tp, SF(14), 0);
        if (D->backend == HADEV_BE_DZ)
            lv_label_set_text_fmt(tp, "%s  -  Domoticz #%d", hadev_type_str(D->type), D->dz_idx);
        else
            lv_label_set_text_fmt(tp, "%s  -  %s", hadev_type_str(D->type), D->entity_id);
        lv_obj_align(tp, LV_ALIGN_LEFT_MID, 4, 12);

        /* Home-pin toggle + remove. */
        lv_obj_t * pinb = small_btn(c, D->pin_home ? "Home " LV_SYMBOL_OK : "Home",
                                    D->pin_home ? COL_PIN_ON : COL_PIN_OFF,
                                    86, 36, on_pin, i);
        lv_obj_align(pinb, LV_ALIGN_RIGHT_MID, -76, 0);

        lv_obj_t * rmb = small_btn(c, LV_SYMBOL_TRASH, COL_REMOVE, 64, 36, on_remove, i);
        lv_obj_align(rmb, LV_ALIGN_RIGHT_MID, -2, 0);
    }
}

/* (Re)build the add-buttons for the current mode. HA mode: one button per HA
 * device type. Domoticz mode: a single "+ Domoticz" chooser button. */
static void build_addrow(void) {
    if (!addrow) return;
    lv_obj_clean(addrow);
    if (g_add_mode == 0) {
        const struct { int type; const char * txt; } adds[] = {
            { HADEV_LIGHT, "+ Light" }, { HADEV_COVER, "+ Cover" },
            { HADEV_SWITCH, "+ Switch" }, { HADEV_SELECT, "+ Select" },
            { HADEV_SCRIPT, "+ Script" }, { HADEV_SCENE, "+ Scene" },
        };
        for (size_t i = 0; i < sizeof(adds)/sizeof(adds[0]); i++)
            small_btn(addrow, adds[i].txt, 0x2e5e8a, SX(140), SY(40), on_add, adds[i].type);
    } else {
        small_btn(addrow, "+ Domoticz device", 0x2e7e5a, SX(240), SY(40), open_dz_chooser, 0);
    }
}

static void on_scr_event(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) { build_addrow(); rebuild(); }
}

lv_obj_t * screen_ha_devices_create(void) {
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_root, on_scr_event, LV_EVENT_SCREEN_LOADED, NULL);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, SF(28), 0);
    lv_label_set_text(title, "Devices");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 14);

    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, 110, 44);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -16, 10);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x3a4658), 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_ext_click_area(back, 18);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_label_set_text(bl, "< Back");
    lv_obj_center(bl);

    /* Add-buttons row: populated by build_addrow() per the active mode. */
    addrow = lv_obj_create(scr_root);
    lv_obj_set_size(addrow, DISP_HOR - 32, SY(48));
    lv_obj_align(addrow, LV_ALIGN_TOP_MID, 0, SY(58));
    lv_obj_set_style_bg_opa(addrow, 0, 0);
    lv_obj_set_style_border_width(addrow, 0, 0);
    lv_obj_set_style_pad_all(addrow, 0, 0);
    lv_obj_set_scroll_dir(addrow, LV_DIR_HOR);   /* type buttons scroll horizontally */
    lv_obj_set_scrollbar_mode(addrow, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(addrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(addrow, 6, 0);
    build_addrow();

    /* Scrollable device list. */
    list = lv_obj_create(scr_root);
    lv_obj_set_size(list, DISP_HOR - 32, DISP_VER - SY(118));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, SY(112));
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);

    empty_hint = lv_label_create(scr_root);
    lv_obj_set_style_text_color(empty_hint, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(empty_hint, SF(20), 0);
    lv_obj_set_width(empty_hint, DISP_HOR - 120);
    lv_label_set_long_mode(empty_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(empty_hint,
        "No devices yet. Use the buttons above to add a light, cover, switch,\n"
        "select, script or scene from Home Assistant, or + Domoticz for a\n"
        "Domoticz device. Both backends share this list.");
    lv_obj_align(empty_hint, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);

    rebuild();
    return scr_root;
}
