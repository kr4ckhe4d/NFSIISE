# Blender (headless): converts an equirectangular HDR sky into a game sky for NFSIISE.
#
#   blender -b --python make_sky.py -- in.hdr out_dir/trNN [--width 8192] [--elevation-min -8] [--brightness 0.2]
#                                       [--yaw 0] [--blend-from 0] [--blend-to 1.5] [--auto-yaw 1]
#
# --auto-yaw 1: the renderer rotates the sky so its sun matches the game's sun (skies with a visible sun);
# 0 for overcast skies (--yaw is used).
#
# Writes out_dir/trNN.jpg (the sky above elevation-min, encoded like the renderer's HDR scene: c / (1 + 0.75 c),
# gamma 2.2, so values up to 4x white survive for bloom and sun shafts) and out_dir/trNN.txt (parameters read by
# the renderer). The sky is scaled so its average brightness (sun excluded) is --brightness.
import bpy, sys, os
import numpy as np

argv = sys.argv[sys.argv.index('--') + 1:]
src, dst = argv[0], argv[1]
opts = dict(width=8192, elevation_min=-8.0, brightness=0.2, yaw=0.0, blend_from=0.0, blend_to=1.5, auto_yaw=1, headlights=0.0, night_ambient=1.0, hide='-')
i = 2
while i < len(argv):
    key = argv[i].lstrip('-').replace('-', '_')
    opts[key] = argv[i + 1] if isinstance(opts.get(key), str) else float(argv[i + 1])
    i += 2

img = bpy.data.images.load(src)
w, h = img.size
px = np.empty(w * h * 4, dtype=np.float32)
img.pixels.foreach_get(px)
px = px.reshape(h, w, 4)[::-1, :, :3]  # Blender stores rows bottom-up; make row 0 the zenith

# Keep the sky above elevation_min (row r has elevation 90 - 180 * (r + 0.5) / h)
rows = int(np.ceil((90.0 - opts['elevation_min']) / 180.0 * h))
sky = px[:rows]

# Downscale to the output width (box filter by an integer factor)
factor = max(1, int(round(w / opts['width'])))
if factor > 1:
    hh = (sky.shape[0] // factor) * factor
    ww = (sky.shape[1] // factor) * factor
    sky = sky[:hh, :ww].reshape(hh // factor, factor, ww // factor, factor, 3).mean(axis=(1, 3))

# Brightness: average luminance above 5 degrees, the sun (top 0.5%) excluded
lum = sky @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
elev = 90.0 - 180.0 * (np.arange(sky.shape[0]) + 0.5) / (sky.shape[0] * 180.0 / (90.0 - opts['elevation_min']))
upper = lum[elev > 5.0]
cut = np.percentile(upper, 99.5)
scale = opts['brightness'] / max(float(upper[upper < cut].mean()), 1e-6)

# Sun direction: brightest region (for aligning with the game's sun)
r, c = np.unravel_index(np.argmax(lum), lum.shape)
sun_az = (c + 0.5) / sky.shape[1] * 360.0 - 180.0
sun_el = elev[r]

enc = sky * scale
enc = np.clip(enc / (1.0 + 0.75 * enc), 0.0, 1.0) ** (1.0 / 2.2)

# Byte image without color management, so the encoded values are written as they are
out = bpy.data.images.new('sky', enc.shape[1], enc.shape[0], alpha=False, float_buffer=False)
out.colorspace_settings.name = 'Non-Color'
rgba = np.ones((enc.shape[0], enc.shape[1], 4), dtype=np.float32)
rgba[:, :, :3] = enc[::-1]
out.pixels.foreach_set(rgba.ravel())
os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
out.filepath_raw = dst + '.jpg'
out.file_format = 'JPEG'
scene = bpy.context.scene
scene.render.image_settings.quality = 95
out.save()
check = enc[enc.shape[0] // 3, enc.shape[1] // 2]
print('check pixel', [round(float(x) * 255) for x in check])

with open(dst + '.txt', 'w') as f:
    f.write(f'image={os.path.basename(dst)}.jpg\n')
    f.write(f'elevation_min={opts["elevation_min"]:.3f}\n')
    f.write(f'yaw={opts["yaw"]:.3f}\n')
    f.write(f'sun_azimuth={sun_az:.3f}\n')
    f.write(f'sun_elevation={sun_el:.3f}\n')
    f.write(f'horizon_blend={opts["blend_from"]:.2f} {opts["blend_to"]:.2f}\n')
    f.write(f'auto_yaw={int(opts["auto_yaw"])}\n')
    if opts['hide'] != '-':
        f.write(f'hide={opts["hide"]}\n')
    if opts['night_ambient'] < 1:
        f.write(f'night_ambient={opts["night_ambient"]:.2f}\n')
    if opts['headlights'] > 0:
        f.write(f'headlights={opts["headlights"]:.2f}\n')
print(f'sky {enc.shape[1]}x{enc.shape[0]} scale {scale:.4f} sun az {sun_az:.1f} el {sun_el:.1f}')
