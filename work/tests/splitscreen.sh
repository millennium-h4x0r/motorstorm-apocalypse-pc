#!/usr/bin/env bash
# Milestone 5: a four-player split-screen race renders.
#
#   1. first boot on a fresh save root: tap CROSS until the first-boot save
#      ("[cellSaveData] SAVE complete"), then stop;
#   2. second boot on that save, four virtual pads (PS3_PAD_FILE_0..3): CROSS
#      through the intro movies and the title, main menu -> WRECKREATION ->
#      QUICK RACE, START on ports 1, 2, 3 (each join is a
#      cellUserInfoSelectUser_ListType), START RACE, CROSS at the fly-by,
#      then full throttle on all four pads.
#
# PASS: three players joined, and in a frame dumped once the race clock
# runs, each of the four quadrant views (x 20..640 / 640..1260, y 20..360 /
# 360..700 of 1280x720) shows a picture -- luma mean in [12, 235], standard
# deviation >= 10 -- and the race clock (float at guest 0x014FC348, as
# sim_speed.sh reads it) moved by at least 3 s over 10 s of wall time. The
# frame is saved as work/runs/splitscreen.png.
#
# Slow (5-10 minutes): run_all.sh runs it only with SLOW=1.
# Usage: tests/splitscreen.sh     BIN=..., RUNS=<dir>.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
RUNS="${RUNS:-$WORK/runs}"; mkdir -p "$RUNS"
BIN="${BIN:-$WORK/port/build-vk/MotorStormRecomp}"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
OUT="$RUNS/splitscreen"
[ -x "$BIN" ] || { echo "FAIL: no binary at $BIN (scripts/build.sh)"; exit 2; }
rm -rf "$OUT"; mkdir -p "$OUT"

python3 -I - "$BIN" "$ELF" "$OUT" "$RUNS/splitscreen.png" <<'EOF'
import os, re, signal, struct, subprocess, sys, time, zlib, shutil
from pathlib import Path

BIN, ELF, OUT, PNG = sys.argv[1], sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4])
ROOT = Path(ELF).parents[3]
CLOCK_EA = 0x014FC348

def fail(msg):
    print("FAIL: " + msg); sys.exit(1)

def hdd1(save):
    d = str(save) + "_hdd1"
    if not os.path.exists(d) and not os.environ.get("FRESH_HDD1") and (ROOT / "hdd1").is_dir():
        shutil.copytree(ROOT / "hdd1", d)
    return d

