"""Helpers for tests that boot MotorStorm Apocalypse through a patch archive.

The game mounts /dev_hdd0/game/BCES00484/USRDIR/ms3patchv0101.psarc (and
...0102) over ms3.psarc; its files replace the base archive's by path. Tests
write one there, in their own PS3_HDD0_ROOT, holding the game's own files
(read from the disc at run time) with small edits. See docs/NOTES.md "Every
track". No game data is kept in the repo.
"""
import hashlib
import struct
import zlib
from pathlib import Path


def psarc_read(path, want):
    """One file from a PSARC (v1.4, zlib, as ms3.psarc)."""
    f = open(path, "rb")
    _, _, _, tocsz, entsz, nent, blksz, _ = struct.unpack(">4sI4sIIIII", f.read(32))
    toc = f.read(tocsz - 32)
    ents = [(struct.unpack(">I", toc[i*entsz+16:i*entsz+20])[0],
             int.from_bytes(toc[i*entsz+20:i*entsz+25], "big"),
             int.from_bytes(toc[i*entsz+25:i*entsz+30], "big")) for i in range(nent)]
    rest = toc[nent * entsz:]
    bsz = [int.from_bytes(rest[i:i+2], "big") for i in range(0, len(rest) - 1, 2)]

    def get(e):
        idx, usz, off = e
        out = b""
        f.seek(off)
        while len(out) < usz:
            d = f.read(bsz[idx] or blksz)
            idx += 1
            try:
                d = zlib.decompress(d)
            except zlib.error:
                pass
            out += d
        return out[:usz]
    names = get(ents[0]).decode("latin1").split("\n")
    return get(ents[1 + names.index(want)])


def psarc_write(out, files, BLK=65536):
    """files: {"/path/in/archive": bytes}. Flags 3 (ignore case, absolute)."""
    names = list(files)
    datas = ["\n".join(names).encode()] + [files[n] for n in names]
    md5s = [b"\0" * 16] + [hashlib.md5(n.upper().encode()).digest() for n in names]
    blocks, ents = [], []
    for d in datas:
        idx, ch = len(blocks), []
        for o in range(0, max(len(d), 1), BLK):
            raw = d[o:o + BLK]
            z = zlib.compress(raw, 9)
            ch.append(z if len(z) < len(raw) else raw)
        ents.append((idx, len(d), ch))
        blocks += ch
    toc = 32 + len(datas) * 30 + len(blocks) * 2
    off, body, te = toc, b"", b""
    for (idx, usz, ch), m in zip(ents, md5s):
        te += m + struct.pack(">I", idx) + usz.to_bytes(5, "big") + off.to_bytes(5, "big")
        for c in ch:
            body += c
            off += len(c)
    bs = b"".join((0 if len(c) == BLK else len(c)).to_bytes(2, "big") for c in blocks)
    Path(out).write_bytes(struct.pack(">4sI4sIIIII", b"PSAR", 0x00010004, b"zlib", toc, 30,
                                      len(datas), BLK, 3) + te + bs + body)


def disc_file(root, path):
    return psarc_read(Path(root) / "disc/PS3_GAME/USRDIR/ms3.psarc", path)


def stock_gamestates(root):
    return disc_file(root, "/data/gamestate/gamestates.xml").decode("utf-8")


def autopilot_hud(root):
    """The race HUD (/ui/be/hud_definition.xml) with every competitor that
    has not finished on AutoPilot (the competitor databinding the game uses
    after a finish), set on every HUD update: the local player's car drives
    the race by itself. Setting it after the finish too kept
    AllLocalHumansFinished false and the HUD on YOU FINISHED forever."""
    s = disc_file(root, "/ui/be/hud_definition.xml").decode("utf-8-sig")
    tag = '<Screen type="HUD" name="GameHUD">'
    assert s.count(tag) == 1, "hud_definition.xml changed shape"
    case = "".join(
        f'\n\t<Case if_databinding="CompetitorManager/Competitors[{i}]/Finished" is_value="false">'
        f'<DataBinding databinding="CompetitorManager/Competitors[{i}]/AutoPilot" value="true" /></Case>'
        for i in range(16))
    return s.replace(tag, tag + case).encode("utf-8")


