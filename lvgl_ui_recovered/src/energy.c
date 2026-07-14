#include "energy.h"
#include "settings.h"
#include "meteradapter.h"
#include "homewizard.h"
#include "p1esphome.h"
#include "homeassistant.h"
#include "i18n.h"

extern meter_state_t meter_state;
extern hw_state_t    hw_state;

int energy_connected(void) {
    switch (settings.energy_source) {
        case 1:  return settings.enable_p1_elec && hw_state.connected_p1;
        case 2:  return p1esp_state.connected;
        case 3:  return settings.enable_ha && ha_state.have_power;
        default: return meter_state.connected;
    }
}

float energy_power_w(void) {
    switch (settings.energy_source) {
        case 1:  return hw_state.power_w;
        case 2:  return p1esp_state.power_w;
        case 3:  return ha_state.energy_power_w;
        default: return meter_state.power_w;
    }
}

/* Cumulative gas — the Toon's own happ_pwrusage HTTP path doesn't expose it, so
 * source 0 has none. <0 means "no gas reading available". */
float energy_gas_m3(void) {
    switch (settings.energy_source) {
        case 1:  return hw_state.connected_p1 ? hw_state.gas_m3 : -1.0f;
        case 2:  return p1esp_state.connected ? p1esp_state.gas_m3 : -1.0f;
        case 3:  return ha_state.have_gas ? ha_state.energy_gas_m3 : -1.0f;
        default: return -1.0f;
    }
}

/* Trailing-hour gas. Both P1 pollers derive this from the cumulative counter;
 * HA gives us only the counter itself, so there is nothing to report there. */
float energy_gas_hour_m3(void) {
    switch (settings.energy_source) {
        case 1:  return hw_state.connected_p1 ? hw_state.gas_hour_m3 : -1.0f;
        case 2:  return p1esp_state.connected ? p1esp_state.gas_hour_m3 : -1.0f;
        default: return -1.0f;
    }
}

const char * energy_offline_label(void) {
    switch (settings.energy_source) {
        case 1:
            return hw_state.polled_p1 ? tr("P1 offline", "P1 offline")
                                      : tr("Initialiseren...", "Initializing...");
        case 2:
            return p1esp_state.polled ? tr("P1 offline", "P1 offline")
                                      : tr("Initialiseren...", "Initializing...");
        case 3:
            if (!settings.ha_power_entity[0])
                return tr("Geen sensor gekozen", "No sensor selected");
            return tr("HA offline", "HA offline");
        default:
            /* meteradapter: last_flow_s == 0 means no notify has arrived yet */
            return meter_state.last_flow_s ? tr("meter offline", "meter offline")
                                           : tr("Initialiseren...", "Initializing...");
    }
}

int   energy_have_solar(void) { return settings.enable_ha && ha_state.have_solar; }
float energy_solar_w(void)    { return ha_state.solar_w; }
