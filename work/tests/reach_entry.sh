#!/usr/bin/env bash
# Milestone 1: the recompiled title loads and runs its own entry point.
#
# PASS: the loader dispatches the guest entry ("[ppu] run: code 0x...") and
# the process is still running when the timeout stops it (status 124), or the
# guest exits on its own after running. A crash, or no dispatch, is a FAIL.
# Usage: tests/reach_entry.sh [seconds]     BIN=... overrides the binary.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
RUNS="${RUNS:-$WORK/runs}"; mkdir -p "$RUNS"   # RUNS=<dir> keeps this run's logs and pictures apart
BIN="${BIN:-$WORK/port/build-vk/MotorStormRecomp}"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
SECS="${1:-20}"
mkdir -p "$RUNS"
LOG="$RUNS/reach_entry.log"
# Its own save, gamedata and /dev_hdd1 (the seed hard-linked, ms3patch.py):
# without them the game writes the player's gamedata/ and ROOT/hdd1.
ROOTS="$RUNS/reach_entry_roots"; chmod -R u+w "$ROOTS" 2>/dev/null; rm -rf "$ROOTS"; mkdir -p "$ROOTS/save" "$ROOTS/gamedata"
if [ -d "$WORK/runs/hdd1_seed" ]; then cp -al "$WORK/runs/hdd1_seed" "$ROOTS/hdd1" && find "$ROOTS/hdd1" -type d -exec chmod u+w {} +
else mkdir -p "$ROOTS/hdd1"; fi
export PS3_SAVEDATA_ROOT="$ROOTS/save" PS3_GAMEDATA_ROOT="$ROOTS/gamedata" PS3_HDD1_ROOT="$ROOTS/hdd1"

[ -x "$BIN" ] || { echo "FAIL: no binary at $BIN (scripts/build.sh)"; exit 2; }
SDL_NO_SIGNAL_HANDLERS=1 SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy \
    timeout -k 5 "$SECS" "$BIN" "$ELF" > "$LOG" 2>&1
rc=$?

if ! grep -q '^\[ppu\] run: code 0x' "$LOG"; then
    echo "FAIL: the guest entry was never dispatched (status $rc, $LOG)"; exit 1
fi
case "$rc" in
    0|124) ;;
    *) echo "FAIL: exited with status $rc after reaching the entry ($LOG)"; exit 3 ;;
esac
echo "PASS: entry dispatched; $(grep -c '' "$LOG") log lines, status $rc ($LOG)"
