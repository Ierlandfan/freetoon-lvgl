#!/bin/sh
# switch_to_freetoon.sh — Install FreeToon if not present, or switch to FreeToon from stock qt-gui.
# Designed for Toon 1 and Toon 2.
# Can be run directly from shell or kicked off by TSC / a QML app.

set -e

# Prevent being killed if parent process (e.g. qt-gui / TSC child) exits
trap '' HUP TERM INT

# Decouple from parent process group / session so init killing startqt does not kill this script
if [ "${SWITCH_DETACHED:-0}" != "1" ]; then
    export SWITCH_DETACHED=1
    if command -v setsid >/dev/null 2>&1; then
        exec setsid "$0" "$@"
    elif command -v busybox >/dev/null 2>&1 && busybox --list 2>/dev/null | grep -q '^setsid$'; then
        exec busybox setsid "$0" "$@"
    elif command -v python >/dev/null 2>&1; then
        exec python -c "import os, sys; os.setsid(); os.execv(sys.argv[1], sys.argv[1:])" "$0" "$@"
    fi
fi

LOG="/var/volatile/tmp/switch_to_freetoon.log"
mkdir -p /var/volatile/tmp 2>/dev/null || true

log() {
    _msg="[$(date '+%Y-%m-%d %H:%M:%S')] [switch_to_freetoon] $*"
    echo "$_msg"
    echo "$_msg" >> "$LOG" 2>/dev/null || true
}

CHOICE_FILE="/mnt/data/ui_choice"
TOONUI_BIN="/mnt/data/toonui"
UI_LAUNCHER="/mnt/data/ui_launcher.sh"
INITTAB="/etc/inittab"
REPO_INSTALL_URL="https://raw.githubusercontent.com/ToonzDev/freetoon-lvgl/main/install.sh"

log "========================================================"
log "Starting FreeToon switch/install helper..."

# 1. Hardware detection
IS_TOON1=0
if [ -f /etc/opkg/arch.conf ] && ! grep -q nxt /etc/opkg/arch.conf 2>/dev/null; then
    IS_TOON1=1
    log "Hardware detected: Toon 1"
else
    log "Hardware detected: Toon 2"
fi

# Reset crash counters so launcher gives FreeToon a clean start
rm -f /var/volatile/tmp/toonui_fails /var/volatile/tmp/toonui_restart 2>/dev/null || true

# 2. Check if FreeToon is already installed
is_installed() {
    [ -f "$TOONUI_BIN" ] && [ -x "$TOONUI_BIN" ] && [ -f "$UI_LAUNCHER" ] && [ -x "$UI_LAUNCHER" ]
}

if ! is_installed; then
    log "FreeToon is NOT installed. Starting installation..."

    # Pre-set choice to freetoon so installer doesn't default to qt-gui on Toon 1
    echo "freetoon" > "$CHOICE_FILE"

    # Locate installer: local file or download
    INSTALLER=""
    HERE="$(cd "$(dirname "$0")" 2>/dev/null && pwd || echo "")"
    for cand in "$HERE/install.sh" "$HERE/../install.sh" /mnt/data/install.sh /root/install.sh; do
        if [ -f "$cand" ]; then
            INSTALLER="$cand"
            break
        fi
    done

    TMP_INSTALLER="/tmp/freetoon_install_$$.sh"
    if [ -z "$INSTALLER" ]; then
        log "Fetching latest install.sh from GitHub..."
        if command -v curl >/dev/null 2>&1; then
            curl -fsSL -4 --connect-timeout 10 --max-time 60 -o "$TMP_INSTALLER" "$REPO_INSTALL_URL" 2>/dev/null || \
            curl -k -fsSL --connect-timeout 10 --max-time 60 -o "$TMP_INSTALLER" "$REPO_INSTALL_URL" 2>/dev/null || true
        elif command -v wget >/dev/null 2>&1; then
            wget --no-check-certificate -q -O "$TMP_INSTALLER" "$REPO_INSTALL_URL" 2>/dev/null || true
        fi

        if [ -s "$TMP_INSTALLER" ]; then
            INSTALLER="$TMP_INSTALLER"
        fi
    fi

    if [ -z "$INSTALLER" ] || [ ! -s "$INSTALLER" ]; then
        log "ERROR: Could not download or locate install.sh. Check network connection."
        rm -f "$TMP_INSTALLER" 2>/dev/null || true
        exit 1
    fi

    log "Executing installer: $INSTALLER"
    chmod +x "$INSTALLER"
    
    # Run installer; capture exit code without tripping set -e
    INSTALL_RC=0
    env FORCE_DOWNLOAD=1 sh "$INSTALLER" --download >> "$LOG" 2>&1 || INSTALL_RC=$?
    rm -f "$TMP_INSTALLER" 2>/dev/null || true

    # Step 6/6 of install.sh restarts the UI and terminates old Qt sessions,
    # which can return code 137 / Killed. Check if the files are actually on disk:
    if is_installed; then
        log "FreeToon binaries and launcher verified on disk. Installation completed successfully!"
    elif [ "$INSTALL_RC" -eq 0 ]; then
        log "Installation finished successfully."
    else
        log "ERROR: Installer exited with code $INSTALL_RC and FreeToon binaries are missing. See details in $LOG."
        exit 1
    fi

    # Ensure selection remains freetoon
    echo "freetoon" > "$CHOICE_FILE"

