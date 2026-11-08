#!/usr/bin/env bash
# Builds the 3D trees and installs them with the per-track billboard tables (needs Blender and internet access).
#
#   tools/trees/install-trees.sh
#
# Models (<settings>/trees/<model>/model.cmdl):
#   jacaranda - Poly Haven "jacaranda_tree" (CC0), reduced by make_tree.py (leaf cards, twigs removed), ~40k triangles
#   conifer   - procedural (make_conifer.py) with Poly Haven "fir_tree_01" (CC0) bark and needle textures
#   palm      - procedural (make_palm.py), no external assets
# Tables (<settings>/trees/trNN.txt): the game's tree billboard textures (left/right card halves) -> model.
# Found from Alt+D frame captures at each track's start (see docs/ai/trees.md); trees elsewhere on a track may use
# other textures: add them to the table.
set -eo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EXPORT="$HERE/../car-models/export_carmodel.py"
case "$(uname)" in
	Darwin) SETTINGS="$HOME/Library/Application Support/nfs2se" ;;
	*) SETTINGS="${NFS2SE_SETTINGS:-$HOME/.nfs2se}" ;;
esac
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$SETTINGS/trees"

# Downloads a Poly Haven model (1k glTF with its textures) and the alpha maps into $TMP/<asset>
fetch() {
	python3 -I - "$1" "$TMP/$1" <<'EOF'
import json, os, sys, urllib.request
asset, out = sys.argv[1], sys.argv[2]
get = lambda u: urllib.request.urlopen(urllib.request.Request(u, headers={'User-Agent': 'nfs2se-tools'})).read()
d = json.loads(get('https://api.polyhaven.com/files/' + asset))
g = d['gltf']['1k']['gltf']
os.makedirs(os.path.join(out, 'textures'), exist_ok=True)
open(os.path.join(out, asset + '.gltf'), 'wb').write(get(g['url']))
for k, v in g['include'].items():
    os.makedirs(os.path.dirname(os.path.join(out, k)), exist_ok=True)
    open(os.path.join(out, k), 'wb').write(get(v['url']))
for k in d:
    if 'alpha' in k:
        open(os.path.join(out, 'textures', k + '.png'), 'wb').write(get(d[k]['1k']['png']['url']))
EOF
}
convert() {
	rm -rf "$SETTINGS/trees/$1"
	blender -b --python "$EXPORT" -- "$TMP/$1.glb" "$SETTINGS/trees/$1" | grep -E "^batch|Error"
}

echo "== jacaranda"
fetch jacaranda_tree
blender -b --python "$HERE/make_tree.py" -- "$TMP/jacaranda_tree/jacaranda_tree.gltf" "$TMP/jacaranda.glb" \
	--leaf-alpha "$TMP/jacaranda_tree/textures/leaves_alpha.png" --leaf-keep 0.14 | grep -E "^leaf|^tree|Error"
convert jacaranda

echo "== conifer"
fetch fir_tree_01
F="$TMP/fir_tree_01/textures"
blender -b --python "$HERE/make_conifer.py" -- "$TMP/conifer.glb" "$F/fir_tree_01_trunk_a_diff_1k.jpg" \
	"$F/fir_tree_01_twig_diff_1k.jpg" "$F/twig_alpha.png" | grep -E "^conifer|Error"
convert conifer

echo "== palm"
blender -b --python "$HERE/make_palm.py" -- "$TMP/palm.glb" | grep -E "^palm|Error"
convert palm

# Billboard textures per track: <hash> <model> [height scale]
table() { printf '%s\n' "${@:2}" > "$SETTINGS/trees/$1.txt"; }
table tr00 "# Proving Grounds: pines" "F82EAC764398FE75 conifer" "235D418E7A3BF496 conifer"
table tr03 "# city: broad tree, young tree" "D95FAB17BCBE0828 jacaranda" "1550EF0003E8DC53 jacaranda" \
	"CF8C7A8A2C4A6E4A jacaranda" "148A2FB008624C6E jacaranda"
table tr04 "# beach: palms (two card pairs)" "D9B073628F4E9030 palm" "86C6450EF41A5920 palm" "2A0169D785188A31 palm" "08907135A058924B palm" \
	"CF7ED6DD99E9E466 palm" "AD4854521FC8D7C1 palm"
table tr05 "# mountains: broad trees" "50919BA613662E19 jacaranda" "5A7D5F62B3C72587 jacaranda" "0DC39460A7D976E4 jacaranda"
table tr06 "# temple: broad trees" "8802E2FAA799FA16 jacaranda" "0BD973C42B0FDC4B jacaranda"
table tr07 "# market: umbrella trees" "9D610F3981545239 jacaranda" "41396C800B7948CB jacaranda" \
	"C42F48CCBAD42477 jacaranda" "A72421503909CF88 jacaranda"
echo "Installed: $SETTINGS/trees"
