/*
 * Marketplace screen — browses the catalog at
 *   https://raw.githubusercontent.com/Ierlandfan/freetoon-integrations/main/catalog/index.json
 * and lets the user install integrations on-device with one tap.
 *
 * Install path: tap "Install" → screen runs `/mnt/data/integrations-install.sh <id>`
 * via system() in a background thread. The shell helper is shipped by
 * install.sh; if missing we fall back to a hint asking the user to run the
 * curl one-liner manually over SSH.
 *
 * The catalog parse is intentionally brittle (grep + sed-style on the JSON
 * text) because we run on a busybox userland with no jq and don't want to
 * pull in cJSON for a single screen. Catalog shape is fixed enough that
 * "find next id/name/description/version/url" walks reliably.
 */
#include "screens.h"
#include "http.h"
#include "settings.h"
#include "icons.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define CATALOG_URL "https://raw.githubusercontent.com/Ierlandfan/freetoon-integrations/main/catalog/index.json"
#define INSTALL_HELPER "/mnt/data/integrations-install.sh"

#define COL_BG       0x0e1a2a
#define COL_CARD     0x1a2940
#define COL_TEXT_HI  0xffffff
#define COL_TEXT_DIM 0x88aabb
#define COL_OK       0x2e6e3a
#define COL_BUSY     0x6a5424
#define COL_WARN     0x6e3a3a

static lv_obj_t * scr_root = NULL;
static lv_obj_t * list_box = NULL;
static lv_obj_t * status_lbl = NULL;
static char       catalog_buf[16384];
static int        catalog_len = 0;

/* One row of the rendered list. We keep btn handles so the per-item
 * install can flip them to "Installing…" / "Installed" without rebuilding
 * the whole list. */
typedef struct {
    char id[64];
    char name[64];
    char description[256];
    char version[32];
    lv_obj_t * row;
    lv_obj_t * btn_install;
    lv_obj_t * btn_install_lbl;
} integration_t;

#define MAX_INTEGRATIONS 16
static integration_t entries[MAX_INTEGRATIONS];
static int           entry_count = 0;

/* ===================================================================== */
/* JSON walker — finds the next "field" : "value" or "field": value pair. */
/* ===================================================================== */
static const char * find_json_str(const char * src, const char * key,
                                  char * out, size_t outsz) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char * p = strstr(src, pat);
    if (!p) return NULL;
    p = strchr(p, ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\n' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < outsz) {
        if (*p == '\\' && p[1]) { p++; }   /* tolerate basic escapes */
        out[n++] = *p++;
    }
    out[n] = 0;
    return p;
}

/* Parse catalog_buf into entries[]. Tolerant of formatting and missing
 * fields — anything we couldn't find stays as empty string. */
static void parse_catalog(void) {
    entry_count = 0;
    const char * scan = strstr(catalog_buf, "\"integrations\"");
    if (!scan) return;
    while (entry_count < MAX_INTEGRATIONS) {
        const char * next = strstr(scan, "\"id\"");
        if (!next) break;
        integration_t * e = &entries[entry_count];
        memset(e, 0, sizeof *e);
        const char * after_id = find_json_str(next, "id", e->id, sizeof e->id);
        if (!after_id) break;
        find_json_str(next, "name",        e->name,        sizeof e->name);
        find_json_str(next, "description", e->description, sizeof e->description);
        find_json_str(next, "version",     e->version,     sizeof e->version);
        entry_count++;
        scan = after_id;   /* move past this id so the next pass picks the next */
    }
}

/* ===================================================================== */
/* Background fetcher                                                    */
/* ===================================================================== */
static void * fetch_thread(void * arg) {
    (void)arg;
    if (status_lbl)
        lv_label_set_text(status_lbl, "Fetching catalog...");
    /* http_fetch returns 0 on success / -1 on failure (NOT a byte count) and
     * fills catalog_buf. Treat a non-zero rc OR an empty body as failure. */
    int rc = http_fetch(CATALOG_URL, catalog_buf, sizeof catalog_buf);
    catalog_len = (int)strlen(catalog_buf);
    if (rc != 0 || catalog_len <= 0) {
        if (status_lbl)
            lv_label_set_text(status_lbl,
                "Catalog fetch failed - check your internet.");
        return NULL;
    }
    parse_catalog();
    /* Rebuild the visible list. We can't manipulate LVGL widgets from a
     * non-UI thread safely on this LVGL version, so we just flag the
     * shared state and let the on_screen_show handler repaint. */
    if (status_lbl) {
        char msg[64];
        snprintf(msg, sizeof msg, "%d integrations available", entry_count);
        lv_label_set_text(status_lbl, msg);
    }
    return NULL;
}

/* ===================================================================== */
/* Install action                                                        */
/* ===================================================================== */
static int install_helper_ok(void) {
    struct stat st;
    return stat(INSTALL_HELPER, &st) == 0 && (st.st_mode & S_IXUSR);
}

