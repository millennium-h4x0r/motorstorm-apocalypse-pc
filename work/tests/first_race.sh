#!/usr/bin/env bash
# Milestone 4: one race, start to end. A first boot with no save goes from
# "Choose A Control Method" straight into the Prologue race ("Mash's
# Prologue"), so this is one boot on a fresh save root
# (PS3_SAVEDATA_ROOT under work/runs/first_race/):
#
#   1. tap CROSS every 2 s until "[cellSaveData] SAVE complete" (the first-boot
#      save: control method chosen), then keep tapping for SKIP_S seconds to
#      pass the r01_60.avi movie;
#   2. hold full throttle (R2=255: the default control method accelerates on
#      R2, and the game turns pad pressure on with cellPadSetPortSetting
#      (port, 0x6), so the value is analog) until the race is over.
#
# The race is over when the game autosaves its progress a second time
# ("[cellSaveData] SAVE complete" again, after the Prologue result). That
# this marks the end of the event is an assumption until a run gets there
# (docs/NOTES.md "Race").
#
# PASS: the second save happened within LIMIT_S seconds of the first, the
# game kept running, the physics kept going (Havok task runs keep being
# logged) and no "unresolved NID" was logged. The last dumped frame is saved
# as work/runs/first_race.png and an in-race frame as
# work/runs/first_race_inrace.png.
#
# Slow: run_all.sh only runs it with SLOW=1.
# Usage: tests/first_race.sh        BIN=... overrides the binary,
#        LIMIT_S=<s> the race time limit (default 3600).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
BIN="${BIN:-$WORK/port/build-vk/MotorStormRecomp}"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
OUT="$WORK/runs/first_race"
[ -x "$BIN" ] || { echo "FAIL: no binary at $BIN (scripts/build.sh)"; exit 2; }
rm -rf "$OUT"; mkdir -p "$OUT"

python3 -I - "$BIN" "$ELF" "$OUT" "$WORK/runs" "${LIMIT_S:-3600}" "${SKIP_S:-60}" <<'EOF'
import os, re, signal, subprocess, sys, time, zlib, struct
from pathlib import Path

BIN, ELF, OUT, RUNS = sys.argv[1], sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4])
LIMIT, SKIP = float(sys.argv[5]), float(sys.argv[6])
ROOT = Path(ELF).parents[3]

def fail(msg):
    print("FAIL: " + msg); sys.exit(1)

save = OUT / "save"; save.mkdir()
(OUT / "d").mkdir()
log, pad = OUT / "log", OUT / "pad"
def setp(held):
    tmp = pad.with_suffix(".tmp"); tmp.write_text(held + "\n"); os.replace(tmp, pad)
setp("")
env = dict(os.environ, SDL_NO_SIGNAL_HANDLERS="1", SDL_AUDIODRIVER="dummy",
           SDL_VIDEODRIVER="dummy", PS3_SAVEDATA_ROOT=str(save), PS3_PAD_FILE_0=str(pad),
           LD_FRAME_DUMP=str(OUT / "d"), LD_FRAME_DUMP_EVERY="150")
f = open(log, "wb")
p = subprocess.Popen([BIN, ELF], cwd=ROOT, env=env, stdout=f, stderr=subprocess.STDOUT,
                     start_new_session=True)
def stop():
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGTERM)
        try: p.wait(5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL); p.wait()
    f.close()
class Tail:
    """What the log has said so far, read incrementally (it grows to
    hundreds of MB over a race)."""
    def __init__(self):
        self.pos, self.rest = 0, b""
        self.saves = self.havok = 0
        self.frames, self.nids = [], set()
    def poll(self):
        with open(log, "rb") as fh:
            fh.seek(self.pos); chunk = fh.read(); self.pos += len(chunk)
        data = self.rest + chunk
        cut = data.rfind(b"\n") + 1
        self.rest = data[cut:]
        t = data[:cut].decode("latin1")
        self.saves += t.count("[cellSaveData] SAVE complete")
        self.havok += len(re.findall(r"async image=3[3-5] task=\d+ RETURNED", t))
        self.frames += re.findall(r"^\[frame-dump\] (\S+)", t, re.M)
        self.nids |= set(re.findall(r"unresolved NID (0x[0-9A-Fa-f]+)", t))
        return self
tail = Tail()
saves = lambda: tail.poll().saves
havok_runs = lambda: tail.poll().havok
frames = lambda: tail.poll().frames

t0 = time.time()
# -- 1. first boot to the first save, then past the movie ----------------------
t_save = None
while True:
    if p.poll() is not None: stop(); fail(f"the game exited before the first save ({log})")
    if time.time() - t0 > 300: stop(); fail(f"no first-boot save within 300 s ({log})")
    if saves() >= 1:
        t_save = time.time(); break
    setp("CROSS"); time.sleep(0.25); setp(""); time.sleep(1.75)
while time.time() - t_save < SKIP:
    setp("CROSS"); time.sleep(0.25); setp(""); time.sleep(1.75)

# -- 2. full throttle until the race is over -----------------------------------
setp("R2=255")
hist = []                          # (time, frames dumped so far)
last_h, last_h_t = havok_runs(), time.time()
while True:
    if p.poll() is not None: stop(); fail(f"the game exited during the race ({log})")
    el = time.time() - t_save
    if saves() >= 2: break
    if el > LIMIT: stop(); fail(f"the race did not end within {LIMIT:.0f} s ({log})")
    h = havok_runs()
    if h != last_h: last_h, last_h_t = h, time.time()
    elif h > 0 and time.time() - last_h_t > 120:
        stop(); fail(f"the physics stopped (no Havok task for 120 s) at {el:.0f} s ({log})")
    fr = frames()
    hist.append((time.time(), len(fr)))
    time.sleep(2)
t_end = time.time()
time.sleep(10)                    # let the result screen settle
fr = frames()
alive = p.poll() is None
stop()
# The in-race picture: the frame dumped about three minutes before the end.
n_then = [n for t, n in hist if t <= t_end - 180]
inrace = fr[n_then[-1] - 1] if n_then and n_then[-1] > 0 else None

def read_ppm(p):
    d = Path(p).read_bytes(); i, fl = 0, []
    while len(fl) < 4:
        while d[i] in b" \t\r\n": i += 1
        j = i
        while d[j] not in b" \t\r\n": j += 1
        fl.append(d[i:j]); i = j
    w, h = int(fl[1]), int(fl[2])
    return w, h, d[i + 1:i + 1 + w * h * 3]
def write_png(dst, src):
    w, h, px = read_ppm(src)
    rows = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    ch = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
    dst.write_bytes(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                    + ch(b"IDAT", zlib.compress(rows, 6)) + ch(b"IEND", b""))
if fr: write_png(RUNS / "first_race.png", fr[-1])
if inrace: write_png(RUNS / "first_race_inrace.png", inrace)

u = sorted(tail.poll().nids)
if u: fail(f"unresolved NIDs {u}")
if not alive: fail(f"the game exited on the result screen ({log})")
print(f"PASS: race finished {t_end - t_save:.0f} s after the first save "
      f"({havok_runs()} Havok task runs) -> {RUNS / 'first_race.png'}")
EOF
