/*
 * i18n.h — Lightweight compile-time translation engine for the Toon thermostat.
 *
 * All UI strings are stored in indexed static arrays (English + Dutch) and
 * resolved via i18n(key) or the TR(key) macro, which is O(1) and zero-heap.
 * settings.lang selects the active language; missing Dutch strings fall back
 * to English so the UI is never broken mid-migration.
 */
#ifndef TOON_I18N_H
#define TOON_I18N_H

enum i18n_key {
    /* Settings — main screen */
    I18N_SETTINGS,
    I18N_BACK,

    /* Settings — category names */
    I18N_CLIMATE_HEATING,
    I18N_SMART_HOME,
    I18N_DISPLAY_LAYOUT,
    I18N_SYSTEM_NETWORK,
    I18N_APPS_EXTENSIONS,

    /* Settings — category item titles */
    I18N_HEATING_CONFIG,
    I18N_BOILER_ADAPTER,
    I18N_OT_BRIDGE,
    I18N_ZWAVE_DEVICES,
    I18N_PRESETS,
    I18N_VENTILATION,
    I18N_ENERGY_SOURCES,
    I18N_HA_DEVICES,
    I18N_DOMOTICZ,
    I18N_MQTT_BROKER,
    I18N_CALENDAR,
    I18N_WASTE_COLLECTION,
    I18N_WEATHER,
    I18N_DISPLAY,
    I18N_TILE_SLOTS,
    I18N_LAYOUT_EDITOR,
    I18N_AUTO_ROTATE,
    I18N_CLEAN,
    I18N_DIM_CONTENT,
    I18N_WIFI,
    I18N_WEB_ACCESS,
    I18N_PIN_CODE,
    I18N_CLIENT_MODE,
    I18N_UI_MODE,
    I18N_UPDATE_CHECK,
    I18N_ABOUT,
    I18N_RESTART_UI,
    I18N_MARKETPLACE,
    I18N_CRYPTO,
    I18N_NEWSREADER,

    /* Settings — item descriptions */
    I18N_HEATING_CONFIG_DESC,
    I18N_BOILER_ADAPTER_DESC,
    I18N_OT_BRIDGE_DESC,
    I18N_ZWAVE_DEVICES_DESC,
    I18N_PRESETS_DESC,
    I18N_VENTILATION_DESC,
    I18N_ENERGY_SOURCES_DESC,
    I18N_HA_DEVICES_DESC,
    I18N_DOMOTICZ_DESC,
    I18N_MQTT_BROKER_DESC,
    I18N_CALENDAR_DESC,
    I18N_WASTE_COLLECTION_DESC,
    I18N_WEATHER_DESC,
    I18N_DISPLAY_DESC,
    I18N_TILE_SLOTS_DESC,
    I18N_LAYOUT_EDITOR_DESC,
    I18N_AUTO_ROTATE_DESC,
    I18N_CLEAN_DESC,
    I18N_DIM_CONTENT_DESC,
    I18N_WIFI_DESC,
    I18N_WEB_ACCESS_DESC,
    I18N_PIN_CODE_DESC,
    I18N_CLIENT_MODE_DESC,
    I18N_UI_MODE_DESC,
    I18N_UPDATE_CHECK_DESC,
    I18N_ABOUT_DESC,
    I18N_RESTART_UI_DESC,
    I18N_MARKETPLACE_DESC,
    I18N_CRYPTO_DESC,
    I18N_NEWSREADER_DESC,

    /* Itho ventilation */
    I18N_ITHO_VENT,
    I18N_ITHO_VENT_DESC,

    /* Language */
    I18N_LANGUAGE,
    I18N_ENGLISH,
    I18N_DUTCH,

    I18N_COUNT
};

const char * i18n(enum i18n_key key);
#define TR(k) i18n(k)

#endif
