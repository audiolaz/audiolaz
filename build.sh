#!/bin/sh
# Builds the audiolaz player and its companions from this folder alone.
#   ./build.sh              build/audiolaz-player; Linux: build/audiolaz-recv, build/libasound_module_pcm_audiolaz.so
#   ./build.sh kmod         Linux: src/linux/kmod/snd-audiolaz.ko (the running kernel's headers, or KDIR=…)
#   ./build.sh mac-driver   macOS: build/audiolaz.driver
#   ./build.sh test         the unit tests (tests/run.sh)
# CC, CFLAGS: as usual. LANG_BUILD=en: the page in English (the default), fr: in French.
set -eu
cd "$(dirname "$0")"
VERSION=$(cat VERSION 2>/dev/null || echo 1.2.6)
CC=${CC:-cc}
W="-std=gnu11 -O3 -Wall -Wextra -Wno-missing-field-initializers -D_DEFAULT_SOURCE -DROOM_PORTABLE -DROOM_PLAYER_ONLY -DROOM_NO_UPSAMPLER"
L=; [ "${LANG_BUILD:-en}" = en ] && L=-DROOM_EN
P=src/player
mkdir -p build
case ${1:-player} in
player)
  case $(uname -s) in
  Linux)
    $CC $W $L ${CFLAGS:-} "-DROOMD_VERSION=\"$VERSION\"" -o build/audiolaz-player \
      $P/roomd.c $P/squeeze.c $P/page.c $P/netout.c $P/out_alsa.c $P/src_loop.c $P/src_upnp.c $P/upnp.c $P/upnp_dec.c \
      -lasound -lm -lpthread -ldl
    $CC -std=gnu11 -O2 -Wall -Wextra ${CFLAGS:-} -o build/audiolaz-recv $P/audiolaz-recv.c $P/anet_mdns.c $P/out_alsa.c -lasound -lpthread -lm
    $CC -std=gnu11 -O2 -Wall -Wextra -fPIC -DPIC -shared ${CFLAGS:-} -o build/libasound_module_pcm_audiolaz.so $P/pcm_azdrv.c -lasound
    ;;
  Darwin)
    for A in arm64 x86_64; do
      $CC -arch $A $W $L -D_DARWIN_C_SOURCE -mmacosx-version-min=13.5 -DROOM_FLAC_BUILTIN -Isrc/mac ${CFLAGS:-} "-DROOMD_VERSION=\"$VERSION\"" \
        -o build/audiolaz-player-$A $P/roomd.c $P/squeeze.c $P/page.c $P/netout.c $P/out_coreaudio.c $P/src_pipe.c $P/src_upnp.c \
        $P/upnp.c $P/upnp_dec.c src/mac/flac.c -framework CoreAudio -framework AudioToolbox -framework CoreFoundation -lm -lpthread
    done
    lipo -create build/audiolaz-player-arm64 build/audiolaz-player-x86_64 -output build/audiolaz-player
    rm -f build/audiolaz-player-arm64 build/audiolaz-player-x86_64
    codesign --force --sign - build/audiolaz-player 2>/dev/null || true
    ;;
  *) echo "Linux or macOS" >&2; exit 1 ;;
  esac
  ./build/audiolaz-player --version
  ;;
kmod) make -C src/linux/kmod ;;
mac-driver) AUDIOLAZ_VERSION=$VERSION sh src/mac/driver/build.sh "$(pwd)/build" ;;
test) sh tests/run.sh ;;
*) echo "usage: ./build.sh [player|kmod|mac-driver|test]" >&2; exit 2 ;;
esac
