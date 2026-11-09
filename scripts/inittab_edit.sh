#!/bin/sh
# inittab_edit.sh — Bulletproof atomic /etc/inittab management for Toon
#
# Guarantees:
# 1. Atomic file locking via mkdir (/etc/inittab.lock) to eliminate race conditions
# 2. Disk space pre-flight check (requires >= 100 KB free on rootfs)
# 3. PID-isolated temporary files (/etc/inittab.tmp.$$)
# 4. Permanent factory golden backup (/etc/inittab.orig) created once, never overwritten
# 5. Strict multi-point validation before commit:
#    - Non-empty and readable
#    - At least 15 lines (catches partial/truncated writes on full disk)
#    - Critical sysvinit anchors preserved: :initdefault:, :sysinit:, rcS, /etc/init.d/rc
#    - Line count sanity delta check
# 6. Flash filesystem safety (sync -> atomic rename -> sync) for UBIFS / JFFS2
# 7. Automatic fallback to backup on validation failure
# 8. Reload init (kill -HUP 1 / telinit q) only on verified successful write

set -eu

INITTAB="/etc/inittab"
LOCK_DIR="/etc/inittab.lock"
ORIG_BACKUP="/etc/inittab.orig"
BAK_BACKUP="/etc/inittab.bak"
TMP_FILE="/etc/inittab.tmp.$$"
LOCK_HELD=0

acquire_lock() {
    # Spin-lock with timeout (10 seconds)
    for _i in $(seq 1 20); do
        if mkdir "$LOCK_DIR" 2>/dev/null; then
            LOCK_HELD=1
            echo $$ > "$LOCK_DIR/pid" 2>/dev/null || true
            return 0
        fi
        # Clean up stale locks if the owning process is dead or lock is older than 60s
        if [ -d "$LOCK_DIR" ]; then
            _owner_pid=$(cat "$LOCK_DIR/pid" 2>/dev/null || true)
            if [ -n "$_owner_pid" ] && ! kill -0 "$_owner_pid" 2>/dev/null; then
                rm -f "$LOCK_DIR/pid" 2>/dev/null || true
                rmdir "$LOCK_DIR" 2>/dev/null || true
                continue
            fi
            _lock_age=$(find "$LOCK_DIR" -maxdepth 0 -mmin +1 2>/dev/null || true)
            if [ -n "$_lock_age" ]; then
                rm -f "$LOCK_DIR/pid" 2>/dev/null || true
                rmdir "$LOCK_DIR" 2>/dev/null || true
            fi
        fi
        sleep 0.5
    done
    echo "inittab_edit: ERROR: timed out waiting for lock $LOCK_DIR" >&2
    return 1
}

release_lock() {
    rm -f "$TMP_FILE" 2>/dev/null || true
    if [ "${LOCK_HELD:-0}" = "1" ]; then
        rm -f "$LOCK_DIR/pid" 2>/dev/null || true
        rmdir "$LOCK_DIR" 2>/dev/null || true
        LOCK_HELD=0
    fi
}

trap release_lock EXIT INT TERM

check_disk_space() {
    _free_kb=$(df -k /etc 2>/dev/null | awk 'NR>1 {print $4}' | tail -n 1)
    if [ -n "${_free_kb:-}" ] && [ "$_free_kb" -lt 100 ] 2>/dev/null; then
        echo "inittab_edit: ERROR: insufficient disk space on /etc (${_free_kb} KB free, >= 100 KB required)" >&2
        return 1
    fi
    return 0
}

validate_inittab() {
    _f="$1"
    [ -f "$_f" ] || return 1
    [ -s "$_f" ] || return 1

    # Line count check: a genuine Toon inittab has 30-40 lines; 15 is absolute minimum
    _lines=$(wc -l < "$_f" 2>/dev/null || echo 0)
    [ "$_lines" -ge 15 ] || return 1

    # Essential sysvinit lines that cannot be missing
    grep -q ':initdefault:' "$_f" 2>/dev/null || return 1
    grep -q ':sysinit:' "$_f" 2>/dev/null || return 1
    grep -q 'rcS' "$_f" 2>/dev/null || return 1
    grep -q '/etc/init.d/rc ' "$_f" 2>/dev/null || return 1

    return 0
}

commit_inittab() {
    _orig_lines=$(wc -l < "$INITTAB" 2>/dev/null || echo 0)
    _new_lines=$(wc -l < "$TMP_FILE" 2>/dev/null || echo 0)

    # Validate temp file
    if ! validate_inittab "$TMP_FILE"; then
        echo "inittab_edit: ERROR: validation failed for new inittab (lines=$_new_lines)! Aborting." >&2
        rm -f "$TMP_FILE" 2>/dev/null || true
        # Ensure active inittab is still valid; if not, restore from backup
        if ! validate_inittab "$INITTAB"; then
            echo "inittab_edit: CRITICAL: active inittab is invalid! Restoring backup..." >&2
            if validate_inittab "$BAK_BACKUP"; then
                cp -f "$BAK_BACKUP" "$INITTAB"
            elif validate_inittab "$ORIG_BACKUP"; then
                cp -f "$ORIG_BACKUP" "$INITTAB"
            fi
            sync
        fi
        return 1
    fi

    # Line count delta check (sanity check against massive truncation)
    if [ "$_orig_lines" -ge 15 ]; then
        _delta=$(( _new_lines - _orig_lines ))
        if [ "$_delta" -lt -5 ] || [ "$_delta" -gt 5 ]; then
            echo "inittab_edit: ERROR: excessive line change delta (old=$_orig_lines, new=$_new_lines). Aborting." >&2
            rm -f "$TMP_FILE" 2>/dev/null || true
            return 1
        fi
    fi

    # Ensure factory golden backup exists before first write
    if [ ! -f "$ORIG_BACKUP" ] && validate_inittab "$INITTAB"; then
        cp -p "$INITTAB" "$ORIG_BACKUP" 2>/dev/null || true
        sync
    fi

    # Always keep immediate prior backup
    cp -p "$INITTAB" "$BAK_BACKUP" 2>/dev/null || true

    # Flash-safe atomic swap on UBIFS
    sync
    mv -f "$TMP_FILE" "$INITTAB"
    sync

    # Reload sysvinit
    kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
    return 0
}

