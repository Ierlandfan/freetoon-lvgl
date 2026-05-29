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
    .show_dim_bars     = 1,
    .dim_bars_swap     = 0,
    .dim_waste_lead_days = 3,
    .vnc_enabled       = 0,
    .vnc_pass          = "",
    .weather_location    = "De Bilt",
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
    /* OT *bridge* mode (off/proxy/wireless) — the quby_bridge/OTGW proxy, NOT
     * the keteladapter's OpenTherm link (that's boiler_type, read from the Toon
     * and unaffected by this). Default off: the proxy ships disabled, so a
     * fresh install shouldn't claim "proxy" before the user enables it. */
    .ot_bridge_mode      = "off",
    .otgw_host           = "",            /* set per-device in Settings → OT Bridge */
    .otgw_user           = "",
    .otgw_pass           = "",
    /* MQTT is opt-in and unconfigured by default — no personal broker baked
     * into the public binary. Enable + fill host/user in Settings → MQTT. */
    .mqtt_enabled        = 0,
    .mqtt_host           = "",
    .mqtt_port           = 1883,
    .mqtt_user           = "",
    .mqtt_pass           = "",
    .mqtt_topics         = {"home/packages/banner", "home/packages/state"},
    .mqtt_topic_count    = 2,
    /* Integrations default OFF — see settings.h for the on-first-boot
     * auto-enable rule based on existing config files. */
    .enable_p1_elec      = 0,
    .enable_p1_water     = 0,
    .enable_vent         = 0,
    .enable_ha           = 0,
    .enable_domoticz     = 0,
    .tile_rotate_enabled = 0,
    .tile_rotate_seconds = 10,
    .news_enabled        = 0,
    .news_rss_url        = "https://feeds.nos.nl/nosnieuwsalgemeen",
    .news_scroll_speed   = 30,
    .calendar_enabled    = 0,
    .calendar_ha_entity  = "",
    .calendar_ics_url    = "",
    .custom_layout_enabled = 0,
    .auto_update_enabled = 0,
    .auto_update_hour    = 2,
    .ha_host             = "",
    .life360_a_entity    = "",
    .life360_a_name      = "",
    .life360_b_entity    = "",
    .life360_b_name      = "",
    .curtain_entity      = "",
    .curtain_bat_a       = "",
    .curtain_bat_b       = "",
    .doorbell_entity     = "",
    .doorbell_camera     = "",
    .doorbell_seconds    = 30,
    .doorbell_stream_url = "",
    .p1_elec_host        = "",
    .p1_water_host       = "",
    .vent_host           = "",
    .opnsense_host       = "",
    .client_mode         = 0,
    .master_host         = "",
    .boot_picker_enabled = 1,
    .hide_offline_tiles  = 0,
    .update_check_enabled = 1,
    .update_channel       = 1,   /* beta/dev by default */
    .energy_source       = 0,   /* meteradapter (official) by default */

    /* Toon 1 panel mounting orientation — TSC2007 reports Y flipped vs the
     * framebuffer on EVERY Toon 1 (not a per-device tweak). Default this
     * to 1 in the TOON1 build so a fresh install lands with usable touch
     * on day one, without the customer SSH-editing the cfg. Toon 2's
     * capacitive panel needs no transform. */
#ifdef TOON1
    .touch_invert_y      = 1,
