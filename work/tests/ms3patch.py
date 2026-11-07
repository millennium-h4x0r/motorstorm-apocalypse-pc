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
