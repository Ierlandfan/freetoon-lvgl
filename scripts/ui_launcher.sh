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

export PATH="/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin:$PATH"

CHOICE_FILE=/mnt/data/ui_choice
TOONUI=/mnt/data/toonui
QTGUI=/qmf/sbin/qt-gui
STARTQT=/usr/bin/startqt
LOG=/var/volatile/tmp/ui_launcher.log
# Dropped by toonui (ui_request_restart) right before an intentional _exit(0)
# — a layout-editor Save/preset/reset or Settings "Restart UI". Tells the
# crash-loop guard below this relaunch is deliberate, not a crash.
RESTART_MARK=/var/volatile/tmp/toonui_restart
TOONCFG=/mnt/data/toonui.cfg

# Clean up stale update bundles and legacy cookie jars from tmpfs
rm -rf /tmp/freetoon.* /var/volatile/tmp/freetoon.* 2>/dev/null || true
rm -f /var/volatile/tmp/.dz_cookies_* /tmp/.dz_cookies_* 2>/dev/null || true

log() { echo "$(date '+%F %T') $*" >> "$LOG"; }

# Read a key from toonui.cfg (key=value, one per line). Echoes the trimmed
# value, or nothing when the file/key is absent. Keeps the launcher in sync
# with whatever the UI saved without duplicating defaults.
cfg_get() {
    [ -r "$TOONCFG" ] || return 0
    sed -n "s/^$1=//p" "$TOONCFG" 2>/dev/null | tr -d '[:space:]' | head -n 1
}

# Toon 1 (i.MX27) vs Toon 2 (i.MX6 "nxt"): arch.conf carries "nxt" only on
# Toon 2 — same probe default_ui() uses. The hardware video pipeline (VPU +
# fb1 overlay) is Toon-1-only, so its prep is gated on this.
is_toon1() { ! grep -q nxt /etc/opkg/arch.conf 2>/dev/null; }

# True if the stock UI can be launched (via startqt or qt-gui directly).
have_qtgui() { [ -x "$STARTQT" ] || [ -x "$QTGUI" ]; }

# Optional qt-gui logging for development/debugging. The stock TSC inittab row
# (qtqt:245:respawn:/usr/bin/startqt >/var/log/qt 2>&1) captures qt-gui output,
# but freetoon's takeover strips that row, so the redirect is lost — and re-added
# manually it's wiped again on the next freetoon update. Make it persistent and
# update-safe via /mnt/data/qtlog (on the data partition, survives updates),
# toggled from Settings → "Switch to qt-gui" dialog. Its contents are the log
# path; empty = a sensible default. Disabled (no file) = unchanged behaviour.
QTLOG=""
if [ -f /mnt/data/qtlog ]; then
    QTLOG=$(cat /mnt/data/qtlog 2>/dev/null | head -n 1 | tr -d ' ')   # busybox head needs -n 1, NOT -1
    [ -z "$QTLOG" ] && QTLOG=/var/volatile/tmp/qt-gui.log
    # NEVER let a bad log path block the stock UI: if it isn't writable, drop the
    # redirect and launch qt-gui normally.
    if [ -n "$QTLOG" ] && ! ( : > "$QTLOG" ) 2>/dev/null; then
        QTLOG=""
    fi
fi

