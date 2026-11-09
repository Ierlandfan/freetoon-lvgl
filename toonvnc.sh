#!/bin/sh
# toonvnc.sh — start a VNC server for the Toon's framebuffer (toonui / qt-gui).
#
# Uses the stock /usr/bin/x11vnc in -rawfb mode (no X server) to serve
# /dev/fb0, with /mnt/data/fbvnc_input bridging remote pointer events back
# into the touchscreen (/dev/input/event1).
#
# Usage:  toonvnc.sh [start|stop|restart|status|respawn]    (default: start)
# Connect:  vncviewer <toon-ip>:5900
#
# `respawn` is what /etc/inittab calls — exec x11vnc in the foreground (no
# `-bg`) so init owns the pid and restarts it on crash / OOM. Logs go to
# stderr → inittab's `>> /var/volatile/tmp/x11vnc.log 2>&1`.
#
# Password: if /mnt/data/toonvnc.plain exists and is non-empty its first line
# is used as the VNC password (x11vnc -passwdfile). toonui's Settings >
# Remote control modal writes this file. No file / empty = no password.

PORT=5900
INPUT=/mnt/data/fbvnc_input
PASSFILE=/mnt/data/toonvnc.plain
LOG=/tmp/x11vnc.log

# Prefer the patched x11vnc if installed (built-in mt touch + fbdev pan following),
# then system /usr/bin/x11vnc, then bundled copy under /mnt/data.
X11VNC=""
for _cand in \
  /usr/local/lib/toon2-x11vnc-mt/x11vnc \
  /mnt/data/x11vnc \
  /usr/bin/x11vnc \
  /mnt/data/x11vnc-bundle/bin/x11vnc
