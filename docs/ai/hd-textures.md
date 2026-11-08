# HD texture packs

Status: working (commit `b2870d4`). An upscaled pack was generated for the owner with Real-ESRGAN.

## Pipeline

```
game texture download (grTexDownloadMipMap) -> RGBA8 conversion -> 64-bit content hash
   TextureDump=1    -> <settings>/texdump/<HASH>.png        (only if not dumped yet)
   TextureReplace=1 -> load <settings>/textures/<HASH>.{png,jpg,tga} or ./textures/<HASH>.* next to the binary
```

1. Run the game with `TextureDump=1` and visit every track/car to cover (textures are only seen when used).
2. `tools/hd-textures/upscale.sh <realesrgan-ncnn-vulkan dir> [model]` upscales `texdump/*.png` (min side >= 32 px,
   `MIN_SIZE` env) into `textures/`, skipping already done files. Default model `realesrgan-x4plus`
   (`realesrgan-x4plus-anime` for flat artwork). Binary: https://github.com/xinntao/Real-ESRGAN/releases
   (portable `realesrgan-ncnn-vulkan`, runs on any Vulkan GPU).
3. `TextureReplace=1` (default) loads them; any size, alpha kept, downscaled if larger than the GPU limit.

## Hash (src/Glide2x/Textures.c, `texHash`)
Over the converted RGBA8 pixels as 32-bit words, two lanes:
```
h1 = 0x811C9DC5 ^ w;  h2 = 0x9E3779B9 ^ (h << 16)
for each word v: h1 = (h1 ^ v) * 0x01000193;  h2 = rotl((h2 ^ v) * 0x85EBCA6B, 13)
h1 ^= h1>>16; h1 *= 0x7FEB352D; h1 ^= h1>>15;   h2 ^= h2>>16; h2 *= 0x846CA68B; h2 ^= h2>>16
name = "%08X%08X" % (h1, h2)
```
Palette textures are hashed after palette conversion, so the same indices with another palette are another texture.
Changing the conversion (e.g. 565 expansion) changes all hashes and invalidates existing packs - avoid that.

## Notes
- Real-ESRGAN x4plus works well on track textures; road textures with fine noise come out smeared - a better
  road/asphalt model or hand-made road textures would help (open idea).
- Car textures in `gamedata/carmodel/pc/*.qfs` (compressed FSH) are also dumped/replaced by hash at runtime, but
  replacement car models ([car-models.md](car-models.md)) bypass them entirely.
- The cache keys GL textures by hash, so replaced textures are loaded from disk once per run.
- Running downloaded binaries (the upscaler) needed explicit permission from the owner in Claude Code; a helper that
  adds an allow rule was used (`.claude/settings.local.json`, excluded via `.git/info/exclude`).