class Game:
    def __init__(self, name, save, pads):
        self.dir = OUT / name
        (self.dir / "d").mkdir(parents=True)
        self.log = self.dir / "log"
        self.pads = [self.dir / f"pad{n}" for n in range(pads)]
        for n in range(pads): self.set(n, "")
        env = dict(os.environ, SDL_NO_SIGNAL_HANDLERS="1", SDL_AUDIODRIVER="dummy",
                   SDL_VIDEODRIVER="dummy", PS3_SAVEDATA_ROOT=str(save), PS3_HDD1_ROOT=hdd1(save),
                   LD_FRAME_DUMP=str(self.dir / "d"), LD_FRAME_DUMP_EVERY="10",
                   PS3RECOMP_ENG_WORLDLOG="1")
        for n, p in enumerate(self.pads): env[f"PS3_PAD_FILE_{n}"] = str(p)
        self.f = open(self.log, "wb")
        self.p = subprocess.Popen([BIN, ELF], cwd=ROOT, env=env, stdout=self.f,
                                  stderr=subprocess.STDOUT, start_new_session=True)
    def set(self, port, held):
        p = self.pads[port]; tmp = p.with_suffix(".tmp")
        tmp.write_text(held + "\n"); os.replace(tmp, p)
    def tap(self, name, port=0, secs=0.25, after=1.2):
        self.set(port, name); time.sleep(secs); self.set(port, ""); time.sleep(after)
    def text(self): return self.log.read_bytes().decode("latin1")
    def frames(self): return re.findall(r"^\[frame-dump\] (\S+)", self.text(), re.M)
    def alive(self): return self.p.poll() is None
    def stop(self):
        if self.alive():
            os.killpg(self.p.pid, signal.SIGTERM)
            try: self.p.wait(5)
            except subprocess.TimeoutExpired:
                os.killpg(self.p.pid, signal.SIGKILL); self.p.wait()
        self.f.close()
    def until(self, cond, limit, tap=None, what=""):
        end = time.time() + limit
        while time.time() < end:
            if not self.alive(): self.stop(); fail(f"the game exited during {what} ({self.log})")
            if cond(): return
            if tap: self.tap(tap, after=1.75)
            else: time.sleep(0.5)
        self.stop(); fail(f"no {what} within {limit} s ({self.log})")
    def clock(self):
        """The race clock, read out of the running process (sim_speed.sh)."""
        if not hasattr(self, "_vm"):
            sym = int([l for l in subprocess.run(["nm", BIN], capture_output=True, text=True).stdout.splitlines()
                       if l.endswith(" B vm_base")][0].split()[0], 16)
            base = None
            for l in open(f"/proc/{self.p.pid}/maps"):
                f = l.split()
                if len(f) >= 6 and os.path.realpath(f[5]) == os.path.realpath(BIN) and f[2] == "00000000":
                    base = int(f[0].split("-")[0], 16); break
            self._mem = open(f"/proc/{self.p.pid}/mem", "rb", buffering=0)
            self._mem.seek(base + sym); self._vm = struct.unpack("<Q", self._mem.read(8))[0]
        self._mem.seek(self._vm + CLOCK_EA)
        return struct.unpack(">f", self._mem.read(4))[0]

def read_ppm(p):
    d = Path(p).read_bytes(); i, f = 0, []
    while len(f) < 4:
        while d[i] in b" \t\r\n": i += 1
        j = i
        while d[j] not in b" \t\r\n": j += 1
        f.append(d[i:j]); i = j
    w, h = int(f[1]), int(f[2])
    return w, h, d[i + 1:i + 1 + w * h * 3]

def box_stats(w, h, px, x0, x1, y0, y1):
    x0, x1, y0, y1 = w * x0 // 1280, w * x1 // 1280, h * y0 // 720, h * y1 // 720
    s = s2 = n = 0
    for y in range(y0, y1, 3):
        r = y * w * 3
        for x in range(x0, x1, 3):
            o = r + x * 3
            l = 0.299 * px[o] + 0.587 * px[o + 1] + 0.114 * px[o + 2]
            s += l; s2 += l * l; n += 1
    m = s / max(n, 1)
    return m, max(0.0, s2 / max(n, 1) - m * m) ** 0.5

def red_mean(p, x0, x1, y0, y1, minus_blue=False):
    w, h, px = read_ppm(p)
    x0, x1, y0, y1 = w * x0 // 1280, w * x1 // 1280, h * y0 // 720, h * y1 // 720
    s = n = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            o = (y * w + x) * 3
            s += px[o] - (px[o + 2] if minus_blue else 0); n += 1
    return s / max(n, 1)
is_title = lambda f: red_mean(f, 300, 700, 130, 270, minus_blue=True) > 25
is_menu = lambda f: not is_title(f) and red_mean(f, 770, 1160, 120, 490) > 10

def write_png(p, w, h, px):
    rows = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    ch = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
    p.write_bytes(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                  + ch(b"IDAT", zlib.compress(rows, 6)) + ch(b"IEND", b""))

# -- 1. first boot: the save -------------------------------------------------
save = OUT / "save"; save.mkdir()
g = Game("first_boot", save, 1)
g.until(lambda: "[cellSaveData] SAVE complete" in g.text(), 300, tap="CROSS", what="first-boot save")
time.sleep(3); g.stop()

