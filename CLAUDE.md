# NFSIISE remaster fork

Goal: make Need for Speed II SE look realistic by enhancing the wrapper's OpenGL 2 renderer and adding small, documented
hooks to the vendored game code (`src/Asm/NFS2SE.asm`). Not a remake: the original game logic stays.

Start here:
1. `docs/ai/HANDOVER.md` - current state, what the owner is testing, next steps, pitfalls.
2. `ROADMAP.md` - progress tracking (update it with every milestone, with the commit hash).
3. `docs/ai/README.md` - index of the topic docs (renderer, car models, skies, terrain, HD textures, game formats,
   reverse engineering, testing).

## Rules
- Commit and push at milestones to the fork: `git push fork master`.
- **Never add "Co-Authored-By: Claude" or any AI attribution lines to commit messages.**
- Don't commit game data or third-party assets; ship converters and download/install scripts.
- Keep `docs/ai/` and `ROADMAP.md` up to date when you learn something non-obvious or change a pipeline.

## Working style
- The owner plays on the real machine (AMD RX 9070 XT, 5120x2160) and reports what looks wrong; reproduce headless,
  fix, verify with screenshots, then report concisely. Prefer showing before/after images.
- Build: `./compile_nfs` (x86-32). The binary links no libm: use `SDL_atan2`, `SDL_pow`, ... in C.
- Test without a screen: `tools/gfxtest/run.sh` (synthetic scene, no game data) and `tools/gametest/race.sh`
  (real game on Xvfb; options `TRACK`, `CAMERA=chase`, `WAIT`, `KEEP`, `MODELS`, `SKIES`, `TERRAIN`, `CONF`).
  Drive with `$NFS2SE_TEST_DIR/xkey <ms> Up`. Details in `docs/ai/testing.md`.
- Blender is installed and used headless (`blender -b --python ...`); no Blender MCP needed.
- New settings: `GfxSettings` (Wrapper.h) + positional initializers in Wrapper.c **and** tools/gfxtest/gfxtest.c +
  parse line + `nfs2se.conf.template` + README.
- Temporary debug code: mark it (e.g. `/* TMPxxx */`) and remove it before committing.
