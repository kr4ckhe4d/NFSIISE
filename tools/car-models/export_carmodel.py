# Blender: imports a glTF/GLB car and writes a replacement car model for NFSIISE (model.cmdl + PNG textures).
# Usage: blender -b --python export_carmodel.py -- in.glb out_dir
# The model should face +Z (glTF forward) with Y up, in metres; the game fits its size to the original car.
import bpy, sys, os, struct, bmesh
argv = sys.argv[sys.argv.index('--') + 1:]
src, out = argv[0], argv[1]
os.makedirs(out, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
def mat_params(m):
    p = dict(color=(0.8, 0.8, 0.8, 1.0), metal=0.0, rough=0.5, transmission=0.0, emission=(0, 0, 0), tex='', clearcoat=0.0)
    if not m or not m.use_nodes:
        return p
    bsdf = next((n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if not bsdf:
        return p
    def val(name, default):
        s = bsdf.inputs.get(name)
        return s.default_value if s is not None else default
    c = val('Base Color', (0.8, 0.8, 0.8, 1.0)); p['color'] = tuple(c)
    p['metal'] = float(val('Metallic', 0.0)); p['rough'] = float(val('Roughness', 0.5))
    p['transmission'] = float(val('Transmission Weight', val('Transmission', 0.0)))
    p['clearcoat'] = float(val('Coat Weight', val('Clearcoat', 0.0)))
    a = float(val('Alpha', 1.0)); p['color'] = (c[0], c[1], c[2], a)
    e = val('Emission Color', (0, 0, 0, 1)); es = float(val('Emission Strength', 1.0))
    p['emission'] = (e[0] * es, e[1] * es, e[2] * es)
    def find_image(socket):
        # The first image texture behind a socket (through mix/multiply nodes, e.g. glTF vertex colour factors)
        todo = [l.from_node for l in socket.links] if socket else []
        seen = set()
        while todo:
            node = todo.pop(0)
            if node.name in seen:
                continue
            seen.add(node.name)
            if node.type == 'TEX_IMAGE' and node.image:
                return node
            todo += [l.from_node for i in node.inputs for l in i.links]
        return None
    link = bsdf.inputs['Base Color'].links
    if link:
        n = find_image(bsdf.inputs['Base Color'])
        if n and n.type == 'TEX_IMAGE' and n.image:
            name = bpy.path.clean_name(n.image.name) + '.png'
            path = os.path.join(out, name)
            if not os.path.exists(path):
                img = n.image.copy(); img.filepath_raw = path; img.file_format = 'PNG'; img.save()
            p['tex'] = name
    # Alpha from the texture (leaves): alpha-cutout batch, marked by transmission = -1
    if bsdf.inputs.get('Alpha') and p['tex'] and find_image(bsdf.inputs['Alpha']):
        p['transmission'] = -1.0
        p['color'] = (p['color'][0], p['color'][1], p['color'][2], 1.0)
    return p
# Wheels: objects named (or parented under) "Wheel<Front|Rear><L|R>..." - group 1..4 = front left, front right,
# rear left, rear right. Brake pads/calipers steer but don't spin.
def wheel_group(ob):
    o = ob
    while o:
        n = o.name.lower().replace('_', '').replace(' ', '')
        for g, key in ((1, 'wheelfrontl'), (2, 'wheelfrontr'), (3, 'wheelrearl'), (4, 'wheelrearr')):
            if n.startswith(key):
                spins = not any(k in ob.name.lower() for k in ('pad', 'caliper'))
                return g, spins
        o = o.parent
    return 0, False
mats = {}; batches = {}; wheel_bounds = {}
for ob in bpy.data.objects:
    if ob.type != 'MESH':
        continue
    group, spins = wheel_group(ob)
    dg = bpy.context.evaluated_depsgraph_get()
    me = ob.evaluated_get(dg).to_mesh()
    bm = bmesh.new(); bm.from_mesh(me); bmesh.ops.triangulate(bm, faces=bm.faces); bm.to_mesh(me); bm.free()
    me.calc_loop_triangles() if hasattr(me, 'calc_loop_triangles') else None
    M = ob.matrix_world; N = M.to_3x3().inverted().transposed()
    uvl = me.uv_layers.active.data if me.uv_layers.active else None
    for tri in me.loop_triangles:
        m = ob.material_slots[tri.material_index].material if ob.material_slots else None
        mkey = m.name if m else '_default'
        if mkey not in mats:
            mats[mkey] = mat_params(m)
        key = (mkey, group, spins)
        b = batches.setdefault(key, [])
        for li in tri.loops:
            l = me.loops[li]; v = M @ me.vertices[l.vertex_index].co
            n = (N @ (l.normal if hasattr(l, 'normal') else me.vertices[l.vertex_index].normal)).normalized()
            uv = uvl[li].uv if uvl else (0, 0)
            b.append((v.x, v.y, v.z, n.x, n.y, n.z, uv[0], 1.0 - uv[1]))
            if group and spins:
                lo, hi = wheel_bounds.setdefault(group, ([1e9] * 3, [-1e9] * 3))
                for c, x in enumerate((v.x, v.y, v.z)):
                    lo[c] = min(lo[c], x); hi[c] = max(hi[c], x)
    ob.evaluated_get(dg).to_mesh_clear()
# Blender is Z-up: convert to Y-up (x, z, -y)
def pivot(group):
    if group not in wheel_bounds:
        return (0.0, 0.0, 0.0)
    lo, hi = wheel_bounds[group]
    c = [(a + b) * 0.5 for a, b in zip(lo, hi)]
    return (c[0], c[2], -c[1])
with open(os.path.join(out, 'model.cmdl'), 'wb') as f:
    keys = [k for k in batches if batches[k]]
    f.write(b'CMDL' + struct.pack('<II', 2, len(keys)))
    for k in keys:
        p = mats[k[0]]; t = p['tex'].encode()[:63]
        f.write(struct.pack('<4f f f f f 3f', *p['color'], p['metal'], p['rough'], p['transmission'], p['clearcoat'], *p['emission']))
        f.write(t + b'\0' * (64 - len(t)))
        f.write(struct.pack('<II3f', k[1], 1 if k[2] else 0, *pivot(k[1])))
        verts = batches[k]
        f.write(struct.pack('<I', len(verts)))
        for x, y, z, nx, ny, nz, u, v in verts:
            f.write(struct.pack('<8f', x, z, -y, nx, nz, -ny, u, v))
        print('batch', k, len(verts) // 3, 'tris', 'pivot', [round(x, 3) for x in pivot(k[1])])
