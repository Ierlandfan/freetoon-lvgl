/*
 * screen_ha_picker.c — HA entity picker. Called from the Settings → HA entities
 * modal's [🔍] browse buttons. Fetches entities from HA via /api/states filtered
 * by domain (cover, sensor, binary_sensor, camera, device_tracker, calendar),
 * shows them in a scrollable list, and on tap writes the chosen entity_id into
 * the calling textarea.
 */
#include "screens.h"
#include "display.h"        /* SX()/SY()/SF() */
#include "i18n.h"
#include "homeassistant.h"  /* ha_discover_entities, ha_discovered_t */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t * scr_root;
static lv_obj_t * lbl_title;
static lv_obj_t * lst_results;
static lv_obj_t * spinner;

/* Set by screen_ha_picker_open*() before the screen is created.
 * g_add_type >= 0 => "add device" mode: the picked entity is appended to the
 * device list as that HADEV_* type. Otherwise the picked entity_id is written
 * into g_target_ta. */
static char          g_domain[32];
static lv_obj_t    * g_target_ta;
static int           g_add_type = -1;

/* Heap-stashed on each result button so on_pick can read back both fields. */
struct pick_item { char entity[64]; char fname[64]; };

static void on_back(lv_event_t * e) {
    (void)e;
    g_target_ta = NULL;
    g_add_type  = -1;
    ui_pop();
}

static void on_pick(lv_event_t * e) {
    lv_obj_t   * btn = lv_event_get_target(e);
    struct pick_item * it = (struct pick_item *)lv_obj_get_user_data(btn);
    if (it && it->entity[0]) {
        if (g_add_type >= 0)
            ha_device_add(g_add_type, it->entity, it->fname, 0);
        else if (g_target_ta)
            lv_textarea_set_text(g_target_ta, it->entity);
    }
    g_target_ta = NULL;
    g_add_type  = -1;
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
        snprintf(msg, sizeof(msg),
                 tr("Geen %s-entiteiten gevonden — controleer HA-verbinding",
                    "No %s entities found — check HA connection"),
                 g_domain);
        lv_list_add_text(lst_results, msg);
        return;
    }

    for (int i = 0; i < count; i++) {
        char label[160];
        snprintf(label, sizeof(label), "%s\n%s",
                 ents[i].friendly_name, ents[i].entity_id);

        lv_obj_t * btn = lv_list_add_btn(lst_results, NULL, label);

        /* Stash entity_id + friendly_name so on_pick can read them back. */
        struct pick_item * it = malloc(sizeof *it);
        if (it) {
            snprintf(it->entity, sizeof it->entity, "%s", ents[i].entity_id);
            snprintf(it->fname,  sizeof it->fname,  "%s", ents[i].friendly_name);
        }
        lv_obj_set_user_data(btn, it);

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
    lv_label_set_text(bl, tr(LV_SYMBOL_LEFT " Terug", LV_SYMBOL_LEFT " Back"));
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
    snprintf(title, sizeof(title), tr("Kies %s-entiteit", "Pick %s entity"), g_domain);
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
    g_add_type  = -1;
    ui_push(screen_ha_picker_create());
}

/* "Add device" mode — the picked entity is appended to the device list as
 * `dev_type` (HADEV_*) instead of being written into a textarea. The caller
 * (the device-manager screen) rebuilds its list on SCREEN_LOADED when this
 * screen pops, so the new device appears immediately. */
void screen_ha_picker_open_add(const char * domain, int dev_type) {
    snprintf(g_domain, sizeof(g_domain), "%s", domain ? domain : "");
    g_target_ta = NULL;
    g_add_type  = dev_type;
    ui_push(screen_ha_picker_create());
}
