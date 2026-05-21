/*
 * Tiny key=value config file at /mnt/data/toonui.cfg.
 * No JSON parser dependency. Loads defaults if file missing.
 */
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CFG_PATH "/mnt/data/toonui.cfg"

settings_t settings = {
    .auto_dim_enabled  = 1,
    .auto_dim_seconds  = 10,
    .auto_home_enabled = 1,
    .auto_home_seconds = 60,
    .active_brightness = 800,
    .dim_brightness    = 80,
    .temp_offset_centi = 0,
    .show_dim_weather  = 1,
    .show_dim_waste    = 1,
    .dim_waste_lead_days = 2,
    .vnc_enabled       = 0,
    .vnc_pass          = "",
    .weather_location    = "Medemblik",
    /* GeoNames id understood by forecast.buienradar.nl/2.0/forecast/<id>.
     * 2757783 = De Bilt — central NL national fallback. The KNMI station
     * code 6249 happens to collide with an unrelated mid-east location
     * in buienradar's GeoNames mapping, so we default to De Bilt and let
     * the user paste their own id into /mnt/data/toonui.cfg if they want
     * Medemblik-specific hourly data (the URL bar on
     * https://www.buienradar.nl/weer/medemblik/nl/<ID> reveals it). */
    .weather_location_id = 2757783,
    .forecast_mode       = FORECAST_AUTO,
    /* Default proxy — bridge sniffs Quby frames, republishes BoilerInfo
     * to BoxTalk, and forwards happ's OT-Writes to OTGW serial port
     * 25238 (which lights up the PWA boiler card). 2026-05-19 follow-up
     * test: when the bridge is started AFTER happ_thermstat has reached
     * a stable state, both proxy mode and CV pressure work — direct vs
     * proxy is robust if the startup race is avoided. The bridge's
     * kick_thermstat + verify_thermstat_on_pty + force_pty_raw logic
     * handles re-entry. */
    /* Default OFF: the quby_bridge proxy ships disabled, so claiming "proxy"
     * before the user enables it misrepresents the running state. */
    .ot_bridge_mode      = "off",
    .otgw_host           = "192.168.99.21",
    .otgw_user           = "",
    .otgw_pass           = "",
    /* MQTT defaults match the existing /mnt/data/mqtt.cfg so first boot
     * after the upgrade keeps the banner subscriber alive without manual
     * config. User edits in Settings → MQTT take precedence and survive
     * a reboot. */
    .mqtt_enabled        = 1,
    .mqtt_host           = "192.168.3.101",
    .mqtt_port           = 1883,
    .mqtt_user           = "brakero1",
    .mqtt_pass           = "",
    .mqtt_topics         = {"home/packages/banner", "home/packages/state"},
    .mqtt_topic_count    = 2,
    /* Integrations default OFF — see settings.h for the on-first-boot
     * auto-enable rule based on existing config files. */
    .enable_p1_elec      = 0,
    .enable_p1_water     = 0,
    .enable_vent         = 0,
    .enable_ha           = 0,
    .boot_picker_enabled = 1,
    .hide_offline_tiles  = 0,
    .update_check_enabled = 1,
    .energy_source       = 0,   /* meteradapter (official) by default */
};

float display_indoor_temp(float raw) {
    if (raw <= 0) return raw;
    return raw + (float)settings.temp_offset_centi / 100.0f;
}

/* True if a /mnt/data/<name> file exists and is non-empty. */
static int file_has_content(const char * path) {
    FILE * f = fopen(path, "r");
    if (!f) return 0;
    int c = fgetc(f);
    fclose(f);
    return (c != EOF);
}

/* True if /mnt/data/p1bridge.conf has a line whose host part matches the
 * given prefix (e.g. "192.168.99.115="). Used to derive enable_p1_water. */
static int p1conf_has_host(const char * host_eq) {
    FILE * f = fopen("/mnt/data/p1bridge.conf", "r");
    if (!f) return 0;
    char line[256]; int hit = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, host_eq, strlen(host_eq)) == 0) { hit = 1; break; }
    }
    fclose(f);
    return hit;
}