#endif

    /* Web login on by default; password starts empty → first visit to /
     * 302s to /set-password to force the user to mint one. */
    .pwa_login_enabled   = 1,
    .pwa_login_user      = "admin",
    .pwa_login_pass      = "",

    /* PIN off by default. */
    .pin_enabled         = 0,
    .pin_code            = "",
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
        else if (strcmp(k, "auto_brightness")   == 0) settings.auto_brightness   = iv;
        else if (strcmp(k, "touch_swap_xy")     == 0) settings.touch_swap_xy     = iv;
        else if (strcmp(k, "touch_invert_x")    == 0) settings.touch_invert_x    = iv;
        else if (strcmp(k, "touch_invert_y")    == 0) settings.touch_invert_y    = iv;
        else if (strcmp(k, "temp_offset_centi") == 0) settings.temp_offset_centi = iv;
        else if (strcmp(k, "show_dim_weather")  == 0) settings.show_dim_weather  = iv;
        else if (strcmp(k, "show_dim_waste")    == 0) settings.show_dim_waste    = iv;
        else if (strcmp(k, "show_dim_bars")     == 0) settings.show_dim_bars     = iv;
        else if (strcmp(k, "dim_bars_swap")     == 0) settings.dim_bars_swap     = iv;
        else if (strcmp(k, "dim_waste_lead_days") == 0) settings.dim_waste_lead_days = iv;
        else if (strcmp(k, "waste_postcode")    == 0)
            snprintf(settings.waste_postcode, sizeof settings.waste_postcode, "%s", v);
        else if (strcmp(k, "waste_housenr")     == 0)
            snprintf(settings.waste_housenr, sizeof settings.waste_housenr, "%s", v);
        else if (strcmp(k, "waste_provider")    == 0) settings.waste_provider = iv;
        else if (strcmp(k, "waste_ics_url")     == 0)
            snprintf(settings.waste_ics_url, sizeof settings.waste_ics_url, "%s", v);
        else if (strcmp(k, "waste_plugin")      == 0)
            snprintf(settings.waste_plugin, sizeof settings.waste_plugin, "%s", v);
        else if (strcmp(k, "waste_icsid")       == 0)
            snprintf(settings.waste_icsid, sizeof settings.waste_icsid, "%s", v);
        else if (strcmp(k, "waste_street")      == 0)
            snprintf(settings.waste_street, sizeof settings.waste_street, "%s", v);
        else if (strcmp(k, "waste_city")        == 0)
            snprintf(settings.waste_city, sizeof settings.waste_city, "%s", v);
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
        else if (strcmp(k, "enable_domoticz") == 0) settings.enable_domoticz = iv;
        else if (strcmp(k, "domoticz_host")   == 0)
            snprintf(settings.domoticz_host, sizeof settings.domoticz_host, "%s", v);
        else if (strcmp(k, "client_mode")     == 0) settings.client_mode = iv;
        else if (strcmp(k, "master_host")     == 0)
            snprintf(settings.master_host, sizeof settings.master_host, "%s", v);
        else if (strncmp(k, "tile_slot_p1_", 13) == 0) {
            int n = atoi(k + 13);                  /* tile_slot_p1_0 .. _11 */
            if (n >= 0 && n < 12) snprintf(settings.tile_slot_page1[n], 48, "%s", v);
        }
        else if (strcmp(k, "tile_rotate_enabled") == 0) settings.tile_rotate_enabled = iv;
        else if (strcmp(k, "tile_rotate_seconds") == 0) settings.tile_rotate_seconds = iv < 3 ? 3 : (iv > 120 ? 120 : iv);
        else if (strcmp(k, "tile_rotate_members") == 0)
            snprintf(settings.tile_rotate_members, sizeof settings.tile_rotate_members, "%s", v);
        else if (strcmp(k, "news_enabled")    == 0) settings.news_enabled = iv;
        else if (strcmp(k, "news_rss_url")    == 0) {
            snprintf(settings.news_rss_url, sizeof settings.news_rss_url, "%s", v);
            /* Feeds are newline-separated in memory but tab-separated on disk
             * (a literal '\n' would split this key=value across two lines and
             * drop every feed after the first). Decode tabs back to newlines. */
            for (char * p = settings.news_rss_url; *p; p++) if (*p == '\t') *p = '\n';
        }
        else if (strcmp(k, "news_scroll_speed") == 0) settings.news_scroll_speed = (iv > 0 && iv < 30) ? 30 : (iv > 150 ? 150 : iv);
        else if (strcmp(k, "calendar_enabled")   == 0) settings.calendar_enabled = iv;
        else if (strcmp(k, "calendar_ha_entity") == 0) snprintf(settings.calendar_ha_entity, sizeof settings.calendar_ha_entity, "%s", v);
        else if (strcmp(k, "calendar_ics_url")   == 0) snprintf(settings.calendar_ics_url, sizeof settings.calendar_ics_url, "%s", v);
        else if (strcmp(k, "custom_layout_enabled") == 0) settings.custom_layout_enabled = iv;
        else if (strcmp(k, "active_layout")       == 0) snprintf(settings.active_layout, sizeof settings.active_layout, "%s", v);
        else if (strcmp(k, "auto_update_enabled") == 0) settings.auto_update_enabled = iv;
        else if (strcmp(k, "auto_update_hour")    == 0) settings.auto_update_hour = (iv < 0 || iv > 23) ? 2 : iv;
        else if (strcmp(k, "ha_host")          == 0) snprintf(settings.ha_host, sizeof settings.ha_host, "%s", v);
        else if (strcmp(k, "life360_a_entity") == 0) snprintf(settings.life360_a_entity, sizeof settings.life360_a_entity, "%s", v);
        else if (strcmp(k, "life360_a_name")   == 0) snprintf(settings.life360_a_name, sizeof settings.life360_a_name, "%s", v);
        else if (strcmp(k, "life360_b_entity") == 0) snprintf(settings.life360_b_entity, sizeof settings.life360_b_entity, "%s", v);
        else if (strcmp(k, "life360_b_name")   == 0) snprintf(settings.life360_b_name, sizeof settings.life360_b_name, "%s", v);
        else if (strcmp(k, "curtain_entity")   == 0) snprintf(settings.curtain_entity, sizeof settings.curtain_entity, "%s", v);
        else if (strcmp(k, "curtain_bat_a")    == 0) snprintf(settings.curtain_bat_a, sizeof settings.curtain_bat_a, "%s", v);
        else if (strcmp(k, "curtain_bat_b")    == 0) snprintf(settings.curtain_bat_b, sizeof settings.curtain_bat_b, "%s", v);
        else if (strcmp(k, "doorbell_entity")  == 0) snprintf(settings.doorbell_entity, sizeof settings.doorbell_entity, "%s", v);
        else if (strcmp(k, "doorbell_camera")  == 0) snprintf(settings.doorbell_camera, sizeof settings.doorbell_camera, "%s", v);
        else if (strcmp(k, "doorbell_seconds") == 0) settings.doorbell_seconds = (iv < 3 || iv > 300) ? 30 : iv;
        else if (strcmp(k, "doorbell_stream_url") == 0) snprintf(settings.doorbell_stream_url, sizeof settings.doorbell_stream_url, "%s", v);
        else if (strcmp(k, "p1_elec_host")     == 0) snprintf(settings.p1_elec_host, sizeof settings.p1_elec_host, "%s", v);
        else if (strcmp(k, "p1_water_host")    == 0) snprintf(settings.p1_water_host, sizeof settings.p1_water_host, "%s", v);
        else if (strcmp(k, "vent_host")        == 0) snprintf(settings.vent_host, sizeof settings.vent_host, "%s", v);
        else if (strcmp(k, "opnsense_host")    == 0) snprintf(settings.opnsense_host, sizeof settings.opnsense_host, "%s", v);
        else if (strcmp(k, "domoticz_user")   == 0)
            snprintf(settings.domoticz_user, sizeof settings.domoticz_user, "%s", v);
        else if (strcmp(k, "domoticz_pass")   == 0)
            snprintf(settings.domoticz_pass, sizeof settings.domoticz_pass, "%s", v);
        else if (strcmp(k, "enable_zwave")    == 0) settings.enable_zwave = iv;
        else if (strcmp(k, "energy_source")   == 0) settings.energy_source = iv;
        else if (strcmp(k, "boot_picker_enabled") == 0) settings.boot_picker_enabled = iv;
        else if (strcmp(k, "hide_offline_tiles")  == 0) settings.hide_offline_tiles = iv;
        else if (strcmp(k, "update_check_enabled") == 0) settings.update_check_enabled = iv;
        else if (strcmp(k, "update_channel")       == 0) settings.update_channel = !!iv;
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
        else if (strcmp(k, "pwa_login_enabled") == 0) settings.pwa_login_enabled = iv;
        else if (strcmp(k, "pwa_login_user")    == 0)
            snprintf(settings.pwa_login_user, sizeof settings.pwa_login_user, "%s", v);
        else if (strcmp(k, "pwa_login_pass")    == 0)
            snprintf(settings.pwa_login_pass, sizeof settings.pwa_login_pass, "%s", v);
        else if (strcmp(k, "pin_enabled")       == 0) settings.pin_enabled = iv;
        else if (strcmp(k, "pin_code")          == 0)
            snprintf(settings.pin_code, sizeof settings.pin_code, "%s", v);
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
        settings.enable_p1_water = legacy ? 1 : (settings.p1_water_host[0] != 0);
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

    /* First boot: write the cfg now so users have an editable file even
     * before they ever open Settings. Especially important on Toon 1 —
     * if the resistive panel is mis-mapped, the GUI is unreachable and
     * the only fix is to set touch_invert_y / touch_swap_xy in the cfg
     * over SSH. With no file there, the user has to invent the key names
     * from memory. autodetect + mqtt migration run before this so the
     * persisted values reflect the right post-detect defaults. */
    if (!cfg_existed) {
        settings_save();
    }
}

