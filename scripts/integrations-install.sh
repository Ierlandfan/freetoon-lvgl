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

# Inittab row ids for this integration:
#   ROW_ID    current: 'i' + 3 hex chars of md5(id) - never collides with stock daemons
#   V105_ID   1.0.5:   'i' + first 3 alnum chars of id (e.g. itel, isto)
#   LEGACY_ID <=1.0.4: first 4 alnum chars of id (e.g. tele, stoc)
# Old-style rows are cleaned up on install and remove. A row is only ever dropped
# when its command points into THIS integration's own directory (OWN_DIR), so
# stock rows and other integrations sharing a short id are never touched.
HEX3=$(printf "%s" "$ID" | md5sum | tr -dc '0-9a-f' | cut -c1-3)
[ -n "$HEX3" ] || die "md5sum unavailable - cannot derive inittab row id"
ROW_ID="i${HEX3}"
V105_ID="i$(echo "$ID" | tr -dc 'a-zA-Z0-9' | cut -c1-3)"
LEGACY_ID=$(echo "$ID" | tr -dc 'a-zA-Z0-9' | cut -c1-4)
OWN_DIR="$INTEGRATIONS_DIR/$ID/"

# Kill every process whose command line contains $1 (pkill -f replacement:
# pkill does not exist on Toon 1). Skips this script's own process.
kill_cmdline() {
    for _d in /proc/[0-9]*; do
        _p=${_d#/proc/}
        [ "$_p" = "$$" ] && continue
        _c=$( { tr '\0' ' ' < "$_d/cmdline"; } 2>/dev/null ) || continue
        case "$_c" in *"$1"*) kill -9 "$_p" 2>/dev/null || true ;; esac
    done
}

# Remove every row (current, 1.0.5-style, legacy) that runs a binary from OWN_DIR.
drop_own_rows() {
    if [ -x /mnt/data/inittab_edit.sh ]; then
        for _rid in "$ROW_ID" "$V105_ID" "$LEGACY_ID"; do
            [ -n "$_rid" ] || continue
            grep -q "^${_rid}:" /etc/inittab 2>/dev/null || continue
            /mnt/data/inittab_edit.sh drop_safe "$_rid" "$OWN_DIR" || return 1
        done
    else
        cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true
        awk -v id1="$ROW_ID" -v id2="$V105_ID" -v id3="$LEGACY_ID" -v own="$OWN_DIR" '
            (index($0, id1 ":") == 1 || index($0, id2 ":") == 1 || (id3 != "" && index($0, id3 ":") == 1)) \
                && index($0, own) > 0 { next }
            { print $0 }
        ' /etc/inittab > /etc/inittab.new 2>/dev/null || true
        if [ -s /etc/inittab.new ] && [ "$(wc -l < /etc/inittab.new 2>/dev/null || echo 0)" -ge 15 ]; then
            sync; mv -f /etc/inittab.new /etc/inittab; sync
        else
            rm -f /etc/inittab.new 2>/dev/null || true
            log "ERROR: inittab verification failed! Original inittab preserved."
            return 1
        fi
    fi
}

# =====================================================================
# UNINSTALL ACTION
# =====================================================================
if [ "$ACTION" = "remove" ]; then
    log "uninstalling integration '$ID'..."
    drop_own_rows || die "could not remove inittab rows for '$ID' - integration files left in place"
    kill_cmdline "$OWN_DIR"
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
# Older packages were tarred from a Windows checkout as 0777 uid 1000, and
# root's tar keeps that. These binaries are respawned by init as root, so
# make them root-owned and not group/world-writable whatever the tarball says.
chown -R 0:0 "$INTEGRATIONS_DIR/$ID" 2>/dev/null || true
chmod -R go-w "$INTEGRATIONS_DIR/$ID" 2>/dev/null || true

BIN=$(sed -n 's/.*"binary":[ ]*"\([^"]*\)".*/\1/p' "$INTEGRATIONS_DIR/$ID/manifest.json")
[ -n "$BIN" ] || die "manifest missing binary field"
[ -x "$INTEGRATIONS_DIR/$ID/$BIN" ] || die "binary '$BIN' missing or not executable"

LOG_PATH="/var/volatile/tmp/integration-$ID.log"
ROW="$ROW_ID:2345:respawn:$INTEGRATIONS_DIR/$ID/$BIN >> $LOG_PATH 2>&1"

log "wiring inittab row '$ROW_ID' -> $BIN"
# Drop this integration's own rows (current + 1.0.5-style + legacy ids) first.
drop_own_rows || die "could not clean up existing inittab rows for '$ID'"
# Any row still using ROW_ID belongs to another integration (md5 prefix collision):
# refuse rather than overwrite it.
if grep -q "^${ROW_ID}:" /etc/inittab 2>/dev/null; then
    die "inittab row id '$ROW_ID' already used by another entry - not overwriting it"
fi
if [ -x /mnt/data/inittab_edit.sh ]; then
    /mnt/data/inittab_edit.sh upsert "$ROW_ID" "$ROW"
else
    cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true
    cp /etc/inittab /etc/inittab.new 2>/dev/null || true
    echo "$ROW" >> /etc/inittab.new
    if [ -s /etc/inittab.new ] && [ "$(wc -l < /etc/inittab.new 2>/dev/null || echo 0)" -ge 15 ]; then
        sync; mv -f /etc/inittab.new /etc/inittab; sync
    else
        rm -f /etc/inittab.new 2>/dev/null || true
        log "ERROR: inittab verification failed! Original inittab preserved."
    fi
fi

# Stop the running instance so init respawns the freshly installed binary.
kill_cmdline "$INTEGRATIONS_DIR/$ID/$BIN"
kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
log "installed - integration logs at $LOG_PATH"
log "toonui will pick up the new BoxTalk service on its next subscribe cycle."
