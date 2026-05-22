#!/bin/sh
# freetoon — on-device installer / updater.
#
# Run THIS on the Toon itself (you have local/SSH access). It pulls the
# latest GitHub release, swaps in the new toonui binary + helper scripts,
# makes sure the inittab launch row is present, and restarts the UI.
#
# One-liner:
#   curl -fsSL https://raw.githubusercontent.com/Ierlandfan/freetoon-lvgl/main/scripts/toon-selfinstall.sh | sh
#
# Re-running is safe (idempotent): it only adds the inittab row if missing
# and always backs up the current binary to /mnt/data/toonui.bak first.
set -e

REPO="Ierlandfan/freetoon-lvgl"
DEST="/mnt/data"
TMP="/tmp/freetoon.$$"
mkdir -p "$TMP"

say() { echo "[freetoon] $*"; }

# Resolve the newest release tag INCLUDING prereleases — all freetoon releases
# are beta (prerelease), so /releases/latest would skip them. per_page=1 gives
# the single newest release; grep the first tag_name.
say "resolving latest release"
TAG=$(curl -fsSL --connect-timeout 8 --max-time 30 \
        "https://api.github.com/repos/$REPO/releases?per_page=1" 2>/dev/null \
      | grep -m1 '"tag_name"' | sed 's/.*"tag_name"[^"]*"\([^"]*\)".*/\1/')
if [ -z "$TAG" ]; then
    say "ERROR: could not resolve latest release tag (no internet?)."
    rm -rf "$TMP"; exit 1
fi
say "latest release is $TAG"
BASE="https://github.com/$REPO/releases/download/$TAG"

dl() {  # dl <asset> <out>
    say "fetching $1"
    curl -fSL --connect-timeout 8 --max-time 120 -o "$2" "$BASE/$1"
}

# 1) toonui binary (required) — sanity-check the size (a GitHub error page or
# truncated download is tiny; the real binary is ~1 MB). busybox-safe.
dl toonui "$TMP/toonui"
SZ=$(wc -c < "$TMP/toonui" 2>/dev/null || echo 0)
if [ "$SZ" -lt 500000 ]; then
    say "ERROR: toonui download too small ($SZ bytes) — aborting."
    rm -rf "$TMP"; exit 1
fi

# 2) helper scripts — always refresh. These are freetoon's own plumbing
# (launcher, VNC, OT-mode, companion gate); they ship bug fixes (e.g. the
# framebuffer-offset VNC fix, the firewall port-open), so a re-run of the
# installer must update them rather than keep a stale copy.
for s in ui_launcher.sh companion_gate.sh ot_mode_switch.sh toonvnc.sh; do
    if dl "$s" "$TMP/$s" && [ -s "$TMP/$s" ]; then
        cp "$TMP/$s" "$DEST/$s" && chmod +x "$DEST/$s"
    fi
done

# 2b) VNC input bridge — lets you control the Toon over VNC (injects remote
# pointer events into the multi-touch screen). Without it VNC is view-only,
# regardless of the x11vnc build. Always refresh (it's our compiled binary).
if dl fbvnc_input "$TMP/fbvnc_input"; then
    if [ "$(wc -c < "$TMP/fbvnc_input" 2>/dev/null || echo 0)" -gt 2000 ]; then
        cp "$TMP/fbvnc_input" "$DEST/fbvnc_input" && chmod +x "$DEST/fbvnc_input"
    fi
fi

# 2c) x11vnc — required for VNC. The Toon's opkg feed (feed.hae.int) is
# VPN-only, so `opkg install x11vnc` won't work on a home Toon; instead we
# ship a bundle (x11vnc + libssl/libcrypto/libjpeg) and run it firmware-safe
# from /mnt/data with LD_LIBRARY_PATH. Only installed when no system x11vnc.
# NB: the Toon's busybox sh has no `command` builtin — use which + path checks.
if [ ! -x /usr/bin/x11vnc ] && ! which x11vnc >/dev/null 2>&1 \
   && [ ! -x "$DEST/x11vnc-bundle/bin/x11vnc" ]; then
    if dl x11vnc-bundle.tgz "$TMP/x11vnc-bundle.tgz" \
       && [ "$(wc -c < "$TMP/x11vnc-bundle.tgz" 2>/dev/null || echo 0)" -gt 100000 ]; then
        mkdir -p "$DEST/x11vnc-bundle"
        if tar xzf "$TMP/x11vnc-bundle.tgz" -C "$DEST/x11vnc-bundle" 2>/dev/null; then
            chmod +x "$DEST/x11vnc-bundle/bin/x11vnc" 2>/dev/null || true
            say "installed bundled x11vnc -> $DEST/x11vnc-bundle"
        fi
    fi
