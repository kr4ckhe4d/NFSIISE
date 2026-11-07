#!/usr/bin/env bash
# Builds the Glide renderer test scene and renders it headless (Xvfb + Mesa).
#
#   tools/gfxtest/run.sh <out.png> [Key=Value ...]          - new renderer
#   GFXTEST_SRC=/path/to/old/src tools/gfxtest/run.sh ...    - another source tree (e.g. original renderer)
#   GFXTEST_OLD=1 ...                                        - source tree without graphics enhancements
#
# Environment: GFXTEST_SIZE=WxH (default 1280x960), GFXTEST_FRAMES=N, GFXTEST_NO_XVFB=1 (use current display).

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${GFXTEST_SRC:-$HERE/../../src}"
BIN="${GFXTEST_BIN:-${TMPDIR:-/tmp}/gfxtest-$(echo "$SRC" | md5sum | cut -c1-8)}"
DEFS="-DNEW_RENDERER"
[[ -n "$GFXTEST_OLD" ]] && DEFS=""

# Wrapper.h maps the Glide API to stdcall on x86-32, exactly like the game build
gcc -m32 -O2 -std=gnu89 -w -DSTACK_REALIGN $DEFS -I"$SRC" "$HERE/gfxtest.c" -o "$BIN" -lSDL2 -lGL -lm

if [[ -n "$GFXTEST_NO_XVFB" ]]; then
	"$BIN" "$@"
else
	SIZE="${GFXTEST_SIZE:-1280x960}"
	xvfb-run -a -s "-screen 0 ${SIZE}x24" "$BIN" "$@"
fi
