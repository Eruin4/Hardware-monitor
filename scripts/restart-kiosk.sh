#!/bin/bash
set -Eeuo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/kiosk-common.sh"

exec 9>"$kiosk_dir/launch.lock"
flock -x 9
stop_dashboard
launch_dashboard
tail -2 "$log"
echo "done"
