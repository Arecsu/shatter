#!/usr/bin/env bash
# Run fluid scenarios through the oracle with the PhysXLoader.dll shim in front of the game loader.
#   tools/run_fluid_shim.sh scenarios/fluid/foo.txt [...]
#   -> captures/fluid/<name>/$SHIM_TAG-<run> (default shim-<run>).{bin,json,log,trace,shimlog} and $SHIM_TAG-<run>-bodies.bin
# Uses the .wine-shim/ prefix and build/shim/run-fluid/, so it runs alongside run_fluid_oracle.sh.
# PXSHIM_TRACE (default 2) and PXSHIM_* switches are passed through to the shim. RUNS=n repeats.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUN="${PX_RUN:-$ROOT/build/shim/run-fluid}"
mkdir -p "$RUN"
cp "${HARNESS_OUT:-$ROOT/build/i686}/fluid_oracle.exe" "$RUN/"
cp "${SHIM_OUT:-$ROOT/build/shim}/PhysXLoader.dll" "$RUN/PhysXLoader.dll"
# NO_ORIG=1: no PhysXLoader_orig.dll, the shim finds the core itself (as installed for players).
if [[ "${NO_ORIG:-0}" == 1 ]]; then rm -f "$RUN/PhysXLoader_orig.dll"
else cp "$ROOT/vendor-dlls/game/PhysXLoader.dll" "$RUN/PhysXLoader_orig.dll"; fi
cp "$ROOT/vendor-dlls/game/PhysXDevice.dll" "$ROOT/vendor-dlls/common/cudart32_60.dll" "$RUN/"
export WINEPREFIX="${PX_PREFIX:-$ROOT/.wine-shim}" WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="mscoree,mshtml="
[[ -f "$WINEPREFIX/system.reg" ]] || "$ROOT/tools/setup_prefix.sh"
export PXSHIM_DEBUG="${PXSHIM_DEBUG:-1}"  # shim log (collected as .shimlog)
export PXSHIM_TRACE="${PXSHIM_TRACE:-2}"
# The shim hosts fluids and cloths by default; here the default stays the original core (observe only).
# Pass PXSHIM_FLUID=ours or ours-all to run our solver.
export PXSHIM_FLUID="${PXSHIM_FLUID:-off}" PXSHIM_CLOTH="${PXSHIM_CLOTH:-off}"
RUNS="${RUNS:-1}"
TAG="${SHIM_TAG:-shim}"  # output prefix, e.g. SHIM_TAG=ours with PXSHIM_FLUID=ours
rc=0
for sc in "$@"; do
  name="$(basename "$sc" .txt)"
  out="$ROOT/captures/fluid/$name"; mkdir -p "$out"
  cp "$sc" "$RUN/scenario.txt"
  for r in $(seq 1 "$RUNS"); do
    rm -f "$RUN/out.bin" "$RUN/bodies.bin" "$RUN/PhysXLoader_shim.log" "$RUN/PhysXLoader_fluid.trace"
    (cd "$RUN" && timeout 600 wine fluid_oracle.exe scenario.txt out.bin) > "$out/$TAG-$r.json" 2> "$out/$TAG-$r.log" || rc=1
    mv "$RUN/out.bin" "$out/$TAG-$r.bin" 2>/dev/null || true
    mv "$RUN/bodies.bin" "$out/$TAG-$r-bodies.bin" 2>/dev/null || true
    mv "$RUN/PhysXLoader_fluid.trace" "$out/$TAG-$r.trace" 2>/dev/null || true
    mv "$RUN/PhysXLoader_shim.log" "$out/$TAG-$r.shimlog" 2>/dev/null || true
    echo "$name $TAG run $r: $(cat "$out/$TAG-$r.json")"
  done
done
wineserver -k 2>/dev/null || true
exit $rc
