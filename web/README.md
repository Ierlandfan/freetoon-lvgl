# freetoon-WASM — full LVGL UI in a browser

Compiles the existing freetoon C source tree (`../lvgl_ui_recovered/src`) to **WebAssembly** via Emscripten + SDL2, so the **full LVGL UI runs in any browser** — Android tablet, iPad, laptop, anywhere. Not the PWA. Not a port. Just the same C code, retargeted.

Mirrors the upstream [`lv_web_emscripten`](https://github.com/lvgl/lv_web_emscripten) template, with a freetoon-specific entry point + a slim JS bridge so the UI runs as a **slave to a master Toon** (reads state from `/api/state/stream`, writes back via `/api/setpoint` etc.).

## Why this is the right shape for client mode

- **Zero install.** Open `http://<toon>:10081/ui/` on any device → full UI. *Add to Home Screen* on iOS/Android makes it feel native.
- **Same-origin** when hosted on the master Toon's `pwa_server` → no CORS hell. The endpoints `client_link.c` already uses (`/api/state/stream`, `/api/setpoint`, `/api/program`, `/api/curtain`) are exactly what the JS bridge calls.
- **Single source of truth.** All integrations (HA / HomeWizard / weather / MQTT / Itho / Domoticz) stay on the master Toon. The client just renders.
- **Cross-platform for free.** Same WASM bundle works on every browser-capable device.

## Tradeoff (vs. native APK)

The browser sandbox blocks raw TCP sockets and most cross-origin HTTP. **Full standalone** (talking directly to HomeWizard P1 / Home Assistant / MQTT *from the tablet* without a master Toon) is therefore **not** the model here — the master Toon does that work. If you ever want a standalone-on-device client, an NDK build would be needed; this scaffolding is slave-mode only.

## Build

1. Install Emscripten (one-time):
   ```sh
   git clone https://github.com/emscripten-core/emsdk
   cd emsdk && ./emsdk install latest && ./emsdk activate latest
   source ./emsdk_env.sh        # <- this line every new shell
   ```
2. From this directory:
   ```sh
   ./build.sh
   # → build/index.html, build/index.wasm, build/index.js
   ```
3. Quick local test:
   ```sh
   cd build && python3 -m http.server 8080
   # browse to http://localhost:8080/index.html
   ```

## Host on the master Toon (one-liner deploy)

Drop the three artefacts into `pwa_server`'s static dir on the Toon (it already serves `/mnt/data/pwa/` at `:10081/`):

```sh
ssh root@<toon> 'mkdir -p /mnt/data/pwa/ui'
scp build/index.{html,wasm,js} root@<toon>:/mnt/data/pwa/ui/
```

Then open `http://<toon>:10081/ui/index.html` from any device on the LAN. Same-origin with the master's API → bridge connects automatically.

## What works (Phase 1) / what's next

| | |
|---|---|
| `web/CMakeLists.txt` + `build.sh` + Emscripten + SDL2 wiring | ✅ scaffolded |
| `main_wasm.c` boots LVGL, mounts IDBFS for settings persistence, runs the screen stack with seeded mock data | ✅ scaffolded |
| `shell.html` provides the canvas + slave-mode JS bridge (EventSource + fetch) | ✅ scaffolded |
| First WASM bundle compiles and renders the home screen in a browser | ⏳ verify on first build |
| `wasm_push_state()` actually parses the SSE JSON and updates `toon_state`/`ha_state` | 🟡 **Phase 2** — reuse the JSON-handling already in `client_link.c` |
| LVGL on-tap → setpoint/program/curtain → master's `/api/*` | 🟡 **Phase 2** — wire LVGL event callbacks to call `Module.ccall('wasm_push_event', …)` → JS `ftPost()` |
| Pinch-to-zoom / responsive layout on tablet | 🟡 **Phase 3** — currently fixed to LVGL's `MONITOR_HOR_RES × MONITOR_VER_RES` (480×320 default); override via `-DMONITOR_HOR_RES=1024 -DMONITOR_VER_RES=600` after testing |
| Install prompt / PWA manifest so "Add to Home Screen" gets a freetoon icon | 🟡 **Phase 3** — add a `manifest.json` + service worker |

## Known limits / known unknowns

- **Threads:** Emscripten threading needs SharedArrayBuffer + COOP/COEP HTTP headers. `pwa_server` doesn't currently emit those, and they break some embedded contexts. Phase 1 is **single-threaded** (no `pthread_create` calls reach the JS side) — fine because the slave bridge is JS-side. If integration threads ever need to run in WASM, we'll add the headers to `pwa_server`.
- **Display size:** the LVGL SDL driver uses `MONITOR_HOR_RES / VER_RES` from `lv_drv_conf.h`. The canvas is full-viewport CSS-scaled; the *render* resolution matches those macros. We'll tune after first paint.
- **First build will fail on something** (this scaffolding hasn't been compiled — no emsdk on the dev box yet). Likely candidates: a freetoon source file that hard-codes a `/mnt/data/...` path, or an integration module's `popen("curl ...")` call. Both are easy to guard with `#ifdef WASM_BUILD`.
