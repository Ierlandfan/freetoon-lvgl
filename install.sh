#!/usr/bin/env bash
# Toon Custom UI installer.
#
# Pushes the toonui binary + companion bridges to a stock Toon and wires
# inittab entries so they start on boot. Idempotent — re-running upgrades
# the binaries and refreshes config without duplicating inittab rows.
#
# Companions:
#   - toonui            LVGL UI (replaces qt-gui), serves PWA on :10081
#   - quby_bridge       Quby<->OT bridge: proxy (default), active, or off
#   - p1bridge          HomeWizard P1 -> BoxTalk publisher
#   - toontap           Cross-compiled touch-event injector (debug helper)
#   - ot_mode_switch.sh Helper called by toonui Settings UI to flip
#                       off/proxy/wireless modes (rewrites inittab + OTGW GW)
#   - PWA static files  index.html / app.js / sw.js / icon-192.png in
#                       /mnt/data/pwa/ — served by toonui's :10081 endpoint
#
# Required input:
#   TOON_HOST   (default: 192.168.3.212)
#   TOON_PASS   (default: toon)
#
# Optional input (passed straight through to the bridges):
#   VENT_USER, VENT_PASS         — Itho-Wifi credentials  → /mnt/data/vent.conf
#   P1_TOKEN                     — HomeWizard P1 v2 bearer → /mnt/data/p1bridge.conf
#                                  Without this the p1bridge sidecar is NOT
#                                  installed → hcb_rrd stays bound to the
#                                  stock meteradapter (basic install).
#   WTR_TOKEN                    — HomeWizard HWE-WTR v2 bearer (second line in
#                                  /mnt/data/p1bridge.conf, prefix 192.168.99.115=)
#   HA_TOKEN                     — Home Assistant Long-Lived Access Token →
#                                  /mnt/data/ha.cfg. Omit for basic install.
#
# Mode flags:
#   --basic   Skip companion install (p1bridge, quby_bridge, ha.cfg, vent.conf)
#             unless the matching env var is set. Use this when the target
#             Toon is a fresh / spare unit with stock meteradapter still in
#             place and you want toonui as a pure cosmetic replacement.
#             toonui boots with all integrations OFF; user opt-ins via
#             Settings → Integrations.
#
# Required external dep on the install host: sshpass, ssh, scp.
#
# Usage:
#   ./install.sh                  # full install (legacy behaviour)
#   ./install.sh --basic          # toonui + PWA only, integrations off
#   TOON_HOST=192.168.1.50 ./install.sh
#   ./install.sh --uninstall      # remove inittab entries + delete binaries

set -euo pipefail

TOON_HOST="${TOON_HOST:-192.168.3.212}"
TOON_PASS="${TOON_PASS:-toon}"
TOON_USER="${TOON_USER:-root}"
HERE="$(cd "$(dirname "$0")" && pwd)"

# --basic short-circuits the optional companion install. Auto-enabled
# when no integration env vars are set, so a bare `./install.sh` on a
# stock-Toon target Just Works™ without the user having to remember
# the flag. Override by setting any of P1_TOKEN / WTR_TOKEN / VENT_*
# / HA_TOKEN, which switches us back to "full" mode for those bits.
BASIC=0
for arg in "$@"; do
    [[ "$arg" == "--basic" ]] && BASIC=1
done
if (( BASIC == 0 )) && \
   [[ -z "${P1_TOKEN:-}${WTR_TOKEN:-}${VENT_USER:-}${VENT_PASS:-}${HA_TOKEN:-}" ]]; then
    BASIC=1
    echo "(no P1_TOKEN/WTR_TOKEN/VENT_*/HA_TOKEN set — installing in --basic mode)"
fi