# -- 2. second boot: Quick Race with four players ------------------------------
g = Game("second_boot", save, 4)
try:
    opens = lambda: len(re.findall(r"^\[cellVdec\] Open\(", g.text(), re.M))
    closes = lambda: len(re.findall(r"^\[cellVdec\] Close", g.text(), re.M))
    g.until(lambda: opens() >= 2, 240, tap="CROSS", what="the second intro movie")
    g.until(lambda: closes() >= 2, 120, tap="CROSS", what="end of the intro movies")
    # Title ("Press Any Button"), then the menu; a CROSS on the menu would
    # start the Festival, so look at the frame before pressing.
    end = time.time() + 60
    while True:
        if time.time() > end: fail(f"no main menu within 60 s of the movies ({g.log})")
        fr = g.frames()
        if fr and is_menu(fr[-1]): break
        if fr and is_title(fr[-1]): g.tap("CROSS", after=3.0)
        else: time.sleep(1.0)
    time.sleep(2)
    # A submenu (WRECKREATION, QUICK RACE) has its name in yellow at the top
    # left; the main menu has buildings there.
    is_sub = lambda f: red_mean(f, 140, 440, 70, 115, minus_blue=True) > 50
    def wait_frame(cond, secs, what):
        end = time.time() + secs
        while time.time() < end:
            fr = g.frames()
            if fr and cond(fr[-1]): return
            time.sleep(0.5)
        fail(f"no {what} within {secs} s ({g.log})")
    g.tap("DOWN"); g.tap("CROSS", after=1.0)       # WRECKREATION
    wait_frame(is_sub, 20, "WRECKREATION screen")
    time.sleep(2.0)
    g.tap("CROSS", after=3.0)                      # QUICK RACE (first item)
    joins = lambda: len(re.findall(r"SelectUser_ListType", g.text()))
    for port in (1, 2, 3):                         # players 2-4 join
        want = port
        for attempt in range(4):
            g.tap("START", port=port, after=0.5)
            end = time.time() + 5
            while joins() < want and time.time() < end: time.sleep(0.3)
            if joins() >= want: break
        time.sleep(2.0)
    if joins() < 3: fail(f"{joins()} of 3 players joined (cellUserInfoSelectUser_ListType) ({g.log})")
    # The cursor is on TRACK after the joins; START RACE is two up.
    g.tap("UP"); g.tap("UP"); g.tap("CROSS", after=3.0)
    wf = lambda: [int(a) for a in re.findall(r"world frame (\d+) at frame", g.text())]
    g.until(lambda: any(n >= 200 for n in wf()), 600, what="200 world frames of the race")
    clock0 = g.clock()
    g.tap("CROSS", after=10.0)                     # the fly-by: START RACE
    for n in range(4): g.set(n, "R2=255")
    t_a = time.time(); c_a = g.clock()
    time.sleep(10.0)
    c_b = g.clock(); moved = c_b - c_a
    if not g.alive(): fail(f"the game exited in the race ({g.log})")
    if moved < 3.0:
        fail(f"race clock moved {moved:.2f} s in {time.time() - t_a:.0f} s (from {clock0:.2f}) ({g.log})")
    time.sleep(2.0)
    fr = g.frames()
    if not fr: fail("no frames dumped")
    w, h, px = read_ppm(fr[-1])
    boxes = [(20, 640, 20, 360), (640, 1260, 20, 360), (20, 640, 360, 700), (640, 1260, 360, 700)]
    stats = [box_stats(w, h, px, *b) for b in boxes]
    write_png(PNG, w, h, px)
    desc = ", ".join(f"P{i+1} {m:.0f}/{sd:.0f}" for i, (m, sd) in enumerate(stats))
    bad = [i + 1 for i, (m, sd) in enumerate(stats) if not (12 <= m <= 235 and sd >= 10)]
    if bad: fail(f"views {bad} show no picture (luma mean/sd: {desc}) in {Path(fr[-1]).name} ({PNG})")
    print(f"PASS: 4 players joined, race clock +{moved:.1f} s in 10 s, views luma mean/sd {desc} "
          f"({Path(fr[-1]).name} -> {PNG})")
finally:
    g.stop()
EOF
