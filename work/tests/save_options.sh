#!/usr/bin/env bash
# Save/load: an option changed in one boot is still changed in the next.
#
# Two boots on one fresh save root. Both boot straight into the OPTIONS
# screen: a patch archive in the run's own PS3_HDD0_ROOT
# (/dev_hdd0/game/BCES00484/USRDIR/ms3patchv0101.psarc, the game's own
# gamestates.xml read from the disc with "boot -> booted" sent to the options
# state; see docs/NOTES.md "Every track"). Then, driven from the frame dumps:
#
#   boot 1: OPTIONS -> DOWN, CROSS (DISPLAY) -> DOWN, DOWN (SUBTITLES); the
#           value box is picture A. RIGHT flips it: picture B (must differ
#           from A). CIRCLE, CIRCLE back out; the game saves the profile
#           ("[cellSaveData] SAVE complete").
#   boot 2: the save loads ("LOAD dir='BCES00484' (new=0)"), same path to
#           SUBTITLES: picture C must match B, not A.
#
# PASS also needs no null call, unresolved call/NID, and no sceNpTrophy
# error in either log (trophies must not block a boot). The pictures are kept
# under $RUNS/save_options/.
# Usage: tests/save_options.sh     BIN=... RUNS=...
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
RUNS="${RUNS:-$WORK/runs}"; mkdir -p "$RUNS"
BIN="${BIN:-$WORK/port/build-vk/MotorStormRecomp}"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
OUT="$RUNS/save_options"
[ -x "$BIN" ] || { echo "FAIL: no binary at $BIN (scripts/build.sh)"; exit 2; }
rm -rf "$OUT"; mkdir -p "$OUT"

python3 -I - "$BIN" "$ELF" "$OUT" "$HERE" <<'EOF'
import os, re, shutil, signal, subprocess, sys, time
from pathlib import Path
BIN, ELF, OUT, HERE = os.path.abspath(sys.argv[1]), sys.argv[2], Path(sys.argv[3]).resolve(), Path(sys.argv[4])
sys.path.insert(0, str(HERE)); import ms3patch as mp
ROOT = Path(ELF).parents[3]

def fail(msg): print("FAIL: " + msg); sys.exit(1)

hdd0 = OUT / "hdd0"; usr = hdd0 / "game/BCES00484/USRDIR"; usr.mkdir(parents=True)
gs = mp.stock_gamestates(ROOT)
gs, n = re.subn(r'<Transition FromState="boot" Name="booted" ToState="mainmenu"[^>]*/>',
                '<Transition FromState="boot" Name="booted" ToState="options"/>', gs)
assert n == 1
mp.psarc_write(usr / "ms3patchv0101.psarc", {"/data/gamestate/gamestates.xml": gs.encode()})
save = OUT / "save"; save.mkdir()
BAD = re.compile(r"^\[null-call\]|unresolved indirect call|unresolved NID|"
                 r"guest_call: OPD .* not registered|\[sceNpTrophy\].*(error|fail|FAIL)", re.M)

class Game:
    def __init__(self, name):
        self.dir = OUT / name; (self.dir / "d").mkdir(parents=True)
        shutil.copytree(ROOT / "hdd1", self.dir / "hdd1")
        self.log, self.pad = self.dir / "log", self.dir / "pad"; self.set("")
        env = dict(os.environ, SDL_NO_SIGNAL_HANDLERS="1", SDL_AUDIODRIVER="dummy", SDL_VIDEODRIVER="dummy",
                   PS3_SAVEDATA_ROOT=str(save), PS3_GAMEDATA_ROOT=str(OUT / "gamedata"), PS3_HDD1_ROOT=str(self.dir / "hdd1"),
                   PS3_HDD0_ROOT=str(hdd0), PS3_PAD_FILE=str(self.pad),
                   LD_FRAME_DUMP=str(self.dir / "d"), LD_FRAME_DUMP_EVERY="10")
        self.f = open(self.log, "wb")
        self.p = subprocess.Popen([BIN, ELF], cwd=ROOT, env=env, stdout=self.f,
                                  stderr=subprocess.STDOUT, start_new_session=True)
    def set(self, held):
        t = self.pad.with_suffix(".tmp"); t.write_text(held + "\n"); os.replace(t, self.pad)
    def tap(self, b, wait=1.5):
        self.check(); self.set(b); time.sleep(0.25); self.set(""); time.sleep(wait)
    def text(self): return self.log.read_bytes().decode("latin1")
    def check(self):
        m = BAD.search(self.text())
        if m: self.stop(); fail(f"{m.group(0)!r} in {self.log}")
        if self.p.poll() is not None: self.stop(); fail(f"the game exited ({self.log})")
    def frame(self, newer_than=0):
        """The newest complete dump, at least newer_than seconds after now."""
        t0 = time.time()
        while True:
            fr = sorted((self.dir / "d").glob("frame_*.ppm"), key=os.path.getmtime)
            if len(fr) > 1 and os.path.getmtime(fr[-2]) >= t0 + newer_than: return fr[-2]
            self.check(); time.sleep(0.3)
    def until(self, cond, limit, what):
        end = time.time() + limit
        while time.time() < end:
            self.check()
            if cond(): return
            time.sleep(1)
        self.stop(); fail(f"no {what} within {limit} s ({self.log})")
    def stop(self):
        if self.p.poll() is None:
            os.killpg(self.p.pid, signal.SIGTERM)
            try: self.p.wait(5)
            except subprocess.TimeoutExpired: os.killpg(self.p.pid, signal.SIGKILL); self.p.wait()
        self.f.close()

