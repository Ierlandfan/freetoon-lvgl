# FreeToon Backlog

## Completed / In Testing

### 1. Marketplace Catalog Dynamic Refresh & Rebuild (Implemented)
- **Status**: Fixed in current working build.
- **Implemented Fixes**:
  - Re-fetch catalog on every Marketplace screen entry (`start_catalog_fetch()`).
  - Added timestamp query parameter (`?t=<timestamp>`) to bypass GitHub CDN 5-minute HTTP cache.
  - Added explicit Refresh button (`LV_SYMBOL_REFRESH`) in Marketplace header.
  - Rescan local installed integration manifests (`tile_slots_init()`).
  - Cleaned up refresh timer and reset entry change detector across screen navigation.
  - Enforce larger touch target for close ('X') and refresh buttons.
  - Automatically sort apps with an update available to the top of the Marketplace list.

### 2. Homescreen Swipe Left vs. Tile Click Discrimination (Implemented)
- **Status**: Fixed in current working build.
- **Implemented Fixes**:
  - Added `lv_indev_wait_release(indev)` inside `on_home_gesture()` on recognized horizontal swipes to cancel pending release/click dispatch on the pressed tile.
  - Added `tile_click_wrapper()` to check pointer gesture direction and displacement (`gesture_sum`, `scroll_sum` > 15px); any swipe drag completely suppresses tile click activation.

### 3. Page 2 Tile Slot 1 (Linksboven) Assignment Conflict (Implemented)
- **Status**: Fixed in current working build.
- **Implemented Fixes**:
  - `tile_slots_bind()` clears `settings.tile_rotate_enabled` when assigning a specific integration to `TILE_SLOT_P1_0`.
  - Auto-rotate background loop and tap handler in `screen_home.c` only run on slot 0 if slot 0 is unassigned (`!tile_slots_binding(TILE_SLOT_P1_0)`).

### 4. Dynamic Font Scaling for Long Titles on Page 2 Tiles (Implemented)
- **Status**: Fixed in current working build.
- **Implemented Fixes**:
  - `render_tile_slot()` and `render_meta_into()` inspect the character length of `m->latest_value`.
  - For values with `length > 15` (e.g. news headlines, Teletekst articles), the label enables multi-line text wrapping (`LV_LABEL_LONG_WRAP`) within the tile's content width and scales font dynamically to `SF(18)` or `SF(15)`. Short numeric values retain large bold fonts (`SF(28)` / `SF(48)`).

### 5. Layout Editor Close & Re-open Double Free Crash (Implemented)
- **Status**: Fixed in current working build.
- **Implemented Fixes**:
  - In `screen_layout_editor.c`, `on_cancel()` and `screen_layout_editor_show()` now properly reset all dangling pointers (`rects[]`, `size_btns[]`, `theme_btn`, `page_btn`, `sel_lbl`) when the editor is closed, preventing use-after-free / double-free crashes when re-opening Indeling.

### 6. Buienradar Rain Forecast Graph & Multi-Timeframe Radar (Implemented)
- **Status**: Completed in Release 0.9.77.
- **Implemented Features**:
  - Integrated dedicated Buienradar rain background polling thread (`buienradar_rain.c`) fetching 3-hour forecast (`RainHistoryForecast`) and 24-hour forecast (`Rain24Hour`).
  - Added homescreen tile `LT_RAINFALL` with toggle buttons (`[3u]`, `[24u]`), rainfall bars, and dynamic time markers.
  - Added ambient dim screen block `DB_RAINFALL` with black background (`#000000`), borderless floating chart, and time labels.
  - Added multi-timeframe view to detailed forecast screen (`screen_forecast.c`) with floating pill bar (`[ Radar ]`, `[ 3 uur ]`, `[ 24 uur ]`).
  - Added horizontal threshold divider lines and labels (*Zwaar*, *Matig*, *Licht*) matching Buienradar mobile app.
  - Implemented clean centered *"Geen neerslag verwacht"* display when dry, hiding chart and redundant top messages.
  - Added GPS coordinate configuration in Settings → Weer with automatic seeding from `buienradar.userSettings.json` or fallback.

