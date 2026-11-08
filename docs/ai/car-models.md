# Replacement car models

Status: working (commits `fadaa97`, `92416e1`). Only `mcf1` (McLaren F1) has a model, the Khronos CarConcept sample.

## Pipeline overview

```
any glTF/GLB car ──Blender (headless)── tools/car-models/export_carmodel.py ──> <settings>/models/<car>/model.cmdl + *.png
                                                                                      │
game loads car GEO ──hook carModelLoaded(name, geo)───────────────────────────────────┤ (model found by name)
game draws car part ──hook carPartHook(part, matrix, translation, object)─────────────┤ skip original part, queue model
game finished scene polygons ──hook carModelsFlush()──> CarModels.c draws queued models (PBR shader, game projection)
```

`<settings>` is `~/.nfs2se` (Linux), `%AppData%\.nfs2se` (Windows), `~/Library/Application Support/nfs2se` (macOS).
`<car>` is the GEO file name without extension (lower case), e.g. `mcf1`, `ff50`, `f355`, `gt90`, `lgt1`, `jagr`,
`isde`, `nazc`, `cala`, `espr`, `idgo`. List them with
`python3 tools/formats/nfs2geo.py viv-list "Need For Speed II SE/gamedata/sim/cardata/cardata.viv"`.
Opponents with the same car share the GEO, so they get the model too.

## Converting a car

```sh
blender -b --python tools/car-models/export_carmodel.py -- car.glb ~/.nfs2se/models/mcf1
# or the test model:
tools/car-models/install-carconcept.sh [car]
```

