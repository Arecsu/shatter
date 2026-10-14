#!/usr/bin/env bash
# Run one or more fluid scenarios through the original PhysX core (2.8.1, matched headers).
#   tools/run_fluid_oracle.sh scenarios/fluid/foo.txt [...]   -> captures/fluid/<name>/oracle-<run>.bin + .json
#   and bodies-<run>.bin when the scenario has dynbox/dynsphere/kinbox lines (see fluid_oracle.cpp header)
# RUNS=n repeats each scenario (oracle-vs-oracle spread).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUN="${PX_RUN:-$ROOT/build/run}"
mkdir -p "$RUN"
cp "${HARNESS_OUT:-$ROOT/build/i686}/fluid_oracle.exe" "$RUN/"
cp "$ROOT"/vendor-dlls/game/PhysXLoader.dll "$ROOT"/vendor-dlls/game/PhysXDevice.dll "$ROOT"/vendor-dlls/common/cudart32_60.dll "$RUN/"
export WINEPREFIX="${PX_PREFIX:-$ROOT/.wine-harness}" WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="mscoree,mshtml="
RUNS="${RUNS:-1}"
rc=0
for sc in "$@"; do
  name="$(basename "$sc" .txt)"
  out="$ROOT/captures/fluid/$name"; mkdir -p "$out"
  cp "$sc" "$RUN/scenario.txt"
  for r in $(seq 1 "$RUNS"); do
    rm -f "$RUN/out.bin" "$RUN/bodies.bin"
    (cd "$RUN" && timeout 600 wine fluid_oracle.exe scenario.txt out.bin) > "$out/oracle-$r.json" 2> "$out/oracle-$r.log" || rc=1
    mv "$RUN/out.bin" "$out/oracle-$r.bin" 2>/dev/null || true
    mv "$RUN/bodies.bin" "$out/bodies-$r.bin" 2>/dev/null || true
    echo "$name run $r: $(cat "$out/oracle-$r.json")"
  done
done
wineserver -k 2>/dev/null || true
exit $rc
