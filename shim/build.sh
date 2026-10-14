#!/usr/bin/env bash
# Build the PhysXLoader.dll shim (i686, MSVC ABI, clang-cl + lld-link, xwin CRT/SDK).
#   shim/build.sh [dev]   -> build/shim/PhysXLoader.dll (+ .pdb), the developer build
#   shim/build.sh release -> build/shim-release/PhysXLoader.dll: no debug info or PDB, source paths mapped away
# Both: /O2, static CRT, the version resource (shim/version.rc, version in shim/version.h). Afterwards the export names/ordinals are diffed against
# the original game loader (vendor-dlls/game/PhysXLoader.dll) when it is present.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XWIN="${XWIN:-$ROOT/.toolchain/xwin}"
ORIG="$ROOT/vendor-dlls/game/PhysXLoader.dll"
MODE="${1:-dev}"
case "$MODE" in
  dev)     OUT="${SHIM_OUT:-$ROOT/build/shim}"
           CFLAGS=(/Zi); LFLAGS=(/debug "/pdb:$OUT/PhysXLoader.pdb") ;;
  release) OUT="${SHIM_OUT:-$ROOT/build/shim-release}"
           CFLAGS=(/DNDEBUG /Brepro "/clang:-ffile-prefix-map=$ROOT/=") ; LFLAGS=(/debug:none /opt:ref /opt:icf /Brepro) ;;
  *) echo "usage: shim/build.sh [dev|release]" >&2; exit 2 ;;
esac
mkdir -p "$OUT"

[[ -d "$XWIN/crt" ]] || { echo "xwin CRT/SDK missing at $XWIN (run tools/bootstrap_toolchain.sh)" >&2; exit 1; }

llvm-rc /nologo /I "$XWIN/sdk/include/um" /I "$XWIN/sdk/include/shared" /I "$ROOT/shim" \
  /fo "$OUT/version.res" "$ROOT/shim/version.rc"

clang-cl --target=i686-pc-windows-msvc -fuse-ld=lld-link \
  /nologo /O2 /MT /W3 "${CFLAGS[@]}" /EHsc /std:c++17 \
  -Wno-microsoft /DNOMINMAX /DWIN32 /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS \
  /imsvc "$XWIN/crt/include" \
  /imsvc "$XWIN/sdk/include/ucrt" /imsvc "$XWIN/sdk/include/um" /imsvc "$XWIN/sdk/include/shared" \
  /LD "$ROOT/shim/PhysXLoader.cpp" "$ROOT/shim/fluid_wrap.cpp" "$ROOT/shim/scene_read.cpp" "$ROOT/shim/fluid_ours.cpp" "$ROOT/shim/cloth_wrap.cpp" "$ROOT/shim/cloth_ours.cpp" "$ROOT/shim/coverage.cpp" "$ROOT/shim/loader_core.cpp" "$ROOT/shim/game_hooks.cpp" \
  "$ROOT/solver/fluid/fluid.cpp" "$ROOT"/solver/cloth/cloth*.cpp "$OUT/version.res" \
  /Fo"$OUT/" /Fd"$OUT/" /Fe"$OUT/PhysXLoader.dll" \
  /link /ignore:4099 /dll /def:"$ROOT/shim/PhysXLoader.def" /machine:x86 "${LFLAGS[@]}" \
  /libpath:"$XWIN/crt/lib/x86" /libpath:"$XWIN/sdk/lib/um/x86" /libpath:"$XWIN/sdk/lib/ucrt/x86"

exports() {  # dll -> "ordinal name" lines
  llvm-readobj --coff-exports "$1" | awk '/Ordinal:/{o=$2} /Name:/{print o, $2}'
}

echo "built ($MODE): $OUT/PhysXLoader.dll"
if [[ -f "$ORIG" ]]; then
  if diff <(exports "$ORIG") <(exports "$OUT/PhysXLoader.dll") >/dev/null; then
    echo "exports match original (names + ordinals):"
    exports "$OUT/PhysXLoader.dll" | sed 's/^/  /'
  else
    echo "EXPORT MISMATCH vs $ORIG:" >&2
    diff <(exports "$ORIG") <(exports "$OUT/PhysXLoader.dll") >&2 || true
    exit 1
  fi
fi
