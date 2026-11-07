#!/usr/bin/env bash
# Simulation pacing (PS3_SIM_60, docs/NOTES.md "Simulation pacing"): in the
# Prologue race the game clock keeps wall-clock time.
#
# Runs first_race.sh (OUT=$RUNS/sim_speed/race: the first-boot save, the movie,
# full throttle, the button prompts) and, beside it, reads the game's memory
# every 0.5 s (/proc/<pid>/mem): its frame counter (present device +0x270),
# vblank counter (present object +0x6C), the race clock (float 0x14FC348) and
# the frame's dt (int64 us at [[[[0x144FB4C]+0x29C]+0x294]]+0x78). Once the
# race clock has moved for SAMPLE_S seconds of wall time the game is stopped.
#
# Each 4 s window where the race clock moved at every sample (not paused at a
# prompt) and the game read a dt of at least half the wall time between two
# presents (not a load, whose loop runs without presenting) is checked against
# what the clock should do. One frame advances it by min(dt, clamp), the clamp
# being the one the port logs ("[sim60] frame dt clamp": 66.7 ms by default,
# PS3_SIM_DT_MAX_MS; the game's own is 40, func_001DA398). PS3_SIM_60 also
# turns off the game's snap of a 31-35 ms dt to 1/30 s, which would put
# windows near 30 fps 5% out. So at clamp * fps >= 1 the clock must keep wall
# time (0.95..1.05), and below that run at about clamp * fps (0.70..1.10 of
# it: frame times vary inside a window, every frame over the clamp loses time,
# and a window holds a whole number of frames). At least 90% of the windows
# must be in range (a hitch of a second inside a window is not the pacing's
# fault), and the dt the game read must follow the wall frame time: no more
# than half the samples may be exactly 33333 us, the stock fixed step.
# A binary with the port's counters (g_sim_dt_us, g_sim_clamped_us: the frame
# time func_001DA398 got and the part its clamp cut off, ppu_native.cpp) is
# held to them instead: in each window the clock must advance by that frame
# time minus the clamped part, within 5%, so a host hitch the clamp cut is not
# counted against the pacing.
# PASS: at least MIN_WINDOWS windows, those checks, and the vblank handler
# running 50..70 times a second on average (GCM_VBLANK_EACH).
#
# Slow (first_race.sh's boot, ~8 min to GO): run_all.sh only runs it with SLOW=1.
# Usage: tests/sim_speed.sh   BIN=..., SAMPLE_S=<s> (default 300),
#        MIN_WINDOWS=<n> (default 15), RUNS=<dir>.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
export TESTS_DIR="$HERE"   # for the python below: ms3patch.hard_limit
WORK="$(cd "$HERE/.." && pwd)"
RUNS="${RUNS:-$WORK/runs}"
BIN="${BIN:-$WORK/port/build-vk/MotorStormRecomp}"
OUT="$RUNS/sim_speed"
[ -x "$BIN" ] || { echo "FAIL: no binary at $BIN (scripts/build.sh)"; exit 2; }
rm -rf "$OUT"; mkdir -p "$OUT"