fi

# 2d) PWA static assets — the phone web UI + settings page that pwa_server
# serves on :10081. The API/settings endpoints work without these, but the
# installable app at "/" 404s until index.html/app.js/etc. exist under
# /mnt/data/pwa. Shipped as a tarball; always refresh so app updates land.
if dl pwa.tgz "$TMP/pwa.tgz" \
   && [ "$(wc -c < "$TMP/pwa.tgz" 2>/dev/null || echo 0)" -gt 1000 ]; then
    mkdir -p "$DEST/pwa"
    if tar xzf "$TMP/pwa.tgz" -C "$DEST/pwa" 2>/dev/null; then
        say "installed PWA assets -> $DEST/pwa"
    fi
fi

# 2e) Open the PWA (10081) + VNC (5900) ports in the stock Toon firewall. The
# HCB-INPUT chain in /etc/default/iptables.conf accepts only 22/80 and drops
# the rest, so pwa_server's phone UI and x11vnc are unreachable on the LAN. Add
# persistent ACCEPTs (survive reboot) and apply them live now. ui_launcher.sh
# also re-adds them each boot in case a firmware update rewrites the conf.
FW=/etc/default/iptables.conf
for p in 10081 5900; do
    if [ -f "$FW" ] && ! grep -q "dport $p " "$FW"; then
        if sed -i "/--dport 80 --tcp-flags/a -A HCB-INPUT -p tcp -m tcp --dport $p --tcp-flags SYN,RST,ACK SYN -j ACCEPT" "$FW" 2>/dev/null; then
            say "opened port $p in $FW"
        fi
    fi
    if [ -x /usr/sbin/iptables ]; then
        /usr/sbin/iptables -C HCB-INPUT -p tcp --dport "$p" -j ACCEPT 2>/dev/null \
            || /usr/sbin/iptables -I HCB-INPUT 1 -p tcp --dport "$p" -j ACCEPT 2>/dev/null || true
    fi
done

# 3) swap the binary (back up the old one).
[ -f "$DEST/toonui" ] && cp "$DEST/toonui" "$DEST/toonui.bak"
cp "$TMP/toonui" "$DEST/toonui"
chmod +x "$DEST/toonui"
rm -rf "$TMP"
say "installed $(ls -l "$DEST/toonui" | awk '{print $5}') bytes -> $DEST/toonui"

# 4) Take over the framebuffer from the stock GUI launcher (idempotent).
# Different Toon firmwares launch qt-gui from different inittab rows:
#   toon:/flas: -> /qmf/sbin/qt-gui directly,  or
#   qtqt:       -> /usr/bin/startqt  (which execs qt-gui; TSC-modified Toons).
# So we strip GUI-launcher rows by COMMAND (startqt / qt-gui / tenant-selection)
# not just by label, and install ui_launcher.sh as the single owner — it still
# runs qt-gui itself via the boot picker / ui_choice, so the two never fight.
GUI_RE='/usr/bin/startqt|/qmf/sbin/qt-gui|qt-tenant-selection|inittabwrap qt-gui'
ROW="toon:345:respawn:$DEST/ui_launcher.sh >> /var/volatile/tmp/toonui.log 2>&1"
if [ -f "$DEST/ui_launcher.sh" ]; then
    NEED=0
    grep -qF "$ROW" /etc/inittab 2>/dev/null || NEED=1
    grep -qE "$GUI_RE" /etc/inittab 2>/dev/null && NEED=1
    if [ "$NEED" = 1 ]; then
        say "taking over the GUI inittab row from the stock launcher"
        grep -vE "^toon:|^flas:|^qtqt:|$GUI_RE" /etc/inittab \
            > /etc/inittab.new \
            && echo "$ROW" >> /etc/inittab.new \
            && mv -f /etc/inittab.new /etc/inittab
        telinit q 2>/dev/null || kill -HUP 1 2>/dev/null || true
    fi
