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
#include <ctype.h>

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
    if (scr_root) { lv_obj_del_async(scr_root); scr_root = NULL; }
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
    if (scr_root) { lv_obj_del_async(scr_root); scr_root = NULL; }
}

/* Cached fetch results so the search box re-filters without re-querying HA
 * (the full /api/states fetch is slow + heavy on this CPU). */
static ha_discovered_t g_ents[HA_DISCOVERED_MAX];
static int             g_ecount  = 0;
static lv_obj_t      * search_ta = NULL;
static lv_obj_t      * kb        = NULL;

/* Case-insensitive substring test. */
static int ci_contains(const char * hay, const char * needle) {
    if (!needle || !needle[0]) return 1;
    size_t nl = strlen(needle);
    for (const char * h = hay; *h; h++) {
        size_t i = 0;
        while (i < nl && h[i] &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

/* Entity matches if its id OR friendly name contains ANY space/comma-separated
 * token — so "energie gas" finds either, and the preset chips can OR a set of
 * keywords. Empty filter = match all. */
static int ent_matches(int i, const char * filter) {
    if (!filter || !filter[0]) return 1;
    char buf[160];
    snprintf(buf, sizeof buf, "%s", filter);
    for (char * tok = strtok(buf, " ,"); tok; tok = strtok(NULL, " ,")) {
        if (ci_contains(g_ents[i].entity_id, tok) ||
            ci_contains(g_ents[i].friendly_name, tok)) return 1;
    }
    return 0;
}

/* Rebuild the visible list from the cache, applying `filter`. Rendering is
 * capped (creating hundreds of LVGL buttons is slow on the 400MHz Toon); the
 * full set is still searched, so typing a fragment surfaces anything. */
#define PICK_MAX_SHOWN 80
static void populate_list(const char * filter) {
    if (!lst_results) return;
    lv_obj_clean(lst_results);
    int shown = 0, matched = 0;
    for (int i = 0; i < g_ecount; i++) {
        if (!ent_matches(i, filter)) continue;
        matched++;
        if (shown >= PICK_MAX_SHOWN) continue;   /* keep counting, stop rendering */
        char label[160];
        snprintf(label, sizeof label, "%s\n%s",
                 g_ents[i].friendly_name, g_ents[i].entity_id);
        lv_obj_t * btn = lv_list_add_btn(lst_results, NULL, label);
        struct pick_item * it = malloc(sizeof *it);
        if (it) {
            snprintf(it->entity, sizeof it->entity, "%s", g_ents[i].entity_id);
            snprintf(it->fname,  sizeof it->fname,  "%s", g_ents[i].friendly_name);
        }
        lv_obj_set_user_data(btn, it);
        lv_obj_add_event_cb(btn, on_pick, LV_EVENT_CLICKED, NULL);
        /* entity_id line (line 2) smaller + dimmer; LV_LIST uses one label. */
        lv_obj_t * lbl = lv_obj_get_child(btn, 0);
        if (lbl) {
            lv_obj_set_style_text_font(lbl, SF(18), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x8899aa), 0);
        }
        shown++;
    }
    if (matched == 0)
        lv_list_add_text(lst_results, g_ecount ? "No match" :
                         "No entities found — check HA connection");
    else if (matched > shown) {
        char more[64];
        snprintf(more, sizeof more, "+%d more — type to narrow", matched - shown);
        lv_list_add_text(lst_results, more);
    }
}

/* Fetch entities from HA once into the cache, then show them (filtered by the
 * current search text, if any). */
static void load_entities(void) {
    g_ecount = 0;
    int ok = ha_discover_entities(g_domain, g_ents, &g_ecount, HA_DISCOVERED_MAX);
    if (ok != 0) g_ecount = 0;
    if (spinner) lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    populate_list(search_ta ? lv_textarea_get_text(search_ta) : "");
}

/* Live re-filter as the search text changes. */
static void on_search(lv_event_t * e) {
    (void)e;
    if (search_ta) populate_list(lv_textarea_get_text(search_ta));
}
/* Clear chip — empty the search box and show the full (capped) list. */
static void on_chip(lv_event_t * e) {
    const char * kw = (const char *)lv_event_get_user_data(e);
    if (search_ta) lv_textarea_set_text(search_ta, kw ? kw : "");
    populate_list(kw ? kw : "");
    if (kb) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}
/* Show the keyboard when the search box is tapped; hide on its OK/close. */
static void on_search_focus(lv_event_t * e) {
    (void)e;
    if (kb) { lv_keyboard_set_textarea(kb, search_ta); lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN); }
}
static void on_kb_done(lv_event_t * e) {
    (void)e;
    if (kb) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void on_refresh(lv_event_t * e) {
    (void)e;
    lv_obj_clean(lst_results);
    lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    load_entities();
}

lv_obj_t * screen_ha_picker_create(void) {
    /* Create on the TOP LAYER — settings modals also live there, and
       lv_layer_top renders above every screen. If we created on the default
       display, the picker would sit behind any open modal backdrop. */
    scr_root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(scr_root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr_root, LV_OBJ_FLAG_CLICKABLE);   /* block clicks from reaching modal behind */

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
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, SY(8));

    /* Search box — type any part of the entity id or name to filter live. */
    search_ta = lv_textarea_create(scr_root);
    lv_textarea_set_one_line(search_ta, true);
    lv_textarea_set_placeholder_text(search_ta, "search…");
    lv_obj_set_width(search_ta, LV_PCT(60));
    lv_obj_align(search_ta, LV_ALIGN_TOP_LEFT, SX(16), SY(44));
    lv_obj_add_event_cb(search_ta, on_search, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(search_ta, on_search_focus, LV_EVENT_FOCUSED, NULL);

    /* Just a Clear chip — empties the search box so the full list shows. Type
     * a fragment of the entity/name in the search box to narrow it. */
    static const char * const chips[][2] = {
        { "Clear", "" },
    };
    int cx = SX(16);
    for (unsigned i = 0; i < sizeof(chips) / sizeof(chips[0]); i++) {
        lv_obj_t * c = lv_btn_create(scr_root);
        lv_obj_set_height(c, SY(34));
        lv_obj_align(c, LV_ALIGN_TOP_LEFT, cx, SY(92));
        lv_obj_set_style_bg_color(c, lv_color_hex(0x2a3a52), 0);
        lv_obj_set_style_radius(c, SY(17), 0);
        lv_obj_add_event_cb(c, on_chip, LV_EVENT_CLICKED, (void *)chips[i][1]);
        lv_obj_t * cl = lv_label_create(c);
        lv_label_set_text(cl, chips[i][0]);
        lv_obj_center(cl);
        lv_obj_update_layout(c);
        cx += lv_obj_get_width(c) + SX(8);
    }

    /* Results list */
    lst_results = lv_list_create(scr_root);
    lv_obj_set_size(lst_results, LV_PCT(94), SY(330));
    lv_obj_align(lst_results, LV_ALIGN_TOP_MID, 0, SY(138));

    /* Spinner — shown while the fetch is in flight */
    spinner = lv_spinner_create(scr_root, 1000, 60);
    lv_obj_set_size(spinner, SY(40), SY(40));
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 0);

    /* On-screen keyboard for the search box — hidden until the box is tapped. */
    kb = lv_keyboard_create(scr_root);
    lv_keyboard_set_textarea(kb, search_ta);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(kb, on_kb_done, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(kb, on_kb_done, LV_EVENT_CANCEL, NULL);

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
    screen_ha_picker_create();  /* creates directly on lv_layer_top — no screen push */
}

/* "Add device" mode — the picked entity is appended to the device list as
 * `dev_type` (HADEV_*) instead of being written into a textarea. The caller
 * (the device-manager screen) rebuilds its list on SCREEN_LOADED when this
 * screen pops, so the new device appears immediately. */
void screen_ha_picker_open_add(const char * domain, int dev_type) {
    snprintf(g_domain, sizeof(g_domain), "%s", domain ? domain : "");
    g_target_ta = NULL;
    g_add_type  = dev_type;
    screen_ha_picker_create();  /* creates directly on lv_layer_top — no screen push */
}