/* Strip control characters (< 0x20: newline, CR, tab, etc.) in place. Keeps
 * every string value on a single, well-formed cfg line — a raw newline in a
 * value would otherwise split the key=value line and corrupt the config (the
 * bug that dropped news feeds). Safe for hosts/URLs/names/entities; none of
 * them legitimately contain control characters. */
void settings_sanitize_str(char * s) {
    if (!s) return;
    char * w = s;
    for (char * r = s; *r; r++)
        if ((unsigned char)*r >= 0x20) *w++ = *r;
    *w = 0;
}

/* Sanitize every free-text setting before persisting, regardless of whether it
 * came from the LVGL textareas, the PWA, or a client-mode mirror. news_rss_url
 * is excluded — it is newline-separated in memory and tab-encoded on write. */
static void sanitize_all_strings(void) {
    char * fields[] = {
        settings.waste_postcode, settings.waste_housenr, settings.waste_ics_url,
        settings.waste_plugin, settings.waste_icsid, settings.waste_street,
        settings.waste_city, settings.vnc_pass, settings.weather_location,
        settings.ot_bridge_mode, settings.otgw_host, settings.otgw_user,
        settings.otgw_pass, settings.mqtt_host, settings.mqtt_user,
        settings.mqtt_pass, settings.domoticz_host, settings.master_host,
        settings.tile_rotate_members, settings.calendar_ha_entity,
        settings.calendar_ics_url, settings.ha_host, settings.curtain_entity,
        settings.curtain_bat_a, settings.curtain_bat_b, settings.doorbell_entity,
        settings.doorbell_camera, settings.doorbell_stream_url, settings.vent_host,
        settings.opnsense_host, settings.domoticz_user, settings.domoticz_pass,
        settings.tile_slot_energy, settings.tile_slot_family, settings.tile_slot_vent,
        settings.tile_slot_water, settings.life360_a_entity, settings.life360_a_name,
        settings.life360_b_entity, settings.life360_b_name,
    };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++)
        settings_sanitize_str(fields[i]);
}