### 7. Hourly Weather Forecast Screen, Clock Hour Labels & Layout Polish (Implemented)
- **Status**: Completed in Release 0.9.78.
- **Implemented Features & Fixes**:
  - **Detailed 24-Hour Weather Forecast Screen (`screen_weather_hourly.c`)**:
    - Clickable 5-day forecast cards on the weather screen (`screen_forecast.c`) opening an hourly breakdown for any selected day.
    - Full 24-hour vertical scrollable list with auto-scroll to the current hour on today's view.
    - Shows hourly time, weather icon, temperature, feel temperature, rain probability & rainfall (mm), Beaufort wind force, and rotated wind direction arrow.
    - Integrated day switcher buttons (`<` and `>`) sized identically to the close button (`X`) and placed snug around the title block.
    - High-contrast warm amber temperature text in light mode for excellent legibility.
  - **24-Hour Rain Chart Clock Labels**:
    - Replaced relative time labels (`+6u`, `+12u`, etc.) with actual forecast hour numbers (`10`, `16`, `22`, etc.) on both the homescreen rain tile and the big forecast screen.
  - **Homescreen Rain Tile Proportions & Margins**:
    - Kept header *"Neerslag"* title next to `[3u]` and `[24u]` toggle buttons.
    - Dropped summary subtitle line on compact tiles (e.g. 3x2).
    - Capped graph height to 60px on compact tiles to avoid overlapping the header row.
    - Increased threshold label margin (44px) so *Zwaar*, *Matig*, and *Licht* sit cleanly inside tile padding without clipping.
  - **Dim Screen Program State & Setpoint Alignment**:
    - Horizontally centered the program state and target temperature row on the dim thermostat block (`DB_THERMO`).
    - Anchored flame/faucet icons below the centered label.
    - Automatically hide the *"Neerslag"* tile title when no rain is expected, leaving *"Geen neerslag verwacht"* centered.
  - **Forecast Screen Layout**:
    - Increased clearance above the rain chart to ensure no collision with the subtitle text.

### 8. Release 0.9.79: Classic Look Integrations, Dynamic Filtering & Auto Light Theme (Implemented)
- **Status**: Completed in Release 0.9.79.
- **Implemented Features**:
  - **Dynamic Integrations in Classic/Stock Theme (`screen_home_stock.c`)**:
    - Tile picker modal (`open_picker`) now lists all installed marketplace integrations (Sonos, Temperature Logger, NOS Teletekst, Domoticz, etc.) and enabled extra local tiles (Afval, Water P1).
    - Integrations render in native stock card style with matching Open Sans typography, dynamic title, live value, and subtitle.
    - Live values and subtitles update automatically every second from BoxTalk/REST state cache (and directly from `templogger` / `wastecollection`).
    - Tapping an integration tile opens its full native detail screen (`open_integration_by_id`).
    - Dynamic layout persistence in `settings.stock_tiles` preserves integration IDs seamlessly.
  - **Dynamic Tile Filtering**:
    - `tile_is_available()` dynamically filters the picker so disabled services (Ventilation, Calendar, Trackers, Solar, Fan+Lamp) are hidden.
    - Default layout slots are sanitized so disabled hardware does not appear as dead tiles.
  - **Auto Light Theme for Classic Look**:
    - Switching to Classic Toon look in *Instellingen → Scherm* automatically enables the Light Theme (`settings.home_light_mode = 1`) and live updates the UI and switches.
  - **Carousel Arrow Wrap-Around**:
    - Pressing right arrow on page 4 wraps to page 1; pressing left arrow on page 1 wraps to page 4.
  - **Marketplace Auto-Restart**:
    - Successfully updating a plugin in the Marketplace automatically triggers `ui_request_restart()` to reload fresh `.so` libraries.
  - **Tmpfs Hygiene & Cleanup**:
    - Auto-purge of stale `/tmp/freetoon.*` update folders and consolidated Domoticz cookie jar (`/var/volatile/tmp/.dz_cookies`).

