#!/bin/sh
# Build the TB library (Cuik) required by the Odin tilde backend, on macOS.
#
# Usage (from the odin-tilde repo root):
#   sh tilde/scripts/build_tb.sh [debug|release]    default: debug
#
# Expected layout:
#   <any-dir>/odin-tilde/     this repo
#   <any-dir>/cuik-tilde/     https://github.com/vajraimb/cuik-tilde (sibling clone)
#
# Output: tilde/build/libtb_macos_dbg.a (debug) or tilde/build/libtb_macos.a (release)
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CUIK="$(cd "$ROOT/../cuik-tilde" 2>/dev/null && pwd)" || {
  echo "ERROR: sibling repo '../cuik-tilde' not found next to '$ROOT'."
  echo "Clone it first:  git clone https://github.com/vajraimb/cuik-tilde.git"
  exit 1
}
MODE="${1:-debug}"
BUILD="$ROOT/tilde/build"
mkdir -p "$BUILD"

COMMON_FLAGS="-std=c11 -DTB_HAS_X64 -DCUIK_ALLOW_THREADS -D_XOPEN_SOURCE=600 -D_DARWIN_C_SOURCE -DMAP_NORESERVE=0 -DMAP_HUGETLB=0"
INCLUDES="-I $ROOT/tilde/shim -I $CUIK/include -I $CUIK/tb -I $CUIK/common -I $CUIK/cuik_pp -I $CUIK/NBHM"
SRCS="$CUIK/tb/libtb.c $CUIK/tb/x64/x64_target.c $CUIK/common/common.c $CUIK/common/perf.c $CUIK/common/emitter.c"

if [ "$MODE" = "release" ]; then
  OPT="-O2 -DNDEBUG"
  OUT="libtb_macos.a"
  PFX=""
else
  OPT="-O1 -g"
  OUT="libtb_macos_dbg.a"
  PFX="dbg_"
fi

OBJS=""
for f in $SRCS; do
  base="$(basename "$f" .c)"
  obj="$BUILD/${PFX}${base}.o"
  clang $OPT $COMMON_FLAGS $INCLUDES -c "$f" -o "$obj"
  OBJS="$OBJS ${PFX}${base}.o"
done
(cd "$BUILD" && ar rcs "$OUT" $OBJS)
echo "TB_LIB_OK: $BUILD/$OUT"
