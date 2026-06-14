#!/bin/sh
# toon_setup.sh — ONE interactive installer / mode-switcher for this Toon.
# Works on Toon 1 (armv5) and Toon 2 (armv7).  Run on the Toon:
#     sh /mnt/data/toon_setup.sh
#
# It groups everything into a small looping menu instead of remembering the
# separate _proxy / _mute / _run / _client / ui_choice bits by hand:
#
#   DEFAULT BOOT UI  (what the screen runs; written to /mnt/data/ui_choice,
#                     ui_launcher.sh acts on it; the boot picker can still
#                     override per-boot):
#       Stock qt-gui      - original Eneco UI.
#       freetoon-lvgl     - custom UI; also serves the phone web app on :10081.
#       qt-gui + WASM PWA - stock screen, PLUS a headless toonui serving the
#                           web app on :10081 (control from a phone browser).
#
#   REMOTE MIRROR (BoxTalk):
#       MASTER  - THIS Toon shares its live data + accepts control (stays normal).
#       CLIENT  - THIS Toon's stock screen becomes a remote screen of a MASTER.
#
# Binaries (toonui / boxtalk_proxy / boxtalk_mirror / toon_wasm_host.sh) must
# already be on /mnt/data — deploy them with the host-side install.sh first.
set -u
DATA=/mnt/data
IT=/etc/inittab
CHOICE=$DATA/ui_choice
CFG=$DATA/toonui.cfg
TOONUI=$DATA/toonui
WASMHOST=$DATA/toon_wasm_host.sh
AUTH=$DATA/boxtalk_proxy.auth
PROXY=$DATA/boxtalk_proxy
MIRROR=$DATA/boxtalk_mirror
RUN=$DATA/boxtalk_mirror_run.sh
MUTE=$DATA/boxtalk_mirror_mute.sh
FWCONF=/etc/default/iptables.conf
IPT=$(command -v iptables 2>/dev/null); [ -n "$IPT" ] || IPT=/usr/sbin/iptables
DROP='-A HCB-INPUT -p tcp -m tcp --tcp-flags SYN,RST,ACK SYN -j DROP'
NEED_REBOOT=0

case "$(uname -m)" in armv5*) HW=Toon1;; armv7*) HW=Toon2;; *) HW="Toon?";; esac

say(){ printf '%s\n' "$*"; }
hr(){ say "------------------------------------------------------------------"; }
ask(){ printf '%s' "$1"; read REPLY; }
yesno(){ printf '%s [y/N]: ' "$1"; read a; case "$a" in y|Y|yes) return 0;; *) return 1;; esac; }
pause(){ printf 'Press Enter...'; read _; }
reload_init(){ telinit q 2>/dev/null || kill -HUP 1 2>/dev/null || true; }
have(){ [ -x "$1" ]; }

ui_now(){ c=$(cat "$CHOICE" 2>/dev/null | tr -d '[:space:]'); [ -n "$c" ] || c=freetoon
  case "$c" in qt-gui|qtgui|stock) echo "stock qt-gui";; wasm|wasm-host|masterslave) echo "qt-gui + WASM PWA";; *) echo "freetoon-lvgl";; esac; }
picker_on(){ grep -q '^boot_picker_enabled=1' "$CFG" 2>/dev/null && echo on || echo off; }
mirror_role(){ if grep -q '^bxpx:' "$IT" 2>/dev/null; then echo MASTER
  elif grep -q '^tmir:' "$IT" 2>/dev/null; then echo CLIENT; else echo none; fi; }
warn_t1_touch(){ [ "$HW" = Toon1 ] && say "   (Toon1: freetoon uses the built-in TSC2007 touch driver on /dev/input/event0; stock UI launches via startqt.)"; }

set_choice(){ printf '%s\n' "$1" > "$CHOICE"; NEED_REBOOT=1; }

