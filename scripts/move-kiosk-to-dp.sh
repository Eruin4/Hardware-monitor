#!/bin/bash
set -Eeuo pipefail

readonly dashboard_url=http://127.0.0.1:8088/

export XDG_RUNTIME_DIR=/run/user/1000
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
if [[ -S /run/user/1000/wayland-0 ]]; then
    export WAYLAND_DISPLAY=wayland-0
elif [[ -S /run/user/1000/wayland-1 ]]; then
    export WAYLAND_DISPLAY=wayland-1
fi

restore_watchdog() {
    systemctl --user start eruin-kiosk-watchdog.service 2>/dev/null || true
}
trap restore_watchdog EXIT

systemctl --user stop eruin-kiosk-watchdog.service 2>/dev/null || true
pkill -f 'firefox.*127.0.0.1:8088' 2>/dev/null || true
sleep 2

gdctl set -P -l logical \
    -L -s 1.0 -M DP-1 -m 1920x1200@59.957 -t 90 \
    -L -s 2.0 -M HDMI-1 -m 3840x2400@29.999 -p --right-of DP-1

nohup firefox --kiosk --private-window "$dashboard_url" \
    >/tmp/kiosk-firefox.log 2>&1 &

for _ in {1..15}; do
    if pgrep -f 'firefox.*127.0.0.1:8088' >/dev/null; then
        break
    fi
    sleep 1
done
sleep 4

pgrep -af 'firefox.*127.0.0.1:8088'
gdctl show
