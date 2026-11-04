#!/usr/bin/env bash
# The race intro on screen: a first boot on a fresh save goes into the
# Prologue race, whose intro (the opening credits over a fly-by of the
# bridge) draws the 3D world well before GO. The world reaches the screen
# through the post chain -- tonemap into 0x02AA0000, an NV3089 copy to main
# memory for the SPU's EDGE MLAA, an NV3089 copy back into the display buffer,
# then the HUD (docs/NOTES.md "Race rendering").
#
#   1. tap CROSS every 2 s until "[cellSaveData] SAVE complete" (the first-boot
#      save), then for SKIP_S more seconds (the r01_60.avi movie);
#   2. wait for WORLD_FRAMES 3D frames ("[rsx engine] world frame N",
#      PS3RECOMP_ENG_WORLDLOG=1), then for two more frame dumps.
#
# PASS: a presented frame dumped after that point shows a picture in its
# middle -- the central 60% has a mean luma between MIN_LUMA and MAX_LUMA
# (neither the black screen nor the white one the bloom pass once left) and a
# luma standard deviation of at least MIN_STDDEV. That frame is saved as
# work/runs/race_intro.png.
#
# Slow (3-15 minutes, mostly the race load): run_all.sh only runs it with
# SLOW=1. Usage: tests/race_intro.sh     BIN=... overrides the binary,
# LIMIT_S=<s> the time limit (default 1500), WORLD_FRAMES=<n> (default 200).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
BIN="${BIN:-$WORK/port/build-vk/MotorStormRecomp}"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
OUT="$WORK/runs/race_intro"
[ -x "$BIN" ] || { echo "FAIL: no binary at $BIN (scripts/build.sh)"; exit 2; }
rm -rf "$OUT"; mkdir -p "$OUT"

python3 -I - "$BIN" "$ELF" "$OUT" "$WORK/runs" "${LIMIT_S:-1500}" "${SKIP_S:-60}" \
    "${WORLD_FRAMES:-200}" <<'EOF'
import os, re, signal, subprocess, sys, time, zlib, struct
from pathlib import Path

BIN, ELF, OUT, RUNS = sys.argv[1], sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4])
LIMIT, SKIP, WF = float(sys.argv[5]), float(sys.argv[6]), int(sys.argv[7])
MIN_LUMA, MAX_LUMA, MIN_STDDEV = 12.0, 235.0, 12.0
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
           LD_FRAME_DUMP=str(OUT / "d"), LD_FRAME_DUMP_EVERY="50",
           PS3RECOMP_ENG_WORLDLOG="1")
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
    def __init__(self): self.pos, self.rest = 0, b""; self.saves = 0; self.wf = []; self.frames = []
    def poll(self):
        with open(log, "rb") as fh:
            fh.seek(self.pos); chunk = fh.read(); self.pos += len(chunk)
        data = self.rest + chunk; cut = data.rfind(b"\n") + 1
        self.rest = data[cut:]; t = data[:cut].decode("latin1")
        self.saves += t.count("[cellSaveData] SAVE complete")
        self.wf += [(int(a), int(b)) for a, b in re.findall(r"world frame (\d+) at frame (\d+)", t)]
        self.frames += re.findall(r"^\[frame-dump\] (\S+)", t, re.M)
        return self
tail = Tail()

def ppm(path):
    b = Path(path).read_bytes()
    parts = b.split(maxsplit=4)
    w, h = int(parts[1]), int(parts[2])
    return w, h, parts[4][: w * h * 3]

def luma_stats(w, h, px):
    x0, x1, y0, y1 = w // 5, w - w // 5, h // 5, h - h // 5
    s = s2 = n = 0
    for y in range(y0, y1, 4):
        row = px[(y * w) * 3:(y * w + w) * 3]
        for x in range(x0, x1, 4):
            r, g, b = row[x * 3], row[x * 3 + 1], row[x * 3 + 2]
            l = 0.299 * r + 0.587 * g + 0.114 * b
            s += l; s2 += l * l; n += 1
    m = s / n
    return m, max(0.0, s2 / n - m * m) ** 0.5

def png(path, w, h, px):
    raw = b"".join(b"\0" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

t0 = time.time(); t_save = None
try:
    while True:
        if p.poll() is not None: fail(f"the game exited ({log})")
        if time.time() - t0 > LIMIT: fail(f"no {WF} world frames within {LIMIT:.0f} s ({log})")
        if t_save is None and tail.poll().saves >= 1: t_save = time.time()
        if t_save is None or time.time() - t_save < SKIP:
            setp("CROSS"); time.sleep(0.25); setp(""); time.sleep(1.75); continue
        time.sleep(2)
        tail.poll()
        reached = [fr for n, fr in tail.wf if n >= WF]
        if not reached: continue
        after = [fp for fp in tail.frames if int(re.search(r"frame_(\d+)", fp).group(1)) > reached[0]]
        if len(after) < 2: continue
        best = None
        for fp in after:
            w, h, px = ppm(fp)
            m, sd = luma_stats(w, h, px)
            if best is None or sd > best[1]: best = (m, sd, fp, w, h, px)
            if MIN_LUMA <= m <= MAX_LUMA and sd >= MIN_STDDEV:
                png(RUNS / "race_intro.png", w, h, px)
                print(f"PASS: {Path(fp).name} central luma mean {m:.1f}, stddev {sd:.1f} "
                      f"({len(after)} frames after world frame {WF}; {RUNS / 'race_intro.png'})")
                sys.exit(0)
        if len(after) >= 4:
            m, sd, fp, w, h, px = best
            png(RUNS / "race_intro.png", w, h, px)
            fail(f"no picture on screen in the race intro: best {Path(fp).name} central luma "
                 f"mean {m:.1f}, stddev {sd:.1f} ({RUNS / 'race_intro.png'})")
finally:
    stop()
EOF
