#!/usr/bin/env bash
# Create/refresh the throwaway 32-bit-capable Wine prefix used by the harness.
# Mirrors the registry layout the PhysX loader expects:
#   HKLM\Software\AGEIA Technologies\PhysXCore Path = C:\PhysX\Engine
# and the loader appends \v<maj>.<min>.<bugfix>\PhysXCore.dll.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export WINEPREFIX="${WINEPREFIX:-$ROOT/.wine-harness}"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="mscoree,mshtml="
WINE="${WINE:-wine}"

[[ -d "$ROOT/vendor-dlls/engine" ]] || "$ROOT/tools/sync_vendor_dlls.sh" >/dev/null

if [[ ! -f "$WINEPREFIX/system.reg" ]]; then
  echo "creating prefix $WINEPREFIX"
  wineboot -i >/dev/null 2>&1
fi

ENG="$WINEPREFIX/drive_c/PhysX/Engine"
mkdir -p "$ENG"
cp -rp "$ROOT/vendor-dlls/engine/." "$ENG/"

# Key in both registry views; the loader is 32-bit (reads WOW64 view) but be explicit.
for view in 32 64; do
  "$WINE" reg add 'HKLM\Software\AGEIA Technologies' /v 'PhysXCore Path' /t REG_SZ \
    /d 'C:\PhysX\Engine' /f /reg:$view >/dev/null
  # Force software simulation: no GPU path in the harness prefix (no nvcuda.dll anyway).
  "$WINE" reg add 'HKLM\Software\AGEIA Technologies' /v HwSelection /t REG_SZ /d CPU /f /reg:$view >/dev/null
done
wineserver -w
echo "prefix ready: $WINEPREFIX"
