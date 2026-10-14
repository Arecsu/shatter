#!/usr/bin/env bash
# Replay a game cloth recording (.pxcr) with the shim's own build (i686, clang-cl, MSVC CRT) under Wine.
#   tools/cloth_replay_w32.sh file.pxcr [cloth_replay options, e.g. --isect 5]
# The native x86-64 build of solver/cloth/cloth_replay.cpp drifts from some game runs (run 20261012-164610 cloth057:
# position hashes differ from step 60) while this build reproduces them bit for bit, so use it to see what the game
# showed. Built into build/replay-w32/ when a solver source is newer than the exe. Uses the .wine-harness-cloth prefix.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XWIN="${XWIN:-$ROOT/.toolchain/xwin}"
OUT="$ROOT/build/replay-w32"
EXE="$OUT/cloth_replay.exe"
mkdir -p "$OUT"
if [[ ! -f "$EXE" ]] || [[ -n "$(find "$ROOT/solver/cloth" "$ROOT/solver/fluid/thread_pool.h" -newer "$EXE" -print -quit)" ]]; then
  clang-cl --target=i686-pc-windows-msvc -fuse-ld=lld-link /nologo /O2 /MT /EHsc /std:c++17 -Wno-microsoft \
    /DNOMINMAX /DWIN32 /D_CRT_SECURE_NO_WARNINGS \
    /imsvc "$XWIN/crt/include" /imsvc "$XWIN/sdk/include/ucrt" /imsvc "$XWIN/sdk/include/um" /imsvc "$XWIN/sdk/include/shared" \
    "$ROOT"/solver/cloth/cloth*.cpp /Fo"$OUT/" /Fe"$EXE" \
    /link /machine:x86 /libpath:"$XWIN/crt/lib/x86" /libpath:"$XWIN/sdk/lib/um/x86" /libpath:"$XWIN/sdk/lib/ucrt/x86" >&2
fi
export WINEPREFIX="${PX_PREFIX:-$ROOT/.wine-harness-cloth}" WINEDEBUG="${WINEDEBUG:--all}"
exec wine "$EXE" "$@"
