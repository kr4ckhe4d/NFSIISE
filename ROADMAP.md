# Roadmap

Realistic graphics for Need for Speed II SE on top of the original game code. Status legend:
`[x]` done · `[~]` in progress · `[ ]` planned. Details and technical notes for each area are in [docs/ai/](docs/ai/README.md).

## Done
- [x] Post-processing: SSAO, bloom, tone mapping and grading, FXAA, sharpening, vignette, film grain (`b2870d4`)
- [x] HD texture pipeline: dump, hash, Real-ESRGAN upscale, replacement (`b2870d4`)
- [x] True Hor+ widescreen, HUD and menus kept 4:3 (`da88c02`)
- [x] Road reflections (SSR) with automatic wetness when it rains, sun shafts, distance haze (`7b7aa3a`)
- [x] Correct reflections and SSAO in the rear-view mirror (`6f7ad9a`)
- [x] Replacement car models: glTF -> Blender -> `.cmdl`, game hooks, PBR shading (`fadaa97`)
- [x] Car model wheels spin and steer (`92416e1`)
- [x] Handoff docs and reusable test/format tools (`d96ff68`)
- [x] High resolution sky panoramas per track with automatic sun alignment (`0feecff`)

## 1. Skies and image-based lighting `[~]`
- [x] Find where the game draws the sky backdrop and where the camera orientation is available every frame
- [x] Detect the current track
- [x] Draw a high-resolution panorama (Poly Haven CC0 HDRIs) instead of the backdrop, oriented by the camera
- [x] Keep the game's distant terrain (mountains) by blending the panorama in above the horizon
- [x] Per-track sky selection and alignment of the panorama's sun with the game's sun
- [x] Download/install script for a set of matching skies
- [x] Replace the game's whole sky gradient, keeping its horizon terrain (backdrop terrain textures have a transparent sky)
- [x] Fog distant geometry into the panorama (user report: Outback mountains looked like floating images)
- [x] Align the sun from the game's sky gradient: panorama from the first frame (user report: old sky at the race start)
- [ ] Tune per-track brightness on the real GPU/monitor (city tr03 is very bright towards the low sun)
- [x] Use the sky for car lighting and reflections (image-based lighting) (`6967400`)
- [x] Road SSR misses sample the panorama (sharp when wet, blurred when dry)
- [ ] Sky of track tr05 (tunnel start) and the bonus track tr08 not checked

## 2. Car polish `[ ]`
- [x] Reflections of the scene in car paint and glass (dynamic environment map accumulated from the screen)
- [x] Draw cars before translucent effects (opaque parts right after the sky, glass at the end)
- [ ] Verify cars in the rear-view mirror
- [ ] Normal, roughness and metallic textures from glTF (CarConcept has none; do it with the first model that needs them)
- [ ] Models for the other cars (needs sourcing; only the McLaren F1 has one)

## 3. Night and weather `[ ]`
- [ ] Headlights that light the road and scenery on night tracks, lens flares
- [ ] Spray behind cars on wet roads
- [ ] Raindrops on the camera in the rain

## 4. Shadows `[ ]`
- [ ] Screen-space sun shadows from the scene depth
- [ ] Shadows of the car models (replacing the game's flat shadow quad)

## 5. Speed effects `[ ]`
- [ ] Motion blur by car speed (also hides wagon-wheel aliasing of fast wheels)
- [ ] Subtle depth of field

## 6. Trees and landscape `[ ]`
- [ ] Find how track objects are drawn; hook them like the cars
- [ ] 3D trees (generated in Blender or sourced) replacing billboards, instanced with LODs
- [ ] Grass, plants and rocks scattered on ground with grass/dirt textures

## 7. Usability `[ ]`
- [ ] HD HUD pinned to the corners on ultrawide screens
- [ ] Pause menu "quit to system" doesn't exit (pre-existing upstream bug)

## Notes
- Keep this file updated with every milestone commit (mark items, add the commit hash).
- Third-party assets are never committed; ship converters and download scripts.
