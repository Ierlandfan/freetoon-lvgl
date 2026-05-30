/*
 * Heater detail → Advanced — full list of OT DataId values, pulled directly
 * from OTGW's HTTP API. Polls every 4 s and renders one row per field.
 *
 * URL: http://<settings.otgw_host>/api/v1/otgw/otmonitor
 * Response shape:
 *   {"otmonitor":[
 *     {"name":"flamestatus","value":"Off","unit":"","epoch":...},
 *     {"name":"boilertemperature","value":21.42,"unit":"°C","epoch":...},
 *     ...
 *   ]}
 *
 * No mapping table — we display whatever otmonitor returns, which is the
 * authoritative list of DIDs OTGW is currently tracking. ~33 fields today.
 */
#include "screens.h"
#include "display.h"
#include "http.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static lv_obj_t * scr_root = NULL;
static lv_obj_t * list_col = NULL;
static lv_obj_t * lbl_status = NULL;
static lv_timer_t * refresh_timer = NULL;
static char        last_json[16384];

/* Extract next "name"/"value"/"unit" triple starting at *p; advances *p past it.
   Returns 1 on success, 0 at end-of-stream. */
static int next_field(const char **p, char *name, size_t namesz,
                      char *value, size_t valsz, char *unit, size_t unitsz) {
    const char *q = strstr(*p, "\"name\":");
    if (!q) return 0;
    q = strchr(q, '"'); if (!q) return 0;        /* opening quote of "name" */
    q = strchr(q + 1, '"'); if (!q) return 0;    /* closing quote of name */
    q = strchr(q + 1, '"'); if (!q) return 0;    /* opening quote of value text */
    /* copy name */
    {
        const char *s = q + 1;
        const char *e = strchr(s, '"'); if (!e) return 0;
        size_t n = e - s; if (n > namesz - 1) n = namesz - 1;
        memcpy(name, s, n); name[n] = 0;
        q = e;
    }
    /* find "value": */
    q = strstr(q, "\"value\":"); if (!q) return 0;
    q += 8; while (*q == ' ') q++;
    /* read string or number */
    if (*q == '"') {
        q++;
        const char *e = strchr(q, '"'); if (!e) return 0;
        size_t n = e - q; if (n > valsz - 1) n = valsz - 1;
        memcpy(value, q, n); value[n] = 0;
        q = e + 1;
    } else {
        size_t n = 0;
        while (*q && (*q == '.' || *q == '-' || *q == '+'
                   || (*q >= '0' && *q <= '9')) && n + 1 < valsz)
            value[n++] = *q++;
        value[n] = 0;
    }
    /* unit */
    unit[0] = 0;
    const char *u = strstr(q, "\"unit\":");
    if (u && (u - q) < 80) {
        u = strchr(u, '"'); if (!u) return 1;
        u = strchr(u + 1, '"'); if (!u) return 1;
        u = strchr(u + 1, '"'); if (!u) return 1;
        const char *s = u + 1;
        const char *e = strchr(s, '"'); if (!e) return 1;
        size_t n = e - s; if (n > unitsz - 1) n = unitsz - 1;
        memcpy(unit, s, n); unit[n] = 0;
        q = e;
    }
    *p = q;
    return 1;
}

/* Clear existing list rows, repopulate from `last_json`. */
static void rebuild_list(void) {
    lv_obj_clean(list_col);
    const char *p = last_json;
    char name[64], value[64], unit[16];
    int rows = 0;
    while (next_field(&p, name, sizeof(name), value, sizeof(value),
                      unit, sizeof(unit))) {
        lv_obj_t * row = lv_obj_create(list_col);
        lv_obj_set_size(row, DISP_HOR - SX(44), SY(40));
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, SF(18), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_label_set_text_fmt(lbl, "%-26s %s %s", name, value, unit);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, SX(4), 0);
        rows++;
    }
    if (lbl_status)
        lv_label_set_text_fmt(lbl_status, "%d fields", rows);
}

static void on_refresh(lv_timer_t * t) {
    (void)t;
    if (!settings.otgw_host[0]) {
        if (lbl_status) lv_label_set_text(lbl_status, "no OTGW host configured");
        return;
    }
    char url[160];
    snprintf(url, sizeof(url), "http://%s/api/v1/otgw/otmonitor", settings.otgw_host);
    int rc = http_fetch(url, last_json, sizeof(last_json));
    if (rc == 0) rebuild_list();
    else if (lbl_status) lv_label_set_text(lbl_status, "fetch failed");
}

static void on_back(lv_event_t * e) {
    (void)e;
    ui_pop();
}

lv_obj_t * screen_heater_advanced_create(void) {
    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(scr_root, 0, 0);

    /* header */
    lv_obj_t * hdr = lv_label_create(scr_root);
    lv_obj_set_style_text_font(hdr, SF(28), 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xffffff), 0);
    lv_label_set_text(hdr, "Boiler — Advanced (OTGW)");
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, SX(20), SY(16));

    /* back button */
    lv_obj_t * back = lv_btn_create(scr_root);
    lv_obj_set_size(back, SX(90), SY(44));
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -SX(16), SY(14));
    lv_obj_set_style_bg_color(back, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_label_set_text(bl, "Back");
    lv_obj_center(bl);

    /* status (field count or error) */
    lbl_status = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_status, SF(14), 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x888888), 0);
    lv_label_set_text(lbl_status, "loading...");
    lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, SX(20), SY(56));

    /* scrollable column for the field list */
    list_col = lv_obj_create(scr_root);
    lv_obj_set_size(list_col, DISP_HOR - SX(20), DISP_VER - SY(90));
    lv_obj_align(list_col, LV_ALIGN_TOP_LEFT, SX(10), SY(82));
    lv_obj_set_style_bg_color(list_col, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(list_col, 0, 0);
    lv_obj_set_flex_flow(list_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_col, 0, 0);

    /* first fetch + 4s refresh */
    on_refresh(NULL);
    refresh_timer = lv_timer_create(on_refresh, 4000, NULL);

    return scr_root;
}
