#!/bin/sh
# $1 (default engine/mac/build), signed with $AUDIOLAZ_SIGN_ID (hardened runtime, timestamp: notarizable) or ad hoc.
# Installed by install-driver.sh (root: the app asks the administrator's password) into /Library/Audio/Plug-Ins/HAL.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/../build}
V=${AUDIOLAZ_VERSION:-1.0}
B="$OUT/audiolaz.driver"
rm -rf "$B"; mkdir -p "$B/Contents/MacOS"
sed "s/VERSION/$V/g" "$HERE/Info.plist" > "$B/Contents/Info.plist"
clang -O2 -Wall -Wextra -std=c11 -bundle -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 -fvisibility=hidden \
  -framework CoreAudio -framework CoreFoundation -o "$B/Contents/MacOS/audiolaz" "$HERE/audiolaz_driver.c"
if [ -n "$AUDIOLAZ_SIGN_ID" ]; then
  codesign --force --options runtime --timestamp --sign "$AUDIOLAZ_SIGN_ID" "$B"
else
  codesign --force --sign - "$B"
fi
echo "$B"
