# Game data formats

Game data lives in `Need For Speed II SE/` (lower-cased file names, not in git). Tool: `tools/formats/nfs2geo.py`
(reads/writes VIV and GEO, exports OBJ; round-trips the original archive byte for byte).

## VIV / BIGF archive

Used for `gamedata/sim/cardata/cardata.viv` (car physics `.dat`, car models `.geo`) and
`gamedata/dashhud/pc/huddata.viv` (HUD, horizons `TRxx.HRZ`, sky).

```
char magic[4] = "BIGF"
u32  total size      (big endian)
u32  entry count     (big endian)
u32  header size     (big endian) - data of the first entry starts here
entries: u32 offset (BE), u32 size (BE), char name[] NUL terminated
data
```
Names keep their original case (`mcf1.geo`, `FF50.GEO`, ...); the game matches case-insensitively. When rewriting,
keep the header size (entry table must fit) - `viv_write` does this.

## GEO car model (`<car>.geo` inside cardata.viv)

```
header (140 bytes): u32 version (2), u32 part pointers[32] (runtime, filled by the loader), 8 bytes 0
32 parts, each:
  u32 vertex count, u32 polygon count
  i32 position[3]          16.16 fixed, metres, part origin in car space
  u32 runtime pointers     +0x14 vertices, +0x18 polygons, +0x1C normals (filled by the loader)
  ... flags (total part header 52 bytes)
  i16 vertices[count][3]   x right, y up, z front; 256 units per metre; count padded to an even number
  polygons[count], 12 bytes: u32 type/flags, u8 index[4], char texture[4] (e.g. "b003")
```
- Polygon type: bit 1 (2) quad, bit 0 (1) triangle (4th index repeats), bit 2 (4) right/mirrored side (normals are
  negated at load). Higher bits are flags (e.g. 0x2000).
- **No texture coordinates**: every polygon shows its whole texture tile (`a000`... from the car's `.qfs`).
- Parts (McLaren F1): 0-8 high detail body pieces (1 = main body, 116 verts), 9 shadow quad, 11 extra,
  12/14/16/18 wheel faces (`m000`), 13/15/17/19 tyre treads (`n000`), 20-25 low detail body (21 = main), 26-31 empty.
  Car length about 4.3 m (`z` -544..561).
- In memory the loader fixes up the pointers in place (`geo + 4 + 4*i` -> part header).

### Engine limits (measured by injecting subdivided models)
- More than roughly **600-680 polygons per car** corrupts memory (crash in texture upload `sub_49755C` with a smashed
  stack); vertex count (tested 1971) and file size (18.9 KB) are not the limit; the 2 MB Glide texture memory is not
  the cause either. The overflowing buffer was not identified.
- Load-time normal generation (`sub_46ED80`) is skipped for parts with > 125 vertices or > 65 polygons.
- u8 indices: max 256 vertices per part.
Conclusion: realistic cars can't be done inside GEO; see [car-models.md](car-models.md).

## Track collision file (`trNN.col`)
"COLL", u32 version (11), u32 size, u32 block count (4), u32 block offsets. Block 4 (after a 24-byte header) is the
road centreline ("virtual road"): 36-byte records starting with int32 16.16 x, y, z in world metres (same frame as
the game's objects and camera). Outback: 1867 points, closed loop, ~2.0 x 2.0 km, heights -96..50 m. Used by
tools/terrain/make_terrain.py.

## Others (not reverse engineered)
- `gamedata/carmodel/pc/*.qfs`: car textures (QFS = RefPack-compressed FSH). `*a.qfs` variants exist per car.
- `gamedata/tracks/se/trNN.trk` (+ `.col`, `.can`, `.qfs`): tracks; needed for scenery/tree replacement.
- `gamedata/render/pc/*.fsh` / `*.qfs`: effects, loading screens.
- `rain.bnk` is loaded on every track; horizons have no weather flag. Rain is detected in the renderer instead
  (see [reverse-engineering.md](reverse-engineering.md)).
