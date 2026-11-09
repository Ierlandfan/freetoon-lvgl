#!/bin/sh
#
# connect_toon_tailscale.sh — ONE-CLICK Tailscale bring-up for a Toon.
#
# Run this on a laptop that is on the SAME LAN as the Toon. It auto-discovers
# the Toon on your network (or you pass/enter its IP), SSHes in, installs the
# static arm Tailscale build into /mnt/data (survives firmware updates),
# wires an inittab respawn row, and joins your tailnet.
#
# Usage:
#   ./connect_toon_tailscale.sh                      # auto-discover, prompt for user/pass
#   ./connect_toon_tailscale.sh <toon-ip>            # skip discovery, use this IP
#   ./connect_toon_tailscale.sh <toon-ip> <user> <password>
#   ./connect_toon_tailscale.sh auto <user> <password>   # force discovery
#
#   <toon-ip>   the Toon's LAN address; omit (or "auto") to auto-discover
#   user        SSH user, default "root"
#   password    SSH password; if omitted you'll be prompted
#
# Joining the tailnet:
#   - Default (no AUTHKEY): runs `tailscale up` interactively; prints a
#     https://login.tailscale.com/a/... URL. Open it, approve the device,
#     done. Zero key management.
#   - Unattended: export AUTHKEY=tskey-auth-... (reusable + ephemeral
#     recommended, from https://login.tailscale.com/admin/settings/keys).
#
# Optional env:
#   HOSTNAME    name in the Tailscale admin console (default: device name)
#   TS_VERSION  pin a tailscale build (default 1.78.1)
#   SUBNET      override the /24 to scan (e.g. 192.168.1) — skips auto-detect
#
# Revert everything:
#   ./connect_toon_tailscale.sh <toon-ip> <user> <password> --uninstall
#
set -eu

TS_VERSION="${TS_VERSION:-1.78.1}"
AUTHKEY="${AUTHKEY:-}"
TS_HOSTNAME="${HOSTNAME:-}"

log()  { echo "[oneclick] $*"; }
err()  { echo "[oneclick] ERROR: $*" >&2; }

is_ipv4() { echo "$1" | grep -Eq '^([0-9]{1,3}\.){3}[0-9]{1,3}$'; }

# ---------------------------------------------------------------------------
# Argument parsing: first IP-looking arg = TOON_IP; "auto" forces discovery.
# Remaining non-flag args = user, password. Trailing --uninstall supported.
# ---------------------------------------------------------------------------
TOON_IP=""
TOON_USER="root"
TOON_PASS=""
DO_UNINSTALL=0
FORCE_DISCOVER=0
POS1=""
POS2=""
POS_COUNT=0
for a in "$@"; do
    case "$a" in
        -h|--help)   sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --uninstall) DO_UNINSTALL=1 ;;
        auto|AUTO)   FORCE_DISCOVER=1 ;;
        *)
            if [ -z "$TOON_IP" ] && is_ipv4 "$a"; then
                TOON_IP="$a"
            else
                if [ "$POS_COUNT" -eq 0 ]; then
                    POS1="$a"
                    POS_COUNT=1
                elif [ "$POS_COUNT" -eq 1 ]; then
                    POS2="$a"
                    POS_COUNT=2
                fi
            fi
            ;;
    esac
done
[ -n "$POS1" ] && TOON_USER="$POS1"
[ -n "$POS2" ] && TOON_PASS="$POS2"

