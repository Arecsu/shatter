#!/usr/bin/env bash
# Build ShatterSetup.exe (i686, clang-cl + lld-link, static CRT, no PDB) with the release shim embedded.
#   installer/build.sh  -> build/installer/ShatterSetup.exe (builds the release shim first)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XWIN="${XWIN:-$ROOT/.toolchain/xwin}"
OUT="$ROOT/build/installer"
mkdir -p "$OUT"
"$ROOT/shim/build.sh" release | grep -v -e LNK4099 -e '^  [0-9]' -e 'exports match'
cp "$ROOT/build/shim-release/PhysXLoader.dll" "$OUT/PhysXLoader.dll"
(cd "$ROOT/installer" && llvm-rc /nologo /I "$XWIN/sdk/include/um" /I "$XWIN/sdk/include/shared" \
  /D 'SHATTER_DLL_PATH="../build/installer/PhysXLoader.dll"' /fo "$OUT/setup.res" setup.rc)
clang-cl --target=i686-pc-windows-msvc -fuse-ld=lld-link \
  /nologo /O2 /MT /W3 /DNDEBUG /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /Brepro \
  "/clang:-ffile-prefix-map=$ROOT/=" \
  /imsvc "$XWIN/crt/include" \
  /imsvc "$XWIN/sdk/include/ucrt" /imsvc "$XWIN/sdk/include/um" /imsvc "$XWIN/sdk/include/shared" \
  "$ROOT/installer/setup.cpp" "$OUT/setup.res" /Fo"$OUT/" /Fe"$OUT/ShatterSetup.exe" \
  /link /subsystem:windows /machine:x86 /debug:none /opt:ref /opt:icf /Brepro \
  /libpath:"$XWIN/crt/lib/x86" /libpath:"$XWIN/sdk/lib/um/x86" /libpath:"$XWIN/sdk/lib/ucrt/x86"
echo "built: $OUT/ShatterSetup.exe ($(stat -c %s "$OUT/ShatterSetup.exe") bytes)"
