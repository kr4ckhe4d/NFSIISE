#!/usr/bin/env bash
# Builds and installs real-world distant terrain for the tracks (needs Blender and the game data).
#
#   tools/terrain/install-terrain.sh
#
# Elevation: AWS Terrain Tiles (open data). Output: <settings>/terrain/trNN/ (model.cmdl, terrain.png, terrain.txt).
# Terrain=1 in nfs2se.conf (default) draws it; the game's own flat stand-ins (its 100 m terrain ring and the
# textures listed in terrain.txt "hide") are hidden.
set -eo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
GAME="$HERE/../../Need For Speed II SE"
case "$(uname)" in
	Darwin) SETTINGS="$HOME/Library/Application Support/nfs2se" ;;
	*) SETTINGS="${NFS2SE_SETTINGS:-$HOME/.nfs2se}" ;;
esac

# track  latitude   longitude  hide (textures of the game's flat stand-ins besides its terrain ring)
TRACKS="
tr02  -25.3444  130.9276  309A8C0623B56146
"
echo "$TRACKS" | while read -r track lat lon hide; do
	[[ -z "$track" ]] && continue
	out="$SETTINGS/terrain/$track"
	mkdir -p "$out"
	echo "== $track: $lat $lon"
	blender -b --python "$HERE/make_terrain.py" -- "$GAME/gamedata/tracks/se/$track.col" "$lat" "$lon" "$out" | grep -E "^road|^base|^terrain|Error"
	{
		echo "hide=$hide"
		echo "fog_cap=350"
		echo "fog_max=0.6"
		echo "visibility=40000"
		echo "game_fog_scale=0.5"
		echo "dissolve=380 490"
	} > "$out/terrain.txt"
done
echo "Installed: $SETTINGS/terrain"
