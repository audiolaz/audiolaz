#!/bin/sh
# the player's netout.c (kind audiolaz) to audiolaz-recv --null at 44.1 / 96 / 192 / 384 / 768 kHz: every frame
# compared with what the receiver took (bit-exact), the receiver's clock 300 ppm off and followed by the player; once
# more at 192 kHz with every core busy. Then mDNS discovery on this machine, and the receiver built for Linux (ALSA)
# when gcc and alsa-lib are here.   sh tests/anet-loop.sh
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d "${TMPDIR:-/tmp}/anet-XXXXXX")
W="-std=gnu11 -O2 -Wall -Wextra -Wno-missing-field-initializers"
cc $W -I src/player -o "$T/proto" tests/anet-proto.c src/player/anet_mdns.c -lpthread
"$T/proto" | tail -1
cc $W -DANET_NULL_ONLY -o "$T/recv" src/player/audiolaz-recv.c src/player/anet_mdns.c -lpthread
cc $W -o "$T/loop" tests/anet-loop.c src/player/netout.c -lpthread -lm
PORT=$((24000 + $$ % 1000))
fail=0
run() {   # rate seconds [the null DAC's buffer run dry every N s]
  rm -f "$T/dump.raw"
  AUDIOLAZ_RECV_TEST_XRUN=${3:-0} "$T/recv" --null --null-ppm 300 --no-mdns --rt 0 --stats --port $PORT --once --dump "$T/dump.raw" 2> "$T/recv.log" &
  RP=$!
  sleep 0.3
  "$T/loop" "audiolaz:127.0.0.1:$PORT" "$1" "$2" "$T/sent.raw" 2> "$T/loop.log" | tail -1
  wait $RP || true
  if cmp -s "$T/sent.raw" "$T/dump.raw"; then echo "  bit-exact: $(wc -c < "$T/sent.raw" | tr -d ' ') bytes"; grep "stats:" "$T/recv.log" | sed "s/^audiolaz-recv: stats: /    /"
  else echo "  NOT BIT-EXACT"; tail -5 "$T/recv.log"; tail -5 "$T/loop.log"; fail=1; fi
}
for r in 44100 96000 192000 384000 768000; do run $r 8; done
N=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
echo "192 kHz with $N busy loops:"
PIDS=""; i=0
while [ $i -lt "$N" ]; do sh -c 'while :; do :; done' & PIDS="$PIDS $!"; i=$((i+1)); done
run 192000 10
kill $PIDS 2>/dev/null || true
# there; more than 5 in 10 s, the stream lost as for a DAC gone
echo "96 kHz, the DAC's buffer run dry every 2 s:"
run 96000 8 2
grep -q "run dry [1-9]" "$T/recv.log" && echo "  made ready again: $(grep -o 'run dry [0-9]*' "$T/recv.log")" || { echo "  the DAC's recovery NOT COUNTED"; fail=1; }
echo "96 kHz, run dry every 0.5 s (more than 5 in 10 s):"
rm -f "$T/dump.raw"
AUDIOLAZ_RECV_TEST_XRUN=0.5 "$T/recv" --null --no-mdns --rt 0 --stats --port $PORT --once --dump "$T/dump.raw" 2> "$T/recv.log" &
RP=$!
sleep 0.3
"$T/loop" "audiolaz:127.0.0.1:$PORT" 96000 8 "$T/sent.raw" > "$T/loop.out" 2> "$T/loop.log" || true
wait $RP || true
if grep -q "DAC failed" "$T/loop.out"; then echo "  the stream lost, as a DAC gone: $(grep -o 'run dry [0-9]*' "$T/recv.log") made ready first"
else echo "  NOT LOST"; tail -4 "$T/recv.log"; tail -2 "$T/loop.out"; fail=1; fi
echo "mDNS:"
"$T/recv" --null --rt 0 --port $PORT --id anet-test-$$ --name "Test DAC" 2> "$T/mdns.log" &
RP=$!
cat > "$T/browse.c" <<'C'
#include <stdio.h>
#include "anet.h"
int main(void) { anet_peer p[8]; int n = anet_mdns_browse(p, 8, 2000), ok = 0;
  for (int i = 0; i < n; i++) { printf("  found %s « %s » %s:%d up to %u Hz\n", p[i].id, p[i].dac, p[i].addr, p[i].port, p[i].max_rate); ok |= p[i].dac[0] == 'T'; }
  return !ok; }
C
cc $W -I src/player -o "$T/browse" "$T/browse.c" src/player/anet_mdns.c -lpthread
sleep 0.5
if "$T/browse"; then echo "  discovery ok"; else echo "  discovery FAILED"; fail=1; fi
kill $RP 2>/dev/null || true
if command -v gcc >/dev/null && [ -f /usr/include/alsa/asoundlib.h ]; then
  gcc $W -o "$T/audiolaz-recv" src/player/audiolaz-recv.c src/player/anet_mdns.c src/player/out_alsa.c -lasound -lpthread && echo "Linux ALSA build ok"
fi
rm -rf "$T"
[ $fail = 0 ] && echo "ANET LOOP OK" || { echo "ANET LOOP FAILED"; exit 1; }
