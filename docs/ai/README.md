# NFSIISE remaster: handoff notes for AI agents

This fork of [NFSIISE](https://github.com/zaps166/NFSIISE) makes Need for Speed II SE look realistic by enhancing the
wrapper's OpenGL 2 renderer and patching the game's (vendored) assembly. It is **not** a remake: the original game code
runs unchanged except for small, documented hooks. These notes let an agent continue the work without re-deriving
everything.

## Documents

Current state and next steps: [HANDOVER.md](HANDOVER.md).

Progress is tracked in [ROADMAP.md](../../ROADMAP.md) at the repository root; update it with each milestone.

| File | Contents |
|---|---|
| [skies.md](skies.md) | High resolution sky panoramas: hook, drawing, sun alignment, sky files, conversion and install scripts |
| [terrain.md](terrain.md) | Distant terrain from real elevation data: generator, hiding the game's stand-ins, fog/dissolve |
| [car-models.md](car-models.md) | Replacing cars with detailed glTF models: pipeline, `.cmdl` format, hooks, fitting, wheels, lighting, adding a new car |
| [hd-textures.md](hd-textures.md) | HD texture packs: dump, hashing, Real-ESRGAN upscale, replacement loading |
| [game-formats.md](game-formats.md) | Game data formats reverse engineered so far: VIV/BIGF archives, GEO car models, engine limits |
| [reverse-engineering.md](reverse-engineering.md) | Game code facts: projection, structures, hook sites, addresses, rain/mirror/sky findings, how to find more |
| [renderer.md](renderer.md) | OpenGL 2 renderer architecture: frame flow, MRT mask, depth, post-processing passes, sky probe, settings |
| [testing.md](testing.md) | Headless testing: Xvfb race automation, gdb tricks, address-to-source mapping, GPU benchmarks, gfxtest |

## Repository layout (fork-specific parts)

```
src/Asm/NFS2SE.asm            game code (vendored, ~240k lines of x86 asm, yasm syntax) with widescreen + car model hooks
src/Glide2x/OpenGL2.c         Glide -> OpenGL 2 renderer (includes Textures.c, PostFX.c, CarModels.c)
src/Glide2x/Textures.c        texture conversion, hashing, dump/replace, mipmaps
src/Glide2x/PostFX.c          post-processing (SSAO, SSR, bloom, sun shafts, haze, tone mapping, FXAA, ...)
src/Glide2x/CarModels.c       replacement car models (loader, fitting, wheel animation, PBR shader)
src/Glide2x/Sky.c             sky panoramas (track sky files, sun alignment, drawing after the game's backdrop)
src/Wrapper.c / Wrapper.h     config parsing (GfxSettings gfx), window, hotkeys
tools/car-models/             Blender converter glTF -> .cmdl, CarConcept installer
tools/skies/                  Blender converter HDR panorama -> game sky, Poly Haven sky set installer
tools/hd-textures/upscale.sh  Real-ESRGAN texture pack builder
tools/formats/nfs2geo.py      VIV/GEO reader/writer/exporter
tools/gametest/               headless race automation, EGL GPU-selection shim
tools/gfxtest/                synthetic Glide scene renderer (no game data needed), XTest key sender
Need For Speed II SE/         game directory (game data is NOT in git; the binary nfs2se is built here)
screenshots/                  development screenshots; screenshots/readme/ is used by README.md
```

## Build and run

```sh
./compile_nfs                       # builds "Need For Speed II SE/nfs2se" (x86-32, needs yasm, gcc-multilib, SDL2 32-bit)
cd "Need For Speed II SE" && ./nfs2se
```

The binary links **no libm**: use SDL functions (`SDL_atan2`, `SDL_sqrt`, ...) in C code or the link fails. Settings live in
`~/.nfs2se/nfs2se.conf` (template: `Need For Speed II SE/nfs2se.conf.template`); new settings need an entry in
`GfxSettings` (Wrapper.h), its positional initializer in Wrapper.c **and** in `tools/gfxtest/gfxtest.c`, a parse line
in Wrapper.c, and documentation in the template and README.

## Working rules agreed with the owner

- Commit and push at milestones to the fork remote: `git push fork master` (https://github.com/kr4ckhe4d/NFSIISE).
- **Never add `Co-Authored-By: Claude` or any AI attribution lines to commits.**
- Game data originally came from the owner's NAS (`//192.168.4.162/barracuda`, guest SMB). Don't commit game data or
  third-party models; ship converters and download scripts instead.
- The owner's machine: AMD RX 9070 XT (+ R9700, + iGPU), 5120x2160 monitor, CachyOS. No DLSS; Vulkan exists but the
  renderer is OpenGL (Zink works via `MESA_LOADER_DRIVER_OVERRIDE=zink`).

## Status (2026-11)

Done: post-processing suite, Hor+ widescreen, HD texture pipeline, SSR road reflections with automatic rain wetness,
sun shafts, haze, mirror-correct SSR/SSAO, replacement car models with spinning/steering wheels.

Open ideas, roughly by value:
- Car models: verify the rear-view mirror; draw before translucent polygons (rain/smoke are hidden behind the model);
  models for the other cars (only `mcf1` has one); car reflections (SSR currently only on ground); motion blur on wheels.
- Trees/scenery: the same hook approach could replace track objects; tracks are in `gamedata/tracks/se/tr*.trk`
  (format not reverse engineered yet).
- Known pre-existing bug (upstream too): in-race pause menu "quit to system" doesn't exit.
