#!/usr/bin/env bash
# Milestone 2: boot reaches the first RSX draw call.
#
# The blocker before it: the game's cross-thread call (fn 0x101D8D4) waits on
# a stack COPY of the target's sys_lwcond_t and the servicing pump
# (fn 0x101D294) signals the copy in its request ring. The lwcond HLE matched
# waiters by EA, so the signal was lost: async_2 slept forever inside the
# post.rpk read and the main thread spun on that read's done flag
# (fn 0x6E5184, byte +0x5C of the reader), never issuing a draw.
#
# PASS: the RSX logs a DRAW_ARRAYS or DRAW_INDEX_ARRAY and the process is
# still running at the timeout (or exits on its own) without crashing.
# Usage: tests/first_draw.sh [seconds]     BIN=... overrides the binary.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
BIN="${BIN:-$WORK/port/build-vk/MotorStormRecomp}"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
SECS="${1:-60}"
mkdir -p "$WORK/runs"
LOG="$WORK/runs/first_draw.log"

[ -x "$BIN" ] || { echo "FAIL: no binary at $BIN (scripts/build.sh)"; exit 2; }
SDL_NO_SIGNAL_HANDLERS=1 SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy \
    timeout -k 5 "$SECS" "$BIN" "$ELF" > "$LOG" 2>&1
rc=$?

grep -q '^\[ppu\] run: code 0x' "$LOG" ||
    { echo "FAIL: the guest entry was never dispatched (status $rc, $LOG)"; exit 1; }
case "$rc" in 0|124) ;; *) echo "FAIL: exited with status $rc ($LOG)"; exit 3 ;; esac
draws=$(grep -cE '^\[RSX\] DRAW_(ARRAYS|INDEX_ARRAY)' "$LOG")
if [ "$draws" -eq 0 ]; then
    echo "FAIL: no RSX draw call in ${SECS}s (status $rc, $LOG)"; exit 1
fi
echo "PASS: $draws RSX draw calls logged; first: $(grep -m1 -E '^\[RSX\] DRAW_' "$LOG") ($LOG)"
