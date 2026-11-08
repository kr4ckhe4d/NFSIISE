#!/usr/bin/env bash
# Headless game test: starts the game on a virtual X display, drives the menus into a race and takes screenshots.
#
#   tools/gametest/race.sh [options] [VAR=value ...]     (VAR=value are passed to the game's environment)
#
# Options (environment variables):
#   TRACK=N         location: number of "Right" presses in the location menu (0 Proving Grounds - rain at night,
#                   1 Outback - sunset, 2 beach, 3 tunnel, 4 city - rain, 5 cliffs, 6 market; 7 wraps around)
#   CAMERA=chase    press C after the start for the chase camera (default: in-car/bumper view)
#   OUT=dir         output directory for game.log and screenshots (default: $NFS2SE_TEST_DIR/out)
#   SIZE=WxH        window size (default 2560x1080)
#   CONF="K=V ..."  extra nfs2se.conf lines (space separated)
#   MODELS=dir      replacement car models to install (copied to <settings>/models)
#   TEXTURES=dir    HD textures to install (copied to <settings>/textures)
#   SKIES=dir       skies to install (copied to <settings>/skies)
#   KEEP=1          leave the game running after the screenshot (drive it with tools/gfxtest/xkey)
#   BIN=cmd         game command (default ./nfs2se; e.g. "gdb -batch -x cmds --args ./nfs2se")
#
# Needs Xvfb, ImageMagick (import) and libXtst. Screenshots: $OUT/race.png (after the start).
# Menu automation is timing based and sometimes lands on a wrong screen; the script checks for the red race timer
# in the HUD and retries up to 3 times. Software rendering on Xvfb runs the game at only ~10-20 fps.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
GAME="$REPO/Need For Speed II SE"
TEST="${NFS2SE_TEST_DIR:-${TMPDIR:-/tmp}/nfs2se-test}"
OUT="${OUT:-$TEST/out}"
SIZE="${SIZE:-2560x1080}"
TRACK="${TRACK:-0}"
export DISPLAY="${XVFB_DISPLAY:-:77}"
mkdir -p "$TEST" "$OUT"

# Virtual display and key sender
if ! xdpyinfo >/dev/null 2>&1; then
	Xvfb "$DISPLAY" -screen 0 "${SIZE}x24" >/dev/null 2>&1 &
	sleep 2
fi
XKEY="$TEST/xkey"
[[ -x "$XKEY" ]] || gcc -O2 "$REPO/tools/gfxtest/xkey.c" -o "$XKEY" -lX11 -lXtst || exit 1

RIGHTS=""
for ((i = 0; i < TRACK; ++i)); do RIGHTS+="Right sleep:900 "; done

for try in 1 2 3; do
	pkill -x nfs2se; sleep 1
	# Fresh settings directory: HOME is redirected so the user's ~/.nfs2se is never touched
	rm -rf "$TEST/home"; mkdir -p "$TEST/home/.nfs2se"
	{
		echo "StartInFullScreen=0"
		echo "VSync=0"
		echo "WindowSize=$SIZE"
		for kv in ${CONF:-}; do echo "$kv"; done
	} > "$TEST/home/.nfs2se/nfs2se.conf"
	[[ -n "${MODELS:-}" ]] && cp -r "$MODELS" "$TEST/home/.nfs2se/models"
	[[ -n "${TEXTURES:-}" ]] && cp -r "$TEXTURES" "$TEST/home/.nfs2se/textures"
	[[ -n "${SKIES:-}" ]] && cp -r "$SKIES" "$TEST/home/.nfs2se/skies"

	(cd "$GAME" && env HOME="$TEST/home" SDL_AUDIODRIVER=dummy "$@" ${BIN:-./nfs2se} > "$OUT/game.log" 2>&1 &)
	sleep 8
	# Title -> main menu -> location -> track -> race
	"$XKEY" Escape sleep:3000 Return sleep:4500 Down sleep:900 Return sleep:5000 $RIGHTS \
		Down Down Down Down Down sleep:800 Return sleep:5500 Down Down Down sleep:800 Return sleep:20000
	[[ "${CAMERA:-}" == "chase" ]] && "$XKEY" c sleep:2000
	import -window root "$OUT/race.png"
	# In a race the red OMEGA timer panel is at the top of the 4:3 HUD
	python3 -I - "$OUT/race.png" <<'EOF' && break
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); w, h = im.size
r, g, b = im.getpixel((int((w - h * 4 / 3) / 2 + 0.33 * h * 4 / 3), int(h * 0.07)))
sys.exit(0 if r > 150 and g < 90 else 1)
EOF
	echo "not in a race (attempt $try), retrying" >&2
done
pgrep -x nfs2se >/dev/null || echo "game is not running (crashed?), see $OUT/game.log" >&2
[[ -n "${KEEP:-}" ]] || pkill -x nfs2se
echo "$OUT/race.png"