static void * install_thread(void * arg) {
    integration_t * e = (integration_t *)arg;
    if (e->btn_install_lbl) lv_label_set_text(e->btn_install_lbl, "Installing...");
    if (e->btn_install)
        lv_obj_set_style_bg_color(e->btn_install, lv_color_hex(COL_BUSY), 0);

    char cmd[256];
    snprintf(cmd, sizeof cmd, "%s %s", INSTALL_HELPER, e->id);
    int rc = system(cmd);

    if (rc == 0) {
        if (e->btn_install_lbl) lv_label_set_text(e->btn_install_lbl, "Installed");
        if (e->btn_install)
            lv_obj_set_style_bg_color(e->btn_install, lv_color_hex(COL_OK), 0);
    } else {
        if (e->btn_install_lbl) lv_label_set_text(e->btn_install_lbl, "Failed");
        if (e->btn_install)
            lv_obj_set_style_bg_color(e->btn_install, lv_color_hex(COL_WARN), 0);
    }
    return NULL;
}

static void on_install_clicked(lv_event_t * e) {
    integration_t * ent = (integration_t *)lv_event_get_user_data(e);
    if (!install_helper_ok()) {
        if (ent->btn_install_lbl)
            lv_label_set_text(ent->btn_install_lbl, "Need helper");
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, install_thread, ent) == 0)
        pthread_detach(t);
}

/* ===================================================================== */
/* Screen build                                                          */
/* ===================================================================== */
static void on_back(lv_event_t * e) { (void)e; ui_pop(); }

static void build_rows(void) {
    /* Wipe any previous rows */
    lv_obj_clean(list_box);

    for (int i = 0; i < entry_count; i++) {
        integration_t * ent = &entries[i];
        ent->row = lv_obj_create(list_box);
        lv_obj_set_size(ent->row, 940, 90);
        lv_obj_set_style_bg_color(ent->row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_border_width(ent->row, 0, 0);
        lv_obj_set_style_radius(ent->row, 12, 0);
        lv_obj_set_style_pad_all(ent->row, 10, 0);
        lv_obj_clear_flag(ent->row, LV_OBJ_FLAG_SCROLLABLE);

        /* Name + version on top row */
        lv_obj_t * lbl = lv_label_create(ent->row);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_HI), 0);
        if (ent->version[0])
            lv_label_set_text_fmt(lbl, "%s  v%s", ent->name, ent->version);
        else
            lv_label_set_text(lbl, ent->name);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 4, 0);

        /* Description, scrolling if long */
        lv_obj_t * desc = lv_label_create(ent->row);
        lv_obj_set_style_text_font(desc, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(desc, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_width(desc, 720);
        lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
        lv_label_set_text(desc, ent->description);
        lv_obj_align(desc, LV_ALIGN_TOP_LEFT, 4, 30);

        /* Install button */
        ent->btn_install = lv_btn_create(ent->row);
        lv_obj_set_size(ent->btn_install, 180, 60);
        lv_obj_align(ent->btn_install, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_set_style_bg_color(ent->btn_install, lv_color_hex(0x2a4060), 0);
        lv_obj_set_style_radius(ent->btn_install, 10, 0);
        lv_obj_add_event_cb(ent->btn_install, on_install_clicked,
                            LV_EVENT_CLICKED, ent);
        ent->btn_install_lbl = lv_label_create(ent->btn_install);
        lv_obj_set_style_text_color(ent->btn_install_lbl,
                                    lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(ent->btn_install_lbl,
                                    &lv_font_montserrat_22, 0);
        lv_label_set_text(ent->btn_install_lbl,
                          install_helper_ok() ? "Install" : "No helper");
        lv_obj_center(ent->btn_install_lbl);
    }
}

/* Called by the periodic refresh timer (every 500 ms). Cheap; only
 * rebuilds rows when entry_count just transitioned from 0 → N. */
static void refresh_cb(lv_timer_t * t) {
    (void)t;
    static int prev_count = -1;
    if (entry_count != prev_count) {
        build_rows();
        prev_count = entry_count;
    }
}

lv_obj_t * screen_marketplace_create(void) {
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Header */
    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_label_set_text(title, "Marketplace");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 180, 24);

    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, 140, 56);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 20, 14);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x3a4658), 0);
    lv_obj_set_style_radius(back, 10, 0);
    lv_obj_set_ext_click_area(back, 20);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_22, 0);
    lv_label_set_text(bl, "< Back");
    lv_obj_center(bl);

    /* Status line shown above the list while we fetch + report errors. */
    status_lbl = lv_label_create(scr_root);
    lv_obj_set_style_text_color(status_lbl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_18, 0);
    lv_label_set_text(status_lbl, "Fetching catalog...");
    lv_obj_align(status_lbl, LV_ALIGN_TOP_LEFT, 22, 80);

    /* Scrollable container for the rows */
    list_box = lv_obj_create(scr_root);
    lv_obj_set_size(list_box, 980, 470);
    lv_obj_align(list_box, LV_ALIGN_TOP_LEFT, 22, 110);
    lv_obj_set_style_bg_opa(list_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_box, 0, 0);
    lv_obj_set_style_pad_all(list_box, 4, 0);
    lv_obj_set_style_pad_row(list_box, 10, 0);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);

    /* Kick off the catalog fetch on entry. */
    catalog_len = 0;
    entry_count = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, fetch_thread, NULL) == 0)
        pthread_detach(t);

    lv_timer_create(refresh_cb, 500, NULL);

    return scr_root;
}
