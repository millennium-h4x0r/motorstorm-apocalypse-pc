#!/bin/sh
# Build and run test_vdec_real against a real MotorStorm Apocalypse movie.
#   ./run_test.sh [movie.avi] [frames]
# Needs: libavcodec/libavutil/libswscale dev packages (pkg-config), an ffmpeg
# CLI for the audio reference (optional).
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../../.. && pwd)
AVI=${1:-$ROOT/disc/PS3_GAME/USRDIR/ui/video/evologo_60.avi}
FRAMES=${2:-300}
OUT=${OUT:-out}
mkdir -p "$OUT"
# ps3/ mirrors the ps3recomp layout so cellVdec.c's relative includes resolve:
# ps3/libs/codec is our copy, the rest are symlinks into the real tree.
[ -e ps3/include ] || { echo "ps3/ symlink tree missing"; exit 1; }

cc -std=gnu17 -O2 -w -Ips3/include -DPS3RECOMP_HAVE_FFMPEG \
   $(pkg-config --cflags libavcodec libavutil libswscale) \
   test_vdec_real.c -o "$OUT/test_vdec_real" \
   $(pkg-config --libs libavcodec libavutil libswscale) -lm

if command -v ffmpeg >/dev/null; then
    ffmpeg -v error -y -i "$AVI" -map 0:a:0 -f f32le "$OUT/audio_ref.f32" || true
fi
"$OUT/test_vdec_real" "$AVI" "$OUT" "$FRAMES"
