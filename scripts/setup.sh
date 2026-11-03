#!/usr/bin/env bash
# Fetch ps3recomp at the commit the toolkit patch is made against, apply the
# patch, and install its Python tools into .venv.
#
#   scripts/setup.sh            toolkit in ./ps3recomp
#   PS3RECOMP_DIR=... scripts/setup.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UPSTREAM=https://github.com/sp00nznet/ps3recomp.git
BASE=42f5f8f9415117f324ca438c46573f2caca63468   # v0.13.0 docs, 2026-10-07
PATCH="$ROOT/work/patches/ps3recomp.patch"
DIR="${PS3RECOMP_DIR:-$ROOT/ps3recomp}"

if [ ! -d "$DIR/.git" ]; then
    git clone "$UPSTREAM" "$DIR"
fi
git -C "$DIR" checkout --quiet "$BASE"
if git -C "$DIR" apply --check "$PATCH" 2>/dev/null; then
    git -C "$DIR" apply "$PATCH"
    echo "patch applied to $DIR"
elif git -C "$DIR" apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "patch already applied in $DIR"
else
    echo "the patch does not apply to $DIR at $BASE -- start from a clean clone" >&2
    exit 1
fi

# Dear ImGui (MIT) for the launcher and the in-game menu, at a fixed tag;
# without it the port builds and runs with no UI.
IMGUI_TAG=v1.91.5
if [ ! -f "$DIR/libs/thirdparty/imgui/imgui.cpp" ]; then
    git clone --quiet --depth 1 --branch "$IMGUI_TAG" https://github.com/ocornut/imgui.git \
        "$DIR/libs/thirdparty/imgui"
fi

# The tools need Python 3.10+ (PEP 604 types). Take the first interpreter
# that has it: $PYTHON3, then the system one, then whatever python3 is on PATH
# (a conda base is often older).
PY3=""
for c in "${PYTHON3:-}" /usr/bin/python3 python3.13 python3.12 python3.11 python3.10 python3; do
    [ -n "$c" ] && command -v "$c" >/dev/null || continue
    if "$c" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null; then PY3="$c"; break; fi
done
[ -n "$PY3" ] || { echo "need Python 3.10 or newer (set PYTHON3=...)" >&2; exit 1; }
if [ -x "$ROOT/.venv/bin/python" ] && ! "$ROOT/.venv/bin/python" -c 'import sys; sys.exit(sys.version_info < (3, 10))'; then
    rm -rf "$ROOT/.venv"
fi
"$PY3" -m venv "$ROOT/.venv"
"$ROOT/.venv/bin/pip" install --quiet -r "$DIR/requirements.txt"
echo "toolkit ready: $DIR (python: $ROOT/.venv)"