void settings_load(void) {
    /* Tracks which integration keys appeared in toonui.cfg. Anything still
     * 0 after the parse loop is filled in by the migration / autodetect
     * step below — keeps existing installs from suddenly losing pollers
     * after the upgrade. */
    int seen_p1_elec = 0, seen_p1_water = 0, seen_vent = 0, seen_ha = 0;
    int cfg_existed = 0;

    FILE * f = fopen(CFG_PATH, "r");
    if (!f) goto autodetect;
    cfg_existed = 1;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char k[64], v[64];
        /* Value runs to end-of-line, not the first space: city names like
         * "Sint Pancras" must survive (%63s would truncate to "Sint"). */
        if (sscanf(line, "%63[^=]=%63[^\n]", k, v) != 2) continue;
        /* Trim a trailing CR (CRLF cfg files) so values stay clean. */
        size_t vlen = strlen(v);
        if (vlen && v[vlen - 1] == '\r') v[vlen - 1] = 0;
        int iv = atoi(v);
        if      (strcmp(k, "auto_dim_enabled")  == 0) settings.auto_dim_enabled  = iv;
        else if (strcmp(k, "auto_dim_seconds")  == 0) settings.auto_dim_seconds  = iv;
        else if (strcmp(k, "auto_home_enabled") == 0) settings.auto_home_enabled = iv;
        else if (strcmp(k, "auto_home_seconds") == 0) settings.auto_home_seconds = iv;
        else if (strcmp(k, "active_brightness") == 0) settings.active_brightness = iv;
        else if (strcmp(k, "dim_brightness")    == 0) settings.dim_brightness    = iv;
        else if (strcmp(k, "temp_offset_centi") == 0) settings.temp_offset_centi = iv;
        else if (strcmp(k, "show_dim_weather")  == 0) settings.show_dim_weather  = iv;
        else if (strcmp(k, "show_dim_waste")    == 0) settings.show_dim_waste    = iv;
        else if (strcmp(k, "dim_waste_lead_days") == 0) settings.dim_waste_lead_days = iv;
        else if (strcmp(k, "waste_postcode")    == 0)
            snprintf(settings.waste_postcode, sizeof settings.waste_postcode, "%s", v);
        else if (strcmp(k, "waste_housenr")     == 0)
            snprintf(settings.waste_housenr, sizeof settings.waste_housenr, "%s", v);
        else if (strcmp(k, "show_dim_weather")  == 0) settings.show_dim_weather  = iv;
        else if (strcmp(k, "temp_offset_centi") == 0) settings.temp_offset_centi = iv;
        else if (strcmp(k, "weather_location_id") == 0) settings.weather_location_id = iv;
        else if (strcmp(k, "forecast_mode")     == 0) settings.forecast_mode       = iv;
        else if (strcmp(k, "weather_location")  == 0)
            snprintf(settings.weather_location,
                     sizeof settings.weather_location, "%s", v);
        else if (strcmp(k, "ot_bridge_mode")    == 0) {
            /* Migrate legacy values to the new 3-way naming. */
            const char *m = v;
            if      (strcmp(v, "keteladapter") == 0) m = "proxy";
            else if (strcmp(v, "otgw")         == 0) m = "wireless";
            snprintf(settings.ot_bridge_mode,
                     sizeof settings.ot_bridge_mode, "%s", m);
        }
        else if (strcmp(k, "otgw_host")         == 0)
            snprintf(settings.otgw_host,
                     sizeof settings.otgw_host, "%s", v);
        else if (strcmp(k, "otgw_user")         == 0)
            snprintf(settings.otgw_user,
                     sizeof settings.otgw_user, "%s", v);
        else if (strcmp(k, "otgw_pass")         == 0)
            snprintf(settings.otgw_pass,
                     sizeof settings.otgw_pass, "%s", v);
        else if (strcmp(k, "mqtt_enabled")      == 0) settings.mqtt_enabled = iv;
        else if (strcmp(k, "mqtt_host")         == 0)
            snprintf(settings.mqtt_host, sizeof settings.mqtt_host, "%s", v);
        else if (strcmp(k, "mqtt_port")         == 0) settings.mqtt_port = iv;
        else if (strcmp(k, "mqtt_user")         == 0)
            snprintf(settings.mqtt_user, sizeof settings.mqtt_user, "%s", v);
        else if (strcmp(k, "mqtt_pass")         == 0)
            snprintf(settings.mqtt_pass, sizeof settings.mqtt_pass, "%s", v);
        else if (strcmp(k, "mqtt_topic_count")  == 0) {
            settings.mqtt_topic_count = iv;
            if (settings.mqtt_topic_count > 8) settings.mqtt_topic_count = 8;
            if (settings.mqtt_topic_count < 0) settings.mqtt_topic_count = 0;
        }
        else if (strncmp(k, "mqtt_topic_", 11)  == 0) {
            int idx = atoi(k + 11);
            if (idx >= 0 && idx < 8)
                snprintf(settings.mqtt_topics[idx],
                         sizeof settings.mqtt_topics[0], "%s", v);
        }
        else if (strcmp(k, "enable_p1_elec")  == 0) { settings.enable_p1_elec  = iv; seen_p1_elec  = 1; }
        else if (strcmp(k, "enable_p1_water") == 0) { settings.enable_p1_water = iv; seen_p1_water = 1; }
        else if (strcmp(k, "enable_vent")     == 0) { settings.enable_vent     = iv; seen_vent     = 1; }
        else if (strcmp(k, "enable_ha")       == 0) { settings.enable_ha       = iv; seen_ha       = 1; }
        else if (strcmp(k, "enable_zwave")    == 0) settings.enable_zwave = iv;
        else if (strcmp(k, "energy_source")   == 0) settings.energy_source = iv;
        else if (strcmp(k, "boot_picker_enabled") == 0) settings.boot_picker_enabled = iv;
        else if (strcmp(k, "hide_offline_tiles")  == 0) settings.hide_offline_tiles = iv;
        else if (strcmp(k, "update_check_enabled") == 0) settings.update_check_enabled = iv;
        else if (strcmp(k, "vnc_enabled")     == 0) settings.vnc_enabled = iv;
        else if (strcmp(k, "vnc_pass")        == 0)
            snprintf(settings.vnc_pass, sizeof settings.vnc_pass, "%s", v);
        else if (strcmp(k, "tile_slot_energy") == 0)
            snprintf(settings.tile_slot_energy, sizeof settings.tile_slot_energy, "%s", v);
        else if (strcmp(k, "tile_slot_family") == 0)
            snprintf(settings.tile_slot_family, sizeof settings.tile_slot_family, "%s", v);
        else if (strcmp(k, "tile_slot_vent") == 0)
            snprintf(settings.tile_slot_vent, sizeof settings.tile_slot_vent, "%s", v);
        else if (strcmp(k, "tile_slot_water") == 0)
            snprintf(settings.tile_slot_water, sizeof settings.tile_slot_water, "%s", v);
    }
    fclose(f);