### 9. Release 0.9.80: Vertical Usage Bars, Preset Black Screen & UI Polish (Implemented)
- **Status**: Completed in Release 0.9.80.
- **Implemented Features**:
  - **Vertical Segmented Usage Bars (`dim_bars_vertical`)**:
    - New toggle switch in *Instellingen → Scherm*: *"Balken: verticaal (klassieke Toon-stijl)"*.
    - Replaces continuous/horizontal energy and gas bars with 12-segment vertical bars inspired by the classic Toon look (bottom 7 green, top 5 amber/yellow, unlit dark grey).
    - Fully supported in both standard standby screen (`screen_dim.c`) and custom dim grid (`screen_dim_grid.c`).
  - **Black Standby Screen on Slapen & Weg Presets**:
    - Optional setting (*"Zwart scherm bij Slapen/Weg"*) that blanks the dim screen completely when the heating program is in *Slapen* or *Weg* preset, matching stock Eneco GUI behavior.
    - Automatically wakes up on screen touch or whenever the program state changes.
  - **Classic Look Home Screen Alignment**:
    - Shifted carousel `G_X0` to 64 and refined the collapsible Devices/HA handle in `screen_home_stock.c` to prevent any overlap with the left tiles on both Toon 1 and Toon 2.
  - **Auto-Rotate Modal Layout Fix**:
    - Resolved collision in *Tegel automatisch wisselen* where the *Toepassen* button overlapped the marketplace notice and checkbox list.
    - Checkbox list now dynamically sizes and clips neatly within view.
  - **Z-Wave Rename Keyboard Fix**:
    - Decoupled the virtual keyboard from the rename card in `screen_zwave.c`, placing the card at the top and the full keyboard at the bottom so the device name input field is never obstructed during typing.

### 10. Release 0.9.81: Solar Integration, Vertical 1-Col Bars, Master Schedule Sync & Appliances Cleanup (Implemented)
- **Status**: Completed in Release 0.9.81.
- **Implemented Features**:
  - **Solar Return Metrics Tab in Energy Statistics Screen (`screen_stats.c`)**:
    - Added dedicated *Zon* (Solar) tab alongside Elektriciteit, Gas, Water en CV bar.
    - Queries DSMR smart meter return registers (`elec_flow_prod`, `elec_quantity_nt_prod`, `elec_quantity_lt_prod`) with automatic summation of normal- and low-tariffs.
    - Built-in automatic fallback to `solar_flow` and `solar_quantity` for dedicated Toon Zon solar inverters.
    - Gracefully handles absent or empty solar RRD archives without errors or crashes.
  - **Dim Block Zon (`DB_SOLAR`) & 1-Column Vertical Bars**:
    - Added dedicated *Zon* block to custom dim layout grid (`screen_dim_grid.c`, `layout.c`, `layout.h`, `screen_dim_layout_editor.c`).
    - Allowed minimum width of 1 unit (`min_w = 1`) for energy, gas, and solar blocks when vertical bar mode (`dim_bars_vertical`) is enabled.
    - When 1-column wide, the value and measurement unit (e.g. `m³`, `W`) are stacked onto two rows with adjusted chart height so labels never truncate or clip outside tile bounds.
  - **Master Toon Heating Schedule Synchronization (`screen_schedule.c`)**:
    - Fixed schedule editing from a Client Toon to reliably update the Master Toon via XML payload (`updateThermostatSettings` and `setThermostatSettings` on `happ_thermstat`), ensuring persistence across master restarts.
  - **Obsolete Apparaten & NILM UI Removal**:
    - Removed redundant *Apparaten* tile from Instellingen (`screen_settings.c`).
    - Removed *Apparaten >* button from Energy Statistics (`screen_stats.c`).
    - Removed obsolete step-change timeline (`screen_energy_timeline.c`) and custom appliance naming screen (`screen_appliances.c`), freeing memory and simplifying UI.

