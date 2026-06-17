#!/bin/sh
# toonvnc.sh — start a VNC server for the Toon's framebuffer (toonui / qt-gui).
#
# Uses the stock /usr/bin/x11vnc in -rawfb mode (no X server) to serve
# /dev/fb0, with /mnt/data/fbvnc_input bridging remote pointer events back
# into the touchscreen (/dev/input/event1).
#
# Usage:  toonvnc.sh [start|stop|restart|status|respawn]   (default: start)
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

# Prefer the system x11vnc; fall back to the bundled copy. The Toon's opkg
# feed (feed.hae.int) is VPN-only, so the installer ships x11vnc + its libs
# under /mnt/data/x11vnc-bundle and we run it with LD_LIBRARY_PATH.
X11VNC=/usr/bin/x11vnc
if [ ! -x "$X11VNC" ]; then
  if [ -x /mnt/data/x11vnc-bundle/bin/x11vnc ]; then
    X11VNC=/mnt/data/x11vnc-bundle/bin/x11vnc
    export LD_LIBRARY_PATH="/mnt/data/x11vnc-bundle/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  else
    X11VNC=x11vnc
  fi
fi

# Auto-detect the framebuffer geometry — must NOT be hardcoded: Toon 2 is
# 1024x600x32, but Toon 1 (i.MX27) is 800x480x16 (RGB565). Feeding x11vnc the
# wrong WxHxBpp makes it misread the stride, shearing/garbling the image (the
# classic "top half scrambled, bottom OK" on a Toon 1). Probe fbset first (it
# reports visible W H + depth in one line), then fall back to sysfs, then to the
# Toon 2 default as a last resort.
_W=""; _H=""; _BPP=""
if which fbset >/dev/null 2>&1; then
  set -- $(fbset 2>/dev/null | awk '/geometry/{print $2, $3, $6}')   # W H depth
  _W=$1; _H=$2; _BPP=$3
fi
if [ -z "$_W" ] || [ -z "$_H" ]; then            # sysfs: visible mode "U:800x480p-0"
  _res=$(head -n 1 /sys/class/graphics/fb0/modes 2>/dev/null)   # busybox head needs -n 1, NOT -1
  _res=${_res#*:}; _res=${_res%%p*}; _res=${_res%%i*}
  case "$_res" in *x*) _W=${_res%x*}; _H=${_res#*x};; esac
fi
[ -z "$_BPP" ] && _BPP=$(cat /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null)
case "$_W"   in ''|*[!0-9]*) _W=1024;; esac      # last-resort Toon 2 defaults
case "$_H"   in ''|*[!0-9]*) _H=600;;  esac
case "$_BPP" in ''|*[!0-9]*) _BPP=32;; esac
GEOM="${_W}x${_H}x${_BPP}"
echo "toonvnc: framebuffer geometry $GEOM"

# Some Toon panels page-flip / pan the framebuffer: /dev/fb0 is double-height
# and the *visible* image sits at a non-zero Y offset. x11vnc -rawfb reads from
# byte 0 by default, so it would serve the stale back page (a frozen
# screenshot). Compute the live offset from the kernel's pan info and feed it to
# x11vnc. On non-panned panels (yoffset 0) this is a no-op.
RAWFB="map:/dev/fb0@$GEOM"
_pan=$(cat /sys/class/graphics/fb0/pan 2>/dev/null)   # "xoff,yoff"
_yoff=${_pan#*,}
case "$_yoff" in ''|*[!0-9]*) _yoff=0;; esac
_stride=$(cat /sys/class/graphics/fb0/stride 2>/dev/null)
case "$_stride" in ''|*[!0-9]*) _stride=$((_W * _BPP / 8));; esac
_off=$((_yoff * _stride))
# x11vnc wants the byte offset AFTER the @WxHxB geometry, e.g.
#   map:/dev/fb0@1024x600x32+2457600
[ "$_off" -gt 0 ] && RAWFB="map:/dev/fb0@$GEOM+$_off"

case "${1:-start}" in
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
    if [ ! -x "$INPUT" ]; then
      echo "WARNING: $INPUT missing/not executable — view-only mode"
      PIPE=""
    else
      PIPE="-pipeinput reopen:$INPUT"
    fi
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
  respawn)
    # Foreground variant for inittab. Identical args to `start` minus the
    # `-bg` flag, so init keeps a real child pid and respawns on death.
    if [ ! -x "$INPUT" ]; then
      PIPE=""
    else
      PIPE="-pipeinput reopen:$INPUT"
    fi
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
    echo "usage: $0 [start|stop|restart|status|respawn]"; exit 1
    ;;
esac
