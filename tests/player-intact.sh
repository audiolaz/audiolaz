#!/bin/sh
# The player end to end, on Linux, as root (a network namespace of its own: nothing reaches your network), no sound
# card touched: a fake Roon (tests/fake-roon-squeeze.py) streams 12 s of 24-bit PCM to the player, whose DAC is ALSA's
# file plugin (paced to the clock by tests/alsa-faults.c):
#   intact  every frame the "card" got is the fake Roon's, bit for bit; /state says bitPerfect true and the sound path;
#   rate    the same through an ALSA plug that plays the card at 48 kHz: bitPerfect false, why ["rate"], deviceRate 48000;
#   free    a change of the sound asked of the free player is refused (403 {"why":"room"}); its page is its own.
# Needs gcc, alsa-lib's headers, python3, curl, unshare (util-linux).   sudo sh tests/player-intact.sh   KEEP=<dir>: the logs
set -u
cd "$(dirname "$0")/.."
T=${KEEP:-$(mktemp -d /tmp/audiolaz-intact-XXXXXX)}
mkdir -p "$T"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$T"' EXIT
P=src/player
W="-std=gnu11 -O2 -Wall -Wextra -Wno-missing-field-initializers -D_DEFAULT_SOURCE -DROOM_PORTABLE -DROOM_PLAYER_ONLY -DROOM_NO_UPSAMPLER"
bad=0
ok() { echo "  ok    $*"; }
no() { echo "  FAIL  $*"; bad=1; }
gcc $W -o "$T/player" $P/roomd.c $P/squeeze.c $P/page.c $P/netout.c $P/out_alsa.c $P/src_loop.c $P/src_upnp.c $P/upnp.c $P/upnp_dec.c -lasound -lm -lpthread -ldl || exit 1
gcc -O2 -Wall -shared -fPIC -o "$T/faults.so" tests/alsa-faults.c -ldl -lasound || exit 1
FAKE=$(pwd)/tests/fake-roon-squeeze.py
cat > "$T/asound.conf" <<EOF
pcm.azcap { type file; slave.pcm "null"; file "$T/intact.raw"; format "raw" }
pcm.azfile48 { type file; slave.pcm "null"; file "$T/rate.raw"; format "raw" }
pcm.az48 { type plug; slave { pcm "azfile48"; rate 48000 } }
EOF
play() {   # $1 name, $2 the card, $3 seconds
  name=$1 dev=$2 secs=$3
  mkdir -p "$T/rooms-$name"
  unshare -n sh -c "
    ip link set lo up
    python3 '$FAKE' --seconds $secs --tracks 1 --prebuffer 30 --speed 4 --quiet --timeout $((secs + 40)) > '$T/$name.fake' 2>&1 &
    fake=\$!
    until grep -q 'fake Roon on' '$T/$name.fake' 2>/dev/null; do sleep 0.1; kill -0 \$fake 2>/dev/null || break; done
    ALSA_CONFIG_PATH=/usr/share/alsa/alsa.conf:$T/asound.conf LD_PRELOAD='$T/faults.so' '$T/player' --loop none --dac $dev \
      --rooms '$T/rooms-$name' --squeeze 127.0.0.1 --name test --rt 0 --writer-rt 0 --page 18765 --net off > '$T/$name.log' 2>&1 &
    eng=\$!
    until grep -q 'playing:' '$T/$name.log' 2>/dev/null; do sleep 0.1; kill -0 \$eng 2>/dev/null || break; done
    sleep $((secs / 2))
    curl -s http://127.0.0.1:18765/state > '$T/$name.state'
    for p in level?db=-3 image?pct=50 listen?to=1 limiter?on=0 dsd?m=64; do
      printf '%s %s\n' \"\$p\" \"\$(curl -s -o /dev/null -w '%{http_code}' -X POST \"http://127.0.0.1:18765/\$p\")\" >> '$T/$name.codes'
    done
    curl -s http://127.0.0.1:18765/ > '$T/$name.html'
    wait \$fake
    sleep 2
    kill -INT \$eng 2>/dev/null; wait"
}
play intact azcap 12
play rate az48 8
echo "intact"
st=$(cat "$T/intact.state" 2>/dev/null)
echo "$st" | grep -q '"bitPerfect":true' && ok "bitPerfect true ($(echo "$st" | grep -o '"bitPerfectSpans":[0-9]*'))" || no "bitPerfect: $(echo "$st" | grep -o '"bitPerfect[^]]*]')"
echo "$st" | grep -o '"soundPath":{[^}]*}' | sed 's/^/        /'
python3 - "$T/intact.raw" <<'PY' && ok "every frame the card got is the source's, bit for bit" || no "the card's frames are not the source's"
import math, struct, sys
raw = open(sys.argv[1], "rb").read()
n = len(raw) // 8
fr = struct.unpack("<%di" % (2 * n), raw[:8 * n])
R, f, amp = 44100, 440.0, 0.1 * (1 << 23)
def want(i):
    v = int(round(amp * math.sin(2 * math.pi * f * (i % R) / R))) & 0xffffff
    if v & 0x800000: v -= 1 << 24
    return v << 8
k = next((i for i in range(n) if fr[2 * i] or fr[2 * i + 1]), None)
if k is None: print("        nothing but silence"); sys.exit(1)
k -= 1
m = 0
for i in range(k, n):
    j = i - k
    if j >= 12 * R: break
    if fr[2 * i] != want(j) or fr[2 * i + 1] != want(j):
        if all(x == 0 for x in fr[2 * i:2 * i + 2000]): break
        print("        frame %d: %d %d, the source sent %d" % (j, fr[2 * i], fr[2 * i + 1], want(j))); sys.exit(1)
    m += 1
print("        %d frames compared (%.1f s)" % (m, m / R))
sys.exit(0 if m > 5 * R else 1)
PY
echo "rate"
st=$(cat "$T/rate.state" 2>/dev/null)
echo "$st" | grep -q '"bitPerfect":false' && echo "$st" | grep -q '"bitPerfectWhy":\["rate"\]' && ok 'bitPerfect false, why ["rate"]' || no "rate: $(echo "$st" | grep -o '"bitPerfect[^]]*]')"
echo "$st" | grep -q '"deviceRate":48000' && ok 'deviceRate 48000' || no "deviceRate: $(echo "$st" | grep -o '"soundPath":{[^}]*}')"
echo "free"
[ "$(grep -cv ' 403$' "$T/intact.codes" 2>/dev/null)" = 0 ] && ok "403 to every change of the sound ($(cut -d'?' -f1 "$T/intact.codes" | tr '\n' ' '))" || no "not refused: $(grep -v ' 403$' "$T/intact.codes" | tr '\n' ' ')"
grep -qiE 'audiolaz player|Lecteur audiolaz' "$T/intact.html" && ok "the player's own page" || no 'the page'
if [ $bad = 0 ]; then echo "PLAYER INTACT OK"; else echo "PLAYER INTACT FAIL (KEEP=<dir> keeps the logs)"; fi
exit $bad
