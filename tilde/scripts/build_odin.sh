#!/bin/sh
# Build Odin with the tilde (Cuik TB) backend enabled, on macOS.
#
# Usage (from the odin-tilde repo root):
#   sh tilde/scripts/build_odin.sh [debug|release]    default: debug
#
# Requires the TB library first:  sh tilde/scripts/build_tb.sh [debug|release]
# Output: ./odin  (repo root, as with the stock build)
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CUIK="$(cd "$ROOT/../cuik-tilde" 2>/dev/null && pwd)" || {
  echo "ERROR: sibling repo '../cuik-tilde' not found next to '$ROOT'."
  echo "Clone it first:  git clone https://github.com/vajraimb/cuik-tilde.git"
  exit 1
}
cd "$ROOT"
MODE="${1:-debug}"

if [ "$MODE" = "release" ]; then
  LIB="$ROOT/tilde/build/libtb_macos.a"
else
  LIB="$ROOT/tilde/build/libtb_macos_dbg.a"
fi
[ -f "$LIB" ] || {
  echo "ERROR: TB library not found at '$LIB'."
  echo "Build it first:  sh tilde/scripts/build_tb.sh $MODE"
  exit 1
}

CPPFLAGS="-DODIN_TILDE_BACKEND" CXXFLAGS="-I $CUIK/include" LDFLAGS="$LIB" \
  sh build_odin.sh "$MODE" && echo ODIN_OK || {
  echo ODIN_FAIL
  exit 1
}
