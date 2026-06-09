#ifndef TOON_SETTINGS_H
#define TOON_SETTINGS_H

typedef struct {
    int auto_dim_enabled;     /* 0/1 — switch to ambient screen after idle */
    int auto_dim_seconds;     /* 5..300 — idle timeout in seconds */
    int auto_home_enabled;    /* 0/1 — return to the home screen after idle */
    int auto_home_seconds;    /* 5..600 — idle timeout before auto-returning home */
    int active_brightness;    /* 0..1000 backlight when active */
    int dim_brightness;       /* 0..1000 backlight while dimmed */
    int auto_brightness;      /* 0/1 — Toon 2: follow the LTR-303 ambient sensor */

    /* Night mode — scale the backlight to night_pct% of normal during night
     * hours. night_source: 0 = fixed LOCAL time range [night_start,night_end),
     * 1 = local sunset->sunrise (geocoded from weather_location). Times are
     * minutes-of-day (0..1439). Applies on both Toons (on Toon 2 it scales on
     * top of the light sensor). */
    int night_mode;           /* 0/1 — enable night dimming */
    int night_source;         /* 0 = fixed time range, 1 = sunset->sunrise */
    int night_start;          /* minutes-of-day — range start (default 22:00) */
    int night_end;            /* minutes-of-day — range end   (default 07:00) */
    int night_pct;            /* 1..100 — night brightness as % of day brightness */

    /* Toon 1 TSC2007 touch panel alignment. Resistive panels are mounted any-
     * which-way; these three booleans cover all 8 orientations. Tweak in
     * /mnt/data/toonui.cfg and restart toonui. Ignored on Toon 2 (eGalax cap
     * touch is already pixel-aligned). */
    int touch_swap_xy;        /* 0/1 — swap the X and Y axes (90/270° rotation) */
    int touch_invert_x;       /* 0/1 — mirror horizontally */
    int touch_invert_y;       /* 0/1 — mirror vertically */
    int temp_offset_centi;    /* -500..+500 — added to displayed indoor temp,
                                 in centi-degrees (e.g. -120 = subtract 1.2°C) */
    int show_dim_weather;     /* 0/1 — show today's weather icon on the dim screen */
    int show_dim_waste;       /* 0/1 — show next-pickup on the dim screen */
    int show_dim_bars;        /* 0/1 — usage bars (energy now / gas hourly) flanking the dim clock */
    int show_dim_metrics;     /* 0/1 — TVOC/eCO2/CH-pressure row on the dim screen */
    int dim_bars_swap;        /* 0 = energy LEFT + gas RIGHT (default); 1 = swapped */
    int dim_waste_lead_days;  /* 0..7 — only show if pickup is within this many days
                                 (0 disables; 1 = only on pickup day; 2 = day before + day of) */
    char waste_postcode[12];  /* e.g. "1671AD" — overrides the TSC waste config when set */
    char waste_housenr[8];    /* house number, e.g. "14" */
    int  waste_provider;      /* 0 = HVC (postcode), 1 = generic ICS calendar URL */
    char waste_ics_url[256];  /* full .ics calendar URL for provider 1 (prezero/cyclus/dar/…) */
    /* ToonSoftwareCollective provider plugin (run by the wastefetch helper) — the
       full stock-app mimic. "" = none (use ics_url / legacy). */
    char waste_plugin[8];     /* provider id from plugin_index.json, e.g. "6", "33" */
    char waste_icsid[48];     /* calendar number for ICSId-based providers (HVC etc.) */
    char waste_street[48];    /* for street-based providers */
    char waste_city[48];
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
    /* MQTT as the PRIMARY device-read path (curl stays a fallback). When on,
     * we subscribe to the backend's MQTT topics and feed the live device model
     * from there; the per-entity curl polls are skipped while MQTT is fresh. */
    int  mqtt_ha_reads;          /* 0/1 — subscribe to HA mqtt_statestream */
    char mqtt_ha_base[64];       /* statestream base topic, default "homeassistant" */
    int  mqtt_domoticz;          /* 0/1 — subscribe domoticz/out (Domoticz MQTT gateway) */

    /* Integration toggles — runtime on/off for optional add-ons.
     * Default is 0 ("basic" install). On first boot (no toonui.cfg key),
     * settings_load() auto-enables a flag if its config file is present:
     *   enable_vent      ← /mnt/data/vent.conf exists (non-empty)
     *   enable_ha        ← /mnt/data/ha.cfg exists (non-empty)
     * After first save the cfg keys are authoritative — user flips in
     * Settings → Integrations and toonui restarts to apply. */
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

    /* Energy/water data source per resource. Each is an ENERGY_SRC_*
     * value: 0=Off, 1=HomeAssistant, 2=HomeWizard P1, 3=Z-Wave meteradapter
     * (ZWAVE only valid for elec+gas). Independent — you can mix sources
     * (e.g. elec from P1, gas from HA, water off).
     * Migrated from the old single `energy_source` key on first load. */
    int  energy_elec_source;
    int  energy_gas_source;
    int  energy_water_source;
    char energy_elec_ha_entity[64];       /* HA sensor for consumption (W) */
    char energy_elec_prod_ha_entity[64];  /* HA sensor for solar production (W), optional */
    char energy_gas_ha_entity[64];        /* HA sensor for cumulative gas (m³) */
    char energy_water_ha_entity[64];      /* HA sensor for cumulative water (m³) */
    /* Domoticz device idx per channel (ENERGY_SRC_DOMOTICZ). The elec device's
     * Usage/UsageDeliv give power + return; gas/water use the Counter total. */
    int  energy_elec_dz_idx;
    int  energy_elec_prod_dz_idx;
    int  energy_gas_dz_idx;
    int  energy_water_dz_idx;

    /* Daily energy accumulation — persisted so dim-screen bars and the
     * statistics screen survive restarts. Date is "YYYY-MM-DD". */
    char  energy_daily_date[16];
    float energy_daily_kwh;
    float energy_daily_net_kwh;   /* signed net (negative = solar export) for Stats */
    float energy_daily_gas_m3;
    float energy_daily_water_m3;

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
    /* Update channel: 1 = beta/dev (newest release incl. prereleases, default),
     * 0 = stable/official only (/releases/latest, skips prereleases). */
    int update_channel;

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
    char tile_slot_page1[12][48];   /* page-2 slot bindings; must match TILE_SLOT_P1_N */

    /* Auto-rotate: one tile position cycles its content through a chosen set
     * of installed integrations every N seconds. rotate_members is a
     * comma-separated list of integration ids. */
    int  tile_rotate_enabled;
    int  tile_rotate_seconds;          /* 3..120 */
    char tile_rotate_members[256];     /* "id1,id2,id3" */

    /* Home Assistant — host:port (no scheme), empty = HA disabled. The two
     * presence entities + display names drive the Family tile (e.g. Life360
     * device_trackers). All empty by default so nothing personal ships. */
    char ha_host[64];
    char life360_a_entity[64];
    char life360_a_name[24];
    char life360_b_entity[64];
    char life360_b_name[24];
    /* Curtain tile — HA cover entity + two optional battery sensors. Empty
     * curtain_entity disables the curtain tile. All empty by default so no
     * personal entity ids ship in the binary. */
    char curtain_entity[64];
    char curtain_bat_a[64];
    char curtain_bat_b[64];
    /* Blinds — second HA cover entity with optional battery sensors. Same
     * shape as the curtain fields above. Empty = blinds tile hidden. */
    char blinds_entity[64];
    char blinds_bat_a[64];
    char blinds_bat_b[64];
    /* Doorbell snapshot — when the HA trigger entity (binary_sensor / input_
     * boolean / event) goes "on", fetch a snapshot of doorbell_camera and show
     * it fullscreen on the Toon for doorbell_seconds. All empty/0 = disabled. */
    char doorbell_entity[64];   /* HA entity that turns "on" when pressed */
    char doorbell_camera[64];   /* HA camera.* entity to snapshot */
    int  doorbell_seconds;      /* how long the snapshot stays up (default 30) */
    /* Optional MJPEG stream for live footage (server-transcoded, e.g. a
     * Frigate/go2rtc resized MJPEG). When set, the overlay plays this stream
     * frame-by-frame instead of a single snapshot. Empty = still snapshot. */
    char doorbell_stream_url[256];

    /* Live video tile (Toon 1 only): hardware MPEG-4/H.264 decode on the
     * i.MX27 VPU via /root/vpu/vpu_stream, shown either direct-to-fb0 (with
     * an LVGL cutout) or as an fb1 graphic-window hardware overlay. Distinct
     * from the doorbell_* snapshot feature above. Cfg keys are video_* with
     * camera_* accepted as aliases (older configs). */
    int  video_enabled;        /* 0/1 — install the Video tile + warm pipeline */
    int  video_size_pct;       /* 25..125 % of video_src_w x video_src_h */
    int  video_src_w;          /* what the sender pushes (default 640) */
    int  video_src_h;          /* (default 480) */
    int  video_x;              /* panel px, or -1 to centre */
    int  video_y;              /* panel px, or -1 to centre */
    int  video_rtp;            /* UDP RTP port for vpu_stream; 0 = legacy TCP 5000 */
    int  video_overlay;        /* 0 = fb0 + cutout; 1 = fb1 FG hardware overlay */

    /* LAN hosts for the optional integrations + healthcheck probes. All
     * empty by default so no personal network topology ships in the binary;
     * a probe/poller is simply skipped when its host is empty. "ip" or
     * "ip:port" / hostname, no scheme. */
    char p1_elec_host[64];     /* HomeWizard P1 electricity meter */
    char p1_water_host[64];    /* HomeWizard P1 water meter */
    char vent_host[64];        /* Itho NRG-WiFi ventilation unit */
    char opnsense_host[64];    /* router, for the "router offline" healthcheck */

    /* Auto-update — when on, the update checker installs a newer release
     * automatically around auto_update_hour (local), with a Toon banner. */
    int  auto_update_enabled;
    int  auto_update_hour;     /* 0..23, default 2 (≈02:00) */

    /* Newsreader — built-in RSS ticker above the forecast on the home screen.
     * Tapping a headline shows a QR to open the article on your phone. */
    int  news_enabled;
    char news_rss_url[640];    /* one or more RSS feed URLs, newline-separated */
    int  news_scroll_speed;   /* ticker scroll speed, px/sec (0 → default 30) */

    /* Calendar — upcoming-events agenda. Two optional sources, merged + sorted:
     *  - calendar_ha_entity: a Home Assistant calendar.* entity (REST, Bearer)
     *  - calendar_ics_url:   a public/private .ics URL (CalDAV/Google/etc.)
     * Either or both; empty disables that source. */
    int  calendar_enabled;
    char calendar_ha_entity[64];
    char calendar_ics_url[256];

    /* Custom home-tile layout — when 1, the home screen places/sizes/hides the
     * built-in tiles from the grid layout (layout.c / toonui_layout.cfg) edited
     * in the "Indeling" editor. When 0 (default) the original hardcoded layout
     * is used unchanged. */
    int  custom_layout_enabled;

    /* Active layout preset name (empty = the default toonui_layout.cfg). Named
     * presets live at <data>/toonui_layout_<name>.cfg; switching writes the name
     * here so it's restored on the next boot. */
    char active_layout[24];

    /* Client mode — this Toon is a "slave" that mirrors a master Toon over
     * its PWA HTTP API instead of talking to local HCB daemons. When 1, all
     * local integrations (BoxTalk, P1, meteradapter, weather, waste, vent,
     * HA, Domoticz, MQTT) are skipped; client_link.c streams the master's
     * /api/state/stream into toon_state and routes setpoint/program/curtain
     * control back to the master. The slave connects ONLY to master_host. */
    int  client_mode;
    char master_host[64];   /* master Toon IP/host, no scheme (PWA port 10081) */

    /* PWA web-interface login (browser-facing).
     *
     * Enabled by default — a freshly-installed Toon shouldn't expose the full
     * thermostat + curtain + schedule UI on the LAN with no gate. First visit
     * after enable is redirected to /set-password to mint a password; until
     * that's done the rest of the routes return 302 → /set-password (no
     * silent "anyone in" window).
     *
     * pwa_login_pass is plaintext to match the rest of the cfg (mqtt_pass,
     * vnc_pass). The threat model is "stop the neighbour or kid from poking
     * the UI", not "an attacker reading /mnt/data/toonui.cfg" — root on the
     * Toon already gives them everything. */
    int  pwa_login_enabled;   /* 0/1 — gate the web UI behind login (default 1) */
    char pwa_login_user[32];  /* default "admin" */
    char pwa_login_pass[32];  /* default "" — empty forces /set-password */

    /* On-device PIN — gates anything the user might tap by accident or that
     * a household member shouldn't change unsupervised:
     *  - Opening Settings (any setting change)
     *  - Comfort/Home/Sleep/Away preset taps (home + dim screens)
     *  - Temperature +/- setpoint adjustments
     *  - Schedule mode toggle (auto ↔ manual)
     *
     * Default OFF — opt-in. When pin_enabled but pin_code is empty, the
     * gate behaves as if disabled (no point asking for a PIN nobody set).
     * Stored plaintext (UI PIN, not a security boundary). */
    int  pin_enabled;
    char pin_code[8];         /* 4-6 numeric chars; empty = effectively off */

	int  lang;		/* 0 = English, 1 = Nederlands — default 0 */
} settings_t;

#define FORECAST_AUTO   0
#define FORECAST_HOURLY 1
#define FORECAST_DAILY  2

/* Per-resource energy/water source selectors. */
#define ENERGY_SRC_OFF       0
#define ENERGY_SRC_HA        1
#define ENERGY_SRC_HW_P1     2
#define ENERGY_SRC_ZWAVE     3
#define ENERGY_SRC_DOMOTICZ  4
#define ENERGY_SRC_MAX       4

/* Display-side adjusted indoor temperature: raw + settings.temp_offset_centi/100 */
float display_indoor_temp(float raw);

extern settings_t settings;

void settings_load(void);
void settings_save(void);

/* Strip control chars (<0x20) from a string in place — keeps user-entered
 * values from corrupting the cfg or injecting newlines. */
void settings_sanitize_str(char * s);

#endif
