#!/usr/bin/env bash
#
# connect_toon_tailscale.sh — ONE-CLICK Tailscale bring-up for a Toon.
#
# Run this on a laptop that is on the SAME LAN as the Toon. It SSHes in,
# installs the static arm Tailscale build into /mnt/data (survives firmware
# updates), wires an inittab respawn row, and joins your tailnet.
#
# Usage:
#   ./connect_toon_tailscale.sh <toon-ip> [user] [password]
#
#   <toon-ip>   required — the Toon's LAN address (e.g. 192.168.1.50)
#   user        optional — SSH user, default "root"
#   password    optional — SSH password; if omitted you'll be prompted
#
# Joining the tailnet:
#   - Default (no AUTHKEY): the script runs `tailscale up` interactively;
#     it prints a https://login.tailscale.com/a/... URL. Open it in your
#     browser, approve the device, and the script finishes + prints the
#     100.x.y.z tailnet IP. Zero key management.
#   - Unattended: export AUTHKEY=tskey-auth-... before running. Generate
#     it at https://login.tailscale.com/admin/settings/keys (reusable +
#     ephemeral recommended). Then it joins with no browser step.
#
# Optional env:
#   HOSTNAME    name in the Tailscale admin console (default: device name)
#   TS_VERSION  pin a tailscale build (default 1.78.1)
#
# Revert everything:
#   ./connect_toon_tailscale.sh <toon-ip> [user] [password] --uninstall
#
set -euo pipefail

TOON_IP="${1:-}"
if [ -z "$TOON_IP" ] || [ "$TOON_IP" = "-h" ] || [ "$TOON_IP" = "--help" ]; then
    sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi
shift

# Parse remaining positional args (user, password) and a trailing --uninstall.
TOON_USER="root"
TOON_PASS=""
DO_UNINSTALL=0
POS=()
for a in "$@"; do
    case "$a" in
        --uninstall) DO_UNINSTALL=1 ;;
        *)           POS+=("$a") ;;
    esac
done
[ "${#POS[@]}" -ge 1 ] && TOON_USER="${POS[0]}"
[ "${#POS[@]}" -ge 2 ] && TOON_PASS="${POS[1]}"

TS_VERSION="${TS_VERSION:-1.78.1}"
AUTHKEY="${AUTHKEY:-}"
TS_HOSTNAME="${HOSTNAME:-}"

command -v sshpass >/dev/null 2>&1 || {
    echo "ERROR: sshpass not found. Install it:" >&2
    echo "  Debian/Ubuntu : sudo apt install sshpass" >&2
    echo "  macOS (brew)  : brew install hudochenkov/sshpass/sshpass" >&2
    exit 2
}

if [ -z "$TOON_PASS" ]; then
    read -rsp "SSH password for ${TOON_USER}@${TOON_IP}: " TOON_PASS
    echo
fi

# -tt forces a pseudo-tty so `tailscale up`'s login URL streams live instead
# of being buffered until the command returns.
SSH=(sshpass -p "$TOON_PASS" ssh -tt
     -o StrictHostKeyChecking=no
     -o UserKnownHostsFile=/dev/null
     -o LogLevel=ERROR
     "${TOON_USER}@${TOON_IP}")

# ---------------------------------------------------------------------------
# Uninstall path
# ---------------------------------------------------------------------------
if [ "$DO_UNINSTALL" = "1" ]; then
    echo "[oneclick] uninstalling Tailscale from ${TOON_IP} ..."
    "${SSH[@]}" 'sh -s' <<'REMOTE'
set -eu
ID=tsd
[ -f /etc/inittab ] && {
    grep -v "^${ID}:" /etc/inittab > /etc/inittab.new
    mv -f /etc/inittab.new /etc/inittab
}
pkill -x tailscaled 2>/dev/null || true
rm -rf /mnt/data/tailscale
rm -f /var/run/tailscaled.sock
kill -HUP 1
echo "[ts] uninstalled."
REMOTE
    echo "[oneclick] done."
    exit 0
fi

# ---------------------------------------------------------------------------
# Install + bring-up. All on-device logic runs via a single here-doc so there
# is no dependency on fetching anything from GitHub onto the laptop.
# AUTHKEY / HOSTNAME / TS_VERSION are passed through the environment.
# ---------------------------------------------------------------------------
echo "[oneclick] connecting to ${TOON_USER}@${TOON_IP} ..."
"${SSH[@]}" \
    "AUTHKEY='${AUTHKEY}' TS_HOSTNAME='${TS_HOSTNAME}' TS_VERSION='${TS_VERSION}' sh -s" <<'REMOTE'
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

# Fetch + unpack the static arm build if not already present.
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

# The Toon kernel has no tun.ko, so run WireGuard in userspace.
FLAGS="--state=$STATE_DIR/tailscaled.state --socket=$SOCKET --tun=userspace-networking"

# inittab respawn row → auto-restart on crash / reboot.
LINE="${ID}:345:respawn:${TSD} ${FLAGS} >> /var/volatile/tmp/tailscaled.log 2>&1"
grep -v "^${ID}:" /etc/inittab > /etc/inittab.new
echo "$LINE" >> /etc/inittab.new
mv -f /etc/inittab.new /etc/inittab

pkill -x tailscaled 2>/dev/null || true
sleep 1
kill -HUP 1
log "tailscaled starting ..."

# Wait up to ~15 s for the control socket.
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
    log "(no tag is set on the interactive path; add one in the admin console if you want)"
    # No --auth-key → tailscale prints a login URL and blocks until you
    # approve the device in the browser, then returns.
    "$TSC" --socket="$SOCKET" up --hostname="$HN" --ssh \
        || die "tailscale up failed — see log"
fi

echo
log "JOINED. Tailnet IP:"
"$TSC" --socket="$SOCKET" ip -4 || true
REMOTE

echo
echo "[oneclick] Done. From any device on your tailnet:"
echo "  ssh ${TOON_USER}@<the-100.x-IP-printed-above>"
echo "  http://<the-100.x-IP-printed-above>:10081/   (PWA)"