fi

# 4b) VNC respawn row — only if x11vnc + the input bridge are present, so VNC
# gives full control (not view-only). Idempotent.
if { [ -x /usr/bin/x11vnc ] || which x11vnc >/dev/null 2>&1 || [ -x "$DEST/x11vnc-bundle/bin/x11vnc" ]; } \
   && [ -x "$DEST/toonvnc.sh" ] && [ -x "$DEST/fbvnc_input" ]; then
    VROW="vncs:345:respawn:$DEST/toonvnc.sh respawn >> /var/volatile/tmp/x11vnc.log 2>&1"
    if ! grep -qF "$VROW" /etc/inittab 2>/dev/null; then
        say "enabling VNC (full control via fbvnc_input bridge) on :5900"
        grep -v '^vncs:' /etc/inittab > /etc/inittab.new \
            && echo "$VROW" >> /etc/inittab.new \
            && mv -f /etc/inittab.new /etc/inittab
        telinit q 2>/dev/null || kill -HUP 1 2>/dev/null || true
    fi
fi

# 4c) OPTIONAL: enable the USB host port (opt-in, OFF by default).
# The i.MX6 SoloX has two USB controllers; the stock display device-tree
# (imx6sx-nxt_display.dtb on the boot partition) leaves the second one in
# peripheral mode. Quby also ships imx6sx-nxt_display_usbhost.dtb, which puts
# it in host mode. Swapping the active dtb to that variant enables a usable
# USB-A host port after a reboot.
#
# This touches the BOOT PARTITION, so it's gated behind FREETOON_USB_HOST=1
# and is fully reversible (the original dtb is backed up to .orig). Enable with:
#   curl -fsSL .../toon-selfinstall.sh | FREETOON_USB_HOST=1 sh
if [ "${FREETOON_USB_HOST:-0}" = "1" ]; then
    BP=/tmp/freetoon_boot.$$
    DEV=/dev/mmcblk3p1
    DTB=imx6sx-nxt_display.dtb
    USBDTB=imx6sx-nxt_display_usbhost.dtb
    if [ -b "$DEV" ]; then
        mkdir -p "$BP"
        if mount -t vfat "$DEV" "$BP" 2>/dev/null; then
            if [ ! -f "$BP/$USBDTB" ]; then
                say "USB host: $USBDTB not on boot partition — skipping (unsupported firmware)."
            elif cmp -s "$BP/$DTB" "$BP/$USBDTB" 2>/dev/null; then
                say "USB host: already enabled (active dtb is the usbhost variant)."
            else
                [ -f "$BP/$DTB.orig" ] || cp "$BP/$DTB" "$BP/$DTB.orig" 2>/dev/null || true
                if cp "$BP/$USBDTB" "$BP/$DTB" 2>/dev/null; then
                    sync
                    say "USB host: enabled. REBOOT required to take effect."
                    say "USB host: to revert -> mount $DEV, 'cp $DTB.orig $DTB', reboot."
                else
                    say "USB host: failed to write dtb (read-only?) — left unchanged."
                fi
            fi
            umount "$BP" 2>/dev/null || true
        else
            say "USB host: could not mount $DEV — skipping."
        fi
        rmdir "$BP" 2>/dev/null || true
    else
        say "USB host: $DEV not found — skipping."
    fi
fi

# 5) restart the UI: stop any stock qt-gui AND the old toonui so init respawns
# through ui_launcher (single owner of the framebuffer).
say "restarting UI"
pkill -x qt-gui 2>/dev/null || true
kill "$(pidof toonui 2>/dev/null)" 2>/dev/null || pkill -x toonui 2>/dev/null || true
sleep 6
if pidof toonui >/dev/null 2>&1; then
    say "done — toonui is running (pid $(pidof toonui))."
elif pidof qt-gui >/dev/null 2>&1; then
    say "qt-gui is running — set ui_choice=freetoon (Settings) or pick freetoon at the boot picker."
else
    say "UI not detected yet; ui_launcher should respawn it from inittab within ~10s."
fi
