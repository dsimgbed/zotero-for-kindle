#!/bin/sh
# Name: Grimoire x Zotero
# Author: grimoire-zotero-bridge
# Icon: /mnt/us/zotero/share/icon.png
# DontUseFBInk
#
# Universal Grimoire <-> Zotero trampoline scriptlet for Kindle Scribe (Vera / SH_Integration) & desktop tests.
# POSIX sh (BusyBox ash compatible). No bashisms, no arrays, no eval/source/sh -c.

# --- configuration (overridable) ---
GZ_BRIDGE_DIR=${GZ_BRIDGE_DIR:-/mnt/us/grimoire_zotero_bridge}
GZ_LOCK_DIR=${GZ_LOCK_DIR:-/tmp/grimoire_zotero_trampoline.lock}
GZ_RAPID_THRESHOLD_SEC=${GZ_RAPID_THRESHOLD_SEC:-3}
GZ_MAX_CRASHES=${GZ_MAX_CRASHES:-2}
GZ_MAX_RAPID_BOUNCES=${GZ_MAX_RAPID_BOUNCES:-4}
GZ_X_WAIT_SEC=${GZ_X_WAIT_SEC:-10}

# --- Kindle detection ---
if [ -z "${GZ_IS_KINDLE:-}" ]; then
    GZ_IS_KINDLE=0
    if [ -f /etc/prettyversion.txt ]; then
        GZ_IS_KINDLE=1
    elif command -v lipc-set-prop >/dev/null 2>&1; then
        GZ_IS_KINDLE=1
    fi
fi
case "$GZ_IS_KINDLE" in
    0|1) ;;
    *) GZ_IS_KINDLE=0 ;;
esac

# Detach when tapped as a Kindle scriptlet (SH_Integration) so the Library
# finishes cleanly and the trampoline survives the scriptlet host exiting.
_detach=0
if [ "$GZ_IS_KINDLE" = 1 ] && [ "${GZ_DETACH:-1}" != 0 ]; then
    _detach=1
    for _arg in "$@"; do
        case "$_arg" in
            --detached|--foreground) _detach=0 ;;
        esac
    done
fi
if [ "$_detach" = 1 ]; then
    if command -v setsid >/dev/null 2>&1; then
        setsid sh "$0" --detached "$@" </dev/null >/dev/null 2>&1 &
    else
        nohup sh "$0" --detached "$@" </dev/null >/dev/null 2>&1 &
    fi
    exit 0
fi

if [ -z "${GZ_SWITCH_SETTLE_SEC:-}" ]; then
    if [ "$GZ_IS_KINDLE" = 1 ]; then
        GZ_SWITCH_SETTLE_SEC=1
    else
        GZ_SWITCH_SETTLE_SEC=0
    fi
fi

# --- auto-detect app commands ---
if [ -z "${GZ_GRIMOIRE_CMD:-}" ]; then
    # Prefer side-by-side isolated "Grimoire (Bridge)" install if present so it never touches official Grimoire.
    if [ -f "/mnt/us/documents/Grimoire (Bridge).sh" ]; then
        sh "/mnt/us/documents/Grimoire (Bridge).sh" --unpack-only >/dev/null 2>&1 || true
    fi
    if [ -f /mnt/us/grimoire-bridge/grimoire/app/grimoire.sh ]; then
        GZ_GRIMOIRE_CMD=/mnt/us/grimoire-bridge/grimoire/app/grimoire.sh
        GRIMOIRE_US=${GRIMOIRE_US:-/mnt/us/grimoire-bridge}
        GRIMOIRE_DEV=1
        export GRIMOIRE_US GRIMOIRE_DEV
    else
        # Standard install fallback: ensure /mnt/us/grimoire/app is unpacked/up-to-date.
        if [ -f /mnt/us/documents/Grimoire.sh ]; then
            sh /mnt/us/documents/Grimoire.sh --unpack-only >/dev/null 2>&1 || true
        fi
        for _c in /mnt/us/grimoire/app/grimoire.sh /mnt/us/grimoire/grimoire.sh /mnt/us/documents/Grimoire.sh; do
            if [ -f "$_c" ]; then GZ_GRIMOIRE_CMD=$_c; break; fi
        done
        [ -z "${GZ_GRIMOIRE_CMD:-}" ] && GZ_GRIMOIRE_CMD=/mnt/us/grimoire/app/grimoire.sh
    fi
fi
if [ -z "${GZ_ZOTERO_CMD:-}" ]; then
    # Prefer side-by-side isolated "Zotero (Bridge)" install if present so it never touches official Zotero.
    for _c in "/mnt/us/documents/Zotero (Bridge).sh" /mnt/us/documents/Zotero.sh /mnt/us/zotero/Zotero.sh /mnt/us/zotero/app/Zotero.sh; do
        if [ -f "$_c" ]; then GZ_ZOTERO_CMD=$_c; break; fi
    done
    [ -z "${GZ_ZOTERO_CMD:-}" ] && GZ_ZOTERO_CMD=/mnt/us/documents/Zotero.sh