autodetect:
    /* Two-mode autodetect:
     *
     *  Legacy upgrade (toonui.cfg already exists but is missing the new
     *  enable_* keys) — assume all integrations were enabled, because in
     *  the pre-toggle build they were unconditionally on. Anything the
     *  user actually doesn't want they can flip off in Settings →
     *  Integrations and that choice is persisted.
     *
     *  Fresh install (no toonui.cfg at all — basic-mode install) — fall
     *  back to file-presence on the companion configs. With everything
     *  blank this comes up with all integrations off, matching the
     *  "basic release" intent. When the user passes P1_TOKEN / VENT_*
     *  / HA_TOKEN to install.sh the matching file gets written and the
     *  corresponding integration auto-comes-up on first boot.
     *
     *  Either way the first settings_save() persists explicit 0/1 values,
     *  so this branch only runs once per install. */
    {
    int legacy = cfg_existed;
    if (!seen_p1_elec)
        settings.enable_p1_elec  = legacy ? 1 : file_has_content("/mnt/data/p1bridge.conf");
    if (!seen_p1_water)
        settings.enable_p1_water = legacy ? 1 : p1conf_has_host("192.168.99.115=");
    if (!seen_vent)
        settings.enable_vent     = legacy ? 1 : file_has_content("/mnt/data/vent.conf");
    if (!seen_ha)
        settings.enable_ha       = legacy ? 1 : file_has_content("/mnt/data/ha.cfg");
    }

    /* Migration: if mqtt_pass is still empty, try to read /mnt/data/mqtt.cfg
     * (host:user:pass format from the legacy single-line tooling). Lets the
     * new settings-driven subscriber connect on first boot without the user
     * having to retype the broker password. */
    if (!settings.mqtt_pass[0]) {
        FILE * mf = fopen("/mnt/data/mqtt.cfg", "r");
        if (mf) {
            char line[256];
            if (fgets(line, sizeof(line), mf)) {
                char * nl = strchr(line, '\n'); if (nl) *nl = 0;
                /* host:user:pass */
                char * p1 = strchr(line, ':');
                char * p2 = p1 ? strchr(p1 + 1, ':') : NULL;
                if (p1 && p2) {
                    *p1 = 0; *p2 = 0;
                    if (!settings.mqtt_host[0])
                        snprintf(settings.mqtt_host, sizeof(settings.mqtt_host),
                                 "%s", line);
                    if (!settings.mqtt_user[0])
                        snprintf(settings.mqtt_user, sizeof(settings.mqtt_user),
                                 "%s", p1 + 1);
                    snprintf(settings.mqtt_pass, sizeof(settings.mqtt_pass),
                             "%s", p2 + 1);
                }
            }
            fclose(mf);
        }
    }
}

