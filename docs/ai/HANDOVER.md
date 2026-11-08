# Handover (2026-11-08)

State at the end of the first long session. Read `CLAUDE.md`, then `ROADMAP.md`, then the topic docs in this folder.

## Where things stand
- Everything is committed and pushed to `fork/master` (https://github.com/kr4ckhe4d/NFSIISE). Last feature commit:
  `4b9cffb` (distant terrain, Outback prototype).
- The game is built (`Need For Speed II SE/nfs2se`) and all assets are installed for the owner:
  - HD textures: `Need For Speed II SE/textures/` (4923, Real-ESRGAN x4plus)
  - Car model: `~/.nfs2se/models/mcf1/` (Khronos CarConcept, CC BY 4.0, cmdl v2 with wheel groups)
  - Skies: `~/.nfs2se/skies/trNN.jpg/.txt` for tr00, 02, 03, 04, 05, 06, 07 (Poly Haven CC0)
  - Terrain: `~/.nfs2se/terrain/tr02/` (Outback, AWS Terrain Tiles)
  - `~/.nfs2se/nfs2se.conf` has no new keys (all new features default on).
  To rebuild assets: `tools/car-models/install-carconcept.sh`, `tools/skies/install-skies.sh`,
  `tools/terrain/install-terrain.sh` (all need Blender; it is installed, run headless).
- Scratch files of the old session (`/tmp/claude-1000/...`) are gone; nothing in them is needed.

## Waiting on the owner
- **Feedback on Outback** after the terrain/fog changes (sky colour, distant mountains, the dissolve at ~400-490 m,
  overall fog). The owner plays on the real machine (RX 9070 XT, 5120x2160) and reports visual issues; the agent
  reproduces them headless (`tools/gametest/race.sh`) and fixes them. Their last report (Outback "floating"
  mountains/tiles) led to the sky fog, gradient-based sun alignment and the terrain work.
- Whether Outback's real geography (mostly flat; Uluru east ~11 km, Kata Tjuta west ~20 km) is dramatic enough, or
  they want another place / rotation / height exaggeration.
- Answer still pending from long ago: in the pause menu, does Enter on CONTINUE resume the race? (Context for the
  pre-existing upstream bug: "quit to system" doesn't exit.)
- Models for the other cars: the owner has to supply them (or name sources); only `mcf1` is replaced.

## Suggested next steps (see ROADMAP.md)
1. Act on the owner's Outback feedback.
2. Terrain for the other tracks: pick real places, find each track's stand-ins (method in `terrain.md`).
3. Night & weather: headlights lighting the road (Proving Grounds night is the weakest scene), rain spray.
4. Shadows (screen-space sun shadows, car model shadows), speed effects (motion blur).
5. Per-track sky brightness tuning (city tr03 very bright towards its low sun).

## Things that bit us (don't repeat)
- Blender: `save_render` colour-manages the image (double gamma). Use a byte image with `Non-Color` and
  `image.save()` for encoded data.
- The game binary has no libm: `atan`, `fmod`, `atan2` fail to link; use `SDL_atan2`, `SDL_pow`, ...
- Drawing at `grClipWindow` is too early (the game draws its backdrop later); sky goes at the first backdrop
  triangle, opaque car/terrain at the first 3D triangle, glass at `carModelsFlush`.
- The game's sky backdrop is queued in its sorted list (drawing in the horizon hook gets covered).
- Untextured triangles keep a stale `g_boundHash`; check `g_textureEnabled` before using it.
- Menu automation in `race.sh` is timing based and flaky; it retries, but check the screenshot.
- The idle demo of the game is a video (no hooks run); drive with `xkey <ms> Up` instead.
- The owner wants concise answers; they don't need Blender MCP (Blender runs headless from scripts).
