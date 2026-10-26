#!/bin/sh
# The player end to end, on Linux, as root (a network namespace of its own: nothing reaches your network), no sound
# card touched: a fake Roon (tests/fake-roon-squeeze.py) streams 12 s of 24-bit PCM to the player, whose DAC is ALSA's
# file plugin (paced to the clock by tests/alsa-faults.c):
#   intact  every frame the "card" got is the fake Roon's, bit for bit; /state says bitPerfect true and the sound path;
#   rate    the same through an ALSA plug that plays the card at 48 kHz: bitPerfect false, why ["rate"], deviceRate 48000;
#   free    a change of the sound asked of the free player is refused (403 {"why":"room"}); its page is its own;
#   channels  left and right swapped and the polarity of the output's right channel inverted (the control's
#           swap.default=1, invert_r.default=1; a source whose channels differ): the swap on the music first, then the
#           output's right word negated: the card's left is the source's right, its right the source's left negated,
#           every frame exact; /state bitPerfect false, why swap and polarity, bitPerfectLossless true, outChannels;
#           POST /channels accepted by the free player (200), written to the control file.
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
pcm.azch { type file; slave.pcm "null"; file "$T/channels.raw"; format "raw" }
EOF
play() {   # $1 name, $2 the card, $3 seconds, $4 the fake Roon's own options, $5 the control file's lines to start with
  name=$1 dev=$2 secs=$3 fopt=${4:-}
  mkdir -p "$T/rooms-$name"
  [ -n "${5:-}" ] && printf '%b' "$5" > "$T/rooms-$name/control"
  unshare -n sh -c "
    ip link set lo up
    python3 '$FAKE' --seconds $secs --tracks 1 --prebuffer 30 --speed 4 --quiet $fopt --timeout $((secs + 40)) > '$T/$name.fake' 2>&1 &
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
    if [ $name = channels ]; then   # the left speaker's polarity as it is (invert_l=0): the sound unchanged, the file rewritten
      curl -s -w ' %{http_code}' -X POST 'http://127.0.0.1:18765/channels?invert_l=0' > '$T/$name.post'
      cp '$T/rooms-$name/control' '$T/$name.control'
    fi
    wait \$fake
    sleep 2
    kill -INT \$eng 2>/dev/null; wait"
}
play intact azcap 12
play rate az48 8
play channels azch 12 '--right-tone 660' 'swap.default=1\ninvert_r.default=1\nchannels_order=2\n'
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
echo "channels"
st=$(cat "$T/channels.state" 2>/dev/null)
echo "$st" | grep -q '"bitPerfect":false' && echo "$st" | grep -q '"bitPerfectWhy":\["swap","polarity"\]' && echo "$st" | grep -q '"bitPerfectLossless":true' \
  && ok 'bitPerfect false, why ["swap","polarity"], lossless' || no "channels: $(echo "$st" | grep -o '"bitPerfect[^}]*' | head -c 300)"
echo "$st" | grep -q '"outChannels":{"swap":true,"invertL":false,"invertR":true,"now":5}' && ok 'outChannels: swapped, the right inverted, now 5' || no "outChannels: $(echo "$st" | grep -o '"outChannels":{[^}]*}')"
grep -q '"ok":true.* 200$' "$T/channels.post" 2>/dev/null && grep -qx 'swap.default=1' "$T/channels.control" && grep -qx 'invert_r.default=1' "$T/channels.control" && ! grep -q '^invert_l' "$T/channels.control" \
  && ok 'POST /channels: 200 from the free player, the control file kept' || no "POST /channels: $(cat "$T/channels.post" 2>/dev/null) / $(tr '\n' ' ' < "$T/channels.control" 2>/dev/null)"
python3 - "$T/channels.raw" <<'PY' && ok "the card's left: the source's right, its right: the source's left negated, bit for bit" || no "the swapped and inverted frames"
import math, struct, sys
raw = open(sys.argv[1], "rb").read()
n = len(raw) // 8
fr = struct.unpack("<%di" % (2 * n), raw[:8 * n])
R = 44100
def tone(i, f, amp):
    v = int(round(amp * math.sin(2 * math.pi * f * (i % R) / R))) & 0xffffff
    if v & 0x800000: v -= 1 << 24
    return v << 8
def neg(x): return 0x7FFFFF00 if x == -(1 << 31) else -x
k = next((i for i in range(n) if fr[2 * i] or fr[2 * i + 1]), None)
if k is None: print("        nothing but silence"); sys.exit(1)
k -= 1   # the first frame of both tones is 0
m = 0
for i in range(k, n):
    j = i - k
    if j >= 12 * R: break
    l, r = tone(j, 440.0, 0.1 * (1 << 23)), tone(j, 660.0, 0.05 * (1 << 23))
    if fr[2 * i] != r or fr[2 * i + 1] != neg(l):
        if all(x == 0 for x in fr[2 * i:2 * i + 2000]): break
        print("        frame %d: %d %d, the source sent %d %d" % (j, fr[2 * i], fr[2 * i + 1], l, r)); sys.exit(1)
    m += 1
print("        %d frames compared (%.1f s)" % (m, m / R))
sys.exit(0 if m > 5 * R else 1)
PY
if [ $bad = 0 ]; then echo "PLAYER INTACT OK"; else echo "PLAYER INTACT FAIL (KEEP=<dir> keeps the logs)"; fi
exit $bad
