#!/usr/bin/env bash
# Build solver/cloth and run scenarios through it -> captures/cloth/<name>/ours-1.bin
# Uses captures/cloth/<name>/cooked.bin (stored edge order) when present.
#   tools/run_cloth_ours.sh [scenario.txt ...]     (default: all scenarios/cloth/*.txt)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OURS_BIN:-$ROOT/build/cloth_ours}"
mkdir -p "$(dirname "$BIN")"
clang++ -std=c++17 -O2 -march=native -o "$BIN" $(ls "$ROOT"/solver/cloth/cloth*.cpp | grep -v cloth_replay) "$ROOT/solver/cloth/run_scenario.cpp" ${OURS_FLAGS:--ffp-contract=off}
files=("$@"); [ ${#files[@]} -eq 0 ] && files=("$ROOT"/scenarios/cloth/*.txt)
for s in "${files[@]}"; do
  n="$(basename "$s" .txt)"; mkdir -p "$ROOT/captures/cloth/$n"
  # edges in cooked-stream order when the oracle dumped cooked.bin for this scenario; OURS_ORDER=first forces the fallback
  extra=()
  if [ -f "$ROOT/captures/cloth/$n/cooked.bin" ] && [ "${OURS_ORDER:-stored}" != first ]; then
    extra=("$ROOT/captures/cloth/$n/cooked.bin")
  fi
  "$BIN" "$s" "$ROOT/captures/cloth/$n/ours-1.bin" "${extra[@]}" >/dev/null
  echo "ran $n"
done
