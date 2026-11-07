#!/usr/bin/env bash
# Builds an HD texture pack from textures dumped by the game (TextureDump=1) with Real-ESRGAN.
#
#   tools/hd-textures/upscale.sh <realesrgan-ncnn-vulkan dir> [model]
#
# Input:  ~/.nfs2se/texdump/*.png   (play every track/car you want covered with TextureDump=1 first)
# Output: ~/.nfs2se/textures/*.png  (loaded automatically with TextureReplace=1)
#
# realesrgan-ncnn-vulkan: https://github.com/xinntao/Real-ESRGAN/releases (portable Linux/Windows/macOS builds)
# Default model "realesrgan-x4plus" suits photographic textures; try "realesrgan-x4plus-anime" for flat artwork.
# Already upscaled textures are skipped, so the script can be re-run after dumping more textures.

set -euo pipefail

ESRGAN_DIR="${1:?usage: $0 <realesrgan-ncnn-vulkan dir> [model]}"
MODEL="${2:-realesrgan-x4plus}"
SETTINGS="${NFS2SE_SETTINGS:-$HOME/.nfs2se}"
IN="$SETTINGS/texdump"
OUT="$SETTINGS/textures"
MIN_SIZE="${MIN_SIZE:-32}"   # Smaller textures (mostly HUD bits and palette variants) are left alone

[[ -x "$ESRGAN_DIR/realesrgan-ncnn-vulkan" ]] || { echo "realesrgan-ncnn-vulkan not found in $ESRGAN_DIR" >&2; exit 1; }
[[ -d "$IN" ]] || { echo "No texture dump in $IN - run the game with TextureDump=1 first" >&2; exit 1; }
mkdir -p "$OUT"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/in" "$WORK/out"

# Collect textures which are not upscaled yet and are big enough
python3 -I - "$IN" "$OUT" "$WORK/in" "$MIN_SIZE" <<'EOF'
import os, sys, shutil, struct
src, out, work, min_size = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
count = 0
for name in os.listdir(src):
    if not name.endswith('.png') or os.path.exists(os.path.join(out, name)):
        continue
    with open(os.path.join(src, name), 'rb') as f:
        header = f.read(24)
    w, h = struct.unpack('>II', header[16:24])
    if min(w, h) < min_size:
        continue
    shutil.copy(os.path.join(src, name), os.path.join(work, name))
    count += 1
print(f'{count} textures to upscale')
EOF

if [[ -n "$(ls -A "$WORK/in")" ]]; then
	"$ESRGAN_DIR/realesrgan-ncnn-vulkan" -i "$WORK/in" -o "$WORK/out" -m "$ESRGAN_DIR/models" -n "$MODEL" -f png
	mv "$WORK/out/"*.png "$OUT/"
fi
echo "HD textures: $(ls "$OUT" | wc -l) in $OUT"