# Where artefacts live. Two layouts are supported without a flag:
#   release tarball  → binaries live next to install.sh ("$HERE/toonui",
#                      "$HERE/p1bridge", optional "$HERE/quby_bridge"),
#                      PWA in "$HERE/pwa/", helper in "$HERE/ot_mode_switch.sh".
#   dev tree         → binaries live inside each component's build subdir
#                      (lvgl_ui_recovered/build/toonui, p1bridge/p1bridge, …).
# Env-vars (TOONUI_BIN=… etc.) override the auto-pick when set.
pick_artefact() {
    # Usage: pick_artefact VAR_NAME candidate1 candidate2 …
    # Echoes the first existing path. Honours $VAR_NAME if pre-set.
    local var="$1"; shift
    local override="${!var:-}"
    if [[ -n "$override" ]]; then printf '%s\n' "$override"; return; fi
    for cand in "$@"; do
        if [[ -e "$cand" ]]; then printf '%s\n' "$cand"; return; fi
    done
    printf '%s\n' "$1"   # echo first candidate so error messages stay useful
}

TOONUI_BIN="$(pick_artefact TOONUI_BIN  "$HERE/toonui"        "$HERE/lvgl_ui_recovered/build/toonui")"
QUBY_BIN="$(  pick_artefact QUBY_BIN    "$HERE/quby_bridge"   "$HERE/quby_bridge/quby_bridge")"
P1_BIN="$(    pick_artefact P1_BIN      "$HERE/p1bridge"      "$HERE/p1bridge/p1bridge")"
TOONTAP_BIN="$(pick_artefact TOONTAP_BIN "$HERE/toontap"      "$HERE/../qt_rebuild/toontap")"
OT_MODE_SCRIPT="$(pick_artefact OT_MODE_SCRIPT "$HERE/ot_mode_switch.sh" "$HERE/scripts/ot_mode_switch.sh")"
if [[ -z "${PWA_DIR:-}" ]]; then
    if   [[ -f "$HERE/pwa/index.html"        ]]; then PWA_DIR="$HERE/pwa"
    elif [[ -f "$HERE/pwa_static/index.html" ]]; then PWA_DIR="$HERE/pwa_static"
    else PWA_DIR="$HERE/pwa_static"; fi
fi

SSH="sshpass -p $TOON_PASS ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR $TOON_USER@$TOON_HOST"
SCP="sshpass -p $TOON_PASS scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

# Inittab rows we own. <id>:<runlevels>:<action>:<command>
# Keep the id stable so re-runs upgrade cleanly instead of stacking.
TOONUI_LINE="toon:345:respawn:/mnt/data/toonui >> /var/volatile/tmp/toonui.log 2>&1"
# Default to proxy mode — shuttles bytes happ_thermstat<->keteladapter 1:1
# AND publishes BoilerInfo to BoxTalk. Original heat path preserved, PWA
# boiler card lit. Users can flip to off/wireless via toonui Settings UI
# (which rewrites this row via /mnt/data/ot_mode_switch.sh).
QUBY_LINE="qbri:345:respawn:/mnt/data/quby_bridge -m proxy >> /var/volatile/tmp/quby_bridge.log 2>&1"
P1_LINE="p1br:345:respawn:/mnt/data/p1bridge >> /var/volatile/tmp/p1bridge.log 2>&1"

# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------
require() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "ERROR: '$1' not on PATH — apt-get install sshpass" >&2
        exit 2
    }
}

remote() { $SSH "$@"; }

