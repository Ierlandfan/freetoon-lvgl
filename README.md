# freetoon-lvgl

A from-scratch UI and integration stack for the **Eneco Toon 1 / Toon 2** smart
thermostat that runs entirely on the device — no cloud, no Quby login, no
subscription. The stock Qt UI is replaced with an LVGL app; a small constellation
of side processes brings the hardware back to life (boiler, P1 meter, vent,
weather, packages) and exposes everything over a tiny progressive web app so
you can also drive the house from your phone.

![Demo](docs/demo.gif)

*Captured live from the device framebuffer: home idle → home with water flowing → settings tile grid → home heating + water → dim ambient with clock, fan spinning, radiator+flame and water drop all visible.*

![Home screen](docs/screenshots/home.png)

## Why

Eneco discontinued cloud support for the original Toon: no more app, no more
weather, no more package tracking, no more software updates. The device itself
is a perfectly capable iMX28 box with a 1024×600 touchscreen, an OpenTherm
adapter, an i.MX P1-meter UART and a humidity/temperature sensor — too good to
land on a shelf.

`freetoon-lvgl` keeps the thermostat doing what it's good at (running OpenTherm,
modulating the boiler, reading the room) while replacing the parts that
required a server somewhere:

| Lost piece | Replacement |
|---|---|
| Eneco cloud UI | LVGL app on the framebuffer + LVGL "dim" ambient screen |
| Mobile app | Self-hosted PWA served from the Toon itself |
| Weather forecast | Direct Buienradar pull |
| Energy graphs | P1 bridge → MQTT → Home Assistant (or local view) |
| Package tracking | IMAP-driven HA automation → MQTT → on-device banner |
| Ventilation control | Itho Wifi addon REST API integration |
| Schedule editor | Native LVGL screen + PWA editor |

## What's in this repo

```
lvgl_ui_recovered/      The LVGL replacement UI
  src/
    main.c              entry point + tick/timer setup
    boxtalk.c           BoxTalk client — speaks to happ_thermstat over TCP 1337
    quby_bridge ↗       see quby_bridge/ — Quby-protocol bridge to keteladapter
    homeassistant.c     HA REST client (curtains, notify, generic)
    homewizard.c        HWE-P1 + HWE-WTR pollers
    ventilation.c       Itho Wifi addon REST client
    mqtt_client.c       Hand-rolled MQTT 3.1.1 subscriber (no libmosquitto)
    packages.c          Tap-to-dismiss banner queue, MQTT-fed
    pwa_server.c        HTTP + SSE server for the PWA on :10081
    screen_home.c       Main tile grid (heater / vent / energy / water / waste)
    screen_dim.c        Ambient screen (clock + temp + flame + forecast)
    screen_thermostat.c "Heater detail" page (OT health, boiler flow/return)
    screen_settings.c   On-device config (MQTT, weather, brightness, …)
    screen_schedule.c   Weekly Comfort/Home/Sleep/Away scheduler
    schedule.c          Read/write hcb_config schedule JSON
    healthcheck.c       /healthz endpoint + watchdog + daily restart
    settings.c          Persistent /mnt/data/toonui.cfg
    gen_icons.py        Bitmap-icon generator (flame, drop, faucet, fan,
                          radiator, weather, waste) — runs at build time
    pwa/                Built into the binary at /, served by pwa_server

quby_bridge/            Userspace bridge for keteladapter (replaces stock BA)
p1bridge/               Pushes HWE-P1 readings into hcb_rrd
ha_packages/            HA-side package-tracking automation (deploy.py)
pwa_static/             PWA shell — index.html / app.js / sw.js
scripts/                Helpers (ot_mode_switch.sh: proxy/wireless/off)
install.sh              One-shot deploy from a Linux host to a Toon
toonshot.sh             Pull /dev/fb0 over SSH → PNG (debug helper)
toontap.sh              Inject a touch event into /dev/input/event1
```

## Screens

### Home
The default LVGL screen. Big tiles for the main signals — indoor temperature
and setpoint, boiler state (with the original-Toon-style radiator+flame icon
when CH is firing), waste-collection reminders, live energy + gas, ventilation
preset + RPM, water flow, curtains. A 5-day weather forecast lives at the
bottom. Tap the heater tile for OT detail; tap the gear-corner for settings.