# Launch the stock UI the way the device itself does. Toon 1 (and TSC-modified
# Toons) start qt-gui through /usr/bin/startqt, which sets the correct Qt
# platform (-platform linuxfb -plugin Tslib on Toon 1); exec'ing
# /qmf/sbin/qt-gui directly there gives a blank screen / no touch. So prefer
# startqt when present, else source qt-env.sh (Toon 2 eglfs) and exec qt-gui.
exec_qtgui() {
    # Ensure quby_bridge is disabled from inittab and unmounted when entering qt-gui
    # so stock qt-gui / happ_thermstat can talk to the real hardware keteladapter.
    if grep -q '^qbri:' /etc/inittab 2>/dev/null; then
        log "disabling quby_bridge in inittab for qt-gui"
        if [ -x /mnt/data/ot_mode_switch.sh ]; then
            /mnt/data/ot_mode_switch.sh off >> /tmp/ot_mode_switch.log 2>&1 || true
        elif [ -x /mnt/data/inittab_edit.sh ]; then
            /mnt/data/inittab_edit.sh drop qbri 2>/dev/null || true
            pkill -9 -x quby_bridge 2>/dev/null || true
            umount -l /dev/ttymxc0 2>/dev/null || true
        else
            cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true
            grep -v '^qbri:' /etc/inittab > /etc/inittab.new 2>/dev/null || true
            if [ -s /etc/inittab.new ] && [ "$(wc -l < /etc/inittab.new 2>/dev/null || echo 0)" -ge 15 ]; then
                sync; mv -f /etc/inittab.new /etc/inittab; sync
                kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
            else
                rm -f /etc/inittab.new 2>/dev/null || true
            fi
            pkill -9 -x quby_bridge 2>/dev/null || true
            umount -l /dev/ttymxc0 2>/dev/null || true
        fi
    else
        pkill -9 -x quby_bridge 2>/dev/null || true
        umount -l /dev/ttymxc0 2>/dev/null || true
    fi

    # Framebuffer depth on Toon 1: toonui forces fb0 (and the video prep fb1) to
    # 16bpp RGB565 and never restores it, but stock qt-gui runs at 32bpp on both
    # planes (measured on a stock Toon 1: fb0/fb1 bits_per_pixel=32). Starting
    # qt-gui on a 16bpp plane gives a garbled, blue-tinted UI. Restore 32bpp via
    # sysfs (fbset and `command -v` are missing on some Toon 1 firmwares).
    if is_toon1; then
        for _fb in /sys/class/graphics/fb0 /sys/class/graphics/fb1; do
            [ -w "$_fb/bits_per_pixel" ] || continue
            if [ "$(cat "$_fb/bits_per_pixel" 2>/dev/null)" != "32" ]; then
                log "restoring ${_fb##*/} to 32bpp for stock qt-gui"
                echo 32 > "$_fb/bits_per_pixel" 2>/dev/null || true
            fi
        done
    fi

    # On Toon 1, starting stock qt-gui takes up to 2-3 minutes.
    # At early boot, psplash starts before bootpick, so bootpick draws over it.
    # startqt only starts psplash if `pidof psplash` is empty, so if psplash was already
    # running, startqt skips it and leaves the screen frozen on bootpick's pixels.
    # Restart psplash fresh so the official Toon boot/progress splash shows immediately.
    if is_toon1 && [ -x /etc/init.d/psplash.sh ]; then
        /etc/init.d/psplash.sh quit 2>/dev/null || killall -9 psplash 2>/dev/null || true
        /etc/init.d/psplash.sh 2>/dev/null || true
        /etc/init.d/psplash.sh progress 30 2>/dev/null || true
    fi

    # Check for startqt in multiple standard locations
    ACTUAL_STARTQT=""
    for cand in "$STARTQT" /qmf/bin/startqt /qmf/sbin/startqt /usr/bin/inittabwrap; do
        if [ -x "$cand" ]; then
            ACTUAL_STARTQT="$cand"
            break
        fi
    done

    # Where stock-UI output goes. Without an explicit redirect qt-gui would
    # inherit the inittab row's `>> /var/volatile/tmp/toonui.log`, so "logging
    # off" (no /mnt/data/qtlog) must send it to /dev/null explicitly.
    QTOUT="${QTLOG:-/dev/null}"

    if [ -n "$ACTUAL_STARTQT" ]; then
        if [ "$ACTUAL_STARTQT" = "/usr/bin/inittabwrap" ]; then
            log "exec inittabwrap qt-gui (output to $QTOUT)"
            exec "$ACTUAL_STARTQT" qt-gui >"$QTOUT" 2>&1
        else
            log "exec $ACTUAL_STARTQT (output to $QTOUT)"
            exec "$ACTUAL_STARTQT" >"$QTOUT" 2>&1
        fi
    fi

    if is_toon1; then
        # Direct fallback on Toon 1 when startqt wrapper is missing:
        # Pass explicit platform and touch plugins so qt-gui does not crash or show blank screen
        log "exec $QTGUI -platform linuxfb -plugin Tslib (output to $QTOUT)"
        exec "$QTGUI" -platform linuxfb -plugin Tslib >"$QTOUT" 2>&1
    fi

    [ -r /etc/profile.d/qt-env.sh ] && . /etc/profile.d/qt-env.sh
    log "exec qt-gui (output to $QTOUT)"
    exec "$QTGUI" >"$QTOUT" 2>&1
}

