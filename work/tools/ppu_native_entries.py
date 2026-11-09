#!/usr/bin/env python3
"""Let native code take over lifted PPU code (port/ppu_native.cpp).

A post-pass over the lifter's output, the PPU twin of lift_spu.py's
add_native_entries. Two kinds of patch:

Native entries (NATIVE_ENTRIES). The entry of each listed function gets

    PPU_NATIVE_HOOK(0x00123456u);     (example address)

before anything else in its body -- with --local-regs, before the guest
registers are loaded into locals. The hook asks the runtime once, on the first
call, for the native function registered at that address (ppu_native_slot)
and caches the answer in a function-local static; after that it costs a load
and a branch. A native function that returns nonzero has done the guest
function's work and left the context as the guest's `blr` would (PPC64 ELF
ABI: r1, r2, r13-r31, f14-f31, v20-v31, cr2-cr4 kept, the return value in r3);
the lifted function then returns at once. One that returns 0 declines, and
the lifted body runs as if there were no hook.

Native blocks (NATIVE_BLOCKS). For a hot loop inside a function too big to
replace whole: C put in at one of its labels, working on the --local-regs
locals, that does what a whole run of the loop does and leaves every register
as the loop would. It falls through to the lifted loop when its fast path does
not apply. Each block names the lifted lines it was written against, and the
pass stops if the lift no longer has them.

Each chunk with a patch gets the definitions they use after its preamble. The
pass is idempotent and the lists are authoritative: a patch that left them is
taken out again. Only chunks whose text changes are written, so a rebuild
recompiles just those. A relift drops every patch (the lifter does not know
about them): scripts/lift.sh runs this after the lift.

PS3RECOMP_NATIVE_PPU=0 turns both off at run time (the compare mode, 2, is
for the entries; the blocks just run). --no-blocks leaves the blocks out.

Usage: work/tools/ppu_native_entries.py [--recomp DIR] [--check] [--no-blocks]
  --check   report what would change and exit 1 if anything would
"""
import argparse
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.dirname(HERE)

# Guest functions port/ppu_native.cpp has a native version of. Every address
# here needs a lifted function of that name, and port/ppu_native.cpp needs an
# entry for it in s_funcs (an entry with no hook is never called; a hook with
# no entry costs one cached lookup).
#
# Add MotorStorm entries here, e.g. (example address):
#     0x00123456,     # memcpy(dst, src, n)
NATIVE_ENTRIES = [
    0x00FF5B40,     # the game's event Set(bool): its reset deadlocks against a late Set
    0x005EE2A4,     # game clock constructor (PS3_SIM_60: remembers the instance, declines)
    0x005EE178,     # game clock step recompute (PS3_SIM_60: real-time, interval 1; declines)
    0x001DA398,     # the race's dt clamp: counts frame time and the clamped part (sim_speed.sh); declines
    0x00675D3C,     # the game's spinlock acquire: futex wait instead of 250 us sleeps
    0x00675E00,     # its release: wakes the futex waiters
    0x008BDB08,     # Havok job queue addJob (PS3_HAVOK_TRACE observer; declines)
    0x008C20D8,     # Havok PPU semaphore acquire (PS3_HAVOK_TRACE; declines)
    0x008C219C,     # Havok PPU semaphore, sibling (PS3_HAVOK_TRACE; declines)
    0x007820C0,     # raycast batch wait (PS3_HAVOK_TRACE; declines)
    0x007828D0,     # raycast batch build (PS3_HAVOK_TRACE; declines)
]

# Hooks for port/ppu_native.cpp's call-count probes (PS3RECOMP_PPU_PROBE=1):
# off, the hook finds nothing and the function runs as lifted. Keep in step
# with s_probes there. Add MotorStorm probes here.
PROBES = [
]

