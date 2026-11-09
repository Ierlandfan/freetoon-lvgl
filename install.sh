#!/bin/sh
# freetoon-lvgl installer & updater.
#
# Replaces the stock Eneco qt-gui with the LVGL UI, sets up the boot picker
# so the user can always switch back, and turns on the VNC respawn so the
# Toon's screen is reachable remotely. Idempotent — re-run to upgrade.
#
# Usage:
#   1) Directly ON the Toon device:
#      sh ./install.sh                           # auto-detects model, uses local or downloads binaries
#      sh ./install.sh --download                # force download latest release from GitHub
#      sh ./install.sh --uninstall               # revert back to stock qt-gui
#
#   2) Remotely from a PC (via SSH):
#      TOON_HOST=192.168.x.x ./install.sh
#      ./install.sh --uninstall
#
# All optional integrations (HomeWizard P1 / WTR, Itho ventilation, Home
# Assistant) are OFF by default; flip them on later from Settings →
# Integrations on the Toon.

set -eu

TOON_HOST="${TOON_HOST:-toon}"
TOON_PASS="${TOON_PASS:-toon}"
TOON_USER="${TOON_USER:-root}"
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -d /mnt/data ] && [ -w /mnt/data ]; then
    DL_DIR="/mnt/data/.freetoon_dl.$$"
else
    DL_DIR="/tmp/freetoon.$$"
fi
FORCE_DOWNLOAD="${FORCE_DOWNLOAD:-0}"

