#!/usr/bin/env bash
# Shared by the native launchers; source this file, do not execute it.
umask 077
readonly runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
readonly kiosk_dir="$runtime_dir/rtk-hardware-monitor"
readonly app="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)/hardware-dashboard"
readonly app_pattern="$(printf '%s' "$app" | sed 's/[][(){}.^$*+?|\\]/\\&/g')"
readonly log="$kiosk_dir/hardware-dashboard.log"
readonly app_uid="$(id -u)"

private_directory() {
    [[ -d "$1" && ! -L "$1" && -O "$1" ]] &&
        (( (8#$(stat -c %a -- "$1") & 077) == 0 ))
}
if ! private_directory "$runtime_dir"; then
    echo "A private, owned XDG_RUNTIME_DIR is required: $runtime_dir" >&2
    exit 1
fi
mkdir -m 700 -p -- "$kiosk_dir"
if ! private_directory "$kiosk_dir" || [[ -L "$log" ]] ||
    [[ -e "$log" && ( ! -f "$log" || ! -O "$log" ) ]]; then
    echo "Unsafe kiosk state directory or log: $kiosk_dir" >&2
    exit 1
fi
if [[ ! -x "$app" ]]; then
    echo "Dashboard executable not found: $app" >&2
    exit 1
fi
: >>"$log"
chmod 600 -- "$log"
export XDG_RUNTIME_DIR="$runtime_dir"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=${runtime_dir}/bus}"
export DISPLAY="${DISPLAY:-:0}"
if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
    for display in wayland-0 wayland-1; do
        if [[ -S "$runtime_dir/$display" ]]; then
            export WAYLAND_DISPLAY="$display"
            break
        fi
    done
fi

dashboard_running() {
    local result=0
    pgrep -u "$app_uid" -fx "$app_pattern" >/dev/null || result=$?
    if (( result > 1 )); then
        echo "Cannot check dashboard process (pgrep exit $result)" >&2
        exit "$result"
    fi
    return "$result"
}
stop_dashboard() {
    local result=0
    pkill -u "$app_uid" -fx "$app_pattern" 2>/dev/null || result=$?
    (( result <= 1 )) || return "$result"
    sleep 2
    if dashboard_running; then
        echo "Dashboard did not stop" >&2
        return 1
    fi
}
launch_dashboard() {
    # Do not let the child retain either launcher's flock descriptors.
    setsid -f "$app" >"$log" 2>&1 8>&- 9>&- || return
    sleep 5
    if ! dashboard_running; then
        echo "Dashboard app not running; see $log" >&2
        return 1
    fi
}