# ---- DEFAULT BOOT UI submenu -------------------------------------------
menu_ui(){
  while true; do
    hr; say " Default boot UI   (current: $(ui_now) · picker: $(picker_on))"; hr
    say "  1) Stock qt-gui"
    say "  2) freetoon-lvgl        (+ phone PWA :10081)"
    say "  3) qt-gui + WASM PWA    (stock panel + phone control :10081)"
    say "  4) Boot picker: toggle  (now: $(picker_on))   — 10s switch screen at boot"
    say "  5) Back"
    ask "Choose [1-5]: "
    case "$REPLY" in
      1) set_choice qt-gui; say ">> default = stock qt-gui";;
      2) if have "$TOONUI"; then set_choice freetoon; say ">> default = freetoon-lvgl"; warn_t1_touch
         else say "!! toonui not installed — deploy via host install.sh first."; fi;;
      3) if ! have "$TOONUI"; then say "!! toonui not installed — deploy first."
         else set_choice wasm
           if have "$WASMHOST"; then grep -q '^tuih:' "$IT" || { printf 'tuih:345:respawn:%s >>/var/volatile/tmp/toon_wasm_host.log 2>&1\n' "$WASMHOST" >> "$IT"; reload_init; }
                say ">> default = qt-gui + WASM PWA"
           else say ">> screen will be qt-gui, but toon_wasm_host.sh is MISSING — PWA host won't run. Deploy it for :10081."; fi
           warn_t1_touch; fi;;
      4) if [ "$(picker_on)" = on ]; then
            if grep -q '^boot_picker_enabled=' "$CFG" 2>/dev/null; then sed -i 's/^boot_picker_enabled=.*/boot_picker_enabled=0/' "$CFG"; else printf 'boot_picker_enabled=0\n' >> "$CFG"; fi; say ">> boot picker OFF (boots default directly)"
         else
            if grep -q '^boot_picker_enabled=' "$CFG" 2>/dev/null; then sed -i 's/^boot_picker_enabled=.*/boot_picker_enabled=1/' "$CFG"; else printf 'boot_picker_enabled=1\n' >> "$CFG"; fi; say ">> boot picker ON (10s switch window at boot)"; fi
         NEED_REBOOT=1;;
      5) return;;
      *) say "?";;
    esac
  done
}

# ---- MIRROR submenu -----------------------------------------------------
setup_master(){
  have "$PROXY" || { say "!! $PROXY not found — deploy boxtalk_proxy first."; return; }
  ask "Listen port [1338]: "; PORT=${REPLY:-1338}
  ask "Username for the client: "; U=$REPLY
  ask "Password: "; P=$REPLY
  ask "Client Toon IP (firewall opens to it only): "; CIP=$REPLY
  [ -n "$U" ] && [ -n "$P" ] && [ -n "$CIP" ] || { say "missing input — aborted."; return; }
  printf '%s:%s\n' "$U" "$P" > "$AUTH"; chmod 600 "$AUTH"
  $IPT -C HCB-INPUT -p tcp -s "$CIP" --dport "$PORT" -j ACCEPT 2>/dev/null || $IPT -I HCB-INPUT 2 -p tcp -s "$CIP" --dport "$PORT" -j ACCEPT
  grep -q "dport $PORT" "$FWCONF" 2>/dev/null || { cp "$FWCONF" "$FWCONF.bak.boxtalk" 2>/dev/null; sed -i "\|$DROP|i -A HCB-INPUT -p tcp -m tcp -s $CIP --dport $PORT --tcp-flags SYN,RST,ACK SYN -j ACCEPT" "$FWCONF"; }
  sed -i '/^bxpx:/d' "$IT"; printf 'bxpx:345:respawn:%s %s >>/var/volatile/tmp/boxtalk_proxy.log 2>&1\n' "$PROXY" "$PORT" >> "$IT"; reload_init
  say ">> MASTER on port $PORT, firewall open to $CIP."
  say ">> On the CLIENT: pick CLIENT and enter host=THIS-Toon-IP port=$PORT user=$U pass=$P"
}
setup_client(){
  have "$MIRROR" || { say "!! $MIRROR not found — deploy boxtalk_mirror first."; return; }
  have "$RUN" || { say "!! $RUN not found."; return; }
  ask "Master host/IP: "; MH=$REPLY
  ask "Master port [1338]: "; MP=${REPLY:-1338}
  ask "Username: "; MU=$REPLY
  ask "Password: "; MPW=$REPLY
  [ -n "$MH" ] && [ -n "$MU" ] && [ -n "$MPW" ] || { say "missing input — aborted."; return; }
  set_choice qt-gui                       # mirror shows through stock qt-gui
  "$MUTE"; "$RUN" install "$MH" "$MP" "$MU" "$MPW"
  say ">> CLIENT/mirror installed → $MH:$MP."; NEED_REBOOT=1
}
disable_mirror(){
  m=$(mirror_role)
  case "$m" in
    MASTER) sed -i '/^bxpx:/d' "$IT"; reload_init
            for d in /proc/[0-9]*; do e=$(readlink "$d/exe" 2>/dev/null); [ "$e" = "$PROXY" ] && kill "${d#/proc/}" 2>/dev/null; done
            $IPT -S HCB-INPUT 2>/dev/null | grep 'dport 13' | sed 's/^-A/-D/' | while read r; do $IPT $r 2>/dev/null; done
            sed -i '/--dport 13[0-9][0-9] --tcp-flags/d' "$FWCONF" 2>/dev/null; say ">> MASTER removed.";;
    CLIENT) "$RUN" --undo; "$MUTE" --undo; say ">> CLIENT reverted (sensors restored)."; NEED_REBOOT=1;;
    *) say "No mirror configured.";;
  esac
}
menu_mirror(){
  while true; do
    hr; say " Remote mirror   (this Toon: $(mirror_role))"; hr
    say "  1) MASTER  — share THIS Toon's data to a client"
    say "  2) CLIENT  — mirror a master Toon on THIS screen"
    say "  3) Disable mirror"
    say "  4) Back"
    ask "Choose [1-4]: "
    case "$REPLY" in 1) setup_master;; 2) setup_client;; 3) disable_mirror;; 4) return;; *) say "?";; esac
  done
}

