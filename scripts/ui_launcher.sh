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
STARTQT=/usr/bin/startqt
LOG=/var/volatile/tmp/ui_launcher.log

log() { echo "$(date '+%F %T') $*" >> "$LOG"; }

# True if the stock UI can be launched (via startqt or qt-gui directly).
have_qtgui() { [ -x "$STARTQT" ] || [ -x "$QTGUI" ]; }

# Launch the stock UI the way the device itself does. Toon 1 (and TSC-modified
# Toons) start qt-gui through /usr/bin/startqt, which sets the correct Qt
# platform (-platform linuxfb -plugin Tslib on Toon 1); exec'ing
# /qmf/sbin/qt-gui directly there gives a blank screen / no touch. So prefer
# startqt when present, else source qt-env.sh (Toon 2 eglfs) and exec qt-gui.
exec_qtgui() {
    if [ -x "$STARTQT" ]; then
        log "exec startqt"
        exec "$STARTQT"
    fi
    [ -r /etc/profile.d/qt-env.sh ] && . /etc/profile.d/qt-env.sh
    log "exec qt-gui"
    exec "$QTGUI"
}

# --- log rotation watchdog -------------------------------------------------
# toonui logs verbosely to stderr (init redirects it to toonui.log); left
# unbounded it fills /var/volatile (tmpfs) and makes the whole device flaky.
# Cap each log at LOG_MAX bytes, keeping one .1 generation. We truncate the
# live log IN PLACE (`: >`), not mv/rm — init holds an O_APPEND fd on that
# inode, so a rename would orphan the fd and never free the space. Runs as a
# detached child that survives the `exec` below.
LOG_MAX=2097152          # 2 MB per file
TOONLOG=/var/volatile/tmp/toonui.log
VNCLOG=/var/volatile/tmp/x11vnc.log
(
    set +e   # a stray failure must never kill the watchdog
    while :; do
        for f in "$TOONLOG" "$LOG" "$VNCLOG"; do
            [ -f "$f" ] || continue
            sz=$(wc -c < "$f" 2>/dev/null || echo 0)
            if [ "${sz:-0}" -gt "$LOG_MAX" ]; then
                cp "$f" "$f.1" 2>/dev/null || true   # keep one previous gen
                : > "$f"                              # truncate in place
            fi
        done
        sleep 120
    done
) >/dev/null 2>&1 &

# Expose the PWA (pwa_server :10081) and VNC (x11vnc :5900) on the LAN. The
# stock Toon firewall's HCB-INPUT chain drops all inbound TCP except 22/80, so
# these are unreachable otherwise. Re-add the ACCEPTs every boot (idempotent,
# inserted before the chain's trailing DROP) since a firmware update can
# rewrite /etc/default/iptables.conf. `|| true` so a firewall hiccup never
# aborts the launcher under `set -e` and leaves the device UI-less.
if [ -x /usr/sbin/iptables ]; then
    for p in 10081 5900; do
        /usr/sbin/iptables -C HCB-INPUT -p tcp --dport "$p" -j ACCEPT 2>/dev/null \
            || /usr/sbin/iptables -I HCB-INPUT 1 -p tcp --dport "$p" -j ACCEPT 2>/dev/null \
            || true
    done
    log "ensured firewall open for PWA 10081 + VNC 5900"
fi

# Default UI when no explicit choice exists: freetoon on Toon 2 (i.MX6 "nxt"),
# but qt-gui on Toon 1 — its 800x480 freetoon layout isn't finished, so the
# stock UI is the safe out-of-box default there.
default_ui() {
    if grep -q nxt /etc/opkg/arch.conf 2>/dev/null; then echo freetoon; else echo qt-gui; fi
}

# Read the persisted preference; fall back to the arch default when
# missing/garbled so a fresh basic-install boots into a working UI.
read_choice() {
    if [ -r "$CHOICE_FILE" ]; then
        c=$(cat "$CHOICE_FILE" 2>/dev/null | tr -d '[:space:]')
        case "$c" in
            qt-gui|qtgui|stock) echo qt-gui;     return ;;
            freetoon|toonui)    echo freetoon;   return ;;
            *)                  default_ui;      return ;;
        esac
    fi
    default_ui
}

CHOICE=$(read_choice)
log "boot: ui_choice=$CHOICE  toonui=$([ -x $TOONUI ] && echo yes || echo no)  qtgui=$(have_qtgui && echo yes || echo no)  startqt=$([ -x $STARTQT ] && echo yes || echo no)"

# If toonui isn't on disk, there's nothing to picker with — fall straight
# through to qt-gui (safest fallback so we never leave the device UI-less).
if [ ! -x "$TOONUI" ]; then
    log "toonui missing → stock UI"
    have_qtgui && exec_qtgui
    log "qt-gui ALSO missing — sleeping 60 then exiting so init can retry"
    sleep 60
    exit 1
fi

# --- crash-loop guard: never strand a user in a broken UI ------------------
# If toonui keeps exiting almost immediately (e.g. a first-time Toon 1 where
# the binary/layout isn't happy and the picker never stays up to be tapped),
# fall back to qt-gui so the device is always usable. A run that lasts longer
# than FAIL_WINDOW resets the counter, so this only trips on a genuine fast
# crash loop — not on a normal long session that later exits. State is two
# fields "epoch count" so we don't depend on busybox `stat -c`.
FAILF=/var/volatile/tmp/toonui_fails
FAIL_WINDOW=120     # a run shorter than this counts as a crash
FAIL_LIMIT=3        # this many fast crashes in a row → force qt-gui
now=$(date +%s)
last=0; fails=0
[ -f "$FAILF" ] && read last fails < "$FAILF" 2>/dev/null || true
[ -n "$last" ]  || last=0
[ -n "$fails" ] || fails=0
[ $((now - last)) -ge "$FAIL_WINDOW" ] && fails=0    # healthy gap → reset
if [ "$fails" -ge "$FAIL_LIMIT" ] && have_qtgui && [ "$CHOICE" != "qt-gui" ]; then
    log "toonui crash-looped ($fails fast exits) → forcing qt-gui fallback"
    : > "$FAILF"     # reset so a later manual switch back to freetoon works
    exec_qtgui
fi
echo "$now $((fails + 1))" > "$FAILF"   # record this attempt

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
        # exec_qtgui prefers /usr/bin/startqt (which sets the right Qt platform
        # — -platform linuxfb -plugin Tslib on Toon 1, eglfs on Toon 2); only
        # when there's no startqt does it source qt-env.sh + exec qt-gui direct.
        if have_qtgui; then
            exec_qtgui
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
        if [ "$CHOICE" = "qt-gui" ] && have_qtgui; then
            exec_qtgui
        fi
        exec "$TOONUI"
        ;;
esac