# Toon 1 hardware-video prep, run once before the UI starts. No-op on Toon 2
# (i.MX6 has no VPU/overlay path) and never touches qt-gui's own setup.
#   - Force both framebuffers to 16bpp: the LVGL UI draws RGB565, and the
#     hardware overlay plane (fb1) must match so the decoder's stride lines up.
#   - Create the i.MX27 VPU char node (no udev). Major comes from
#     /proc/devices when the driver is loaded; 251 is the historical fallback.
#   - Open the inbound video UDP port iff one is configured (video_rtp>0 in
#     toonui.cfg). vpu_stream listens for the RTP/UDP stream there. Skipped
#     when video isn't set up, so nothing is exposed on a plain install.
toon1_video_prep() {
    is_toon1 || return 0
    # Reap a warm vpu_stream decoder orphaned by a previous toonui that
    # _exit(0)'d (UI restart) or crashed — toonui never runs video_shutdown on
    # those paths, so without this the next toonui spawns a second decoder and
    # the two fight over the single VPU. Runs before any UI starts, so the only
    # vpu_stream around here is a straggler.
    vpu_pids=$(pidof vpu_stream 2>/dev/null || true)
    if [ -n "${vpu_pids:-}" ]; then
        kill $vpu_pids 2>/dev/null || true
        log "reaped stale vpu_stream ($vpu_pids)"
    fi
    if command -v fbset >/dev/null 2>&1; then
        fbset -fb /dev/fb0 -g 800 480 800 480 16 2>/dev/null || fbset -fb /dev/fb0 -depth 16 2>/dev/null || true
        [ -e /dev/fb1 ] && { fbset -fb /dev/fb1 -g 800 480 800 480 16 2>/dev/null || fbset -fb /dev/fb1 -depth 16 2>/dev/null || true; }
    fi
    if [ ! -e /dev/mxc_vpu ]; then
        vpu_maj=$(awk '$2 == "mxc_vpu" { print $1 }' /proc/devices 2>/dev/null)
        mknod /dev/mxc_vpu c "${vpu_maj:-251}" 0 2>/dev/null || true
    fi
    vp=$(cfg_get video_rtp)
    if [ -n "${vp:-}" ] && [ "$vp" -gt 0 ] 2>/dev/null && [ -x /usr/sbin/iptables ]; then
        /usr/sbin/iptables -C INPUT -p udp --dport "$vp" -j ACCEPT 2>/dev/null \
            || /usr/sbin/iptables -I INPUT 1 -p udp --dport "$vp" -j ACCEPT 2>/dev/null \
            || true
        log "Toon 1 video prep: firewall open for UDP $vp"
    else
        log "Toon 1 video prep: fb/VPU ready (no video port configured)"
    fi
}

# Launch the freetoon UI. When the boot picker is disabled the UI comes up
# immediately — possibly before happ_thermstat is serving the room temperature,
# leaving it blank — so add a short settle delay. With the picker enabled its
# 10 s screen already covers the gap, so skip the extra wait then. (The Toon 1
# fb/VPU prep already ran earlier, before the picker.)
launch_toonui() {
    # If psplash was started during boot (rcS) on Toon 1, kill it so it doesn't linger in memory
    # or contend for framebuffer / console resources.
    if is_toon1 && [ -x /etc/init.d/psplash.sh ]; then
        /etc/init.d/psplash.sh quit 2>/dev/null || killall -9 psplash 2>/dev/null || true
    fi
    if [ "$(cfg_get boot_picker_enabled)" = "0" ]; then
        log "picker disabled — 5s backend settle before toonui"
        sleep 5
    fi
    log "exec toonui"
    exec "$TOONUI"
}

# --- log rotation watchdog -------------------------------------------------
# toonui logs verbosely to stderr (init redirects it to toonui.log); left
# unbounded it fills /var/volatile (tmpfs) and makes the whole device flaky.
# Cap each log at LOG_MAX bytes, keeping one .1 generation. We truncate the
# live log IN PLACE (`: >`), not mv/rm — init holds an O_APPEND fd on that
# inode, so a rename would orphan the fd and never free the space. Runs as a
# detached child that survives the `exec` below.
LOG_MAX=2097152          # 2 MB per file
LOGROT_PIDFILE=/var/volatile/tmp/ui_logrot.pid

