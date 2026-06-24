#ifndef EFANLAMP_H
#define EFANLAMP_H

/* ESPHome native API client for the ld2411-ble-fanlamp ESP32-C3 at
 * settings.efanlamp_host (default 192.168.3.34, port 6053).
 *
 * Uses the Noise_NNpsk0_25519_ChaChaPoly_SHA256 encrypted native API —
 * fully independent of HA and MQTT; talks directly to the device. PSK is
 * read from settings.efanlamp_psk (base64, 32 bytes decoded).
 *
 * Fan speeds are ESPHome speed_level 1-6.
 * Light brightness is 0-100 % (converted to 0.0-1.0 float internally). */

typedef struct {
    volatile int  connected;          /* 1 = native API session live */
    volatile int  fan_on;             /* 1 = fan is on */
    volatile int  fan_speed;          /* 1-6, valid when fan_on */
    volatile int  light_on;           /* 1 = light is on */
    volatile int  light_brightness;   /* 0-100 % */
    volatile char last_source[16];    /* "toon"/"ha"/"remote"/"web" */
} efanlamp_state_t;

extern efanlamp_state_t efanlamp;

/* Start the background connection thread. Call once at startup.
 * Returns 0 on success (thread launched; connection happens async). */
int efanlamp_start(void);

/* Set fan on/off and speed (1-6). off = any speed value. Non-blocking. */
void efanlamp_fan_set(int on, int speed_level);

/* Toggle fan (on→off or off→on at last known speed or speed 3). */
void efanlamp_fan_toggle(void);

/* Set light on/off and brightness 0-100. Non-blocking. */
void efanlamp_light_set(int on, int brightness_pct);

#endif