### 11. Release 0.9.82: Auto-Dim Wake Fix, Screen Cleaning Fix, Tile Localization, HA/Z-Wave Modal Overlaps & Stats Euro Mode (Implemented)
- **Status**: Completed in Release 0.9.82.
- **Implemented Features & Fixes**:
  - **Auto-Dim Spontaneous Wake Fix (`screen_settings.c`)**:
    - Fixed an issue where the Toon would automatically wake from the dim standby screen every few minutes without user touch.
    - When the auto-home timer expired while dimmed, `screen_settings_close_modals()` was called, invoking `clean_finish()`.
    - `clean_finish()` previously marked user activity unconditionally (`ui_mark_activity()`), causing `ui_idle_tick()` to register pending wake and dismiss the dim screen.
    - Added an active check guard in `clean_finish()` (`if (!g_clean_overlay && !g_clean_tmr) return;`) to ensure inactive screen-cleaning logic never emits false activity wakeups.
  - **Screen Cleaning Stability on Toon 1 (`screen_settings.c`)**:
    - Completely decoupled cleaning overlay cleanup from UI timer callbacks to prevent access to deallocated objects on Toon 1.
  - **Energy Statistics: Euro Display Mode, Rates Editing & Gas Polish (`screen_stats.c`, `stats.c`, `settings.c`, `settings.h`, `client_link.c`)**:
    - Added toggle between physical units (*Verbruik*: kWh, m³) and financial cost (*Euro*: €) across Energy statistics tabs (Elektriciteit, Gas).
    - Added interactive modal dialogs on the left panel to configure electricity rate (€/kWh) and gas rate (€/m³).
    - Persisted `rate_elec_kwh`, `rate_gas_m3`, and `stats_show_euro` in `toonui.cfg`.
    - Extended LVGL Montserrat fonts with Euro (`€`) and superscript 3 (`³`) glyphs across all font sizes.
    - Displayed gas flow in m³/h instead of l/h in *Gas - Nu*.
    - Added hourly delta fallback in *Gas - Nu* for DSMR smart meters reporting NaN in 5-minute `gas_flow`.
    - Fixed bar rounding in *Gas - Dagen* by enforcing higher Y-scale for small values.
    - Defaulted stock Toon HTTP queries in client/slave mode to port 80.
  - **Tile Localization (`tile_metadata.c`)**:
    - Localized Weather integration tile title to Dutch *"Weer"* instead of English *"Weather"*.
  - **Modal Layout & Component Overlap Fixes**:
    - Fixed Home Assistant entity picker list overlapping the top close button (`screen_ha_settings.c`).
    - Fixed button overlap on Z-Wave rename device modal (`screen_zwave.c`).

---

## Open / Backlog

### 12. Doorbell Live Snapshot/Feed Investigation ('feed unavailable')
- **Status**: Open / Investigation needed.
- **Description**:
  - After pressing the physical doorbell, the Toon correctly opens the doorbell popup via WebSocket trigger, but displays *"Doorbell: snapshot unavailable"*.
- **Investigation Points**:
  - **Camera Proxy vs. Event Image**: Battery-powered Eufy doorbells remain in sleep mode; check if `camera.doorbell` provides a snapshot immediately or if `image.doorbell_event_image` holds the captured doorbell press frame.
  - **HTTP Response Verification**: Check what `/usr/bin/curl` actually downloads from `http://<ha>:8123/api/camera_proxy/camera.doorbell` to `/tmp/doorbell.jpg` (HTTP status code, 401/404/500 JSON vs valid JPEG header `FF D8 FF`).
  - **Stream/Snapshot Wake-up Latency**: Determine if a short delay or wake-up call (or MJPEG stream via go2rtc/RTSP) is needed for battery-powered cameras.

