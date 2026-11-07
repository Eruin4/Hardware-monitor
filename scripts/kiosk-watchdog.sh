#!/usr/bin/env bash
set -Eeuo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/kiosk-common.sh"

readonly url=http://127.0.0.1:8088/health
fail_count=0
exec 8>"$kiosk_dir/watchdog.lock"
flock -n 8 || exit 0

while true; do
    if curl -fsS -m 3 "$url" >/dev/null 2>&1; then
        exec 9>"$kiosk_dir/launch.lock"
        flock -x 9
        if (( fail_count >= 2 )); then
            logger -t kiosk-watchdog \
                "dashboard recovered after ${fail_count} failed probes -> restarting app" || true
            if ! stop_dashboard; then
                flock -u 9
                sleep 30
                continue
            fi
        fi
        fail_count=0
        if ! dashboard_running; then
            logger -t kiosk-watchdog "dashboard app not running -> launching" || true
            launch_dashboard || true
        fi
        flock -u 9
    else
        fail_count=$((fail_count + 1))
    fi
    sleep 30
done
