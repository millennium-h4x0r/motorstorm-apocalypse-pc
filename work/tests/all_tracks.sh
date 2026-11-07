#!/usr/bin/env bash
# Milestone 5 groundwork: load every race track and check each race reaches GO.
#
# The game reads patch archives from /dev_hdd0/game/BCES00484/USRDIR/
# (ms3patchv0101.psarc, ms3patchv0102.psarc) on top of ms3.psarc, by path.
# For each track this builds a one-file patch archive in the track's own
# PS3_HDD0_ROOT. It holds the game's own /data/gamestate/gamestates.xml (read
# from the disc's ms3.psarc at run time), changed in two places:
#   - the "boot -> booted" transition goes to QuickRacing instead of mainmenu;
#   - the "quickrace" GameConfig is set up for one track (World, Environment,
#     Gags, the ticket "Track NN", race.mode, the race GUI) with 3 AI and one
#     local human player, 1 lap.
# So the game boots straight into a quick race on that track: no movies, no
# menus, no save needed. The pad taps CROSS every 3 s (skips the pre-race
# flyover's START RACE).
#
# Per track, PASS when, within LIMIT_S (default 600 s):
#   - the race HUD is up: the yellow "POS" at the bottom left of the frame
#     dump (x 40..220, y 560..640 at 1280x720, more than 100 yellow pixels);
#   - the race clock is running: its box (x 1080..1190, y 45..75) changes
#     between 3 pairs of consecutive dumps while the HUD is up (the clock
#     starts at GO);
#   - Havok tasks keep returning, and the log has no null call, unresolved
#     indirect call or unresolved NID.
# The race picture is kept as $OUT/<track>.png. A summary table is printed and
# written to $OUT/summary.txt.
#
# Usage: tests/all_tracks.sh [track ...]    (default: every track below)
#   BIN=<binary>  RUNS=<dir>  PAR=<n> games at once (default 1)
#   LIMIT_S=<s> per track.  Track names are the worlds, e.g. union_v1.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
RUNS="${RUNS:-$WORK/runs}"; mkdir -p "$RUNS"
BIN="${BIN:-$WORK/port/build-vk/MotorStormRecomp}"
ELF="$WORK/../disc/PS3_GAME/USRDIR/EBOOT.elf"
OUT="$RUNS/all_tracks"
[ -x "$BIN" ] || { echo "FAIL: no binary at $BIN (scripts/build.sh)"; exit 2; }
rm -rf "$OUT"; mkdir -p "$OUT"

python3 -I - "$BIN" "$ELF" "$OUT" "${PAR:-1}" "${LIMIT_S:-600}" "$@" <<'EOF'
import hashlib, os, re, shutil, signal, struct, subprocess, sys, time, zlib
from pathlib import Path