### 13. Settings Cleanup: Remove Redundant Options from 'Scherm'
- **Status**: Implemented.
- **Description**:
  - Clean up and streamline `Instellingen -> Scherm` (`screen_settings.c`) by removing obsolete/redundant configuration rows:
    1. **Taal (Language selection)**: Remove toggle; streamline default UI language.
    2. **Licht thema: grote waarde = binnentemp**: Hardcode to 1 (`settings.stock_big_indoor = 1`) and remove the row.
    3. **Maanstand op kloktegel**: Obsolete feature on modern clock tile; remove switch and associated row.

### 14. Waste Tile on Dim Screen: Category Name Abbreviations
- **Status**: Implemented (Dim Screen).
- **Description**:
  - On the standby/dim screen waste tile (`DB_WASTE` in `screen_dim.c` / `screen_dim_grid.c` / `wastecollection.c`), shorten long category names for better fit and legibility:
    - *"GFT"* instead of *"Groente/Fruit/Tuinafval"*.
    - *"PMD"* instead of *"Plastic/Metaal/Drankpakken"*.

### 15. Dim Screen Transition Over Detail / Modal Screens
- **Status**: Implemented.
- **Description**:
  - When detail/settings modals (`lv_layer_top()`) are open, the dim screen now cleanly covers them by hiding the top layer while dimmed, and restoring it on wake (or closing it when auto-home expires). Doorbell alerts remain on `lv_layer_sys()` so they always appear over the dim screen with full brightness.

### 16. Dim Screen Waste Tile: Dynamic Category Icon
- **Status**: Implemented (Dim Screen).
- **Description**:
  - On the Dim screen, the next upcoming waste collection date now displays the category-specific waste icon (e.g. leaf for GFT, milk carton for PMD, newspaper for paper, bin for residual) rendered in crisp 80x80 white.

### 17. Thermostat Home Tile: Preset Buttons Should Resume Program Mode from Manual
- **Status**: Implemented.
- **Description**:
  - When the thermostat is in Manual mode and the user taps one of the preset buttons (Comfort, Thuis, Slapen, Weg) on the homescreen tile, the thermostat should activate Program mode (`PROG_TEMPOVERRIDE` or `PROG_BASE`) with that preset, rather than staying in Manual mode.

### 18. Vertical Meter Bars on Dim Screen: Value Text Below Bar
- **Status**: Implemented.
- **Description**:
  - When vertical segmented usage bars (`dim_bars_vertical`) are selected on the dim screen, position the reading / unit text underneath the bar rather than beside it to optimize horizontal space.

### 19. Calendar Tile Layout & Background Consistency Across Tile Slots
- **Status**: Implemented.
- **Description**:
  - If the Agenda/Calendar function is assigned to a different slot (such as the ventilation or family tile), the background color and layout differ from the dedicated Agenda tile. Standardize them so all calendar representations use the preferred native Agenda tile style.

### 20. Temporary Setpoint Override Alignment Across Dim, Tile, and Full Thermostat Screen
- **Status**: Implemented.
- **Description**:
  - When the thermostat is in Program mode and the user adjusts the setpoint temperature on the homescreen tile (+/-), the dim screen switches its label to "Handmatig" while the homescreen tile and big thermostat screen stay in Program mode.
  - Align behavior so that a setpoint change is treated as a temporary override in Program mode (holding the new setpoint until the next scheduled program switch), consistently reflected across all screens.

### 21. Big Thermostat Screen: BoxTalk Debug Banner Cleanup
- **Status**: Implemented.
- **Description**:
  - The big thermostat screen currently displays `"BoxTalk OK msg=xxx"`.
  - Hide this debug status during normal operation, only surfacing an alert if communication fails (or routing connectivity warnings to the Inbox).



