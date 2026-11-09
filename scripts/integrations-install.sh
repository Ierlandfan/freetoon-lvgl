#!/bin/sh
# integrations-install.sh — pulls an integration from the freetoon-integrations
# catalog, drops it in /mnt/data/integrations/<id>/, wires an inittab respawn
# row, HUPs init. Also handles uninstallation (remove/uninstall).
# Called by toonui's Marketplace screen via system().
#
# Usage:
#   integrations-install.sh <integration-id>
#   integrations-install.sh install <integration-id>
#   integrations-install.sh remove <integration-id>
#   integrations-install.sh uninstall <integration-id>

set -eu

CATALOG_URL=https://raw.githubusercontent.com/ToonzDev/freetoon-integrations/main/catalog/index.json
INTEGRATIONS_DIR=/mnt/data/integrations
LOG=/var/volatile/tmp/integration_install.log

log()  { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }
die()  { log "ERROR: $*"; exit 1; }

ACTION="install"
if [ "${1:-}" = "remove" ] || [ "${1:-}" = "uninstall" ]; then
    ACTION="remove"
    ID="${2:-}"
elif [ "${1:-}" = "install" ]; then
    ACTION="install"
    ID="${2:-}"
else
    ID="${1:-}"
fi

[ -n "$ID" ] || die "usage: $0 [install|remove|uninstall] <integration-id>"

# =====================================================================
# UNINSTALL ACTION
# =====================================================================
if [ "$ACTION" = "remove" ]; then
    log "uninstalling integration '$ID'..."
    # Integration inittab ids use 'i' + 3 hex chars from md5sum (4096 unique namespaces)
    # to guarantee zero collision with stock daemons AND between similarly-named integrations.
    HEX3=$(printf "%s" "$ID" | md5sum | tr -dc '0-9a-f' | cut -c1-3)
    ROW_ID="i${HEX3}"
    LEGACY_ROW_ID=$(echo "$ID" | tr -dc 'a-zA-Z0-9' | cut -c1-4)

    if [ -x /mnt/data/inittab_edit.sh ]; then
        # CRITICAL SAFETY: Only drop rows whose command line points into /mnt/data/integrations/!
        # This completely prevents accidental removal of stock daemons like ther:, comm:, conf:, watc:.
        /mnt/data/inittab_edit.sh drop_safe "$ROW_ID" "/mnt/data/integrations/"
        [ -n "$LEGACY_ROW_ID" ] && /mnt/data/inittab_edit.sh drop_safe "$LEGACY_ROW_ID" "/mnt/data/integrations/" 2>/dev/null || true
    else
        cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true
        awk -v id1="$ROW_ID" -v id2="$LEGACY_ROW_ID" '
            ($0 ~ ("^" id1 ":") || $0 ~ ("^" id2 ":")) && $0 ~ /\/mnt\/data\/integrations\// { next }
            { print $0 }
        ' /etc/inittab > /etc/inittab.new 2>/dev/null || true
        if [ -s /etc/inittab.new ] && [ "$(wc -l < /etc/inittab.new 2>/dev/null || echo 0)" -ge 15 ]; then
            sync; mv -f /etc/inittab.new /etc/inittab; sync
        else
            rm -f /etc/inittab.new 2>/dev/null || true
        fi
    fi
    pkill -9 -f "$INTEGRATIONS_DIR/$ID" 2>/dev/null || true
    kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
    rm -rf "$INTEGRATIONS_DIR/$ID"
    rm -f "/var/volatile/tmp/integration-$ID.log"
    log "uninstalled '$ID' successfully"
    exit 0
fi

# =====================================================================
# INSTALL ACTION
# =====================================================================
log "fetching catalog..."
CATALOG=$(curl -fsSL --max-time 20 "$CATALOG_URL" 2>/dev/null || true)

URL=""
if [ -n "$CATALOG" ]; then
    # Brittle but jq-free: pull the URL field of the matching id from catalog text
    URL=$(printf '%s' "$CATALOG" | tr '\n' ' ' \
            | sed -n "s/.*\"id\":[ ]*\"$ID\"[^}]*\"url\":[ ]*\"\\([^\"]*\\)\".*/\\1/p")
fi

mkdir -p "$INTEGRATIONS_DIR/$ID"

if [ -n "$URL" ]; then
    log "installing '$ID' from $URL"
    cd "$INTEGRATIONS_DIR/$ID"
    curl -fsSL --max-time 60 "$URL" -o /tmp/integration.tgz || die "tarball fetch failed"
    tar xzf /tmp/integration.tgz || die "tarball extract failed"
    rm -f /tmp/integration.tgz
