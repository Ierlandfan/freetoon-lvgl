/*
 * Tile-reassignment registry — scans /mnt/data/integrations on startup,
 * parses each manifest.json, exposes the result via tile_slots_xxx so the
 * Settings modal and home-tile renderer can iterate. BoxTalk subscribes
 * to each service so notify frames feed the latest-value cache.
 */
#include "tile_slots.h"
#include "settings.h"
#include "boxtalk.h"
#include "notify.h"
#include "i18n.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define INTEG_ROOT "/mnt/data/integrations"

static integration_meta_t g_integ[MAX_INSTALLED_INTEGRATIONS];
static int                g_integ_count = 0;

const char * tile_slot_label(int slot) {
    static char buf[48];
    if (slot == 0) return tr("Energie", "Energy");
    if (slot == 1) return tr("Familie", "Family");
    if (slot == 2) return tr("Ventilatie", "Ventilation");
    if (slot == 3) return tr("Water", "Water");
    if (slot == 4) return tr("Pagina 2 - Linksboven", "Page 2 - Top left");
    if (slot == 5) return tr("Pagina 2 - Rechtsboven", "Page 2 - Top right");
    if (slot == 6) return tr("Pagina 2 - Linksonder", "Page 2 - Bottom left");
    if (slot == 7) return tr("Pagina 2 - Rechtsonder", "Page 2 - Bottom right");
    if (slot >= TILE_SLOT_P1_0 && slot < TILE_SLOT_COUNT) {
        snprintf(buf, sizeof buf, "%s %d", tr("Pagina 2 - Slot", "Page 2 - Slot"), slot - TILE_SLOT_P1_0 + 1);
        return buf;
    }
    return "?";
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
    extract_str(json, "version",    m->version,    sizeof m->version);
    extract_str(json, "service_id", m->service_id, sizeof m->service_id);
    extract_tile_str(json, "type",          m->tile_type,     sizeof m->tile_type);
    extract_tile_str(json, "title",         m->tile_title,    sizeof m->tile_title);
    extract_tile_str(json, "color",         m->tile_color,    sizeof m->tile_color);
    extract_tile_str(json, "icon",          m->tile_icon,     sizeof m->tile_icon);
    extract_tile_str(json, "value_field",   m->value_field,   sizeof m->value_field);
    extract_tile_str(json, "value_unit",    m->value_unit,    sizeof m->value_unit);
    extract_tile_str(json, "subtitle_field",m->subtitle_field,sizeof m->subtitle_field);
    extract_tile_str(json, "subtitle_unit", m->subtitle_unit, sizeof m->subtitle_unit);
    extract_tile_str(json, "alert_field",   m->alert_field,   sizeof m->alert_field);

    /* Check for optional detail_screen */
    const char * ds = strstr(json, "\"detail_screen\"");
    if (ds) {
        const char * ds_obj = strchr(ds, '{');
        if (ds_obj) {
            extract_str(ds_obj, "type", m->detail_type, sizeof m->detail_type);
            extract_str(ds_obj, "title", m->detail_title, sizeof m->detail_title);
            extract_str(ds_obj, "y_unit", m->detail_y_unit, sizeof m->detail_y_unit);

            /* Parse series array if present */
            const char * s_arr = strstr(ds_obj, "\"series\"");
            if (s_arr) {
                const char * p = strchr(s_arr, '[');
                while (p && *p && m->series_count < INTEG_MAX_SERIES) {
                    const char * obj_start = strchr(p, '{');
                    if (!obj_start) break;
                    const char * obj_end = strchr(obj_start, '}');
                    if (!obj_end) break;

                    char sbuf[512];
                    size_t slen = (size_t)(obj_end - obj_start + 1);
                    if (slen >= sizeof sbuf) slen = sizeof sbuf - 1;
                    memcpy(sbuf, obj_start, slen);
                    sbuf[slen] = 0;

                    integ_series_meta_t * ser = &m->series[m->series_count];
                    extract_str(sbuf, "name", ser->name, sizeof ser->name);
                    extract_str(sbuf, "color", ser->color, sizeof ser->color);
                    extract_str(sbuf, "live_field", ser->live_field, sizeof ser->live_field);
                    extract_str(sbuf, "data_file", ser->data_file, sizeof ser->data_file);
                    m->series_count++;

                    p = obj_end + 1;
                    const char * next_arr_end = strchr(p, ']');
                    const char * next_obj = strchr(p, '{');
                    if (next_arr_end && (!next_obj || next_arr_end < next_obj)) break;
                }
            }
        }
    }

    /* Check for cached state.json if daemon has written one */
    char state_path[256];
    snprintf(state_path, sizeof state_path, "%s/state.json", dir_path);
    FILE * sf = fopen(state_path, "r");
    if (sf) {
        char sbuf[512];
        size_t sn = fread(sbuf, 1, sizeof sbuf - 1, sf);
        sbuf[sn] = 0;
        fclose(sf);
        extract_str(sbuf, "value", (char *)m->latest_value, sizeof m->latest_value);
        extract_str(sbuf, "subtitle", (char *)m->latest_subtitle, sizeof m->latest_subtitle);
        extract_str(sbuf, "alert", (char *)m->latest_alert, sizeof m->latest_alert);
    }

    g_integ_count++;
    fprintf(stderr,
        "[tiles] loaded integration: id=%s service=%s title='%s' "
        "value=%s%s subtitle=%s%s detail=%s (series=%d)\n",
        m->id, m->service_id, m->tile_title,
        m->value_field, m->value_unit,
        m->subtitle_field, m->subtitle_unit,
        m->detail_type[0] ? m->detail_type : "none",
        m->series_count);
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
    const char * bare_id = (strncmp(id, "local:", 6) == 0) ? id + 6 : id;
    for (int i = 0; i < g_integ_count; i++)
        if (strcmp(g_integ[i].id, id) == 0 || strcmp(g_integ[i].id, bare_id) == 0) return &g_integ[i];
    return NULL;
}

