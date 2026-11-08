# Distant terrain from real elevation data

Status: all seven tracks (places in `tools/terrain/install-terrain.sh`); Outback checked by the owner, the others
only headless. Code: `src/Glide2x/Terrain.c` (+ terrain support in `CarModels.c`, game shader
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
wrong: in Outback's canyon the road dips to -96 m and the ground stood up to 65 m above it, a brown ceiling.) Colours by `--palette` (desert, temperate,
alpine, mediterranean: ground, noise patches, speckle, rock on slopes), snow above `--snowline` (real metres above sea
level), DEM at or below sea level becomes flat dark sea with a light shore; no imagery. `--exaggerate` scales the
relief. Output in the car model format (world metres, X written mirrored because the loader mirrors).
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

## Places per track
| Track | Look | Place | Options |
|---|---|---|---|
| tr00 | Proving Grounds (test circuit, trees) | Eifel at the Nuerburgring | temperate |
| tr02 | Outback | between Uluru and Kata Tjuta | desert |
| tr03 | city by the water | Vancouver (English Bay west, North Shore mountains) | temperate, snow line 1600 |
| tr04 | tropical beach | Honolulu / Waikiki | temperate, rotation -90 (sea west, like the game's water) |
| tr05 | pine forest, snowy mountains, tunnel | Banff | alpine, snow line 2500 |
| tr06 | Greek temple, sea cliffs | Cape Sounion | mediterranean |
| tr07 | market with prayer flags | Namche Bazaar (Everest 20 km) | alpine, snow line 5200 |

Track look -> trNN: Alt+D capture header has `track=`. Menu order of locations (race.sh `TRACK`): 0 tr00, 1 tr02,
2 tr04, 3 tr05, 4 tr03, 5 tr06, 6 tr07. None of the six new tracks has a big far stand-in card at the start (all far
textures are within the ~500 m draw distance); their 100 m rings are hidden automatically.

Sea side: project the water triangles of an Alt+D capture into the world (capture header has the camera position and
matrix; camera X/Y/Z = ((x - cx) / sx * Z, (y - cy) / sy * Z, 1 / oow), world = pos + X*row0 + Y*row1 + Z*row2),
compare their centroid with the track centre and set `--rotation` so the real sea is on that side
(rotation r: world west shows geographic south for r = -90).

## Open issues
- Outback's surroundings are mostly flat in reality: Uluru (east, ~2 deg high) and Kata Tjuta (west) are the
  features. A more dramatic choice of place (or `--rotation`) is a design decision.
- Only Outback has been seen by the owner; the other six places/rotations/palettes are headless-checked only.
- The dissolve is a dither pattern (visible at low resolutions). The mesh is indexed (cmdl v3, 15 MB per track).
