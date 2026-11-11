#!/usr/bin/env bash
# Configure and build the port.
#
#   scripts/build.sh          work/port/build-vk, the Vulkan build you play
#   scripts/build.sh --tests  work/port/build too, the headless build tests use
#
# The lifted PPU code compiles with clang++ (work/port/tools/ppu_cxx.sh) in
# chunks of about 2 GB each, so the default is four jobs, and on systemd hosts
# the build runs in a 14 GB memory scope with no swap: a wide parallel build
# can exhaust RAM and take the desktop down. PORT_PPU_GCC=1 compiles them with
# GCC instead, ~5 GB a chunk, and two jobs. PORT_BUILD_MEM=<size> raises the
# memory scope on a bigger machine. JOBS=<n> and PORT_SPU_MARCH=<march>
# override (x86-64-v3 for a CPU without AVX-512; the port checks at start-up
# that the CPU has what it was built for).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# One build at a time: several agents share this tree, and two builds in the
# same build directory corrupt each other. A second build waits here.
if [ -z "${PORT_BUILD_LOCKED:-}" ] && command -v flock >/dev/null; then
    export PORT_BUILD_LOCKED=1
    exec flock "$ROOT/work/.build.lock" "$0" "$@"
fi
DIR="${PS3RECOMP_DIR:-$ROOT/ps3recomp}"
PORT="$ROOT/work/port"
if [ -n "${PORT_PPU_GCC:-}" ]; then JOBS="${JOBS:-2}"; else JOBS="${JOBS:-4}"; fi
# The SPU code's -march: AVX-512 (x86-64-v4) when this CPU has it, else
# x86-64-v3 (AVX2). PORT_SPU_MARCH overrides, e.g. to build for another machine.
if [ -z "${PORT_SPU_MARCH:-}" ]; then
    if grep -qw avx512bw /proc/cpuinfo 2>/dev/null && grep -qw avx512vl /proc/cpuinfo; then
        PORT_SPU_MARCH=x86-64-v4
    else
        PORT_SPU_MARCH=x86-64-v3
    fi
fi
MARCH="$PORT_SPU_MARCH"
# The lifted PPU code's PGO profile (CMakeLists.txt PORT_PPU_PROFILE): used
# when work/pgo/ppu.profdata exists (it is not in git: an instrumented build
# and a training run make it, docs/NOTES.md "PGO"); PORT_PPU_PROFILE=<file>
# picks another, PORT_PPU_PROFILE= none. clang only.
PROFILE="${PORT_PPU_PROFILE-$ROOT/work/pgo/ppu.profdata}"
if [ -n "${PORT_PPU_GCC:-}" ] || [ ! -f "$PROFILE" ]; then PROFILE=""; fi
# The same for the lifted SPU code: work/pgo/spu.profdata. PORT_SPU_PROFILE_GEN=1
# (with PORT_PGO_RT and PORT_PGO_DUMP) builds the instrumented training binary.
SPU_PROFILE="${PORT_SPU_PROFILE-$ROOT/work/pgo/spu.profdata}"
if [ -n "${PORT_SPU_GCC:-}" ] || [ ! -f "$SPU_PROFILE" ]; then SPU_PROFILE=""; fi
SPU_GEN=OFF; [ -n "${PORT_SPU_PROFILE_GEN:-}" ] && SPU_GEN=ON

[ -f "$ROOT/work/src/recomp/ppu_recomp.h" ] || { echo "nothing lifted yet -- run scripts/lift.sh" >&2; exit 1; }

# Distribution compilers (Ubuntu's GCC and clang) add a stack-protector canary
# to every function with a local array and CET markers to every function: a
# cost on each call of the runtime's per-method and per-draw paths (the RSX
# thread's method dispatch showed the canary check among its hottest
# instructions). PORT_HARDEN=1 keeps the distribution's defaults.
HARDEN="-fno-stack-protector -fcf-protection=none"
[ -n "${PORT_HARDEN:-}" ] && HARDEN=""
configure() {   # <build dir> <vulkan ON|OFF>
    cmake -S "$PORT" -B "$1" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DPS3RECOMP_DIR="$DIR" -DRECOMP_DIR="$ROOT/work/src/recomp" -DSPU_DIR="$PORT/spu" \
        -DPS3RECOMP_RSX_VULKAN="$2" -DPython3_EXECUTABLE="${PYTHON:-$ROOT/.venv/bin/python}" -DPORT_SPU_MARCH="$MARCH" \
        -DCMAKE_C_COMPILER_LAUNCHER="$PORT/tools/spu_cc.sh" \
        -DCMAKE_CXX_COMPILER_LAUNCHER="$PORT/tools/ppu_cxx.sh" \
        -DCMAKE_C_FLAGS="${PORT_CFLAGS-$HARDEN}" -DCMAKE_CXX_FLAGS="${PORT_CXXFLAGS-$HARDEN}" \
        -DPORT_PPU_PROFILE="$PROFILE" -DPORT_SPU_PROFILE="$SPU_PROFILE" -DPORT_SPU_PROFILE_GEN="$SPU_GEN" \
        -DPORT_PGO_RT="${PORT_PGO_RT:-}" -DPORT_PGO_DUMP="${PORT_PGO_DUMP:-}"
}
build() {
    if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
        systemd-run --user --scope -q -p MemoryMax="${PORT_BUILD_MEM:-14G}" -p MemorySwapMax=0 nice -n 10 \
            cmake --build "$1" -j "$JOBS" -- -k 0
    else
        nice -n 10 cmake --build "$1" -j "$JOBS" -- -k 0
    fi
}

configure "$PORT/build-vk" ON
build "$PORT/build-vk"
if [ "${1:-}" = "--tests" ]; then
    configure "$PORT/build" OFF
    build "$PORT/build"
fi
echo "built: work/port/build-vk/MotorStormRecomp -- run work/play.sh"
