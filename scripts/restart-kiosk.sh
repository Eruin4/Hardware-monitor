#!/bin/bash
pkill -fx '/home/eruin/project/rtk-hardware-monitor/hardware-dashboard' || true
sleep 2
if [ -S /run/user/1000/wayland-0 ]; then
  export WAYLAND_DISPLAY=wayland-0
elif [ -S /run/user/1000/wayland-1 ]; then
  export WAYLAND_DISPLAY=wayland-1
else
  export DISPLAY=:0
fi
export XDG_RUNTIME_DIR=/run/user/1000
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
nohup /home/eruin/project/rtk-hardware-monitor/hardware-dashboard \
  >/tmp/hardware-dashboard.log 2>&1 &
sleep 5
pgrep -afx '/home/eruin/project/rtk-hardware-monitor/hardware-dashboard' || echo "dashboard app not running"
tail -2 /tmp/hardware-dashboard.log
echo "done"
