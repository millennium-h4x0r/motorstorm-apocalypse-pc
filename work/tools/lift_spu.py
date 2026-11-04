#!/usr/bin/env python3
"""Lift MotorStorm Apocalypse's SPU code into work/port/spu/, reproducibly.

Everything is found by scanning the decrypted EBOOT (BCES-00484 01.00); no
address below is typed in. What runs on the SPUs in this title, and how the
port's runtime (ps3recomp's cellSpurs HLE, runtime/spu) dispatches each kind:

  * the WWS Job Manager policy module. The PPU (fn 0x86B3C0, "Wws_Job") adds
    it as a SPURS workload with _cellSpursWorkloadAttributeInitialize +
    cellSpursAddWorkloadWithAttribute, giving the module EA and its size
    rounded up to 128 bytes (a TOC getter pair: `lwz r3,ea(r2)` and
    `lwz r3,size(r2); addi 127; rlwinm ..,0,24`). The SPURS kernel
    (libs/spurs/spurs_kernel.c) copies that many bytes to LS 0xA00 and runs the
    lift registered under their FNV fingerprint. It is image 2: the runtime's
    WWS handling (job-code buffer base at LS[0x1320], the bra-vector interrupt
    entry, the job return path) is keyed to that id. This PM is a newer build
    than inFAMOUS 2's, but its data layout (0x12E0/0x1320/0x1330/0xDF0/0xC00)
    and its 0xA2C service entry are the same.
  * the C0DEC0DE WWS job modules the PM streams into its job-code buffer
    (LS 0x4000). They are linked into the EBOOT uncompressed. 39 are named
    by a {name*, ea, size} table in rodata; 10 more (entry +0x30, referenced
    from the audio synth code) are not. tools/extract_jobmods.py --elf pulls
    them out, tools/lift_jobmods.py lifts each at 0x4000 and registers it by its
    16-byte header (jobmods_register.c).
  * the SPURS taskset tasks: 8 SPU ELFs linked at LS 0x3000 (Havok animation,
    four Havok physics tasks, the MultiStream SPURS core with MP3, EDGE post
    MLAA, EDGE zlib inflate), started with cellSpursCreateTask /
    CreateTaskWithAttribute / CreateTask2. cellSpursCreateTask* fingerprints
    the task ELF; tools/build_spu_workloads.py lifts each and registers it by
    that fingerprint and its code segments as regions (spu_elf_workloads.c).
  * 17 MultiStream DSP effect plugins (position-independent `*.pic` SPU ELFs,
    linked at 0x80). The MultiStream core loads them into its own local store
    at an address it picks at run time, so a lift (whose branch targets are
    absolute) has nowhere to be registered. They are found and checked, not
    lifted, unless MS_DSP_LSAS names the LS address(es) they run at.

Not present, and checked for: SPURS job chains (no cellSpursCreateJobChain
import), raw SPU / SPU thread groups (no sys_spu_thread_group_create or
sys_spu_thread_initialize syscall), SPU code in ms3.psarc (none of its 8420
entries is an SPU ELF or a job module; see docs/NOTES.md "SPU side").

Usage: work/tools/lift_spu.py --toolkit DIR --eboot ELF --spu-dir work/port/spu

The output directory is cleaned of this script's outputs first. Every lift
must succeed and every expected kind of image must be found, or the script
exits nonzero. SPU_LIFT_EXTRA adds lifter arguments for an experiment.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.dirname(HERE)

# The executable this port was made for. With it, the image counts below are
# exact; with another one the script still runs but only requires one of each.
EXPECT_SHA256 = "387c9fb795e1709bd2cf1a1145c0548a0fc911a295aa860a27b41441a15418ff"
EXPECT_COUNTS = {"wws_pm": 1, "jobmod": 49, "task": 8, "ms_dsp": 17}

PM_LSA = 0xA00
PM_IMAGE_ID = 2             # runtime/spu: the WWS handling is keyed to image 2
TASK_IMAGE_BASE = 30        # clear of ids the runtime special-cases (1,2,3,6,22,23)
JOBMOD_IMAGE_BASE = 200     # fixed by tools/lift_jobmods.py
DSP_IMAGE_BASE = 260        # only used when MS_DSP_LSAS is set
TASK_LS_BASE = 0x3000       # SPURS taskset tasks are linked here
DSP_LINK_BASE = 0x80        # the MultiStream .pic plugins

# The LS address(es) the MultiStream core loads its DSP plugins at. Not known
# statically (see docs/NOTES.md); fill in from a run (SPU_MISS_LOG=1 or
# SPU_DUMP_OVL) to have every plugin lifted and registered there.
MS_DSP_LSAS = []

# Every image is lifted with its registers in locals (--local-regs), each run of
# contiguous chunks as one C function (--merge-chunks) and `bi` on a register
# just loaded from a constant dispatched directly (--const-bi), as in the
# inFAMOUS 2 port.
LIFT_ARGS = ["--local-regs", "--merge-chunks", "--const-bi"] + os.environ.get("SPU_LIFT_EXTRA", "").split()

# Extra function entries for taskset tasks, found at run time (SPU_MISS_LOG=1
# and the "[spu] img=N branched into unlifted LS ..." lines): the targets of
# Havok's function-pointer tables, which the PPU builds in main memory so no
# static scan sees them, and the call-return points that interpreted callees
# return into. Without them those paths ran on the interpreter, and a
# collide step took 20-200 ms.
TASK_EXTRA_ENTRIES = {
    "task_hkp_collide_01246700": [
        0x032B8, 0x080F0, 0x086D8, 0x08968, 0x08AB0, 0x08C10, 0x08FE0, 0x090F8, 0x091F0,
        0x09298, 0x092B0, 0x09370, 0x09A40, 0x0CD28, 0x0EA88, 0x0EB1C, 0x10FD0, 0x110A4,
        0x110F4, 0x11218, 0x113A4, 0x11F3C, 0x126C8, 0x13D08, 0x14724, 0x14D34, 0x15808,
        0x1592C, 0x15930, 0x15A28, 0x15A68, 0x15D10, 0x1CB80, 0x3BD58, 0x3BDA0, 0x3BDE8],
    "task_hkp_collision_query_0126C600": [
        0x03148, 0x046E0, 0x04720, 0x048D0, 0x05190, 0x06558, 0x068E8, 0x0AF58, 0x0BBF0,
        0x0C628, 0x0C630, 0x0C6C0, 0x0C720, 0x0C798, 0x0C7A0, 0x0C7F8, 0x0C8A8, 0x0C918,
        0x0C9D8, 0x0CA48, 0x0CBA8, 0x0CD08, 0x0CE08, 0x0CE78, 0x0CF70, 0x0D018, 0x0D030,
        0x0D0E0, 0x0D480, 0x0D6E0, 0x0D7C8, 0x0D8B0, 0x0DA88, 0x0DCA0, 0x0DD38, 0x0DF60,
        0x105A0, 0x105F8, 0x10668, 0x10838, 0x111F8],
}

# Lifted functions a native replacement may take over (keyed by lifted
# directory name -> LS addresses). None yet for this title.
NATIVE_ENTRIES = {}

# What this script owns in --spu-dir (removed before each run).
GENERATED_FILES = ("spu_workloads.c", "spu_elf_workloads.c", "jobmods_register.c",
                   "jobmods_manifest.json", "spu_manifest.json", "_tmp.elf")
GENERATED_DIRS = ("images", "jobmods", "elf_images", "pm_images")


def die(msg):
    sys.exit(f"[lift_spu] ERROR: {msg}")


def fnv1a64(b):
    # runtime/spu/spu_workload.c's spu_workload_fingerprint (basis and prime).
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def run(cmd):
    print("+", " ".join(cmd), flush=True)
    r = subprocess.run(cmd, check=False)
    if r.returncode:
        die(f"{os.path.basename(cmd[1])} failed ({r.returncode})")


# ---- the PPU executable --------------------------------------------------------

class Eboot:
    def __init__(self, path):
        self.path = path
        self.d = Path(path).read_bytes()
        d = self.d
        if d[:4] != b"\x7fELF" or d[4] != 2:
            die(f"{path} is not an ELF64 (decrypted EBOOT.elf expected)")
        phoff = struct.unpack_from(">Q", d, 0x20)[0]
        phnum = struct.unpack_from(">H", d, 0x38)[0]
        self.segs = []      # (file off, vaddr, filesz, flags)
        for i in range(phnum):
            p_type, flags, off, va, _, filesz, _, _ = struct.unpack_from(">IIQQQQQQ", d, phoff + i * 56)
            if p_type == 1 and filesz:
                self.segs.append((off, va, filesz, flags))
        self.text = [s for s in self.segs if s[3] & 1]

    def ea_of(self, off):
        for o, va, fs, _ in self.segs:
            if o <= off < o + fs:
                return va + off - o
        return None

    def off_of(self, ea):
        for o, va, fs, _ in self.segs:
            if va <= ea < va + fs:
                return o + ea - va
        return None

    def read(self, ea, n):
        o = self.off_of(ea)
        if o is None or self.off_of(ea + n - 1) is None:
            die(f"EA 0x{ea:X}+0x{n:X} is not file-backed")
        return self.d[o:o + n]

    def word(self, ea):
        return be32(self.read(ea, 4), 0)

    def data_refs(self, value):
        """EAs of every aligned 32-bit word in the file-backed segments equal to value."""
        pat = struct.pack(">I", value)
        out = []
        for o, va, fs, _ in self.segs:
            blob = self.d[o:o + fs]
            i = blob.find(pat)
            while i >= 0:
                if i % 4 == 0:
                    out.append(va + i)
                i = blob.find(pat, i + 1)
        return out

    def cstr(self, ea):
        o = self.off_of(ea)
        if o is None:
            return None
        m = re.match(rb"[ -~]{2,}\0", self.d[o:o + 256])
        return m.group(0)[:-1].decode() if m else None



def module_toc(out_dir, eboot):
    """The executable's TOC base (r2), from tools/ppu_loader.py's loader JSON."""
    name = os.path.splitext(os.path.basename(eboot))[0]
    path = os.path.join(out_dir, f"{name}.loader.json")
    if not os.path.exists(path):
        die(f"no {path}: run tools/ppu_loader.py first (scripts/lift.sh does)")
    return int(json.loads(Path(path).read_text())["module_toc"], 16)


def imported_names(toolkit, out_dir, eboot):
    """Names of the EBOOT's imported functions, from the PPU loader's imports
    JSON (work/out/<name>.imports.json) and the toolkit's NID database."""
    name = os.path.splitext(os.path.basename(eboot))[0]
    path = os.path.join(out_dir, f"{name}.imports.json")
    if not os.path.exists(path):
        die(f"no {path}: run tools/ppu_loader.py first (scripts/lift.sh does)")
    sys.path.insert(0, os.path.join(toolkit, "tools"))
    import nid_database
    db = nid_database.get_default_db()
    db.load_implemented(toolkit)
    names = set()
    for x in json.loads(Path(path).read_text()):
        r = db.lookup_nid(int(x["nid"], 16))
        if r:
            names.add(r[1])
    return names


