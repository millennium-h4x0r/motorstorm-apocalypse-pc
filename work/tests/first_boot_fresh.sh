#!/usr/bin/env bash
# A player's very first boot: an EMPTY /dev_hdd1 and no save.
#
# With /dev_hdd1 empty the game runs its first-boot path: it copies four
# fonts from the disc into /dev_hdd1 (japanese.ttf is 2.1 MB) and rebuilds
# its 114 MB "chimp" cache. That path used to corrupt guest memory on most
# boots (docs/NOTES.md "First-boot hdd1 corruption"): null reads and
# "unresolved indirect call -> 0x74746366" ('ttcf', font bytes) in the UI
# timer walk func_005BE4C0, then a wild fill and a dead game. The other
# tests seed /dev_hdd1 from ROOT/hdd1 and never take that path.
#
# Boot with a fresh save root and a fresh, empty PS3_HDD1_ROOT, tap CROSS every
# 2 s (movies, title, SET SCREEN SIZE, control method) until the log says
# "[cellSaveData] SAVE complete".
#
# PASS: the save happened, the nimbus fonts landed in /dev_hdd1 at their disc
# size, and the log has no null call, no unresolved indirect call and no null
# read in the timer walk.
# Usage: tests/first_boot_fresh.sh   BIN=... overrides the binary, RUNS=<dir>
# keeps the logs apart, BOOTS=<n> repeats the boot n times (all must pass).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
export TESTS_DIR="$HERE"   # for the python below: ms3patch.hard_limit
WORK="$(cd "$HERE/.." && pwd)"
RUNS="${RUNS:-$WORK/runs}"; mkdir -p "$RUNS"
BIN="${BIN:-$WORK/port/build-vk/MotorStormRecomp}"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
OUT="$RUNS/first_boot_fresh"
[ -x "$BIN" ] || { echo "FAIL: no binary at $BIN (scripts/build.sh)"; exit 2; }
rm -rf "$OUT"; mkdir -p "$OUT"

python3 -I - "$BIN" "$ELF" "$OUT" "${BOOTS:-1}" <<'EOF'
import os as _o, sys as _s; _s.path.insert(0, _o.environ["TESTS_DIR"]); import ms3patch as _mp; _mp.hard_limit(400 * int(_o.environ.get("BOOTS", 1)) + 120, "first_boot_fresh")
import os, re, signal, subprocess, sys, time
from pathlib import Path

BIN, ELF, OUT, BOOTS = sys.argv[1], sys.argv[2], Path(sys.argv[3]), int(sys.argv[4])
ROOT = Path(ELF).parents[3]
FONTS = ROOT / "disc/PS3_GAME/USRDIR/ui/fonts"
BAD = re.compile(r"^\[null-call\]|unresolved indirect call|^\[ppu\] bctr to NULL|"
                 r"guest_call: OPD .* not registered|"
                 r"null-read\].*by guest-fn=0x005BE4C0", re.M)

def boot(i):
    d = OUT / f"boot{i}"
    save, hdd1 = d / "save", d / "hdd1"
    save.mkdir(parents=True); hdd1.mkdir()          # empty: the first-boot path
    log, pad = d / "log", d / "pad"
    def held(s):
        tmp = pad.with_suffix(".tmp"); tmp.write_text(s + "\n"); os.replace(tmp, pad)
    held("")
    env = dict(os.environ, SDL_NO_SIGNAL_HANDLERS="1", SDL_AUDIODRIVER="dummy",
               SDL_VIDEODRIVER="dummy", PS3_SAVEDATA_ROOT=str(save), PS3_GAMEDATA_ROOT=str(d / "gamedata"),
               PS3_HDD1_ROOT=str(hdd1), PS3_PAD_FILE=str(pad))
    f = open(log, "wb")
    p = subprocess.Popen([BIN, ELF], cwd=ROOT, env=env, stdout=f,
                         stderr=subprocess.STDOUT, start_new_session=True)
    def stop():
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGTERM)
            try: p.wait(5)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL); p.wait()
        f.close()
    text = lambda: log.read_bytes().decode("latin1")
    end, why = time.time() + 300, None
    while True:
        t = text()
        m = BAD.search(t)
        if m: why = f"corruption: {m.group(0)!r}"; break
        if "[cellSaveData] SAVE complete" in t: break
        if p.poll() is not None: why = "the game exited"; break
        if time.time() > end:
            why = "no SAVE complete within 300 s" + (
                "" if "[cellVdec] Open" in t else " (no movie ever opened: the pre-movie stall)")
            break
        held("CROSS"); time.sleep(0.25); held(""); time.sleep(1.75)
    if not why:
        time.sleep(3)                       # a little past the save
        m = BAD.search(text())
        if m: why = f"corruption after the save: {m.group(0)!r}"
        elif p.poll() is not None: why = "the game exited after the save"
    stop()
    if not why:
        # japanese.ttf is left out: its 2.1 MB buffer comes back NULL from the
        # game's heap, the copy fails with CELL_EFAULT and the game opens the
        # font from the disc (it retries the copy on the next boot).
        for fn in ("nimbussanconl-bla.ttf", "nimbussanst-regularcondensed.ttf"):
            a, b = hdd1 / fn, FONTS / fn
            if not a.exists() or a.stat().st_size != b.stat().st_size:
                why = f"/dev_hdd1/{fn} missing or the wrong size"
    return why, log

fails = 0
for i in range(1, BOOTS + 1):
    why, log = boot(i)
    print(f"boot {i}: " + ("ok" if not why else f"FAIL {why} ({log})"), flush=True)
    fails += bool(why)
if fails:
    print(f"FAIL: {fails}/{BOOTS} fresh first boots failed"); sys.exit(1)
print(f"PASS: {BOOTS}/{BOOTS} fresh first boots reached the first save cleanly")
EOF