do
  if [ -x "$_cand" ]; then
    X11VNC="$_cand"
    case "$_cand" in
      */x11vnc-bundle/*)
        export LD_LIBRARY_PATH="/mnt/data/x11vnc-bundle/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        ;;
    esac
    break
  fi
done
[ -n "$X11VNC" ] || X11VNC=/usr/bin/x11vnc

# Capture the command (start|stop|restart|status|respawn) NOW, before the
# geometry probe below runs `set --` — which on a device that HAS fbset would
# overwrite the positional params ($1) with the framebuffer dimensions, losing
# the action and dropping us into the usage branch (so inittab's `respawn` never
# launched x11vnc → VNC silently dead after a restart).
CMD="${1:-start}"

# Auto-detect the framebuffer geometry — must NOT be hardcoded: Toon 2 is
# 1024x600x32, but Toon 1 (i.MX27) is 800x480x16 (RGB565). Feeding x11vnc the
# wrong WxHxBpp makes it misread the stride, shearing/garbling the image (the
# classic "top half scrambled, bottom OK" on a Toon 1). Probe fbset first (it
# reports visible W H + depth in one line), then fall back to sysfs, then to the
# Toon 2 default as a last resort.
_W=""; _H=""; _BPP=""
if which fbset >/dev/null 2>&1; then
  set -- $(fbset 2>/dev/null | awk '/geometry/{print $2, $3, $6}')    # W H depth
  _W=$1; _H=$2; _BPP=$3
fi
if [ -z "$_W" ] || [ -z "$_H" ]; then             # sysfs: visible mode "U:800x480p-0"
  _res=$(head -n 1 /sys/class/graphics/fb0/modes 2>/dev/null)    # busybox head needs -n 1, NOT -1
  _res=${_res#*:}; _res=${_res%%p*}; _res=${_res%%i*}
  case "$_res" in *x*) _W=${_res%x*}; _H=${_res#*x};; esac
fi
if [ -z "$_W" ] || [ -z "$_H" ]; then
  _vsize=$(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null)
  case "$_vsize" in *,*) _W=${_vsize%,*}; _H=${_vsize#*,};; esac
fi
if [ -z "$_W" ] || [ -z "$_H" ]; then
  if grep -qi "i\.MX27\|ARM926" /proc/cpuinfo 2>/dev/null; then
    _W=800; _H=480; _BPP=16
  fi
fi
[ -z "$_BPP" ] && _BPP=$(cat /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null)

# Fallbacks and Toon 1 / Toon 2 safety checks
case "$_W"    in ''|*[!0-9]*) _W=1024;; esac      # last-resort Toon 2 defaults
case "$_H"    in ''|*[!0-9]*) _H=600;;  esac
case "$_BPP" in ''|*[!0-9]*) _BPP=32;; esac

# Force 16-bit color depth if width is 800 (Toon 1 hardware safety check)
if [ "$_W" = "800" ]; then
  _BPP=16
fi

GEOM="${_W}x${_H}x${_BPP}"
echo "toonvnc: framebuffer geometry $GEOM"

# Some Toon panels page-flip / pan the framebuffer: /dev/fb0 is double-height
# and the *visible* image sits at a non-zero Y offset. x11vnc -rawfb reads from
# byte 0 by default, so it would serve the stale back page (a frozen
# screenshot). Compute the live offset from the kernel's pan info and feed it to
# x11vnc. On non-panned panels (yoffset 0) this is a no-op.
RAWFB="map:/dev/fb0@$GEOM"
_pan=$(cat /sys/class/graphics/fb0/pan 2>/dev/null)    # "xoff,yoff"
_yoff=${_pan#*,}
case "$_yoff" in ''|*[!0-9]*) _yoff=0;; esac
_stride=$(cat /sys/class/graphics/fb0/stride 2>/dev/null)
case "$_stride" in ''|*[!0-9]*) _stride=$((_W * _BPP / 8));; esac
_off=$((_yoff * _stride))
# x11vnc wants the byte offset AFTER the @WxHxB geometry, e.g.
#    map:/dev/fb0@1024x600x32+2457600
[ "$_off" -gt 0 ] && RAWFB="map:/dev/fb0@$GEOM+$_off"

# Configure touch injection and framebuffer mapping.
# If x11vnc has built-in multitouch injection ("mt" option from our patch),
# write events directly into the touchscreen event device (with tslib calibration
# on resistive screens like Toon 1). The patched x11vnc also automatically tracks
# display pan via FBIOGET_VSCREENINFO, so static offsets are unnecessary.
# Otherwise, fall back to the external fbvnc_input helper.
setup_pipeinput() {
  TOUCH_DEV=""
  if [ -e /dev/input/touchscreen0 ]; then
    TOUCH_DEV=/dev/input/touchscreen0
  elif grep -q "TSC2007" /proc/bus/input/devices 2>/dev/null; then
    TOUCH_DEV=/dev/input/event0
  elif [ -e /dev/input/event1 ]; then
    TOUCH_DEV=/dev/input/event1
  elif [ -e /dev/input/event0 ]; then
    TOUCH_DEV=/dev/input/event0
  fi

  TSLIB_OPT=""
  if [ -f /etc/pointercal ]; then
    TSLIB_OPT=",tslib_cal=/etc/pointercal"
  fi

  if [ -n "$TOUCH_DEV" ] && "$X11VNC" -help 2>&1 | grep -q "mt"; then
    PIPE="-pipeinput UINPUT:mt,touch_always=1,pressure=128${TSLIB_OPT},direct_abs=$TOUCH_DEV,nouinput"
    RAWFB="map:/dev/fb0@$GEOM"
  elif [ -x "$INPUT" ]; then
    PIPE="-pipeinput reopen:$INPUT"
  else
    echo "WARNING: no touch injection method available — view-only mode"
    PIPE=""
  fi
}

case "$CMD" in
  stop)
    pkill -x x11vnc 2>/dev/null && echo "x11vnc stopped" || echo "x11vnc not running"
    ;;
  restart)
    pkill -x x11vnc 2>/dev/null
    sleep 1
    exec "$0" start
    ;;
  status)
    if pgrep -x x11vnc >/dev/null; then
      echo "x11vnc running (pid $(pgrep -x x11vnc)) on port $PORT"
    else
      echo "x11vnc not running"
    fi
    ;;
  start)
    if pgrep -x x11vnc >/dev/null; then
      echo "x11vnc already running (pid $(pgrep -x x11vnc))"
      exit 0
    fi
    setup_pipeinput
    if [ -s "$PASSFILE" ]; then
      AUTH="-passwdfile $PASSFILE"
    else
      AUTH="-nopw"
    fi
    "$X11VNC" \
      -rawfb "$RAWFB" \
      $PIPE \
      -rfbport $PORT -forever -shared -nocursor \
      -desktop ToonUI $AUTH \
      -bg -o "$LOG" >/dev/null 2>&1
    sleep 1
    if pgrep -x x11vnc >/dev/null; then
      echo "x11vnc started on port $PORT (log: $LOG)"
    else
      echo "x11vnc failed to start — see $LOG"; tail -n 5 "$LOG" 2>/dev/null
    fi
    ;;
  enable)
    VNCS_ROW="vncs:2345:respawn:/mnt/data/toonvnc.sh respawn >> /var/volatile/tmp/x11vnc.log 2>&1"
    if [ -x /mnt/data/inittab_edit.sh ]; then
      /mnt/data/inittab_edit.sh upsert vncs "$VNCS_ROW"
    else
      cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true
      grep -v '^vncs:' /etc/inittab > /etc/inittab.new 2>/dev/null || true
      echo "$VNCS_ROW" >> /etc/inittab.new
      if [ -s /etc/inittab.new ] && [ "$(wc -l < /etc/inittab.new 2>/dev/null || echo 0)" -ge 15 ]; then
        sync; mv -f /etc/inittab.new /etc/inittab; sync
        kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
      else
        rm -f /etc/inittab.new 2>/dev/null || true
      fi
    fi
    echo "x11vnc enabled in inittab"
    ;;
  disable)
    if [ -x /mnt/data/inittab_edit.sh ]; then
      /mnt/data/inittab_edit.sh drop vncs
    else
      cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true
      grep -v '^vncs:' /etc/inittab > /etc/inittab.new 2>/dev/null || true
      if [ -s /etc/inittab.new ] && [ "$(wc -l < /etc/inittab.new 2>/dev/null || echo 0)" -ge 15 ]; then
        sync; mv -f /etc/inittab.new /etc/inittab; sync
        kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
      else
        rm -f /etc/inittab.new 2>/dev/null || true
      fi
    fi
    sleep 1
    pkill -9 -x x11vnc 2>/dev/null || true
    echo "x11vnc disabled and stopped"
    ;;
  respawn)
    # Check if VNC is explicitly enabled in toonui.cfg. If not, drop inittab row and exit cleanly.
    if [ -f /mnt/data/toonui.cfg ] && ! grep -q '^vnc_enabled=1' /mnt/data/toonui.cfg 2>/dev/null; then
      echo "toonvnc: vnc_enabled is not 1 in toonui.cfg; removing from inittab"
      if [ -x /mnt/data/inittab_edit.sh ]; then
        /mnt/data/inittab_edit.sh drop vncs
      else
        cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true
        grep -v '^vncs:' /etc/inittab > /etc/inittab.new 2>/dev/null || true
        if [ -s /etc/inittab.new ] && [ "$(wc -l < /etc/inittab.new 2>/dev/null || echo 0)" -ge 15 ]; then
          sync; mv -f /etc/inittab.new /etc/inittab 2>/dev/null || true; sync
          kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
        else
          rm -f /etc/inittab.new 2>/dev/null || true
        fi
      fi
      exit 0
    fi
    # If x11vnc binary is missing, remove inittab respawn to prevent tight loop
    if ! command -v "$X11VNC" >/dev/null 2>&1 && [ ! -x "$X11VNC" ]; then
      echo "toonvnc: x11vnc binary not found; removing from inittab"
      if [ -x /mnt/data/inittab_edit.sh ]; then
        /mnt/data/inittab_edit.sh drop vncs
      else
        cp -p /etc/inittab /etc/inittab.bak 2>/dev/null || true
        grep -v '^vncs:' /etc/inittab > /etc/inittab.new 2>/dev/null || true
        if [ -s /etc/inittab.new ] && [ "$(wc -l < /etc/inittab.new 2>/dev/null || echo 0)" -ge 15 ]; then
          sync; mv -f /etc/inittab.new /etc/inittab 2>/dev/null || true; sync
          kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
        else
          rm -f /etc/inittab.new 2>/dev/null || true
        fi
      fi
      exit 0
    fi
    # Foreground variant for inittab. Identical args to `start` minus the
    # `-bg` flag, so init keeps a real child pid and respawns on death.
    setup_pipeinput
    if [ -s "$PASSFILE" ]; then
      AUTH="-passwdfile $PASSFILE"
    else
      AUTH="-nopw"
    fi
    # exec replaces the shell with x11vnc, so init sees x11vnc directly
    # (clean `pkill -x x11vnc` from the existing stop/restart actions).
    exec "$X11VNC" \
      -rawfb "$RAWFB" \
      $PIPE \
      -rfbport $PORT -forever -shared -nocursor \
      -desktop ToonUI $AUTH
    ;;
  *)
    echo "usage: $0 [start|stop|restart|status|respawn|enable|disable]"; exit 1
    ;;
esac
