#!/usr/bin/env bash
# Regression check for the bit-exact single-particle probes: rebuild our solver, run it on every
# scenarios/fluid/probe*/ file, and compare against the stored oracle-1.bin.
#   tools/fluid_probe_check.sh            # ORACLE=1 also re-runs the oracle first
# Probes of original artifacts are checked against scenarios/fluid/probe_golden.json (our accepted output);
# after an intended change to them, re-accept with: tools/fluid_probe_check.sh --update-golden
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OURS_BIN="$ROOT/build/native/fluid_run"
mkdir -p "$ROOT/build/native"
clang++ -std=c++17 -O2 -march=native -o "$OURS_BIN" \
  "$ROOT/solver/fluid/fluid.cpp" "$ROOT/solver/fluid/run_scenario.cpp" ${OURS_FLAGS:-}

# probe_dyn/ (dynamic shapes, two-way): our runner replays the bodies from the oracle's bodies-1.bin (no rigid solver)
mapfile -t scen < <(find "$ROOT"/scenarios/fluid/probe* -name '*.txt' | sort)
[[ "${ORACLE:-0}" != 0 ]] && "$ROOT/tools/run_fluid_oracle.sh" "${scen[@]}"
names=()
for sc in "${scen[@]}"; do
  name="$(basename "$sc" .txt)"; out="$ROOT/captures/fluid/$name"; mkdir -p "$out"
  bodies=(); [[ -f "$out/bodies-1.bin" ]] && bodies=("$out/bodies-1.bin" "$out/ours-impulses.txt")
  "$OURS_BIN" "$sc" "$out/ours.bin" "${bodies[@]}" > "$out/ours.json" &
  names+=("$name")
done
wait
python3 "$ROOT/analysis/probe_check.py" "$@" "${names[@]}"