# -- frame dumps ----------------------------------------------------------------
def read_ppm(p):
    d = Path(p).read_bytes()
    i, f = 0, []
    while len(f) < 4:
        while d[i] in b" \t\r\n":
            i += 1
        j = i
        while d[j] not in b" \t\r\n":
            j += 1
        f.append(d[i:j])
        i = j
    w, h = int(f[1]), int(f[2])
    return w, h, d[i + 1:i + 1 + w * h * 3]


def _box(w, h, box):
    x0, x1, y0, y1 = box
    return w * x0 // 1280, w * x1 // 1280, h * y0 // 720, h * y1 // 720


def count(px, w, h, box, pred, step=2):
    """Pixels in a box (given at 1280x720) for which pred(r, g, b) holds,
    sampling every step-th pixel each way."""
    x0, x1, y0, y1 = _box(w, h, box)
    n = 0
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            o = (y * w + x) * 3
            if pred(px[o], px[o + 1], px[o + 2]):
                n += 1
    return n


def crop(px, w, h, box):
    x0, x1, y0, y1 = _box(w, h, box)
    return b"".join(px[(y * w + x0) * 3:(y * w + x1) * 3] for y in range(y0, y1))


def diff(a, b):
    """Mean absolute difference of two equal crops, 0..255."""
    return sum(abs(x - y) for x, y in zip(a, b)) / max(len(a), 1)


