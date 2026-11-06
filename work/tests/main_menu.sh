#!/usr/bin/env bash
# Milestone 3: from the boot to the main menu, driven by a virtual pad, and
# the menu reacting to input.
#
# MotorStorm Apocalypse only shows its main menu once a save exists: a first
# boot goes PLAY -> movies -> title -> SET SCREEN SIZE -> control method ->
# autosave -> straight into the Prologue race. So this runs the game twice,
# each with a fresh save root (PS3_SAVEDATA_ROOT) under work/runs/main_menu/:
#
#   1. first boot: tap CROSS every 2 s until the log says
#      "[cellSaveData] SAVE complete" (the first-boot save was created through
#      cellSaveDataUserAutoSave), then stop the game.
#   2. second boot, on that save: tap CROSS every 2 s until the second movie
#      (maintitles) has closed, CROSS on "Press Any Button" while the dumped
#      frame shows the title logo, wait until it shows the menu, then DOWN,
#      DOWN, UP. The frame dumped before and after each of
#      those must differ in the menu's right-hand panel (picture and text of
#      the highlighted item), by more than 4x its idle change and at least 5.
#
# The pad is a PS3_PAD_FILE held-state file written by the controller below,
# not a timed PS3_PAD_SCRIPT: how long the movies and loads take varies from
# run to run, and a CROSS that lands on the menu starts the Festival.
#
# PASS: both phases reached their marker, all three presses changed the menu,
# and neither log has an "unresolved NID". The menu frame is saved as
# work/runs/main_menu.png.
# Usage: tests/main_menu.sh        BIN=... overrides the binary.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
RUNS="${RUNS:-$WORK/runs}"; mkdir -p "$RUNS"   # RUNS=<dir> keeps this run's logs and pictures apart
BIN="${BIN:-$WORK/port/build-vk/MotorStormRecomp}"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
OUT="$RUNS/main_menu"
[ -x "$BIN" ] || { echo "FAIL: no binary at $BIN (scripts/build.sh)"; exit 2; }
rm -rf "$OUT"; mkdir -p "$OUT"

python3 -I - "$BIN" "$ELF" "$OUT" "$RUNS/main_menu.png" <<'EOF'
import os, re, signal, subprocess, sys, time, zlib, struct
from pathlib import Path

BIN, ELF, OUT, PNG = sys.argv[1], sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4])
ROOT = Path(ELF).parents[3]          # the project dir (disc/ lives here)

def fail(msg):
    print("FAIL: " + msg); sys.exit(1)

class Game:
    def __init__(self, name, save):
        self.dir = OUT / name
        (self.dir / "d").mkdir(parents=True)
        self.log = self.dir / "log"
        self.pad = self.dir / "pad"
        self.set("")
        env = dict(os.environ, SDL_NO_SIGNAL_HANDLERS="1", SDL_AUDIODRIVER="dummy",
                   SDL_VIDEODRIVER="dummy", PS3_SAVEDATA_ROOT=str(save),
                   PS3_PAD_FILE=str(self.pad), LD_FRAME_DUMP=str(self.dir / "d"),
                   LD_FRAME_DUMP_EVERY="10")
        self.f = open(self.log, "wb")
        self.p = subprocess.Popen([BIN, ELF], cwd=ROOT, env=env, stdout=self.f,
                                  stderr=subprocess.STDOUT, start_new_session=True)
        self.t0 = time.time()
    def set(self, held):
        tmp = self.pad.with_suffix(".tmp")
        tmp.write_text(held + "\n"); os.replace(tmp, self.pad)
    def tap(self, name, secs=0.25):
        self.set(name); time.sleep(secs); self.set("")
    def text(self):
        return self.log.read_bytes().decode("latin1")
    def frames(self):
        return re.findall(r"^\[frame-dump\] (\S+)", self.text(), re.M)
    def alive(self):
        return self.p.poll() is None
    def stop(self):
        if self.alive():
            os.killpg(self.p.pid, signal.SIGTERM)
            try: self.p.wait(5)
            except subprocess.TimeoutExpired:
                os.killpg(self.p.pid, signal.SIGKILL); self.p.wait()
        self.f.close()
    def until(self, cond, limit, tap=None, every=2.0, what=""):
        end = time.time() + limit
        while time.time() < end:
            if not self.alive(): self.stop(); fail(f"the game exited during {what} ({self.log})")
            if cond(): return True
            if tap: self.tap(tap)
            time.sleep(every if tap else 0.5)
        self.stop(); fail(f"no {what} within {limit} s ({self.log})")

def read_ppm(p):
    d = Path(p).read_bytes(); i, f = 0, []
    while len(f) < 4:
        while d[i] in b" \t\r\n": i += 1
        j = i
        while d[j] not in b" \t\r\n": j += 1
        f.append(d[i:j]); i = j
    w, h = int(f[1]), int(f[2])
    return w, h, d[i + 1:i + 1 + w * h * 3]

