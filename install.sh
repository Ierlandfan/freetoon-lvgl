#!/usr/bin/env bash
# freetoon-lvgl one-click installer.
#
# Replaces the stock Eneco qt-gui with the LVGL UI, sets up the boot picker
# so the user can always switch back, and turns on the VNC respawn so the
# Toon's screen is reachable remotely. Idempotent — re-run to upgrade.
#
# All optional integrations (HomeWizard P1 / WTR, Itho ventilation, Home
# Assistant) are OFF by default; flip them on later from Settings →
# Integrations on the Toon.
#
# Usage:
#   ./install.sh                                 # uses defaults below
#   TOON_HOST=192.168.x.x ./install.sh           # different IP
#   ./install.sh --uninstall                     # back to stock qt-gui
#
# Required on the install host: sshpass, ssh, scp.
#
# Run this script from inside the release tarball OR from a checkout of
# the dev tree (the binaries / scripts get resolved either way).

set -euo pipefail

TOON_HOST="${TOON_HOST:-192.168.3.212}"
TOON_PASS="${TOON_PASS:-toon}"
TOON_USER="${TOON_USER:-root}"
HERE="$(cd "$(dirname "$0")" && pwd)"

# Resolve each artefact — release tarballs put them next to install.sh,
# dev checkouts keep them under each component's build dir.
pick() {
    local var="$1"; shift
    local v="${!var:-}"; [[ -n "$v" ]] && { echo "$v"; return; }
    for c in "$@"; do [[ -e "$c" ]] && { echo "$c"; return; }; done
    echo "$1"   # echo first candidate for clearer error messages
}

TOONUI_BIN="$(pick TOONUI_BIN       "$HERE/toonui"             "$HERE/lvgl_ui_recovered/build/toonui")"
UI_LAUNCHER="$(pick UI_LAUNCHER     "$HERE/ui_launcher.sh"     "$HERE/scripts/ui_launcher.sh")"
COMPANION_GATE="$(pick COMPANION_GATE "$HERE/companion_gate.sh" "$HERE/scripts/companion_gate.sh")"
INTEG_INSTALLER="$(pick INTEG_INSTALLER "$HERE/integrations-install.sh" "$HERE/scripts/integrations_install.sh")"
TOONVNC_SH="$(pick TOONVNC_SH       "$HERE/toonvnc.sh"         "$HERE/toonvnc.sh")"
OT_MODE_SH="$(pick OT_MODE_SH       "$HERE/ot_mode_switch.sh"  "$HERE/scripts/ot_mode_switch.sh")"
FBVNC_INPUT="$(pick FBVNC_INPUT     "$HERE/fbvnc_input"        "$HERE/../qt_rebuild/fbvnc_input")"
PWA_DIR="${PWA_DIR:-}"
if [[ -z "$PWA_DIR" ]]; then
    for d in "$HERE/pwa" "$HERE/pwa_static"; do
        [[ -f "$d/index.html" ]] && PWA_DIR="$d" && break
    done
    : "${PWA_DIR:=$HERE/pwa_static}"   # fallback for error message
fi

SSH="sshpass -p $TOON_PASS ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR $TOON_USER@$TOON_HOST"
SCP="sshpass -p $TOON_PASS scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

# `toon:` runs ui_launcher.sh — it shows the 10 s boot picker, then exec's
# either toonui or /qmf/sbin/qt-gui based on /mnt/data/ui_choice. Keep the
# id stable so re-runs upgrade cleanly.
TOON_ROW="toon:345:respawn:/mnt/data/ui_launcher.sh >> /var/volatile/tmp/toonui.log 2>&1"
VNCS_ROW="vncs:345:respawn:/mnt/data/toonvnc.sh respawn >> /var/volatile/tmp/x11vnc.log 2>&1"

require() { command -v "$1" >/dev/null 2>&1 || { echo "ERROR: '$1' not on PATH — apt-get install sshpass" >&2; exit 2; }; }
remote() { $SSH "$@"; }
push() {
    local src="$1" dest="$2"
    echo "  → $dest"
    $SCP "$src" "$TOON_USER@$TOON_HOST:${dest}.new"
    remote "mv -f ${dest}.new ${dest} && chmod +x ${dest}"
}
upsert_row() {
    local row="$1" id="${1%%:*}"
    remote "grep -v '^${id}:' /etc/inittab > /etc/inittab.new && echo '${row}' >> /etc/inittab.new && mv -f /etc/inittab.new /etc/inittab"
}
drop_row() {
    remote "grep -v '^${1}:' /etc/inittab > /etc/inittab.new && mv -f /etc/inittab.new /etc/inittab"
}

check_artefacts() {
    local missing=0
    for pair in "toonui:$TOONUI_BIN" "ui_launcher.sh:$UI_LAUNCHER" \
                "companion_gate.sh:$COMPANION_GATE" "toonvnc.sh:$TOONVNC_SH" \
                "ot_mode_switch.sh:$OT_MODE_SH"; do
        local name="${pair%%:*}" path="${pair#*:}"
        if [[ ! -e "$path" ]]; then
            echo "  missing $name: $path" >&2
            missing=1
        fi
    done
    if [[ ! -f "$PWA_DIR/index.html" ]]; then
        echo "  missing PWA bundle: $PWA_DIR/index.html" >&2
        missing=1
    fi
    [[ -x "$FBVNC_INPUT" ]] || echo "  note: $FBVNC_INPUT absent — VNC will be view-only" >&2
    (( missing )) && {
        echo >&2
        echo "If this is a release tarball, expand it first and run install.sh from inside it." >&2
        echo "If this is the dev tree, build toonui with 'make' in lvgl_ui_recovered/src/." >&2
        exit 3
    }
}

