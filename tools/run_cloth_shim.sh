#!/usr/bin/env bash
# Run cloth scenarios through the oracle with the PhysXLoader.dll shim in front of the game loader.
#   tools/run_cloth_shim.sh scenarios/cloth/foo.txt [...]
#   -> captures/cloth/<name>/$SHIM_TAG-<run>.{bin,json,log,shimlog} (default SHIM_TAG=shim)
# Compare with the plain oracle: analysis/clothbin.py compare captures/cloth/<name>/oracle-1.bin <...>/shim-1.bin
# Uses the .wine-harness-cloth prefix unless PX_PREFIX is set, and build/run-cloth unless PX_RUN is set
# (same defaults as tools/run_cloth_oracle.sh; the shim DLL comes from SHIM_OUT, the oracle from HARNESS_OUT).
# PXSHIM_CLOTH=0 turns the cloth layer off (default here: off = observed, original core); PXSHIM_CLOTH_MESH=1 hooks createClothMesh on core 2.8.0 too.
# RUNS=n repeats each scenario.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUN="${PX_RUN:-$ROOT/build/run-cloth}"
export WINEPREFIX="${PX_PREFIX:-$ROOT/.wine-harness-cloth}" WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="mscoree,mshtml="
[[ -f "$WINEPREFIX/system.reg" ]] || "$ROOT/tools/setup_prefix.sh"
[[ -f "$WINEPREFIX/.gitignore" ]] || printf '*\n' > "$WINEPREFIX/.gitignore"   # keep the prefix out of git
mkdir -p "$RUN"
cp "${HARNESS_OUT:-$ROOT/build/i686}/cloth_oracle.exe" "$RUN/"
cp "${SHIM_OUT:-$ROOT/build/shim}/PhysXLoader.dll" "$RUN/PhysXLoader.dll"
# NO_ORIG=1: no PhysXLoader_orig.dll, the shim finds the core itself (as installed for players).
if [[ "${NO_ORIG:-0}" == 1 ]]; then rm -f "$RUN/PhysXLoader_orig.dll"
else cp "$ROOT/vendor-dlls/game/PhysXLoader.dll" "$RUN/PhysXLoader_orig.dll"; fi
cp "$ROOT/vendor-dlls/game/PhysXDevice.dll" "$ROOT/vendor-dlls/common/cudart32_60.dll" "$RUN/"
# The shim hosts cloths and fluids by default; here the default stays the original core (observe only).
# Pass PXSHIM_CLOTH=ours to run our solver.
export PXSHIM_DEBUG="${PXSHIM_DEBUG:-1}"  # shim log (collected as .shimlog)
export PXSHIM_CLOTH="${PXSHIM_CLOTH:-off}" PXSHIM_FLUID="${PXSHIM_FLUID:-off}"
# Self contact (ours, not in the original) stays off here so hosted runs compare with the oracle; PXSHIM_CLOTH_SELF=0.2 to test it.
export PXSHIM_CLOTH_SELF="${PXSHIM_CLOTH_SELF:-0}"
TAG="${SHIM_TAG:-shim}"
RUNS="${RUNS:-1}"
rc=0
for sc in "$@"; do
  name="$(basename "$sc" .txt)"
  out="$ROOT/captures/cloth/$name"; mkdir -p "$out"
  cp "$sc" "$RUN/scenario.txt"
  for r in $(seq 1 "$RUNS"); do
    rm -f "$RUN/out.bin" "$RUN/PhysXLoader_shim.log"
    (cd "$RUN" && timeout 600 wine cloth_oracle.exe scenario.txt out.bin) > "$out/$TAG-$r.json" 2> "$out/$TAG-$r.log" || rc=1
    mv "$RUN/out.bin" "$out/$TAG-$r.bin" 2>/dev/null || true
    mv "$RUN/PhysXLoader_shim.log" "$out/$TAG-$r.shimlog" 2>/dev/null || true
    echo "$name $TAG run $r: $(cat "$out/$TAG-$r.json")"
  done
done
wineserver -k 2>/dev/null || true   # WINEPREFIX is ours (exported above)
exit $rc