# The sampler starts first_race.sh itself: with kernel.yama.ptrace_scope=1 (the
# default) only an ancestor of the game may read its /proc/<pid>/mem, and a
# sampler beside the race got EPERM on every read ("ended after 0 s of race
# clock" while the race itself passed).
python3 -I - "$BIN" "$OUT" "${SAMPLE_S:-300}" "${MIN_WINDOWS:-15}" "$HERE/first_race.sh" <<'EOF'
import os as _o, sys as _s; _s.path.insert(0, _o.environ["TESTS_DIR"]); import ms3patch as _mp; _mp.hard_limit(6000, "sim_speed")
import os, signal, struct, subprocess, sys, time
from pathlib import Path
BIN, OUT, SAMPLE, MINW, FIRST_RACE = sys.argv[1], Path(sys.argv[2]), float(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
save = str(OUT / "race" / "save")
RACE = subprocess.Popen(["bash", FIRST_RACE], stdout=open(OUT / "first_race.out", "wb"), stderr=subprocess.STDOUT,
                        env=dict(os.environ, OUT=str(OUT / "race"), BIN=BIN, LIMIT_S="3600"))

def fail(msg):
    print("FAIL: " + msg); sys.exit(1)

def find_game():
    """The game first_race.sh started: the one whose save root is ours."""
    for d in os.listdir("/proc"):
        if not d.isdigit(): continue
        try:
            if os.readlink(f"/proc/{d}/exe") != os.path.realpath(BIN): continue
            env = open(f"/proc/{d}/environ", "rb").read().split(b"\0")
        except OSError: continue
        if f"PS3_SAVEDATA_ROOT={save}".encode() in env: return int(d)
    return None

class G:
    def __init__(self, pid):
        nm_lines = subprocess.run(["nm", BIN], capture_output=True, text=True).stdout.splitlines()
        sym = int([l for l in nm_lines if l.endswith(" B vm_base")][0].split()[0], 16)
        base = None
        for l in open(f"/proc/{pid}/maps"):
            f = l.split()
            if len(f) >= 6 and os.path.realpath(f[5]) == os.path.realpath(BIN) and f[2] == "00000000":
                base = int(f[0].split("-")[0], 16); break
        self.mem = open(f"/proc/{pid}/mem", "rb", buffering=0)
        self.vm = struct.unpack("<Q", self.hr(base + sym, 8))[0]
        # the port's counts of frame time and of the part the dt clamp cut
        # (ppu_native.cpp nat_dt_clamp_note); absent in older binaries
        syms = {l.split()[2]: int(l.split()[0], 16) for l in nm_lines
                if len(l.split()) == 3 and l.split()[2] in ("g_sim_dt_us", "g_sim_clamped_us")}
        self.cnt = (base + syms["g_sim_dt_us"], base + syms["g_sim_clamped_us"]) if len(syms) == 2 else None
    def counts(self):
        if not self.cnt: return (None, None)
        return tuple(struct.unpack("<Q", self.hr(a, 8))[0] for a in self.cnt)
    def hr(self, a, n): self.mem.seek(a); return self.mem.read(n)
    def rd(self, ea, n): return self.hr(self.vm + ea, n)
    def r32(self, ea): return struct.unpack(">I", self.rd(ea, 4))[0]
    def rf(self, ea): return struct.unpack(">f", self.rd(ea, 4))[0]

def stop(pid):
    for sig, wait in ((signal.SIGTERM, 5), (signal.SIGKILL, 5)):
        try: os.kill(pid, sig)
        except ProcessLookupError: return
        t = time.time()
        while time.time() - t < wait:
            try: os.kill(pid, 0)
            except ProcessLookupError: return
            time.sleep(0.2)

def race_done():
    return RACE.poll() is not None

t0 = time.time(); pid = None
while pid is None:
    if race_done(): fail(f"first_race.sh ended before the game was found ({OUT / 'first_race.out'})")
    if time.time() - t0 > 120: fail("the game did not start")
    pid = find_game(); time.sleep(1)
g = None
samples = []          # (wall, frames, vblanks, clock, dt_us)
moved = 0.0; last = None
log = open(OUT / "samples.txt", "w")
while moved < SAMPLE:
    if race_done(): fail(f"first_race.sh ended after {moved:.0f} s of race clock ({OUT / 'first_race.out'})")
    if time.time() - t0 > 1500 and moved == 0: stop(pid); fail("the race clock never moved within 1500 s")
    time.sleep(0.5)
    try:
        if g is None: g = G(pid)
        obj = g.r32(0x144F150)
        if not obj: continue
        dev = g.r32(obj + 4)
        tm = g.r32(g.r32(g.r32(g.r32(0x144FB4C) + 0x29C) + 0x294))
        if not dev or not tm: continue
        s = (time.time(), g.r32(dev + 0x270), g.r32(obj + 0x6C), g.rf(0x14FC348),
             struct.unpack(">q", g.rd(tm + 0x78, 8))[0]) + g.counts()
    except (OSError, IndexError, struct.error, TypeError):
        continue
    print(*s, file=log, flush=True)
    if last and s[3] != last[3]: moved += s[0] - last[0]
    last = s; samples.append(s)
stop(pid)
while not race_done(): time.sleep(0.5)      # first_race.sh sees the game gone and ends

# the clamp the port set ("[sim60] frame dt clamp <ms> ms"); a binary that
# does not say leaves the game's 40 ms
import re
m = re.search(rb"\[sim60\] frame dt clamp ([0-9.]+) ms", (OUT / "race" / "log").read_bytes())
clamp = (float(m.group(1)) if m else 40.0) / 1000.0
print(f"dt clamp {clamp * 1000:.1f} ms", flush=True)
bad, n, vbl, ok_real = [], 0, [], []
for i in range(0, len(samples) - 8, 8):
    w = samples[i:i + 9]
    if any(b[3] == a[3] for a, b in zip(w, w[1:])): continue     # paused somewhere in it
    T = w[-1][0] - w[0][0]; F = w[-1][1] - w[0][1]
    if T <= 0 or F <= 0: continue
    fps = F / T; ratio = (w[-1][3] - w[0][3]) / T
    dts = sorted(x[4] for x in w)
    if dts[len(dts) // 2] < 0.5e6 / fps: continue    # the loop runs without presenting (loads)
    vbl.append((w[-1][2] - w[0][2]) / T)
    n += 1
    e = min(1.0, clamp * fps)
    if w[0][5] is not None:
        # With the port's counters: the clock must advance by the frame time
        # the game read minus what the clamp cut off, whatever the frame rate
        # (a host hitch is clamped time, not a pacing fault).
        dtsum = (w[-1][5] - w[0][5]) / 1e6; cut = (w[-1][6] - w[0][6]) / 1e6
        e = (dtsum - cut) / T if T > 0 else 1.0
        good = dtsum > 0 and 0.95 * e <= ratio <= 1.05 * e
        if cut == 0: ok_real.append(ratio)
        extra = f" dt/wall={dtsum / T:4.2f} clamped={cut:5.2f}s"
    else:
        lo = 0.95 if e >= 1.0 else 0.70 * e
        good = lo <= ratio <= (1.05 if e >= 1.0 else 1.10 * e)
        if e >= 1.0: ok_real.append(ratio)
        extra = ""
    line = (f"t={w[0][0] - samples[0][0]:6.1f} fps={fps:5.1f} clock/wall={ratio:5.2f} expected={e:4.2f} "
            f"dt={dts[len(dts) // 2]:6d} vbl/s={vbl[-1]:5.1f}{extra}")
    print(line + ("" if good else "  OFF"), flush=True)
    if not good: bad.append(line)
fixed = sum(1 for x in samples if x[4] == 33333) / len(samples)
if fixed > 0.5: fail(f"{fixed:.0%} of the samples have the stock fixed dt 33333 us: not real-time")
if n < MINW: fail(f"only {n} windows with the race clock moving (want {MINW}); {OUT / 'samples.txt'}")
v = sum(vbl) / len(vbl)
if not 50 <= v <= 70: fail(f"vblank handler ran {v:.1f} times a second on average (want ~60)")
if len(bad) > 0.1 * n: fail(f"{len(bad)} of {n} windows off: {bad[0]}")
rt = f", {len(ok_real)} at full speed (median {sorted(ok_real)[len(ok_real) // 2]:.2f})" if ok_real else \
     ", none fast enough for full speed"
print(f"PASS: {n} windows, {len(bad)} off{rt}; vblank {v:.1f}/s -> {OUT}")
EOF
