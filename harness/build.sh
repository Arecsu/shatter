#!/usr/bin/env bash
# Build 32-bit MSVC-ABI harness binaries with clang-cl + lld-link against the
# xwin-splatted CRT/SDK. Output: build/i686/*.exe
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XWIN="${XWIN:-$ROOT/.toolchain/xwin}"
PHYSX_SDK="${PHYSX_SDK:-$ROOT/third_party/physx-sdk-2.8.1/SDKs}"
OUT="${HARNESS_OUT:-$ROOT/build/i686}"
mkdir -p "$OUT"

[[ -d "$XWIN/crt" ]] || { echo "xwin CRT/SDK missing at $XWIN (run tools/bootstrap_toolchain.sh)" >&2; exit 1; }
[[ -f "$PHYSX_SDK/Physics/include/NxPhysics.h" ]] || {
  echo "PhysX 2.8.1 SDK headers missing at $PHYSX_SDK (see CONTRIBUTING.md)" >&2; exit 1; }

CFLAGS=(
  --target=i686-pc-windows-msvc -fuse-ld=lld-link
  /nologo /O2 /EHsc /MT /W3 /Zi /std:c++17
  -Wno-microsoft -Wno-unused-private-field -Wno-deprecated-declarations
  /DWIN32 /D_WINDOWS /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS
  /imsvc "$XWIN/crt/include"
  /imsvc "$XWIN/sdk/include/ucrt" /imsvc "$XWIN/sdk/include/um" /imsvc "$XWIN/sdk/include/shared"
  /imsvc "$PHYSX_SDK/Foundation/include" /imsvc "$PHYSX_SDK/Physics/include"
  /imsvc "$PHYSX_SDK/PhysXLoader/include" /imsvc "$PHYSX_SDK/Cooking/include"
)
LDFLAGS=(
  /link /libpath:"$XWIN/crt/lib/x86" /libpath:"$XWIN/sdk/lib/um/x86" /libpath:"$XWIN/sdk/lib/ucrt/x86"
  /machine:x86 /subsystem:console /debug version.lib
)

build() {  # name src...
  local name=$1; shift
  clang-cl "${CFLAGS[@]}" "$@" /Fo"$OUT/" /Fe"$OUT/$name.exe" "${LDFLAGS[@]}"
}

build hello "$ROOT/harness/src/hello.cpp"
build fluid_oracle "$ROOT/harness/src/fluid_oracle.cpp"
build cloth_oracle "$ROOT/harness/src/cloth_oracle.cpp"
build cloth_replay_oracle "$ROOT/harness/src/cloth_replay_oracle.cpp"

# compile_commands.json for editors/clangd
python3 - "$ROOT" "${CFLAGS[@]}" <<'EOF'
import json, sys, glob, os
root, flags = sys.argv[1], sys.argv[2:]
srcs = glob.glob(os.path.join(root, "harness/src/*.cpp"))
json.dump([{"directory": root, "file": s, "arguments": ["clang-cl", *flags, "/c", s]} for s in srcs],
          open(os.path.join(root, "compile_commands.json"), "w"), indent=1)
EOF
echo "built: $(ls "$OUT"/*.exe | xargs -n1 basename | tr '\n' ' ')"