void settings_save(void) {
    FILE * f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "auto_dim_enabled=%d\n",  settings.auto_dim_enabled);
    fprintf(f, "auto_dim_seconds=%d\n",  settings.auto_dim_seconds);
    fprintf(f, "auto_home_enabled=%d\n", settings.auto_home_enabled);
    fprintf(f, "auto_home_seconds=%d\n", settings.auto_home_seconds);
    fprintf(f, "active_brightness=%d\n", settings.active_brightness);
    fprintf(f, "dim_brightness=%d\n",    settings.dim_brightness);
    fprintf(f, "temp_offset_centi=%d\n", settings.temp_offset_centi);
    fprintf(f, "show_dim_weather=%d\n",  settings.show_dim_weather);
    fprintf(f, "show_dim_waste=%d\n",    settings.show_dim_waste);
    fprintf(f, "dim_waste_lead_days=%d\n", settings.dim_waste_lead_days);
    fprintf(f, "waste_postcode=%s\n",      settings.waste_postcode);
    fprintf(f, "waste_housenr=%s\n",       settings.waste_housenr);
    fprintf(f, "vnc_enabled=%d\n",       settings.vnc_enabled);
    fprintf(f, "vnc_pass=%s\n",          settings.vnc_pass);
    fprintf(f, "weather_location=%s\n",    settings.weather_location);
    fprintf(f, "weather_location_id=%d\n", settings.weather_location_id);
    fprintf(f, "forecast_mode=%d\n",       settings.forecast_mode);
    fprintf(f, "ot_bridge_mode=%s\n",      settings.ot_bridge_mode);
    fprintf(f, "otgw_host=%s\n",           settings.otgw_host);
    fprintf(f, "otgw_user=%s\n",           settings.otgw_user);
    fprintf(f, "otgw_pass=%s\n",           settings.otgw_pass);
    fprintf(f, "mqtt_enabled=%d\n",        settings.mqtt_enabled);
    fprintf(f, "mqtt_host=%s\n",           settings.mqtt_host);
    fprintf(f, "mqtt_port=%d\n",           settings.mqtt_port);
    fprintf(f, "mqtt_user=%s\n",           settings.mqtt_user);
    fprintf(f, "mqtt_pass=%s\n",           settings.mqtt_pass);
    fprintf(f, "mqtt_topic_count=%d\n",    settings.mqtt_topic_count);
    for (int i = 0; i < settings.mqtt_topic_count && i < 8; i++)
        fprintf(f, "mqtt_topic_%d=%s\n", i, settings.mqtt_topics[i]);
    fprintf(f, "enable_p1_elec=%d\n",  settings.enable_p1_elec);
    fprintf(f, "enable_p1_water=%d\n", settings.enable_p1_water);
    fprintf(f, "enable_vent=%d\n",     settings.enable_vent);
    fprintf(f, "enable_ha=%d\n",       settings.enable_ha);
    fprintf(f, "enable_zwave=%d\n",    settings.enable_zwave);
    fprintf(f, "energy_source=%d\n",   settings.energy_source);
    fprintf(f, "boot_picker_enabled=%d\n", settings.boot_picker_enabled);
    fprintf(f, "hide_offline_tiles=%d\n",  settings.hide_offline_tiles);
    fprintf(f, "update_check_enabled=%d\n", settings.update_check_enabled);
    if (settings.tile_slot_energy[0]) fprintf(f, "tile_slot_energy=%s\n", settings.tile_slot_energy);
    if (settings.tile_slot_family[0]) fprintf(f, "tile_slot_family=%s\n", settings.tile_slot_family);
    if (settings.tile_slot_vent  [0]) fprintf(f, "tile_slot_vent=%s\n",   settings.tile_slot_vent);
    if (settings.tile_slot_water [0]) fprintf(f, "tile_slot_water=%s\n",  settings.tile_slot_water);
    fclose(f);
}
