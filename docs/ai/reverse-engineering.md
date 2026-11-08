# Game code reverse engineering notes

The game is `src/Asm/NFS2SE.asm`: a disassembly (IDA style labels `sub_<addr>`, `loc_<addr>`, `dword_<addr>` named
after the original Windows executable's addresses) assembled with yasm and linked with the C wrapper. Labels are
**not** runtime addresses. Coding convention is Watcom register calls: arguments in `eax, edx, ebx, ecx`, then stack;
callees often `ret N`. Calls from asm into C use `stdcall` with `REALIGN` (stack realignment) on the C side.

## Patches in the asm (search for these comments)

| Where | What |
|---|---|
| `sub_433910` (`; widescreen: squeeze X projection when width == 640.0`), and 3 more `fmul [widescreenScale]` | Hor+ widescreen: full-width 3D views get X scale * `widescreenScale` (= 640 / virtual width, set by the renderer) |
| `sub_47FCD0` start (`; Car models:`) | per car part: `pushad; push esi; push [esp+28h]; push edx; push eax; call carPartHook; popad; cmp [carPartSkip],0; jz draw; ret 0Ch` |
| `loc_46E407` in the GEO loader `sub_46E270` | `carModelLoaded(&filename_on_stack, geo)` |
| end of `sub_42A3C0` (render list) | `carModelsFlush()` |
| `global dword_4D5294, flt_4D5298, flt_4D529C, flt_4D52A0` | projection constants exported to C |

Pattern for adding a hook: `pushad` / push args / `call cHook` / `popad`; with `pushad` the original `[esp+4]` is at
`[esp+24h]` (+4 per extra push). Declare `extern cHook` near the other externs at the top of the file and provide a
stub in `src/Glide2x/OpenGL1.c` (the GL1 renderer is also linked in some builds).

## Rendering facts

- The game transforms and projects everything on the CPU and sends pre-projected Glide vertices: `x, y` in 640x480
  units + `VertexSnap` (0xC0000 = 786432, subtract it), `oow` = 1/w with w = Z in metres (`oow = 65536 / Z_16.16`).
- Projection (`sub_480330`, per-vertex transform): `X,Y,Z = v * M + t` (16.16, row vector), then
  `x = dword_4D5294 * X/Z + flt_4D529C`, `y = flt_4D5298 * Y/Z + flt_4D52A0` (camera Y up, so `flt_4D5298` < 0).
  Typical values at 21:9: 180, -330.3, 320, 240. Scale is set in `sub_433910`.
- Matrix multiply `sub_49EAC8(eax = vector, edx = matrix, ebx = out)`: 3x3 16.16, `out.x = v.x*M0 + v.y*M3 + v.z*M6`.
- HUD/2D: flat at `oow == 1.0` (fog stays enabled for everything). Sky backdrop: flat strip at constant depth spanning
  +-1.5 focal lengths, drawn before 3D without depth test (`sub_42E660`); the renderer widens it (`MapBackdrop`).
- The game swaps fog tables mid-frame.
- Rear-view mirror: its own camera centred on its clip rectangle (game units ~x 486-636, y 40-110) with the main view's
  **vertical** field of view (measured from road geometry: ground slope ratio 56/390 = height ratio 70/480).
- Rain: drawn as flat (all three `oow` equal, < 0.999) additive (`dfactor == GL_ONE`) world triangles, 150-280 per
  frame on rainy tracks (Proving Grounds, city), none on dry ones. The renderer counts them (`g_frameRainTriangles`,
  threshold 30) for automatic wetness. Candidate weather globals `dword_512208/5122B0/51220C` turned out not to be it.

## Structures

Car render object (`esi` in `sub_475380`, the per-object part loop):
```
+0x50  i32 matrix[9]       orientation, 16.16, rows = local axes in world
+0x74  i32 position[3]     world, 16.16 metres
+0x8C  i32 part count
+0x90  part pointers[]     (GEO parts of the car)
```
Camera (`edi` in `sub_475380`): `+0x08` position[3], `+0x44` matrix[9]. Per part the game computes
`relative = object.pos - camera.pos` -> camera space (`sub_480720`), `M = object.matrix * camera.matrix`
(`sub_49E840`) and calls `sub_47FCD0(eax = part, edx = &M, ebx, ecx, stack: &cameraSpaceTranslation, lod, ...)`.

Car object passed to the GEO loader (`ebx` -> `[esp+108h]`): `+0x21C` -> car info (`[+0] == 4`, `< 0x0F` checks),
`+0x2E4`, `+0x610..+0x61C` filled by `sub_476F60`.

Draw call chain for a car polygon: `sub_46F980` (view) -> `sub_429010` (scene) -> `sub_42A3C0` (render list, dispatch
table `dword_4EC500[type]`) -> `sub_42B320` / `sub_42AF00` (one projected polygon) -> `sub_432580` / `sub_432670`
(emitters) -> `sub_4975E8` / `sub_497B24` / `sub_4983B0` -> `grDrawTriangle`.

Other: `sub_4800B0` per-vertex shading (rotates normals by a matrix); `sub_46D3D0` load-time vertex pass;
texture upload `sub_49755C`; texture memory allocator `loc_4974DD` (`dword_56EF38` running address); allocator
`sub_484498` (named pools, e.g. "carGeo").

## How things were found (reuse these techniques)
- **Who draws a triangle**: temporarily log, in `grDrawTriangle`, the stack words that fall into the asm `.text`
  range (`(const uint32_t *)&a - 1` upwards) and map offsets to labels with the yasm listing (see testing.md).
  Group triangles by call chain and by screen position/`oow` to identify objects.
- **Who reads data every frame**: gdb hardware read watchpoint (`rwatch`) on the data (e.g. a GEO vertex), set from a
  breakpoint at the loader; log `$pc` for every hit and count.
- **Engine limits**: modify data files (tools/formats/nfs2geo.py) and bisect in real races.
- **Projection/geometry**: dump Glide vertices for one frame and fit (e.g. ground y vs oow lines).
