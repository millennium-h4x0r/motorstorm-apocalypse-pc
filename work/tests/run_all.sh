#!/usr/bin/env bash
# Every MotorStorm Apocalypse boot regression, one after the other: each
# work/tests/*.sh other than this one. Each prints its verdict as its last
# line, starting with PASS for a pass: reach_entry, boot_past_fios,
# first_draw, first_frame, main_menu (milestone 3). Add new ones as
# work/tests/<name>.sh.
# Usage: tests/run_all.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
fail=0 n=0
for f in "$HERE"/*.sh; do
    [ -e "$f" ] || continue
    t="$(basename "$f" .sh)"
    [ "$t" = run_all ] && continue
    n=$((n + 1))
    out=$(bash "$f" 2>&1 | tail -1)
    echo "[$t] $out"
    case "$out" in PASS*) ;; *) fail=$((fail + 1)) ;; esac
done
[ "$n" -eq 0 ] && echo "no tests in $HERE"
echo "failed: $fail of $n"
exit $((fail > 0))