BIN, ELF, OUT, PAR, LIMIT = sys.argv[1], sys.argv[2], Path(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
WANT = sys.argv[6:]
ROOT = Path(ELF).parents[3]
PSARC = ROOT / "disc/PS3_GAME/USRDIR/ms3.psarc"

# ticket, world, environment (from the game's own Track NN tickets).
TRACKS = [
    ("Track 01", "track_DogPatch_v1"), ("Track 02", "track_DogPatch_v2"),
    ("Track 03", "track_DogPatch_v3"), ("Track 04", "track_DogPatch_v4"),
    ("Track 05", "track_pier_v1"), ("Track 06", "track_pier_v2"), ("Track 07", "track_pier_v3"),
    ("Track 12", "track_haight_v1"), ("Track 13", "track_haight_v2"),
    ("Track 14", "track_haight_v3"), ("Track 15", "track_haight_v4"),
    ("Track 16", "track_yerbabuena_v1"), ("Track 17", "track_yerbabuena_v2"),
    ("Track 18", "track_yerbabuena_v3"), ("Track 18a", "track_yerbabuena_v4"),
    ("Track 19", "track_interstate_v1"), ("Track 20", "track_interstate_v2"),
    ("Track 21", "track_interstate_v3"), ("Track 22", "track_interstate_v4"),
    ("Track 23", "track_union_v1"), ("Track 24", "track_union_v2"),
    ("Track 25", "track_union_v3"), ("Track 25b", "track_union_v4"),
    ("Track 26", "track_market_v1"), ("Track 26a", "track_market_v2"), ("Track 26b", "track_market_v3"),
    ("Track 27", "track_mainline_v1"), ("Track 28", "track_mainline_v2"), ("Track 29", "track_mainline_v3"),
    ("Track 31", "track_transbay_v1"), ("Track 32", "track_transbay_v2"),
    ("Track 33", "track_transbay_v3"), ("Track 34", "track_transbay_v4"),
    ("Track 35", "track_vertigo_v1"), ("Track 36", "track_vertigo_v2"), ("Track 37", "track_vertigo_v3"),
    ("Track 38", "track_alcatraz_v1"), ("Track 39", "track_alcatraz_v2"), ("Track 40", "track_alcatraz_v3"),
]
short = lambda world: world[len("track_"):].lower()
if WANT:
    TRACKS = [t for t in TRACKS if short(t[1]) in WANT]
    if not TRACKS: print(f"FAIL: no track matches {WANT}"); sys.exit(2)

# -- PSARC: read one file from ms3.psarc, write a one-file patch archive -------
def psarc_read(path, want):
    f = open(path, "rb")
    _, _, _, tocsz, entsz, nent, blksz, _ = struct.unpack(">4sI4sIIIII", f.read(32))
    toc = f.read(tocsz - 32)
    ents = [(struct.unpack(">I", toc[i*entsz+16:i*entsz+20])[0],
             int.from_bytes(toc[i*entsz+20:i*entsz+25], "big"),
             int.from_bytes(toc[i*entsz+25:i*entsz+30], "big")) for i in range(nent)]
    rest = toc[nent * entsz:]
    bsz = [int.from_bytes(rest[i:i+2], "big") for i in range(0, len(rest) - 1, 2)]
    def get(e):
        idx, usz, off = e; out = b""; f.seek(off)
        while len(out) < usz:
            d = f.read(bsz[idx] or blksz); idx += 1
            try: d = zlib.decompress(d)
            except zlib.error: pass
            out += d
        return out[:usz]
    names = get(ents[0]).decode("latin1").split("\n")
    return get(ents[1 + names.index(want)])

def psarc_write(out, name, data, BLK=65536):
    datas = [name.encode(), data]
    md5s = [b"\0" * 16, hashlib.md5(name.upper().encode()).digest()]
    blocks, ents = [], []
    for d in datas:
        idx = len(blocks); ch = []
        for o in range(0, max(len(d), 1), BLK):
            raw = d[o:o + BLK]; z = zlib.compress(raw, 9)
            ch.append(z if len(z) < len(raw) else raw)
        ents.append((idx, len(d), ch)); blocks += ch
    toc = 32 + len(datas) * 30 + len(blocks) * 2
    off, body, te = toc, b"", b""
    for (idx, usz, ch), m in zip(ents, md5s):
        te += m + struct.pack(">I", idx) + usz.to_bytes(5, "big") + off.to_bytes(5, "big")
        for c in ch: body += c; off += len(c)
    bs = b"".join((0 if len(c) == BLK else len(c)).to_bytes(2, "big") for c in blocks)
    Path(out).write_bytes(struct.pack(">4sI4sIIIII", b"PSAR", 0x00010004, b"zlib", toc, 30,
                                      len(datas), BLK, 3) + te + bs + body)

STOCK = psarc_read(PSARC, "/data/gamestate/gamestates.xml").decode("utf-8")

def gamestates(ticket, world):
    env = "sky_" + world[len("track_"):]
    gags = ticket.lower().replace(" ", "_")
    qr = f'''<GameConfig Name="quickrace" ticket="{ticket}">
    <GUIResource name="Art/UI_Art_BE.rpk"/>
    <LoadCommonPausedGUIResource setting="false"/>
    <IsRaceConfig setting="true"/>
    <GameMode name="race.mode"/>
    <World name="{world}"/>
    <Environment name="{env}"/>
    <Gags name="{gags}"/>
    <Player name="AI1" vehicle="buggy41_LV01" character="m_pro_09_LV01" control="AI" location="LOCAL" viewport="-1"/>
    <Player name="AI2" vehicle="rallycar41_LV01" character="m_pro_01_LV01" control="AI" location="LOCAL" viewport="-1"/>
    <Player name="AI3" vehicle="supercar41_LV01" character="m_pro_11_LV01" control="AI" location="LOCAL" viewport="-1"/>
    <Player name="Player" vehicle="musclecar41_LV01" character="m_pro_04_LV01" control="HUMAN" location="LOCAL" viewport="0"/>
    <RaceSettings racetype="Standard" laps="1"/>
  </GameConfig>'''
    s, n1 = re.subn(r'<GameConfig Name="quickrace" base="race">.*?</GameConfig>', qr, STOCK, flags=re.S)
    s, n2 = re.subn(r'<Transition FromState="boot" Name="booted" ToState="mainmenu"[^>]*/>',
                    '<Transition FromState="boot" Name="booted" ToState="QuickRacing"/>', s)
    assert n1 == 1 and n2 == 1, "gamestates.xml changed shape"
    return s.encode("utf-8")

# -- frame tests ---------------------------------------------------------------
def read_ppm(p):
    d = Path(p).read_bytes(); i, f = 0, []
    while len(f) < 4:
        while d[i] in b" \t\r\n": i += 1
        j = i
        while d[j] not in b" \t\r\n": j += 1
        f.append(d[i:j]); i = j
    w, h = int(f[1]), int(f[2])
    return w, h, d[i + 1:i + 1 + w * h * 3]

def hud(px, w, h):
    """Yellow 'POS' at the bottom left."""
    n = 0
    for y in range(h * 560 // 720, h * 640 // 720, 2):
        for x in range(w * 40 // 1280, w * 220 // 1280, 2):
            o = (y * w + x) * 3
            if px[o] > 200 and px[o+1] > 150 and px[o+2] < 90: n += 1
    return n > 25

def clock(px, w, h):
    return bytes(px[(y * w + x) * 3] for y in range(h * 45 // 720, h * 75 // 720)
                 for x in range(w * 1080 // 1280, w * 1190 // 1280))

def write_png(p, w, h, px):
    rows = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    ch = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
    Path(p).write_bytes(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                        + ch(b"IDAT", zlib.compress(rows, 6)) + ch(b"IEND", b""))

BAD = re.compile(r"^\[null-call\]|unresolved indirect call|unresolved NID|"
                 r"guest_call: OPD .* not registered", re.M)

class Run:
    def __init__(self, ticket, world):
        self.name = short(world); self.ticket = ticket
        self.dir = OUT / self.name; d = self.dir
        usr = d / "hdd0/game/BCES00484/USRDIR"; usr.mkdir(parents=True)
        psarc_write(usr / "ms3patchv0101.psarc", "/data/gamestate/gamestates.xml", gamestates(ticket, world))
        (d / "save").mkdir(); (d / "d").mkdir()
        shutil.copytree(ROOT / "hdd1", d / "hdd1") if (ROOT / "hdd1").is_dir() else (d / "hdd1").mkdir()
        self.log, self.pad = d / "log", d / "pad"
        self.set("")
        env = dict(os.environ, SDL_NO_SIGNAL_HANDLERS="1", SDL_AUDIODRIVER="dummy", SDL_VIDEODRIVER="dummy",
                   PS3_SAVEDATA_ROOT=str(d / "save"), PS3_HDD1_ROOT=str(d / "hdd1"),
                   PS3_HDD0_ROOT=str(d / "hdd0"), PS3_PAD_FILE=str(self.pad),
                   LD_FRAME_DUMP=str(d / "d"), LD_FRAME_DUMP_EVERY="30")
        self.f = open(self.log, "wb")
        self.p = subprocess.Popen([BIN, ELF], cwd=ROOT, env=env, stdout=self.f,
                                  stderr=subprocess.STDOUT, start_new_session=True)
        self.t0 = time.time(); self.next_tap = self.t0 + 20
        self.seen = 0; self.hud_t = None; self.ticks = 0; self.last = None
        self.havok0 = None; self.result = None; self.shot = None
    def set(self, held):
        tmp = self.pad.with_suffix(".tmp"); tmp.write_text(held + "\n"); os.replace(tmp, self.pad)
    def text(self):
        return self.log.read_bytes().decode("latin1")
    def havok(self, t):
        return len(re.findall(r"async image=3[3-5] task=\d+ RETURNED", t))
    def stop(self):
        if self.p.poll() is None:
            os.killpg(self.p.pid, signal.SIGTERM)
            try: self.p.wait(5)
            except subprocess.TimeoutExpired:
                os.killpg(self.p.pid, signal.SIGKILL); self.p.wait()
        self.f.close()
        for fr in self.dir.joinpath("d").glob("*.ppm"):   # keep the disk use down
            if str(fr) != self.shot: fr.unlink()
    def finish(self, result):
        self.result = result; self.stop()
        if self.shot:
            w, h, px = read_ppm(self.shot); write_png(OUT / f"{self.name}.png", w, h, px)
    def poll(self):
        now = time.time(); el = now - self.t0
        t = self.text()
        m = BAD.search(t)
        if m: return self.finish(f"FAIL {m.group(0).strip()!r} at {el:.0f} s")
        if self.p.poll() is not None: return self.finish(f"FAIL the game exited at {el:.0f} s")
        if self.hud_t is None and now >= self.next_tap:       # START RACE etc.
            self.set("CROSS"); time.sleep(0.25); self.set(""); self.next_tap = now + 3
        frames = sorted(self.dir.joinpath("d").glob("frame_*.ppm"))
        for fr in frames[self.seen:-1]:                        # the last may still be written
            w, h, px = read_ppm(fr)
            if hud(px, w, h):
                if self.hud_t is None:
                    self.hud_t = el; self.havok0 = self.havok(t)
                c = clock(px, w, h)
                if self.last is not None and c != self.last: self.ticks += 1
                self.last = c; self.shot = str(fr)
            else:
                self.last = None
        self.seen = max(self.seen, len(frames) - 1)
        if self.ticks >= 3:
            if self.havok(t) <= (self.havok0 or 0):
                return self.finish(f"FAIL HUD and clock but no Havok task since the HUD ({el:.0f} s)")
            return self.finish(f"PASS GO at <= {el:.0f} s (HUD at {self.hud_t:.0f} s)")
        if el > LIMIT:
            what = "no race HUD" if self.hud_t is None else "the race clock never ran"
            nb = "black" if not frames else "pictures"
            return self.finish(f"FAIL {what} within {LIMIT} s ({len(frames)} dumps, {nb})")

pending = list(TRACKS); running = []; done = []
while pending or running:
    while pending and len(running) < PAR:
        r = Run(*pending.pop(0)); running.append(r)
        print(f"start {r.name} ({r.ticket})", flush=True)
    time.sleep(2)
    for r in list(running):
        r.poll()
        if r.result:
            running.remove(r); done.append(r)
            print(f"{r.name:18s} {r.result}  ({r.log})", flush=True)

lines = [f"{r.name:18s} {r.ticket:10s} {r.result}" for r in done]
(OUT / "summary.txt").write_text("\n".join(lines) + "\n")
fails = [r for r in done if not r.result.startswith("PASS")]
print("\n".join(lines))
if fails:
    print(f"FAIL: {len(fails)}/{len(done)} tracks did not reach GO: {', '.join(r.name for r in fails)}")
    sys.exit(1)
print(f"PASS: {len(done)}/{len(done)} tracks reached GO")
EOF