# Native blocks: hot loops inside functions too big to replace whole. Each is
#
#   {"func": 0x00123456,          # the lifted function holding the loop
#    "label": "loc_00123480",     # where the block goes (just after the label)
#    "code": [...],               # C lines; on its fast path it leaves every
#                                 # register as the loop would and jumps to
#                                 # ppu_nat_resume_<label[4:]> (placed after
#                                 # "expect") or to the loop's exit label;
#                                 # otherwise it falls through to the loop
#    "before": [...],             # the lifted lines right before the label
#    "expect": [...]}             # the lifted lines right after it
#
# "before" and "expect" are the lift's text the block was written against;
# the pass stops if the lift no longer has them. The code works on the
# --local-regs locals (_r0.._r31, _cr, _xer) and may use ppu_nat_range_ok(ea,
# n) to check a guest range takes the inline memory paths.
#
# (inFAMOUS 2 had one: a memchr over a byte array in its solve loop.)
# Add MotorStorm blocks here.
NATIVE_BLOCKS = [
    # func_00911128's per-packet loop top: a packet whose header (byte +3)
    # has a quadword count of 0, or a cursor past the list end, ends the list
    # there instead of starting a copy that never stops (the 0xD0000000
    # crashes). port/ppu_native.cpp ppu_pkt_guard; PS3_PKT_GUARD=0 turns it off.
    {"func": 0x00911128,
     "label": "loc_00911378",
     "code": [
         "        { const uint32_t _pend = vm_read32(_r1 + 0xEB8);",
         "          if (((uint32_t)_r19 >= _pend || (vm_read8(_r19 + 3) >> 4) == 0) &&",
         "              ppu_pkt_guard((uint32_t)_r19, _pend, (uint32_t)_r1)) { _r19 = _pend; goto loc_00912348; } }",
     ],
     "before": [
         "        vm_write64(_r1 + 0xE60, _r18);",
         "        vm_write8(_r1 + 0xE80, _r18);",
         "        vm_write64(_r1 + 0xEA8, _r20);",
     ],
     "expect": [
         "        { uint64_t _a = _r1; uint64_t _r = _a + (uint64_t)(int64_t)(0x9F0); _r3 = _r; _xer = (_xer & ~(1u << 29)) | ((_r < _a) ? (1u << 29) : 0u); }",
         "        _r7 = vm_read64(_r1 + 0xE88);",
     ]},
]

MARK = "/* ppu-native: tools/ppu_native_entries.py */"
MARK_END = "/* ppu-native end */"
PREAMBLE_HOOK = [
    'extern "C" int (*ppu_native_slot(uint32_t addr))(ppu_context*);',
    ("#define PPU_NATIVE_HOOK(a) do { static int (*const s_nat)(ppu_context*) = ppu_native_slot(a); "
     "if (s_nat && s_nat(ctx)) return; } while (0)"),
]
# Blocks on (port/ppu_native.cpp, PS3RECOMP_NATIVE_PPU not 0) and the range
# one the inline accessors would take access by access (ppu_recomp.h vm_inl_ok).
PREAMBLE_BLOCK = [
    'extern "C" int g_ppu_native_blocks;',
    'extern "C" int ppu_pkt_guard(uint32_t p, uint32_t end, uint32_t r1);   /* port/ppu_native.cpp */',
    "static inline int ppu_nat_range_ok(uint32_t ea, uint64_t n)",
    "{",
    "    const uint64_t ctl = ppu_hle_inject_base + 0x2008u;",
    "    return __builtin_expect(g_ppu_native_blocks && g_vm_inl_lim != 0, 1) && ea - 0x10000u < g_vm_inl_lim &&",
    "           ea + n <= 0x10000ull + g_vm_inl_lim && (ctl + 4 <= ea || ctl >= ea + n);",
    "}",
]
BLOCK_TAG = "  /* ppu-native block */"
HDR = re.compile(r"^void (?:__attribute__\(\(weak\)\) )?func_([0-9A-F]{8})\(ppu_context\* __restrict ctx\) \{$")
HOOK = re.compile(r"^    PPU_NATIVE_HOOK\(0x([0-9A-F]{8})u\);$")
RESUME = re.compile(r"^ppu_nat_resume_[0-9A-F]{8}:$")


def strip(lines):
    if MARK in lines:
        k = lines.index(MARK)
        e = lines.index(MARK_END, k)
        del lines[k:e + 2]
    return [l for l in lines if not HOOK.match(l) and not l.endswith(BLOCK_TAG) and not RESUME.match(l)]


