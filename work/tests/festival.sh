#!/usr/bin/env bash
# Milestone 5: Festival (story) mode. Play the first Festival event after the
# Prologue to the end, and check the progress is saved and loaded.
#
# Needs a save that has finished the Prologue. SAVE_FROM=<save root> (the
# directory holding BCES00484/) uses one; otherwise this first runs
# first_race.sh (about 20-30 min) and takes the save it leaves.
#
# The car is driven by the game itself: a patch archive in the run's own
# PS3_HDD0_ROOT (/dev_hdd0/game/BCES00484/USRDIR/ms3patchv0101.psarc) holds the
# race HUD (/ui/be/hud_definition.xml) with every competitor that has not
# finished put on AutoPilot (the competitor flag the game itself sets after a
# finish), see ms3patch.autopilot_hud. Nothing else is changed: the boot, the
# menus and the Festival flow are the game's own.
#
# Boot 1 (the save copied to a fresh root): CROSS through the movies, the
# title, MAIN MENU > FESTIVAL > Continue, the event's intro, and its results.
# While the race HUD (yellow POS, bottom left) is up nothing is pressed. If
# the car did not qualify the results offer only QUIT and RETRY RACE (SQUARE):
# seen by QUIT's pink circle and RETRY's square without CONTINUE's blue cross
# (qualified results show all three), it presses SQUARE and CROSS (the
# retry asks to confirm); else CROSS. Done at the first
# "[cellSaveData] SAVE complete" after a race HUD was seen: the event's
# progress.
# Boot 2 (same save root): the save loads (new=0), CROSS through to FESTIVAL >
# Continue again, up to the race HUD. The track world read last before the
# HUD must differ from boot 1's: the next event. Worlds are named from the
# ms3.psarc read offsets (FS_CALLER=1 logs every cellFsLseek;
# ms3patch.psarc_ranges).
#
# PASS: both of those, and no null call, unresolved call/NID in either log.
# LIMIT_S (default 3600) bounds each boot.
# Usage: tests/festival.sh   BIN=... RUNS=... SAVE_FROM=...
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
export TESTS_DIR="$HERE"   # for the python below: ms3patch.hard_limit
WORK="$(cd "$HERE/.." && pwd)"
RUNS="${RUNS:-$WORK/runs}"; mkdir -p "$RUNS"
BIN="${BIN:-$WORK/port/build-vk/MotorStormRecomp}"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
OUT="$RUNS/festival"
[ -x "$BIN" ] || { echo "FAIL: no binary at $BIN (scripts/build.sh)"; exit 2; }
rm -rf "$OUT"; mkdir -p "$OUT"
SAVE_FROM="${SAVE_FROM:-}"
if [ -z "$SAVE_FROM" ]; then
    echo "no SAVE_FROM: playing the Prologue first (first_race.sh)"
    OUT="$OUT/prologue" BIN="$BIN" RUNS="$RUNS" "$HERE/first_race.sh" || { echo "FAIL: first_race.sh"; exit 1; }
    SAVE_FROM="$OUT/prologue/save"
fi
[ -d "$SAVE_FROM/BCES00484" ] || { echo "FAIL: no BCES00484 save in $SAVE_FROM"; exit 2; }

python3 -I - "$BIN" "$ELF" "$OUT" "$HERE" "$SAVE_FROM" "${LIMIT_S:-3600}" <<'EOF'
import os as _o, sys as _s; _s.path.insert(0, _o.environ["TESTS_DIR"]); import ms3patch as _mp; _mp.hard_limit(2 * int(_o.environ.get("LIMIT_S", 3600)) + 900, "festival")
_o.environ.setdefault("SPU_TS_WATCHDOG", "20")   # a Havok/taskset hang leaves a [ts-watchdog] dump (cheap otherwise; SPU_TS_WATCHDOG=0 off)
import bisect, os, re, shutil, signal, subprocess, sys, time
from pathlib import Path
BIN, ELF, OUT, HERE = os.path.abspath(sys.argv[1]), sys.argv[2], Path(sys.argv[3]).resolve(), Path(sys.argv[4])
SAVE_FROM, LIMIT = Path(sys.argv[5]).resolve(), int(sys.argv[6])
sys.path.insert(0, str(HERE)); import ms3patch as mp
ROOT = Path(ELF).parents[3]
def fail(msg): print("FAIL: " + msg); sys.exit(1)