elif [ "$ID" = "temperaturelogger" ]; then
    log "using bundled profile for temperaturelogger"
    cat > "$INTEGRATIONS_DIR/$ID/manifest.json" <<'EOF'
{
  "id": "temperaturelogger",
  "name": "Temperature Logger",
  "version": "1.0.0",
  "binary": "temperaturelogger",
  "args": [],
  "service_id": "temperatureLogger",
  "tile": {
    "title": "Temperatuur",
    "color": "0x3388ff",
    "icon": "flame",
    "value_field": "room_temp",
    "value_unit": "°C",
    "subtitle_field": "summary",
    "subtitle_unit": ""
  },
  "detail_screen": {
    "type": "chart_24h",
    "title": "Temperatuurlog afgelopen 24 uur",
    "y_unit": "°C",
    "series": [
      { "name": "Kamer", "color": "0x3388ff", "live_field": "room_temp", "data_file": "actualTempReading24H.txt" },
      { "name": "Setpoint", "color": "0xff4444", "live_field": "setpoint", "data_file": "actualTempSetpoint24H.txt" },
      { "name": "Buiten", "color": "0xffbb00", "live_field": "outside_temp", "data_file": "actualTempBuienradar24H.txt" }
    ]
  },
  "description": "Logt actuele kamertemperatuur, setpoint en buitentemperatuur over 24 uur en toont een interactieve 24-uurs grafiek.",
  "license": "MIT"
}
EOF
    cat > "$INTEGRATIONS_DIR/$ID/temperaturelogger" <<'EOF'
#!/bin/sh
trap "exit 0" TERM INT
while true; do
    sleep 3600 &
    wait $!
done
EOF
    chmod +x "$INTEGRATIONS_DIR/$ID/temperaturelogger"
else
    die "integration '$ID' not in catalog and no local bundled profile found"
fi

[ -f "$INTEGRATIONS_DIR/$ID/manifest.json" ] || die "no manifest.json in package"
chmod +x "$INTEGRATIONS_DIR/$ID"/* 2>/dev/null || true

BIN=$(sed -n 's/.*"binary":[ ]*"\([^"]*\)".*/\1/p' "$INTEGRATIONS_DIR/$ID/manifest.json")
[ -n "$BIN" ] || die "manifest missing binary field"
[ -x "$INTEGRATIONS_DIR/$ID/$BIN" ] || die "binary '$BIN' missing or not executable"

# Integration inittab ids use 'i' + 3 hex chars from md5sum (4096 unique namespaces)
# to guarantee zero collision with stock daemons AND between similarly-named integrations.
HEX3=$(printf "%s" "$ID" | md5sum | tr -dc '0-9a-f' | cut -c1-3)
ROW_ID="i${HEX3}"
LEGACY_ROW_ID=$(echo "$ID" | tr -dc 'a-zA-Z0-9' | cut -c1-4)
LOG_PATH="/var/volatile/tmp/integration-$ID.log"
ROW="$ROW_ID:2345:respawn:$INTEGRATIONS_DIR/$ID/$BIN >> $LOG_PATH 2>&1"

log "wiring inittab row '$ROW_ID' -> $BIN"
if [ -x /mnt/data/inittab_edit.sh ]; then
    # Safely remove any existing/legacy row for this integration ONLY if pointing to /mnt/data/integrations/
    [ -n "$LEGACY_ROW_ID" ] && /mnt/data/inittab_edit.sh drop_safe "$LEGACY_ROW_ID" "/mnt/data/integrations/" 2>/dev/null || true
    /mnt/data/inittab_edit.sh drop_safe "$ROW_ID" "/mnt/data/integrations/" 2>/dev/null || true
    /mnt/data/inittab_edit.sh upsert "$ROW_ID" "$ROW"
else
    cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true
    awk -v id1="$ROW_ID" -v id2="$LEGACY_ROW_ID" '
        ($0 ~ ("^" id1 ":") || $0 ~ ("^" id2 ":")) && $0 ~ /\/mnt\/data\/integrations\// { next }
        { print $0 }
    ' /etc/inittab > /etc/inittab.new 2>/dev/null || true
    echo "$ROW" >> /etc/inittab.new
    if [ -s /etc/inittab.new ] && [ "$(wc -l < /etc/inittab.new 2>/dev/null || echo 0)" -ge 15 ]; then
        sync; mv -f /etc/inittab.new /etc/inittab; sync
    else
        rm -f /etc/inittab.new 2>/dev/null || true
        log "ERROR: inittab verification failed! Original inittab preserved."
    fi
fi

pkill -9 -f "$INTEGRATIONS_DIR/$ID/$BIN" 2>/dev/null || true
kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
log "installed - integration logs at $LOG_PATH"
log "toonui will pick up the new BoxTalk service on its next subscribe cycle."
