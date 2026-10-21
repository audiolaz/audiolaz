#!/bin/sh
# The unit tests, on Linux or macOS, no sound card touched: the PCM path of the Squeezebox input, the clock and the
# 32-bit conversions, AirPlay's metadata, the bit-perfect check, the speakers' channels, audiolaz Link's protocol and
# its loop end to end (a sender and a receiver on this machine, the receiver's clock its own). Then, as root on Linux: sudo sh tests/player-intact.sh
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d "${TMPDIR:-/tmp}/audiolaz-tests.XXXXXX")
trap 'rm -rf "$T"' EXIT
W="-std=gnu11 -O2 -Wall -Wextra -Wno-missing-field-initializers -D_DEFAULT_SOURCE -DROOM_PORTABLE -DROOM_PLAYER_ONLY -DROOM_NO_UPSAMPLER"
P=src/player
ok=0
step() { printf '%-28s' "$1"; shift; if out=$("$@" 2>&1); then echo "$(printf '%s\n' "$out" | tail -1)"; else echo "FAILED"; printf '%s\n' "$out" | tail -8; ok=1; fi; }
cc $W -o "$T/pcmclock" $P/test_pcmclock.c -lm && step "pcmclock.h" "$T/pcmclock"
cc $W -o "$T/airmeta" $P/test_airmeta.c && step "airmeta.h" "$T/airmeta"
cc $W -o "$T/bitperfect" tests/bitperfect.c && step "bitperfect.h" "$T/bitperfect"
cc $W -o "$T/channels" $P/test_channels.c -lm && step "channels.h" "$T/channels"
if [ "$(uname -s)" = Linux ]; then
  cc $W -o "$T/pcm" $P/test_pcm.c -lm -lpthread -ldl && step "the Squeezebox input (PCM)" "$T/pcm"
fi
step "audiolaz Link" sh tests/anet-loop.sh
[ $ok = 0 ] && echo "TESTS OK" || { echo "TESTS FAILED"; exit 1; }