cmd="${1:-}"
case "$cmd" in
    upsert)
        # Usage: inittab_edit.sh upsert <row_id> "<full_inittab_row>"
        row_id="$2"
        row_content="$3"
        [ -n "$row_id" ] || exit 1
        [ -n "$row_content" ] || exit 1

        acquire_lock
        check_disk_space

        # Filter out existing row with this id, append new row
        grep -v "^${row_id}:" "$INITTAB" > "$TMP_FILE" 2>/dev/null || true
        echo "$row_content" >> "$TMP_FILE"

        commit_inittab
        ;;

    drop)
        # Usage: inittab_edit.sh drop <row_id>
        row_id="$2"
        [ -n "$row_id" ] || exit 1

        acquire_lock
        check_disk_space

        grep -v "^${row_id}:" "$INITTAB" > "$TMP_FILE" 2>/dev/null || true

        commit_inittab
        ;;

    drop_safe)
        # Usage: inittab_edit.sh drop_safe <row_id> <must_contain_text>
        # Only drops row_id if the line ALSO contains must_contain_text, matched as a
        # literal substring (not a regex), e.g. "/mnt/data/integrations/<id>/".
        # Guarantees stock system daemons (ther:, comm:, conf:, etc.) are never accidentally deleted.
        row_id="$2"
        pattern="$3"
        [ -n "$row_id" ] || exit 1
        [ -n "$pattern" ] || exit 1

        acquire_lock
        check_disk_space

        awk -v id="$row_id" -v pat="$pattern" '
            index($0, id ":") == 1 && index($0, pat) > 0 { next }
            { print $0 }
        ' "$INITTAB" > "$TMP_FILE"

        commit_inittab
        ;;

    comment_pattern)
        # Usage: inittab_edit.sh comment_pattern "startqt|qt-gui"
        pattern="$2"
        [ -n "$pattern" ] || exit 1

        acquire_lock
        check_disk_space

        # Use awk instead of sed -i to avoid temp-file corruption on BusyBox
        awk -v pat="$pattern" '
            $0 ~ pat && $0 !~ /^#/ { print "#FREETOON#" $0; next }
            { print $0 }
        ' "$INITTAB" > "$TMP_FILE"

        commit_inittab
        ;;

    uncomment_freetoon)
        # Usage: inittab_edit.sh uncomment_freetoon
        acquire_lock
        check_disk_space

        awk '
            /^#FREETOON#/ { sub(/^#FREETOON#/, ""); print $0; next }
            { print $0 }
        ' "$INITTAB" > "$TMP_FILE"

        commit_inittab
        ;;

    restore_stock_qt)
        # Usage: inittab_edit.sh restore_stock_qt
        acquire_lock
        check_disk_space

        awk '
            /^#FREETOON#/ { sub(/^#FREETOON#/, ""); print $0; next }
            { print $0 }
        ' "$INITTAB" > "$TMP_FILE"

        # If neither startqt nor qt-gui row exists, append standard fallback
        if ! grep -q 'startqt' "$TMP_FILE" 2>/dev/null && ! grep -q 'qt-gui' "$TMP_FILE" 2>/dev/null; then
            if [ -x /usr/bin/startqt ]; then
                echo 'qtqt:2345:respawn:/usr/bin/startqt >/dev/null 2>&1' >> "$TMP_FILE"
            elif [ -x /qmf/sbin/qt-gui ]; then
                echo 'qtqt:2345:respawn:/qmf/sbin/qt-gui -platform linuxfb -plugin Tslib >/dev/null 2>&1' >> "$TMP_FILE"
            fi
        fi

        commit_inittab
        ;;

    check)
        if validate_inittab "$INITTAB"; then
            echo "OK: /etc/inittab is valid ($(wc -l < "$INITTAB") lines)"
            exit 0
        else
            echo "FAIL: /etc/inittab is invalid!" >&2
            exit 1
        fi
        ;;

    restore_backup)
        acquire_lock
        if validate_inittab "$BAK_BACKUP"; then
            cp -f "$BAK_BACKUP" "$INITTAB"
            sync
            kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
            echo "Restored from $BAK_BACKUP"
            exit 0
        elif validate_inittab "$ORIG_BACKUP"; then
            cp -f "$ORIG_BACKUP" "$INITTAB"
            sync
            kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true
            echo "Restored from $ORIG_BACKUP"
            exit 0
        else
            echo "ERROR: no valid backup found to restore!" >&2
            exit 1
        fi
        ;;

    *)
        echo "Usage: $0 {upsert <id> <row>|drop <id>|comment_pattern <pat>|uncomment_freetoon|restore_stock_qt|check|restore_backup}" >&2
        exit 1
        ;;
esac
