#!/usr/bin/env bash
# Lift the game's code from YOUR decrypted executable into work/: the PPU
# into work/src/recomp, the SPU images into work/port/spu. Nothing produced
# here may be shared: it is the game's code, translated.
#
#   scripts/lift.sh [path/to/EBOOT.elf]   (default: disc/PS3_GAME/USRDIR/EBOOT.elf)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${PS3RECOMP_DIR:-$ROOT/ps3recomp}"
PY="${PYTHON:-$ROOT/.venv/bin/python}"
ELF="${1:-$ROOT/disc/PS3_GAME/USRDIR/EBOOT.elf}"
# The addresses in this port are those of BCES-00484 (Europe) version 01.00.
EXPECT=387c9fb795e1709bd2cf1a1145c0548a0fc911a295aa860a27b41441a15418ff

[ -f "$ELF" ] || { echo "no decrypted executable at $ELF (see README: Dump and decrypt)" >&2; exit 1; }
[ -d "$DIR/tools" ] || { echo "no toolkit at $DIR -- run scripts/setup.sh" >&2; exit 1; }
got=$(sha256sum "$ELF" | cut -c1-64)
if [ "$got" != "$EXPECT" ]; then
    echo "warning: $ELF is not the BCES-00484 01.00 executable this port was made for" >&2
    echo "         (sha256 $got); the lift may build but the port will not run it" >&2
fi

name=$(basename "$ELF"); name="${name%.*}"
rm -rf "$ROOT/work/src/recomp"
mkdir -p "$ROOT/work/out" "$ROOT/work/src"
"$PY" "$DIR/tools/ppu_loader.py" "$ELF" -o "$ROOT/work/out"
"$PY" "$DIR/tools/ppu_lifter.py" "$ELF" \
    --functions "$ROOT/work/out/$name.functions.json" \
    --hle-stubs "$ROOT/work/out/$name.imports.json" \
    --code-end 0x13d0468 --local-regs --output "$ROOT/work/src/recomp"
# Native stand-ins for some lifted functions and loops (work/port/ppu_native.cpp).
"$PY" "$ROOT/work/tools/ppu_native_entries.py" --recomp "$ROOT/work/src/recomp"
"$PY" "$ROOT/work/tools/lift_spu.py" --toolkit "$DIR" --eboot "$ELF" --spu-dir "$ROOT/work/port/spu"
echo "lifted: work/src/recomp (PPU), work/port/spu (SPU)"
