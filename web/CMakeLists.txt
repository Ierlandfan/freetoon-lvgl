# freetoon-WASM build.
#
# Compiles the existing freetoon C source tree (../lvgl_ui_recovered/src) +
# LVGL + lv_drivers/sdl to WebAssembly via Emscripten. The output is a
# browser-runnable index.html + index.wasm + index.js that any device on the
# LAN can open. Hosted on the master Toon's pwa_server, the bundle is
# same-origin with /api/state/stream so the slave bridge has no CORS issues.
#
# Build:
#   emcmake cmake -B build .
#   emmake make -C build -j
#
# Mirrors the upstream lv_web_emscripten template:
#   https://github.com/lvgl/lv_web_emscripten

cmake_minimum_required(VERSION 3.16)
project(freetoon_wasm C)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "This CMakeLists.txt only builds under Emscripten. "
                        "Invoke via `emcmake cmake .. && emmake make`.")
endif()

# ---- panel variant ------------------------------------------------------------
# Default build is the Toon 2 layout (1024x600). Configure with -DTOON1=ON to
# build the Toon 1 variant: defines TOON1 (activates display.h's 800x480 DISP_*
# and the per-tile #ifdef tweaks) and sizes the SDL framebuffer to 800x480.
option(TOON1 "Build the Toon 1 (800x480) layout variant" OFF)
if(TOON1)
    set(PANEL_HOR 800)
    set(PANEL_VER 480)
    message(STATUS "freetoon-wasm: Toon 1 variant (800x480)")
else()
    set(PANEL_HOR 1024)
    set(PANEL_VER 600)
    message(STATUS "freetoon-wasm: Toon 2 variant (1024x600)")
endif()

# ---- locations ----------------------------------------------------------------
# web/CMakeLists.txt → repo root → lvgl_ui_recovered/src
set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(FREETOON  ${REPO_ROOT}/lvgl_ui_recovered/src)

# ---- freetoon source set ------------------------------------------------------
# Glob freetoon's .c, then drop the files that don't apply on web:
#   - main.c        : Toon-only main; replaced by main_wasm.c
#   - sim.c         : own main() (headless render-to-PPM)
#   - bootpick.c    : Toon-only boot picker
#   - backlight.c   : /sys/class/backlight (Toon hardware)
# Integration .c files stay in the build for Phase 1; their start_* threads
# are simply NOT called from main_wasm.c (slave-mode only). When the slave
# bridge can't reach a localhost service the modules mark themselves offline
# and the UI shows "--" gracefully.
file(GLOB FT_SOURCES CONFIGURE_DEPENDS ${FREETOON}/*.c)
list(REMOVE_ITEM FT_SOURCES
    ${FREETOON}/main.c          # Toon-only main; replaced by main_wasm.c
    ${FREETOON}/sim.c           # has own main() (headless render-to-PPM)
    ${FREETOON}/bootpick.c      # Toon-only boot picker
    ${FREETOON}/backlight.c     # /sys/class/backlight (Toon hardware)
    ${FREETOON}/evdump.c        # standalone evdev dumper (uses linux/input.h)
    ${FREETOON}/fbvnc_input.c   # standalone VNC input bridge (linux/input.h)
    ${FREETOON}/wastefetch.c    # standalone QuickJS helper binary
    ${FREETOON}/gifdec.c        # superseded by LVGL's vendored gifdec
    ${FREETOON}/toon1_touch.c   # Toon-1 evdev touch (linux/input.h); active only
                                # under -DTOON1, and the WASM uses SDL input —
                                # called only by main.c/bootpick.c (also excluded)
)

# ---- LVGL ---------------------------------------------------------------------
# We don't pull in lv_drivers/sdl: its SDL_Thread-based input polling needs
# pthread support, which we deliberately leave off (no SharedArrayBuffer hassle).
# main_wasm.c implements a thin inline SDL flush + event pump instead.
file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS ${FREETOON}/lvgl/src/*.c)
list(APPEND LVGL_SOURCES
    ${FREETOON}/lv_font_montserrat_96_custom.c
    ${FREETOON}/lv_font_montserrat_64_custom.c)

# ---- the WASM module ----------------------------------------------------------
add_executable(index
    main_wasm.c
    stubs_wasm.c
    ${FT_SOURCES}
    ${LVGL_SOURCES})

target_include_directories(index PRIVATE
    ${FREETOON}
    ${FREETOON}/lvgl
    ${FREETOON}/lv_drivers
    ${CMAKE_CURRENT_LIST_DIR})

target_compile_definitions(index PRIVATE
    LV_CONF_INCLUDE_SIMPLE
    LV_LVGL_H_INCLUDE_SIMPLE
    # Enable LVGL's SDL display/input driver and give it the design resolution.
    # lv_drv_conf.h gates the SDL driver behind USE_SDL and the MONITOR_*_RES
    # defines behind USE_MONITOR; flip both on here rather than patching the
    # vendored conf file. The 1024x600 matches DESIGN_HOR/VER in display.h, so
    # screen-position macros land where they do on the Toon 2 panel.
    # USE_SDL and USE_MONITOR are mutually exclusive in the driver (the
    # `sdl.c` check rejects both). MONITOR_*_RES is still used by sdl.c
    # internals for buffer sizing — set as plain defines without USE_MONITOR.
    USE_SDL=1
    MONITOR_HOR_RES=${PANEL_HOR}
    MONITOR_VER_RES=${PANEL_VER}
    SDL_HOR_RES=${PANEL_HOR}
    SDL_VER_RES=${PANEL_VER}
    $<$<BOOL:${TOON1}>:TOON1>
    WASM_BUILD
    BUILD_VERSION="v0.9.34")

target_compile_options(index PRIVATE
    -O2
    -Wno-unused-parameter
    -Wno-unused-function
    "SHELL:-s USE_SDL=2")

# Emscripten link flags:
#   USE_SDL=2            – Emscripten supplies SDL2 to the WASM build.
#   INITIAL_MEMORY=64MB  – freetoon's fonts + screens + draw buffers fit
#                          comfortably; can shrink later once measured.
#   ALLOW_MEMORY_GROWTH  – grow if a screen allocates more (safer default).
#   EXPORTED_RUNTIME_METHODS=ccall,cwrap – let the JS bridge call into C.
#   --shell-file shell.html – wrap the canvas + slave-mode JS bridge.
#   ASYNCIFY             – lets blocking C code (e.g. settings load from
#                          mounted IDBFS) yield control to the browser. Small
#                          performance cost; worth it for portability.
set_target_properties(index PROPERTIES
    SUFFIX ".html"
    LINK_FLAGS "-O2 \
        -s USE_SDL=2 \
        -s INITIAL_MEMORY=67108864 \
        -s ALLOW_MEMORY_GROWTH=1 \
        -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','stringToUTF8','UTF8ToString','lengthBytesUTF8','addRunDependency','removeRunDependency','FS'] \
        -s EXPORTED_FUNCTIONS=['_main','_malloc','_free','_wasm_push_state','_wasm_push_event','_wasm_map_done'] \
        -s FORCE_FILESYSTEM=1 \
        -s ASYNCIFY=1 \
        -s FETCH=1 \
        -s ASSERTIONS=1 \
        --shell-file ${CMAKE_CURRENT_LIST_DIR}/shell.html")
