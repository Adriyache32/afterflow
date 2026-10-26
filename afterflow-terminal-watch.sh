#!/usr/bin/env bash

set -u
umask 077

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

ensure_rule() {
    command -v hyprctl >/dev/null 2>&1 || return 0
    hyprctl eval 'hl.window_rule({name="afterflow-terminal", match={class="^(afterflow)$"}, float=true, size={"1000","700"}})' >/dev/null 2>&1 || true
}

terminal_enabled() {
    [ ! -r "$ENABLED" ] || [ "$(cat "$ENABLED" 2>/dev/null)" != "0" ]
}

stamp() {
    stat -c '%i:%y:%s' "$TRIGGER" 2>/dev/null || true
}

ensure_rule

last="$(stamp)"
if [ -n "$last" ]; then
    printf '%s ignoring pre-existing trigger\n' "$(date '+%H:%M:%S')" >>"$LOG"
fi

while true; do
    current="$(stamp)"
    if [ -n "$current" ] && [ "$current" != "$last" ]; then
        last="$current"
        rm -f "$TRIGGER" 2>/dev/null || true
        if ! terminal_enabled; then
            printf '%s terminal disabled\n' "$(date '+%H:%M:%S')" >>"$LOG"
            continue
        fi
        printf '%s opening kitty\n' "$(date '+%H:%M:%S')" >>"$LOG"
        ensure_rule
        if [ -r "$LAUNCHER" ]; then
            /bin/bash "$LAUNCHER" >>"$LOG" 2>&1 || true
        else
            printf '%s launcher missing: %s\n' "$(date '+%H:%M:%S')" "$LAUNCHER" >>"$LOG"
        fi
    fi
    sleep 0.2
done