# ---------------------------------------------------------------------------
# LAN discovery helpers
# ---------------------------------------------------------------------------
detect_subnet() {
    # Print the laptop's primary IPv4 /24 prefix (first three octets), e.g.
    # "192.168.1". Tries Linux `ip`, then macOS/BSD route+ipconfig, then
    # a generic ifconfig parse. Empty on failure.
    [ -n "${SUBNET:-}" ] && { echo "$SUBNET"; return; }
    local ip=""
    if command -v ip >/dev/null 2>&1; then
        ip=$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.*src \([0-9.]*\).*/\1/p' | head -n1)
    fi
    if [ -z "$ip" ] && command -v ipconfig >/dev/null 2>&1; then
        # macOS: default interface
        local dev
        dev=$(route -n get default 2>/dev/null | sed -n 's/.*interface: \(.*\)/\1/p')
        [ -n "$dev" ] && ip=$(ipconfig getifaddr "$dev" 2>/dev/null || true)
    fi
    if [ -z "$ip" ]; then
        # generic: first private RFC1918 addr from ifconfig
        ip=$(ifconfig 2>/dev/null | grep -oE 'inet (addr:)?[0-9.]+' \
             | grep -oE '[0-9.]+' \
             | grep -E '^(192\.168|10\.|172\.(1[6-9]|2[0-9]|3[01]))' \
             | head -n1)
    fi
    [ -n "$ip" ] && echo "${ip%.*}"
}

port_open() {  # host port  → 0 if TCP connect succeeds quickly
    local h="$1" p="$2"
    if command -v nc >/dev/null 2>&1; then
        nc -z -w1 "$h" "$p" >/dev/null 2>&1
    else
        # bash /dev/tcp with a background-kill timeout (works without `timeout`)
        ( exec 3<>"/dev/tcp/$h/$p" ) >/dev/null 2>&1 &
        local pid=$!
        ( sleep 1; kill "$pid" 2>/dev/null ) >/dev/null 2>&1 &
        local killer=$!
        if wait "$pid" 2>/dev/null; then kill "$killer" 2>/dev/null; return 0; fi
        return 1
    fi
}

is_toon() {  # ip → 0 if the HCB web server identifies this as a Toon
    local ip="$1"
    # happ_thermstat's HTTP action returns JSON with "currentTemp" — a strong
    # Toon signature that needs no credentials. Fall back to the HCB root.
    curl -s -m 3 "http://$ip/happ_thermstat?action=getThermostatInfo" 2>/dev/null \
        | grep -q "currentTemp" && return 0
    curl -s -m 3 "http://$ip/" 2>/dev/null | grep -qiE "qooxdoo|hcb|toon" && return 0
    return 1
}

discover_toons() {
    # Scan the local /24 for hosts answering on :80, confirm each is a Toon,
    # print confirmed IPs (one per line).
    sub=$(detect_subnet)
    if [ -z "$sub" ]; then
        err "could not detect your LAN subnet — set SUBNET=192.168.x or pass the IP."
        return 1
    fi
    log "scanning ${sub}.0/24 for a Toon (port 80 + HCB fingerprint) ..." >&2
    SCAN_TMP="/tmp/.toon_scan.$$"
    : > "$SCAN_TMP"
    if command -v nmap >/dev/null 2>&1; then
        nmap -n -p 80 --open -oG - "${sub}.0/24" 2>/dev/null \
            | sed -n 's/^Host: \([0-9.]*\).*80\/open.*/\1/p' > "$SCAN_TMP"
    else
        for i in $(seq 1 254); do
            { port_open "${sub}.${i}" 80 && echo "${sub}.${i}" >> "$SCAN_TMP"; } &
            if [ $(( i % 64 )) -eq 0 ]; then wait; fi
        done
        wait
    fi
    if [ -s "$SCAN_TMP" ]; then
        while read -r ip; do
            if is_toon "$ip"; then echo "$ip"; fi
        done < "$SCAN_TMP"
    fi
    rm -f "$SCAN_TMP"
}

prompt_manual_ip() {
    while :; do
        printf "Enter the Toon's LAN IP: "
        read -r ip
        if is_ipv4 "$ip"; then echo "$ip"; return 0; fi
        err "not a valid IPv4 address — try again." >&2
    done
}

