#!/usr/bin/env bash

set -u
umask 077
export PATH="${HOME:-}/.local/bin:/usr/local/bin:/usr/bin:/bin:${PATH:-}"

RUNDIR="/dev/shm/afterflow-$(id -un)"
if [ ! -d /dev/shm ] || [ ! -w /dev/shm ]; then
    RUNDIR="/tmp/afterflow-$(id -un)"
fi
TRIGGER="$RUNDIR/open-kitty"
LAUNCHER="$RUNDIR/terminal.sh"
LOG="$RUNDIR/watch.log"
ENABLED="$RUNDIR/terminal-enabled"
LEGACY="/tmp/afterflow-$(id -un)"

mkdir -p "$RUNDIR" 2>/dev/null || true
chmod 700 "$RUNDIR" 2>/dev/null || true

# Old versions kept the terminal log on /tmp, which can be disk-backed.
# Remove those copies when the volatile runtime directory is available.
if [ "$RUNDIR" != "$LEGACY" ]; then
    rm -f "$LEGACY/terminal.log" "$LEGACY/watch.log" "$LEGACY/kitty.log" 2>/dev/null || true
fi

log_line() {
    local line
    line="$(date '+%H:%M:%S') $*"
    printf '%s\n' "$line" >>"$LOG" 2>/dev/null || true
    printf '%s\n' "$line" >&2
}

import_hyprland_env() {
    local comm pid entry
    for comm in /proc/[0-9]*/comm; do
        [ "$(cat "$comm" 2>/dev/null)" = "Hyprland" ] || continue
        pid="${comm#/proc/}"
        pid="${pid%/comm}"
        [ -r "/proc/$pid/environ" ] || continue
        while IFS= read -r -d '' entry; do
            case "$entry" in
                WAYLAND_DISPLAY=*|HYPRLAND_INSTANCE_SIGNATURE=*|XDG_RUNTIME_DIR=*|DISPLAY=*|XDG_SESSION_TYPE=*|XDG_CURRENT_DESKTOP=*|DBUS_SESSION_BUS_ADDRESS=*)
                    export "$entry"
                    ;;
            esac
        done < "/proc/$pid/environ"
        return 0
    done
    return 1
}

ensure_rule() {
    command -v hyprctl >/dev/null 2>&1 || return 0
    hyprctl eval 'hl.window_rule({name="afterflow-terminal", match={class="^(afterflow|afterflow-nocapture)$"}, float=true, size={"1000","700"}})' >/dev/null 2>&1 || true
}

terminal_enabled() {
    [ ! -r "$ENABLED" ] || [ "$(cat "$ENABLED" 2>/dev/null)" != "0" ]
}

stamp() {
    stat -c '%i:%y:%s' "$TRIGGER" 2>/dev/null || true
}

import_hyprland_env || true
ensure_rule

last="$(stamp)"
if [ -n "$last" ]; then
    log_line "ignoring pre-existing trigger"
fi

while true; do
    current="$(stamp)"
    if [ -n "$current" ] && [ "$current" != "$last" ]; then
        last="$current"
        rm -f "$TRIGGER" 2>/dev/null || true
        if ! terminal_enabled; then
            log_line "terminal disabled"
            continue
        fi
        if ! import_hyprland_env; then
            log_line "hyprland environment unavailable"
        fi
        if ! command -v kitty >/dev/null 2>&1; then
            log_line "kitty not found on path"
            continue
        fi
        log_line "opening kitty"
        ensure_rule
        if [ -r "$LAUNCHER" ]; then
            /bin/bash "$LAUNCHER" >>"$LOG" 2>&1 || true
        else
            log_line "launcher missing: $LAUNCHER"
        fi
    fi
    sleep 0.2
done