![Home](docs/screenshots/home.png)

### Dim / ambient
After the configured idle timeout the screen drops to a near-black ambient
view: clock, indoor temperature with a small radiator+flame indicator when
the boiler is firing, vent fan spinner on the left mirroring the flame on
the right, top trash hint, top-right outdoor temperature, package banners
at the top, weather strip at the bottom.

![Dim](docs/screenshots/dim.png)

### Heater detail
Tap the heater tile from home to see boiler-side telemetry — flow + return
water temps, modulation level, CH setpoint from the stooklijn, water
pressure, eCO₂ / TVOC / humidity. The `Advanced` button opens a full dump
of every OpenTherm DataId currently tracked by OTGW.

### PWA
Served at `http://<toon>:10081/` from the device itself. Same controls as the
LVGL screen plus a packages list with manual entry and the weekly schedule
editor. Add to Home Screen for a phone-app feel; works offline-first via a
small service worker.

## Architecture

```
                        ┌───────────────────┐
                        │   happ_thermstat  │ ← stock Toon process, untouched
                        │  (OpenTherm logic)│
                        └────────┬──────────┘
                                 │ BoxTalk (XML/NUL-framed over 127.0.0.1:1337)
                                 │ HTTP    (127.0.0.1:10080/happ_thermstat?…)
                                 ▼
┌────────────┐  Quby   ┌───────────────────────────┐    LVGL    ┌────────────┐
│keteladapter│◀──ttymxc0──▶│        toonui          │──────▶│  /dev/fb0  │
│  (boiler)  │  bridged    │   (this repo, on /mnt/ │       │  1024×600  │
└────────────┘  via       │       data/toonui)      │       └────────────┘
                quby_bridge└──┬─────┬─────┬─────┬───┘
                              │     │     │     │
                              │     │     │     └──── PWA HTTP/SSE on :10081
                              │     │     │
                              │     │     └──────── MQTT subscriber → home/packages/*
                              │     │
                              │     └──── Itho Wifi REST (vent), HWE-P1 (power/gas),
                              │           HA REST (curtains, notify), Buienradar
                              │
                              └──── Boxtalk client: setpoint, program, schedule
```

`toonui` is the only new long-running process on the device. The stock
`qt-gui` is removed from `inittab` so it can't fight for the framebuffer;
`toonui` is added with `respawn` so init brings it back on crash. Config
lives at `/mnt/data/toonui.cfg`; logs go to `/var/volatile/tmp/toonui.log`.

## Install

Two paths: grab a pre-built **release** (no toolchain required), or build
from **source** if you want to hack on the code. Both use the same
`install.sh` script — it auto-detects which layout it's running in.

### Prerequisites (both paths)

* A Linux host with `bash`, `ssh`, `scp`, and `sshpass` on `PATH`
  (`apt-get install sshpass`).
* SSH enabled on the Toon — toggle from the stock `hcb_config` menu
  (Internals → SSH server). Default credentials are `root` / `toon`.
* `/mnt/data/` is writable on stock firmware; nothing to do there.
* A reachable MQTT broker is recommended (powers the packages banner
  and the energy/water logging path).
* Home Assistant is optional (drives curtains, Life360 location, push
  notifications) — set the LLAT in `/mnt/data/ha.cfg` after install.

### Path A — Use a release (recommended)

