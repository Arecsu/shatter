#!/usr/bin/env bash
# Fetch xwin (pinned, sha256-checked) and splat the x86 MSVC CRT + Windows SDK into
# .toolchain/xwin. Running this implies accepting Microsoft's license for those files.
# System requirements (pacman): clang lld llvm wine python-pefile
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TC="$ROOT/.toolchain"
XWIN_VER=0.10.0
XWIN_SHA256=d870eb4b2f390878af6da1ccd3cf321d22fcb72720984853b4be732ae597fc88
TARBALL="xwin-$XWIN_VER-x86_64-unknown-linux-musl.tar.gz"

for t in clang-cl lld-link wine python3; do
  command -v "$t" >/dev/null || { echo "missing $t (pacman -S clang lld wine python-pefile)" >&2; exit 1; }
done
python3 -c 'import pefile' 2>/dev/null || { echo "missing python-pefile" >&2; exit 1; }

mkdir -p "$TC/bin"
if [[ ! -x "$TC/bin/xwin" ]]; then
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
  curl -fsSL -o "$tmp/$TARBALL" "https://github.com/Jake-Shadle/xwin/releases/download/$XWIN_VER/$TARBALL"
  echo "$XWIN_SHA256  $tmp/$TARBALL" | sha256sum -c -
  tar -xzf "$tmp/$TARBALL" -C "$tmp"
  install -m755 "$tmp/xwin-$XWIN_VER-x86_64-unknown-linux-musl/xwin" "$TC/bin/xwin"
fi

if [[ ! -d "$TC/xwin/crt/lib/x86" ]]; then
  "$TC/bin/xwin" --accept-license --arch x86 --cache-dir "$TC/xwin-cache" splat --output "$TC/xwin"
  rm -rf "$TC/xwin-cache"
fi
echo "toolchain ready: $("$TC/bin/xwin" --version), $(clang-cl --version | head -1)"
