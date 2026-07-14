#ifndef TOON_ENERGY_H
#define TOON_ENERGY_H

/* One place that resolves settings.energy_source to a live reading, so the
 * screens don't each carry their own copy of the selector (they used to, and
 * adding a source meant editing five ternaries that had already drifted apart).
 *
 *   0 = meteradapter (the Toon's own meter, via happ_pwrusage)  — default
 *   1 = HomeWizard P1                                           — homewizard.c
 *   2 = ESPHome P1 reader (Zuidwijk SlimmeLezer)                — p1esphome.c
 *   3 = Home Assistant sensor entities                          — homeassistant.c
 */
int   energy_connected(void);
float energy_power_w(void);          /* watts; negative = exporting */
float energy_gas_m3(void);           /* cumulative m³, or <0 when unavailable */
float energy_gas_hour_m3(void);      /* trailing hour m³, or <0 when unavailable */
const char * energy_offline_label(void);   /* "P1 offline" / "Initializing..." etc */

/* Solar production (watts). Independent of energy_source — a meter can only see
 * net export, never gross production, so this always comes from the configured
 * HA production sensor. energy_have_solar() is 0 when it isn't set up. */
int   energy_have_solar(void);
float energy_solar_w(void);

#endif