status(){
  hr; say " Status — $HW"; hr
  say "Screen default : $(ui_now)    (boot picker: $(picker_on))"
  say "Remote mirror  : $(mirror_role)"
  [ "$(mirror_role)" = CLIENT ] && say "  -> $(cat $DATA/boxtalk_mirror.conf 2>/dev/null | tr '\n' ' ')"
  [ "$(mirror_role)" = MASTER ] && say "  -> $(grep '^bxpx:' $IT)"
  say "PWA :10081     : $($IPT -S HCB-INPUT 2>/dev/null | grep -q 'dport 10081' && echo open || echo closed)"
  say "installed      : toonui=$(have $TOONUI&&echo y||echo n) wasmhost=$(have $WASMHOST&&echo y||echo n) proxy=$(have $PROXY&&echo y||echo n) mirror=$(have $MIRROR&&echo y||echo n)"
}
undo_all(){
  yesno "Revert EVERYTHING to stock qt-gui (remove mirror + WASM host)" || return
  disable_mirror
  set_choice qt-gui; sed -i '/^tuih:/d' "$IT" 2>/dev/null; reload_init
  say ">> Reverted to stock qt-gui."
}

# ---- main loop ----------------------------------------------------------
while true; do
  [ -t 0 ] && clear 2>/dev/null
  hr; say " TOON setup — $HW        now: $(ui_now)$( [ "$(mirror_role)" = none ] || printf ' + mirror:%s' "$(mirror_role)")"; hr
  say ""
  say "  1) Default boot UI     (stock qt-gui / freetoon / qt-gui+WASM, + picker)"
  say "  2) Remote mirror       (MASTER share / CLIENT mirror a master)"
  say "  3) Status"
  say "  4) Undo / back to stock"
  say "  5) Quit$( [ "$NEED_REBOOT" = 1 ] && printf '   (* changes pending — reboot to apply)')"
  say ""
  ask "Choose [1-5]: "
  case "$REPLY" in
    1) menu_ui;;
    2) menu_mirror;;
    3) status; pause;;
    4) undo_all; pause;;
    5) if [ "$NEED_REBOOT" = 1 ] && yesno "Changes pending — reboot now to apply"; then
         say "Rebooting..."; (sleep 1; /sbin/reboot) >/dev/null 2>&1 &
       fi
       say "Bye."; exit 0;;
    *) say "?"; pause;;
  esac
done
