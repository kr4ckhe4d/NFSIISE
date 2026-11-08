# Distant terrain from real elevation data

Status: prototype on Outback (tr02). Code: `src/Glide2x/Terrain.c` (+ terrain support in `CarModels.c`, game shader
in `OpenGL2.c`); tools: `tools/terrain/`.

## Why
The game shows distant landscape as flat stand-ins: a ring of cards at exactly 100 m around the camera (8 textured
tiles - for Outback a 360-degree strip of desert with Uluru - plus an untextured base strip) and a few large flat
"mountain" cards (e.g. Outback's slanted grey rock card, texture 309A8C0623B56146). Against the sky panoramas they
read as pasted images. They are replaced by a real terrain mesh.

## Pipeline
```
gamedata/tracks/se/trNN.col  -- road centreline (block 4, 36-byte records, int32 16.16 x y z; see game-formats.md)
AWS Terrain Tiles (terrarium/12/x/y.png, open data, no key)
    --Blender: tools/terrain/make_terrain.py <col> <lat> <lon> <out>-->  <settings>/terrain/trNN/model.cmdl + terrain.png
tools/terrain/install-terrain.sh writes terrain.txt (hide list, fog parameters) per track
```
make_terrain.py: grid 512x512 over +-22 km around the track centre (track bounding box centre = the real place at
lat/lon; world +Z = north, +X = east, `--rotation` turns it). Height = the lowest road height within ~1 km (spread outwards, smoothed
without rising) - 6 m near the track (another -20 m within 250 m), blending to the real relief (DEM - median elevation near the
centre) from 400 to 2500 m; cells within 60 m of the road are dropped. (A blurred average of the road heights was
wrong: in Outback's canyon the road dips to -96 m and the ground stood up to 65 m above it, a brown ceiling.) Colours: red sand / scrub / rock by slope and
noise (no imagery). Output in the car model format (world metres, X written mirrored because the loader mirrors).
Outback: centre -25.3444, 130.9276 (11 km west of Uluru, ~20 km east-southeast of Kata Tjuta) - the game's own ring
shows Uluru in the same direction (east, measured from its ring tiles: world azimuth of tiles 0..6 = -51..56 deg).

## Runtime (Terrain.c)
- `terrainQueue()` at the first 3D triangle of a view (after the sky): loads `terrain/trNN/` for the current track
  and queues a CarDraw with world -> camera transform (camera position and matrix from the horizon hook,
  `g_skyView.camPos/camToWorld`). Drawn with the car shader (image based lighting) with `uTerrain = 1`: mask green 0
  (not a car, so it is in the environment map and SSR), and its own fog: the game's fog table up to `fog_cap`
  metres, at most `fog_max`, then aerial perspective `1 - exp(-d / visibility)`, towards the panorama horizon
  colour (mixed with the game's fog colour by the sky's `game_fog`).
- The game's geometry (game shader, `uTerrainView`): fog scaled by `game_fog_scale` (0.5 - the game's fog was made
  to hide its 500 m draw distance and turned distant hills into flat silhouettes) and screen-door dissolve between
  `dissolve` distances (380-490 m) so the terrain shows where the game's scenery ends. Only for geometry within
  1.5x the dissolve end: the game's sky glow layers are farther and keep their fog.
- `terrainHidesTriangle()`: hides triangles with all vertices at 99-101 m (the camera-attached ring, textured or
  not) and textures listed in `hide`.

## Finding stand-ins of other tracks
Log, per texture hash, the nearest/farthest distance at which it is drawn (temporary code in `grDrawTriangle`
using `g_boundHash` and `1/oow`): the ring textures are always exactly 100 m away; large flat cards are drawn far
beyond the ~500 m draw distance (Outback's grey card up to 1.4 km). A screen-point picker (log the hash of triangles
covering a game coordinate) identifies a specific object. View candidates in `<settings>/texdump/` (TextureDump=1).

## Open issues
- Outback's surroundings are mostly flat in reality: Uluru (east, ~2 deg high) and Kata Tjuta (west) are the
  features. A more dramatic choice of place (or `--rotation`) is a design decision.
- Other tracks need a real place each and their stand-in textures.
- The dissolve is a dither pattern (visible at low resolutions); the mesh is not indexed (50 MB for Outback).
