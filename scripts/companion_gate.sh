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
        esac
    fi
    echo freetoon
}

CHOICE=$(read_choice)
echo "$(date '+%F %T') $NAME: ui_choice=$CHOICE" >> "$LOG"

if [ "$CHOICE" = "qt-gui" ]; then
    # In qt-gui mode we want stock daemons in charge. Sleep long enough
    # that init doesn't respawn us in a tight loop — the user kills us
    # with `pkill -f $NAME` after flipping modes back.
    exec sleep 86400
fi

if [ ! -x "$BIN" ]; then
    echo "$(date '+%F %T') $NAME: $BIN not executable — sleeping" >> "$LOG"
    exec sleep 86400
fi

exec "$BIN" "$@"
