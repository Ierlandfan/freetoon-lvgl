#!/bin/sh
# Switch the Toon's OT data path between three topologies.
#
#   off        Original Toon behaviour: happ_thermstat owns /dev/ttymxc0
#              directly, no bridge. OTGW set to GW=1 (relay master traffic).
#              Bulletproof fallback — use if anything goes wrong with proxy.
#              No PWA boiler card (flow/return/ch_setpoint not published).
#
#   proxy      *** NEW DEFAULT, verified 2026-05-18 ***
#              quby_bridge in proxy mode: bind-mounts a PTY over
#              /dev/ttymxc0, opens the real UART, shuttles every byte
#              between happ_thermstat (via PTY) and keteladapter (via
#              UART) 1:1 without faking responses. Sniffs each Quby
#              frame and publishes BoilerInfo/ThermostatInfo notifies
#              on BoxTalk so toonui (and any future consumer) sees live
#              boiler flow/return/control-setpoint/burner. Original Toon
#              heating behaviour preserved (happ stooklijn unaffected).
#              OTGW still on GW=1.
#
#   wireless   quby_bridge in active mode: bind-mounts a PTY and FAKES
#              keteladapter responses, forwarding OT writes to OTGW
#              over HTTP. OTGW set to GW=2 (operate without thermostat).
#              Setpoint write path still blocked at OTGW firmware level
#              (HTTP CS=/CH= overrides not driving boiler). Keep for
#              future RE work; do not use for daily heating.
#
# Called from toonui's OT Bridge Apply button: /mnt/data/ot_mode_switch.sh off|proxy|wireless
#
# Survives reboot via the inittab edit (kill -HUP 1 re-reads it).
set -e
MODE="${1:-proxy}"
QBRI_PROXY='qbri:345:respawn:/mnt/data/quby_bridge -m proxy >> /var/volatile/tmp/quby_bridge.log 2>&1'
QBRI_ACTIVE='qbri:345:respawn:/mnt/data/quby_bridge -m active >> /var/volatile/tmp/quby_bridge.log 2>&1'
OTGW_HOST="${OTGW_HOST:-192.168.99.21}"

log() { echo "[$(date +%H:%M:%S)] $*"; }

set_otgw_cmd() {
    log "OTGW <- $1"
    /usr/bin/curl -s --max-time 5 -X POST \
        -H 'Content-Type: application/json' \
        -d "{\"command\":\"$1\"}" \
        "http://$OTGW_HOST/api/v0/otgw/command" > /dev/null || true
}
set_otgw_setting() {
    log "OTGW persist: $1=$2"
    /usr/bin/curl -s --max-time 5 -X POST \
        -H 'Content-Type: application/json' \
        -d "{\"name\":\"$1\",\"value\":\"$2\"}" \
        "http://$OTGW_HOST/api/v0/settings" > /dev/null || true
}

# Correct ordering matters: kill -HUP 1 must come BEFORE pkill of qbri,
# otherwise init re-spawns the bridge using the OLD inittab line (we saw
# this hit empirically — bridge starting in proxy mode despite a wireless
# flip). Also need a real sleep after the inittab swap so init's SIGHUP
# handler actually finishes re-reading the file before we kill anything.
apply_mode_swap() {
    local new_line="$1"     # inittab line to set, "" to remove qbri
    [ -f /etc/inittab ] || return 0

    if [ -x /mnt/data/inittab_edit.sh ]; then
        if [ -n "$new_line" ]; then
            /mnt/data/inittab_edit.sh upsert qbri "$new_line"
        else
            /mnt/data/inittab_edit.sh drop qbri
        fi
    else
        cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true
        grep -v '^qbri:' /etc/inittab > /etc/inittab.new 2>/dev/null || true
        [ -n "$new_line" ] && echo "$new_line" >> /etc/inittab.new
        if [ -s /etc/inittab.new ] && [ "$(wc -l < /etc/inittab.new 2>/dev/null || echo 0)" -ge 15 ]; then
            sync; mv -f /etc/inittab.new /etc/inittab; sync
        else
            rm -f /etc/inittab.new 2>/dev/null || true
            log "ERROR: inittab verification failed! Original inittab preserved."
            return 1
        fi
        kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
    fi
    sleep 2                 # init re-reads inittab
    # killall first: pkill does not exist on Toon 1 (busybox there has killall only)
    killall -9 quby_bridge 2>/dev/null || pkill -9 -x quby_bridge 2>/dev/null || true
    sleep 1
    umount -l /dev/ttymxc0 2>/dev/null || true
    sleep 1
    killall -9 happ_thermstat 2>/dev/null || pkill -9 happ_thermstat 2>/dev/null || true
    sleep 1
    # Second SIGHUP after the kills so init definitely spawns any new
    # respawn entries (qbri was empirically not spawning when going
    # off→wireless with only the pre-kill HUP). Cheap belt-and-braces.
    kill -HUP 1
}

case "$MODE" in
off|wired)   # accept legacy `wired` synonym
    log "switching to OFF mode (no bridge, original keteladapter behaviour)"
    set_otgw_cmd      "GW=1"
    set_otgw_setting  "otgwcommands" "GW=1"
    apply_mode_swap ""
    log "off mode applied — heat resumes in ~15s"
    ;;
proxy)
    log "switching to PROXY mode (bridge sniffs Quby + republishes BoilerInfo)"
    set_otgw_cmd      "GW=1"
    set_otgw_setting  "otgwcommands" "GW=1"
    apply_mode_swap "$QBRI_PROXY"
    log "proxy mode applied — bridge will sniff + publish; happ heat-path unchanged"
    ;;
wireless)
    log "switching to WIRELESS mode (bridge fakes keteladapter, OTGW drives boiler)"
    set_otgw_cmd      "GW=2"
    set_otgw_setting  "otgwcommands" "GW=2"
    apply_mode_swap "$QBRI_ACTIVE"
    log "wireless mode applied — happ handshake takes 45-60s, then writes via port 25238"
    ;;
*)
    echo "usage: $0 off|proxy|wireless" >&2
    exit 1
    ;;
esac