def menu_diff(a, b):
    """Mean abs difference over the menu's right-hand panel (the picture and
    description of the highlighted item, x 770..1160, y 120..490 at
    1280x720), in 0..255. Not the item column: the highlighted item pulses
    between white and grey on its own."""
    (w, h, pa), (_, _, pb) = read_ppm(a), read_ppm(b)
    x0, x1, y0, y1 = w * 770 // 1280, w * 1160 // 1280, h * 120 // 720, h * 490 // 720
    s = n = 0
    for y in range(y0, y1, 2):
        r = y * w * 3
        ra, rb = pa[r + x0 * 3:r + x1 * 3], pb[r + x0 * 3:r + x1 * 3]
        s += sum(abs(ra[k] - rb[k]) for k in range(0, len(ra), 6)); n += len(ra) // 6
    return s / max(n, 1)

def write_png(p, w, h, px):
    rows = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    ch = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
    p.write_bytes(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                  + ch(b"IDAT", zlib.compress(rows, 6)) + ch(b"IEND", b""))

def unresolved(g):
    return sorted(set(re.findall(r"unresolved NID (0x[0-9A-Fa-f]+)", g.text())))

# -- 1. first boot: create the save --------------------------------------------
save = OUT / "save"; save.mkdir()
g = Game("first_boot", save)
g.until(lambda: "[cellSaveData] SAVE complete" in g.text(), 240, tap="CROSS",
        what="first-boot save (SAVE complete)")
time.sleep(3)
g.stop()
u1 = unresolved(g)
if not (save / "BCES00484" / "PARAM.SFO").exists():
    fail(f"SAVE complete logged but no {save}/BCES00484/PARAM.SFO")

# -- 2. second boot: to the menu and move the cursor ---------------------------
g = Game("second_boot", save)
closes = lambda: len(re.findall(r"^\[cellVdec\] Close", g.text(), re.M))
g.until(lambda: closes() >= 2, 180, tap="CROSS", what="end of the intro movies")

def red_mean(p, x0, x1, y0, y1, minus_blue=False):
    """Mean red (or red minus blue) over a box given at 1280x720 (title logo /
    menu panel tests)."""
    w, h, px = read_ppm(p)
    x0, x1, y0, y1 = w * x0 // 1280, w * x1 // 1280, h * y0 // 720, h * y1 // 720
    s = n = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            o = (y * w + x) * 3
            s += px[o] - (px[o + 2] if minus_blue else 0); n += 1
    return s / max(n, 1)
# "APOC" of the title logo, red minus blue: about 50 on the title, 0 on the
# menu -- whose 3D background, now that it draws, is bright but grey/white
# (plain red read ~210 there and the menu passed for the title).
is_title = lambda f: red_mean(f, 300, 700, 130, 270, minus_blue=True) > 25
# the menu's right-hand panel (sunburst picture), with no title logo.
is_menu = lambda f: not is_title(f) and red_mean(f, 770, 1160, 120, 490) > 10

# "Press Any Button To Continue": it ignores the D-pad, and a CROSS that lands
# on the menu starts the Festival. The CROSS that skipped maintitles has
# sometimes already passed it (it reads the held button), so look first.
end = time.time() + 40
while True:
    if time.time() > end: g.stop(); fail(f"no main menu within 40 s of the movies ({g.log})")
    if not g.alive(): g.stop(); fail(f"the game exited before the menu ({g.log})")
    fr = g.frames()
    if fr and is_title(fr[-1]): g.tap("CROSS"); time.sleep(2); continue
    if fr and is_menu(fr[-1]): break
    time.sleep(0.5)
time.sleep(3)                           # the menu finishes fading in

def frame_now():
    n = len(g.frames()); g.until(lambda: len(g.frames()) > n + 1, 20, what="a new frame")
    return g.frames()[-1]

shots = [frame_now()]
for b in ("DOWN", "DOWN", "UP"):
    g.tap(b, 0.2); time.sleep(1.5); shots.append(frame_now())
still = frame_now()                     # no input: how much the menu animates by itself
alive = g.alive()
g.stop()
u2 = unresolved(g)

w, h, px = read_ppm(shots[0]); write_png(PNG, w, h, px)
noise = menu_diff(shots[-1], still)
diffs = [menu_diff(shots[i], shots[i + 1]) for i in range(3)]
desc = ", ".join(f"{b} {d:.2f}" for b, d in zip(("DOWN", "DOWN", "UP"), diffs))
if u1 or u2:
    fail(f"unresolved NIDs: first boot {u1}, second boot {u2}")
if not alive:
    fail(f"the game exited on the menu ({g.log})")
if min(diffs) < max(5.0, 4 * noise):
    fail(f"the menu did not react to every press: {desc}; idle {noise:.2f} ({shots}) ")
print(f"PASS: save created, menu reached and moved ({desc}; idle {noise:.2f}) -> {PNG}")
EOF
