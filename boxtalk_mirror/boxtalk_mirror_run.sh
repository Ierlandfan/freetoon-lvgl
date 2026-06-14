#!/bin/sh
# Run boxtalk_mirror instances under init so they survive SSH logout / reboots
# and get respawned. One instance per stock daemon we impersonate, so every tile
# mirrors the master:
#   thermostat -> happ_thermstat  (temp/setpoint/program + water pressure)
#   power      -> happ_pwrusage    (Stroom nu)
#   humidity   -> hdrv_sensory     (Luchtvochtigheid)
#
#   boxtalk_mirror_run.sh install <master_host> <port> <user> <pass>
#       -> write conf + add the tmir/tmip/tmis inittab rows.
#   boxtalk_mirror_run.sh --undo            -> remove rows + stop instances.
#   boxtalk_mirror_run.sh --exec <kind>     (called by init; not for humans)
#
# Pair with boxtalk_mirror_mute.sh to silence the matching local publishers first.
set -u
BIN=/mnt/data/boxtalk_mirror
CONF=/mnt/data/boxtalk_mirror.conf
SELF=/mnt/data/boxtalk_mirror_run.sh
LOGDIR=/var/volatile/tmp
IT=/etc/inittab
THERM_CFG=/HCBv2/config/config_happ_thermstat.xml

reload_init() { telinit q 2>/dev/null || kill -HUP 1 2>/dev/null || true; }

# This Toon's own thermostat instance GUID — the <uuid> in the happ_thermstat
# config block whose <internalAddress> is "thermostatSettings". busybox-safe awk.
dev_therm_guid() {
    [ -r "$THERM_CFG" ] || return 0
    awk -F'[<>]' '/<uuid>/{u=$3} /thermostatSettings/{print u; exit}' "$THERM_CFG" 2>/dev/null
}

case "${1:-}" in
  install)
    [ $# -ge 5 ] || { echo "usage: $0 install <host> <port> <user> <pass>"; exit 2; }
    printf 'HOST=%s\nPORT=%s\nUSER=%s\nPASS=%s\n' "$2" "$3" "$4" "$5" > "$CONF"
    sed -i '/^tmir:/d; /^tmip:/d; /^tmis:/d' "$IT"
    printf 'tmir:345:respawn:%s --exec thermostat\n' "$SELF" >> "$IT"
    printf 'tmip:345:respawn:%s --exec power\n'      "$SELF" >> "$IT"
    printf 'tmis:345:respawn:%s --exec humidity\n'   "$SELF" >> "$IT"
    reload_init
    echo "installed mirror rows (thermostat/power/humidity) -> $2:$3"
    ;;
  --undo)
    sed -i '/^tmir:/d; /^tmip:/d; /^tmis:/d' "$IT"
    reload_init
    pkill -x boxtalk_mirror 2>/dev/null || true
    echo "removed mirror rows + stopped instances"
    ;;
  --exec)
    kind="${2:-thermostat}"
    [ -r "$CONF" ] || { echo "no $CONF"; sleep 5; exit 1; }
    . "$CONF"
    case "$kind" in
      thermostat)
        LOG="$LOGDIR/boxtalk_mirror.log"
        GUID=$(dev_therm_guid); DG=""; [ -n "$GUID" ] && DG="--devguid $GUID"
        [ -f "$LOG" ] && [ "$(wc -c <"$LOG" 2>/dev/null || echo 0)" -gt 1048576 ] && : > "$LOG"
        exec "$BIN" $DG "$HOST" "$PORT" "$USER" "$PASS" thermostat >>"$LOG" 2>&1 ;;
      power)
        LOG="$LOGDIR/bxm_power.log"
        [ -f "$LOG" ] && [ "$(wc -c <"$LOG" 2>/dev/null || echo 0)" -gt 524288 ] && : > "$LOG"
        exec "$BIN" --daemon happ_pwrusage "$HOST" "$PORT" "$USER" "$PASS" all >>"$LOG" 2>&1 ;;
      humidity)
        LOG="$LOGDIR/bxm_humidity.log"
        [ -f "$LOG" ] && [ "$(wc -c <"$LOG" 2>/dev/null || echo 0)" -gt 524288 ] && : > "$LOG"
        exec "$BIN" --daemon hdrv_sensory "$HOST" "$PORT" "$USER" "$PASS" all >>"$LOG" 2>&1 ;;
      *) echo "unknown kind $kind"; sleep 5; exit 1 ;;
    esac
    ;;
  *)
    echo "usage: $0 install <host> <port> <user> <pass> | --undo | --exec <kind>"
    exit 2 ;;
esac
