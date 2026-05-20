#pragma once
/* freetoon ESP32 client — edit these for your network + Toon. */

#define WIFI_SSID   "your-wifi-ssid"
#define WIFI_PASS   "your-wifi-password"

/* Toon LAN IP + the PWA server port (pwa_server listens on 10081). */
#define TOON_HOST   "192.168.3.212"
#define TOON_PORT   10081

/* How often to refresh /api/state (ms). */
#define POLL_MS     3000

/* Setpoint step per +/- tap (°C). */
#define SP_STEP     0.5f