check_artefacts() {
    local missing=0
    # REQUIRED — toonui itself + the mode-switch helper + PWA bundle.
    if [[ ! -x "$TOONUI_BIN" ]]; then
        echo "  missing (required): $TOONUI_BIN" >&2
        missing=1
    fi
    # p1bridge is REQUIRED in full mode (anyone passing P1/WTR tokens), but
    # OPTIONAL in basic mode (target keeps stock meteradapter). Treating it
    # as optional in basic mode means a release tarball can omit the binary
    # entirely for stock-Toon installs.
    if (( BASIC == 0 )) && [[ ! -x "$P1_BIN" ]]; then
        echo "  missing (required for full install): $P1_BIN" >&2
        missing=1
    fi
    if [[ ! -f "$OT_MODE_SCRIPT" ]]; then
        echo "  missing (required): $OT_MODE_SCRIPT (mode-switch helper)" >&2
        missing=1
    fi
    if [[ ! -f "$PWA_DIR/index.html" ]]; then
        echo "  missing (required) PWA static dir: $PWA_DIR/index.html" >&2
        missing=1
    fi
    # OPTIONAL — release tarballs may skip these; warn but don't fail.
    [[ -x "$QUBY_BIN"    ]] || echo "  note: $QUBY_BIN absent — quby_bridge will not be installed/started" >&2
    [[ -x "$TOONTAP_BIN" ]] || echo "  note: $TOONTAP_BIN absent — toontap (debug helper) will not be installed" >&2
    if (( missing )); then
        echo >&2
        echo "If you're using a release tarball, expand it first and run install.sh from inside it." >&2
        echo "If you're building from source, run 'make' in lvgl_ui_recovered/src/ and p1bridge/." >&2
        exit 3
    fi
}

# Push a binary atomically: scp to .new, rename in place — survives
# overwriting a running ELF (kernel keeps the old text mapped).
push_atomic() {
    local src="$1" dest="$2"
    echo "  → $dest"
    $SCP "$src" "$TOON_USER@$TOON_HOST:${dest}.new"
    remote "mv -f ${dest}.new ${dest} && chmod +x ${dest}"
}

write_remote_file() {
    local content="$1" dest="$2"
    echo "  → $dest"
    printf '%s' "$content" | remote "cat > ${dest}.new && mv -f ${dest}.new ${dest}"
}

# Replace-or-append a single inittab row keyed on its leading "id:" field.
# Always pkill the existing process so respawn picks the new binary.
upsert_inittab_row() {
    local row="$1"
    local id="${row%%:*}"
    remote "grep -v '^${id}:' /etc/inittab > /etc/inittab.new && echo '${row}' >> /etc/inittab.new && mv -f /etc/inittab.new /etc/inittab"
}

drop_inittab_row() {
    local id="$1"
    remote "grep -v '^${id}:' /etc/inittab > /etc/inittab.new && mv -f /etc/inittab.new /etc/inittab"
}

reload_init() {
    # kill -HUP 1 makes init re-read /etc/inittab without rebooting.
    remote "kill -HUP 1"
}

