#!/bin/sh
# WASM-host gate. An inittab row runs this so the headless toonui (data daemons
# + pwa_server, NO framebuffer) runs ONLY in the dedicated "wasm" UI mode —
# stock qt-gui on the panel, freetoon serving the WASM slave UI + /api on :10081.
#
# In normal freetoon mode the full on-screen toonui already serves the PWA, so a
# second headless one would be redundant (and fight over :10081) — we idle.
# In plain qt-gui mode there is no PWA — we idle.
#
# inittab:
#   tuih:345:respawn:/mnt/data/toon_wasm_host.sh
#
#   ui_choice = wasm  → exec toonui --headless
#   else              → poll-idle: stay alive so init keeps the row, re-check,
#                       and exit on a flip TO wasm so init respawns us and the
#                       fresh entry exec's the headless host.

set -u

CHOICE_FILE=/mnt/data/ui_choice
TOONUI=/mnt/data/toonui
LOG=/var/volatile/tmp/toon_wasm_host.log
POLL_S=5

read_choice() {
    if [ -r "$CHOICE_FILE" ]; then
        c=$(cat "$CHOICE_FILE" 2>/dev/null | tr -d '[:space:]')
        case "$c" in
            wasm|wasm-host|masterslave) echo wasm; return ;;
        esac
    fi
    echo other
}

CHOICE=$(read_choice)
echo "$(date '+%F %T') wasm_host: ui_choice=$CHOICE" >> "$LOG"

# Stay alive (init keeps the tuih row happy) while re-checking ui_choice. Exit
# the moment it flips TO wasm so init respawns us and the fresh entry exec's the
# headless host. POLL — never `exec sleep`, which blinds init to the flip
# (see feedback_companion_gate_poll_not_sleep / the qbri bug).
idle_loop() {
    while true; do
        sleep "$POLL_S"
        if [ "$(read_choice)" = wasm ]; then
            echo "$(date '+%F %T') wasm_host: ui_choice→wasm — exiting so init respawns into headless" >> "$LOG"
            exit 0
        fi
    done
}

if [ "$CHOICE" != wasm ]; then
    idle_loop
fi

if [ ! -x "$TOONUI" ]; then
    echo "$(date '+%F %T') wasm_host: $TOONUI not executable — idling" >> "$LOG"
    idle_loop
fi

# Headless toonui needs no QT_QPA / Qt env (it never touches the framebuffer).
echo "$(date '+%F %T') wasm_host: launching toonui --headless" >> "$LOG"
exec "$TOONUI" --headless
