/*
 * screen_ha_picker.c — HA entity picker. Called from the Settings → HA entities
 * modal's [🔍] browse buttons. Fetches entities from HA via /api/states filtered
 * by domain (cover, sensor, binary_sensor, camera, device_tracker, calendar),
 * shows them in a scrollable list, and on tap writes the chosen entity_id into
 * the calling textarea.
 */
#include "screens.h"
#include "display.h"        /* SX()/SY()/SF() */
#include "homeassistant.h"  /* ha_discover_entities, ha_discovered_t */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t * scr_root;
static lv_obj_t * lbl_title;
static lv_obj_t * lst_results;
static lv_obj_t * spinner;

/* Set by screen_ha_picker_open() before the screen is created. */
static char          g_domain[32];
static lv_obj_t    * g_target_ta;

static void on_back(lv_event_t * e) {
    (void)e;
    g_target_ta = NULL;
    ui_pop();
}

static void on_pick(lv_event_t * e) {
    lv_obj_t   * btn = lv_event_get_target(e);
    const char * entity_id = (const char *)lv_obj_get_user_data(btn);
    if (entity_id && entity_id[0] && g_target_ta) {
        lv_textarea_set_text(g_target_ta, entity_id);
    }
    g_target_ta = NULL;
    ui_pop();
}

/* Fetch entities from HA and populate the list. */
static void load_entities(void) {
    lv_obj_clean(lst_results);

    ha_discovered_t ents[HA_DISCOVERED_MAX];
    int count = 0;

    int ok = ha_discover_entities(g_domain, ents, &count, HA_DISCOVERED_MAX);

    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);

    if (ok != 0 || count == 0) {
        char msg[80];
        snprintf(msg, sizeof(msg), "No %s entities found — check HA connection",
                 g_domain);
        lv_list_add_text(lst_results, msg);
        return;
    }

    for (int i = 0; i < count; i++) {
        char label[160];
        snprintf(label, sizeof(label), "%s\n%s",
                 ents[i].friendly_name, ents[i].entity_id);

        lv_obj_t * btn = lv_list_add_btn(lst_results, NULL, label);

        /* Stash the entity_id so on_pick can read it back. */
        char * id_copy = strdup(ents[i].entity_id);
        lv_obj_set_user_data(btn, id_copy);

        lv_obj_add_event_cb(btn, on_pick, LV_EVENT_CLICKED, NULL);

        /* Style the entity_id line smaller and dimmer — it's line 1 of the
         * label. LV_LIST's default button label is a single lv_label, so the
         * newline gives us a natural two-line look. */
        lv_obj_t * lbl = lv_obj_get_child(btn, 0);
        if (lbl) {
            lv_obj_set_style_text_font(lbl, SF(18), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x8899aa), 0);
        }
    }
}

static void on_refresh(lv_event_t * e) {
    (void)e;
    lv_obj_clean(lst_results);
    lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    load_entities();
}

lv_obj_t * screen_ha_picker_create(void) {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x101418), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button */
    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, SX(8), SY(8));
    lv_obj_t * bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    /* Refresh button */
    lv_obj_t * refresh = lv_btn_create(scr_root);
    lv_obj_align(refresh, LV_ALIGN_TOP_RIGHT, SX(-8), SY(8));
    lv_obj_t * rl = lv_label_create(refresh);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_add_event_cb(refresh, on_refresh, LV_EVENT_CLICKED, NULL);

    /* Title */
    lbl_title = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_title, SF(22), 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xffffff), 0);
    char title[64];
    snprintf(title, sizeof(title), "Pick %s entity", g_domain);
    lv_label_set_text(lbl_title, title);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, SY(14));

    /* Results list */
    lst_results = lv_list_create(scr_root);
    lv_obj_set_size(lst_results, LV_PCT(94), SY(420));
    lv_obj_align(lst_results, LV_ALIGN_TOP_MID, 0, SY(60));

    /* Spinner — shown while the curl call is in flight */
    spinner = lv_spinner_create(scr_root, 1000, 60);
    lv_obj_set_size(spinner, SY(40), SY(40));
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 0);

    /* Kick off the fetch */
    load_entities();

    return scr_root;
}

/* Called by the browse buttons in screen_settings.c. Stores the target
 * textarea + domain filter, then pushes the picker screen. */
void screen_ha_picker_open(const char * domain, lv_obj_t * target_ta) {
    snprintf(g_domain, sizeof(g_domain), "%s", domain ? domain : "");
    g_target_ta = target_ta;
    ui_push(screen_ha_picker_create());
}