# Kill any stale log-rotator from a previous launcher run, otherwise after
# a UI restart (toonui _exit → init respawns ui_launcher.sh) the old
# backgrounded subshell outlives the exec and a second one accumulates.
if [ -f "$LOGROT_PIDFILE" ]; then
    read old_pid < "$LOGROT_PIDFILE" 2>/dev/null || true
    [ -n "${old_pid:-}" ] && kill "$old_pid" 2>/dev/null || true
fi

TOONLOG=/var/volatile/tmp/toonui.log
VNCLOG=/var/volatile/tmp/x11vnc.log
(
    set +e   # a stray failure must never kill the watchdog
    # Background the sleep + wait on it, and TERM-trap to kill it, so the
    # stale-kill above (next launcher run) reaps the whole watchdog instead of
    # orphaning a lingering `sleep 120` for up to two minutes.
    sp=
    # Guard the empty case: `kill 0` would signal the whole process group.
    trap '[ -n "$sp" ] && kill "$sp" 2>/dev/null; exit 0' TERM INT
    while :; do
        for f in "$TOONLOG" "$LOG" "$VNCLOG"; do
            [ -f "$f" ] || continue
            sz=$(wc -c < "$f" 2>/dev/null || echo 0)
            if [ "${sz:-0}" -gt "$LOG_MAX" ]; then
                cp "$f" "$f.1" 2>/dev/null || true   # keep one previous gen
                : > "$f"                              # truncate in place
            fi
        done
        sleep 120 & sp=$!
        wait "$sp"
    done
) >/dev/null 2>&1 &
    echo $! > "$LOGROT_PIDFILE"

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

# Toon 1 framebuffer + VPU + video-port prep — runs before the picker so the
# picker (and the main UI) draw at the right depth. No-op on Toon 2 / qt-gui.
# `|| true` so a prep hiccup never aborts the launcher under `set -e`.
toon1_video_prep || true

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
            # WASM-host mode: the PANEL runs stock qt-gui; freetoon runs
            # headless (data + pwa_server on :10081) under the separate
            # tuih / toon_wasm_host.sh row. So on-screen == qt-gui here.
            wasm|wasm-host|masterslave) echo qt-gui; return ;;
            *)                  default_ui;      return ;;
        esac
    fi
    default_ui
}

CHOICE=$(read_choice)
log "boot: ui_choice=$CHOICE  toonui=$([ -x $TOONUI ] && echo yes || echo no)  qtgui=$(have_qtgui && echo yes || echo no)  startqt=$([ -x $STARTQT ] && echo yes || echo no)"

# WASM-host mode: the PANEL must always run stock qt-gui, while freetoon runs
# headless (data + pwa_server on :10081) under the separate tuih /
# toon_wasm_host.sh row. The boot picker (bootpick.c) only knows freetoon vs
# qt-gui and resolves "wasm" to freetoon, so it CANNOT be used here — we must
# bypass it and exec qt-gui directly. Without this, `toonui --bootpick` returns
# rc=0 and the dispatcher below would start the freetoon GUI on the panel,
# fighting the headless instance for the framebuffer + :10081.
RAW_CHOICE=$(cat "$CHOICE_FILE" 2>/dev/null | tr -d '[:space:]')
case "$RAW_CHOICE" in
    wasm|wasm-host|masterslave)
        log "wasm-host mode → stock qt-gui on panel (freetoon headless via tuih)"
        if have_qtgui; then
            exec_qtgui
        fi
        # No qt-gui to show: do NOT fall back to the freetoon GUI (it would
        # collide with the headless toonui on :10081). Idle so init retries.
        log "wasm-host: qt-gui not launchable — idling 60s so init retries"
        sleep 60
        exit 1
        ;;
esac

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
# An intentional restart (toonui dropped RESTART_MARK before _exit) is not a
# crash: consume the marker and clear the counter so editor Save / settings
# "Restart UI" round-trips never trip the qt-gui fallback.
if [ -f "$RESTART_MARK" ]; then
    rm -f "$RESTART_MARK"
    fails=0
    export TOONUI_WARM_RESTART=1
    log "intentional UI restart (marker) — crash counter reset"
fi
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
        launch_toonui
        ;;
    0)
        launch_toonui
        ;;
    *)
        # Any unexpected exit (bootpick crashed, segfault, etc.) — pick by
        # ui_choice so we never strand the user without a UI.
        log "unexpected rc — falling back to ui_choice=$CHOICE"
        if [ "$CHOICE" = "qt-gui" ] && have_qtgui; then
            exec_qtgui
        fi
        launch_toonui
        ;;
esac
