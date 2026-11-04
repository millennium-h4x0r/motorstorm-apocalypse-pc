#!/usr/bin/env bash
# Milestone 2: the first real picture. Boot headless with the RSX frame dump
# on (rsx_live_draw.c: LD_FRAME_DUMP=<dir> writes the PRESENTED surface as a
# .ppm every LD_FRAME_DUMP_EVERY flips) and look at what was presented.
#
# PASS: some dumped frame is not one flat colour -- at least MIN_COLOURS
# distinct colours and a luma standard deviation of at least MIN_STDDEV. The
# first such frame is saved as work/runs/first_frame.png.
# Usage: tests/first_frame.sh [seconds]     BIN=... overrides the binary,
#        EVERY=<n> the dump interval (default 10 flips).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
BIN="${BIN:-$WORK/port/build-vk/MotorStormRecomp}"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
SECS="${1:-120}"
EVERY="${EVERY:-10}"
mkdir -p "$WORK/runs"
LOG="$WORK/runs/first_frame.log"
DIR="$WORK/runs/first_frame_dump"
rm -rf "$DIR"; mkdir -p "$DIR"

[ -x "$BIN" ] || { echo "FAIL: no binary at $BIN (scripts/build.sh)"; exit 2; }
LD_FRAME_DUMP="$DIR" LD_FRAME_DUMP_EVERY="$EVERY" \
SDL_NO_SIGNAL_HANDLERS=1 SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy \
    timeout -k 5 "$SECS" "$BIN" "$ELF" > "$LOG" 2>&1
rc=$?

grep -aq '^\[ppu\] run: code 0x' "$LOG" ||
    { echo "FAIL: the guest entry was never dispatched (status $rc, $LOG)"; exit 1; }
case "$rc" in 0|124) ;; *) echo "FAIL: exited with status $rc ($LOG)"; exit 3 ;; esac

python3 -I - "$DIR" "$WORK/runs/first_frame.png" "$(grep -ac '^\[frame-dump\]' "$LOG")" <<'EOF'
import math, struct, sys, zlib
from pathlib import Path

MIN_COLOURS, MIN_STDDEV = 16, 4.0
src, dst, ndumped = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]

def read_ppm(p):
    d = p.read_bytes()
    i, f = 0, []
    while len(f) < 4:
        while d[i] in b" \t\r\n": i += 1
        j = i
        while d[j] not in b" \t\r\n": j += 1
        f.append(d[i:j]); i = j
    if f[0] != b"P6": return None
    w, h = int(f[1]), int(f[2])
    return w, h, d[i + 1:i + 1 + w * h * 3]

def stats(w, h, px):
    step = max(1, (w * h) // 200000)          # sample ~200k pixels
    cols, n, s, s2 = set(), 0, 0.0, 0.0
    for k in range(0, w * h, step):
        r, g, b = px[3 * k], px[3 * k + 1], px[3 * k + 2]
        cols.add((r, g, b))
        y = 0.299 * r + 0.587 * g + 0.114 * b
        n += 1; s += y; s2 += y * y
    mean = s / n
    return len(cols), math.sqrt(max(0.0, s2 / n - mean * mean)), mean

def write_png(p, w, h, px):
    rows = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    ch = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
    p.write_bytes(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                  + ch(b"IDAT", zlib.compress(rows, 6)) + ch(b"IEND", b""))

frames = sorted(src.glob("frame_*.ppm"))
if not frames:
    print(f"FAIL: no frame was dumped ({ndumped} [frame-dump] lines; no flip reached the presenter?)")
    sys.exit(1)
best = None
for p in frames:
    r = read_ppm(p)
    if not r: continue
    nc, sd, mean = stats(*r)
    if best is None or sd > best[2]: best = (p, nc, sd, mean)
    if nc >= MIN_COLOURS and sd >= MIN_STDDEV:
        write_png(dst, *r)
        print(f"PASS: {p.name} {r[0]}x{r[1]}: {nc} colours, luma mean {mean:.1f} sd {sd:.1f} "
              f"({len(frames)} frames dumped) -> {dst}")
        sys.exit(0)
p, nc, sd, mean = best
print(f"FAIL: {len(frames)} frames dumped, all flat; best {p.name}: {nc} colours, "
      f"luma mean {mean:.1f} sd {sd:.1f}")
sys.exit(1)
EOF
