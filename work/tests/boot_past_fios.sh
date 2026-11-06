#!/usr/bin/env bash
# Milestone 2 regression: FIOS's EDGE zlib task (SPU image 31) keeps serving
# ms3.psarc decompression requests after Havok creates its taskset.
#
# The old failure: every taskset had workload id 0, so the zlib task's LFQueue
# waiter id was 0x0000 and a PPU push to the SPURS-owned queue (q=0x452F8A00
# then)
# woke "the last taskset created". Once HavokSpursTaskset existed, the push
# signalled Havok's task 0, the zlib task slept in WAIT_SIGNAL forever and
# "fios mediathread 5" blocked in cellSpursEventFlagWait on flag 0x452F8980;
# boot never reached cellGcmInit.
#
# PASS: no cellSpursEventFlagWait is BLOCKED for 10 s or more, every LFQueue
# wake goes to the zlib taskset, and boot reaches cellGcmInit (the
# "[cellGcmSys] Init(" line, logged after Havok's taskset is created).
# Usage: tests/boot_past_fios.sh [seconds]     BIN=... overrides the binary.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
RUNS="${RUNS:-$WORK/runs}"; mkdir -p "$RUNS"   # RUNS=<dir> keeps this run's logs and pictures apart
BIN="${BIN:-$WORK/port/build-vk/MotorStormRecomp}"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
SECS="${1:-45}"
mkdir -p "$RUNS"
LOG="$RUNS/boot_past_fios.log"

[ -x "$BIN" ] || { echo "FAIL: no binary at $BIN (scripts/build.sh)"; exit 2; }
SPURS_LFQ_LOG=1 SDL_NO_SIGNAL_HANDLERS=1 SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy \
    timeout -k 5 "$SECS" "$BIN" "$ELF" > "$LOG" 2>&1
rc=$?

grep -q '^\[ppu\] run: code 0x' "$LOG" ||
    { echo "FAIL: the guest entry was never dispatched (status $rc, $LOG)"; exit 1; }
case "$rc" in 0|124) ;; *) echo "FAIL: exited with status $rc ($LOG)"; exit 3 ;; esac

blocked=$(grep -E 'EventFlagWait BLOCKED .* ([1-9][0-9]+)s on pattern' "$LOG" | head -1)
if [ -n "$blocked" ]; then
    echo "FAIL: a PPU thread is stuck in cellSpursEventFlagWait: $blocked ($LOG)"; exit 1
fi
# The zlib task's taskset: the one its context (image 31) was built for.
zts=$(grep -m1 -o 'built SpursTasksetContext image=31 taskset=0x[0-9A-F]*' "$LOG" | sed 's/.*taskset=//')
# The zlib request queue: the one 32-byte x 12 LFQueue FIOS creates (its
# address follows the heap layout: 0x452F8A00 with v0.13's user memory at
# 0x40000000, 0x162F7180 with v0.18's at 0x10000000).
zq=$(grep -m1 -o '_LFQueueInitialize(owner=0x[0-9A-F]* q=0x[0-9A-F]* buf=0x[0-9A-F]* size=32 depth=12' "$LOG" |
     sed 's/.* q=\(0x[0-9A-F]*\).*/\1/')
pushes=$(grep -c "q=${zq:-NONE} wake id=" "$LOG")
wrong=$(grep "q=${zq:-NONE} wake id=" "$LOG" | grep -vc "taskset ${zts:-NONE} ")
if [ "${pushes:-0}" -eq 0 ]; then
    echo "FAIL: no zlib LFQueue wake was logged ($LOG)"; exit 1
fi
if [ "$wrong" -ne 0 ]; then
    echo "FAIL: $wrong of $pushes zlib queue wakes went to a taskset other than ${zts:-?} ($LOG)"; exit 1
fi
grep -q '^\[cellGcmSys\] Init(' "$LOG" ||
    { echo "FAIL: boot never reached cellGcmInit ($pushes zlib wakes; $LOG)"; exit 1; }
echo "PASS: $pushes zlib queue wakes, all to taskset $zts; no EventFlagWait stall; reached cellGcmInit ($LOG)"
