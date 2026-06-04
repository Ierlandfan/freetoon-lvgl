#!/bin/sh
# Companion gate. Called by p1br / qbri inittab rows in place of the
# direct binary, so that when the user switches the UI to stock qt-gui
# our sidecars (p1bridge republishing meter data, quby_bridge proxying
# OT bytes) also go quiet — leaving stock meteradapter / keteladapter
# in charge.
#
# Usage (from inittab):
#   p1br:345:respawn:/mnt/data/companion_gate.sh p1bridge /mnt/data/p1bridge
#   qbri:345:respawn:/mnt/data/companion_gate.sh quby_bridge /mnt/data/quby_bridge -m proxy
#
# Behaviour:
#   ui_choice = freetoon  → exec the binary normally
#   ui_choice = qt-gui    → log + `exec sleep 86400` so init thinks we're
#                           running but we do nothing. On the next mode
#                           flip the user kills us and init respawns →
#                           we re-evaluate ui_choice on entry.

set -eu

CHOICE_FILE=/mnt/data/ui_choice
LOG=/var/volatile/tmp/companion_gate.log
NAME="$1"; shift
BIN="$1";  shift   # remaining args are passed through to the binary

read_choice() {
    if [ -r "$CHOICE_FILE" ]; then
        c=$(cat "$CHOICE_FILE" 2>/dev/null | tr -d '[:space:]')
        case "$c" in
            qt-gui|qtgui|stock) echo qt-gui; return ;;
            # WASM-host mode runs stock qt-gui on the panel and the stock
            # boiler/meter daemons own the hardware — our bridges must stay
            # quiet there too, exactly as in plain qt-gui mode.
            wasm|wasm-host|masterslave) echo qt-gui; return ;;
        esac
    fi
    echo freetoon
}

CHOICE=$(read_choice)
echo "$(date '+%F %T') $NAME: ui_choice=$CHOICE" >> "$LOG"

# Poll interval for re-checking /mnt/data/ui_choice while idling in
# qt-gui mode. Five seconds is a reasonable trade-off — short enough that
# a user flipping back to freetoon doesn't wait noticeably for their
# bridges to come back online, long enough that the gate doesn't show
# up in `top` as a CPU consumer.
POLL_S=5

idle_loop() {
    # Stay alive (init thinks the qbri / p1br row is fine) but re-check
    # ui_choice every POLL_S seconds. As soon as it flips away from
    # qt-gui, exit so init respawns us — the fresh entry then exec's the
    # bridge binary because the new CHOICE matches.
    #
    # The previous `exec sleep 86400` design left init in the dark
    # for a full day: even after ui_choice flipped back the gate was
    # already gone from init's POV (it had exec'd into sleep, which
    # didn't itself terminate), so the qbri row never respawned and the
    # bridge stayed offline until someone manually killed the sleep.
    while true; do
        sleep "$POLL_S"
        new=$(read_choice)
        if [ "$new" != "qt-gui" ]; then
            echo "$(date '+%F %T') $NAME: ui_choice flipped to $new — exiting so init respawns the gate" >> "$LOG"
            exit 0
        fi
    done
}

if [ "$CHOICE" = "qt-gui" ]; then
    idle_loop
    # idle_loop only returns via exit; this line is unreachable.
fi

if [ ! -x "$BIN" ]; then
    echo "$(date '+%F %T') $NAME: $BIN not executable — entering idle loop" >> "$LOG"
    # Reuse idle_loop: it'll keep re-checking ui_choice anyway. If the
    # binary appears we'll cycle back through main on the next respawn
    # and pick it up.
    idle_loop
fi

exec "$BIN" "$@"
