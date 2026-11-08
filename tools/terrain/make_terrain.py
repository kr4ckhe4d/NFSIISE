# Blender (headless): builds real-world distant terrain around a track from public elevation data.
#
#   blender -b --python make_terrain.py -- <trNN.col> <lat> <lon> <out_dir> [--radius 22000] [--grid 512]
#                                          [--rotation 0] [--texture 2048]
#
# <lat> <lon>: the real place at the centre of the track (north = world +Z, east = world +X; --rotation turns the
# terrain clockwise in degrees). Elevation: AWS Terrain Tiles (Terrarium PNG, open data, Mapzen/Tilezen sources).
# Writes out_dir/model.cmdl (the renderer's model format, world coordinates in metres) and out_dir/terrain.png.
# Near the track the terrain stays below the track's ground (the game's scenery covers it); the real relief takes
# over from ~400 m. Colours: red sand, scrub and rock by slope and height (no imagery needed).
import bpy, sys, os, math, struct, urllib.request, io
import numpy as np

argv = sys.argv[sys.argv.index('--') + 1:]
col_path, lat0, lon0, out = argv[0], float(argv[1]), float(argv[2]), argv[3]
opts = dict(radius=22000.0, grid=512, rotation=0.0, texture=2048)
i = 4
while i < len(argv):
    opts[argv[i].lstrip('-')] = float(argv[i + 1])
    i += 2
os.makedirs(out, exist_ok=True)
R, N, TEX = opts['radius'], int(opts['grid']), int(opts['texture'])

# Track centreline (virtual road) from the .col file: block 4, 36-byte records, int32 16.16 x, y, z first
col = open(col_path, 'rb').read()
nblocks = struct.unpack_from('<I', col, 12)[0]
offs = struct.unpack_from('<%dI' % nblocks, col, 16)
vr = offs[-1] + 16 + 8
road = []
p = vr
while p + 12 <= len(col):
    x, y, z = struct.unpack_from('<iii', col, p)
    if road and math.dist((x, y, z), road[-1]) / 65536 > 50:
        break
    road.append((x, y, z))
    p += 36
road = np.array(road, dtype=np.float64) / 65536.0
cx, cz = (road[:, 0].min() + road[:, 0].max()) / 2, (road[:, 2].min() + road[:, 2].max()) / 2
print('road points', len(road), 'extent', np.ptp(road[:, 0]), np.ptp(road[:, 2]), 'centre', cx, cz, 'y', road[:, 1].min(), road[:, 1].max())

# Elevation tiles (Terrarium: height = R*256 + G + B/256 - 32768 metres)
Z = 12
def tile_xy(lat, lon):
    n = 2 ** Z
    x = (np.asarray(lon) + 180.0) / 360.0 * n
    y = (1.0 - np.arcsinh(np.tan(np.radians(lat))) / np.pi) / 2.0 * n
    return x, y
m_per_deg_lat = 111320.0
m_per_deg_lon = 111320.0 * math.cos(math.radians(lat0))
lat_r, lon_r = R / m_per_deg_lat, R / m_per_deg_lon
x0, y0 = tile_xy(lat0 + lat_r, lon0 - lon_r)
x1, y1 = tile_xy(lat0 - lat_r, lon0 + lon_r)
tiles = {}
for ty in range(int(np.floor(y0)), int(np.floor(y1)) + 1):
    for tx in range(int(np.floor(x0)), int(np.floor(x1)) + 1):
        url = 'https://s3.amazonaws.com/elevation-tiles-prod/terrarium/%d/%d/%d.png' % (Z, tx, ty)
        path = os.path.join(out, 'tile_%d_%d.png' % (tx, ty))
        if not os.path.exists(path):
            urllib.request.urlretrieve(url, path)
        img = bpy.data.images.load(path)
        px = np.empty(256 * 256 * 4, dtype=np.float32)
        img.colorspace_settings.name = 'Non-Color'
        img.pixels.foreach_get(px)
        px = (px.reshape(256, 256, 4)[::-1] * 255.0).round()
        tiles[(tx, ty)] = px[:, :, 0] * 256 + px[:, :, 1] + px[:, :, 2] / 256 - 32768
        bpy.data.images.remove(img)
print('tiles', len(tiles))

def elevation(lat, lon):
    x, y = tile_xy(lat, lon)
    fx, fy = (x - np.floor(x)) * 256 - 0.5, (y - np.floor(y)) * 256 - 0.5
    out = np.zeros_like(x)
    for (tx, ty), t in tiles.items():
        sel = (np.floor(x) == tx) & (np.floor(y) == ty)
        if not sel.any():
            continue
        ix = np.clip(fx[sel], 0, 254.999); iy = np.clip(fy[sel], 0, 254.999)
        i0, j0 = ix.astype(int), iy.astype(int)
        ax, ay = ix - i0, iy - j0
        out[sel] = (t[j0, i0] * (1 - ax) * (1 - ay) + t[j0, i0 + 1] * ax * (1 - ay) +
                    t[j0 + 1, i0] * (1 - ax) * ay + t[j0 + 1, i0 + 1] * ax * ay)
    return out

# Grid in local metres (east, north), rotated by --rotation
u = np.linspace(-R, R, N)
gx, gz = np.meshgrid(u, u)            # gz rows: south -> north
rot = math.radians(opts['rotation'])
ex = gx * math.cos(rot) + gz * math.sin(rot)    # geographic east of a world position
nz = -gx * math.sin(rot) + gz * math.cos(rot)   # geographic north
dem = elevation(lat0 + nz / m_per_deg_lat, lon0 + ex / m_per_deg_lon)
base = float(np.median(dem[np.hypot(gx, gz) < 3000]))
rel = dem - base
print('base elevation', base, 'relief', rel.min(), rel.max())

