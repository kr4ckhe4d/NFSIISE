# Blender (headless): builds a coconut palm procedurally (no external assets) for the tree replacement:
# curved ringed trunk + crown of arching fronds with drooping leaflets. Base at the origin, height 1.
#
#   blender -b --python make_palm.py -- <out.glb> [--seed 1] [--fronds 14] [--lean 0.12]
import bpy, bmesh, sys, math, random
import numpy as np
from mathutils import Vector

argv = sys.argv[sys.argv.index('--') + 1:]
dst = argv[0]
opts = {'seed': 1, 'fronds': 14, 'lean': 0.12}
i = 1
while i < len(argv):
    opts[argv[i].lstrip('-')] = float(argv[i + 1])
    i += 2
random.seed(int(opts['seed']))
bpy.ops.wm.read_factory_settings(use_empty=True)

def material(name, color, rough, image=None):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes['Principled BSDF']
    bsdf.inputs['Base Color'].default_value = color
    bsdf.inputs['Roughness'].default_value = rough
    if image:
        t = m.node_tree.nodes.new('ShaderNodeTexImage')
        t.image = image
        m.node_tree.links.new(t.outputs['Color'], bsdf.inputs['Base Color'])
    return m

# Bark texture: grey-brown with darker horizontal rings (leaf scars) and noise
W, H = 64, 256
rng = np.random.default_rng(int(opts['seed']))
v = np.linspace(0, 1, H)[:, None]
rings = 0.75 + 0.25 * np.abs(np.cos(v * math.pi * 2 * 24)) ** 8
noise = rng.normal(0, 0.06, (H, W))
base = np.array([0.42, 0.36, 0.28])
tex = np.clip(base[None, None, :] * (rings[..., None] * 0.6 + 0.4 + noise[..., None]), 0, 1)
img = bpy.data.images.new('palm_bark', W, H, alpha=False)
rgba = np.ones((H, W, 4), dtype=np.float32); rgba[..., :3] = tex
img.pixels.foreach_set(rgba.ravel())
img.pack()
bark = material('palm_bark', (1, 1, 1, 1), 0.85, img)
leaf = material('palm_leaf', (0.05, 0.12, 0.025, 1), 0.55)
rachis = material('palm_rachis', (0.2, 0.18, 0.08, 1), 0.7)

me = bpy.data.meshes.new('palm')
ob = bpy.data.objects.new('palm', me)
bpy.context.collection.objects.link(ob)
for m in (bark, leaf, rachis):
    me.materials.append(m)
bm = bmesh.new()
uv = bm.loops.layers.uv.new()

# Trunk: Blender Z up; slight lean and S-curve, tapering, 10 sides
lean, seg, sides, height = opts['lean'], 16, 10, 1.0
ang = random.uniform(0, 2 * math.pi)
def trunk_point(t):
    off = lean * (t ** 1.6) + 0.02 * math.sin(t * math.pi * 2)
    return Vector((math.cos(ang) * off, math.sin(ang) * off, t * height * 0.86))
rings_v = []
for s in range(seg + 1):
    t = s / seg
    c = trunk_point(t)
    r = 0.022 * (1.25 - 0.45 * t) + (0.012 if s == 0 else 0)
    ring = [bm.verts.new(c + Vector((math.cos(a) * r, math.sin(a) * r, 0))) for a in (2 * math.pi * k / sides for k in range(sides))]
    rings_v.append(ring)
for s in range(seg):
    for k in range(sides):
        a, b = rings_v[s][k], rings_v[s][(k + 1) % sides]
        c, d = rings_v[s + 1][(k + 1) % sides], rings_v[s + 1][k]
        f = bm.faces.new((a, b, c, d))
        f.material_index = 0
        for l, (u, vv) in zip(f.loops, ((k / sides, s / seg), ((k + 1) / sides, s / seg), ((k + 1) / sides, (s + 1) / seg), (k / sides, (s + 1) / seg))):
            l[uv].uv = (u, vv * 6)
top = trunk_point(1.0)

# Fronds: rachis arching out and down, leaflet pairs drooping along it
def quad(p, mat):
    f = bm.faces.new([bm.verts.new(x) for x in p])
    f.material_index = mat
    return f
n = int(opts['fronds'])
for fi in range(n):
    yaw = 2 * math.pi * fi / n + random.uniform(-0.15, 0.15)
    rise = random.uniform(0.15, 0.75)             # initial upward angle
    length = random.uniform(0.38, 0.5)
    d = Vector((math.cos(yaw), math.sin(yaw), 0))
    side = Vector((-d.y, d.x, 0))
    pts = []
    steps = 16
    for s in range(steps + 1):
        t = s / steps
        h = d * (length * t)
        z = length * (rise * t - 0.9 * t * t)    # parabola: up, then droops
        pts.append(top + h + Vector((0, 0, z)))
    for s in range(steps):
        a, b = pts[s], pts[s + 1]
        w = 0.006 * (1 - s / steps) + 0.002
        quad((a - side * w, a + side * w, b + side * w, b - side * w), 2)
        # Leaflets on both sides (none near the trunk), longest in the middle, drooping
        t = (s + 0.5) / steps
        if t < 0.12:
            continue
        ll = 0.16 * math.sin(math.pi * min(1.0, (t - 0.05) / 0.95)) + 0.02
        for sgn in (-1, 1):
            out = (side * sgn * 0.8 + d * 0.35 + Vector((0, 0, -0.55))).normalized()
            m = (a + b) * 0.5
            tip = m + out * ll
            wdt = (b - a).length * 0.45
            quad((m - (b - a).normalized() * wdt, m + (b - a).normalized() * wdt, tip + (b - a).normalized() * wdt * 0.2, tip - (b - a).normalized() * wdt * 0.2), 1)
bm.normal_update()
bm.to_mesh(me)
bm.free()
# Base at the origin, height 1
co = np.array([v.co[:] for v in me.vertices])
lo, hi = co.min(0), co.max(0)
h = hi[2] - lo[2]
for v in me.vertices:
    v.co = (v.co[0] / h, v.co[1] / h, (v.co[2] - lo[2]) / h)
print('palm triangles', sum(len(p.vertices) - 2 for p in me.polygons))
bpy.ops.export_scene.gltf(filepath=dst, export_format='GLB', export_yup=True)
