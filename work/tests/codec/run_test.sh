#!/bin/sh
# Build and run test_vdec_real against a real MotorStorm Apocalypse movie.
#   ./run_test.sh [movie.avi] [frames]
# PS3RECOMP_DIR=<toolkit> picks the toolkit (default ./ps3recomp). Needs the
# libavcodec/libavutil dev headers (the toolkit loads FFmpeg at run time) and,
# optionally, an ffmpeg CLI for the audio reference.
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../../.. && pwd)
TK=$(cd "${PS3RECOMP_DIR:-$ROOT/ps3recomp}" && pwd)
AVI=${1:-$ROOT/disc/PS3_GAME/USRDIR/ui/video/evologo_60.avi}
FRAMES=${2:-300}
OUT=${OUT:-out}
mkdir -p "$OUT"
# test_vdec_real.c includes the toolkit's sources as "ps3/libs/...":
# $OUT/ps3 points at the toolkit.
ln -sfn "$TK" "$OUT/ps3"
AVINC=$(pkg-config --cflags-only-I libavcodec libavutil 2>/dev/null || true)
[ -n "$AVINC" ] || AVINC=-I/usr/include/$(cc -print-multiarch 2>/dev/null)

cc -std=gnu17 -O2 -w -I"$OUT" -I"$TK/include" -DPS3RECOMP_FFMPEG_RUNTIME $AVINC \
   test_vdec_real.c -o "$OUT/test_vdec_real" -ldl -lpthread -lm

if command -v ffmpeg >/dev/null; then
    ffmpeg -v error -y -i "$AVI" -map 0:a:0 -f f32le "$OUT/audio_ref.f32" || true
fi
"$OUT/test_vdec_real" "$AVI" "$OUT" "$FRAMES"
