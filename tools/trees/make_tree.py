# Blender (headless): turns a detailed tree (e.g. a Poly Haven glTF) into a game-ready tree for the renderer's tree
# replacement (src/Glide2x/Trees.c): a few tens of thousands of triangles, base at the origin, height 1 (the renderer
# scales it to the height of the game's billboard), leaves as alpha-cutout cards.
#
#   blender -b --python make_tree.py -- <in.gltf> <out.glb> [--leaf-alpha alpha.png] [--leaf-keep 0.08]
#                                       [--leaf-ratio 1] [--wood-ratio 0.03] [--seed 1] [--object name_part]
#                                       [--leaf-names leaf,leaves,twig]
#
# --leaf-keep: fraction of leaf clusters kept, each replaced by one flat card (the rest is deleted, the kept ones grow
# to keep the canopy full); small twigs are deleted,
# --leaf-ratio / --wood-ratio: decimation of the leaves / trunk and branches. Materials whose name contains "leaf" or
# "leaves" are leaves; --leaf-alpha (a separate alpha mask, as Poly Haven ships it) is merged into their colour texture.
# Then convert with tools/car-models/export_carmodel.py (alpha-cutout leaves become cutout batches).
import bpy, bmesh, sys, os, random
from mathutils import Vector
import numpy as np

argv = sys.argv[sys.argv.index('--') + 1:]
src, dst = argv[0], argv[1]
opts = {'leaf-alpha': '', 'leaf-keep': 0.08, 'leaf-ratio': 1.0, 'wood-ratio': 0.03, 'seed': 1, 'object': '', 'leaf-names': 'leaf,leaves'}
i = 2
while i < len(argv):
    k = argv[i].lstrip('-')
    opts[k] = argv[i + 1] if isinstance(opts.get(k), str) else float(argv[i + 1])
    i += 2
random.seed(int(opts['seed']))

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
meshes = [o for o in bpy.data.objects if o.type == 'MESH']
if opts['object']:
    # Only one tree of an asset with several (e.g. fir_tree_01 a/b/c)
    for o in meshes:
        if opts['object'] not in o.name:
            bpy.data.objects.remove(o)
    meshes = [o for o in bpy.data.objects if o.type == 'MESH']
LEAF = [n.strip().lower() for n in opts['leaf-names'].split(',') if n.strip()]
def is_leaf(name):
    return any(k in name.lower() for k in LEAF)
bpy.ops.object.select_all(action='DESELECT')
for o in meshes:
    o.select_set(True)
bpy.context.view_layer.objects.active = meshes[0]
if len(meshes) > 1:
    bpy.ops.object.join()
tree = bpy.context.view_layer.objects.active
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
me = tree.data
leaf_mats = [i for i, m in enumerate(me.materials) if m and is_leaf(m.name)]
print('materials', [m.name for m in me.materials], 'leaves', leaf_mats)

# Leaves: keep a random subset of the leaf clusters (connected islands) and replace each kept cluster by one flat card
# showing the same part of the leaf texture (least squares map from its UVs to 3D), grown to keep the canopy's coverage.
# Twigs (small wood islands) are deleted: the leaves hide them.
def islands(bm, pred):
    seen = set()
    for f in bm.faces:
        if f.index in seen or not pred(f):
            continue
        isl, stack = [], [f]
        seen.add(f.index)
        while stack:
            g = stack.pop()
            isl.append(g)
            for e in g.edges:
                for h in e.link_faces:
                    if h.index not in seen and pred(h):
                        seen.add(h.index)
                        stack.append(h)
        yield isl
bm = bmesh.new()
bm.from_mesh(me)
bm.faces.ensure_lookup_table()
uvl = bm.loops.layers.uv.active
co_all = np.array([v.co[:] for v in bm.verts])
height = co_all[:, 2].max() - co_all[:, 2].min()
remove, cards = [], []
keep, grow = opts['leaf-keep'], 1.0 / max(opts['leaf-keep'], 1e-3) ** 0.35
twigs = 0
for isl in islands(bm, lambda f: f.material_index in leaf_mats):
    remove.extend(isl)
    if random.random() > keep:
        continue
    P, UV = [], []
    for f in isl:
        for l in f.loops:
            P.append(l.vert.co[:]); UV.append(l[uvl].uv[:])
    P, UV = np.array(P), np.array(UV)
    A = np.c_[UV, np.ones(len(UV))]
    M, *_ = np.linalg.lstsq(A, P, rcond=None)
    u0, v0 = UV.min(0); u1, v1 = UV.max(0)
    corners = [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]
    pts = [np.array([u, v, 1.0]) @ M for u, v in corners]
    c = sum(pts) / 4
    pts = [c + (p - c) * grow for p in pts]
    cards.append((pts, corners, isl[0].material_index))
