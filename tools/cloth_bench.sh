#!/usr/bin/env bash
# Cloth solver cost, working tree vs a git ref (default HEAD): replays game recordings (.pxcr) with the i686 replay
# build of each, one at a time, single-threaded, and prints the solver ms per step of both.
#   tools/cloth_bench.sh [ref] [recordings...]   (default: every captures/game-run/*/*.pxcr)
# Both replay the game's input with the velocity writes as changes (--dual), so each runs its own solver. Keep the
# machine otherwise idle; timings of a busy machine say nothing.
set -euo pipefail
export LC_ALL=C
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REF="${1:-HEAD}"; shift || true
files=("$@"); [ ${#files[@]} -eq 0 ] && mapfile -t files < <(find "$ROOT/captures/game-run" -name '*.pxcr' | sort)
OUT="$ROOT/build/bench"; mkdir -p "$OUT"
# the ref's solver sources in a temporary tree, built with the same script
REFDIR="$(mktemp -d)"; trap 'rm -rf "$REFDIR"' EXIT
mkdir -p "$REFDIR/tools" "$REFDIR/solver"
git -C "$ROOT" archive "$REF" solver tools/cloth_replay_w32.sh | tar -x -C "$REFDIR"
ln -s "$ROOT/.toolchain" "$REFDIR/.toolchain"
XWIN="$ROOT/.toolchain/xwin" bash "$REFDIR/tools/cloth_replay_w32.sh" >/dev/null 2>&1 || true
cp "$REFDIR/build/replay-w32/cloth_replay.exe" "$OUT/ref.exe"
bash "$ROOT/tools/cloth_replay_w32.sh" >/dev/null 2>&1 || true
cp "$ROOT/build/replay-w32/cloth_replay.exe" "$OUT/new.exe"
export WINEPREFIX="$ROOT/.wine-harness-cloth" WINEDEBUG=-all
ms() { wine "$OUT/$1.exe" "$2" --quiet --dual 2>/dev/null | sed -nE 's/^solver: ([0-9.]+) ms per step.*/\1/p'; }
printf '%-42s %9s %9s %7s\n' recording "$REF" working change
tr=0; tn=0
for f in "${files[@]}"; do
  r=$(ms ref "$f"); n=$(ms new "$f")
  [ -z "$r" ] || [ -z "$n" ] && continue
  tr=$(echo "$tr + $r" | bc -l); tn=$(echo "$tn + $n" | bc -l)
  printf '%-42s %9.3f %9.3f %+6.0f%%\n' "$(basename "$(dirname "$f")")/$(basename "$f" .pxcr)" "$r" "$n" "$(echo "($n / $r - 1) * 100" | bc -l)"
done
printf '%-42s %9.3f %9.3f %+6.0f%%\n' "sum (ms per step)" "$tr" "$tn" "$(echo "($tn / $tr - 1) * 100" | bc -l)"
