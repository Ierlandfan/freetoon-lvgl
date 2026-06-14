#!/bin/sh
# boxtalk_setup.sh — ONE interactive installer for the BoxTalk master/mirror
# feature. Replaces juggling boxtalk_proxy / _mute / _run / _client by hand.
#
#   MASTER = the Toon you want to watch & control from afar (e.g. your home Toon).
#            It shares its live data + accepts control, and stays fully normal.
#   CLIENT = a Toon that becomes a live remote SCREEN of the master (e.g. a spare
#            or remote Toon). Its own sensors switch off; its STOCK screen shows
#            the master's temperature / setpoint / program / water pressure, and
#            its on-screen buttons drive the master. (One reboot to take effect.)
#
# Run it on each Toon with no arguments:  sh /mnt/data/boxtalk_setup.sh
set -u
DATA=/mnt/data
IT=/etc/inittab
AUTH=$DATA/boxtalk_proxy.auth
PROXY=$DATA/boxtalk_proxy
MIRROR=$DATA/boxtalk_mirror
RUN=$DATA/boxtalk_mirror_run.sh
MUTE=$DATA/boxtalk_mirror_mute.sh
FWCONF=/etc/default/iptables.conf
IPT=$(command -v iptables 2>/dev/null); [ -n "$IPT" ] || IPT=/usr/sbin/iptables
DROPLINE='-A HCB-INPUT -p tcp -m tcp --tcp-flags SYN,RST,ACK SYN -j DROP'

say()  { printf '%s\n' "$*"; }
ask()  { printf '%s' "$1"; read REPLY; }          # ask "prompt"; answer in $REPLY
yesno(){ printf '%s [y/N]: ' "$1"; read a; case "$a" in y|Y|yes) return 0;; *) return 1;; esac; }
reload_init() { telinit q 2>/dev/null || kill -HUP 1 2>/dev/null || true; }
hr()   { say "------------------------------------------------------------------"; }

state() {   # master | client | stock
    if grep -q '^bxpx:' "$IT" 2>/dev/null; then echo master
    elif grep -q '^tmir:' "$IT" 2>/dev/null; then echo client
    else echo stock; fi
}

# ---------------------------------------------------------------- MASTER
setup_master() {
    hr; say " MASTER setup"; hr
    say "This Toon will run boxtalk_proxy: an authenticated gateway that lets a"
    say "client Toon read this Toon's live bus over the LAN/VPN. Nothing about this"
    say "Toon's own operation changes."; say ""
    [ -x "$PROXY" ] || { say "!! $PROXY not found. Copy the boxtalk_proxy binary there first."; return 1; }
    ask "Listen port [1338]: ";                 PORT=${REPLY:-1338}
    ask "Choose a username the client will use: "; U=$REPLY
    ask "Choose a password: ";                   P=$REPLY
    ask "Client Toon's IP (firewall opens to it only), e.g. 10.10.20.3: "; CIP=$REPLY
    [ -n "$U" ] && [ -n "$P" ] && [ -n "$CIP" ] || { say "Missing input — aborted."; return 1; }
    printf '%s:%s\n' "$U" "$P" > "$AUTH"; chmod 600 "$AUTH"
    # firewall: open PORT to the client IP only (live + persisted before the DROP)
    $IPT -C HCB-INPUT -p tcp -s "$CIP" --dport "$PORT" -j ACCEPT 2>/dev/null \
        || $IPT -I HCB-INPUT 2 -p tcp -s "$CIP" --dport "$PORT" -j ACCEPT
    if ! grep -q "dport $PORT" "$FWCONF" 2>/dev/null; then
        cp "$FWCONF" "$FWCONF.bak.boxtalk" 2>/dev/null
        sed -i "\|$DROPLINE|i -A HCB-INPUT -p tcp -m tcp -s $CIP --dport $PORT --tcp-flags SYN,RST,ACK SYN -j ACCEPT" "$FWCONF"
    fi
    # init-manage the proxy so it survives reboots
    sed -i '/^bxpx:/d' "$IT"
    printf 'bxpx:345:respawn:%s %s >>/var/volatile/tmp/boxtalk_proxy.log 2>&1\n' "$PROXY" "$PORT" >> "$IT"
    reload_init
    say ""; say ">> DONE. This Toon is a MASTER on port $PORT (firewall open to $CIP)."
    say ">> On the CLIENT Toon run this installer, pick CLIENT, and enter:"
    say "      host = THIS Toon's IP   port = $PORT   user = $U   pass = $P"
}