do_install() {
    echo "[1/5] Verifying artefacts..."
    check_artefacts

    echo "[2/5] Pushing binaries + scripts to $TOON_HOST..."
    push "$TOONUI_BIN"     /mnt/data/toonui
    push "$UI_LAUNCHER"    /mnt/data/ui_launcher.sh
    push "$COMPANION_GATE" /mnt/data/companion_gate.sh
    push "$INTEG_INSTALLER" /mnt/data/integrations-install.sh
    push "$TOONVNC_SH"     /mnt/data/toonvnc.sh
    push "$OT_MODE_SH"     /mnt/data/ot_mode_switch.sh
    [[ -x "$FBVNC_INPUT" ]] && push "$FBVNC_INPUT" /mnt/data/fbvnc_input

    echo "[3/5] Pushing PWA static files..."
    remote "mkdir -p /mnt/data/pwa"
    for f in index.html app.js sw.js manifest.json icon-192.png; do
        [[ -f "$PWA_DIR/$f" ]] || continue
        echo "  → /mnt/data/pwa/$f"
        $SCP "$PWA_DIR/$f" "$TOON_USER@$TOON_HOST:/mnt/data/pwa/${f}.new"
        remote "mv -f /mnt/data/pwa/${f}.new /mnt/data/pwa/${f}"
    done

    echo "[4/5] Seeding default config..."
    # ui_choice — pick freetoon on first install; respect existing user choice
    remote "[ -f /mnt/data/ui_choice ] || echo freetoon > /mnt/data/ui_choice"
    # toonui.cfg — write a basic-mode default ONLY if user has no cfg yet.
    # Existing installs keep their integration toggles.
    remote "[ -f /mnt/data/toonui.cfg ] || cat > /mnt/data/toonui.cfg" <<'CFG'
auto_dim_enabled=1
auto_dim_seconds=10
active_brightness=800
dim_brightness=80
boot_picker_enabled=1
enable_p1_elec=0
enable_p1_water=0
enable_vent=0
enable_ha=0
CFG

    echo "[5/5] Wiring inittab + restart..."
    # Drop legacy quby_bridge / p1bridge rows from earlier installs.
    # 2026-05-19: keeping the bridge in the /dev/ttymxc0 path breaks happ↔BA
    # framing and kills CV pressure. Default install no longer wires those
    # bridges; users who really want the PWA boiler card can re-add them by
    # hand. See project_quby_bridge_is_the_culprit_2026-05-19 in memory.
    drop_row qbri
    drop_row p1br
    remote "pkill -x quby_bridge 2>/dev/null; pkill -x p1bridge 2>/dev/null; true"
    # Drop any leftover bind-mount so happ_thermstat re-opens the real UART.
    remote "umount -l /dev/ttymxc0 2>/dev/null; true"
    upsert_row "$TOON_ROW"
    upsert_row "$VNCS_ROW"
    # Stop the running UI / VNC so init respawns through the new launcher.
    remote "pkill -9 -x toonui 2>/dev/null; pkill -9 -x x11vnc 2>/dev/null; true"
    remote "kill -HUP 1"

    echo
    echo "Installed. Toon should be running freetoon-lvgl in basic mode."
    echo "  • Boot picker: 10 s window at boot to switch to stock qt-gui"
    echo "  • Settings → UI mode: same toggle from inside the running UI"
    echo "  • Settings → Integrations: opt in to P1 / Itho / HA pollers"
}

do_uninstall() {
    echo "[1/3] Dropping inittab rows..."
    drop_row toon
    drop_row vncs
    drop_row p1br    # legacy rows from earlier installers
    drop_row qbri
    echo "[2/3] Stopping daemons..."
    remote "pkill -9 -x toonui 2>/dev/null; pkill -9 -x x11vnc 2>/dev/null; pkill -9 -x quby_bridge 2>/dev/null; pkill -9 -x p1bridge 2>/dev/null; true"
    echo "[3/3] Removing files..."
    remote "rm -f /mnt/data/toonui /mnt/data/ui_launcher.sh /mnt/data/companion_gate.sh /mnt/data/toonvnc.sh /mnt/data/ot_mode_switch.sh /mnt/data/fbvnc_input /mnt/data/ui_choice /mnt/data/toonvnc.plain /mnt/data/toonui.cfg"
    remote "rm -rf /mnt/data/pwa"
    remote "kill -HUP 1"
    echo "Uninstalled. Stock qt-gui takes over on next launcher cycle."
}

require sshpass
require ssh
require scp

case "${1:-install}" in
    install|"")  do_install   ;;
    --uninstall) do_uninstall ;;
    -h|--help)   sed -n '2,25p' "$0" ;;
    *) echo "Unknown command '$1' — see --help" >&2; exit 1 ;;
esac