Requirements for the source model:
- glTF/GLB (anything Blender can import works if the importer line is changed), metres, Y up, **front towards +Z**
  (glTF convention; the exporter converts Blender's Z-up back). The game is left handed: the renderer mirrors X.
- Wheels: objects (or their parents) named `WheelFrontL`, `WheelFrontR`, `WheelRearL`, `WheelRearR` (case and `_`/space
  insensitive prefix match). Children named `*pad*` or `*caliper*` steer but don't spin. Everything else is body.
  Rename wheels in Blender if a model uses other names, otherwise they won't move.
- Ground contact at the lowest point of the model (y = min); scale doesn't matter (fitted automatically).
- Materials: Principled BSDF values are exported per material (base color, metallic, roughness, transmission,
  clear coat, emission) plus the base color texture only (normal/roughness maps are not exported yet).
  The glTF importer sets white emission on non-emissive materials; the loader ignores emission exactly (1,1,1).
- Triangle budget: CarConcept is 213k triangles and fine; everything is drawn every frame (no LODs).

Check the result with `tools/gametest/race.sh` (see [testing.md](testing.md)), e.g.
`TRACK=1 CAMERA=chase MODELS=~/.nfs2se/models tools/gametest/race.sh`. The game log prints
`Car model "mcf1": N batches` and `Car "mcf1": replacement model, scale S` (S should be close to 1 for a real car of the
same size).

## `.cmdl` format (little endian)

```
char  magic[4] = "CMDL"
u32   version  (1 or 2)
u32   batch count
per batch:
  f32 color[4]                 linear base color (alpha < 0.99 -> transparent pass)
  f32 metallic, roughness, transmission, clearcoat
  f32 emission[3]              linear, already multiplied by strength
  char texture[64]             base color PNG file name next to model.cmdl, "" if none
  version >= 2:
    u32 wheel                  0 body, 1 front left, 2 front right, 3 rear left, 4 rear right
    u32 spins                  1 = rotates around the axle (tyre, rim, disc), 0 = steer only (caliper)
    f32 pivot[3]               wheel centre (bounding box centre of the spinning parts of that wheel)
  u32 vertex count             (non-indexed triangles)
  f32 vertices[count][8]       position xyz, normal xyz, uv (v already flipped: 1 - v)
```

Coordinates are glTF-like (right handed, Y up, +Z front). The loader negates X of positions, normals and pivots.

## Runtime (src/Glide2x/CarModels.c)

- **carModelLoaded(fileName, geo)** – called by the game after loading a GEO (asm `loc_46E407`). Records the GEO
  pointer and looks for `models/<name>/model.cmdl`. GL resources are created lazily at first draw (the load hook may
  not run with the GL context current).
- **carPartHook(part, m, t, object)** – called at the start of the per-part draw `sub_47FCD0`. Finds which registered
  GEO and part index this is (compares with the 32 part pointers at `geo + 4 + 4*i`). For a replaced car it sets
  `carPartSkip = 1` (the asm then returns without drawing) for every part except 10; part 9 (the game's flat shadow quad)
  is replaced by a soft contact shadow (`sprayCarShadows` in Spray.c).
  On part 1 or 21 (high/low detail body) it queues one model draw with:
  - `rot` = the game's model->camera matrix (16.16, row-vector convention: `cam = v * M`, transposed to column form),
  - `origin` = camera-space translation minus `pos_part * M` (so the car origin, not the part origin),
  - the projection constants at that moment, and the wheel state from `carUpdateMotion(object)`.
  Duplicate queueing of the same car (body high+low LOD) is filtered by origin distance.
- **Opaque parts** are drawn at the first 3D triangle of the view (`worldMask()`, right after the sky panorama), so
  every later polygon of the game - also translucent rain, spray and smoke - is depth tested against the car.
- **carModelsFlush()** – called at the end of `sub_42A3C0` (the game finished drawing its sorted polygon list for the
  view). Draws the transparent parts (glass) over the scene behind them, and opaque parts not drawn yet. Drawing earlier is wrong: e.g. on Proving Grounds the game sets the clip window and
  then draws the sky backdrop without depth test, which overwrote models drawn at the clip window change.

### Fitting (`carFitModel`)
Car space = GEO part space + part position. Body box = parts 0..8; ground = lowest vertex of wheel parts 12..19.
`scale = body length (Z) / model length`; X and Z centred on the body box; model y = 0 placed on the ground.

### Projection (exactly the game's)
Camera space in metres, X right, Y up, Z forward. Game screen in 640x480 units:
`x = sx * X/Z + cx`, `y = sy * Y/Z + cy` with `sx = dword_4D5294`, `sy = flt_4D5298`, `cx = flt_4D529C`,
`cy = flt_4D52A0` (typically 320 * widescreenScale, -330, 320, 240). Depth: the game writes `d = 1 - oow` with
`oow = 1/Z`; a clip-space z of `Z - 2` with w = Z gives exactly that. The projection maps to the current clip window
(`g_clipWindow`, game units) so sub-views work.

### Wheels (`carUpdateMotion`)
The game's own wheel parts are useless for this: they are culled most frames and spin by texture swaps. Instead each
game object (`esi` at the hook, `+0x74` position 16.16, `+0x50` matrix 16.16) is tracked per frame:
- spin += forward distance / (wheel radius * scale) (radius = wheel pivot height above the model's lowest point),
- steer = atan2(wheelbase * yawRate, speed) (bicycle model), clamped to `min(0.6, 5 / |speed|)` rad and smoothed
  over 0.15 s, so collisions and slides don't flick the wheels. Left turn = negative angle (verified numerically).
The vertex shader rotates wheel vertices: spin around X, then steer around Y, about the batch pivot.

### Scene reflections (dynamic environment)
`PostFX.c` keeps an equirectangular map of world directions around the camera (256x128 RGBA16F ping-pong, plus
64x32 and 16x8 blurred copies). Each frame (`postEffects`, after the linear depth) every direction that is visible
on screen is blended towards the scene colour there (rate 0.25), skipping overlays, the mirror and car-model pixels
(car models write green = 1 into the mask attachment; the mask is cleared to (1,0,0,1)). Directions behind the
camera keep what was seen when they were in front, so the scenery that was passed shows in the paint. Alpha is how
much was seen (colour is premultiplied by it). The map is cleared when the track changes. The camera of the main
view comes from the horizon hook for every frame (`skyMainCamera`), with or without a sky panorama.
The car shader blends it over the panorama/sky reflections by alpha (sharp/blurred by roughness) and mixes the
diffuse copy at the normal into the ambient light (50 %).

### Lighting
With a sky panorama for the track, reflections, diffuse light and the sun come from it (image-based lighting, see
skies.md). Otherwise (and as a minimum on dark skies) the following applies.
Physically based (Lambert + GGX sun, Schlick Fresnel, clear coat layer, glass with Fresnel alpha) but in the **game's
exposure**: the game shows textures at roughly full brightness whatever the sky, so a physical sky-lit car is
near-black on night tracks. Base illumination is fixed and only tinted by the sky hue from the sky probe; a bright sky
and a visible compact sun (probe texel 1/2) add to it. Clear-coated paint has metallic capped at 0.45 (the glTF marks
it fully metallic, which makes red paint look black under a dark sky). Output is gamma encoded with a soft highlight
compression, written to both MRT targets (mask = 1, world) so all post effects apply.

## Known limitations / next steps
- Rear-view mirror untested (the hook is called with the mirror's clip window active? verify; projection supports it).
- No reflections of the scene on the car (only a procedural sky/ground environment). SSR is ground-only.
- Normal/roughness/metallic textures are ignored; only base color textures are exported.
- `CarModels=0` disables everything (parts are then never skipped).
