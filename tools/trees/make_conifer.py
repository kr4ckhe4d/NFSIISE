# Blender (headless): builds a conifer (fir/pine) procedurally from a bark texture and an atlas of needle sprays
# (Poly Haven fir_tree_01: trunk diffuse, twig diffuse + twig alpha): a tapering trunk and whorls of drooping branches,
# each branch a few crossed spray cards (alpha cutout). Base at the origin, height 1. A few thousand triangles.
#
#   blender -b --python make_conifer.py -- <out.glb> <bark.jpg> <twig_diffuse.jpg> <twig_alpha.png>
#        [--seed 1] [--radius 0.24] [--spray u0,v0,u1,v1]
#
# --spray: the UV box of one needle spray in the atlas (default: the large spray of fir_tree_01's twig texture).
import bpy, bmesh, sys, math, random
import numpy as np
from mathutils import Vector

argv = sys.argv[sys.argv.index('--') + 1:]
dst, bark_path, twig_path, alpha_path = argv[:4]
opts = {'seed': 1, 'radius': 0.24, 'spray': '0.315,0.228,0.64,0.6'}
i = 4
while i < len(argv):
    k = argv[i].lstrip('-')
    opts[k] = argv[i + 1] if isinstance(opts.get(k), str) else float(argv[i + 1])
    i += 2
random.seed(int(opts['seed']))
su0, sv0, su1, sv1 = (float(x) for x in opts['spray'].split(','))
bpy.ops.wm.read_factory_settings(use_empty=True)

def image(path, alpha_path=None):
    img = bpy.data.images.load(path)
    if not alpha_path:
        return img
    a = bpy.data.images.load(alpha_path)
    a.colorspace_settings.name = 'Non-Color'
    w, h = img.size
    if a.size[0] != w or a.size[1] != h:
        a.scale(w, h)
    px = np.empty(w * h * 4, dtype=np.float32); img.pixels.foreach_get(px)
    ap = np.empty(w * h * 4, dtype=np.float32); a.pixels.foreach_get(ap)
    px = px.reshape(-1, 4); px[:, 3] = ap.reshape(-1, 4)[:, 0]
    out = bpy.data.images.new('conifer_twig', w, h, alpha=True)
    out.pixels.foreach_set(px.ravel())
    out.pack()
    return out
def material(name, img, cutout=False):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes['Principled BSDF']
    bsdf.inputs['Roughness'].default_value = 0.8
    t = m.node_tree.nodes.new('ShaderNodeTexImage')
    t.image = img
    m.node_tree.links.new(t.outputs['Color'], bsdf.inputs['Base Color'])
    if cutout:
        m.node_tree.links.new(t.outputs['Alpha'], bsdf.inputs['Alpha'])
        bsdf.inputs['Base Color'].default_value = (0.5, 0.55, 0.45, 1)  # Tint (the exporter multiplies the texture)
    return m
bark = material('conifer_bark', image(bark_path))
twig = material('conifer_twig', image(twig_path, alpha_path), True)

me = bpy.data.meshes.new('conifer')
ob = bpy.data.objects.new('conifer', me)
bpy.context.collection.objects.link(ob)
me.materials.append(bark)
me.materials.append(twig)
bm = bmesh.new()
uv = bm.loops.layers.uv.new()
def face(pts, uvs, mat):
    f = bm.faces.new([bm.verts.new(p) for p in pts])
    f.material_index = mat
    for l, t in zip(f.loops, uvs):
        l[uv].uv = t
    return f

# Trunk (Z up), 8 sides
sides, seg = 8, 10
rings = []
for s in range(seg + 1):
    t = s / seg
    r = 0.016 * (1.0 - 0.9 * t) + 0.002
    rings.append([Vector((math.cos(a) * r, math.sin(a) * r, t)) for a in (2 * math.pi * k / sides for k in range(sides))])
for s in range(seg):
    for k in range(sides):
        k1 = (k + 1) % sides
        face((rings[s][k], rings[s][k1], rings[s + 1][k1], rings[s + 1][k]),
             ((k / sides, s / seg * 4), ((k + 1) / sides, s / seg * 4), ((k + 1) / sides, (s + 1) / seg * 4), (k / sides, (s + 1) / seg * 4)), 0)

# Branch whorls: cone of drooping branches, each made of spray cards (flat + tilted copy for volume)
R = opts['radius']
z = 0.12
while z < 0.985:
    t = (z - 0.12) / 0.88
    length = R * (1.0 - t) ** 1.05 + 0.025
    n = max(5, int(11 - 5 * t)) + random.randint(0, 1)
    phase = random.uniform(0, 2 * math.pi)
    for b in range(n):
        yaw = phase + 2 * math.pi * b / n + random.uniform(-0.2, 0.2)
        d = Vector((math.cos(yaw), math.sin(yaw), 0))
        side = Vector((-d.y, d.x, 0))
        droop = random.uniform(0.15, 0.45) + 0.2 * (1 - t)
        cards = 2 if length > 0.08 else 1
        for c in range(cards):
            a0, a1 = c / cards, (c + 1) / cards
            p0 = Vector((0, 0, z)) + d * (length * a0) + Vector((0, 0, -droop * length * a0 * a0))
            p1 = Vector((0, 0, z)) + d * (length * a1) + Vector((0, 0, -droop * length * a1 * a1))
            w = (p1 - p0).length * 0.55
            for tilt in (0.0, 0.8):
                up = (side * math.cos(tilt) + Vector((0, 0, 1)) * math.sin(tilt)).normalized()
                face((p0 - up * w, p0 + up * w, p1 + up * w, p1 - up * w),
                     ((su0, sv0), (su1, sv0), (su1, sv1), (su0, sv1)), 1)
    z += 0.028 + random.uniform(-0.005, 0.005)
bm.normal_update()
bm.to_mesh(me)
bm.free()
print('conifer triangles', sum(len(p.vertices) - 2 for p in me.polygons))
bpy.ops.export_scene.gltf(filepath=dst, export_format='GLB', export_yup=True)