def syscall_numbers(eb):
    """lv2 syscall numbers the PPU code issues (`li r11,N` shortly before `sc`)."""
    nums = set()
    for o, va, fs, _ in eb.text:
        i = eb.d.find(b"\x44\x00\x00\x02", o, o + fs)
        while i >= 0:
            if i % 4 == 0:
                for k in range(1, 9):
                    p = be32(eb.d, i - 4 * k)
                    if p >> 16 == 0x3960:
                        nums.add(p & 0xFFFF)
                        break
            i = eb.d.find(b"\x44\x00\x00\x02", i + 4, o + fs)
    return nums


# ---- discovery -----------------------------------------------------------------

def spu_note_name(elf):
    i = elf.find(b"SPUNAME\0")
    if i < 0:
        return ""
    m = re.match(rb"[ -~]+", elf[i + 8:i + 8 + 64])
    return m.group(0).decode() if m else ""


def spu_load_segments(elf):
    phoff, phes, phn = be32(elf, 0x1C), struct.unpack_from(">H", elf, 0x2A)[0], struct.unpack_from(">H", elf, 0x2C)[0]
    out = []
    for i in range(phn):
        p = phoff + i * phes
        if be32(elf, p) == 1:
            out.append(dict(off=be32(elf, p + 4), vaddr=be32(elf, p + 8), filesz=be32(elf, p + 16),
                            memsz=be32(elf, p + 20), flags=be32(elf, p + 24)))
    return out


