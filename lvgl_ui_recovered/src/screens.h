#ifndef TOON_SCREENS_H
#define TOON_SCREENS_H

#include "lvgl/lvgl.h"

/* Each screen builds its widgets onto its own root lv_obj
   (created via lv_obj_create(NULL)). Use ui_push to navigate
   forward and ui_pop to go back. */

void ui_push(lv_obj_t * scr);
void ui_pop(void);

/* Per-screen builders. Each returns a freshly created screen object. */
lv_obj_t * screen_home_create(void);
/* Auto-home: drop the home screen's swipe page back to page 1 (the main page)
 * when idle. No-op if already there. */
void screen_home_reset_to_main(void);
void screen_home_force_page(int n);   /* sim/testing: jump to a home page (page 2 is gesture-only) */
void screen_home_set_ticker_speed(int spd);
void screen_home_open_news(void);
lv_obj_t * screen_thermostat_create(void);
lv_obj_t * screen_dim_create(void);
lv_obj_t * screen_settings_create(void);
lv_obj_t * screen_schedule_create(void);
lv_obj_t * screen_stats_create(void);
lv_obj_t * screen_forecast_create(void);

/* Inbox modal popup — opened from the envelope button on home. */
void screen_inbox_show(void);
/* Agenda modal — upcoming calendar events (HA + iCal). */
void screen_calendar_show(void);
/* Home-tile layout editor ("Indeling"). */
void screen_layout_editor_show(void);
lv_obj_t * screen_layout_editor_create(void);
lv_obj_t * screen_crypto_picker_create(void);   /* Settings -> Crypto coin live-search */
lv_obj_t * screen_ha_picker_create(void);       /* Settings -> HA entity picker (domain-filtered list) */
void screen_ha_picker_open(const char * domain, lv_obj_t * target_ta);  /* sets domain+target, then ui_push */
void screen_ha_picker_open_add(const char * domain, int dev_type);      /* add-mode: pick -> ha_device_add */
lv_obj_t * screen_ha_devices_create(void);      /* Settings -> Devices manager (add/remove/pin) */
lv_obj_t * screen_crypto_create(void);           /* crypto tile-tap -> price-history graphs */
lv_obj_t * screen_forecast_create(void);
lv_obj_t * screen_stats_create(void);
lv_obj_t * screen_schedule_create(void);
lv_obj_t * screen_heater_advanced_create(void);
lv_obj_t * screen_vent_remote_create(void);
lv_obj_t * screen_vent_advanced_create(void);
lv_obj_t * screen_lights_create(void);
lv_obj_t * screen_marketplace_create(void);
lv_obj_t * screen_zwave_create(void);
lv_obj_t * screen_wifi_create(void);
lv_obj_t * screen_adapters_create(void);
lv_obj_t * screen_domoticz_create(void);

/* Open the Tile-slots picker modal — used both by the Settings → Tiles
 * tile and the long-press handler on the four right-column home tiles. */
void screen_settings_open_tile_slots_modal(void);

/* Called once at boot. Loads home as root. */
void ui_init(void);

/* Idle/wake controller — call ui_mark_activity() on every touch
   (already wired in main via indev read_cb wrapper). The idle ticker
   pushes the dim screen and lowers the backlight when timeout elapses. */
void ui_mark_activity(void);
void ui_idle_tick(void);   /* call from main loop, cheap */
void ui_wake_now(void);    /* explicit wake from dim screen tap */

#endif
