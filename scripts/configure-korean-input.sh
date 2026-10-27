#!/usr/bin/env bash
set -Eeuo pipefail

export XDG_RUNTIME_DIR=/run/user/1000
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
export DISPLAY=:0
if [[ -S /run/user/1000/wayland-0 ]]; then
    export WAYLAND_DISPLAY=wayland-0
fi

gsettings set org.gnome.desktop.input-sources sources \
    "[('xkb', 'us'), ('ibus', 'hangul')]"
gsettings set org.gnome.desktop.wm.keybindings switch-input-source \
    "['<Super>space', 'Hangul', 'XF86Keyboard']"
gsettings set org.gnome.desktop.wm.keybindings switch-input-source-backward \
    "['<Shift><Super>space']"

gsettings set org.freedesktop.ibus.engine.hangul hangul-keyboard '2'
gsettings set org.freedesktop.ibus.engine.hangul switch-keys \
    'Hangul,Shift+space'
gsettings set org.freedesktop.ibus.engine.hangul initial-input-mode 'latin'

gsettings set org.gnome.desktop.interface font-name 'Noto Sans CJK KR 11'
gsettings set org.gnome.desktop.interface document-font-name 'Noto Sans CJK KR 11'
gsettings set org.gnome.desktop.interface monospace-font-name \
    'Noto Sans Mono CJK KR 11'

ibus-daemon --daemonize --replace --xim --panel disable
sleep 2

echo 'input sources:'
gsettings get org.gnome.desktop.input-sources sources
echo 'switch keys:'
gsettings get org.gnome.desktop.wm.keybindings switch-input-source
echo 'fonts:'
gsettings get org.gnome.desktop.interface font-name
gsettings get org.gnome.desktop.interface monospace-font-name
