# Testing without a screen

Everything below works headless over SSH; nothing touches the owner's desktop or `~/.nfs2se`.

## Synthetic renderer test (no game data)
```sh
tools/gfxtest/run.sh out.png [Key=Value ...] [DebugView=N]      # Xvfb + Mesa llvmpipe, 1280x960
GFXTEST_SIZE=2560x1080 GFXTEST_FRAMES=100 tools/gfxtest/run.sh out.png Wetness=1
```
`tools/gfxtest/gfxtest.c` drives the Glide API like the game (street scene, rain-wet road, HUD). It must compile after
renderer changes: new `GfxSettings` fields need its positional initializer updated, new extern game symbols need stubs
(e.g. the projection globals `dword_4D5294`...).

### Benchmark on a real GPU without a display
SDL's offscreen video driver uses the first EGL device (often the iGPU). Use the shim:
```sh
gcc -m32 -shared -fPIC -O2 tools/gametest/egldev.c -o /tmp/egldev.so -ldl -lEGL
SDL_EGL_LIBRARY=/tmp/egldev.so EGL_ONLY_NODE=/dev/dri/renderD128 SDL_VIDEODRIVER=offscreen \
  GFXTEST_SIZE=5120x2160 GFXTEST_FRAMES=500 <gfxtest binary> out.png       # binary path printed by run.sh's build
```
Find the node with `ls -l /dev/dri/by-path` / `lspci` (owner: renderD128 = RX 9070 XT, renderD129 = R9700,
renderD130 = iGPU).

## Real game, headless
```sh
TRACK=1 CAMERA=chase tools/gametest/race.sh                     # Outback, chase view -> $NFS2SE_TEST_DIR/out/race.png
TRACK=0 CONF="Wetness=1 Reflections=1.5" tools/gametest/race.sh  # Proving Grounds (rain), extra config
MODELS=~/.nfs2se/models KEEP=1 tools/gametest/race.sh NFS2SE_SOMETHING=1   # keep running, env var for the game
```
See the header of `tools/gametest/race.sh` for all options (`SKIES=dir` installs skies). Tracks by number of "Right" presses in the location menu:
0 Proving Grounds (night, rain), 1 Outback (sunset), 2 beach, 3 tunnel, 4 city (rain + low sun), 5 cliffs, 6 market.
Player car is the McLaren F1 (`mcf1`); start position is last (8/8).

Driving with `KEEP=1` (game left running on `DISPLAY=:77`):
```sh
xkey=$NFS2SE_TEST_DIR/xkey
$xkey 5000 Up                                 # hold accelerate 5 s (first arg = hold time in ms)
($xkey 3000 Up &); sleep 0.2; $xkey 1500 Left # accelerate and steer at the same time
$xkey c                                       # camera; alt+m debug views; alt+p effects on/off
import -window root shot.png
```
Software rendering runs at ~10-20 fps, so "frame N" based triggers take long; prefer env-var gated logging in C.

## Debugging the game code
- **gdb**: `BIN="gdb -batch -x cmds.gdb --args ./nfs2se" tools/gametest/race.sh` (attaching with `gdb -p` is blocked
  by ptrace_scope). Under gdb the menus are slower; if the automation misses, drive the menu with xkey by hand.
  Useful command file: `handle SIGUSR1 SIGUSR2 SIGPIPE nostop noprint`, `break *ADDR` + `commands ... end`,
  `rwatch *(int*)ADDR` (hardware read watchpoint), `printf "PC %x\n", $pc`.
- **Runtime address <-> asm label**: the asm object is linked first; its `.text` starts at
  `nm nfs2se_unstripped | grep sub_41B250` minus that label's offset in the object (`nm NFS2SE.Elf32.o`). With the
  current build: base `0x0804BED0`. Generate a listing and look up `addr - base`:
  ```sh
  yasm -f elf32 src/Asm/NFS2SE.asm -o /dev/null -l /tmp/list.txt     # columns: line, offset, bytes, source
  ```
  (labels in the listing give the enclosing `sub_`/`loc_`; most local labels are not in the symbol table).
  The release build is stripped; build an unstripped copy with the link line from `compile_nfs` without `-s`.
- **Crash location in C vs asm**: offsets beyond the asm `.text` size are C code or garbage (a "return address" in
  `.bss` means a smashed stack).

## Data experiments
`tools/formats/nfs2geo.py` extracts/replaces entries in `cardata.viv`; always back up the original archive first
(the game directory is not in git). The engine crashes on cars with more than ~600 polygons.

## Screenshots for the owner
Full-size shots are kept in `screenshots/` (committed); README images are resized JPEGs in `screenshots/readme/`.
The owner also likes copies on the NAS: `smbclient -N //192.168.4.162/barracuda -c 'cd "NFSIISE screenshots"; prompt OFF; mput *.png'`.
