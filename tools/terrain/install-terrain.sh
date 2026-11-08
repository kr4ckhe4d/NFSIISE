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

# track  latitude   longitude  hide (textures of the game's flat stand-ins besides its terrain ring; - for none)
#        make_terrain.py options (palette, rotation, snow line, ...)
# Places: tr00 Proving Grounds - Eifel hills at the Nuerburgring; tr02 Outback - between Uluru and Kata Tjuta;
# tr03 city - Vancouver (sea, North Shore mountains); tr04 beach - Honolulu (Diamond Head, Ko'olau range);
# tr05 mountains/tunnel - Banff (Canadian Rockies); tr06 temple on cliffs - Cape Sounion; tr07 market with prayer
# flags - Namche Bazaar (Everest region). Coastal tracks are turned (--rotation) so that the real sea lies on the
# side of the game's own water (found by projecting the water triangles of an Alt+D capture back into the world).
TRACKS="${TRACKS:-
tr00  50.3350    6.9470    -                 --palette temperate
tr02  -25.3444  130.9276  309A8C0623B56146  --palette desert
tr03  49.2827  -123.1207  -                 --palette temperate --snowline 1600
tr04  21.2793  -157.8292  -                 --palette temperate --rotation -90
tr05  51.1784  -115.5708  -                 --palette alpine --snowline 2500
tr06  37.6600    24.0300   -                 --palette mediterranean
tr07  27.8050    86.7140   -                 --palette alpine --snowline 5200
}"
echo "$TRACKS" | while read -r track lat lon hide opts; do
	[[ -z "$track" ]] && continue
	out="$SETTINGS/terrain/$track"
	mkdir -p "$out"
	echo "== $track: $lat $lon"
	blender -b --python "$HERE/make_terrain.py" -- "$GAME/gamedata/tracks/se/$track.col" "$lat" "$lon" "$out" $opts | grep -E "^road|^base|^terrain|Error"
	{
		[[ "$hide" != "-" ]] && echo "hide=$hide"
		echo "fog_cap=350"
		echo "fog_max=0.6"
		echo "visibility=40000"
		echo "game_fog_scale=0.5"
		echo "dissolve=380 490"
	} > "$out/terrain.txt"
done
echo "Installed: $SETTINGS/terrain"
