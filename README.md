# RTK native hardware monitor

A C / GTK 3 / Cairo telemetry display for the dedicated 1200×1920 monitor.

The display uses a monochrome canvas and two identical 610-point host cards.
Both cards share the same renderer, spacing, headings, type scale and order:

- Compact machine name and connection state.
- CPU utilization, memory utilization and CPU temperature, each with a large
  number and history trace.
- Device-specific details: GPU utilization, temperature and power on the PC;
  1/5/15-minute load averages on the server.
- Memory capacity and usage bar, followed by download and upload rates with
  separate history traces. Agent version or uptime sits under memory capacity.
  Network rates use decimal Mbps/Kbps, converted from feed bytes/second (×8).

Room temperature and humidity live in a separate environment strip. The bottom
panel contains up to three filesystems and twelve temperature sensors, sorted
by name; overflow reports `+N more`. Missing values show an em dash. A lost feed
retains the snapshot and labels the cards stale; current network readings are
withheld when the associated feed is unavailable.

Text and graphs use only grayscale. Threshold crossings brighten readings to
white; short ticks mark the thresholds on history plots. Thresholds remain
90% memory, 85°C PC CPU, 80°C GPU, 75°C server temperature and 90% disk usage.
The top masthead and machine titles stay compact so telemetry gets the space.

## Build

On Ubuntu, install the compiler and development libraries:

```sh
sudo apt-get install -y build-essential pkg-config libgtk-3-dev libsoup-3.0-dev libjson-glib-dev
cd /home/eruin/project/rtk-hardware-monitor
make
make check
make preview
```

`preview-native.png` uses sample data and is rendered by the actual C renderer.
The checks and preview generation work without a graphical display. Pango
handles Unicode, Korean text, and ellipsis; install `fonts-noto-cjk` if needed.

`make check` runs a syntax-only C compilation, shell checks, launcher regression
checks, and the headless C self-test. `make check-syntax` only needs the compiler
and development headers; `make check-shell` needs Bash and Python 3, with no GTK
or display. Launcher tests mock process and desktop commands and make no network
requests.

Run in a graphical session:

```sh
./hardware-dashboard
```

## Runtime

- Asynchronous HTTP fetch from `http://127.0.0.1:8088/status.json` every 3 seconds.
- Only one request at a time, a 2-second socket timeout, a 5-second total request
  deadline, and a 2 MiB response cap. Timed-out requests retain the last snapshot
  and the next refresh can retry.
- Repaints when the JSON or connection state changes, or GTK requests a redraw.
- Keeps the last valid snapshot on HTTP, transport, or JSON errors.
- Displays up to 90 history samples, three filesystems, and twelve temperature
  sensors. No browser, animation loop, Python runtime, or image assets.
- Preserves the title `Eruin Hardware Dashboard` and WM class
  `eruin-hardware-dashboard` / `EruinHardwareDashboard` for monitor placement.
- Uses a borderless maximized window instead of the fullscreen hint, retaining
  the existing workaround for Mutter direct-scanout transitions.
- Supports SIGINT/SIGTERM with cancellation and draining of pending requests.

## Deployment status

The native launchers are `scripts/restart-kiosk.sh` and
`scripts/kiosk-watchdog.sh`. They find the binary beside the `scripts/` directory
and match its literal command line for the current user. The watchdog uses
`curl` to probe the local feed and only launches when it is healthy. It retries
failed launches and restarts after recovery from two or more failed probes.

Run the launchers as the desktop user. They use the current `XDG_RUNTIME_DIR`
(default `/run/user/$(id -u)`) and preserve supplied display/session variables,
or detect `wayland-0` / `wayland-1`. The runtime directory must be owned by the
user with no group/other access. Logs and lock files live in its private
`rtk-hardware-monitor/` subdirectory, rather than `/tmp`; each launch replaces
`hardware-dashboard.log`. `flock` (util-linux) prevents duplicate watchdogs and
serializes manual restarts with watchdog actions. A restart returns failure if
the executable is missing, the old process does not stop, or the new one exits.
The `eruin-kiosk-watchdog.service` user service is expected to own the watchdog;
its unit is not included here.

`scripts/move-kiosk-to-dp.sh` is a legacy Firefox launcher with hard-coded monitor
and account settings. It does not place the native dashboard; do not use it for
native deployment. `scripts/configure-korean-input.sh` changes the original
account's desktop input/font settings and is also host-specific. Neither script
is part of the native launch path. Earlier Python/WebKit implementations and
backups mentioned in older documentation are not included in this source tree.
CPU and power savings have not been benchmarked.
