#!/usr/bin/env bash
# Run one or more cloth scenarios through the original PhysX core (2.8.1, matched headers).
#   tools/run_cloth_oracle.sh scenarios/cloth/foo.txt [...]   -> captures/cloth/<name>/oracle-<run>.bin + .json
# RUNS=n repeats each scenario (oracle-vs-oracle spread).
# Uses its own Wine prefix (.wine-harness-cloth, created on first use) and run dir (build/run-cloth), so it can run
# next to tools/run_fluid_oracle.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUN="${PX_RUN:-$ROOT/build/run-cloth}"
export WINEPREFIX="${PX_PREFIX:-$ROOT/.wine-harness-cloth}" WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="mscoree,mshtml="
[[ -f "$WINEPREFIX/system.reg" ]] || "$ROOT/tools/setup_prefix.sh"
[[ -f "$WINEPREFIX/.gitignore" ]] || printf '*\n' > "$WINEPREFIX/.gitignore"   # keep the prefix out of git
mkdir -p "$RUN"
cp "${HARNESS_OUT:-$ROOT/build/i686}/cloth_oracle.exe" "$RUN/"
cp "$ROOT"/vendor-dlls/game/PhysXLoader.dll "$ROOT"/vendor-dlls/game/PhysXDevice.dll "$ROOT"/vendor-dlls/common/cudart32_60.dll "$RUN/"
RUNS="${RUNS:-1}"
rc=0
for sc in "$@"; do
  name="$(basename "$sc" .txt)"
  out="$ROOT/captures/cloth/$name"; mkdir -p "$out"
  cp "$sc" "$RUN/scenario.txt"
  for r in $(seq 1 "$RUNS"); do
    (cd "$RUN" && timeout 600 wine cloth_oracle.exe scenario.txt out.bin) > "$out/oracle-$r.json" 2> "$out/oracle-$r.log" || rc=1
    mv "$RUN/out.bin" "$out/oracle-$r.bin" 2>/dev/null || true
    mv "$RUN/cooked.bin" "$out/cooked.bin" 2>/dev/null || true   # cooked mesh stream (same for every run)
    echo "$name run $r: $(cat "$out/oracle-$r.json")"
  done
done
wineserver -k 2>/dev/null || true   # WINEPREFIX is ours (exported above)
exit $rc