wood = [i for i in range(len(me.materials)) if i not in leaf_mats]
for isl in islands(bm, lambda f: f.material_index in wood):
    vs = np.array([v.co[:] for f in isl for v in f.verts])
    if np.linalg.norm(vs.max(0) - vs.min(0)) < 0.04 * height:
        remove.extend(isl)
        twigs += 1
bmesh.ops.delete(bm, geom=list(set(remove)), context='FACES')
for pts, corners, mi in cards:
    vs = [bm.verts.new(p.tolist()) for p in pts]
    f = bm.faces.new(vs)
    f.material_index = mi
    for l, (u, v) in zip(f.loops, corners):
        l[uvl].uv = (u, v)
bm.normal_update()
bm.to_mesh(me)
bm.free()
print('leaf cards', len(cards), 'grown', round(grow, 2), 'twigs removed', twigs)

# Decimate each material separately (leaves: --leaf-ratio, trunk and branches: --wood-ratio), then join again
bpy.ops.object.select_all(action='DESELECT')
tree.select_set(True)
bpy.context.view_layer.objects.active = tree
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.separate(type='MATERIAL')
bpy.ops.object.mode_set(mode='OBJECT')
parts = [o for o in bpy.data.objects if o.type == 'MESH']
for o in parts:
    leaf = any(s.material and is_leaf(s.material.name) for s in o.material_slots)
    ratio = opts['leaf-ratio'] if leaf else opts['wood-ratio']
    before = len(o.data.polygons)
    if ratio >= 1.0:
        continue
    mod = o.modifiers.new('dec', 'DECIMATE')
    mod.ratio = ratio
    mod.use_collapse_triangulate = True
    bpy.context.view_layer.objects.active = o
    bpy.ops.object.modifier_apply(modifier='dec')
    print('part', o.name, [s.material.name for s in o.material_slots if s.material], before, '->', len(o.data.polygons))
bpy.ops.object.select_all(action='DESELECT')
for o in parts:
    o.select_set(True)
bpy.context.view_layer.objects.active = parts[0]
bpy.ops.object.join()
tree = bpy.context.view_layer.objects.active
me = tree.data
leaf_mats = [i for i, m in enumerate(me.materials) if m and is_leaf(m.name)]

# Base at the origin, height 1 (Blender Z up)
co = np.array([v.co[:] for v in me.vertices])
lo, hi = co.min(0), co.max(0)
cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
# Trunk centre: the lowest 5% of the wood
low = co[co[:, 2] < lo[2] + 0.05 * (hi[2] - lo[2])]
if len(low):
    cx, cy = low[:, 0].mean(), low[:, 1].mean()
h = hi[2] - lo[2]
for v in me.vertices:
    v.co = ((v.co[0] - cx) / h, (v.co[1] - cy) / h, (v.co[2] - lo[2]) / h)

# Leaves: colour texture + separate alpha mask -> RGBA, alpha clip
if opts['leaf-alpha']:
    alpha = bpy.data.images.load(opts['leaf-alpha'])
    alpha.colorspace_settings.name = 'Non-Color'
    for i in leaf_mats:
        m = me.materials[i]
        bsdf = next(n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
        tex = bsdf.inputs['Base Color'].links[0].from_node
        while tex.type != 'TEX_IMAGE':
            src_link = next(l for inp in tex.inputs for l in inp.links if l.from_node.type == 'TEX_IMAGE' or l.from_node.inputs)
            tex = src_link.from_node
        # Use the colour texture directly (drop AO mixes): the exporter reads the first texture behind Base Color
        m.node_tree.links.new(tex.outputs['Color'], bsdf.inputs['Base Color'])
        img = tex.image
        w, hh = img.size
        if alpha.size[0] != w or alpha.size[1] != hh:
            alpha.scale(w, hh)
        px = np.empty(w * hh * 4, dtype=np.float32); img.pixels.foreach_get(px)
        ap = np.empty(w * hh * 4, dtype=np.float32); alpha.pixels.foreach_get(ap)
        px = px.reshape(-1, 4); px[:, 3] = ap.reshape(-1, 4)[:, 0]
        rgba = bpy.data.images.new('leaves_rgba', w, hh, alpha=True)
        rgba.pixels.foreach_set(px.ravel())
        tex.image = rgba
        m.node_tree.links.new(tex.outputs['Alpha'], bsdf.inputs['Alpha'])
        try:
            m.blend_method = 'CLIP'
        except Exception:
            pass
tris = sum(len(p.vertices) - 2 for p in me.polygons)
print('tree triangles', tris, 'height', round(h, 2))
bpy.ops.export_scene.gltf(filepath=dst, export_format='GLB', export_yup=True)