fi
if [ -f /mnt/us/zotero-bridge/bin/zk-sync ] && [ ! -L /mnt/us/zotero-bridge/bin/zk-sync ]; then
    [ -z "${GRIMOIRE_ZK_SYNC_BIN:-}" ] && GRIMOIRE_ZK_SYNC_BIN=/mnt/us/zotero-bridge/bin/zk-sync
    [ -z "${ZK_HOME:-}" ] && ZK_HOME=/mnt/us/zotero-bridge
    [ -z "${ZK_CONFIG_DIR:-}" ] && ZK_CONFIG_DIR=/var/local/zotero-bridge
    export GRIMOIRE_ZK_SYNC_BIN ZK_HOME ZK_CONFIG_DIR
fi

# --- helpers ---
log() {
    echo "$(date '+%F %T') grimoire_zotero: $*" >> "$GZ_BRIDGE_DIR/bridge.log"
}

run_app() {
    _cmd=$1
    shift
    if [ -x "$_cmd" ]; then
        "$_cmd" "$@"
    else
        sh "$_cmd" "$@"
    fi
}

wait_for_x() {
    [ "$GZ_IS_KINDLE" = 1 ] || return 0
    _disp=${DISPLAY:-:0}
    DISPLAY="$_disp"
    export DISPLAY
    command -v xwininfo >/dev/null 2>&1 || return 0
    _waited=0
    while ! DISPLAY="$_disp" xwininfo -root >/dev/null 2>&1; do
        if [ "$_waited" -ge "${GZ_X_WAIT_SEC:-10}" ]; then
            log "warning: X display $_disp still unavailable after ${_waited}s"
            return 1
        fi
        _waited=$((_waited + 1))
        sleep 1
    done
    if [ "$_waited" -gt 0 ]; then
        log "X display $_disp ready after ${_waited}s"
    fi
    return 0
}

# Validate bridge dir is not a symlink; create if absent.
if [ -L "$GZ_BRIDGE_DIR" ]; then
    echo "GrimoireZotero: bridge dir is a symlink, refusing" >&2
    exit 1
fi
mkdir -p "$GZ_BRIDGE_DIR" 2>/dev/null || {
    echo "GrimoireZotero: cannot create bridge dir" >&2
    exit 1
}
if [ -L "$GZ_BRIDGE_DIR" ]; then
    echo "GrimoireZotero: bridge dir became a symlink, refusing" >&2
    exit 1
fi

# --- single-instance lock ---
acquire_lock() {
    while :; do
        if mkdir "$GZ_LOCK_DIR" 2>/dev/null; then
            echo $$ > "$GZ_LOCK_DIR/pid"
            return 0
        fi
        _pid=$(cat "$GZ_LOCK_DIR/pid" 2>/dev/null)
        case "$_pid" in
            ''|*[!0-9]*) rm -rf "$GZ_LOCK_DIR"; continue ;;
        esac
        if kill -0 "$_pid" 2>/dev/null; then
            return 1
        fi
        rm -rf "$GZ_LOCK_DIR"
    done
}

if ! acquire_lock; then
    echo "GrimoireZotero: another instance is running" >&2
    log "lock held by another instance; refusing second launch"
    exit 1
fi

cleanup() { rm -rf "$GZ_LOCK_DIR"; }
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

# --- handoff validation / cleanup ---
# Returns 0 if a valid handoff exists, 1 otherwise (and removes bad ones).
sanitize_handoff() {
    _f="$GZ_BRIDGE_DIR/handoff.json"
    [ -e "$_f" ] || [ -L "$_f" ] || return 1
    if [ -L "$_f" ]; then
        rm -f "$_f"
        log "removed symlink handoff.json (security)"
        return 1
    fi
    if [ ! -f "$_f" ]; then
        rm -f "$_f"
        return 1
    fi
    _size=$(wc -c < "$_f" 2>/dev/null | tr -d '[:space:]')
    case "$_size" in ''|*[!0-9]*) _size=0 ;; esac
    if [ "$_size" -gt 262144 ]; then
        rm -f "$_f"
        log "removed oversized handoff.json (${_size} bytes)"
        return 1
    fi
    if ! grep -Eq '"schema_version"[[:space:]]*:[[:space:]]*1' "$_f"; then
        rm -f "$_f"
        log "removed malformed handoff.json (missing schema_version)"
        return 1
    fi
    if ! grep -Eq '"target"[[:space:]]*:[[:space:]]*"(grimoire|zotero)"' "$_f"; then
        rm -f "$_f"
        log "removed malformed handoff.json (invalid target)"
        return 1
    fi
    return 0
}

handoff_targets_zotero() {
    _f="$GZ_BRIDGE_DIR/handoff.json"
    sanitize_handoff >/dev/null 2>&1 || return 1
    [ -f "$_f" ] || return 1
    grep -Eq '"target"[[:space:]]*:[[:space:]]*"zotero"' "$_f"
}

# --- CLI ---
_start=""
while [ $# -gt 0 ]; do
    case "$1" in
        --start)
            _start="$2"
            shift 2
            ;;
        --start=*)
            _start="${1#--start=}"
            shift
            ;;
        *)
            shift
            ;;
    esac