# ---------------------------------------------------------------------------
# Resolve TOON_IP: explicit > discovery > manual prompt
# ---------------------------------------------------------------------------
if [ "$FORCE_DISCOVER" = "1" ]; then TOON_IP=""; fi
if [ -z "$TOON_IP" ]; then
    DISC_TMP="/tmp/.toon_disc.$$"
    discover_toons > "$DISC_TMP" 2>/dev/null || true
    c_count=$(wc -l < "$DISC_TMP" 2>/dev/null || echo 0)
    c_count=$(echo "$c_count" | tr -d '[:space:]')
    if [ "$c_count" -eq 1 ]; then
        read -r TOON_IP < "$DISC_TMP"
        log "found Toon at ${TOON_IP}"
    elif [ "$c_count" -gt 1 ]; then
        log "found $c_count Toons:"
        i=1
        while read -r c; do
            echo "   $i) $c"
            i=$((i + 1))
        done < "$DISC_TMP"
        printf "Pick one [1-%s], or type an IP: " "$c_count"
        read -r sel
        if is_ipv4 "$sel"; then
            TOON_IP="$sel"
        elif echo "$sel" | grep -Eq '^[0-9]+$' && [ "$sel" -ge 1 ] && [ "$sel" -le "$c_count" ]; then
            TOON_IP=$(sed -n "${sel}p" "$DISC_TMP")
        else
            err "invalid selection"; rm -f "$DISC_TMP"; exit 1
        fi
    else
        log "no Toon auto-discovered on the LAN."
        TOON_IP=$(prompt_manual_ip)
    fi
    rm -f "$DISC_TMP"
fi
log "target Toon: ${TOON_IP}"

# ---------------------------------------------------------------------------
# Credentials
# ---------------------------------------------------------------------------
command -v sshpass >/dev/null 2>&1 || {
    err "sshpass not found. Install it:"
    echo "  Debian/Ubuntu : sudo apt install sshpass" >&2
    echo "  macOS (brew)  : brew install hudochenkov/sshpass/sshpass" >&2
    exit 2
}
if [ -z "$TOON_PASS" ]; then
    printf "SSH password for %s@%s: " "$TOON_USER" "$TOON_IP"
    stty -echo 2>/dev/null || true
    read -r TOON_PASS
    stty echo 2>/dev/null || true
    echo
fi

# Helper function to run commands over SSH
run_ssh() {
    sshpass -p "$TOON_PASS" ssh -tt \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR \
        "${TOON_USER}@${TOON_IP}" "$@"
}

# ---------------------------------------------------------------------------
# Uninstall path
# ---------------------------------------------------------------------------
if [ "$DO_UNINSTALL" = "1" ]; then
    log "uninstalling Tailscale from ${TOON_IP} ..."
    run_ssh 'sh -s' <<'REMOTE'
set -eu
ID=tsd
[ -f /etc/inittab ] && {
    if [ -x /mnt/data/inittab_edit.sh ]; then
        /mnt/data/inittab_edit.sh drop "$ID"
    else
        cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true
        grep -v "^${ID}:" /etc/inittab > /etc/inittab.new 2>/dev/null || true
        if [ -s /etc/inittab.new ] && [ "$(wc -l < /etc/inittab.new 2>/dev/null || echo 0)" -ge 15 ]; then
            sync; mv -f /etc/inittab.new /etc/inittab; sync
        else
            rm -f /etc/inittab.new 2>/dev/null || true
        fi
    fi
}
pkill -x tailscaled 2>/dev/null || true
rm -rf /mnt/data/tailscale
rm -f /var/run/tailscaled.sock
kill -HUP 1
echo "[ts] uninstalled."
REMOTE
    log "done."
    exit 0
fi

# ---------------------------------------------------------------------------
# Install + bring-up (all on-device logic in one here-doc).
# ---------------------------------------------------------------------------
log "connecting to ${TOON_USER}@${TOON_IP} ..."
run_ssh "AUTHKEY='${AUTHKEY}' TS_HOSTNAME='${TS_HOSTNAME}' TS_VERSION='${TS_VERSION}' sh -s" <<'REMOTE'
set -eu

