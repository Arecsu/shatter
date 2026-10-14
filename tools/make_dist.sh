#!/usr/bin/env bash
# Build fresh player packages; --dll-only skips the installer. Version comes from shim/version.h.
set -euo pipefail
export TZ=UTC
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
v() { awk -v k="SHATTER_VERSION_$1" '$2 == k {print $3}' "$ROOT/shim/version.h"; }
VER="$(v MAJOR).$(v MINOR).$(v PATCH)"
DEFAULT_LICENSE="MIT (see LICENSE.txt). Not affiliated with NVIDIA or EA; PhysX and Mirror's Edge belong to their owners."
LICENSE_TEXT="${LICENSE_TEXT:-$DEFAULT_LICENSE}"

case "${1:-}" in
  "") "$ROOT/installer/build.sh" ;;
  --dll-only) "$ROOT/shim/build.sh" release ;;
  *) echo "usage: tools/make_dist.sh [--dll-only]" >&2; exit 2 ;;
esac
STAGE="$ROOT/build/dist/Shatter-$VER"
rm -rf "$STAGE"; mkdir -p "$STAGE" "$ROOT/dist"
cp "$ROOT/build/shim-release/PhysXLoader.dll" "$STAGE/"
sed -e "s/%VERSION%/$VER/" -e "s/%LICENSE%/$LICENSE_TEXT/" "$ROOT/package/README.txt" | sed 's/$/\r/' > "$STAGE/README.txt"
sed 's/$/\r/' "$ROOT/package/shatter.ini" > "$STAGE/shatter.ini"
sed 's/$/\r/' "$ROOT/LICENSE" > "$STAGE/LICENSE.txt"
ZIP="$ROOT/dist/Shatter-$VER.zip"
rm -f "$ZIP"
(cd "$ROOT/build/dist" && find "Shatter-$VER" -exec touch -t 202601010000 {} + && zip -X -q -r "$ZIP" "Shatter-$VER")
echo "zip: $ZIP"; unzip -l "$ZIP" | tail -n +2
if [[ "${1:-}" != --dll-only ]]; then
  cp "$ROOT/build/installer/ShatterSetup.exe" "$ROOT/dist/ShatterSetup-$VER.exe"
  echo "installer: $ROOT/dist/ShatterSetup-$VER.exe"
fi