done

if [ -z "$_start" ]; then
    if handoff_targets_zotero; then
        _start=zotero
    elif [ "$GZ_IS_KINDLE" = 1 ] && [ ! -f "$GZ_BRIDGE_DIR/library_index.json" ]; then
        # First launch on Kindle: start in Zotero so it exports library_index.json
        # and lets you send your first citation straight into Grimoire.
        _start=zotero
    else
        _start=grimoire
    fi
fi
case "$_start" in
    grimoire|zotero) ;;
    *) _start=grimoire ;;
esac

current=$_start
crash_streak=0
rapid_bounces=0
zotero_restarts=0

trip_circuit_breaker() {
    rm -f "$GZ_BRIDGE_DIR/handoff.json"
    log "CIRCUIT_BREAKER: tripped (app=$current)"
    if [ "$GZ_IS_KINDLE" = 1 ]; then
        killall -CONT awesome 2>/dev/null
        killall -CONT volumd 2>/dev/null
        if command -v lipc-set-prop >/dev/null 2>&1; then
            lipc-set-prop com.lab126.pillow disableEnablePillow enable 2>/dev/null
        fi
        rm -f /tmp/zk-wet-ink-session
    fi
    exit 1
}

# Tell child launchers (Grimoire (Bridge).sh / Zotero (Bridge).sh) they are already
# supervised by the trampoline so they never re-exec Grimoire x Zotero.sh recursively.
GZ_UNDER_TRAMPOLINE=1
export GZ_UNDER_TRAMPOLINE

# initial sanitize
sanitize_handoff >/dev/null 2>&1 || true

while :; do
    t0=$(date +%s)
    if [ "$current" = grimoire ]; then
        run_app "$GZ_GRIMOIRE_CMD"
        rc=$?
    else
        wait_for_x || true
        if handoff_targets_zotero; then
            run_app "$GZ_ZOTERO_CMD" --handoff
        else
            run_app "$GZ_ZOTERO_CMD"
        fi
        rc=$?
    fi
    t1=$(date +%s)
    elapsed=$((t1 - t0))
    [ "$elapsed" -lt 0 ] && elapsed=0

    case "$rc" in
        0)
            log "clean exit app=$current duration=${elapsed}s"
            exit 0
            ;;
        42)
            # expected from grimoire
            log "handoff grimoire -> zotero duration=${elapsed}s"
            crash_streak=0
            if [ "$elapsed" -lt "$GZ_RAPID_THRESHOLD_SEC" ]; then
                rapid_bounces=$((rapid_bounces + 1))
            else
                rapid_bounces=0
            fi
            if [ "$rapid_bounces" -gt "$GZ_MAX_RAPID_BOUNCES" ]; then
                trip_circuit_breaker
            fi
            current=zotero
            if [ "$GZ_SWITCH_SETTLE_SEC" != 0 ]; then
                sleep "$GZ_SWITCH_SETTLE_SEC"
            fi
            continue
            ;;
        43)
            log "handoff zotero -> grimoire duration=${elapsed}s"
            crash_streak=0
            if [ "$elapsed" -lt "$GZ_RAPID_THRESHOLD_SEC" ]; then
                rapid_bounces=$((rapid_bounces + 1))
            else
                rapid_bounces=0
            fi
            if [ "$rapid_bounces" -gt "$GZ_MAX_RAPID_BOUNCES" ]; then
                trip_circuit_breaker
            fi
            current=grimoire
            if [ "$GZ_SWITCH_SETTLE_SEC" != 0 ]; then
                sleep "$GZ_SWITCH_SETTLE_SEC"
            fi
            continue
            ;;
        75)
            zotero_restarts=$((zotero_restarts + 1))
            if [ "$zotero_restarts" -gt 3 ]; then
                trip_circuit_breaker
            fi
            crash_streak=0
            current=zotero
            if [ "$GZ_SWITCH_SETTLE_SEC" != 0 ]; then
                sleep "$GZ_SWITCH_SETTLE_SEC"
            fi
            continue
            ;;
        *)
            if [ "$elapsed" -lt "$GZ_RAPID_THRESHOLD_SEC" ]; then
                crash_streak=$((crash_streak + 1))
            else
                crash_streak=1
            fi
            log "crash app=$current exit=$rc duration=${elapsed}s"
            rm -f "$GZ_BRIDGE_DIR/handoff.json"
            if [ "$crash_streak" -ge "$GZ_MAX_CRASHES" ] && [ "$elapsed" -lt "$GZ_RAPID_THRESHOLD_SEC" ]; then
                trip_circuit_breaker
            fi
            if [ "$elapsed" -ge "$GZ_RAPID_THRESHOLD_SEC" ]; then
                log "slow crash; exiting"
                exit 0
            fi
            # single rapid crash: retry same app once
            if [ "$crash_streak" -lt "$GZ_MAX_CRASHES" ]; then
                continue
            fi
            trip_circuit_breaker
            ;;
    esac
done