INSTALL_DIR=/mnt/data/tailscale
TSD=$INSTALL_DIR/tailscaled
TSC=$INSTALL_DIR/tailscale
STATE_DIR=$INSTALL_DIR/state
SOCKET=/var/run/tailscaled.sock
ID=tsd
TARBALL_URL="https://pkgs.tailscale.com/stable/tailscale_${TS_VERSION}_arm.tgz"

log() { echo "[ts] $*"; }
die() { echo "[ts] ERROR: $*" >&2; exit 1; }

[ "$(id -u)" = "0" ] || die "must run as root on the Toon"
[ -w /mnt/data ] || die "/mnt/data not writable — wrong partition?"

mkdir -p "$INSTALL_DIR" "$STATE_DIR"

if [ ! -x "$TSD" ] || [ ! -x "$TSC" ]; then
    log "downloading $TARBALL_URL"
    cd /tmp
    rm -f tailscale.tgz
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o tailscale.tgz "$TARBALL_URL" || die "download failed — Toon needs internet + DNS"
    else
        wget -q -O tailscale.tgz "$TARBALL_URL" || die "download failed — Toon needs internet + DNS"
    fi
    rm -rf /tmp/ts_unpack && mkdir -p /tmp/ts_unpack
    tar -xzf tailscale.tgz -C /tmp/ts_unpack
    cp -f /tmp/ts_unpack/tailscale_*_arm/tailscaled "$TSD"
    cp -f /tmp/ts_unpack/tailscale_*_arm/tailscale  "$TSC"
    chmod +x "$TSD" "$TSC"
    rm -rf /tmp/ts_unpack /tmp/tailscale.tgz
    log "installed tailscale ${TS_VERSION}"
else
    log "tailscale already present, reusing"
fi

# Toon kernel has no tun.ko → userspace WireGuard.
FLAGS="--state=$STATE_DIR/tailscaled.state --socket=$SOCKET --tun=userspace-networking"
LINE="${ID}:345:respawn:${TSD} ${FLAGS} >> /var/volatile/tmp/tailscaled.log 2>&1"
if [ -x /mnt/data/inittab_edit.sh ]; then
    /mnt/data/inittab_edit.sh upsert "$ID" "$LINE"
else
    cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true
    grep -v "^${ID}:" /etc/inittab > /etc/inittab.new 2>/dev/null || true
    echo "$LINE" >> /etc/inittab.new
    if [ -s /etc/inittab.new ] && [ "$(wc -l < /etc/inittab.new 2>/dev/null || echo 0)" -ge 15 ]; then
        sync; mv -f /etc/inittab.new /etc/inittab; sync
    else
        rm -f /etc/inittab.new 2>/dev/null || true
    fi
fi

pkill -x tailscaled 2>/dev/null || true
sleep 1
kill -HUP 1
log "tailscaled starting ..."

i=0
while [ ! -S "$SOCKET" ] && [ $i -lt 30 ]; do sleep 0.5; i=$((i+1)); done
[ -S "$SOCKET" ] || die "tailscaled didn't open $SOCKET — see /var/volatile/tmp/tailscaled.log"

HN="${TS_HOSTNAME:-$(hostname 2>/dev/null || echo toon)}"

if [ -n "$AUTHKEY" ]; then
    log "joining tailnet as '$HN' with auth-key (unattended)"
    "$TSC" --socket="$SOCKET" up \
        --auth-key="$AUTHKEY" --hostname="$HN" \
        --accept-routes=false --advertise-tags=tag:toon --ssh \
        || die "tailscale up failed — see log"
else
    log "joining tailnet as '$HN' — OPEN THE URL BELOW IN YOUR BROWSER:"
    "$TSC" --socket="$SOCKET" up --hostname="$HN" --ssh \
        || die "tailscale up failed — see log"
fi

echo
log "JOINED. Tailnet IP:"
"$TSC" --socket="$SOCKET" ip -4 || true
REMOTE

echo
log "Done. From any device on your tailnet:"
echo "  ssh ${TOON_USER}@<the-100.x-IP-printed-above>"
echo "  http://<the-100.x-IP-printed-above>:10081/   (PWA)"
