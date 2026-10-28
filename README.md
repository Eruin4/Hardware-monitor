# RTK native hardware monitor

A C / GTK 3 / Cairo telemetry display for the dedicated 1200×1920 monitor.
The original Python implementation remains available as `hardware-dashboard.py`.

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
Both checks and preview generation work without a graphical display. Pango
handles Unicode, Korean text, and ellipsis; install `fonts-noto-cjk` if needed.

Run in a graphical session:

```sh
./hardware-dashboard
```

## Runtime

- Asynchronous HTTP fetch from `http://127.0.0.1:8088/status.json` every 3 seconds.
- Only one request at a time, a 2-second socket timeout, and a 2 MiB response cap.
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

Built as `/home/eruin/project/rtk-hardware-monitor/hardware-dashboard`. Autostart,
restart, and watchdog launchers execute this native binary directly. Process matching is
exact so restarting the dashboard does not match unrelated shell commands.
The `eruin-kiosk-watchdog.service` user service owns the persistent launch.

Validation: compiler warnings enabled, parsing/rendering self-tests,
AddressSanitizer/UndefinedBehaviorSanitizer (third-party leak detection disabled),
and an asynchronous fetch and render against the live status endpoint.
The rendered preview was also checked outside the sandbox. Text uses Pango
outlines to avoid glyph-cache artifacts observed in the host environment.

The original Python implementation remains in this project. Current launcher scripts
are in `scripts/`; the previous launchers are preserved in `deployment-backup/`.
The earlier WebKit version is also preserved there. CPU and power savings have not
been benchmarked.
