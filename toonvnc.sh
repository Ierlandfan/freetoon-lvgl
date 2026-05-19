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
    /usr/bin/x11vnc \
      -rawfb map:/dev/fb0@1024x600x32 \
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
    exec /usr/bin/x11vnc \
      -rawfb map:/dev/fb0@1024x600x32 \
      $PIPE \
      -rfbport $PORT -forever -shared -nocursor \
      -desktop ToonUI $AUTH
    ;;
  *)
    echo "usage: $0 [start|stop|restart|status|respawn]"; exit 1
    ;;
esac
