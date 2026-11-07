#!/usr/bin/env bash
# Downloads the Khronos "CarConcept" glTF sample model (CC BY 4.0, Eric Chadwick / Darmstadt Graphics Group)
# and installs it as a replacement car model. Needs Blender (used headless for the conversion).
#
#   tools/car-models/install-carconcept.sh [car]     car = game car to replace (default: mcf1 - McLaren F1)
#
# Car names: the .geo file names in gamedata/sim/cardata/cardata.viv, e.g. mcf1 (McLaren F1), ff50 (Ferrari F50), gt90 (Ford GT90)
set -e
CAR="${1:-mcf1}"
HERE="$(cd "$(dirname "$0")" && pwd)"
case "$(uname)" in
	Darwin) SETTINGS="$HOME/Library/Application Support/nfs2se" ;;
	*) SETTINGS="$HOME/.nfs2se" ;;
esac
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fL -o "$TMP/CarConcept.glb" https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/CarConcept/glTF-Binary/CarConcept.glb
mkdir -p "$SETTINGS/models/$CAR"
blender -b --python "$HERE/export_carmodel.py" -- "$TMP/CarConcept.glb" "$SETTINGS/models/$CAR" | grep -E "^batch|Error" | cut -c1-60
echo "Installed: $SETTINGS/models/$CAR"