# ----------------------------------------------------------------------
# Install
# ----------------------------------------------------------------------
do_install() {
    echo "[1/6] Checking artefacts..."
    check_artefacts

    echo "[2/6] Pushing binaries to $TOON_HOST..."
    push_atomic "$TOONUI_BIN"   "/mnt/data/toonui"
    push_atomic "$OT_MODE_SCRIPT" "/mnt/data/ot_mode_switch.sh"
    # p1bridge + quby_bridge are full-install only — basic install keeps the
    # stock meteradapter and skips the OT bridge entirely.
    if (( BASIC == 0 )); then
        [[ -x "$P1_BIN"      ]] && push_atomic "$P1_BIN"      "/mnt/data/p1bridge"
        [[ -x "$QUBY_BIN"    ]] && push_atomic "$QUBY_BIN"    "/mnt/data/quby_bridge"
    fi
    [[ -x "$TOONTAP_BIN" ]] && push_atomic "$TOONTAP_BIN" "/mnt/data/toontap"

    echo "[2b/6] Pushing PWA static files to /mnt/data/pwa/..."
    remote "mkdir -p /mnt/data/pwa"
    for f in index.html app.js sw.js manifest.json icon-192.png; do
        if [[ -f "$PWA_DIR/$f" ]]; then
            echo "  → /mnt/data/pwa/$f"
            $SCP "$PWA_DIR/$f" "$TOON_USER@$TOON_HOST:/mnt/data/pwa/${f}.new"
            remote "mv -f /mnt/data/pwa/${f}.new /mnt/data/pwa/${f}"
        fi
    done

    echo "[3/6] Writing companion configs..."
    if [[ -n "${VENT_USER:-}" && -n "${VENT_PASS:-}" ]]; then
        write_remote_file "$VENT_USER:$VENT_PASS"$'\n' /mnt/data/vent.conf
    else
        echo "  (skip vent.conf — set VENT_USER + VENT_PASS to write it)"
    fi
    # Build /mnt/data/p1bridge.conf from whichever of P1_TOKEN / WTR_TOKEN
    # the user provided. Either line is independently optional; absent
    # tokens mean toonui's auto-detect leaves that integration off.
    local p1cfg=""
    [[ -n "${P1_TOKEN:-}"  ]] && p1cfg+="192.168.99.69=$P1_TOKEN"$'\n'
    [[ -n "${WTR_TOKEN:-}" ]] && p1cfg+="192.168.99.115=$WTR_TOKEN"$'\n'
    if [[ -n "$p1cfg" ]]; then
        write_remote_file "$p1cfg" /mnt/data/p1bridge.conf
    else
        echo "  (skip p1bridge.conf — set P1_TOKEN and/or WTR_TOKEN to write it)"
    fi
    if [[ -n "${HA_TOKEN:-}" ]]; then
        write_remote_file "$HA_TOKEN"$'\n' /mnt/data/ha.cfg
        remote "chmod 600 /mnt/data/ha.cfg"
    else
        echo "  (skip ha.cfg — set HA_TOKEN to write it)"
    fi

    echo "[4/6] Stopping any running instances so the new binary respawns..."
    remote "pkill -x toonui      2>/dev/null; pkill -x quby_bridge 2>/dev/null; pkill -x p1bridge 2>/dev/null; true"
    # quby_bridge bind-mounts a PTY over /dev/ttymxc0. A stale mount blocks
    # the new instance; lazy-unmount lets existing happ_thermstat fd linger
    # while freeing the path so re-bind succeeds.
    remote "umount -l /dev/ttymxc0 2>/dev/null; true"

    echo "[5/6] Wiring /etc/inittab..."
    upsert_inittab_row "$TOONUI_LINE"
    # p1br + qbri rows only when those binaries were actually pushed. On a
    # basic install we drop any stale rows so init doesn't keep trying to
    # respawn binaries that no longer exist on disk.
    if (( BASIC == 0 )) && [[ -x "$P1_BIN" ]]; then
        upsert_inittab_row "$P1_LINE"
    else
        drop_inittab_row p1br
    fi
    if (( BASIC == 0 )) && [[ -x "$QUBY_BIN" ]]; then
        upsert_inittab_row "$QUBY_LINE"
    else
        drop_inittab_row qbri
    fi

    echo "[6/6] Reloading init (kill -HUP 1)..."
    reload_init

    echo
    echo "Install complete. Sanity-check:"
    sleep 4
    remote "pgrep -fa 'toonui|quby_bridge|p1bridge'"
}

# ----------------------------------------------------------------------
# Uninstall
# ----------------------------------------------------------------------
do_uninstall() {
    echo "[1/3] Dropping inittab rows..."
    drop_inittab_row toon
    drop_inittab_row qbri
    drop_inittab_row p1br

    echo "[2/3] Killing running processes..."
    remote "pkill -x toonui      2>/dev/null; pkill -x quby_bridge 2>/dev/null; pkill -x p1bridge 2>/dev/null; true"
    remote "umount -l /dev/ttymxc0 2>/dev/null; true"

    echo "[3/3] Removing binaries + configs + PWA + script..."
    remote "rm -f /mnt/data/toonui /mnt/data/quby_bridge /mnt/data/p1bridge /mnt/data/toontap /mnt/data/ot_mode_switch.sh /mnt/data/vent.conf /mnt/data/p1bridge.conf"
    remote "rm -rf /mnt/data/pwa"

    reload_init
    echo "Uninstalled."
}

# ----------------------------------------------------------------------
require sshpass
require ssh
require scp

case "${1:-install}" in
    install|""|--basic)  do_install   ;;
    --uninstall)         do_uninstall ;;
    -h|--help)
        sed -n '2,40p' "$0"
        ;;
    *) echo "Unknown command '$1' — see --help" >&2; exit 1 ;;
esac