# Names for the stripped / generic task images, from their note name and the
# monitor-timer strings each carries (Havok's Tt/St timers).
TASK_NAMES = [
    (b"StSampleAnimation", "hka_sample_anim"),
    (b"TtSolve", "hkp_integrate"),
    (b"StNarrowPhaseTOI", "hkp_collide"),
    (b"TtCollQueryPairGetClosestPoints", "hkp_collision_query"),
    (b"TtCollQueryWorldRayCast", "hkp_raycast"),
    (b"msngSPURS_MP3", "multistream_mp3"),
    (b"edgepost_mlaa.cpp", "edge_post_mlaa"),
    (b"edgezlib_inflate_task.cpp", "edge_zlib_inflate"),
]


def task_name(elf, note):
    for marker, nm in TASK_NAMES:
        if marker in elf:
            return nm
    base = note.rsplit("/", 1)[-1]
    base = re.sub(r"\.(elf|pic|self)$", "", base)
    base = re.sub(r"[^0-9A-Za-z]+", "_", base).strip("_").lower()
    return base or "task"


def find_spu_elfs(eb, toolkit):
    """Every embedded SPU ELF, classified as a taskset task or a MultiStream DSP
    plugin. Anything else is an error: this script would not know how it runs."""
    sys.path.insert(0, os.path.join(toolkit, "tools"))
    import extract_spu_images     # sizes exactly as runtime spu_elf_image_size()
    tasks, dsps = [], []
    for off, size in extract_spu_images.find_spu_images(eb.d):
        ea = eb.ea_of(off)
        if ea is None:
            continue    # not loaded (a section-table false hit cannot be an SPU ELF)
        elf = eb.d[off:off + size]
        segs = spu_load_segments(elf)
        code = [s for s in segs if s["flags"] & 1]
        if not code:
            die(f"SPU ELF at EA 0x{ea:X} has no executable segment")
        base = min(s["vaddr"] for s in segs if s["filesz"] or s["memsz"])
        note = spu_note_name(elf)
        entry = be32(elf, 0x18)
        rec = dict(ea=ea, size=size, entry=entry, note=note, base=base,
                   fp=fnv1a64(elf), segments=segs, bytes=elf)
        if note.startswith("mstream_dsp_") or note.endswith(".pic"):
            rec["kind"] = "ms_dsp"
            rec["name"] = "msdsp_" + re.sub(r"[^0-9A-Za-z]+", "_", note[len("mstream_dsp_"):]
                                            .replace(".pic", "")).strip("_").lower()
            if base != DSP_LINK_BASE:
                die(f"DSP plugin {note} at EA 0x{ea:X} is linked at 0x{base:X}, not 0x{DSP_LINK_BASE:X}")
            dsps.append(rec)
        elif base == TASK_LS_BASE:
            rec["kind"] = "task"
            rec["name"] = task_name(elf, note)
            tasks.append(rec)
        else:
            die(f"SPU ELF at EA 0x{ea:X} ({note or 'no note'}) is linked at LS 0x{base:X}: "
                f"neither a SPURS task (0x{TASK_LS_BASE:X}) nor a DSP plugin; classify it")
    return tasks, dsps