1. Grab the latest tarball from
   [Releases](https://github.com/Ierlandfan/freetoon-lvgl/releases) — pick
   `freetoon-lvgl-vX.Y.Z.tar.gz`.

2. Extract somewhere on your Linux host:

   ```bash
   tar xzf freetoon-lvgl-v0.5.0.tar.gz
   cd freetoon-lvgl-v0.5.0
   ```

   The tarball contains:

   ```
   toonui                 # LVGL UI binary (ARMv7 hardfloat)
   p1bridge               # HomeWizard → hcb_rrd bridge
   quby_bridge            # (optional) Quby-protocol bridge to keteladapter
   ot_mode_switch.sh      # helper called by the on-device Settings UI
   pwa/                   # PWA static assets served on :10081
   install.sh             # the same script as in the repo
   ```

3. Run the installer against your Toon:

   ```bash
   TOON_HOST=192.168.3.212 \
   P1_TOKEN=<HomeWizard v2 bearer token> \
   VENT_USER=<Itho-Wifi user> VENT_PASS=<…> \
   ./install.sh
   ```

   The script scps every binary to `/mnt/data/`, writes
   `/mnt/data/p1bridge.conf` + `/mnt/data/vent.conf`, upserts the
   `toon` / `p1br` / `qbri` rows in `/etc/inittab`, then `kill -HUP 1`s
   init. After ~4 seconds it pgreps the new processes so you can see them
   running. Idempotent — re-run to upgrade.

4. (Optional) edit `/mnt/data/toonui.cfg` on-device — or open the
   Settings tile on the LVGL UI — to point at your MQTT broker, set the
   weather location, dial brightness/idle timeout, etc.

To uninstall: `./install.sh --uninstall` (drops inittab rows, kills
processes, removes binaries + PWA + configs).

### Path B — Build from source

Use this if you want to modify the UI or the bridges.

1. Clone the repo and grab the LVGL submodule:

   ```bash
   git clone https://github.com/Ierlandfan/freetoon-lvgl.git
   cd freetoon-lvgl
   git submodule update --init --recursive
   ```

2. Cross-compile for ARMv7 hardfloat. The Makefile expects a Linaro
   toolchain at `/tmp/qt_rebuild/linaro/` (mirror of
   `gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabihf.tar.xz`). Adjust
   the path in the Makefiles if yours lives elsewhere.

   ```bash
   make -C lvgl_ui_recovered/src        # produces lvgl_ui_recovered/build/toonui
   make -C p1bridge                     # produces p1bridge/p1bridge
   make -C quby_bridge                  # produces quby_bridge/quby_bridge (optional)
   ```

   Native desktop build with the SDL2 driver is also wired up — useful
   for previewing UI changes without a Toon on the desk. See
   `lvgl_ui_recovered/src/Makefile` for the `sdl` target.

3. Deploy with the same script — it picks the in-tree build paths
   automatically:

   ```bash
   TOON_HOST=192.168.3.212 P1_TOKEN=… ./install.sh
   ```

Edit `lvgl_ui_recovered/src/settings.c` defaults or `/mnt/data/toonui.cfg`
on-device to point at your own IPs.

## Status

The thermostat path is **the** path on this install — `happ_thermstat` →
`keteladapter` over the Quby protocol, with `quby_bridge` patched to mimic
the discontinued BoilerAdapter bit-for-bit. OTGW runs in GW=1 (relay) mode
and forwards the same OpenTherm frames the original BA produced, so the
boiler's CH/DHW state machines see no change from when Eneco's hardware was
in the loop. Verified by warming a 17.9 °C room to 20.0 °C unattended.

The repo is opinionated to one physical install (the author's), and parts
of the integration glue (HA entities, MQTT topic names, Itho user/password
location) are tuned to that environment. The LVGL UI and the core BoxTalk /
Quby / OTGW plumbing are portable; the per-integration screens are easy to
disable.

## Acknowledgements

* **OTGW** — Robert van den Breemen's HTTP-firmware fork of the OpenTherm
  Gateway is the boiler-side workhorse.
* **Itho Wifi** — Arjen Hiemstra's add-on board + REST API made vent control
  trivial.
* **HomeWizard** — open `/api/v1/data` on the HWE-P1 / HWE-WTR.
* **LVGL** — the embedded UI library this app is built on.
* **Toon community** — the reverse-engineering work scattered across
  hacktoon and various Tweakers threads that mapped out happ_thermstat,
  hcb_config and the Quby protocol.

## License

This repository is published for educational and personal use. It contains no
Eneco / Quby proprietary code. The stock Toon binaries (`happ_thermstat`,
`hcb_config`, `keteladapter` firmware, …) remain Eneco's; nothing in this
repo redistributes or modifies them.