const char * tile_slots_integration_title(const integration_meta_t * m) {
    if (!m) return "";
    const char * t = m->tile_title[0] ? m->tile_title : (m->name[0] ? m->name : m->id);
    if (!strcasecmp(t, "weather") || !strcasecmp(m->id, "weather")) {
        return tr("Weer", "Weather");
    }
    return t;
}

/* Built-in local integrations selectable for the auto-rotate tile. */
static const struct { const char * id; const char * label; } g_locals[] = {
    { "local:energy", "Energie" },
    { "local:water",  "Water (P1)" },
    { "local:vent",   "Ventilatie" },
    { "local:family", "Familie" },
    { "local:air",    "Luchtkwaliteit" },
    { "local:waste",  "Afval" },
    { "local:agenda", "Agenda" },
    { "local:templogger", "Temperatuurlog" },
};
int tile_slots_local_count(void) {
    return (int)(sizeof g_locals / sizeof g_locals[0]);
}
const char * tile_slots_local_id(int i) {
    return (i >= 0 && i < tile_slots_local_count()) ? g_locals[i].id : "";
}
const char * tile_slots_local_label(int i) {
    return (i >= 0 && i < tile_slots_local_count()) ? g_locals[i].label : "";
}
#include "homewizard.h"
#include "ventilation.h"
#include "homeassistant.h"

