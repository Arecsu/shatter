#!/usr/bin/env bash
# Copy the user's own PhysX DLLs out of the (read-only) game install/prefix into
# vendor-dlls/ (gitignored, never redistributed). Records sha256 of each copy.
#
# Override sources with GAME_PREFIX=... if the install lives elsewhere.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME_PREFIX="${GAME_PREFIX:?set GAME_PREFIX to the Wine prefix that holds the game}"
BIN="$GAME_PREFIX/drive_c/Mirror's Edge/Binaries"
NV="$GAME_PREFIX/drive_c/Program Files (x86)/NVIDIA Corporation/PhysX"
OUT="$ROOT/vendor-dlls"

for d in "$BIN" "$NV/Engine/v2.8.0" "$NV/Common"; do
  [[ -d "$d" ]] || { echo "missing source dir: $d" >&2; exit 1; }
done

mkdir -p "$OUT/game" "$OUT/common" "$OUT/engine"
# Game-shipped (Epic "2.8.0_FC7" build of the loader + helpers)
for f in PhysXLoader.dll PhysXDevice.dll PhysXExtensions.dll NxCooking.dll NxCharacter.dll; do
  cp -p "$BIN/$f" "$OUT/game/$f"
done
# System software (PhysX 9.13.1220): CUDA runtime the cores import, newer loader for reference
for f in cudart32_60.dll PhysXDevice.dll PhysXLoader.dll; do
  cp -p "$NV/Common/$f" "$OUT/common/$f"
done
# Versioned cores. v2.8.0 is what the game requests (NX_PHYSICS_SDK_VERSION 0x02080000);
# v2.8.1 matches the only SDK headers we have.
for v in v2.8.0 v2.8.1; do
  mkdir -p "$OUT/engine/$v"
  cp -p "$NV/Engine/$v/PhysXCore.dll" "$NV/Engine/$v/PhysXCooking.dll" "$OUT/engine/$v/"
done

(cd "$OUT" && find . -name '*.dll' -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
echo "vendor DLLs synced to $OUT"
cat "$OUT/SHA256SUMS"