# Distance to the track and the track's height nearby (normalized convolution of the road heights)
cell = 2 * R / (N - 1)
hit = np.zeros((N, N)); ysum = np.zeros((N, N))
ri = np.clip(np.round((road[:, 2] - cz + R) / cell).astype(int), 0, N - 1)
rj = np.clip(np.round((road[:, 0] - cx + R) / cell).astype(int), 0, N - 1)
np.add.at(hit, (ri, rj), 1.0); np.add.at(ysum, (ri, rj), road[:, 1])
dist = np.where(hit > 0, 0.0, 1e9)
for _ in range(2):  # chamfer distance transform
    for r in range(N):
        if r: dist[r] = np.minimum(dist[r], dist[r - 1] + cell)
        for _k in range(1):
            row = dist[r]
            for c in range(1, N): row[c] = min(row[c], row[c - 1] + cell)
            for c in range(N - 2, -1, -1): row[c] = min(row[c], row[c + 1] + cell)
    for r in range(N - 2, -1, -1):
        dist[r] = np.minimum(dist[r], dist[r + 1] + cell)
def blur(a, k):
    ker = np.exp(-np.arange(-3 * k, 3 * k + 1) ** 2 / (2.0 * k * k)); ker /= ker.sum()
    a = np.apply_along_axis(lambda v: np.convolve(v, ker, 'same'), 0, a)
    return np.apply_along_axis(lambda v: np.convolve(v, ker, 'same'), 1, a)
k = max(2, int(600 / cell))
ty_s = blur(ysum, k) / np.maximum(blur(hit, k), 1e-9)
far = road[:, 1].mean()
w_near = np.clip(blur(hit, k) * 50, 0, 1)
track_y = ty_s * w_near + far * (1 - w_near)
s = np.clip((dist - 400.0) / 2100.0, 0, 1); s = s * s * (3 - 2 * s)
height = track_y - 6.0 - 20.0 * (dist < 250) * (1 - dist / 250.0) + s * rel
print('distance to track: min', dist.min(), 'max', dist.max())

# Colour: red sand, scrub speckle, rock on slopes / high relief
gy_, gx_ = np.gradient(height, cell)
slope = np.hypot(gx_, gy_)
rng = np.random.default_rng(2)
def noise(scale):
    n = rng.standard_normal((N // scale + 2, N // scale + 2))
    return np.kron(n, np.ones((scale, scale)))[:N, :N]
nz1, nz2 = blur(noise(4), 2), blur(noise(16), 6)
sand = np.array([0.62, 0.30, 0.15]); scrub = np.array([0.42, 0.36, 0.22]); rock = np.array([0.55, 0.22, 0.10])
dry = np.array([0.70, 0.45, 0.30])
col_ = sand * (1 + 0.15 * nz1[..., None])
col_ = col_ * (1 - np.clip(0.45 + 0.6 * nz2, 0, 0.7)[..., None] * 0.6) + scrub * np.clip(0.45 + 0.6 * nz2, 0, 0.7)[..., None] * 0.6
col_ = col_ * 0.85 + dry * 0.15 * np.clip(nz1, 0, 1)[..., None]
r_w = np.clip((slope - 0.25) / 0.35, 0, 1)[..., None]
col_ = col_ * (1 - r_w) + rock * (0.8 + 0.2 * nz1[..., None]) * r_w
col_ = np.clip(col_, 0, 1)
# Texture (upsampled grid colours)
img = bpy.data.images.new('terrain', TEX, TEX, alpha=False, float_buffer=False)
img.colorspace_settings.name = 'Non-Color'
idx = (np.arange(TEX) * (N - 1) / (TEX - 1)).round().astype(int)
tex = col_[idx][:, idx]
rgba = np.ones((TEX, TEX, 4), dtype=np.float32); rgba[:, :, :3] = tex ** (1 / 2.2)
img.pixels.foreach_set(rgba.ravel())
img.filepath_raw = os.path.join(out, 'terrain.png'); img.file_format = 'PNG'; img.save()

# Mesh (non-indexed triangles, cmdl v2: one body batch); X written mirrored (the loader mirrors it back)
wx = gx + cx; wz = gz + cz
nrm = np.dstack([-gx_, np.ones_like(height), -gy_]); nrm /= np.linalg.norm(nrm, axis=2, keepdims=True)
uvx = (gx + R) / (2 * R); uvz = (gz + R) / (2 * R)
def vert(r, c):
    return (-wx[r, c], height[r, c], wz[r, c], -nrm[r, c, 0], nrm[r, c, 1], nrm[r, c, 2], uvx[r, c], 1.0 - uvz[r, c])
tris = []
for r in range(N - 1):
    for c in range(N - 1):
        if max(dist[r, c], dist[r + 1, c + 1]) < 60:  # under the road itself: never visible
            continue
        a, b, cc, d = vert(r, c), vert(r, c + 1), vert(r + 1, c + 1), vert(r + 1, c)
        tris += [a, b, cc, a, cc, d]
arr = np.array(tris, dtype=np.float32)
with open(os.path.join(out, 'model.cmdl'), 'wb') as f:
    f.write(b'CMDL' + struct.pack('<II', 2, 1))
    f.write(struct.pack('<4f4f3f', 1, 1, 1, 1, 0.0, 0.95, 0.0, 0.0, 0, 0, 0))
    t = b'terrain.png'; f.write(t + b'\0' * (64 - len(t)))
    f.write(struct.pack('<II3f', 0, 0, 0, 0, 0))
    f.write(struct.pack('<I', len(arr)))
    f.write(arr.tobytes())
for name in os.listdir(out):
    if name.startswith('tile_'):
        os.remove(os.path.join(out, name))
print('terrain', len(arr) // 3, 'triangles, centre', cx, cz)
