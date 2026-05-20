# freetoon ESP32 display client (CYD)

A small wall display that mirrors and controls the Toon thermostat over WiFi —
a satellite for the freetoon-lvgl GUI. Built for the **"Cheap Yellow Display"
ESP32-2432S028R** (ESP32-WROOM, 2.8" 320×240 ILI9341 + XPT2046 resistive touch).

It does **not** speak BoxTalk. It just talks to the Toon's existing PWA HTTP API
(`pwa_server`, port 10081):

| call | purpose |
|------|---------|
| `GET /api/state` | indoor temp, setpoint, program, pressure, humidity, air, burner |
| `POST /api/setpoint` `{"value":"18.50"}` | nudge the setpoint |
| `POST /api/program` `{"state":0..3}` | Comfort / Home / Sleep / Away |

So any number of these can sit around the house as cheap extra thermostats.

## Build & flash

1. Install [PlatformIO](https://platformio.org/) (`pip install platformio`).
2. Edit `src/config.h` — your WiFi SSID/password and the Toon's LAN IP.
3. Build + flash:
   ```sh
   pio run -t upload          # or just `pio run` to compile
   pio device monitor         # serial log @115200
   ```

## Hardware notes

- Display config is set entirely via `build_flags` in `platformio.ini` (no
  `User_Setup.h` edits). The default is **ILI9341** (classic single-micro-USB
  CYD). The newer USB-C "2-USB" board uses **ST7789** — swap
  `ILI9341_2_DRIVER` for `ST7789_DRIVER` in `platformio.ini`.
- Touch (XPT2046) is on a **separate SPI bus** from the display — the usual CYD
  gotcha. Pins are in `main.cpp` (`TOUCH_*`). If taps land in the wrong spot,
  adjust the `TS_MINX/MAXX/MINY/MAXY` calibration constants in `main.cpp`.
- Uses the 3 MB `huge_app` partition (LVGL + TFT_eSPI + WiFi is large).

## Scope

v1 is a thermostat: live temperature, setpoint ± , and a tap-to-cycle mode
button, plus a status line (CH pressure / humidity / CO₂ / burner). Energy
graphs, lights, vent etc. can be added later against the same API.
