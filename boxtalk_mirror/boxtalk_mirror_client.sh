#!/bin/sh
# One-shot CLIENT setup: turn THIS Toon into a live two-way mirror of a MASTER
# Toon — the stock qt-gui shows the master's data and its controls drive the
# master. No freetoon UI needed here; runs alongside stock qt-gui.
#
#   boxtalk_mirror_client.sh <master_host> <proxy_port> <user> <pass> [filter]
#       1. mute the local data publishers (so the mirror is the only source),
#       2. install the init-managed mirror (auto-discovers this Toon's own
#          thermostat GUID — no manual step), pointing at the master's proxy.
#   boxtalk_mirror_client.sh --undo
#       restore the local publishers + remove the mirror (back to stock).
#
# The MASTER must be running boxtalk_proxy (see boxtalk_proxy_run.sh) with a
# matching <user>:<pass> and a firewall opening <proxy_port> to this client.
set -u
HERE=$(dirname "$0")
MUTE="$HERE/boxtalk_mirror_mute.sh"
RUN="$HERE/boxtalk_mirror_run.sh"
[ -x "$MUTE" ] || MUTE=/mnt/data/boxtalk_mirror_mute.sh
[ -x "$RUN" ]  || RUN=/mnt/data/boxtalk_mirror_run.sh

if [ "${1:-}" = "--undo" ]; then
    "$RUN" --undo
    "$MUTE" --undo
    echo "client mirror removed; local publishers restored (stock mode)"
    exit 0
fi

[ $# -ge 4 ] || { echo "usage: $0 <master_host> <proxy_port> <user> <pass> [filter] | --undo"; exit 2; }
[ -x /mnt/data/boxtalk_mirror ] || { echo "error: /mnt/data/boxtalk_mirror not installed"; exit 1; }

"$MUTE"                                   # full-stop local thermostat/sensor/p1/hvac/pwrusage
"$RUN" install "$1" "$2" "$3" "$4"        # thermostat + power + humidity instances
echo "client mirror live — toon shows $1's data (thermostat + power + humidity)"
