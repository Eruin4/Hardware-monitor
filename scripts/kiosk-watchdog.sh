#!/usr/bin/env bash
set -u

readonly url=http://127.0.0.1:8088/health
readonly runtime_dir=/run/user/1000
readonly app=/home/eruin/project/rtk-hardware-monitor/hardware-dashboard
readonly app_pattern='/home/eruin/project/rtk-hardware-monitor/hardware-dashboard'
fail_count=0

launch_dashboard() {
    XDG_RUNTIME_DIR="$runtime_dir" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=${runtime_dir}/bus" \
        WAYLAND_DISPLAY=wayland-0 \
        DISPLAY=:0 \
        setsid -f "$app" >/tmp/hardware-dashboard.log 2>&1
}

while true; do
    if curl -fsS -m 3 "$url" >/dev/null 2>&1; then
        if (( fail_count >= 2 )); then
            logger -t kiosk-watchdog \
                "dashboard recovered after ${fail_count} failed probes -> restarting app"
            pkill -u eruin -fx "$app_pattern" 2>/dev/null || true
            sleep 2
        fi
        fail_count=0
        if ! pgrep -u eruin -fx "$app_pattern" >/dev/null 2>&1; then
            logger -t kiosk-watchdog "dashboard app not running -> launching"
            launch_dashboard
            sleep 5
        fi
    else
        fail_count=$((fail_count + 1))
    fi
    sleep 30
done
