/*
 * Tile-reassignment registry — scans /mnt/data/integrations on startup,
 * parses each manifest.json, exposes the result via tile_slots_xxx so the
 * Settings modal and home-tile renderer can iterate. BoxTalk subscribes
 * to each service so notify frames feed the latest-value cache.
 */
#include "tile_slots.h"
#include "settings.h"
#include "boxtalk.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define INTEG_ROOT "/mnt/data/integrations"

static integration_meta_t g_integ[MAX_INSTALLED_INTEGRATIONS];
static int                g_integ_count = 0;

static const char * SLOT_LABELS[TILE_SLOT_COUNT] = {
    "Energy", "Family", "Vent", "Water"
};
const char * tile_slot_label(int slot) {
    if (slot < 0 || slot >= TILE_SLOT_COUNT) return "?";
    return SLOT_LABELS[slot];
}

/* ===================================================================== */
/* manifest.json walker — same brittle-but-OK grep style used by
 * screen_marketplace.c. Pulls "key":"value" or "key":{...} fields. */
/* ===================================================================== */
static int extract_str(const char * src, const char * key,
                       char * out, size_t outsz) {
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char * p = strstr(src, needle);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < outsz) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = 0;
    return 1;
}

/* The tile.* sub-fields live inside a nested object. Find the "tile":{"
 * boundary first, then run extract_str against just that substring so
 * we don't accidentally pick up a top-level "title" or "color". */
static int extract_tile_str(const char * src, const char * key,
                            char * out, size_t outsz) {
    const char * obj = strstr(src, "\"tile\"");
    if (!obj) return 0;
    obj = strchr(obj, '{');
    if (!obj) return 0;
    const char * end = strchr(obj, '}');
    if (!end) return 0;
    /* Copy the sub-object into a local buffer so extract_str's strstr
     * doesn't escape it. 512 chars handles any reasonable tile spec. */
    char sub[512];
    size_t n = (size_t)(end - obj);
    if (n >= sizeof sub) n = sizeof sub - 1;
    memcpy(sub, obj, n);
    sub[n] = 0;
    return extract_str(sub, key, out, outsz);
}

/* Read one integration's manifest into g_integ[]. */
static int load_manifest(const char * dir_path) {
    if (g_integ_count >= MAX_INSTALLED_INTEGRATIONS) return -1;

    char path[256];
    snprintf(path, sizeof path, "%s/manifest.json", dir_path);
    FILE * f = fopen(path, "r");
    if (!f) return -1;

    static char json[4096];
    size_t n = fread(json, 1, sizeof json - 1, f);
    json[n] = 0;
    fclose(f);
    if (n == 0) return -1;

    integration_meta_t * m = &g_integ[g_integ_count];
    memset(m, 0, sizeof *m);

    if (!extract_str(json, "id", m->id, sizeof m->id)) return -1;
    extract_str(json, "name",       m->name,       sizeof m->name);
    extract_str(json, "service_id", m->service_id, sizeof m->service_id);
    extract_tile_str(json, "title",         m->tile_title,    sizeof m->tile_title);
    extract_tile_str(json, "color",         m->tile_color,    sizeof m->tile_color);
    extract_tile_str(json, "icon",          m->tile_icon,     sizeof m->tile_icon);
    extract_tile_str(json, "value_field",   m->value_field,   sizeof m->value_field);
    extract_tile_str(json, "value_unit",    m->value_unit,    sizeof m->value_unit);
    extract_tile_str(json, "subtitle_field",m->subtitle_field,sizeof m->subtitle_field);
    extract_tile_str(json, "subtitle_unit", m->subtitle_unit, sizeof m->subtitle_unit);

    g_integ_count++;
    fprintf(stderr,
        "[tiles] loaded integration: id=%s service=%s title='%s' "
        "value=%s%s subtitle=%s%s\n",
        m->id, m->service_id, m->tile_title,
        m->value_field, m->value_unit,
        m->subtitle_field, m->subtitle_unit);
    return 0;
}

int tile_slots_integration_count(void) {
    return g_integ_count;
}

const integration_meta_t * tile_slots_integration_at(int i) {
    if (i < 0 || i >= g_integ_count) return NULL;
    return &g_integ[i];
}