def jobmod_headers(eb):
    """(ea, entry, imagesize) of every `4 ila | entry | size | 0 | 0 | C0DEC0DE`
    header, as tools/extract_jobmods.py finds them."""
    out = []
    i = eb.d.find(b"\xc0\xde\xc0\xde")
    while i >= 0:
        st = i - 0x20
        if st >= 0 and st % 16 == 0 and all(eb.d[st + 4 * k] in (0x42, 0x43) for k in range(4)):
            ea = eb.ea_of(st)
            if ea is not None:
                out.append((ea, be32(eb.d, st + 0x10), be32(eb.d, st + 0x14)))
        i = eb.d.find(b"\xc0\xde\xc0\xde", i + 4)
    return out


def jobmod_names(eb, mods):
    """The rodata table that names job modules: {char* name, u32 ea, u32 size}
    with size = header image size - 32. Returns {ea: name}."""
    by_ea = {ea: isz for ea, _, isz in mods}
    names = {}
    for ea, isz in by_ea.items():
        for ref in eb.data_refs(ea):
            try:
                np_, sz = eb.word(ref - 4), eb.word(ref + 4)
            except SystemExit:
                continue
            if sz == isz - 32:
                s = eb.cstr(np_)
                if s:
                    names[ea] = s
    return names


def ilas(blob, n=4):
    return all(blob[4 * k] in (0x42, 0x43) for k in range(n))


def find_wws_pm(eb, toc):
    """The WWS policy module: 16-byte aligned, four `ila` words, not a job
    module, and storing the job-code base with `stqa $r3, 0x1320`. Its size is
    the data word after the TOC entry that holds its EA, rounded as the PPU's
    getter rounds it before cellSpursAddWorkloadWithAttribute."""
    STQA_1320 = 0x20826403
    cands = []
    for o, va, fs, _ in eb.text:
        blob = eb.d[o:o + fs]
        i = blob.find(struct.pack(">I", STQA_1320))
        while i >= 0:
            # walk back to the nearest 4-ila header within 16 KB
            st = i & ~15
            while st > i - 0x4000 and st >= 0:
                if ilas(blob[st:st + 16]) and blob[st + 0x20:st + 0x24] != b"\xc0\xde\xc0\xde" \
                        and be32(blob, st + 0x10) >> 23 == 0x061 \
                        and be32(blob, st + 0x10) & 0x7F == 1:          # lqa $r1, stack top
                    cands.append(va + st)
                    break
                st -= 16
            i = blob.find(struct.pack(">I", STQA_1320), i + 4)
    cands = sorted(set(cands))
    pms = []
    for ea in cands:
        refs = eb.data_refs(ea)
        if not refs:
            continue
        for ref in refs:
            raw = eb.word(ref + 4)
            if not (0x100 <= raw <= 0x3F600 - PM_LSA):
                continue
            size = getter_rounded_size(eb, toc, ref, raw)
            pms.append(dict(ea=ea, ref=ref, raw_size=raw, size=size))
            break
    return pms