def header(fr):
    """Yellow OPTIONS title, top left."""
    w, h, px = mp.read_ppm(fr)
    return mp.count(px, w, h, (100, 380, 40, 120), lambda r, g, b: r > 200 and g > 150 and b < 90) > 150

VALUE = (930, 1040, 372, 400)      # the SUBTITLES value ("Off"/"On"), 1280x720
def value(fr):
    w, h, px = mp.read_ppm(fr); return mp.crop(px, w, h, VALUE)

YEL = lambda r, g, b: r > 200 and g > 150 and b < 90
def display(fr):
    """The DISPLAY SETTINGS subtitle under OPTIONS."""
    w, h, px = mp.read_ppm(fr); return mp.count(px, w, h, (80, 260, 82, 108), YEL, 1) > 600
def on_subtitles(fr):
    """The yellow < > of the focused row, on the SUBTITLES row."""
    w, h, px = mp.read_ppm(fr)
    return (mp.count(px, w, h, (812, 842, 372, 398), YEL, 1) > 100 and
            mp.count(px, w, h, (1132, 1162, 372, 398), YEL, 1) > 100)

def to_subtitles(g):
    """OPTIONS -> DISPLAY -> SUBTITLES, checking each step on the frames
    (a press while a screen slides in is lost)."""
    g.until(lambda: header(g.frame()), 240, "OPTIONS screen")
    time.sleep(3)
    for _ in range(4):
        g.tap("DOWN"); g.tap("CROSS", 3)
        if display(g.frame(0.5)): break
        g.tap("CIRCLE", 3)                    # wrong screen: back to the OPTIONS list, top item
        g.tap("UP"); g.tap("UP"); g.tap("UP"); g.tap("UP")
    else:
        g.stop(); fail(f"could not open DISPLAY settings ({g.log})")
    for _ in range(6):
        fr = g.frame(0.5)
        if on_subtitles(fr): return fr
        g.tap("DOWN", 2)
    g.stop(); fail(f"could not focus SUBTITLES ({g.log})")

# -- boot 1: change it ----------------------------------------------------------
g = Game("boot1")
fa = to_subtitles(g); a = value(fa)
g.tap("RIGHT", 2); fb = g.frame(0.5); b = value(fb)
if mp.diff(a, b) < 8: g.stop(); fail(f"RIGHT did not change SUBTITLES ({fa} vs {fb}, diff {mp.diff(a, b):.1f})")
saves = lambda: g.text().count("[cellSaveData] SAVE complete")
g.tap("CIRCLE", 2); g.tap("CIRCLE", 2)
g.until(lambda: saves() >= 1, 120, "profile save after leaving OPTIONS")
time.sleep(3); g.stop()
shutil.copy(fa, OUT / "A.ppm"); shutil.copy(fb, OUT / "B.ppm")

# -- boot 2: is it still changed? -----------------------------------------------
g = Game("boot2")
g.until(lambda: "LOAD dir='BCES00484' (new=0)" in g.text(), 240, "save load (new=0)")
fc = to_subtitles(g); c = value(fc)
g.stop(); shutil.copy(fc, OUT / "C.ppm")
dab, dcb, dca = mp.diff(a, b), mp.diff(c, b), mp.diff(c, a)
if not (dcb < dab / 3 and dca > dab / 2):
    fail(f"SUBTITLES after the reboot does not match the saved change: |C-B| {dcb:.1f}, |C-A| {dca:.1f}, "
         f"|A-B| {dab:.1f} ({OUT})")
print(f"PASS: option changed, saved and loaded on the next boot (|A-B| {dab:.1f}, |C-B| {dcb:.1f}, "
      f"|C-A| {dca:.1f}) -> {OUT}")
EOF