hdd0 = OUT / "hdd0"; usr = hdd0 / "game/BCES00484/USRDIR"; usr.mkdir(parents=True)
mp.psarc_write(usr / "ms3patchv0101.psarc", {"/ui/be/hud_definition.xml": mp.autopilot_hud(ROOT)})
save = OUT / "save"; shutil.copytree(SAVE_FROM, save)
WORLDS = mp.psarc_ranges(ROOT / "disc/PS3_GAME/USRDIR/ms3.psarc", "/worlds/track_")
WORLDS = [w for w in WORLDS if not w[2].endswith("_objects.rpk")]
STARTS = [w[0] for w in WORLDS]
BAD = re.compile(r"^\[null-call\]|unresolved indirect call|unresolved NID|guest_call: OPD .* not registered", re.M)

class Game:
    def __init__(self, name):
        self.dir = OUT / name; (self.dir / "d").mkdir(parents=True)
        mp.seed_hdd1(ROOT, self.dir / "hdd1")
        self.log, self.pad = self.dir / "log", self.dir / "pad"; self.set("")
        env = dict(os.environ, SDL_NO_SIGNAL_HANDLERS="1", SDL_AUDIODRIVER="dummy", SDL_VIDEODRIVER="dummy",
                   PS3_SAVEDATA_ROOT=str(save), PS3_GAMEDATA_ROOT=str(OUT / "gamedata"),
                   PS3_HDD1_ROOT=str(self.dir / "hdd1"),
                   PS3_HDD0_ROOT=str(hdd0), PS3_PAD_FILE=str(self.pad), FS_CALLER="1",
                   LD_FRAME_DUMP=str(self.dir / "d"), LD_FRAME_DUMP_EVERY="60")
        self.f = open(self.log, "wb")
        self.p = subprocess.Popen([BIN, ELF], cwd=ROOT, env=env, stdout=self.f,
                                  stderr=subprocess.STDOUT, start_new_session=True, preexec_fn=mp.allow_ptrace)
        self.t0 = time.time(); self.taps = 0; self.pos = 0; self.worlds = []
        self.last_frame, self.last_frame_t = None, time.time()
    def set(self, held):
        t = self.pad.with_suffix(".tmp"); t.write_text(held + "\n"); os.replace(t, self.pad)
    def tap(self, b):
        self.set(b); time.sleep(0.25); self.set("")
    def text(self): return self.log.read_bytes().decode("latin1")
    def check(self, t):
        m = BAD.search(t)
        if m: self.stop(); fail(f"{m.group(0)!r} in {self.log}")
        if self.p.poll() is not None: self.stop(); fail(f"the game exited ({self.log})")
        if time.time() - self.t0 > LIMIT: self.stop(); fail(f"{self.dir.name}: nothing done within {LIMIT} s ({self.log})")
        fr = sorted((self.dir / "d").glob("frame_*.ppm"), key=os.path.getmtime)
        if fr and fr[-1] != self.last_frame: self.last_frame, self.last_frame_t = fr[-1], time.time()
        if self.last_frame and time.time() - self.last_frame_t > 120:
            bt = self.dir / "hang.bt"; mp.host_backtrace(self.p.pid, bt)
            self.stop(); fail(f"{self.dir.name}: no new frame for 120 s, the game hung; host stacks in {bt} ({self.log})")
    def latest(self):
        fr = sorted((self.dir / "d").glob("frame_*.ppm"), key=os.path.getmtime)
        return mp.read_ppm(fr[-2]) if len(fr) > 1 else None
    def drive(self):
        """One step of the pad policy: hands off while the race HUD (yellow
        POS) shows. The results screen (prompts QUIT's pink circle and RETRY
        RACE's square, bottom right) gets CROSS (CONTINUE) when CONTINUE's
        blue cross is there too -- qualified -- and SQUARE (RETRY RACE) only
        when it is not. QUIT is on both, so its circle alone does not mean
        "not qualified": that retried a race won in 1st place. CROSS on
        anything else."""
        f = self.latest()
        if f is None: self.tap("CROSS"); return False
        w, h, px = f
        # The results screens first: their yellow "Cards Found" sits in the
        # race HUD's POS box, and taking that screen for the race left it
        # alone forever (two runs stuck an hour on QUIT / RETRY RACE).
        results = (mp.count(px, w, h, (930, 962, 640, 668),
                            lambda r, g, b: r > 190 and 70 < g < 160 and b > 100 and r - g > 60, 1) > 15 and
                   mp.count(px, w, h, (1028, 1052, 643, 662),
                            lambda r, g, b: r > 150 and g > 110 and b > 120 and 15 < r - g < 70, 1) > 6 and
                   mp.count(px, w, h, (1060, 1170, 643, 664),           # the white "RETRY RACE"
                            lambda r, g, b: r > 215 and g > 215 and b > 215, 1) > 400)
        if results:
            qualified = mp.count(px, w, h, (790, 820, 640, 668),
                                 lambda r, g, b: b > 150 and b - r > 40, 1) > 8
            if qualified:
                self.tap("CROSS")                 # CONTINUE
            else:                                 # RETRY RACE, then CROSS on its "are you sure"
                self.tap("SQUARE"); time.sleep(1.5); self.tap("CROSS")
            return False
        if mp.count(px, w, h, (40, 220, 560, 640), lambda r, g, b: r > 200 and g > 150 and b < 90) > 25:
            return True
        self.tap("CROSS")
        return False
    def new_worlds(self, t):
        """Track worlds the game reads, in order. The boot reads a header of
        some (the DLC ones); the event's world is read later, before its race,
        so the world read last before the race HUD is the event's (a world
        is read in a few big sequential reads: one or two lseeks)."""
        for m in re.finditer(r"lseek fd=\d+ off=(\d+)", t[self.pos:]):
            o = int(m.group(1)); i = bisect.bisect_right(STARTS, o) - 1
            if i >= 0 and WORLDS[i][0] <= o < WORLDS[i][1]:
                n = WORLDS[i][2][len("/worlds/"):-len(".rpk")]
                if not self.worlds or self.worlds[-1] != n:
                    self.worlds.append(n)
        self.pos = len(t)
    def stop(self):
        if self.p.poll() is None:
            os.killpg(self.p.pid, signal.SIGTERM)
            try: self.p.wait(5)
            except subprocess.TimeoutExpired: os.killpg(self.p.pid, signal.SIGKILL); self.p.wait()
        self.f.close()