cleanup() {
    rm -rf "$DL_DIR" /mnt/data/.freetoon_dl.* /tmp/freetoon.* /var/volatile/tmp/freetoon.* 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Detect if running ON the Toon directly (BusyBox, root, /etc/inittab, /mnt/data)
IS_LOCAL=0
if [ -f /etc/inittab ] && [ -d /mnt/data ] && { [ -f /etc/opkg/arch.conf ] || [ -d /qmf ]; }; then
    IS_LOCAL=1
fi

# Detect Toon model (Toon 1 vs Toon 2)
IS_TOON1=0
if [ -f /etc/opkg/arch.conf ] && ! grep -q nxt /etc/opkg/arch.conf 2>/dev/null; then
    IS_TOON1=1
fi

say() { echo "[freetoon] $*"; }

# Resolve each artefact — checks override var, then candidate paths
pick() {
    _val="$1"; shift
    if [ -n "$_val" ] && [ -e "$_val" ]; then echo "$_val"; return 0; fi
    for _c in "$@"; do
        if [ -n "$_c" ] && [ -e "$_c" ]; then echo "$_c"; return 0; fi
    done
    echo ""
}

# Helper to find release binary candidate (checking release_* folders, HERE, build folders, /mnt/data)
find_cand() {
    _bin="$1"; shift
    for _d in "$@"; do
        if [ -n "$_d" ] && [ -f "$_d" ]; then echo "$_d"; return 0; fi
    done
    for _rel in "$HERE"/release_*; do
        if [ -d "$_rel" ] && [ -f "$_rel/$_bin" ]; then echo "$_rel/$_bin"; return 0; fi
    done
    echo ""
}

resolve_artefacts() {
    if [ "$IS_TOON1" = 1 ]; then
        TOONUI_BIN="$(pick "${TOONUI_BIN:-}" "$DL_DIR/toonui" "$(find_cand toonui-toon1 "$HERE/toonui-toon1" "$HERE/lvgl_ui_recovered/build-toon1/toonui-toon1" "$HERE/lvgl_ui_recovered/build1/toonui")" "$HERE/toonui-toon1")"
        WASTEFETCH_BIN="$(pick "${WASTEFETCH_BIN:-}" "$DL_DIR/wastefetch" "$(find_cand wastefetch-toon1 "$HERE/wastefetch-toon1" "$HERE/lvgl_ui_recovered/build-toon1/wastefetch" "$HERE/lvgl_ui_recovered/build1/wastefetch")" "$HERE/wastefetch-toon1")"
        QUBY_BRIDGE_BIN="$(pick "${QUBY_BRIDGE_BIN:-}" "$DL_DIR/quby_bridge" "$(find_cand quby_bridge-toon1 "$HERE/quby_bridge-toon1")" "$HERE/quby_bridge" "$HERE/quby_bridge/quby_bridge")"
        P1_BRIDGE_BIN="$(pick "${P1_BRIDGE_BIN:-}" "$DL_DIR/p1bridge" "$(find_cand p1bridge-toon1 "$HERE/p1bridge-toon1")" "$HERE/p1bridge" "$HERE/p1bridge/p1bridge")"
    else
        TOONUI_BIN="$(pick "${TOONUI_BIN:-}" "$DL_DIR/toonui" "$(find_cand toonui "$HERE/toonui" "$HERE/lvgl_ui_recovered/build/toonui")" "$HERE/toonui")"
        WASTEFETCH_BIN="$(pick "${WASTEFETCH_BIN:-}" "$DL_DIR/wastefetch" "$(find_cand wastefetch "$HERE/wastefetch" "$HERE/lvgl_ui_recovered/build/wastefetch")" "$HERE/wastefetch")"
        QUBY_BRIDGE_BIN="$(pick "${QUBY_BRIDGE_BIN:-}" "$DL_DIR/quby_bridge" "$(find_cand quby_bridge "$HERE/quby_bridge" "$HERE/quby_bridge/quby_bridge")" "$HERE/quby_bridge")"
        P1_BRIDGE_BIN="$(pick "${P1_BRIDGE_BIN:-}" "$DL_DIR/p1bridge" "$(find_cand p1bridge "$HERE/p1bridge" "$HERE/p1bridge/p1bridge")" "$HERE/p1bridge")"
    fi
    FBVNC_INPUT="$(pick "${FBVNC_INPUT:-}" "$DL_DIR/fbvnc_input" "$(find_cand fbvnc_input "$HERE/fbvnc_input" "$HERE/../qt_rebuild/fbvnc_input")")"
    INITTAB_EDIT="$(pick "${INITTAB_EDIT:-}"       "$DL_DIR/inittab_edit.sh"       "$HERE/inittab_edit.sh"       "$HERE/scripts/inittab_edit.sh")"
    UI_LAUNCHER="$(pick "${UI_LAUNCHER:-}"         "$DL_DIR/ui_launcher.sh"         "$HERE/ui_launcher.sh"         "$HERE/scripts/ui_launcher.sh")"
    COMPANION_GATE="$(pick "${COMPANION_GATE:-}"   "$DL_DIR/companion_gate.sh"     "$HERE/companion_gate.sh"     "$HERE/scripts/companion_gate.sh")"
    INTEG_INSTALLER="$(pick "${INTEG_INSTALLER:-}" "$DL_DIR/integrations-install.sh" "$HERE/integrations-install.sh" "$HERE/scripts/integrations-install.sh")"
    TOONVNC_SH="$(pick "${TOONVNC_SH:-}"           "$DL_DIR/toonvnc.sh"             "$HERE/toonvnc.sh"             "$HERE/scripts/toonvnc.sh")"
    OT_MODE_SH="$(pick "${OT_MODE_SH:-}"           "$DL_DIR/ot_mode_switch.sh"      "$HERE/ot_mode_switch.sh"      "$HERE/scripts/ot_mode_switch.sh")"

    PWA_DIR="${PWA_DIR:-}"
    if [ -z "$PWA_DIR" ]; then
        for d in "$DL_DIR/pwa/ui" "$HERE/pwa/ui" "$HERE/web/build" "$HERE/build"; do
            if [ -f "$d/index.wasm" ]; then PWA_DIR="$d"; break; fi
        done
        : "${PWA_DIR:=$HERE/web/build}"
    fi
}

# Download matching release binaries & assets from GitHub (ToonzDev repository)
download_artefacts() {
    REPO="${REPO:-ToonzDev/freetoon-lvgl}"
    mkdir -p "$DL_DIR"
    echo "@@STEP 1/6: Nieuwste versie opzoeken"
    say "Resolving latest release from GitHub ($REPO)..."
    curl -fsSL -4 --connect-timeout 8 --max-time 15 \
            "https://api.github.com/repos/$REPO/releases?per_page=1" > "$DL_DIR/release.json" 2>/dev/null || true
    TAG=$(grep -m1 '"tag_name"' "$DL_DIR/release.json" 2>/dev/null | sed 's/.*"tag_name"[^"]*"\([^"]*\)".*/\1/')

    if [ -z "$TAG" ]; then
        echo "@@FAIL Kon nieuwste versie niet opvragen van GitHub"
        say "Notice: no release tag found on https://github.com/$REPO/releases (using local/existing binaries)."
        return 1
    fi
    say "Latest release is $TAG (from $REPO)"

    has_asset() {
        grep -q "\"name\":[[:space:]]*\"$1\"" "$DL_DIR/release.json" 2>/dev/null
    }

    echo "@@STEP 2/6: Binary downloaden"
    BASE="https://github.com/$REPO/releases/download/$TAG"

    # Main UI binary
    if [ "$IS_TOON1" = 1 ]; then
        BIN_ASSET="toonui-toon1"
        say "Detected Toon 1 (ARMv5) — fetching $BIN_ASSET..."
    else
        BIN_ASSET="toonui"
        say "Detected Toon 2 (ARMv7) — fetching $BIN_ASSET..."
    fi

    if ! has_asset "$BIN_ASSET"; then
        echo "@@FAIL Release $TAG bevat geen $BIN_ASSET"
        say "FATAL: asset $BIN_ASSET not present in release $TAG"
        return 1
    fi

    if curl -fSL -4 --connect-timeout 8 --max-time 120 -o "$DL_DIR/toonui" "$BASE/$BIN_ASSET" 2>/dev/null; then
        chmod +x "$DL_DIR/toonui"
        TOONUI_BIN="$DL_DIR/toonui"
    else
        echo "@@FAIL Downloaden van $BIN_ASSET mislukt"
        say "FATAL: could not download $BIN_ASSET from release $TAG on $REPO"
        return 1
    fi

    echo "@@STEP 3/6: Download controleren"
    _sz=$(wc -c < "$DL_DIR/toonui" 2>/dev/null || echo 0)
    if [ "$_sz" -lt 500000 ]; then
        echo "@@FAIL Binary te klein of beschadigd (${_sz} B)"
        say "FATAL: downloaded $BIN_ASSET is corrupted or too small (${_sz} bytes)"
        return 1
    fi
    say "Binary verified ($BIN_ASSET, ${_sz} bytes)"

    echo "@@STEP 4/6: Helpers + assets"
    # Helper scripts directly from user repository
    RAW_BASE="https://raw.githubusercontent.com/$REPO/main"
    for s in ui_launcher.sh companion_gate.sh ot_mode_switch.sh toonvnc.sh integrations-install.sh; do
        if [ "$FORCE_DOWNLOAD" = 1 ] || { [ ! -f "$HERE/scripts/$s" ] && [ ! -f "$HERE/$s" ] && [ ! -f "/mnt/data/$s" ]; }; then
            say "Fetching $s from $REPO..."
            curl -fSL -4 --connect-timeout 8 --max-time 30 -o "$DL_DIR/$s" "$RAW_BASE/scripts/$s" 2>/dev/null \
                || curl -fSL -4 --connect-timeout 8 --max-time 30 -o "$DL_DIR/$s" "$RAW_BASE/$s" 2>/dev/null || true
            [ -s "$DL_DIR/$s" ] && chmod +x "$DL_DIR/$s"
        fi
    done

    # Optional companions - check if present in release first!
    WF_ASSET="wastefetch"
    if [ "$IS_TOON1" = 1 ]; then WF_ASSET="wastefetch-toon1"; fi
    if has_asset "$WF_ASSET"; then
        say "Fetching $WF_ASSET..."
        if curl -fSL -4 --connect-timeout 8 --max-time 60 -o "$DL_DIR/wastefetch" "$BASE/$WF_ASSET" 2>/dev/null; then
            [ -s "$DL_DIR/wastefetch" ] && chmod +x "$DL_DIR/wastefetch" && WASTEFETCH_BIN="$DL_DIR/wastefetch"
        fi
    fi

    QB_ASSET="quby_bridge"
    if [ "$IS_TOON1" = 1 ]; then QB_ASSET="quby_bridge-toon1"; fi
    if has_asset "$QB_ASSET"; then
        say "Fetching $QB_ASSET..."
        if curl -fSL -4 --connect-timeout 8 --max-time 60 -o "$DL_DIR/quby_bridge" "$BASE/$QB_ASSET" 2>/dev/null; then
            [ -s "$DL_DIR/quby_bridge" ] && chmod +x "$DL_DIR/quby_bridge" && QUBY_BRIDGE_BIN="$DL_DIR/quby_bridge"
        fi
    fi

    P1_ASSET="p1bridge"
    if [ "$IS_TOON1" = 1 ]; then P1_ASSET="p1bridge-toon1"; fi
    if has_asset "$P1_ASSET"; then
        say "Fetching $P1_ASSET..."
        if curl -fSL -4 --connect-timeout 8 --max-time 60 -o "$DL_DIR/p1bridge" "$BASE/$P1_ASSET" 2>/dev/null; then
            [ -s "$DL_DIR/p1bridge" ] && chmod +x "$DL_DIR/p1bridge" && P1_BRIDGE_BIN="$DL_DIR/p1bridge"
        fi
    fi

    if has_asset "fbvnc_input"; then
        say "Fetching fbvnc_input..."
        if curl -fSL -4 --connect-timeout 8 --max-time 60 -o "$DL_DIR/fbvnc_input" "$BASE/fbvnc_input" 2>/dev/null; then
            [ -s "$DL_DIR/fbvnc_input" ] && chmod +x "$DL_DIR/fbvnc_input" && FBVNC_INPUT="$DL_DIR/fbvnc_input"
        fi
    fi

    # WASM bundle for phone/browser UI
    if has_asset "index.wasm"; then
        say "Fetching WASM bundle..."
        mkdir -p "$DL_DIR/pwa/ui"
        for f in index.html index.wasm index.js; do
            curl -fSL -4 --connect-timeout 8 --max-time 60 -o "$DL_DIR/pwa/ui/$f" "$BASE/$f" 2>/dev/null || true
        done
        [ -s "$DL_DIR/pwa/ui/index.wasm" ] && PWA_DIR="$DL_DIR/pwa/ui"
    fi

    # Bundled x11vnc if needed
    if [ ! -x /usr/bin/x11vnc ] && ! which x11vnc >/dev/null 2>&1 && [ ! -x /mnt/data/x11vnc-bundle/bin/x11vnc ]; then
        if has_asset "x11vnc-bundle.tgz"; then
            say "Fetching x11vnc bundle..."
            if curl -fSL -4 --connect-timeout 8 --max-time 120 -o "$DL_DIR/x11vnc-bundle.tgz" "$BASE/x11vnc-bundle.tgz" 2>/dev/null; then
                mkdir -p /mnt/data/x11vnc-bundle
                tar xzf "$DL_DIR/x11vnc-bundle.tgz" -C /mnt/data/x11vnc-bundle 2>/dev/null || true
                chmod +x /mnt/data/x11vnc-bundle/bin/x11vnc 2>/dev/null || true
            fi
        fi
    fi

    return 0
}

if [ "$IS_LOCAL" = 1 ]; then
    remote() { sh -c "$*"; }
    push() {
        _src="$1"
        _dest="$2"
        [ -z "$_src" ] && return 0
        if [ "$_src" = "$_dest" ]; then
            chmod +x "$_dest" 2>/dev/null || true
            return 0
        fi
        if [ ! -e "$_src" ]; then
            if [ -e "$_dest" ]; then
                say "Notice: $_src not found, keeping existing $_dest"
                chmod +x "$_dest" 2>/dev/null || true
                return 0
            fi
            say "Warning: $_src not found and $_dest does not exist"
            return 1
        fi
        echo "  → $_dest"
        cp -f "$_src" "${_dest}.new"
        mv -f "${_dest}.new" "$_dest"
        chmod +x "$_dest"
    }
else
    SSH="sshpass -p $TOON_PASS ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR $TOON_USER@$TOON_HOST"
    SCP="sshpass -p $TOON_PASS scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
    remote() { $SSH "$@"; }
    push() {
        _src="$1"
        _dest="$2"
        [ -z "$_src" ] && return 0
        if [ ! -e "$_src" ]; then
            say "Notice: $_src not found locally, skipping remote copy to $_dest"
            return 0
        fi
        echo "  → $_dest"
        $SCP "$_src" "$TOON_USER@$TOON_HOST:${_dest}.new"
        remote "mv -f ${_dest}.new ${_dest} && chmod +x ${_dest}"
    }
fi

TOON_ROW="toon:2345:respawn:/mnt/data/ui_launcher.sh >> /var/volatile/tmp/toonui.log 2>&1"
VNCS_ROW="vncs:2345:respawn:/mnt/data/toonvnc.sh respawn >> /var/volatile/tmp/x11vnc.log 2>&1"

require() {
    if [ "$IS_LOCAL" = 1 ]; then return 0; fi
    command -v "$1" >/dev/null 2>&1 || { echo "ERROR: '$1' not on PATH — apt-get install sshpass" >&2; exit 2; }
}

upsert_row() {
    _row="$1"
    _id="${1%%:*}"
    remote "if [ -x /mnt/data/inittab_edit.sh ]; then \
        /mnt/data/inittab_edit.sh upsert '${_id}' '${_row}'; \
    else \
        cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true; \
        grep -v '^${_id}:' /etc/inittab > /etc/inittab.new 2>/dev/null || true; \
        echo '${_row}' >> /etc/inittab.new; \
        if [ -s /etc/inittab.new ] && [ \$(wc -l < /etc/inittab.new 2>/dev/null || echo 0) -ge 15 ]; then \
            sync; mv -f /etc/inittab.new /etc/inittab; sync; \
        else rm -f /etc/inittab.new 2>/dev/null || true; fi; \
    fi"
}

drop_row() {
    remote "if [ -x /mnt/data/inittab_edit.sh ]; then \
        /mnt/data/inittab_edit.sh drop '${1}'; \
    else \
        cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true; \
        grep -v '^${1}:' /etc/inittab > /etc/inittab.new 2>/dev/null || true; \
        if [ -s /etc/inittab.new ] && [ \$(wc -l < /etc/inittab.new 2>/dev/null || echo 0) -ge 15 ]; then \
            sync; mv -f /etc/inittab.new /etc/inittab; sync; \
        else rm -f /etc/inittab.new 2>/dev/null || true; fi; \
    fi"
}

disable_stock_qt() {
    # Safely comment out any stock Qt inittab rows via inittab_edit.sh (atomic lock, validation, sync)
    remote "if [ -x /mnt/data/inittab_edit.sh ]; then \
        /mnt/data/inittab_edit.sh comment_pattern 'startqt|qt-gui'; \
    else \
        cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true; \
        awk '\$0 ~ /(startqt|qt-gui)/ && \$0 !~ /^#/ { print \"#FREETOON#\" \$0; next } { print \$0 }' /etc/inittab > /etc/inittab.new 2>/dev/null || true; \
        if [ -s /etc/inittab.new ] && [ \$(wc -l < /etc/inittab.new 2>/dev/null || echo 0) -ge 15 ]; then \
            sync; mv -f /etc/inittab.new /etc/inittab; sync; \
        else rm -f /etc/inittab.new 2>/dev/null || true; fi; \
    fi"
}

restore_stock_qt() {
    # Restore stock Qt inittab rows upon uninstalling FreeToon safely
    remote "if [ -x /mnt/data/inittab_edit.sh ]; then \
        /mnt/data/inittab_edit.sh restore_stock_qt; \
    else \
        cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true; \
        awk '/^#FREETOON#/ { sub(/^#FREETOON#/, \"\"); print \$0; next } { print \$0 }' /etc/inittab > /etc/inittab.new 2>/dev/null || true; \
        if [ -s /etc/inittab.new ] && [ \$(wc -l < /etc/inittab.new 2>/dev/null || echo 0) -ge 15 ]; then \
            sync; mv -f /etc/inittab.new /etc/inittab; sync; \
        else rm -f /etc/inittab.new 2>/dev/null || true; fi; \
    fi"
}

check_artefacts() {
    if [ "$FORCE_DOWNLOAD" = 1 ]; then
        TOONUI_BIN=""
        WASTEFETCH_BIN=""
        QUBY_BRIDGE_BIN=""
        P1_BRIDGE_BIN=""
        FBVNC_INPUT=""
    fi
    resolve_artefacts

    # If toonui binary is missing or forced download requested: fetch from GitHub
    if [ -z "$TOONUI_BIN" ] || [ ! -f "$TOONUI_BIN" ] || [ "$FORCE_DOWNLOAD" = 1 ]; then
        say "No pre-compiled toonui binary found locally (or --download was specified)."
        download_artefacts || true
        resolve_artefacts
    fi

    missing=0
    for pair in "toonui:$TOONUI_BIN"; do
        name="${pair%%:*}"
        path="${pair#*:}"
        if [ -z "$path" ] || [ ! -e "$path" ]; then
            echo "  missing $name: ${path:-not found}" >&2
            missing=1
        fi
    done

    # Check required helper scripts
    for pair in "ui_launcher.sh:$UI_LAUNCHER" "companion_gate.sh:$COMPANION_GATE" \
                "toonvnc.sh:$TOONVNC_SH" "ot_mode_switch.sh:$OT_MODE_SH"; do
        name="${pair%%:*}"
        path="${pair#*:}"
        if [ -z "$path" ] || [ ! -e "$path" ]; then
            if [ -f "/mnt/data/$name" ]; then
                say "Notice: $name using existing /mnt/data/$name"
            else
                echo "  missing $name: ${path:-not found}" >&2
                missing=1
            fi
        fi
    done

    if [ "$missing" -ne 0 ]; then
        say "Some required scripts are missing. Attempting download from $REPO..."
        download_artefacts || true
        resolve_artefacts
    fi

    if [ -z "$TOONUI_BIN" ] || [ ! -f "$TOONUI_BIN" ]; then
        echo "@@FAIL toonui binary ontbreekt"
        say "FATAL: toonui binary could not be resolved or downloaded."
        say "Please ensure a release exists on https://github.com/$REPO or place toonui at $DL_DIR/toonui."
        exit 3
    fi
    [ -n "$FBVNC_INPUT" ] && [ -x "$FBVNC_INPUT" ] || echo "  note: fbvnc_input absent — VNC will be view-only (not fatal)" >&2
}

do_install() {
    say "Verifying artefacts..."
    check_artefacts

    target_desc="$TOON_HOST"
    if [ "$IS_LOCAL" = 1 ]; then target_desc="local Toon"; fi
    echo "@@STEP 5/6: Binary plaatsen"
    say "Installing binaries + scripts to $target_desc..."
    [ -n "$INITTAB_EDIT" ] && [ -e "$INITTAB_EDIT" ] && push "$INITTAB_EDIT" /mnt/data/inittab_edit.sh
    push "$TOONUI_BIN"      /mnt/data/toonui
    [ -n "$UI_LAUNCHER" ] && [ -e "$UI_LAUNCHER" ] && push "$UI_LAUNCHER"     /mnt/data/ui_launcher.sh
    [ -n "$COMPANION_GATE" ] && [ -e "$COMPANION_GATE" ] && push "$COMPANION_GATE"  /mnt/data/companion_gate.sh
    [ -n "$INTEG_INSTALLER" ] && [ -e "$INTEG_INSTALLER" ] && push "$INTEG_INSTALLER" /mnt/data/integrations-install.sh
    [ -n "$TOONVNC_SH" ] && [ -e "$TOONVNC_SH" ] && push "$TOONVNC_SH"      /mnt/data/toonvnc.sh
    [ -n "$OT_MODE_SH" ] && [ -e "$OT_MODE_SH" ] && push "$OT_MODE_SH"      /mnt/data/ot_mode_switch.sh
    [ -n "$FBVNC_INPUT" ] && [ -e "$FBVNC_INPUT" ] && push "$FBVNC_INPUT" /mnt/data/fbvnc_input
    if [ -n "$WASTEFETCH_BIN" ] && [ -e "$WASTEFETCH_BIN" ]; then
        push "$WASTEFETCH_BIN" /mnt/data/wastefetch
    else
        say "Note: wastefetch absent — waste calendar provider plugin execution will be unavailable"
    fi
    if [ -n "$P1_BRIDGE_BIN" ] && [ -e "$P1_BRIDGE_BIN" ]; then
        push "$P1_BRIDGE_BIN" /mnt/data/p1bridge
    fi

    echo "[3/5] Setting up WASM slave UI bundle..."
    if [ -n "$PWA_DIR" ] && [ -d "$PWA_DIR" ] && [ -f "$PWA_DIR/index.wasm" ]; then
        remote "mkdir -p /mnt/data/pwa/ui"
        for f in index.html index.js index.wasm; do
            [ -f "$PWA_DIR/$f" ] || continue
            echo "  → /mnt/data/pwa/ui/$f"
            if [ "$IS_LOCAL" = 1 ]; then
                if [ "$PWA_DIR/$f" != "/mnt/data/pwa/ui/$f" ]; then
                    cp -f "$PWA_DIR/$f" "/mnt/data/pwa/ui/${f}.new"
                    mv -f "/mnt/data/pwa/ui/${f}.new" "/mnt/data/pwa/ui/$f"
                fi
            else
                $SCP "$PWA_DIR/$f" "$TOON_USER@$TOON_HOST:/mnt/data/pwa/ui/${f}.new"
                remote "mv -f /mnt/data/pwa/ui/${f}.new /mnt/data/pwa/ui/${f}"
            fi
        done
        remote '[ -s /mnt/data/pwa/ui/index.wasm ] && cd /mnt/data/pwa && rm -f \
            app.js sw.js manifest.json icon-192.png index.html index.js index.wasm \
            index.html.bak index.html.staticbak index.js.bak index.wasm.bak || true'
    else
        echo "  (skipping WASM bundle — not found or not built)"
    fi

    echo "[4/5] Seeding default config..."
    # Toon 1 default to qt-gui on first install; Toon 2 default to freetoon
    if [ "$IS_TOON1" = 1 ]; then
        remote "[ -f /mnt/data/ui_choice ] || echo qt-gui > /mnt/data/ui_choice"
    else
        remote "[ -f /mnt/data/ui_choice ] || echo freetoon > /mnt/data/ui_choice"
    fi
    # toonui.cfg — write a basic-mode default ONLY if user has no cfg yet.
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

    echo "[5/5] Wiring inittab + firewall + restart..."
    # Open PWA (10081) and VNC (5900) ports in stock Toon firewall
    if [ "$IS_LOCAL" = 1 ]; then
        FW=/etc/default/iptables.conf
        for p in 10081 5900; do
            if [ -f "$FW" ] && ! grep -q "dport $p " "$FW"; then
                sed -i "/--dport 80 --tcp-flags/a -A HCB-INPUT -p tcp -m tcp --dport $p --tcp-flags SYN,RST,ACK SYN -j ACCEPT" "$FW" 2>/dev/null || true
            fi
            if [ -x /usr/sbin/iptables ]; then
                /usr/sbin/iptables -C HCB-INPUT -p tcp --dport "$p" -j ACCEPT 2>/dev/null \
                    || /usr/sbin/iptables -I HCB-INPUT 1 -p tcp --dport "$p" -j ACCEPT 2>/dev/null || true
            fi
        done
    fi

    drop_row qbri
    drop_row p1br
    disable_stock_qt
    remote "pkill -x quby_bridge 2>/dev/null; pkill -x p1bridge 2>/dev/null; true"
    remote "umount -l /dev/ttymxc0 2>/dev/null; true"
    upsert_row "$TOON_ROW"
    if [ "$IS_LOCAL" = 1 ]; then
        if [ -f /mnt/data/toonui.cfg ] && grep -q '^vnc_enabled=1' /mnt/data/toonui.cfg 2>/dev/null; then
            upsert_row "$VNCS_ROW"
        else
            drop_row vncs
            pkill -9 -x x11vnc 2>/dev/null || true
        fi
    else
        if remote "[ -f /mnt/data/toonui.cfg ] && grep -q '^vnc_enabled=1' /mnt/data/toonui.cfg 2>/dev/null"; then
            upsert_row "$VNCS_ROW"
        else
            drop_row vncs
            remote "pkill -9 -x x11vnc 2>/dev/null || true"
        fi
    fi
    if [ -n "$QUBY_BRIDGE_BIN" ] && [ -e "$QUBY_BRIDGE_BIN" ]; then
        push "$QUBY_BRIDGE_BIN" /mnt/data/quby_bridge
        # Only enable OT bridge proxy if FreeToon is active; stock qt-gui needs direct UART access
        if remote "[ \"\$(cat /mnt/data/ui_choice 2>/dev/null)\" != 'qt-gui' ]"; then
            remote "/mnt/data/ot_mode_switch.sh proxy" || true
        else
            remote "/mnt/data/ot_mode_switch.sh off" || true
        fi
    else
        echo "  (skipping bridge install — quby_bridge absent; OT bridge stays off)"
    fi
    echo "@@STEP 6/6: UI herstarten"
    say "Restarting UI..."
    remote "killall -9 toonui 2>/dev/null || true; pkill -9 -x toonui 2>/dev/null || true; for _p in \$(pidof toonui 2>/dev/null); do kill -9 \"\$_p\" 2>/dev/null || true; done; pkill -9 -x x11vnc 2>/dev/null; pkill -9 -x qt-gui 2>/dev/null; pkill -9 -f /qmf/sbin/qt-gui 2>/dev/null; pkill -9 -x startqt 2>/dev/null; true"
    remote "kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true"

    rm -rf "$DL_DIR" /tmp/freetoon.* /var/volatile/tmp/freetoon.* 2>/dev/null || true

    echo
    echo "Installed successfully! Toon is now running FreeToon."
    echo "  • Boot picker: 10 s window at boot to switch to stock qt-gui"
    echo "  • Settings → UI mode: switch between FreeToon and qt-gui"
    echo "  • Settings → Integrations: configure P1 / Itho / HA"
}

do_uninstall() {
    echo "[1/4] Dropping inittab rows..."
    drop_row toon
    drop_row vncs
    drop_row p1br
    drop_row qbri
    echo "[2/4] Restoring stock qt inittab row..."
    restore_stock_qt
    echo "[3/4] Stopping daemons..."
    remote "killall -9 toonui 2>/dev/null || true; pkill -9 -x toonui 2>/dev/null || true; for _p in \$(pidof toonui 2>/dev/null); do kill -9 \"\$_p\" 2>/dev/null || true; done; pkill -9 -x x11vnc 2>/dev/null; pkill -9 -x quby_bridge 2>/dev/null; pkill -9 -x p1bridge 2>/dev/null; true"
    echo "[4/4] Removing files..."
    remote "rm -f /mnt/data/toonui /mnt/data/inittab_edit.sh /mnt/data/ui_launcher.sh /mnt/data/companion_gate.sh /mnt/data/toonvnc.sh /mnt/data/ot_mode_switch.sh /mnt/data/fbvnc_input /mnt/data/ui_choice /mnt/data/toonvnc.plain /mnt/data/toonui.cfg"
    remote "rm -rf /mnt/data/pwa"
    remote "kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true"
    echo "Uninstalled. Stock qt-gui takes over on next launcher cycle."
}

case "${1:-install}" in
    install|"")
        [ "$IS_LOCAL" = 0 ] && { require sshpass; require ssh; require scp; }
        do_install
        ;;
    --download|--update)
        FORCE_DOWNLOAD=1
        [ "$IS_LOCAL" = 0 ] && { require sshpass; require ssh; require scp; }
        do_install
        ;;
    --uninstall)
        [ "$IS_LOCAL" = 0 ] && { require sshpass; require ssh; require scp; }
        do_uninstall
        ;;
    -h|--help)
        sed -n '2,20p' "$0"
        ;;
    *)
        echo "Unknown command '$1' — see --help" >&2
        exit 1
        ;;
esac
