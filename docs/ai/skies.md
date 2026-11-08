# High resolution skies

Status: working (see ROADMAP.md section 1). Code: `src/Glide2x/Sky.c`; tools: `tools/skies/`.

## Pipeline

```
Poly Haven HDR (equirectangular, CC0) --Blender: tools/skies/make_sky.py--> <settings>/skies/trNN.jpg + trNN.txt
tools/skies/install-skies.sh downloads and converts a matching set for all tracks (8K by default)

game opens gamedata\tracks\...\trNN.*  -> gfxCurrentTrack = NN (Wrapper.c convertFilePath)
game queues the horizon (sub_42DAF0)   -> skyHook(camera): load sky of the track if needed, capture camera + projection
first 3D triangle of the view          -> skyDrawPending(): full-view pass, panorama blended above the horizon
```

## Sky file (`trNN.txt`, key=value)
| key | meaning |
|---|---|
| `image` | JPEG/PNG next to the txt (equirectangular, top row = zenith, bottom row = `elevation_min`) |
| `elevation_min` | elevation of the bottom row in degrees (make_sky crops at -8) |
| `yaw` | rotation in degrees (panorama azimuth = world azimuth + yaw); used when `auto_yaw=0` or no sun is seen |
| `sun_azimuth`, `sun_elevation` | the panorama's sun (brightest pixel), written by make_sky |
| `horizon_blend` | `from to` elevations (degrees) where the panorama fades in over the game's backdrop |
| `auto_yaw` | 1: measure the game's sun and rotate the panorama to match (needs a visible sun/glow); 0 for overcast |

Image encoding: linear HDR * brightness scale -> `c / (1 + 0.75 c)` -> gamma 1/2.2 -> 8 bit. This is the inverse of
the post-processing's `expandHDR` (white = 4), so up to 4x white survives and bright sky areas drive bloom/shafts.
`make_sky.py --brightness` sets the average sky luminance (sun excluded) before encoding. Blender's image saving must
not apply colour management (byte image, Non-Color, `image.save()`), otherwise values are gamma encoded twice.

Chosen set (install-skies.sh): tr00 kloppenheim_07 (night, 0.035, no auto yaw), tr02 kloppenheim_06 (sunset 0.20),
tr04 kloofendal_48d_partly_cloudy (0.30), tr05 kloofendal_38d_partly_cloudy (0.30, track's sky not checked),
tr03 kloppenheim_01 (0.08), tr06 kloofendal_misty_morning (0.25, no auto yaw), tr07 kloofendal_overcast (0.22, no
auto yaw). Use the `*_puresky` variants: sky only with a clean horizon, which blends over the game's terrain.

## Drawing
The game's backdrop has two layers: an opaque sky gradient and terrain textures (mesas, hills, skyline) whose sky
part is transparent. With a panorama, `grDrawTriangle` draws it (opaque) at the first backdrop triangle of a view
(flat `oow >= 0.999` before any 3D, race frames only), then skips backdrop triangles that are untextured or use an
opaque texture (`g_boundAlphaMode == AlphaOpaque`) and keeps the transparent terrain ones over it. While the sun is
being aligned nothing is skipped (the game's sun must stay visible). `horizon_blend` is only used for the fallback
(panorama drawn at the first 3D triangle when a view has no backdrop triangles).

- Camera space direction of a pixel from the game's projection (`dword_4D5294`.. captured in the hook) and the clip
  window; world direction with the camera matrix (see reverse-engineering.md); azimuth `atan(x, z)`, elevation
  `asin(y)`; equirect lookup (no mipmaps: the panorama is magnified on screen, and mipmaps would seam at the wrap).
- Alpha = smoothstep over `horizon_blend`; written to both MRT targets (mask = world) with blending, no depth.
- The game's backdrop is queued in its sorted render list, so drawing in the hook gets covered; the draw happens
  in `worldMask()` when the first non-flat (3D) triangle of the view arrives.

## Sun alignment (`skyAlign`)
For up to 10 s after the first frame on a track the panorama is not drawn; each frame the post-processing sky probe
(texel 1: sun uv, coverage, spread of very bright sky pixels) of the game's own sky is converted to a world azimuth.
After 20 measurements (coverage > 0.002, spread < 0.25, outliers > 90 deg rejected) `yaw = sun_azimuth - measured`.
Without a measurement the configured yaw is used. Outback measured ~87 deg, beach ~151, city ~34.

## Image-based lighting (car models)
At load, `skyBuildIbl` decodes the sky image to linear HDR and makes three smaller copies with the same encoding:
512 wide (sharp), 128 wide blurred (sigma 2 px, ~6 deg), 32 wide blurred (sigma 2.5 px, ~30 deg, diffuse).
`skyIblBind(unit, ...)` binds them (car shader: units 2-4) with the current view's camera->world matrix, the yaw,
the bottom elevation and the panorama's sun direction in world space (`sun_azimuth - yaw`, `sun_elevation`); the
sun strength is 1 for `auto_yaw=1` skies (a real sun) and 0 for overcast ones. The car shader looks up reflections
by roughness (sharp -> blurred -> diffuse), diffuse light from the diffuse copy around the normal, the sun colour
from the sharp copy at the sun direction; below the horizon it uses 15% of the average sky as the ground. The
game-exposure ambient (see car-models.md) * 0.6 remains a minimum, so cars stay visible under night skies.

## Road reflections
`skyDrawPending` records the main (full width) view's camera in `g_skyMain`; `skyMainPanorama()` binds the sharp and
blurred copies for the SSR pass (units 3, 4). SSR rays that miss use the panorama in the reflected direction
(sharp when wet, blurred when dry, average sky below the horizon) instead of searching the screen for sky pixels.
Mirror pixels keep the screen search.

## Known issues / ideas
- A backdrop terrain texture that is fully opaque (no sky part) would be skipped like the gradient (not seen yet).
- City (tr03) is very bright towards the low sun; brightness per track should be tuned on the real display.
- Night tracks: the minimum ambient makes cars look flat; local lights (street lamps, headlights) would help.
- The mirror view uses the same sky file; not checked visually.