const integration_meta_t * tile_slots_integration_by_id(const char * id) {
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < g_integ_count; i++)
        if (strcmp(g_integ[i].id, id) == 0) return &g_integ[i];
    return NULL;
}

integration_meta_t * tile_slots_integration_by_service(const char * service_id) {
    if (!service_id || !service_id[0]) return NULL;
    for (int i = 0; i < g_integ_count; i++)
        if (strcmp(g_integ[i].service_id, service_id) == 0) return &g_integ[i];
    return NULL;
}

/* ===================================================================== */
/* Slot binding accessors — read/write settings.tile_slot_<name>          */
/* ===================================================================== */
static char * slot_field(int slot) {
    switch (slot) {
        case TILE_SLOT_ENERGY: return settings.tile_slot_energy;
        case TILE_SLOT_FAMILY: return settings.tile_slot_family;
        case TILE_SLOT_VENT:   return settings.tile_slot_vent;
        case TILE_SLOT_WATER:  return settings.tile_slot_water;
        default: return NULL;
    }
}

const char * tile_slots_binding(int slot) {
    char * f = slot_field(slot);
    return (f && f[0]) ? f : NULL;
}

const integration_meta_t * tile_slots_meta_for(int slot) {
    const char * id = tile_slots_binding(slot);
    if (!id) return NULL;
    return tile_slots_integration_by_id(id);
}

void tile_slots_bind(int slot, const char * integration_id) {
    char * f = slot_field(slot);
    if (!f) return;
    snprintf(f, sizeof settings.tile_slot_energy, "%s",
             integration_id ? integration_id : "");
    settings_save();
    fprintf(stderr, "[tiles] slot %s bound to '%s'\n",
            tile_slot_label(slot), f);
}

/* ===================================================================== */
/* BoxTalk notify dispatch — called from boxtalk.c when an unknown        */
/* service notify lands.                                                  */
/* ===================================================================== */
static void extract_field_value(const char * xml, const char * field,
                                char * out, size_t outsz) {
    if (!field || !field[0]) { out[0] = 0; return; }
    char open[64];
    snprintf(open, sizeof open, "<%s>", field);
    const char * p = strstr(xml, open);
    if (!p) { out[0] = 0; return; }
    p += strlen(open);
    char close[64];
    snprintf(close, sizeof close, "</%s>", field);
    const char * e = strstr(p, close);
    if (!e) { out[0] = 0; return; }
    size_t n = (size_t)(e - p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

void tile_slots_on_notify(const char * service_id, const char * xml) {
    integration_meta_t * m = tile_slots_integration_by_service(service_id);
    if (!m) return;

    char buf[INTEG_VALUE_MAX];
    if (m->value_field[0]) {
        extract_field_value(xml, m->value_field, buf, sizeof buf);
        if (buf[0]) snprintf((char *)m->latest_value,
                             sizeof m->latest_value, "%s", buf);
    }
    if (m->subtitle_field[0]) {
        extract_field_value(xml, m->subtitle_field, buf, sizeof buf);
        if (buf[0]) snprintf((char *)m->latest_subtitle,
                             sizeof m->latest_subtitle, "%s", buf);
    }
    m->latest_epoch = (long)time(NULL);
}

/* ===================================================================== */
/* Init — scan + register integrations + ask boxtalk to subscribe         */
/* ===================================================================== */
int tile_slots_init(void) {
    g_integ_count = 0;
    DIR * d = opendir(INTEG_ROOT);
    if (!d) {
        fprintf(stderr, "[tiles] no %s dir — no marketplace integrations\n",
                INTEG_ROOT);
        return 0;
    }
    struct dirent * e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[256];
        snprintf(sub, sizeof sub, "%s/%s", INTEG_ROOT, e->d_name);
        struct stat st;
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        load_manifest(sub);
    }
    closedir(d);

    fprintf(stderr, "[tiles] %d integrations registered\n", g_integ_count);
    return 0;
}

/* Called by boxtalk after the handshake / on every reconnect — the broker
 * doesn't persist subscriptions across a socket drop. */
void tile_slots_subscribe_all(void) {
    for (int i = 0; i < g_integ_count; i++) {
        if (g_integ[i].service_id[0])
            boxtalk_subscribe_service(g_integ[i].service_id);
    }
}
