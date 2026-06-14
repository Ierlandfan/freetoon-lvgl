#!/bin/sh
# Full-stop the dev Toon's local data publishers so boxtalk_mirror's injected
# (mirrored) data is the only source the stock qt-gui sees. Reversible:
#   boxtalk_mirror_mute.sh          -> comment the rows + kill the daemons
#   boxtalk_mirror_mute.sh --undo   -> restore them
#
# We stop ONLY the data publishers we replace. We KEEP hcb_comm (the bus),
# hcb_config, lighttpd, etc. — qt-gui still needs those.
set -u
ROWS="ther sens p1p1 hvac pwru"
DAEMONS="happ_thermstat hdrv_sensory hdrv_p1 happ_hvac happ_pwrusage"
IT=/etc/inittab
SD=/qmf/etc/start.d   # ⚠ HCBv2 REGENERATES /etc/inittab from these markers at boot,
                      # so an inittab-only mute does NOT survive a reboot. Must disable
                      # the start.d marker too (verified 2026-06-07).

# kill a running daemon by its real exe path (busybox pkill -x fails on prctl-renamed HCB daemons)
kill_daemon() {
    for d in /proc/[0-9]*; do
        e=$(readlink "$d/exe" 2>/dev/null)
        case "$e" in */"$1") kill -9 "${d#/proc/}" 2>/dev/null;; esac
    done
}

if [ "${1:-}" = "--undo" ]; then
    for d in $DAEMONS; do [ -f "$SD/$d.muted" ] && mv "$SD/$d.muted" "$SD/$d"; done
    for r in $ROWS; do sed -i "s/^#MIRROR#${r}:/${r}:/" "$IT"; done
    telinit q 2>/dev/null || kill -HUP 1 2>/dev/null || true
    echo "restored start.d markers + inittab rows: $DAEMONS"
    exit 0
fi
# 1. persistent: disable the start.d marker so HCBv2 won't re-add the daemon on boot
for d in $DAEMONS; do [ -f "$SD/$d" ] && mv "$SD/$d" "$SD/$d.muted"; done
# 2. this session: comment the inittab rows + stop respawn
for r in $ROWS; do sed -i "s/^${r}:/#MIRROR#${r}:/" "$IT"; done
telinit q 2>/dev/null || kill -HUP 1 2>/dev/null || true
# 3. kill the currently-running real daemons (by exe, not name)
for d in $DAEMONS; do kill_daemon "$d"; done
echo "muted ($DAEMONS) via start.d markers + inittab — boxtalk_mirror is now the data source (survives reboot)"