def write_png(p, w, h, px):
    rows = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def ch(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
    Path(p).write_bytes(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                        + ch(b"IDAT", zlib.compress(rows, 6)) + ch(b"IEND", b""))


def psarc_ranges(path, prefix=""):
    """[(start, end, name)] byte ranges of the archive's files (compressed,
    as stored), for the names starting with prefix: maps a read offset in
    ms3.psarc (cellFsLseek, logged with FS_CALLER=1) back to the file."""
    f = open(path, "rb")
    _, _, _, tocsz, entsz, nent, blksz, _ = struct.unpack(">4sI4sIIIII", f.read(32))
    toc = f.read(tocsz - 32)
    ents = [(struct.unpack(">I", toc[i*entsz+16:i*entsz+20])[0],
             int.from_bytes(toc[i*entsz+20:i*entsz+25], "big"),
             int.from_bytes(toc[i*entsz+25:i*entsz+30], "big")) for i in range(nent)]
    rest = toc[nent * entsz:]
    bsz = [int.from_bytes(rest[i:i+2], "big") or blksz for i in range(0, len(rest) - 1, 2)]
    idx0, usz0, off0 = ents[0]
    nblk = lambda usz: max(1, (usz + blksz - 1) // blksz)
    f.seek(off0)
    man = b"".join(zlib.decompress(c) if c[:1] == b"\x78" else c
                   for c in (f.read(bsz[i]) for i in range(idx0, idx0 + nblk(usz0))))[:usz0]
    names = man.decode("latin1").split("\n")
    out = []
    for name, (idx, usz, off) in zip(names, ents[1:]):
        if name.startswith(prefix):
            out.append((off, off + sum(bsz[idx:idx + nblk(usz)]), name))
    return sorted(out)


def allow_ptrace():
    """preexec_fn: let any process of this user attach (Yama ptrace_scope=1
    only lets a parent): a hung game can then be backtraced with gdb -p.
    PR_SET_PTRACER survives the exec."""
    import ctypes
    ctypes.CDLL(None).prctl(0x59616d61, ctypes.c_ulong(0xffffffffffffffff), 0, 0, 0)


def host_backtrace(pid, out):
    """Every host thread's stack (lifted guest functions show as func_<addr>,
    so this is the guest call chain too) into the file out."""
    import subprocess, re
    with open(out, "w") as f:
        subprocess.run(["gdb", "-p", str(pid), "-batch", "-ex", "set pagination off",
                        "-ex", "p/x (unsigned long)vm_base",
                        "-ex", "thread apply all bt 40"], stdout=f, stderr=subprocess.STDOUT, timeout=300)
    try:
        with open(out) as f:
            mb = re.search(r"^\$1 = (0x[0-9a-f]+)", f.read(), re.M)
        if mb:
            with open(str(out) + ".jobqueue", "w") as f:
                f.write(havok_queue_dump(pid, int(mb.group(1), 16)))
    except Exception as e:  # diagnostics only
        with open(str(out) + ".jobqueue", "w") as f:
            f.write(f"no dump: {e}\n")


def havok_queue_dump(pid, vm_base, Q=0x14700F90):
    """The Havok hkJobQueue at Q and its dynamic data D=[Q+0x30] (592 bytes,
    what the SPU tasks GET under the CellSyncMutex at Q), read from a live
    game: D+2t active jobs of type t (halfwords), D+0x18 mask, D+0x1C wait
    flag, D+0x22+2q tasks waiting on queue q's semaphore [Q+0x90+4q], queue q
    at D+0x40+20q = {elements, capacity, head, tail, count}. Text."""
    import struct
    with open(f"/proc/{pid}/mem", "rb") as m:
        def rd(ea, n):
            m.seek(vm_base + ea); return m.read(n)
        q = rd(Q, 0x100)
        D = struct.unpack(">I", q[0x30:0x34])[0]
        d = rd(D, 0x250)
        sems = rd(0x146F7800, 0x200)   # the Havok semaphores (queue ones, and main's batch one at 0x146F7840)
        # The last raycast (queue 3) job and its batch header: the 128-byte
        # slot [D+0x7C] + 128*((tail-1) mod cap); its +8 is the semaphore it
        # posts, +0xC the header whose word 0 counts the open sub-jobs (the
        # finisher that takes it to 0 posts), +0x10 a done counter.
        g = lambda o: struct.unpack(">I", d[o:o + 4])[0]
        cap3 = g(0x80) or 1
        job3 = (g(0x7C) + 128 * ((g(0x88) - 1) % cap3)) & 0xFFFFFFFF
        j3 = rd(job3, 128)
        hdr3 = struct.unpack(">I", j3[0xC:0x10])[0]
        h3 = rd(hdr3, 16) if 0x10000 <= hdr3 < 0xC0000000 else b""
        cnt3 = struct.unpack(">I", j3[0x10:0x14])[0]
        c3 = rd(cnt3, 4) if 0x10000 <= cnt3 < 0xC0000000 else b""
    W = lambda b, o: struct.unpack(">I", b[o:o + 4])[0]
    H = lambda o: struct.unpack(">H", d[o:o + 2])[0]
    s = [f"vm_base {vm_base:#x} queue {Q:08X} mutex {W(q, 0):08X} rec {W(q, 4)} owner {W(q, 8):08X}{W(q, 12):08X} D {D:08X}"]
    s.append(f"active[t] {[H(2 * t) for t in range(10)]}  mask {W(d, 0x18):X}  waitflag {W(d, 0x1C)}")
    s.append(f"waiting[q] {[H(0x22 + 2 * k) for k in range(12)]}")
    for k in range(22):
        o = 0x40 + 20 * k
        s.append(f"queue {k:2d} sem {W(q, 0x90 + 4 * k) if k < 12 else 0:08X} elems {W(d, o):08X} cap {W(d, o + 4)} "
                 f"head {W(d, o + 8)} tail {W(d, o + 12)} count {W(d, o + 16)}")
    s += [f"Q+{o:03X}: " + " ".join(f"{W(q, o + i):08X}" for i in range(0, 16, 4)) for o in range(0, 0x100, 0x10)]
    s += [f"D+{o:03X}: " + " ".join(f"{W(d, o + i):08X}" for i in range(0, 16, 4)) for o in range(0, 0x250, 0x10)]
    s.append(f"last queue-3 job {job3:08X}: sem {W(j3, 8):08X} header {hdr3:08X} = "
             + (" ".join(f"{W(h3, i):08X}" for i in range(0, 16, 4)) if h3 else "?")
             + f"  done-counter {cnt3:08X} = " + (f"{W(c3, 0):08X}" if c3 else "?"))
    s += [f"J {job3 + o:08X}: " + " ".join(f"{W(j3, o + i):08X}" for i in range(0, 16, 4)) for o in range(0, 128, 16)]
    s += [f"S {0x146F7800 + o:08X}: " + " ".join(f"{W(sems, o + i):08X}" for i in range(0, 16, 4)) for o in range(0, 0x200, 0x10)]
    return "\n".join(s) + "\n"


# -- a hard time limit for a whole test ---------------------------------------
def _descendants(pid):
    """Every process below pid (by /proc ppid), deepest last."""
    import os
    kids = {}
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            with open(f"/proc/{d}/stat") as f:
                ppid = int(f.read().rsplit(")", 1)[1].split()[1])
        except (OSError, IndexError, ValueError):
            continue
        kids.setdefault(ppid, []).append(int(d))
    out, todo = [], [pid]
    while todo:
        p = todo.pop()
        for c in kids.get(p, []):
            out.append(c)
            todo.append(c)
    return out


def hard_limit(seconds, what):
    """Fail the test (exit 1) if it is still running after `seconds`: kill
    every process it started (games run in their own session, so each one's
    whole group) and say so. A wait that never ends -- a game that stopped
    presenting frames, a screen the driver does not know -- then costs the
    suite this long instead of hanging it. HARD_LIMIT_S overrides."""
    import os
    import signal
    seconds = int(os.environ.get("HARD_LIMIT_S", seconds))

    def fire(*_):
        print(f"FAIL: {what}: still running after the hard limit of {seconds} s "
              f"(HARD_LIMIT_S); killed", flush=True)
        for p in reversed(_descendants(os.getpid())):
            for kill in (lambda: os.killpg(p, signal.SIGKILL), lambda: os.kill(p, signal.SIGKILL)):
                try:
                    kill()
                except (ProcessLookupError, PermissionError):
                    pass
        os._exit(1)
    signal.signal(signal.SIGALRM, fire)
    signal.alarm(seconds)


# -- /dev_hdd1 seed -----------------------------------------------------------
# A boot with an empty /dev_hdd1 copies four fonts there and builds the
# game's UI cache: it writes "chimp" and renames it to a hash name, 1558
# times, ~1.2 GB in all (the same files RPCS3 keeps in
# dev_hdd1/caches/BCES00484_BCES00484439898). Tests skip that with a seed:
# work/runs/hdd1_seed, built once by a clean boot and then read-only, and
# hard-linked into each run's own PS3_HDD1_ROOT (the game only reads the
# cache once it exists; a write would hit the read-only file, not change
# the seed). Never ROOT/hdd1, which play.sh and any run without its own
# PS3_HDD1_ROOT rewrite in place.
HDD1_FONTS = {"nimbussanconl-bla.ttf": 132372, "nimbussandot-blaconita.ttf": 229120,
              "nimbussanst-regularcondensed.ttf": 83912}
HDD1_CACHE_FILES = 1558


def _hdd1_cache(d):
    import os
    try:
        return [f for f in os.listdir(d) if f not in HDD1_FONTS and f not in ("japanese.ttf", "cache")
                and os.path.isfile(os.path.join(d, f))]
    except OSError:
        return []


def hdd1_complete(d):
    """The nimbus fonts copied and the whole cache renamed into place (no
    "chimp" left half-built). japanese.ttf may be 0 bytes: its 2.1 MB copy
    can fail (docs/NOTES.md "First-boot hdd1 corruption") and the game then
    reads it from the disc."""
    import os
    d = Path(d)
    try:
        if not all(os.path.getsize(d / f) == n for f, n in HDD1_FONTS.items()):
            return False
    except OSError:
        return False
    c = _hdd1_cache(d)
    return "chimp" not in c and len(c) >= HDD1_CACHE_FILES


def _freeze(src, seed):
    """Move a finished hdd1 into place as the seed, read-only."""
    import os
    import subprocess
    if not hdd1_complete(src):
        return False
    os.rename(src, seed)                  # before the chmod: a read-only dir cannot be moved
    subprocess.run(["chmod", "-R", "a-w", str(seed)], check=True)
    return True


def _generate_hdd1(root, out):
    """Boot once with an empty hdd1 into `out`, CROSS every 2 s, until the
    cache is complete and nothing in it has changed for 60 s."""
    import os
    import signal
    import subprocess
    import time
    elf = Path(root) / "disc/PS3_GAME/USRDIR/EBOOT.elf"
    binp = os.environ.get("BIN") or str(Path(root) / "work/port/build-vk/MotorStormRecomp")
    out = Path(out)
    out.mkdir(parents=True)
    (out.parent / "save").mkdir(exist_ok=True)
    pad = out.parent / "pad"
    pad.write_text("\n")
    env = dict(os.environ, SDL_NO_SIGNAL_HANDLERS="1", SDL_AUDIODRIVER="dummy", SDL_VIDEODRIVER="dummy",
               PS3_HDD1_ROOT=str(out), PS3_SAVEDATA_ROOT=str(out.parent / "save"),
               PS3_GAMEDATA_ROOT=str(out.parent / "gamedata"), PS3_PAD_FILE=str(pad))
    with open(out.parent / "log", "wb") as f:
        p = subprocess.Popen([os.path.abspath(binp), str(elf)], cwd=root, env=env, stdout=f,
                             stderr=subprocess.STDOUT, start_new_session=True)
        end = time.time() + 2400
        try:
            last, quiet_since = None, time.time()
            while time.time() < end and p.poll() is None:
                state = sorted(_hdd1_cache(out))
                if state != last:
                    last, quiet_since = state, time.time()
                if hdd1_complete(out) and time.time() - quiet_since > 60:
                    break
                pad.write_text("CROSS\n"); time.sleep(0.25); pad.write_text("\n"); time.sleep(1.75)
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    return hdd1_complete(out)


def hdd1_seed(root):
    """The read-only seed, built on first use by one boot with an empty hdd1
    (a few minutes; tests starting meanwhile wait on a lock)."""
    import fcntl
    import shutil
    import subprocess
    root = Path(root)
    seed = root / "work/runs/hdd1_seed"
    if hdd1_complete(seed):
        return seed
    lock = open(str(seed) + ".lock", "w")
    fcntl.flock(lock, fcntl.LOCK_EX)
    if hdd1_complete(seed):
        return seed
    if seed.exists():
        raise SystemExit(f"FAIL: {seed} exists but is not a complete hdd1; remove it (chmod -R u+w first)")
    gen = root / "work/runs/hdd1_seed_gen"
    subprocess.run(["chmod", "-R", "u+w", str(gen)], stderr=subprocess.DEVNULL)
    shutil.rmtree(gen, ignore_errors=True)
    print("[hdd1] building the /dev_hdd1 seed: one boot with an empty /dev_hdd1", flush=True)
    if not _generate_hdd1(root, gen / "hdd1") or not _freeze(gen / "hdd1", seed):
        raise SystemExit(f"FAIL: could not build an hdd1 seed (see {gen}/log)")
    return seed


def seed_hdd1(root, dst):
    """The seed hard-linked into dst (a test's own PS3_HDD1_ROOT): instant,
    no 1.2 GB copy. Its directories are made writable (the game adds files);
    the files stay the seed's read-only inodes."""
    import os
    import shutil
    shutil.copytree(hdd1_seed(root), dst, copy_function=os.link)
    for d, _, _ in os.walk(dst):
        os.chmod(d, 0o755)
    return dst
