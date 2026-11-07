# Game code patches

Patches for the `src/Asm` submodule (upstream: zaps166/NFSIISE-ASM) that are not in the submodule's
remote yet. Apply with:

```sh
cd src/Asm && git am ../../patches/asm-widescreen.patch
```

* `asm-widescreen.patch` - Hor+ widescreen: the game scales the X projection of full-width views by
  `widescreenScale`, which the OpenGL 2 renderer sets from the window aspect ratio (`Widescreen=1`).
  Without it the build still works, but the picture is stretched instead of widened.
