#!/usr/bin/env bash
# Launch Mirror's Edge from the project's clone of the game's Wine prefix with the observing shim.
#   tools/run_game_logged.sh [game args, e.g. a map name]   -> logs in captures/game-run/<timestamp>/
#   tools/run_game_logged.sh <spot>              start at a test spot (list: docs/test-spots.md, or run with "spots")
#   tools/run_game_logged.sh Map_p:Checkpoint    any checkpoint ("Map_p?Loadcheckpoint=Checkpoint -nostartupmovies")
# Shim switches pass through the environment. Our fluids and cloths run by default; the original core:
#   PXSHIM_FLUID=off PXSHIM_CLOTH=off tools/run_game_logged.sh Escape_p
# Other switches, e.g. PXSHIM_FLUID=ours PXSHIM_TRACE=1 PXSHIM_GEOM_CENSUS=1 PXSHIM_CLOTH_TRACE=1
# (ours = our solver runs only the collide-only fluids; TRACE=1 records the fluid API calls; CENSUS logs the scene
# geometry; CLOTH_TRACE=1 logs the NxCloth calls between simulates when they change)
#
# .wine-game/ is a btrfs reflink copy (cp -a --reflink=always) of the game's Wine prefix, game included,
# so the real prefix and install stay untouched. Its Binaries/PhysXLoader.dll (and the identical
# PhysXLocal/ copy) are the shim, installed drop-in (no PhysXLoader_orig.dll: the shim finds the core itself, as for
# players). ORIG=1 puts the original loader back next to it as PhysXLoader_orig.dll (old dev setup).
# RELEASE=1 installs the release build (build/shim-release) and leaves PXSHIM_DEBUG unset: no log, as players get it.
# Otherwise PXSHIM_DEBUG=1 (shim log). Cloth recordings: PXSHIM_CLOTH_REC=1.
# FPS=n sets the frame cap in the clone's TdEngine.ini (MaxSmoothedFrameRate, smoothing on); it stays until changed.
# LOCAL_CORE=path/PhysXCore.dll puts that core next to the exe for this run (some copies of the game ship 2.8.0.7 there,
# which the loader then prefers over the system one); without it any PhysXCore.dll there is removed.
# PXSHIM_SLIDEFIX=1: the slide steering fix (off by default, shim/game_hooks.cpp).
# PXSHIM_MOVELOG=1: movement/aim logger (shim/game_hooks.cpp) -> movelog.bin in the run's log dir (tools/movelog.py).
# Machine-specific settings (PROTONPATH, the GE-Proton directory) go in .local.env at the repo root (gitignored).
# Launch uses umu-run with the configured Proton runner and launcher defaults
# (esync/fsync on, no extra env). GAMEID umu-0 is an assumption for a sideloaded app [A].
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PFX="$ROOT/.wine-game"
BIN="$PFX/drive_c/Mirror's Edge/Binaries"
OUT="$ROOT/captures/game-run/$(date +%Y%m%d-%H%M%S)"
[[ -f "$ROOT/.local.env" ]] && source "$ROOT/.local.env"
: "${PROTONPATH:?set PROTONPATH (GE-Proton directory) in .local.env}"
[[ -f "$BIN/PhysXLoader.dll" ]] || { echo "no game in $BIN" >&2; exit 1; }