# -- boot 1: play the event ----------------------------------------------------------
g = Game("boot1"); raced = False; saves0 = None
while True:
    time.sleep(3); t = g.text(); g.check(t); g.new_worlds(t)
    n = t.count("[cellSaveData] SAVE complete")
    if g.drive():
        if not raced:
            if not g.worlds: g.stop(); fail(f"race HUD but no track world loaded ({g.log})")
            event1 = g.worlds[-1]
            print(f"  boot1: race HUD at {time.time()-g.t0:.0f} s on {event1}", flush=True)
        raced = True; saves0 = n
    elif raced and saves0 is not None and n > saves0:
        break
time.sleep(5); g.stop()
print(f"  boot1: event {event1} finished and saved at {time.time()-g.t0:.0f} s", flush=True)

# -- boot 2: does the Festival continue from the next event? -------------------------
g = Game("boot2")
while True:
    time.sleep(3); t = g.text(); g.check(t); g.new_worlds(t)
    if g.drive(): break
if "LOAD dir='BCES00484' (new=0)" not in g.text(): g.stop(); fail(f"boot 2 did not load the save ({g.log})")
g.stop()
if not g.worlds: fail(f"boot 2: race HUD but no track world loaded ({g.log})")
event2 = g.worlds[-1]
if event2 == event1:
    fail(f"boot 2 continued the Festival on the same event ({event1}): the progress was not kept")
print(f"PASS: Festival event on {event1} finished and saved; the next boot continues on {event2}")
EOF
