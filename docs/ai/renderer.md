# OpenGL 2 renderer (fork enhancements)

`src/Glide2x/OpenGL2.c` implements Glide 2 on OpenGL 2.1 (`GL2_ENHANCED` = desktop GL). It includes
`Textures.c`, `PostFX.c` and `CarModels.c` (single translation unit; they share statics).

## Frame flow
1. The game calls Glide functions; triangles are batched (`drawTriangles()` flushes on state changes).
2. Everything renders into an off-screen scene framebuffer (optionally supersampled `RenderScale`, MSAA) with two
   color attachments (MRT): color, and a **world mask** (1 = 3D world, 0 = HUD/overlay). The mask comes from
   `worldMask()`: flat `oow == 1` geometry and 2D are overlay; the cleared background counts as world (sky).
3. `grBufferSwap` -> `postPresent()` -> `postEffects()` for frames with 3D (`g_frameHas3D`), then the final pass into
   the window (Hor+ virtual width, letterboxing).

Depth: Glide `oow` is written as `d = 1 - oow` (ortho with near -1, far 0). View-space depth `w = 1 / (1 - d)` metres;
sky pixels have `d >= 0.999999`. The half-resolution linear depth texture (`aoLin`) stores `w`, negative for overlay
pixels and `1e7` for sky.

View-space reconstruction: `viewPos(uv, w) = ((uv*2-1) * uProj * w, w)` with `uProj = (V/2/focal, 240/focal)`
(`gfx.focalLength` default 320, V = virtual width), Y up. Sub-views (mirror) use their own rectangle and projection
(`POST_VIEW` GLSL block: `setView(uv)`, `uSub`, `uProjSub`; aspect from the clip window, same vertical FOV).

## Post-processing order (`postEffects`)
1. Linear depth (`aoLin`, half res) if SSAO or reflections are on.
2. **Sky probe** (4x1 RGBA16F): x0 average sky color (HDR), x1 sun (uv, coverage, spread), x2 sun color,
   x3 average color of the visible scenery. The sun is the centroid of sky pixels with luma > 2; large spread = evenly
   bright sky (no sun). Used by SSR, sun shafts, haze and the car model shader (previous frame's values).
3. **SSR** on up-facing, unsaturated (asphalt-like) pixels: screen-space march with 1/z interpolation, quadratic step
   distribution, start 3 texels out, ordered 4x4 jitter, binary refinement, edge fade; misses fall back to the sky at
   the projected reflected direction (upward search) or the average sky. Wetness: `Wetness=auto` -> `0.15 + 0.7 *
   rainLevel` (rain detector in OpenGL2.c, smoothed 0.015/frame). Dry roads: rough Fresnel, F0 = 0, mostly sky sheen,
   soft radiance limit. Then a depth-aware blur (horizontal 2.5-2*wet texels, vertical 2.5-1*wet).
4. **Sun shafts**: radial blur of bright sky pixels towards the probed sun (48 samples, half res), only for a compact sun.
5. **SSAO** (normal-oriented hemisphere, 12 spiral samples, depth-aware blur).
6. **Bloom** (prefilter + down/up chain, energy normalized).
7. **Composite**: haze `mix(hdr, sky, haze*(1-exp(-w*0.003)))`, reflections `mix(hdr, refl.rgb, refl.a*mask)`,
   `+ (bloom + shafts) * mask`, tone mapping/grading, vignette, grain. Then FXAA, sharpening.

HDR convention: the scene is LDR gamma-encoded; passes use `expandHDR(toLinear(c), white)` (invertible highlight
expansion, white = 4) and compress back. Debug views: `Alt+M` cycles 0 off, 1 world mask, 2 ambient occlusion,
3 depth, 4 bloom, 5 reflections, 6 sun shafts. `Alt+P` toggles effects, `Alt+S` screenshot.

## Settings (GfxSettings in Wrapper.h, parsed in Wrapper.c)
`RenderScale, MSAA, AnisotropicFiltering, Mipmaps, PerPixelFog, TextureReplace, TextureDump, PostProcessing,
AmbientOcclusion, AmbientOcclusionRadius, Bloom, BloomThreshold, ToneMapping, Exposure, Contrast, Saturation, Warmth,
FXAA, Sharpen, Vignette, FilmGrain, Reflections, Wetness (auto|0..1), SunShafts, Haze, FocalLength, Widescreen,
CarModels`. Adding one: struct field, positional initializers in Wrapper.c **and** tools/gfxtest/gfxtest.c, parse
line, template + README docs.

## Performance (RX 9070 XT, 5120x2160, synthetic gfxtest scene)
Base post-processing 1.23 ms/frame; with reflections, shafts and haze 2.05 ms. Car models not benchmarked
(CarConcept = 213k triangles per car).