void settings_save(void) {
    sanitize_all_strings();
    FILE * f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "auto_dim_enabled=%d\n",  settings.auto_dim_enabled);
    fprintf(f, "auto_dim_seconds=%d\n",  settings.auto_dim_seconds);
    fprintf(f, "auto_home_enabled=%d\n", settings.auto_home_enabled);
    fprintf(f, "auto_home_seconds=%d\n", settings.auto_home_seconds);
    fprintf(f, "active_brightness=%d\n", settings.active_brightness);
    fprintf(f, "dim_brightness=%d\n",    settings.dim_brightness);
    fprintf(f, "auto_brightness=%d\n",   settings.auto_brightness);
    fprintf(f, "touch_swap_xy=%d\n",     settings.touch_swap_xy);
    fprintf(f, "touch_invert_x=%d\n",    settings.touch_invert_x);
    fprintf(f, "touch_invert_y=%d\n",    settings.touch_invert_y);
    fprintf(f, "temp_offset_centi=%d\n", settings.temp_offset_centi);
    fprintf(f, "show_dim_weather=%d\n",  settings.show_dim_weather);
    fprintf(f, "show_dim_waste=%d\n",    settings.show_dim_waste);
    fprintf(f, "show_dim_bars=%d\n",     settings.show_dim_bars);
    fprintf(f, "dim_bars_swap=%d\n",     settings.dim_bars_swap);
    fprintf(f, "dim_waste_lead_days=%d\n", settings.dim_waste_lead_days);
    fprintf(f, "waste_postcode=%s\n",      settings.waste_postcode);
    fprintf(f, "waste_housenr=%s\n",       settings.waste_housenr);
    fprintf(f, "waste_provider=%d\n",      settings.waste_provider);
    fprintf(f, "waste_ics_url=%s\n",       settings.waste_ics_url);
    fprintf(f, "waste_plugin=%s\n",        settings.waste_plugin);
    fprintf(f, "waste_icsid=%s\n",         settings.waste_icsid);
    fprintf(f, "waste_street=%s\n",        settings.waste_street);
    fprintf(f, "waste_city=%s\n",          settings.waste_city);
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
    fprintf(f, "enable_domoticz=%d\n", settings.enable_domoticz);
    fprintf(f, "domoticz_host=%s\n",   settings.domoticz_host);
    fprintf(f, "client_mode=%d\n",     settings.client_mode);
    fprintf(f, "master_host=%s\n",     settings.master_host);
    for (int i = 0; i < 12; i++)
        fprintf(f, "tile_slot_p1_%d=%s\n", i, settings.tile_slot_page1[i]);
    fprintf(f, "tile_rotate_enabled=%d\n", settings.tile_rotate_enabled);
    fprintf(f, "tile_rotate_seconds=%d\n", settings.tile_rotate_seconds);
    fprintf(f, "tile_rotate_members=%s\n", settings.tile_rotate_members);
    fprintf(f, "news_enabled=%d\n", settings.news_enabled);
    /* Encode the newline-separated feed list as tabs so the whole list stays on
     * one key=value line (a raw '\n' would split it and drop later feeds).
     * Decoded back to newlines on load. URLs never contain tabs. */
    {
        char news_enc[sizeof settings.news_rss_url];
        snprintf(news_enc, sizeof news_enc, "%s", settings.news_rss_url);
        for (char * p = news_enc; *p; p++) if (*p == '\n') *p = '\t';
        fprintf(f, "news_rss_url=%s\n", news_enc);
    }
    fprintf(f, "calendar_enabled=%d\n", settings.calendar_enabled);
    fprintf(f, "calendar_ha_entity=%s\n", settings.calendar_ha_entity);
    fprintf(f, "calendar_ics_url=%s\n", settings.calendar_ics_url);
    fprintf(f, "custom_layout_enabled=%d\n", settings.custom_layout_enabled);
    fprintf(f, "active_layout=%s\n", settings.active_layout);
    fprintf(f, "news_scroll_speed=%d\n", settings.news_scroll_speed);
    fprintf(f, "auto_update_enabled=%d\n", settings.auto_update_enabled);
    fprintf(f, "auto_update_hour=%d\n", settings.auto_update_hour);
    fprintf(f, "ha_host=%s\n", settings.ha_host);
    fprintf(f, "life360_a_entity=%s\n", settings.life360_a_entity);
    fprintf(f, "life360_a_name=%s\n", settings.life360_a_name);
    fprintf(f, "life360_b_entity=%s\n", settings.life360_b_entity);
    fprintf(f, "life360_b_name=%s\n", settings.life360_b_name);
    fprintf(f, "curtain_entity=%s\n", settings.curtain_entity);
    fprintf(f, "curtain_bat_a=%s\n", settings.curtain_bat_a);
    fprintf(f, "curtain_bat_b=%s\n", settings.curtain_bat_b);
    fprintf(f, "doorbell_entity=%s\n", settings.doorbell_entity);
    fprintf(f, "doorbell_camera=%s\n", settings.doorbell_camera);
    fprintf(f, "doorbell_seconds=%d\n", settings.doorbell_seconds);
    fprintf(f, "doorbell_stream_url=%s\n", settings.doorbell_stream_url);
    fprintf(f, "p1_elec_host=%s\n", settings.p1_elec_host);
    fprintf(f, "p1_water_host=%s\n", settings.p1_water_host);
    fprintf(f, "vent_host=%s\n", settings.vent_host);
    fprintf(f, "opnsense_host=%s\n", settings.opnsense_host);
    fprintf(f, "domoticz_user=%s\n",   settings.domoticz_user);
    fprintf(f, "domoticz_pass=%s\n",   settings.domoticz_pass);
    fprintf(f, "enable_zwave=%d\n",    settings.enable_zwave);
    fprintf(f, "energy_source=%d\n",   settings.energy_source);
    fprintf(f, "boot_picker_enabled=%d\n", settings.boot_picker_enabled);
    fprintf(f, "hide_offline_tiles=%d\n",  settings.hide_offline_tiles);
    fprintf(f, "update_check_enabled=%d\n", settings.update_check_enabled);
    fprintf(f, "update_channel=%d\n", settings.update_channel);
    if (settings.tile_slot_energy[0]) fprintf(f, "tile_slot_energy=%s\n", settings.tile_slot_energy);
    if (settings.tile_slot_family[0]) fprintf(f, "tile_slot_family=%s\n", settings.tile_slot_family);
    if (settings.tile_slot_vent  [0]) fprintf(f, "tile_slot_vent=%s\n",   settings.tile_slot_vent);
    if (settings.tile_slot_water [0]) fprintf(f, "tile_slot_water=%s\n",  settings.tile_slot_water);
    fprintf(f, "pwa_login_enabled=%d\n", settings.pwa_login_enabled);
    fprintf(f, "pwa_login_user=%s\n",    settings.pwa_login_user);
    fprintf(f, "pwa_login_pass=%s\n",    settings.pwa_login_pass);
    fprintf(f, "pin_enabled=%d\n",       settings.pin_enabled);
    fprintf(f, "pin_code=%s\n",          settings.pin_code);
    fclose(f);
}