# ---------------------------------------------------------------- CLIENT
setup_client() {
    hr; say " CLIENT setup"; hr
    say "This Toon's STOCK screen will mirror a master Toon. Its own sensors are"
    say "switched off (fully reversible). A reboot is needed for the screen to bind."
    say ""
    [ -x "$MIRROR" ] || { say "!! $MIRROR not found. Copy the boxtalk_mirror binary first."; return 1; }
    [ -x "$RUN" ] || { say "!! $RUN not found."; return 1; }
    ask "Master Toon host/IP: ";        MH=$REPLY
    ask "Master port [1338]: ";         MP=${REPLY:-1338}
    ask "Username: ";                   MU=$REPLY
    ask "Password: ";                   MPW=$REPLY
    [ -n "$MH" ] && [ -n "$MU" ] && [ -n "$MPW" ] || { say "Missing input — aborted."; return 1; }
    say ""; say "Muting local sensors + installing the mirror..."
    "$MUTE"
    "$RUN" install "$MH" "$MP" "$MU" "$MPW"
    say ""; say ">> DONE. The mirror is installed and points at $MH:$MP."
    if yesno ">> Reboot now to activate the mirror"; then
        say "Rebooting..."; (sleep 1; /sbin/reboot) >/dev/null 2>&1 &
    else
        say ">> Not rebooting. Run '/sbin/reboot' when ready (required to show the mirror)."
    fi
}

# ---------------------------------------------------------------- UNDO
undo() {
    s=$(state)
    case "$s" in
      master)
        say "Removing MASTER role (stop proxy + firewall)..."
        sed -i '/^bxpx:/d' "$IT"; reload_init
        for d in /proc/[0-9]*; do e=$(readlink "$d/exe" 2>/dev/null); [ "$e" = "$PROXY" ] && kill "${d#/proc/}" 2>/dev/null; done
        # remove the firewall rule(s) we added for :1338-ish ports
        $IPT -S HCB-INPUT 2>/dev/null | grep 'dport 13' | sed 's/^-A/-D/' | while read r; do $IPT $r 2>/dev/null; done
        sed -i '/--dport 13[0-9][0-9] --tcp-flags/d' "$FWCONF" 2>/dev/null
        say ">> MASTER role removed. (auth file left at $AUTH — delete it if you like.)" ;;
      client)
        say "Reverting CLIENT to stock (restore sensors + remove mirror)..."
        "$RUN" --undo; "$MUTE" --undo
        say ">> Client reverted. REBOOT to fully restore stock daemons."
        yesno "Reboot now" && { say "Rebooting..."; (sleep 1; /sbin/reboot) >/dev/null 2>&1 & } ;;
      *) say "Nothing to undo — this Toon is already stock." ;;
    esac
}

# ---------------------------------------------------------------- STATUS
status() {
    hr; say " Status"; hr
    s=$(state)
    say "This Toon is currently: $s"
    case "$s" in
      master) say "  proxy row : $(grep '^bxpx:' $IT)"
              say "  firewall  : $($IPT -S HCB-INPUT 2>/dev/null | grep 'dport 13' | tr '\n' ';')"
              say "  auth file : $([ -f "$AUTH" ] && echo present || echo MISSING)" ;;
      client) say "  mirror rows: $(grep -cE '^tm[a-z]+:' $IT) installed"
              say "  conf       : $(cat $DATA/boxtalk_mirror.conf 2>/dev/null | tr '\n' ' ')"
              say "  muted      : $(ls /qmf/etc/start.d 2>/dev/null | grep -c muted) daemons" ;;
      stock)  say "  (no mirror or proxy configured)" ;;
    esac
}

# ---------------------------------------------------------------- MENU
[ -t 0 ] && clear 2>/dev/null
hr
say " BoxTalk master / mirror installer        (this Toon is: $(state))"
hr
say ""
say "  MASTER = the Toon to watch/control from afar (shares its data)."
say "  CLIENT = a Toon that becomes a remote screen of a master."
say ""
say "  1) Set up this Toon as MASTER  (share its data + accept control)"
say "  2) Set up this Toon as CLIENT  (mirror a master on this screen)"
say "  3) Status"
say "  4) Undo / revert this Toon to stock"
say "  5) Quit"
say ""
ask "Choose [1-5]: "
case "$REPLY" in
  1) setup_master ;;
  2) setup_client ;;
  3) status ;;
  4) undo ;;
  *) say "Bye." ;;
esac
