#!/usr/bin/env bash
# Statistical fluid runs: each scenario is run SEEDS times with a tiny initial-position jitter through
# both the original core (oracle) and our solver, so analysis/fluid_metrics.py can compare ours against
# the oracle's own chaotic spread.
#   tools/fluid_stat.sh scenarios/fluid/foo.txt [...]
#     -> captures/fluid/<name>.j<seed>/{oracle-1.bin,ours.bin}
# Env: SEEDS (default 8), JITTER (default 1e-5 m), ORACLE=0 / OURS=0 to skip a side.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SEEDS="${SEEDS:-8}" JITTER="${JITTER:-1e-5}"
VAR="$ROOT/captures/fluid/_variants"; mkdir -p "$VAR"
OURS_BIN="$ROOT/build/native/fluid_run"

if [[ "${OURS:-1}" != 0 ]]; then
  mkdir -p "$ROOT/build/native"
  clang++ -std=c++17 -O2 -march=native -o "$OURS_BIN" \
    "$ROOT/solver/fluid/fluid.cpp" "$ROOT/solver/fluid/run_scenario.cpp" ${OURS_FLAGS:-}
fi

variants=()
for sc in "$@"; do
  name="$(basename "$sc" .txt)"
  for s in $(seq 1 "$SEEDS"); do
    v="$VAR/$name.j$s.txt"
    { grep -v '^jitter' "$sc"; echo "jitter $JITTER $s"; } > "$v"
    variants+=("$v")
  done
done

if [[ "${OURS:-1}" != 0 ]]; then
  for v in "${variants[@]}"; do
    out="$ROOT/captures/fluid/$(basename "$v" .txt)"; mkdir -p "$out"
    "$OURS_BIN" "$v" "$out/ours.bin" > "$out/ours.json" &
  done
  wait
  echo "ours: ${#variants[@]} runs done"
fi
if [[ "${ORACLE:-1}" != 0 ]]; then
  "$ROOT/tools/run_fluid_oracle.sh" "${variants[@]}"
fi