else
    log "FreeToon is already installed. Switching active UI to FreeToon..."

    # Set UI preference to freetoon
    echo "freetoon" > "$CHOICE_FILE"
fi

# 3. Ensure inittab is properly wired and start FreeToon
TOON_ROW="toon:2345:respawn:/mnt/data/ui_launcher.sh >> /var/volatile/tmp/toonui.log 2>&1"

if [ -x /mnt/data/inittab_edit.sh ]; then
    log "Configuring inittab safely via inittab_edit.sh..."
    /mnt/data/inittab_edit.sh comment_pattern 'startqt|qt-gui'
    /mnt/data/inittab_edit.sh upsert toon "$TOON_ROW"
else
    # Fallback to guarded rewrite if inittab_edit.sh is missing
    cp -p "$INITTAB" "$INITTAB.bak" 2>/dev/null || true
    awk '
        $0 ~ /(startqt|qt-gui)/ && $0 !~ /^#/ { print "#FREETOON#" $0; next }
        { print $0 }
    ' "$INITTAB" | grep -v '^toon:' > "$INITTAB.new" 2>/dev/null || true
    echo "$TOON_ROW" >> "$INITTAB.new"
    if [ -s "$INITTAB.new" ] && [ "$(wc -l < "$INITTAB.new" 2>/dev/null || echo 0)" -ge 15 ]; then
        sync
        mv -f "$INITTAB.new" "$INITTAB"
        sync
        kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
    else
        rm -f "$INITTAB.new" 2>/dev/null || true
        log "WARNING: inittab verification failed! Restoring backup..."
        cp -f "$INITTAB.bak" "$INITTAB" 2>/dev/null || true
    fi
fi

# Switch OT bridge mode if proxy is enabled in toonui.cfg
if [ -f /mnt/data/toonui.cfg ] && grep -q '^ot_bridge_mode=proxy' /mnt/data/toonui.cfg 2>/dev/null; then
    if [ -x /mnt/data/ot_mode_switch.sh ]; then
        log "Configuring OpenTherm proxy bridge..."
        /mnt/data/ot_mode_switch.sh proxy >> "$LOG" 2>&1 || true
    fi
fi

log "Signaling init to reload inittab..."
kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
sleep 1

# Check if stock Qt processes are still lingering and terminate them
if pidof qt-gui >/dev/null 2>&1 || pidof startqt >/dev/null 2>&1; then
    log "Stopping lingering stock qt-gui processes..."
    killall -9 qt-gui 2>/dev/null || true
    pkill -9 -x qt-gui 2>/dev/null || true
    pkill -9 -f /qmf/sbin/qt-gui 2>/dev/null || true
    pkill -9 -x startqt 2>/dev/null || true
    kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
fi

log "FreeToon launch initiated successfully."
exit 0
