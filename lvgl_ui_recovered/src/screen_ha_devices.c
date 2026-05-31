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

static void on_scr_event(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) rebuild();
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

    /* Add-buttons row: one per device type. */
    lv_obj_t * addrow = lv_obj_create(scr_root);
    lv_obj_set_size(addrow, DISP_HOR - 32, SY(48));
    lv_obj_align(addrow, LV_ALIGN_TOP_MID, 0, SY(58));
    lv_obj_set_style_bg_opa(addrow, 0, 0);
    lv_obj_set_style_border_width(addrow, 0, 0);
    lv_obj_set_style_pad_all(addrow, 0, 0);
    lv_obj_clear_flag(addrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(addrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(addrow, 6, 0);
    const struct { int type; const char * txt; } adds[] = {
        { HADEV_LIGHT, "+ Light" }, { HADEV_COVER, "+ Cover" },
        { HADEV_SWITCH, "+ Switch" }, { HADEV_SCRIPT, "+ Script" },
        { HADEV_SCENE, "+ Scene" },
    };
    for (size_t i = 0; i < sizeof(adds)/sizeof(adds[0]); i++)
        small_btn(addrow, adds[i].txt, 0x2e5e8a, SX(140), SY(40), on_add, adds[i].type);

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
        "No devices yet. Use the buttons above to add a light, cover,\n"
        "switch, script or scene from your Home Assistant.");
    lv_obj_align(empty_hint, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(empty_hint, LV_OBJ_FLAG_HIDDEN);

    rebuild();
    return scr_root;
}
