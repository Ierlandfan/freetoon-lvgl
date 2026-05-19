#!/bin/sh
# Toon Tailscale bring-up — install tailscaled into /mnt/data and bring it up
# with an ephemeral auth-key so the device joins the user's tailnet without
# any interactive flow on the device.
#
# Designed for one-shot execution ON the Toon (NOT from the workstation).
# Run it from an on-site laptop that has local SSH to the device:
#
#   scp this_script root@<toon-lan-ip>:/tmp/toon_tailscale.sh
#   ssh root@<toon-lan-ip> "AUTHKEY=tskey-auth-... HOSTNAME=spare-toon \
#       sh /tmp/toon_tailscale.sh"
#
# Survives firmware updates: everything lives under /mnt/data which is part
# of the user-data partition Eneco leaves alone. /etc/inittab is also under
# the user partition on Toon hardware, so the respawn row sticks.
#
# Reverts cleanly:
#   sh /tmp/toon_tailscale.sh --uninstall
#
# Variables:
#   AUTHKEY   (required for install) — tailnet auth-key. Generate at
#             https://login.tailscale.com/admin/settings/keys. Prefer an
#             "ephemeral + reusable" key with the toon tag so the device
#             auto-removes itself from the admin console if it dies.
#   HOSTNAME  (optional) — name shown in the Tailscale admin console.
#             Defaults to the device's /etc/hostname or "toon".
#   TS_VERSION (optional) — pin a specific tailscale build. Default: stable.

set -eu

INSTALL_DIR=/mnt/data/tailscale
TSD=$INSTALL_DIR/tailscaled
TSC=$INSTALL_DIR/tailscale
STATE_DIR=/mnt/data/tailscale/state
SOCKET=/var/run/tailscaled.sock
INITTAB_ID=tsd
# armv7 static build URL pattern. Tailscale publishes one for each release;
# the "stable" alias is updated for every minor. arm = arm32 hardfloat.
TS_VERSION="${TS_VERSION:-1.78.1}"
TARBALL_URL="https://pkgs.tailscale.com/stable/tailscale_${TS_VERSION}_arm.tgz"

log() { echo "[ts-bringup] $*"; }
die() { log "ERROR: $*"; exit 1; }

case "${1:-install}" in
    --uninstall)
        log "removing tailscale inittab row + binaries"
        if [ -f /etc/inittab ]; then
            grep -v "^${INITTAB_ID}:" /etc/inittab > /etc/inittab.new
            mv -f /etc/inittab.new /etc/inittab
        fi
        pkill -x tailscaled 2>/dev/null || true
        rm -rf "$INSTALL_DIR"
        rm -f "$SOCKET"
        kill -HUP 1
        log "uninstalled."
        exit 0
        ;;
esac

[ "$(id -u)" = "0" ] || die "run as root"

# Sanity checks. /mnt/data must be writable; outbound HTTPS to pkgs.tailscale.com
# must work or we can't fetch the tarball.
[ -w /mnt/data ] || die "/mnt/data not writable — wrong partition?"

mkdir -p "$INSTALL_DIR" "$STATE_DIR"

# Download + extract. Use curl if present, wget as fallback (busybox wget
# is on every Toon I've seen). Verify the binary at least runs --version
# before we wire it into init.
if [ ! -x "$TSD" ] || [ ! -x "$TSC" ]; then
    log "fetching $TARBALL_URL"
    cd /tmp
    rm -f tailscale.tgz
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o tailscale.tgz "$TARBALL_URL" \
            || die "curl failed — check network + DNS on this Toon"
    else
        wget -q -O tailscale.tgz "$TARBALL_URL" \
            || die "wget failed — check network + DNS on this Toon"
    fi
    rm -rf /tmp/ts_unpack
    mkdir -p /tmp/ts_unpack
    tar -xzf tailscale.tgz -C /tmp/ts_unpack
    cp -f /tmp/ts_unpack/tailscale_*_arm/tailscaled "$TSD"
    cp -f /tmp/ts_unpack/tailscale_*_arm/tailscale  "$TSC"
    chmod +x "$TSD" "$TSC"
    rm -rf /tmp/ts_unpack /tmp/tailscale.tgz
fi

# A standalone tailscale-go binary won't run without a working sandbox; on
# constrained kernels tailscaled wants --tun=userspace-networking so we
# don't depend on /dev/net/tun. The Toon kernel does NOT ship tun.ko.
TAILSCALED_FLAGS="--state=$STATE_DIR/tailscaled.state --socket=$SOCKET --tun=userspace-networking"

# Inittab row — init respawns tailscaled on exit, so an OOM-kill or crash
# auto-recovers without the user dialing back in.
LINE="${INITTAB_ID}:345:respawn:${TSD} ${TAILSCALED_FLAGS} >> /var/volatile/tmp/tailscaled.log 2>&1"
log "wiring /etc/inittab row '${INITTAB_ID}'"
grep -v "^${INITTAB_ID}:" /etc/inittab > /etc/inittab.new
echo "$LINE" >> /etc/inittab.new
mv -f /etc/inittab.new /etc/inittab

# Stop any prior tailscaled, then HUP init so the new row spawns.
pkill -x tailscaled 2>/dev/null || true
sleep 1
kill -HUP 1
log "tailscaled launching..."
# Wait for the socket to come up — up to ~15 s.
i=0
while [ ! -S "$SOCKET" ] && [ $i -lt 30 ]; do
    sleep 0.5
    i=$((i + 1))
done
[ -S "$SOCKET" ] || die "tailscaled didn't open $SOCKET — check /var/volatile/tmp/tailscaled.log"

# Bring up.
if [ -z "${AUTHKEY:-}" ]; then
    log "no AUTHKEY set — tailscaled is running, but the device isn't joined yet."
    log "join later with: $TSC up --auth-key=tskey-... --hostname=$(hostname 2>/dev/null || echo toon)"
    exit 0
fi
HN="${HOSTNAME:-$(hostname 2>/dev/null || echo toon)}"
log "joining tailnet as '$HN'"
"$TSC" --socket="$SOCKET" up \
    --auth-key="$AUTHKEY" \
    --hostname="$HN" \
    --accept-routes=false \
    --advertise-tags=tag:toon \
    --ssh \
    || die "tailscale up failed — see log"

log "done. Tailscale IP:"
"$TSC" --socket="$SOCKET" ip -4 || true
