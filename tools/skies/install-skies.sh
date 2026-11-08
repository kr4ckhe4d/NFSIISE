#!/usr/bin/env bash
# Downloads CC0 sky panoramas from Poly Haven (https://polyhaven.com, public domain) and installs them as
# high resolution skies for every track. Needs Blender (headless conversion) and curl.
#
#   tools/skies/install-skies.sh [resolution]     resolution: 4k or 8k (default 8k; ~70 MB download per sky)
#
# Output: <settings>/skies/trNN.jpg + trNN.txt (Skies=1 in nfs2se.conf, default). Edit a trNN.txt to tune it
# (yaw, horizon_blend, auto_yaw) or rerun make_sky.py with other options/panoramas.
set -e
RES="${1:-8k}"
HERE="$(cd "$(dirname "$0")" && pwd)"
case "$(uname)" in
	Darwin) SETTINGS="$HOME/Library/Application Support/nfs2se" ;;
	*) SETTINGS="${NFS2SE_SETTINGS:-$HOME/.nfs2se}" ;;
esac
mkdir -p "$SETTINGS/skies"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# track  Poly Haven id                          brightness  auto_yaw  headlights  night  # location (menu order) - game's sky
SKIES="
tr00  kloppenheim_07_puresky                 0.035       0         1.0   0.30  # Proving Grounds - night, rain
tr02  kloppenheim_06_puresky                 0.20        1         0     1     # Outback - sunset
tr04  kloofendal_48d_partly_cloudy_puresky   0.30        1         0     1     # beach - clear day
tr05  kloofendal_38d_partly_cloudy_puresky   0.30        1         0     1     # tunnel start
tr03  kloppenheim_01_puresky                 0.08        1         0.4   1     # city - rain, low sun
tr06  kloofendal_misty_morning_puresky       0.25        0         0     1     # cliffs - misty
tr07  kloofendal_overcast_puresky            0.22        0         0     1     # market - overcast
"
echo "$SKIES" | while read -r track id brightness autoyaw headlights night rest; do
	[[ -z "$track" ]] && continue
	echo "== $track: $id"
	curl -fL -o "$TMP/sky.hdr" "https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/$RES/${id}_$RES.hdr"
	blender -b --python "$HERE/make_sky.py" -- "$TMP/sky.hdr" "$SETTINGS/skies/$track" \
		--brightness "$brightness" --auto-yaw "$autoyaw" --headlights "$headlights" --night-ambient "$night" | grep -E "^sky|Error"
	rm -f "$TMP/sky.hdr"
done
echo "Installed: $SETTINGS/skies"