def _norm(line):
    """A lifted line as the default lift spells it, for matching a block's
    before/expect lines against a lift made with the codegen options
    (ppu_lifter.py --stack-fast names r1-relative accesses vm_*_stk;
    --split-xer spells an XER update as a block over _xca/_xrest): the
    accessor names folded back, and a line cut at its XER update."""
    line = re.sub(r"\bvm_(read|write)(8|16|32|64)_stk\(", r"vm_\1\2(", line)
    for cut in ("_xer = ", "{ const uint32_t _xert = "):
        k = line.find(cut)
        if k >= 0:
            line = line[:k]
    return line.rstrip()


def apply_blocks(lines, i, blocks):
    """Patch the function whose header is lines[i]; returns the end index."""
    j = i + 1
    while lines[j] != "}":
        j += 1
    for b in blocks:
        try:
            at = lines.index(b["label"] + ":", i, j)
        except ValueError:
            sys.exit(f"func_{b['func']:08X}: no {b['label']} for its native block")
        n, m = len(b["expect"]), len(b["before"])
        if ([_norm(l) for l in lines[at + 1:at + 1 + n]] != [_norm(l) for l in b["expect"]] or
                [_norm(l) for l in lines[at - m:at]] != [_norm(l) for l in b["before"]]):
            sys.exit(f"func_{b['func']:08X} {b['label']}: the lifted loop is not the one its native block was "
                     "written for (tools/ppu_native_entries.py NATIVE_BLOCKS)")
        lines[at + 1 + n:at + 1 + n] = [f"ppu_nat_resume_{b['label'][4:]}:"]
        lines[at + 1:at + 1] = [c + BLOCK_TAG for c in b["code"]]
        j += 1 + len(b["code"])
    return j


def patch_chunk(text, want, blocks):
    lines = strip(text.split("\n"))
    found, nblk = set(), 0
    i = 0
    while i < len(lines):
        m = HDR.match(lines[i])
        if not m:
            i += 1
            continue
        a = int(m.group(1), 16)
        if a in want:
            found.add(a)
            lines.insert(i + 1, f"    PPU_NATIVE_HOOK(0x{a:08X}u);")
        mine = [b for b in blocks if b["func"] == a]
        if mine:
            found.add(("block", a))
            nblk += len(mine)
        i = apply_blocks(lines, i, mine) + 1 if mine else i + 1
    if found:
        pre = [MARK] + PREAMBLE_HOOK + (PREAMBLE_BLOCK if nblk else []) + [MARK_END, ""]
        first = next(i for i, l in enumerate(lines) if HDR.match(l))
        while first > 0 and lines[first - 1].startswith("/*"):
            first -= 1
        lines[first:first] = pre
    return "\n".join(lines), found


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--recomp", default=os.path.join(WORK, "src", "recomp"))
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--no-blocks", action="store_true")
    a = ap.parse_args()
    want = set(NATIVE_ENTRIES) | set(PROBES)
    blocks = [] if a.no_blocks else NATIVE_BLOCKS
    chunks = sorted(glob.glob(os.path.join(a.recomp, "ppu_recomp_*.cpp")))
    if not chunks:
        if not want and not blocks:
            # Nothing listed and nothing lifted yet: nothing to do.
            print(f"ppu native: 0 entries, 0 block(s); no lifted chunks in {a.recomp}")
            return 0
        sys.exit(f"no lifted chunks in {a.recomp}")
    found_all, changed = set(), []
    for ch in chunks:
        with open(ch, encoding="utf-8") as f:
            old = f.read()
        new, found = patch_chunk(old, want, blocks)
        found_all |= found
        if new != old:
            changed.append(ch)
            if not a.check:
                with open(ch, "w", encoding="utf-8", newline="\n") as f:
                    f.write(new)
    missing = (want | {("block", b["func"]) for b in blocks}) - found_all
    if missing:
        sys.exit("no lifted function for " + ", ".join(
            f"0x{x[1]:08X}'s native block" if isinstance(x, tuple) else f"native entry 0x{x:08X}"
            for x in sorted(missing, key=lambda x: x[1] if isinstance(x, tuple) else x)))
    verb = "would change" if a.check else "changed"
    print(f"ppu native: {len(want)} entr{'y' if len(want) == 1 else 'ies'}, {len(blocks)} block(s); "
          f"{verb} {len(changed)} chunk(s)" + "".join(f"\n  {os.path.basename(c)}" for c in changed))
    return 1 if a.check and changed else 0


if __name__ == "__main__":
    sys.exit(main())
