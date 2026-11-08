# NFSIISE remaster fork

Read `docs/ai/README.md` first: it indexes the handoff notes (car model replacement, HD textures, game data formats,
reverse-engineering facts and hook sites, renderer architecture, headless testing).

Rules:
- Commit and push at milestones to the fork: `git push fork master`.
- Never add "Co-Authored-By: Claude" or any AI attribution lines to commit messages.
- Don't commit game data or third-party models; ship converters/download scripts.
- Build with `./compile_nfs`. The binary links no libm (use `SDL_atan2` etc.). Test headless with
  `tools/gfxtest/run.sh` (synthetic) and `tools/gametest/race.sh` (real game, needs the game data).
- Keep `docs/ai/` up to date when you learn something non-obvious about the game or change a pipeline.
