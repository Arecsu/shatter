#!/usr/bin/env bash
# One command: bootstrap toolchain (if needed), copy the user's PhysX DLLs, prepare
# the throwaway prefix, build the 32-bit harness, run it in both modes, and write
# captures/hello.json. Exit code 0 iff both runs pass.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

tools/bootstrap_toolchain.sh
[[ -f vendor-dlls/SHA256SUMS ]] || tools/sync_vendor_dlls.sh >/dev/null
tools/setup_prefix.sh 2>&1 | grep -v -e MESA -e 'pci id' || true
harness/build.sh 2>&1 | grep -v -e LNK4099 -e 'failed to load reference' || true

# Stage like the game's Binaries/ dir: exe + game loader + device module + CUDA runtime
RUN="$ROOT/build/run"
mkdir -p "$RUN" captures
cp build/i686/hello.exe "$RUN/"
cp vendor-dlls/game/PhysXLoader.dll vendor-dlls/game/PhysXDevice.dll vendor-dlls/common/cudart32_60.dll "$RUN/"

export WINEPREFIX="$ROOT/.wine-harness" WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="mscoree,mshtml="
run() {  # label args...
  local label=$1; shift
  local log="captures/hello-$label.log" rc=0 json
  (cd "$RUN" && timeout 120 wine hello.exe "$@") >"captures/hello-$label.out" 2>"$log" || rc=$?
  json="$(grep '^{' "captures/hello-$label.out" | tail -1)"
  [[ -n "$json" ]] || json='{"ok":false,"error":"no output"}'
  printf '"%s":{"exit_code":%d,"result":%s}' "$label" "$rc" "$json"
}
{
  printf '{"wine":"%s",' "$(wine --version)"
  run abi281 --abi 2.8.1; printf ','
  run probe280 --abi 2.8.0-probe
  printf '}\n'
} > captures/hello.json
wineserver -k 2>/dev/null || true

python3 - <<'EOF'
import json, sys
d = json.load(open("captures/hello.json"))
print(json.dumps(d, indent=1))
ok = all(d[k]["exit_code"] == 0 and d[k]["result"].get("ok") for k in ("abi281", "probe280"))
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
EOF