# Test spots: checkpoint nearest to the content (docs/test-spots.md). The URL form is the one the exe itself carries
# ("escape_p?Loadcheckpoint=ChaseFlyby ... -nostartupmovies").
declare -A SPOTS=(
  [subway]="Subway_p:Renovation_Fight"      # tearable sheet with valid bounds (36 m); in the exe's own start list
  [escape]="Escape_p:Chase_SolarPanels"     # heavy vent heli smoke, many cloths with run-time valid bounds
  [mall]="Mall_p:After_fan"                 # tearable plastic sheets
  [cranes]="Cranes_p:SP03_Afterbalance"     # crane cloths with valid bounds
  [scraper]="Scraper_p:Out_soft"            # explosion smoke
  [factory]="Factory_p:Conveyor_puzzle"     # coolant smoke
  [stormdrain]="Stormdrain_p:Waterfall"     # waterslide smoke + waterfall
  [boat]="Boat_p:Celeste_Chase"             # ship steam
  [edge]="Edge_p:Cops"                      # shootout: glass and debris
)
if [[ "${1:-}" == spots ]]; then for k in "${!SPOTS[@]}"; do echo "$k -> ${SPOTS[$k]}"; done | sort; exit 0; fi
# MirrorsEdge.exe throws its command line away: at start-up it copies a fixed string over it (0x010eb38a: the flyby demo
# URL with -FlybyFlight, "-nostartupmovies" with -nostartupmovies, otherwise empty) [V: disassembly]. So a map on the
# command line never reaches the engine, which then loads [URL] LocalMap from TdEngine.ini. A spot is started by writing
# its URL there in the clone (form of the exe's own list: "Subway_p?LoadCheckpoint=X?Game=TdGame.TdSpStoryGame");
# any launch without a spot puts TdMainMenu back.
ENGINE_INI="$PFX/drive_c/users/steamuser/Documents/EA Games/Mirror's Edge/TdGame/Config/TdEngine.ini"
INPUT_INI="$PFX/drive_c/users/steamuser/Documents/EA Games/Mirror's Edge/TdGame/Config/TdInput.ini"
localmap="TdMainMenu"
if [[ $# -ge 1 ]]; then
  spot="${SPOTS[$1]:-$1}"
  if [[ "$spot" == *_p:* ]]; then
    shift
    localmap="${spot%%:*}?LoadCheckpoint=${spot#*:}?Game=TdGame.TdSpStoryGame"
    set -- -nostartupmovies "$@"
  fi
fi
python3 - "$ENGINE_INI" "$INPUT_INI" "$localmap" "${FPS:-}" <<'PY'
import sys
engine, inp, url, fps = sys.argv[1:5]
lines = open(engine, newline="").read().splitlines(True)
sec = None
for k, l in enumerate(lines):
    t = l.strip()
    if t.startswith("["): sec = t
    elif sec == "[URL]" and t.startswith("LocalMap="): lines[k] = "LocalMap=%s\r\n" % url
    elif fps and sec == "[Engine.GameEngine]" and t.startswith("MaxSmoothedFrameRate="):
        lines[k] = "MaxSmoothedFrameRate=%s\r\n" % fps
    elif fps and sec == "[Engine.GameEngine]" and t.startswith("bSmoothFrameRate="): lines[k] = "bSmoothFrameRate=TRUE\r\n"
open(engine, "w", newline="").write("".join(lines))
# an earlier version bound F11 to "start <url>" (the game ignored it at the main menu): remove it
lines = open(inp, newline="").read().splitlines(True)
open(inp, "w", newline="").write("".join(l for l in lines if 'Name="F11"' not in l))
PY
echo "start map ([URL] LocalMap): $localmap"
grep -a -h "^MaxSmoothedFrameRate=" "$ENGINE_INI" | tr -d '\r' | sed 's/^/frame cap: /' || true
echo "game args: $*"

# Refresh the shim from the build.
SHIM="$ROOT/build/shim/PhysXLoader.dll"
if [[ "${RELEASE:-0}" == 1 ]]; then SHIM="$ROOT/build/shim-release/PhysXLoader.dll"; else export PXSHIM_DEBUG="${PXSHIM_DEBUG:-1}"; fi
cp "$SHIM" "$BIN/PhysXLoader.dll"
cp "$SHIM" "$BIN/PhysXLocal/PhysXLoader.dll"
for d in "$BIN" "$BIN/PhysXLocal"; do
  if [[ "${ORIG:-0}" == 1 ]]; then cp "$ROOT/vendor-dlls/game/PhysXLoader.dll" "$d/PhysXLoader_orig.dll"
  else rm -f "$d/PhysXLoader_orig.dll"; fi
done
rm -f "$BIN/PhysXLoader_shim.log" "$BIN/PhysXLocal/PhysXLoader_shim.log" "$BIN/PhysXLoader_fluid.trace" "$BIN"/PhysXLoader_step*.pxsd "$BIN"/PhysXLoader_cloth*.pxcr "$BIN/movelog.bin" "$BIN/PhysXLocal/movelog.bin"
if [[ -n "${LOCAL_CORE:-}" ]]; then cp "$LOCAL_CORE" "$BIN/PhysXCore.dll"; echo "local core: $LOCAL_CORE"
else rm -f "$BIN/PhysXCore.dll"; fi
mkdir -p "$OUT"
env | grep '^PXSHIM_' > "$OUT/shim_env.txt" || true

export WINEPREFIX="$PFX"
export PROTONPATH
export GAMEID="umu-0"
export PROTON_VERB="waitforexitandrun"
export PROTON_LOG=1 PROTON_LOG_DIR="$OUT"

cd "$BIN"
umu-run "$BIN/MirrorsEdge.exe" "$@" > "$OUT/umu.log" 2>&1 || echo "game exited with $?" | tee -a "$OUT/umu.log"

for f in "$BIN/PhysXLoader_shim.log" "$BIN/PhysXLocal/PhysXLoader_shim.log"; do
  [[ -f "$f" ]] && cp "$f" "$OUT/$(basename "$(dirname "$f")")_shim.log"
done
[[ -f "$BIN/PhysXLoader_fluid.trace" ]] && mv "$BIN/PhysXLoader_fluid.trace" "$OUT/fluid.trace"
for f in "$BIN"/PhysXLoader_step*.pxsd "$BIN"/PhysXLoader_cloth*.pxcr "$BIN/movelog.bin" "$BIN/PhysXLocal/movelog.bin"; do [[ -f "$f" ]] && mv "$f" "$OUT/"; done
echo "logs: $OUT"
grep -h "coverage (" "$OUT"/*shim.log 2>/dev/null | tail -1 || true
command ls -la "$OUT"
