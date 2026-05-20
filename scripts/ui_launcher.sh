#!/bin/sh
# Toon UI launcher. Called by the `toon:345:respawn:` inittab row instead
# of running /mnt/data/toonui directly.
#
# Boot flow:
#   1. Run toonui in --bootpick mode (10 s picker screen with countdown).
#      It reads /mnt/data/ui_choice for the default selection.
#   2. Inspect the picker's exit code:
#        rc 0   → user picked / defaulted to freetoon → exec toonui normally
#        rc 99  → user picked / defaulted to qt-gui    → exec /qmf/sbin/qt-gui
#        rc *   → any other status (crash, missing binary, bootpick skipped
#                 because picker_enabled=0) → fall through to ui_choice
#
# This indirection keeps /etc/inittab stable across UI switches — flipping
# modes only writes /mnt/data/ui_choice, init never gets touched.
set -eu

CHOICE_FILE=/mnt/data/ui_choice
TOONUI=/mnt/data/toonui
QTGUI=/qmf/sbin/qt-gui
LOG=/var/volatile/tmp/ui_launcher.log

log() { echo "$(date '+%F %T') $*" >> "$LOG"; }

# Expose the PWA (pwa_server :10081) on the LAN. The stock Toon firewall's
# HCB-INPUT chain drops all inbound TCP except 22/80, so the phone UI is
# unreachable otherwise. Re-add the ACCEPT every boot (idempotent, inserted
# before the chain's trailing DROP) since a firmware update can rewrite
# /etc/default/iptables.conf. `|| true` so a firewall hiccup never aborts the
# launcher under `set -e` and leaves the device UI-less.
if [ -x /usr/sbin/iptables ]; then
    /usr/sbin/iptables -C HCB-INPUT -p tcp --dport 10081 -j ACCEPT 2>/dev/null \
        || /usr/sbin/iptables -I HCB-INPUT 1 -p tcp --dport 10081 -j ACCEPT 2>/dev/null \
        || true
    log "ensured PWA port 10081 open in firewall"
fi

# Read the persisted preference; default to freetoon when missing/garbled
# so a fresh basic-install boots into our UI without configuring anything.
read_choice() {
    if [ -r "$CHOICE_FILE" ]; then
        c=$(cat "$CHOICE_FILE" 2>/dev/null | tr -d '[:space:]')
        case "$c" in
            qt-gui|qtgui|stock) echo qt-gui; return ;;
            freetoon|toonui|*)  echo freetoon; return ;;
        esac
    fi
    echo freetoon
}

CHOICE=$(read_choice)
log "boot: ui_choice=$CHOICE  toonui=$([ -x $TOONUI ] && echo yes || echo no)  qtgui=$([ -x $QTGUI ] && echo yes || echo no)"

# If toonui isn't on disk, there's nothing to picker with — fall straight
# through to qt-gui (safest fallback so we never leave the device UI-less).
if [ ! -x "$TOONUI" ]; then
    log "toonui missing → exec qt-gui"
    [ -x "$QTGUI" ] && exec "$QTGUI"
    log "qt-gui ALSO missing — sleeping 60 then exiting so init can retry"
    sleep 60
    exit 1
fi

# Run the picker. It honours boot_picker_enabled in toonui.cfg internally
# (skipping the 10 s screen when disabled) and just returns the rc that
# matches the current ui_choice, so this dispatcher stays untouched.
#
# rc=99 means "user picked qt-gui" — a non-zero exit that `set -e` would
# otherwise treat as a script failure and abort us before the case below
# runs (resulting in the launcher being respawned and the picker firing
# forever). Wrap the call so the exit status is captured cleanly.
set +e
"$TOONUI" --bootpick
rc=$?
set -e
log "bootpick exited rc=$rc"

case "$rc" in
    99)
        if [ -x "$QTGUI" ]; then
            log "exec qt-gui"
            # qt-gui must run on the Toon's eglfs (framebuffer) Qt platform
            # plugin, not xcb. The init env doesn't set QT_QPA_PLATFORM so
            # the binary defaults to xcb and dies immediately ("Could not
            # find the Qt platform plugin xcb"). /etc/profile.d/qt-env.sh
            # exports the right values (QT_QPA_PLATFORM, evdev params,
            # physical screen mm dimensions). Source it before exec so qt-gui
            # gets the same environment the stock `flas` inittab row used.
            [ -r /etc/profile.d/qt-env.sh ] && . /etc/profile.d/qt-env.sh
            exec "$QTGUI"
        fi
        log "qt-gui binary missing — falling back to toonui"
        exec "$TOONUI"
        ;;
    0)
        log "exec toonui"
        exec "$TOONUI"
        ;;
    *)
        # Any unexpected exit (bootpick crashed, segfault, etc.) — pick by
        # ui_choice so we never strand the user without a UI.
        log "unexpected rc — falling back to ui_choice=$CHOICE"
        if [ "$CHOICE" = "qt-gui" ] && [ -x "$QTGUI" ]; then
            exec "$QTGUI"
        fi
        exec "$TOONUI"
        ;;
esac
