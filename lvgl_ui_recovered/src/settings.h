#ifndef TOON_SETTINGS_H
#define TOON_SETTINGS_H

typedef struct {
    int auto_dim_enabled;     /* 0/1 — switch to ambient screen after idle */
    int auto_dim_seconds;     /* 5..300 — idle timeout in seconds */
    int auto_home_enabled;    /* 0/1 — return to the home screen after idle */
    int auto_home_seconds;    /* 5..600 — idle timeout before auto-returning home */
    int active_brightness;    /* 0..1000 backlight when active */
    int dim_brightness;       /* 0..1000 backlight while dimmed */
    int temp_offset_centi;    /* -500..+500 — added to displayed indoor temp,
                                 in centi-degrees (e.g. -120 = subtract 1.2°C) */
    int show_dim_weather;     /* 0/1 — show today's weather icon on the dim screen */
    int show_dim_waste;       /* 0/1 — show next-pickup on the dim screen */
    int dim_waste_lead_days;  /* 0..7 — only show if pickup is within this many days
                                 (0 disables; 1 = only on pickup day; 2 = day before + day of) */
    char waste_postcode[12];  /* e.g. "1671AD" — overrides the TSC waste config when set */
    char waste_housenr[8];    /* house number, e.g. "14" */
    int  waste_provider;      /* 0 = HVC (postcode), 1 = generic ICS calendar URL */
    char waste_ics_url[256];  /* full .ics calendar URL for provider 1 (prezero/cyclus/dar/…) */
    int  vnc_enabled;         /* 0/1 — run the x11vnc remote-control server */
    char vnc_pass[16];        /* VNC password (plaintext, max 8 effective chars;
                                 empty = no password). No spaces. */
    char weather_location[32];   /* Free-text location name shown in the UI
                                    (default "Medemblik"). Cosmetic for now. */
    int  weather_location_id;    /* Buienradar wikiCode for hourly-forecast
                                    fetch — default 6249 ≈ Berkhout (KNMI
                                    station closest to Medemblik). 0 disables
                                    the hourly fetch. */
    int  forecast_mode;          /* 0 = auto (hourly if available, else daily)
                                    1 = force hourly
                                    2 = force daily */
    /* OT bridge / OTGW configuration — three-way selector matching
       ot_mode_switch.sh:
         "off"      — bare keteladapter on /dev/ttymxc0, no bridge.
                      Bulletproof fallback; PWA boiler card not lit.
         "proxy"    — quby_bridge in proxy mode: shuttles bytes happ↔
                      keteladapter 1:1 and republishes BoilerInfo on
                      BoxTalk. Default. Original heat path preserved
                      plus full PWA boiler card.
         "wireless" — quby_bridge in active mode + OTGW GW=2: bridge
                      fakes Quby responses to happ and forwards CS/CH/
                      etc to OTGW via port 25238. Keteladapter not in
                      loop. Use as fallback if keteladapter dies.
       Legacy values "keteladapter"→proxy, "otgw"→wireless are migrated
       on load. */
    char ot_bridge_mode[16];     /* "off" | "proxy" | "wireless" — default proxy */
    char otgw_host[64];          /* "192.168.99.21" — IP or hostname (no scheme) */
    char otgw_user[32];          /* HTTP Basic-Auth user, empty = no auth */
    char otgw_pass[64];          /* HTTP Basic-Auth pass */

    /* MQTT broker for the banner subscriber. Empty host disables subscriber.
     * Topics: filter strings the subscriber listens on; each new PUBLISH
     * triggers a banner with topic+payload. Cap of 8 keeps the settings
     * file small and the topic-select UI digestible. */
    int  mqtt_enabled;           /* 0/1 — run the MQTT banner subscriber */
    char mqtt_host[64];
    int  mqtt_port;              /* 1883 default */
    char mqtt_user[32];
    char mqtt_pass[64];
    char mqtt_topics[8][96];
    int  mqtt_topic_count;

    /* Integration toggles — runtime on/off for optional add-ons.
     * Default is 0 ("basic" install). On first boot (no toonui.cfg key),
     * settings_load() auto-enables a flag if its config file is present:
     *   enable_p1_elec   ← /mnt/data/p1bridge.conf exists
     *   enable_p1_water  ← /mnt/data/p1bridge.conf has a .115 line
     *   enable_vent      ← /mnt/data/vent.conf exists (non-empty)
     *   enable_ha        ← /mnt/data/ha.cfg exists (non-empty)
     * After first save the cfg keys are authoritative — user flips in
     * Settings → Integrations and toonui restarts to apply. */
    int enable_p1_elec;
    int enable_p1_water;
    int enable_vent;
    int enable_ha;

    /* Domoticz — alternative to HA for lights + blinds (for users who run
     * Domoticz). Talks to its JSON API at domoticz_host ("ip:port"); optional
     * basic-auth user/pass. */
    int  enable_domoticz;
    char domoticz_host[64];
    char domoticz_user[32];
    char domoticz_pass[48];

    /* Z-Wave control — gates the Settings → Z-Wave management screen's
     * write actions (include/exclude/on-off/rename). Default 0. Flipping it
     * on also writes <supportControl>1</supportControl> into the stock
     * config_hdrv_zwave.xml and restarts hdrv_zwave so the built-in
     * controller's advanced control is genuinely enabled. See
     * reference_toon_zwave_api memory for the HTTP API. */
    int enable_zwave;

    /* Energy data source for the Energy tile.
     *   0 = meteradapter — the Toon's own built-in smart-meter reading via
     *       happ_pwrusage (the official/stock path). Default.
     *   1 = p1 — HomeWizard P1 over the LAN (the homewizard.c poller).
     * Per-device: a Toon with a working meteradapter uses 0; a setup that
     * reads the meter via a HomeWizard P1 instead picks 1. */
    int energy_source;

    /* Boot-picker — when 1, the launcher-spawned `toonui --bootpick`
     * shows a 10 s "freetoon vs stock qt-gui" picker before dispatching.
     * When 0 the picker short-circuits and the launcher boots straight
     * into whatever /mnt/data/ui_choice selects. Default ON so a new
     * user can always escape to the stock UI by tapping a button. */
    int boot_picker_enabled;

    /* Update check — poll GitHub Releases every 6 h, banner on the
     * home tile when a newer freetoon-lvgl release is available.
     * Default 1; flip to 0 in toonui.cfg to disable the background
     * fetch entirely (no network call, no banner). */
    int update_check_enabled;

    /* Hide offline tiles on the home screen. Default 0 → tiles for
     * disabled / disconnected integrations stay visible with an
     * "offline" placeholder ("P1 offline" / "HA offline" / "Itho
     * offline"). 1 → those tiles get LV_OBJ_FLAG_HIDDEN so the home
     * screen drops the corresponding rectangles entirely. The user
     * picks one or the other in Settings → Integrations. */
    int hide_offline_tiles;

    /* Tile-reassignment slots (Phase 2 of marketplace). Each holds the id
     * of an installed integration whose tile should replace the matching
     * right-column home tile. Empty = built-in behaviour. See
     * tile_slots.h for the dispatch model. 48 chars matches INTEG_ID_MAX. */
    char tile_slot_energy[48];
    char tile_slot_family[48];
    char tile_slot_vent[48];
    char tile_slot_water[48];
    /* Page-2 (swipe) tile slots — 4 independent assignable positions that
     * render whatever marketplace integration you bind to them. Empty = the
     * "tap to assign" placeholder. */
    char tile_slot_page1[4][48];

    /* Auto-rotate: one tile position cycles its content through a chosen set
     * of installed integrations every N seconds. rotate_members is a
     * comma-separated list of integration ids. */
    int  tile_rotate_enabled;
    int  tile_rotate_seconds;          /* 3..120 */
    char tile_rotate_members[256];     /* "id1,id2,id3" */

    /* Client mode — this Toon is a "slave" that mirrors a master Toon over
     * its PWA HTTP API instead of talking to local HCB daemons. When 1, all
     * local integrations (BoxTalk, P1, meteradapter, weather, waste, vent,
     * HA, Domoticz, MQTT) are skipped; client_link.c streams the master's
     * /api/state/stream into toon_state and routes setpoint/program/curtain
     * control back to the master. The slave connects ONLY to master_host. */
    int  client_mode;
    char master_host[64];   /* master Toon IP/host, no scheme (PWA port 10081) */
} settings_t;

#define FORECAST_AUTO   0
#define FORECAST_HOURLY 1
#define FORECAST_DAILY  2

/* Display-side adjusted indoor temperature: raw + settings.temp_offset_centi/100 */
float display_indoor_temp(float raw);

extern settings_t settings;

void settings_load(void);
void settings_save(void);

#endif
