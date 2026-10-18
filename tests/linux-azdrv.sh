#!/bin/sh
#   docker run --rm -v "$PWD":/src -w /src [--platform linux/amd64] debian:bookworm sh tests/linux-azdrv.sh
# Builds the plugin and tests/linux-azdrv.c, then: bit for bit at 44.1/96/192/384/768 kHz in each format (aplay too),
# the app's pace following a fake DAC 300 ppm off the host (under CPU load), the host clock standing in with no reader,
set -e
command -v gcc >/dev/null && command -v aplay >/dev/null && [ -f /usr/include/alsa/pcm_ioplug.h ] || { apt-get update -qq >/dev/null; apt-get install -y -qq gcc libasound2-dev alsa-utils procps >/dev/null; }
T=$(mktemp -d); E=src/player
gcc -O2 -Wall -Wextra -fPIC -DPIC -shared -o "$T/libasound_module_pcm_azdrv.so" $E/pcm_azdrv.c -lasound
gcc -O2 -Wall -Wextra -o "$T/azt" tests/linux-azdrv.c $E/src_loop.c -lasound -lm
cat > "$T/asound.conf" <<EOF
pcm_type.azdrv { lib "$T/libasound_module_pcm_azdrv.so" }
pcm.audiolaz { type azdrv }
EOF
export ALSA_CONFIG_PATH=/usr/share/alsa/alsa.conf:$T/asound.conf AUDIOLAZ_SHM=/azt.$$
fail=0
pair() {   # rate fmt seconds ppm
  "$T/azt" read "$1" $(( $3 + 4 )) "$4" "$2" > "$T/r" 2>&1 & rp=$!
  "$T/azt" write audiolaz "$2" "$1" "$3" > "$T/w" 2>&1 || fail=1
  wait $rp || fail=1
  cat "$T/w" "$T/r"
}
for x in "44100 S16" "96000 S24" "192000 S32" "384000 S24_3" "768000 FLOAT" "44100 S32" "768000 S32"; do pair $x 4 0; done
echo "-- the fake DAC 300 ppm fast, the CPUs loaded:"
# (tests/anet-loop.sh's sh -c 'while :…'), and left these 64 spinning
n=$(nproc); i=0; BUSY=; while [ $i -lt "$n" ]; do (while :; do :; done) & BUSY="$BUSY $!"; i=$((i+1)); done
pair 96000 S32 20 300
kill $BUSY 2>/dev/null || true
echo "-- no reader (the host clock stands in):"
"$T/azt" write audiolaz S32 48000 3 || fail=1
echo "-- aplay, bit for bit (16 and 24-bit WAV):"
for b in S16_LE S24_3LE; do
  f=$( [ $b = S16_LE ] && echo S16 || echo S24_3 )
  rm -f "$T/p.raw"; "$T/azt" write "file:'$T/p.raw',raw" $f 44100 4 >/dev/null 2>&1   # the pattern into a file (null device)
  "$T/azt" read 44100 8 0 $f > "$T/r" 2>&1 & rp=$!
  aplay -q -D audiolaz -t raw -f $b -c 2 -r 44100 "$T/p.raw" || fail=1
  wait $rp || fail=1; cat "$T/r"
done
echo "-- an app killed holding it:"
"$T/azt" write audiolaz S32 44100 30 >/dev/null 2>&1 & wp=$!
"$T/azt" read 44100 30 0 S32 > "$T/r" 2>&1 & rp=$!
sleep 2; kill -9 $wp; t=$(date +%s); wait $rp || true; echo "reader let go after $(( $(date +%s) - t )) s (2 s of nothing written): $(cat "$T/r")"
echo "-- roomd reading it (--in-driver 0, no Loopback, the null DAC paced to the host clock):"
[ -f /usr/include/FLAC/stream_decoder.h ] || apt-get install -y -qq libflac-dev >/dev/null
(cd src/player && gcc -std=gnu11 -O2 -w -DROOM_PORTABLE -DROOM_PLAYER_ONLY -DROOM_NO_UPSAMPLER -o "$T/roomd" roomd.c squeeze.c page.c netout.c out_alsa.c src_loop.c src_upnp.c upnp.c upnp_dec.c -lasound -ldl -lm -lpthread)
gcc -O2 -shared -fPIC -o "$T/faults.so" tests/alsa-faults.c -ldl -lasound
mkdir -p "$T/rooms"; printf 'room=none\nbypass=1\ngain=-13\nupsample=0\n' > "$T/rooms/control"
LD_PRELOAD="$T/faults.so" "$T/roomd" --loop nocard --loop-subs 1 --in-driver 0 --dac null --rooms "$T/rooms" --page 8775 --output test --rt 0 --writer-rt 0 > "$T/roomd.log" 2>&1 & ep=$!
sleep 1
"$T/azt" write audiolaz S32 96000 ${ROOMD_S:-25} || fail=1
grep -E "roon_slowed_ppm|steady_ppm|^state" "$T/rooms/status" 2>/dev/null | tr '\n' ' '; echo
kill -INT $ep; sleep 1; kill -9 $ep 2>/dev/null || true
grep -E "audiolaz|clock:|ring ran dry|Roon slowed|play" "$T/roomd.log" | head -12
grep -q "« audiolaz »" "$T/roomd.log" || fail=1
rm -f /dev/shm/azt.$$
[ $fail = 0 ] && echo "linux-azdrv: OK" || { echo "linux-azdrv: FAILED"; exit 1; }
