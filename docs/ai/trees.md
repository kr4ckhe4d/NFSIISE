# 3D trees

Status: working on the tracks' start areas (tables from Alt+D captures); headless-checked only. Code:
`src/Glide2x/Trees.c` (+ cutout support in `CarModels.c`); tools: `tools/trees/`.

## How the game draws trees
Static crossed billboards: two or more vertical cards sharing the trunk line (seen by projecting an Alt+D capture
back into the world: the halves of a palm share the centre vertex). Many trees use a left and a right texture half.
Far "forest wall" strips and bushes are other cutout textures and are not replaced.

## Runtime (Trees.c)
- `<settings>/trees/trNN.txt`: `<texture hash> <model> [height scale]`; models `<settings>/trees/<model>/model.cmdl`
  (base at the origin, height 1).
- `treesTriangle()` (from `grDrawTriangle`, main view only - the mirror keeps the billboards): projects the triangle
  back with the horizon hook's camera (`g_skyView`) and the game projection (Z = 1/oow, X = (x - cx) / sx * Z,
  Y = (y - cy) / sy * Z, world = pos + X row0 + Y row1 + Z row2), merges it into a per-track registry (same model
  within 4 m: position = running average of triangle centroids, base/top = union of heights over all frames), and
  hides it.
- `treesQueue()` (at `carModelsFlush`, before the glass pass): every tree seen this frame becomes a `CarDraw`
  (model -> camera = world -> camera rows x a turn by a stable random angle x the height), drawn with the car shader
  as scenery (`CarGeo.terrain` + `scenery`: world mask, the game's fog table without the terrain cap).
- `CAR_MAX_DRAWS` is 512 (cars + terrain + trees).

## Alpha cutout
`export_carmodel.py` marks a material whose Alpha is driven by its texture as cutout (`transmission = -1`); the car
shader discards alpha < 0.5 for those batches, and the loader builds coverage-preserving mipmaps
(`carUploadCutoutMips`: each level's alpha scaled until the share of texels above 0.5 matches the full size texture;
plain mipmaps made distant leaves vanish).

## Models (tools/trees/install-trees.sh)
| Model | Source | Triangles |
|---|---|---|
| jacaranda | Poly Haven `jacaranda_tree` (CC0, 3.8M triangles) via `make_tree.py`: 14% of the leaf clusters kept, each replaced by one card mapped to its UV box (least squares UV -> 3D), grown to keep coverage; twigs < 4% of the height deleted; trunk and branches decimated to 3% | ~45k |
| conifer | `make_conifer.py`: procedural whorls of drooping branches made of crossed needle-spray cards from Poly Haven `fir_tree_01`'s twig atlas, its trunk texture | ~2k |
| palm | `make_palm.py`: procedural ringed curved trunk, 14 arching fronds with drooping leaflets, no external assets | ~1.5k |
`fir_tree_01` itself could not be reduced well (800k twig clusters become sparse oversized cards).

## Tables (start areas only)
tr00 pines -> conifer; tr03 broad + young trees -> jacaranda; tr04 palms -> palm; tr05, tr06 broad trees, tr07
umbrella trees -> jacaranda. Other trees on a lap keep their billboards until their textures are added: find them
with Alt+D (green cutout textures of world triangles, `tree-replaced` marks replaced ones).

## Open
- Owner check (look, density, colours, performance); trees on the rest of each lap; Outback (dead trees, bushes).
- LOD for distant trees; wind.
