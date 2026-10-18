#!/bin/sh
# Installs audiolaz.driver ($1) into /Library/Audio/Plug-Ins/HAL and restarts coreaudiod. Run as root: the app
set -e
D=/Library/Audio/Plug-Ins/HAL
if [ "$1" = "--remove" ]; then
  rm -rf "$D/audiolaz.driver"
else
  [ -d "$1" ] || { echo "usage: $0 <audiolaz.driver> | --remove" >&2; exit 2; }
  mkdir -p "$D"
  rm -rf "$D/audiolaz.driver"
  cp -R "$1" "$D/audiolaz.driver"
  chown -R root:wheel "$D/audiolaz.driver"
  chmod -R go-w "$D/audiolaz.driver"
fi
killall coreaudiod 2>/dev/null || launchctl kickstart -k system/com.apple.audio.coreaudiod