def getter_rounded_size(eb, toc, ea_word, raw):
    """The PPU reads the PM's EA and size through two TOC getters,
         lwz r3,D(r2) ; blr                                  (EA, the word at ea_word)
         lwz r3,D+4(r2) ; addi r3,r3,127 ; rlwinm r3,r3,0,0,24 ; blr   (size)
    and passes the rounded size to _cellSpursWorkloadAttributeInitialize; the
    kernel fingerprints exactly that many bytes. Die if the getters are absent."""
    d = ea_word - toc
    if not -0x8000 <= d < 0x7FFC:
        die(f"PM EA word 0x{ea_word:X} is not TOC-relative (TOC 0x{toc:X})")
    pat = struct.pack(">6I", 0x80620000 | (d & 0xFFFF), 0x4E800020,
                      0x80620000 | ((d + 4) & 0xFFFF), 0x3863007F, 0x54630030, 0x4E800020)
    for o, va, fs, _ in eb.text:
        i = eb.d.find(pat, o, o + fs)
        if i >= 0 and i % 4 == 0:
            print(f"[lift_spu] PM EA/size getters at 0x{eb.ea_of(i):X}: size rounded to 128 bytes")
            return (raw + 127) & ~127
    die(f"no `lwz r3,size(r2); addi 127; rlwinm` getter for the PM words at 0x{ea_word:X}")


def pm_extra_entries(pm, lsa=PM_LSA):
    """Entries a static scan of the PM does not find, derived from its code:
      * the interrupt vector target: the PM assembles `bra imm` at LS 0 with
        `ilhu $rX,0x180 ; iohl $rX,imm ; shli $rX,$rX,5` (inFAMOUS: 0xA2C);
      * after each `bisl $r0,$rA` (a call into job code staged at run time) the
        return point, and the return point of each `brsl` that follows before
        the next branch: the job comes back through an indirect branch.
    On inFAMOUS 2's PM this reproduces the list that port found at run time
    (0xA2C, 0x3218, 0x3220, 0x3228)."""
    words = [be32(pm, o) for o in range(0, len(pm) & ~3, 4)]
    out = set()
    for k, w in enumerate(words):
        if w >> 23 == 0x082 and (w >> 7) & 0xFFFF == 0x180:            # ilhu $rX, 0x180
            rx = w & 0x7F
            for j in range(k + 1, min(k + 8, len(words))):
                w2 = words[j]
                if w2 >> 23 == 0x0C1 and w2 & 0x7F == rx:                # iohl $rX, imm
                    imm = (w2 >> 7) & 0xFFFF
                    if lsa <= imm < lsa + len(pm) and imm % 4 == 0:
                        out.add(imm)
                    break
        if w >> 21 == 0x1A9 and w & 0x7F == 0:                           # bisl $r0, $rA
            out.add(lsa + 4 * k + 4)
            for j in range(k + 1, min(k + 9, len(words))):
                w2 = words[j]
                if w2 >> 23 == 0x066:                                    # brsl
                    out.add(lsa + 4 * j + 4)
                if w2 >> 21 == 0x1A8 or w2 >> 23 == 0x064:              # bi / br
                    break
    return sorted(out)


# ---- output --------------------------------------------------------------------

def clean(spu):
    """Remove what a previous run of this script wrote; leave anything else."""
    if not os.path.isdir(spu):
        return
    for name in os.listdir(spu):
        p = os.path.join(spu, name)
        if name in GENERATED_FILES and os.path.isfile(p):
            os.remove(p)
        elif os.path.isdir(p) and (name in GENERATED_DIRS or os.path.exists(os.path.join(p, "spu_recomp.c"))
                                   or os.path.exists(os.path.join(p, "spu_recomp.h"))):
            shutil.rmtree(p)
    left = os.listdir(spu)
    if left:
        print(f"[lift_spu] note: left in {spu} (not ours): {', '.join(sorted(left))}")


def debase(path):
    """The lifter includes the runtime by a path relative to its own tree; the
    port builds with runtime/spu on the include path instead."""
    p = Path(path)
    if not p.exists():
        return
    t = p.read_text(encoding="utf-8")
    t2 = re.sub(r'#include\s+"(?:\.\./)+runtime/spu/([A-Za-z0-9_]+\.h)"', r'#include "\1"', t)
    if t2 != t:
        p.write_text(t2, encoding="utf-8", newline="\n")


def require_entry(spu, name, prefix, entry):
    hdr = Path(spu, name, "spu_recomp.h")
    src = Path(spu, name, "spu_recomp.c")
    if not hdr.exists() or not src.exists():
        die(f"{name}: the lift left no spu_recomp.c/.h")
    sym = f"{prefix}spu_func_{entry:08X}("
    if sym not in hdr.read_text(encoding="utf-8", errors="ignore"):
        die(f"{name}: no lifted function at its entry 0x{entry:X} ({sym[:-1]})")


def add_native_entries(spu):
    for name, addrs in NATIVE_ENTRIES.items():
        p = Path(spu, name, "spu_recomp.c")
        if not p.exists():
            die(f"native entries for {name}, which was not lifted")
        src = p.read_text(encoding="utf-8")
        for a in addrs:
            head = f"void {name}_spu_func_{a:08X}(spu_context* ctx) {{\n"
            hook = (f"    {{ extern int spu_native_entry(spu_context*, uint32_t); "
                    f"if (spu_native_entry(ctx, 0x{a:X}u)) SPU_RET(ctx); }}\n")
            if head not in src:
                die(f"{name}: no function at 0x{a:X} for its native entry")
            if head + hook not in src:
                src = src.replace(head, head + hook, 1)
        p.write_text(src, encoding="utf-8", newline="\n")