int tile_slots_local_enabled(int i) {
    switch (i) {
        case 0: return 1;                                 /* energy — core (meteradapter/P1) */
        case 1: return settings.enable_p1_water || (settings.client_mode && hw_state.connected_water);
        case 2: return settings.enable_vent     || (settings.client_mode && vent_state.connected);
        case 3: return (settings.enable_ha || (settings.client_mode && ha_state.connected)) &&
                       (settings.life360_a_entity[0] || settings.life360_b_entity[0] || settings.life360_c_entity[0] || ha_state.loc_a[0]);
        case 4: return 1;                                 /* air quality — core sensor */
        case 5: return 1;                                 /* waste pickup */
        case 6: return settings.calendar_enabled;
        case 7: return 1;                                 /* templogger */
        default: return 0;
    }
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
        default:
            if (slot >= TILE_SLOT_P1_0 && slot < TILE_SLOT_COUNT)
                return settings.tile_slot_page1[slot - TILE_SLOT_P1_0];
            return NULL;
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
    if (slot == TILE_SLOT_P1_0 && integration_id && integration_id[0]) {
        settings.tile_rotate_enabled = 0;
    }
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

/* Slave bridge — replace the entire registry with a remote snapshot.
 * The slave doesn't run BoxTalk subscriptions; the master does, and
 * delivers per-integration manifest + latest_value/subtitle in every SSE
 * frame. We just copy them in. Idempotent; safe to call on each frame. */
void tile_slots_set_from_remote(int n, const integration_meta_t * src) {
    if (n < 0) n = 0;
    if (n > MAX_INSTALLED_INTEGRATIONS) n = MAX_INSTALLED_INTEGRATIONS;
    for (int i = 0; i < n; i++) g_integ[i] = src[i];
    g_integ_count = n;
}

void tile_slots_set_value(const char * integration_id, const char * val, const char * sub, const char * alert) {
    if (!integration_id || !integration_id[0]) return;
    const char * bare_id = (strncmp(integration_id, "local:", 6) == 0) ? integration_id + 6 : integration_id;
    for (int i = 0; i < g_integ_count; i++) {
        if (strcmp(g_integ[i].id, integration_id) == 0 || strcmp(g_integ[i].id, bare_id) == 0) {
            if (val) snprintf((char *)g_integ[i].latest_value, sizeof(g_integ[i].latest_value), "%s", val);
            if (sub) snprintf((char *)g_integ[i].latest_subtitle, sizeof(g_integ[i].latest_subtitle), "%s", sub);
            if (alert) {
                if (strcmp(alert, (char *)g_integ[i].latest_alert) != 0) {
                    if (alert[0]) notify_show("integration", g_integ[i].service_id, alert);
                    else          notify_clear("integration", g_integ[i].service_id);
                    snprintf((char *)g_integ[i].latest_alert, sizeof(g_integ[i].latest_alert), "%s", alert);
                }
            }
            g_integ[i].latest_epoch = (long)time(NULL);
            return;
        }
    }
}

void tile_slots_set_title(const char * integration_id, const char * title) {
    if (!integration_id || !integration_id[0] || !title || !title[0]) return;
    const char * bare_id = (strncmp(integration_id, "local:", 6) == 0) ? integration_id + 6 : integration_id;
    for (int i = 0; i < g_integ_count; i++) {
        if (strcmp(g_integ[i].id, integration_id) == 0 || strcmp(g_integ[i].id, bare_id) == 0) {
            snprintf(g_integ[i].tile_title, sizeof(g_integ[i].tile_title), "%s", title);
            return;
        }
    }
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
    /* Generic alert channel: integration sets <alert_field> non-empty to raise
     * a notification, empty to clear it. Edge-triggered on a change of text so
     * we don't re-post the same alert every notify cycle. */
    if (m->alert_field[0]) {
        extract_field_value(xml, m->alert_field, buf, sizeof buf);
        if (strcmp(buf, (char *)m->latest_alert) != 0) {
            if (buf[0]) notify_show("integration", m->service_id, buf);
            else        notify_clear("integration", m->service_id);
            snprintf((char *)m->latest_alert, sizeof m->latest_alert, "%s", buf);
        }
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
        if (g_integ[i].service_id[0]) {
            /* Subscribe with the FULL serviceId URN, like every built-in service
             * (boxtalk.c subscribes TemperatureSensor/BoilerInfo/… as
             * "urn:hcb-hae-com:serviceId:<name>"). The broker routes notifies by
             * exact serviceid, and providers publish the URN form
             * (urn:hcb-hae-com:serviceId:cryptoTicker), so a bare-id subscription
             * never matched -> the notify was never delivered and the tile stayed
             * empty. handle_notify still matches the manifest by stripped tail. */
            char urn[INTEG_SERVICE_MAX + 32];
            snprintf(urn, sizeof urn, "urn:hcb-hae-com:serviceId:%s",
                     g_integ[i].service_id);
            boxtalk_subscribe_service(urn);
        }
    }
}