def lift_raw(tools, img_dir, spu, name, blob, base, entry, extra):
    """Wrap raw LS bytes as an SPU ELF at `base` and lift them."""
    b = os.path.join(img_dir, name + ".bin")
    e = os.path.join(img_dir, name + ".elf")
    Path(b).write_bytes(blob)
    run([sys.executable, os.path.join(tools, "wrap_spu_elf.py"), b, "--entry", hex(entry),
         "--base", hex(base), "--out", e])
    out = os.path.join(spu, name)
    run([sys.executable, os.path.join(tools, "spu_lifter.py"), "--auto-functions", e,
         "--base", hex(base), "--symbol-prefix", name + "_", "-o", out,
         "--source-name", "spu_recomp.c", "--header-name", "spu_recomp.h",
         "--extra-funcs", ",".join(hex(a) for a in sorted(set([entry] + extra)))] + LIFT_ARGS)
    for f in ("spu_recomp.c", "spu_recomp.h"):
        debase(os.path.join(out, f))
    require_entry(spu, name, name + "_", entry)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    toolkit = os.environ.get("PS3RECOMP_DIR") or os.path.join(WORK, "..", "ps3recomp")
    ap.add_argument("--toolkit", default=toolkit)
    ap.add_argument("--eboot", default=os.path.join(WORK, "..", "disc", "PS3_GAME", "USRDIR", "EBOOT.elf"))
    ap.add_argument("--spu-dir", default=os.path.join(WORK, "port", "spu"))
    ap.add_argument("--loader-out", default=os.path.join(WORK, "out"),
                    help="tools/ppu_loader.py output (for the import list)")
    args = ap.parse_args()

    toolkit = os.path.abspath(args.toolkit)
    tools = os.path.join(toolkit, "tools")
    if not os.path.exists(os.path.join(tools, "spu_lifter.py")):
        die(f"no toolkit at {toolkit}")
    py = sys.executable
    spu = os.path.abspath(args.spu_dir)
    eb = Eboot(args.eboot)
    exact = hashlib.sha256(eb.d).hexdigest() == EXPECT_SHA256
    if not exact:
        print("[lift_spu] warning: not the BCES-00484 01.00 EBOOT; image counts are not checked exactly")

    # ---- what the PPU side can launch ------------------------------------------
    imports = imported_names(toolkit, args.loader_out, args.eboot)
    calls = syscall_numbers(eb)
    if imports & {"cellSpursCreateJobChain", "cellSpursCreateJobChainWithAttribute"}:
        die("the EBOOT imports SPURS job chains; this script does not lift job-chain binaries")
    if calls & {170, 172}:   # sys_spu_thread_group_create / sys_spu_thread_initialize
        die("the EBOOT creates SPU thread groups itself; classify their images")
    if not imports & {"cellSpursCreateTask", "cellSpursCreateTaskWithAttribute", "cellSpursCreateTask2"}:
        die("no cellSpursCreateTask* import, yet the EBOOT carries SPURS task ELFs")
    if "cellSpursAddWorkloadWithAttribute" not in imports and "cellSpursAddWorkload" not in imports:
        die("no cellSpursAddWorkload* import, yet a WWS policy module is expected")

    # ---- discovery ---------------------------------------------------------------
    tasks, dsps = find_spu_elfs(eb, toolkit)
    pms = find_wws_pm(eb, module_toc(args.loader_out, args.eboot))
    mods = jobmod_headers(eb)
    mod_names = jobmod_names(eb, mods)
    found = {"wws_pm": len(pms), "jobmod": len(mods), "task": len(tasks), "ms_dsp": len(dsps)}
    print(f"[lift_spu] found: {found}")
    for kind, n in EXPECT_COUNTS.items():
        if (exact and found[kind] != n) or (not exact and found[kind] == 0):
            die(f"expected {n if exact else 'some'} image(s) of kind {kind}, found {found[kind]}")
    pm = pms[0]
    sigs = [eb.read(ea, 16) for ea, _, _ in mods]
    if len(set(sigs)) != len(sigs):
        die("two job modules share a 16-byte header: the runtime could not tell them apart")

    clean(spu)
    os.makedirs(spu, exist_ok=True)
    img_dir = os.path.join(spu, "images")
    os.makedirs(img_dir, exist_ok=True)
    manifest = []

    # 1. The WWS policy module (image 2).
    pm_bytes = eb.read(pm["ea"], pm["size"])
    pm_fp = fnv1a64(pm_bytes)
    pm_extra = pm_extra_entries(pm_bytes)
    print(f"[lift_spu] WWS PM at EA 0x{pm['ea']:X}, size 0x{pm['raw_size']:X} -> 0x{pm['size']:X} "
          f"fingerprinted, fp 0x{pm_fp:016X}, extra entries {[hex(a) for a in pm_extra]}")
    lift_raw(tools, img_dir, spu, "wws_pm", pm_bytes, PM_LSA, PM_LSA, pm_extra)
    manifest.append(dict(name="wws_pm", kind="wws_policy_module", ea=pm["ea"], size=pm["size"],
                         ls=PM_LSA, entry=PM_LSA, image_id=PM_IMAGE_ID, fp=f"{pm_fp:016X}",
                         launch="cellSpursAddWorkloadWithAttribute; kernel copies to LS 0xA00",
                         extra_entries=[hex(a) for a in pm_extra]))

    # 2. Job modules (images 200+).
    jobmods = os.path.join(spu, "jobmods")
    run([py, os.path.join(tools, "extract_jobmods.py"), "--elf", args.eboot, "--out", jobmods])
    n_bins = len([f for f in os.listdir(jobmods) if f.endswith(".bin")])
    if n_bins != len(mods):
        die(f"extract_jobmods.py wrote {n_bins} module(s), the scan found {len(mods)}")
    run([py, os.path.join(tools, "lift_jobmods.py"), "--jobmods", jobmods, "--out", spu,
         "--register", os.path.join(spu, "jobmods_register.c"), "--symbol-prefix", "motorstorm"]
        + [f"--lifter-arg={a}" for a in LIFT_ARGS])
    jm = json.loads(Path(spu, "jobmods_manifest.json").read_text())
    if len(jm) != len(mods):
        lifted = {m["name"] for m in jm}
        missing = [f[:-4] for f in os.listdir(jobmods) if f.endswith(".bin") and f[:-4] not in lifted]
        die(f"lift_jobmods.py lifted {len(jm)} of {len(mods)} job modules; failed: {', '.join(missing)}")
    if JOBMOD_IMAGE_BASE + len(jm) > DSP_IMAGE_BASE:
        die("job module image ids run into the DSP plugins'")
    by_sig = {eb.read(ea, 16).hex(): (ea, entry, isz) for ea, entry, isz in mods}
    for i, m in enumerate(jm):
        for f in ("spu_recomp.c", "spu_recomp.h"):
            debase(os.path.join(spu, m["name"], f))
        require_entry(spu, m["name"], m["prefix"], m["entry"])
        ea, entry, isz = by_sig[m["sig"]]
        manifest.append(dict(name=m["name"], kind="wws_job_module", ea=ea, size=m["size"],
                             job_name=mod_names.get(ea), ls=0x4000, entry=m["entry"],
                             image_id=JOBMOD_IMAGE_BASE + i, sig=m["sig"],
                             funcs=m["funcs"], unsupported=m["unsupported"],
                             launch="WWS PM streams it into the job-code buffer"))

    # 3. SPURS taskset tasks (images 30+), by ELF fingerprint.
    elf_dir = os.path.join(spu, "elf_images")
    os.makedirs(elf_dir, exist_ok=True)
    tasks.sort(key=lambda t: t["ea"])
    for t in tasks:
        t["dir"] = f"task_{t['name']}_{t['ea']:08X}"
        Path(elf_dir, t["dir"] + ".elf").write_bytes(t["bytes"])
    run([py, os.path.join(tools, "build_spu_workloads.py"), "--images", elf_dir, "--lifted", spu,
         "--out", os.path.join(spu, "spu_elf_workloads.c"), "--register-fn", "port_spu_elf_register_all",
         "--constructor", "--title", "MotorStorm Apocalypse", "--first-image-id", str(TASK_IMAGE_BASE),
         "--eboot", args.eboot, "--relift"] + [f"--lift-arg={a}" for a in LIFT_ARGS]
        + [f"--extra-funcs={k}=" + ",".join(hex(a) for a in v) for k, v in TASK_EXTRA_ENTRIES.items()])
    # build_spu_workloads.py numbers images in sorted file-name order.
    for i, t in enumerate(sorted(tasks, key=lambda t: t["dir"])):
        prefix = re.sub(r"[^0-9A-Za-z_]", "_", t["dir"]) + "_"
        require_entry(spu, t["dir"], prefix, t["entry"])
        manifest.append(dict(name=t["dir"], kind="spurs_task", ea=t["ea"], size=t["size"], note=t["note"],
                             ls=TASK_LS_BASE, entry=t["entry"], image_id=TASK_IMAGE_BASE + i,
                             fp=f"{t['fp']:016X}",
                             segments=[{k: hex(v) for k, v in s.items()} for s in t["segments"]],
                             launch="cellSpursCreateTask* (taskset task)"))
    if TASK_IMAGE_BASE + len(tasks) > JOBMOD_IMAGE_BASE:
        die("task image ids run into the job modules'")

    # 4. MultiStream DSP plugins: lifted only at known load addresses.
    dsp_lifted = []
    for t in sorted(dsps, key=lambda t: t["ea"]):
        code = [s for s in t["segments"] if s["flags"] & 1][0]
        text_ea = t["ea"] + code["off"]
        t["text_ea"], t["text_size"] = text_ea, code["filesz"]
        manifest.append(dict(name=t["name"], kind="ms_dsp_plugin", ea=t["ea"], size=t["size"], note=t["note"],
                             text_ea=text_ea, text_size=code["filesz"], link_base=DSP_LINK_BASE,
                             entry=t["entry"], fp=f"{t['fp']:016X}",
                             text_fp=f"{fnv1a64(t['bytes'][code['off']:code['off'] + code['filesz']]):016X}",
                             launch="cellMSDSPLoadDSPFromMemory; the MultiStream core loads it into its LS",
                             lifted_at=[hex(a) for a in MS_DSP_LSAS]))
        for lsa in MS_DSP_LSAS:
            name = f"{t['name']}_ls{lsa:05X}"
            blob = t["bytes"][code["off"]:code["off"] + code["filesz"]]
            entry = lsa + t["entry"] - code["vaddr"]
            lift_raw(tools, img_dir, spu, name, blob, lsa, entry, [])
            dsp_lifted.append((name, text_ea, lsa, len(blob), DSP_IMAGE_BASE + len(dsp_lifted)))
    if DSP_IMAGE_BASE + len(dsp_lifted) > 512:
        die("image ids run past 511 (runtime tables are 512 wide)")
    if not MS_DSP_LSAS:
        print(f"[lift_spu] {len(dsps)} MultiStream DSP plugin(s) classified, not lifted: "
              "their LS load address is chosen at run time (set MS_DSP_LSAS)")

    # 5. Registration of the PM and the DSP plugins (the tasks register from
    #    spu_elf_workloads.c, the job modules from jobmods_register.c).
    dsp_decl = "".join(f"extern void {nm}_spu_recomp_register(void);\n" for nm, *_ in dsp_lifted)
    dsp_reg = "".join(
        f"    spu_begin_image({img}); {nm}_spu_recomp_register();\n"
        f"    spu_overlay_register_region_at(0x{ea:08X}u, 0x{lsa:X}u, 0x{n:X}u, {img});\n"
        for nm, ea, lsa, n, img in dsp_lifted)
    src = f"""/* spu_workloads.c -- GENERATED by work/tools/lift_spu.py. Do not edit.
 * MotorStorm Apocalypse's WWS Job Manager policy module (EA 0x{pm['ea']:X}, 0x{pm['size']:X}
 * bytes at LS 0x{PM_LSA:X}): the SPURS kernel fingerprints the module it copies
 * into local store and runs this lift when the fingerprint matches. Image
 * {PM_IMAGE_ID}: the runtime's WWS job-manager handling is keyed to it.
 * MultiStream DSP plugins lifted at known load addresses: {len(dsp_lifted)}. */
#include "spu_workload.h"

extern void spu_begin_image(int image_id);
extern void spu_overlay_register_region_at(uint32_t content_ea, uint32_t lsa, uint32_t span, int image_id);
extern void wws_pm_spu_recomp_register(void);
extern void wws_pm_spu_func_{PM_LSA:08X}(spu_context*);
{dsp_decl}
void port_spu_register_all(void)
{{
    spu_begin_image({PM_IMAGE_ID}); wws_pm_spu_recomp_register();
    spu_workload_register_img(0x{pm_fp:016X}ULL, wws_pm_spu_func_{PM_LSA:08X}, {PM_IMAGE_ID}, "wws_pm");
{dsp_reg}    spu_begin_image(0);
}}

__attribute__((constructor)) static void port_spu_register_all_ctor(void)
{{
    port_spu_register_all();
}}
"""
    Path(spu, "spu_workloads.c").write_text(src, encoding="utf-8", newline="\n")
    add_native_entries(spu)

    for m in manifest:
        for k in ("ea", "size", "ls", "entry", "text_ea", "text_size", "link_base"):
            if isinstance(m.get(k), int):
                m[k] = hex(m[k])
    Path(spu, "spu_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    lifted = sorted(d for d in os.listdir(spu) if os.path.exists(os.path.join(spu, d, "spu_recomp.c")))
    unsup = sum(m.get("unsupported", 0) for m in manifest)
    print(f"[lift_spu] lifted {len(lifted)} image(s) into {spu} "
          f"(1 PM, {len(jm)} job modules, {len(tasks)} tasks, {len(dsp_lifted)} DSP lifts); "
          f"{unsup} unsupported instruction(s) in job modules; manifest spu_manifest.json")


if __name__ == "__main__":
    main()
